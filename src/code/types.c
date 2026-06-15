#include "base/util.h"
#include "priv.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

typedef enum ny_builtin_type_kind_t {
  NY_BT_UNKNOWN = 0,
  NY_BT_INT,
  NY_BT_NUMBER,
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
  NY_BT_C64,
  NY_BT_C128,
  NY_BT_COMPLEX,
  NY_BT_RESULT,
  NY_BT_PTR,
  NY_BT_CSTR,
  NY_BT_FNPTR,
  NY_BT_HANDLE,
  NY_BT_LIST,
  NY_BT_DICT,
  NY_BT_TUPLE,
  NY_BT_SEQ,
  NY_BT_SET,
  NY_BT_BYTES,
  NY_BT_RANGE,
  NY_BT_BIGINT,
  NY_BT_INTEGER,
  NY_BT_FLOAT,
  NY_BT_SCALAR,
  NY_BT_COLLECTION,
  NY_BT_CONTAINER,
  NY_BT_ITERABLE,
  NY_BT_INDEXABLE,
  NY_BT_ALLOCATOR,
} ny_builtin_type_kind_t;

static const char *type_skip_nullable(const char *name) {
  if (!name)
    return NULL;
  while (*name == '?')
    name++;
  return name;
}

static const char *type_attached_owner(const char *name) {
  if (!name)
    return NULL;
  while (*name == '?' || *name == '*')
    name++;
  if (!*name)
    return NULL;
  const char *leaf = strrchr(name, '.');
  leaf = leaf ? leaf + 1 : name;
  if (strcmp(leaf, "any") == 0 || strcmp(leaf, "str") == 0 ||
      strcmp(leaf, "list") == 0 || strcmp(leaf, "dict") == 0 ||
      strcmp(leaf, "set") == 0 || strcmp(leaf, "tuple") == 0 ||
      strcmp(leaf, "bytes") == 0 || strcmp(leaf, "range") == 0 ||
      strcmp(leaf, "bigint") == 0 || strcmp(leaf, "int") == 0 ||
      strcmp(leaf, "float") == 0 || strcmp(leaf, "f32") == 0 ||
      strcmp(leaf, "f64") == 0 || strcmp(leaf, "bool") == 0)
    return leaf;
  return name;
}

static const char *type_builtin_generic_owner(const char *owner) {
  if (!owner)
    return NULL;
  const char *leaf = strrchr(owner, '.');
  leaf = leaf ? leaf + 1 : owner;
#define NY_GENERIC_OWNER_IF_BASE(name)                                         \
  do {                                                                         \
    size_t len = sizeof(name) - 1;                                             \
    if (strncmp(leaf, name, len) == 0 && leaf[len] == '<')                     \
      return name;                                                             \
  } while (0)
  NY_GENERIC_OWNER_IF_BASE("list");
  NY_GENERIC_OWNER_IF_BASE("dict");
  NY_GENERIC_OWNER_IF_BASE("set");
  NY_GENERIC_OWNER_IF_BASE("tuple");
  NY_GENERIC_OWNER_IF_BASE("Result");
  NY_GENERIC_OWNER_IF_BASE("Option");
#undef NY_GENERIC_OWNER_IF_BASE
  return NULL;
}

static binding *type_lookup_binding(codegen_t *cg, scope *scopes, size_t depth,
                                    const char *name, size_t name_len,
                                    uint64_t hash) {
  return lookup_binding_hash(cg, scopes, depth, name, name_len, hash);
}

static bool type_call_builtin_name_shadowed(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *callee) {
  return ny_call_builtin_name_shadowed(cg, scopes, depth, callee);
}

static fun_sig *type_lookup_attached_method(codegen_t *cg,
                                            const char *type_name,
                                            const char *method_name) {
  const char *owner = type_attached_owner(type_name);
  if (!cg || !owner || !*owner || !method_name || !*method_name)
    return NULL;
  char direct[512];
  int n = snprintf(direct, sizeof(direct), "%s.%s", owner, method_name);
  if (n > 0 && (size_t)n < sizeof(direct)) {
    fun_sig *sig = lookup_fun(cg, direct, 0);
    if (sig)
      return sig;
  }
  if (strcmp(owner, "integer") == 0) {
    fun_sig *sig = type_lookup_attached_method(cg, "bigint", method_name);
    if (sig)
      return sig;
    sig = type_lookup_attached_method(cg, "int", method_name);
    if (sig)
      return sig;
  }
  const char *generic_owner = type_builtin_generic_owner(owner);
  if (generic_owner && strcmp(generic_owner, owner) != 0) {
    fun_sig *sig = type_lookup_attached_method(cg, generic_owner, method_name);
    if (sig)
      return sig;
  }
  if (strchr(owner, '.'))
    return NULL;
  for (size_t i = cg->user_use_modules.len; i > 0; i--) {
    const char *mod = cg->user_use_modules.data[i - 1];
    if (!mod || !*mod)
      continue;
    char imported[512];
    int in = snprintf(imported, sizeof(imported), "%s.%s.%s", mod, owner,
                      method_name);
    if (in > 0 && (size_t)in < sizeof(imported)) {
      fun_sig *sig = lookup_fun(cg, imported, 0);
      if (sig)
        return sig;
    }
  }
  for (size_t i = cg->use_modules.len; i > 0; i--) {
    const char *mod = cg->use_modules.data[i - 1];
    if (!mod || !*mod)
      continue;
    char imported[512];
    int in = snprintf(imported, sizeof(imported), "%s.%s.%s", mod, owner,
                      method_name);
    if (in > 0 && (size_t)in < sizeof(imported)) {
      fun_sig *sig = lookup_fun(cg, imported, 0);
      if (sig)
        return sig;
    }
  }
  char core_builtin[512];
  int core_n = snprintf(core_builtin, sizeof(core_builtin), "std.core.%s.%s",
                        owner, method_name);
  if (core_n > 0 && (size_t)core_n < sizeof(core_builtin)) {
    fun_sig *sig = lookup_fun(cg, core_builtin, 0);
    if (sig)
      return sig;
  }
  return NULL;
}

static const char *type_name_from_binding(binding *b) {
  if (!b)
    return NULL;
  if (b->type_name)
    return b->type_name;
  if (b->decl_type_name)
    return b->decl_type_name;
  if (b->is_f64_slot || b->is_f64_direct)
    return "f64";
  if (b->is_f32_slot || b->is_f32_direct)
    return "f32";
  if (b->is_int_slot || b->is_int_direct)
    return "int";
  return NULL;
}

static const char *type_member_module_alias_global_type(codegen_t *cg,
                                                        scope *scopes,
                                                        size_t depth,
                                                        expr_t *e) {
  if (!cg || !e || e->kind != NY_E_MEMBER || !e->as.member.target ||
      e->as.member.target->kind != NY_E_IDENT ||
      !e->as.member.target->as.ident.name || !e->as.member.name)
    return NULL;
  expr_t *target = e->as.member.target;
  const char *target_name = target->as.ident.name;
  size_t target_len = (size_t)target->tok.len;
  if (target_len == 0)
    target_len = strlen(target_name);
  const char *module_name = ny_lookup_module_alias(
      cg, scopes, depth, target_name, target_len, target->as.ident.hash);
  if (!module_name || !*module_name)
    return NULL;
  char dotted[512];
  int nw =
      snprintf(dotted, sizeof(dotted), "%s.%s", module_name, e->as.member.name);
  if (nw <= 0 || (size_t)nw >= sizeof(dotted))
    return NULL;
  binding *gb = lookup_global(cg, dotted);
  if (!gb) {
    const char *resolved = resolve_import_alias(cg, dotted);
    if (resolved && *resolved && strcmp(resolved, dotted) != 0)
      gb = lookup_global(cg, resolved);
  }
  return type_name_from_binding(gb);
}

static const char *type_enum_member_owner_name(codegen_t *cg, expr_t *e) {
  if (!cg || !e)
    return NULL;
  enum_def_t *owner = NULL;
  enum_member_def_t *member = NULL;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    member = lookup_enum_member_owner(cg, e->as.ident.name, &owner);
  } else if (e->kind == NY_E_MEMBER) {
    char *full_name = codegen_full_name(cg, e, cg->arena);
    if (full_name)
      member = lookup_enum_member_owner(cg, full_name, &owner);
  }
  return (member && owner && owner->name) ? owner->name : NULL;
}

static const char *type_sig_return_name(fun_sig *sig) {
  if (!sig)
    return NULL;
  if (sig->return_type)
    return sig->return_type;
  return sig->inferred_return_type;
}

static fun_sig *type_lookup_static_call_sig_by_name(codegen_t *cg,
                                                    const char *name,
                                                    size_t argc,
                                                    uint64_t hash) {
  if (!cg || !name || !*name)
    return NULL;
  fun_sig *sig = resolve_overload(cg, name, argc, hash);
  if (!sig)
    sig = lookup_use_module_fun(cg, name, argc);
  if (!sig)
    sig = lookup_fun(cg, name, hash);
  if (sig && !ny_sig_in_current_sigs(cg, sig))
    return NULL;
  return sig;
}

static fun_sig *type_lookup_static_call_sig(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *callee,
                                            size_t argc) {
  if (!cg || !callee)
    return NULL;
  if (callee->kind == NY_E_IDENT) {
    const char *name = callee->as.ident.name;
    fun_sig *sig = type_lookup_static_call_sig_by_name(
        cg, name, argc, callee->as.ident.hash);
    if (sig)
      return sig;
  }
  if (callee->kind == NY_E_MEMBER && callee->as.member.target &&
      callee->as.member.name) {
    char module_path[1024];
    if (ny_resolve_module_expr_path(cg, scopes, depth, callee->as.member.target,
                                    module_path, sizeof(module_path))) {
      char resolved_fun[1280];
      if (ny_resolve_module_function_path(cg, module_path,
                                          callee->as.member.name,
                                          resolved_fun, sizeof(resolved_fun))) {
        fun_sig *sig =
            type_lookup_static_call_sig_by_name(cg, resolved_fun, argc, 0);
        if (sig)
          return sig;
      }
    }
  }
  char *full = codegen_full_name(cg, callee, cg->arena);
  if (!full)
    return NULL;
  return type_lookup_static_call_sig_by_name(cg, full, argc, 0);
}

static ssize_t ny_enum_type_param_index(enum_def_t *owner, const char *name) {
  if (!owner || !name)
    return -1;
  for (size_t i = 0; i < owner->type_params.len; i++) {
    if (owner->type_params.data[i] &&
        strcmp(owner->type_params.data[i], name) == 0)
      return (ssize_t)i;
  }
  return -1;
}

