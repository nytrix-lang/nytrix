#include "base/common.h"
#include "base/util.h"

#include "llvm.h"
#include "nullnarrow.h"
#include "priv.h"
#include "rt/shared.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif
#include "jit.h"
#include <inttypes.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
  uint64_t count;
  double total_ms;
  double self_ms;
} ny_codegen_kind_prof_t;

static ny_codegen_kind_prof_t g_expr_kind_prof[64];
static double g_expr_kind_child_ms[4096];
static int g_expr_kind_depth;
static int g_expr_kind_profile_enabled = -1;
static int g_expr_kind_profile_registered;

static bool ny_codegen_expr_kind_profile_enabled(void) {
  if (g_expr_kind_profile_enabled < 0) {
    const char *env = getenv("NYTRIX_PROFILE_CODEGEN_KINDS");
    if (!env || !*env)
      env = getenv("NYTRIX_PROFILE_CODEGEN_EXPR");
    g_expr_kind_profile_enabled =
        (env && *env && strcmp(env, "0") != 0 && strcmp(env, "false") != 0 &&
         strcmp(env, "off") != 0)
            ? 1
            : 0;
  }
  return g_expr_kind_profile_enabled == 1;
}

static const char *ny_expr_kind_profile_name(int kind) {
  switch (kind) {
  case NY_E_IDENT: return "IDENT";
  case NY_E_LITERAL: return "LITERAL";
  case NY_E_UNARY: return "UNARY";
  case NY_E_BINARY: return "BINARY";
  case NY_E_LOGICAL: return "LOGICAL";
  case NY_E_TERNARY: return "TERNARY";
  case NY_E_CALL: return "CALL";
  case NY_E_MEMCALL: return "MEMCALL";
  case NY_E_INDEX: return "INDEX";
  case NY_E_LAMBDA: return "LAMBDA";
  case NY_E_FN: return "FN";
  case NY_E_LIST: return "LIST";
  case NY_E_TUPLE: return "TUPLE";
  case NY_E_DICT: return "DICT";
  case NY_E_SET: return "SET";
  case NY_E_ASM: return "ASM";
  case NY_E_COMPTIME: return "COMPTIME";
  case NY_E_FSTRING: return "FSTRING";
  case NY_E_INFERRED_MEMBER: return "INFERRED_MEMBER";
  case NY_E_EMBED: return "EMBED";
  case NY_E_MATCH: return "MATCH";
  case NY_E_MEMBER: return "MEMBER";
  case NY_E_PTR_TYPE: return "PTR_TYPE";
  case NY_E_DEREF: return "DEREF";
  case NY_E_SIZEOF: return "SIZEOF";
  case NY_E_TRY: return "TRY";
  default: return "UNKNOWN";
  }
}

static void ny_codegen_expr_kind_profile_report(void) {
  if (!ny_codegen_expr_kind_profile_enabled())
    return;
  for (int i = 0; i < (int)(sizeof(g_expr_kind_prof) / sizeof(g_expr_kind_prof[0])); ++i) {
    ny_codegen_kind_prof_t p = g_expr_kind_prof[i];
    if (p.count == 0)
      continue;
    fprintf(stderr,
            "[codegen-expr-kind] kind=%s count=%" PRIu64
            " total_ms=%.3f self_ms=%.3f avg_us=%.3f\n",
            ny_expr_kind_profile_name(i), p.count, p.total_ms, p.self_ms,
            (p.total_ms * 1000.0) / (double)p.count);
  }
}

static void ny_codegen_expr_kind_profile_add(int kind, double total_ms, double child_ms) {
  if (kind < 0 || kind >= (int)(sizeof(g_expr_kind_prof) / sizeof(g_expr_kind_prof[0])))
    kind = (int)(sizeof(g_expr_kind_prof) / sizeof(g_expr_kind_prof[0])) - 1;
  double self_ms = total_ms - child_ms;
  if (self_ms < 0.0)
    self_ms = 0.0;
  g_expr_kind_prof[kind].count++;
  g_expr_kind_prof[kind].total_ms += total_ms;
  g_expr_kind_prof[kind].self_ms += self_ms;
}

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

static binding *expr_lookup_binding(codegen_t *cg, scope *scopes, size_t depth,
                                    const char *name, size_t name_len, uint64_t hash) {
  return lookup_binding_hash(cg, scopes, depth, name, name_len, hash);
}

static LLVMValueRef ny_binding_tagged_int_value(codegen_t *cg, binding *b);

static LLVMValueRef expr_value_from_binding(codegen_t *cg, binding *b) {
  if (!cg || !b)
    return ny_c0(cg);
  b->is_used = true;
  if (b->is_f64_slot || b->is_f64_direct || b->is_f32_slot || b->is_f32_direct) {
    LLVMValueRef fv;
    if (b->is_f64_slot || b->is_f64_direct) {
      fv = b->is_slot ? LLVMBuildLoad2(cg->builder, cg->type_f64, b->value, "f64_ld")
                      : b->value;
    } else {
      fv = b->is_slot ? LLVMBuildLoad2(cg->builder, cg->type_f32, b->value, "f32_ld")
                      : b->value;
      fv = LLVMBuildFPExt(cg->builder, fv, cg->type_f64, "f2f");
    }
    fun_sig *box_sig = lookup_fun(cg, "__flt_box_val", 0);
    if (box_sig)
      return LLVMBuildCall2(cg->builder, box_sig->type, box_sig->value,
                            (LLVMValueRef[]){ny_bitcast(cg, fv, cg->type_i64, "")}, 1,
                            "box");
  }
  if (b->is_slot)
    return ny_load(cg, b->value, "");
  if (b->is_int_direct && b->is_int_raw_direct)
    return ny_binding_tagged_int_value(cg, b);
  return b->value;
}

static const char *expr_member_module_alias_target(codegen_t *cg, scope *scopes,
                                                   size_t depth, expr_t *e) {
  if (!cg || !e || e->kind != NY_E_MEMBER || !e->as.member.target ||
      e->as.member.target->kind != NY_E_IDENT || !e->as.member.target->as.ident.name ||
      !e->as.member.name)
    return NULL;
  expr_t *target = e->as.member.target;
  const char *target_name = target->as.ident.name;
  size_t target_len = (size_t)target->tok.len;
  if (target_len == 0)
    target_len = strlen(target_name);
  return ny_lookup_module_alias(cg, scopes, depth, target_name, target_len,
                                target->as.ident.hash);
}

static binding *expr_member_module_alias_global(codegen_t *cg, scope *scopes, size_t depth,
                                                expr_t *e, char *out, size_t out_cap) {
  const char *module_name = expr_member_module_alias_target(cg, scopes, depth, e);
  if (!module_name)
    return NULL;
  if (!e || !e->as.member.name)
    return NULL;
  {
    char stack_name[512];
    char *name_buf = out && out_cap > 0 ? out : stack_name;
    size_t name_cap = out && out_cap > 0 ? out_cap : sizeof(stack_name);
    int nw = snprintf(name_buf, name_cap, "%s.%s", module_name, e->as.member.name);
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

static LLVMValueRef expr_cast_to_i64(codegen_t *cg, LLVMValueRef v,
                                     const char *name) {
  if (!cg || !v)
    return v;
  if (LLVMTypeOf(v) == cg->type_i64)
    return v;
  return ny_ptr2i64(cg, v, ny_llvm_name(cg, name));
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
      "int", "i8",  "i16", "i32", "i64",
      "u8",  "u16", "u32", "u64",
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
    binding *b = expr_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                     name_len, target->as.ident.hash);
    return b && b->is_list_storage;
  }
  return false;
}

static binding *expr_f64_list_target_binding(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *target) {
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
    return NULL;
  size_t name_len = (size_t)target->tok.len;
  if (name_len == 0)
    name_len = strlen(target->as.ident.name);
  binding *b = expr_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                   name_len, target->as.ident.hash);
  return (b && b->is_list_storage && b->is_f64_list_storage) ? b : NULL;
}

static bool expr_is_f64_default_value(codegen_t *cg, scope *scopes, size_t depth,
                                      expr_t *e) {
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
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name,
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
                                       LLVMValueRef idx_v,
                                       const char *name) {
  idx_v = expr_cast_to_i64(cg, idx_v, name ? name : "index");
  int64_t lit = 0;
  if (ny_expr_literal_i64(idx_expr, &lit))
    return LLVMConstInt(cg->type_i64, (uint64_t)lit, true);
  if (idx_expr && idx_expr->kind == NY_E_IDENT && idx_expr->as.ident.name) {
    size_t name_len = (size_t)idx_expr->tok.len;
    if (name_len == 0)
      name_len = strlen(idx_expr->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth,
                                     idx_expr->as.ident.name, name_len,
                                     idx_expr->as.ident.hash);
    if (b && b->raw_int_value && b->is_int_direct)
      return b->raw_int_value;
    if (ny_env_enabled("NYTRIX_RAW_INT_SLOT_EXPR_FAST") && b &&
        b->raw_int_value && b->is_int_slot && !ny_binding_is_valid(cg, b))
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
  binding *b = expr_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                   name_len, target->as.ident.hash);
  if (!b || !b->has_list_len_min)
    return false;
  if (out_min_len)
    *out_min_len = b->list_len_min_raw;
  return true;
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
    const char *name =
        ny_builtin_surface_name_for_callee(e->as.call.callee, &name_len,
                                           &name_hash);
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
  binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name,
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

static bool ny_comptime_main_enabled(codegen_t *cg, token_t tok) {
  if (cg && cg->source_main_file && *cg->source_main_file && tok.filename &&
      *tok.filename)
    return ny_codegen_token_is_source_file(cg, tok);
  return ny_env_enabled("NYTRIX_TEST_MODE");
}

static bool ny_lambda_values_need_closure(codegen_t *cg) {
  return ny_env_enabled("NYTRIX_FORCE_CLOSURE_LAMBDAS") ||
         ny_module_target_is_apple_arm64(cg ? cg->module : NULL);
}

static bool ny_ct_platform_ident(const char *name, ny_ct_fast_val_t *out) {
  if (!name || !out)
    return false;
  const char *os = ny_host_os_name();
  const char *arch = ny_host_arch_name();
  bool is_windows = strcmp(os, "windows") == 0;
  bool is_macos = strcmp(os, "macos") == 0;
  bool is_linux = strcmp(os, "linux") == 0;
  bool is_unix = !is_windows && strcmp(os, "unknown") != 0;
  bool is_x86_64 = strcmp(arch, "x86_64") == 0;
  bool is_x86 = strcmp(arch, "x86") == 0 || is_x86_64;
  bool is_aarch64 = strcmp(arch, "aarch64") == 0 || strcmp(arch, "arm64") == 0;
  bool is_arm = strcmp(arch, "arm") == 0 || is_aarch64;
  bool is_riscv = strcmp(arch, "riscv") == 0;

  if (strcmp(name, "OS") == 0) {
    out->kind = NY_CT_FAST_STR;
    out->s = os;
    return true;
  }
  if (strcmp(name, "ARCH") == 0) {
    out->kind = NY_CT_FAST_STR;
    out->s = arch;
    return true;
  }

#define NY_CT_PLATFORM_BOOL(symbol, value)                                     \
  if (strcmp(name, symbol) == 0) {                                             \
    out->kind = NY_CT_FAST_BOOL;                                               \
    out->b = (value);                                                          \
    return true;                                                               \
  }

  NY_CT_PLATFORM_BOOL("linux", is_linux)
  NY_CT_PLATFORM_BOOL("LINUX", is_linux)
  NY_CT_PLATFORM_BOOL("IS_LINUX", is_linux)
  NY_CT_PLATFORM_BOOL("macos", is_macos)
  NY_CT_PLATFORM_BOOL("mac", is_macos)
  NY_CT_PLATFORM_BOOL("MACOS", is_macos)
  NY_CT_PLATFORM_BOOL("IS_MACOS", is_macos)
  NY_CT_PLATFORM_BOOL("windows", is_windows)
  NY_CT_PLATFORM_BOOL("IS_WINDOWS", is_windows)
  NY_CT_PLATFORM_BOOL("unix", is_unix)
  NY_CT_PLATFORM_BOOL("posix", is_unix)
  NY_CT_PLATFORM_BOOL("UNIX", is_unix)
  NY_CT_PLATFORM_BOOL("IS_UNIX", is_unix)
  NY_CT_PLATFORM_BOOL("x86_64", is_x86_64)
  NY_CT_PLATFORM_BOOL("x64", is_x86_64)
  NY_CT_PLATFORM_BOOL("IS_X86_64", is_x86_64)
  NY_CT_PLATFORM_BOOL("x86", is_x86)
  NY_CT_PLATFORM_BOOL("IS_X86", is_x86)
  NY_CT_PLATFORM_BOOL("aarch64", is_aarch64)
  NY_CT_PLATFORM_BOOL("arm64", is_aarch64)
  NY_CT_PLATFORM_BOOL("IS_AARCH64", is_aarch64)
  NY_CT_PLATFORM_BOOL("arm", is_arm)
  NY_CT_PLATFORM_BOOL("IS_ARM", is_arm)
  NY_CT_PLATFORM_BOOL("riscv", is_riscv)
  NY_CT_PLATFORM_BOOL("IS_RISCV", is_riscv)

#undef NY_CT_PLATFORM_BOOL
  return false;
}

static bool ny_ct_fast_eval_unary(const char *op, const ny_ct_fast_val_t *r,
                                  ny_ct_fast_val_t *out) {
  if (!op || !r || !out)
    return false;
  switch (op[0]) {
  case '!':
    if (!op[1]) {
      bool t = false;
      if (!ny_ct_fast_truthy(r, &t))
        return false;
      out->kind = NY_CT_FAST_BOOL;
      out->b = !t;
      return true;
    }
    break;
  case '-':
    if (!op[1] && r->kind == NY_CT_FAST_INT) {
      if (r->i == INT64_MIN)
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = -r->i;
      return true;
    }
    break;
  case '~':
    if (!op[1] && r->kind == NY_CT_FAST_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = ~r->i;
      return true;
    }
    break;
  }
  return false;
}

#define NY_CT_OP_INT(sym, oper)                                                \
  if (strcmp(op, sym) == 0) {                                                  \
    out->kind = NY_CT_FAST_INT;                                                \
    out->i = l oper r;                                                         \
    return true;                                                               \
  }

#define NY_CT_OP_INT_CHECK_ZERO(sym, oper)                                     \
  if (strcmp(op, sym) == 0) {                                                  \
    if (r == 0)                                                                \
      return false;                                                            \
    out->kind = NY_CT_FAST_INT;                                                \
    out->i = l oper r;                                                         \
    return true;                                                               \
  }

#define NY_CT_OP_BOOL(sym, oper)                                               \
  if (strcmp(op, sym) == 0) {                                                  \
    out->kind = NY_CT_FAST_BOOL;                                               \
    out->b = l oper r;                                                         \
    return true;                                                               \
  }

static bool ny_ct_fast_int_op(const char *op, int64_t l, int64_t r,
                              ny_ct_fast_val_t *out) {
  switch (op[0]) {
  case '+':
    if (__builtin_add_overflow(l, r, &out->i) || !ny_small_int_fits_i64(out->i))
      return false;
    out->kind = NY_CT_FAST_INT;
    return true;
  case '-':
    if (__builtin_sub_overflow(l, r, &out->i) || !ny_small_int_fits_i64(out->i))
      return false;
    out->kind = NY_CT_FAST_INT;
    return true;
  case '*':
    if (__builtin_mul_overflow(l, r, &out->i) || !ny_small_int_fits_i64(out->i))
      return false;
    out->kind = NY_CT_FAST_INT;
    return true;
  case '/':
    if (r == 0)
      return false;
    out->kind = NY_CT_FAST_INT;
    out->i = l / r;
    return true;
  case '%':
    if (r == 0)
      return false;
    out->kind = NY_CT_FAST_INT;
    out->i = l % r;
    return true;
  case '<':
    if (op[1] == '=') {
      out->kind = NY_CT_FAST_BOOL;
      out->b = l <= r;
      return true;
    }
    if (op[1] == '<') {
      if (r < 0 || r >= 63)
        return false;
      if (!ny_small_int_fits_i64((int64_t)((uint64_t)l << r)))
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = l << r;
      return true;
    }
    out->kind = NY_CT_FAST_BOOL;
    out->b = l < r;
    return true;
  case '>':
    if (op[1] == '=') {
      out->kind = NY_CT_FAST_BOOL;
      out->b = l >= r;
      return true;
    }
    if (op[1] == '>') {
      if (r < 0 || r >= 63)
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = l >> r;
      return true;
    }
    out->kind = NY_CT_FAST_BOOL;
    out->b = l > r;
    return true;
  case '=':
    if (op[1] == '=') {
      out->kind = NY_CT_FAST_BOOL;
      out->b = l == r;
      return true;
    }
    break;
  case '!':
    if (op[1] == '=') {
      out->kind = NY_CT_FAST_BOOL;
      out->b = l != r;
      return true;
    }
    break;
  case '|':
    out->kind = NY_CT_FAST_INT;
    out->i = l | r;
    return true;
  case '&':
    out->kind = NY_CT_FAST_INT;
    out->i = l & r;
    return true;
  case '^':
    if (op[1] == '^') {
      out->kind = NY_CT_FAST_INT;
      out->i = l ^ r;
      return true;
    }
    if (!op[1] && ny_checked_small_pow_i64(l, r, &out->i)) {
      out->kind = NY_CT_FAST_INT;
      return true;
    }
  }
  return false;
}

#undef NY_CT_OP_INT
#undef NY_CT_OP_INT_CHECK_ZERO
#undef NY_CT_OP_BOOL

static bool ny_ct_numeric_eq(const ny_ct_fast_val_t *l,
                             const ny_ct_fast_val_t *r, bool *out);

static bool ny_ct_fast_eval_binary(const char *op, const ny_ct_fast_val_t *l,
                                   const ny_ct_fast_val_t *r,
                                   ny_ct_fast_val_t *out) {
  if (!op || !l || !r || !out)
    return false;
  if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
    bool eq = false;
    if (ny_ct_numeric_eq(l, r, &eq)) {

    } else if (l->kind == NY_CT_FAST_INT && r->kind == NY_CT_FAST_INT) {
      eq = (l->i == r->i);
    } else if (l->kind == NY_CT_FAST_BOOL && r->kind == NY_CT_FAST_BOOL) {
      eq = (l->b == r->b);
    } else if (l->kind == NY_CT_FAST_STR && r->kind == NY_CT_FAST_STR) {
      eq = strcmp(l->s ? l->s : "", r->s ? r->s : "") == 0;
    } else if (l->kind == NY_CT_FAST_NONE && r->kind == NY_CT_FAST_NONE) {
      eq = true;
    } else {
      eq = false;
    }
    out->kind = NY_CT_FAST_BOOL;
    out->b = (strcmp(op, "==") == 0) ? eq : !eq;
    return true;
  }
  if (l->kind == NY_CT_FAST_INT && r->kind == NY_CT_FAST_INT) {
    return ny_ct_fast_int_op(op, l->i, r->i, out);
  }
  return false;
}

static const char *ny_ct_fast_callee_leaf_name(expr_t *callee) {
  expr_t *cur = callee;
  int guard = 0;
  while (cur && guard++ < 16) {
    if (cur->kind == NY_E_IDENT)
      return cur->as.ident.name;
    if (cur->kind == NY_E_MEMBER) {
      if (cur->as.member.name && *cur->as.member.name)
        return cur->as.member.name;
      cur = cur->as.member.target;
      continue;
    }
    break;
  }
  return NULL;
}

static bool ny_ct_fast_push_byte(int64_t *acc, int64_t byte);
static void ny_ct_fast_val_free(ny_ct_fast_val_t *v);
static bool ny_ct_fast_val_clone(const ny_ct_fast_val_t *src,
                                 ny_ct_fast_val_t *dst);

static char *ny_ct_i64_to_dec(int64_t v) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%" PRId64, v);
  return ny_strdup(buf);
}

