/*
 * Lightweight C scanner: finds complete function definitions for formatter
 * audit and clone analysis.  It is deliberately a scanner, not a parser: it
 * tokenizes C well enough to attribute declarations, then validates the
 * declarator immediately preceding a top-of-scope `{`.
 */
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

enum { CSCAN_IDENT, CSCAN_PUNCT, CSCAN_NUMBER, CSCAN_STRING };

typedef struct {
  CScanToken *items;
  size_t len;
  size_t cap;
} CScanTokenVec;

typedef struct {
  size_t fn_index;
  int depth;
} CScanActive;

typedef struct {
  CScanActive *items;
  size_t len;
  size_t cap;
} CScanActiveVec;

static int cs_push(CScanFunctions *out, const CScanFunction *fn) {
  if (out->len == out->cap) {
    size_t cap = out->cap ? out->cap * 2 : 32;
    if (cap < out->cap || cap > SIZE_MAX / sizeof(*out->items))
      return 0;
    void *items = realloc(out->items, cap * sizeof(*out->items));
    if (!items)
      return 0;
    out->items = items;
    out->cap = cap;
  }
  out->items[out->len++] = *fn;
  return 1;
}

static int cs_tok_push(CScanTokenVec *v, const CScanToken *tok) {
  if (v->len == v->cap) {
    size_t cap = v->cap ? v->cap * 2 : 256;
    if (cap < v->cap || cap > SIZE_MAX / sizeof(*v->items))
      return 0;
    void *items = realloc(v->items, cap * sizeof(*v->items));
    if (!items)
      return 0;
    v->items = items;
    v->cap = cap;
  }
  v->items[v->len++] = *tok;
  return 1;
}

static int cs_active_push(CScanActiveVec *v, size_t fn_index, int depth) {
  if (v->len == v->cap) {
    size_t cap = v->cap ? v->cap * 2 : 16;
    if (cap < v->cap || cap > SIZE_MAX / sizeof(*v->items))
      return 0;
    void *items = realloc(v->items, cap * sizeof(*v->items));
    if (!items)
      return 0;
    v->items = items;
    v->cap = cap;
  }
  v->items[v->len].fn_index = fn_index;
  v->items[v->len].depth = depth;
  v->len++;
  return 1;
}

static int cs_word_eq(const char *src, const CScanToken *tok, const char *word) {
  size_t n = tok->end - tok->start;
  size_t w = strlen(word);
  return tok->kind == CSCAN_IDENT && n == w &&
         memcmp(src + tok->start, word, w) == 0;
}

static int cs_is_keyword(const char *src, const CScanToken *tok) {
  static const char *const words[] = {
      "auto",     "break",  "case",     "char",   "const",    "continue",
      "default",  "do",     "double",   "else",   "enum",     "extern",
      "float",    "for",    "goto",     "if",     "inline",   "int",
      "long",     "register", "restrict", "return", "short",  "signed",
      "sizeof",   "static", "struct",   "switch", "typedef",  "union",
      "unsigned", "void",   "volatile", "while",  "_Alignas", "_Alignof",
      "_Atomic",  "_Bool",  "_Complex", "_Generic", "_Imaginary", "_Noreturn",
      "_Static_assert", "_Thread_local", NULL};
  for (size_t i = 0; words[i]; ++i)
    if (cs_word_eq(src, tok, words[i]))
      return 1;
  return 0;
}

static int cs_is_control(const char *src, const CScanToken *tok) {
  static const char *const words[] = {"if",     "for",    "while",
                                      "switch", "return", "sizeof",
                                      "defined", "do",    "else",
                                      "case",   "goto",   "typedef",
                                      NULL};
  for (size_t i = 0; words[i]; ++i)
    if (cs_word_eq(src, tok, words[i]))
      return 1;
  return 0;
}

