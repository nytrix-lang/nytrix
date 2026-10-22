/*
 * Handles expression-level type dispatch in the LLVM codegen layer.
 * It determines the type-specific lowering path for each expression node.
 *
 * The tagged-int fast/slow path dispatch (see
 * src/code/lang/llvm/binary.c) is invoked from this layer when
 * lowering binary operator expressions.  The dispatch logic here selects
 * between inline tagged-int LLVM IR and BigInt-allocating stdlib calls based on
 * type provenance facts collected during semantic analysis.
 *
 * Unproven binary operations use a PHI with fast and slow basic blocks.
 * Monomorphized raw-int lowering retains the tag/range guard and falls back
 * to ordinary tagged semantics whenever that guard fails.
 *
 * Comptime fuel: the fast evaluator and AST interpreter share the
 * compilation-wide counter initialized from NYTRIX_COMPTIME_FUEL (default
 * 1 000 000 steps). Exhaustion is a compile error before the JIT fallback is
 * considered; it must not become a runtime value or warning-only fallback.
 */
#include "code/lang/types/pipeline.h"
#include <inttypes.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>

#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#ifdef NYTRIX_HAS_Z3
#include <z3.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

LLVMValueRef expr_fail(codegen_t *cg, token_t tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char msg[512];
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  ny_diag_error(tok, "%s", msg);
  cg->had_error = 1;
  return ny_c0(cg);
}

static inline uint64_t ny_const_str_hash(const char *s, size_t len) {
  return ny_hash64(s, len);
}

static binding *expr_lookup_source_global(codegen_t *cg, const char *name,
                                          token_t tok) {
  if (!cg || !name || !*name || !tok.filename)
    return NULL;
  for (size_t i = 0; i < cg->global_vars.len; ++i) {
    binding *candidate = &cg->global_vars.data[i];
    if (!candidate->name || !candidate->stmt_t ||
        !candidate->stmt_t->tok.filename ||
        strcmp(candidate->stmt_t->tok.filename, tok.filename) != 0)
      continue;
    const char *dot = strrchr(candidate->name, '.');
    if (strcmp(dot ? dot + 1 : candidate->name, name) == 0)
      return candidate;
  }
  return NULL;
}

static LLVMValueRef ny_binding_tagged_int_value(codegen_t *cg, binding *b);

static LLVMValueRef expr_value_from_binding(codegen_t *cg, binding *b) {
  if (!cg || !b)
    return ny_c0(cg);
  b->is_used = true;
  if (b->is_f64_slot || b->is_f64_direct || b->is_f32_slot ||
      b->is_f32_direct) {
    LLVMValueRef fv;
    if (b->is_f64_slot || b->is_f64_direct) {
      fv = b->is_slot
               ? LLVMBuildLoad2(cg->builder, cg->type_f64, b->value, "f64_ld")
               : b->value;
    } else {
      fv = b->is_slot
               ? LLVMBuildLoad2(cg->builder, cg->type_f32, b->value, "f32_ld")
               : b->value;
      fv = LLVMBuildFPExt(cg->builder, fv, cg->type_f64, "f2f");
    }
    fun_sig *box_sig = lookup_fun(cg, "__flt_box_val", 0);
    if (box_sig)
      return LLVMBuildCall2(
          cg->builder, box_sig->type, box_sig->value,
          (LLVMValueRef[]){ny_bitcast(cg, fv, cg->type_i64, "")}, 1, "box");
  }
  if (b->is_c_abi_global && b->is_slot) {
    LLVMTypeRef stored_ty = LLVMGlobalGetValueType(b->value);
    LLVMValueRef raw =
        LLVMBuildLoad2(cg->builder, stored_ty, b->value, "c_global_load");
    LLVMTypeKind kind = LLVMGetTypeKind(stored_ty);
    if (kind == LLVMPointerTypeKind)
      return ny_ptr2i64(cg, raw, "c_global_ptr");
    if (kind == LLVMIntegerTypeKind) {
      unsigned bits = LLVMGetIntTypeWidth(stored_ty);
      if (bits < 64)
        raw =
            b->is_c_abi_unsigned
                ? LLVMBuildZExt(cg->builder, raw, cg->type_i64, "c_global_zext")
                : LLVMBuildSExt(cg->builder, raw, cg->type_i64,
                                "c_global_sext");
      else if (bits > 64)
        raw = LLVMBuildTrunc(cg->builder, raw, cg->type_i64, "c_global_trunc");
      return ny_tag_int(cg, raw);
    }
  }
  if (b->is_slot)
    return ny_load(cg, b->value, "");
  if (b->is_int_direct && b->is_int_raw_direct)
    return ny_binding_tagged_int_value(cg, b);
  return b->value;
}

bool ny_legacy_sig_param_is_tagged_dynamic(const fun_sig *sig, size_t index) {
  if (!sig || sig->arity < 0 || index >= (size_t)sig->arity)
    return false;
  const param_t *param = NULL;
  if (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC &&
      index < sig->stmt_t->as.fn.params.len)
    param = &sig->stmt_t->as.fn.params.data[index];
  else if (sig->stmt_t && sig->stmt_t->kind == NY_S_EXTERN &&
           index < sig->stmt_t->as.ext.params.len)
    param = &sig->stmt_t->as.ext.params.data[index];
  /*
   * `any` is always a tagged dynamic value, even when older imported
   * signatures carry a stale resolved representation fact.
   */
  if (param && ny_type_name_is_any(param->type))
    return true;
  if (param && param->semantic.resolved) {
    bool result = param->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC;
    return result;
  }
  const char *type_name =
      param ? param->type : ny_sig_param_type((fun_sig *)sig, index);
  bool result =
      ny_semantic_rep_for_type(type_name) == NY_SEM_REP_TAGGED_DYNAMIC;
  return result;
}

static bool ny_legacy_expr_is_bigint_to_int_call(expr_t *expr) {
  return expr && expr->kind == NY_E_CALL && expr->as.call.callee &&
         expr->as.call.callee->kind == NY_E_IDENT &&
         expr->as.call.callee->as.ident.name &&
         strcmp(expr->as.call.callee->as.ident.name, "__bigint_to_int") == 0;
}

bool ny_legacy_expr_is_raw_int_arg(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *expr) {
  if (!cg || !expr)
    return false;
  /*
   * This bridge is explicitly declared as a raw-i64 result.  Its historical
   * declaration omitted the return type, leaving older semantic metadata
   * unable to distinguish it from an already tagged dynamic value.
   */
  if (ny_legacy_expr_is_bigint_to_int_call(expr))
    return true;
  /*
   * A statically promoted integer expression already yields a tagged BigInt
   * handle in legacy LLVM.  Semantic `int` provenance alone cannot make that
   * runtime value a raw i64 argument.
   */
  if (ny_legacy_expr_is_static_bigintish(cg, scopes, depth, expr, 0))
    return false;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    size_t name_len = (size_t)expr->tok.len;
    if (name_len == 0)
      name_len = strlen(expr->as.ident.name);
    binding *binding = lookup_binding_hash(
        cg, scopes, depth, expr->as.ident.name, name_len, expr->as.ident.hash);
    expr_t *init = binding && !binding->is_mut
                       ? ny_binding_var_init_expr(binding, expr->as.ident.name)
                       : NULL;
    if (ny_legacy_expr_is_bigint_to_int_call(init))
      return true;
    bool result = binding && binding->is_int_direct &&
                  binding->is_int_raw_direct && binding->raw_int_value;
    return result;
  }
  /*
   * Integer operators produce tagged dynamic values (and may promote), even
   * when their source-level semantic type is `int`. Only typed call results
   * have a raw i64 ABI here; boxing an operator result would encode the
   * already-tagged word a second time.
   */
  return expr->kind == NY_E_CALL && expr->semantic.resolved &&
         expr->semantic.rep == NY_SEM_REP_RAW_INT;
}

static const char *expr_member_module_alias_target(codegen_t *cg, scope *scopes,
                                                   size_t depth, expr_t *e) {
  if (!cg || !e || e->kind != NY_E_MEMBER || !e->as.member.target ||
      e->as.member.target->kind != NY_E_IDENT ||
      !e->as.member.target->as.ident.name || !e->as.member.name)
    return NULL;
  expr_t *target = e->as.member.target;
  const char *target_name = target->as.ident.name;
  size_t target_len = (size_t)target->tok.len;
  if (target_len == 0)
    target_len = strlen(target_name);
  return ny_lookup_module_alias(cg, scopes, depth, target_name, target_len,
                                target->as.ident.hash);
}

static binding *expr_member_module_alias_global(codegen_t *cg, scope *scopes,
                                                size_t depth, expr_t *e,
                                                char *out, size_t out_cap) {
  const char *module_name =
      expr_member_module_alias_target(cg, scopes, depth, e);
  if (!module_name)
    return NULL;
  if (!e || !e->as.member.name)
    return NULL;
  {
    char stack_name[512];
    char *name_buf = out && out_cap > 0 ? out : stack_name;
    size_t name_cap = out && out_cap > 0 ? out_cap : sizeof(stack_name);
    int nw =
        snprintf(name_buf, name_cap, "%s.%s", module_name, e->as.member.name);
    if (nw <= 0 || (size_t)nw >= name_cap)
      return NULL;
    binding *gb = lookup_global(cg, name_buf);
    if (!gb) {
      const char *resolved = resolve_import_alias(cg, name_buf);
      if (resolved && *resolved && strcmp(resolved, name_buf) != 0)
        gb = lookup_global(cg, resolved);
    }
    if (!gb) {
      const char *resolved = ny_resolve_used_module_export_alias(cg, name_buf);
      if (resolved && *resolved && strcmp(resolved, name_buf) != 0)
        gb = lookup_global(cg, resolved);
    }
    return gb;
  }
}

static LLVMValueRef expr_build_untagged_or_raw_i64(codegen_t *cg,
                                                   LLVMValueRef v,
                                                   const char *name) {
  LLVMValueRef lsb = ny_and(cg, v, ny_c1(cg), "index_lsb");
  LLVMValueRef is_tagged = ny_eq(cg, lsb, ny_c1(cg), "index_is_tagged");
  LLVMValueRef untagged = ny_ashr(cg, v, ny_c1(cg), "index_untag");
  return ny_select(cg, is_tagged, untagged, v, name ? name : "index_raw");
}

static bool expr_type_base_is(const char *type_name, const char *want) {
  if (!type_name || !want)
    return false;
  if (*type_name == '?')
    return false;
  const char *leaf = ny_type_leaf(type_name);
  if (!leaf)
    return false;
  size_t want_len = strlen(want);
  if (strncmp(leaf, want, want_len) != 0)
    return false;
  return leaf[want_len] == '\0' || leaf[want_len] == '<';
}

static bool expr_type_is_int_index(const char *type_name) {
  static const char *const k_int_types[] = {
      "int", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
  };
  for (size_t i = 0; i < sizeof(k_int_types) / sizeof(k_int_types[0]); ++i) {
    if (expr_type_base_is(type_name, k_int_types[i]))
      return true;
  }
  return false;
}

static bool expr_index_is_int_key(codegen_t *cg, scope *scopes, size_t depth,
                                  expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      e->tok.kind != NY_T_NIL)
    return true;
  if (ny_is_proven_int(cg, scopes, depth, e, NULL))
    return true;
  return expr_type_is_int_index(infer_expr_type(cg, scopes, depth, e));
}

static bool expr_target_is_known_list_like(codegen_t *cg, scope *scopes,
                                           size_t depth, expr_t *target) {
  if (!target)
    return false;
  if (target->kind == NY_E_LIST || target->kind == NY_E_TUPLE)
    return true;
  const char *type_name = infer_expr_type(cg, scopes, depth, target);
  if (expr_type_base_is(type_name, "list") ||
      expr_type_base_is(type_name, "tuple"))
    return true;
  if (target->kind == NY_E_IDENT && target->as.ident.name) {
    size_t name_len = (size_t)target->tok.len;
    if (name_len == 0)
      name_len = strlen(target->as.ident.name);
    binding *b = lookup_binding_hash(cg, scopes, depth, target->as.ident.name,
                                     name_len, target->as.ident.hash);
    return b && b->is_list_storage;
  }
  return false;
}

static bool expr_target_is_known_string_like(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *target) {
  if (!target)
    return false;
  if (target->kind == NY_E_LITERAL && target->as.literal.kind == NY_LIT_STR)
    return true;
  const char *type_name = infer_expr_type(cg, scopes, depth, target);
  if (type_name && expr_type_base_is(type_name, "str"))
    return true;
  if (target->kind == NY_E_IDENT && target->as.ident.name) {
    size_t name_len = (size_t)target->tok.len;
    if (name_len == 0)
      name_len = strlen(target->as.ident.name);
    binding *b = lookup_binding_hash(cg, scopes, depth, target->as.ident.name,
                                     name_len, target->as.ident.hash);
    if (b && b->type_name && expr_type_base_is(b->type_name, "str"))
      return true;
    expr_t *init =
        b ? ny_binding_var_init_expr(b, target->as.ident.name) : NULL;
    return init && init->kind == NY_E_LITERAL &&
           init->as.literal.kind == NY_LIT_STR;
  }
  return false;
}

static bool expr_negative_literal_i64(expr_t *e, int64_t *out) {
  if (!e || e->kind != NY_E_UNARY || !e->as.unary.op ||
      strcmp(e->as.unary.op, "-") != 0 || !e->as.unary.right)
    return false;
  int64_t value = 0;
  if (!ny_expr_literal_i64(e->as.unary.right, &value) || value < 0)
    return false;
  if (out)
    *out = -value;
  return true;
}

static binding *expr_f64_list_target_binding(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *target) {
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
    return NULL;
  binding *b = ny_static_int_list_target_binding(cg, scopes, depth, target);
  return (b && ((b->is_list_storage && b->is_f64_list_storage) ||
                (b->raw_f64_list_mutation && b->raw_f64_list_ptr &&
                 b->raw_f64_list_len > 0)))
             ? b
             : NULL;
}

static bool expr_is_f64_default_value(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *e) {
  if (!e)
    return true;
  if (ny_is_proven_int(cg, scopes, depth, e, NULL))
    return true;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_FLOAT;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = lookup_binding_hash(cg, scopes, depth, e->as.ident.name,
                                     name_len, e->as.ident.hash);
    if (b && (b->is_f64_slot || b->is_f64_direct || b->is_f32_slot ||
              b->is_f32_direct))
      return true;
  }
  const char *t = infer_expr_type(cg, scopes, depth, e);
  return t && (strcmp(t, "f64") == 0 || strcmp(t, "float") == 0 ||
               strcmp(t, "f32") == 0);
}

