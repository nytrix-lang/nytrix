#include "base/util.h"
#include "code/llvm.h"
#include "code/priv.h"
#include "code/typeinfer.h"
#include "code/visitor.h"
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
  cg->ownership_runtime_cleanup = cg->ownership_enabled;
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
  cg->ownership_enabled = ny_env_enabled("NYTRIX_OWNERSHIP");
  cg->ownership_strict = ny_env_enabled("NYTRIX_OWNERSHIP_STRICT");
  cg->ownership_runtime_cleanup = cg->ownership_enabled;
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

static size_t ny_lazy_name_set_slot(uint64_t hash, size_t cap) {
  uint64_t mixed = hash ^ (hash >> 33);
  mixed *= UINT64_C(0xff51afd7ed558ccd);
  mixed ^= mixed >> 33;
  return (size_t)mixed & (cap - 1);
}

static bool ny_lazy_name_set_grow(ny_name_set *set, size_t min_cap) {
  if (!set)
    return false;
  size_t cap = set->cap ? set->cap : 32;
  while (cap < min_cap)
    cap <<= 1;
  ny_name_set_slot *slots = calloc(cap, sizeof(*slots));
  if (!slots)
    return false;
  if (set->slots) {
    for (size_t i = 0; i < set->cap; ++i) {
      ny_name_set_slot old = set->slots[i];
      if (!old.name)
        continue;
      size_t idx = ny_lazy_name_set_slot(old.hash, cap);
      while (slots[idx].name)
        idx = (idx + 1) & (cap - 1);
      slots[idx] = old;
    }
    free(set->slots);
  }
  set->slots = slots;
  set->cap = cap;
  return true;
}

static bool ny_lazy_name_set_contains_hash(const ny_name_set *set,
                                           const char *name, uint64_t hash) {
  if (!set || !set->slots || !set->cap || !name || !*name)
    return false;
  size_t name_len = strlen(name);
  size_t idx = ny_lazy_name_set_slot(hash, set->cap);
  for (size_t probe = 0; probe < set->cap; ++probe) {
    const ny_name_set_slot *slot = &set->slots[idx];
    if (!slot->name)
      return false;
    if (slot->hash == hash && slot->len == name_len &&
        (slot->name == name || memcmp(slot->name, name, name_len) == 0))
      return true;
    idx = (idx + 1) & (set->cap - 1);
  }
  return false;
}

static bool ny_lazy_name_set_insert_hash(ny_name_set *set, const char *name,
                                         uint64_t hash) {
  if (!set || !name || !*name)
    return false;
  size_t name_len = strlen(name);
  if (ny_lazy_name_set_contains_hash(set, name, hash))
    return false;
  if (!set->cap || ((set->len + 1) * 10) >= (set->cap * 7)) {
    size_t target = set->cap ? set->cap << 1 : 32;
    if (!ny_lazy_name_set_grow(set, target))
      return false;
  }
  size_t idx = ny_lazy_name_set_slot(hash, set->cap);
  while (set->slots[idx].name)
    idx = (idx + 1) & (set->cap - 1);
  set->slots[idx].name = name;
  set->slots[idx].hash = hash;
  set->slots[idx].len = name_len;
  set->len++;
  return true;
}

static void ny_lazy_name_set_free(ny_name_set *set) {
  if (!set)
    return;
  free(set->slots);
  set->slots = NULL;
  set->cap = 0;
  set->len = 0;
}

static bool ny_lazy_emit_name_list_contains(const assigned_name_list *names,
                                            const assigned_hash_list *hashes,
                                            const uint64_t bloom[4],
                                            const ny_name_set *set,
                                            const char *name, uint64_t hash) {
  if (!name || !*name)
    return false;
  if (!ny_name_bloom_maybe_has(bloom, hash))
    return false;
  if (set && set->slots)
    return ny_lazy_name_set_contains_hash(set, name, hash);
  return assigned_name_has(names, hashes, name, hash, bloom);
}

static void ny_lazy_emit_add_stable_name(codegen_t *cg,
                                         assigned_name_list *names,
                                         assigned_hash_list *hashes,
                                         uint64_t bloom[4], ny_name_set *set,
                                         const char *name) {
  if (!cg || !names || !hashes || !name || !*name)
    return;
  uint64_t hash = ny_hash64_cstr(name);
  if (ny_lazy_emit_name_list_contains(names, hashes, bloom, set, name, hash))
    return;
  const char *stable = arena_strndup(cg->arena, name, strlen(name));
  vec_push(names, stable);
  vec_push(hashes, hash);
  ny_name_bloom_add(bloom, hash);
  ny_lazy_name_set_insert_hash(set, stable, hash);
}

static void ny_lazy_emit_add_name(codegen_t *cg, const char *name) {
  ny_lazy_emit_add_stable_name(cg, &cg->lazy_emit_names, &cg->lazy_emit_hashes,
                               cg->lazy_emit_bloom, &cg->lazy_emit_name_set,
                               name);
}

static bool ny_lazy_emit_collected_function(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return false;
  uint64_t hash = ny_hash64_cstr(name);
  return ny_lazy_emit_name_list_contains(
      &cg->lazy_emit_collected_names, &cg->lazy_emit_collected_hashes,
      cg->lazy_emit_collected_bloom, &cg->lazy_emit_collected_set, name, hash);
}

static void ny_lazy_emit_mark_collected_function(codegen_t *cg,
                                                 const char *name) {
  if (!cg || !name || !*name)
    return;
  ny_lazy_emit_add_stable_name(cg, &cg->lazy_emit_collected_names,
                               &cg->lazy_emit_collected_hashes,
                               cg->lazy_emit_collected_bloom,
                               &cg->lazy_emit_collected_set, name);
}

static void ny_lazy_emit_add_resolved_name(codegen_t *cg, const char *name,
                                           int depth) {
  if (!cg || !name || !*name || depth > 8)
    return;
  ny_lazy_emit_add_name(cg, name);
  const char *resolved = resolve_import_alias(cg, name);
  if (resolved && *resolved && strcmp(resolved, name) != 0)
    ny_lazy_emit_add_resolved_name(cg, resolved, depth + 1);
}

static bool ny_lazy_emit_has_name(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return false;
  uint64_t hash = ny_hash64_cstr(name);
  return ny_lazy_emit_name_list_contains(&cg->lazy_emit_names,
                                         &cg->lazy_emit_hashes,
                                         cg->lazy_emit_bloom,
                                         &cg->lazy_emit_name_set, name, hash);
}

static bool ny_lazy_emit_has_name_or_qname(codegen_t *cg, const char *name,
                                           const char *cur_mod) {
  if (!cg || !name || !*name)
    return false;
  if (ny_lazy_emit_has_name(cg, name))
    return true;
  const char *leaf = ny_name_leaf(name);
  if (leaf && leaf != name && ny_lazy_emit_has_name(cg, leaf))
    return true;
  if (!cur_mod || !*cur_mod)
    return false;
  size_t mlen = strlen(cur_mod);
  if (strncmp(name, cur_mod, mlen) == 0 && name[mlen] == '.')
    return ny_lazy_emit_has_name(cg, name);
  char qname[1536];
  int n = snprintf(qname, sizeof(qname), "%s.%s", cur_mod, name);
  return n > 0 && (size_t)n < sizeof(qname) && ny_lazy_emit_has_name(cg, qname);
}

bool ny_lazy_emit_stdlib_var_needed(codegen_t *cg, stmt_t *s,
                                    const char *cur_mod) {
  if (!cg || !s || s->kind != NY_S_VAR)
    return true;
  if (!cg->lazy_emit_stdlib_enabled || !ny_is_stdlib_tok(s->tok) ||
      ny_codegen_stmt_is_source_file(cg, s) ||
      ny_codegen_module_is_source_file(cg, cur_mod))
    return true;
  if (cur_mod && strcmp(cur_mod, "std.core.syntax.type") == 0)
    return true;
  for (size_t i = 0; i < s->as.var.names.len; ++i) {
    const char *name = s->as.var.names.data[i];
    const char *tail = ny_name_leaf(name);
    if (tail && (strncmp(tail, "_TAG_", 5) == 0 ||
                 strncmp(tail, "_CORE_TAG_", 10) == 0))
      return true;
    if (ny_lazy_emit_has_name_or_qname(cg, name, cur_mod))
      return true;
  }
  return false;
}

static bool ny_lazy_emit_is_conservative_keep(const char *final_name) {
  if (!final_name || !*final_name)
    return true;
  const char *tail = ny_name_leaf(final_name);
  if (!tail || !*tail)
    return true;

  if (strncmp(final_name, "std.math.nt.__", 14) == 0 ||
      strncmp(final_name, "std.math.matrix.__", 18) == 0)
    return true;
  return false;
}

static void ny_lazy_emit_collect_stmt(codegen_t *cg, stmt_t *s);

typedef struct {
  codegen_t *cg;
  assigned_name_list locals;
  assigned_hash_list local_hashes;
  uint64_t local_bloom[4];
} ny_lazy_emit_collect_ctx_t;

static bool ny_lazy_emit_local_has(ny_lazy_emit_collect_ctx_t *ctx,
                                   const char *name) {
  if (!ctx || !name || !*name)
    return false;
  return assigned_name_contains(&ctx->locals, &ctx->local_hashes,
                                ctx->local_bloom, name);
}

static void ny_lazy_emit_local_add(ny_lazy_emit_collect_ctx_t *ctx,
                                   const char *name) {
  if (!ctx || !name || !*name)
    return;
  assigned_name_add(&ctx->locals, &ctx->local_hashes, ctx->local_bloom, name);
}

static const char *ny_lazy_emit_module_prefix(codegen_t *cg,
                                              const char *qname) {
  if (!cg || !qname || !*qname)
    return NULL;
  const char *dot = strrchr(qname, '.');
  if (!dot || dot == qname)
    return NULL;
  return arena_strndup(cg->arena, qname, (size_t)(dot - qname));
}

