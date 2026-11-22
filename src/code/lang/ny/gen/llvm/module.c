/*
 * Module codegen: compiles Nytrix module declarations to LLVM IR,
 * managing module-level metadata, imports, and symbol visibility.
 */
#include "base/loader.h"
#include "base/trace.h"
#include "base/util.h"
#include "code/lang/ny/parser/visitor.h"
#include "code/lang/ny/gen/internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NY_MODULE_STMT_LOOKUP_CACHE_SLOTS 8192u
#define NY_MODULE_STMT_LOOKUP_KEY_MAX 192u
#define NY_MODULE_PUBLIC_TARGET_CACHE_SLOTS 4096u
#define NY_MODULE_PUBLIC_TARGET_KEY_MAX 128u
#define NY_MODULE_PUBLIC_TARGET_PROFILE_MAX 32u
#define NY_MODULE_PUBLIC_TARGET_VALUE_MAX 1024u
#define NY_MODULE_LOOKUP_CACHE_PROBES 8u

static void ny_module_trace_event(const char *tag, const char *fmt, ...) {
  char detail[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  size_t len = strlen(detail);
  if (len && detail[len - 1] == '\n')
    detail[len - 1] = '\0';
  ny_trace_line(tag, "%s", detail);
}

typedef struct ny_module_public_target_entry_t {
  const codegen_t *cg;
  const stmt_t *mod;
  const void *body_data;
  size_t body_len;
  uint64_t hash;
  uint16_t target_len;
  uint8_t profile_len;
  uint8_t include_child_uses;
  uint8_t state;
  uint16_t value_len;
  char target[NY_MODULE_PUBLIC_TARGET_KEY_MAX];
  char profile[NY_MODULE_PUBLIC_TARGET_PROFILE_MAX];
  char value[NY_MODULE_PUBLIC_TARGET_VALUE_MAX];
} ny_module_public_target_entry_t;

typedef struct ny_module_public_target_cache_t {
  ny_module_public_target_entry_t entries[NY_MODULE_PUBLIC_TARGET_CACHE_SLOTS];
} ny_module_public_target_cache_t;

static inline uint64_t ny_module_mix64(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

static size_t ny_alias_index_min_cap(size_t len) {
  size_t cap = 64;
  while (cap < len * 2u)
    cap <<= 1u;
  return cap;
}

static import_alias_slot **ny_alias_index_slots(codegen_t *cg, bool user_only) {
  return user_only ? &cg->user_import_alias_index : &cg->import_alias_index;
}

static size_t *ny_alias_index_cap_ptr(codegen_t *cg, bool user_only) {
  return user_only ? &cg->user_import_alias_index_cap
                   : &cg->import_alias_index_cap;
}

static binding *ny_alias_bindings(codegen_t *cg, bool user_only) {
  return user_only ? cg->user_import_aliases.data : cg->import_aliases.data;
}

static size_t ny_alias_binding_len(codegen_t *cg, bool user_only) {
  return user_only ? cg->user_import_aliases.len : cg->import_aliases.len;
}

static void ny_alias_index_insert_existing(import_alias_slot *slots, size_t cap,
                                           binding *data, size_t idx,
                                           uint64_t hash) {
  if (!slots || cap == 0 || !data || idx == (size_t)-1 || !data[idx].name)
    return;
  size_t mask = cap - 1u;
  size_t pos = hash & mask;
  for (;;) {
    import_alias_slot *slot = &slots[pos];
    if (!slot->occupied) {
      slot->occupied = true;
      slot->hash = hash;
      slot->index = idx;
      return;
    }
    if (slot->hash == hash && data[slot->index].name &&
        strcmp(data[slot->index].name, data[idx].name) == 0)
      return;
    pos = (pos + 1u) & mask;
  }
}

static void ny_alias_index_rebuild(codegen_t *cg, bool user_only,
                                   size_t min_cap) {
  if (!cg)
    return;
  binding *data = ny_alias_bindings(cg, user_only);
  size_t len = ny_alias_binding_len(cg, user_only);
  size_t cap = ny_alias_index_min_cap(len + 1u);
  if (cap < min_cap)
    cap = ny_alias_index_min_cap(min_cap);
  import_alias_slot *slots = calloc(cap, sizeof(import_alias_slot));
  if (!slots)
    return;
  assigned_hash_list *hashes =
      user_only ? &cg->user_import_alias_hashes : &cg->import_alias_hashes;
  for (size_t i = 0; i < len; ++i) {
    if (!data[i].name)
      continue;
    uint64_t hash = 0;
    if (hashes && hashes->len == len) {
      hash = hashes->data[i];
    } else {
      size_t n = data[i].name_len ? data[i].name_len : strlen(data[i].name);
      hash =
          data[i].name_hash ? data[i].name_hash : ny_hash_name(data[i].name, n);
      data[i].name_len = (uint32_t)n;
      data[i].name_hash = hash;
    }
    ny_alias_index_insert_existing(slots, cap, data, i, hash);
  }
  import_alias_slot **slot_ptr = ny_alias_index_slots(cg, user_only);
  size_t *cap_ptr = ny_alias_index_cap_ptr(cg, user_only);
  free(*slot_ptr);
  *slot_ptr = slots;
  *cap_ptr = cap;
}

static void ny_module_alias_index_insert_existing(import_alias_slot *slots,
                                                  size_t cap, binding *data,
                                                  size_t idx, uint64_t hash) {
  if (!slots || cap == 0 || !data || idx == (size_t)-1 || !data[idx].name)
    return;
  size_t mask = cap - 1u;
  size_t pos = hash & mask;
  for (;;) {
    import_alias_slot *slot = &slots[pos];
    if (!slot->occupied) {
      slot->occupied = true;
      slot->hash = hash;
      slot->index = idx;
      return;
    }
    if (slot->hash == hash && data[slot->index].name &&
        strcmp(data[slot->index].name, data[idx].name) == 0) {
      return;
    }
    pos = (pos + 1u) & mask;
  }
}

static void ny_module_alias_index_rebuild(codegen_t *cg) {
  if (!cg)
    return;
  size_t len = cg->aliases.len;
  size_t cap = ny_alias_index_min_cap(len + 1u);
  import_alias_slot *slots = calloc(cap, sizeof(import_alias_slot));
  if (!slots)
    return;
  for (size_t i = len; i > 0; --i) {
    size_t idx = i - 1u;
    binding *b = &cg->aliases.data[idx];
    if (!b->name)
      continue;
    size_t n = b->name_len ? b->name_len : strlen(b->name);
    uint64_t hash = b->name_hash ? b->name_hash : ny_hash_name(b->name, n);
    b->name_len = (uint32_t)n;
    b->name_hash = hash;
    ny_module_alias_index_insert_existing(slots, cap, cg->aliases.data, idx,
                                          hash);
  }
  free(cg->module_alias_index);
  cg->module_alias_index = slots;
  cg->module_alias_index_cap = cap;
  cg->module_alias_index_len = len;
}

static const char *ny_lookup_module_alias_linear(codegen_t *cg,
                                                 const char *name,
                                                 size_t name_len) {
  if (!cg || !name || !*name)
    return NULL;
  for (size_t i = cg->aliases.len; i > 0; --i) {
    binding *al = &cg->aliases.data[i - 1];
    if (!al->name)
      continue;
    size_t alias_len = al->name_len ? al->name_len : strlen(al->name);
    if (alias_len == name_len && strncmp(al->name, name, name_len) == 0) {
      al->is_used = true;
      return (const char *)al->stmt_t;
    }
  }
  return NULL;
}

static const char *ny_lookup_module_alias_indexed(codegen_t *cg,
                                                  const char *name,
                                                  size_t name_len,
                                                  uint64_t name_hash) {
  if (!cg || !name || !*name)
    return NULL;
  if (name_len == 0)
    name_len = strlen(name);
  if (!name_hash)
    name_hash = ny_hash_name(name, name_len);
  if (!cg->module_alias_index ||
      cg->module_alias_index_len != cg->aliases.len) {
    ny_module_alias_index_rebuild(cg);
  }
  import_alias_slot *slots = cg->module_alias_index;
  size_t cap = cg->module_alias_index_cap;
  if (!slots || cap == 0)
    return ny_lookup_module_alias_linear(cg, name, name_len);

  size_t mask = cap - 1u;
  size_t pos = name_hash & mask;
  for (;;) {
    import_alias_slot *slot = &slots[pos];
    if (!slot->occupied)
      return NULL;
    if (slot->hash == name_hash && slot->index < cg->aliases.len) {
      binding *al = &cg->aliases.data[slot->index];
      if (al->name) {
        size_t alias_len = al->name_len ? al->name_len : strlen(al->name);
        if (alias_len == name_len && strncmp(al->name, name, name_len) == 0) {
          al->is_used = true;
          return (const char *)al->stmt_t;
        }
      }
    }
    pos = (pos + 1u) & mask;
  }
}

static long ny_alias_index_find(codegen_t *cg, bool user_only,
                                const char *alias, size_t alias_len,
                                uint64_t alias_hash) {
  if (!cg || !alias || !*alias)
    return -1;
  size_t len = ny_alias_binding_len(cg, user_only);
  import_alias_slot **slot_ptr = ny_alias_index_slots(cg, user_only);
  size_t *cap_ptr = ny_alias_index_cap_ptr(cg, user_only);
  if (!*slot_ptr || *cap_ptr == 0 || len * 10u >= (*cap_ptr) * 7u)
    ny_alias_index_rebuild(cg, user_only, len + 1u);
  import_alias_slot *slots = *slot_ptr;
  size_t cap = *cap_ptr;
  binding *data = ny_alias_bindings(cg, user_only);
  if (!slots || cap == 0 || !data)
    return -1;
  size_t mask = cap - 1u;
  size_t pos = alias_hash & mask;
  for (;;) {
    import_alias_slot *slot = &slots[pos];
    if (!slot->occupied)
      return -1;
    if (slot->hash == alias_hash && slot->index < len) {
      binding *b = &data[slot->index];
      if (b->name) {
        uint32_t cur_len = b->name_len;
        if (!cur_len) {
          cur_len = (uint32_t)strlen(b->name);
          b->name_len = cur_len;
        }
        if ((size_t)cur_len == alias_len &&
            memcmp(b->name, alias, alias_len) == 0 &&
            b->name[alias_len] == '\0')
          return (long)slot->index;
      }
    }
    pos = (pos + 1u) & mask;
  }
}

static void ny_alias_index_insert_new(codegen_t *cg, bool user_only, size_t idx,
                                      uint64_t alias_hash) {
  if (!cg)
    return;
  size_t len = ny_alias_binding_len(cg, user_only);
  import_alias_slot **slot_ptr = ny_alias_index_slots(cg, user_only);
  size_t *cap_ptr = ny_alias_index_cap_ptr(cg, user_only);
  if (!*slot_ptr || *cap_ptr == 0 || len * 10u >= (*cap_ptr) * 7u)
    ny_alias_index_rebuild(cg, user_only, len + 1u);
  if (*slot_ptr && *cap_ptr)
    ny_alias_index_insert_existing(
        *slot_ptr, *cap_ptr, ny_alias_bindings(cg, user_only), idx, alias_hash);
}

static void ny_push_import_alias_unique_ex(codegen_t *cg, bool user_only,
                                           const char *alias,
                                           const char *full_name,
                                           bool replace_existing) {
  if (!cg || !alias || !*alias || !full_name || !*full_name)
    return;
  binding *data =
      user_only ? cg->user_import_aliases.data : cg->import_aliases.data;
  assigned_hash_list *hashes =
      user_only ? &cg->user_import_alias_hashes : &cg->import_alias_hashes;
  uint64_t *bloom =
      user_only ? cg->user_import_alias_bloom : cg->import_alias_bloom;
  size_t alias_len = strlen(alias);
  uint64_t alias_hash = ny_hash_name(alias, alias_len);
  long found_idx =
      ny_alias_bloom_maybe_has((const uint64_t *)bloom, alias_hash)
          ? ny_alias_index_find(cg, user_only, alias, alias_len, alias_hash)
          : -1;
  if (found_idx >= 0) {
    size_t i = (size_t)found_idx;
    if (!data[i].name)
      return;
    uint32_t cur_len = data[i].name_len;
    if (!cur_len) {
      size_t n = strlen(data[i].name);
      cur_len = (uint32_t)n;
      data[i].name_len = cur_len;
    }
    if ((size_t)cur_len != alias_len)
      return;
    uint64_t cur_hash = data[i].name_hash;
    if (!cur_hash) {
      cur_hash = ny_hash_name(data[i].name, cur_len);
      data[i].name_hash = cur_hash;
    }
    if (cur_hash != alias_hash)
      return;
    if (memcmp(data[i].name, alias, alias_len) != 0 ||
        data[i].name[alias_len] != '\0')
      return;
    const char *cur = (const char *)data[i].stmt_t;
    if (cur && strcmp(cur, full_name) == 0)
      return;
    bool placeholder_self =
        cur && strcmp(cur, alias) == 0 && strcmp(full_name, alias) != 0;
    if (!replace_existing && !placeholder_self)
      return;
    if (!data[i].owned) {
      data[i].name = ny_strdup(data[i].name);
      data[i].owned = true;
    } else {
      free(data[i].stmt_t);
    }
    data[i].stmt_t = (stmt_t *)ny_strdup(full_name);
    ny_alias_lookup_cache_clear(cg);
    return;
  }
  binding alias_bind = {0};
  alias_bind.name = ny_strdup(alias);
  alias_bind.name_len = (uint32_t)alias_len;
  alias_bind.name_hash = alias_hash;
  alias_bind.stmt_t = (stmt_t *)ny_strdup(full_name);
  alias_bind.owned = true;
  if (user_only)
    vec_push(&cg->user_import_aliases, alias_bind);
  else
    vec_push(&cg->import_aliases, alias_bind);
  size_t new_idx = user_only ? cg->user_import_aliases.len - 1u
                             : cg->import_aliases.len - 1u;
  if (hashes) {
    vec_push(hashes, alias_hash);
    ny_alias_bloom_add(bloom, alias_hash);
  }
  ny_alias_index_insert_new(cg, user_only, new_idx, alias_hash);
}

/*
 * A qualified lookup can bypass the import-alias table. Mark its exact source
 * alias by name and target; scanning every imported export on every symbol
 * lookup makes broad module imports quadratic.
 */
void ny_mark_import_alias_used(codegen_t *cg, const char *alias,
                               const char *target) {
  if (!cg || !alias || !*alias || !target || !*target)
    return;
  size_t alias_len = strlen(alias);
  uint64_t alias_hash = ny_hash_name(alias, alias_len);
  long idx = ny_alias_index_find(cg, true, alias, alias_len, alias_hash);
  if (idx >= 0 && (size_t)idx < cg->user_import_aliases.len) {
    binding *b = &cg->user_import_aliases.data[idx];
    const char *bound = b->stmt_t ? (const char *)b->stmt_t : NULL;
    if (bound && strcmp(bound, target) == 0)
      b->is_used = true;
  }
  /*
   * Comptime and module snapshots use child codegen contexts.  Their symbol
   * tables are local, but import diagnostics belong to the owning context;
   * propagate this exact alias through the context chain.
   */
  if (cg->parent)
    ny_mark_import_alias_used(cg->parent, alias, target);
}

const char *ny_lookup_import_alias_indexed(codegen_t *cg, bool user_only,
                                           const char *alias, size_t alias_len,
                                           uint64_t alias_hash) {
  if (!cg || !alias || !*alias)
    return NULL;
  if (alias_len == 0)
    alias_len = strlen(alias);
  if (alias_hash == 0)
    alias_hash = ny_hash_name(alias, alias_len);

  binding *data = ny_alias_bindings(cg, user_only);
  size_t len = ny_alias_binding_len(cg, user_only);
  assigned_hash_list *hashes =
      user_only ? &cg->user_import_alias_hashes : &cg->import_alias_hashes;
  const uint64_t *bloom =
      user_only ? cg->user_import_alias_bloom : cg->import_alias_bloom;
  bool can_use_bloom = hashes && hashes->len == len;
  if (can_use_bloom && !ny_alias_bloom_maybe_has(bloom, alias_hash))
    return NULL;

  long found = ny_alias_index_find(cg, user_only, alias, alias_len, alias_hash);
  if (found >= 0 && (size_t)found < len && data && data[found].name) {
    data[found].is_used = true;
    return (const char *)data[found].stmt_t;
  }

  import_alias_slot **slot_ptr = ny_alias_index_slots(cg, user_only);
  size_t *cap_ptr = ny_alias_index_cap_ptr(cg, user_only);
  if (slot_ptr && *slot_ptr && cap_ptr && *cap_ptr)
    return NULL;

  if (!data)
    return NULL;
  for (size_t i = 0; i < len; ++i) {
    if (!data[i].name)
      continue;
    uint64_t cur_hash = data[i].name_hash;
    if (!cur_hash) {
      size_t cur_len =
          data[i].name_len ? data[i].name_len : strlen(data[i].name);
      cur_hash = ny_hash_name(data[i].name, cur_len);
      data[i].name_len = (uint32_t)cur_len;
      data[i].name_hash = cur_hash;
    }
    if (cur_hash != alias_hash)
      continue;
    uint32_t cur_len = data[i].name_len;
    if (!cur_len) {
      cur_len = (uint32_t)strlen(data[i].name);
      data[i].name_len = cur_len;
    }
    if ((size_t)cur_len == alias_len &&
        memcmp(data[i].name, alias, alias_len) == 0 &&
        data[i].name[alias_len] == '\0') {
      data[i].is_used = true;
      return (const char *)data[i].stmt_t;
    }
  }
  return NULL;
}

static void ny_push_import_alias_unique(codegen_t *cg, bool user_only,
                                        const char *alias,
                                        const char *full_name) {
  ny_push_import_alias_unique_ex(cg, user_only, alias, full_name, true);
}

static void ny_push_import_alias_from_full_unique(codegen_t *cg, bool user_only,
                                                  const char *full_name) {
  if (!full_name || !*full_name)
    return;
  const char *last_dot = strrchr(full_name, '.');
  const char *alias = last_dot ? last_dot + 1 : full_name;
  ny_push_import_alias_unique(cg, user_only, alias, full_name);
}

static void ny_push_import_alias_from_full_unique_weak(codegen_t *cg,
                                                       bool user_only,
                                                       const char *full_name) {
  if (!full_name || !*full_name)
    return;
  const char *last_dot = strrchr(full_name, '.');
  const char *alias = last_dot ? last_dot + 1 : full_name;
  if (lookup_fun_exact(cg, alias) || lookup_global_exact(cg, alias))
    return;
  ny_push_import_alias_unique_ex(cg, user_only, alias, full_name, false);
}

void add_import_alias(codegen_t *cg, const char *alias, const char *full_name) {
  ny_push_import_alias_unique(cg, false, alias, full_name);
}

void add_import_alias_from_full(codegen_t *cg, const char *full_name) {
  ny_push_import_alias_from_full_unique(cg, false, full_name);
}

static void add_import_alias_from_full_weak(codegen_t *cg,
                                            const char *full_name) {
  ny_push_import_alias_from_full_unique_weak(cg, false, full_name);
}

static void add_user_import_alias(codegen_t *cg, const char *alias,
                                  const char *full_name) {
  ny_push_import_alias_unique(cg, true, alias, full_name);
}

static void add_user_import_alias_from_full(codegen_t *cg,
                                            const char *full_name) {
  ny_push_import_alias_from_full_unique(cg, true, full_name);
}

static void add_user_import_alias_from_full_weak(codegen_t *cg,
                                                 const char *full_name) {
  ny_push_import_alias_from_full_unique_weak(cg, true, full_name);
}

static bool ny_trace_imports_enabled(void) {
  return ny_trace_enabled("NYTRIX_TRACE_IMPORTS");
}

stmt_t *find_module_stmt(stmt_t *s, const char *name) {
  if (!s || !name)
    return NULL;
  if (s->kind == NY_S_MODULE && s->as.module.name &&
      strcmp(s->as.module.name, name) == 0) {
    return s;
  }
  return NULL;
}

static size_t ny_module_stmt_index_cap_for(size_t len) {
  size_t cap = 256;
  while (cap < len * 2u)
    cap <<= 1u;
  return cap;
}

static void ny_module_stmt_index_insert(module_stmt_slot *slots, size_t cap,
                                        stmt_t *s) {
  if (!slots || cap == 0 || !s || s->kind != NY_S_MODULE ||
      !s->as.module.name || !*s->as.module.name)
    return;
  const char *name = s->as.module.name;
  size_t name_len = strlen(name);
  uint64_t hash = ny_hash_name(name, name_len);
  size_t mask = cap - 1u;
  size_t pos = hash & mask;
  for (;;) {
    module_stmt_slot *slot = &slots[pos];
    if (!slot->occupied) {
      slot->occupied = true;
      slot->hash = hash;
      slot->name = name;
      slot->stmt = s;
      return;
    }
    if (slot->hash == hash && slot->name && strcmp(slot->name, name) == 0) {
      return;
    }
    pos = (pos + 1u) & mask;
  }
}

static size_t ny_count_top_modules(program_t *prog) {
  if (!prog)
    return 0;
  size_t n = 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_MODULE && s->as.module.name && *s->as.module.name)
      n++;
  }
  return n;
}

