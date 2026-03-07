#include "priv.h"

#include <stdio.h>
#include <string.h>

bool ny_gencall_type_is_nullable(const char *type_name) {
  return type_name && type_name[0] == '?';
}

bool ny_gencall_type_is(const char *type_name, const char *want_tail) {
  const char *tail = ny_type_leaf(type_name);
  if (!tail || !want_tail)
    return false;
  size_t want_len = strlen(want_tail);
  return strncmp(tail, want_tail, want_len) == 0 &&
         (tail[want_len] == '\0' || tail[want_len] == '<');
}

bool ny_gencall_type_is_real_number(const char *type_name) {
  const char *tail = ny_type_leaf(type_name);
  if (!tail)
    return false;
  return strcmp(tail, "int") == 0 || strcmp(tail, "float") == 0 ||
         strcmp(tail, "f32") == 0 || strcmp(tail, "f64") == 0 ||
         strcmp(tail, "f128") == 0 || strcmp(tail, "i8") == 0 ||
         strcmp(tail, "i16") == 0 || strcmp(tail, "i32") == 0 ||
         strcmp(tail, "i64") == 0 || strcmp(tail, "i128") == 0 ||
         strcmp(tail, "u8") == 0 || strcmp(tail, "u16") == 0 ||
         strcmp(tail, "u32") == 0 || strcmp(tail, "u64") == 0 ||
         strcmp(tail, "u128") == 0;
}

bool ny_gencall_type_is_integer_number(const char *type_name) {
  return ny_gencall_type_is_real_number(type_name) &&
         !ny_gencall_type_is(type_name, "float") &&
         !ny_gencall_type_is(type_name, "f32") &&
         !ny_gencall_type_is(type_name, "f64") &&
         !ny_gencall_type_is(type_name, "f128");
}

bool ny_gencall_type_is_bigint(const char *type_name) {
  const char *tail = ny_type_leaf(type_name);
  return tail && (strcmp(tail, "bigint") == 0 || strcmp(tail, "BigInt") == 0);
}

bool ny_gencall_type_is_number(const char *type_name) {
  const char *tail = ny_type_leaf(type_name);
  return (tail && strcmp(tail, "number") == 0) ||
         ny_gencall_type_is_real_number(type_name) ||
         ny_gencall_type_is_bigint(type_name);
}

bool ny_gencall_type_is_ordered_number(const char *type_name) {
  return ny_gencall_type_is_number(type_name);
}

typedef enum {
  NY_MATH_CONTRACT_NONE = 0,
  NY_MATH_CONTRACT_NUMBER,
  NY_MATH_CONTRACT_REAL,
  NY_MATH_CONTRACT_INTEGER,
  NY_MATH_CONTRACT_ORDERED,
} ny_math_contract_kind_t;

static ny_math_contract_kind_t ny_gencall_math_contract_kind(const char *name) {
  if (!name || !*name)
    return NY_MATH_CONTRACT_NONE;
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
      ny_name_tail_is(name, "std.math.round") ||
      ny_name_tail_is(name, "std.math.pow") ||
      ny_name_tail_is(name, "std.math.lerp"))
    return NY_MATH_CONTRACT_REAL;
  if (ny_name_tail_is(name, "std.math.gcd") ||
      ny_name_tail_is(name, "std.math.lcm") ||
      ny_name_tail_is(name, "std.math.factorial"))
    return NY_MATH_CONTRACT_INTEGER;
  if (ny_name_tail_is(name, "std.math.clamp") ||
      ny_name_tail_is(name, "std.math.min") ||
      ny_name_tail_is(name, "std.math.max") ||
      ny_name_tail_is(name, "std.math.abs") ||
      ny_name_tail_is(name, "std.math.sign"))
    return NY_MATH_CONTRACT_ORDERED;
  if (ny_name_tail_is(name, "std.math.mod"))
    return NY_MATH_CONTRACT_NUMBER;
  return NY_MATH_CONTRACT_NONE;
}

static const char *ny_math_contract_expected(ny_math_contract_kind_t kind) {
  switch (kind) {
  case NY_MATH_CONTRACT_NUMBER:
    return "number";
  case NY_MATH_CONTRACT_REAL:
    return "real number (int|float)";
  case NY_MATH_CONTRACT_INTEGER:
    return "integer number (int|bigint)";
  case NY_MATH_CONTRACT_ORDERED:
    return "ordered number";
  default:
    return "value";
  }
}

