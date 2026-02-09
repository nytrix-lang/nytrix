#include "base/util.h"
#include "priv.h"
#include "rt/shared.h"
#include <alloca.h>
#include <ctype.h>
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Use static functions for local helpers */
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

static bool abi_type_is_tagged(const char *type_name) {
  if (!type_name || !*type_name)
    return true;
  return strcmp(type_name, "int") == 0 || strcmp(type_name, "str") == 0 ||
         strcmp(type_name, "bool") == 0 || strcmp(type_name, "Result") == 0;
}

static bool abi_type_is_ptr(const char *type_name) {
  return type_name && type_name[0] == '*';
}

static bool abi_type_is_float(const char *type_name) {
  return type_name && (strcmp(type_name, "f32") == 0 ||
                       strcmp(type_name, "f64") == 0 ||
                       strcmp(type_name, "f128") == 0);
}

static bool abi_type_is_signed_int(const char *type_name) {
  return type_name && (strcmp(type_name, "i8") == 0 ||
                       strcmp(type_name, "i16") == 0 ||
                       strcmp(type_name, "i32") == 0 ||
                       strcmp(type_name, "i64") == 0 ||
                       strcmp(type_name, "i128") == 0 ||
                       strcmp(type_name, "char") == 0);
}

static bool abi_type_is_unsigned_int(const char *type_name) {
  return type_name && (strcmp(type_name, "u8") == 0 ||
                       strcmp(type_name, "u16") == 0 ||
                       strcmp(type_name, "u32") == 0 ||
                       strcmp(type_name, "u64") == 0 ||
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
  LLVMValueRef shift =
      LLVMConstInt(cg->type_i64, 1, false);
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
    fun_sig *unbox = lookup_fun(cg, "__flt_unbox_val");
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
    fun_sig *box = lookup_fun(cg, "__flt_box_val");
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
  if (abi_type_is_signed_int(type_name) || abi_type_is_unsigned_int(type_name)) {
    bool signed_int = abi_type_is_signed_int(type_name);
    LLVMTypeRef target = cg->type_i64;
    LLVMValueRef widened =
        abi_cast_int(cg, v, target, signed_int);
    LLVMValueRef sh = LLVMBuildShl(
        cg->builder, widened, LLVMConstInt(cg->type_i64, 1, false), "");
    return LLVMBuildOr(cg->builder, sh, LLVMConstInt(cg->type_i64, 1, false),
                       "");
  }
  return v;
}

static void add_extern_sig(codegen_t *cg, const char *name, int arity) {
  if (!cg || !name || !*name || arity < 0)
    return;
  for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
    if (strcmp(cg->fun_sigs.data[i].name, name) == 0)
      return;
  }
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
                 .link_name = ny_strdup(name)};
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
  if (!arg || arg->kind != NY_E_LITERAL ||
      arg->as.literal.kind != NY_LIT_STR)
    return false;
  *out = arg->as.literal.as.s.data;
  return true;
}