static void ny_module_stmt_index_add_program(module_stmt_slot *slots,
                                             size_t cap, program_t *prog) {
  if (!prog)
    return;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_module_stmt_index_insert(slots, cap, prog->body.data[i]);
}

static void ny_module_stmt_index_build(codegen_t *cg) {
  if (!cg)
    return;
  if (cg->module_stmt_index)
    return;
  size_t n = ny_count_top_modules(cg->prog);
  for (size_t p = 0; p < cg->extra_progs.len; ++p)
    n += ny_count_top_modules(cg->extra_progs.data[p]);
  if (cg->module_stmt_index && cg->module_stmt_index_len == n)
    return;
  free(cg->module_stmt_index);
  cg->module_stmt_index = NULL;
  cg->module_stmt_index_cap = 0;
  cg->module_stmt_index_len = 0;
  free(cg->module_stmt_lookup_cache);
  cg->module_stmt_lookup_cache = NULL;
  size_t cap = ny_module_stmt_index_cap_for(n + 1u);
  module_stmt_slot *slots = calloc(cap, sizeof(module_stmt_slot));
  if (!slots)
    return;
  ny_module_stmt_index_add_program(slots, cap, cg->prog);
  for (size_t p = 0; p < cg->extra_progs.len; ++p)
    ny_module_stmt_index_add_program(slots, cap, cg->extra_progs.data[p]);
  cg->module_stmt_index = slots;
  cg->module_stmt_index_cap = cap;
  cg->module_stmt_index_len = n;
}

void codegen_module_graph_changed(codegen_t *cg) {
  if (!cg)
    return;
  free(cg->module_stmt_index);
  cg->module_stmt_index = NULL;
  cg->module_stmt_index_cap = 0;
  cg->module_stmt_index_len = 0;
  free(cg->module_stmt_lookup_cache);
  cg->module_stmt_lookup_cache = NULL;
}

stmt_t *ny_module_lookup_stmt_by_name(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return NULL;
  size_t name_len = strlen(name);
  uint64_t hash = ny_hash_name(name, name_len);
  ny_module_stmt_index_build(cg);
  if (cg->module_stmt_index && cg->module_stmt_index_cap) {
    size_t mask = cg->module_stmt_index_cap - 1u;
    size_t pos = hash & mask;
    for (;;) {
      module_stmt_slot *slot = &cg->module_stmt_index[pos];
      if (!slot->occupied)
        return NULL;
      if (slot->hash == hash && slot->name &&
          memcmp(slot->name, name, name_len) == 0 &&
          slot->name[name_len] == '\0')
        return slot->stmt;
      pos = (pos + 1u) & mask;
    }
  }
  if (cg->prog) {
    for (size_t i = 0; i < cg->prog->body.len; ++i) {
      stmt_t *m = find_module_stmt(cg->prog->body.data[i], name);
      if (m)
        return m;
    }
  }
  for (size_t p = 0; p < cg->extra_progs.len; ++p) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; ++i) {
      stmt_t *m = find_module_stmt(prog->body.data[i], name);
      if (m)
        return m;
    }
  }
  return NULL;
}

bool ny_codegen_module_is_std(codegen_t *cg, const char *name, bool *known) {
  if (known)
    *known = false;
  if (!cg || !name || !*name)
    return false;
  stmt_t *module = ny_module_lookup_stmt_by_name((codegen_t *)cg, name);
  if (module && module->kind == NY_S_MODULE) {
    if (known)
      *known = true;
    return ny_is_stdlib_tok(module->tok);
  }
  int module_idx = ny_std_find_module_by_name(name);
  if (module_idx >= 0) {
    if (known)
      *known = true;
    return (ny_std_module_features((size_t)module_idx) &
            NY_MODULE_FEATURE_STDLIB) != 0;
  }
  return false;
}

bool ny_codegen_current_module_is_std(const codegen_t *cg, bool *known) {
  if (known)
    *known = false;
  if (!cg)
    return false;
  if (cg->current_fun_sig && cg->current_fun_sig->stmt_t &&
      cg->current_fun_sig->stmt_t->kind == NY_S_FUNC) {
    if (known)
      *known = true;
    return ny_is_stdlib_tok(cg->current_fun_sig->stmt_t->tok);
  }
  return ny_codegen_module_is_std((codegen_t *)cg, cg->current_module_name,
                                  known);
}

bool ny_codegen_current_module_matches_std(const codegen_t *cg,
                                           const char *registered_name) {
  if (!cg || !registered_name || !*registered_name)
    return false;
  int std_index = ny_std_find_module_by_name(registered_name);
  if (std_index < 0)
    return false;
  const ny_module_id_t expected_id = ny_std_module_id((size_t)std_index);
  if (expected_id == NY_MODULE_ID_UNKNOWN)
    return false;

  token_t module_tok = {0};
  if (cg->current_fun_sig && cg->current_fun_sig->stmt_t &&
      cg->current_fun_sig->stmt_t->kind == NY_S_FUNC) {
    module_tok = cg->current_fun_sig->stmt_t->tok;
  } else if (cg->current_module_name && *cg->current_module_name) {
    stmt_t *module =
        ny_module_lookup_stmt_by_name((codegen_t *)cg, cg->current_module_name);
    if (!module || module->kind != NY_S_MODULE)
      return false;
    module_tok = module->tok;
  } else {
    return false;
  }

  return module_tok.module_id == expected_id &&
         (module_tok.module_features & NY_MODULE_FEATURE_STDLIB) != 0;
}

bool ny_codegen_module_matches_std(const codegen_t *cg, const char *module_name,
                                   const char *registered_name) {
  if (!cg || !module_name || !*module_name || !registered_name ||
      !*registered_name)
    return false;
  int std_index = ny_std_find_module_by_name(registered_name);
  if (std_index < 0)
    return false;
  const ny_module_id_t expected_id = ny_std_module_id((size_t)std_index);
  if (expected_id == NY_MODULE_ID_UNKNOWN)
    return false;

  stmt_t *module = ny_module_lookup_stmt_by_name((codegen_t *)cg, module_name);
  if (module && module->kind == NY_S_MODULE)
    return module->tok.module_id == expected_id &&
           (module->tok.module_features & NY_MODULE_FEATURE_STDLIB) != 0;

  int module_index = ny_std_find_module_by_name(module_name);
  return module_index >= 0 &&
         ny_std_module_id((size_t)module_index) == expected_id &&
         (ny_std_module_features((size_t)module_index) &
          NY_MODULE_FEATURE_STDLIB) != 0;
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

static bool ny_export_profile_is_core(const char *profile) {
  return !profile || !*profile || strcmp(profile, "core") == 0;
}

static bool ny_export_profile_visible(const char *profile,
                                      const char *requested) {
  if (!requested || !*requested || strcmp(requested, "core") == 0)
    return ny_export_profile_is_core(profile);
  return ny_export_profile_is_core(profile) ||
         (profile && strcmp(profile, requested) == 0);
}

static bool ny_module_name_is_internal(stmt_t *mod, const char *name) {
  if (!mod || !name)
    return false;
  const char *leaf = strrchr(name, '.');
  leaf = leaf ? leaf + 1 : name;
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (!child || child->kind != NY_S_EXPORT || !child->as.exprt.is_internal)
      continue;
    for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
      const char *hidden = child->as.exprt.names.data[j];
      if (hidden && strcmp(hidden, leaf) == 0)
        return true;
    }
  }
  return false;
}

static bool ny_str_list_has(const str_list *items, const char *value) {
  if (!items || !value)
    return false;
  for (size_t i = 0; i < items->len; ++i) {
    if (items->data[i] && strcmp(items->data[i], value) == 0)
      return true;
  }
  return false;
}

typedef struct ny_str_dedup_slot_t {
  const char *str;
  uint64_t hash;
} ny_str_dedup_slot_t;

typedef struct ny_str_dedup_set_t {
  ny_str_dedup_slot_t *slots;
  size_t cap;
  size_t len;
  ny_str_dedup_slot_t stack_slots[64];
} ny_str_dedup_set_t;

static void ny_str_dedup_set_init(ny_str_dedup_set_t *set,
                                  const str_list *seed) {
  memset(set->stack_slots, 0, sizeof(set->stack_slots));
  set->slots = set->stack_slots;
  set->cap = 64u;
  set->len = 0;
  if (!seed || seed->len == 0)
    return;
  if (seed->len * 2u >= set->cap) {
    size_t cap = 64u;
    while (cap <= seed->len * 2u)
      cap <<= 1u;
    ny_str_dedup_slot_t *heap = calloc(cap, sizeof(*heap));
    if (heap) {
      set->slots = heap;
      set->cap = cap;
    }
  }
  for (size_t i = 0; i < seed->len; ++i) {
    const char *s = seed->data[i];
    if (!s)
      continue;
    uint64_t h = ny_hash_name(s, strlen(s));
    if (!h)
      h = 1u;
    size_t mask = set->cap - 1u;
    size_t pos = h & mask;
    while (set->slots[pos].str) {
      if (set->slots[pos].hash == h && strcmp(set->slots[pos].str, s) == 0)
        break;
      pos = (pos + 1u) & mask;
    }
    if (!set->slots[pos].str) {
      set->slots[pos].str = s;
      set->slots[pos].hash = h;
      set->len++;
    }
  }
}

static void ny_str_dedup_set_free(ny_str_dedup_set_t *set) {
  if (set && set->slots && set->slots != set->stack_slots)
    free(set->slots);
}

static bool ny_str_dedup_set_insert(ny_str_dedup_set_t *set, const char *s) {
  if (!set || !s)
    return false;
  if ((set->len + 1u) * 10u >= set->cap * 7u) {
    size_t new_cap = set->cap * 2u;
    ny_str_dedup_slot_t *heap = calloc(new_cap, sizeof(*heap));
    if (!heap)
      return true;
    size_t mask = new_cap - 1u;
    for (size_t i = 0; i < set->cap; ++i) {
      if (!set->slots[i].str)
        continue;
      size_t pos = set->slots[i].hash & mask;
      while (heap[pos].str)
        pos = (pos + 1u) & mask;
      heap[pos] = set->slots[i];
    }
    if (set->slots != set->stack_slots)
      free(set->slots);
    set->slots = heap;
    set->cap = new_cap;
  }
  uint64_t h = ny_hash_name(s, strlen(s));
  if (!h)
    h = 1u;
  size_t mask = set->cap - 1u;
  size_t pos = h & mask;
  while (set->slots[pos].str) {
    if (set->slots[pos].hash == h && strcmp(set->slots[pos].str, s) == 0)
      return false;
    pos = (pos + 1u) & mask;
  }
  set->slots[pos].str = s;
  set->slots[pos].hash = h;
  set->len++;
  return true;
}

static void ny_str_list_push_unique_owned_set(str_list *items, char *value,
                                              ny_str_dedup_set_t *seen) {
  if (!items || !value)
    return;
  if (seen) {
    if (!ny_str_dedup_set_insert(seen, value)) {
      free(value);
      return;
    }
    vec_push(items, value);
    return;
  }
  if (ny_str_list_has(items, value)) {
    free(value);
    return;
  }
  vec_push(items, value);
}

static void ny_str_list_push_unique_owned(str_list *items, char *value) {
  ny_str_list_push_unique_owned_set(items, value, NULL);
}

static void ny_str_list_free_owned(str_list *items) {
  if (!items)
    return;
  for (size_t i = 0; i < items->len; ++i)
    free(items->data[i]);
  vec_free(items);
}

void collect_module_exports(stmt_t *mod, str_list *exports,
                            const char *profile) {
  if (!mod || mod->kind != NY_S_MODULE)
    return;
  const char *mod_name = mod->as.module.name;
  ny_str_dedup_set_t seen;
  ny_str_dedup_set_init(&seen, exports);
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (child->kind != NY_S_EXPORT)
      continue;
    if (child->as.exprt.is_internal)
      continue;
    if (!ny_export_profile_visible(child->as.exprt.profile, profile))
      continue;
    for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
      const char *name = child->as.exprt.names.data[j];
      if (!name)
        continue;
      char *full = NULL;
      if (strchr(name, '.')) {
        full = ny_strdup(name);
      } else {
        size_t mod_len = strlen(mod_name);
        size_t name_len = strlen(name);
        size_t len = mod_len + 1u + name_len + 1u;
        full = malloc(len);
        if (!full)
          continue;
        memcpy(full, mod_name, mod_len);
        full[mod_len] = '.';
        memcpy(full + mod_len + 1u, name, name_len + 1u);
      }
      ny_str_list_push_unique_owned_set(exports, full, &seen);
    }
  }
  ny_str_dedup_set_free(&seen);
}

void collect_module_defs(stmt_t *mod, str_list *exports) {
  if (!mod || mod->kind != NY_S_MODULE)
    return;
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (child->kind == NY_S_FUNC) {
      if (!ny_module_name_is_internal(mod, child->as.fn.name))
        vec_push(exports, ny_strdup(child->as.fn.name));
    } else if (child->kind == NY_S_VAR) {
      for (size_t j = 0; j < child->as.var.names.len; ++j)
        if (!ny_module_name_is_internal(mod, child->as.var.names.data[j]))
          vec_push(exports, ny_strdup(child->as.var.names.data[j]));
    }
  }
}

