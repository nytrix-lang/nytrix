#include "base/util.h"
#include "priv.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

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

static bool is_ptr_type_name(const char *name) {
  return name && name[0] == '*';
}

static bool is_float_type_name(const char *name) {
  const char *t = type_tail(name);
  if (!t)
    return false;
  return strcmp(t, "f32") == 0 || strcmp(t, "f64") == 0 ||
         strcmp(t, "f128") == 0;
}

static bool is_int_type_name(const char *name) {
  const char *t = type_tail(name);
  if (!t)
    return false;
  return strcmp(t, "int") == 0 || strcmp(t, "char") == 0 ||
         strcmp(t, "i8") == 0 ||
         strcmp(t, "i16") == 0 || strcmp(t, "i32") == 0 ||
         strcmp(t, "i64") == 0 || strcmp(t, "i128") == 0 ||
         strcmp(t, "u8") == 0 || strcmp(t, "u16") == 0 ||
         strcmp(t, "u32") == 0 || strcmp(t, "u64") == 0 ||
         strcmp(t, "u128") == 0;
}

static bool is_bool_type_name(const char *name) {
  const char *t = type_tail(name);
  return t && strcmp(t, "bool") == 0;
}

static bool is_str_type_name(const char *name) {
  const char *t = type_tail(name);
  return t && strcmp(t, "str") == 0;
}

static bool is_char_type_name(const char *name) {
  const char *t = type_tail(name);
  return t && strcmp(t, "char") == 0;
}

static bool is_result_type_name(const char *name) {
  const char *t = type_tail(name);
  return t && strcmp(t, "Result") == 0;
}

static bool is_void_type_name(const char *name) {
  const char *t = type_tail(name);
  return t && strcmp(t, "void") == 0;
}

static bool literal_int_fits(int64_t val, const char *want) {
  const char *t = type_tail(want);
  if (!t)
    return true;
  if (strcmp(t, "int") == 0 || strcmp(t, "i64") == 0)
    return true;
  if (strcmp(t, "i8") == 0)
    return val >= INT8_MIN && val <= INT8_MAX;
  if (strcmp(t, "i16") == 0)
    return val >= INT16_MIN && val <= INT16_MAX;
  if (strcmp(t, "i32") == 0)
    return val >= INT32_MIN && val <= INT32_MAX;
  if (strcmp(t, "i128") == 0)
    return true;
  if (strcmp(t, "u8") == 0)
    return val >= 0 && val <= UINT8_MAX;
  if (strcmp(t, "u16") == 0)
    return val >= 0 && val <= UINT16_MAX;
  if (strcmp(t, "u32") == 0)
    return val >= 0 && val <= UINT32_MAX;
  if (strcmp(t, "u64") == 0 || strcmp(t, "u128") == 0)
    return val >= 0;
  return true;
}

static bool type_compatible_simple(const char *want, const char *got) {
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
    if (e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT) {
      const char *n = e->as.call.callee->as.ident.name;
      fun_sig *sig = lookup_fun(cg, n);
      if (sig && sig->return_type)
        return sig->return_type;
    }
    return NULL;
  case NY_E_MEMCALL:
    if (e->as.memcall.target &&
        e->as.memcall.target->kind == NY_E_IDENT) {
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
    if (expr->kind == NY_E_LITERAL &&
        expr->as.literal.kind == NY_LIT_INT) {
      if (expr->tok.kind == NY_T_NIL || expr->as.literal.as.i == 0)
        return true;
    }
    ny_diag_error(tok, "void %s cannot return a value", ctx ? ctx : "context");
    cg->had_error = 1;
    return false;
  }
  if (expr->kind == NY_E_LITERAL) {
    if (expr->as.literal.kind == NY_LIT_INT) {
      if (expr->tok.kind == NY_T_NIL)
        return true;
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
  if (!got || type_name_eq(got, "nil"))
    return true;
  if (!type_compatible_simple(want, got)) {
    ny_diag_error(tok, "type mismatch: expected %s, got %s",
                  type_tail(want), type_tail(got));
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

  // Handle pointer types first
  if (type_name[0] == '*') {
    // For now, all pointers are i64 (representing memory addresses)
    // A more sophisticated type system would resolve the base type as well.
    return cg->type_i64;
  }

  if (strcmp(type_name, "int") == 0) {
    return cg->type_i64; // Nytrix 'int' is 64-bit tagged int
  } else if (strcmp(type_name, "str") == 0) {
    return cg->type_i64; // Nytrix 'str' is 64-bit tagged ptr
  } else if (strcmp(type_name, "bool") == 0) {
    return cg->type_i64; // Nytrix 'bool' is 64-bit tagged bool
  } else if (strcmp(type_name, "void") == 0) {
    return cg->type_i64; // Nytrix 'void' for now maps to i64 (like nil)
  } else if (strcmp(type_name, "char") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "i8") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "i16") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "i32") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "i64") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "i128") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "u8") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "u16") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "u32") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "u64") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "u128") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "f32") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "f64") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "f128") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "Result") == 0) {
    return cg->type_i64;
  }

  // TODO: Resolve user-defined types (enums, structs)

  ny_diag_error(tok, "unknown type name '%s'", type_name);
  cg->had_error = 1;
  return cg->type_i64; // Fallback to default
}