static int cs_is_attribute(const char *src, const CScanToken *tok) {
  static const char *const words[] = {"__attribute__", "__attribute",
                                      "__declspec",    "__asm__",
                                      "asm",           "noexcept",
                                      "_Alignas",      NULL};
  for (size_t i = 0; words[i]; ++i)
    if (cs_word_eq(src, tok, words[i]))
      return 1;
  return 0;
}

static int cs_punct(const CScanToken *tok, char ch) {
  (void)ch;
  return tok->kind == CSCAN_PUNCT && tok->end == tok->start + 1 &&
         tok->start < tok->end;
}

static int cs_punct_at(const char *src, const CScanToken *tok, char ch) {
  return cs_punct(tok, ch) && src[tok->start] == ch;
}

/*
 * Skip a balanced (), [] or {} group; *pi points at the opening token.
 */
static int cs_skip_group(const char *src, const CScanToken *tokens, size_t n,
                         size_t *pi, char open_ch, char close_ch) {
  size_t i = *pi;
  if (i >= n || !cs_punct_at(src, &tokens[i], open_ch))
    return 0;
  int depth = 0;
  while (i < n) {
    if (cs_punct_at(src, &tokens[i], open_ch)) {
      depth++;
    } else if (cs_punct_at(src, &tokens[i], close_ch)) {
      depth--;
      if (depth == 0) {
        *pi = i + 1;
        return 1;
      }
    }
    i++;
  }
  return 0;
}

/*
 * Parse a C declarator.  Recognizes pointers, parenthesized declarators and
 * suffixes (parameter lists, arrays, attributes).  The first parameter-list
 * suffix attached to the declared name is the function's parameter list; for
 * `int (*factory(void))(int)` that is `(void)`, not `(int)`.
 */
static int cs_declarator(const char *src, const CScanToken *tokens, size_t n,
                         size_t *pi, size_t end, size_t *name,
                         size_t *params_open, size_t *params_close) {
  size_t i = *pi;
  while (i < end && cs_punct_at(src, &tokens[i], '*'))
    i++;
  if (i >= end)
    return 0;
  if (cs_punct_at(src, &tokens[i], '(')) {
    i++;
    if (!cs_declarator(src, tokens, n, &i, end, name, params_open,
                       params_close))
      return 0;
    if (i >= end || !cs_punct_at(src, &tokens[i], ')'))
      return 0;
    i++;
  } else if (tokens[i].kind == CSCAN_IDENT &&
             !cs_is_keyword(src, &tokens[i])) {
    *name = i;
    i++;
  } else {
    return 0;
  }

  for (;;) {
    if (i >= end)
      break;
    if (cs_punct_at(src, &tokens[i], '[')) {
      if (!cs_skip_group(src, tokens, n, &i, '[', ']'))
        return 0;
      continue;
    }
    if (cs_punct_at(src, &tokens[i], '(')) {
      size_t open = i;
      if (!cs_skip_group(src, tokens, n, &i, '(', ')'))
        return 0;
      if (*params_open == SIZE_MAX) {
        *params_open = open;
        *params_close = i - 1;
      }
      continue;
    }
    if (tokens[i].kind == CSCAN_IDENT && cs_is_attribute(src, &tokens[i])) {
      i++;
      if (i < end && cs_punct_at(src, &tokens[i], '(')) {
        if (!cs_skip_group(src, tokens, n, &i, '(', ')'))
          return 0;
      }
      continue;
    }
    break;
  }
  *pi = i;
  return 1;
}