static const char *ny_infer_generic_adt_return_type(codegen_t *cg,
                                                    scope *scopes, size_t depth,
                                                    enum_def_t *owner,
                                                    enum_member_def_t *member,
                                                    ny_call_arg_list *args) {
  if (!cg || !owner || !member || owner->type_params.len == 0)
    return owner ? owner->name : NULL;
  const char **actuals = alloca(sizeof(const char *) * owner->type_params.len);
  for (size_t i = 0; i < owner->type_params.len; i++)
    actuals[i] = NULL;
  for (size_t ai = 0; args && ai < args->len; ai++) {
    call_arg_t *arg = &args->data[ai];
    ssize_t field_idx = -1;
    if (arg->name) {
      for (size_t fi = 0; fi < member->fields.len; fi++) {
        if (strcmp(member->fields.data[fi].name, arg->name) == 0) {
          field_idx = (ssize_t)fi;
          break;
        }
      }
    } else if (ai < member->fields.len) {
      field_idx = (ssize_t)ai;
    }
    if (field_idx < 0)
      continue;
    enum_field_def_t *field = &member->fields.data[field_idx];
    ssize_t param_idx = ny_enum_type_param_index(owner, field->type_name);
    if (param_idx < 0)
      continue;
    const char *arg_type = infer_expr_type(cg, scopes, depth, arg->val);
    if (arg_type && *arg_type)
      actuals[param_idx] = arg_type;
  }
  for (size_t i = 0; i < owner->type_params.len; i++) {
    if (!actuals[i])
      return owner->name;
  }
  size_t len = strlen(owner->name) + 3;
  for (size_t i = 0; i < owner->type_params.len; i++)
    len += strlen(actuals[i]) + (i ? 2 : 0);
  char *out = arena_alloc(cg->arena, len);
  size_t at = 0;
  size_t nlen = strlen(owner->name);
  memcpy(out + at, owner->name, nlen);
  at += nlen;
  out[at++] = '<';
  for (size_t i = 0; i < owner->type_params.len; i++) {
    if (i) {
      out[at++] = ',';
      out[at++] = ' ';
    }
    size_t alen = strlen(actuals[i]);
    memcpy(out + at, actuals[i], alen);
    at += alen;
  }
  out[at++] = '>';
  out[at] = '\0';
  return out;
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
    } else if (name[0] == 'c') {
      if (name[1] == '6' && name[2] == '4')
        return NY_BT_C64;
    } else if (name[0] == 's' && name[1] == 't' && name[2] == 'r') {
      return NY_BT_STR;
    } else if (name[0] == 'p' && name[1] == 't' && name[2] == 'r') {
      return NY_BT_PTR;
    } else if (name[0] == 's' && name[1] == 'e' && name[2] == 'q') {
      return NY_BT_SEQ;
    } else if (name[0] == 's' && name[1] == 'e' && name[2] == 't') {
      return NY_BT_SET;
    }
    break;
  case 4:
    if (memcmp(name, "bool", 4) == 0)
      return NY_BT_BOOL;
    if (memcmp(name, "void", 4) == 0)
      return NY_BT_VOID;
    if (memcmp(name, "char", 4) == 0)
      return NY_BT_CHAR;
    if (memcmp(name, "cstr", 4) == 0)
      return NY_BT_CSTR;
    if (name[0] == 'i' && memcmp(name, "i128", 4) == 0)
      return NY_BT_I128;
    if (name[0] == 'u' && memcmp(name, "u128", 4) == 0)
      return NY_BT_U128;
    if (name[0] == 'f' && memcmp(name, "f128", 4) == 0)
      return NY_BT_F128;
    if (name[0] == 'c' && memcmp(name, "c128", 4) == 0)
      return NY_BT_C128;
    if (memcmp(name, "list", 4) == 0)
      return NY_BT_LIST;
    if (memcmp(name, "dict", 4) == 0)
      return NY_BT_DICT;
    break;
  case 5:
    if (memcmp(name, "fnptr", 5) == 0)
      return NY_BT_FNPTR;
    if (memcmp(name, "tuple", 5) == 0)
      return NY_BT_TUPLE;
    if (memcmp(name, "bytes", 5) == 0)
      return NY_BT_BYTES;
    if (memcmp(name, "range", 5) == 0)
      return NY_BT_RANGE;
    if (memcmp(name, "float", 5) == 0)
      return NY_BT_FLOAT;
    break;
  case 6:
    if (memcmp(name, "Result", 6) == 0)
      return NY_BT_RESULT;
    if (memcmp(name, "bigint", 6) == 0)
      return NY_BT_BIGINT;
    if (memcmp(name, "handle", 6) == 0)
      return NY_BT_HANDLE;
    if (memcmp(name, "number", 6) == 0)
      return NY_BT_NUMBER;
    if (memcmp(name, "scalar", 6) == 0)
      return NY_BT_SCALAR;
    break;
  case 7:
    if (memcmp(name, "complex", 7) == 0)
      return NY_BT_COMPLEX;
    if (memcmp(name, "numeric", 7) == 0)
      return NY_BT_NUMBER;
    if (memcmp(name, "integer", 7) == 0)
      return NY_BT_INTEGER;
    break;
  case 8:
    if (memcmp(name, "sequence", 8) == 0)
      return NY_BT_SEQ;
    if (memcmp(name, "iterable", 8) == 0)
      return NY_BT_ITERABLE;
    break;
  case 9:
    if (memcmp(name, "indexable", 9) == 0)
      return NY_BT_INDEXABLE;
    if (memcmp(name, "allocator", 9) == 0)
      return NY_BT_ALLOCATOR;
    if (memcmp(name, "container", 9) == 0)
      return NY_BT_CONTAINER;
    break;
  case 10:
    if (memcmp(name, "collection", 10) == 0)
      return NY_BT_COLLECTION;
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
  t = t ? t + 1 : name;
  const char *lt = strchr(t, '<');
  if (!lt)
    return classify_builtin_type_exact(t);
  size_t base_len = (size_t)(lt - t);
  char stack_buf[64];
  if (base_len < sizeof(stack_buf)) {
    memcpy(stack_buf, t, base_len);
    stack_buf[base_len] = '\0';
    return classify_builtin_type_exact(stack_buf);
  }
  char *base = ny_strndup(t, base_len);
  ny_builtin_type_kind_t out = classify_builtin_type_exact(base);
  free(base);
  return out;
}

static LLVMTypeRef complex_abi_type(codegen_t *cg, const char *name) {
  ny_builtin_type_kind_t k = classify_builtin_type_tail(name);
  LLVMTypeRef elem = (k == NY_BT_C64) ? cg->type_f32 : cg->type_f64;
  LLVMTypeRef elems[2] = {elem, elem};
  return LLVMStructTypeInContext(cg->ctx, elems, 2, false);
}

static bool type_name_eq(const char *a, const char *b) {
  if (!a || !b)
    return false;
  if (strcmp(a, b) == 0)
    return true;
  const char *ta = ny_name_leaf(a);
  const char *tb = ny_name_leaf(b);
  return ta && tb && strcmp(ta, tb) == 0;
}

static bool is_opaque_system_abi_type(const char *name);

void ny_register_tagged_type(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return;
  const char *base = type_skip_nullable(name);
  if (!base || !*base || base[0] == '*')
    return;
  if (classify_builtin_type_tail(base) != NY_BT_UNKNOWN)
    return;
  if (lookup_layout(cg, base))
    return;
  for (size_t i = 0; i < cg->tagged_types.len; ++i) {
    if (type_name_eq(cg->tagged_types.data[i], base))
      return;
  }
  vec_push(&cg->tagged_types, ny_strdup(base));
}

bool ny_lookup_tagged_type(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return false;
  const char *base = type_skip_nullable(name);
  if (!base || !*base || base[0] == '*')
    return false;
  char generic_base[256];
  const char *lt = strchr(base, '<');
  if (lt) {
    size_t n = (size_t)(lt - base);
    if (n >= sizeof(generic_base))
      n = sizeof(generic_base) - 1;
    memcpy(generic_base, base, n);
    generic_base[n] = '\0';
    base = generic_base;
  }
  for (size_t i = 0; i < cg->tagged_types.len; ++i) {
    if (type_name_eq(cg->tagged_types.data[i], base))
      return true;
  }
  if (cg->current_module_name && !strchr(base, '.')) {
    const char *qname = codegen_qname(cg, base, cg->current_module_name);
    for (size_t i = 0; i < cg->tagged_types.len; ++i) {
      if (type_name_eq(cg->tagged_types.data[i], qname))
        return true;
    }
  }
  if (cg->comptime && cg->parent)
    return ny_lookup_tagged_type(cg->parent, base);
  return false;
}

static bool is_nullable_type_name(const char *name) {
  return name && name[0] == '?';
}

static bool is_any_type_name(const char *name) {
  const char *tail = ny_name_leaf(type_skip_nullable(name));
  return tail && strcmp(tail, "any") == 0;
}

static bool is_ptr_type_name(const char *name) {
  name = type_skip_nullable(name);
  return (name && name[0] == '*') || (strcmp(name, "ptr") == 0);
}

static bool is_float_type_name(const char *name) {
  ny_builtin_type_kind_t k = classify_builtin_type_tail(name);
  return k == NY_BT_F32 || k == NY_BT_F64 || k == NY_BT_F128 ||
         k == NY_BT_FLOAT;
}

static bool is_number_type_name(const char *name) {
  return ny_name_tail_is(type_skip_nullable(name), "number") ||
         ny_name_tail_is(type_skip_nullable(name), "numeric");
}

