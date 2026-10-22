/*
 * Direct emitter from optimized NYIR functions into LLVM IR modules.
 *
 * Provides a canonical bridge from NYIR into LLVM-C representation,
 * enabling LLVM machine codegen to consume all NYIR-level optimizations
 * (alias analysis, load forwarding, store sinking, bounds/tag check
 * elimination) without divergent front-end AST lowering.
 */
#include "base/options.h"
#include "code/backend/abi.h"
#include "code/backend/internal.h"
#include "code/backend/llvm/bridge.h"
#include <inttypes.h>
#include <limits.h>
#include <llvm-c/Core.h>
#include <nyr/ir.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline LLVMValueRef ny_llvm_c0(ny_llvm_context_t *context) {
  return LLVMConstInt(context->type_i64, 0, false);
}

static inline LLVMValueRef ny_as_i64(ny_llvm_context_t *cg, LLVMValueRef val) {
  if (!val)
    return ny_llvm_c0(cg);
  LLVMTypeRef ty = LLVMTypeOf(val);
  if (!ty)
    return ny_llvm_c0(cg);
  if (ty == cg->type_i64)
    return val;
  LLVMTypeKind kind = LLVMGetTypeKind(ty);
  if (kind == LLVMVoidTypeKind)
    return ny_llvm_c0(cg);
  if (kind == LLVMIntegerTypeKind) {
    unsigned bits = LLVMGetIntTypeWidth(ty);
    if (bits < 64)
      return LLVMBuildZExt(cg->builder, val, cg->type_i64, "");
    if (bits > 64)
      return LLVMBuildTrunc(cg->builder, val, cg->type_i64, "");
    return val;
  }
  if (kind == LLVMPointerTypeKind)
    return LLVMBuildPtrToInt(cg->builder, val, cg->type_i64, "");
  if (ty == cg->type_f64)
    return LLVMBuildBitCast(cg->builder, val, cg->type_i64, "");
  if (ty == cg->type_f32) {
    LLVMValueRef ext = LLVMBuildFPExt(cg->builder, val, cg->type_f64, "");
    return LLVMBuildBitCast(cg->builder, ext, cg->type_i64, "");
  }
  return val;
}

static LLVMValueRef ny_llvm_srem_const_i64(ny_llvm_context_t *cg,
                                           LLVMValueRef lhs, int64_t divisor) {
  uint64_t abs_divisor =
      divisor < 0 ? (uint64_t)(-(divisor + 1)) + 1 : (uint64_t)divisor;
  int64_t magic;
  unsigned shift;
  nyr_sdiv_magic_i64(abs_divisor, &magic, &shift);

  LLVMTypeRef type_i128 =
      LLVMInt128TypeInContext(LLVMGetModuleContext(cg->module));
  LLVMValueRef x = ny_as_i64(cg, lhs);
  LLVMValueRef x128 = LLVMBuildSExt(cg->builder, x, type_i128, "");
  LLVMValueRef m = LLVMConstInt(cg->type_i64, (uint64_t)magic, false);
  LLVMValueRef m128 = LLVMBuildSExt(cg->builder, m, type_i128, "");
  LLVMValueRef product = LLVMBuildMul(cg->builder, x128, m128, "");
  LLVMValueRef high128 = LLVMBuildAShr(cg->builder, product,
                                       LLVMConstInt(type_i128, 64, false), "");
  LLVMValueRef quotient =
      LLVMBuildTrunc(cg->builder, high128, cg->type_i64, "");
  if (magic < 0)
    quotient = LLVMBuildAdd(cg->builder, quotient, x, "");
  if (shift)
    quotient = LLVMBuildAShr(cg->builder, quotient,
                             LLVMConstInt(cg->type_i64, shift, false), "");
  LLVMValueRef sign =
      LLVMBuildAShr(cg->builder, x, LLVMConstInt(cg->type_i64, 63, false), "");
  quotient = LLVMBuildSub(cg->builder, quotient, sign, "");
  if (divisor < 0)
    quotient = LLVMBuildNeg(cg->builder, quotient, "");
  LLVMValueRef d = LLVMConstInt(cg->type_i64, (uint64_t)divisor, false);
  return LLVMBuildSub(cg->builder, x,
                      LLVMBuildMul(cg->builder, quotient, d, ""), "");
}

static inline LLVMValueRef ny_as_f64(ny_llvm_context_t *cg, LLVMValueRef val) {
  if (!val)
    return LLVMConstReal(cg->type_f64, 0.0);
  LLVMTypeRef ty = LLVMTypeOf(val);
  if (!ty)
    return LLVMConstReal(cg->type_f64, 0.0);
  if (ty == cg->type_f64)
    return val;
  LLVMTypeKind kind = LLVMGetTypeKind(ty);
  if (kind == LLVMVoidTypeKind)
    return LLVMConstReal(cg->type_f64, 0.0);
  if (ty == cg->type_f32)
    return LLVMBuildFPExt(cg->builder, val, cg->type_f64, "");
  if (ty == cg->type_i64)
    return LLVMBuildBitCast(cg->builder, val, cg->type_f64, "");
  if (kind == LLVMIntegerTypeKind)
    return LLVMBuildSIToFP(cg->builder, val, cg->type_f64, "");
  return val;
}

static inline LLVMValueRef ny_as_f32(ny_llvm_context_t *cg, LLVMValueRef val) {
  if (!val)
    return LLVMConstReal(cg->type_f32, 0.0);
  LLVMTypeRef ty = LLVMTypeOf(val);
  if (ty == cg->type_f32)
    return val;
  if (ty == cg->type_f64)
    return LLVMBuildFPTrunc(cg->builder, val, cg->type_f32, "");
  if (ty == cg->type_i64) {
    LLVMValueRef f64v = LLVMBuildBitCast(cg->builder, val, cg->type_f64, "");
    return LLVMBuildFPTrunc(cg->builder, f64v, cg->type_f32, "");
  }
  if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind)
    return LLVMBuildSIToFP(cg->builder, val, cg->type_f32, "");
  return val;
}

static LLVMTypeRef ny_llvm_aggregate_return_type(ny_llvm_context_t *cg,
                                                 uint32_t size,
                                                 const uint8_t classes[2]) {
  if (!cg || !classes || size == 0 || size > 16)
    return NULL;
  unsigned pieces = (size + 7u) / 8u;
  LLVMTypeRef fields[2] = {NULL, NULL};
  for (unsigned i = 0; i < pieces; ++i) {
    unsigned bytes = size - i * 8u;
    if (bytes > 8u)
      bytes = 8u;
    if (classes[i] == NYIR_ARG_CLASS_INTEGER) {
      unsigned bits = 8;
      while (bits < bytes * 8u)
        bits <<= 1u;
      fields[i] = LLVMIntTypeInContext(cg->context, bits);
    } else if (classes[i] == NYIR_ARG_CLASS_SSE) {
      fields[i] = bytes >= 8u   ? cg->type_f64
                  : bytes >= 4u ? cg->type_f32
                                : NULL;
    }
    if (!fields[i])
      return NULL;
  }
  return LLVMStructTypeInContext(cg->context, fields, pieces, false);
}

static int ny_llvm_capture_return_piece(const nyr_inst_t *call,
                                        int64_t selector) {
  if (!call || !call->ret_aggregate_size)
    return -1;
  unsigned pieces = (call->ret_aggregate_size + 7u) / 8u;
  int integer_piece[2] = {-1, -1};
  int sse_piece[2] = {-1, -1};
  unsigned integers = 0, sses = 0;
  for (unsigned i = 0; i < pieces && i < 2; ++i) {
    if (call->ret_aggregate_classes[i] == NYIR_ARG_CLASS_INTEGER &&
        integers < 2)
      integer_piece[integers++] = (int)i;
    else if (call->ret_aggregate_classes[i] == NYIR_ARG_CLASS_SSE && sses < 2)
      sse_piece[sses++] = (int)i;
  }
  switch (selector) {
  case 0:
    return integers > 1 ? integer_piece[1] : -1; /* RDX */
  case 1:
    return integers > 0 ? integer_piece[0] : -1; /* RAX */
  case 2:
    return sses > 0 ? sse_piece[0] : -1; /* XMM0 */
  case 3:
    return sses > 1 ? sse_piece[1] : -1; /* XMM1 */
  default:
    return -1;
  }
}

static LLVMValueRef ny_llvm_emit_f64_unary_runtime(ny_llvm_context_t *cg,
                                                   const char *name,
                                                   LLVMValueRef value) {
  LLVMTypeRef params[] = {cg->type_f64};
  LLVMTypeRef fn_ty = LLVMFunctionType(cg->type_f64, params, 1, false);
  LLVMValueRef fn = LLVMGetNamedFunction(cg->module, name);
  if (!fn) {
    fn = LLVMAddFunction(cg->module, name, fn_ty);
    LLVMSetLinkage(fn, LLVMExternalLinkage);
  }
  LLVMValueRef arg = ny_as_f64(cg, value);
  return LLVMBuildCall2(cg->builder, fn_ty, fn, &arg, 1, "");
}

static void ny_llvm_set_nyir_debug_loc(ny_llvm_context_t *cg,
                                       LLVMMetadataRef function_scope,
                                       const nyr_inst_t *in) {
  if (!cg || !cg->debug_symbols || !cg->di_builder || !in) {
    if (cg && cg->builder)
      LLVMSetCurrentDebugLocation2(cg->builder, NULL);
    return;
  }
  if (!in->debug.line) {
    cg->di_loc = NULL;
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
    return;
  }
  const char *filename = in->debug.file;
  unsigned line = in->debug.line;
  unsigned column = in->debug.column ? in->debug.column : 1;
  LLVMMetadataRef scope =
      cg->debug_scope ? cg->debug_scope(cg->userdata, filename, line, column)
                      : NULL;
  if (!scope)
    scope = function_scope;
  LLVMMetadataRef loc =
      LLVMDIBuilderCreateDebugLocation(cg->context, line, column, scope, NULL);
  cg->di_loc = loc;
  LLVMSetCurrentDebugLocation2(cg->builder, loc);
}

static LLVMTypeRef ny_llvm_vector_type(ny_llvm_context_t *cg, nyr_op_t op) {
  if (!cg)
    return NULL;
  if (op == NYIR_VEC4_LOAD_F64 || op == NYIR_VEC4_STORE_F64 ||
      op == NYIR_VEC4_ADD_F64 || op == NYIR_VEC4_SUB_F64 ||
      op == NYIR_VEC4_MUL_F64 || op == NYIR_VEC4_DIV_F64 ||
      op == NYIR_VEC4_FMA_F64 || op == NYIR_VEC4_SET1_F64 ||
      op == NYIR_VEC4_SHUFFLE_F64 || op == NYIR_VEC4_REDUCE_ADD_F64)
    return LLVMVectorType(cg->type_f64, 4);
  if (op == NYIR_VEC8_LOAD_F32 || op == NYIR_VEC8_STORE_F32 ||
      op == NYIR_VEC8_ADD_F32 || op == NYIR_VEC8_SUB_F32 ||
      op == NYIR_VEC8_MUL_F32 || op == NYIR_VEC8_DIV_F32 ||
      op == NYIR_VEC8_FMA_F32 || op == NYIR_VEC8_SET1_F32 ||
      op == NYIR_VEC8_SHUFFLE_F32)
    return LLVMVectorType(cg->type_f32, 4);
  if (op == NYIR_VEC4_LOAD_I64 || op == NYIR_VEC4_STORE_I64 ||
      op == NYIR_VEC4_ADD_I64 || op == NYIR_VEC4_SUB_I64 ||
      op == NYIR_VEC4_SET1_I64 || op == NYIR_VEC4_AND_I64 ||
      op == NYIR_VEC4_OR_I64 || op == NYIR_VEC4_XOR_I64 ||
      op == NYIR_VEC4_SHL_I64 || op == NYIR_VEC4_SAR_I64 ||
      op == NYIR_VEC4_REDUCE_ADD_I64)
    return LLVMVectorType(cg->type_i64, 2);
  if (op == NYIR_VEC8_LOAD_I64 || op == NYIR_VEC8_STORE_I64 ||
      op == NYIR_VEC8_ADD_I64 || op == NYIR_VEC8_SUB_I64 ||
      op == NYIR_VEC8_AND_I64 || op == NYIR_VEC8_OR_I64 ||
      op == NYIR_VEC8_XOR_I64 || op == NYIR_VEC8_REDUCE_ADD_I64)
    return LLVMVectorType(cg->type_i64, 4);
  return NULL;
}

