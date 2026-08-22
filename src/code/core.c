/*
 * Codegen core: top-level LLVM code-generation driver, module emission,
 * optimization pipeline setup, and the main compile-entry orchestration.
 *
 * Large implementation sections are kept in focused include fragments so the
 * build still sees one translation unit while initialization, lazy reachability,
 * link collection, emission, and disposal remain independently navigable.
 */
#include "base/util.h"
#include "code/jit.h"
#include "code/llvm.h"
#include "code/priv.h"
#include "code/typeinfer.h"
#include "code/visitor.h"
#include "fficlang.h"
#include "priv.h"
#include "rt/shared.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif
#include <limits.h>
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
#include <time.h>

static bool ny_effect_analysis_requested(void) {
  static int cached = -1;
  if (cached >= 0)
    return cached != 0;
  const char *forbid = getenv("NYTRIX_EFFECT_FORBID");
  if (forbid && *forbid)
    cached = 1;
  else
    cached = (ny_env_enabled("NYTRIX_EFFECT_DIAG") ||
              ny_env_enabled("NYTRIX_EFFECT_DIAG_VERBOSE") ||
              ny_env_enabled("NYTRIX_EFFECT_REQUIRE_PURE") ||
              ny_env_enabled("NYTRIX_EFFECT_REQUIRE_KNOWN") ||
              ny_env_enabled("NYTRIX_EFFECT_ASYNC_LOWERING"))
                 ? 1
                 : 0;
  return cached != 0;
}

static LLVMValueRef ny_replace_llvm_used_global(LLVMModuleRef module,
                                                LLVMTypeRef elem_ty,
                                                LLVMValueRef *elements,
                                                size_t count) {
  if (!module || !elem_ty || !elements || count == 0 || count > UINT_MAX)
    return NULL;
  LLVMValueRef old = LLVMGetNamedGlobal(module, "llvm.used");
  if (old)
    LLVMDeleteGlobal(old);
  LLVMTypeRef arr_ty = LLVMArrayType(elem_ty, (unsigned)count);
  LLVMValueRef arr = LLVMConstArray(elem_ty, elements, (unsigned)count);
  LLVMValueRef used = LLVMAddGlobal(module, arr_ty, "llvm.used");
  LLVMSetLinkage(used, LLVMAppendingLinkage);
  LLVMSetSection(used, "llvm.metadata");
  LLVMSetGlobalConstant(used, true);
  LLVMSetInitializer(used, arr);
  return used;
}

static void ny_register_impl_types_stmt(codegen_t *cg, stmt_t *s) {
  if (!cg || !s)
    return;
  switch (s->kind) {
  case NY_S_IMPL:
    ny_register_tagged_type(cg, s->as.impl.type_name);
    break;
  case NY_S_LEMMA:
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++)
      ny_register_impl_types_stmt(cg, s->as.module.body.data[i]);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      ny_register_impl_types_stmt(cg, s->as.block.body.data[i]);
    break;
  case NY_S_IF:
    ny_register_impl_types_stmt(cg, s->as.iff.conseq);
    ny_register_impl_types_stmt(cg, s->as.iff.alt);
    break;
  case NY_S_GUARD:
    ny_register_impl_types_stmt(cg, s->as.guard.fallback);
    break;
  default:
    break;
  }
}

static void ny_cg_init_types(codegen_t *cg) {
  cg->type_i1 = ny_i1_ty(cg);
  cg->type_i8 = ny_i8_ty(cg);
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
  cg->strict_types = ny_env_enabled("NYTRIX_STRICT_TYPES");
  cg->user_native_abi = ny_env_enabled_default_on("NYTRIX_USER_NATIVE_ABI");
  cg->auto_purity_infer = ny_env_enabled_default_on("NYTRIX_AUTO_PURITY") ||
                          ny_effect_analysis_requested();
  cg->auto_memoize = ny_env_enabled("NYTRIX_AUTO_MEMO");
  cg->auto_memoize_impure = ny_env_enabled("NYTRIX_AUTO_MEMO_IMPURE");
  cg->trace_exec = ny_env_enabled("NYTRIX_TRACE");
  cg->trace_emit_disabled =
      !cg->trace_exec || ny_env_enabled("NYTRIX_NO_TRACE");
#ifdef DEBUG
  cg->debug_symbols = true;
  cg->trace_exec = true;
  cg->trace_emit_disabled = ny_env_enabled("NYTRIX_NO_TRACE");
#endif
  cg->llvm_value_names =
      cg->debug_symbols || ny_env_enabled("NYTRIX_LLVM_NAMES");
  if (cg->strict_diagnostics)
    NY_LOG_V1("Strict diagnostics enabled (NYTRIX_STRICT_DIAGNOSTICS)\n");
  if (cg->strict_types)
    NY_LOG_V1("Compile-time type checks enabled (NYTRIX_STRICT_TYPES)\n");
}