static bool is_bigint_type_name(const char *name) {
  const char *tail = ny_name_leaf(type_skip_nullable(name));
  return tail && (strcmp(tail, "bigint") == 0 || strcmp(tail, "BigInt") == 0);
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
  case NY_BT_HANDLE:
  case NY_BT_INTEGER:
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

static bool is_seq_type_name(const char *name) {
  return classify_builtin_type_tail(name) == NY_BT_SEQ;
}

static bool is_seq_compatible_type_name(const char *name) {
  ny_builtin_type_kind_t k = classify_builtin_type_tail(name);
  return k == NY_BT_LIST || k == NY_BT_TUPLE || k == NY_BT_STR ||
         k == NY_BT_RANGE || k == NY_BT_BYTES;
}

static bool is_type_group_name(const char *name) {
  switch (classify_builtin_type_tail(name)) {
  case NY_BT_NUMBER:
  case NY_BT_INTEGER:
  case NY_BT_FLOAT:
  case NY_BT_SCALAR:
  case NY_BT_SEQ:
  case NY_BT_COLLECTION:
  case NY_BT_CONTAINER:
  case NY_BT_ITERABLE:
  case NY_BT_INDEXABLE:
  case NY_BT_ALLOCATOR:
    return true;
  default:
    return false;
  }
}

static bool type_name_collection_base_is(const char *name, const char *base) {
  if (!name || !base)
    return false;
  name = type_skip_nullable(name);
  const char *leaf = ny_name_leaf(name);
  if (!leaf)
    return false;
  size_t n = strcspn(leaf, "<| ");
  return strlen(base) == n && strncmp(leaf, base, n) == 0;
}

static bool type_group_accepts_type(const char *group, const char *type) {
  ny_builtin_type_kind_t g = classify_builtin_type_tail(group);
  ny_builtin_type_kind_t t = classify_builtin_type_tail(type);
  if (g == NY_BT_UNKNOWN || !type || !*type)
    return false;
  if (g == t)
    return true;
  if (g == NY_BT_NUMBER)
    return is_int_type_name(type) || is_float_type_name(type) ||
           is_bigint_type_name(type) || is_number_type_name(type);
  if (g == NY_BT_INTEGER)
    return is_int_type_name(type) || is_bigint_type_name(type);
  if (g == NY_BT_FLOAT)
    return t == NY_BT_F32 || t == NY_BT_F64 || t == NY_BT_F128 ||
           t == NY_BT_FLOAT;
  if (g == NY_BT_SCALAR)
    return type_group_accepts_type("number", type) || t == NY_BT_BOOL ||
           t == NY_BT_STR || t == NY_BT_CHAR;
  if (g == NY_BT_SEQ)
    return is_seq_compatible_type_name(type) ||
           type_name_collection_base_is(type, "list") ||
           type_name_collection_base_is(type, "tuple");
  if (g == NY_BT_COLLECTION)
    return type_name_collection_base_is(type, "list") ||
           type_name_collection_base_is(type, "tuple") ||
           type_name_collection_base_is(type, "dict") ||
           type_name_collection_base_is(type, "set");
  if (g == NY_BT_CONTAINER)
    return type_group_accepts_type("collection", type) || t == NY_BT_BYTES ||
           t == NY_BT_RANGE;
  if (g == NY_BT_ITERABLE)
    return type_group_accepts_type("seq", type) ||
           type_name_collection_base_is(type, "dict") ||
           type_name_collection_base_is(type, "set") || t == NY_BT_BYTES;
  if (g == NY_BT_INDEXABLE)
    return type_group_accepts_type("seq", type) ||
           type_name_collection_base_is(type, "dict") || t == NY_BT_BYTES;
  if (g == NY_BT_ALLOCATOR)
    return t == NY_BT_PTR || t == NY_BT_HANDLE;
  return false;
}

static int vector_type_dim(const char *name) {
  name = type_skip_nullable(name);
  const char *tail = ny_name_leaf(name);
  if (!tail)
    return 0;
  if (strcmp(tail, "vec2") == 0 || strcmp(tail, "Vector2") == 0)
    return 2;
  if (strcmp(tail, "vec3") == 0 || strcmp(tail, "Vector3") == 0)
    return 3;
  if (strcmp(tail, "vec4") == 0 || strcmp(tail, "Vector4") == 0)
    return 4;
  return 0;
}

static bool is_vector_type_name(const char *name) {
  return vector_type_dim(name) > 0;
}

static bool operator_type_name_eq(const char *want, const char *got) {
  if (type_name_eq(want, got))
    return true;
  int want_vec = vector_type_dim(want);
  int got_vec = vector_type_dim(got);
  return want_vec > 0 && want_vec == got_vec;
}

static bool operator_type_is_core_scalar(const char *name) {
  if (is_ptr_type_name(name))
    return true;
  ny_builtin_type_kind_t k = classify_builtin_type_tail(name);
  return k == NY_BT_INT || k == NY_BT_STR || k == NY_BT_BOOL ||
         k == NY_BT_CHAR || k == NY_BT_I8 || k == NY_BT_I16 || k == NY_BT_I32 ||
         k == NY_BT_I64 || k == NY_BT_I128 || k == NY_BT_U8 || k == NY_BT_U16 ||
         k == NY_BT_U32 || k == NY_BT_U64 || k == NY_BT_U128 ||
         k == NY_BT_F32 || k == NY_BT_F64 || k == NY_BT_F128 || k == NY_BT_PTR;
}

static const char *infer_scoped_operator_type(codegen_t *cg, const char *op,
                                              const char *lt, const char *rt) {
  if (!cg || !op || !lt || !rt || cg->operators.len == 0)
    return NULL;
  if (operator_type_is_core_scalar(lt) && operator_type_is_core_scalar(rt))
    return NULL;
  for (size_t i = cg->operators.len; i > 0; --i) {
    ny_operator_def_t *def = &cg->operators.data[i - 1];
    if (!def->op || strcmp(def->op, op) != 0)
      continue;
    if (def->module_name && *def->module_name) {
      bool active = (cg->current_module_name &&
                     strcmp(cg->current_module_name, def->module_name) == 0) ||
                    ny_is_module_active(cg, def->module_name);
      if (!active)
        continue;
    }
    if (operator_type_name_eq(def->left_type, lt) &&
        operator_type_name_eq(def->right_type, rt))
      return def->return_type;
  }
  return NULL;
}

static const char *vector_type_name_for_dim(int dim) {
  switch (dim) {
  case 2:
    return "vec2";
  case 3:
    return "vec3";
  case 4:
    return "vec4";
  default:
    return NULL;
  }
}

static const char *vector_constructor_return_type(const char *name) {
  const char *tail = ny_name_leaf(name);
  if (!tail)
    return NULL;
  if (strcmp(tail, "Vector2") == 0)
    return "vec2";
  if (strcmp(tail, "Vector3") == 0)
    return "vec3";
  if (strcmp(tail, "Vector4") == 0)
    return "vec4";
  return NULL;
}

bool ny_type_is_tagged(const char *name) {
  if (!name || !*name)
    return true;
  if (is_vector_type_name(name))
    return true;
  ny_builtin_type_kind_t k = classify_builtin_type_tail(name);
  switch (k) {
  case NY_BT_PTR:
  case NY_BT_FNPTR:
  case NY_BT_INT:
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
  case NY_BT_C64:
  case NY_BT_C128:
  case NY_BT_COMPLEX:
  case NY_BT_CHAR:
  case NY_BT_HANDLE:
    return false;
  case NY_BT_STR:
  case NY_BT_BOOL:
  case NY_BT_RESULT:
  case NY_BT_LIST:
  case NY_BT_DICT:
  case NY_BT_TUPLE:
  case NY_BT_SEQ:
  case NY_BT_SET:
  case NY_BT_BYTES:
  case NY_BT_RANGE:
  case NY_BT_BIGINT:
  case NY_BT_NUMBER:
  case NY_BT_INTEGER:
  case NY_BT_FLOAT:
  case NY_BT_SCALAR:
  case NY_BT_COLLECTION:
  case NY_BT_CONTAINER:
  case NY_BT_ITERABLE:
  case NY_BT_INDEXABLE:
  case NY_BT_ALLOCATOR:
    return true;
  default:
    if (strcmp(name, "any") == 0)
      return true;
    return false;
  }
}

bool ny_is_native_abi_type_name(const char *name) {
  if (!name || !*name)
    return false;
  name = type_skip_nullable(name);
  if (!*name)
    return false;
  if (name[0] == '*')
    return true;
  if (strcmp(name, "fnptr") == 0)
    return true;
  if (is_opaque_system_abi_type(name))
    return true;
  ny_builtin_type_kind_t k = classify_builtin_type_tail(name);
  switch (k) {
  case NY_BT_I8:
  case NY_BT_I16:
  case NY_BT_I32:
  case NY_BT_I64:
  case NY_BT_U8:
  case NY_BT_U16:
  case NY_BT_U32:
  case NY_BT_U64:
  case NY_BT_F32:
  case NY_BT_F64:
  case NY_BT_C64:
  case NY_BT_C128:
  case NY_BT_COMPLEX:
  case NY_BT_PTR:
  case NY_BT_CSTR:
  case NY_BT_FNPTR:
  case NY_BT_HANDLE:
    return true;
  case NY_BT_INT:
    return true;
  default:
    return false;
  }
}

static bool is_opaque_system_abi_type(const char *name) {
  if (!name || !*name)
    return false;
  name = type_skip_nullable(name);
  if (strncmp(name, "struct ", 7) == 0)
    name += 7;
  const char *leaf = ny_name_leaf(name);
  if (!leaf || !*leaf)
    leaf = name;
  return strcmp(leaf, "timespec") == 0 || strcmp(leaf, "timeval") == 0;
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

static const char *infer_builtin_collection_call_type(const char *name) {
  const char *tail = ny_name_leaf(name);
  if (!tail)
    return NULL;
  if (strcmp(tail, "list") == 0)
    return "list";
  if (strcmp(tail, "dict") == 0)
    return "dict";
  if (strcmp(tail, "set") == 0)
    return "set";
  if (strcmp(tail, "tuple") == 0)
    return "tuple";
  if (strcmp(tail, "dict_keys") == 0 || strcmp(tail, "dict_values") == 0 ||
      strcmp(tail, "dict_items") == 0 || strcmp(tail, "keys") == 0 ||
      strcmp(tail, "values") == 0 || strcmp(tail, "items") == 0)
    return "list";
  return NULL;
}

static bool call_name_tail_is(const char *name, const char *tail) {
  return ny_name_tail_is(name, tail);
}

static const char *type_infer_make_result(codegen_t *cg, const char *ok_type,
                                          const char *err_type) {
  if (!ok_type || !*ok_type)
    ok_type = "any";
  if (!err_type || !*err_type)
    err_type = "any";
  size_t ok_len = strlen(ok_type);
  size_t err_len = strlen(err_type);
  char *out = arena_alloc(cg ? cg->arena : NULL, ok_len + err_len + 11);
  if (!out)
    return "Result";
  memcpy(out, "Result<", 7);
  memcpy(out + 7, ok_type, ok_len);
  out[7 + ok_len] = ',';
  out[8 + ok_len] = ' ';
  memcpy(out + 9 + ok_len, err_type, err_len);
  out[9 + ok_len + err_len] = '>';
  out[10 + ok_len + err_len] = '\0';
  return out;
}

static const char *
type_result_payload_type(codegen_t *cg, const char *type_name, bool want_ok) {
  if (!type_name || !*type_name)
    return NULL;
  const char *leaf = ny_name_leaf(type_name);
  if (!leaf)
    leaf = type_name;
  if (strncmp(leaf, "Result<", 7) != 0)
    return NULL;
  const char *lt = strchr(leaf, '<');
  const char *gt = strrchr(leaf, '>');
  if (!lt || !gt || gt <= lt + 1)
    return NULL;
  int depth = 0;
  const char *comma = NULL;
  for (const char *p = lt + 1; p < gt; ++p) {
    if (*p == '<')
      depth++;
    else if (*p == '>')
      depth--;
    else if (*p == ',' && depth == 0) {
      comma = p;
      break;
    }
  }
  if (!comma)
    return NULL;
  const char *start = want_ok ? lt + 1 : comma + 1;
  const char *end = want_ok ? comma : gt;
  while (start < end && (*start == ' ' || *start == '\t'))
    start++;
  while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
    end--;
  if (start >= end)
    return "any";
  return arena_strndup(cg ? cg->arena : NULL, start, (size_t)(end - start));
}

static const char *type_generic_arg_type(codegen_t *cg, const char *type_name,
                                         const char *base, size_t want_index) {
  if (!type_name || !base)
    return NULL;
  const char *owner = type_attached_owner(type_name);
  const char *leaf = ny_name_leaf(owner ? owner : type_name);
  if (!leaf)
    return NULL;
  size_t base_len = strlen(base);
  if (strncmp(leaf, base, base_len) != 0 || leaf[base_len] != '<')
    return NULL;
  const char *lt = leaf + base_len;
  const char *gt = strrchr(lt + 1, '>');
  if (!gt || gt <= lt + 1)
    return NULL;
  const char *start = lt + 1;
  int depth = 0;
  size_t index = 0;
  for (const char *p = start; p <= gt; ++p) {
    bool at_end = p == gt;
    if (!at_end) {
      if (*p == '<')
        depth++;
      else if (*p == '>')
        depth--;
    }
    if ((at_end || (*p == ',' && depth == 0))) {
      if (index == want_index) {
        const char *end = p;
        while (start < end && (*start == ' ' || *start == '\t'))
          start++;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
          end--;
        if (start >= end)
          return "any";
        return arena_strndup(cg ? cg->arena : NULL, start,
                             (size_t)(end - start));
      }
      index++;
      start = p + 1;
    }
  }
  return NULL;
}

static const char *
type_index_result_from_container_type(codegen_t *cg, const char *type_name) {
  if (!type_name)
    return NULL;
  const char *elem = type_generic_arg_type(cg, type_name, "list", 0);
  if (elem)
    return elem;
  elem = type_generic_arg_type(cg, type_name, "tuple", 0);
  if (elem)
    return elem;
  elem = type_generic_arg_type(cg, type_name, "set", 0);
  if (elem)
    return elem;
  elem = type_generic_arg_type(cg, type_name, "dict", 1);
  if (elem)
    return elem;
  return NULL;
}

static bool call_name_is_int_length_builtin(const char *name) {
  return call_name_tail_is(name, "len");
}

static bool call_name_is_int_arith_builtin(const char *name) {
  if (!name)
    return false;
  return call_name_tail_is(name, "add") || call_name_tail_is(name, "sub") ||
         call_name_tail_is(name, "mul") || call_name_tail_is(name, "band") ||
         call_name_tail_is(name, "bor") || call_name_tail_is(name, "bxor") ||
         call_name_tail_is(name, "bshl") || call_name_tail_is(name, "bshr") ||
         strcmp(name, "__add") == 0 || strcmp(name, "__sub") == 0 ||
         strcmp(name, "__mul") == 0;
}

static bool call_name_is_int_zeroarg_builtin(const char *name) {
  if (!name)
    return false;
  return call_name_tail_is(name, "ticks") || call_name_tail_is(name, "pid");
}

static bool call_name_is_bool_builtin(const char *name) {
  if (!name)
    return false;
  return call_name_tail_is(name, "eq") || call_name_tail_is(name, "ne") ||
         call_name_tail_is(name, "lt") || call_name_tail_is(name, "le") ||
         call_name_tail_is(name, "gt") || call_name_tail_is(name, "ge") ||
         call_name_tail_is(name, "is_int") ||
         call_name_tail_is(name, "is_list") ||
         call_name_tail_is(name, "is_dict") ||
         call_name_tail_is(name, "is_set") ||
         call_name_tail_is(name, "is_tuple") ||
         call_name_tail_is(name, "is_str") ||
         call_name_tail_is(name, "is_bytes") ||
         call_name_tail_is(name, "is_float") ||
         call_name_tail_is(name, "is_bool") ||
         call_name_tail_is(name, "is_nil") ||
         call_name_tail_is(name, "is_none") ||
         call_name_tail_is(name, "is_ptr") ||
         call_name_tail_is(name, "is_nytrix_obj") ||
         strcmp(name, "__eq") == 0 || strcmp(name, "__ne") == 0 ||
         strcmp(name, "__lt") == 0 || strcmp(name, "__le") == 0 ||
         strcmp(name, "__gt") == 0 || strcmp(name, "__ge") == 0 ||
         strcmp(name, "__is_int") == 0 || strcmp(name, "__is_ny_obj") == 0 ||
         strcmp(name, "__is_str_obj") == 0 ||
         strcmp(name, "__is_float_obj") == 0 || strcmp(name, "__is_ptr") == 0 ||
         strcmp(name, "__has_tag") == 0 || strcmp(name, "__runtime_tag") == 0;
}

static const char *ny_math_call_arg_type(codegen_t *cg, scope *scopes,
                                         size_t depth, ny_call_arg_list *args,
                                         size_t index) {
  if (!args || index >= args->len || !args->data[index].val)
    return NULL;
  return infer_expr_type(cg, scopes, depth, args->data[index].val);
}

static const char *ny_math_precise_numeric_type(const char *type_name) {
  if (!type_name)
    return NULL;
  if (is_float_type_name(type_name) || is_int_type_name(type_name) ||
      is_bigint_type_name(type_name))
    return type_name;
  return NULL;
}

static const char *ny_math_best_numeric_arg_type(codegen_t *cg, scope *scopes,
                                                 size_t depth,
                                                 ny_call_arg_list *args) {
  if (!args)
    return NULL;
  const char *int_type = NULL;
  for (size_t i = 0; i < args->len; i++) {
    const char *t = ny_math_call_arg_type(cg, scopes, depth, args, i);
    if (is_float_type_name(t))
      return t;
    if (is_bigint_type_name(t))
      return t;
    if (!int_type && is_int_type_name(t))
      int_type = "int";
  }
  return int_type;
}

static bool ny_math_any_arg_is_float(codegen_t *cg, scope *scopes, size_t depth,
                                     ny_call_arg_list *args) {
  if (!args)
    return false;
  for (size_t i = 0; i < args->len; i++) {
    const char *t = ny_math_call_arg_type(cg, scopes, depth, args, i);
    if (is_float_type_name(t))
      return true;
  }
  return false;
}

static bool ny_math_any_arg_is_bigint(codegen_t *cg, scope *scopes,
                                      size_t depth, ny_call_arg_list *args) {
  if (!args)
    return false;
  for (size_t i = 0; i < args->len; i++) {
    const char *t = ny_math_call_arg_type(cg, scopes, depth, args, i);
    if (is_bigint_type_name(t))
      return true;
  }
  return false;
}

static const char *infer_std_math_call_return_type(codegen_t *cg, scope *scopes,
                                                   size_t depth,
                                                   const char *name,
                                                   ny_call_arg_list *args) {
  if (!name)
    return NULL;
  if (ny_name_tail_is(name, "std.math.nt.mod"))
    return "bigint";
  if (ny_name_tail_is(name, "std.math.sqrt") ||
      ny_name_tail_is(name, "std.math.clamp01") ||
      ny_name_tail_is(name, "std.math.sin") ||
      ny_name_tail_is(name, "std.math.cos") ||
      ny_name_tail_is(name, "std.math.tan") ||
      ny_name_tail_is(name, "std.math.asin") ||
      ny_name_tail_is(name, "std.math.acos") ||
      ny_name_tail_is(name, "std.math.atan") ||
      ny_name_tail_is(name, "std.math.atan2") ||
      ny_name_tail_is(name, "std.math.exp") ||
      ny_name_tail_is(name, "std.math.log") ||
      ny_name_tail_is(name, "std.math.log2") ||
      ny_name_tail_is(name, "std.math.log10") ||
      ny_name_tail_is(name, "std.math.fmod") ||
      ny_name_tail_is(name, "std.math.floor") ||
      ny_name_tail_is(name, "std.math.ceil") ||
      ny_name_tail_is(name, "std.math.round"))
    return "f64";
  if (ny_name_tail_is(name, "std.math.sign"))
    return "int";
  if (ny_name_tail_is(name, "std.math.clamp") ||
      ny_name_tail_is(name, "std.math.min") ||
      ny_name_tail_is(name, "std.math.max")) {
    const char *best = ny_math_best_numeric_arg_type(cg, scopes, depth, args);
    return best ? best : "number";
  }
  if (ny_name_tail_is(name, "std.math.abs")) {
    const char *t = ny_math_call_arg_type(cg, scopes, depth, args, 0);
    const char *precise = ny_math_precise_numeric_type(t);
    return precise ? precise : "number";
  }
  if (ny_name_tail_is(name, "std.math.gcd") ||
      ny_name_tail_is(name, "std.math.lcm") ||
      ny_name_tail_is(name, "std.math.factorial")) {
    const char *t = ny_math_call_arg_type(cg, scopes, depth, args, 0);
    if (is_bigint_type_name(t))
      return t;
    if (is_int_type_name(t))
      return "int";
    return "number";
  }
  if (ny_name_tail_is(name, "std.math.mod")) {
    const char *t = ny_math_call_arg_type(cg, scopes, depth, args, 0);
    if (is_bigint_type_name(t) ||
        ny_math_any_arg_is_bigint(cg, scopes, depth, args))
      return "bigint";
    if (is_float_type_name(t) ||
        ny_math_any_arg_is_float(cg, scopes, depth, args))
      return "f64";
    if (is_int_type_name(t))
      return "int";
    return "number";
  }
  if (ny_name_tail_is(name, "std.math.pow") ||
      ny_name_tail_is(name, "std.math.lerp"))
    return ny_math_any_arg_is_float(cg, scopes, depth, args) ? "f64" : "number";
  return NULL;
}

static expr_t *ny_typeinfer_static_indexable_init(codegen_t *cg, scope *scopes,
                                                  size_t depth,
                                                  expr_t *target) {
  if (ny_expr_is_list_or_tuple_lit(target))
    return target;
  if (!scopes || !target || target->kind != NY_E_IDENT ||
      !target->as.ident.name)
    return NULL;
  size_t name_len = (size_t)target->tok.len;
  if (name_len == 0)
    name_len = strlen(target->as.ident.name);
  binding *b = type_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                   name_len, target->as.ident.hash);
  expr_t *init = ny_binding_static_indexable_lit(b);
  if (init)
    return init;
  return NULL;
}

static bool ny_literal_is_small_int(expr_t *e) {
  const int64_t ny_small_int_min = INT64_C(-4611686018427387904);
  const int64_t ny_small_int_max = INT64_C(4611686018427387903);
  return e && e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
         e->tok.kind != NY_T_NIL && e->as.literal.as.i >= ny_small_int_min &&
         e->as.literal.as.i <= ny_small_int_max;
}

static const char *ny_static_indexable_elem_type(scope *scopes, size_t depth,
                                                 expr_t *target) {
  expr_t *init =
      ny_typeinfer_static_indexable_init(NULL, scopes, depth, target);
  if (!init || init->as.list_like.len == 0)
    return NULL;
  const char *elem_type = NULL;
  for (size_t i = 0; i < init->as.list_like.len; ++i) {
    expr_t *item = init->as.list_like.data[i];
    if (ny_literal_is_small_int(item)) {
      if (!elem_type)
        elem_type = "int";
      else if (strcmp(elem_type, "int") != 0)
        return NULL;
      continue;
    }
    if (item && item->kind == NY_E_LITERAL &&
        item->as.literal.kind == NY_LIT_FLOAT) {
      if (!elem_type)
        elem_type = "f64";
      else if (strcmp(elem_type, "f64") != 0)
        return NULL;
      continue;
    }
    return NULL;
  }
  return elem_type;
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

static bool type_is_builtin_tagged_object(const char *name) {
  switch (classify_builtin_type_tail(name)) {
  case NY_BT_STR:
  case NY_BT_BOOL:
  case NY_BT_RESULT:
  case NY_BT_LIST:
  case NY_BT_DICT:
  case NY_BT_TUPLE:
  case NY_BT_SEQ:
  case NY_BT_SET:
  case NY_BT_BYTES:
  case NY_BT_RANGE:
  case NY_BT_BIGINT:
    return true;
  default:
    return false;
  }
}

static bool type_is_runtime_object_expr_type(const char *name) {
  if (!name || !*name)
    return false;
  if (is_ptr_type_name(name))
    return true;
  return type_is_builtin_tagged_object(name);
}

static bool typed_object_accepts_runtime_object(codegen_t *cg, const char *want,
                                                const char *got) {
  if (!want || !got)
    return false;
  const char *want_base = type_skip_nullable(want);
  const char *got_base = type_skip_nullable(got);
  if (is_ptr_type_name(want_base) && lookup_layout(cg, got_base))
    return true;
  if (!type_is_runtime_object_expr_type(got_base))
    return false;
  if (type_is_builtin_tagged_object(want_base))
    return true;
  return ny_lookup_tagged_type(cg, want_base);
}

static bool report_nil_type_mismatch(codegen_t *cg, token_t tok,
                                     const char *want, const char *ctx) {
  ny_diag_error(tok, "cannot use nil for %s %s", ny_name_leaf(want),
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
    return val >= 0 && val <= 255;
  case NY_BT_U16:
    return val >= 0 && val <= 65535;
  case NY_BT_U32:
    return val >= 0 && val <= 4294967295LL;
  case NY_BT_U64:
  case NY_BT_U128:
    return val >=
           0; /* already checked by parser strtoull range, but check sign */
  default:
    return true;
  }
}

static bool type_compatible_non_nullable(const char *want, const char *got) {
  if (!want || !got)
    return true;
  if (is_any_type_name(want) || is_any_type_name(got))
    return true;
  if (type_name_eq(want, got))
    return true;
  const char *want_gen = strchr(want, '<');
  const char *got_gen = strchr(got, '<');
  if (want_gen || got_gen) {
    size_t want_base_len = want_gen ? (size_t)(want_gen - want) : strlen(want);
    size_t got_base_len = got_gen ? (size_t)(got_gen - got) : strlen(got);
    if (want_base_len == got_base_len &&
        strncmp(want, got, want_base_len) == 0) {
      if (!want_gen || !got_gen)
        return true;
    }
  }
  int want_vec = vector_type_dim(want);
  int got_vec = vector_type_dim(got);
  if (want_vec && got_vec && want_vec == got_vec)
    return true;
  if (want_vec) {
    ny_builtin_type_kind_t got_kind = classify_builtin_type_tail(got);
    if (got_kind == NY_BT_LIST || got_kind == NY_BT_DICT)
      return true;
  }
  if (is_result_type_name(want) && is_result_type_name(got))
    return true;
  if (is_type_group_name(want) && type_group_accepts_type(want, got))
    return true;
  if (is_type_group_name(got) && type_group_accepts_type(got, want))
    return true;
  if (is_seq_type_name(want) && is_seq_compatible_type_name(got))
    return true;
  if (is_number_type_name(want) &&
      (is_int_type_name(got) || is_float_type_name(got) ||
       is_number_type_name(got) || is_bigint_type_name(got)))
    return true;
  if (is_ptr_type_name(want) && is_ptr_type_name(got))
    return true;
  if (is_int_type_name(want) && is_int_type_name(got)) {
    if (type_name_eq(want, "int") || type_name_eq(got, "int"))
      return true;
  }
  if (is_ptr_type_name(want) && type_name_eq(got, "int"))
    return true;
  if (type_name_eq(want, "int") && is_ptr_type_name(got))
    return true;

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
  if (!want_nullable && got_nullable)
    return false;
  return type_compatible_non_nullable(want_base, got_base);
}

static const char *infer_comptime_stmt_type(codegen_t *cg, scope *scopes,
                                            size_t depth, stmt_t *s) {
  if (!s)
    return NULL;
  switch (s->kind) {
  case NY_S_RETURN:
    return s->as.ret.value ? infer_expr_type(cg, scopes, depth, s->as.ret.value)
                           : "nil";
  case NY_S_EXPR:
    return infer_expr_type(cg, scopes, depth, s->as.expr.expr);
  case NY_S_BLOCK:
    for (size_t i = s->as.block.body.len; i > 0; --i) {
      const char *t = infer_comptime_stmt_type(cg, scopes, depth,
                                               s->as.block.body.data[i - 1]);
      if (t)
        return t;
    }
    return NULL;
  case NY_S_IF: {
    const char *a =
        infer_comptime_stmt_type(cg, scopes, depth, s->as.iff.conseq);
    const char *b = infer_comptime_stmt_type(cg, scopes, depth, s->as.iff.alt);
    return (a && b && strcmp(a, b) == 0) ? a : NULL;
  }
  default:
    return NULL;
  }
}

static const char *infer_expr_type_uncached(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *e) {
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
      case NY_LIT_HINT_I128:
        return "i128";
      case NY_LIT_HINT_U8:
        return "u8";
      case NY_LIT_HINT_U16:
        return "u16";
      case NY_LIT_HINT_U32:
        return "u32";
      case NY_LIT_HINT_U64:
        return "u64";
      case NY_LIT_HINT_U128:
        return "u128";
      default:
        return "int";
      }
    }
    return NULL;
  case NY_E_IDENT: {
    if (scopes) {
      size_t name_len = (size_t)e->tok.len;
      if (name_len == 0)
        name_len = strlen(e->as.ident.name);
      binding *b = type_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                       name_len, e->as.ident.hash);
      if (b) {
        if (b->type_name && !is_int_type_name(b->type_name) &&
            !is_float_type_name(b->type_name))
          return b->type_name;
        /* Check type inference flags first */
        if (b->is_int_slot || b->is_int_direct)
          return "int";
        if (b->is_f64_slot || b->is_f64_direct)
          return "f64";
        if (b->type_name)
          return b->type_name;
        if (!b->is_mut) {
          expr_t *init = ny_binding_var_init_expr(b, e->as.ident.name);
          const char *init_type = infer_expr_type(cg, scopes, depth, init);
          if (init_type)
            return init_type;
        }
        return NULL;
      }
    }
    fun_sig *sig = lookup_fun(cg, e->as.ident.name, e->as.ident.hash);
    if (sig)
      return "fnptr";
    const char *enum_owner = type_enum_member_owner_name(cg, e);
    if (enum_owner)
      return enum_owner;
    return NULL;
  }
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const char *n = e->as.call.callee->as.ident.name;
      uint64_t h = e->as.call.callee->as.ident.hash;
      bool builtin_shadowed =
          type_call_builtin_name_shadowed(cg, scopes, depth, e->as.call.callee);
      enum_def_t *enum_owner = NULL;
      enum_member_def_t *enum_member =
          lookup_enum_member_owner(cg, n, &enum_owner);
      if (enum_member && enum_owner && enum_member->has_payload)
        return ny_infer_generic_adt_return_type(cg, scopes, depth, enum_owner,
                                                enum_member, &e->as.call.args);
      if (!builtin_shadowed && e->as.call.args.len == 0 &&
          call_name_is_int_zeroarg_builtin(n))
        return "int";
      const char *collection_type =
          builtin_shadowed ? NULL : infer_builtin_collection_call_type(n);
      if (collection_type)
        return collection_type;
      if (!builtin_shadowed && e->as.call.args.len == 1 &&
          (call_name_tail_is(n, "ok") || strcmp(n, "__result_ok") == 0)) {
        const char *ok_type =
            infer_expr_type(cg, scopes, depth, e->as.call.args.data[0].val);
        return type_infer_make_result(cg, ok_type, "any");
      }
      if (!builtin_shadowed && e->as.call.args.len == 1 &&
          (call_name_tail_is(n, "err") || strcmp(n, "__result_err") == 0)) {
        const char *err_type =
            infer_expr_type(cg, scopes, depth, e->as.call.args.data[0].val);
        return type_infer_make_result(cg, "any", err_type);
      }
      if (!builtin_shadowed && e->as.call.args.len == 1 &&
          (call_name_tail_is(n, "unwrap") || strcmp(n, "__unwrap") == 0)) {
        const char *arg_type =
            infer_expr_type(cg, scopes, depth, e->as.call.args.data[0].val);
        const char *ok_type = type_result_payload_type(cg, arg_type, true);
        if (ok_type)
          return ok_type;
      }
      if (!builtin_shadowed && e->as.call.args.len == 2 &&
          call_name_tail_is(n, "unwrap_or")) {
        const char *arg_type =
            infer_expr_type(cg, scopes, depth, e->as.call.args.data[0].val);
        const char *ok_type = type_result_payload_type(cg, arg_type, true);
        if (ok_type)
          return ok_type;
      }
      const char *vec_type =
          builtin_shadowed ? NULL : vector_constructor_return_type(n);
      if (vec_type)
        return vec_type;
      if (!builtin_shadowed &&
          (call_name_tail_is(n, "malloc") ||
           call_name_tail_is(n, "malloc_raw") ||
           call_name_tail_is(n, "zalloc") || call_name_tail_is(n, "realloc") ||
           strcmp(n, "__malloc") == 0 || strcmp(n, "__malloc_raw") == 0 ||
           strcmp(n, "__realloc") == 0))
        return "ptr";
      if (!builtin_shadowed &&
          (call_name_tail_is(n, "type") || call_name_tail_is(n, "type_name")))
        return "str";
      if (!builtin_shadowed &&
          (call_name_tail_is(n, "float") || call_name_tail_is(n, "to_float") ||
           call_name_tail_is(n, "f64")))
        return "f64";
      if (!builtin_shadowed && call_name_tail_is(n, "f64buf_load"))
        return "f64";
      if (!builtin_shadowed && call_name_tail_is(n, "f32buf_load"))
        return "f32";
      if (!builtin_shadowed &&
          (strcmp(n, "__load64_h") == 0 || call_name_tail_is(n, "load64_h")))
        return "handle";
      if (!builtin_shadowed && call_name_tail_is(n, "load64_i"))
        return "int";
      if (!builtin_shadowed &&
          (call_name_is_int_length_builtin(n) || strcmp(n, "__tagof") == 0 ||
           strcmp(n, "__runtime_tag") == 0 || strcmp(n, "__tag") == 0 ||
           strcmp(n, "__load8_idx") == 0 ||
           strcmp(n, "__load16_idx") == 0 || strcmp(n, "__load32_idx") == 0 ||
           strcmp(n, "__load64_idx") == 0 || call_name_tail_is(n, "load8") ||
           call_name_tail_is(n, "load16") || call_name_tail_is(n, "load32") ||
           call_name_tail_is(n, "load64") || call_name_tail_is(n, "load32_h") ||
           call_name_tail_is(n, "dict_len") ||
           call_name_tail_is(n, "type_tag") ||
           call_name_tail_is(n, "from_int") || strcmp(n, "__add") == 0 ||
           strcmp(n, "__sub") == 0 || strcmp(n, "__mul") == 0))
        return "int";
      if (!builtin_shadowed && call_name_is_bool_builtin(n))
        return "bool";
      if (!builtin_shadowed &&
          (call_name_tail_is(n, "to_int") || strcmp(n, "__untag") == 0))
        return NULL;
      bool want_builtin_get =
          !builtin_shadowed &&
          (strcmp(n, "get") == 0 || strcmp(n, "std.core.get") == 0 ||
           strcmp(n, "std.core.reflect.get") == 0 ||
           call_name_tail_is(n, "get"));
      if (want_builtin_get && e->as.call.args.len >= 2) {
        expr_t *target = e->as.call.args.data[0].val;
        if (target && target->kind == NY_E_IDENT && target->as.ident.name) {
          size_t name_len = (size_t)target->tok.len;
          if (name_len == 0)
            name_len = strlen(target->as.ident.name);
          binding *b =
              type_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                  name_len, target->as.ident.hash);
          if (b && b->is_int_list_storage)
            return "int";
          const char *elem_from_binding =
              b ? type_index_result_from_container_type(cg, b->type_name)
                : NULL;
          if (elem_from_binding)
            return elem_from_binding;
        }
        const char *target_type = infer_expr_type(cg, scopes, depth, target);
        const char *elem_from_type =
            type_index_result_from_container_type(cg, target_type);
        if (elem_from_type)
          return elem_from_type;
        expr_t *init =
            ny_typeinfer_static_indexable_init(cg, scopes, depth, target);
        const char *elem_type = NULL;
        if (init)
          elem_type = ny_static_indexable_elem_type(scopes, depth, init);
        if (elem_type)
          return elem_type;
      }
      fun_sig *sig = lookup_fun(cg, n, h);
      if (sig) {
        const char *math_ret = infer_std_math_call_return_type(
            cg, scopes, depth, sig->name ? sig->name : n, &e->as.call.args);
        if (!math_ret && n && !strchr(n, '.')) {
          char math_name[256];
          int mn = snprintf(math_name, sizeof(math_name), "std.math.%s", n);
          if (mn > 0 && (size_t)mn < sizeof(math_name) &&
              lookup_fun(cg, math_name, 0) == sig)
            math_ret = infer_std_math_call_return_type(
                cg, scopes, depth, math_name, &e->as.call.args);
        }
        if (math_ret)
          return math_ret;
        if (sig->return_type)
          return sig->return_type;
        if (sig->inferred_return_type)
          return sig->inferred_return_type;
      }
      if (e->as.call.args.len == 1 && ny_lookup_tagged_type(cg, n))
        return n;
    }
    if (e->as.call.callee && e->as.call.callee->kind != NY_E_IDENT) {
      fun_sig *sig =
          type_lookup_static_call_sig(cg, scopes, depth, e->as.call.callee,
                                      e->as.call.args.len);
      const char *ret = type_sig_return_name(sig);
      if (ret)
        return ret;
    }
    return NULL;
  case NY_E_INDEX: {
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name && e->as.index.start &&
        ny_is_proven_int(cg, scopes, depth, e->as.index.start, NULL)) {
      expr_t *target = e->as.index.target;
      size_t name_len = (size_t)target->tok.len;
      if (name_len == 0)
        name_len = strlen(target->as.ident.name);
      binding *b = type_lookup_binding(cg, scopes, depth,
                                        target->as.ident.name, name_len,
                                        target->as.ident.hash);
      if (b && b->is_int_list_storage)
        return "int";
    }
    const char *target_type =
        infer_expr_type(cg, scopes, depth, e->as.index.target);
    const char *elem_from_type =
        type_index_result_from_container_type(cg, target_type);
    if (elem_from_type)
      return elem_from_type;
    expr_t *init = ny_typeinfer_static_indexable_init(cg, scopes, depth,
                                                      e->as.index.target);
    const char *elem_type = NULL;
    if (init)
      elem_type = ny_static_indexable_elem_type(scopes, depth, init);
    return elem_type;
  }
  case NY_E_LIST:
    return "list";
  case NY_E_TUPLE:
    return "tuple";
  case NY_E_DICT:
    return "dict";
  case NY_E_SET:
    return "set";
  case NY_E_MEMCALL:
    if (e->as.memcall.target && e->as.memcall.name) {
      char module_path[1024];
      if (ny_resolve_module_expr_path(cg, scopes, depth, e->as.memcall.target,
                                      module_path, sizeof(module_path))) {
        char resolved_fun[1280];
        if (ny_resolve_module_function_path(cg, module_path, e->as.memcall.name,
                                            resolved_fun,
                                            sizeof(resolved_fun))) {
          fun_sig *sig = lookup_fun(cg, resolved_fun, 0);
          const char *ret = type_sig_return_name(sig);
          if (ret)
            return ret;
        }
      }
      const char *target_type =
          infer_expr_type(cg, scopes, depth, e->as.memcall.target);
      fun_sig *sig = type_lookup_attached_method(
          cg, target_type ? target_type : "any", e->as.memcall.name);
      if (!sig && target_type)
        sig = type_lookup_attached_method(cg, "any", e->as.memcall.name);
      if (sig) {
        if (sig->return_type)
          return sig->return_type;
        if (sig->inferred_return_type)
          return sig->inferred_return_type;
      }
      if (e->as.memcall.target->kind == NY_E_IDENT) {
        char dotted[256];
        const char *target = e->as.memcall.target->as.ident.name;
        snprintf(dotted, sizeof(dotted), "%s.%s", target, e->as.memcall.name);
        enum_def_t *enum_owner = NULL;
        enum_member_def_t *enum_member =
            lookup_enum_member_owner(cg, dotted, &enum_owner);
        if (enum_member && enum_owner && enum_member->has_payload)
          return ny_infer_generic_adt_return_type(
              cg, scopes, depth, enum_owner, enum_member, &e->as.memcall.args);
        char resolved_fun[512];
        if (ny_resolve_module_function_path(cg, target, e->as.memcall.name,
                                            resolved_fun, sizeof(resolved_fun)))
          sig = lookup_fun(cg, resolved_fun, 0);
        else
          sig = lookup_fun(cg, dotted, 0);
        if (sig && sig->return_type)
          return sig->return_type;
      }
    }
    return NULL;
  case NY_E_MEMBER: {
    const char *enum_owner = type_enum_member_owner_name(cg, e);
    if (enum_owner)
      return enum_owner;
  }
    if (e->as.member.name && e->as.member.target) {
      if (strcmp(e->as.member.name, "long") == 0)
        return "integer";
      const char *module_alias_type =
          type_member_module_alias_global_type(cg, scopes, depth, e);
      if (module_alias_type)
        return module_alias_type;
      char module_path[1024];
      char resolved_fun[1280];
      if (ny_resolve_module_expr_path(cg, scopes, depth, e->as.member.target,
                                      module_path, sizeof(module_path)) &&
          ny_resolve_module_function_path(cg, module_path, e->as.member.name,
                                          resolved_fun, sizeof(resolved_fun)))
        return "fnptr";
      const char *target_type =
          infer_expr_type(cg, scopes, depth, e->as.member.target);
      const char *owner = type_attached_owner(target_type);
      const char *owner_leaf = ny_name_leaf(owner ? owner : target_type);
      if (!owner_leaf)
        owner_leaf = owner ? owner : target_type;
      if (owner_leaf && strcmp(owner_leaf, "nil") == 0)
        return "nil";
      int vec_dim = vector_type_dim(target_type);
      if (vec_dim > 0) {
        const char *member = e->as.member.name;
        if (strcmp(member, "x") == 0 || strcmp(member, "y") == 0 ||
            (vec_dim >= 3 && strcmp(member, "z") == 0) ||
            (vec_dim >= 4 && strcmp(member, "w") == 0))
          return "f64";
      }
      const char *dict_value =
          type_generic_arg_type(cg, target_type, "dict", 1);
      if (dict_value)
        return dict_value;
      layout_def_t *layout = owner ? lookup_layout(cg, owner) : NULL;
      if (layout) {
        for (size_t i = 0; i < layout->fields.len; ++i) {
          layout_field_info_t *field = &layout->fields.data[i];
          if (field->name && strcmp(field->name, e->as.member.name) == 0)
            return field->type_name;
        }
      }
      fun_sig *sig = type_lookup_attached_method(
          cg, target_type ? target_type : "any", e->as.member.name);
      if (!sig && target_type)
        sig = type_lookup_attached_method(cg, "any", e->as.member.name);
      if (ny_sig_allows_zero_arg_property(sig))
        return type_sig_return_name(sig);
    }
    if (e->as.member.name && strcmp(e->as.member.name, "len") == 0)
      return "int";
    return NULL;
  case NY_E_LAMBDA:
  case NY_E_FN:
    return "fnptr";
  case NY_E_BINARY: {
    const char *lt = infer_expr_type(cg, scopes, depth, e->as.binary.left);
    const char *rt = infer_expr_type(cg, scopes, depth, e->as.binary.right);
    const char *op = e->as.binary.op;
    bool is_arith = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                    strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                    strcmp(op, "%") == 0 || strcmp(op, "^") == 0;
    bool is_bitwise = strcmp(op, "|") == 0 || strcmp(op, "&") == 0 ||
                      strcmp(op, "^^") == 0 || strcmp(op, "<<") == 0 ||
                      strcmp(op, ">>") == 0;
    bool is_cmp = strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                  strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                  strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
    bool l_int = lt && strcmp(lt, "int") == 0;
    bool r_int = rt && strcmp(rt, "int") == 0;
    bool l_flt = lt && (strcmp(lt, "f64") == 0 || strcmp(lt, "f32") == 0);
    bool r_flt = rt && (strcmp(rt, "f64") == 0 || strcmp(rt, "f32") == 0);
    int l_vec = vector_type_dim(lt);
    int r_vec = vector_type_dim(rt);
    const char *scoped_ret = infer_scoped_operator_type(cg, op, lt, rt);
    if (scoped_ret)
      return scoped_ret;
    if (l_vec || r_vec) {
      if (l_vec && r_vec && l_vec == r_vec) {
        if (strcmp(op, "*") == 0)
          return "f64";
        if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0)
          return vector_type_name_for_dim(l_vec);
      }
      if (l_vec && (strcmp(op, "*") == 0 || strcmp(op, "/") == 0) &&
          (!rt || r_int || r_flt))
        return vector_type_name_for_dim(l_vec);
      if (r_vec && strcmp(op, "*") == 0 && (!lt || l_int || l_flt))
        return vector_type_name_for_dim(r_vec);
      if (is_cmp)
        return "bool";
    }
    if (l_int && r_int) {
      if (is_arith || is_bitwise)
        return "int";
      if (is_cmp)
        return "bool";
    }
    if ((l_flt || r_flt) && (is_arith || is_cmp)) {
      bool l_ok = l_flt || l_int || !lt;
      bool r_ok = r_flt || r_int || !rt;
      if (l_ok && r_ok) {
        if (is_arith)
          return "f64";
        if (is_cmp)
          return "bool";
      }
    }
    return NULL;
  }
  case NY_E_UNARY: {
    const char *rt = infer_expr_type(cg, scopes, depth, e->as.unary.right);
    if (!rt)
      return NULL;
    const char *op = e->as.unary.op;
    if (strcmp(op, "async") == 0)
      return "handle";
    if (strcmp(op, "await") == 0)
      return "any";
    if (strcmp(op, "!") == 0)
      return "bool";
    if ((strcmp(op, "-") == 0 || strcmp(op, "+") == 0 ||
         strcmp(op, "~") == 0) &&
        strcmp(rt, "int") == 0)
      return "int";
    return NULL;
  }
  case NY_E_TERNARY: {
    const char *tt =
        infer_expr_type(cg, scopes, depth, e->as.ternary.true_expr);
    const char *ft =
        infer_expr_type(cg, scopes, depth, e->as.ternary.false_expr);
    if (!tt || !ft)
      return NULL;
    if (strcmp(tt, ft) == 0)
      return tt;
    return NULL;
  }
  case NY_E_COMPTIME:
    return infer_comptime_stmt_type(cg, scopes, depth,
                                    e->as.comptime_expr.body);
  default:
    return NULL;
  }
}

