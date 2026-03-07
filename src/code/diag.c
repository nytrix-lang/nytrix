/*  ──────────────────────────────────────────────────────────────────────
 *  Nytrix Diagnostic System — Overhauled
 *  • Coded errors  [E####]  and warnings  [W####]
 *  • Smart contextual suggestions
 *  • Unified hint / fix / note pipeline
 *  • Deduplication with frequency counter
 *  ────────────────────────────────────────────────────────────────────── */

#include "base/common.h"
#include "base/intern.h"
#include "base/util.h"
#include "priv.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── error / warning codes ─────────────────────────────────────────── */
typedef enum {
  /* General (E1xxx) */
  E_SYNTAX = 1001,
  E_UNDEFINED = 1002,
  E_TYPE_MISMATCH = 1003,
  E_ARITY = 1004,
  E_DUPLICATE_NAME = 1005,
  E_SHADOWING = 1006,
  E_INVALID_LITERAL = 1007,
  E_RETURN_MISMATCH = 1008,
  E_IMMUTABLE_MODIFY = 1009,
  E_NOT_CALLABLE = 1010,
  E_INDEX_OOB = 1011,
  E_MATCH_EXHAUSTIVE = 1012,
  E_EFFECT_VIOLATION = 1013,
  E_INVALID_ATTR = 1014,
  E_LAYOUT_SIZE = 1015,
  E_INVALID_FFI = 1016,

  /* Warnings (W2xxx) */
  W_UNUSED = 2001,
  W_SHADOWING = 2002,
  W_DEPRECATED = 2003,
  W_IMPLICIT_COERCE = 2004,
  W_DIV_ZERO = 2005,
} diag_code_t;

/* ── dedup tables ──────────────────────────────────────────────────── */
typedef struct {
  char *key;
  int count;
} diag_entry_t;

static diag_entry_t *g_diag_seen_tbl = NULL;
static size_t g_diag_seen_cap = 0;
static size_t g_diag_seen_len = 0;

/* ── last-primary tracking ─────────────────────────────────────────── */
static bool g_last_primary_emitted = false;
static int g_warn_level = 1; /* 0=none, 1=useful, 2=all */
static bool g_diag_compact_mode = false;

/* ── source cache ──────────────────────────────────────────────────── */
static char *g_diag_cached_file = NULL;
static char *g_diag_cached_src = NULL;

static const char *diag_token_filename(token_t tok) {
  const char *filename = tok.filename;
  if (!filename)
    return NULL;
  return ny_intern_contains_ptr(filename) ? filename : NULL;
}

static const char *diag_token_filename_or_unknown(token_t tok) {
  const char *filename = diag_token_filename(tok);
  return filename ? filename : "unknown";
}

void ny_diag_configure(int warn_level, bool compact_mode) {
  if (warn_level < 0)
    warn_level = 0;
  if (warn_level > 2)
    warn_level = 2;
  g_warn_level = warn_level;
  g_diag_compact_mode = compact_mode;
}

/* ── source loading ────────────────────────────────────────────────── */
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
  const char *filename = diag_token_filename(tok);
  if (!filename || filename[0] == '<' || tok.line <= 0 || tok.col <= 0)
    return;
  const char *src = diag_load_source(filename);
  if (!src)
    return;
  ny_print_snippet(src, tok.line, tok.col, tok.len, color);
}

/* ── hash table ────────────────────────────────────────────────────── */
static uint64_t diag_hash(const char *s) { return ny_hash64_cstr(s); }

static bool diag_tbl_grow(void) {
  size_t old_cap = g_diag_seen_cap;
  diag_entry_t *old_tbl = g_diag_seen_tbl;
  g_diag_seen_cap = g_diag_seen_cap ? g_diag_seen_cap * 2 : 2048;
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
  if (g_diag_seen_cap == 0 || (g_diag_seen_len + 1) * 3 >= g_diag_seen_cap * 2) {
    if (!diag_tbl_grow())
      return true;
  }
  uint64_t h = diag_hash(key);
  size_t mask = g_diag_seen_cap - 1;
  size_t idx = (size_t)h & mask;
  while (g_diag_seen_tbl[idx].key) {
    if (strcmp(g_diag_seen_tbl[idx].key, key) == 0) {
      g_diag_seen_tbl[idx].count++;
      return g_diag_seen_tbl[idx].count <= 4;
    }
    idx = (idx + 1) & mask;
  }
  g_diag_seen_tbl[idx].key = ny_strdup(key);
  g_diag_seen_tbl[idx].count = 1;
  g_diag_seen_len++;
  return true;
}

/* ── public dedup gate ─────────────────────────────────────────────── */
bool ny_diag_should_emit(const char *kind, token_t tok, const char *name) {
  char key[512];
  const char *file = diag_token_filename_or_unknown(tok);
  snprintf(key, sizeof(key), "%s|%s|%d|%d|%s", kind ? kind : "diag", file, tok.line, tok.col,
           name ? name : "");
  return diag_mark_seen(key);
}

