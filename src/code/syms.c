#include "priv.h"
#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct builtin_def {
  const char *name;
  int args;
} builtin_defs[] = {
    {"__malloc", 1},
    {"__free", 1},
    {"__realloc", 2},
    {"__memcpy", 3},
    {"__memset", 3},
    {"__memcmp", 3},
    {"__load8_idx", 2},
    {"__load16_idx", 2},
    {"__load32_idx", 2},
    {"__load64_idx", 2},
    {"__store8_idx", 3},
    {"__store16_idx", 3},
    {"__store32_idx", 3},
    {"__store64_idx", 3},
    {"__sys_read_off", 4},
    {"__sys_write_off", 4},
    {"__add", 2},
    {"__sub", 2},
    {"__mul", 2},
    {"__div", 2},
    {"__mod", 2},
    {"__and", 2},
    {"__or", 2},
    {"__xor", 2},
    {"__shl", 2},
    {"__shr", 2},
    {"__not", 1},
    {"__str_concat", 2},
    {"__eq", 2},
    {"__lt", 2},
    {"__le", 2},
    {"__gt", 2},
    {"__ge", 2},
    {"__is_int", 1},
    {"__is_ptr", 1},
    {"__is_str", 1},
    {"__is_flt", 1},
    {"__to_str", 1},
    {"__panic", 1},
    {"__argc", 0},
    {"__argv", 1},
    {"__envp", 0},
    {"__envc", 0},
    {"__errno", 0},
    {"__syscall", 7},
    {"__execve", 3},
    {"__dlopen", 2},
    {"__dlsym", 2},
    {"__dlclose", 1},
    {"__dlerror", 0},
    {"__globals", 0},
    {"__set_globals", 1},
    {"__get_panic_val", 0},
    {"__set_panic_env", 1},
    {"__clear_panic_env", 0},
    {"__jmpbuf_size", 0},
    {"__thread_spawn", 2},
    {"__thread_join", 1},
    {"__mutex_new", 0},
    {"__mutex_lock64", 1},
    {"__mutex_unlock64", 1},
    {"__mutex_free", 1},
    {"__kwarg", 2},
    {"__parse_ast", 1},
    {"__set_args", 3},
    {"__flt_from_int", 1},
    {"__flt_to_int", 1},
    {"__flt_trunc", 1},
    {"__flt_add", 2},
    {"__flt_sub", 2},
    {"__flt_mul", 2},
    {"__flt_div", 2},
    {"__flt_lt", 2},
    {"__flt_gt", 2},
    {"__flt_eq", 2},
    {"__flt_box_val", 1},
    {"__flt_unbox_val", 1},
    {"__rand64", 0},
    {"__srand", 1},
};

bool builtin_allowed_comptime(const char *name) {
  // Disallow non-deterministic or system-interacting builtins at comptime.
  static const char *deny[] = {
      "__argc",        "__argv",     "__envp",      "__envc",
      "__errno",       "__syscall",  "__execve",    "__dlopen",
      "__dlsym",       "__dlclose",  "__dlerror",   "__thread_spawn",
      "__thread_join", "__rand64",   "__srand",     "__globals",
      "__set_globals", "__set_args", "__parse_ast", NULL,
  };
  for (int i = 0; deny[i]; ++i) {
    if (strcmp(name, deny[i]) == 0)
      return false;
  }
  return true;
}

