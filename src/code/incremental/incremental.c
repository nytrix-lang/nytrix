/*
 * Incremental compilation: file-fingerprint caching, dependency-graph
 * construction, change detection, and transitive recompile-set computation.
 */
#include "code/incremental/incremental.h"
#include "base/common.h"
#include "base/util.h"
#include "base/time.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/*
 * Incremental compilation:
 *
 *  - Stable file fingerprint (content hash + mtime_nsec + size + ino + dev).
 *  - Dependency graph captured per source file (imports / includes).
 *  - Persistent graph serialization so a later run only recompiles changed
 *    files plus their transitive dependents.
 *  - Cache invalidation protocol that removes stale artifacts for exactly
 *    the files whose fingerprints changed.
 *
 * Nothing here compiles code itself; it decides *what* to recompile and
 * *what cache entries to drop*, leaving the existing pipeline to do the work.
 */

#define NY_INC_GRAPH_MAGIC 0x6e79696e63ULL     /* "nyinc" */
#define NY_INC_GRAPH_VERSION 1u

static const ny_incremental_config_t g_default_config = {
    .enable_incremental = true, .track_system_deps = false, .max_depth = 32};
static ny_incremental_config_t g_config;
static bool g_initialized = false;

/*
 * small dynamic string-array helpers
 */

static bool strlist_push(char ***list, size_t *len, size_t *cap, const char *s) {
  if (*len == *cap) {
    size_t nc = *cap ? *cap * 2 : 8;
    char **p = realloc(*list, nc * sizeof(**list));
    if (!p)
      return false;
    *list = p;
    *cap = nc;
  }
  (*list)[(*len)++] = ny_strdup(s);
  return (*list)[(*len)-1] != NULL;
}

static void strlist_free(char **list, size_t len) {
  for (size_t i = 0; i < len; ++i)
    free(list[i]);
  free(list);
}

/*
 * initialization
 */

bool ny_incremental_init(const ny_incremental_config_t *config) {
  g_config = config ? *config : g_default_config;
  if (!g_config.max_depth)
    g_config.max_depth = 32;
  if (config && config->project_root)
    g_config.project_root = ny_strdup(config->project_root);
  if (config && config->cache_dir)
    g_config.cache_dir = ny_strdup(config->cache_dir);
  g_initialized = true;
  return true;
}

void ny_incremental_shutdown(void) {
  free((char *)g_config.project_root);
  free((char *)g_config.cache_dir);
  g_config = g_default_config;
  g_initialized = false;
}

/*
 * stable file fingerprint
 */

bool ny_incremental_file_fingerprint(const char *file_path,
                                     ny_file_fingerprint_t *out_fingerprint) {
  if (!file_path || !*file_path || !out_fingerprint)
    return false;
  memset(out_fingerprint, 0, sizeof(*out_fingerprint));
  struct stat st;
  if (stat(file_path, &st) != 0)
    return false;
  if (!S_ISREG(st.st_mode))
    return false;
  size_t len = 0;
  char *content = ny_read_file_raw(file_path, &len);
  if (!content)
    return false;
  out_fingerprint->content_hash = ny_hash64(content, len);
  free(content);
  out_fingerprint->mtime_nsec = ny_stat_mtime_nsec(&st);
  out_fingerprint->size = (uint64_t)st.st_size;
#ifndef _WIN32
  out_fingerprint->ino = (uint64_t)st.st_ino;
  out_fingerprint->dev = (uint64_t)st.st_dev;
#else
  out_fingerprint->ino = 0;
  out_fingerprint->dev = 0;
#endif
  return true;
}

static bool fingerprint_equal(const ny_file_fingerprint_t *a,
                              const ny_file_fingerprint_t *b) {
  return a->content_hash == b->content_hash && a->size == b->size &&
         a->mtime_nsec == b->mtime_nsec && a->ino == b->ino &&
         a->dev == b->dev;
}

/*
 * dependency extraction
 */