static bool ny_lazy_emit_expr_pre(ny_visitor_t *v, expr_t *e) {
  if (!v || !e)
    return true;
  ny_lazy_emit_collect_ctx_t *ctx = (ny_lazy_emit_collect_ctx_t *)v->ctx;
  codegen_t *cg = ctx ? ctx->cg : NULL;
  if (!cg)
    return true;
  switch (e->kind) {
  case NY_E_IDENT:
    if (ny_lazy_emit_local_has(ctx, e->as.ident.name))
      break;
    ny_lazy_emit_add_resolved_name(cg, e->as.ident.name, 0);
    if (cg->current_module_name && *cg->current_module_name &&
        e->as.ident.name && !strchr(e->as.ident.name, '.')) {
      ny_lazy_emit_add_name(
          cg, codegen_qname(cg, e->as.ident.name, cg->current_module_name));
    }
    break;
  case NY_E_MEMBER: {
    bool resolved_member = false;
    if (e->as.member.target && e->as.member.name) {
      char module_path[1024];
      char resolved_fun[1280];
      if (ny_resolve_module_expr_path(cg, NULL, 0, e->as.member.target,
                                      module_path, sizeof(module_path))) {
        if (ny_resolve_module_function_path(cg, module_path, e->as.member.name,
                                            resolved_fun,
                                            sizeof(resolved_fun))) {
          ny_lazy_emit_add_resolved_name(cg, resolved_fun, 0);
          resolved_member = true;
        }
        char resolved_global[1280];
        int gw = snprintf(resolved_global, sizeof(resolved_global), "%s.%s",
                          module_path, e->as.member.name);
        if (gw > 0 && (size_t)gw < sizeof(resolved_global)) {
          ny_lazy_emit_add_resolved_name(cg, resolved_global, 0);
          resolved_member = true;
        }
      }
    }
    if (!resolved_member)
      ny_lazy_emit_add_resolved_name(cg, e->as.member.name, 0);
    if (e->as.member.target && e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name && e->as.member.name) {
      const char *target = e->as.member.target->as.ident.name;
      for (size_t i = 0; i < cg->aliases.len; ++i) {
        binding *al = &cg->aliases.data[i];
        if (!al->name || strcmp(al->name, target) != 0)
          continue;
        const char *module_name = (const char *)al->stmt_t;
        if (!module_name || !*module_name)
          continue;
        char dotted[512];
        int nw = snprintf(dotted, sizeof(dotted), "%s.%s", module_name,
                          e->as.member.name);
        if (nw > 0 && (size_t)nw < sizeof(dotted))
          ny_lazy_emit_add_resolved_name(cg, dotted, 0);
        break;
      }
    }
    break;
  }
  case NY_E_MEMCALL: {
    bool resolved_memcall = false;
    if (e->as.memcall.target && e->as.memcall.name) {
      char module_path[1024];
      char resolved_fun[1280];
      if (ny_resolve_module_expr_path(cg, NULL, 0, e->as.memcall.target,
                                      module_path, sizeof(module_path)) &&
          ny_resolve_module_function_path(cg, module_path, e->as.memcall.name,
                                          resolved_fun, sizeof(resolved_fun))) {
        ny_lazy_emit_add_resolved_name(cg, resolved_fun, 0);
        resolved_memcall = true;
      }
    }
    if (!resolved_memcall)
      ny_lazy_emit_add_resolved_name(cg, e->as.memcall.name, 0);
    break;
  }
  case NY_E_INFERRED_MEMBER:
    ny_lazy_emit_add_resolved_name(cg, e->as.inferred_member.name, 0);
    break;
  case NY_E_LAMBDA:
  case NY_E_FN:
    ny_lazy_emit_collect_stmt(cg, e->as.lambda.body);
    break;
  default:
    break;
  }
  return true;
}

static bool ny_lazy_emit_stmt_pre(ny_visitor_t *v, stmt_t *s) {
  if (!v || !s)
    return true;
  ny_lazy_emit_collect_ctx_t *ctx = (ny_lazy_emit_collect_ctx_t *)v->ctx;
  codegen_t *cg = ctx ? ctx->cg : NULL;
  if (!cg)
    return true;
  if (s->kind == NY_S_IF) {
    bool truthy = false;
    if (ny_eval_comptime_if(cg, s, &truthy)) {
      ny_visit_stmt(v, truthy ? s->as.iff.conseq : s->as.iff.alt);
      return false;
    }
  }
  if (s->kind == NY_S_FUNC) {
    for (size_t i = 0; i < s->as.fn.params.len; ++i)
      ny_lazy_emit_local_add(ctx, s->as.fn.params.data[i].name);
  } else if (s->kind == NY_S_VAR) {
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      ny_lazy_emit_local_add(ctx, s->as.var.names.data[i]);
  } else if (s->kind == NY_S_FOR && s->as.fr.iter_var) {
    ny_lazy_emit_local_add(ctx, s->as.fr.iter_var);
    if (s->as.fr.iter_index_var)
      ny_lazy_emit_local_add(ctx, s->as.fr.iter_index_var);
  }
  if (s->kind == NY_S_GUARD && s->as.guard.type_name) {
    ny_lazy_emit_add_name(cg, s->as.guard.type_name);
  }
  return true;
}

static void ny_lazy_emit_collect_stmt(codegen_t *cg, stmt_t *s) {
  if (!cg || !s)
    return;
  ny_lazy_emit_collect_ctx_t ctx = {0};
  ctx.cg = cg;
  vec_init(&ctx.locals);
  vec_init(&ctx.local_hashes);
  ny_visitor_t vis = {0};
  vis.ctx = &ctx;
  vis.visit_expr_pre = ny_lazy_emit_expr_pre;
  vis.visit_stmt_pre = ny_lazy_emit_stmt_pre;
  ny_visit_stmt(&vis, s);
  vec_free(&ctx.locals);
  vec_free(&ctx.local_hashes);
}

static void ny_lazy_emit_collect_stmt_in_module(codegen_t *cg, stmt_t *s,
                                                const char *cur_mod) {
  if (!cg || !s)
    return;
  const char *saved_mod = cg->current_module_name;
  cg->current_module_name = cur_mod;
  ny_lazy_emit_collect_stmt(cg, s);
  cg->current_module_name = saved_mod;
}

static bool ny_lazy_emit_collect_reached_var_deps_stmt(codegen_t *cg, stmt_t *s,
                                                       const char *cur_mod) {
  if (!cg || !s)
    return false;
  bool changed = false;
  switch (s->kind) {
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      if (ny_lazy_emit_collect_reached_var_deps_stmt(
              cg, s->as.module.body.data[i], s->as.module.name))
        changed = true;
    }
    return changed;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (ny_lazy_emit_collect_reached_var_deps_stmt(
              cg, s->as.block.body.data[i], cur_mod))
        changed = true;
    }
    return changed;
  case NY_S_VAR:
    if (!ny_is_stdlib_tok(s->tok) || ny_codegen_stmt_is_source_file(cg, s) ||
        !ny_lazy_emit_stdlib_var_needed(cg, s, cur_mod))
      return false;
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      if (!name || !*name)
        continue;
      const char *qname = codegen_qname(cg, name, cur_mod);
      if (ny_lazy_emit_collected_function(cg, qname))
        continue;
      ny_lazy_emit_mark_collected_function(cg, qname);
      size_t before = cg->lazy_emit_names.len;
      ny_lazy_emit_collect_stmt_in_module(cg, s, cur_mod);
      if (ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_VARS")) {
        fprintf(stderr, "[lazy-stdlib-codegen] var_collect %s +%zu\n", qname,
                cg->lazy_emit_names.len - before);
      }
      if (cg->lazy_emit_names.len != before)
        changed = true;
      break;
    }
    return changed;
  default:
    return false;
  }
}

static bool ny_lazy_emit_collect_reached_var_deps(codegen_t *cg) {
  if (!cg || !cg->prog)
    return false;
  bool changed = false;
  for (size_t i = 0; i < cg->prog->body.len; ++i) {
    if (ny_lazy_emit_collect_reached_var_deps_stmt(cg, cg->prog->body.data[i],
                                                  NULL))
      changed = true;
  }
  for (size_t p = 0; p < cg->extra_progs.len; ++p) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; ++i) {
      if (ny_lazy_emit_collect_reached_var_deps_stmt(cg, prog->body.data[i],
                                                    NULL))
        changed = true;
    }
  }
  return changed;
}

static void ny_lazy_emit_add_function_name(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return;
  ny_lazy_emit_add_name(cg, name);
}

static bool ny_lazy_emit_function_reached(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return false;
  return ny_lazy_emit_has_name(cg, name);
}

bool ny_codegen_token_is_source_file(codegen_t *cg, token_t tok) {
  if (!cg || !cg->source_main_file || !*cg->source_main_file || !tok.filename ||
      !*tok.filename)
    return false;
  if (strcmp(cg->source_main_file, tok.filename) == 0)
    return true;
  size_t want_len = strlen(cg->source_main_file);
  size_t got_len = strlen(tok.filename);
  if (want_len < got_len &&
      strcmp(tok.filename + got_len - want_len, cg->source_main_file) == 0) {
    char sep = tok.filename[got_len - want_len - 1];
    if (sep == '/' || sep == '\\')
      return true;
  }
  if (got_len < want_len &&
      strcmp(cg->source_main_file + want_len - got_len, tok.filename) == 0) {
    char sep = cg->source_main_file[want_len - got_len - 1];
    if (sep == '/' || sep == '\\')
      return true;
  }
  return false;
}

bool ny_codegen_stmt_is_source_file(codegen_t *cg, stmt_t *s) {
  return s && ny_codegen_token_is_source_file(cg, s->tok);
}

bool ny_codegen_module_is_source_file(codegen_t *cg, const char *module_name) {
  if (!cg || !cg->source_main_file || !*cg->source_main_file ||
      !module_name || !*module_name)
    return false;
  if (strncmp(module_name, "std.", 4) != 0)
    return false;

  char rel[1024];
  size_t pos = 0;
  const char *p = module_name + 4;
  memcpy(rel, "lib/", 4);
  pos = 4;
  while (*p && pos + 4 < sizeof(rel)) {
    rel[pos++] = *p == '.' ? '/' : *p;
    p++;
  }
  if (*p || pos + 3 >= sizeof(rel))
    return false;
  memcpy(rel + pos, ".ny", 4);
  pos += 3;
  rel[pos] = '\0';

  size_t want_len = strlen(cg->source_main_file);
  size_t rel_len = strlen(rel);
  if (want_len == rel_len && strcmp(cg->source_main_file, rel) == 0)
    return true;
  if (want_len > rel_len &&
      strcmp(cg->source_main_file + want_len - rel_len, rel) == 0) {
    char sep = cg->source_main_file[want_len - rel_len - 1];
    return sep == '/' || sep == '\\';
  }
  return false;
}

bool ny_stmt_tree_has_source_file(codegen_t *cg, stmt_t *s) {
  if (!s)
    return false;
  if (ny_codegen_stmt_is_source_file(cg, s))
    return true;
  switch (s->kind) {
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++) {
      if (ny_stmt_tree_has_source_file(cg, s->as.module.body.data[i]))
        return true;
    }
    return false;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (ny_stmt_tree_has_source_file(cg, s->as.block.body.data[i]))
        return true;
    }
    return false;
  case NY_S_IF:
    return ny_stmt_tree_has_source_file(cg, s->as.iff.conseq) ||
           ny_stmt_tree_has_source_file(cg, s->as.iff.alt);
  case NY_S_GUARD:
    return ny_stmt_tree_has_source_file(cg, s->as.guard.fallback);
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; i++) {
      if (ny_stmt_tree_has_source_file(cg, s->as.impl.methods.data[i]))
        return true;
    }
    return false;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.methods.len; i++) {
      if (ny_stmt_tree_has_source_file(cg, s->as.struc.methods.data[i]))
        return true;
    }
    return false;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.methods.len; i++) {
      if (ny_stmt_tree_has_source_file(cg, s->as.layout.methods.data[i]))
        return true;
    }
    return false;
  default:
    return false;
  }
}

static bool ny_stmt_tree_is_source_context(codegen_t *cg, stmt_t *s) {
  if (!s)
    return false;
  if (ny_stmt_tree_has_source_file(cg, s))
    return true;
  return s->kind == NY_S_MODULE &&
         ny_codegen_module_is_source_file(cg, s->as.module.name);
}

static bool ny_stmt_tree_has_zero_arg_call_named(const stmt_t *s,
                                                 const char *name);

static bool ny_expr_is_zero_arg_ident_call(const expr_t *e, const char *name) {
  return e && e->kind == NY_E_CALL &&
         ny_expr_ident_is_name(e->as.call.callee, name) &&
         e->as.call.args.len == 0;
}

