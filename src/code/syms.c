#include "base/util.h"
#include "priv.h"
#include <alloca.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ny_user_ctx_without_prelude(const codegen_t *cg) {
  if (cg->implicit_prelude)
    return false;
  if (!cg->current_module_name)
    return true;
  return strncmp(cg->current_module_name, "std.", 4) != 0 &&
         strncmp(cg->current_module_name, "lib.", 4) != 0;
}

static bool ny_block_implicit_std_symbol(const codegen_t *cg, const char *query,
                                         const char *candidate_name) {
  if (!ny_user_ctx_without_prelude(cg))
    return false;
  if (!query || strchr(query, '.'))
    return false;
  if (!candidate_name || !*candidate_name)
    return false;
  return strncmp(candidate_name, "std.", 4) == 0 ||
         strncmp(candidate_name, "lib.", 4) == 0;
}

bool builtin_allowed_comptime(const char *name) {
  // Disallow non-deterministic or system-interacting builtins at comptime.
  static const char *deny[] = {
      "__syscall",      "__execve",      "__dlopen",
      "__dlsym",        "__dlclose",     "__dlerror",
      "__thread_spawn", "__thread_join", "__rand64",
      "__srand",        "__globals",     "__set_globals",
      "__set_args",     "__parse_ast",   NULL,
  };
  for (int i = 0; deny[i]; ++i) {
    if (strcmp(name, deny[i]) == 0)
      return false;
  }
  return true;
}

void add_builtins(codegen_t *cg) {
  if (!cg) {
    fprintf(stderr, "add_builtins: cg is NULL\n");
    return;
  }
  if (!cg->ctx) {
    fprintf(stderr, "add_builtins: cg->ctx is NULL\n");
    return;
  }
  if (!cg->module) {
    fprintf(stderr, "add_builtins: cg->module is NULL\n");
    return;
  }
  if (!cg->type_i64) {
    fprintf(stderr, "add_builtins: cg->type_i64 is NULL\n");
    return;
  }

  LLVMTypeRef fn_types[32];
  for (int i = 0; i < 32; i++) {
    LLVMTypeRef *pts = alloca(sizeof(LLVMTypeRef) * (size_t)i);
    for (int j = 0; j < i; j++)
      pts[j] = cg->type_i64;
    fn_types[i] = LLVMFunctionType(cg->type_i64, pts, (unsigned)i, 0);
    if (!fn_types[i])
      fprintf(stderr, "add_builtins: failed to create fn_type for arity %d\n",
              i);
  }

#define RT_DEF(name, implementation, args, sig, doc)                           \
  do {                                                                         \
    if (cg->comptime && !builtin_allowed_comptime(name))                       \
      break;                                                                   \
    LLVMTypeRef ty = NULL;                                                     \
    if (strcmp(name, "__copy_mem") == 0) {                                     \
      ty = LLVMFunctionType(                                                   \
          LLVMVoidTypeInContext(cg->ctx),                                      \
          (LLVMTypeRef[]){cg->type_i64, cg->type_i64, cg->type_i64}, 3, 0);    \
    } else {                                                                   \
      ty = fn_types[args];                                                     \
    }                                                                          \
    const char *impl_name = #implementation;                                   \
    LLVMValueRef f = LLVMGetNamedFunction(cg->module, impl_name);              \
    if (!f)                                                                    \
      f = LLVMAddFunction(cg->module, impl_name, ty);                          \
    fun_sig sig_obj = {ny_strdup(name), ty,    f,    NULL, (int)args,          \
                       false,           false, NULL, NULL, false};             \
    vec_push(&cg->fun_sigs, sig_obj);                                          \
  } while (0);

#define RT_GV(name, p, t, doc)                                                 \
  do {                                                                         \
    LLVMValueRef g = LLVMAddGlobal(cg->module, cg->type_i64, name);            \
    LLVMSetLinkage(g, LLVMExternalLinkage);                                    \
    binding b = {ny_strdup(name), g, NULL, false, false, false, NULL};         \
    vec_push(&cg->global_vars, b);                                             \
  } while (0);

#include "rt/defs.h"

#undef RT_DEF
#undef RT_GV
}

enum_member_def_t *lookup_enum_member(codegen_t *cg, const char *name) {
  if (!name || !*name) {
    return NULL;
  }

  const char *dot = strrchr(name, '.');

  if (dot) {
    // Fully qualified name: "EnumName.MemberName"
    size_t enum_name_len = dot - name;
    const char *member_name = dot + 1;

    for (size_t i = 0; i < cg->enums.len; ++i) {
      enum_def_t *enum_def = cg->enums.data[i];
      if (enum_def->name && strncmp(enum_def->name, name, enum_name_len) == 0 &&
          enum_def->name[enum_name_len] == '\0') {
        // Found the enum definition, now look for the member
        for (size_t j = 0; j < enum_def->members.len; ++j) {
          enum_member_def_t *member_def = &enum_def->members.data[j];
          if (member_def->name && strcmp(member_def->name, member_name) == 0) {
            return member_def;
          }
        }
      }
    }
  } else {
    // Unqualified name: "MemberName"
    // Search all enums for a member with this name
    for (size_t i = 0; i < cg->enums.len; ++i) {
      enum_def_t *enum_def = cg->enums.data[i];
      for (size_t j = 0; j < enum_def->members.len; ++j) {
        enum_member_def_t *member_def = &enum_def->members.data[j];
        if (member_def->name && strcmp(member_def->name, name) == 0) {
          return member_def;
        }
      }
    }
  }

  return NULL;
}