bool ny_module_declares_exported_member(codegen_t *cg, const char *module_name,
                                        const char *member) {
  if (!cg || !module_name || !*module_name || !member || !*member)
    return false;
  stmt_t *mod = ny_module_lookup_stmt_by_name(cg, module_name);
  if (!mod)
    return false;
  if (mod->as.module.export_all) {
    for (size_t i = 0; i < mod->as.module.body.len; ++i) {
      stmt_t *child = mod->as.module.body.data[i];
      if (child->kind == NY_S_FUNC) {
        const char *name = child->as.fn.name;
        if (name && !ny_module_name_is_internal(mod, name)) {
          const char *leaf = strrchr(name, '.');
          if (strcmp(leaf ? leaf + 1 : name, member) == 0)
            return true;
        }
      } else if (child->kind == NY_S_VAR) {
        for (size_t j = 0; j < child->as.var.names.len; ++j) {
          const char *name = child->as.var.names.data[j];
          if (name && !ny_module_name_is_internal(mod, name)) {
            const char *leaf = strrchr(name, '.');
            if (strcmp(leaf ? leaf + 1 : name, member) == 0)
              return true;
          }
        }
      }
    }
    return false;
  }

  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (child->kind != NY_S_EXPORT || child->as.exprt.is_internal ||
        !ny_export_profile_visible(child->as.exprt.profile, NULL))
      continue;
    for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
      const char *name = child->as.exprt.names.data[j];
      if (!name)
        continue;
      const char *leaf = strrchr(name, '.');
      if (strcmp(leaf ? leaf + 1 : name, member) == 0)
        return true;
    }
  }
  return false;
}

void add_imports_from_prefix(codegen_t *cg, bool user_use, const char *mod) {
  if (!mod || !*mod)
    return;
  size_t mod_len = strlen(mod);
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    const char *name = cg->fun_sigs.data[i].name;
    if (strncmp(name, mod, mod_len) == 0 && name[mod_len] == '.') {
      add_import_alias_from_full_weak(cg, name);
      if (user_use)
        add_user_import_alias_from_full_weak(cg, name);
    }
  }
  for (size_t i = 0; i < cg->global_vars.len; ++i) {
    const char *name = cg->global_vars.data[i].name;
    if (strncmp(name, mod, mod_len) == 0 && name[mod_len] == '.') {
      add_import_alias_from_full_weak(cg, name);
      if (user_use)
        add_user_import_alias_from_full_weak(cg, name);
    }
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

static bool ny_module_user_ctx_is_non_std(const codegen_t *cg) {
  bool known = false;
  bool is_std = ny_codegen_current_module_is_std(cg, &known);
  return known ? !is_std : true;
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

static bool ny_path_suffix_match(const char *filename, const char *raw) {
  if (!filename || !*filename || !raw || !*raw)
    return false;
  const char *want = raw;
  while (want[0] == '.' && want[1] == '/')
    want += 2;
  if (!*want)
    return false;
  size_t flen = strlen(filename);
  size_t wlen = strlen(want);
  if (flen < wlen)
    return false;
  const char *tail = filename + flen - wlen;
  if (strcmp(tail, want) == 0)
    return true;
  return strcmp(filename, raw) == 0;
}

static bool ny_path_is_absolute(const char *path) {
  if (!path || !*path)
    return false;
  if (path[0] == '/' || path[0] == '\\')
    return true;
  return isalpha((unsigned char)path[0]) && path[1] == ':' &&
         (path[2] == '/' || path[2] == '\\');
}

static bool ny_dirname_copy(char *out, size_t out_cap, const char *path) {
  if (!out || out_cap == 0)
    return false;
  if (!path || !*path || path[0] == '<') {
    snprintf(out, out_cap, ".");
    return true;
  }
  const char *a = strrchr(path, '/');
  const char *b = strrchr(path, '\\');
  const char *slash = (!a || (b && b > a)) ? b : a;
  if (!slash) {
    snprintf(out, out_cap, ".");
    return true;
  }
  size_t len = (size_t)(slash - path);
  if (len == 0)
    len = 1;
  if (len >= out_cap)
    len = out_cap - 1;
  memcpy(out, path, len);
  out[len] = '\0';
  return true;
}

static bool ny_paths_equal_best_effort(const char *a, const char *b) {
  if (!a || !*a || !b || !*b)
    return false;
  char ra[4096];
  char rb[4096];
  if (ny_realpath(a, ra) && ny_realpath(b, rb))
    return strcmp(ra, rb) == 0;
  return strcmp(a, b) == 0;
}

static bool ny_local_use_path_match(const char *module_file,
                                    const char *use_file, const char *raw) {
  if (!module_file || !*module_file || !raw || !*raw)
    return false;
  if (!use_file || !*use_file || use_file[0] == '<')
    return ny_path_suffix_match(module_file, raw);

  char candidate[4096];
  if (ny_path_is_absolute(raw)) {
    snprintf(candidate, sizeof(candidate), "%s", raw);
  } else {
    char dir[4096];
    if (!ny_dirname_copy(dir, sizeof(dir), use_file))
      return false;
    ny_join_path(candidate, sizeof(candidate), dir, raw);
  }
  if (ny_paths_equal_best_effort(candidate, module_file))
    return true;
  return false;
}

static bool ny_resolve_local_use_file(char *out, size_t out_cap,
                                      const char *use_file, const char *raw) {
  if (!out || out_cap == 0 || !raw || !*raw || !strchr(raw, '/'))
    return false;
  if (ny_path_is_absolute(raw)) {
    int nw = snprintf(out, out_cap, "%s", raw);
    return nw > 0 && (size_t)nw < out_cap;
  }
  char dir[4096];
  if (!ny_dirname_copy(dir, sizeof(dir), use_file))
    return false;
  ny_join_path(out, out_cap, dir, raw);
  return out[0] != '\0';
}

static stmt_t *
ny_find_local_use_declared_module_in_program(program_t *prog, const char *raw,
                                             const char *use_file) {
  if (!prog || !raw || !*raw)
    return NULL;
  for (size_t i = 0; i < prog->body.len; ++i) {
    stmt_t *s = prog->body.data[i];
    if (!s || s->kind != NY_S_MODULE)
      continue;
    const char *module_file =
        s->tok.filename ? s->tok.filename : s->as.module.path;
    if (ny_local_use_path_match(module_file, use_file, raw))
      return s;
  }
  return NULL;
}

static stmt_t *ny_find_local_use_declared_module(codegen_t *cg, const char *raw,
                                                 const char *use_file) {
  if (!cg || !raw || !*raw || !strchr(raw, '/'))
    return NULL;
  stmt_t *m =
      ny_find_local_use_declared_module_in_program(cg->prog, raw, use_file);
  if (m)
    return m;
  for (size_t p = 0; p < cg->extra_progs.len; ++p) {
    m = ny_find_local_use_declared_module_in_program(cg->extra_progs.data[p],
                                                     raw, use_file);
    if (m)
      return m;
  }
  return NULL;
}

static char *normalize_use_module_name_at(codegen_t *cg, const char *raw,
                                          const char *use_file) {
  if (raw && strchr(raw, '/')) {
    stmt_t *decl = ny_find_local_use_declared_module(cg, raw, use_file);
    if (decl && decl->as.module.name && *decl->as.module.name)
      return ny_strdup(decl->as.module.name);
    char resolved_file[4096];
    if (ny_resolve_local_use_file(resolved_file, sizeof(resolved_file),
                                  use_file, raw)) {
      char *declared_name = ny_read_declared_module_name(resolved_file);
      if (declared_name && *declared_name)
        return declared_name;
      free(declared_name);
    }
  }
  return normalize_module_name(raw);
}

static void ny_add_import_aliases(codegen_t *cg, bool user_use,
                                  const char *alias, const char *full_name);
static void ny_add_import_aliases_from_full(codegen_t *cg, bool user_use,
                                            const char *full_name);
static void ny_add_import_aliases_from_full_weak(codegen_t *cg, bool user_use,
                                                 const char *full_name);
static void ny_add_import_aliases_from_full_scoped_weak(codegen_t *cg,
                                                        bool user_use,
                                                        const char *full_name);
static bool ny_module_public_target(codegen_t *cg, stmt_t *mod,
                                    const char *target, char *out,
                                    size_t out_cap, const char *profile,
                                    bool include_child_uses, int depth);
static bool ny_module_local_import_target(codegen_t *cg, stmt_t *mod,
                                          const char *target, char *out,
                                          size_t out_cap, int depth);
static bool ny_module_local_import_target_memo(codegen_t *cg, stmt_t *mod,
                                               const char *name, char *out,
                                               size_t out_cap);
static const char *ny_resolve_export_source(codegen_t *cg, const char *source,
                                            char *out, size_t out_cap,
                                            int depth);

static void ny_collect_module_use_surface(codegen_t *cg, stmt_t *s,
                                          str_list *exports,
                                          const char *profile, int depth) {
  if (!cg || !s || !exports || depth > 8)
    return;
  if (s->kind == NY_S_USE) {
    if (ny_stmt_is_bare_core_use(s))
      return;
    bool bare_use =
        !s->as.use.import_all && s->as.use.imports.len == 0 && !s->as.use.alias;
    if (!s->as.use.import_all && !bare_use)
      return;
    char *mod =
        normalize_use_module_name_at(cg, s->as.use.module, s->tok.filename);
    if (!mod)
      return;
    stmt_t *used = ny_module_lookup_stmt_by_name(cg, mod);
    if (used) {
      bool has_exports = module_has_export_list(used);
      const char *use_profile = s->as.use.profile ? s->as.use.profile : profile;
      if (has_exports)
        collect_module_exports(used, exports, use_profile);
      if (!has_exports || used->as.module.export_all)
        collect_module_defs(used, exports);
    }
    free(mod);
    return;
  }
  if (s->kind == NY_S_IF) {
    bool truthy = false;
    if (ny_eval_comptime_if(cg, s, &truthy)) {
      if (truthy)
        ny_collect_module_use_surface(cg, s->as.iff.conseq, exports, profile,
                                      depth + 1);
      else if (s->as.iff.alt)
        ny_collect_module_use_surface(cg, s->as.iff.alt, exports, profile,
                                      depth + 1);
      return;
    }
    ny_collect_module_use_surface(cg, s->as.iff.conseq, exports, profile,
                                  depth + 1);
    if (s->as.iff.alt)
      ny_collect_module_use_surface(cg, s->as.iff.alt, exports, profile,
                                    depth + 1);
    return;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_collect_module_use_surface(cg, s->as.block.body.data[i], exports,
                                    profile, depth + 1);
  }
}

static void ny_collect_module_public_surface_inner(codegen_t *cg, stmt_t *mod,
                                                   str_list *exports,
                                                   const char *profile,
                                                   bool include_child_uses,
                                                   int depth);

static bool ny_module_has_explicit_child_use(codegen_t *cg, stmt_t *mod,
                                             const char *child_path,
                                             const char *leaf) {
  if (!mod || mod->kind != NY_S_MODULE || !child_path || !*child_path)
    return false;
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *s = mod->as.module.body.data[i];
    if (!s || s->kind != NY_S_USE || ny_stmt_is_bare_core_use(s))
      continue;
    if (s->as.use.alias || s->as.use.import_all || s->as.use.imports.len > 0)
      continue;
    const char *raw = s->as.use.module;
    if (!raw || !*raw)
      continue;
    char *owned = NULL;
    const char *used = raw;
    if (strchr(raw, '/')) {
      owned = normalize_use_module_name_at(cg, raw, s->tok.filename);
      if (!owned)
        continue;
      used = owned;
    }
    const char *used_leaf = ny_name_leaf(used);
    bool hit = strcmp(used, child_path) == 0 ||
               (leaf && used_leaf && strcmp(used_leaf, leaf) == 0);
    free(owned);
    if (hit)
      return true;
  }
  return false;
}

static bool ny_module_has_any_explicit_child_use(stmt_t *mod) {
  if (!mod || mod->kind != NY_S_MODULE)
    return false;
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *s = mod->as.module.body.data[i];
    if (!s || s->kind != NY_S_USE || ny_stmt_is_bare_core_use(s))
      continue;
    if (!s->as.use.alias && !s->as.use.import_all && s->as.use.imports.len == 0)
      return true;
  }
  return false;
}

static bool ny_module_needs_exported_child_bindings(stmt_t *mod) {
  if (!mod || mod->kind != NY_S_MODULE)
    return false;
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *s = mod->as.module.body.data[i];
    if (!s)
      continue;
    if (s->kind == NY_S_EXPORT || s->kind == NY_S_USE || s->kind == NY_S_MODULE)
      continue;
    return true;
  }
  return false;
}

static void ny_add_module_alias_binding_unique(codegen_t *cg, const char *alias,
                                               const char *target) {
  if (!cg || !alias || !*alias || !target || !*target)
    return;
  for (size_t i = 0; i < cg->aliases.len; ++i) {
    binding *al = &cg->aliases.data[i];
    if (!al->name || !al->stmt_t)
      continue;
    if (strcmp(al->name, alias) == 0 &&
        strcmp((const char *)al->stmt_t, target) == 0)
      return;
  }
  binding alias_bind = {0};
  alias_bind.name = ny_strdup(alias);
  alias_bind.stmt_t = (stmt_t *)ny_strdup(target);
  alias_bind.owned = true;
  vec_push(&cg->aliases, alias_bind);
}

/*
 * Joins `base` and `leaf` into `buf` as "base.leaf". Returns true when the
 * result fits in `buf_size`, false otherwise (buf is left unspecified).
 */
static bool ny_join_scoped_name(char *buf, size_t buf_size, const char *base,
                                const char *leaf) {
  int nw = snprintf(buf, buf_size, "%s.%s", base, leaf);
  return nw > 0 && (size_t)nw < buf_size;
}

/*
 * Same join, but leaves `name` as-is (copied into `buf`) when it already
 * contains a '.' and is therefore already fully qualified.
 */
static bool ny_join_scoped_name_unless_dotted(char *buf, size_t buf_size,
                                              const char *base,
                                              const char *name) {
  int nw = strchr(name, '.') ? snprintf(buf, buf_size, "%s", name)
                             : snprintf(buf, buf_size, "%s.%s", base, name);
  return nw > 0 && (size_t)nw < buf_size;
}

static void ny_add_scoped_module_alias(codegen_t *cg, const char *alias,
                                       const char *target) {
  if (!cg || !alias || !*alias || !target || !*target)
    return;
  if (cg->current_module_name && *cg->current_module_name) {
    char scoped[512];
    if (ny_join_scoped_name(scoped, sizeof(scoped), cg->current_module_name,
                            alias)) {
      ny_add_module_alias_binding_unique(cg, scoped, target);
      return;
    }
  }
  ny_add_module_alias_binding_unique(cg, alias, target);
}

static bool ny_module_alias_name_exists(codegen_t *cg, const char *alias) {
  if (!cg || !alias || !*alias)
    return false;
  for (size_t i = cg->aliases.len; i > 0; --i) {
    binding *al = &cg->aliases.data[i - 1];
    if (al->name && strcmp(al->name, alias) == 0)
      return true;
  }
  return false;
}

static void ny_add_scoped_module_alias_weak(codegen_t *cg, const char *alias,
                                            const char *target) {
  if (!cg || !alias || !*alias || !target || !*target)
    return;
  if (cg->current_module_name && *cg->current_module_name) {
    char scoped[512];
    if (ny_join_scoped_name(scoped, sizeof(scoped), cg->current_module_name,
                            alias)) {
      if (!ny_module_alias_name_exists(cg, scoped))
        ny_add_module_alias_binding_unique(cg, scoped, target);
      return;
    }
  }
  if (!ny_module_alias_name_exists(cg, alias))
    ny_add_module_alias_binding_unique(cg, alias, target);
}

static void ny_add_module_alias_from_full_if_module(codegen_t *cg,
                                                    const char *full_name) {
  if (!cg || !full_name || !*full_name)
    return;
  if (!ny_module_lookup_stmt_by_name(cg, full_name))
    return;
  const char *leaf = ny_name_leaf(full_name);
  if (!leaf || !*leaf)
    return;
  ny_add_scoped_module_alias_weak(cg, leaf, full_name);
}

static void ny_collect_exported_child_module_aliases(codegen_t *cg, stmt_t *mod,
                                                     const char *profile,
                                                     int depth) {
  if (!cg || !mod || mod->kind != NY_S_MODULE || depth > 8)
    return;
  if (!ny_module_needs_exported_child_bindings(mod))
    return;
  const char *mod_name = mod->as.module.name;
  if (!mod_name || !*mod_name)
    return;
  bool has_explicit_child_uses = ny_module_has_any_explicit_child_use(mod);
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (!child || child->kind != NY_S_EXPORT || child->as.exprt.is_internal)
      continue;
    if (!ny_export_profile_visible(child->as.exprt.profile, profile))
      continue;
    for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
      const char *name = child->as.exprt.names.data[j];
      if (!name || !*name)
        continue;
      char child_path[1024];
      if (!ny_join_scoped_name_unless_dotted(child_path, sizeof(child_path),
                                             mod_name, name))
        continue;
      if (has_explicit_child_uses &&
          ny_module_has_explicit_child_use(cg, mod, child_path, name))
        continue;
      if (!ny_module_lookup_stmt_by_name(cg, child_path))
        continue;
      const char *leaf = ny_name_leaf(child_path);
      if (leaf && *leaf)
        ny_add_scoped_module_alias(cg, leaf, child_path);
    }
  }
}