/*
 * Scan a source buffer for `use` / `import` / `include` of other modules.
 * Deliberately conservative: any token formed of identifier/path characters
 * after "use"/"import" becomes a candidate dependency; the driver resolves
 * against the file set later.
 */
bool ny_incremental_parse_dependencies(const char *file_path,
                                       const char *source, size_t source_len,
                                       char ***out_deps,
                                       size_t *out_dep_count) {
  (void)file_path;
  if (!out_deps || !out_dep_count)
    return false;
  *out_deps = NULL;
  *out_dep_count = 0;
  if (!source)
    return true;
  size_t cap = 0;
  const char *s = source;
  const char *end = source + source_len;
  /*
   * Tokenize to find keywords "use" / "import" outside comments/strings.
   */
  enum { NORMAL, LINE_COMMENT, BLOCK_COMMENT } state = NORMAL;
  while (s < end) {
    char c = *s;
    if (state == LINE_COMMENT) {
      if (c == '\n')
        state = NORMAL;
      s++;
      continue;
    }
    if (state == BLOCK_COMMENT) {
      if (c == '*' && s + 1 < end && s[1] == '/') {
        state = NORMAL;
        s += 2;
      } else {
        s++;
      }
      continue;
    }
    /*
     * cheap string handling: skip quoted regions
     */
    if (c == '"' || c == '\'') {
      char q = c;
      s++;
      while (s < end && *s != q) {
        if (*s == '\\' && s + 1 < end)
          s++;
        s++;
      }
      if (s < end)
        s++;
      continue;
    }
    if (c == '/' && s + 1 < end && s[1] == '/') {
      state = LINE_COMMENT;
      s += 2;
      continue;
    }
    if (c == '/' && s + 1 < end && s[1] == '*') {
      state = BLOCK_COMMENT;
      s += 2;
      continue;
    }
    if (c == '\n' || c == '\r' || c == ';') {
      state = NORMAL;
      s++;
      continue;
    }
    if (!(isalpha((unsigned char)c) || c == '_')) {
      s++;
      continue;
    }
    /*
     * read an identifier/path token
     */
    const char *tok = s;
    while (s < end && (isalnum((unsigned char)*s) || *s == '_' || *s == '.' ||
                       *s == '/'))
      s++;
    size_t tok_len = (size_t)(s - tok);
    if (tok_len == 3 && strncmp(tok, "use", 3) == 0) {
      /*
       * scan following token
       */
      const char *p = s;
      while (p < end && isspace((unsigned char)*p))
        p++;
      const char *mod = p;
      size_t dep_len = 0;
      if (p < end && (*p == '"' || *p == '\'')) {
        /*
         * quoted module path: `use "./provider.ny"`
         */
        char q = *p++;
        const char *inner = p;
        while (p < end && *p != q) {
          if (*p == '\\' && p + 1 < end)
            p++;
          p++;
        }
        if (p >= end)
          break;
        dep_len = (size_t)(p - inner);
        mod = inner;
        p++; /* closing quote */
      } else {
        while (p < end && (isalnum((unsigned char)*p) || *p == '_' ||
                           *p == '.' || *p == '/'))
          p++;
        dep_len = (size_t)(p - mod);
      }
      if (dep_len > 0) {
        char *dep = (char *)malloc(dep_len + 1);
        if (dep) {
          memcpy(dep, mod, dep_len);
          dep[dep_len] = '\0';
          /*
           * dedupe + skip std
           */
          if (!(strncmp(dep, "std", 3) == 0)) {
            bool dup = false;
            for (size_t k = 0; k < *out_dep_count; ++k)
              if (strcmp((*out_deps)[k], dep) == 0) {
                dup = true;
                break;
              }
            if (!dup)
              strlist_push(out_deps, out_dep_count, &cap, dep);
          }
          free(dep);
        }
        s = p;
        continue;
      }
    }
  }
  return true;
}

/*
 * forward for node add
 */
static ny_dep_node_t *graph_find_node(ny_dep_graph_t *g, const char *file_path,
                                      uint32_t hash);

