#include "base/util.h"
#include "priv.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef enum ny_builtin_type_kind_t {
  NY_BT_UNKNOWN = 0,
  NY_BT_INT,
  NY_BT_STR,
  NY_BT_BOOL,
  NY_BT_VOID,
  NY_BT_CHAR,
  NY_BT_I8,
  NY_BT_I16,
  NY_BT_I32,
  NY_BT_I64,
  NY_BT_I128,
  NY_BT_U8,
  NY_BT_U16,
  NY_BT_U32,
  NY_BT_U64,
  NY_BT_U128,
  NY_BT_F32,
  NY_BT_F64,
  NY_BT_F128,
  NY_BT_RESULT,
} ny_builtin_type_kind_t;

static const char *type_skip_nullable(const char *name) {
  if (!name)
    return NULL;
  while (*name == '?')
    name++;
  return name;
}

static ny_builtin_type_kind_t classify_builtin_type_exact(const char *name) {
  if (!name || !*name)
    return NY_BT_UNKNOWN;

  size_t len = strlen(name);
  switch (len) {
  case 2:
    if (name[0] == 'i' && name[1] == '8')
      return NY_BT_I8;
    if (name[0] == 'u' && name[1] == '8')
      return NY_BT_U8;
    break;
  case 3:
    if (name[0] == 'i') {
      if (name[1] == 'n' && name[2] == 't')
        return NY_BT_INT;
      if (name[1] == '1' && name[2] == '6')
        return NY_BT_I16;
      if (name[1] == '3' && name[2] == '2')
        return NY_BT_I32;
      if (name[1] == '6' && name[2] == '4')
        return NY_BT_I64;
    } else if (name[0] == 'u') {
      if (name[1] == '1' && name[2] == '6')
        return NY_BT_U16;
      if (name[1] == '3' && name[2] == '2')
        return NY_BT_U32;
      if (name[1] == '6' && name[2] == '4')
        return NY_BT_U64;
    } else if (name[0] == 'f') {
      if (name[1] == '3' && name[2] == '2')
        return NY_BT_F32;
      if (name[1] == '6' && name[2] == '4')
        return NY_BT_F64;
    } else if (name[0] == 's' && name[1] == 't' && name[2] == 'r') {
      return NY_BT_STR;
    }
    break;
  case 4:
    if (memcmp(name, "bool", 4) == 0)
      return NY_BT_BOOL;
    if (memcmp(name, "void", 4) == 0)
      return NY_BT_VOID;
    if (memcmp(name, "char", 4) == 0)
      return NY_BT_CHAR;
    if (name[0] == 'i' && memcmp(name, "i128", 4) == 0)
      return NY_BT_I128;
    if (name[0] == 'u' && memcmp(name, "u128", 4) == 0)
      return NY_BT_U128;
    if (name[0] == 'f' && memcmp(name, "f128", 4) == 0)
      return NY_BT_F128;
    break;
  case 6:
    if (memcmp(name, "Result", 6) == 0)
      return NY_BT_RESULT;
    break;
  default:
    break;
  }
  return NY_BT_UNKNOWN;
}

static ny_builtin_type_kind_t classify_builtin_type_tail(const char *name) {
  if (!name || !*name)
    return NY_BT_UNKNOWN;
  name = type_skip_nullable(name);
  const char *t = strrchr(name, '.');
  return classify_builtin_type_exact(t ? t + 1 : name);
}

static const char *type_tail(const char *name) {
  if (!name)
    return NULL;
  const char *dot = strrchr(name, '.');
  return dot ? dot + 1 : name;
}

static bool type_name_eq(const char *a, const char *b) {
  if (!a || !b)
    return false;
  if (strcmp(a, b) == 0)
    return true;
  const char *ta = type_tail(a);
  const char *tb = type_tail(b);
  return ta && tb && strcmp(ta, tb) == 0;
}

static bool is_nullable_type_name(const char *name) {
  return name && name[0] == '?';
}

