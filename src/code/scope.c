#include "base/util.h"
#include "priv.h"

#include <stdlib.h>
#include <string.h>

uint64_t ny_hash_name(const char *s, size_t len) { return ny_hash64(s, len); }

void ny_scope_bloom_add(scope *sc, uint64_t hash) {
  if (!sc)
    return;
  unsigned b0 = (unsigned)(hash & 255u);
  unsigned b1 = (unsigned)((hash >> 8) & 255u);
  sc->name_bloom[b0 >> 6] |= (uint64_t)1u << (b0 & 63u);
  sc->name_bloom[b1 >> 6] |= (uint64_t)1u << (b1 & 63u);
}

bool ny_scope_bloom_maybe_has(const scope *sc, uint64_t hash) {
  if (!sc || sc->vars.len == 0)
    return false;
  unsigned b0 = (unsigned)(hash & 255u);
  unsigned b1 = (unsigned)((hash >> 8) & 255u);
  if ((sc->name_bloom[b0 >> 6] & ((uint64_t)1u << (b0 & 63u))) == 0)
    return false;
  if ((sc->name_bloom[b1 >> 6] & ((uint64_t)1u << (b1 & 63u))) == 0)
    return false;
  return true;
}

uint32_t ny_binding_name_len(binding *b) {
  if (!b || !b->name)
    return 0;
  if (b->name[0] == '\0') {
    b->name_len = 0;
    return 0;
  }
  if (b->name_len)
    return b->name_len;
  size_t n = strlen(b->name);
  b->name_len = (uint32_t)n;
  return (uint32_t)n;
}

uint64_t ny_binding_name_hash(binding *b) {
  if (!b || !b->name)
    return 0;
  if (!b->name_hash)
    b->name_hash = ny_hash_name(b->name, ny_binding_name_len(b));
  return b->name_hash;
}

typedef struct scope_lookup_cache_ref_t {
  const char **lookup_name;
  size_t *lookup_name_len;
  binding **lookup_hit;
  size_t *lookup_vars_len;
  binding **lookup_vars_data;
  const char **lookup_miss_name;
  size_t *lookup_miss_name_len;
  uint64_t *lookup_miss_hash;
  size_t *lookup_miss_vars_len;
  binding **lookup_miss_vars_data;
} scope_lookup_cache_ref_t;

static inline scope_lookup_cache_ref_t scope_lookup_cache_ref(scope *sc, bool mark_used) {
  if (mark_used) {
    return (scope_lookup_cache_ref_t){
        .lookup_name = &sc->lookup_name,
        .lookup_name_len = &sc->lookup_name_len,
        .lookup_hit = &sc->lookup_hit,
        .lookup_vars_len = &sc->lookup_vars_len,
        .lookup_vars_data = &sc->lookup_vars_data,
        .lookup_miss_name = &sc->lookup_miss_name,
        .lookup_miss_name_len = &sc->lookup_miss_name_len,
        .lookup_miss_hash = &sc->lookup_miss_hash,
        .lookup_miss_vars_len = &sc->lookup_miss_vars_len,
        .lookup_miss_vars_data = &sc->lookup_miss_vars_data,
    };
  }
  return (scope_lookup_cache_ref_t){
      .lookup_name = &sc->lookup_name_no_mark,
      .lookup_name_len = &sc->lookup_name_len_no_mark,
      .lookup_hit = &sc->lookup_hit_no_mark,
      .lookup_vars_len = &sc->lookup_vars_len_no_mark,
      .lookup_vars_data = &sc->lookup_vars_data_no_mark,
      .lookup_miss_name = &sc->lookup_miss_name_no_mark,
      .lookup_miss_name_len = &sc->lookup_miss_name_len_no_mark,
      .lookup_miss_hash = &sc->lookup_miss_hash_no_mark,
      .lookup_miss_vars_len = &sc->lookup_miss_vars_len_no_mark,
      .lookup_miss_vars_data = &sc->lookup_miss_vars_data_no_mark,
  };
}