static bool ny_ct_is_space_char(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

static char *ny_ct_canonical_decimal(const char *raw) {
  if (!raw)
    return ny_strdup("0");
  const char *p = raw;
  while (ny_ct_is_space_char(*p))
    p++;
  bool neg = false;
  if (*p == '+' || *p == '-') {
    neg = *p == '-';
    p++;
  }
  while (*p == '0')
    p++;
  const char *digits = p;
  while (*p >= '0' && *p <= '9')
    p++;
  const char *tail = p;
  while (ny_ct_is_space_char(*tail))
    tail++;
  if (*tail != '\0')
    return NULL;
  size_t len = (size_t)(p - digits);
  if (len == 0)
    return ny_strdup("0");
  size_t out_len = len + (neg ? 1u : 0u);
  char *out = malloc(out_len + 1);
  if (!out)
    return NULL;
  size_t pos = 0;
  if (neg)
    out[pos++] = '-';
  memcpy(out + pos, digits, len);
  out[out_len] = '\0';
  return out;
}

static char *ny_ct_dec_mul_add_byte(const char *dec, int64_t byte) {
  const char *src = (dec && *dec) ? dec : "0";
  size_t len = strlen(src);
  size_t cap = len + 8;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  size_t pos = cap - 1;
  buf[pos] = '\0';
  int carry = (int)((uint64_t)byte & 255ULL);
  for (ssize_t i = (ssize_t)len - 1; i >= 0; --i) {
    char c = src[i];
    if (c < '0' || c > '9') {
      free(buf);
      return NULL;
    }
    int v = (c - '0') * 256 + carry;
    if (pos == 0) {
      free(buf);
      return NULL;
    }
    buf[--pos] = (char)('0' + (v % 10));
    carry = v / 10;
  }
  while (carry > 0) {
    if (pos == 0) {
      free(buf);
      return NULL;
    }
    buf[--pos] = (char)('0' + (carry % 10));
    carry /= 10;
  }
  while (buf[pos] == '0' && buf[pos + 1] != '\0')
    pos++;
  char *out = ny_strdup(buf + pos);
  free(buf);
  return out;
}

static bool ny_ct_long_accum_byte(int64_t *small, bool *small_ok,
                                  char **big_dec, int64_t byte) {
  if (!small || !small_ok || !big_dec)
    return false;
  if (*small_ok) {
    int64_t prev = *small;
    if (ny_ct_fast_push_byte(small, byte))
      return true;
    *small_ok = false;
    *big_dec = ny_ct_i64_to_dec(prev);
    if (!*big_dec)
      return false;
  }
  char *next = ny_ct_dec_mul_add_byte(*big_dec, byte);
  if (!next)
    return false;
  free(*big_dec);
  *big_dec = next;
  return true;
}

static bool ny_ct_long_finish(int64_t small, bool small_ok, char *big_dec,
                              ny_ct_fast_val_t *out) {
  if (!out) {
    free(big_dec);
    return false;
  }
  if (small_ok) {
    free(big_dec);
    out->kind = NY_CT_FAST_INT;
    out->i = small;
    return true;
  }
  out->kind = NY_CT_FAST_BIGINT;
  if (!big_dec) {
    big_dec = ny_strdup("0");
    if (!big_dec)
      return false;
  }
  out->s = big_dec;
  return true;
}

static bool ny_ct_numeric_eq(const ny_ct_fast_val_t *l,
                             const ny_ct_fast_val_t *r, bool *out) {
  if (!l || !r || !out)
    return false;
  bool ln = l->kind == NY_CT_FAST_INT || l->kind == NY_CT_FAST_BIGINT;
  bool rn = r->kind == NY_CT_FAST_INT || r->kind == NY_CT_FAST_BIGINT;
  if (!ln || !rn)
    return false;
  char lbuf[64], rbuf[64];
  const char *ls = l->s;
  const char *rs = r->s;
  if (l->kind == NY_CT_FAST_INT) {
    snprintf(lbuf, sizeof(lbuf), "%" PRId64, l->i);
    ls = lbuf;
  }
  if (r->kind == NY_CT_FAST_INT) {
    snprintf(rbuf, sizeof(rbuf), "%" PRId64, r->i);
    rs = rbuf;
  }
  *out = strcmp(ls ? ls : "0", rs ? rs : "0") == 0;
  return true;
}

static bool ny_ct_range_len_raw(int64_t start, int64_t stop, int64_t step,
                                int64_t *out) {
  if (!out || step == 0)
    return false;
  *out = 0;
  if (step > 0) {
    if (start >= stop)
      return true;
    int64_t span = 0;
    if (__builtin_sub_overflow(stop, start, &span) || span <= 0)
      return false;
    *out = ((span - 1) / step) + 1;
    return true;
  }
  if (start <= stop)
    return true;
  int64_t span = 0;
  if (__builtin_sub_overflow(start, stop, &span) || span <= 0)
    return false;
  *out = ((span - 1) / -step) + 1;
  return true;
}

static bool ny_ct_make_range(int64_t start, int64_t stop, int64_t step,
                             ny_ct_fast_val_t *out) {
  if (!out)
    return false;
  if (step == 0)
    step = 1;
  int64_t len = 0;
  if (!ny_ct_range_len_raw(start, stop, step, &len) || len < 0 ||
      len > 65536)
    return false;
  out->kind = NY_CT_FAST_RANGE;
  out->range_start = start;
  out->range_stop = stop;
  out->range_step = step;
  out->len = (size_t)len;
  out->items = NULL;
  return true;
}

static bool ny_ct_seq_len(const ny_ct_fast_val_t *v, int64_t *out) {
  if (!v || !out)
    return false;
  if (v->kind == NY_CT_FAST_LIST || v->kind == NY_CT_FAST_TUPLE ||
      v->kind == NY_CT_FAST_RANGE) {
    if (v->len > (size_t)INT64_MAX)
      return false;
    *out = (int64_t)v->len;
    return true;
  }
  return false;
}

static bool ny_ct_seq_item(const ny_ct_fast_val_t *v, int64_t idx,
                           ny_ct_fast_val_t *out) {
  if (!v || !out || idx < 0)
    return false;
  int64_t len = 0;
  if (!ny_ct_seq_len(v, &len) || idx >= len)
    return false;
  if (v->kind == NY_CT_FAST_LIST || v->kind == NY_CT_FAST_TUPLE) {
    if (!v->items)
      return false;
    return ny_ct_fast_val_clone(&v->items[idx], out);
  }
  if (v->kind == NY_CT_FAST_RANGE) {
    int64_t scaled = 0, item = 0;
    if (__builtin_mul_overflow(idx, v->range_step, &scaled) ||
        __builtin_add_overflow(v->range_start, scaled, &item) ||
        !ny_small_int_fits_i64(item))
      return false;
    out->kind = NY_CT_FAST_INT;
    out->i = item;
    return true;
  }
  return false;
}

static bool ny_ct_make_seq(ny_ct_fast_kind_t kind, size_t len,
                           ny_ct_fast_val_t *out) {
  if (!out || (kind != NY_CT_FAST_LIST && kind != NY_CT_FAST_TUPLE) ||
      len > 65536)
    return false;
  ny_ct_fast_val_t *items = NULL;
  if (len > 0) {
    items = calloc(len, sizeof(*items));
    if (!items)
      return false;
  }
  out->kind = kind;
  out->len = len;
  out->items = items;
  return true;
}

typedef struct ny_ct_ptr_seen_t {
  void **items;
  size_t len;
  size_t cap;
} ny_ct_ptr_seen_t;

static bool ny_ct_seen_add(ny_ct_ptr_seen_t *seen, const void *ptr) {
  if (!seen || !ptr)
    return false;
  for (size_t i = 0; i < seen->len; i++) {
    if (seen->items[i] == ptr)
      return false;
  }
  if (seen->len == seen->cap) {
    size_t next_cap = seen->cap ? seen->cap * 2 : 16;
    void **grown = realloc(seen->items, next_cap * sizeof(*seen->items));
    if (!grown)
      return false;
    seen->items = grown;
    seen->cap = next_cap;
  }
  seen->items[seen->len++] = (void *)ptr;
  return true;
}

static void ny_ct_fast_val_free_seen(ny_ct_fast_val_t *v,
                                     ny_ct_ptr_seen_t *seen) {
  if (!v)
    return;
  if (v->kind == NY_CT_FAST_BIGINT && v->s && ny_ct_seen_add(seen, v->s))
    free((void *)v->s);
  if ((v->kind == NY_CT_FAST_LIST || v->kind == NY_CT_FAST_TUPLE) &&
      v->items) {
    ny_ct_fast_val_t *items = v->items;
    size_t len = v->len;
    if (ny_ct_seen_add(seen, items)) {
      for (size_t i = 0; i < len; i++)
        ny_ct_fast_val_free_seen(&items[i], seen);
      free(items);
    }
  }
  *v = ny_ct_fast_none();
}

static void ny_ct_fast_val_free(ny_ct_fast_val_t *v) {
  ny_ct_ptr_seen_t seen = {0};
  ny_ct_fast_val_free_seen(v, &seen);
  free(seen.items);
}

static void ny_ct_fast_val_move(ny_ct_fast_val_t *dst,
                                ny_ct_fast_val_t *src) {
  if (!dst || !src || dst == src)
    return;
  ny_ct_fast_val_free(dst);
  *dst = *src;
  *src = ny_ct_fast_none();
}

static bool ny_ct_fast_val_clone(const ny_ct_fast_val_t *src,
                                 ny_ct_fast_val_t *dst) {
  if (!src || !dst)
    return false;
  ny_ct_fast_val_t out = *src;
  out.s = src->s;
  out.items = NULL;
  if (src->kind == NY_CT_FAST_BIGINT) {
    out.s = ny_strdup(src->s ? src->s : "0");
    if (!out.s)
      return false;
  } else if (src->kind == NY_CT_FAST_LIST ||
             src->kind == NY_CT_FAST_TUPLE) {
    if (src->len > 0 && !src->items)
      return false;
    if (!ny_ct_make_seq(src->kind, src->len, &out))
      return false;
    for (size_t i = 0; i < src->len; i++) {
      if (!ny_ct_fast_val_clone(&src->items[i], &out.items[i])) {
        ny_ct_fast_val_free(&out);
        return false;
      }
    }
  }
  *dst = out;
  return true;
}

static bool ny_try_eval_comptime_expr_fast(codegen_t *cg, expr_t *e,
                                           ny_ct_fast_val_t *out, int depth);
static bool ny_try_eval_comptime_fast_value(codegen_t *cg, stmt_t *body,
                                            ny_ct_fast_val_t *out);

static expr_t *ny_binding_var_init_expr_any(binding *b, const char *name) {
  if (!b || !name || !b->stmt_t || b->stmt_t->kind != NY_S_VAR)
    return NULL;
  stmt_var_t *var = &b->stmt_t->as.var;
  for (size_t i = 0; i < var->names.len && i < var->exprs.len; ++i) {
    const char *n = var->names.data[i];
    if (n && strcmp(n, name) == 0)
      return var->exprs.data[i];
  }
  return NULL;
}

static bool ny_try_eval_binding_comptime_const(codegen_t *cg, binding *b,
                                               const char *name,
                                               ny_ct_fast_val_t *out,
                                               int depth) {
  if (!b || b->is_mut || !name || !out || depth > 64)
    return false;

  expr_t *init = ny_binding_var_init_expr(b, name);
  if (init)
    return ny_try_eval_comptime_expr_fast(cg, init, out, depth + 1);

  init = ny_binding_var_init_expr_any(b, name);
  if (init && init->kind == NY_E_COMPTIME && init->as.comptime_expr.body)
    return ny_try_eval_comptime_fast_value(cg, init->as.comptime_expr.body,
                                           out);
  return false;
}

static bool ny_ct_fast_eval_bigint_constructor_arg(const ny_ct_fast_val_t *arg,
                                                   ny_ct_fast_val_t *out) {
  if (!arg || !out)
    return false;
  char *dec = NULL;
  if (arg->kind == NY_CT_FAST_INT) {
    dec = ny_ct_i64_to_dec(arg->i);
  } else if (arg->kind == NY_CT_FAST_STR || arg->kind == NY_CT_FAST_BIGINT) {
    dec = ny_ct_canonical_decimal(arg->s ? arg->s : "0");
  } else {
    return false;
  }
  if (!dec)
    return false;
  out->kind = NY_CT_FAST_BIGINT;
  out->s = dec;
  return true;
}

static bool ny_ct_fast_push_byte(int64_t *acc, int64_t byte) {
  if (!acc)
    return false;
  int64_t b = (int64_t)((uint64_t)byte & 255ULL);
  if (*acc > (INT64_MAX - b) / 256)
    return false;
  int64_t next = (*acc * 256) + b;
  if (!ny_small_int_fits_i64(next))
    return false;
  *acc = next;
  return true;
}

static bool ny_ct_fast_bytes_long(const char *data, size_t len,
                                  ny_ct_fast_val_t *out) {
  if (!out)
    return false;
  int64_t small = 0;
  bool small_ok = true;
  char *big_dec = NULL;
  for (size_t i = 0; i < len; i++) {
    unsigned char byte = data ? (unsigned char)data[i] : 0;
    if (!ny_ct_long_accum_byte(&small, &small_ok, &big_dec, (int64_t)byte)) {
      free(big_dec);
      return false;
    }
  }
  return ny_ct_long_finish(small, small_ok, big_dec, out);
}

static int ny_ct_fast_hex_nibble(unsigned char c) {
  if (c >= '0' && c <= '9')
    return (int)(c - '0');
  if (c >= 'A' && c <= 'F')
    return (int)(c - 'A' + 10);
  if (c >= 'a' && c <= 'f')
    return (int)(c - 'a' + 10);
  return 0;
}

static bool ny_ct_fast_unhex_long(const char *data, size_t len,
                                  ny_ct_fast_val_t *out) {
  if (!out)
    return false;
  int64_t small = 0;
  bool small_ok = true;
  char *big_dec = NULL;
  size_t i = 0;
  if ((len & 1u) != 0) {
    int lo = ny_ct_fast_hex_nibble(data ? (unsigned char)data[0] : 0);
    if (!ny_ct_long_accum_byte(&small, &small_ok, &big_dec, lo)) {
      free(big_dec);
      return false;
    }
    i = 1;
  }
  while (i < len) {
    int hi = ny_ct_fast_hex_nibble(data ? (unsigned char)data[i] : 0);
    int lo = ny_ct_fast_hex_nibble(data ? (unsigned char)data[i + 1] : 0);
    if (!ny_ct_long_accum_byte(&small, &small_ok, &big_dec,
                               (hi << 4) | lo)) {
      free(big_dec);
      return false;
    }
    i += 2;
  }
  return ny_ct_long_finish(small, small_ok, big_dec, out);
}

static bool ny_ct_fast_literal_str_long(expr_t *e, ny_ct_fast_val_t *out) {
  if (!e || e->kind != NY_E_LITERAL || e->as.literal.kind != NY_LIT_STR)
    return false;
  return ny_ct_fast_bytes_long(e->as.literal.as.s.data, e->as.literal.as.s.len,
                               out);
}

static bool ny_ct_fast_literal_unhex_long(expr_t *e, ny_ct_fast_val_t *out) {
  if (!e || e->kind != NY_E_LITERAL || e->as.literal.kind != NY_LIT_STR)
    return false;
  return ny_ct_fast_unhex_long(e->as.literal.as.s.data, e->as.literal.as.s.len,
                               out);
}

static bool ny_ct_fast_list_long(codegen_t *cg, expr_t *e,
                                 ny_ct_fast_val_t *out, int depth) {
  if (!e || !out || (e->kind != NY_E_LIST && e->kind != NY_E_TUPLE))
    return false;
  int64_t small = 0;
  bool small_ok = true;
  char *big_dec = NULL;
  for (size_t i = 0; i < e->as.list_like.len; i++) {
    ny_ct_fast_val_t item = {0};
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.list_like.data[i], &item,
                                        depth + 1) ||
        item.kind != NY_CT_FAST_INT) {
      free(big_dec);
      ny_ct_fast_val_free(&item);
      return false;
    }
    if (!ny_ct_long_accum_byte(&small, &small_ok, &big_dec, item.i)) {
      free(big_dec);
      ny_ct_fast_val_free(&item);
      return false;
    }
    ny_ct_fast_val_free(&item);
  }
  return ny_ct_long_finish(small, small_ok, big_dec, out);
}

static bool ny_ct_fast_eval_long_property(codegen_t *cg, expr_t *target,
                                          ny_ct_fast_val_t *out, int depth) {
  if (!target || !out || depth > 64)
    return false;
  if (target->kind == NY_E_LITERAL) {
    if (target->as.literal.kind == NY_LIT_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = target->as.literal.as.i;
      return true;
    }
    if (target->as.literal.kind == NY_LIT_STR)
      return ny_ct_fast_literal_str_long(target, out);
  }
  if (target->kind == NY_E_LIST || target->kind == NY_E_TUPLE)
    return ny_ct_fast_list_long(cg, target, out, depth + 1);
  if (target->kind == NY_E_MEMBER && target->as.member.name) {
    if (strcmp(target->as.member.name, "unhex") == 0)
      return ny_ct_fast_literal_unhex_long(target->as.member.target, out);
    if (strcmp(target->as.member.name, "to_bytes") == 0)
      return ny_ct_fast_eval_long_property(cg, target->as.member.target, out,
                                           depth + 1);
  }
  ny_ct_fast_val_t v = {0};
  if (!ny_try_eval_comptime_expr_fast(cg, target, &v, depth + 1))
    return false;
  if (v.kind == NY_CT_FAST_INT) {
    ny_ct_fast_val_move(out, &v);
    return true;
  }
  if (v.kind == NY_CT_FAST_BIGINT) {
    ny_ct_fast_val_move(out, &v);
    return true;
  }
  if (v.kind == NY_CT_FAST_STR) {
    bool ok = ny_ct_fast_bytes_long(v.s, v.s ? strlen(v.s) : 0, out);
    ny_ct_fast_val_free(&v);
    return ok;
  }
  ny_ct_fast_val_free(&v);
  return false;
}

static bool ny_try_eval_comptime_expr_fast(codegen_t *cg, expr_t *e,
                                           ny_ct_fast_val_t *out, int depth) {
  if (!e || !out || depth > 64)
    return false;

  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = e->as.literal.as.i;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_BOOL) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = e->as.literal.as.b;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_STR) {
      out->kind = NY_CT_FAST_STR;
      out->s = e->as.literal.as.s.data ? e->as.literal.as.s.data : "";
      return true;
    }
    return false;

  case NY_E_IDENT:
    if (ny_ct_platform_ident(e->as.ident.name, out))
      return true;
    if (cg) {
      binding *b = lookup_global(cg, e->as.ident.name);
      if (ny_try_eval_binding_comptime_const(cg, b, e->as.ident.name, out,
                                             depth + 1))
        return true;
    }
    return false;

  case NY_E_UNARY: {
    ny_ct_fast_val_t r = {0};
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.unary.right, &r, depth + 1))
      return false;
    bool ok = ny_ct_fast_eval_unary(e->as.unary.op, &r, out);
    ny_ct_fast_val_free(&r);
    return ok;
  }

  case NY_E_LOGICAL: {
    ny_ct_fast_val_t l = {0};
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.logical.left, &l, depth + 1))
      return false;
    bool lt = false;
    if (!ny_ct_fast_truthy(&l, &lt)) {
      ny_ct_fast_val_free(&l);
      return false;
    }
    if (strcmp(e->as.logical.op, "&&") == 0) {
      ny_ct_fast_val_free(&l);
      if (!lt) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = false;
        return true;
      }
      ny_ct_fast_val_t r = {0};
      bool rt = false;
      if (!ny_try_eval_comptime_expr_fast(cg, e->as.logical.right, &r, depth + 1) ||
          !ny_ct_fast_truthy(&r, &rt)) {
        ny_ct_fast_val_free(&r);
        return false;
      }
      out->kind = NY_CT_FAST_BOOL;
      out->b = rt;
      ny_ct_fast_val_free(&r);
      return true;
    }
    if (strcmp(e->as.logical.op, "||") == 0) {
      ny_ct_fast_val_free(&l);
      if (lt) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = true;
        return true;
      }
      ny_ct_fast_val_t r = {0};
      bool rt = false;
      if (!ny_try_eval_comptime_expr_fast(cg, e->as.logical.right, &r, depth + 1) ||
          !ny_ct_fast_truthy(&r, &rt)) {
        ny_ct_fast_val_free(&r);
        return false;
      }
      out->kind = NY_CT_FAST_BOOL;
      out->b = rt;
      ny_ct_fast_val_free(&r);
      return true;
    }
    ny_ct_fast_val_free(&l);
    return false;
  }

  case NY_E_BINARY: {
    ny_ct_fast_val_t l = {0}, r = {0};
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.binary.left, &l, depth + 1))
      return false;
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.binary.right, &r, depth + 1)) {
      ny_ct_fast_val_free(&l);
      return false;
    }
    bool ok = ny_ct_fast_eval_binary(e->as.binary.op, &l, &r, out);
    ny_ct_fast_val_free(&l);
    ny_ct_fast_val_free(&r);
    return ok;
  }

  case NY_E_TERNARY: {
    ny_ct_fast_val_t c = {0};
    bool ct = false;
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.ternary.cond, &c, depth + 1) ||
        !ny_ct_fast_truthy(&c, &ct)) {
      ny_ct_fast_val_free(&c);
      return false;
    }
    ny_ct_fast_val_free(&c);
    return ny_try_eval_comptime_expr_fast(cg, ct ? e->as.ternary.true_expr
                                             : e->as.ternary.false_expr,
                                          out, depth + 1);
  }

  case NY_E_MEMBER:
    if (e->as.member.name && strcmp(e->as.member.name, "long") == 0)
      return ny_ct_fast_eval_long_property(cg, e->as.member.target, out,
                                           depth + 1);
    return false;

  case NY_E_COMPTIME:
    return ny_try_eval_comptime_fast_value(cg, e->as.comptime_expr.body, out);

  case NY_E_CALL: {
    if (!e->as.call.callee)
      return false;
    const char *name = ny_ct_fast_callee_leaf_name(e->as.call.callee);
    if (!name)
      return false;
    bool zero_arg = (e->as.call.args.len == 0);
    bool one_member_arg =
        (e->as.call.args.len == 1 && e->as.call.callee->kind == NY_E_MEMBER);
    if (strcmp(name, "Z") == 0 && e->as.call.args.len == 1) {
      ny_ct_fast_val_t arg = {0};
      if (!ny_try_eval_comptime_expr_fast(cg, e->as.call.args.data[0].val, &arg,
                                          depth + 1))
        return false;
      bool ok = ny_ct_fast_eval_bigint_constructor_arg(&arg, out);
      ny_ct_fast_val_free(&arg);
      return ok;
    }
    if (strcmp(name, "__main") == 0 && (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = ny_comptime_main_enabled(cg, e->tok);
      return true;
    }
    if ((strcmp(name, "__os_name") == 0 || strcmp(name, "os") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_host_os_name();
      return true;
    }
    if ((strcmp(name, "__arch_name") == 0 || strcmp(name, "arch") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_host_arch_name();
      return true;
    }
    return false;
  }

  default:
    return false;
  }
}

static bool ny_ct_fast_to_tagged(const ny_ct_fast_val_t *v,
                                 int64_t *out_tagged) {
  if (!v || !out_tagged)
    return false;
  if (v->kind == NY_CT_FAST_BOOL) {
    *out_tagged = v->b ? NY_IMM_TRUE : NY_IMM_FALSE;
    return true;
  }
  if (v->kind == NY_CT_FAST_INT) {
    if (!ny_small_int_fits_i64(v->i))
      return false;
    *out_tagged = (int64_t)((((uint64_t)v->i) << 1) | 1u);
    return true;
  }
  if (v->kind == NY_CT_FAST_NONE) {
    *out_tagged = NY_IMM_NIL;
    return true;
  }
  return false;
}

static bool ny_try_eval_comptime_fast_value(codegen_t *cg, stmt_t *body,
                                            ny_ct_fast_val_t *out) {
  if (!body || !out)
    return false;

  if (body->kind == NY_S_BLOCK) {
    ny_ct_fast_val_t res = ny_ct_fast_none();
    for (size_t i = 0; i < body->as.block.body.len; ++i) {
      stmt_t *s = body->as.block.body.data[i];
      if (!s)
        continue;
      if (s->kind == NY_S_FUNC || s->kind == NY_S_EXTERN ||
          s->kind == NY_S_LINK || s->kind == NY_S_MODULE ||
          s->kind == NY_S_STRUCT || s->kind == NY_S_LAYOUT ||
          s->kind == NY_S_ENUM || s->kind == NY_S_MACRO ||
          s->kind == NY_S_USE || s->kind == NY_S_EXPORT ||
          s->kind == NY_S_OPERATOR) {

        continue;
      }
      ny_ct_fast_val_t next = ny_ct_fast_none();
      if (!ny_try_eval_comptime_fast_value(cg, s, &next)) {
        ny_ct_fast_val_free(&res);
        return false;
      }
      ny_ct_fast_val_move(&res, &next);
    }
    ny_ct_fast_val_move(out, &res);
    return true;
  }

  if (body->kind == NY_S_IF) {
    ny_ct_fast_val_t cond_v = {0};
    bool truthy = false;
    if (ny_try_eval_comptime_expr_fast(cg, body->as.iff.test, &cond_v, 0) &&
        ny_ct_fast_truthy(&cond_v, &truthy)) {
      ny_ct_fast_val_free(&cond_v);
      if (truthy) {
        if (body->as.iff.conseq)
          return ny_try_eval_comptime_fast_value(cg, body->as.iff.conseq, out);
        ny_ct_fast_val_free(out);
        *out = ny_ct_fast_none();
        return true;
      } else {
        if (body->as.iff.alt)
          return ny_try_eval_comptime_fast_value(cg, body->as.iff.alt, out);
        ny_ct_fast_val_free(out);
        *out = ny_ct_fast_none();
        return true;
      }
    }
    ny_ct_fast_val_free(&cond_v);
    return false;
  }

  expr_t *e = NULL;
  if (body->kind == NY_S_RETURN) {
    e = body->as.ret.value;
  } else if (body->kind == NY_S_EXPR) {
    e = body->as.expr.expr;
  } else {
    return false;
  }

  if (!e) {
    ny_ct_fast_val_free(out);
    *out = ny_ct_fast_none();
    return true;
  }

  return ny_try_eval_comptime_expr_fast(cg, e, out, 0);
}

typedef struct ny_ct_interp_var_t {
  const char *name;
  ny_ct_fast_val_t value;
} ny_ct_interp_var_t;

typedef struct ny_ct_interp_ctx_t {
  codegen_t *cg;
  ny_ct_interp_var_t *vars;
  size_t len;
  size_t cap;
  size_t steps;
  size_t max_steps;
} ny_ct_interp_ctx_t;

static bool ny_ct_interp_step(ny_ct_interp_ctx_t *ctx) {
  if (!ctx)
    return false;
  if (++ctx->steps > ctx->max_steps)
    return false;
  return true;
}

static void ny_ct_interp_ctx_free(ny_ct_interp_ctx_t *ctx) {
  if (!ctx)
    return;
  for (size_t i = 0; i < ctx->len; i++)
    ny_ct_fast_val_free(&ctx->vars[i].value);
  free(ctx->vars);
  ctx->vars = NULL;
  ctx->len = 0;
  ctx->cap = 0;
}

static bool ny_ct_interp_ctx_clone(const ny_ct_interp_ctx_t *src,
                                   ny_ct_interp_ctx_t *dst) {
  if (!src || !dst)
    return false;
  memset(dst, 0, sizeof(*dst));
  dst->cg = src->cg;
  dst->max_steps = src->max_steps;
  dst->steps = src->steps;
  if (src->len == 0)
    return true;
  dst->vars = calloc(src->len, sizeof(*dst->vars));
  if (!dst->vars)
    return false;
  dst->cap = src->len;
  for (size_t i = 0; i < src->len; i++) {
    dst->vars[i].name = src->vars[i].name;
    if (!ny_ct_fast_val_clone(&src->vars[i].value, &dst->vars[i].value)) {
      dst->len = i;
      ny_ct_interp_ctx_free(dst);
      return false;
    }
    dst->len++;
  }
  return true;
}

static bool ny_ct_interp_get(ny_ct_interp_ctx_t *ctx, const char *name,
                             ny_ct_fast_val_t *out) {
  if (!ctx || !name || !*name || !out)
    return false;
  for (size_t i = ctx->len; i > 0; --i) {
    ny_ct_interp_var_t *v = &ctx->vars[i - 1];
    if (v->name && strcmp(v->name, name) == 0) {
      return ny_ct_fast_val_clone(&v->value, out);
    }
  }
  return false;
}

static bool ny_ct_interp_set(ny_ct_interp_ctx_t *ctx, const char *name,
                             ny_ct_fast_val_t value) {
  if (!ctx || !name || !*name)
    return false;
  ny_ct_fast_val_t copy = ny_ct_fast_none();
  if (!ny_ct_fast_val_clone(&value, &copy))
    return false;
  for (size_t i = ctx->len; i > 0; --i) {
    ny_ct_interp_var_t *v = &ctx->vars[i - 1];
    if (v->name && strcmp(v->name, name) == 0) {
      ny_ct_fast_val_free(&v->value);
      v->value = copy;
      return true;
    }
  }
  if (ctx->len == ctx->cap) {
    size_t next_cap = ctx->cap ? (ctx->cap * 2) : 16;
    ny_ct_interp_var_t *grown =
        realloc(ctx->vars, sizeof(*ctx->vars) * next_cap);
    if (!grown) {
      ny_ct_fast_val_free(&copy);
      return false;
    }
    ctx->vars = grown;
    ctx->cap = next_cap;
  }
  ctx->vars[ctx->len++] = (ny_ct_interp_var_t){.name = name, .value = copy};
  return true;
}

static bool ny_ct_interp_eval_expr(expr_t *e, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *out, int depth);
static bool ny_ct_interp_eval_stmt(stmt_t *s, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *ret, bool *did_return,
                                   int depth);

static bool ny_ct_interp_eval_long_property(expr_t *target,
                                            ny_ct_interp_ctx_t *ctx,
                                            ny_ct_fast_val_t *out,
                                            int depth) {
  if (!target || !ctx || !out || depth > 256)
    return false;
  if (ny_ct_fast_eval_long_property(ctx->cg, target, out, 0))
    return true;
  if (target->kind == NY_E_MEMBER && target->as.member.name) {
    if (strcmp(target->as.member.name, "unhex") == 0) {
      ny_ct_fast_val_t s = ny_ct_fast_none();
      if (!ny_ct_interp_eval_expr(target->as.member.target, ctx, &s,
                                  depth + 1) ||
          s.kind != NY_CT_FAST_STR) {
        ny_ct_fast_val_free(&s);
        return false;
      }
      bool ok = ny_ct_fast_unhex_long(s.s, s.s ? strlen(s.s) : 0, out);
      ny_ct_fast_val_free(&s);
      return ok;
    }
    if (strcmp(target->as.member.name, "to_bytes") == 0)
      return ny_ct_interp_eval_long_property(target->as.member.target, ctx, out,
                                             depth + 1);
  }
  ny_ct_fast_val_t v = ny_ct_fast_none();
  if (!ny_ct_interp_eval_expr(target, ctx, &v, depth + 1))
    return false;
  if (v.kind == NY_CT_FAST_INT) {
    ny_ct_fast_val_move(out, &v);
    return true;
  }
  if (v.kind == NY_CT_FAST_BIGINT) {
    ny_ct_fast_val_move(out, &v);
    return true;
  }
  if (v.kind == NY_CT_FAST_STR) {
    bool ok = ny_ct_fast_bytes_long(v.s, v.s ? strlen(v.s) : 0, out);
    ny_ct_fast_val_free(&v);
    return ok;
  }
  ny_ct_fast_val_free(&v);
  return false;
}

static bool ny_ct_interp_eval_lambda1(expr_t *fn, ny_ct_interp_ctx_t *ctx,
                                      ny_ct_fast_val_t arg,
                                      ny_ct_fast_val_t *out, int depth) {
  if (!fn || !ctx || !out ||
      (fn->kind != NY_E_LAMBDA && fn->kind != NY_E_FN) ||
      fn->as.lambda.params.len != 1 || !fn->as.lambda.body)
    return false;
  const char *param = fn->as.lambda.params.data[0].name;
  if (!param || !*param)
    return false;
  ny_ct_interp_ctx_t nested = {0};
  if (!ny_ct_interp_ctx_clone(ctx, &nested))
    return false;
  bool ok = ny_ct_interp_set(&nested, param, arg);
  ny_ct_fast_val_t ret = ny_ct_fast_none();
  bool did_return = false;
  if (ok)
    ok = ny_ct_interp_eval_stmt(fn->as.lambda.body, &nested, &ret,
                                &did_return, depth + 1);
  ctx->steps = nested.steps;
  ny_ct_interp_ctx_free(&nested);
  if (!ok) {
    ny_ct_fast_val_free(&ret);
    return false;
  }
  ny_ct_fast_val_move(out, &ret);
  return true;
}

static bool ny_ct_interp_eval_map(expr_t *target, expr_t *fn,
                                  ny_ct_interp_ctx_t *ctx,
                                  ny_ct_fast_val_t *out, int depth) {
  if (!target || !fn || !ctx || !out)
    return false;
  ny_ct_fast_val_t seq = ny_ct_fast_none();
  int64_t len = 0;
  if (!ny_ct_interp_eval_expr(target, ctx, &seq, depth + 1) ||
      !ny_ct_seq_len(&seq, &len) || len < 0 || len > 65536) {
    ny_ct_fast_val_free(&seq);
    return false;
  }

  ny_ct_fast_kind_t out_kind =
      seq.kind == NY_CT_FAST_TUPLE ? NY_CT_FAST_TUPLE : NY_CT_FAST_LIST;
  if (!ny_ct_make_seq(out_kind, (size_t)len, out)) {
    ny_ct_fast_val_free(&seq);
    return false;
  }
  for (int64_t i = 0; i < len; i++) {
    ny_ct_fast_val_t item = ny_ct_fast_none();
    ny_ct_fast_val_t mapped = ny_ct_fast_none();
    if (!ny_ct_seq_item(&seq, i, &item) ||
        !ny_ct_interp_eval_lambda1(fn, ctx, item, &mapped, depth + 1)) {
      ny_ct_fast_val_free(&item);
      ny_ct_fast_val_free(&mapped);
      ny_ct_fast_val_free(out);
      ny_ct_fast_val_free(&seq);
      return false;
    }
    ny_ct_fast_val_free(&item);
    ny_ct_fast_val_move(&out->items[i], &mapped);
  }
  ny_ct_fast_val_free(&seq);
  return true;
}

static bool ny_ct_interp_eval_stmt(stmt_t *s, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *ret, bool *did_return,
                                   int depth) {
  if (!s || !ctx || !ret || !did_return)
    return false;
  if (depth > 256 || !ny_ct_interp_step(ctx))
    return false;
  switch (s->kind) {
  case NY_S_BLOCK: {
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (!ny_ct_interp_eval_stmt(s->as.block.body.data[i], ctx, ret,
                                  did_return, depth + 1))
        return false;
      if (*did_return)
        return true;
    }
    return true;
  }
  case NY_S_RETURN: {
    if (!s->as.ret.value) {
      ny_ct_fast_val_free(ret);
      *ret = ny_ct_fast_none();
    } else {
      ny_ct_fast_val_t tmp = ny_ct_fast_none();
      if (!ny_ct_interp_eval_expr(s->as.ret.value, ctx, &tmp, depth + 1))
        return false;
      ny_ct_fast_val_move(ret, &tmp);
    }
    *did_return = true;
    return true;
  }
  case NY_S_EXPR: {
    ny_ct_fast_val_t tmp = ny_ct_fast_none();
    if (!s->as.expr.expr)
      return false;
    if (!ny_ct_interp_eval_expr(s->as.expr.expr, ctx, &tmp, depth + 1))
      return false;
    ny_ct_fast_val_move(ret, &tmp);
    return true;
  }
  case NY_S_VAR: {
    if (s->as.var.is_destructure)
      return false;
    for (size_t i = 0; i < s->as.var.names.len; i++) {
      const char *name = s->as.var.names.data[i];
      if (!name || !*name)
        return false;
      ny_ct_fast_val_t v = ny_ct_fast_none();
      if (!s->as.var.is_del) {
        expr_t *rhs = NULL;
        if (s->as.var.exprs.len == s->as.var.names.len &&
            i < s->as.var.exprs.len)
          rhs = s->as.var.exprs.data[i];
        else if (s->as.var.exprs.len > 0)
          rhs = s->as.var.exprs.data[0];
        if (rhs && !ny_ct_interp_eval_expr(rhs, ctx, &v, depth + 1)) {
          ny_ct_fast_val_free(&v);
          return false;
        }
      }
      if (!ny_ct_interp_set(ctx, name, v)) {
        ny_ct_fast_val_free(&v);
        return false;
      }
      ny_ct_fast_val_free(&v);
    }
    return true;
  }
  case NY_S_IF: {
    if (!s->as.iff.test)
      return false;
    ny_ct_fast_val_t cond = ny_ct_fast_none();
    bool truthy = false;
    if (!ny_ct_interp_eval_expr(s->as.iff.test, ctx, &cond, depth + 1) ||
        !ny_ct_fast_truthy(&cond, &truthy)) {
      ny_ct_fast_val_free(&cond);
      return false;
    }
    ny_ct_fast_val_free(&cond);
    if (truthy) {
      if (s->as.iff.conseq)
        return ny_ct_interp_eval_stmt(s->as.iff.conseq, ctx, ret, did_return,
                                      depth + 1);
      return true;
    }
    if (s->as.iff.alt)
      return ny_ct_interp_eval_stmt(s->as.iff.alt, ctx, ret, did_return,
                                    depth + 1);
    return true;
  }
  case NY_S_WHILE: {
    if (!s->as.whl.test || !s->as.whl.body)
      return false;
    if (s->as.whl.init) {
      if (!ny_ct_interp_eval_stmt(s->as.whl.init, ctx, ret, did_return,
                                  depth + 1))
        return false;
      if (*did_return)
        return true;
    }
    size_t guard = 0;
    while (1) {
      ny_ct_fast_val_t cond = ny_ct_fast_none();
      bool truthy = false;
      if (!ny_ct_interp_eval_expr(s->as.whl.test, ctx, &cond, depth + 1) ||
          !ny_ct_fast_truthy(&cond, &truthy)) {
        ny_ct_fast_val_free(&cond);
        return false;
      }
      ny_ct_fast_val_free(&cond);
      if (!truthy)
        break;
      if (++guard > 100000)
        return false;
      if (!ny_ct_interp_eval_stmt(s->as.whl.body, ctx, ret, did_return,
                                  depth + 1))
        return false;
      if (*did_return)
        return true;
      if (s->as.whl.update) {
        if (!ny_ct_interp_eval_stmt(s->as.whl.update, ctx, ret, did_return,
                                    depth + 1))
          return false;
        if (*did_return)
          return true;
      }
    }
    return true;
  }
  case NY_S_MACRO: {
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      ny_ct_fast_val_t arg = ny_ct_fast_none();
      if (!ny_ct_interp_eval_expr(s->as.macro.args.data[i], ctx, &arg,
                                  depth + 1)) {
        ny_ct_fast_val_free(&arg);
        return false;
      }
      ny_ct_fast_val_free(&arg);
    }
    if (!s->as.macro.body)
      return true;
    return ny_ct_interp_eval_stmt(s->as.macro.body, ctx, ret, did_return,
                                  depth + 1);
  }
  case NY_S_OPERATOR:
    return true;
  default:
    return false;
  }
}