enum_member_def_t *lookup_enum_member_owner(codegen_t *cg, const char *name,
                                            enum_def_t **out_enum) {
  if (out_enum)
    *out_enum = NULL;
  if (!name || !*name)
    return NULL;

  const char *dot = strrchr(name, '.');
  if (dot) {
    size_t enum_name_len = (size_t)(dot - name);
    const char *member_name = dot + 1;
    for (size_t i = 0; i < cg->enums.len; ++i) {
      enum_def_t *enum_def = cg->enums.data[i];
      if (enum_def->name && strncmp(enum_def->name, name, enum_name_len) == 0 &&
          enum_def->name[enum_name_len] == '\0') {
        for (size_t j = 0; j < enum_def->members.len; ++j) {
          enum_member_def_t *member_def = &enum_def->members.data[j];
          if (member_def->name && strcmp(member_def->name, member_name) == 0) {
            if (out_enum)
              *out_enum = enum_def;
            return member_def;
          }
        }
      }
    }
    return NULL;
  }

  enum_member_def_t *found = NULL;
  enum_def_t *owner = NULL;
  int hits = 0;
  for (size_t i = 0; i < cg->enums.len; ++i) {
    enum_def_t *enum_def = cg->enums.data[i];
    for (size_t j = 0; j < enum_def->members.len; ++j) {
      enum_member_def_t *member_def = &enum_def->members.data[j];
      if (member_def->name && strcmp(member_def->name, name) == 0) {
        hits++;
        found = member_def;
        owner = enum_def;
      }
    }
  }
  if (hits == 1 && out_enum)
    *out_enum = owner;
  return (hits == 1) ? found : NULL;
}

char *codegen_full_name(codegen_t *cg, expr_t *e, arena_t *a) {
  if (!e)
    return NULL;
  if (e->kind == NY_E_IDENT) {
    const char *resolved_alias = resolve_import_alias(cg, e->as.ident.name);
    if (resolved_alias)
      return arena_strndup(a, resolved_alias, strlen(resolved_alias));
    return arena_strndup(a, e->as.ident.name, strlen(e->as.ident.name));
  }
  if (e->kind == NY_E_MEMBER) {
    char *target_name = codegen_full_name(cg, e->as.member.target, a);
    if (!target_name)
      return NULL;
    size_t len = strlen(target_name) + 1 + strlen(e->as.member.name);
    char *full_name = arena_alloc(a, len + 1);
    snprintf(full_name, len + 1, "%s.%s", target_name, e->as.member.name);
    return full_name;
  }
  return NULL;
}

fun_sig *lookup_fun(codegen_t *cg, const char *name) {
  fun_sig *res = NULL;

  if (cg->implicit_prelude && name && strcmp(name, "eq") == 0 &&
      strchr(name, '.') == NULL) {
    for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
      if (strcmp(cg->fun_sigs.data[i].name, "std.core.reflect.eq") == 0) {
        return &cg->fun_sigs.data[i];
      }
    }
  }

  // 1. Precise name match (local or unqualified global)
  for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
    if (strcmp(cg->fun_sigs.data[i].name, name) == 0) {
      if (ny_block_implicit_std_symbol(cg, name, cg->fun_sigs.data[i].name))
        continue;
      res = &cg->fun_sigs.data[i];
      goto end;
    }
  }

  // 2. Namespaced lookup if name is not qualified
  if (cg->current_module_name && strchr(name, '.') == NULL) {
    if (strcmp(name, "__globals") == 0) {
      fprintf(stderr,
              "DEBUG: lookup_fun searching for __globals (cur_mod=%s)\n",
              cg->current_module_name ? cg->current_module_name : "NULL");
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.%s", cg->current_module_name, name);
    for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
      if (strcmp(cg->fun_sigs.data[i].name, buf) == 0) {
        res = &cg->fun_sigs.data[i];
        goto end;
      }
    }
  }

  // 3. Import aliases and common fallbacks if name is not qualified
  if (strchr(name, '.') == NULL) {
    const char *alias_full = resolve_import_alias(cg, name);
    if (alias_full) {
      fun_sig *ares = lookup_fun(cg, alias_full);
      if (ares) {
        res = ares;
        goto end;
      }
    }

    if (cg->implicit_prelude) {
      const char *fallbacks[] = {"std.core",
                                 "std.core.reflect",
                                 "std.core.primitives",
                                 "std.core.error",
                                 "std.core.mem",
                                 "std.core.list",
                                 "std.core.dict",
                                 "std.core.set",
                                 "std.core.sort",
                                 "std.str",
                                 "std.str.io",
                                 "std.str.path",
                                 "std.str.fmt",
                                 "std.math",
                                 "std.math.bigint",
                                 "std.math.float",
                                 "std.math.random",
                                 "std.os",
                                 "std.os.fs",
                                 "std.os.process",
                                 "std.os.time",
                                 "std.os.ffi",
                                 "std.os.args",
                                 "std.net",
                                 "std.net.socket",
                                 "std.net.http",
                                 "std.util",
                                 "std.util.inspect",
                                 "std.util.uuid",
                                 "std.core.iter",
                                 NULL};
      for (int j = 0; fallbacks[j]; ++j) {
        if (cg->current_module_name &&
            strcmp(cg->current_module_name, fallbacks[j]) == 0)
          continue;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s.%s", fallbacks[j], name);
        for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
          if (strcmp(cg->fun_sigs.data[i].name, buf) == 0) {
            res = &cg->fun_sigs.data[i];
            goto end;
          }
        }
      }
    }
  }
  // Check aliases if name has dot
  const char *dot = strchr(name, '.');
  if (dot) {
    size_t prefix_len = dot - name;
    for (size_t i = 0; i < cg->aliases.len; ++i) {
      const char *alias = cg->aliases.data[i].name;
      if (strlen(alias) == prefix_len &&
          strncmp(name, alias, prefix_len) == 0) {
        const char *real_mod_name = (const char *)cg->aliases.data[i].stmt_t;
        // Avoid infinite recursion if alias matches itself
        if (strncmp(name, real_mod_name, prefix_len) == 0 &&
            real_mod_name[prefix_len] == '\0') {
          continue;
        }
        // Construct resolved name: real_mod_name + dot + suffix
        char *resolved = malloc(strlen(real_mod_name) + strlen(dot) + 1);
        strcpy(resolved, real_mod_name);
        strcat(resolved, dot);
        fun_sig *recursive_res = lookup_fun(cg, resolved);
        free(resolved);
        if (recursive_res) {
          res = recursive_res;
          goto end;
        }
      }
    }
  } else {
    // Check if the whole name is a module alias, try alias_target.name
    for (size_t i = 0; i < cg->aliases.len; ++i) {
      if (strcmp(cg->aliases.data[i].name, name) == 0) {
        const char *real_mod_name = (const char *)cg->aliases.data[i].stmt_t;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s.%s", real_mod_name, name);
        fun_sig *s = lookup_fun(cg, buf);
        if (s) {
          res = s;
          goto end;
        }
      }
    }
  }
  for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
    const char *sig_name = cg->fun_sigs.data[i].name;
    // Also try matching after the last dot if the input name is not qualified
    if (strchr(name, '.') == NULL) {
      const char *last_dot = strrchr(sig_name, '.');
      if (last_dot && strcmp(last_dot + 1, name) == 0) {
        // We found a match in a module. But is this module "used"?
        // Check if the prefix (module name) is in use_modules
        size_t mod_len = last_dot - sig_name;
        bool user_only = !cg->implicit_prelude &&
                         (!cg->current_module_name ||
                          (strncmp(cg->current_module_name, "std.", 4) != 0 &&
                           strncmp(cg->current_module_name, "lib.", 4) != 0));
        char *const *mods_data =
            user_only ? cg->user_use_modules.data : cg->use_modules.data;
        size_t mods_len =
            user_only ? cg->user_use_modules.len : cg->use_modules.len;
        for (size_t m = 0; m < mods_len; ++m) {
          const char *um = mods_data[m];
          if (strlen(um) == mod_len && strncmp(um, sig_name, mod_len) == 0) {
            res = &cg->fun_sigs.data[i];
            goto end;
          }
        }
      }
    }
  }
