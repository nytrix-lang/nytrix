/*
 * Function codegen: lowers Nytrix function definitions to LLVM IR
 * including signature construction, argument marshalling, and bodies.
 */
#include "base/options.h"
#include "base/util.h"

#include "code/backend/llvm/bridge.h"
#include "code/lang/parse/proof.h"
#include "code/lang/priv.h"
#include "code/lang/types/ownership.h"
#include "code/lang/types/typeinfer.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif
#include <ctype.h>
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "code/lang/c/clang.h"

static bool ny_codegen_func_profile_enabled(void) {
  const char *env = getenv("NYTRIX_PROFILE_CODEGEN_FUNCS");
  return env && *env && strcmp(env, "0") != 0 && strcmp(env, "false") != 0 &&
         strcmp(env, "off") != 0;
}

static double ny_codegen_func_profile_threshold_ms(void) {
  const char *env = getenv("NYTRIX_PROFILE_CODEGEN_FUNCS_MS");
  if (!env || !*env)
    return 100.0;
  char *end = NULL;
  double val = strtod(env, &end);
  return (end != env && val >= 0.0) ? val : 100.0;
}

static void ny_codegen_func_profile_emit(const char *name, const char *file,
                                         double total_ms, double sig_ms,
                                         double assigned_ms, double params_ms,
                                         double infer_ms, double body_ms) {
  if (total_ms < ny_codegen_func_profile_threshold_ms())
    return;
  fprintf(stderr,
          "[codegen-func] total_ms=%.3f sig_ms=%.3f assigned_ms=%.3f "
          "params_ms=%.3f infer_ms=%.3f body_ms=%.3f name=%s file=%s\n",
          total_ms, sig_ms, assigned_ms, params_ms, infer_ms, body_ms,
          name ? name : "<anon>", file ? file : "<unknown>");
}

static LLVMValueRef func_param_float_value(codegen_t *cg, LLVMValueRef v,
                                           bool want_f32) {
  if (!cg || !v)
    return v;
  LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(v));
  if (want_f32) {
    if (kind == LLVMFloatTypeKind)
      return v;
    if (kind == LLVMDoubleTypeKind)
      return LLVMBuildFPTrunc(cg->builder, v, cg->type_f32, "param_f32");
    if (kind == LLVMIntegerTypeKind) {
      if (LLVMGetIntTypeWidth(LLVMTypeOf(v)) == 64) {
        LLVMValueRef f64v = ny_unbox_float(cg, v);
        return LLVMBuildFPTrunc(cg->builder, f64v, cg->type_f32, "param_f32");
      }
      return LLVMBuildSIToFP(cg->builder, v, cg->type_f32, "param_i2f32");
    }
    return v;
  }
  if (kind == LLVMDoubleTypeKind)
    return v;
  if (kind == LLVMFloatTypeKind)
    return LLVMBuildFPExt(cg->builder, v, cg->type_f64, "param_f64");
  if (kind == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(LLVMTypeOf(v)) == 64)
    return ny_unbox_float(cg, v);
  if (kind == LLVMIntegerTypeKind)
    return LLVMBuildSIToFP(cg->builder, v, cg->type_f64, "param_i2f64");
  return v;
}

bool ny_fn_uses_native_abi(codegen_t *cg, const stmt_t *fn) {
  if (!fn || fn->kind != NY_S_FUNC)
    return true;
  return fn->as.fn.is_extern || !cg || cg->user_native_abi;
}

LLVMTypeRef ny_fn_resolve_user_abi_type(codegen_t *cg, const stmt_t *fn,
                                        bool use_native_abi,
                                        const char *type_name, token_t tok) {
  if (!use_native_abi || !type_name)
    return cg->type_i64;
  if (fn && fn->kind == NY_S_FUNC && !fn->as.fn.is_extern &&
      ny_type_is(type_name, "fnptr"))
    return cg->type_i64;
  return resolve_abi_type_name(cg, type_name, tok);
}

static bool fn_tagged_abi_should_normalize_param(const char *type_name) {
  const char *leaf = ny_type_leaf(type_name);
  if (!leaf || !*leaf)
    return false;
  return strcmp(leaf, "int") == 0 || strcmp(leaf, "i8") == 0 ||
         strcmp(leaf, "i16") == 0 || strcmp(leaf, "i32") == 0 ||
         strcmp(leaf, "i64") == 0 || strcmp(leaf, "u8") == 0 ||
         strcmp(leaf, "u16") == 0 || strcmp(leaf, "u32") == 0 ||
         strcmp(leaf, "u64") == 0 || strcmp(leaf, "handle") == 0;
}

static layout_def_t *func_layout_abi_type(codegen_t *cg,
                                          const char *type_name) {
  if (!cg || !type_name || !*type_name)
    return NULL;
  while (*type_name == '?')
    type_name++;
  if (!*type_name || *type_name == '*')
    return NULL;
  layout_def_t *layout = lookup_layout(cg, type_name);
  return (layout && layout->llvm_type) ? layout : NULL;
}

static LLVMTypeRef func_layout_abi_carrier_type(codegen_t *cg,
                                                layout_def_t *layout) {
  if (!cg || !layout)
    return NULL;
  switch (layout->size) {
  case 1:
    return cg->type_i8;
  case 2:
    return cg->type_i16;
  case 4:
    return cg->type_i32;
  case 8:
    return cg->type_i64;
  default:
    return layout->llvm_type;
  }
}

static int ny_should_disable_trace_emission(codegen_t *cg) {
  return !cg || cg->trace_emit_disabled;
}

static void emit_trace_enter(codegen_t *cg, const char *name, token_t tok) {
  if (!cg || !cg->builder || !name)
    return;

  if (ny_should_disable_trace_emission(cg))
    return;
  fun_sig *ts = lookup_fun(cg, "__trace_enter", 0);
  if (!ts)
    return;
  LLVMValueRef nstr_g = const_string_ptr(cg, name, strlen(name));
  LLVMValueRef nstr = ny_load(cg, nstr_g, "");
  const char *fname = tok.filename ? tok.filename : "<unknown>";
  LLVMValueRef fstr_g = const_string_ptr(cg, fname, strlen(fname));
  LLVMValueRef fstr = ny_load(cg, fstr_g, "");
  int line = tok.line > 0 ? tok.line : 1;
  LLVMValueRef line_v =
      LLVMConstInt(cg->type_i64, ((uint64_t)line << 1) | 1, false);
  LLVMBuildCall2(cg->builder, ts->type, ts->value,
                 (LLVMValueRef[]){nstr, fstr, line_v}, 3, "");
}

static bool trace_ret_type_is_ptr(const char *type_name) {
  if (!type_name)
    return false;
  while (*type_name == '?')
    type_name++;
  return *type_name == '*' || ny_type_is(type_name, "ptr");
}

static bool trace_ret_type_is_bool(const char *type_name) {
  return ny_type_is(type_name, "bool");
}

static bool trace_ret_type_is_unsigned(const char *type_name) {
  const char *leaf = ny_type_leaf(type_name);
  if (!leaf || !*leaf)
    return false;
  return strcmp(leaf, "usize") == 0 ||
         (leaf[0] == 'u' && isdigit((unsigned char)leaf[1]));
}

static bool trace_ret_type_is_complex(const char *type_name) {
  const char *leaf = ny_type_leaf(type_name);
  return leaf && (strcmp(leaf, "complex") == 0 || strcmp(leaf, "c64") == 0 ||
                  strcmp(leaf, "c128") == 0);
}

static LLVMValueRef trace_i64_arg(codegen_t *cg, LLVMValueRef v,
                                  bool is_unsigned) {
  if (!cg || !v)
    return v;
  LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(v));
  if (kind == LLVMIntegerTypeKind) {
    unsigned bits = LLVMGetIntTypeWidth(LLVMTypeOf(v));
    if (bits < 64)
      return is_unsigned
                 ? LLVMBuildZExt(cg->builder, v, cg->type_i64, "trace_zext")
                 : LLVMBuildSExt(cg->builder, v, cg->type_i64, "trace_sext");
    if (bits > 64)
      return LLVMBuildTrunc(cg->builder, v, cg->type_i64, "trace_trunc");
    return v;
  }
  if (kind == LLVMPointerTypeKind)
    return LLVMBuildPtrToInt(cg->builder, v, cg->type_i64, "trace_ptr");
  return LLVMBuildBitCast(cg->builder, v, cg->type_i64, "trace_i64");
}

void ny_cg_emit_trace_return(codegen_t *cg, LLVMValueRef v,
                             const char *ret_type) {
  if (!cg || !cg->builder || !v)
    return;
  if (ny_should_disable_trace_emission(cg))
    return;

  if (!ret_type || !ny_is_native_abi_type_name(ret_type) ||
      ny_type_is_tagged(ret_type)) {
    fun_sig *ts = lookup_fun(cg, "__trace_ret_tagged", 0);
    if (!ts)
      return;
    LLVMBuildCall2(cg->builder, ts->type, ts->value, &v, 1, "");
    return;
  }

  if (ny_type_is(ret_type, "f32") || ny_type_is(ret_type, "f64") ||
      ny_type_is(ret_type, "f128")) {
    fun_sig *ts = lookup_fun(cg, "__trace_ret_f64_bits", 0);
    if (!ts)
      return;
    LLVMValueRef fv = v;
    LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(v));
    if (kind == LLVMFloatTypeKind)
      fv = LLVMBuildFPExt(cg->builder, v, cg->type_f64, "trace_f64");
    else if (kind == LLVMFP128TypeKind)
      fv = LLVMBuildFPTrunc(cg->builder, v, cg->type_f64, "trace_f64");
    LLVMValueRef bits =
        LLVMBuildBitCast(cg->builder, fv, cg->type_i64, "trace_f64_bits");
    LLVMBuildCall2(cg->builder, ts->type, ts->value, &bits, 1, "");
    return;
  }

  if (trace_ret_type_is_bool(ret_type)) {
    fun_sig *ts = lookup_fun(cg, "__trace_ret_bool", 0);
    if (!ts)
      return;
    LLVMValueRef arg = trace_i64_arg(cg, v, true);
    LLVMBuildCall2(cg->builder, ts->type, ts->value, &arg, 1, "");
    return;
  }

  if (trace_ret_type_is_ptr(ret_type)) {
    fun_sig *ts = lookup_fun(cg, "__trace_ret_ptr", 0);
    if (!ts)
      return;
    LLVMValueRef arg = trace_i64_arg(cg, v, true);
    LLVMBuildCall2(cg->builder, ts->type, ts->value, &arg, 1, "");
    return;
  }

  if (trace_ret_type_is_complex(ret_type)) {
    fun_sig *ts = lookup_fun(cg, "__trace_ret_tagged", 0);
    if (!ts)
      return;
    LLVMValueRef boxed = ny_box_abi_result(cg, v, ret_type);
    LLVMBuildCall2(cg->builder, ts->type, ts->value, &boxed, 1, "");
    return;
  }

  {
    fun_sig *ts =
        lookup_fun(cg,
                   trace_ret_type_is_unsigned(ret_type) ? "__trace_ret_u64"
                                                        : "__trace_ret_i64",
                   0);
    if (!ts)
      return;
    LLVMValueRef arg =
        trace_i64_arg(cg, v, trace_ret_type_is_unsigned(ret_type));
    LLVMBuildCall2(cg->builder, ts->type, ts->value, &arg, 1, "");
  }
}

void ny_cg_emit_trace_return_void(codegen_t *cg) {
  if (!cg || !cg->builder)
    return;
  if (ny_should_disable_trace_emission(cg))
    return;
  fun_sig *ts = lookup_fun(cg, "__trace_ret_void", 0);
  if (!ts)
    return;
  LLVMBuildCall2(cg->builder, ts->type, ts->value, NULL, 0, "");
}

void ny_cg_emit_trace_exit(codegen_t *cg) {
  if (!cg || !cg->builder)
    return;
  if (ny_should_disable_trace_emission(cg))
    return;
  fun_sig *ts = lookup_fun(cg, "__trace_exit", 0);
  if (!ts)
    return;
  LLVMBuildCall2(cg->builder, ts->type, ts->value, NULL, 0, "");
}

static void emit_trace_arg_call(codegen_t *cg, const char *name,
                                LLVMValueRef value) {
  if (!cg || !cg->builder || !value || ny_should_disable_trace_emission(cg))
    return;
  fun_sig *ts = lookup_fun(cg, name, 0);
  if (!ts)
    return;
  LLVMBuildCall2(cg->builder, ts->type, ts->value, &value, 1, "");
}

static void emit_trace_args(codegen_t *cg, scope *scopes, size_t depth,
                            stmt_t *fn) {
  if (!cg || !scopes || !fn || fn->kind != NY_S_FUNC ||
      ny_should_disable_trace_emission(cg))
    return;
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    const char *name = fn->as.fn.params.data[i].name;
    binding *b = name ? scope_lookup(scopes, depth, name) : NULL;
    if (!b || !b->value)
      continue;
    const char *ptype = fn->as.fn.params.data[i].type;
    bool is_float = ny_type_is(ptype, "f32") || ny_type_is(ptype, "f64") ||
                    b->is_f64_slot || b->is_f64_direct || b->is_f32_slot ||
                    b->is_f32_direct;
    if (is_float) {
      LLVMValueRef value =
          b->is_slot ? ny_load(cg, b->value, NY_LLVM_NAME(cg, "trace.f64"))
                     : b->value;
      LLVMTypeKind kind =
          value ? LLVMGetTypeKind(LLVMTypeOf(value)) : LLVMVoidTypeKind;
      if (value && kind == LLVMFloatTypeKind)
        value = LLVMBuildFPExt(cg->builder, value, cg->type_f64, "trace.f64");
      emit_trace_arg_call(cg, "__trace_arg_f64", value);
      continue;
    }
    if (b->raw_int_value && (b->is_int_slot || b->is_int_direct)) {
      LLVMValueRef raw = b->is_slot ? ny_load(cg, b->raw_int_value,
                                              NY_LLVM_NAME(cg, "trace.i64"))
                                    : b->raw_int_value;
      emit_trace_arg_call(cg, "__trace_arg_i64", raw);
      continue;
    }
    LLVMValueRef value =
        b->is_slot ? ny_load(cg, b->value, NY_LLVM_NAME(cg, "trace.arg"))
                   : b->value;
    emit_trace_arg_call(cg, "__trace_arg_tagged", value);
  }
}

void add_fn_enum_attr(codegen_t *cg, LLVMValueRef fn, const char *name,
                      uint64_t val) {
  if (!cg || !fn || !name)
    return;
  unsigned kind_id =
      LLVMGetEnumAttributeKindForName(name, (unsigned)strlen(name));
  if (kind_id == 0)
    return;
  LLVMAttributeRef attr = LLVMCreateEnumAttribute(cg->ctx, kind_id, val);
  LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, attr);
}

void add_fn_string_attr(codegen_t *cg, LLVMValueRef fn, const char *name,
                        const char *value) {
  if (!cg || !fn || !name || !*name)
    return;
  if (!value)
    value = "";
  LLVMAttributeRef attr = LLVMCreateStringAttribute(
      cg->ctx, name, (unsigned)strlen(name), value, (unsigned)strlen(value));
  LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, attr);
}

static inline bool attr_name_eq(const attribute_t *attr, const char *name) {
  if (!attr || !attr->name || !name)
    return false;
  return strcmp(attr->name, name) == 0;
}