const char *infer_expr_type(codegen_t *cg, scope *scopes, size_t depth,
                            expr_t *e) {
  return infer_expr_type_uncached(cg, scopes, depth, e);
}

bool ensure_expr_type_compatible(codegen_t *cg, scope *scopes, size_t depth,
                                 const char *want, expr_t *expr, token_t tok,
                                 const char *ctx) {
  if (!cg || !want || !*want || !expr)
    return true;
  if (is_any_type_name(want))
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
      if (is_number_type_name(want))
        return true;
      /* Allow integer literals as ptr (null pointer idiom for extern calls) */
      if (strcmp(want, "ptr") == 0)
        return true;
      if (is_bool_type_name(want) || is_str_type_name(want) ||
          is_result_type_name(want)) {
        ny_diag_error(tok, "cannot assign integer literal to %s", want);
        cg->had_error = 1;
        return false;
      }
    } else if (expr->as.literal.kind == NY_LIT_FLOAT) {
      if (is_float_type_name(want) || is_number_type_name(want))
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
      if (is_seq_type_name(want))
        return true;
      if (is_char_type_name(want)) {
        if (expr->as.literal.as.s.len == 1)
          return true;
        ny_diag_error(tok, "char literal must be a single character");
        cg->had_error = 1;
        return false;
      }
      /* Allow string literals as ptr (C const char*) for extern fn calls */
      if (strcmp(want, "ptr") == 0 || strcmp(want, "i64") == 0)
        return true;
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
  if (typed_object_accepts_runtime_object(cg, want, got))
    return true;
  if (!type_compatible_simple(want, got)) {
    const char *want_name = ny_name_leaf(want);
    const char *got_name = ny_name_leaf(got);
    ny_diag_type_mismatch(tok, want_name ? want_name : want,
                          got_name ? got_name : got,
                          (ctx && *ctx) ? ctx : "assignment");
    cg->had_error = 1;
    return false;
  }
  return true;
}