void add_builtins(codegen_t *cg) {
  LLVMTypeRef fn0 = LLVMFunctionType(cg->type_i64, NULL, 0, 0);
  LLVMTypeRef fn1 =
      LLVMFunctionType(cg->type_i64, (LLVMTypeRef[]){cg->type_i64}, 1, 0);
  LLVMTypeRef fn2 = LLVMFunctionType(
      cg->type_i64, (LLVMTypeRef[]){cg->type_i64, cg->type_i64}, 2, 0);
  LLVMTypeRef fn3 = LLVMFunctionType(
      cg->type_i64, (LLVMTypeRef[]){cg->type_i64, cg->type_i64, cg->type_i64},
      3, 0);
  LLVMTypeRef fn4 = LLVMFunctionType(
      cg->type_i64,
      (LLVMTypeRef[]){cg->type_i64, cg->type_i64, cg->type_i64, cg->type_i64},
      4, 0);
  LLVMTypeRef fn7 = LLVMFunctionType(
      cg->type_i64,
      (LLVMTypeRef[]){cg->type_i64, cg->type_i64, cg->type_i64, cg->type_i64,
                      cg->type_i64, cg->type_i64, cg->type_i64},
      7, 0);
  for (size_t i = 0; i < sizeof(builtin_defs) / sizeof(builtin_defs[0]); ++i) {
    if (cg->comptime && !builtin_allowed_comptime(builtin_defs[i].name))
      continue;
    LLVMTypeRef ty = NULL;
    switch (builtin_defs[i].args) {
    case 0:
      ty = fn0;
      break;
    case 1:
      ty = fn1;
      break;
    case 2:
      ty = fn2;
      break;
    case 3:
      ty = fn3;
      break;
    case 4:
      ty = fn4;
      break;
    case 7:
      ty = fn7;
      break;
    default:
      fprintf(stderr, "bad args cnt\n");
      exit(1);
    }
    LLVMValueRef f = LLVMGetNamedFunction(cg->module, builtin_defs[i].name);
    if (!f)
      f = LLVMAddFunction(cg->module, builtin_defs[i].name, ty);
    fun_sig sig = {.name = strdup(builtin_defs[i].name),
                   .type = ty,
                   .value = f,
                   .stmt_t = NULL,
                   .arity = builtin_defs[i].args,
                   .is_variadic = false};
    vec_push(&cg->fun_sigs, sig);

    // Alias common runtime functions
    if (strcmp(builtin_defs[i].name, "__argc") == 0) {
      fun_sig alias = sig;
      alias.name = strdup("argc");
      vec_push(&cg->fun_sigs, alias);
    } else if (strcmp(builtin_defs[i].name, "__argv") == 0) {
      fun_sig alias = sig;
      alias.name = strdup("argv");
      vec_push(&cg->fun_sigs, alias);
    }
  }
  for (int n = 0; n <= 13; n++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "__call%d", n);
    LLVMTypeRef *pts = alloca(sizeof(LLVMTypeRef) * (size_t)(n + 1));
    for (int j = 0; j <= n; j++)
      pts[j] = cg->type_i64;
    LLVMTypeRef cty = LLVMFunctionType(cg->type_i64, pts, (unsigned)(n + 1), 0);
    LLVMValueRef f = LLVMAddFunction(cg->module, buf, cty);
    fun_sig sig = {.name = strdup(buf),
                   .type = cty,
                   .value = f,
                   .stmt_t = NULL,
                   .arity = n + 1,
                   .is_variadic = false};
    vec_push(&cg->fun_sigs, sig);
  }
}

fun_sig *lookup_fun(codegen_t *cg, const char *name) {
  // 1a. Try namespaced lookup if name is not qualified
  if (cg->current_module_name && strchr(name, '.') == NULL) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.%s", cg->current_module_name, name);
    for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
      if (strcmp(cg->fun_sigs.data[i].name, buf) == 0)
        return &cg->fun_sigs.data[i];
    }
  }
  // 1b. Try common fallbacks if name is not qualified
  if (strchr(name, '.') == NULL) {
    const char *alias_full = resolve_impo_alias(cg, name);
    if (alias_full) {
      return lookup_fun(cg, alias_full);
    }
    const char *fallbacks[] = {
        "std.core", "std.io", "std.collections", "std.strings.str", "std.math",
        "std.os",   NULL};
    for (int j = 0; fallbacks[j]; ++j) {
      if (cg->current_module_name &&
          strcmp(cg->current_module_name, fallbacks[j]) == 0)
        continue;
      char buf[256];
      snprintf(buf, sizeof(buf), "%s.%s", fallbacks[j], name);
      for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
        if (strcmp(cg->fun_sigs.data[i].name, buf) == 0)
          return &cg->fun_sigs.data[i];
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
        fun_sig *res =
            lookup_fun(cg, resolved); // Recursive lookup with resolved name
        free(resolved);
        return res;
      }
    }
  }
  for (ssize_t i = (ssize_t)cg->fun_sigs.len - 1; i >= 0; --i) {
    const char *sig_name = cg->fun_sigs.data[i].name;
    if (strcmp(sig_name, name) == 0)
      return &cg->fun_sigs.data[i];
    // Also try matching after the last dot if the input name is not qualified
    if (strchr(name, '.') == NULL) {
      const char *last_dot = strrchr(sig_name, '.');
      if (last_dot && strcmp(last_dot + 1, name) == 0) {
        // We found a match in a module. But is this module "used"?
        // Check if the prefix (module name) is in use_modules
        size_t mod_len = last_dot - sig_name;
        for (size_t m = 0; m < cg->use_modules.len; ++m) {
          const char *um = cg->use_modules.data[m];
          if (strlen(um) == mod_len && strncmp(um, sig_name, mod_len) == 0) {
            return &cg->fun_sigs.data[i];
          }
        }
      }
    }
  }
  return NULL;
}