static bool ny_ct_interp_eval_expr(expr_t *e, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *out, int depth) {
  if (!e || !ctx || !out)
    return false;
  if (depth > 256 || !ny_ct_interp_step(ctx))
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = e->as.literal.as.i;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_BOOL) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = e->as.literal.as.b;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_STR) {
      out->kind = NY_CT_FAST_STR;
      out->s = e->as.literal.as.s.data;
      return true;
    }
    return false;
  case NY_E_IDENT: {
    const char *name = e->as.ident.name;
    if (!name || !*name)
      return false;
    if (strcmp(name, "none") == 0) {
      *out = ny_ct_fast_none();
      return true;
    }
    if (ny_ct_platform_ident(name, out))
      return true;
    if (ny_ct_interp_get(ctx, name, out))
      return true;
    if (ctx->cg) {
      binding *b = lookup_global(ctx->cg, name);
      expr_t *init = b && !b->is_mut ? ny_binding_var_init_expr(b, name) : NULL;
      if (init && init != e)
        return ny_ct_interp_eval_expr(init, ctx, out, depth + 1);
      if (ny_try_eval_binding_comptime_const(ctx->cg, b, name, out,
                                             depth + 1))
        return true;
    }
    return false;
  }
  case NY_E_UNARY: {
    ny_ct_fast_val_t r = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(e->as.unary.right, ctx, &r, depth + 1))
      return false;
    bool ok = ny_ct_fast_eval_unary(e->as.unary.op, &r, out);
    ny_ct_fast_val_free(&r);
    return ok;
  }
  case NY_E_LOGICAL: {
    ny_ct_fast_val_t l = ny_ct_fast_none();
    bool lt = false;
    if (!ny_ct_interp_eval_expr(e->as.logical.left, ctx, &l, depth + 1) ||
        !ny_ct_fast_truthy(&l, &lt)) {
      ny_ct_fast_val_free(&l);
      return false;
    }
    if (strcmp(e->as.logical.op, "&&") == 0) {
      ny_ct_fast_val_free(&l);
      if (!lt) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = false;
        return true;
      }
      ny_ct_fast_val_t r = ny_ct_fast_none();
      bool rt = false;
      if (!ny_ct_interp_eval_expr(e->as.logical.right, ctx, &r, depth + 1) ||
          !ny_ct_fast_truthy(&r, &rt)) {
        ny_ct_fast_val_free(&r);
        return false;
      }
      out->kind = NY_CT_FAST_BOOL;
      out->b = rt;
      ny_ct_fast_val_free(&r);
      return true;
    }
    if (strcmp(e->as.logical.op, "||") == 0) {
      ny_ct_fast_val_free(&l);
      if (lt) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = true;
        return true;
      }
      ny_ct_fast_val_t r = ny_ct_fast_none();
      bool rt = false;
      if (!ny_ct_interp_eval_expr(e->as.logical.right, ctx, &r, depth + 1) ||
          !ny_ct_fast_truthy(&r, &rt)) {
        ny_ct_fast_val_free(&r);
        return false;
      }
      out->kind = NY_CT_FAST_BOOL;
      out->b = rt;
      ny_ct_fast_val_free(&r);
      return true;
    }
    ny_ct_fast_val_free(&l);
    return false;
  }
  case NY_E_BINARY: {
    ny_ct_fast_val_t l = ny_ct_fast_none(), r = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(e->as.binary.left, ctx, &l, depth + 1))
      return false;
    if (!ny_ct_interp_eval_expr(e->as.binary.right, ctx, &r, depth + 1)) {
      ny_ct_fast_val_free(&l);
      return false;
    }
    bool ok = ny_ct_fast_eval_binary(e->as.binary.op, &l, &r, out);
    ny_ct_fast_val_free(&l);
    ny_ct_fast_val_free(&r);
    return ok;
  }
  case NY_E_TERNARY: {
    ny_ct_fast_val_t c = ny_ct_fast_none();
    bool ct = false;
    if (!ny_ct_interp_eval_expr(e->as.ternary.cond, ctx, &c, depth + 1) ||
        !ny_ct_fast_truthy(&c, &ct)) {
      ny_ct_fast_val_free(&c);
      return false;
    }
    ny_ct_fast_val_free(&c);
    return ny_ct_interp_eval_expr(ct ? e->as.ternary.true_expr
                                     : e->as.ternary.false_expr,
                                  ctx, out, depth + 1);
  }
  case NY_E_LIST:
  case NY_E_TUPLE: {
    ny_ct_fast_kind_t kind =
        e->kind == NY_E_TUPLE ? NY_CT_FAST_TUPLE : NY_CT_FAST_LIST;
    if (!ny_ct_make_seq(kind, e->as.list_like.len, out))
      return false;
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      if (!ny_ct_interp_eval_expr(e->as.list_like.data[i], ctx, &out->items[i],
                                  depth + 1)) {
        ny_ct_fast_val_free(out);
        return false;
      }
    }
    return true;
  }
  case NY_E_COMPTIME: {
    ny_ct_interp_ctx_t nested = {0};
    ny_ct_fast_val_t nested_ret = ny_ct_fast_none();
    bool did_return = false;
    if (!ny_ct_interp_ctx_clone(ctx, &nested))
      return false;
    bool ok = ny_ct_interp_eval_stmt(e->as.comptime_expr.body, &nested,
                                     &nested_ret, &did_return, depth + 1);
    ctx->steps = nested.steps;
    ny_ct_interp_ctx_free(&nested);
    if (!ok) {
      ny_ct_fast_val_free(&nested_ret);
      return false;
    }
    ny_ct_fast_val_move(out, &nested_ret);
    return true;
  }
  case NY_E_MEMBER:
    if (e->as.member.name && strcmp(e->as.member.name, "long") == 0)
      return ny_ct_interp_eval_long_property(e->as.member.target, ctx, out,
                                             depth + 1);
    if (e->as.member.name && strcmp(e->as.member.name, "len") == 0) {
      ny_ct_fast_val_t target = ny_ct_fast_none();
      int64_t len = 0;
      if (!ny_ct_interp_eval_expr(e->as.member.target, ctx, &target,
                                  depth + 1) ||
          !ny_ct_seq_len(&target, &len)) {
        ny_ct_fast_val_free(&target);
        return false;
      }
      out->kind = NY_CT_FAST_INT;
      out->i = len;
      ny_ct_fast_val_free(&target);
      return true;
    }
    return false;
  case NY_E_INDEX: {
    if (!e->as.index.target || !e->as.index.start || e->as.index.stop ||
        e->as.index.step)
      return false;
    ny_ct_fast_val_t target = ny_ct_fast_none();
    ny_ct_fast_val_t idx = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(e->as.index.target, ctx, &target, depth + 1))
      return false;
    if (!ny_ct_interp_eval_expr(e->as.index.start, ctx, &idx, depth + 1) ||
        idx.kind != NY_CT_FAST_INT) {
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&idx);
      return false;
    }
    int64_t len = 0;
    if (!ny_ct_seq_len(&target, &len)) {
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&idx);
      return false;
    }
    int64_t resolved = idx.i;
    if (resolved < 0)
      resolved += len;
    bool ok = ny_ct_seq_item(&target, resolved, out);
    ny_ct_fast_val_free(&target);
    ny_ct_fast_val_free(&idx);
    return ok;
  }
  case NY_E_CALL: {
    if (!e->as.call.callee)
      return false;
    const char *name = ny_ct_fast_callee_leaf_name(e->as.call.callee);
    if (!name)
      return false;
    bool zero_arg = (e->as.call.args.len == 0);
    bool one_member_arg =
        (e->as.call.args.len == 1 && e->as.call.callee->kind == NY_E_MEMBER);
    if (strcmp(name, "range") == 0 && e->as.call.args.len >= 1 &&
        e->as.call.args.len <= 3) {
      ny_ct_fast_val_t vals[3] = {ny_ct_fast_none(), ny_ct_fast_none(),
                                  ny_ct_fast_none()};
      for (size_t i = 0; i < e->as.call.args.len; i++) {
        if (!ny_ct_interp_eval_expr(e->as.call.args.data[i].val, ctx, &vals[i],
                                    depth + 1) ||
            vals[i].kind != NY_CT_FAST_INT) {
          for (size_t j = 0; j <= i && j < 3; j++)
            ny_ct_fast_val_free(&vals[j]);
          return false;
        }
      }
      bool ok = false;
      if (e->as.call.args.len == 1)
        ok = ny_ct_make_range(0, vals[0].i, 1, out);
      else if (e->as.call.args.len == 2)
        ok = ny_ct_make_range(vals[0].i, vals[1].i, 1, out);
      else
        ok = ny_ct_make_range(vals[0].i, vals[1].i, vals[2].i, out);
      for (size_t j = 0; j < 3; j++)
        ny_ct_fast_val_free(&vals[j]);
      return ok;
    }
    if (strcmp(name, "range2") == 0 && e->as.call.args.len >= 2 &&
        e->as.call.args.len <= 3) {
      ny_ct_fast_val_t vals[3] = {ny_ct_fast_none(), ny_ct_fast_none(),
                                  ny_ct_fast_none()};
      for (size_t i = 0; i < e->as.call.args.len; i++) {
        if (!ny_ct_interp_eval_expr(e->as.call.args.data[i].val, ctx, &vals[i],
                                    depth + 1) ||
            vals[i].kind != NY_CT_FAST_INT) {
          for (size_t j = 0; j <= i && j < 3; j++)
            ny_ct_fast_val_free(&vals[j]);
          return false;
        }
      }
      bool ok = ny_ct_make_range(vals[0].i, vals[1].i,
                                 e->as.call.args.len == 3 ? vals[2].i : 1,
                                 out);
      for (size_t j = 0; j < 3; j++)
        ny_ct_fast_val_free(&vals[j]);
      return ok;
    }
    if (strcmp(name, "Z") == 0 && e->as.call.args.len == 1) {
      ny_ct_fast_val_t arg = ny_ct_fast_none();
      if (!ny_ct_interp_eval_expr(e->as.call.args.data[0].val, ctx, &arg,
                                  depth + 1))
        return false;
      bool ok = ny_ct_fast_eval_bigint_constructor_arg(&arg, out);
      ny_ct_fast_val_free(&arg);
      return ok;
    }
    if (strcmp(name, "__main") == 0 && (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = ny_comptime_main_enabled(ctx->cg, e->tok);
      return true;
    }
    if ((strcmp(name, "__os_name") == 0 || strcmp(name, "os") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_host_os_name();
      return true;
    }
    if ((strcmp(name, "__arch_name") == 0 || strcmp(name, "arch") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_host_arch_name();
      return true;
    }
    return false;
  }
  case NY_E_MEMCALL: {
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "map") == 0 &&
        e->as.memcall.args.len == 1) {
      expr_t *fn = e->as.memcall.args.data[0].val;
      return ny_ct_interp_eval_map(e->as.memcall.target, fn, ctx, out,
                                   depth + 1);
    }
    return false;
  }
  default:
    return false;
  }
}

static bool ny_ct_interp_to_tagged(const ny_ct_fast_val_t *v,
                                   int64_t *out_tagged) {
  return ny_ct_fast_to_tagged(v, out_tagged);
}

static bool ny_try_eval_comptime_interp_value(codegen_t *cg, stmt_t *body,
                                              ny_ct_fast_val_t *out) {
  if (!body || !out)
    return false;
  ny_ct_interp_ctx_t ctx = {0};
  ctx.cg = cg;
  ctx.max_steps = 500000;
  ny_ct_fast_val_t ret = ny_ct_fast_none();
  bool did_return = false;
  bool ok = ny_ct_interp_eval_stmt(body, &ctx, &ret, &did_return, 0);
  ny_ct_interp_ctx_free(&ctx);
  if (!ok) {
    ny_ct_fast_val_free(&ret);
    return false;
  }
  ny_ct_fast_val_move(out, &ret);
  return true;
}

static bool ny_try_eval_comptime_interp(codegen_t *cg, stmt_t *body,
                                        int64_t *out_tagged) {
  ny_ct_fast_val_t ret = ny_ct_fast_none();
  if (!ny_try_eval_comptime_interp_value(cg, body, &ret))
    return false;
  bool ok = ny_ct_interp_to_tagged(&ret, out_tagged);
  ny_ct_fast_val_free(&ret);
  return ok;
}

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
    LLVMValueRef g = const_string_ptr(cg, v.s ? v.s : "", strlen(v.s ? v.s : ""));
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
        (s->as.fr.iter_index_var &&
         strcmp(s->as.fr.iter_index_var, name) == 0);
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
      if (closure_expr_contains_ident_name(s->as.enu.items.data[i].value,
                                           name))
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
  const char *callable_return_type = return_type;
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
  scope sc[64] = {0};

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
    env_ptr =
        LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
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