static bool ny_expr_tree_has_zero_arg_call_named(const expr_t *e,
                                                 const char *name) {
  if (!e || !name || !*name)
    return false;
  if (ny_expr_is_zero_arg_ident_call(e, name))
    return true;
  switch (e->kind) {
  case NY_E_UNARY:
    return ny_expr_tree_has_zero_arg_call_named(e->as.unary.right, name);
  case NY_E_BINARY:
    return ny_expr_tree_has_zero_arg_call_named(e->as.binary.left, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.binary.right, name);
  case NY_E_LOGICAL:
    return ny_expr_tree_has_zero_arg_call_named(e->as.logical.left, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.logical.right, name);
  case NY_E_TERNARY:
    return ny_expr_tree_has_zero_arg_call_named(e->as.ternary.cond, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.ternary.true_expr, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.ternary.false_expr, name);
  case NY_E_CALL:
    if (ny_expr_tree_has_zero_arg_call_named(e->as.call.callee, name))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      if (ny_expr_tree_has_zero_arg_call_named(e->as.call.args.data[i].val,
                                               name))
        return true;
    }
    return false;
  case NY_E_MEMCALL:
    if (ny_expr_tree_has_zero_arg_call_named(e->as.memcall.target, name))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      if (ny_expr_tree_has_zero_arg_call_named(e->as.memcall.args.data[i].val,
                                               name))
        return true;
    }
    return false;
  case NY_E_INDEX:
    return ny_expr_tree_has_zero_arg_call_named(e->as.index.target, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.index.start, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.index.stop, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.index.step, name);
  case NY_E_LAMBDA:
  case NY_E_FN:
    return false;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      if (ny_expr_tree_has_zero_arg_call_named(e->as.list_like.data[i], name))
        return true;
    }
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      dict_pair_t pair = e->as.dict.pairs.data[i];
      if (ny_expr_tree_has_zero_arg_call_named(pair.key, name) ||
          ny_expr_tree_has_zero_arg_call_named(pair.value, name))
        return true;
    }
    return false;
  case NY_E_COMPTIME:
    return ny_stmt_tree_has_zero_arg_call_named(e->as.comptime_expr.body,
                                                name);
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t part = e->as.fstring.parts.data[i];
      if (part.kind == NY_FSP_EXPR &&
          ny_expr_tree_has_zero_arg_call_named(part.as.e, name))
        return true;
    }
    return false;
  case NY_E_MATCH:
    return ny_expr_tree_has_zero_arg_call_named(e->as.match.test, name);
  case NY_E_MEMBER:
    return ny_expr_tree_has_zero_arg_call_named(e->as.member.target, name);
  case NY_E_PTR_TYPE:
    return ny_expr_tree_has_zero_arg_call_named(e->as.ptr_type.target, name);
  case NY_E_DEREF:
    return ny_expr_tree_has_zero_arg_call_named(e->as.deref.target, name);
  case NY_E_SIZEOF:
    return ny_expr_tree_has_zero_arg_call_named(e->as.szof.target, name);
  case NY_E_TRY:
    return ny_expr_tree_has_zero_arg_call_named(e->as.try_expr.target, name);
  default:
    return false;
  }
}

static bool ny_stmt_tree_has_zero_arg_call_named(const stmt_t *s,
                                                 const char *name) {
  if (!s || !name || !*name)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (ny_stmt_tree_has_zero_arg_call_named(s->as.block.body.data[i], name))
        return true;
    }
    return false;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; i++) {
      if (ny_expr_tree_has_zero_arg_call_named(s->as.var.exprs.data[i], name))
        return true;
    }
    return false;
  case NY_S_EXPR:
    return ny_expr_tree_has_zero_arg_call_named(s->as.expr.expr, name);
  case NY_S_IF:
    return ny_expr_tree_has_zero_arg_call_named(s->as.iff.test, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.iff.init, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.iff.conseq, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.iff.alt, name);
  case NY_S_GUARD:
    return ny_expr_tree_has_zero_arg_call_named(s->as.guard.value, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.guard.fallback, name);
  case NY_S_WHILE:
    return ny_stmt_tree_has_zero_arg_call_named(s->as.whl.init, name) ||
           ny_expr_tree_has_zero_arg_call_named(s->as.whl.test, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.whl.update, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.whl.body, name);
  case NY_S_FOR:
    return ny_stmt_tree_has_zero_arg_call_named(s->as.fr.init, name) ||
           ny_expr_tree_has_zero_arg_call_named(s->as.fr.cond, name) ||
           ny_expr_tree_has_zero_arg_call_named(s->as.fr.iterable, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.fr.update, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.fr.body, name);
  case NY_S_TRY:
    return ny_stmt_tree_has_zero_arg_call_named(s->as.tr.body, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.tr.handler, name);
  case NY_S_RETURN:
    return ny_expr_tree_has_zero_arg_call_named(s->as.ret.value, name);
  case NY_S_DEFER:
    return ny_stmt_tree_has_zero_arg_call_named(s->as.de.body, name);
  case NY_S_MATCH:
    return ny_expr_tree_has_zero_arg_call_named(s->as.match.test, name);
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++) {
      if (ny_stmt_tree_has_zero_arg_call_named(s->as.module.body.data[i], name))
        return true;
    }
    return false;
  default:
    return false;
  }
}

static bool ny_stmt_has_main_guard(const stmt_t *s) {
  if (!s)
    return false;
  if (s->kind == NY_S_IF &&
      ny_expr_tree_has_zero_arg_call_named(s->as.iff.test, "__main"))
    return true;
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (ny_stmt_has_main_guard(s->as.block.body.data[i]))
        return true;
    }
  }
  return false;
}

bool ny_program_has_explicit_main_entry(codegen_t *cg, program_t *prog) {
  if (!prog)
    return false;
  for (size_t i = 0; i < prog->body.len; i++) {
    stmt_t *s = prog->body.data[i];
    if (!s)
      continue;
    if (cg && cg->source_main_file && *cg->source_main_file) {
      if (!ny_stmt_tree_is_source_context(cg, s))
        continue;
    } else if (ny_is_stdlib_tok(s->tok)) {
      continue;
    }
    if (s->kind == NY_S_EXPR &&
        ny_expr_is_zero_arg_ident_call(s->as.expr.expr, "main"))
      return true;
    if (ny_stmt_has_main_guard(s))
      return true;
  }
  return false;
}

static bool ny_lazy_emit_treat_as_root(codegen_t *cg, stmt_t *s,
                                       const char *cur_mod) {
  return s &&
         (!ny_is_stdlib_tok(s->tok) || ny_codegen_stmt_is_source_file(cg, s) ||
          ny_codegen_module_is_source_file(cg, cur_mod));
}

static void ny_lazy_emit_seed_stmt(codegen_t *cg, stmt_t *s,
                                   const char *cur_mod) {
  if (!cg || !s)
    return;
  switch (s->kind) {
  case NY_S_FUNC: {
    const char *final_name = codegen_qname(cg, s->as.fn.name, cur_mod);
    if (ny_lazy_emit_treat_as_root(cg, s, cur_mod)) {
      ny_lazy_emit_add_function_name(cg, final_name);
      if (!ny_lazy_emit_collected_function(cg, final_name)) {
        ny_lazy_emit_mark_collected_function(cg, final_name);
        ny_lazy_emit_collect_stmt_in_module(cg, s, cur_mod);
      }
    }
    return;
  }
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; i++)
      ny_lazy_emit_seed_stmt(cg, s->as.impl.methods.data[i], cur_mod);
    return;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.methods.len; i++)
      ny_lazy_emit_seed_stmt(cg, s->as.struc.methods.data[i], cur_mod);
    return;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.methods.len; i++)
      ny_lazy_emit_seed_stmt(cg, s->as.layout.methods.data[i], cur_mod);
    return;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++)
      ny_lazy_emit_seed_stmt(cg, s->as.module.body.data[i], s->as.module.name);
    return;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      ny_lazy_emit_seed_stmt(cg, s->as.block.body.data[i], cur_mod);
    if (ny_lazy_emit_treat_as_root(cg, s, cur_mod))
      ny_lazy_emit_collect_stmt(cg, s);
    return;
  default:
    if (ny_lazy_emit_treat_as_root(cg, s, cur_mod))
      ny_lazy_emit_collect_stmt(cg, s);
    return;
  }
}

static void ny_lazy_emit_build_reachable_set(codegen_t *cg) {
  if (!cg || !cg->prog)
    return;
  if (!cg->comptime && (cg->is_repl || cg->emit_module_decls_only))
    return;
  if (cg->skip_stdlib && !cg->emit_cached_stdlib_init)
    return;
  if (!ny_env_enabled_default_on("NYTRIX_LAZY_STDLIB_CODEGEN") &&
      !ny_env_enabled("NYTRIX_UNSAFE_LAZY_STDLIB_CODEGEN"))
    return;
  if (!cg->comptime && (!cg->source_main_file || !*cg->source_main_file))
    return;
  bool trace_lazy = ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_CODEGEN");
  ny_tick_t t_lazy = trace_lazy ? ny_ticks_now() : 0;
  cg->lazy_emit_stdlib_enabled = true;
  if (!cg->lazy_emit_stdlib_enabled)
    return;
  if (trace_lazy) {
    fprintf(stderr,
            "[lazy-stdlib-codegen] begin body=%zu extra=%zu fun_sigs=%zu\n",
            cg->prog->body.len, cg->extra_progs.len, cg->fun_sigs.len);
  }
  ny_lazy_emit_add_name(cg, "main");
  ny_lazy_emit_add_name(cg, "_ny_top_entry");
  for (size_t i = 0; i < cg->prog->body.len; i++)
    ny_lazy_emit_seed_stmt(cg, cg->prog->body.data[i], NULL);
  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; i++)
      ny_lazy_emit_seed_stmt(cg, prog->body.data[i], NULL);
  }
  if (trace_lazy) {
    fprintf(stderr,
            "[lazy-stdlib-codegen] seed names=%zu collected=%zu fun_sigs=%zu "
            "elapsed=%.4fs\n",
            cg->lazy_emit_names.len, cg->lazy_emit_collected_names.len,
            cg->fun_sigs.len, ny_ticks_elapsed_sec(t_lazy));
  }

  bool changed = true;
  size_t guard = 0;
  while (changed && guard++ < 64) {
    changed = false;
    size_t before = cg->lazy_emit_names.len;
    size_t collected_before = cg->lazy_emit_collected_names.len;
    ny_tick_t t_round = trace_lazy ? ny_ticks_now() : 0;
    for (size_t i = 0; i < cg->fun_sigs.len; i++) {
      fun_sig *sig = &cg->fun_sigs.data[i];
      if (!sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC || !sig->name)
        continue;
      if (!ny_lazy_emit_function_reached(cg, sig->name))
        continue;
      if (!ny_lazy_emit_collected_function(cg, sig->name)) {
        const char *sig_mod = sig->module_name && *sig->module_name
                                  ? sig->module_name
                                  : ny_lazy_emit_module_prefix(cg, sig->name);
        ny_lazy_emit_mark_collected_function(cg, sig->name);
        ny_lazy_emit_collect_stmt_in_module(cg, sig->stmt_t, sig_mod);
      }
      ny_lazy_emit_add_function_name(cg, sig->name);
    }
    if (ny_lazy_emit_collect_reached_var_deps(cg))
      changed = true;
    changed = changed || cg->lazy_emit_names.len != before;
    if (trace_lazy) {
      fprintf(stderr,
              "[lazy-stdlib-codegen] round=%zu names=%zu +%zu collected=%zu "
              "+%zu elapsed=%.4fs\n",
              guard, cg->lazy_emit_names.len, cg->lazy_emit_names.len - before,
              cg->lazy_emit_collected_names.len,
              cg->lazy_emit_collected_names.len - collected_before,
              ny_ticks_elapsed_sec(t_round));
    }
  }
  if (trace_lazy) {
    fprintf(
        stderr,
        "[lazy-stdlib-codegen] reachable_names=%zu collected=%zu total=%.4fs\n",
        cg->lazy_emit_names.len, cg->lazy_emit_collected_names.len,
        ny_ticks_elapsed_sec(t_lazy));
  }
}

void ny_lazy_emit_prepare_reachable(codegen_t *cg) {
  ny_lazy_emit_build_reachable_set(cg);
}