bool ny_gencall_check_math_contract(codegen_t *cg, scope *scopes,
                                    size_t depth, fun_sig *sig,
                                    expr_t *arg) {
  if (!cg || !sig || !sig->name || !arg)
    return true;
  ny_math_contract_kind_t kind = ny_gencall_math_contract_kind(sig->name);
  if (kind == NY_MATH_CONTRACT_NONE)
    return true;
  const char *got = infer_expr_type(cg, scopes, depth, arg);
  if (!got)
    return true;
  bool ok = false;
  switch (kind) {
  case NY_MATH_CONTRACT_NUMBER:
    ok = ny_gencall_type_is_number(got);
    break;
  case NY_MATH_CONTRACT_REAL:
    ok = ny_gencall_type_is_real_number(got);
    break;
  case NY_MATH_CONTRACT_INTEGER:
    ok = ny_gencall_type_is_integer_number(got) ||
         ny_gencall_type_is_bigint(got);
    break;
  case NY_MATH_CONTRACT_ORDERED:
    ok = ny_gencall_type_is_ordered_number(got);
    break;
  default:
    ok = true;
    break;
  }
  if (ok)
    return true;
  ny_diag_type_mismatch(arg->tok, ny_math_contract_expected(kind),
                        ny_name_leaf(got) ? ny_name_leaf(got) : got,
                        "argument");
  cg->had_error = 1;
  return false;
}

