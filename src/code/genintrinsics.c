#include "base/util.h"
#include "priv.h"

#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif
#include <ctype.h>
#include <limits.h>
#include <llvm-c/Core.h>
#include <string.h>

static LLVMValueRef ny_intrinsic_cast_to_i64(codegen_t *cg, LLVMValueRef v,
                                             const char *name) {
  if (!cg || !v)
    return v;
  if (LLVMTypeOf(v) == cg->type_i64)
    return v;
  return ny_ptr2i64(cg, v, ny_llvm_name(cg, name));
}

static LLVMValueRef ny_intrinsic_tag_bool(codegen_t *cg, LLVMValueRef pred,
                                          const char *name) {
  return ny_select(cg, pred, ny_ctrue(cg), ny_cfalse(cg),
                   name ? name : "llvm_bool");
}

static bool ny_llvm_splice_name_from_expr(expr_t *e, const char **out_name,
                                          size_t *out_len) {
  if (!e || !out_name || !out_len)
    return false;
  if (e->kind != NY_E_LITERAL || e->as.literal.kind != NY_LIT_STR)
    return false;
  const char *name = e->as.literal.as.s.data;
  size_t len = e->as.literal.as.s.len;
  if (!name || len == 0)
    return false;
  *out_name = name;
  *out_len = len;
  return true;
}

static bool ny_llvm_splice_name_allowed(const char *name, size_t len) {
  if (!name || len == 0)
    return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)name[i];
    if (!(isalnum(c) || c == '.' || c == '_'))
      return false;
  }
  return true;
}

static bool ny_llvm_splice_parse_uint(const char *s, size_t n, unsigned *out) {
  if (!s || n == 0 || !out)
    return false;
  unsigned v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9')
      return false;
    unsigned digit = (unsigned)(s[i] - '0');
    if (v > (UINT_MAX - digit) / 10)
      return false;
    v = v * 10 + digit;
  }
  *out = v;
  return true;
}

static bool ny_llvm_splice_parse_type_token(codegen_t *cg, const char *s,
                                            size_t n, LLVMTypeRef *out) {
  if (!cg || !s || n == 0 || !out)
    return false;
  if (n >= 2 && s[0] == 'i') {
    unsigned bits = 0;
    if (ny_llvm_splice_parse_uint(s + 1, n - 1, &bits) && bits > 0) {
      *out = LLVMIntTypeInContext(cg->ctx, bits);
      return true;
    }
  }
  if (n == 3 && memcmp(s, "f16", 3) == 0) {
    *out = LLVMHalfTypeInContext(cg->ctx);
    return true;
  }
  if (n == 3 && memcmp(s, "f32", 3) == 0) {
    *out = cg->type_f32;
    return true;
  }
  if (n == 3 && memcmp(s, "f64", 3) == 0) {
    *out = cg->type_f64;
    return true;
  }
  if (n == 4 && memcmp(s, "f128", 4) == 0) {
    *out = cg->type_f128;
    return true;
  }
  if (s[0] == 'p') {
    unsigned addrspace = 0;
    if (n == 1 || ny_llvm_splice_parse_uint(s + 1, n - 1, &addrspace)) {
      *out = LLVMPointerType(cg->type_i8, addrspace);
      return true;
    }
  }
  if (n >= 3 && s[0] == 'v') {
    size_t pos = 1;
    while (pos < n && s[pos] >= '0' && s[pos] <= '9')
      pos++;
    unsigned lanes = 0;
    LLVMTypeRef elem = NULL;
    if (pos > 1 &&
        ny_llvm_splice_parse_uint(s + 1, pos - 1, &lanes) && lanes > 0 &&
        ny_llvm_splice_parse_type_token(cg, s + pos, n - pos, &elem)) {
      *out = LLVMVectorType(elem, lanes);
      return true;
    }
  }
  return false;
}

static size_t ny_llvm_splice_collect_overload_types(codegen_t *cg,
                                                    const char *name,
                                                    size_t len,
                                                    LLVMTypeRef *out,
                                                    size_t cap) {
  size_t count = 0;
  size_t start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i != len && name[i] != '.')
      continue;
    if (i > start && count < cap) {
      LLVMTypeRef ty = NULL;
      if (ny_llvm_splice_parse_type_token(cg, name + start, i - start, &ty))
        out[count++] = ty;
    }
    start = i + 1;
  }
  return count;
}

static const char *ny_llvm_splice_int_abi_name(unsigned bits) {
  switch (bits) {
  case 8:
    return "i8";
  case 16:
    return "i16";
  case 32:
    return "i32";
  case 64:
    return "i64";
  case 128:
    return "i128";
  default:
    return NULL;
  }
}

