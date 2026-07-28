#include "cscan.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t start;
  size_t end;
  int line;
  int kind;
} CScanToken;

enum { CSCAN_IDENT, CSCAN_PUNCT };

static int cscan_push(CScanFunctions *out, const CScanFunction *fn) {
  if (out->len == out->cap) {
    size_t cap = out->cap ? out->cap * 2 : 32;
    void *items = realloc(out->items, cap * sizeof(*out->items));
    if (!items)
      return 0;
    out->items = items;
    out->cap = cap;
  }
  out->items[out->len++] = *fn;
  return 1;
}

static int cscan_is_control(const char *src, const CScanToken *tok) {
  static const char *words[] = {"if", "for", "while", "switch", "return", "sizeof", "defined", NULL};
  size_t n = tok->end - tok->start;
  for (int i = 0; words[i]; i++)
    if (strlen(words[i]) == n && memcmp(src + tok->start, words[i], n) == 0)
      return 1;
  return 0;
}

static int cscan_is_static(const char *src, const CScanToken *tokens, size_t from, size_t to) {
  for (size_t i = from; i < to; i++) {
    size_t n = tokens[i].end - tokens[i].start;
    if (tokens[i].kind == CSCAN_IDENT && n == 6 && memcmp(src + tokens[i].start, "static", 6) == 0)
      return 1;
  }
  return 0;
}

static int cscan_candidate(const char *src, const CScanToken *tokens, size_t count,
                           size_t decl_start, size_t close, size_t *open_out,
                           size_t *name_out) {
  if (close == 0 || tokens[close].kind != CSCAN_PUNCT || src[tokens[close].start] != ')')
    return 0;
  int depth = 0;
  size_t open = close;
  for (;;) {
    char ch = src[tokens[open].start];
    if (ch == ')')
      depth++;
    else if (ch == '(' && --depth == 0)
      break;
    if (open == decl_start)
      return 0;
    open--;
  }
  if (open == 0 || tokens[open - 1].kind != CSCAN_IDENT || cscan_is_control(src, &tokens[open - 1]))
    return 0;
  /* A definition needs a declaration prefix. This rejects call continuations
   * such as strcmp(...) == 0) { without trying to parse C types. */
  if (open < 2 || open - 1 <= decl_start)
    return 0;
  for (size_t i = decl_start; i < open - 1; i++)
    if (tokens[i].kind == CSCAN_PUNCT && src[tokens[i].start] == '=')
      return 0;
  *open_out = open;
  *name_out = open - 1;
  (void)count;
  return 1;
}

void cscan_functions_free(CScanFunctions *out) {
  if (!out)
    return;
  free(out->items);
  memset(out, 0, sizeof(*out));
}

int cscan_functions(const char *src, size_t len, CScanFunctions *out) {
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  if (!src)
    return len == 0;

  CScanToken *tokens = NULL;
  size_t token_len = 0, token_cap = 0, decl_start = 0;
  size_t i = 0;
  int line = 1, brace_depth = 0, directive = 0, line_start = 1;
  size_t active_body = (size_t)-1;
  while (i < len) {
    char ch = src[i], next = i + 1 < len ? src[i + 1] : '\0';
    if (ch == '\n') {
      line++;
      line_start = 1;
      if (directive && (i == 0 || src[i - 1] != '\\'))
        directive = 0;
      i++;
      continue;
    }
    if (line_start && isspace((unsigned char)ch)) { i++; continue; }
    if (line_start && ch == '#') { directive = 1; line_start = 0; i++; continue; }
    line_start = 0;
    if (directive) { i++; continue; }
    if (ch == '/' && next == '/') { while (i < len && src[i] != '\n') i++; continue; }
    if (ch == '/' && next == '*') {
      i += 2;
      while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) {
        if (src[i++] == '\n') { line++; line_start = 1; }
      }
      if (i + 1 >= len) { out->malformed = 1; break; }
      i += 2;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      char quote = ch;
      i++;
      while (i < len) {
        if (src[i] == '\\' && i + 1 < len) { i += 2; continue; }
        if (src[i] == quote) { i++; break; }
        if (src[i++] == '\n') { line++; line_start = 1; }
      }
      if (i >= len && src[len - 1] != quote) out->malformed = 1;
      continue;
    }
    if (isspace((unsigned char)ch)) { i++; continue; }
    if (token_len == token_cap) {
      size_t cap = token_cap ? token_cap * 2 : 256;
      void *p = realloc(tokens, cap * sizeof(*tokens));
      if (!p) { free(tokens); cscan_functions_free(out); return 0; }
      tokens = p; token_cap = cap;
    }
    CScanToken *tok = &tokens[token_len++];
    tok->start = i; tok->line = line; tok->kind = CSCAN_PUNCT;
    if (isalpha((unsigned char)ch) || ch == '_') {
      tok->kind = CSCAN_IDENT;
      while (i < len && (isalnum((unsigned char)src[i]) || src[i] == '_')) i++;
    } else i++;
    tok->end = i;
    if (tok->kind != CSCAN_PUNCT)
      continue;
    if (ch == '{' && brace_depth == 0) {
      size_t open = 0, name = 0;
      if (cscan_candidate(src, tokens, token_len - 1, decl_start, token_len - 2, &open, &name)) {
        CScanFunction fn;
        memset(&fn, 0, sizeof(fn));
        size_t n = tokens[name].end - tokens[name].start;
        if (n >= sizeof(fn.name)) n = sizeof(fn.name) - 1;
        memcpy(fn.name, src + tokens[name].start, n);
        fn.start_offset = tokens[decl_start].start;
        fn.start_line = tokens[decl_start].line;
        fn.is_static = cscan_is_static(src, tokens, decl_start, name);
        if (!cscan_push(out, &fn)) { free(tokens); cscan_functions_free(out); return 0; }
        active_body = out->len - 1;
      }
      brace_depth++;
    } else if (ch == '{') {
      brace_depth++;
    } else if (ch == '}') {
      if (brace_depth > 0) brace_depth--;
      else out->malformed = 1;
      if (brace_depth == 0 && active_body != (size_t)-1) {
        out->items[active_body].end_offset = i;
        out->items[active_body].end_line = line;
        active_body = (size_t)-1;
        decl_start = token_len;
      }
    } else if (ch == ';' && brace_depth == 0) {
      decl_start = token_len;
    }
  }
  if (brace_depth != 0 || active_body != (size_t)-1) {
    out->malformed = 1;
    if (active_body != (size_t)-1)
      out->len = active_body;
  }
  free(tokens);
  return 1;
}