static bool ny_triple_is_apple(const char *triple) {
  if (!triple || !*triple)
    return false;
  return strstr(triple, "apple") || strstr(triple, "darwin") ||
         strstr(triple, "macos");
}

static bool ny_triple_is_apple_arm64(const char *triple) {
  if (!ny_triple_is_apple(triple))
    return false;
  bool is_arm64 = strstr(triple, "arm64") || strstr(triple, "aarch64");
  return is_arm64;
}

bool ny_module_target_is_apple_arm64(LLVMModuleRef module) {
  const char *triple = module ? LLVMGetTarget(module) : NULL;
  if (triple && *triple)
    return ny_triple_is_apple_arm64(triple);
  const char *env_triple = getenv("NYTRIX_HOST_TRIPLE");
  if (env_triple && *env_triple)
    return ny_triple_is_apple_arm64(env_triple);
  char *default_triple = LLVMGetDefaultTargetTriple();
  bool result = ny_triple_is_apple_arm64(default_triple);
  if (default_triple)
    LLVMDisposeMessage(default_triple);
  return result;
}

static bool ny_module_target_is_apple(LLVMModuleRef module) {
  const char *triple = module ? LLVMGetTarget(module) : NULL;
  if (triple && *triple)
    return ny_triple_is_apple(triple);
  const char *env_triple = getenv("NYTRIX_HOST_TRIPLE");
  if (env_triple && *env_triple)
    return ny_triple_is_apple(env_triple);
  char *default_triple = LLVMGetDefaultTargetTriple();
  bool result = ny_triple_is_apple(default_triple);
  if (default_triple)
    LLVMDisposeMessage(default_triple);
  return result;
}

void ny_apply_rt_fn_attrs(codegen_t *cg, LLVMValueRef fn) {
  if (!cg || !fn)
    return;

  bool is_apple = ny_module_target_is_apple(cg->module);
  bool is_apple_arm64 = ny_module_target_is_apple_arm64(cg->module);

  if (cg->debug_symbols || is_apple) {

    add_fn_string_attr(cg, fn, "frame-pointer", "all");

    if (is_apple_arm64) {
      add_fn_string_attr(cg, fn, "no-frame-pointer-elim", "true");
      add_fn_string_attr(cg, fn, "no-frame-pointer-elim-non-leaf", "true");

      add_fn_string_attr(cg, fn, "no-red-zone", "true");
    }

    add_fn_enum_attr(cg, fn, "uwtable", 1);
  }
}

void ny_apply_longjmp_fn_attrs(codegen_t *cg, LLVMValueRef fn) {
  if (!cg || !fn)
    return;
  add_fn_string_attr(cg, fn, "frame-pointer", "all");
  add_fn_string_attr(cg, fn, "no-frame-pointer-elim", "true");
  add_fn_string_attr(cg, fn, "no-frame-pointer-elim-non-leaf", "true");
  add_fn_string_attr(cg, fn, "no-red-zone", "true");
  add_fn_enum_attr(cg, fn, "uwtable", 1);
}

static void ny_debug_apply_fn_attrs(codegen_t *cg, LLVMValueRef fn) {
  ny_apply_rt_fn_attrs(cg, fn);
}