static LLVMValueRef ny_llvm_splice_to_i1(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *arg,
                                         token_t tok) {
  LLVMValueRef v = gen_expr(cg, scopes, depth, arg);
  if (!v)
    return NULL;
  LLVMTypeRef ty = LLVMTypeOf(v);
  if (ty && LLVMGetTypeKind(ty) == LLVMIntegerTypeKind &&
      LLVMGetIntTypeWidth(ty) == 1)
    return v;
  v = ny_intrinsic_cast_to_i64(cg, v, "llvm_i1_arg");
  if (LLVMIsAConstantInt(v)) {
    uint64_t raw = LLVMConstIntGetZExtValue(v);
    bool truthy = raw != NY_IMM_NIL && raw != NY_IMM_FALSE && raw != 1;
    return LLVMConstInt(cg->type_i1, truthy ? 1 : 0, false);
  }
  LLVMValueRef not_nil =
      ny_ne(cg, v, LLVMConstInt(cg->type_i64, NY_IMM_NIL, false),
            NY_LLVM_NAME(cg, "llvm_i1_not_nil"));
  LLVMValueRef not_false =
      ny_ne(cg, v, LLVMConstInt(cg->type_i64, NY_IMM_FALSE, false),
            NY_LLVM_NAME(cg, "llvm_i1_not_false"));
  LLVMValueRef not_zero =
      ny_ne(cg, v, ny_c1(cg), NY_LLVM_NAME(cg, "llvm_i1_not_zero"));
  (void)tok;
  return ny_and(cg,
                ny_and(cg, not_nil, not_false, NY_LLVM_NAME(cg, "llvm_i1_a")),
                not_zero, NY_LLVM_NAME(cg, "llvm_i1"));
}

static LLVMValueRef ny_llvm_splice_coerce_arg(codegen_t *cg, scope *scopes,
                                              size_t depth, expr_t *arg,
                                              LLVMTypeRef want_ty,
                                              token_t tok) {
  if (!cg || !arg || !want_ty)
    return NULL;
  LLVMTypeKind kind = LLVMGetTypeKind(want_ty);
  if (kind == LLVMIntegerTypeKind) {
    unsigned bits = LLVMGetIntTypeWidth(want_ty);
    if (bits == 1)
      return ny_llvm_splice_to_i1(cg, scopes, depth, arg, tok);
    const char *abi_name = ny_llvm_splice_int_abi_name(bits);
    if (!abi_name) {
      ny_diag_error(tok, "llvm(...) does not support i%u intrinsic arguments",
                    bits);
      cg->had_error = 1;
      return ny_c0(cg);
    }
    LLVMValueRef v = gen_expr(cg, scopes, depth, arg);
    return v ? ny_coerce_to_abi(cg, v, abi_name) : NULL;
  }
  if (kind == LLVMPointerTypeKind) {
    LLVMValueRef v = gen_expr(cg, scopes, depth, arg);
    return v ? ny_coerce_to_abi(cg, v, "ptr") : NULL;
  }
  if (kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind ||
      kind == LLVMFP128TypeKind) {
    LLVMValueRef v = gen_expr(cg, scopes, depth, arg);
    if (!v)
      return NULL;
    const char *abi_name = kind == LLVMFloatTypeKind    ? "f32"
                           : kind == LLVMDoubleTypeKind ? "f64"
                                                        : "f128";
    return ny_coerce_to_abi(cg, v, abi_name);
  }
  ny_diag_error(tok, "llvm(...) intrinsic parameter type is not supported");
  ny_diag_hint("supported parameter classes: integer scalars, i1, pointers, f32, f64, f128");
  cg->had_error = 1;
  return ny_c0(cg);
}

static LLVMValueRef ny_llvm_splice_box_result(codegen_t *cg, LLVMValueRef raw,
                                              LLVMTypeRef ret_ty,
                                              token_t tok) {
  if (!cg || !raw || !ret_ty)
    return raw;
  LLVMTypeKind kind = LLVMGetTypeKind(ret_ty);
  if (kind == LLVMVoidTypeKind)
    return ny_c0(cg);
  if (kind == LLVMIntegerTypeKind) {
    unsigned bits = LLVMGetIntTypeWidth(ret_ty);
    if (bits == 1)
      return ny_intrinsic_tag_bool(cg, raw, NY_LLVM_NAME(cg, "llvm_i1_ret"));
    const char *abi_name = ny_llvm_splice_int_abi_name(bits);
    if (!abi_name) {
      ny_diag_error(tok, "llvm(...) does not support i%u intrinsic returns",
                    bits);
      cg->had_error = 1;
      return ny_c0(cg);
    }
    return ny_box_abi_result(cg, raw, abi_name);
  }
  if (kind == LLVMPointerTypeKind)
    return ny_box_abi_result(cg, raw, "ptr");
  if (kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind ||
      kind == LLVMFP128TypeKind) {
    const char *abi_name = kind == LLVMFloatTypeKind    ? "f32"
                           : kind == LLVMDoubleTypeKind ? "f64"
                                                        : "f128";
    return ny_box_abi_result(cg, raw, abi_name);
  }
  ny_diag_error(tok, "llvm(...) intrinsic return type is not supported");
  ny_diag_hint("supported return classes: void, integer scalars, i1, pointers, f32, f64, f128");
  cg->had_error = 1;
  return ny_c0(cg);
}

