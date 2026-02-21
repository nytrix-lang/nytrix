#include "base/util.h"
#include "priv.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Lookup hot-path cache:
 * Symbol resolution in codegen repeatedly scans fun/global vectors. Keep a
 * tiny direct-mapped cache per query kind and invalidate entries when codegen
 * context mutates (stamp changes).
 */
#define NY_LOOKUP_CACHE_SLOTS 2048u
#define NY_LOOKUP_KEY_MAX 96u
#define NY_LOOKUP_EXACT_INDEX_SLOTS 8192u
#define NY_MODULE_USED_INDEX_SLOTS 1024u
#define NY_OVERLOAD_NAME_INDEX_SLOTS 8192u

typedef struct ny_fun_lookup_cache_entry_t {
  const codegen_t *cg;
  uint64_t stamp;
  uint64_t hash;
  uint16_t len;
  uint8_t state; /* 0=empty, 1=cached miss, 2=cached hit */
  char key[NY_LOOKUP_KEY_MAX];
  fun_sig *value;
} ny_fun_lookup_cache_entry_t;

typedef struct ny_global_lookup_cache_entry_t {
  const codegen_t *cg;
  uint64_t stamp;
  uint64_t hash;
  uint16_t len;
  uint8_t state; /* 0=empty, 1=cached miss, 2=cached hit */
  char key[NY_LOOKUP_KEY_MAX];
  binding *value;
} ny_global_lookup_cache_entry_t;

typedef struct ny_alias_lookup_cache_entry_t {
  const codegen_t *cg;
  uint64_t stamp;
  uint64_t hash;
  uint16_t len;
  uint8_t state; /* 0=empty, 1=cached miss, 2=cached hit */
  char key[NY_LOOKUP_KEY_MAX];
  const char *value;
} ny_alias_lookup_cache_entry_t;

typedef struct ny_overload_lookup_cache_entry_t {
  const codegen_t *cg;
  uint64_t stamp;
  uint64_t hash;
  uint16_t len;
  uint32_t argc;
  uint8_t state; /* 0=empty, 1=cached miss, 2=cached hit */
  char key[NY_LOOKUP_KEY_MAX];
  fun_sig *value;
} ny_overload_lookup_cache_entry_t;

static ny_fun_lookup_cache_entry_t g_fun_lookup_cache[NY_LOOKUP_CACHE_SLOTS];
static ny_global_lookup_cache_entry_t
    g_global_lookup_cache[NY_LOOKUP_CACHE_SLOTS];
static ny_alias_lookup_cache_entry_t
    g_alias_lookup_cache[NY_LOOKUP_CACHE_SLOTS];
static ny_overload_lookup_cache_entry_t
    g_overload_lookup_cache[NY_LOOKUP_CACHE_SLOTS];

typedef struct ny_fun_exact_index_entry_t {
  uint64_t hash;
  uint32_t len;
  const char *name;
  fun_sig *value;
  uint8_t state;
} ny_fun_exact_index_entry_t;

typedef struct ny_global_exact_index_entry_t {
  uint64_t hash;
  uint32_t len;
  const char *name;
  binding *value;
  uint8_t state;
} ny_global_exact_index_entry_t;

typedef struct ny_fun_tail_index_entry_t {
  uint64_t hash;
  uint32_t len;
  const char *tail_name;
  fun_sig *value;
  uint8_t state;
} ny_fun_tail_index_entry_t;

typedef struct ny_global_tail_index_entry_t {
  uint64_t hash;
  uint32_t len;
  const char *tail_name;
  binding *value;
  uint8_t state;
} ny_global_tail_index_entry_t;

typedef struct ny_module_used_index_entry_t {
  uint64_t hash;
  uint32_t len;
  const char *name;
  uint8_t state;
} ny_module_used_index_entry_t;

static ny_fun_exact_index_entry_t
    g_fun_exact_index[NY_LOOKUP_EXACT_INDEX_SLOTS];
static ny_global_exact_index_entry_t
    g_global_exact_index[NY_LOOKUP_EXACT_INDEX_SLOTS];
static ny_fun_tail_index_entry_t g_fun_tail_index[NY_LOOKUP_EXACT_INDEX_SLOTS];
static ny_global_tail_index_entry_t
    g_global_tail_index[NY_LOOKUP_EXACT_INDEX_SLOTS];
static ny_module_used_index_entry_t
    g_use_module_index[NY_MODULE_USED_INDEX_SLOTS];
static ny_module_used_index_entry_t
    g_user_use_module_index[NY_MODULE_USED_INDEX_SLOTS];
static int32_t g_overload_name_heads[NY_OVERLOAD_NAME_INDEX_SLOTS];
static int32_t *g_overload_name_next = NULL;
static size_t g_overload_name_next_cap = 0;

static const codegen_t *g_fun_exact_index_cg = NULL;
static const codegen_t *g_global_exact_index_cg = NULL;
static const codegen_t *g_fun_tail_index_cg = NULL;
static const codegen_t *g_global_tail_index_cg = NULL;
static const codegen_t *g_use_module_index_cg = NULL;
static const codegen_t *g_user_use_module_index_cg = NULL;
static const codegen_t *g_overload_name_index_cg = NULL;
static uint64_t g_fun_exact_index_stamp = 0;
static uint64_t g_global_exact_index_stamp = 0;
static uint64_t g_fun_tail_index_stamp = 0;
static uint64_t g_global_tail_index_stamp = 0;
static uint64_t g_use_module_index_stamp = 0;
static uint64_t g_user_use_module_index_stamp = 0;
static uint64_t g_overload_name_index_stamp = 0;
static bool g_fun_exact_index_ready = false;
static bool g_global_exact_index_ready = false;
static bool g_fun_tail_index_ready = false;
static bool g_global_tail_index_ready = false;
static bool g_use_module_index_ready = false;
static bool g_user_use_module_index_ready = false;
static bool g_overload_name_index_ready = false;

typedef struct ny_lookup_stamp_cache_t {
  const codegen_t *cg;
  void *module;
  void *ctx;
  const void *fun_data;
  const void *global_data;
  const void *alias_data;
  const void *import_alias_data;
  const void *user_import_alias_data;
  const void *use_modules_data;
  const void *user_use_modules_data;
  size_t fun_len;
  size_t global_len;
  size_t alias_len;
  size_t import_alias_len;
  size_t user_import_alias_len;
  size_t use_modules_len;
  size_t user_use_modules_len;
  const char *current_module_name;
  uint64_t stamp;
  bool ready;
} ny_lookup_stamp_cache_t;

