#include "base/loader.h"
#include "parse/ast.h"
#include "base/common.h"
#ifdef _WIN32
#include "base/compat.h"
#endif
#include "base/util.h"
#include "parse/parser.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <io.h>
#include <stdlib.h>
#endif
#ifndef _WIN32
#include <sys/file.h>
#endif

typedef struct ny_std_mod {
  char *name;
  char *path;
  char *package;
} ny_std_mod;

static ny_std_mod *ny_std_mods = NULL;
static size_t ny_std_mods_len = 0;
static size_t ny_std_mods_cap = 0;

typedef struct {
  const char *key;
  int idx;
} ny_mod_slot;

typedef struct {
  const char *key;
  bool used;
} ny_pkg_slot;

static ny_mod_slot *ny_std_mod_ht = NULL;
static size_t ny_std_mod_ht_cap = 0;
static ny_pkg_slot *ny_std_pkg_ht = NULL;
static size_t ny_std_pkg_ht_cap = 0;
static time_t ny_std_latest_src_mtime = 0;
static int ny_std_latest_src_mtime_known = 0;
static uint64_t ny_std_latest_src_fingerprint = 0;
static int ny_std_latest_src_fingerprint_known = 0;

static char *read_file(const char *path);
char *ny_read_declared_module_name(const char *path);

static void ny_loader_oom(void) {
  fprintf(stderr, "oom\n");
  exit(1);
}

static void *ny_loader_xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p)
    ny_loader_oom();
  return p;
}

static void *ny_loader_xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q)
    ny_loader_oom();
  return q;
}

static char *ny_loader_xstrdup(const char *s) {
  char *d = ny_strdup(s);
  if (!d)
    ny_loader_oom();
  return d;
}

static int ny_std_trace_enabled(void) {
  static int cached = -1;
  if (cached >= 0)
    return cached;
  const char *env = getenv("NYTRIX_STD_TRACE");
  if (!env || !*env) {
    cached = 0;
    return cached;
  }
  if (strcmp(env, "0") == 0 || strcmp(env, "false") == 0) {
    cached = 0;
    return cached;
  }
  cached = 1;
  return cached;
}

static int ny_std_trace_chunks_enabled(void) {
  static int cached = -1;
  if (cached >= 0)
    return cached;
  const char *env = getenv("NYTRIX_STD_TRACE_CHUNKS");
  if (!env || !*env) {
    cached = 0;
    return cached;
  }
  if (strcmp(env, "0") == 0 || strcmp(env, "false") == 0) {
    cached = 0;
    return cached;
  }
  cached = 1;
  return cached;
}

static void std_push_mod(const char *name, const char *path, const char *package) {
  if (!name || !path || !package)
    return;
  if (ny_std_mods_len == ny_std_mods_cap) {
    size_t nc = ny_std_mods_cap ? ny_std_mods_cap * 2 : 64;
    ny_std_mod *nm = ny_loader_xrealloc(ny_std_mods, nc * sizeof(ny_std_mod));
    ny_std_mods = nm;
    ny_std_mods_cap = nc;
  }
  ny_std_mods[ny_std_mods_len++] = (ny_std_mod){
      .name = ny_loader_xstrdup(name),
      .path = ny_loader_xstrdup(path),
      .package = ny_loader_xstrdup(package),
  };
}

void ny_std_free_modules(void) {
  free(ny_std_mod_ht);
  ny_std_mod_ht = NULL;
  ny_std_mod_ht_cap = 0;
  free(ny_std_pkg_ht);
  ny_std_pkg_ht = NULL;
  ny_std_pkg_ht_cap = 0;
  if (!ny_std_mods)
    return;
  for (size_t i = 0; i < ny_std_mods_len; ++i) {
    free(ny_std_mods[i].name);
    free(ny_std_mods[i].path);
    free(ny_std_mods[i].package);
  }
  free(ny_std_mods);
  ny_std_mods = NULL;
  ny_std_mods_len = ny_std_mods_cap = 0;
  ny_std_latest_src_mtime = 0;
  ny_std_latest_src_mtime_known = 0;
  ny_std_latest_src_fingerprint = 0;
  ny_std_latest_src_fingerprint_known = 0;
}

static int is_ny_file(const char *name) {
  size_t n = strlen(name);
  return n > 3 && strcmp(name + n - 3, ".ny") == 0;
}

static char *path_join(const char *a, const char *b) {
  size_t al = strlen(a), bl = strlen(b);
  char *out = ny_loader_xmalloc(al + bl + 2);
  memcpy(out, a, al);
  out[al] = '/';
  memcpy(out + al + 1, b, bl);
  out[al + 1 + bl] = '\0';
  return out;
}

static void add_module_from_path(const char *root, const char *full_path) {
  size_t rl = strlen(root);
  if (strncmp(full_path, root, rl) != 0)
    return;
  const char *rel = full_path + rl;
  if (*rel == '/')
    rel++;
  if (!is_ny_file(rel))
    return;
  char *name = ny_loader_xstrdup(rel);
  size_t n = strlen(name);
  name[n - 3] = '\0';
  n -= 3;
  if (n > 4 && strcmp(name + n - 4, "/mod") == 0) {
    name[n - 4] = '\0';
    n -= 4;
  }
  for (char *p = name; *p; ++p) {
    if (*p == '/')
      *p = '.';
  }
  char *lib_ptr = strstr(name, ".lib.");
  if (lib_ptr) {
    size_t rest_len = strlen(lib_ptr + 5);
    memmove(lib_ptr + 1, lib_ptr + 5, rest_len + 1);
  } else {
    size_t len = strlen(name);
    if (len > 4 && strcmp(name + len - 4, ".lib") == 0) {
      name[len - 4] = '\0';
    }
  }
  const char *final_name = name;
  if (strncmp(name, "src.std.", 8) == 0) {
    final_name = name + 8;
  } else if (strncmp(name, "src.lib.", 8) == 0) {
    final_name = name + 8;
  } else if (strncmp(name, "std.", 4) == 0) {
    final_name = name + 4;
  } else if (strncmp(name, "lib.", 4) == 0) {
    final_name = name + 4;
  }
  const char *prefix = "";
  if (strstr(root, "std") != NULL)
    prefix = "std.";
  else if (strstr(root, "lib") != NULL)
    prefix = "lib.";
  char *fallback_name = ny_loader_xmalloc(strlen(prefix) + strlen(final_name) + 1);
  size_t prefix_len = strlen(prefix);
  size_t final_len = strlen(final_name);
  memcpy(fallback_name, prefix, prefix_len);
  memcpy(fallback_name + prefix_len, final_name, final_len + 1);
  char *declared_name = ny_read_declared_module_name(full_path);
  const char *module_name = (declared_name && declared_name[0]) ? declared_name : fallback_name;
  const char *dot = strchr(module_name, '.');
  char *pkg =
      dot ? ny_strndup(module_name, (size_t)(dot - module_name)) : ny_loader_xstrdup(module_name);
  if (!pkg)
    ny_loader_oom();
  std_push_mod(module_name, full_path, pkg);
  free(declared_name);
  free(fallback_name);
  free(name);
  free(pkg);
}

static void scan_dir_recursive(const char *root, const char *dir) {
  if (ny_std_trace_enabled()) {
    fprintf(stderr, "STD_TRACE dir: %s\n", dir);
  }
  DIR *d = opendir(dir);
  if (!d) {
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.' &&
        (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
      continue;
    if (ent->d_name[0] == 't' && strcmp(ent->d_name, "test") == 0)
      continue;
    char *fp = path_join(dir, ent->d_name);
    bool handled = false;
#if defined(_DIRENT_HAVE_D_TYPE)
    if (ent->d_type == DT_DIR) {
      scan_dir_recursive(root, fp);
      handled = true;
    } else if (ent->d_type == DT_REG) {
      if (is_ny_file(fp))
        add_module_from_path(root, fp);
      handled = true;
    }
#endif
    if (!handled) {
      struct stat st;
      if (stat(fp, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
          scan_dir_recursive(root, fp);
        } else if (S_ISREG(st.st_mode) && is_ny_file(fp)) {
          add_module_from_path(root, fp);
        }
      }
    }
    free(fp);
  }
  closedir(d);
}

static void ny_std_init_modules(void);

static int mod_cmp(const void *a, const void *b) {
  const ny_std_mod *ma = (const ny_std_mod *)a;
  const ny_std_mod *mb = (const ny_std_mod *)b;
  return strcmp(ma->name, mb->name);
}

static size_t ny_next_pow2(size_t n) {
  size_t p = 1;
  while (p < n)
    p <<= 1;
  return p;
}

static void ny_std_build_mod_lookup(void) {
  free(ny_std_mod_ht);
  ny_std_mod_ht = NULL;
  ny_std_mod_ht_cap = 0;
  if (ny_std_mods_len == 0)
    return;
  size_t cap = ny_next_pow2(ny_std_mods_len * 2 + 16);
  ny_std_mod_ht = (ny_mod_slot *)calloc(cap, sizeof(ny_mod_slot));
  if (!ny_std_mod_ht)
    return;
  ny_std_mod_ht_cap = cap;
  for (size_t i = 0; i < ny_std_mods_len; ++i) {
    const char *k = ny_std_mods[i].name;
    if (!k || !*k)
      continue;
    uint64_t h = ny_hash64_cstr(k);
    size_t pos = (size_t)(h & (uint64_t)(cap - 1));
    while (ny_std_mod_ht[pos].key) {
      if (strcmp(ny_std_mod_ht[pos].key, k) == 0) {
        ny_std_mod_ht[pos].idx = (int)i;
        goto next_mod;
      }
      pos = (pos + 1) & (cap - 1);
    }
    ny_std_mod_ht[pos].key = k;
    ny_std_mod_ht[pos].idx = (int)i;
  next_mod:
    (void)0;
  }
}

static int ny_std_lookup_exact_fast(const char *name) {
  if (!name || !*name || !ny_std_mod_ht || ny_std_mod_ht_cap == 0)
    return -1;
  size_t mask = ny_std_mod_ht_cap - 1;
  size_t pos = (size_t)(ny_hash64_cstr(name) & (uint64_t)mask);
  for (;;) {
    const char *k = ny_std_mod_ht[pos].key;
    if (!k)
      return -1;
    if (strcmp(k, name) == 0)
      return ny_std_mod_ht[pos].idx;
    pos = (pos + 1) & mask;
  }
}

static bool ny_scan_recursive_if_dir(const char *root, const char *dir) {
  struct stat st;
  if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
    scan_dir_recursive(root, dir);
    return true;
  }
  return false;
}