LLVMValueRef build_alloca(codegen_t *cg, const char *name, LLVMTypeRef type) {
  LLVMBuilderRef b = cg->alloca_builder;
  if (!b)
    return NULL;
  LLVMValueRef f = cg->current_fn_value;
  if (!f) {
    f = ny_cur_fn(cg);
    if (!f)
      return NULL;
  }
  LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(f);
  if (!entry)
    return NULL;
  LLVMValueRef first = LLVMGetFirstInstruction(entry);
  if (first)
    LLVMPositionBuilderBefore(b, first);
  else
    LLVMPositionBuilderAtEnd(b, entry);
  LLVMValueRef slot = LLVMBuildAlloca(b, type, ny_llvm_name(cg, name));
  LLVMSetAlignment(slot, 16);
  return slot;
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
  cg->ownership_enabled = ny_env_enabled("NYTRIX_OWNERSHIP");
  cg->ownership_strict = ny_env_enabled("NYTRIX_OWNERSHIP_STRICT");
  cg->ownership_runtime_cleanup = ny_env_enabled("NYTRIX_OWNERSHIP_CLEANUP");
  cg->heap_policy = getenv("NYTRIX_HEAP_POLICY");
  if (!cg->heap_policy || !*cg->heap_policy)
    cg->heap_policy = "manual";
  cg->rc_heap_enabled =
      ny_env_enabled("NYTRIX_RC_GC") || strcmp(cg->heap_policy, "rc") == 0;
  cg->emit_module_name = NULL;
  cg->emit_module_decls_only = false;
  cg->emit_script = true;
  ny_llvm_prepare_module(cg->module, 3);
  vec_reserve(&cg->fun_sigs, 4096);
  vec_reserve(&cg->global_vars, 1024);
  vec_reserve(&cg->interns, 512);
  vec_init(&cg->aliases);
  vec_init(&cg->import_aliases);
  vec_init(&cg->user_import_aliases);
  vec_init(&cg->import_alias_hashes);
  vec_init(&cg->user_import_alias_hashes);
  vec_init(&cg->use_modules);
  vec_init(&cg->user_use_modules);
  vec_init(&cg->link_allowed_modules);
  vec_init(&cg->tagged_types);
  vec_init(&cg->lazy_emit_names);
  vec_init(&cg->lazy_emit_hashes);
  vec_init(&cg->lazy_emit_collected_names);
  vec_init(&cg->lazy_emit_collected_hashes);
  vec_init(&cg->labels);
  vec_init(&cg->extra_arenas);
  vec_init(&cg->extra_progs);
  vec_init(&cg->operators);
  vec_init(&cg->enums);
  vec_init(&cg->layouts);
  vec_init(&cg->mono_specs);
  vec_init(&cg->links);
  vec_init(&cg->ffi.defines);
  ny_cg_init_types(cg);
  ny_cg_init_options(cg);
  add_builtins(cg);
}