static ny_lookup_stamp_cache_t g_lookup_stamp_cache;

static fun_sig *ny_fun_tail_find(codegen_t *cg, const char *tail);
static binding *ny_global_tail_find(codegen_t *cg, const char *tail);
static void ny_overload_name_index_rebuild(codegen_t *cg, uint64_t stamp);
static int32_t ny_overload_name_bucket_head(codegen_t *cg, uint64_t want_hash);

typedef void *(*ny_recurse_lookup_fn)(codegen_t *cg, const char *name,
                                      void *ctx);

typedef struct ny_overload_recurse_ctx_t {
  size_t argc;
} ny_overload_recurse_ctx_t;

static void *ny_lookup_fun_recurse(codegen_t *cg, const char *name, void *ctx) {
  (void)ctx;
  return lookup_fun(cg, name);
}

static void *ny_lookup_global_recurse(codegen_t *cg, const char *name,
                                      void *ctx) {
  (void)ctx;
  return lookup_global(cg, name);
}

static void *ny_lookup_overload_recurse(codegen_t *cg, const char *name,
                                        void *ctx) {
  ny_overload_recurse_ctx_t *ov = (ny_overload_recurse_ctx_t *)ctx;
  return resolve_overload(cg, name, ov ? ov->argc : 0);
}

static void *ny_lookup_try_scoped_or_alias(codegen_t *cg, const char *name,
                                           bool qualified,
                                           ny_recurse_lookup_fn recurse,
                                           void *ctx) {
  if (!cg || !name || !*name || qualified || !recurse)
    return NULL;

  if (cg->current_module_name && *cg->current_module_name) {
    char scoped[256];
    int nw = snprintf(scoped, sizeof(scoped), "%s.%s", cg->current_module_name,
                      name);
    if (nw > 0 && (size_t)nw < sizeof(scoped)) {
      void *scoped_res = recurse(cg, scoped, ctx);
      if (scoped_res)
        return scoped_res;
    }
  }

  const char *alias_full = resolve_import_alias(cg, name);
  if (!alias_full || !*alias_full)
    return NULL;
  if (strcmp(alias_full, name) == 0)
    return NULL;
  return recurse(cg, alias_full, ctx);
}

static inline uint64_t ny_mix64(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

static uint64_t ny_lookup_stamp(const codegen_t *cg) {
  if (g_lookup_stamp_cache.ready && g_lookup_stamp_cache.cg == cg &&
      g_lookup_stamp_cache.module == (void *)cg->module &&
      g_lookup_stamp_cache.ctx == (void *)cg->ctx &&
      g_lookup_stamp_cache.fun_data == (const void *)cg->fun_sigs.data &&
      g_lookup_stamp_cache.global_data == (const void *)cg->global_vars.data &&
      g_lookup_stamp_cache.alias_data == (const void *)cg->aliases.data &&
      g_lookup_stamp_cache.import_alias_data ==
          (const void *)cg->import_aliases.data &&
      g_lookup_stamp_cache.user_import_alias_data ==
          (const void *)cg->user_import_aliases.data &&
      g_lookup_stamp_cache.use_modules_data ==
          (const void *)cg->use_modules.data &&
      g_lookup_stamp_cache.user_use_modules_data ==
          (const void *)cg->user_use_modules.data &&
      g_lookup_stamp_cache.fun_len == cg->fun_sigs.len &&
      g_lookup_stamp_cache.global_len == cg->global_vars.len &&
      g_lookup_stamp_cache.alias_len == cg->aliases.len &&
      g_lookup_stamp_cache.import_alias_len == cg->import_aliases.len &&
      g_lookup_stamp_cache.user_import_alias_len ==
          cg->user_import_aliases.len &&
      g_lookup_stamp_cache.use_modules_len == cg->use_modules.len &&
      g_lookup_stamp_cache.user_use_modules_len == cg->user_use_modules.len &&
      g_lookup_stamp_cache.current_module_name == cg->current_module_name) {
    return g_lookup_stamp_cache.stamp;
  }

  uint64_t h = 0xcbf29ce484222325ULL;
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->module);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->ctx);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->fun_sigs.data);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->global_vars.data);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->aliases.data);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->import_aliases.data);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->user_import_aliases.data);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->use_modules.data);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->user_use_modules.data);
  h = ny_mix64(h, (uint64_t)cg->fun_sigs.len);
  h = ny_mix64(h, (uint64_t)cg->global_vars.len);
  h = ny_mix64(h, (uint64_t)cg->aliases.len);
  h = ny_mix64(h, (uint64_t)cg->import_aliases.len);
  h = ny_mix64(h, (uint64_t)cg->user_import_aliases.len);
  h = ny_mix64(h, (uint64_t)cg->use_modules.len);
  h = ny_mix64(h, (uint64_t)cg->user_use_modules.len);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->current_module_name);

  g_lookup_stamp_cache.cg = cg;
  g_lookup_stamp_cache.module = (void *)cg->module;
  g_lookup_stamp_cache.ctx = (void *)cg->ctx;
  g_lookup_stamp_cache.fun_data = (const void *)cg->fun_sigs.data;
  g_lookup_stamp_cache.global_data = (const void *)cg->global_vars.data;
  g_lookup_stamp_cache.alias_data = (const void *)cg->aliases.data;
  g_lookup_stamp_cache.import_alias_data =
      (const void *)cg->import_aliases.data;
  g_lookup_stamp_cache.user_import_alias_data =
      (const void *)cg->user_import_aliases.data;
  g_lookup_stamp_cache.use_modules_data = (const void *)cg->use_modules.data;
  g_lookup_stamp_cache.user_use_modules_data =
      (const void *)cg->user_use_modules.data;
  g_lookup_stamp_cache.fun_len = cg->fun_sigs.len;
  g_lookup_stamp_cache.global_len = cg->global_vars.len;
  g_lookup_stamp_cache.alias_len = cg->aliases.len;
  g_lookup_stamp_cache.import_alias_len = cg->import_aliases.len;
  g_lookup_stamp_cache.user_import_alias_len = cg->user_import_aliases.len;
  g_lookup_stamp_cache.use_modules_len = cg->use_modules.len;
  g_lookup_stamp_cache.user_use_modules_len = cg->user_use_modules.len;
  g_lookup_stamp_cache.current_module_name = cg->current_module_name;
  g_lookup_stamp_cache.stamp = h;
  g_lookup_stamp_cache.ready = true;
  return h;
}