static LLVMValueRef expr_index_raw_i64(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *idx_expr,
                                       LLVMValueRef idx_v, const char *name) {
  idx_v = ny_llvm_cast_to_i64(cg, idx_v, name ? name : "index");
  int64_t lit = 0;
  if (ny_expr_literal_i64(idx_expr, &lit))
    return LLVMConstInt(cg->type_i64, (uint64_t)lit, true);
  if (idx_expr && idx_expr->kind == NY_E_IDENT && idx_expr->as.ident.name) {
    size_t name_len = (size_t)idx_expr->tok.len;
    if (name_len == 0)
      name_len = strlen(idx_expr->as.ident.name);
    binding *b = lookup_binding_hash(cg, scopes, depth, idx_expr->as.ident.name,
                                     name_len, idx_expr->as.ident.hash);
    if (b && b->raw_int_value && b->is_int_direct)
      return b->raw_int_value;
    if ((cg->opt_enabled || ny_env_enabled("NYTRIX_RAW_INT_SLOT_EXPR_FAST")) &&
        b && b->raw_int_value && b->is_int_slot && !ny_binding_is_valid(cg, b))
      return ny_load(cg, b->raw_int_value, name ? name : "index_raw");
  }
  if (ny_is_proven_int(cg, scopes, depth, idx_expr, idx_v))
    return ny_untag_int(cg, idx_v);
  return expr_build_untagged_or_raw_i64(cg, idx_v, name ? name : "index_raw");
}

static bool expr_int_range(codegen_t *cg, scope *scopes, size_t depth,
                           expr_t *e, int64_t *out_min, int64_t *out_max);

static bool expr_list_len_min(codegen_t *cg, scope *scopes, size_t depth,
                              expr_t *target, int64_t *out_min_len) {
  if (!target)
    return false;
  if (target->kind == NY_E_LIST || target->kind == NY_E_TUPLE) {
    if (out_min_len)
      *out_min_len = (int64_t)target->as.list_like.len;
    return true;
  }
  if (target->kind != NY_E_IDENT || !target->as.ident.name)
    return false;
  size_t name_len = (size_t)target->tok.len;
  if (name_len == 0)
    name_len = strlen(target->as.ident.name);
  binding *b = lookup_binding_hash(cg, scopes, depth, target->as.ident.name,
                                   name_len, target->as.ident.hash);
  if (!b)
    return false;
  if (b->has_list_len_min) {
    if (out_min_len)
      *out_min_len = b->list_len_min_raw;
    return true;
  }
  expr_t *init =
      !b->is_mut ? ny_binding_var_init_expr(b, target->as.ident.name) : NULL;
  if (init && (init->kind == NY_E_LIST || init->kind == NY_E_TUPLE)) {
    if (out_min_len)
      *out_min_len = (int64_t)init->as.list_like.len;
    return true;
  }
  return false;
}

static bool expr_int_range(codegen_t *cg, scope *scopes, size_t depth,
                           expr_t *e, int64_t *out_min, int64_t *out_max) {
  int64_t lit = 0;
  if (ny_expr_literal_i64(e, &lit)) {
    if (out_min)
      *out_min = lit;
    if (out_max)
      *out_max = lit;
    return true;
  }
  if (e && e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name && e->as.call.args.len == 1) {
    size_t name_len = 0;
    uint64_t name_hash = 0;
    const char *name = ny_builtin_surface_name_for_callee(
        e->as.call.callee, &name_len, &name_hash);
    bool shadowed = ny_builtin_name_shadowed_by_user_symbol(
        cg, scopes, depth, name, name_len, name_hash);
    if (name && !shadowed && ny_name_tail_is(name, "len")) {
      int64_t len = 0;
      if (expr_list_len_min(cg, scopes, depth, e->as.call.args.data[0].val,
                            &len)) {
        if (out_min)
          *out_min = len;
        if (out_max)
          *out_max = len;
        return true;
      }
    }
  }
  if (e && e->kind == NY_E_BINARY && e->as.binary.op) {
    int64_t lmin = 0, lmax = 0, rmin = 0, rmax = 0;
    if (!expr_int_range(cg, scopes, depth, e->as.binary.left, &lmin, &lmax) ||
        !expr_int_range(cg, scopes, depth, e->as.binary.right, &rmin, &rmax))
      return false;
    const char *op = e->as.binary.op;
    int64_t lo = 0, hi = 0;
    if (strcmp(op, "+") == 0) {
      if (!ny_add_range_ok(lmin, rmin, &lo) ||
          !ny_add_range_ok(lmax, rmax, &hi))
        return false;
    } else if (strcmp(op, "-") == 0) {
      if (!ny_sub_range_ok(lmin, rmax, &lo) ||
          !ny_sub_range_ok(lmax, rmin, &hi))
        return false;
    } else if (strcmp(op, "*") == 0) {
      int64_t c[4];
      if (!ny_mul_range_ok(lmin, rmin, &c[0]) ||
          !ny_mul_range_ok(lmin, rmax, &c[1]) ||
          !ny_mul_range_ok(lmax, rmin, &c[2]) ||
          !ny_mul_range_ok(lmax, rmax, &c[3]))
        return false;
      lo = c[0];
      hi = c[0];
      for (int i = 1; i < 4; ++i) {
        if (c[i] < lo)
          lo = c[i];
        if (c[i] > hi)
          hi = c[i];
      }
    } else if (strcmp(op, "%") == 0) {
      if (rmin != rmax || rmax <= 0 || lmin < 0)
        return false;
      lo = 0;
      hi = lmax < rmax ? lmax : rmax - 1;
    } else if (strcmp(op, "&") == 0) {
      if (rmin != rmax || rmax < 0 || lmin < 0)
        return false;
      lo = 0;
      hi = rmax;
    } else if (strcmp(op, ">>") == 0) {
      if (rmin != rmax || rmin < 0 || rmin >= 64 || lmin < 0)
        return false;
      unsigned shift = (unsigned)rmin;
      lo = (int64_t)((uint64_t)lmin >> shift);
      hi = (int64_t)((uint64_t)lmax >> shift);
    } else {
      return false;
    }
    if (lo > hi)
      return false;
    if (out_min)
      *out_min = lo;
    if (out_max)
      *out_max = hi;
    return true;
  }
  if (!e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  size_t name_len = (size_t)e->tok.len;
  if (name_len == 0)
    name_len = strlen(e->as.ident.name);
  binding *b = lookup_binding_hash(cg, scopes, depth, e->as.ident.name,
                                   name_len, e->as.ident.hash);
  if (b && b->has_int_range) {
    if (out_min)
      *out_min = b->int_min_raw;
    if (out_max)
      *out_max = b->int_max_raw;
    return true;
  }
  expr_t *init =
      b && !b->is_mut ? ny_binding_var_init_expr(b, e->as.ident.name) : NULL;
  if (ny_expr_literal_i64(init, &lit)) {
    if (out_min)
      *out_min = lit;
    if (out_max)
      *out_max = lit;
    return true;
  }
  return init && init != e &&
         expr_int_range(cg, scopes, depth, init, out_min, out_max);
}

static bool expr_index_is_nonnegative(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *key) {
  int64_t idx_min = 0, idx_max = 0;
  if (expr_int_range(cg, scopes, depth, key, &idx_min, &idx_max))
    return idx_min >= 0;
  const char *t = infer_expr_type(cg, scopes, depth, key);
  return t && (expr_type_base_is(t, "u8") || expr_type_base_is(t, "u16") ||
               expr_type_base_is(t, "u32") || expr_type_base_is(t, "u64"));
}

static bool expr_index_in_list_len_min(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *target,
                                       expr_t *key) {
  int64_t idx_min = 0, idx_max = 0, len_min = 0;
  if (!expr_int_range(cg, scopes, depth, key, &idx_min, &idx_max) ||
      idx_min < 0)
    return false;
  if (!expr_list_len_min(cg, scopes, depth, target, &len_min))
    return false;
  return len_min > 0 && idx_max < len_min;
}

static bool ny_lambda_values_need_closure(codegen_t *cg) {
  return ny_env_enabled("NYTRIX_FORCE_CLOSURE_LAMBDAS") ||
         ny_module_target_is_apple_arm64(cg ? cg->module : NULL);
}

bool ny_try_eval_comptime_expr_fast(codegen_t *cg, expr_t *e,
                                    ny_ct_fast_val_t *out, int depth);
bool ny_try_eval_comptime_fast_value(codegen_t *cg, stmt_t *body,
                                     ny_ct_fast_val_t *out);

static LLVMValueRef ny_try_host_platform_ident(codegen_t *cg,
                                               const char *name) {
  if (!cg || !name)
    return NULL;

  ny_ct_fast_val_t v = {0};
  if (!ny_ct_platform_ident(name, &v))
    return NULL;
  LLVMValueRef tag_true = ny_ctrue(cg);
  LLVMValueRef tag_false = ny_cfalse(cg);

  if (v.kind == NY_CT_FAST_STR) {
    LLVMValueRef g =
        const_string_ptr(cg, v.s ? v.s : "", strlen(v.s ? v.s : ""));
    return ny_load(cg, g, NY_LLVM_NAME(cg, "host_os"));
  }
  if (v.kind == NY_CT_FAST_BOOL)
    return v.b ? tag_true : tag_false;
  return NULL;
}

static bool closure_param_list_shadows_name(const ny_param_list *params,
                                            const char *name) {
  if (!params || !name)
    return false;
  for (size_t i = 0; i < params->len; ++i) {
    if (params->data[i].name && strcmp(params->data[i].name, name) == 0)
      return true;
  }
  return false;
}

static bool closure_stmt_contains_ident_name(stmt_t *s, const char *name);

static bool closure_expr_contains_ident_name(expr_t *e, const char *name) {
  if (!e || !name)
    return false;
  switch (e->kind) {
  case NY_E_IDENT:
    return ny_expr_ident_is_name(e, name);
  case NY_E_UNARY:
    return closure_expr_contains_ident_name(e->as.unary.right, name);
  case NY_E_BINARY:
    return closure_expr_contains_ident_name(e->as.binary.left, name) ||
           closure_expr_contains_ident_name(e->as.binary.right, name);
  case NY_E_LOGICAL:
    return closure_expr_contains_ident_name(e->as.logical.left, name) ||
           closure_expr_contains_ident_name(e->as.logical.right, name);
  case NY_E_TERNARY:
    return closure_expr_contains_ident_name(e->as.ternary.cond, name) ||
           closure_expr_contains_ident_name(e->as.ternary.true_expr, name) ||
           closure_expr_contains_ident_name(e->as.ternary.false_expr, name);
  case NY_E_CALL:
    if (closure_expr_contains_ident_name(e->as.call.callee, name))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (closure_expr_contains_ident_name(e->as.call.args.data[i].val, name))
        return true;
    }
    return false;
  case NY_E_MEMCALL:
    if (closure_expr_contains_ident_name(e->as.memcall.target, name))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i) {
      if (closure_expr_contains_ident_name(e->as.memcall.args.data[i].val,
                                           name))
        return true;
    }
    return false;
  case NY_E_INDEX:
    return closure_expr_contains_ident_name(e->as.index.target, name) ||
           closure_expr_contains_ident_name(e->as.index.start, name) ||
           closure_expr_contains_ident_name(e->as.index.stop, name) ||
           closure_expr_contains_ident_name(e->as.index.step, name);
  case NY_E_MEMBER:
    return closure_expr_contains_ident_name(e->as.member.target, name);
  case NY_E_PTR_TYPE:
    return closure_expr_contains_ident_name(e->as.ptr_type.target, name);
  case NY_E_DEREF:
    return closure_expr_contains_ident_name(e->as.deref.target, name);
  case NY_E_SIZEOF:
    return !e->as.szof.is_type &&
           closure_expr_contains_ident_name(e->as.szof.target, name);
  case NY_E_TRY:
    return closure_expr_contains_ident_name(e->as.try_expr.target, name);
  case NY_E_LAMBDA:
  case NY_E_FN:
    for (size_t i = 0; i < e->as.lambda.params.len; ++i) {
      if (closure_expr_contains_ident_name(e->as.lambda.params.data[i].def,
                                           name))
        return true;
    }
    if (closure_param_list_shadows_name(&e->as.lambda.params, name))
      return false;
    return closure_stmt_contains_ident_name(e->as.lambda.body, name);
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i) {
      if (closure_expr_contains_ident_name(e->as.as_asm.args.data[i], name))
        return true;
    }
    return false;
  case NY_E_COMPTIME:
    return closure_stmt_contains_ident_name(e->as.comptime_expr.body, name);
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR &&
          closure_expr_contains_ident_name(part->as.e, name))
        return true;
    }
    return false;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      if (closure_expr_contains_ident_name(e->as.list_like.data[i], name))
        return true;
    }
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      if (closure_expr_contains_ident_name(e->as.dict.pairs.data[i].key,
                                           name) ||
          closure_expr_contains_ident_name(e->as.dict.pairs.data[i].value,
                                           name))
        return true;
    }
    return false;
  case NY_E_MATCH:
    if (closure_expr_contains_ident_name(e->as.match.test, name))
      return true;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t p = 0; p < arm->patterns.len; ++p) {
        if (closure_expr_contains_ident_name(arm->patterns.data[p], name))
          return true;
      }
      if (closure_expr_contains_ident_name(arm->guard, name) ||
          closure_stmt_contains_ident_name(arm->conseq, name))
        return true;
    }
    return closure_stmt_contains_ident_name(e->as.match.default_conseq, name);
  default:
    return false;
  }
}

static bool closure_params_contain_ident_name(const ny_param_list *params,
                                              const char *name) {
  if (!params || !name)
    return false;
  for (size_t i = 0; i < params->len; ++i) {
    if (closure_expr_contains_ident_name(params->data[i].def, name))
      return true;
  }
  return false;
}

static bool closure_stmt_list_contains_ident_name(ny_stmt_list *list,
                                                  const char *name) {
  if (!list || !name)
    return false;
  for (size_t i = 0; i < list->len; ++i) {
    if (closure_stmt_contains_ident_name(list->data[i], name))
      return true;
  }
  return false;
}