static bool ny_lazy_emit_should_emit_func(codegen_t *cg, stmt_t *s,
                                          const char *final_name) {
  if (!cg || !cg->lazy_emit_stdlib_enabled || !s || !ny_is_stdlib_tok(s->tok))
    return true;
  if (ny_codegen_stmt_is_source_file(cg, s))
    return true;
  if (ny_lazy_emit_is_conservative_keep(final_name))
    return true;
  return ny_lazy_emit_function_reached(cg, final_name);
}

void emit_top_functions(codegen_t *cg, stmt_t *s, scope *gsc, size_t gd,
                        const char *cur_mod) {
  if (s->kind == NY_S_FUNC) {
    if (!ny_emit_module_match(cg, cur_mod))
      return;
    cg->current_module_name = cur_mod;
    const char *final_name = codegen_qname(cg, s->as.fn.name, cur_mod);
    if (!ny_lazy_emit_should_emit_func(cg, s, final_name))
      return;
    if (cg->lazy_emit_stdlib_enabled &&
        ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_EMIT") &&
        ny_is_stdlib_tok(s->tok)) {
      fprintf(stderr, "[lazy-stdlib-codegen] emit_top %s\n",
              final_name ? final_name : "(nil)");
    }
    gen_func(cg, s, final_name, gsc, gd, NULL);
  } else if (s->kind == NY_S_IMPL) {
    for (size_t i = 0; i < s->as.impl.methods.len; i++)
      emit_top_functions(cg, s->as.impl.methods.data[i], gsc, gd, cur_mod);
  } else if (s->kind == NY_S_STRUCT) {
    for (size_t i = 0; i < s->as.struc.methods.len; i++)
      emit_top_functions(cg, s->as.struc.methods.data[i], gsc, gd, cur_mod);
  } else if (s->kind == NY_S_LAYOUT) {
    for (size_t i = 0; i < s->as.layout.methods.len; i++)
      emit_top_functions(cg, s->as.layout.methods.data[i], gsc, gd, cur_mod);
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      emit_top_functions(cg, s->as.module.body.data[i], gsc, gd,
                         s->as.module.name);
  } else if (s->kind == NY_S_BLOCK && s->as.block.transparent) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      emit_top_functions(cg, s->as.block.body.data[i], gsc, gd, cur_mod);
  }
}

static bool ny_fn_has_body(LLVMValueRef fn) {
  return fn && LLVMGetFirstBasicBlock(fn) != NULL;
}

static bool ny_emit_referenced_function_declarations(codegen_t *cg, scope *gsc,
                                                     size_t gd) {
  if (!cg || !cg->lazy_emit_stdlib_enabled)
    return false;
  bool emitted = false;
  bool trace_uses = ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_USES");
  size_t trace_limit = 80;
  const char *trace_limit_env = getenv("NYTRIX_TRACE_LAZY_STDLIB_USE_LIMIT");
  if (trace_limit_env && *trace_limit_env) {
    long parsed = strtol(trace_limit_env, NULL, 10);
    if (parsed > 0)
      trace_limit = (size_t)parsed;
  }
  static size_t trace_use_count = 0;
  for (size_t i = 0; i < cg->fun_sigs.len; i++) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig || sig->is_extern || !sig->stmt_t ||
        sig->stmt_t->kind != NY_S_FUNC || !sig->value)
      continue;
    if (ny_fn_has_body(sig->value))
      continue;
    if (!LLVMGetFirstUse(sig->value))
      continue;
    if (trace_uses && trace_use_count < trace_limit) {
      LLVMUseRef use = LLVMGetFirstUse(sig->value);
      LLVMValueRef user = use ? LLVMGetUser(use) : NULL;
      char *printed = user ? LLVMPrintValueToString(user) : NULL;
      fprintf(stderr,
              "[lazy-stdlib-codegen] demand_use %s llvm=%s user=%s\n",
              sig->name ? sig->name : "(nil)",
              sig->value ? LLVMGetValueName(sig->value) : "(nil)",
              printed ? printed : "(nil)");
      if (printed)
        LLVMDisposeMessage(printed);
      trace_use_count++;
    }
    const char *saved_mod = cg->current_module_name;
    const char *sig_mod = sig->module_name && *sig->module_name
                              ? sig->module_name
                              : ny_lazy_emit_module_prefix(cg, sig->name);
    if (!ny_lazy_emit_collected_function(cg, sig->name)) {
      ny_lazy_emit_mark_collected_function(cg, sig->name);
      ny_lazy_emit_collect_stmt_in_module(cg, sig->stmt_t, sig_mod);
      ny_lazy_emit_add_function_name(cg, sig->name);
      ny_lazy_emit_collect_reached_var_deps(cg);
      if (ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_CODEGEN")) {
        fprintf(stderr, "[lazy-stdlib-codegen] demand_collect %s\n",
                sig->name);
      }
    }
    cg->current_module_name = sig_mod;
    gen_func(cg, sig->stmt_t, sig->name, gsc, gd, NULL);
    cg->current_module_name = saved_mod;
    emitted = true;
  }
  return emitted;
}

void ny_lazy_emit_demand_referenced(codegen_t *cg, scope *gsc, size_t gd,
                                    const char *phase) {
  if (!cg || !cg->lazy_emit_stdlib_enabled)
    return;
  LLVMBasicBlockRef saved_block = ny_cur_block(cg);
  size_t rounds = 0;
  while (rounds++ < 64 &&
         ny_emit_referenced_function_declarations(cg, gsc, gd)) {
  }
  if (saved_block)
    ny_pos(cg, saved_block);
  if (ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_CODEGEN")) {
    fprintf(stderr, "[lazy-stdlib-codegen] %s_demand_rounds=%zu\n",
            phase && *phase ? phase : "unknown", rounds > 0 ? rounds - 1 : 0);
  }
}

static bool ny_stmt_contains_top_function(stmt_t *s) {
  if (!s)
    return false;
  switch (s->kind) {
  case NY_S_FUNC:
    return true;
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; i++) {
      if (ny_stmt_contains_top_function(s->as.impl.methods.data[i]))
        return true;
    }
    return false;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.methods.len; i++) {
      if (ny_stmt_contains_top_function(s->as.struc.methods.data[i]))
        return true;
    }
    return false;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.methods.len; i++) {
      if (ny_stmt_contains_top_function(s->as.layout.methods.data[i]))
        return true;
    }
    return false;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++) {
      if (ny_stmt_contains_top_function(s->as.module.body.data[i]))
        return true;
    }
    return false;
  case NY_S_BLOCK:
    if (!s->as.block.transparent)
      return false;
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (ny_stmt_contains_top_function(s->as.block.body.data[i]))
        return true;
    }
    return false;
  default:
    return false;
  }
}

static bool ny_user_use_has(codegen_t *cg, const char *mod) {
  if (!cg || !mod || !*mod)
    return false;
  for (size_t i = 0; i < cg->user_use_modules.len; i++) {
    const char *m = cg->user_use_modules.data[i];
    if (m && strcmp(m, mod) == 0)
      return true;
  }
  return false;
}

static bool ny_link_allowed_has(codegen_t *cg, const char *mod) {
  if (!cg || !mod || !*mod)
    return false;
  for (size_t i = 0; i < cg->link_allowed_modules.len; i++) {
    const char *m = cg->link_allowed_modules.data[i];
    if (m && strcmp(m, mod) == 0)
      return true;
  }
  return false;
}

static void ny_collect_use_modules_stmt(stmt_t *s, str_list *out) {
  if (!s || !out)
    return;
  if (s->kind == NY_S_USE) {
    if (ny_stmt_is_bare_std_use(s)) {
      vec_push(out, (char *)"std.core");
      vec_push(out, (char *)"std.os.prim");
    } else if (s->as.use.module) {
      vec_push(out, (char *)s->as.use.module);
    }
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      ny_collect_use_modules_stmt(s->as.module.body.data[i], out);
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      ny_collect_use_modules_stmt(s->as.block.body.data[i], out);
  } else if (s->kind == NY_S_IF) {
    if (s->as.iff.conseq)
      ny_collect_use_modules_stmt(s->as.iff.conseq, out);
    if (s->as.iff.alt)
      ny_collect_use_modules_stmt(s->as.iff.alt, out);
  } else if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body)
      ny_collect_use_modules_stmt(s->as.whl.body, out);
    if (s->as.whl.update)
      ny_collect_use_modules_stmt(s->as.whl.update, out);
    if (s->as.whl.init)
      ny_collect_use_modules_stmt(s->as.whl.init, out);
  } else if (s->kind == NY_S_FOR) {
    if (s->as.fr.init)
      ny_collect_use_modules_stmt(s->as.fr.init, out);
    if (s->as.fr.body)
      ny_collect_use_modules_stmt(s->as.fr.body, out);
    if (s->as.fr.update)
      ny_collect_use_modules_stmt(s->as.fr.update, out);
  } else if (s->kind == NY_S_TRY) {
    if (s->as.tr.body)
      ny_collect_use_modules_stmt(s->as.tr.body, out);
    if (s->as.tr.handler)
      ny_collect_use_modules_stmt(s->as.tr.handler, out);
  } else if (s->kind == NY_S_DEFER) {
    if (s->as.de.body)
      ny_collect_use_modules_stmt(s->as.de.body, out);
  } else if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      if (s->as.match.arms.data[i].conseq)
        ny_collect_use_modules_stmt(s->as.match.arms.data[i].conseq, out);
    }
    if (s->as.match.default_conseq)
      ny_collect_use_modules_stmt(s->as.match.default_conseq, out);
  }
}

static void ny_collect_module_names_stmt(stmt_t *s, str_list *out) {
  if (!s || !out)
    return;
  if (s->kind == NY_S_MODULE) {
    if (s->as.module.name && *s->as.module.name)
      vec_push(out, (char *)s->as.module.name);
    for (size_t i = 0; i < s->as.module.body.len; i++)
      ny_collect_module_names_stmt(s->as.module.body.data[i], out);
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      ny_collect_module_names_stmt(s->as.block.body.data[i], out);
  } else if (s->kind == NY_S_IF) {
    if (s->as.iff.conseq)
      ny_collect_module_names_stmt(s->as.iff.conseq, out);
    if (s->as.iff.alt)
      ny_collect_module_names_stmt(s->as.iff.alt, out);
  } else if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body)
      ny_collect_module_names_stmt(s->as.whl.body, out);
    if (s->as.whl.update)
      ny_collect_module_names_stmt(s->as.whl.update, out);
    if (s->as.whl.init)
      ny_collect_module_names_stmt(s->as.whl.init, out);
  } else if (s->kind == NY_S_FOR) {
    if (s->as.fr.init)
      ny_collect_module_names_stmt(s->as.fr.init, out);
    if (s->as.fr.body)
      ny_collect_module_names_stmt(s->as.fr.body, out);
    if (s->as.fr.update)
      ny_collect_module_names_stmt(s->as.fr.update, out);
  } else if (s->kind == NY_S_TRY) {
    if (s->as.tr.body)
      ny_collect_module_names_stmt(s->as.tr.body, out);
    if (s->as.tr.handler)
      ny_collect_module_names_stmt(s->as.tr.handler, out);
  } else if (s->kind == NY_S_DEFER) {
    if (s->as.de.body)
      ny_collect_module_names_stmt(s->as.de.body, out);
  } else if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      if (s->as.match.arms.data[i].conseq)
        ny_collect_module_names_stmt(s->as.match.arms.data[i].conseq, out);
    }
    if (s->as.match.default_conseq)
      ny_collect_module_names_stmt(s->as.match.default_conseq, out);
  }
}

