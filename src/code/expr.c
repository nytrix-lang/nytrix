#include "base/common.h"
#include "base/util.h"
#include "priv.h"
#include "rt/shared.h"
#include "std_symbols.h"
#include <alloca.h>
#include <ctype.h>
#include <inttypes.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

// Helpers moved to gencall.c

static LLVMValueRef expr_fail(codegen_t *cg, token_t tok, const char *fmt,
                              ...) {
  va_list ap;
  va_start(ap, fmt);
  char msg[512];
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  ny_diag_error(tok, "%s", msg);
  cg->had_error = 1;
  return LLVMConstInt(cg->type_i64, 0, false);
}

LLVMValueRef gen_closure(codegen_t *cg, scope *scopes, size_t depth,
                         ny_param_list params, stmt_t *body, bool is_variadic,
                         const char *return_type, const char *name_hint) {
  /* Capture All Visible Variables (scopes[1..depth]) */
  binding_list captures;
  vec_init(&captures);
  for (ssize_t i = 1; i <= (ssize_t)depth; i++) {
    for (size_t j = 0; j < scopes[i].vars.len; j++) {
      vec_push(&captures, scopes[i].vars.data[j]);
      // Mark the original variable as used since it's being captured
      scopes[i].vars.data[j].is_used = true;
    }
  }
  char name[64];
  if (name_hint && strncmp(name_hint, "__lambda", 8) == 0) {
    snprintf(name, sizeof(name), "%s_%d", name_hint, cg->lambda_count++);
  } else {
    snprintf(name, sizeof(name), "%s_%d", name_hint ? name_hint : "__lambda",
             cg->lambda_count++);
  }
  stmt_t sfn;
  memset(&sfn, 0, sizeof(sfn));
  sfn.kind = NY_S_FUNC;
  sfn.as.fn.name = strdup(name);
  sfn.as.fn.params = params;
  sfn.as.fn.body = body;
  sfn.as.fn.is_variadic = is_variadic;
  sfn.as.fn.return_type = return_type;
  // Copy location from body if possible
  if (body)
    sfn.tok = body->tok;
  scope sc[64] = {0};
  // Check if we actually need environment. name_hint check for __defer is
  // heuristic.
  bool uses_env = captures.len > 0;
  if (name_hint && strcmp(name_hint, "__defer") == 0)
    uses_env = true;

  gen_func(cg, &sfn, name, sc, 0, uses_env ? &captures : NULL);
  free((void *)sfn.as.fn.name);
  LLVMValueRef lf = LLVMGetNamedFunction(cg->module, name);
  LLVMValueRef fn_ptr_tagged = LLVMBuildOr(
      cg->builder, LLVMBuildPtrToInt(cg->builder, lf, cg->type_i64, ""),
      LLVMConstInt(cg->type_i64, 2, false), "");

  if (!uses_env) {
    /* Standard function (tag 2) if no captures */
    vec_free(&captures);
    return fn_ptr_tagged;
  }
  /* Create Env */
  fun_sig *malloc_sig = lookup_fun(cg, "__malloc");
  if (!malloc_sig) {
    token_t tok = body ? body->tok : (token_t){0};
    return expr_fail(cg, tok, "__malloc required for closures");
  }
  LLVMValueRef env_alloc_size = LLVMConstInt(
      cg->type_i64, (uint64_t)(((uint64_t)captures.len * 8) << 1) | 1, false);
  LLVMValueRef env_ptr =
      LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                     (LLVMValueRef[]){env_alloc_size}, 1, "env");
  LLVMValueRef env_raw = LLVMBuildIntToPtr(
      cg->builder, env_ptr, LLVMPointerType(cg->type_i64, 0), "env_raw");
  for (size_t i = 0; i < captures.len; i++) {
    LLVMValueRef slot_val =
        LLVMBuildLoad2(cg->builder, cg->type_i64, captures.data[i].value, "");
    LLVMValueRef dst = LLVMBuildGEP2(
        cg->builder, cg->type_i64, env_raw,
        (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)i, false)}, 1,
        "");
    LLVMBuildStore(cg->builder, slot_val, dst);
  }
  /* Create Closure Object [Tag=105 | Code | Env] */
  LLVMValueRef cls_size =
      LLVMConstInt(cg->type_i64, ((uint64_t)16 << 1) | 1, false);
  LLVMValueRef cls_ptr =
      LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                     (LLVMValueRef[]){cls_size}, 1, "closure");
  LLVMValueRef cls_raw = LLVMBuildIntToPtr(
      cg->builder, cls_ptr, LLVMPointerType(cg->type_i64, 0), "");
  /* Set Tag -8 */
  LLVMValueRef tag_addr = LLVMBuildGEP2(
      cg->builder, LLVMInt8TypeInContext(cg->ctx),
      LLVMBuildBitCast(cg->builder, cls_raw,
                       LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0), ""),
      (LLVMValueRef[]){LLVMConstInt(cg->type_i64, -8, true)}, 1, "");
  LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 105, false),
                 LLVMBuildBitCast(cg->builder, tag_addr,
                                  LLVMPointerType(cg->type_i64, 0), ""));
  /* Store Code at 0 */
  LLVMBuildStore(cg->builder, fn_ptr_tagged, cls_raw);
  /* Store Env at 8 */
  LLVMValueRef env_store_addr = LLVMBuildGEP2(
      cg->builder, cg->type_i64, cls_raw,
      (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 1, false)}, 1, "");
  LLVMBuildStore(cg->builder, env_ptr, env_store_addr);
  vec_free(&captures);
  return cls_ptr;
}