bool ny_expr_type_compatible(codegen_t *cg, scope *scopes, size_t depth,
                             const char *want, expr_t *expr) {
  if (!cg || !want || !*want || !expr)
    return false;
  if (is_any_type_name(want))
    return true;
  const char *got = infer_expr_type(cg, scopes, depth, expr);
  if (!got)
    return false;
  if (is_nil_type_name(got))
    return nil_assignable_to_type(want);
  if (typed_object_accepts_runtime_object(cg, want, got))
    return true;
  return type_compatible_simple(want, got);
}

LLVMTypeRef resolve_type_name(codegen_t *cg, const char *type_name,
                              token_t tok) {
  if (!type_name || !*type_name) {
    return cg->type_i64;
  }
  const char *resolved_name = type_skip_nullable(type_name);
  if (is_any_type_name(resolved_name))
    return cg->type_i64;
  if (ny_name_tail_is(resolved_name, "number"))
    return cg->type_i64;
  if (resolved_name[0] == '*') {
    return cg->type_i64;
  }
  if (is_vector_type_name(resolved_name))
    return cg->type_i64;
  switch (classify_builtin_type_tail(resolved_name)) {
  case NY_BT_INT:
  case NY_BT_NUMBER:
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
  case NY_BT_C64:
  case NY_BT_C128:
  case NY_BT_COMPLEX:
  case NY_BT_RESULT:
  case NY_BT_PTR:
  case NY_BT_CSTR:
  case NY_BT_FNPTR:
  case NY_BT_HANDLE:
  case NY_BT_LIST:
  case NY_BT_DICT:
  case NY_BT_TUPLE:
  case NY_BT_SEQ:
  case NY_BT_SET:
  case NY_BT_BYTES:
  case NY_BT_RANGE:
  case NY_BT_BIGINT:
  case NY_BT_INTEGER:
  case NY_BT_FLOAT:
  case NY_BT_SCALAR:
  case NY_BT_COLLECTION:
  case NY_BT_CONTAINER:
  case NY_BT_ITERABLE:
  case NY_BT_INDEXABLE:
  case NY_BT_ALLOCATOR:
    return cg->type_i64;
  default:
    break;
  }
  layout_def_t *layout = lookup_layout(cg, resolved_name);
  if (layout) {
    if (layout->llvm_type)
      return layout->llvm_type;
    return cg->type_i64;
  }
  if (ny_lookup_tagged_type(cg, resolved_name))
    return cg->type_i64;
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
  if (is_any_type_name(resolved_name))
    return cg->type_i64;
  if (ny_name_tail_is(resolved_name, "number"))
    return cg->type_i64;
  if (strcmp(resolved_name, "fnptr") == 0) {
    return cg->type_i8ptr;
  }
  if (resolved_name[0] == '*') {
    return cg->type_i8ptr;
  }
  if (is_vector_type_name(resolved_name))
    return cg->type_i64;
  switch (classify_builtin_type_tail(resolved_name)) {
  case NY_BT_INT:
  case NY_BT_NUMBER:
  case NY_BT_STR:
  case NY_BT_BOOL:
  case NY_BT_RESULT:
  case NY_BT_LIST:
  case NY_BT_DICT:
  case NY_BT_TUPLE:
  case NY_BT_SEQ:
  case NY_BT_SET:
  case NY_BT_BYTES:
  case NY_BT_RANGE:
  case NY_BT_BIGINT:
  case NY_BT_INTEGER:
  case NY_BT_FLOAT:
  case NY_BT_SCALAR:
  case NY_BT_COLLECTION:
  case NY_BT_CONTAINER:
  case NY_BT_ITERABLE:
  case NY_BT_INDEXABLE:
  case NY_BT_ALLOCATOR:
    return cg->type_i64;
  case NY_BT_PTR:
  case NY_BT_CSTR:
  case NY_BT_FNPTR:
    return cg->type_i8ptr;
  case NY_BT_HANDLE:
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
  case NY_BT_C64:
  case NY_BT_C128:
  case NY_BT_COMPLEX:
    return complex_abi_type(cg, resolved_name);
  default:
    break;
  }
  layout_def_t *layout = lookup_layout(cg, resolved_name);
  if (layout)
    return layout->llvm_type ? ny_layout_abi_carrier_type(cg, layout)
                             : cg->type_i64;
  if (is_opaque_system_abi_type(resolved_name))
    return cg->type_i8ptr;
  if (ny_lookup_tagged_type(cg, resolved_name))
    return cg->type_i64;
  ny_diag_error(tok, "unknown type name '%s' at native ABI boundary",
                type_name);
  ny_diag_hint("use a builtin ABI type, a layout/layout record, or a struct "
               "type visible in this session");
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
  if (cg->current_module_name && !strchr(name, '.')) {
    const char *qname = codegen_qname(cg, name, cg->current_module_name);
    for (size_t i = 0; i < cg->layouts.len; i++) {
      layout_def_t *def = cg->layouts.data[i];
      if (def && def->name && strcmp(def->name, qname) == 0)
        return def;
    }
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
  if (is_any_type_name(resolved_name))
    return make_layout(8, 8, cg->type_i64);
  if (ny_name_tail_is(resolved_name, "number"))
    return make_layout(8, 8, cg->type_i64);
  if (strcmp(resolved_name, "fnptr") == 0) {
    return make_layout(ptr_size(), ptr_size(), cg->type_i8ptr);
  }
  if (strcmp(resolved_name, "cstr") == 0) {
    return make_layout(ptr_size(), ptr_size(), cg->type_i8ptr);
  }
  if (resolved_name[0] == '*') {
    return make_layout(ptr_size(), ptr_size(), cg->type_i8ptr);
  }
  switch (classify_builtin_type_exact(resolved_name)) {
  case NY_BT_INT:
  case NY_BT_RESULT:
  case NY_BT_LIST:
  case NY_BT_DICT:
  case NY_BT_TUPLE:
  case NY_BT_SEQ:
  case NY_BT_SET:
  case NY_BT_BYTES:
  case NY_BT_RANGE:
  case NY_BT_BIGINT:
    return make_layout(8, 8, cg->type_i64);
  case NY_BT_PTR:
  case NY_BT_CSTR:
  case NY_BT_FNPTR:
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
  case NY_BT_C64:
    return make_layout(8, 4, complex_abi_type(cg, resolved_name));
  case NY_BT_C128:
  case NY_BT_COMPLEX:
    return make_layout(16, 8, complex_abi_type(cg, resolved_name));
  default:
    break;
  }
  layout_def_t *layout = lookup_layout(cg, resolved_name);
  if (layout) {
    return make_layout(layout->size, layout->align, layout->llvm_type);
  }
  if (ny_lookup_tagged_type(cg, resolved_name))
    return make_layout(8, 8, cg->type_i64);
  ny_diag_error(tok, "unknown type name '%s' in layout", type_name);
  cg->had_error = 1;
  return invalid;
}

typedef struct {
  const char *name;
} ny_type_param_int_t;

enum { NY_TYPE_PROVEN_INT_CALL_STACK_MAX = 64 };

static _Thread_local const fun_sig
    *g_type_proven_int_call_stack[NY_TYPE_PROVEN_INT_CALL_STACK_MAX];
static _Thread_local size_t g_type_proven_int_call_stack_len;

static bool type_proven_int_call_active(const fun_sig *sig) {
  if (!sig)
    return false;
  for (size_t i = 0; i < g_type_proven_int_call_stack_len; ++i)
    if (g_type_proven_int_call_stack[i] == sig)
      return true;
  return false;
}

static bool type_proven_int_call_push(const fun_sig *sig) {
  if (!sig || type_proven_int_call_active(sig) ||
      g_type_proven_int_call_stack_len >= NY_TYPE_PROVEN_INT_CALL_STACK_MAX)
    return false;
  g_type_proven_int_call_stack[g_type_proven_int_call_stack_len++] = sig;
  return true;
}

static void type_proven_int_call_pop(const fun_sig *sig) {
  if (g_type_proven_int_call_stack_len == 0)
    return;
  if (g_type_proven_int_call_stack[g_type_proven_int_call_stack_len - 1] ==
      sig) {
    g_type_proven_int_call_stack_len--;
    return;
  }
  for (size_t i = g_type_proven_int_call_stack_len; i > 0; --i) {
    if (g_type_proven_int_call_stack[i - 1] == sig) {
      memmove(&g_type_proven_int_call_stack[i - 1],
              &g_type_proven_int_call_stack[i],
              (g_type_proven_int_call_stack_len - i) *
                  sizeof(g_type_proven_int_call_stack[0]));
      g_type_proven_int_call_stack_len--;
      return;
    }
  }
}

static expr_t *type_single_return_expr(stmt_t *s) {
  if (!s)
    return NULL;
  if (s->kind == NY_S_RETURN)
    return s->as.ret.value;
  if (s->kind == NY_S_BLOCK && s->as.block.body.len == 1)
    return type_single_return_expr(s->as.block.body.data[0]);
  return NULL;
}

static bool type_expr_proven_int_with_params(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e,
                                             const ny_type_param_int_t *params,
                                             size_t param_count, int recursion);
static bool type_stmt_returns_proven_int_with_params(
    codegen_t *cg, scope *scopes, size_t depth, stmt_t *s,
    const ny_type_param_int_t *params, size_t param_count, int recursion);

static bool type_call_returns_proven_int(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *call,
                                         const ny_type_param_int_t *params,
                                         size_t param_count, int recursion,
                                         bool *out_static_user_call) {
  if (out_static_user_call)
    *out_static_user_call = false;
  if (!cg || !call || call->kind != NY_E_CALL || !call->as.call.callee ||
      recursion > 24)
    return false;
  if (call->as.call.args.len > 16)
    return false;
  fun_sig *sig = type_lookup_static_call_sig(cg, scopes, depth,
                                             call->as.call.callee,
                                             call->as.call.args.len);
  if (!sig || sig->is_extern || sig->is_variadic || sig->is_recursive ||
      !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return false;
  if (out_static_user_call)
    *out_static_user_call = true;
  if (sig->return_type && (strcmp(sig->return_type, "int") == 0 ||
                           strcmp(sig->return_type, "i64") == 0))
    return true;
  if (sig->stmt_t->as.fn.params.len < call->as.call.args.len)
    return false;
  ny_type_param_int_t local_params[16] = {0};
  for (size_t i = 0; i < call->as.call.args.len; ++i) {
    local_params[i].name = sig->stmt_t->as.fn.params.data[i].name;
    if (!local_params[i].name ||
        !type_expr_proven_int_with_params(cg, scopes, depth,
                                          call->as.call.args.data[i].val,
                                          params, param_count, recursion + 1))
      return false;
  }
  if (!type_proven_int_call_push(sig))
    return false;
  bool proven = false;
  if (type_stmt_returns_proven_int_with_params(
          cg, scopes, depth, sig->stmt_t->as.fn.body, local_params,
          call->as.call.args.len, recursion + 1)) {
    proven = true;
    goto done;
  }
  expr_t *ret = type_single_return_expr(sig->stmt_t->as.fn.body);
  if (!ret)
    goto done;
  proven = type_expr_proven_int_with_params(cg, scopes, depth, ret,
                                            local_params,
                                            call->as.call.args.len,
                                            recursion + 1);
done:
  type_proven_int_call_pop(sig);
  return proven;
}

static bool type_get_default_is_proven_int(codegen_t *cg, scope *scopes,
                                           size_t depth, expr_t *default_expr,
                                           int recursion) {
  if (!default_expr)
    return true; /* get(...): implicit default is 0 */
  return type_expr_proven_int_with_params(cg, scopes, depth, default_expr, NULL,
                                          0, recursion + 1);
}

static bool type_expr_proven_int_with_params(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e,
                                             const ny_type_param_int_t *params,
                                             size_t param_count,
                                             int recursion) {
  if (!e || recursion > 32)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return ny_literal_is_small_int(e);
  case NY_E_IDENT:
    if (e->as.ident.name) {
      for (size_t i = 0; i < param_count; ++i) {
        if (params[i].name && strcmp(params[i].name, e->as.ident.name) == 0)
          return true;
      }
    }
    return ny_is_proven_int(cg, scopes, depth, e, NULL);
  case NY_E_UNARY:
    return e->as.unary.op &&
           (strcmp(e->as.unary.op, "+") == 0 ||
            strcmp(e->as.unary.op, "-") == 0) &&
           type_expr_proven_int_with_params(cg, scopes, depth,
                                            e->as.unary.right, params,
                                            param_count, recursion + 1);
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    if (!op ||
        !type_expr_proven_int_with_params(cg, scopes, depth, e->as.binary.left,
                                          params, param_count, recursion + 1) ||
        !type_expr_proven_int_with_params(cg, scopes, depth, e->as.binary.right,
                                          params, param_count, recursion + 1))
      return false;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
        strcmp(op, "^") == 0 || strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^^") == 0 ||
        strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0)
      return true;
    if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0)
      return true;
    return false;
  }
  case NY_E_CALL:
    {
      bool static_user_call = false;
      bool proven = type_call_returns_proven_int(
          cg, scopes, depth, e, params, param_count, recursion + 1,
          &static_user_call);
      if (proven || static_user_call)
        return proven;
      return ny_is_proven_int(cg, scopes, depth, e, NULL);
    }
  case NY_E_MEMBER:
    return e->as.member.name && strcmp(e->as.member.name, "len") == 0;
  default:
    return false;
  }
}

