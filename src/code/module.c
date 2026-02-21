#include "base/util.h"
#include "priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ny_import_alias_has_name(const codegen_t *cg, bool user_only,
                                     const char *name) {
  if (!cg || !name || !*name)
    return false;
  binding *data =
      user_only ? cg->user_import_aliases.data : cg->import_aliases.data;
  size_t len = user_only ? cg->user_import_aliases.len : cg->import_aliases.len;
  for (size_t i = 0; i < len; ++i) {
    if (data[i].name && strcmp(data[i].name, name) == 0)
      return true;
  }
  return false;
}

static void ny_push_import_alias_unique(codegen_t *cg, bool user_only,
                                        const char *alias,
                                        const char *full_name) {
  if (!cg || !alias || !*alias || !full_name || !*full_name)
    return;
  if (ny_import_alias_has_name(cg, user_only, alias))
    return;
  binding alias_bind = {0};
  alias_bind.name = ny_strdup(alias);
  alias_bind.stmt_t = (stmt_t *)ny_strdup(full_name);
  if (user_only)
    vec_push(&cg->user_import_aliases, alias_bind);
  else
    vec_push(&cg->import_aliases, alias_bind);
}

static void ny_push_import_alias_from_full_unique(codegen_t *cg, bool user_only,
                                                  const char *full_name) {
  if (!full_name || !*full_name)
    return;
  const char *last_dot = strrchr(full_name, '.');
  const char *alias = last_dot ? last_dot + 1 : full_name;
  ny_push_import_alias_unique(cg, user_only, alias, full_name);
}

void add_import_alias(codegen_t *cg, const char *alias, const char *full_name) {
  ny_push_import_alias_unique(cg, false, alias, full_name);
}

void add_import_alias_from_full(codegen_t *cg, const char *full_name) {
  ny_push_import_alias_from_full_unique(cg, false, full_name);
}

static void add_user_import_alias(codegen_t *cg, const char *alias,
                                  const char *full_name) {
  ny_push_import_alias_unique(cg, true, alias, full_name);
}

static void add_user_import_alias_from_full(codegen_t *cg,
                                            const char *full_name) {
  ny_push_import_alias_from_full_unique(cg, true, full_name);
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
      for (size_t j = 0; j < child->as.var.names.len; ++j)
        vec_push(exports, ny_strdup(child->as.var.names.data[j]));
    }
  }
}

void add_imports_from_prefix(codegen_t *cg, const char *mod) {
  if (!mod || !*mod)
    return;
  size_t mod_len = strlen(mod);
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    const char *name = cg->fun_sigs.data[i].name;
    if (strncmp(name, mod, mod_len) == 0 && name[mod_len] == '.')
      add_import_alias_from_full(cg, name);
  }
  for (size_t i = 0; i < cg->global_vars.len; ++i) {
    const char *name = cg->global_vars.data[i].name;
    if (strncmp(name, mod, mod_len) == 0 && name[mod_len] == '.')
      add_import_alias_from_full(cg, name);
  }
}

static bool ny_use_module_has(const codegen_t *cg, bool user_only,
                              const char *value) {
  if (!cg || !value || !*value)
    return false;
  char **data = user_only ? cg->user_use_modules.data : cg->use_modules.data;
  size_t len = user_only ? cg->user_use_modules.len : cg->use_modules.len;
  for (size_t i = 0; i < len; ++i) {
    if (data[i] && strcmp(data[i], value) == 0)
      return true;
  }
  return false;
}

static void ny_push_use_module_unique(codegen_t *cg, bool user_only,
                                      const char *value) {
  if (!cg || !value || !*value)
    return;
  if (ny_use_module_has(cg, user_only, value))
    return;
  if (user_only)
    vec_push(&cg->user_use_modules, ny_strdup(value));
  else
    vec_push(&cg->use_modules, ny_strdup(value));
}

char *normalize_module_name(const char *raw) {
  if (!raw)
    return NULL;
  if (strchr(raw, '/')) {
    const char *last_slash = strrchr(raw, '/');
    const char *start = last_slash ? last_slash + 1 : raw;
    char *name = ny_strdup(start);
    char *dot = strrchr(name, '.');
    if (dot && dot != name)
      *dot = '\0';
    return name;
  }
  return ny_strdup(raw);
}

