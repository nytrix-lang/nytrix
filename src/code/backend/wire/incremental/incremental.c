/*
 * Incremental compilation: file-fingerprint caching, dependency-graph
 * construction, change detection, and transitive recompile-set computation.
 */
#include "code/backend/wire/incremental/incremental.h"
#include "base/common.h"
#include "base/time.h"
#include "base/util.h"

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

#define NY_INC_GRAPH_MAGIC 0x6e79696e63ULL /* "nyinc" */
#define NY_INC_GRAPH_VERSION 2u

static const ny_incremental_config_t g_default_config = {
    .enable_incremental = true, .track_system_deps = false, .max_depth = 32};
static ny_incremental_config_t g_config;
static bool g_initialized = false;

/*
 * small dynamic string-array helpers
 */

static bool strlist_push(char ***list, size_t *len, size_t *cap,
                         const char *s) {
  if (*len == *cap) {
    size_t nc = *cap ? *cap * 2 : 8;
    if (nc < *cap || nc > SIZE_MAX / sizeof(**list))
      return false;
    char **p = realloc(*list, nc * sizeof(**list));
    if (!p)
      return false;
    *list = p;
    *cap = nc;
  }
  (*list)[(*len)++] = ny_strdup(s);
  return (*list)[(*len) - 1] != NULL;
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
  ny_incremental_config_t next = config ? *config : g_default_config;
  next.project_root = next.project_root ? ny_strdup(next.project_root) : NULL;
  next.cache_dir = next.cache_dir ? ny_strdup(next.cache_dir) : NULL;
  if (config && ((config->project_root && !next.project_root) ||
                 (config->cache_dir && !next.cache_dir))) {
    free((char *)next.project_root);
    free((char *)next.cache_dir);
    return false;
  }
  if (!next.max_depth)
    next.max_depth = 32;
  if (g_initialized)
    ny_incremental_shutdown();
  g_config = next;
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

static bool fingerprint_from_content(const char *file_path, const char *content,
                                     size_t len,
                                     ny_file_fingerprint_t *out_fingerprint) {
  if (!file_path || !*file_path || !content || !out_fingerprint)
    return false;
  struct stat st;
  if (stat(file_path, &st) != 0)
    return false;
  if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != len)
    return false;
  memset(out_fingerprint, 0, sizeof(*out_fingerprint));
  out_fingerprint->content_hash = ny_hash64(content, len);
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

bool ny_incremental_file_fingerprint(const char *file_path,
                                     ny_file_fingerprint_t *out_fingerprint) {
  if (!file_path || !*file_path || !out_fingerprint)
    return false;
  memset(out_fingerprint, 0, sizeof(*out_fingerprint));
  size_t len = 0;
  char *content = ny_read_file_raw(file_path, &len);
  if (!content)
    return false;
  bool ok = fingerprint_from_content(file_path, content, len, out_fingerprint);
  free(content);
  return ok;
}

static bool fingerprint_equal(const ny_file_fingerprint_t *a,
                              const ny_file_fingerprint_t *b) {
  return a->content_hash == b->content_hash && a->size == b->size &&
         a->mtime_nsec == b->mtime_nsec && a->ino == b->ino && a->dev == b->dev;
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
    while (s < end &&
           (isalnum((unsigned char)*s) || *s == '_' || *s == '.' || *s == '/'))
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
          {
            bool dup = false;
            for (size_t k = 0; k < *out_dep_count; ++k)
              if (strcmp((*out_deps)[k], dep) == 0) {
                dup = true;
                break;
              }
            if (!dup && !strlist_push(out_deps, out_dep_count, &cap, dep)) {
              free(dep);
              strlist_free(*out_deps, *out_dep_count);
              *out_deps = NULL;
              *out_dep_count = 0;
              return false;
            }
          }
          free(dep);
        } else {
          strlist_free(*out_deps, *out_dep_count);
          *out_deps = NULL;
          *out_dep_count = 0;
          return false;
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
  if (!n->file_path) {
    free(n);
    return NULL;
  }
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
  ny_dep_edge_t *re = calloc(1, sizeof(*re));
  if (!fe || !re) {
    free(fe);
    free(re);
    return false;
  }
  fe->to_file = ny_strdup(to);
  fe->from_file = ny_strdup(from);
  re->from_file = ny_strdup(from);
  re->to_file = ny_strdup(to);
  if (!fe->to_file || !fe->from_file || !re->from_file || !re->to_file) {
    free(fe->to_file);
    free(fe->from_file);
    free(re->from_file);
    free(re->to_file);
    free(fe);
    free(re);
    return false;
  }
  fe->next = fn->deps;
  fn->deps = fe;
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
    if (!graph_add_or_get(out_graph, source_files[i]))
      goto fail;
  /*
   * parse each file for deps
   */
  for (size_t i = 0; i < file_count; ++i) {
    const char *fp = source_files[i];
    size_t len = 0;
    char *content = ny_read_file_raw(fp, &len);
    if (!content)
      continue;
    ny_dep_node_t *node = graph_find_node(out_graph, fp, 0);
    if (node && fingerprint_from_content(fp, content, len, &node->fingerprint))
      node->fingerprint_valid = true;
    char **deps = NULL;
    size_t dep_count = 0;
    bool ok =
        ny_incremental_parse_dependencies(fp, content, len, &deps, &dep_count);
    if (ok) {
      for (size_t d = 0; d < dep_count; ++d) {
        /*
         * resolve name to a file in the set: match the absolute path, its
         * source spelling (e.g. "./provider.ny"), or the stripped basename.
         */
        for (size_t j = 0; j < file_count; ++j) {
          const char *cand = source_files[j];
          if (strcmp(cand, deps[d]) == 0 || ny_name_tail_is(cand, deps[d]) ||
              source_basename_matches(cand, deps[d])) {
            /* Ambiguous basenames must invalidate every possible provider,
             * never whichever input happened to occur first. */
            if (!graph_add_edge(out_graph, fp, cand)) {
              ok = false;
              break;
            }
          }
        }
      }
    }
    strlist_free(deps, dep_count);
    free(content);
    if (!ok)
      goto fail;
  }
  return true;
fail:
  ny_incremental_free_graph(out_graph);
  return false;
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
         graph_write_u64(f, fp->mtime_nsec) && graph_write_u64(f, fp->size) &&
         graph_write_u64(f, fp->ino) && graph_write_u64(f, fp->dev);
}
static bool graph_read_fp(FILE *f, ny_file_fingerprint_t *fp) {
  return graph_read_u64(f, &fp->content_hash) &&
         graph_read_u64(f, &fp->mtime_nsec) && graph_read_u64(f, &fp->size) &&
         graph_read_u64(f, &fp->ino) && graph_read_u64(f, &fp->dev);
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
    ok = graph_write_cstr(f, n->file_path) &&
         graph_write_fp(f, &n->fingerprint) &&
         graph_write_u32(f, n->fingerprint_valid ? 1u : 0u);
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
    uint32_t fingerprint_valid = 0;
    ok = path && graph_read_fp(f, &fp) &&
         graph_read_u32(f, &fingerprint_valid) && fingerprint_valid <= 1;
    if (ok) {
      ny_dep_node_t *n = graph_add_or_get(out_graph, path);
      if (!n)
        ok = false;
      else {
        n->fingerprint = fp;
        n->fingerprint_valid = fingerprint_valid != 0;
      }
    }
    if (ok) {
      uint32_t ed = 0;
      ok = graph_read_u32(f, &ed);
      for (uint32_t k = 0; ok && k < ed; ++k) {
        char *to = graph_read_cstr(f);
        ok = to && graph_add_edge(out_graph, path ? path : "", to);
        free(to);
      }
    }
    free(path);
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
    if (!n->fingerprint_valid ||
        !ny_incremental_file_fingerprint(n->file_path, &cur) ||
        !fingerprint_equal(&cur, &n->fingerprint))
      if (!strlist_push(out_changed, out_changed_count, &cap, n->file_path))
        return false;
  }
  return true;
}