static token_t attr_diag_tok(const stmt_t *fn_stmt, const attribute_t *attr,
                             size_t arg_index) {
  if (attr && arg_index < attr->args.len) {
    expr_t *arg = attr->args.data[arg_index];
    if (arg)
      return arg->tok;
  }
  if (attr)
    return attr->tok;
  if (fn_stmt)
    return fn_stmt->tok;
  token_t empty = {0};
  return empty;
}

static bool attr_arg_text_view(expr_t *arg, const char **text, size_t *len) {
  if (!arg || !text || !len)
    return false;
  if (arg->kind == NY_E_IDENT && arg->as.ident.name) {
    *text = arg->as.ident.name;
    *len = strlen(arg->as.ident.name);
    return true;
  }
  if (arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_STR &&
      arg->as.literal.as.s.data) {
    *text = arg->as.literal.as.s.data;
    *len = arg->as.literal.as.s.len;
    return true;
  }

  if (arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_INT &&
      arg->as.literal.as.i == 0 && arg->tok.lexeme && arg->tok.len > 0) {
    *text = arg->tok.lexeme;
    *len = arg->tok.len;
    return true;
  }
  return false;
}

static bool accel_target_view_is_supported(const char *target, size_t len) {
  static const char *const names[] = {
      "auto",   "none",   "off",    "nvptx", "ptx",  "cuda",  "spirv", "spv",
      "opencl", "vulkan", "amdgpu", "hip",   "rocm", "hsaco", "hsa"};
  if (!target || len == 0)
    return false;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    size_t n = strlen(names[i]);
    if (n == len && strncmp(target, names[i], len) == 0)
      return true;
  }
  return false;
}

static bool effect_attr_name_mask(const char *name, size_t len,
                                  uint32_t *mask_out) {
  if (!name || len == 0 || !mask_out)
    return false;
  if (len == 2 && strncmp(name, "io", 2) == 0) {
    *mask_out = NY_FX_IO;
    return true;
  }
  if (len == 5 && strncmp(name, "alloc", 5) == 0) {
    *mask_out = NY_FX_ALLOC;
    return true;
  }
  if (len == 3 && strncmp(name, "ffi", 3) == 0) {
    *mask_out = NY_FX_FFI;
    return true;
  }
  if (len == 6 && strncmp(name, "thread", 6) == 0) {
    *mask_out = NY_FX_THREAD;
    return true;
  }
  if ((len == 4 && strncmp(name, "read", 4) == 0) ||
      (len == 11 && strncmp(name, "read_memory", 11) == 0)) {
    *mask_out = NY_FX_READ_MEMORY;
    return true;
  }
  if ((len == 5 && strncmp(name, "write", 5) == 0) ||
      (len == 12 && strncmp(name, "write_memory", 12) == 0)) {
    *mask_out = NY_FX_WRITE_MEMORY;
    return true;
  }
  if (len == 3 && strncmp(name, "all", 3) == 0) {
    *mask_out = NY_FX_ALL;
    return true;
  }
  if ((len == 4 && strncmp(name, "none", 4) == 0) ||
      (len == 4 && strncmp(name, "pure", 4) == 0)) {
    *mask_out = NY_FX_NONE;
    return true;
  }
  return false;
}

static bool parse_effect_attr_args(codegen_t *cg, const stmt_t *fn_stmt,
                                   const attribute_t *attr, uint32_t *mask_out,
                                   ny_type_t **effect_type_out) {
  if (!cg || !fn_stmt || !attr || !mask_out)
    return false;
  uint32_t mask = NY_FX_NONE;
  bool saw_any = false;
  for (size_t i = 0; i < attr->args.len; i++) {
    expr_t *arg_expr = attr->args.data[i];
    if (arg_expr && arg_expr->kind == NY_E_CALL && arg_expr->as.call.callee &&
        arg_expr->as.call.callee->kind == NY_E_IDENT &&
        arg_expr->as.call.callee->as.ident.name) {
      const char *cname = arg_expr->as.call.callee->as.ident.name;
      if (strcmp(cname, "cap") == 0 || strcmp(cname, "capability") == 0) {
        bool cap_saw_any = false;
        for (size_t a = 0; a < arg_expr->as.call.args.len; ++a) {
          expr_t *subarg = arg_expr->as.call.args.data[a].val;
          const char *subname = NULL;
          size_t sublen = 0;
          if (attr_arg_text_view(subarg, &subname, &sublen) && sublen > 0) {
            if (sublen > NY_TYPE_CAPABILITY_NAME_MAX) {
              ny_diag_error(attr_diag_tok(fn_stmt, attr, i),
                            "capability name in @effects(cap(...)) exceeds "
                            "the %u-byte limit",
                            NY_TYPE_CAPABILITY_NAME_MAX);
              cg->had_error = 1;
              continue;
            }
            char cap_buf[NY_TYPE_CAPABILITY_NAME_MAX + 1] = {0};
            {
              memcpy(cap_buf, subname, sublen);
              cap_buf[sublen] = '\0';
              char identity[NY_TYPE_CAPABILITY_NAME_MAX + 1] = {0};
              if (!ny_type_capability_identity(cg->current_module_name, cap_buf,
                                               identity)) {
                ny_diag_error(
                    attr_diag_tok(fn_stmt, attr, i),
                    "capability identity in @effects(cap(...)) exceeds the "
                    "%u-byte limit after module qualification",
                    NY_TYPE_CAPABILITY_NAME_MAX);
                cg->had_error = 1;
                continue;
              }
              if (effect_type_out) {
                ny_type_t *cap_row =
                    ny_type_capability_effect(NULL, identity, *effect_type_out);
                if (!cap_row) {
                  ny_diag_error(attr_diag_tok(fn_stmt, attr, i),
                                "could not represent capability in "
                                "@effects(cap(...))");
                  cg->had_error = 1;
                  continue;
                }
                *effect_type_out = cap_row;
              }
              cap_saw_any = true;
            }
          }
        }
        if (cap_saw_any) {
          saw_any = true;
          continue;
        }
      } else if (strcmp(cname, "typestate") == 0 ||
                 strcmp(cname, "state") == 0) {
        const char *from_state = NULL;
        const char *to_state = NULL;
        size_t slen = 0;
        if (arg_expr->as.call.args.len >= 1 &&
            attr_arg_text_view(arg_expr->as.call.args.data[0].val, &from_state,
                               &slen)) {
          if (arg_expr->as.call.args.len >= 2)
            attr_arg_text_view(arg_expr->as.call.args.data[1].val, &to_state,
                               &slen);
          if (effect_type_out) {
            ny_type_t *ts =
                ny_type_typestate(NULL, "state", from_state, to_state, NULL);
            *effect_type_out = *effect_type_out ? ny_type_row(NULL, "state", ts,
                                                              *effect_type_out)
                                                : ts;
          }
          saw_any = true;
          continue;
        }
      }
    }
    const char *name = NULL;
    size_t len = 0;
    if (!attr_arg_text_view(attr->args.data[i], &name, &len)) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, i),
                    "expected effect name in @effects(...)");
      ny_diag_hint("supported: io, alloc, ffi, thread, all, none, capability, "
                   "or effect variable");
      cg->had_error = 1;
      continue;
    }
    uint32_t tok_mask = NY_FX_NONE;
    if (effect_attr_name_mask(name, len, &tok_mask)) {
      mask |= tok_mask;
      saw_any = true;
      continue;
    }
    /*
     * Effect variable: single letter, 'var, or eff
     */
    if ((len == 1 && (name[0] >= 'a' && name[0] <= 'z')) ||
        (len > 1 && name[0] == '\'') ||
        (len >= 3 && strncmp(name, "eff", 3) == 0)) {
      if (effect_type_out && !*effect_type_out) {
        *effect_type_out = ny_type_effect_var(NULL);
      }
      saw_any = true;
      continue;
    }
    /*
     * Capability name: e.g. read, write, fs, net, console, or cap(...)
     */
    if (len > 0) {
      char cap_name[64] = {0};
      if (len >= 5 && strncmp(name, "cap(", 4) == 0 && name[len - 1] == ')') {
        size_t c_len = len - 5;
        if (c_len < sizeof(cap_name)) {
          memcpy(cap_name, name + 4, c_len);
          cap_name[c_len] = '\0';
        }
      } else if (len < sizeof(cap_name)) {
        memcpy(cap_name, name, len);
        cap_name[len] = '\0';
      }
      if (cap_name[0]) {
        if (effect_type_out) {
          char identity[NY_TYPE_CAPABILITY_NAME_MAX + 1] = {0};
          if (!ny_type_capability_identity(cg->current_module_name, cap_name,
                                           identity)) {
            ny_diag_error(attr_diag_tok(fn_stmt, attr, i),
                          "capability identity in @effects(...) exceeds the "
                          "%u-byte limit after module qualification",
                          NY_TYPE_CAPABILITY_NAME_MAX);
            cg->had_error = 1;
            continue;
          }
          *effect_type_out =
              ny_type_capability_effect(NULL, identity, *effect_type_out);
        }
        saw_any = true;
        continue;
      }
    }
    ny_diag_error(attr_diag_tok(fn_stmt, attr, i),
                  "unknown effect name in @effects(...)");
    ny_diag_hint("supported: io, alloc, ffi, thread, all, none, capability, or "
                 "effect variable");
    cg->had_error = 1;
  }
  if (!saw_any) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                  "@effects(...) requires at least one effect name");
    ny_diag_hint("use @pure or @effects(none) for effect-free functions");
    cg->had_error = 1;
  }
  *mask_out = mask;
  return saw_any;
}

static bool fn_has_param_named(const stmt_t *fn_stmt, const char *name,
                               size_t len) {
  if (!fn_stmt || !name || fn_stmt->kind != NY_S_FUNC)
    return false;
  for (size_t i = 0; i < fn_stmt->as.fn.params.len; i++) {
    const char *param = fn_stmt->as.fn.params.data[i].name;
    if (param && strlen(param) == len && strncmp(param, name, len) == 0)
      return true;
  }
  return false;
}

static char *codegen_strndup(codegen_t *cg, const char *s, size_t len) {
  if (!s)
    s = "";
  if (cg && cg->arena)
    return arena_strndup(cg->arena, s, len);
  return ny_strndup(s, len);
}

static char *codegen_strdup(codegen_t *cg, const char *s) {
  return codegen_strndup(cg, s ? s : "", s ? strlen(s) : 0);
}

static char *parse_single_param_contract_attr(codegen_t *cg,
                                              const stmt_t *fn_stmt,
                                              const attribute_t *attr,
                                              const char *spelling) {
  if (!cg || !fn_stmt || !attr)
    return NULL;
  if (attr->args.len != 1) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                  "%s requires exactly one parameter name", spelling);
    cg->had_error = 1;
    return NULL;
  }
  const char *name = NULL;
  size_t len = 0;
  if (!attr_arg_text_view(attr->args.data[0], &name, &len) || len == 0) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                  "expected parameter name in %s", spelling);
    cg->had_error = 1;
    return NULL;
  }
  if (!fn_has_param_named(fn_stmt, name, len)) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                  "%s references unknown parameter '%.*s'", spelling, (int)len,
                  name);
    cg->had_error = 1;
    return NULL;
  }
  return codegen_strndup(cg, name, len);
}

static void parse_param_contract_list_attr(codegen_t *cg, const stmt_t *fn_stmt,
                                           const attribute_t *attr,
                                           const char *spelling,
                                           ny_str_list *out) {
  if (!cg || !fn_stmt || !attr || !out)
    return;
  if (attr->args.len == 0) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                  "%s requires at least one parameter name", spelling);
    cg->had_error = 1;
    return;
  }
  for (size_t i = 0; i < attr->args.len; i++) {
    const char *name = NULL;
    size_t len = 0;
    if (!attr_arg_text_view(attr->args.data[i], &name, &len) || len == 0) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, i),
                    "expected parameter name in %s", spelling);
      cg->had_error = 1;
      continue;
    }
    if (!fn_has_param_named(fn_stmt, name, len)) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, i),
                    "%s references unknown parameter '%.*s'", spelling,
                    (int)len, name);
      cg->had_error = 1;
      continue;
    }
    if (cg && cg->arena)
      vec_push_arena(cg->arena, out, codegen_strndup(cg, name, len));
    else
      vec_push(out, codegen_strndup(cg, name, len));
  }
}

static void parse_allowance_attr(codegen_t *cg, const stmt_t *fn_stmt,
                                 const attribute_t *attr,
                                 ny_allowance_list *out) {
  if (!cg || !fn_stmt || !attr || !out)
    return;
  if (attr->args.len != 2) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                  "@allowance requires (parameter, limit-or-[lo,hi])");
    cg->had_error = 1;
    return;
  }
  const char *name = NULL;
  size_t len = 0;
  if (!attr_arg_text_view(attr->args.data[0], &name, &len) || len == 0) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                  "expected parameter name in @allowance");
    cg->had_error = 1;
    return;
  }
  if (!fn_has_param_named(fn_stmt, name, len)) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                  "@allowance references unknown parameter '%.*s'", (int)len,
                  name);
    cg->had_error = 1;
    return;
  }
  for (size_t i = 0; i < out->len; i++) {
    if (out->data[i].name && strlen(out->data[i].name) == len &&
        strncmp(out->data[i].name, name, len) == 0) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                    "duplicate @allowance for parameter '%.*s'", (int)len,
                    name);
      cg->had_error = 1;
      return;
    }
  }
  expr_t *limit_expr = attr->args.data[1];
  int64_t lo = 0, hi = -1;
  bool valid = false;
  if (limit_expr && limit_expr->kind == NY_E_LIST &&
      limit_expr->as.list_like.len == 2) {
    expr_t *lo_expr = limit_expr->as.list_like.data[0];
    expr_t *hi_expr = limit_expr->as.list_like.data[1];
    bool lo_valid = lo_expr && lo_expr->kind == NY_E_LITERAL &&
                    lo_expr->as.literal.kind == NY_LIT_INT &&
                    lo_expr->as.literal.as.i >= 0;
    bool hi_valid = false;
    if (hi_expr && hi_expr->kind == NY_E_LITERAL &&
        hi_expr->as.literal.kind == NY_LIT_INT &&
        hi_expr->as.literal.as.i >= 0) {
      hi = hi_expr->as.literal.as.i;
      hi_valid = true;
    } else if (hi_expr && hi_expr->kind == NY_E_IDENT &&
               hi_expr->as.ident.name &&
               (strcmp(hi_expr->as.ident.name, "omega") == 0 ||
                strcmp(hi_expr->as.ident.name, "inf") == 0 ||
                strcmp(hi_expr->as.ident.name, "infinity") == 0)) {
      hi = -1;
      hi_valid = true;
    }
    if (lo_valid) {
      lo = lo_expr->as.literal.as.i;
      valid = hi_valid && (hi < 0 || lo <= hi);
    }
  } else if (limit_expr && limit_expr->kind == NY_E_LITERAL &&
             limit_expr->as.literal.kind == NY_LIT_INT &&
             limit_expr->as.literal.as.i >= 0) {
    lo = hi = limit_expr->as.literal.as.i;
    valid = true;
  } else if (limit_expr && limit_expr->kind == NY_E_IDENT &&
             limit_expr->as.ident.name &&
             (strcmp(limit_expr->as.ident.name, "omega") == 0 ||
              strcmp(limit_expr->as.ident.name, "inf") == 0 ||
              strcmp(limit_expr->as.ident.name, "infinity") == 0)) {
    lo = 0;
    hi = -1;
    valid = true;
  }
  if (!valid) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 1),
                  "expected non-negative integer or valid [lo,hi] interval "
                  "with 'omega'/'inf' upper bound in @allowance");
    cg->had_error = 1;
    return;
  }
  ny_allowance_t item;
  item.name = codegen_strndup(cg, name, len);
  item.lo = lo;
  item.hi = hi;
  if (cg && cg->arena)
    vec_push_arena(cg->arena, out, item);
  else
    vec_push(out, item);
}

