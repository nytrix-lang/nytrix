#include "priv.h"
#include "std_symbols.h"
#include <alloca.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

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
  for (size_t i = 0; i < cg->interns.len; ++i)
    if (cg->interns.data[i].len == len &&
        memcmp(cg->interns.data[i].data, s, len) == 0)
      return cg->interns.data[i].val;
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
  // *(uint64_t*)(obj_data) = 0x545249584E5954ULL; // NY_MAGIC1
  // *(uint64_t*)(obj_data + 8) = total_len - 128; // Raw capacity
  // *(uint64_t*)(obj_data + 16) = 0x4E59545249584EULL; // NY_MAGIC2
  *(uint64_t *)(obj_data + 48) =
      ((uint64_t)final_len << 1) | 1; // Length at p-16 (tagged)
  *(uint64_t *)(obj_data + 56) = 241; // Tag at p-8 (TAG_STR)
  // Write Data
  memcpy(obj_data + header_size, final_s, final_len);
  obj_data[header_size + final_len] = '\0';
  // Write Tail
  // *(uint64_t*)(obj_data + header_size + final_len + 1) = NY_MAGIC3
  LLVMTypeRef arr_ty =
      LLVMArrayType(LLVMInt8TypeInContext(cg->ctx), (unsigned)total_len);
  LLVMValueRef g = LLVMAddGlobal(cg->module, arr_ty, ".str");
  LLVMSetInitializer(g, LLVMConstStringInContext(cg->ctx, obj_data,
                                                 (unsigned)total_len, true));
  LLVMSetGlobalConstant(g, true);
  LLVMSetLinkage(g, LLVMPrivateLinkage);
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
  const char *rt = NULL;
  if (strcmp(op, "+") == 0)
    rt = "__add";
  else if (strcmp(op, "-") == 0)
    rt = "__sub";
  else if (strcmp(op, "*") == 0)
    rt = "__mul";
  else if (strcmp(op, "/") == 0)
    rt = "__div";
  else if (strcmp(op, "%") == 0)
    rt = "__mod";
  else if (strcmp(op, "|") == 0)
    rt = "__or";
  else if (strcmp(op, "&") == 0)
    rt = "__and";
  else if (strcmp(op, "^") == 0)
    rt = "__xor";
  else if (strcmp(op, "<") == 0)
    rt = "__lt";
  else if (strcmp(op, "<=") == 0)
    rt = "__le";
  else if (strcmp(op, ">") == 0)
    rt = "__gt";
  else if (strcmp(op, ">=") == 0)
    rt = "__ge";
  else if (strcmp(op, "<<") == 0)
    rt = "__shl";
  else if (strcmp(op, ">>") == 0)
    rt = "__shr";
  if (strcmp(op, "==") == 0) {
    fun_sig *s = lookup_fun(cg, "std.core.reflect.eq");
    if (!s)
      s = lookup_fun(cg, "eq");
    if (!s)
      s = lookup_fun(cg, "__eq");
    if (!s) {
      fprintf(stderr, "Error: '==' requires 'eq' (or __eq)\n");
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){l, r}, 2, "");
  }
  if (rt) {
    fun_sig *s = lookup_fun(cg, rt);
    if (!s) {
      fprintf(stderr, "Error: builtin %s missing\n", rt);
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){l, r}, 2, "");
  }
  if (strcmp(op, "!=") == 0)
    return LLVMBuildSub(cg->builder, LLVMConstInt(cg->type_i64, 6, false),
                        gen_binary(cg, "==", l, r), "");
  // Simplified: handled by __* functions above
  if (strcmp(op, "in") == 0) {
    fun_sig *s = lookup_fun(cg, "contains");
    if (!s) {
      fprintf(stderr, "Error: 'in' requires 'contains'\n");
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){r, l}, 2, "");
  }
  fprintf(stderr, "Error: undef op %s\n", op);
  cg->had_error = 1;
  return LLVMConstInt(cg->type_i64, 0, false);
}