static void ny_collect_exported_child_module_surface(codegen_t *cg, stmt_t *mod,
                                                     str_list *exports,
                                                     const char *profile,
                                                     int depth) {
  if (!cg || !mod || mod->kind != NY_S_MODULE || !exports || depth > 8)
    return;
  const char *mod_name = mod->as.module.name;
  if (!mod_name || !*mod_name)
    return;
  bool has_explicit_child_uses = ny_module_has_any_explicit_child_use(mod);
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (!child || child->kind != NY_S_EXPORT || child->as.exprt.is_internal)
      continue;
    if (!ny_export_profile_visible(child->as.exprt.profile, profile))
      continue;
    for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
      const char *name = child->as.exprt.names.data[j];
      if (!name || !*name)
        continue;
      char path[1024];
      if (!ny_join_scoped_name_unless_dotted(path, sizeof(path), mod_name,
                                             name))
        continue;
      if (has_explicit_child_uses &&
          ny_module_has_explicit_child_use(cg, mod, path, name))
        continue;
      stmt_t *child_mod = ny_module_lookup_stmt_by_name(cg, path);
      if (!child_mod)
        continue;
      ny_str_list_push_unique_owned(exports, ny_strdup(path));

      ny_collect_module_public_surface_inner(cg, child_mod, exports, profile,
                                             false, depth + 1);
    }
  }
}

static void ny_collect_module_public_surface_inner(codegen_t *cg, stmt_t *mod,
                                                   str_list *exports,
                                                   const char *profile,
                                                   bool include_child_uses,
                                                   int depth) {
  if (!mod || mod->kind != NY_S_MODULE || !exports)
    return;
  if (depth > 8)
    return;
  bool has_export_list = module_has_export_list(mod);
  if (has_export_list)
    collect_module_exports(mod, exports, profile);
  if (!has_export_list || mod->as.module.export_all)
    collect_module_defs(mod, exports);
  if (!include_child_uses)
    return;
  if (has_export_list)
    ny_collect_exported_child_module_surface(cg, mod, exports, profile, depth);
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (!child || child->kind == NY_S_MODULE || child->kind == NY_S_FUNC)
      continue;
    ny_collect_module_use_surface(cg, child, exports, profile, 0);
  }
}

static void ny_collect_module_public_surface(codegen_t *cg, stmt_t *mod,
                                             str_list *exports,
                                             const char *profile,
                                             bool include_child_uses) {
  ny_collect_module_public_surface_inner(cg, mod, exports, profile,
                                         include_child_uses, 0);
}

typedef struct {
  const char *alias;
  const char *module;
} ny_std_root_alias_t;

static const ny_std_root_alias_t k_core_root_aliases[] = {
    {"core", "core"},    {"math", "core.math"},  {"os", "core.os"},
    {"str", "core.str"}, {"nt", "core.math.nt"}, {"bin", "core.math.bin"},
};

static const char *const k_core_bare_surface_modules[] = {
    "core",         "core.os",       "core.os.prim", "core.math",
    "core.math.nt", "core.math.bin", "core.str",
};

static const char *const k_core_root_use_modules[] = {
    "core",          "core.os.prim", "core.math", "core.math.nt",
    "core.math.bin", "core.os",      "core.str",
};

static bool ny_bare_use_keeps_shallow_surface(const char *mod) {
  if (!mod)
    return false;
  return strcmp(mod, "core") == 0 || strcmp(mod, "core.math.crypto") == 0;
}

static void ny_process_bare_core_use_imports(codegen_t *cg, bool user_use) {
  for (size_t i = 0;
       i < sizeof(k_core_root_aliases) / sizeof(k_core_root_aliases[0]); ++i)
    ny_add_import_aliases(cg, user_use, k_core_root_aliases[i].alias,
                          k_core_root_aliases[i].module);

  for (size_t mi = 0; mi < sizeof(k_core_bare_surface_modules) /
                               sizeof(k_core_bare_surface_modules[0]);
       ++mi) {
    const char *mod = k_core_bare_surface_modules[mi];
    str_list exports = {0};
    stmt_t *mod_stmt = ny_module_lookup_stmt_by_name(cg, mod);
    if (mod_stmt)
      ny_collect_module_public_surface(cg, mod_stmt, &exports, NULL, false);
    if (!mod_stmt || exports.len == 0) {
      add_imports_from_prefix(cg, user_use, mod);
    } else {
      for (size_t i = 0; i < exports.len; ++i) {
        ny_add_import_aliases_from_full_scoped_weak(cg, user_use,
                                                    exports.data[i]);
      }
    }
    ny_str_list_free_owned(&exports);
  }
}

static void ny_process_exported_child_module_imports(codegen_t *cg,
                                                     stmt_t *mod) {
  if (!cg || !mod || mod->kind != NY_S_MODULE || !module_has_export_list(mod))
    return;
  if (!ny_module_needs_exported_child_bindings(mod))
    return;
  str_list exports = {0};
  bool user_use = !ny_is_stdlib_tok(mod->tok);
  ny_collect_exported_child_module_surface(cg, mod, &exports, NULL, 0);
  for (size_t i = 0; i < exports.len; ++i) {
    if (ny_trace_imports_enabled())
      ny_module_trace_event("IMPORTS", "module-export alias %s -> %s\n",
                            mod->as.module.name ? mod->as.module.name
                                                : "(module)",
                            exports.data[i] ? exports.data[i] : "(nil)");
    ny_add_import_aliases_from_full_scoped_weak(cg, user_use, exports.data[i]);
  }
  ny_str_list_free_owned(&exports);
}

static void ny_add_import_aliases(codegen_t *cg, bool user_use,
                                  const char *alias, const char *full_name) {
  add_import_alias(cg, alias, full_name);
  if (user_use)
    add_user_import_alias(cg, alias, full_name);
}

static void ny_add_import_aliases_scoped(codegen_t *cg, bool user_use,
                                         const char *alias,
                                         const char *full_name) {
  if (cg && cg->current_module_name && *cg->current_module_name) {
    char scoped[512];
    if (ny_join_scoped_name(scoped, sizeof(scoped), cg->current_module_name,
                            alias)) {
      add_import_alias(cg, scoped, full_name);
      if (user_use)
        add_user_import_alias(cg, scoped, full_name);
    }
    return;
  }
  ny_add_import_aliases(cg, user_use, alias, full_name);
}

static void ny_add_import_aliases_from_full(codegen_t *cg, bool user_use,
                                            const char *full_name) {
  add_import_alias_from_full(cg, full_name);
  if (user_use)
    add_user_import_alias_from_full(cg, full_name);
}

static void ny_add_import_aliases_from_full_weak(codegen_t *cg, bool user_use,
                                                 const char *full_name) {
  add_import_alias_from_full_weak(cg, full_name);
  if (user_use)
    add_user_import_alias_from_full_weak(cg, full_name);
}

static void ny_add_import_aliases_from_full_scoped_weak(codegen_t *cg,
                                                        bool user_use,
                                                        const char *full_name) {
  if (cg && cg->current_module_name && *cg->current_module_name) {
    const char *leaf = ny_name_leaf(full_name);
    if (leaf && *leaf) {
      char scoped[512];
      if (ny_join_scoped_name(scoped, sizeof(scoped), cg->current_module_name,
                              leaf))
        if (!lookup_fun_exact(cg, scoped) && !lookup_global_exact(cg, scoped)) {
          ny_push_import_alias_unique_ex(cg, false, scoped, full_name, false);
          if (user_use)
            ny_push_import_alias_unique_ex(cg, true, scoped, full_name, false);
        }
    }
    return;
  }
  ny_add_import_aliases_from_full_weak(cg, user_use, full_name);
}

void process_default_core_imports(codegen_t *cg) {
  if (!cg)
    return;
  char *mod = normalize_module_name("core");
  if (!mod)
    return;

  str_list exports = {0};
  stmt_t *mod_stmt = ny_module_lookup_stmt_by_name(cg, mod);
  if (mod_stmt) {
    bool has_export_list = module_has_export_list(mod_stmt);
    if (has_export_list)
      collect_module_exports(mod_stmt, &exports, NULL);
    if (!has_export_list || mod_stmt->as.module.export_all)
      collect_module_defs(mod_stmt, &exports);
  }

  if (!mod_stmt || exports.len == 0) {
    add_imports_from_prefix(cg, true, mod);
  } else {
    for (size_t i = 0; i < exports.len; ++i) {
      ny_add_import_aliases_from_full(cg, true, exports.data[i]);
    }
  }
  ny_str_list_free_owned(&exports);
  free(mod);
}

void process_use_imports(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_USE) {
    if (ny_stmt_is_bare_core_use(s)) {
      ny_process_bare_core_use_imports(cg, !ny_is_stdlib_tok(s->tok));
      return;
    }
    bool bare_use =
        !s->as.use.import_all && s->as.use.imports.len == 0 && !s->as.use.alias;
    bool alias_only =
        !s->as.use.import_all && s->as.use.imports.len == 0 && s->as.use.alias;
    bool user_use = !ny_is_stdlib_tok(s->tok);
    char *mod =
        normalize_use_module_name_at(cg, s->as.use.module, s->tok.filename);
    if (alias_only) {
      str_list exports = {0};
      stmt_t *mod_stmt = ny_module_lookup_stmt_by_name(cg, mod);
      if (mod_stmt)
        ny_collect_module_public_surface(cg, mod_stmt, &exports,
                                         s->as.use.profile, false);
      if (!mod_stmt || exports.len == 0) {
        add_imports_from_prefix(cg, user_use, mod);
      } else {
        for (size_t i = 0; i < exports.len; ++i) {
          ny_add_import_aliases_from_full_scoped_weak(cg, user_use,
                                                      exports.data[i]);
        }
      }
      ny_str_list_free_owned(&exports);
      free(mod);
      return;
    }
    if (!s->as.use.import_all && s->as.use.imports.len == 0 && !bare_use) {
      free(mod);
      return;
    }
    if (s->as.use.imports.len > 0) {
      size_t mod_len = strlen(mod);
      size_t max_item_len = 0;
      for (size_t i = 0; i < s->as.use.imports.len; ++i) {
        use_item_t *item = &s->as.use.imports.data[i];
        if (!item->name)
          continue;
        size_t item_len = strlen(item->name);
        if (item_len > max_item_len)
          max_item_len = item_len;
      }
      size_t full_cap = mod_len + 1 + max_item_len + 1;
      char stack_buf[256];
      char *heap_buf = full_cap <= sizeof(stack_buf) ? NULL : malloc(full_cap);
      char *full = heap_buf ? heap_buf : stack_buf;
      stmt_t *used_mod = ny_module_lookup_stmt_by_name(cg, mod);
      for (size_t i = 0; i < s->as.use.imports.len; ++i) {
        use_item_t *item = &s->as.use.imports.data[i];
        if (!item->name)
          continue;
        snprintf(full, full_cap, "%s.%s", mod, item->name);
        const char *alias = item->alias ? item->alias : item->name;
        const char *import_target = full;
        char local_target[1024];
        char resolved_target[1024];
        if (used_mod && ny_module_local_import_target_memo(
                            cg, used_mod, item->name, local_target,
                            sizeof(local_target))) {
          import_target = ny_resolve_export_source(
              cg, local_target, resolved_target, sizeof(resolved_target), 0);
        }
        ny_add_import_aliases_scoped(cg, user_use, alias, import_target);
      }
      free(heap_buf);
      free(mod);
      return;
    }
    if (s->as.use.import_all || bare_use) {
      str_list exports = {0};
      stmt_t *mod_stmt = ny_module_lookup_stmt_by_name(cg, mod);
      if (bare_use && user_use && !s->as.use.profile &&
          ny_bare_use_keeps_shallow_surface(mod) && mod_stmt &&
          module_has_export_list(mod_stmt)) {
        collect_module_exports(mod_stmt, &exports, s->as.use.profile);
        for (size_t i = 0; i < exports.len; ++i)
          ny_add_module_alias_from_full_if_module(cg, exports.data[i]);
        ny_str_list_free_owned(&exports);
        free(mod);
        return;
      }
      if (mod_stmt)
        ny_collect_module_public_surface(cg, mod_stmt, &exports,
                                         s->as.use.profile, true);
      if (ny_trace_imports_enabled()) {
        ny_module_trace_event(
            "IMPORTS", "use %s%s%s%s mod_stmt=%s export_list=%d exports=%zu\n",
            mod, s->as.use.profile ? ":" : "",
            s->as.use.profile ? s->as.use.profile : "",
            s->as.use.import_all ? " *" : "", mod_stmt ? "yes" : "no",
            mod_stmt && module_has_export_list(mod_stmt) ? 1 : 0, exports.len);
      }
      if (!mod_stmt || exports.len == 0) {
        if (ny_trace_imports_enabled())
          ny_module_trace_event("IMPORTS", "fallback prefix %s\n", mod);
        add_imports_from_prefix(cg, user_use, mod);
      } else {
        const char *module_leaf = ny_name_leaf(mod);
        for (size_t i = 0; i < exports.len; ++i) {
          if (ny_trace_imports_enabled())
            ny_module_trace_event("IMPORTS", "alias %s\n", exports.data[i]);
          char resolved_export[1024];
          const char *export_target = ny_resolve_export_source(
              cg, exports.data[i], resolved_export, sizeof(resolved_export), 0);
          const char *export_leaf = ny_name_leaf(exports.data[i]);
          if (!module_leaf || !export_leaf ||
              strcmp(module_leaf, export_leaf) != 0)
            ny_add_module_alias_from_full_if_module(cg, exports.data[i]);
          ny_add_import_aliases_from_full_scoped_weak(cg, user_use,
                                                      export_target);
        }
      }
      ny_str_list_free_owned(&exports);
      free(mod);
      return;
    }
    free(mod);
  } else if (s->kind == NY_S_MODULE) {
    const char *prev_mod = cg->current_module_name;
    cg->current_module_name = s->as.module.name;
    ny_process_exported_child_module_imports(cg, s);
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      process_use_imports(cg, s->as.module.body.data[i]);
    cg->current_module_name = prev_mod;
  } else if (s->kind == NY_S_FUNC) {
    if (s->as.fn.body)
      process_use_imports(cg, s->as.fn.body);
  } else if (s->kind == NY_S_IF) {
    bool truthy = false;
    if (ny_eval_comptime_if(cg, s, &truthy)) {
      if (truthy) {
        process_use_imports(cg, s->as.iff.conseq);
      } else if (s->as.iff.alt) {
        process_use_imports(cg, s->as.iff.alt);
      }
    } else {
      process_use_imports(cg, s->as.iff.conseq);
      if (s->as.iff.alt)
        process_use_imports(cg, s->as.iff.alt);
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      process_use_imports(cg, s->as.block.body.data[i]);
  }
}

void collect_use_aliases(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_USE) {
    if (ny_stmt_is_bare_core_use(s)) {
      for (size_t i = 0;
           i < sizeof(k_core_root_aliases) / sizeof(k_core_root_aliases[0]);
           ++i) {
        const char *alias = k_core_root_aliases[i].alias;
        const char *target = k_core_root_aliases[i].module;
        char scoped_alias[512];
        if (cg && cg->current_module_name && *cg->current_module_name &&
            ny_join_scoped_name(scoped_alias, sizeof(scoped_alias),
                                cg->current_module_name, alias))
          alias = scoped_alias;
        binding alias_bind = {0};
        alias_bind.name = ny_strdup(alias);
        alias_bind.stmt_t = (stmt_t *)ny_strdup(target);
        alias_bind.owned = true;
        vec_push(&cg->aliases, alias_bind);
      }
      return;
    }
    if (s->as.use.import_all || s->as.use.imports.len > 0)
      return;
    const char *alias = s->as.use.alias;
    char *mod =
        normalize_use_module_name_at(cg, s->as.use.module, s->tok.filename);
    if (!alias) {
      const char *dot = strrchr(mod, '.');
      alias = dot ? dot + 1 : mod;
    }
    char scoped_alias[512];
    if (cg && cg->current_module_name && *cg->current_module_name &&
        ny_join_scoped_name(scoped_alias, sizeof(scoped_alias),
                            cg->current_module_name, alias))
      alias = scoped_alias;
    binding alias_bind = {0};
    alias_bind.name = ny_strdup(alias);
    alias_bind.stmt_t = (stmt_t *)ny_strdup(mod);
    alias_bind.owned = true;
    vec_push(&cg->aliases, alias_bind);
    free(mod);
  } else if (s->kind == NY_S_MODULE) {
    const char *prev_mod = cg->current_module_name;
    cg->current_module_name = s->as.module.name;
    ny_collect_exported_child_module_aliases(cg, s, NULL, 0);
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      collect_use_aliases(cg, s->as.module.body.data[i]);
    cg->current_module_name = prev_mod;
  } else if (s->kind == NY_S_FUNC) {
    if (s->as.fn.body)
      collect_use_aliases(cg, s->as.fn.body);
  } else if (s->kind == NY_S_IF) {
    bool truthy = false;
    if (ny_eval_comptime_if(cg, s, &truthy)) {
      if (truthy) {
        collect_use_aliases(cg, s->as.iff.conseq);
      } else if (s->as.iff.alt) {
        collect_use_aliases(cg, s->as.iff.alt);
      }
    } else {
      collect_use_aliases(cg, s->as.iff.conseq);
      if (s->as.iff.alt)
        collect_use_aliases(cg, s->as.iff.alt);
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      collect_use_aliases(cg, s->as.block.body.data[i]);
  }
}