/* ── stdlib token check ────────────────────────────────────────────── */
bool ny_is_stdlib_tok(token_t tok) {
  const char *filename = diag_token_filename(tok);
  if (!filename)
    return false;
  bool res = false;
  if (filename[0] == '<') {
    res = strcmp(filename, "<stdlib>") == 0 || strcmp(filename, "<repl_std>") == 0;
  } else {
    char norm[4096];
    size_t i = 0;
    for (; filename[i] && i + 1 < sizeof(norm); ++i)
      norm[i] = filename[i] == '\\' ? '/' : filename[i];
    norm[i] = '\0';
    if (strncmp(norm, "lib/", 4) == 0 ||
        strstr(norm, "/nytrix/lib/") != NULL ||
        strstr(norm, "/nytrix/nytrix/lib/") != NULL ||
        strstr(norm, "std.ny") != NULL ||
        strstr(norm, "/share/nytrix/") != NULL ||
        strstr(norm, "/lib/nytrix/std/") != NULL) {
      res = true;
    }
  }
  return res;
}

bool ny_strict_error_enabled(codegen_t *cg, token_t tok) {
  return cg->strict_diagnostics && !ny_is_stdlib_tok(tok);
}

static bool ny_warning_code_is_noisy(int code) {
  return code == (int)W_SHADOWING;
}

/* ── unique primary gate ───────────────────────────────────────────── */
static bool ny_diag_emit_unique(const char *level, token_t tok, const char *rendered) {
  char key[1536];
  const char *file = diag_token_filename_or_unknown(tok);
  int line = tok.line < 0 ? 0 : tok.line;
  int col = tok.col < 0 ? 0 : tok.col;
  snprintf(key, sizeof(key), "line|%s|%s|%d|%d|%s", level, file, line, col,
           rendered ? rendered : "");
  return diag_mark_seen(key);
}

/* ── primary message emitter ───────────────────────────────────────── */
static void ny_diag_primary(const char *label, const char *code, const char *label_color,
                            const char *code_color, token_t tok, const char *fmt, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  char rendered[1024];
  vsnprintf(rendered, sizeof(rendered), fmt, cp);
  va_end(cp);

  if (!ny_diag_emit_unique(label, tok, rendered)) {
    g_last_primary_emitted = false;
    return;
  }

  const char *file = diag_token_filename_or_unknown(tok);
  int line = tok.line < 0 ? 0 : tok.line;
  int col = tok.col < 0 ? 0 : tok.col;

  if (g_diag_compact_mode) {
    fprintf(stderr, "%s:%d:%d: [%s] %s: %s\n", file, line, col, code, label, rendered);
  } else {
    /* file:line:col:  [EXXXX]  label:  message */
    fprintf(stderr, "%s:%d:%d:  %s[%s]%s %s%s:%s %s\n", file, line, col, clr(code_color), code,
            clr(NY_CLR_RESET), clr(label_color), label, clr(NY_CLR_RESET), rendered);
    diag_print_snippet(tok, label_color);
  }

  g_last_primary_emitted = true;
}

/* ── attachable secondary messages ─────────────────────────────────── */
static void ny_secondary(const char *label, const char *label_color, const char *rendered) {
  if (!g_last_primary_emitted)
    return;
  if (g_diag_compact_mode)
    fprintf(stderr, "       %s %s\n", label, rendered);
  else
    fprintf(stderr, "       %s%s%s  %s\n", clr(label_color), label, clr(NY_CLR_RESET), rendered);
}

/* ── public API ────────────────────────────────────────────────────── */

void ny_diag_error(token_t tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ny_diag_primary("error", "E1001", NY_CLR_RED, NY_CLR_CYAN, tok, fmt, ap);
  va_end(ap);
}