LLVMValueRef gen_comptime_eval(codegen_t *cg, stmt_t *body) {
  LLVMContextRef ctx = LLVMContextCreate();
  LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("ct", ctx);
  LLVMBuilderRef bld = LLVMCreateBuilderInContext(ctx);
  codegen_t tcg = {.ctx = ctx,
                   .module = mod,
                   .builder = bld,
                   .prog = cg->prog,
                   .llvm_ctx_owned = true,
                   .comptime = true};
  tcg.fun_sigs.len = tcg.fun_sigs.cap = 0;
  tcg.fun_sigs.data = NULL;
  tcg.interns.len = tcg.interns.cap = 0;
  tcg.interns.data = NULL;
  tcg.type_i64 = LLVMInt64TypeInContext(ctx);
  add_builtins(&tcg);
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
  int64_t (*f)(void) = (int64_t (*)(void))LLVMGetFunctionAddress(ee, "ctm");
  int64_t res = f ? f() : 0;
  LLVMDisposeExecutionEngine(ee);
  LLVMContextDispose(ctx);
  if ((res & 1) == 0) {
    fprintf(stderr, "Error: comptime must return an int64 (tagged int)\n");
    cg->had_error = 1;
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  return LLVMConstInt(cg->type_i64, res, true);
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
        NY_LOG_ERR("__flt_box_val not found\n");
        cg->had_error = 1;
        return LLVMConstInt(cg->type_i64, 0, false);
      }
      LLVMValueRef fval =
          LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), e->as.literal.as.f);
      return LLVMBuildCall2(cg->builder, box_sig->type, box_sig->value,
                            (LLVMValueRef[]){LLVMBuildBitCast(
                                cg->builder, fval, cg->type_i64, "")},
                            1, "");
    }
    return LLVMConstInt(cg->type_i64, 0, false);
  case NY_E_IDENT: {
    binding *b = scope_lookup(scopes, depth, e->as.ident.name);
    if (b)
      return LLVMBuildLoad2(cg->builder, cg->type_i64, b->value, "");
    binding *gb = lookup_global(cg, e->as.ident.name);
    if (gb)
      return LLVMBuildLoad2(cg->builder, cg->type_i64, gb->value, "");
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
    fprintf(stderr, "%s:%d:%d: \033[31merror:\033[0m undef %s\n",
            e->tok.filename ? e->tok.filename : "unknown", e->tok.line,
            e->tok.col, e->as.ident.name);
    cg->had_error = 1;
    // Suggest
    const char *best = NULL;
    int best_d = 100;
    // Check funs
    for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
      const char *cand = cg->fun_sigs.data[i].name;
      int l1 = strlen(e->as.ident.name);
      int l2 = strlen(cand);
      if (abs(l1 - l2) > 3)
        continue;
      const char *dot = strrchr(cand, '.');
      const char *base = dot ? dot + 1 : cand;
      l2 = strlen(base);
      int d[32][32];
      if (l1 > 30)
        l1 = 30;
      if (l2 > 30)
        l2 = 30;
      for (int x = 0; x <= l1; x++)
        d[x][0] = x;
      for (int y = 0; y <= l2; y++)
        d[0][y] = y;
      for (int x = 1; x <= l1; x++) {
        for (int y = 1; y <= l2; y++) {
          int cost = (e->as.ident.name[x - 1] == base[y - 1]) ? 0 : 1;
          int dist_del = d[x - 1][y] + 1;
          int dist_ins = d[x][y - 1] + 1;
          int c_cost = d[x - 1][y - 1] + cost;
          int min = dist_del < dist_ins ? dist_del : dist_ins;
          if (c_cost < min)
            min = c_cost;
          d[x][y] = min;
        }
      }
      int dist = d[l1][l2];
      if (dist < best_d && dist < 4) {
        best_d = dist;
        best = cand;
      }
    }
    // Check globals
    for (size_t i = 0; i < cg->global_vars.len; ++i) {
      const char *cand = cg->global_vars.data[i].name;
      int l1 = strlen(e->as.ident.name);
      int l2 = strlen(cand);
      if (abs(l1 - l2) > 3)
        continue;
      const char *dot = strrchr(cand, '.');
      const char *base = dot ? dot + 1 : cand;
      l2 = strlen(base);
      int d[32][32];
      if (l1 > 30)
        l1 = 30;
      if (l2 > 30)
        l2 = 30;
      for (int x = 0; x <= l1; x++)
        d[x][0] = x;
      for (int y = 0; y <= l2; y++)
        d[0][y] = y;
      for (int x = 1; x <= l1; x++) {
        for (int y = 1; y <= l2; y++) {
          int cost = (e->as.ident.name[x - 1] == base[y - 1]) ? 0 : 1;
          int dist_del = d[x - 1][y] + 1;
          int dist_ins = d[x][y - 1] + 1;
          int c_cost = d[x - 1][y - 1] + cost;
          int min = dist_del < dist_ins ? dist_del : dist_ins;
          if (c_cost < min)
            min = c_cost;
          d[x][y] = min;
        }
      }
      int dist = d[l1][l2];
      if (dist < best_d && dist < 4) {
        best_d = dist;
        best = cand;
      }
    }
    if (best)
      fprintf(stderr, "       Did you mean '%s'?\n", best);
    cg->had_error = 1;
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  case NY_E_UNARY: {
    LLVMValueRef r = gen_expr(cg, scopes, depth, e->as.unary.right);
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
    fprintf(stderr, "Error: unsupported unary op %s\n", e->as.unary.op);
    cg->had_error = 1;
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  case NY_E_BINARY:
    return gen_binary(cg, e->as.binary.op,
                      gen_expr(cg, scopes, depth, e->as.binary.left),
                      gen_expr(cg, scopes, depth, e->as.binary.right));
  case NY_E_CALL:
  case NY_E_MEMCALL: {
    expr_call_t *c = (e->kind == NY_E_CALL) ? &e->as.call : NULL;
    expr_memcall_t *mc = (e->kind == NY_E_MEMCALL) ? &e->as.memcall : NULL;
    LLVMValueRef callee = NULL;
    LLVMTypeRef ft = NULL;
    LLVMValueRef fv = NULL;
    bool is_variadic = false;
    int sig_arity = 0;
    bool has_sig = false;
    bool skip_target = false;
    if (mc) {
      char buf[128];
      const char *prefixes[] = {"dict_",  "list_", "str_",    "set_", "bytes_",
                                "queue_", "heap_", "bigint_", NULL};
      fun_sig *sig_found = NULL;
      // Priority 1: Check if target is a module alias
      if (mc->target->kind == NY_E_IDENT) {
        const char *target_name = mc->target->as.ident.name;
        const char *module_name = target_name;
        for (size_t k = 0; k < cg->aliases.len; ++k) {
          if (strcmp(cg->aliases.data[k].name, target_name) == 0) {
            module_name = (const char *)cg->aliases.data[k].stmt_t;
            break;
          }
        }
        // If it's an alias, or if it's NOT a local function/variable/keyword,
        // it might be a module call (e.g. m.add)
        if (module_name != target_name ||
            (lookup_fun(cg, target_name) == NULL &&
             scope_lookup(scopes, depth, target_name) == NULL)) {
          char dotted[256];
          snprintf(dotted, sizeof(dotted), "%s.%s", module_name, mc->name);
          sig_found = lookup_fun(cg, dotted);
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
        }
      }
      // Priority 2: Check standard prefixes (dict_, list_, etc.)
      // Priority 1: Check if target is a module alias
      if (mc->target->kind == NY_E_IDENT) {
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
          char dotted[256];
          snprintf(dotted, sizeof(dotted), "%s.%s", module_name, mc->name);
          sig_found = lookup_fun(cg, dotted);
          if (sig_found) {
            ft = sig_found->type;
            fv = sig_found->value;
            sig_arity = sig_found->arity;
            is_variadic = sig_found->is_variadic;
            has_sig = true;
            callee = fv;
            goto static_call_handling;
          }
          // If it was an ALIAS, but method not found, we shouldn't fall back to
          // standard methods
          if (is_alias) {
            fprintf(stderr, "Error: function %s.%s not found\n", module_name,
                    mc->name);
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
    static_call_handling:;
      if (!sig_found) {
        const char *tname = (mc && mc->target->kind == NY_E_IDENT)
                                ? mc->target->as.ident.name
                                : "<expr_t>";
        fprintf(stderr, "Error: function %s.%s not found\n", tname, mc->name);
        // Suggest corrections
        const char *best_match = NULL;
        int best_dist = 100;
        for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
          const char *candidate = cg->fun_sigs.data[i].name;
          // Simple distance check: Levenshtein or substring
          // We'll implemented a simple distance inline to avoid large function
          int len1 = strlen(mc->name);
          int len2 = strlen(candidate);
          if (abs(len1 - len2) > 3)
            continue;
          // Substring match for namespacing suggestions?
          // Or just check suffix matching?
          const char *dot = strrchr(candidate, '.');
          const char *base = dot ? dot + 1 : candidate;
          // Levenshtein on base name
          int d[32][32]; // Max name length 31 for suggestion optimization
          int l1 = strlen(mc->name);
          int l2 = strlen(base);
          if (l1 > 30)
            l1 = 30;
          if (l2 > 30)
            l2 = 30;
          for (int x = 0; x <= l1; x++)
            d[x][0] = x;
          for (int y = 0; y <= l2; y++)
            d[0][y] = y;
          for (int x = 1; x <= l1; x++) {
            for (int y = 1; y <= l2; y++) {
              int cost = (mc->name[x - 1] == base[y - 1]) ? 0 : 1;
              int a = d[x - 1][y] + 1;
              int b = d[x][y - 1] + 1;
              int cost_sub = d[x - 1][y - 1] + cost;
              int min = a < b ? a : b;
              if (cost_sub < min)
                min = cost_sub;
              d[x][y] = min;
            }
          }
          int dist = d[l1][l2];
          if (dist < best_dist && dist < 4) {
            best_dist = dist;
            best_match = candidate;
          }
        }
        if (best_match) {
          fprintf(stderr, "       Did you mean '%s'?\n", best_match);
        }
        cg->had_error = 1;
        return LLVMConstInt(cg->type_i64, 0, false);
      }
      ft = sig_found->type;
      fv = sig_found->value;
      sig_arity = sig_found->arity;
      is_variadic = sig_found->is_variadic;
      has_sig = true;
      callee = fv;
    } else {
      const char *name =
          (c->callee->kind == NY_E_IDENT) ? c->callee->as.ident.name : NULL;
      if (name) {
        binding *b = scope_lookup(scopes, depth, name);
        if (b) {
          callee = LLVMBuildLoad2(cg->builder, cg->type_i64, b->value, "");
        } else {
          binding *gb = lookup_global(cg, name);
          if (gb)
            callee = LLVMBuildLoad2(cg->builder, cg->type_i64, gb->value, "");
        }
      }
      if (!callee) {
        fun_sig *sig_found =
            name ? resolve_overload(cg, name, c->args.len) : NULL;
        if (!sig_found && name)
          sig_found = lookup_use_module_fun(cg, name, c->args.len);
        if (sig_found) {
          ft = sig_found->type;
          fv = sig_found->value;
          sig_arity = sig_found->arity;
          is_variadic = sig_found->is_variadic;
          has_sig = true;
          callee = fv;
        } else {
          callee = gen_expr(cg, scopes, depth, c->callee);
        }
      }
    }
    if (!ft) {
      size_t n = c ? c->args.len : (mc->args.len + 1);
      char buf[32];
      snprintf(buf, sizeof(buf), "__call%zu", n);
      fun_sig *rsig = lookup_fun(cg, buf);
      if (!rsig) {
        fprintf(stderr, "%serror (linker): undefined symbol '%s'%s\n",
                clr(NY_CLR_RED), buf, clr(NY_CLR_RESET));
        const char *best_match = NULL;
        for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
          const char *candidate = cg->fun_sigs.data[i].name;
          if (strstr(candidate, buf) || strstr(buf, candidate)) {
            best_match = candidate;
            break;
          }
        }
        if (best_match) {
          fprintf(stderr, "  %snote:%s did you mean '%s'?\n",
                  clr(NY_CLR_YELLOW), clr(NY_CLR_RESET), best_match);
        }
        cg->had_error = 1;
        return LLVMConstInt(cg->type_i64, 0, false);
      }
      LLVMTypeRef rty = rsig->type;
      LLVMValueRef rval = rsig->value;
      LLVMValueRef callee_int =
          (LLVMTypeOf(callee) == cg->type_i64)
              ? callee
              : LLVMBuildPtrToInt(cg->builder, callee, cg->type_i64,
                                  "callee_int");
      LLVMValueRef *call_args = malloc(sizeof(LLVMValueRef) * (n + 1));
      call_args[0] = callee_int;
      if (c) {
        for (size_t i = 0; i < n; i++)
          call_args[i + 1] = gen_expr(cg, scopes, depth, c->args.data[i].val);
      } else {
        call_args[1] = gen_expr(cg, scopes, depth, mc->target);
        for (size_t i = 0; i < mc->args.len; i++)
          call_args[i + 2] = gen_expr(cg, scopes, depth, mc->args.data[i].val);
      }
      LLVMValueRef res = LLVMBuildCall2(cg->builder, rty, rval, call_args,
                                        (unsigned)n + 1, "");
      free(call_args);
      return res;
    }
    size_t call_argc =
        c ? c->args.len : (skip_target ? mc->args.len : mc->args.len + 1);
    size_t sig_argc = (has_sig && is_variadic)
                          ? (size_t)sig_arity
                          : (has_sig ? (size_t)sig_arity : call_argc);
    size_t final_argc = (sig_argc > call_argc) ? sig_argc : call_argc;
    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * final_argc);
    size_t user_args_len = c ? c->args.len : mc->args.len;
    call_arg_t *user_args = c ? c->args.data : mc->args.data;
    for (size_t i = 0; i < final_argc; i++) {
      size_t user_idx = (mc && !skip_target) ? (i - 1) : i;
      if (mc && !skip_target && i == 0) {
        args[i] = gen_expr(cg, scopes, depth, mc->target);
      } else if (has_sig && is_variadic && i == (size_t)sig_arity - 1) {
        /* Variadic packaging */
        fun_sig *ls_s = lookup_fun(cg, "list");
        if (!ls_s)
          ls_s = lookup_fun(cg, "std.core.list");
        fun_sig *as_s = lookup_fun(cg, "append");
        if (!as_s)
          as_s = lookup_fun(cg, "std.core.append");
        if (!ls_s || !as_s) {
          fprintf(stderr,
                  "Error: variadic arguments require 'list' and "
                  "'append' functions to be defined (missing std.core?)\n");
          cg->had_error = 1;
          return LLVMConstInt(cg->type_i64, 0, false);
        }
        LLVMTypeRef lty = ls_s->type, aty = as_s->type;
        LLVMValueRef lval = ls_s->value, aval = as_s->value;
        LLVMValueRef vl = LLVMBuildCall2(
            cg->builder, lty, lval,
            (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 35, false)}, 1, "");
        for (size_t j = user_idx; j < user_args_len; j++) {
          call_arg_t *a = &user_args[j];
          LLVMValueRef av = gen_expr(cg, scopes, depth, a->val);
          if (a->name) {
            fun_sig *ks_s = lookup_fun(cg, "__kwarg");
            if (!ks_s) {
              fprintf(stderr, "Error: keyword args require '__kwarg'\n");
              cg->had_error = 1;
              return LLVMConstInt(cg->type_i64, 0, false);
            }
            LLVMTypeRef kty = ks_s->type;
            LLVMValueRef kval = ks_s->value;
            LLVMValueRef name_runtime_global =
                const_string_ptr(cg, a->name, strlen(a->name));
            LLVMValueRef name_ptr = LLVMBuildLoad2(cg->builder, cg->type_i64,
                                                   name_runtime_global, "");
            av = LLVMBuildCall2(cg->builder, kty, kval,
                                (LLVMValueRef[]){name_ptr, av}, 2, "");
          }
          vl = LLVMBuildCall2(cg->builder, aty, aval, (LLVMValueRef[]){vl, av},
                              2, "");
        }
        args[i] = vl;
        break;
      } else if (user_idx < user_args_len) {
        args[i] = gen_expr(cg, scopes, depth, user_args[user_idx].val);
      } else if (has_sig && sig_arity > (int)i &&
                 i < user_args_len) { // fallback
        args[i] = LLVMConstInt(cg->type_i64, 0, false);
      } else {
        args[i] = LLVMConstInt(cg->type_i64, 0, false);
      }
    }
    if (has_sig) {
      /* const char *callee_name = (c && c->callee->kind == NY_E_IDENT) ?
       * c->callee->as.ident.name : (mc ? mc->name : "ptr"); */
      /* fprintf(stderr, "DEBUG: Call gen '%s' - is_variadic: %d, sig_arity: %d,
       * call_argc: %zu\n", callee_name, is_variadic, sig_arity, c ? c->args.len
       * : mc->args.len); */
    }
    LLVMValueRef res = LLVMBuildCall2(
        cg->builder, ft, callee, args,
        (unsigned)(has_sig && is_variadic ? (size_t)sig_arity : final_argc),
        "");
    free(args);
    return res;
  }
  case NY_E_INDEX: {
    if (e->as.index.stop || e->as.index.step || !e->as.index.start) {
      fun_sig *s = lookup_fun(cg, "slice");
      if (!s) {
        fprintf(stderr, "Error: slice requires 'slice'\n");
        cg->had_error = 1;
        return LLVMConstInt(cg->type_i64, 0, false);
      }
      LLVMValueRef start =
          e->as.index.start ? gen_expr(cg, scopes, depth, e->as.index.start)
                            : LLVMConstInt(cg->type_i64, 1, false); // 0 tagged
      LLVMValueRef stop =
          e->as.index.stop
              ? gen_expr(cg, scopes, depth, e->as.index.stop)
              : LLVMConstInt(cg->type_i64, ((0x3fffffffULL) << 1) | 1, false);
      LLVMValueRef step =
          e->as.index.step ? gen_expr(cg, scopes, depth, e->as.index.step)
                           : LLVMConstInt(cg->type_i64, 3, false); // 1 tagged
      return LLVMBuildCall2(
          cg->builder, s->type, s->value,
          (LLVMValueRef[]){gen_expr(cg, scopes, depth, e->as.index.target),
                           start, stop, step},
          4, "");
    }
    fun_sig *s = lookup_fun(cg, "get");
    if (!s)
      s = lookup_fun(cg, "std.core.get");
    if (!s) {
      fprintf(stderr, "Error: index requires 'get'\n");
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    return LLVMBuildCall2(
        cg->builder, s->type, s->value,
        (LLVMValueRef[]){gen_expr(cg, scopes, depth, e->as.index.target),
                         gen_expr(cg, scopes, depth, e->as.index.start)},
        2, "");
  }
  case NY_E_LIST:
  case NY_E_TUPLE: {
    fun_sig *ls = lookup_fun(cg, "list");
    if (!ls)
      ls = lookup_fun(cg, "std.core.list");
    fun_sig *as = lookup_fun(cg, "append");
    if (!as)
      as = lookup_fun(cg, "std.core.append");
    if (!ls || !as) {
      fprintf(stderr, "Error: list requires list/append (searched 'list', "
                      "'std.core.list', 'append', 'std.core.append')\n");
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 0, false);
    }
    LLVMValueRef vl = LLVMBuildCall2(
        cg->builder, ls->type, ls->value,
        (LLVMValueRef[]){LLVMConstInt(
            cg->type_i64, ((uint64_t)e->as.list_like.len << 1) | 1, false)},
        1, "");
    for (size_t i = 0; i < e->as.list_like.len; i++)
      vl = LLVMBuildCall2(
          cg->builder, as->type, as->value,
          (LLVMValueRef[]){
              vl, gen_expr(cg, scopes, depth, e->as.list_like.data[i])},
          2, "");
    return vl;
  }
  case NY_E_DICT: {
    fun_sig *ds = lookup_fun(cg, "dict");
    if (!ds)
      ds = lookup_fun(cg, "std.collections.dict.dict");
    fun_sig *ss = lookup_fun(cg, "dict_set");
    if (!ss)
      ss = lookup_fun(cg, "std.collections.dict.dict_set");
    if (!ds || !ss) {
      fprintf(stderr, "Error: dict requires dict/dict_set (searched 'dict', "
                      "'std.collections.dict.dict', 'dict_set', "
                      "'std.collections.dict.dict_set')\n");
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 0, false);
    }
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
  case NY_E_LOGICAL: {
    bool and = strcmp(e->as.logical.op, "&&") == 0;
    LLVMValueRef left =
        to_bool(cg, gen_expr(cg, scopes, depth, e->as.logical.left));
    LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
    LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlock(f, "lrhs"),
                      end_bb = LLVMAppendBasicBlock(f, "lend");
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
  case NY_E_TERNARY: {
    LLVMValueRef cond =
        to_bool(cg, gen_expr(cg, scopes, depth, e->as.ternary.cond));
    LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
    LLVMBasicBlockRef true_bb = LLVMAppendBasicBlock(f, "tern_true");
    LLVMBasicBlockRef false_bb = LLVMAppendBasicBlock(f, "tern_false");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(f, "tern_end");
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
      fstring_pa__t p = e->as.fstring.parts.data[i];
      LLVMValueRef pv;
      if (p.kind == NY_FSP_STR) {
        LLVMValueRef pa__runtime_global =
            const_string_ptr(cg, p.as.s.data, p.as.s.len);
        pv = LLVMBuildLoad2(cg->builder, cg->type_i64, pa__runtime_global, "");
      } else {
        pv = LLVMBuildCall2(
            cg->builder, ts->type, ts->value,
            (LLVMValueRef[]){gen_expr(cg, scopes, depth, p.as.e)}, 1, "");
      }
      res = LLVMBuildCall2(cg->builder, cs->type, cs->value,
                           (LLVMValueRef[]){res, pv}, 2, "");
    }
    return res;
  }
  case NY_E_LAMBDA:
  case NY_E_FN: {
    /* Capture All Visible Variables (scopes[1..depth]) */
    binding_list captures = {0};
    for (ssize_t i = 1; i <= (ssize_t)depth; i++) {
      for (size_t j = 0; j < scopes[i].vars.len; j++) {
        vec_push(&captures, scopes[i].vars.data[j]);
      }
    }
    char name[64];
    snprintf(name, sizeof(name), "__lambda_%d", cg->lambda_count++);
    stmt_t sfn = {.kind = NY_S_FUNC,
                  .as.fn = {.name = strdup(name),
                            .params = e->as.lambda.params,
                            .body = e->as.lambda.body,
                            .is_variadic = e->as.lambda.is_variadic}};
    scope sc[64] = {0};
    gen_func(cg, &sfn, name, sc, 0, &captures);
    free((void *)sfn.as.fn.name);
    LLVMValueRef lf = LLVMGetNamedFunction(cg->module, name);
    LLVMValueRef fn_ptr_tagged = LLVMBuildOr(
        cg->builder, LLVMBuildPtrToInt(cg->builder, lf, cg->type_i64, ""),
        LLVMConstInt(cg->type_i64, 2, false), "");
    if (captures.len == 0 && e->kind != NY_E_LAMBDA) {
      /* Standard function (tag 2) if no captures */
      vec_free(&captures);
      return fn_ptr_tagged;
    }
    /* Create Env */
    fun_sig *malloc_sig = lookup_fun(cg, "__malloc");
    if (!malloc_sig) {
      fprintf(stderr, "Error: __malloc required for closures\n");
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 0, false);
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
                         LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0),
                         ""),
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
  case NY_E_MATCH: {
    LLVMValueRef old_store = cg->result_store_val;
    LLVMValueRef slot = build_alloca(cg, "match_res");
    LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 1, false), slot);
    cg->result_store_val = slot;
    stmt_t fake = {.kind = NY_S_MATCH, .as.match = e->as.match, .tok = e->tok};
    size_t d = depth;
    gen_stmt(cg, scopes, &d, &fake, cg->func_root_idx, true);
    cg->result_store_val = old_store;
    return LLVMBuildLoad2(cg->builder, cg->type_i64, slot, "");
  }
  default: {
    const char *fname = e->tok.filename ? e->tok.filename : "<input>";
    fprintf(stderr,
            "Error: unsupported expr_t kind %d token_kind=%d at %s:%d "
            "token_t='%.*s'\n",
            e->kind, e->tok.kind, fname, e->tok.line, (int)e->tok.len,
            e->tok.lexeme ? e->tok.lexeme : "");
    cg->had_error = 1;
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  }
}