LLVMValueRef to_bool(codegen_t *cg, LLVMValueRef v) {
  LLVMValueRef is_none =
      LLVMBuildICmp(cg->builder, LLVMIntEQ, v,
                    LLVMConstInt(cg->type_i64, 0, false), "is_none");
  LLVMValueRef is_false =
      LLVMBuildICmp(cg->builder, LLVMIntEQ, v,
                    LLVMConstInt(cg->type_i64, 4, false), "is_false");
  LLVMValueRef is_zero =
      LLVMBuildICmp(cg->builder, LLVMIntEQ, v,
                    LLVMConstInt(cg->type_i64, 1, false), "is_zero");
  return LLVMBuildNot(
      cg->builder,
      LLVMBuildOr(cg->builder, LLVMBuildOr(cg->builder, is_none, is_false, ""),
                  is_zero, ""),
      "to_bool");
}

LLVMValueRef const_string_ptr(codegen_t *cg, const char *s, size_t len) {
  /* Interning loop disabled to prevent cross-module issues in REPL */
  /*
  for (size_t i = 0; i < cg->interns.len; ++i)
    if (cg->interns.data[i].len == len &&
        memcmp(cg->interns.data[i].data, s, len) == 0)
      return cg->interns.data[i].val;
  */
  const char *final_s = s;
  size_t final_len = len;
  size_t header_size = 64;
  size_t tail_size = 16;
  size_t total_len = header_size + final_len + 1 + tail_size;
  char *obj_data = calloc(1, total_len);
  // Write Header
  // We do NOT write heap magic numbers (NY_MAGIC1/2) here.
  // If we did, the runtime would treat this as a heap pointer and strict bounds
  // checking (__check_oob) would forbid accessing header fields (like length
  // at -16). By leaving magics as 0, is_heap_ptr returns false, allowing
  // access.
  *(uint64_t *)(obj_data) = 0;                       // NY_MAGIC1;
  *(uint64_t *)(obj_data + 8) = (uint64_t)final_len; // Capacity
  *(uint64_t *)(obj_data + 16) = 0;                  // NY_MAGIC2;
  *(uint64_t *)(obj_data + 48) =
      ((uint64_t)final_len << 1) | 1; // Length at p-16 (tagged)
  *(uint64_t *)(obj_data + 56) = 241; // Tag at p-8 (TAG_STR)
  // Write Data
  memcpy(obj_data + header_size, final_s, final_len);
  obj_data[header_size + final_len] = '\0';
  // Write Tail
  uint64_t magic3 = NY_MAGIC3;
  memcpy(obj_data + header_size + final_len + 1, &magic3, sizeof(magic3));
  LLVMTypeRef arr_ty =
      LLVMArrayType(LLVMInt8TypeInContext(cg->ctx), (unsigned)total_len);
  LLVMValueRef g = LLVMAddGlobal(cg->module, arr_ty, ".str");
  LLVMSetInitializer(g, LLVMConstStringInContext(cg->ctx, obj_data,
                                                 (unsigned)total_len, true));
  LLVMSetGlobalConstant(g, true);
  LLVMSetLinkage(g, LLVMInternalLinkage);
  LLVMSetUnnamedAddr(g, true);
  LLVMSetAlignment(g, 64);
  // Store the global and metadata
  string_intern in = {.data = obj_data + header_size,
                      .len = final_len,
                      .val = g,
                      .gv = g,
                      .alloc = obj_data};
  vec_push(&cg->interns, in);
  // Create a global i64 variable to hold the runtime pointer address
  // This is initialized to 0 but will be set in a runtime init function
  char ptr_name[128];
  snprintf(ptr_name, sizeof(ptr_name), ".str.runtime.%zu", cg->interns.len - 1);
  LLVMValueRef runtime_ptr_global =
      LLVMAddGlobal(cg->module, cg->type_i64, ptr_name);
  LLVMValueRef indices[] = {
      LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, false),
      LLVMConstInt(LLVMInt32TypeInContext(cg->ctx),
                   (unsigned long long)header_size, false)};
  LLVMValueRef const_ptr = LLVMConstGEP2(arr_ty, g, indices, 2);
  LLVMSetInitializer(runtime_ptr_global,
                     LLVMConstPtrToInt(const_ptr, cg->type_i64));
  LLVMSetLinkage(runtime_ptr_global, LLVMInternalLinkage);
  // Store this runtime pointer global in the intern struct
  cg->interns.data[cg->interns.len - 1].val = runtime_ptr_global;
  // Return the runtime pointer global (callers will load from it)
  return runtime_ptr_global;
}

