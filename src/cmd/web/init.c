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
static void ny_web_meta_json_string(const char *s) {
  putchar('"');
  for (; s && *s; ++s) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\')
      printf("\\%c", c);
    else if (c == '\n')
      fputs("\\n", stdout);
    else if (c == '\r')
      fputs("\\r", stdout);
    else if (c == '\t')
      fputs("\\t", stdout);
    else if (c >= 0x20)
      putchar(c);
  }
  putchar('"');
}

static int ny_web_meta_main(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    nyt_err("ny-doc", "metadata: cannot read %s", path);
    return 1;
  }
  char line[1024], title[256] = {0}, keywords[768] = {0};
  int header = 1;
  while (header && fgets(line, sizeof(line), fp)) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      ++p;
    size_t n = strlen(p);
    while (n && (p[n - 1] == '\n' || p[n - 1] == '\r'))
      p[--n] = '\0';
    if (strncasecmp(p, ";; keywords:", 12) == 0) {
      snprintf(keywords, sizeof(keywords), "%s", p + 12);
      continue;
    }
    if (strncmp(p, ";;", 2) == 0) {
      char *text = p + 2;
      while (*text == ' ' || *text == '\t')
        ++text;
      if (*text && !title[0] && strlen(text) < sizeof(title))
        snprintf(title, sizeof(title), "%s", text);
      continue;
    }
    if (*p && p[0] != '#')
      header = 0;
  }
  fclose(fp);
  printf("{\"title\":");
  ny_web_meta_json_string(title);
  printf(",\"keywords\":[");
  int first = 1;
  for (char *p = keywords; *p;) {
    while (*p == ' ' || *p == '\t' || *p == ',')
      ++p;
    if (!*p)
      break;
    char word[128];
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != ',') {
      if (i + 1 < sizeof(word))
        word[i++] = (char)tolower((unsigned char)*p);
      ++p;
    }
    word[i] = '\0';
    if (!i)
      continue;
    if (!first)
      putchar(',');
    first = 0;
    ny_web_meta_json_string(word);
  }
  puts("]}");
  return 0;
}

int ny_web_main(int argc, char **argv) {
  if (argc > 1 && argv[1] && strcmp(argv[1], "meta") == 0) {
    if (argc != 3) {
      nyt_err("ny-doc", "usage: ny doc meta SOURCE");
      return 2;
    }
    return ny_web_meta_main(argv[2]);
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
  if (!mkdir_p(opts.out_dir)) {
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
  if (!mkdir_p(cache_docs_dir)) {
    nyt_path_join(cache_docs_dir, sizeof(cache_docs_dir),
                  nyt_default_cache_root_dir(), "docs");
    if (!mkdir_p(cache_docs_dir)) {
      free(bundle);
      nyt_err("ny-doc", "failed to create cache dir: %s", cache_docs_dir);
      return 1;
    }
  }
  if (!join_path(cache_website_dir, sizeof(cache_website_dir), cache_docs_dir,
                 "website") ||
      !mkdir_p(cache_website_dir)) {
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