static bool type_param_int_contains(const ny_type_param_int_t *params,
                                    size_t param_count, const char *name) {
  if (!params || !name)
    return false;
  for (size_t i = 0; i < param_count; ++i)
    if (params[i].name && strcmp(params[i].name, name) == 0)
      return true;
  return false;
}

static bool type_param_int_add(ny_type_param_int_t *params,
                               size_t *param_count, size_t cap,
                               const char *name) {
  if (!params || !param_count || !name)
    return false;
  if (type_param_int_contains(params, *param_count, name))
    return true;
  if (*param_count >= cap)
    return false;
  params[*param_count].name = name;
  (*param_count)++;
  return true;
}

static bool type_stmt_preserves_int_locals_with_params(
    codegen_t *cg, scope *scopes, size_t depth, stmt_t *s,
    ny_type_param_int_t *params, size_t *param_count, size_t cap,
    int recursion) {
  if (!s || recursion > 32)
    return false;
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (!type_stmt_preserves_int_locals_with_params(
              cg, scopes, depth, s->as.block.body.data[i], params,
              param_count, cap, recursion + 1))
        return false;
    }
    return true;
  }
  if (s->kind == NY_S_VAR) {
    stmt_var_t *var = &s->as.var;
    if (var->is_del || var->is_destructure || var->names.len != var->exprs.len)
      return false;
    for (size_t i = 0; i < var->names.len; ++i) {
      bool rhs_int = type_expr_proven_int_with_params(
          cg, scopes, depth, var->exprs.data[i], params, *param_count,
          recursion + 1);
      if (!rhs_int) {
        if (type_param_int_contains(params, *param_count, var->names.data[i]))
          return false;
        continue;
      }
      if (!type_param_int_add(params, param_count, cap, var->names.data[i]))
        return false;
    }
    return true;
  }
  if (s->kind == NY_S_RETURN)
    return type_expr_proven_int_with_params(cg, scopes, depth, s->as.ret.value,
                                            params, *param_count,
                                            recursion + 1);
  if (s->kind == NY_S_IF) {
    if (s->as.iff.init &&
        !type_stmt_preserves_int_locals_with_params(
            cg, scopes, depth, s->as.iff.init, params, param_count, cap,
            recursion + 1))
      return false;
    ny_type_param_int_t conseq_params[32] = {0};
    ny_type_param_int_t alt_params[32] = {0};
    if (*param_count > 32)
      return false;
    memcpy(conseq_params, params, *param_count * sizeof(*params));
    memcpy(alt_params, params, *param_count * sizeof(*params));
    size_t conseq_count = *param_count;
    size_t alt_count = *param_count;
    if (s->as.iff.conseq &&
        !type_stmt_preserves_int_locals_with_params(
            cg, scopes, depth, s->as.iff.conseq, conseq_params,
            &conseq_count, 32, recursion + 1))
      return false;
    if (s->as.iff.alt &&
        !type_stmt_preserves_int_locals_with_params(
            cg, scopes, depth, s->as.iff.alt, alt_params, &alt_count, 32,
            recursion + 1))
      return false;
    return true;
  }
  return false;
}