static inline uint64_t ny_fun_index_version(const codegen_t *cg) {
  uint64_t h = 0x9ae16a3b2f90404fULL;
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->fun_sigs.data);
  h = ny_mix64(h, (uint64_t)cg->fun_sigs.len);
  return h;
}

static inline uint64_t ny_global_index_version(const codegen_t *cg) {
  uint64_t h = 0x517cc1b727220a95ULL;
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->global_vars.data);
  h = ny_mix64(h, (uint64_t)cg->global_vars.len);
  return h;
}

static inline bool ny_fun_sig_in_current_sigs(const codegen_t *cg,
                                              const fun_sig *sig) {
  if (!cg || !sig || !cg->fun_sigs.data || cg->fun_sigs.len == 0)
    return false;
  const fun_sig *begin = cg->fun_sigs.data;
  const fun_sig *end = begin + cg->fun_sigs.len;
  return sig >= begin && sig < end;
}

static inline uint64_t ny_fun_tail_index_version(const codegen_t *cg) {
  uint64_t h = 0xa0761d6478bd642fULL;
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->fun_sigs.data);
  h = ny_mix64(h, (uint64_t)cg->fun_sigs.len);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->use_modules.data);
  h = ny_mix64(h, (uint64_t)cg->use_modules.len);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->user_use_modules.data);
  h = ny_mix64(h, (uint64_t)cg->user_use_modules.len);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->current_module_name);
  return h;
}

static inline uint64_t ny_global_tail_index_version(const codegen_t *cg) {
  uint64_t h = 0xe7037ed1a0b428dbULL;
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->global_vars.data);
  h = ny_mix64(h, (uint64_t)cg->global_vars.len);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->use_modules.data);
  h = ny_mix64(h, (uint64_t)cg->use_modules.len);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->user_use_modules.data);
  h = ny_mix64(h, (uint64_t)cg->user_use_modules.len);
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg->current_module_name);
  return h;
}

static inline uint64_t ny_module_used_index_version(const codegen_t *cg,
                                                    bool user_only) {
  uint64_t h = user_only ? 0x6f4f6b7d3159635bULL : 0x6e40f31a0e31f26bULL;
  h = ny_mix64(h, (uint64_t)(uintptr_t)cg);
  if (user_only) {
    h = ny_mix64(h, (uint64_t)(uintptr_t)cg->user_use_modules.data);
    h = ny_mix64(h, (uint64_t)cg->user_use_modules.len);
  } else {
    h = ny_mix64(h, (uint64_t)(uintptr_t)cg->use_modules.data);
    h = ny_mix64(h, (uint64_t)cg->use_modules.len);
  }
  return h;
}

static inline uint32_t ny_fun_name_len(fun_sig *fs) {
  if (!fs || !fs->name)
    return 0;
  if (fs->name[0] == '\0') {
    fs->name_len = 0;
    return 0;
  }
  /*
   * Treat cached length as valid only when a matching hash has been computed.
   * Some code paths clone signatures and reset hash while changing the name.
   */
  if (fs->name_len && fs->name_hash)
    return fs->name_len;
  size_t n = strlen(fs->name);
  fs->name_len = (uint32_t)n;
  return (uint32_t)n;
}

static inline uint64_t ny_fun_name_hash(fun_sig *fs) {
  if (!fs || !fs->name)
    return 0;
  if (!fs->name_hash) {
    fs->name_hash = ny_hash_name(fs->name, ny_fun_name_len(fs));
  }
  return fs->name_hash;
}

/* Build open-addressed exact/tail indexes for both fun/global symbol vectors.
 */
#define NY_DEFINE_EXACT_INDEX_REBUILD(                                         \
    fn_name, index_arr, index_cg, index_stamp, index_ready, vec_field,         \
    item_type, item_name_expr, item_len_expr, item_hash_expr, entry_type,      \
    entry_name_field, entry_value_field, item_value_expr)                      \
  static void fn_name(codegen_t *cg, uint64_t stamp) {                         \
    memset(index_arr, 0, sizeof(index_arr));                                   \
    for (ssize_t i = (ssize_t)cg->vec_field.len - 1; i >= 0; --i) {            \
      item_type *item = &cg->vec_field.data[i];                                \
      const char *name = (item_name_expr);                                     \
      if (!name || !*name)                                                     \
        continue;                                                              \
      size_t len = (size_t)(item_len_expr);                                    \
      uint64_t hash = (item_hash_expr);                                        \
      size_t pos = (size_t)(hash & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u));        \
      for (size_t probe = 0; probe < NY_LOOKUP_EXACT_INDEX_SLOTS; ++probe) {   \
        entry_type *e = &index_arr[pos];                                       \
        if (!e->state) {                                                       \
          e->state = 1u;                                                       \
          e->hash = hash;                                                      \
          e->len = (uint32_t)len;                                              \
          e->entry_name_field = name;                                          \
          e->entry_value_field = (item_value_expr);                            \
          break;                                                               \
        }                                                                      \
        if (e->hash == hash && e->len == (uint32_t)len &&                      \
            memcmp(e->entry_name_field, name, len) == 0 &&                     \
            e->entry_name_field[len] == '\0') {                                \
          break;                                                               \
        }                                                                      \
        pos = (pos + 1u) & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u);                 \
      }                                                                        \
    }                                                                          \
    index_cg = cg;                                                             \
    index_stamp = stamp;                                                       \
    index_ready = true;                                                        \
  }

NY_DEFINE_EXACT_INDEX_REBUILD(ny_fun_exact_index_rebuild, g_fun_exact_index,
                              g_fun_exact_index_cg, g_fun_exact_index_stamp,
                              g_fun_exact_index_ready, fun_sigs, fun_sig,
                              item->name, ny_fun_name_len(item),
                              ny_fun_name_hash(item),
                              ny_fun_exact_index_entry_t, name, value, item)

NY_DEFINE_EXACT_INDEX_REBUILD(ny_global_exact_index_rebuild,
                              g_global_exact_index, g_global_exact_index_cg,
                              g_global_exact_index_stamp,
                              g_global_exact_index_ready, global_vars, binding,
                              item->name, ny_binding_name_len(item),
                              ny_binding_name_hash(item),
                              ny_global_exact_index_entry_t, name, value, item)

