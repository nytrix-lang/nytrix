#include "base/util.h"
#include "priv.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

static int parse_runtime_call_arity(const char *name) {
  if (!name || strncmp(name, "__call", 6) != 0)
    return -1;
  const char *num = name + 6;
  if (!*num)
    return -1;
  int arity = 0;
  for (; *num; ++num) {
    if (*num < '0' || *num > '9')
      return -1;
    arity = arity * 10 + (*num - '0');
  }
  return arity;
}

static LLVMValueRef
ny_build_memoized_direct_call(codegen_t *cg, token_t tok, LLVMTypeRef ft,
                              LLVMValueRef callee, LLVMValueRef *args,
                              unsigned argc, bool int_only) {
  if (!cg || !cg->builder)
    return 0;
#define NY_MEMO_FALLBACK_CALL()                                                \
  do {                                                                         \
    ny_dbg_loc(cg, tok);                                                       \
    return LLVMBuildCall2(cg->builder, ft, callee, args, argc, "");            \
  } while (0)
  if (!cg->auto_memoize)
    NY_MEMO_FALLBACK_CALL();
  if (LLVMGetReturnType(ft) != cg->type_i64)
    NY_MEMO_FALLBACK_CALL();
  if (argc > 6)
    NY_MEMO_FALLBACK_CALL();
  for (unsigned i = 0; i < argc; i++) {
    if (LLVMTypeOf(args[i]) != cg->type_i64)
      NY_MEMO_FALLBACK_CALL();
  }

  LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
  if (!cur_bb)
    NY_MEMO_FALLBACK_CALL();
  LLVMValueRef parent_fn = LLVMGetBasicBlockParent(cur_bb);
  if (!parent_fn)
    NY_MEMO_FALLBACK_CALL();

  uint64_t site_id = ++cg->auto_memo_site_seq;
  char valid_name[96];
  char res_name[96];
  char args_name[96];
  char depth_name[96];
  snprintf(valid_name, sizeof(valid_name), ".__ny_memo.valid.%llu",
           (unsigned long long)site_id);
  snprintf(res_name, sizeof(res_name), ".__ny_memo.res.%llu",
           (unsigned long long)site_id);
  snprintf(args_name, sizeof(args_name), ".__ny_memo.args.%llu",
           (unsigned long long)site_id);
  snprintf(depth_name, sizeof(depth_name), ".__ny_memo.depth.%llu",
           (unsigned long long)site_id);

  LLVMValueRef valid_g = LLVMGetNamedGlobal(cg->module, valid_name);
  if (!valid_g) {
    valid_g = LLVMAddGlobal(cg->module, cg->type_i1, valid_name);
    LLVMSetInitializer(valid_g, LLVMConstInt(cg->type_i1, 0, false));
    LLVMSetLinkage(valid_g, LLVMInternalLinkage);
    LLVMSetThreadLocal(valid_g, 1);
  }

  LLVMValueRef res_g = LLVMGetNamedGlobal(cg->module, res_name);
  if (!res_g) {
    res_g = LLVMAddGlobal(cg->module, cg->type_i64, res_name);
    LLVMSetInitializer(res_g, LLVMConstInt(cg->type_i64, 0, false));
    LLVMSetLinkage(res_g, LLVMInternalLinkage);
    LLVMSetThreadLocal(res_g, 1);
  }

  LLVMTypeRef arg_arr_ty = NULL;
  LLVMValueRef args_g = NULL;
  if (argc > 0) {
    arg_arr_ty = LLVMArrayType(cg->type_i64, argc);
    args_g = LLVMGetNamedGlobal(cg->module, args_name);
    if (!args_g) {
      args_g = LLVMAddGlobal(cg->module, arg_arr_ty, args_name);
      LLVMSetInitializer(args_g, LLVMConstNull(arg_arr_ty));
      LLVMSetLinkage(args_g, LLVMInternalLinkage);
      LLVMSetThreadLocal(args_g, 1);
    }
  }

  LLVMValueRef depth_g = LLVMGetNamedGlobal(cg->module, depth_name);
  if (!depth_g) {
    depth_g = LLVMAddGlobal(cg->module, cg->type_i64, depth_name);
    LLVMSetInitializer(depth_g, LLVMConstInt(cg->type_i64, 0, false));
    LLVMSetLinkage(depth_g, LLVMInternalLinkage);
    LLVMSetThreadLocal(depth_g, 1);
  }

  LLVMBasicBlockRef hit_bb = LLVMAppendBasicBlock(parent_fn, "memo.hit");
  LLVMBasicBlockRef miss_bb = LLVMAppendBasicBlock(parent_fn, "memo.miss");
  LLVMBasicBlockRef join_bb = LLVMAppendBasicBlock(parent_fn, "memo.join");

  ny_dbg_loc(cg, tok);
  LLVMValueRef valid = LLVMBuildLoad2(cg->builder, cg->type_i1, valid_g, "");
  LLVMValueRef hit_cond = valid;
  LLVMValueRef memo_args_ok = LLVMConstInt(cg->type_i1, 1, false);
  if (int_only && argc > 0) {
    for (unsigned i = 0; i < argc; i++) {
      LLVMValueRef bit = LLVMBuildAnd(cg->builder, args[i],
                                      LLVMConstInt(cg->type_i64, 1, false), "");
      LLVMValueRef is_int =
          LLVMBuildICmp(cg->builder, LLVMIntEQ, bit,
                        LLVMConstInt(cg->type_i64, 1, false), "");
      memo_args_ok = LLVMBuildAnd(cg->builder, memo_args_ok, is_int, "");
    }
  }
  LLVMValueRef depth_now =
      LLVMBuildLoad2(cg->builder, cg->type_i64, depth_g, "memo_depth");
  LLVMValueRef memo_depth_ok =
      LLVMBuildICmp(cg->builder, LLVMIntEQ, depth_now,
                    LLVMConstInt(cg->type_i64, 0, false), "");
  LLVMValueRef memo_lookup_ok =
      LLVMBuildAnd(cg->builder, memo_depth_ok, memo_args_ok, "");
  if (argc > 0 && args_g) {
    LLVMValueRef all_eq = LLVMConstInt(cg->type_i1, 1, false);
    for (unsigned i = 0; i < argc; i++) {
      LLVMValueRef idxs[2] = {LLVMConstInt(cg->type_i64, 0, false),
                              LLVMConstInt(cg->type_i64, i, false)};
      LLVMValueRef slot =
          LLVMBuildInBoundsGEP2(cg->builder, arg_arr_ty, args_g, idxs, 2, "");
      LLVMValueRef cached = LLVMBuildLoad2(cg->builder, cg->type_i64, slot, "");
      LLVMValueRef eq =
          LLVMBuildICmp(cg->builder, LLVMIntEQ, cached, args[i], "");
      all_eq = LLVMBuildAnd(cg->builder, all_eq, eq, "");
    }
    hit_cond = LLVMBuildAnd(cg->builder, valid, all_eq, "");
  }
  hit_cond = LLVMBuildAnd(cg->builder, hit_cond, memo_lookup_ok, "");
  LLVMBuildCondBr(cg->builder, hit_cond, hit_bb, miss_bb);

  LLVMPositionBuilderAtEnd(cg->builder, hit_bb);
  ny_dbg_loc(cg, tok);
  LLVMValueRef hit_res = LLVMBuildLoad2(cg->builder, cg->type_i64, res_g, "");
  LLVMBuildBr(cg->builder, join_bb);
  LLVMBasicBlockRef hit_end_bb = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, miss_bb);
  ny_dbg_loc(cg, tok);
  LLVMValueRef depth_inc = LLVMBuildAdd(
      cg->builder, depth_now, LLVMConstInt(cg->type_i64, 1, false), "");
  LLVMBuildStore(cg->builder, depth_inc, depth_g);
  LLVMValueRef miss_res =
      LLVMBuildCall2(cg->builder, ft, callee, args, argc, "");
  LLVMValueRef depth_after =
      LLVMBuildLoad2(cg->builder, cg->type_i64, depth_g, "");
  LLVMValueRef depth_dec = LLVMBuildSub(
      cg->builder, depth_after, LLVMConstInt(cg->type_i64, 1, false), "");
  LLVMBuildStore(cg->builder, depth_dec, depth_g);

  LLVMValueRef result_cacheable = LLVMConstInt(cg->type_i1, 1, false);
  if (int_only) {
    LLVMValueRef res_lsb = LLVMBuildAnd(
        cg->builder, miss_res, LLVMConstInt(cg->type_i64, 1, false), "");
    LLVMValueRef res_is_int =
        LLVMBuildICmp(cg->builder, LLVMIntEQ, res_lsb,
                      LLVMConstInt(cg->type_i64, 1, false), "");
    LLVMValueRef res_is_none =
        LLVMBuildICmp(cg->builder, LLVMIntEQ, miss_res,
                      LLVMConstInt(cg->type_i64, 0, false), "");
    LLVMValueRef res_is_true =
        LLVMBuildICmp(cg->builder, LLVMIntEQ, miss_res,
                      LLVMConstInt(cg->type_i64, 2, false), "");
    LLVMValueRef res_is_false =
        LLVMBuildICmp(cg->builder, LLVMIntEQ, miss_res,
                      LLVMConstInt(cg->type_i64, 4, false), "");
    LLVMValueRef res_small = LLVMBuildOr(
        cg->builder, res_is_none,
        LLVMBuildOr(cg->builder, res_is_true, res_is_false, ""), "");
    result_cacheable = LLVMBuildOr(cg->builder, res_is_int, res_small, "");
  }
  LLVMValueRef can_store =
      LLVMBuildAnd(cg->builder, memo_lookup_ok, result_cacheable, "");

  LLVMValueRef old_res = LLVMBuildLoad2(cg->builder, cg->type_i64, res_g, "");
  LLVMValueRef store_res =
      LLVMBuildSelect(cg->builder, can_store, miss_res, old_res, "");
  LLVMBuildStore(cg->builder, store_res, res_g);

  if (argc > 0 && args_g) {
    for (unsigned i = 0; i < argc; i++) {
      LLVMValueRef idxs[2] = {LLVMConstInt(cg->type_i64, 0, false),
                              LLVMConstInt(cg->type_i64, i, false)};
      LLVMValueRef slot =
          LLVMBuildInBoundsGEP2(cg->builder, arg_arr_ty, args_g, idxs, 2, "");
      LLVMValueRef old_arg =
          LLVMBuildLoad2(cg->builder, cg->type_i64, slot, "");
      LLVMValueRef store_arg =
          LLVMBuildSelect(cg->builder, can_store, args[i], old_arg, "");
      LLVMBuildStore(cg->builder, store_arg, slot);
    }
  }
  LLVMValueRef old_valid =
      LLVMBuildLoad2(cg->builder, cg->type_i1, valid_g, "");
  LLVMValueRef new_valid =
      LLVMBuildSelect(cg->builder, can_store,
                      LLVMConstInt(cg->type_i1, 1, false), old_valid, "");
  if (int_only) {
    LLVMValueRef should_clear =
        LLVMBuildAnd(cg->builder, memo_lookup_ok,
                     LLVMBuildNot(cg->builder, result_cacheable, ""), "");
    new_valid =
        LLVMBuildSelect(cg->builder, should_clear,
                        LLVMConstInt(cg->type_i1, 0, false), new_valid, "");
  }
  LLVMBuildStore(cg->builder, new_valid, valid_g);
  LLVMBuildBr(cg->builder, join_bb);
  LLVMBasicBlockRef miss_end_bb = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, join_bb);
  LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "memo_res");
  LLVMAddIncoming(phi, (LLVMValueRef[]){hit_res, miss_res},
                  (LLVMBasicBlockRef[]){hit_end_bb, miss_end_bb}, 2);
  return phi;