/*
 * graph build
 */

static ny_dep_node_t *graph_add_or_get(ny_dep_graph_t *g,
                                       const char *file_path) {
  if (!g || !file_path || !*file_path)
    return NULL;
  uint64_t h = ny_hash64_cstr(file_path);
  ny_dep_node_t *n = graph_find_node(g, file_path, (uint32_t)h);
  if (n)
    return n;
  n = calloc(1, sizeof(*n));
  if (!n)
    return NULL;
  n->file_path = ny_strdup(file_path);
  g->node_count++;
  n->next = g->nodes;
  g->nodes = n;
  return n;
}

static ny_dep_node_t *graph_find_node(ny_dep_graph_t *g, const char *file_path,
                                      uint32_t hash) {
  (void)hash;
  if (!g || !file_path)
    return NULL;
  for (ny_dep_node_t *n = g->nodes; n; n = n->next) {
    if (n->file_path && strcmp(n->file_path, file_path) == 0)
      return n;
  }
  return NULL;
}

static bool graph_add_edge(ny_dep_graph_t *g, const char *from,
                           const char *to) {
  if (!g || !from || !to)
    return false;
  ny_dep_node_t *fn = graph_add_or_get(g, from);
  ny_dep_node_t *tn = graph_add_or_get(g, to);
  if (!fn || !tn)
    return false;
  if (fn == tn)
    return true;
  /*
   * dedupe outgoing edges
   */
  for (ny_dep_edge_t *e = fn->deps; e; e = e->next)
    if (strcmp(e->to_file, to) == 0)
      return true;
  ny_dep_edge_t *fe = calloc(1, sizeof(*fe));
  if (!fe)
    return false;
  fe->to_file = ny_strdup(to);
  fe->from_file = ny_strdup(from);
  fe->next = fn->deps;
  fn->deps = fe;
  ny_dep_edge_t *re = calloc(1, sizeof(*re));
  if (!re)
    return true;
  re->from_file = ny_strdup(from); /* dependent */
  re->to_file = ny_strdup(to);     /* dependency this node satisfies */
  re->next = tn->rdeps;
  tn->rdeps = re;
  return true;
}

/*
 * Compare a dependency spelling (e.g. "./provider.ny") against a candidate
 * source path by their final path components, so relative `use` names resolve
 * regardless of lead-"./" or directory nesting.
 */
static bool source_basename_matches(const char *cand, const char *dep) {
  if (!cand || !dep || !*cand || !*dep)
    return false;
  /*
   * normalize: strip leading "./"
   */
  if (dep[0] == '.' && dep[1] == '/')
    dep += 2;
  const char *cl = strrchr(cand, '/');
  cl = cl ? cl + 1 : cand;
  const char *dl = strrchr(dep, '/');
  dl = dl ? dl + 1 : dep;
  if (strcmp(cl, dl) == 0)
    return true;
  /*
   * also match ignoring the ".ny" extension
   */
  const char *cb = strrchr(cl, '.');
  const char *db = strrchr(dl, '.');
  bool cdot = cb && strcmp(cb, ".ny") == 0;
  bool ddot = db && strcmp(db, ".ny") == 0;
  if (cdot != ddot)
    return false;
  size_t clen = cdot ? (size_t)(cb - cl) : strlen(cl);
  size_t dlen = ddot ? (size_t)(db - dl) : strlen(dl);
  return clen == dlen && strncmp(cl, dl, clen) == 0;
}