#undef NY_DEFINE_EXACT_INDEX_REBUILD

fun_sig *lookup_fun_exact(codegen_t *cg, const char *name) {
  if (!name || !*name)
    return NULL;
  size_t len = strlen(name);
  bool rebuilt_after_stale = false;
  uint64_t stamp = 0;
  uint64_t hash = 0;
  size_t pos = 0;
retry:
  stamp = ny_fun_index_version(cg);
  if (!g_fun_exact_index_ready || g_fun_exact_index_cg != cg ||
      g_fun_exact_index_stamp != stamp) {
    ny_fun_exact_index_rebuild(cg, stamp);
  }
  hash = ny_hash_name(name, len);
  pos = (size_t)(hash & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u));
  for (size_t probe = 0; probe < NY_LOOKUP_EXACT_INDEX_SLOTS; ++probe) {
    ny_fun_exact_index_entry_t *e = &g_fun_exact_index[pos];
    if (!e->state)
      return NULL;
    if (e->hash == hash && e->len == (uint32_t)len &&
        memcmp(e->name, name, len) == 0 && e->name[len] == '\0') {
      if (!ny_fun_sig_in_current_sigs(cg, e->value)) {
        if (!rebuilt_after_stale) {
          ny_fun_exact_index_rebuild(cg, stamp);
          rebuilt_after_stale = true;
          goto retry;
        }
        return NULL;
      }
      return e->value;
    }
    pos = (pos + 1u) & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u);
  }
  return NULL;
}

binding *lookup_global_exact(codegen_t *cg, const char *name) {
  if (!name || !*name)
    return NULL;
  size_t len = strlen(name);
  uint64_t stamp = ny_global_index_version(cg);
  if (!g_global_exact_index_ready || g_global_exact_index_cg != cg ||
      g_global_exact_index_stamp != stamp) {
    ny_global_exact_index_rebuild(cg, stamp);
  }
  uint64_t hash = ny_hash_name(name, len);
  size_t pos = (size_t)(hash & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u));
  for (size_t probe = 0; probe < NY_LOOKUP_EXACT_INDEX_SLOTS; ++probe) {
    ny_global_exact_index_entry_t *e = &g_global_exact_index[pos];
    if (!e->state)
      return NULL;
    if (e->hash == hash && e->len == (uint32_t)len &&
        memcmp(e->name, name, len) == 0 && e->name[len] == '\0') {
      return e->value;
    }
    pos = (pos + 1u) & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u);
  }
  return NULL;
}

static bool ny_cacheable_name(const char *name, size_t *out_len) {
  if (!name || !*name)
    return false;
  size_t len = strlen(name);
  if (len == 0 || len >= NY_LOOKUP_KEY_MAX)
    return false;
  *out_len = len;
  return true;
}

static inline uint64_t ny_overload_cache_hash(const char *name, size_t len,
                                              size_t argc) {
  return ny_hash_name(name, len) ^ ((uint64_t)argc * 11400714819323198485ULL);
}

#define NY_CACHE_ENTRY_MATCH(e, cg, stamp, hash, len, name)                    \
  ((e)->state && (e)->cg == (cg) && (e)->stamp == (stamp) &&                   \
   (e)->hash == (hash) && (e)->len == (uint16_t)(len) &&                       \
   memcmp((e)->key, (name), (len)) == 0)

#define NY_CACHE_ENTRY_FILL(e, cg, name, len, hash, value_expr)                \
  do {                                                                         \
    (e)->cg = (cg);                                                            \
    (e)->stamp = ny_lookup_stamp(cg);                                          \
    (e)->hash = (hash);                                                        \
    (e)->len = (uint16_t)(len);                                                \
    memcpy((e)->key, (name), (len));                                           \
    (e)->key[(len)] = '\0';                                                    \
    (e)->value = (value_expr);                                                 \
    (e)->state = (value_expr) ? 2u : 1u;                                       \
  } while (0)

static int ny_fun_cache_get(codegen_t *cg, const char *name, fun_sig **out) {
  size_t len = 0;
  if (!ny_cacheable_name(name, &len))
    return -1;
  uint64_t hash = ny_hash_name(name, len);
  uint64_t stamp = ny_lookup_stamp(cg);
  ny_fun_lookup_cache_entry_t *e =
      &g_fun_lookup_cache[hash & (NY_LOOKUP_CACHE_SLOTS - 1u)];
  if (!NY_CACHE_ENTRY_MATCH(e, cg, stamp, hash, len, name))
    return -1;
  if (e->state == 2u) {
    if (!ny_fun_sig_in_current_sigs(cg, e->value)) {
      e->state = 0u;
      e->value = NULL;
      return -1;
    }
    *out = e->value;
    return 1;
  }
  return 0;
}

static void ny_fun_cache_put(codegen_t *cg, const char *name, fun_sig *value) {
  size_t len = 0;
  if (!ny_cacheable_name(name, &len))
    return;
  uint64_t hash = ny_hash_name(name, len);
  ny_fun_lookup_cache_entry_t *e =
      &g_fun_lookup_cache[hash & (NY_LOOKUP_CACHE_SLOTS - 1u)];
  if (value && !ny_fun_sig_in_current_sigs(cg, value)) {
    e->state = 0u;
    e->value = NULL;
    return;
  }
  NY_CACHE_ENTRY_FILL(e, cg, name, len, hash, value);
}

#define NY_DEFINE_SIMPLE_LOOKUP_CACHE_GET(fn_name, entry_type, table_name,     \
                                          out_type)                            \
  static int fn_name(codegen_t *cg, const char *name, out_type out) {          \
    size_t len = 0;                                                            \
    if (!ny_cacheable_name(name, &len))                                        \
      return -1;                                                               \
    uint64_t hash = ny_hash_name(name, len);                                   \
    uint64_t stamp = ny_lookup_stamp(cg);                                      \
    entry_type *e = &table_name[hash & (NY_LOOKUP_CACHE_SLOTS - 1u)];          \
    if (!NY_CACHE_ENTRY_MATCH(e, cg, stamp, hash, len, name))                  \
      return -1;                                                               \
    if (e->state == 2u) {                                                      \
      *out = e->value;                                                         \
      return 1;                                                                \
    }                                                                          \
    return 0;                                                                  \
  }