#undef NY_MEMO_FALLBACK_CALL
}

static bool ny_gencall_sig_in_current_sigs(const codegen_t *cg,
                                           const fun_sig *sig);

static fun_sig *ny_gencall_lookup_helper(codegen_t *cg, fun_sig **cache_slot,
                                         const char *const *names,
                                         size_t names_len) {
  if (!cg || !names || names_len == 0)
    return NULL;
  if (cache_slot && *cache_slot &&
      ny_gencall_sig_in_current_sigs(cg, *cache_slot))
    return *cache_slot;
  for (size_t i = 0; i < names_len; ++i) {
    const char *name = names[i];
    if (!name || !*name)
      continue;
    fun_sig *sig = lookup_fun(cg, name);
    if (sig) {
      if (cache_slot)
        *cache_slot = sig;
      return sig;
    }
  }
  return NULL;
}

#define NY_DEFINE_GENCALL_LOOKUP_WRAPPER(fn_name, cache_field, ...)            \
  static fun_sig *fn_name(codegen_t *cg) {                                     \
    static const char *const k_names[] = {__VA_ARGS__};                        \
    return ny_gencall_lookup_helper(cg, &cg->cache_field, k_names,             \
                                    sizeof(k_names) / sizeof(k_names[0]));     \
  }

NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_flt_unbox, cached_fn_flt_unbox,
                                 "__flt_unbox_val")
NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_flt_box, cached_fn_flt_box,
                                 "__flt_box_val")
NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_getter, cached_fn_get, "get",
                                 "std.core.get", "std.core.reflect.get",
                                 "dict_get")
NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_globals, cached_fn_globals,
                                 "__globals")
NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_list, cached_fn_list, "list",
                                 "std.core.list")
NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_kwarg, cached_fn_kwarg, "__kwarg",
                                 "std.core.__kwarg")

#undef NY_DEFINE_GENCALL_LOOKUP_WRAPPER

static bool ny_gencall_sig_in_current_sigs(const codegen_t *cg,
                                           const fun_sig *sig) {
  if (!cg || !sig || !cg->fun_sigs.data || cg->fun_sigs.len == 0)
    return false;
  const fun_sig *begin = cg->fun_sigs.data;
  const fun_sig *end = begin + cg->fun_sigs.len;
  return sig >= begin && sig < end;
}

static bool ny_gencall_is_thread_attr(fun_sig *sig) {
  if (!sig || sig->is_extern || !sig->stmt_t)
    return false;
  if (sig->stmt_t->kind != NY_S_FUNC)
    return false;
  return sig->stmt_t->as.fn.attr_thread;
}

static bool abi_type_is_tagged(const char *type_name) {
  if (!type_name || !*type_name)
    return true;
  return strcmp(type_name, "int") == 0 || strcmp(type_name, "str") == 0 ||
         strcmp(type_name, "bool") == 0 || strcmp(type_name, "Result") == 0;
}

