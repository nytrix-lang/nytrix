#include "base/loader.h"
#include "base/util.h"
#include "priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define NY_MODULE_STMT_LOOKUP_CACHE_SLOTS 8192u
#define NY_MODULE_STMT_LOOKUP_KEY_MAX 192u
#define NY_MODULE_PUBLIC_TARGET_CACHE_SLOTS 4096u
#define NY_MODULE_PUBLIC_TARGET_KEY_MAX 128u
#define NY_MODULE_PUBLIC_TARGET_PROFILE_MAX 32u
#define NY_MODULE_PUBLIC_TARGET_VALUE_MAX 1024u
#define NY_MODULE_LOOKUP_CACHE_PROBES 8u

typedef struct ny_module_stmt_lookup_entry_t {
  const codegen_t *cg;
  const program_t *prog;
  const void *extra_data;
  size_t extra_len;
  uint64_t hash;
  uint16_t len;
  uint8_t state; /* 0=empty, 1=cached miss, 2=cached hit */
  char key[NY_MODULE_STMT_LOOKUP_KEY_MAX];
  stmt_t *value;
} ny_module_stmt_lookup_entry_t;

typedef struct ny_module_stmt_lookup_cache_t {
  ny_module_stmt_lookup_entry_t entries[NY_MODULE_STMT_LOOKUP_CACHE_SLOTS];
} ny_module_stmt_lookup_cache_t;

typedef struct ny_module_public_target_entry_t {
  const codegen_t *cg;
  const stmt_t *mod;
  const void *body_data;
  size_t body_len;
  uint64_t hash;
  uint16_t target_len;
  uint8_t profile_len;
  uint8_t include_child_uses;
  uint8_t state; /* 0=empty, 1=cached miss, 2=cached hit */
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
                                                  size_t idx,
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
    if (alias_len == name_len && strncmp(al->name, name, name_len) == 0)
      return (const char *)al->stmt_t;
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
        if (alias_len == name_len && strncmp(al->name, name, name_len) == 0)
          return (const char *)al->stmt_t;
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
  if (found >= 0 && (size_t)found < len && data && data[found].name)
    return (const char *)data[found].stmt_t;

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
        data[i].name[alias_len] == '\0')
      return (const char *)data[i].stmt_t;
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
  const char *env = getenv("NYTRIX_TRACE_IMPORTS");
  return env && *env && strcmp(env, "0") != 0 && strcmp(env, "false") != 0;
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
  if (!cg || cg->module_stmt_index)
    return;
  size_t n = ny_count_top_modules(cg->prog);
  for (size_t p = 0; p < cg->extra_progs.len; ++p)
    n += ny_count_top_modules(cg->extra_progs.data[p]);
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

static bool ny_module_stmt_cacheable_name(const char *name, size_t *len_out) {
  if (!name || !*name)
    return false;
  size_t len = strlen(name);
  if (len == 0 || len >= NY_MODULE_STMT_LOOKUP_KEY_MAX ||
      len > UINT16_MAX)
    return false;
  if (len_out)
    *len_out = len;
  return true;
}

static ny_module_stmt_lookup_cache_t *
ny_module_stmt_lookup_cache(codegen_t *cg) {
  if (!cg)
    return NULL;
  if (!cg->module_stmt_lookup_cache) {
    cg->module_stmt_lookup_cache =
        calloc(1, sizeof(ny_module_stmt_lookup_cache_t));
  }
  return (ny_module_stmt_lookup_cache_t *)cg->module_stmt_lookup_cache;
}

static int ny_module_stmt_cache_get(codegen_t *cg, const char *name,
                                    size_t name_len, uint64_t hash,
                                    stmt_t **out) {
  if (!cg || !name || name_len == 0 || name_len >= NY_MODULE_STMT_LOOKUP_KEY_MAX)
    return -1;
  ny_module_stmt_lookup_cache_t *cache = ny_module_stmt_lookup_cache(cg);
  if (!cache)
    return -1;
  size_t base = hash & (NY_MODULE_STMT_LOOKUP_CACHE_SLOTS - 1u);
  for (size_t probe = 0; probe < NY_MODULE_LOOKUP_CACHE_PROBES; ++probe) {
    ny_module_stmt_lookup_entry_t *e = &cache->entries[
        (base + probe) & (NY_MODULE_STMT_LOOKUP_CACHE_SLOTS - 1u)];
    if (!e->state)
      continue;
    if (e->cg != cg || e->prog != cg->prog ||
        e->extra_data != cg->extra_progs.data ||
        e->extra_len != cg->extra_progs.len || e->hash != hash ||
        e->len != (uint16_t)name_len ||
        memcmp(e->key, name, name_len) != 0 || e->key[name_len] != '\0')
      continue;
    if (e->state == 2u) {
      *out = e->value;
      return 1;
    }
    return 0;
  }
  return -1;
}