const char *ny_lookup_module_alias(codegen_t *cg, scope *scopes, size_t depth,
                                   const char *name, size_t name_len,
                                   uint64_t name_hash) {
  if (!cg || !name || !*name)
    return NULL;
  if (name_len == 0)
    name_len = strlen(name);
  if (!name_hash)
    name_hash = ny_hash_name(name, name_len);
  if (scope_lookup_hash(scopes, depth, name, name_len, name_hash))
    return NULL;

  char scoped[1024];
  if (cg->current_module_name && *cg->current_module_name) {
    int nw = snprintf(scoped, sizeof(scoped), "%s.%.*s",
                      cg->current_module_name, (int)name_len, name);
    if (nw > 0 && (size_t)nw < sizeof(scoped)) {
      const char *scoped_target = ny_lookup_module_alias_indexed(
          cg, scoped, (size_t)nw, ny_hash_name(scoped, (size_t)nw));
      if (scoped_target)
        return scoped_target;
    }
  }

  const char *direct =
      ny_lookup_module_alias_indexed(cg, name, name_len, name_hash);
  if (direct)
    return direct;
  /*
   * Imported standard-library functions may be emitted by a child codegen
   * context while their module aliases live on the persistent parent.
   */
  if (cg->parent)
    return ny_lookup_module_alias(cg->parent, NULL, 0, name, name_len,
                                  name_hash);
  return NULL;
}

static void collect_use_modules_inner(codegen_t *cg, stmt_t *s,
                                      bool in_std_mod) {
  if (s->kind == NY_S_USE) {
    bool has_alias = !s->as.use.import_all && s->as.use.alias;
    const char *mod = s->as.use.module;
    if (mod && *mod) {
      if (ny_stmt_is_bare_core_use(s)) {
        for (size_t i = 0; i < sizeof(k_core_root_use_modules) /
                                   sizeof(k_core_root_use_modules[0]);
             ++i)
          ny_push_use_module_unique(cg, false, k_core_root_use_modules[i]);
        if (!in_std_mod && !has_alias) {
          for (size_t i = 0; i < sizeof(k_core_root_use_modules) /
                                     sizeof(k_core_root_use_modules[0]);
               ++i)
            ny_push_use_module_unique(cg, true, k_core_root_use_modules[i]);
        }
        return;
      }
      char *norm = normalize_use_module_name_at(cg, mod, s->tok.filename);
      const char *mod_name = (norm && *norm) ? norm : mod;
      ny_push_use_module_unique(cg, false, mod_name);
      if (!in_std_mod && !has_alias)
        ny_push_use_module_unique(cg, true, mod_name);
      free(norm);
    }
  } else if (s->kind == NY_S_MODULE) {
    /*
     * Module spelling is user-controlled. Provenance comes from the parser's
     * registered source identity carried on the module token.
     */
    bool is_std = ny_is_stdlib_tok(s->tok);
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      collect_use_modules_inner(cg, s->as.module.body.data[i],
                                in_std_mod || is_std);
  } else if (s->kind == NY_S_FUNC) {
    if (s->as.fn.body)
      collect_use_modules_inner(cg, s->as.fn.body, in_std_mod);
  } else if (s->kind == NY_S_IF) {
    bool truthy = false;
    if (ny_eval_comptime_if(cg, s, &truthy)) {
      if (truthy) {
        collect_use_modules_inner(cg, s->as.iff.conseq, in_std_mod);
      } else if (s->as.iff.alt) {
        collect_use_modules_inner(cg, s->as.iff.alt, in_std_mod);
      }
    } else {
      collect_use_modules_inner(cg, s->as.iff.conseq, in_std_mod);
      if (s->as.iff.alt)
        collect_use_modules_inner(cg, s->as.iff.alt, in_std_mod);
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      collect_use_modules_inner(cg, s->as.block.body.data[i], in_std_mod);
  }
}

void collect_use_modules(codegen_t *cg, stmt_t *s) {
  collect_use_modules_inner(cg, s, false);
}

static const char *ny_resolve_dotted_module_export_alias(codegen_t *cg,
                                                         const char *name) {
  if (!cg || !name || !*name)
    return NULL;
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name || !dot[1])
    return NULL;

  size_t mod_len = (size_t)(dot - name);
  char stack_mod[512];
  char *mod_name =
      mod_len < sizeof(stack_mod) ? stack_mod : malloc(mod_len + 1);
  if (!mod_name)
    return NULL;
  memcpy(mod_name, name, mod_len);
  mod_name[mod_len] = '\0';

  /*
   * A qualified call through a module alias (for example `core_ref.to_str`)
   * has to resolve the module prefix before looking up its exported member.
   * Treating the alias as a literal module name makes the later unqualified
   * fallback select an unrelated same-named export in the current facade.
   */
  const char *module_path = mod_name;
  size_t module_len = strlen(mod_name);
  const char *module_alias = ny_lookup_module_alias(
      cg, NULL, 0, mod_name, module_len, ny_hash_name(mod_name, module_len));
  if (!module_alias) {
    /*
     * Some generated std-module bodies are emitted after the module context
     * has been cleared.  Recover an unscoped alias only when it is unique;
     * this preserves lexical shadowing instead of guessing between modules.
     */
    const char *unique = NULL;
    size_t matches = 0;
    for (size_t i = 0; i < cg->aliases.len; ++i) {
      binding *alias = &cg->aliases.data[i];
      if (!alias->name || !alias->stmt_t)
        continue;
      const char *leaf = strrchr(alias->name, '.');
      leaf = leaf ? leaf + 1 : alias->name;
      if (strcmp(leaf, mod_name) != 0)
        continue;
      const char *target = (const char *)alias->stmt_t;
      if (!ny_module_lookup_stmt_by_name(cg, target))
        continue;
      unique = target;
      matches++;
    }
    if (matches == 1)
      module_alias = unique;
  }
  if (module_alias && *module_alias)
    module_path = module_alias;

  stmt_t *mod = ny_module_lookup_stmt_by_name(cg, module_path);
  if (!mod) {
    if (mod_name != stack_mod)
      free(mod_name);
    return NULL;
  }

  char local_target[1024];
  if (!ny_module_local_import_target_memo(cg, mod, dot + 1, local_target,
                                          sizeof(local_target))) {
    if (mod_name != stack_mod)
      free(mod_name);
    return NULL;
  }
  if (mod_name != stack_mod)
    free(mod_name);

  char resolved_target[1024];
  const char *target = ny_resolve_export_source(
      cg, local_target, resolved_target, sizeof(resolved_target), 0);
  if (!target || !*target)
    return NULL;
  if (!lookup_fun_exact(cg, target) && !lookup_global_exact(cg, target) &&
      !ny_module_lookup_stmt_by_name(cg, target))
    return NULL;

  ny_push_import_alias_unique_ex(cg, false, name, target, false);
  bool user_only = ny_module_user_ctx_is_non_std(cg);
  if (user_only)
    ny_push_import_alias_unique_ex(cg, true, name, target, false);

  size_t name_len = strlen(name);
  uint64_t name_hash = ny_hash_name(name, name_len);
  return ny_lookup_import_alias_indexed(cg, user_only, name, name_len,
                                        name_hash);
}

/*
 * ny_module_local_import_target answers from the module's AST alone (its
 * `use` statements, comptime guards and public surfaces, all fixed once the
 * program is expanded). Bare-name alias resolution asks it for every used
 * module and every unresolved identifier, mostly locals, and each walk
 * re-normalized every `use` path: 11% of a large serial compile. Remember
 * the answer per (codegen, module, name).
 */
typedef struct ny_local_import_memo_t {
  const codegen_t *cg;
  const stmt_t *mod;
  char *name;
  char *target; /* NULL when the module does not import the name */
  uint64_t hash;
} ny_local_import_memo_t;
static _Thread_local ny_local_import_memo_t *g_local_import_memo;
static _Thread_local size_t g_local_import_memo_cap, g_local_import_memo_len;

static uint64_t ny_local_import_memo_hash(const codegen_t *cg,
                                          const stmt_t *mod,
                                          const char *name) {
  uint64_t h = 1469598103934665603ULL ^ (uint64_t)(uintptr_t)cg;
  h = (h ^ (uint64_t)(uintptr_t)mod) * 1099511628211ULL;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    h = (h ^ *p) * 1099511628211ULL;
  return h ? h : 1;
}

static bool ny_local_import_memo_grow(void) {
  size_t cap = g_local_import_memo_cap ? g_local_import_memo_cap * 2 : 4096;
  ny_local_import_memo_t *t = calloc(cap, sizeof(*t));
  if (!t)
    return false;
  for (size_t i = 0; i < g_local_import_memo_cap; ++i) {
    ny_local_import_memo_t *e = &g_local_import_memo[i];
    if (!e->hash)
      continue;
    size_t at = e->hash & (cap - 1);
    while (t[at].hash)
      at = (at + 1) & (cap - 1);
    t[at] = *e;
  }
  free(g_local_import_memo);
  g_local_import_memo = t;
  g_local_import_memo_cap = cap;
  return true;
}

static bool ny_module_local_import_target(codegen_t *cg, stmt_t *mod,
                                          const char *target, char *out,
                                          size_t out_cap, int depth);
static bool ny_module_local_import_target_memo(codegen_t *cg, stmt_t *mod,
                                               const char *name, char *out,
                                               size_t out_cap);

static bool ny_no_import_memo(void) {
  static int cached = -1;
  if (cached < 0)
    cached = getenv("NYTRIX_NO_IMPORT_MEMO") ? 1 : 0;
  return cached != 0;
}

static bool ny_module_local_import_target_memo(codegen_t *cg, stmt_t *mod,
                                               const char *name, char *out,
                                               size_t out_cap) {
  if (ny_no_import_memo())
    return ny_module_local_import_target(cg, mod, name, out, out_cap, 0);
  if ((g_local_import_memo_len + 1) * 10 >= g_local_import_memo_cap * 7 &&
      !ny_local_import_memo_grow())
    return ny_module_local_import_target(cg, mod, name, out, out_cap, 0);
  uint64_t h = ny_local_import_memo_hash(cg, mod, name);
  size_t mask = g_local_import_memo_cap - 1;
  size_t at = h & mask;
  for (; g_local_import_memo[at].hash; at = (at + 1) & mask) {
    ny_local_import_memo_t *e = &g_local_import_memo[at];
    if (e->hash == h && e->cg == cg && e->mod == mod &&
        strcmp(e->name, name) == 0) {
      if (!e->target)
        return false;
      size_t n = strlen(e->target);
      if (n >= out_cap)
        return ny_module_local_import_target(cg, mod, name, out, out_cap, 0);
      memcpy(out, e->target, n + 1);
      return true;
    }
  }
  bool found = ny_module_local_import_target(cg, mod, name, out, out_cap, 0);
  char *key = strdup(name);
  char *value = found ? strdup(out) : NULL;
  if (!key || (found && !value)) {
    free(key);
    free(value);
    return found;
  }
  g_local_import_memo[at] = (ny_local_import_memo_t){
      .cg = cg, .mod = mod, .name = key, .target = value, .hash = h};
  g_local_import_memo_len++;
  return found;
}

typedef struct ny_used_export_neg_entry_t {
  uint64_t hash;
  char *name;
} ny_used_export_neg_entry_t;

static _Thread_local ny_used_export_neg_entry_t *g_used_export_neg;
static _Thread_local size_t g_used_export_neg_cap, g_used_export_neg_len;
static _Thread_local const codegen_t *g_used_export_neg_cg;
static _Thread_local size_t g_used_export_neg_mods_len = SIZE_MAX;
static _Thread_local size_t g_used_export_neg_extra_len = SIZE_MAX;
static _Thread_local size_t g_used_export_neg_funs_len = SIZE_MAX;
static _Thread_local size_t g_used_export_neg_globs_len = SIZE_MAX;

static void ny_used_export_neg_reset(void) {
  for (size_t i = 0; i < g_used_export_neg_cap; ++i)
    free(g_used_export_neg[i].name);
  free(g_used_export_neg);
  g_used_export_neg = NULL;
  g_used_export_neg_cap = 0;
  g_used_export_neg_len = 0;
}

static bool ny_used_export_neg_check(const codegen_t *cg, size_t mods_len,
                                     const char *name, uint64_t h) {
  if (g_used_export_neg_cg != cg || g_used_export_neg_mods_len != mods_len ||
      g_used_export_neg_extra_len != cg->extra_progs.len ||
      g_used_export_neg_funs_len != cg->fun_sigs.len ||
      g_used_export_neg_globs_len != cg->global_vars.len) {
    ny_used_export_neg_reset();
    g_used_export_neg_cg = cg;
    g_used_export_neg_mods_len = mods_len;
    g_used_export_neg_extra_len = cg->extra_progs.len;
    g_used_export_neg_funs_len = cg->fun_sigs.len;
    g_used_export_neg_globs_len = cg->global_vars.len;
    return false;
  }
  if (!g_used_export_neg || g_used_export_neg_cap == 0)
    return false;
  size_t mask = g_used_export_neg_cap - 1u;
  size_t at = h & mask;
  for (; g_used_export_neg[at].hash; at = (at + 1u) & mask) {
    if (g_used_export_neg[at].hash == h &&
        strcmp(g_used_export_neg[at].name, name) == 0)
      return true;
  }
  return false;
}

static void ny_used_export_neg_add(const char *name, uint64_t h) {
  if ((g_used_export_neg_len + 1u) * 10u >= g_used_export_neg_cap * 7u) {
    size_t cap = g_used_export_neg_cap ? g_used_export_neg_cap * 2u : 1024u;
    ny_used_export_neg_entry_t *t = calloc(cap, sizeof(*t));
    if (!t)
      return;
    for (size_t i = 0; i < g_used_export_neg_cap; ++i) {
      if (!g_used_export_neg[i].hash)
        continue;
      size_t at = g_used_export_neg[i].hash & (cap - 1u);
      while (t[at].hash)
        at = (at + 1u) & (cap - 1u);
      t[at] = g_used_export_neg[i];
    }
    free(g_used_export_neg);
    g_used_export_neg = t;
    g_used_export_neg_cap = cap;
  }
  size_t mask = g_used_export_neg_cap - 1u;
  size_t at = h & mask;
  for (; g_used_export_neg[at].hash; at = (at + 1u) & mask) {
    if (g_used_export_neg[at].hash == h &&
        strcmp(g_used_export_neg[at].name, name) == 0)
      return;
  }
  char *dup = strdup(name);
  if (!dup)
    return;
  g_used_export_neg[at].hash = h;
  g_used_export_neg[at].name = dup;
  g_used_export_neg_len++;
}

const char *ny_resolve_used_module_export_alias(codegen_t *cg,
                                                const char *name) {
  if (!cg || !name || !*name)
    return NULL;

  if (strchr(name, '.'))
    return ny_resolve_dotted_module_export_alias(cg, name);

  if (!ny_module_user_ctx_is_non_std(cg))
    return NULL;

  bool user_only = true;
  char **mods = user_only ? cg->user_use_modules.data : cg->use_modules.data;
  size_t mods_len = user_only ? cg->user_use_modules.len : cg->use_modules.len;
  if (!mods || mods_len == 0)
    return NULL;

  uint64_t name_h = ny_hash_name(name, strlen(name));
  if (!name_h)
    name_h = 1u;
  if (!ny_no_import_memo() &&
      ny_used_export_neg_check(cg, mods_len, name, name_h))
    return NULL;

  for (size_t i = mods_len; i > 0; --i) {
    const char *mod_name = mods[i - 1];
    if (!mod_name || !*mod_name)
      continue;
    stmt_t *mod = ny_module_lookup_stmt_by_name(cg, mod_name);
    if (!mod)
      continue;

    char local_target[1024];
    if (!ny_module_local_import_target_memo(cg, mod, name, local_target,
                                            sizeof(local_target)))
      continue;

    char resolved_target[1024];
    const char *target = ny_resolve_export_source(
        cg, local_target, resolved_target, sizeof(resolved_target), 0);
    if (!target || !*target)
      continue;
    if (!lookup_fun_exact(cg, target) && !lookup_global_exact(cg, target) &&
        !ny_module_lookup_stmt_by_name(cg, target))
      continue;

    ny_push_import_alias_unique_ex(cg, false, name, target, false);
    if (user_only)
      ny_push_import_alias_unique_ex(cg, true, name, target, false);

    size_t name_len = strlen(name);
    uint64_t name_hash = ny_hash_name(name, name_len);
    return ny_lookup_import_alias_indexed(cg, user_only, name, name_len,
                                          name_hash);
  }

  if (!ny_no_import_memo())
    ny_used_export_neg_add(name, name_h);
  return NULL;
}