end:;
  return res;
}

static bool module_is_used(const codegen_t *cg, const char *mod,
                           size_t mod_len) {
  bool user_only = !cg->implicit_prelude &&
                   (!cg->current_module_name ||
                    (strncmp(cg->current_module_name, "std.", 4) != 0 &&
                     strncmp(cg->current_module_name, "lib.", 4) != 0));
  char *const *mods_data =
      user_only ? cg->user_use_modules.data : cg->use_modules.data;
  size_t mods_len = user_only ? cg->user_use_modules.len : cg->use_modules.len;
  for (size_t i = 0; i < mods_len; ++i) {
    const char *used = mods_data[i];
    if (!used)
      continue;
    if (strlen(used) == mod_len && strncmp(used, mod, mod_len) == 0)
      return true;
  }
  return false;
}

static int typo_distance_if_relevant(const char *want, const char *cand) {
  size_t wl = strlen(want);
  size_t cl = strlen(cand);
  size_t diff = (wl > cl) ? (wl - cl) : (cl - wl);
  if (diff > 2)
    return 99;
  return ny_levenshtein(want, cand);
}

static void maybe_add_suggestion(const char **s1, int *d1, const char **s2,
                                 int *d2, const char *cand, int dist) {
  if (dist >= 3 || !cand)
    return;
  if (*s1 && strcmp(*s1, cand) == 0)
    return;
  if (*s2 && strcmp(*s2, cand) == 0)
    return;
  if (dist < *d1) {
    *s2 = *s1;
    *d2 = *d1;
    *s1 = cand;
    *d1 = dist;
  } else if (dist < *d2) {
    *s2 = cand;
    *d2 = dist;
  }
}