#define NY_DEFINE_SIMPLE_LOOKUP_CACHE_PUT(fn_name, entry_type, table_name,     \
                                          value_type)                          \
  static void fn_name(codegen_t *cg, const char *name, value_type value) {     \
    size_t len = 0;                                                            \
    if (!ny_cacheable_name(name, &len))                                        \
      return;                                                                  \
    uint64_t hash = ny_hash_name(name, len);                                   \
    entry_type *e = &table_name[hash & (NY_LOOKUP_CACHE_SLOTS - 1u)];          \
    NY_CACHE_ENTRY_FILL(e, cg, name, len, hash, value);                        \
  }

NY_DEFINE_SIMPLE_LOOKUP_CACHE_GET(ny_global_cache_get,
                                  ny_global_lookup_cache_entry_t,
                                  g_global_lookup_cache, binding **)
NY_DEFINE_SIMPLE_LOOKUP_CACHE_PUT(ny_global_cache_put,
                                  ny_global_lookup_cache_entry_t,
                                  g_global_lookup_cache, binding *)

NY_DEFINE_SIMPLE_LOOKUP_CACHE_GET(ny_alias_cache_get,
                                  ny_alias_lookup_cache_entry_t,
                                  g_alias_lookup_cache, const char **)
NY_DEFINE_SIMPLE_LOOKUP_CACHE_PUT(ny_alias_cache_put,
                                  ny_alias_lookup_cache_entry_t,
                                  g_alias_lookup_cache, const char *)

#undef NY_DEFINE_SIMPLE_LOOKUP_CACHE_GET
#undef NY_DEFINE_SIMPLE_LOOKUP_CACHE_PUT

static int ny_overload_cache_get(codegen_t *cg, const char *name, size_t argc,
                                 fun_sig **out) {
  size_t len = 0;
  if (!ny_cacheable_name(name, &len))
    return -1;
  uint64_t hash = ny_overload_cache_hash(name, len, argc);
  uint64_t stamp = ny_lookup_stamp(cg);
  ny_overload_lookup_cache_entry_t *e =
      &g_overload_lookup_cache[hash & (NY_LOOKUP_CACHE_SLOTS - 1u)];
  if (!NY_CACHE_ENTRY_MATCH(e, cg, stamp, hash, len, name) ||
      e->argc != (uint32_t)argc)
    return -1;
  if (e->state == 2u) {
    if (!ny_fun_sig_in_current_sigs(cg, e->value)) {
      e->state = 0u;
      e->value = NULL;
      return -1;
    }
    *out = e->value;
    return 1;
  }
  return 0;
}

static void ny_overload_cache_put(codegen_t *cg, const char *name, size_t argc,
                                  fun_sig *value) {
  size_t len = 0;
  if (!ny_cacheable_name(name, &len))
    return;
  uint64_t hash = ny_overload_cache_hash(name, len, argc);
  ny_overload_lookup_cache_entry_t *e =
      &g_overload_lookup_cache[hash & (NY_LOOKUP_CACHE_SLOTS - 1u)];
  if (value && !ny_fun_sig_in_current_sigs(cg, value)) {
    e->state = 0u;
    e->value = NULL;
    return;
  }
  NY_CACHE_ENTRY_FILL(e, cg, name, len, hash, value);
  e->argc = (uint32_t)argc;
}

#undef NY_CACHE_ENTRY_MATCH
#undef NY_CACHE_ENTRY_FILL

static void ny_overload_name_index_rebuild(codegen_t *cg, uint64_t stamp) {
  memset(g_overload_name_heads, 0xff, sizeof(g_overload_name_heads));
  size_t len = cg->fun_sigs.len;
  if (g_overload_name_next_cap < len) {
    int32_t *grown = realloc(g_overload_name_next, sizeof(int32_t) * len);
    if (!grown) {
      g_overload_name_index_ready = false;
      g_overload_name_index_cg = NULL;
      g_overload_name_index_stamp = 0;
      return;
    }
    g_overload_name_next = grown;
    g_overload_name_next_cap = len;
  }
  for (size_t i = 0; i < len; ++i) {
    g_overload_name_next[i] = -1;
  }
  for (ssize_t i = (ssize_t)len - 1; i >= 0; --i) {
    fun_sig *fs = &cg->fun_sigs.data[i];
    if (!fs->name || !*fs->name)
      continue;
    uint64_t hash = ny_fun_name_hash(fs);
    size_t bucket = (size_t)(hash & (NY_OVERLOAD_NAME_INDEX_SLOTS - 1u));
    g_overload_name_next[i] = g_overload_name_heads[bucket];
    g_overload_name_heads[bucket] = (int32_t)i;
  }
  g_overload_name_index_cg = cg;
  g_overload_name_index_stamp = stamp;
  g_overload_name_index_ready = true;
}

static int32_t ny_overload_name_bucket_head(codegen_t *cg, uint64_t want_hash) {
  uint64_t stamp = ny_fun_index_version(cg);
  if (!g_overload_name_index_ready || g_overload_name_index_cg != cg ||
      g_overload_name_index_stamp != stamp) {
    ny_overload_name_index_rebuild(cg, stamp);
  }
  if (!g_overload_name_index_ready)
    return -1;
  return g_overload_name_heads[want_hash & (NY_OVERLOAD_NAME_INDEX_SLOTS - 1u)];
}

static bool ny_user_ctx_is_non_std(const codegen_t *cg) {
  if (!cg->current_module_name)
    return true;
  return strncmp(cg->current_module_name, "std.", 4) != 0 &&
         strncmp(cg->current_module_name, "lib.", 4) != 0;
}