static bool
closure_layout_fields_contain_ident_name(ny_layout_field_list *fields,
                                         const char *name) {
  if (!fields || !name)
    return false;
  for (size_t i = 0; i < fields->len; ++i) {
    if (closure_expr_contains_ident_name(fields->data[i].default_value, name))
      return true;
  }
  return false;
}

static bool closure_stmt_contains_ident_name(stmt_t *s, const char *name) {
  if (!s || !name)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    return closure_stmt_list_contains_ident_name(&s->as.block.body, name);
  case NY_S_MODULE:
    return closure_stmt_list_contains_ident_name(&s->as.module.body, name);
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      if (closure_expr_contains_ident_name(s->as.var.exprs.data[i], name))
        return true;
    }
    return false;
  case NY_S_EXPR:
    return closure_expr_contains_ident_name(s->as.expr.expr, name);
  case NY_S_IF:
    return closure_stmt_contains_ident_name(s->as.iff.init, name) ||
           closure_expr_contains_ident_name(s->as.iff.test, name) ||
           closure_stmt_contains_ident_name(s->as.iff.conseq, name) ||
           closure_stmt_contains_ident_name(s->as.iff.alt, name);
  case NY_S_GUARD:
    if (closure_expr_contains_ident_name(s->as.guard.value, name))
      return true;
    if (s->as.guard.name && strcmp(s->as.guard.name, name) == 0)
      return false;
    return closure_stmt_contains_ident_name(s->as.guard.fallback, name);
  case NY_S_WHILE:
    return closure_stmt_contains_ident_name(s->as.whl.init, name) ||
           closure_expr_contains_ident_name(s->as.whl.test, name) ||
           closure_stmt_contains_ident_name(s->as.whl.body, name) ||
           closure_stmt_contains_ident_name(s->as.whl.update, name);
  case NY_S_FOR: {
    if (closure_stmt_contains_ident_name(s->as.fr.init, name) ||
        closure_expr_contains_ident_name(s->as.fr.cond, name) ||
        closure_expr_contains_ident_name(s->as.fr.iterable, name) ||
        closure_stmt_contains_ident_name(s->as.fr.update, name))
      return true;
    bool iter_shadows =
        (s->as.fr.iter_var && strcmp(s->as.fr.iter_var, name) == 0) ||
        (s->as.fr.iter_index_var && strcmp(s->as.fr.iter_index_var, name) == 0);
    return !iter_shadows &&
           closure_stmt_contains_ident_name(s->as.fr.body, name);
  }
  case NY_S_TRY:
    if (closure_stmt_contains_ident_name(s->as.tr.body, name))
      return true;
    if (s->as.tr.err && strcmp(s->as.tr.err, name) == 0)
      return false;
    return closure_stmt_contains_ident_name(s->as.tr.handler, name);
  case NY_S_RETURN:
    return closure_expr_contains_ident_name(s->as.ret.value, name);
  case NY_S_DEFER:
    return closure_stmt_contains_ident_name(s->as.de.body, name);
  case NY_S_MATCH:
    if (closure_expr_contains_ident_name(s->as.match.test, name))
      return true;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t p = 0; p < arm->patterns.len; ++p) {
        if (closure_expr_contains_ident_name(arm->patterns.data[p], name))
          return true;
      }
      if (closure_expr_contains_ident_name(arm->guard, name) ||
          closure_stmt_contains_ident_name(arm->conseq, name))
        return true;
    }
    return closure_stmt_contains_ident_name(s->as.match.default_conseq, name);
  case NY_S_FUNC:
    if (closure_params_contain_ident_name(&s->as.fn.params, name))
      return true;
    if (closure_param_list_shadows_name(&s->as.fn.params, name))
      return false;
    return closure_stmt_contains_ident_name(s->as.fn.body, name);
  case NY_S_LAYOUT:
    return closure_layout_fields_contain_ident_name(&s->as.layout.fields,
                                                    name) ||
           closure_stmt_list_contains_ident_name(&s->as.layout.methods, name);
  case NY_S_STRUCT:
    return closure_layout_fields_contain_ident_name(&s->as.struc.fields,
                                                    name) ||
           closure_stmt_list_contains_ident_name(&s->as.struc.methods, name);
  case NY_S_ENUM:
    for (size_t i = 0; i < s->as.enu.items.len; ++i) {
      if (closure_expr_contains_ident_name(s->as.enu.items.data[i].value, name))
        return true;
    }
    return false;
  case NY_S_MACRO:
    for (size_t i = 0; i < s->as.macro.args.len; ++i) {
      if (closure_expr_contains_ident_name(s->as.macro.args.data[i], name))
        return true;
    }
    return closure_stmt_contains_ident_name(s->as.macro.body, name);
  case NY_S_IMPL:
    return closure_stmt_list_contains_ident_name(&s->as.impl.methods, name);
  default:
    return false;
  }
}

static bool closure_seen_name(const str_list *seen, const char *name) {
  if (!seen || !name)
    return false;
  for (size_t i = 0; i < seen->len; ++i) {
    if (seen->data[i] && strcmp(seen->data[i], name) == 0)
      return true;
  }
  return false;
}

LLVMValueRef gen_closure(codegen_t *cg, scope *scopes, size_t depth,
                         ny_param_list params, stmt_t *body, bool is_variadic,
                         const char *return_type, const char *name_hint) {
  if (cg)
    cg->last_lambda_sig = NULL;
  binding_list captures;
  vec_init(&captures);
  str_list seen_names;
  vec_init(&seen_names);
  for (ssize_t i = (ssize_t)depth; i >= 1; i--) {
    for (size_t j = 0; j < scopes[i].vars.len; j++) {
      binding *candidate = &scopes[i].vars.data[j];
      if (!candidate->name || closure_seen_name(&seen_names, candidate->name))
        continue;
      vec_push(&seen_names, (char *)candidate->name);
      if (closure_param_list_shadows_name(&params, candidate->name))
        continue;
      if (!closure_params_contain_ident_name(&params, candidate->name) &&
          !closure_stmt_contains_ident_name(body, candidate->name))
        continue;
      vec_push(&captures, *candidate);
      candidate->is_used = true;
    }
  }
  vec_free(&seen_names);
  token_t closure_tok = body ? body->tok : (token_t){0};
  char name[64];
  if (name_hint && strncmp(name_hint, "__lambda", 8) == 0) {
    if (closure_tok.line > 0) {
      snprintf(name, sizeof(name), "%s_L%d_C%d_%d", name_hint, closure_tok.line,
               closure_tok.col, cg->lambda_count++);
    } else {
      snprintf(name, sizeof(name), "%s_%d", name_hint, cg->lambda_count++);
    }
  } else {
    snprintf(name, sizeof(name), "%s_%d", name_hint ? name_hint : "__lambda",
             cg->lambda_count++);
  }
  stmt_t *sfn = arena_alloc(cg->arena, sizeof(*sfn));
  memset(sfn, 0, sizeof(*sfn));
  ny_param_list callable_params = params;
  /*
   * An unannotated lambda still returns a language value.  Keep its
   * generated function on the tagged NyValue ABI so a dynamic arithmetic
   * expression is not tagged once by the expression and again by the
   * untyped-tail return path.  Explicit annotations continue to select the
   * corresponding raw/native ABI.
   */
  const char *callable_return_type = return_type ? return_type : "any";
  bool needs_tagged_adapter_abi = callable_return_type != NULL;
  for (size_t i = 0; i < params.len; ++i) {
    if (params.data[i].type) {
      needs_tagged_adapter_abi = true;
      break;
    }
  }
  if (needs_tagged_adapter_abi) {
    memset(&callable_params, 0, sizeof(callable_params));
    for (size_t i = 0; i < params.len; ++i) {
      param_t p = params.data[i];
      p.type = NULL;
      vec_push_arena(cg->arena, &callable_params, p);
    }
    callable_return_type = NULL;
  }
  sfn->kind = NY_S_FUNC;
  sfn->as.fn.name = arena_strndup(cg->arena, name, strlen(name));
  sfn->as.fn.params = callable_params;
  sfn->as.fn.body = body;
  sfn->as.fn.is_variadic = is_variadic;
  sfn->as.fn.return_type = callable_return_type;

  if (body)
    sfn->tok = body->tok;
  scope sc[NY_SCOPE_STACK_CAP] = {0};

  bool uses_env = captures.len > 0 || ny_lambda_values_need_closure(cg);
  if (name_hint && strcmp(name_hint, "__defer") == 0)
    uses_env = true;

  gen_func(cg, sfn, name, sc, 0, uses_env ? &captures : NULL);
  LLVMValueRef lf = ny_get_named_fn(cg, name);
  if (lf)
    LLVMSetLinkage(lf, LLVMInternalLinkage);
  fun_sig *lambda_sig = lookup_fun(cg, name, 0);
  cg->last_lambda_sig = (!uses_env && lambda_sig) ? lambda_sig : NULL;

  LLVMValueRef fn_ptr_raw = ny_ptr2i64(cg, lf, "");

  if (!uses_env) {

    vec_free(&captures);
    return fn_ptr_raw;
  }

  bool is_defer = (name_hint && strcmp(name_hint, "__defer") == 0);
  LLVMValueRef env_ptr = NULL;

  fun_sig *malloc_sig = lookup_fun(cg, "__malloc", 0);
  if (!malloc_sig) {
    token_t tok = body ? body->tok : (token_t){0};
    return expr_fail(cg, tok, "__malloc required for closures");
  }

  if (is_defer) {
    LLVMValueRef stack_env = LLVMBuildAlloca(
        cg->builder, LLVMArrayType(cg->type_i64, captures.len), "defer_env");
    LLVMSetAlignment(stack_env, 16);
    env_ptr = LLVMBuildPtrToInt(cg->builder, stack_env, cg->type_i64, "env");
  } else {
    LLVMValueRef env_alloc_size = LLVMConstInt(
        cg->type_i64, (uint64_t)(((uint64_t)captures.len * 8) << 1) | 1, false);
    env_ptr = LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                             (LLVMValueRef[]){env_alloc_size}, 1, "env");
  }
  LLVMValueRef env_raw =
      LLVMBuildIntToPtr(cg->builder, env_ptr, ny_ptr_i64_ty(cg), "env_raw");
  for (size_t i = 0; i < captures.len; i++) {
    LLVMValueRef slot_val = captures.data[i].is_slot
                                ? LLVMBuildLoad2(cg->builder, cg->type_i64,
                                                 captures.data[i].value, "")
                                : captures.data[i].value;
    LLVMValueRef dst = LLVMBuildGEP2(
        cg->builder, cg->type_i64, env_raw,
        (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)i, false)}, 1,
        "");
    if (dst && slot_val) {
      ny_store(cg, dst, slot_val);
    }
  }

  LLVMValueRef cls_size =
      LLVMConstInt(cg->type_i64, ((uint64_t)16 << 1) | 1, false);
  LLVMValueRef cls_ptr =
      LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                     (LLVMValueRef[]){cls_size}, 1, "closure");
  LLVMValueRef cls_raw =
      LLVMBuildIntToPtr(cg->builder, cls_ptr, ny_ptr_i64_ty(cg), "");

  LLVMValueRef tag_addr = LLVMBuildGEP2(
      cg->builder, ny_i8_ty(cg),
      ny_bitcast(cg, cls_raw, LLVMPointerType(ny_i8_ty(cg), 0), ""),
      (LLVMValueRef[]){LLVMConstInt(cg->type_i64, -8, true)}, 1, "");
  if (tag_addr) {
    ny_store(cg, ny_bitcast(cg, tag_addr, ny_ptr_i64_ty(cg), ""),
             LLVMConstInt(cg->type_i64, TAG_CLOSURE, false));
  }

  if (cls_raw && fn_ptr_raw) {
    ny_store(cg, cls_raw, fn_ptr_raw);
  }

  LLVMValueRef env_store_addr = LLVMBuildGEP2(
      cg->builder, cg->type_i64, cls_raw, (LLVMValueRef[]){ny_c1(cg)}, 1, "");
  if (env_store_addr && env_ptr) {
    ny_store(cg, env_store_addr, env_ptr);
  }
  vec_free(&captures);
  return cls_ptr;
}

static bool ny_fun_sig_needs_tagged_callable_adapter(fun_sig *sig) {
  if (!sig || sig->is_extern || sig->is_variadic || sig->arity < 0 ||
      sig->arity > 15)
    return false;
  if (sig->return_type && *sig->return_type)
    return true;
  for (int i = 0; i < sig->arity; ++i) {
    const char *ptype = ny_sig_param_type(sig, (size_t)i);
    if (ptype && *ptype)
      return true;
  }
  return false;
}