static binding *scope_lookup_impl(scope *scopes, size_t depth, const char *name, size_t name_len,
                                  uint64_t want_hash, bool mark_used) {
  if (!scopes)
    return NULL;
  if (!name || !*name)
    return NULL;
  if (name_len == 0)
    name_len = strlen(name);
  if (want_hash == 0)
    want_hash = ny_hash_name(name, name_len);
  for (ssize_t s = (ssize_t)depth; s >= 0; --s) {
    scope *sc = &scopes[s];
    scope_lookup_cache_ref_t c = scope_lookup_cache_ref(sc, mark_used);
    if (*c.lookup_miss_name && *c.lookup_miss_vars_data == sc->vars.data &&
        *c.lookup_miss_vars_len == sc->vars.len && *c.lookup_miss_name_len == name_len &&
        *c.lookup_miss_hash == want_hash &&
        (*c.lookup_miss_name == name || (memcmp(*c.lookup_miss_name, name, name_len) == 0 &&
                                         (*c.lookup_miss_name)[name_len] == '\0'))) {
      continue;
    }
    if (!ny_scope_bloom_maybe_has(sc, want_hash)) {
      *c.lookup_miss_name = name;
      *c.lookup_miss_name_len = name_len;
      *c.lookup_miss_hash = want_hash;
      *c.lookup_miss_vars_len = sc->vars.len;
      *c.lookup_miss_vars_data = sc->vars.data;
      continue;
    }
    if (*c.lookup_hit && *c.lookup_name && *c.lookup_vars_data == sc->vars.data &&
        *c.lookup_vars_len == sc->vars.len && *c.lookup_name_len == name_len &&
        ny_binding_name_hash(*c.lookup_hit) == want_hash &&
        (*c.lookup_name == name ||
         (memcmp(*c.lookup_name, name, name_len) == 0 && (*c.lookup_name)[name_len] == '\0')) &&
        *c.lookup_hit >= sc->vars.data && *c.lookup_hit < sc->vars.data + sc->vars.len) {
      if (mark_used)
        (*c.lookup_hit)->is_used = true;
      return *c.lookup_hit;
    }
    for (ssize_t i = (ssize_t)sc->vars.len - 1; i >= 0; --i) {
      binding *b = &sc->vars.data[i];
      if (ny_binding_name_hash(b) != want_hash)
        continue;
      if ((size_t)ny_binding_name_len(b) != name_len)
        continue;
      if (b->name == name || (memcmp(b->name, name, name_len) == 0 && b->name[name_len] == '\0')) {
        if (mark_used) {
          b->is_used = true;
        }
        *c.lookup_name = name;
        *c.lookup_name_len = name_len;
        *c.lookup_hit = b;
        *c.lookup_vars_len = sc->vars.len;
        *c.lookup_vars_data = sc->vars.data;
        return b;
      }
    }
    *c.lookup_miss_name = name;
    *c.lookup_miss_name_len = name_len;
    *c.lookup_miss_hash = want_hash;
    *c.lookup_miss_vars_len = sc->vars.len;
    *c.lookup_miss_vars_data = sc->vars.data;
  }
  return NULL;
}

binding *scope_lookup(scope *scopes, size_t depth, const char *name) {
  return scope_lookup_impl(scopes, depth, name, 0, 0, true);
}

static binding *scope_lookup_no_mark(scope *scopes, size_t depth, const char *name) {
  return scope_lookup_impl(scopes, depth, name, 0, 0, false);
}

binding *scope_lookup_hash(scope *scopes, size_t depth, const char *name, size_t name_len,
                           uint64_t hash) {
  return scope_lookup_impl(scopes, depth, name, name_len, hash, true);
}

binding *scope_lookup_hash_no_mark(scope *scopes, size_t depth, const char *name,
                                   size_t name_len, uint64_t hash) {
  return scope_lookup_impl(scopes, depth, name, name_len, hash, false);
}

binding *lookup_binding_hash(codegen_t *cg, scope *scopes, size_t depth, const char *name,
                             size_t name_len, uint64_t hash) {
  if (!name || !*name)
    return NULL;
  if (name_len == 0)
    name_len = strlen(name);
  binding *b = scope_lookup_hash(scopes, depth, name, name_len, hash);
  if (b)
    return b;
  if (hash) {
    b = scope_lookup(scopes, depth, name);
    if (b)
      return b;
  }
  if (!cg)
    return NULL;
  b = lookup_global_hash(cg, name, hash);
  if (b)
    return b;
  return hash ? lookup_global(cg, name) : NULL;
}

static bool ny_user_binding_shadows_builtin(binding *b) {
  return b && (!b->stmt_t || !ny_is_stdlib_tok(b->stmt_t->tok));
}

static bool ny_user_fun_shadows_builtin(const char *name, fun_sig *sig) {
  return sig && ((!sig->stmt_t && !(name && name[0] == '_' && name[1] == '_')) ||
                 (sig->stmt_t && !ny_is_stdlib_tok(sig->stmt_t->tok)));
}