static bool ny_block_implicit_std_symbol(const codegen_t *cg, const char *query,
                                         const char *candidate_name) {
  if (!ny_user_ctx_is_non_std(cg))
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
      "__syscall",
      "__execve",
      "__dlopen",
      "__dlsym",
      "__dlclose",
      "__dlerror",
      "__thread_spawn",
      "__thread_spawn_call",
      "__thread_launch_call",
      "__thread_join",
      "__rand64",
      "__srand",
      "__globals",
      "__set_globals",
      "__set_args",
      "__parse_ast",
      NULL,
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

#define RT_DEF(rt_name, implementation, args, sig, doc)                        \
  do {                                                                         \
    if (cg->comptime && !builtin_allowed_comptime(rt_name))                    \
      break;                                                                   \
    LLVMTypeRef ty = NULL;                                                     \
    if (strcmp(rt_name, "__copy_mem") == 0) {                                  \
      ty = LLVMFunctionType(                                                   \
          cg->type_i64,                                                        \
          (LLVMTypeRef[]){cg->type_i64, cg->type_i64, cg->type_i64}, 3, 0);    \
    } else {                                                                   \
      ty = fn_types[args];                                                     \
    }                                                                          \
    const char *impl_name = #implementation;                                   \
    LLVMValueRef f = LLVMGetNamedFunction(cg->module, impl_name);              \
    if (!f)                                                                    \
      f = LLVMAddFunction(cg->module, impl_name, ty);                          \
    fun_sig sig_obj = {.name = ny_strdup(rt_name),                             \
                       .type = ty,                                             \
                       .value = f,                                             \
                       .stmt_t = NULL,                                         \
                       .arity = (int)args,                                     \
                       .is_variadic = false,                                   \
                       .is_extern = false,                                     \
                       .effects = NY_FX_ALL,                                   \
                       .args_escape = true,                                    \
                       .args_mutated = true,                                   \
                       .returns_alias = true,                                  \
                       .effects_known = false,                                 \
                       .link_name = NULL,                                      \
                       .return_type = NULL,                                    \
                       .owned = false,                                         \
                       .name_hash = 0};                                        \
    vec_push(&cg->fun_sigs, sig_obj);                                          \
  } while (0);

#define RT_GV(rt_name, p, t, doc)                                              \
  do {                                                                         \
    LLVMValueRef g = LLVMGetNamedGlobal(cg->module, rt_name);                  \
    if (!g) {                                                                  \
      g = LLVMAddGlobal(cg->module, cg->type_i64, rt_name);                    \
      LLVMSetLinkage(g, LLVMExternalLinkage);                                  \
    }                                                                          \
    binding b = {.name = ny_strdup(rt_name),                                   \
                 .value = g,                                                   \
                 .stmt_t = NULL,                                               \
                 .is_slot = true,                                              \
                 .is_mut = false,                                              \
                 .is_used = false,                                             \
                 .owned = false,                                               \
                 .type_name = NULL,                                            \
                 .decl_type_name = NULL,                                       \
                 .name_hash = 0};                                              \
    vec_push(&cg->global_vars, b);                                             \
  } while (0);

#ifdef _WIN32
#ifdef __argc
#undef __argc
#endif
#ifdef __argv
#undef __argv
#endif
#endif

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
  if (!name || !*name)
    return NULL;
  fun_sig *cached = NULL;
  int cache_state = ny_fun_cache_get(cg, name, &cached);
  if (cache_state == 1)
    return cached;
  if (cache_state == 0)
    return NULL;

  fun_sig *res = NULL;
  bool qualified = strchr(name, '.') != NULL;

  // 1. Precise name match (local or unqualified global)
  res = lookup_fun_exact(cg, name);
  if (res && !ny_block_implicit_std_symbol(cg, name, res->name))
    goto end;
  res = NULL;

  // 2. Current module scope + import alias fallback (unqualified names only)
  res = ny_lookup_try_scoped_or_alias(cg, name, qualified,
                                      ny_lookup_fun_recurse, NULL);
  if (res)
    goto end;
  // Check aliases if name has dot
  const char *dot = qualified ? strchr(name, '.') : NULL;
  if (dot) {
    size_t prefix_len = dot - name;
    for (size_t i = 0; i < cg->aliases.len; ++i) {
      binding *al = &cg->aliases.data[i];
      const char *alias = al->name;
      size_t alias_len = (size_t)ny_binding_name_len(al);
      if (alias_len == prefix_len && strncmp(name, alias, prefix_len) == 0) {
        const char *real_mod_name = (const char *)al->stmt_t;
        // Avoid infinite recursion if alias matches itself
        if (strncmp(name, real_mod_name, prefix_len) == 0 &&
            real_mod_name[prefix_len] == '\0') {
          continue;
        }
        // Construct resolved name: real_mod_name + dot + suffix
        size_t mod_len = strlen(real_mod_name);
        size_t dot_len = strlen(dot);
        size_t full_len = mod_len + dot_len;
        char stack_buf[256];
        char *resolved =
            full_len < sizeof(stack_buf) ? stack_buf : malloc(full_len + 1);
        if (!resolved)
          continue;
        memcpy(resolved, real_mod_name, mod_len);
        memcpy(resolved + mod_len, dot, dot_len + 1);
        fun_sig *recursive_res = lookup_fun(cg, resolved);
        if (resolved != stack_buf)
          free(resolved);
        if (recursive_res) {
          res = recursive_res;
          goto end;
        }
      }
    }
  } else {
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
  if (!res && !qualified) {
    res = ny_fun_tail_find(cg, name);
    if (res)
      goto end;
  }
  if (!res && cg->parent) {
    fun_sig *p = lookup_fun(cg->parent, name);
    if (p && p->value) {
      const char *fn_link_name = LLVMGetValueName(p->value);
      LLVMValueRef my_fn = LLVMGetNamedFunction(cg->module, fn_link_name);
      if (my_fn) {
        fun_sig *n = arena_alloc(cg->arena, sizeof(fun_sig));
        *n = *p;
        n->value = my_fn;
        res = n;
        goto end;
      }
    }
  }
end:
  if (res && !ny_fun_sig_in_current_sigs(cg, res) && !cg->parent) {
    res = NULL;
  }
  ny_fun_cache_put(cg, name, res);
  return res;
}

static void ny_module_used_index_rebuild(codegen_t *cg, bool user_only,
                                         uint64_t stamp) {
  ny_module_used_index_entry_t *index =
      user_only ? g_user_use_module_index : g_use_module_index;
  memset(index, 0,
         sizeof(ny_module_used_index_entry_t) * NY_MODULE_USED_INDEX_SLOTS);
  char *const *mods_data =
      user_only ? cg->user_use_modules.data : cg->use_modules.data;
  size_t mods_len = user_only ? cg->user_use_modules.len : cg->use_modules.len;
  for (size_t i = 0; i < mods_len; ++i) {
    const char *used = mods_data[i];
    if (!used || !*used)
      continue;
    size_t used_len = strlen(used);
    uint64_t hash = ny_hash_name(used, used_len);
    size_t pos = (size_t)(hash & (NY_MODULE_USED_INDEX_SLOTS - 1u));
    for (size_t probe = 0; probe < NY_MODULE_USED_INDEX_SLOTS; ++probe) {
      ny_module_used_index_entry_t *e = &index[pos];
      if (!e->state) {
        e->state = 1u;
        e->hash = hash;
        e->len = (uint32_t)used_len;
        e->name = used;
        break;
      }
      if (e->hash == hash && e->len == (uint32_t)used_len &&
          memcmp(e->name, used, used_len) == 0 && e->name[used_len] == '\0') {
        break;
      }
      pos = (pos + 1u) & (NY_MODULE_USED_INDEX_SLOTS - 1u);
    }
  }
  if (user_only) {
    g_user_use_module_index_cg = cg;
    g_user_use_module_index_stamp = stamp;
    g_user_use_module_index_ready = true;
  } else {
    g_use_module_index_cg = cg;
    g_use_module_index_stamp = stamp;
    g_use_module_index_ready = true;
  }
}

static bool ny_module_used_lookup(codegen_t *cg, bool user_only,
                                  const char *mod, size_t mod_len) {
  if (!mod || !*mod)
    return false;
  uint64_t stamp = ny_module_used_index_version(cg, user_only);
  ny_module_used_index_entry_t *index =
      user_only ? g_user_use_module_index : g_use_module_index;
  bool ready =
      user_only ? g_user_use_module_index_ready : g_use_module_index_ready;
  const codegen_t *idx_cg =
      user_only ? g_user_use_module_index_cg : g_use_module_index_cg;
  uint64_t idx_stamp =
      user_only ? g_user_use_module_index_stamp : g_use_module_index_stamp;
  if (!ready || idx_cg != cg || idx_stamp != stamp) {
    ny_module_used_index_rebuild(cg, user_only, stamp);
  }
  uint64_t hash = ny_hash_name(mod, mod_len);
  size_t pos = (size_t)(hash & (NY_MODULE_USED_INDEX_SLOTS - 1u));
  for (size_t probe = 0; probe < NY_MODULE_USED_INDEX_SLOTS; ++probe) {
    ny_module_used_index_entry_t *e = &index[pos];
    if (!e->state)
      return false;
    if (e->hash == hash && e->len == (uint32_t)mod_len &&
        memcmp(e->name, mod, mod_len) == 0 && e->name[mod_len] == '\0') {
      return true;
    }
    pos = (pos + 1u) & (NY_MODULE_USED_INDEX_SLOTS - 1u);
  }
  return false;
}

static bool module_is_used(codegen_t *cg, const char *mod, size_t mod_len) {
  bool user_only = ny_user_ctx_is_non_std(cg);
  return ny_module_used_lookup(cg, user_only, mod, mod_len);
}

#define NY_DEFINE_TAIL_INDEX_REBUILD(                                          \
    fn_name, index_arr, index_cg, index_stamp, index_ready, vec_field,         \
    item_type, item_name_expr, item_name_len, entry_type, entry_tail_field,    \
    entry_value_field, item_value_expr)                                        \
  static void fn_name(codegen_t *cg, uint64_t stamp) {                         \
    memset(index_arr, 0, sizeof(index_arr));                                   \
    const char *last_mod = NULL;                                               \
    size_t last_mod_len = 0;                                                   \
    bool last_mod_used = false;                                                \
    for (ssize_t i = (ssize_t)cg->vec_field.len - 1; i >= 0; --i) {            \
      item_type *item = &cg->vec_field.data[i];                                \
      const char *sig_name = (item_name_expr);                                 \
      if (!sig_name || !*sig_name)                                             \
        continue;                                                              \
      const char *dot = strrchr(sig_name, '.');                                \
      if (!dot)                                                                \
        continue;                                                              \
      size_t mod_len = (size_t)(dot - sig_name);                               \
      bool mod_used = false;                                                   \
      if (last_mod && last_mod_len == mod_len &&                               \
          memcmp(last_mod, sig_name, mod_len) == 0) {                          \
        mod_used = last_mod_used;                                              \
      } else {                                                                 \
        mod_used = module_is_used(cg, sig_name, mod_len);                      \
        last_mod = sig_name;                                                   \
        last_mod_len = mod_len;                                                \
        last_mod_used = mod_used;                                              \
      }                                                                        \
      if (!mod_used)                                                           \
        continue;                                                              \
      const char *tail = dot + 1;                                              \
      if (!*tail)                                                              \
        continue;                                                              \
      size_t sig_len = (size_t)(item_name_len);                                \
      size_t len = sig_len - mod_len - 1u;                                     \
      uint64_t hash = ny_hash_name(tail, len);                                 \
      size_t pos = (size_t)(hash & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u));        \
      for (size_t probe = 0; probe < NY_LOOKUP_EXACT_INDEX_SLOTS; ++probe) {   \
        entry_type *e = &index_arr[pos];                                       \
        if (!e->state) {                                                       \
          e->state = 1u;                                                       \
          e->hash = hash;                                                      \
          e->len = (uint32_t)len;                                              \
          e->entry_tail_field = tail;                                          \
          e->entry_value_field = (item_value_expr);                            \
          break;                                                               \
        }                                                                      \
        if (e->hash == hash && e->len == (uint32_t)len &&                      \
            memcmp(e->entry_tail_field, tail, len) == 0 &&                     \
            e->entry_tail_field[len] == '\0') {                                \
          break;                                                               \
        }                                                                      \
        pos = (pos + 1u) & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u);                 \
      }                                                                        \
    }                                                                          \
    index_cg = cg;                                                             \
    index_stamp = stamp;                                                       \
    index_ready = true;                                                        \
  }