bool ny_incremental_build_graph(const char **source_files, size_t file_count,
                                ny_dep_graph_t *out_graph) {
  if (!out_graph)
    return false;
  memset(out_graph, 0, sizeof(*out_graph));
  if ((!source_files || file_count == 0))
    return true;
  /*
   * create all nodes first
   */
  for (size_t i = 0; i < file_count; ++i)
    graph_add_or_get(out_graph, source_files[i]);
  /*
   * parse each file for deps
   */
  for (size_t i = 0; i < file_count; ++i) {
    const char *fp = source_files[i];
    size_t len = 0;
    char *content = ny_read_file_raw(fp, &len);
    if (!content)
      continue;
    char **deps = NULL;
    size_t dep_count = 0;
    if (ny_incremental_parse_dependencies(fp, content, len, &deps,
                                          &dep_count)) {
      for (size_t d = 0; d < dep_count; ++d) {
        /*
         * resolve name to a file in the set: match the absolute path, its
         * source spelling (e.g. "./provider.ny"), or the stripped basename.
         */
        for (size_t j = 0; j < file_count; ++j) {
          const char *cand = source_files[j];
          if (strcmp(cand, deps[d]) == 0 ||
              ny_name_tail_is(cand, deps[d]) ||
              source_basename_matches(cand, deps[d])) {
            graph_add_edge(out_graph, fp, cand);
            break;
          }
        }
      }
    }
    strlist_free(deps, dep_count);
    free(content);
  }
  /*
   * fill fingerprints for existing files
   */
  for (ny_dep_node_t *n = out_graph->nodes; n; n = n->next) {
    ny_file_fingerprint_t fp = {0};
    if (ny_incremental_file_fingerprint(n->file_path, &fp))
      n->fingerprint = fp;
  }
  return true;
}

/*
 * graph serialization
 */

static bool graph_write_u64(FILE *f, uint64_t v) {
  return fwrite(&v, sizeof(v), 1, f) == 1;
}
static bool graph_write_u32(FILE *f, uint32_t v) {
  return fwrite(&v, sizeof(v), 1, f) == 1;
}
static bool graph_read_u64(FILE *f, uint64_t *v) {
  return fread(v, sizeof(*v), 1, f) == 1;
}
static bool graph_read_u32(FILE *f, uint32_t *v) {
  return fread(v, sizeof(*v), 1, f) == 1;
}
static bool graph_write_cstr(FILE *f, const char *s) {
  uint32_t len = (uint32_t)(s ? strlen(s) : 0);
  if (!graph_write_u32(f, len))
    return false;
  if (len && fwrite(s, 1, len, f) != len)
    return false;
  return true;
}
static char *graph_read_cstr(FILE *f) {
  uint32_t len = 0;
  if (!graph_read_u32(f, &len) || len > 1u << 20)
    return NULL;
  char *s = malloc((size_t)len + 1);
  if (!s)
    return NULL;
  if (len && fread(s, 1, len, f) != len) {
    free(s);
    return NULL;
  }
  s[len] = '\0';
  return s;
}
static bool graph_write_fp(FILE *f, const ny_file_fingerprint_t *fp) {
  return graph_write_u64(f, fp->content_hash) &&
         graph_write_u64(f, fp->mtime_nsec) &&
         graph_write_u64(f, fp->size) && graph_write_u64(f, fp->ino) &&
         graph_write_u64(f, fp->dev);
}
static bool graph_read_fp(FILE *f, ny_file_fingerprint_t *fp) {
  return graph_read_u64(f, &fp->content_hash) &&
         graph_read_u64(f, &fp->mtime_nsec) &&
         graph_read_u64(f, &fp->size) && graph_read_u64(f, &fp->ino) &&
         graph_read_u64(f, &fp->dev);
}