static bool is_ptr_type_name(const char *name) {
  name = type_skip_nullable(name);
  return name && name[0] == '*';
}

static bool is_float_type_name(const char *name) {
  ny_builtin_type_kind_t k = classify_builtin_type_tail(name);
  return k == NY_BT_F32 || k == NY_BT_F64 || k == NY_BT_F128;
}

static bool is_int_type_name(const char *name) {
  ny_builtin_type_kind_t k = classify_builtin_type_tail(name);
  switch (k) {
  case NY_BT_INT:
  case NY_BT_CHAR:
  case NY_BT_I8:
  case NY_BT_I16:
  case NY_BT_I32:
  case NY_BT_I64:
  case NY_BT_I128:
  case NY_BT_U8:
  case NY_BT_U16:
  case NY_BT_U32:
  case NY_BT_U64:
  case NY_BT_U128:
    return true;
  default:
    return false;
  }
}

static bool is_bool_type_name(const char *name) {
  return classify_builtin_type_tail(name) == NY_BT_BOOL;
}

static bool is_str_type_name(const char *name) {
  return classify_builtin_type_tail(name) == NY_BT_STR;
}

static bool is_char_type_name(const char *name) {
  return classify_builtin_type_tail(name) == NY_BT_CHAR;
}

static bool is_result_type_name(const char *name) {
  return classify_builtin_type_tail(name) == NY_BT_RESULT;
}

static bool is_void_type_name(const char *name) {
  return classify_builtin_type_tail(name) == NY_BT_VOID;
}

static bool is_nil_type_name(const char *name) {
  return type_name_eq(name, "nil") || type_name_eq(name, "none");
}

static bool nil_assignable_to_type(const char *want) {
  if (!want || !*want)
    return true;
  if (is_nullable_type_name(want))
    return true;
  if (is_ptr_type_name(want))
    return true;
  return is_nil_type_name(want);
}

static bool report_nil_type_mismatch(codegen_t *cg, token_t tok,
                                     const char *want, const char *ctx) {
  ny_diag_error(tok, "cannot use nil for %s %s", type_tail(want),
                ctx ? ctx : "context");
  ny_diag_hint("typed values are non-null by default; use a nullable type (?T) "
               "or pointer type (*T) if nil is intended");
  cg->had_error = 1;
  return false;
}

static bool literal_int_fits(int64_t val, const char *want) {
  ny_builtin_type_kind_t k = classify_builtin_type_tail(want);
  switch (k) {
  case NY_BT_UNKNOWN:
  case NY_BT_INT:
  case NY_BT_I64:
  case NY_BT_I128:
    return true;
  case NY_BT_I8:
    return val >= INT8_MIN && val <= INT8_MAX;
  case NY_BT_I16:
    return val >= INT16_MIN && val <= INT16_MAX;
  case NY_BT_I32:
    return val >= INT32_MIN && val <= INT32_MAX;
  case NY_BT_U8:
    return val >= 0 && val <= UINT8_MAX;
  case NY_BT_U16:
    return val >= 0 && val <= UINT16_MAX;
  case NY_BT_U32:
    return val >= 0 && val <= UINT32_MAX;
  case NY_BT_U64:
  case NY_BT_U128:
    return val >= 0;
  default:
    return true;
  }
}

static bool type_compatible_non_nullable(const char *want, const char *got) {
  if (!want || !got)
    return true;
  if (type_name_eq(want, got))
    return true;
  if (is_result_type_name(want) && is_result_type_name(got))
    return true;
  if (is_ptr_type_name(want) && is_ptr_type_name(got))
    return true;
  if (is_int_type_name(want) && is_int_type_name(got)) {
    if (type_name_eq(want, "int") || type_name_eq(got, "int"))
      return true;
  }
  if (is_float_type_name(want) && is_float_type_name(got))
    return true;
  if (is_float_type_name(want) && is_int_type_name(got))
    return true;
  return false;
}

