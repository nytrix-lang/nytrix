#include "base/util.h"
#include "braun.h"
#include "code/llvm.h"
#include "code/priv.h"
#include "priv.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static bool braun_should_init(void) {
  return ny_env_enabled("NYTRIX_BRAUN") || ny_env_enabled("NYTRIX_BRAUN_SSA");
}

static bool ny_env_enabled_default_on_local(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v)
    return true;
  return ny_env_enabled(name);
}

static bool ny_effect_analysis_requested(void) {
  const char *forbid = getenv("NYTRIX_EFFECT_FORBID");
  if (forbid && *forbid)
    return true;
  return ny_env_enabled("NYTRIX_EFFECT_DIAG") ||
         ny_env_enabled("NYTRIX_EFFECT_DIAG_VERBOSE") ||
         ny_env_enabled("NYTRIX_EFFECT_REQUIRE_PURE") ||
         ny_env_enabled("NYTRIX_EFFECT_REQUIRE_KNOWN");
}

static void ny_debug_apply_fn_attrs(codegen_t *cg, LLVMValueRef fn) {
  if (!cg || !fn || !cg->debug_symbols)
    return;
  LLVMAttributeRef fp =
      LLVMCreateStringAttribute(cg->ctx, "frame-pointer", 13, "all", 3);
  LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, fp);
  LLVMAttributeRef dtc =
      LLVMCreateStringAttribute(cg->ctx, "disable-tail-calls", 18, "true", 4);
  LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, dtc);
  LLVMAttributeRef nfpe = LLVMCreateStringAttribute(
      cg->ctx, "no-frame-pointer-elim", 21, "true", 4);
  LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, nfpe);
  LLVMAttributeRef nfpenl = LLVMCreateStringAttribute(
      cg->ctx, "no-frame-pointer-elim-non-leaf", 30, "true", 4);
  LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, nfpenl);
  unsigned uw_kind = LLVMGetEnumAttributeKindForName("uwtable", 7);
  if (uw_kind != 0) {
    LLVMAttributeRef uw = LLVMCreateEnumAttribute(cg->ctx, uw_kind, 0);
    LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, uw);
  }
}

LLVMValueRef build_alloca(codegen_t *cg, const char *name, LLVMTypeRef type) {
  LLVMBuilderRef b = cg->alloca_builder;
  if (!b)
    return NULL;
  LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
  if (!f)
    return NULL;
  LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(f);
  if (!entry)
    return NULL;
  LLVMValueRef first = LLVMGetFirstInstruction(entry);
  if (first)
    LLVMPositionBuilderBefore(b, first);
  else
    LLVMPositionBuilderAtEnd(b, entry);
  return LLVMBuildAlloca(b, type, name);
}

void codegen_init_with_context(codegen_t *cg, program_t *prog,
                               struct arena_t *arena, LLVMModuleRef mod,
                               LLVMContextRef ctx, LLVMBuilderRef builder) {
  memset(cg, 0, sizeof(codegen_t));
  cg->ctx = ctx;
  cg->module = mod;
  cg->builder = builder;
  cg->alloca_builder = LLVMCreateBuilderInContext(ctx);
  cg->prog = prog;
  cg->arena = arena;
  ny_llvm_prepare_module(cg->module);

  cg->comptime = false;
  cg->strict_diagnostics = getenv("NYTRIX_STRICT_DIAGNOSTICS") != NULL;
  cg->auto_purity_infer = ny_env_enabled_default_on_local("NYTRIX_AUTO_PURITY");
  if (!cg->auto_purity_infer && ny_effect_analysis_requested())
    cg->auto_purity_infer = true;
  cg->auto_memoize = ny_env_enabled("NYTRIX_AUTO_MEMO");
  cg->auto_memoize_impure = ny_env_enabled("NYTRIX_AUTO_MEMO_IMPURE");
  cg->auto_memo_site_seq = 0;
  if (braun_should_init()) {
    cg->braun = malloc(sizeof(*cg->braun));
    if (cg->braun) {
      braun_ssa_init(cg->braun, cg, arena);
      braun_ssa_set_enabled(cg->braun, true);
    }
  }
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
  memset(cg, 0, sizeof(codegen_t));
  cg->prog = prog;
  cg->arena = arena;
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMLoadLibraryPermanently(NULL);
  cg->ctx = LLVMContextCreate();
  cg->llvm_ctx_owned = true;
  cg->module = LLVMModuleCreateWithNameInContext(name, cg->ctx);
  cg->builder = LLVMCreateBuilderInContext(cg->ctx);
  cg->alloca_builder = LLVMCreateBuilderInContext(cg->ctx);
  ny_llvm_prepare_module(cg->module);
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
  if (braun_should_init()) {
    cg->braun = malloc(sizeof(*cg->braun));
    if (cg->braun) {
      braun_ssa_init(cg->braun, cg, arena);
      braun_ssa_set_enabled(cg->braun, true);
    }
  }

  cg->strict_diagnostics = getenv("NYTRIX_STRICT_DIAGNOSTICS") != NULL;
  cg->auto_purity_infer = ny_env_enabled_default_on_local("NYTRIX_AUTO_PURITY");
  if (!cg->auto_purity_infer && ny_effect_analysis_requested())
    cg->auto_purity_infer = true;
  cg->auto_memoize = ny_env_enabled("NYTRIX_AUTO_MEMO");
  cg->auto_memoize_impure = ny_env_enabled("NYTRIX_AUTO_MEMO_IMPURE");
  cg->auto_memo_site_seq = 0;
  if (cg->strict_diagnostics)
    NY_LOG_V1("Strict diagnostics enabled (NYTRIX_STRICT_DIAGNOSTICS)\n");
  add_builtins(cg);
  LLVMAddGlobal(cg->module, cg->type_i64, "__NYTRIX__");
}