void report_undef_symbol(codegen_t *cg, const char *name, token_t tok) {
  ny_diag_error(tok, "undefined symbol \033[1;37m'%s'\033[0m", name);
  if (verbose_enabled >= 2) {
    ny_diag_hint("searched %zu functions and %zu globals", cg->fun_sigs.len,
                 cg->global_vars.len);
  }
  cg->had_error = 1;

  const char *best = NULL;
  int best_d = 100;
  const char *alt1 = NULL, *alt2 = NULL;
  int alt1_d = 100, alt2_d = 100;

  /* 1. Check for capitalization errors (common mistake) */
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    if (strcasecmp(name, cg->fun_sigs.data[i].name) == 0 ||
        (strrchr(cg->fun_sigs.data[i].name, '.') &&
         strcasecmp(name, strrchr(cg->fun_sigs.data[i].name, '.') + 1) == 0)) {
      ny_diag_hint("did you mean '%s'? (case mismatch)",
                   cg->fun_sigs.data[i].name);
      ny_diag_fix("use the exact spelling '%s'", cg->fun_sigs.data[i].name);
      return;
    }
  }

  /* 2. Check for missing imports/prefixes for common builtins */
  struct {
    const char *sym;
    const char *hint;
  } common[] = {{"write", "did you mean 'sys_write' or 'print'?"},
                {"open", "did you mean 'sys_open' or 'std.str.io.open'?"},
                {"socket", "try 'import std.net.socket'"},
                {"json_encode", "try 'import std.str.json'"},
                {"json_decode", "try 'import std.str.json'"},
                {"Thread", "try 'import std.os.thread'"},
                {"sleep", "try 'import std.os.time'"},
                {"printf", "Nytrix uses 'print' or 'std.str.fmt'"},
                {"malloc", "try 'alloc' or 'std.core.mem.alloc'"},
                {"free", "try 'std.core.mem.free'"},
                {NULL, NULL}};
  for (int i = 0; common[i].sym; i++) {
    if (strcmp(name, common[i].sym) == 0) {
      ny_diag_hint("%s", common[i].hint);
      ny_diag_fix("import the suggested module or replace '%s' with the Nytrix "
                  "equivalent",
                  name);
      return;
    }
  }

  /* 3. Check if symbol exists in a module that is not imported */
  if (strchr(name, '.') == NULL) {
    for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
      const char *cand = cg->fun_sigs.data[i].name;
      const char *dot = strrchr(cand, '.');
      if (!dot || strcmp(dot + 1, name) != 0)
        continue;
      size_t mod_len = (size_t)(dot - cand);
      if (module_is_used(cg, cand, mod_len))
        continue;
      ny_diag_hint("'%s' exists in module '%.*s'", name, (int)mod_len, cand);
      ny_diag_hint("add 'use %.*s;' or call '%s' explicitly", (int)mod_len,
                   cand, cand);
      ny_diag_fix("add at file top: use %.*s;", (int)mod_len, cand);
      return;
    }
    for (size_t i = 0; i < cg->global_vars.len; ++i) {
      const char *cand = cg->global_vars.data[i].name;
      const char *dot = strrchr(cand, '.');
      if (!dot || strcmp(dot + 1, name) != 0)
        continue;
      size_t mod_len = (size_t)(dot - cand);
      if (module_is_used(cg, cand, mod_len))
        continue;
      ny_diag_hint("'%s' exists in module '%.*s'", name, (int)mod_len, cand);
      ny_diag_hint("add 'use %.*s;' or reference '%s'", (int)mod_len, cand,
                   cand);
      ny_diag_fix("add at file top: use %.*s;", (int)mod_len, cand);
      return;
    }
  }

  /* 4. Levenshtein for typos */
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    const char *cand = cg->fun_sigs.data[i].name;
    const char *dot = strrchr(cand, '.');
    const char *base = dot ? dot + 1 : cand;
    int dist = typo_distance_if_relevant(name, base);
    if (dist < best_d && dist < 3) {
      maybe_add_suggestion(&alt1, &alt1_d, &alt2, &alt2_d, best, best_d);
      best_d = dist;
      best = cand;
    } else {
      maybe_add_suggestion(&alt1, &alt1_d, &alt2, &alt2_d, cand, dist);
    }
  }
  for (size_t i = 0; i < cg->global_vars.len; ++i) {
    const char *cand = cg->global_vars.data[i].name;
    const char *dot = strrchr(cand, '.');
    const char *base = dot ? dot + 1 : cand;
    int dist = typo_distance_if_relevant(name, base);
    if (dist < best_d && dist < 3) {
      maybe_add_suggestion(&alt1, &alt1_d, &alt2, &alt2_d, best, best_d);
      best_d = dist;
      best = cand;
    } else {
      maybe_add_suggestion(&alt1, &alt1_d, &alt2, &alt2_d, cand, dist);
    }
  }

  if (best) {
    ny_diag_hint("did you mean '%s'?", best);
    if (alt1)
      ny_diag_hint("other close match: '%s'", alt1);
    if (alt2)
      ny_diag_hint("other close match: '%s'", alt2);
    ny_diag_fix("replace '%s' with the correct symbol name", name);
  } else if (strcmp(name, "int") == 0 || strcmp(name, "float") == 0) {
    ny_diag_hint("types are dynamic; use 'is_int'/'is_float' checks");
    ny_diag_fix("remove the static type name and keep the value dynamic");
  } else if (strcmp(name, "char") == 0) {
    ny_diag_hint("use strings for characters");
    ny_diag_fix("replace 'char' with a single-character string, e.g. \"a\"");
  } else if (strchr(name, '.') == NULL) {
    ny_diag_fix("if '%s' is from stdlib, add 'use std.<module>;' at file top",
                name);
  }
}

fun_sig *lookup_use_module_fun(codegen_t *cg, const char *name, size_t argc) {
  if (!name || !*name)
    return NULL;
  const char *alias_full = resolve_import_alias(cg, name);
  if (alias_full) {
    fun_sig *aliased = resolve_overload(cg, alias_full, argc);
    if (aliased)
      return aliased;
  }
  return NULL;
}

const char *resolve_import_alias(codegen_t *cg, const char *name) {
  binding *data = ny_user_ctx_without_prelude(cg) ? cg->user_import_aliases.data
                                                  : cg->import_aliases.data;
  size_t len = ny_user_ctx_without_prelude(cg) ? cg->user_import_aliases.len
                                               : cg->import_aliases.len;
  for (size_t i = 0; i < len; ++i) {
    if (strcmp(data[i].name, name) == 0) {
      return (const char *)data[i].stmt_t;
    }
  }
  return NULL;
}

