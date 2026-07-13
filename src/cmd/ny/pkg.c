#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "pkg.h"
#include "base/args.h"
#include "base/process.h"
#include "base/util.h"
#include "cmd/tools/tool.h"
#include "wire/build.h"

#include <ctype.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <termios.h>
#endif
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getcwd _getcwd
#define getpid _getpid
#else
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

typedef struct {
  char *name;
  char *source;
  char *ref;
} ny_pkg_dep_t;

typedef struct {
  ny_pkg_dep_t *items;
  size_t len;
  size_t cap;
  char *name;
  char *version;
  char *description;
  char *author;
  char *license;
  char *repository;
  char path[4096];
  char root[4096];
} ny_pkg_manifest_t;

typedef struct {
  ny_pkg_dep_t dep;
  char *path;
  char *commit;
} ny_pkg_lock_entry_t;

typedef struct {
  ny_pkg_lock_entry_t *items;
  size_t len;
  size_t cap;
} ny_pkg_lock_t;

typedef struct {
  char *name;
  char *source;
  char *kind;
  char *repo;
  char *path;
  char *version;
  char *description;
  int score;
} ny_pkg_search_entry_t;

typedef struct {
  ny_pkg_search_entry_t *items;
  size_t len;
  size_t cap;
} ny_pkg_search_list_t;

typedef struct {
  bool global;
  bool system;
  bool venv;
  bool vendor;
  bool force;
  bool verbose;
  bool interactive;
  int limit;
  char manifest[4096];
  char root[4096];
  char install_root[4096];
  const char *init_name;
  const char *init_version;
  const char *init_description;
  const char *init_author;
  const char *init_license;
  const char *init_repository;
} ny_pkg_opts_t;

typedef struct {
  const char *name;
  const char *desc;
} ny_pkg_usage_entry_t;

static void ny_pkg_usage_section(const char *title, const ny_pkg_usage_entry_t *items, size_t len,
                                 const char *color) {
  printf("%s%s:%s\n", nyt_clr(NYT_BOLD), title, nyt_clr(NYT_RESET));
  for (size_t i = 0; i < len; ++i)
    printf("  %s%s%s %s\n", nyt_clr(color), items[i].name, nyt_clr(NYT_RESET), items[i].desc);
}