const char *codegen_qname(codegen_t *cg, const char *name,
                          const char *cur_mod) {
  if (!cur_mod || !*cur_mod)
    return name;
  size_t mlen = strlen(cur_mod);
  if (strncmp(name, cur_mod, mlen) == 0 && name[mlen] == '.')
    return name;

  int len = snprintf(NULL, 0, "%s.%s", cur_mod, name);
  char *buf = arena_alloc(cg->arena, (size_t)len + 1);
  snprintf(buf, (size_t)len + 1, "%s.%s", cur_mod, name);
  return buf;
}

void emit_top_functions(codegen_t *cg, stmt_t *s, scope *gsc, size_t gd,
                        const char *cur_mod) {
  if (s->kind == NY_S_FUNC) {
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
  // Infer effect-free functions before body emission so call generation can
  // use purity metadata for optional memoization.
  infer_pure_functions(cg);
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
  ny_debug_apply_fn_attrs(cg, fn);
  LLVMMetadataRef prev_scope = cg->di_scope;
  if (cg->debug_symbols && cg->di_builder) {
    token_t tok = {0};
    tok.filename = cg->debug_main_file ? cg->debug_main_file : "<inline>";
    tok.line = 0;
    tok.col = 0;
    LLVMMetadataRef sp = codegen_debug_subprogram(cg, fn, name, tok);
    if (sp)
      cg->di_scope = sp;
    /* Let statement emission establish source locations; avoid a wide
       synthetic line-1 range over function setup/prologue. */
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
  }
  LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
  if (cg->braun)
    braun_ssa_reset(cg->braun);
  // Create two blocks: init (for internal setup) and body (for user code)
  LLVMBasicBlockRef init_block = LLVMAppendBasicBlock(fn, "init");
  LLVMBasicBlockRef body_block = LLVMAppendBasicBlock(fn, "body");
  // 1. Generate user code first (into body_block) to discover all strings
  LLVMPositionBuilderAtEnd(cg->builder, body_block);
  if (cg->braun)
    braun_ssa_start_block(cg->braun, body_block);
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
  if (cg->braun) {
    braun_ssa_start_block(cg->braun, init_block);
    braun_ssa_seal_block(cg->braun, init_block);
  }
  if (cg->debug_symbols && cg->di_builder) {
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
  }
  codegen_emit_string_init(cg);
  // Jump from init to body
  LLVMBuildBr(cg->builder, body_block);
  if (cg->braun) {
    braun_ssa_start_block(cg->braun, body_block);
    braun_ssa_add_predecessor(cg->braun, init_block);
    braun_ssa_seal_block(cg->braun, body_block);
  }
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

void codegen_emit_string_init(codegen_t *cg) {
  LLVMTypeRef i8_ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
  LLVMTypeRef asm_func_ty =
      LLVMFunctionType(i8_ptr_ty, (LLVMTypeRef[]){i8_ptr_ty}, 1, false);
  LLVMValueRef identity_asm =
      LLVMConstInlineAsm(asm_func_ty, "", "=r,0", true, false);
  for (size_t i = 0; i < cg->interns.len; i++) {
    if (cg->interns.data[i].module != cg->module)
      continue;
    LLVMValueRef str_array_global = cg->interns.data[i].gv;
    LLVMValueRef runtime_ptr_global = cg->interns.data[i].val;
    if (!str_array_global || !runtime_ptr_global)
      continue;
    LLVMTypeRef rt_ty = LLVMTypeOf(runtime_ptr_global);
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
    // 4. Store pointer (or ptrtoint if runtime slot is integer)
    if (LLVMGetTypeKind(rt_ty) == LLVMPointerTypeKind) {
      LLVMBuildStore(cg->builder, str_data_ptr, runtime_ptr_global);
    } else {
      LLVMValueRef str_data_int =
          LLVMBuildPtrToInt(cg->builder, str_data_ptr, cg->type_i64, "");
      LLVMBuildStore(cg->builder, str_data_int, runtime_ptr_global);
    }
  }
}

void codegen_dispose(codegen_t *cg) {
  if (!cg)
    return;
  if (cg->braun) {
    if (ny_env_enabled("NYTRIX_BRAUN_DIAG")) {
      uint64_t phi_created = 0, phi_eliminated = 0, allocas_avoided = 0;
      braun_ssa_get_stats(cg->braun, &phi_created, &phi_eliminated,
                          &allocas_avoided);
      fprintf(
          stderr,
          "[braun] phi-created=%llu phi-eliminated=%llu writes-tracked=%llu\n",
          (unsigned long long)phi_created, (unsigned long long)phi_eliminated,
          (unsigned long long)allocas_avoided);
    }
    braun_ssa_dispose(cg->braun);
    free(cg->braun);
    cg->braun = NULL;
  }
  codegen_debug_finalize(cg);
  if (cg->alloca_builder) {
    LLVMDisposeBuilder(cg->alloca_builder);
    cg->alloca_builder = NULL;
  }
  if (cg->ee) {
    LLVMDisposeExecutionEngine(cg->ee);
    cg->ee = NULL;
  }
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
  for (size_t i = 0; i < cg->fun_sigs.len; i++) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    free((void *)sig->name);
    if (sig->link_name) {
      bool seen = false;
      for (size_t j = 0; j < i; j++) {
        if (cg->fun_sigs.data[j].link_name == sig->link_name) {
          seen = true;
          break;
        }
      }
      if (!seen)
        free((void *)sig->link_name);
    }
    if (sig->return_type) {
      bool seen = false;
      for (size_t j = 0; j < i; j++) {
        if (cg->fun_sigs.data[j].return_type == sig->return_type) {
          seen = true;
          break;
        }
      }
      if (!seen)
        free((void *)sig->return_type);
    }
  }
  for (size_t i = 0; i < cg->global_vars.len; i++) {
    free((void *)cg->global_vars.data[i].name);
  }
  for (size_t i = 0; i < cg->interns.len; i++) {
    if (cg->interns.data[i].alloc)
      free(cg->interns.data[i].alloc);
  }
  for (size_t i = 0; i < cg->aliases.len; i++) {
    free((void *)cg->aliases.data[i].name);
    free((void *)cg->aliases.data[i].stmt_t);
  }
  for (size_t i = 0; i < cg->import_aliases.len; i++) {
    free((void *)cg->import_aliases.data[i].name);
    free((void *)cg->import_aliases.data[i].stmt_t);
  }
  for (size_t i = 0; i < cg->user_import_aliases.len; i++) {
    free((void *)cg->user_import_aliases.data[i].name);
    free((void *)cg->user_import_aliases.data[i].stmt_t);
  }
  for (size_t i = 0; i < cg->use_modules.len; i++) {
    free((void *)cg->use_modules.data[i]);
  }
  for (size_t i = 0; i < cg->user_use_modules.len; i++) {
    free((void *)cg->user_use_modules.data[i]);
  }
  for (size_t i = 0; i < cg->labels.len; i++) {
    free((void *)cg->labels.data[i].name);
  }
  for (size_t i = 0; i < cg->enums.len; i++) {
    enum_def_t *enu = cg->enums.data[i];
    if (!enu)
      continue;
    free((void *)enu->name);
    for (size_t j = 0; j < enu->members.len; j++) {
      free((void *)enu->members.data[j].name);
    }
    vec_free(&enu->members);
  }
  for (size_t i = 0; i < cg->layouts.len; i++) {
    layout_def_t *def = cg->layouts.data[i];
    if (!def)
      continue;
    free((void *)def->name);
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
  vec_free(&cg->labels);
  vec_free(&cg->enums);
  vec_free(&cg->layouts);
  if (cg->prog && cg->prog_owned) {
    program_free(cg->prog, (arena_t *)cg->arena);
    free(cg->prog);
  }
}