static LLVMValueRef ny_fun_sig_tagged_callable_adapter(codegen_t *cg,
                                                       fun_sig *target,
                                                       token_t tok,
                                                       bool hidden_env) {
  if (!cg || !target || !target->name)
    return NULL;
  uint64_t h =
      target->name_hash ? target->name_hash : ny_hash64_cstr(target->name);
  char adapter_name_buf[96];
  snprintf(adapter_name_buf, sizeof(adapter_name_buf),
           hidden_env ? "__ny_callable_adapter_env_%llx_%d"
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
    param_t p = {.name = pname};
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

  scope sc[64] = {0};
  binding_list empty_captures = {0};
  gen_func(cg, wrapper, wrapper->as.fn.name, sc, 0,
           hidden_env ? &empty_captures : NULL);
  LLVMValueRef adapter = ny_get_named_fn(cg, wrapper->as.fn.name);
  if (adapter)
    LLVMSetLinkage(adapter, LLVMInternalLinkage);
  return adapter;
}

static bool ny_named_callable_values_need_closure(codegen_t *cg) {
  return ny_module_target_is_apple_arm64(cg ? cg->module : NULL);
}

static LLVMValueRef ny_box_callable_closure(codegen_t *cg, LLVMValueRef fn_ptr_raw,
                                            token_t tok) {
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

  LLVMValueRef not_none = ny_ne(cg, v, ny_cnil(cg), "");
  LLVMValueRef not_zero = ny_ne(cg, v, ny_c1(cg), "");
  LLVMValueRef not_false = ny_ne(cg, v, ny_cfalse(cg), "");
  return ny_and(cg, ny_and(cg, not_none, not_zero, ""), not_false,
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
  *(uint64_t *)(obj_data + 48) =
      ((uint64_t)final_len << 1) | 1;
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
  LLVMValueRef one = ny_c1(cg);
  LLVMValueRef lsb = ny_and(cg, v, one, NY_LLVM_NAME(cg, "int_lsb"));
  return ny_eq(cg, lsb, one, NY_LLVM_NAME(cg, "is_tagged_int"));
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
    sig = resolve_overload(cg, "std.core.range2", 3, 0);
  if (!sig)
    sig = resolve_overload(cg, "std.core.iter.range2", 3, 0);
  return sig;
}

static LLVMValueRef gen_range_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {
  if (!e || !e->as.binary.left || !e->as.binary.right)
    return ny_c0(cg);
  fun_sig *sig = ny_lookup_range2(cg);
  if (!sig) {
    ny_diag_hint("import std.core or std.core.iter to provide range2");
    return expr_fail(cg, e->tok, "range expression '..' requires range2(start, stop, step)");
  }
  LLVMValueRef start = gen_expr(cg, scopes, depth, e->as.binary.left);
  LLVMValueRef stop = gen_expr(cg, scopes, depth, e->as.binary.right);
  LLVMValueRef one = LLVMConstInt(cg->type_i64, ((uint64_t)1 << 1) | 1u, false);
  LLVMValueRef inclusive_stop = gen_binary(cg, scopes, depth, "+", stop, one,
                                           e->as.binary.right, NULL);
  LLVMValueRef args[3] = {
      sig->is_native_abi ? ny_coerce_to_abi(cg, start, "int") : start,
      sig->is_native_abi ? ny_coerce_to_abi(cg, inclusive_stop, "int") : inclusive_stop,
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
  fun_sig *unbox =
      ny_module_target_is_apple_arm64(cg ? cg->module : NULL) ? lookup_fun(cg, "__flt_unbox_val", 0)
                                                              : NULL;
  if (unbox) {
    LLVMValueRef bits = LLVMBuildCall2(cg->builder, unbox->type, unbox->value, &v, 1, "");
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

static LLVMValueRef ny_unbox_known_numeric_float(codegen_t *cg, LLVMValueRef v) {
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
  LLVMValueRef ptr =
      LLVMBuildIntToPtr(cg->builder, v, LLVMPointerType(cg->type_f64, 0),
                        "known_flt_ptr");
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

static LLVMValueRef ny_try_emit_f64_list_get_as_f64(codegen_t *cg,
                                                    scope *scopes,
                                                    size_t depth, expr_t *e) {
  if (!cg || !e || e->kind != NY_E_MEMCALL || !e->as.memcall.name ||
      !ny_name_tail_is(e->as.memcall.name, "get") ||
      !e->as.memcall.target ||
      (e->as.memcall.args.len != 1 && e->as.memcall.args.len != 2))
    return NULL;
  if (!expr_f64_list_target_binding(cg, scopes, depth, e->as.memcall.target))
    return NULL;
  expr_t *key = e->as.memcall.args.data[0].val;
  expr_t *default_expr =
      e->as.memcall.args.len == 2 ? e->as.memcall.args.data[1].val : NULL;
  if (!expr_index_is_int_key(cg, scopes, depth, key) ||
      !expr_is_f64_default_value(cg, scopes, depth, default_expr))
    return NULL;

  LLVMValueRef target_v = expr_cast_to_i64(
      cg, gen_expr(cg, scopes, depth, e->as.memcall.target),
      "f64_list_get_target");
  LLVMValueRef key_v =
      expr_cast_to_i64(cg, gen_expr(cg, scopes, depth, key),
                       "f64_list_get_key");
  if (!target_v || !key_v)
    return NULL;
  LLVMValueRef default_v =
      default_expr ? gen_expr_as_f64(cg, scopes, depth, default_expr)
                   : LLVMConstReal(cg->type_f64, 0.0);

  LLVMValueRef target_ptr =
      LLVMBuildIntToPtr(cg->builder, target_v, ny_ptr_i64_ty(cg),
                        "f64_list_get_ptr_i64");
  LLVMValueRef len_tagged = ny_load(cg, target_ptr, "f64_list_get_len");
  LLVMValueRef len_raw =
      expr_build_untagged_or_raw_i64(cg, len_tagged, "f64_list_get_len_raw");
  LLVMValueRef key_raw =
      expr_index_raw_i64(cg, scopes, depth, key, key_v, "f64_list_get_key_raw");

  int64_t key_min = 0, key_max = 0;
  bool key_range = expr_int_range(cg, scopes, depth, key, &key_min, &key_max);
  bool assume_nonnegative = key_range && key_min >= 0;
  LLVMValueRef adj_key = key_raw;
  if (!assume_nonnegative) {
    LLVMValueRef is_neg =
        LLVMBuildICmp(cg->builder, LLVMIntSLT, key_raw, ny_c0(cg),
                      "f64_list_get_is_neg");
    LLVMValueRef wrapped =
        ny_add(cg, key_raw, len_raw, "f64_list_get_wrapped_idx");
    adj_key = ny_select(cg, is_neg, wrapped, key_raw, "f64_list_get_adj_idx");
  }

  bool assume_in_bounds = false;
  int64_t len_min = 0;
  if (key_range && key_min >= 0 &&
      expr_list_len_min(cg, scopes, depth, e->as.memcall.target, &len_min) &&
      key_max < len_min)
    assume_in_bounds = true;

  LLVMValueRef elem_addr = NULL;
  LLVMValueRef elem_ptr = NULL;
  LLVMValueRef boxed = NULL;
  if (assume_in_bounds) {
    LLVMValueRef scaled =
        LLVMBuildShl(cg->builder, adj_key, LLVMConstInt(cg->type_i64, 3, false),
                     "f64_list_get_scaled");
    LLVMValueRef byte_off =
        ny_add(cg, LLVMConstInt(cg->type_i64, 16, false), scaled,
               "f64_list_get_off");
    elem_addr = ny_add(cg, target_v, byte_off, "f64_list_get_addr");
    elem_ptr = LLVMBuildIntToPtr(cg->builder, elem_addr, ny_ptr_i64_ty(cg),
                                 "f64_list_get_elem_ptr_i64");
    boxed = ny_load(cg, elem_ptr, "f64_list_get_elem");
    LLVMValueRef ptr = LLVMBuildIntToPtr(
        cg->builder, boxed, LLVMPointerType(cg->type_f64, 0),
        "f64_list_get_fptr");
    return LLVMBuildLoad2(cg->builder, cg->type_f64, ptr,
                          "f64_list_get_f64");
  }

  LLVMBasicBlockRef entry_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
  LLVMBasicBlockRef load_bb = ny_bb_fn(fn, "f64_list_get.load");
  LLVMBasicBlockRef default_bb = ny_bb_fn(fn, "f64_list_get.default");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "f64_list_get.join");

  LLVMValueRef low_ok =
      LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_key, ny_c0(cg),
                    "f64_list_get_low_ok");
  LLVMValueRef high_ok =
      LLVMBuildICmp(cg->builder, LLVMIntSLT, adj_key, len_raw,
                    "f64_list_get_hi_ok");
  LLVMValueRef in_bounds =
      ny_and(cg, low_ok, high_ok, "f64_list_get_in_bounds");
  ny_cond_br(cg, in_bounds, load_bb, default_bb);

  ny_pos(cg, load_bb);
  LLVMValueRef scaled =
      LLVMBuildShl(cg->builder, adj_key, LLVMConstInt(cg->type_i64, 3, false),
                   "f64_list_get_scaled");
  LLVMValueRef byte_off =
      ny_add(cg, LLVMConstInt(cg->type_i64, 16, false), scaled,
             "f64_list_get_off");
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
  static const char *const k_names[] = {"std.core.get", "std.core.reflect.get", "get"};
  return ny_helper_lookup(cg, &cg->cached_fn_get, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_index_read(codegen_t *cg) {
  static const char *const k_names[] = {"std.core.index_read", "std.core.reflect.index_read",
                                        "index_read"};
  return ny_helper_lookup(cg, &cg->cached_fn_index_read, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static LLVMValueRef expr_try_fast_index_read(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e,
                                             fun_sig *fallback_sig) {
  if (!cg || !e || !fallback_sig || !e->as.index.target || !e->as.index.start)
    return NULL;
  if (ny_env_enabled("NYTRIX_INDEX_READ_PARITY") ||
      ny_env_enabled("NYTRIX_DISABLE_FAST_INDEX_READ"))
    return NULL;
  if (!expr_target_is_known_list_like(cg, scopes, depth, e->as.index.target) ||
      !expr_index_is_int_key(cg, scopes, depth, e->as.index.start))
    return NULL;

  LLVMValueRef target_v = gen_expr(cg, scopes, depth, e->as.index.target);
  LLVMValueRef key_v = gen_expr(cg, scopes, depth, e->as.index.start);
  if (!target_v || !key_v)
    return NULL;
  target_v = expr_cast_to_i64(cg, target_v, "fast_index_target");
  key_v = expr_cast_to_i64(cg, key_v, "fast_index_key");
  bool assume_nonnegative =
      expr_index_is_nonnegative(cg, scopes, depth, e->as.index.start);
  bool assume_in_bounds =
      assume_nonnegative &&
      expr_index_in_list_len_min(cg, scopes, depth, e->as.index.target,
                                 e->as.index.start);

  ny_dbg_loc(cg, e->tok);
  LLVMValueRef key_raw =
      expr_index_raw_i64(cg, scopes, depth, e->as.index.start, key_v,
                         "fast_index_key_raw");
  if (assume_in_bounds) {
    LLVMValueRef scaled =
        LLVMBuildShl(cg->builder, key_raw, LLVMConstInt(cg->type_i64, 3, false),
                     "fast_index_inbounds_scaled");
    LLVMValueRef byte_off =
        LLVMBuildAdd(cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
                     "fast_index_inbounds_off");
    LLVMValueRef elem_addr =
        ny_add(cg, target_v, byte_off, NY_LLVM_NAME(cg, "fast_index_addr"));
    LLVMValueRef elem_ptr =
        LLVMBuildIntToPtr(cg->builder, elem_addr, ny_ptr_i64_ty(cg),
                          "fast_index_elem_ptr_i64");
    return ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "fast_index_elem"));
  }

  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef load_bb = ny_bb_fn(fn, "fast_index.load");
  LLVMBasicBlockRef fallback_bb = ny_bb_fn(fn, "fast_index.fallback");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "fast_index.join");

  LLVMValueRef target_ptr = LLVMBuildIntToPtr(
      cg->builder, target_v, ny_ptr_i64_ty(cg), "fast_index_ptr_i64");
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
  if (assume_nonnegative && ny_is_proven_int(cg, scopes, depth,
                                             e->as.index.start, key_v)) {
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
  LLVMValueRef scaled =
      LLVMBuildShl(cg->builder, adj_idx, LLVMConstInt(cg->type_i64, 3, false),
                   "fast_index_scaled");
  LLVMValueRef byte_off =
      LLVMBuildAdd(cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
                   "fast_index_off");
  LLVMValueRef elem_addr =
      ny_add(cg, target_v, byte_off, NY_LLVM_NAME(cg, "fast_index_addr"));
  LLVMValueRef elem_ptr =
      LLVMBuildIntToPtr(cg->builder, elem_addr, ny_ptr_i64_ty(cg),
                        "fast_index_elem_ptr_i64");
  LLVMValueRef elem =
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
  LLVMValueRef incoming_vals[2] = {elem, fallback};
  LLVMBasicBlockRef incoming_bbs[2] = {load_end_bb, fallback_end_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

static fun_sig *ny_helper_dict(codegen_t *cg) {
  static const char *const k_names[] = {"dict", "std.core.dict",
                                        "std.core.dict_mod.dict"};
  return ny_helper_lookup(cg, &cg->cached_fn_dict, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_set(codegen_t *cg) {
  static const char *const k_names[] = {"std.core.set", "set"};
  return ny_helper_lookup(cg, &cg->cached_fn_set, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static fun_sig *ny_helper_set_add(codegen_t *cg) {
  static const char *const k_names[] = {"set_add", "std.core.set_add"};
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
  static const char *const k_names[] = {"to_str", "std.core.to_str",
                                        "std.core.reflect.to_str", "__to_str"};
  return ny_helper_lookup(cg, &cg->cached_fn_to_str, k_names,
                          sizeof(k_names) / sizeof(k_names[0]));
}

static size_t ny_count_unterminated_blocks(LLVMModuleRef mod) {
  size_t count = 0;
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f); bb;
         bb = LLVMGetNextBasicBlock(bb)) {
      if (!LLVMGetBasicBlockTerminator(bb))
        count++;
    }
  }
  return count;
}

static LLVMValueRef *ny_seal_unterminated_blocks(LLVMModuleRef mod,
                                                 LLVMContextRef ctx,
                                                 size_t *out_count) {
  size_t count = ny_count_unterminated_blocks(mod);
  if (out_count)
    *out_count = count;
  if (count == 0)
    return NULL;

  LLVMValueRef *terms = calloc(count, sizeof(*terms));
  if (!terms) {
    if (out_count)
      *out_count = 0;
    return NULL;
  }

  LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
  size_t idx = 0;
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f); bb;
         bb = LLVMGetNextBasicBlock(bb)) {
      if (LLVMGetBasicBlockTerminator(bb))
        continue;
      LLVMPositionBuilderAtEnd(builder, bb);
      terms[idx++] = LLVMBuildUnreachable(builder);
    }
  }
  LLVMDisposeBuilder(builder);
  return terms;
}

static void ny_unseal_blocks(LLVMValueRef *terms, size_t count) {
  if (!terms)
    return;
  for (size_t i = 0; i < count; i++) {
    if (terms[i])
      LLVMInstructionEraseFromParent(terms[i]);
  }
  free(terms);
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
    return LLVMBuildCall2(cg->builder, big_from_str->type,
                          big_from_str->value, (LLVMValueRef[]){str_ptr}, 1,
                          "ct_big_lit");
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
    LLVMValueRef out =
        LLVMBuildCall2(cg->builder, list_new->type, list_new->value,
                       (LLVMValueRef[]){tagged_len}, 1,
                       NY_LLVM_NAME(cg, "ct_interp_list"));
    for (size_t i = 0; i < v->len; i++) {
      LLVMValueRef item =
          ny_ct_fast_to_llvm_value(cg, &v->items[i], tok);
      if (!item)
        return NULL;
      (void)LLVMBuildCall2(
          cg->builder, store_item->type, store_item->value,
          (LLVMValueRef[]){
              out,
              LLVMConstInt(cg->type_i64, (((uint64_t)i << 1) | 1u), false),
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
  if (v->kind == NY_CT_FAST_RANGE) {
    if (!ny_small_int_fits_i64(v->range_start) ||
        !ny_small_int_fits_i64(v->range_stop) ||
        !ny_small_int_fits_i64(v->range_step))
      return expr_fail(cg, tok, "comptime range bounds are out of range");
    fun_sig *range_new = lookup_fun(cg, "__range_new", 0);
    if (!range_new)
      return expr_fail(cg, tok,
                       "comptime range result requires range runtime helper");
    LLVMValueRef start =
        LLVMConstInt(cg->type_i64, (((uint64_t)v->range_start << 1) | 1u),
                     true);
    LLVMValueRef stop =
        LLVMConstInt(cg->type_i64, (((uint64_t)v->range_stop << 1) | 1u),
                     true);
    LLVMValueRef step =
        LLVMConstInt(cg->type_i64, (((uint64_t)v->range_step << 1) | 1u),
                     true);
    return LLVMBuildCall2(cg->builder, range_new->type, range_new->value,
                          (LLVMValueRef[]){start, stop, step}, 3,
                          NY_LLVM_NAME(cg, "ct_interp_range"));
  }
  return NULL;
}

static bool ny_ct_jit_heap_tag(int64_t v, int64_t *tag) {
  if (!tag)
    return false;
  int64_t p = rt_heap_object_ptr(v);
  if (!p)
    return false;
  int64_t raw = 0;
  if (!rt_try_read_i64((uintptr_t)p - 8, &raw))
    return false;
  *tag = raw;
  return true;
}

static bool ny_ct_jit_seq_len(int64_t v, int64_t *out_len) {
  if (!out_len)
    return false;
  int64_t p = rt_heap_object_ptr(v);
  if (!p)
    return false;
  int64_t tagged = 0;
  if (!rt_try_read_i64((uintptr_t)p, &tagged))
    return false;
  int64_t n = is_int(tagged) ? (tagged >> 1) : tagged;
  if (n < 0 || n > 65536)
    return false;
  *out_len = n;
  return true;
}

static bool ny_ct_jit_seq_item(int64_t v, int64_t idx, int64_t *out) {
  if (!out || idx < 0)
    return false;
  int64_t p = rt_heap_object_ptr(v);
  if (!p)
    return false;
  return rt_try_read_i64((uintptr_t)p + 16 + (uintptr_t)idx * 8, out);
}

static LLVMValueRef ny_ct_jit_value_to_llvm(codegen_t *cg, int64_t v,
                                            token_t tok, int depth) {
  if (!cg || depth > 64)
    return NULL;
  if (v == NY_IMM_NIL || v == NY_IMM_FALSE || v == NY_IMM_TRUE || is_int(v))
    return LLVMConstInt(cg->type_i64, (uint64_t)v, true);

  if (is_v_str(v)) {
    size_t len = rt_tagged_str_len(v);
    LLVMValueRef g =
        const_string_ptr(cg, (const char *)(uintptr_t)v, len);
    return ny_load(cg, g, NY_LLVM_NAME(cg, "ct_jit_str"));
  }

  int64_t tag = 0;
  if (!ny_ct_jit_heap_tag(v, &tag))
    return NULL;

  if (tag == TAG_LIST || tag == TAG_TUPLE) {
    int64_t n = 0;
    if (!ny_ct_jit_seq_len(v, &n))
      return NULL;
    fun_sig *list_new = lookup_fun(cg, "__list_new", 0);
    fun_sig *store_item = lookup_fun(cg, "__store_item_fast", 0);
    fun_sig *set_len = lookup_fun(cg, "__list_set_len", 0);
    if (!list_new || !store_item || !set_len)
      return expr_fail(cg, tok,
                       "comptime list result requires list runtime helpers");
    LLVMValueRef out = LLVMBuildCall2(
        cg->builder, list_new->type, list_new->value,
        (LLVMValueRef[]){LLVMConstInt(cg->type_i64,
                                      (((uint64_t)n << 1) | 1u), false)},
        1, NY_LLVM_NAME(cg, "ct_jit_list"));
    for (int64_t i = 0; i < n; i++) {
      int64_t item_raw = 0;
      if (!ny_ct_jit_seq_item(v, i, &item_raw))
        return NULL;
      LLVMValueRef item =
          ny_ct_jit_value_to_llvm(cg, item_raw, tok, depth + 1);
      if (!item)
        return NULL;
      (void)LLVMBuildCall2(
          cg->builder, store_item->type, store_item->value,
          (LLVMValueRef[]){
              out,
              LLVMConstInt(cg->type_i64, (((uint64_t)i << 1) | 1u), false),
              item},
          3, "");
    }
    (void)LLVMBuildCall2(
        cg->builder, set_len->type, set_len->value,
        (LLVMValueRef[]){out, LLVMConstInt(cg->type_i64,
                                           (((uint64_t)n << 1) | 1u), false)},
        2, "");
    if (tag == TAG_TUPLE) {
      fun_sig *as_tuple = lookup_fun(cg, "__list_as_tuple", 0);
      if (!as_tuple)
        return expr_fail(cg, tok,
                         "comptime tuple result requires tuple runtime helper");
      out = LLVMBuildCall2(cg->builder, as_tuple->type, as_tuple->value,
                           (LLVMValueRef[]){out}, 1,
                           NY_LLVM_NAME(cg, "ct_jit_tuple"));
    }
    return out;
  }

  if (tag == TAG_RANGE) {
    int64_t p = rt_heap_object_ptr(v);
    int64_t start = 0, stop = 0, step = 0;
    if (!p || !rt_try_read_i64((uintptr_t)p + 0, &start) ||
        !rt_try_read_i64((uintptr_t)p + 8, &stop) ||
        !rt_try_read_i64((uintptr_t)p + 16, &step))
      return NULL;
    fun_sig *range_new = lookup_fun(cg, "__range_new", 0);
    if (!range_new)
      return expr_fail(cg, tok,
                       "comptime range result requires range runtime helper");
    return LLVMBuildCall2(cg->builder, range_new->type, range_new->value,
                          (LLVMValueRef[]){
                              LLVMConstInt(cg->type_i64, (uint64_t)start, true),
                              LLVMConstInt(cg->type_i64, (uint64_t)stop, true),
                              LLVMConstInt(cg->type_i64, (uint64_t)step, true)},
                          3, NY_LLVM_NAME(cg, "ct_jit_range"));
  }

  return NULL;
}

static LLVMTypeRef ny_ct_parent_global_type(codegen_t *tcg, const binding *b) {
  if (!tcg || !b)
    return NULL;
  if (b->is_f64_slot || (b->type_name &&
                         (strcmp(b->type_name, "f64") == 0 ||
                          strcmp(b->type_name, "float") == 0)))
    return tcg->type_f64;
  if (b->is_f32_slot || (b->type_name && strcmp(b->type_name, "f32") == 0))
    return tcg->type_f32;
  return tcg->type_i64;
}

static const char *ny_ct_global_module_name(codegen_t *tcg,
                                            const binding *b) {
  if (!tcg || !b || !b->name)
    return NULL;
  const char *dot = strrchr(b->name, '.');
  if (!dot || dot == b->name)
    return NULL;
  size_t len = (size_t)(dot - b->name);
  char *out = arena_alloc(tcg->arena, len + 1);
  if (!out)
    return NULL;
  memcpy(out, b->name, len);
  out[len] = '\0';
  return out;
}

static bool ny_ct_seen_stmt(stmt_t **seen, size_t len, stmt_t *s) {
  if (!s)
    return true;
  for (size_t i = 0; i < len; i++) {
    if (seen[i] == s)
      return true;
  }
  return false;
}

static const char *ny_ct_parent_stmt_module(codegen_t *parent,
                                            const fun_sig *src) {
  const char *best = src ? src->module_name : NULL;
  size_t best_len = best ? strlen(best) : 0;
  if (!parent || !src || !src->stmt_t)
    return best;
  for (size_t i = 0; i < parent->fun_sigs.len; i++) {
    fun_sig *other = &parent->fun_sigs.data[i];
    if (!other || other->stmt_t != src->stmt_t || !other->module_name)
      continue;
    size_t len = strlen(other->module_name);
    if (len > best_len) {
      best = other->module_name;
      best_len = len;
    }
  }
  return best;
}

static const char *ny_ct_parent_stmt_canonical_name(codegen_t *parent,
                                                    const fun_sig *src,
                                                    const char *module_name) {
  if (!parent || !src || !src->stmt_t || !module_name)
    return NULL;
  const char *decl_name = NULL;
  if (src->stmt_t->kind == NY_S_FUNC)
    decl_name = src->stmt_t->as.fn.name;
  else if (src->stmt_t->kind == NY_S_EXTERN)
    decl_name = src->stmt_t->as.ext.name;
  if (decl_name && *decl_name) {
    if (src->name && strcmp(src->name, decl_name) == 0)
      return src->name;
    for (size_t i = 0; i < parent->fun_sigs.len; i++) {
      fun_sig *other = &parent->fun_sigs.data[i];
      if (!other || other->stmt_t != src->stmt_t || !other->name)
        continue;
      if (strcmp(other->name, decl_name) == 0)
        return other->name;
    }
  }
  for (size_t i = 0; i < parent->fun_sigs.len; i++) {
    fun_sig *other = &parent->fun_sigs.data[i];
    if (!other || other == src || other->stmt_t != src->stmt_t ||
        !other->name || !other->module_name)
      continue;
    if (strcmp(other->module_name, module_name) == 0)
      return other->name;
  }
  return NULL;
}

static const fun_sig *ny_ct_parent_fun_sig_by_name(codegen_t *parent,
                                                   const char *name) {
  if (!parent || !name || !*name)
    return NULL;
  for (size_t i = 0; i < parent->fun_sigs.len; i++) {
    fun_sig *sig = &parent->fun_sigs.data[i];
    if (sig && sig->name && strcmp(sig->name, name) == 0)
      return sig;
  }
  return NULL;
}

static void ny_ct_collect_std_import_surface_stmt(codegen_t *tcg, stmt_t *s) {
  if (!tcg || !s || !ny_is_stdlib_tok(s->tok))
    return;
  if (s->kind == NY_S_MODULE) {
    process_use_imports(tcg, s);
    collect_use_aliases(tcg, s);
    collect_use_modules(tcg, s);
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      ny_ct_collect_std_import_surface_stmt(tcg, s->as.block.body.data[i]);
  }
}

static void ny_ct_collect_parent_std_import_surface(codegen_t *tcg,
                                                    codegen_t *parent) {
  if (!tcg || !parent)
    return;
  if (parent->prog) {
    for (size_t i = 0; i < parent->prog->body.len; i++)
      ny_ct_collect_std_import_surface_stmt(tcg, parent->prog->body.data[i]);
  }
  for (size_t p = 0; p < parent->extra_progs.len; p++) {
    program_t *prog = parent->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; i++)
      ny_ct_collect_std_import_surface_stmt(tcg, prog->body.data[i]);
  }
  ny_alias_lookup_cache_clear(tcg);
}

static void ny_ct_prune_repl_wrapper_aliases(codegen_t *tcg) {
  if (!tcg)
    return;
  for (size_t i = 0; i < tcg->aliases.len; i++) {
    binding *b = &tcg->aliases.data[i];
    const char *target = b && b->stmt_t ? (const char *)b->stmt_t : NULL;
    if (b && b->name && strcmp(b->name, "prim") == 0 && target &&
        strcmp(target, "std.os.prim") == 0) {
      b->name = "";
      b->stmt_t = NULL;
    }
  }
  binding prim = {0};
  prim.name = ny_strdup("prim");
  prim.stmt_t = (stmt_t *)ny_strdup("std.core.primitives");
  prim.owned = true;
  vec_push(&tcg->aliases, prim);
  if (tcg->aliases.len > 1) {
    binding inserted = tcg->aliases.data[tcg->aliases.len - 1];
    memmove(&tcg->aliases.data[1], &tcg->aliases.data[0],
            (tcg->aliases.len - 1) * sizeof(tcg->aliases.data[0]));
    tcg->aliases.data[0] = inserted;
  }
  ny_alias_lookup_cache_clear(tcg);
}

static void ny_ct_ensure_parent_fun_sig(codegen_t *tcg, const fun_sig *src,
                                        const char *module_name) {
  if (!tcg || !src || !src->name || !*src->name || !src->stmt_t)
    return;
  if (lookup_fun_exact(tcg, src->name))
    return;

  stmt_t *s = src->stmt_t;
  ny_param_list *params = NULL;
  const char *ret_name = NULL;
  bool is_variadic = src->is_variadic;
  bool is_extern = src->is_extern;
  if (s->kind == NY_S_FUNC) {
    params = &s->as.fn.params;
    ret_name = s->as.fn.return_type;
    is_variadic = s->as.fn.is_variadic;
    is_extern = false;
  } else if (s->kind == NY_S_EXTERN) {
    params = &s->as.ext.params;
    ret_name = s->as.ext.return_type;
    is_variadic = s->as.ext.is_variadic;
    is_extern = true;
  } else {
    return;
  }

  size_t param_count = params ? params->len : 0;
  LLVMTypeRef *pt = param_count ? alloca(sizeof(*pt) * param_count) : NULL;
  for (size_t i = 0; i < param_count; i++) {
    const char *ptype = params->data[i].type;
    bool tagged_user_fnptr = !is_extern && ptype && ny_type_is(ptype, "fnptr");
    pt[i] = (src->is_native_abi && ptype && !tagged_user_fnptr)
                ? resolve_abi_type_name(tcg, ptype, s->tok)
                : tcg->type_i64;
  }
  bool tagged_user_fnptr_ret = !is_extern && ret_name &&
                               ny_type_is(ret_name, "fnptr");
  LLVMTypeRef rty = (src->is_native_abi && ret_name &&
                     !tagged_user_fnptr_ret)
                        ? resolve_abi_type_name(tcg, ret_name, s->tok)
                        : tcg->type_i64;
  LLVMTypeRef ft = LLVMFunctionType(rty, pt, (unsigned)param_count, 0);
  const char *llvm_name = src->link_name && *src->link_name ? src->link_name
                                                            : src->name;
  LLVMValueRef f = ny_get_named_fn(tcg, llvm_name);
  if (!f)
    f = LLVMAddFunction(tcg->module, llvm_name, ft);

  fun_sig sig;
  ny_fun_sig_init(&sig, src->name, ft, f, s, (int)param_count, is_variadic,
                  is_extern);
  sig.module_name = module_name ? ny_strdup(module_name) : NULL;
  if (params)
    ny_fun_sig_set_params(&sig, params);
  sig.is_native_abi = src->is_native_abi;
  sig.tailcall = src->tailcall;
  sig.link_name = src->link_name ? ny_strdup(src->link_name) : NULL;
  sig.return_type = ret_name ? ny_strdup(ret_name) : NULL;
  sig.returns_owned = src->returns_owned;
  sig.effects = src->effects;
  sig.args_escape = src->args_escape;
  sig.args_mutated = src->args_mutated;
  sig.returns_alias = src->returns_alias;
  sig.effects_known = src->effects_known;
  vec_push(&tcg->fun_sigs, sig);
}

static void ny_ct_import_repl_parent_surface(codegen_t *tcg,
                                             codegen_t *parent) {
  if (!tcg || !parent || !parent->is_repl)
    return;

  for (size_t i = 0; i < parent->aliases.len; i++) {
    binding b = parent->aliases.data[i];
    if (!b.name || !strchr(b.name, '.'))
      continue;
    b.owned = false;
    vec_push(&tcg->aliases, b);
  }
  for (size_t i = 0; i < parent->import_aliases.len; i++) {
    binding b = parent->import_aliases.data[i];
    b.owned = false;
    vec_push(&tcg->import_aliases, b);
  }
  for (size_t i = 0; i < parent->user_import_aliases.len; i++) {
    binding b = parent->user_import_aliases.data[i];
    b.owned = false;
    vec_push(&tcg->user_import_aliases, b);
  }
  free(tcg->import_alias_index);
  tcg->import_alias_index = NULL;
  tcg->import_alias_index_cap = 0;
  free(tcg->user_import_alias_index);
  tcg->user_import_alias_index = NULL;
  tcg->user_import_alias_index_cap = 0;
  ny_alias_lookup_cache_clear(tcg);
  for (size_t i = 0; i < parent->use_modules.len; i++) {
    if (parent->use_modules.data[i])
      vec_push(&tcg->use_modules, ny_strdup(parent->use_modules.data[i]));
  }
  for (size_t i = 0; i < parent->user_use_modules.len; i++) {
    if (parent->user_use_modules.data[i])
      vec_push(&tcg->user_use_modules,
               ny_strdup(parent->user_use_modules.data[i]));
  }
  ny_ct_collect_parent_std_import_surface(tcg, parent);
  ny_ct_prune_repl_wrapper_aliases(tcg);

  const char *saved_mod = tcg->current_module_name;
  for (size_t i = 0; i < parent->fun_sigs.len; i++) {
    fun_sig *sig = &parent->fun_sigs.data[i];
    if (!sig || !sig->stmt_t || !ny_is_stdlib_tok(sig->stmt_t->tok))
      continue;
    const char *sig_mod = ny_ct_parent_stmt_module(parent, sig);
    const char *canonical = ny_ct_parent_stmt_canonical_name(parent, sig, sig_mod);
    if (canonical && strcmp(canonical, sig->name) != 0) {
      add_import_alias(tcg, sig->name, canonical);
      const fun_sig *target = ny_ct_parent_fun_sig_by_name(parent, canonical);
      if (target)
        ny_ct_ensure_parent_fun_sig(
            tcg, target, ny_ct_parent_stmt_module(parent, target));
      continue;
    }
    tcg->current_module_name = sig_mod;
    collect_sigs(tcg, sig->stmt_t);
    ny_ct_ensure_parent_fun_sig(tcg, sig, sig_mod);
  }
  tcg->current_module_name = saved_mod;

  for (size_t i = 0; i < parent->global_vars.len; i++) {
    binding *b = &parent->global_vars.data[i];
    if (!b || !b->name || !*b->name || !b->stmt_t ||
        !ny_is_stdlib_tok(b->stmt_t->tok))
      continue;
    if (lookup_global_exact(tcg, b->name))
      continue;
    LLVMTypeRef ty = ny_ct_parent_global_type(tcg, b);
    LLVMValueRef g = LLVMGetNamedGlobal(tcg->module, b->name);
    if (!g) {
      g = LLVMAddGlobal(tcg->module, ty ? ty : tcg->type_i64, b->name);
      LLVMSetInitializer(g, LLVMConstNull(ty ? ty : tcg->type_i64));
    }
    binding nb = *b;
    nb.value = g;
    nb.owned = false;
    vec_push(&tcg->global_vars, nb);
  }

  tcg->lazy_emit_stdlib_enabled = true;
}

static void ny_ct_emit_parent_std_init(codegen_t *tcg, codegen_t *parent,
                                       scope *scopes, size_t *depth) {
  if (!tcg || !parent || !parent->is_repl || !scopes || !depth)
    return;
  stmt_t **seen =
      parent->global_vars.len ? calloc(parent->global_vars.len, sizeof(*seen))
                              : NULL;
  size_t seen_len = 0;
  const char *saved_mod = tcg->current_module_name;
  for (size_t i = 0; i < parent->global_vars.len; i++) {
    binding *b = &parent->global_vars.data[i];
    if (!b || !b->stmt_t || !ny_is_stdlib_tok(b->stmt_t->tok) ||
        b->stmt_t->kind != NY_S_VAR)
      continue;
    const char *mod_name = ny_ct_global_module_name(tcg, b);
    if (tcg->lazy_emit_stdlib_enabled &&
        !ny_lazy_emit_stdlib_var_needed(tcg, b->stmt_t, mod_name))
      continue;
    if (ny_ct_seen_stmt(seen, seen_len, b->stmt_t))
      continue;
    if (seen)
      seen[seen_len++] = b->stmt_t;
    tcg->current_module_name = mod_name;
    gen_stmt(tcg, scopes, depth, b->stmt_t, 0, false);
    if (tcg->had_error)
      break;
  }
  tcg->current_module_name = saved_mod;
  free(seen);
}

static void ny_ct_emit_std_init(codegen_t *tcg, codegen_t *parent,
                                scope *scopes, size_t *depth) {
  if (!tcg || !tcg->prog || !scopes || !depth)
    return;
  for (size_t i = 0; i < tcg->prog->body.len; i++) {
    stmt_t *s = tcg->prog->body.data[i];
    if (!s || !ny_is_stdlib_tok(s->tok) || s->kind == NY_S_FUNC)
      continue;
    gen_stmt(tcg, scopes, depth, s, 0, false);
  }
  for (size_t p = 0; p < tcg->extra_progs.len; p++) {
    program_t *prog = tcg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; i++) {
      stmt_t *s = prog->body.data[i];
      if (!s || !ny_is_stdlib_tok(s->tok) || s->kind == NY_S_FUNC)
        continue;
      gen_stmt(tcg, scopes, depth, s, 0, false);
    }
  }
  ny_ct_emit_parent_std_init(tcg, parent, scopes, depth);
}

LLVMValueRef gen_comptime_eval(codegen_t *cg, stmt_t *body) {
  ny_ct_fast_val_t fast_value = ny_ct_fast_none();
  if (ny_try_eval_comptime_fast_value(cg, body, &fast_value)) {
    LLVMValueRef v = ny_ct_fast_to_llvm_value(cg, &fast_value, body->tok);
    ny_ct_fast_val_free(&fast_value);
    if (v)
      return v;
  }
  ny_ct_fast_val_free(&fast_value);

  ny_ct_fast_val_t interp_value = ny_ct_fast_none();
  if (ny_try_eval_comptime_interp_value(cg, body, &interp_value)) {
    LLVMValueRef v = ny_ct_fast_to_llvm_value(cg, &interp_value, body->tok);
    ny_ct_fast_val_free(&interp_value);
    if (v)
      return v;
  }
  ny_ct_fast_val_free(&interp_value);

  int64_t interp_tagged = 0;
  char *err = NULL;
  LLVMBasicBlockRef prev_bb = cg->builder ? ny_cur_block(cg) : NULL;

  size_t sealed_count = 0;
  LLVMValueRef *sealed_terms =
      ny_seal_unterminated_blocks(cg->module, cg->ctx, &sealed_count);
  LLVMMemoryBufferRef bitcode = LLVMWriteBitcodeToMemoryBuffer(cg->module);
  ny_unseal_blocks(sealed_terms, sealed_count);

  bool ctm_ctx_owned = true;
  LLVMContextRef ctm_ctx = LLVMContextCreate();
  LLVMModuleRef mod = NULL;
  if (LLVMParseBitcodeInContext(ctm_ctx, bitcode, &mod, &err) != 0) {
    NY_LOG_WARN(
        "Comptime snapshot parse failed; trying AST interpreter fallback: %s\n",
        err ? err : "unknown error");
    if (err) {
      LLVMDisposeMessage(err);
      err = NULL;
    }
    if (ny_try_eval_comptime_interp(cg, body, &interp_tagged)) {
      if (prev_bb)
        ny_pos(cg, prev_bb);
      LLVMDisposeMemoryBuffer(bitcode);
      LLVMContextDispose(ctm_ctx);
      return LLVMConstInt(cg->type_i64, (uint64_t)interp_tagged, true);
    }
    if (prev_bb)
      ny_pos(cg, prev_bb);
    LLVMDisposeMemoryBuffer(bitcode);
    LLVMContextDispose(ctm_ctx);
    return expr_fail(cg, body->tok, "failed to parse bitcode snapshot");
  }
  LLVMDisposeMemoryBuffer(bitcode);

  char entry_name[64];
  static int ctm_count = 0;
  snprintf(entry_name, sizeof(entry_name), "__ctm_entry_%d", ctm_count++);

  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    if (LLVMIsDeclaration(f)) {
      if (verbose_enabled >= 2) {
        fprintf(stderr, "[jit] snapshot decl: %s\n", LLVMGetValueName(f));
      }
    }
  }

  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    if (strcmp(LLVMGetValueName(f), entry_name) == 0)
      continue;
    bool broken = false;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f); bb;
         bb = LLVMGetNextBasicBlock(bb)) {
      if (!LLVMGetBasicBlockTerminator(bb)) {
        broken = true;
        break;
      }
    }
    if (broken) {
      if (verbose_enabled >= 1) {
        fprintf(stderr, "[jit] clearing broken function: %s\n",
                LLVMGetValueName(f));
      }
      ny_llvm_clear_function(f);
    }
  }

#ifdef _WIN32

  LLVMStripModuleDebugInfo(mod);
#else
  if (LLVMGetModuleContext(mod)) {
    LLVMDIBuilderRef snapshot_dib = LLVMCreateDIBuilder(mod);
    LLVMDIBuilderFinalize(snapshot_dib);
    LLVMDisposeDIBuilder(snapshot_dib);
  }
#endif

  char *verify_err = NULL;
  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verify_err) != 0) {
    NY_LOG_WARN("Comptime snapshot module verification failed: %s\n",
                verify_err);
    LLVMDisposeMessage(verify_err);
  }

  LLVMBuilderRef bld = LLVMCreateBuilderInContext(ctm_ctx);
  codegen_t tcg;
  codegen_init_with_context(&tcg, cg->prog, cg->arena, mod, ctm_ctx, bld);
  tcg.llvm_ctx_owned = false;
  tcg.parent = cg;
  tcg.comptime = true;
  tcg.strict_diagnostics = cg->strict_diagnostics;
  tcg.strict_types = cg->strict_types;
  tcg.ownership_enabled = cg->ownership_enabled;
  tcg.ownership_strict = cg->ownership_strict;
  tcg.debug_symbols = cg->debug_symbols;
  tcg.di_builder = NULL;
  tcg.source_main_file = cg->source_main_file;
  tcg.user_source = cg->user_source;
  tcg.user_source_len = cg->user_source_len;

  codegen_prepare(&tcg);
  ny_ct_import_repl_parent_surface(&tcg, cg);
  ny_lazy_emit_prepare_reachable(&tcg);

  LLVMValueRef entry_fn = LLVMAddFunction(
      mod, entry_name, LLVMFunctionType(tcg.type_i64, NULL, 0, 0));
  LLVMPositionBuilderAtEnd(
      bld, LLVMAppendBasicBlockInContext(ctm_ctx, entry_fn, "e"));

  codegen_repopulate_interns(&tcg);

  scope ctm_scopes[64] = {0};
  size_t ctm_depth = 0;
  ny_ct_emit_std_init(&tcg, cg, ctm_scopes, &ctm_depth);
  if (tcg.had_error) {
    if (prev_bb)
      ny_pos(cg, prev_bb);
    cg->had_error = 1;
    codegen_dispose(&tcg);
    if (ctm_ctx_owned)
      LLVMContextDispose(ctm_ctx);
    return ny_c0(cg);
  }
  gen_stmt(&tcg, ctm_scopes, &ctm_depth, body, 0, true);
  if (tcg.had_error) {
    if (prev_bb)
      ny_pos(cg, prev_bb);
    cg->had_error = 1;
    codegen_dispose(&tcg);
    if (ctm_ctx_owned)
      LLVMContextDispose(ctm_ctx);
    return ny_c0(cg);
  }
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(bld))) {
    LLVMBuildRet(bld, LLVMConstInt(tcg.type_i64, 1, false));
  }
  ny_lazy_emit_demand_referenced(&tcg, ctm_scopes, ctm_depth, "comptime");
  if (tcg.had_error) {
    if (prev_bb)
      ny_pos(cg, prev_bb);
    cg->had_error = 1;
    codegen_dispose(&tcg);
    if (ctm_ctx_owned)
      LLVMContextDispose(ctm_ctx);
    return ny_c0(cg);
  }
  LLVMBasicBlockRef ctm_end_bb = LLVMGetInsertBlock(bld);
  LLVMBasicBlockRef ctm_entry_bb = LLVMGetEntryBasicBlock(entry_fn);
  LLVMValueRef first_inst = LLVMGetFirstInstruction(ctm_entry_bb);
  if (first_inst)
    LLVMPositionBuilderBefore(bld, first_inst);
  else
    LLVMPositionBuilderAtEnd(bld, ctm_entry_bb);
  ny_ct_emit_std_init(&tcg, cg, ctm_scopes, &ctm_depth);
  codegen_emit_string_init(&tcg);
  if (ctm_end_bb)
    LLVMPositionBuilderAtEnd(bld, ctm_end_bb);

  ny_jit_define_runtime_trampolines(mod);

  if (getenv("NYTRIX_COMPTIME_DUMP_IR")) {
    char *ir = LLVMPrintModuleToString(mod);
    if (ir) {
      fprintf(stderr, "%s\n", ir);
      LLVMDisposeMessage(ir);
    }
  }

  LLVMExecutionEngineRef ee = NULL;
  struct LLVMMCJITCompilerOptions jit_opts;
  ny_jit_init_native_once();
  ny_jit_init_options(&jit_opts, mod);
  if (ny_module_target_is_apple_arm64(mod))
    jit_opts.EnableFastISel = 0;
  ny_jit_add_runtime_symbols();
  if (LLVMCreateMCJITCompilerForModule(&ee, mod, &jit_opts, sizeof(jit_opts),
                                       &err) != 0) {
    NY_LOG_ERR("Comptime JIT error: %s\n", err);
    LLVMDisposeMessage(err);
    codegen_dispose(&tcg);
    LLVMContextDispose(ctm_ctx);
    return expr_fail(cg, body->tok, "failed to create mcjit");
  }

  register_jit_symbols(ee, mod, &tcg);
  ny_jit_map_unresolved_symbols(ee, mod, entry_name);
  LLVMValueRef entry_val = LLVMGetNamedFunction(mod, entry_name);
  uint64_t addr = entry_val ? (uint64_t)LLVMGetPointerToGlobal(ee, entry_val) : 0;
  if (!addr)
    addr = LLVMGetFunctionAddress(ee, entry_name);
  int64_t res = 1;
  if (addr) {
    ny_jit_prepare_execution();
    res = ((int64_t (*)(void))addr)();
  }

  if (prev_bb)
    ny_pos(cg, prev_bb);

  LLVMValueRef materialized =
      ny_ct_jit_value_to_llvm(cg, res, body->tok, 0);

  LLVMDisposeExecutionEngine(ee);
  codegen_dispose(&tcg);
  if (ctm_ctx_owned)
    LLVMContextDispose(ctm_ctx);

  if (materialized)
    return materialized;

  return expr_fail(cg, body->tok,
                   "comptime result cannot be embedded in runtime code");
}