bool ny_incremental_save_graph(const ny_dep_graph_t *graph,
                               const char *cache_path) {
  if (!graph || !cache_path || !*cache_path)
    return false;
  char dir[1024];
  ny_dir_name(dir, sizeof(dir), cache_path);
  ny_ensure_dir_recursive(dir);
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", cache_path, (long)getpid());
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return false;
  bool ok = graph_write_u64(f, NY_INC_GRAPH_MAGIC) &&
            graph_write_u32(f, NY_INC_GRAPH_VERSION) &&
            graph_write_u64(f, graph->graph_hash) &&
            graph_write_cstr(f, graph->project_root) &&
            graph_write_u32(f, (uint32_t)graph->node_count);
  size_t written_nodes = 0;
  for (ny_dep_node_t *n = graph->nodes; ok && n; n = n->next) {
    ok = graph_write_cstr(f, n->file_path) && graph_write_fp(f, &n->fingerprint);
    /*
     * outgoing edges
     */
    uint32_t ed = 0;
    for (ny_dep_edge_t *e = n->deps; e; e = e->next)
      ed++;
    ok = ok && graph_write_u32(f, ed);
    for (ny_dep_edge_t *e = n->deps; ok && e; e = e->next)
      ok = graph_write_cstr(f, e->to_file);
    written_nodes++;
  }
  ok = ok && written_nodes == graph->node_count;
  if (fclose(f) != 0)
    ok = false;
  if (ok && rename(tmp, cache_path) != 0)
    ok = false;
  if (!ok)
    remove(tmp);
  return ok;
}

bool ny_incremental_load_graph(const char *cache_path,
                               ny_dep_graph_t *out_graph) {
  if (!out_graph)
    return false;
  memset(out_graph, 0, sizeof(*out_graph));
  if (!cache_path || !*cache_path)
    return false;
  FILE *f = fopen(cache_path, "rb");
  if (!f)
    return false;
  uint64_t magic = 0, ghash = 0;
  uint32_t version = 0, node_count = 0;
  bool ok = graph_read_u64(f, &magic) && graph_read_u32(f, &version) &&
            graph_read_u64(f, &ghash) && version == NY_INC_GRAPH_VERSION &&
            magic == NY_INC_GRAPH_MAGIC;
  out_graph->graph_hash = ghash;
  char *proot = graph_read_cstr(f);
  if (!ok)
    free(proot);
  else
    out_graph->project_root = proot;
  ok = ok && graph_read_u32(f, &node_count);
  for (uint32_t i = 0; ok && i < node_count; ++i) {
    char *path = graph_read_cstr(f);
    ny_file_fingerprint_t fp = {0};
    ok = path && graph_read_fp(f, &fp);
    if (ok) {
      ny_dep_node_t *n = graph_add_or_get(out_graph, path);
      if (!n)
        ok = false;
      else
        n->fingerprint = fp;
    }
    free(path);
    if (ok) {
      uint32_t ed = 0;
      ok = graph_read_u32(f, &ed);
      for (uint32_t k = 0; ok && k < ed; ++k) {
        char *to = graph_read_cstr(f);
        ok = to && graph_add_edge(out_graph, path ? path : "", to);
        free(to);
      }
    }
  }
  fclose(f);
  if (!ok) {
    ny_incremental_free_graph(out_graph);
    return false;
  }
  return true;
}

void ny_incremental_free_graph(ny_dep_graph_t *graph) {
  if (!graph)
    return;
  for (ny_dep_node_t *n = graph->nodes; n;) {
    ny_dep_node_t *nn = n->next;
    free(n->file_path);
    for (ny_dep_edge_t *e = n->deps; e;) {
      ny_dep_edge_t *ne = e->next;
      free(e->from_file);
      free(e->to_file);
      free(e);
      e = ne;
    }
    for (ny_dep_edge_t *e = n->rdeps; e;) {
      ny_dep_edge_t *ne = e->next;
      free(e->from_file);
      free(e->to_file);
      free(e);
      e = ne;
    }
    free(n);
    n = nn;
  }
  free(graph->project_root);
  memset(graph, 0, sizeof(*graph));
}

/*
 * change detection
 */

bool ny_incremental_check_changes(ny_dep_graph_t *graph, char ***out_changed,
                                  size_t *out_changed_count) {
  if (!graph || !out_changed || !out_changed_count)
    return false;
  *out_changed = NULL;
  *out_changed_count = 0;
  size_t cap = 0;
  for (ny_dep_node_t *n = graph->nodes; n; n = n->next) {
    if (!n->file_path)
      continue;
    ny_file_fingerprint_t cur = {0};
    if (ny_incremental_file_fingerprint(n->file_path, &cur)) {
      if (!fingerprint_equal(&cur, &n->fingerprint))
        if (!strlist_push(out_changed, out_changed_count, &cap, n->file_path))
          return false;
    }
  }
  return true;
}