static void ny_pkg_usage(void) {
  static const ny_pkg_usage_entry_t commands[] = {
      {"init [name]            ", "create ny.pkg.json"},
      {"add <name> <source>    ", "add, fetch, and lock a package"},
      {"get <name> [source]    ", "alias for add; source may come from registry"},
      {"install [name [source]]", "install manifest deps or one dep"},
      {"uninstall <name>       ", "remove local install and manifest dep"},
      {"sync                   ", "install manifest deps"},
      {"update [name]          ", "refresh git packages and lockfile"},
      {"list                   ", "list manifest dependencies"},
      {"search [query]         ", "fuzzy-search registries and package repositories"},
      {"path [name]            ", "print install root or package path"},
      {"info                   ", "print package metadata and deps"},
      {"venv                   ", "create/print .nytrix/venv/lib"},
      {"repo add <r> <source>  ", "add a package repository for name lookup"},
      {"repo list|sync|rm      ", "inspect, cache, or remove repositories"},
      {"registry [name]        ", "read NYTRIX_PKG_REGISTRY/.ny.registry"},
  };
  static const ny_pkg_usage_entry_t options[] = {
      {"--manifest PATH        ", "manifest path (default ny.pkg.json)"},
      {"--inplace              ", "install into ./ny_modules (default)"},
      {"--venv                 ", "install into ./.nytrix/venv/lib"},
      {"--vendor               ", "install into ./vendor/ny_modules"},
      {"--user, --global       ", "install into $NYTRIX_PKG_HOME or ~/.nytrix/pkg"},
      {"--system, --syswide    ", "install into /usr/local/share/nytrix/pkg"},
      {"--root DIR             ", "install root override"},
      {"--ref REF              ", "git ref/version for add/get/install"},
      {"--author NAME          ", "metadata for init"},
      {"--version VER          ", "metadata for init (default 0.1.0)"},
      {"--license NAME         ", "metadata for init"},
      {"--description TEXT     ", "metadata for init"},
      {"--repository URL       ", "metadata for init"},
      {"-i, --interactive      ", "open the built-in fuzzy package picker"},
      {"--limit N              ", "cap package search results"},
      {"--force                ", "replace local copies when possible"},
      {"--color MODE           ", "auto | always | never"},
  };
  nyt_heading("Nytrix Packages");
  printf("%susage:%s %sny pkg%s %s<command> [args]%s\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET),
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny get%s %s<name> [source]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny install%s %s[name [source]]%s\n\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  ny_pkg_usage_section("commands", commands, sizeof(commands) / sizeof(commands[0]), NYT_CYAN);
  printf("\n");
  ny_pkg_usage_section("options", options, sizeof(options) / sizeof(options[0]), NYT_GREEN);
  printf("\n%sSources:%s local dir/file, git+URL, git@host:path, URL.git, archive tar/zip, archive+PATH\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("%sRepos:%s ny pkg repo add core git+https://host/pkgs.git; ny pkg repo list; ny get pkgname\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
}

static bool ny_pkg_path_exists(const char *path) {
  return path && *path && ny_access(path, F_OK) == 0;
}

static bool ny_pkg_is_dir(const char *path) {
  if (!path || !*path)
    return false;
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *ny_pkg_strndup(const char *s, size_t n) {
  char *out = (char *)malloc(n + 1);
  if (!out)
    return NULL;
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static char *ny_pkg_trim_dup(const char *s, size_t n) {
  while (n > 0 && isspace((unsigned char)*s)) {
    s++;
    n--;
  }
  while (n > 0 && isspace((unsigned char)s[n - 1]))
    n--;
  if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
    s++;
    n -= 2;
  }
  return ny_pkg_strndup(s, n);
}

static void ny_pkg_join(char *out, size_t out_len, const char *a, const char *b) {
  ny_join_path(out, out_len, a && *a ? a : ".", b && *b ? b : "");
}

static int ny_pkg_run_argv(const char *const argv[], bool verbose) {
  if (verbose && argv && argv[0]) {
    fprintf(stderr, "[pkg]");
    for (size_t i = 0; argv[i]; ++i)
      fprintf(stderr, " %s", argv[i]);
    fputc('\n', stderr);
  }
  int rc = ny_exec_spawn(argv);
  if (rc != 0 && argv && argv[0])
    nyt_err("ny pkg", "command failed: %s (rc=%d)", argv[0], rc);
  return rc == 0 ? 0 : 1;
}

static char *ny_pkg_capture_argv(const char *const argv[]) {
  char *out = NULL;
  int rc = ny_process_capture(argv, &out, true);
  if (rc != 0 || !out) {
    free(out);
    return NULL;
  }
  /* trim trailing whitespace */
  size_t n = strlen(out);
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
    out[--n] = '\0';
  if (n == 0) {
    free(out);
    return NULL;
  }
  return out;
}

static char *ny_pkg_guess_author(void) {
  const char *env_author = getenv("NYTRIX_AUTHOR");
  if (env_author && *env_author)
    return ny_strdup(env_author);

  char *name = NULL;
  char *email = NULL;
  const char *env_name = getenv("GIT_AUTHOR_NAME");
  const char *env_email = getenv("GIT_AUTHOR_EMAIL");
  if (env_name && *env_name)
    name = ny_strdup(env_name);
  if (env_email && *env_email)
    email = ny_strdup(env_email);
  if (!name) {
    const char *git_name_argv[] = {"git", "config", "--get", "user.name", NULL};
    name = ny_pkg_capture_argv(git_name_argv);
  }
  if (!email) {
    const char *git_email_argv[] = {"git", "config", "--get", "user.email", NULL};
    email = ny_pkg_capture_argv(git_email_argv);
  }

  char *out = NULL;
  if (name && *name && email && *email) {
    size_t n = strlen(name) + strlen(email) + 4;
    out = (char *)malloc(n);
    if (out)
      snprintf(out, n, "%s <%s>", name, email);
  } else if (name && *name) {
    out = ny_strdup(name);
  } else if (email && *email) {
    out = ny_strdup(email);
  }
  free(name);
  free(email);
  return out;
}

static bool ny_pkg_name_ok(const char *name) {
  if (!name || !*name)
    return false;
  for (const char *p = name; *p; ++p) {
    if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.'))
      return false;
  }
  return true;
}

static void ny_pkg_dep_free(ny_pkg_dep_t *dep) {
  if (!dep)
    return;
  free(dep->name);
  free(dep->source);
  free(dep->ref);
  dep->name = NULL;
  dep->source = NULL;
  dep->ref = NULL;
}

static void ny_pkg_manifest_free(ny_pkg_manifest_t *m) {
  if (!m)
    return;
  for (size_t i = 0; i < m->len; ++i)
    ny_pkg_dep_free(&m->items[i]);
  free(m->items);
  free(m->name);
  free(m->version);
  free(m->description);
  free(m->author);
  free(m->license);
  free(m->repository);
  memset(m, 0, sizeof(*m));
}

static void ny_pkg_manifest_set_owned(char **slot, char *value) {
  if (!slot)
    return;
  if (!value || !*value) {
    free(value);
    return;
  }
  free(*slot);
  *slot = value;
}

static void ny_pkg_manifest_set_dup(char **slot, const char *value) {
  if (slot && value && *value)
    ny_pkg_manifest_set_owned(slot, ny_strdup(value));
}

static void ny_pkg_manifest_set_meta(ny_pkg_manifest_t *m, const char *version,
                                     const char *description, const char *author,
                                     const char *license, const char *repository) {
  if (!m)
    return;
  ny_pkg_manifest_set_dup(&m->version, version);
  ny_pkg_manifest_set_dup(&m->description, description);
  ny_pkg_manifest_set_dup(&m->author, author);
  ny_pkg_manifest_set_dup(&m->license, license);
  ny_pkg_manifest_set_dup(&m->repository, repository);
}

static void ny_pkg_lock_free(ny_pkg_lock_t *lock) {
  if (!lock)
    return;
  for (size_t i = 0; i < lock->len; ++i) {
    ny_pkg_dep_free(&lock->items[i].dep);
    free(lock->items[i].path);
    free(lock->items[i].commit);
  }
  free(lock->items);
  memset(lock, 0, sizeof(*lock));
}

static void ny_pkg_search_entry_free(ny_pkg_search_entry_t *e) {
  if (!e)
    return;
  free(e->name);
  free(e->source);
  free(e->kind);
  free(e->repo);
  free(e->path);
  free(e->version);
  free(e->description);
  memset(e, 0, sizeof(*e));
}

static void ny_pkg_search_list_free(ny_pkg_search_list_t *list) {
  if (!list)
    return;
  for (size_t i = 0; i < list->len; ++i)
    ny_pkg_search_entry_free(&list->items[i]);
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static bool ny_pkg_search_seen(const ny_pkg_search_list_t *list, const char *name,
                               const char *source) {
  if (!list || !name)
    return false;
  for (size_t i = 0; i < list->len; ++i) {
    const ny_pkg_search_entry_t *e = &list->items[i];
    if (strcmp(e->name ? e->name : "", name) == 0 &&
        strcmp(e->source ? e->source : "", source ? source : "") == 0)
      return true;
  }
  return false;
}

static void ny_pkg_search_push(ny_pkg_search_list_t *list, const char *name, const char *source,
                               const char *kind, const char *repo, const char *path,
                               const char *version, const char *description) {
  if (!list || !name || !*name || ny_pkg_search_seen(list, name, source))
    return;
  if (list->len == list->cap) {
    size_t nc = list->cap ? list->cap * 2 : 32;
    ny_pkg_search_entry_t *n =
        (ny_pkg_search_entry_t *)realloc(list->items, nc * sizeof(ny_pkg_search_entry_t));
    if (!n)
      return;
    memset(n + list->cap, 0, (nc - list->cap) * sizeof(ny_pkg_search_entry_t));
    list->items = n;
    list->cap = nc;
  }
  ny_pkg_search_entry_t *e = &list->items[list->len++];
  e->name = ny_strdup(name);
  e->source = source && *source ? ny_strdup(source) : NULL;
  e->kind = kind && *kind ? ny_strdup(kind) : NULL;
  e->repo = repo && *repo ? ny_strdup(repo) : NULL;
  e->path = path && *path ? ny_strdup(path) : NULL;
  e->version = version && *version ? ny_strdup(version) : NULL;
  e->description = description && *description ? ny_strdup(description) : NULL;
  e->score = 1000000;
}

static void ny_pkg_manifest_push(ny_pkg_manifest_t *m, const char *name, const char *source,
                                 const char *ref) {
  if (!m || !name || !source)
    return;
  for (size_t i = 0; i < m->len; ++i) {
    if (strcmp(m->items[i].name, name) == 0) {
      free(m->items[i].source);
      free(m->items[i].ref);
      m->items[i].source = ny_strdup(source);
      m->items[i].ref = ref && *ref ? ny_strdup(ref) : NULL;
      return;
    }
  }
  if (m->len == m->cap) {
    size_t nc = m->cap ? m->cap * 2 : 8;
    ny_pkg_dep_t *n = (ny_pkg_dep_t *)realloc(m->items, nc * sizeof(ny_pkg_dep_t));
    if (!n)
      return;
    m->items = n;
    m->cap = nc;
  }
  m->items[m->len].name = ny_strdup(name);
  m->items[m->len].source = ny_strdup(source);
  m->items[m->len].ref = ref && *ref ? ny_strdup(ref) : NULL;
  m->len++;
}

static bool ny_pkg_manifest_remove(ny_pkg_manifest_t *m, const char *name) {
  if (!m || !name)
    return false;
  for (size_t i = 0; i < m->len; ++i) {
    if (strcmp(m->items[i].name, name) != 0)
      continue;
    ny_pkg_dep_free(&m->items[i]);
    for (size_t j = i + 1; j < m->len; ++j)
      m->items[j - 1] = m->items[j];
    m->len--;
    if (m->len < m->cap)
      memset(&m->items[m->len], 0, sizeof(m->items[m->len]));
    return true;
  }
  return false;
}

static void ny_pkg_lock_push(ny_pkg_lock_t *lock, const ny_pkg_dep_t *dep, const char *path,
                             const char *commit) {
  if (!lock || !dep || !dep->name || !dep->source)
    return;
  for (size_t i = 0; i < lock->len; ++i) {
    if (strcmp(lock->items[i].dep.name, dep->name) == 0) {
      free(lock->items[i].path);
      free(lock->items[i].commit);
      lock->items[i].path = path ? ny_strdup(path) : NULL;
      lock->items[i].commit = commit ? ny_strdup(commit) : NULL;
      return;
    }
  }
  if (lock->len == lock->cap) {
    size_t nc = lock->cap ? lock->cap * 2 : 16;
    ny_pkg_lock_entry_t *n =
        (ny_pkg_lock_entry_t *)realloc(lock->items, nc * sizeof(ny_pkg_lock_entry_t));
    if (!n)
      return;
    lock->items = n;
    lock->cap = nc;
  }
  lock->items[lock->len].dep.name = ny_strdup(dep->name);
  lock->items[lock->len].dep.source = ny_strdup(dep->source);
  lock->items[lock->len].dep.ref = dep->ref ? ny_strdup(dep->ref) : NULL;
  lock->items[lock->len].path = path ? ny_strdup(path) : NULL;
  lock->items[lock->len].commit = commit ? ny_strdup(commit) : NULL;
  lock->len++;
}

static const char *ny_pkg_skip_ws(const char *p, const char *end) {
  while (p < end && isspace((unsigned char)*p))
    p++;
  return p;
}

static char *ny_pkg_parse_json_string(const char **pp, const char *end) {
  const char *p = ny_pkg_skip_ws(*pp, end);
  if (p >= end || *p != '"')
    return NULL;
  p++;
  char *out = (char *)malloc((size_t)(end - p) + 1);
  if (!out)
    return NULL;
  char *w = out;
  while (p < end && *p != '"') {
    if (*p == '\\' && p + 1 < end)
      p++;
    *w++ = *p++;
  }
  if (p < end && *p == '"')
    p++;
  *w = '\0';
  *pp = p;
  return out;
}

static const char *ny_pkg_find_matching_brace(const char *open, const char *end) {
  int depth = 0;
  bool quote = false;
  bool esc = false;
  for (const char *p = open; p < end; ++p) {
    if (quote) {
      if (esc)
        esc = false;
      else if (*p == '\\')
        esc = true;
      else if (*p == '"')
        quote = false;
      continue;
    }
    if (*p == '"') {
      quote = true;
    } else if (*p == '{') {
      depth++;
    } else if (*p == '}') {
      depth--;
      if (depth == 0)
        return p;
    }
  }
  return NULL;
}

static char *ny_pkg_json_object_string(const char *obj, const char *end, const char *key) {
  const char *raw = key ? key : "";
  size_t raw_len = strlen(raw);
  if (raw_len >= 2 && raw[0] == '"' && raw[raw_len - 1] == '"') {
    raw++;
    raw_len -= 2;
  }
  if (raw_len == 0 || raw_len > 240)
    return NULL;
  char pat[256];
  snprintf(pat, sizeof(pat), "\"%.*s\"", (int)raw_len, raw);
  size_t key_len = strlen(pat);
  const char *p = obj;
  while (p && p < end) {
    p = strstr(p, pat);
    if (!p || p >= end)
      break;
    const char *q = ny_pkg_skip_ws(p + key_len, end);
    if (q < end && *q == ':') {
      if (!q || q >= end)
        return NULL;
      q++;
      return ny_pkg_parse_json_string(&q, end);
    }
    p += key_len;
  }
  return NULL;
}

static void ny_pkg_split_source_ref(const char *source, const char *fallback_ref, char **out_source,
                                    char **out_ref) {
  const char *hash = source ? strchr(source, '#') : NULL;
  if (hash && hash[1]) {
    *out_source = ny_pkg_strndup(source, (size_t)(hash - source));
    *out_ref = ny_strdup(hash + 1);
  } else {
    *out_source = ny_strdup(source ? source : "");
    *out_ref = fallback_ref && *fallback_ref ? ny_strdup(fallback_ref) : NULL;
  }
}

static void ny_pkg_parse_json_manifest(ny_pkg_manifest_t *m, const char *txt, const char *end) {
  char *project = ny_pkg_json_object_string(txt, end, "name");
  if (project && *project) {
    free(m->name);
    m->name = project;
  } else {
    free(project);
  }
  ny_pkg_manifest_set_owned(&m->version, ny_pkg_json_object_string(txt, end, "version"));
  ny_pkg_manifest_set_owned(&m->description,
                            ny_pkg_json_object_string(txt, end, "description"));
  ny_pkg_manifest_set_owned(&m->author, ny_pkg_json_object_string(txt, end, "author"));
  ny_pkg_manifest_set_owned(&m->license, ny_pkg_json_object_string(txt, end, "license"));
  ny_pkg_manifest_set_owned(&m->repository, ny_pkg_json_object_string(txt, end, "repository"));

  const char *deps = strstr(txt, "\"dependencies\"");
  if (!deps || deps >= end)
    return;
  const char *open = strchr(deps, '{');
  if (!open || open >= end)
    return;
  const char *close = ny_pkg_find_matching_brace(open, end);
  if (!close)
    return;
  const char *p = open + 1;
  while (p < close) {
    p = ny_pkg_skip_ws(p, close);
    if (p >= close)
      break;
    if (*p == ',') {
      p++;
      continue;
    }
    char *name = ny_pkg_parse_json_string(&p, close);
    if (!name)
      break;
    p = ny_pkg_skip_ws(p, close);
    if (p >= close || *p != ':') {
      free(name);
      break;
    }
    p++;
    p = ny_pkg_skip_ws(p, close);
    char *source = NULL;
    char *ref = NULL;
    if (p < close && *p == '"') {
      source = ny_pkg_parse_json_string(&p, close);
    } else if (p < close && *p == '{') {
      const char *obj_end = ny_pkg_find_matching_brace(p, close);
      if (!obj_end) {
        free(name);
        break;
      }
      source = ny_pkg_json_object_string(p, obj_end, "source");
      if (!source)
        source = ny_pkg_json_object_string(p, obj_end, "git");
      if (!source)
        source = ny_pkg_json_object_string(p, obj_end, "path");
      ref = ny_pkg_json_object_string(p, obj_end, "ref");
      if (!ref)
        ref = ny_pkg_json_object_string(p, obj_end, "version");
      p = obj_end + 1;
    }
    if (source && *source && ny_pkg_name_ok(name)) {
      char *clean_source = NULL;
      char *clean_ref = NULL;
      ny_pkg_split_source_ref(source, ref, &clean_source, &clean_ref);
      ny_pkg_manifest_push(m, name, clean_source, clean_ref);
      free(clean_source);
      free(clean_ref);
    }
    free(name);
    free(source);
    free(ref);
  }
}

static bool ny_pkg_line_field(const char *s, const char *key, char **slot) {
  size_t k = strlen(key);
  if (strncmp(s, key, k) != 0)
    return false;
  const char *v = s + k;
  if (*v == '=') {
    v++;
  } else if (isspace((unsigned char)*v)) {
    while (isspace((unsigned char)*v))
      v++;
  } else {
    return false;
  }
  ny_pkg_manifest_set_owned(slot, ny_pkg_trim_dup(v, strlen(v)));
  return true;
}

static bool ny_pkg_parse_line_meta(ny_pkg_manifest_t *m, const char *s) {
  if (ny_pkg_line_field(s, "package", &m->name))
    return true;
  if (ny_pkg_line_field(s, "name", &m->name))
    return true;
  if (ny_pkg_line_field(s, "version", &m->version))
    return true;
  if (ny_pkg_line_field(s, "description", &m->description))
    return true;
  if (ny_pkg_line_field(s, "author", &m->author))
    return true;
  if (ny_pkg_line_field(s, "license", &m->license))
    return true;
  if (ny_pkg_line_field(s, "repository", &m->repository))
    return true;
  return false;
}

static void ny_pkg_parse_line_manifest(ny_pkg_manifest_t *m, const char *txt) {
  const char *p = txt;
  while (*p) {
    const char *line = p;
    while (*p && *p != '\n')
      p++;
    size_t n = (size_t)(p - line);
    if (*p == '\n')
      p++;
    char *tmp = ny_pkg_trim_dup(line, n);
    if (!tmp || !*tmp || tmp[0] == '#') {
      free(tmp);
      continue;
    }
    char *s = tmp;
    if (ny_pkg_parse_line_meta(m, s)) {
      free(tmp);
      continue;
    }
    if (strncmp(s, "dep ", 4) == 0)
      s += 4;
    char *eq = strchr(s, '=');
    char *sp = NULL;
    if (!eq) {
      for (char *q = s; *q; ++q) {
        if (isspace((unsigned char)*q)) {
          sp = q;
          break;
        }
      }
    }
    char *name = NULL;
    char *source = NULL;
    if (eq) {
      name = ny_pkg_trim_dup(s, (size_t)(eq - s));
      source = ny_pkg_trim_dup(eq + 1, strlen(eq + 1));
    } else if (sp) {
      name = ny_pkg_trim_dup(s, (size_t)(sp - s));
      source = ny_pkg_trim_dup(sp + 1, strlen(sp + 1));
    }
    if (name && source && *source && ny_pkg_name_ok(name)) {
      char *clean_source = NULL;
      char *clean_ref = NULL;
      ny_pkg_split_source_ref(source, NULL, &clean_source, &clean_ref);
      ny_pkg_manifest_push(m, name, clean_source, clean_ref);
      free(clean_source);
      free(clean_ref);
    }
    free(name);
    free(source);
    free(tmp);
  }
}

static void ny_pkg_manifest_default(char *out, size_t out_len) {
  if (ny_pkg_path_exists("ny.pkg.json"))
    snprintf(out, out_len, "%s", "ny.pkg.json");
  else if (ny_pkg_path_exists("nytrix.pkg.json"))
    snprintf(out, out_len, "%s", "nytrix.pkg.json");
  else if (ny_pkg_path_exists("ny.pkg"))
    snprintf(out, out_len, "%s", "ny.pkg");
  else
    snprintf(out, out_len, "%s", "ny.pkg.json");
}

static bool ny_pkg_read_manifest(const char *path, ny_pkg_manifest_t *m) {
  memset(m, 0, sizeof(*m));
  snprintf(m->path, sizeof(m->path), "%s", path && *path ? path : "ny.pkg.json");
  ny_dir_name(m->root, sizeof(m->root), m->path);
  char *txt = ny_read_file(m->path);
  if (!txt)
    return false;
  const char *end = txt + strlen(txt);
  ny_pkg_parse_json_manifest(m, txt, end);
  ny_pkg_parse_line_manifest(m, txt);
  free(txt);
  return true;
}

static void ny_pkg_json_str(FILE *f, const char *s) {
  fputc('"', f);
  for (const char *p = s ? s : ""; *p; ++p) {
    if (*p == '"' || *p == '\\')
      fputc('\\', f);
    if (*p == '\n') {
      fputs("\\n", f);
    } else {
      fputc(*p, f);
    }
  }
  fputc('"', f);
}

static void ny_pkg_write_json_field(FILE *f, const char *key, const char *value) {
  if (!value || !*value)
    return;
  fputs("  \"", f);
  fputs(key, f);
  fputs("\": ", f);
  ny_pkg_json_str(f, value);
  fputs(",\n", f);
}

static int ny_pkg_write_manifest(const ny_pkg_manifest_t *m) {
  char parent[4096];
  ny_dir_name(parent, sizeof(parent), m->path);
  ny_ensure_dir_recursive(parent);
  FILE *f = fopen(m->path, "wb");
  if (!f) {
    nyt_err("ny pkg", "cannot write %s: %s", m->path, strerror(errno));
    return 1;
  }
  fputs("{\n  \"schema\": \"ny.pkg.v1\",\n  \"name\": ", f);
  ny_pkg_json_str(f, m->name && *m->name ? m->name : "app");
  fputs(",\n", f);
  ny_pkg_write_json_field(f, "version", m->version);
  ny_pkg_write_json_field(f, "description", m->description);
  ny_pkg_write_json_field(f, "author", m->author);
  ny_pkg_write_json_field(f, "license", m->license);
  ny_pkg_write_json_field(f, "repository", m->repository);
  fputs("  \"dependencies\": {\n", f);
  for (size_t i = 0; i < m->len; ++i) {
    const ny_pkg_dep_t *d = &m->items[i];
    fputs("    ", f);
    ny_pkg_json_str(f, d->name);
    fputs(": {\"source\": ", f);
    ny_pkg_json_str(f, d->source);
    if (d->ref && *d->ref) {
      fputs(", \"ref\": ", f);
      ny_pkg_json_str(f, d->ref);
    }
    fputs("}", f);
    fputs(i + 1 == m->len ? "\n" : ",\n", f);
  }
  fputs("  }\n}\n", f);
  fclose(f);
  return 0;
}

static void ny_pkg_global_root(char *out, size_t out_len) {
  const char *env = getenv("NYTRIX_PKG_HOME");
  if (env && *env) {
    snprintf(out, out_len, "%s", env);
    return;
  }
  const char *home = getenv("HOME");
#ifdef _WIN32
  if (!home || !*home)
    home = getenv("USERPROFILE");
#endif
  if (!home || !*home)
    home = ny_get_temp_dir();
  snprintf(out, out_len, "%s/.nytrix/pkg", home);
}

static void ny_pkg_system_root(char *out, size_t out_len) {
  const char *env = getenv("NYTRIX_SYSTEM_PKG_HOME");
  snprintf(out, out_len, "%s", (env && *env) ? env : "/usr/local/share/nytrix/pkg");
}

static void ny_pkg_install_root(const ny_pkg_opts_t *opts, const ny_pkg_manifest_t *m, char *out,
                                size_t out_len) {
  if (opts->install_root[0]) {
    snprintf(out, out_len, "%s", opts->install_root);
  } else if (opts->venv) {
    char venv_dir[4096];
    ny_pkg_join(venv_dir, sizeof(venv_dir), m && m->root[0] ? m->root : ".", ".nytrix/venv");
    ny_pkg_join(out, out_len, venv_dir, "lib");
  } else if (opts->vendor) {
    char vendor_dir[4096];
    ny_pkg_join(vendor_dir, sizeof(vendor_dir), m && m->root[0] ? m->root : ".", "vendor");
    ny_pkg_join(out, out_len, vendor_dir, "ny_modules");
  } else if (opts->system) {
    ny_pkg_system_root(out, out_len);
  } else if (opts->global) {
    ny_pkg_global_root(out, out_len);
  } else {
    ny_pkg_join(out, out_len, m && m->root[0] ? m->root : ".", "ny_modules");
  }
}

static bool ny_pkg_starts_with(const char *s, const char *prefix) {
  return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool ny_pkg_ends_with_ci(const char *s, const char *suffix) {
  if (!s || !suffix)
    return false;
  size_t n = strlen(s);
  size_t m = strlen(suffix);
  if (m > n)
    return false;
  s += n - m;
  for (size_t i = 0; i < m; ++i) {
    if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i]))
      return false;
  }
  return true;
}

static bool ny_pkg_source_is_url(const char *source) {
  return ny_pkg_starts_with(source, "http://") || ny_pkg_starts_with(source, "https://");
}

static const char *ny_pkg_archive_source(const char *source) {
  return ny_pkg_starts_with(source, "archive+") ? source + 8 : source;
}

static bool ny_pkg_source_is_archive(const char *source) {
  if (!source || !*source)
    return false;
  source = ny_pkg_archive_source(source);
  return ny_pkg_ends_with_ci(source, ".tar") || ny_pkg_ends_with_ci(source, ".tar.gz") ||
         ny_pkg_ends_with_ci(source, ".tgz") || ny_pkg_ends_with_ci(source, ".tar.xz") ||
         ny_pkg_ends_with_ci(source, ".txz") || ny_pkg_ends_with_ci(source, ".tar.bz2") ||
         ny_pkg_ends_with_ci(source, ".tbz") || ny_pkg_ends_with_ci(source, ".zip");
}

static bool ny_pkg_source_is_git(const char *source) {
  if (!source || !*source)
    return false;
  size_t n = strlen(source);
  return ny_pkg_starts_with(source, "git+") || ny_pkg_starts_with(source, "git@") ||
         ny_pkg_starts_with(source, "ssh://") || (n > 4 && strcmp(source + n - 4, ".git") == 0);
}

static const char *ny_pkg_git_url(const char *source) {
  return source && strncmp(source, "git+", 4) == 0 ? source + 4 : source;
}

static int ny_pkg_copy_local(const char *source, const char *dest, bool force, bool verbose) {
  char parent[4096];
  ny_dir_name(parent, sizeof(parent), dest);
  ny_ensure_dir_recursive(parent);
  if (ny_pkg_path_exists(dest) && !force)
    return 0;
  int rc = 0;
  if (force && ny_pkg_path_exists(dest)) {
    const char *rm_argv[] = {"rm", "-rf", dest, NULL};
    rc = ny_pkg_run_argv(rm_argv, verbose);
  }
  if (rc == 0)
    ny_ensure_dir_recursive(dest);
  if (rc == 0) {
    if (ny_pkg_is_dir(source)) {
      const char *cp_argv[] = {"cp", "-R", source, dest, NULL};
      /* Copy the whole directory; the source trailing slash handling is platform-dependent,
       * so use the safer form: cp -R <src> <dst> where dst is the parent. */
      const char *cp_dir_argv[] = {"cp", "-R", /* filled below */ NULL, NULL};
      char src_slash[4096];
      snprintf(src_slash, sizeof(src_slash), "%s/.", source);
      cp_dir_argv[2] = src_slash;
      cp_dir_argv[3] = dest;
      rc = ny_pkg_run_argv(cp_dir_argv, verbose);
    } else {
      char mod_path[4096];
      ny_pkg_join(mod_path, sizeof(mod_path), dest, "mod.ny");
      const char *cp_argv[] = {"cp", source, mod_path, NULL};
      rc = ny_pkg_run_argv(cp_argv, verbose);
    }
  }
  return rc;
}

static int ny_pkg_remove_path(const char *path, bool verbose) {
  if (!ny_pkg_path_exists(path))
    return 0;
  const char *rm_argv[] = {"rm", "-rf", path, NULL};
  return ny_pkg_run_argv(rm_argv, verbose);
}

static int ny_pkg_install_archive(const char *source, const char *dest, bool force, bool verbose,
                                  bool strip_single_root) {
  source = ny_pkg_archive_source(source);
  char parent[4096];
  ny_dir_name(parent, sizeof(parent), dest);
  ny_ensure_dir_recursive(parent);
  if (ny_pkg_path_exists(dest) && !force)
    return 0;

  char tmp_dir[4096];
  snprintf(tmp_dir, sizeof(tmp_dir), "%s/ny-pkg-%ld-%s", ny_get_temp_dir(), (long)getpid(),
           strrchr(dest, '/') ? strrchr(dest, '/') + 1 : "archive");
  char archive_path[4096];
  bool downloaded = ny_pkg_source_is_url(source);
  if (downloaded) {
    snprintf(archive_path, sizeof(archive_path), "%s.pkg", tmp_dir);
  } else {
    snprintf(archive_path, sizeof(archive_path), "%s", source);
  }

  int rc = ny_pkg_remove_path(tmp_dir, verbose);
  if (rc == 0)
    rc = ny_pkg_remove_path(dest, verbose);
  if (rc == 0)
    ny_ensure_dir_recursive(dest);
  if (rc == 0) {
    const char *mkdir_argv[] = {"mkdir", "-p", tmp_dir, NULL};
    rc = ny_pkg_run_argv(mkdir_argv, verbose);
  }
  if (rc == 0 && downloaded) {
    /* Try curl, fall back to wget */
    const char *curl_argv[] = {"curl", "-fsSL", "-o", archive_path, source, NULL};
    rc = ny_pkg_run_argv(curl_argv, verbose);
    if (rc != 0) {
      const char *wget_argv[] = {"wget", "-q", "-O", archive_path, source, NULL};
      rc = ny_pkg_run_argv(wget_argv, verbose);
    }
  }
  if (rc == 0) {
    if (ny_pkg_ends_with_ci(source, ".zip")) {
      const char *unzip_argv[] = {"unzip", "-q", archive_path, "-d", tmp_dir, NULL};
      rc = ny_pkg_run_argv(unzip_argv, verbose);
    } else {
      const char *tar_argv[] = {"tar", "-xf", archive_path, "-C", tmp_dir, NULL};
      rc = ny_pkg_run_argv(tar_argv, verbose);
    }
  }
  if (rc == 0) {
    if (strip_single_root) {
      /* Native check: count top-level entries in tmp_dir, if exactly one and it is
       * a directory, move its contents instead of the directory itself. */
      int top_count = 0;
      char first_entry[4096];
      first_entry[0] = '\0';
#ifndef _WIN32
      DIR *d = opendir(tmp_dir);
      if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
          if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' ||
              (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
          top_count++;
          if (top_count == 1) {
            snprintf(first_entry, sizeof(first_entry), "%s/%s", tmp_dir, ent->d_name);
          }
        }
        closedir(d);
      }
#endif
      if (top_count == 1 && first_entry[0] && ny_pkg_is_dir(first_entry)) {
        /* Move contents of the single root directory into dest */
        const char *cp_argv[] = {"cp", "-R", /* src/., dst */ NULL, NULL};
        char src_dot[4096];
        snprintf(src_dot, sizeof(src_dot), "%s/.", first_entry);
        cp_argv[2] = src_dot;
        cp_argv[3] = dest;
        rc = ny_pkg_run_argv(cp_argv, verbose);
      } else {
        const char *cp_argv[] = {"cp", "-R", /* tmp/., dst */ NULL, NULL};
        char src_dot[4096];
        snprintf(src_dot, sizeof(src_dot), "%s/.", tmp_dir);
        cp_argv[2] = src_dot;
        cp_argv[3] = dest;
        rc = ny_pkg_run_argv(cp_argv, verbose);
      }
    } else {
      const char *cp_argv[] = {"cp", "-R", /* tmp/., dst */ NULL, NULL};
      char src_dot[4096];
      snprintf(src_dot, sizeof(src_dot), "%s/.", tmp_dir);
      cp_argv[2] = src_dot;
      cp_argv[3] = dest;
      rc = ny_pkg_run_argv(cp_argv, verbose);
    }
  }
  if (downloaded)
    (void)ny_pkg_remove_path(archive_path, verbose);
  (void)ny_pkg_remove_path(tmp_dir, verbose);
  return rc;
}

static int ny_pkg_install_git(const char *source, const char *ref, const char *dest, bool force,
                              bool verbose) {
  char parent[4096];
  ny_dir_name(parent, sizeof(parent), dest);
  ny_ensure_dir_recursive(parent);
  char git_dir[4096];
  ny_pkg_join(git_dir, sizeof(git_dir), dest, ".git");
  int rc = 0;
  if (force && ny_pkg_path_exists(dest)) {
    rc = ny_pkg_remove_path(dest, verbose);
  }
  if (rc == 0) {
    if (ny_pkg_is_dir(git_dir)) {
      const char *remote_argv[] = {"git", "-C", dest, "remote", "set-url", "origin",
                                  ny_pkg_git_url(source), NULL};
      rc = ny_pkg_run_argv(remote_argv, verbose);
      if (rc == 0) {
        const char *fetch_argv[] = {"git", "-C", dest, "fetch", "--all", "--tags", "--prune", NULL};
        rc = ny_pkg_run_argv(fetch_argv, verbose);
      }
    } else if (ny_pkg_path_exists(dest)) {
      nyt_err("ny pkg", "%s exists and is not a git checkout (use --force)", dest);
      rc = 1;
    } else {
      const char *clone_argv[] = {"git", "clone", ny_pkg_git_url(source), dest, NULL};
      rc = ny_pkg_run_argv(clone_argv, verbose);
    }
  }
  if (rc == 0 && ref && *ref) {
    const char *co_argv[] = {"git", "-C", dest, "-c", "advice.detachedHead=false",
                               "checkout", ref, NULL};
    rc = ny_pkg_run_argv(co_argv, verbose);
  } else if (rc == 0 && ny_pkg_is_dir(git_dir)) {
    const char *pull_argv[] = {"git", "-C", dest, "pull", "--ff-only", NULL};
    (void)ny_pkg_run_argv(pull_argv, verbose);
  }
  return rc;
}

static char *ny_pkg_git_commit(const char *dest) {
  char git_dir[4096];
  ny_pkg_join(git_dir, sizeof(git_dir), dest, ".git");
  if (!ny_pkg_is_dir(git_dir))
    return NULL;
  const char *argv[] = {"git", "-C", dest, "rev-parse", "HEAD", NULL};
  return ny_pkg_capture_argv(argv);
}

static void ny_pkg_config_home(char *out, size_t out_len) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    snprintf(out, out_len, "%s", xdg);
    return;
  }
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
  if (!home || !*home)
    home = getenv("APPDATA");