static void ny_collect_module_names_prog(program_t *prog, str_list *out) {
  if (!prog || !out)
    return;
  for (size_t i = 0; i < prog->body.len; i++)
    ny_collect_module_names_stmt(prog->body.data[i], out);
}

static stmt_t *ny_find_module_stmt_any(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return NULL;
  if (cg->prog) {
    for (size_t i = 0; i < cg->prog->body.len; i++) {
      stmt_t *m = find_module_stmt(cg->prog->body.data[i], name);
      if (m)
        return m;
    }
  }
  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; i++) {
      stmt_t *m = find_module_stmt(prog->body.data[i], name);
      if (m)
        return m;
    }
  }
  return NULL;
}

static void ny_build_link_allowed_modules(codegen_t *cg) {
  if (!cg)
    return;
  for (size_t i = 0; i < cg->link_allowed_modules.len; i++)
    free(cg->link_allowed_modules.data[i]);
  cg->link_allowed_modules.len = 0;

  VEC(const char *) queue;
  vec_init(&queue);
  for (size_t i = 0; i < cg->user_use_modules.len; i++) {
    const char *m = cg->user_use_modules.data[i];
    if (m && *m)
      vec_push(&queue, m);
  }
  if (queue.len == 0 && cg->current_module_name &&
      strncmp(cg->current_module_name, "std.", 4) == 0) {
    vec_push(&queue, cg->current_module_name);
  }
  if (queue.len == 0) {
    str_list mods = {0};
    ny_collect_module_names_prog(cg->prog, &mods);
    for (size_t p = 0; p < cg->extra_progs.len; p++) {
      program_t *prog = cg->extra_progs.data[p];
      ny_collect_module_names_prog(prog, &mods);
    }
    for (size_t i = 0; i < mods.len; i++) {
      const char *m = mods.data[i];
      if (m && *m)
        vec_push(&queue, m);
    }
    vec_free(&mods);
  }
  while (queue.len > 0) {
    const char *mod = queue.data[queue.len - 1];
    queue.len--;
    if (!mod || !*mod)
      continue;
    if (ny_link_allowed_has(cg, mod))
      continue;
    vec_push(&cg->link_allowed_modules, ny_strdup(mod));
    stmt_t *mstmt = ny_find_module_stmt_any(cg, mod);
    if (!mstmt || mstmt->kind != NY_S_MODULE)
      continue;
    str_list deps = {0};
    for (size_t i = 0; i < mstmt->as.module.body.len; i++)
      ny_collect_use_modules_stmt(mstmt->as.module.body.data[i], &deps);
    for (size_t i = 0; i < deps.len; i++) {
      const char *dep = deps.data[i];
      if (dep && *dep)
        vec_push(&queue, dep);
    }
    vec_free(&deps);
  }
  vec_free(&queue);
}

static bool ny_link_allowed_for_module(codegen_t *cg, const char *mod) {
  if (ny_env_enabled("NYTRIX_LINK_ALLOW_ALL"))
    return true;
  if (!mod || !*mod)
    return true;
  if (strncmp(mod, "std.", 4) == 0)
    return true;
  if (strncmp(mod, "lib.", 4) != 0)
    return true;
  if (cg->link_allowed_modules.len == 0 && !cg->current_module_name)
    return true;
  if (cg->link_allowed_modules.len == 0 && cg->current_module_name &&
      strncmp(cg->current_module_name, "std.", 4) == 0)
    return true;
  if (cg->link_allowed_modules.len == 0 && cg->current_module_name &&
      strcmp(cg->current_module_name, mod) == 0)
    return true;
  if (cg->link_allowed_modules.len == 0 && ny_user_use_has(cg, mod))
    return true;
  return ny_link_allowed_has(cg, mod);
}

static void process_links(codegen_t *cg, stmt_t *s, const char *cur_mod) {
  if (s->kind == NY_S_LINK) {
    if (!ny_link_allowed_for_module(cg, cur_mod))
      return;
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
      process_links(cg, s->as.module.body.data[i], s->as.module.name);
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      process_links(cg, s->as.block.body.data[i], cur_mod);
  } else if (s->kind == NY_S_IF) {
    bool truthy = false;
    if (ny_eval_comptime_if(cg, s, &truthy)) {
      if (truthy) {
        if (s->as.iff.conseq)
          process_links(cg, s->as.iff.conseq, cur_mod);
      } else if (s->as.iff.alt) {
        process_links(cg, s->as.iff.alt, cur_mod);
      }
    } else {
      if (s->as.iff.conseq)
        process_links(cg, s->as.iff.conseq, cur_mod);
      if (s->as.iff.alt)
        process_links(cg, s->as.iff.alt, cur_mod);
    }
  } else if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body)
      process_links(cg, s->as.whl.body, cur_mod);
    if (s->as.whl.update)
      process_links(cg, s->as.whl.update, cur_mod);
    if (s->as.whl.init)
      process_links(cg, s->as.whl.init, cur_mod);
  } else if (s->kind == NY_S_FOR) {
    if (s->as.fr.init)
      process_links(cg, s->as.fr.init, cur_mod);
    if (s->as.fr.body)
      process_links(cg, s->as.fr.body, cur_mod);
    if (s->as.fr.update)
      process_links(cg, s->as.fr.update, cur_mod);
  } else if (s->kind == NY_S_TRY) {
    if (s->as.tr.body)
      process_links(cg, s->as.tr.body, cur_mod);
    if (s->as.tr.handler)
      process_links(cg, s->as.tr.handler, cur_mod);
  } else if (s->kind == NY_S_DEFER) {
    if (s->as.de.body)
      process_links(cg, s->as.de.body, cur_mod);
  } else if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      if (s->as.match.arms.data[i].conseq)
        process_links(cg, s->as.match.arms.data[i].conseq, cur_mod);
    }
    if (s->as.match.default_conseq)
      process_links(cg, s->as.match.default_conseq, cur_mod);
  }
}

void codegen_collect_links(codegen_t *cg, program_t *prog) {
  if (!cg || !prog)
    return;
  NY_COMPILER_ASSERT(
      prog->body.len <= prog->body.cap,
      "codegen_collect_links program body vector len exceeds cap");
  NY_COMPILER_ASSERT(
      prog->body.data != NULL || prog->body.len == 0,
      "codegen_collect_links program body vector has len but no data");
  NY_COMPILER_ASSERT(
      cg->extra_progs.len <= cg->extra_progs.cap,
      "codegen_collect_links extra_progs vector len exceeds cap");
  NY_COMPILER_ASSERT(
      cg->extra_progs.data != NULL || cg->extra_progs.len == 0,
      "codegen_collect_links extra_progs vector has len but no data");
  if (cg->use_modules.len == 0 && cg->user_use_modules.len == 0) {
    for (size_t i = 0; i < prog->body.len; i++) {
      stmt_t *s = prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL, "null collect-links use stmt at index %zu",
                          i);
      if (!s)
        continue;
      collect_use_modules(cg, s);
    }
  }
  ny_build_link_allowed_modules(cg);
  for (size_t i = 0; i < prog->body.len; i++) {
    stmt_t *s = prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL,
                        "null collect-links process stmt at index %zu", i);
    if (!s)
      continue;
    process_links(cg, s, NULL);
  }

  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *eprog = cg->extra_progs.data[p];
    if (!eprog)
      continue;
    NY_COMPILER_ASSERTF(
        eprog->body.len <= eprog->body.cap,
        "extra program %zu body vector len exceeds cap during link collection",
        p);
    NY_COMPILER_ASSERTF(eprog->body.data != NULL || eprog->body.len == 0,
                        "extra program %zu body vector has len but no data "
                        "during link collection",
                        p);
    for (size_t i = 0; i < eprog->body.len; i++) {
      stmt_t *s = eprog->body.data[i];
      NY_COMPILER_ASSERTF(
          s != NULL, "null extra collect-links stmt p=%zu index=%zu", p, i);
      if (!s)
        continue;
      process_links(cg, s, NULL);
    }
  }
}

void codegen_prepare(codegen_t *cg) {
  if (!cg || !cg->prog || cg->is_preparing)
    return;

  cg->is_preparing = true;
  NY_COMPILER_ASSERT(cg->module != NULL, "codegen_prepare missing LLVM module");
  NY_COMPILER_ASSERT(cg->builder != NULL,
                     "codegen_prepare missing LLVM builder");
  NY_COMPILER_ASSERT(cg->prog->body.len <= cg->prog->body.cap,
                     "codegen_prepare program body vector len exceeds cap");
  NY_COMPILER_ASSERT(cg->prog->body.data != NULL || cg->prog->body.len == 0,
                     "codegen_prepare program body vector has len but no data");
  NY_COMPILER_ASSERT(cg->extra_progs.len <= cg->extra_progs.cap,
                     "codegen_prepare extra_progs vector len exceeds cap");
  NY_COMPILER_ASSERT(cg->extra_progs.data != NULL || cg->extra_progs.len == 0,
                     "codegen_prepare extra_progs vector has len but no data");

  if (cg->debug_symbols) {
    stmt_t *first_stmt =
        (cg->prog->body.len > 0) ? cg->prog->body.data[0] : NULL;
    NY_COMPILER_ASSERT(first_stmt != NULL || cg->prog->body.len == 0,
                       "codegen_prepare first top-level statement is null");
    const char *main_file =
        (cg->debug_main_file && *cg->debug_main_file)
            ? cg->debug_main_file
            : (first_stmt ? first_stmt->tok.filename : "<inline>");

    bool inline_source = !main_file || !*main_file || main_file[0] == '<' ||
                         strcmp(main_file, "-") == 0;
    if (inline_source && cg->user_source && cg->user_source_len > 0) {
      char inline_file[PATH_MAX];
      char inline_name[64];
      snprintf(inline_name, sizeof(inline_name), "ny_inline_%ld.ny",
               (long)getpid());
      ny_join_path(inline_file, sizeof(inline_file), ny_get_temp_dir(),
                   inline_name);
      FILE *f = fopen(inline_file, "w");
      if (f) {
        fwrite(cg->user_source, 1, cg->user_source_len, f);
        fclose(f);

        static char abs_inline[4096];
        if (ny_realpath(inline_file, abs_inline)) {
          main_file = abs_inline;
        } else {
          main_file = inline_file;
        }

        size_t start = cg->prog->body.len > 20 ? cg->prog->body.len - 20 : 0;
        for (size_t i = start; i < cg->prog->body.len; i++) {
          stmt_t *s = cg->prog->body.data[i];
          NY_COMPILER_ASSERTF(s != NULL,
                              "null debug inline-source stmt at index %zu", i);
          if (!s)
            continue;
          const char *fn = s->tok.filename;
          if (fn) {
            s->tok.filename = main_file;
          }
        }
      }
    }
    codegen_debug_init(cg, main_file);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null top-level statement at index %zu", i);
    if (!s)
      continue;
    collect_use_aliases(cg, s);
    collect_use_modules(cg, s);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null impl registration stmt at index %zu",
                        i);
    if (!s)
      continue;
    ny_register_impl_types_stmt(cg, s);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL,
                        "null signature collection stmt at index %zu", i);
    if (!s)
      continue;
    collect_sigs(cg, s);
  }

  process_default_core_imports(cg);

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null import processing stmt at index %zu",
                        i);
    if (!s)
      continue;
    process_use_imports(cg, s);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null export processing stmt at index %zu",
                        i);
    if (!s)
      continue;
    process_exports(cg, s);
  }

  ny_build_link_allowed_modules(cg);
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null link processing stmt at index %zu", i);
    if (!s)
      continue;
    process_links(cg, s, NULL);
  }
  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    NY_COMPILER_ASSERTF(prog->body.len <= prog->body.cap,
                        "extra program %zu body vector len exceeds cap", p);
    NY_COMPILER_ASSERTF(prog->body.data != NULL || prog->body.len == 0,
                        "extra program %zu body vector has len but no data", p);
    for (size_t i = 0; i < prog->body.len; i++) {
      stmt_t *s = prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL,
                          "null extra-program link stmt p=%zu index=%zu", p, i);
      if (!s)
        continue;
      process_links(cg, s, NULL);
    }
  }

  infer_pure_functions(cg);

  cg->is_preparing = false;
}