static int ny_llvm_vector_root(int *parents, int value) {
  int root = value;
  while (parents[root] != root)
    root = parents[root];
  while (parents[value] != value) {
    int next = parents[value];
    parents[value] = root;
    value = next;
  }
  return root;
}

static void ny_llvm_vector_union(int *parents, int left, int right) {
  int left_root = ny_llvm_vector_root(parents, left);
  int right_root = ny_llvm_vector_root(parents, right);
  if (left_root != right_root)
    parents[right_root] = left_root;
}

static bool ny_llvm_vector_mark(LLVMTypeRef *types, int *parents, size_t count,
                                int value, LLVMTypeRef type) {
  if (value < 0 || (size_t)value >= count || !type)
    return true;
  int root = ny_llvm_vector_root(parents, value);
  if (types[root] && types[root] != type)
    return false;
  types[root] = type;
  return true;
}

static bool ny_llvm_infer_vector_value_types(ny_llvm_context_t *cg,
                                             const nyr_func_t *f,
                                             LLVMTypeRef *types, size_t count) {
  if (count > INT_MAX)
    return false;
  int *parents = malloc(count * sizeof(*parents));
  if (!parents)
    return false;
  for (size_t i = 0; i < count; ++i)
    parents[i] = (int)i;

  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
        (size_t)in->dst < count && (size_t)in->a < count) {
      ny_llvm_vector_union(parents, in->dst, in->a);
    } else if (in->op == NYIR_PHI && in->dst >= 0 && (size_t)in->dst < count) {
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int incoming = in->phi_incoming[k].value;
        if (incoming >= 0 && (size_t)incoming < count)
          ny_llvm_vector_union(parents, in->dst, incoming);
      }
    }
  }

  bool valid = true;
  for (size_t i = 0; i < f->len && valid; ++i) {
    const nyr_inst_t *in = &f->data[i];
    LLVMTypeRef type = ny_llvm_vector_type(cg, in->op);
    if (!type)
      continue;
    switch (in->op) {
    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC8_LOAD_F32:
    case NYIR_VEC4_LOAD_I64:
    case NYIR_VEC8_LOAD_I64:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC8_SET1_F32:
    case NYIR_VEC4_SET1_I64:
      valid = ny_llvm_vector_mark(types, parents, count, in->dst, type);
      break;
    case NYIR_VEC4_STORE_F64:
    case NYIR_VEC8_STORE_F32:
    case NYIR_VEC4_STORE_I64:
    case NYIR_VEC8_STORE_I64:
      valid = ny_llvm_vector_mark(types, parents, count,
                                  in->b >= 0 ? in->b : in->c, type);
      break;
    case NYIR_VEC4_REDUCE_ADD_F64:
    case NYIR_VEC4_REDUCE_ADD_I64:
    case NYIR_VEC8_REDUCE_ADD_I64:
      valid = ny_llvm_vector_mark(types, parents, count, in->b, type);
      break;
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC8_SHUFFLE_F32:
      valid = ny_llvm_vector_mark(types, parents, count, in->dst, type) &&
              ny_llvm_vector_mark(types, parents, count, in->a, type);
      break;
    case NYIR_VEC4_FMA_F64:
    case NYIR_VEC8_FMA_F32:
      valid = ny_llvm_vector_mark(types, parents, count, in->dst, type) &&
              ny_llvm_vector_mark(types, parents, count, in->a, type) &&
              ny_llvm_vector_mark(types, parents, count, in->b, type) &&
              ny_llvm_vector_mark(types, parents, count, in->c, type);
      break;
    default:
      valid = ny_llvm_vector_mark(types, parents, count, in->dst, type) &&
              ny_llvm_vector_mark(types, parents, count, in->a, type) &&
              ny_llvm_vector_mark(types, parents, count, in->b, type);
      break;
    }
  }

  if (valid)
    for (size_t i = 0; i < count; ++i)
      types[i] = types[ny_llvm_vector_root(parents, (int)i)];
  free(parents);
  return valid;
}

static LLVMValueRef ny_llvm_vector_slot(ny_llvm_context_t *cg,
                                        LLVMValueRef *vals, size_t val_count,
                                        int value, LLVMTypeRef type) {
  (void)cg;
  if (value >= 0 && (size_t)value < val_count && vals[value] &&
      LLVMTypeOf(vals[value]) == type)
    return vals[value];
  return LLVMGetUndef(type);
}

static LLVMValueRef ny_llvm_vector_splat(ny_llvm_context_t *cg,
                                         LLVMValueRef scalar,
                                         LLVMTypeRef vector_type) {
  unsigned lanes = LLVMGetVectorSize(vector_type);
  LLVMTypeRef scalar_type = LLVMGetElementType(vector_type);
  LLVMValueRef value = scalar;
  if (!value || LLVMTypeOf(value) != scalar_type)
    value = LLVMGetUndef(scalar_type);
  LLVMValueRef result = LLVMGetUndef(vector_type);
  LLVMTypeRef index_type = LLVMInt32TypeInContext(cg->context);
  for (unsigned i = 0; i < lanes; ++i) {
    LLVMValueRef index = LLVMConstInt(index_type, i, false);
    result = LLVMBuildInsertElement(cg->builder, result, value, index, "");
  }
  return result;
}

static LLVMValueRef ny_llvm_vector_fma(ny_llvm_context_t *cg, LLVMValueRef a,
                                       LLVMValueRef b, LLVMValueRef c,
                                       LLVMTypeRef vector_type) {
  static const char name[] = "llvm.fma";
  unsigned id = LLVMLookupIntrinsicID(name, sizeof(name) - 1);
  if (!cg || !id || !a || !b || !c || !vector_type)
    return NULL;
  LLVMValueRef fn =
      LLVMGetIntrinsicDeclaration(cg->module, id, &vector_type, 1);
  if (!fn)
    return NULL;
  LLVMValueRef args[] = {a, b, c};
  return LLVMBuildCall2(cg->builder, LLVMGlobalGetValueType(fn), fn, args, 3,
                        "");
}

static LLVMValueRef ny_llvm_vector_shuffle(ny_llvm_context_t *cg,
                                           LLVMValueRef value, LLVMTypeRef type,
                                           nyr_op_t op, int64_t mask) {
  unsigned lanes = LLVMGetVectorSize(type);
  LLVMValueRef *indices = calloc(lanes, sizeof(*indices));
  if (!indices)
    return LLVMGetUndef(type);
  LLVMTypeRef index_type = LLVMInt32TypeInContext(cg->context);
  for (unsigned i = 0; i < lanes; ++i) {
    unsigned selected;
    if (op == NYIR_VEC4_SHUFFLE_F64) {
      selected = ((uint64_t)mask >> (i * 2u)) & 3u;
    } else if (lanes == 2) {
      selected =
          i == 0 ? ((uint64_t)mask & 1u) : 2u + (((uint64_t)mask >> 1) & 1u);
    } else {
      unsigned shift = i < 2 ? i * 2u : 4u + (i - 2u) * 2u;
      selected = ((uint64_t)mask >> shift) & 3u;
      if (i >= 2)
        selected += 2u;
    }
    indices[i] = LLVMConstInt(index_type, selected, false);
  }
  LLVMValueRef mask_value = LLVMConstVector(indices, lanes);
  free(indices);
  return LLVMBuildShuffleVector(cg->builder, value, value, mask_value, "");
}

static const char *ny_llvm_nyir_origin_section(const ny_llvm_context_t *context,
                                               const char *name, bool global) {
  if (!name)
    return NULL;
  const char *origin = strcmp(name, "_ny_top_entry") == 0 ? "ny.user"
                       : context && context->origin_section
                           ? context->origin_section(context->userdata, name)
                           : NULL;
  if (!origin)
    return NULL;
#ifdef __APPLE__
  if (strcmp(origin, "ny.std") == 0)
    return global ? "__DATA,ny_std" : "__TEXT,ny_std";
  if (strcmp(origin, "ny.user") == 0)
    return global ? "__DATA,ny_user" : "__TEXT,ny_user";
#else
  if (strcmp(origin, "ny.std") == 0)
    return global ? "ny.std.data" : "ny.std.text";
  if (strcmp(origin, "ny.user") == 0)
    return global ? "ny.user.data" : "ny.user.text";
#endif
  return origin;
}

