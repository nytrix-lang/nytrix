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

static uint64_t diag_hash(const char *s) {
  // FNV-1a 64-bit
  uint64_t h = 1469598103934665603ULL;
  for (; s && *s; ++s) {
    h ^= (unsigned char)*s;
    h *= 1099511628211ULL;
  }
  return h;
}

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
  fprintf(stderr, "%s:%d:%d: %s%s:\033[0m ", file, line, col, color, level);
  fputs(rendered, stderr);
  fputc('\n', stderr);
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
  ny_diag_vprint("error", "\033[31m", tok, fmt, ap);
  va_end(ap);
}

void ny_diag_warning(token_t tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ny_diag_vprint("warning", "\033[33m", tok, fmt, ap);
  va_end(ap);
}

void ny_diag_hint(const char *fmt, ...) {
  if (!ny_diag_budget(g_primary_hint_count, 1, 2))
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
  fprintf(stderr, "  \033[33mhint:\033[0m ");
  fputs(rendered, stderr);
  fputc('\n', stderr);
  g_primary_hint_count++;
}

void ny_diag_fix(const char *fmt, ...) {
  if (!ny_diag_budget(g_primary_fix_count, 0, 1))
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
  fprintf(stderr, "  \033[32mfix:\033[0m ");
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
  fprintf(stderr, "%s:%d:%d: \033[36mnote:\033[0m ", file, line, col);
  fputs(rendered, stderr);
  fputc('\n', stderr);
  g_primary_note_count++;
}