LLVMValueRef gen_binary(codegen_t *cg, const char *op, LLVMValueRef l,
                        LLVMValueRef r) {
  const char *generic_name = NULL;
  const char *builtin_name = NULL;

  if (strcmp(op, "+") == 0) {
    generic_name = "add";
    builtin_name = "__add";
  } else if (strcmp(op, "-") == 0) {
    generic_name = "sub";
    builtin_name = "__sub";
  } else if (strcmp(op, "*") == 0) {
    generic_name = "mul";
    builtin_name = "__mul";
  } else if (strcmp(op, "/") == 0) {
    generic_name = "div";
    builtin_name = "__div";
  } else if (strcmp(op, "%") == 0) {
    generic_name = "mod";
    builtin_name = "__mod";
  } else if (strcmp(op, "|") == 0) {
    generic_name = "bor";
    builtin_name = "__or";
  } else if (strcmp(op, "&") == 0) {
    generic_name = "band";
    builtin_name = "__and";
  } else if (strcmp(op, "^") == 0) {
    generic_name = "bxor";
    builtin_name = "__xor";
  } else if (strcmp(op, "<") == 0) {
    generic_name = "lt";
    builtin_name = "__lt";
  } else if (strcmp(op, "<=") == 0) {
    generic_name = "le";
    builtin_name = "__le";
  } else if (strcmp(op, ">") == 0) {
    generic_name = "gt";
    builtin_name = "__gt";
  } else if (strcmp(op, ">=") == 0) {
    generic_name = "ge";
    builtin_name = "__ge";
  } else if (strcmp(op, "<<") == 0) {
    generic_name = "bshl";
    builtin_name = "__shl";
  } else if (strcmp(op, ">>") == 0) {
    generic_name = "bshr";
    builtin_name = "__shr";
  }

  if (strcmp(op, "==") == 0) {
    fun_sig *s = lookup_fun(cg, "std.core.reflect.eq");
    if (!s)
      s = lookup_fun(cg, "eq");
    if (!s)
      s = lookup_fun(cg, "__eq");
    if (!s) {
      return expr_fail(cg, (token_t){0}, "'==' requires 'eq' (or __eq)");
    }
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){l, r}, 2, "");
  }

  if (generic_name) {
    char full_generic[128];
    snprintf(full_generic, sizeof(full_generic), "std.core.reflect.%s",
             generic_name);
    fun_sig *s = lookup_fun(cg, full_generic);
    if (!s)
      s = lookup_fun(cg, generic_name);

    if (s && strcmp(s->name, builtin_name) != 0) {
      if (s->stmt_t && !ny_is_stdlib_tok(s->stmt_t->tok)) {
        s = NULL;
      }
    }
    if (s && strcmp(s->name, builtin_name) != 0) {
      return LLVMBuildCall2(cg->builder, s->type, s->value,
                            (LLVMValueRef[]){l, r}, 2, "");
    }
  }

  if (builtin_name) {
    fun_sig *s = lookup_fun(cg, builtin_name);
    if (!s) {
      return expr_fail(cg, (token_t){0}, "builtin %s missing", builtin_name);
    }
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){l, r}, 2, "");
  }

  if (strcmp(op, "!=") == 0)
    return LLVMBuildSub(cg->builder, LLVMConstInt(cg->type_i64, 6, false),
                        gen_binary(cg, "==", l, r), "");

  if (strcmp(op, "in") == 0) {
    fun_sig *s = lookup_fun(cg, "contains");
    if (!s) {
      return expr_fail(cg, (token_t){0}, "'in' requires 'contains'");
    }
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){r, l}, 2, "");
  }
  return expr_fail(cg, (token_t){0}, "undefined operator '%s'", op);
}