static LLVMValueRef
ny_fun_sig_tagged_callable_adapter(codegen_t *cg, fun_sig *target, token_t tok,
                                   bool hidden_env, bool native_abi) {
  if (!cg || !target || !target->name)
    return NULL;
  uint64_t h =
      target->name_hash ? target->name_hash : ny_hash64_cstr(target->name);
  char adapter_name_buf[96];
  snprintf(adapter_name_buf, sizeof(adapter_name_buf),
           native_abi   ? "__ny_native_callback_adapter_%llx_%d"
           : hidden_env ? "__ny_callable_adapter_env_%llx_%d"
                        : "__ny_callable_adapter_%llx_%d",
           (unsigned long long)h, target->arity);
  LLVMValueRef existing = ny_get_named_fn(cg, adapter_name_buf);
  if (existing)
    return existing;

  stmt_t *wrapper = arena_alloc(cg->arena, sizeof(*wrapper));
  memset(wrapper, 0, sizeof(*wrapper));
  wrapper->kind = NY_S_FUNC;
  wrapper->tok = tok;
  wrapper->as.fn.name =
      arena_strndup(cg->arena, adapter_name_buf, strlen(adapter_name_buf));

  expr_t *callee = arena_alloc(cg->arena, sizeof(*callee));
  memset(callee, 0, sizeof(*callee));
  callee->kind = NY_E_IDENT;
  callee->tok = tok;
  callee->as.ident.name = target->name;
  callee->as.ident.hash = h;

  expr_t *call = arena_alloc(cg->arena, sizeof(*call));
  memset(call, 0, sizeof(*call));
  call->kind = NY_E_CALL;
  call->tok = tok;
  call->as.call.callee = callee;

  for (int i = 0; i < target->arity; ++i) {
    char pname_buf[24];
    snprintf(pname_buf, sizeof(pname_buf), "__arg%d", i);
    const char *pname = arena_strndup(cg->arena, pname_buf, strlen(pname_buf));
    param_t p = {.name = pname,
                 .type =
                     native_abi ? ny_sig_param_type(target, (size_t)i) : NULL};
    vec_push_arena(cg->arena, &wrapper->as.fn.params, p);

    expr_t *arg_ident = arena_alloc(cg->arena, sizeof(*arg_ident));
    memset(arg_ident, 0, sizeof(*arg_ident));
    arg_ident->kind = NY_E_IDENT;
    arg_ident->tok = tok;
    arg_ident->as.ident.name = pname;
    arg_ident->as.ident.hash = ny_hash64_cstr(pname);
    call_arg_t arg = {.val = arg_ident};
    vec_push_arena(cg->arena, &call->as.call.args, arg);
  }

  stmt_t *ret = stmt_new(cg->arena, NY_S_RETURN, tok);
  ret->as.ret.value = call;
  stmt_t *body = stmt_new(cg->arena, NY_S_BLOCK, tok);
  vec_push_arena(cg->arena, &body->as.block.body, ret);
  wrapper->as.fn.body = body;
  wrapper->as.fn.return_type = native_abi ? target->return_type : "any";

  scope sc[NY_SCOPE_STACK_CAP] = {0};
  binding_list empty_captures = {0};
  gen_func(cg, wrapper, wrapper->as.fn.name, sc, 0,
           hidden_env ? &empty_captures : NULL);
  LLVMValueRef adapter = ny_get_named_fn(cg, wrapper->as.fn.name);
  if (adapter)
    LLVMSetLinkage(adapter, LLVMInternalLinkage);
  return adapter;
}

LLVMValueRef ny_native_callback_adapter_value(codegen_t *cg, expr_t *expr) {
  if (!cg || !expr || expr->kind != NY_E_IDENT || !expr->as.ident.name)
    return NULL;
  fun_sig *target = lookup_fun(cg, expr->as.ident.name, expr->as.ident.hash);
  if (!target || target->is_extern || target->is_variadic)
    return NULL;
  LLVMValueRef adapter =
      ny_fun_sig_tagged_callable_adapter(cg, target, expr->tok, false, true);
  return adapter ? ny_ptr2i64(cg, adapter, "native_callback") : NULL;
}

static bool ny_named_callable_values_need_closure(codegen_t *cg) {
  return ny_module_target_is_apple_arm64(cg ? cg->module : NULL);
}

static LLVMValueRef
ny_box_callable_closure(codegen_t *cg, LLVMValueRef fn_ptr_raw, token_t tok) {
  if (!cg || !fn_ptr_raw)
    return NULL;
  fun_sig *malloc_sig = lookup_fun(cg, "__malloc", 0);
  if (!malloc_sig)
    return expr_fail(cg, tok, "__malloc required for callable closures");

  LLVMValueRef cls_size =
      LLVMConstInt(cg->type_i64, ((uint64_t)16 << 1) | 1, false);
  LLVMValueRef cls_ptr =
      LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                     (LLVMValueRef[]){cls_size}, 1, "callable_closure");
  LLVMValueRef cls_raw =
      LLVMBuildIntToPtr(cg->builder, cls_ptr, ny_ptr_i64_ty(cg), "");
  LLVMValueRef tag_addr = LLVMBuildGEP2(
      cg->builder, ny_i8_ty(cg),
      ny_bitcast(cg, cls_raw, LLVMPointerType(ny_i8_ty(cg), 0), ""),
      (LLVMValueRef[]){LLVMConstInt(cg->type_i64, -8, true)}, 1, "");
  if (tag_addr)
    ny_store(cg, ny_bitcast(cg, tag_addr, ny_ptr_i64_ty(cg), ""),
             LLVMConstInt(cg->type_i64, TAG_CLOSURE, false));
  ny_store(cg, cls_raw, fn_ptr_raw);
  LLVMValueRef env_store_addr = LLVMBuildGEP2(
      cg->builder, cg->type_i64, cls_raw, (LLVMValueRef[]){ny_c1(cg)}, 1, "");
  if (env_store_addr)
    ny_store(cg, env_store_addr, ny_cnil(cg));
  return cls_ptr;
}

LLVMValueRef to_bool(codegen_t *cg, LLVMValueRef v) {
  if (v && LLVMTypeOf(v) == ny_i1_ty(cg))
    return v;

  if (v && LLVMIsAConstantInt(v)) {
    int64_t val = LLVMConstIntGetSExtValue(v);
    if (val == NY_IMM_TRUE)
      return LLVMConstInt(ny_i1_ty(cg), 1, false);
    if (val == NY_IMM_FALSE || val == NY_IMM_NIL || val == 1)
      return LLVMConstInt(ny_i1_ty(cg), 0, false);
  }

  if (v && LLVMIsASelectInst(v)) {
    LLVMValueRef tv = LLVMGetOperand(v, 1);
    LLVMValueRef fv = LLVMGetOperand(v, 2);
    if (LLVMIsAConstantInt(tv) && LLVMIsAConstantInt(fv) &&
        LLVMConstIntGetZExtValue(tv) == NY_IMM_TRUE &&
        LLVMConstIntGetZExtValue(fv) == NY_IMM_FALSE) {
      return LLVMGetOperand(v, 0);
    }
  }

  return LLVMBuildICmp(cg->builder, LLVMIntUGT, v,
                       LLVMConstInt(cg->type_i64, 2, false),
                       NY_LLVM_NAME(cg, "to_bool"));
}

static void intern_map_resize(codegen_t *cg, size_t new_cap) {
  intern_entry *new_map = calloc(new_cap, sizeof(intern_entry));
  if (!new_map) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  for (size_t i = 0; i < cg->intern_map_cap; i++) {
    intern_entry *e = &cg->intern_map[i];
    if (e->intern_idx == 0)
      continue;

    string_intern *in = &cg->interns.data[e->intern_idx - 1];
    uint64_t hash = in->hash;
    size_t pos = hash & (new_cap - 1);
    while (1) {
      if (new_map[pos].intern_idx == 0) {
        new_map[pos].intern_idx = e->intern_idx;
        break;
      }
      pos = (pos + 1) & (new_cap - 1);
    }
  }
  free(cg->intern_map);
  cg->intern_map = new_map;
  cg->intern_map_cap = new_cap;
}

static void intern_map_put(codegen_t *cg, uint64_t hash, uint32_t intern_idx) {
  if (cg->intern_map_len * 2 >= cg->intern_map_cap) {
    size_t new_cap = cg->intern_map_cap ? cg->intern_map_cap * 2 : 1024;
    intern_map_resize(cg, new_cap);
  }
  size_t pos = hash & (cg->intern_map_cap - 1);
  while (1) {
    if (cg->intern_map[pos].intern_idx == 0) {
      cg->intern_map[pos].intern_idx = intern_idx;
      cg->intern_map_len++;
      return;
    }
    pos = (pos + 1) & (cg->intern_map_cap - 1);
  }
}

LLVMValueRef const_string_ptr(codegen_t *cg, const char *s, size_t len) {
  uint64_t key_hash = ny_const_str_hash(s, len);

  if (cg->intern_map_cap > 0) {
    size_t pos = key_hash & (cg->intern_map_cap - 1);
    while (1) {
      uint32_t idx = cg->intern_map[pos].intern_idx;
      if (idx == 0)
        break;

      string_intern *in = &cg->interns.data[idx - 1];
      if (in->hash == key_hash && in->len == len && in->module == cg->module) {
        if (memcmp(in->data, s, len) == 0) {
          return in->val;
        }
      }
      pos = (pos + 1) & (cg->intern_map_cap - 1);
    }
  }

  const char *final_s = s;
  size_t final_len = len;
  size_t header_size = 64;
  size_t tail_size = 16;
  size_t total_len = header_size + final_len + 1 + tail_size;
  char *obj_data = calloc(1, total_len);

  *(uint64_t *)(obj_data) = 0;
  *(uint64_t *)(obj_data + 8) = (uint64_t)final_len;
  *(uint64_t *)(obj_data + 16) = 0;
  *(uint64_t *)(obj_data + 48) = ((uint64_t)final_len << 1) | 1;
  *(uint64_t *)(obj_data + 56) = 120;

  memcpy(obj_data + header_size, final_s, final_len);
  obj_data[header_size + final_len] = '\0';

  uint64_t magic3 = NY_MAGIC3;
  memcpy(obj_data + header_size + final_len + 1, &magic3, sizeof(magic3));
  char data_name[128];
  snprintf(data_name, sizeof(data_name), ".str.data.%llx",
           (unsigned long long)key_hash);
  LLVMValueRef g = ny_get_global(cg, data_name);
  if (!g) {
    LLVMTypeRef arr_ty = LLVMArrayType(ny_i8_ty(cg), (unsigned)total_len);
    g = LLVMAddGlobal(cg->module, arr_ty, data_name);
    LLVMSetInitializer(g, LLVMConstStringInContext(cg->ctx, obj_data,
                                                   (unsigned)total_len, true));
    LLVMSetGlobalConstant(g, true);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(g, true);
    LLVMSetAlignment(g, 64);
  }

  char ptr_name[128];
  snprintf(ptr_name, sizeof(ptr_name), ".str.runtime.%llx",
           (unsigned long long)key_hash);
  LLVMValueRef runtime_ptr_global = ny_get_global(cg, ptr_name);
  if (!runtime_ptr_global) {
    runtime_ptr_global = LLVMAddGlobal(cg->module, cg->type_i64, ptr_name);
    LLVMSetInitializer(runtime_ptr_global, ny_c0(cg));
    LLVMSetLinkage(runtime_ptr_global, LLVMPrivateLinkage);
  }

  string_intern in = {.data = obj_data + header_size,
                      .len = final_len,
                      .hash = key_hash,
                      .val = runtime_ptr_global,
                      .gv = g,
                      .module = cg->module,
                      .alloc = obj_data};
  vec_push(&cg->interns, in);
  intern_map_put(cg, key_hash, (uint32_t)cg->interns.len);

  return runtime_ptr_global;
}

LLVMValueRef ny_is_tagged_int(codegen_t *cg, LLVMValueRef v) {
  if (!v)
    return LLVMConstInt(ny_i1_ty(cg), 0, false);
  LLVMTypeRef ty = LLVMTypeOf(v);
  if (!ty || LLVMGetTypeKind(ty) != LLVMIntegerTypeKind ||
      LLVMGetIntTypeWidth(ty) != 64) {
    return LLVMConstInt(ny_i1_ty(cg), 0, false);
  }
  /*
   * Tagged integers are exactly the values with bit 0 set.  Truncating an
   * i64 to i1 is the tag test, and avoids materializing an AND plus compare
   * in unoptimized/low-opt IR.
   */
  return LLVMBuildTrunc(cg->builder, v, ny_i1_ty(cg),
                        NY_LLVM_NAME(cg, "is_tagged_int"));
}

LLVMValueRef ny_untag_int(codegen_t *cg, LLVMValueRef v) {
  return ny_ashr(cg, v, ny_c1(cg), "untag_int");
}

LLVMValueRef ny_tag_int(codegen_t *cg, LLVMValueRef v) {
  LLVMValueRef sh = ny_shl(cg, v, ny_c1(cg), "");
  return ny_or(cg, sh, ny_c1(cg), "tag_int");
}

static LLVMValueRef ny_binding_tagged_int_value(codegen_t *cg, binding *b) {
  if (!b)
    return NULL;
  if (b->is_int_direct && b->is_int_raw_direct && b->raw_int_value)
    return ny_tag_int(cg, b->raw_int_value);
  return b->value;
}

static fun_sig *ny_lookup_range2(codegen_t *cg) {
  fun_sig *sig = resolve_overload(cg, "range2", 3, 0);
  if (!sig)
    sig = resolve_overload(cg, "core.range2", 3, 0);
  if (!sig)
    sig = resolve_overload(cg, "core.iter.range2", 3, 0);
  return sig;
}

static LLVMValueRef gen_range_expr(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e) {
  if (!e || !e->as.binary.left || !e->as.binary.right)
    return ny_c0(cg);
  fun_sig *sig = ny_lookup_range2(cg);
  if (!sig) {
    ny_diag_hint("import core or core.iter to provide range2");
    return expr_fail(
        cg, e->tok, "range expression '..' requires range2(start, stop, step)");
  }
  LLVMValueRef start = gen_expr(cg, scopes, depth, e->as.binary.left);
  LLVMValueRef stop = gen_expr(cg, scopes, depth, e->as.binary.right);
  LLVMValueRef one = LLVMConstInt(cg->type_i64, ((uint64_t)1 << 1) | 1u, false);
  LLVMValueRef inclusive_stop =
      gen_binary(cg, scopes, depth, "+", stop, one, e->as.binary.right, NULL);
  LLVMValueRef args[3] = {
      sig->is_native_abi ? ny_coerce_to_abi(cg, start, "int") : start,
      sig->is_native_abi ? ny_coerce_to_abi(cg, inclusive_stop, "int")
                         : inclusive_stop,
      sig->is_native_abi ? ny_coerce_to_abi(cg, one, "int") : one,
  };
  return LLVMBuildCall2(cg->builder, sig->type, sig->value, args, 3,
                        NY_LLVM_NAME(cg, "range"));
}

LLVMValueRef ny_is_float(codegen_t *cg, LLVMValueRef v) {
  fun_sig *s = lookup_fun(cg, "__is_float_obj", 0);
  if (!s)
    return LLVMConstInt(ny_i1_ty(cg), 0, false);
  LLVMValueRef res_tagged = LLVMBuildCall2(cg->builder, s->type, s->value,
                                           (LLVMValueRef[]){v}, 1, "");
  return ny_eq(cg, res_tagged, ny_ctrue(cg), "is_flt");
}