bool ny_eval_comptime_if(codegen_t *cg, stmt_t *s, bool *truthy) {
  if (!s || s->kind != NY_S_IF || !s->as.iff.test)
    return false;
  if (s->as.iff.test->kind != NY_E_COMPTIME)
    return false;
  LLVMValueRef val =
      gen_comptime_eval(cg, s->as.iff.test->as.comptime_expr.body);
  if (val && LLVMIsAConstantInt(val)) {
    uint64_t raw = LLVMConstIntGetZExtValue(val);
    if (truthy)
      *truthy = (raw != NY_IMM_NIL && raw != NY_IMM_FALSE && raw != 1);
    return true;
  }
  return false;
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
      if (ny_small_int_fits_i64(raw) && raw != INT64_MIN && ny_small_int_fits_i64(-raw))
        return LLVMConstInt(cg->type_i64, (((uint64_t)(-raw)) << 1) | 1u, true);
    } else if (right->as.literal.kind == NY_LIT_FLOAT) {
      fun_sig *box_sig = ny_helper_flt_box(cg);
      if (!box_sig)
        return expr_fail(cg, e->tok, "__flt_box_val not found");
      double d = -right->as.literal.as.f;
      if (right->as.literal.hint == NY_LIT_HINT_F32)
        d = (double)(float)d;
      LLVMValueRef fval = LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), d);
      return LLVMBuildCall2(cg->builder, box_sig->type, box_sig->value,
                            (LLVMValueRef[]){ny_bitcast(cg, fval, cg->type_i64, "")},
                            1, "");
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
    return LLVMBuildCall2(cg->builder, join_sig->type, join_sig->value,
                          &handle, 1, "async_await");
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
    LLVMValueRef argc_val =
        LLVMConstInt(cg->type_i64, (((uint64_t)argc << 1) | 1), false);
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
  return LLVMBuildCall2(cg->builder, s->type, s->value, args, 2, "");
}

LLVMValueRef gen_expr_list_stack_alloc(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e) {
  size_t item_count = e->as.list_like.len;

  LLVMValueRef mem_ptr = LLVMBuildAlloca(cg->builder, LLVMArrayType(cg->type_i64, 3 + item_count), "list_stack");
  LLVMSetAlignment(mem_ptr, 16);

  LLVMValueRef p_idx[2] = { ny_c0(cg), ny_c1(cg) };
  LLVMValueRef p_addr = LLVMBuildGEP2(cg->builder, LLVMArrayType(cg->type_i64, 3 + item_count), mem_ptr, p_idx, 2, "p_header");
  LLVMValueRef p = LLVMBuildPtrToInt(cg->builder, p_addr, cg->type_i64, "p_val");

  LLVMValueRef tag_addr = LLVMBuildIntToPtr(cg->builder, ny_sub(cg, p, ny_ci(cg, 8), ""), ny_ptr_i64_ty(cg), "");
  uint64_t tag = (e->kind == NY_E_LIST) ? TAG_LIST : TAG_TUPLE;
  ny_store(cg, tag_addr, ny_ci(cg, tag));

  LLVMValueRef len_addr = LLVMBuildIntToPtr(cg->builder, p, ny_ptr_i64_ty(cg), "");
  ny_store(cg, len_addr, ny_ci(cg, (item_count << 1) | 1));

  LLVMValueRef cap_addr = LLVMBuildIntToPtr(cg->builder, ny_add(cg, p, ny_ci(cg, 8), ""), ny_ptr_i64_ty(cg), "");
  ny_store(cg, cap_addr, ny_ci(cg, (item_count << 1) | 1));

  for (size_t i = 0; i < item_count; i++) {
    LLVMValueRef item_val = gen_expr(cg, scopes, depth, e->as.list_like.data[i]);
    LLVMValueRef item_addr = LLVMBuildIntToPtr(cg->builder, ny_add(cg, p, ny_ci(cg, 16 + i * 8), ""), ny_ptr_i64_ty(cg), "");
    ny_store(cg, item_addr, item_val);
  }

  return p;
}

static LLVMValueRef gen_expr_list_like(codegen_t *cg, scope *scopes,
                                        size_t depth, expr_t *e) {  fun_sig *ls = lookup_fun(cg, "__list_new", 0);
  fun_sig *st = lookup_fun(cg, "__store_item_fast", 0);
  fun_sig *set_len = lookup_fun(cg, "__list_set_len", 0);
  if (!ls || !st || !set_len)
    return expr_fail(cg, e->tok,
                     "list literal requires "
                     "__list_new/__store_item_fast/__list_set_len helpers");
  ny_dbg_loc(cg, e->tok);
  size_t item_count = e->as.list_like.len;
  LLVMValueRef vl = LLVMBuildCall2(
      cg->builder, ls->type, ls->value,
      (LLVMValueRef[]){LLVMConstInt(cg->type_i64,
                                    (((uint64_t)item_count << 1) | 1u), false)},
      1, "");
  for (size_t i = 0; i < item_count; i++) {
    if (i > 0 && i % 64 == 0) {
      LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
      LLVMValueRef cur_fn = LLVMGetBasicBlockParent(cur_bb);
      LLVMBasicBlockRef next_bb = ny_bb_fn(cur_fn, "lst_chunk");
      ny_br(cg, next_bb);
      ny_pos(cg, next_bb);
    }
    LLVMValueRef item = gen_expr(cg, scopes, depth, e->as.list_like.data[i]);
    ny_dbg_loc(cg, e->tok);
    (void)LLVMBuildCall2(
        cg->builder, st->type, st->value,
        (LLVMValueRef[]){
            vl, LLVMConstInt(cg->type_i64, (((uint64_t)i << 1) | 1u), false),
            item},
        3, "");
  }
  (void)LLVMBuildCall2(
      cg->builder, set_len->type, set_len->value,
      (LLVMValueRef[]){vl,
                       LLVMConstInt(cg->type_i64,
                                    (((uint64_t)item_count << 1) | 1u), false)},
      2, "");
  return vl;
}

static LLVMValueRef gen_expr_dict(codegen_t *cg, scope *scopes, size_t depth,
                                  expr_t *e) {
  fun_sig *ds = ny_helper_dict(cg);
  fun_sig *ss = ny_helper_set(cg);
  if (!ds || !ss)
    return expr_fail(cg, e->tok, "dict literal requires dict/set helpers");
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef dl = LLVMBuildCall2(
      cg->builder, ds->type, ds->value,
      (LLVMValueRef[]){LLVMConstInt(
          cg->type_i64, ((uint64_t)e->as.dict.pairs.len << 2) | 1, false)},
      1, "");
  for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
    if (i > 0 && i % 64 == 0) {
      LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
      LLVMValueRef cur_fn = LLVMGetBasicBlockParent(cur_bb);
      LLVMBasicBlockRef next_bb = ny_bb_fn(cur_fn, "dct_chunk");
      ny_br(cg, next_bb);
      ny_pos(cg, next_bb);
    }
    LLVMValueRef next = LLVMBuildCall2(
        cg->builder, ss->type, ss->value,
        (LLVMValueRef[]){
            dl, gen_expr(cg, scopes, depth, e->as.dict.pairs.data[i].key),
            gen_expr(cg, scopes, depth, e->as.dict.pairs.data[i].value)},
        3, "");
    if (next)
      dl = next;
  }
  return dl;
}