#else
  const char *home = getenv("HOME");
#endif
  if (home && *home)
    ny_pkg_join(out, out_len, home, ".config");
  else if (out_len > 0)
    out[0] = '\0';
}

static void ny_pkg_config_path(size_t idx, char *out, size_t out_len) {
  if (out_len == 0)
    return;
  out[0] = '\0';
  if (idx == 0) {
    const char *explicit_path = getenv("NYTRIX_CONFIG");
    if (!explicit_path || !*explicit_path)
      explicit_path = getenv("NY_CONFIG");
    snprintf(out, out_len, "%s", explicit_path && *explicit_path ? explicit_path : "");
    return;
  }
  if (idx == 1) {
    char dir[4096];
    ny_pkg_join(dir, sizeof(dir), ".nytrix", "config");
    snprintf(out, out_len, "%s", dir);
    return;
  }
  if (idx == 2) {
    snprintf(out, out_len, "%s", "nytrix.config");
    return;
  }
  char base[4096];
  ny_pkg_config_home(base, sizeof(base));
  if (!base[0])
    return;
  if (idx == 3) {
    char dir[4096];
    ny_pkg_join(dir, sizeof(dir), base, "nytrix");
    ny_pkg_join(out, out_len, dir, "config");
  } else if (idx == 4) {
    char dir[4096];
    ny_pkg_join(dir, sizeof(dir), base, "ny");
    ny_pkg_join(out, out_len, dir, "config");
  }
}

