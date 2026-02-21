#include "base/common.h"
#include "base/util.h"
#include "priv.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char **g_diag_seen_tbl = NULL;
static size_t g_diag_seen_cap = 0;
static size_t g_diag_seen_len = 0;
static bool g_last_primary_emitted = false;
static unsigned g_primary_hint_count = 0;
static unsigned g_primary_fix_count = 0;
static unsigned g_primary_note_count = 0;
static char *g_diag_cached_file = NULL;
static char *g_diag_cached_src = NULL;

static const char *diag_load_source(const char *filename) {
  if (!filename || filename[0] == '<')
    return NULL;
  if (g_diag_cached_file && strcmp(g_diag_cached_file, filename) == 0)
    return g_diag_cached_src;
  free(g_diag_cached_file);
  free(g_diag_cached_src);
  g_diag_cached_file = ny_strdup(filename);
  g_diag_cached_src = ny_read_file(filename);
  return g_diag_cached_src;
}

static bool diag_extract_line(const char *src, int line, const char **out_start,
                              size_t *out_len) {
  if (!src || line <= 0 || !out_start || !out_len)
    return false;
  const char *cur = src;
  int cur_line = 1;
  while (*cur && cur_line < line) {
    if (*cur == '\n')
      cur_line++;
    cur++;
  }
  if (cur_line != line)
    return false;
  const char *start = cur;
  while (*cur && *cur != '\n')
    cur++;
  *out_start = start;
  *out_len = (size_t)(cur - start);
  return true;
}

static void diag_print_snippet(token_t tok, const char *color) {
  if (!tok.filename || tok.filename[0] == '<' || tok.line <= 0 || tok.col <= 0)
    return;
  const char *src = diag_load_source(tok.filename);
  if (!src)
    return;
  const char *line_start = NULL;
  size_t line_len = 0;
  if (!diag_extract_line(src, tok.line, &line_start, &line_len))
    return;
  if (line_len == 0)
    return;

  size_t caret_col = (size_t)(tok.col - 1);
  if (caret_col > line_len)
    caret_col = line_len;
  size_t caret_len = tok.len ? tok.len : 1;
  if (caret_col + caret_len > line_len)
    caret_len = line_len > caret_col ? (line_len - caret_col) : 1;

  const size_t max_len = 200;
  size_t start = 0;
  size_t end = line_len;
  bool prefix = false;
  bool suffix = false;
  if (line_len > max_len) {
    if (caret_col > max_len / 2)
      start = caret_col - max_len / 2;
    if (start + max_len > line_len)
      start = line_len - max_len;
    end = start + max_len;
    prefix = start > 0;
    suffix = end < line_len;
  }

  size_t show_len = end - start;
  char *buf = malloc(show_len + 1);
  if (!buf)
    return;
  for (size_t i = 0; i < show_len; i++) {
    char c = line_start[start + i];
    buf[i] = (c == '\t') ? ' ' : c;
  }
  buf[show_len] = '\0';

  int line_no = tok.line;
  int width = 1;
  for (int tmp = line_no; tmp >= 10; tmp /= 10)
    width++;

  const char *gray = clr(NY_CLR_GRAY);
  const char *reset = clr(NY_CLR_RESET);
  const char *mark = clr(color);

  fprintf(stderr, "  %s%*d%s | %s%s%s\n", gray, width, line_no, reset,
          prefix ? "..." : "", buf, suffix ? "..." : "");

  size_t caret_pad = caret_col - start + (prefix ? 3 : 0);
  fprintf(stderr, "  %s%*s%s | ", gray, width, "", reset);
  for (size_t i = 0; i < caret_pad; i++)
    fputc(' ', stderr);
  fputs(mark, stderr);
  for (size_t i = 0; i < caret_len; i++)
    fputc('^', stderr);
  fputs(reset, stderr);
  fputc('\n', stderr);
  free(buf);
}

static uint64_t diag_hash(const char *s) { return ny_hash64_cstr(s); }

static bool diag_tbl_grow(void) {
  size_t new_cap = g_diag_seen_cap ? g_diag_seen_cap * 2 : 1024;
  char **new_tbl = calloc(new_cap, sizeof(char *));
  if (!new_tbl)
    return false;

  for (size_t i = 0; i < g_diag_seen_cap; ++i) {
    char *entry = g_diag_seen_tbl[i];
    if (!entry)
      continue;
    uint64_t h = diag_hash(entry);
    size_t mask = new_cap - 1;
    size_t idx = (size_t)h & mask;
    while (new_tbl[idx])
      idx = (idx + 1) & mask;
    new_tbl[idx] = entry;
  }

  free(g_diag_seen_tbl);
  g_diag_seen_tbl = new_tbl;
  g_diag_seen_cap = new_cap;
  return true;
}

static bool diag_mark_seen(const char *key) {
  if (!key)
    return false;
  if (g_diag_seen_cap == 0) {
    if (!diag_tbl_grow())
      return true; // Fail open: better emit than hide diagnostics.
  }
  // Keep load factor <= 0.66
  if ((g_diag_seen_len + 1) * 3 >= g_diag_seen_cap * 2) {
    if (!diag_tbl_grow())
      return true; // Fail open.
  }

  uint64_t h = diag_hash(key);
  size_t mask = g_diag_seen_cap - 1;
  size_t idx = (size_t)h & mask;
  while (g_diag_seen_tbl[idx]) {
    if (strcmp(g_diag_seen_tbl[idx], key) == 0)
      return false;
    idx = (idx + 1) & mask;
  }
  g_diag_seen_tbl[idx] = ny_strdup(key);
  g_diag_seen_len++;
  return true;
}