LLVMValueRef gen_comptime_eval(codegen_t *cg, stmt_t *body) {
  LLVMContextRef ctx = LLVMContextCreate();
  LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("ct", ctx);
  LLVMBuilderRef bld = LLVMCreateBuilderInContext(ctx);
  codegen_t tcg;
  codegen_init_with_context(&tcg, cg->prog, cg->arena, mod, ctx, bld);
  tcg.llvm_ctx_owned = true;
  tcg.comptime = true;

  LLVMValueRef fn =
      LLVMAddFunction(mod, "ctm", LLVMFunctionType(tcg.type_i64, NULL, 0, 0));
  LLVMPositionBuilderAtEnd(bld, LLVMAppendBasicBlock(fn, "e"));
  scope sc[64] = {0};
  size_t d = 0;
  gen_stmt(&tcg, sc, &d, body, 0, true);
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(bld)))
    LLVMBuildRet(bld, LLVMConstInt(tcg.type_i64, 1, false));
  LLVMExecutionEngineRef ee;
  LLVMCreateJITCompilerForModule(&ee, mod, 3, NULL);
  // EE now owns the module, so prevent codegen_dispose from freeing it
  tcg.module = NULL;
  int64_t (*f)(void) = (int64_t (*)(void))LLVMGetFunctionAddress(ee, "ctm");
  int64_t res = f ? f() : 0;
  LLVMDisposeExecutionEngine(ee);
  codegen_dispose(
      &tcg); // This will dispose the context since llvm_ctx_owned=true
  if ((res & 1) == 0) {
    token_t tok = body ? body->tok : (token_t){0};
    return expr_fail(cg, tok, "comptime must return an int64 (tagged int)");
  }
  return LLVMConstInt(cg->type_i64, res, true);
}

static LLVMValueRef gen_expr_unary(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e) {
  LLVMValueRef r = gen_expr(cg, scopes, depth, e->as.unary.right);
  ny_dbg_loc(cg, e->tok);
  if (strcmp(e->as.unary.op, "!") == 0)
    return LLVMBuildSelect(cg->builder, to_bool(cg, r),
                           LLVMConstInt(cg->type_i64, 4, false),
                           LLVMConstInt(cg->type_i64, 2, false), "");
  if (strcmp(e->as.unary.op, "-") == 0) {
    fun_sig *s = lookup_fun(cg, "__sub");
    return LLVMBuildCall2(
        cg->builder, s->type, s->value,
        (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 1, false), r}, 2, "");
  }
  if (strcmp(e->as.unary.op, "~") == 0) {
    fun_sig *s = lookup_fun(cg, "__not");
    return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){r},
                          1, "");
  }
  return expr_fail(cg, e->tok, "unsupported unary operator '%s'",
                   e->as.unary.op);
}