static LLVMValueRef gen_expr_set(codegen_t *cg, scope *scopes, size_t depth,
                                 expr_t *e) {
  fun_sig *ss = ny_helper_set(cg);
  fun_sig *as = ny_helper_set_add(cg);
  if (!ss || !as) {
    if (e->as.list_like.len == 0)
      return expr_fail(cg, e->tok, "use dict() for empty dict or {'key': val} for dict literal",
                       "'{}' is not valid syntax; use dict() for an empty dict");
    return expr_fail(cg, e->tok, "set literal requires set/set_add helpers");
  }
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef sl = LLVMBuildCall2(
      cg->builder, ss->type, ss->value,
      (LLVMValueRef[]){LLVMConstInt(
          cg->type_i64, ((uint64_t)e->as.list_like.len << 1) | 1, false)},
      1, "");
  for (size_t i = 0; i < e->as.list_like.len; i++)
    LLVMBuildCall2(cg->builder, as->type, as->value,
                   (LLVMValueRef[]){sl, gen_expr(cg, scopes, depth,
                                                 e->as.list_like.data[i])},
                   2, "");
  return sl;
}

static LLVMValueRef expr_gen_cond_i1(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e);

static LLVMValueRef gen_expr_logical(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  bool and = strcmp(e->as.logical.op, "&&") == 0;
  ny_null_narrow_list_t rhs_narrow;
  vec_init(&rhs_narrow);
  bool narrow_rhs =
      ny_null_narrow_collect_logical_rhs(e->as.logical.left, and, &rhs_narrow);
  LLVMValueRef left = expr_gen_cond_i1(cg, scopes, depth, e->as.logical.left);
  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef rhs_bb = ny_bb_fn(f, "lrhs"), end_bb = ny_bb_fn(f, "lend");
  ny_dbg_loc(cg, e->tok);
  if (and)
    ny_cond_br(cg, left, rhs_bb, end_bb);
  else
    ny_cond_br(cg, left, end_bb, rhs_bb);

  ny_pos(cg, rhs_bb);

  ny_null_narrow_restore_list_t rhs_applied;
  if (narrow_rhs)
    ny_null_narrow_apply(cg, scopes, depth, &rhs_narrow, true, &rhs_applied);
  LLVMValueRef rv = gen_expr(cg, scopes, depth, e->as.logical.right);
  if (narrow_rhs)
    ny_null_narrow_restore(&rhs_applied);
  vec_free(&rhs_narrow);
  ny_br(cg, end_bb);
  LLVMBasicBlockRef rend_bb = ny_cur_block(cg);

  ny_pos(cg, end_bb);

  LLVMValueRef phi = ny_phi(cg, cg->type_i64, "");
  LLVMAddIncoming(phi,
                  (LLVMValueRef[]){and ? ny_cfalse(cg)
                                       : ny_ctrue(cg),
                                   rv},
                  (LLVMBasicBlockRef[]){cur_bb, rend_bb}, 2);
  return phi;
}

static bool ny_name_leaf_is_f64_cast(const char *name) {
  if (!name)
    return false;
  const char *leaf = strrchr(name, '.');
  leaf = leaf ? leaf + 1 : name;
  return strcmp(leaf, "float") == 0 || strcmp(leaf, "to_float") == 0 ||
         strcmp(leaf, "f64") == 0;
}

static bool ny_call_expr_is_f64_cast(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT || e->as.call.args.len != 1)
    return false;
  size_t surface_len = 0;
  uint64_t surface_hash = 0;
  const char *surface =
      ny_builtin_surface_name_for_callee(e->as.call.callee, &surface_len,
                                         &surface_hash);
  if (surface) {
    bool shadowed = ny_builtin_name_shadowed_by_user_symbol(
        cg, scopes, depth, surface, surface_len, surface_hash);
    return !shadowed && ny_name_leaf_is_f64_cast(surface);
  }
  return ny_name_tail_is(e->as.call.callee->as.ident.name, "f64");
}

static bool ny_is_f64_like_limited(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e, unsigned budget) {
  if (!e)
    return false;
  if (budget == 0)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_FLOAT;
  if (e->kind == NY_E_IDENT) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                                     e->as.ident.hash);
    return b && (b->is_f64_slot || b->is_f64_direct || b->is_f32_slot ||
                 b->is_f32_direct);
  }
  if (e->kind == NY_E_BINARY) {
    return ny_is_f64_like_limited(cg, scopes, depth, e->as.binary.left,
                                  budget - 1) ||
           ny_is_f64_like_limited(cg, scopes, depth, e->as.binary.right,
                                  budget - 1);
  }
  if (e->kind == NY_E_INDEX) {
    const char *t = infer_expr_type(cg, scopes, depth, e);
    return t && (strcmp(t, "f64") == 0 || strcmp(t, "f32") == 0 ||
                 strcmp(t, "float") == 0);
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    if (ny_call_expr_is_f64_cast(cg, scopes, depth, e))
      return true;
    const char *fn_name = e->as.call.callee->as.ident.name;
    if (fn_name) {
      fun_sig *sig = resolve_overload(cg, fn_name, e->as.call.args.len, 0);
      const char *ret_type =
          sig ? (sig->return_type ? sig->return_type
                                  : sig->inferred_return_type)
              : NULL;
      return ret_type && (strcmp(ret_type, "f64") == 0 ||
                          strcmp(ret_type, "f32") == 0 ||
                          strcmp(ret_type, "float") == 0);
    }
  }
  return false;
}

static bool ny_is_f64_like(codegen_t *cg, scope *scopes, size_t depth,
                           expr_t *e) {
  return ny_is_f64_like_limited(cg, scopes, depth, e, 3);
}

static bool ny_is_f64_arith_op(const char *op);

static bool ny_is_numeric_expr_like_limited(codegen_t *cg, scope *scopes, size_t depth,
                                            expr_t *e, unsigned budget) {
  if (!e)
    return false;
  if (budget == 0)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT ||
           e->as.literal.kind == NY_LIT_FLOAT;
  if (e->kind == NY_E_IDENT) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                                     e->as.ident.hash);
    return b && (b->is_int_slot || b->is_int_direct || b->is_f64_slot ||
                 b->is_f64_direct || b->is_f32_slot || b->is_f32_direct ||
                 (b->type_name &&
                  (strcmp(b->type_name, "int") == 0 ||
                   strcmp(b->type_name, "i64") == 0 ||
                   strcmp(b->type_name, "f32") == 0 ||
                   strcmp(b->type_name, "f64") == 0)));
  }
  if (e->kind == NY_E_BINARY) {
    if (expr_int_range(cg, scopes, depth, e, NULL, NULL))
      return true;
    const char *op = e->as.binary.op;
    if (!ny_is_f64_arith_op(op))
      return false;
    return ny_is_numeric_expr_like_limited(cg, scopes, depth, e->as.binary.left,
                                           budget - 1) &&
           ny_is_numeric_expr_like_limited(cg, scopes, depth, e->as.binary.right,
                                           budget - 1);
  }
  if (e->kind == NY_E_INDEX) {
    const char *t = infer_expr_type(cg, scopes, depth, e);
    return t && (strcmp(t, "int") == 0 || strcmp(t, "i64") == 0 ||
                 strcmp(t, "f32") == 0 || strcmp(t, "f64") == 0 ||
                 strcmp(t, "float") == 0);
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    if (ny_call_expr_is_f64_cast(cg, scopes, depth, e))
      return true;
    const char *fn_name = e->as.call.callee->as.ident.name;
    if (fn_name) {
      fun_sig *sig = resolve_overload(cg, fn_name, e->as.call.args.len, 0);
      const char *ret_type =
          sig ? (sig->return_type ? sig->return_type
                                  : sig->inferred_return_type)
              : NULL;
      return ret_type && (strcmp(ret_type, "int") == 0 ||
                          strcmp(ret_type, "i64") == 0 ||
                          strcmp(ret_type, "f32") == 0 ||
                          strcmp(ret_type, "f64") == 0 ||
                          strcmp(ret_type, "float") == 0);
    }
  }
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0 &&
      (e->as.memcall.args.len == 1 || e->as.memcall.args.len == 2)) {
    const char *target_type =
        infer_expr_type(cg, scopes, depth, e->as.memcall.target);
    return target_type && (strcmp(target_type, "list") == 0 ||
                           strcmp(target_type, "tuple") == 0 ||
                           strncmp(target_type, "list<", 5) == 0 ||
                           strncmp(target_type, "tuple<", 6) == 0);
  }
  return false;
}

static bool ny_is_numeric_expr_like(codegen_t *cg, scope *scopes, size_t depth,
                                    expr_t *e) {
  return ny_is_numeric_expr_like_limited(cg, scopes, depth, e, 3);
}

static bool ny_is_f64_arith_op(const char *op) {
   return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
          strcmp(op, "/") == 0;
}

static bool expr_is_cmp_op(const char *op) {
  return op && (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);
}

static LLVMIntPredicate expr_icmp_pred_for_op(const char *op) {
  if (strcmp(op, "<") == 0)
    return LLVMIntSLT;
  if (strcmp(op, "<=") == 0)
    return LLVMIntSLE;
  if (strcmp(op, ">") == 0)
    return LLVMIntSGT;
  if (strcmp(op, ">=") == 0)
    return LLVMIntSGE;
  if (strcmp(op, "!=") == 0)
    return LLVMIntNE;
  return LLVMIntEQ;
}

static LLVMRealPredicate expr_fcmp_pred_for_op(const char *op) {
  if (strcmp(op, "<") == 0)
    return LLVMRealOLT;
  if (strcmp(op, "<=") == 0)
    return LLVMRealOLE;
  if (strcmp(op, ">") == 0)
    return LLVMRealOGT;
  if (strcmp(op, ">=") == 0)
    return LLVMRealOGE;
  if (strcmp(op, "!=") == 0)
    return LLVMRealUNE;
  return LLVMRealOEQ;
}

static LLVMValueRef expr_gen_cond_i1(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  if (!e)
    return LLVMConstInt(ny_i1_ty(cg), 0, false);

  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_BOOL)
    return LLVMConstInt(ny_i1_ty(cg), e->as.literal.as.b ? 1 : 0, false);

  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      strcmp(e->as.unary.op, "!") == 0) {
    LLVMValueRef inner = expr_gen_cond_i1(cg, scopes, depth, e->as.unary.right);
    return LLVMBuildNot(cg->builder, inner, "expr_cond_not");
  }

  if (e->kind == NY_E_LOGICAL && e->as.logical.op) {
    bool and_op = strcmp(e->as.logical.op, "&&") == 0;
    bool or_op = strcmp(e->as.logical.op, "||") == 0;
    if (and_op || or_op) {
      ny_null_narrow_list_t rhs_narrow;
      vec_init(&rhs_narrow);
      bool narrow_rhs =
          ny_null_narrow_collect_logical_rhs(e->as.logical.left, and_op, &rhs_narrow);

      LLVMValueRef left = expr_gen_cond_i1(cg, scopes, depth, e->as.logical.left);
      LLVMBasicBlockRef left_bb = ny_cur_block(cg);
      LLVMValueRef fn = LLVMGetBasicBlockParent(left_bb);
      LLVMBasicBlockRef rhs_bb = ny_bb_fn(fn, "expr_cond_rhs");
      LLVMBasicBlockRef end_bb = ny_bb_fn(fn, "expr_cond_end");
      if (and_op)
        ny_cond_br(cg, left, rhs_bb, end_bb);
      else
        ny_cond_br(cg, left, end_bb, rhs_bb);

      ny_pos(cg, rhs_bb);
      ny_null_narrow_restore_list_t rhs_applied;
      if (narrow_rhs)
        ny_null_narrow_apply(cg, scopes, depth, &rhs_narrow, true, &rhs_applied);
      LLVMValueRef right = expr_gen_cond_i1(cg, scopes, depth, e->as.logical.right);
      if (narrow_rhs)
        ny_null_narrow_restore(&rhs_applied);
      vec_free(&rhs_narrow);
      LLVMBasicBlockRef rhs_done = ny_cur_block(cg);
      ny_br(cg, end_bb);

      ny_pos(cg, end_bb);
      LLVMValueRef phi = ny_phi(cg, ny_i1_ty(cg), "expr_cond_logic");
      LLVMValueRef short_value =
          LLVMConstInt(ny_i1_ty(cg), and_op ? 0 : 1, false);
      LLVMAddIncoming(phi, (LLVMValueRef[]){short_value, right},
                      (LLVMBasicBlockRef[]){left_bb, rhs_done}, 2);
      return phi;
    }
  }

  if (e->kind == NY_E_BINARY && expr_is_cmp_op(e->as.binary.op)) {
    const char *op = e->as.binary.op;
    expr_t *le = e->as.binary.left;
    expr_t *re = e->as.binary.right;
    if ((ny_is_f64_like(cg, scopes, depth, le) ||
         ny_is_f64_like(cg, scopes, depth, re)) &&
        ny_is_numeric_expr_like(cg, scopes, depth, le) &&
        ny_is_numeric_expr_like(cg, scopes, depth, re)) {
      LLVMValueRef lf = gen_expr_as_f64(cg, scopes, depth, le);
      LLVMValueRef rf = gen_expr_as_f64(cg, scopes, depth, re);
      return LLVMBuildFCmp(cg->builder, expr_fcmp_pred_for_op(op), lf, rf,
                           "expr_cond_fcmp");
    }

    if (ny_is_proven_int(cg, scopes, depth, le, NULL) &&
        ny_is_proven_int(cg, scopes, depth, re, NULL)) {
      LLVMValueRef l = gen_expr(cg, scopes, depth, le);
      LLVMValueRef r = gen_expr(cg, scopes, depth, re);
      return LLVMBuildICmp(cg->builder, expr_icmp_pred_for_op(op), l, r,
                           "expr_cond_icmp");
    }
  }

  return to_bool(cg, gen_expr(cg, scopes, depth, e));
}

static const char *ny_ownership_helper_name(expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT || !e->as.call.callee->as.ident.name ||
      e->as.call.args.len != 1)
    return NULL;
  const char *n = e->as.call.callee->as.ident.name;
  const char *dot = strrchr(n, '.');
  n = dot ? dot + 1 : n;
  if (strcmp(n, "borrow") == 0 || strcmp(n, "own") == 0 || strcmp(n, "release") == 0 ||
      strcmp(n, "forget") == 0)
    return n;
  return NULL;
}

static LLVMValueRef gen_ownership_helper_expr(codegen_t *cg, scope *scopes, size_t depth,
                                              expr_t *e, const char *name) {
  expr_t *arg = e->as.call.args.data[0].val;
  if (strcmp(name, "borrow") == 0 || strcmp(name, "own") == 0)
    return gen_expr(cg, scopes, depth, arg);
  if (strcmp(name, "release") == 0) {
    LLVMValueRef v = gen_expr(cg, scopes, depth, arg);
    fun_sig *drop_sig = lookup_fun(cg, "__drop_owned", 0);
    if (drop_sig)
      LLVMBuildCall2(cg->builder, drop_sig->type, drop_sig->value, (LLVMValueRef[]){v}, 1,
                     "own.release");
    return ny_c0(cg);
  }
  return ny_c0(cg);
}

static bool ny_adt_type_is_param(enum_def_t *owner, const char *type_name) {
  if (!owner || !type_name)
    return false;
  for (size_t i = 0; i < owner->type_params.len; i++) {
    if (owner->type_params.data[i] && strcmp(owner->type_params.data[i], type_name) == 0)
      return true;
  }
  return false;
}

static LLVMValueRef gen_adt_constructor_expr(codegen_t *cg, scope *scopes, size_t depth,
                                             expr_t *e) {
  char *name = ny_adt_member_call_full_name(cg, e);
  if (!name)
    return NULL;
  enum_def_t *owner = NULL;
  enum_member_def_t *mem = lookup_enum_member_owner(cg, name, &owner);
  if (!mem || !owner || !mem->has_payload)
    return NULL;

  call_arg_t *args = NULL;
  size_t arg_count = 0;
  ny_expr_call_args(e, &args, &arg_count);
  size_t nfields = mem->fields.len ? mem->fields.len : 1;
  bool *seen_fields = alloca(sizeof(bool) * nfields);
  expr_t **field_exprs = alloca(sizeof(expr_t *) * nfields);
  memset(seen_fields, 0, sizeof(bool) * nfields);
  memset(field_exprs, 0, sizeof(expr_t *) * nfields);

  for (size_t i = 0; i < arg_count; i++) {
    call_arg_t *arg = &args[i];
    ssize_t idx = -1;
    const char *field_name = NULL;
    if (arg->name) {
      idx = ny_enum_member_field_index(mem, arg->name);
      field_name = arg->name;
      if (idx < 0) {
        return expr_fail(cg, e->tok, "unknown field '%s' for ADT variant '%s.%s'", arg->name,
                         owner->name, mem->name);
      }
    } else {
      if (i >= mem->fields.len) {
        return expr_fail(cg, e->tok, "too many positional fields for ADT variant '%s.%s'",
                         owner->name, mem->name);
      }
      idx = (ssize_t)i;
      field_name = mem->fields.data[idx].name;
    }
    if (seen_fields[idx]) {
      return expr_fail(cg, e->tok, "duplicate field '%s' for ADT variant '%s.%s'", field_name,
                       owner->name, mem->name);
    }
    seen_fields[idx] = true;
    field_exprs[idx] = arg->val;
  }

  for (size_t i = 0; i < mem->fields.len; i++) {
    if (!seen_fields[i]) {
      return expr_fail(cg, e->tok, "missing field '%s' for ADT variant '%s.%s'",
                       mem->fields.data[i].name, owner->name, mem->name);
    }
  }

  fun_sig *malloc_sig = lookup_fun(cg, "__malloc", 0);
  if (!malloc_sig)
    return expr_fail(cg, e->tok, "__malloc required for ADT constructor");

  size_t bytes = mem->fields.len * sizeof(int64_t);
  LLVMValueRef p =
      LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                     (LLVMValueRef[]){ny_ci(cg, ((uint64_t)bytes << 1) | 1u)}, 1,
                     NY_LLVM_NAME(cg, "adt_alloc"));
  LLVMValueRef tag_addr = LLVMBuildIntToPtr(cg->builder, ny_sub(cg, p, ny_ci(cg, 8), ""),
                                            ny_ptr_i64_ty(cg), "");
  ny_store(cg, tag_addr, ny_ci(cg, mem->runtime_tag));
  LLVMValueRef size_addr = LLVMBuildIntToPtr(cg->builder, ny_sub(cg, p, ny_ci(cg, 16), ""),
                                             ny_ptr_i64_ty(cg), "");
  ny_store(cg, size_addr, ny_ci(cg, ((uint64_t)bytes << 1) | 1u));

  for (size_t i = 0; i < mem->fields.len; i++) {
    enum_field_def_t *field = &mem->fields.data[i];
    if (field->type_name && strcmp(field->type_name, "any") != 0 &&
        !ny_adt_type_is_param(owner, field->type_name))
      ensure_expr_type_compatible(cg, scopes, depth, field->type_name, field_exprs[i],
                                  field_exprs[i] ? field_exprs[i]->tok : e->tok,
                                  "ADT constructor");
    LLVMValueRef val = gen_expr(cg, scopes, depth, field_exprs[i]);
    LLVMValueRef slot_addr =
        LLVMBuildIntToPtr(cg->builder, ny_add(cg, p, ny_ci(cg, (uint64_t)i * 8), ""),
                          ny_ptr_i64_ty(cg), "");
    ny_store(cg, slot_addr, val);
  }
  return p;
}

static void ny_emit_f64_div_zero_guard(codegen_t *cg, LLVMValueRef rf) {
  fun_sig *panic_sig = lookup_fun(cg, "__panic", 0);
  if (!panic_sig)
    return;
  LLVMValueRef zero = LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), 0.0);
  LLVMValueRef is_zero = LLVMBuildFCmp(cg->builder, LLVMRealOEQ, rf, zero, "fdiv_zero");
  LLVMBasicBlockRef cur = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur);
  LLVMBasicBlockRef panic_bb = ny_bb_fn(fn, "fdiv.zero.panic");
  LLVMBasicBlockRef ok_bb = ny_bb_fn(fn, "fdiv.zero.ok");
  ny_cond_br(cg, is_zero, panic_bb, ok_bb);

  ny_pos(cg, panic_bb);
  const char *msg = "division by zero";
  LLVMValueRef msg_global = const_string_ptr(cg, msg, strlen(msg));
  LLVMValueRef msg_ptr = ny_load(cg, msg_global, "fdiv_zero_msg");
  LLVMBuildCall2(cg->builder, panic_sig->type, panic_sig->value,
                 (LLVMValueRef[]){msg_ptr}, 1, "");
  LLVMBuildUnreachable(cg->builder);

  ny_pos(cg, ok_bb);
}

static bool ny_f64_expr_proven_nonzero(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL) {
    if (e->as.literal.kind == NY_LIT_INT)
      return e->as.literal.as.i != 0;
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return e->as.literal.as.f != 0.0;
  }
  int64_t min_raw = 0, max_raw = 0;
  if (expr_int_range(cg, scopes, depth, e, &min_raw, &max_raw))
    return min_raw > 0 || max_raw < 0;
  return false;
}

static LLVMValueRef ny_emit_f64_op(codegen_t *cg, const char *op,
                                   LLVMValueRef lf, LLVMValueRef rf,
                                   bool divisor_nonzero) {
  if (strcmp(op, "+") == 0)
    return LLVMBuildFAdd(cg->builder, lf, rf, "fadd");
  if (strcmp(op, "-") == 0)
    return LLVMBuildFSub(cg->builder, lf, rf, "fsub");
  if (strcmp(op, "*") == 0)
    return LLVMBuildFMul(cg->builder, lf, rf, "fmul");
  if (strcmp(op, "/") == 0) {
    if (!divisor_nonzero)
      ny_emit_f64_div_zero_guard(cg, rf);
    return LLVMBuildFDiv(cg->builder, lf, rf, "fdiv");
  }
  return NULL;
}

static bool ny_f64_raw_int_expr_shape_supported(expr_t *e, unsigned budget) {
  if (!e || budget == 0)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_INT;
  case NY_E_IDENT:
    return true;
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    if (!op)
      return false;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
        strcmp(op, "*") == 0 || strcmp(op, "&") == 0) {
      return ny_f64_raw_int_expr_shape_supported(e->as.binary.left,
                                                 budget - 1) &&
             ny_f64_raw_int_expr_shape_supported(e->as.binary.right,
                                                 budget - 1);
    }
    if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0 ||
        strcmp(op, ">>") == 0) {
      int64_t rhs_lit = 0;
      if (!ny_expr_literal_i64(e->as.binary.right, &rhs_lit))
        return false;
      if ((strcmp(op, "/") == 0 || strcmp(op, "%") == 0) && rhs_lit <= 0)
        return false;
      if (strcmp(op, ">>") == 0 && (rhs_lit < 0 || rhs_lit >= 64))
        return false;
      return ny_f64_raw_int_expr_shape_supported(e->as.binary.left,
                                                 budget - 1);
    }
    return false;
  }
  default:
    return false;
  }
}

static bool ny_f64_inline_try_raw_int_value(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *e,
                                            LLVMValueRef *raw_out,
                                            int64_t *min_out,
                                            int64_t *max_out) {
  if (raw_out)
    *raw_out = NULL;
  if (!cg || !e || !ny_f64_raw_int_expr_shape_supported(e, 24))
    return false;
  int64_t min_raw = 0, max_raw = 0;
  if (!expr_int_range(cg, scopes, depth, e, &min_raw, &max_raw))
    return false;
  LLVMValueRef raw = NULL;
  LLVMValueRef ok = NULL;
  if (!ny_build_mono_raw_int_expr(cg, scopes, depth, e, &raw, &ok) || !raw ||
      !ok || !LLVMIsAConstantInt(ok) || LLVMConstIntGetZExtValue(ok) == 0)
    return false;
  if (raw_out)
    *raw_out = raw;
  if (min_out)
    *min_out = min_raw;
  if (max_out)
    *max_out = max_raw;
  return true;
}

static LLVMValueRef ny_try_raw_int_expr_as_f64(codegen_t *cg, scope *scopes,
                                               size_t depth, expr_t *e) {
  if (!cg || !e || ny_env_enabled("NYTRIX_DISABLE_F64_RAW_INT_EXPR"))
    return NULL;
  if (!ny_f64_raw_int_expr_shape_supported(e, 24))
    return NULL;
  int64_t min_raw = 0, max_raw = 0;
  if (!expr_int_range(cg, scopes, depth, e, &min_raw, &max_raw))
    return NULL;
  LLVMValueRef raw = NULL;
  LLVMValueRef ok = NULL;
  if (!ny_build_mono_raw_int_expr(cg, scopes, depth, e, &raw, &ok) || !raw ||
      !ok || !LLVMIsAConstantInt(ok) || LLVMConstIntGetZExtValue(ok) == 0)
    return NULL;
  (void)min_raw;
  (void)max_raw;
  return LLVMBuildSIToFP(cg->builder, raw, cg->type_f64,
                         NY_LLVM_NAME(cg, "raw_int_i2f"));
}