static void ny_module_stmt_cache_put(codegen_t *cg, const char *name,
                                     size_t name_len, uint64_t hash,
                                     stmt_t *value) {
  if (!cg || !name || name_len == 0 || name_len >= NY_MODULE_STMT_LOOKUP_KEY_MAX)
    return;
  ny_module_stmt_lookup_cache_t *cache = ny_module_stmt_lookup_cache(cg);
  if (!cache)
    return;
  size_t base = hash & (NY_MODULE_STMT_LOOKUP_CACHE_SLOTS - 1u);
  ny_module_stmt_lookup_entry_t *e = &cache->entries[base];
  for (size_t probe = 0; probe < NY_MODULE_LOOKUP_CACHE_PROBES; ++probe) {
    ny_module_stmt_lookup_entry_t *cur = &cache->entries[
        (base + probe) & (NY_MODULE_STMT_LOOKUP_CACHE_SLOTS - 1u)];
    if (!cur->state) {
      e = cur;
      break;
    }
    if (cur->cg == cg && cur->prog == cg->prog &&
        cur->extra_data == cg->extra_progs.data &&
        cur->extra_len == cg->extra_progs.len && cur->hash == hash &&
        cur->len == (uint16_t)name_len &&
        memcmp(cur->key, name, name_len) == 0 &&
        cur->key[name_len] == '\0') {
      e = cur;
      break;
    }
  }
  e->cg = cg;
  e->prog = cg->prog;
  e->extra_data = cg->extra_progs.data;
  e->extra_len = cg->extra_progs.len;
  e->hash = hash;
  e->len = (uint16_t)name_len;
  memcpy(e->key, name, name_len);
  e->key[name_len] = '\0';
  e->value = value;
  e->state = value ? 2u : 1u;
}

static stmt_t *find_module_stmt_any(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return NULL;
  ny_module_stmt_index_build(cg);
  size_t name_len = 0;
  bool cacheable = ny_module_stmt_cacheable_name(name, &name_len);
  uint64_t hash = cacheable ? ny_hash_name(name, name_len) : 0;
  if (cacheable) {
    stmt_t *cached = NULL;
    int cache_hit = ny_module_stmt_cache_get(cg, name, name_len, hash, &cached);
    if (cache_hit == 1)
      return cached;
    if (cache_hit == 0)
      return NULL;
  }
  if (cg->module_stmt_index && cg->module_stmt_index_cap) {
    if (!cacheable) {
      name_len = strlen(name);
      hash = ny_hash_name(name, name_len);
    }
    size_t mask = cg->module_stmt_index_cap - 1u;
    size_t pos = hash & mask;
    for (;;) {
      module_stmt_slot *slot = &cg->module_stmt_index[pos];
      if (!slot->occupied) {
        if (cacheable)
          ny_module_stmt_cache_put(cg, name, name_len, hash, NULL);
        return NULL;
      }
      if (slot->hash == hash && slot->name &&
          memcmp(slot->name, name, name_len) == 0 &&
          slot->name[name_len] == '\0') {
        if (cacheable)
          ny_module_stmt_cache_put(cg, name, name_len, hash, slot->stmt);
        return slot->stmt;
      }
      pos = (pos + 1u) & mask;
    }
  }
  if (cg->prog) {
    for (size_t i = 0; i < cg->prog->body.len; ++i) {
      stmt_t *m = find_module_stmt(cg->prog->body.data[i], name);
      if (m) {
        if (cacheable)
          ny_module_stmt_cache_put(cg, name, name_len, hash, m);
        return m;
      }
    }
  }
  for (size_t p = 0; p < cg->extra_progs.len; ++p) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; ++i) {
      stmt_t *m = find_module_stmt(prog->body.data[i], name);
      if (m) {
        if (cacheable)
          ny_module_stmt_cache_put(cg, name, name_len, hash, m);
        return m;
      }
    }
  }
  if (cacheable)
    ny_module_stmt_cache_put(cg, name, name_len, hash, NULL);
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

static void ny_str_list_push_unique_owned(str_list *items, char *value) {
  if (!items || !value)
    return;
  if (ny_str_list_has(items, value)) {
    free(value);
    return;
  }
  vec_push(items, value);
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
        size_t len = strlen(mod_name) + 1 + strlen(name) + 1;
        full = malloc(len);
        snprintf(full, len, "%s.%s", mod_name, name);
      }
      ny_str_list_push_unique_owned(exports, full);
    }
  }
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
  if (!cg || !cg->current_module_name)
    return true;
  return strncmp(cg->current_module_name, "std.", 4) != 0 &&
         strncmp(cg->current_module_name, "lib.", 4) != 0;
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

static char *normalize_use_module_name(codegen_t *cg, const char *raw) {
  return normalize_use_module_name_at(cg, raw, NULL);
}

static void ny_add_import_aliases(codegen_t *cg, bool user_use,
                                  const char *alias, const char *full_name);