static size_t ny_pkg_registry_path_count(void) { return 9; }

static void ny_pkg_registry_path(size_t idx, char *out, size_t out_len) {
  if (idx == 0) {
    const char *env = getenv("NYTRIX_PKG_REGISTRY");
    snprintf(out, out_len, "%s", env && *env ? env : "");
    return;
  }
  if (idx == 1) {
    snprintf(out, out_len, "%s", ".ny.registry");
    return;
  }
  if (idx == 2) {
    snprintf(out, out_len, "%s", "ny.registry");
    return;
  }
  if (idx >= 3 && idx < 8) {
    ny_pkg_config_path(idx - 3, out, out_len);
    return;
  }
  char home_root[4096];
  ny_pkg_global_root(home_root, sizeof(home_root));
  ny_pkg_join(out, out_len, home_root, "registry");
}

static void ny_pkg_registry_write_path(const ny_pkg_opts_t *opts, char *out, size_t out_len) {
  if (opts && opts->system) {
    char root[4096];
    ny_pkg_system_root(root, sizeof(root));
    ny_pkg_join(out, out_len, root, "registry");
    return;
  }
  if (opts && opts->global) {
    char root[4096];
    ny_pkg_global_root(root, sizeof(root));
    ny_pkg_join(out, out_len, root, "registry");
    return;
  }
  char root[4096];
  ny_dir_name(root, sizeof(root), opts && opts->manifest[0] ? opts->manifest : "ny.pkg.json");
  ny_pkg_join(out, out_len, root, ".ny.registry");
}

static bool ny_pkg_parse_registry_pair(const char *line, const char *prefix, char **out_name,
                                       char **out_source) {
  *out_name = NULL;
  *out_source = NULL;
  char *tmp = ny_pkg_trim_dup(line, strlen(line));
  if (!tmp || !*tmp || tmp[0] == '#') {
    free(tmp);
    return false;
  }
  char *s = tmp;
  if (prefix && *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) != 0 || !isspace((unsigned char)s[n])) {
      free(tmp);
      return false;
    }
    s += n;
    while (isspace((unsigned char)*s))
      s++;
  } else if (strncmp(s, "repo ", 5) == 0) {
    free(tmp);
    return false;
  }

  char *name_start = s;
  while (*s && !isspace((unsigned char)*s) && *s != '=')
    s++;
  if (s == name_start) {
    free(tmp);
    return false;
  }
  char *name = ny_pkg_trim_dup(name_start, (size_t)(s - name_start));
  while (isspace((unsigned char)*s))
    s++;
  if (*s == '=')
    s++;
  while (isspace((unsigned char)*s))
    s++;
  char *source = ny_pkg_trim_dup(s, strlen(s));
  if (!name || !*name || !source || !*source) {
    free(name);
    free(source);
    free(tmp);
    return false;
  }
  *out_name = name;
  *out_source = source;
  free(tmp);
  return true;
}

static char *ny_pkg_registry_direct_lookup(const char *name) {
  if (!name || !*name)
    return NULL;
  for (size_t idx = 0; idx < ny_pkg_registry_path_count(); ++idx) {
    char path[4096];
    ny_pkg_registry_path(idx, path, sizeof(path));
    if (!path[0])
      continue;
    char *txt = ny_read_file(path);
    if (!txt)
      continue;
    const char *p = txt;
    while (*p) {
      const char *line = p;
      while (*p && *p != '\n')
        p++;
      size_t n = (size_t)(p - line);
      if (*p == '\n')
        p++;
      char *tmp = ny_pkg_strndup(line, n);
      char *entry = NULL;
      char *source = NULL;
      bool ok = tmp && ny_pkg_parse_registry_pair(tmp, NULL, &entry, &source);
      free(tmp);
      if (ok && strcmp(entry, name) == 0) {
        free(entry);
        free(txt);
        return source;
      }
      free(entry);
      free(source);
    }
    free(txt);
  }
  return NULL;
}

static char *ny_pkg_registry_repo_lookup(const char *repo_name) {
  if (!repo_name || !*repo_name)
    return NULL;
  for (size_t idx = 0; idx < ny_pkg_registry_path_count(); ++idx) {
    char path[4096];
    ny_pkg_registry_path(idx, path, sizeof(path));
    if (!path[0])
      continue;
    char *txt = ny_read_file(path);
    if (!txt)
      continue;
    const char *p = txt;
    while (*p) {
      const char *line = p;
      while (*p && *p != '\n')
        p++;
      size_t n = (size_t)(p - line);
      if (*p == '\n')
        p++;
      char *tmp = ny_pkg_strndup(line, n);
      char *name = NULL;
      char *source = NULL;
      bool ok = tmp && ny_pkg_parse_registry_pair(tmp, "repo", &name, &source);
      free(tmp);
      if (ok && strcmp(name, repo_name) == 0) {
        free(name);
        free(txt);
        return source;
      }
      free(name);
      free(source);
    }
    free(txt);
  }
  return NULL;
}

static void ny_pkg_repo_cache_path(const char *repo_name, char *out, size_t out_len) {
  char root[4096];
  char repos[4096];
  ny_pkg_global_root(root, sizeof(root));
  ny_pkg_join(repos, sizeof(repos), root, "repos");
  ny_pkg_join(out, out_len, repos, repo_name);
}

static char *ny_pkg_repo_package_candidate(const char *repo_root, const char *pkg_name) {
  char path[4096];
  ny_pkg_join(path, sizeof(path), repo_root, pkg_name);
  if (ny_pkg_path_exists(path))
    return ny_strdup(path);
  char file[4096];
  snprintf(file, sizeof(file), "%s.ny", path);
  if (ny_pkg_path_exists(file))
    return ny_strdup(file);
  return NULL;
}

static bool ny_pkg_repo_entry_is_package(const char *path) {
  if (!path || !*path)
    return false;
  if (ny_pkg_is_dir(path)) {
    char child[4096];
    ny_pkg_join(child, sizeof(child), path, "mod.ny");
    if (ny_pkg_path_exists(child))
      return true;
    ny_pkg_join(child, sizeof(child), path, "ny.pkg.json");
    if (ny_pkg_path_exists(child))
      return true;
    ny_pkg_join(child, sizeof(child), path, "nytrix.pkg.json");
    if (ny_pkg_path_exists(child))
      return true;
    ny_pkg_join(child, sizeof(child), path, "ny.pkg");
    return ny_pkg_path_exists(child);
  }
  return ny_pkg_ends_with_ci(path, ".ny");
}

static void ny_pkg_repo_manifest_path(const char *pkg_path, char *out, size_t out_len) {
  if (!out || out_len == 0)
    return;
  out[0] = '\0';
  if (!ny_pkg_is_dir(pkg_path))
    return;
  static const char *names[] = {"ny.pkg.json", "nytrix.pkg.json", "ny.pkg"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char path[4096];
    ny_pkg_join(path, sizeof(path), pkg_path, names[i]);
    if (ny_pkg_path_exists(path)) {
      snprintf(out, out_len, "%s", path);
      return;
    }
  }
}

static char *ny_pkg_repo_materialize(const char *repo_name, const char *repo_source,
                                     const ny_pkg_opts_t *opts) {
  if (!repo_source || !*repo_source)
    return NULL;
  if (!ny_pkg_source_is_git(repo_source) && !ny_pkg_source_is_archive(repo_source))
    return ny_strdup(repo_source);

  char cache[4096];
  ny_pkg_repo_cache_path(repo_name, cache, sizeof(cache));
  int rc = 0;
  if (ny_pkg_source_is_git(repo_source)) {
    rc = ny_pkg_install_git(repo_source, NULL, cache, opts && opts->force, opts && opts->verbose);
  } else {
    rc = ny_pkg_install_archive(repo_source, cache, opts && opts->force,
                                opts && opts->verbose, false);
  }
  return rc == 0 ? ny_strdup(cache) : NULL;
}

static char *ny_pkg_registry_repo_pkg_lookup(const char *pkg_name, const ny_pkg_opts_t *opts) {
  if (!pkg_name || !*pkg_name)
    return NULL;
  for (size_t idx = 0; idx < ny_pkg_registry_path_count(); ++idx) {
    char path[4096];
    ny_pkg_registry_path(idx, path, sizeof(path));
    if (!path[0])
      continue;
    char *txt = ny_read_file(path);
    if (!txt)
      continue;
    const char *p = txt;
    while (*p) {
      const char *line = p;
      while (*p && *p != '\n')
        p++;
      size_t n = (size_t)(p - line);
      if (*p == '\n')
        p++;
      char *tmp = ny_pkg_strndup(line, n);
      char *repo_name = NULL;
      char *repo_source = NULL;
      bool ok = tmp && ny_pkg_parse_registry_pair(tmp, "repo", &repo_name, &repo_source);
      free(tmp);
      if (!ok) {
        free(repo_name);
        free(repo_source);
        continue;
      }
      char *repo_root = ny_pkg_repo_materialize(repo_name, repo_source, opts);
      char *candidate = repo_root ? ny_pkg_repo_package_candidate(repo_root, pkg_name) : NULL;
      free(repo_root);
      if (candidate) {
        size_t need = strlen(repo_name) + strlen(pkg_name) + 7;
        char *source = (char *)malloc(need);
        if (source)
          snprintf(source, need, "repo+%s/%s", repo_name, pkg_name);
        free(candidate);
        free(repo_name);
        free(repo_source);
        free(txt);
        return source;
      }
      free(repo_name);
      free(repo_source);
    }
    free(txt);
  }
  return NULL;
}

static char *ny_pkg_registry_lookup(const char *name, const ny_pkg_opts_t *opts) {
  char *source = ny_pkg_registry_direct_lookup(name);
  if (source)
    return source;
  return ny_pkg_registry_repo_pkg_lookup(name, opts);
}

static bool ny_pkg_source_is_repo(const char *source) {
  return ny_pkg_starts_with(source, "repo+") || ny_pkg_starts_with(source, "repo:");
}