static bool ny_f64_inline_body_supported(stmt_t *body) {
  if (!body)
    return false;
  if (body->kind == NY_S_RETURN || body->kind == NY_S_EXPR)
    return true;
  if (body->kind != NY_S_BLOCK || body->as.block.body.len == 0)
    return false;
  for (size_t i = 0; i < body->as.block.body.len; ++i) {
    stmt_t *s = body->as.block.body.data[i];
    if (i + 1 == body->as.block.body.len)
      return ny_f64_inline_body_supported(s);
    if (!s || s->kind != NY_S_VAR || s->as.var.is_mut ||
        s->as.var.is_del || s->as.var.is_destructure ||
        s->as.var.names.len != s->as.var.exprs.len)
      return false;
  }
  return false;
}

static void ny_f64_inline_bind_raw_int(codegen_t *cg, scope *scopes,
                                       size_t depth, const char *name,
                                       LLVMValueRef raw, const char *explicit_type,
                                       int64_t min_raw, int64_t max_raw) {
  if (!cg || !name || !raw)
    return;
  LLVMValueRef tagged = ny_tag_int(cg, raw);
  scope_bind(cg, scopes, depth, name, tagged, NULL, false,
             explicit_type ? explicit_type : "int", false);
  binding *b = &scopes[depth].vars.data[scopes[depth].vars.len - 1];
  b->is_int_direct = true;
  b->is_int_raw_direct = true;
  b->raw_int_value = raw;
  b->has_int_range = true;
  b->int_min_raw = min_raw;
  b->int_max_raw = max_raw;
}

static void ny_f64_inline_bind_value(codegen_t *cg, scope *scopes,
                                     size_t depth, const char *name,
                                     LLVMValueRef value, const char *explicit_type,
                                     expr_t *source_expr) {
  if (!name || !value)
    return;
  const char *inferred_type =
      explicit_type ? explicit_type : infer_expr_type(cg, scopes, depth, source_expr);
  if (LLVMTypeOf(value) == cg->type_f64 || LLVMTypeOf(value) == cg->type_f32) {
    const char *bind_type =
        explicit_type ? explicit_type
                      : (LLVMTypeOf(value) == cg->type_f32 ? "f32" : "f64");
    scope_bind(cg, scopes, depth, name, value, NULL, false, bind_type, false);
    binding *b = &scopes[depth].vars.data[scopes[depth].vars.len - 1];
    if (LLVMTypeOf(value) == cg->type_f32)
      b->is_f32_direct = true;
    else
      b->is_f64_direct = true;
    return;
  }
  value = expr_cast_to_i64(cg, value, "inline_f64_value");
  bool proven_int = ny_type_is(inferred_type, "int") ||
                    ny_type_is(inferred_type, "i64") ||
                    ny_is_proven_int(cg, scopes, depth, source_expr, value);
  bool proven_f64 = ny_type_is(inferred_type, "f64") ||
                    ny_type_is(inferred_type, "f32") ||
                    ny_type_is(inferred_type, "float") ||
                    ny_is_f64_like(cg, scopes, depth, source_expr);
  int64_t min_raw = 0, max_raw = 0;
  bool has_int_range =
      proven_int && expr_int_range(cg, scopes, depth, source_expr, &min_raw,
                                   &max_raw);
  const char *bind_type =
      explicit_type ? explicit_type : (proven_int ? "int" : (proven_f64 ? "f64" : NULL));
  scope_bind(cg, scopes, depth, name, value, NULL, false, bind_type, false);
  binding *b = &scopes[depth].vars.data[scopes[depth].vars.len - 1];
  if (proven_int) {
    b->is_int_direct = true;
    b->is_int_raw_direct = true;
    b->raw_int_value = ny_untag_int(cg, value);
    if (has_int_range) {
      b->has_int_range = true;
      b->int_min_raw = min_raw;
      b->int_max_raw = max_raw;
    }
  } else if (proven_f64) {
    b->is_f64_direct = true;
  }
}

static LLVMValueRef ny_emit_inline_f64_body(codegen_t *cg, scope *scopes,
                                            size_t depth, stmt_t *body) {
  if (!body)
    return NULL;
  if (body->kind == NY_S_RETURN)
    return gen_expr_as_f64(cg, scopes, depth, body->as.ret.value);
  if (body->kind == NY_S_EXPR)
    return gen_expr_as_f64(cg, scopes, depth, body->as.expr.expr);
  if (body->kind != NY_S_BLOCK)
    return NULL;
  for (size_t i = 0; i < body->as.block.body.len; ++i) {
    stmt_t *s = body->as.block.body.data[i];
    if (i + 1 == body->as.block.body.len)
      return ny_emit_inline_f64_body(cg, scopes, depth, s);
    if (!s || s->kind != NY_S_VAR)
      return NULL;
    stmt_var_t *var = &s->as.var;
    for (size_t k = 0; k < var->names.len; ++k) {
      expr_t *init = var->exprs.data[k];
      const char *decl_type = k < var->types.len ? var->types.data[k] : NULL;
      LLVMValueRef raw = NULL;
      int64_t min_raw = 0, max_raw = 0;
      if (ny_f64_inline_try_raw_int_value(cg, scopes, depth, init, &raw,
                                          &min_raw, &max_raw)) {
        ny_f64_inline_bind_raw_int(cg, scopes, depth, var->names.data[k], raw,
                                   decl_type, min_raw, max_raw);
        continue;
      }
      LLVMValueRef v = gen_expr(cg, scopes, depth, init);
      ny_f64_inline_bind_value(cg, scopes, depth, var->names.data[k], v,
                               decl_type, init);
    }
  }
  return NULL;
}

static LLVMValueRef ny_try_inline_call_as_f64(codegen_t *cg, scope *scopes,
                                              size_t depth, expr_t *call_expr,
                                              fun_sig *sig) {
  if (!cg || !scopes || !call_expr || call_expr->kind != NY_E_CALL || !sig ||
      sig->is_variadic || sig->is_extern || sig->is_recursive || !sig->stmt_t ||
      sig->stmt_t->kind != NY_S_FUNC || !sig->stmt_t->as.fn.body)
    return NULL;
  if (!ny_codegen_speed_profile_enabled(cg) &&
      !ny_env_enabled("NYTRIX_INLINE_F64_HELPERS"))
    return NULL;
  if (ny_is_stdlib_tok(sig->stmt_t->tok))
    return NULL;
  expr_call_t *call = &call_expr->as.call;
  ny_param_list *params = &sig->stmt_t->as.fn.params;
  if (call->args.len != params->len || params->len > 8)
    return NULL;
  for (size_t i = 0; i < call->args.len; ++i)
    if (call->args.data[i].name)
      return NULL;
  if (!ny_f64_inline_body_supported(sig->stmt_t->as.fn.body))
    return NULL;

  LLVMValueRef arg_values[8] = {0};
  const char *arg_types[8] = {0};
  bool arg_is_int[8] = {0};
  bool arg_is_f64[8] = {0};
  bool arg_has_range[8] = {0};
  LLVMValueRef arg_raw_values[8] = {0};
  int64_t arg_min_raw[8] = {0};
  int64_t arg_max_raw[8] = {0};
  for (size_t i = 0; i < call->args.len; ++i) {
    expr_t *arg = call->args.data[i].val;
    if (!arg)
      return NULL;
    const char *arg_type = infer_expr_type(cg, scopes, depth, arg);
    bool proven_int = ny_type_is(arg_type, "int") ||
                      ny_type_is(arg_type, "i64") ||
                      ny_is_proven_int(cg, scopes, depth, arg, NULL);
    bool proven_f64 = ny_type_is(arg_type, "f64") ||
                      ny_type_is(arg_type, "f32") ||
                      ny_type_is(arg_type, "float") ||
                      ny_is_f64_like(cg, scopes, depth, arg);
    LLVMValueRef raw = NULL;
    bool raw_arg = proven_int &&
                   ny_f64_inline_try_raw_int_value(cg, scopes, depth, arg,
                                                   &raw, &arg_min_raw[i],
                                                   &arg_max_raw[i]);
    LLVMValueRef v = raw_arg ? ny_tag_int(cg, raw)
                             : ((proven_f64 && !proven_int)
                                    ? gen_expr_as_f64(cg, scopes, depth, arg)
                                    : gen_expr(cg, scopes, depth, arg));
    if (!v)
      return NULL;
    if (!(proven_f64 && !proven_int))
      v = expr_cast_to_i64(cg, v, "inline_f64_arg");
    arg_values[i] = v;
    arg_raw_values[i] = raw_arg ? raw : NULL;
    arg_is_int[i] = proven_int;
    arg_is_f64[i] = proven_f64 && !proven_int;
    arg_has_range[i] =
        raw_arg ||
        (proven_int &&
         expr_int_range(cg, scopes, depth, arg, &arg_min_raw[i],
                        &arg_max_raw[i]));
    arg_types[i] = proven_int ? "int" : (proven_f64 ? "f64" : NULL);
  }

  size_t inline_depth = depth + 1;
  memset(&scopes[inline_depth], 0, sizeof(scopes[inline_depth]));
  for (size_t i = 0; i < params->len; ++i) {
    param_t *p = &params->data[i];
    if (!p->name || !arg_values[i])
      continue;
    const char *ptype = p->type ? p->type : arg_types[i];
    scope_bind(cg, scopes, inline_depth, p->name, arg_values[i], NULL, false,
               ptype, false);
    binding *b =
        &scopes[inline_depth].vars.data[scopes[inline_depth].vars.len - 1];
    if (arg_is_int[i]) {
      b->is_int_direct = true;
      b->is_int_raw_direct = true;
      b->raw_int_value = arg_raw_values[i] ? arg_raw_values[i]
                                           : ny_untag_int(cg, arg_values[i]);
      if (arg_has_range[i]) {
        b->has_int_range = true;
        b->int_min_raw = arg_min_raw[i];
        b->int_max_raw = arg_max_raw[i];
      }
    } else if (arg_is_f64[i]) {
      b->is_f64_direct = true;
    }
  }

  LLVMValueRef out =
      ny_emit_inline_f64_body(cg, scopes, inline_depth, sig->stmt_t->as.fn.body);
  scope_pop(scopes, &inline_depth);
  return out;
}

LLVMValueRef gen_expr_as_f64(codegen_t *cg, scope *scopes, size_t depth,
                             expr_t *e) {
  if (!e)
    return LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), 0.0);
  LLVMTypeRef f64_ty = LLVMDoubleTypeInContext(cg->ctx);

  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_FLOAT) {
      double d = e->as.literal.as.f;
      if (e->as.literal.hint == NY_LIT_HINT_F32)
        d = (double)(float)d;
      return LLVMConstReal(f64_ty, d);
    }
    if (e->as.literal.kind == NY_LIT_INT) {
      return LLVMConstReal(f64_ty, (double)e->as.literal.as.i);
    }
    break;
  case NY_E_IDENT: {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                                     e->as.ident.hash);
    if (b && (b->is_f64_slot || b->is_f64_direct)) {
      b->is_used = true;
      return b->is_slot
                 ? LLVMBuildLoad2(cg->builder, cg->type_f64, b->value, "f64_ld")
                 : b->value;
    }
    if (b && (b->is_f32_slot || b->is_f32_direct)) {
      b->is_used = true;
      LLVMValueRef f32_val =
          b->is_slot
              ? LLVMBuildLoad2(cg->builder, cg->type_f32, b->value, "f32_ld")
              : b->value;
      return LLVMBuildFPExt(cg->builder, f32_val, cg->type_f64, "f2f");
    }

    if (b && b->type_name && strcmp(b->type_name, "int") == 0) {
      b->is_used = true;
      LLVMValueRef raw =
          (!b->is_slot && b->is_int_raw_direct && b->raw_int_value)
              ? b->raw_int_value
              : ny_untag_int(cg, b->is_slot ? ny_load(cg, b->value, "") : b->value);
      return LLVMBuildSIToFP(cg->builder, raw, f64_ty, "i2f");
    }
    break;
  }
  case NY_E_BINARY: {
    LLVMValueRef raw_int_f64 = ny_try_raw_int_expr_as_f64(cg, scopes, depth, e);
    if (raw_int_f64)
      return raw_int_f64;
    const char *op = e->as.binary.op;
    if (!ny_is_f64_arith_op(op))
      break;
    if (!ny_is_numeric_expr_like(cg, scopes, depth, e->as.binary.left) ||
        !ny_is_numeric_expr_like(cg, scopes, depth, e->as.binary.right))
      break;

    LLVMValueRef lf = gen_expr_as_f64(cg, scopes, depth, e->as.binary.left);
    LLVMValueRef rf = gen_expr_as_f64(cg, scopes, depth, e->as.binary.right);
    bool divisor_nonzero =
        strcmp(op, "/") == 0 &&
        ny_f64_expr_proven_nonzero(cg, scopes, depth, e->as.binary.right);
    return ny_emit_f64_op(cg, op, lf, rf, divisor_nonzero);
  }
  case NY_E_UNARY:
    if (e->as.unary.op && e->as.unary.right) {
      if (strcmp(e->as.unary.op, "+") == 0)
        return gen_expr_as_f64(cg, scopes, depth, e->as.unary.right);
      if (strcmp(e->as.unary.op, "-") == 0)
        return LLVMBuildFNeg(cg->builder,
                             gen_expr_as_f64(cg, scopes, depth, e->as.unary.right),
                             "fneg");
    }
    break;
  case NY_E_TERNARY: {
    LLVMValueRef cond = expr_gen_cond_i1(cg, scopes, depth, e->as.ternary.cond);
    LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
    LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
    LLVMBasicBlockRef true_bb = ny_bb_fn(fn, "tern_f64_true");
    LLVMBasicBlockRef false_bb = ny_bb_fn(fn, "tern_f64_false");
    LLVMBasicBlockRef end_bb = ny_bb_fn(fn, "tern_f64_end");
    ny_cond_br(cg, cond, true_bb, false_bb);

    ny_pos(cg, true_bb);
    LLVMValueRef true_val =
        gen_expr_as_f64(cg, scopes, depth, e->as.ternary.true_expr);
    ny_br(cg, end_bb);
    LLVMBasicBlockRef true_end_bb = ny_cur_block(cg);

    ny_pos(cg, false_bb);
    LLVMValueRef false_val =
        gen_expr_as_f64(cg, scopes, depth, e->as.ternary.false_expr);
    ny_br(cg, end_bb);
    LLVMBasicBlockRef false_end_bb = ny_cur_block(cg);

    ny_pos(cg, end_bb);
    LLVMValueRef phi = ny_phi(cg, cg->type_f64, "tern_f64");
    LLVMAddIncoming(phi, (LLVMValueRef[]){true_val, false_val},
                    (LLVMBasicBlockRef[]){true_end_bb, false_end_bb}, 2);
    return phi;
  }
  case NY_E_CALL: {
    LLVMValueRef f64buf_load =
        ny_try_fast_f64buf_load_as_f64(cg, scopes, depth, e);
    if (f64buf_load)
      return f64buf_load;
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      size_t surface_len = 0;
      uint64_t surface_hash = 0;
      const char *surface = ny_builtin_surface_name_for_callee(
          e->as.call.callee, &surface_len, &surface_hash);
      bool builtin_shadowed =
          !surface || ny_builtin_name_shadowed_by_user_symbol(
                          cg, scopes, depth, surface, surface_len, surface_hash);
      const char *fn_name = e->as.call.callee->as.ident.name;
      if (fn_name) {
        const char *leaf = strrchr(fn_name, '.');
        leaf = leaf ? leaf + 1 : fn_name;
        const char *surface_leaf = surface ? strrchr(surface, '.') : NULL;
        surface_leaf = surface_leaf ? surface_leaf + 1 : surface;
        if (!builtin_shadowed && e->as.call.args.len == 1 &&
            ((surface_leaf && (strcmp(surface_leaf, "float") == 0 ||
                               strcmp(surface_leaf, "to_float") == 0 ||
                               strcmp(surface_leaf, "f64") == 0)) ||
             strcmp(leaf, "std.core.float") == 0 ||
             strcmp(leaf, "std.core.to_float") == 0 ||
             strcmp(leaf, "std.core.f64") == 0 ||
             strcmp(leaf, "float") == 0 || strcmp(leaf, "to_float") == 0 ||
             strcmp(leaf, "f64") == 0)) {
          expr_t *arg = e->as.call.args.data[0].val;
          if (!arg)
            return LLVMConstReal(f64_ty, 0.0);
          if (ny_is_proven_int(cg, scopes, depth, arg, NULL)) {
            LLVMValueRef tagged = gen_expr(cg, scopes, depth, arg);
            LLVMValueRef raw = ny_untag_int(cg, tagged);
            return LLVMBuildSIToFP(cg->builder, raw, f64_ty, "i2f");
          }
          return gen_expr_as_f64(cg, scopes, depth, arg);
        }
        fun_sig *sig = resolve_overload(cg, fn_name, e->as.call.args.len, 0);
        const char *ret_type =
            sig ? (sig->return_type ? sig->return_type
                                    : sig->inferred_return_type)
                : NULL;
        if (!ret_type)
          ret_type = infer_expr_type(cg, scopes, depth, e);
        if (sig && ret_type &&
            (strcmp(ret_type, "f64") == 0 || strcmp(ret_type, "f32") == 0 ||
             strcmp(ret_type, "float") == 0)) {
          LLVMValueRef inlined =
              ny_try_inline_call_as_f64(cg, scopes, depth, e, sig);
          if (inlined)
            return inlined;
          LLVMValueRef native = ny_try_native_call_as_f64(cg, scopes, depth, e);
          if (native)
            return native;
          LLVMValueRef call_result = gen_call_expr(cg, scopes, depth, e);
          return ny_unbox_float(cg, call_result);
        }
        if (sig && ret_type && strcmp(ret_type, "int") == 0) {
          LLVMValueRef call_result = gen_call_expr(cg, scopes, depth, e);
          return LLVMBuildSIToFP(cg->builder, ny_untag_int(cg, call_result),
                                 f64_ty, "i2f.call");
        }
      }
    }
    break;
  }
  case NY_E_MEMCALL: {
    LLVMValueRef f64_get = ny_try_emit_f64_list_get_as_f64(cg, scopes, depth, e);
    if (f64_get)
      return f64_get;
    break;
  }
  case NY_E_INDEX: {
    const char *t = infer_expr_type(cg, scopes, depth, e);
    if (t && (strcmp(t, "f64") == 0 || strcmp(t, "f32") == 0 ||
              strcmp(t, "float") == 0)) {
      return ny_unbox_known_numeric_float(cg, gen_expr(cg, scopes, depth, e));
    }
    break;
  }
  default:
    break;
  }

  LLVMValueRef v = gen_expr(cg, scopes, depth, e);

  if (e->kind == NY_E_IDENT) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                                     e->as.ident.hash);
    if (b && (b->is_f64_slot || b->is_f64_direct)) {
      LLVMValueRef ptr =
          LLVMBuildIntToPtr(cg->builder, v, LLVMPointerType(f64_ty, 0), "");
      return LLVMBuildLoad2(cg->builder, f64_ty, ptr, "f64_load");
    }
  }

  return ny_unbox_float(cg, v);
}

static LLVMValueRef ny_try_emit_mono_raw_int_expr(codegen_t *cg, scope *scopes, size_t depth,
                                                  expr_t *e) {
  if (!cg || !e || cg->mono_raw_expr_disabled)
    return NULL;
  bool mono_mode = cg->mono_emitting;
  bool proven_fast =
      !mono_mode && ny_fast_path_enabled(cg, "NYTRIX_PROVEN_RAW_INT_EXPR_FAST") &&
      ny_is_proven_int(cg, scopes, depth, e, NULL);
  if (!mono_mode && !proven_fast)
    return NULL;
  if (!mono_mode && e->kind == NY_E_BINARY && e->as.binary.op &&
      (strcmp(e->as.binary.op, "+") == 0 ||
       strcmp(e->as.binary.op, "-") == 0) &&
      ((e->as.binary.left && e->as.binary.left->kind == NY_E_LITERAL &&
        e->as.binary.left->as.literal.kind == NY_LIT_INT) ||
       (e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL &&
        e->as.binary.right->as.literal.kind == NY_LIT_INT))) {
    return NULL;
  }
  if (!mono_mode && e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "%") == 0 && e->as.binary.right &&
      e->as.binary.right->kind == NY_E_LITERAL &&
      e->as.binary.right->as.literal.kind == NY_LIT_INT &&
      e->as.binary.right->as.literal.as.i > 0) {
    return NULL;
  }
  LLVMValueRef raw = NULL;
  LLVMValueRef ok = NULL;
  if (!ny_build_mono_raw_int_expr(cg, scopes, depth, e, &raw, &ok) || !raw || !ok)
    return NULL;

  if (!mono_mode) {
    if (!LLVMIsAConstantInt(ok) || LLVMConstIntGetZExtValue(ok) == 0)
      return NULL;
    return ny_tag_int(cg, raw);
  }

  LLVMValueRef cur_fn = ny_cur_fn(cg);
  if (!cur_fn)
    return NULL;
  LLVMBasicBlockRef fast_bb = ny_bb_fn(cur_fn, "mono.raw.fast");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(cur_fn, "mono.raw.slow");
  LLVMBasicBlockRef done_bb = ny_bb_fn(cur_fn, "mono.raw.done");
  ny_cond_br(cg, ok, fast_bb, slow_bb);

  ny_pos(cg, fast_bb);
  LLVMValueRef fast_value = ny_tag_int(cg, raw);
  LLVMBasicBlockRef fast_end = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, slow_bb);
  bool old_disabled = cg->mono_raw_expr_disabled;
  cg->mono_raw_expr_disabled = true;
  LLVMValueRef slow_value = gen_expr(cg, scopes, depth, e);
  cg->mono_raw_expr_disabled = old_disabled;
  LLVMBasicBlockRef slow_end = ny_cur_block(cg);
  if (!ny_has_terminator(cg))
    ny_br(cg, done_bb);

  ny_pos(cg, done_bb);
  LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "mono_raw_int"));
  LLVMAddIncoming(phi, (LLVMValueRef[]){fast_value, slow_value},
                  (LLVMBasicBlockRef[]){fast_end, slow_end}, 2);
  return phi;
}

static size_t ny_int_literal_value_len(token_t tok) {
  size_t len = (size_t)tok.len;
  const char *s = tok.lexeme;
  if (!s || len < 2)
    return len;
  size_t i = len;
  while (i > 0 && isdigit((unsigned char)s[i - 1]))
    i--;
  if (i == 0 || i == len)
    return len;
  char c = s[i - 1];
  if (c != 'i' && c != 'I' && c != 'u' && c != 'U')
    return len;
  size_t suffix_start = i - 1;
  if (suffix_start > 0 && s[suffix_start - 1] == '_')
    return suffix_start - 1;
  return suffix_start;
}

static int ny_int_literal_digit_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (c - 'A');
  return -1;
}

static char *ny_dec_mul_add_small(const char *dec, unsigned base, unsigned digit) {
  const char *src = (dec && *dec) ? dec : "0";
  size_t len = strlen(src);
  size_t cap = len + 16;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  size_t pos = cap - 1;
  buf[pos] = '\0';
  unsigned carry = digit;
  for (ssize_t i = (ssize_t)len - 1; i >= 0; --i) {
    char c = src[i];
    if (c < '0' || c > '9') {
      free(buf);
      return NULL;
    }
    unsigned v = (unsigned)(c - '0') * base + carry;
    if (pos == 0) {
      free(buf);
      return NULL;
    }
    buf[--pos] = (char)('0' + (v % 10u));
    carry = v / 10u;
  }
  while (carry > 0) {
    if (pos == 0) {
      free(buf);
      return NULL;
    }
    buf[--pos] = (char)('0' + (carry % 10u));
    carry /= 10u;
  }
  while (buf[pos] == '0' && buf[pos + 1] != '\0')
    pos++;
  char *out = ny_strdup(buf + pos);
  free(buf);
  return out;
}

static char *ny_int_literal_decimal_from_token(token_t tok, int64_t fallback) {
  char fallback_buf[64];
  snprintf(fallback_buf, sizeof(fallback_buf), "%" PRId64, fallback);
  if (!tok.lexeme || tok.len <= 0)
    return ny_strdup(fallback_buf);

  size_t len = ny_int_literal_value_len(tok);
  const char *s = tok.lexeme;
  unsigned base = 10;
  size_t i = 0;
  if (len > 2 && s[0] == '0') {
    if (s[1] == 'x' || s[1] == 'X') {
      base = 16;
      i = 2;
    } else if (s[1] == 'b' || s[1] == 'B') {
      base = 2;
      i = 2;
    } else if (s[1] == 'o' || s[1] == 'O') {
      base = 8;
      i = 2;
    }
  }

  char *dec = ny_strdup("0");
  if (!dec)
    return NULL;
  bool saw_digit = false;
  for (; i < len; i++) {
    if (s[i] == '_')
      continue;
    int digit = ny_int_literal_digit_value(s[i]);
    if (digit < 0 || (unsigned)digit >= base) {
      free(dec);
      return ny_strdup(fallback_buf);
    }
    saw_digit = true;
    char *next = ny_dec_mul_add_small(dec, base, (unsigned)digit);
    free(dec);
    if (!next)
      return NULL;
    dec = next;
  }
  if (!saw_digit) {
    free(dec);
    return ny_strdup(fallback_buf);
  }
  return dec;
}