static void ny_add_import_aliases_from_full(codegen_t *cg, bool user_use,
                                            const char *full_name);
static void ny_add_import_aliases_from_full_scoped(codegen_t *cg, bool user_use,
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
static const char *ny_resolve_export_source(codegen_t *cg, const char *source,
                                            char *out, size_t out_cap,
                                            int depth);

static void ny_collect_module_use_surface(codegen_t *cg, stmt_t *s,
                                          str_list *exports,
                                          const char *profile, int depth) {
  if (!cg || !s || !exports || depth > 8)
    return;
  if (s->kind == NY_S_USE) {
    if (ny_stmt_is_bare_std_use(s))
      return;
    bool bare_use =
        !s->as.use.import_all && s->as.use.imports.len == 0 && !s->as.use.alias;
    if (!s->as.use.import_all && !bare_use)
      return;
    char *mod =
        normalize_use_module_name_at(cg, s->as.use.module, s->tok.filename);
    if (!mod)
      return;
    stmt_t *used = find_module_stmt_any(cg, mod);
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
    if (!s || s->kind != NY_S_USE || ny_stmt_is_bare_std_use(s))
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
    if (!s || s->kind != NY_S_USE || ny_stmt_is_bare_std_use(s))
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

static void ny_add_scoped_module_alias(codegen_t *cg, const char *alias,
                                       const char *target) {
  if (!cg || !alias || !*alias || !target || !*target)
    return;
  if (cg->current_module_name && *cg->current_module_name) {
    char scoped[512];
    int nw = snprintf(scoped, sizeof(scoped), "%s.%s", cg->current_module_name,
                      alias);
    if (nw > 0 && (size_t)nw < sizeof(scoped)) {
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
    int nw = snprintf(scoped, sizeof(scoped), "%s.%s", cg->current_module_name,
                      alias);
    if (nw > 0 && (size_t)nw < sizeof(scoped)) {
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
  if (!find_module_stmt_any(cg, full_name))
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
      int nw = strchr(name, '.')
                   ? snprintf(child_path, sizeof(child_path), "%s", name)
                   : snprintf(child_path, sizeof(child_path), "%s.%s", mod_name,
                              name);
      if (nw <= 0 || (size_t)nw >= sizeof(child_path))
        continue;
      if (has_explicit_child_uses &&
          ny_module_has_explicit_child_use(cg, mod, child_path, name))
        continue;
      if (!find_module_stmt_any(cg, child_path))
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
      int nw = strchr(name, '.')
                   ? snprintf(path, sizeof(path), "%s", name)
                   : snprintf(path, sizeof(path), "%s.%s", mod_name, name);
      if (nw <= 0 || (size_t)nw >= sizeof(path))
        continue;
      if (has_explicit_child_uses &&
          ny_module_has_explicit_child_use(cg, mod, path, name))
        continue;
      stmt_t *child_mod = find_module_stmt_any(cg, path);
      if (!child_mod)
        continue;
      ny_str_list_push_unique_owned(exports, ny_strdup(path));
      // Eager package imports expose the next public layer. Deeper exported
      // symbols are resolved lazily by name so root package imports stay cheap.
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

static const ny_std_root_alias_t k_std_root_aliases[] = {
    {"std", "std"},          {"core", "std.core"},    {"math", "std.math"},
    {"os", "std.os"},        {"str", "std.core.str"}, {"nt", "std.math.nt"},
    {"bin", "std.math.bin"},
};

static const char *const k_std_bare_surface_modules[] = {"std.core",
                                                         "std.os.prim"};

static const char *const k_std_root_use_modules[] = {
    "std.core",     "std.os.prim", "std.math",     "std.math.nt",
    "std.math.bin", "std.os",      "std.core.str",
};

static bool ny_bare_use_keeps_shallow_surface(const char *mod) {
  if (!mod)
    return false;
  return strcmp(mod, "std") == 0 || strcmp(mod, "std.math") == 0 ||
         strcmp(mod, "std.math.crypto") == 0;
}

static void ny_process_bare_std_use_imports(codegen_t *cg, bool user_use) {
  for (size_t i = 0;
       i < sizeof(k_std_root_aliases) / sizeof(k_std_root_aliases[0]); ++i)
    ny_add_import_aliases(cg, user_use, k_std_root_aliases[i].alias,
                          k_std_root_aliases[i].module);

  for (size_t mi = 0; mi < sizeof(k_std_bare_surface_modules) /
                               sizeof(k_std_bare_surface_modules[0]);
       ++mi) {
    const char *mod = k_std_bare_surface_modules[mi];
    str_list exports = {0};
    stmt_t *mod_stmt = find_module_stmt_any(cg, mod);
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
      fprintf(stderr, "[imports] module-export alias %s -> %s\n",
              mod->as.module.name ? mod->as.module.name : "(module)",
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
    int nw = snprintf(scoped, sizeof(scoped), "%s.%s", cg->current_module_name,
                      alias);
    if (nw > 0 && (size_t)nw < sizeof(scoped)) {
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

static void ny_add_import_aliases_from_full_scoped(codegen_t *cg, bool user_use,
                                                   const char *full_name) {
  if (cg && cg->current_module_name && *cg->current_module_name) {
    const char *leaf = ny_name_leaf(full_name);
    if (leaf && *leaf)
      ny_add_import_aliases_scoped(cg, false, leaf, full_name);
    return;
  }
  ny_add_import_aliases_from_full(cg, user_use, full_name);
}

static void ny_add_import_aliases_from_full_scoped_weak(codegen_t *cg,
                                                        bool user_use,
                                                        const char *full_name) {
  if (cg && cg->current_module_name && *cg->current_module_name) {
    const char *leaf = ny_name_leaf(full_name);
    if (leaf && *leaf) {
      char scoped[512];
      int nw = snprintf(scoped, sizeof(scoped), "%s.%s",
                        cg->current_module_name, leaf);
      if (nw > 0 && (size_t)nw < sizeof(scoped))
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
  char *mod = normalize_module_name("std.core");
  if (!mod)
    return;

  str_list exports = {0};
  stmt_t *mod_stmt = find_module_stmt_any(cg, mod);
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
    if (ny_stmt_is_bare_std_use(s)) {
      ny_process_bare_std_use_imports(cg, !ny_is_stdlib_tok(s->tok));
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
      stmt_t *mod_stmt = find_module_stmt_any(cg, mod);
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
      stmt_t *used_mod = find_module_stmt_any(cg, mod);
      for (size_t i = 0; i < s->as.use.imports.len; ++i) {
        use_item_t *item = &s->as.use.imports.data[i];
        if (!item->name)
          continue;
        snprintf(full, full_cap, "%s.%s", mod, item->name);
        const char *alias = item->alias ? item->alias : item->name;
        const char *import_target = full;
        char local_target[1024];
        char resolved_target[1024];
        if (used_mod &&
            ny_module_local_import_target(cg, used_mod, item->name,
                                          local_target, sizeof(local_target), 0)) {
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
      stmt_t *mod_stmt = find_module_stmt_any(cg, mod);
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
        fprintf(
            stderr,
            "[imports] use %s%s%s%s mod_stmt=%s export_list=%d exports=%zu\n",
            mod, s->as.use.profile ? ":" : "",
            s->as.use.profile ? s->as.use.profile : "",
            s->as.use.import_all ? " *" : "", mod_stmt ? "yes" : "no",
            mod_stmt && module_has_export_list(mod_stmt) ? 1 : 0, exports.len);
      }
      if (!mod_stmt || exports.len == 0) {
        if (ny_trace_imports_enabled())
          fprintf(stderr, "[imports] fallback prefix %s\n", mod);
        add_imports_from_prefix(cg, user_use, mod);
      } else {
        const char *module_leaf = ny_name_leaf(mod);
        for (size_t i = 0; i < exports.len; ++i) {
          if (ny_trace_imports_enabled())
            fprintf(stderr, "[imports] alias %s\n", exports.data[i]);
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
    if (ny_stmt_is_bare_std_use(s)) {
      for (size_t i = 0;
           i < sizeof(k_std_root_aliases) / sizeof(k_std_root_aliases[0]);
           ++i) {
        const char *alias = k_std_root_aliases[i].alias;
        const char *target = k_std_root_aliases[i].module;
        char scoped_alias[512];
        if (cg && cg->current_module_name && *cg->current_module_name) {
          int nw = snprintf(scoped_alias, sizeof(scoped_alias), "%s.%s",
                            cg->current_module_name, alias);
          if (nw > 0 && (size_t)nw < sizeof(scoped_alias))
            alias = scoped_alias;
        }
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
    if (cg && cg->current_module_name && *cg->current_module_name) {
      int nw = snprintf(scoped_alias, sizeof(scoped_alias), "%s.%s",
                        cg->current_module_name, alias);
      if (nw > 0 && (size_t)nw < sizeof(scoped_alias))
        alias = scoped_alias;
    }
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

  return ny_lookup_module_alias_indexed(cg, name, name_len, name_hash);
}

static void collect_use_modules_inner(codegen_t *cg, stmt_t *s,
                                      bool in_std_mod) {
  if (s->kind == NY_S_USE) {
    bool has_alias = !s->as.use.import_all && s->as.use.alias;
    const char *mod = s->as.use.module;
    if (mod && *mod) {
      if (ny_stmt_is_bare_std_use(s)) {
        for (size_t i = 0; i < sizeof(k_std_root_use_modules) /
                                   sizeof(k_std_root_use_modules[0]);
             ++i)
          ny_push_use_module_unique(cg, false, k_std_root_use_modules[i]);
        if (!in_std_mod && !has_alias) {
          for (size_t i = 0; i < sizeof(k_std_root_use_modules) /
                                     sizeof(k_std_root_use_modules[0]);
               ++i)
            ny_push_use_module_unique(cg, true, k_std_root_use_modules[i]);
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
    const char *mname = s->as.module.name;
    bool is_std = mname && (strncmp(mname, "std.", 4) == 0 ||
                            strncmp(mname, "lib.", 4) == 0);
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

  stmt_t *mod = find_module_stmt_any(cg, mod_name);
  if (!mod) {
    if (mod_name != stack_mod)
      free(mod_name);
    return NULL;
  }

  char local_target[1024];
  if (!ny_module_local_import_target(cg, mod, dot + 1, local_target,
                                     sizeof(local_target), 0)) {
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
      !find_module_stmt_any(cg, target))
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

const char *ny_resolve_used_module_export_alias(codegen_t *cg,
                                                const char *name) {
  if (!cg || !name || !*name)
    return NULL;

  if (strchr(name, '.'))
    return ny_resolve_dotted_module_export_alias(cg, name);

  if (cg->current_module_name &&
      (strncmp(cg->current_module_name, "std.", 4) == 0 ||
       strncmp(cg->current_module_name, "lib.", 4) == 0))
    return NULL;

  bool user_only = true;
  char **mods = user_only ? cg->user_use_modules.data : cg->use_modules.data;
  size_t mods_len = user_only ? cg->user_use_modules.len : cg->use_modules.len;
  if (!mods || mods_len == 0)
    return NULL;

  for (size_t i = mods_len; i > 0; --i) {
    const char *mod_name = mods[i - 1];
    if (!mod_name || !*mod_name)
      continue;
    stmt_t *mod = find_module_stmt_any(cg, mod_name);
    if (!mod)
      continue;

    char local_target[1024];
    if (!ny_module_local_import_target(cg, mod, name, local_target,
                                       sizeof(local_target), 0))
      continue;

    char resolved_target[1024];
    const char *target = ny_resolve_export_source(
        cg, local_target, resolved_target, sizeof(resolved_target), 0);
    if (!target || !*target)
      continue;
    if (!lookup_fun_exact(cg, target) && !lookup_global_exact(cg, target) &&
        !find_module_stmt_any(cg, target))
      continue;

    ny_push_import_alias_unique_ex(cg, false, name, target, false);
    if (user_only)
      ny_push_import_alias_unique_ex(cg, true, name, target, false);

    size_t name_len = strlen(name);
    uint64_t name_hash = ny_hash_name(name, name_len);
    return ny_lookup_import_alias_indexed(cg, user_only, name, name_len,
                                          name_hash);
  }

  return NULL;
}

bool ny_is_module_active(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return false;
  if (cg->current_module_name && strcmp(cg->current_module_name, name) == 0)
    return true;
  for (size_t i = 0; i < cg->use_modules.len; ++i) {
    if (cg->use_modules.data[i] && strcmp(cg->use_modules.data[i], name) == 0)
      return true;
  }
  for (size_t i = 0; i < cg->user_use_modules.len; ++i) {
    if (cg->user_use_modules.data[i] &&
        strcmp(cg->user_use_modules.data[i], name) == 0)
      return true;
  }
  return cg->parent ? ny_is_module_active(cg->parent, name) : false;
}

static bool ny_module_local_import_target_inner(codegen_t *cg, stmt_t *s,
                                                const char *target, char *out,
                                                size_t out_cap, int depth) {
  if (!cg || !s || !target || !*target || !out || out_cap == 0 || depth > 8)
    return false;
  if (s->kind == NY_S_USE) {
    char *mod =
        normalize_use_module_name_at(cg, s->as.use.module, s->tok.filename);
    if (!mod)
      return false;
    bool alias_only =
        !s->as.use.import_all && s->as.use.imports.len == 0 && s->as.use.alias;
    if (alias_only && s->as.use.alias && strcmp(s->as.use.alias, target) == 0) {
      int nw = snprintf(out, out_cap, "%s", mod);
      free(mod);
      return nw > 0 && (size_t)nw < out_cap;
    }
    if (s->as.use.imports.len > 0) {
      for (size_t i = 0; i < s->as.use.imports.len; ++i) {
        use_item_t *item = &s->as.use.imports.data[i];
        if (!item->name)
          continue;
        const char *local = item->alias ? item->alias : item->name;
        if (!local || strcmp(local, target) != 0)
          continue;
        int nw = snprintf(out, out_cap, "%s.%s", mod, item->name);
        free(mod);
        return nw > 0 && (size_t)nw < out_cap;
      }
      free(mod);
      return false;
    }

    bool bare_use =
        !s->as.use.import_all && s->as.use.imports.len == 0 && !s->as.use.alias;
    if (s->as.use.import_all || bare_use) {
      const char *leaf = ny_name_leaf(mod);
      if (bare_use && leaf && strcmp(leaf, target) == 0 &&
          find_module_stmt_any(cg, mod)) {
        int nw = snprintf(out, out_cap, "%s", mod);
        free(mod);
        return nw > 0 && (size_t)nw < out_cap;
      }
      stmt_t *used = find_module_stmt_any(cg, mod);
      if (used && ny_module_public_target(cg, used, target, out, out_cap,
                                          s->as.use.profile, true, depth + 1)) {
        free(mod);
        return true;
      }
      if (!used) {
        int nw = snprintf(out, out_cap, "%s.%s", mod, target);
        free(mod);
        return nw > 0 && (size_t)nw < out_cap;
      }
    }
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
    return s->as.iff.alt && ny_module_local_import_target_inner(
                                cg, s->as.iff.alt, target, out, out_cap,
                                depth + 1);
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

static bool ny_module_exported_child_target(codegen_t *cg, stmt_t *mod,
                                            const char *target, char *out,
                                            size_t out_cap, int depth) {
  if (!cg || !mod || mod->kind != NY_S_MODULE || !target || !*target || !out ||
      out_cap == 0 || depth > 8)
    return false;
  const char *mod_name = mod->as.module.name;
  if (!mod_name || !*mod_name)
    return false;
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
      int cw = strchr(name, '.')
                   ? snprintf(child_path, sizeof(child_path), "%s", name)
                   : snprintf(child_path, sizeof(child_path), "%s.%s", mod_name,
                              name);
      if (cw <= 0 || (size_t)cw >= sizeof(child_path))
        continue;
      stmt_t *child_mod = find_module_stmt_any(cg, child_path);
      if (!child_mod)
        continue;
      const char *child_leaf = ny_name_leaf(child_path);
      if (child_leaf && strcmp(child_leaf, target) == 0) {
        int nw = snprintf(out, out_cap, "%s", child_path);
        return nw > 0 && (size_t)nw < out_cap;
      }
      if (ny_module_public_target(cg, child_mod, target, out, out_cap, NULL,
                                  true, depth + 1))
        return true;
    }
  }
  return false;
}

static bool ny_module_public_target_cacheable(const char *target,
                                              const char *profile,
                                              size_t *target_len_out,
                                              size_t *profile_len_out) {
  if (!target || !*target)
    return false;
  size_t target_len = strlen(target);
  size_t profile_len = profile ? strlen(profile) : 0;
  if (target_len == 0 || target_len >= NY_MODULE_PUBLIC_TARGET_KEY_MAX ||
      target_len > UINT16_MAX ||
      profile_len >= NY_MODULE_PUBLIC_TARGET_PROFILE_MAX)
    return false;
  if (target_len_out)
    *target_len_out = target_len;
  if (profile_len_out)
    *profile_len_out = profile_len;
  return true;
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

static ny_module_public_target_cache_t *
ny_module_public_target_cache(codegen_t *cg) {
  if (!cg)
    return NULL;
  if (!cg->module_public_target_cache)
    cg->module_public_target_cache =
        calloc(1, sizeof(ny_module_public_target_cache_t));
  return (ny_module_public_target_cache_t *)cg->module_public_target_cache;
}

static int ny_module_public_target_cache_get(
    codegen_t *cg, stmt_t *mod, const char *target, size_t target_len,
    const char *profile, size_t profile_len, bool include_child_uses,
    uint64_t hash, char *out, size_t out_cap) {
  if (!cg || !mod || !target || !out || out_cap == 0)
    return -1;
  ny_module_public_target_cache_t *cache = ny_module_public_target_cache(cg);
  if (!cache)
    return -1;
  size_t base = hash & (NY_MODULE_PUBLIC_TARGET_CACHE_SLOTS - 1u);
  for (size_t probe = 0; probe < NY_MODULE_LOOKUP_CACHE_PROBES; ++probe) {
    ny_module_public_target_entry_t *e = &cache->entries[
        (base + probe) & (NY_MODULE_PUBLIC_TARGET_CACHE_SLOTS - 1u)];
    if (!e->state)
      continue;
    if (e->cg != cg || e->mod != mod ||
        e->body_data != mod->as.module.body.data ||
        e->body_len != mod->as.module.body.len || e->hash != hash ||
        e->target_len != (uint16_t)target_len ||
        e->profile_len != (uint8_t)profile_len ||
        e->include_child_uses != (include_child_uses ? 1u : 0u) ||
        memcmp(e->target, target, target_len) != 0 ||
        e->target[target_len] != '\0')
      continue;
    if (profile_len > 0) {
      if (!profile || memcmp(e->profile, profile, profile_len) != 0 ||
          e->profile[profile_len] != '\0')
        continue;
    } else if (e->profile[0] != '\0') {
      continue;
    }
    if (e->state == 2u) {
      if ((size_t)e->value_len >= out_cap)
        return 0;
      memcpy(out, e->value, (size_t)e->value_len + 1u);
      return 1;
    }
    return 0;
  }
  return -1;
}

static void ny_module_public_target_cache_put(
    codegen_t *cg, stmt_t *mod, const char *target, size_t target_len,
    const char *profile, size_t profile_len, bool include_child_uses,
    uint64_t hash, const char *value) {
  if (!cg || !mod || !target)
    return;
  ny_module_public_target_cache_t *cache = ny_module_public_target_cache(cg);
  if (!cache)
    return;
  size_t base = hash & (NY_MODULE_PUBLIC_TARGET_CACHE_SLOTS - 1u);
  ny_module_public_target_entry_t *e = &cache->entries[base];
  for (size_t probe = 0; probe < NY_MODULE_LOOKUP_CACHE_PROBES; ++probe) {
    ny_module_public_target_entry_t *cur = &cache->entries[
        (base + probe) & (NY_MODULE_PUBLIC_TARGET_CACHE_SLOTS - 1u)];
    if (!cur->state) {
      e = cur;
      break;
    }
    if (cur->cg == cg && cur->mod == mod &&
        cur->body_data == mod->as.module.body.data &&
        cur->body_len == mod->as.module.body.len && cur->hash == hash &&
        cur->target_len == (uint16_t)target_len &&
        cur->profile_len == (uint8_t)profile_len &&
        cur->include_child_uses == (include_child_uses ? 1u : 0u) &&
        memcmp(cur->target, target, target_len) == 0 &&
        cur->target[target_len] == '\0') {
      bool profile_match =
          profile_len > 0
              ? profile && memcmp(cur->profile, profile, profile_len) == 0 &&
                    cur->profile[profile_len] == '\0'
              : cur->profile[0] == '\0';
      if (profile_match) {
        e = cur;
        break;
      }
    }
  }
  e->cg = cg;
  e->mod = mod;
  e->body_data = mod->as.module.body.data;
  e->body_len = mod->as.module.body.len;
  e->hash = hash;
  e->target_len = (uint16_t)target_len;
  e->profile_len = (uint8_t)profile_len;
  e->include_child_uses = include_child_uses ? 1u : 0u;
  memcpy(e->target, target, target_len);
  e->target[target_len] = '\0';
  if (profile_len > 0 && profile) {
    memcpy(e->profile, profile, profile_len);
    e->profile[profile_len] = '\0';
  } else {
    e->profile[0] = '\0';
  }
  if (value && *value) {
    size_t value_len = strlen(value);
    if (value_len >= NY_MODULE_PUBLIC_TARGET_VALUE_MAX)
      value_len = NY_MODULE_PUBLIC_TARGET_VALUE_MAX - 1u;
    memcpy(e->value, value, value_len);
    e->value[value_len] = '\0';
    e->value_len = (uint16_t)value_len;
    e->state = 2u;
  } else {
    e->value[0] = '\0';
    e->value_len = 0;
    e->state = 1u;
  }
}

static bool ny_module_public_target_uncached(codegen_t *cg, stmt_t *mod,
                                             const char *target, char *out,
                                             size_t out_cap,
                                             const char *profile,
                                             bool include_child_uses,
                                             int depth) {
  if (!cg || !mod || mod->kind != NY_S_MODULE || !target || !*target || !out ||
      out_cap == 0 || depth > 8)
    return false;
  const char *mod_name = mod->as.module.name;
  if (!mod_name || !*mod_name)
    return false;

  bool has_export_list = module_has_export_list(mod);
  if (has_export_list) {
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
        char full[1024];
        int nw = strchr(name, '.')
                     ? snprintf(full, sizeof(full), "%s", name)
                     : snprintf(full, sizeof(full), "%s.%s", mod_name, name);
        if (nw <= 0 || (size_t)nw >= sizeof(full))
          continue;
        const char *leaf = ny_name_leaf(full);
        if (leaf && strcmp(leaf, target) == 0) {
          if (lookup_fun_exact(cg, full) || lookup_global_exact(cg, full) ||
              find_module_stmt_any(cg, full)) {
            int cw = snprintf(out, out_cap, "%s", full);
            return cw > 0 && (size_t)cw < out_cap;
          }
          char import_target[1024];
          if (ny_module_local_import_target(cg, mod, target, import_target,
                                            sizeof(import_target), depth + 1)) {
            int cw = snprintf(out, out_cap, "%s", import_target);
            return cw > 0 && (size_t)cw < out_cap;
          }
        }
        if (!include_child_uses)
          continue;
        stmt_t *child_mod = find_module_stmt_any(cg, full);
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
        int nw = snprintf(out, out_cap, "%s", q);
        return nw > 0 && (size_t)nw < out_cap;
      }
      if (child->kind == NY_S_VAR) {
        for (size_t j = 0; j < child->as.var.names.len; ++j) {
          const char *name = child->as.var.names.data[j];
          if (name && strcmp(name, target) == 0 &&
              !ny_module_name_is_internal(mod, name)) {
            const char *q = codegen_qname(cg, name, mod_name);
            int nw = snprintf(out, out_cap, "%s", q);
            return nw > 0 && (size_t)nw < out_cap;
          }
        }
      }
    }
  }

  return false;
}

static bool ny_module_public_target(codegen_t *cg, stmt_t *mod,
                                    const char *target, char *out,
                                    size_t out_cap, const char *profile,
                                    bool include_child_uses, int depth) {
  if (!cg || !mod || mod->kind != NY_S_MODULE || !target || !*target || !out ||
      out_cap == 0 || depth > 8)
    return false;

  size_t target_len = 0;
  size_t profile_len = 0;
  bool cacheable = ny_module_public_target_cacheable(
      target, profile, &target_len, &profile_len);
  uint64_t hash =
      cacheable ? ny_module_public_target_hash(
                      mod, target, target_len, profile, profile_len,
                      include_child_uses)
                : 0;
  if (cacheable) {
    int hit = ny_module_public_target_cache_get(
        cg, mod, target, target_len, profile, profile_len, include_child_uses,
        hash, out, out_cap);
    if (hit == 1)
      return true;
    if (hit == 0)
      return false;
  }

  char tmp[NY_MODULE_PUBLIC_TARGET_VALUE_MAX];
  bool ok = ny_module_public_target_uncached(
      cg, mod, target, tmp, sizeof(tmp), profile, include_child_uses, depth);
  if (cacheable)
    ny_module_public_target_cache_put(cg, mod, target, target_len, profile,
                                      profile_len, include_child_uses, hash,
                                      ok ? tmp : NULL);
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
  stmt_t *mod = find_module_stmt_any(cg, mod_name);
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

  stmt_t *source_mod = find_module_stmt_any(cg, source);
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

static bool ny_module_exported_module_path(codegen_t *cg,
                                           const char *module_name,
                                           const char *member, char *out,
                                           size_t out_cap) {
  if (!cg || !module_name || !*module_name || !member || !*member || !out ||
      out_cap == 0)
    return false;

  stmt_t *mod = find_module_stmt_any(cg, module_name);
  if (mod) {
    char local[1024];
    if (ny_module_local_import_target(cg, mod, member, local, sizeof(local), 0)) {
      char resolved[1024];
      const char *target =
          ny_resolve_export_source(cg, local, resolved, sizeof(resolved), 0);
      if (find_module_stmt_any(cg, target))
        return ny_copy_module_path(out, out_cap, target);
    }
  }

  char direct[1024];
  int nw = snprintf(direct, sizeof(direct), "%s.%s", module_name, member);
  if (nw <= 0 || (size_t)nw >= sizeof(direct))
    return false;
  if (find_module_stmt_any(cg, direct))
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

  stmt_t *mod = find_module_stmt_any(cg, module_name);
  if (mod) {
    char local[1024];
    if (ny_module_public_target(cg, mod, member, local, sizeof(local), NULL,
                                true, 0) &&
        ny_resolve_function_source_path(cg, local, out, out_cap))
      return true;
  }

  return false;
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
      return false;
    if (strcmp(module_name, "std") == 0)
      return ny_copy_module_path(out, out_cap, module_name);
    return find_module_stmt_any(cg, module_name)
               ? ny_copy_module_path(out, out_cap, module_name)
               : false;
  }

  if (e->kind == NY_E_MEMBER && e->as.member.target && e->as.member.name) {
    char parent[1024];
    if (!ny_resolve_module_expr_path(cg, scopes, depth, e->as.member.target,
                                     parent, sizeof(parent)))
      return false;
    return ny_module_exported_module_path(cg, parent, e->as.member.name, out,
                                          out_cap);
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
    new_sig.module_name =
        fs->module_name ? ny_strdup(fs->module_name) : NULL;
    new_sig.source_file =
        fs->source_file ? ny_strdup(fs->source_file) : NULL;
    new_sig.link_name = fs->link_name ? ny_strdup(fs->link_name) : NULL;
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
          find_module_stmt_any(cg, child_path)) {
        int dw = snprintf(default_fun, sizeof(default_fun), "%s.%s", child_path,
                          target);
        if (dw > 0 && (size_t)dw < sizeof(default_fun) &&
            (lookup_fun_exact(cg, default_fun) ||
             lookup_global_exact(cg, default_fun))) {
          source = default_fun;
        } else {
          return;
        }
      } else if (ny_module_local_import_target(cg, mod, target, local_source,
                                               sizeof(local_source), 0)) {
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
    fprintf(stderr, "[exports] %s.%s <= %s\n", mod_name, export_name,
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
      stmt_t *mod = find_module_stmt_any(cg, mod_name);
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