static bool ny_memo_impure_return_allowed(const char *type_name) {
  if (!type_name || !*type_name)
    return false;
  return strcmp(type_name, "int") == 0 || strcmp(type_name, "bool") == 0 ||
         strcmp(type_name, "none") == 0;
}

static bool abi_type_is_ptr(const char *type_name) {
  return type_name && type_name[0] == '*';
}

static bool abi_type_is_float(const char *type_name) {
  return type_name &&
         (strcmp(type_name, "f32") == 0 || strcmp(type_name, "f64") == 0 ||
          strcmp(type_name, "f128") == 0);
}

static bool abi_type_is_signed_int(const char *type_name) {
  return type_name &&
         (strcmp(type_name, "i8") == 0 || strcmp(type_name, "i16") == 0 ||
          strcmp(type_name, "i32") == 0 || strcmp(type_name, "i64") == 0 ||
          strcmp(type_name, "i128") == 0 || strcmp(type_name, "char") == 0);
}

static bool abi_type_is_unsigned_int(const char *type_name) {
  return type_name &&
         (strcmp(type_name, "u8") == 0 || strcmp(type_name, "u16") == 0 ||
          strcmp(type_name, "u32") == 0 || strcmp(type_name, "u64") == 0 ||
          strcmp(type_name, "u128") == 0);
}

static LLVMTypeRef abi_type_from_name(codegen_t *cg, const char *type_name) {
  if (!type_name || !*type_name)
    return cg->type_i64;
  if (abi_type_is_ptr(type_name))
    return cg->type_i8ptr;
  if (abi_type_is_tagged(type_name))
    return cg->type_i64;
  if (strcmp(type_name, "void") == 0)
    return LLVMVoidTypeInContext(cg->ctx);
  if (strcmp(type_name, "char") == 0 || strcmp(type_name, "i8") == 0 ||
      strcmp(type_name, "u8") == 0)
    return cg->type_i8;
  if (strcmp(type_name, "i16") == 0 || strcmp(type_name, "u16") == 0)
    return cg->type_i16;
  if (strcmp(type_name, "i32") == 0 || strcmp(type_name, "u32") == 0)
    return cg->type_i32;
  if (strcmp(type_name, "i64") == 0 || strcmp(type_name, "u64") == 0)
    return cg->type_i64;
  if (strcmp(type_name, "i128") == 0 || strcmp(type_name, "u128") == 0)
    return cg->type_i128;
  if (strcmp(type_name, "f32") == 0)
    return cg->type_f32;
  if (strcmp(type_name, "f64") == 0)
    return cg->type_f64;
  if (strcmp(type_name, "f128") == 0)
    return cg->type_f128;
  return cg->type_i64;
}

static LLVMValueRef abi_untag_int(codegen_t *cg, LLVMValueRef v,
                                  bool is_signed) {
  LLVMValueRef shift = LLVMConstInt(cg->type_i64, 1, false);
  if (is_signed) {
    return LLVMBuildAShr(cg->builder, v, shift, "untag_i");
  }
  return LLVMBuildLShr(cg->builder, v, shift, "untag_u");
}

static LLVMValueRef abi_cast_int(codegen_t *cg, LLVMValueRef v,
                                 LLVMTypeRef target, bool is_signed) {
  if (LLVMTypeOf(v) == target)
    return v;
  unsigned src_w = LLVMGetIntTypeWidth(LLVMTypeOf(v));
  unsigned dst_w = LLVMGetIntTypeWidth(target);
  if (dst_w < src_w) {
    return LLVMBuildTrunc(cg->builder, v, target, "int_trunc");
  }
  if (dst_w > src_w) {
    return is_signed ? LLVMBuildSExt(cg->builder, v, target, "int_sext")
                     : LLVMBuildZExt(cg->builder, v, target, "int_zext");
  }
  return LLVMBuildBitCast(cg->builder, v, target, "int_cast");
}

static LLVMValueRef coerce_extern_arg(codegen_t *cg, LLVMValueRef v,
                                      const char *type_name) {
  if (!type_name || abi_type_is_tagged(type_name))
    return v;
  if (abi_type_is_ptr(type_name)) {
    return LLVMBuildIntToPtr(cg->builder, v, cg->type_i8ptr, "arg_ptr");
  }
  if (abi_type_is_float(type_name)) {
    fun_sig *unbox = ny_gencall_flt_unbox(cg);
    if (!unbox)
      return v;
    LLVMValueRef bits =
        LLVMBuildCall2(cg->builder, unbox->type, unbox->value, &v, 1, "");
    LLVMValueRef dbl = LLVMBuildBitCast(cg->builder, bits, cg->type_f64, "");
    if (strcmp(type_name, "f64") == 0)
      return dbl;
    if (strcmp(type_name, "f32") == 0)
      return LLVMBuildFPTrunc(cg->builder, dbl, cg->type_f32, "f32_arg");
    if (strcmp(type_name, "f128") == 0)
      return LLVMBuildFPExt(cg->builder, dbl, cg->type_f128, "f128_arg");
    return dbl;
  }
  if (abi_type_is_signed_int(type_name)) {
    LLVMValueRef raw = abi_untag_int(cg, v, true);
    LLVMTypeRef target = abi_type_from_name(cg, type_name);
    return abi_cast_int(cg, raw, target, true);
  }
  if (abi_type_is_unsigned_int(type_name)) {
    LLVMValueRef raw = abi_untag_int(cg, v, false);
    LLVMTypeRef target = abi_type_from_name(cg, type_name);
    return abi_cast_int(cg, raw, target, false);
  }
  return v;
}

static LLVMValueRef box_extern_result(codegen_t *cg, LLVMValueRef v,
                                      const char *type_name) {
  if (!type_name || abi_type_is_tagged(type_name))
    return v;
  if (strcmp(type_name, "void") == 0)
    return LLVMConstInt(cg->type_i64, 0, false);
  if (abi_type_is_ptr(type_name)) {
    return LLVMBuildPtrToInt(cg->builder, v, cg->type_i64, "ret_ptr");
  }
  if (abi_type_is_float(type_name)) {
    fun_sig *box = ny_gencall_flt_box(cg);
    if (!box)
      return LLVMConstInt(cg->type_i64, 0, false);
    LLVMValueRef dbl = v;
    if (strcmp(type_name, "f32") == 0) {
      dbl = LLVMBuildFPExt(cg->builder, v, cg->type_f64, "f32_to_f64");
    } else if (strcmp(type_name, "f128") == 0) {
      dbl = LLVMBuildFPTrunc(cg->builder, v, cg->type_f64, "f128_to_f64");
    }
    LLVMValueRef bits = LLVMBuildBitCast(cg->builder, dbl, cg->type_i64, "");
    return LLVMBuildCall2(cg->builder, box->type, box->value, &bits, 1, "");
  }
  if (abi_type_is_signed_int(type_name) ||
      abi_type_is_unsigned_int(type_name)) {
    bool signed_int = abi_type_is_signed_int(type_name);
    LLVMTypeRef target = cg->type_i64;
    LLVMValueRef widened = abi_cast_int(cg, v, target, signed_int);
    LLVMValueRef sh = LLVMBuildShl(cg->builder, widened,
                                   LLVMConstInt(cg->type_i64, 1, false), "");
    return LLVMBuildOr(cg->builder, sh, LLVMConstInt(cg->type_i64, 1, false),
                       "");
  }
  return v;
}