static int cs_make_function(const char *src, const CScanToken *tokens,
                            size_t token_len, size_t decl_start,
                            size_t body_index, int nesting_depth,
                            CScanFunction *out) {
  if (body_index <= decl_start || decl_start >= token_len)
    return 0;
  if (cs_is_control(src, &tokens[decl_start]))
    return 0;

  for (size_t i = decl_start; i < body_index; ++i) {
    if (cs_punct_at(src, &tokens[i], ';') ||
        cs_punct_at(src, &tokens[i], '='))
      return 0;
  }

  for (size_t k = decl_start; k < body_index; ++k) {
    if (tokens[k].kind == CSCAN_IDENT && cs_is_keyword(src, &tokens[k]))
      continue;
    size_t name = SIZE_MAX, params_open = SIZE_MAX, params_close = SIZE_MAX;
    size_t pos = k;
    if (!cs_declarator(src, tokens, token_len, &pos, body_index, &name,
                       &params_open, &params_close))
      continue;
    if (name == SIZE_MAX || params_open == SIZE_MAX || pos != body_index)
      continue;
    if (k == decl_start)
      continue;
    int bad_prefix = 0;
    for (size_t i = decl_start; i < k; ++i) {
      if (cs_is_control(src, &tokens[i])) {
        bad_prefix = 1;
        break;
      }
    }
    if (bad_prefix)
      continue;

    memset(out, 0, sizeof(*out));
    size_t name_n = tokens[name].end - tokens[name].start;
    if (name_n >= sizeof(out->name))
      name_n = sizeof(out->name) - 1;
    memcpy(out->name, src + tokens[name].start, name_n);
    out->name[name_n] = '\0';
    out->start_offset = tokens[decl_start].start;
    out->start_line = tokens[decl_start].line;
    out->end_offset = tokens[body_index].start + 1; /* provisional */
    out->end_line = tokens[body_index].line;
    out->name_offset = tokens[name].start;
    out->body_open_offset = tokens[body_index].start;
    out->body_close_offset = tokens[body_index].start;
    out->nesting_depth = nesting_depth;
    out->is_static = 0;
    for (size_t i = decl_start; i < name; ++i)
      if (cs_word_eq(src, &tokens[i], "static")) {
        out->is_static = 1;
        break;
      }
    return 1;
  }
  return 0;
}