binding *lookup_global(codegen_t *cg, const char *name) {
  if (!cg->global_vars.data)
    return NULL;

  // 1. Precise name match (local or unqualified global)
  for (ssize_t i = (ssize_t)cg->global_vars.len - 1; i >= 0; --i) {
    if (strcmp(cg->global_vars.data[i].name, name) == 0) {
      if (ny_block_implicit_std_symbol(cg, name, cg->global_vars.data[i].name))
        continue;
      cg->global_vars.data[i].is_used = true;
      return &cg->global_vars.data[i];
    }
  }

  // 2. Namespaced lookup if name is not qualified
  if (cg->current_module_name && strchr(name, '.') == NULL) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.%s", cg->current_module_name, name);
    for (ssize_t i = (ssize_t)cg->global_vars.len - 1; i >= 0; --i) {
      if (strcmp(cg->global_vars.data[i].name, buf) == 0) {
        cg->global_vars.data[i].is_used = true;
        return &cg->global_vars.data[i];
      }
    }
  }

  // 3. Import aliases and common fallbacks if name is not qualified
  if (strchr(name, '.') == NULL) {
    const char *alias_full = resolve_import_alias(cg, name);
    if (alias_full) {
      binding *ares = lookup_global(cg, alias_full);
      if (ares)
        return ares;
    }

    if (cg->implicit_prelude) {
      const char *fallbacks[] = {"std.core",
                                 "std.core.reflect",
                                 "std.core.primitives",
                                 "std.core.error",
                                 "std.core.mem",
                                 "std.core.list",
                                 "std.core.dict",
                                 "std.core.set",
                                 "std.core.sort",
                                 "std.str",
                                 "std.str.io",
                                 "std.str.path",
                                 "std.str.fmt",
                                 "std.math",
                                 "std.math.bigint",
                                 "std.math.float",
                                 "std.math.random",
                                 "std.os",
                                 "std.os.fs",
                                 "std.os.process",
                                 "std.os.time",
                                 "std.os.ffi",
                                 "std.os.args",
                                 "std.net",
                                 "std.net.socket",
                                 "std.net.http",
                                 "std.util",
                                 "std.util.inspect",
                                 "std.util.uuid",
                                 "std.core.iter",
                                 NULL};
      for (int j = 0; fallbacks[j]; ++j) {
        if (cg->current_module_name &&
            strcmp(cg->current_module_name, fallbacks[j]) == 0)
          continue;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s.%s", fallbacks[j], name);
        for (ssize_t i = (ssize_t)cg->global_vars.len - 1; i >= 0; --i) {
          if (strcmp(cg->global_vars.data[i].name, buf) == 0)
            return &cg->global_vars.data[i];
        }
      }
    }
  }
  for (ssize_t i = (ssize_t)cg->global_vars.len - 1; i >= 0; --i) {
    const char *sig_name = cg->global_vars.data[i].name;
    // Also try matching after the last dot if the input name is not qualified
    if (strchr(name, '.') == NULL) {
      const char *last_dot = strrchr(sig_name, '.');
      if (last_dot && strcmp(last_dot + 1, name) == 0) {
        size_t mod_len = last_dot - sig_name;
        bool user_only = !cg->implicit_prelude &&
                         (!cg->current_module_name ||
                          (strncmp(cg->current_module_name, "std.", 4) != 0 &&
                           strncmp(cg->current_module_name, "lib.", 4) != 0));
        char *const *mods_data =
            user_only ? cg->user_use_modules.data : cg->use_modules.data;
        size_t mods_len =
            user_only ? cg->user_use_modules.len : cg->use_modules.len;
        for (size_t m = 0; m < mods_len; ++m) {
          const char *um = mods_data[m];
          if (strlen(um) == mod_len && strncmp(um, sig_name, mod_len) == 0) {
            return &cg->global_vars.data[i];
          }
        }
      }
    }
  }
  return NULL;
}

fun_sig *resolve_overload(codegen_t *cg, const char *name, size_t argc) {
  if (cg->implicit_prelude && name && strcmp(name, "eq") == 0 &&
      strchr(name, '.') == NULL) {
    for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
      fun_sig *fs = &cg->fun_sigs.data[i];
      if (strcmp(fs->name, "std.core.reflect.eq") == 0 ||
          strcmp(fs->name, "std.core.eq") == 0) {
        return fs;
      }
    }
  }
  if (cg->implicit_prelude && name && strcmp(name, "set") == 0 &&
      strchr(name, '.') == NULL) {
    for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
      fun_sig *fs = &cg->fun_sigs.data[i];
      if (strcmp(fs->name, "std.core.set.set") == 0 ||
          strcmp(fs->name, "std.core.set") == 0) {
        return fs;
      }
    }
  }
  if (cg->current_module_name && name && strchr(name, '.') == NULL) {
    char scoped[256];
    snprintf(scoped, sizeof(scoped), "%s.%s", cg->current_module_name, name);
    fun_sig *scoped_res = resolve_overload(cg, scoped, argc);
    if (scoped_res)
      return scoped_res;
  }
  if (strchr(name, '.') == NULL) {
    const char *alias_full = resolve_import_alias(cg, name);
    if (alias_full) {
      return resolve_overload(cg, alias_full, argc);
    }
  }
  fun_sig *best = NULL;
  int best_score = -1;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *fs = &cg->fun_sigs.data[i];
    if (strcmp(fs->name, name) != 0)
      continue;
    int score = -1;
    if (!fs->is_variadic) {
      if (fs->arity == (int)argc)
        score = 100;
      else if ((int)argc < fs->arity)
        score = 80;
    } else {
      int fixed = fs->arity - 1;
      if ((int)argc >= fixed)
        score = 60 + (int)fixed;
    }
    if (score > best_score) {
      if (ny_block_implicit_std_symbol(cg, name, fs->name))
        continue;
      best_score = score;
      best = fs;
    }
  }
  if (cg->implicit_prelude && !best && strchr(name, '.') == NULL) {
    const char *fallbacks[] = {"std.core",
                               "std.core.reflect",
                               "std.core.primitives",
                               "std.core.error",
                               "std.core.mem",
                               "std.core.list",
                               "std.core.dict",
                               "std.core.set",
                               "std.core.sort",
                               "std.str",
                               "std.str.io",
                               "std.str.path",
                               "std.str.fmt",
                               "std.math",
                               "std.math.bigint",
                               "std.math.float",
                               "std.math.random",
                               "std.os",
                               "std.os.fs",
                               "std.os.process",
                               "std.os.time",
                               "std.os.ffi",
                               "std.os.args",
                               "std.net",
                               "std.net.socket",
                               "std.net.http",
                               "std.util",
                               "std.util.inspect",
                               NULL};
    for (int j = 0; fallbacks[j]; ++j) {
      char buf[256];
      snprintf(buf, sizeof(buf), "%s.%s", fallbacks[j], name);
      fun_sig *fb = resolve_overload(cg, buf, argc);
      if (fb)
        return fb;
    }
  }
  return best;
}