static void ny_add_import_aliases(codegen_t *cg, bool user_use,
                                  const char *alias, const char *full_name) {
  add_import_alias(cg, alias, full_name);
  if (user_use)
    add_user_import_alias(cg, alias, full_name);
}

static void ny_add_import_aliases_from_full(codegen_t *cg, bool user_use,
                                            const char *full_name) {
  add_import_alias_from_full(cg, full_name);
  if (user_use)
    add_user_import_alias_from_full(cg, full_name);
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
        ny_add_import_aliases(cg, user_use, alias, full);
        free(full);
      }
      free(mod);
      return;
    }
    if (s->as.use.import_all) {
      str_list exports = {0};
      bool has_export_list = false;
      stmt_t *mod_stmt = NULL;
      for (size_t i = 0; i < cg->prog->body.len; ++i) {
        mod_stmt = find_module_stmt(cg->prog->body.data[i], mod);
        if (mod_stmt)
          break;
      }
      if (mod_stmt) {
        has_export_list = module_has_export_list(mod_stmt);
        if (has_export_list)
          collect_module_exports(mod_stmt, &exports);
        if (!has_export_list || mod_stmt->as.module.export_all)
          collect_module_defs(mod_stmt, &exports);
      }
      if (!mod_stmt || exports.len == 0) {
        add_imports_from_prefix(cg, mod);
      } else {
        for (size_t i = 0; i < exports.len; ++i) {
          ny_add_import_aliases_from_full(cg, user_use, exports.data[i]);
          free(exports.data[i]);
        }
        vec_free(&exports);
      }
      free(mod);
      return;
    }
    free(mod);
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      process_use_imports(cg, s->as.module.body.data[i]);
  }
}

void collect_use_aliases(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_USE) {
    if (s->as.use.import_all || s->as.use.imports.len > 0)
      return;
    const char *alias = s->as.use.alias;
    char *mod = normalize_module_name(s->as.use.module);
    if (!alias) {
      const char *dot = strrchr(mod, '.');
      alias = dot ? dot + 1 : mod;
    }
    binding alias_bind = {0};
    alias_bind.name = ny_strdup(alias);
    alias_bind.stmt_t = (stmt_t *)ny_strdup(mod);
    vec_push(&cg->aliases, alias_bind);
    free(mod);
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      collect_use_aliases(cg, s->as.module.body.data[i]);
  }
}

void collect_use_modules(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_USE) {
    if (!s->as.use.import_all && s->as.use.imports.len > 0)
      return;
    const char *mod = s->as.use.module;
    if (mod && *mod) {
      ny_push_use_module_unique(cg, false, mod);
      if (!ny_is_stdlib_tok(s->tok))
        ny_push_use_module_unique(cg, true, mod);
    }
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      collect_use_modules(cg, s->as.module.body.data[i]);
  }
}

static void ny_export_aliased_symbol(codegen_t *cg, const char *mod_name,
                                     const char *target) {
  char alias[256];
  snprintf(alias, sizeof(alias), "%s.%s", mod_name, target);
  char full_target[256];
  snprintf(full_target, sizeof(full_target), "%s.%s", mod_name, target);
  fun_sig *fs = lookup_fun_exact(cg, full_target);
  if (!fs)
    fs = lookup_fun(cg, target);
  if (fs) {
    fun_sig new_sig = *fs;
    new_sig.name = ny_strdup(alias);
    new_sig.name_hash = 0;
    vec_push(&cg->fun_sigs, new_sig);
    return;
  }
  binding *gb = lookup_global_exact(cg, full_target);
  if (!gb)
    gb = lookup_global(cg, target);
  if (gb) {
    binding new_bind = *gb;
    new_bind.name = ny_strdup(alias);
    new_bind.name_hash = 0;
    vec_push(&cg->global_vars, new_bind);
  }
}

void process_exports(codegen_t *cg, stmt_t *s) {
  if (s->kind != NY_S_MODULE)
    return;
  const char *prev_mod = cg->current_module_name;
  cg->current_module_name = s->as.module.name;
  const char *mod_name = s->as.module.name;
  for (size_t i = 0; i < s->as.module.body.len; ++i) {
    stmt_t *child = s->as.module.body.data[i];
    if (child->kind == NY_S_EXPORT) {
      for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
        ny_export_aliased_symbol(cg, mod_name, child->as.exprt.names.data[j]);
      }
    } else if (child->kind == NY_S_MODULE) {
      process_exports(cg, child);
    }
  }
  cg->current_module_name = prev_mod;
}