static char *ny_pkg_resolve_repo_source(const char *source, const ny_pkg_opts_t *opts) {
  if (!ny_pkg_source_is_repo(source))
    return NULL;
  const char *spec = source + 5;
  const char *slash = strchr(spec, '/');
  if (!slash || slash == spec || !slash[1])
    return NULL;
  char *repo_name = ny_pkg_strndup(spec, (size_t)(slash - spec));
  char *pkg_name = ny_strdup(slash + 1);
  char *repo_source = repo_name ? ny_pkg_registry_repo_lookup(repo_name) : NULL;
  char *repo_root = repo_source ? ny_pkg_repo_materialize(repo_name, repo_source, opts) : NULL;
  char *candidate = repo_root ? ny_pkg_repo_package_candidate(repo_root, pkg_name) : NULL;
  if (!candidate)
    nyt_err("ny pkg", "repo package not found: %s", source);
  free(repo_name);
  free(pkg_name);
  free(repo_source);
  free(repo_root);
  return candidate;
}

static int ny_pkg_write_lock(const char *manifest_path, const ny_pkg_lock_t *lock) {
  char lock_path[4096];
  snprintf(lock_path, sizeof(lock_path), "%s.lock",
           manifest_path && *manifest_path ? manifest_path : "ny.pkg.json");
  FILE *f = fopen(lock_path, "wb");
  if (!f) {
    nyt_err("ny pkg", "cannot write %s: %s", lock_path, strerror(errno));
    return 1;
  }
  fputs("{\n  \"schema\": \"ny.pkg.lock.v1\",\n  \"packages\": [\n", f);
  for (size_t i = 0; i < lock->len; ++i) {
    const ny_pkg_lock_entry_t *e = &lock->items[i];
    fputs("    {\"name\": ", f);
    ny_pkg_json_str(f, e->dep.name);
    fputs(", \"source\": ", f);
    ny_pkg_json_str(f, e->dep.source);
    if (e->dep.ref && *e->dep.ref) {
      fputs(", \"ref\": ", f);
      ny_pkg_json_str(f, e->dep.ref);
    }
    fputs(", \"path\": ", f);
    ny_pkg_json_str(f, e->path);
    if (e->commit && *e->commit) {
      fputs(", \"commit\": ", f);
      ny_pkg_json_str(f, e->commit);
    }
    fputs("}", f);
    fputs(i + 1 == lock->len ? "\n" : ",\n", f);
  }
  fputs("  ]\n}\n", f);
  fclose(f);
  return 0;
}

static void ny_pkg_remove_lockfile(const char *manifest_path) {
  char lock_path[4096];
  snprintf(lock_path, sizeof(lock_path), "%s.lock",
           manifest_path && *manifest_path ? manifest_path : "ny.pkg.json");
  (void)remove(lock_path);
}

static int ny_pkg_install_dep(const ny_pkg_dep_t *dep, const ny_pkg_opts_t *opts,
                              const ny_pkg_manifest_t *manifest, ny_pkg_lock_t *lock, int depth);

static void ny_pkg_install_nested(const char *dest, const ny_pkg_opts_t *opts,
                                  const ny_pkg_manifest_t *root_manifest, ny_pkg_lock_t *lock,
                                  int depth) {
  if (depth >= 16)
    return;
  const char *names[] = {"ny.pkg.json", "nytrix.pkg.json", "ny.pkg"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char path[4096];
    ny_pkg_join(path, sizeof(path), dest, names[i]);
    ny_pkg_manifest_t child;
    if (!ny_pkg_read_manifest(path, &child))
      continue;
    for (size_t j = 0; j < child.len; ++j)
      (void)ny_pkg_install_dep(&child.items[j], opts, root_manifest, lock, depth + 1);
    ny_pkg_manifest_free(&child);
    return;
  }
}

static int ny_pkg_install_dep(const ny_pkg_dep_t *dep, const ny_pkg_opts_t *opts,
                              const ny_pkg_manifest_t *manifest, ny_pkg_lock_t *lock, int depth) {
  if (!dep || !ny_pkg_name_ok(dep->name) || !dep->source || !*dep->source)
    return 1;
  char root[4096];
  ny_pkg_install_root(opts, manifest, root, sizeof(root));
  ny_ensure_dir_recursive(root);
  char dest[4096];
  ny_pkg_join(dest, sizeof(dest), root, dep->name);

  char *resolved_source = ny_pkg_resolve_repo_source(dep->source, opts);
  const char *source = resolved_source ? resolved_source : dep->source;
  int rc = 0;
  if (ny_pkg_source_is_archive(source)) {
    rc = ny_pkg_install_archive(source, dest, opts->force, opts->verbose, true);
  } else if (ny_pkg_source_is_git(source)) {
    rc = ny_pkg_install_git(source, dep->ref, dest, opts->force, opts->verbose);
  } else {
    rc = ny_pkg_copy_local(source, dest, opts->force, opts->verbose);
  }
  if (rc != 0) {
    free(resolved_source);
    return rc;
  }
  char *commit = ny_pkg_git_commit(dest);
  ny_pkg_lock_push(lock, dep, dest, commit);
  free(commit);
  free(resolved_source);
  ny_pkg_install_nested(dest, opts, manifest, lock, depth);
  nyt_msg("PKG", NYT_GREEN, "installed %s -> %s", dep->name, dest);
  return 0;
}

static int ny_pkg_install_manifest(ny_pkg_manifest_t *m, const ny_pkg_opts_t *opts,
                                   const char *only_name) {
  ny_pkg_lock_t lock = {0};
  int rc = 0;
  for (size_t i = 0; i < m->len; ++i) {
    if (only_name && *only_name && strcmp(m->items[i].name, only_name) != 0)
      continue;
    if (ny_pkg_install_dep(&m->items[i], opts, m, &lock, 0) != 0)
      rc = 1;
  }
  if (rc == 0)
    rc = ny_pkg_write_lock(m->path, &lock);
  ny_pkg_lock_free(&lock);
  return rc;
}

static int ny_pkg_cmd_init(int argc, char **argv, ny_pkg_opts_t *opts) {
  const char *name = opts->init_name && *opts->init_name ? opts->init_name
                     : (argc > 0 ? argv[0] : "app");
  if (!ny_pkg_name_ok(name)) {
    nyt_err("ny pkg", "invalid package name: %s", name ? name : "");
    return 2;
  }
  ny_pkg_manifest_t m = {0};
  snprintf(m.path, sizeof(m.path), "%s", opts->manifest);
  ny_dir_name(m.root, sizeof(m.root), m.path);
  m.name = ny_strdup(name && *name ? name : "app");
  char *guessed_author = NULL;
  if (!opts->init_author)
    guessed_author = ny_pkg_guess_author();
  ny_pkg_manifest_set_meta(&m, opts->init_version ? opts->init_version : "0.1.0",
                           opts->init_description,
                           opts->init_author ? opts->init_author : guessed_author,
                           opts->init_license, opts->init_repository);
  if (ny_pkg_path_exists(m.path) && !opts->force) {
    nyt_err("ny pkg", "%s already exists (use --force)", m.path);
    free(guessed_author);
    ny_pkg_manifest_free(&m);
    return 1;
  }
  int rc = ny_pkg_write_manifest(&m);
  if (rc == 0)
    nyt_msg("PKG", NYT_GREEN, "created %s", m.path);
  free(guessed_author);
  ny_pkg_manifest_free(&m);
  return rc;
}

static int ny_pkg_cmd_add(int argc, char **argv, ny_pkg_opts_t *opts, const char *default_ref) {
  if (argc < 1) {
    ny_pkg_usage();
    return 2;
  }
  char *name = NULL;
  char *source = NULL;
  const char *eq = strchr(argv[0], '=');
  if (eq) {
    name = ny_pkg_trim_dup(argv[0], (size_t)(eq - argv[0]));
    source = ny_pkg_trim_dup(eq + 1, strlen(eq + 1));
  } else {
    name = ny_strdup(argv[0]);
    source = argc > 1 ? ny_strdup(argv[1]) : ny_pkg_registry_lookup(name, opts);
  }
  if (!ny_pkg_name_ok(name) || !source || !*source) {
    nyt_err("ny pkg", "add needs <name> <source> or a registry entry for <name>");
    free(name);
    free(source);
    return 2;
  }
  char *clean_source = NULL;
  char *clean_ref = NULL;
  ny_pkg_split_source_ref(source, default_ref, &clean_source, &clean_ref);
  ny_pkg_manifest_t m;
  if (!ny_pkg_read_manifest(opts->manifest, &m)) {
    memset(&m, 0, sizeof(m));
    snprintf(m.path, sizeof(m.path), "%s", opts->manifest);
    ny_dir_name(m.root, sizeof(m.root), m.path);
    m.name = ny_strdup("app");
    ny_pkg_manifest_set_meta(&m, opts->init_version ? opts->init_version : "0.1.0",
                             opts->init_description, opts->init_author, opts->init_license,
                             opts->init_repository);
  }
  ny_pkg_manifest_push(&m, name, clean_source, clean_ref);
  int rc = ny_pkg_write_manifest(&m);
  if (rc == 0)
    rc = ny_pkg_install_manifest(&m, opts, name);
  free(name);
  free(source);
  free(clean_source);
  free(clean_ref);
  ny_pkg_manifest_free(&m);
  return rc;
}

static int ny_pkg_cmd_install(int argc, char **argv, ny_pkg_opts_t *opts, const char *ref) {
  if (argc > 0)
    return ny_pkg_cmd_add(argc, argv, opts, ref);
  ny_pkg_manifest_t m;
  if (!ny_pkg_read_manifest(opts->manifest, &m)) {
    nyt_err("ny pkg", "manifest not found: %s", opts->manifest);
    return 1;
  }
  int rc = ny_pkg_install_manifest(&m, opts, NULL);
  ny_pkg_manifest_free(&m);
  return rc;
}

static int ny_pkg_cmd_list(ny_pkg_opts_t *opts) {
  ny_pkg_manifest_t m;
  if (!ny_pkg_read_manifest(opts->manifest, &m)) {
    nyt_err("ny pkg", "manifest not found: %s", opts->manifest);
    return 1;
  }
  for (size_t i = 0; i < m.len; ++i) {
    printf("%s %s", m.items[i].name, m.items[i].source);
    if (m.items[i].ref && *m.items[i].ref)
      printf("#%s", m.items[i].ref);
    putchar('\n');
  }
  ny_pkg_manifest_free(&m);
  return 0;
}

static int ny_pkg_cmd_uninstall(int argc, char **argv, ny_pkg_opts_t *opts) {
  if (argc < 1 || !ny_pkg_name_ok(argv[0])) {
    nyt_err("ny pkg", "uninstall needs a package name");
    return 2;
  }
  const char *name = argv[0];
  ny_pkg_manifest_t m;
  bool have = ny_pkg_read_manifest(opts->manifest, &m);
  if (!have) {
    memset(&m, 0, sizeof(m));
    snprintf(m.path, sizeof(m.path), "%s", opts->manifest);
    snprintf(m.root, sizeof(m.root), ".");
  }

  char root[4096];
  ny_pkg_install_root(opts, &m, root, sizeof(root));
  char dest[4096];
  ny_pkg_join(dest, sizeof(dest), root, name);
  int rc = ny_pkg_remove_path(dest, opts->verbose);
  bool changed = ny_pkg_manifest_remove(&m, name);
  if (rc == 0 && have && changed)
    rc = ny_pkg_write_manifest(&m);
  if (rc == 0) {
    ny_pkg_remove_lockfile(m.path);
    nyt_msg("PKG", NYT_GREEN, "removed %s -> %s", name, dest);
  }
  ny_pkg_manifest_free(&m);
  return rc;
}

static void ny_pkg_print_meta(const ny_pkg_manifest_t *m) {
  nyt_kv("name", "%s", m->name && *m->name ? m->name : "app");
  nyt_kv("version", "%s", m->version && *m->version ? m->version : "0.0.0");
  if (m->description && *m->description)
    nyt_kv("description", "%s", m->description);
  if (m->author && *m->author)
    nyt_kv("author", "%s", m->author);
  if (m->license && *m->license)
    nyt_kv("license", "%s", m->license);
  if (m->repository && *m->repository)
    nyt_kv("repository", "%s", m->repository);
}