static void ny_scan_std_or_lib_roots(const char *src_root, const char *pkg) {
  char path_src[4096];
  char path_root[4096];
  char path_local_src[64];
  snprintf(path_src, sizeof(path_src), "%s/src/%s", src_root, pkg);
  if (ny_scan_recursive_if_dir(path_src, path_src))
    return;
  snprintf(path_root, sizeof(path_root), "%s/%s", src_root, pkg);
  if (ny_scan_recursive_if_dir(path_root, path_root))
    return;
  snprintf(path_local_src, sizeof(path_local_src), "src/%s", pkg);
  if (ny_scan_recursive_if_dir(path_local_src, path_local_src))
    return;
  if (strcmp(pkg, "std") == 0) {
    scan_dir_recursive("std", "std");
    return;
  }
  (void)ny_scan_recursive_if_dir(pkg, pkg);
}

static long long ny_get_dir_mtime(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return (long long)st.st_mtime;
  }
  return 0;
}

static long long ny_get_tree_mtime(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;

  long long latest = (long long)st.st_mtime;
  if (!S_ISDIR(st.st_mode))
    return latest;

  DIR *d = opendir(path);
  if (!d)
    return latest;

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.' &&
        (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
      continue;
    if (ent->d_name[0] == 't' && strcmp(ent->d_name, "test") == 0)
      continue;

    char *fp = path_join(path, ent->d_name);
    struct stat child_st;
    if (stat(fp, &child_st) == 0) {
      long long child_mtime = 0;
      if (S_ISDIR(child_st.st_mode)) {
        child_mtime = ny_get_tree_mtime(fp);
      } else if (S_ISREG(child_st.st_mode) && is_ny_file(fp)) {
        child_mtime = (long long)child_st.st_mtime;
      }
      if (child_mtime > latest)
        latest = child_mtime;
    }
    free(fp);
  }
  closedir(d);
  return latest;
}

static time_t ny_std_compute_latest_source_mtime(void) {
  time_t latest = 0;
  for (size_t i = 0; i < ny_std_mods_len; ++i) {
    const char *p = ny_std_mods[i].path;
    struct stat st;
    time_t mt = 0;
    if (p && *p && stat(p, &st) == 0)
      mt = st.st_mtime;
    if (mt > latest)
      latest = mt;
  }
  return latest;
}

static char *ny_std_get_cache_path(void) {
  static char path[1024];
  const char *root = ny_default_cache_root_dir();
  if (!root || !*root)
    return NULL;
  snprintf(path, sizeof(path), "%s/loader.idx", root);
  return path;
}

static bool ny_std_load_cache(long long current_mtime) {
  char *cp = ny_std_get_cache_path();
  if (!cp || ny_access(cp, R_OK) != 0)
    return false;
  FILE *f = fopen(cp, "rb");
  if (!f)
    return false;
  uint32_t magic = 0;
  if (fread(&magic, 4, 1, f) != 1 || (magic != 0xACE711DC && magic != 0xACE711DD)) {
    fclose(f);
    return false;
  }
  long long cached_mtime = 0;
  if (fread(&cached_mtime, 8, 1, f) != 1 || cached_mtime != current_mtime) {
    fclose(f);
    return false;
  }
  ny_std_latest_src_mtime = 0;
  ny_std_latest_src_mtime_known = 0;
  if (magic == 0xACE711DD) {
    int64_t src_latest = 0;
    if (fread(&src_latest, sizeof(src_latest), 1, f) != 1) {
      fclose(f);
      return false;
    }
    ny_std_latest_src_mtime = (time_t)src_latest;
    ny_std_latest_src_mtime_known = 1;
  }
  uint32_t count = 0;
  if (fread(&count, 4, 1, f) != 1) {
    fclose(f);
    return false;
  }
  char *name = NULL;
  char *path = NULL;
  char *pkg = NULL;
  size_t name_cap = 0;
  size_t path_cap = 0;
  size_t pkg_cap = 0;
  bool ok = true;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t nl = 0, pl = 0, kl = 0;
    if (fread(&nl, 4, 1, f) != 1 || fread(&pl, 4, 1, f) != 1 || fread(&kl, 4, 1, f) != 1) {
      ok = false;
      break;
    }
    if ((size_t)nl + 1 > name_cap) {
      char *grown = realloc(name, (size_t)nl + 1);
      if (!grown) {
        ok = false;
        break;
      }
      name = grown;
      name_cap = (size_t)nl + 1;
    }
    if ((size_t)pl + 1 > path_cap) {
      char *grown = realloc(path, (size_t)pl + 1);
      if (!grown) {
        ok = false;
        break;
      }
      path = grown;
      path_cap = (size_t)pl + 1;
    }
    if ((size_t)kl + 1 > pkg_cap) {
      char *grown = realloc(pkg, (size_t)kl + 1);
      if (!grown) {
        ok = false;
        break;
      }
      pkg = grown;
      pkg_cap = (size_t)kl + 1;
    }
    if (!name || !path || !pkg) {
      ok = false;
      break;
    }
    if (fread(name, 1, nl, f) != nl || fread(path, 1, pl, f) != pl || fread(pkg, 1, kl, f) != kl) {
      ok = false;
      break;
    }
    name[nl] = '\0';
    path[pl] = '\0';
    pkg[kl] = '\0';
    if (ny_access(path, R_OK) != 0) {
      ok = false;
      break;
    }
    std_push_mod(name, path, pkg);
  }
  free(name);
  free(path);
  free(pkg);
  fclose(f);
  if (!ok) {
    ny_std_free_modules();
    return false;
  }
  return true;
}

static void ny_std_save_cache(long long current_mtime) {
  char *cp = ny_std_get_cache_path();
  if (!cp)
    return;
  char dir[1024];
  if (snprintf(dir, sizeof(dir), "%s", cp) >= (int)sizeof(dir)) {
    free(cp);
    return;
  }
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = '\0';
    ny_ensure_dir_recursive(dir);
  }
  FILE *f = fopen(cp, "wb");
  if (!f)
    return;
  uint32_t magic = 0xACE711DD;
  fwrite(&magic, 4, 1, f);
  fwrite(&current_mtime, 8, 1, f);
  int64_t latest_i64 = (int64_t)ny_std_latest_src_mtime;
  fwrite(&latest_i64, sizeof(latest_i64), 1, f);
  uint32_t count = (uint32_t)ny_std_mods_len;
  fwrite(&count, 4, 1, f);
  for (size_t i = 0; i < ny_std_mods_len; i++) {
    uint32_t nl = (uint32_t)strlen(ny_std_mods[i].name);
    uint32_t pl = (uint32_t)strlen(ny_std_mods[i].path);
    uint32_t kl = (uint32_t)strlen(ny_std_mods[i].package);
    fwrite(&nl, 4, 1, f);
    fwrite(&pl, 4, 1, f);
    fwrite(&kl, 4, 1, f);
    fwrite(ny_std_mods[i].name, 1, nl, f);
    fwrite(ny_std_mods[i].path, 1, pl, f);
    fwrite(ny_std_mods[i].package, 1, kl, f);
  }
  fclose(f);
}

static void ny_std_init_modules(void) {
  static int init = 0;
  if (init)
    return;

  const char *root = ny_src_root();
  long long m1 = ny_get_dir_mtime(root);
  char pstd[4096], plib[4096];
  snprintf(pstd, sizeof(pstd), "%s/std", root);
  snprintf(plib, sizeof(plib), "%s/lib", root);
  long long m2 = ny_get_tree_mtime(pstd);
  long long m3 = ny_get_tree_mtime(plib);
  long long total_mtime = m1 ^ m2 ^ m3;

  if (ny_std_load_cache(total_mtime)) {
    if (!ny_std_latest_src_mtime_known) {
      ny_std_latest_src_mtime = ny_std_compute_latest_source_mtime();
      ny_std_latest_src_mtime_known = 1;
    }
    ny_std_build_mod_lookup();
    if (ny_std_trace_enabled())
      fprintf(stderr, "STD_TRACE: loaded module index from cache\n");
    init = 1;
    return;
  }

  if (ny_std_trace_enabled()) {
    fprintf(stderr, "STD_TRACE init_modules\n");
  }
  ny_tick_t t0 = ny_ticks_now();
  ny_scan_std_or_lib_roots(root, "std");
  ny_scan_std_or_lib_roots(root, "lib");
  if (ny_std_mods_len > 0) {
    qsort(ny_std_mods, ny_std_mods_len, sizeof(ny_std_mod), mod_cmp);
  }
  ny_std_latest_src_mtime = ny_std_compute_latest_source_mtime();
  ny_std_latest_src_mtime_known = 1;
  ny_std_build_mod_lookup();
  if (ny_std_trace_enabled()) {
    fprintf(stderr, "STD_TRACE: scanned %zu modules in %.4fs\n", ny_std_mods_len,
            ny_ticks_elapsed_sec(t0));
  }

  ny_std_save_cache(total_mtime);
  init = 1;
}