void codegen_repopulate_interns(codegen_t *cg) {
  if (!cg || !cg->module)
    return;

  for (LLVMValueRef g = LLVMGetFirstGlobal(cg->module); g;
       g = LLVMGetNextGlobal(g)) {
    const char *name = LLVMGetValueName(g);
    if (!name)
      continue;

    if (strncmp(name, ".str.runtime.", 13) == 0) {
      const char *suffix = name + 13;
      char data_name[256];
      snprintf(data_name, sizeof(data_name), ".str.data.%s", suffix);
      LLVMValueRef dg = ny_get_global(cg, data_name);
      if (!dg) {
        const char *dot = strchr(suffix, '.');
        if (dot && dot > suffix) {
          size_t base_len = (size_t)(dot - suffix);
          if (base_len < sizeof(data_name) - sizeof(".str.data.")) {
            snprintf(data_name, sizeof(data_name), ".str.data.%.*s",
                     (int)base_len, suffix);
            dg = ny_get_global(cg, data_name);
          }
        }
      }
      if (dg) {
        bool exists = false;
        for (size_t i = 0; i < cg->interns.len; i++) {
          string_intern *old = &cg->interns.data[i];
          if (old->val == g) {
            exists = true;
            break;
          }
        }
        if (exists)
          continue;
        string_intern in = {0};
        in.gv = dg;
        in.val = g;
        in.module = cg->module;
        vec_push(&cg->interns, in);
      }
    }
  }
}

void codegen_rebind_llvm_symbols(codegen_t *cg) {
  if (!cg || !cg->module)
    return;
  for (size_t i = 0; i < cg->fun_sigs.len; i++) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    const char *link_name =
        (sig->link_name && *sig->link_name) ? sig->link_name : sig->name;
    if (!link_name || !*link_name)
      continue;
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, link_name);
    if (!fn)
      continue;
    sig->value = fn;
    sig->type = LLVMGlobalGetValueType(fn);
  }
  for (size_t i = 0; i < cg->global_vars.len; i++) {
    binding *b = &cg->global_vars.data[i];
    if (!b->name || !*b->name)
      continue;
    LLVMValueRef gv = LLVMGetNamedGlobal(cg->module, b->name);
    if (gv)
      b->value = gv;
  }
}

static LLVMValueRef ny_const_string_runtime_initializer(
    codegen_t *cg, LLVMValueRef str_array_global,
    LLVMValueRef runtime_ptr_global, LLVMTypeRef i8_ty) {
  if (!cg || !str_array_global || !runtime_ptr_global)
    return NULL;
  LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, false)};
  LLVMValueRef str_data_ptr =
      LLVMConstInBoundsGEP2(i8_ty, str_array_global, indices, 1);
  LLVMTypeRef value_ty = LLVMGlobalGetValueType(runtime_ptr_global);
  LLVMTypeKind value_kind = LLVMGetTypeKind(value_ty);
  if (value_kind == LLVMPointerTypeKind)
    return LLVMConstPointerCast(str_data_ptr, value_ty);
  if (value_kind == LLVMIntegerTypeKind)
    return LLVMConstPtrToInt(str_data_ptr, value_ty);
  return NULL;
}

static bool ny_set_const_string_runtime_initializer(
    codegen_t *cg, LLVMValueRef str_array_global,
    LLVMValueRef runtime_ptr_global, LLVMTypeRef i8_ty) {
  LLVMValueRef init = ny_const_string_runtime_initializer(
      cg, str_array_global, runtime_ptr_global, i8_ty);
  if (!init)
    return false;
  LLVMSetInitializer(runtime_ptr_global, init);
  return true;
}

void codegen_emit(codegen_t *cg) {
  NY_COMPILER_ASSERT(cg != NULL, "codegen_emit missing codegen");
  if (!cg)
    return;
  NY_COMPILER_ASSERT(cg->prog != NULL, "codegen_emit missing program");
  NY_COMPILER_ASSERT(cg->module != NULL, "codegen_emit missing LLVM module");
  NY_COMPILER_ASSERT(cg->builder != NULL, "codegen_emit missing LLVM builder");
  if (!cg->prog)
    return;
  NY_COMPILER_ASSERT(cg->prog->body.len <= cg->prog->body.cap,
                     "codegen_emit program body vector len exceeds cap");
  NY_COMPILER_ASSERT(cg->prog->body.data != NULL || cg->prog->body.len == 0,
                     "codegen_emit program body vector has len but no data");
  NY_COMPILER_ASSERT(cg->extra_progs.len <= cg->extra_progs.cap,
                     "codegen_emit extra_progs vector len exceeds cap");
  NY_COMPILER_ASSERT(cg->extra_progs.data != NULL || cg->extra_progs.len == 0,
                     "codegen_emit extra_progs vector has len but no data");

  scope gsc[64] = {0};
  size_t gd = 0;

  ny_lazy_emit_build_reachable_set(cg);

  ny_tick_t t_emit_top = ny_ticks_now();
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL,
                        "null top-level stmt during emit at index %zu", i);
    if (!s)
      continue;
    if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
        !ny_stmt_tree_is_source_context(cg, s)) {
      continue;
    }
    emit_top_functions(cg, s, gsc, gd, NULL);
  }
  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    NY_COMPILER_ASSERTF(
        prog->body.len <= prog->body.cap,
        "extra program %zu body vector len exceeds cap during emit", p);
    NY_COMPILER_ASSERTF(
        prog->body.data != NULL || prog->body.len == 0,
        "extra program %zu body vector has len but no data during emit", p);
    for (size_t i = 0; i < prog->body.len; i++) {
      cg->current_module_name = NULL;
      stmt_t *s = prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL,
                          "null extra-program stmt during emit p=%zu index=%zu",
                          p, i);
      if (!s)
        continue;
      if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
          !ny_stmt_tree_is_source_context(cg, s)) {
        continue;
      }
      emit_top_functions(cg, s, gsc, gd, NULL);
    }
  }
  ny_lazy_emit_demand_referenced(cg, gsc, gd, "top");
  if (verbose_enabled >= 1)
    fprintf(stderr, "[*] Codegen: emit top:       %.4fs\n",
            ny_ticks_elapsed_sec(t_emit_top));
}

typedef struct {
  assigned_name_list *names;
  assigned_hash_list *hashes;
  uint64_t *bloom;
} ny_top_entry_block_ctx_t;

static bool ny_collect_top_entry_blocked_expr_pre(ny_visitor_t *v, expr_t *e) {
  if (!v || !e || e->kind != NY_E_IDENT)
    return true;
  ny_top_entry_block_ctx_t *ctx = (ny_top_entry_block_ctx_t *)v->ctx;
  if (!ctx)
    return true;
  assigned_name_add(ctx->names, ctx->hashes, ctx->bloom, e->as.ident.name);
  return true;
}

static bool ny_collect_top_entry_blocked_stmt_pre(ny_visitor_t *v, stmt_t *s) {
  if (!v || !s || s->kind != NY_S_VAR)
    return true;
  ny_top_entry_block_ctx_t *ctx = (ny_top_entry_block_ctx_t *)v->ctx;
  if (!ctx)
    return true;
  for (size_t i = 0; i < s->as.var.names.len; ++i) {
    const char *name = s->as.var.names.data[i];
    if (name && *name)
      assigned_name_add(ctx->names, ctx->hashes, ctx->bloom, name);
  }
  return true;
}

static void ny_collect_top_entry_blocked_names(stmt_t *s,
                                               assigned_name_list *names,
                                               assigned_hash_list *hashes,
                                               uint64_t bloom[4]) {
  if (!s || !names || !hashes)
    return;
  if (s->kind == NY_S_FUNC) {
    ny_top_entry_block_ctx_t ctx = {names, hashes, bloom};
    ny_visitor_t vis = {0};
    vis.ctx = &ctx;
    vis.visit_expr_pre = ny_collect_top_entry_blocked_expr_pre;
    vis.visit_stmt_pre = ny_collect_top_entry_blocked_stmt_pre;
    ny_visit_stmt(&vis, s->as.fn.body);
    return;
  }
  if (s->kind == NY_S_IF) {
    ny_collect_top_entry_blocked_names(s->as.iff.conseq, names, hashes, bloom);
    ny_collect_top_entry_blocked_names(s->as.iff.alt, names, hashes, bloom);
  } else if (s->kind == NY_S_GUARD) {
    ny_collect_top_entry_blocked_names(s->as.guard.fallback, names, hashes,
                                       bloom);
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_collect_top_entry_blocked_names(s->as.block.body.data[i], names,
                                         hashes, bloom);
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_collect_top_entry_blocked_names(s->as.module.body.data[i], names,
                                         hashes, bloom);
  }
}

static void ny_apply_top_level_typeinfer_to_sema(typeinfer_ctx_t *ctx,
                                                 stmt_t *s) {
  if (!ctx || !s)
    return;
  switch (s->kind) {
  case NY_S_VAR: {
    if (s->sema_kind != NY_STMT_SEMA_VAR)
      return;
       sema_var_t *sv = (sema_var_t *)s->sema;
       if (!sv)
         return;
       arena_t *sema_arena = ctx->cg ? ctx->cg->arena : NULL;
       for (size_t i = 0; i < s->as.var.names.len; ++i) {
         const char *name = s->as.var.names.data[i];
         while (sv->is_int_proven.len <= i) {
           if (sema_arena)
             vec_push_arena(sema_arena, &sv->is_int_proven, false);
           else
             vec_push(&sv->is_int_proven, false);
         }
         while (sv->is_f64_proven.len <= i) {
           if (sema_arena)
             vec_push_arena(sema_arena, &sv->is_f64_proven, false);
           else
             vec_push(&sv->is_f64_proven, false);
         }
         while (sv->escapes.len <= i) {
           if (sema_arena)
             vec_push_arena(sema_arena, &sv->escapes, false);
           else
             vec_push(&sv->escapes, false);
         }
      bool proven_i64 = name && typeinfer_is_i64(ctx, name) &&
                        !typeinfer_needs_dynamic(ctx, name);
      bool proven_f64 = name && typeinfer_is_f64(ctx, name) &&
                        !typeinfer_needs_dynamic(ctx, name);
      bool escapes = name && typeinfer_escapes(ctx, name);
      sv->is_int_proven.data[i] = proven_i64;
      sv->is_f64_proven.data[i] = proven_f64;
      sv->escapes.data[i] = escapes;
    }
    return;
  }
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_apply_top_level_typeinfer_to_sema(ctx, s->as.block.body.data[i]);
    return;
  case NY_S_IF:
    ny_apply_top_level_typeinfer_to_sema(ctx, s->as.iff.conseq);
    ny_apply_top_level_typeinfer_to_sema(ctx, s->as.iff.alt);
    return;
  case NY_S_GUARD:
    ny_apply_top_level_typeinfer_to_sema(ctx, s->as.guard.fallback);
    return;
  default:
    return;
  }
}