#define NY_BUILTIN_SHADOW_CACHE_SLOTS 8192u
#define NY_BUILTIN_SHADOW_KEY_MAX 96u
#define NY_BUILTIN_SHADOW_CACHE_PROBES 8u

typedef struct ny_builtin_shadow_cache_entry {
  const codegen_t *cg;
  const fun_sig *data;
  size_t fun_len;
  uint64_t hash;
  uint16_t name_len;
  uint8_t state;
  char key[NY_BUILTIN_SHADOW_KEY_MAX];
} ny_builtin_shadow_cache_entry;

static ny_builtin_shadow_cache_entry *ny_builtin_shadow_cache(codegen_t *cg) {
  if (!cg)
    return NULL;
  if (!cg->builtin_shadow_cache) {
    cg->builtin_shadow_cache =
        calloc(NY_BUILTIN_SHADOW_CACHE_SLOTS,
               sizeof(ny_builtin_shadow_cache_entry));
  }
  return (ny_builtin_shadow_cache_entry *)cg->builtin_shadow_cache;
}

static inline uint32_t ny_scope_fun_name_len(fun_sig *fs) {
  return ny_cached_fun_name_len(fs);
}

static inline uint64_t ny_scope_fun_name_hash(fun_sig *fs) {
  if (!fs || !fs->name)
    return 0;
  if (!fs->name_hash)
    fs->name_hash = ny_hash_name(fs->name, ny_scope_fun_name_len(fs));
  return fs->name_hash;
}

static int ny_builtin_shadow_cache_get(codegen_t *cg, const char *name,
                                       size_t name_len, uint64_t hash,
                                       bool *out) {
  if (!cg || !name || !*name || name_len == 0 ||
      name_len >= NY_BUILTIN_SHADOW_KEY_MAX)
    return -1;
  ny_builtin_shadow_cache_entry *cache = ny_builtin_shadow_cache(cg);
  if (!cache)
    return -1;
  size_t fun_len = cg->builtin_shadow_cache_stable_len
                       ? cg->builtin_shadow_cache_stable_len
                       : cg->fun_sigs.len;
  size_t base = hash & (NY_BUILTIN_SHADOW_CACHE_SLOTS - 1u);
  for (size_t probe = 0; probe < NY_BUILTIN_SHADOW_CACHE_PROBES; ++probe) {
    ny_builtin_shadow_cache_entry *e =
        &cache[(base + probe) & (NY_BUILTIN_SHADOW_CACHE_SLOTS - 1u)];
    if (!e->state)
      continue;
    if (e->cg != cg || e->fun_len != fun_len || e->hash != hash ||
        e->name_len != (uint16_t)name_len ||
        memcmp(e->key, name, name_len) != 0 || e->key[name_len] != '\0')
      continue;
    *out = e->state == 2u;
    return 0;
  }
  return -1;
}

static void ny_builtin_shadow_cache_put(codegen_t *cg, const char *name,
                                        size_t name_len, uint64_t hash,
                                        bool value) {
  if (!cg || !name || !*name || name_len == 0 ||
      name_len >= NY_BUILTIN_SHADOW_KEY_MAX)
    return;
  ny_builtin_shadow_cache_entry *cache = ny_builtin_shadow_cache(cg);
  if (!cache)
    return;
  size_t fun_len = cg->builtin_shadow_cache_stable_len
                       ? cg->builtin_shadow_cache_stable_len
                       : cg->fun_sigs.len;
  size_t base = hash & (NY_BUILTIN_SHADOW_CACHE_SLOTS - 1u);
  ny_builtin_shadow_cache_entry *e = &cache[base];
  for (size_t probe = 0; probe < NY_BUILTIN_SHADOW_CACHE_PROBES; ++probe) {
    ny_builtin_shadow_cache_entry *cur =
        &cache[(base + probe) & (NY_BUILTIN_SHADOW_CACHE_SLOTS - 1u)];
    if (!cur->state) {
      e = cur;
      break;
    }
    if (cur->cg == cg && cur->fun_len == fun_len && cur->hash == hash &&
        cur->name_len == (uint16_t)name_len &&
        memcmp(cur->key, name, name_len) == 0 &&
        cur->key[name_len] == '\0') {
      e = cur;
      break;
    }
  }
  e->cg = cg;
  e->data = cg->fun_sigs.data;
  e->fun_len = fun_len;
  e->hash = hash;
  e->name_len = (uint16_t)name_len;
  memcpy(e->key, name, name_len);
  e->key[name_len] = '\0';
  e->state = value ? 2u : 1u;
}