typedef struct ny_active_mod_slot_t {
  uint64_t hash;
  const char *name;
} ny_active_mod_slot_t;

static _Thread_local struct {
  const codegen_t *cg;
  size_t use_len;
  size_t user_use_len;
  ny_active_mod_slot_t *slots;
  size_t cap;
} g_active_mod_set;

static void ny_active_mod_set_rebuild(const codegen_t *cg) {
  size_t total = cg->use_modules.len + cg->user_use_modules.len;
  size_t cap = 64u;
  while (cap < total * 2u + 8u)
    cap <<= 1u;
  ny_active_mod_slot_t *slots = calloc(cap, sizeof(*slots));
  if (!slots)
    return;
  size_t mask = cap - 1u;
  for (int pass = 0; pass < 2; ++pass) {
    char **data =
        pass == 0 ? cg->use_modules.data : cg->user_use_modules.data;
    size_t len = pass == 0 ? cg->use_modules.len : cg->user_use_modules.len;
    for (size_t i = 0; i < len; ++i) {
      const char *nm = data[i];
      if (!nm || !*nm)
        continue;
      uint64_t h = ny_hash_name(nm, strlen(nm));
      if (!h)
        h = 1u;
      size_t pos = (size_t)h & mask;
      while (slots[pos].hash) {
        if (slots[pos].hash == h && strcmp(slots[pos].name, nm) == 0)
          break;
        pos = (pos + 1u) & mask;
      }
      if (!slots[pos].hash) {
        slots[pos].hash = h;
        slots[pos].name = nm;
      }
    }
  }
  free(g_active_mod_set.slots);
  g_active_mod_set.cg = cg;
  g_active_mod_set.use_len = cg->use_modules.len;
  g_active_mod_set.user_use_len = cg->user_use_modules.len;
  g_active_mod_set.slots = slots;
  g_active_mod_set.cap = cap;
}

bool ny_is_module_active(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return false;
  if (cg->current_module_name && strcmp(cg->current_module_name, name) == 0)
    return true;
  if (cg->use_modules.len + cg->user_use_modules.len > 0) {
    if (g_active_mod_set.cg != cg ||
        g_active_mod_set.use_len != cg->use_modules.len ||
        g_active_mod_set.user_use_len != cg->user_use_modules.len)
      ny_active_mod_set_rebuild(cg);
    if (g_active_mod_set.slots && g_active_mod_set.cap) {
      uint64_t h = ny_hash_name(name, strlen(name));
      if (!h)
        h = 1u;
      size_t mask = g_active_mod_set.cap - 1u;
      size_t pos = (size_t)h & mask;
      for (; g_active_mod_set.slots[pos].hash; pos = (pos + 1u) & mask) {
        if (g_active_mod_set.slots[pos].hash == h &&
            strcmp(g_active_mod_set.slots[pos].name, name) == 0)
          return true;
      }
    } else {
      for (size_t i = 0; i < cg->use_modules.len; ++i) {
        if (cg->use_modules.data[i] &&
            strcmp(cg->use_modules.data[i], name) == 0)
          return true;
      }
      for (size_t i = 0; i < cg->user_use_modules.len; ++i) {
        if (cg->user_use_modules.data[i] &&
            strcmp(cg->user_use_modules.data[i], name) == 0)
          return true;
      }
    }
  }
  return cg->parent ? ny_is_module_active(cg->parent, name) : false;
}

static bool ny_module_path_write(char *out, size_t out_cap, const char *first,
                                 const char *second) {
  if (!out || !out_cap || !first)
    return false;
  size_t first_len = strlen(first);
  size_t second_len = second ? strlen(second) : 0;
  if (second &&
      (second_len > SIZE_MAX - 2 || first_len > SIZE_MAX - second_len - 2))
    return false;
  size_t len = first_len + second_len + (second ? 1u : 0u);
  if (!len || len >= out_cap)
    return false;
  memcpy(out, first, first_len);
  if (second) {
    out[first_len] = '.';
    memcpy(out + first_len + 1u, second, second_len);
  }
  out[len] = '\0';
  return true;
}

/*
 * Per-statement facts the import walks recomputed on every query: a `use`
 * statement's normalized module name and declaration, and a module's
 * exported child modules. They depend only on the parsed program, so they
 * are kept per (codegen, statement) and dropped when another program is
 * attached. Bare-name alias resolution asks the walks for every used module
 * and unresolved identifier; normalizing paths and building child names
 * there was most of its cost.
 */
typedef struct {
  const codegen_t *cg;
  const stmt_t *key;
  char *mod;      /* use: normalized module name */
  stmt_t *used;   /* use: declaration of mod, or NULL */
  struct ny_child_export_t *children; /* module: exported child modules */
  size_t child_len;
  bool children_ready;
  bool child_failed; /* child list incomplete: walk uncached */
} ny_stmt_fact_t;

typedef struct ny_child_export_t {
  char *path;
  stmt_t *mod;
} ny_child_export_t;

static _Thread_local ny_stmt_fact_t *g_stmt_facts;
static _Thread_local size_t g_stmt_facts_cap, g_stmt_facts_len;
static _Thread_local size_t g_stmt_facts_progs = SIZE_MAX;
static _Thread_local const codegen_t *g_stmt_facts_cg;

static void ny_stmt_facts_reset(void) {
  for (size_t i = 0; i < g_stmt_facts_cap; ++i) {
    ny_stmt_fact_t *f = &g_stmt_facts[i];
    if (!f->key)
      continue;
    free(f->mod);
    for (size_t k = 0; k < f->child_len; ++k)
      free(f->children[k].path);
    free(f->children);
  }
  free(g_stmt_facts);
  g_stmt_facts = NULL;
  g_stmt_facts_cap = g_stmt_facts_len = 0;
}

/*
 * Returns the slot for (cg, key); *fresh says whether it must be filled.
 */
static bool ny_no_use_fact_cache(void) {
  static int cached = -1;
  if (cached < 0)
    cached = getenv("NYTRIX_NO_USE_FACT_CACHE") ? 1 : 0;
  return cached != 0;
}

static ny_stmt_fact_t *ny_stmt_fact_slot(codegen_t *cg, const stmt_t *key,
                                         bool *fresh) {
  *fresh = false;
  if (ny_no_use_fact_cache())
    return NULL;
  if (g_stmt_facts_cg != cg || g_stmt_facts_progs != cg->extra_progs.len) {
    ny_stmt_facts_reset();
    g_stmt_facts_cg = cg;
    g_stmt_facts_progs = cg->extra_progs.len;
  }
  if ((g_stmt_facts_len + 1) * 2 > g_stmt_facts_cap) {
    size_t cap = g_stmt_facts_cap ? g_stmt_facts_cap * 2 : 1024;
    ny_stmt_fact_t *t = calloc(cap, sizeof(*t));
    if (!t)
      return NULL;
    for (size_t i = 0; i < g_stmt_facts_cap; ++i) {
      if (!g_stmt_facts[i].key)
        continue;
      size_t at = ((uintptr_t)g_stmt_facts[i].key >> 4) & (cap - 1);
      while (t[at].key)
        at = (at + 1) & (cap - 1);
      t[at] = g_stmt_facts[i];
    }
    free(g_stmt_facts);
    g_stmt_facts = t;
    g_stmt_facts_cap = cap;
  }
  size_t mask = g_stmt_facts_cap - 1;
  size_t at = ((uintptr_t)key >> 4) & mask;
  while (g_stmt_facts[at].key && g_stmt_facts[at].key != key)
    at = (at + 1) & mask;
  if (!g_stmt_facts[at].key) {
    g_stmt_facts[at] = (ny_stmt_fact_t){.cg = cg, .key = key};
    g_stmt_facts_len++;
    *fresh = true;
  }
  return &g_stmt_facts[at];
}

/*
 * The normalized module of a `use` statement; owned by the cache unless
 * *owned is set (caller frees).
 */
static const char *ny_use_stmt_module(codegen_t *cg, stmt_t *s, stmt_t **used,
                                      bool *owned) {
  bool fresh = false;
  ny_stmt_fact_t *f = ny_stmt_fact_slot(cg, s, &fresh);
  if (!f) {
    char *mod =
        normalize_use_module_name_at(cg, s->as.use.module, s->tok.filename);
    *owned = true;
    if (used)
      *used = mod ? ny_module_lookup_stmt_by_name(cg, mod) : NULL;
    return mod;
  }
  *owned = false;
  if (fresh) {
    f->mod = normalize_use_module_name_at(cg, s->as.use.module, s->tok.filename);
    f->used = f->mod ? ny_module_lookup_stmt_by_name(cg, f->mod) : NULL;
  }
  if (used)
    *used = f->used;
  return f->mod;
}

static bool ny_module_local_import_target_inner(codegen_t *cg, stmt_t *s,
                                                const char *target, char *out,
                                                size_t out_cap, int depth) {
  if (!cg || !s || !target || !*target || !out || out_cap == 0 || depth > 8)
    return false;
  if (s->kind == NY_S_USE) {
    bool mod_owned = false;
    stmt_t *cached_used = NULL;
    char *mod = (char *)ny_use_stmt_module(cg, s, &cached_used, &mod_owned);
    if (!mod)
      return false;
    bool alias_only =
        !s->as.use.import_all && s->as.use.imports.len == 0 && s->as.use.alias;
    if (alias_only && s->as.use.alias && strcmp(s->as.use.alias, target) == 0) {
      bool copied = ny_module_path_write(out, out_cap, mod, NULL);
      if (mod_owned)
        free(mod);
      return copied;
    }
    if (s->as.use.imports.len > 0) {
      for (size_t i = 0; i < s->as.use.imports.len; ++i) {
        use_item_t *item = &s->as.use.imports.data[i];
        if (!item->name)
          continue;
        const char *local = item->alias ? item->alias : item->name;
        if (!local || strcmp(local, target) != 0)
          continue;
        bool copied = ny_module_path_write(out, out_cap, mod, item->name);
        if (mod_owned)
        free(mod);
        return copied;
      }
      if (mod_owned)
        free(mod);
      return false;
    }

    bool bare_use =
        !s->as.use.import_all && s->as.use.imports.len == 0 && !s->as.use.alias;
    if (s->as.use.import_all || bare_use) {
      const char *leaf = ny_name_leaf(mod);
      if (bare_use && leaf && strcmp(leaf, target) == 0 &&
          (mod_owned ? ny_module_lookup_stmt_by_name(cg, mod)
                     : cached_used)) {
        bool copied = ny_module_path_write(out, out_cap, mod, NULL);
        if (mod_owned)
        free(mod);
        return copied;
      }
      stmt_t *used =
          mod_owned ? ny_module_lookup_stmt_by_name(cg, mod) : cached_used;
      if (used && ny_module_public_target(cg, used, target, out, out_cap,
                                          s->as.use.profile, true, depth + 1)) {
        if (mod_owned)
        free(mod);
        return true;
      }
      if (!used) {
        bool copied = ny_module_path_write(out, out_cap, mod, target);
        if (mod_owned)
        free(mod);
        return copied;
      }
    }
    if (mod_owned)
      if (mod_owned)
        free(mod);
    return false;
  }
  if (s->kind == NY_S_IF) {
    bool truthy = false;
    if (ny_eval_comptime_if(cg, s, &truthy)) {
      if (truthy)
        return ny_module_local_import_target_inner(cg, s->as.iff.conseq, target,
                                                   out, out_cap, depth + 1);
      if (s->as.iff.alt)
        return ny_module_local_import_target_inner(cg, s->as.iff.alt, target,
                                                   out, out_cap, depth + 1);
      return false;
    }
    if (ny_module_local_import_target_inner(cg, s->as.iff.conseq, target, out,
                                            out_cap, depth + 1))
      return true;
    return s->as.iff.alt &&
           ny_module_local_import_target_inner(cg, s->as.iff.alt, target, out,
                                               out_cap, depth + 1);
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (ny_module_local_import_target_inner(cg, s->as.block.body.data[i],
                                              target, out, out_cap, depth + 1))
        return true;
    }
  }
  return false;
}

static ny_stmt_fact_t *ny_module_ensure_child_exports(codegen_t *cg,
                                                      stmt_t *mod) {
  if (!cg || !mod || mod->kind != NY_S_MODULE)
    return NULL;
  const char *mod_name = mod->as.module.name;
  if (!mod_name || !*mod_name)
    return NULL;
  bool fresh = false;
  ny_stmt_fact_t *facts = ny_stmt_fact_slot(cg, mod, &fresh);
  if (!facts)
    return NULL;
  if (!facts->children_ready) {
    facts->children_ready = true;
    for (size_t i = 0; i < mod->as.module.body.len; ++i) {
      stmt_t *child = mod->as.module.body.data[i];
      if (!child || child->kind != NY_S_EXPORT || child->as.exprt.is_internal)
        continue;
      if (!ny_export_profile_visible(child->as.exprt.profile, NULL))
        continue;
      for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
        const char *name = child->as.exprt.names.data[j];
        if (!name || !*name)
          continue;
        char child_path[1024];
        bool copied =
            strchr(name, '.')
                ? ny_module_path_write(child_path, sizeof(child_path), name,
                                       NULL)
                : ny_module_path_write(child_path, sizeof(child_path),
                                       mod_name, name);
        if (!copied)
          continue;
        stmt_t *child_mod = ny_module_lookup_stmt_by_name(cg, child_path);
        if (!child_mod)
          continue;
        ny_child_export_t *grown = realloc(
            facts->children, (facts->child_len + 1) * sizeof(*grown));
        char *dup = strdup(child_path);
        if (!grown || !dup) {
          free(dup);
          if (grown)
            facts->children = grown;
          facts->child_failed = true;
          break;
        }
        facts->children = grown;
        facts->children[facts->child_len++] =
            (ny_child_export_t){.path = dup, .mod = child_mod};
      }
      if (facts->child_failed)
        break;
    }
  }
  return facts;
}

static bool ny_module_exported_child_target(codegen_t *cg, stmt_t *mod,
                                            const char *target, char *out,
                                            size_t out_cap, int depth) {
  if (!cg || !mod || mod->kind != NY_S_MODULE || !target || !*target || !out ||
      out_cap == 0 || depth > 8)
    return false;
  const char *mod_name = mod->as.module.name;
  if (!mod_name || !*mod_name)
    return false;
  ny_stmt_fact_t *facts = ny_module_ensure_child_exports(cg, mod);
  if (facts && !facts->child_failed) {
    for (size_t k = 0; k < facts->child_len; ++k) {
      const char *child_path = facts->children[k].path;
      const char *child_leaf = ny_name_leaf(child_path);
      if (child_leaf && strcmp(child_leaf, target) == 0)
        return ny_module_path_write(out, out_cap, child_path, NULL);
      if (ny_module_public_target(cg, facts->children[k].mod, target, out,
                                  out_cap, NULL, true, depth + 1))
        return true;
    }
    return false;
  }
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (!child || child->kind != NY_S_EXPORT || child->as.exprt.is_internal)
      continue;
    if (!ny_export_profile_visible(child->as.exprt.profile, NULL))
      continue;
    for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
      const char *name = child->as.exprt.names.data[j];
      if (!name || !*name)
        continue;
      char child_path[1024];
      bool copied =
          strchr(name, '.')
              ? ny_module_path_write(child_path, sizeof(child_path), name, NULL)
              : ny_module_path_write(child_path, sizeof(child_path), mod_name,
                                     name);
      if (!copied)
        continue;
      stmt_t *child_mod = ny_module_lookup_stmt_by_name(cg, child_path);
      if (!child_mod)
        continue;
      const char *child_leaf = ny_name_leaf(child_path);
      if (child_leaf && strcmp(child_leaf, target) == 0) {
        return ny_module_path_write(out, out_cap, child_path, NULL);
      }
      if (ny_module_public_target(cg, child_mod, target, out, out_cap, NULL,
                                  true, depth + 1))
        return true;
    }
  }
  return false;
}

static uint64_t ny_module_public_target_hash(stmt_t *mod, const char *target,
                                             size_t target_len,
                                             const char *profile,
                                             size_t profile_len,
                                             bool include_child_uses) {
  uint64_t h = ny_hash_name(target, target_len);
  h = ny_module_mix64(h, (uint64_t)(uintptr_t)mod);
  if (profile_len > 0)
    h = ny_module_mix64(h, ny_hash_name(profile, profile_len));
  h = ny_module_mix64(h, include_child_uses ? 1u : 0u);
  return h ? h : 1u;
}