static bool type_stmt_returns_proven_int_with_params(
    codegen_t *cg, scope *scopes, size_t depth, stmt_t *s,
    const ny_type_param_int_t *params, size_t param_count, int recursion) {
  if (!s || recursion > 32)
    return false;
  if (s->kind == NY_S_RETURN)
    return type_expr_proven_int_with_params(cg, scopes, depth, s->as.ret.value,
                                            params, param_count,
                                            recursion + 1);
  if (s->kind == NY_S_EXPR)
    return type_expr_proven_int_with_params(cg, scopes, depth, s->as.expr.expr,
                                            params, param_count,
                                            recursion + 1);
  if (s->kind != NY_S_BLOCK || s->as.block.body.len == 0 || param_count > 32)
    return false;

  ny_type_param_int_t local_params[32] = {0};
  memcpy(local_params, params, param_count * sizeof(*params));
  size_t local_count = param_count;
  for (size_t i = 0; i < s->as.block.body.len; ++i) {
    stmt_t *child = s->as.block.body.data[i];
    if (i + 1 == s->as.block.body.len)
      return type_stmt_returns_proven_int_with_params(
          cg, scopes, depth, child, local_params, local_count, recursion + 1);
    if (!type_stmt_preserves_int_locals_with_params(
            cg, scopes, depth, child, local_params, &local_count, 32,
            recursion + 1))
      return false;
  }
  return false;
}