static bool ny_any_user_fun_shadows_builtin(codegen_t *cg, const char *name,
                                            size_t name_len, uint64_t hash) {
  if (!cg || !name || !*name)
    return false;
  if (name_len == 0)
    name_len = strlen(name);
  if (!hash)
    hash = ny_hash_name(name, name_len);
  bool cached = false;
  if (ny_builtin_shadow_cache_get(cg, name, name_len, hash, &cached) == 0)
    return cached;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *fs = &cg->fun_sigs.data[i];
    if (!fs || !fs->name)
      continue;
    if (ny_scope_fun_name_hash(fs) != hash)
      continue;
    if ((size_t)ny_scope_fun_name_len(fs) != name_len)
      continue;
    if (!(fs->name == name || (memcmp(fs->name, name, name_len) == 0 &&
                               fs->name[name_len] == '\0')))
      continue;
    if (ny_user_fun_shadows_builtin(name, fs)) {
      ny_builtin_shadow_cache_put(cg, name, name_len, hash, true);
      return true;
    }
  }
  ny_builtin_shadow_cache_put(cg, name, name_len, hash, false);
  return false;
}

bool ny_builtin_name_shadowed_by_user_symbol(codegen_t *cg, scope *scopes, size_t depth,
                                             const char *name, size_t name_len,
                                             uint64_t hash) {
  if (!name || !*name)
    return true;
  if (name_len == 0)
    name_len = strlen(name);
  if (!hash)
    hash = ny_hash_name(name, name_len);
  if (scope_lookup_hash(scopes, depth, name, name_len, hash))
    return true;
  binding *global = cg ? lookup_global_hash(cg, name, hash) : NULL;
  if (ny_user_binding_shadows_builtin(global))
    return true;
  if (ny_any_user_fun_shadows_builtin(cg, name, name_len, hash))
    return true;
  fun_sig *sig = cg ? lookup_fun(cg, name, hash) : NULL;
  return ny_user_fun_shadows_builtin(name, sig);
}

const char *ny_builtin_surface_name_for_callee(expr_t *callee, size_t *out_len,
                                               uint64_t *out_hash) {
  if (out_len)
    *out_len = 0;
  if (out_hash)
    *out_hash = 0;
  if (!callee || callee->kind != NY_E_IDENT || !callee->as.ident.name)
    return NULL;

  const char *name = callee->as.ident.name;
  size_t name_len = strlen(name);
  const char *surface = name;
  size_t surface_len = (size_t)callee->tok.len;
  uint64_t surface_hash = callee->as.ident.hash;

  const char *dot = strrchr(name, '.');
  if (dot && callee->tok.len > 0 && (size_t)callee->tok.len < name_len) {
    surface = dot + 1;
    surface_len = strlen(surface);
    surface_hash = 0;
  } else {
    if (surface_len == 0 || surface_len > name_len)
      surface_len = name_len;
  }

  if (out_len)
    *out_len = surface_len;
  if (out_hash)
    *out_hash = surface_hash;
  return surface;
}

binding *lookup_binding_hash_no_mark(scope *scopes, size_t depth, const char *name,
                                     size_t name_len, uint64_t hash) {
  if (!name || !*name)
    return NULL;
  if (name_len == 0)
    name_len = strlen(name);
  binding *b = scope_lookup_hash_no_mark(scopes, depth, name, name_len, hash);
  if (b)
    return b;
  return hash ? scope_lookup_no_mark(scopes, depth, name) : NULL;
}

static void scope_cache_clear(scope_lookup_cache_ref_t c) {
  *c.lookup_name = NULL;
  *c.lookup_name_len = 0;
  *c.lookup_hit = NULL;
  *c.lookup_vars_len = 0;
  *c.lookup_vars_data = NULL;
  *c.lookup_miss_name = NULL;
  *c.lookup_miss_name_len = 0;
  *c.lookup_miss_hash = 0;
  *c.lookup_miss_vars_len = 0;
  *c.lookup_miss_vars_data = NULL;
}

static void scope_lookup_cache_reset(scope *sc) {
  scope_cache_clear(scope_lookup_cache_ref(sc, true));
  scope_cache_clear(scope_lookup_cache_ref(sc, false));
}