static int ny_pkg_cmd_info(int argc, char **argv, ny_pkg_opts_t *opts) {
  ny_pkg_manifest_t m;
  if (!ny_pkg_read_manifest(opts->manifest, &m)) {
    nyt_err("ny pkg", "manifest not found: %s", opts->manifest);
    return 1;
  }
  char root[4096];
  ny_pkg_install_root(opts, &m, root, sizeof(root));

  if (argc > 0) {
    const char *name = argv[0];
    for (size_t i = 0; i < m.len; ++i) {
      const ny_pkg_dep_t *d = &m.items[i];
      if (strcmp(d->name, name) != 0)
        continue;
      char dest[4096];
      ny_pkg_join(dest, sizeof(dest), root, d->name);
      nyt_heading("Nytrix Package");
      nyt_kv("name", "%s", d->name);
      nyt_kv("source", "%s", d->source);
      if (d->ref && *d->ref)
        nyt_kv("ref", "%s", d->ref);
      nyt_kv("path", "%s", dest);
      nyt_kv("installed", "%s", ny_pkg_path_exists(dest) ? "yes" : "no");
      ny_pkg_manifest_free(&m);
      return 0;
    }
    ny_pkg_manifest_free(&m);
    nyt_err("ny pkg", "package not in manifest: %s", name);
    return 1;
  }

  nyt_heading("Nytrix Package");
  ny_pkg_print_meta(&m);
  nyt_kv("manifest", "%s", m.path);
  nyt_kv("root", "%s", root);
  nyt_subheading("Dependencies");
  if (m.len == 0) {
    printf("  none\n");
  } else {
    for (size_t i = 0; i < m.len; ++i) {
      printf("  %s%s%s %s", nyt_clr(NYT_CYAN), m.items[i].name, nyt_clr(NYT_RESET),
             m.items[i].source);
      if (m.items[i].ref && *m.items[i].ref)
        printf("#%s", m.items[i].ref);
      putchar('\n');
    }
  }
  ny_pkg_manifest_free(&m);
  return 0;
}

static int ny_pkg_cmd_path(int argc, char **argv, ny_pkg_opts_t *opts) {
  ny_pkg_manifest_t m;
  bool have = ny_pkg_read_manifest(opts->manifest, &m);
  if (!have) {
    memset(&m, 0, sizeof(m));
    snprintf(m.root, sizeof(m.root), ".");
  }
  char root[4096];
  ny_pkg_install_root(opts, &m, root, sizeof(root));
  if (argc <= 0) {
    printf("%s\n", root);
  } else {
    char dest[4096];
    ny_pkg_join(dest, sizeof(dest), root, argv[0]);
    printf("%s\n", dest);
  }
  ny_pkg_manifest_free(&m);
  return 0;
}

static int ny_pkg_registry_upsert_repo(const ny_pkg_opts_t *opts, const char *name,
                                       const char *source) {
  if (!ny_pkg_name_ok(name) || !source || !*source) {
    nyt_err("ny pkg", "repo add needs <name> <source>");
    return 2;
  }
  char path[4096];
  ny_pkg_registry_write_path(opts, path, sizeof(path));
  char parent[4096];
  ny_dir_name(parent, sizeof(parent), path);
  ny_ensure_dir_recursive(parent);
  char *txt = ny_read_file(path);
  FILE *f = fopen(path, "wb");
  if (!f) {
    nyt_err("ny pkg", "cannot write %s: %s", path, strerror(errno));
    free(txt);
    return 1;
  }
  if (txt) {
    const char *p = txt;
    while (*p) {
      const char *line = p;
      while (*p && *p != '\n')
        p++;
      size_t n = (size_t)(p - line);
      if (*p == '\n')
        p++;
      char *tmp = ny_pkg_strndup(line, n);
      char *repo_name = NULL;
      char *repo_source = NULL;
      bool is_repo = tmp && ny_pkg_parse_registry_pair(tmp, "repo", &repo_name, &repo_source);
      if (!is_repo || strcmp(repo_name, name) != 0)
        fprintf(f, "%.*s\n", (int)n, line);
      free(tmp);
      free(repo_name);
      free(repo_source);
    }
  }
  fprintf(f, "repo %s = %s\n", name, source);
  fclose(f);
  free(txt);
  char cache[4096];
  ny_pkg_repo_cache_path(name, cache, sizeof(cache));
  (void)ny_pkg_remove_path(cache, opts && opts->verbose);
  nyt_msg("PKG", NYT_GREEN, "repo %s -> %s", name, source);
  return 0;
}

static int ny_pkg_registry_remove_repo(const ny_pkg_opts_t *opts, const char *name) {
  if (!ny_pkg_name_ok(name)) {
    nyt_err("ny pkg", "repo remove needs a repo name");
    return 2;
  }
  char path[4096];
  ny_pkg_registry_write_path(opts, path, sizeof(path));
  char *txt = ny_read_file(path);
  if (!txt)
    return 0;
  FILE *f = fopen(path, "wb");
  if (!f) {
    nyt_err("ny pkg", "cannot write %s: %s", path, strerror(errno));
    free(txt);
    return 1;
  }
  const char *p = txt;
  while (*p) {
    const char *line = p;
    while (*p && *p != '\n')
      p++;
    size_t n = (size_t)(p - line);
    if (*p == '\n')
      p++;
    char *tmp = ny_pkg_strndup(line, n);
    char *repo_name = NULL;
    char *repo_source = NULL;
    bool is_repo = tmp && ny_pkg_parse_registry_pair(tmp, "repo", &repo_name, &repo_source);
    if (!is_repo || strcmp(repo_name, name) != 0)
      fprintf(f, "%.*s\n", (int)n, line);
    free(tmp);
    free(repo_name);
    free(repo_source);
  }
  fclose(f);
  free(txt);
  nyt_msg("PKG", NYT_GREEN, "removed repo %s", name);
  return 0;
}

static int ny_pkg_cmd_repo(int argc, char **argv, ny_pkg_opts_t *opts) {
  const char *cmd = argc > 0 ? argv[0] : "list";
  if (strcmp(cmd, "add") == 0) {
    if (argc < 3)
      return ny_pkg_registry_upsert_repo(opts, "", "");
    return ny_pkg_registry_upsert_repo(opts, argv[1], argv[2]);
  }
  if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0) {
    if (argc < 2)
      return ny_pkg_registry_remove_repo(opts, "");
    return ny_pkg_registry_remove_repo(opts, argv[1]);
  }
  if (strcmp(cmd, "sync") == 0) {
    const char *only = argc > 1 ? argv[1] : NULL;
    int rc = 0;
    for (size_t idx = 0; idx < ny_pkg_registry_path_count(); ++idx) {
      char path[4096];
      ny_pkg_registry_path(idx, path, sizeof(path));
      char *txt = path[0] ? ny_read_file(path) : NULL;
      if (!txt)
        continue;
      const char *p = txt;
      while (*p) {
        const char *line = p;
        while (*p && *p != '\n')
          p++;
        size_t n = (size_t)(p - line);
        if (*p == '\n')
          p++;
        char *tmp = ny_pkg_strndup(line, n);
        char *repo_name = NULL;
        char *repo_source = NULL;
        bool is_repo = tmp && ny_pkg_parse_registry_pair(tmp, "repo", &repo_name, &repo_source);
        if (is_repo && (!only || strcmp(only, repo_name) == 0)) {
          char *root = ny_pkg_repo_materialize(repo_name, repo_source, opts);
          if (root) {
            nyt_msg("PKG", NYT_GREEN, "synced repo %s -> %s", repo_name, root);
          } else {
            rc = 1;
          }
          free(root);
        }
        free(tmp);
        free(repo_name);
        free(repo_source);
      }
      free(txt);
    }
    return rc;
  }
  if (strcmp(cmd, "path") == 0 && argc > 1) {
    char path[4096];
    ny_pkg_repo_cache_path(argv[1], path, sizeof(path));
    printf("%s\n", path);
    return 0;
  }
  if (strcmp(cmd, "list") != 0 && strcmp(cmd, "ls") != 0) {
    nyt_err("ny pkg", "unknown repo command '%s'", cmd);
    return 2;
  }
  printf("%-18s %-72s %s\n", "name", "source", "cache");
  for (size_t idx = 0; idx < ny_pkg_registry_path_count(); ++idx) {
    char path[4096];
    ny_pkg_registry_path(idx, path, sizeof(path));
    char *txt = path[0] ? ny_read_file(path) : NULL;
    if (!txt)
      continue;
    const char *p = txt;
    while (*p) {
      const char *line = p;
      while (*p && *p != '\n')
        p++;
      size_t n = (size_t)(p - line);
      if (*p == '\n')
        p++;
      char *tmp = ny_pkg_strndup(line, n);
      char *repo_name = NULL;
      char *repo_source = NULL;
      bool is_repo = tmp && ny_pkg_parse_registry_pair(tmp, "repo", &repo_name, &repo_source);
      if (is_repo) {
        char cache[4096];
        ny_pkg_repo_cache_path(repo_name, cache, sizeof(cache));
        printf("%-18s %-72s %s\n", repo_name, repo_source, cache);
      }
      free(tmp);
      free(repo_name);
      free(repo_source);
    }
    free(txt);
  }
  return 0;
}

static int ny_pkg_cmd_registry(int argc, char **argv, ny_pkg_opts_t *opts) {
  if (argc > 0) {
    char *source = ny_pkg_registry_lookup(argv[0], opts);
    if (!source)
      return 1;
    printf("%s %s\n", argv[0], source);
    free(source);
    return 0;
  }
  for (size_t idx = 0; idx < ny_pkg_registry_path_count(); ++idx) {
    char path[4096];
    ny_pkg_registry_path(idx, path, sizeof(path));
    if (path[0] && ny_pkg_path_exists(path))
      printf("%s\n", path);
  }
  return 0;
}

static int ny_pkg_char_ci(int c) { return tolower((unsigned char)c); }

static int ny_pkg_fuzzy_score_text(const char *text, const char *needle) {
  if (!needle || !*needle)
    return 0;
  if (!text || !*text)
    return -1;
  size_t tn = strlen(text);
  size_t qn = strlen(needle);
  if (tn == qn) {
    bool exact = true;
    for (size_t i = 0; i < tn; ++i) {
      if (ny_pkg_char_ci(text[i]) != ny_pkg_char_ci(needle[i])) {
        exact = false;
        break;
      }
    }
    if (exact)
      return 0;
  }
  if (tn >= qn) {
    bool prefix = true;
    for (size_t i = 0; i < qn; ++i) {
      if (ny_pkg_char_ci(text[i]) != ny_pkg_char_ci(needle[i])) {
        prefix = false;
        break;
      }
    }
    if (prefix)
      return 8 + (int)(tn - qn);
  }
  for (size_t start = 0; start < tn; ++start) {
    size_t i = 0;
    while (i < qn && start + i < tn &&
           ny_pkg_char_ci(text[start + i]) == ny_pkg_char_ci(needle[i]))
      i++;
    if (i == qn)
      return 40 + (int)start + (int)(tn - qn);
  }

  size_t q = 0;
  int gap = 0;
  int last = -1;
  for (size_t i = 0; i < tn && q < qn; ++i) {
    if (ny_pkg_char_ci(text[i]) != ny_pkg_char_ci(needle[q]))
      continue;
    if (last >= 0)
      gap += (int)i - last - 1;
    last = (int)i;
    q++;
  }
  return q == qn ? 120 + gap * 3 + (int)tn : -1;
}

static int ny_pkg_search_term_score(const ny_pkg_search_entry_t *e, const char *term) {
  int best = ny_pkg_fuzzy_score_text(e->name, term);
  int s = ny_pkg_fuzzy_score_text(e->repo, term);
  if (s >= 0 && (best < 0 || s + 35 < best))
    best = s + 35;
  s = ny_pkg_fuzzy_score_text(e->description, term);
  if (s >= 0 && (best < 0 || s + 80 < best))
    best = s + 80;
  s = ny_pkg_fuzzy_score_text(e->source, term);
  if (s >= 0 && (best < 0 || s + 120 < best))
    best = s + 120;
  return best;
}

static int ny_pkg_search_score(const ny_pkg_search_entry_t *e, const char *query) {
  if (!query || !*query)
    return 1000;
  char *tmp = ny_strdup(query);
  if (!tmp)
    return -1;
  int score = 0;
  char *p = tmp;
  while (*p) {
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p)
      break;
    char *start = p;
    while (*p && !isspace((unsigned char)*p))
      p++;
    if (*p)
      *p++ = '\0';
    int term_score = ny_pkg_search_term_score(e, start);
    if (term_score < 0) {
      free(tmp);
      return -1;
    }
    score += term_score;
  }
  free(tmp);
  return score;
}

static int ny_pkg_search_cmp(const void *a, const void *b) {
  const ny_pkg_search_entry_t *ea = (const ny_pkg_search_entry_t *)a;
  const ny_pkg_search_entry_t *eb = (const ny_pkg_search_entry_t *)b;
  if (ea->score != eb->score)
    return ea->score < eb->score ? -1 : 1;
  return strcmp(ea->name ? ea->name : "", eb->name ? eb->name : "");
}