static bool ny_is_proven_int_inner(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e, LLVMValueRef v) {
  const int64_t ny_small_int_min = INT64_C(-4611686018427387904);
  const int64_t ny_small_int_max = INT64_C(4611686018427387903);
  if (v && LLVMIsAConstantInt(v)) {
    int64_t val = LLVMConstIntGetSExtValue(v);
    return (val & 1) != 0;
  }
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_INT &&
           e->as.literal.as.i >= ny_small_int_min &&
           e->as.literal.as.i <= ny_small_int_max;
  case NY_E_CALL: {
    bool builtin_shadowed =
        type_call_builtin_name_shadowed(cg, scopes, depth, e->as.call.callee);
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.args.len == 0) {
      const char *n = e->as.call.callee->as.ident.name;
      if (!n)
        return false;
      if (!builtin_shadowed && call_name_is_int_zeroarg_builtin(n))
        return true;
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.args.len >= 2) {
      const char *n = e->as.call.callee->as.ident.name;
      if (!n)
        return false;
      if (!builtin_shadowed && call_name_is_int_arith_builtin(n) &&
          e->as.call.args.len == 2) {
        return ny_is_proven_int(cg, scopes, depth, e->as.call.args.data[0].val,
                                NULL) &&
               ny_is_proven_int(cg, scopes, depth, e->as.call.args.data[1].val,
                                NULL);
      }
      if (!builtin_shadowed &&
          (strcmp(n, "__load8_idx") == 0 || strcmp(n, "__load16_idx") == 0 ||
           strcmp(n, "__load32_idx") == 0 || strcmp(n, "__load64_idx") == 0 ||
           call_name_tail_is(n, "load8") || call_name_tail_is(n, "load16") ||
           call_name_tail_is(n, "load32") || call_name_tail_is(n, "load64") ||
           call_name_tail_is(n, "load64_i") ||
           call_name_tail_is(n, "load32_h")) &&
          e->as.call.args.len == 2)
        return true;
      bool want_builtin_get =
          !builtin_shadowed &&
          (strcmp(n, "get") == 0 || strcmp(n, "std.core.get") == 0 ||
           strcmp(n, "std.core.reflect.get") == 0 ||
           call_name_tail_is(n, "get"));
      if (want_builtin_get) {
        expr_t *default_expr =
            e->as.call.args.len >= 3 ? e->as.call.args.data[2].val : NULL;
        if (!type_get_default_is_proven_int(cg, scopes, depth, default_expr, 0))
          return false;
        expr_t *target = e->as.call.args.data[0].val;
        if (target && target->kind == NY_E_IDENT && target->as.ident.name) {
          size_t name_len = (size_t)target->tok.len;
          if (name_len == 0)
            name_len = strlen(target->as.ident.name);
          binding *b =
              type_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                  name_len, target->as.ident.hash);
          if (b && b->is_int_dict_storage)
            return true;
          if (b && b->is_int_list_storage)
            return true;
        }
        expr_t *init =
            ny_typeinfer_static_indexable_init(cg, scopes, depth, target);
        const char *elem_type = NULL;
        if (init)
          elem_type = ny_static_indexable_elem_type(scopes, depth, init);
        return elem_type && strcmp(elem_type, "int") == 0;
      }
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.args.len == 1) {
      const char *n = e->as.call.callee->as.ident.name;
      if (!n)
        return false;
      if (!builtin_shadowed &&
          (call_name_is_int_length_builtin(n) || strcmp(n, "__tagof") == 0 ||
           strcmp(n, "__runtime_tag") == 0 || strcmp(n, "__tag") == 0 ||
           strcmp(n, "__load8_idx") == 0 ||
           strcmp(n, "__load16_idx") == 0 || strcmp(n, "__load32_idx") == 0 ||
           strcmp(n, "__load64_idx") == 0 || call_name_tail_is(n, "load8") ||
           call_name_tail_is(n, "load16") || call_name_tail_is(n, "load32") ||
           call_name_tail_is(n, "load64") || call_name_tail_is(n, "load64_i") ||
           call_name_tail_is(n, "load32_h") ||
           call_name_tail_is(n, "dict_len") ||
           call_name_tail_is(n, "type_tag") ||
           call_name_tail_is(n, "from_int")))
        return true;
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const char *n = e->as.call.callee->as.ident.name;
      if (!n)
        return false;
      fun_sig *sig = resolve_overload(cg, n, e->as.call.args.len,
                                      e->as.call.callee->as.ident.hash);
      const char *ret_type =
          sig ? (sig->return_type ? sig->return_type
                                  : sig->inferred_return_type)
              : NULL;
      if (ret_type && (strcmp(ret_type, "int") == 0 ||
                       strcmp(ret_type, "i64") == 0))
        return true;
      bool static_user_call = false;
      if (type_call_returns_proven_int(cg, scopes, depth, e, NULL, 0, 0,
                                       &static_user_call))
        return true;
      if (static_user_call)
        return false;
    }
    if (e->as.call.callee && e->as.call.callee->kind != NY_E_IDENT) {
      fun_sig *sig =
          type_lookup_static_call_sig(cg, scopes, depth, e->as.call.callee,
                                      e->as.call.args.len);
      const char *ret_type = type_sig_return_name(sig);
      if (ret_type && (strcmp(ret_type, "int") == 0 ||
                       strcmp(ret_type, "i64") == 0))
        return true;
      bool static_user_call = false;
      if (type_call_returns_proven_int(cg, scopes, depth, e, NULL, 0, 0,
                                       &static_user_call))
        return true;
      if (static_user_call)
        return false;
    }
    return false;
  }
  case NY_E_INDEX: {
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name && e->as.index.start &&
        ny_is_proven_int(cg, scopes, depth, e->as.index.start, NULL)) {
      expr_t *target = e->as.index.target;
      size_t name_len = (size_t)target->tok.len;
      if (name_len == 0)
        name_len = strlen(target->as.ident.name);
      binding *b = type_lookup_binding(cg, scopes, depth,
                                        target->as.ident.name, name_len,
                                        target->as.ident.hash);
      if (b && b->is_int_list_storage)
        return true;
    }
    const char *t = infer_expr_type(cg, scopes, depth, e);
    return t && (strcmp(t, "int") == 0 || strcmp(t, "i64") == 0 ||
                 strcmp(t, "i32") == 0 || strcmp(t, "i16") == 0 ||
                 strcmp(t, "i8") == 0 || strcmp(t, "u64") == 0 ||
                 strcmp(t, "u32") == 0 || strcmp(t, "u16") == 0 ||
                 strcmp(t, "u8") == 0);
  }
  case NY_E_MEMCALL:
    if (e->as.memcall.name && call_name_tail_is(e->as.memcall.name, "get") &&
        e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name) {
      expr_t *default_expr =
          e->as.memcall.args.len >= 2 ? e->as.memcall.args.data[1].val : NULL;
      if (!type_get_default_is_proven_int(cg, scopes, depth, default_expr, 0))
        return false;
      expr_t *target = e->as.memcall.target;
      size_t name_len = (size_t)target->tok.len;
      if (name_len == 0)
        name_len = strlen(target->as.ident.name);
      binding *b = type_lookup_binding(cg, scopes, depth,
                                        target->as.ident.name, name_len,
                                        target->as.ident.hash);
      if (b && (b->is_int_dict_storage || b->is_int_list_storage))
        return true;
      expr_t *init = ny_typeinfer_static_indexable_init(cg, scopes, depth,
                                                        target);
      const char *elem_type = NULL;
      if (init)
        elem_type = ny_static_indexable_elem_type(scopes, depth, init);
      return elem_type && strcmp(elem_type, "int") == 0;
    }
    return false;
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    expr_t *le = e->as.binary.left;
    expr_t *re = e->as.binary.right;
    if (!op || !le || !re)
      return false;
    bool l_ok = ny_is_proven_int(cg, scopes, depth, le, NULL);
    bool r_ok = ny_is_proven_int(cg, scopes, depth, re, NULL);
    if (!l_ok || !r_ok)
      return false;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
        strcmp(op, "^") == 0 || strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^^") == 0 ||
        strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0)
      return true;
    if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0)
      return true;
    return false;
  }
  case NY_E_UNARY:
    if (!e->as.unary.op || !e->as.unary.right)
      return false;
    if (strcmp(e->as.unary.op, "+") == 0 || strcmp(e->as.unary.op, "-") == 0 ||
        strcmp(e->as.unary.op, "~") == 0)
      return ny_is_proven_int(cg, scopes, depth, e->as.unary.right, NULL);
    return false;
  case NY_E_IDENT: {
    if (!e->as.ident.name)
      return false;
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = type_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                     name_len, e->as.ident.hash);
    if (b) {
      if (b->type_name && !is_int_type_name(b->type_name))
        return false;
      if (b->is_int_slot || b->is_int_direct)
        return true;
      expr_t *init =
          !b->is_mut ? ny_binding_var_init_expr(b, e->as.ident.name) : NULL;
      if (init)
        return ny_is_proven_int(cg, scopes, depth, init, NULL);
      return false;
    }
    return false;
  }
  case NY_E_MEMBER:
    return e->as.member.name && strcmp(e->as.member.name, "len") == 0;
  default:
    return false;
  }
}

bool ny_is_proven_int(codegen_t *cg, scope *scopes, size_t depth, expr_t *e,
                      LLVMValueRef v) {
  static _Thread_local unsigned recursion_depth = 0;
  if (recursion_depth >= 128)
    return false;
  recursion_depth++;
  bool result = ny_is_proven_int_inner(cg, scopes, depth, e, v);
  recursion_depth--;
  return result;
}

bool ny_is_proven_bool(codegen_t *cg, scope *scopes, size_t depth, expr_t *e,
                       LLVMValueRef v) {
  if (v && LLVMIsAConstantInt(v)) {
    int64_t val = LLVMConstIntGetSExtValue(v);
    return (val == NY_IMM_TRUE || val == NY_IMM_FALSE);
  }
  if (!e)
    return false;
  const char *t = infer_expr_type(cg, scopes, depth, e);
  return t && (strcmp(t, "bool") == 0);
}