binding *scope_lookup(scope *scopes, size_t depth, const char *name) {
  for (ssize_t s = (ssize_t)depth; s >= 0; --s)
    for (ssize_t i = (ssize_t)scopes[s].vars.len - 1; i >= 0; --i)
      if (strcmp(scopes[s].vars.data[i].name, name) == 0) {
        NY_LOG_DEBUG("scope_lookup: found '%s' at depth %zd, is_mut=%d\n", name,
                     s, scopes[s].vars.data[i].is_mut);
        scopes[s].vars.data[i].is_used = true;
        return &scopes[s].vars.data[i];
      }
  return NULL;
}

static binding *scope_lookup_no_mark(scope *scopes, size_t depth,
                                     const char *name) {
  for (ssize_t s = (ssize_t)depth; s >= 0; --s) {
    for (ssize_t i = (ssize_t)scopes[s].vars.len - 1; i >= 0; --i) {
      if (strcmp(scopes[s].vars.data[i].name, name) == 0)
        return &scopes[s].vars.data[i];
    }
  }
  return NULL;
}

void bind(scope *scopes, size_t depth, const char *name, LLVMValueRef v,
          stmt_t *stmt, bool is_mut, const char *type_name) {
  // Check for redefinition in current scope
  for (size_t i = 0; i < scopes[depth].vars.len; i++) {
    if (strcmp(scopes[depth].vars.data[i].name, name) == 0) {
      binding *prev = &scopes[depth].vars.data[i];
      if (stmt) {
        ny_diag_error(stmt->tok, "redefinition of '%s' in the same scope",
                      name);
        if (prev->stmt_t)
          ny_diag_note_tok(prev->stmt_t->tok,
                           "previous definition of '%s' is here", name);
      } else {
        ny_diag_error((token_t){0}, "redefinition of '%s' in argument list",
                      name);
      }
    }
  }

  // Check for shadowing (only if depth > 0 to search parent scopes)
  if (depth > 0) {
    binding *shadow = scope_lookup_no_mark(scopes, depth - 1, name);
    if (shadow && stmt &&
        ny_diag_should_emit("shadow_local", stmt->tok, name)) {
      ny_diag_warning(stmt->tok, "declaration of '%s' shadows a previous local",
                      name);
      if (shadow->stmt_t)
        ny_diag_note_tok(shadow->stmt_t->tok, "previous '%s' declared here",
                         name);
    }
  }

  binding b = {name, v, stmt, is_mut, false, false, type_name};
  vec_push(&scopes[depth].vars, b);
}

void scope_pop(scope *scopes, size_t *depth) {
  for (size_t i = 0; i < scopes[*depth].vars.len; i++) {
    binding *b = &scopes[*depth].vars.data[i];
    if (!b->stmt_t)
      continue;
    bool is_stdlib = b->stmt_t->tok.filename &&
                     (strcmp(b->stmt_t->tok.filename, "<stdlib>") == 0 ||
                      strcmp(b->stmt_t->tok.filename, "<repl_std>") == 0);
    if (!b->is_used && b->name && b->name[0] != '_' &&
        ny_diag_should_emit("unused_var", b->stmt_t->tok, b->name) &&
        (!is_stdlib || verbose_enabled >= 2)) {
      ny_diag_warning(b->stmt_t->tok, "unused variable \033[1;37m'%s'\033[0m",
                      b->name);
    }
  }
  vec_free(&scopes[*depth].defers);
  vec_free(&scopes[*depth].vars);
  (*depth)--;
}

void add_import_alias(codegen_t *cg, const char *alias, const char *full_name) {
  if (!alias || !*alias || !full_name || !*full_name)
    return;
  for (size_t i = 0; i < cg->import_aliases.len; ++i) {
    if (cg->import_aliases.data[i].name &&
        strcmp(cg->import_aliases.data[i].name, alias) == 0)
      return;
  }
  binding alias_bind = {0};
  alias_bind.name = ny_strdup(alias);
  alias_bind.stmt_t = (stmt_t *)ny_strdup(full_name);
  vec_push(&cg->import_aliases, alias_bind);
}

