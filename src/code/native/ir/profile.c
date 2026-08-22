/*
 * NYIR profile I/O: reads and writes block/edge heat data for
 * profile-guided optimization (PGO) tiering and layout decisions.
 */
#include "code/native/ir.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char name[128];
  uint64_t steps;
  uint64_t total_edges;
  uint64_t *pc_counts;
  size_t pc_len;
  nyir_profile_edge_t *edges;
  size_t edge_count;
  size_t edge_cap;
  bool loaded;
  bool active;
} ny_loaded_profile_t;

static ny_loaded_profile_t ny_loaded_profile;

void ny_native_profile_clear(void) {
  free(ny_loaded_profile.pc_counts);
  free(ny_loaded_profile.edges);
  memset(&ny_loaded_profile, 0, sizeof(ny_loaded_profile));
  ny_native_profile_set_runtime(0, 0);
}

static bool profile_pc_reserve(size_t need) {
  if (need <= ny_loaded_profile.pc_len)
    return true;
  uint64_t *p = realloc(ny_loaded_profile.pc_counts, need * sizeof(*p));
  if (!p)
    return false;
  memset(p + ny_loaded_profile.pc_len, 0,
         (need - ny_loaded_profile.pc_len) * sizeof(*p));
  ny_loaded_profile.pc_counts = p;
  ny_loaded_profile.pc_len = need;
  return true;
}

static bool profile_edge_push(size_t from, size_t to, uint64_t count) {
  if (ny_loaded_profile.edge_count == ny_loaded_profile.edge_cap) {
    size_t cap = ny_loaded_profile.edge_cap ? ny_loaded_profile.edge_cap * 2 : 32;
    nyir_profile_edge_t *p =
        realloc(ny_loaded_profile.edges, cap * sizeof(*p));
    if (!p)
      return false;
    ny_loaded_profile.edges = p;
    ny_loaded_profile.edge_cap = cap;
  }
  ny_loaded_profile.edges[ny_loaded_profile.edge_count++] =
      (nyir_profile_edge_t){from, to, count};
  ny_loaded_profile.total_edges += count;
  return true;
}

bool ny_native_profile_load_path(const char *path, char *err, size_t err_len) {
  ny_native_profile_clear();
  if (!path || !*path)
    return true;
  FILE *in = fopen(path, "rb");
  if (!in) {
    if (err && err_len)
      snprintf(err, err_len, "cannot open profile '%s'", path);
    return false;
  }
  char line[512];
  bool header = false;
  while (fgets(line, sizeof(line), in)) {
    char name[128] = {0};
    uint64_t steps = 0;
    if (sscanf(line, "NYP2 function=%127s steps=%" SCNu64, name, &steps) == 2) {
      snprintf(ny_loaded_profile.name, sizeof(ny_loaded_profile.name), "%s", name);
      ny_loaded_profile.steps = steps;
      header = true;
      continue;
    }
    size_t pc = 0;
    uint64_t count = 0;
    if (sscanf(line, "pc %zu count=%" SCNu64, &pc, &count) == 2) {
      if (!profile_pc_reserve(pc + 1))
        goto oom;
      ny_loaded_profile.pc_counts[pc] = count;
      continue;
    }
    size_t from = 0, to = 0;
    if (sscanf(line, "edge %zu %zu count=%" SCNu64, &from, &to, &count) == 3) {
      if (!profile_edge_push(from, to, count))
        goto oom;
    }
  }
  fclose(in);
  if (!header) {
    if (err && err_len)
      snprintf(err, err_len, "profile '%s' is not NyP2", path);
    ny_native_profile_clear();
    return false;
  }
  ny_loaded_profile.loaded = true;
  if (err && err_len)
    err[0] = '\0';
  return true;
oom:
  fclose(in);
  if (err && err_len)
    snprintf(err, err_len, "out of memory loading profile '%s'", path);
  ny_native_profile_clear();
  return false;
}

void ny_native_profile_select(const char *name) {
  ny_loaded_profile.active =
      ny_loaded_profile.loaded && name && strcmp(ny_loaded_profile.name, name) == 0;
  ny_native_profile_set_runtime(ny_loaded_profile.active
                                    ? ny_loaded_profile.total_edges
                                    : 0,
                                ny_loaded_profile.active ? ny_loaded_profile.steps : 0);
}

uint64_t ny_native_profile_block_hot(size_t pc) {
  if (!ny_loaded_profile.active || pc >= ny_loaded_profile.pc_len)
    return 0;
  return ny_loaded_profile.pc_counts[pc];
}

uint64_t ny_native_profile_edge_hot(size_t from_pc, size_t to_pc) {
  if (!ny_loaded_profile.active)
    return 0;
  for (size_t i = 0; i < ny_loaded_profile.edge_count; ++i)
    if (ny_loaded_profile.edges[i].from_pc == from_pc &&
        ny_loaded_profile.edges[i].to_pc == to_pc)
      return ny_loaded_profile.edges[i].count;
  return 0;
}

uint64_t ny_native_profile_loop_hot(size_t header_pc) {
  if (!ny_loaded_profile.active)
    return 0;
  uint64_t n = 0;
  for (size_t i = 0; i < ny_loaded_profile.edge_count; ++i)
    if (ny_loaded_profile.edges[i].to_pc == header_pc &&
        ny_loaded_profile.edges[i].from_pc >= header_pc)
      n += ny_loaded_profile.edges[i].count;
  return n;
}

bool ny_native_profile_write(FILE *out, uint64_t edge_count, uint64_t steps,
                             uint64_t hash) {
  return out && fprintf(out, "NyP2 edges=%" PRIu64 " steps=%" PRIu64
                             " hash=%" PRIu64 "\n",
                        edge_count, steps, hash) > 0;
}

bool ny_native_profile_read(FILE *in, uint64_t *edge_count, uint64_t *steps,
                            uint64_t *hash) {
  if (!in || !edge_count || !steps || !hash)
    return false;
  return fscanf(in, "NyP2 edges=%" SCNu64 " steps=%" SCNu64
                    " hash=%" SCNu64,
                edge_count, steps, hash) == 3;
}