static void add_extern_sig(codegen_t *cg, const char *name, int arity) {
  if (!cg || !name || !*name || arity < 0)
    return;
  if (lookup_fun_exact(cg, name))
    return;
  LLVMTypeRef *pt = NULL;
  if (arity > 0)
    pt = alloca(sizeof(LLVMTypeRef) * (size_t)arity);
  for (int i = 0; i < arity; i++)
    pt[i] = cg->type_i64;
  LLVMTypeRef ft = LLVMFunctionType(cg->type_i64, pt, (unsigned)arity, 0);
  LLVMValueRef f = LLVMGetNamedFunction(cg->module, name);
  if (!f)
    f = LLVMAddFunction(cg->module, name, ft);
  fun_sig sig = {.name = ny_strdup(name),
                 .type = ft,
                 .value = f,
                 .stmt_t = NULL,
                 .arity = arity,
                 .is_variadic = false,
                 .is_extern = true,
                 .effects = NY_FX_ALL,
                 .args_escape = true,
                 .args_mutated = true,
                 .returns_alias = true,
                 .effects_known = false,
                 .link_name = ny_strdup(name),
                 .return_type = NULL,
                 .owned = false,
                 .name_hash = 0};
  vec_push(&cg->fun_sigs, sig);
}

static bool handle_extern_all_args(codegen_t *cg, ny_call_arg_list *args) {
  if (!args || args->len != 1)
    return false;
  expr_t *arg = args->data[0].val;
  if (!arg || arg->kind != NY_E_LIST)
    return false;
  for (size_t i = 0; i < arg->as.list_like.len; i++) {
    expr_t *item = arg->as.list_like.data[i];
    const char *name = NULL;
    int arity = 0;
    if (item->kind == NY_E_LITERAL && item->as.literal.kind == NY_LIT_STR) {
      name = item->as.literal.as.s.data;
      arity = 0;
    } else if ((item->kind == NY_E_LIST || item->kind == NY_E_TUPLE) &&
               item->as.list_like.len == 2) {
      expr_t *n = item->as.list_like.data[0];
      expr_t *a = item->as.list_like.data[1];
      if (n->kind == NY_E_LITERAL && n->as.literal.kind == NY_LIT_STR &&
          a->kind == NY_E_LITERAL && a->as.literal.kind == NY_LIT_INT) {
        name = n->as.literal.as.s.data;
        arity = (int)a->as.literal.as.i;
      }
    }
    if (!name || arity < 0) {
      ny_diag_error((token_t){0},
                    "extern_all expects list of names or [name, arity]");
      cg->had_error = 1;
      return true;
    }
    add_extern_sig(cg, name, arity);
  }
  return true;
}

static bool layout_query_arg(expr_t *arg, const char **out) {
  if (!arg || arg->kind != NY_E_LITERAL || arg->as.literal.kind != NY_LIT_STR)
    return false;
  *out = arg->as.literal.as.s.data;
  return true;
}

static LLVMValueRef emit_layout_query(codegen_t *cg, token_t tok,
                                      const char *layout_name,
                                      const char *field_name, bool want_align,
                                      bool want_offset) {
  if (!layout_name) {
    ny_diag_error(tok, "layout query expects a string literal name");
    cg->had_error = 1;
    return LLVMConstInt(cg->type_i64, 1, false);
  }
  layout_def_t *def = lookup_layout(cg, layout_name);
  if (!def) {
    ny_diag_error(tok, "unknown layout '%s'", layout_name);
    cg->had_error = 1;
    return LLVMConstInt(cg->type_i64, 1, false);
  }
  size_t val = def->size;
  if (want_align)
    val = def->align;
  if (want_offset) {
    if (!field_name) {
      ny_diag_error(tok, "layout offset expects a field name");
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 1, false);
    }
    bool found = false;
    for (size_t i = 0; i < def->fields.len; i++) {
      layout_field_info_t *fi = &def->fields.data[i];
      if (fi->name && strcmp(fi->name, field_name) == 0) {
        val = fi->offset;
        found = true;
        break;
      }
    }
    if (!found) {
      ny_diag_error(tok, "unknown field '%s' in layout '%s'", field_name,
                    layout_name);
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 1, false);
    }
  }
  return LLVMConstInt(cg->type_i64, ((uint64_t)val << 1) | 1, false);
}

static void report_missing_runtime_call_helper(codegen_t *cg, token_t tok,
                                               const char *name, size_t want) {
  ny_diag_error(tok, "undefined runtime call helper '%s'", name);
  const char *best_match = NULL;
  int best_delta = 1 << 30;
  int max_supported = -1;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    const char *candidate = cg->fun_sigs.data[i].name;
    int ar = parse_runtime_call_arity(candidate);
    if (ar >= 0) {
      if (ar > max_supported)
        max_supported = ar;
      int delta = ar - (int)want;
      if (delta < 0)
        delta = -delta;
      if (delta < best_delta) {
        best_delta = delta;
        best_match = candidate;
      }
    }
    if (strstr(candidate, name) || strstr(name, candidate)) {
      best_match = candidate;
      break;
    }
  }
  if (best_match)
    ny_diag_hint("did you mean '%s'?", best_match);
  if (max_supported >= 0 && (int)want > max_supported) {
    ny_diag_hint("runtime supports function calls up to %d arguments",
                 max_supported);
  }
  ny_diag_hint("runtime/library mismatch can cause missing __callN helpers");
  cg->had_error = 1;
}

static bool check_call_arity_diag(codegen_t *cg, token_t tok,
                                  fun_sig *sig_found, bool is_variadic,
                                  int sig_arity, size_t call_argc,
                                  bool member_with_target) {
  size_t min_arity = (size_t)sig_arity;
  if (!is_variadic && sig_found && sig_found->stmt_t &&
      sig_found->stmt_t->kind == NY_S_FUNC) {
    ny_param_list *params = &sig_found->stmt_t->as.fn.params;
    if (params->len == (size_t)sig_arity) {
      for (size_t i = 0; i < params->len; i++) {
        if (params->data[i].def) {
          min_arity = i;
          break;
        }
      }
    }
  }
  if (!is_variadic &&
      (call_argc < min_arity || call_argc > (size_t)sig_arity)) {
    bool strict_err = ny_strict_error_enabled(cg, tok);
    bool is_stdlib = ny_is_stdlib_tok(tok);
    bool emit_diag;
    if (is_stdlib && !strict_err && !debug_enabled) {
      token_t t = tok;
      t.line = 0;
      t.col = 0;
      emit_diag = ny_diag_should_emit("arity_mismatch_std", t,
                                      sig_found ? sig_found->name : "call");
    } else {
      emit_diag = ny_diag_should_emit("arity_mismatch", tok,
                                      sig_found ? sig_found->name : "call");
    }
    if (is_stdlib && !strict_err && verbose_enabled < 2)
      emit_diag = false;
    if (emit_diag) {
      if (strict_err)
        ny_diag_error(tok, "arity mismatch for \033[1;37m'%s'\033[0m",
                      sig_found->name);
      else
        ny_diag_warning(tok, "arity mismatch for \033[1;37m'%s'\033[0m",
                        sig_found->name);
      if (min_arity != (size_t)sig_arity) {
        ny_diag_hint("expected %zu..%d arguments, got %zu", min_arity,
                     sig_arity, call_argc);
      } else {
        ny_diag_hint("expected %d arguments, got %zu", sig_arity, call_argc);
      }
      if (member_with_target)
        ny_diag_hint(
            "member calls pass the target object as the first argument");
      if (strict_err)
        ny_diag_hint("strict diagnostics are enabled; unset "
                     "NYTRIX_STRICT_DIAGNOSTICS to downgrade to warning");
      if (call_argc < min_arity) {
        ny_diag_fix("call '%s' with at least %zu argument(s)", sig_found->name,
                    min_arity);
      } else {
        ny_diag_fix("call '%s' with %d argument(s)", sig_found->name,
                    sig_arity);
      }
    }
    if (strict_err) {
      cg->had_error = 1;
      return false;
    }
    return true;
  }
  if (is_variadic && call_argc < (size_t)sig_arity - 1) {
    ny_diag_error(tok,
                  "not enough arguments for variadic \033[1;37m'%s'\033[0m",
                  sig_found->name);
    ny_diag_hint("expected at least %d arguments, got %zu", sig_arity - 1,
                 call_argc);
    ny_diag_fix("add %d more argument(s) or use a non-variadic overload",
                (sig_arity - 1) - (int)call_argc);
    cg->had_error = 1;
    return false;
  }
  return true;
}