bool ny_llvm_emit_nyir_func(ny_llvm_context_t *cg, nyr_func_t *f,
                            const char *name, bool tag_return, char *err,
                            size_t err_len) {
  (void)tag_return;
  if (!cg || !f || !name || !*name) {
    if (err && err_len > 0)
      snprintf(err, err_len, "Invalid arguments to ny_llvm_emit_nyir_func");
    return false;
  }
  nyr_reconcile_local_scalar_classes(f);
  char verify_err[512] = {0};
  if (!nyr_ir_verify(f, verify_err, sizeof(verify_err))) {
    if (getenv("NY_DUMP_FAIL_FN"))
      nyr_dump(stderr, f, name);
    if (err && err_len > 0)
      snprintf(err, err_len, "LLVM NYIR ABI verification for %s failed: %s",
               name, verify_err);
    return false;
  }

  /*
   * Native object backends consistently keep source functions in the
   * `ny_fn_` namespace. LLVM must use the same namespace: runtime helpers
   * and unqualified C imports keep their public names, while a source
   * function such as `len` can shadow `rt_len` safely.
   */
  char function_name[512];
  const char *llvm_name = name;
  if (strcmp(name, "_ny_top_entry") != 0) {
    int n = snprintf(function_name, sizeof(function_name), "ny_fn_%s", name);
    if (n < 0 || (size_t)n >= sizeof(function_name)) {
      if (err && err_len > 0)
        snprintf(err, err_len,
                 "LLVM NYIR emission: function symbol too long: %s", name);
      return false;
    }
    llvm_name = function_name;
  }

  /*
   * 1. Find or declare function in LLVM module
   */
  for (size_t i = 0; i < f->param_count; ++i) {
    if (f->param_types && f->param_types[i] == NYIR_PARAM_AGGREGATE) {
      if (err && err_len > 0)
        snprintf(err, err_len,
                 "LLVM NYIR emission: aggregate parameters are not supported "
                 "for %s",
                 name);
      return false;
    }
  }
  if (f->return_aggregate_size) {
    if (err && err_len > 0)
      snprintf(err, err_len,
               "LLVM NYIR emission: aggregate returns are not supported for %s",
               name);
    return false;
  }
  LLVMValueRef fn = LLVMGetNamedFunction(cg->module, llvm_name);
  size_t param_count = f->param_count;
  LLVMTypeRef *param_tys = malloc(sizeof(LLVMTypeRef) * (param_count + 1));
  for (size_t i = 0; i < param_count; ++i) {
    if (f->param_types && i < f->param_count) {
      if (f->param_types[i] == NYIR_PARAM_F64)
        param_tys[i] = cg->type_f64;
      else if (f->param_types[i] == NYIR_PARAM_F32)
        param_tys[i] = cg->type_f32;
      else
        param_tys[i] = cg->type_i64;
    } else {
      param_tys[i] = cg->type_i64;
    }
  }
  LLVMTypeRef ret_ty = cg->type_i64;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_RET) {
      if (f->data[i].scalar_class == NYIR_SCALAR_F64)
        ret_ty = cg->type_f64;
      else if (f->data[i].scalar_class == NYIR_SCALAR_F32)
        ret_ty = cg->type_f32;
      break;
    }
  }
  LLVMTypeRef fn_ty =
      LLVMFunctionType(ret_ty, param_tys, (unsigned)param_count, false);
  free(param_tys);

  if (fn) {
    if (LLVMGlobalGetValueType(fn) != fn_ty) {
      LLVMValueRef new_fn = LLVMAddFunction(cg->module, "", fn_ty);
      LLVMReplaceAllUsesWith(fn, new_fn);
      LLVMDeleteFunction(fn);
      LLVMSetValueName2(new_fn, llvm_name, strlen(llvm_name));
      fn = new_fn;
    } else {
      ny_llvm_clear_function(fn);
    }
  } else {
    fn = LLVMAddFunction(cg->module, llvm_name, fn_ty);
    LLVMSetLinkage(fn, LLVMExternalLinkage);
  }
  const char *origin_section = ny_llvm_nyir_origin_section(cg, name, false);
  if (origin_section)
    LLVMSetSection(fn, origin_section);

  LLVMMetadataRef previous_scope = cg->di_scope;
  LLVMMetadataRef previous_loc = cg->di_loc;
  LLVMMetadataRef function_scope = previous_scope;
  if (cg->debug_symbols && cg->di_builder && cg->debug_subprogram) {
    const nyr_debug_loc_t *debug = NULL;
    for (size_t i = 0; i < f->len; ++i)
      if (f->data[i].debug.line) {
        debug = &f->data[i].debug;
        break;
      }
    const char *filename = debug && debug->file && *debug->file
                               ? debug->file
                               : cg->default_filename;
    unsigned line = debug ? debug->line : 1;
    unsigned column = debug && debug->column ? debug->column : 1;
    LLVMMetadataRef subprogram =
        cg->debug_subprogram(cg->userdata, fn, name, filename, line, column);
    if (subprogram) {
      function_scope = subprogram;
      cg->di_scope = subprogram;
      cg->di_loc = NULL;
    }
  }

  /*
   * 2. Collect max label ID and map basic blocks
   */
  int64_t max_label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL && in->imm > max_label)
      max_label = in->imm;
    if ((in->op == NYIR_BR || in->op == NYIR_BR_IF) && in->imm > max_label)
      max_label = in->imm;
  }

  size_t num_blocks = (max_label >= 0) ? (size_t)(max_label + 1) : 0;
  LLVMBasicBlockRef *blocks = calloc(num_blocks + 1, sizeof(LLVMBasicBlockRef));
  /*
   * Native NYIR blocks can lower into several LLVM blocks (for example, a
   * checked list access introduces bcheck_fail/bcheck_cont).  PHI incoming
   * labels name NYIR blocks, so remember the actual LLVM fallthrough tail
   * and any explicit branch source for each NYIR label.
   */
  LLVMBasicBlockRef *block_tails =
      calloc(num_blocks ? num_blocks : 1, sizeof(*block_tails));
  LLVMBasicBlockRef *block_branch_sources =
      calloc(num_blocks ? num_blocks : 1, sizeof(*block_branch_sources));
  int64_t *block_branch_targets =
      malloc((num_blocks ? num_blocks : 1) * sizeof(*block_branch_targets));
  if (!blocks || !block_tails || !block_branch_sources ||
      !block_branch_targets) {
    free(blocks);
    free(block_tails);
    free(block_branch_sources);
    free(block_branch_targets);
    if (err && err_len > 0)
      snprintf(err, err_len,
               "LLVM NYIR emission: out of memory mapping CFG blocks");
    return false;
  }
  if (block_branch_targets)
    for (size_t i = 0; i < (num_blocks ? num_blocks : 1); ++i)
      block_branch_targets[i] = -1;
  for (size_t i = 0; i < num_blocks; ++i) {
    char lbl_buf[64];
    snprintf(lbl_buf, sizeof(lbl_buf), "L%" PRId64, (int64_t)i);
    blocks[i] = LLVMAppendBasicBlockInContext(cg->context, fn, lbl_buf);
  }

  LLVMBasicBlockRef entry_bb =
      LLVMAppendBasicBlockInContext(cg->context, fn, "entry");
  LLVMBasicBlockRef first_bb = LLVMGetFirstBasicBlock(fn);
  if (first_bb && first_bb != entry_bb)
    LLVMMoveBasicBlockBefore(entry_bb, first_bb);

  LLVMPositionBuilderAtEnd(cg->builder, entry_bb);

  /*
   * 3. Allocate local variable storage in entry block
   *
   * Every slot is zero-initialized: the language defines uninitialized
   * locals as zero (the baseline stack emitter's zeroed frame provides
   * this for free).  Leaving the allocas undef let LLVM fold loop-head
   * loads of not-yet-stored slots into arbitrary constants, and mapcat
   * read its element 1 as 0.
   */
  size_t local_count = nyr_local_count(f);
  LLVMValueRef *local_allocas = calloc(local_count + 1, sizeof(LLVMValueRef));
  LLVMValueRef slot_zero = LLVMConstInt(cg->type_i64, 0, false);
  for (size_t i = 0; i < local_count; ++i) {
    char slot_name[32];
    snprintf(slot_name, sizeof(slot_name), "local_slot_%zu", i);
    local_allocas[i] = LLVMBuildAlloca(cg->builder, cg->type_i64, slot_name);
    LLVMBuildStore(cg->builder, slot_zero, local_allocas[i]);
  }

  /*
   * 4. Value storage table
   */
  size_t max_v = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst >= 0 && (size_t)in->dst > max_v)
      max_v = (size_t)in->dst;
    if (in->a >= 0 && (size_t)in->a > max_v)
      max_v = (size_t)in->a;
    if (in->b >= 0 && (size_t)in->b > max_v)
      max_v = (size_t)in->b;
    if (in->c >= 0 && (size_t)in->c > max_v)
      max_v = (size_t)in->c;
    if (in->d >= 0 && (size_t)in->d > max_v)
      max_v = (size_t)in->d;
    if (in->e >= 0 && (size_t)in->e > max_v)
      max_v = (size_t)in->e;
    if (in->f >= 0 && (size_t)in->f > max_v)
      max_v = (size_t)in->f;
  }
  if (f->next_value > 0 && (size_t)f->next_value > max_v)
    max_v = (size_t)f->next_value;
  size_t val_count = max_v + 32;
  LLVMValueRef *vals = calloc(val_count + 1, sizeof(LLVMValueRef));
  LLVMTypeRef *value_types = calloc(val_count + 1, sizeof(LLVMTypeRef));
  if (!vals || !value_types ||
      !ny_llvm_infer_vector_value_types(cg, f, value_types, val_count)) {
    free(vals);
    free(value_types);
    free(local_allocas);
    free(blocks);
    free(block_tails);
    free(block_branch_sources);
    free(block_branch_targets);
    if (err && err_len > 0)
      snprintf(err, err_len,
               "LLVM NYIR emission: cannot infer consistent vector value "
               "types for %s",
               name);
    return false;
  }

  /*
   * Fixed NYIR stack objects have function lifetime even when the NYIR
   * instruction sits in a loop/conditional. Native object backends reserve
   * them in the fixed frame; mirror that contract by materializing every
   * NYIR_ALLOCA in the LLVM entry block up front. This prevents repeated
   * dynamic stack growth when an allocation site is executed in a loop.
   */
  if (vals) {
    LLVMTypeRef i8_type = LLVMInt8TypeInContext(cg->context);
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_ALLOCA || in->dst < 0 ||
          (size_t)in->dst >= val_count || vals[in->dst])
        continue;
      LLVMValueRef bytes = LLVMConstInt(
          cg->type_i64, (uint64_t)(in->imm > 0 ? in->imm : 1), false);
      LLVMValueRef storage =
          LLVMBuildArrayAlloca(cg->builder, i8_type, bytes, "nyr_alloca");
      LLVMSetAlignment(storage, 16);
      vals[in->dst] = LLVMBuildPtrToInt(cg->builder, storage, cg->type_i64, "");
    }
  }

  /*
   * Parameters occupy local slots, not SSA IDs. An instruction result can
   * have the same numeric ID as a parameter; seeding vals here would replace
   * entry-block stack objects before their first use.
   */
  for (size_t i = 0; i < f->param_count && i < local_count; ++i) {
    LLVMValueRef pval = LLVMGetParam(fn, (unsigned)i);
    LLVMBuildStore(cg->builder, ny_as_i64(cg, pval), local_allocas[i]);
  }

  bool entry_terminated = false;
  /*
   * NYIR transformations may leave dead instructions after a terminator.
   * They are harmless in NYIR, but LLVM requires a terminator to be the last
   * instruction in its basic block.  Drop that dead tail until the next
   * label; conditional branches create an explicit fallthrough block below,
   * so they do not terminate the insertion block.
   */
  bool block_terminated = false;
  bool unsupported = false;
  int64_t current_nyir_label = -1;

  /*
   * Track PHI nodes to resolve incoming branches in a second pass
   */
  size_t phi_count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_PHI)
      phi_count++;
  }

  /*
   * 5. Lower instructions
   */
  LLVMValueRef last_aggregate_return = NULL;
  uint32_t last_aggregate_return_size = 0;
  uint8_t last_aggregate_return_classes[2] = {0, 0};
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP)
      continue;

    ny_llvm_set_nyir_debug_loc(cg, function_scope, in);

    if (in->op == NYIR_LABEL) {
      if (in->imm >= 0 && (size_t)in->imm < num_blocks && blocks[in->imm]) {
        LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
        if (current_nyir_label >= 0 &&
            (size_t)current_nyir_label < num_blocks) {
          block_tails[current_nyir_label] = cur_bb;
        }
        if (!entry_terminated) {
          LLVMBuildBr(cg->builder, blocks[in->imm]);
          entry_terminated = true;
        } else if (cur_bb && !LLVMGetBasicBlockTerminator(cur_bb)) {
          LLVMBuildBr(cg->builder, blocks[in->imm]);
        }
        LLVMPositionBuilderAtEnd(cg->builder, blocks[in->imm]);
        current_nyir_label = in->imm;
        block_terminated = false;
      }
      continue;
    }

    if (block_terminated)
      continue;

    if (!entry_terminated)
      entry_terminated = true;

    switch (in->op) {
    case NYIR_CONST_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count)
        vals[in->dst] = LLVMConstInt(cg->type_i64, (uint64_t)in->imm, false);
      break;

    case NYIR_CONST_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        union {
          int64_t i;
          double d;
        } u;
        u.i = in->imm;
        vals[in->dst] = LLVMConstReal(cg->type_f64, u.d);
      }
      break;

    case NYIR_CONST_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        union {
          int32_t i;
          float f;
        } u;
        u.i = (int32_t)in->imm;
        vals[in->dst] = LLVMConstReal(cg->type_f32, u.f);
      }
      break;

    case NYIR_COPY:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        vals[in->dst] = (in->a >= 0 && (size_t)in->a < val_count)
                            ? vals[in->a]
                            : ny_llvm_c0(cg);
      }
      break;

    case NYIR_PHI:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMTypeRef phi_type =
            value_types[in->dst] ? value_types[in->dst] : cg->type_i64;
        vals[in->dst] = LLVMBuildPhi(cg->builder, phi_type, "phi");
      }
      break;

    case NYIR_ADD_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] =
            LLVMBuildAdd(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_SUB_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] =
            LLVMBuildSub(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_MUL_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] =
            LLVMBuildMul(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_DIV_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildSDiv(cg->builder, ny_as_i64(cg, va),
                                      ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_MOD_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMValueRef divisor = ny_as_i64(cg, vb);
        if (LLVMIsAConstantInt(divisor)) {
          int64_t constant = LLVMConstIntGetSExtValue(divisor);
          /*
           * 0 retains the normal LLVM trap/poison behaviour; INT64_MIN has
           * no positive magnitude representable by the magic routine.
           */
          if (constant != 0 && constant != 1 && constant != -1 &&
              constant != INT64_MIN)
            vals[in->dst] = ny_llvm_srem_const_i64(cg, va, constant);
          else
            vals[in->dst] =
                LLVMBuildSRem(cg->builder, ny_as_i64(cg, va), divisor, "");
        } else {
          vals[in->dst] =
              LLVMBuildSRem(cg->builder, ny_as_i64(cg, va), divisor, "");
        }
      }
      break;

    case NYIR_AND_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] =
            LLVMBuildAnd(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_OR_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] =
            LLVMBuildOr(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_XOR_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] =
            LLVMBuildXor(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_SHL_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMValueRef count = LLVMBuildAnd(cg->builder, ny_as_i64(cg, vb),
                                          LLVMConstInt(cg->type_i64, 63, false),
                                          "nyr_shl_count");
        vals[in->dst] = LLVMBuildShl(cg->builder, ny_as_i64(cg, va), count, "");
      }
      break;

    case NYIR_SAR_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMValueRef count = LLVMBuildAnd(cg->builder, ny_as_i64(cg, vb),
                                          LLVMConstInt(cg->type_i64, 63, false),
                                          "nyr_sar_count");
        vals[in->dst] =
            LLVMBuildAShr(cg->builder, ny_as_i64(cg, va), count, "");
      }
      break;

    case NYIR_ROR_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count)
                              ? ny_as_i64(cg, vals[in->a])
                              : ny_llvm_c0(cg);
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count)
                              ? ny_as_i64(cg, vals[in->b])
                              : ny_llvm_c0(cg);
        const char *ror_name = "llvm.fshr.i64";
        unsigned ror_id = LLVMLookupIntrinsicID(ror_name, strlen(ror_name));
        LLVMTypeRef overload = cg->type_i64;
        LLVMTypeRef ror_fn_ty =
            LLVMIntrinsicGetType(cg->context, ror_id, &overload, 1);
        LLVMValueRef ror_fn =
            LLVMGetIntrinsicDeclaration(cg->module, ror_id, &overload, 1);
        LLVMValueRef argv[3] = {va, va, vb};
        vals[in->dst] =
            (ror_id != 0 && ror_fn_ty && ror_fn)
                ? LLVMBuildCall2(cg->builder, ror_fn_ty, ror_fn, argv, 3, "")
                : NULL;
        if (!vals[in->dst]) {
          LLVMValueRef sh1 = LLVMBuildLShr(cg->builder, va, vb, "");
          LLVMValueRef diff = LLVMBuildSub(
              cg->builder, LLVMConstInt(cg->type_i64, 64, false), vb, "");
          LLVMValueRef sh2 = LLVMBuildShl(cg->builder, va, diff, "");
          vals[in->dst] = LLVMBuildOr(cg->builder, sh1, sh2, "");
        }
      }
      break;

    case NYIR_ROR32_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count)
                              ? ny_as_i64(cg, vals[in->a])
                              : ny_llvm_c0(cg);
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count)
                              ? ny_as_i64(cg, vals[in->b])
                              : ny_llvm_c0(cg);
        LLVMValueRef mask =
            LLVMConstInt(cg->type_i64, UINT64_C(0xffffffff), false);
        va = LLVMBuildAnd(cg->builder, va, mask, "");
        vb = LLVMBuildAnd(cg->builder, vb,
                          LLVMConstInt(cg->type_i64, 31, false), "");
        LLVMTypeRef i32 = LLVMInt32TypeInContext(cg->context);
        LLVMValueRef va32 = LLVMBuildTrunc(cg->builder, va, i32, "");
        LLVMValueRef vb32 = LLVMBuildTrunc(cg->builder, vb, i32, "");
        const char *ror_name = "llvm.fshr.i32";
        unsigned ror_id = LLVMLookupIntrinsicID(ror_name, strlen(ror_name));
        LLVMTypeRef overload = i32;
        LLVMTypeRef ror_fn_ty =
            LLVMIntrinsicGetType(cg->context, ror_id, &overload, 1);
        LLVMValueRef ror_fn =
            LLVMGetIntrinsicDeclaration(cg->module, ror_id, &overload, 1);
        LLVMValueRef argv[3] = {va32, va32, vb32};
        LLVMValueRef rotated =
            (ror_id != 0 && ror_fn_ty && ror_fn)
                ? LLVMBuildCall2(cg->builder, ror_fn_ty, ror_fn, argv, 3, "")
                : NULL;
        if (rotated) {
          vals[in->dst] = LLVMBuildZExt(cg->builder, rotated, cg->type_i64, "");
        } else {
          LLVMValueRef sh1 = LLVMBuildLShr(cg->builder, va, vb, "");
          LLVMValueRef diff = LLVMBuildSub(
              cg->builder, LLVMConstInt(cg->type_i64, 32, false), vb, "");
          LLVMValueRef sh2 = LLVMBuildShl(cg->builder, va, diff, "");
          vals[in->dst] = LLVMBuildAnd(
              cg->builder, LLVMBuildOr(cg->builder, sh1, sh2, ""), mask, "");
        }
      }
      break;

    case NYIR_SELECT_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMValueRef vc =
            (in->c >= 0 && (size_t)in->c < val_count) ? vals[in->c] : NULL;
        LLVMValueRef cond = LLVMBuildICmp(
            cg->builder, LLVMIntNE, ny_as_i64(cg, va), ny_llvm_c0(cg), "");
        vals[in->dst] = LLVMBuildSelect(cg->builder, cond, ny_as_i64(cg, vb),
                                        ny_as_i64(cg, vc), "");
      }
      break;

    case NYIR_BSWAP_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count)
                              ? ny_as_i64(cg, vals[in->a])
                              : ny_llvm_c0(cg);
        const char *bswap_name = "llvm.bswap.i64";
        unsigned id = LLVMLookupIntrinsicID(bswap_name, strlen(bswap_name));
        LLVMTypeRef overload = cg->type_i64;
        LLVMTypeRef bswap_fn_ty =
            LLVMIntrinsicGetType(cg->context, id, &overload, 1);
        LLVMValueRef bswap_fn =
            LLVMGetIntrinsicDeclaration(cg->module, id, &overload, 1);
        LLVMValueRef argv[1] = {va};
        vals[in->dst] = (id != 0 && bswap_fn_ty && bswap_fn)
                            ? LLVMBuildCall2(cg->builder, bswap_fn_ty, bswap_fn,
                                             argv, 1, "")
                            : va;
      }
      break;

    case NYIR_BSWAP32_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count)
                              ? ny_as_i64(cg, vals[in->a])
                              : ny_llvm_c0(cg);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(cg->context);
        LLVMValueRef va32 = LLVMBuildTrunc(cg->builder, va, i32, "");
        const char *bswap32_name = "llvm.bswap.i32";
        unsigned id = LLVMLookupIntrinsicID(bswap32_name, strlen(bswap32_name));
        LLVMTypeRef overload = i32;
        LLVMTypeRef bswap32_fn_ty =
            LLVMIntrinsicGetType(cg->context, id, &overload, 1);
        LLVMValueRef bswap32_fn =
            LLVMGetIntrinsicDeclaration(cg->module, id, &overload, 1);
        LLVMValueRef argv[1] = {va32};
        LLVMValueRef swapped = (id != 0 && bswap32_fn_ty && bswap32_fn)
                                   ? LLVMBuildCall2(cg->builder, bswap32_fn_ty,
                                                    bswap32_fn, argv, 1, "")
                                   : va32;
        vals[in->dst] = LLVMBuildZExt(cg->builder, swapped, cg->type_i64, "");
      }
      break;

    case NYIR_CMP_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMIntPredicate pred = LLVMIntEQ;
        switch (in->cmp) {
        case NYIR_CMP_EQ:
          pred = LLVMIntEQ;
          break;
        case NYIR_CMP_NE:
          pred = LLVMIntNE;
          break;
        case NYIR_CMP_LT:
          pred = LLVMIntSLT;
          break;
        case NYIR_CMP_LE:
          pred = LLVMIntSLE;
          break;
        case NYIR_CMP_GT:
          pred = LLVMIntSGT;
          break;
        case NYIR_CMP_GE:
          pred = LLVMIntSGE;
          break;
        default:
          pred = LLVMIntEQ;
          break;
        }
        LLVMValueRef cmp = LLVMBuildICmp(cg->builder, pred, ny_as_i64(cg, va),
                                         ny_as_i64(cg, vb), "");
        vals[in->dst] = LLVMBuildZExt(cg->builder, cmp, cg->type_i64, "");
      }
      break;

    case NYIR_CMP_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMRealPredicate pred = LLVMRealOEQ;
        switch (in->cmp) {
        case NYIR_CMP_EQ:
          pred = LLVMRealOEQ;
          break;
        case NYIR_CMP_NE:
          pred = LLVMRealONE;
          break;
        case NYIR_CMP_LT:
          pred = LLVMRealOLT;
          break;
        case NYIR_CMP_LE:
          pred = LLVMRealOLE;
          break;
        case NYIR_CMP_GT:
          pred = LLVMRealOGT;
          break;
        case NYIR_CMP_GE:
          pred = LLVMRealOGE;
          break;
        default:
          pred = LLVMRealOEQ;
          break;
        }
        LLVMValueRef cmp = LLVMBuildFCmp(cg->builder, pred, ny_as_f64(cg, va),
                                         ny_as_f64(cg, vb), "");
        vals[in->dst] = LLVMBuildZExt(cg->builder, cmp, cg->type_i64, "");
      }
      break;

    case NYIR_SQRT_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] = ny_llvm_emit_f64_unary_runtime(cg, "rt_sqrt_f64", va);
      }
      break;

    case NYIR_SIN_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] = ny_llvm_emit_f64_unary_runtime(cg, "rt_sin_f64", va);
      }
      break;

    case NYIR_COS_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] = ny_llvm_emit_f64_unary_runtime(cg, "rt_cos_f64", va);
      }
      break;

    case NYIR_ADD_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFAdd(cg->builder, ny_as_f64(cg, va),
                                      ny_as_f64(cg, vb), "");
      }
      break;

    case NYIR_SUB_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFSub(cg->builder, ny_as_f64(cg, va),
                                      ny_as_f64(cg, vb), "");
      }
      break;

    case NYIR_MUL_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFMul(cg->builder, ny_as_f64(cg, va),
                                      ny_as_f64(cg, vb), "");
      }
      break;

    case NYIR_DIV_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFDiv(cg->builder, ny_as_f64(cg, va),
                                      ny_as_f64(cg, vb), "");
      }
      break;

    case NYIR_I64_TO_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        /*
         * The machine lower turns a conversion whose source already carries
         * the destination's f64 type into a move (mach_same_value_type) —
         * the whole-float literal lowering emits CONST_F64 ahead of an
         * explicit conversion.  SIToFP of the bit pattern here would
         * double-convert: 1.0 became 4.60718e+18 in every float constant
         * the JIT touched.  Mirror the machine semantics exactly.
         */
        if (va && LLVMTypeOf(va) == cg->type_f64) {
          vals[in->dst] = va;
        } else {
          vals[in->dst] =
              LLVMBuildSIToFP(cg->builder, ny_as_i64(cg, va), cg->type_f64, "");
        }
      }
      break;

    case NYIR_F32_TO_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] =
            LLVMBuildFPToSI(cg->builder, ny_as_f32(cg, va), cg->type_i64, "");
      }
      break;

    case NYIR_F64_TO_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] =
            LLVMBuildFPToSI(cg->builder, ny_as_f64(cg, va), cg->type_i64, "");
      }
      break;

    case NYIR_ADD_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFAdd(cg->builder, ny_as_f32(cg, va),
                                      ny_as_f32(cg, vb), "");
      }
      break;

    case NYIR_SUB_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFSub(cg->builder, ny_as_f32(cg, va),
                                      ny_as_f32(cg, vb), "");
      }
      break;

    case NYIR_MUL_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFMul(cg->builder, ny_as_f32(cg, va),
                                      ny_as_f32(cg, vb), "");
      }
      break;

    case NYIR_DIV_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFDiv(cg->builder, ny_as_f32(cg, va),
                                      ny_as_f32(cg, vb), "");
      }
      break;

    case NYIR_CMP_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb =
            (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMRealPredicate pred = LLVMRealOEQ;
        switch (in->cmp) {
        case NYIR_CMP_EQ:
          pred = LLVMRealOEQ;
          break;
        case NYIR_CMP_NE:
          pred = LLVMRealONE;
          break;
        case NYIR_CMP_LT:
          pred = LLVMRealOLT;
          break;
        case NYIR_CMP_LE:
          pred = LLVMRealOLE;
          break;
        case NYIR_CMP_GT:
          pred = LLVMRealOGT;
          break;
        case NYIR_CMP_GE:
          pred = LLVMRealOGE;
          break;
        default:
          pred = LLVMRealOEQ;
          break;
        }
        LLVMValueRef cmp = LLVMBuildFCmp(cg->builder, pred, ny_as_f32(cg, va),
                                         ny_as_f32(cg, vb), "");
        vals[in->dst] = LLVMBuildZExt(cg->builder, cmp, cg->type_i64, "");
      }
      break;

    case NYIR_I64_TO_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        /*
         * Mirror the machine lower's type-driven conversion (see
         * NYIR_I64_TO_F64): an f32-typed source is already the destination
         * type, and an f64-typed source truncates rather than re-converting
         * its bit pattern as an integer.
         */
        if (va && LLVMTypeOf(va) == cg->type_f32) {
          vals[in->dst] = va;
        } else if (va && LLVMTypeOf(va) == cg->type_f64) {
          vals[in->dst] = LLVMBuildFPTrunc(cg->builder, va, cg->type_f32, "");
        } else {
          vals[in->dst] =
              LLVMBuildSIToFP(cg->builder, ny_as_i64(cg, va), cg->type_f32, "");
        }
      }
      break;

    case NYIR_F64_TO_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] =
            LLVMBuildFPTrunc(cg->builder, ny_as_f64(cg, va), cg->type_f32, "");
      }
      break;

    case NYIR_F32_TO_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] =
            LLVMBuildFPExt(cg->builder, ny_as_f32(cg, va), cg->type_f64, "");
      }
      break;

    case NYIR_LOAD_LOCAL:
      if (in->dst >= 0 && (size_t)in->dst < val_count && in->imm >= 0 &&
          (size_t)in->imm < local_count) {
        vals[in->dst] = LLVMBuildLoad2(cg->builder, cg->type_i64,
                                       local_allocas[in->imm], "");
      }
      break;

    case NYIR_STORE_LOCAL:
      if (in->imm >= 0 && (size_t)in->imm < local_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMBuildStore(cg->builder, ny_as_i64(cg, va), local_allocas[in->imm]);
      }
      break;

    case NYIR_ADDR_LOCAL:
      if (in->dst >= 0 && (size_t)in->dst < val_count && in->imm >= 0 &&
          (size_t)in->imm < local_count) {
        vals[in->dst] = LLVMBuildPtrToInt(cg->builder, local_allocas[in->imm],
                                          cg->type_i64, "");
      }
      break;

    case NYIR_ALLOCA:
      /*
       * Already materialized in the entry block above.
       */
      break;

    case NYIR_LOAD_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef base = ny_as_i64(cg, va);
        if (in->imm != 0) {
          LLVMValueRef off =
              LLVMConstInt(cg->type_i64, (uint64_t)in->imm, false);
          base = LLVMBuildAdd(cg->builder, base, off, "");
        }
        if (in->flags & NYIR_INST_F_MEM_BYTE) {
          LLVMTypeRef i8_type = LLVMInt8TypeInContext(cg->context);
          LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, base,
                                               LLVMPointerType(i8_type, 0), "");
          LLVMValueRef loaded_i8 =
              LLVMBuildLoad2(cg->builder, i8_type, ptr, "");
          vals[in->dst] =
              LLVMBuildZExt(cg->builder, loaded_i8, cg->type_i64, "");
        } else if (in->scalar_class == NYIR_SCALAR_F64) {
          LLVMTypeRef f64_type = LLVMDoubleTypeInContext(cg->context);
          LLVMValueRef ptr = LLVMBuildIntToPtr(
              cg->builder, base, LLVMPointerType(f64_type, 0), "");
          vals[in->dst] = LLVMBuildLoad2(cg->builder, f64_type, ptr, "");
        } else {
          LLVMValueRef ptr = LLVMBuildIntToPtr(
              cg->builder, base, LLVMPointerType(cg->type_i64, 0), "");
          vals[in->dst] = LLVMBuildLoad2(cg->builder, cg->type_i64, ptr, "");
        }
      }
      break;

    case NYIR_STORE_I64: {
      LLVMValueRef va =
          (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
      LLVMValueRef vb =
          (in->c >= 0 && (size_t)in->c < val_count) ? vals[in->c] : NULL;
      LLVMValueRef base = ny_as_i64(cg, va);
      if (in->imm != 0) {
        LLVMValueRef off = LLVMConstInt(cg->type_i64, (uint64_t)in->imm, false);
        base = LLVMBuildAdd(cg->builder, base, off, "");
      }
      if (in->flags & NYIR_INST_F_MEM_BYTE) {
        LLVMTypeRef i8_type = LLVMInt8TypeInContext(cg->context);
        LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, base,
                                             LLVMPointerType(i8_type, 0), "");
        LLVMValueRef val_i8 =
            LLVMBuildTrunc(cg->builder, ny_as_i64(cg, vb), i8_type, "");
        LLVMBuildStore(cg->builder, val_i8, ptr);
      } else if (in->scalar_class == NYIR_SCALAR_F64) {
        LLVMTypeRef f64_type = LLVMDoubleTypeInContext(cg->context);
        LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, base,
                                             LLVMPointerType(f64_type, 0), "");
        LLVMBuildStore(cg->builder, ny_as_f64(cg, vb), ptr);
      } else {
        LLVMValueRef ptr = LLVMBuildIntToPtr(
            cg->builder, base, LLVMPointerType(cg->type_i64, 0), "");
        LLVMBuildStore(cg->builder, ny_as_i64(cg, vb), ptr);
      }
      break;
    }

    case NYIR_COPY_STRUCT: {
      /*
       * Aggregate ABI lowering uses raw byte pointers: a is the destination,
       * b is the source, and imm is the exact byte count.
       */
      if (in->imm < 0)
        break;
      LLVMTypeRef byte_ptr =
          LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
      LLVMValueRef dst =
          (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
      LLVMValueRef src =
          (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
      dst = LLVMBuildIntToPtr(cg->builder, ny_as_i64(cg, dst), byte_ptr, "");
      src = LLVMBuildIntToPtr(cg->builder, ny_as_i64(cg, src), byte_ptr, "");
      LLVMBuildMemCpy(cg->builder, dst, 1, src, 1,
                      LLVMConstInt(cg->type_i64, (uint64_t)in->imm, false));
      break;
    }

    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC4_STORE_F64:
    case NYIR_VEC4_ADD_F64:
    case NYIR_VEC4_SUB_F64:
    case NYIR_VEC4_MUL_F64:
    case NYIR_VEC4_DIV_F64:
    case NYIR_VEC4_FMA_F64:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC4_REDUCE_ADD_F64:
    case NYIR_VEC8_LOAD_F32:
    case NYIR_VEC8_STORE_F32:
    case NYIR_VEC8_ADD_F32:
    case NYIR_VEC8_SUB_F32:
    case NYIR_VEC8_MUL_F32:
    case NYIR_VEC8_DIV_F32:
    case NYIR_VEC8_FMA_F32:
    case NYIR_VEC8_SET1_F32:
    case NYIR_VEC8_SHUFFLE_F32:
    case NYIR_VEC4_LOAD_I64:
    case NYIR_VEC4_STORE_I64:
    case NYIR_VEC4_ADD_I64:
    case NYIR_VEC4_SUB_I64:
    case NYIR_VEC4_SET1_I64:
    case NYIR_VEC4_AND_I64:
    case NYIR_VEC4_OR_I64:
    case NYIR_VEC4_XOR_I64:
    case NYIR_VEC4_SHL_I64:
    case NYIR_VEC4_SAR_I64:
    case NYIR_VEC4_REDUCE_ADD_I64:
    case NYIR_VEC8_LOAD_I64:
    case NYIR_VEC8_STORE_I64:
    case NYIR_VEC8_ADD_I64:
    case NYIR_VEC8_SUB_I64:
    case NYIR_VEC8_AND_I64:
    case NYIR_VEC8_OR_I64:
    case NYIR_VEC8_XOR_I64:
    case NYIR_VEC8_REDUCE_ADD_I64: {
      LLVMTypeRef vector_type = ny_llvm_vector_type(cg, in->op);
      LLVMTypeRef element_type = LLVMGetElementType(vector_type);
      bool is_float =
          element_type == cg->type_f64 || element_type == cg->type_f32;
      unsigned lanes = LLVMGetVectorSize(vector_type);
      if (in->op == NYIR_VEC4_LOAD_F64 || in->op == NYIR_VEC8_LOAD_F32 ||
          in->op == NYIR_VEC4_LOAD_I64 || in->op == NYIR_VEC8_LOAD_I64) {
        LLVMValueRef address =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef ptr =
            LLVMBuildIntToPtr(cg->builder, ny_as_i64(cg, address),
                              LLVMPointerType(vector_type, 0), "");
        if (in->dst >= 0 && (size_t)in->dst < val_count) {
          LLVMValueRef load = LLVMBuildLoad2(cg->builder, vector_type, ptr, "");
          /*
           * NYIR vectorization proves lane layout, not base-pointer alignment.
           * Keep the memory access valid for typed buffers and other raw
           * pointers whose runtime alignment can be smaller than the vector
           * ABI alignment.
           */
          LLVMSetAlignment(load, 1);
          vals[in->dst] = load;
        }
        break;
      }
      if (in->op == NYIR_VEC4_STORE_F64 || in->op == NYIR_VEC8_STORE_F32 ||
          in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC8_STORE_I64) {
        LLVMValueRef address =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        int vector_value = in->b >= 0 ? in->b : in->c;
        LLVMValueRef vector =
            ny_llvm_vector_slot(cg, vals, val_count, vector_value, vector_type);
        LLVMValueRef ptr =
            LLVMBuildIntToPtr(cg->builder, ny_as_i64(cg, address),
                              LLVMPointerType(vector_type, 0), "");
        LLVMValueRef store = LLVMBuildStore(cg->builder, vector, ptr);
        LLVMSetAlignment(store, 1);
        break;
      }
      if (in->op == NYIR_VEC4_SET1_F64 || in->op == NYIR_VEC8_SET1_F32 ||
          in->op == NYIR_VEC4_SET1_I64) {
        LLVMValueRef scalar =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        if (is_float)
          scalar = element_type == cg->type_f64 ? ny_as_f64(cg, scalar)
                                                : ny_as_f32(cg, scalar);
        else
          scalar = ny_as_i64(cg, scalar);
        if (in->dst >= 0 && (size_t)in->dst < val_count)
          vals[in->dst] = ny_llvm_vector_splat(cg, scalar, vector_type);
        break;
      }
      if (in->op == NYIR_VEC4_REDUCE_ADD_F64 ||
          in->op == NYIR_VEC4_REDUCE_ADD_I64 ||
          in->op == NYIR_VEC8_REDUCE_ADD_I64) {
        LLVMValueRef scalar =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vector =
            ny_llvm_vector_slot(cg, vals, val_count, in->b, vector_type);
        LLVMValueRef result =
            is_float ? (element_type == cg->type_f64 ? ny_as_f64(cg, scalar)
                                                     : ny_as_f32(cg, scalar))
                     : ny_as_i64(cg, scalar);
        LLVMTypeRef index_type = LLVMInt32TypeInContext(cg->context);
        for (unsigned lane = 0; lane < lanes; ++lane) {
          LLVMValueRef index = LLVMConstInt(index_type, lane, false);
          LLVMValueRef item =
              LLVMBuildExtractElement(cg->builder, vector, index, "");
          result = is_float ? LLVMBuildFAdd(cg->builder, result, item, "")
                            : LLVMBuildAdd(cg->builder, result, item, "");
        }
        if (in->dst >= 0 && (size_t)in->dst < val_count)
          vals[in->dst] = result;
        break;
      }
      LLVMValueRef lhs =
          ny_llvm_vector_slot(cg, vals, val_count, in->a, vector_type);
      LLVMValueRef rhs =
          ny_llvm_vector_slot(cg, vals, val_count, in->b, vector_type);
      LLVMValueRef result = NULL;
      switch (in->op) {
      case NYIR_VEC4_ADD_F64:
      case NYIR_VEC8_ADD_F32:
        result = LLVMBuildFAdd(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_SUB_F64:
      case NYIR_VEC8_SUB_F32:
        result = LLVMBuildFSub(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_MUL_F64:
      case NYIR_VEC8_MUL_F32:
        result = LLVMBuildFMul(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_DIV_F64:
      case NYIR_VEC8_DIV_F32:
        result = LLVMBuildFDiv(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_FMA_F64:
      case NYIR_VEC8_FMA_F32: {
        LLVMValueRef addend =
            ny_llvm_vector_slot(cg, vals, val_count, in->c, vector_type);
        result = ny_llvm_vector_fma(cg, lhs, rhs, addend, vector_type);
        break;
      }
      case NYIR_VEC4_ADD_I64:
      case NYIR_VEC8_ADD_I64:
        result = LLVMBuildAdd(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_SUB_I64:
      case NYIR_VEC8_SUB_I64:
        result = LLVMBuildSub(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_AND_I64:
      case NYIR_VEC8_AND_I64:
        result = LLVMBuildAnd(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_OR_I64:
      case NYIR_VEC8_OR_I64:
        result = LLVMBuildOr(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_XOR_I64:
      case NYIR_VEC8_XOR_I64:
        result = LLVMBuildXor(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_SHL_I64:
        result = LLVMBuildShl(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_SAR_I64:
        result = LLVMBuildAShr(cg->builder, lhs, rhs, "");
        break;
      case NYIR_VEC4_SHUFFLE_F64:
      case NYIR_VEC8_SHUFFLE_F32:
        result = ny_llvm_vector_shuffle(cg, lhs, vector_type, in->op, in->imm);
        break;
      default:
        break;
      }
      if (result && in->dst >= 0 && (size_t)in->dst < val_count)
        vals[in->dst] = result;
      break;
    }

    case NYIR_BOUNDS_CHECK: {
      /*
       * Keep the LLVM path's bounds contract identical to the native
       * encoders: `b` is the byte offset and `c` is an optional dynamic
       * exclusive bound; when `c` is absent, `imm` is the fixed bound.
       * Emit an inline unsigned comparison (ULT) branching to a cold trap block
       * so loop bodies stay free of opaque external calls and can vectorize.
       */
      LLVMValueRef offset =
          (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
      LLVMValueRef limit =
          (in->c >= 0 && (size_t)in->c < val_count) ? vals[in->c] : NULL;
      if (!limit)
        limit = LLVMConstInt(cg->type_i64, (uint64_t)in->imm, false);
      if (!offset || !limit)
        break;

      LLVMValueRef off_val = ny_as_i64(cg, offset);
      LLVMValueRef lim_val = ny_as_i64(cg, limit);
      LLVMValueRef ok =
          LLVMBuildICmp(cg->builder, LLVMIntULT, off_val, lim_val, "bcheck_ok");

      LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
      LLVMValueRef cur_fn = LLVMGetBasicBlockParent(cur_bb);
      LLVMBasicBlockRef fail_bb =
          LLVMAppendBasicBlockInContext(cg->context, cur_fn, "bcheck_fail");
      LLVMBasicBlockRef cont_bb =
          LLVMAppendBasicBlockInContext(cg->context, cur_fn, "bcheck_cont");

      LLVMBuildCondBr(cg->builder, ok, cont_bb, fail_bb);

      LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
      LLVMTypeRef params[] = {cg->type_i64, cg->type_i64};
      LLVMTypeRef check_ty = LLVMFunctionType(
          LLVMVoidTypeInContext(cg->context), params, 2, false);
      LLVMValueRef check = LLVMGetNamedFunction(cg->module, "rt_bounds_check");
      if (!check) {
        check = LLVMAddFunction(cg->module, "rt_bounds_check", check_ty);
        LLVMSetLinkage(check, LLVMExternalLinkage);
      }
      LLVMValueRef args[] = {off_val, lim_val};
      LLVMBuildCall2(cg->builder, check_ty, check, args, 2, "");
      LLVMBuildUnreachable(cg->builder);

      LLVMPositionBuilderAtEnd(cg->builder, cont_bb);
      break;
    }

    case NYIR_ADDR_SYMBOL:
      if (in->dst >= 0 && (size_t)in->dst < val_count && in->symbol) {
        const char *addr_name = in->symbol;
        LLVMValueRef sym = LLVMGetNamedGlobal(cg->module, addr_name);
        if (!sym)
          sym = LLVMGetNamedFunction(cg->module, addr_name);
        /*
         * Function-valued identifiers can be encountered while emitting a
         * caller before the collector reaches the callee body.  Never create
         * the fallback data global for those names: it permanently prevents
         * LLVM from installing the real function with the same symbol.
         */
        if (!sym && strncmp(addr_name, "ny_fn_", 6) == 0) {
          LLVMTypeRef provisional =
              LLVMFunctionType(cg->type_i64, NULL, 0, false);
          sym = LLVMAddFunction(cg->module, addr_name, provisional);
          LLVMSetLinkage(sym, LLVMExternalLinkage);
        }
        if (!sym) {
          sym = cg->global_symbol ? cg->global_symbol(cg->userdata, in->symbol)
                                  : NULL;
          if (!sym) {
            sym = LLVMAddGlobal(cg->module, cg->type_i64, in->symbol);
            LLVMSetInitializer(sym, LLVMConstInt(cg->type_i64, 0, false));
            LLVMSetLinkage(sym, LLVMPrivateLinkage);
            LLVMSetAlignment(sym, 8);
          }
        }
        if (sym && LLVMIsAGlobalVariable(sym)) {
          const char *origin =
              ny_llvm_nyir_origin_section(cg, in->symbol, true);
          if (origin)
            LLVMSetSection(sym, origin);
        }
        vals[in->dst] = LLVMBuildPtrToInt(cg->builder, sym, cg->type_i64, "");
      }
      break;

    case NYIR_CALL: {
      LLVMTypeRef aggregate_ret_ty = ny_llvm_aggregate_return_type(
          cg, in->ret_aggregate_size, in->ret_aggregate_classes);
      if (in->ret_aggregate_size && !aggregate_ret_ty) {
        if (err && err_len > 0)
          snprintf(err, err_len,
                   "LLVM NYIR emission: unsupported aggregate return layout "
                   "for %s",
                   in->symbol ? in->symbol : "<unknown>");
        unsupported = true;
        break;
      }
      int call_args[NYIR_CALL_MAX_ARGS];
      int call_argc = 0;
      char c_err[128] = {0};
      (void)nyr_call_args(in, (int)val_count, call_args, NYIR_CALL_MAX_ARGS,
                           &call_argc, c_err, sizeof(c_err));
      const char *callee_name = in->symbol ? in->symbol : "unknown_fn";
      /*
       * Lowering selects runtime helpers while declaration identity is
       * available. The emitter must retain source call symbols verbatim.
       */
      char user_callee_name[512];
      if (!(in->flags & NYIR_INST_F_EXTERN) &&
          strcmp(callee_name, "_ny_top_entry") != 0) {
        int n = snprintf(user_callee_name, sizeof(user_callee_name), "ny_fn_%s",
                         callee_name);
        if (n < 0 || (size_t)n >= sizeof(user_callee_name)) {
          if (err && err_len > 0)
            snprintf(err, err_len,
                     "LLVM NYIR emission: call symbol too long: %s",
                     callee_name);
          unsupported = true;
          break;
        }
        callee_name = user_callee_name;
      }
      LLVMValueRef callee = LLVMGetNamedFunction(cg->module, callee_name);
      if (!callee && strncmp(callee_name, "ny_fn_core.", 15) == 0) {
        char stripped[512];
        snprintf(stripped, sizeof(stripped), "ny_fn_%s", callee_name + 15);
        callee = LLVMGetNamedFunction(cg->module, stripped);
      }
      LLVMTypeRef *arg_tys =
          malloc(sizeof(LLVMTypeRef) * (NYIR_CALL_MAX_ARGS * 2u + 1u));
      for (int k = 0; k < call_argc; ++k) {
        int v = call_args[k];
        LLVMValueRef value = (v >= 0 && (size_t)v < val_count) ? vals[v] : NULL;
        LLVMTypeRef ty = value ? LLVMTypeOf(value) : NULL;
        arg_tys[k] = (ty == cg->type_f64)   ? cg->type_f64
                     : (ty == cg->type_f32) ? cg->type_f32
                                            : cg->type_i64;
        nyr_scalar_class_t arg_class = nyr_call_arg_class_get(in, (size_t)k);
        if (arg_class == NYIR_SCALAR_F64)
          arg_tys[k] = cg->type_f64;
        else if (arg_class == NYIR_SCALAR_F32)
          arg_tys[k] = cg->type_f32;
        /*
         * NYIR keeps an aggregate's address in its value word.  A SysV
         * MEMORY-class aggregate is passed to C as a pointer with LLVM's
         * byval contract, not as the integer bits of that address.
         */
        uint32_t packed_agg = in->arg_sizes ? in->arg_sizes[k] : 0;
        uint32_t agg_size = NYIR_ARG_AGG_SIZE(packed_agg);
        uint32_t agg_class = NYIR_ARG_AGG_CLASS(packed_agg, 0);
        if (agg_size > 0 &&
            (agg_size > 16 || agg_class == NYIR_ARG_CLASS_MEMORY))
          arg_tys[k] = LLVMPointerType(cg->type_i8, 0);
      }
      /*
       * Runtime helpers consume the canonical Ny word ABI even when the
       * producer expression currently happens to be represented as f64 in
       * LLVM.  Inferring their declaration from the producer created mixed
       * declarations such as rt_any_to_i64(double) and later i64 call sites;
       * clang correctly rejects those calls for Wasm.  Keep the explicitly
       * floating math helpers on their declared f64 path and normalize the
       * dynamic/value helpers here before creating or repairing declarations.
       */
      const ny_native_runtime_abi_decl_t *runtime_abi =
          ny_native_runtime_abi_decl(callee_name);
      /*
       * Runtime call typing belongs to the declaration, not the `rt_`
       * spelling convention. Manifest coverage is checked by
       * check_native_abi.py; only declared runtime entries receive this
       * canonical i64/f64 parameter normalization.
       */
      bool runtime_symbol = runtime_abi != NULL;
      if (runtime_symbol)
        for (int k = 0; k < call_argc; ++k) {
          nyr_scalar_class_t arg_class =
              nyr_call_arg_class_get(in, (size_t)k);
          bool argument_flagged_f64 = arg_class == NYIR_SCALAR_F64;
          bool argument_flagged_f32 = arg_class == NYIR_SCALAR_F32;
          bool argument_declared_f64 =
              ny_native_runtime_abi_arg_is_f64(runtime_abi, (size_t)k);
          arg_tys[k] = argument_flagged_f64 || argument_declared_f64
                           ? cg->type_f64
                       : argument_flagged_f32 ? cg->type_f32
                                              : cg->type_i64;
        }
      int arg_starts[NYIR_CALL_MAX_ARGS] = {0};
      int arg_piece_counts[NYIR_CALL_MAX_ARGS] = {0};
      LLVMTypeRef source_arg_tys[NYIR_CALL_MAX_ARGS] = {0};
      int llvm_argc = 0;
      for (int k = 0; k < call_argc; ++k) {
        source_arg_tys[k] = arg_tys[k];
        uint32_t packed_agg = in->arg_sizes ? in->arg_sizes[k] : 0;
        uint32_t agg_size = NYIR_ARG_AGG_SIZE(packed_agg);
        uint32_t cls0 = NYIR_ARG_AGG_CLASS(packed_agg, 0);
        uint32_t cls1 = NYIR_ARG_AGG_CLASS(packed_agg, 1);
        arg_starts[k] = llvm_argc;
        if (agg_size > 0 && agg_size <= 16 &&
            (cls0 == NYIR_ARG_CLASS_INTEGER || cls0 == NYIR_ARG_CLASS_SSE)) {
          unsigned pieces = (agg_size + 7u) / 8u;
          for (unsigned piece = 0; piece < pieces; ++piece) {
            uint32_t cls = piece == 0 ? cls0 : cls1;
            uint32_t piece_bytes = agg_size - piece * 8u;
            if (piece_bytes > 8u)
              piece_bytes = 8u;
            LLVMTypeRef piece_ty = NULL;
            if (cls == NYIR_ARG_CLASS_INTEGER) {
              unsigned bits = 8;
              while (bits < piece_bytes * 8u)
                bits <<= 1u;
              piece_ty = LLVMIntTypeInContext(cg->context, bits);
            } else if (cls == NYIR_ARG_CLASS_SSE) {
              piece_ty = piece_bytes >= 8u   ? cg->type_f64
                         : piece_bytes >= 4u ? cg->type_f32
                                             : NULL;
            }
            if (!piece_ty || llvm_argc >= NYIR_CALL_MAX_ARGS * 2) {
              if (err && err_len > 0)
                snprintf(err, err_len,
                         "LLVM NYIR emission: unsupported aggregate argument "
                         "class for %s argument %d",
                         callee_name, k + 1);
              free(arg_tys);
              unsupported = true;
              break;
            }
            arg_tys[llvm_argc++] = piece_ty;
            arg_piece_counts[k]++;
          }
          if (unsupported)
            break;
        } else {
          if (agg_size > 0 && agg_size <= 16 && cls0 != NYIR_ARG_CLASS_NONE &&
              cls0 != NYIR_ARG_CLASS_MEMORY) {
            if (err && err_len > 0)
              snprintf(err, err_len,
                       "LLVM NYIR emission: unsupported aggregate argument "
                       "class for %s argument %d",
                       callee_name, k + 1);
            free(arg_tys);
            unsupported = true;
            break;
          }
          arg_tys[llvm_argc++] = source_arg_tys[k];
          arg_piece_counts[k] = 1;
        }
      }
      if (unsupported)
        break;
      if (!callee) {
        LLVMTypeRef decl_ret_ty =
            aggregate_ret_ty                      ? aggregate_ret_ty
            : in->scalar_class == NYIR_SCALAR_F64 ? cg->type_f64
            : in->scalar_class == NYIR_SCALAR_F32 ? cg->type_f32
                                                  : cg->type_i64;
        for (int k = 0; k < call_argc; ++k) {
          nyr_scalar_class_t arg_class =
              nyr_call_arg_class_get(in, (size_t)k);
          if (arg_class == NYIR_SCALAR_F64)
            arg_tys[arg_starts[k]] = cg->type_f64;
          else if (arg_class == NYIR_SCALAR_F32)
            arg_tys[arg_starts[k]] = cg->type_f32;
        }
        LLVMTypeRef callee_ty =
            LLVMFunctionType(decl_ret_ty, arg_tys, (unsigned)llvm_argc, false);
        callee = LLVMAddFunction(cg->module, callee_name, callee_ty);
        LLVMSetLinkage(callee, LLVMExternalLinkage);
      }
      LLVMTypeRef callee_fn_ty = LLVMGlobalGetValueType(callee);
      unsigned callee_argc = LLVMCountParamTypes(callee_fn_ty);
      LLVMTypeRef *callee_arg_tys =
          callee_argc ? malloc(sizeof(LLVMTypeRef) * callee_argc) : NULL;
      if (callee_argc)
        LLVMGetParamTypes(callee_fn_ty, callee_arg_tys);
      LLVMTypeRef expected_ret_ty =
          aggregate_ret_ty                      ? aggregate_ret_ty
          : in->scalar_class == NYIR_SCALAR_F64 ? cg->type_f64
          : in->scalar_class == NYIR_SCALAR_F32 ? cg->type_f32
                                                : cg->type_i64;
      /*
       * Runtime aliases registered by the legacy AST backend can already
       * exist in the module with the expanded `any` ABI.  NYIR calls carry
       * their canonical raw argument list, so repair a declaration whenever
       * either its return type or arity differs.
       *
       * Exception: an i64 NYIR value feeding a declared f64/f32 parameter is
       * the raw-bits handoff (f64 expressions travel as IEEE bits in i64
       * registers).  The call site bitcasts via ny_as_f64 below, so the
       * shared declaration must keep its float signature.  Retyping it to
       * i64 made the bits flow in a GP register while the C function read
       * XMM0 (c.sqrt(variable) returned garbage while c.sqrt(25.0) was
       * exact).
       */
      bool arg_types_differ = callee_argc != (unsigned)llvm_argc;
      if (!arg_types_differ) {
        for (int k = 0; k < llvm_argc; ++k) {
          if (callee_arg_tys[k] != arg_tys[k]) {
            bool bits_to_float = (callee_arg_tys[k] == cg->type_f64 ||
                                  callee_arg_tys[k] == cg->type_f32) &&
                                 arg_tys[k] == cg->type_i64;
            if (!bits_to_float) {
              arg_types_differ = true;
              break;
            }
          }
        }
      }
      if ((LLVMGetReturnType(callee_fn_ty) != expected_ret_ty ||
           arg_types_differ) &&
          LLVMCountBasicBlocks(callee) == 0) {
        LLVMTypeRef new_fn_ty = LLVMFunctionType(expected_ret_ty, arg_tys,
                                                 (unsigned)llvm_argc, false);
        LLVMValueRef new_fn = LLVMAddFunction(cg->module, "", new_fn_ty);
        LLVMReplaceAllUsesWith(callee, new_fn);
        LLVMDeleteFunction(callee);
        LLVMSetValueName2(new_fn, callee_name, strlen(callee_name));
        callee = new_fn;
        callee_fn_ty = new_fn_ty;
        free(callee_arg_tys);
        callee_argc = (unsigned)llvm_argc;
        callee_arg_tys = arg_tys;
        arg_tys = NULL;
      }
      if (callee_argc != (unsigned)llvm_argc) {
        if (err && err_len > 0)
          snprintf(err, err_len,
                   "LLVM NYIR emission: call from %s to %s has %d arguments, "
                   "but its ABI lowering has %u parameters",
                   name, callee_name, llvm_argc, callee_argc);
        free(callee_arg_tys);
        free(arg_tys);
        unsupported = true;
        break;
      }
      LLVMValueRef *call_vals = malloc(sizeof(LLVMValueRef) * (llvm_argc + 1));
      for (int k = 0; k < llvm_argc; ++k) {
        int source_k = 0;
        while (source_k + 1 < call_argc && arg_starts[source_k + 1] <= k)
          source_k++;
        int v = call_args[source_k];
        LLVMValueRef value = (v >= 0 && (size_t)v < val_count) ? vals[v] : NULL;
        LLVMTypeRef param_ty = callee_arg_tys[(unsigned)k];
        uint32_t packed_agg = in->arg_sizes ? in->arg_sizes[source_k] : 0;
        uint32_t agg_size = NYIR_ARG_AGG_SIZE(packed_agg);
        uint32_t agg_class = NYIR_ARG_AGG_CLASS(packed_agg, 0);
        bool register_aggregate = agg_size > 0 && agg_size <= 16 &&
                                  (agg_class == NYIR_ARG_CLASS_INTEGER ||
                                   agg_class == NYIR_ARG_CLASS_SSE);
        bool byval_aggregate =
            agg_size > 0 &&
            (agg_size > 16 || agg_class == NYIR_ARG_CLASS_MEMORY);
        if (register_aggregate) {
          LLVMValueRef raw = ny_as_i64(cg, value);
          LLVMValueRef base = LLVMBuildIntToPtr(
              cg->builder, raw, LLVMPointerType(cg->type_i8, 0), "");
          unsigned piece = (unsigned)(k - arg_starts[source_k]);
          LLVMValueRef offset = LLVMConstInt(cg->type_i64, piece * 8u, false);
          LLVMValueRef address =
              LLVMBuildGEP2(cg->builder, cg->type_i8, base, &offset, 1, "");
          LLVMValueRef typed = LLVMBuildBitCast(
              cg->builder, address, LLVMPointerType(param_ty, 0), "");
          call_vals[k] = LLVMBuildLoad2(cg->builder, param_ty, typed, "");
          LLVMSetAlignment(call_vals[k], 1);
        } else if (byval_aggregate) {
          LLVMValueRef raw = ny_as_i64(cg, value);
          call_vals[k] = LLVMBuildIntToPtr(cg->builder, raw, param_ty, "");
        } else if (param_ty == cg->type_f64)
          call_vals[k] = ny_as_f64(cg, value);
        else if (param_ty == cg->type_f32)
          call_vals[k] = ny_as_f32(cg, value);
        else if (LLVMGetTypeKind(param_ty) == LLVMIntegerTypeKind) {
          LLVMValueRef integer = ny_as_i64(cg, value);
          unsigned bits = LLVMGetIntTypeWidth(param_ty);
          if (bits < 64)
            call_vals[k] = LLVMBuildTrunc(cg->builder, integer, param_ty, "");
          else if (bits > 64)
            call_vals[k] = LLVMBuildSExt(cg->builder, integer, param_ty, "");
          else
            call_vals[k] = integer;
        } else {
          call_vals[k] = ny_as_i64(cg, value);
        }
      }
      /*
       * setjmp is a returns-twice primitive.  Without this attribute LLVM's
       * optimizer is allowed to treat values/allocas after the first return
       * as if control can never re-enter the call site, which corrupts
       * try/catch join slots after rt_panic longjmps back into NYIR code.
       */
      bool returns_twice = strcmp(callee_name, "_setjmp") == 0 ||
                           strcmp(callee_name, "setjmp") == 0;
      if (returns_twice) {
        unsigned kind = LLVMGetEnumAttributeKindForName("returns_twice", 13);
        if (kind != 0) {
          LLVMAttributeRef attr = LLVMCreateEnumAttribute(cg->context, kind, 0);
          LLVMAddAttributeAtIndex(callee, LLVMAttributeFunctionIndex, attr);
        }
      }
      LLVMValueRef call_res =
          LLVMBuildCall2(cg->builder, callee_fn_ty, callee, call_vals,
                         (unsigned)llvm_argc, "");
      if (call_res && LLVMIsAInstruction(call_res) && in->arg_sizes) {
        unsigned byval_kind = LLVMGetEnumAttributeKindForName("byval", 5);
        unsigned align_kind = LLVMGetEnumAttributeKindForName("align", 5);
        for (int k = 0; k < call_argc; ++k) {
          uint32_t packed_agg = in->arg_sizes[k];
          uint32_t agg_size = NYIR_ARG_AGG_SIZE(packed_agg);
          uint32_t agg_class = NYIR_ARG_AGG_CLASS(packed_agg, 0);
          if (agg_size == 0 ||
              (agg_size <= 16 && agg_class != NYIR_ARG_CLASS_MEMORY))
            continue;
          LLVMTypeRef byval_ty = LLVMArrayType(cg->type_i8, agg_size);
          LLVMAttributeIndex attr_index = (LLVMAttributeIndex)arg_starts[k] + 1;
          unsigned attr_count =
              LLVMGetAttributeCountAtIndex(callee, attr_index);
          LLVMAttributeRef *attrs =
              attr_count ? malloc(sizeof(*attrs) * attr_count) : NULL;
          if (attrs) {
            LLVMGetAttributesAtIndex(callee, attr_index, attrs);
            for (unsigned ai = 0; ai < attr_count; ++ai) {
              if (byval_kind && LLVMIsTypeAttribute(attrs[ai]) &&
                  LLVMGetEnumAttributeKind(attrs[ai]) == byval_kind) {
                byval_ty = LLVMGetTypeAttributeValue(attrs[ai]);
                break;
              }
            }
          }
          if (byval_kind) {
            LLVMAttributeRef attr =
                LLVMCreateTypeAttribute(cg->context, byval_kind, byval_ty);
            LLVMAddAttributeAtIndex(callee, attr_index, attr);
            LLVMAddCallSiteAttribute(call_res, attr_index, attr);
          }
          uint64_t align = 0;
          LLVMAttributeRef declared_align =
              align_kind
                  ? LLVMGetEnumAttributeAtIndex(callee, attr_index, align_kind)
                  : NULL;
          if (declared_align)
            align = LLVMGetEnumAttributeValue(declared_align);
          if (!align && LLVMGetModuleDataLayout(cg->module))
            align = LLVMABIAlignmentOfType(LLVMGetModuleDataLayout(cg->module),
                                           byval_ty);
          if (!align)
            align = 8;
          if (align_kind) {
            LLVMAttributeRef attr =
                LLVMCreateEnumAttribute(cg->context, align_kind, align);
            LLVMAddAttributeAtIndex(callee, attr_index, attr);
            LLVMAddCallSiteAttribute(call_res, attr_index, attr);
          }
          free(attrs);
        }
      }
      if (returns_twice && call_res && LLVMIsAInstruction(call_res)) {
        unsigned kind = LLVMGetEnumAttributeKindForName("returns_twice", 13);
        if (kind != 0) {
          LLVMAttributeRef attr = LLVMCreateEnumAttribute(cg->context, kind, 0);
          LLVMAddCallSiteAttribute(call_res, LLVMAttributeFunctionIndex, attr);
        }
      }
      last_aggregate_return = aggregate_ret_ty ? call_res : NULL;
      last_aggregate_return_size =
          aggregate_ret_ty ? in->ret_aggregate_size : 0;
      last_aggregate_return_classes[0] =
          aggregate_ret_ty ? in->ret_aggregate_classes[0] : 0;
      last_aggregate_return_classes[1] =
          aggregate_ret_ty ? in->ret_aggregate_classes[1] : 0;
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        vals[in->dst] =
            aggregate_ret_ty
                ? LLVMBuildExtractValue(cg->builder, call_res, 0, "")
                : call_res;
      }
      free(call_vals);
      free(callee_arg_tys);
      free(arg_tys);
      break;
    }

    case NYIR_BR:
      if (in->imm >= 0 && (size_t)in->imm < num_blocks && blocks[in->imm]) {
        if (current_nyir_label >= 0 &&
            (size_t)current_nyir_label < num_blocks) {
          block_branch_sources[current_nyir_label] =
              LLVMGetInsertBlock(cg->builder);
          block_branch_targets[current_nyir_label] = in->imm;
        }
        LLVMBuildBr(cg->builder, blocks[in->imm]);
        block_terminated = true;
      }
      break;

    case NYIR_BR_IF:
      if (in->imm >= 0 && (size_t)in->imm < num_blocks && blocks[in->imm]) {
        LLVMValueRef va =
            (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef cond = LLVMBuildICmp(
            cg->builder, LLVMIntNE, ny_as_i64(cg, va), ny_llvm_c0(cg), "");
        LLVMBasicBlockRef true_bb = blocks[in->imm];
        if (current_nyir_label >= 0 &&
            (size_t)current_nyir_label < num_blocks) {
          block_branch_sources[current_nyir_label] =
              LLVMGetInsertBlock(cg->builder);
          block_branch_targets[current_nyir_label] = in->imm;
        }
        char ft_name[64];
        snprintf(ft_name, sizeof(ft_name), "br_fallthrough_%zu", i);
        LLVMBasicBlockRef ft_bb =
            LLVMAppendBasicBlockInContext(cg->context, fn, ft_name);
        LLVMBuildCondBr(cg->builder, cond, true_bb, ft_bb);
        LLVMPositionBuilderAtEnd(cg->builder, ft_bb);
      }
      break;

    case NYIR_RET: {
      LLVMValueRef va =
          (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
      if (in->scalar_class == NYIR_SCALAR_F64)
        LLVMBuildRet(cg->builder, ny_as_f64(cg, va));
      else if (in->scalar_class == NYIR_SCALAR_F32)
        LLVMBuildRet(cg->builder, ny_as_f32(cg, va));
      else
        LLVMBuildRet(cg->builder, ny_as_i64(cg, va));
      block_terminated = true;
      break;
    }

    case NYIR_CAPTURE_RET: {
      nyr_inst_t aggregate_call = {0};
      aggregate_call.ret_aggregate_size = last_aggregate_return_size;
      aggregate_call.ret_aggregate_classes[0] =
          last_aggregate_return_classes[0];
      aggregate_call.ret_aggregate_classes[1] =
          last_aggregate_return_classes[1];
      int piece = ny_llvm_capture_return_piece(&aggregate_call, in->imm);
      if (!last_aggregate_return || piece < 0) {
        if (err && err_len > 0)
          snprintf(err, err_len,
                   "LLVM NYIR emission: capture.ret selector %" PRId64
                   " has no corresponding aggregate return register",
                   in->imm);
        unsupported = true;
        break;
      }
      LLVMValueRef captured = LLVMBuildExtractValue(
          cg->builder, last_aggregate_return, (unsigned)piece, "");
      LLVMTypeRef captured_ty = LLVMTypeOf(captured);
      if (captured_ty == cg->type_f64)
        captured = LLVMBuildBitCast(cg->builder, captured, cg->type_i64, "");
      else if (captured_ty == cg->type_f32) {
        LLVMTypeRef i32 = LLVMIntTypeInContext(cg->context, 32);
        captured = LLVMBuildZExt(
            cg->builder, LLVMBuildBitCast(cg->builder, captured, i32, ""),
            cg->type_i64, "");
      } else
        captured = ny_as_i64(cg, captured);
      if (in->dst >= 0 && (size_t)in->dst < val_count)
        vals[in->dst] = captured;
      break;
    }

    default:
      if (err && err_len > 0) {
        if (in->debug.line)
          snprintf(err, err_len,
                   "LLVM NYIR emission: unsupported opcode %s at %s:%u:%u",
                   nyr_op_name(in->op),
                   in->debug.file && *in->debug.file ? in->debug.file
                                                     : "<source>",
                   in->debug.line, in->debug.column ? in->debug.column : 1);
        else
          snprintf(err, err_len, "LLVM NYIR emission: unsupported opcode %s",
                   nyr_op_name(in->op));
      }
      unsupported = true;
      break;
    }
  }

  if (unsupported) {
    free(vals);
    free(value_types);
    free(local_allocas);
    free(blocks);
    free(block_tails);
    free(block_branch_sources);
    free(block_branch_targets);
    cg->di_scope = previous_scope;
    cg->di_loc = previous_loc;
    LLVMSetCurrentDebugLocation2(cg->builder, previous_loc);
    return false;
  }

  /*
   * 6. Populate PHI incoming values
   */
  if (phi_count > 0) {
    int64_t phi_block_label = -1;
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_LABEL)
        phi_block_label = in->imm;
      if (in->op == NYIR_PHI && in->dst >= 0 && (size_t)in->dst < val_count &&
          vals[in->dst]) {
        LLVMValueRef phi_val = vals[in->dst];
        for (size_t k = 0; k < in->phi_incoming_len; ++k) {
          int64_t plbl = in->phi_incoming[k].predecessor_label;
          int inc_v = in->phi_incoming[k].value;
          LLVMTypeRef phi_type = LLVMTypeOf(phi_val);
          LLVMValueRef raw_inc =
              (inc_v >= 0 && (size_t)inc_v < val_count && vals[inc_v])
                  ? vals[inc_v]
                  : (phi_type == cg->type_i64 ? ny_llvm_c0(cg)
                                              : LLVMGetUndef(phi_type));
          if (plbl >= 0 && (size_t)plbl < num_blocks && blocks[plbl]) {
            bool added = false;
            if (block_branch_targets[plbl] == phi_block_label &&
                block_branch_sources[plbl]) {
              LLVMBasicBlockRef branch_bb = block_branch_sources[plbl];
              LLVMValueRef term = LLVMGetBasicBlockTerminator(branch_bb);
              if (term)
                LLVMPositionBuilderBefore(cg->builder, term);
              else
                LLVMPositionBuilderAtEnd(cg->builder, branch_bb);
              /*
               * Coercions may emit instructions (e.g. f64-to-i64 bitcast).
               * Build them on the incoming edge so their operands dominate
               * the PHI and the value is available on this predecessor.
               */
              LLVMValueRef incoming_value =
                  phi_type == cg->type_i64 ? ny_as_i64(cg, raw_inc)
                                           : (LLVMTypeOf(raw_inc) == phi_type
                                                  ? raw_inc
                                                  : LLVMGetUndef(phi_type));
              LLVMAddIncoming(phi_val, &incoming_value, &branch_bb, 1);
              added = true;
            }
            LLVMBasicBlockRef tail_bb = block_tails[plbl];
            LLVMValueRef tail_term =
                tail_bb ? LLVMGetBasicBlockTerminator(tail_bb) : NULL;
            bool tail_reaches_phi = false;
            if (tail_term && phi_block_label >= 0 &&
                (size_t)phi_block_label < num_blocks) {
              unsigned successors = LLVMGetNumSuccessors(tail_term);
              for (unsigned si = 0; si < successors; ++si)
                if (LLVMGetSuccessor(tail_term, si) == blocks[phi_block_label])
                  tail_reaches_phi = true;
            }
            if (tail_bb && tail_reaches_phi &&
                (!added || tail_bb != block_branch_sources[plbl])) {
              LLVMPositionBuilderBefore(cg->builder, tail_term);
              LLVMValueRef incoming_value =
                  phi_type == cg->type_i64 ? ny_as_i64(cg, raw_inc)
                                           : (LLVMTypeOf(raw_inc) == phi_type
                                                  ? raw_inc
                                                  : LLVMGetUndef(phi_type));
              LLVMAddIncoming(phi_val, &incoming_value, &tail_bb, 1);
              added = true;
            }
            if (!added) {
              LLVMBasicBlockRef fallback_bb = blocks[plbl];
              LLVMValueRef term = LLVMGetBasicBlockTerminator(fallback_bb);
              if (term)
                LLVMPositionBuilderBefore(cg->builder, term);
              else
                LLVMPositionBuilderAtEnd(cg->builder, fallback_bb);
              LLVMValueRef incoming_value =
                  phi_type == cg->type_i64 ? ny_as_i64(cg, raw_inc)
                                           : (LLVMTypeOf(raw_inc) == phi_type
                                                  ? raw_inc
                                                  : LLVMGetUndef(phi_type));
              LLVMAddIncoming(phi_val, &incoming_value, &fallback_bb, 1);
            }
          }
        }
      }
    }
  }

  /*
   * 7. Ensure all blocks have terminators
   */
  for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;
       bb = LLVMGetNextBasicBlock(bb)) {
    if (!LLVMGetBasicBlockTerminator(bb)) {
      LLVMPositionBuilderAtEnd(cg->builder, bb);
      /*
       * Dead/unreachable NYIR blocks still need a well-typed terminator for
       * LLVM's verifier.  Returning the default i64 value here was harmless
       * for integer functions, but made otherwise valid f32/f64 WebAssembly
       * modules fail in clang with "value doesn't match function result
       * type" (large UI programs expose these blocks frequently).
       */
      LLVMValueRef fallback =
          ret_ty == cg->type_f64   ? LLVMConstReal(cg->type_f64, 0.0)
          : ret_ty == cg->type_f32 ? LLVMConstReal(cg->type_f32, 0.0)
                                   : ny_llvm_c0(cg);
      LLVMBuildRet(cg->builder, fallback);
    }
  }

  free(vals);
  free(value_types);
  free(local_allocas);
  free(blocks);
  free(block_tails);
  free(block_branch_sources);
  free(block_branch_targets);
  cg->di_scope = previous_scope;
  cg->di_loc = previous_loc;
  LLVMSetCurrentDebugLocation2(cg->builder, previous_loc);
  return true;
}

bool ny_llvm_emit_nyir_module(ny_llvm_context_t *cg,
                              nyr_backend_module_t *module, char *err,
                              size_t err_len) {
  if (!cg || !module)
    return false;

  /*
   * Stream the borrowed neutral module into LLVM and check a bounded RSS cap
   * between functions. The language pipeline owns lowering and module
   * lifetime; this boundary consumes NYIR only.
   */
  long rss_cap_mb = cg->module_declarations_only ? 8192 : 12288;
  const char *rss_cap_env = getenv("NYTRIX_STD_BC_MAX_RSS_MB");
  if (rss_cap_env && *rss_cap_env) {
    char *end = NULL;
    long v = strtol(rss_cap_env, &end, 10);
    if (end != rss_cap_env && v > 0 && v < 1000000)
      rss_cap_mb = v;
  }
  bool ok = true;
  for (size_t i = 0; i < module->function_count; ++i) {
    const nyr_func_t *func = &module->functions[i];
    const char *name = module->function_names && module->function_names[i]
                           ? module->function_names[i]
                           : "<fn>";
    if (getenv("NYTRIX_STD_BC_TRACE_FUNCTIONS")) {
      fprintf(stderr, "[std-bc] emitting function %zu/%zu %s (nyir=%zu)\n",
              i + 1, module->function_count, name, func->len);
      fflush(stderr);
    }
    if (!ny_llvm_emit_nyir_func(cg, (nyr_func_t *)func, name, false, err,
                                err_len)) {
      ok = false;
      break;
    }
    nyr_ir_func_free(&module->functions[i]);
    unsigned long long rss_kb = 0;
    FILE *sf = fopen("/proc/self/status", "r");
    if (sf) {
      char line[256];
      while (fgets(line, sizeof(line), sf)) {
        unsigned long long kb = 0;
        if (sscanf(line, "VmRSS: %llu kB", &kb) == 1) {
          rss_kb = kb;
          break;
        }
      }
      fclose(sf);
    }
    if (rss_kb > (unsigned long long)rss_cap_mb * 1024ULL) {
      if (err && err_len > 0)
        snprintf(err, err_len,
                 "stdlib bitcode RSS cap exceeded: %llu MiB > %ld MiB at "
                 "function %zu/%zu (%s); set "
                 "NYTRIX_STD_BC_MAX_RSS_MB to raise",
                 rss_kb / 1024ULL, rss_cap_mb, i + 1, module->function_count,
                 name);
      ok = false;
      break;
    }
  }

  /*
   * Module-only compilations contribute declarations and function bodies to
   * their parent, so they do not emit a synthetic entry point.
   */
  if (ok && module->entry && module->entry->len > 0 &&
      !cg->module_declarations_only && cg->emit_entry) {
    if (getenv("NY_DUMP_LLVM"))
      nyr_dump(stderr, module->entry, "rt_main_pre_emit");
    ok = ny_llvm_emit_nyir_func(cg, (nyr_func_t *)module->entry,
                                "_ny_top_entry", false, err, err_len);
  }
  if (module->entry)
    nyr_ir_func_free(module->entry);
  if (ok && getenv("NY_DUMP_LLVM"))
    fprintf(stderr, "%s", LLVMPrintModuleToString(cg->module));
  return ok;
}