static LLVMValueRef gen_expr_index(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e) {
  if (e->as.index.stop || e->as.index.step || !e->as.index.start) {
    fun_sig *s = lookup_fun(cg, "slice");
    if (!s)
      return expr_fail(cg, e->tok, "slice operation requires 'slice'");
    LLVMValueRef start = e->as.index.start
                             ? gen_expr(cg, scopes, depth, e->as.index.start)
                             : LLVMConstInt(cg->type_i64, 1, false); // 0 tagged
    LLVMValueRef stop =
        e->as.index.stop
            ? gen_expr(cg, scopes, depth, e->as.index.stop)
            : LLVMConstInt(cg->type_i64, ((0x3fffffffULL) << 1) | 1, false);
    LLVMValueRef step = e->as.index.step
                            ? gen_expr(cg, scopes, depth, e->as.index.step)
                            : LLVMConstInt(cg->type_i64, 3, false); // 1 tagged
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(
        cg->builder, s->type, s->value,
        (LLVMValueRef[]){gen_expr(cg, scopes, depth, e->as.index.target), start,
                         stop, step},
        4, "");
  }
  fun_sig *s = lookup_fun(cg, "get");
  if (!s)
    s = lookup_fun(cg, "std.core.get");
  if (!s)
    return expr_fail(cg, e->tok, "index operation requires 'get'");
  ny_dbg_loc(cg, e->tok);
  return LLVMBuildCall2(
      cg->builder, s->type, s->value,
      (LLVMValueRef[]){gen_expr(cg, scopes, depth, e->as.index.target),
                       gen_expr(cg, scopes, depth, e->as.index.start)},
      2, "");
}

static LLVMValueRef gen_expr_list_like(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e) {
  fun_sig *ls = lookup_fun(cg, "list");
  if (!ls)
    ls = lookup_fun(cg, "std.core.list");
  fun_sig *as = lookup_fun(cg, "append");
  if (!as)
    as = lookup_fun(cg, "std.core.append");
  if (!ls || !as)
    return expr_fail(cg, e->tok, "list literal requires list/append helpers");
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef vl = LLVMBuildCall2(
      cg->builder, ls->type, ls->value,
      (LLVMValueRef[]){LLVMConstInt(
          cg->type_i64, ((uint64_t)e->as.list_like.len << 1) | 1, false)},
      1, "");
  for (size_t i = 0; i < e->as.list_like.len; i++)
    vl = LLVMBuildCall2(cg->builder, as->type, as->value,
                        (LLVMValueRef[]){vl, gen_expr(cg, scopes, depth,
                                                      e->as.list_like.data[i])},
                        2, "");
  return vl;
}

static LLVMValueRef gen_expr_dict(codegen_t *cg, scope *scopes, size_t depth,
                                  expr_t *e) {
  fun_sig *ds = lookup_fun(cg, "dict");
  if (!ds)
    ds = lookup_fun(cg, "std.core.dict");
  fun_sig *ss = lookup_fun(cg, "dict_set");
  if (!ss)
    ss = lookup_fun(cg, "std.core.dict_set");
  if (!ds || !ss)
    return expr_fail(cg, e->tok, "dict literal requires dict/dict_set helpers");
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef dl = LLVMBuildCall2(
      cg->builder, ds->type, ds->value,
      (LLVMValueRef[]){LLVMConstInt(
          cg->type_i64, ((uint64_t)e->as.dict.pairs.len << 2) | 1, false)},
      1, "");
  for (size_t i = 0; i < e->as.dict.pairs.len; i++)
    LLVMBuildCall2(
        cg->builder, ss->type, ss->value,
        (LLVMValueRef[]){
            dl, gen_expr(cg, scopes, depth, e->as.dict.pairs.data[i].key),
            gen_expr(cg, scopes, depth, e->as.dict.pairs.data[i].value)},
        3, "");
  return dl;
}