void ny_fun_sig_copy_contracts(fun_sig *sig, const stmt_func_t *fn) {
  if (!sig || !fn)
    return;
  sig->returns_borrow =
      fn->attr_returns_borrow ? ny_strdup(fn->attr_returns_borrow) : NULL;
  for (size_t i = 0; i < fn->attr_borrows.len; i++)
    vec_push(&sig->borrows, ny_strdup(fn->attr_borrows.data[i]));
  for (size_t i = 0; i < fn->attr_consumes.len; i++)
    vec_push(&sig->consumes, ny_strdup(fn->attr_consumes.data[i]));
  for (size_t i = 0; i < fn->attr_mutates.len; i++)
    vec_push(&sig->mutates, ny_strdup(fn->attr_mutates.data[i]));
  for (size_t i = 0; i < fn->attr_releases.len; i++)
    vec_push(&sig->releases, ny_strdup(fn->attr_releases.data[i]));
  for (size_t i = 0; i < fn->attr_forgets.len; i++)
    vec_push(&sig->forgets, ny_strdup(fn->attr_forgets.data[i]));
  for (size_t i = 0; i < fn->attr_allowances.len; i++) {
    ny_allowance_t a;
    a.name = ny_strdup(fn->attr_allowances.data[i].name);
    a.lo = fn->attr_allowances.data[i].lo;
    a.hi = fn->attr_allowances.data[i].hi;
    vec_push(&sig->allowances, a);
  }
  sig->requires_cond = fn->requires_cond;
  sig->decreases_expr = fn->decreases_expr;
  sig->is_total = fn->attr_total;
  sig->effect_type = fn->effect_type;
}

static void mark_simple_flag_attr(codegen_t *cg, const stmt_t *fn_stmt,
                                  const attribute_t *attr,
                                  const char *attr_spelling, bool *flag) {
  if (!cg || !fn_stmt || !attr || !attr_spelling || !flag)
    return;
  if (*flag) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0), "duplicate attribute '%s'",
                  attr_spelling);
    cg->had_error = 1;
  }
  if (attr->args.len != 0) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0), "%s does not take arguments",
                  attr_spelling);
    cg->had_error = 1;
  }
  *flag = true;
}

void ny_resolve_fn_attrs(codegen_t *cg, stmt_t *fn_stmt) {
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC)
    return;
  stmt_func_t *decl = &fn_stmt->as.fn;
  if (decl->attrs_resolved)
    return;
  bool is_naked = false, is_jit = false, is_thread = false,
       is_async_effects = false;
  bool is_pure = false, is_cache = false;
  bool is_total = false;
  bool is_extern = false;
  const char *link_name = NULL;
  bool has_effect_contract = false;
  uint32_t effect_contract_mask = NY_FX_NONE;
  ny_type_t *effect_type = NULL;
  for (size_t i = 0; i < fn_stmt->attributes.len; i++) {
    attribute_t *attr = &fn_stmt->attributes.data[i];
    if (attr_name_eq(attr, "naked")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@naked", &is_naked);
      continue;
    }
    if (attr_name_eq(attr, "jit")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@jit", &is_jit);
      continue;
    }
    if (attr_name_eq(attr, "thread")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@thread", &is_thread);
      continue;
    }
    if (attr_name_eq(attr, "async_effects")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@async_effects",
                            &is_async_effects);
      continue;
    }
    if (attr_name_eq(attr, "pure")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@pure", &is_pure);
      continue;
    }
    if (attr_name_eq(attr, "total")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@total", &is_total);
      continue;
    }
    if (attr_name_eq(attr, "cache")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@cache", &is_cache);
      continue;
    }
    if (attr_name_eq(attr, "proves")) {
      if (decl->attr_proves) {
        ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                      "duplicate attribute '@proves'");
        cg->had_error = 1;
      }
      if (attr->args.len != 1) {
        ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                      "@proves requires exactly one condition argument");
        cg->had_error = 1;
      } else {
        decl->attr_proves = attr->args.data[0];
      }
      continue;
    }
    if (attr_name_eq(attr, "inline")) {
      decl->attr_inline = true;
      continue;
    }
    if (attr_name_eq(attr, "noinline")) {
      decl->attr_noinline = true;
      continue;
    }
    if (attr_name_eq(attr, "readnone")) {
      decl->attr_readnone = true;
      continue;
    }
    if (attr_name_eq(attr, "readonly")) {
      decl->attr_readonly = true;
      continue;
    }
    if (attr_name_eq(attr, "writeonly")) {
      decl->attr_writeonly = true;
      continue;
    }
    if (attr_name_eq(attr, "argmemonly")) {
      decl->attr_argmemonly = true;
      continue;
    }
    if (attr_name_eq(attr, "nounwind")) {
      decl->attr_nounwind = true;
      continue;
    }
    if (attr_name_eq(attr, "mustprogress")) {
      decl->attr_mustprogress = true;
      continue;
    }
    if (attr_name_eq(attr, "willreturn")) {
      decl->attr_willreturn = true;
      continue;
    }
    if (attr_name_eq(attr, "cold")) {
      decl->attr_cold = true;
      continue;
    }
    if (attr_name_eq(attr, "hot")) {
      decl->attr_hot = true;
      continue;
    }
    if (attr_name_eq(attr, "flatten")) {
      decl->attr_flatten = true;
      continue;
    }
    if (attr_name_eq(attr, "tailcall")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@tailcall",
                            &decl->attr_tailcall);
      continue;
    }
    if (attr_name_eq(attr, "sys")) {
      decl->attr_sys = true;
      continue;
    }
    if (attr_name_eq(attr, "nogc")) {
      decl->attr_nogc = true;
      continue;
    }
    if (attr_name_eq(attr, "consteval")) {
      decl->attr_consteval = true;
      continue;
    }
    if (attr_name_eq(attr, "constant_time")) {
      decl->attr_constant_time = true;
      continue;
    }
    if (attr_name_eq(attr, "optimize")) {
      if (decl->attr_optimize) {
        ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                      "duplicate attribute '@optimize(...)'");
        cg->had_error = 1;
        continue;
      }
      if (attr->args.len != 1 || !attr->args.data[0] ||
          attr->args.data[0]->kind != NY_E_LITERAL ||
          attr->args.data[0]->as.literal.kind != NY_LIT_INT ||
          attr->args.data[0]->as.literal.as.i < 0 ||
          attr->args.data[0]->as.literal.as.i > 3) {
        ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                      "@optimize expects one integer level from 0 through 3");
        ny_diag_hint(
            "use @optimize(0), @optimize(1), @optimize(2), or @optimize(3)");
        cg->had_error = 1;
        continue;
      }
      decl->attr_optimize = true;
      decl->attr_optimize_level = (int)attr->args.data[0]->as.literal.as.i;
      continue;
    }
    if (attr_name_eq(attr, "accel") || attr_name_eq(attr, "kernel")) {
      if (decl->attr_accel) {
        ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                      "duplicate attribute '@accel'");
        cg->had_error = 1;
      }
      if (attr->args.len > 1) {
        ny_diag_error(attr_diag_tok(fn_stmt, attr, 1),
                      "@accel expects at most one target");
        ny_diag_hint("use @accel, @accel(spirv), or @accel(nvptx)");
        cg->had_error = 1;
      } else if (attr->args.len == 1) {
        const char *target = NULL;
        size_t target_len = 0;
        if (!attr_arg_text_view(attr->args.data[0], &target, &target_len) ||
            target_len == 0) {
          ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                        "@accel target must be an identifier or string");
          cg->had_error = 1;
        } else if (!accel_target_view_is_supported(target, target_len)) {
          ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                        "unsupported @accel target '%.*s'", (int)target_len,
                        target);
          ny_diag_hint("supported accelerator targets: auto, nvptx, spirv, "
                       "amdgpu, hsaco");
          cg->had_error = 1;
        } else {
          decl->attr_accel_target = codegen_strndup(cg, target, target_len);
        }
      }
      decl->attr_accel = true;
      continue;
    }
    if (attr_name_eq(attr, "returns_owned")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@returns_owned",
                            &decl->attr_returns_owned);
      continue;
    }
    if (attr_name_eq(attr, "returns_borrow")) {
      if (decl->attr_returns_borrow) {
        ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                      "duplicate attribute '@returns_borrow(...)'");
        cg->had_error = 1;
      } else {
        decl->attr_returns_borrow = parse_single_param_contract_attr(
            cg, fn_stmt, attr, "@returns_borrow(...)");
      }
      continue;
    }
    if (attr_name_eq(attr, "borrows")) {
      parse_param_contract_list_attr(cg, fn_stmt, attr, "@borrows(...)",
                                     &decl->attr_borrows);
      continue;
    }
    if (attr_name_eq(attr, "consumes")) {
      parse_param_contract_list_attr(cg, fn_stmt, attr, "@consumes(...)",
                                     &decl->attr_consumes);
      continue;
    }
    if (attr_name_eq(attr, "mutates")) {
      parse_param_contract_list_attr(cg, fn_stmt, attr, "@mutates(...)",
                                     &decl->attr_mutates);
      continue;
    }
    if (attr_name_eq(attr, "releases")) {
      parse_param_contract_list_attr(cg, fn_stmt, attr, "@releases(...)",
                                     &decl->attr_releases);
      continue;
    }
    if (attr_name_eq(attr, "forgets")) {
      parse_param_contract_list_attr(cg, fn_stmt, attr, "@forgets(...)",
                                     &decl->attr_forgets);
      continue;
    }
    if (attr_name_eq(attr, "allowance")) {
      parse_allowance_attr(cg, fn_stmt, attr, &decl->attr_allowances);
      continue;
    }
    if (attr_name_eq(attr, "extern")) {
      is_extern = true;
      if (attr->args.len > 0) {
        expr_t *arg = attr->args.data[0];
        if (arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_STR)
          link_name = arg->as.literal.as.s.data;
      }
      continue;
    }
    if (attr_name_eq(attr, "effects")) {
      if (has_effect_contract) {
        ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                      "duplicate attribute '@effects(...)'");
        cg->had_error = 1;
        continue;
      }
      if (parse_effect_attr_args(cg, fn_stmt, attr, &effect_contract_mask,
                                 &effect_type))
        has_effect_contract = true;
      continue;
    }
  }
  if (is_jit && is_thread) {
    ny_diag_error(fn_stmt->tok, "conflicting attributes '@jit' and '@thread'");
    ny_diag_hint("use only one of them on the same function");
    cg->had_error = 1;
    is_jit = false;
  }
  if (is_jit && is_async_effects) {
    ny_diag_error(fn_stmt->tok,
                  "conflicting attributes '@jit' and '@async_effects'");
    ny_diag_hint(
        "@async_effects marks IO functions for stackless async lowering");
    cg->had_error = 1;
    is_async_effects = false;
  }
  if (is_pure && has_effect_contract && effect_contract_mask != NY_FX_NONE) {
    ny_diag_error(fn_stmt->tok,
                  "conflicting attributes '@pure' and '@effects(...)'");
    ny_diag_hint("use @effects(none) when combining with @pure");
    cg->had_error = 1;
  }
  if (is_cache && is_extern) {
    ny_diag_error(fn_stmt->tok,
                  "attribute '@cache' is not supported on extern functions");
    cg->had_error = 1;
    is_cache = false;
  }
  if (is_cache && decl->is_variadic) {
    ny_diag_error(fn_stmt->tok,
                  "attribute '@cache' is not supported on variadic functions");
    cg->had_error = 1;
    is_cache = false;
  }
  if (is_cache && is_thread) {
    ny_diag_error(fn_stmt->tok,
                  "attribute '@cache' is not supported with '@thread'");
    cg->had_error = 1;
    is_cache = false;
  }
  if (is_cache && is_async_effects) {
    ny_diag_error(fn_stmt->tok,
                  "attribute '@cache' is not supported with '@async_effects'");
    cg->had_error = 1;
    is_cache = false;
    is_async_effects = false;
  }
  if (is_cache && is_naked) {
    ny_diag_error(fn_stmt->tok,
                  "attribute '@cache' is not supported with '@naked'");
    cg->had_error = 1;
    is_cache = false;
  }
  if (is_naked && is_async_effects) {
    ny_diag_error(fn_stmt->tok,
                  "conflicting attributes '@naked' and '@async_effects'");
    ny_diag_hint("@async_effects needs the normal Nytrix function ABI");
    cg->had_error = 1;
    is_async_effects = false;
  }
  if (decl->attr_accel && is_naked) {
    ny_diag_error(fn_stmt->tok, "conflicting attributes '@accel' and '@naked'");
    cg->had_error = 1;
    decl->attr_accel = false;
  }
  decl->attr_naked = is_naked;
  decl->attr_jit = is_jit;
  decl->attr_thread = is_thread;
  decl->attr_async_effects = is_async_effects;
  decl->attr_pure = is_pure;
  decl->attr_total = is_total;
  decl->attr_cache = is_cache;
  decl->is_extern = is_extern;
  decl->link_name = link_name ? codegen_strdup(cg, link_name) : NULL;
  if (is_pure) {
    decl->effect_contract_known = true;
    decl->effect_contract_mask = NY_FX_NONE;
    decl->attr_readnone = true;
    decl->attr_nounwind = true;
    decl->attr_willreturn = true;
    decl->attr_mustprogress = true;
  } else {
    decl->effect_contract_known = has_effect_contract;
    decl->effect_contract_mask =
        has_effect_contract ? effect_contract_mask : NY_FX_ALL;
    decl->effect_type = effect_type;
  }
  if (decl->attr_jit) {
    decl->attr_inline = true;
    decl->attr_hot = true;
  }
  if (decl->attr_thread) {
    decl->attr_noinline = true;
    decl->attr_cold = true;
  }
  if (decl->attr_consteval) {

    decl->attr_inline = true;
    decl->attr_readnone = true;
    decl->attr_nounwind = true;
    decl->attr_willreturn = true;
    decl->attr_mustprogress = true;
    decl->effect_contract_known = true;
    decl->effect_contract_mask = NY_FX_NONE;
  }
  if (decl->attr_constant_time) {

    decl->attr_noinline = false;
    decl->attr_hot = true;
  }
  if (decl->attr_accel) {
    decl->attr_hot = true;
    if (!decl->attr_accel_target) {
      const char *env_target = getenv("NYTRIX_ACCEL_TARGET");
      decl->attr_accel_target =
          codegen_strdup(cg, (env_target && *env_target) ? env_target : "auto");
    }
  }
  if (decl->attr_tailcall)
    decl->attr_hot = true;
  bool has_ownership_contract =
      decl->attr_returns_owned || decl->attr_returns_borrow ||
      decl->attr_borrows.len != 0 || decl->attr_consumes.len != 0 ||
      decl->attr_mutates.len != 0 || decl->attr_releases.len != 0 ||
      decl->attr_forgets.len != 0 || decl->attr_allowances.len != 0;
  const char *warn_ownership_contracts =
      getenv("NYTRIX_WARN_OWNERSHIP_CONTRACTS");
  if (has_ownership_contract && !cg->ownership_enabled &&
      warn_ownership_contracts && *warn_ownership_contracts &&
      strcmp(warn_ownership_contracts, "0") != 0 &&
      !ny_is_stdlib_tok(fn_stmt->tok)) {
    ny_diag_warning(fn_stmt->tok,
                    "ownership contract attributes are parsed but not enforced "
                    "without --borrow-check or --ownership");
  }
  decl->attrs_resolved = true;
}

