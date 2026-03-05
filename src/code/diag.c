#include "base/common.h"
#include "base/util.h"
#include "priv.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *key;
  int count;
} diag_entry_t;

static diag_entry_t *g_diag_seen_tbl = NULL;
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

static void diag_print_snippet(token_t tok, const char *color) {
  if (!tok.filename || tok.filename[0] == '<' || tok.line <= 0 || tok.col <= 0)
    return;
  const char *src = diag_load_source(tok.filename);
  if (!src)
    return;
  ny_print_snippet(src, tok.line, tok.col, tok.len, color);
}

static uint64_t diag_hash(const char *s) { return ny_hash64_cstr(s); }

static bool diag_tbl_grow(void) {
  size_t old_cap = g_diag_seen_cap;
  diag_entry_t *old_tbl = g_diag_seen_tbl;
  g_diag_seen_cap = g_diag_seen_cap ? g_diag_seen_cap * 2 : 1024;
  g_diag_seen_tbl = calloc(g_diag_seen_cap, sizeof(diag_entry_t));
  if (!g_diag_seen_tbl)
    return false;
  size_t mask = g_diag_seen_cap - 1;
  for (size_t i = 0; i < old_cap; ++i) {
    if (!old_tbl[i].key)
      continue;
    uint64_t h = diag_hash(old_tbl[i].key);
    size_t idx = (size_t)h & mask;
    while (g_diag_seen_tbl[idx].key)
      idx = (idx + 1) & mask;
    g_diag_seen_tbl[idx] = old_tbl[i];
  }
  free(old_tbl);
  return true;
}

static bool diag_mark_seen(const char *key) {
  if (!key)
    return false;
  if (g_diag_seen_cap == 0 ||
      (g_diag_seen_len + 1) * 3 >= g_diag_seen_cap * 2) {
    if (!diag_tbl_grow())
      return true;
  }
  uint64_t h = diag_hash(key);
  size_t mask = g_diag_seen_cap - 1;
  size_t idx = (size_t)h & mask;
  while (g_diag_seen_tbl[idx].key) {
    if (strcmp(g_diag_seen_tbl[idx].key, key) == 0) {
      g_diag_seen_tbl[idx].count++;
      return g_diag_seen_tbl[idx].count <= 3;
    }
    idx = (idx + 1) & mask;
  }
  g_diag_seen_tbl[idx].key = ny_strdup(key);
  g_diag_seen_tbl[idx].count = 1;
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
  bool res = false;
  if (tok.filename[0] == '<') {
    res = strcmp(tok.filename, "<stdlib>") == 0 ||
          strcmp(tok.filename, "<repl_std>") == 0;
  } else if (strncmp(tok.filename, "lib/", 4) == 0 ||
             strncmp(tok.filename, "lib\\", 4) == 0 ||
             strstr(tok.filename, "std.ny") != NULL ||
             strstr(tok.filename, "/share/nytrix/") != NULL ||
             strstr(tok.filename, "/lib/nytrix/std/") != NULL) {
    res = true;
  }
  static int debug_std = -1;
  if (debug_std < 0)
    debug_std = getenv("NYTRIX_DEBUG_STD") != NULL;
  if (debug_std)
    fprintf(stderr, "[STD_TOK] '%s' -> %s\n", tok.filename,
            res ? "true" : "false");
  return res;
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

/* Enhanced error with context and suggestions */
void ny_diag_error_context(token_t tok, const char *primary_msg,
                           const char *context, const char *suggestion) {
  ny_diag_error(tok, "%s", primary_msg);

  if (context && *context) {
    fprintf(stderr, "  %scontext:%s %s\n", clr(NY_CLR_CYAN), clr(NY_CLR_RESET),
            context);
  }
  if (suggestion && *suggestion) {
    fprintf(stderr, "  %sfix:%s %s\n", clr(NY_CLR_GREEN), clr(NY_CLR_RESET),
            suggestion);
  }
}

/* Type mismatch error with helpful suggestions */
void ny_diag_type_mismatch(token_t tok, const char *expected, const char *got,
                           const char *context) {
  ny_diag_error(tok, "type mismatch: expected '%s', got '%s'", expected, got);
  if (context && *context) {
    fprintf(stderr, "  %scontext:%s %s\n", clr(NY_CLR_CYAN), clr(NY_CLR_RESET),
            context);
  }
  /* Provide helpful suggestions based on types */
  if (strcmp(expected, "int") == 0 && strcmp(got, "f64") == 0) {
    ny_diag_fix("Use 'trunc(x)' or 'floor(x)' to convert f64 to int");
  } else if (strcmp(expected, "f64") == 0 && strcmp(got, "int") == 0) {
    ny_diag_fix("Use 'f64(x)' to convert int to f64");
  } else if (strstr(expected, "list") && strstr(got, "dict")) {
    ny_diag_fix("Use 'list(d.keys())' or 'list(d.values())' to convert");
  } else if (strstr(expected, "str") && strstr(got, "int")) {
    ny_diag_fix("Use 'to_str(x)' to convert int to string");
  } else if (strstr(expected, "int") && strstr(got, "str")) {
    ny_diag_fix("Use 'atoi(s)' to convert string to int");
  }
}

void ny_diag_hint(const char *fmt, ...) {
  if (!g_last_primary_emitted)
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
  if (!g_last_primary_emitted)
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
  if (!g_last_primary_emitted)
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

/* Enhanced error reporting with context and suggestions */
void ny_diag_error_with_context(token_t tok, const char *primary_msg,
                                const char *common_cause,
                                const char *fix_suggestion) {
  ny_diag_error(tok, "%s", primary_msg);
  if (common_cause && *common_cause) {
    ny_diag_hint("Common cause: %s", common_cause);
  }
  if (fix_suggestion && *fix_suggestion) {
    ny_diag_fix("%s", fix_suggestion);
  }
}