void add_import_alias_from_full(codegen_t *cg, const char *full_name) {
  if (!full_name || !*full_name)
    return;
  const char *last_dot = strrchr(full_name, '.');
  const char *alias = last_dot ? last_dot + 1 : full_name;
  add_import_alias(cg, alias, full_name);
}

static void add_user_import_alias(codegen_t *cg, const char *alias,
                                  const char *full_name) {
  if (!alias || !*alias || !full_name || !*full_name)
    return;
  for (size_t i = 0; i < cg->user_import_aliases.len; ++i) {
    if (cg->user_import_aliases.data[i].name &&
        strcmp(cg->user_import_aliases.data[i].name, alias) == 0)
      return;
  }
  binding alias_bind = {0};
  alias_bind.name = ny_strdup(alias);
  alias_bind.stmt_t = (stmt_t *)ny_strdup(full_name);
  vec_push(&cg->user_import_aliases, alias_bind);
}

static void add_user_import_alias_from_full(codegen_t *cg,
                                            const char *full_name) {
  if (!full_name || !*full_name)
    return;
  const char *last_dot = strrchr(full_name, '.');
  const char *alias = last_dot ? last_dot + 1 : full_name;
  add_user_import_alias(cg, alias, full_name);
}

stmt_t *find_module_stmt(stmt_t *s, const char *name) {
  if (!s || !name)
    return NULL;
  if (s->kind == NY_S_MODULE && s->as.module.name &&
      strcmp(s->as.module.name, name) == 0) {
    return s;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      stmt_t *found = find_module_stmt(s->as.module.body.data[i], name);
      if (found)
        return found;
    }
  }
  return NULL;
}

bool module_has_export_list(const stmt_t *mod) {
  if (!mod || mod->kind != NY_S_MODULE)
    return false;
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    if (mod->as.module.body.data[i]->kind == NY_S_EXPORT)
      return true;
  }
  return false;
}

void collect_module_exports(stmt_t *mod, str_list *exports) {
  if (!mod || mod->kind != NY_S_MODULE)
    return;
  const char *mod_name = mod->as.module.name;
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (child->kind != NY_S_EXPORT)
      continue;
    for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
      const char *name = child->as.exprt.names.data[j];
      if (!name)
        continue;
      char *full = NULL;
      if (strchr(name, '.')) {
        full = ny_strdup(name);
      } else {
        size_t len = strlen(mod_name) + 1 + strlen(name) + 1;
        full = malloc(len);
        snprintf(full, len, "%s.%s", mod_name, name);
      }
      vec_push(exports, full);
    }
  }
}

void collect_module_defs(stmt_t *mod, str_list *exports) {
  if (!mod || mod->kind != NY_S_MODULE)
    return;
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (child->kind == NY_S_FUNC) {
      vec_push(exports, ny_strdup(child->as.fn.name));
    } else if (child->kind == NY_S_VAR) {
      for (size_t j = 0; j < child->as.var.names.len; ++j) {
        vec_push(exports, ny_strdup(child->as.var.names.data[j]));
      }
    }
  }
}

void add_imports_from_prefix(codegen_t *cg, const char *mod) {
  if (!mod || !*mod)
    return;
  size_t mod_len = strlen(mod);
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    const char *name = cg->fun_sigs.data[i].name;
    if (strncmp(name, mod, mod_len) == 0 && name[mod_len] == '.') {
      add_import_alias_from_full(cg, name);
    }
  }
  for (size_t i = 0; i < cg->global_vars.len; ++i) {
    const char *name = cg->global_vars.data[i].name;
    if (strncmp(name, mod, mod_len) == 0 && name[mod_len] == '.') {
      add_import_alias_from_full(cg, name);
    }
  }
}

char *normalize_module_name(const char *raw) {
  if (!raw)
    return NULL;
  // If raw contains '/', assume it's a file path
  if (strchr(raw, '/')) {
    const char *last_slash = strrchr(raw, '/');
    const char *start = last_slash ? last_slash + 1 : raw;
    char *name = ny_strdup(start);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) {
      *dot = '\0';
    }
    return name;
  }
  return ny_strdup(raw);
}

void process_use_imports(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_USE) {
    if (!s->as.use.import_all && s->as.use.imports.len == 0)
      return;
    bool user_use = !ny_is_stdlib_tok(s->tok);
    char *mod = normalize_module_name(s->as.use.module);
    if (s->as.use.imports.len > 0) {
      for (size_t i = 0; i < s->as.use.imports.len; ++i) {
        use_item_t *item = &s->as.use.imports.data[i];
        if (!item->name)
          continue;
        size_t len = strlen(mod) + 1 + strlen(item->name) + 1;
        char *full = malloc(len);
        snprintf(full, len, "%s.%s", mod, item->name);
        const char *alias = item->alias ? item->alias : item->name;
        add_import_alias(cg, alias, full);
        if (user_use)
          add_user_import_alias(cg, alias, full);
        free(full);
      }
      free(mod);
      return;
    }
    if (s->as.use.import_all) {
      str_list exports = {0};
      bool has_export_list = false;
      stmt_t *mod_stmt = NULL;
      // Also try finding module definition stmt matching normalized name
      for (size_t i = 0; i < cg->prog->body.len; ++i) {
        mod_stmt = find_module_stmt(cg->prog->body.data[i], mod);
        if (mod_stmt)
          break;
      }
      // If not found, maybe fallback to raw name? loader renames it though.

      if (mod_stmt) {
        has_export_list = module_has_export_list(mod_stmt);
        if (has_export_list) {
          collect_module_exports(mod_stmt, &exports);
        }
        if (!has_export_list || mod_stmt->as.module.export_all) {
          collect_module_defs(mod_stmt, &exports);
        }
      }
      if (!mod_stmt || exports.len == 0) {
        add_imports_from_prefix(cg, mod);
      } else {
        for (size_t i = 0; i < exports.len; ++i) {
          add_import_alias_from_full(cg, exports.data[i]);
          if (user_use)
            add_user_import_alias_from_full(cg, exports.data[i]);
          free(exports.data[i]);
        }
        vec_free(&exports);
      }
      free(mod);
      return;
    }
    free(mod);
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      process_use_imports(cg, s->as.module.body.data[i]);
    }
  }
}