static bool type_compatible_simple(const char *want, const char *got) {
  if (!want || !got)
    return true;

  bool want_nullable = is_nullable_type_name(want);
  bool got_nullable = is_nullable_type_name(got);
  const char *want_base = type_skip_nullable(want);
  const char *got_base = type_skip_nullable(got);

  // Prevent implicit narrowing from nullable -> non-null.
  if (!want_nullable && got_nullable)
    return false;

  return type_compatible_non_nullable(want_base, got_base);
}

const char *infer_expr_type(codegen_t *cg, scope *scopes, size_t depth,
                            expr_t *e) {
  if (!e)
    return NULL;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_BOOL)
      return "bool";
    if (e->as.literal.kind == NY_LIT_STR)
      return "str";
    if (e->as.literal.kind == NY_LIT_FLOAT) {
      switch (e->as.literal.hint) {
      case NY_LIT_HINT_F32:
        return "f32";
      case NY_LIT_HINT_F128:
        return "f128";
      case NY_LIT_HINT_F64:
      default:
        return "f64";
      }
    }
    if (e->as.literal.kind == NY_LIT_INT) {
      if (e->tok.kind == NY_T_NIL)
        return "nil";
      switch (e->as.literal.hint) {
      case NY_LIT_HINT_I8:
        return "i8";
      case NY_LIT_HINT_I16:
        return "i16";
      case NY_LIT_HINT_I32:
        return "i32";
      case NY_LIT_HINT_I64:
        return "i64";
      case NY_LIT_HINT_U8:
        return "u8";
      case NY_LIT_HINT_U16:
        return "u16";
      case NY_LIT_HINT_U32:
        return "u32";
      case NY_LIT_HINT_U64:
        return "u64";
      default:
        return "int";
      }
    }
    return NULL;
  case NY_E_IDENT: {
    if (scopes) {
      binding *b = scope_lookup(scopes, depth, e->as.ident.name);
      if (b && b->type_name)
        return b->type_name;
    }
    binding *gb = lookup_global(cg, e->as.ident.name);
    if (gb && gb->type_name)
      return gb->type_name;
    fun_sig *sig = lookup_fun(cg, e->as.ident.name);
    if (sig && sig->return_type)
      return sig->return_type;
    return NULL;
  }
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const char *n = e->as.call.callee->as.ident.name;
      fun_sig *sig = lookup_fun(cg, n);
      if (sig && sig->return_type)
        return sig->return_type;
    }
    return NULL;
  case NY_E_MEMCALL:
    if (e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT) {
      char dotted[256];
      const char *target = e->as.memcall.target->as.ident.name;
      snprintf(dotted, sizeof(dotted), "%s.%s", target, e->as.memcall.name);
      fun_sig *sig = lookup_fun(cg, dotted);
      if (sig && sig->return_type)
        return sig->return_type;
    }
    return NULL;
  case NY_E_LAMBDA:
  case NY_E_FN:
    return e->as.lambda.return_type;
  default:
    return NULL;
  }
}