void scope_bind(codegen_t *cg, scope *scopes, size_t depth, const char *name, LLVMValueRef v,
                stmt_t *stmt, bool is_mut, const char *type_name, bool is_slot) {
  static int warn_stdlib_shadow_cached = -1;
  if (warn_stdlib_shadow_cached < 0) {
    const char *env = getenv("NYTRIX_WARN_STDLIB_SHADOW");
    warn_stdlib_shadow_cached =
        (env && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0 || strcmp(env, "on") == 0))
            ? 1
            : 0;
  }
  size_t name_len = strlen(name);
  uint64_t want_hash = ny_hash_name(name, name_len);
  if (ny_scope_bloom_maybe_has(&scopes[depth], want_hash)) {
    for (size_t i = 0; i < scopes[depth].vars.len; i++) {
      binding *cur = &scopes[depth].vars.data[i];
      if (ny_binding_name_hash(cur) != want_hash)
        continue;
      if ((size_t)ny_binding_name_len(cur) != name_len)
        continue;
      if (cur->name == name ||
          (memcmp(cur->name, name, name_len) == 0 && cur->name[name_len] == '\0')) {
        binding *prev = cur;
        if (stmt) {
          ny_diag_error(stmt->tok, "redefinition of %s'%s'%s in the same scope", clr(NY_CLR_BOLD),
                        name, clr(NY_CLR_RESET));
          ny_diag_fix("use a different name, or %smut %s = ...%s to reassign", clr(NY_CLR_BOLD),
                      name, clr(NY_CLR_RESET));
          if (prev->stmt_t)
            ny_diag_note_tok(prev->stmt_t->tok, "previous definition here");
        } else {
          ny_diag_error((token_t){0}, "redefinition of %s'%s'%s in argument list", clr(NY_CLR_BOLD),
                        name, clr(NY_CLR_RESET));
          ny_diag_hint("each parameter name must be unique");
        }
      }
    }
  }
  if (depth > 0) {
    binding *shadow = scope_lookup_no_mark(scopes, depth - 1, name);
    bool shadow_is_stdlib = stmt && ny_is_stdlib_tok(stmt->tok);
    if (shadow && stmt && (!shadow_is_stdlib || warn_stdlib_shadow_cached) &&
        ny_diag_should_emit("shadow_local", stmt->tok, name)) {
      ny_diag_warning_code(stmt->tok, 2002, "declaration of %s'%s'%s shadows a previous local",
                           clr(NY_CLR_BOLD), name, clr(NY_CLR_RESET));
      ny_diag_hint("this may hide the outer variable — use a different name if unintended");
      if (shadow->stmt_t)
        ny_diag_note_tok(shadow->stmt_t->tok, "previous '%s' declared here", name);
    }
  }
  binding b = {0};
  b.name = name;
  b.value = v;
  b.stmt_t = stmt;
  b.is_slot = is_slot;
  b.is_mut = is_mut;
  b.type_name = type_name;
  b.decl_type_name = type_name;
  b.name_hash = want_hash;
  b.name_len = (uint32_t)name_len;
  vec_push(&scopes[depth].vars, b);
  ny_scope_bloom_add(&scopes[depth], want_hash);
  scope_lookup_cache_reset(&scopes[depth]);
  if (!(stmt && stmt->kind == NY_S_FUNC))
    codegen_debug_variable(cg, name, type_name, v, stmt ? stmt->tok : (token_t){0}, false, 0,
                           is_slot);
}

void scope_pop(scope *scopes, size_t *depth) {
  static int warn_stdlib_unused_cached = -1;
  if (warn_stdlib_unused_cached < 0) {
    const char *env = getenv("NYTRIX_WARN_STDLIB_UNUSED");
    warn_stdlib_unused_cached =
        (env && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0 || strcmp(env, "on") == 0))
            ? 1
            : 0;
  }
  for (size_t i = 0; i < scopes[*depth].vars.len; i++) {
    binding *b = &scopes[*depth].vars.data[i];
    if (!b->stmt_t)
      continue;
    bool is_stdlib = ny_is_stdlib_tok(b->stmt_t->tok);
    bool is_param = (b->stmt_t->kind == NY_S_FUNC);
    if (is_stdlib && !warn_stdlib_unused_cached)
      continue;
    if (!b->is_used && b->name && b->name[0] != '_' && !is_param &&
        ny_diag_should_emit("unused_var", b->stmt_t->tok, b->name)) {
      ny_diag_warning_code(b->stmt_t->tok, 2001, "unused variable %s'%s'%s", clr(NY_CLR_BOLD),
                           b->name, clr(NY_CLR_RESET));
    }
  }
  vec_free(&scopes[*depth].defers);
  vec_free(&scopes[*depth].vars);
  (*depth)--;
}
