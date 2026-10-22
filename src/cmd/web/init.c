/*
 * ny-web entry point: CLI dispatch for the documentation generator
 * and website builder bundled under `ny web` / `ny doc`.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "web.h"
#include "base/args.h"
#include "base/util.h"
#include "../tools/repo.h"

#include "core.c"
static char *ny_web_doc_collect_paths(const char *docs_dir);

static char *ny_doc_meta_keywords_json(const char *source) {
  sb_t out = {0};
  size_t len = source ? strlen(source) : 0, start = 0;
  char *raw = calloc(768, 1);
  if (!raw) return NULL;
  while (start < len) {
    size_t stop = start;
    while (stop < len && source[stop] != '\n') ++stop;
    size_t left = start, right = stop;
    while (left < right && (source[left] == ' ' || source[left] == '\t')) ++left;
    if (right > left && source[right-1] == '\r') --right;
    if (right-left >= 12 && strncasecmp(source+left, ";; keywords:", 12) == 0) {
      left += 12;
      while (left < right && (source[left] == ' ' || source[left] == '\t')) ++left;
      size_t n = right-left < 767 ? right-left : 767;
      memcpy(raw, source+left, n);
      break;
    }
    if (right > left && !(source[left] == '#' ||
        (right-left >= 2 && source[left] == ';' && source[left+1] == ';'))) break;
    start = stop < len ? stop+1 : len;
  }
  int ok = sb_add(&out, "[");
  char *cursor = raw;
  while (ok && *cursor) {
    char *end = strchr(cursor, ',');
    if (end) *end = '\0';
    char *p = cursor;
    while (*p) {
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
      if (!*p) break;
      char *word = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
      size_t n = (size_t)(p-word); if (n > 127) n = 127;
      char saved = word[n]; word[n] = '\0';
      for (size_t i=0;i<n;++i) word[i]=(char)tolower((unsigned char)word[i]);
      if (out.len > 1) ok = sb_add(&out, ",");
      if (ok) ok = sb_add_json_str(&out, word);
      word[n] = saved;
    }
    if (!end) break;
    cursor = end+1;
  }
  if (ok) ok = sb_add(&out, "]");
  free(raw);
  if (!ok) { free(out.data); return NULL; }
  return out.data;
}

static int ny_web_meta_request(const char *path, int mode, char **result_json) {
  if (result_json) *result_json = NULL;
  if (!path || !*path || !result_json) return 2;
  sb_t out = {0};
  int ok = 1;
  if (mode == 0) {
    char *source = ny_read_file_raw(path, NULL);
    if (!source) { nyt_err("ny-doc", "metadata: cannot read %s", path); return 2; }
    char *title = ny_doc_meta_title_from_source(source);
    char *keywords = ny_doc_meta_keywords_json(source);
    ok = keywords && sb_add(&out, "{\"title\":") &&
         sb_add_json_str(&out, title ? title : "") &&
         sb_add(&out, ",\"keywords\":") && sb_add(&out, keywords) &&
         sb_add(&out, "}");
    free(source); free(title); free(keywords);
  } else {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
      nyt_err("ny-doc", "metadata: cannot collect %s", path); return 2;
    }
    char *paths = ny_web_doc_collect_paths(path);
    if (!paths) return 2;
    const char *p = paths;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '[' || !sb_add(&out, "[")) ok = 0;
    int first = 1;
    while (ok) {
      while (isspace((unsigned char)*p)) ++p;
      if (*p == ']') { ++p; break; }
      char *file = json_read_string(&p);
      if (!file) { ok = 0; break; }
      char *source = ny_read_file_raw(file, NULL);
      char *route = doc_route_name(path, file);
      char *title = source ? ny_doc_meta_title_from_source(source) : NULL;
      char *keywords = source ? ny_doc_meta_keywords_json(source) : NULL;
      if (!title && route) title = doc_title_from_route(route);
      if (!source || !route || !title || !keywords) ok = 0;
      if (ok && !first) ok = sb_add(&out, ",");
      if (ok) ok = sb_add(&out, "{\"route\":") && sb_add_json_str(&out, route) &&
          sb_add(&out, ",\"title\":") && sb_add_json_str(&out, title) &&
          sb_add(&out, ",\"keywords\":") && sb_add(&out, keywords) &&
          sb_add(&out, ",\"path\":") && sb_add_json_str(&out, file) &&
          sb_add(&out, "}");
      first = 0;
      free(file); free(source); free(route); free(title); free(keywords);
      while (isspace((unsigned char)*p)) ++p;
      if (*p == ',') ++p;
      else if (*p != ']') ok = 0;
    }
    while (isspace((unsigned char)*p)) ++p;
    ok = ok && !*p && sb_add(&out, "]");
    free(paths);
  }
  if (!ok) { free(out.data); return 1; }
  *result_json = out.data ? out.data : strdup("");
  return *result_json ? 0 : 1;
}

typedef struct ny_web_doc_paths {
  char **items;
  size_t len;
  size_t cap;
} ny_web_doc_paths_t;

static int ny_web_doc_path_compare(const void *a, const void *b) {
  const char *const *left = (const char *const *)a;
  const char *const *right = (const char *const *)b;
  return strcmp(*left, *right);
}

static int ny_web_doc_paths_walk(const char *dir, ny_web_doc_paths_t *paths,
                                 unsigned depth) {
  if (!dir || !paths || depth > 32 || paths->len > 100000)
    return 0;
  DIR *stream = opendir(dir);
  if (!stream)
    return 0;
  int ok = 1;
  struct dirent *entry;
  while (ok && (entry = readdir(stream)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    char path[PATH_MAX];
    if (!join_path(path, sizeof(path), dir, entry->d_name)) {
      ok = 0;
      break;
    }
    struct stat st;
    if (stat(path, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      if (strcmp(entry->d_name, "build") == 0 ||
          strcmp(entry->d_name, "tmp") == 0 ||
          strcmp(entry->d_name, "node_modules") == 0 ||
          strcmp(entry->d_name, "__pycache__") == 0)
        continue;
      ok = ny_web_doc_paths_walk(path, paths, depth + 1);
      continue;
    }
    if (!S_ISREG(st.st_mode) || !docs_file_supported(path))
      continue;
    if (paths->len >= 100000) {
      ok = 0;
      break;
    }
    if (paths->len == paths->cap) {
      size_t next_cap = paths->cap ? paths->cap * 2 : 32;
      if (next_cap < paths->cap || next_cap > 100000 ||
          next_cap > SIZE_MAX / sizeof(*paths->items)) {
        ok = 0;
        break;
      }
      char **next = (char **)realloc(paths->items,
                                     next_cap * sizeof(*paths->items));
      if (!next) {
        ok = 0;
        break;
      }
      paths->items = next;
      paths->cap = next_cap;
    }
    paths->items[paths->len] = strdup(path);
    if (!paths->items[paths->len]) {
      ok = 0;
      break;
    }
    ++paths->len;
  }
  if (closedir(stream) != 0)
    ok = 0;
  return ok;
}

static char *ny_web_doc_collect_paths(const char *docs_dir) {
  ny_web_doc_paths_t paths = {0};
  if (!ny_web_doc_paths_walk(docs_dir, &paths, 0)) {
    for (size_t i = 0; i < paths.len; ++i)
      free(paths.items[i]);
    free(paths.items);
    return NULL;
  }
  qsort(paths.items, paths.len, sizeof(*paths.items), ny_web_doc_path_compare);
  sb_t json = {0};
  int ok = sb_add(&json, "[");
  for (size_t i = 0; ok && i < paths.len; ++i) {
    if (i)
      ok = sb_add(&json, ",");
    if (ok)
      ok = sb_add_json_str(&json, paths.items[i]);
  }
  if (ok)
    ok = sb_add(&json, "]");
  for (size_t i = 0; i < paths.len; ++i)
    free(paths.items[i]);
  free(paths.items);
  if (!ok) {
    free(json.data);
    return NULL;
  }
  return json.data ? json.data : strdup("[]");
}

static int ny_web_meta_main(const char *path, bool collect_docs) {
  char *json = NULL;
  int rc = ny_web_meta_request(path, collect_docs ? 1 : 0, &json);
  if (rc == 0 && json)
    fprintf(stdout, "%s\n", json);
  free(json);
  return rc;
}

int ny_web_main(int argc, char **argv) {
  if (argc > 1 && argv[1] && strcmp(argv[1], "meta") == 0) {
    if (argc == 3)
      return ny_web_meta_main(argv[2], false);
    if (argc == 4 && argv[2] && strcmp(argv[2], "--collect") == 0)
      return ny_web_meta_main(argv[3], true);
    {
      nyt_err("ny-doc", "usage: ny doc meta SOURCE | ny doc meta --collect DOCS_DIR");
      return 2;
    }
  }
  if (argc > 1 && argv[1] &&
      (strcmp(argv[1], "search") == 0 || strcmp(argv[1], "find") == 0 ||
       strcmp(argv[1], "get") == 0 || strcmp(argv[1], "show") == 0)) {
    int get_one = strcmp(argv[1], "get") == 0 || strcmp(argv[1], "show") == 0;
    return ny_doc_search_main(argc, argv, get_one);
  }

  web_opts_t opts = {0};
  char err[256];
  int rc = web_parse_args(argc, argv, &opts, err, sizeof(err));
  if (rc != 0)
    return rc == 1 ? 0 : rc;

  char root[PATH_MAX];
  const char *src_root = find_nytrix_share_root();
  if (!src_root || !*src_root) {
    nyt_err("ny-doc", "failed to resolve nytrix share root");
    return 1;
  }
  snprintf(root, sizeof(root), "%s", src_root);

  char default_input[PATH_MAX];
  const char *input = web_resolve_input(root, opts.input, default_input);

  int source_files = 0;
  char *bundle = read_web_input_bundle(input, root, &source_files);
  if (!bundle) {
    nyt_err("ny-doc", "failed to read input: %s", input);
    return 1;
  }
  if (!ny_mkdir_p(opts.out_dir)) {
    free(bundle);
    nyt_err("ny-doc", "failed to create output dir: %s", opts.out_dir);
    return 1;
  }

  char cache_docs_dir[PATH_MAX], cache_website_dir[PATH_MAX];
  if (!join_path(cache_docs_dir, sizeof(cache_docs_dir), root,
                 "build/cache/docs")) {
    free(bundle);
    nyt_err("ny-doc", "repository path is too long");
    return 1;
  }
  if (!ny_mkdir_p(cache_docs_dir)) {
    nyt_path_join(cache_docs_dir, sizeof(cache_docs_dir),
                  nyt_default_cache_root_dir(), "docs");
    if (!ny_mkdir_p(cache_docs_dir)) {
      free(bundle);
      nyt_err("ny-doc", "failed to create cache dir: %s", cache_docs_dir);
      return 1;
    }
  }
  if (!join_path(cache_website_dir, sizeof(cache_website_dir), cache_docs_dir,
                 "website") ||
      !ny_mkdir_p(cache_website_dir)) {
    free(bundle);
    nyt_err("ny-doc", "failed to create cache dir: %s", cache_website_dir);
    return 1;
  }

  web_paths_t paths = {0};
  if (!web_build_asset_paths(root, opts.out_dir, cache_docs_dir,
                             cache_website_dir, &paths)) {
    free(bundle);
    nyt_err("ny-doc", "repository path is too long");
    return 1;
  }

  int parsed_modules = 0, parsed_symbols = 0;
  if (!web_render_docs_page(root, opts.site_url, bundle, source_files,
                            opts.out_dir, &paths, &parsed_modules,
                            &parsed_symbols)) {
    free(bundle);
    return 1;
  }
  free(bundle);

  if (!web_copy_output_assets(&paths)) {
    nyt_err("ny-doc", "failed copying output assets");
    return 1;
  }

  nyt_msg("DOCS", NYT_CYAN,
          "parsed %d modules, %d symbols from %d source files", parsed_modules,
          parsed_symbols, source_files);
  nyt_msg("CACHE", NYT_CYAN, "website OG assets: %s", cache_website_dir);
  nyt_msg("OK", NYT_GREEN, "docs ready: %s", opts.out_dir);
  char display_index_path[PATH_MAX];
  abs_path_for_display(display_index_path, sizeof(display_index_path),
                       paths.index_path);
  nyt_msg("OPEN", NYT_GREEN, "%s",
          display_index_path[0] ? display_index_path : paths.index_path);

  if (opts.serve)
    return serve_http_forever(opts.out_dir, opts.port);
  return 0;
}