LLVMValueRef ny_try_direct_llvm_intrinsic(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *e,
                                          const char *callee_name,
                                          bool shadowed, expr_call_t *c) {
  if (!cg || !e || !callee_name || !c || shadowed ||
      strcmp(callee_name, "llvm") != 0)
    return NULL;
  if (c->args.len < 1) {
    ny_diag_error(e->tok, "llvm(...) expects an intrinsic name and arguments");
    ny_diag_hint("use llvm(\"llvm.ctpop.i64\", value)");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  const char *intr_name = NULL;
  size_t intr_len = 0;
  if (!ny_llvm_splice_name_from_expr(c->args.data[0].val, &intr_name,
                                     &intr_len)) {
    ny_diag_error(c->args.data[0].val ? c->args.data[0].val->tok : e->tok,
                  "llvm(...) first argument must be a string literal intrinsic name");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  if (!ny_llvm_splice_name_allowed(intr_name, intr_len)) {
    ny_diag_error(c->args.data[0].val->tok,
                  "llvm(...) intrinsic name must contain only LLVM name characters");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  char prefixed_name[512];
  const char *lookup_name = intr_name;
  size_t lookup_len = intr_len;
  if (intr_len < 5 || strncmp(intr_name, "llvm.", 5) != 0) {
    if (intr_len + 5 >= sizeof(prefixed_name)) {
      ny_diag_error(c->args.data[0].val->tok,
                    "llvm(...) intrinsic name is too long");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    memcpy(prefixed_name, "llvm.", 5);
    memcpy(prefixed_name + 5, intr_name, intr_len);
    prefixed_name[intr_len + 5] = '\0';
    lookup_name = prefixed_name;
    lookup_len = intr_len + 5;
  }

  unsigned id = LLVMLookupIntrinsicID(lookup_name, lookup_len);
  if (id == 0) {
    ny_diag_error(c->args.data[0].val->tok, "unknown LLVM intrinsic '%.*s'",
                  (int)lookup_len, lookup_name);
    cg->had_error = 1;
    return ny_c0(cg);
  }

  LLVMTypeRef overload_types[8];
  size_t overload_count = 0;
  if (LLVMIntrinsicIsOverloaded(id)) {
    overload_count = ny_llvm_splice_collect_overload_types(
        cg, lookup_name, lookup_len, overload_types,
        sizeof(overload_types) / sizeof(overload_types[0]));
    if (overload_count == 0) {
      ny_diag_error(c->args.data[0].val->tok,
                    "overloaded LLVM intrinsic '%.*s' needs a typed intrinsic spelling",
                    (int)lookup_len, lookup_name);
      ny_diag_hint("example: llvm(\"llvm.ctpop.i64\", value)");
      cg->had_error = 1;
      return ny_c0(cg);
    }
  }

  LLVMTypeRef fn_ty =
      LLVMIntrinsicGetType(cg->ctx, id, overload_types, overload_count);
  if (!fn_ty) {
    ny_diag_error(c->args.data[0].val->tok,
                  "could not resolve LLVM intrinsic '%.*s'", (int)lookup_len,
                  lookup_name);
    cg->had_error = 1;
    return ny_c0(cg);
  }

  unsigned want_argc = LLVMCountParamTypes(fn_ty);
  unsigned got_argc = (unsigned)(c->args.len - 1);
  if (got_argc != want_argc) {
    ny_diag_error(e->tok,
                  "llvm(...) intrinsic '%.*s' expects %u argument(s), got %u",
                  (int)lookup_len, lookup_name, want_argc, got_argc);
    cg->had_error = 1;
    return ny_c0(cg);
  }

  LLVMTypeRef *param_tys = NULL;
  LLVMValueRef *argv = NULL;
  if (want_argc > 0) {
    param_tys = alloca(sizeof(*param_tys) * want_argc);
    argv = alloca(sizeof(*argv) * want_argc);
    LLVMGetParamTypes(fn_ty, param_tys);
    for (unsigned i = 0; i < want_argc; i++) {
      expr_t *arg = c->args.data[i + 1].val;
      argv[i] = ny_llvm_splice_coerce_arg(
          cg, scopes, depth, arg, param_tys[i], arg ? arg->tok : e->tok);
      if (!argv[i])
        return ny_c0(cg);
    }
  }

  LLVMValueRef fn =
      LLVMGetIntrinsicDeclaration(cg->module, id, overload_types, overload_count);
  if (!fn) {
    ny_diag_error(e->tok, "could not declare LLVM intrinsic '%.*s'",
                  (int)lookup_len, lookup_name);
    cg->had_error = 1;
    return ny_c0(cg);
  }
  LLVMValueRef raw = LLVMBuildCall2(cg->builder, fn_ty, fn, argv, want_argc,
                                    NY_LLVM_NAME(cg, "llvm_intrinsic"));
  return ny_llvm_splice_box_result(cg, raw, LLVMGetReturnType(fn_ty), e->tok);
}