void ny_diag_warning(token_t tok, const char *fmt, ...) {
  if (g_warn_level <= 0) {
    g_last_primary_emitted = false;
    return;
  }
  if (g_warn_level <= 1 && ny_is_stdlib_tok(tok)) {
    g_last_primary_emitted = false;
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  ny_diag_primary("warning", "W2000", NY_CLR_YELLOW, NY_CLR_CYAN, tok, fmt, ap);
  va_end(ap);
}

void ny_diag_error_code(token_t tok, int code, const char *fmt, ...) {
  char code_buf[16];
  if (code <= 0)
    code = (int)E_SYNTAX;
  snprintf(code_buf, sizeof(code_buf), "E%04d", code);
  va_list ap;
  va_start(ap, fmt);
  ny_diag_primary("error", code_buf, NY_CLR_RED, NY_CLR_CYAN, tok, fmt, ap);
  va_end(ap);
}

void ny_diag_warning_code(token_t tok, int code, const char *fmt, ...) {
  if (g_warn_level <= 0) {
    g_last_primary_emitted = false;
    return;
  }
  if (g_warn_level <= 1 && (ny_warning_code_is_noisy(code) || ny_is_stdlib_tok(tok))) {
    g_last_primary_emitted = false;
    return;
  }
  char code_buf[16];
  if (code <= 0)
    code = (int)W_UNUSED;
  snprintf(code_buf, sizeof(code_buf), "W%04d", code);
  va_list ap;
  va_start(ap, fmt);
  ny_diag_primary("warning", code_buf, NY_CLR_YELLOW, NY_CLR_CYAN, tok, fmt, ap);
  va_end(ap);
}

void ny_diag_hint(const char *fmt, ...) {
  if (!g_last_primary_emitted)
    return;
  va_list ap;
  va_start(ap, fmt);
  char rendered[1024];
  vsnprintf(rendered, sizeof(rendered), fmt, ap);
  va_end(ap);
  char key[1200];
  snprintf(key, sizeof(key), "hint|%s", rendered);
  if (!diag_mark_seen(key))
    return;
  ny_secondary("hint:", NY_CLR_YELLOW, rendered);
}

void ny_diag_fix(const char *fmt, ...) {
  if (!g_last_primary_emitted)
    return;
  va_list ap;
  va_start(ap, fmt);
  char rendered[1024];
  vsnprintf(rendered, sizeof(rendered), fmt, ap);
  va_end(ap);
  char key[1200];
  snprintf(key, sizeof(key), "fix|%s", rendered);
  if (!diag_mark_seen(key))
    return;
  ny_secondary("fix:", NY_CLR_GREEN, rendered);
}

void ny_diag_note_tok(token_t tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char rendered[1024];
  vsnprintf(rendered, sizeof(rendered), fmt, ap);
  va_end(ap);
  const char *file = diag_token_filename_or_unknown(tok);
  int line = tok.line < 0 ? 0 : tok.line;
  int col = tok.col < 0 ? 0 : tok.col;
  char key[1536];
  snprintf(key, sizeof(key), "note|%s|%d|%d|%s", file, line, col, rendered);
  if (!diag_mark_seen(key))
    return;
  fprintf(stderr, "%s:%d:%d: %s[%s]%s %s\n", file, line, col, clr(NY_CLR_CYAN), "note",
          clr(NY_CLR_RESET), rendered);
}

/* ── convenience helpers ───────────────────────────────────────────── */

void ny_diag_error_context(token_t tok, const char *primary_msg, const char *context,
                           const char *suggestion) {
  ny_diag_error(tok, "%s", primary_msg);
  if (context && *context)
    ny_diag_hint("%s", context);
  if (suggestion && *suggestion)
    ny_diag_fix("%s", suggestion);
}

void ny_diag_type_mismatch(token_t tok, const char *expected, const char *got,
                           const char *context) {
  ny_diag_error(tok, "type mismatch: expected %s'%s'%s, got %s'%s'%s", clr(NY_CLR_BOLD), expected,
                clr(NY_CLR_RESET), clr(NY_CLR_BOLD), got, clr(NY_CLR_RESET));
  if (context && *context)
    ny_diag_hint("in %s", context);

  /* Smart type-conversion suggestions */
  if (strcmp(expected, "int") == 0 && strcmp(got, "f64") == 0) {
    ny_diag_fix("Use %strunc(x)%s or %sfloor(x)%s to convert f64 → int", clr(NY_CLR_BOLD),
                clr(NY_CLR_RESET), clr(NY_CLR_BOLD), clr(NY_CLR_RESET));
  } else if (strcmp(expected, "f64") == 0 && strcmp(got, "int") == 0) {
    ny_diag_fix("Use %sf64(x)%s to convert int → f64", clr(NY_CLR_BOLD), clr(NY_CLR_RESET));
  } else if (strstr(expected, "list") && strstr(got, "dict")) {
    ny_diag_fix("Use %slist(d)%s (keys) or %svalues(d)%s to convert dict → list", clr(NY_CLR_BOLD),
                clr(NY_CLR_RESET), clr(NY_CLR_BOLD), clr(NY_CLR_RESET));
  } else if (strcmp(expected, "str") == 0 && strcmp(got, "int") == 0) {
    ny_diag_fix("Use %sto_str(x)%s to convert int → string", clr(NY_CLR_BOLD), clr(NY_CLR_RESET));
  } else if (strcmp(expected, "int") == 0 && strcmp(got, "str") == 0) {
    ny_diag_fix("Use %satoi(s)%s to convert string → int", clr(NY_CLR_BOLD), clr(NY_CLR_RESET));
  } else {
    ny_diag_hint("Consider whether a type conversion or different variable is needed");
  }
}

void ny_diag_error_with_context(token_t tok, const char *primary_msg, const char *common_cause,
                                const char *fix_suggestion) {
  ny_diag_error(tok, "%s", primary_msg);
  if (common_cause && *common_cause)
    ny_diag_hint("%s", common_cause);
  if (fix_suggestion && *fix_suggestion)
    ny_diag_fix("%s", fix_suggestion);
}