LLVMTypeRef resolve_abi_type_name(codegen_t *cg, const char *type_name,
                                  token_t tok) {
  if (!type_name || !*type_name) {
    return cg->type_i64;
  }

  if (type_name[0] == '*') {
    return cg->type_i8ptr;
  }

  if (strcmp(type_name, "int") == 0 || strcmp(type_name, "str") == 0 ||
      strcmp(type_name, "bool") == 0 || strcmp(type_name, "Result") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "void") == 0) {
    return LLVMVoidTypeInContext(cg->ctx);
  } else if (strcmp(type_name, "char") == 0) {
    return cg->type_i8;
  } else if (strcmp(type_name, "i8") == 0 || strcmp(type_name, "u8") == 0) {
    return cg->type_i8;
  } else if (strcmp(type_name, "i16") == 0 ||
             strcmp(type_name, "u16") == 0) {
    return cg->type_i16;
  } else if (strcmp(type_name, "i32") == 0 ||
             strcmp(type_name, "u32") == 0) {
    return cg->type_i32;
  } else if (strcmp(type_name, "i64") == 0 ||
             strcmp(type_name, "u64") == 0) {
    return cg->type_i64;
  } else if (strcmp(type_name, "i128") == 0 ||
             strcmp(type_name, "u128") == 0) {
    return cg->type_i128;
  } else if (strcmp(type_name, "f32") == 0) {
    return cg->type_f32;
  } else if (strcmp(type_name, "f64") == 0) {
    return cg->type_f64;
  } else if (strcmp(type_name, "f128") == 0) {
    return cg->type_f128;
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
  return NULL;
}

type_layout_t resolve_raw_layout(codegen_t *cg, const char *type_name,
                                 token_t tok) {
  type_layout_t invalid = {0, 0, NULL, false};
  if (!type_name || !*type_name)
    return invalid;

  if (type_name[0] == '*') {
    return make_layout(ptr_size(), ptr_size(), cg->type_i8ptr);
  }

  if (strcmp(type_name, "int") == 0) {
    return make_layout(8, 8, cg->type_i64);
  } else if (strcmp(type_name, "str") == 0) {
    return make_layout(ptr_size(), ptr_size(), cg->type_i8ptr);
  } else if (strcmp(type_name, "bool") == 0) {
    return make_layout(1, 1, cg->type_bool);
  } else if (strcmp(type_name, "void") == 0) {
    return make_layout(0, 1, LLVMVoidTypeInContext(cg->ctx));
  } else if (strcmp(type_name, "char") == 0) {
    return make_layout(1, 1, cg->type_i8);
  } else if (strcmp(type_name, "i8") == 0 || strcmp(type_name, "u8") == 0) {
    return make_layout(1, 1, cg->type_i8);
  } else if (strcmp(type_name, "i16") == 0 ||
             strcmp(type_name, "u16") == 0) {
    return make_layout(2, 2, cg->type_i16);
  } else if (strcmp(type_name, "i32") == 0 ||
             strcmp(type_name, "u32") == 0) {
    return make_layout(4, 4, cg->type_i32);
  } else if (strcmp(type_name, "i64") == 0 ||
             strcmp(type_name, "u64") == 0) {
    return make_layout(8, 8, cg->type_i64);
  } else if (strcmp(type_name, "i128") == 0 ||
             strcmp(type_name, "u128") == 0) {
    return make_layout(16, 16, cg->type_i128);
  } else if (strcmp(type_name, "f32") == 0) {
    return make_layout(4, 4, cg->type_f32);
  } else if (strcmp(type_name, "f64") == 0) {
    return make_layout(8, 8, cg->type_f64);
  } else if (strcmp(type_name, "f128") == 0) {
    return make_layout(16, 16, cg->type_f128);
  }

  layout_def_t *layout = lookup_layout(cg, type_name);
  if (layout) {
    return make_layout(layout->size, layout->align, NULL);
  }

  ny_diag_error(tok, "unknown type name '%s' in layout", type_name);
  cg->had_error = 1;
  return invalid;
}
