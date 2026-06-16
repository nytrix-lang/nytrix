#include "base/intern.h"
#include "base/util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NY_INTERN_MAP_CAP_INIT 8192
#define NY_INTERN_PTR_CACHE_SLOTS 8192
#define NY_INTERN_PTR_MAP_CAP_INIT 8192

typedef struct {
  const char *str;
  size_t len;
  uint64_t hash;
} ny_intern_entry;

static ny_intern_entry *g_intern_table = NULL;
static size_t g_intern_count = 0;
static size_t g_intern_cap = 0;

static uint32_t *g_intern_map = NULL;
static size_t g_intern_map_cap = 0;
static const char *g_intern_ptr_cache[NY_INTERN_PTR_CACHE_SLOTS];
static const char **g_intern_ptr_map = NULL;
static size_t g_intern_ptr_map_cap = 0;
static size_t g_intern_ptr_map_len = 0;

static size_t ny_intern_ptr_hash(const char *ptr) {
  uintptr_t x = (uintptr_t)ptr;
  x >>= 3;
  x ^= x >> 33;
  x *= (uintptr_t)0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (size_t)x;
}

static void ny_intern_ptr_map_resize(size_t new_cap) {
  const char **old_map = g_intern_ptr_map;
  size_t old_cap = g_intern_ptr_map_cap;
  g_intern_ptr_map = calloc(new_cap, sizeof(const char *));
  if (!g_intern_ptr_map) {
    g_intern_ptr_map = old_map;
    return;
  }
  g_intern_ptr_map_cap = new_cap;
  g_intern_ptr_map_len = 0;
  for (size_t i = 0; i < old_cap; ++i) {
    const char *ptr = old_map[i];
    if (!ptr)
      continue;
    size_t mask = g_intern_ptr_map_cap - 1;
    size_t idx = ny_intern_ptr_hash(ptr) & mask;
    while (g_intern_ptr_map[idx])
      idx = (idx + 1) & mask;
    g_intern_ptr_map[idx] = ptr;
    g_intern_ptr_map_len++;
  }
  free(old_map);
}

static void ny_intern_ptr_map_put(const char *ptr) {
  if (!ptr)
    return;
  if (!g_intern_ptr_map) {
    g_intern_ptr_map_cap = NY_INTERN_PTR_MAP_CAP_INIT;
    g_intern_ptr_map = calloc(g_intern_ptr_map_cap, sizeof(const char *));
    g_intern_ptr_map_len = 0;
    if (!g_intern_ptr_map)
      return;
  } else if ((g_intern_ptr_map_len + 1) * 10 > g_intern_ptr_map_cap * 7) {
    ny_intern_ptr_map_resize(g_intern_ptr_map_cap * 2);
  }
  size_t mask = g_intern_ptr_map_cap - 1;
  size_t idx = ny_intern_ptr_hash(ptr) & mask;
  while (g_intern_ptr_map[idx]) {
    if (g_intern_ptr_map[idx] == ptr)
      return;
    idx = (idx + 1) & mask;
  }
  g_intern_ptr_map[idx] = ptr;
  g_intern_ptr_map_len++;
}

void ny_intern_init(void) {
  if (g_intern_table)
    return;
  g_intern_cap = 4096;
  g_intern_table = malloc(g_intern_cap * sizeof(ny_intern_entry));

  g_intern_table[0].str = "";
  g_intern_table[0].len = 0;
  g_intern_table[0].hash = 0;
  g_intern_count = 1;

  g_intern_map_cap = NY_INTERN_MAP_CAP_INIT;
  g_intern_map = calloc(g_intern_map_cap, sizeof(uint32_t));
  g_intern_ptr_map_cap = NY_INTERN_PTR_MAP_CAP_INIT;
  g_intern_ptr_map = calloc(g_intern_ptr_map_cap, sizeof(const char *));
  g_intern_ptr_map_len = 0;
  ny_intern_ptr_map_put(g_intern_table[0].str);
}

