#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "fmt.h"
#include "base/args.h"
#include "base/util.h"
#include "../tools/repo.h"
#include "../tools/tool.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
  int analyze;
  int check;
  int fix;
  int json;
  int tidy;
  int optimize;
  int audit;
  int cloc;
  int cloc_full;
  int cloc_top;
  int dupes;
  int dupes_emit;
  int dupes_min;
  int apply;
  int diff;
  int verbose;
  int conv;
  int types_strict;
  int color;
  int limit;
  const char *audit_mode;
  char audit_mode_buf[256];
  const char *min_sev;
  const char *conv_input;
  const char *conv_name;
  const char *conv_format;
  const char *conv_section;
  const char *conv_output;
  int c2ny;
  const char *c2ny_output;
  int align_macros;
  StrVec paths;
} FmtOpts;

typedef struct {
  char file[PATH_MAX];
  int line;
  int col;
  char code[24];
  char sev[12];
  char msg[512];
  char note[512];
} Issue;

typedef struct {
  Issue *items;
  size_t len;
  size_t cap;
} IssueVec;

typedef struct {
  int files;
  int ny_files;
  int c_files;
  int h_files;
  int py_files;
  int functions;
  int ny_functions;
  int c_functions;
  int public_functions;
  int missing_doc;
  int duplicate_names;
  int long_functions;
  int long_lines;
  int tabs;
  int trailing_ws;
  int todos;
  int unsafe_c;
} AnalyzeStats;

typedef struct {
  int files;
  int loc_total;
  int loc_src;
  int loc_lib;
  int loc_projects;
  int loc_tests;
  int loc_other;
  int ny_files;
  int nyt_files;
  int c_files;
  int h_files;
  int ny_loc;
  int nyt_loc;
  int c_loc;
  int h_loc;
  int diff_files;
  int diff_add;
  int diff_del;
} ClocStats;

typedef struct {
  const char *path;
  int loc;
  int add;
  int del;
} ClocRow;

typedef struct {
  char file[PATH_MAX];
  char name[128];
  int line;
  int end_line;
  int norm_len;
  uint64_t hash;
  char *norm;
} DupFn;

typedef struct {
  DupFn *items;
  size_t len;
  size_t cap;
} DupFnVec;

typedef struct {
  size_t start;
  size_t count;
} DupGroup;

typedef struct {
  DupGroup *items;
  size_t len;
  size_t cap;
} DupGroupVec;

#define VEC_GROW_OR(v, initial_cap, on_fail)                   \
  do {                                                         \
    if ((v)->len == (v)->cap) {                                \
      size_t nc = (v)->cap ? (v)->cap * 2 : (initial_cap);     \
      void *p = realloc((v)->items, nc * sizeof(*(v)->items)); \
      if (!p) {                                                \
        on_fail;                                               \
      }                                                        \
      (v)->items = p;                                          \
      (v)->cap = nc;                                           \
    }                                                          \
  } while (0)

typedef struct {
  char file[PATH_MAX];
  int bytes;
  int lines;
  int nonblank;
  int functions;
  int largest_fn;
  int max_line;
  int dict_gets;
  int dict_sets;
  int appends;
  int env_reads;
  int mallocs;
  int legacy_calls;
  int untyped_params;
  int missing_returns;
  int eager_defaults;
  int eager_ternaries;
  int repeated_lines;
  int repeated_score;
  int score;
} FilePressure;

#define NY_PARAM_BUF_SZ 2048

typedef struct {
  char file[PATH_MAX];
  char name[128];
  int line;
  int end_line;
  int lines;
  int bytes;
  int dict_gets;
  int dict_sets;
  int appends;
  int loops;
  int mallocs;
  int legacy_calls;
  int untyped_params;
  int missing_returns;
  int eager_defaults;
  int eager_ternaries;
  int score;
} FunctionPressure;

typedef struct {
  FilePressure *items;
  size_t len;
  size_t cap;
} FilePressureVec;

typedef struct {
  char seq[192];
  char file[PATH_MAX];
  int line;
  int count;
} CallSeq;

typedef struct {
  CallSeq *items;
  size_t len;
  size_t cap;
} CallSeqVec;

typedef struct {
  FunctionPressure *items;
  size_t len;
  size_t cap;
} FunctionPressureVec;

typedef struct {
  int files;
  int findings;
  int hot_files;
  int hot_functions;
  int repeated_lines;
  int repeated_calls;
  int trim_targets;
  int bug_findings;
  int legacy_calls;
  int method_syntax;
  int receiver_rewrites;
  int untyped_params;
  int missing_returns;
  int type_suggestions;
  int append_builders;
  int literal_tables;
  int repeated_get_shapes;
  int trivial_main_wrappers;
  int accepted_findings;
} AuditStats;

typedef struct {
  int files;
  int functions;
  int normalized;
  int duplicate_groups;
  int duplicate_functions;
  int min_len;
} DupStats;

static int cmp_cstr_ptr(const void *a, const void *b) {
  const char *const *sa = (const char *const *)a;
  const char *const *sb = (const char *const *)b;
  return strcmp(*sa ? *sa : "", *sb ? *sb : "");
}

static void sv_sort(StrVec *v) {
  if (!v || v->len < 2 || !v->items)
    return;
  qsort(v->items, v->len, sizeof(v->items[0]), cmp_cstr_ptr);
}

static void sv_dedup_sorted(StrVec *v) {
  if (!v || v->len < 2 || !v->items)
    return;
  size_t w = 1;
  for (size_t r = 1; r < v->len; r++) {
    if (strcmp(v->items[r], v->items[w - 1]) != 0) {
      v->items[w++] = v->items[r];
    } else {
      free(v->items[r]);
    }
  }
  v->len = w;
}

static void sv_push_n(StrVec *v, const char *s, size_t n) {
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 32;
    char **p = (char **)realloc(v->items, nc * sizeof(char *));
    if (!p)
      return;
    v->items = p;
    v->cap = nc;
  }
  char *copy = (char *)malloc(n + 1);
  if (!copy)
    return;
  memcpy(copy, s ? s : "", n);
  copy[n] = '\0';
  v->items[v->len++] = copy;
}

static void split_lines_keep_empty(const char *txt, StrVec *out) {
  const char *start = txt ? txt : "";
  for (const char *p = start; *p; p++) {
    if (*p == '\n') {
      size_t n = (size_t)(p - start);
      if (n > 0 && start[n - 1] == '\r')
        n--;
      sv_push_n(out, start, n);
      start = p + 1;
    }
  }
  if (*start)
    sv_push(out, start);
}

static int normalized_repo_path(const char *path, char *out, size_t out_sz);

static void issue_push(IssueVec *v, const char *file, int line, int col, const char *code,
                       const char *sev, const char *msg, const char *note) {
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 128;
    Issue *p = (Issue *)realloc(v->items, nc * sizeof(Issue));
    if (!p)
      return;
    v->items = p;
    v->cap = nc;
  }
  Issue *it = &v->items[v->len++];
  memset(it, 0, sizeof(*it));
  char rel[PATH_MAX];
  if (normalized_repo_path(file, rel, sizeof(rel)))
    snprintf(it->file, sizeof(it->file), "%s", rel);
  else
    snprintf(it->file, sizeof(it->file), "%s", file ? file : "");
  it->line = line > 0 ? line : 1;
  it->col = col > 0 ? col : 1;
  snprintf(it->code, sizeof(it->code), "%s", code ? code : "NYFMT0000");
  snprintf(it->sev, sizeof(it->sev), "%s", sev ? sev : "warning");
  snprintf(it->msg, sizeof(it->msg), "%s", msg ? msg : "");
  snprintf(it->note, sizeof(it->note), "%s", note ? note : "");
}

static void audit_push_trim(IssueVec *issues, AuditStats *stats, const char *path,
                            int line, const char *code, const char *sev,
                            const char *msg, const char *note) {
  issue_push(issues, path, line, 1, code, sev, msg, note);
  stats->findings++;
  stats->trim_targets++;
}

static const char *sev_color(const char *sev) {
  if (!sev)
    return NYT_YELLOW;
  if (strcmp(sev, "error") == 0)
    return NYT_RED;
  if (strcmp(sev, "note") == 0)
    return NYT_CYAN;
  return NYT_YELLOW;
}

typedef struct {
  const char *name;
  int rank;
} SeverityRankAlias;

static int severity_rank_lookup(const SeverityRankAlias *aliases, size_t len,
                                const char *sev, int fallback) {
  if (!sev || !*sev)
    return fallback;
  for (size_t i = 0; i < len; i++) {
    if (strcmp(sev, aliases[i].name) == 0)
      return aliases[i].rank;
  }
  return fallback;
}

static const SeverityRankAlias k_min_sev_ranks[] = {
    {"LOW", 3}, {"low", 3}, {"MED", 2},  {"med", 2}, {"medium", 2},
    {"HIGH", 1}, {"high", 1}, {"CRIT", 0}, {"crit", 0}, {"critical", 0},
};

static const SeverityRankAlias k_issue_sev_ranks[] = {
    {"critical", 0}, {"crit", 0}, {"CRIT", 0}, {"error", 1}, {"high", 1},
    {"HIGH", 1},     {"warning", 2}, {"med", 2}, {"MED", 2},
};

static int min_sev_rank(const char *sev) {
  return severity_rank_lookup(k_min_sev_ranks,
                              sizeof(k_min_sev_ranks) / sizeof(k_min_sev_ranks[0]),
                              sev, 3);
}

static int issue_sev_rank(const char *sev) {
  return severity_rank_lookup(k_issue_sev_ranks,
                              sizeof(k_issue_sev_ranks) / sizeof(k_issue_sev_ranks[0]),
                              sev, 2);
}

static int issue_visible_for_min(const Issue *it, const char *min_sev) {
  return issue_sev_rank(it ? it->sev : "warning") <= min_sev_rank(min_sev);
}

static void json_str(const char *s) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    if (*p == '"' || *p == '\\')
      printf("\\%c", *p);
    else if (*p == '\n')
      printf("\\n");
    else if (*p == '\r')
      printf("\\r");
    else if (*p == '\t')
      printf("\\t");
    else if (*p < 32)
      printf("\\u%04x", *p);
    else
      putchar(*p);
  }
  putchar('"');
}

static int is_code_ext(const char *p) {
  return nyt_ends_with(p, ".ny") || nyt_ends_with(p, ".c") || nyt_ends_with(p, ".h") || nyt_ends_with(p, ".py");
}

static int is_fmt_ext(const char *p) {
  return is_code_ext(p) || nyt_ends_with(p, ".md");
}

static int is_ny(const char *p) { return nyt_ends_with(p, ".ny"); }

static int normalized_repo_path(const char *path, char *out, size_t out_sz) {
  if (!path || !*path || !out || out_sz == 0)
    return 0;
  const char *p = path;
  while (p[0] == '.' && (p[1] == '/' || p[1] == '\\'))
    p += 2;
  if ((p[0] == '/' || p[0] == '\\')) {
    char root[PATH_MAX];
    if (getcwd(root, sizeof(root))) {
      size_t rn = strlen(root);
      if (strncmp(p, root, rn) == 0 && (p[rn] == '/' || p[rn] == '\\'))
        p += rn + 1;
    }
  }
  size_t n = 0;
  for (; p[n] && n + 1 < out_sz; n++)
    out[n] = (p[n] == '\\') ? '/' : p[n];
  out[n] = '\0';
  return 1;
}

static int is_expected_error_fixture(const char *path) {
  char rel[PATH_MAX];
  if (!normalized_repo_path(path, rel, sizeof(rel)))
    return 0;
  return strncmp(rel, "etc/tests/fuzz/errors/", 22) == 0;
}

static int path_is_std_lib_source(const char *path) {
  if (!path || !*path)
    return 0;
  while (path[0] == '.' && path[1] == '/')
    path += 2;
  if (strncmp(path, "lib/", 4) == 0)
    return 1;
  if (path[0] == '/') {
    char root[PATH_MAX];
    if (getcwd(root, sizeof(root))) {
      size_t rn = strlen(root);
      if (strncmp(path, root, rn) == 0 && path[rn] == '/') {
        const char *rel = path + rn + 1;
        return strncmp(rel, "lib/", 4) == 0;
      }
    }
  }
  return 0;
}

static int is_cloc_ext(const char *p) {
  return nyt_ends_with(p, ".ny") || nyt_ends_with(p, ".nyt") || nyt_ends_with(p, ".c") || nyt_ends_with(p, ".h");
}

static int is_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return S_ISDIR(st.st_mode);
}

static int is_file(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return S_ISREG(st.st_mode);
}

static int ensure_repo_root(char *out, size_t out_sz) {
  const char *env = getenv("NYTRIX_ROOT");
  if (env && *env) {
    snprintf(out, out_sz, "%s", env);
    return 1;
  }
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd) - 1))
    return 0;
  char cur[PATH_MAX];
  snprintf(cur, sizeof(cur), "%s", cwd);
  for (;;) {
    char probe[PATH_MAX];
    if (strlen(cur) + 16 >= sizeof(probe))
      return 0;
    snprintf(probe, sizeof(probe), "%s/etc", cur);
    if (is_dir(probe)) {
      snprintf(out, out_sz, "%s", cur);
      ny_setenv("NYTRIX_ROOT", cur, 1);
      return 1;
    }
    snprintf(probe, sizeof(probe), "%s/ny.pkg.json", cur);
    if (is_file(probe)) {
      snprintf(out, out_sz, "%s", cur);
      return 1;
    }
    size_t n = strlen(cur);
    while (n > 0 && cur[n - 1] == '/')
      cur[--n] = '\0';
    while (n > 0 && cur[n - 1] != '/')
      cur[--n] = '\0';
    while (n > 0 && cur[n - 1] == '/')
      cur[--n] = '\0';
    if (!cur[0])
      break;
  }
  return 0;
}

static void join_path(char *out, size_t out_sz, const char *a, const char *b) {
  if (!a || !*a) {
    snprintf(out, out_sz, "%s", b ? b : "");
    return;
  }
  if (!b || !*b) {
    snprintf(out, out_sz, "%s", a);
    return;
  }
  if (b[0] == '/')
    snprintf(out, out_sz, "%s", b);
  else
    snprintf(out, out_sz, "%s/%s", a, b);
}

static int is_hidden_name(const char *name) {
  return name && name[0] == '.';
}

static int is_cloc_skip_dir(const char *name) {
  if (!name || !*name)
    return 0;
  return strcmp(name, "build") == 0 || strcmp(name, ".cache") == 0 || strcmp(name, "node_modules") == 0 ||
         strcmp(name, "__pycache__") == 0 || strcmp(name, ".venv") == 0 || strcmp(name, "dist") == 0 ||
         strcmp(name, "out") == 0;
}

typedef int (*PathPred)(const char *path);
typedef int (*SkipDirPred)(const char *name);

static int is_c_or_h(const char *p) {
  return nyt_ends_with(p, ".c") || nyt_ends_with(p, ".h");
}

static int skip_tmp_dir(const char *name) {
  return strcmp(name, "tmp") == 0;
}

static int skip_code_dir(const char *name) {
  return skip_tmp_dir(name) || strcmp(name, "build") == 0 || strcmp(name, "node_modules") == 0;
}

static void collect_matching_rec(const char *path, StrVec *out, PathPred want,
                                 SkipDirPred skip_dir) {
  if (!path || !*path)
    return;
  if (is_file(path)) {
    if (want(path))
      sv_push(out, path);
    return;
  }
  if (!is_dir(path))
    return;

  DIR *d = opendir(path);
  if (!d)
    return;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    const char *name = ent->d_name;
    if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    if (is_hidden_name(name))
      continue;
    if (skip_dir && skip_dir(name))
      continue;
    char child[PATH_MAX];
    join_path(child, sizeof(child), path, name);
    if (is_dir(child))
      collect_matching_rec(child, out, want, skip_dir);
    else if (want(child))
      sv_push(out, child);
  }
  closedir(d);
}

static void collect_files_rec(const char *path, StrVec *out, int want_ny_only) {
  collect_matching_rec(path, out, want_ny_only ? is_ny : is_fmt_ext, skip_tmp_dir);
}

static void collect_code_files_rec(const char *path, StrVec *out) {
  collect_matching_rec(path, out, is_code_ext, skip_code_dir);
}

static void collect_c_files_rec(const char *path, StrVec *out) {
  collect_matching_rec(path, out, is_c_or_h, skip_code_dir);
}

static void collect_cloc_files_rec(const char *path, StrVec *out) {
  collect_matching_rec(path, out, is_cloc_ext, is_cloc_skip_dir);
}

static int write_file(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return 0;
  size_t wr = fwrite(data, 1, len, f);
  fclose(f);
  return wr == len;
}

static void buf_append_char(char **buf, size_t *len, size_t *cap, char c) {
  if (*len + 1 >= *cap) {
    size_t nc = (*cap == 0) ? 1024 : (*cap * 2);
    char *np = (char *)realloc(*buf, nc);
    if (!np)
      return;
    *buf = np;
    *cap = nc;
  }
  (*buf)[(*len)++] = c;
}

static void buf_append_str(char **buf, size_t *len, size_t *cap, const char *s) {
  while (s && *s)
    buf_append_char(buf, len, cap, *s++);
}

static void trim_trailing_ws(char *line) {
  size_t n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
    line[--n] = '\0';
}

static const char *lstrip_ws(const char *line) {
  while (line && (*line == ' ' || *line == '\t'))
    line++;
  return line ? line : "";
}

static void normalize_line_endings(const char *in, char **out) {
  size_t len = 0, cap = 0;
  char *buf = NULL;
  for (size_t i = 0; in[i]; i++) {
    if (in[i] == '\r')
      continue;
    if (in[i] == '\t') {
      buf_append_str(&buf, &len, &cap, "   ");
      continue;
    }
    buf_append_char(&buf, &len, &cap, in[i]);
  }
  buf_append_char(&buf, &len, &cap, '\0');
  *out = buf;
}

static int split_comment_index(const char *line) {
  int quote = 0, esc = 0;
  for (int i = 0; line[i]; i++) {
    char ch = line[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quote = ch;
      continue;
    }
    if (ch == ';')
      return i;
  }
  return -1;
}

static size_t fmt_find_matching_paren(const char *s, size_t open);
static int fmt_keyword_at(const char *s, size_t i, const char *kw);

static void collapse_kw_paren(char *s, const char *kw) {
  size_t k = strlen(kw);
  size_t n = strlen(s);
  for (size_t i = 0; i + k + 2 <= n; i++) {
    if ((i == 0 || !isalnum((unsigned char)s[i - 1])) && memcmp(s + i, kw, k) == 0 &&
        s[i + k] == ' ' && s[i + k + 1] == '(') {
      memmove(s + i + k, s + i + k + 1, n - (i + k));
      n--;
    }
  }
}

static void fmt_set_one_space_after(char *s, size_t cap, size_t pos) {
  size_t n = strlen(s);
  if (pos >= n || cap <= n + 1)
    return;
  size_t p = pos;
  while (s[p] == ' ' || s[p] == '\t')
    p++;
  if (p == pos) {
    memmove(s + pos + 1, s + pos, n - pos + 1);
    s[pos] = ' ';
    return;
  }
  if (p > pos + 1)
    memmove(s + pos + 1, s + p, strlen(s + p) + 1);
  s[pos] = ' ';
}

static void drop_kw_condition_parens(char *s, size_t cap, const char *kw) {
  size_t k = strlen(kw);
  for (size_t i = 0; s && s[i]; i++) {
    if (!fmt_keyword_at(s, i, kw))
      continue;
    size_t open = i + k;
    while (s[open] == ' ' || s[open] == '\t')
      open++;
    if (s[open] != '(')
      continue;
    size_t close = fmt_find_matching_paren(s, open);
    if (close == (size_t)-1)
      continue;
    size_t body = close + 1;
    while (s[body] == ' ' || s[body] == '\t')
      body++;
    if (s[body] != '{') {
      fmt_set_one_space_after(s, cap, i + k);
      i = i + k;
      continue;
    }
    memmove(s + close, s + close + 1, strlen(s + close + 1) + 1);
    memmove(s + open, s + open + 1, strlen(s + open + 1) + 1);
    fmt_set_one_space_after(s, cap, i + k);
    i = i + k;
  }
}

static void drop_control_condition_parens(char *s, size_t cap) {
  drop_kw_condition_parens(s, cap, "if");
  drop_kw_condition_parens(s, cap, "elif");
  drop_kw_condition_parens(s, cap, "while");
  drop_kw_condition_parens(s, cap, "for");
  drop_kw_condition_parens(s, cap, "match");
}

static int starts_with_use_line(const char *s);

static int fmt_word_ending_at_is_control(const char *s, size_t end) {
  if (!s || end == (size_t)-1 || (!isalnum((unsigned char)s[end]) && s[end] != '_'))
    return 0;
  size_t start = end;
  while (start > 0 && (isalnum((unsigned char)s[start - 1]) || s[start - 1] == '_'))
    start--;
  size_t n = end - start + 1;
  if ((n == 2 && strncmp(s + start, "if", 2) == 0) ||
      (n == 4 && strncmp(s + start, "elif", 4) == 0) ||
      (n == 5 && strncmp(s + start, "while", 5) == 0) ||
      (n == 3 && strncmp(s + start, "for", 3) == 0) ||
      (n == 5 && strncmp(s + start, "match", 5) == 0))
    return 1;
  return 0;
}

static void short_forms(char *s) {
  drop_control_condition_parens(s, 8192);
  char *p = strstr(s, "comptime {");
  while (p) {
    memmove(p + 8, p + 9, strlen(p + 9) + 1);
    p = strstr(p + 1, "comptime {");
  }
  if (starts_with_use_line(s))
    return;
  for (size_t i = 0; s[i]; i++) {
    if ((isalnum((unsigned char)s[i]) || s[i] == '_') && s[i + 1] == ' ' && s[i + 2] == '(') {
      if (fmt_word_ending_at_is_control(s, i))
        continue;
      memmove(s + i + 1, s + i + 2, strlen(s + i + 2) + 1);
      i++;
    }
  }
  drop_control_condition_parens(s, 8192);
}

static size_t fmt_find_matching_paren(const char *s, size_t open) {
  int quote = 0, esc = 0, depth = 0;
  for (size_t i = open; s && s[i]; i++) {
    char ch = s[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '(')
      depth++;
    else if (ch == ')' && --depth == 0)
      return i;
  }
  return (size_t)-1;
}

static size_t fmt_skip_angle(const char *s, size_t i) {
  if (!s || s[i] != '<')
    return i;
  int quote = 0, esc = 0, depth = 0;
  for (; s[i]; i++) {
    char ch = s[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '<')
      depth++;
    else if (ch == '>' && --depth == 0)
      return i + 1;
  }
  return i;
}

static size_t fmt_skip_template_hole(const char *s, size_t i) {
  if (!s || s[i] != '$' || s[i + 1] != '{')
    return i;
  int quote = 0, esc = 0, depth = 1;
  i += 2;
  for (; s[i]; i++) {
    char ch = s[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '{')
      depth++;
    else if (ch == '}' && --depth == 0)
      return i + 1;
  }
  return i;
}

static size_t fmt_fn_param_open(const char *s, size_t fn_pos) {
  size_t i = fn_pos + 2;
  while (s[i] == ' ' || s[i] == '\t')
    i++;
  if (s[i] == '(')
    return i;
  int saw_name = 0;
  for (;;) {
    if (isalpha((unsigned char)s[i]) || s[i] == '_') {
      saw_name = 1;
      while (ny_is_ident_char(s[i]))
        i++;
      continue;
    }
    if (s[i] == '$' && s[i + 1] == '{') {
      size_t next = fmt_skip_template_hole(s, i);
      if (next == i)
        return (size_t)-1;
      saw_name = 1;
      i = next;
      continue;
    }
    break;
  }
  if (!saw_name)
    return (size_t)-1;
  while (s[i] == ' ' || s[i] == '\t')
    i++;
  if (s[i] == '<') {
    i = fmt_skip_angle(s, i);
    while (s[i] == ' ' || s[i] == '\t')
      i++;
  }
  return s[i] == '(' ? i : (size_t)-1;
}

static size_t fmt_fn_body_brace(const char *s, size_t close) {
  size_t i = close + 1;
  while (s[i] == ' ' || s[i] == '\t')
    i++;
  if (s[i] == ':') {
    i++;
    while (s[i] == ' ' || s[i] == '\t')
      i++;
  }

  int quote = 0, esc = 0, paren = 0, bracket = 0, angle = 0;
  for (; s[i]; i++) {
    char ch = s[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '$' && s[i + 1] == '{') {
      i = fmt_skip_template_hole(s, i);
      if (s[i] == '\0')
        break;
      i--;
      continue;
    }
    if (ch == '(')
      paren++;
    else if (ch == ')' && paren > 0)
      paren--;
    else if (ch == '[')
      bracket++;
    else if (ch == ']' && bracket > 0)
      bracket--;
    else if (ch == '<')
      angle++;
    else if (ch == '>' && angle > 0)
      angle--;
    else if (ch == '{' && paren == 0 && bracket == 0 && angle == 0)
      return i;
  }
  return (size_t)-1;
}

static int fmt_ensure_one_space_before(char *s, size_t cap, size_t idx) {
  if (!s || idx == 0 || s[idx] == '\0')
    return 0;
  size_t start = idx;
  while (start > 0 && (s[start - 1] == ' ' || s[start - 1] == '\t'))
    start--;
  if (idx - start == 1 && s[start] == ' ')
    return 0;
  size_t len = strlen(s);
  if (start == idx) {
    if (len + 2 > cap)
      return 0;
    memmove(s + idx + 1, s + idx, len - idx + 1);
    s[idx] = ' ';
    return 1;
  }
  s[start] = ' ';
  memmove(s + start + 1, s + idx, len - idx + 1);
  return 1;
}

static void normalize_fn_scope_space(char *s, size_t cap) {
  int quote = 0, esc = 0;
  for (size_t i = 0; s && s[i]; i++) {
    char ch = s[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch != 'f' || s[i + 1] != 'n')
      continue;
    char before = i == 0 ? '\0' : s[i - 1];
    char after = s[i + 2];
    if (ny_is_ident_char(before) || !(after == '(' || after == ' ' || after == '\t'))
      continue;
    size_t open = fmt_fn_param_open(s, i);
    if (open == (size_t)-1)
      continue;
    size_t close = fmt_find_matching_paren(s, open);
    if (close == (size_t)-1)
      continue;
    size_t brace = fmt_fn_body_brace(s, close);
    if (brace == (size_t)-1)
      continue;
    if (fmt_ensure_one_space_before(s, cap, brace))
      brace++;
    i = brace;
  }
}

static int fmt_keyword_at(const char *s, size_t i, const char *kw) {
  size_t n = strlen(kw);
  if (strncmp(s + i, kw, n) != 0)
    return 0;
  char before = i == 0 ? '\0' : s[i - 1];
  char after = s[i + n];
  return !ny_is_ident_char(before) && !ny_is_ident_char(after);
}

static size_t fmt_control_body_brace(const char *s, size_t start) {
  int quote = 0, esc = 0, paren = 0, bracket = 0;
  for (size_t i = start; s && s[i]; i++) {
    char ch = s[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '$' && s[i + 1] == '{') {
      size_t next = fmt_skip_template_hole(s, i);
      if (next == i || s[next] == '\0')
        break;
      i = next - 1;
      continue;
    }
    if (ch == '(')
      paren++;
    else if (ch == ')' && paren > 0)
      paren--;
    else if (ch == '[')
      bracket++;
    else if (ch == ']' && bracket > 0)
      bracket--;
    else if (ch == '{' && paren == 0 && bracket == 0)
      return i;
  }
  return (size_t)-1;
}

static void normalize_control_scope_space(char *s, size_t cap) {
  static const char *keys[] = {"if", "elif", "while", "for", "match", "else"};
  int quote = 0, esc = 0;
  for (size_t i = 0; s && s[i]; i++) {
    char ch = s[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '$' && s[i + 1] == '{') {
      size_t next = fmt_skip_template_hole(s, i);
      if (next == i || s[next] == '\0')
        break;
      i = next - 1;
      continue;
    }
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
      const char *kw = keys[k];
      size_t n = strlen(kw);
      if (!fmt_keyword_at(s, i, kw))
        continue;
      size_t brace = fmt_control_body_brace(s, i + n);
      if (brace == (size_t)-1)
        continue;
      if (fmt_ensure_one_space_before(s, cap, brace))
        brace++;
      i = brace;
      break;
    }
  }
}

static void normalize_use_import_star(char *s) {
  char *p = (char *)lstrip_ws(s);
  if (strncmp(p, "use", 3) != 0 || (isalnum((unsigned char)p[3]) || p[3] == '_'))
    return;
  p += 3;
  if (!isspace((unsigned char)*p))
    return;
  if (strchr(p, '(') || strstr(p, " as "))
    return;

  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1]))
    end--;
  if (end > s && end[-1] == ';') {
    end--;
    while (end > s && isspace((unsigned char)end[-1]))
      end--;
  }
  if (end <= s || end[-1] != '*')
    return;

  char *star = end - 1;
  char *cut = star;
  while (cut > s && isspace((unsigned char)cut[-1]))
    cut--;
  if (cut == star)
    return;
  *cut = '\0';
  trim_trailing_ws(s);
}

static void normalize_use_import_list_space(char *s, size_t cap) {
  if (!starts_with_use_line(s) || !s || cap == 0)
    return;
  char *open = strchr(s, '(');
  if (!open || open == s || isspace((unsigned char)open[-1]))
    return;
  size_t len = strlen(s);
  if (len + 2 > cap)
    return;
  memmove(open + 1, open, strlen(open) + 1);
  *open = ' ';
}

static int split_attr_fn_line(const char *line, StrVec *out) {
  if (!line || !out)
    return 0;
  const char *p = lstrip_ws(line);
  if (*p != '@')
    return 0;

  const char *attr_starts[16];
  size_t attr_lens[16];
  size_t attr_count = 0;
  const char *cur = p;
  while (*cur == '@' && attr_count < (sizeof(attr_starts) / sizeof(attr_starts[0]))) {
    const char *start = cur;
    cur++;
    int depth = 0;
    int quote = 0;
    int esc = 0;
    while (*cur) {
      char ch = *cur;
      if (quote) {
        if (esc)
          esc = 0;
        else if (ch == '\\')
          esc = 1;
        else if (ch == quote)
          quote = 0;
        cur++;
        continue;
      }
      if (ch == '"' || ch == '\'') {
        quote = ch;
        cur++;
        continue;
      }
      if (ch == '(' || ch == '[' || ch == '{') {
        depth++;
        cur++;
        continue;
      }
      if ((ch == ')' || ch == ']' || ch == '}') && depth > 0) {
        depth--;
        cur++;
        continue;
      }
      if (depth == 0 && (isspace((unsigned char)ch) || ch == ';'))
        break;
      cur++;
    }
    if (cur == start + 1)
      return 0;
    attr_starts[attr_count] = start;
    attr_lens[attr_count] = (size_t)(cur - start);
    attr_count++;
    while (*cur == ' ' || *cur == '\t')
      cur++;
  }

  if (attr_count == 0 || *cur == ';')
    return 0;

  if (*cur == '\0' && attr_count < 2)
    return 0;

  for (size_t i = 0; i < attr_count; i++)
    sv_push_n(out, attr_starts[i], attr_lens[i]);

  if (*cur)
    sv_push(out, cur);
  return 1;
}

static int is_open_delim(char ch) { return ch == '{' || ch == '(' || ch == '['; }

static int is_close_delim(char ch) { return ch == '}' || ch == ')' || ch == ']'; }

static int count_brace_delta(const char *line, int *starts_with_close) {
  int delta = 0, quote = 0, esc = 0;
  const char *p = line;
  while (*p == ' ' || *p == '\t')
    p++;
  *starts_with_close = is_close_delim(*p);
  for (int i = 0; line[i]; i++) {
    char ch = line[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == ';')
      break;
    if (is_open_delim(ch))
      delta++;
    else if (is_close_delim(ch))
      delta--;
  }
  return delta;
}

static int count_brace_delta_state(const char *line, int *starts_with_close, int *quote_state, int *esc_state) {
  int delta = 0;
  int quote = quote_state ? *quote_state : 0;
  int esc = esc_state ? *esc_state : 0;
  const char *p = line;
  while (*p == ' ' || *p == '\t')
    p++;
  *starts_with_close = quote ? 0 : is_close_delim(*p);
  for (int i = 0; line[i]; i++) {
    char ch = line[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == ';')
      break;
    if (is_open_delim(ch))
      delta++;
    else if (is_close_delim(ch))
      delta--;
  }
  if (quote_state)
    *quote_state = quote;
  if (esc_state)
    *esc_state = esc;
  return delta;
}

enum NyTopKind {
  NY_TOP_NONE = 0,
  NY_TOP_COMMENT,
  NY_TOP_MODULE,
  NY_TOP_USE,
  NY_TOP_DECORATOR,
  NY_TOP_FN,
  NY_TOP_DEF,
  NY_TOP_DECL,
  NY_TOP_MAIN,
  NY_TOP_CLOSE,
  NY_TOP_STMT,
  NY_TOP_OTHER,
};

static int starts_with_word(const char *s, const char *word) {
  size_t n = strlen(word);
  if (strncmp(s, word, n) != 0)
    return 0;
  unsigned char ch = (unsigned char)s[n];
  return ch == '\0' || isspace(ch) || ch == '(' || ch == '{';
}

static const char *find_outside_string(const char *line, const char *needle);

static void compact_main_guard_prefix(const char *s, char *out, size_t out_sz) {
  size_t n = 0;
  int quote = 0, esc = 0;
  if (!out || out_sz == 0)
    return;
  out[0] = '\0';
  for (size_t i = 0; s && s[i] && n + 1 < out_sz; i++) {
    char ch = s[i];
    if (!quote && ch == ';')
      break;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      out[n++] = ch;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      out[n++] = ch;
      continue;
    }
    if (isspace((unsigned char)ch))
      continue;
    out[n++] = ch;
  }
  out[n] = '\0';
}

static int starts_with_hash_main_guard(const char *s) {
  if (!s)
    return 0;
  s = lstrip_ws(s);
  return strncmp(s, "#main", 5) == 0 &&
         (s[5] == '\0' || isspace((unsigned char)s[5]) || s[5] == '{');
}

static int starts_with_verbose_main_guard(const char *s) {
  char compact[160];
  compact_main_guard_prefix(lstrip_ws(s), compact, sizeof(compact));
  if (strncmp(compact, "if(", 3) != 0)
    return 0;
  const char *cond = compact + 3;
  if (strncmp(cond, "__main", 6) == 0 &&
      strncmp(cond + 6, "())", 3) == 0)
    return 1;
  if (strncmp(cond, "comptime{", 9) != 0)
    return 0;
  cond += 9;
  if (strncmp(cond, "return", 6) == 0)
    cond += 6;
  return strncmp(cond, "__main", 6) == 0 &&
         strncmp(cond + 6, "()})", 4) == 0;
}

static int starts_with_main_guard(const char *s) {
  return starts_with_hash_main_guard(s) || starts_with_verbose_main_guard(s);
}

static int starts_with_use_line(const char *s) {
  s = lstrip_ws(s);
  return starts_with_word(s, "use");
}

static int legacy_use_import_star_col(const char *s) {
  char buf[4096];
  if (!s)
    return -1;
  snprintf(buf, sizeof(buf), "%s", s);
  int comment = split_comment_index(buf);
  if (comment >= 0)
    buf[comment] = '\0';
  trim_trailing_ws(buf);
  const char *p = lstrip_ws(buf);
  if (strncmp(p, "use", 3) != 0 || isalnum((unsigned char)p[3]) || p[3] == '_')
    return -1;
  p += 3;
  if (!isspace((unsigned char)*p))
    return -1;
  if (strchr(p, '(') || strstr(p, " as "))
    return -1;
  const char *end = buf + strlen(buf);
  if (end <= buf || end[-1] != '*')
    return -1;
  const char *star = end - 1;
  const char *cut = star;
  while (cut > buf && isspace((unsigned char)cut[-1]))
    cut--;
  if (cut == star)
    return -1;
  return (int)(star - buf);
}

static int compact_use_import_list_col(const char *s) {
  char buf[4096];
  if (!s)
    return -1;
  snprintf(buf, sizeof(buf), "%s", s);
  int comment = split_comment_index(buf);
  if (comment >= 0)
    buf[comment] = '\0';
  trim_trailing_ws(buf);
  if (!starts_with_use_line(buf))
    return -1;
  const char *open = strchr(buf, '(');
  if (!open || open == buf || isspace((unsigned char)open[-1]))
    return -1;
  return (int)(open - buf);
}

static int repeated_mut_decl_col(const char *s) {
  char buf[4096];
  if (!s)
    return -1;
  snprintf(buf, sizeof(buf), "%s", s);
  int comment = split_comment_index(buf);
  if (comment >= 0)
    buf[comment] = '\0';
  trim_trailing_ws(buf);
  const char *p = lstrip_ws(buf);
  if (!starts_with_word(p, "mut"))
    return -1;
  if (find_outside_string(p, " while(") || find_outside_string(p, " if(") ||
      find_outside_string(p, " for(") || find_outside_string(p, "{"))
    return -1;
  int quote = 0, esc = 0, hits = 0;
  for (const char *q = p; *q; q++) {
    if (!quote && q[0] == '/' && q[1] == '/')
      break;
    if (quote) {
      if (esc)
        esc = 0;
      else if (*q == '\\')
        esc = 1;
      else if (*q == quote)
        quote = 0;
      continue;
    }
    if (*q == '"' || *q == '\'') {
      quote = *q;
      continue;
    }
    if (strncmp(q, "mut", 3) == 0 &&
        (q == p || isspace((unsigned char)q[-1])) &&
        (q[3] == '\0' || isspace((unsigned char)q[3]))) {
      hits++;
      if (hits >= 2)
        return (int)(q - buf) + 1;
      q += 2;
    }
  }
  return -1;
}

static int reversed_layout_field_col(const char *s) {
  char buf[4096];
  if (!s)
    return -1;
  snprintf(buf, sizeof(buf), "%s", s);
  int comment = split_comment_index(buf);
  if (comment >= 0)
    buf[comment] = '\0';
  trim_trailing_ws(buf);
  const char *p = lstrip_ws(buf);
  if (!*p || *p == '}' || starts_with_word(p, "layout") || starts_with_word(p, "derive") ||
      starts_with_word(p, "pack") || starts_with_word(p, "align"))
    return -1;
  if (!isalpha((unsigned char)*p) && *p != '_')
    return -1;
  const char *name = p;
  while (isalnum((unsigned char)*p) || *p == '_')
    p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != ':')
    return -1;
  p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (!isalpha((unsigned char)*p) && *p != '_')
    return -1;
  return (int)(name - buf) + 1;
}

typedef struct {
  char keyword[8];
  char name[96];
  char rhs[256];
  int indent;
} NySimpleDecl;

static int parse_ny_simple_decl_line(const char *line, NySimpleDecl *out) {
  char buf[4096];
  if (!line || !out)
    return 0;
  snprintf(buf, sizeof(buf), "%s", line);
  if (split_comment_index(buf) >= 0)
    return 0;
  trim_trailing_ws(buf);
  const char *p = buf;
  int indent = 0;
  while (*p == ' ') {
    indent++;
    p++;
  }
  const char *keyword = NULL;
  if (starts_with_word(p, "def"))
    keyword = "def";
  else if (starts_with_word(p, "mut"))
    keyword = "mut";
  else
    return 0;
  p += 3;
  if (!isspace((unsigned char)*p))
    return 0;
  while (isspace((unsigned char)*p))
    p++;
  if (!isalpha((unsigned char)*p) && *p != '_')
    return 0;
  const char *name_start = p;
  p++;
  while (isalnum((unsigned char)*p) || *p == '_')
    p++;
  size_t name_len = (size_t)(p - name_start);
  while (isspace((unsigned char)*p))
    p++;
  if (*p != '=')
    return 0;
  p++;
  while (isspace((unsigned char)*p))
    p++;
  if (!*p)
    return 0;
  if (find_outside_string(p, " def ") || find_outside_string(p, " mut ") ||
      find_outside_string(p, " = ") || find_outside_string(p, "] =") ||
      find_outside_string(p, " while(") || find_outside_string(p, " if(") ||
      find_outside_string(p, " for(") ||
      find_outside_string(p, " += ") || find_outside_string(p, " -= ") ||
      find_outside_string(p, " *= ") || find_outside_string(p, " /= ") ||
      find_outside_string(p, " return "))
    return 0;
  snprintf(out->keyword, sizeof(out->keyword), "%s", keyword);
  snprintf(out->name, sizeof(out->name), "%.*s", (int)name_len, name_start);
  snprintf(out->rhs, sizeof(out->rhs), "%s", p);
  trim_trailing_ws(out->rhs);
  out->indent = indent;
  return out->rhs[0] != '\0';
}

static int ny_names_form_pair(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  size_t na = strlen(a), nb = strlen(b);
  if (na == 0 || na != nb)
    return 0;
  int diffs = 0;
  for (size_t i = 0; i < na; i++)
    diffs += a[i] != b[i];
  return diffs == 1;
}

static int adjacent_simple_decl_group_col(const StrVec *lines, size_t idx, char *note,
                                          size_t note_cap) {
  if (!lines || idx + 1 >= lines->len || !note || note_cap < 512)
    return -1;
  NySimpleDecl cur, next, prev;
  if (!parse_ny_simple_decl_line(lines->items[idx], &cur) ||
      !parse_ny_simple_decl_line(lines->items[idx + 1], &next))
    return -1;
  if (idx > 0 && parse_ny_simple_decl_line(lines->items[idx - 1], &prev) &&
      prev.indent == cur.indent && strcmp(prev.keyword, cur.keyword) == 0)
    return -1;
  if (cur.indent != next.indent || strcmp(cur.keyword, next.keyword) != 0)
    return -1;
  if (cur.indent == 0 || !ny_names_form_pair(cur.name, next.name))
    return -1;
  int combined_width = cur.indent + (int)strlen(cur.keyword) + 1 +
                       (int)strlen(cur.name) + 2 + (int)strlen(next.name) + 3 +
                       (int)strlen(cur.rhs) + 2 + (int)strlen(next.rhs);
  if (combined_width > 120)
    return -1;
  snprintf(note, 512, "prefer '%.3s %.80s, %.80s = %.120s, %.120s'",
           cur.keyword, cur.name, next.name, cur.rhs, next.rhs);
  return cur.indent + 1;
}

typedef struct {
  char name[96];
  char rhs[256];
  int indent;
} NySimpleAssign;

static void copy_trim_span(char *dst, size_t cap, const char *start, const char *end) {
  if (!dst || cap == 0)
    return;
  dst[0] = '\0';
  if (!start)
    return;
  if (!end)
    end = start + strlen(start);
  while (start < end && isspace((unsigned char)*start))
    start++;
  while (end > start && isspace((unsigned char)end[-1]))
    end--;
  size_t n = (size_t)(end - start);
  if (n >= cap)
    n = cap - 1;
  memcpy(dst, start, n);
  dst[n] = '\0';
}

static const char *ny_next_assign_boundary(const char *rhs) {
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = rhs; p && *p; p++) {
    if (quote) {
      if (esc)
        esc = 0;
      else if (*p == '\\')
        esc = 1;
      else if (*p == quote)
        quote = 0;
      continue;
    }
    if (*p == '"' || *p == '\'') {
      quote = *p;
      continue;
    }
    if (is_open_delim(*p)) {
      depth++;
      continue;
    }
    if (is_close_delim(*p)) {
      if (depth > 0)
        depth--;
      continue;
    }
    if (depth != 0 || !isspace((unsigned char)*p))
      continue;
    const char *prev = p;
    while (prev > rhs && isspace((unsigned char)prev[-1]))
      prev--;
    if (prev > rhs && prev[-1] == ',')
      continue;
    const char *q = p;
    while (isspace((unsigned char)*q))
      q++;
    if (!isalpha((unsigned char)*q) && *q != '_')
      continue;
    const char *name = q;
    q++;
    while (isalnum((unsigned char)*q) || *q == '_')
      q++;
    while (isspace((unsigned char)*q))
      q++;
    if (*q == '=' && q[1] != '=' && q > name)
      return name;
  }
  return NULL;
}

static int parse_ny_simple_assign_at(const char *line_start, const char *p, NySimpleAssign *out,
                                     const char **next_out) {
  if (next_out)
    *next_out = NULL;
  if (!line_start || !p || !out)
    return 0;
  while (isspace((unsigned char)*p))
    p++;
  if (starts_with_word(p, "def") || starts_with_word(p, "mut") ||
      starts_with_word(p, "if") || starts_with_word(p, "elif") ||
      starts_with_word(p, "while") || starts_with_word(p, "for") ||
      starts_with_word(p, "return"))
    return 0;
  int indent = (int)(p - line_start);
  if (!isalpha((unsigned char)*p) && *p != '_')
    return 0;
  const char *name_start = p;
  p++;
  while (isalnum((unsigned char)*p) || *p == '_')
    p++;
  const char *name_end = p;
  while (isspace((unsigned char)*p))
    p++;
  if (*p != '=' || p[1] == '=' || (p > line_start && strchr("+-*/%!<>|&", p[-1])))
    return 0;
  p++;
  while (isspace((unsigned char)*p))
    p++;
  const char *next = ny_next_assign_boundary(p);
  const char *rhs_end = next ? next : p + strlen(p);
  copy_trim_span(out->name, sizeof(out->name), name_start, name_end);
  copy_trim_span(out->rhs, sizeof(out->rhs), p, rhs_end);
  out->indent = indent;
  if (!out->name[0] || !out->rhs[0])
    return 0;
  if (find_outside_string(out->rhs, " return ") || find_outside_string(out->rhs, " def ") ||
      find_outside_string(out->rhs, " mut "))
    return 0;
  if (next_out)
    *next_out = next;
  return 1;
}

static bool ny_assign_names_distinct(const NySimpleAssign *items, int n) {
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (strcmp(items[i].name, items[j].name) == 0)
        return false;
  return true;
}

static bool ny_rhs_get_base(const char *rhs, char *base, size_t cap) {
  const char *p = rhs ? strstr(rhs, ".get(") : NULL;
  if (!p || p == rhs || !base || cap == 0)
    return false;
  const char *start = rhs;
  while (isspace((unsigned char)*start))
    start++;
  for (const char *q = start; q < p; q++) {
    if (!isalnum((unsigned char)*q) && *q != '_' && *q != '.')
      return false;
  }
  copy_trim_span(base, cap, start, p);
  return base[0] != '\0';
}

static bool ny_assigns_share_get_base(const NySimpleAssign *items, int n) {
  if (!items || n < 2)
    return false;
  char first[128], cur[128];
  if (!ny_rhs_get_base(items[0].rhs, first, sizeof(first)))
    return false;
  for (int i = 1; i < n; i++) {
    if (!ny_rhs_get_base(items[i].rhs, cur, sizeof(cur)) || strcmp(first, cur) != 0)
      return false;
  }
  return true;
}

static void build_assignment_group_hint(const NySimpleAssign *items, int n, char *note,
                                        size_t note_cap) {
  char names[256] = {0};
  char rhs[512] = {0};
  size_t nl = 0, rl = 0;
  for (int i = 0; i < n; i++) {
    int nw = snprintf(names + nl, sizeof(names) - nl, "%s%.80s", i ? ", " : "", items[i].name);
    if (nw > 0)
      nl += (size_t)nw < sizeof(names) - nl ? (size_t)nw : sizeof(names) - nl - 1;
    int rw = snprintf(rhs + rl, sizeof(rhs) - rl, "%s%.140s", i ? ", " : "", items[i].rhs);
    if (rw > 0)
      rl += (size_t)rw < sizeof(rhs) - rl ? (size_t)rw : sizeof(rhs) - rl - 1;
  }
  snprintf(note, note_cap, "prefer '%.240s = %.240s'", names, rhs);
}

static int same_line_assignment_group_col(const char *line, char *note, size_t note_cap) {
  char buf[4096];
  if (!line || !note || note_cap < 512)
    return -1;
  snprintf(buf, sizeof(buf), "%s", line);
  int comment = split_comment_index(buf);
  if (comment >= 0)
    buf[comment] = '\0';
  trim_trailing_ws(buf);
  const char *p = lstrip_ws(buf);
  NySimpleAssign items[8];
  int count = 0;
  const char *next = NULL;
  while (count < 8 && parse_ny_simple_assign_at(buf, p, &items[count], &next)) {
    count++;
    if (!next)
      break;
    p = next;
  }
  if (count < 2 || !ny_assign_names_distinct(items, count))
    return -1;
  build_assignment_group_hint(items, count, note, note_cap);
  return items[0].indent + 1;
}

static int adjacent_simple_assign_group_col(const StrVec *lines, size_t idx, char *note,
                                            size_t note_cap) {
  if (!lines || idx + 1 >= lines->len || !note || note_cap < 512)
    return -1;
  char cur_buf[4096], next_buf[4096], prev_buf[4096];
  snprintf(cur_buf, sizeof(cur_buf), "%s", lines->items[idx]);
  snprintf(next_buf, sizeof(next_buf), "%s", lines->items[idx + 1]);
  int cmt = split_comment_index(cur_buf);
  if (cmt >= 0)
    cur_buf[cmt] = '\0';
  cmt = split_comment_index(next_buf);
  if (cmt >= 0)
    next_buf[cmt] = '\0';
  trim_trailing_ws(cur_buf);
  trim_trailing_ws(next_buf);
  NySimpleAssign items[2], prev;
  const char *cur_next = NULL, *next_next = NULL;
  if (!parse_ny_simple_assign_at(cur_buf, cur_buf, &items[0], &cur_next) || cur_next ||
      !parse_ny_simple_assign_at(next_buf, next_buf, &items[1], &next_next) || next_next)
    return -1;
  if (idx > 0) {
    snprintf(prev_buf, sizeof(prev_buf), "%s", lines->items[idx - 1]);
    cmt = split_comment_index(prev_buf);
    if (cmt >= 0)
      prev_buf[cmt] = '\0';
    trim_trailing_ws(prev_buf);
    const char *prev_next = NULL;
    if (parse_ny_simple_assign_at(prev_buf, prev_buf, &prev, &prev_next) && !prev_next &&
        prev.indent == items[0].indent)
      return -1;
  }
  if (items[0].indent != items[1].indent || !ny_assign_names_distinct(items, 2))
    return -1;
  if (!ny_names_form_pair(items[0].name, items[1].name) && !ny_assigns_share_get_base(items, 2))
    return -1;
  int width = items[0].indent + (int)strlen(items[0].name) + 2 +
              (int)strlen(items[1].name) + 3 + (int)strlen(items[0].rhs) + 2 +
              (int)strlen(items[1].rhs);
  if (width > 140)
    return -1;
  build_assignment_group_hint(items, 2, note, note_cap);
  return items[0].indent + 1;
}

static int ny_long_line_is_compact_ok(const char *line, int width) {
  if (!line || width <= 120)
    return 0;
  const char *s = lstrip_ws(line);
  if (*s == '"') {
    int esc = 0;
    for (const char *p = s + 1; *p; ++p) {
      if (esc) {
        esc = 0;
        continue;
      }
      if (*p == '\\') {
        esc = 1;
        continue;
      }
      if (*p == '"') {
        ++p;
        while (*p && isspace((unsigned char)*p))
          ++p;
        if (*p == ',' || *p == ']' || *p == '\0')
          return 1;
        break;
      }
    }
  }
  if ((starts_with_word(s, "def") || starts_with_word(s, "mut")) &&
      find_outside_string(s, " = \""))
    return 1;
  if (width > 240)
    return 0;
  if (starts_with_word(s, "def") && find_outside_string(s, " = ["))
    return 1;
  if (starts_with_word(s, "fn") && strchr(s, '(') && width <= 180)
    return 1;
  if (starts_with_word(s, "fn") && find_outside_string(s, "{") &&
      find_outside_string(s, "}"))
    return 1;
  if ((starts_with_word(s, "if") || starts_with_word(s, "elif")) &&
      find_outside_string(s, "{") && find_outside_string(s, "}"))
    return 1;
  if ((starts_with_word(s, "def") || starts_with_word(s, "mut")) &&
      (find_outside_string(s, " def ") || find_outside_string(s, " mut ")))
    return 1;
  if (strchr(s, '(') && strchr(s, ')') && !strchr(s, '{') && width <= 180)
    return 1;
  return 0;
}

static int starts_with_ny_template_line(const char *s) {
  s = lstrip_ws(s);
  return strncmp(s, "comptime template", 17) == 0 &&
         (s[17] == '\0' || isspace((unsigned char)s[17]));
}

static enum NyTopKind ny_top_kind(const char *line, int eff_indent) {
  if (eff_indent != 0)
    return NY_TOP_OTHER;
  if (!line || !line[0])
    return NY_TOP_NONE;
  if (line[0] == ';')
    return NY_TOP_COMMENT;
  if (line[0] == '@')
    return NY_TOP_DECORATOR;
  if (strcmp(line, "}") == 0 || strcmp(line, ")") == 0 || strcmp(line, "]") == 0)
    return NY_TOP_CLOSE;
  if (starts_with_word(line, "module"))
    return NY_TOP_MODULE;
  if (starts_with_word(line, "use"))
    return NY_TOP_USE;
  if (starts_with_word(line, "layout") || starts_with_word(line, "struct") ||
      starts_with_word(line, "enum") || starts_with_word(line, "impl") ||
      starts_with_word(line, "operator"))
    return NY_TOP_DECL;
  if (starts_with_word(line, "comptime") &&
      (strstr(line, "diagnostic") || strstr(line, "table") || strstr(line, "template") ||
       strstr(line, "fields") || strstr(line, "exports")))
    return NY_TOP_DECL;
  if (starts_with_word(line, "fn"))
    return NY_TOP_FN;
  if (starts_with_word(line, "def") || starts_with_word(line, "mut"))
    return NY_TOP_DEF;
  if (starts_with_word(line, "if"))
    return NY_TOP_MAIN;
  if (starts_with_main_guard(line))
    return NY_TOP_MAIN;
  return NY_TOP_STMT;
}

static int ny_wants_blank_before(enum NyTopKind prev, enum NyTopKind cur) {
  if (prev == NY_TOP_NONE || cur == NY_TOP_NONE || cur == NY_TOP_OTHER || cur == NY_TOP_CLOSE)
    return 0;
  if (cur == NY_TOP_COMMENT)
    return prev != NY_TOP_COMMENT;
  if (cur == NY_TOP_USE)
    return prev == NY_TOP_CLOSE || prev == NY_TOP_STMT;
  if (cur == NY_TOP_MODULE)
    return prev == NY_TOP_CLOSE || prev == NY_TOP_STMT;
  if (cur == NY_TOP_DECORATOR || cur == NY_TOP_FN)
    return prev == NY_TOP_USE || prev == NY_TOP_CLOSE || prev == NY_TOP_DEF ||
           prev == NY_TOP_FN || prev == NY_TOP_STMT;
  if (cur == NY_TOP_DECL)
    return prev == NY_TOP_USE || prev == NY_TOP_CLOSE || prev == NY_TOP_FN ||
           prev == NY_TOP_DEF || prev == NY_TOP_DECL || prev == NY_TOP_STMT;
  if (cur == NY_TOP_DEF)
    return prev == NY_TOP_USE || prev == NY_TOP_CLOSE || prev == NY_TOP_DECL;
  if (cur == NY_TOP_MAIN)
    return prev == NY_TOP_USE || prev == NY_TOP_CLOSE || prev == NY_TOP_FN ||
           prev == NY_TOP_DEF || prev == NY_TOP_DECL || prev == NY_TOP_STMT;
  if (cur == NY_TOP_STMT)
    return prev == NY_TOP_USE || prev == NY_TOP_CLOSE || prev == NY_TOP_DECL;
  return 0;
}

static char *format_ny_text(const char *in) {
  char *norm = NULL;
  normalize_line_endings(in, &norm);
  if (!norm)
    return NULL;
  char *work = strdup(norm);
  free(norm);
  if (!work)
    return NULL;

  size_t out_len = 0, out_cap = 0;
  char *out = NULL;
  int indent = 0;
  int pending_blank = 0;
  int wrote_any = 0;
  int quote_state = 0;
  int esc_state = 0;
  enum NyTopKind prev_top = NY_TOP_NONE;

  char *save = NULL;
  for (char *ln = strtok_r(work, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
    int cidx = split_comment_index(ln);
    char code[8192];
    char comment[8192];
    code[0] = '\0';
    comment[0] = '\0';
    if (cidx >= 0) {
      snprintf(code, sizeof(code), "%.*s", cidx, ln);
      snprintf(comment, sizeof(comment), "%s", ln + cidx);
    } else {
      snprintf(code, sizeof(code), "%s", ln);
    }
    trim_trailing_ws(code);
    char code_stripped[8192];
    snprintf(code_stripped, sizeof(code_stripped), "%s", lstrip_ws(code));
    short_forms(code_stripped);
    normalize_fn_scope_space(code_stripped, sizeof(code_stripped));
    normalize_control_scope_space(code_stripped, sizeof(code_stripped));
    normalize_use_import_star(code_stripped);
    normalize_use_import_list_space(code_stripped, sizeof(code_stripped));

    char merged[16384];
    if (comment[0]) {
      const char *cptr = comment;
      while (*cptr == ' ' || *cptr == '\t')
        cptr++;
      if (code_stripped[0])
        snprintf(merged, sizeof(merged), "%s %s", code_stripped, cptr);
      else
        snprintf(merged, sizeof(merged), "%s", cptr);
    } else {
      snprintf(merged, sizeof(merged), "%s", code_stripped);
    }
    trim_trailing_ws(merged);

    int blank = (merged[0] == '\0');
    if (blank) {
      if (wrote_any)
        pending_blank = 1;
      continue;
    }

    int starts_with_close = 0;
    int delta = count_brace_delta_state(merged, &starts_with_close, &quote_state, &esc_state);
    int eff = indent;
    if (delta < 0)
      eff = indent + delta;
    else if (starts_with_close)
      eff = indent - 1;
    if (eff < 0)
      eff = 0;
    StrVec logical = {0};
    if (!split_attr_fn_line(merged, &logical))
      sv_push(&logical, merged);
    for (size_t i = 0; i < logical.len; i++) {
      enum NyTopKind cur_top = ny_top_kind(logical.items[i], eff);
      int want_blank = ny_wants_blank_before(prev_top, cur_top);
      int keep_pending = pending_blank;
      if (cur_top != NY_TOP_OTHER && prev_top != NY_TOP_NONE)
        keep_pending = want_blank;
      if (wrote_any && (want_blank || keep_pending))
        buf_append_char(&out, &out_len, &out_cap, '\n');
      pending_blank = 0;
      for (int j = 0; j < eff; j++)
        buf_append_str(&out, &out_len, &out_cap, "   ");
      buf_append_str(&out, &out_len, &out_cap, logical.items[i]);
      buf_append_char(&out, &out_len, &out_cap, '\n');
      if (cur_top != NY_TOP_OTHER)
        prev_top = cur_top;
      wrote_any = 1;
    }
    sv_free(&logical);
    indent += delta;
    if (indent < 0)
      indent = 0;
  }

  free(work);
  if (!out) {
    out = strdup("\n");
    return out;
  }
  out[out_len] = '\0';
  return out;
}

static int format_file(const char *path, int *changed) {
  *changed = 0;
  size_t n = 0;
  char *before = ny_read_file_raw(path, &n);
  if (!before)
    return 0;
  char *norm = NULL;
  normalize_line_endings(before, &norm);
  if (!norm) {
    free(before);
    return 0;
  }
  char *after = NULL;
  if (is_ny(path))
    after = format_ny_text(norm);
  else
    after = strdup(norm);
  free(norm);
  if (!after) {
    free(before);
    return 0;
  }
  if (strcmp(before, after) != 0) {
    if (!write_file(path, after, strlen(after))) {
      free(before);
      free(after);
      return 0;
    }
    *changed = 1;
  }
  free(before);
  free(after);
  return 1;
}

typedef struct {
  int line;
  int col;
  int net;
  char ctx[160];
} Swallow;

static int brace_check_file(const char *path, int fix, int verbose, int *has_issue) {
  *has_issue = 0;
  size_t n = 0;
  char *txt = ny_read_file_raw(path, &n);
  if (!txt)
    return 0;
  int line = 1, col = 1;
  int quote = 0, esc = 0, in_comment = 0;
  int stack_depth = 0;
  int extra = 0;
  Swallow sw[64];
  int sw_n = 0;

  for (size_t i = 0; i < n; i++) {
    char ch = txt[i];
    if (ch == '\n') {
      in_comment = 0;
      line++;
      col = 1;
      continue;
    }
    if (in_comment) {
      col++;
      continue;
    }
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      col++;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      col++;
      continue;
    }
    if (ch == ';') {
      int opens = 0, closes = 0;
      size_t j = i + 1;
      for (; j < n && txt[j] != '\n'; j++) {
        if (txt[j] == '{')
          opens++;
        else if (txt[j] == '}')
          closes++;
      }
      int net = closes - opens;
      if (net > 0 && sw_n < (int)(sizeof(sw) / sizeof(sw[0]))) {
        sw[sw_n].line = line;
        sw[sw_n].col = col;
        sw[sw_n].net = net;
        int k = 0;
        for (size_t t = i + 1; t < n && txt[t] != '\n' && k < (int)sizeof(sw[sw_n].ctx) - 1; t++)
          sw[sw_n].ctx[k++] = txt[t];
        sw[sw_n].ctx[k] = '\0';
        sw_n++;
      }
      in_comment = 1;
      col++;
      continue;
    }
    if (ch == '{')
      stack_depth++;
    else if (ch == '}') {
      if (stack_depth > 0)
        stack_depth--;
      else
        extra++;
    }
    col++;
  }

  int real_swallows = (stack_depth || extra) ? sw_n : 0;
  if (!(stack_depth || extra || real_swallows)) {
    if (verbose)
      printf("  %sOK%s %s\n", nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET), path);
    free(txt);
    return 1;
  }

  *has_issue = 1;
  printf("\n%sISSUE%s %s\n", nyt_clr(NYT_RED), nyt_clr(NYT_RESET), path);
  if (real_swallows > 0) {
    printf("  %s%d%s semicolon-swallow bug(s):\n", nyt_clr(NYT_YELLOW), real_swallows,
           nyt_clr(NYT_RESET));
    for (int i = 0; i < real_swallows && i < 6; i++)
      printf("    %sline %d:%d%s hides %d '}'  %s\n", nyt_clr(NYT_CYAN), sw[i].line,
             sw[i].col, nyt_clr(NYT_RESET), sw[i].net, sw[i].ctx);
  }
  if (stack_depth > 0)
    printf("  %s%d%s unclosed '{'\n", nyt_clr(NYT_YELLOW), stack_depth, nyt_clr(NYT_RESET));
  if (extra > 0)
    printf("  %s%d%s extra '}'\n", nyt_clr(NYT_YELLOW), extra, nyt_clr(NYT_RESET));

  if (fix && real_swallows > 0) {
    char *fixed = strdup(txt);
    if (fixed) {
      int l = 1;
      for (size_t i = 0; fixed[i]; i++) {
        int target = 0;
        for (int s = 0; s < real_swallows; s++) {
          if (sw[s].line == l) {
            target = 1;
            break;
          }
        }
        if (target) {
          int q = 0, e = 0;
          for (size_t j = i; fixed[j] && fixed[j] != '\n'; j++) {
            char ch = fixed[j];
            if (q) {
              if (e)
                e = 0;
              else if (ch == '\\')
                e = 1;
              else if (ch == q)
                q = 0;
              continue;
            }
            if (ch == '"' || ch == '\'') {
              q = ch;
              continue;
            }
            if (ch == ';') {
              memmove(fixed + j, fixed + j + 1, strlen(fixed + j + 1) + 1);
              break;
            }
          }
        }
        while (fixed[i] && fixed[i] != '\n')
          i++;
        if (!fixed[i])
          break;
        l++;
      }
      if (strcmp(fixed, txt) != 0) {
        write_file(path, fixed, strlen(fixed));
        printf("  %sfixed%s semicolon removed\n", nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      }
      free(fixed);
    }
  }
  free(txt);
  return 1;
}

typedef struct {
  char name[128];
  char file[PATH_MAX];
  int line;
  int end_line;
  int is_private;
  int is_method;
  int has_doc;
  int is_c;
  int untyped_params;
  int missing_return;
  int type_inferred_intent;
  char params[NY_PARAM_BUF_SZ];
} Fn;

typedef struct {
  Fn *items;
  size_t len;
  size_t cap;
} FnVec;

#define DEFINE_VEC_PUSH(name, VecType, ItemType, initial_cap) \
  static void name(VecType *v, const ItemType *item) {        \
    VEC_GROW_OR(v, initial_cap, return);                      \
    v->items[v->len++] = *item;                               \
  }

DEFINE_VEC_PUSH(fv_push, FnVec, Fn, 128)

static int ny_fn_has_type_infer_audit_directive(StrVec *lines, size_t fn_idx) {
  if (!lines || fn_idx == 0)
    return 0;
  int checked = 0;
  for (size_t i = fn_idx; i > 0 && checked < 4; --i) {
    const char *line = lines->items[i - 1];
    while (line && isspace((unsigned char)*line))
      line++;
    if (!line || !*line)
      continue;
    if (strncmp(line, ";;", 2) != 0 && strncmp(line, "//", 2) != 0)
      break;
    checked++;
    if (strstr(line, "audit: infer") || strstr(line, "hm: infer") ||
        strstr(line, "type: infer") || strstr(line, "type-infer"))
      return 1;
  }
  return 0;
}

DEFINE_VEC_PUSH(fpv_push, FilePressureVec, FilePressure, 128)
#undef DEFINE_VEC_PUSH

static void fpress_push(FunctionPressureVec *v, const FunctionPressure *f) {
  VEC_GROW_OR(v, 256, return);
  v->items[v->len++] = *f;
}

static void callseq_push(CallSeqVec *v, const char *seq, const char *file, int line) {
  for (size_t i = 0; i < v->len; i++) {
    if (strcmp(v->items[i].seq, seq) == 0) {
      v->items[i].count++;
      return;
    }
  }
  VEC_GROW_OR(v, 128, return);
  CallSeq *it = &v->items[v->len++];
  memset(it, 0, sizeof(*it));
  snprintf(it->seq, sizeof(it->seq), "%s", seq ? seq : "");
  snprintf(it->file, sizeof(it->file), "%s", file ? file : "");
  it->line = line;
  it->count = 1;
}

static void dupf_push(DupFnVec *v, const DupFn *f) {
  VEC_GROW_OR(v, 256, free(f ? f->norm : NULL); return);
  v->items[v->len++] = *f;
}

static void dupf_free(DupFnVec *v) {
  if (!v)
    return;
  for (size_t i = 0; i < v->len; i++)
    free(v->items[i].norm);
  free(v->items);
  v->items = NULL;
  v->len = 0;
  v->cap = 0;
}

static void dupg_push(DupGroupVec *v, size_t start, size_t count) {
  if (count < 2)
    return;
  VEC_GROW_OR(v, 128, return);
  v->items[v->len].start = start;
  v->items[v->len].count = count;
  v->len++;
}

static void dupg_free(DupGroupVec *v) {
  free(v->items);
  v->items = NULL;
  v->len = 0;
  v->cap = 0;
}

static int visual_len(const char *s) {
  int n = 0;
  for (; s && *s; s++)
    n += (*s == '\t') ? 3 : 1;
  return n;
}

static int has_trailing_ws(const char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
    n--;
  return n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t');
}

static const char *find_outside_string(const char *line, const char *needle) {
  if (!line || !needle || !*needle)
    return NULL;
  size_t n = strlen(needle);
  int quote = 0, esc = 0;
  for (const char *p = line; *p; p++) {
    if (!quote && p[0] == '/' && p[1] == '/')
      break;
    if (quote) {
      if (esc)
        esc = 0;
      else if (*p == '\\')
        esc = 1;
      else if (*p == quote)
        quote = 0;
      continue;
    }
    if (*p == '"' || *p == '\'') {
      quote = *p;
      continue;
    }
    if (strncmp(p, needle, n) == 0)
      return p;
  }
  return NULL;
}

static int parse_ny_fn_line(const char *line, char *name, size_t name_sz, char *params, size_t params_sz) {
  const char *p = lstrip_ws(line);
  while (*p == '@') {
    while (*p && !isspace((unsigned char)*p))
      p++;
    while (*p == ' ' || *p == '\t')
      p++;
  }
  if (strncmp(p, "fn", 2) != 0 || !(p[2] == ' ' || p[2] == '\t'))
    return 0;
  p += 2;
  while (*p == ' ' || *p == '\t')
    p++;
  if (!(isalpha((unsigned char)*p) || *p == '_'))
    return 0;
  size_t n = 0;
  while ((isalnum((unsigned char)*p) || *p == '_') && n + 1 < name_sz)
    name[n++] = *p++;
  name[n] = '\0';
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != '(')
    return 1;
  p++;
  n = 0;
  while (*p && *p != ')' && n + 1 < params_sz)
    params[n++] = *p++;
  params[n] = '\0';
  return 1;
}

static int ny_params_untyped_count(const char *params) {
  int count = 0;
  int quote = 0, esc = 0, depth = 0;
  const char *start = params ? params : "";
  for (const char *p = start;; p++) {
    char ch = *p;
    int at_end = ch == '\0';
    if (!at_end && quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (!at_end && (ch == '"' || ch == '\'')) {
      quote = ch;
      continue;
    }
    if (!at_end && (ch == '(' || ch == '[' || ch == '{')) {
      depth++;
      continue;
    }
    if (!at_end && (ch == ')' || ch == ']' || ch == '}')) {
      if (depth > 0)
        depth--;
      continue;
    }
    if (!at_end && !(ch == ',' && depth == 0))
      continue;

    const char *a = start;
    const char *b = p;
    while (a < b && isspace((unsigned char)*a))
      a++;
    while (b > a && isspace((unsigned char)b[-1]))
      b--;
    if (b > a) {
      if ((size_t)(b - a) >= 3 && a[0] == '.' && a[1] == '.' && a[2] == '.') {
        if (at_end)
          break;
        start = p + 1;
        continue;
      }
      while (b > a && *a == '.')
        a++;
      int typed = 0;
      for (const char *q = a; q < b; q++) {
        if (*q == ':') {
          typed = 1;
          break;
        }
      }
      if (!typed)
        count++;
    }
    if (at_end)
      break;
    start = p + 1;
  }
  return count;
}

static int ny_fn_missing_return_type(const char *line) {
  const char *p = lstrip_ws(line);
  while (*p == '@') {
    while (*p && !isspace((unsigned char)*p))
      p++;
    while (*p == ' ' || *p == '\t')
      p++;
  }
  if (strncmp(p, "fn", 2) != 0 || !(p[2] == ' ' || p[2] == '\t'))
    return 0;
  const char *close = strrchr(p, ')');
  if (!close)
    return 1;
  for (const char *q = close + 1; *q; q++) {
    if (isspace((unsigned char)*q))
      continue;
    if (*q == ':')
      return 0;
    if (*q == '{')
      return 1;
    return 0;
  }
  return 1;
}

static void ny_collect_fn_signature(StrVec *lines, size_t idx, char *out, size_t out_sz) {
  if (!out || out_sz == 0)
    return;
  out[0] = '\0';
  if (!lines || idx >= lines->len)
    return;
  size_t used = 0;
  for (size_t j = idx; j < lines->len && j < idx + 64; j++) {
    const char *line = lstrip_ws(lines->items[j]);
    if (j > idx && (strncmp(line, "fn ", 3) == 0 || line[0] == '}'))
      break;
    size_t n = strlen(line);
    if (used > 0 && used + 1 < out_sz)
      out[used++] = ' ';
    if (n > out_sz - used - 1)
      n = out_sz - used - 1;
    memcpy(out + used, line, n);
    used += n;
    out[used] = '\0';
    if (strncmp(line, "fn ", 3) == 0 && strstr(line, " as \""))
      break;
    if (find_outside_string(line, "{"))
      break;
  }
}

static int previous_doc_comment(StrVec *lines, size_t idx) {
  int scanned = 0;
  while (idx > 0 && scanned < 4) {
    idx--;
    const char *s = lstrip_ws(lines->items[idx]);
    if (!*s)
      break;
    if (strncmp(s, ";;", 2) == 0 || strncmp(s, "///", 3) == 0 || strstr(s, "*/") || strstr(s, "/**"))
      return 1;
    if (s[0] != '@')
      break;
    scanned++;
  }
  return 0;
}

static int ny_fn_has_doc(StrVec *lines, size_t idx) {
  if (previous_doc_comment(lines, idx))
    return 1;
  size_t body_line = idx;
  const char *open = find_outside_string(lines->items[idx], "{");
  if (open) {
    ++open;
    while (*open && isspace((unsigned char)*open))
      ++open;
    if (*open == '"' || (*open == 'r' && open[1] == '"'))
      return 1;
  } else {
    size_t limit = idx + 24;
    for (size_t j = idx + 1; j < lines->len && j < limit; j++) {
      const char *s = lstrip_ws(lines->items[j]);
      if (!*s || strncmp(s, ";;", 2) == 0)
        continue;
      open = find_outside_string(s, "{");
      if (open) {
        body_line = j;
        ++open;
        while (*open && isspace((unsigned char)*open))
          ++open;
        if (*open == '"' || (*open == 'r' && open[1] == '"'))
          return 1;
        break;
      }
    }
  }
  for (size_t j = body_line + 1; j < lines->len && j < body_line + 8; j++) {
    const char *s = lstrip_ws(lines->items[j]);
    if (!*s || strncmp(s, ";;", 2) == 0)
      continue;
    return s[0] == '"' || (s[0] == 'r' && s[1] == '"');
  }
  return 0;
}

static int ny_fn_is_forwarding_wrapper(const char *line) {
  const char *open = find_outside_string(line, "{");
  if (!open)
    return 0;
  const char *close = strrchr(open + 1, '}');
  if (!close || close <= open)
    return 0;
  char body[1024];
  size_t n = (size_t)(close - open - 1);
  if (n >= sizeof(body))
    return 0;
  memcpy(body, open + 1, n);
  body[n] = '\0';
  trim_trailing_ws(body);
  const char *p = lstrip_ws(body);
  if (starts_with_word(p, "return")) {
    p += 6;
    while (*p && isspace((unsigned char)*p))
      ++p;
  }
  if (!isalpha((unsigned char)*p) && *p != '_')
    return 0;
  while (isalnum((unsigned char)*p) || *p == '_')
    ++p;
  int saw_dot = 0;
  while (*p == '.') {
    saw_dot = 1;
    ++p;
    if (!isalpha((unsigned char)*p) && *p != '_')
      return 0;
    while (isalnum((unsigned char)*p) || *p == '_')
      ++p;
  }
  if (saw_dot && *p == '\0')
    return 1;
  if (*p != '(')
    return 0;
  int depth = 0, quote = 0, esc = 0;
  for (; *p; ++p) {
    if (quote) {
      if (esc)
        esc = 0;
      else if (*p == '\\')
        esc = 1;
      else if (*p == quote)
        quote = 0;
      continue;
    }
    if (*p == '"' || *p == '\'') {
      quote = *p;
      continue;
    }
    if (*p == '(')
      ++depth;
    else if (*p == ')') {
      --depth;
      if (depth == 0) {
        ++p;
        while (*p && isspace((unsigned char)*p))
          ++p;
        return *p == '\0';
      }
    }
  }
  return 0;
}

static int c_keyword_fn_name(const char *name) {
  static const char *bad[] = {"if", "for", "while", "switch", "return", "sizeof", "defined", NULL};
  for (int i = 0; bad[i]; i++)
    if (strcmp(name, bad[i]) == 0)
      return 1;
  return 0;
}

static int parse_c_fn_line(const char *line, const char *next, char *name, size_t name_sz) {
  const char *s = lstrip_ws(line);
  if (!*s || *s == '#' || strstr(s, "typedef") || strstr(s, " return ") || strchr(s, ';'))
    return 0;
  const char *open = strchr(s, '(');
  const char *close = strrchr(s, ')');
  if (!open || !close || close < open)
    return 0;
  if (!strchr(close, '{') && (!next || lstrip_ws(next)[0] != '{'))
    return 0;
  const char *p = open;
  while (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))
    p--;
  if (p == open)
    return 0;
  size_t n = (size_t)(open - p);
  if (n >= name_sz)
    n = name_sz - 1;
  memcpy(name, p, n);
  name[n] = '\0';
  return !c_keyword_fn_name(name);
}

static int c_brace_delta_line(const char *line) {
  int delta = 0, quote = 0, esc = 0;
  for (int i = 0; line[i]; i++) {
    char ch = line[i];
    if (!quote && ch == '/' && line[i + 1] == '/')
      break;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '{')
      delta++;
    else if (ch == '}')
      delta--;
  }
  return delta;
}

static int find_end_line(StrVec *lines, size_t start, int is_c) {
  int depth = 0;
  for (size_t i = start; i < lines->len; i++) {
    int starts_close = 0;
    int d = is_c ? c_brace_delta_line(lines->items[i]) : count_brace_delta(lines->items[i], &starts_close);
    depth += d;
    if (depth <= 0 && i > start)
      return (int)i + 1;
  }
  return (int)lines->len;
}

static uint64_t fnv1a64_mem(const char *s, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static char *dupe_join_lines(StrVec *lines, size_t start, size_t end_inclusive) {
  if (!lines || start >= lines->len || end_inclusive < start)
    return NULL;
  size_t cap = 0, len = 0;
  char *buf = NULL;
  for (size_t i = start; i <= end_inclusive && i < lines->len; i++) {
    buf_append_str(&buf, &len, &cap, lines->items[i] ? lines->items[i] : "");
    buf_append_char(&buf, &len, &cap, '\n');
  }
  buf_append_char(&buf, &len, &cap, '\0');
  return buf;
}

static char *dupe_strip_c_comments(const char *src, size_t n, size_t *out_n) {
  size_t cap = n + 1;
  char *out = (char *)malloc(cap);
  if (!out)
    return NULL;
  size_t w = 0;
  int quote = 0;
  int esc = 0;
  int in_line = 0;
  int in_block = 0;
  for (size_t i = 0; i < n; i++) {
    char ch = src[i];
    char next = (i + 1 < n) ? src[i + 1] : '\0';
    if (in_line) {
      if (ch == '\n' || ch == '\r') {
        in_line = 0;
        out[w++] = ch;
      }
      continue;
    }
    if (in_block) {
      if (ch == '*' && next == '/') {
        in_block = 0;
        i++;
      }
      continue;
    }
    if (quote) {
      out[w++] = ch;
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      out[w++] = ch;
      continue;
    }
    if (ch == '/' && next == '/') {
      in_line = 1;
      i++;
      continue;
    }
    if (ch == '/' && next == '*') {
      in_block = 1;
      i++;
      continue;
    }
    out[w++] = ch;
  }
  out[w] = '\0';
  if (out_n)
    *out_n = w;
  return out;
}

static char *dupe_normalize_c_body(const char *fn_src, int *norm_len) {
  if (norm_len)
    *norm_len = 0;
  if (!fn_src)
    return NULL;
  const char *open = strchr(fn_src, '{');
  const char *close = strrchr(fn_src, '}');
  if (!open || !close || close <= open)
    return NULL;
  open++;
  size_t body_n = (size_t)(close - open);
  size_t stripped_n = 0;
  char *stripped = dupe_strip_c_comments(open, body_n, &stripped_n);
  if (!stripped)
    return NULL;
  char *norm = (char *)malloc(stripped_n + 1);
  if (!norm) {
    free(stripped);
    return NULL;
  }
  size_t w = 0;
  for (size_t i = 0; i < stripped_n; i++) {
    unsigned char ch = (unsigned char)stripped[i];
    if (!isspace(ch))
      norm[w++] = (char)ch;
  }
  norm[w] = '\0';
  free(stripped);
  if (norm_len)
    *norm_len = (int)w;
  return norm;
}

static void scan_c_file_dupes(const char *path, DupFnVec *out, int min_len,
                              int *fn_total, int *kept_total) {
  if (!path || !out || (!nyt_ends_with(path, ".c") && !nyt_ends_with(path, ".h")))
    return;
  size_t n = 0;
  char *txt = ny_read_file_raw(path, &n);
  if (!txt)
    return;
  StrVec lines = {0};
  split_lines_keep_empty(txt, &lines);
  for (size_t i = 0; i < lines.len; i++) {
    char name[128] = {0};
    const char *next = (i + 1 < lines.len) ? lines.items[i + 1] : "";
    if (!parse_c_fn_line(lines.items[i], next, name, sizeof(name)))
      continue;
    if (fn_total)
      (*fn_total)++;
    int end_line = find_end_line(&lines, i, 1);
    size_t end_idx = end_line > 0 ? (size_t)(end_line - 1) : i;
    char *fn_src = dupe_join_lines(&lines, i, end_idx);
    int norm_len = 0;
    char *norm = dupe_normalize_c_body(fn_src, &norm_len);
    free(fn_src);
    if (!norm || norm_len < min_len) {
      free(norm);
      continue;
    }
    DupFn f;
    memset(&f, 0, sizeof(f));
    snprintf(f.file, sizeof(f.file), "%s", path);
    snprintf(f.name, sizeof(f.name), "%s", name);
    f.line = (int)i + 1;
    f.end_line = end_line;
    f.norm_len = norm_len;
    f.hash = fnv1a64_mem(norm, (size_t)norm_len);
    f.norm = norm;
    dupf_push(out, &f);
    if (kept_total)
      (*kept_total)++;
  }
  sv_free(&lines);
  free(txt);
}

static void analyze_file(const char *path, FnVec *fns, IssueVec *issues, AnalyzeStats *stats) {
  size_t n = 0;
  char *txt = ny_read_file_raw(path, &n);
  if (!txt)
    return;
  StrVec lines = {0};
  split_lines_keep_empty(txt, &lines);

  int is_ny_file = nyt_ends_with(path, ".ny");
  int is_c_file = nyt_ends_with(path, ".c");
  int is_h_file = nyt_ends_with(path, ".h");
  int is_py_file = nyt_ends_with(path, ".py");
  int wants_public_docs = is_ny_file && path_is_std_lib_source(path);
  stats->files++;
  stats->ny_files += is_ny_file;
  stats->c_files += is_c_file;
  stats->h_files += is_h_file;
  stats->py_files += is_py_file;

  int ny_impl_depth = 0;
  int ny_template_depth = 0;
  int ny_main_guard_depth = 0;
  int ny_layout_depth = 0;
  for (size_t i = 0; i < lines.len; i++) {
    const char *line = lines.items[i];
    int ln = (int)i + 1;
    const char *stripped = lstrip_ws(line);
    int starts_impl = is_ny_file && starts_with_word(stripped, "impl");
    int starts_template = is_ny_file && starts_with_ny_template_line(stripped);
    int starts_main_guard = is_ny_file && starts_with_main_guard(stripped);
    int starts_layout = is_ny_file && starts_with_word(stripped, "layout");
    int in_layout = starts_layout || ny_layout_depth > 0;
    int in_impl = starts_impl || ny_impl_depth > 0;
    int in_template = starts_template || ny_template_depth > 0;
    int in_main_guard = starts_main_guard || ny_main_guard_depth > 0;
    if (has_trailing_ws(line)) {
      stats->trailing_ws++;
      issue_push(issues, path, ln, (int)strlen(line), "NYFMT1100", "warning",
                 "trailing whitespace", "run ny-fmt on this file");
    }
    const char *tab = strchr(line, '\t');
    if (tab) {
      stats->tabs++;
      issue_push(issues, path, ln, (int)(tab - line) + 1, "NYFMT1101", "warning",
                 "tab indentation; use three spaces", "ny-fmt expands tabs during formatting");
    }
    int line_width = visual_len(line);
    if (line_width > 120 && !(is_ny_file && ny_long_line_is_compact_ok(line, line_width))) {
      stats->long_lines++;
      issue_push(issues, path, ln, 121, "NYFMT1102", "note",
                 "line exceeds 120 columns", "split dense expressions or generated-looking declarations");
    }
    if (find_outside_string(line, "TODO") || find_outside_string(line, "FIXME")) {
      stats->todos++;
      issue_push(issues, path, ln, 1, "NYFMT1200", "note",
                 "open TODO/FIXME marker", "keep if intentional; otherwise resolve or move to docs");
    }
    if (is_ny_file) {
      int legacy_star_col = legacy_use_import_star_col(line);
      if (legacy_star_col >= 0) {
        issue_push(issues, path, ln, legacy_star_col + 1, "NYFMT1300", "note",
                   "legacy import spelling: use module *",
                   "prefer bare 'use module'; it already imports exported names and keeps the module leaf alias");
      }
      int compact_list_col = compact_use_import_list_col(line);
      if (compact_list_col >= 0) {
        issue_push(issues, path, ln, compact_list_col + 1, "NYFMT1301", "note",
                   "missing space before import list",
                   "prefer 'use module (name)' so formatter, docs, and LSP stay aligned");
      }
      int repeated_mut_col = repeated_mut_decl_col(line);
      if (repeated_mut_col >= 0) {
        issue_push(issues, path, ln, repeated_mut_col, "NYFMT1302", "warning",
                   "same-line mutable declarations repeat 'mut'",
                   "write one declaration group, e.g. 'mut n, b, s = a, b, c'");
      }
      int layout_col = in_layout ? reversed_layout_field_col(line) : -1;
      if (layout_col >= 0) {
        issue_push(issues, path, ln, layout_col, "NYFMT1305", "warning",
                   "layout field uses name-first syntax",
                   "layout fields are type-first: write 'f32 center_x,' not 'center_x: f32'");
      }
      char group_note[512];
      int group_col = adjacent_simple_decl_group_col(&lines, i, group_note, sizeof(group_note));
      if (group_col >= 0) {
        issue_push(issues, path, ln, group_col, "NYFMT1303", "note",
                   "adjacent simple declarations can be grouped", group_note);
      }
      char assign_note[512];
      int assign_col = same_line_assignment_group_col(line, assign_note, sizeof(assign_note));
      if (assign_col < 0)
        assign_col = adjacent_simple_assign_group_col(&lines, i, assign_note, sizeof(assign_note));
      if (assign_col >= 0) {
        issue_push(issues, path, ln, assign_col, "NYFMT1304", "note",
                   "simple assignments can be grouped", assign_note);
      }
    }

    if (is_ny_file && !in_template) {
      char name[128] = {0};
      char params[NY_PARAM_BUF_SZ] = {0};
      if (parse_ny_fn_line(line, name, sizeof(name), params, sizeof(params))) {
        char fn_sig[2048];
        ny_collect_fn_signature(&lines, i, fn_sig, sizeof(fn_sig));
        parse_ny_fn_line(fn_sig, name, sizeof(name), params, sizeof(params));
        int fn_has_body = find_outside_string(fn_sig, "{") != NULL;
        Fn fn;
        memset(&fn, 0, sizeof(fn));
        snprintf(fn.name, sizeof(fn.name), "%s", name);
        snprintf(fn.params, sizeof(fn.params), "%s", params);
        snprintf(fn.file, sizeof(fn.file), "%s", path);
        fn.line = ln;
        fn.end_line = find_end_line(&lines, i, 0);
        fn.is_method = in_impl;
        fn.is_private = (fn.name[0] == '_') || fn.is_method || in_main_guard;
        fn.has_doc = !fn_has_body || ny_fn_has_doc(&lines, i) || ny_fn_is_forwarding_wrapper(line);
        fn.is_c = 0;
        fn.untyped_params = ny_params_untyped_count(params);
        fn.missing_return = ny_fn_missing_return_type(fn_sig);
        fn.type_inferred_intent = ny_fn_has_type_infer_audit_directive(&lines, i);
        fv_push(fns, &fn);
        stats->functions++;
        stats->ny_functions++;
        if (!fn.is_private)
          stats->public_functions++;
        if (wants_public_docs && fn_has_body && !fn.is_private && !fn.has_doc) {
          stats->missing_doc++;
          issue_push(issues, path, ln, 1, "NYFMT2000", "warning",
                     "public std Ny function is missing a doc string",
                     "stdlib APIs should start with a short string literal doc");
        }
        if (fn.end_line - fn.line + 1 > 120) {
          stats->long_functions++;
          issue_push(issues, path, ln, 1, "NYFMT2001", "warning",
                     "large Ny function; split compact helpers out",
                     "long functions make fmt, diagnostics, and perf work harder");
        }
      }
    } else if (is_c_file || is_h_file) {
      char name[128] = {0};
      const char *next = (i + 1 < lines.len) ? lines.items[i + 1] : "";
      if (parse_c_fn_line(line, next, name, sizeof(name))) {
        Fn fn;
        memset(&fn, 0, sizeof(fn));
        snprintf(fn.name, sizeof(fn.name), "%s", name);
        snprintf(fn.file, sizeof(fn.file), "%s", path);
        fn.line = ln;
        fn.end_line = find_end_line(&lines, i, 1);
        fn.is_private = strstr(line, "static") != NULL;
        fn.has_doc = previous_doc_comment(&lines, i);
        fn.is_c = 1;
        fv_push(fns, &fn);
        stats->functions++;
        stats->c_functions++;
        if (fn.end_line - fn.line + 1 > 180) {
          stats->long_functions++;
          issue_push(issues, path, ln, 1, "NYFMT3100", "warning",
                     "large C function; split into focused helpers",
                     "large tooling functions are where formatter bugs hide");
        }
      }
      if (find_outside_string(line, "strcpy(") || find_outside_string(line, "strcat(") ||
          find_outside_string(line, "sprintf(")) {
        stats->unsafe_c++;
        issue_push(issues, path, ln, 1, "NYFMT3200", "warning",
                   "unbounded C string helper", "prefer snprintf/memcpy with explicit sizes");
      }
    }
    if (is_ny_file && (starts_impl || ny_impl_depth > 0)) {
      int starts_close = 0;
      ny_impl_depth += count_brace_delta(line, &starts_close);
      if (ny_impl_depth < 0)
        ny_impl_depth = 0;
    }
    if (is_ny_file && (starts_template || ny_template_depth > 0)) {
      int starts_close = 0;
      ny_template_depth += count_brace_delta(line, &starts_close);
      if (ny_template_depth < 0)
        ny_template_depth = 0;
    }
    if (is_ny_file && (starts_main_guard || ny_main_guard_depth > 0)) {
      int starts_close = 0;
      ny_main_guard_depth += count_brace_delta(line, &starts_close);
      if (ny_main_guard_depth < 0)
        ny_main_guard_depth = 0;
    }
    if (is_ny_file && (starts_layout || ny_layout_depth > 0)) {
      int starts_close = 0;
      ny_layout_depth += count_brace_delta(line, &starts_close);
      if (ny_layout_depth < 0)
        ny_layout_depth = 0;
    }
  }

  sv_free(&lines);
  free(txt);
}

static void analyze_duplicates(FnVec *fns, IssueVec *issues, AnalyzeStats *stats) {
  for (size_t i = 0; i < fns->len; i++) {
    if (fns->items[i].is_private)
      continue;
    int duplicate = 0;
    for (size_t j = i + 1; j < fns->len; j++) {
      if (fns->items[j].is_private)
        continue;
      if (strcmp(fns->items[i].file, fns->items[j].file) == 0 &&
          strcmp(fns->items[i].name, fns->items[j].name) == 0) {
        duplicate = 1;
        issue_push(issues, fns->items[j].file, fns->items[j].line, 1, "NYFMT2100", "warning",
                   "duplicate function name in one file", fns->items[j].name);
      }
    }
    stats->duplicate_names += duplicate;
  }
}

typedef struct {
  const char *modes;
  const char *sections;
} AuditModeMap;

static int token_list_contains(const char *tokens, const char *token) {
  if (!tokens || !token || !*token)
    return 0;
  size_t token_len = strlen(token);
  const char *start = tokens;
  while (*start) {
    const char *end = strchr(start, '|');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len == token_len && strncmp(start, token, len) == 0)
      return 1;
    if (!end)
      break;
    start = end + 1;
  }
  return 0;
}

static const AuditModeMap k_audit_mode_maps[] = {
    {"smart|overhaul",
     "bugs|bloat|batteries|profiles|trim|loops|legacy|methods|contracts|specialize|meta|types|dead|constants|calls"},
    {"modules", "bloat|batteries|profiles"},
    {"trim", "bloat|profiles|legacy|methods|contracts|specialize|meta"},
    {"bugs|bug|correctness|lint", "bugs"},
    {"similarities", "calls"},
    {"layouts", "layout"},
    {"contracts|backend", "contracts"},
    {"meta|metaprog|roadmap|codebase|features", "meta"},
    {"specialize|specialization|constfold|partial", "specialize"},
    {"legacy", "methods"},
    {"methods|syntax", "methods|legacy|types"},
    {"constants|consts", "constants"},
    {"loops|continue|guards", "trim|loops"},
};

static int audit_wants_one(const char *mode, const char *section) {
  if (!mode || !*mode || strcmp(mode, "all") == 0)
    return 1;
  if (strcmp(mode, section) == 0)
    return 1;
  for (size_t i = 0; i < sizeof(k_audit_mode_maps) / sizeof(k_audit_mode_maps[0]); i++) {
    if (token_list_contains(k_audit_mode_maps[i].modes, mode) &&
        token_list_contains(k_audit_mode_maps[i].sections, section))
      return 1;
  }
  return 0;
}

static int audit_wants(const char *mode, const char *section) {
  if (!mode || !*mode || strcmp(mode, "all") == 0)
    return 1;
  const char *start = mode;
  while (*start) {
    const char *end = strchr(start, '|');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    char token[64];
    if (len >= sizeof(token))
      len = sizeof(token) - 1;
    memcpy(token, start, len);
    token[len] = '\0';
    if (audit_wants_one(token, section))
      return 1;
    if (!end)
      break;
    start = end + 1;
  }
  return 0;
}

static int count_substr(const char *s, const char *needle) {
  if (!s || !needle || !*needle)
    return 0;
  int count = 0;
  size_t n = strlen(needle);
  for (const char *p = strstr(s, needle); p; p = strstr(p + n, needle))
    count++;
  return count;
}

static int count_word_occurrences(const char *text, const char *word) {
  if (!text || !word || !*word)
    return 0;
  int count = 0;
  size_t n = strlen(word);
  for (const char *p = strstr(text, word); p; p = strstr(p + n, word)) {
    unsigned char a = (p == text) ? 0 : (unsigned char)p[-1];
    unsigned char b = (unsigned char)p[n];
    if (!(isalnum(a) || a == '_') && !(isalnum(b) || b == '_'))
      count++;
  }
  return count;
}

static void audit_str_append(char *dst, size_t dst_sz, const char *src) {
  if (!dst || dst_sz == 0 || !src || !*src)
    return;
  size_t used = strlen(dst);
  if (used >= dst_sz - 1)
    return;
  snprintf(dst + used, dst_sz - used, "%s", src);
}

static void audit_sample_append(char *dst, size_t dst_sz, const char *sep, const char *item) {
  if (!dst || dst_sz == 0 || !item || !*item)
    return;
  if (dst[0])
    audit_str_append(dst, dst_sz, sep ? sep : ", ");
  audit_str_append(dst, dst_sz, item);
}

static int audit_line_has_truthy_env(const char *line) {
  if (!line || !find_outside_string(line, "env("))
    return 0;
  if (!find_outside_string(line, "==") && !find_outside_string(line, "!="))
    return 0;
  return strstr(line, "\"1\"") || strstr(line, "\"0\"") || strstr(line, "\"true\"") ||
         strstr(line, "\"false\"") || strstr(line, "\"yes\"") || strstr(line, "\"no\"") ||
         strstr(line, "\"on\"") || strstr(line, "\"off\"");
}

static int audit_count_named_call(const char *line, const char *name, int allow_free,
                                  int allow_receiver) {
  if (!line || !name || !*name)
    return 0;
  int count = 0;
  int quote = 0, esc = 0;
  size_t name_n = strlen(name);
  for (size_t i = 0; line[i]; i++) {
    char ch = line[i];
    if (!quote && ch == ';')
      break;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (!(isalpha((unsigned char)ch) || ch == '_'))
      continue;
    size_t start = i;
    i++;
    while (isalnum((unsigned char)line[i]) || line[i] == '_')
      i++;
    size_t end = i;
    size_t look = end;
    while (line[look] == ' ' || line[look] == '\t')
      look++;
    if (line[look] != '(') {
      i = end ? end - 1 : start;
      continue;
    }
    if (end - start == name_n && strncmp(line + start, name, name_n) == 0) {
      unsigned char prev = start == 0 ? 0 : (unsigned char)line[start - 1];
      int receiver = prev == '.';
      int free_call = !receiver && !isalnum(prev) && prev != '_';
      const char *before = line + start;
      while (before > line && isspace((unsigned char)before[-1]))
        before--;
      if ((size_t)(before - line) >= 2 && before[-2] == 'f' && before[-1] == 'n') {
        const char fn_prev = ((size_t)(before - line) >= 3) ? before[-3] : 0;
        if (!(isalnum((unsigned char)fn_prev) || fn_prev == '_')) {
          i = end ? end - 1 : start;
          continue;
        }
      }
      if ((receiver && allow_receiver) || (free_call && allow_free))
        count++;
    }
    i = end ? end - 1 : start;
  }
  return count;
}

static int audit_line_eager_default(const char *line) {
  if (!line)
    return 0;
  const char *p = find_outside_string(line, ".get(");
  if (!p)
    return 0;
  return strstr(p, "list(") || strstr(p, "dict(") || strstr(p, "malloc(") ||
         strstr(p, "[") || strstr(p, "{");
}

static int audit_line_legacy_calls(const char *line) {
  static const char *names[] = {
      "dict_get", "dict_set", "dict_has", "dict_del", "dict_len",
      "str_len", "bytes_len", "type_len",
      "set_idx", "store_item", "__store_item", "append", "contains", "get", NULL};
  int count = 0;
  int quote = 0, esc = 0;
  for (size_t i = 0; line && line[i]; i++) {
    char ch = line[i];
    if (!quote && ch == ';')
      break;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (!(isalpha((unsigned char)ch) || ch == '_'))
      continue;
    size_t start = i;
    i++;
    while (isalnum((unsigned char)line[i]) || line[i] == '_')
      i++;
    size_t end = i;
    size_t look = end;
    while (line[look] == ' ' || line[look] == '\t')
      look++;
    if (line[look] != '(') {
      i = end ? end - 1 : start;
      continue;
    }
    unsigned char prev = start == 0 ? 0 : (unsigned char)line[start - 1];
    if (prev == '.' || isalnum(prev) || prev == '_') {
      i = end ? end - 1 : start;
      continue;
    }
    const char *before = line + start;
    while (before > line && isspace((unsigned char)before[-1]))
      before--;
    if ((size_t)(before - line) >= 2 && before[-2] == 'f' && before[-1] == 'n') {
      const char fn_prev = ((size_t)(before - line) >= 3) ? before[-3] : 0;
      if (!(isalnum((unsigned char)fn_prev) || fn_prev == '_')) {
        i = end ? end - 1 : start;
        continue;
      }
    }
    char name[32];
    size_t n = end - start;
    if (n >= sizeof(name))
      n = sizeof(name) - 1;
    memcpy(name, line + start, n);
    name[n] = '\0';
    for (int k = 0; names[k]; k++) {
      if (strcmp(name, names[k]) == 0) {
        count++;
        break;
      }
    }
    i = end ? end - 1 : start;
  }
  return count;
}

static int audit_count_outside_string(const char *line, const char *needle) {
  int count = 0;
  size_t n = needle ? strlen(needle) : 0;
  const char *p = line;
  while (p && *p && n > 0) {
    const char *hit = find_outside_string(p, needle);
    if (!hit)
      break;
    count++;
    p = hit + n;
  }
  return count;
}

static int audit_line_receiver_rewrite_calls(const char *line) {
  static const char *free_names[] = {
      "bytes_to_hex", "hex_to_bytes", "bytes_to_base64", "base64_to_bytes",
      "bytes_to_long", "long_to_bytes", "bytes_to_str", "str_to_bytes",
      "bytes_concat", "bytes_xor", "bytes_repeat", "bytes_reverse",
      "bytes_trim_leading_zeros", NULL};
  static const char *qualified[] = {
      "bin.bytes_to_hex(", "bin.hex_to_bytes(", "bin.bytes_to_base64(",
      "bin.base64_to_bytes(", "bin.bytes_to_long(", "bin.long_to_bytes(",
      "bin.bytes_to_str(", "bin.str_to_bytes(", "bin.bytes_concat(",
      "bin.bytes_xor(", "bin.bytes_repeat(", "bin.bytes_reverse(",
      "bin.bytes_trim_leading_zeros(", NULL};
  int count = 0;
  for (int i = 0; free_names[i]; i++)
    count += audit_count_named_call(line, free_names[i], 1, 0);
  for (int i = 0; qualified[i]; i++)
    count += audit_count_outside_string(line, qualified[i]);
  return count;
}

static int audit_method_property_name(const char *name) {
  static const char *names[] = {
      "len",        "sqrt",       "bytes",      "hex",      "as_bytes",
      "as_hex",     "is_square",  "is_prime",  "next_prime", "prev_prime",
      "bitlen",     "long",       "text",      "base64",   "to_list",
      "to_bytes",   "unhex",      "base64_decode", "trim0", "rev", NULL};
  for (int i = 0; names[i]; i++)
    if (strcmp(name, names[i]) == 0)
      return 1;
  return 0;
}

static int audit_line_zero_arg_method_properties(const char *line) {
  int count = 0;
  int quote = 0, esc = 0;
  for (size_t i = 0; line && line[i]; i++) {
    char ch = line[i];
    if (!quote && ch == ';')
      break;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch != '.')
      continue;
    size_t start = i + 1;
    if (!(isalpha((unsigned char)line[start]) || line[start] == '_'))
      continue;
    size_t end = start + 1;
    while (isalnum((unsigned char)line[end]) || line[end] == '_')
      end++;
    char name[48];
    size_t name_n = end - start;
    if (name_n >= sizeof(name))
      name_n = sizeof(name) - 1;
    memcpy(name, line + start, name_n);
    name[name_n] = '\0';
    if (!audit_method_property_name(name)) {
      i = end ? end - 1 : start;
      continue;
    }
    size_t p = end;
    while (line[p] == ' ' || line[p] == '\t')
      p++;
    if (line[p] != '(')
      continue;
    p++;
    while (line[p] == ' ' || line[p] == '\t')
      p++;
    if (line[p] == ')')
      count++;
  }
  return count;
}

static void audit_add_method_syntax_findings(const char *path, StrVec *lines, IssueVec *issues,
                                             AuditStats *stats) {
  int count = 0;
  int first_line = 0;
  for (size_t i = 0; i < lines->len; i++) {
    int hits = audit_line_zero_arg_method_properties(lines->items[i]);
    if (hits > 0 && !first_line)
      first_line = (int)i + 1;
    count += hits;
  }
  if (count <= 0)
    return;
  char msg[256];
  snprintf(msg, sizeof(msg), "%d zero-argument method call(s) can be property syntax", count);
  issue_push(issues, path, first_line ? first_line : 1, 1, "NYAUD8101",
             count >= 12 ? "warning" : "note", msg,
             "prefer x.len, Z(n).sqrt, b.hex, b.long, and similar attached properties when no arguments are passed");
  stats->findings++;
  stats->trim_targets++;
  stats->method_syntax += count;
}

static int audit_extract_branch_condition(const char *line, char *out, size_t out_sz);

static int audit_case_selector_simple(const char *selector) {
  if (!selector || !*selector)
    return 0;
  if (strcmp(selector, "i") == 0 || strcmp(selector, "j") == 0 ||
      strcmp(selector, "k") == 0 || strcmp(selector, "idx") == 0 ||
      strcmp(selector, "pos") == 0 || strcmp(selector, "off") == 0)
    return 0;
  int saw_name = 0;
  for (const char *p = selector; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    if (isalnum(ch) || ch == '_' || ch == '.') {
      saw_name = 1;
      continue;
    }
    if (isspace(ch))
      continue;
    return 0;
  }
  return saw_name;
}

static int audit_case_segment_selector(const char *start, const char *end,
                                       char *out, size_t out_sz) {
  char seg[512];
  copy_trim_span(seg, sizeof(seg), start, end);
  if (!seg[0])
    return 0;
  const char *ops[] = {"==", "!=", "<=", ">=", "<", ">", NULL};
  const char *best = NULL;
  for (int i = 0; ops[i]; i++) {
    const char *p = find_outside_string(seg, ops[i]);
    if (p && (!best || p < best))
      best = p;
  }
  if (!best)
    return 0;
  char lhs[128];
  copy_trim_span(lhs, sizeof(lhs), seg, best);
  if (!audit_case_selector_simple(lhs))
    return 0;
  snprintf(out, out_sz, "%s", lhs);
  return 1;
}

static int audit_condition_case_selector(const char *cond, char *selector,
                                         size_t selector_sz) {
  if (!cond || !selector || selector_sz == 0)
    return 0;
  selector[0] = '\0';
  const char *start = cond;
  int quote = 0, esc = 0, depth = 0;
  int comparisons = 0;
  for (const char *p = cond;; p++) {
    char ch = *p;
    int at_end = ch == '\0';
    if (!at_end && quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (!at_end && (ch == '"' || ch == '\'')) {
      quote = ch;
      continue;
    }
    if (!at_end && is_open_delim(ch)) {
      depth++;
      continue;
    }
    if (!at_end && is_close_delim(ch)) {
      if (depth > 0)
        depth--;
      continue;
    }
    int split = at_end || (depth == 0 &&
                           ((ch == '&' && p[1] == '&') ||
                            (ch == '|' && p[1] == '|')));
    if (!split)
      continue;
    char piece_selector[128];
    if (audit_case_segment_selector(start, p, piece_selector, sizeof(piece_selector))) {
      if (selector[0] && strcmp(selector, piece_selector) != 0)
        return 0;
      snprintf(selector, selector_sz, "%s", piece_selector);
      comparisons++;
    }
    if (at_end)
      break;
    p++;
    start = p + 1;
  }
  return comparisons;
}

static int audit_line_case_dispatch_score(const char *line, char *selector,
                                          size_t selector_sz) {
  if (selector && selector_sz > 0)
    selector[0] = '\0';
  const char *s = lstrip_ws(line);
  if (!s || !*s)
    return 0;
  int branch_chain = starts_with_word(s, "elif") ||
                     find_outside_string(s, "} elif") ||
                     find_outside_string(s, "} else if");
  int branch = starts_with_word(s, "if") || branch_chain;
  if (!branch || find_outside_string(s, "case "))
    return 0;
  char cond[512];
  if (!audit_extract_branch_condition(s, cond, sizeof(cond)))
    return 0;
  char local_selector[128];
  int comparisons = audit_condition_case_selector(cond, local_selector, sizeof(local_selector));
  if (comparisons <= 0)
    return 0;
  if (selector && selector_sz > 0)
    snprintf(selector, selector_sz, "%s", local_selector);
  if (comparisons >= 2)
    return comparisons;
  return branch_chain ? 1 : 0;
}

static int audit_line_has_marker(const char *line, const char *needle) {
  return line && needle && find_outside_string(line, needle) != NULL;
}

static int audit_line_has_plain_call(const char *line, const char *name) {
  if (!line || !name || !*name)
    return 0;
  size_t name_len = strlen(name);
  int quote = 0, esc = 0;
  for (size_t i = 0; line[i]; i++) {
    char ch = line[i];
    if (!quote && ch == ';')
      break;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (!(isalpha((unsigned char)ch) || ch == '_'))
      continue;
    size_t start = i;
    while (isalnum((unsigned char)line[i]) || line[i] == '_')
      i++;
    size_t end = i;
    if (end - start != name_len || memcmp(line + start, name, name_len) != 0) {
      i = end ? end - 1 : start;
      continue;
    }
    unsigned char prev = start == 0 ? 0 : (unsigned char)line[start - 1];
    if (prev == '.' || isalnum(prev) || prev == '_') {
      i = end ? end - 1 : start;
      continue;
    }
    while (line[i] == ' ' || line[i] == '\t')
      i++;
    if (line[i] == '(')
      return 1;
    i = end ? end - 1 : start;
  }
  return 0;
}

static int audit_line_has_comptime_marker(const char *line) {
  return audit_line_has_marker(line, "comptime{") || audit_line_has_marker(line, "comptime {");
}

static int audit_line_has_compile_time_probe(const char *line) {
  return audit_line_has_plain_call(line, "__os_name") || audit_line_has_plain_call(line, "__arch_name") ||
         audit_line_has_plain_call(line, "__main") || audit_line_has_plain_call(line, "os") ||
         audit_line_has_plain_call(line, "arch") || audit_line_has_plain_call(line, "sizeof") ||
         audit_line_has_plain_call(line, "__layout_size") ||
         audit_line_has_plain_call(line, "__layout_align") ||
         audit_line_has_plain_call(line, "__layout_offset");
}

static int audit_line_brace_delta(const char *line) {
  int quote = 0, esc = 0, delta = 0;
  for (size_t i = 0; line && line[i]; i++) {
    char ch = line[i];
    if (!quote && ch == ';')
      break;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '{')
      delta++;
    else if (ch == '}')
      delta--;
  }
  return delta;
}

static int audit_case_line_has_guard(const char *line) {
  const char *arrow = find_outside_string(line, "->");
  const char *guard = find_outside_string(line, " if ");
  return arrow && guard && guard < arrow;
}

static int audit_parse_decl_name(const char *line, const char *kw, char *name, size_t name_sz) {
  const char *p = lstrip_ws(line);
  if (!starts_with_word(p, kw))
    return 0;
  p += strlen(kw);
  while (*p == ' ' || *p == '\t')
    p++;
  if (strcmp(kw, "layout") == 0 &&
      (starts_with_word(p, "record") || starts_with_word(p, "shape"))) {
    p += starts_with_word(p, "record") ? 6 : 5;
    while (*p == ' ' || *p == '\t')
      p++;
  }
  if (!(isalpha((unsigned char)*p) || *p == '_'))
    return 0;
  size_t n = 0;
  while ((isalnum((unsigned char)*p) || *p == '_' || *p == '.') && n + 1 < name_sz)
    name[n++] = *p++;
  name[n] = '\0';
  return n > 0;
}

static int audit_parse_top_def_name(const char *line, char *name, size_t name_sz) {
  const char *p = lstrip_ws(line);
  if (!starts_with_word(p, "def"))
    return 0;
  p += 3;
  while (*p == ' ' || *p == '\t')
    p++;
  if (!(isalpha((unsigned char)*p) || *p == '_'))
    return 0;
  size_t n = 0;
  while ((isalnum((unsigned char)*p) || *p == '_') && n + 1 < name_sz)
    name[n++] = *p++;
  name[n] = '\0';
  if (n == 0)
    return 0;
  while (*p == ' ' || *p == '\t')
    p++;
  return *p == '=';
}

static int audit_is_const_like_name(const char *name) {
  if (!name || !*name)
    return 0;
  int has_upper = 0;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    if (!(isalnum(*p) || *p == '_'))
      return 0;
    if (islower(*p))
      return 0;
    if (isupper(*p))
      has_upper = 1;
  }
  return has_upper;
}

static int audit_is_key_const_name(const char *name) {
  return name && (strncmp(name, "KEY_", 4) == 0 || strncmp(name, "VK_", 3) == 0 ||
                  strncmp(name, "WM_", 3) == 0 || strncmp(name, "XK_", 3) == 0 ||
                  strncmp(name, "BUTTON", 6) == 0);
}

static int audit_is_api_const_name(const char *name) {
  if (!name || !*name)
    return 0;
  static const char *prefixes[] = {
      "KEY_", "VK_", "WM_", "XK_", "WL_", "XDG_", "XI_", "RR_",
      "SM_", "SW_", "WS_", "CS_", "PM_", "KF_", "RID_", "RIDEV_",
      "RIM_", "XBUTTON", "IDC_", "IDI_", "ICON_", "GWL_", "GCLP_",
      "DWM_", "MAPVK_", "DM_", "CDS_", "DISP_", "MDT_", "CF_",
      "CP_", "HWND_", NULL};
  for (int i = 0; prefixes[i]; i++) {
    size_t n = strlen(prefixes[i]);
    if (strncmp(name, prefixes[i], n) == 0)
      return 1;
  }
  if (strncmp(name, "Xkb", 3) == 0 || strncmp(name, "Button", 6) == 0 ||
      strncmp(name, "Mod", 3) == 0 || strstr(name, "Mask") != NULL)
    return 1;
  return 0;
}

static const char *audit_leaf_name(const char *name) {
  const char *dot = name ? strrchr(name, '.') : NULL;
  return dot ? dot + 1 : name;
}

static int audit_fn_family_key(const char *name, char *key, size_t key_sz) {
  static const char *suffixes[] = {
      "_x", "_y", "_z", "_2d", "_3d", "_f32", "_f64", "_i32", "_i64",
      "_u32", "_u64", "_encrypt", "_decrypt", "_encode", "_decode",
      "_show", "_hide", "_iconify", "_maximize", "_restore", NULL};
  if (!name || !*name)
    return 0;
  for (int i = 0; suffixes[i]; i++) {
    size_t nn = strlen(name);
    size_t sn = strlen(suffixes[i]);
    if (nn <= sn || strcmp(name + nn - sn, suffixes[i]) != 0)
      continue;
    size_t n = nn - sn;
    if (n < 3)
      continue;
    if (n + 3 >= key_sz)
      n = key_sz - 4;
    memcpy(key, name, n);
    key[n++] = '_';
    key[n++] = '*';
    key[n] = '\0';
    return 1;
  }
  return 0;
}

static int audit_line_has_key_constant(const char *line) {
  static const char *marks[] = {"KEY_", "VK_", "XK_", "VK_FORMAT_", "0xff", NULL};
  for (int i = 0; marks[i]; i++) {
    if (audit_line_has_marker(line, marks[i]))
      return 1;
  }
  return 0;
}

static void audit_add_metaprog_findings(const char *path, const char *txt, FnVec *fns,
                                        StrVec *lines, IssueVec *issues,
                                        AuditStats *stats) {
  const int is_ny_file = nyt_ends_with(path, ".ny");
  const int is_c_file = nyt_ends_with(path, ".c") || nyt_ends_with(path, ".h");
  int dict_key_gets = 0, dict_key_first = 0, dict_type_guards = 0;
  int mallocs = 0, frees = 0, malloc_first = 0;
  int resource_blocks = 0, defer_free_lines = 0, defer_free_first = 0;
  int builder_cleanup_lines = 0, builder_cleanup_first = 0;
  int set_chain = 0, set_chain_first = 0;
  int key_table_lines = 0, key_table_first = 0;
  int layout_allocs = 0, layout_alloc_first = 0, store_layouts = 0, load_layouts = 0;
  int layout_guard_lines = 0, layout_shape_lines = 0, layout_record_lines = 0;
  int externs = 0, extern_first = 0;
  int parser_errors = 0, parser_error_first = 0;
  int issue_pushes = 0, issue_push_first = 0;
  int ny_diag_errors = 0, ny_diag_error_first = 0, ny_diag_fixes = 0;
  int verbose_main_guards = 0, verbose_main_guard_first = 0;

  for (size_t i = 0; i < lines->len; i++) {
    const char *raw = lines->items[i];
    const char *line = lstrip_ws(raw);
    int ln = (int)i + 1;
    if (!*line)
      continue;

    if (is_ny_file) {
      if (find_outside_string(line, ".get(\"") || find_outside_string(line, ".get('")) {
        if (!dict_key_first)
          dict_key_first = ln;
        dict_key_gets++;
      }
      if (audit_line_has_plain_call(line, "is_dict") || audit_line_has_plain_call(line, "is_list") ||
          audit_line_has_plain_call(line, "is_str"))
        dict_type_guards++;
      if (find_outside_string(line, ".set(\"") || find_outside_string(line, ".set('")) {
        if (!set_chain_first)
          set_chain_first = ln;
        set_chain++;
      }
      if (audit_line_has_plain_call(line, "malloc")) {
        if (!malloc_first)
          malloc_first = ln;
        mallocs++;
      }
      if (starts_with_word(line, "with"))
        resource_blocks++;
      if (audit_line_has_plain_call(line, "free") || starts_with_word(line, "defer"))
        frees++;
      if (starts_with_word(line, "defer") && audit_line_has_plain_call(line, "free")) {
        if (!defer_free_first)
          defer_free_first = ln;
        defer_free_lines++;
      }
      if (audit_line_has_plain_call(line, "builder_free") ||
          audit_line_has_plain_call(line, "close")) {
        if (!builder_cleanup_first)
          builder_cleanup_first = ln;
        builder_cleanup_lines++;
      }
      if (audit_line_has_key_constant(line) &&
          (find_outside_string(line, "->") || find_outside_string(line, "=>") ||
           find_outside_string(line, "==") || find_outside_string(line, ".set("))) {
        if (!key_table_first)
          key_table_first = ln;
        key_table_lines++;
      }
      if (audit_line_has_plain_call(line, "store_layout"))
        store_layouts++;
      if (audit_line_has_plain_call(line, "load_layout"))
        load_layouts++;
      if (audit_line_has_plain_call(line, "malloc") &&
          find_outside_string(line, "__layout_size(")) {
        if (!layout_alloc_first)
          layout_alloc_first = ln;
        layout_allocs++;
      }
      if (starts_with_word(line, "layout guard"))
        layout_guard_lines++;
      if (starts_with_word(line, "layout shape"))
        layout_shape_lines++;
      if (starts_with_word(line, "layout record"))
        layout_record_lines++;
      if (starts_with_word(line, "extern fn")) {
        if (!extern_first)
          extern_first = ln;
        externs++;
      }
      if (starts_with_verbose_main_guard(line)) {
        if (!verbose_main_guard_first)
          verbose_main_guard_first = ln;
        verbose_main_guards++;
      }
    } else if (is_c_file) {
      if (audit_line_has_plain_call(line, "parser_error")) {
        if (!parser_error_first)
          parser_error_first = ln;
        parser_errors++;
      }
      if (audit_line_has_plain_call(line, "ny_diag_error") ||
          audit_line_has_plain_call(line, "ny_diag_warning") ||
          audit_line_has_plain_call(line, "ny_diag_error_code") ||
          audit_line_has_plain_call(line, "ny_diag_warning_code")) {
        if (!ny_diag_error_first)
          ny_diag_error_first = ln;
        ny_diag_errors++;
      }
      if (audit_line_has_plain_call(line, "ny_diag_fix"))
        ny_diag_fixes++;
      if (audit_line_has_plain_call(line, "issue_push") ||
          audit_line_has_plain_call(line, "audit_push_trim") ||
          audit_line_has_plain_call(line, "audit_push_bug")) {
        if (!issue_push_first)
          issue_push_first = ln;
        issue_pushes++;
      }
    }
  }

  if (is_ny_file && dict_key_gets >= 8) {
    audit_push_trim(issues, stats, path, dict_key_first, "NYAUD4101",
                    dict_key_gets >= 24 ? "warning" : "note",
                    "shape/record boundary candidate",
                    "many string-key reads; declare a layout shape with defaults and use layout guard Shape: value = input else { ... } at the boundary");
  }
  if (is_ny_file && dict_key_gets >= 5 && dict_type_guards >= 3) {
    audit_push_trim(issues, stats, path, dict_key_first, "NYAUD4102", "note",
                    "typed boundary guard candidate",
                    "clustered type checks and dict reads should become layout guard Shape: value = input else { ... } once the boundary shape is declared");
  }
  if (is_ny_file && layout_allocs >= 2 && store_layouts >= 2) {
    audit_push_trim(issues, stats, path, layout_alloc_first, "NYAUD4103", "note",
                    "layout record constructor candidate",
                    "repeated malloc(__layout_size) plus store_layout can usually become layout record Type derive(default, store) with typed defaults");
  }
  if (is_ny_file && load_layouts >= 10 && layout_shape_lines == 0 && layout_record_lines == 0) {
    audit_push_trim(issues, stats, path, 1, "NYAUD4104", "note",
                    "direct layout helper candidate",
                    "many load_layout calls can be replaced with layout record/shape derive(load) helpers so hot code has typed field accessors");
  }
  if (is_ny_file && dict_key_gets >= 5 && layout_shape_lines > 0 && layout_guard_lines == 0) {
    audit_push_trim(issues, stats, path, dict_key_first, "NYAUD4105", "note",
                    "missing layout guard boundary",
                    "this file declares layout shapes but still does manual dict reads; use layout guard Shape: value = input else { ... } near the input edge");
  }
  if (is_ny_file && mallocs >= 2 && frees >= 1 && resource_blocks == 0) {
    audit_push_trim(issues, stats, path, malloc_first, "NYAUD4201", "note",
                    "resource block candidate",
                    "malloc/free or defer ownership is repeated here; use with ptr: name = alloc { ... } for scoped pointer ownership");
  }
  if (is_ny_file && defer_free_lines >= 2) {
    audit_push_trim(issues, stats, path, defer_free_first, "NYAUD4202", "note",
                    "manual defer-free cleanup can collapse",
                    "replace repeated def ptr + defer { free(ptr) } pairs with with ptr: name = alloc { ... }");
  }
  if (is_ny_file && builder_cleanup_lines >= 3 && resource_blocks == 0) {
    audit_push_trim(issues, stats, path, builder_cleanup_first, "NYAUD4203", "note",
                    "closeable/builder resource candidate",
                    "builder_free/close patterns can become with Type: name = create { ... } plus a typed close(resource) helper");
  }
  if (is_ny_file && set_chain >= 6) {
    audit_push_trim(issues, stats, path, set_chain_first, "NYAUD4301",
                    set_chain >= 16 ? "warning" : "note",
                    "startup table construction candidate",
                    "repeated literal .set/dict_set chains should become comptime table/static data instead of runtime map setup");
  }
  if (is_ny_file && key_table_lines >= 8) {
    audit_push_trim(issues, stats, path, key_table_first, "NYAUD4302", "note",
                    "key/enum map can be table-driven",
                    "use case ranges for local dispatch, or comptime table plus comptime match for reusable key/format maps");
  }
  if (is_ny_file && externs >= 5) {
    audit_push_trim(issues, stats, path, extern_first, "NYAUD4401", "note",
                    "extern surface can be block/code-generated",
                    "group adjacent FFI declarations and consider data-driven module generation for repeated native wrappers");
  }
  if (is_ny_file && verbose_main_guards > 0) {
    audit_push_trim(issues, stats, path, verbose_main_guard_first, "NYAUD4901", "note",
                    "verbose direct-main guard",
                    "prefer #main { ... } for module self-tests and direct-run code");
  }
  if (is_c_file && parser_errors >= 12) {
    audit_push_trim(issues, stats, path, parser_error_first, "NYAUD4501", "note",
                    "diagnostic rule candidate",
                    "many parser_error sites in one file; repeated syntax guidance should move toward compile-time diagnostic rules");
  }
  if (is_c_file && issue_pushes >= 12) {
    audit_push_trim(issues, stats, path, issue_push_first, "NYAUD4502", "note",
                    "audit diagnostic table candidate",
                    "many issue_push sites can become data-driven diagnostic definitions with shared severity/fix text");
  }
  if (is_c_file && ny_diag_errors >= 10) {
    audit_push_trim(issues, stats, path, ny_diag_error_first, "NYAUD4503", "note",
                    "compile-time diagnostic rule candidate",
                    ny_diag_fixes >= 3
                        ? "many compiler diagnostics with fixes live here; move repeated call-shape checks toward comptime diagnostic rule declarations"
                        : "many compiler diagnostics live here; repeated call/type predicates can become comptime diagnostic rule declarations");
  }
  if (is_ny_file && (strstr(path, "/platform/") || strstr(path, "/render/vk/") ||
                     strstr(path, "/window/")) &&
      (externs >= 3 || key_table_lines >= 6)) {
    audit_push_trim(issues, stats, path,
                    extern_first ? extern_first : (key_table_first ? key_table_first : 1),
                    "NYAUD4601", "note",
                    "data-driven backend module candidate",
                    "platform/Vulkan files with repeated wrappers or maps can use module ... generated from Spec { props; emit template(...) }");
  }

  for (size_t i = 0; i < fns->len; i++) {
    Fn *fn = &fns->items[i];
    if (strcmp(fn->file, path) != 0)
      continue;
    char key[128];
    if (!audit_fn_family_key(fn->name, key, sizeof(key)))
      continue;
    int count = 1;
    int first_line = fn->line;
    char sample[192] = {0};
    snprintf(sample, sizeof(sample), "%s", fn->name);
    for (size_t j = i + 1; j < fns->len; j++) {
      Fn *other = &fns->items[j];
      if (strcmp(other->file, path) != 0)
        continue;
      char other_key[128];
      if (!audit_fn_family_key(other->name, other_key, sizeof(other_key)) ||
          strcmp(key, other_key) != 0)
        continue;
      count++;
      audit_sample_append(sample, sizeof(sample), ", ", other->name);
    }
    if (count >= 3) {
      char msg[256];
      snprintf(msg, sizeof(msg), "%d sibling functions look like one family: %s", count, sample);
      audit_push_trim(issues, stats, path, first_line, "NYAUD4701", "note", msg,
                      "use comptime template plus comptime emit or for name in comptime [...] { emit make(name) } to generate axis/type/backend variants");
      break;
    }
  }

  if (is_ny_file) {
    for (size_t i = 0; i < lines->len; i++) {
      char decl[128];
      const char *line = lines->items[i];
      if (!audit_parse_decl_name(line, "layout", decl, sizeof(decl)) &&
          !audit_parse_decl_name(line, "struct", decl, sizeof(decl)))
        continue;
      const char *leaf = audit_leaf_name(decl);
      if (!leaf || !*leaf)
        continue;
      char needle[128];
      int helpers = 0;
      static const char *suffixes[] = {"_to_str", "_eq", "_hash", "_zero", "_copy", "_default", NULL};
      size_t leaf_len = strlen(leaf);
      for (int k = 0; suffixes[k]; k++) {
        if (leaf_len + strlen(suffixes[k]) >= sizeof(needle))
          continue;
        snprintf(needle, sizeof(needle), "%s%s", leaf, suffixes[k]);
        helpers += count_word_occurrences(txt, needle) > 0;
      }
      if (leaf_len + 8 < sizeof(needle)) {
        snprintf(needle, sizeof(needle), "default_%s", leaf);
        helpers += count_word_occurrences(txt, needle) > 0;
      }
      if (helpers >= 2) {
        audit_push_trim(issues, stats, path, (int)i + 1, "NYAUD4801", "note",
                        "derive block candidate",
                        "layout/struct has repeated helper functions; use layout record Type derive(default, eq, hash, debug_str) or layout Type derive(load, store, zero)");
        break;
      }
    }
  }
}

static void audit_add_specialize_findings(const char *path, StrVec *lines, IssueVec *issues,
                                          AuditStats *stats) {
  if (!nyt_ends_with(path, ".ny"))
    return;
  int in_case = 0, start = 0, depth = 0, arms = 0, ranges = 0, guards = 0;
  int case_has_probe = 0, case_has_comptime = 0, emitted_direct = 0;
  int layout_probe_count = 0, layout_probe_first = 0;
  for (size_t i = 0; i < lines->len; i++) {
    const char *raw = lines->items[i];
    const char *line = lstrip_ws(raw);
    int ln = (int)i + 1;
    if (!*line)
      continue;

    if (find_outside_string(line, "comptime_match")) {
      issue_push(issues, path, ln, 1, "NYAUD3105", "warning",
                 "legacy comptime table call syntax",
                 "use the keyword form: comptime match Table(value, fallback)");
      stats->findings++;
      stats->trim_targets++;
    }

    if (audit_line_has_plain_call(line, "__layout_offset") ||
        audit_line_has_plain_call(line, "__layout_size") ||
        audit_line_has_plain_call(line, "__layout_align")) {
      if (layout_probe_count == 0)
        layout_probe_first = ln;
      layout_probe_count++;
    }

    int branch = starts_with_word(line, "if") || starts_with_word(line, "elif") ||
                 find_outside_string(line, "} elif") || find_outside_string(line, "} else if");
    if (branch && !audit_line_has_comptime_marker(line) &&
        audit_line_has_compile_time_probe(line)) {
      issue_push(issues, path, ln, 1, "NYAUD3102", "note",
                 "branch can be compile-time-pruned",
                 "wrap the condition in comptime{ return ... } or use platform guards so AOT emits only the selected path");
      stats->findings++;
      stats->trim_targets++;
    }

    if (!in_case && starts_with_word(line, "case")) {
      in_case = 1;
      start = ln;
      depth = audit_line_brace_delta(raw);
      arms = count_substr(line, "->");
      ranges = audit_line_has_marker(line, "..") ? 1 : 0;
      guards = audit_case_line_has_guard(line) ? 1 : 0;
      case_has_probe = audit_line_has_compile_time_probe(line);
      case_has_comptime = audit_line_has_comptime_marker(line);
      emitted_direct = 0;
      if (!case_has_comptime && case_has_probe) {
        issue_push(issues, path, ln, 1, "NYAUD3101", "note",
                   "case selector can be compile-time-specialized",
                   "use case comptime{ return selector } so literal/range arms lower to the selected guardless arm only");
        stats->findings++;
        stats->trim_targets++;
        emitted_direct = 1;
      }
      if (depth <= 0)
        in_case = 0;
      continue;
    }

    if (in_case) {
      arms += count_substr(line, "->");
      if (audit_line_has_marker(line, ".."))
        ranges++;
      if (audit_case_line_has_guard(line))
        guards++;
      depth += audit_line_brace_delta(raw);
      if (depth > 0)
        continue;
      if (!case_has_comptime && !case_has_probe && !emitted_direct && guards == 0 &&
          (arms >= 12 || (arms >= 10 && ranges > 0))) {
        issue_push(issues, path, start, 1, "NYAUD3103", "note",
                   "large literal/range case can specialize when selector is build-time data",
                   "if it is a keymap/format/tag table, move the arms to comptime table and call it with comptime match; otherwise compute the selector with comptime{ return ... }");
        stats->findings++;
        stats->trim_targets++;
      }
      in_case = 0;
    }
  }
  if (layout_probe_count >= 3) {
    issue_push(issues, path, layout_probe_first, 1, "NYAUD3104", "note",
               "repeated layout metadata probes can be generated",
               "use comptime fields(Type) as f { emit ... } to generate offset/type assertions or accessors once per field");
    stats->findings++;
    stats->trim_targets++;
  }
}

static int audit_repeated_line(StrVec *seen, const char *raw) {
  const char *s = lstrip_ws(raw);
  if (!*s || strncmp(s, ";;", 2) == 0 || strncmp(s, "//", 2) == 0 || *s == '#')
    return 0;
  if (isalpha((unsigned char)s[0]) || s[0] == '_') {
    const char *p = s + 1;
    while (isalnum((unsigned char)*p) || *p == '_')
      p++;
    if (*p == ':')
      return 0;
  }
  char tmp[256];
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1]))
    n--;
  if (n < 32)
    return 0;
  if (n >= sizeof(tmp))
    n = sizeof(tmp) - 1;
  memcpy(tmp, s, n);
  tmp[n] = '\0';
  for (size_t i = 0; i < seen->len; i++) {
    if (strcmp(seen->items[i], tmp) == 0)
      return 1;
  }
  sv_push(seen, tmp);
  return 0;
}

static int audit_keyword_boundary(char ch) {
  return ch == '\0' || isspace((unsigned char)ch) || ch == '(' || ch == '{' || ch == ';';
}

static int audit_starts_keyword(const char *s, const char *kw) {
  if (!s || !kw)
    return 0;
  s = lstrip_ws(s);
  size_t n = strlen(kw);
  return strncmp(s, kw, n) == 0 && audit_keyword_boundary(s[n]);
}

static int audit_line_indent(const char *line) {
  int n = 0;
  for (const char *p = line ? line : ""; *p == ' ' || *p == '\t'; p++)
    n += (*p == '\t') ? 3 : 1;
  return n;
}

static void audit_code_line(const char *line, int is_ny_file, char *out, size_t out_sz) {
  if (!out || out_sz == 0)
    return;
  out[0] = '\0';
  if (!line)
    return;
  int quote = 0, esc = 0;
  size_t w = 0;
  for (size_t i = 0; line[i] && w + 1 < out_sz; i++) {
    char ch = line[i];
    if (quote) {
      out[w++] = ch;
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      out[w++] = ch;
      continue;
    }
    if (is_ny_file && ch == ';')
      break;
    if (!is_ny_file && ch == '/' && line[i + 1] == '/')
      break;
    out[w++] = ch;
  }
  out[w] = '\0';
  trim_trailing_ws(out);
}

static const char *audit_find_matching_paren(const char *open) {
  if (!open || *open != '(')
    return NULL;
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = open; *p; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '(') {
      depth++;
      continue;
    }
    if (ch == ')') {
      depth--;
      if (depth == 0)
        return p;
    }
  }
  return NULL;
}

static int audit_span_has_top_level_comma(const char *start, const char *end) {
  if (!start || !end || end < start)
    return 0;
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = start; p < end; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (is_open_delim(ch)) {
      depth++;
      continue;
    }
    if (is_close_delim(ch)) {
      if (depth > 0)
        depth--;
      continue;
    }
    if (ch == ',' && depth == 0)
      return 1;
  }
  return 0;
}

static int audit_simple_free_arg(const char *raw, char *arg, size_t arg_sz, int *col_out) {
  if (!raw || !arg || arg_sz == 0)
    return 0;
  char code[8192];
  audit_code_line(raw, 1, code, sizeof(code));
  const char *s = lstrip_ws(code);
  if (strncmp(s, "free", 4) != 0 || (isalnum((unsigned char)s[4]) || s[4] == '_'))
    return 0;
  const char *p = s + 4;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != '(')
    return 0;
  const char *close = audit_find_matching_paren(p);
  if (!close)
    return 0;
  const char *tail = close + 1;
  while (*tail == ' ' || *tail == '\t')
    tail++;
  if (*tail != '\0')
    return 0;
  const char *a = p + 1;
  const char *b = close;
  while (a < b && isspace((unsigned char)*a))
    a++;
  while (b > a && isspace((unsigned char)b[-1]))
    b--;
  if (a >= b || audit_span_has_top_level_comma(a, b))
    return 0;
  size_t n = (size_t)(b - a);
  if (n >= arg_sz)
    n = arg_sz - 1;
  memcpy(arg, a, n);
  arg[n] = '\0';
  if (col_out) {
    const char *raw_s = lstrip_ws(raw);
    *col_out = (int)(raw_s - raw) + 1;
  }
  return 1;
}

static void audit_add_variadic_free_findings(const char *path, StrVec *lines, IssueVec *issues,
                                             AuditStats *stats) {
  if (!nyt_ends_with(path, ".ny") || !lines)
    return;
  size_t i = 0;
  while (i < lines->len) {
    char first_arg[128];
    int first_col = 1;
    if (!audit_simple_free_arg(lines->items[i], first_arg, sizeof(first_arg), &first_col)) {
      i++;
      continue;
    }
    char sample[384] = {0};
    audit_str_append(sample, sizeof(sample), first_arg);
    size_t start = i;
    size_t count = 1;
    i++;
    while (i < lines->len) {
      char arg[128];
      int col = 1;
      if (!audit_simple_free_arg(lines->items[i], arg, sizeof(arg), &col))
        break;
      if (count < 5)
        audit_sample_append(sample, sizeof(sample), ", ", arg);
      count++;
      i++;
    }
    if (count >= 2) {
      char msg[256];
      char note[512];
      snprintf(msg, sizeof(msg), "%zu adjacent free() calls can use variadic free", count);
      snprintf(note, sizeof(note), "collapse cleanup to free(%s%s)", sample,
               count > 5 ? ", ..." : "");
      issue_push(issues, path, (int)start + 1, first_col, "NYAUD4204", "note", msg, note);
      stats->findings++;
      stats->trim_targets++;
    }
  }
}

static const char *audit_branch_condition_bounds(const char *line, const char **close_out) {
  if (close_out)
    *close_out = NULL;
  const char *s = lstrip_ws(line);
  const char *p = NULL;
  if (audit_starts_keyword(s, "if"))
    p = s + 2;
  else if (audit_starts_keyword(s, "elif"))
    p = s + 4;
  else if (audit_starts_keyword(s, "while"))
    p = s + 5;
  else if (audit_starts_keyword(s, "for"))
    p = s + 3;
  else {
    p = find_outside_string(s, "else if");
    if (p)
      p += 7;
  }
  if (!p)
    return NULL;
  while (*p && *p != '(')
    p++;
  if (*p != '(')
    return NULL;
  const char *close = audit_find_matching_paren(p);
  if (!close)
    return NULL;
  if (close_out)
    *close_out = close;
  return p + 1;
}

static int audit_extract_branch_condition(const char *line, char *out, size_t out_sz) {
  const char *close = NULL;
  const char *open = audit_branch_condition_bounds(line, &close);
  if (!open || !close || close <= open || !out || out_sz == 0)
    return 0;
  copy_trim_span(out, out_sz, open, close);
  return out[0] != '\0';
}

static int audit_condition_has_assignment(const char *cond) {
  int quote = 0, esc = 0;
  for (const char *p = cond ? cond : ""; *p; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch != '=')
      continue;
    char prev = p > cond ? p[-1] : 0;
    char next = p[1];
    if (next == '=' || next == '>' || prev == '=' || prev == '!' || prev == '<' ||
        prev == '>' || prev == ':' || prev == '+' || prev == '-' || prev == '*' ||
        prev == '/' || prev == '%')
      continue;
    return 1;
  }
  return 0;
}

static int audit_condition_is_constant_branch(const char *cond) {
  char buf[128];
  copy_trim_span(buf, sizeof(buf), cond, NULL);
  return strcmp(buf, "true") == 0 || strcmp(buf, "false") == 0 ||
         strcmp(buf, "nil") == 0 || strcmp(buf, "0") == 0 || strcmp(buf, "1") == 0;
}

static void audit_compact_expr(const char *src, char *out, size_t out_sz) {
  if (!out || out_sz == 0)
    return;
  out[0] = '\0';
  if (!src)
    return;
  int quote = 0, esc = 0;
  size_t w = 0;
  for (const char *p = src; *p && w + 1 < out_sz; p++) {
    char ch = *p;
    if (quote) {
      out[w++] = ch;
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      out[w++] = ch;
      continue;
    }
    if (isspace((unsigned char)ch))
      continue;
    out[w++] = ch;
  }
  out[w] = '\0';
}

static int audit_same_expr(const char *a, const char *b) {
  char ca[512], cb[512];
  audit_compact_expr(a, ca, sizeof(ca));
  audit_compact_expr(b, cb, sizeof(cb));
  return ca[0] && strcmp(ca, cb) == 0;
}

static int audit_previous_branch_same_condition(StrVec *lines, size_t idx, int is_ny_file,
                                                int indent, const char *cond) {
  if (!lines || !cond || !*cond || idx == 0)
    return 0;
  int scanned = 0;
  for (size_t j = idx; j-- > 0 && scanned < 200;) {
    scanned++;
    char code[4096];
    audit_code_line(lines->items[j], is_ny_file, code, sizeof(code));
    const char *s = lstrip_ws(code);
    if (!*s)
      continue;
    int jindent = audit_line_indent(code);
    if (jindent < indent)
      break;
    if (jindent > indent)
      continue;
    char prev_cond[512];
    if (audit_extract_branch_condition(code, prev_cond, sizeof(prev_cond))) {
      if (audit_same_expr(cond, prev_cond))
        return (int)j + 1;
      continue;
    }
    if (*s == '}')
      continue;
    break;
  }
  return 0;
}

static int audit_condition_duplicate_symbol_operand(const char *cond, const char *op,
                                                    char *dup, size_t dup_sz) {
  if (!cond || !op || strlen(op) != 2)
    return 0;
  char seen[16][160];
  int seen_count = 0;
  const char *start = cond;
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = cond;; p++) {
    char ch = *p;
    int at_end = ch == '\0';
    if (!at_end && quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (!at_end && (ch == '"' || ch == '\'')) {
      quote = ch;
      continue;
    }
    if (!at_end && is_open_delim(ch)) {
      depth++;
      continue;
    }
    if (!at_end && is_close_delim(ch)) {
      if (depth > 0)
        depth--;
      continue;
    }
    int split = at_end || (depth == 0 && ch == op[0] && p[1] == op[1]);
    if (!split)
      continue;
    char item[160];
    copy_trim_span(item, sizeof(item), start, p);
    char compact[160];
    audit_compact_expr(item, compact, sizeof(compact));
    if (compact[0]) {
      for (int i = 0; i < seen_count; i++) {
        if (strcmp(seen[i], compact) == 0) {
          if (dup && dup_sz > 0)
            snprintf(dup, dup_sz, "%s", item);
          return 1;
        }
      }
      if (seen_count < 16) {
        snprintf(seen[seen_count], sizeof(seen[seen_count]), "%s", compact);
        seen_count++;
      }
    }
    if (at_end)
      break;
    p++;
    start = p + 1;
  }
  return 0;
}

static int audit_condition_duplicate_logical_operand(const char *cond, char *dup,
                                                    size_t dup_sz, char *op_out,
                                                    size_t op_out_sz) {
  if (audit_condition_duplicate_symbol_operand(cond, "&&", dup, dup_sz)) {
    if (op_out && op_out_sz > 0)
      snprintf(op_out, op_out_sz, "&&");
    return 1;
  }
  if (audit_condition_duplicate_symbol_operand(cond, "||", dup, dup_sz)) {
    if (op_out && op_out_sz > 0)
      snprintf(op_out, op_out_sz, "||");
    return 1;
  }
  return 0;
}

static int audit_trim_unwrapped_expr(const char *src, char *out, size_t out_sz) {
  if (!out || out_sz == 0)
    return 0;
  copy_trim_span(out, out_sz, src, NULL);
  while (out[0] == '(') {
    const char *close = audit_find_matching_paren(out);
    if (!close)
      break;
    const char *tail = close + 1;
    while (*tail == ' ' || *tail == '\t')
      tail++;
    if (*tail != '\0')
      break;
    char inner[512];
    copy_trim_span(inner, sizeof(inner), out + 1, close);
    snprintf(out, out_sz, "%s", inner);
  }
  return out[0] != '\0';
}

static int audit_extract_simple_comparison(const char *expr, char *lhs, size_t lhs_sz,
                                           char *op, size_t op_sz, char *rhs,
                                           size_t rhs_sz) {
  if (!expr || !lhs || !op || !rhs || lhs_sz == 0 || op_sz == 0 || rhs_sz == 0)
    return 0;
  lhs[0] = '\0';
  op[0] = '\0';
  rhs[0] = '\0';
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = expr; *p; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (is_open_delim(ch)) {
      depth++;
      continue;
    }
    if (is_close_delim(ch)) {
      if (depth > 0)
        depth--;
      continue;
    }
    if (depth != 0)
      continue;

    const char *found = NULL;
    size_t op_len = 0;
    if ((ch == '=' && p[1] == '=') || (ch == '!' && p[1] == '=') ||
        (ch == '<' && p[1] == '=') || (ch == '>' && p[1] == '=')) {
      found = p;
      op_len = 2;
    } else if ((ch == '<' && p[1] != '<') || (ch == '>' && p[1] != '>')) {
      found = p;
      op_len = 1;
    }
    if (!found)
      continue;
    copy_trim_span(lhs, lhs_sz, expr, found);
    copy_trim_span(op, op_sz, found, found + op_len);
    copy_trim_span(rhs, rhs_sz, found + op_len, NULL);
    return lhs[0] != '\0' && rhs[0] != '\0';
  }
  return 0;
}

static int audit_expr_is_literalish(const char *expr) {
  char buf[160];
  copy_trim_span(buf, sizeof(buf), expr, NULL);
  if (!buf[0])
    return 0;
  if (buf[0] == '"' || buf[0] == '\'')
    return 1;
  if (isdigit((unsigned char)buf[0]) ||
      ((buf[0] == '-' || buf[0] == '+') && isdigit((unsigned char)buf[1])))
    return 1;
  return strcmp(buf, "true") == 0 || strcmp(buf, "false") == 0 ||
         strcmp(buf, "nil") == 0;
}

static int audit_expr_has_call(const char *expr) {
  int quote = 0, esc = 0;
  for (const char *p = expr ? expr : ""; *p; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '(')
      return 1;
  }
  return 0;
}

static void audit_copy_cstr(char *dst, size_t dst_sz, const char *src) {
  if (!dst || dst_sz == 0)
    return;
  if (!src)
    src = "";
  size_t n = strlen(src);
  if (n >= dst_sz)
    n = dst_sz - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static int audit_condition_self_comparison(const char *cond, char *lhs_out,
                                           size_t lhs_sz, char *op_out,
                                           size_t op_sz) {
  const char *start = cond ? cond : "";
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = start;; p++) {
    char ch = *p;
    int at_end = ch == '\0';
    if (!at_end && quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (!at_end && (ch == '"' || ch == '\'')) {
      quote = ch;
      continue;
    }
    if (!at_end && is_open_delim(ch)) {
      depth++;
      continue;
    }
    if (!at_end && is_close_delim(ch)) {
      if (depth > 0)
        depth--;
      continue;
    }
    int split = at_end || (depth == 0 && ((ch == '&' && p[1] == '&') ||
                                          (ch == '|' && p[1] == '|')));
    if (!split)
      continue;

    char raw[512], seg[512], lhs[256], op[8], rhs[256], ca[256], cb[256];
    copy_trim_span(raw, sizeof(raw), start, p);
    audit_trim_unwrapped_expr(raw, seg, sizeof(seg));
    if (audit_extract_simple_comparison(seg, lhs, sizeof(lhs), op, sizeof(op),
                                        rhs, sizeof(rhs)) &&
        !audit_expr_has_call(lhs) && !audit_expr_has_call(rhs)) {
      audit_compact_expr(lhs, ca, sizeof(ca));
      audit_compact_expr(rhs, cb, sizeof(cb));
      if (ca[0] && strcmp(ca, cb) == 0) {
        if (lhs_out && lhs_sz > 0)
          audit_copy_cstr(lhs_out, lhs_sz, lhs);
        if (op_out && op_sz > 0)
          audit_copy_cstr(op_out, op_sz, op);
        return 1;
      }
    }
    if (at_end)
      break;
    p++;
    start = p + 1;
  }
  return 0;
}

static int audit_condition_literal_pair_conflict(const char *cond, const char *split_op,
                                                 const char *cmp_op, char *lhs_out,
                                                 size_t lhs_sz, char *first_out,
                                                 size_t first_sz, char *second_out,
                                                 size_t second_sz) {
  char seen_lhs[16][160];
  char seen_rhs[16][160];
  char seen_rhs_raw[16][160];
  int seen_count = 0;
  const char *start = cond ? cond : "";
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = start;; p++) {
    char ch = *p;
    int at_end = ch == '\0';
    if (!at_end && quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (!at_end && (ch == '"' || ch == '\'')) {
      quote = ch;
      continue;
    }
    if (!at_end && is_open_delim(ch)) {
      depth++;
      continue;
    }
    if (!at_end && is_close_delim(ch)) {
      if (depth > 0)
        depth--;
      continue;
    }
    int split = at_end || (depth == 0 && ch == split_op[0] && p[1] == split_op[1]);
    if (!split)
      continue;

    char raw[512], seg[512], lhs[256], op[8], rhs[256], lhs_key[160], rhs_key[160];
    copy_trim_span(raw, sizeof(raw), start, p);
    audit_trim_unwrapped_expr(raw, seg, sizeof(seg));
    if (audit_extract_simple_comparison(seg, lhs, sizeof(lhs), op, sizeof(op),
                                        rhs, sizeof(rhs)) &&
        strcmp(op, cmp_op) == 0 && audit_expr_is_literalish(rhs) &&
        !audit_expr_has_call(lhs)) {
      audit_compact_expr(lhs, lhs_key, sizeof(lhs_key));
      audit_compact_expr(rhs, rhs_key, sizeof(rhs_key));
      for (int i = 0; i < seen_count; i++) {
        if (strcmp(seen_lhs[i], lhs_key) == 0 && strcmp(seen_rhs[i], rhs_key) != 0) {
          if (lhs_out && lhs_sz > 0)
            audit_copy_cstr(lhs_out, lhs_sz, lhs);
          if (first_out && first_sz > 0)
            audit_copy_cstr(first_out, first_sz, seen_rhs_raw[i]);
          if (second_out && second_sz > 0)
            audit_copy_cstr(second_out, second_sz, rhs);
          return 1;
        }
      }
      if (seen_count < 16 && lhs_key[0] && rhs_key[0]) {
        audit_copy_cstr(seen_lhs[seen_count], sizeof(seen_lhs[seen_count]), lhs_key);
        audit_copy_cstr(seen_rhs[seen_count], sizeof(seen_rhs[seen_count]), rhs_key);
        audit_copy_cstr(seen_rhs_raw[seen_count], sizeof(seen_rhs_raw[seen_count]), rhs);
        seen_count++;
      }
    }
    if (at_end)
      break;
    p++;
    start = p + 1;
  }
  return 0;
}

static int audit_c_empty_control_body(const char *line) {
  const char *s = lstrip_ws(line);
  if (!audit_starts_keyword(s, "if") && !audit_starts_keyword(s, "while") &&
      !audit_starts_keyword(s, "for"))
    return 0;
  const char *close = NULL;
  if (!audit_branch_condition_bounds(s, &close) || !close)
    return 0;
  close++;
  while (*close == ' ' || *close == '\t')
    close++;
  return *close == ';';
}

static const char *audit_unsafe_c_call(const char *line) {
  static const char *calls[] = {"strcpy", "strcat", "sprintf", "vsprintf", "gets", NULL};
  for (int i = 0; calls[i]; i++) {
    if (audit_line_has_plain_call(line, calls[i]))
      return calls[i];
  }
  return NULL;
}

static int audit_ny_fn_return_type(const char *sig, char *out, size_t out_sz) {
  if (!out || out_sz == 0)
    return 0;
  out[0] = '\0';
  const char *p = lstrip_ws(sig);
  if (!starts_with_word(p, "fn"))
    return 0;
  const char *open = find_outside_string(p, "(");
  const char *close = audit_find_matching_paren(open);
  if (!close)
    return 0;
  p = close + 1;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != ':')
    return 0;
  p++;
  while (*p == ' ' || *p == '\t')
    p++;
  const char *start = p;
  while (*p && !isspace((unsigned char)*p) && *p != '{' && *p != ',')
    p++;
  copy_trim_span(out, out_sz, start, p);
  return out[0] != '\0';
}

static int audit_return_type_allows_nil(const char *ret) {
  if (!ret || !*ret)
    return 1;
  return strcmp(ret, "any") == 0 || strcmp(ret, "nil") == 0 || ret[0] == '*' ||
         ret[0] == '?' ||
         strstr(ret, "nil") != NULL || strstr(ret, "optional") != NULL;
}

static int audit_condition_counter_name(const char *cond, char *out, size_t out_sz) {
  if (!out || out_sz == 0)
    return 0;
  out[0] = '\0';
  const char *p = lstrip_ws(cond);
  if (!(isalpha((unsigned char)*p) || *p == '_'))
    return 0;
  const char *start = p;
  while (isalnum((unsigned char)*p) || *p == '_')
    p++;
  const char *end = p;
  while (*p == ' ' || *p == '\t')
    p++;
  if (!(*p == '<' || *p == '>' || (*p == '!' && p[1] == '=')))
    return 0;
  copy_trim_span(out, out_sz, start, end);
  return out[0] != '\0';
}

static int audit_line_modifies_name(const char *line, const char *name) {
  if (!line || !name || !*name)
    return 0;
  char needle[160];
  const char *ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^=", NULL};
  for (int i = 0; ops[i]; i++) {
    snprintf(needle, sizeof(needle), "%s %s", name, ops[i]);
    if (find_outside_string(line, needle))
      return 1;
  }
  snprintf(needle, sizeof(needle), "%s,", name);
  const char *tuple = find_outside_string(line, needle);
  if (tuple && find_outside_string(tuple + strlen(needle), "="))
    return 1;
  snprintf(needle, sizeof(needle), "%s++", name);
  if (find_outside_string(line, needle))
    return 1;
  snprintf(needle, sizeof(needle), "%s--", name);
  if (find_outside_string(line, needle))
    return 1;
  snprintf(needle, sizeof(needle), "++%s", name);
  if (find_outside_string(line, needle))
    return 1;
  snprintf(needle, sizeof(needle), "--%s", name);
  return find_outside_string(line, needle) != NULL;
}

static int audit_ny_line_ends_expr_continuation(const char *line) {
  if (!line)
    return 0;
  const char *p = line + strlen(line);
  while (p > line && isspace((unsigned char)p[-1]))
    p--;
  if (p <= line)
    return 0;
  char ch = p[-1];
  return ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%' ||
         ch == '&' || ch == '|' || ch == '^' || ch == ',' || ch == '.' ||
         ch == '?' || ch == ':';
}

static int audit_ny_line_starts_expr_continuation(const char *line) {
  const char *s = lstrip_ws(line);
  if (!*s)
    return 0;
  return *s == '.' || *s == '+' || *s == '-' || *s == '*' || *s == '/' ||
         *s == '%' || *s == '&' || *s == '|' || *s == '^' || *s == '?' ||
         *s == ':';
}

static int audit_ny_next_line_continues_expr(StrVec *lines, size_t idx, int indent) {
  if (!lines)
    return 0;
  for (size_t j = idx + 1; j < lines->len; j++) {
    char code[4096];
    audit_code_line(lines->items[j], 1, code, sizeof(code));
    const char *s = lstrip_ws(code);
    if (!*s)
      continue;
    if (audit_line_indent(code) <= indent)
      return 0;
    return audit_ny_line_starts_expr_continuation(s);
  }
  return 0;
}

static int audit_ny_while_counter_advances(StrVec *lines, size_t idx, const char *counter) {
  int depth = 0;
  int saw_body = 0;
  for (size_t j = idx; j < lines->len; j++) {
    char code[4096];
    audit_code_line(lines->items[j], 1, code, sizeof(code));
    if (j > idx && audit_line_modifies_name(code, counter))
      return 1;
    depth += audit_line_brace_delta(code);
    if (j == idx && depth <= 0)
      return 1;
    if (j > idx)
      saw_body = 1;
    if (saw_body && depth <= 0)
      break;
  }
  return 0;
}

typedef struct {
  int depth;
  int key_count;
  char keys[64][128];
  int lines[64];
} AuditDictKeyFrame;

static int audit_line_string_dict_key(const char *line, char *key, size_t key_sz) {
  if (!key || key_sz == 0)
    return 0;
  key[0] = '\0';
  const char *s = lstrip_ws(line);
  if (*s != '"' && *s != '\'')
    return 0;
  char quote = *s++;
  const char *start = s;
  int esc = 0;
  while (*s) {
    if (esc) {
      esc = 0;
    } else if (*s == '\\') {
      esc = 1;
    } else if (*s == quote) {
      const char *end = s++;
      while (*s == ' ' || *s == '\t')
        s++;
      if (*s != ':')
        return 0;
      copy_trim_span(key, key_sz, start, end);
      return key[0] != '\0';
    }
    s++;
  }
  return 0;
}

static void audit_line_curly_counts(const char *line, int *opens, int *closes) {
  if (opens)
    *opens = 0;
  if (closes)
    *closes = 0;
  int quote = 0, esc = 0;
  for (const char *p = line ? line : ""; *p; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '{' && opens)
      (*opens)++;
    else if (ch == '}' && closes)
      (*closes)++;
  }
}

static void audit_push_bug(IssueVec *issues, AuditStats *stats, const char *path,
                           int line, const char *code, const char *sev,
                           const char *msg, const char *note) {
  issue_push(issues, path, line, 1, code, sev ? sev : "warning", msg, note);
  stats->findings++;
  stats->bug_findings++;
}

static void audit_add_duplicate_dict_key_findings(const char *path, StrVec *lines,
                                                  IssueVec *issues, AuditStats *stats) {
  AuditDictKeyFrame frames[32];
  int frame_count = 0;
  int curly_depth = 0;
  int quote_state = 0, esc_state = 0;
  memset(frames, 0, sizeof(frames));

  for (size_t i = 0; i < lines->len; i++) {
    int quote_before = quote_state;
    int starts_close = 0;
    count_brace_delta_state(lines->items[i], &starts_close, &quote_state, &esc_state);

    char code[4096];
    audit_code_line(lines->items[i], 1, code, sizeof(code));
    if (!quote_before && !quote_state) {
      char key[128];
      if (frame_count > 0 && audit_line_string_dict_key(code, key, sizeof(key))) {
        AuditDictKeyFrame *frame = &frames[frame_count - 1];
        for (int k = 0; k < frame->key_count; k++) {
          if (strcmp(frame->keys[k], key) == 0) {
            char msg[320];
            snprintf(msg, sizeof(msg), "duplicate dictionary key \"%s\" also appears on line %d",
                     key, frame->lines[k]);
            audit_push_bug(issues, stats, path, (int)i + 1, "NYAUD1113", "warning", msg,
                           "delete the earlier key or merge the values before constructing the dict");
            break;
          }
        }
        if (frame->key_count < 64) {
          snprintf(frame->keys[frame->key_count], sizeof(frame->keys[frame->key_count]),
                   "%s", key);
          frame->lines[frame->key_count] = (int)i + 1;
          frame->key_count++;
        }
      }
    }

    int opens = 0, closes = 0;
    audit_line_curly_counts(code, &opens, &closes);
    for (int k = 0; k < opens && frame_count < 32; k++) {
      AuditDictKeyFrame *frame = &frames[frame_count++];
      memset(frame, 0, sizeof(*frame));
      frame->depth = curly_depth + k + 1;
    }
    curly_depth += opens - closes;
    if (curly_depth < 0)
      curly_depth = 0;
    while (frame_count > 0 && frames[frame_count - 1].depth > curly_depth)
      frame_count--;
  }
}

typedef struct {
  char token[128];
  int line;
} AuditCaseArm;

static void audit_normalize_case_token(char *token) {
  if (!token)
    return;
  char *s = (char *)lstrip_ws(token);
  if (s != token)
    memmove(token, s, strlen(s) + 1);
  trim_trailing_ws(token);
  while (token[0] == '{' || token[0] == '}') {
    memmove(token, token + 1, strlen(token));
    s = (char *)lstrip_ws(token);
    if (s != token)
      memmove(token, s, strlen(s) + 1);
    trim_trailing_ws(token);
  }
}

static int audit_case_token_is_value(const char *token) {
  if (!token || !*token)
    return 0;
  if (strcmp(token, "_") == 0)
    return 0;
  if (starts_with_word(token, "case") || starts_with_word(token, "if") ||
      starts_with_word(token, "elif") || starts_with_word(token, "else"))
    return 0;
  return 1;
}

static void audit_add_case_token(const char *path, int ln, const char *raw,
                                 AuditCaseArm *seen, int *seen_count,
                                 IssueVec *issues, AuditStats *stats) {
  char token[128];
  copy_trim_span(token, sizeof(token), raw, NULL);
  char *guard = strstr(token, " if ");
  if (guard) {
    *guard = '\0';
    trim_trailing_ws(token);
  }
  audit_normalize_case_token(token);
  if (!audit_case_token_is_value(token))
    return;
  for (int i = 0; i < *seen_count; i++) {
    if (strcmp(seen[i].token, token) == 0) {
      char msg[320];
      snprintf(msg, sizeof(msg), "case arm '%s' already appears on line %d",
               token, seen[i].line);
      audit_push_bug(issues, stats, path, ln, "NYAUD1114", "warning", msg,
                     "case arms are first-match; remove the duplicate arm or merge the bodies");
      return;
    }
  }
  if (*seen_count < 128) {
    snprintf(seen[*seen_count].token, sizeof(seen[*seen_count].token), "%s", token);
    seen[*seen_count].line = ln;
    (*seen_count)++;
  }
}

static void audit_add_case_arm_tokens_from_line(const char *path, int ln, const char *line,
                                                AuditCaseArm *seen, int *seen_count,
                                                IssueVec *issues, AuditStats *stats) {
  const char *arrow = find_outside_string(line, "->");
  if (!arrow)
    return;
  const char *start = line;
  for (const char *p = line; p < arrow; p++) {
    if (*p == '{')
      start = p + 1;
  }
  char head[512];
  copy_trim_span(head, sizeof(head), start, arrow);
  char *guard = strstr(head, " if ");
  if (guard)
    *guard = '\0';
  const char *piece = head;
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = head;; p++) {
    char ch = *p;
    int at_end = ch == '\0';
    if (!at_end && quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (!at_end && (ch == '"' || ch == '\'')) {
      quote = ch;
      continue;
    }
    if (!at_end && is_open_delim(ch)) {
      depth++;
      continue;
    }
    if (!at_end && is_close_delim(ch)) {
      if (depth > 0)
        depth--;
      continue;
    }
    if (!at_end && !(depth == 0 && ch == ','))
      continue;
    char token[160];
    copy_trim_span(token, sizeof(token), piece, p);
    audit_add_case_token(path, ln, token, seen, seen_count, issues, stats);
    if (at_end)
      break;
    piece = p + 1;
  }
}

static void audit_add_duplicate_case_arm_findings(const char *path, StrVec *lines,
                                                  IssueVec *issues, AuditStats *stats) {
  AuditCaseArm seen[128];
  int seen_count = 0;
  int in_case = 0;
  int depth = 0;

  for (size_t i = 0; i < lines->len; i++) {
    char code[4096];
    audit_code_line(lines->items[i], 1, code, sizeof(code));
    const char *s = lstrip_ws(code);
    if (!*s)
      continue;
    if (starts_with_word(s, "case")) {
      in_case = 1;
      depth = 0;
      seen_count = 0;
    }
    if (in_case)
      audit_add_case_arm_tokens_from_line(path, (int)i + 1, s, seen, &seen_count,
                                          issues, stats);
    if (in_case) {
      depth += audit_line_brace_delta(code);
      if (depth <= 0) {
        in_case = 0;
        seen_count = 0;
      }
    }
  }
}

static int audit_literal_zero_at(const char *p) {
  while (*p == ' ' || *p == '\t')
    p++;
  int paren = 0;
  if (*p == '(') {
    paren = 1;
    p++;
    while (*p == ' ' || *p == '\t')
      p++;
  }
  if (*p == '+')
    p++;
  if (*p != '0')
    return 0;
  int saw_digit = 0;
  int nonzero = 0;
  while (isdigit((unsigned char)*p) || *p == '_' || *p == '.') {
    if (isdigit((unsigned char)*p)) {
      saw_digit = 1;
      if (*p != '0')
        nonzero = 1;
    }
    p++;
  }
  if (!saw_digit || nonzero)
    return 0;
  while (*p == ' ' || *p == '\t')
    p++;
  if (paren) {
    if (*p != ')')
      return 0;
    p++;
    while (*p == ' ' || *p == '\t')
      p++;
  }
  return !(isalnum((unsigned char)*p) || *p == '_' || *p == '.');
}

static int audit_line_literal_zero_divisor(const char *line, char *op_out,
                                           size_t op_out_sz) {
  int quote = 0, esc = 0;
  for (const char *p = line ? line : ""; *p; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch != '/' && ch != '%')
      continue;
    if ((ch == '/' && (p[1] == '/' || p[1] == '*' || p[1] == '=')) ||
        (ch == '%' && p[1] == '='))
      continue;
    if (!audit_literal_zero_at(p + 1))
      continue;
    if (op_out && op_out_sz > 0)
      snprintf(op_out, op_out_sz, "%c", ch);
    return 1;
  }
  return 0;
}

static int audit_line_negative_index(const char *line, char *idx_out, size_t idx_sz) {
  int quote = 0, esc = 0;
  for (const char *p = line ? line : ""; *p; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch != '[')
      continue;
    const char *prev = p;
    while (prev > line && isspace((unsigned char)prev[-1]))
      prev--;
    if (prev <= line)
      continue;
    unsigned char before = (unsigned char)prev[-1];
    if (isalnum(before) || before == '_') {
      const char *word = prev - 1;
      while (word > line && (isalnum((unsigned char)word[-1]) || word[-1] == '_'))
        word--;
      size_t word_len = (size_t)(prev - word);
      if (word_len == 6 && strncmp(word, "return", 6) == 0)
        continue;
    }
    if (!(isalnum(before) || before == '_' || before == ')' || before == ']'))
      continue;
    const char *q = p + 1;
    while (*q == ' ' || *q == '\t')
      q++;
    if (*q != '-' || !isdigit((unsigned char)q[1]))
      continue;
    const char *end = q + 2;
    while (isdigit((unsigned char)*end) || *end == '_')
      end++;
    if (idx_out && idx_sz > 0)
      copy_trim_span(idx_out, idx_sz, q, end);
    return 1;
  }
  return 0;
}

static const char *audit_find_plain_call_open(const char *line, const char *name) {
  if (!line || !name || !*name)
    return NULL;
  size_t name_len = strlen(name);
  int quote = 0, esc = 0;
  for (size_t i = 0; line[i]; i++) {
    char ch = line[i];
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (!(isalpha((unsigned char)ch) || ch == '_'))
      continue;
    size_t start = i;
    while (isalnum((unsigned char)line[i]) || line[i] == '_')
      i++;
    size_t end = i;
    if (end - start != name_len || memcmp(line + start, name, name_len) != 0) {
      i = end ? end - 1 : start;
      continue;
    }
    unsigned char prev = start == 0 ? 0 : (unsigned char)line[start - 1];
    if (prev == '.' || isalnum(prev) || prev == '_') {
      i = end ? end - 1 : start;
      continue;
    }
    while (line[i] == ' ' || line[i] == '\t')
      i++;
    if (line[i] == '(')
      return line + i;
    i = end ? end - 1 : start;
  }
  return NULL;
}

static int audit_call_first_arg(const char *open, char *out, size_t out_sz) {
  if (!open || *open != '(' || !out || out_sz == 0)
    return 0;
  const char *close = audit_find_matching_paren(open);
  if (!close)
    return 0;
  const char *start = open + 1;
  const char *end = close;
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = start; p < close; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (is_open_delim(ch)) {
      depth++;
      continue;
    }
    if (is_close_delim(ch)) {
      if (depth > 0)
        depth--;
      continue;
    }
    if (ch == ',' && depth == 0) {
      end = p;
      break;
    }
  }
  copy_trim_span(out, out_sz, start, end);
  return out[0] != '\0';
}

static int audit_expr_is_negative_literal(const char *expr) {
  char buf[160];
  copy_trim_span(buf, sizeof(buf), expr, NULL);
  const char *p = buf;
  if (*p == '(') {
    p++;
    while (*p == ' ' || *p == '\t')
      p++;
  }
  return *p == '-' && isdigit((unsigned char)p[1]);
}

static int audit_line_invalid_alloc_size(const char *line, char *call_out,
                                         size_t call_sz, char *arg_out,
                                         size_t arg_sz) {
  static const char *calls[] = {"malloc", "realloc", "calloc", "list", "Builder", NULL};
  for (int i = 0; calls[i]; i++) {
    const char *open = audit_find_plain_call_open(line, calls[i]);
    if (!open)
      continue;
    char arg[160];
    if (!audit_call_first_arg(open, arg, sizeof(arg)))
      continue;
    if (!audit_expr_is_negative_literal(arg))
      continue;
    if (call_out && call_sz > 0)
      snprintf(call_out, call_sz, "%s", calls[i]);
    if (arg_out && arg_sz > 0)
      snprintf(arg_out, arg_sz, "%s", arg);
    return 1;
  }
  return 0;
}

static int audit_expr_has_unclosed_delim(const char *expr) {
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = expr ? expr : ""; *p; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (is_open_delim(ch)) {
      depth++;
      continue;
    }
    if (is_close_delim(ch)) {
      if (depth > 0)
        depth--;
    }
  }
  return depth > 0;
}

static int audit_simple_write_from_line(const char *code, NySimpleAssign *out,
                                        int *is_decl_out) {
  if (is_decl_out)
    *is_decl_out = 0;
  const char *next = NULL;
  if (parse_ny_simple_assign_at(code, code, out, &next) && !next)
    return 1;
  NySimpleDecl decl;
  if (!parse_ny_simple_decl_line(code, &decl))
    return 0;
  if (is_decl_out)
    *is_decl_out = 1;
  snprintf(out->name, sizeof(out->name), "%s", decl.name);
  snprintf(out->rhs, sizeof(out->rhs), "%s", decl.rhs);
  out->indent = decl.indent;
  return 1;
}

static int audit_expr_mentions_name(const char *expr, const char *name) {
  return count_word_occurrences(expr, name) > 0;
}

static void audit_add_bug_findings(const char *path, StrVec *lines, IssueVec *issues,
                                   AuditStats *stats) {
  const int is_ny_file = nyt_ends_with(path, ".ny");
  const int is_c_file = nyt_ends_with(path, ".c") || nyt_ends_with(path, ".h");
  if (!is_ny_file && !is_c_file)
    return;

  int depth = 0;
  int fn_base_depth = -1;
  char fn_return[64] = {0};
  int nil_return_reported = 0;
  int dead_indent = -1;
  int dead_line = 0;
  int ny_quote_state = 0;
  int ny_esc_state = 0;
  NySimpleAssign last_write;
  int last_write_live = 0;
  int last_write_line = 0;
  int last_write_decl = 0;
  int previous_code_line = 0;
  memset(&last_write, 0, sizeof(last_write));

  for (size_t i = 0; i < lines->len; i++) {
    int quote_before = ny_quote_state;
    if (is_ny_file) {
      int starts_close = 0;
      count_brace_delta_state(lines->items[i], &starts_close, &ny_quote_state,
                              &ny_esc_state);
    }
    char code[4096];
    audit_code_line(lines->items[i], is_ny_file, code, sizeof(code));
    const char *s = lstrip_ws(code);
    int ln = (int)i + 1;
    int indent = audit_line_indent(code);

    if (!*s) {
      continue;
    }
    if (is_ny_file && (quote_before || ny_quote_state))
      continue;

    if (is_ny_file) {
      NySimpleAssign write;
      int write_is_decl = 0;
      int simple_write = audit_simple_write_from_line(code, &write, &write_is_decl);
      if (simple_write) {
        if (last_write_live && last_write_line == previous_code_line &&
            !last_write_decl && !write_is_decl && last_write.indent == write.indent &&
            strcmp(last_write.name, write.name) == 0 &&
            !audit_expr_has_unclosed_delim(write.rhs) &&
            !audit_expr_mentions_name(write.rhs, write.name)) {
          char msg[320];
          snprintf(msg, sizeof(msg), "'%s' is assigned again before any intervening read",
                   write.name);
          audit_push_bug(issues, stats, path, ln, "NYAUD1117", "warning", msg,
                         "delete the overwritten assignment or fold the intended value into one expression");
        }
        last_write = write;
        last_write_live = 1;
        last_write_line = ln;
        last_write_decl = write_is_decl;
      } else {
        last_write_live = 0;
        last_write_decl = 0;
      }
    }

    if (dead_line > 0) {
      if (*s == '}' || indent < dead_indent) {
        dead_line = 0;
        dead_indent = -1;
      } else if (indent >= dead_indent && !audit_starts_keyword(s, "else") &&
                 !audit_starts_keyword(s, "elif")) {
        char msg[256];
        snprintf(msg, sizeof(msg), "statement after line %d is unreachable", dead_line);
        audit_push_bug(issues, stats, path, ln, "NYAUD1106", "warning", msg,
                       "delete the dead statement or move it before return/break/continue");
        dead_line = 0;
        dead_indent = -1;
      }
    }

    char cond[512];
    if (audit_extract_branch_condition(code, cond, sizeof(cond))) {
      if (is_ny_file && audit_condition_has_assignment(cond)) {
        audit_push_bug(issues, stats, path, ln, "NYAUD1101", "warning",
                       "assignment-like '=' inside branch condition",
                       "use '==' for comparison, or lift intentional assignment before the branch");
      }
      if ((audit_starts_keyword(s, "if") || audit_starts_keyword(s, "elif")) &&
          audit_condition_is_constant_branch(cond)) {
        audit_push_bug(issues, stats, path, ln, "NYAUD1102", "note",
                       "constant branch condition",
                       "delete the dead branch or mark the condition as comptime when it is intentional");
      }
      if ((is_ny_file || is_c_file) &&
          (audit_starts_keyword(s, "elif") || find_outside_string(s, "else if"))) {
        int prev = audit_previous_branch_same_condition(lines, i, is_ny_file, indent, cond);
        if (prev > 0) {
          char msg[256];
          snprintf(msg, sizeof(msg), "branch condition repeats line %d", prev);
          audit_push_bug(issues, stats, path, ln, "NYAUD1108", "warning", msg,
                         "change the duplicate condition or merge the branch bodies");
        }
      }
      if (is_ny_file || is_c_file) {
        char dup[160], op[4];
        if (audit_condition_duplicate_logical_operand(cond, dup, sizeof(dup), op,
                                                      sizeof(op))) {
          char msg[320];
          snprintf(msg, sizeof(msg), "condition repeats operand '%s' around %s", dup, op);
          audit_push_bug(issues, stats, path, ln, "NYAUD1109", "warning", msg,
                         "delete the duplicate operand or replace it with the intended alternate check");
        }
        char lhs[160], rhs_a[160], rhs_b[160];
        if (audit_condition_self_comparison(cond, lhs, sizeof(lhs), op, sizeof(op))) {
          char msg[320];
          snprintf(msg, sizeof(msg), "condition compares '%s' with itself using %s",
                   lhs, op);
          audit_push_bug(issues, stats, path, ln, "NYAUD1110", "warning", msg,
                         "replace one side with the intended value or delete the tautology");
        } else if (audit_condition_literal_pair_conflict(cond, "&&", "==", lhs,
                                                         sizeof(lhs), rhs_a,
                                                         sizeof(rhs_a), rhs_b,
                                                         sizeof(rhs_b))) {
          char msg[640];
          snprintf(msg, sizeof(msg), "condition requires '%s' to equal both %s and %s",
                   lhs, rhs_a, rhs_b);
          audit_push_bug(issues, stats, path, ln, "NYAUD1111", "warning", msg,
                         "use || for alternatives, or collapse the condition to the reachable value");
        } else if (audit_condition_literal_pair_conflict(cond, "||", "!=", lhs,
                                                         sizeof(lhs), rhs_a,
                                                         sizeof(rhs_a), rhs_b,
                                                         sizeof(rhs_b))) {
          char msg[640];
          snprintf(msg, sizeof(msg), "condition is always true: '%s' cannot be both %s and %s",
                   lhs, rhs_a, rhs_b);
          audit_push_bug(issues, stats, path, ln, "NYAUD1112", "warning", msg,
                         "use && when rejecting multiple values, or remove the redundant branch");
        }
      }
      if (is_ny_file && audit_starts_keyword(s, "while")) {
        char counter[64];
        if (audit_condition_counter_name(cond, counter, sizeof(counter)) &&
            !audit_ny_while_counter_advances(lines, i, counter)) {
          char msg[256];
          snprintf(msg, sizeof(msg), "loop counter '%s' is not advanced in this while block",
                   counter);
          audit_push_bug(issues, stats, path, ln, "NYAUD1107", "warning", msg,
                         "advance the counter in every live path, or rewrite as for/each");
        }
      }
    }

    if (is_ny_file || is_c_file) {
      char op[4];
      if (audit_line_literal_zero_divisor(code, op, sizeof(op))) {
        char msg[256];
        snprintf(msg, sizeof(msg), "literal zero divisor after '%s'", op);
        audit_push_bug(issues, stats, path, ln, "NYAUD1115", "warning", msg,
                       "guard the denominator or remove the impossible arithmetic path");
      }
    }
    if (is_ny_file) {
      char idx[64];
      if (audit_line_negative_index(code, idx, sizeof(idx))) {
        char msg[256];
        snprintf(msg, sizeof(msg), "negative literal index %s", idx);
        audit_push_bug(issues, stats, path, ln, "NYAUD1116", "warning", msg,
                       "normalize the index before indexing, or spell the intended tail access explicitly");
      }
    }
    if (is_ny_file || is_c_file) {
      char call[64], arg[160];
      if (audit_line_invalid_alloc_size(code, call, sizeof(call), arg, sizeof(arg))) {
        char msg[320];
        snprintf(msg, sizeof(msg), "%s called with negative literal size %s", call, arg);
        audit_push_bug(issues, stats, path, ln, "NYAUD1118", "warning", msg,
                       "validate or clamp sizes before allocation/construction");
      }
    }

    if (is_c_file && audit_c_empty_control_body(code)) {
      audit_push_bug(issues, stats, path, ln, "NYAUD1103", "warning",
                     "empty control body after condition",
                     "remove the stray ';' or wrap the intended body in braces");
    }
    if (is_c_file) {
      const char *unsafe = audit_unsafe_c_call(code);
      if (unsafe) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unsafe C string helper '%s'", unsafe);
        audit_push_bug(issues, stats, path, ln, "NYAUD1104", "warning", msg,
                       "use snprintf/memcpy/strl-style helpers with explicit destination size");
      }
    }

    if (is_ny_file) {
      if (fn_base_depth < 0 && starts_with_word(s, "fn")) {
        char sig[NY_PARAM_BUF_SZ];
        ny_collect_fn_signature(lines, i, sig, sizeof(sig));
        if (audit_ny_fn_return_type(sig, fn_return, sizeof(fn_return)) &&
            !audit_return_type_allows_nil(fn_return)) {
          fn_base_depth = depth;
          nil_return_reported = 0;
        }
      }
      if (fn_base_depth >= 0 && !nil_return_reported &&
          find_outside_string(s, "return nil")) {
        char msg[256];
        snprintf(msg, sizeof(msg), "return nil in function declared as %s", fn_return);
        audit_push_bug(issues, stats, path, ln, "NYAUD1105", "warning", msg,
                       "use ': any' for nil-able APIs or return a typed fallback/error value");
        nil_return_reported = 1;
      }
    }

    int stmt_delta_starts_close = 0;
    int stmt_delta = count_brace_delta(s, &stmt_delta_starts_close);
    if (is_ny_file &&
        (audit_starts_keyword(s, "return") || audit_starts_keyword(s, "break") ||
         audit_starts_keyword(s, "continue")) &&
        !find_outside_string(s, "{") && !find_outside_string(s, "}") &&
        stmt_delta <= 0 &&
        !audit_ny_line_ends_expr_continuation(s) &&
        !audit_ny_next_line_continues_expr(lines, i, indent)) {
      dead_line = ln;
      dead_indent = indent;
    }

    depth += audit_line_brace_delta(code);
    if (fn_base_depth >= 0 && depth <= fn_base_depth) {
      fn_base_depth = -1;
      fn_return[0] = '\0';
      nil_return_reported = 0;
    }
    if (depth < 0)
      depth = 0;
    previous_code_line = ln;
  }

  if (is_ny_file) {
    audit_add_duplicate_dict_key_findings(path, lines, issues, stats);
    audit_add_duplicate_case_arm_findings(path, lines, issues, stats);
  }
}

static void audit_add_battery_findings(const char *path, StrVec *lines, IssueVec *issues,
                                       AuditStats *stats) {
  for (size_t i = 0; i < lines->len; i++) {
    const char *line = lstrip_ws(lines->items[i]);
    int ln = (int)i + 1;
    if (!*line)
      continue;
    if (audit_line_has_truthy_env(line)) {
      issue_push(issues, path, ln, 1, "NYAUD2001", "warning",
                 "manual env truthy/falsey comparison",
                 "use a shared cached env bool helper so hot code avoids repeated string normalization");
      stats->findings++;
    }
    if (find_outside_string(line, "str.lower(str.strip(env(") ||
        find_outside_string(line, "str.strip(to_str(env(")) {
      issue_push(issues, path, ln, 1, "NYAUD2002", "warning",
                 "manual env normalization",
                 "centralize env parsing in one config/helper function");
      stats->findings++;
    }
    if (find_outside_string(line, "{ return true }")) {
      int has_false_tail = 0;
      for (size_t j = i + 1; j < lines->len && j < i + 5; j++)
        has_false_tail |= strstr(lines->items[j], "return false") != NULL;
      if (has_false_tail) {
        issue_push(issues, path, ln, 1, "NYAUD2003", "note",
                   "boolean branch can collapse",
                   "return the boolean expression directly");
        stats->findings++;
      }
    }
    if (find_outside_string(line, "while(") && find_outside_string(line, "< len(")) {
      issue_push(issues, path, ln, 1, "NYAUD2004", "note",
                 "len() recomputed in while condition",
                 "cache len() before stable hot loops");
      stats->findings++;
    }
    if (find_outside_string(line, "malloc(")) {
      int guarded = 0;
      for (size_t j = i + 1; j < lines->len && j < i + 10; j++) {
        const char *near_line = lines->items[j];
        if (strstr(near_line, "defer") || strstr(near_line, "free(") ||
            strstr(near_line, "with ") || strstr(near_line, "if(!") ||
            strstr(near_line, "if (!") || strstr(near_line, "== 0"))
          guarded = 1;
      }
      if (!guarded) {
        issue_push(issues, path, ln, 1, "NYAUD2005", "warning",
                   "malloc without nearby guard/free/defer",
                   "check allocation and make ownership visible near the allocation site,"
                   " preferably with ptr: name = malloc(...) { ... }");
        stats->findings++;
      }
    }
    if (starts_with_word(line, "fn") && strstr(line, "__") && strstr(line, "(")) {
      const char *magic[] = {"__add", "__sub", "__mul", "__div", "__eq", "__lt", "__gt", "__len", NULL};
      for (int k = 0; magic[k]; k++) {
        if (strstr(line, magic[k])) {
          issue_push(issues, path, ln, 1, "NYAUD2006", "warning",
                     "magic operator hook",
                     "prefer scoped typed operator declarations over exported magic hook names");
          stats->findings++;
          break;
        }
      }
    }
  }
}

static void audit_add_ffi_findings(const char *path, StrVec *lines, IssueVec *issues, AuditStats *stats) {
  int start = -1;
  int count = 0;
  char names[180] = {0};
  for (size_t i = 0; i <= lines->len; i++) {
    const char *line = (i < lines->len) ? lstrip_ws(lines->items[i]) : "";
    int is_extern = strncmp(line, "extern fn ", 10) == 0;
    if (is_extern) {
      if (start < 0) {
        start = (int)i + 1;
        names[0] = '\0';
      }
      count++;
      const char *p = line + 10;
      char name[48];
      size_t n = 0;
      while ((isalnum((unsigned char)*p) || *p == '_') && n + 1 < sizeof(name))
        name[n++] = *p++;
      name[n] = '\0';
      audit_sample_append(names, sizeof(names), ", ", name);
      continue;
    }
    if (start >= 0 && (!*line || strncmp(line, "#include", 8) == 0 || strncmp(line, "link ", 5) == 0))
      continue;
    if (start >= 0 && count >= 2) {
      char msg[256];
      snprintf(msg, sizeof(msg), "%d adjacent extern fn declarations: %s", count, names);
      issue_push(issues, path, start, 1, "NYAUD5001", count >= 5 ? "warning" : "note",
                 msg, "use an extern block so library/symbol metadata is written once");
      stats->findings++;
    }
    start = -1;
    count = 0;
    names[0] = '\0';
  }
}

static void audit_add_dead_findings(const char *path, const char *txt, FnVec *fns,
                                    IssueVec *issues, AuditStats *stats) {
  if (!nyt_ends_with(path, ".ny"))
    return;
  for (size_t i = 0; i < fns->len; i++) {
    Fn *fn = &fns->items[i];
    if (strcmp(fn->file, path) != 0 || !fn->is_private || fn->is_method)
      continue;
    if (fn->name[0] != '_')
      continue;
    if (count_word_occurrences(txt, fn->name) <= 1) {
      issue_push(issues, path, fn->line, 1, "NYAUD7001", "note",
                 "private helper appears unused",
                 "delete it or wire it into the module; generated helpers should be marked/exported explicitly");
      stats->findings++;
    }
  }
}

static void audit_add_profile_findings(const char *path, const char *txt, StrVec *lines,
                                       IssueVec *issues, AuditStats *stats) {
  if (!nyt_ends_with(path, ".ny"))
    return;
  for (size_t i = 0; i < lines->len; i++) {
    const char *line = lines->items[i];
    const char *site = find_outside_string(line, "internal(");
    if (!site)
      continue;
    const char *p = site + 9;
    while (*p && *p != ')') {
      while (*p == ' ' || *p == '\t' || *p == ',')
        p++;
      if (!(isalpha((unsigned char)*p) || *p == '_')) {
        if (*p)
          p++;
        continue;
      }
      char name[128];
      size_t n = 0;
      while ((isalnum((unsigned char)*p) || *p == '_') && n + 1 < sizeof(name))
        name[n++] = *p++;
      name[n] = '\0';
      if (!name[0])
        continue;
      int refs = count_word_occurrences(txt, name);
      if (refs <= 2) {
        char msg[256];
        snprintf(msg, sizeof(msg), "internal helper '%s' appears unused", name);
        issue_push(issues, path, (int)i + 1, 1, "NYAUD7201", "note",
                   msg,
                   "delete it, wire it into the module, or keep it in a debug export profile if tests/tools need it");
        stats->findings++;
        stats->trim_targets++;
      }
    }
  }
}

static void audit_add_constant_findings(const char *path, const char *txt, StrVec *lines,
                                        IssueVec *issues, AuditStats *stats,
                                        const char *mode) {
  if (!nyt_ends_with(path, ".ny"))
    return;
  if (strstr(path, "/window/platform/") || strstr(path, "lib/os/ui/window/platform/"))
    return;
  if (!(audit_wants(mode, "constants") || audit_wants(mode, "trim") ||
        audit_wants(mode, "dead") || audit_wants(mode, "meta")))
    return;

  int depth = 0;
  int first_unused = 0;
  int first_key_unused = 0;
  int unused_total = 0;
  int key_total = 0;
  int key_unused = 0;
  char unused_sample[256] = {0};
  char key_sample[256] = {0};

  for (size_t i = 0; i < lines->len; i++) {
    const char *raw = lines->items[i];
    if (depth == 0) {
      char name[128];
      if (audit_parse_top_def_name(raw, name, sizeof(name)) &&
          audit_is_const_like_name(name)) {
        if (audit_is_api_const_name(name))
          continue;
        int refs = count_word_occurrences(txt, name);
        int is_key = audit_is_key_const_name(name);
        if (is_key)
          key_total++;
        if (refs <= 1) {
          unused_total++;
          if (!first_unused)
            first_unused = (int)i + 1;
          audit_sample_append(unused_sample, sizeof(unused_sample), ",", name);
          if (is_key) {
            key_unused++;
            if (!first_key_unused)
              first_key_unused = (int)i + 1;
            audit_sample_append(key_sample, sizeof(key_sample), ",", name);
          }
        }
      }
    }
    depth += audit_line_brace_delta(raw);
    if (depth < 0)
      depth = 0;
  }

  if (unused_total > 0) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%d unused top-level constant def(s): %s",
             unused_total, unused_sample[0] ? unused_sample : "(sample unavailable)");
    issue_push(issues, path, first_unused ? first_unused : 1, 1, "NYAUD7301",
               unused_total >= 8 ? "warning" : "note", msg,
               "remove dead constants or collapse literal ranges/tables; this is behavior-preserving cleanup");
    stats->findings++;
    stats->trim_targets++;
  }
  if (key_total >= 12 && key_unused >= 1) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%d/%d key constants are dead: %s",
             key_unused, key_total, key_sample[0] ? key_sample : "(sample unavailable)");
    issue_push(issues, path, first_key_unused ? first_key_unused : 1, 1, "NYAUD7302", "warning",
               msg,
               "trim only unused key constants; keep active key translation paths untouched");
    stats->findings++;
    stats->trim_targets++;
  }
}

static int audit_backend_path(const char *path) {
  return path && (strstr(path, "/window/platform/") || strstr(path, "lib/os/ui/window/platform/") ||
                  strstr(path, "/render/vk/mod.ny") || strstr(path, "/render/term.ny"));
}

static int audit_backend_contract_fn(const char *name) {
  static const char *required[] = {
      "create_basic_window", "destroy_basic_window", "poll_window_events", "set_title",
      "get_framebuffer_size", "get_window_size", "swap_buffers", "create_surface",
      "show_window", "hide_window", "create_cursor", "create_standard_cursor", "destroy_cursor",
      "poll_joysticks", "get_joystick_axes", "get_joystick_buttons", "get_gamepad_state", NULL};
  for (int i = 0; required[i]; i++) {
    if (strcmp(name, required[i]) == 0)
      return 1;
  }
  return 0;
}

static int audit_param_piece_typed(const char *a, const char *b) {
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = a; p && p < b; p++) {
    char ch = *p;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '(' || ch == '[' || ch == '{') {
      depth++;
      continue;
    }
    if (ch == ')' || ch == ']' || ch == '}') {
      if (depth > 0)
        depth--;
      continue;
    }
    if (ch == ':' && depth == 0)
      return 1;
  }
  return 0;
}

static int audit_param_piece_name(const char *a, const char *b, char *out, size_t out_sz) {
  while (a < b && isspace((unsigned char)*a))
    a++;
  while (b > a && isspace((unsigned char)b[-1]))
    b--;
  while (a < b && *a == '.')
    a++;
  const char *eq = a;
  int quote = 0, esc = 0, depth = 0;
  while (eq < b) {
    char ch = *eq;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      eq++;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      eq++;
      continue;
    }
    if (ch == '(' || ch == '[' || ch == '{') {
      depth++;
      eq++;
      continue;
    }
    if (ch == ')' || ch == ']' || ch == '}') {
      if (depth > 0)
        depth--;
      eq++;
      continue;
    }
    if (ch == '=' && depth == 0)
      break;
    eq++;
  }
  b = eq;
  while (b > a && isspace((unsigned char)b[-1]))
    b--;
  if (a >= b || !(isalpha((unsigned char)*a) || *a == '_'))
    return 0;
  size_t n = 0;
  while (a < b && (isalnum((unsigned char)*a) || *a == '_') && n + 1 < out_sz)
    out[n++] = *a++;
  out[n] = '\0';
  return n > 0;
}

static int audit_name_is_any(const char *name, const char *const *items) {
  if (!name)
    return 0;
  for (int i = 0; items[i]; i++)
    if (strcmp(name, items[i]) == 0)
      return 1;
  return 0;
}

static int audit_name_has_suffix(const char *name, const char *suffix) {
  if (!name || !suffix)
    return 0;
  size_t nn = strlen(name), sn = strlen(suffix);
  return nn >= sn && strcmp(name + nn - sn, suffix) == 0;
}

static int audit_std_math_real_fn(const char *fn_name) {
  static const char *names[] = {"sqrt", "clamp01", "sin", "cos", "tan", "asin", "acos",
                                "atan", "atan2", "exp", "log", "log2", "log10", "fmod",
                                "floor", "ceil", "round", "pow", "lerp", NULL};
  return audit_name_is_any(fn_name, names);
}

static int audit_std_math_ordered_fn(const char *fn_name) {
  static const char *names[] = {"abs", "min", "max", "clamp", "sign", "mod", NULL};
  return audit_name_is_any(fn_name, names);
}

static int audit_std_math_integer_contract_fn(const char *fn_name) {
  static const char *names[] = {"gcd", "lcm", "factorial", NULL};
  return audit_name_is_any(fn_name, names);
}

static const char *audit_suggest_param_type(const char *fn_name, const char *name,
                                            const char *fn_src, const char **reason) {
  static const char *bool_names[] = {"ok", "valid", "enabled", "done", "flag", "found", NULL};
  static const char *str_names[] = {"s", "str", "text", "source", "path", "name", "msg",
                                    "message", "line", "sep", "key", "prefix", "suffix", NULL};
  static const char *dict_names[] = {"d", "dict", "map", "cache", "table", "headers", NULL};
  static const char *list_names[] = {"xs", "ys", "items", "list", "arr", "array", "rows",
                                     "lines", "tokens", "parts", NULL};
  static const char *int_names[] = {"i", "j", "k", "n", "len", "count", "idx", "index",
                                    "size", "start", "end", "pos", "off", "offset", NULL};
  static const char *num_names[] = {"x", "y", "z", "a", "b", "lo", "hi", "min", "max",
                                    "value", "val", "num", "den", "mod", "p", "q", NULL};
  if (!name || !*name)
    return NULL;
  if (audit_std_math_real_fn(fn_name)) {
    if (reason)
      *reason = "std.math real-number contract; bigint should not flow here";
    return "f64";
  }
  if (audit_std_math_integer_contract_fn(fn_name)) {
    if (reason)
      *reason = "std.math integer contract is int|bigint; keep compiler contract until public union syntax exists";
    return NULL;
  }
  if (audit_std_math_ordered_fn(fn_name)) {
    if (reason)
      *reason = "std.math ordered numeric contract";
    return "number";
  }
  char probe[96];
  if (snprintf(probe, sizeof(probe), "is_str(%s", name) > 0 && fn_src &&
      strstr(fn_src, probe)) {
    if (reason)
      *reason = "body calls is_str on this parameter";
    return "str";
  }
  if (snprintf(probe, sizeof(probe), "is_int(%s", name) > 0 && fn_src &&
      strstr(fn_src, probe)) {
    if (reason)
      *reason = "body calls is_int on this parameter";
    return "int";
  }
  if (snprintf(probe, sizeof(probe), "is_bigint(%s", name) > 0 && fn_src &&
      strstr(fn_src, probe)) {
    if (reason)
      *reason = "body calls is_bigint on this parameter";
    return "bigint";
  }
  if (snprintf(probe, sizeof(probe), "is_number(%s", name) > 0 && fn_src &&
      strstr(fn_src, probe)) {
    if (reason)
      *reason = "body calls is_number on this parameter";
    return "number";
  }
  if (audit_name_is_any(name, bool_names) || strncmp(name, "is_", 3) == 0 ||
      strncmp(name, "has_", 4) == 0) {
    if (reason)
      *reason = "boolean-style parameter name";
    return "bool";
  }
  if (audit_name_is_any(name, str_names) || audit_name_has_suffix(name, "_str") ||
      audit_name_has_suffix(name, "_path") || audit_name_has_suffix(name, "_name")) {
    if (reason)
      *reason = "string-style parameter name";
    return "str";
  }
  if (audit_name_is_any(name, dict_names) || audit_name_has_suffix(name, "_dict") ||
      audit_name_has_suffix(name, "_map")) {
    if (reason)
      *reason = "dictionary-style parameter name";
    return "dict";
  }
  if (audit_name_is_any(name, list_names) || audit_name_has_suffix(name, "_list") ||
      audit_name_has_suffix(name, "_items")) {
    if (reason)
      *reason = "list-style parameter name";
    return "list";
  }
  if (audit_name_is_any(name, int_names) || audit_name_has_suffix(name, "_len") ||
      audit_name_has_suffix(name, "_count") || audit_name_has_suffix(name, "_idx")) {
    if (reason)
      *reason = "integer index/count-style parameter name";
    return "int";
  }
  if (audit_name_is_any(name, num_names)) {
    if (reason)
      *reason = "numeric math-style parameter name";
    return "number";
  }
  return NULL;
}

static const char *audit_suggest_return_type(const char *fn_name, const char **reason) {
  if (!fn_name || !*fn_name)
    return NULL;
  if (strncmp(fn_name, "is_", 3) == 0 || strncmp(fn_name, "has_", 4) == 0 ||
      strncmp(fn_name, "can_", 4) == 0) {
    if (reason)
      *reason = "predicate-style function name";
    return "bool";
  }
  if (audit_name_has_suffix(fn_name, "_len") || audit_name_has_suffix(fn_name, "_count") ||
      audit_name_has_suffix(fn_name, "_idx") || audit_name_has_suffix(fn_name, "_index") ||
      strcmp(fn_name, "len") == 0) {
    if (reason)
      *reason = "count/index-style function name";
    return "int";
  }
  if (audit_name_has_suffix(fn_name, "_str") || audit_name_has_suffix(fn_name, "_text") ||
      strcmp(fn_name, "to_str") == 0) {
    if (reason)
      *reason = "string-producing function name";
    return "str";
  }
  return NULL;
}

static void audit_add_type_findings_for_fn(const char *path, const Fn *fn, const char *fn_src,
                                           IssueVec *issues, AuditStats *stats) {
  if (!fn || fn->is_c || fn->type_inferred_intent)
    return;
  const char *base_sev = fn->is_private ? "note" : "warning";
  const char *start = fn->params;
  int quote = 0, esc = 0, depth = 0;
  for (const char *p = start;; p++) {
    char ch = *p;
    int at_end = ch == '\0';
    if (!at_end && quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (!at_end && (ch == '"' || ch == '\'')) {
      quote = ch;
      continue;
    }
    if (!at_end && (ch == '(' || ch == '[' || ch == '{')) {
      depth++;
      continue;
    }
    if (!at_end && (ch == ')' || ch == ']' || ch == '}')) {
      if (depth > 0)
        depth--;
      continue;
    }
    int split = at_end || (ch == ',' && depth == 0);
    if (!split)
      continue;
    const char *a = start;
    const char *b = p;
    while (a < b && isspace((unsigned char)*a))
      a++;
    while (b > a && isspace((unsigned char)b[-1]))
      b--;
    if ((size_t)(b - a) >= 3 && a[0] == '.' && a[1] == '.' && a[2] == '.') {
      if (at_end)
        break;
      start = p + 1;
      continue;
    }
    if (b > a && !audit_param_piece_typed(a, b)) {
      char pname[96];
      if (audit_param_piece_name(a, b, pname, sizeof(pname))) {
        const char *reason = NULL;
        const char *suggest = audit_suggest_param_type(fn->name, pname, fn_src, &reason);
        char msg[512];
        char note[512];
        if (suggest) {
          snprintf(msg, sizeof(msg), "%s(%s) lacks a type; suggest %s", fn->name, pname,
                   suggest);
          snprintf(note, sizeof(note), "symbol=%s param=%s type=%s confidence=high reason=%s",
                   fn->name, pname, suggest, reason ? reason : "heuristic");
          stats->type_suggestions++;
        } else {
          snprintf(msg, sizeof(msg), "%s(%s) lacks a type", fn->name, pname);
          snprintf(note, sizeof(note), "symbol=%s param=%s type=unknown confidence=low",
                   fn->name, pname);
        }
        issue_push(issues, path, fn->line, 1, "NYAUD9002",
                   suggest ? base_sev : "note", msg, note);
        stats->findings++;
      }
    }
    if (at_end)
      break;
    start = p + 1;
  }
  if (fn->missing_return) {
    const char *reason = NULL;
    const char *suggest = audit_suggest_return_type(fn->name, &reason);
    char msg[512];
    char note[512];
    if (suggest) {
      snprintf(msg, sizeof(msg), "%s.%s lacks a return type; suggest %s", path, fn->name,
               suggest);
      snprintf(note, sizeof(note), "symbol=%s return suggested_type=%s confidence=high reason=%s",
               fn->name, suggest, reason ? reason : "heuristic");
      stats->type_suggestions++;
    } else {
      snprintf(msg, sizeof(msg), "%s.%s lacks a return type", path, fn->name);
      snprintf(note, sizeof(note),
               "symbol=%s return suggested_type=unknown confidence=low reason=no stable local heuristic",
               fn->name);
    }
    issue_push(issues, path, fn->line, 1, "NYAUD9003",
               suggest ? base_sev : "note", msg, note);
    stats->findings++;
  }
}

static void audit_add_contract_findings(const char *path, StrVec *lines, IssueVec *issues,
                                        AuditStats *stats) {
  if (!nyt_ends_with(path, ".ny") || !audit_backend_path(path))
    return;
  int first = 0;
  int hits = 0;
  char sample[160] = {0};
  char seen[512] = {0};
  for (size_t i = 0; i < lines->len; i++) {
    char name[128], params[NY_PARAM_BUF_SZ];
    if (!parse_ny_fn_line(lines->items[i], name, sizeof(name), params, sizeof(params)))
      continue;
    if (!audit_backend_contract_fn(name))
      continue;
    char key[144];
    snprintf(key, sizeof(key), "|%s|", name);
    if (strstr(seen, key))
      continue;
    audit_str_append(seen, sizeof(seen), key);
    if (!first)
      first = (int)i + 1;
    hits++;
    audit_sample_append(sample, sizeof(sample), ", ", name);
  }
  if (hits < 4)
    return;
  char msg[256];
  snprintf(msg, sizeof(msg), "%d backend surface function(s): %s", hits, sample);
  issue_push(issues, path, first, 1, "NYAUD6101", hits >= 8 ? "warning" : "note",
             msg,
             "use std.os.ui.window.platform.contract masks so shared event/state code owns the contract and backend files keep native calls/tables");
  stats->findings++;
  stats->trim_targets++;
}

static int audit_fn_score(FunctionPressure *f) {
  return f->lines + f->dict_gets * 8 + f->dict_sets * 7 + f->appends * 4 + f->loops * 8 +
         f->mallocs * 12 + f->legacy_calls * 6 + f->untyped_params * 10 +
         f->missing_returns * 8 + f->eager_defaults * 14 + f->eager_ternaries * 10;
}

static int audit_file_score(FilePressure *f) {
  return f->lines / 8 + f->dict_gets * 5 + f->dict_sets * 4 + f->appends * 2 + f->env_reads * 8 +
         f->mallocs * 8 + f->legacy_calls * 5 + f->untyped_params * 8 +
         f->missing_returns * 5 + f->eager_defaults * 12 + f->eager_ternaries * 8 +
         f->repeated_score / 40;
}

static int audit_count_char(const char *s, char needle) {
  int n = 0;
  for (const char *p = s ? s : ""; *p; p++) {
    if (*p == needle)
      n++;
  }
  return n;
}

static int audit_decl_name(const char *line, char *out, size_t out_sz) {
  if (!line || !out || out_sz == 0)
    return 0;
  out[0] = '\0';
  const char *p = lstrip_ws(line);
  if (starts_with_word(p, "def"))
    p += 3;
  else if (starts_with_word(p, "mut"))
    p += 3;
  else
    return 0;
  while (*p && isspace((unsigned char)*p))
    p++;
  size_t w = 0;
  while (*p && (isalnum((unsigned char)*p) || *p == '_') && w + 1 < out_sz)
    out[w++] = *p++;
  out[w] = '\0';
  return out[0] != '\0';
}

static int audit_large_literal_decl_score(const char *line, char *name, size_t name_sz) {
  char code[2048];
  audit_code_line(line, 1, code, sizeof(code));
  const char *s = lstrip_ws(code);
  if (!audit_decl_name(s, name, name_sz))
    return 0;
  if (!find_outside_string(s, "="))
    return 0;
  int width = visual_len(line);
  int opens = audit_count_char(s, '[') + audit_count_char(s, '{');
  int commas = audit_count_char(s, ',');
  int quotes = audit_count_char(s, '"') + audit_count_char(s, '\'');
  if (width < 180 && opens < 6)
    return 0;
  if (commas < 12 && quotes < 8)
    return 0;
  return width / 60 + opens * 2 + commas / 8 + quotes / 8;
}

static int audit_get_literal_key(const char *line, char *key, size_t key_sz) {
  if (!line || !key || key_sz == 0)
    return 0;
  key[0] = '\0';
  char code[2048];
  audit_code_line(line, 1, code, sizeof(code));
  const char *p = find_outside_string(code, ".get(");
  if (!p)
    p = find_outside_string(code, "get(");
  if (!p)
    return 0;
  p = strchr(p, '(');
  if (!p)
    return 0;
  p++;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p != '"' && *p != '\'')
    return 0;
  char quote = *p++;
  size_t w = 0;
  while (*p && *p != quote && w + 1 < key_sz) {
    if (*p == '\\' && p[1])
      p++;
    key[w++] = *p++;
  }
  key[w] = '\0';
  return key[0] != '\0' && *p == quote;
}

static int audit_trivial_main_wrapper(const StrVec *lines, size_t start) {
  if (!lines || start >= lines->len)
    return 0;
  char compact[128];
  size_t w = 0;
  int depth = 0;
  int seen_open = 0;
  for (size_t j = start; j < lines->len && j < start + 12; j++) {
    char code[1024];
    audit_code_line(lines->items[j], 1, code, sizeof(code));
    const char *p = code;
    if (j == start) {
      const char *m = strstr(p, "#main");
      if (m)
        p = m + 5;
    }
    for (; *p && w + 1 < sizeof(compact); p++) {
      char ch = *p;
      if (ch == '{') {
        depth++;
        seen_open = 1;
        continue;
      }
      if (ch == '}') {
        depth--;
        continue;
      }
      if (!seen_open || isspace((unsigned char)ch))
        continue;
      compact[w++] = ch;
    }
    if (seen_open && depth <= 0)
      break;
  }
  compact[w] = '\0';
  return strcmp(compact, "main()") == 0 || strcmp(compact, "returnmain()") == 0;
}

static int audit_code_number(const char *code);
static int issue_is_audit(const Issue *it);

static int audit_accept_token(const char *s, const char *token) {
  if (!s || !token || !*token)
    return 0;
  size_t n = strlen(token);
  for (const char *p = strstr(s, token); p; p = strstr(p + n, token)) {
    unsigned char before = p == s ? 0 : (unsigned char)p[-1];
    unsigned char after = (unsigned char)p[n];
    int left_ok = p == s || !(isalnum(before) || before == '_' || before == '-');
    int right_ok = after == 0 || !(isalnum(after) || after == '_' || after == '-');
    if (left_ok && right_ok)
      return 1;
  }
  return 0;
}

static int audit_accept_line_matches(const char *line, const char *code, int file_only) {
  const char *p = strstr(line ? line : "", "ny-fmt:");
  if (!p)
    p = strstr(line ? line : "", "nyfmt:");
  if (!p)
    return 0;
  int accepts = audit_accept_token(p, "accept") || audit_accept_token(p, "accept-file") ||
                audit_accept_token(p, "allow") || audit_accept_token(p, "ignore");
  if (!accepts)
    return 0;
  if (file_only && !audit_accept_token(p, "accept-file"))
    return 0;
  if (audit_accept_token(p, "all") || (code && audit_accept_token(p, code)))
    return 1;
  int n = audit_code_number(code);
  if (n > 0) {
    char short_code[16];
    snprintf(short_code, sizeof(short_code), "%d", n);
    if (audit_accept_token(p, short_code))
      return 1;
  }
  return 0;
}

static int audit_issue_is_accepted(const StrVec *lines, const Issue *it) {
  if (!lines || !it)
    return 0;
  int idx = it->line - 1;
  int first = idx - 2;
  int last = idx;
  if (first < 0)
    first = 0;
  if (last >= (int)lines->len)
    last = (int)lines->len - 1;
  for (int i = first; i <= last; i++) {
    if (audit_accept_line_matches(lines->items[i], it->code, 0))
      return 1;
  }
  for (size_t i = 0; i < lines->len; i++) {
    if (audit_accept_line_matches(lines->items[i], it->code, 1))
      return 1;
  }
  return 0;
}

static void audit_stats_accept(AuditStats *stats, const Issue *it) {
  if (!stats || !it)
    return;
  stats->accepted_findings++;
  if (stats->findings > 0)
    stats->findings--;
  int n = audit_code_number(it->code);
  if (n >= 1100 && n < 1200 && stats->bug_findings > 0)
    stats->bug_findings--;
  if (((n >= 3000 && n < 5000) || n == 8102) && stats->trim_targets > 0)
    stats->trim_targets--;
  if (n == 1001 && stats->hot_files > 0)
    stats->hot_files--;
  if (n == 1002 && stats->hot_functions > 0)
    stats->hot_functions--;
  if ((n == 9002 || n == 9003) && stats->type_suggestions > 0)
    stats->type_suggestions--;
  if (n == 3004 && stats->append_builders > 0)
    stats->append_builders--;
  if (n == 3005 && stats->repeated_get_shapes > 0)
    stats->repeated_get_shapes--;
  if (n == 3006 && stats->literal_tables > 0)
    stats->literal_tables--;
  if (n == 4902 && stats->trivial_main_wrappers > 0)
    stats->trivial_main_wrappers--;
}

static void audit_apply_accepts(IssueVec *issues, AuditStats *stats,
                                const StrVec *lines, size_t start) {
  if (!issues || !lines || start >= issues->len)
    return;
  size_t w = start;
  for (size_t r = start; r < issues->len; r++) {
    if (issue_is_audit(&issues->items[r]) && audit_issue_is_accepted(lines, &issues->items[r])) {
      audit_stats_accept(stats, &issues->items[r]);
      continue;
    }
    if (w != r)
      issues->items[w] = issues->items[r];
    w++;
  }
  issues->len = w;
}

static int audit_find_or_add_slot(char *items, size_t width, int *count, int cap,
                                  const char *value, int *firsts, int line) {
  for (int i = 0; i < *count; i++) {
    if (strcmp(items + (size_t)i * width, value) == 0)
      return i;
  }
  if (*count >= cap)
    return -1;
  int slot = (*count)++;
  snprintf(items + (size_t)slot * width, width, "%s", value);
  if (firsts)
    firsts[slot] = line;
  return slot;
}

static int audit_indent_cols(const char *line) {
  int n = 0;
  for (const char *p = line; p && *p; p++) {
    if (*p == ' ')
      n++;
    else if (*p == '\t')
      n += 3;
    else
      break;
  }
  return n;
}

static size_t audit_next_nonblank_line(StrVec *lines, size_t from) {
  for (size_t i = from; i < lines->len; i++) {
    const char *s = lstrip_ws(lines->items[i]);
    if (*s)
      return i;
  }
  return lines->len;
}

static void audit_add_continue_findings(const char *path, StrVec *lines, IssueVec *issues, AuditStats *stats) {
  if (!nyt_ends_with(path, ".ny") && !nyt_ends_with(path, ".c") && !nyt_ends_with(path, ".h"))
    return;
  for (size_t i = 0; i < lines->len; i++) {
    const char *line = lines->items[i];
    const char *s = lstrip_ws(line);
    if (!starts_with_word(s, "for") && !starts_with_word(s, "while"))
      continue;
    int loop_indent = audit_indent_cols(line);
    size_t j = audit_next_nonblank_line(lines, i + 1);
    if (j >= lines->len)
      continue;
    const char *jline = lines->items[j];
    const char *js = lstrip_ws(jline);
    if (!starts_with_word(js, "if"))
      continue;
    int if_indent = audit_indent_cols(jline);
    if (if_indent <= loop_indent)
      continue;
    int scope = 0;
    int if_lines = 0;
    size_t close = j;
    for (size_t k = j; k < lines->len; k++) {
      const char *ks = lstrip_ws(lines->items[k]);
      if (!*ks)
        continue;
      scope += audit_count_char(ks, '{') - audit_count_char(ks, '}');
      if_lines++;
      close = k;
      if (k > j && scope <= 0)
        break;
    }
    if (if_lines < 4)
      continue;
    size_t after = audit_next_nonblank_line(lines, close + 1);
    int has_else = 0;
    int if_body_is_loop_body = 0;
    int followed_by_same_level_work = 0;
    if (after < lines->len) {
      const char *as = lstrip_ws(lines->items[after]);
      int after_indent = audit_indent_cols(lines->items[after]);
      has_else = starts_with_word(as, "else") || starts_with_word(as, "elif");
      if_body_is_loop_body = !has_else && after_indent <= loop_indent;
      followed_by_same_level_work = !has_else && after_indent == if_indent;
    } else {
      if_body_is_loop_body = 1;
    }
    if (has_else)
      continue;
    if (if_body_is_loop_body) {
      issue_push(issues, path, (int)j + 1, 1, "NYAUD3007", "note",
                 "nested if block takes up the loop body",
                 "invert the condition and use early 'continue' so the loop body stays flat and easier to scan");
      stats->findings++;
      stats->trim_targets++;
    } else if (followed_by_same_level_work && if_lines >= 6) {
      issue_push(issues, path, (int)j + 1, 1, "NYAUD3008", "note",
                 "large leading if inside loop",
                 "consider a guard 'if !condition { continue }' before the heavy body to reduce indentation and branch work");
      stats->findings++;
      stats->trim_targets++;
    }
  }
}

static void audit_scan_file(const char *path, FnVec *fns, FilePressureVec *files,
                            FunctionPressureVec *functions, IssueVec *issues, AuditStats *stats,
                            const char *mode) {
  size_t n = 0;
  char *txt = ny_read_file_raw(path, &n);
  if (!txt)
    return;
  StrVec lines = {0};
  split_lines_keep_empty(txt, &lines);
  size_t issue_start = issues->len;

  FilePressure fp;
  memset(&fp, 0, sizeof(fp));
  snprintf(fp.file, sizeof(fp.file), "%s", path);
  fp.bytes = (int)n;
  fp.lines = (int)lines.len;

  StrVec seen = {0};
  const int debug_legacy = getenv("NYFMT_DEBUG_LEGACY") != NULL;
  char case_selectors[32][64] = {{0}};
  int case_scores[32] = {0};
  int case_firsts[32] = {0};
  int case_selector_count = 0;
  int indexed_set_helpers = 0;
  int indexed_set_first = 0;
  int receiver_rewrites = 0;
  int receiver_rewrite_first = 0;
  int literal_table_score = 0;
  int literal_table_first = 0;
  char literal_table_name[128] = {0};
  char get_keys[64][80] = {{0}};
  int get_counts[64] = {0};
  int get_firsts[64] = {0};
  int get_key_count = 0;
  int get_literal_total = 0;
  for (size_t i = 0; i < lines.len; i++) {
    const char *line = lines.items[i];
    const char *s = lstrip_ws(line);
    if (*s)
      fp.nonblank++;
    int vl = visual_len(line);
    if (vl > fp.max_line)
      fp.max_line = vl;
    fp.dict_gets += audit_count_named_call(line, "dict_get", 1, 0) +
                    audit_count_named_call(line, "get", 1, 0);
    fp.dict_sets += audit_count_named_call(line, "dict_set", 1, 0) +
                    audit_count_named_call(line, "set", 1, 0);
    fp.appends += audit_count_named_call(line, "append", 1, 0);
    fp.env_reads += count_substr(line, "env(");
    fp.mallocs += count_substr(line, "malloc(");
    int receiver_hits = audit_line_receiver_rewrite_calls(line);
    if (receiver_hits > 0 && !receiver_rewrite_first)
      receiver_rewrite_first = (int)i + 1;
    receiver_rewrites += receiver_hits;
    int legacy_hits = audit_line_legacy_calls(line) + receiver_hits;
    if (debug_legacy && legacy_hits > 0)
      fprintf(stderr, "[legacy] %s:%zu: %s\n", path, i + 1, line);
    fp.legacy_calls += legacy_hits;
    fp.eager_defaults += audit_line_eager_default(line);
    fp.eager_ternaries += (strstr(line, " ? ") && strstr(line, " : ")) ? 1 : 0;
    if (nyt_ends_with(path, ".ny")) {
      if (starts_with_word(s, "#main") && audit_trivial_main_wrapper(&lines, i)) {
        issue_push(issues, path, (int)i + 1, 1, "NYAUD4902", "note",
                   "trivial #main wrapper",
                   "write the body directly in #main or keep main() only when external callers also use it");
        stats->findings++;
        stats->trim_targets++;
        stats->trivial_main_wrappers++;
      }
      char literal_name[128];
      int literal_score = audit_large_literal_decl_score(line, literal_name, sizeof(literal_name));
      if (literal_score > 0) {
        literal_table_score += literal_score;
        if (!literal_table_first) {
          literal_table_first = (int)i + 1;
          snprintf(literal_table_name, sizeof(literal_table_name), "%s", literal_name);
        }
      }
      char get_key[80];
      if (audit_get_literal_key(line, get_key, sizeof(get_key))) {
        get_literal_total++;
        int slot = audit_find_or_add_slot((char *)get_keys, sizeof(get_keys[0]),
                                          &get_key_count, 64, get_key,
                                          get_firsts, (int)i + 1);
        if (slot >= 0)
          get_counts[slot]++;
      }
      char selector[64];
      int case_score = audit_line_case_dispatch_score(line, selector, sizeof(selector));
      if (case_score > 0 && selector[0]) {
        int found = audit_find_or_add_slot((char *)case_selectors,
                                           sizeof(case_selectors[0]),
                                           &case_selector_count, 32, selector,
                                           case_firsts, (int)i + 1);
        if (found >= 0)
          case_scores[found] += case_score;
      }
      int indexed_writes = audit_count_named_call(line, "set_idx", 1, 0) +
                           audit_count_named_call(line, "store_item", 1, 0) +
                           audit_count_named_call(line, "__store_item", 1, 0);
      if (indexed_writes > 0) {
        if (!indexed_set_first)
          indexed_set_first = (int)i + 1;
        indexed_set_helpers += indexed_writes;
      }
    }
    if (audit_repeated_line(&seen, line))
      fp.repeated_lines++;
  }
  fp.repeated_score = fp.repeated_lines * 80;

  for (size_t i = 0; i < fns->len; i++) {
    Fn *fn = &fns->items[i];
    if (strcmp(fn->file, path) != 0)
      continue;
    int effective_untyped_params = fn->type_inferred_intent ? 0 : fn->untyped_params;
    int effective_missing_return = fn->type_inferred_intent ? 0 : fn->missing_return;
    fp.functions++;
    fp.untyped_params += effective_untyped_params;
    fp.missing_returns += effective_missing_return;
    int start = fn->line < 1 ? 1 : fn->line;
    int end = fn->end_line < start ? start : fn->end_line;
    if (end > (int)lines.len)
      end = (int)lines.len;
    FunctionPressure f;
    memset(&f, 0, sizeof(f));
    snprintf(f.file, sizeof(f.file), "%s", path);
    snprintf(f.name, sizeof(f.name), "%s", fn->name);
    f.line = start;
    f.end_line = end;
    f.lines = end - start + 1;
    f.untyped_params = effective_untyped_params;
    f.missing_returns = effective_missing_return;
    if (f.lines > fp.largest_fn)
      fp.largest_fn = f.lines;
    for (int ln = start; ln <= end; ln++) {
      const char *line = lines.items[(size_t)ln - 1];
      f.bytes += (int)strlen(line) + 1;
      f.dict_gets += audit_count_named_call(line, "dict_get", 1, 0) +
                     audit_count_named_call(line, "get", 1, 0);
      f.dict_sets += audit_count_named_call(line, "dict_set", 1, 0) +
                     audit_count_named_call(line, "set", 1, 0);
      f.appends += audit_count_named_call(line, "append", 1, 0);
      f.loops += count_substr(line, "while(") + count_substr(line, "for(");
      f.mallocs += count_substr(line, "malloc(");
      f.legacy_calls += audit_line_legacy_calls(line) + audit_line_receiver_rewrite_calls(line);
      f.eager_defaults += audit_line_eager_default(line);
      f.eager_ternaries += (strstr(line, " ? ") && strstr(line, " : ")) ? 1 : 0;
    }
    f.score = audit_fn_score(&f);
    if (f.lines >= 50 || f.score >= 110)
      fpress_push(functions, &f);
    if (audit_wants(mode, "types") && !fn->type_inferred_intent &&
        (fn->untyped_params > 0 || fn->missing_return)) {
      char *fn_src = dupe_join_lines(&lines, (size_t)start - 1, (size_t)end - 1);
      audit_add_type_findings_for_fn(path, fn, fn_src, issues, stats);
      free(fn_src);
    }
    if (audit_wants(mode, "bloat") && (f.lines >= 160 || f.score >= 260)) {
      issue_push(issues, path, start, 1, "NYAUD1002", "warning",
                 "hot bloated function",
                 "split parsing/state extraction away from the per-frame or inner-loop path");
      stats->findings++;
      stats->hot_functions++;
    }
    if (audit_wants(mode, "trim") && f.appends >= 12 && f.loops > 0) {
      char msg[256];
      snprintf(msg, sizeof(msg), "%s appends %d time(s) across %d loop marker(s)",
               f.name, f.appends, f.loops);
      issue_push(issues, path, start, 1, "NYAUD3004", f.appends >= 32 ? "warning" : "note",
                 msg,
                 "when result length is known, allocate list(n) and fill x[i]; otherwise isolate the builder helper");
      stats->findings++;
      stats->trim_targets++;
      stats->append_builders++;
    }
  }

  fp.score = audit_file_score(&fp);
  fpv_push(files, &fp);
  stats->files++;
  stats->repeated_lines += fp.repeated_lines;
  stats->legacy_calls += fp.legacy_calls;
  stats->receiver_rewrites += receiver_rewrites;
  stats->untyped_params += fp.untyped_params;
  stats->missing_returns += fp.missing_returns;
  if (audit_wants(mode, "bloat") && (fp.score >= 280 || fp.lines >= 2000)) {
    issue_push(issues, path, 1, 1, "NYAUD1001", "warning",
               "high refactor pressure file",
               "ranked by size, repeated code, dict lookups, env reads, mallocs, and long functions");
    stats->findings++;
    stats->hot_files++;
  }
  if (audit_wants(mode, "bugs"))
    audit_add_bug_findings(path, &lines, issues, stats);
  if (audit_wants(mode, "batteries"))
    audit_add_battery_findings(path, &lines, issues, stats);
  if (audit_wants(mode, "ffi"))
    audit_add_ffi_findings(path, &lines, issues, stats);
  if (audit_wants(mode, "dead"))
    audit_add_dead_findings(path, txt, fns, issues, stats);
  if (audit_wants(mode, "profiles"))
    audit_add_profile_findings(path, txt, &lines, issues, stats);
  audit_add_constant_findings(path, txt, &lines, issues, stats, mode);
  if (audit_wants(mode, "contracts"))
    audit_add_contract_findings(path, &lines, issues, stats);
  if (audit_wants(mode, "specialize"))
    audit_add_specialize_findings(path, &lines, issues, stats);
  if (audit_wants(mode, "meta"))
    audit_add_metaprog_findings(path, txt, fns, &lines, issues, stats);
  if (audit_wants(mode, "methods"))
    audit_add_method_syntax_findings(path, &lines, issues, stats);
  if (audit_wants(mode, "trim") || audit_wants(mode, "legacy") || audit_wants(mode, "methods"))
    audit_add_variadic_free_findings(path, &lines, issues, stats);
  if (audit_wants(mode, "layout") && (fp.dict_gets >= 40 || fp.dict_sets >= 20)) {
    issue_push(issues, path, 1, 1, "NYAUD6001",
               (fp.dict_gets >= 160 || fp.dict_sets >= 80) ? "warning" : "note",
               "layout/codegen candidate",
               "many repeated dict accesses; consider typed records, slab accessors, or generated field unpacking");
    stats->findings++;
  }
  if (audit_wants(mode, "trim") && (fp.score >= 180 || fp.repeated_lines >= 20)) {
    issue_push(issues, path, 1, 1, "NYAUD3001", "note",
               "trim target",
               "candidate for generated layout accessors, typed records, table-driven dispatch, or helper extraction");
    stats->findings++;
    stats->trim_targets++;
  }
  int case_dispatch_score = 0;
  int case_dispatch_first = 0;
  for (int si = 0; si < case_selector_count; si++) {
    if (case_scores[si] > case_dispatch_score) {
      case_dispatch_score = case_scores[si];
      case_dispatch_first = case_firsts[si];
    }
  }
  if (audit_wants(mode, "trim") && case_dispatch_score >= 5) {
    issue_push(issues, path, case_dispatch_first, 1, "NYAUD3002", "note",
               "if/elif dispatch can become case ranges",
               "use case with comma/range arms, then promote stable keymaps/token/enum maps"
               " to comptime table plus comptime match to avoid runtime setup");
    stats->findings++;
    stats->trim_targets++;
  }
  if (audit_wants(mode, "trim") || audit_wants(mode, "loops")) {
    audit_add_continue_findings(path, &lines, issues, stats);
  }
  if (audit_wants(mode, "trim") && indexed_set_helpers > 0) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%d helper indexed write(s)", indexed_set_helpers);
    issue_push(issues, path, indexed_set_first, 1, "NYAUD3003", "note",
               msg, "prefer compact indexed assignment: x[i] = value");
    stats->findings++;
    stats->trim_targets++;
  }
  if (audit_wants(mode, "trim") && literal_table_score >= 8) {
    char msg[256];
    snprintf(msg, sizeof(msg), "dense literal table%s%s%s",
             literal_table_name[0] ? " '" : "",
             literal_table_name[0] ? literal_table_name : "",
             literal_table_name[0] ? "'" : "");
    issue_push(issues, path, literal_table_first ? literal_table_first : 1, 1,
               "NYAUD3006", literal_table_score >= 24 ? "warning" : "note", msg,
               "move large static data to a small data module/asset"
               " or generate rows from compact specs; runtime modules should keep behavior readable");
    stats->findings++;
    stats->trim_targets++;
    stats->literal_tables++;
  }
  if (audit_wants(mode, "trim") && get_literal_total >= 12) {
    int best = -1;
    for (int gi = 0; gi < get_key_count; gi++) {
      if (best < 0 || get_counts[gi] > get_counts[best])
        best = gi;
    }
    if (best >= 0 && get_counts[best] >= 4) {
      char msg[256];
      snprintf(msg, sizeof(msg), "%d literal-key get() call(s); most repeated key '%s' appears %d time(s)",
               get_literal_total, get_keys[best], get_counts[best]);
      issue_push(issues, path, get_firsts[best] ? get_firsts[best] : 1, 1,
                 "NYAUD3005", get_literal_total >= 32 ? "warning" : "note", msg,
                 "promote repeated dict shapes to layout guards/records or unpack once at the boundary");
      stats->findings++;
      stats->trim_targets++;
      stats->repeated_get_shapes++;
    }
  }
  if ((audit_wants(mode, "legacy") || audit_wants(mode, "methods")) && receiver_rewrites > 0) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%d receiver-method rewrite candidate(s)", receiver_rewrites);
    issue_push(issues, path, receiver_rewrite_first ? receiver_rewrite_first : 1, 1,
               "NYAUD8102", receiver_rewrites >= 12 ? "warning" : "note", msg,
               "prefer s.to_bytes, h.unhex, b.hex, b.text, b.long,"
               " a.concat(b), and n.bytes over legacy byte helper calls");
    stats->findings++;
    stats->trim_targets++;
  }
  if (audit_wants(mode, "legacy") && fp.legacy_calls > 0) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%d legacy free helper call(s)", fp.legacy_calls);
    issue_push(issues, path, 1, 1, "NYAUD8001", fp.legacy_calls >= 20 ? "warning" : "note",
               msg, "prefer receiver methods/properties such as d.get(k), b.hex, x.len,"
               " and indexed assignment x[i] = value");
    stats->findings++;
  }
  if (audit_wants(mode, "types") && (fp.untyped_params > 0 || fp.missing_returns > 0)) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%d untyped param(s), %d missing return annotation(s)",
             fp.untyped_params, fp.missing_returns);
    issue_push(issues, path, 1, 1, "NYAUD9001",
               (fp.untyped_params + fp.missing_returns) >= 40 ? "warning" : "note",
               msg, "add type-first params/returns so attached method lookup,"
               " property syntax, and codegen specialization stay precise");
    stats->findings++;
  }

  audit_apply_accepts(issues, stats, &lines, issue_start);
  sv_free(&seen);
  sv_free(&lines);
  free(txt);
}

static int audit_skip_call_name(const char *name) {
  static const char *skip[] = {
      "if", "elif", "while", "for", "match", "return", "sizeof", "defined", "fn",
      "def", "mut", "comptime", "use", "module", "extern", NULL};
  for (int i = 0; skip[i]; i++)
    if (strcmp(name, skip[i]) == 0)
      return 1;
  return 0;
}

static void audit_collect_calls_line(StrVec *out, const char *line) {
  int quote = 0, esc = 0;
  for (size_t i = 0; line && line[i]; i++) {
    char ch = line[i];
    if (!quote && ch == '/' && line[i + 1] == '/')
      break;
    if (!quote && ch == ';')
      break;
    if (quote) {
      if (esc)
        esc = 0;
      else if (ch == '\\')
        esc = 1;
      else if (ch == quote)
        quote = 0;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (!(isalpha((unsigned char)ch) || ch == '_'))
      continue;
    size_t start = i;
    i++;
    while (isalnum((unsigned char)line[i]) || line[i] == '_' || line[i] == '.')
      i++;
    size_t end = i;
    while (line[i] == ' ' || line[i] == '\t')
      i++;
    if (line[i] != '(' || end <= start)
      continue;
    char name[80];
    size_t n = end - start;
    if (n >= sizeof(name))
      n = sizeof(name) - 1;
    memcpy(name, line + start, n);
    name[n] = '\0';
    const char *dot = strrchr(name, '.');
    const char *base = dot ? dot + 1 : name;
    if (!audit_skip_call_name(base))
      sv_push(out, base);
  }
}

static void audit_scan_calls_file(const char *path, FnVec *fns, CallSeqVec *calls) {
  size_t n = 0;
  char *txt = ny_read_file_raw(path, &n);
  if (!txt)
    return;
  StrVec lines = {0};
  split_lines_keep_empty(txt, &lines);
  for (size_t i = 0; i < fns->len; i++) {
    Fn *fn = &fns->items[i];
    if (strcmp(fn->file, path) != 0)
      continue;
    int start = fn->line < 1 ? 1 : fn->line;
    int end = fn->end_line < start ? start : fn->end_line;
    if (end > (int)lines.len)
      end = (int)lines.len;
    StrVec names = {0};
    for (int ln = start; ln <= end; ln++)
      audit_collect_calls_line(&names, lines.items[(size_t)ln - 1]);
    for (size_t j = 0; j + 2 < names.len; j++) {
      char seq[192];
      snprintf(seq, sizeof(seq), "%s > %s > %s", names.items[j], names.items[j + 1], names.items[j + 2]);
      callseq_push(calls, seq, path, start);
    }
    sv_free(&names);
  }
  sv_free(&lines);
  free(txt);
}

static int cmp_file_score(const void *a, const void *b) {
  const FilePressure *aa = (const FilePressure *)a;
  const FilePressure *bb = (const FilePressure *)b;
  if (bb->score != aa->score)
    return bb->score - aa->score;
  return bb->lines - aa->lines;
}

static int cmp_file_size(const void *a, const void *b) {
  const FilePressure *aa = (const FilePressure *)a;
  const FilePressure *bb = (const FilePressure *)b;
  return bb->bytes - aa->bytes;
}

static int cmp_file_types(const void *a, const void *b) {
  const FilePressure *aa = (const FilePressure *)a;
  const FilePressure *bb = (const FilePressure *)b;
  int as = aa->untyped_params * 2 + aa->missing_returns;
  int bs = bb->untyped_params * 2 + bb->missing_returns;
  if (bs != as)
    return bs - as;
  return bb->lines - aa->lines;
}

static int cmp_fn_score(const void *a, const void *b) {
  const FunctionPressure *aa = (const FunctionPressure *)a;
  const FunctionPressure *bb = (const FunctionPressure *)b;
  if (bb->score != aa->score)
    return bb->score - aa->score;
  return bb->lines - aa->lines;
}

static int cmp_fn_lines(const void *a, const void *b) {
  const FunctionPressure *aa = (const FunctionPressure *)a;
  const FunctionPressure *bb = (const FunctionPressure *)b;
  return bb->lines - aa->lines;
}

static int cmp_fn_types(const void *a, const void *b) {
  const FunctionPressure *aa = (const FunctionPressure *)a;
  const FunctionPressure *bb = (const FunctionPressure *)b;
  int as = aa->untyped_params * 2 + aa->missing_returns;
  int bs = bb->untyped_params * 2 + bb->missing_returns;
  if (bs != as)
    return bs - as;
  return bb->lines - aa->lines;
}

static int cmp_callseq_count(const void *a, const void *b) {
  const CallSeq *aa = (const CallSeq *)a;
  const CallSeq *bb = (const CallSeq *)b;
  if (bb->count != aa->count)
    return bb->count - aa->count;
  return strcmp(aa->seq, bb->seq);
}

static void print_file_pressure(const FilePressureVec *v, int limit, int by_size) {
  size_t n = v->len;
  if (limit > 0 && n > (size_t)limit)
    n = (size_t)limit;
  printf("%s%-6s %-6s %-6s %-5s %-5s %-5s %-5s %-5s %-5s %-5s %-6s %s%s\n",
         nyt_clr(NYT_GRAY), by_size ? "bytes" : "score", "lines", "kb", "get", "set", "app", "env",
         "old", "typ", "ret", "repeat",
         "file", nyt_clr(NYT_RESET));
  for (size_t i = 0; i < n; i++) {
    const FilePressure *f = &v->items[i];
    printf("%-6d %-6d %-6d %-5d %-5d %-5d %-5d %-5d %-5d %-5d %-6d %s\n",
           by_size ? f->bytes : f->score, f->lines, f->bytes / 1024, f->dict_gets, f->dict_sets,
           f->appends, f->env_reads, f->legacy_calls, f->untyped_params, f->missing_returns,
           f->repeated_lines, f->file);
  }
}

static void print_debloat_priorities(const AuditStats *s) {
  const char *gray = nyt_clr(NYT_GRAY);
  const char *rs = nyt_clr(NYT_RESET);
  nyt_subheading("Debloat Priorities");
  if (s->receiver_rewrites > 0)
    printf("  1. Replace %d bytes/bigint helper call(s) with receiver methods: "
           "s.to_bytes, b.hex, h.unhex, b.text, n.bytes.\n",
           s->receiver_rewrites);
  if (s->legacy_calls > 0)
    printf("  2. Replace %d generic helper call(s) with methods/indexing: "
           "d.get(k), d.set(k,v), x[i] = v, xs.append(v).\n",
           s->legacy_calls);
  if (s->untyped_params || s->missing_returns)
    printf("  3. Add %d param type(s) and %d return annotation(s) so method/property lookup "
           "and specialization stay cheap.\n",
           s->untyped_params, s->missing_returns);
  if (s->repeated_lines > 0)
    printf("  4. Remove repeated source by lifting tables/templates; repeated line hits: %d.\n",
           s->repeated_lines);
  if (s->literal_tables > 0)
    printf("  5. Review %d dense literal table(s): move static data out"
           " or generate it when that keeps behavior clearer.\n",
           s->literal_tables);
  if (s->append_builders > 0)
    printf("  6. Review %d append-built loop result(s): preallocate only when size is known and clarity improves.\n",
           s->append_builders);
  if (s->repeated_get_shapes > 0)
    printf("  7. Review %d repeated literal-key dict shape(s): use typed layouts or one boundary unpack when the shape is stable.\n",
           s->repeated_get_shapes);
  if (s->trivial_main_wrappers > 0)
    printf("  8. Review %d trivial #main wrapper(s): keep them only when main() is intentionally public API.\n",
           s->trivial_main_wrappers);
  if (s->receiver_rewrites == 0 && s->legacy_calls == 0 && s->untyped_params == 0 &&
      s->missing_returns == 0 && s->repeated_lines == 0 && s->literal_tables == 0 &&
      s->append_builders == 0 && s->repeated_get_shapes == 0 && s->trivial_main_wrappers == 0)
    printf("  %sNo obvious debloat priority in this scan.%s\n", gray, rs);
}

static void print_callseqs(const CallSeqVec *v, int limit) {
  size_t shown = 0;
  printf("%s%-6s %-5s %s%s\n", nyt_clr(NYT_GRAY), "count", "line", "sequence", nyt_clr(NYT_RESET));
  for (size_t i = 0; i < v->len; i++) {
    const CallSeq *c = &v->items[i];
    if (c->count <= 1)
      continue;
    if (limit > 0 && shown >= (size_t)limit)
      break;
    printf("%-6d %-5d %s  %s%s%s\n", c->count, c->line, c->seq, nyt_clr(NYT_GRAY), c->file,
           nyt_clr(NYT_RESET));
    shown++;
  }
}

static int issue_is_audit(const Issue *it) {
  return it && strncmp(it->code, "NYAUD", 5) == 0;
}

typedef struct {
  int code;
  const char *rewrite;
  const char *syntax;
} AuditHint;

static const AuditHint k_audit_hints[] = {
    {1001, "split-file", "split module Foo(...); keep public exports in mod.ny"},
    {1002, "split-function", "fn small_part(T x) R { ... }; call it from the wrapper"},
    {1101, "condition-assignment-check", "if(x == y){ ... }; lift x = y before the branch"},
    {1102, "constant-branch-check", "if(comptime{ return cond }){ ... } or delete dead branch"},
    {1103, "empty-control-body-check", "if(cond){ ... }"},
    {1104, "bounded-c-string-check", "snprintf(dst, dst_len, \"%s\", src)"},
    {1105, "nil-return-contract-check", "fn f(...) any { return nil } or return a typed fallback"},
    {1106, "unreachable-code-check", "return value; delete or move later statements before it"},
    {1107, "loop-progress-check", "while(i < n){ ...; i += 1 }"},
    {1108, "duplicate-branch-check", "elif(other_cond){ ... } or merge identical branches"},
    {1109, "duplicate-boolean-operand", "if(a && b){ ... }; remove repeated a && a / a || a"},
    {1110, "self-comparison-check", "if(x == y){ ... }; avoid x == x / x != x"},
    {1111, "impossible-equality-chain", "if(x == a || x == b){ ... } for alternatives"},
    {1112, "tautological-inequality-chain", "if(x != a && x != b){ ... } to reject both"},
    {1113, "duplicate-dict-key-check", "{\"key\": value}; keep each key once"},
    {1114, "duplicate-case-arm-check", "case x { a -> one; b -> two; _ -> fallback }"},
    {1115, "literal-zero-divisor-check", "if(den != 0){ value / den }"},
    {1116, "negative-index-check", "idx = xs.len - 1; xs[idx]"},
    {1117, "overwritten-assignment-check", "def x = one_expression"},
    {1118, "allocation-size-check", "if(n >= 0){ malloc(n) }"},
    {2001, "cached-env-bool", "def enabled = env(\"NAME\", \"\") == \"1\""},
    {2002, "centralized-env-parser", "fn env_bool(str name, bool fallback=false) bool { ... }"},
    {2003, "boolean-expression-return", "return cond"},
    {2004, "cached-loop-length", "def n = xs.len; while(i < n){ ... }"},
    {2005, "guarded-allocation", "def p = malloc(n); if(!p){ return nil }"},
    {2006, "typed-operator", "impl T { operator + T: T = add }"},
    {3001, "trim-refactor", "lift helper/table; keep leaf fn short and typed"},
    {3002, "case-range-dispatch", "case x { 0, 1 -> a; 2..9 -> b; _ -> c }"},
    {3003, "indexed-assignment", "x[i] = value"},
    {3004, "preallocated-builder", "def out = list(n); out[i] = value"},
    {3005, "layout-boundary-unpack", "layout guard Shape: row = input else { ... }; row.field"},
    {3006, "data-module-or-generated-table", "def rows = load_table(\"asset\") or comptime{ emit_rows(spec) }"},
    {3101, "comptime-case-selector", "case comptime{ return selector } { key -> value; _ -> fallback }"},
    {3102, "comptime-branch", "if(comptime{ return cond }){ ... }"},
    {3103, "comptime-table-dispatch", "comptime table Name = {...}; comptime match Name(key, fallback)"},
    {3104, "generated-layout-metadata", "comptime fields(Type) as f { emit accessor(f) }"},
    {3105, "comptime-match-keyword", "comptime match Table(value, fallback)"},
    {4101, "layout-shape-boundary", "layout shape Shape derive(load) { T: field = default }"},
    {4102, "layout-guard-boundary", "layout guard Shape: v = input else { return nil }; v.field"},
    {4103, "layout-record-constructor", "layout record Type derive(default, store) { T: field = default }"},
    {4104, "layout-record-accessors", "layout record/shape Type derive(load, store); value.field"},
    {4105, "layout-guard-boundary", "layout guard Shape: v = input else { fallback }"},
    {4201, "with-ptr-resource", "with ptr: p = alloc { ... }"},
    {4202, "with-ptr-defer-free", "with ptr: p = alloc { ... }"},
    {4203, "with-closeable-resource", "with Type: r = create { ... close(r) }"},
    {4204, "variadic-free", "free(a, b, c)"},
    {4301, "comptime-table", "def TABLE = comptime{ return {\"k\": v} }"},
    {4302, "case-or-comptime-table", "case key { \"a\" -> A; \"b\" -> B; _ -> fallback }"},
    {4401, "generated-ffi-surface", "extern \"lib\" { fn a(...); fn b(...); }"},
    {4501, "diagnostic-rule", "diag rule Name { when pattern; fix \"...\" }"},
    {4502, "audit-diagnostic-table", "static const table[] = {{code, severity, message, fix}}"},
    {4503, "diagnostic-rule", "diag rule Name { when check; fix \"...\" }"},
    {4601, "generated-backend-module", "module backend from Spec { emit fn wrapper(...) }"},
    {4701, "comptime-template-family", "for name in comptime [...] { emit make(name) }"},
    {4801, "derive-block", "layout record Type derive(default, eq, hash, debug_str) { ... }"},
    {4901, "main-guard", "#main { ... }"},
    {4902, "inline-trivial-main", "#main { body() } only when body is also a public entry"},
    {5001, "extern-block", "extern \"lib\" { fn a(...); fn b(...); }"},
    {6001, "typed-layout-accessors", "layout shape Row derive(load, store) { T: field = default }"},
    {6101, "backend-contract", "layout BackendContract pack(4){ i32: caps }"},
    {7001, "delete-or-wire-private-helper", "delete unused helper or wire it into the call path"},
    {7201, "delete-or-export-internal-helper", "export helper or keep it private and used"},
    {7301, "remove-dead-constant", "delete unused constant"},
    {7302, "remove-dead-key-constant", "delete unused key/table constant"},
    {8001, "receiver-methods", "d.get(k), b.hex, b.text, x.len, x[i] = v"},
    {8101, "property-method", "impl T { fn prop(T self) R { ... } }; value.prop"},
    {8102, "receiver-methods", "s.to_bytes, h.unhex, b.hex, b.text, b.long, a.concat(b)"},
    {9001, "typed-signature", "fn f(str s, int n=0) bool { ... }"},
    {9002, "typed-parameter", "fn f(Type arg) R { ... }"},
    {9003, "typed-return", "fn f(...) ReturnType { ... }"},
};

static int audit_code_number(const char *code) {
  if (!code || strncmp(code, "NYAUD", 5) != 0)
    return 0;
  return atoi(code + 5);
}

static const AuditHint *audit_hint_for_number(int n) {
  for (size_t i = 0; i < sizeof(k_audit_hints) / sizeof(k_audit_hints[0]); i++) {
    if (k_audit_hints[i].code == n)
      return &k_audit_hints[i];
  }
  return NULL;
}

static const char *audit_rewrite_shape_for_code(const char *code) {
  int n = audit_code_number(code);
  if (n == 0)
    return "none";
  const AuditHint *hint = audit_hint_for_number(n);
  if (hint && hint->rewrite)
    return hint->rewrite;
  if (n >= 1100 && n < 1200)
    return "bug-check";
  if (n >= 3000 && n < 4000)
    return "specialize-or-trim";
  if (n >= 4000 && n < 5000)
    return "metaprogramming";
  if (n >= 8000 && n < 9000)
    return "method-rewrite";
  if (n >= 9000 && n < 10000)
    return "type-annotation";
  return "audit-review";
}

static const char *audit_syntax_hint_for_code(const char *code) {
  int n = audit_code_number(code);
  if (n == 0)
    return "";
  const AuditHint *hint = audit_hint_for_number(n);
  if (hint && hint->syntax)
    return hint->syntax;
  if (n >= 1100 && n < 1200)
    return "prefer explicit comparison, typed nil-able return, and visible loop progress";
  if (n >= 3000 && n < 4000)
    return "case/comptime/table syntax where the selector is stable";
  if (n >= 4000 && n < 5000)
    return "layout shape/record + guard at dynamic boundaries";
  if (n >= 8000 && n < 9000)
    return "receiver syntax: value.method(args) or value.property";
  if (n >= 9000 && n < 10000)
    return "typed signature: fn f(Type arg) Return { ... }";
  return "";
}

static uint64_t audit_hash_update_cstr(uint64_t h, const char *s) {
  const char *v = s ? s : "";
  h ^= fnv1a64_mem(v, strlen(v));
  h *= 1099511628211ULL;
  return h;
}

static uint64_t audit_issue_hash(const Issue *it, const char *shape) {
  char loc[48];
  snprintf(loc, sizeof(loc), "%d:%d", it ? it->line : 1, it ? it->col : 1);
  uint64_t h = 1469598103934665603ULL;
  h = audit_hash_update_cstr(h, it ? it->code : "");
  h = audit_hash_update_cstr(h, it ? it->sev : "");
  h = audit_hash_update_cstr(h, it ? it->file : "");
  h = audit_hash_update_cstr(h, loc);
  h = audit_hash_update_cstr(h, it ? it->msg : "");
  h = audit_hash_update_cstr(h, shape ? shape : "");
  return h;
}

static void audit_issue_id(const Issue *it, char *out, size_t out_sz) {
  if (!out || out_sz == 0)
    return;
  const char *shape = audit_rewrite_shape_for_code(it ? it->code : NULL);
  uint64_t h = audit_issue_hash(it, shape);
  snprintf(out, out_sz, "%s:%s:%d:%s:%016llx",
           (it && it->code[0]) ? it->code : "NYAUD0000",
           (it && it->sev[0]) ? it->sev : "note",
           it ? it->line : 1, shape, (unsigned long long)h);
}

static int cmp_issue_stable(const void *a, const void *b) {
  const Issue *ia = (const Issue *)a;
  const Issue *ib = (const Issue *)b;
  int c = strcmp(ia->file, ib->file);
  if (c != 0)
    return c;
  if (ia->line != ib->line)
    return ia->line - ib->line;
  if (ia->col != ib->col)
    return ia->col - ib->col;
  c = strcmp(ia->code, ib->code);
  if (c != 0)
    return c;
  c = strcmp(ia->sev, ib->sev);
  if (c != 0)
    return c;
  return strcmp(ia->msg, ib->msg);
}

static void print_fn_pressure(const FunctionPressureVec *v, int limit) {
  size_t n = v->len;
  if (limit > 0 && n > (size_t)limit)
    n = (size_t)limit;
  printf("%s%-6s %-6s %-5s %-5s %-5s %-5s %-5s %-5s %-5s %-5s %-5s %s%s\n",
         nyt_clr(NYT_GRAY), "score", "lines", "get", "set", "app", "loop", "malloc", "old",
         "typ", "ret", "line",
         "function", nyt_clr(NYT_RESET));
  for (size_t i = 0; i < n; i++) {
    const FunctionPressure *f = &v->items[i];
    printf("%-6d %-6d %-5d %-5d %-5d %-5d %-5d %-5d %-5d %-5d %-5d %s:%s\n",
           f->score, f->lines, f->dict_gets, f->dict_sets, f->appends, f->loops, f->mallocs,
           f->legacy_calls, f->untyped_params, f->missing_returns, f->line, f->file, f->name);
  }
}

static void print_audit_json(const AuditStats *s, IssueVec *issues, FilePressureVec *files,
                             FunctionPressureVec *functions, int limit,
                             const char *min_sev) {
  printf("{\n");
  printf("  \"schema\": \"ny-fmt.audit.v1\",\n");
  printf("  \"audit_schema\": 1,\n");
  printf("  \"files\": %d,\n", s->files);
  printf("  \"findings\": %d,\n", s->findings);
  printf("  \"hot_files\": %d,\n", s->hot_files);
  printf("  \"hot_functions\": %d,\n", s->hot_functions);
  printf("  \"repeated_lines\": %d,\n", s->repeated_lines);
  printf("  \"repeated_calls\": %d,\n", s->repeated_calls);
  printf("  \"trim_targets\": %d,\n", s->trim_targets);
  printf("  \"bug_findings\": %d,\n", s->bug_findings);
  printf("  \"legacy_calls\": %d,\n", s->legacy_calls);
  printf("  \"method_syntax\": %d,\n", s->method_syntax);
  printf("  \"receiver_rewrites\": %d,\n", s->receiver_rewrites);
  printf("  \"untyped_params\": %d,\n", s->untyped_params);
  printf("  \"missing_returns\": %d,\n", s->missing_returns);
  printf("  \"type_suggestions\": %d,\n", s->type_suggestions);
  printf("  \"append_builders\": %d,\n", s->append_builders);
  printf("  \"literal_tables\": %d,\n", s->literal_tables);
  printf("  \"repeated_get_shapes\": %d,\n", s->repeated_get_shapes);
  printf("  \"trivial_main_wrappers\": %d,\n", s->trivial_main_wrappers);
  printf("  \"accepted_findings\": %d,\n", s->accepted_findings);
  printf("  \"top_files\": [\n");
  size_t nf = files->len;
  if (limit > 0 && nf > (size_t)limit)
    nf = (size_t)limit;
  for (size_t i = 0; i < nf; i++) {
    FilePressure *f = &files->items[i];
    printf("    {\"file\": ");
    json_str(f->file);
    printf(", \"score\": %d, \"lines\": %d, \"bytes\": %d,"
           " \"dict_gets\": %d, \"legacy_calls\": %d,"
           " \"untyped_params\": %d, \"missing_returns\": %d,"
           " \"repeated_lines\": %d}%s\n",
           f->score, f->lines, f->bytes, f->dict_gets, f->legacy_calls,
           f->untyped_params, f->missing_returns, f->repeated_lines,
           (i + 1 == nf) ? "" : ",");
  }
  printf("  ],\n");
  printf("  \"top_functions\": [\n");
  size_t nfn = functions->len;
  if (limit > 0 && nfn > (size_t)limit)
    nfn = (size_t)limit;
  for (size_t i = 0; i < nfn; i++) {
    FunctionPressure *f = &functions->items[i];
    printf("    {\"file\": ");
    json_str(f->file);
    printf(", \"name\": ");
    json_str(f->name);
    printf(", \"line\": %d, \"score\": %d, \"lines\": %d,"
           " \"dict_gets\": %d, \"legacy_calls\": %d,"
           " \"untyped_params\": %d, \"missing_return\": %d}%s\n",
           f->line, f->score, f->lines, f->dict_gets, f->legacy_calls,
           f->untyped_params, f->missing_returns,
           (i + 1 == nfn) ? "" : ",");
  }
  printf("  ],\n");
  printf("  \"issues\": [\n");
  size_t printed = 0;
  for (size_t i = 0; i < issues->len; i++) {
    Issue *it = &issues->items[i];
    if (!issue_visible_for_min(it, min_sev))
      continue;
    if (printed > 0)
      printf(",\n");
    char id[512];
    audit_issue_id(it, id, sizeof(id));
    const char *shape = audit_rewrite_shape_for_code(it->code);
    printf("    {\"id\": ");
    json_str(id);
    printf(", \"code\": ");
    json_str(it->code);
    printf(", \"severity\": ");
    json_str(it->sev);
    printf(", \"path\": ");
    json_str(it->file);
    printf(", \"line\": %d, \"col\": %d, \"reason\": ", it->line, it->col);
    json_str(it->msg);
    printf(", \"rewrite_shape\": ");
    json_str(shape);
    printf(", \"syntax_hint\": ");
    json_str(audit_syntax_hint_for_code(it->code));
    printf(", \"file\": ");
    json_str(it->file);
    printf(", \"message\": ");
    json_str(it->msg);
    printf(", \"note\": ");
    json_str(it->note);
    printf("}");
    printed++;
  }
  if (printed > 0)
    putchar('\n');
  printf("  ]\n");
  printf("}\n");
}

static void print_issues(IssueVec *issues, int limit, const char *min_sev);

static int run_audit_simple(StrVec *paths, const char *mode, int json_mode, int limit,
                            const char *min_sev, int types_strict) {
  FnVec fns = {0};
  IssueVec throwaway = {0};
  IssueVec issues = {0};
  AnalyzeStats astats = {0};
  AuditStats stats = {0};
  FilePressureVec files = {0};
  FunctionPressureVec functions = {0};
  CallSeqVec calls = {0};
  StrVec scan = {0};

  if (paths->len == 0) {
    collect_code_files_rec("lib", &scan);
    collect_code_files_rec("src", &scan);
    collect_code_files_rec("etc/projects", &scan);
    collect_code_files_rec("etc/tests/rt", &scan);
    collect_code_files_rec("etc/tests/fuzz/bench", &scan);
  } else {
    for (size_t i = 0; i < paths->len; i++)
      collect_code_files_rec(paths->items[i], &scan);
  }
  sv_sort(&scan);
  sv_dedup_sorted(&scan);

  for (size_t i = 0; i < scan.len; i++)
    analyze_file(scan.items[i], &fns, &throwaway, &astats);
  for (size_t i = 0; i < scan.len; i++)
    audit_scan_file(scan.items[i], &fns, &files, &functions, &issues, &stats, mode);
  if (audit_wants(mode, "calls")) {
    for (size_t i = 0; i < scan.len; i++)
      audit_scan_calls_file(scan.items[i], &fns, &calls);
  }

  qsort(files.items, files.len, sizeof(FilePressure), cmp_file_score);
  qsort(functions.items, functions.len, sizeof(FunctionPressure), cmp_fn_score);
  qsort(calls.items, calls.len, sizeof(CallSeq), cmp_callseq_count);
  if (issues.len > 1)
    qsort(issues.items, issues.len, sizeof(Issue), cmp_issue_stable);
  for (size_t i = 0; i < calls.len; i++)
    stats.repeated_calls += calls.items[i].count > 1;

  if (json_mode) {
    print_audit_json(&stats, &issues, &files, &functions, limit, min_sev);
  } else {
    nyt_msg("AUDIT", NYT_CYAN, "mode=%s scanning %zu file(s)",
            mode && *mode ? mode : "all", scan.len);
    nyt_heading("Nytrix Native Audit");
    nyt_kv("files", "%d", stats.files);
    nyt_kv("pressure", "%d hot file(s), %d hot function(s), %d repeated source line(s)",
           stats.hot_files, stats.hot_functions, stats.repeated_lines);
    nyt_kv("similarity", "%d repeated call sequence(s)", stats.repeated_calls);
    nyt_kv("findings", "%d audit finding(s), %d trim target(s)", stats.findings,
           stats.trim_targets);
    if (audit_wants(mode, "bugs"))
      nyt_kv("bugs", "%d likely bug/check finding(s)", stats.bug_findings);
    nyt_kv("syntax", "%d legacy/free helper call(s), %d receiver rewrite(s), %d property opportunity(s)",
           stats.legacy_calls, stats.receiver_rewrites, stats.method_syntax);
    nyt_kv("types", "%d untyped param(s), %d missing return annotation(s), %d suggestion(s)",
           stats.untyped_params, stats.missing_returns, stats.type_suggestions);
    nyt_kv("smells", "%d append builder(s), %d dense table(s), %d repeated dict shape(s), %d trivial main wrapper(s)",
           stats.append_builders, stats.literal_tables, stats.repeated_get_shapes,
           stats.trivial_main_wrappers);
    if (stats.accepted_findings > 0)
      nyt_kv("accepted", "%d justified audit finding(s) hidden by ny-fmt accept comments",
             stats.accepted_findings);
    fputc('\n', stdout);

    if (audit_wants(mode, "trim") || audit_wants(mode, "legacy") ||
        audit_wants(mode, "methods"))
      print_debloat_priorities(&stats);

    if (audit_wants(mode, "bloat")) {
      nyt_subheading("Top Files By Refactor Pressure");
      print_file_pressure(&files, limit, 0);
      FilePressureVec size_rank = files;
      FilePressure *copy = NULL;
      if (files.len) {
        copy = (FilePressure *)malloc(files.len * sizeof(FilePressure));
        if (copy) {
          memcpy(copy, files.items, files.len * sizeof(FilePressure));
          size_rank.items = copy;
          qsort(size_rank.items, size_rank.len, sizeof(FilePressure), cmp_file_size);
          nyt_subheading("Top Files By Size");
          print_file_pressure(&size_rank, limit, 1);
        }
      }
      free(copy);
      nyt_subheading("Top Functions By Pressure");
      print_fn_pressure(&functions, limit);
      if (functions.len) {
        FunctionPressure *copy_fn = (FunctionPressure *)malloc(functions.len * sizeof(FunctionPressure));
        if (copy_fn) {
          memcpy(copy_fn, functions.items, functions.len * sizeof(FunctionPressure));
          FunctionPressureVec line_rank = {copy_fn, functions.len, functions.len};
          qsort(line_rank.items, line_rank.len, sizeof(FunctionPressure), cmp_fn_lines);
          nyt_subheading("Longest Functions");
          print_fn_pressure(&line_rank, limit);
          free(copy_fn);
        }
      }
    }
    if (audit_wants(mode, "types")) {
      FilePressure *copy_files = NULL;
      if (files.len) {
        copy_files = (FilePressure *)malloc(files.len * sizeof(FilePressure));
        if (copy_files) {
          memcpy(copy_files, files.items, files.len * sizeof(FilePressure));
          FilePressureVec type_files = {copy_files, files.len, files.len};
          qsort(type_files.items, type_files.len, sizeof(FilePressure), cmp_file_types);
          nyt_subheading("Top Files By Type Pressure");
          print_file_pressure(&type_files, limit, 0);
          free(copy_files);
        }
      }
      if (functions.len) {
        FunctionPressure *copy_fn = (FunctionPressure *)malloc(functions.len * sizeof(FunctionPressure));
        if (copy_fn) {
          memcpy(copy_fn, functions.items, functions.len * sizeof(FunctionPressure));
          FunctionPressureVec type_fns = {copy_fn, functions.len, functions.len};
          qsort(type_fns.items, type_fns.len, sizeof(FunctionPressure), cmp_fn_types);
          nyt_subheading("Top Functions By Type Pressure");
          print_fn_pressure(&type_fns, limit);
          free(copy_fn);
        }
      }
    }
    if (issues.len) {
      nyt_subheading("Audit Findings");
      print_issues(&issues, limit, min_sev);
    }
    if (audit_wants(mode, "calls") && calls.len) {
      nyt_subheading("Repeated Call Sequences");
      print_callseqs(&calls, limit);
    }
    fputc('\n', stdout);
  }

  free(fns.items);
  free(throwaway.items);
  free(issues.items);
  free(files.items);
  free(functions.items);
  free(calls.items);
  sv_free(&scan);
  if (types_strict && audit_wants(mode, "types") &&
      (stats.untyped_params > 0 || stats.missing_returns > 0 || stats.type_suggestions > 0))
    return 1;
  return 0;
}

static void print_issue(const Issue *it) {
  const char *sc = nyt_clr(sev_color(it->sev));
  const char *code_c = nyt_clr(NYT_MAGENTA);
  const char *path_c = nyt_clr(NYT_CYAN);
  const char *gray = nyt_clr(NYT_GRAY);
  const char *rs = nyt_clr(NYT_RESET);
  printf("%s%s%s:%d:%d: %s[%s]%s %s%s:%s %s\n",
         path_c, it->file, rs, it->line, it->col,
         code_c, it->code, rs, sc, it->sev, rs, it->msg);
  if (issue_is_audit(it)) {
    char id[512];
    audit_issue_id(it, id, sizeof(id));
    printf("    %saudit-id:%s %s\n", gray, rs, id);
    printf("    %srewrite:%s %s\n", gray, rs, audit_rewrite_shape_for_code(it->code));
    const char *syntax = audit_syntax_hint_for_code(it->code);
    if (syntax && *syntax)
      printf("    %ssyntax:%s %s\n", gray, rs, syntax);
  }
  if (it->note[0])
    printf("    %snote:%s %s\n", gray, rs, it->note);
}

static void print_issues(IssueVec *issues, int limit, const char *min_sev) {
  size_t visible = 0;
  for (size_t i = 0; i < issues->len; i++)
    if (issue_visible_for_min(&issues->items[i], min_sev))
      visible++;
  size_t shown = 0;
  for (size_t i = 0; i < issues->len; i++) {
    if (!issue_visible_for_min(&issues->items[i], min_sev))
      continue;
    if (limit > 0 && shown >= (size_t)limit)
      break;
    print_issue(&issues->items[i]);
    shown++;
  }
  if (limit > 0 && visible > shown)
    printf("%s... %zu more issue(s); rerun with --limit 0 to show all%s\n",
           nyt_clr(NYT_GRAY), visible - shown, nyt_clr(NYT_RESET));
}

static void print_analyze_json(const AnalyzeStats *s, IssueVec *issues) {
  printf("{\n");
  printf("  \"files\": %d,\n", s->files);
  printf("  \"ny_files\": %d,\n", s->ny_files);
  printf("  \"c_files\": %d,\n", s->c_files);
  printf("  \"h_files\": %d,\n", s->h_files);
  printf("  \"functions\": %d,\n", s->functions);
  printf("  \"ny_functions\": %d,\n", s->ny_functions);
  printf("  \"c_functions\": %d,\n", s->c_functions);
  printf("  \"missing_doc\": %d,\n", s->missing_doc);
  printf("  \"duplicate_names\": %d,\n", s->duplicate_names);
  printf("  \"long_functions\": %d,\n", s->long_functions);
  printf("  \"long_lines\": %d,\n", s->long_lines);
  printf("  \"tabs\": %d,\n", s->tabs);
  printf("  \"trailing_ws\": %d,\n", s->trailing_ws);
  printf("  \"todos\": %d,\n", s->todos);
  printf("  \"unsafe_c\": %d,\n", s->unsafe_c);
  printf("  \"issues\": [\n");
  for (size_t i = 0; i < issues->len; i++) {
    Issue *it = &issues->items[i];
    printf("    {\"file\": ");
    json_str(it->file);
    printf(", \"line\": %d, \"col\": %d, \"code\": ", it->line, it->col);
    json_str(it->code);
    printf(", \"severity\": ");
    json_str(it->sev);
    printf(", \"message\": ");
    json_str(it->msg);
    printf(", \"note\": ");
    json_str(it->note);
    printf("}%s\n", (i + 1 == issues->len) ? "" : ",");
  }
  printf("  ]\n");
  printf("}\n");
}

static void run_analyze_simple(StrVec *paths, int json_mode, int limit) {
  FnVec fns = {0};
  IssueVec issues = {0};
  AnalyzeStats stats = {0};
  StrVec files = {0};
  if (paths->len == 0) {
    collect_code_files_rec("src", &files);
    collect_code_files_rec("lib", &files);
    collect_code_files_rec("etc/tests", &files);
  } else {
    for (size_t i = 0; i < paths->len; i++)
      collect_code_files_rec(paths->items[i], &files);
  }
  for (size_t i = 0; i < files.len; i++)
    analyze_file(files.items[i], &fns, &issues, &stats);
  analyze_duplicates(&fns, &issues, &stats);

  if (json_mode) {
    print_analyze_json(&stats, &issues);
  } else {
    int warnings = 0, notes = 0, errors = 0;
    for (size_t i = 0; i < issues.len; i++) {
      if (strcmp(issues.items[i].sev, "error") == 0)
        errors++;
      else if (strcmp(issues.items[i].sev, "note") == 0)
        notes++;
      else
        warnings++;
    }

    nyt_msg("ANALYZE", NYT_CYAN, "scanning %zu file(s)", files.len);
    nyt_heading("Nytrix Source Analysis");
    nyt_kv("files", "%d  (ny=%d c=%d h=%d py=%d)", stats.files, stats.ny_files,
           stats.c_files, stats.h_files, stats.py_files);
    nyt_kv("functions", "%d  (ny=%d c=%d public=%d)", stats.functions,
           stats.ny_functions, stats.c_functions, stats.public_functions);
    nyt_kv("std docs", "%d missing public std doc string(s)", stats.missing_doc);
    nyt_kv("shape", "%d long function(s), %d long line(s), %d duplicate name group(s)",
           stats.long_functions, stats.long_lines, stats.duplicate_names);
    nyt_kv("hygiene", "%d trailing whitespace, %d tab(s), %d TODO/FIXME marker(s), %d C string risk(s)",
           stats.trailing_ws, stats.tabs, stats.todos, stats.unsafe_c);
    nyt_kv("issues", "%s%d warning(s)%s, %s%d note(s)%s, %s%d error(s)%s",
           nyt_clr(NYT_YELLOW), warnings, nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), notes,
           nyt_clr(NYT_RESET), nyt_clr(NYT_RED), errors, nyt_clr(NYT_RESET));
    fputc('\n', stdout);
    if (issues.len)
      print_issues(&issues, limit, NULL);
    fputc('\n', stdout);
  }

  free(fns.items);
  free(issues.items);
  sv_free(&files);
}

static int cmp_dupf_norm(const void *a, const void *b) {
  const DupFn *x = (const DupFn *)a;
  const DupFn *y = (const DupFn *)b;
  if (x->hash < y->hash)
    return -1;
  if (x->hash > y->hash)
    return 1;
  if (x->norm_len != y->norm_len)
    return x->norm_len - y->norm_len;
  int sc = strcmp(x->norm ? x->norm : "", y->norm ? y->norm : "");
  if (sc != 0)
    return sc;
  sc = strcmp(x->file, y->file);
  if (sc != 0)
    return sc;
  if (x->line != y->line)
    return x->line - y->line;
  return strcmp(x->name, y->name);
}

static const DupFn *g_dupe_cmp_items = NULL;

static int dup_group_dup_norm_bytes(const DupGroup *g) {
  if (!g || g->count < 2 || !g_dupe_cmp_items)
    return 0;
  const DupFn *lead = &g_dupe_cmp_items[g->start];
  return lead->norm_len * (int)(g->count - 1);
}

static int dup_group_score(const DupGroup *g) {
  if (!g || !g_dupe_cmp_items || g->count < 2)
    return 0;
  const DupFn *lead = &g_dupe_cmp_items[g->start];
  int dup_norm = dup_group_dup_norm_bytes(g);
  int fanout_bonus = (int)(g->count - 1) * 200;
  int size_bonus = lead->norm_len >= 256 ? (lead->norm_len / 2) : (lead->norm_len / 4);
  return dup_norm + fanout_bonus + size_bonus;
}

static const char *dup_group_sev(const DupGroup *g) {
  if (!g || !g_dupe_cmp_items || g->count < 2)
    return "low";
  int score = dup_group_score(g);
  if (score >= 1200)
    return "high";
  if (score >= 700)
    return "med";
  return "low";
}

static const char *dup_group_sev_color(const char *sev) {
  if (!sev)
    return NYT_CYAN;
  if (strcmp(sev, "high") == 0)
    return NYT_RED;
  if (strcmp(sev, "med") == 0)
    return NYT_YELLOW;
  return NYT_CYAN;
}

static void dup_emit_helper_name(const DupFn *lead, size_t rank, char *out, size_t out_sz) {
  if (!out || out_sz == 0) {
    return;
  }
  out[0] = '\0';
  const char *name = lead ? lead->name : "";
  if (strncmp(name, "ny_", 3) == 0 || strncmp(name, "rt_", 3) == 0)
    name += 3;
  size_t w = 0;
  const char *prefix = "dup_helper_";
  for (size_t i = 0; prefix[i] && w + 1 < out_sz; i++)
    out[w++] = prefix[i];
  int wrote_name = 0;
  for (size_t i = 0; name[i] && w + 1 < out_sz; i++) {
    unsigned char ch = (unsigned char)name[i];
    if (isalnum(ch) || ch == '_') {
      out[w++] = (char)tolower(ch);
      wrote_name = 1;
    } else if (w > 0 && out[w - 1] != '_') {
      out[w++] = '_';
    }
  }
  if (!wrote_name) {
    char tail[48];
    snprintf(tail, sizeof(tail), "group_%zu", rank + 1);
    for (size_t i = 0; tail[i] && w + 1 < out_sz; i++)
      out[w++] = tail[i];
  }
  out[w] = '\0';
}

static int cmp_dupg_rank(const void *a, const void *b) {
  const DupGroup *x = (const DupGroup *)a;
  const DupGroup *y = (const DupGroup *)b;
  int sx = dup_group_score(x);
  int sy = dup_group_score(y);
  if (sx < sy)
    return 1;
  if (sx > sy)
    return -1;
  if (x->count != y->count)
    return (x->count > y->count) ? -1 : 1;
  const DupFn *fx = &g_dupe_cmp_items[x->start];
  const DupFn *fy = &g_dupe_cmp_items[y->start];
  if (fx->norm_len != fy->norm_len)
    return fy->norm_len - fx->norm_len;
  int sc = strcmp(fx->file, fy->file);
  if (sc != 0)
    return sc;
  if (fx->line != fy->line)
    return fx->line - fy->line;
  return strcmp(fx->name, fy->name);
}

static void print_dupes_json(const DupStats *stats, const DupFnVec *fns,
                             const DupGroupVec *groups, int limit, int emit_mode) {
  printf("{\n");
  printf("  \"files\": %d,\n", stats->files);
  printf("  \"functions\": %d,\n", stats->functions);
  printf("  \"normalized\": %d,\n", stats->normalized);
  printf("  \"duplicate_groups\": %d,\n", stats->duplicate_groups);
  printf("  \"duplicate_functions\": %d,\n", stats->duplicate_functions);
  printf("  \"min_norm_len\": %d,\n", stats->min_len);
  printf("  \"groups\": [\n");
  size_t n = groups->len;
  if (limit > 0 && n > (size_t)limit)
    n = (size_t)limit;
  for (size_t i = 0; i < n; i++) {
    const DupGroup *g = &groups->items[i];
    const DupFn *lead = &fns->items[g->start];
    int score = dup_group_score(g);
    int dup_norm = dup_group_dup_norm_bytes(g);
    const char *sev = dup_group_sev(g);
    printf("    {\"count\": %zu, \"norm_len\": %d, \"dup_norm_bytes\": %d, \"score\": %d, \"severity\": ",
           g->count, lead->norm_len, dup_norm, score);
    json_str(sev);
    printf(", \"hash\": \"0x%016llx\", \"members\": [", (unsigned long long)lead->hash);
    for (size_t j = 0; j < g->count; j++) {
      const DupFn *m = &fns->items[g->start + j];
      if (j > 0)
        printf(", ");
      printf("{\"file\": ");
      json_str(m->file);
      printf(", \"line\": %d, \"end_line\": %d, \"name\": ", m->line, m->end_line);
      json_str(m->name);
      printf("}");
    }
    printf("]}%s\n", (i + 1 == n) ? "" : ",");
  }
  printf("  ]");
  if (emit_mode) {
    printf(",\n  \"emit\": [\n");
    for (size_t i = 0; i < n; i++) {
      const DupGroup *g = &groups->items[i];
      const DupFn *lead = &fns->items[g->start];
      char helper[160];
      dup_emit_helper_name(lead, i, helper, sizeof(helper));
      printf("    {\"group\": %zu, \"helper\": ", i + 1);
      json_str(helper);
      printf(", \"reason\": ");
      char reason[192];
      snprintf(reason, sizeof(reason), "score=%d, dup_norm=%d, members=%zu",
               dup_group_score(g), dup_group_dup_norm_bytes(g), g->count);
      json_str(reason);
      printf("}%s\n", (i + 1 == n) ? "" : ",");
    }
    printf("  ]\n");
  } else {
    printf("\n");
  }
  printf("}\n");
}

static void print_dupes_text(const DupStats *stats, const DupFnVec *fns,
                             const DupGroupVec *groups, int limit, int emit_mode) {
  nyt_heading("Nytrix Duplicate Body Scan");
  nyt_kv("files", "%d C/H file(s)", stats->files);
  nyt_kv("functions", "%d total, %d normalized (min=%d)", stats->functions, stats->normalized,
         stats->min_len);
  nyt_kv("duplicates", "%d group(s), %d function(s) in duplicate groups",
         stats->duplicate_groups, stats->duplicate_functions);
  fputc('\n', stdout);
  if (groups->len == 0) {
    printf("%sNo duplicate C function bodies found.%s\n\n", nyt_clr(NYT_GRAY),
           nyt_clr(NYT_RESET));
    return;
  }

  printf("%s%-6s %-6s %-8s %-6s %-6s %-18s %s%s\n", nyt_clr(NYT_GRAY), "count", "norm", "dup",
         "score", "sev", "hash", "first", nyt_clr(NYT_RESET));
  size_t shown = 0;
  for (size_t i = 0; i < groups->len; i++) {
    if (limit > 0 && shown >= (size_t)limit)
      break;
    const DupGroup *g = &groups->items[i];
    const DupFn *lead = &fns->items[g->start];
    int score = dup_group_score(g);
    int dup_norm = dup_group_dup_norm_bytes(g);
    const char *sev = dup_group_sev(g);
    printf("%-6zu %-6d %-8d %-6d %s%-6s%s 0x%016llx %s:%d %s\n", g->count, lead->norm_len,
           dup_norm, score, nyt_clr(dup_group_sev_color(sev)), sev, nyt_clr(NYT_RESET),
           (unsigned long long)lead->hash, lead->file, lead->line, lead->name);
    for (size_t j = 1; j < g->count; j++) {
      const DupFn *m = &fns->items[g->start + j];
      printf("       %s:%d %s\n", m->file, m->line, m->name);
    }
    shown++;
  }
  if (limit > 0 && groups->len > shown)
    printf("%s... %zu more group(s); rerun with --limit 0 to show all%s\n",
           nyt_clr(NYT_GRAY), groups->len - shown, nyt_clr(NYT_RESET));
  fputc('\n', stdout);

  if (!emit_mode)
    return;

  nyt_subheading("Refactor Stubs");
  shown = 0;
  for (size_t i = 0; i < groups->len; i++) {
    if (limit > 0 && shown >= (size_t)limit)
      break;
    const DupGroup *g = &groups->items[i];
    const DupFn *lead = &fns->items[g->start];
    char helper[160];
    dup_emit_helper_name(lead, i, helper, sizeof(helper));
    int score = dup_group_score(g);
    int dup_norm = dup_group_dup_norm_bytes(g);
    const char *sev = dup_group_sev(g);
    printf("/* group %zu: sev=%s score=%d dup_norm=%d members=%zu */\n",
           i + 1, sev, score, dup_norm, g->count);
    printf("static inline void %s(/* TODO: shared args */) {\n", helper);
    printf("   /* TODO: extract shared body from:\n");
    for (size_t j = 0; j < g->count; j++) {
      const DupFn *m = &fns->items[g->start + j];
      printf("      - %s:%d %s\n", m->file, m->line, m->name);
    }
    printf("   */\n");
    printf("}\n");
    for (size_t j = 0; j < g->count; j++) {
      const DupFn *m = &fns->items[g->start + j];
      printf("/* patch %s:%d -> call %s(...) */\n", m->file, m->line, helper);
    }
    fputc('\n', stdout);
    shown++;
  }
}

static int run_dupes_mode(const FmtOpts *o) {
  StrVec files = {0};
  DupFnVec funcs = {0};
  DupGroupVec groups = {0};
  DupStats stats = {0};
  stats.min_len = o->dupes_min > 0 ? o->dupes_min : 30;

  if (o->paths.len == 0) {
    collect_c_files_rec("src", &files);
  } else {
    for (size_t i = 0; i < o->paths.len; i++)
      collect_c_files_rec(o->paths.items[i], &files);
  }
  sv_sort(&files);
  sv_dedup_sorted(&files);
  stats.files = (int)files.len;

  if (!o->json)
    nyt_msg("DUPES", NYT_CYAN, "scanning %zu C/H file(s)", files.len);

  for (size_t i = 0; i < files.len; i++)
    scan_c_file_dupes(files.items[i], &funcs, stats.min_len, &stats.functions, &stats.normalized);

  if (funcs.len > 1)
    qsort(funcs.items, funcs.len, sizeof(funcs.items[0]), cmp_dupf_norm);

  for (size_t i = 0; i < funcs.len;) {
    size_t j = i + 1;
    while (j < funcs.len && funcs.items[j].hash == funcs.items[i].hash &&
           funcs.items[j].norm_len == funcs.items[i].norm_len &&
           strcmp(funcs.items[j].norm ? funcs.items[j].norm : "",
                  funcs.items[i].norm ? funcs.items[i].norm : "") == 0) {
      j++;
    }
    size_t count = j - i;
    if (count >= 2) {
      dupg_push(&groups, i, count);
      stats.duplicate_functions += (int)count;
    }
    i = j;
  }
  stats.duplicate_groups = (int)groups.len;

  g_dupe_cmp_items = funcs.items;
  if (groups.len > 1) {
    qsort(groups.items, groups.len, sizeof(groups.items[0]), cmp_dupg_rank);
  }

  if (o->json)
    print_dupes_json(&stats, &funcs, &groups, o->limit, o->dupes_emit);
  else
    print_dupes_text(&stats, &funcs, &groups, o->limit, o->dupes_emit);
  g_dupe_cmp_items = NULL;

  dupf_free(&funcs);
  dupg_free(&groups);
  sv_free(&files);
  return 0;
}

static int cloc_line_has_code(const char *line, size_t n, int *in_block_comment) {
  size_t i = 0;
  while (i < n) {
    while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
      i++;
    if (i >= n)
      return 0;

    if (in_block_comment && *in_block_comment) {
      int closed = 0;
      while (i + 1 < n) {
        if (line[i] == '*' && line[i + 1] == '/') {
          *in_block_comment = 0;
          i += 2;
          closed = 1;
          break;
        }
        i++;
      }
      if (!closed)
        return 0;
      continue;
    }

    if (line[i] == ';')
      return 0;
    if (i + 1 < n && line[i] == '/' && line[i + 1] == '/')
      return 0;
    if (i + 1 < n && line[i] == '/' && line[i + 1] == '*') {
      if (in_block_comment)
        *in_block_comment = 1;
      i += 2;
      continue;
    }
    return 1;
  }
  return 0;
}

static int cloc_count_loc_file(const char *path) {
  size_t n = 0;
  char *src = ny_read_file_raw(path, &n);
  if (!src)
    return 0;
  int loc = 0;
  int in_block_comment = 0;
  size_t line_start = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i == n || src[i] == '\n') {
      if (cloc_line_has_code(src + line_start, i - line_start, &in_block_comment))
        loc++;
      line_start = i + 1;
    }
  }
  free(src);
  return loc;
}

enum {
  CLOC_BUCKET_SRC = 0,
  CLOC_BUCKET_LIB = 1,
  CLOC_BUCKET_PROJECTS = 2,
  CLOC_BUCKET_TESTS = 3,
  CLOC_BUCKET_OTHER = 4
};

static int cloc_bucket_from_path(const char *path) {
  const char *p = path ? path : "";
  while (p[0] == '.' && p[1] == '/')
    p += 2;

  if (strncmp(p, "etc/projects/", 13) == 0 || strstr(p, "/etc/projects/"))
    return CLOC_BUCKET_PROJECTS;
  if (strncmp(p, "src/", 4) == 0 || strstr(p, "/src/"))
    return CLOC_BUCKET_SRC;
  if (strncmp(p, "lib/", 4) == 0 || strstr(p, "/lib/"))
    return CLOC_BUCKET_LIB;
  if (strncmp(p, "etc/tests/", 10) == 0 || strstr(p, "/etc/tests/"))
    return CLOC_BUCKET_TESTS;
  return CLOC_BUCKET_OTHER;
}

static int cmp_cloc_row_desc(const void *a, const void *b) {
  const ClocRow *ra = (const ClocRow *)a;
  const ClocRow *rb = (const ClocRow *)b;
  if (ra->loc != rb->loc)
    return (rb->loc - ra->loc);
  return strcmp(ra->path ? ra->path : "", rb->path ? rb->path : "");
}

static int cloc_row_churn(const ClocRow *r) {
  if (!r)
    return 0;
  return r->add + r->del;
}

static int cloc_row_abs_net(const ClocRow *r) {
  if (!r)
    return 0;
  int net = r->add - r->del;
  return net < 0 ? -net : net;
}

static int cmp_cloc_row_churn_desc(const void *a, const void *b) {
  const ClocRow *ra = (const ClocRow *)a;
  const ClocRow *rb = (const ClocRow *)b;
  int ca = cloc_row_churn(ra);
  int cb = cloc_row_churn(rb);
  if ((ca > 0) != (cb > 0))
    return cb - ca;
  if (ca != cb)
    return cb - ca;
  int na = cloc_row_abs_net(ra);
  int nb = cloc_row_abs_net(rb);
  if (na != nb)
    return nb - na;
  if (ra->loc != rb->loc)
    return rb->loc - ra->loc;
  return strcmp(ra->path ? ra->path : "", rb->path ? rb->path : "");
}

static void cloc_fmt_int(int value, char *out, size_t out_sz) {
  if (!out || out_sz == 0)
    return;
  long long x = (long long)value;
  int neg = 0;
  if (x < 0) {
    neg = 1;
    x = -x;
  }

  char rev[64];
  int r = 0;
  int group = 0;
  do {
    if (group == 3) {
      rev[r++] = ',';
      group = 0;
    }
    rev[r++] = (char)('0' + (x % 10));
    x /= 10;
    group++;
  } while (x > 0 && r < (int)sizeof(rev) - 2);

  if (neg && r < (int)sizeof(rev) - 1)
    rev[r++] = '-';

  int w = 0;
  while (r > 0 && w + 1 < (int)out_sz)
    out[w++] = rev[--r];
  out[w] = '\0';
}

static void cloc_fmt_prefix_char(char prefix, int value, char *out, size_t out_sz) {
  char n[32];
  cloc_fmt_int(value < 0 ? -value : value, n, sizeof(n));
  if (!out || out_sz == 0)
    return;
  out[0] = prefix;
  if (out_sz == 1)
    return;
  size_t nn = strlen(n);
  size_t copy_n = nn;
  if (copy_n > out_sz - 2)
    copy_n = out_sz - 2;
  memcpy(out + 1, n, copy_n);
  out[1 + copy_n] = '\0';
}

static void cloc_fmt_delta(int value, char *out, size_t out_sz) {
  cloc_fmt_prefix_char(value >= 0 ? '+' : '-', value, out, out_sz);
}

static void cloc_fmt_prefixed_abs(char prefix, int value, char *out, size_t out_sz) {
  cloc_fmt_prefix_char(prefix, value, out, out_sz);
}

static void cloc_compact_path(const char *path, char *out, size_t out_sz, size_t visual_max) {
  if (!out || out_sz == 0)
    return;
  const char *p = path ? path : "";
  size_t n = strlen(p);
  if (visual_max < 12 || n <= visual_max) {
    snprintf(out, out_sz, "%s", p);
    return;
  }
  size_t tail = visual_max / 2;
  size_t head = visual_max - tail - 3;
  if (head < 4)
    head = 4;
  if (head + tail + 3 > n) {
    snprintf(out, out_sz, "%s", p);
    return;
  }
  snprintf(out, out_sz, "%.*s...%s", (int)head, p, p + (n - tail));
}

static const char *cloc_delta_color(int value) {
  if (value > 0)
    return NYT_GREEN;
  if (value < 0)
    return NYT_RED;
  return NYT_GRAY;
}

static void cloc_norm_path(char *out, size_t out_sz, const char *path, const char *root) {
  if (!out || out_sz == 0)
    return;
  out[0] = '\0';
  const char *p = path ? path : "";
  while (p[0] == '.' && p[1] == '/')
    p += 2;
  if ((p[0] == 'a' || p[0] == 'b') && p[1] == '/')
    p += 2;
  if (root && *root) {
    size_t rn = strlen(root);
    if (strncmp(p, root, rn) == 0 && (p[rn] == '/' || p[rn] == '\0')) {
      p += rn;
      while (*p == '/')
        p++;
    }
  }
  snprintf(out, out_sz, "%s", p);
}

static int cloc_path_eq(const char *a, const char *b, const char *root) {
  char na[PATH_MAX];
  char nb[PATH_MAX];
  cloc_norm_path(na, sizeof(na), a, root);
  cloc_norm_path(nb, sizeof(nb), b, root);
  return strcmp(na, nb) == 0;
}

static int cloc_find_row_index(const ClocRow *rows, size_t row_count, const char *path, const char *root) {
  if (!rows || row_count == 0 || !path || !*path)
    return -1;
  for (size_t i = 0; i < row_count; i++) {
    if (cloc_path_eq(rows[i].path, path, root))
      return (int)i;
  }
  return -1;
}

static void cloc_attach_git_diff(const char *repo_root, ClocStats *stats, ClocRow *rows, size_t row_count) {
  if (!stats || !rows || row_count == 0)
    return;

  FILE *pipe = popen("git --no-pager diff --numstat --no-renames HEAD -- 2>/dev/null", "r");
  if (!pipe)
    return;

  char line[PATH_MAX * 2];
  while (fgets(line, sizeof(line), pipe)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = '\0';
    if (!line[0])
      continue;

    char *tab1 = strchr(line, '\t');
    if (!tab1)
      continue;
    *tab1++ = '\0';
    char *tab2 = strchr(tab1, '\t');
    if (!tab2)
      continue;
    *tab2++ = '\0';
    if (!*tab2)
      continue;

    int add = (strcmp(line, "-") == 0) ? 0 : atoi(line);
    int del = (strcmp(tab1, "-") == 0) ? 0 : atoi(tab1);
    int idx = cloc_find_row_index(rows, row_count, tab2, repo_root);
    if (idx < 0)
      continue;

    rows[idx].add += add;
    rows[idx].del += del;
  }

  int code = pclose(pipe);
  if (code == -1)
    return;

  for (size_t i = 0; i < row_count; i++) {
    if (rows[i].add > 0 || rows[i].del > 0) {
      stats->diff_files++;
      stats->diff_add += rows[i].add;
      stats->diff_del += rows[i].del;
    }
  }
}

static void print_cloc_json(const ClocStats *s, const ClocRow *rows, size_t row_count, int top_n, int full_rows) {
  int lim = top_n;
  if (lim < 1)
    lim = 1;
  if ((size_t)lim > row_count)
    lim = (int)row_count;

  printf("{\n");
  printf("  \"files\": %d,\n", s->files);
  printf("  \"loc_total\": %d,\n", s->loc_total);
  printf("  \"loc\": {\"src\": %d, \"lib\": %d, \"projects\": %d, \"tests\": %d, \"other\": %d},\n", s->loc_src,
         s->loc_lib, s->loc_projects, s->loc_tests, s->loc_other);
  printf("  \"diff\": {\"files\": %d, \"add\": %d, \"del\": %d, \"net\": %d},\n", s->diff_files, s->diff_add,
         s->diff_del, s->diff_add - s->diff_del);
  printf("  \"ext\": {\n");
  printf("    \"ny\": {\"files\": %d, \"loc\": %d},\n", s->ny_files, s->ny_loc);
  printf("    \"nyt\": {\"files\": %d, \"loc\": %d},\n", s->nyt_files, s->nyt_loc);
  printf("    \"c\": {\"files\": %d, \"loc\": %d},\n", s->c_files, s->c_loc);
  printf("    \"h\": {\"files\": %d, \"loc\": %d}\n", s->h_files, s->h_loc);
  printf("  },\n");
  printf("  \"top\": [\n");
  for (int i = 0; i < lim; i++) {
    printf("    {\"path\": ");
    json_str(rows[i].path ? rows[i].path : "");
    printf(", \"loc\": %d, \"add\": %d, \"del\": %d, \"net\": %d}%s\n", rows[i].loc, rows[i].add, rows[i].del,
           rows[i].add - rows[i].del, (i + 1 == lim) ? "" : ",");
  }
  printf("  ]");

  if (full_rows) {
    printf(",\n  \"rows\": [\n");
    for (size_t i = 0; i < row_count; i++) {
      printf("    {\"path\": ");
      json_str(rows[i].path ? rows[i].path : "");
      printf(", \"loc\": %d, \"add\": %d, \"del\": %d, \"net\": %d}%s\n", rows[i].loc, rows[i].add, rows[i].del,
             rows[i].add - rows[i].del, (i + 1 == row_count) ? "" : ",");
    }
    printf("  ]\n");
  } else {
    printf("\n");
  }
  printf("}\n");
}

static int run_cloc_mode(const FmtOpts *o) {
  static const char *default_roots[] = {
    "src", "lib", "etc/projects", "etc/tests", NULL
  };
  StrVec files = {0};
  ClocStats stats = {0};

  if (o->paths.len == 0) {
    for (int i = 0; default_roots[i]; i++)
      collect_cloc_files_rec(default_roots[i], &files);
  } else {
    for (size_t i = 0; i < o->paths.len; i++)
      collect_cloc_files_rec(o->paths.items[i], &files);
  }

  sv_sort(&files);
  sv_dedup_sorted(&files);
  stats.files = (int)files.len;

  if (files.len == 0) {
    if (o->json) {
      printf("{\"files\": 0, \"loc_total\": 0, \"loc\": {\"src\": 0, \"lib\": 0, \"projects\": 0, \"tests\": 0, "
             "\"other\": 0}, \"diff\": {\"files\": 0, \"add\": 0, \"del\": 0, \"net\": 0}, \"ext\": {\"ny\": "
             "{\"files\": 0, \"loc\": 0}, \"nyt\": {\"files\": 0, \"loc\": 0}, \"c\": {\"files\": 0, \"loc\": 0}, "
             "\"h\": {\"files\": 0, \"loc\": 0}}, \"top\": []}\n");
    } else {
      nyt_msg("CLOC", NYT_YELLOW, "no matching files");
    }
    sv_free(&files);
    return 0;
  }

  ClocRow *rows = (ClocRow *)calloc(files.len, sizeof(ClocRow));
  ClocRow *top = (ClocRow *)calloc(files.len, sizeof(ClocRow));
  if (!rows || !top) {
    free(rows);
    free(top);
    sv_free(&files);
    nyt_err("ny-fmt", "cloc: allocation failed");
    return 1;
  }

  for (size_t i = 0; i < files.len; i++) {
    const char *path = files.items[i];
    int loc = cloc_count_loc_file(path);
    rows[i].path = path;
    rows[i].loc = loc;
    top[i] = rows[i];
    stats.loc_total += loc;

    switch (cloc_bucket_from_path(path)) {
      case CLOC_BUCKET_SRC:
        stats.loc_src += loc;
        break;
      case CLOC_BUCKET_LIB:
        stats.loc_lib += loc;
        break;
      case CLOC_BUCKET_PROJECTS:
        stats.loc_projects += loc;
        break;
      case CLOC_BUCKET_TESTS:
        stats.loc_tests += loc;
        break;
      default:
        stats.loc_other += loc;
        break;
    }

    if (nyt_ends_with(path, ".ny")) {
      stats.ny_files++;
      stats.ny_loc += loc;
    } else if (nyt_ends_with(path, ".nyt")) {
      stats.nyt_files++;
      stats.nyt_loc += loc;
    } else if (nyt_ends_with(path, ".c")) {
      stats.c_files++;
      stats.c_loc += loc;
    } else if (nyt_ends_with(path, ".h")) {
      stats.h_files++;
      stats.h_loc += loc;
    }
  }

  char repo_root[PATH_MAX];
  repo_root[0] = '\0';
  if (getcwd(repo_root, sizeof(repo_root)))
    cloc_attach_git_diff(repo_root, &stats, rows, files.len);
  for (size_t i = 0; i < files.len; i++)
    top[i] = rows[i];

  qsort(top, files.len, sizeof(ClocRow),
        stats.diff_files > 0 ? cmp_cloc_row_churn_desc : cmp_cloc_row_desc);
  int top_n = o->cloc_top > 0 ? o->cloc_top : 20;
  if ((size_t)top_n > files.len)
    top_n = (int)files.len;
  if (stats.diff_files > 0 && top_n > stats.diff_files)
    top_n = stats.diff_files;

  if (o->json) {
    print_cloc_json(&stats, top, files.len, top_n, o->cloc_full);
  } else {
    nyt_msg("CLOC", NYT_CYAN, "scanning %zu file(s)", files.len);
    nyt_heading("Nytrix CLOC");
    const char *rs = nyt_clr(NYT_RESET);
    const char *bold = nyt_clr(NYT_BOLD);
    const char *gray = nyt_clr(NYT_GRAY);
    const char *cyan = nyt_clr(NYT_CYAN);
    const char *mag = nyt_clr(NYT_MAGENTA);
    const char *green = nyt_clr(NYT_GREEN);
    const char *red = nyt_clr(NYT_RED);
    char files_s[32], loc_total_s[32], loc_src_s[32], loc_lib_s[32], loc_proj_s[32], loc_tests_s[32], loc_other_s[32];
    char diff_files_s[32], diff_add_s[32], diff_del_s[32], diff_net_s[32];
    char ny_files_s[32], ny_loc_s[32], nyt_files_s[32], nyt_loc_s[32], c_files_s[32], c_loc_s[32], h_files_s[32],
        h_loc_s[32];
    cloc_fmt_int(stats.files, files_s, sizeof(files_s));
    cloc_fmt_int(stats.loc_total, loc_total_s, sizeof(loc_total_s));
    cloc_fmt_int(stats.loc_src, loc_src_s, sizeof(loc_src_s));
    cloc_fmt_int(stats.loc_lib, loc_lib_s, sizeof(loc_lib_s));
    cloc_fmt_int(stats.loc_projects, loc_proj_s, sizeof(loc_proj_s));
    cloc_fmt_int(stats.loc_tests, loc_tests_s, sizeof(loc_tests_s));
    cloc_fmt_int(stats.loc_other, loc_other_s, sizeof(loc_other_s));
    cloc_fmt_int(stats.diff_files, diff_files_s, sizeof(diff_files_s));
    cloc_fmt_prefixed_abs('+', stats.diff_add, diff_add_s, sizeof(diff_add_s));
    cloc_fmt_prefixed_abs('-', stats.diff_del, diff_del_s, sizeof(diff_del_s));
    cloc_fmt_delta(stats.diff_add - stats.diff_del, diff_net_s, sizeof(diff_net_s));
    cloc_fmt_int(stats.ny_files, ny_files_s, sizeof(ny_files_s));
    cloc_fmt_int(stats.ny_loc, ny_loc_s, sizeof(ny_loc_s));
    cloc_fmt_int(stats.nyt_files, nyt_files_s, sizeof(nyt_files_s));
    cloc_fmt_int(stats.nyt_loc, nyt_loc_s, sizeof(nyt_loc_s));
    cloc_fmt_int(stats.c_files, c_files_s, sizeof(c_files_s));
    cloc_fmt_int(stats.c_loc, c_loc_s, sizeof(c_loc_s));
    cloc_fmt_int(stats.h_files, h_files_s, sizeof(h_files_s));
    cloc_fmt_int(stats.h_loc, h_loc_s, sizeof(h_loc_s));
    printf("%sfiles%s  %s%s%s\n", gray, rs, bold, files_s, rs);
    printf("%sloc%s    %s%s%s  %s(src%s %s | lib %s | proj %s | tests %s | other %s)%s\n", gray, rs, bold, loc_total_s,
           rs, gray, rs, loc_src_s, loc_lib_s, loc_proj_s, loc_tests_s, loc_other_s, rs);
    printf("%sdiff%s   %s%s%s %sfiles%s  %s%s%s  %s%s%s  net %s%s%s\n", gray, rs, bold, diff_files_s, rs, gray, rs,
           green, diff_add_s, rs, red, diff_del_s, rs, nyt_clr(cloc_delta_color(stats.diff_add - stats.diff_del)),
           diff_net_s, rs);
    printf("%sext%s    %sny%s %s/%s  %snyt%s %s/%s  %sc%s %s/%s  %sh%s %s/%s\n", gray, rs, cyan, rs, ny_files_s,
           ny_loc_s, cyan, rs, nyt_files_s, nyt_loc_s, cyan, rs, c_files_s, c_loc_s, cyan, rs, h_files_s, h_loc_s);
    fputc('\n', stdout);
    printf("%sTop %d %s%s\n", bold, top_n,
           stats.diff_files > 0 ? "changed files" : "by LOC", rs);
    printf(" %s#%s  %s%9s%s %s%9s%s %s%9s%s %s%9s%s  %sPATH%s\n", gray, rs, cyan, "LOC", rs, green, "+ADD", rs, red,
           "-DEL", rs, mag, "NET", rs, gray, rs);
    for (int i = 0; i < top_n; i++) {
      char loc_s[32], add_s[32], del_s[32], net_s[32], path_s[PATH_MAX];
      cloc_fmt_int(top[i].loc, loc_s, sizeof(loc_s));
      cloc_fmt_prefixed_abs('+', top[i].add, add_s, sizeof(add_s));
      cloc_fmt_prefixed_abs('-', top[i].del, del_s, sizeof(del_s));
      cloc_fmt_delta(top[i].add - top[i].del, net_s, sizeof(net_s));
      cloc_compact_path(top[i].path, path_s, sizeof(path_s), 72);
      printf("%s%2d%s  %s%9s%s %s%9s%s %s%9s%s %s%9s%s  %s%s%s\n", gray, i + 1, rs, cyan, loc_s, rs, green, add_s,
             rs, red, del_s, rs, nyt_clr(cloc_delta_color(top[i].add - top[i].del)), net_s, rs, bold, path_s, rs);
    }
    if (o->cloc_full) {
      fputc('\n', stdout);
      printf("%sAll files%s:\n", bold, rs);
      printf("%s%9s%s %s%9s%s %s%9s%s %s%9s%s  %sPATH%s\n", cyan, "LOC", rs, green, "+ADD", rs, red, "-DEL", rs,
             mag, "NET", rs, gray, rs);
      for (size_t i = 0; i < files.len; i++) {
        char loc_s[32], add_s[32], del_s[32], net_s[32], path_s[PATH_MAX];
        cloc_fmt_int(rows[i].loc, loc_s, sizeof(loc_s));
        cloc_fmt_prefixed_abs('+', rows[i].add, add_s, sizeof(add_s));
        cloc_fmt_prefixed_abs('-', rows[i].del, del_s, sizeof(del_s));
        cloc_fmt_delta(rows[i].add - rows[i].del, net_s, sizeof(net_s));
        cloc_compact_path(rows[i].path, path_s, sizeof(path_s), 96);
        printf("%s%9s%s %s%9s%s %s%9s%s %s%9s%s  %s\n", cyan, loc_s, rs, green, add_s, rs, red, del_s, rs,
               nyt_clr(cloc_delta_color(rows[i].add - rows[i].del)), net_s, rs, path_s);
      }
    }
    fputc('\n', stdout);
  }

  free(rows);
  free(top);
  sv_free(&files);
  return 0;
}

static void usage(void) {
  nyt_heading("Nytrix Format And Audit");
  printf("%susage:%s %sny fmt%s %s[mode] [options] [paths ...]%s\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET),
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny fmt --cloc%s %s[--full] [--top N] [paths ...]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny fmt --dupes%s %s[--dupes-min N] [--dupes-emit] [--json] [paths ...]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny fmt --conv%s %s--input file.texi --name NAME [--format man|md] [-o out]%s\n\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("%smodes:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %s--check --fix --analyze --audit --trim --syntax --types --dead%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--smart --overhaul --bugs --checks --bloat --modules --profiles --layouts --loops%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--contracts --ffi --constants --specialize --metaprog --constfold%s\n\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("%soptions:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %s--json --tidy --optimize --apply --diff%s\n", nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--color MODE --limit N --threshold N --root DIR --dirs DIR%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--min-sev CRIT|HIGH|MED|LOW --types-strict -v%s\n", nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("  audit modes compose; use %s--audit=loops,trim%s to find continue/guard-loop flattening wins\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  accept justified smells with %sny-fmt: accept NYAUDxxxx reason%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
}

static char *str_replace_all(const char *in, const char *pat, const char *rep) {
  if (!in || !pat || !*pat || !rep)
    return in ? strdup(in) : NULL;
  size_t in_n = strlen(in), p_n = strlen(pat), r_n = strlen(rep);
  size_t count = 0;
  for (const char *p = strstr(in, pat); p; p = strstr(p + p_n, pat))
    count++;
  size_t out_n = in_n + count * (r_n - p_n) + 1;
  char *out = (char *)malloc(out_n);
  if (!out)
    return NULL;
  char *dst = out;
  const char *cur = in;
  while (1) {
    const char *p = strstr(cur, pat);
    if (!p) {
      size_t tail = strlen(cur);
      memcpy(dst, cur, tail);
      dst += tail;
      break;
    }
    size_t chunk = (size_t)(p - cur);
    memcpy(dst, cur, chunk);
    dst += chunk;
    memcpy(dst, rep, r_n);
    dst += r_n;
    cur = p + p_n;
  }
  *dst = '\0';
  return out;
}

typedef struct {
  const char *pat;
  const char *rep;
} ReplaceRule;

static void replace_rules_owned(char **s, const ReplaceRule *rules) {
  if (!s || !*s || !rules)
    return;
  for (int i = 0; rules[i].pat; i++) {
    char *tmp = str_replace_all(*s, rules[i].pat, rules[i].rep ? rules[i].rep : "");
    free(*s);
    *s = tmp ? tmp : strdup("");
  }
}

static char *convert_texi_basic(const char *input, const char *name, const char *fmt, const char *section) {
  char *s = strdup(input ? input : "");
  if (!s)
    return NULL;
  const ReplaceRule drops[] = {{"@contents", ""},      {"@appendix", ""},
                               {"@printindex", ""},    {"@node", ""},
                               {"@menu", ""},          {"@dircategory", ""},
                               {"@direntry", ""},      {"@titlepage", ""},
                               {"\\input texinfo", ""}, {"@setfilename", ""},
                               {"@settitle", ""},      {NULL, NULL}};
  replace_rules_owned(&s, drops);
  if (strcmp(fmt, "md") == 0) {
    const ReplaceRule md_rules[] = {{"@chapter ", "# "}, {"@section ", "## "},
                                    {"@subsection ", "### "}, {"@code{", "`"},
                                    {"}", "`"}, {NULL, NULL}};
    replace_rules_owned(&s, md_rules);
    char head[256];
    snprintf(head, sizeof(head), "# %s\n\n", name ? name : "Nytrix");
    size_t n = strlen(head) + strlen(s) + 2;
    char *out = (char *)malloc(n);
    if (!out) {
      free(s);
      return NULL;
    }
    snprintf(out, n, "%s%s", head, s);
    free(s);
    return out;
  }

  char header[512];
  snprintf(header, sizeof(header), ".TH %s %s \"\" \"\" \"Nytrix\"\n", name ? name : "nytrix",
           section ? section : "1");
  const ReplaceRule man_rules[] = {{"@chapter ", ".SH "}, {"@section ", ".SH "},
                                   {"@subsection ", ".SS "}, {"@code{", "\\fB"},
                                   {"}", "\\fP"}, {NULL, NULL}};
  replace_rules_owned(&s, man_rules);
  size_t n = strlen(header) + strlen(s) + 2;
  char *out = (char *)malloc(n);
  if (!out) {
    free(s);
    return NULL;
  }
  snprintf(out, n, "%s%s", header, s);
  free(s);
  return out;
}

typedef struct { char *data; size_t len; size_t cap; } sb_t2;
static int sb2_add(sb_t2 *b, const char *s) {
  size_t sl = strlen(s);
  size_t need = b->len + sl + 1;
  if (need > b->cap) {
    size_t newcap = b->cap ? b->cap : 4096;
    while (newcap < need) newcap *= 2;
    char *p = (char *)realloc(b->data, newcap);
    if (!p) return 0;
    b->data = p;
    b->cap = newcap;
  }
  memcpy(b->data + b->len, s, sl);
  b->len += sl;
  b->data[b->len] = '\0';
  return 1;
}
static int sb2_addn(sb_t2 *b, const char *s, size_t n) {
  size_t need = b->len + n + 1;
  if (need > b->cap) {
    size_t newcap = b->cap ? b->cap : 4096;
    while (newcap < need) newcap *= 2;
    char *p = (char *)realloc(b->data, newcap);
    if (!p) return 0;
    b->data = p;
    b->cap = newcap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return 1;
}

static const char *c2ny_map_type(const char *ct) {
  if (!ct || !*ct) return "any";
  if (strcmp(ct, "int") == 0 || strcmp(ct, "signed") == 0) return "int";
  if (strcmp(ct, "unsigned") == 0 || strcmp(ct, "unsigned int") == 0) return "int";
  if (strcmp(ct, "float") == 0 || strcmp(ct, "double") == 0) return "f64";
  if (strcmp(ct, "long") == 0 || strcmp(ct, "long long") == 0) return "int";
  if (strcmp(ct, "unsigned long") == 0) return "int";
  if (strcmp(ct, "size_t") == 0) return "int";
  if (strcmp(ct, "char") == 0) return "int";
  if (strcmp(ct, "void") == 0) return "any";
  if (strcmp(ct, "_Bool") == 0 || strcmp(ct, "bool") == 0) return "bool";
  if (strcmp(ct, "uint8_t") == 0 || strcmp(ct, "int8_t") == 0) return "int";
  if (strcmp(ct, "uint16_t") == 0 || strcmp(ct, "int16_t") == 0) return "int";
  if (strcmp(ct, "uint32_t") == 0) return "int";
  if (strcmp(ct, "uint64_t") == 0 || strcmp(ct, "int64_t") == 0) return "int";
  if (strstr(ct, "*") || strstr(ct, "const char")) return "str";
  return "any";
}

typedef struct { char *data; size_t len; size_t cap; } sb_t;
static int sb_grow(sb_t *b, size_t need) {
  if (need <= b->cap) return 1;
  size_t nc = b->cap ? b->cap : 4096;
  while (nc < need) nc *= 2;
  char *p = realloc(b->data, nc);
  if (!p) return 0;
  b->data = p; b->cap = nc;
  return 1;
}
static void sb_add(sb_t *b, const char *s) {
  size_t n = strlen(s);
  if (sb_grow(b, b->len + n + 1)) { memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0; }
}
static void sb_addc(sb_t *b, char c) {
  if (sb_grow(b, b->len + 2)) { b->data[b->len++] = c; b->data[b->len] = 0; }
}
static void sb_addn(sb_t *b, const char *s, size_t n) {
  if (sb_grow(b, b->len + n + 1)) { memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0; }
}

static const char *skip_ws(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static int is_preproc(const char *s) { return *s == '#'; }

static int is_keyword(const char *s) {
  static const char *kws[] = {"if","for","while","switch","return","goto","break","continue","else","case","default","sizeof",NULL};
  for (int i = 0; kws[i]; i++) {
    size_t l = strlen(kws[i]);
    if (strncmp(s, kws[i], l) == 0 && (s[l] == ' ' || s[l] == '(' || s[l] == 0 || s[l] == ';'))
      return 1;
  }
  return 0;
}

static int is_func_def(const char *line) {
  const char *s = skip_ws(line);
  if (is_keyword(s)) return 0;
  if (strncmp(s, "static ", 7) == 0) s = skip_ws(s + 7);
  if (strncmp(s, "inline ", 7) == 0) s = skip_ws(s + 7);
  if (strncmp(s, "extern ", 7) == 0) s = skip_ws(s + 7);
  if (strncmp(s, "const ", 6) == 0) s = skip_ws(s + 6);
  if (is_keyword(s)) return 0;
  if (!*s || *s == '*' || *s == '(' || *s == '}' || *s == '{') return 0;

  if (strncmp(s, "int", 3) != 0 && strncmp(s, "void", 4) != 0 && strncmp(s, "float", 5) != 0 &&
      strncmp(s, "double", 6) != 0 && strncmp(s, "char", 4) != 0 && strncmp(s, "long", 4) != 0 &&
      strncmp(s, "short", 5) != 0 && strncmp(s, "unsigned", 8) != 0 && strncmp(s, "size_t", 6) != 0 &&
      strncmp(s, "bool", 4) != 0 && strncmp(s, "_Bool", 5) != 0 && strncmp(s, "uint8", 5) != 0 &&
      strncmp(s, "uint16", 6) != 0 && strncmp(s, "uint32", 6) != 0 && strncmp(s, "uint64", 6) != 0 &&
      strncmp(s, "int8", 4) != 0 && strncmp(s, "int16", 5) != 0 && strncmp(s, "int32", 5) != 0 &&
      strncmp(s, "int64", 5) != 0 && strncmp(s, "const ", 6) != 0 && strncmp(s, "static ", 7) != 0 &&
      strncmp(s, "struct ", 7) != 0 && strncmp(s, "enum ", 5) != 0)
    return 0;

  while (*s && *s != '(' && *s != '{' && *s != ';' && *s != '=') s++;
  return *s == '(' && !strchr(line, '=');
}

static void parse_func_sig(const char *line, char *name, size_t nsz, char *rtype, size_t rsz) {
  const char *s = skip_ws(line);

  if (strncmp(s, "static ", 7) == 0) s = skip_ws(s + 7);
  if (strncmp(s, "inline ", 7) == 0) s = skip_ws(s + 7);
  if (strncmp(s, "extern ", 7) == 0) s = skip_ws(s + 7);

  const char *paren = strchr(s, '(');
  if (!paren) { name[0] = 0; strcpy(rtype, "any"); return; }

  const char *name_end = paren;
  while (name_end > s && (name_end[-1] == ' ' || name_end[-1] == '\t' || name_end[-1] == '*')) name_end--;
  const char *name_start = name_end;
  while (name_start > s && name_start[-1] != ' ' && name_start[-1] != '\t' && name_start[-1] != '*') name_start--;

  size_t nl = name_end - name_start;
  if (nl >= nsz) nl = nsz - 1;
  memcpy(name, name_start, nl); name[nl] = 0;

  size_t rl = name_start - s;
  if (rl >= rsz) rl = rsz - 1;
  while (rl > 0 && (s[rl-1] == ' ' || s[rl-1] == '\t')) rl--;
  memcpy(rtype, s, rl); rtype[rl] = 0;
  if (!rtype[0]) strcpy(rtype, "int");
}

static int c2ny_line(const char *line, sb_t *out, int *indent, int *in_func) {
  const char *s = skip_ws(line);
  if (!*s) { sb_add(out, "\n"); return 1; }

  if (*s == '#') {
    if (strncmp(s, "#include", 8) == 0) {
      sb_add(out, "#include"); sb_add(out, s + 8); sb_add(out, "\n");
    } else if (strncmp(s, "#define", 7) == 0) {
      sb_add(out, "def "); sb_addn(out, s + 8, strlen(s) - 8); sb_add(out, "\n");
    } else if (strncmp(s, "#if", 3) == 0 || strncmp(s, "#ifdef", 6) == 0 || strncmp(s, "#ifndef", 7) == 0) {
      sb_add(out, "#if"); sb_addn(out, s + (s[1]=='i'&&s[2]=='f'?3:s[1]=='i'&&s[2]=='f'&&s[3]=='d'?6:7), strlen(s)- (s[1]=='i'&&s[2]=='f'?3:6)); sb_add(out, " {\n");
      *indent += 1;
    } else if (strncmp(s, "#else", 5) == 0) {
      sb_add(out, "} #else {\n");
    } else if (strncmp(s, "#endif", 6) == 0) {
      *indent -= 1; sb_add(out, "}\n");
    } else {
      sb_add(out, ";; "); sb_add(out, s + 1); sb_add(out, "\n");
    }
    return 1;
  }

  if (s[0] == '/' && s[1] == '/') { sb_add(out, ";;"); sb_add(out, s + 2); sb_add(out, "\n"); return 1; }
  if (s[0] == '/' && s[1] == '*') { sb_add(out, ";;"); sb_addn(out, s + 2, strlen(s) - 4); sb_add(out, "\n"); return 1; }

  if (*s == '}') {
    *indent -= 1;
    if (*in_func && *indent == 0) { *in_func = 0; return 0; }
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "}\n");
    return 1;
  }

  if (strncmp(s, "struct ", 7) == 0 || strncmp(s, "typedef ", 8) == 0 || strncmp(s, "enum ", 5) == 0) {
    sb_add(out, ";; "); sb_addn(out, s, strlen(s)); sb_add(out, "\n");
    return 1;
  }

  if (is_func_def(line)) {
    char name[256], rtype[256];
    parse_func_sig(line, name, sizeof(name), rtype, sizeof(rtype));
    if (strcmp(name, "main") == 0) {
      sb_add(out, "#main {\n");
    } else {
      const char *ny_type = c2ny_map_type(rtype);

      const char *paren = strchr(line, '(');
      const char *close = paren ? strchr(paren, ')') : NULL;
      sb_add(out, "fn "); sb_add(out, name); sb_add(out, "(");
      if (paren && close && close > paren + 1) {
        char params[4096];
        size_t plen = (size_t)(close - paren - 1);
        if (plen >= sizeof(params)) plen = sizeof(params) - 1;
        memcpy(params, paren + 1, plen); params[plen] = 0;
        if (strcmp(params, "void") != 0 && params[0]) {

          char *ctx = NULL;
          char *tok = strtok_r(params, ",", &ctx);
          int first = 1;
          while (tok) {
            while (*tok == ' ' || *tok == '\t') tok++;

            char *name_part = strrchr(tok, ' ');
            if (!name_part) name_part = tok;
            else name_part++;

            char type_part[256] = {0};
            if (name_part > tok) {
              size_t tlen = (size_t)(name_part - tok);
              while (tlen > 0 && (tok[tlen-1] == ' ' || tok[tlen-1] == '\t' || tok[tlen-1] == '*')) tlen--;
              if (tlen >= sizeof(type_part)) tlen = sizeof(type_part) - 1;
              memcpy(type_part, tok, tlen);
            }
            if (!first) sb_add(out, ", ");
            sb_add(out, c2ny_map_type(type_part)); sb_add(out, " "); sb_add(out, name_part);
            first = 0;
            tok = strtok_r(NULL, ",", &ctx);
          }
        }
      }
      sb_add(out, ") "); sb_add(out, ny_type); sb_add(out, " {\n");
    }
    *indent += 1; *in_func = 1;
    return 1;
  }

  if ((strncmp(s, "int ", 4) == 0 || strncmp(s, "int*", 4) == 0 || strncmp(s, "float ", 6) == 0 || strncmp(s, "double ", 7) == 0 ||
       strncmp(s, "char ", 5) == 0 || strncmp(s, "long ", 5) == 0 || strncmp(s, "short ", 6) == 0 ||
       strncmp(s, "unsigned ", 9) == 0 || strncmp(s, "size_t ", 7) == 0 || strncmp(s, "bool ", 5) == 0 ||
       strncmp(s, "_Bool ", 6) == 0 || strncmp(s, "uint", 4) == 0 || strncmp(s, "int", 3) == 0 ||
       strncmp(s, "const ", 6) == 0 || strncmp(s, "static ", 7) == 0 || strncmp(s, "auto ", 5) == 0 ||
       strncmp(s, "void *", 6) == 0 || strncmp(s, "void*", 5) == 0 || strncmp(s, "FILE *", 6) == 0 ||
       strncmp(s, "int *", 5) == 0 || strncmp(s, "int*", 4) == 0 || strncmp(s, "char *", 6) == 0 || strncmp(s, "char*", 5) == 0 ||
       strncmp(s, "float *", 7) == 0 || strncmp(s, "float*", 6) == 0) &&
      strchr(s, '=') && !strchr(s, '(')) {

    const char *var = s;
    while (*var && *var != ' ' && *var != '\t' && *var != '*') var++;
    if (strncmp(var, " *", 2) == 0) var += 2;
    while (*var == ' ' || *var == '\t' || *var == '*') var++;

    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "def ");

    const char *eq = strchr(var, '=');
    size_t name_len = eq ? (size_t)(eq - var) : strlen(var);
    while (name_len > 0 && (var[name_len-1] == ' ' || var[name_len-1] == '\t')) name_len--;
    sb_addn(out, var, name_len);
    sb_add(out, " = ");
    if (eq) {
      const char *val = skip_ws(eq + 1);
      size_t vlen = strlen(val);
      while (vlen > 0 && val[vlen-1] == ';') vlen--;
      sb_addn(out, val, vlen);
    }
    sb_add(out, "\n");
    return 1;
  }

  if ((strncmp(s, "int ", 4) == 0 || strncmp(s, "int*", 4) == 0 || strncmp(s, "float ", 6) == 0 || strncmp(s, "double ", 7) == 0 ||
       strncmp(s, "char ", 5) == 0 || strncmp(s, "size_t ", 7) == 0 || strncmp(s, "bool ", 5) == 0 ||
       strncmp(s, "uint", 4) == 0) && !strchr(s, '(')) {
    const char *var = s;
    while (*var && *var != ' ' && *var != '\t') var++;
    while (*var == ' ' || *var == '\t' || *var == '*') var++;
    size_t vlen = strlen(var);
    while (vlen > 0 && var[vlen-1] == ';') vlen--;
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "mut "); sb_addn(out, var, vlen); sb_add(out, " = 0\n");
    return 1;
  }

  if (strncmp(s, "return", 6) == 0 && (s[6] == ' ' || s[6] == ';' || s[6] == 0 || s[6] == '\n' || s[6] == '(')) {
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "return");
    if (s[6] != ';' && s[6] != 0) {
      const char *rest = skip_ws(s + 6);
      if (*rest && *rest != ';') { sb_add(out, " "); sb_add(out, rest); }
    }
    sb_add(out, "\n");
    return 1;
  }

  if (strncmp(s, "if (", 4) == 0) {
    const char *cond_end = strstr(s + 4, ") {");
    if (cond_end) {
      for (int i = 0; i < *indent; i++) sb_add(out, "   ");
      sb_add(out, "if "); sb_addn(out, s + 4, (size_t)(cond_end - s - 4)); sb_add(out, " {\n");
      *indent += 1;
      return 1;
    }
  }

  if (strncmp(s, "} else if (", 11) == 0 || strncmp(s, "else if (", 9) == 0) {
    const char *start = strstr(s, "if (");
    const char *cond_end = start ? strstr(start + 4, ") {") : NULL;
    if (start && cond_end) {
      *indent -= 1;
      for (int i = 0; i < *indent; i++) sb_add(out, "   ");
      sb_add(out, "} elif "); sb_addn(out, start + 4, (size_t)(cond_end - start - 4)); sb_add(out, " {\n");
      *indent += 1;
      return 1;
    }
  }

  if (strncmp(s, "} else {", 8) == 0 || strcmp(s, "else {") == 0) {
    *indent -= 1;
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "} else {\n");
    *indent += 1;
    return 1;
  }

  if (strncmp(s, "while (", 7) == 0) {
    const char *cond_end = strstr(s + 7, ") {");
    if (cond_end) {
      for (int i = 0; i < *indent; i++) sb_add(out, "   ");
      sb_add(out, "while "); sb_addn(out, s + 7, (size_t)(cond_end - s - 7)); sb_add(out, " {\n");
      *indent += 1;
      return 1;
    }
  }

  if (strncmp(s, "for (", 5) == 0) {

    char buf[4096];
    strncpy(buf, s, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    char *init = buf + 5;
    char *semi1 = strchr(init, ';');
    char *cond = semi1 ? semi1 + 1 : NULL;
    char *semi2 = cond ? strchr(cond, ';') : NULL;
    char *incr = semi2 ? semi2 + 1 : NULL;
    char *close = incr ? strchr(incr, ')') : NULL;

    if (semi1 && cond && semi2 && close) {
      *semi1 = 0; *semi2 = 0; *close = 0;

      char *sp = init;
      while (*sp == ' ' || *sp == '\t') sp++;

      while (*sp && *sp != ' ' && *sp != '\t') sp++;
      while (*sp == ' ' || *sp == '\t') sp++;
      char *varname = sp;
      char *eq = strchr(varname, '=');
      if (eq) { *eq = 0; while (eq > varname && (eq[-1]==' '||eq[-1]=='\t')) *--eq = 0; }

      char cbuf[256] = {0};
      char *cs = cond;
      while (*cs == ' ' || *cs == '\t') cs++;
      strncpy(cbuf, cs, sizeof(cbuf)-1);

      char ibuf[256] = {0};
      char *is = incr;
      while (*is == ' ' || *is == '\t') is++;
      strncpy(ibuf, is, sizeof(ibuf)-1);

      for (int i = 0; i < *indent; i++) sb_add(out, "   ");
      sb_add(out, "for "); sb_add(out, varname); sb_add(out, " in range(");

      const char *start_val = "0";
      if (eq) {
        const char *sv = skip_ws(eq + 1);

        while (*sv == ' ' || *sv == '\t') sv++;
        start_val = sv;
      }

      const char *end_val = cbuf;
      char *lt = strchr(cbuf, '<');
      if (lt) {
        *lt = 0;
        const char *cond_var = skip_ws(cbuf);

        if (strcmp(cond_var, varname) != 0) start_val = cond_var;
        end_val = skip_ws(lt + 1);
        if (*end_val == '=') end_val = skip_ws(end_val + 1);
      }

      sb_add(out, start_val); sb_add(out, ", "); sb_add(out, end_val);

      if (strstr(ibuf, "++") && !strstr(ibuf, "+=")) {

      } else if (strstr(ibuf, "+=")) {
        sb_add(out, ", ");
        const char *step_val = strstr(ibuf, "+=") + 2;
        sb_add(out, skip_ws(step_val));
      } else if (strstr(ibuf, "--")) {
        sb_add(out, ", -1");
      }

      sb_add(out, ")\n");
      *indent += 1;
      return 1;
    }

    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, ";; for: "); sb_addn(out, s, strlen(s)); sb_add(out, "\n");
    return 1;
  }

  if (strncmp(s, "printf(", 7) == 0) {
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, "print(");
    const char *rest = s + 7;

    const char *close = strrchr(rest, ')');
    if (close) { sb_addn(out, rest, (size_t)(close - rest)); sb_add(out, ")"); }
    else sb_add(out, rest);
    sb_add(out, "\n");
    return 1;
  }

  if (strncmp(s, "malloc(", 7) == 0 || strncmp(s, "calloc(", 7) == 0 || strncmp(s, "free(", 5) == 0 ||
      strncmp(s, "realloc(", 8) == 0 || strncmp(s, "memset(", 7) == 0 || strncmp(s, "memcpy(", 7) == 0) {
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");

    size_t slen = strlen(s);
    while (slen > 0 && s[slen-1] == ';') slen--;
    sb_addn(out, s, slen); sb_add(out, "\n");
    return 1;
  }

  if (strstr(s, "NULL")) {
    char buf[4096];
    strncpy(buf, s, sizeof(buf)-1); buf[sizeof(buf)-1]=0;

    char *npos;
    while ((npos = strstr(buf, "NULL")) != NULL) {
      memmove(npos + 3, npos + 4, strlen(npos + 4) + 1);
      memcpy(npos, "nil", 3);
    }
    for (int i = 0; i < *indent; i++) sb_add(out, "   ");
    sb_add(out, buf); sb_add(out, "\n");
    return 1;
  }

  for (int i = 0; i < *indent; i++) sb_add(out, "   ");
  size_t slen = strlen(s);
  while (slen > 0 && (s[slen-1] == ';' || s[slen-1] == ' ' || s[slen-1] == '\t')) slen--;
  sb_addn(out, s, slen); sb_add(out, "\n");
  return 1;
}

static int run_c2ny(const char *input_path, const char *output_path) {
  size_t n = 0;
  char *src = ny_read_file_raw(input_path, &n);
  if (!src) {
    nyt_err("ny-fmt", "c2ny: failed to read %s", input_path);
    return 1;
  }

  sb_t out = {0};
  sb_add(&out, ";; Generated by ny-fmt --c2ny from "); sb_add(&out, input_path); sb_add(&out, "\n");
  sb_add(&out, "use std.core\n\n");

  int indent = 0, in_func = 0;
  char *line = src, *end = src + n;

  while (line < end) {
    char *nl = memchr(line, '\n', (size_t)(end - line));
    size_t llen = nl ? (size_t)(nl - line) : (size_t)(end - line);

    while (llen > 0 && line[llen-1] == '\r') llen--;

    char lbuf[8192];
    size_t copy = llen < sizeof(lbuf) - 1 ? llen : sizeof(lbuf) - 1;
    memcpy(lbuf, line, copy);
    lbuf[copy] = 0;

    c2ny_line(lbuf, &out, &indent, &in_func);

    line = nl ? nl + 1 : end;
  }

  while (indent > 0) {
    indent--;
    for (int i = 0; i < indent; i++) sb_add(&out, "   ");
    sb_add(&out, "}\n");
  }

  free(src);

  if (!write_file(output_path, out.data, out.len)) {
    nyt_err("ny-fmt", "c2ny: failed to write %s", output_path);
    free(out.data);
    return 1;
  }

  printf("c2ny: %s -> %s (%zu bytes)\n", input_path, output_path, out.len);
  free(out.data);
  return 0;
}

static int run_conv(const FmtOpts *o) {
  if (!o->conv_input || !o->conv_name) {
    nyt_err("ny-fmt", "--conv requires --input and --name");
    return 2;
  }
  size_t n = 0;
  char *src = ny_read_file_raw(o->conv_input, &n);
  if (!src) {
    nyt_err("ny-fmt", "conv: failed to read %s", o->conv_input);
    return 1;
  }
  const char *fmt = o->conv_format ? o->conv_format : "man";
  char *out = convert_texi_basic(src, o->conv_name, fmt, o->conv_section ? o->conv_section : "1");
  free(src);
  if (!out) {
    nyt_err("ny-fmt", "conv: conversion failed");
    return 1;
  }
  int rc = 0;
  if (o->conv_output) {
    if (!write_file(o->conv_output, out, strlen(out))) {
      nyt_err("ny-fmt", "conv: failed to write %s", o->conv_output);
      rc = 1;
    }
  } else {
    fputs(out, stdout);
  }
  free(out);
  return rc;
}

typedef struct {
  const char *arg;
  const char *mode;
} FmtAuditAlias;

static const FmtAuditAlias k_fmt_audit_aliases[] = {
    {"audit", "all"},      {"all", "all"},             {"bloat", "bloat"},
    {"modules", "modules"}, {"profiles", "profiles"},   {"batteries", "batteries"},
    {"bugs", "bugs"},      {"bug", "bugs"},             {"correctness", "bugs"},
    {"lint", "bugs"},      {"checks", "bugs"},          {"bugchecks", "bugs"},
    {"sanity", "bugs"},
    {"trim", "trim"},      {"layouts", "layouts"},     {"layout", "layouts"},
    {"contracts", "contracts"}, {"backend-contracts", "contracts"},
    {"specialize", "specialize"}, {"specialization", "specialize"},
    {"constfold", "specialize"}, {"partial", "specialize"},
    {"metaprog", "metaprog"}, {"meta", "metaprog"}, {"roadmap", "metaprog"},
    {"codebase", "metaprog"}, {"features", "metaprog"},
    {"ffi", "ffi"},        {"dead", "dead"},           {"calls", "calls"},
    {"similarities", "calls"}, {"types", "types"},      {"legacy", "legacy"},
    {"methods", "methods"}, {"method-syntax", "methods"}, {"syntax", "methods"},
    {"smart", "smart"},    {"overhaul", "smart"},      {"constants", "constants"},
    {"consts", "constants"},
};

static const char *fmt_audit_mode_for_arg(const char *arg) {
  if (!arg || !*arg)
    return NULL;
  if (arg[0] == '-' && arg[1] == '-')
    arg += 2;
  for (size_t i = 0; i < sizeof(k_fmt_audit_aliases) / sizeof(k_fmt_audit_aliases[0]); i++) {
    if (strcmp(arg, k_fmt_audit_aliases[i].arg) == 0)
      return k_fmt_audit_aliases[i].mode;
  }
  return NULL;
}

static void fmt_audit_mode_set(FmtOpts *o, const char *mode) {
  if (!o)
    return;
  snprintf(o->audit_mode_buf, sizeof(o->audit_mode_buf), "%s",
           (mode && *mode) ? mode : "all");
  o->audit_mode = o->audit_mode_buf;
}

static void fmt_audit_mode_add(FmtOpts *o, const char *mode) {
  if (!o || !mode || !*mode)
    return;
  if (strcmp(mode, "all") == 0) {
    fmt_audit_mode_set(o, "all");
    return;
  }
  if (strcmp(o->audit_mode_buf, "all") == 0)
    o->audit_mode_buf[0] = '\0';
  if (token_list_contains(o->audit_mode_buf, mode)) {
    o->audit_mode = o->audit_mode_buf;
    return;
  }
  size_t used = strlen(o->audit_mode_buf);
  if (used > 0 && used + 1 < sizeof(o->audit_mode_buf)) {
    o->audit_mode_buf[used++] = '|';
    o->audit_mode_buf[used] = '\0';
  }
  if (used < sizeof(o->audit_mode_buf) - 1) {
    strncat(o->audit_mode_buf, mode, sizeof(o->audit_mode_buf) - used - 1);
  }
  o->audit_mode = o->audit_mode_buf[0] ? o->audit_mode_buf : "all";
}

static int parse_args(int argc, char **argv, FmtOpts *o) {
  memset(o, 0, sizeof(*o));
  o->c2ny_output = "out.ny";
  o->min_sev = "LOW";
  o->conv_format = "man";
  o->conv_section = "1";
  o->color = -2;
  o->limit = 80;
  o->cloc_top = 20;
  o->dupes_min = 30;
  o->audit_mode = "all";
  char err[256];
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage();
      return 1;
    }
    int color_mode = -2;
    int color_idx = i;
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode, err, sizeof(err));
    if (color_rc < 0) {
      nyt_err("ny-fmt", "%s", err);
      return 0;
    }
    if (color_rc > 0) {
      o->color = color_mode;
      i = color_idx;
      continue;
    }

    const char *audit_mode = fmt_audit_mode_for_arg(a);
    if (audit_mode) {
      o->audit = 1;
      fmt_audit_mode_add(o, audit_mode);
    } else if (strcmp(a, "--analyze") == 0) {
      o->analyze = 1;
    } else if (strcmp(a, "--cloc") == 0 || strcmp(a, "cloc") == 0) {
      o->cloc = 1;
    } else if (strcmp(a, "--dupes") == 0 || strcmp(a, "--duplicates") == 0 ||
               strcmp(a, "dupes") == 0 || strcmp(a, "duplicates") == 0) {
      o->dupes = 1;
    } else if (strcmp(a, "--dupes-emit") == 0 || strcmp(a, "dupes-emit") == 0) {
      o->dupes = 1;
      o->dupes_emit = 1;
    } else if (strcmp(a, "--dupes-min") == 0 && i + 1 < argc) {
      o->dupes = 1;
      o->dupes_min = atoi(argv[++i]);
    } else if (strncmp(a, "--dupes-min=", 12) == 0) {
      o->dupes = 1;
      o->dupes_min = atoi(a + 12);
    } else if (strcmp(a, "--full") == 0 || strcmp(a, "-f") == 0) {
      o->cloc_full = 1;
    } else if (strcmp(a, "--top") == 0 && i + 1 < argc) {
      o->cloc_top = atoi(argv[++i]);
    } else if (strncmp(a, "--top=", 6) == 0) {
      o->cloc_top = atoi(a + 6);
    } else if (strcmp(a, "--audit-mode") == 0 && i + 1 < argc) {
      o->audit = 1;
      fmt_audit_mode_set(o, argv[++i]);
    } else if (strncmp(a, "--audit-mode=", 13) == 0) {
      o->audit = 1;
      fmt_audit_mode_set(o, a + 13);
    } else if (strcmp(a, "--check") == 0) {
      o->check = 1;
    } else if (strcmp(a, "--fix") == 0) {
      o->fix = 1;
    } else if (strcmp(a, "--json") == 0) {
      o->json = 1;
    } else if (strcmp(a, "--types-strict") == 0) {
      o->audit = 1;
      fmt_audit_mode_add(o, "types");
      o->types_strict = 1;
    } else if (strcmp(a, "--limit") == 0 && i + 1 < argc) {
      o->limit = atoi(argv[++i]);
    } else if (strncmp(a, "--limit=", 8) == 0) {
      o->limit = atoi(a + 8);
    } else if (strcmp(a, "--threshold") == 0 && i + 1 < argc) {
      i++;
    } else if (strncmp(a, "--threshold=", 12) == 0) {

    } else if (strcmp(a, "--root") == 0 && i + 1 < argc) {
      i++;
    } else if (strncmp(a, "--root=", 7) == 0) {

    } else if (strcmp(a, "--dirs") == 0 && i + 1 < argc) {
      sv_push(&o->paths, argv[++i]);
    } else if (strncmp(a, "--dirs=", 7) == 0) {
      sv_push(&o->paths, a + 7);
    } else if (strcmp(a, "--tidy") == 0) {
      o->tidy = 1;
    } else if (strcmp(a, "--optimize") == 0) {
      o->optimize = 1;
    } else if (strcmp(a, "--apply") == 0) {
      o->apply = 1;
    } else if (strcmp(a, "--diff") == 0) {
      o->diff = 1;
    } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
      o->verbose = 1;
    } else if (strcmp(a, "--align") == 0 || strcmp(a, "--align-macros") == 0) {
      o->align_macros = 1;
    } else if (strcmp(a, "--c2ny") == 0) {
      o->c2ny = 1;
    } else if (strcmp(a, "--conv") == 0) {
      o->conv = 1;
    } else if (strcmp(a, "--input") == 0 && i + 1 < argc) {
      o->conv_input = argv[++i];
    } else if (strcmp(a, "--name") == 0 && i + 1 < argc) {
      o->conv_name = argv[++i];
    } else if (strcmp(a, "--format") == 0 && i + 1 < argc) {
      o->conv_format = argv[++i];
    } else if (strcmp(a, "--section") == 0 && i + 1 < argc) {
      o->conv_section = argv[++i];
    } else if ((strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) && i + 1 < argc) {
      o->conv_output = argv[++i];
    } else if (strcmp(a, "--min-sev") == 0 && i + 1 < argc) {
      o->min_sev = argv[++i];
    } else if (strncmp(a, "--min-sev=", 10) == 0) {
      o->min_sev = a + 10;
    } else if (a[0] == '-') {
      nyt_err("ny-fmt", "unknown option: %s", a);
      return 0;
    } else {
      sv_push(&o->paths, a);
    }
  }
  return 2;
}

static void run_check_mode(const FmtOpts *opts) {
  StrVec files = {0};
  if (opts->paths.len == 0) {
    collect_files_rec("lib", &files, 1);
    collect_files_rec("etc/tests", &files, 1);
  } else {
    for (size_t i = 0; i < opts->paths.len; i++)
      collect_files_rec(opts->paths.items[i], &files, 1);
  }

  size_t check_count = 0;
  for (size_t i = 0; i < files.len; i++) {
    if (!is_expected_error_fixture(files.items[i]))
      check_count++;
  }
  nyt_msg("CHECK", NYT_CYAN, "scanning %zu files for parse bugs", check_count);

  int failed = 0;
  for (size_t i = 0; i < files.len; i++) {
    if (is_expected_error_fixture(files.items[i]))
      continue;
    int issue = 0;
    brace_check_file(files.items[i], opts->fix, opts->verbose, &issue);
    if (issue)
      failed++;
  }
  if (failed == 0)
    nyt_msg("OK", NYT_GREEN, "check complete: all %zu files OK", check_count);
  else
    nyt_msg("CHECK", NYT_RED, "%d file(s) with issues", failed);
  sv_free(&files);
}

static int run_align_macros_mode(const FmtOpts *opts) {
  StrVec files = {0};
  if (opts->paths.len == 0) {
    collect_c_files_rec("src", &files);
  } else {
    for (size_t i = 0; i < opts->paths.len; i++)
      collect_c_files_rec(opts->paths.items[i], &files);
  }
  int changed = 0;
  for (size_t i = 0; i < files.len; i++) {
    size_t n = 0;
    char *src = ny_read_file_raw(files.items[i], &n);
    if (!src) continue;
    char *dst = malloc(n * 2 + 1);
    if (!dst) { free(src); continue; }
    size_t di = 0, si = 0;
    int block_changed = 0;
    while (si < n) {
      const char *line_start = src + si;
      const char *nl = memchr(line_start, '\n', n - si);
      size_t line_len = nl ? (size_t)(nl - line_start) : n - si;
      const char *trimmed = line_start;
      while (trimmed < line_start + line_len && (*trimmed == ' ' || *trimmed == '\t'))
        trimmed++;
      if (strncmp(trimmed, "#define ", 8) == 0 && nl && si + line_len + 1 < n) {

        const char *next = src + si + line_len + 1;
        const char *next_trim = next;
        while (next_trim < src + n && (*next_trim == ' ' || *next_trim == '\t'))
          next_trim++;
        const char *next_nl = memchr(next, '\n', n - (next - src));

        const char *bs = memchr(next, '\\', next_nl ? (size_t)(next_nl - next) : (n - (next - src)));
        if (bs && strncmp(next_trim, "do {", 4) == 0) {

          memcpy(dst + di, line_start, line_len);
          di += line_len; si += line_len + 1;
          if (dst[di - 1] != '\n') dst[di++] = '\n';

          while (si < n) {
            const char *bl = src + si;
            const char *bnl = memchr(bl, '\n', n - si);
            size_t bl_len = bnl ? (size_t)(bnl - bl) : n - si;
            memcpy(dst + di, bl, bl_len);
            di += bl_len; si += bl_len + 1;
            if (!bnl) break;

            if (strstr(bl, "while (0)") || strstr(bl, "while(0)")) {
              if (dst[di - 1] != '\n') dst[di++] = '\n';
              break;
            }
          }
          continue;
        }
      }

      memcpy(dst + di, line_start, line_len);
      di += line_len;
      si += line_len + 1;
      if (si < n && dst[di - 1] != '\n') dst[di++] = '\n';
    }
    dst[di] = '\0';
    if (block_changed) {
      if (write_file(files.items[i], dst, di))
        changed++;
    }
    free(dst);
    free(src);
  }
  nyt_msg("ALIGN", changed ? NYT_GREEN : NYT_GRAY, "aligned macros in %d file(s)", changed);
  sv_free(&files);
  return 0;
}

int ny_fmt_main(int argc, char **argv) {
  char root[PATH_MAX];
  if (!ensure_repo_root(root, sizeof(root))) {
    nyt_err("ny-fmt", "could not locate repository root");
    return 1;
  }
  if (chdir(root) != 0) {
    nyt_err("ny-fmt", "failed to chdir to root: %s", root);
    return 1;
  }

  FmtOpts opts;
  int ps = parse_args(argc, argv, &opts);
  if (ps == 0) {
    sv_free(&opts.paths);
    return 2;
  }
  if (ps == 1) {
    sv_free(&opts.paths);
    return 0;
  }
  if (opts.json)
    ny_setenv("NYTRIX_TOOL_COLOR", "never", 1);
  else if (opts.color == 1)
    ny_setenv("NYTRIX_TOOL_COLOR", "always", 1);
  else if (opts.color == 0)
    ny_setenv("NYTRIX_TOOL_COLOR", "never", 1);
  else if (opts.color == -1)
    ny_setenv("NYTRIX_TOOL_COLOR", "auto", 1);
  if (opts.limit < 0)
    opts.limit = 0;

  if (opts.tidy) {
    opts.check = 1;
    opts.analyze = 1;
  }

  if (opts.c2ny) {
    const char *in = opts.paths.len > 0 ? opts.paths.items[0] : NULL;
    const char *out = opts.conv_output ? opts.conv_output : "out.ny";
    if (!in) { nyt_err("ny-fmt", "--c2ny requires an input C file"); sv_free(&opts.paths); return 2; }
    int rc = run_c2ny(in, out);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.align_macros) {
    int rc = run_align_macros_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.conv) {
    int rc = run_conv(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.cloc) {
    int rc = run_cloc_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.dupes) {
    int rc = run_dupes_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  int only_default_fmt =
      !(opts.analyze || opts.audit || opts.check || opts.optimize || opts.tidy || opts.dupes);

  if (only_default_fmt || opts.tidy) {
    StrVec files = {0};
    if (opts.paths.len == 0) {
      collect_files_rec("src", &files, 0);
      collect_files_rec("lib", &files, 0);
      collect_files_rec("etc/tests", &files, 0);
    } else {
      for (size_t i = 0; i < opts.paths.len; i++)
        collect_files_rec(opts.paths.items[i], &files, 0);
    }
    int changed = 0;
    for (size_t i = 0; i < files.len; i++) {
      int chg = 0;
      if (format_file(files.items[i], &chg) && chg)
        changed++;
    }
    nyt_msg("FMT", changed ? NYT_GREEN : NYT_GRAY, "complete (%d files updated)", changed);
    sv_free(&files);
  }

  if (opts.check || opts.tidy)
    run_check_mode(&opts);

  if (opts.analyze || opts.optimize || opts.tidy)
    run_analyze_simple(&opts.paths, opts.json, opts.limit);

  int audit_rc = 0;
  if (opts.audit)
    audit_rc = run_audit_simple(&opts.paths, opts.audit_mode, opts.json, opts.limit,
                                opts.min_sev, opts.types_strict);

  if (opts.optimize && opts.apply) {
    StrVec files = {0};
    if (opts.paths.len == 0) {
      collect_files_rec("src", &files, 1);
      collect_files_rec("lib", &files, 1);
      collect_files_rec("etc/tests", &files, 1);
    } else {
      for (size_t i = 0; i < opts.paths.len; i++)
        collect_files_rec(opts.paths.items[i], &files, 1);
    }
    int changed = 0;
    for (size_t i = 0; i < files.len; i++) {
      int chg = 0;
      if (format_file(files.items[i], &chg) && chg)
        changed++;
    }
    if (opts.diff)
      nyt_warn("ny-fmt", "optimize --diff is not yet implemented in C mode");
    nyt_msg("OPT", NYT_GREEN, "applied updates to %d file(s)", changed);
    sv_free(&files);
  }

  sv_free(&opts.paths);
  return audit_rc;
}