static void ny_pkg_search_collect_registry(ny_pkg_search_list_t *out) {
  for (size_t idx = 0; idx < ny_pkg_registry_path_count(); ++idx) {
    char path[4096];
    ny_pkg_registry_path(idx, path, sizeof(path));
    char *txt = path[0] ? ny_read_file(path) : NULL;
    if (!txt)
      continue;
    const char *p = txt;
    while (*p) {
      const char *line = p;
      while (*p && *p != '\n')
        p++;
      size_t n = (size_t)(p - line);
      if (*p == '\n')
        p++;
      char *tmp = ny_pkg_strndup(line, n);
      char *name = NULL;
      char *source = NULL;
      bool ok = tmp && ny_pkg_parse_registry_pair(tmp, NULL, &name, &source);
      if (ok)
        ny_pkg_search_push(out, name, source, "registry", NULL, path, NULL, NULL);
      free(tmp);
      free(name);
      free(source);
    }
    free(txt);
  }
}

static void ny_pkg_search_collect_repo_packages(ny_pkg_search_list_t *out, const char *repo_name,
                                                const char *repo_root) {
  if (!out || !repo_name || !repo_root)
    return;
#ifdef _WIN32
  (void)out;
  (void)repo_name;
  (void)repo_root;
#else
  DIR *dir = opendir(repo_root);
  if (!dir)
    return;
  struct dirent *de;
  while ((de = readdir(dir)) != NULL) {
    const char *raw = de->d_name;
    if (!raw || raw[0] == '.')
      continue;
    char path[4096];
    ny_pkg_join(path, sizeof(path), repo_root, raw);
    if (!ny_pkg_repo_entry_is_package(path))
      continue;
    char name[512];
    snprintf(name, sizeof(name), "%s", raw);
    if (ny_pkg_ends_with_ci(name, ".ny")) {
      size_t n = strlen(name);
      name[n - 3] = '\0';
    }
    char source[1024];
    snprintf(source, sizeof(source), "repo+%s/%s", repo_name, name);
    char manifest_path[4096];
    ny_pkg_repo_manifest_path(path, manifest_path, sizeof(manifest_path));
    char *version = NULL;
    char *description = NULL;
    if (manifest_path[0]) {
      ny_pkg_manifest_t m;
      if (ny_pkg_read_manifest(manifest_path, &m)) {
        version = m.version ? ny_strdup(m.version) : NULL;
        description = m.description ? ny_strdup(m.description) : NULL;
        ny_pkg_manifest_free(&m);
      }
    }
    ny_pkg_search_push(out, name, source, "repo", repo_name, path, version, description);
    free(version);
    free(description);
  }
  closedir(dir);
#endif
}

static void ny_pkg_search_collect_repos(ny_pkg_search_list_t *out, const ny_pkg_opts_t *opts) {
  for (size_t idx = 0; idx < ny_pkg_registry_path_count(); ++idx) {
    char path[4096];
    ny_pkg_registry_path(idx, path, sizeof(path));
    char *txt = path[0] ? ny_read_file(path) : NULL;
    if (!txt)
      continue;
    const char *p = txt;
    while (*p) {
      const char *line = p;
      while (*p && *p != '\n')
        p++;
      size_t n = (size_t)(p - line);
      if (*p == '\n')
        p++;
      char *tmp = ny_pkg_strndup(line, n);
      char *repo_name = NULL;
      char *repo_source = NULL;
      bool ok = tmp && ny_pkg_parse_registry_pair(tmp, "repo", &repo_name, &repo_source);
      if (ok) {
        char *repo_root = ny_pkg_repo_materialize(repo_name, repo_source, opts);
        if (repo_root) {
          ny_pkg_search_push(out, repo_name, repo_source, "repository", repo_name, repo_root,
                             NULL, "package repository");
          ny_pkg_search_collect_repo_packages(out, repo_name, repo_root);
        }
        free(repo_root);
      }
      free(tmp);
      free(repo_name);
      free(repo_source);
    }
    free(txt);
  }
}

static void ny_pkg_search_filter(ny_pkg_search_list_t *list, const char *query) {
  size_t w = 0;
  for (size_t i = 0; i < list->len; ++i) {
    int score = ny_pkg_search_score(&list->items[i], query);
    if (score < 0) {
      ny_pkg_search_entry_free(&list->items[i]);
      continue;
    }
    list->items[i].score = score;
    if (w != i)
      list->items[w] = list->items[i];
    w++;
  }
  list->len = w;
  qsort(list->items, list->len, sizeof(list->items[0]), ny_pkg_search_cmp);
}

static void ny_pkg_search_print(const ny_pkg_search_list_t *list, int limit) {
  size_t shown = 0;
  size_t max = limit > 0 ? (size_t)limit : 60;
  printf("%s%-24s %-11s %-14s %-12s %s%s\n", nyt_clr(NYT_GRAY), "name", "kind", "version",
         "repo", "source", nyt_clr(NYT_RESET));
  for (size_t i = 0; i < list->len && shown < max; ++i, ++shown) {
    const ny_pkg_search_entry_t *e = &list->items[i];
    printf("%s%-24s%s %-11s %-14s %-12s %s\n", nyt_clr(NYT_CYAN), e->name ? e->name : "",
           nyt_clr(NYT_RESET), e->kind ? e->kind : "", e->version ? e->version : "",
           e->repo ? e->repo : "", e->source ? e->source : "");
    if (e->description && *e->description)
      printf("  %s%s%s\n", nyt_clr(NYT_GRAY), e->description, nyt_clr(NYT_RESET));
  }
  if (list->len > shown)
    printf("%s... %zu more%s\n", nyt_clr(NYT_GRAY), list->len - shown, nyt_clr(NYT_RESET));
}

static bool ny_pkg_search_can_interact(void) {
#ifdef _WIN32
  return false;
#else
  return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
#endif
}

#ifndef _WIN32
static bool ny_pkg_read_byte_after(unsigned char *out, long usec) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = usec;
  int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
  return ready > 0 && read(STDIN_FILENO, out, 1) == 1;
}
#endif

static int ny_pkg_search_pick(const ny_pkg_search_list_t *list, const char *initial_query) {
#ifdef _WIN32
  (void)initial_query;
  ny_pkg_search_print(list, 40);
  return 0;
#else
  if (!ny_pkg_search_can_interact()) {
    ny_pkg_search_print(list, 40);
    return 0;
  }
  struct termios oldt;
  if (tcgetattr(STDIN_FILENO, &oldt) != 0) {
    ny_pkg_search_print(list, 40);
    return 0;
  }
  struct termios raw = oldt;
  raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
    ny_pkg_search_print(list, 40);
    return 0;
  }

  char query[256];
  snprintf(query, sizeof(query), "%s", initial_query ? initial_query : "");
  size_t selected = 0;
  int rc = 1;
  for (;;) {
    int visible[16];
    size_t visible_len = 0;
    while (visible_len < sizeof(visible) / sizeof(visible[0])) {
      int best_idx = -1;
      int best_score = 0;
      for (size_t i = 0; i < list->len; ++i) {
        bool used = false;
        for (size_t j = 0; j < visible_len; ++j) {
          if (visible[j] == (int)i) {
            used = true;
            break;
          }
        }
        if (used)
          continue;
        int score = ny_pkg_search_score(&list->items[i], query);
        if (score < 0)
          continue;
        if (best_idx < 0 || score < best_score ||
            (score == best_score &&
             strcmp(list->items[i].name ? list->items[i].name : "",
                    list->items[best_idx].name ? list->items[best_idx].name : "") < 0)) {
          best_idx = (int)i;
          best_score = score;
        }
      }
      if (best_idx < 0)
        break;
      visible[visible_len++] = best_idx;
    }
    if (selected >= visible_len)
      selected = visible_len ? visible_len - 1 : 0;
    printf("\033[2J\033[H%s%sNytrix package search%s  %s%s%s\n",
           nyt_clr(NYT_BOLD), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET),
           nyt_clr(NYT_GRAY), "enter selects, ctrl-c cancels", nyt_clr(NYT_RESET));
    printf("%s/%s %s\n\n", nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET), query);
    for (size_t row = 0; row < visible_len; ++row) {
      const ny_pkg_search_entry_t *e = &list->items[visible[row]];
      bool active = row == selected;
      printf("%s%s %-24s%s %-10s %s%s%s\n",
             active ? nyt_clr(NYT_BOLD) : "", active ? ">" : " ", e->name ? e->name : "",
             nyt_clr(NYT_RESET), e->kind ? e->kind : "", nyt_clr(NYT_GRAY),
             e->source ? e->source : "", nyt_clr(NYT_RESET));
      if (e->description && *e->description)
        printf("  %s%s%s\n", nyt_clr(NYT_GRAY), e->description, nyt_clr(NYT_RESET));
    }
    if (!visible_len)
      printf("%sno matches%s\n", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
    fflush(stdout);

    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1)
      break;
    if (c == 3) {
      rc = 130;
      break;
    }
    if (c == '\r' || c == '\n') {
      printf("\033[2J\033[H");
      if (visible_len) {
        const ny_pkg_search_entry_t *e = &list->items[visible[selected]];
        printf("%s %s\n", e->name ? e->name : "", e->source ? e->source : "");
        rc = 0;
      }
      break;
    }
    if (c == 127 || c == 8) {
      size_t n = strlen(query);
      if (n > 0)
        query[n - 1] = '\0';
      continue;
    }
    if (c == '\033') {
      unsigned char seq[2];
      if (!ny_pkg_read_byte_after(&seq[0], 35000)) {
        rc = 130;
        break;
      }
      if (ny_pkg_read_byte_after(&seq[1], 35000) && seq[0] == '[') {
        if (seq[1] == 'A' && selected > 0)
          selected--;
        if (seq[1] == 'B' && selected + 1 < visible_len)
          selected++;
      }
      continue;
    }
    if (isprint(c)) {
      size_t n = strlen(query);
      if (n + 1 < sizeof(query)) {
        query[n] = (char)c;
        query[n + 1] = '\0';
      }
    }
  }
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
  return rc;
#endif
}

static int ny_pkg_cmd_search(int argc, char **argv, ny_pkg_opts_t *opts) {
  char query[512] = "";
  for (int i = 0; i < argc; ++i) {
    if (i > 0)
      strncat(query, " ", sizeof(query) - strlen(query) - 1);
    strncat(query, argv[i], sizeof(query) - strlen(query) - 1);
  }
  ny_pkg_search_list_t results = {0};
  ny_pkg_search_collect_registry(&results);
  ny_pkg_search_collect_repos(&results, opts);
  bool live_picker = opts->interactive && ny_pkg_search_can_interact();
  ny_pkg_search_filter(&results, live_picker ? "" : query);
  if (results.len == 0) {
    nyt_warn("ny pkg", "no packages found%s%s", query[0] ? " for " : "", query[0] ? query : "");
    ny_pkg_search_list_free(&results);
    return 1;
  }
  int rc = opts->interactive ? ny_pkg_search_pick(&results, query) : 0;
  if (!opts->interactive)
    ny_pkg_search_print(&results, opts->limit);
  ny_pkg_search_list_free(&results);
  return rc;
}

static int ny_pkg_cmd_venv(ny_pkg_opts_t *opts) {
  ny_pkg_manifest_t m;
  bool have = ny_pkg_read_manifest(opts->manifest, &m);
  if (!have) {
    memset(&m, 0, sizeof(m));
    snprintf(m.root, sizeof(m.root), ".");
  }
  opts->venv = true;
  opts->global = false;
  opts->system = false;
  opts->vendor = false;
  char root[4096];
  ny_pkg_install_root(opts, &m, root, sizeof(root));
  ny_ensure_dir_recursive(root);
  printf("%s\n", root);
  ny_pkg_manifest_free(&m);
  return 0;
}