ny_sym_id ny_intern_str(const char *str, size_t len) {
  if (!str || len == 0)
    return 0;
  if (!g_intern_table)
    ny_intern_init();
  if (!g_intern_table || !g_intern_map)
    return 0;
  uint64_t hash = ny_hash64(str, len);
  size_t mask = g_intern_map_cap - 1;
  size_t idx = hash & mask;

  while (g_intern_map[idx] != 0) {
    uint32_t entry_id = g_intern_map[idx];
    ny_intern_entry *e = &g_intern_table[entry_id];
    if (e->len == len && e->hash == hash && memcmp(e->str, str, len) == 0) {
      return entry_id;
    }
    idx = (idx + 1) & mask;
  }

  if (g_intern_count >= g_intern_cap) {
    g_intern_cap *= 2;
    g_intern_table = realloc(g_intern_table, g_intern_cap * sizeof(ny_intern_entry));
  }

  uint32_t new_id = g_intern_count++;
  char *dup = malloc(len + 1);
  memcpy(dup, str, len);
  dup[len] = '\0';

  g_intern_table[new_id].str = dup;
  g_intern_table[new_id].len = len;
  g_intern_table[new_id].hash = hash;
  g_intern_ptr_cache[(((uintptr_t)dup) >> 3) & (NY_INTERN_PTR_CACHE_SLOTS - 1u)] = dup;
  ny_intern_ptr_map_put(dup);

  g_intern_map[idx] = new_id;

  if (g_intern_count * 10 > g_intern_map_cap * 7) {
    size_t new_map_cap = g_intern_map_cap * 2;
    uint32_t *new_map = calloc(new_map_cap, sizeof(uint32_t));
    size_t new_mask = new_map_cap - 1;
    for (size_t i = 1; i < g_intern_count; ++i) {
      uint64_t h = g_intern_table[i].hash;
      size_t ni = h & new_mask;
      while (new_map[ni] != 0) {
        ni = (ni + 1) & new_mask;
      }
      new_map[ni] = i;
    }
    free(g_intern_map);
    g_intern_map = new_map;
    g_intern_map_cap = new_map_cap;
  }

  return new_id;
}

ny_sym_id ny_intern_cstr(const char *str) {
  if (!str)
    return 0;
  return ny_intern_str(str, strlen(str));
}

const char *ny_intern_get(ny_sym_id id) {
  if (id >= g_intern_count)
    return "";
  return g_intern_table[id].str;
}

bool ny_intern_contains_ptr(const char *str) {
  if (!str || !g_intern_table)
    return false;
  size_t slot = (((uintptr_t)str) >> 3) & (NY_INTERN_PTR_CACHE_SLOTS - 1u);
  if (g_intern_ptr_cache[slot] == str)
    return true;
  if (!g_intern_ptr_map || g_intern_ptr_map_cap == 0)
    return false;
  size_t mask = g_intern_ptr_map_cap - 1;
  size_t idx = ny_intern_ptr_hash(str) & mask;
  while (g_intern_ptr_map[idx]) {
    if (g_intern_ptr_map[idx] == str) {
      g_intern_ptr_cache[slot] = str;
      return true;
    }
    idx = (idx + 1) & mask;
  }
  return false;
}

void ny_intern_cleanup(void) {
  if (!g_intern_table)
    return;
  for (size_t i = 1; i < g_intern_count; ++i) {
    free((void *)g_intern_table[i].str);
  }
  free(g_intern_table);
  g_intern_table = NULL;

  free(g_intern_map);
  g_intern_map = NULL;
  free(g_intern_ptr_map);
  g_intern_ptr_map = NULL;
  memset(g_intern_ptr_cache, 0, sizeof(g_intern_ptr_cache));

  g_intern_count = 0;
  g_intern_cap = 0;
  g_intern_map_cap = 0;
  g_intern_ptr_map_cap = 0;
  g_intern_ptr_map_len = 0;
}