static bool
ny_module_public_target_uncached(codegen_t *cg, stmt_t *mod, const char *target,
                                 char *out, size_t out_cap, const char *profile,
                                 bool include_child_uses, int depth) {
  if (!cg || !mod || mod->kind != NY_S_MODULE || !target || !*target || !out ||
      out_cap == 0 || depth > 8)
    return false;
  const char *mod_name = mod->as.module.name;
  if (!mod_name || !*mod_name)
    return false;

  bool has_export_list = module_has_export_list(mod);
  if (has_export_list) {
    ny_stmt_fact_t *child_facts =
        (include_child_uses && (!profile || !*profile))
            ? ny_module_ensure_child_exports(cg, mod)
            : NULL;
    bool use_child_facts = child_facts && !child_facts->child_failed;
    for (size_t i = 0; i < mod->as.module.body.len; ++i) {
      stmt_t *child = mod->as.module.body.data[i];
      if (!child || child->kind != NY_S_EXPORT || child->as.exprt.is_internal)
        continue;
      if (!ny_export_profile_visible(child->as.exprt.profile, profile))
        continue;
      for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
        const char *name = child->as.exprt.names.data[j];
        if (!name || !*name)
          continue;
        const char *name_leaf = ny_name_leaf(name);
        bool leaf_match = name_leaf && strcmp(name_leaf, target) == 0;
        if (!leaf_match &&
            (!include_child_uses ||
             (use_child_facts && child_facts->child_len == 0)))
          continue;
        char full[1024];
        bool copied =
            strchr(name, '.')
                ? ny_module_path_write(full, sizeof(full), name, NULL)
                : ny_module_path_write(full, sizeof(full), mod_name, name);
        if (!copied)
          continue;
        if (leaf_match) {
          if (lookup_fun_exact(cg, full) || lookup_global_exact(cg, full) ||
              ny_module_lookup_stmt_by_name(cg, full)) {
            return ny_module_path_write(out, out_cap, full, NULL);
          }
          char import_target[1024];
          if (ny_module_local_import_target(cg, mod, target, import_target,
                                            sizeof(import_target), depth + 1)) {
            return ny_module_path_write(out, out_cap, import_target, NULL);
          }
        }
        if (!include_child_uses)
          continue;
        stmt_t *child_mod = NULL;
        if (use_child_facts) {
          for (size_t k = 0; k < child_facts->child_len; ++k) {
            if (strcmp(child_facts->children[k].path, full) == 0) {
              child_mod = child_facts->children[k].mod;
              break;
            }
          }
        } else {
          child_mod = ny_module_lookup_stmt_by_name(cg, full);
        }
        if (child_mod &&
            ny_module_public_target(cg, child_mod, target, out, out_cap,
                                    profile, true, depth + 1))
          return true;
      }
    }
  }

  if (!has_export_list || mod->as.module.export_all) {
    for (size_t i = 0; i < mod->as.module.body.len; ++i) {
      stmt_t *child = mod->as.module.body.data[i];
      if (!child)
        continue;
      if (child->kind == NY_S_FUNC && child->as.fn.name &&
          strcmp(child->as.fn.name, target) == 0 &&
          !ny_module_name_is_internal(mod, child->as.fn.name)) {
        const char *q = codegen_qname(cg, child->as.fn.name, mod_name);
        return ny_module_path_write(out, out_cap, q, NULL);
      }
      if (child->kind == NY_S_VAR) {
        for (size_t j = 0; j < child->as.var.names.len; ++j) {
          const char *name = child->as.var.names.data[j];
          if (name && strcmp(name, target) == 0 &&
              !ny_module_name_is_internal(mod, name)) {
            const char *q = codegen_qname(cg, name, mod_name);
            return ny_module_path_write(out, out_cap, q, NULL);
          }
        }
      }
    }
  }

  return false;
}

typedef struct ny_pub_target_memo_slot_t {
  uint64_t hash;
  const codegen_t *cg;
  const stmt_t *mod;
  const void *body_data;
  size_t body_len;
  uint8_t include_child_uses;
  char *target;
  char *profile;
  char *value; /* NULL when not found */
} ny_pub_target_memo_slot_t;

static _Thread_local ny_pub_target_memo_slot_t *g_pub_target_memo;
static _Thread_local size_t g_pub_target_memo_cap, g_pub_target_memo_len;
static _Thread_local const codegen_t *g_pub_target_memo_cg;
static _Thread_local size_t g_pub_target_memo_extra_len = SIZE_MAX;

static void ny_pub_target_memo_reset(void) {
  for (size_t i = 0; i < g_pub_target_memo_cap; ++i) {
    free(g_pub_target_memo[i].target);
    free(g_pub_target_memo[i].profile);
    free(g_pub_target_memo[i].value);
  }
  free(g_pub_target_memo);
  g_pub_target_memo = NULL;
  g_pub_target_memo_cap = 0;
  g_pub_target_memo_len = 0;
}

static bool ny_pub_target_memo_grow(void) {
  size_t cap = g_pub_target_memo_cap ? g_pub_target_memo_cap * 2u : 4096u;
  ny_pub_target_memo_slot_t *t = calloc(cap, sizeof(*t));
  if (!t)
    return false;
  size_t mask = cap - 1u;
  for (size_t i = 0; i < g_pub_target_memo_cap; ++i) {
    if (!g_pub_target_memo[i].hash)
      continue;
    size_t pos = (size_t)g_pub_target_memo[i].hash & mask;
    while (t[pos].hash)
      pos = (pos + 1u) & mask;
    t[pos] = g_pub_target_memo[i];
  }
  free(g_pub_target_memo);
  g_pub_target_memo = t;
  g_pub_target_memo_cap = cap;
  return true;
}

static bool ny_module_public_target(codegen_t *cg, stmt_t *mod,
                                    const char *target, char *out,
                                    size_t out_cap, const char *profile,
                                    bool include_child_uses, int depth) {
  if (!cg || !mod || mod->kind != NY_S_MODULE || !target || !*target || !out ||
      out_cap == 0 || depth > 8)
    return false;

  if (g_pub_target_memo_cg != cg ||
      g_pub_target_memo_extra_len != cg->extra_progs.len) {
    ny_pub_target_memo_reset();
    g_pub_target_memo_cg = cg;
    g_pub_target_memo_extra_len = cg->extra_progs.len;
  }

  size_t target_len = strlen(target);
  size_t profile_len = profile ? strlen(profile) : 0;
  uint64_t hash = ny_module_public_target_hash(
      mod, target, target_len, profile, profile_len, include_child_uses);
  uint8_t inc = include_child_uses ? 1u : 0u;
  if (g_pub_target_memo && g_pub_target_memo_cap) {
    size_t mask = g_pub_target_memo_cap - 1u;
    size_t pos = (size_t)hash & mask;
    for (; g_pub_target_memo[pos].hash; pos = (pos + 1u) & mask) {
      ny_pub_target_memo_slot_t *e = &g_pub_target_memo[pos];
      if (e->hash == hash && e->cg == cg && e->mod == mod &&
          e->body_data == mod->as.module.body.data &&
          e->body_len == mod->as.module.body.len &&
          e->include_child_uses == inc && strcmp(e->target, target) == 0 &&
          ((!e->profile && profile_len == 0) ||
           (e->profile && profile && strcmp(e->profile, profile) == 0))) {
        if (!e->value)
          return false;
        size_t vlen = strlen(e->value);
        if (vlen >= out_cap)
          return false;
        memcpy(out, e->value, vlen + 1u);
        return true;
      }
    }
  }

  char tmp[NY_MODULE_PUBLIC_TARGET_VALUE_MAX];
  bool ok = ny_module_public_target_uncached(
      cg, mod, target, tmp, sizeof(tmp), profile, include_child_uses, depth);
  if ((g_pub_target_memo_len + 1u) * 10u >= g_pub_target_memo_cap * 7u)
    (void)ny_pub_target_memo_grow();
  if (g_pub_target_memo && g_pub_target_memo_cap) {
    size_t mask = g_pub_target_memo_cap - 1u;
    size_t pos = (size_t)hash & mask;
    while (g_pub_target_memo[pos].hash) {
      ny_pub_target_memo_slot_t *e = &g_pub_target_memo[pos];
      if (e->hash == hash && e->cg == cg && e->mod == mod &&
          e->body_data == mod->as.module.body.data &&
          e->body_len == mod->as.module.body.len &&
          e->include_child_uses == inc && strcmp(e->target, target) == 0 &&
          ((!e->profile && profile_len == 0) ||
           (e->profile && profile && strcmp(e->profile, profile) == 0))) {
        free(e->value);
        e->value = ok ? ny_strdup(tmp) : NULL;
        goto copy_out;
      }
      pos = (pos + 1u) & mask;
    }
    char *tdup = ny_strdup(target);
    char *pdup = profile_len ? ny_strdup(profile) : NULL;
    char *vdup = ok ? ny_strdup(tmp) : NULL;
    if (tdup && (!profile_len || pdup) && (!ok || vdup)) {
      g_pub_target_memo[pos] = (ny_pub_target_memo_slot_t){
          .hash = hash,
          .cg = cg,
          .mod = mod,
          .body_data = mod->as.module.body.data,
          .body_len = mod->as.module.body.len,
          .include_child_uses = inc,
          .target = tdup,
          .profile = pdup,
          .value = vdup};
      g_pub_target_memo_len++;
    } else {
      free(tdup);
      free(pdup);
      free(vdup);
    }
  }
copy_out:
  if (!ok)
    return false;
  size_t value_len = strlen(tmp);
  if (value_len >= out_cap)
    return false;
  memcpy(out, tmp, value_len + 1u);
  return true;
}

static bool ny_module_local_import_target(codegen_t *cg, stmt_t *mod,
                                          const char *target, char *out,
                                          size_t out_cap, int depth) {
  if (!mod || mod->kind != NY_S_MODULE || depth > 8)
    return false;
  for (size_t i = 0; i < mod->as.module.body.len; ++i) {
    stmt_t *child = mod->as.module.body.data[i];
    if (!child || child->kind == NY_S_MODULE)
      continue;
    if (ny_module_local_import_target_inner(cg, child, target, out, out_cap,
                                            depth + 1))
      return true;
  }
  if (ny_module_exported_child_target(cg, mod, target, out, out_cap, depth + 1))
    return true;
  return false;
}

static const char *ny_resolve_export_source(codegen_t *cg, const char *source,
                                            char *out, size_t out_cap,
                                            int depth) {
  if (!cg || !source || !*source || !out || out_cap == 0 || depth > 16)
    return source;
  if (lookup_fun_exact(cg, source) || lookup_global_exact(cg, source))
    return source;
  /*
   * A declared MODULE path is its own export source.  Without this check
   * "core.os.io" was treated as the export "io" of the parent core.os facade,
   * and the facade's own `use core.io` rewrote the target to
   * core.io, so osio.spawn failed with "no exported member 'spawn'".
   */
  if (ny_module_lookup_stmt_by_name(cg, source))
    return source;
  const char *dot = strrchr(source, '.');
  if (!dot || dot == source || !dot[1])
    return source;
  size_t mod_len = (size_t)(dot - source);
  char stack_mod[512];
  char *mod_name =
      mod_len < sizeof(stack_mod) ? stack_mod : malloc(mod_len + 1);
  if (!mod_name)
    return source;
  memcpy(mod_name, source, mod_len);
  mod_name[mod_len] = '\0';
  stmt_t *mod = ny_module_lookup_stmt_by_name(cg, mod_name);
  if (mod_name != stack_mod)
    free(mod_name);
  if (!mod)
    return source;

  char local[1024];
  if (!ny_module_local_import_target(cg, mod, dot + 1, local, sizeof(local),
                                     depth + 1))
    return source;
  const char *resolved =
      ny_resolve_export_source(cg, local, out, out_cap, depth + 1);
  if (resolved != out) {
    int nw = snprintf(out, out_cap, "%s", resolved);
    if (nw <= 0 || (size_t)nw >= out_cap)
      return source;
  }
  return out;
}

static bool ny_copy_module_path(char *out, size_t out_cap, const char *path) {
  if (!out || out_cap == 0 || !path || !*path)
    return false;
  int nw = snprintf(out, out_cap, "%s", path);
  return nw > 0 && (size_t)nw < out_cap;
}

static bool ny_copy_function_path(char *out, size_t out_cap, const char *path) {
  return ny_copy_module_path(out, out_cap, path);
}

static bool ny_resolve_function_source_path_inner(codegen_t *cg,
                                                  const char *source, char *out,
                                                  size_t out_cap, int depth) {
  if (!cg || !source || !*source || !out || out_cap == 0)
    return false;
  if (depth > 32)
    return false;
  if (lookup_fun_exact(cg, source))
    return ny_copy_function_path(out, out_cap, source);

  const char *aliased = resolve_import_alias(cg, source);
  if (!aliased) {
    size_t source_len = strlen(source);
    aliased = ny_lookup_import_alias_indexed(cg, false, source, source_len,
                                             ny_hash_name(source, source_len));
  }
  if (aliased && *aliased && strcmp(aliased, source) != 0)
    return ny_resolve_function_source_path_inner(cg, aliased, out, out_cap,
                                                 depth + 1);

  stmt_t *source_mod = ny_module_lookup_stmt_by_name(cg, source);
  if (source_mod) {
    const char *leaf = ny_name_leaf(source);
    if (leaf && *leaf) {
      char default_fun[1280];
      int dw =
          snprintf(default_fun, sizeof(default_fun), "%s.%s", source, leaf);
      if (dw > 0 && (size_t)dw < sizeof(default_fun) &&
          lookup_fun_exact(cg, default_fun))
        return ny_copy_function_path(out, out_cap, default_fun);
    }
  }

  char resolved[1024];
  const char *target =
      ny_resolve_export_source(cg, source, resolved, sizeof(resolved), 0);
  if (target && *target && strcmp(target, source) != 0)
    return ny_resolve_function_source_path_inner(cg, target, out, out_cap,
                                                 depth + 1);
  return false;
}

static bool ny_resolve_function_source_path(codegen_t *cg, const char *source,
                                            char *out, size_t out_cap) {
  return ny_resolve_function_source_path_inner(cg, source, out, out_cap, 0);
}

bool ny_module_exported_module_path(codegen_t *cg, const char *module_name,
                                    const char *member, char *out,
                                    size_t out_cap) {
  if (!cg || !module_name || !*module_name || !member || !*member || !out ||
      out_cap == 0)
    return false;

  stmt_t *mod = ny_module_lookup_stmt_by_name(cg, module_name);
  if (mod) {
    char local[1024];
    if (ny_module_local_import_target_memo(cg, mod, member, local,
                                           sizeof(local))) {
      char resolved[1024];
      const char *target =
          ny_resolve_export_source(cg, local, resolved, sizeof(resolved), 0);
      if (ny_module_lookup_stmt_by_name(cg, target))
        return ny_copy_module_path(out, out_cap, target);
    }
  }

  char direct[1024];
  int nw = snprintf(direct, sizeof(direct), "%s.%s", module_name, member);
  if (nw <= 0 || (size_t)nw >= sizeof(direct))
    return false;
  if (ny_module_lookup_stmt_by_name(cg, direct))
    return ny_copy_module_path(out, out_cap, direct);
  return false;
}

bool ny_resolve_module_function_path(codegen_t *cg, const char *module_name,
                                     const char *member, char *out,
                                     size_t out_cap) {
  if (!cg || !module_name || !*module_name || !member || !*member || !out ||
      out_cap == 0)
    return false;

  char direct[1280];
  int dw = snprintf(direct, sizeof(direct), "%s.%s", module_name, member);
  if (dw > 0 && (size_t)dw < sizeof(direct) && lookup_fun_exact(cg, direct))
    return ny_copy_function_path(out, out_cap, direct);

  stmt_t *mod = ny_module_lookup_stmt_by_name(cg, module_name);
  if (mod) {
    char local[1024];
    if (ny_module_public_target(cg, mod, member, local, sizeof(local), NULL,
                                true, 0) &&
        ny_resolve_function_source_path(cg, local, out, out_cap))
      return true;
  }

  return false;
}

static bool ny_module_expr_spelled_path(expr_t *e, char *out, size_t out_cap) {
  if (!e || !out || out_cap == 0)
    return false;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    int n = snprintf(out, out_cap, "%s", e->as.ident.name);
    return n > 0 && (size_t)n < out_cap;
  }
  if (e->kind != NY_E_MEMBER || !e->as.member.target || !e->as.member.name)
    return false;
  char parent[1024];
  if (!ny_module_expr_spelled_path(e->as.member.target, parent, sizeof(parent)))
    return false;
  int n = snprintf(out, out_cap, "%s.%s", parent, e->as.member.name);
  return n > 0 && (size_t)n < out_cap;
}

bool ny_resolve_module_expr_path(codegen_t *cg, scope *scopes, size_t depth,
                                 expr_t *e, char *out, size_t out_cap) {
  if (!cg || !e || !out || out_cap == 0)
    return false;

  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const char *name = e->as.ident.name;
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(name);
    const char *module_name = ny_lookup_module_alias(
        cg, scopes, depth, name, name_len, e->as.ident.hash);
    if (!module_name || !*module_name)
      module_name = resolve_import_alias(cg, name);
    if (!module_name || !*module_name)
      return false;
    if (strcmp(module_name, "core") == 0)
      return ny_copy_module_path(out, out_cap, module_name);
    return ny_module_lookup_stmt_by_name(cg, module_name)
               ? ny_copy_module_path(out, out_cap, module_name)
               : false;
  }

  if (e->kind == NY_E_MEMBER && e->as.member.target && e->as.member.name) {
    /*
     * A declared fully-qualified module is its own namespace root. Resolve
     * it before walking through a same-named imported root alias (for
     * example, user `module core.math.float` versus the stdlib `core`
     * facade).
     */
    char spelled_path[1024];
    if (ny_module_expr_spelled_path(e, spelled_path, sizeof(spelled_path)) &&
        (ny_module_lookup_stmt_by_name(cg, spelled_path) ||
         ny_module_path_has_descendant(cg, spelled_path)))
      return ny_copy_module_path(out, out_cap, spelled_path);
    char parent[1024];
    if (!ny_resolve_module_expr_path(cg, scopes, depth, e->as.member.target,
                                     parent, sizeof(parent)))
      return false;
    return ny_module_exported_module_path(cg, parent, e->as.member.name, out,
                                          out_cap);
  }

  return false;
}