static size_t count_stmts_in_body(const stmt_t *body) {
  if (!body)
    return 0;

  size_t count = 0;

  if (body->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < body->as.block.body.len; i++) {
      const stmt_t *s = body->as.block.body.data[i];
      if (s->kind == NY_S_EXPR || s->kind == NY_S_VAR || s->kind == NY_S_EXPR ||
          s->kind == NY_S_RETURN) {
        count++;
      } else if (s->kind == NY_S_IF) {
        count += 2;
        if (s->as.iff.conseq)
          count += count_stmts_in_body(s->as.iff.conseq);
        if (s->as.iff.alt)
          count += count_stmts_in_body(s->as.iff.alt);
      } else if (s->kind == NY_S_WHILE || s->kind == NY_S_FOR) {
        count += 3;
      }
    }
  } else {
    count = 1;
  }

  return count;
}

static void stmt_scan_body_summary(const stmt_t *s, bool *has_try,
                                   bool *has_label_or_goto) {
  if (!s)
    return;
  if (*has_try && *has_label_or_goto)
    return;
  switch (s->kind) {
  case NY_S_TRY:
    *has_try = true;
    stmt_scan_body_summary(s->as.tr.body, has_try, has_label_or_goto);
    stmt_scan_body_summary(s->as.tr.handler, has_try, has_label_or_goto);
    break;
  case NY_S_LABEL:
  case NY_S_GOTO:
    *has_label_or_goto = true;
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      stmt_scan_body_summary(s->as.block.body.data[i], has_try,
                             has_label_or_goto);
      if (*has_try && *has_label_or_goto)
        return;
    }
    break;
  case NY_S_IF:
    stmt_scan_body_summary(s->as.iff.init, has_try, has_label_or_goto);
    stmt_scan_body_summary(s->as.iff.conseq, has_try, has_label_or_goto);
    stmt_scan_body_summary(s->as.iff.alt, has_try, has_label_or_goto);
    break;
  case NY_S_WHILE:
    stmt_scan_body_summary(s->as.whl.init, has_try, has_label_or_goto);
    stmt_scan_body_summary(s->as.whl.body, has_try, has_label_or_goto);
    stmt_scan_body_summary(s->as.whl.update, has_try, has_label_or_goto);
    break;
  case NY_S_FOR:
    stmt_scan_body_summary(s->as.fr.init, has_try, has_label_or_goto);
    stmt_scan_body_summary(s->as.fr.body, has_try, has_label_or_goto);
    stmt_scan_body_summary(s->as.fr.update, has_try, has_label_or_goto);
    break;
  case NY_S_GUARD:
    stmt_scan_body_summary(s->as.guard.fallback, has_try, has_label_or_goto);
    break;
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      stmt_scan_body_summary(s->as.match.arms.data[i].conseq, has_try,
                             has_label_or_goto);
      if (*has_try && *has_label_or_goto)
        return;
    }
    stmt_scan_body_summary(s->as.match.default_conseq, has_try,
                           has_label_or_goto);
    break;
  case NY_S_LEMMA:
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      stmt_scan_body_summary(s->as.module.body.data[i], has_try,
                             has_label_or_goto);
      if (*has_try && *has_label_or_goto)
        return;
    }
    break;
  case NY_S_DEFER:
    stmt_scan_body_summary(s->as.de.body, has_try, has_label_or_goto);
    break;
  case NY_S_MACRO:
    stmt_scan_body_summary(s->as.macro.body, has_try, has_label_or_goto);
    break;
  case NY_S_FUNC:
  case NY_S_EXTERN:
    break;
  default:
    break;
  }
}

static void stmt_func_body_summary(stmt_t *fn_stmt, bool *has_try,
                                   bool *has_label_or_goto) {
  bool local_has_try = false;
  bool local_has_label_or_goto = false;
  if (fn_stmt && fn_stmt->kind == NY_S_FUNC) {
    stmt_func_t *fn = &fn_stmt->as.fn;
    if (!fn->body_summary_known) {
      stmt_scan_body_summary(fn->body, &fn->body_has_try,
                             &fn->body_has_label_or_goto);
      fn->body_summary_known = true;
    }
    local_has_try = fn->body_has_try;
    local_has_label_or_goto = fn->body_has_label_or_goto;
  }
  if (has_try)
    *has_try = local_has_try;
  if (has_label_or_goto)
    *has_label_or_goto = local_has_label_or_goto;
}

bool ny_stmt_func_body_has_try(stmt_t *fn_stmt) {
  bool has_try = false;
  stmt_func_body_summary(fn_stmt, &has_try, NULL);
  return has_try;
}

static bool stmt_func_body_has_label_or_goto(stmt_t *fn_stmt) {
  bool has_label_or_goto = false;
  stmt_func_body_summary(fn_stmt, NULL, &has_label_or_goto);
  return has_label_or_goto;
}

void ny_apply_decl_fn_attrs(codegen_t *cg, LLVMValueRef fn, stmt_t *fn_stmt) {
  if (!cg || !fn || !fn_stmt)
    return;

  stmt_func_t *decl = &fn_stmt->as.fn;
  bool has_try = ny_stmt_func_body_has_try(fn_stmt);

  ny_apply_rt_fn_attrs(cg, fn);

  if (cg->opt_inline_small && !has_try && !decl->attr_noinline) {
    size_t stmt_count = count_stmts_in_body(decl->body);
    if (stmt_count < 8 || decl->attr_inline || decl->attr_flatten)
      add_fn_enum_attr(cg, fn, "alwaysinline", 0);
  }

  if (decl->attr_naked)
    add_fn_enum_attr(cg, fn, "naked", 0);
  if (decl->attr_jit) {
    if (!has_try)
      add_fn_enum_attr(cg, fn, "alwaysinline", 0);
    add_fn_enum_attr(cg, fn, "hot", 0);
  }
  if (decl->attr_thread) {
    add_fn_enum_attr(cg, fn, "noinline", 0);
    add_fn_enum_attr(cg, fn, "cold", 0);
  }
  if (decl->attr_inline && decl->attr_noinline) {
    ny_diag_error(fn_stmt->tok,
                  "conflicting attributes '@inline' and '@noinline'");
    cg->had_error = 1;
  } else if (decl->attr_inline && !has_try) {
    add_fn_enum_attr(cg, fn, "alwaysinline", 0);
  } else if (decl->attr_noinline) {
    add_fn_enum_attr(cg, fn, "noinline", 0);
  }

  if (decl->attr_nounwind && !has_try)
    add_fn_enum_attr(cg, fn, "nounwind", 0);
  if (decl->attr_mustprogress && !has_try)
    add_fn_enum_attr(cg, fn, "mustprogress", 0);
  if (decl->attr_willreturn && !has_try)
    add_fn_enum_attr(cg, fn, "willreturn", 0);
  if (decl->attr_cold)
    add_fn_enum_attr(cg, fn, "cold", 0);
  if (decl->attr_hot)
    add_fn_enum_attr(cg, fn, "hot", 0);
  if (decl->attr_flatten && !has_try)
    add_fn_enum_attr(cg, fn, "flatten", 0);
  if (decl->attr_tailcall) {
    add_fn_string_attr(cg, fn, "disable-tail-calls", "false");
    add_fn_enum_attr(cg, fn, "hot", 0);
  }
  if (decl->attr_consteval)
    add_fn_enum_attr(cg, fn, "alwaysinline", 0);
  if (decl->attr_constant_time)
    add_fn_string_attr(cg, fn, "target-features", "+cmov");
  if (decl->attr_optimize) {
    char level[2] = {(char)('0' + decl->attr_optimize_level), '\0'};
    add_fn_string_attr(cg, fn, "nytrix.optimize.level", level);
    if (decl->attr_optimize_level == 0)
      add_fn_enum_attr(cg, fn, "optnone", 0);
  }
  if (decl->attr_accel) {
    add_fn_string_attr(cg, fn, "nytrix.accel", "true");
    add_fn_string_attr(cg, fn, "nytrix.accel.target",
                       decl->attr_accel_target ? decl->attr_accel_target
                                               : "auto");
    add_fn_string_attr(cg, fn, "min-legal-vector-width", "128");
    add_fn_enum_attr(cg, fn, "hot", 0);
  }
  for (size_t i = 0; i < fn_stmt->attributes.len; i++) {
    const attribute_t *attr = &fn_stmt->attributes.data[i];
    if (!attr_name_eq(attr, "backend"))
      continue;
    if (attr->args.len < 1 || attr->args.len > 2) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                    "@backend expects 1 or 2 arguments");
      ny_diag_hint("use @backend(name) or @backend(name, value)");
      cg->had_error = 1;
      continue;
    }
    const char *attr_name = NULL;
    size_t attr_name_len = 0;
    if (!attr_arg_text_view(attr->args.data[0], &attr_name, &attr_name_len) ||
        attr_name_len == 0) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                    "@backend first argument must be an identifier or string");
      cg->had_error = 1;
      continue;
    }
    if (attr->args.len == 1) {
      unsigned kind_id =
          LLVMGetEnumAttributeKindForName(attr_name, (unsigned)attr_name_len);
      if (kind_id != 0) {
        LLVMAttributeRef backend_attr =
            LLVMCreateEnumAttribute(cg->ctx, kind_id, 0);
        LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, backend_attr);
      } else {
        char *name_owned = ny_strndup(attr_name, attr_name_len);
        add_fn_string_attr(cg, fn, name_owned, "");
        free(name_owned);
      }
      continue;
    }
    const char *attr_value = NULL;
    size_t attr_value_len = 0;
    if (!attr_arg_text_view(attr->args.data[1], &attr_value, &attr_value_len)) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, 1),
                    "@backend second argument must be an identifier or string");
      cg->had_error = 1;
      continue;
    }
    char *name_owned = ny_strndup(attr_name, attr_name_len);
    char *value_owned = ny_strndup(attr_value, attr_value_len);
    add_fn_string_attr(cg, fn, name_owned, value_owned);
    free(name_owned);
    free(value_owned);
  }
}

/*
 * Instantiate a deftype layout at an explicit comptime-int bound (Buf<16>).
 * Creates a cached layout_def whose deftype_param_vals bind the first param to
 * `bound`, so array extents `[u8, n]` resolve to the bound instead of the
 * param default.
 */

static void collect_assigned_names_expr(expr_t *e,
                                        assigned_name_list *out_names,
                                        assigned_hash_list *out_hashes,
                                        uint64_t out_bloom[4]);

static void collect_assigned_names_stmt(stmt_t *s,
                                        assigned_name_list *out_names,
                                        assigned_hash_list *out_hashes,
                                        uint64_t out_bloom[4]);

static void collect_assigned_names_expr_list(const ny_expr_list *exprs,
                                             assigned_name_list *out_names,
                                             assigned_hash_list *out_hashes,
                                             uint64_t out_bloom[4]) {
  if (!exprs)
    return;
  for (size_t i = 0; i < exprs->len; i++)
    collect_assigned_names_expr(exprs->data[i], out_names, out_hashes,
                                out_bloom);
}

static void collect_assigned_names_call_args(const ny_call_arg_list *args,
                                             assigned_name_list *out_names,
                                             assigned_hash_list *out_hashes,
                                             uint64_t out_bloom[4]) {
  if (!args)
    return;
  for (size_t i = 0; i < args->len; i++)
    collect_assigned_names_expr(args->data[i].val, out_names, out_hashes,
                                out_bloom);
}