bool ensure_expr_type_compatible(codegen_t *cg, scope *scopes, size_t depth,
                                 const char *want, expr_t *expr, token_t tok,
                                 const char *ctx) {
  if (!cg || !want || !*want || !expr)
    return true;
  if (is_void_type_name(want)) {
    if (expr->kind == NY_E_LITERAL && expr->as.literal.kind == NY_LIT_INT) {
      if (expr->tok.kind == NY_T_NIL || expr->as.literal.as.i == 0)
        return true;
    }
    ny_diag_error(tok, "void %s cannot return a value", ctx ? ctx : "context");
    cg->had_error = 1;
    return false;
  }
  if (expr->kind == NY_E_LITERAL) {
    if (expr->as.literal.kind == NY_LIT_INT) {
      if (expr->tok.kind == NY_T_NIL) {
        if (nil_assignable_to_type(want))
          return true;
        return report_nil_type_mismatch(cg, tok, want, ctx);
      }
      int64_t val = expr->as.literal.as.i;
      if (is_int_type_name(want)) {
        if (is_char_type_name(want)) {
          if (val >= 0 && val <= 255)
            return true;
          ny_diag_error(tok, "char literal out of range");
          cg->had_error = 1;
          return false;
        }
        if (!literal_int_fits(val, want)) {
          ny_diag_error(tok, "integer literal does not fit %s", want);
          cg->had_error = 1;
          return false;
        }
        return true;
      }
      if (is_float_type_name(want))
        return true;
      if (is_bool_type_name(want) || is_str_type_name(want) ||
          is_result_type_name(want)) {
        ny_diag_error(tok, "cannot assign integer literal to %s", want);
        cg->had_error = 1;
        return false;
      }
    } else if (expr->as.literal.kind == NY_LIT_FLOAT) {
      if (is_float_type_name(want))
        return true;
      ny_diag_error(tok, "cannot assign float literal to %s", want);
      cg->had_error = 1;
      return false;
    } else if (expr->as.literal.kind == NY_LIT_BOOL) {
      if (is_bool_type_name(want))
        return true;
      ny_diag_error(tok, "cannot assign bool literal to %s", want);
      cg->had_error = 1;
      return false;
    } else if (expr->as.literal.kind == NY_LIT_STR) {
      if (is_str_type_name(want))
        return true;
      if (is_char_type_name(want)) {
        if (expr->as.literal.as.s.len == 1)
          return true;
        ny_diag_error(tok, "char literal must be a single character");
        cg->had_error = 1;
        return false;
      }
      ny_diag_error(tok, "cannot assign string literal to %s", want);
      cg->had_error = 1;
      return false;
    }
  }

  const char *got = infer_expr_type(cg, scopes, depth, expr);
  if (!got)
    return true;
  if (is_nil_type_name(got)) {
    if (nil_assignable_to_type(want))
      return true;
    return report_nil_type_mismatch(cg, tok, want, ctx);
  }
  if (!type_compatible_simple(want, got)) {
    ny_diag_error(tok, "type mismatch: expected %s, got %s", type_tail(want),
                  type_tail(got));
    cg->had_error = 1;
    return false;
  }
  return true;
}

LLVMTypeRef resolve_type_name(codegen_t *cg, const char *type_name,
                              token_t tok) {
  if (!type_name || !*type_name) {
    return cg->type_i64; // Default to i64 if no type specified
  }
  const char *resolved_name = type_skip_nullable(type_name);

  if (resolved_name[0] == '*') {
    // For now, all pointers are i64 (representing memory addresses)
    // A more sophisticated type system would resolve the base type as well.
    return cg->type_i64;
  }

  switch (classify_builtin_type_exact(resolved_name)) {
  case NY_BT_INT:
  case NY_BT_STR:
  case NY_BT_BOOL:
  case NY_BT_VOID:
  case NY_BT_CHAR:
  case NY_BT_I8:
  case NY_BT_I16:
  case NY_BT_I32:
  case NY_BT_I64:
  case NY_BT_I128:
  case NY_BT_U8:
  case NY_BT_U16:
  case NY_BT_U32:
  case NY_BT_U64:
  case NY_BT_U128:
  case NY_BT_F32:
  case NY_BT_F64:
  case NY_BT_F128:
  case NY_BT_RESULT:
    return cg->type_i64;
  default:
    break;
  }

  // User-defined types (structs/layouts) usually decay to pointer (i64)
  // or use their specific type if needed.
  layout_def_t *layout = lookup_layout(cg, resolved_name);
  if (layout) {
    if (layout->llvm_type)
      return layout->llvm_type;
    return cg->type_i64;
  }

  ny_diag_error(tok, "unknown type name '%s'", type_name);
  cg->had_error = 1;
  return cg->type_i64;
}

