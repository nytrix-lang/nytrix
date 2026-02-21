#include "base/util.h"
#include "priv.h"

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
  if (b->name_len && b->name_hash)
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

static inline scope_lookup_cache_ref_t scope_lookup_cache_ref(scope *sc,
                                                              bool mark_used) {
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

static binding *scope_lookup_impl(scope *scopes, size_t depth, const char *name,
                                  bool mark_used) {
  if (!name || !*name)
    return NULL;
  size_t name_len = strlen(name);
  uint64_t want_hash = ny_hash_name(name, name_len);
  for (ssize_t s = (ssize_t)depth; s >= 0; --s) {
    scope *sc = &scopes[s];
    scope_lookup_cache_ref_t c = scope_lookup_cache_ref(sc, mark_used);
    if (*c.lookup_miss_name && *c.lookup_miss_vars_data == sc->vars.data &&
        *c.lookup_miss_vars_len == sc->vars.len &&
        *c.lookup_miss_name_len == name_len &&
        *c.lookup_miss_hash == want_hash &&
        (*c.lookup_miss_name == name ||
         (memcmp(*c.lookup_miss_name, name, name_len) == 0 &&
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
    if (*c.lookup_hit && *c.lookup_name &&
        *c.lookup_vars_data == sc->vars.data &&
        *c.lookup_vars_len == sc->vars.len && *c.lookup_name_len == name_len &&
        ny_binding_name_hash(*c.lookup_hit) == want_hash &&
        (*c.lookup_name == name ||
         (memcmp(*c.lookup_name, name, name_len) == 0 &&
          (*c.lookup_name)[name_len] == '\0')) &&
        *c.lookup_hit >= sc->vars.data &&
        *c.lookup_hit < sc->vars.data + sc->vars.len) {
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
      if (b->name == name ||
          (memcmp(b->name, name, name_len) == 0 && b->name[name_len] == '\0')) {
        if (mark_used) {
          NY_LOG_DEBUG("scope_lookup: found '%s' at depth %zd, is_mut=%d\n",
                       name, s, b->is_mut);
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
  return scope_lookup_impl(scopes, depth, name, true);
}

static binding *scope_lookup_no_mark(scope *scopes, size_t depth,
                                     const char *name) {
  return scope_lookup_impl(scopes, depth, name, false);
}

static void scope_lookup_cache_reset(scope *sc) {
  sc->lookup_name = NULL;
  sc->lookup_name_len = 0;
  sc->lookup_hit = NULL;
  sc->lookup_vars_len = 0;
  sc->lookup_vars_data = NULL;
  sc->lookup_miss_name = NULL;
  sc->lookup_miss_name_len = 0;
  sc->lookup_miss_hash = 0;
  sc->lookup_miss_vars_len = 0;
  sc->lookup_miss_vars_data = NULL;

  sc->lookup_name_no_mark = NULL;
  sc->lookup_name_len_no_mark = 0;
  sc->lookup_hit_no_mark = NULL;
  sc->lookup_vars_len_no_mark = 0;
  sc->lookup_vars_data_no_mark = NULL;
  sc->lookup_miss_name_no_mark = NULL;
  sc->lookup_miss_name_len_no_mark = 0;
  sc->lookup_miss_hash_no_mark = 0;
  sc->lookup_miss_vars_len_no_mark = 0;
  sc->lookup_miss_vars_data_no_mark = NULL;
}

void scope_bind(codegen_t *cg, scope *scopes, size_t depth, const char *name,
                LLVMValueRef v, stmt_t *stmt, bool is_mut,
                const char *type_name, bool is_slot) {
  size_t name_len = strlen(name);
  uint64_t want_hash = ny_hash_name(name, name_len);
  if (ny_scope_bloom_maybe_has(&scopes[depth], want_hash)) {
    for (size_t i = 0; i < scopes[depth].vars.len; i++) {
      binding *cur = &scopes[depth].vars.data[i];
      if (ny_binding_name_hash(cur) != want_hash)
        continue;
      if ((size_t)ny_binding_name_len(cur) != name_len)
        continue;
      if (cur->name == name || (memcmp(cur->name, name, name_len) == 0 &&
                                cur->name[name_len] == '\0')) {
        binding *prev = cur;
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
  }

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

  binding b = {
      name,      v,         stmt,      is_slot,           is_mut, false, false,
      type_name, type_name, want_hash, (uint32_t)name_len};
  vec_push(&scopes[depth].vars, b);
  ny_scope_bloom_add(&scopes[depth], want_hash);
  scope_lookup_cache_reset(&scopes[depth]);

  codegen_debug_variable(cg, name, v, stmt ? stmt->tok : (token_t){0}, false, 0,
                         is_slot);
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