LLVMValueRef gen_call_expr(codegen_t *cg, scope *scopes, size_t depth,
                           expr_t *e) {
  if (!cg || !e) {
    if (cg)
      cg->had_error = 1;
    return LLVMConstInt(cg ? cg->type_i64 : LLVMInt64Type(), 0, false);
  }
  if (e->kind != NY_E_CALL && e->kind != NY_E_MEMCALL) {
    ny_diag_error(e->tok,
                  "internal error: expected call expression in gen_call_expr");
    cg->had_error = 1;
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  expr_call_t *c = (e->kind == NY_E_CALL) ? &e->as.call : NULL;
  expr_memcall_t *mc = (e->kind == NY_E_MEMCALL) ? &e->as.memcall : NULL;
  if (c && !c->callee) {
    ny_diag_error(e->tok, "invalid call expression: missing callee");
    cg->had_error = 1;
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  if (mc && (!mc->target || !mc->name || !*mc->name)) {
    ny_diag_error(e->tok, "invalid member call expression");
    cg->had_error = 1;
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  LLVMValueRef callee = NULL;
  LLVMTypeRef ft = NULL;
  LLVMValueRef fv = NULL;
  bool is_variadic = false;
  int sig_arity = 0;
  bool has_sig = false;
  bool skip_target = false;
  fun_sig *sig_found = NULL;

  if (c && c->callee && c->callee->kind == NY_E_IDENT &&
      c->callee->as.ident.name) {
    const char *n = c->callee->as.ident.name;
    if (strcmp(n, "extern_all") == 0 || strcmp(n, "__extern_all") == 0) {
      if (handle_extern_all_args(cg, &c->args))
        return LLVMConstInt(cg->type_i64, 0, false);
    }
    if (strcmp(n, "__layout_size") == 0 || strcmp(n, "__layout_align") == 0 ||
        strcmp(n, "__layout_offset") == 0) {
      bool want_align = strcmp(n, "__layout_align") == 0;
      bool want_offset = strcmp(n, "__layout_offset") == 0;
      if (want_offset) {
        if (c->args.len != 2) {
          ny_diag_error(e->tok, "%s expects 2 arguments", n);
          cg->had_error = 1;
          return LLVMConstInt(cg->type_i64, 1, false);
        }
        const char *layout_name = NULL;
        const char *field_name = NULL;
        if (!layout_query_arg(c->args.data[0].val, &layout_name) ||
            !layout_query_arg(c->args.data[1].val, &field_name)) {
          ny_diag_error(e->tok, "%s expects string literal arguments", n);
          cg->had_error = 1;
          return LLVMConstInt(cg->type_i64, 1, false);
        }
        return emit_layout_query(cg, e->tok, layout_name, field_name, false,
                                 true);
      }
      if (c->args.len != 1) {
        ny_diag_error(e->tok, "%s expects 1 argument", n);
        cg->had_error = 1;
        return LLVMConstInt(cg->type_i64, 1, false);
      }
      const char *layout_name = NULL;
      if (!layout_query_arg(c->args.data[0].val, &layout_name)) {
        ny_diag_error(e->tok, "%s expects a string literal", n);
        cg->had_error = 1;
        return LLVMConstInt(cg->type_i64, 1, false);
      }
      return emit_layout_query(cg, e->tok, layout_name, NULL, want_align,
                               false);
    }
  }
  if (mc && mc->name && strcmp(mc->name, "extern_all") == 0) {
    if (handle_extern_all_args(cg, &mc->args))
      return LLVMConstInt(cg->type_i64, 0, false);
  }
  if (mc) {
    char buf[128];
    const char *prefixes[] = {"dict_",  "list_", "str_",    "set_", "bytes_",
                              "queue_", "heap_", "bigint_", NULL};
    bool looked_like_module_target = false;
    const char *resolved_module_name = NULL;
    // sig_found declared above
    // Priority 1: Check if target is a module alias
    if (mc->target && mc->target->kind == NY_E_IDENT) {
      const char *target_name = mc->target->as.ident.name;
      const char *module_name = target_name;
      bool is_alias = false;
      for (size_t k = 0; k < cg->aliases.len; ++k) {
        if (strcmp(cg->aliases.data[k].name, target_name) == 0) {
          module_name = (const char *)cg->aliases.data[k].stmt_t;
          is_alias = true;
          break;
        }
      }
      // If it's an alias, it MUST be a module call.
      // If it's NOT an alias, check if it doesn't exist as a local
      // variable/function, in which case it might be a direct module usage
      // (e.g. math.add)
      if (is_alias || (lookup_fun(cg, target_name) == NULL &&
                       scope_lookup(scopes, depth, target_name) == NULL)) {
        looked_like_module_target = true;
        resolved_module_name = module_name;
        char dotted[256];
        snprintf(dotted, sizeof(dotted), "%s.%s", module_name, mc->name);
        sig_found = lookup_fun(cg, dotted);
        if (sig_found && !ny_gencall_sig_in_current_sigs(cg, sig_found))
          sig_found = NULL;
        if (sig_found) {
          ft = sig_found->type;
          fv = sig_found->value;
          sig_arity = sig_found->arity;
          is_variadic = sig_found->is_variadic;
          has_sig = true;
          skip_target = true;
          callee = fv;
          goto static_call_handling;
        }
        // If it was an ALIAS, but method not found, we shouldn't fall back to
        // standard methods
        if (is_alias) {
          ny_diag_error(e->tok, "function %s.%s not found", module_name,
                        mc->name);
          if (verbose_enabled >= 1)
            ny_diag_hint("alias '%s' resolves to module '%s'", target_name,
                         module_name);
          ny_diag_hint("make sure '%s' is exported from '%s'", mc->name,
                       module_name);
          cg->had_error = 1;
          return LLVMConstInt(cg->type_i64, 0, false);
        }
      }
    }
    // Priority 2: Check standard prefixes (dict_, list_, etc.)
    for (int i = 0; prefixes[i]; i++) {
      snprintf(buf, sizeof(buf), "%s%s", prefixes[i], mc->name);
      sig_found = lookup_fun(cg, buf);
      if (sig_found)
        break;
    }
    // Priority 3: Direct name
    if (!sig_found)
      sig_found = lookup_fun(cg, mc->name);
    if (sig_found && !ny_gencall_sig_in_current_sigs(cg, sig_found))
      sig_found = NULL;
  static_call_handling:;
    if (!sig_found) {
      /* Fallback: try dynamic property lookup (e.g. obj.method -> get(obj,
       * "method")) */
      fun_sig *getter = ny_gencall_getter(cg);
      if (getter && mc->name && strcmp(mc->name, "get") != 0) {
        LLVMValueRef target_val = gen_expr(cg, scopes, depth, mc->target);
        if (!target_val) {
          ny_diag_error(e->tok,
                        "failed to evaluate member call target for '%s'",
                        mc->name ? mc->name : "<unknown>");
          cg->had_error = 1;
          return LLVMConstInt(cg->type_i64, 0, false);
        }
        LLVMValueRef name_global =
            const_string_ptr(cg, mc->name, strlen(mc->name));
        LLVMValueRef name_ptr =
            LLVMBuildLoad2(cg->builder, cg->type_i64, name_global, "");
        ny_dbg_loc(cg, e->tok);
        callee = LLVMBuildCall2(cg->builder, getter->type, getter->value,
                                (LLVMValueRef[]){target_val, name_ptr}, 2,
                                "dyn_func");
        if (mc->args.len == 0) {
          return callee;
        }
        ft = NULL; /* Trigger generic call handling */
        has_sig = false;
        skip_target = true;
        goto skip_static_handling;
      }

      if (looked_like_module_target && resolved_module_name) {
        char dotted[256];
        snprintf(dotted, sizeof(dotted), "%s.%s", resolved_module_name,
                 mc->name);
        report_undef_symbol(cg, dotted, e->tok);
      } else {
        report_undef_symbol(cg, mc->name, e->tok);
      }
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    ft = sig_found->type;
    fv = sig_found->value;
    sig_arity = sig_found->arity;
    is_variadic = sig_found->is_variadic;
    has_sig = true;
    callee = fv;
  skip_static_handling:;
  } else {
    const char *name = (c && c->callee && c->callee->kind == NY_E_IDENT)
                           ? c->callee->as.ident.name
                           : NULL;
    if (name) {
      binding *b = scope_lookup(scopes, depth, name);
      if (b) {
        b->is_used = true;
        callee = b->is_slot
                     ? LLVMBuildLoad2(cg->builder, cg->type_i64, b->value, "")
                     : b->value;
      } else {
        binding *gb = lookup_global(cg, name);
        if (gb) {
          gb->is_used = true;
          callee = gb->is_slot ? LLVMBuildLoad2(cg->builder, cg->type_i64,
                                                gb->value, "")
                               : gb->value;
        }
      }
    }
    if (!callee) {
      sig_found = name ? resolve_overload(cg, name, c->args.len) : NULL;
      if (!sig_found && name)
        sig_found = lookup_use_module_fun(cg, name, c->args.len);
      if (!sig_found && name)
        sig_found = lookup_fun(cg, name);
      if (sig_found && !ny_gencall_sig_in_current_sigs(cg, sig_found))
        sig_found = NULL;
      if (sig_found) {
        ft = sig_found->type;
        fv = sig_found->value;
        sig_arity = sig_found->arity;
        is_variadic = sig_found->is_variadic;
        has_sig = true;
        callee = fv;
      } else {
        if (name) {
          fun_sig *globals_sig = ny_gencall_globals(cg);
          fun_sig *getter = ny_gencall_getter(cg);
          if (globals_sig && getter) {
            ny_dbg_loc(cg, e->tok);
            LLVMValueRef gtbl = LLVMBuildCall2(cg->builder, globals_sig->type,
                                               globals_sig->value, NULL, 0, "");
            LLVMValueRef name_global = const_string_ptr(cg, name, strlen(name));
            LLVMValueRef name_ptr =
                LLVMBuildLoad2(cg->builder, cg->type_i64, name_global, "");
            LLVMValueRef def_val =
                LLVMConstInt(cg->type_i64, 0, false); // default: none
            LLVMValueRef gargs[3] = {gtbl, name_ptr, def_val};
            unsigned gargc = getter->arity >= 3 ? 3 : 2;
            ny_dbg_loc(cg, e->tok);
            callee = LLVMBuildCall2(cg->builder, getter->type, getter->value,
                                    gargs, gargc, "dyn_global");
            ft = NULL;
            has_sig = false;
          } else {
            callee = gen_expr(cg, scopes, depth, c->callee);
          }
        } else {
          callee = gen_expr(cg, scopes, depth, c->callee);
        }
      }
    }
  }
  if (!ft) {
    size_t n = c ? c->args.len : (mc->args.len + 1);
    char buf[32];
    snprintf(buf, sizeof(buf), "__call%zu", n);
    fun_sig *rsig = lookup_fun(cg, buf);
    if (!rsig) {
      report_missing_runtime_call_helper(cg, e->tok, buf, n);
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    LLVMTypeRef rty = rsig->type;
    LLVMValueRef rval = rsig->value;
    if (!callee) {
      ny_diag_error(e->tok, "call target resolved to none");
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    LLVMValueRef callee_int =
        (LLVMTypeOf(callee) == cg->type_i64)
            ? callee
            : LLVMBuildPtrToInt(cg->builder, callee, cg->type_i64,
                                "callee_int");
    LLVMValueRef *call_args = malloc(sizeof(LLVMValueRef) * (n + 1));
    if (!call_args) {
      ny_diag_error(e->tok, "out of memory preparing dynamic call arguments");
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    call_args[0] = callee_int;
    if (c) {
      for (size_t i = 0; i < n; i++) {
        call_args[i + 1] = gen_expr(cg, scopes, depth, c->args.data[i].val);
        if (!call_args[i + 1]) {
          ny_diag_error(e->tok, "failed to evaluate argument %zu", i + 1);
          cg->had_error = 1;
          free(call_args);
          return LLVMConstInt(cg->type_i64, 0, false);
        }
      }
    } else {
      call_args[1] = gen_expr(cg, scopes, depth, mc->target);
      if (!call_args[1]) {
        ny_diag_error(e->tok, "failed to evaluate member call target argument");
        cg->had_error = 1;
        free(call_args);
        return LLVMConstInt(cg->type_i64, 0, false);
      }
      for (size_t i = 0; i < mc->args.len; i++) {
        call_args[i + 2] = gen_expr(cg, scopes, depth, mc->args.data[i].val);
        if (!call_args[i + 2]) {
          ny_diag_error(e->tok, "failed to evaluate argument %zu", i + 2);
          cg->had_error = 1;
          free(call_args);
          return LLVMConstInt(cg->type_i64, 0, false);
        }
      }
    }
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef res =
        LLVMBuildCall2(cg->builder, rty, rval, call_args, (unsigned)n + 1, "");
    free(call_args);
    return res;
  }
  size_t call_argc =
      c ? c->args.len : (skip_target ? mc->args.len : mc->args.len + 1);
  fun_sig sig_snapshot = {0};
  fun_sig *sig_meta = NULL;
  if (has_sig && sig_found) {
    if (ny_gencall_sig_in_current_sigs(cg, sig_found)) {
      sig_snapshot = *sig_found;
      sig_meta = &sig_snapshot;
    } else {
      has_sig = false;
      sig_found = NULL;
    }
  }

  if (has_sig) {
    if (!check_call_arity_diag(cg, e->tok, sig_meta, is_variadic, sig_arity,
                               call_argc, mc && !skip_target)) {
      return LLVMConstInt(cg->type_i64, 0, false);
    }
  }

  size_t sig_argc = (has_sig && is_variadic)
                        ? (size_t)sig_arity
                        : (has_sig ? (size_t)sig_arity : call_argc);
  size_t final_argc = (sig_argc > call_argc) ? sig_argc : call_argc;
  LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * final_argc);
  if (!args) {
    ny_diag_error(e->tok, "out of memory preparing call arguments");
    cg->had_error = 1;
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  size_t user_args_len = c ? c->args.len : mc->args.len;
  call_arg_t *user_args = c ? c->args.data : mc->args.data;
  expr_t *default_expr = NULL;
  ny_param_list *func_params = NULL;
  if (has_sig && sig_meta && sig_meta->stmt_t) {
    if (sig_meta->stmt_t->kind == NY_S_FUNC)
      func_params = &sig_meta->stmt_t->as.fn.params;
    else if (sig_meta->stmt_t->kind == NY_S_EXTERN)
      func_params = &sig_meta->stmt_t->as.ext.params;
  }
  for (size_t i = 0; i < final_argc; i++) {
    size_t user_idx = (mc && !skip_target) ? (i - 1) : i;
    const char *param_type = (func_params && i < func_params->len)
                                 ? func_params->data[i].type
                                 : NULL;
    expr_t *expr_for_check = NULL;
    if (mc && !skip_target && i == 0) {
      expr_for_check = mc->target;
      if (param_type && expr_for_check)
        ensure_expr_type_compatible(cg, scopes, depth, param_type,
                                    expr_for_check, expr_for_check->tok,
                                    "argument");
      args[i] = gen_expr(cg, scopes, depth, mc->target);
      if (!args[i]) {
        ny_diag_error(e->tok, "failed to evaluate member target argument");
        cg->had_error = 1;
        goto call_fail;
      }
    } else if (has_sig && is_variadic && i == (size_t)sig_arity - 1) {
      /* Variadic packaging: build list in-place to avoid append call chains. */
      fun_sig *ls_s = ny_gencall_list(cg);
      fun_sig *st_s = lookup_fun(cg, "__store64_idx");
      if (!ls_s || !st_s) {
        ny_diag_error(e->tok,
                      "variadic arguments require list/__store64_idx helpers");
        ny_diag_hint(
            "missing std.core imports for 'list' or runtime '__store64_idx'");
        cg->had_error = 1;
        goto call_fail;
      }
      LLVMTypeRef lty = ls_s->type, sty = st_s->type;
      LLVMValueRef lval = ls_s->value, sval = st_s->value;
      size_t var_count =
          (user_args_len > user_idx) ? (user_args_len - user_idx) : 0;
      size_t list_cap = var_count > 0 ? var_count : 1;
      uint64_t tagged_cap = (((uint64_t)list_cap) << 1) | 1u;
      ny_dbg_loc(cg, e->tok);
      LLVMValueRef vl = LLVMBuildCall2(
          cg->builder, lty, lval,
          (LLVMValueRef[]){LLVMConstInt(cg->type_i64, tagged_cap, false)}, 1,
          "");
      size_t out_i = 0;
      for (size_t j = user_idx; j < user_args_len; j++) {
        call_arg_t *a = &user_args[j];
        LLVMValueRef av = gen_expr(cg, scopes, depth, a->val);
        if (!av) {
          ny_diag_error(e->tok, "failed to evaluate variadic argument %zu",
                        j + 1);
          cg->had_error = 1;
          goto call_fail;
        }
        if (a->name) {
          fun_sig *ks_s = ny_gencall_kwarg(cg);
          if (!ks_s) {
            ny_diag_error(e->tok, "keyword args require '__kwarg'");
            ny_diag_hint("import std.core or call without keyword arguments");
            cg->had_error = 1;
            goto call_fail;
          }
          LLVMTypeRef kty = ks_s->type;
          LLVMValueRef kval = ks_s->value;
          LLVMValueRef name_runtime_global =
              const_string_ptr(cg, a->name, strlen(a->name));
          LLVMValueRef name_ptr = LLVMBuildLoad2(cg->builder, cg->type_i64,
                                                 name_runtime_global, "");
          ny_dbg_loc(cg, e->tok);
          av = LLVMBuildCall2(cg->builder, kty, kval,
                              (LLVMValueRef[]){name_ptr, av}, 2, "");
        }
        uint64_t tagged_off =
            ((((uint64_t)16 + (uint64_t)out_i * 8u) << 1) | 1u);
        ny_dbg_loc(cg, e->tok);
        (void)LLVMBuildCall2(
            cg->builder, sty, sval,
            (LLVMValueRef[]){vl, LLVMConstInt(cg->type_i64, tagged_off, false),
                             av},
            3, "");
        out_i++;
      }
      ny_dbg_loc(cg, e->tok);
      (void)LLVMBuildCall2(
          cg->builder, sty, sval,
          (LLVMValueRef[]){vl, LLVMConstInt(cg->type_i64, 1, false),
                           LLVMConstInt(cg->type_i64,
                                        ((((uint64_t)var_count) << 1) | 1u),
                                        false)},
          3, "");
      args[i] = vl;
      break;
    } else if (user_idx < user_args_len) {
      expr_for_check = user_args[user_idx].val;
      if (param_type && expr_for_check)
        ensure_expr_type_compatible(cg, scopes, depth, param_type,
                                    expr_for_check, expr_for_check->tok,
                                    "argument");
      args[i] = gen_expr(cg, scopes, depth, user_args[user_idx].val);
      if (!args[i]) {
        ny_diag_error(e->tok, "failed to evaluate argument %zu", i + 1);
        cg->had_error = 1;
        goto call_fail;
      }
    } else if (has_sig && sig_arity > (int)i && i < user_args_len) {
      args[i] = LLVMConstInt(cg->type_i64, 0, false); // none
    } else {
      default_expr = NULL;
      if (has_sig && sig_meta && sig_meta->stmt_t &&
          sig_meta->stmt_t->kind == NY_S_FUNC) {
        func_params = &sig_meta->stmt_t->as.fn.params;
        size_t param_idx = i;
        if (param_idx < func_params->len) {
          default_expr = func_params->data[param_idx].def;
        }
      }
      if (default_expr) {
        if (param_type)
          ensure_expr_type_compatible(cg, scopes, depth, param_type,
                                      default_expr, default_expr->tok,
                                      "argument");
        args[i] = gen_expr(cg, scopes, depth, default_expr);
        if (!args[i]) {
          ny_diag_error(e->tok, "failed to evaluate default argument %zu",
                        i + 1);
          cg->had_error = 1;
          goto call_fail;
        }
      } else {
        args[i] = LLVMConstInt(cg->type_i64, 0, false); // none
      }
    }
  }
  if (has_sig) {
    /* const char *callee_name = (c && c->callee->kind == NY_E_IDENT) ?
     * c->callee->as.ident.name : (mc ? mc->name : "ptr"); */
    /* fprintf(stderr, "DEBUG: Call gen '%s' - is_variadic: %d, sig_arity: %d,
     * call_argc: %zu\n", callee_name, is_variadic, sig_arity, c ? c->args.len
     * : mc->args.len); */
  }
  if (has_sig && sig_meta && sig_meta->is_extern) {
    func_params = NULL;
    if (sig_meta->stmt_t && sig_meta->stmt_t->kind == NY_S_EXTERN) {
      func_params = &sig_meta->stmt_t->as.ext.params;
    }
    if (func_params && func_params->len > 0) {
      size_t max_conv = func_params->len;
      size_t call_limit =
          (has_sig && is_variadic) ? (size_t)sig_arity : final_argc;
      if (max_conv > call_limit)
        max_conv = call_limit;
      for (size_t i = 0; i < max_conv; i++) {
        if (sig_meta->is_variadic && (int)i >= sig_arity - 1)
          break;
        const char *tname = func_params->data[i].type;
        if (tname && *tname) {
          args[i] = coerce_extern_arg(cg, args[i], tname);
        }
      }
    }
  }
  if (has_sig && ny_gencall_is_thread_attr(sig_meta)) {
    if (is_variadic || final_argc > 15) {
      ny_diag_error(e->tok, "@thread call '%s' supports up to 15 arguments",
                    sig_meta->name ? sig_meta->name : "<anon>");
      ny_diag_hint("reduce arguments or pass a packed object");
      cg->had_error = 1;
      free(args);
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    LLVMTypeRef ret_ty = LLVMGetReturnType(ft);
    if (ret_ty != cg->type_i64) {
      ny_diag_error(e->tok,
                    "@thread function '%s' must return tagged int/any (i64)",
                    sig_meta->name ? sig_meta->name : "<anon>");
      cg->had_error = 1;
      free(args);
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    bool detach_stmt_call = cg->thread_detach_stmt_call;
    fun_sig *spawn_sig =
        detach_stmt_call ? NULL : lookup_fun(cg, "__thread_spawn_call");
    fun_sig *launch_sig =
        detach_stmt_call ? lookup_fun(cg, "__thread_launch_call") : NULL;
    fun_sig *join_sig =
        detach_stmt_call ? NULL : lookup_fun(cg, "__thread_join");
    if ((!detach_stmt_call && (!spawn_sig || !join_sig)) ||
        (detach_stmt_call && !launch_sig)) {
      ny_diag_error(e->tok, "missing runtime thread helpers");
      if (detach_stmt_call) {
        ny_diag_hint("expected __thread_launch_call in runtime symbols");
      } else {
        ny_diag_hint("expected __thread_spawn_call/__thread_join in runtime "
                     "symbols");
      }
      cg->had_error = 1;
      free(args);
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    LLVMValueRef fn_val =
        (LLVMTypeOf(callee) == cg->type_i64)
            ? callee
            : LLVMBuildPtrToInt(cg->builder, callee, cg->type_i64, "thread_fn");

    LLVMValueRef argc_val =
        LLVMConstInt(cg->type_i64, (((uint64_t)final_argc << 1) | 1), false);
    LLVMValueRef argv_ptr = LLVMConstInt(cg->type_i64, 0, false);
    if (final_argc > 0) {
      LLVMTypeRef argv_ty = LLVMArrayType(cg->type_i64, (unsigned)final_argc);
      ny_dbg_loc(cg, e->tok);
      LLVMValueRef argv_stack =
          LLVMBuildAlloca(cg->builder, argv_ty, "thread_argv");
      for (size_t i = 0; i < final_argc; i++) {
        LLVMValueRef idxs[2] = {LLVMConstInt(cg->type_i64, 0, false),
                                LLVMConstInt(cg->type_i64, (uint64_t)i, false)};
        LLVMValueRef slot =
            LLVMBuildGEP2(cg->builder, argv_ty, argv_stack, idxs, 2, "");
        LLVMBuildStore(cg->builder, args[i], slot);
      }
      argv_ptr = LLVMBuildPtrToInt(cg->builder, argv_stack, cg->type_i64,
                                   "thread_argv_ptr");
    }

    ny_dbg_loc(cg, e->tok);
    LLVMValueRef handle = NULL;
    if (detach_stmt_call) {
      LLVMBuildCall2(cg->builder, launch_sig->type, launch_sig->value,
                     (LLVMValueRef[]){fn_val, argc_val, argv_ptr}, 3,
                     "thread_launch");
    } else {
      handle = LLVMBuildCall2(cg->builder, spawn_sig->type, spawn_sig->value,
                              (LLVMValueRef[]){fn_val, argc_val, argv_ptr}, 3,
                              "thread_spawn");
    }
    if (detach_stmt_call) {
      free(args);
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef joined =
        LLVMBuildCall2(cg->builder, join_sig->type, join_sig->value, &handle, 1,
                       "thread_join");
    free(args);
    return joined;
  }
  unsigned call_nargs =
      (unsigned)(has_sig && is_variadic ? (size_t)sig_arity : final_argc);
  if (!ft || !callee) {
    ny_diag_error(e->tok, "invalid call target");
    cg->had_error = 1;
    free(args);
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  LLVMValueRef res = 0;
  bool memo_alias_safe = has_sig && sig_meta && !sig_meta->args_escape &&
                         !sig_meta->args_mutated && !sig_meta->returns_alias;
  bool memo_impure_effect_safe =
      has_sig && sig_meta && sig_meta->effects_known &&
      (sig_meta->effects & (NY_FX_IO | NY_FX_FFI | NY_FX_THREAD)) == 0;
  bool impure_return_ok = has_sig && sig_meta &&
                          ny_memo_impure_return_allowed(sig_meta->return_type);
  bool memo_for_impure = has_sig && sig_meta && !sig_meta->is_pure &&
                         cg->auto_memoize_impure && sig_meta->is_memo_safe &&
                         memo_alias_safe && memo_impure_effect_safe &&
                         impure_return_ok;
  bool memo_eligible = has_sig && sig_meta && memo_alias_safe &&
                       (sig_meta->is_pure || memo_for_impure);
  if (cg->auto_memoize && memo_eligible && !sig_meta->is_extern &&
      !sig_meta->is_variadic && !sig_meta->is_recursive &&
      !ny_gencall_is_thread_attr(sig_meta)) {
    res = ny_build_memoized_direct_call(cg, e->tok, ft, callee, args,
                                        call_nargs, memo_for_impure);
  } else {
    ny_dbg_loc(cg, e->tok);
    res = LLVMBuildCall2(cg->builder, ft, callee, args, call_nargs, "");
  }
  free(args);
  if (has_sig && sig_meta && sig_meta->is_extern) {
    LLVMTypeRef ret_ty = LLVMGetReturnType(ft);
    if (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind)
      return LLVMConstInt(cg->type_i64, 0, false);
    return box_extern_result(cg, res, sig_meta->return_type);
  }
  return res;

call_fail:
  free(args);
  return LLVMConstInt(cg->type_i64, 0, false);
}
