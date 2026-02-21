#include "base/loader.h"
#include "ast/ast.h"
#include "base/common.h"
#ifdef _WIN32
#include "base/compat.h"
#endif
#include "base/util.h"
#include "parse/parser.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#define access _access
#endif

typedef struct ny_std_mod {
  char *name;    // package.module
  char *path;    // file path
  char *package; // package name
} ny_std_mod;

static ny_std_mod *ny_std_mods = NULL;
static size_t ny_std_mods_len = 0;
static size_t ny_std_mods_cap = 0;

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

static void std_push_mod(const char *name, const char *path,
                         const char *package) {
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
  name[n - 3] = '\0'; // strip .ny
  n -= 3;
  // Strip .mod suffix if present (e.g. std/io/mod.ny -> std/io)
  if (n > 4 && strcmp(name + n - 4, "/mod") == 0) {
    name[n - 4] = '\0';
    n -= 4;
  }
  for (char *p = name; *p; ++p) {
    if (*p == '/')
      *p = '.';
  }
  // Strip .lib segment if present (e.g. std.core.lib.alloc -> std.core.alloc)
  char *lib_ptr = strstr(name, ".lib.");
  if (lib_ptr) {
    size_t rest_len = strlen(lib_ptr + 5); // Skip ".lib."
    memmove(lib_ptr + 1, lib_ptr + 5,
            rest_len + 1); // Overwrite "lib." with "." + rest
  } else {
    // Check for trailing .lib (e.g. std/core/lib/mod.ny -> std.core.lib)
    size_t len = strlen(name);
    if (len > 4 && strcmp(name + len - 4, ".lib") == 0) {
      name[len - 4] = '\0';
    }
  }
  // Strip "src.std." or "src.lib." prefix if present
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
  // Prefix with std./lib. based on root to keep consistent module ids.
  const char *prefix = "";
  if (strstr(root, "std") != NULL)
    prefix = "std.";
  else if (strstr(root, "lib") != NULL)
    prefix = "lib.";
  char *final_copy = ny_loader_xmalloc(strlen(prefix) + strlen(final_name) + 1);
  strcpy(final_copy, prefix);
  strcat(final_copy, final_name);
  const char *dot = strchr(final_copy, '.');
  char *pkg = dot ? ny_strndup(final_copy, (size_t)(dot - final_copy))
                  : ny_loader_xstrdup(final_copy);
  if (!pkg)
    ny_loader_oom();
  std_push_mod(final_copy, full_path, pkg);
  free(final_copy);
  free(name);
  free(pkg);
}