static void collect_assigned_names_match_arms(const ny_match_arm_list *arms,
                                              assigned_name_list *out_names,
                                              assigned_hash_list *out_hashes,
                                              uint64_t out_bloom[4]) {
  if (!arms)
    return;
  for (size_t i = 0; i < arms->len; i++) {
    for (size_t j = 0; j < arms->data[i].patterns.len; j++)
      collect_assigned_names_expr(arms->data[i].patterns.data[j], out_names,
                                  out_hashes, out_bloom);
    collect_assigned_names_expr(arms->data[i].guard, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(arms->data[i].conseq, out_names, out_hashes,
                                out_bloom);
  }
}

static void collect_assigned_names_stmt_list(const ny_stmt_list *stmts,
                                             assigned_name_list *out_names,
                                             assigned_hash_list *out_hashes,
                                             uint64_t out_bloom[4]) {
  if (!stmts)
    return;
  for (size_t i = 0; i < stmts->len; i++)
    collect_assigned_names_stmt(stmts->data[i], out_names, out_hashes,
                                out_bloom);
}

static void collect_assigned_names_expr(expr_t *e,
                                        assigned_name_list *out_names,
                                        assigned_hash_list *out_hashes,
                                        uint64_t out_bloom[4]) {
  if (!e || !out_names || !out_hashes)
    return;
  switch (e->kind) {
  case NY_E_UNARY:
    collect_assigned_names_expr(e->as.unary.right, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    collect_assigned_names_expr(e->as.binary.left, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.binary.right, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_TERNARY:
    collect_assigned_names_expr(e->as.ternary.cond, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.ternary.true_expr, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.ternary.false_expr, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_CALL: {
    /*
     * addr_of(local) / borrow(local) require an addressable stack slot.
     * Record the target so can_bind_decl_direct keeps it in a slot even
     * when the local is never reassigned (which would otherwise bind it
     * straight to an SSA register with no address to take).
     */
    if (e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
        e->as.call.args.data[0].val &&
        e->as.call.args.data[0].val->kind == NY_E_IDENT &&
        e->as.call.args.data[0].val->as.ident.name) {
      const char *callee_name =
          e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
              ? e->as.call.callee->as.ident.name
              : NULL;
      const char *leaf = callee_name ? strrchr(callee_name, '.') : NULL;
      leaf = leaf ? leaf + 1 : callee_name;
      if (leaf && (strcmp(leaf, "addr_of") == 0 || strcmp(leaf, "borrow") == 0))
        assigned_name_add(out_names, out_hashes, out_bloom,
                          e->as.call.args.data[0].val->as.ident.name);
    }
    collect_assigned_names_expr(e->as.call.callee, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_call_args(&e->as.call.args, out_names, out_hashes,
                                     out_bloom);
    break;
  }
  case NY_E_MEMCALL:
    collect_assigned_names_expr(e->as.memcall.target, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_call_args(&e->as.memcall.args, out_names, out_hashes,
                                     out_bloom);
    break;
  case NY_E_INDEX:
    collect_assigned_names_expr(e->as.index.target, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.index.start, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.index.stop, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.index.step, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_MEMBER:
    collect_assigned_names_expr(e->as.member.target, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_PTR_TYPE:
    collect_assigned_names_expr(e->as.ptr_type.target, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_DEREF:
    collect_assigned_names_expr(e->as.deref.target, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_SIZEOF:
    collect_assigned_names_expr(e->as.szof.target, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_TRY:
    collect_assigned_names_expr(e->as.try_expr.target, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_LAMBDA:
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    collect_assigned_names_expr_list(&e->as.list_like, out_names, out_hashes,
                                     out_bloom);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      collect_assigned_names_expr(e->as.dict.pairs.data[i].key, out_names,
                                  out_hashes, out_bloom);
      collect_assigned_names_expr(e->as.dict.pairs.data[i].value, out_names,
                                  out_hashes, out_bloom);
    }
    break;
  case NY_E_COMPTIME:
    collect_assigned_names_stmt(e->as.comptime_expr.body, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR)
        collect_assigned_names_expr(part->as.e, out_names, out_hashes,
                                    out_bloom);
    }
    break;
  case NY_E_MATCH:
    collect_assigned_names_expr(e->as.match.test, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_match_arms(&e->as.match.arms, out_names, out_hashes,
                                      out_bloom);
    collect_assigned_names_stmt(e->as.match.default_conseq, out_names,
                                out_hashes, out_bloom);
    break;
  default:
    break;
  }
}

static void collect_assigned_names_stmt(stmt_t *s,
                                        assigned_name_list *out_names,
                                        assigned_hash_list *out_hashes,
                                        uint64_t out_bloom[4]) {
  if (!s || !out_names || !out_hashes)
    return;
  switch (s->kind) {
  case NY_S_BLOCK:
    collect_assigned_names_stmt_list(&s->as.block.body, out_names, out_hashes,
                                     out_bloom);
    break;
  case NY_S_VAR:
    if (!s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++)
        assigned_name_add(out_names, out_hashes, out_bloom,
                          s->as.var.names.data[i]);
    }
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      collect_assigned_names_expr(s->as.var.exprs.data[i], out_names,
                                  out_hashes, out_bloom);
    break;
  case NY_S_EXPR:
    collect_assigned_names_expr(s->as.expr.expr, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_S_IF:
    collect_assigned_names_expr(s->as.iff.test, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(s->as.iff.conseq, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(s->as.iff.alt, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_S_WHILE:
    collect_assigned_names_expr(s->as.whl.test, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(s->as.whl.body, out_names, out_hashes,
                                out_bloom);
    if (s->as.whl.update)
      collect_assigned_names_stmt(s->as.whl.update, out_names, out_hashes,
                                  out_bloom);
    if (s->as.whl.init)
      collect_assigned_names_stmt(s->as.whl.init, out_names, out_hashes,
                                  out_bloom);
    break;
  case NY_S_FOR:
    if (s->as.fr.init)
      collect_assigned_names_stmt(s->as.fr.init, out_names, out_hashes,
                                  out_bloom);
    if (s->as.fr.cond)
      collect_assigned_names_expr(s->as.fr.cond, out_names, out_hashes,
                                  out_bloom);
    if (s->as.fr.iterable)
      collect_assigned_names_expr(s->as.fr.iterable, out_names, out_hashes,
                                  out_bloom);
    collect_assigned_names_stmt(s->as.fr.body, out_names, out_hashes,
                                out_bloom);
    if (s->as.fr.update)
      collect_assigned_names_stmt(s->as.fr.update, out_names, out_hashes,
                                  out_bloom);
    break;
  case NY_S_TRY:
    collect_assigned_names_stmt(s->as.tr.body, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(s->as.tr.handler, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_S_DEFER:
    collect_assigned_names_stmt(s->as.de.body, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_S_MATCH:
    collect_assigned_names_expr(s->as.match.test, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_match_arms(&s->as.match.arms, out_names, out_hashes,
                                      out_bloom);
    collect_assigned_names_stmt(s->as.match.default_conseq, out_names,
                                out_hashes, out_bloom);
    break;
  case NY_S_LEMMA:
    collect_assigned_names_expr(s->as.lemma.proposition, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_S_FUNC:
    break;
  case NY_S_MODULE:
    collect_assigned_names_stmt_list(&s->as.module.body, out_names, out_hashes,
                                     out_bloom);
    break;
  case NY_S_MACRO:
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      collect_assigned_names_expr(s->as.macro.args.data[i], out_names,
                                  out_hashes, out_bloom);
    }
    collect_assigned_names_stmt(s->as.macro.body, out_names, out_hashes,
                                out_bloom);
    break;
  default:
    break;
  }
}

typedef struct type_scratch_t {
  char **owned;
  size_t len;
  size_t cap;
} type_scratch_t;

typedef struct {
  const char **names;
  const char **types;
  int count;
  type_scratch_t *scratch;
} type_env_t;

#define NY_FUNC_TYPE_ENV_MAX 64

static const char *type_scratch_take(type_env_t *env, char *s) {
  if (!s)
    return NULL;
  if (!env || !env->scratch) {
    free(s);
    return NULL;
  }
  type_scratch_t *scratch = env->scratch;
  if (scratch->len == scratch->cap) {
    size_t next_cap = scratch->cap ? scratch->cap * 2 : 16;
    char **grown = realloc(scratch->owned, next_cap * sizeof(*scratch->owned));
    if (!grown) {
      free(s);
      return NULL;
    }
    scratch->owned = grown;
    scratch->cap = next_cap;
  }
  scratch->owned[scratch->len++] = s;
  return s;
}

static void type_scratch_free(type_scratch_t *scratch) {
  if (!scratch)
    return;
  for (size_t i = 0; i < scratch->len; i++)
    free(scratch->owned[i]);
  free(scratch->owned);
  scratch->owned = NULL;
  scratch->len = 0;
  scratch->cap = 0;
}

static const char *env_lookup(const type_env_t *env, const char *name) {
  if (!env || !name)
    return NULL;
  for (int i = 0; i < env->count; i++)
    if (env->names[i] && env->types[i] && strcmp(name, env->names[i]) == 0)
      return env->types[i];
  return NULL;
}

static void env_push_type(type_env_t *env, const char *name, const char *type) {
  if (!env || !name || !*name || !type || !*type)
    return;
  for (int i = env->count; i > 0; --i) {
    if (env->names[i - 1] && strcmp(env->names[i - 1], name) == 0) {
      env->types[i - 1] = type;
      return;
    }
  }
  if (env->count >= NY_FUNC_TYPE_ENV_MAX)
    return;
  env->names[env->count] = name;
  env->types[env->count] = type;
  env->count++;
}

static const char *func_type_make_result(type_env_t *env, const char *ok,
                                         const char *err) {
  if (!ok || !*ok)
    ok = "any";
  if (!err || !*err)
    err = "any";
  size_t ok_len = strlen(ok);
  size_t err_len = strlen(err);
  char *out = malloc(ok_len + err_len + 11);
  if (!out)
    return "Result";
  snprintf(out, ok_len + err_len + 11, "Result<%s, %s>", ok, err);
  return type_scratch_take(env, out);
}

static const char *func_type_merge_result_arg(const char *a, const char *b) {
  if (!a || !*a || strcmp(a, "any") == 0)
    return (b && *b) ? b : "any";
  if (!b || !*b || strcmp(b, "any") == 0)
    return a;
  if (strcmp(a, b) == 0)
    return a;
  return "any";
}

static const char *ast_infer_type(expr_t *e, const type_env_t *env) {
  if (!e)
    return NULL;
  if (e->kind == NY_E_LITERAL) {
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return "f64";
    if (e->as.literal.kind == NY_LIT_INT)
      return e->tok.kind == NY_T_NIL ? "nil" : "int";
    return NULL;
  }
  if (e->kind == NY_E_IDENT)
    return env_lookup(env, e->as.ident.name);
  if (e->kind == NY_E_UNARY) {
    if (e->as.unary.op && strcmp(e->as.unary.op, "async") == 0)
      return "handle";
    if (e->as.unary.op && strcmp(e->as.unary.op, "await") == 0)
      return "any";
    return ast_infer_type(e->as.unary.right, env);
  }
  if (e->kind == NY_E_BINARY) {
    const char *lt = ast_infer_type(e->as.binary.left, env);
    const char *rt = ast_infer_type(e->as.binary.right, env);
    bool lf = lt && strcmp(lt, "f64") == 0;
    bool rf = rt && strcmp(rt, "f64") == 0;
    if (lf || rf)
      return "f64";
    bool li = lt && strcmp(lt, "int") == 0;
    bool ri = rt && strcmp(rt, "int") == 0;
    if (li && ri)
      return "int";
    return NULL;
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const char *name = e->as.call.callee->as.ident.name;
    const char *leaf = ny_name_leaf(name);
    if (leaf && e->as.call.args.len == 1 &&
        (strcmp(leaf, "ok") == 0 || strcmp(leaf, "__result_ok") == 0)) {
      const char *ok = ast_infer_type(e->as.call.args.data[0].val, env);
      return func_type_make_result((type_env_t *)env, ok ? ok : "any", "any");
    }
    if (leaf && e->as.call.args.len == 1 &&
        (strcmp(leaf, "err") == 0 || strcmp(leaf, "__result_err") == 0)) {
      const char *err = ast_infer_type(e->as.call.args.data[0].val, env);
      return func_type_make_result((type_env_t *)env, "any", err ? err : "any");
    }
    if (leaf && e->as.call.args.len == 1 &&
        (strcmp(leaf, "unwrap") == 0 || strcmp(leaf, "__unwrap") == 0)) {
      const char *result = ast_infer_type(e->as.call.args.data[0].val, env);
      if (ny_generic_type_base_is(result, "Result")) {
        char *ok = ny_generic_type_arg_owned(result, 0);
        return ok ? type_scratch_take((type_env_t *)env, ok) : "any";
      }
    }
  }
  return NULL;
}

static void mark_expr_params(expr_t *e, type_env_t *env,
                             const char **param_names, const char **param_types,
                             int nparam) {
  if (!e)
    return;
  if (e->kind == NY_E_BINARY) {
    const char *lt = ast_infer_type(e->as.binary.left, env);
    const char *rt = ast_infer_type(e->as.binary.right, env);
    bool lf = lt && strcmp(lt, "f64") == 0;
    bool rf = rt && strcmp(rt, "f64") == 0;
    const char *op = e->as.binary.op;
    bool is_arith = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                    strcmp(op, "*") == 0 || strcmp(op, "/") == 0;
    bool li = lt && strcmp(lt, "int") == 0;
    bool ri = rt && strcmp(rt, "int") == 0;
    bool is_bitwise = strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
                      strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 ||
                      strcmp(op, ">>") == 0;

    bool is_cmp = strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                  strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                  strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
    bool is_mod = strcmp(op, "%") == 0;
    const char *inferred = NULL;
    if ((lf || rf) && is_arith)
      inferred = "f64";
    else if ((li || ri) && (is_bitwise || is_arith || is_cmp || is_mod) &&
             !(lf || rf))
      inferred = "int";

    if (inferred) {
      for (int side = 0; side < 2; side++) {
        expr_t *other = side == 0 ? e->as.binary.right : e->as.binary.left;
        if (other && other->kind == NY_E_IDENT) {
          const char *oname = other->as.ident.name;
          for (int i = 0; i < nparam; i++) {
            if (param_names[i] && strcmp(oname, param_names[i]) == 0) {
              if (!param_types[i])
                param_types[i] = inferred;
              break;
            }
          }

          bool found = false;
          for (int k = 0; k < env->count; k++) {
            if (strcmp(env->names[k], oname) == 0) {
              if (!env->types[k])
                env->types[k] = inferred;
              found = true;
              break;
            }
          }
          if (!found && env->count < 64) {
            env->names[env->count] = oname;
            env->types[env->count] = inferred;
            env->count++;
          }
        }
      }
    }
    mark_expr_params(e->as.binary.left, env, param_names, param_types, nparam);
    mark_expr_params(e->as.binary.right, env, param_names, param_types, nparam);
    return;
  }
  if (e->kind == NY_E_LOGICAL) {
    mark_expr_params(e->as.logical.left, env, param_names, param_types, nparam);
    mark_expr_params(e->as.logical.right, env, param_names, param_types,
                     nparam);
    return;
  }
  if (e->kind == NY_E_UNARY) {
    mark_expr_params(e->as.unary.right, env, param_names, param_types, nparam);
    return;
  }
  if (e->kind == NY_E_CALL) {
    for (size_t i = 0; i < e->as.call.args.len; i++)
      mark_expr_params(e->as.call.args.data[i].val, env, param_names,
                       param_types, nparam);
    return;
  }
}

static void scan_body_for_param_types(stmt_t *body, type_env_t *env,
                                      const char **param_names,
                                      const char **param_types, int nparam) {
  if (!body)
    return;
  if (body->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < body->as.block.body.len; i++)
      scan_body_for_param_types(body->as.block.body.data[i], env, param_names,
                                param_types, nparam);
    return;
  }
  if (body->kind == NY_S_VAR) {
    for (size_t i = 0; i < body->as.var.names.len; i++) {
      expr_t *init =
          (i < body->as.var.exprs.len) ? body->as.var.exprs.data[i] : NULL;
      if (init) {
        mark_expr_params(init, env, param_names, param_types, nparam);
        const char *t = ast_infer_type(init, env);

        for (int j = 0; j < nparam; j++) {
          if (param_names[j] &&
              strcmp(body->as.var.names.data[i], param_names[j]) == 0) {
            if (t) {
              if (strcmp(t, "f64") == 0)
                param_types[j] = "f64";
              else if (!param_types[j])
                param_types[j] = t;
            }
          }
        }

        if (t && env->count < 64) {
          bool found = false;
          for (int k = 0; k < env->count; k++) {
            if (strcmp(env->names[k], body->as.var.names.data[i]) == 0) {
              env->types[k] = t;
              found = true;
              break;
            }
          }
          if (!found) {
            env->names[env->count] = body->as.var.names.data[i];
            env->types[env->count] = t;
            env->count++;
          }
        }

        if (init->kind == NY_E_IDENT) {
          const char *lhs_type = NULL;
          for (int k = 0; k < env->count; k++) {
            if (strcmp(env->names[k], body->as.var.names.data[i]) == 0) {
              lhs_type = env->types[k];
              break;
            }
          }
          if (lhs_type) {
            for (int j = 0; j < nparam; j++) {
              if (param_names[j] &&
                  strcmp(init->as.ident.name, param_names[j]) == 0) {
                if (!param_types[j])
                  param_types[j] = lhs_type;
              }
            }
          }
        }
      }
    }
    return;
  }
  if (body->kind == NY_S_EXPR) {
    mark_expr_params(body->as.expr.expr, env, param_names, param_types, nparam);
    return;
  }
  if (body->kind == NY_S_WHILE) {
    mark_expr_params(body->as.whl.test, env, param_names, param_types, nparam);
    scan_body_for_param_types(body->as.whl.body, env, param_names, param_types,
                              nparam);
    if (body->as.whl.update)
      scan_body_for_param_types(body->as.whl.update, env, param_names,
                                param_types, nparam);
    if (body->as.whl.init)
      scan_body_for_param_types(body->as.whl.init, env, param_names,
                                param_types, nparam);
    return;
  }
  if (body->kind == NY_S_IF) {
    mark_expr_params(body->as.iff.test, env, param_names, param_types, nparam);
    scan_body_for_param_types(body->as.iff.conseq, env, param_names,
                              param_types, nparam);
    scan_body_for_param_types(body->as.iff.alt, env, param_names, param_types,
                              nparam);
    return;
  }
  if (body->kind == NY_S_FOR) {
    if (body->as.fr.init)
      scan_body_for_param_types(body->as.fr.init, env, param_names, param_types,
                                nparam);
    if (body->as.fr.cond)
      mark_expr_params(body->as.fr.cond, env, param_names, param_types, nparam);
    if (body->as.fr.iterable)
      mark_expr_params(body->as.fr.iterable, env, param_names, param_types,
                       nparam);
    scan_body_for_param_types(body->as.fr.body, env, param_names, param_types,
                              nparam);
    if (body->as.fr.update)
      scan_body_for_param_types(body->as.fr.update, env, param_names,
                                param_types, nparam);
    return;
  }
  if (body->kind == NY_S_TRY) {
    scan_body_for_param_types(body->as.tr.body, env, param_names, param_types,
                              nparam);
    scan_body_for_param_types(body->as.tr.handler, env, param_names,
                              param_types, nparam);
    return;
  }
  if (body->kind == NY_S_DEFER) {
    scan_body_for_param_types(body->as.de.body, env, param_names, param_types,
                              nparam);
    return;
  }
  if (body->kind == NY_S_MATCH) {
    mark_expr_params(body->as.match.test, env, param_names, param_types,
                     nparam);
    for (size_t i = 0; i < body->as.match.arms.len; i++)
      scan_body_for_param_types(body->as.match.arms.data[i].conseq, env,
                                param_names, param_types, nparam);
    if (body->as.match.default_conseq)
      scan_body_for_param_types(body->as.match.default_conseq, env, param_names,
                                param_types, nparam);
    return;
  }
  if (body->kind == NY_S_LEMMA) {
    mark_expr_params(body->as.lemma.proposition, env, param_names, param_types,
                     nparam);
    return;
  }
  if (body->kind == NY_S_RETURN) {
    mark_expr_params(body->as.ret.value, env, param_names, param_types, nparam);
    return;
  }
}

static void scan_float_usage_expr(expr_t *e, const char **pnames,
                                  bool *used_float, int np) {
  if (!e)
    return;
  if (e->kind == NY_E_CALL) {

    expr_t *callee = e->as.call.callee;
    bool is_float_fn = false;
    if (callee && callee->kind == NY_E_IDENT) {
      const char *fn_name = callee->as.ident.name;
      is_float_fn =
          (strcmp(fn_name, "float") == 0 || strcmp(fn_name, "to_float") == 0 ||
           strcmp(fn_name, "store32_f32") == 0 ||
           strcmp(fn_name, "store64_f64") == 0 ||
           strcmp(fn_name, "is_int") == 0 || strcmp(fn_name, "is_float") == 0 ||
           strcmp(fn_name, "is_str") == 0 || strcmp(fn_name, "is_dict") == 0 ||
           strcmp(fn_name, "is_list") == 0 || strcmp(fn_name, "type_of") == 0);
    }
    if (is_float_fn) {
      for (size_t a = 0; a < e->as.call.args.len; a++) {
        expr_t *arg = e->as.call.args.data[a].val;
        if (arg && arg->kind == NY_E_IDENT) {
          for (int i = 0; i < np; i++) {
            if (pnames[i] && strcmp(arg->as.ident.name, pnames[i]) == 0)
              used_float[i] = true;
          }
        }
      }
    }
    for (size_t a = 0; a < e->as.call.args.len; a++)
      scan_float_usage_expr(e->as.call.args.data[a].val, pnames, used_float,
                            np);
    return;
  }
  if (e->kind == NY_E_BINARY) {
    scan_float_usage_expr(e->as.binary.left, pnames, used_float, np);
    scan_float_usage_expr(e->as.binary.right, pnames, used_float, np);
    return;
  }
  if (e->kind == NY_E_LOGICAL) {
    scan_float_usage_expr(e->as.logical.left, pnames, used_float, np);
    scan_float_usage_expr(e->as.logical.right, pnames, used_float, np);
    return;
  }
  if (e->kind == NY_E_UNARY) {
    scan_float_usage_expr(e->as.unary.right, pnames, used_float, np);
    return;
  }
  if (e->kind == NY_E_TERNARY) {
    scan_float_usage_expr(e->as.ternary.cond, pnames, used_float, np);
    scan_float_usage_expr(e->as.ternary.true_expr, pnames, used_float, np);
    scan_float_usage_expr(e->as.ternary.false_expr, pnames, used_float, np);
    return;
  }
}

static void scan_float_usage(stmt_t *s, const char **pnames, bool *used_float,
                             int np) {
  if (!s)
    return;
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      scan_float_usage(s->as.block.body.data[i], pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_EXPR) {
    scan_float_usage_expr(s->as.expr.expr, pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_VAR) {
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      scan_float_usage_expr(s->as.var.exprs.data[i], pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_RETURN) {
    scan_float_usage_expr(s->as.ret.value, pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_IF) {
    scan_float_usage_expr(s->as.iff.test, pnames, used_float, np);
    scan_float_usage(s->as.iff.conseq, pnames, used_float, np);
    scan_float_usage(s->as.iff.alt, pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_WHILE) {
    scan_float_usage_expr(s->as.whl.test, pnames, used_float, np);
    scan_float_usage(s->as.whl.body, pnames, used_float, np);
    if (s->as.whl.update)
      scan_float_usage(s->as.whl.update, pnames, used_float, np);
    if (s->as.whl.init)
      scan_float_usage(s->as.whl.init, pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_FOR) {
    if (s->as.fr.init)
      scan_float_usage(s->as.fr.init, pnames, used_float, np);
    if (s->as.fr.cond)
      scan_float_usage_expr(s->as.fr.cond, pnames, used_float, np);
    if (s->as.fr.iterable)
      scan_float_usage_expr(s->as.fr.iterable, pnames, used_float, np);
    scan_float_usage(s->as.fr.body, pnames, used_float, np);
    if (s->as.fr.update)
      scan_float_usage(s->as.fr.update, pnames, used_float, np);
    return;
  }
}

static bool is_numeric_fn(const char *fn_name) {
  if (!fn_name)
    return false;

  static const char *numeric_fns[] = {"abs",
                                      "sqrt",
                                      "sin",
                                      "cos",
                                      "tan",
                                      "floor",
                                      "ceil",
                                      "round",
                                      "min",
                                      "max",
                                      "pow",
                                      "log",
                                      "log2",
                                      "log10",
                                      "exp",
                                      "float",
                                      "to_float",
                                      "int",
                                      "to_int",
                                      "clamp",
                                      "fib_naive",
                                      "fib_linear",
                                      "fib_fast_doubling",
                                      "fib_matrix",
                                      "fib_fast_squaring",
                                      NULL};
  for (const char **p = numeric_fns; *p; p++)
    if (strcmp(fn_name, *p) == 0)
      return true;
  return false;
}

static void scan_poly_usage_expr(expr_t *e, const char **pnames, bool *poly,
                                 int np) {
  if (!e)
    return;
  if (e->kind == NY_E_CALL) {
    expr_t *callee = e->as.call.callee;
    bool is_numeric = false;
    if (callee && callee->kind == NY_E_IDENT)
      is_numeric = is_numeric_fn(callee->as.ident.name);

    if (!is_numeric && callee) {
      for (size_t a = 0; a < e->as.call.args.len; a++) {
        expr_t *arg = e->as.call.args.data[a].val;
        if (arg && arg->kind == NY_E_IDENT) {
          for (int i = 0; i < np; i++) {
            if (pnames[i] && strcmp(arg->as.ident.name, pnames[i]) == 0)
              poly[i] = true;
          }
        }
      }
    }
    for (size_t a = 0; a < e->as.call.args.len; a++)
      scan_poly_usage_expr(e->as.call.args.data[a].val, pnames, poly, np);
    return;
  }
  if (e->kind == NY_E_BINARY) {
    scan_poly_usage_expr(e->as.binary.left, pnames, poly, np);
    scan_poly_usage_expr(e->as.binary.right, pnames, poly, np);
    return;
  }
  if (e->kind == NY_E_LOGICAL) {
    scan_poly_usage_expr(e->as.logical.left, pnames, poly, np);
    scan_poly_usage_expr(e->as.logical.right, pnames, poly, np);
    return;
  }
  if (e->kind == NY_E_UNARY) {
    scan_poly_usage_expr(e->as.unary.right, pnames, poly, np);
    return;
  }
  if (e->kind == NY_E_TERNARY) {
    scan_poly_usage_expr(e->as.ternary.cond, pnames, poly, np);
    scan_poly_usage_expr(e->as.ternary.true_expr, pnames, poly, np);
    scan_poly_usage_expr(e->as.ternary.false_expr, pnames, poly, np);
    return;
  }

  if (e->kind == NY_E_INDEX) {
    expr_t *obj = e->as.index.target;
    if (obj && obj->kind == NY_E_IDENT) {
      for (int i = 0; i < np; i++) {
        if (pnames[i] && strcmp(obj->as.ident.name, pnames[i]) == 0)
          poly[i] = true;
      }
    }
    scan_poly_usage_expr(e->as.index.target, pnames, poly, np);
    scan_poly_usage_expr(e->as.index.start, pnames, poly, np);
    return;
  }
  if (e->kind == NY_E_MEMBER) {
    expr_t *obj = e->as.member.target;
    if (obj && obj->kind == NY_E_IDENT) {
      for (int i = 0; i < np; i++) {
        if (pnames[i] && strcmp(obj->as.ident.name, pnames[i]) == 0)
          poly[i] = true;
      }
    }
    scan_poly_usage_expr(e->as.member.target, pnames, poly, np);
    return;
  }
}

static void scan_poly_usage(stmt_t *s, const char **pnames, bool *poly,
                            int np) {
  if (!s)
    return;
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      scan_poly_usage(s->as.block.body.data[i], pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_EXPR) {
    scan_poly_usage_expr(s->as.expr.expr, pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_VAR) {
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      scan_poly_usage_expr(s->as.var.exprs.data[i], pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_RETURN) {
    scan_poly_usage_expr(s->as.ret.value, pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_IF) {
    scan_poly_usage_expr(s->as.iff.test, pnames, poly, np);
    scan_poly_usage(s->as.iff.conseq, pnames, poly, np);
    scan_poly_usage(s->as.iff.alt, pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_WHILE) {
    scan_poly_usage_expr(s->as.whl.test, pnames, poly, np);
    scan_poly_usage(s->as.whl.body, pnames, poly, np);
    if (s->as.whl.update)
      scan_poly_usage(s->as.whl.update, pnames, poly, np);
    if (s->as.whl.init)
      scan_poly_usage(s->as.whl.init, pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_FOR) {
    if (s->as.fr.init)
      scan_poly_usage(s->as.fr.init, pnames, poly, np);
    if (s->as.fr.cond)
      scan_poly_usage_expr(s->as.fr.cond, pnames, poly, np);
    if (s->as.fr.iterable)
      scan_poly_usage_expr(s->as.fr.iterable, pnames, poly, np);
    scan_poly_usage(s->as.fr.body, pnames, poly, np);
    if (s->as.fr.update)
      scan_poly_usage(s->as.fr.update, pnames, poly, np);
    return;
  }
}

void ny_infer_param_types(stmt_t *fn, const char **param_types) {
  int nparam = (int)fn->as.fn.params.len;
  if (nparam == 0 || nparam > 16)
    return;

  bool is_stdlib = ny_is_stdlib_tok(fn->tok);
  if (is_stdlib) {
    for (int i = 0; i < nparam; i++)
      param_types[i] = fn->as.fn.params.data[i].type;
    return;
  }
  const char *param_names[16];
  for (int i = 0; i < nparam; i++) {
    param_names[i] = fn->as.fn.params.data[i].name;
    param_types[i] = fn->as.fn.params.data[i].type;
  }
  const char *env_names[64];
  const char *env_types[64];
  type_env_t env = {env_names, env_types, 0, NULL};
  for (int i = 0; i < nparam; i++) {
    if (param_types[i]) {
      env.names[env.count] = param_names[i];
      env.types[env.count] = param_types[i];
      env.count++;
    }
  }

  for (int pass = 0; pass < 3; pass++)
    scan_body_for_param_types(fn->as.fn.body, &env, param_names, param_types,
                              nparam);

  bool param_used_as_float[16] = {0};
  bool param_used_poly[16] = {0};
  scan_float_usage(fn->as.fn.body, param_names, param_used_as_float, nparam);
  scan_poly_usage(fn->as.fn.body, param_names, param_used_poly, nparam);
  for (int i = 0; i < nparam; i++) {
    if (param_types[i] && strcmp(param_types[i], "int") == 0 &&
        !fn->as.fn.params.data[i].type) {
      if (param_used_as_float[i] || param_used_poly[i])
        param_types[i] = NULL;
      expr_t *dv = fn->as.fn.params.data[i].def;
      if (dv && dv->kind == NY_E_LITERAL && dv->as.literal.kind == NY_LIT_FLOAT)
        param_types[i] = NULL;
    }
  }
}

static const char *infer_return_type_merge(type_env_t *env, const char *cur,
                                           const char *next);

static const char *infer_return_type_walk(stmt_t *body, type_env_t *env,
                                          const char *cur, bool tail_position) {
  if (!body)
    return cur;
  if (body->kind == NY_S_RETURN) {
    const char *t =
        body->as.ret.value ? ast_infer_type(body->as.ret.value, env) : NULL;
    if (!t)
      return cur;
    return infer_return_type_merge(env, cur, t);
  }
  if (body->kind == NY_S_EXPR) {
    if (!tail_position)
      return cur;
    const char *t = ast_infer_type(body->as.expr.expr, env);
    if (!ny_generic_type_base_is(t, "Result"))
      return cur;
    return infer_return_type_merge(env, cur, t);
  }
  if (body->kind == NY_S_VAR) {
    for (size_t i = 0; i < body->as.var.names.len; ++i) {
      const char *name = body->as.var.names.data[i];
      const char *decl =
          i < body->as.var.types.len ? body->as.var.types.data[i] : NULL;
      expr_t *init =
          i < body->as.var.exprs.len ? body->as.var.exprs.data[i] : NULL;
      const char *inferred = decl && *decl ? decl : ast_infer_type(init, env);
      env_push_type(env, name, inferred);
    }
    return cur;
  }
  if (body->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < body->as.block.body.len; i++) {
      cur = infer_return_type_walk(body->as.block.body.data[i], env, cur,
                                   tail_position &&
                                       i + 1 == body->as.block.body.len);
      if (cur && cur[0] == '?')
        return cur;
    }
    return cur;
  }
  if (body->kind == NY_S_IF) {
    type_env_t conseq_env = *env;
    type_env_t alt_env = *env;
    cur = infer_return_type_walk(body->as.iff.conseq, &conseq_env, cur,
                                 tail_position);
    if (cur && cur[0] == '?')
      return cur;
    cur =
        infer_return_type_walk(body->as.iff.alt, &alt_env, cur, tail_position);
    return cur;
  }
  if (body->kind == NY_S_WHILE) {
    type_env_t body_env = *env;
    const char *r =
        infer_return_type_walk(body->as.whl.body, &body_env, NULL, false);
    cur = infer_return_type_merge(env, cur, r);
    if (cur && cur[0] == '?')
      return cur;
    if (body->as.whl.update)
      cur = infer_return_type_walk(body->as.whl.update, env, cur, false);
    return cur;
  }
  if (body->kind == NY_S_FOR) {
    if (body->as.fr.init)
      cur = infer_return_type_walk(body->as.fr.init, env, cur, false);
    type_env_t body_env = *env;
    const char *r =
        infer_return_type_walk(body->as.fr.body, &body_env, NULL, false);
    cur = infer_return_type_merge(env, cur, r);
    if (cur && cur[0] == '?')
      return cur;
    if (body->as.fr.update)
      cur = infer_return_type_walk(body->as.fr.update, env, cur, false);
    return cur;
  }
  if (body->kind == NY_S_GUARD) {
    cur = infer_return_type_walk(body->as.guard.fallback, env, cur,
                                 tail_position);
    if (cur && cur[0] == '?')
      return cur;
    if (body->as.guard.name && body->as.guard.type_name) {
      size_t n = strlen(body->as.guard.type_name);
      char *ptr_type = malloc(n + 2);
      if (ptr_type) {
        ptr_type[0] = '*';
        memcpy(ptr_type + 1, body->as.guard.type_name, n + 1);
        env_push_type(env, body->as.guard.name,
                      type_scratch_take(env, ptr_type));
      }
    }
    return cur;
  }
  return cur;
}

static const char *infer_return_type_merge(type_env_t *env, const char *cur,
                                           const char *next) {
  if (!next)
    return cur;
  if (!cur)
    return next;
  if (ny_generic_type_base_is(cur, "Result") &&
      ny_generic_type_base_is(next, "Result")) {
    char *cur_ok = ny_generic_type_arg_owned(cur, 0);
    char *cur_err = ny_generic_type_arg_owned(cur, 1);
    char *next_ok = ny_generic_type_arg_owned(next, 0);
    char *next_err = ny_generic_type_arg_owned(next, 1);
    const char *ok = func_type_merge_result_arg(cur_ok, next_ok);
    const char *err = func_type_merge_result_arg(cur_err, next_err);
    const char *out = func_type_make_result(env, ok, err);
    free(cur_ok);
    free(cur_err);
    free(next_ok);
    free(next_err);
    return out;
  }
  if (strcmp(cur, next) != 0)
    return "?";
  return cur;
}

char *ny_infer_fn_return_type(stmt_t *fn, const char **param_types) {
  if (fn->as.fn.return_type)
    return NULL;
  int nparam = (int)fn->as.fn.params.len;
  if (nparam > 16)
    return NULL;
  bool is_stdlib = ny_is_stdlib_tok(fn->tok);
  if (is_stdlib)
    return NULL;
  const char *env_names[NY_FUNC_TYPE_ENV_MAX];
  const char *env_types[NY_FUNC_TYPE_ENV_MAX];
  type_scratch_t scratch = {0};
  type_env_t env = {env_names, env_types, 0, &scratch};
  for (int i = 0; i < nparam; i++) {
    const char *t =
        param_types[i] ? param_types[i] : fn->as.fn.params.data[i].type;
    env_push_type(&env, fn->as.fn.params.data[i].name, t);
  }
  const char *ret = infer_return_type_walk(fn->as.fn.body, &env, NULL, true);
  char *out = (ret && ret[0] != '?') ? ny_strdup(ret) : NULL;
  type_scratch_free(&scratch);
  return out;
}

void gen_func(codegen_t *cg, stmt_t *fn, const char *name, scope *scopes,
              size_t depth, binding_list *captures) {
  bool prof_func = ny_codegen_func_profile_enabled();
  ny_tick_t prof_start = prof_func ? ny_ticks_now() : 0;
  ny_tick_t prof_mark = prof_start;
  double prof_sig_ms = 0.0;
  double prof_assigned_ms = 0.0;
  double prof_params_ms = 0.0;
  double prof_infer_ms = 0.0;
  double prof_body_ms = 0.0;
  ny_resolve_fn_attrs(cg, fn);
  if (!fn->as.fn.body)
    return;
  bool function_native_abi = ny_fn_uses_native_abi(cg, fn);
  bool split_worker_attached_method = cg && cg->emit_module_decls_only &&
                                      fn->as.fn.name &&
                                      strchr(fn->as.fn.name, '.') != NULL;

  const char *origin_section = "ny.user";
  if (ny_is_stdlib_tok(fn->tok))
    origin_section = "ny.std";
#ifdef __APPLE__
  origin_section = (strcmp(origin_section, "ny.std") == 0) ? "__TEXT,ny_std"
                                                           : "__TEXT,ny_user";
#endif
  LLVMValueRef f = ny_get_named_fn(cg, name);
  if (!f) {
    size_t n_params = fn->as.fn.params.len;
    if (fn->sema)
      NY_SEMA_ASSERT(fn, NY_STMT_SEMA_FUNC);
    sema_func_t *sema =
        (fn->sema_kind == NY_STMT_SEMA_FUNC) ? (sema_func_t *)fn->sema : NULL;
    size_t total_args = captures ? n_params + 1 : n_params;
    LLVMTypeRef *pt = alloca(sizeof(LLVMTypeRef) * total_args);
    if (captures) {
      pt[0] = cg->type_i64;
    }
    for (size_t i = 0; i < n_params; i++) {
      LLVMTypeRef pty = cg->type_i64;
      if (sema && i < sema->resolved_param_types.len)
        pty = sema->resolved_param_types.data[i];
      else if (fn->as.fn.params.data[i].type)
        pty =
            ny_fn_resolve_user_abi_type(cg, fn, function_native_abi,
                                        fn->as.fn.params.data[i].type, fn->tok);
      pt[i + (captures ? 1 : 0)] = pty;
    }
    LLVMTypeRef rty = cg->type_i64;
    if (sema)
      rty = sema->resolved_return_type;
    else if (fn->as.fn.return_type)
      rty = ny_fn_resolve_user_abi_type(cg, fn, function_native_abi,
                                        fn->as.fn.return_type, fn->tok);
    LLVMTypeRef ft = LLVMFunctionType(rty, pt, (unsigned)total_args, 0);
    f = LLVMAddFunction(cg->module, name, ft);
    if (!cg->is_repl)
      LLVMSetSection(f, origin_section);
    fun_sig sig = {
        .name = ny_strdup(name),
        .module_name =
            cg->current_module_name ? ny_strdup(cg->current_module_name) : NULL,
        .type = ft,
        .value = f,
        .stmt_t = fn,
        .source_file = fn->tok.filename ? ny_strdup(fn->tok.filename) : NULL,
        .arity = (int)n_params,
        .is_variadic = fn->as.fn.is_variadic,
        .is_extern = false,
        .effects = NY_FX_ALL,
        .args_escape = true,
        .args_mutated = true,
        .returns_alias = true,
        .effects_known = false,
        .link_name = NULL,
        .return_type =
            fn->as.fn.return_type ? ny_strdup(fn->as.fn.return_type) : NULL,
        .returns_owned = fn->as.fn.attr_returns_owned,
        .owned = false,
        .is_native_abi = function_native_abi,
        .tailcall = fn->as.fn.attr_tailcall,
        .name_hash = 0};
    ny_fun_sig_set_params(&sig, &fn->as.fn.params);
    ny_fun_sig_copy_contracts(&sig, &fn->as.fn);
    vec_push(&cg->fun_sigs, sig);
    ny_apply_decl_fn_attrs(cg, f, fn);
    if (ny_stmt_func_body_has_try(fn))
      ny_apply_longjmp_fn_attrs(cg, f);
  } else {
    ny_llvm_clear_function(f);

    const char *sec = LLVMGetSection(f);
#ifdef __APPLE__
    if (cg->is_repl) {

    } else if (sec && strcmp(sec, "ny.std") == 0)
      LLVMSetSection(f, "__TEXT,ny_std");
    else if (sec && strcmp(sec, "ny.user") == 0)
      LLVMSetSection(f, "__TEXT,ny_user");
    else if (!sec || !*sec)
      LLVMSetSection(f, origin_section);
#else
    if (!sec || !*sec)
      LLVMSetSection(f, origin_section);
#endif
  }
  if (split_worker_attached_method)
    LLVMSetLinkage(f, LLVMWeakODRLinkage);
  if (prof_func) {
    ny_tick_t now = ny_ticks_now();
    prof_sig_ms = ny_ticks_delta_ms(prof_mark, now);
    prof_mark = now;
  }
  LLVMBasicBlockRef cur = ny_cur_block(cg);
  LLVMBasicBlockRef entry_bb = ny_bb_fn(f, "entry");
  ny_pos(cg, entry_bb);

  LLVMMetadataRef prev_scope = cg->di_scope;
  LLVMMetadataRef prev_loc = cg->di_loc;
  if (cg->debug_symbols && cg->di_builder) {
    LLVMMetadataRef sp = codegen_debug_subprogram(cg, f, name, fn->tok);
    if (sp)
      cg->di_scope = sp;
  }
  ny_dbg_loc(cg, fn->tok);
  if (!fn->as.fn.attr_naked)
    emit_trace_enter(cg, name ? name : "<anon>", fn->tok);
  LLVMValueRef prev_fn_value = cg->current_fn_value;
  cg->current_fn_value = f;
  size_t fd = depth + 1;
  size_t root = fd;
  assigned_name_list assigned_names = {0};
  assigned_hash_list assigned_hashes = {0};
  uint64_t assigned_bloom[4] = {0, 0, 0, 0};
  bool use_assigned_prepass =
      !ny_env_enabled("NYTRIX_DISABLE_ASSIGNED_PREPASS");
  if (use_assigned_prepass) {
    collect_assigned_names_stmt(fn->as.fn.body, &assigned_names,
                                &assigned_hashes, assigned_bloom);
  }
  if (prof_func) {
    ny_tick_t now = ny_ticks_now();
    prof_assigned_ms = ny_ticks_delta_ms(prof_mark, now);
    prof_mark = now;
  }
  memset(&scopes[fd], 0, sizeof(scopes[fd]));
  size_t param_offset = 0;
  if (!fn->as.fn.attr_naked) {
    if (captures) {
      param_offset = 1;
      unsigned actual_params = LLVMCountParams(f);
      LLVMValueRef env_arg = (actual_params > 0) ? LLVMGetParam(f, 0) : NULL;
      LLVMValueRef env_raw =
          env_arg ? LLVMBuildIntToPtr(cg->builder, env_arg, ny_ptr_i64_ty(cg),
                                      "env_raw")
                  : NULL;
      for (size_t i = 0; i < captures->len; i++) {
        LLVMValueRef src =
            env_raw ? LLVMBuildGEP2(cg->builder, cg->type_i64, env_raw,
                                    (LLVMValueRef[]){LLVMConstInt(
                                        cg->type_i64, (uint64_t)i, false)},
                                    1, "")
                    : NULL;
        LLVMValueRef val = src ? ny_load(cg, src, "") : NULL;
        bool needs_slot =
            !use_assigned_prepass ||
            (captures->data[i].is_mut &&
             assigned_name_contains(&assigned_names, &assigned_hashes,
                                    assigned_bloom, captures->data[i].name));
        if (needs_slot) {
          if (captures->data[i].is_mut && src) {
            scope_bind(cg, scopes, fd, captures->data[i].name, src,
                       captures->data[i].stmt_t ? captures->data[i].stmt_t : fn,
                       true, captures->data[i].type_name, true);
          } else {
            LLVMValueRef lv =
                build_alloca(cg, captures->data[i].name, cg->type_i64);
            if (lv && val) {
              ny_store(cg, lv, val);
            }
            scope_bind(cg, scopes, fd, captures->data[i].name, lv,
                       captures->data[i].stmt_t ? captures->data[i].stmt_t : fn,
                       true, captures->data[i].type_name, true);
          }
        } else {
          scope_bind(cg, scopes, fd, captures->data[i].name, val,
                     captures->data[i].stmt_t ? captures->data[i].stmt_t : fn,
                     captures->data[i].is_mut, captures->data[i].type_name,
                     false);
        }
        scopes[fd].vars.data[scopes[fd].vars.len - 1].is_used = true;
      }
    }
    const char *inferred_types[16] = {0};
    if (fn->as.fn.params.len <= 16)
      ny_infer_param_types(fn, inferred_types);
    static int debug_infer = -1;
    if (debug_infer < 0)
      debug_infer = (getenv("NYTRIX_DEBUG_INFER") != NULL);
    if (debug_infer) {
      for (size_t di = 0; di < fn->as.fn.params.len && di < 16; di++) {
        if (inferred_types[di] && !fn->as.fn.params.data[di].type)
          fprintf(stderr, "[INFER] %s param[%zu] '%s' -> %s (%s)\n", name, di,
                  fn->as.fn.params.data[di].name, inferred_types[di],
                  fn->tok.filename ? fn->tok.filename : "<null>");
      }
    }
    unsigned actual_params = LLVMCountParams(f);
    for (size_t i = 0; i < fn->as.fn.params.len; i++) {
      const char *param_name = fn->as.fn.params.data[i].name;
      const char *explicit_ptype = fn->as.fn.params.data[i].type;
      unsigned param_idx = (unsigned)(i + param_offset);
      LLVMValueRef param_val =
          (param_idx < actual_params) ? LLVMGetParam(f, param_idx) : NULL;
      LLVMValueRef raw_int_param_val = NULL;
      bool is_inferred_f64 = !fn->as.fn.params.data[i].type && i < 16 &&
                             inferred_types[i] &&
                             strcmp(inferred_types[i], "f64") == 0;
      bool is_inferred_int = !fn->as.fn.params.data[i].type && i < 16 &&
                             inferred_types[i] &&
                             strcmp(inferred_types[i], "int") == 0;
      bool needs_slot =
          !use_assigned_prepass ||
          assigned_name_contains(&assigned_names, &assigned_hashes,
                                 assigned_bloom, param_name);
      bool is_explicit_f32 = ny_type_is(explicit_ptype, "f32");
      bool is_explicit_f64 = ny_type_is(explicit_ptype, "f64");
      bool is_explicit_int = ny_type_is(explicit_ptype, "int");
      bool is_raw_int_direct_param = false;
      if (param_val && (is_explicit_f32 || is_explicit_f64)) {
        LLVMTypeRef float_ty = is_explicit_f32 ? cg->type_f32 : cg->type_f64;
        LLVMValueRef fv =
            func_param_float_value(cg, param_val, is_explicit_f32);
        if (needs_slot) {
          LLVMValueRef slot = build_alloca(cg, param_name, float_ty);
          ny_store(cg, slot, fv);
          scope_bind(cg, scopes, fd, param_name, slot, fn, true, explicit_ptype,
                     true);
          binding *b = &scopes[fd].vars.data[scopes[fd].vars.len - 1];
          b->is_f32_slot = is_explicit_f32;
          b->is_f64_slot = is_explicit_f64;
          if (cg->debug_symbols && cg->di_builder && slot) {
            codegen_debug_variable(cg, param_name, explicit_ptype, slot,
                                   fn->tok, true,
                                   (int)i + 1 + (int)param_offset, true);
          }
        } else {
          scope_bind(cg, scopes, fd, param_name, fv, fn, true, explicit_ptype,
                     false);
          binding *b = &scopes[fd].vars.data[scopes[fd].vars.len - 1];
          b->is_f32_direct = is_explicit_f32;
          b->is_f64_direct = is_explicit_f64;
          if (cg->debug_symbols && cg->di_builder && fv) {
            codegen_debug_variable(cg, param_name, explicit_ptype, fv, fn->tok,
                                   true, (int)i + 1 + (int)param_offset, false);
          }
        }
        continue;
      }
      layout_def_t *layout_param = func_layout_abi_type(cg, explicit_ptype);
      if (param_val && function_native_abi && layout_param) {
        LLVMTypeRef layout_carrier =
            func_layout_abi_carrier_type(cg, layout_param);
        fun_sig *malloc_sig = lookup_fun(cg, "__malloc", 0);
        if (!malloc_sig) {
          ny_diag_error(fn->tok,
                        "__malloc required for native layout ABI parameter");
          cg->had_error = 1;
        } else {
          LLVMValueRef size_arg = LLVMConstInt(
              cg->type_i64, (((uint64_t)layout_param->size << 1) | 1u), false);
          LLVMValueRef ptr_i64 =
              LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                             &size_arg, 1, "param_layout_alloc");
          LLVMValueRef dst = LLVMBuildIntToPtr(
              cg->builder, ptr_i64, LLVMPointerType(layout_carrier, 0),
              "param_layout_ptr");
          ny_store(cg, dst, param_val);
          param_val = ptr_i64;
        }
      } else if (param_val && !function_native_abi &&
                 fn_tagged_abi_should_normalize_param(explicit_ptype)) {
        LLVMValueRef raw = ny_coerce_to_abi(cg, param_val, explicit_ptype);
        if (is_explicit_int)
          raw_int_param_val = raw;
        param_val = ny_box_abi_result(cg, raw, explicit_ptype);
      } else if (param_val && function_native_abi && explicit_ptype != NULL &&
                 ny_is_native_abi_type_name(explicit_ptype) &&
                 !ny_type_is_tagged(explicit_ptype) &&
                 !ny_type_is(explicit_ptype, "fnptr")) {
        LLVMTypeRef pllty = LLVMTypeOf(param_val);
        LLVMTypeKind pk = LLVMGetTypeKind(pllty);
        if (pk == LLVMDoubleTypeKind || pk == LLVMFloatTypeKind) {
          LLVMValueRef f64val = param_val;
          if (pk == LLVMFloatTypeKind)
            f64val = LLVMBuildFPExt(cg->builder, param_val, cg->type_f64, "");
          LLVMValueRef bits = ny_bitcast(cg, f64val, cg->type_i64, "");
          fun_sig *box_sig = lookup_fun(cg, "__flt_box_val", 0);
          if (box_sig) {
            param_val = LLVMBuildCall2(cg->builder, box_sig->type,
                                       box_sig->value, &bits, 1, "");
          } else {
            param_val = bits;
          }
        } else if (pk == LLVMPointerTypeKind) {
          param_val = ny_ptr2i64(cg, param_val, "");
        } else if (pk == LLVMStructTypeKind) {
          param_val = ny_box_abi_result(cg, param_val, explicit_ptype);
        } else if (pk == LLVMIntegerTypeKind) {
          unsigned w = LLVMGetIntTypeWidth(pllty);
          if (w < 64)
            param_val = LLVMBuildSExt(cg->builder, param_val, cg->type_i64, "");
          if (is_explicit_int)
            raw_int_param_val = param_val;
          if (cg->mono_emitting && is_explicit_int && !needs_slot) {
            is_raw_int_direct_param = true;
          } else {
            param_val = ny_tag_int(cg, param_val);
          }
        }
      }
      if (is_inferred_f64 && param_val) {
        LLVMValueRef f64v = ny_unbox_float(cg, param_val);
        if (needs_slot) {
          LLVMValueRef slot = build_alloca(cg, param_name, cg->type_f64);
          ny_store(cg, slot, f64v);
          scope_bind(cg, scopes, fd, param_name, slot, fn, true, "f64", true);
          binding *b = &scopes[fd].vars.data[scopes[fd].vars.len - 1];
          b->is_f64_slot = true;
          if (cg->debug_symbols && cg->di_builder && slot) {
            codegen_debug_variable(cg, param_name, "f64", slot, fn->tok, true,
                                   (int)i + 1 + (int)param_offset, true);
          }
        } else {
          scope_bind(cg, scopes, fd, param_name, f64v, fn, true, "f64", false);
          binding *b = &scopes[fd].vars.data[scopes[fd].vars.len - 1];
          b->is_f64_direct = true;
          if (cg->debug_symbols && cg->di_builder && f64v) {
            codegen_debug_variable(cg, param_name, "f64", f64v, fn->tok, true,
                                   (int)i + 1 + (int)param_offset, false);
          }
        }
      } else if (needs_slot) {
        LLVMValueRef slot = build_alloca(cg, param_name, cg->type_i64);
        if (slot && param_val) {
          ny_store(cg, slot, param_val);
        }
        const char *ptype = fn->as.fn.params.data[i].type;
        if (!ptype && is_inferred_int)
          ptype = "int";
        sema_func_t *param_sema = (fn->sema_kind == NY_STMT_SEMA_FUNC)
                                      ? (sema_func_t *)fn->sema
                                      : NULL;
        uint8_t mono_kind = (param_sema && i < NY_MONO_MAX_ARITY)
                                ? param_sema->mono_param_kinds[i]
                                : 0;
        bool mono_list_param = mono_kind == NY_MONO_TYPE_LIST ||
                               mono_kind == NY_MONO_TYPE_F64_LIST;
        scope_bind(cg, scopes, fd, param_name, slot, fn, true, ptype, true);
        binding *b = &scopes[fd].vars.data[scopes[fd].vars.len - 1];
        if (ptype && strcmp(ptype, "int") == 0) {
          b->is_int_slot = true;
          b->raw_int_value = build_alloca(cg, "raw.param.int", cg->type_i64);
          if (b->raw_int_value && param_val)
            ny_store(cg, b->raw_int_value, ny_untag_int(cg, param_val));
        }
        if ((ptype && ny_type_is(ptype, "list")) || mono_list_param) {
          b->is_list_storage = true;
          b->is_f64_list_storage = mono_kind == NY_MONO_TYPE_F64_LIST;
          if (param_sema && i < NY_MONO_MAX_ARITY &&
              param_sema->mono_param_list_len_min_known[i]) {
            b->has_list_len_min = true;
            b->list_len_min_raw = param_sema->mono_param_list_len_min_raw[i];
          }
        }
        if (cg->debug_symbols && cg->di_builder && slot) {
          codegen_debug_variable(cg, param_name, ptype, slot, fn->tok, true,
                                 (int)i + 1 + (int)param_offset, true);
        }
      } else {
        const char *ptype = fn->as.fn.params.data[i].type;
        if (!ptype && is_inferred_int)
          ptype = "int";
        sema_func_t *param_sema = (fn->sema_kind == NY_STMT_SEMA_FUNC)
                                      ? (sema_func_t *)fn->sema
                                      : NULL;
        uint8_t mono_kind = (param_sema && i < NY_MONO_MAX_ARITY)
                                ? param_sema->mono_param_kinds[i]
                                : 0;
        bool mono_list_param = mono_kind == NY_MONO_TYPE_LIST ||
                               mono_kind == NY_MONO_TYPE_F64_LIST;
        scope_bind(cg, scopes, fd, param_name, param_val, fn, true, ptype,
                   false);
        binding *b = &scopes[fd].vars.data[scopes[fd].vars.len - 1];
        if (ptype && strcmp(ptype, "int") == 0)
          b->is_int_direct = true;
        if (ptype && strcmp(ptype, "int") == 0)
          b->is_int_raw_direct = is_raw_int_direct_param;
        if (ptype && strcmp(ptype, "int") == 0 && param_val)
          b->raw_int_value = raw_int_param_val ? raw_int_param_val
                                               : ny_untag_int(cg, param_val);
        else if (ptype && strcmp(ptype, "int") == 0 && raw_int_param_val)
          b->raw_int_value = raw_int_param_val;
        if ((ptype && ny_type_is(ptype, "list")) || mono_list_param) {
          b->is_list_storage = true;
          b->is_f64_list_storage = mono_kind == NY_MONO_TYPE_F64_LIST;
          if (param_sema && i < NY_MONO_MAX_ARITY &&
              param_sema->mono_param_list_len_min_known[i]) {
            b->has_list_len_min = true;
            b->list_len_min_raw = param_sema->mono_param_list_len_min_raw[i];
          }
        }
        if (cg->debug_symbols && cg->di_builder && param_val) {
          codegen_debug_variable(cg, param_name, ptype, param_val, fn->tok,
                                 true, (int)i + 1 + (int)param_offset, false);
        }
      }
    }
  }
  emit_trace_args(cg, scopes, fd, fn);
  if (prof_func) {
    ny_tick_t now = ny_ticks_now();
    prof_params_ms = ny_ticks_delta_ms(prof_mark, now);
    prof_mark = now;
  }
  size_t old_root = cg->func_root_idx;
  cg->func_root_idx = root;
  const char **old_assigned_names_data = cg->assigned_names_data;
  size_t old_assigned_names_len = cg->assigned_names_len;
  const uint64_t *old_assigned_name_hashes_data = cg->assigned_name_hashes_data;
  size_t old_assigned_name_hashes_len = cg->assigned_name_hashes_len;
  uint64_t old_assigned_names_bloom[4] = {
      cg->assigned_names_bloom[0], cg->assigned_names_bloom[1],
      cg->assigned_names_bloom[2], cg->assigned_names_bloom[3]};
  cg->assigned_names_data = assigned_names.data;
  cg->assigned_names_len = assigned_names.len;
  cg->assigned_name_hashes_data = assigned_hashes.data;
  cg->assigned_name_hashes_len = assigned_hashes.len;
  cg->assigned_names_bloom[0] = assigned_bloom[0];
  cg->assigned_names_bloom[1] = assigned_bloom[1];
  cg->assigned_names_bloom[2] = assigned_bloom[2];
  cg->assigned_names_bloom[3] = assigned_bloom[3];
  const char *prev_mod = cg->current_module_name;
  char *temp_mod = NULL;
  const char *last_dot = strrchr(name, '.');
  if (last_dot) {
    size_t len = last_dot - name;
    temp_mod = malloc(len + 1);
    memcpy(temp_mod, name, len);
    temp_mod[len] = '\0';
    bool attached_in_lexical_module = false;
    if (prev_mod && *prev_mod) {
      size_t mod_len = strlen(prev_mod);
      if (strncmp(name, prev_mod, mod_len) == 0 && name[mod_len] == '.') {
        const char *rest = name + mod_len + 1;
        attached_in_lexical_module = strchr(rest, '.') != NULL;
      }
    }
    cg->current_module_name = attached_in_lexical_module ? prev_mod : temp_mod;
  }
  fun_sig *prev_fun_sig = cg->current_fun_sig;
  cg->current_fun_sig = lookup_fun(cg, name, 0);
  const char *prev_ret = cg->current_fn_ret_type;
  const char *prev_returns_borrow = cg->current_fn_returns_borrow;
  bool prev_returns_owned = cg->current_fn_returns_owned;
  stmt_t *prev_fn_body = cg->current_fn_body;
  bool prev_native_abi = cg->current_fn_native_abi;
  bool prev_naked = cg->current_fn_attr_naked;
  bool prev_tailcall = cg->current_fn_attr_tailcall;
  cg->current_fn_ret_type = fn->as.fn.return_type;
  cg->current_fn_returns_borrow = fn->as.fn.attr_returns_borrow;
  cg->current_fn_returns_owned = fn->as.fn.attr_returns_owned;
  cg->current_fn_body = fn->as.fn.body;
  cg->current_fn_native_abi = function_native_abi;
  cg->current_fn_attr_naked = fn->as.fn.attr_naked;
  cg->current_fn_attr_tailcall = fn->as.fn.attr_tailcall;

  if (cg->opt_type_infer) {
    typeinfer_ctx_t infer_ctx = {0};
    size_t max_infer_vars = 256;
    typeinfer_ctx_init(&infer_ctx, max_infer_vars, scopes, cg);
    typeinfer_func_body(&infer_ctx, fn->as.fn.body);

    typeinfer_apply_to_scopes(&infer_ctx, scopes, fd + 1);
    typeinfer_ctx_dispose(&infer_ctx);
  }
  if (prof_func) {
    ny_tick_t now = ny_ticks_now();
    prof_infer_ms = ny_ticks_delta_ms(prof_mark, now);
    prof_mark = now;
  }

  label_binding *saved_label_data = cg->labels.data;
  size_t saved_label_len = cg->labels.len;
  size_t saved_label_cap = cg->labels.cap;
  cg->labels.data = NULL;
  cg->labels.len = 0;
  cg->labels.cap = 0;
  if (stmt_func_body_has_label_or_goto(fn))
    collect_labels(cg, f, fn->as.fn.body, root);
  gen_stmt(cg, scopes, &fd, fn->as.fn.body, root, true);
  if (fn->as.fn.attr_proves)
    ny_proof_check_fn_proves(cg, scopes, depth, fn);
  for (size_t i = 0; i < cg->labels.len; ++i)
    free((void *)cg->labels.data[i].name);
  free(cg->labels.data);
  cg->labels.data = saved_label_data;
  cg->labels.len = saved_label_len;
  cg->labels.cap = saved_label_cap;
  if (prof_func) {
    ny_tick_t now = ny_ticks_now();
    prof_body_ms = ny_ticks_delta_ms(prof_mark, now);
    prof_mark = now;
  }
  cg->current_fn_ret_type = prev_ret;
  cg->current_fn_returns_borrow = prev_returns_borrow;
  cg->current_fn_returns_owned = prev_returns_owned;
  cg->current_fn_body = prev_fn_body;
  cg->current_fn_native_abi = prev_native_abi;
  cg->current_fn_attr_naked = prev_naked;
  cg->current_fn_attr_tailcall = prev_tailcall;
  cg->current_fn_value = prev_fn_value;
  cg->current_fun_sig = prev_fun_sig;
  cg->current_module_name = prev_mod;
  if (temp_mod)
    free(temp_mod);
  cg->func_root_idx = old_root;
  cg->assigned_names_data = old_assigned_names_data;
  cg->assigned_names_len = old_assigned_names_len;
  cg->assigned_name_hashes_data = old_assigned_name_hashes_data;
  cg->assigned_name_hashes_len = old_assigned_name_hashes_len;
  cg->assigned_names_bloom[0] = old_assigned_names_bloom[0];
  cg->assigned_names_bloom[1] = old_assigned_names_bloom[1];
  cg->assigned_names_bloom[2] = old_assigned_names_bloom[2];
  cg->assigned_names_bloom[3] = old_assigned_names_bloom[3];
  if (!ny_has_terminator(cg)) {
    if (fn->as.fn.attr_naked) {
      LLVMBuildUnreachable(cg->builder);
    } else {
      /*
       * Fall-off-the-end is only defined for dynamically typed/nil returns.
       * A concrete return type cannot safely consume the nil ABI sentinel;
       * reject it instead of allowing a backend-dependent value to escape.
       */
      const char *rt = fn->as.fn.return_type;
      const char *dot = rt ? strrchr(rt, '.') : NULL;
      const char *base = dot ? dot + 1 : rt;
      if (base && *base && strcmp(base, "void") != 0 &&
          strcmp(base, "any") != 0 && strcmp(base, "nil") != 0 &&
          fn->tok.line > 0) {
        ny_diag_error_code(fn->tok, 2006,
                           "function '%s' falls off the end without "
                           "returning a %s value",
                           fn->as.fn.name, base);
      }
      ny_cg_emit_trace_return_void(cg);
      ny_cg_emit_trace_exit(cg);
      LLVMBuildRet(cg->builder, ny_cnil(cg));
    }
  }
  scope_pop(scopes, &fd);
  vec_free(&assigned_names);
  vec_free(&assigned_hashes);
  cg->di_scope = prev_scope;
  cg->di_loc = prev_loc;
  if (cg->debug_symbols && cg->builder) {
    LLVMSetCurrentDebugLocation2(cg->builder, prev_loc);
    if (cg->alloca_builder) {
      LLVMSetCurrentDebugLocation2(cg->alloca_builder, prev_loc);
    }
  }
  if (cur)
    ny_pos(cg, cur);
  if (prof_func) {
    ny_codegen_func_profile_emit(
        name, fn->tok.filename, ny_ticks_elapsed_ms(prof_start), prof_sig_ms,
        prof_assigned_ms, prof_params_ms, prof_infer_ms, prof_body_ms);
  }
}