static const char *ny_std_pkgs[64] = {0};
static size_t ny_std_pkgs_len = 0;
static int ny_std_pkgs_init = 0;

static int ny_cstr_ptr_cmp(const void *a, const void *b) {
  const char *aa = *(const char *const *)a;
  const char *bb = *(const char *const *)b;
  if (!aa && !bb)
    return 0;
  if (!aa)
    return -1;
  if (!bb)
    return 1;
  return strcmp(aa, bb);
}

static void ny_std_init_packages(void) {
  if (ny_std_pkgs_init)
    return;
  ny_std_pkgs_init = 1;
  ny_std_init_modules();
  const size_t pkg_cap = sizeof(ny_std_pkgs) / sizeof(ny_std_pkgs[0]);
  for (size_t i = 0; i < ny_std_mods_len && ny_std_pkgs_len < pkg_cap; ++i) {
    ny_std_pkgs[ny_std_pkgs_len++] = ny_std_mods[i].package;
  }
  if (ny_std_pkgs_len > 1) {
    qsort(ny_std_pkgs, ny_std_pkgs_len, sizeof(ny_std_pkgs[0]), ny_cstr_ptr_cmp);
    size_t out = 1;
    for (size_t i = 1; i < ny_std_pkgs_len; ++i) {
      if (strcmp(ny_std_pkgs[out - 1], ny_std_pkgs[i]) != 0)
        ny_std_pkgs[out++] = ny_std_pkgs[i];
    }
    ny_std_pkgs_len = out;
  }

  free(ny_std_pkg_ht);
  ny_std_pkg_ht = NULL;
  ny_std_pkg_ht_cap = 0;
  if (ny_std_pkgs_len > 0) {
    size_t cap = ny_next_pow2(ny_std_pkgs_len * 2 + 16);
    ny_std_pkg_ht = (ny_pkg_slot *)calloc(cap, sizeof(ny_pkg_slot));
    if (ny_std_pkg_ht) {
      ny_std_pkg_ht_cap = cap;
      for (size_t i = 0; i < ny_std_pkgs_len; ++i) {
        const char *k = ny_std_pkgs[i];
        if (!k || !*k)
          continue;
        size_t pos = (size_t)(ny_hash64_cstr(k) & (uint64_t)(cap - 1));
        while (ny_std_pkg_ht[pos].used && ny_std_pkg_ht[pos].key &&
               strcmp(ny_std_pkg_ht[pos].key, k) != 0) {
          pos = (pos + 1) & (cap - 1);
        }
        ny_std_pkg_ht[pos].used = true;
        ny_std_pkg_ht[pos].key = k;
      }
    }
  }
}

size_t ny_std_module_count(void) {
  ny_std_init_modules();
  return ny_std_mods_len;
}

const char *ny_std_module_name(size_t idx) {
  if (idx >= ny_std_module_count())
    return NULL;
  return ny_std_mods[idx].name;
}

const char *ny_std_module_path(size_t idx) {
  if (idx >= ny_std_module_count())
    return NULL;
  return ny_std_mods[idx].path;
}

size_t ny_std_package_count(void) {
  ny_std_init_packages();
  return ny_std_pkgs_len;
}

const char *ny_std_package_name(size_t idx) {
  ny_std_init_packages();
  if (idx >= ny_std_pkgs_len)
    return NULL;
  return ny_std_pkgs[idx];
}

time_t ny_std_latest_source_mtime(void) {
  ny_std_init_modules();
  if (!ny_std_latest_src_mtime_known) {
    ny_std_latest_src_mtime = ny_std_compute_latest_source_mtime();
    ny_std_latest_src_mtime_known = 1;
  }
  return ny_std_latest_src_mtime;
}