static void scan_dir_recursive(const char *root, const char *dir) {
  DIR *d = opendir(dir);
  if (!d) {
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
        strcmp(ent->d_name, "test") == 0)
      continue;
    char *fp = path_join(dir, ent->d_name);
    struct stat st;
    if (stat(fp, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        scan_dir_recursive(root, fp);
      } else if (S_ISREG(st.st_mode) && is_ny_file(fp)) {
        // NY_LOG_INFO("Found module file: %s\n", fp);
        add_module_from_path(root, fp);
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

static void ny_std_init_modules(void) {
  static int init = 0;
  if (init)
    return;
  init = 1;

  const char *root = ny_src_root();
  ny_scan_std_or_lib_roots(root, "std");
  ny_scan_std_or_lib_roots(root, "lib");

  if (ny_std_mods_len > 1) {
    qsort(ny_std_mods, ny_std_mods_len, sizeof(ny_std_mod), mod_cmp);
  }
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
    qsort(ny_std_pkgs, ny_std_pkgs_len, sizeof(ny_std_pkgs[0]),
          ny_cstr_ptr_cmp);
    size_t out = 1;
    for (size_t i = 1; i < ny_std_pkgs_len; ++i) {
      if (strcmp(ny_std_pkgs[out - 1], ny_std_pkgs[i]) != 0)
        ny_std_pkgs[out++] = ny_std_pkgs[i];
    }
    ny_std_pkgs_len = out;
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

static const char *strip_std_prefix(const char *name) {
  if (!name)
    return name;
  if (strncmp(name, "std.", 4) == 0)
    return name + 4;
  return name;
}

static const char *strip_pkg_prefix(const char *name) {
  if (!name)
    return name;
  if (strncmp(name, "std.", 4) == 0)
    return name + 4;
  if (strncmp(name, "lib.", 4) == 0)
    return name + 4;
  return name;
}

static int ny_find_module_exact_name(const char *name) {
  if (!name)
    return -1;
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
  const char *tries[4] = {name, NULL, NULL, NULL};
  char buf_mod[256], buf_core[256];
  snprintf(buf_mod, sizeof(buf_mod), "%s.mod", name);
  snprintf(buf_core, sizeof(buf_core), "%s.core", name);
  tries[1] = buf_mod;
  tries[2] = buf_core;
  for (int t = 0; t < 3; t++) {
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

static int find_module_index(const char *name) {
  return ny_std_find_module_by_name(name);
}

static bool is_package_name(const char *pkg) {
  if (!pkg)
    return false;
  ny_std_init_packages();
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

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t read = fread(buf, 1, (size_t)sz, f);
  buf[read] = '\0';
  fclose(f);
  return buf;
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

static void append_fn_proto(stmt_t *s, char **hdr, size_t *len, size_t *capv) {
  if (!s)
    return;
  if (s->kind == NY_S_FUNC) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "fn %s(", s->as.fn.name);
    for (size_t j = 0; j < s->as.fn.params.len; ++j) {
      const char *sep = (j + 1 < s->as.fn.params.len) ? ", " : "";
      int written = snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s",
                             s->as.fn.params.data[j].name, sep);
      if (written > 0)
        n += written;
    }
    snprintf(buf + n, sizeof(buf) - (size_t)n, ");");
    append_text(hdr, len, capv, buf);
    return;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      append_fn_proto(s->as.module.body.data[i], hdr, len, capv);
    }
  }
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

static char *find_local_module(const char *name, const char *base_dir) {
  if (!name || !*name)
    return NULL;
  if (!base_dir || !*base_dir)
    base_dir = ".";
  size_t base_len = strlen(base_dir);
  size_t name_len = strlen(name);
  size_t path_cap = base_len + 1 + name_len + 4 + 1;
  char *path = malloc(path_cap);
  if (!path)
    return NULL;
  strcpy(path, base_dir);
  path[base_len] = '/';
  memcpy(path + base_len + 1, name, name_len + 1);
  // Replace . with / if not a file path
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

  // Remove trailing .ny if we are going to add it
  if (is_ny_file(path)) {
    path[strlen(path) - 3] = '\0';
  }

  strcat(path, ".ny");
  if (access(path, R_OK) == 0)
    return path;
  // Try name/mod.ny
  size_t mod_cap = base_len + 1 + name_len + 8 + 1;
  char *path_mod = malloc(mod_cap);
  if (!path_mod) {
    free(path);
    return NULL;
  }
  strcpy(path_mod, base_dir);
  path_mod[base_len] = '/';
  memcpy(path_mod + base_len + 1, name, name_len + 1);
  for (char *p = path_mod; *p; ++p)
    if (*p == '.')
      *p = '/';
  strcat(path_mod, "/mod.ny");
  if (access(path_mod, R_OK) == 0) {
    free(path);
    return path_mod;
  }
  free(path);
  free(path_mod);
  return NULL;
}

static char *resolve_module_path(const char *raw, const char *base_dir,
                                 bool prefer_local, bool *is_std_out) {
  if (!raw)
    return NULL;
  bool explicit_std = (strncmp(raw, "std.", 4) == 0);
  bool explicit_lib = (strncmp(raw, "lib.", 4) == 0);
  bool explicit_pkg = explicit_std || explicit_lib;
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
} mod_entry;

typedef struct {
  mod_entry *entries;
  size_t len;
  size_t cap;
  bool skip_std;
} mod_list;

static int mod_priority(const char *path) {
  if (!path)
    return 100;
  if (strstr(path, "std/core/base.ny"))
    return 0;
  if (strstr(path, "std/core/mod.ny"))
    return 1;
  if (strstr(path, "std/core/primitives.ny"))
    return 1;
  if (strstr(path, "std/core/reflect.ny"))
    return 2;
  return 100;
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

static void mod_list_add(mod_list *list, const char *path, const char *name,
                         bool is_std) {
  for (size_t i = 0; i < list->len; ++i) {
    if (strcmp(list->entries[i].path, path) == 0)
      return;
  }
  if (list->len == list->cap) {
    size_t new_cap = list->cap ? list->cap * 2 : 16;
    list->entries =
        ny_loader_xrealloc(list->entries, new_cap * sizeof(mod_entry));
    list->cap = new_cap;
  }
  list->entries[list->len++] = (mod_entry){.path = ny_loader_xstrdup(path),
                                           .name = ny_loader_xstrdup(name),
                                           .processed = false,
                                           .is_std = is_std};
}

static void scan_stmt_uses(mod_list *list, stmt_t *s, const char *base_dir,
                           bool prefer_local) {
  if (!s)
    return;
  if (s->kind == NY_S_USE) {
    const char *raw = s->as.use.module;
    bool explicit_std =
        (strcmp(raw, "std") == 0) || (strncmp(raw, "std.", 4) == 0);
    bool explicit_lib =
        (strcmp(raw, "lib") == 0) || (strncmp(raw, "lib.", 4) == 0);
    bool explicit_pkg = explicit_std || explicit_lib;
    bool is_std = false;
    char *path = resolve_module_path(raw, base_dir, prefer_local, &is_std);
    if (path) {
      if (!(list->skip_std && is_std)) {
        char *mname = is_std ? (char *)raw : ny_modname_from_path(path);
        mod_list_add(list, path, mname, is_std);
        if (!is_std)
          free(mname);
      }
      free(path);
      return;
    }
    if (strcmp(raw, "std") == 0) {
      return;
    }
    if ((!prefer_local || explicit_std) && (explicit_std || !explicit_pkg)) {
      const char *pkg_name = strip_pkg_prefix(raw);
      if (is_package_name(pkg_name)) {
        if (list->skip_std)
          return;
        ny_std_init_modules();
        for (size_t k = 0; k < ny_std_mods_len; ++k) {
          if (strcmp(ny_std_mods[k].package, pkg_name) == 0) {
            mod_list_add(list, ny_std_mods[k].path, ny_std_mods[k].name, true);
          }
        }
      }
    }
    return;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      scan_stmt_uses(list, s->as.module.body.data[i], base_dir, prefer_local);
    }
  }
}

static void scan_dependencies(mod_list *list, size_t idx) {
  if (list->entries[idx].processed)
    return;
  NY_LOG_V2("Scanning dependencies for %s\n", list->entries[idx].path);
  list->entries[idx].processed = true;
  char *txt = read_file(list->entries[idx].path);
  if (!txt) {
    NY_LOG_V2("Failed to read file: %s\n", list->entries[idx].path);
    return;
  }
  NY_LOG_V3("Parsing file: %s (size: %zu)\n", list->entries[idx].path,
            strlen(txt));
  parser_t parser;
  parser_init(&parser, txt, list->entries[idx].path);
  // We only parse, we don't care about errors here much, just grabbing 'use'
  // But we need a valid program_t structure to find 'use' statements safely
  program_t prog = parse_program(&parser);
  char *base_dir = dir_from_path(list->entries[idx].path);
  bool prefer_local = !list->entries[idx].is_std;
  for (size_t i = 0; i < prog.body.len; ++i) {
    scan_stmt_uses(list, prog.body.data[i], base_dir, prefer_local);
  }
  program_free(&prog, parser.arena);
  free(base_dir);
  free(txt);
}

char *ny_build_std_bundle(const char **modules, size_t module_count,
                          std_mode_t mode, int verbose,
                          const char *entry_path) {
  NY_LOG_V1("Building standard library bundle (mode=%d, count=%zu)\n", mode,
            module_count);
  if (mode == STD_MODE_NONE && module_count == 0)
    return NULL;
  ny_std_init_modules();
  // mode or artifacts only)
  char *prebuilt_src = NULL;
  if (ny_std_find_module_by_name("std.core.mod") < 0) {
    const char *prebuilt = getenv("NYTRIX_STD_PREBUILT");
    if (!prebuilt || access(prebuilt, R_OK) != 0) {
      if (access("build/std.ny", R_OK) == 0) {
        prebuilt = "build/std.ny";
      }
    }
    if (prebuilt && access(prebuilt, R_OK) == 0) {
      if (verbose)
        printf("Using prebuilt standard library: %s\n", prebuilt);
      prebuilt_src = read_file(prebuilt);
    }
  }
  mod_list mods = {0};
  mods.skip_std = (mode == STD_MODE_NONE);
  char *entry_dir = entry_path ? dir_from_path(entry_path) : NULL;
  // 1. Seed the list
  if (mode == STD_MODE_FULL) {
    for (size_t i = 0; i < ny_std_mods_len; ++i) {
      mod_list_add(&mods, ny_std_mods[i].path, ny_std_mods[i].name, true);
    }
  } else {
    const char **seed_modules = modules;
    size_t seed_count = module_count;

    // If no explicit imports were discovered, fall back to full std seeding.
    // This mirrors prebuilt-bundle behavior and keeps scripts with only
    // nested/conditional uses working when prebuilt loading is unavailable.
    if (seed_count == 0 && mode != STD_MODE_NONE) {
      for (size_t j = 0; j < ny_std_mods_len; ++j) {
        mod_list_add(&mods, ny_std_mods[j].path, ny_std_mods[j].name, true);
      }
    }

    for (size_t i = 0; i < seed_count; ++i) {
      const char *raw = seed_modules[i];
      const char *name = strip_std_prefix(raw);
      if (strcmp(name, "std") == 0) {
        // use std -> full std
        for (size_t j = 0; j < ny_std_mods_len; ++j) {
          mod_list_add(&mods, ny_std_mods[j].path, ny_std_mods[j].name, true);
        }
        continue;
      }
      // Try directory/package for std
      if (is_package_name(name)) {
        for (size_t k = 0; k < ny_std_mods_len; ++k) {
          if (strcmp(ny_std_mods[k].package, name) == 0) {
            mod_list_add(&mods, ny_std_mods[k].path, ny_std_mods[k].name, true);
          }
        }
        continue;
      }
      bool is_std = false;
      char *path =
          resolve_module_path(raw, entry_dir ? entry_dir : ".", true, &is_std);
      if (path) {
        char *mname = is_std ? (char *)raw : ny_modname_from_path(path);
        mod_list_add(&mods, path, mname, is_std);
        if (!is_std)
          free(mname);
        free(path);
      } else {
        if (verbose)
          NY_LOG_ERR("Module not found: %s\n", raw);
      }
    }
  }
  // 2. Scan dependencies iteratively
  bool changed = true;
  while (changed) {
    changed = false;
    size_t current_len = mods.len;
    for (size_t i = 0; i < current_len; ++i) {
      if (!mods.entries[i].processed) {
        scan_dependencies(&mods, i);
        // constraint? We used 'current_len' so new elements are processed in
        if (mods.len > current_len)
          changed = true;
      }
    }
    // If we processed items without adding new ones, we might still have
    // appended items. Actually, simpler:
  }
  for (size_t i = 0; i < mods.len; ++i) {
    if (!mods.entries[i].processed) {
      scan_dependencies(&mods, i);
    }
  }
  // Stabilize module order to avoid dependency-order override quirks.
  if (mods.len > 1) {
    qsort(mods.entries, mods.len, sizeof(mod_entry), mod_entry_path_cmp);
  }

  // If no modules to bundle and no prebuilt source, nothing to do.
  if (mods.len == 0 && !prebuilt_src) {
    free(entry_dir);
    if (mods.entries)
      free(mods.entries);
    return NULL;
  }

  // 3. Build bundle
  size_t total = 0, cap = 4096;
  if (prebuilt_src)
    cap += strlen(prebuilt_src);
  char *bundle = malloc(cap);
  if (!bundle)
    return NULL;
  bundle[0] = '\0';
  if (prebuilt_src) {
    strcpy(bundle, prebuilt_src);
    total = strlen(bundle);
    free(prebuilt_src);
  }
  for (size_t i = 0; i < mods.len; ++i) {
    if (verbose)
      printf("Including module: %s (%s)\n", mods.entries[i].name,
             mods.entries[i].path);
    char *txt = read_file(mods.entries[i].path);
    if (txt) {
      bool has_decl = false;
      const char *p = txt;
      while (*p) {
        while (*p && isspace(*p))
          p++;
        if (!*p)
          break;
        if (*p == ';' || *p == '#') {
          while (*p && *p != '\n')
            p++;
          continue;
        }
        if (strncmp(p, "module", 6) == 0 && (isspace(p[6]) || p[6] == '\0')) {
          has_decl = true;
          break;
        }
        // If we see anything else (use, fn, etc), assume no module decl at top
        // implies implicit wrapper needed? Actually 'use' is allowed before
        // 'module'.
        if (strncmp(p, "use", 3) == 0 && (isspace(p[3]) || p[3] == '\0')) {
          while (*p && *p != '\n')
            p++;
          continue;
        }
        // Any other token_t -> stop looking
        break;
      }
      if (has_decl) {
        append_text(&bundle, &total, &cap, txt);
        append_text(&bundle, &total, &cap, "\n");
      } else {
        // Wrap all modules in their namespace, defaulting to export all (*)
        char *wrapped = malloc(strlen(txt) + strlen(mods.entries[i].name) + 64);
        sprintf(wrapped, "module %s * {\n%s\n}", mods.entries[i].name, txt);
        append_text(&bundle, &total, &cap, wrapped);
        free(wrapped);
      }
      free(txt);
    }
  }
  // Cleanup
  for (size_t i = 0; i < mods.len; ++i) {
    free(mods.entries[i].path);
    free(mods.entries[i].name);
  }
  if (mods.entries)
    free(mods.entries);
  if (verbose)
    NY_LOG_INFO("Loaded module bundle: %zu bytes\n", total);
  free(entry_dir);
  return bundle;
}

char *ny_std_generate_header(std_mode_t mode) {
  char *bundle = ny_build_std_bundle(NULL, 0, mode, 0, NULL);
  if (!bundle)
    return NULL;
  parser_t parser;
  parser_init(&parser, bundle, "<std_bundle>");
  program_t prog = parse_program(&parser);
  size_t total = 0, cap = 4096;
  char *header = malloc(cap);
  if (!header) {
    free(bundle);
    return NULL;
  }
  header[0] = '\0';
  for (size_t i = 0; i < prog.body.len; ++i) {
    append_fn_proto(prog.body.data[i], &header, &total, &cap);
  }
  program_free(&prog, parser.arena);
  free(bundle);
  if (getenv("NYTRIX_DUMP_HEADER")) {
    char dump_path[4096];
    ny_join_path(dump_path, sizeof(dump_path), ny_get_temp_dir(),
                 "nytrix_std_header.ny");
    FILE *df = fopen(dump_path, "w");
    if (df) {
      fputs(header, df);
      fclose(df);
    }
  }
  return header;
}