NY_DEFINE_TAIL_INDEX_REBUILD(ny_fun_tail_index_rebuild, g_fun_tail_index,
                             g_fun_tail_index_cg, g_fun_tail_index_stamp,
                             g_fun_tail_index_ready, fun_sigs, fun_sig,
                             item->name, ny_fun_name_len(item),
                             ny_fun_tail_index_entry_t, tail_name, value, item)

NY_DEFINE_TAIL_INDEX_REBUILD(ny_global_tail_index_rebuild, g_global_tail_index,
                             g_global_tail_index_cg, g_global_tail_index_stamp,
                             g_global_tail_index_ready, global_vars, binding,
                             item->name, ny_binding_name_len(item),
                             ny_global_tail_index_entry_t, tail_name, value,
                             item)

#undef NY_DEFINE_TAIL_INDEX_REBUILD

static fun_sig *ny_fun_tail_find(codegen_t *cg, const char *tail) {
  if (!tail || !*tail)
    return NULL;
  size_t len = strlen(tail);
  bool rebuilt_after_stale = false;
  uint64_t stamp = 0;
  uint64_t hash = 0;
  size_t pos = 0;
retry:
  stamp = ny_fun_tail_index_version(cg);
  if (!g_fun_tail_index_ready || g_fun_tail_index_cg != cg ||
      g_fun_tail_index_stamp != stamp) {
    ny_fun_tail_index_rebuild(cg, stamp);
  }
  hash = ny_hash_name(tail, len);
  pos = (size_t)(hash & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u));
  for (size_t probe = 0; probe < NY_LOOKUP_EXACT_INDEX_SLOTS; ++probe) {
    ny_fun_tail_index_entry_t *e = &g_fun_tail_index[pos];
    if (!e->state)
      return NULL;
    if (e->hash == hash && e->len == (uint32_t)len &&
        memcmp(e->tail_name, tail, len) == 0 && e->tail_name[len] == '\0') {
      if (!ny_fun_sig_in_current_sigs(cg, e->value)) {
        if (!rebuilt_after_stale) {
          ny_fun_tail_index_rebuild(cg, stamp);
          rebuilt_after_stale = true;
          goto retry;
        }
        return NULL;
      }
      return e->value;
    }
    pos = (pos + 1u) & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u);
  }
  return NULL;
}