void collect_use_aliases(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_USE) {
    if (s->as.use.import_all || s->as.use.imports.len > 0)
      return;
    const char *alias = s->as.use.alias;
    char *mod = normalize_module_name(s->as.use.module);
    if (!alias) {
      // Infer alias from module name (not path)
      const char *dot = strrchr(mod, '.');
      alias = dot ? dot + 1 : mod;
    }
    binding alias_bind = {0};
    alias_bind.name = ny_strdup(alias);
    alias_bind.stmt_t = (stmt_t *)ny_strdup(mod);
    // Handle specific imports list: use Mod (a, b as c)
    // Actually this branch is for plain `use Mod` (or `use Mod as Alias`)
    // The imports list is handled in the if earlier?
    // Wait, collect_use_aliases logic above checks empty imports.
    // So this handles `use Mod` and `use Mod as M`.

    vec_push(&cg->aliases, alias_bind);
    free(mod);
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      collect_use_aliases(cg, s->as.module.body.data[i]);
  }
}

void collect_use_modules(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_USE) {
    if (!s->as.use.import_all && s->as.use.imports.len > 0)
      return;
    const char *mod = s->as.use.module;
    if (mod && *mod) {
      bool seen_global = false;
      for (size_t i = 0; i < cg->use_modules.len; ++i) {
        if (strcmp(cg->use_modules.data[i], mod) == 0) {
          seen_global = true;
          break;
        }
      }
      if (!seen_global)
        vec_push(&cg->use_modules, ny_strdup(mod));
      if (!ny_is_stdlib_tok(s->tok)) {
        bool seen_user = false;
        for (size_t i = 0; i < cg->user_use_modules.len; ++i) {
          if (strcmp(cg->user_use_modules.data[i], mod) == 0) {
            seen_user = true;
            break;
          }
        }
        if (!seen_user)
          vec_push(&cg->user_use_modules, ny_strdup(mod));
      }
    }
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      collect_use_modules(cg, s->as.module.body.data[i]);
  }
}

void process_exports(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_MODULE) {
    const char *prev_mod = cg->current_module_name;
    cg->current_module_name = s->as.module.name;
    const char *mod_name = s->as.module.name;
    if (cg->implicit_prelude && s->as.module.export_all) {
      str_list defs = {0};
      collect_module_defs(s, &defs);
      for (size_t i = 0; i < defs.len; i++) {
        const char *full_target = defs.data[i];
        const char *last_dot = strrchr(full_target, '.');
        const char *short_name = last_dot ? last_dot + 1 : full_target;

        if (strcmp(full_target, short_name) != 0) {
          fun_sig *fs = lookup_fun(cg, full_target);
          if (fs) {
            bool exists = false;
            for (ssize_t k = (ssize_t)cg->fun_sigs.len - 1; k >= 0; k--) {
              if (strcmp(cg->fun_sigs.data[k].name, short_name) == 0) {
                exists = true;
                break;
              }
            }
            if (!exists) {
              fun_sig ns = *fs;
              ns.name = ny_strdup(short_name);
              vec_push(&cg->fun_sigs, ns);
            }
          } else {
            binding *gb = lookup_global(cg, full_target);
            if (gb) {
              bool exists = false;
              for (ssize_t k = (ssize_t)cg->global_vars.len - 1; k >= 0; k--) {
                if (strcmp(cg->global_vars.data[k].name, short_name) == 0) {
                  exists = true;
                  break;
                }
              }
              if (!exists) {
                binding nb = *gb;
                nb.name = ny_strdup(short_name);
                vec_push(&cg->global_vars, nb);
              }
            }
          }
        }
        free(defs.data[i]);
      }
      vec_free(&defs);
    }
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      stmt_t *child = s->as.module.body.data[i];
      if (child->kind == NY_S_EXPORT) {
        for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
          const char *target = child->as.exprt.names.data[j];
          char alias[256];
          snprintf(alias, sizeof(alias), "%s.%s", mod_name, target);
          char full_target[256];
          snprintf(full_target, sizeof(full_target), "%s.%s", mod_name, target);
          fun_sig *fs = lookup_fun(cg, full_target);
          if (!fs)
            fs = lookup_fun(cg, target);
          if (fs) {
            fun_sig new_sig = *fs;
            new_sig.name = ny_strdup(alias);
            vec_push(&cg->fun_sigs, new_sig);
          } else {
            binding *gb = lookup_global(cg, full_target);
            if (!gb)
              gb = lookup_global(cg, target);
            if (gb) {
              binding new_bind = *gb;
              new_bind.name = ny_strdup(alias);
              vec_push(&cg->global_vars, new_bind);
            }
          }
        }
      } else if (child->kind == NY_S_MODULE) {
        process_exports(cg, child);
      }
    }
    cg->current_module_name = prev_mod;
  }
}