bool ny_diag_should_emit(const char *kind, token_t tok, const char *name) {
  char key[512];
  const char *file = tok.filename ? tok.filename : "unknown";
  snprintf(key, sizeof(key), "%s|%s|%d|%d|%s", kind ? kind : "diag", file,
           tok.line, tok.col, name ? name : "");
  return diag_mark_seen(key);
}

bool ny_is_stdlib_tok(token_t tok) {
  if (!tok.filename)
    return false;
  return strcmp(tok.filename, "<stdlib>") == 0 ||
         strcmp(tok.filename, "<repl_std>") == 0;
}

bool ny_strict_error_enabled(codegen_t *cg, token_t tok) {
  return cg->strict_diagnostics && !ny_is_stdlib_tok(tok);
}

static bool ny_diag_emit_unique(const char *level, token_t tok,
                                const char *rendered) {
  char key[1536];
  const char *file = tok.filename ? tok.filename : "unknown";
  int line = tok.line < 0 ? 0 : tok.line;
  int col = tok.col < 0 ? 0 : tok.col;
  snprintf(key, sizeof(key), "line|%s|%s|%d|%d|%s", level, file, line, col,
           rendered ? rendered : "");
  return diag_mark_seen(key);
}

static int ny_diag_budget(unsigned kind_count, int v0_budget, int v1_budget) {
  if (!g_last_primary_emitted)
    return 0;
  if (verbose_enabled >= 2)
    return 1;
  if (verbose_enabled >= 1)
    return (int)kind_count < v1_budget;
  return (int)kind_count < v0_budget;
}

static void ny_diag_vprint(const char *level, const char *color, token_t tok,
                           const char *fmt, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  char rendered[1024];
  vsnprintf(rendered, sizeof(rendered), fmt, cp);
  va_end(cp);
  if (!ny_diag_emit_unique(level, tok, rendered))
    goto done;
  const char *file = tok.filename ? tok.filename : "unknown";
  int line = tok.line < 0 ? 0 : tok.line;
  int col = tok.col < 0 ? 0 : tok.col;
  fprintf(stderr, "%s:%d:%d: %s%s:%s ", file, line, col, clr(color), level,
          clr(NY_CLR_RESET));
  fputs(rendered, stderr);
  fputc('\n', stderr);
  diag_print_snippet(tok, color);
  g_last_primary_emitted = true;
  g_primary_hint_count = 0;
  g_primary_fix_count = 0;
  g_primary_note_count = 0;
  return;
done:
  g_last_primary_emitted = false;
}

void ny_diag_error(token_t tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ny_diag_vprint("error", NY_CLR_RED, tok, fmt, ap);
  va_end(ap);
}

void ny_diag_warning(token_t tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ny_diag_vprint("warning", NY_CLR_YELLOW, tok, fmt, ap);
  va_end(ap);
}

void ny_diag_hint(const char *fmt, ...) {
  if (!ny_diag_budget(g_primary_hint_count, 2, 3))
    return;
  va_list ap;
  va_start(ap, fmt);
  char rendered[1024];
  char key[1200];
  vsnprintf(rendered, sizeof(rendered), fmt, ap);
  va_end(ap);
  snprintf(key, sizeof(key), "hint|%s", rendered);
  if (!diag_mark_seen(key))
    return;
  fprintf(stderr, "  %shint:%s ", clr(NY_CLR_YELLOW), clr(NY_CLR_RESET));
  fputs(rendered, stderr);
  fputc('\n', stderr);
  g_primary_hint_count++;
}

void ny_diag_fix(const char *fmt, ...) {
  if (!ny_diag_budget(g_primary_fix_count, 1, 2))
    return;
  va_list ap;
  va_start(ap, fmt);
  char rendered[1024];
  char key[1200];
  vsnprintf(rendered, sizeof(rendered), fmt, ap);
  va_end(ap);
  snprintf(key, sizeof(key), "fix|%s", rendered);
  if (!diag_mark_seen(key))
    return;
  fprintf(stderr, "  %sfix:%s ", clr(NY_CLR_GREEN), clr(NY_CLR_RESET));
  fputs(rendered, stderr);
  fputc('\n', stderr);
  g_primary_fix_count++;
}

void ny_diag_note_tok(token_t tok, const char *fmt, ...) {
  if (!ny_diag_budget(g_primary_note_count, 1, 2))
    return;
  va_list ap;
  va_start(ap, fmt);
  char rendered[1024];
  vsnprintf(rendered, sizeof(rendered), fmt, ap);
  va_end(ap);
  const char *file = tok.filename ? tok.filename : "unknown";
  int line = tok.line < 0 ? 0 : tok.line;
  int col = tok.col < 0 ? 0 : tok.col;
  char key[1536];
  snprintf(key, sizeof(key), "note|%s|%d|%d|%s", file, line, col, rendered);
  if (!diag_mark_seen(key))
    return;
  fprintf(stderr, "%s:%d:%d: %snote:%s ", file, line, col, clr(NY_CLR_CYAN),
          clr(NY_CLR_RESET));
  fputs(rendered, stderr);
  fputc('\n', stderr);
  g_primary_note_count++;
}