LLVMValueRef ny_unbox_float(codegen_t *cg, LLVMValueRef v) {
  fun_sig *unbox = ny_module_target_is_apple_arm64(cg ? cg->module : NULL)
                       ? lookup_fun(cg, "__flt_unbox_val", 0)
                       : NULL;
  if (unbox) {
    LLVMValueRef bits =
        LLVMBuildCall2(cg->builder, unbox->type, unbox->value, &v, 1, "");
    return ny_bitcast(cg, bits, cg->type_f64, NY_LLVM_NAME(cg, "unbox_flt"));
  }

  LLVMBasicBlockRef entry_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
  LLVMBasicBlockRef int_bb = ny_bb_fn(fn, "unbox_flt.int");
  LLVMBasicBlockRef check_flt_bb = ny_bb_fn(fn, "unbox_flt.check_flt");
  LLVMBasicBlockRef flt_bb = ny_bb_fn(fn, "unbox_flt.flt");
  LLVMBasicBlockRef fallback_bb = ny_bb_fn(fn, "unbox_flt.fallback");
  LLVMBasicBlockRef done_bb = ny_bb_fn(fn, "unbox_flt.done");

  ny_cond_br(cg, ny_is_tagged_int(cg, v), int_bb, check_flt_bb);

  ny_pos(cg, int_bb);
  LLVMValueRef raw_int = ny_untag_int(cg, v);
  LLVMValueRef d_from_i = LLVMBuildSIToFP(cg->builder, raw_int, cg->type_f64,
                                          NY_LLVM_NAME(cg, "d_from_i"));
  LLVMBasicBlockRef int_done_bb = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, check_flt_bb);
  ny_cond_br(cg, ny_is_float(cg, v), flt_bb, fallback_bb);

  ny_pos(cg, flt_bb);
  LLVMValueRef ptr =
      LLVMBuildIntToPtr(cg->builder, v, LLVMPointerType(cg->type_f64, 0), "");
  LLVMValueRef d_from_p = LLVMBuildLoad2(cg->builder, cg->type_f64, ptr,
                                         NY_LLVM_NAME(cg, "d_from_p"));
  LLVMBasicBlockRef flt_done_bb = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, fallback_bb);
  LLVMValueRef zero = LLVMConstReal(cg->type_f64, 0.0);
  LLVMBasicBlockRef fallback_done_bb = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, done_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_f64, NY_LLVM_NAME(cg, "unbox_flt_res"));
  LLVMValueRef incoming_vals[3] = {d_from_i, d_from_p, zero};
  LLVMBasicBlockRef incoming_bbs[3] = {int_done_bb, flt_done_bb,
                                       fallback_done_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 3);
  return phi;
}

static LLVMValueRef ny_unbox_known_numeric_float(codegen_t *cg,
                                                 LLVMValueRef v) {
  if (ny_module_target_is_apple_arm64(cg ? cg->module : NULL))
    return ny_unbox_float(cg, v);

  LLVMBasicBlockRef entry_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
  LLVMBasicBlockRef int_bb = ny_bb_fn(fn, "known_flt.int");
  LLVMBasicBlockRef flt_bb = ny_bb_fn(fn, "known_flt.ptr");
  LLVMBasicBlockRef done_bb = ny_bb_fn(fn, "known_flt.done");

  ny_cond_br(cg, ny_is_tagged_int(cg, v), int_bb, flt_bb);

  ny_pos(cg, int_bb);
  LLVMValueRef raw_int = ny_untag_int(cg, v);
  LLVMValueRef from_int =
      LLVMBuildSIToFP(cg->builder, raw_int, cg->type_f64, "known_flt_i2f");
  LLVMBasicBlockRef int_done = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, flt_bb);
  LLVMValueRef ptr = LLVMBuildIntToPtr(
      cg->builder, v, LLVMPointerType(cg->type_f64, 0), "known_flt_ptr");
  LLVMValueRef from_ptr =
      LLVMBuildLoad2(cg->builder, cg->type_f64, ptr, "known_flt_load");
  LLVMBasicBlockRef flt_done = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, done_bb);
  LLVMValueRef phi = ny_phi(cg, cg->type_f64, "known_flt");
  LLVMAddIncoming(phi, (LLVMValueRef[]){from_int, from_ptr},
                  (LLVMBasicBlockRef[]){int_done, flt_done}, 2);
  return phi;
}

static LLVMValueRef
ny_emit_f64_list_get_parts_as_f64(codegen_t *cg, scope *scopes, size_t depth,
                                  expr_t *target, expr_t *key,
                                  expr_t *default_expr) {
  if (!cg || !target || !key)
    return NULL;
  binding *target_binding =
      expr_f64_list_target_binding(cg, scopes, depth, target);
  if (!target_binding)
    return NULL;
  if (!expr_index_is_int_key(cg, scopes, depth, key) ||
      !expr_is_f64_default_value(cg, scopes, depth, default_expr))
    return NULL;

  LLVMValueRef key_v = ny_llvm_cast_to_i64(cg, gen_expr(cg, scopes, depth, key),
                                           "f64_list_get_key");
  if (!key_v)
    return NULL;
  LLVMValueRef default_v =
      default_expr ? gen_expr_as_f64(cg, scopes, depth, default_expr)
                   : LLVMConstReal(cg->type_f64, 0.0);
  LLVMValueRef key_raw =
      expr_index_raw_i64(cg, scopes, depth, key, key_v, "f64_list_get_key_raw");

  int64_t key_min = 0, key_max = 0;
  bool key_range = expr_int_range(cg, scopes, depth, key, &key_min, &key_max);
  bool assume_nonnegative = key_range && key_min >= 0;

  if (target_binding->raw_f64_list_mutation &&
      target_binding->raw_f64_list_ptr &&
      target_binding->raw_f64_list_len > 0) {
    LLVMValueRef len_raw = LLVMConstInt(
        cg->type_i64, (uint64_t)target_binding->raw_f64_list_len, false);
    LLVMValueRef adj_key = key_raw;
    if (!assume_nonnegative) {
      LLVMValueRef is_neg = LLVMBuildICmp(cg->builder, LLVMIntSLT, key_raw,
                                          ny_c0(cg), "raw_f64_list_get_is_neg");
      LLVMValueRef wrapped =
          ny_add(cg, key_raw, len_raw, "raw_f64_list_get_wrapped_idx");
      adj_key =
          ny_select(cg, is_neg, wrapped, key_raw, "raw_f64_list_get_adj_idx");
    }
    bool assume_in_bounds = key_range && key_min >= 0 &&
                            key_max < (int64_t)target_binding->raw_f64_list_len;
    if (assume_in_bounds) {
      LLVMValueRef ptr = LLVMBuildInBoundsGEP2(
          cg->builder, cg->type_f64, target_binding->raw_f64_list_ptr, &adj_key,
          1, "raw_f64_list_get_ptr");
      return ny_load(cg, ptr, "raw_f64_list_get_f64");
    }
    LLVMValueRef low_ok = LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_key,
                                        ny_c0(cg), "raw_f64_list_get_low_ok");
    LLVMValueRef high_ok = LLVMBuildICmp(cg->builder, LLVMIntSLT, adj_key,
                                         len_raw, "raw_f64_list_get_hi_ok");
    LLVMValueRef in_bounds =
        ny_and(cg, low_ok, high_ok, "raw_f64_list_get_in_bounds");
    LLVMValueRef fn = LLVMGetBasicBlockParent(ny_cur_block(cg));
    LLVMBasicBlockRef load_bb = ny_bb_fn(fn, "raw_f64_list_get.load");
    LLVMBasicBlockRef default_bb = ny_bb_fn(fn, "raw_f64_list_get.default");
    LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "raw_f64_list_get.join");
    ny_cond_br(cg, in_bounds, load_bb, default_bb);
    ny_pos(cg, load_bb);
    LLVMValueRef ptr = LLVMBuildInBoundsGEP2(
        cg->builder, cg->type_f64, target_binding->raw_f64_list_ptr, &adj_key,
        1, "raw_f64_list_get_ptr");
    LLVMValueRef loaded = ny_load(cg, ptr, "raw_f64_list_get_f64");
    LLVMBasicBlockRef load_done = ny_cur_block(cg);
    ny_br(cg, join_bb);
    ny_pos(cg, default_bb);
    LLVMBasicBlockRef default_done = ny_cur_block(cg);
    ny_br(cg, join_bb);
    ny_pos(cg, join_bb);
    LLVMValueRef phi = ny_phi(cg, cg->type_f64, "raw_f64_list_get_result");
    LLVMAddIncoming(phi, (LLVMValueRef[]){loaded, default_v},
                    (LLVMBasicBlockRef[]){load_done, default_done}, 2);
    return phi;
  }

  LLVMValueRef target_v = ny_llvm_cast_to_i64(
      cg, gen_expr(cg, scopes, depth, target), "f64_list_get_target");
  if (!target_v)
    return NULL;
  LLVMValueRef target_ptr = LLVMBuildIntToPtr(
      cg->builder, target_v, ny_ptr_i64_ty(cg), "f64_list_get_ptr_i64");
  LLVMValueRef len_tagged = ny_load(cg, target_ptr, "f64_list_get_len");
  LLVMValueRef len_raw =
      expr_build_untagged_or_raw_i64(cg, len_tagged, "f64_list_get_len_raw");
  LLVMValueRef adj_key = key_raw;
  if (!assume_nonnegative) {
    LLVMValueRef is_neg = LLVMBuildICmp(cg->builder, LLVMIntSLT, key_raw,
                                        ny_c0(cg), "f64_list_get_is_neg");
    LLVMValueRef wrapped =
        ny_add(cg, key_raw, len_raw, "f64_list_get_wrapped_idx");
    adj_key = ny_select(cg, is_neg, wrapped, key_raw, "f64_list_get_adj_idx");
  }

  bool assume_in_bounds = false;
  int64_t len_min = 0;
  if (key_range && key_min >= 0 &&
      expr_list_len_min(cg, scopes, depth, target, &len_min) &&
      key_max < len_min)
    assume_in_bounds = true;

  LLVMValueRef elem_addr = NULL;
  LLVMValueRef elem_ptr = NULL;
  LLVMValueRef boxed = NULL;
  if (assume_in_bounds) {
    LLVMValueRef scaled =
        LLVMBuildShl(cg->builder, adj_key, LLVMConstInt(cg->type_i64, 3, false),
                     "f64_list_get_scaled");
    LLVMValueRef byte_off = ny_add(cg, LLVMConstInt(cg->type_i64, 16, false),
                                   scaled, "f64_list_get_off");
    elem_addr = ny_add(cg, target_v, byte_off, "f64_list_get_addr");
    elem_ptr = LLVMBuildIntToPtr(cg->builder, elem_addr, ny_ptr_i64_ty(cg),
                                 "f64_list_get_elem_ptr_i64");
    boxed = ny_load(cg, elem_ptr, "f64_list_get_elem");
    LLVMValueRef ptr =
        LLVMBuildIntToPtr(cg->builder, boxed, LLVMPointerType(cg->type_f64, 0),
                          "f64_list_get_fptr");
    return LLVMBuildLoad2(cg->builder, cg->type_f64, ptr, "f64_list_get_f64");
  }

  LLVMBasicBlockRef entry_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
  LLVMBasicBlockRef load_bb = ny_bb_fn(fn, "f64_list_get.load");
  LLVMBasicBlockRef default_bb = ny_bb_fn(fn, "f64_list_get.default");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "f64_list_get.join");

  LLVMValueRef low_ok = LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_key,
                                      ny_c0(cg), "f64_list_get_low_ok");
  LLVMValueRef high_ok = LLVMBuildICmp(cg->builder, LLVMIntSLT, adj_key,
                                       len_raw, "f64_list_get_hi_ok");
  LLVMValueRef in_bounds =
      ny_and(cg, low_ok, high_ok, "f64_list_get_in_bounds");
  ny_cond_br(cg, in_bounds, load_bb, default_bb);

  ny_pos(cg, load_bb);
  LLVMValueRef scaled =
      LLVMBuildShl(cg->builder, adj_key, LLVMConstInt(cg->type_i64, 3, false),
                   "f64_list_get_scaled");
  LLVMValueRef byte_off = ny_add(cg, LLVMConstInt(cg->type_i64, 16, false),
                                 scaled, "f64_list_get_off");
  elem_addr = ny_add(cg, target_v, byte_off, "f64_list_get_addr");
  elem_ptr = LLVMBuildIntToPtr(cg->builder, elem_addr, ny_ptr_i64_ty(cg),
                               "f64_list_get_elem_ptr_i64");
  boxed = ny_load(cg, elem_ptr, "f64_list_get_elem");
  LLVMValueRef ptr =
      LLVMBuildIntToPtr(cg->builder, boxed, LLVMPointerType(cg->type_f64, 0),
                        "f64_list_get_fptr");
  LLVMValueRef loaded =
      LLVMBuildLoad2(cg->builder, cg->type_f64, ptr, "f64_list_get_f64");
  LLVMBasicBlockRef load_done = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, default_bb);
  LLVMBasicBlockRef default_done = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi = ny_phi(cg, cg->type_f64, "f64_list_get_result");
  LLVMAddIncoming(phi, (LLVMValueRef[]){loaded, default_v},
                  (LLVMBasicBlockRef[]){load_done, default_done}, 2);
  return phi;
}

static LLVMValueRef ny_try_emit_f64_list_get_as_f64(codegen_t *cg,
                                                    scope *scopes, size_t depth,
                                                    expr_t *e) {
  if (!cg || !e || e->kind != NY_E_MEMCALL || !e->as.memcall.name ||
      !ny_name_tail_is(e->as.memcall.name, "get") || !e->as.memcall.target ||
      (e->as.memcall.args.len != 1 && e->as.memcall.args.len != 2))
    return NULL;
  expr_t *key = e->as.memcall.args.data[0].val;
  expr_t *default_expr =
      e->as.memcall.args.len == 2 ? e->as.memcall.args.data[1].val : NULL;
  return ny_emit_f64_list_get_parts_as_f64(
      cg, scopes, depth, e->as.memcall.target, key, default_expr);
}

static fun_sig *ny_helper_lookup(codegen_t *cg, fun_sig **cache_slot,
                                 const char *const *names, size_t names_len) {
  if (!cg || !names || names_len == 0)
    return NULL;
  if (cache_slot && *cache_slot) {
    if (ny_sig_in_current_sigs(cg, *cache_slot))
      return *cache_slot;
    *cache_slot = NULL;
  }
  for (size_t i = 0; i < names_len; ++i) {
    const char *name = names[i];
    if (!name || !*name)
      continue;
    fun_sig *sig = lookup_fun(cg, name, 0);
    if (sig) {
      if (cache_slot)
        *cache_slot = sig;
      return sig;
    }
  }
  return NULL;
}