typedef struct nk_queue_t {
  ny_dep_node_t **items;
  size_t len, cap;
} nk_queue_t;

static void nkq_push(nk_queue_t *q, ny_dep_node_t *n) {
  if (q->len == q->cap) {
    q->cap = q->cap ? q->cap * 2 : 16;
    q->items = realloc(q->items, q->cap * sizeof(*q->items));
  }
  if (q->items)
    q->items[q->len++] = n;
}

bool ny_incremental_compute_recompile_set(ny_dep_graph_t *graph,
                                          const char **changed_files,
                                          size_t changed_count,
                                          char ***out_recompile,
                                          size_t *out_recompile_count) {
  if (!graph || !out_recompile || !out_recompile_count)
    return false;
  *out_recompile = NULL;
  *out_recompile_count = 0;
  size_t cap = 0;
  nk_queue_t q = {0};
  /*
   * seed queue with changed files; mark each node dirty once
   */
  for (size_t i = 0; i < changed_count; ++i) {
    const char *cf = changed_files[i];
    ny_dep_node_t *n = graph_find_node(graph, cf, 0);
    if (n && !n->needs_recompile) {
      n->needs_recompile = true;
      nkq_push(&q, n);
    }
  }
  while (q.len) {
    ny_dep_node_t *n = q.items[--q.len];
    if (!n->file_path)
      continue;
    strlist_push(out_recompile, out_recompile_count, &cap, n->file_path);
    /*
     * dependents
     */
    for (ny_dep_edge_t *e = n->rdeps; e; e = e->next) {
      ny_dep_node_t *dep = graph_find_node(graph, e->from_file, 0);
      if (dep && !dep->needs_recompile) {
        dep->needs_recompile = true;
        nkq_push(&q, dep);
      }
    }
  }
  free(q.items);
  /*
   * reset markers
   */
  for (ny_dep_node_t *n = graph->nodes; n; n = n->next)
    n->needs_recompile = false;
  return true;
}

/*
 * cache invalidation
 */

bool ny_incremental_invalidate_cache(const ny_dep_graph_t *graph,
                                     const char **recompile_files,
                                     size_t recompile_count) {
  if (!graph || !recompile_files)
    return recompile_count == 0;
  const char *cache_dir = g_config.cache_dir;
  if (!cache_dir || !*cache_dir)
    return true; /* nothing to invalidate if no cache dir configured */
  bool ok = true;
  for (size_t i = 0; i < recompile_count; ++i) {
    const char *file = recompile_files[i];
    /*
     * Derived artifact pattern: <cache>/<hash>.{bc,so}
     */
    uint64_t h = ny_hash64_cstr(file);
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s/%016llx", cache_dir,
             (unsigned long long)h);
    /*
     * Remove bitcode and native siblings
     */
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s.bc", base);
    if (remove(p) == 0)
      ok = true;
    snprintf(p, sizeof(p), "%s.ll", base);
    if (remove(p) == 0)
      ok = true;
    snprintf(p, sizeof(p), "%s.so", base);
    if (remove(p) == 0)
      ok = true;
    snprintf(p, sizeof(p), "%s.manifest", base);
    remove(p);
  }
  return ok;
}

/*
 * top-level incremental compile
 */