void codegen_init(codegen_t *cg, program_t *prog, struct arena_t *arena,
                  const char *name) {
  memset(cg, 0, sizeof(codegen_t));
  cg->prog = prog;
  cg->arena = arena;
  /*
   * LLVM native target init is process-global and idempotent but not free.
   * Guard it so repeated codegen_init calls (REPL, tests) don't re-register.
   */
  static int g_cg_native_initialized = 0;
  if (!g_cg_native_initialized) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMLoadLibraryPermanently(NULL);
    g_cg_native_initialized = 1;
  }
  cg->ctx = LLVMContextCreate();
  cg->llvm_ctx_owned = true;
  cg->module = LLVMModuleCreateWithNameInContext(name, cg->ctx);
  cg->builder = LLVMCreateBuilderInContext(cg->ctx);
  cg->alloca_builder = LLVMCreateBuilderInContext(cg->ctx);
  ny_llvm_prepare_module(cg->module, 3);
  cg->owned_metadata = true;
  cg->ownership_enabled = ny_env_enabled("NYTRIX_OWNERSHIP");
  cg->ownership_strict = ny_env_enabled("NYTRIX_OWNERSHIP_STRICT");
  cg->ownership_runtime_cleanup = ny_env_enabled("NYTRIX_OWNERSHIP_CLEANUP");
  cg->heap_policy = getenv("NYTRIX_HEAP_POLICY");
  if (!cg->heap_policy || !*cg->heap_policy)
    cg->heap_policy = "manual";
  cg->rc_heap_enabled =
      ny_env_enabled("NYTRIX_RC_GC") || strcmp(cg->heap_policy, "rc") == 0;
  cg->emit_module_name = NULL;
  cg->emit_module_decls_only = false;
  cg->emit_script = true;
  vec_reserve(&cg->fun_sigs, 16384);
  vec_reserve(&cg->global_vars, 4096);
  vec_reserve(&cg->interns, 2048);
  vec_init(&cg->aliases);
  vec_init(&cg->import_aliases);
  vec_init(&cg->user_import_aliases);
  vec_init(&cg->import_alias_hashes);
  vec_init(&cg->user_import_alias_hashes);
  vec_init(&cg->use_modules);
  vec_init(&cg->user_use_modules);
  vec_init(&cg->link_allowed_modules);
  vec_init(&cg->tagged_types);
  vec_init(&cg->lazy_emit_names);
  vec_init(&cg->lazy_emit_hashes);
  vec_init(&cg->lazy_emit_collected_names);
  vec_init(&cg->lazy_emit_collected_hashes);
  vec_init(&cg->labels);
  vec_init(&cg->extra_arenas);
  vec_init(&cg->extra_progs);
  vec_init(&cg->operators);
  vec_init(&cg->enums);
  vec_init(&cg->layouts);
  vec_init(&cg->mono_specs);
  vec_init(&cg->links);
  vec_init(&cg->ffi.defines);
  ny_cg_init_types(cg);
  ny_cg_init_options(cg);
  add_builtins(cg);
  LLVMAddGlobal(cg->module, cg->type_i64, "__NYTRIX__");

  cg->opt_enabled = cg->debug_opt_level > 0 ||
                    ny_env_enabled("NYTRIX_ENABLE_OPTIMIZE") ||
                    ny_env_enabled("NYTRIX_OPT_ENABLE");
  cg->opt_type_infer =
      ny_env_enabled("NYTRIX_ENABLE_TYPEINFER") ||
      ny_env_enabled("NYTRIX_ENABLE_OPTIMIZE") ||
      !ny_env_enabled("NYTRIX_DISABLE_TYPEINFER");
  cg->opt_const_fold =
      cg->opt_enabled || ny_env_enabled("NYTRIX_ENABLE_CONST_FOLD");
  cg->opt_tail_call =
      cg->opt_enabled || ny_env_enabled("NYTRIX_ENABLE_TAIL_CALL");
  cg->opt_inline_small =
      cg->opt_enabled || ny_env_enabled("NYTRIX_ENABLE_INLINE");
  cg->opt_lazy_load =
      cg->opt_enabled || ny_env_enabled("NYTRIX_ENABLE_LAZY_LOAD");

  cg->opt_sys_mode =
      ny_env_enabled("NYTRIX_SYS_MODE") || ny_env_enabled("NYTRIX_SYS");
  cg->opt_unsafe_arith = ny_env_enabled("NYTRIX_UNSAFE_ARITH") ||
                         ny_env_enabled("NYTRIX_UNSAFE_FIXNUM");
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

bool ny_emit_module_match(codegen_t *cg, const char *cur_mod) {
  if (!cg || !cg->emit_module_name)
    return true;
  if (!cg->emit_module_name[0])
    return (!cur_mod || !*cur_mod);
  if (!cur_mod || !*cur_mod)
    return false;
  if (strcmp(cg->emit_module_name, cur_mod) == 0)
    return true;
  if (strcmp(cg->emit_module_name, "std") != 0 &&
      strcmp(cg->emit_module_name, "lib") != 0)
    return false;
  size_t wanted_len = strlen(cg->emit_module_name);
  return strncmp(cg->emit_module_name, cur_mod, wanted_len) == 0 &&
         cur_mod[wanted_len] == '.';
}

#include "core/lazy.h"

#include "core/links.h"

#include "core/emit.h"

#include "core/dispose.h"