static fun_sig *ny_helper_sub(codegen_t *cg) {
  static const char *const k_names[] = {"__sub"};
  return ny_helper_lookup(cg, &cg->cached_fn_sub, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_not(codegen_t *cg) {
  static const char *const k_names[] = {"__not"};
  return ny_helper_lookup(cg, &cg->cached_fn_not, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_slice(codegen_t *cg) {
  static const char *const k_names[] = {"slice"};
  return ny_helper_lookup(cg, &cg->cached_fn_slice, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_get(codegen_t *cg) {
  static const char *const k_names[] = {"core.get", "core.reflect.get",
                                        "get"};
  return ny_helper_lookup(cg, &cg->cached_fn_get, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_index_read(codegen_t *cg) {
  static const char *const k_names[] = {
      "core.index_read", "core.reflect.index_read", "index_read"};
  return ny_helper_lookup(cg, &cg->cached_fn_index_read, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static bool expr_target_is_literal_backed_list(codegen_t *cg, scope *scopes,
                                               size_t depth, expr_t *target);

static LLVMValueRef expr_try_fast_index_read(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e,
                                             fun_sig *fallback_sig) {
  if (!cg || !e || !fallback_sig || !e->as.index.target || !e->as.index.start)
    return NULL;
  if (ny_env_enabled("NYTRIX_INDEX_READ_PARITY") ||
      ny_env_enabled("NYTRIX_DISABLE_FAST_INDEX_READ"))
    return NULL;

  /*
   * Mutable sequence locals are runtime objects.  Their source initializer
   * is useful for type/length facts, but it is not a value-replacement proof:
   * append/set operations may have changed the object before this read.
   * In particular, the legacy header-layout fast path is not valid for the
   * native typed-buffer representation used by mutable lists.
   */
  if (e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name) {
    size_t name_len = (size_t)e->as.index.target->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.index.target->as.ident.name);
    binding *b = lookup_binding_hash(
        cg, scopes, depth, e->as.index.target->as.ident.name, name_len,
        e->as.index.target->as.ident.hash);
    if (ny_binding_declared_mutable(b))
      return NULL;
  }

  /*
   * Direct indexed read on list or tuple literal
   */
  if (e->as.index.target->kind == NY_E_LIST ||
      e->as.index.target->kind == NY_E_TUPLE) {
    int64_t lit_idx = 0;
    if (ny_expr_literal_i64(e->as.index.start, &lit_idx)) {
      if (lit_idx >= 0 &&
          (size_t)lit_idx < e->as.index.target->as.list_like.len) {
        return gen_expr(cg, scopes, depth,
                        e->as.index.target->as.list_like.data[lit_idx]);
      }
    }
  }

  /*
   * Direct indexed read on immutable local list/tuple binding (SROA forward)
   */
  if (e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name) {
    size_t name_len = (size_t)e->as.index.target->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.index.target->as.ident.name);
    binding *b = lookup_binding_hash(
        cg, scopes, depth, e->as.index.target->as.ident.name, name_len,
        e->as.index.target->as.ident.hash);
    if (b && !ny_binding_declared_mutable(b) && !b->static_indexable_invalid) {
      expr_t *init =
          ny_binding_var_init_expr(b, e->as.index.target->as.ident.name);
      if (init && (init->kind == NY_E_TUPLE || init->kind == NY_E_LIST)) {
        int64_t lit_idx = 0;
        if (ny_expr_literal_i64(e->as.index.start, &lit_idx)) {
          if (lit_idx >= 0 && (size_t)lit_idx < init->as.list_like.len) {
            /*
             * Only re-generate the element when it is side-effect free. The
             * element was already evaluated once when the binding was
             * initialized; re-generating an impure element (call, allocation)
             * here would evaluate it a second time per read. For impure
             * elements fall through to the ordinary stored-value read.
             */
            if (ny_expr_is_pure(cg, scopes, depth,
                                init->as.list_like.data[lit_idx]))
              return gen_expr(cg, scopes, depth,
                              init->as.list_like.data[lit_idx]);
          }
        }
      }
    }
  }

  /*
   * Native list/tuple locals are represented by a typed-buffer data pointer,
   * while the ordinary fast path below assumes a managed-list header.  A
   * negative literal is not covered by the direct SROA proof above, so use
   * the representation-aware runtime accessor when the source length proves
   * the wrapped index valid.  Keeping the proof here preserves index_read's
   * panic behavior for unknown/out-of-range indices.
   */
  int64_t negative_literal = 0;
  int64_t known_len = 0;
  if (expr_negative_literal_i64(e->as.index.start, &negative_literal) &&
      negative_literal < 0 &&
      expr_target_is_known_list_like(cg, scopes, depth, e->as.index.target) &&
      expr_list_len_min(cg, scopes, depth, e->as.index.target, &known_len) &&
      negative_literal >= -known_len) {
    LLVMValueRef target_v = gen_expr(cg, scopes, depth, e->as.index.target);
    LLVMValueRef key_v = gen_expr(cg, scopes, depth, e->as.index.start);
    if (target_v && key_v) {
      target_v = ny_llvm_cast_to_i64(cg, target_v, "negative_index_target");
      key_v = ny_llvm_cast_to_i64(cg, key_v, "negative_index_key");
      LLVMValueRef key_raw =
          expr_index_raw_i64(cg, scopes, depth, e->as.index.start, key_v,
                             "negative_index_key_raw");
      LLVMTypeRef args_ty[3] = {cg->type_i64, cg->type_i64, cg->type_i64};
      LLVMTypeRef fn_ty = LLVMFunctionType(cg->type_i64, args_ty, 3, 0);
      LLVMValueRef fn =
          LLVMGetNamedFunction(cg->module, "rt_value_get_index_raw");
      if (!fn)
        fn = LLVMAddFunction(cg->module, "rt_value_get_index_raw", fn_ty);
      if (fn && key_raw)
        return LLVMBuildCall2(cg->builder, fn_ty, fn,
                              (LLVMValueRef[]){target_v, key_raw, ny_c0(cg)}, 3,
                              NY_LLVM_NAME(cg, "negative_index_runtime_get"));
    }
  }

  /*
   * Elided static int list: the object was elided away, so the binding value
   * is a null marker.  The elements live in a static constant array, and reads
   * must go through that array (same representation as the get() builtin).
   * Without this branch a dynamic index read would dereference the null marker
   * and segfault.
   */
  if (e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name) {
    binding *b = ny_static_int_list_target_binding(cg, scopes, depth,
                                                   e->as.index.target);
    /*
     * Literal integer tables do not need a runtime list object at all.
     * This used to be restricted to the object-elided case, which meant a
     * perfectly ordinary immutable table such as SHA-256's K[64] fell
     * through to rt_tbuf_index_any_raw on every round.  The static-list
     * helper already rejects escaping/mutable-unsafe bindings, so use the
     * pooled constant array whenever that proof is available.
     */
    expr_t *init = ny_binding_flow_static_int_list_init(b);
    if (init) {
      LLVMTypeRef array_ty = NULL;
      LLVMValueRef g = ny_static_int_list_global(cg, b, init, &array_ty);
      if (g && array_ty) {
        LLVMValueRef key_v = gen_expr(cg, scopes, depth, e->as.index.start);
        if (key_v) {
          key_v = ny_llvm_cast_to_i64(cg, key_v, "elided_index_key");
          bool assume_nonnegative =
              expr_index_is_nonnegative(cg, scopes, depth, e->as.index.start);
          bool assume_in_bounds =
              assume_nonnegative &&
              expr_index_in_list_len_min(cg, scopes, depth, e->as.index.target,
                                         e->as.index.start);
          LLVMValueRef key_raw =
              expr_index_raw_i64(cg, scopes, depth, e->as.index.start, key_v,
                                 "elided_index_key_raw");
          size_t len = init->as.list_like.len;
          ny_dbg_loc(cg, e->tok);
          if (assume_in_bounds) {
            LLVMValueRef idxs[2] = {ny_c0(cg), key_raw};
            LLVMValueRef elem_ptr = LLVMBuildInBoundsGEP2(
                cg->builder, array_ty, g, idxs, 2, "elided_index_elem_ptr");
            LLVMValueRef val =
                ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "elided_index_elem"));
            return b->static_int_list_untagged ? ny_tag_int(cg, val) : val;
          }
          LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
          LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
          LLVMBasicBlockRef load_bb = ny_bb_fn(fn, "elided_index.load");
          LLVMBasicBlockRef panic_bb = ny_bb_fn(fn, "elided_index.panic");
          LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "elided_index.join");
          LLVMValueRef len_raw =
              LLVMConstInt(cg->type_i64, (uint64_t)len, false);
          LLVMValueRef adj_idx = key_raw;
          LLVMValueRef low_ok = LLVMConstInt(cg->type_i1, 1, false);
          if (!assume_nonnegative) {
            LLVMValueRef is_neg =
                LLVMBuildICmp(cg->builder, LLVMIntSLT, key_raw, ny_c0(cg),
                              "elided_index_is_neg");
            LLVMValueRef wrapped =
                ny_add(cg, key_raw, len_raw, "elided_index_wrapped");
            adj_idx =
                ny_select(cg, is_neg, wrapped, key_raw, "elided_index_adj");
            low_ok = LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_idx, ny_c0(cg),
                                   "elided_index_low_ok");
          }
          LLVMValueRef high_ok = LLVMBuildICmp(cg->builder, LLVMIntSLT, adj_idx,
                                               len_raw, "elided_index_high_ok");
          LLVMValueRef in_bounds =
              ny_and(cg, low_ok, high_ok, "elided_index_in_bounds");
          ny_cond_br(cg, in_bounds, load_bb, panic_bb);
          ny_pos(cg, load_bb);
          LLVMValueRef idx_for_load[2] = {ny_c0(cg), adj_idx};
          LLVMValueRef elem_ptr =
              LLVMBuildInBoundsGEP2(cg->builder, array_ty, g, idx_for_load, 2,
                                    "elided_index_elem_ptr");
          LLVMValueRef elem_val =
              ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "elided_index_elem"));
          if (b->static_int_list_untagged)
            elem_val = ny_tag_int(cg, elem_val);
          LLVMBasicBlockRef load_end_bb = ny_cur_block(cg);
          ny_br(cg, join_bb);
          ny_pos(cg, panic_bb);
          fun_sig *panic_sig = lookup_fun(cg, "__panic", 0);
          if (panic_sig) {
            const char *msg = "index out of range";
            LLVMValueRef msg_global = const_string_ptr(cg, msg, strlen(msg));
            LLVMValueRef msg_ptr =
                ny_load(cg, msg_global, "elided_index_panic_msg");
            LLVMBuildCall2(cg->builder, panic_sig->type, panic_sig->value,
                           (LLVMValueRef[]){msg_ptr}, 1, "");
          }
          LLVMBuildUnreachable(cg->builder);
          ny_pos(cg, join_bb);
          LLVMValueRef phi =
              ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "elided_index_result"));
          LLVMAddIncoming(phi, &elem_val, &load_end_bb, 1);
          return phi;
        }
      }
    }
  }

  /*
   * Runtime list literals use the native tbuf data-pointer representation.
   * The raw pointer fast path below assumes the legacy managed-list header
   * and reads at data + 16 + index * 8, which skips two elements for tbufs.
   * Static integer lists were handled above; route other literal-backed
   * lists through the checked representation-aware index helper.
   */
  if (expr_target_is_literal_backed_list(cg, scopes, depth, e->as.index.target))
    return NULL;

  if (!expr_target_is_known_list_like(cg, scopes, depth, e->as.index.target) ||
      !expr_index_is_int_key(cg, scopes, depth, e->as.index.start))
    return NULL;

  LLVMValueRef target_v = gen_expr(cg, scopes, depth, e->as.index.target);
  LLVMValueRef key_v = gen_expr(cg, scopes, depth, e->as.index.start);
  if (!target_v || !key_v)
    return NULL;
  target_v = ny_llvm_cast_to_i64(cg, target_v, "fast_index_target");
  key_v = ny_llvm_cast_to_i64(cg, key_v, "fast_index_key");
  bool assume_nonnegative =
      expr_index_is_nonnegative(cg, scopes, depth, e->as.index.start);
  bool assume_in_bounds =
      assume_nonnegative &&
      expr_index_in_list_len_min(cg, scopes, depth, e->as.index.target,
                                 e->as.index.start);
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef key_raw = expr_index_raw_i64(
      cg, scopes, depth, e->as.index.start, key_v, "fast_index_key_raw");
  LLVMValueRef target_ptr = LLVMBuildIntToPtr(
      cg->builder, target_v, ny_ptr_i64_ty(cg), "fast_index_ptr_i64");

  if (assume_in_bounds) {
    LLVMValueRef elem_idx =
        LLVMBuildAdd(cg->builder, key_raw, LLVMConstInt(cg->type_i64, 2, false),
                     "fast_index_inbounds_gep_idx");
    LLVMValueRef elem_ptr =
        LLVMBuildGEP2(cg->builder, cg->type_i64, target_ptr, &elem_idx, 1,
                      "fast_index_inbounds_elem_gep");
    return ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "fast_index_elem"));
  }

  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef load_bb = ny_bb_fn(fn, "fast_index.load");
  LLVMBasicBlockRef fallback_bb = ny_bb_fn(fn, "fast_index.fallback");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "fast_index.join");

  LLVMValueRef len_tagged =
      ny_load(cg, target_ptr, NY_LLVM_NAME(cg, "fast_index_len"));
  LLVMValueRef len_raw = ny_untag_int(cg, len_tagged);

  LLVMValueRef adj_idx = key_raw;
  LLVMValueRef low_ok = LLVMConstInt(cg->type_i1, 1, false);
  if (!assume_nonnegative) {
    LLVMValueRef is_neg =
        LLVMBuildICmp(cg->builder, LLVMIntSLT, key_raw, ny_c0(cg),
                      NY_LLVM_NAME(cg, "fast_index_is_neg"));
    LLVMValueRef wrapped =
        ny_add(cg, key_raw, len_raw, NY_LLVM_NAME(cg, "fast_index_wrapped"));
    adj_idx = ny_select(cg, is_neg, wrapped, key_raw,
                        NY_LLVM_NAME(cg, "fast_index_idx"));
    low_ok = LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_idx, ny_c0(cg),
                           NY_LLVM_NAME(cg, "fast_index_low_ok"));
  }
  LLVMValueRef high_ok = NULL;
  if (assume_nonnegative &&
      ny_is_proven_int(cg, scopes, depth, e->as.index.start, key_v)) {
    high_ok = LLVMBuildICmp(cg->builder, LLVMIntSLT, key_v, len_tagged,
                            NY_LLVM_NAME(cg, "fast_index_high_ok_tagged"));
  } else {
    high_ok = LLVMBuildICmp(cg->builder, LLVMIntSLT, adj_idx, len_raw,
                            NY_LLVM_NAME(cg, "fast_index_high_ok"));
  }
  LLVMValueRef in_bounds =
      ny_and(cg, low_ok, high_ok, NY_LLVM_NAME(cg, "fast_index_in_bounds"));
  ny_cond_br(cg, in_bounds, load_bb, fallback_bb);

  ny_pos(cg, load_bb);
  LLVMValueRef elem_idx =
      LLVMBuildAdd(cg->builder, adj_idx, LLVMConstInt(cg->type_i64, 2, false),
                   "fast_index_gep_idx");
  LLVMValueRef elem_ptr = LLVMBuildGEP2(cg->builder, cg->type_i64, target_ptr,
                                        &elem_idx, 1, "fast_index_elem_gep");
  LLVMValueRef elem_val =
      ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "fast_index_elem"));
  LLVMBasicBlockRef load_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, fallback_bb);
  LLVMValueRef args[2] = {target_v, key_v};
  LLVMValueRef fallback =
      LLVMBuildCall2(cg->builder, fallback_sig->type, fallback_sig->value, args,
                     2, NY_LLVM_NAME(cg, "fast_index_fallback"));
  LLVMBasicBlockRef fallback_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "fast_index_result"));
  LLVMValueRef incoming_vals[2] = {elem_val, fallback};
  LLVMBasicBlockRef incoming_bbs[2] = {load_end_bb, fallback_end_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

static fun_sig *ny_helper_dict(codegen_t *cg) {
  static const char *const k_names[] = {"dict", "core.dict",
                                        "core.dict_mod.dict"};
  return ny_helper_lookup(cg, &cg->cached_fn_dict, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_set(codegen_t *cg) {
  static const char *const k_names[] = {"core.set", "set"};
  return ny_helper_lookup(cg, &cg->cached_fn_set, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_set_add(codegen_t *cg) {
  static const char *const k_names[] = {"set_add", "core.set_add"};
  return ny_helper_lookup(cg, &cg->cached_fn_set_add, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_flt_box(codegen_t *cg) {
  static const char *const k_names[] = {"__flt_box_val"};
  return ny_helper_lookup(cg, &cg->cached_fn_flt_box, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_str_concat(codegen_t *cg) {
  static const char *const k_names[] = {"__str_concat"};
  return ny_helper_lookup(cg, &cg->cached_fn_str_concat, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_to_str(codegen_t *cg) {
  static const char *const k_names[] = {"to_str", "core.to_str",
                                        "core.reflect.to_str", "__to_str"};
  return ny_helper_lookup(cg, &cg->cached_fn_to_str, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static LLVMValueRef ny_ct_fast_to_llvm_value(codegen_t *cg,
                                             const ny_ct_fast_val_t *v,
                                             token_t tok) {
  if (!cg || !v)
    return NULL;
  if (v->kind == NY_CT_FAST_NONE)
    return ny_c0(cg);
  if (v->kind == NY_CT_FAST_BOOL)
    return v->b ? ny_ctrue(cg) : ny_cfalse(cg);
  if (v->kind == NY_CT_FAST_FLOAT) {
    int64_t bits = 0;
    memcpy(&bits, &v->f64, sizeof(bits));
    fun_sig *box_float = lookup_fun(cg, "__flt_box_val", 0);
    if (!box_float)
      return expr_fail(cg, tok, "builtin __flt_box_val missing");
    return LLVMBuildCall2(
        cg->builder, box_float->type, box_float->value,
        (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)bits, false)}, 1,
        NY_LLVM_NAME(cg, "ct_float"));
  }
  char int_big_buf[64];
  const char *int_big_lit = NULL;
  if (v->kind == NY_CT_FAST_INT) {
    if (ny_small_int_fits_i64(v->i))
      return LLVMConstInt(cg->type_i64, (((uint64_t)v->i) << 1) | 1u, true);
    snprintf(int_big_buf, sizeof(int_big_buf), "%" PRId64, v->i);
    int_big_lit = int_big_buf;
  }
  if (v->kind == NY_CT_FAST_STR) {
    const char *s = v->s ? v->s : "";
    LLVMValueRef g = const_string_ptr(cg, s, strlen(s));
    return ny_load(cg, g, NY_LLVM_NAME(cg, "ct_str"));
  }
  if (int_big_lit || v->kind == NY_CT_FAST_BIGINT) {
    const char *s = int_big_lit ? int_big_lit : (v->s ? v->s : "0");
    LLVMValueRef str_runtime_global = const_string_ptr(cg, s, strlen(s));
    LLVMValueRef str_ptr = ny_load(cg, str_runtime_global, "ct_big_lit_str");
    fun_sig *big_from_str = lookup_fun(cg, "__bigint_from_str", 0);
    if (!big_from_str)
      return expr_fail(cg, tok, "builtin __bigint_from_str missing");
    return LLVMBuildCall2(cg->builder, big_from_str->type, big_from_str->value,
                          (LLVMValueRef[]){str_ptr}, 1, "ct_big_lit");
  }
  if (v->kind == NY_CT_FAST_LIST || v->kind == NY_CT_FAST_TUPLE) {
    fun_sig *list_new = lookup_fun(cg, "__list_new", 0);
    fun_sig *store_item = lookup_fun(cg, "__store_item_fast", 0);
    fun_sig *set_len = lookup_fun(cg, "__list_set_len", 0);
    if (!list_new || !store_item || !set_len)
      return expr_fail(cg, tok,
                       "comptime list result requires list runtime helpers");
    LLVMValueRef tagged_len =
        LLVMConstInt(cg->type_i64, (((uint64_t)v->len << 1) | 1u), false);
    LLVMValueRef out = LLVMBuildCall2(
        cg->builder, list_new->type, list_new->value,
        (LLVMValueRef[]){tagged_len}, 1, NY_LLVM_NAME(cg, "ct_interp_list"));
    for (size_t i = 0; i < v->len; i++) {
      LLVMValueRef item = ny_ct_fast_to_llvm_value(cg, &v->items[i], tok);
      if (!item)
        return NULL;
      (void)LLVMBuildCall2(
          cg->builder, store_item->type, store_item->value,
          (LLVMValueRef[]){
              out, LLVMConstInt(cg->type_i64, (((uint64_t)i << 1) | 1u), false),
              item},
          3, "");
    }
    (void)LLVMBuildCall2(cg->builder, set_len->type, set_len->value,
                         (LLVMValueRef[]){out, tagged_len}, 2, "");
    if (v->kind == NY_CT_FAST_TUPLE) {
      fun_sig *as_tuple = lookup_fun(cg, "__list_as_tuple", 0);
      if (!as_tuple)
        return expr_fail(cg, tok,
                         "comptime tuple result requires tuple runtime helper");
      out = LLVMBuildCall2(cg->builder, as_tuple->type, as_tuple->value,
                           (LLVMValueRef[]){out}, 1,
                           NY_LLVM_NAME(cg, "ct_interp_tuple"));
    }
    return out;
  }
  if (v->kind == NY_CT_FAST_DICT) {
    if (v->len > (size_t)(INT64_MAX / 2))
      return expr_fail(cg, tok, "comptime dictionary is too large");
    fun_sig *dict_new = lookup_fun(cg, "rt_dict_new_raw", 0);
    fun_sig *dict_set = lookup_fun(cg, "rt_native_dict_set_nir_i64", 0);
    if (!dict_new || !dict_set)
      return expr_fail(
          cg, tok,
          "comptime dictionary result requires dictionary runtime helpers");
    int64_t capacity = v->len < 4 ? 8 : (int64_t)v->len * 2;
    LLVMValueRef out = LLVMBuildCall2(
        cg->builder, dict_new->type, dict_new->value,
        (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)capacity, false)},
        1, NY_LLVM_NAME(cg, "ct_interp_dict"));
    for (size_t i = 0; i < v->len; ++i) {
      LLVMValueRef key = ny_ct_fast_to_llvm_value(cg, &v->items[i * 2], tok);
      LLVMValueRef item =
          ny_ct_fast_to_llvm_value(cg, &v->items[i * 2 + 1], tok);
      if (!key || !item)
        return NULL;
      (void)LLVMBuildCall2(cg->builder, dict_set->type, dict_set->value,
                           (LLVMValueRef[]){out, key, item}, 3, "");
    }
    return out;
  }
  if (v->kind == NY_CT_FAST_RANGE) {
    if (!ny_small_int_fits_i64(v->range_start) ||
        !ny_small_int_fits_i64(v->range_stop) ||
        !ny_small_int_fits_i64(v->range_step))
      return expr_fail(cg, tok, "comptime range bounds are out of range");
    fun_sig *range_new = lookup_fun(cg, "__range_new", 0);
    if (!range_new)
      return expr_fail(cg, tok,
                       "comptime range result requires range runtime helper");
    LLVMValueRef start = LLVMConstInt(
        cg->type_i64, (((uint64_t)v->range_start << 1) | 1u), true);
    LLVMValueRef stop =
        LLVMConstInt(cg->type_i64, (((uint64_t)v->range_stop << 1) | 1u), true);
    LLVMValueRef step =
        LLVMConstInt(cg->type_i64, (((uint64_t)v->range_step << 1) | 1u), true);
    return LLVMBuildCall2(cg->builder, range_new->type, range_new->value,
                          (LLVMValueRef[]){start, stop, step}, 3,
                          NY_LLVM_NAME(cg, "ct_interp_range"));
  }
  return NULL;
}

LLVMValueRef gen_comptime_eval(codegen_t *cg, stmt_t *body) {
  ny_ct_fast_val_t fast_value = ny_ct_fast_none();
  if (ny_try_eval_comptime_fast_value(cg, body, &fast_value)) {
    LLVMValueRef value = ny_ct_fast_to_llvm_value(cg, &fast_value, body->tok);
    ny_ct_fast_val_free(&fast_value);
    if (value)
      return value;
  }
  ny_ct_fast_val_free(&fast_value);

  ny_ct_fast_val_t interp_value = ny_ct_fast_none();
  if (ny_try_eval_comptime_interp_value(cg, body, &interp_value)) {
    LLVMValueRef value = ny_ct_fast_to_llvm_value(cg, &interp_value, body->tok);
    ny_ct_fast_val_free(&interp_value);
    if (value)
      return value;
    if (getenv("NYTRIX_COMPTIME_INTERP_ONLY"))
      return NULL;
  }
  ny_ct_fast_val_free(&interp_value);

  if (cg->env_cache.comptime_fuel_total > 0 &&
      cg->env_cache.comptime_fuel_remaining <= 0)
    return expr_fail(cg, body->tok,
                     "comptime fuel exhausted (%" PRId64
                     " steps); increase --comptime-limit or "
                     "NYTRIX_COMPTIME_FUEL",
                     cg->env_cache.comptime_fuel_total);

  return expr_fail(cg, body->tok,
                   "comptime evaluator cannot evaluate this form");
}

static LLVMValueRef gen_unary_op_common(
    codegen_t *cg, expr_t *e, LLVMValueRef r,
    LLVMValueRef (*build_fast)(LLVMBuilderRef, LLVMValueRef, const char *),
    const char *op_name, fun_sig *slow_sig, LLVMValueRef *slow_args,
    int slow_argc) {
  if (!slow_sig || !slow_sig->type || !slow_sig->value)
    return expr_fail(cg, e->tok, "builtin for %s missing", op_name);

  LLVMBasicBlockRef entry_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);

  LLVMBasicBlockRef fast_bb = ny_bb_fn(fn, "un.int.fast");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, "un.runtime.slow");
  LLVMBasicBlockRef merge_bb = ny_bb_fn(fn, "un.merge");
  ny_cond_br(cg, ny_is_tagged_int(cg, r), fast_bb, slow_bb);

  ny_pos(cg, fast_bb);

  LLVMValueRef raw = ny_untag_int(cg, r);
  LLVMValueRef res_raw = build_fast(cg->builder, raw, op_name);
  LLVMValueRef fast_value = ny_tag_int(cg, res_raw);
  LLVMBasicBlockRef fast_done_bb = ny_cur_block(cg);
  ny_br(cg, merge_bb);

  ny_pos(cg, slow_bb);

  LLVMValueRef slow_value =
      LLVMBuildCall2(cg->builder, slow_sig->type, slow_sig->value, slow_args,
                     (unsigned)slow_argc, "");
  LLVMBasicBlockRef slow_done_bb = ny_cur_block(cg);

  ny_br(cg, merge_bb);

  ny_pos(cg, merge_bb);

  LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "un_result"));
  LLVMValueRef incoming_vals[2] = {fast_value, slow_value};
  LLVMBasicBlockRef incoming_bbs[2] = {fast_done_bb, slow_done_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

static LLVMValueRef gen_expr_unary(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e) {
  if (!e->as.unary.right)
    return expr_fail(cg, e->tok, "missing operand for unary '%s'",
                     e->as.unary.op);
  if (strcmp(e->as.unary.op, "-") == 0 &&
      e->as.unary.right->kind == NY_E_LITERAL) {
    expr_t *right = e->as.unary.right;
    if (right->as.literal.kind == NY_LIT_INT && right->tok.kind != NY_T_NIL) {
      int64_t raw = right->as.literal.as.i;
      if (ny_small_int_fits_i64(raw) && raw != INT64_MIN &&
          ny_small_int_fits_i64(-raw))
        return LLVMConstInt(cg->type_i64, (((uint64_t)(-raw)) << 1) | 1u, true);
    } else if (right->as.literal.kind == NY_LIT_FLOAT) {
      fun_sig *box_sig = ny_helper_flt_box(cg);
      if (!box_sig)
        return expr_fail(cg, e->tok, "__flt_box_val not found");
      double d = -right->as.literal.as.f;
      if (right->as.literal.hint == NY_LIT_HINT_F32)
        d = (double)(float)d;
      LLVMValueRef fval = LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), d);
      return LLVMBuildCall2(
          cg->builder, box_sig->type, box_sig->value,
          (LLVMValueRef[]){ny_bitcast(cg, fval, cg->type_i64, "")}, 1, "");
    }
  }
  if (strcmp(e->as.unary.op, "await") == 0) {
    LLVMValueRef handle = gen_expr(cg, scopes, depth, e->as.unary.right);
    if (!handle)
      return expr_fail(cg, e->tok, "failed to evaluate await operand");
    fun_sig *join_sig = lookup_fun(cg, "__async_await_blocking", 0);
    if (!join_sig)
      return expr_fail(cg, e->tok, "builtin __async_await_blocking missing");
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, join_sig->type, join_sig->value, &handle,
                          1, "async_await");
  }
  if (strcmp(e->as.unary.op, "async") == 0) {
    expr_t *task = e->as.unary.right;
    expr_t *callee = task;
    ny_call_arg_list *call_args = NULL;
    size_t argc = 0;
    if (task->kind == NY_E_CALL) {
      callee = task->as.call.callee;
      call_args = &task->as.call.args;
      argc = call_args->len;
    } else if (task->kind == NY_E_MEMCALL) {
      return expr_fail(cg, e->tok,
                       "async member calls should be wrapped in a function or "
                       "lambda");
    }
    if (!callee)
      return expr_fail(cg, e->tok, "async requires a callable expression");
    if (argc > 15)
      return expr_fail(cg, e->tok,
                       "async calls support up to 15 arguments; pass a packed "
                       "object for more");
    LLVMValueRef fn = gen_expr(cg, scopes, depth, callee);
    if (!fn)
      return expr_fail(cg, e->tok, "failed to evaluate async callable");
    LLVMValueRef args[15];
    for (size_t i = 0; i < argc; i++) {
      call_arg_t *arg = &call_args->data[i];
      if (arg->name)
        return expr_fail(cg, task->tok,
                         "async call syntax does not support named arguments");
      args[i] = gen_expr(cg, scopes, depth, arg->val);
      if (!args[i])
        return expr_fail(cg, task->tok, "failed to evaluate async argument");
    }
    fun_sig *spawn_sig = lookup_fun(cg, "__async_task_new", 0);
    if (!spawn_sig)
      return expr_fail(cg, e->tok, "builtin __async_task_new missing");
    LLVMValueRef fn_val =
        (LLVMTypeOf(fn) == cg->type_i64)
            ? fn
            : ny_ptr2i64(cg, fn, NY_LLVM_NAME(cg, "async_fn"));
    /*
     * Runtime callback APIs expose argc through a raw C integer parameter.
     */
    LLVMValueRef argc_val = LLVMConstInt(cg->type_i64, (uint64_t)argc, false);
    LLVMValueRef argv_ptr = ny_c0(cg);
    if (argc > 0) {
      LLVMTypeRef argv_ty = LLVMArrayType(cg->type_i64, (unsigned)argc);
      ny_dbg_loc(cg, e->tok);
      LLVMValueRef argv_stack =
          LLVMBuildAlloca(cg->builder, argv_ty, NY_LLVM_NAME(cg, "async_argv"));
      LLVMSetAlignment(argv_stack, 16);
      for (size_t i = 0; i < argc; i++) {
        LLVMValueRef idxs[2] = {ny_c0(cg),
                                LLVMConstInt(cg->type_i64, (uint64_t)i, false)};
        LLVMValueRef slot =
            LLVMBuildGEP2(cg->builder, argv_ty, argv_stack, idxs, 2, "");
        ny_store(cg, slot, args[i]);
      }
      argv_ptr = ny_ptr2i64(cg, argv_stack, "async_argv_ptr");
    }
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, spawn_sig->type, spawn_sig->value,
                          (LLVMValueRef[]){fn_val, argc_val, argv_ptr}, 3,
                          "async_task");
  }
  LLVMValueRef r = gen_expr(cg, scopes, depth, e->as.unary.right);
  if (!r)
    return expr_fail(cg, e->tok, "failed to evaluate unary operand");
  ny_dbg_loc(cg, e->tok);
  if (strcmp(e->as.unary.op, "!") == 0)
    return ny_select(cg, to_bool(cg, r), ny_cfalse(cg), ny_ctrue(cg), "");
  if (strcmp(e->as.unary.op, "-") == 0) {
    fun_sig *s = ny_helper_sub(cg);
    if (!s)
      return expr_fail(cg, e->tok, "builtin __sub missing");
    LLVMValueRef args[] = {ny_c1(cg), r};
    return LLVMBuildCall2(cg->builder, s->type, s->value, args, 2,
                          "neg_runtime");
  }
  if (strcmp(e->as.unary.op, "~") == 0) {
    fun_sig *s = ny_helper_not(cg);
    if (!s)
      return expr_fail(cg, e->tok, "builtin __not missing");
    LLVMValueRef args[] = {r};
    return gen_unary_op_common(cg, e, r, LLVMBuildNot, "not_int", s, args, 1);
  }
  return expr_fail(cg, e->tok, "unsupported unary operator '%s'",
                   e->as.unary.op);
}

static bool expr_target_is_literal_backed_list(codegen_t *cg, scope *scopes,
                                               size_t depth, expr_t *target) {
  if (!target)
    return false;
  if (target->kind == NY_E_LIST || target->kind == NY_E_TUPLE)
    return true;
  if (target->kind != NY_E_IDENT || !target->as.ident.name)
    return false;
  size_t name_len = (size_t)target->tok.len;
  if (name_len == 0)
    name_len = strlen(target->as.ident.name);
  binding *b = lookup_binding_hash(cg, scopes, depth, target->as.ident.name,
                                   name_len, target->as.ident.hash);
  if (!b)
    return false;
  expr_t *init = ny_binding_var_init_expr(b, target->as.ident.name);
  return init && (init->kind == NY_E_LIST || init->kind == NY_E_TUPLE);
}

static LLVMValueRef expr_decode_proven_int_index(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *expr,
                                                 LLVMValueRef value) {
  if (!value || !ny_is_proven_int(cg, scopes, depth, expr, NULL))
    return value;

  fun_sig *to_i64 = lookup_fun(cg, "__any_to_i64", 0);
  if (!to_i64)
    return expr_fail(cg, expr ? expr->tok : (token_t){0},
                     "builtin __any_to_i64 missing for dynamic integer index");
  return LLVMBuildCall2(cg->builder, to_i64->type, to_i64->value,
                        (LLVMValueRef[]){value}, 1,
                        NY_LLVM_NAME(cg, "index_int_value"));
}

static LLVMValueRef gen_expr_index(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e) {
  if (e->as.index.stop || e->as.index.step || !e->as.index.start) {
    fun_sig *s = ny_helper_slice(cg);
    if (!s)
      return expr_fail(cg, e->tok, "slice operation requires 'slice'");
    LLVMValueRef start = e->as.index.start
                             ? gen_expr(cg, scopes, depth, e->as.index.start)
                             : ny_c1(cg);
    LLVMValueRef stop =
        e->as.index.stop
            ? gen_expr(cg, scopes, depth, e->as.index.stop)
            : LLVMConstInt(cg->type_i64, ((0x3fffffffULL) << 1) | 1, false);
    LLVMValueRef step = e->as.index.step
                            ? gen_expr(cg, scopes, depth, e->as.index.step)
                            : LLVMConstInt(cg->type_i64, 3, false);
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(
        cg->builder, s->type, s->value,
        (LLVMValueRef[]){gen_expr(cg, scopes, depth, e->as.index.target), start,
                         stop, step},
        4, "");
  }
  /*
   * String indexing is a character lookup, not a raw native-buffer lookup.
   * An inferred/unknown string local can otherwise enter the dynamic index
   * fast path below; that path is valid for tbuf-backed sequences but can
   * interpret a C-string payload as a suffix slice.  Keep the ordinary
   * checked index contract for every statically identifiable string.
   */
  if (e->as.index.target && e->as.index.start &&
      expr_target_is_known_string_like(cg, scopes, depth, e->as.index.target)) {
    fun_sig *string_index = ny_helper_index_read(cg);
    if (string_index) {
      LLVMValueRef target = gen_expr(cg, scopes, depth, e->as.index.target);
      LLVMValueRef key = gen_expr(cg, scopes, depth, e->as.index.start);
      if (target && key)
        return LLVMBuildCall2(cg->builder, string_index->type,
                              string_index->value,
                              (LLVMValueRef[]){target, key}, 2,
                              NY_LLVM_NAME(cg, "string_index_read"));
    }
  }
  /*
   * Literal-backed lists have representation proof strong enough to bypass
   * the dynamic tbuf bridge.  Try the existing SROA/static/direct-index
   * machinery before the conservative generic list path below.  Keeping
   * this targeted avoids assuming that arbitrary list-like values (notably
   * native typed buffers) use the legacy managed-list header layout.
   */
  if (expr_target_is_literal_backed_list(cg, scopes, depth,
                                         e->as.index.target) &&
      expr_index_is_int_key(cg, scopes, depth, e->as.index.start)) {
    fun_sig *literal_index = ny_helper_index_read(cg);
    LLVMValueRef fast =
        literal_index
            ? expr_try_fast_index_read(cg, scopes, depth, e, literal_index)
            : NULL;
    if (fast)
      return fast;
  }

  if (expr_target_is_known_list_like(cg, scopes, depth, e->as.index.target) &&
      expr_index_is_int_key(cg, scopes, depth, e->as.index.start)) {
    LLVMValueRef target_v = gen_expr(cg, scopes, depth, e->as.index.target);
    LLVMValueRef key_v = gen_expr(cg, scopes, depth, e->as.index.start);
    if (target_v && key_v) {
      target_v = ny_llvm_cast_to_i64(cg, target_v, "dynamic_index_target");
      key_v = ny_llvm_cast_to_i64(cg, key_v, "dynamic_index_key");
      LLVMValueRef key_raw = expr_index_raw_i64(
          cg, scopes, depth, e->as.index.start, key_v, "dynamic_index_key_raw");
      LLVMTypeRef args_ty[2] = {cg->type_i64, cg->type_i64};
      LLVMTypeRef fn_ty = LLVMFunctionType(cg->type_i64, args_ty, 2, 0);
      /*
       * key_raw is already an untagged native index.  Use the raw bridge;
       * the tagged entry point would interpret odd raw indices (1, 3, ...)
       * as language integers and untag them a second time.
       */
      LLVMValueRef fn =
          LLVMGetNamedFunction(cg->module, "rt_tbuf_index_any_raw");
      if (!fn)
        fn = LLVMAddFunction(cg->module, "rt_tbuf_index_any_raw", fn_ty);
      if (fn && key_raw)
        return expr_decode_proven_int_index(
            cg, scopes, depth, e,
            LLVMBuildCall2(cg->builder, fn_ty, fn,
                           (LLVMValueRef[]){target_v, key_raw}, 2,
                           NY_LLVM_NAME(cg, "dynamic_index_any")));
    }
  }

  /*
   * An any-valued container carries a raw index at this ABI boundary.  Keep
   * literal integer subscripts on the explicit raw bridge instead of passing
   * the unboxed value to the legacy tagged accessor (where 1 would be read as
   * tagged zero).  Restrict this fast path to literals; dynamic keys continue
   * through index_read so its type and bounds diagnostics remain intact.
   */
  const char *target_type =
      infer_expr_type(cg, scopes, depth, e->as.index.target);
  bool dynamic_target =
      !target_type || ny_gencall_type_is(target_type, "any") ||
      ny_gencall_type_is(target_type, "unknown") ||
      /*
       * `bytes` is a managed runtime sequence, but it is not represented by
       * the legacy list/tbuf ABI.  Keep it on the canonical value dispatcher
       * so generic indexing observes the actual runtime tag instead of
       * falling through to the source-level index_read helper.
       */
      ny_gencall_type_is(target_type, "bytes");
  bool string_target =
      expr_target_is_known_string_like(cg, scopes, depth, e->as.index.target);
  if (dynamic_target && !string_target &&
      e->as.index.start->kind == NY_E_LITERAL &&
      e->as.index.start->as.literal.kind == NY_LIT_INT &&
      e->as.index.start->as.literal.as.i >= 0) {
    LLVMValueRef target_v = gen_expr(cg, scopes, depth, e->as.index.target);
    LLVMValueRef key_v = gen_expr(cg, scopes, depth, e->as.index.start);
    if (target_v && key_v) {
      target_v = ny_llvm_cast_to_i64(cg, target_v, "raw_index_target");
      key_v = ny_llvm_cast_to_i64(cg, key_v, "raw_index_key");
      LLVMValueRef key_raw = expr_index_raw_i64(
          cg, scopes, depth, e->as.index.start, key_v, "raw_index_key_value");
      LLVMTypeRef args_ty[3] = {cg->type_i64, cg->type_i64, cg->type_i64};
      LLVMTypeRef fn_ty = LLVMFunctionType(cg->type_i64, args_ty, 3, 0);
      LLVMValueRef fn =
          LLVMGetNamedFunction(cg->module, "rt_value_get_index_raw");
      if (!fn)
        fn = LLVMAddFunction(cg->module, "rt_value_get_index_raw", fn_ty);
      if (fn && key_raw)
        return expr_decode_proven_int_index(
            cg, scopes, depth, e,
            LLVMBuildCall2(cg->builder, fn_ty, fn,
                           (LLVMValueRef[]){target_v, key_raw, ny_c1(cg)}, 3,
                           NY_LLVM_NAME(cg, "raw_dynamic_index_get")));
    }
  }
  fun_sig *s = ny_helper_index_read(cg);
  if (!s)
    return expr_fail(cg, e->tok, "index operation requires 'index_read'");
  LLVMValueRef fast = expr_try_fast_index_read(cg, scopes, depth, e, s);
  if (fast)
    return fast;
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef args[2];
  args[0] = gen_expr(cg, scopes, depth, e->as.index.target);
  args[1] = gen_expr(cg, scopes, depth, e->as.index.start);
  LLVMValueRef value =
      LLVMBuildCall2(cg->builder, s->type, s->value, args, 2, "");
  return expr_decode_proven_int_index(cg, scopes, depth, e, value);
}