fun_sig *lookup_use_module_fun(codegen_t *cg, const char *name, size_t argc) {
  if (!name || !*name)
    return NULL;
  for (size_t i = 0; i < cg->use_modules.len; ++i) {
    const char *mod = cg->use_modules.data[i];
    if (!mod)
      continue;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.%s", mod, name);
    fun_sig *s = resolve_overload(cg, buf, argc);
    if (s)
      return s;
  }
  return NULL;
}

const char *resolve_impo_alias(codegen_t *cg, const char *name) {
  for (size_t i = 0; i < cg->impo_aliases.len; ++i) {
    if (strcmp(cg->impo_aliases.data[i].name, name) == 0) {
      return (const char *)cg->impo_aliases.data[i].stmt_t;
    }
  }
  return NULL;
}

binding *lookup_global(codegen_t *cg, const char *name) {
  if (!cg->global_vars.data)
    return NULL;
  // 1. Try namespaced lookup if name is not qualified
  if (cg->current_module_name && strchr(name, '.') == NULL) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.%s", cg->current_module_name, name);
    for (ssize_t i = (ssize_t)cg->global_vars.len - 1; i >= 0; --i) {
      if (strcmp(cg->global_vars.data[i].name, buf) == 0)
        return &cg->global_vars.data[i];
    }
  }
  // 1b. Try common fallbacks if name is not qualified
  if (strchr(name, '.') == NULL) {
    const char *alias_full = resolve_impo_alias(cg, name);
    if (alias_full) {
      return lookup_global(cg, alias_full);
    }
    const char *fallbacks[] = {"std.core", "std.io", "std.os", "std.core.test",
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
  for (ssize_t i = (ssize_t)cg->global_vars.len - 1; i >= 0; --i) {
    const char *sig_name = cg->global_vars.data[i].name;
    if (strcmp(sig_name, name) == 0)
      return &cg->global_vars.data[i];
    // Also try matching after the last dot if the input name is not qualified
    if (strchr(name, '.') == NULL) {
      const char *last_dot = strrchr(sig_name, '.');
      if (last_dot && strcmp(last_dot + 1, name) == 0) {
        size_t mod_len = last_dot - sig_name;
        for (size_t m = 0; m < cg->use_modules.len; ++m) {
          const char *um = cg->use_modules.data[m];
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
      best_score = score;
      best = fs;
    }
  }
  if (!best && strchr(name, '.') == NULL) {
    const char *fallbacks[] = {"std.core", "std.io", "std.collections", NULL};
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
      if (strcmp(scopes[s].vars.data[i].name, name) == 0)
        return &scopes[s].vars.data[i];
  return NULL;
}

void bind(scope *scopes, size_t depth, const char *name, LLVMValueRef v,
          stmt_t *stmt) {
  binding b = {name, v, stmt};
  vec_push(&scopes[depth].vars, b);
}

void add_impo_alias(codegen_t *cg, const char *alias, const char *full_name) {
  if (!alias || !*alias || !full_name || !*full_name)
    return;
  // fprintf(stderr, "DEBUG: add_impo__alias alias=%s full=%s\n", alias,
  // full_name);
  for (size_t i = 0; i < cg->impo_aliases.len; ++i) {
    if (cg->impo_aliases.data[i].name &&
        strcmp(cg->impo_aliases.data[i].name, alias) == 0)
      return;
  }
  binding alias_bind = {0};
  alias_bind.name = strdup(alias);
  alias_bind.stmt_t = (stmt_t *)strdup(full_name);
  vec_push(&cg->impo_aliases, alias_bind);
}

void add_impo_alias_from_full(codegen_t *cg, const char *full_name) {
  if (!full_name || !*full_name)
    return;
  const char *last_dot = strrchr(full_name, '.');
  const char *alias = last_dot ? last_dot + 1 : full_name;
  add_impo_alias(cg, alias, full_name);
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

bool module_has_expo_list(const stmt_t *mod) {
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
        full = strdup(name);
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
      vec_push(exports, strdup(child->as.fn.name));
    } else if (child->kind == NY_S_VAR) {
      for (size_t j = 0; j < child->as.var.names.len; ++j) {
        vec_push(exports, strdup(child->as.var.names.data[j]));
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
      add_impo_alias_from_full(cg, name);
    }
  }
  for (size_t i = 0; i < cg->global_vars.len; ++i) {
    const char *name = cg->global_vars.data[i].name;
    if (strncmp(name, mod, mod_len) == 0 && name[mod_len] == '.') {
      add_impo_alias_from_full(cg, name);
    }
  }
}

void process_use_imports(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_USE) {
    if (!s->as.use.impo_all && s->as.use.imports.len == 0)
      return;
    const char *mod = s->as.use.module;
    if (s->as.use.imports.len > 0) {
      for (size_t i = 0; i < s->as.use.imports.len; ++i) {
        use_item_t *item = &s->as.use.imports.data[i];
        if (!item->name)
          continue;
        size_t len = strlen(mod) + 1 + strlen(item->name) + 1;
        char *full = malloc(len);
        snprintf(full, len, "%s.%s", mod, item->name);
        add_impo_alias(cg, item->alias ? item->alias : item->name, full);
        free(full);
      }
      return;
    }
    if (s->as.use.impo_all) {
      str_list exports = {0};
      bool has_expo_list = false;
      stmt_t *mod_stmt = NULL;
      for (size_t i = 0; i < cg->prog->body.len; ++i) {
        mod_stmt = find_module_stmt(cg->prog->body.data[i], mod);
        if (mod_stmt)
          break;
      }
      if (mod_stmt) {
        has_expo_list = module_has_expo_list(mod_stmt);
        if (has_expo_list) {
          collect_module_exports(mod_stmt, &exports);
        }
        if (!has_expo_list || mod_stmt->as.module.expo_all) {
          collect_module_defs(mod_stmt, &exports);
        }
      }
      if (!mod_stmt || exports.len == 0) {
        add_imports_from_prefix(cg, mod);
      } else {
        for (size_t i = 0; i < exports.len; ++i) {
          add_impo_alias_from_full(cg, exports.data[i]);
          free(exports.data[i]);
        }
        vec_free(&exports);
      }
      return;
    }
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      process_use_imports(cg, s->as.module.body.data[i]);
    }
  }
}

void collect_use_aliases(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_USE) {
    if (s->as.use.impo_all || s->as.use.imports.len > 0)
      return;
    const char *alias = s->as.use.alias;
    if (!alias) {
      // Infer alias from module path (last component)
      const char *mod = s->as.use.module;
      const char *dot = strrchr(mod, '.');
      alias = dot ? dot + 1 : mod;
    }
    binding alias_bind = {0};
    alias_bind.name = strdup(alias);
    alias_bind.stmt_t = (stmt_t *)strdup(s->as.use.module);
    // Handle specific imports list: use Mod (a, b as c)
    for (size_t i = 0; i < s->as.use.imports.len; ++i) {
      use_item_t item = s->as.use.imports.data[i];
      const char *target = item.name;
      const char *item_alias = item.alias ? item.alias : item.name;
      // Maps alias -> Module.target
      binding impo_bind = {0};
      impo_bind.name = strdup(item_alias);
      char *full_target =
          malloc(strlen(s->as.use.module) + 1 + strlen(target) + 1);
      sprintf(full_target, "%s.%s", s->as.use.module, target);
      impo_bind.stmt_t = (stmt_t *)full_target;
      vec_push(&cg->impo_aliases, impo_bind);
    }
    vec_push(&cg->aliases, alias_bind);
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      collect_use_aliases(cg, s->as.module.body.data[i]);
  }
}

void collect_use_modules(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_USE) {
    if (!s->as.use.impo_all)
      return;
    const char *mod = s->as.use.module;
    if (mod && *mod) {
      for (size_t i = 0; i < cg->use_modules.len; ++i) {
        if (strcmp(cg->use_modules.data[i], mod) == 0)
          return;
      }
      vec_push(&cg->use_modules, strdup(mod));
    }
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      collect_use_modules(cg, s->as.module.body.data[i]);
  }
}

void process_exports(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_MODULE) {
    const char *mod_name = s->as.module.name;
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
            new_sig.name = strdup(alias);
            vec_push(&cg->fun_sigs, new_sig);
          } else {
            binding *gb = lookup_global(cg, full_target);
            if (!gb)
              gb = lookup_global(cg, target);
            if (gb) {
              binding new_bind = *gb;
              new_bind.name = strdup(alias);
              vec_push(&cg->global_vars, new_bind);
            }
          }
        }
      } else if (child->kind == NY_S_MODULE) {
        // Recurse? Though nesting modules resets name context in parser_t
        // currently, so checking submodule is valid recursively.
        process_exports(cg, child);
      }
    }
  }
}