bool ny_module_path_exists(codegen_t *cg, const char *path) {
  return ny_module_lookup_stmt_by_name(cg, path) != NULL;
}

bool ny_module_path_has_descendant(codegen_t *cg, const char *path) {
  if (!cg || !path || !*path)
    return false;
  size_t len = strlen(path);
  ny_module_stmt_index_build(cg);
  for (size_t i = 0; i < cg->module_stmt_index_cap; ++i) {
    module_stmt_slot *slot = &cg->module_stmt_index[i];
    if (slot->occupied && slot->name && strncmp(slot->name, path, len) == 0 &&
        slot->name[len] == '.')
      return true;
  }
  return false;
}

static bool ny_copy_exported_fun_sig(codegen_t *cg, const char *alias,
                                     fun_sig *fs) {
  if (!cg || !alias || !fs)
    return false;
  if (fs) {
    fun_sig new_sig = *fs;
    new_sig.name = ny_strdup(alias);
    new_sig.module_name = fs->module_name ? ny_strdup(fs->module_name) : NULL;
    new_sig.source_file = fs->source_file ? ny_strdup(fs->source_file) : NULL;
    new_sig.link_name = fs->link_name ? ny_strdup(fs->link_name) : NULL;
    new_sig.llvm_name = fs->llvm_name ? ny_strdup(fs->llvm_name) : NULL;
    new_sig.return_type = fs->return_type ? ny_strdup(fs->return_type) : NULL;
    new_sig.abi_return_type =
        fs->abi_return_type ? ny_strdup(fs->abi_return_type) : NULL;
    new_sig.inferred_return_type =
        fs->inferred_return_type ? ny_strdup(fs->inferred_return_type) : NULL;
    new_sig.param_types = (ny_str_list){0};
    for (size_t i = 0; i < fs->param_types.len; i++) {
      const char *param_type = fs->param_types.data[i];
      vec_push(&new_sig.param_types, param_type ? ny_strdup(param_type) : NULL);
    }
    new_sig.returns_borrow =
        fs->returns_borrow ? ny_strdup(fs->returns_borrow) : NULL;
    new_sig.borrows = (ny_str_list){0};
    new_sig.consumes = (ny_str_list){0};
    new_sig.mutates = (ny_str_list){0};
    new_sig.releases = (ny_str_list){0};
    new_sig.forgets = (ny_str_list){0};
    for (size_t i = 0; i < fs->borrows.len; i++)
      vec_push(&new_sig.borrows, ny_strdup(fs->borrows.data[i]));
    for (size_t i = 0; i < fs->consumes.len; i++)
      vec_push(&new_sig.consumes, ny_strdup(fs->consumes.data[i]));
    for (size_t i = 0; i < fs->mutates.len; i++)
      vec_push(&new_sig.mutates, ny_strdup(fs->mutates.data[i]));
    for (size_t i = 0; i < fs->releases.len; i++)
      vec_push(&new_sig.releases, ny_strdup(fs->releases.data[i]));
    for (size_t i = 0; i < fs->forgets.len; i++)
      vec_push(&new_sig.forgets, ny_strdup(fs->forgets.data[i]));
    new_sig.owned = true;
    new_sig.name_hash = 0;
    new_sig.tail_cached = false;
    vec_push(&cg->fun_sigs, new_sig);
    return true;
  }
  return false;
}

static bool ny_copy_exported_global(codegen_t *cg, const char *alias,
                                    binding *gb) {
  if (!cg || !alias || !gb)
    return false;
  if (gb) {
    binding new_bind = *gb;
    new_bind.name = ny_strdup(alias);
    new_bind.owned = true;
    new_bind.name_hash = 0;
    new_bind.tail_cached = false;
    vec_push(&cg->global_vars, new_bind);
    return true;
  }
  return false;
}

static void ny_export_aliased_symbol(codegen_t *cg, stmt_t *mod,
                                     const char *target) {
  if (!cg || !mod || mod->kind != NY_S_MODULE || !target || !*target)
    return;
  const char *mod_name = mod->as.module.name;
  const char *export_name = ny_name_leaf(target);
  if (!mod_name || !*mod_name || !export_name || !*export_name)
    return;

  char alias[512];
  int aw = snprintf(alias, sizeof(alias), "%s.%s", mod_name, export_name);
  if (aw <= 0 || (size_t)aw >= sizeof(alias))
    return;
  if (lookup_fun_exact(cg, alias) || lookup_global_exact(cg, alias))
    return;

  char scoped[512];
  char local_source[1024];
  char default_fun[1280];
  const char *source = NULL;
  if (!strchr(target, '.')) {
    int sw = snprintf(scoped, sizeof(scoped), "%s.%s", mod_name, target);
    if (sw > 0 && (size_t)sw < sizeof(scoped) &&
        (lookup_fun_exact(cg, scoped) || lookup_global_exact(cg, scoped))) {
      source = scoped;
    } else {
      char child_path[1024];
      int cw =
          snprintf(child_path, sizeof(child_path), "%s.%s", mod_name, target);
      if (cw > 0 && (size_t)cw < sizeof(child_path) &&
          ny_module_lookup_stmt_by_name(cg, child_path)) {
        int dw = snprintf(default_fun, sizeof(default_fun), "%s.%s", child_path,
                          target);
        if (dw > 0 && (size_t)dw < sizeof(default_fun) &&
            (lookup_fun_exact(cg, default_fun) ||
             lookup_global_exact(cg, default_fun))) {
          source = default_fun;
        } else {
          return;
        }
      } else if (ny_module_local_import_target_memo(
                     cg, mod, target, local_source, sizeof(local_source))) {
        source = local_source;
      } else {
        source = target;
      }
    }
  } else if (strchr(target, '.')) {
    source = target;
  } else {
    source = target;
  }

  char resolved_source[1024];
  source = ny_resolve_export_source(cg, source, resolved_source,
                                    sizeof(resolved_source), 0);
  if (ny_trace_imports_enabled())
    ny_module_trace_event("EXPORTS", "%s.%s <= %s\n", mod_name, export_name,
                          source ? source : "(nil)");
  if (source && *source && strcmp(source, alias) != 0) {
    ny_push_import_alias_unique_ex(cg, false, alias, source, true);
    ny_push_import_alias_unique_ex(cg, true, alias, source, true);
  }

  fun_sig *fs = lookup_fun_exact(cg, source);
  if (!fs)
    fs = lookup_fun(cg, source, 0);
  if (fs) {
    ny_copy_exported_fun_sig(cg, alias, fs);
    return;
  }

  binding *gb = lookup_global_exact(cg, source);
  if (!gb)
    gb = lookup_global(cg, source);
  if (gb && (!source || strcmp(source, alias) == 0))
    ny_copy_exported_global(cg, alias, gb);
}

static void process_exports_inner(codegen_t *cg, const char *mod_name,
                                  stmt_t *child) {
  if (child->kind == NY_S_EXPORT) {
    if (child->as.exprt.is_internal)
      return;
    for (size_t j = 0; j < child->as.exprt.names.len; ++j) {
      stmt_t *mod = ny_module_lookup_stmt_by_name(cg, mod_name);
      ny_export_aliased_symbol(cg, mod, child->as.exprt.names.data[j]);
    }
  } else if (child->kind == NY_S_MODULE) {
    process_exports(cg, child);
  } else if (child->kind == NY_S_IF) {
    bool truthy = false;
    if (ny_eval_comptime_if(cg, child, &truthy)) {
      if (truthy) {
        process_exports_inner(cg, mod_name, child->as.iff.conseq);
      } else if (child->as.iff.alt) {
        process_exports_inner(cg, mod_name, child->as.iff.alt);
      }
    } else {
      process_exports_inner(cg, mod_name, child->as.iff.conseq);
      if (child->as.iff.alt)
        process_exports_inner(cg, mod_name, child->as.iff.alt);
    }
  } else if (child->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < child->as.block.body.len; ++i)
      process_exports_inner(cg, mod_name, child->as.block.body.data[i]);
  }
}

void process_exports(codegen_t *cg, stmt_t *s) {
  if (s->kind != NY_S_MODULE)
    return;
  const char *prev_mod = cg->current_module_name;
  cg->current_module_name = s->as.module.name;
  const char *mod_name = s->as.module.name;
  for (size_t i = 0; i < s->as.module.body.len; ++i) {
    process_exports_inner(cg, mod_name, s->as.module.body.data[i]);
  }
  cg->current_module_name = prev_mod;
}

typedef struct ny_import_usage_scan_t {
  const char *alias;
  bool used;
} ny_import_usage_scan_t;

static bool ny_import_usage_expr_pre(ny_visitor_t *visitor, expr_t *e) {
  ny_import_usage_scan_t *scan =
      visitor ? (ny_import_usage_scan_t *)visitor->ctx : NULL;
  if (!scan || scan->used || !e || !scan->alias || !*scan->alias)
    return true;

  expr_t *root = NULL;
  if (e->kind == NY_E_MEMBER) {
    root = e->as.member.target;
  } else if (e->kind == NY_E_MEMCALL) {
    root = e->as.memcall.target;
  } else {
    return true;
  }
  while (root && root->kind == NY_E_MEMBER)
    root = root->as.member.target;
  if (root && root->kind == NY_E_IDENT && root->as.ident.name &&
      strcmp(root->as.ident.name, scan->alias) == 0)
    scan->used = true;
  return true;
}

static bool ny_import_used_in_module_body(const stmt_t *module,
                                          const char *alias) {
  if (!module || module->kind != NY_S_MODULE || !alias || !*alias)
    return false;
  ny_import_usage_scan_t scan = {.alias = alias, .used = false};
  ny_visitor_t visitor = {.ctx = &scan,
                          .visit_expr_pre = ny_import_usage_expr_pre};
  for (size_t i = 0; i < module->as.module.body.len && !scan.used; ++i) {
    stmt_t *child = module->as.module.body.data[i];
    /*
     * Nested modules have independent import scopes and are linted in their
     * own pass; do not let a same-spelled alias there satisfy this import.
     */
    if (child && child->kind != NY_S_MODULE)
      ny_visit_stmt(&visitor, child);
  }
  return scan.used;
}

/*
 * Top-level script imports have no owning module: scan the program's
 * statements from the same file instead (`use M as m` at script scope with
 * `m.f()` was reported unused).
 */
static const program_t *g_lint_prog;

static bool ny_import_used_in_program(const program_t *prog,
                                      const char *alias, const char *file) {
  if (!prog || !alias || !*alias)
    return false;
  ny_import_usage_scan_t scan = {.alias = alias, .used = false};
  ny_visitor_t visitor = {.ctx = &scan,
                          .visit_expr_pre = ny_import_usage_expr_pre};
  for (size_t i = 0; i < prog->body.len && !scan.used; ++i) {
    stmt_t *child = prog->body.data[i];
    if (!child || child->kind == NY_S_MODULE || child->kind == NY_S_USE)
      continue;
    if (file && child->tok.filename && strcmp(file, child->tok.filename) != 0)
      continue;
    ny_visit_stmt(&visitor, child);
  }
  return scan.used;
}

static void ny_lint_use_stmt(codegen_t *cg, stmt_t *s,
                             const stmt_t *owner_module) {
  if (!cg || !s || s->kind != NY_S_USE)
    return;
  if (ny_is_stdlib_tok(s->tok) || ny_stmt_is_bare_core_use(s))
    return;
  if (s->as.use.import_all)
    return;
  if (s->as.use.imports.len > 0)
    return; /* selective imports bind via the export table, not aliases */

  const char *alias = s->as.use.alias;
  char *mod =
      normalize_use_module_name_at(cg, s->as.use.module, s->tok.filename);
  if (mod) {
    /*
     * Root std modules are auto-imported and export into the global
     * namespace (no alias prefix is ever resolved), so alias-usage tracking
     * cannot prove them unused.  Only lint genuinely optional modules.
     */
    for (size_t i = 0; i < sizeof(k_core_root_use_modules) /
                               sizeof(k_core_root_use_modules[0]);
         ++i) {
      if (strcmp(mod, k_core_root_use_modules[i]) == 0) {
        free(mod);
        return;
      }
    }
  }
  if (!alias && mod) {
    const char *dot = strrchr(mod, '.');
    alias = dot ? dot + 1 : mod;
  }
  if (!alias || !*alias || !mod) {
    free(mod);
    return;
  }

  /*
   * `use M as m` is represented in the module-alias table.  Member lookup
   * marks that binding, while the old lint path only inspected the export
   * alias table, which made valid forms such as `m.fn()` look unused.
   * Scoped aliases are accepted here because imports can live in modules.
   */
  bool module_alias_used = false;
  for (size_t i = 0; i < cg->aliases.len; ++i) {
    binding *b = &cg->aliases.data[i];
    if (!b->name || !b->stmt_t || !b->is_used)
      continue;
    const char *name = b->name;
    const char *dot = strrchr(name, '.');
    const char *leaf = dot ? dot + 1 : name;
    if (strcmp(leaf, alias) == 0 && strcmp((const char *)b->stmt_t, mod) == 0) {
      module_alias_used = true;
      break;
    }
  }
  if (module_alias_used) {
    free(mod);
    return;
  }

  /*
   * Native lowering and the legacy LLVM resolver share the AST, but not all
   * of their usage bookkeeping.  A qualified member expression is still an
   * unambiguous use of its module alias, so consult the owning module body
   * before emitting a diagnostic.
   */
  if (owner_module && ny_import_used_in_module_body(owner_module, alias)) {
    free(mod);
    return;
  }
  if (!owner_module &&
      ny_import_used_in_program(g_lint_prog, alias, s->tok.filename)) {
    free(mod);
    return;
  }

  /*
   * Module-member resolution materializes aliases for the exported members,
   * regardless of whether the source import had an explicit module alias.
   * Inspect those canonical bindings instead of relying on the spelling of
   * the use statement; otherwise `use M as m; m.f()` is reported unused even
   * though the resolver marked `M.f` as used.
   */
  size_t mod_len = strlen(mod);
  for (size_t i = 0; i < cg->user_import_aliases.len && !module_alias_used;
       ++i) {
    binding *b = &cg->user_import_aliases.data[i];
    const char *target = b->stmt_t ? (const char *)b->stmt_t : NULL;
    if (b->is_used && target && strncmp(target, mod, mod_len) == 0 &&
        target[mod_len] == '.')
      module_alias_used = true;
  }
  for (size_t i = 0; i < cg->global_vars.len && !module_alias_used; ++i) {
    binding *b = &cg->global_vars.data[i];
    if (b->name && b->is_used && strncmp(b->name, mod, mod_len) == 0 &&
        b->name[mod_len] == '.')
      module_alias_used = true;
  }
  if (module_alias_used) {
    free(mod);
    return;
  }
  binding *data = cg->user_import_aliases.data;
  size_t len = cg->user_import_aliases.len;
  bool used = false;
  for (size_t i = 0; i < len; ++i) {
    if (!data[i].name || strcmp(data[i].name, alias) != 0)
      continue;
    used = data[i].is_used;
    break;
  }
  if (!used && ny_diag_should_emit("unused_import", s->tok, alias)) {
    ny_diag_warning_code(s->tok, 2007, "unused import %s'%s'%s",
                         clr(NY_CLR_BOLD), s->as.use.module, clr(NY_CLR_RESET));
    ny_diag_hint("remove the 'use' statement, or reference %s'%s.%s'%s to "
                 "use the module",
                 clr(NY_CLR_BOLD), s->as.use.module, alias, clr(NY_CLR_RESET));
  }
  free(mod);
}

static void ny_lint_unused_imports_stmt(codegen_t *cg, stmt_t *s,
                                        const stmt_t *owner_module) {
  if (!s)
    return;
  switch (s->kind) {
  case NY_S_USE:
    ny_lint_use_stmt(cg, s, owner_module);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_lint_unused_imports_stmt(cg, s->as.block.body.data[i], owner_module);
    break;
  case NY_S_IF:
    ny_lint_unused_imports_stmt(cg, s->as.iff.conseq, owner_module);
    if (s->as.iff.alt)
      ny_lint_unused_imports_stmt(cg, s->as.iff.alt, owner_module);
    break;
  case NY_S_FUNC:
    if (s->as.fn.body)
      ny_lint_unused_imports_stmt(cg, s->as.fn.body, owner_module);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_lint_unused_imports_stmt(cg, s->as.module.body.data[i], s);
    break;
  default:
    break;
  }
}

void ny_lint_unused_imports(codegen_t *cg, program_t *prog) {
  if (!cg || !prog)
    return;
  g_lint_prog = prog;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_lint_unused_imports_stmt(cg, prog->body.data[i], NULL);
  g_lint_prog = NULL;
}