static uint64_t ny_loader_hash_u64(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (i * 8)) & 0xffu;
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t ny_loader_hash_cstr(uint64_t h, const char *s) {
  if (!s)
    return ny_loader_hash_u64(h, 0);
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t ny_loader_stat_time_ns(const struct stat *st, bool ctime_field) {
  if (!st)
    return 0;
  uint64_t sec = (uint64_t)(ctime_field ? st->st_ctime : st->st_mtime);
  uint64_t nsec = 0;
#if defined(__APPLE__)
  nsec = (uint64_t)(ctime_field ? st->st_ctimespec.tv_nsec : st->st_mtimespec.tv_nsec);
#elif !defined(_WIN32)
  nsec = (uint64_t)(ctime_field ? st->st_ctim.tv_nsec : st->st_mtim.tv_nsec);
#endif
  return sec * 1000000000ULL + nsec;
}

static uint64_t ny_std_compute_source_fingerprint(void) {
  uint64_t h = 1469598103934665603ULL;
  bool strict_file_id = ny_env_enabled("NYTRIX_CACHE_STRICT_FILE_ID");
  h = ny_loader_hash_u64(h, (uint64_t)ny_std_mods_len);
  for (size_t i = 0; i < ny_std_mods_len; ++i) {
    const char *p = ny_std_mods[i].path;
    h = ny_loader_hash_cstr(h, ny_std_mods[i].name);
    h = ny_loader_hash_cstr(h, p);
    struct stat st;
    if (p && *p && stat(p, &st) == 0) {
      h = ny_loader_hash_u64(h, ny_loader_stat_time_ns(&st, false));
      h = ny_loader_hash_u64(h, (uint64_t)st.st_size);
      if (strict_file_id)
        h = ny_loader_hash_u64(h, ny_loader_stat_time_ns(&st, true));
#ifndef _WIN32
      if (strict_file_id) {
        h = ny_loader_hash_u64(h, (uint64_t)st.st_ino);
        h = ny_loader_hash_u64(h, (uint64_t)st.st_dev);
      }
#endif
    } else {
      h = ny_loader_hash_u64(h, 0);
    }
  }
  return h ? h : 1;
}

uint64_t ny_std_source_fingerprint(void) {
  ny_std_init_modules();
  if (!ny_std_latest_src_fingerprint_known) {
    ny_std_latest_src_fingerprint = ny_std_compute_source_fingerprint();
    ny_std_latest_src_fingerprint_known = 1;
  }
  return ny_std_latest_src_fingerprint;
}

static const char *strip_std_prefix(const char *name) {
  if (!name)
    return name;
  if (strncmp(name, "std.", 4) == 0)
    return name + 4;
  return name;
}

static int ny_find_module_exact_name(const char *name) {
  if (!name)
    return -1;
  int fast = ny_std_lookup_exact_fast(name);
  if (fast >= 0)
    return fast;
  size_t lo = 0, hi = ny_std_mods_len;
  while (lo < hi) {
    size_t mid = lo + ((hi - lo) >> 1);
    int cmp = strcmp(name, ny_std_mods[mid].name);
    if (cmp == 0)
      return (int)mid;
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return -1;
}

static int ny_find_module_prefixed_name(const char *name) {
  if (!name)
    return -1;
  if (strncmp(name, "std.", 4) == 0 || strncmp(name, "lib.", 4) == 0)
    return -1;
  char pbuf[512];
  snprintf(pbuf, sizeof(pbuf), "std.%s", name);
  int idx = ny_find_module_exact_name(pbuf);
  if (idx >= 0)
    return idx;
  snprintf(pbuf, sizeof(pbuf), "lib.%s", name);
  return ny_find_module_exact_name(pbuf);
}

int ny_std_find_module_by_name(const char *name) {
  if (!name || !*name)
    return -1;
  ny_std_init_modules();
  const char *tries[5] = {name, NULL, NULL, NULL, NULL};
  char buf_mod[256], buf_core[256], buf_compat_mod[256];
  snprintf(buf_mod, sizeof(buf_mod), "%s.mod", name);
  snprintf(buf_core, sizeof(buf_core), "%s.core", name);
  size_t name_len = strlen(name);
  if (!(name_len >= 4 && strcmp(name + name_len - 4, "_mod") == 0))
    snprintf(buf_compat_mod, sizeof(buf_compat_mod), "%s_mod", name);
  tries[1] = buf_mod;
  tries[2] = buf_core;
  tries[3] = buf_compat_mod[0] ? buf_compat_mod : NULL;
  for (int t = 0; t < 4; t++) {
    const char *curr = tries[t];
    if (!curr)
      continue;
    int idx = ny_find_module_exact_name(curr);
    if (idx >= 0)
      return idx;
    idx = ny_find_module_prefixed_name(curr);
    if (idx >= 0)
      return idx;
  }
  return -1;
}

static int find_module_index(const char *name) { return ny_std_find_module_by_name(name); }

static bool is_package_name(const char *pkg) {
  if (!pkg)
    return false;
  ny_std_init_packages();
  if (ny_std_pkg_ht && ny_std_pkg_ht_cap > 0) {
    size_t mask = ny_std_pkg_ht_cap - 1;
    size_t pos = (size_t)(ny_hash64_cstr(pkg) & (uint64_t)mask);
    for (;;) {
      if (!ny_std_pkg_ht[pos].used)
        break;
      const char *k = ny_std_pkg_ht[pos].key;
      if (k && strcmp(pkg, k) == 0)
        return true;
      pos = (pos + 1) & mask;
    }
    return false;
  }
  size_t lo = 0, hi = ny_std_pkgs_len;
  while (lo < hi) {
    size_t mid = lo + ((hi - lo) >> 1);
    int cmp = strcmp(pkg, ny_std_pkgs[mid]);
    if (cmp == 0)
      return true;
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return false;
}

static char *read_file(const char *path) { return ny_read_file(path); }

static bool ny_mod_ident_start(char c) {
  unsigned char uc = (unsigned char)c;
  return isalpha(uc) || c == '_';
}

static bool ny_mod_ident_char(char c) {
  unsigned char uc = (unsigned char)c;
  return isalnum(uc) || c == '_' || c == '.';
}

char *ny_read_declared_module_name(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  // Only read the first 4KB - module declaration must be near the top
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n <= 0)
    return NULL;
  buf[n] = '\0';
  const char *p = buf;
  while (*p) {
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p)
      break;
    if (*p == ';' || *p == '#') {
      while (*p && *p != '\n')
        p++;
      continue;
    }
    if (strncmp(p, "use", 3) == 0 && (isspace((unsigned char)p[3]) || p[3] == '\0')) {
      while (*p && *p != '\n')
        p++;
      continue;
    }
    if (strncmp(p, "module", 6) == 0 && (isspace((unsigned char)p[6]) || p[6] == '\0')) {
      p += 6;
      while (*p && isspace((unsigned char)*p))
        p++;
      if (!ny_mod_ident_start(*p))
        break;
      const char *start = p++;
      while (*p && ny_mod_ident_char(*p))
        p++;
      size_t len = (size_t)(p - start);
      if (len > 0) {
        char *name = ny_loader_xmalloc(len + 1);
        memcpy(name, start, len);
        name[len] = '\0';
        return name;
      }
      break;
    }
    break;
  }
  return NULL;
}

static void append_text(char **buf, size_t *len, size_t *cap, const char *txt) {
  if (!txt)
    return;
  size_t add = strlen(txt);
  if (*len + add + 2 > *cap) {
    size_t new_cap = (*len + add + 2) * 2;
    char *nb = realloc(*buf, new_cap);
    if (!nb) {
      NY_LOG_ERR("oom\n");
      exit(1);
    }
    *buf = nb;
    *cap = new_cap;
  }
  memcpy(*buf + *len, txt, add);
  *len += add;
  (*buf)[(*len)++] = '\n';
  (*buf)[*len] = '\0';
}

static const char *last_sep(const char *path) {
  const char *a = strrchr(path, '/');
  const char *b = strrchr(path, '\\');
  if (!a)
    return b;
  if (!b)
    return a;
  return (a > b) ? a : b;
}

static char *ny_modname_from_path(const char *path) {
  if (!path)
    return NULL;
  const char *last_slash = last_sep(path);
  const char *start = last_slash ? last_slash + 1 : path;
  char *name = ny_strdup(start);
  char *dot = strrchr(name, '.');
  if (dot && dot != name) {
    *dot = '\0';
  }
  return name;
}

static bool ny_module_name_is_path_like(const char *raw) {
  if (!raw || !*raw)
    return false;
  if (strchr(raw, '/') || strchr(raw, '\\'))
    return true;
  size_t len = strlen(raw);
  if (len >= 3 && strcmp(raw + len - 3, ".ny") == 0)
    return true;
  if (strncmp(raw, "./", 2) == 0 || strncmp(raw, "../", 3) == 0)
    return true;
  if (raw[0] == '/')
    return true;
  if (isalpha((unsigned char)raw[0]) && raw[1] == ':')
    return true;
  return false;
}

static char *ny_bundle_module_name(const char *raw, const char *path, bool is_std) {
  if (is_std)
    return (char *)raw;
  if (path && *path) {
    char *declared = ny_read_declared_module_name(path);
    if (declared && *declared)
      return declared;
    free(declared);
  }
  if (raw && *raw && !ny_module_name_is_path_like(raw))
    return ny_strdup(raw);
  return ny_modname_from_path(path);
}

static char *find_local_module_in_base(const char *name, const char *base_dir) {
  if (!name || !*name)
    return NULL;
  if (!base_dir || !*base_dir)
    base_dir = ".";
  size_t base_len = strlen(base_dir);
  size_t name_len = strlen(name);
  if (base_len + 1 + name_len + 8 >= 4096)
    return NULL;
  char path[4096];
  memcpy(path, base_dir, base_len);
  path[base_len] = '/';
  memcpy(path + base_len + 1, name, name_len + 1);
  bool is_abs = false;
  if (strncmp(name, "./", 2) == 0 || strncmp(name, "/", 1) == 0)
    is_abs = true;
  if (name[0] == '\\' && name[1] == '\\')
    is_abs = true;
  if (isalpha((unsigned char)name[0]) && name[1] == ':')
    is_abs = true;
  if (!is_abs && strstr(name, ".ny") == NULL && strstr(name, "\\") == NULL) {
    for (char *p = path + base_len + 1; *p; ++p)
      if (*p == '.')
        *p = '/';
  }
  if (is_ny_file(path)) {
    path[strlen(path) - 3] = '\0';
  }
  size_t path_len = strlen(path);
  memcpy(path + path_len, ".ny", 4);
  if (ny_access(path, R_OK) == 0)
    return ny_strdup(path);

  char path_mod[4096];
  memcpy(path_mod, base_dir, base_len);
  path_mod[base_len] = '/';
  memcpy(path_mod + base_len + 1, name, name_len + 1);
  if (!is_abs && strstr(name, "\\") == NULL) {
    for (char *p = path_mod + base_len + 1; *p; ++p)
      if (*p == '.')
        *p = '/';
  }
  size_t path_mod_len = strlen(path_mod);
  memcpy(path_mod + path_mod_len, "/mod.ny", 8);
  if (ny_access(path_mod, R_OK) == 0)
    return ny_strdup(path_mod);

  if (!ny_module_name_is_path_like(name) && strchr(name, '.')) {
    const char *leaf = strrchr(name, '.');
    if (leaf && leaf[1]) {
      char sib[4096];
      int nw = snprintf(sib, sizeof(sib), "%s/%s.ny", base_dir, leaf + 1);
      if (nw > 0 && (size_t)nw < sizeof(sib) && ny_access(sib, R_OK) == 0)
        return ny_strdup(sib);

      char sib_mod[4096];
      nw = snprintf(sib_mod, sizeof(sib_mod), "%s/%s/mod.ny", base_dir, leaf + 1);
      if (nw > 0 && (size_t)nw < sizeof(sib_mod) && ny_access(sib_mod, R_OK) == 0)
        return ny_strdup(sib_mod);
    }
  }
  return NULL;
}

static char *find_module_in_project_package_roots(const char *name, const char *cur) {
  const char *roots[] = {"ny_modules", "vendor/ny_modules", ".nytrix/venv/lib"};
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
    char pkg_base[4096];
    int nw = snprintf(pkg_base, sizeof(pkg_base), "%s/%s", cur && *cur ? cur : ".", roots[i]);
    if (nw <= 0 || (size_t)nw >= sizeof(pkg_base))
      continue;
    char *local = find_local_module_in_base(name, pkg_base);
    if (local)
      return local;
  }
  return NULL;
}

static char *find_module_in_package_path_list(const char *name, const char *paths) {
  if (!paths || !*paths)
    return NULL;
  const char *p = paths;
  while (*p) {
    while (*p == ':' || *p == ';')
      p++;
    const char *start = p;
    while (*p && *p != ':' && *p != ';')
      p++;
    if (p > start && (size_t)(p - start) < 4096) {
      char root[4096];
      memcpy(root, start, (size_t)(p - start));
      root[p - start] = '\0';
      char *local = find_local_module_in_base(name, root);
      if (local)
        return local;
    }
  }
  return NULL;
}

static char *find_module_in_global_package_roots(const char *name) {
  char *local = find_module_in_package_path_list(name, getenv("NYTRIX_PKG_PATH"));
  if (local)
    return local;

  char root[4096];
  const char *home_pkg = getenv("NYTRIX_PKG_HOME");
  if (home_pkg && *home_pkg) {
    local = find_local_module_in_base(name, home_pkg);
    if (local)
      return local;
  }

  const char *home = getenv("HOME");
#ifdef _WIN32
  if (!home || !*home)
    home = getenv("USERPROFILE");
#endif
  if (home && *home) {
    snprintf(root, sizeof(root), "%s/.nytrix/pkg", home);
    local = find_local_module_in_base(name, root);
    if (local)
      return local;
  }

  const char *sys_pkg = getenv("NYTRIX_SYSTEM_PKG_HOME");
  if (sys_pkg && *sys_pkg) {
    local = find_local_module_in_base(name, sys_pkg);
    if (local)
      return local;
  }
#ifndef _WIN32
  const char *system_roots[] = {"/usr/local/share/nytrix/pkg", "/usr/share/nytrix/pkg",
                                "/opt/homebrew/share/nytrix/pkg", "/opt/nytrix/share/pkg"};
  for (size_t i = 0; i < sizeof(system_roots) / sizeof(system_roots[0]); ++i) {
    local = find_local_module_in_base(name, system_roots[i]);
    if (local)
      return local;
  }
#endif
  return NULL;
}

static char *find_local_module(const char *name, const char *base_dir) {
  char *local = find_local_module_in_base(name, base_dir);
  if (local || ny_module_name_is_path_like(name))
    return local;

  char cur[4096];
  snprintf(cur, sizeof(cur), "%s", (base_dir && *base_dir) ? base_dir : ".");
  while (cur[0]) {
    local = find_module_in_project_package_roots(name, cur);
    if (local)
      return local;
    size_t n = strlen(cur);
    while (n > 1 && (cur[n - 1] == '/' || cur[n - 1] == '\\'))
      cur[--n] = '\0';
    char *slash = strrchr(cur, '/');
#ifdef _WIN32
    char *bslash = strrchr(cur, '\\');
    if (bslash && (!slash || bslash > slash))
      slash = bslash;
#endif
    if (!slash) {
      if (strcmp(cur, ".") != 0) {
        snprintf(cur, sizeof(cur), "%s", ".");
        continue;
      }
      break;
    }
    if (slash == cur) {
      if (slash == cur) {
        cur[1] = '\0';
        local = find_module_in_project_package_roots(name, cur);
        if (local)
          return local;
      }
      break;
    }
    *slash = '\0';
  }
  return find_module_in_global_package_roots(name);
}