static void ny_pkg_parse_opts(int *argc, char **argv, ny_pkg_opts_t *opts, const char **ref) {
  ny_pkg_manifest_default(opts->manifest, sizeof(opts->manifest));
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)))
    snprintf(opts->root, sizeof(opts->root), "%s", cwd);
  int w = 1;
  for (int r = 1; r < *argc; ++r) {
    const char *a = argv[r];
    int color_mode = -2;
    int color_idx = r;
    char err[256];
    int color_rc = ny_arg_consume_color(&color_idx, *argc, argv, &color_mode, err, sizeof(err));
    if (color_rc < 0) {
      nyt_err("ny pkg", "%s", err);
    } else if (color_rc > 0) {
      ny_arg_apply_color_mode(color_mode);
      r = color_idx;
    } else if (strcmp(a, "--global") == 0 || strcmp(a, "--user") == 0 || strcmp(a, "-g") == 0) {
      opts->global = true;
      opts->system = false;
      opts->venv = false;
      opts->vendor = false;
    } else if (strcmp(a, "--system") == 0 || strcmp(a, "--syswide") == 0) {
      opts->system = true;
      opts->global = false;
      opts->venv = false;
      opts->vendor = false;
    } else if (strcmp(a, "--venv") == 0) {
      opts->venv = true;
      opts->global = false;
      opts->system = false;
      opts->vendor = false;
    } else if (strcmp(a, "--vendor") == 0 || strcmp(a, "--vendored") == 0 ||
               strcmp(a, "--repo-local") == 0) {
      opts->vendor = true;
      opts->venv = false;
      opts->global = false;
      opts->system = false;
    } else if (strcmp(a, "--inplace") == 0 || strcmp(a, "--local") == 0) {
      opts->venv = false;
      opts->global = false;
      opts->system = false;
      opts->vendor = false;
    } else if (strcmp(a, "--force") == 0) {
      opts->force = true;
    } else if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) {
      opts->verbose = true;
    } else if (strcmp(a, "--interactive") == 0 || strcmp(a, "-i") == 0) {
      opts->interactive = true;
    } else if (strcmp(a, "--limit") == 0 && r + 1 < *argc) {
      opts->limit = atoi(argv[++r]);
    } else if (strncmp(a, "--limit=", 8) == 0) {
      opts->limit = atoi(a + 8);
    } else if (strcmp(a, "--manifest") == 0 && r + 1 < *argc) {
      snprintf(opts->manifest, sizeof(opts->manifest), "%s", argv[++r]);
    } else if (strncmp(a, "--manifest=", 11) == 0) {
      snprintf(opts->manifest, sizeof(opts->manifest), "%s", a + 11);
    } else if (strcmp(a, "--root") == 0 && r + 1 < *argc) {
      snprintf(opts->install_root, sizeof(opts->install_root), "%s", argv[++r]);
    } else if (strncmp(a, "--root=", 7) == 0) {
      snprintf(opts->install_root, sizeof(opts->install_root), "%s", a + 7);
    } else if (strcmp(a, "--ref") == 0 && r + 1 < *argc) {
      *ref = argv[++r];
    } else if (strncmp(a, "--ref=", 6) == 0) {
      *ref = a + 6;
    } else if (strcmp(a, "--name") == 0 && r + 1 < *argc) {
      opts->init_name = argv[++r];
    } else if (strncmp(a, "--name=", 7) == 0) {
      opts->init_name = a + 7;
    } else if (strcmp(a, "--version") == 0 && r + 1 < *argc) {
      opts->init_version = argv[++r];
    } else if (strncmp(a, "--version=", 10) == 0) {
      opts->init_version = a + 10;
    } else if (strcmp(a, "--description") == 0 && r + 1 < *argc) {
      opts->init_description = argv[++r];
    } else if (strncmp(a, "--description=", 14) == 0) {
      opts->init_description = a + 14;
    } else if (strcmp(a, "--author") == 0 && r + 1 < *argc) {
      opts->init_author = argv[++r];
    } else if (strncmp(a, "--author=", 9) == 0) {
      opts->init_author = a + 9;
    } else if (strcmp(a, "--license") == 0 && r + 1 < *argc) {
      opts->init_license = argv[++r];
    } else if (strncmp(a, "--license=", 10) == 0) {
      opts->init_license = a + 10;
    } else if ((strcmp(a, "--repository") == 0 || strcmp(a, "--repo") == 0) &&
               r + 1 < *argc) {
      opts->init_repository = argv[++r];
    } else if (strncmp(a, "--repository=", 13) == 0) {
      opts->init_repository = a + 13;
    } else if (strncmp(a, "--repo=", 7) == 0) {
      opts->init_repository = a + 7;
    } else {
      argv[w++] = argv[r];
    }
  }
  *argc = w;
  argv[w] = NULL;
}

static void ny_new_usage(void) {
  static const ny_pkg_usage_entry_t options[] = {
      {"--name NAME        ", "package name when it differs from dir"},
      {"--author NAME      ", "author metadata"},
      {"--version VER      ", "version metadata (default 0.1.0)"},
      {"--license NAME     ", "license metadata"},
      {"--description TEXT ", "description metadata"},
      {"--repository URL   ", "repository metadata"},
      {"--force            ", "overwrite generated files"},
  };
  nyt_heading("Nytrix New");
  printf("%susage:%s %sny new%s %s[dir] [options]%s\n\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET),
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  Creates a small app project with %sny.pkg.json%s, %ssrc/main.ny%s, README,\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  and local package/cache ignores. Dependencies install with %sny pkg add%s.\n\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  ny_pkg_usage_section("options", options, sizeof(options) / sizeof(options[0]), NYT_GREEN);
}

static const char *ny_pkg_basename(const char *path) {
  if (!path || !*path)
    return "app";
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *bslash = strrchr(path, '\\');
  if (bslash && (!slash || bslash > slash))
    slash = bslash;
#endif
  return slash && slash[1] ? slash + 1 : path;
}

static char *ny_pkg_clean_name(const char *raw) {
  raw = raw && *raw ? raw : "app";
  size_t n = strlen(raw);
  char *out = (char *)malloc(n + 1);
  if (!out)
    return NULL;
  size_t w = 0;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)raw[i];
    if (isalnum(c) || c == '_' || c == '-' || c == '.')
      out[w++] = (char)c;
    else if (w > 0 && out[w - 1] != '-')
      out[w++] = '-';
  }
  while (w > 0 && out[w - 1] == '-')
    w--;
  out[w] = '\0';
  if (w == 0) {
    free(out);
    return ny_strdup("app");
  }
  return out;
}

static int ny_pkg_write_text_file(const char *path, const char *text, bool force) {
  if (ny_pkg_path_exists(path) && !force) {
    nyt_err("ny new", "%s already exists (use --force)", path);
    return 1;
  }
  char parent[4096];
  ny_dir_name(parent, sizeof(parent), path);
  ny_ensure_dir_recursive(parent);
  FILE *f = fopen(path, "wb");
  if (!f) {
    nyt_err("ny new", "cannot write %s: %s", path, strerror(errno));
    return 1;
  }
  fputs(text ? text : "", f);
  fclose(f);
  return 0;
}

int ny_new_main(int argc, char **argv) {
  ny_pkg_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  const char *ref = NULL;
  ny_pkg_parse_opts(&argc, argv, &opts, &ref);
  (void)ref;

  const char *arg = argc > 1 ? argv[1] : NULL;
  if (arg && (strcmp(arg, "help") == 0 || strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)) {
    ny_new_usage();
    return 0;
  }

  const char *dir = arg && *arg ? arg : (opts.init_name && *opts.init_name ? opts.init_name : "app");
  char *name = ny_pkg_clean_name(opts.init_name && *opts.init_name ? opts.init_name
                                                                  : ny_pkg_basename(dir));
  if (!name)
    return 1;

  ny_ensure_dir_recursive(dir);
  char src_dir[4096];
  char main_path[4096];
  char readme_path[4096];
  char ignore_path[4096];
  ny_pkg_join(src_dir, sizeof(src_dir), dir, "src");
  ny_pkg_join(main_path, sizeof(main_path), src_dir, "main.ny");
  ny_pkg_join(readme_path, sizeof(readme_path), dir, "README.md");
  ny_pkg_join(ignore_path, sizeof(ignore_path), dir, ".gitignore");
  ny_ensure_dir_recursive(src_dir);

  char *guessed_author = NULL;
  if (!opts.init_author)
    guessed_author = ny_pkg_guess_author();

  ny_pkg_manifest_t m = {0};
  ny_pkg_join(m.path, sizeof(m.path), dir, "ny.pkg.json");
  ny_dir_name(m.root, sizeof(m.root), m.path);
  m.name = ny_strdup(name);
  ny_pkg_manifest_set_meta(&m, opts.init_version ? opts.init_version : "0.1.0",
                           opts.init_description ? opts.init_description : "Nytrix app",
                           opts.init_author ? opts.init_author : guessed_author, opts.init_license,
                           opts.init_repository);

  int rc = 0;
  if (ny_pkg_path_exists(m.path) && !opts.force) {
    nyt_err("ny new", "%s already exists (use --force)", m.path);
    rc = 1;
  } else {
    rc = ny_pkg_write_manifest(&m);
  }

  char main_text[1024];
  snprintf(main_text, sizeof(main_text),
           "use std.core\n\n"
           "fn main() int {\n"
           "   print(\"hello from %s\")\n"
           "   print(\"argc=\" + to_str(argc()))\n"
           "   0\n"
           "}\n",
           name);
  if (rc == 0)
    rc = ny_pkg_write_text_file(main_path, main_text, opts.force);

  char readme_text[4096];
  snprintf(readme_text, sizeof(readme_text),
           "# %s\n\n"
           "Nytrix app created with `ny new`.\n\n"
           "## Quickstart\n\n"
           "```bash\n"
           "ny src/main.ny              # JIT/default run\n"
           "ny -run src/main.ny         # build and run native\n"
           "ny -emit-only -o app src/main.ny\n"
           "./app\n"
           "```\n\n"
           "## Tooling\n\n"
           "```bash\n"
           "ny fmt                  # format source\n"
           "ny fmt --check          # check formatting\n"
           "ny test                 # run tests\n"
           "ny doc search imports   # search docs and stdlib APIs\n"
           "```\n\n"
           "## Dependencies\n\n"
           "```bash\n"
           "ny pkg add foo ../foo\n"
           "ny pkg add bar git+https://example.com/bar.git#main\n"
           "ny pkg add arc ./arcfoo.tgz\n"
           "ny pkg list\n"
           "ny pkg uninstall foo\n"
           "```\n",
           name);
  if (rc == 0)
    rc = ny_pkg_write_text_file(readme_path, readme_text, opts.force);
  if (rc == 0)
    rc = ny_pkg_write_text_file(ignore_path, "ny_modules/\n.nytrix/\n*.out\n", opts.force);

  if (rc == 0) {
    nyt_msg("NEW", NYT_GREEN, "created %s", dir);
    nyt_kv("run", "cd %s && ny src/main.ny", dir);
    nyt_kv("aot", "cd %s && ny -run src/main.ny", dir);
  }
  free(guessed_author);
  ny_pkg_manifest_free(&m);
  free(name);
  return rc;
}

int ny_pkg_main(int argc, char **argv) {
  ny_pkg_opts_t opts;
  memset(&opts, 0, sizeof(opts));
  const char *ref = NULL;
  ny_pkg_parse_opts(&argc, argv, &opts, &ref);
  const char *cmd = argc > 1 ? argv[1] : "help";
  char **args = argc > 2 ? argv + 2 : argv + argc;
  int nargs = argc > 2 ? argc - 2 : 0;
  if (strcmp(cmd, "pkg") == 0 && nargs > 0) {
    cmd = args[0];
    args++;
    nargs--;
  }
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
    ny_pkg_usage();
    return 0;
  }
  if (strcmp(cmd, "init") == 0)
    return ny_pkg_cmd_init(nargs, args, &opts);
  if (strcmp(cmd, "new") == 0)
    return ny_new_main(nargs + 1, argv + 1);
  if (strcmp(cmd, "add") == 0 || strcmp(cmd, "get") == 0)
    return ny_pkg_cmd_add(nargs, args, &opts, ref);
  if (strcmp(cmd, "install") == 0 || strcmp(cmd, "sync") == 0)
    return ny_pkg_cmd_install(nargs, args, &opts, ref);
  if (strcmp(cmd, "uninstall") == 0 || strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0)
    return ny_pkg_cmd_uninstall(nargs, args, &opts);
  if (strcmp(cmd, "update") == 0) {
    opts.force = false;
    if (nargs > 0) {
      ny_pkg_manifest_t m;
      if (!ny_pkg_read_manifest(opts.manifest, &m)) {
        nyt_err("ny pkg", "manifest not found: %s", opts.manifest);
        return 1;
      }
      int rc = ny_pkg_install_manifest(&m, &opts, args[0]);
      ny_pkg_manifest_free(&m);
      return rc;
    }
    return ny_pkg_cmd_install(0, NULL, &opts, ref);
  }
  if (strcmp(cmd, "list") == 0)
    return ny_pkg_cmd_list(&opts);
  if (strcmp(cmd, "info") == 0 || strcmp(cmd, "show") == 0)
    return ny_pkg_cmd_info(nargs, args, &opts);
  if (strcmp(cmd, "path") == 0)
    return ny_pkg_cmd_path(nargs, args, &opts);
  if (strcmp(cmd, "venv") == 0)
    return ny_pkg_cmd_venv(&opts);
  if (strcmp(cmd, "repo") == 0 || strcmp(cmd, "repos") == 0)
    return ny_pkg_cmd_repo(nargs, args, &opts);
  if (strcmp(cmd, "search") == 0 || strcmp(cmd, "find") == 0)
    return ny_pkg_cmd_search(nargs, args, &opts);
  if (strcmp(cmd, "registry") == 0)
    return ny_pkg_cmd_registry(nargs, args, &opts);
  nyt_err("ny pkg", "unknown command '%s'", cmd);
  ny_pkg_usage();
  return 2;
}