static LLVMValueRef gen_expr_inner(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {

  if (cg->builder) {
    LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
    if (cur_bb && LLVMGetBasicBlockTerminator(cur_bb)) {
      return LLVMGetUndef(cg->type_i64);
    }
  }
  if (!e || cg->had_error)
    return ny_c0(cg);

  ny_dbg_loc(cg, e->tok);

  switch (e->kind) {
  case NY_E_COMPTIME:
    return gen_comptime_eval(cg, e->as.comptime_expr.body);
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT) {
      int64_t raw = e->as.literal.as.i;
      if (ny_small_int_fits_i64(raw))
        return LLVMConstInt(cg->type_i64, ((uint64_t)raw << 1) | 1, true);
      char *dec = ny_int_literal_decimal_from_token(e->tok, raw);
      const char *lit = dec ? dec : "0";
      LLVMValueRef str_runtime_global = const_string_ptr(cg, lit, strlen(lit));
      free(dec);
      LLVMValueRef str_ptr = ny_load(cg, str_runtime_global, "big_lit_str");
      fun_sig *big_from_str = lookup_fun(cg, "__bigint_from_str", 0);
      if (!big_from_str)
        return expr_fail(cg, e->tok, "builtin __bigint_from_str missing");
      return LLVMBuildCall2(cg->builder, big_from_str->type,
                            big_from_str->value, (LLVMValueRef[]){str_ptr}, 1,
                            "big_lit");
    }
    if (e->as.literal.kind == NY_LIT_BOOL)
      return e->as.literal.as.b ? ny_ctrue(cg) : ny_cfalse(cg);
    if (e->as.literal.kind == NY_LIT_STR) {

      LLVMValueRef str_runtime_global =
          const_string_ptr(cg, e->as.literal.as.s.data, e->as.literal.as.s.len);

      return ny_load(cg, str_runtime_global, "str_ptr");
    }
    if (e->as.literal.kind == NY_LIT_FLOAT) {
      fun_sig *box_sig = ny_helper_flt_box(cg);
      if (!box_sig) {
        return expr_fail(cg, e->tok, "__flt_box_val not found");
      }
      double fval_d = e->as.literal.as.f;
      if (e->as.literal.hint == NY_LIT_HINT_F32) {
        float f32 = (float)fval_d;
        fval_d = (double)f32;
      }
      LLVMValueRef fval =
          LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), fval_d);
      return LLVMBuildCall2(
          cg->builder, box_sig->type, box_sig->value,
          (LLVMValueRef[]){ny_bitcast(cg, fval, cg->type_i64, "")}, 1, "");
    }
    return ny_c0(cg);
  case NY_E_IDENT: {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                                     e->as.ident.hash);
    if (b) {
      if (cg->comptime && b->stmt_t && !ny_is_stdlib_tok(b->stmt_t->tok) &&
          b->value && LLVMIsAGlobalVariable(b->value)) {
        ny_ct_fast_val_t folded = ny_ct_fast_none();
        if (ny_try_eval_binding_comptime_const(cg, b, e->as.ident.name,
                                               &folded, 0)) {
          LLVMValueRef v = ny_ct_fast_to_llvm_value(cg, &folded, e->tok);
          ny_ct_fast_val_free(&folded);
          if (v)
            return v;
        }
        ny_ct_fast_val_free(&folded);
        return expr_fail(cg, e->tok,
                         "comptime cannot capture runtime global '%s'",
                         e->as.ident.name);
      }
      return expr_value_from_binding(cg, b);
    }

    fun_sig *s = lookup_fun(cg, e->as.ident.name, e->as.ident.hash);
    if (s) {
      bool boxed_callable =
          !s->is_extern && ny_named_callable_values_need_closure(cg);
      LLVMValueRef sv =
          (boxed_callable || ny_fun_sig_needs_tagged_callable_adapter(s))
              ? ny_fun_sig_tagged_callable_adapter(cg, s, e->tok, boxed_callable)
              : s->value;
      if (boxed_callable) {
        LLVMValueRef raw = ny_ptr2i64(cg, sv, "");
        return ny_box_callable_closure(cg, raw, e->tok);
      }

      return ny_ptr2i64(cg, sv, "");
    }

    enum_member_def_t *emd = lookup_enum_member(cg, e->as.ident.name);
    if (emd) {
      return LLVMConstInt(cg->type_i64, ((uint64_t)emd->value << 1) | 1, true);
    }

    LLVMValueRef host_ident = ny_try_host_platform_ident(cg, e->as.ident.name);
    if (host_ident)
      return host_ident;

    report_undef_symbol(cg, e->as.ident.name, e->tok);
    return ny_c0(cg);
  }
  case NY_E_UNARY: {
    return gen_expr_unary(cg, scopes, depth, e);
  }
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    expr_t *le = e->as.binary.left, *re = e->as.binary.right;

    if (op && strcmp(op, "..") == 0)
      return gen_range_expr(cg, scopes, depth, e);

    bool is_user_eq = op && (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);
    bool in_std_module = cg->current_module_name &&
                         (strncmp(cg->current_module_name, "std.", 4) == 0 ||
                          strncmp(cg->current_module_name, "lib.", 4) == 0);
    if (is_user_eq && !in_std_module) {
      ny_ct_fast_val_t folded = {0};
      if (ny_try_eval_comptime_expr_fast(cg, e, &folded, 0) && folded.kind == NY_CT_FAST_BOOL) {
        bool folded_bool = folded.b;
        ny_ct_fast_val_free(&folded);
        return folded_bool ? ny_ctrue(cg) : ny_cfalse(cg);
      }
      ny_ct_fast_val_free(&folded);
      if (lookup_fun(cg, "std.core.eq", 0)) {
        expr_t callee = {0};
        callee.kind = NY_E_IDENT;
        callee.tok = e->tok;
        callee.as.ident.name = "std.core.eq";
        call_arg_t args[2] = {{.val = le}, {.val = re}};
        expr_t call = {0};
        call.kind = NY_E_CALL;
        call.tok = e->tok;
        call.as.call.callee = &callee;
        call.as.call.args.data = args;
        call.as.call.args.len = 2;
        call.as.call.args.cap = 2;
        LLVMValueRef eq_res = gen_call_expr(cg, scopes, depth, &call);
        if (strcmp(op, "!=") == 0)
          return ny_select(cg, to_bool(cg, eq_res), ny_cfalse(cg), ny_ctrue(cg),
                           "ne.user");
        if (LLVMTypeOf(eq_res) == ny_i1_ty(cg))
          return ny_select(cg, eq_res, ny_ctrue(cg), ny_cfalse(cg),
                           "eq.user.tagged");
        return eq_res;
      }
    }

    LLVMValueRef mono_raw_int = ny_try_emit_mono_raw_int_expr(cg, scopes, depth, e);
    if (mono_raw_int)
      return mono_raw_int;

    bool is_arith = ny_is_f64_arith_op(op);
    bool is_cmp = (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                   strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                   strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);

    if (is_arith || is_cmp) {
      if ((ny_is_f64_like(cg, scopes, depth, le) ||
           ny_is_f64_like(cg, scopes, depth, re)) &&
          ny_is_numeric_expr_like(cg, scopes, depth, le) &&
          ny_is_numeric_expr_like(cg, scopes, depth, re)) {
        ny_dbg_loc(cg, e->tok);
        LLVMValueRef lf = gen_expr_as_f64(cg, scopes, depth, le);
        LLVMValueRef rf = gen_expr_as_f64(cg, scopes, depth, re);

        if (is_arith) {
          bool divisor_nonzero =
              strcmp(op, "/") == 0 &&
              ny_f64_expr_proven_nonzero(cg, scopes, depth, re);
          LLVMValueRef res = ny_emit_f64_op(cg, op, lf, rf, divisor_nonzero);
          fun_sig *box_sig = lookup_fun(cg, "__flt_box_val", 0);
          if (box_sig) {
            return LLVMBuildCall2(
                cg->builder, box_sig->type, box_sig->value,
                (LLVMValueRef[]){ny_bitcast(cg, res, cg->type_i64, "")}, 1,
                "box");
          }
        } else {
          LLVMRealPredicate pred = LLVMRealOEQ;
          if (strcmp(op, "<") == 0)
            pred = LLVMRealOLT;
          else if (strcmp(op, "<=") == 0)
            pred = LLVMRealOLE;
          else if (strcmp(op, ">") == 0)
            pred = LLVMRealOGT;
          else if (strcmp(op, ">=") == 0)
            pred = LLVMRealOGE;
          else if (strcmp(op, "==") == 0)
            pred = LLVMRealOEQ;
          else if (strcmp(op, "!=") == 0)
            pred = LLVMRealUNE;

          LLVMValueRef cmp = LLVMBuildFCmp(cg->builder, pred, lf, rf, "fcmp");
          return ny_select(cg, cmp, ny_ctrue(cg), ny_cfalse(cg), "tag_bool");
        }
      }
    }

    LLVMValueRef l = gen_expr(cg, scopes, depth, le);
    LLVMValueRef r = gen_expr(cg, scopes, depth, re);
    ny_dbg_loc(cg, e->tok);
    return gen_binary(cg, scopes, depth, op, l, r, le, re);
  }
  case NY_E_CALL:
    {
      LLVMValueRef adt = gen_adt_constructor_expr(cg, scopes, depth, e);
      if (adt)
        return adt;
      const char *own_helper = ny_ownership_helper_name(e);
      if (own_helper)
        return gen_ownership_helper_expr(cg, scopes, depth, e, own_helper);
      if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
        const char *fn_name = e->as.call.callee->as.ident.name;
        fun_sig *sig = fn_name ? resolve_overload(cg, fn_name,
                                                  e->as.call.args.len, 0)
                               : NULL;
        const char *ret_type =
            sig ? (sig->return_type ? sig->return_type
                                    : sig->inferred_return_type)
                : NULL;
        if (!ret_type)
          ret_type = infer_expr_type(cg, scopes, depth, e);
        if (ret_type &&
            (strcmp(ret_type, "f64") == 0 || strcmp(ret_type, "f32") == 0 ||
             strcmp(ret_type, "float") == 0)) {
          LLVMValueRef f64v = ny_try_inline_call_as_f64(cg, scopes, depth, e, sig);
          if (f64v) {
            fun_sig *box_sig = ny_helper_flt_box(cg);
            if (!box_sig)
              return expr_fail(cg, e->tok, "__flt_box_val not found");
            return LLVMBuildCall2(
                cg->builder, box_sig->type, box_sig->value,
                (LLVMValueRef[]){ny_bitcast(cg, f64v, cg->type_i64, "")}, 1,
                "inline_f64_box");
          }
        }
      }
    }
    return gen_call_expr(cg, scopes, depth, e);
  case NY_E_MEMCALL:
    {
      LLVMValueRef adt = gen_adt_constructor_expr(cg, scopes, depth, e);
      if (adt)
        return adt;
    }
    return gen_call_expr(cg, scopes, depth, e);
  case NY_E_INDEX: {
    return gen_expr_index(cg, scopes, depth, e);
  }
  case NY_E_LIST:
  case NY_E_TUPLE: {
    return gen_expr_list_like(cg, scopes, depth, e);
  }
  case NY_E_DICT: {
    return gen_expr_dict(cg, scopes, depth, e);
  }
  case NY_E_SET: {
    return gen_expr_set(cg, scopes, depth, e);
  }
  case NY_E_PTR_TYPE:
    return ny_c0(cg);
  case NY_E_DEREF: {
    LLVMValueRef ptr = gen_expr(cg, scopes, depth, e->as.deref.target);

    ny_dbg_loc(cg, e->tok);
    LLVMValueRef raw_ptr = LLVMBuildIntToPtr(cg->builder, ptr, cg->type_i8ptr,
                                             NY_LLVM_NAME(cg, "raw_ptr"));
    return ny_load(cg, raw_ptr, NY_LLVM_NAME(cg, "deref"));
  }
  case NY_E_MEMBER: {

    char *full_name = codegen_full_name(cg, e, cg->arena);
    if (full_name) {
      enum_member_def_t *emd = lookup_enum_member(cg, full_name);
      if (emd) {
        return LLVMConstInt(cg->type_i64, ((uint64_t)emd->value << 1) | 1,
                            true);
      }
      if (!emd && cg->current_module_name && *cg->current_module_name) {
        size_t cur_len = strlen(cg->current_module_name);
        bool already_scoped =
            strncmp(full_name, cg->current_module_name, cur_len) == 0 &&
            full_name[cur_len] == '.';
        if (already_scoped)
          goto member_enum_scoped_done;
        size_t scoped_len = strlen(cg->current_module_name) + 1 +
                            strlen(full_name) + 1;
        char *scoped_name = arena_alloc(cg->arena, scoped_len);
        snprintf(scoped_name, scoped_len, "%s.%s", cg->current_module_name,
                 full_name);
        emd = lookup_enum_member(cg, scoped_name);
        if (emd) {
          return LLVMConstInt(cg->type_i64, ((uint64_t)emd->value << 1) | 1,
                              true);
        }
      }
    member_enum_scoped_done:;
      binding *gb = expr_lookup_binding(cg, NULL, 0, full_name, strlen(full_name),
                                        ny_hash64(full_name, strlen(full_name)));
      if (gb)
        return expr_value_from_binding(cg, gb);
    }
    char alias_full_name[512];
    if (e->as.member.target && e->as.member.name) {
      char module_path[1024];
      char resolved_fun[1280];
      if (ny_resolve_module_expr_path(cg, scopes, depth, e->as.member.target,
                                      module_path, sizeof(module_path)) &&
          ny_resolve_module_function_path(cg, module_path, e->as.member.name,
                                          resolved_fun,
                                          sizeof(resolved_fun))) {
        fun_sig *fs = lookup_fun(cg, resolved_fun, 0);
        if (fs)
          return ny_ptr2i64(cg, fs->value, "");
      }
    }
    binding *alias_gb = expr_member_module_alias_global(cg, scopes, depth, e, alias_full_name,
                                                        sizeof(alias_full_name));
    if (alias_gb) {
      alias_gb->is_used = true;
      return expr_value_from_binding(cg, alias_gb);
    }
    const char *alias_module = expr_member_module_alias_target(cg, scopes, depth, e);
    if (alias_module) {
      const char *alias_name = e->as.member.target->as.ident.name;
      ny_diag_error(e->tok, "module '%s' has no exported member '%s'",
                    alias_module, e->as.member.name);
      ny_diag_hint("alias '%s' resolves to module '%s'", alias_name, alias_module);
      ny_diag_fix("export '%s' from '%s' or import the module that defines it",
                  e->as.member.name, alias_module);
      cg->had_error = 1;
      return ny_c0(cg);
    }

    LLVMValueRef typed_property = ny_try_member_property_expr(cg, scopes, depth, e);
    if (typed_property)
      return typed_property;

    LLVMValueRef target = gen_expr(cg, scopes, depth, e->as.member.target);
    LLVMValueRef key_str_global =
        const_string_ptr(cg, e->as.member.name, strlen(e->as.member.name));
    LLVMValueRef key_str = ny_load(cg, key_str_global, "");

    fun_sig *get_sig = ny_helper_get(cg);
    if (!get_sig) {
      ny_diag_hint(
          "the 'std.core' module provides the standard 'get' implementation");
      ny_diag_fix("add 'use std.core' to your script");
      return expr_fail(
          cg, e->tok,
          "Member access on a dynamic object requires the 'get' function.");
    }

    LLVMValueRef args[16];
    args[0] = target;
    args[1] = key_str;
    unsigned arg_count = get_sig->arity;
    if (arg_count < 2)
      arg_count = 2;
    if (arg_count > 16)
      arg_count = 16;
    for (unsigned i = 2; i < arg_count; i++) {
      args[i] = ny_c1(cg);
    }
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, get_sig->type, get_sig->value, args,
                          arg_count, "");
  }
  case NY_E_SIZEOF: {
    const char *type_name = NULL;
    if (e->as.szof.is_type)
      type_name = e->as.szof.type_name;
    if (!type_name && e->as.szof.target &&
        e->as.szof.target->kind == NY_E_IDENT) {
      type_name = e->as.szof.target->as.ident.name;
    }
    if (!type_name) {
      ny_diag_error(e->tok, "sizeof expects a type name");
      cg->had_error = 1;
      return ny_c1(cg);
    }
    type_layout_t tl = resolve_raw_layout(cg, type_name, e->tok);
    if (!tl.is_valid) {
      cg->had_error = 1;
      return ny_c1(cg);
    }
    uint64_t sz = (uint64_t)tl.size;
    return LLVMConstInt(cg->type_i64, (sz << 1) | 1ULL, false);
  }
  case NY_E_LOGICAL: {
    return gen_expr_logical(cg, scopes, depth, e);
  }
  case NY_E_TERNARY: {
    LLVMValueRef cond = expr_gen_cond_i1(cg, scopes, depth, e->as.ternary.cond);
    LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
    LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
    LLVMBasicBlockRef true_bb = ny_bb_fn(f, "tern_true");
    LLVMBasicBlockRef false_bb = ny_bb_fn(f, "tern_false");
    LLVMBasicBlockRef end_bb = ny_bb_fn(f, "tern_end");
    ny_dbg_loc(cg, e->tok);
    ny_cond_br(cg, cond, true_bb, false_bb);

    ny_pos(cg, true_bb);

    LLVMValueRef true_val =
        gen_expr(cg, scopes, depth, e->as.ternary.true_expr);
    ny_br(cg, end_bb);
    LLVMBasicBlockRef true_end_bb = ny_cur_block(cg);

    ny_pos(cg, false_bb);

    LLVMValueRef false_val =
        gen_expr(cg, scopes, depth, e->as.ternary.false_expr);
    ny_br(cg, end_bb);
    LLVMBasicBlockRef false_end_bb = ny_cur_block(cg);

    ny_pos(cg, end_bb);

    ny_dbg_loc(cg, e->tok);
    LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "tern"));
    LLVMAddIncoming(phi, (LLVMValueRef[]){true_val, false_val},
                    (LLVMBasicBlockRef[]){true_end_bb, false_end_bb}, 2);
    return phi;
  }
  case NY_E_ASM: {
    unsigned nargs = e->as.as_asm.args.len;
    LLVMValueRef llvm_args[nargs > 0 ? nargs : 1];
    LLVMTypeRef arg_types[nargs > 0 ? nargs : 1];
    for (unsigned i = 0; i < nargs; ++i) {
      llvm_args[i] = gen_expr(cg, scopes, depth, e->as.as_asm.args.data[i]);
      arg_types[i] = cg->type_i64;
    }
    LLVMTypeRef func_type =
        LLVMFunctionType(cg->type_i64, arg_types, nargs, false);
    LLVMValueRef asm_val = LLVMConstInlineAsm(
        func_type, e->as.as_asm.code, e->as.as_asm.constraints, true, false);
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, func_type, asm_val, llvm_args, nargs,
                          "");
  }
  case NY_E_FSTRING: {

    LLVMValueRef empty_runtime_global = const_string_ptr(cg, "", 0);
    LLVMValueRef res = ny_load(cg, empty_runtime_global, "");
    fun_sig *cs = ny_helper_str_concat(cg), *ts = ny_helper_to_str(cg);
    if (!cs || !cs->type || !cs->value)
      return expr_fail(cg, e->tok, "__str_concat not found for f-string lowering");
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t p = e->as.fstring.parts.data[i];
      LLVMValueRef pv;
      if (p.kind == NY_FSP_STR) {
        LLVMValueRef part_runtime_global =
            const_string_ptr(cg, p.as.s.data, p.as.s.len);
        pv = ny_load(cg, part_runtime_global, "");
      } else {
        if (!ts || !ts->type || !ts->value)
          return expr_fail(cg, e->tok, "__to_str not found for f-string lowering");
        pv = LLVMBuildCall2(
            cg->builder, ts->type, ts->value,
            (LLVMValueRef[]){gen_expr(cg, scopes, depth, p.as.e)}, 1, "");
      }
      ny_dbg_loc(cg, e->tok);
      res = LLVMBuildCall2(cg->builder, cs->type, cs->value,
                           (LLVMValueRef[]){res, pv}, 2, "");
    }
    return res;
  }
  case NY_E_LAMBDA:
  case NY_E_FN: {

    return gen_closure(cg, scopes, depth, e->as.lambda.params,
                       e->as.lambda.body, e->as.lambda.is_variadic,
                       e->as.lambda.return_type, "__lambda");
  }
  case NY_E_EMBED: {
    const char *fname = e->as.embed.path;
    FILE *f = fopen(fname, "rb");
    if (!f) {
      char cwd[1024];
      if (!getcwd(cwd, sizeof(cwd)))
        cwd[0] = '\0';
      return expr_fail(cg, e->tok,
                       "failed to open file for embed: %s (cwd: %s)", fname,
                       cwd);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
      fclose(f);
      return expr_fail(cg, e->tok, "OOM reading file for embed");
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
      free(buf);
      fclose(f);
      return expr_fail(cg, e->tok, "failed to read file for embed: %s", fname);
    }
    fclose(f);
    LLVMValueRef g = const_string_ptr(cg, buf, (size_t)size);
    free(buf);
    return ny_load(cg, g, NY_LLVM_NAME(cg, "embed_ptr"));
  }
  case NY_E_TRY: {
    LLVMValueRef res = gen_expr(cg, scopes, depth, e->as.unary.right);
    LLVMValueRef f = ny_cur_fn(cg);
    LLVMBasicBlockRef ok_bb = ny_bb_fn(f, "try_ok");
    LLVMBasicBlockRef err_bb = ny_bb_fn(f, "try_err");

    fun_sig *is_ok_sig = lookup_fun(cg, "__is_ok", 0);
    if (!is_ok_sig) {
      return expr_fail(cg, e->tok, "__is_ok not found for '?' operator");
    }
    LLVMValueRef is_ok = LLVMBuildCall2(cg->builder, is_ok_sig->type,
                                        is_ok_sig->value, &res, 1, "");
    ny_dbg_loc(cg, e->tok);
    ny_cond_br(cg, to_bool(cg, is_ok), ok_bb, err_bb);

    ny_pos(cg, err_bb);

    if (cg->result_store_val) {
      ny_store(cg, cg->result_store_val, res);
    } else {
      emit_defers(cg, scopes, depth, cg->func_root_idx);
      ny_cg_emit_trace_return(cg, res, cg->current_fn_ret_type);
      ny_cg_emit_trace_exit(cg);
      LLVMBuildRet(cg->builder, res);
    }

    ny_pos(cg, ok_bb);

    fun_sig *unwrap_sig = lookup_fun(cg, "__unwrap", 0);
    if (!unwrap_sig) {
      return expr_fail(cg, e->tok, "__unwrap not found for '?' operator");
    }
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, unwrap_sig->type, unwrap_sig->value,
                          &res, 1, "unwrapped");
  }
  case NY_E_MATCH: {
    LLVMValueRef old_store = cg->result_store_val;
    LLVMValueRef slot = build_alloca(cg, "match_res", cg->type_i64);
    ny_store(cg, slot, ny_c1(cg));
    cg->result_store_val = slot;
    stmt_t fake = {.kind = NY_S_MATCH, .as.match = e->as.match, .tok = e->tok};
    size_t d = depth;
    gen_stmt(cg, scopes, &d, &fake, cg->func_root_idx, true);
    cg->result_store_val = old_store;
    return ny_load(cg, slot, "");
  }
  default: {
    return expr_fail(cg, e->tok,
                     "unsupported expression kind %d (token kind %d)", e->kind,
                     e->tok.kind);
  }
  }
}

LLVMValueRef gen_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {
  if (!ny_codegen_expr_kind_profile_enabled())
    return gen_expr_inner(cg, scopes, depth, e);
  if (!g_expr_kind_profile_registered) {
    atexit(ny_codegen_expr_kind_profile_report);
    g_expr_kind_profile_registered = 1;
  }
  int kind = e ? (int)e->kind : 63;
  int slot = g_expr_kind_depth;
  if (slot >= 0 && slot < (int)(sizeof(g_expr_kind_child_ms) / sizeof(g_expr_kind_child_ms[0])))
    g_expr_kind_child_ms[slot] = 0.0;
  g_expr_kind_depth++;
  ny_tick_t start = ny_ticks_now();
  LLVMValueRef out = gen_expr_inner(cg, scopes, depth, e);
  double total_ms = ny_ticks_elapsed_ms(start);
  g_expr_kind_depth--;
  double child_ms = 0.0;
  if (slot >= 0 && slot < (int)(sizeof(g_expr_kind_child_ms) / sizeof(g_expr_kind_child_ms[0])))
    child_ms = g_expr_kind_child_ms[slot];
  if (g_expr_kind_depth > 0) {
    int parent = g_expr_kind_depth - 1;
    if (parent >= 0 &&
        parent < (int)(sizeof(g_expr_kind_child_ms) / sizeof(g_expr_kind_child_ms[0])))
      g_expr_kind_child_ms[parent] += total_ms;
  }
  ny_codegen_expr_kind_profile_add(kind, total_ms, child_ms);
  return out;
}