static int cs_punct_len(const char *src, size_t i, size_t n, size_t *len_out) {
  static const char *const three[] = {"...", "<<=", ">>=", NULL};
  static const char *const two[] = {"->", "++", "--", "<<", ">>", "<=",
                                    ">=", "==", "!=", "&&", "||", "+=",
                                    "-=", "*=", "/=", "%=", "&=", "|=",
                                    "^=", "##", NULL};
  for (size_t k = 0; three[k]; ++k) {
    if (i + 3 <= n && memcmp(src + i, three[k], 3) == 0) {
      *len_out = 3;
      return 1;
    }
  }
  for (size_t k = 0; two[k]; ++k) {
    if (i + 2 <= n && memcmp(src + i, two[k], 2) == 0) {
      *len_out = 2;
      return 1;
    }
  }
  *len_out = 1;
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

  CScanTokenVec tokens = {0};
  CScanActiveVec active = {0};
  size_t i = 0;
  int line = 1;
  int bol = 1;
  int brace_depth = 0;
  size_t decl_start = 0;
  int ok = 1;

  while (i < len && ok) {
    char ch = src[i];
    char next = i + 1 < len ? src[i + 1] : '\0';
    if (ch == '\\' && next == '\n') {
      i += 2;
      line++;
      bol = 1;
      continue;
    }
    if (ch == '\n') {
      line++;
      bol = 1;
      i++;
      continue;
    }
    if (isspace((unsigned char)ch)) {
      i++;
      continue;
    }
    if (ch == '/' && next == '/') {
      while (i < len && src[i] != '\n')
        i++;
      bol = 0;
      continue;
    }
    if (ch == '/' && next == '*') {
      i += 2;
      while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) {
        if (src[i] == '\n')
          line++;
        i++;
      }
      if (i + 1 >= len) {
        out->malformed = 1;
        break;
      }
      i += 2;
      bol = 0;
      continue;
    }
    if (ch == '#' && bol) {
      while (i < len && src[i] != '\n') {
        if (src[i] == '\\' && i + 1 < len && src[i + 1] == '\n') {
          i += 2;
          line++;
        } else {
          i++;
        }
      }
      bol = 1;
      continue;
    }

    CScanToken tok;
    memset(&tok, 0, sizeof(tok));
    tok.start = i;
    tok.line = line;
    tok.kind = CSCAN_PUNCT;

    if (ch == '"' || ch == '\'') {
      char quote = ch;
      tok.kind = CSCAN_STRING;
      i++;
      while (i < len) {
        if (src[i] == '\\' && i + 1 < len) {
          if (src[i + 1] == '\n')
            line++;
          i += 2;
          continue;
        }
        if (src[i] == quote) {
          i++;
          break;
        }
        if (src[i] == '\n') {
          line++;
          out->malformed = 1;
          break;
        }
        i++;
      }
      tok.end = i;
      bol = 0;
      if (!cs_tok_push(&tokens, &tok)) {
        ok = 0;
        break;
      }
      continue;
    }
    if (isalpha((unsigned char)ch) || ch == '_' || (unsigned char)ch >= 0x80) {
      tok.kind = CSCAN_IDENT;
      while (i < len && (isalnum((unsigned char)src[i]) || src[i] == '_' ||
                         (unsigned char)src[i] >= 0x80))
        i++;
      tok.end = i;
      bol = 0;
      if (!cs_tok_push(&tokens, &tok)) {
        ok = 0;
        break;
      }
      continue;
    }
    if (isdigit((unsigned char)ch) ||
        (ch == '.' && isdigit((unsigned char)next))) {
      tok.kind = CSCAN_NUMBER;
      while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '_' || c == '.' || c == '\'') {
          i++;
          continue;
        }
        if ((c == '+' || c == '-') && i > tok.start &&
            (src[i - 1] == 'e' || src[i - 1] == 'E' || src[i - 1] == 'p' ||
             src[i - 1] == 'P')) {
          i++;
          continue;
        }
        break;
      }
      tok.end = i;
      bol = 0;
      if (!cs_tok_push(&tokens, &tok)) {
        ok = 0;
        break;
      }
      continue;
    }

    size_t punct_len = 1;
    cs_punct_len(src, i, len, &punct_len);
    i += punct_len;
    tok.end = i;
    bol = 0;
    /*
     * Push the punctuator before attributing a body.  `cs_make_function`
     * reads the `{` token for its provisional byte span, so it must already
     * be present in the token vector.
     */
    if (!cs_tok_push(&tokens, &tok)) {
      ok = 0;
      break;
    }
    size_t tok_index = tokens.len - 1;
    if (punct_len == 1 && ch == '{') {
      CScanFunction fn;
      if (cs_make_function(src, tokens.items, tokens.len, decl_start,
                           tok_index, brace_depth, &fn)) {
        if (!cs_push(out, &fn) ||
            !cs_active_push(&active, out->len - 1, brace_depth + 1)) {
          ok = 0;
          break;
        }
      }
      brace_depth++;
      decl_start = tokens.len;
    } else if (punct_len == 1 && ch == '}') {
      if (brace_depth == 0) {
        out->preprocessing_ambiguous = 1;
      } else {
        if (active.len &&
            active.items[active.len - 1].depth == brace_depth) {
          CScanFunction *fn = &out->items[active.items[active.len - 1].fn_index];
          fn->end_offset = i;
          fn->end_line = line;
          fn->body_close_offset = tokens.items[tok_index].start;
          active.len--;
        }
        brace_depth--;
      }
      decl_start = tokens.len;
    } else if (punct_len == 1 && ch == ';') {
      decl_start = tokens.len;
    }
  }

  if (active.len) {
    out->malformed = 1;
    out->len = active.items[0].fn_index;
  } else if (brace_depth != 0) {
    /*
     * Raw conditional-compilation branches can contribute braces that do not
     * belong to one configured translation unit.  Keep every complete
     * function found, but tell callers the raw analysis view is ambiguous.
     */
    out->preprocessing_ambiguous = 1;
  }
  free(active.items);
  free(tokens.items);
  return ok && out != NULL;
}