LLVMValueRef codegen_emit_script(codegen_t *cg, const char *name) {
  NY_COMPILER_ASSERT(cg != NULL, "codegen_emit_script missing codegen");
  if (!cg)
    return NULL;
  NY_COMPILER_ASSERT(cg->prog != NULL, "codegen_emit_script missing program");
  NY_COMPILER_ASSERT(cg->module != NULL,
                     "codegen_emit_script missing LLVM module");
  NY_COMPILER_ASSERT(cg->builder != NULL,
                     "codegen_emit_script missing LLVM builder");
  NY_COMPILER_ASSERT(name && *name, "codegen_emit_script missing entry name");
  if (!cg->prog || !name || !*name)
    return NULL;
  NY_COMPILER_ASSERT(cg->type_i64 != NULL,
                     "codegen_emit_script missing i64 type");
  NY_COMPILER_ASSERT(cg->prog->body.len <= cg->prog->body.cap,
                     "codegen_emit_script program body vector len exceeds cap");
  NY_COMPILER_ASSERT(
      cg->prog->body.data != NULL || cg->prog->body.len == 0,
      "codegen_emit_script program body vector has len but no data");

  cg->current_module_name = NULL;
  LLVMValueRef fn = ny_get_named_fn(cg, name);
  if (ny_fn_has_body(fn))
    return fn;
  if (!fn) {
    fn = LLVMAddFunction(cg->module, name,
                         LLVMFunctionType(cg->type_i64, NULL, 0, 0));
  }
  ny_debug_apply_fn_attrs(cg, fn);
  LLVMMetadataRef prev_scope = cg->di_scope;
  LLVMMetadataRef prev_loc = cg->di_loc;
  LLVMBasicBlockRef cur = ny_cur_block(cg);
  LLVMBasicBlockRef init_block = ny_bb_fn(fn, "init");
  LLVMBasicBlockRef body_block = ny_bb_fn(fn, "body");
  if (cg->debug_symbols && cg->di_builder) {
    token_t tok = {0};
    tok.filename = cg->debug_main_file ? cg->debug_main_file : "<inline>";
    tok.line = 1;
    tok.col = 0;
    LLVMMetadataRef sp = codegen_debug_subprogram(cg, fn, name, tok);
    if (sp)
      cg->di_scope = sp;
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
    if (cg->alloca_builder)
      LLVMSetCurrentDebugLocation2(cg->alloca_builder, NULL);
  }
  ny_pos(cg, body_block);
  scope sc[64] = {0};
  size_t d = 0;
  assigned_name_list top_entry_blocked_names = {0};
  assigned_hash_list top_entry_blocked_hashes = {0};
  uint64_t top_entry_blocked_bloom[4] = {0, 0, 0, 0};

  if (cg->opt_type_infer && cg->prog && cg->prog->body.len > 0) {
    typeinfer_ctx_t infer_ctx = {0};
    typeinfer_ctx_init(&infer_ctx, 256, sc, cg);

    for (size_t i = 0; i < cg->prog->body.len; i++) {
      stmt_t *s = cg->prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL,
                          "null top-level typeinfer stmt at index %zu", i);
      if (!s)
        continue;
      if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
          !cg->emit_cached_stdlib_init && !ny_stmt_tree_is_source_context(cg, s))
        continue;
      if (s->kind != NY_S_FUNC)
        typeinfer_walk_stmt(&infer_ctx, s);
    }
    for (size_t i = 0; i < cg->prog->body.len; i++) {
      stmt_t *s = cg->prog->body.data[i];
      NY_COMPILER_ASSERTF(
          s != NULL, "null top-level typeinfer apply stmt at index %zu", i);
      if (!s)
        continue;
      if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
          !cg->emit_cached_stdlib_init && !ny_stmt_tree_is_source_context(cg, s))
        continue;
      if (s->kind != NY_S_FUNC)
        ny_apply_top_level_typeinfer_to_sema(&infer_ctx, s);
    }
    for (size_t i = 0; i < infer_ctx.var_names_len; ++i) {
      const char *name = infer_ctx.vars[i].name;
      if (!name || !*name)
        continue;
      if (typeinfer_needs_dynamic(&infer_ctx, name))
        assigned_name_add(&top_entry_blocked_names, &top_entry_blocked_hashes,
                          top_entry_blocked_bloom, name);
    }

    typeinfer_apply_to_scopes(&infer_ctx, sc, 1);
    typeinfer_ctx_dispose(&infer_ctx);
  }

  if (cg->prog && cg->prog->body.len > 0) {
    const char *root_file =
        (cg->debug_main_file && *cg->debug_main_file) ? cg->debug_main_file
                                                      : NULL;
    if (!root_file && cg->source_main_file && *cg->source_main_file)
      root_file = cg->source_main_file;
    bool has_user_top_funcs = false;
    if (!root_file) {
      for (size_t i = 0; i < cg->prog->body.len; i++) {
        stmt_t *s = cg->prog->body.data[i];
        if (!s || ny_is_stdlib_tok(s->tok))
          continue;
        if (s->tok.filename && *s->tok.filename) {
          root_file = s->tok.filename;
          break;
        }
        if (ny_stmt_contains_top_function(s))
          has_user_top_funcs = true;
      }
    }
    for (size_t i = 0; i < cg->prog->body.len; i++) {
      stmt_t *s = cg->prog->body.data[i];
      if (!s || ny_is_stdlib_tok(s->tok))
        continue;
      if (ny_stmt_contains_top_function(s)) {
        has_user_top_funcs = true;
        break;
      }
    }
    if (!root_file)
      root_file = cg->debug_main_file;
    size_t start_idx = 0;
    if (root_file && *root_file) {
      start_idx = cg->prog->body.len;
      while (start_idx > 0) {
        stmt_t *s = cg->prog->body.data[start_idx - 1];
        if (!s || ny_is_stdlib_tok(s->tok))
          break;
        if (!s->tok.filename || strcmp(root_file, s->tok.filename) != 0)
          break;
        start_idx--;
      }
      if (start_idx == cg->prog->body.len)
        start_idx = 0;
    }
    for (size_t i = start_idx; i < cg->prog->body.len; i++) {
      stmt_t *s = cg->prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL,
                          "null top-entry blocked-name stmt at index %zu", i);
      if (!s)
        continue;
      if (ny_is_stdlib_tok(s->tok))
        continue;
      if (root_file && s->tok.filename &&
          strcmp(root_file, s->tok.filename) != 0)
        continue;
      if (has_user_top_funcs && s->kind == NY_S_VAR) {
        for (size_t j = 0; j < s->as.var.names.len; ++j) {
          const char *name = s->as.var.names.data[j];
          if (name && *name)
            assigned_name_add(&top_entry_blocked_names,
                              &top_entry_blocked_hashes,
                              top_entry_blocked_bloom, name);
        }
      }
      ny_collect_top_entry_blocked_names(s, &top_entry_blocked_names,
                                         &top_entry_blocked_hashes,
                                         top_entry_blocked_bloom);
    }
  }
  cg->top_entry_blocked_names_data = top_entry_blocked_names.data;
  cg->top_entry_blocked_names_len = top_entry_blocked_names.len;
  cg->top_entry_blocked_hashes_data = top_entry_blocked_hashes.data;
  cg->top_entry_blocked_hashes_len = top_entry_blocked_hashes.len;
  cg->top_entry_blocked_bloom[0] = top_entry_blocked_bloom[0];
  cg->top_entry_blocked_bloom[1] = top_entry_blocked_bloom[1];
  cg->top_entry_blocked_bloom[2] = top_entry_blocked_bloom[2];
  cg->top_entry_blocked_bloom[3] = top_entry_blocked_bloom[3];
  cg->top_entry_local_hoist_enabled = true;

  LLVMValueRef std_init = ny_get_named_fn(cg, "__std_init");
  if (std_init) {
    LLVMBuildCall2(cg->builder, LLVMGlobalGetValueType(std_init), std_init,
                   NULL, 0, "");
  }

  size_t stmt_count = 0;
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null top-level script stmt at index %zu",
                        i);
    if (!s)
      continue;
    if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
        !cg->emit_cached_stdlib_init && !ny_stmt_tree_is_source_context(cg, s))
      continue;
    if (s->kind != NY_S_FUNC) {
      cg->current_module_name = NULL;
      NY_COMPILER_ASSERTF(
          d < 64,
          "top-level scope depth %zu exceeds fixed stack before stmt %zu", d,
          i);
      if (stmt_count > 0 && stmt_count % 100 == 0) {
        LLVMBasicBlockRef next_bb = ny_bb_fn(fn, "top_chunk");
        ny_br(cg, next_bb);
        ny_pos(cg, next_bb);
      }
      gen_stmt(cg, sc, &d, s, 0, false);
      cg->current_module_name = NULL;
      NY_COMPILER_ASSERTF(
          d < 64,
          "top-level scope depth %zu exceeds fixed stack after stmt %zu", d, i);
      stmt_count++;
    }
  }
  ny_lazy_emit_demand_referenced(cg, sc, d, "script");
  cg->current_module_name = NULL;
  if (!ny_has_terminator(cg)) {
    LLVMBuildRet(cg->builder, ny_c1(cg));
  }
  ny_pos(cg, init_block);
  if (cg->debug_symbols && cg->di_builder) {
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
  }
  codegen_emit_string_init(cg);
  ny_br(cg, body_block);
  vec_free(&sc[0].defers);
  vec_free(&sc[0].vars);
  if (cur) {
    ny_pos(cg, cur);
  }
  cg->top_entry_local_hoist_enabled = false;
  cg->top_entry_blocked_names_data = NULL;
  cg->top_entry_blocked_names_len = 0;
  cg->top_entry_blocked_hashes_data = NULL;
  cg->top_entry_blocked_hashes_len = 0;
  cg->top_entry_blocked_bloom[0] = 0;
  cg->top_entry_blocked_bloom[1] = 0;
  cg->top_entry_blocked_bloom[2] = 0;
  cg->top_entry_blocked_bloom[3] = 0;
  vec_free(&top_entry_blocked_names);
  vec_free(&top_entry_blocked_hashes);
  cg->di_scope = prev_scope;
  cg->di_loc = prev_loc;
  if (cg->debug_symbols && cg->builder) {
    LLVMSetCurrentDebugLocation2(cg->builder, prev_loc);
    if (cg->alloca_builder)
      LLVMSetCurrentDebugLocation2(cg->alloca_builder, prev_loc);
  }
  return fn;
}