ny_incremental_result_t
ny_incremental_compile(const char **source_files, size_t file_count,
                       const ny_incremental_config_t *config) {
  ny_incremental_result_t res = {0};
  ny_tick_t t0 = ny_ticks_now();
  if (!config)
    return res;
  if (!g_initialized) {
    /*
     * accept a config passed directly to the driver
     */
    g_config = config ? *config : g_default_config;
    if (config && config->project_root)
      g_config.project_root = ny_strdup(config->project_root);
    if (config && config->cache_dir)
      g_config.cache_dir = ny_strdup(config->cache_dir);
    g_initialized = true;
  }
  if (!config->enable_incremental || config->force_full_rebuild ||
      file_count == 0) {
    res.success = true;
    return res;
  }
  char graph_path[PATH_MAX];
  if (g_config.cache_dir && *g_config.cache_dir) {
    snprintf(graph_path, sizeof(graph_path), "%s/incgraph.bin",
             g_config.cache_dir);
  } else {
    snprintf(graph_path, sizeof(graph_path), "%s/incgraph.bin",
             ny_default_cache_root_dir());
  }
  /*
   * load prior graph (if any)
   */
  ny_dep_graph_t old = {0};
  ny_incremental_load_graph(graph_path, &old);
  /*
   * build current graph
   */
  ny_dep_graph_t cur = {0};
  if (!ny_incremental_build_graph(source_files, file_count, &cur)) {
    ny_incremental_free_graph(&old);
    return res;
  }
  /*
   * find files shared between old and current to detect changes
   */
  char **changed = NULL;
  size_t changed_count = 0;
  size_t cap = 0;
  for (ny_dep_node_t *n = old.nodes; n; n = n->next) {
    ny_dep_node_t *cur_n = graph_find_node(&cur, n->file_path, 0);
    if (!cur_n)
      continue; /* removed file */
    ny_file_fingerprint_t curfp = {0};
    if (ny_incremental_file_fingerprint(n->file_path, &curfp)) {
      if (!fingerprint_equal(&curfp, &n->fingerprint))
        if (!strlist_push(&changed, &changed_count, &cap, n->file_path))
          break;
    }
  }
  /*
   * If a graph already existed, newly-added files also count as changed.
   */
  if (old.node_count > 0) {
    for (ny_dep_node_t *n = cur.nodes; n; n = n->next) {
      if (!graph_find_node(&old, n->file_path, 0))
        if (!strlist_push(&changed, &changed_count, &cap, n->file_path))
          break;
    }
  }
  /*
   * compute transitive dependents
   */
  char **rset = NULL;
  size_t rset_count = 0;
  ny_incremental_compute_recompile_set(&cur, (const char **)changed,
                                       changed_count, &rset, &rset_count);
  strlist_free(changed, changed_count);
  /*
   * publish recompile set
   */
  res.recompile_files = rset;
  res.recompile_count = rset_count;
  /*
   * invalidate any cached artifacts for the recompile set
   */
  if (rset_count > 0)
    ny_incremental_invalidate_cache(&cur, (const char **)rset, rset_count);
  /*
   * save updated graph so the next run diff is against this state
   */
  ny_incremental_save_graph(&cur, graph_path);
  ny_incremental_free_graph(&old);
  ny_incremental_free_graph(&cur);
  res.success = true;
  res.elapsed_ms = ny_ticks_elapsed_ms(t0);
  return res;
}

void ny_incremental_free_result(ny_incremental_result_t *result) {
  if (!result)
    return;
  strlist_free(result->recompile_files, result->recompile_count);
  strlist_free(result->invalidated_cache, result->invalidated_count);
  free(result->error_message);
  memset(result, 0, sizeof(*result));
}

void ny_incremental_dump_graph(const ny_dep_graph_t *graph, FILE *out) {
  if (!graph || !out)
    return;
  fprintf(out, "=== incremental dependency graph (nodes=%zu) ===\n",
          graph->node_count);
  for (ny_dep_node_t *n = graph->nodes; n; n = n->next) {
    fprintf(out, "  %s  [fp=%016llx]", n->file_path ? n->file_path : "?",
            (unsigned long long)n->fingerprint.content_hash);
    if (n->deps) {
      fprintf(out, " ->");
      for (ny_dep_edge_t *e = n->deps; e; e = e->next)
        fprintf(out, " %s", e->to_file ? e->to_file : "?");
    }
    fprintf(out, "\n");
  }
}