typedef struct nk_queue_t {
  ny_dep_node_t **items;
  size_t len, cap;
} nk_queue_t;

static bool nkq_push(nk_queue_t *q, ny_dep_node_t *n) {
  if (q->len == q->cap) {
    size_t cap = q->cap ? q->cap * 2 : 16;
    if (cap < q->cap || cap > SIZE_MAX / sizeof(*q->items))
      return false;
    ny_dep_node_t **items = realloc(q->items, cap * sizeof(*items));
    if (!items)
      return false;
    q->items = items;
    q->cap = cap;
  }
  q->items[q->len++] = n;
  return true;
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
      if (!nkq_push(&q, n))
        goto fail;
    }
  }
  while (q.len) {
    ny_dep_node_t *n = q.items[--q.len];
    if (!n->file_path)
      continue;
    if (!strlist_push(out_recompile, out_recompile_count, &cap, n->file_path))
      goto fail;
    /*
     * dependents
     */
    for (ny_dep_edge_t *e = n->rdeps; e; e = e->next) {
      ny_dep_node_t *dep = graph_find_node(graph, e->from_file, 0);
      if (dep && !dep->needs_recompile) {
        dep->needs_recompile = true;
        if (!nkq_push(&q, dep))
          goto fail;
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
fail:
  free(q.items);
  for (ny_dep_node_t *n = graph->nodes; n; n = n->next)
    n->needs_recompile = false;
  strlist_free(*out_recompile, *out_recompile_count);
  *out_recompile = NULL;
  *out_recompile_count = 0;
  return false;
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
  static const char *const suffixes[] = {".bc", ".ll", ".so", ".manifest"};
  for (size_t i = 0; i < recompile_count; ++i) {
    const char *file = recompile_files[i];
    /*
     * Derived artifact pattern: <cache>/<hash>.{bc,so}
     */
    uint64_t h = ny_hash64_cstr(file);
    for (size_t j = 0; j < sizeof(suffixes) / sizeof(*suffixes); ++j) {
      char path[PATH_MAX];
      int len = snprintf(path, sizeof(path), "%s/%016llx%s", cache_dir,
                         (unsigned long long)h, suffixes[j]);
      if (len < 0 || (size_t)len >= sizeof(path) ||
          (remove(path) != 0 && errno != ENOENT))
        ok = false;
    }
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
  if (!config || !ny_incremental_init(config))
    return res;
  if (file_count == 0) {
    res.success = true;
    return res;
  }
  char graph_path[PATH_MAX];
  const char *root = g_config.project_root ? g_config.project_root : ".";
  const char *cache = g_config.cache_dir && *g_config.cache_dir
                          ? g_config.cache_dir
                          : ny_default_cache_root_dir();
  int path_len =
      snprintf(graph_path, sizeof(graph_path), "%s/incgraph-%016llx.bin", cache,
               (unsigned long long)ny_hash64_cstr(root));
  if (path_len < 0 || (size_t)path_len >= sizeof(graph_path))
    return res;
  /*
   * load prior graph (if any)
   */
  ny_dep_graph_t old = {0};
  bool have_old = ny_incremental_load_graph(graph_path, &old);
  /*
   * build current graph
   */
  ny_dep_graph_t cur = {0};
  if (!ny_incremental_build_graph(source_files, file_count, &cur)) {
    ny_incremental_free_graph(&old);
    return res;
  }
  /* Cold/forced builds must schedule every input, not return an empty set. */
  char **changed = NULL;
  size_t changed_count = 0;
  size_t cap = 0;
  char **old_affected = NULL;
  size_t old_affected_count = 0;
  char **rset = NULL;
  size_t rset_count = 0;
  for (ny_dep_node_t *n = cur.nodes; n; n = n->next) {
    ny_dep_node_t *previous = graph_find_node(&old, n->file_path, 0);
    if (!have_old || !config->enable_incremental ||
        config->force_full_rebuild || !previous || !n->fingerprint_valid ||
        !previous->fingerprint_valid ||
        !fingerprint_equal(&n->fingerprint, &previous->fingerprint))
      if (!strlist_push(&changed, &changed_count, &cap, n->file_path))
        goto fail;
  }
  /*
   * Removed providers/edges exist only in the previous graph. Propagate there
   * before applying the surviving names to the current dependency closure.
   */
  for (ny_dep_node_t *n = old.nodes; n; n = n->next) {
    if (!graph_find_node(&cur, n->file_path, 0) &&
        !strlist_push(&changed, &changed_count, &cap, n->file_path))
      goto fail;
  }
  if (!ny_incremental_compute_recompile_set(&old, (const char **)changed,
                                            changed_count, &old_affected,
                                            &old_affected_count))
    goto fail;
  for (size_t i = 0; i < old_affected_count; ++i)
    if (graph_find_node(&cur, old_affected[i], 0) &&
        !strlist_push(&changed, &changed_count, &cap, old_affected[i]))
      goto fail;
  if (!ny_incremental_compute_recompile_set(&cur, (const char **)changed,
                                            changed_count, &rset, &rset_count))
    goto fail;
  strlist_free(changed, changed_count);
  changed = NULL;
  changed_count = 0;
  strlist_free(old_affected, old_affected_count);
  old_affected = NULL;
  old_affected_count = 0;
  /*
   * invalidate any cached artifacts for the recompile set
   */
  if (rset_count > 0 &&
      !ny_incremental_invalidate_cache(&cur, (const char **)rset, rset_count))
    goto fail;
  /*
   * save updated graph so the next run diff is against this state
   */
  if (!ny_incremental_save_graph(&cur, graph_path))
    goto fail;
  res.recompile_files = rset;
  res.recompile_count = rset_count;
  ny_incremental_free_graph(&old);
  ny_incremental_free_graph(&cur);
  res.success = true;
  res.elapsed_ms = ny_ticks_elapsed_ms(t0);
  return res;
fail:
  strlist_free(changed, changed_count);
  strlist_free(old_affected, old_affected_count);
  strlist_free(rset, rset_count);
  ny_incremental_free_graph(&old);
  ny_incremental_free_graph(&cur);
  res.error_message = ny_strdup("incremental dependency update failed");
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