static LLVMValueRef gen_expr_set(codegen_t *cg, scope *scopes, size_t depth,
                                 expr_t *e) {
  fun_sig *ss = lookup_fun(cg, "set");
  if (!ss)
    ss = lookup_fun(cg, "std.core.set");
  fun_sig *as = lookup_fun(cg, "set_add");
  if (!as)
    as = lookup_fun(cg, "std.core.set_add");
  if (!ss || !as)
    return expr_fail(cg, e->tok, "set literal requires set/set_add helpers");
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

static LLVMValueRef gen_expr_logical(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  bool and = strcmp(e->as.logical.op, "&&") == 0;
  LLVMValueRef left =
      to_bool(cg, gen_expr(cg, scopes, depth, e->as.logical.left));
  LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
  LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlock(f, "lrhs"),
                    end_bb = LLVMAppendBasicBlock(f, "lend");
  ny_dbg_loc(cg, e->tok);
  if (and)
    LLVMBuildCondBr(cg->builder, left, rhs_bb, end_bb);
  else
    LLVMBuildCondBr(cg->builder, left, end_bb, rhs_bb);
  LLVMPositionBuilderAtEnd(cg->builder, rhs_bb);
  LLVMValueRef rv = gen_expr(cg, scopes, depth, e->as.logical.right);
  LLVMBuildBr(cg->builder, end_bb);
  LLVMBasicBlockRef rend_bb = LLVMGetInsertBlock(cg->builder);
  LLVMPositionBuilderAtEnd(cg->builder, end_bb);
  LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "");
  LLVMAddIncoming(phi,
                  (LLVMValueRef[]){and ? LLVMConstInt(cg->type_i64, 4, false)
                                       : LLVMConstInt(cg->type_i64, 2, false),
                                   rv},
                  (LLVMBasicBlockRef[]){cur_bb, rend_bb}, 2);
  return phi;
}