void codegen_emit_string_init(codegen_t *cg) {
  LLVMTypeRef i8_ptr_ty = LLVMPointerType(ny_i8_ty(cg), 0);
  LLVMTypeRef i8_ty = ny_i8_ty(cg);
  bool const_string_global_init =
      ny_codegen_speed_profile_enabled(cg) ||
      ny_fast_path_enabled(cg, "NYTRIX_CONST_STRING_GLOBAL_INIT");

  size_t str_count = 0;
  bool init_all = (cg->current_module_name == NULL);
  for (size_t i = 0; i < cg->interns.len; i++) {
    if ((init_all || cg->interns.data[i].module == cg->module) &&
        cg->interns.data[i].gv && cg->interns.data[i].val)
      str_count++;
  }

  if (str_count > 0) {
    LLVMValueRef *elements = malloc(str_count * sizeof(LLVMValueRef));
    if (!elements)
      return;
    size_t idx = 0;
    for (size_t i = 0; i < cg->interns.len; i++) {
      string_intern *si = &cg->interns.data[i];
      if ((!init_all && si->module != cg->module) || !si->gv || !si->val)
        continue;
      elements[idx++] = LLVMConstPointerCast(si->gv, i8_ptr_ty);
    }

    LLVMValueRef used_global = LLVMGetNamedGlobal(cg->module, "llvm.used");
    if (used_global) {

      LLVMValueRef old_init = LLVMGetInitializer(used_global);
      size_t old_count = LLVMGetArrayLength(LLVMTypeOf(old_init));
      size_t new_count = old_count + str_count;
      LLVMValueRef *new_elements = malloc(new_count * sizeof(LLVMValueRef));
      for (size_t j = 0; j < old_count; j++)
        new_elements[j] = LLVMGetAggregateElement(old_init, (unsigned)j);
      for (size_t j = 0; j < str_count; j++)
        new_elements[old_count + j] = elements[j];
      ny_replace_llvm_used_global(cg->module, i8_ptr_ty, new_elements,
                                  new_count);
      free(new_elements);
    } else {
      ny_replace_llvm_used_global(cg->module, i8_ptr_ty, elements, str_count);
    }
    free(elements);
  }

  if (cg->emit_module_decls_only) {
    LLVMValueRef g = LLVMGetFirstGlobal(cg->module);
    while (g) {
      const char *gname = LLVMGetValueName(g);
      if (gname && strncmp(gname, ".str.data.", 10) == 0) {

        bool found = false;
        LLVMValueRef ug = LLVMGetNamedGlobal(cg->module, "llvm.used");
        if (ug) {
          LLVMValueRef init = LLVMGetInitializer(ug);
          size_t uc = LLVMGetArrayLength(LLVMTypeOf(init));
          for (size_t j = 0; j < uc; j++) {
            LLVMValueRef elem = LLVMGetAggregateElement(init, (unsigned)j);
            if (elem == g ||
                (elem && LLVMIsAConstantExpr(elem) &&
                 LLVMGetOperand(LLVMIsAConstantExpr(elem), 0) == g)) {
              found = true;
              break;
            }
          }
        }
        if (!found) {
          LLVMValueRef cast = LLVMConstPointerCast(g, i8_ptr_ty);
          LLVMValueRef ug2 = LLVMGetNamedGlobal(cg->module, "llvm.used");
          if (ug2) {
            LLVMValueRef old_init = LLVMGetInitializer(ug2);
            size_t old_count = LLVMGetArrayLength(LLVMTypeOf(old_init));
            size_t new_count = old_count + 1;
            LLVMValueRef *new_elements =
                malloc(new_count * sizeof(LLVMValueRef));
            for (size_t j = 0; j < old_count; j++)
              new_elements[j] = LLVMGetAggregateElement(old_init, (unsigned)j);
            new_elements[old_count] = cast;
            ny_replace_llvm_used_global(cg->module, i8_ptr_ty, new_elements,
                                        new_count);
            free(new_elements);
          }
        }
      }
      g = LLVMGetNextGlobal(g);
    }
  }

  for (size_t i = 0; i < cg->interns.len; i++) {
    if (!init_all && cg->interns.data[i].module != cg->module)
      continue;
    LLVMValueRef str_array_global = cg->interns.data[i].gv;
    LLVMValueRef runtime_ptr_global = cg->interns.data[i].val;
    if (!str_array_global || !runtime_ptr_global)
      continue;
    if (const_string_global_init &&
        ny_set_const_string_runtime_initializer(cg, str_array_global,
                                                runtime_ptr_global, i8_ty))
      continue;
    LLVMTypeRef rt_ty = LLVMTypeOf(runtime_ptr_global);

    LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, 0)};
    LLVMValueRef str_data_ptr = LLVMBuildInBoundsGEP2(
        cg->builder, i8_ty, str_array_global, indices, 1, "");
    if (LLVMGetTypeKind(rt_ty) == LLVMPointerTypeKind) {
      ny_store(cg, runtime_ptr_global, str_data_ptr);
    } else {
      LLVMValueRef str_data_int = ny_ptr2i64(cg, str_data_ptr, "");
      ny_store(cg, runtime_ptr_global, str_data_int);
    }
  }

  if (init_all) {
    size_t old_intern_len = cg->interns.len;
    codegen_repopulate_interns(cg);
    for (size_t i = old_intern_len; i < cg->interns.len; i++) {
      if (cg->interns.data[i].module != cg->module)
        continue;
      LLVMValueRef str_array_global = cg->interns.data[i].gv;
      LLVMValueRef runtime_ptr_global = cg->interns.data[i].val;
      if (!str_array_global || !runtime_ptr_global)
        continue;
      if (const_string_global_init &&
          ny_set_const_string_runtime_initializer(cg, str_array_global,
                                                  runtime_ptr_global, i8_ty))
        continue;
      LLVMTypeRef rt_ty = LLVMTypeOf(runtime_ptr_global);

      LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, 0)};
      LLVMValueRef str_data_ptr = LLVMBuildInBoundsGEP2(
          cg->builder, i8_ty, str_array_global, indices, 1, "");
      if (LLVMGetTypeKind(rt_ty) == LLVMPointerTypeKind) {
        ny_store(cg, runtime_ptr_global, str_data_ptr);
      } else {
        LLVMValueRef str_data_int = ny_ptr2i64(cg, str_data_ptr, "");
        ny_store(cg, runtime_ptr_global, str_data_int);
      }
    }
  }
}

static void codegen_free_owned_binding_name(binding *b) {
  if (!b || !b->owned || !b->name)
    return;
  if (!ny_intern_contains_ptr(b->name))
    free((void *)b->name);
  b->name = NULL;
}

static void codegen_free_owned_alias_binding(binding *b) {
  if (!b || !b->owned)
    return;
  codegen_free_owned_binding_name(b);
  free((void *)b->stmt_t);
  b->stmt_t = NULL;
}

static void codegen_free_layout_def(layout_def_t *def) {
  if (!def)
    return;
  bool owns_field_strings = def->stmt == NULL;
  if (def->name && !ny_intern_contains_ptr(def->name))
    free((void *)def->name);
  def->name = NULL;
  for (size_t i = 0; i < def->fields.len; i++) {
    if (owns_field_strings) {
      free((void *)def->fields.data[i].name);
      free((void *)def->fields.data[i].type_name);
    }
  }
  vec_free(&def->fields);
  if (def->heap_allocated)
    free(def);
}

void codegen_dispose(codegen_t *cg) {
  if (!cg)
    return;

  if (cg->di_builder) {
    codegen_debug_finalize(cg);
  }
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
  for (size_t i = 0; i < cg->fun_sigs.len; i++)
    ny_fun_sig_free_members(&cg->fun_sigs.data[i]);
  vec_free(&cg->fun_sigs);
  for (size_t i = 0; i < cg->global_vars.len; i++)
    codegen_free_owned_binding_name(&cg->global_vars.data[i]);
  vec_free(&cg->global_vars);
  for (size_t i = 0; i < cg->interns.len; i++) {
    void *alloc = cg->interns.data[i].alloc;
    if (!alloc)
      continue;
    bool seen = false;
    for (size_t j = 0; j < i; j++) {
      if (cg->interns.data[j].alloc == alloc) {
        seen = true;
        break;
      }
    }
    if (!seen)
      free(alloc);
  }
  vec_free(&cg->interns);
  if (cg->intern_map) {
    free(cg->intern_map);
    cg->intern_map = NULL;
  }
  free(cg->builtin_shadow_cache);
  cg->builtin_shadow_cache = NULL;
  for (size_t i = 0; i < cg->aliases.len; i++)
    codegen_free_owned_alias_binding(&cg->aliases.data[i]);
  vec_free(&cg->aliases);
  free(cg->module_alias_index);
  cg->module_alias_index = NULL;
  cg->module_alias_index_cap = 0;
  cg->module_alias_index_len = 0;
  for (size_t i = 0; i < cg->import_aliases.len; i++)
    codegen_free_owned_alias_binding(&cg->import_aliases.data[i]);
  vec_free(&cg->import_aliases);
  for (size_t i = 0; i < cg->user_import_aliases.len; i++)
    codegen_free_owned_alias_binding(&cg->user_import_aliases.data[i]);
  vec_free(&cg->user_import_aliases);
  vec_free(&cg->import_alias_hashes);
  vec_free(&cg->user_import_alias_hashes);
  free(cg->import_alias_index);
  free(cg->user_import_alias_index);
  cg->import_alias_index = NULL;
  cg->user_import_alias_index = NULL;
  cg->import_alias_index_cap = 0;
  cg->user_import_alias_index_cap = 0;
  free(cg->module_stmt_index);
  cg->module_stmt_index = NULL;
  cg->module_stmt_index_cap = 0;
  cg->module_stmt_index_len = 0;
  free(cg->module_stmt_lookup_cache);
  cg->module_stmt_lookup_cache = NULL;
  free(cg->module_public_target_cache);
  cg->module_public_target_cache = NULL;
  free(cg->use_alias_lookup_cache);
  cg->use_alias_lookup_cache = NULL;
  vec_free(&cg->use_modules);
  vec_free(&cg->user_use_modules);
  for (size_t i = 0; i < cg->link_allowed_modules.len; i++)
    free(cg->link_allowed_modules.data[i]);
  vec_free(&cg->link_allowed_modules);
  for (size_t i = 0; i < cg->tagged_types.len; i++)
    free(cg->tagged_types.data[i]);
  vec_free(&cg->tagged_types);
  vec_free(&cg->lazy_emit_names);
  vec_free(&cg->lazy_emit_hashes);
  ny_lazy_name_set_free(&cg->lazy_emit_name_set);
  vec_free(&cg->lazy_emit_collected_names);
  vec_free(&cg->lazy_emit_collected_hashes);
  ny_lazy_name_set_free(&cg->lazy_emit_collected_set);
  vec_free(&cg->labels);
  vec_free(&cg->operators);
  vec_free(&cg->enums);
  for (size_t i = 0; i < cg->layouts.len; i++)
    codegen_free_layout_def(cg->layouts.data[i]);
  vec_free(&cg->layouts);
  vec_free(&cg->mono_specs);
  for (size_t i = 0; i < cg->links.len; i++)
    free(cg->links.data[i]);
  vec_free(&cg->links);
  for (size_t i = 0; i < cg->ffi.defines.len; i++)
    free(cg->ffi.defines.data[i]);
  vec_free(&cg->ffi.defines);
  for (size_t i = 0; i < cg->ffi.includes_len; i++) {
    free((void *)cg->ffi.includes[i].path);
    free((void *)cg->ffi.includes[i].prefix);
    free((void *)cg->ffi.includes[i].lib);
  }
  free(cg->ffi.includes);
  for (size_t i = 0; i < cg->extra_progs.len; i++) {
    program_t *prog = cg->extra_progs.data[i];
    arena_t *arena = i < cg->extra_arenas.len ? cg->extra_arenas.data[i] : NULL;
    bool arena_seen = false;
    for (size_t j = 0; j < i && j < cg->extra_arenas.len; j++) {
      if (cg->extra_arenas.data[j] == arena) {
        arena_seen = true;
        break;
      }
    }
    if (arena && arena != (arena_t *)cg->arena && !arena_seen)
      program_free(prog, arena);
    if (prog && prog != cg->prog)
      free(prog);
  }
  for (size_t i = cg->extra_progs.len; i < cg->extra_arenas.len; i++) {
    arena_t *arena = cg->extra_arenas.data[i];
    bool arena_seen = false;
    for (size_t j = 0; j < i; j++) {
      if (cg->extra_arenas.data[j] == arena) {
        arena_seen = true;
        break;
      }
    }
    if (arena && arena != (arena_t *)cg->arena && !arena_seen) {
      arena_free(arena);
      free(arena);
    }
  }
  vec_free(&cg->extra_arenas);
  vec_free(&cg->extra_progs);
  if (cg->prog && cg->prog_owned) {
    program_free(cg->prog, (arena_t *)cg->arena);
    free(cg->prog);
  }
}