#ifndef _WIN32
#include <dlfcn.h>
#endif

typedef void *(*curl_easy_init_t)(void);
typedef int (*curl_easy_setopt_t)(void *, int, ...);
typedef int (*curl_easy_perform_t)(void *);
typedef void (*curl_easy_cleanup_t)(void *);
typedef const char *(*curl_easy_strerror_t)(int);
typedef int (*curl_global_init_t)(long);

#define NY_CURL_GLOBAL_ALL 3L
#define NY_CURLOPT_URL 10002
#define NY_CURLOPT_WRITEDATA 10001
#define NY_CURLOPT_FOLLOWLOCATION 52
#define NY_CURLOPT_USERAGENT 10018
#define NY_CURLOPT_FAILONERROR 10194
#define NY_CURLOPT_TIMEOUT 10013

static char *resolve_remote_module(const char *url) {
  static void *curl_handle = NULL;
  static curl_easy_init_t f_init = NULL;
  static curl_easy_setopt_t f_setopt = NULL;
  static curl_easy_perform_t f_perform = NULL;
  static curl_easy_cleanup_t f_cleanup = NULL;
  static curl_easy_strerror_t f_strerror = NULL;
  static curl_global_init_t f_global_init = NULL;
  static bool curl_initialized = false;
  static bool curl_failed = false;

  if (curl_failed)
    return NULL;

  if (!curl_handle) {
#ifdef _WIN32
    curl_handle = LoadLibraryA("libcurl.dll");
    if (!curl_handle)
      curl_handle = LoadLibraryA("curl.dll");
#else
    curl_handle = dlopen("libcurl.so.4", RTLD_LAZY);
    if (!curl_handle)
      curl_handle = dlopen("libcurl.so", RTLD_LAZY);
#endif
    if (!curl_handle) {
      NY_LOG_INFO("libcurl not found, remote module loading disabled\n");
      curl_failed = true;
      return NULL;
    }
#ifdef _WIN32
    f_init = (curl_easy_init_t)GetProcAddress(curl_handle, "curl_easy_init");
    f_setopt = (curl_easy_setopt_t)GetProcAddress(curl_handle, "curl_easy_setopt");
    f_perform = (curl_easy_perform_t)GetProcAddress(curl_handle, "curl_easy_perform");
    f_cleanup = (curl_easy_cleanup_t)GetProcAddress(curl_handle, "curl_easy_cleanup");
    f_strerror = (curl_easy_strerror_t)GetProcAddress(curl_handle, "curl_easy_strerror");
    f_global_init = (curl_global_init_t)GetProcAddress(curl_handle, "curl_global_init");
#else
    f_init = (curl_easy_init_t)dlsym(curl_handle, "curl_easy_init");
    f_setopt = (curl_easy_setopt_t)dlsym(curl_handle, "curl_easy_setopt");
    f_perform = (curl_easy_perform_t)dlsym(curl_handle, "curl_easy_perform");
    f_cleanup = (curl_easy_cleanup_t)dlsym(curl_handle, "curl_easy_cleanup");
    f_strerror = (curl_easy_strerror_t)dlsym(curl_handle, "curl_easy_strerror");
    f_global_init = (curl_global_init_t)dlsym(curl_handle, "curl_global_init");
#endif
  }

  if (!curl_initialized && f_global_init) {
    f_global_init(NY_CURL_GLOBAL_ALL);
    curl_initialized = true;
  }

  if (!url)
    return NULL;

  uint64_t h = ny_hash64_cstr(url);
  char cache_dir[1024];
  const char *root = ny_default_cache_root_dir();
  snprintf(cache_dir, sizeof(cache_dir), "%s/remote",
           root && *root ? root : ny_get_temp_dir());
  ny_ensure_dir_recursive(cache_dir);

  char cache_path[1024];
  snprintf(cache_path, sizeof(cache_path), "%s/%016llx.ny", cache_dir, (unsigned long long)h);

  struct stat st;
  bool reload = ny_env_enabled("NYTRIX_REMOTE_RELOAD");
  if (!reload && stat(cache_path, &st) == 0) {
    return ny_strdup(cache_path);
  }

  NY_LOG_INFO("Downloading remote module: %s\n", url);
  void *curl = f_init();
  if (!curl)
    return NULL;

  FILE *fp = fopen(cache_path, "wb");
  if (!fp) {
    f_cleanup(curl);
    return NULL;
  }

  f_setopt(curl, NY_CURLOPT_URL, url);
  f_setopt(curl, NY_CURLOPT_WRITEDATA, fp);
  f_setopt(curl, NY_CURLOPT_FOLLOWLOCATION, 1L);
  f_setopt(curl, NY_CURLOPT_USERAGENT, "nytrix-compiler/1.0");
  f_setopt(curl, NY_CURLOPT_FAILONERROR, 1L);
  f_setopt(curl, NY_CURLOPT_TIMEOUT, 60L);

  int res_code = f_perform(curl);
  fclose(fp);
  f_cleanup(curl);

  if (res_code != 0) {
    fprintf(stderr, "Error: Failed to download remote module: %s (%s)\n", url,
            f_strerror(res_code));
    unlink(cache_path);
    return NULL;
  }

  return ny_strdup(cache_path);
}

static char *resolve_module_path(const char *raw, const char *base_dir, bool prefer_local,
                                 bool *is_std_out) {
  if (!raw)
    return NULL;

  if (strncmp(raw, "http://", 7) == 0 || strncmp(raw, "https://", 8) == 0) {
    if (is_std_out)
      *is_std_out = false;
    return resolve_remote_module(raw);
  }

  bool explicit_std = (strncmp(raw, "std.", 4) == 0);
  bool explicit_lib = (strncmp(raw, "lib.", 4) == 0);
  bool explicit_pkg = explicit_std || explicit_lib;
  if (prefer_local && explicit_lib) {
    char *local = find_local_module(raw, base_dir);
    if (local) {
      if (is_std_out)
        *is_std_out = false;
      return local;
    }
  }
  if (prefer_local && !explicit_pkg) {
    char *local = find_local_module(raw, base_dir);
    if (local) {
      if (is_std_out)
        *is_std_out = false;
      return local;
    }
  }
  int idx = find_module_index(raw);
  if (idx >= 0) {
    if (is_std_out)
      *is_std_out = true;
    return ny_strdup(ny_std_mods[idx].path);
  }
  if (!prefer_local || explicit_pkg)
    return NULL;
  char *local = find_local_module(raw, base_dir);
  if (local) {
    if (is_std_out)
      *is_std_out = false;
    return local;
  }
  return NULL;
}

static char *dir_from_path(const char *path) {
  if (!path || !*path)
    return ny_strdup(".");
  const char *slash = last_sep(path);
  if (!slash)
    return ny_strdup(".");
  size_t len = (size_t)(slash - path);
  if (len == 0)
    return ny_strdup("/");
  char *out = malloc(len + 1);
  if (!out) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  memcpy(out, path, len);
  out[len] = '\0';
  return out;
}

typedef struct {
  char *path;
  char *name;
  bool processed;
  bool is_std;
  char *bundle_txt;
  size_t bundle_len;
  bool skip_entry;
  int export_depth;
  char **deps;
  size_t deps_len;
  size_t deps_cap;
} mod_entry;

typedef struct {
  mod_entry *entries;
  size_t len;
  size_t cap;
  const char **path_ht;
  size_t path_ht_cap;
  size_t path_ht_len;
  bool skip_std;
} mod_list;

static size_t mod_list_pow2_cap(size_t n) {
  size_t cap = 16;
  while (cap < n)
    cap <<= 1u;
  return cap;
}

static size_t mod_list_hash_path(const char *path) {
  return (size_t)ny_hash64(path, strlen(path));
}

static void mod_list_path_ht_init(mod_list *list, size_t want_cap) {
  if (list->path_ht)
    return;
  list->path_ht_cap = mod_list_pow2_cap(want_cap ? want_cap : 16);
  list->path_ht = ny_loader_xmalloc(list->path_ht_cap * sizeof(list->path_ht[0]));
  memset((void *)list->path_ht, 0, list->path_ht_cap * sizeof(list->path_ht[0]));
  list->path_ht_len = 0;
}

static void mod_list_path_ht_rebuild(mod_list *list, size_t want_cap) {
  const char **old_ht = list->path_ht;
  size_t old_cap = list->path_ht_cap;

  list->path_ht = NULL;
  list->path_ht_cap = 0;
  list->path_ht_len = 0;
  mod_list_path_ht_init(list, want_cap);

  if (!old_ht)
    return;
  for (size_t i = 0; i < old_cap; ++i) {
    const char *k = old_ht[i];
    if (!k)
      continue;
    size_t mask = list->path_ht_cap - 1u;
    size_t pos = mod_list_hash_path(k) & mask;
    while (list->path_ht[pos])
      pos = (pos + 1u) & mask;
    list->path_ht[pos] = k;
    list->path_ht_len++;
  }
  free((void *)old_ht);
}

static bool mod_list_has_path(mod_list *list, const char *path) {
  mod_list_path_ht_init(list, list->cap ? list->cap * 2 : 16);
  if (!list->path_ht_cap)
    return false;
  size_t mask = list->path_ht_cap - 1u;
  size_t pos = mod_list_hash_path(path) & mask;
  for (;;) {
    const char *k = list->path_ht[pos];
    if (!k)
      return false;
    if (strcmp(k, path) == 0)
      return true;
    pos = (pos + 1u) & mask;
  }
}

