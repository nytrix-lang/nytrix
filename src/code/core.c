#include "base/util.h"
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
#include <time.h>
#include <sys/types.h>

static bool ny_effect_analysis_requested(void) {
  const char *forbid = getenv("NYTRIX_EFFECT_FORBID");
  if (forbid && *forbid)
    return true;
  return ny_env_enabled("NYTRIX_EFFECT_DIAG") ||
         ny_env_enabled("NYTRIX_EFFECT_DIAG_VERBOSE") ||
         ny_env_enabled("NYTRIX_EFFECT_REQUIRE_PURE") ||
         ny_env_enabled("NYTRIX_EFFECT_REQUIRE_KNOWN");
}

static void ny_cg_init_types(codegen_t *cg) {
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
}

static void ny_cg_init_options(codegen_t *cg) {
  cg->strict_diagnostics = getenv("NYTRIX_STRICT_DIAGNOSTICS") != NULL;
  cg->auto_purity_infer = ny_env_enabled_default_on("NYTRIX_AUTO_PURITY") ||
                          ny_effect_analysis_requested();
  cg->auto_memoize = ny_env_enabled("NYTRIX_AUTO_MEMO");
  cg->auto_memoize_impure = ny_env_enabled("NYTRIX_AUTO_MEMO_IMPURE");
  cg->trace_exec = ny_env_enabled("NYTRIX_TRACE");
#ifdef DEBUG
  cg->debug_symbols = true;
  cg->trace_exec = true;
#endif
  if (cg->strict_diagnostics)
    NY_LOG_V1("Strict diagnostics enabled (NYTRIX_STRICT_DIAGNOSTICS)\n");
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
  cg->owned_metadata = false;
  ny_llvm_prepare_module(cg->module, 3);
  vec_reserve(&cg->fun_sigs, 1024);
  vec_reserve(&cg->global_vars, 256);
  vec_reserve(&cg->interns, 256);
  vec_init(&cg->aliases);
  vec_init(&cg->import_aliases);
  vec_init(&cg->user_import_aliases);
  vec_init(&cg->use_modules);
  vec_init(&cg->user_use_modules);
  vec_init(&cg->labels);
  vec_init(&cg->extra_arenas);
  vec_init(&cg->extra_progs);
  vec_init(&cg->enums);
  vec_init(&cg->layouts);
  vec_init(&cg->links);
  ny_cg_init_types(cg);
  ny_cg_init_options(cg);
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
  ny_llvm_prepare_module(cg->module, 3);
  cg->owned_metadata = true;
  vec_reserve(&cg->fun_sigs, 16384);
  vec_reserve(&cg->global_vars, 4096);
  vec_reserve(&cg->interns, 4096);
  ny_cg_init_types(cg);
  ny_cg_init_options(cg);
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

static void process_links(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_LINK) {
    if (s->as.link.lib) {
      bool found = false;
      for (size_t i = 0; i < cg->links.len; i++) {
        if (strcmp(cg->links.data[i], s->as.link.lib) == 0) {
          found = true;
          break;
        }
      }
      if (!found)
        vec_push(&cg->links, ny_strdup(s->as.link.lib));
    }
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      process_links(cg, s->as.module.body.data[i]);
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      process_links(cg, s->as.block.body.data[i]);
  }
}

void codegen_prepare(codegen_t *cg) {
  if (!cg || !cg->prog || cg->is_preparing)
    return;

  cg->is_preparing = true;

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    collect_use_aliases(cg, s);
    collect_use_modules(cg, s);
    process_links(cg, s);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    collect_sigs(cg, s);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    process_exports(cg, s);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    process_use_imports(cg, s);
  }

  infer_pure_functions(cg);

  cg->is_preparing = false;
}

void codegen_emit(codegen_t *cg) {
  scope gsc[64] = {0};
  size_t gd = 0;

  clock_t t_emit_top = clock();
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok)) {
      continue;
    }
    emit_top_functions(cg, s, gsc, gd, NULL);
  }
  if (verbose_enabled >= 1)
    fprintf(stderr, "[*] Codegen: emit top:       %.4fs\n", (double)(clock() - t_emit_top) / CLOCKS_PER_SEC);
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
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
  }
  LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
  LLVMBasicBlockRef init_block = LLVMAppendBasicBlock(fn, "init");
  LLVMBasicBlockRef body_block = LLVMAppendBasicBlock(fn, "body");
  LLVMPositionBuilderAtEnd(cg->builder, body_block);
  scope sc[64] = {0};
  size_t d = 0;
  LLVMValueRef std_init = LLVMGetNamedFunction(cg->module, "__std_init");
  if (std_init) {
    LLVMBuildCall2(cg->builder, LLVMGlobalGetValueType(std_init), std_init, NULL, 0, "");
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok))
      continue;
    if (s->kind != NY_S_FUNC) {
      gen_stmt(cg, sc, &d, s, 0, false);
    }
  }
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    LLVMBuildRet(cg->builder, LLVMConstInt(cg->type_i64, 1, false));
  }
  LLVMPositionBuilderAtEnd(cg->builder, init_block);
  if (cg->debug_symbols && cg->di_builder) {
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
  }
  codegen_emit_string_init(cg);
  LLVMBuildBr(cg->builder, body_block);
  vec_free(&sc[0].defers);
  vec_free(&sc[0].vars);
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
    LLVMValueRef global_i8_ptr =
        LLVMBuildBitCast(cg->builder, str_array_global, i8_ptr_ty, "");
    LLVMValueRef runtime_base =
        LLVMBuildCall2(cg->builder, asm_func_ty, identity_asm,
                       (LLVMValueRef[]){global_i8_ptr}, 1, "");
    LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, 0)};
    LLVMValueRef str_data_ptr =
        LLVMBuildInBoundsGEP2(cg->builder, LLVMInt8TypeInContext(cg->ctx),
                              runtime_base, indices, 1, "");
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
  codegen_debug_finalize(cg);
  ny_sym_state_free(cg);
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
  vec_free(&cg->fun_sigs);
  vec_free(&cg->global_vars);
  vec_free(&cg->interns);
  if (cg->intern_map) {
    free(cg->intern_map);
    cg->intern_map = NULL;
  }
  vec_free(&cg->aliases);
  vec_free(&cg->import_aliases);
  vec_free(&cg->user_import_aliases);
  vec_free(&cg->use_modules);
  vec_free(&cg->user_use_modules);
  vec_free(&cg->labels);
  vec_free(&cg->enums);
  vec_free(&cg->layouts);
  vec_free(&cg->links);
  vec_free(&cg->extra_arenas);
  vec_free(&cg->extra_progs);
  if (cg->prog && cg->prog_owned) {
    program_free(cg->prog, (arena_t *)cg->arena);
    free(cg->prog);
  }
}