LLVMTypeRef resolve_abi_type_name(codegen_t *cg, const char *type_name,
                                  token_t tok) {
  if (!type_name || !*type_name) {
    return cg->type_i64;
  }
  const char *resolved_name = type_skip_nullable(type_name);

  if (resolved_name[0] == '*') {
    return cg->type_i8ptr;
  }

  switch (classify_builtin_type_exact(resolved_name)) {
  case NY_BT_INT:
  case NY_BT_STR:
  case NY_BT_BOOL:
  case NY_BT_RESULT:
    return cg->type_i64;
  case NY_BT_VOID:
    return LLVMVoidTypeInContext(cg->ctx);
  case NY_BT_CHAR:
  case NY_BT_I8:
  case NY_BT_U8:
    return cg->type_i8;
  case NY_BT_I16:
  case NY_BT_U16:
    return cg->type_i16;
  case NY_BT_I32:
  case NY_BT_U32:
    return cg->type_i32;
  case NY_BT_I64:
  case NY_BT_U64:
    return cg->type_i64;
  case NY_BT_I128:
  case NY_BT_U128:
    return cg->type_i128;
  case NY_BT_F32:
    return cg->type_f32;
  case NY_BT_F64:
    return cg->type_f64;
  case NY_BT_F128:
    return cg->type_f128;
  default:
    break;
  }

  ny_diag_error(tok, "unknown type name '%s' in extern ABI", type_name);
  cg->had_error = 1;
  return cg->type_i64;
}

static type_layout_t make_layout(size_t size, size_t align,
                                 LLVMTypeRef llvm_type) {
  type_layout_t layout;
  layout.size = size;
  layout.align = align;
  layout.llvm_type = llvm_type;
  layout.is_valid = true;
  return layout;
}

static size_t ptr_size(void) { return sizeof(void *); }

layout_def_t *lookup_layout(codegen_t *cg, const char *name) {
  if (!cg || !name)
    return NULL;
  for (size_t i = 0; i < cg->layouts.len; i++) {
    layout_def_t *def = cg->layouts.data[i];
    if (def && def->name && strcmp(def->name, name) == 0)
      return def;
  }
  if (cg->comptime && cg->parent)
    return lookup_layout(cg->parent, name);
  return NULL;
}

type_layout_t resolve_raw_layout(codegen_t *cg, const char *type_name,
                                 token_t tok) {
  type_layout_t invalid = {0, 0, NULL, false};
  if (!type_name || !*type_name)
    return invalid;
  const char *resolved_name = type_skip_nullable(type_name);

  if (resolved_name[0] == '*') {
    return make_layout(ptr_size(), ptr_size(), cg->type_i8ptr);
  }

  switch (classify_builtin_type_exact(resolved_name)) {
  case NY_BT_INT:
  case NY_BT_RESULT:
    return make_layout(8, 8, cg->type_i64);
  case NY_BT_STR:
    return make_layout(ptr_size(), ptr_size(), cg->type_i8ptr);
  case NY_BT_BOOL:
    return make_layout(1, 1, cg->type_bool);
  case NY_BT_VOID:
    return make_layout(0, 1, LLVMVoidTypeInContext(cg->ctx));
  case NY_BT_CHAR:
  case NY_BT_I8:
  case NY_BT_U8:
    return make_layout(1, 1, cg->type_i8);
  case NY_BT_I16:
  case NY_BT_U16:
    return make_layout(2, 2, cg->type_i16);
  case NY_BT_I32:
  case NY_BT_U32:
    return make_layout(4, 4, cg->type_i32);
  case NY_BT_I64:
  case NY_BT_U64:
    return make_layout(8, 8, cg->type_i64);
  case NY_BT_I128:
  case NY_BT_U128:
    return make_layout(16, 16, cg->type_i128);
  case NY_BT_F32:
    return make_layout(4, 4, cg->type_f32);
  case NY_BT_F64:
    return make_layout(8, 8, cg->type_f64);
  case NY_BT_F128:
    return make_layout(16, 16, cg->type_f128);
  default:
    break;
  }

  layout_def_t *layout = lookup_layout(cg, resolved_name);
  if (layout) {
    return make_layout(layout->size, layout->align, layout->llvm_type);
  }

  ny_diag_error(tok, "unknown type name '%s' in layout", type_name);
  cg->had_error = 1;
  return invalid;
}