static LLVMValueRef emit_layout_query(codegen_t *cg, token_t tok,
                                      const char *layout_name,
                                      const char *field_name,
                                      bool want_align, bool want_offset) {
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
        ny_diag_fix("call '%s' with at least %zu argument(s)",
                    sig_found->name, min_arity);
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
  expr_call_t *c = (e->kind == NY_E_CALL) ? &e->as.call : NULL;
  expr_memcall_t *mc = (e->kind == NY_E_MEMCALL) ? &e->as.memcall : NULL;
  LLVMValueRef callee = NULL;
  LLVMTypeRef ft = NULL;
  LLVMValueRef fv = NULL;
  bool is_variadic = false;
  int sig_arity = 0;
  bool has_sig = false;
  bool skip_target = false;
  fun_sig *sig_found = NULL;

  if (c && c->callee->kind == NY_E_IDENT) {
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
        looked_like_module_target = true;
        resolved_module_name = module_name;
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
  static_call_handling:;
    if (!sig_found) {
      /* Fallback: try dynamic property lookup (e.g. obj.method -> get(obj,
       * "method")) */
      fun_sig *getter = lookup_fun(cg, "get");
      if (!getter)
        getter = lookup_fun(cg, "std.core.get");
      if (!getter)
        getter = lookup_fun(cg, "std.core.reflect.get");
      if (!getter)
        getter = lookup_fun(cg, "dict_get");
      if (getter && strcmp(mc->name, "get") != 0) {
        LLVMValueRef target_val = gen_expr(cg, scopes, depth, mc->target);
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
    const char *name =
        (c->callee->kind == NY_E_IDENT) ? c->callee->as.ident.name : NULL;
    if (name) {
      binding *b = scope_lookup(scopes, depth, name);
      if (b) {
        b->is_used = true;
        callee = LLVMBuildLoad2(cg->builder, cg->type_i64, b->value, "");
      } else {
        binding *gb = lookup_global(cg, name);
        if (gb) {
          gb->is_used = true;
          callee = LLVMBuildLoad2(cg->builder, cg->type_i64, gb->value, "");
        }
      }
    }
    if (!callee) {
      sig_found = name ? resolve_overload(cg, name, c->args.len) : NULL;
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
        if (name) {
          fun_sig *globals_sig = lookup_fun(cg, "__globals");
          fun_sig *getter = lookup_fun(cg, "std.core.reflect.get");
          if (!getter)
            getter = lookup_fun(cg, "std.core.get");
          if (!getter)
            getter = lookup_fun(cg, "get");
          if (!getter)
            getter = lookup_fun(cg, "dict_get");
          if (globals_sig && getter) {
            ny_dbg_loc(cg, e->tok);
            LLVMValueRef gtbl = LLVMBuildCall2(
                cg->builder, globals_sig->type, globals_sig->value, NULL, 0,
                "");
            LLVMValueRef name_global =
                const_string_ptr(cg, name, strlen(name));
            LLVMValueRef name_ptr = LLVMBuildLoad2(
                cg->builder, cg->type_i64, name_global, "");
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
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef res =
        LLVMBuildCall2(cg->builder, rty, rval, call_args, (unsigned)n + 1, "");
    free(call_args);
    return res;
  }
  size_t call_argc =
      c ? c->args.len : (skip_target ? mc->args.len : mc->args.len + 1);

  if (has_sig) {
    if (!check_call_arity_diag(cg, e->tok, sig_found, is_variadic, sig_arity,
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
  if (has_sig && sig_found && sig_found->stmt_t) {
    if (sig_found->stmt_t->kind == NY_S_FUNC)
      func_params = &sig_found->stmt_t->as.fn.params;
    else if (sig_found->stmt_t->kind == NY_S_EXTERN)
      func_params = &sig_found->stmt_t->as.ext.params;
  }
  for (size_t i = 0; i < final_argc; i++) {
    size_t user_idx = (mc && !skip_target) ? (i - 1) : i;
    const char *param_type =
        (func_params && i < func_params->len) ? func_params->data[i].type : NULL;
    expr_t *expr_for_check = NULL;
    if (mc && !skip_target && i == 0) {
      expr_for_check = mc->target;
      if (param_type && expr_for_check)
        ensure_expr_type_compatible(cg, scopes, depth, param_type,
                                     expr_for_check, expr_for_check->tok,
                                     "argument");
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
        ny_diag_error(e->tok, "variadic arguments require list/append helpers");
        ny_diag_hint("missing std.core imports for 'list'/'append'");
        cg->had_error = 1;
        goto call_fail;
      }
      LLVMTypeRef lty = ls_s->type, aty = as_s->type;
      LLVMValueRef lval = ls_s->value, aval = as_s->value;
      ny_dbg_loc(cg, e->tok);
      LLVMValueRef vl = LLVMBuildCall2(
          cg->builder, lty, lval,
          (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 35, false)}, 1, "");
      for (size_t j = user_idx; j < user_args_len; j++) {
        call_arg_t *a = &user_args[j];
        LLVMValueRef av = gen_expr(cg, scopes, depth, a->val);
        if (a->name) {
          fun_sig *ks_s = lookup_fun(cg, "__kwarg");
          if (!ks_s)
            ks_s = lookup_fun(cg, "std.core.__kwarg");
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
        ny_dbg_loc(cg, e->tok);
        vl = LLVMBuildCall2(cg->builder, aty, aval, (LLVMValueRef[]){vl, av}, 2,
                            "");
      }
      args[i] = vl;
      break;
    } else if (user_idx < user_args_len) {
      expr_for_check = user_args[user_idx].val;
      if (param_type && expr_for_check)
        ensure_expr_type_compatible(cg, scopes, depth, param_type,
                                     expr_for_check, expr_for_check->tok,
                                     "argument");
      args[i] = gen_expr(cg, scopes, depth, user_args[user_idx].val);
    } else if (has_sig && sig_arity > (int)i && i < user_args_len) { // fallback
      args[i] = LLVMConstInt(cg->type_i64, 0, false); // none
    } else {
      default_expr = NULL;
      if (has_sig && sig_found && sig_found->stmt_t &&
          sig_found->stmt_t->kind == NY_S_FUNC) {
        func_params = &sig_found->stmt_t->as.fn.params;
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
  if (has_sig && sig_found && sig_found->is_extern) {
    func_params = NULL;
    if (sig_found->stmt_t && sig_found->stmt_t->kind == NY_S_EXTERN) {
      func_params = &sig_found->stmt_t->as.ext.params;
    }
    if (func_params && func_params->len > 0) {
      size_t max_conv = func_params->len;
      size_t call_limit =
          (has_sig && is_variadic) ? (size_t)sig_arity : final_argc;
      if (max_conv > call_limit)
        max_conv = call_limit;
      for (size_t i = 0; i < max_conv; i++) {
        if (sig_found->is_variadic && (int)i >= sig_arity - 1)
          break;
        const char *tname = func_params->data[i].type;
        if (tname && *tname) {
          args[i] = coerce_extern_arg(cg, args[i], tname);
        }
      }
    }
  }
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef res = LLVMBuildCall2(
      cg->builder, ft, callee, args,
      (unsigned)(has_sig && is_variadic ? (size_t)sig_arity : final_argc), "");
  free(args);
  if (has_sig && sig_found && sig_found->is_extern) {
    LLVMTypeRef ret_ty = LLVMGetReturnType(ft);
    if (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind)
      return LLVMConstInt(cg->type_i64, 0, false);
    return box_extern_result(cg, res, sig_found->return_type);
  }
  return res;

call_fail:
  free(args);
  return LLVMConstInt(cg->type_i64, 0, false);
}