static bool mod_list_has_descendant_name(mod_list *list, const char *name) {
  if (!list || !name || !*name)
    return false;
  size_t name_len = strlen(name);
  for (size_t i = 0; i < list->len; ++i) {
    const char *cur = list->entries[i].name;
    if (!cur || !*cur)
      continue;
    if (strncmp(cur, name, name_len) == 0 && cur[name_len] == '.')
      return true;
  }
  return false;
}

static void mod_list_put_path(mod_list *list, const char *path) {
  mod_list_path_ht_init(list, list->cap ? list->cap * 2 : 16);
  if ((list->path_ht_len + 1u) * 10u >= list->path_ht_cap * 7u)
    mod_list_path_ht_rebuild(list, list->path_ht_cap ? list->path_ht_cap * 2u : 32u);
  size_t mask = list->path_ht_cap - 1u;
  size_t pos = mod_list_hash_path(path) & mask;
  while (list->path_ht[pos])
    pos = (pos + 1u) & mask;
  list->path_ht[pos] = path;
  list->path_ht_len++;
}

static int mod_priority(const char *path) {
  if (!path)
    return 100;
  if (strstr(path, "os/sys.ny"))
    return 1;
  if (strstr(path, "str/mod.ny"))
    return 2;
  if (strstr(path, "str/io.ny"))
    return 3;
  if (strstr(path, "core/reflect.ny"))
    return 4;
  if (strstr(path, "core/error.ny"))
    return 5;
  if (strstr(path, "core/dict.ny"))
    return 6;
  if (strstr(path, "core/set.ny"))
    return 7;
  if (strstr(path, "core/mod.ny"))
    return 10;
  if (strstr(path, "/mod.ny") ||
      (strlen(path) >= 6 && strcmp(path + strlen(path) - 6, "mod.ny") == 0))
    return 20;
  return 30;
}

static int mod_entry_path_cmp(const void *a, const void *b) {
  const mod_entry *ma = (const mod_entry *)a;
  const mod_entry *mb = (const mod_entry *)b;
  if (!ma->path && !mb->path)
    return 0;
  if (!ma->path)
    return -1;
  if (!mb->path)
    return 1;
  int pa = mod_priority(ma->path);
  int pb = mod_priority(mb->path);
  if (pa != pb)
    return pa - pb;
  return strcmp(ma->path, mb->path);
}

static void mod_entry_add_dep(mod_entry *entry, const char *path) {
  if (!entry || !path || !*path)
    return;
  if (entry->path && strcmp(entry->path, path) == 0)
    return;
  for (size_t i = 0; i < entry->deps_len; ++i) {
    if (strcmp(entry->deps[i], path) == 0)
      return;
  }
  if (entry->deps_len == entry->deps_cap) {
    size_t nc = entry->deps_cap ? entry->deps_cap * 2 : 4;
    entry->deps = ny_loader_xrealloc(entry->deps, nc * sizeof(char *));
    entry->deps_cap = nc;
  }
  entry->deps[entry->deps_len++] = ny_loader_xstrdup(path);
}

static long mod_list_find_entry_by_path(mod_list *list, const char *path) {
  if (!list || !path)
    return -1;
  for (size_t i = 0; i < list->len; ++i) {
    if (list->entries[i].path && strcmp(list->entries[i].path, path) == 0)
      return (long)i;
  }
  return -1;
}

static void mod_list_visit_deps(mod_list *list, size_t idx, unsigned char *state,
                                size_t *order, size_t *order_len) {
  if (!list || idx >= list->len || !state || !order || !order_len)
    return;
  if (state[idx] == 2)
    return;
  if (state[idx] == 1)
    return;
  state[idx] = 1;
  mod_entry *entry = &list->entries[idx];
  for (size_t i = 0; i < entry->deps_len; ++i) {
    long dep_idx = mod_list_find_entry_by_path(list, entry->deps[i]);
    if (dep_idx >= 0 && (size_t)dep_idx != idx && !list->entries[dep_idx].skip_entry)
      mod_list_visit_deps(list, (size_t)dep_idx, state, order, order_len);
  }
  state[idx] = 2;
  order[(*order_len)++] = idx;
}

static void mod_list_sort_for_bundle(mod_list *list) {
  if (!list || list->len < 2)
    return;
  qsort(list->entries, list->len, sizeof(mod_entry), mod_entry_path_cmp);
  unsigned char *state = calloc(list->len, sizeof(unsigned char));
  size_t *order = malloc(list->len * sizeof(size_t));
  if (!state || !order) {
    free(state);
    free(order);
    return;
  }
  size_t order_len = 0;
  for (size_t i = 0; i < list->len; ++i)
    mod_list_visit_deps(list, i, state, order, &order_len);
  if (order_len == list->len) {
    mod_entry *sorted = ny_loader_xmalloc(list->len * sizeof(mod_entry));
    for (size_t i = 0; i < list->len; ++i)
      sorted[i] = list->entries[order[i]];
    free(list->entries);
    list->entries = sorted;
  }
  free(state);
  free(order);
}

static long mod_list_find_path_index(mod_list *list, const char *path) {
  if (!list || !path)
    return -1;
  for (size_t i = 0; i < list->len; ++i) {
    if (list->entries[i].path && strcmp(list->entries[i].path, path) == 0)
      return (long)i;
  }
  return -1;
}

static bool ny_export_depth_more_permissive(int next, int cur) {
  if (next < 0)
    return cur >= 0;
  if (cur < 0)
    return false;
  return next > cur;
}

static void mod_list_add_ex(mod_list *list, const char *path, const char *name,
                            bool is_std, int export_depth) {
  if (mod_list_has_path(list, path)) {
    long idx = mod_list_find_path_index(list, path);
    if (idx >= 0 &&
        ny_export_depth_more_permissive(export_depth,
                                        list->entries[idx].export_depth)) {
      list->entries[idx].export_depth = export_depth;
      list->entries[idx].processed = false;
    }
    return;
  }
  if (list->len == list->cap) {
    size_t new_cap = list->cap ? list->cap * 2 : 16;
    list->entries = ny_loader_xrealloc(list->entries, new_cap * sizeof(mod_entry));
    list->cap = new_cap;
  }
  char *path_dup = ny_loader_xstrdup(path);
  list->entries[list->len++] = (mod_entry){.path = path_dup,
                                           .name = ny_loader_xstrdup(name),
                                           .processed = false,
                                           .is_std = is_std,
                                           .bundle_txt = NULL,
                                           .bundle_len = 0,
                                           .skip_entry = false,
                                           .export_depth = export_depth};
  mod_list_put_path(list, path_dup);
}

static void mod_list_add(mod_list *list, const char *path, const char *name, bool is_std) {
  mod_list_add_ex(list, path, name, is_std, -1);
}

static void add_bare_std_modules(mod_list *list) {
  static const char *const k_std_default_modules[] = {
      "std.core",      "std.os.prim", "std.math",        "std.math.nt",
      "std.math.bin",  "std.os",      "std.core.str",
  };
  for (size_t i = 0; i < sizeof(k_std_default_modules) / sizeof(k_std_default_modules[0]); ++i) {
    int idx = ny_std_find_module_by_name(k_std_default_modules[i]);
    if (idx >= 0)
      mod_list_add(list, ny_std_mods[idx].path, ny_std_mods[idx].name, true);
  }
}

static void add_module_dependency_ex(mod_list *list, const char *raw,
                                     const char *base_dir, bool prefer_local,
                                     int export_depth) {
  if (!list || !raw || !*raw)
    return;
  bool is_std = false;
  char *path = resolve_module_path(raw, base_dir, prefer_local, &is_std);
  if (!path)
    return;
  if (!(list->skip_std && is_std)) {
    char *mname = ny_bundle_module_name(raw, path, is_std);
    mod_list_add_ex(list, path, mname, is_std, export_depth);
    if (!is_std)
      free(mname);
  }
  free(path);
}

static void add_module_dependency(mod_list *list, const char *raw,
                                  const char *base_dir, bool prefer_local) {
  add_module_dependency_ex(list, raw, base_dir, prefer_local, -1);
}

static bool ny_std_module_has_descendant(const char *module_name) {
  if (!module_name || !*module_name)
    return false;
  size_t n = strlen(module_name);
  for (size_t i = 0; i < ny_std_mods_len; ++i) {
    const char *cur = ny_std_mods[i].name;
    if (!cur || !*cur)
      continue;
    if (strncmp(cur, module_name, n) == 0 && cur[n] == '.')
      return true;
  }
  return false;
}

static int module_export_child_depth(const char *module_name) {
  if (!module_name)
    return -1;
  /*
   * Exported names that are also module children should load as namespace
   * children.  If the parent has a known subtree, expose one further child
   * layer so package roots can publish category modules without hand-written
   * `use` fanout in the package file.  Non-stdlib/local declared modules keep
   * the legacy recursive behavior because their sibling graph is not indexed.
   */
  if (strncmp(module_name, "std.", 4) == 0 && ny_std_module_has_descendant(module_name))
    return 1;
  return -1;
}