int ny_gencall_vec_type_dim(const char *type_name) {
  const char *tail = ny_type_leaf(type_name);
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

bool ny_gencall_type_is_vec(const char *type_name) {
  return ny_gencall_vec_type_dim(type_name) > 0;
}

const char *ny_gencall_attached_owner(const char *type_name) {
  if (!type_name)
    return NULL;
  while (*type_name == '?' || *type_name == '*')
    type_name++;
  if (!*type_name)
    return NULL;
  const char *leaf = strrchr(type_name, '.');
  leaf = leaf ? leaf + 1 : type_name;
#define NY_OWNER_IF_BASE(name)                                                 \
  do {                                                                         \
    size_t len = sizeof(name) - 1;                                             \
    if (strncmp(leaf, name, len) == 0 &&                                       \
        (leaf[len] == '\0' || leaf[len] == '<'))                               \
      return name;                                                             \
  } while (0)
  NY_OWNER_IF_BASE("any");
  NY_OWNER_IF_BASE("str");
  NY_OWNER_IF_BASE("list");
  NY_OWNER_IF_BASE("dict");
  NY_OWNER_IF_BASE("set");
  NY_OWNER_IF_BASE("tuple");
  NY_OWNER_IF_BASE("bytes");
  NY_OWNER_IF_BASE("range");
  NY_OWNER_IF_BASE("bigint");
  NY_OWNER_IF_BASE("int");
  NY_OWNER_IF_BASE("float");
  NY_OWNER_IF_BASE("f32");
  NY_OWNER_IF_BASE("f64");
  NY_OWNER_IF_BASE("bool");
#undef NY_OWNER_IF_BASE
  return type_name;
}

static fun_sig *ny_gencall_lookup_attached_method_candidate(codegen_t *cg,
                                                            const char *name) {
  fun_sig *sig = lookup_fun(cg, name, 0);
  if (sig && !cg->parent && !ny_sig_in_current_sigs(cg, sig))
    return NULL;
  return sig;
}

fun_sig *ny_gencall_lookup_attached_method(codegen_t *cg,
                                           const char *type_name,
                                           const char *method_name) {
  const char *owner = ny_gencall_attached_owner(type_name);
  if (!cg || !owner || !*owner || !method_name || !*method_name)
    return NULL;
  char direct[512];
  int n = snprintf(direct, sizeof(direct), "%s.%s", owner, method_name);
  if (n > 0 && (size_t)n < sizeof(direct)) {
    fun_sig *sig = ny_gencall_lookup_attached_method_candidate(cg, direct);
    if (sig)
      return sig;
  }
  if (strcmp(owner, "integer") == 0) {
    fun_sig *sig = ny_gencall_lookup_attached_method(cg, "bigint", method_name);
    if (sig)
      return sig;
    sig = ny_gencall_lookup_attached_method(cg, "int", method_name);
    if (sig)
      return sig;
  }
  if (strchr(owner, '.'))
    return NULL;
  if (cg->current_module_name && *cg->current_module_name) {
    char mod_buf[512];
    int mod_n =
        snprintf(mod_buf, sizeof(mod_buf), "%s", cg->current_module_name);
    if (mod_n > 0 && (size_t)mod_n < sizeof(mod_buf)) {
      char *mod = mod_buf;
      while (*mod) {
        const char *tail = strrchr(mod, '.');
        tail = tail ? tail + 1 : mod;
        if (strcmp(tail, owner) == 0) {
          char scoped_owner[512];
          int sn = snprintf(scoped_owner, sizeof(scoped_owner), "%s.%s", mod,
                            method_name);
          if (sn > 0 && (size_t)sn < sizeof(scoped_owner)) {
            fun_sig *sig =
                ny_gencall_lookup_attached_method_candidate(cg, scoped_owner);
            if (sig)
              return sig;
          }
        }
        char scoped[512];
        int mn = snprintf(scoped, sizeof(scoped), "%s.%s.%s", mod, owner,
                          method_name);
        if (mn > 0 && (size_t)mn < sizeof(scoped)) {
          fun_sig *sig =
              ny_gencall_lookup_attached_method_candidate(cg, scoped);
          if (sig)
            return sig;
        }
        char *dot = strrchr(mod, '.');
        if (!dot)
          break;
        *dot = '\0';
      }
    }
  }
  for (size_t i = cg->user_use_modules.len; i > 0; i--) {
    const char *mod = cg->user_use_modules.data[i - 1];
    if (!mod || !*mod)
      continue;
    char imported[512];
    int in = snprintf(imported, sizeof(imported), "%s.%s.%s", mod, owner,
                      method_name);
    if (in > 0 && (size_t)in < sizeof(imported)) {
      fun_sig *sig = ny_gencall_lookup_attached_method_candidate(cg, imported);
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
      fun_sig *sig = ny_gencall_lookup_attached_method_candidate(cg, imported);
      if (sig)
        return sig;
    }
  }
  char core_builtin[512];
  int core_n = snprintf(core_builtin, sizeof(core_builtin), "std.core.%s.%s",
                        owner, method_name);
  if (core_n > 0 && (size_t)core_n < sizeof(core_builtin)) {
    fun_sig *sig = ny_gencall_lookup_attached_method_candidate(cg, core_builtin);
    if (sig)
      return sig;
  }
  return NULL;
}

int ny_gencall_known_obj_tag(const char *type_name) {
  if (ny_gencall_type_is_nullable(type_name))
    return -1;
  if (ny_gencall_type_is_vec(type_name))
    return 101;
  if (ny_gencall_type_is(type_name, "list"))
    return 100;
  if (ny_gencall_type_is(type_name, "dict"))
    return 101;
  if (ny_gencall_type_is(type_name, "set"))
    return 102;
  if (ny_gencall_type_is(type_name, "tuple"))
    return 103;
  if (ny_gencall_type_is(type_name, "bytes"))
    return 122;
  if (ny_gencall_type_is(type_name, "bigint"))
    return 130;
  return -1;
}

int ny_gencall_known_tagof(const char *type_name) {
  if (ny_gencall_type_is_nullable(type_name))
    return -1;
  if (ny_gencall_type_is_integer_number(type_name))
    return -1;
  if (ny_gencall_type_is(type_name, "f32") ||
      ny_gencall_type_is(type_name, "f64") ||
      ny_gencall_type_is(type_name, "f128"))
    return 110;
  return ny_gencall_known_obj_tag(type_name);
}

bool ny_gencall_type_is_known_obj(const char *type_name) {
  if (ny_gencall_type_is_nullable(type_name))
    return false;
  return ny_gencall_known_obj_tag(type_name) >= 0 ||
         ny_gencall_type_is(type_name, "str");
}

bool ny_gencall_type_is_known_non_obj(const char *type_name) {
  if (!type_name || !*type_name)
    return false;
  if (ny_gencall_type_is_nullable(type_name))
    return false;
  return ny_gencall_type_is(type_name, "int") ||
         ny_gencall_type_is(type_name, "i8") ||
         ny_gencall_type_is(type_name, "i16") ||
         ny_gencall_type_is(type_name, "i32") ||
         ny_gencall_type_is(type_name, "i64") ||
         ny_gencall_type_is(type_name, "i128") ||
         ny_gencall_type_is(type_name, "u8") ||
         ny_gencall_type_is(type_name, "u16") ||
         ny_gencall_type_is(type_name, "u32") ||
         ny_gencall_type_is(type_name, "u64") ||
         ny_gencall_type_is(type_name, "u128") ||
         ny_gencall_type_is(type_name, "f32") ||
         ny_gencall_type_is(type_name, "f64") ||
         ny_gencall_type_is(type_name, "f128") ||
         ny_gencall_type_is(type_name, "bool") ||
         ny_gencall_type_is(type_name, "nil") ||
         ny_gencall_type_is(type_name, "none") ||
         ny_gencall_type_is(type_name, "ptr") ||
         ny_gencall_type_is(type_name, "handle") ||
         ny_gencall_type_is(type_name, "char");
}