LLVMValueRef gen_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {

  // Check for dead code - don't generate instructions if block is terminated
  if (cg->builder) {
    LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
    if (cur_bb && LLVMGetBasicBlockTerminator(cur_bb)) {
      return LLVMGetUndef(cg->type_i64);
    }
  }
  if (!e || cg->had_error)
    return LLVMConstInt(cg->type_i64, 0, false);
  ny_dbg_loc(cg, e->tok);

  // fprintf(stderr, "DEBUG: gen_expr kind %d tok %d\n", e->kind, e->tok.kind);

  switch (e->kind) {
  case NY_E_COMPTIME:
    return gen_comptime_eval(cg, e->as.comptime_expr.body);
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT)
      return LLVMConstInt(cg->type_i64, ((uint64_t)e->as.literal.as.i << 1) | 1,
                          true);
    if (e->as.literal.kind == NY_LIT_BOOL)
      return LLVMConstInt(cg->type_i64, e->as.literal.as.b ? 2 : 4, false);
    if (e->as.literal.kind == NY_LIT_STR) {
      // Get the runtime pointer global for this string
      LLVMValueRef str_runtime_global =
          const_string_ptr(cg, e->as.literal.as.s.data, e->as.literal.as.s.len);
      // Load the pointer value (will be initialized by string init function)
      return LLVMBuildLoad2(cg->builder, cg->type_i64, str_runtime_global,
                            "str_ptr");
    }
    if (e->as.literal.kind == NY_LIT_FLOAT) {
      fun_sig *box_sig = lookup_fun(cg, "__flt_box_val");
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
      return LLVMBuildCall2(cg->builder, box_sig->type, box_sig->value,
                            (LLVMValueRef[]){LLVMBuildBitCast(
                                cg->builder, fval, cg->type_i64, "")},
                            1, "");
    }
    return LLVMConstInt(cg->type_i64, 0, false);
  case NY_E_IDENT: {
    binding *b = scope_lookup(scopes, depth, e->as.ident.name);
    if (b) {
      b->is_used = true;
      if (LLVMGetTypeKind(LLVMTypeOf(b->value)) == LLVMPointerTypeKind)
        return LLVMBuildLoad2(cg->builder, cg->type_i64, b->value, "");
      return b->value;
    }
    binding *gb = lookup_global(cg, e->as.ident.name);
    if (gb) {
      gb->is_used = true;
      if (LLVMGetTypeKind(LLVMTypeOf(gb->value)) == LLVMPointerTypeKind) {
        return LLVMBuildLoad2(cg->builder, cg->type_i64, gb->value, "");
      }
      return gb->value;
    }
    fun_sig *s = lookup_fun(cg, e->as.ident.name);
    if (s) {
      LLVMValueRef sv = s->value;
      bool has_stmt = s->stmt_t != NULL;
      LLVMValueRef val = LLVMBuildPtrToInt(cg->builder, sv, cg->type_i64, "");
      if (has_stmt) {
        val = LLVMBuildOr(cg->builder, val,
                          LLVMConstInt(cg->type_i64, 2, false), "");
      }
      return val;
    }

    // NEW: Try resolving as an unqualified enum member
    enum_member_def_t *emd = lookup_enum_member(cg, e->as.ident.name);
    if (emd) {
      return LLVMConstInt(cg->type_i64, ((uint64_t)emd->value << 1) | 1, true);
    }

    report_undef_symbol(cg, e->as.ident.name, e->tok);
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  case NY_E_UNARY: {
    return gen_expr_unary(cg, scopes, depth, e);
  }
  case NY_E_BINARY:
  {
    LLVMValueRef l = gen_expr(cg, scopes, depth, e->as.binary.left);
    LLVMValueRef r = gen_expr(cg, scopes, depth, e->as.binary.right);
    ny_dbg_loc(cg, e->tok);
    return gen_binary(cg, e->as.binary.op, l, r);
  }
  case NY_E_CALL:
  case NY_E_MEMCALL:
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
    return LLVMConstInt(cg->type_i64, 0, false);
  case NY_E_DEREF: {
    LLVMValueRef ptr = gen_expr(cg, scopes, depth, e->as.deref.target);
    // Low level: treat ptr as raw address
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef raw_ptr =
        LLVMBuildIntToPtr(cg->builder, ptr, cg->type_i8ptr, "raw_ptr");
    return LLVMBuildLoad2(cg->builder, cg->type_i64, raw_ptr, "deref");
  }
  case NY_E_MEMBER: {
    // First, attempt to resolve as a static enum member or qualified global.
    char *full_name = codegen_full_name(cg, e, cg->arena);
    if (full_name) {
        enum_member_def_t *emd = lookup_enum_member(cg, full_name);
        if (emd) {
            return LLVMConstInt(cg->type_i64, ((uint64_t)emd->value << 1) | 1, true);
        }
        binding *gb = lookup_global(cg, full_name);
        if (gb) {
            gb->is_used = true;
            if (LLVMGetTypeKind(LLVMTypeOf(gb->value)) == LLVMPointerTypeKind)
                return LLVMBuildLoad2(cg->builder, cg->type_i64, gb->value, "");
            return gb->value;
        }
    }

    // Fallback to dynamic member access: `get(target, "member")`
    LLVMValueRef target = gen_expr(cg, scopes, depth, e->as.member.target);
    LLVMValueRef key_str_global = const_string_ptr(cg, e->as.member.name, strlen(e->as.member.name));
    LLVMValueRef key_str = LLVMBuildLoad2(cg->builder, cg->type_i64, key_str_global, "");

    fun_sig *get_sig = lookup_fun(cg, "get");
    if (!get_sig) {
        return expr_fail(cg, e->tok, "Member access on a dynamic object requires the 'get' function.");
    }

    // Assume `get` can take a default value as a third argument.
    LLVMValueRef args[] = {target, key_str};
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, get_sig->type, get_sig->value, args, 2, "");
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
      return LLVMConstInt(cg->type_i64, 1, false);
    }
    type_layout_t tl = resolve_raw_layout(cg, type_name, e->tok);
    if (!tl.is_valid) {
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 1, false);
    }
    uint64_t sz = (uint64_t)tl.size;
    return LLVMConstInt(cg->type_i64, (sz << 1) | 1ULL, false);
  }
  case NY_E_LOGICAL: {
    return gen_expr_logical(cg, scopes, depth, e);
  }
  case NY_E_TERNARY: {
    LLVMValueRef cond =
        to_bool(cg, gen_expr(cg, scopes, depth, e->as.ternary.cond));
    LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
    LLVMBasicBlockRef true_bb = LLVMAppendBasicBlock(f, "tern_true");
    LLVMBasicBlockRef false_bb = LLVMAppendBasicBlock(f, "tern_false");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(f, "tern_end");
    ny_dbg_loc(cg, e->tok);
    LLVMBuildCondBr(cg->builder, cond, true_bb, false_bb);
    LLVMPositionBuilderAtEnd(cg->builder, true_bb);
    LLVMValueRef true_val =
        gen_expr(cg, scopes, depth, e->as.ternary.true_expr);
    LLVMBuildBr(cg->builder, end_bb);
    LLVMBasicBlockRef true_end_bb = LLVMGetInsertBlock(cg->builder);
    LLVMPositionBuilderAtEnd(cg->builder, false_bb);
    LLVMValueRef false_val =
        gen_expr(cg, scopes, depth, e->as.ternary.false_expr);
    LLVMBuildBr(cg->builder, end_bb);
    LLVMBasicBlockRef false_end_bb = LLVMGetInsertBlock(cg->builder);
    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "tern");
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
    // Empty string init
    LLVMValueRef empty_runtime_global = const_string_ptr(cg, "", 0);
    LLVMValueRef res =
        LLVMBuildLoad2(cg->builder, cg->type_i64, empty_runtime_global, "");
    fun_sig *cs = lookup_fun(cg, "__str_concat"),
            *ts = lookup_fun(cg, "__to_str");
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t p = e->as.fstring.parts.data[i];
      LLVMValueRef pv;
      if (p.kind == NY_FSP_STR) {
        LLVMValueRef part_runtime_global =
            const_string_ptr(cg, p.as.s.data, p.as.s.len);
        pv = LLVMBuildLoad2(cg->builder, cg->type_i64, part_runtime_global, "");
      } else {
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
    /* Capture All Visible Variables (scopes[1..depth]) */
    return gen_closure(cg, scopes, depth, e->as.lambda.params,
                       e->as.lambda.body, e->as.lambda.is_variadic,
                       e->as.lambda.return_type, "__lambda");
  }
  case NY_E_EMBED: {
    const char *fname = e->as.embed.path;
    // fprintf(stderr, "DEBUG: embed opening '%s'\n", fname);
    FILE *f = fopen(fname, "rb");
    if (!f) {
      char cwd[1024];
      if (getcwd(cwd, sizeof(cwd)))
        fprintf(stderr, "DEBUG: embed failed. CWD: %s, PATH: %s\n", cwd, fname);
      return expr_fail(cg, e->tok, "failed to open file for embed: %s", fname);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size);
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
    // fprintf(stderr, "DEBUG: file read, size %ld\n", size);
    LLVMValueRef g = const_string_ptr(cg, buf, (size_t)size);
    // fprintf(stderr, "DEBUG: global created\n");
    free(buf);
    return LLVMBuildLoad2(cg->builder, cg->type_i64, g, "embed_ptr");
  }
  case NY_E_TRY: {
    LLVMValueRef res = gen_expr(cg, scopes, depth, e->as.unary.right);
    LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlock(f, "try_ok");
    LLVMBasicBlockRef err_bb = LLVMAppendBasicBlock(f, "try_err");

    fun_sig *is_ok_sig = lookup_fun(cg, "__is_ok");
    if (!is_ok_sig) {
      return expr_fail(cg, e->tok, "__is_ok not found for '?' operator");
    }
    LLVMValueRef is_ok = LLVMBuildCall2(cg->builder, is_ok_sig->type,
                                        is_ok_sig->value, &res, 1, "");
    ny_dbg_loc(cg, e->tok);
    LLVMBuildCondBr(cg->builder, to_bool(cg, is_ok), ok_bb, err_bb);

    LLVMPositionBuilderAtEnd(cg->builder, err_bb);
    // return res (which is the error result)
    if (cg->result_store_val) {
      LLVMBuildStore(cg->builder, res, cg->result_store_val);
    } else {
      emit_defers(cg, scopes, depth, cg->func_root_idx);
      LLVMBuildRet(cg->builder, res);
    }
    // We need a dummy terminator if there are instructions after this try
    // but LLVM usually handles this if we branch.

    LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
    // Unwrap value
    fun_sig *unwrap_sig = lookup_fun(cg, "__unwrap");
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
    LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 1, false), slot);
    cg->result_store_val = slot;
    stmt_t fake = {.kind = NY_S_MATCH, .as.match = e->as.match, .tok = e->tok};
    size_t d = depth;
    gen_stmt(cg, scopes, &d, &fake, cg->func_root_idx, true);
    cg->result_store_val = old_store;
    return LLVMBuildLoad2(cg->builder, cg->type_i64, slot, "");
  }
  default: {
    return expr_fail(cg, e->tok,
                     "unsupported expression kind %d (token kind %d)", e->kind,
                     e->tok.kind);
  }
  }
}