static void scan_module_export_dependencies(mod_list *list, const char *module_name,
                                            lexer_t *lx, token_t *tok,
                                            const char *base_dir,
                                            bool prefer_local,
                                            int child_export_depth) {
  if (!list || !module_name || !*module_name || !lx || !tok)
    return;
  token_t t = *tok;
  if (t.kind != NY_T_LPAREN)
    return;

  int paren_depth = 1;
  for (;;) {
    t = lexer_next(lx);
    if (t.kind == NY_T_EOF)
      break;
    if (t.kind == NY_T_LPAREN) {
      paren_depth++;
      continue;
    }
    if (t.kind == NY_T_RPAREN) {
      paren_depth--;
      if (paren_depth == 0) {
        *tok = lexer_next(lx);
        return;
      }
      continue;
    }
    if (paren_depth != 1 || t.kind != NY_T_IDENT)
      continue;

    char *leaf = dup_token_lexeme(t);
    if (!leaf)
      continue;
    size_t full_len = strlen(module_name) + 1 + strlen(leaf);
    char *child = malloc(full_len + 1);
    if (child) {
      snprintf(child, full_len + 1, "%s.%s", module_name, leaf);
      add_module_dependency_ex(list, child, base_dir, prefer_local, child_export_depth);
      free(child);
    }
    free(leaf);
  }
  *tok = t;
}

#ifndef _WIN32
static int ny_std_load_threads(size_t work_items) {
  const char *env = getenv("NYTRIX_STD_THREADS");
  if (env && *env) {
    int v = atoi(env);
    return v > 0 ? v : 1;
  }
  if (work_items < 4)
    return 1;
  long ncpu = ny_cpu_count();
  int cpu = (ncpu > 0) ? (int)ncpu : 1;
  int cap = cpu > 8 ? 8 : cpu;
  return cap < 1 ? 1 : cap;
}
#endif

static bool ny_same_path(const char *a, const char *b) {
  if (!a || !b)
    return false;
  char ra[4096], rb[4096];
  if (ny_realpath(a, ra) && ny_realpath(b, rb))
    return strcmp(ra, rb) == 0;
  return strcmp(a, b) == 0;
}

static char *ny_build_module_chunk(const char *path, const char *name, size_t *out_len,
                                   bool append_module_use) {
  if (!path || !name)
    return NULL;
  if (ny_std_trace_enabled()) {
    fprintf(stderr, "STD_TRACE chunk: %s\n", path);
  }
  char *txt = read_file(path);
  if (!txt)
    return NULL;
  size_t total = 0, cap = strlen(txt) + 256;
  char *bundle = malloc(cap);
  if (!bundle) {
    free(txt);
    return NULL;
  }
  bundle[0] = '\0';
  char line_buf[1024];
  snprintf(line_buf, sizeof(line_buf), "#line 1 \"%s\"", path);
  append_text(&bundle, &total, &cap, line_buf);
  bool has_decl = false;
  const char *p = txt;
  while (*p) {
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p)
      break;
    if (*p == ';' || *p == '#') {
      while (*p && *p != '\n')
        p++;
      continue;
    }
    if (strncmp(p, "module", 6) == 0 && (isspace((unsigned char)p[6]) || p[6] == '\0')) {
      has_decl = true;
      break;
    }
    if (strncmp(p, "use", 3) == 0 && (isspace((unsigned char)p[3]) || p[3] == '\0')) {
      while (*p && *p != '\n')
        p++;
      continue;
    }
    break;
  }
  if (has_decl) {
    append_text(&bundle, &total, &cap, txt);
    append_text(&bundle, &total, &cap, "\n");
  } else {
    char header[512];
    snprintf(header, sizeof(header), "module %s * {", name);
    append_text(&bundle, &total, &cap, header);
    append_text(&bundle, &total, &cap, "\n");
    append_text(&bundle, &total, &cap, txt);
    append_text(&bundle, &total, &cap, "\n");
    append_text(&bundle, &total, &cap, "}\n");
  }
  if (append_module_use) {
    char use_stmt[512];
    snprintf(use_stmt, sizeof(use_stmt), "use %s", name);
    append_text(&bundle, &total, &cap, use_stmt);
    append_text(&bundle, &total, &cap, "\n");
  }
  free(txt);
  if (out_len)
    *out_len = total;
  if (ny_std_trace_chunks_enabled()) {
    fprintf(stderr, "STD_TRACE_CHUNK_BEGIN %s (%s)\n", name, path);
    fwrite(bundle, 1, total, stderr);
    if (total == 0 || bundle[total - 1] != '\n')
      fputc('\n', stderr);
    fprintf(stderr, "STD_TRACE_CHUNK_END %s\n", name);
  }
  if (ny_std_trace_enabled()) {
    fprintf(stderr, "STD_TRACE done: %s (%zu bytes)\n", path, total);
  }
  return bundle;
}

#ifndef _WIN32
typedef struct {
  mod_entry *entries;
  size_t count;
  size_t next;
  const char *entry_path;
  bool append_module_uses;
  pthread_mutex_t mu;
} bundle_ctx;

static void *ny_std_source_worker(void *arg) {
  bundle_ctx *ctx = (bundle_ctx *)arg;
  for (;;) {
    size_t idx;
    pthread_mutex_lock(&ctx->mu);
    idx = ctx->next++;
    pthread_mutex_unlock(&ctx->mu);
    if (idx >= ctx->count)
      break;
    mod_entry *e = &ctx->entries[idx];
    if (!e->path || e->bundle_txt)
      continue;
    if (ctx->entry_path && ny_same_path(e->path, ctx->entry_path)) {
      e->skip_entry = true;
      continue;
    }
    e->bundle_txt =
        ny_build_module_chunk(e->path, e->name, &e->bundle_len, ctx->append_module_uses);
  }
  return NULL;
}
#endif

static void scan_dependencies(mod_list *list, size_t idx) {
  if (list->entries[idx].processed)
    return;
  if (ny_std_trace_enabled()) {
    fprintf(stderr, "STD_TRACE scan: %s\n", list->entries[idx].path);
  }
  NY_LOG_V2("Scanning dependencies for %s\n", list->entries[idx].path);
  list->entries[idx].processed = true;
  char *txt = read_file(list->entries[idx].path);
  if (!txt) {
    NY_LOG_V2("Failed to read file: %s\n", list->entries[idx].path);
    return;
  }

  char *base_dir = dir_from_path(list->entries[idx].path);
  bool prefer_local = !list->entries[idx].is_std;

  // Optimized scanner for 'use' and 'module'
  lexer_t lx;
  lexer_init(&lx, txt, list->entries[idx].path);
  int depth = 0;
  for (;;) {
    token_t t = lexer_next(&lx);
    if (t.kind == NY_T_EOF)
      break;

  process_tok:
    if (t.kind == NY_T_LBRACE || t.kind == NY_T_LPAREN || t.kind == NY_T_LBRACK) {
      depth++;
    } else if (t.kind == NY_T_RBRACE || t.kind == NY_T_RPAREN || t.kind == NY_T_RBRACK) {
      if (depth > 0)
        depth--;
    } else if (depth == 0) {
      if (t.kind == NY_T_MODULE) {
        token_t mod_tok = lexer_next(&lx);
        char *module_name = NULL;
        if (mod_tok.kind == NY_T_IDENT || mod_tok.kind == NY_T_NUMBER)
          module_name = parse_use_name(&lx, &mod_tok, &t);
        else
          t = mod_tok;
        if (module_name && list->entries[idx].export_depth != 0) {
          int child_export_depth = list->entries[idx].export_depth > 0
                                       ? list->entries[idx].export_depth - 1
                                       : module_export_child_depth(module_name);
          scan_module_export_dependencies(list, module_name, &lx, &t, base_dir,
                                          prefer_local, child_export_depth);
        }
        free(module_name);
        if (t.kind != NY_T_EOF)
          goto process_tok;
      } else if (t.kind == NY_T_USE) {
        token_t mod_tok = lexer_next(&lx);
        if (mod_tok.kind == NY_T_IDENT || mod_tok.kind == NY_T_STRING) {
          token_t next_tok;
          char *raw = parse_use_name(&lx, &mod_tok, &next_tok);
          if (raw) {
            if (strcmp(raw, "std") == 0 && next_tok.kind == NY_T_IDENT &&
                next_tok.line == mod_tok.line) {
              token_t after_tail = next_tok;
              char *tail = parse_use_name(&lx, &next_tok, &after_tail);
              if (tail && *tail) {
                size_t full_len = strlen(raw) + 1 + strlen(tail);
                char *full = malloc(full_len + 1);
                if (full) {
                  snprintf(full, full_len + 1, "%s.%s", raw, tail);
                  free(raw);
                  raw = full;
                  next_tok = after_tail;
                }
              }
              free(tail);
            }
            bool dep_is_std = false;
            char *dep_path = resolve_module_path(raw, base_dir, prefer_local, &dep_is_std);
            if (dep_path) {
              if (!(list->skip_std && dep_is_std)) {
                char *mname = ny_bundle_module_name(raw, dep_path, dep_is_std);
                mod_list_add_ex(list, dep_path, mname, dep_is_std, -1);
                if (!dep_is_std)
                  free(mname);
                mod_entry_add_dep(&list->entries[idx], dep_path);
              }
              free(dep_path);
            }
            free(raw);
          }
          t = next_tok;
          if (t.kind != NY_T_EOF)
            goto process_tok;
        }
      }
    }
  }

  free(base_dir);
  free(txt);
}