static binding *ny_global_tail_find(codegen_t *cg, const char *tail) {
  if (!tail || !*tail)
    return NULL;
  size_t len = strlen(tail);
  uint64_t stamp = ny_global_tail_index_version(cg);
  if (!g_global_tail_index_ready || g_global_tail_index_cg != cg ||
      g_global_tail_index_stamp != stamp) {
    ny_global_tail_index_rebuild(cg, stamp);
  }
  uint64_t hash = ny_hash_name(tail, len);
  size_t pos = (size_t)(hash & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u));
  for (size_t probe = 0; probe < NY_LOOKUP_EXACT_INDEX_SLOTS; ++probe) {
    ny_global_tail_index_entry_t *e = &g_global_tail_index[pos];
    if (!e->state)
      return NULL;
    if (e->hash == hash && e->len == (uint32_t)len &&
        memcmp(e->tail_name, tail, len) == 0 && e->tail_name[len] == '\0') {
      return e->value;
    }
    pos = (pos + 1u) & (NY_LOOKUP_EXACT_INDEX_SLOTS - 1u);
  }
  return NULL;
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
  if (!name || !*name)
    return NULL;
  const char *cached = NULL;
  int cache_state = ny_alias_cache_get(cg, name, &cached);
  if (cache_state == 1)
    return cached;
  if (cache_state == 0)
    return NULL;

  const char *res = NULL;
  bool user_only = ny_user_ctx_is_non_std(cg);
  binding *data =
      user_only ? cg->user_import_aliases.data : cg->import_aliases.data;
  size_t len = user_only ? cg->user_import_aliases.len : cg->import_aliases.len;
  for (size_t i = 0; i < len; ++i) {
    if (strcmp(data[i].name, name) == 0) {
      res = (const char *)data[i].stmt_t;
      break;
    }
  }
  ny_alias_cache_put(cg, name, res);
  return res;
}

binding *lookup_global(codegen_t *cg, const char *name) {
  if (!name || !*name)
    return NULL;
  binding *cached = NULL;
  int cache_state = ny_global_cache_get(cg, name, &cached);
  if (cache_state == 1) {
    cached->is_used = true;
    return cached;
  }
  if (cache_state == 0)
    return NULL;

  binding *res = NULL;
  bool qualified = strchr(name, '.') != NULL;
  if (!cg->global_vars.data)
    goto end;

  // 1. Precise name match (local or unqualified global)
  res = lookup_global_exact(cg, name);
  if (res && !ny_block_implicit_std_symbol(cg, name, res->name))
    goto end;
  res = NULL;

  // 2. Current module scope + import alias fallback (unqualified names only)
  res = ny_lookup_try_scoped_or_alias(cg, name, qualified,
                                      ny_lookup_global_recurse, NULL);
  if (res)
    goto end;
  if (!qualified) {
    res = ny_global_tail_find(cg, name);
    if (res)
      goto end;
  }
  goto end;

end:
  if (res)
    res->is_used = true;
  ny_global_cache_put(cg, name, res);
  return res;
}

fun_sig *resolve_overload(codegen_t *cg, const char *name, size_t argc) {
  if (!name || !*name)
    return NULL;
  fun_sig *cached = NULL;
  int cache_state = ny_overload_cache_get(cg, name, argc, &cached);
  if (cache_state == 1)
    return cached;
  if (cache_state == 0)
    return NULL;

  fun_sig *best = NULL;
  bool qualified = strchr(name, '.') != NULL;

  ny_overload_recurse_ctx_t ov_ctx = {.argc = argc};
  best = ny_lookup_try_scoped_or_alias(cg, name, qualified,
                                       ny_lookup_overload_recurse, &ov_ctx);
  if (best)
    goto end;
  {
    int best_score = -1;
    size_t name_len = strlen(name);
    uint64_t want_hash = ny_hash_name(name, name_len);
    int32_t idx = ny_overload_name_bucket_head(cg, want_hash);
    while (idx >= 0) {
      if ((size_t)idx >= cg->fun_sigs.len)
        break;
      fun_sig *fs = &cg->fun_sigs.data[idx];
      idx = g_overload_name_next[idx];
      if (ny_fun_name_hash(fs) != want_hash)
        continue;
      if ((size_t)ny_fun_name_len(fs) != name_len)
        continue;
      if (!(fs->name == name || (memcmp(fs->name, name, name_len) == 0 &&
                                 fs->name[name_len] == '\0')))
        continue;
      if (ny_block_implicit_std_symbol(cg, name, fs->name))
        continue;
      int score = -1;
      if (!fs->is_variadic) {
        if (fs->arity == (int)argc) {
          best = fs;
          best_score = 100;
          break;
        }
        if ((int)argc < fs->arity)
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
  }
  goto end;

end:
  if (best && !ny_fun_sig_in_current_sigs(cg, best) && !cg->parent) {
    best = NULL;
  }
  ny_overload_cache_put(cg, name, argc, best);
  return best;
}