char *ny_build_std_source_ex(const char **modules, size_t module_count, std_mode_t mode,
                             int verbose, const char *entry_path, bool append_module_uses) {
  NY_LOG_V1("Generating std.ny source (mode=%d, count=%zu)\n", mode, module_count);
  if (mode == STD_MODE_NONE && module_count == 0)
    return NULL;
  ny_std_init_modules();
  char *prebuilt_src = NULL;
  if (ny_std_find_module_by_name("std.core.mod") < 0) {
    const char *prebuilt = getenv("NYTRIX_STD_PREBUILT");
    if (prebuilt && ny_access(prebuilt, R_OK) == 0) {
      if (verbose)
        printf("Using prebuilt standard library: %s\n", prebuilt);
      prebuilt_src = read_file(prebuilt);
    }
  }
  mod_list mods = {0};
  mods.skip_std = (mode == STD_MODE_NONE);
  char *entry_dir = entry_path ? dir_from_path(entry_path) : NULL;
  const char **seed_modules = modules;
  size_t seed_count = module_count;
  if (mode == STD_MODE_FULL) {
    for (size_t i = 0; i < ny_std_mods_len; ++i) {
      mod_list_add(&mods, ny_std_mods[i].path, ny_std_mods[i].name, true);
    }
  } else {
    if (seed_count == 0 && mode != STD_MODE_NONE) {
      int core_idx = ny_std_find_module_by_name("std.core");
      if (core_idx >= 0) {
        mod_list_add(&mods, ny_std_mods[core_idx].path, ny_std_mods[core_idx].name, true);
      } else {
        // Fallback to all if core not found
        for (size_t j = 0; j < ny_std_mods_len; ++j) {
          mod_list_add(&mods, ny_std_mods[j].path, ny_std_mods[j].name, true);
        }
      }
    }
  }
  for (size_t i = 0; i < seed_count; ++i) {
    const char *raw = seed_modules[i];
    const char *name = strip_std_prefix(raw);
    if (strcmp(name, "std") == 0) {
      add_bare_std_modules(&mods);
      continue;
    }
    bool is_std = false;
    char *path = resolve_module_path(raw, entry_dir ? entry_dir : ".", true, &is_std);
    if (path) {
      char *mname = ny_bundle_module_name(raw, path, is_std);
      mod_list_add(&mods, path, mname, is_std);
      if (!is_std)
        free(mname);
      free(path);
      continue;
    }
    if (is_package_name(name)) {
      for (size_t k = 0; k < ny_std_mods_len; ++k) {
        if (strcmp(ny_std_mods[k].package, name) == 0) {
          mod_list_add(&mods, ny_std_mods[k].path, ny_std_mods[k].name, true);
        }
      }
      continue;
    }
    if (verbose)
      NY_LOG_ERR("Module not found: %s\n", raw);
  }
  bool changed = true;
  while (changed) {
    changed = false;
    size_t current_len = mods.len;
    for (size_t i = 0; i < current_len; ++i) {
      if (!mods.entries[i].processed) {
        scan_dependencies(&mods, i);
        if (mods.len > current_len)
          changed = true;
      }
    }
  }
  for (size_t i = 0; i < mods.len; ++i) {
    if (!mods.entries[i].processed) {
      scan_dependencies(&mods, i);
    }
  }
  mod_list_sort_for_bundle(&mods);
  if (mods.len == 0 && !prebuilt_src) {
    free(entry_dir);
    if (mods.path_ht)
      free((void *)mods.path_ht);
    if (mods.entries)
      free(mods.entries);
    return NULL;
  }
#ifndef _WIN32
  if (mods.len > 0) {
    int threads = ny_std_load_threads(mods.len);
    if (threads > 1) {
      bundle_ctx ctx = {
          .entries = mods.entries,
          .count = mods.len,
          .next = 0,
          .entry_path = entry_path,
          .append_module_uses = append_module_uses};
      pthread_mutex_init(&ctx.mu, NULL);
      if ((size_t)threads > mods.len)
        threads = (int)mods.len;
      pthread_t *tids = ny_loader_xmalloc(sizeof(pthread_t) * (size_t)threads);
      int created = 0;
      for (int i = 0; i < threads; i++) {
        if (pthread_create(&tids[i], NULL, ny_std_source_worker, &ctx) == 0) {
          created++;
        } else {
          NY_LOG_WARN("Failed to spawn std bundler thread; falling back.\n");
          break;
        }
      }
      for (int i = 0; i < created; i++) {
        pthread_join(tids[i], NULL);
      }
      pthread_mutex_destroy(&ctx.mu);
      free(tids);
      if (created == 0) {
        for (size_t i = 0; i < mods.len; ++i) {
          if (entry_path && ny_same_path(mods.entries[i].path, entry_path)) {
            mods.entries[i].skip_entry = true;
            continue;
          }
          if (!mods.entries[i].bundle_txt) {
            mods.entries[i].bundle_txt = ny_build_module_chunk(
                mods.entries[i].path, mods.entries[i].name, &mods.entries[i].bundle_len,
                append_module_uses);
          }
        }
      }
    } else {
      for (size_t i = 0; i < mods.len; ++i) {
        if (entry_path && ny_same_path(mods.entries[i].path, entry_path)) {
          mods.entries[i].skip_entry = true;
          continue;
        }
        if (!mods.entries[i].bundle_txt) {
          mods.entries[i].bundle_txt = ny_build_module_chunk(
              mods.entries[i].path, mods.entries[i].name, &mods.entries[i].bundle_len,
              append_module_uses);
        }
      }
    }
  }
#else
  for (size_t i = 0; i < mods.len; ++i) {
    if (entry_path && ny_same_path(mods.entries[i].path, entry_path)) {
      mods.entries[i].skip_entry = true;
      continue;
    }
    if (!mods.entries[i].bundle_txt) {
      mods.entries[i].bundle_txt = ny_build_module_chunk(mods.entries[i].path, mods.entries[i].name,
                                                         &mods.entries[i].bundle_len,
                                                         append_module_uses);
    }
  }
#endif
  size_t total = 0, cap = 4096;
  if (prebuilt_src)
    cap += strlen(prebuilt_src);
  char *bundle = malloc(cap);
  if (!bundle)
    return NULL;
  bundle[0] = '\0';
  if (prebuilt_src) {
    total = strlen(prebuilt_src);
    memcpy(bundle, prebuilt_src, total + 1);
    free(prebuilt_src);
  }
  for (size_t i = 0; i < mods.len; ++i) {
    if (mods.entries[i].skip_entry)
      continue;
    if (verbose)
      printf("Including module: %s (%s)\n", mods.entries[i].name, mods.entries[i].path);
    char *chunk = mods.entries[i].bundle_txt;
    if (chunk) {
      append_text(&bundle, &total, &cap, chunk);
      free(chunk);
      mods.entries[i].bundle_txt = NULL;
    }
  }
  for (size_t i = 0; i < mods.len; ++i) {
    free(mods.entries[i].path);
    free(mods.entries[i].name);
    for (size_t j = 0; j < mods.entries[i].deps_len; ++j)
      free(mods.entries[i].deps[j]);
    free(mods.entries[i].deps);
    if (mods.entries[i].bundle_txt)
      free(mods.entries[i].bundle_txt);
  }
  if (mods.entries)
    free(mods.entries);
  if (mods.path_ht)
    free((void *)mods.path_ht);
  if (verbose)
    NY_LOG_INFO("Loaded module bundle: %zu bytes\n", total);
  free(entry_dir);
  return bundle;
}

char *ny_build_std_source(const char **modules, size_t module_count, std_mode_t mode, int verbose,
                          const char *entry_path) {
  return ny_build_std_source_ex(modules, module_count, mode, verbose, entry_path, true);
}

static const char *std_symbol_qname(arena_t *arena, const char *pkg,
                                    const char *name) {
  if (!pkg || !*pkg || !name || !*name)
    return name;
  size_t pkg_len = strlen(pkg);
  if (strncmp(name, pkg, pkg_len) == 0 && name[pkg_len] == '.')
    return name;
  size_t len = pkg_len + 1 + strlen(name) + 1;
  char *out = arena_alloc(arena, len);
  snprintf(out, len, "%s.%s", pkg, name);
  return out;
}

static void append_c_symbol(stmt_t *s, const char *pkg, char **hdr,
                            size_t *len, size_t *capv, arena_t *scratch) {
  if (!s)
    return;
  if (s->kind == NY_S_FUNC) {
    const char *name = s->as.fn.name;
    if (name[0] == '_' && name[1] != '_')
      return;
    const char *qname = std_symbol_qname(scratch, pkg, name);
    char buf[512];
    snprintf(buf, sizeof(buf), "    {\"%s\", \"%s\"},", qname, pkg);
    append_text(hdr, len, capv, buf);
    return;
  }
  if (s->kind == NY_S_VAR) {
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      if (name[0] == '_' && name[1] != '_')
        continue;
      const char *qname = std_symbol_qname(scratch, pkg, name);
      char buf[512];
      snprintf(buf, sizeof(buf), "    {\"%s\", \"%s\"},", qname, pkg);
      append_text(hdr, len, capv, buf);
    }
    return;
  }
  if (s->kind == NY_S_MODULE) {
    const char *mod_pkg = s->as.module.name;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      append_c_symbol(s->as.module.body.data[i], mod_pkg, hdr, len, capv,
                      scratch);
    }
  }
}

char *ny_std_generate_c_symbols_header(std_mode_t mode) {
  char *bundle = ny_build_std_source_ex(NULL, 0, mode, 0, NULL, false);
  if (!bundle)
    return NULL;
  parser_t parser;
  parser_init(&parser, bundle, "<std>");
  program_t prog = parse_program(&parser);
  size_t total = 0, cap = 8192;
  char *header = malloc(cap);
  if (!header) {
    free(bundle);
    return NULL;
  }
  header[0] = '\0';
  append_text(&header, &total, &cap, "#pragma once");
  append_text(&header, &total, &cap,
              "typedef struct { const char *sym; const char *mod; } "
              "nt_std_symbol;");
  append_text(&header, &total, &cap, "static const nt_std_symbol nt_std_symbols[] = {");
  for (size_t i = 0; i < prog.body.len; ++i) {
    append_c_symbol(prog.body.data[i], "std", &header, &total, &cap,
                    parser.arena);
  }
  append_text(&header, &total, &cap, "    {0, 0}");
  append_text(&header, &total, &cap, "};");

  program_free(&prog, parser.arena);
  free(bundle);
  return header;
}
