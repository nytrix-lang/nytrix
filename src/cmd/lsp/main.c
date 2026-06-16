#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/intern.h"
#include "base/loader.h"
#include "base/util.h"
#include "parse/parser.h"
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static ssize_t read_exact(int fd, void *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = read(fd, (char *)buf + total, len - total);
    if (n <= 0)
      return total ? (ssize_t)total : n;
    total += (size_t)n;
  }
  return (ssize_t)total;
}

static ssize_t read_header_line(char *buf, size_t cap) {
  size_t idx = 0;
  while (idx + 1 < cap) {
    char c;
    ssize_t n = read_exact(STDIN_FILENO, &c, 1);
    if (n <= 0)
      return -1;
    if (c == '\r')
      continue;
    if (c == '\n') {
      buf[idx] = '\0';
      return (ssize_t)idx;
    }
    buf[idx++] = c;
  }
  buf[idx] = '\0';
  return (ssize_t)idx;
}

static char *read_message(void) {
  char line[256];
  ssize_t len = 0;
  ssize_t content_len = 0;
  while ((len = read_header_line(line, sizeof(line))) >= 0) {
    if (len == 0)
      break;
    if (strncasecmp(line, "Content-Length:", 15) == 0) {
      content_len = atoi(line + 15);
    }
  }
  if (content_len <= 0)
    return NULL;

  if (content_len > 10 * 1024 * 1024)
    return NULL;
  char *body = malloc((size_t)content_len + 1);
  if (!body)
    return NULL;
  if (read_exact(STDIN_FILENO, body, (size_t)content_len) <= 0) {
    free(body);
    return NULL;
  }
  body[content_len] = '\0';
  return body;
}

static char *json_decode_string(const char *start, size_t len) {
  char *out = malloc(len + 1);
  if (!out)
    return NULL;
  size_t o = 0;
  for (size_t i = 0; i < len; ++i) {
    char c = start[i];
    if (c == '\\' && i + 1 < len) {
      ++i;
      char esc = start[i];
      switch (esc) {
      case '"':
        out[o++] = '"';
        break;
      case '\\':
        out[o++] = '\\';
        break;
      case '/':
        out[o++] = '/';
        break;
      case 'b':
        out[o++] = '\b';
        break;
      case 'f':
        out[o++] = '\f';
        break;
      case 'n':
        out[o++] = '\n';
        break;
      case 'r':
        out[o++] = '\r';
        break;
      case 't':
        out[o++] = '\t';
        break;
      default:
        out[o++] = esc;
        break;
      }
    } else {
      out[o++] = c;
    }
  }
  out[o] = '\0';
  return out;
}

static char *json_extract_string(const char *json, const char *key) {
  if (!json || !key)
    return NULL;
  char pattern[128];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *pos = strstr(json, pattern);
  if (!pos)
    return NULL;
  const char *colon = strchr(pos + strlen(pattern), ':');
  if (!colon)
    return NULL;
  const char *quote = strchr(colon, '"');
  if (!quote)
    return NULL;
  const char *start = quote + 1;
  const char *end = start;
  while (*end && (*end != '"' || *(end - 1) == '\\'))
    end++;
  size_t len = (size_t)(end - start);
  return json_decode_string(start, len);
}

static char *json_extract_string_near(const char *json, const char *needle, const char *key) {
  if (!json)
    return NULL;
  const char *section = strstr(json, needle);
  if (section)
    return json_extract_string(section, key);
  return json_extract_string(json, key);
}

static char *json_extract_id(const char *json) {
  if (!json)
    return NULL;
  const char *pos = strstr(json, "\"id\"");
  if (!pos)
    return NULL;
  const char *colon = strchr(pos, ':');
  if (!colon)
    return NULL;
  const char *p = colon + 1;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p == '"') {
    const char *start = p++;
    while (*p && *p != '"') {
      if (*p == '\\' && p[1])
        p += 2;
      else
        p++;
    }
    if (*p == '"')
      p++;
    size_t len = (size_t)(p - start);
    char *out = malloc(len + 1);
    if (!out)
      return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
  }
  const char *start = p;
  while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p))
    p++;
  size_t len = (size_t)(p - start);
  char *out = malloc(len + 1);
  if (!out)
    return NULL;
  memcpy(out, start, len);
  out[len] = '\0';
  return out;
}

static bool json_extract_int_near(const char *json, const char *needle, const char *key,
                                  int *out) {
  if (!json || !key || !out)
    return false;
  const char *section = needle ? strstr(json, needle) : json;
  if (!section)
    section = json;
  char pattern[128];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *pos = strstr(section, pattern);
  if (!pos)
    return false;
  const char *colon = strchr(pos + strlen(pattern), ':');
  if (!colon)
    return false;
  const char *p = colon + 1;
  while (*p && isspace((unsigned char)*p))
    p++;
  *out = atoi(p);
  return true;
}

static void send_response(const char *json) {
  if (!json)
    return;
  char header[64];
  int body_len = (int)strlen(json);
  int header_len = snprintf(header, sizeof(header), "Content-Length: %d\r\n\r\n", body_len);
  write(STDOUT_FILENO, header, (size_t)header_len);
  write(STDOUT_FILENO, json, (size_t)body_len);
  fsync(STDOUT_FILENO);
}

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} sbuf_t;

static void sb_reserve(sbuf_t *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap)
    return;
  size_t nc = b->cap ? b->cap * 2 : 1024;
  while (nc < b->len + extra + 1)
    nc *= 2;
  char *next = realloc(b->data, nc);
  if (!next)
    return;
  b->data = next;
  b->cap = nc;
}

static void sb_append_n(sbuf_t *b, const char *s, size_t n) {
  if (!b || !s || n == 0)
    return;
  sb_reserve(b, n);
  if (!b->data)
    return;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void sb_append(sbuf_t *b, const char *s) {
  if (s)
    sb_append_n(b, s, strlen(s));
}

static void sb_appendf(sbuf_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need <= 0) {
    va_end(ap2);
    return;
  }
  sb_reserve(b, (size_t)need);
  if (b->data) {
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
    b->len += (size_t)need;
  }
  va_end(ap2);
}

static void sb_append_json(sbuf_t *b, const char *s) {
  sb_append(b, "\"");
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
      switch (*p) {
      case '"':
        sb_append(b, "\\\"");
        break;
      case '\\':
        sb_append(b, "\\\\");
        break;
      case '\n':
        sb_append(b, "\\n");
        break;
      case '\r':
        sb_append(b, "\\r");
        break;
      case '\t':
        sb_append(b, "\\t");
        break;
      default:
        if (*p < 0x20)
          sb_appendf(b, "\\u%04x", *p);
        else
          sb_append_n(b, (const char *)p, 1);
        break;
      }
    }
  }
  sb_append(b, "\"");
}

typedef struct {
  char *name;
  char *detail;
  char *doc;
  char *uri;
  int kind;
  int line;
  int col;
  int end_line;
  int end_col;
} lsp_symbol_t;

typedef struct {
  char *uri;
  char *text;
  lsp_symbol_t *symbols;
  size_t symbols_len;
  size_t symbols_cap;
} lsp_doc_t;

typedef struct {
  int line;
  int col;
  int end_line;
  int end_col;
  int severity;
  char *code;
  char *message;
  char *hint;
  char *source;
} lsp_diag_t;

typedef struct {
  lsp_diag_t *items;
  size_t len;
  size_t cap;
} lsp_diag_vec_t;

static lsp_doc_t *g_docs = NULL;
static size_t g_docs_len = 0;
static size_t g_docs_cap = 0;
static lsp_symbol_t *g_stdlib_symbols = NULL;
static size_t g_stdlib_symbols_len = 0;
static size_t g_stdlib_symbols_cap = 0;
static bool g_stdlib_loaded = false;
static lsp_symbol_t g_stdlib_symbol_hit = {0};

static void doc_rebuild_symbols(lsp_doc_t *doc);
static void append_range(sbuf_t *b, int line, int col, int end_line, int end_col);
static bool symbol_name_matches(const lsp_symbol_t *s, const char *word);

static void diag_free(lsp_diag_t *d) {
  if (!d)
    return;
  free(d->code);
  free(d->message);
  free(d->hint);
  free(d->source);
  memset(d, 0, sizeof(*d));
}

static void diag_vec_free(lsp_diag_vec_t *v) {
  if (!v)
    return;
  for (size_t i = 0; i < v->len; ++i)
    diag_free(&v->items[i]);
  free(v->items);
  v->items = NULL;
  v->len = 0;
  v->cap = 0;
}

static void diag_vec_push(lsp_diag_vec_t *v, int line, int col, int end_line, int end_col,
                          int severity, const char *code, const char *message, const char *hint,
                          const char *source) {
  if (!v || !message || !*message)
    return;
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 8;
    lsp_diag_t *next = realloc(v->items, nc * sizeof(*next));
    if (!next)
      return;
    memset(next + v->cap, 0, (nc - v->cap) * sizeof(*next));
    v->items = next;
    v->cap = nc;
  }
  lsp_diag_t *d = &v->items[v->len++];
  d->line = line >= 0 ? line : 0;
  d->col = col >= 0 ? col : 0;
  d->end_line = end_line >= d->line ? end_line : d->line;
  d->end_col = end_col > d->col ? end_col : d->col + 1;
  d->severity = severity > 0 ? severity : 1;
  d->code = ny_strdup(code ? code : "NYLSP0000");
  d->message = ny_strdup(message);
  d->hint = (hint && *hint) ? ny_strdup(hint) : NULL;
  d->source = ny_strdup(source ? source : "nytrix");
}

enum {
  LSP_SK_MODULE = 2,
  LSP_SK_ENUM = 10,
  LSP_SK_FUNCTION = 12,
  LSP_SK_VARIABLE = 13,
  LSP_SK_CONSTANT = 14,
  LSP_SK_STRUCT = 23,
  LSP_SK_OPERATOR = 25
};

static void symbol_free(lsp_symbol_t *s) {
  if (!s)
    return;
  free(s->name);
  free(s->detail);
  free(s->doc);
  free(s->uri);
  memset(s, 0, sizeof(*s));
}

static void symbol_copy(lsp_symbol_t *dst, const lsp_symbol_t *src) {
  if (!dst) {
    return;
  }
  symbol_free(dst);
  if (!src) {
    return;
  }
  dst->name = src->name ? ny_strdup(src->name) : NULL;
  dst->detail = src->detail ? ny_strdup(src->detail) : NULL;
  dst->doc = src->doc ? ny_strdup(src->doc) : NULL;
  dst->uri = src->uri ? ny_strdup(src->uri) : NULL;
  dst->kind = src->kind;
  dst->line = src->line;
  dst->col = src->col;
  dst->end_line = src->end_line;
  dst->end_col = src->end_col;
}

static void doc_clear_symbols(lsp_doc_t *doc) {
  if (!doc)
    return;
  for (size_t i = 0; i < doc->symbols_len; ++i)
    symbol_free(&doc->symbols[i]);
  free(doc->symbols);
  doc->symbols = NULL;
  doc->symbols_len = 0;
  doc->symbols_cap = 0;
}

static void doc_add_symbol(lsp_doc_t *doc, const char *name, const char *detail, const char *docstr,
                           int kind, token_t tok, int end_line, int end_col) {
  if (!doc || !name || !*name)
    return;
  if (doc->symbols_len == doc->symbols_cap) {
    size_t nc = doc->symbols_cap ? doc->symbols_cap * 2 : 64;
    lsp_symbol_t *next = realloc(doc->symbols, nc * sizeof(*doc->symbols));
    if (!next)
      return;
    memset(next + doc->symbols_cap, 0, (nc - doc->symbols_cap) * sizeof(*doc->symbols));
    doc->symbols = next;
    doc->symbols_cap = nc;
  }
  lsp_symbol_t *s = &doc->symbols[doc->symbols_len++];
  s->name = ny_strdup(name);
  s->detail = detail ? ny_strdup(detail) : ny_strdup(name);
  s->doc = docstr ? ny_strdup(docstr) : NULL;
  s->uri = doc->uri ? ny_strdup(doc->uri) : NULL;
  s->kind = kind;
  s->line = tok.line > 0 ? tok.line - 1 : 0;
  s->col = tok.col > 0 ? tok.col - 1 : 0;
  const char *source_name = strrchr(name, '.');
  source_name = source_name ? source_name + 1 : name;
  if (tok.lexeme && source_name && *source_name) {
    const char *line_end = tok.lexeme;
    while (*line_end && *line_end != '\n')
      line_end++;
    size_t source_len = strlen(source_name);
    for (const char *p = tok.lexeme; p + source_len <= line_end; ++p) {
      if (strncmp(p, source_name, source_len) == 0) {
        s->col = (tok.col > 0 ? tok.col - 1 : 0) + (int)(p - tok.lexeme);
        break;
      }
    }
  }
  s->end_line = end_line >= s->line ? end_line : s->line;
  s->end_col = end_col > 0 ? end_col : s->col + (int)strlen(name);
}

static lsp_doc_t *doc_find(const char *uri) {
  if (!uri)
    return NULL;
  for (size_t i = 0; i < g_docs_len; ++i) {
    if (g_docs[i].uri && strcmp(g_docs[i].uri, uri) == 0)
      return &g_docs[i];
  }
  return NULL;
}

static void doc_put(const char *uri, const char *text) {
  if (!uri || !text)
    return;
  lsp_doc_t *doc = doc_find(uri);
  if (!doc) {
    if (g_docs_len == g_docs_cap) {
      size_t nc = g_docs_cap ? g_docs_cap * 2 : 16;
      size_t old_cap = g_docs_cap;
      lsp_doc_t *next = realloc(g_docs, nc * sizeof(*g_docs));
      if (!next)
        return;
      memset(next + old_cap, 0, (nc - old_cap) * sizeof(*next));
      g_docs = next;
      g_docs_cap = nc;
    }
    doc = &g_docs[g_docs_len++];
    memset(doc, 0, sizeof(*doc));
    doc->uri = ny_strdup(uri);
    doc->text = NULL;
  }
  char *copy = ny_strdup(text);
  if (!copy)
    return;
  free(doc->text);
  doc->text = copy;
  doc_rebuild_symbols(doc);
}

static void doc_remove(const char *uri) {
  if (!uri)
    return;
  for (size_t i = 0; i < g_docs_len; ++i) {
    if (g_docs[i].uri && strcmp(g_docs[i].uri, uri) == 0) {
      free(g_docs[i].uri);
      free(g_docs[i].text);
      doc_clear_symbols(&g_docs[i]);
      g_docs[i] = g_docs[g_docs_len - 1];
      g_docs_len--;
      return;
    }
  }
}

static void doc_clear_all(void) {
  for (size_t i = 0; i < g_docs_len; ++i) {
    free(g_docs[i].uri);
    free(g_docs[i].text);
    doc_clear_symbols(&g_docs[i]);
  }
  symbol_free(&g_stdlib_symbol_hit);
  free(g_docs);
  g_docs = NULL;
  g_docs_len = 0;
  g_docs_cap = 0;
  for (size_t i = 0; i < g_stdlib_symbols_len; ++i)
    symbol_free(&g_stdlib_symbols[i]);
  free(g_stdlib_symbols);
  g_stdlib_symbols = NULL;
  g_stdlib_symbols_len = 0;
  g_stdlib_symbols_cap = 0;
  g_stdlib_loaded = false;
  symbol_free(&g_stdlib_symbol_hit);
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + c - 'a';
  if (c >= 'A' && c <= 'F')
    return 10 + c - 'A';
  return -1;
}

static char *uri_to_path(const char *uri) {
  if (!uri)
    return NULL;
  const char *p = uri;
  if (strncmp(uri, "file://", 7) == 0)
    p = uri + 7;
  char *out = malloc(strlen(p) + 1);
  if (!out)
    return NULL;
  size_t w = 0;
  for (size_t i = 0; p[i]; ++i) {
    if (p[i] == '%' && isxdigit((unsigned char)p[i + 1]) &&
        isxdigit((unsigned char)p[i + 2])) {
      int hi = hex_val(p[i + 1]);
      int lo = hex_val(p[i + 2]);
      out[w++] = (char)((hi << 4) | lo);
      i += 2;
    } else {
      out[w++] = p[i];
    }
  }
  out[w] = '\0';
#ifdef _WIN32
  if (out[0] == '/' && isalpha((unsigned char)out[1]) && out[2] == ':')
    memmove(out, out + 1, strlen(out));
#endif
  return out;
}

static char *path_to_file_uri(const char *path) {
  if (!path || !*path)
    return NULL;
  sbuf_t b = {0};
  sb_append(&b, "file://");
  sb_append(&b, path);
  return b.data;
}

static char *read_uri_text(const char *uri) {
  char *path = uri_to_path(uri);
  if (!path)
    return NULL;
  char *text = ny_read_file(path);
  free(path);
  return text;
}

static int stmt_end_line(stmt_t *s) {
  if (!s)
    return 0;
  if (s->kind == NY_S_FUNC && s->as.fn.src_end && s->as.fn.src_start) {
    int line = s->tok.line > 0 ? s->tok.line - 1 : 0;
    for (const char *p = s->as.fn.src_start; p < s->as.fn.src_end; ++p) {
      if (*p == '\n')
        line++;
    }
    return line;
  }
  if (s->kind == NY_S_MODULE && s->as.module.src_end && s->as.module.src_start) {
    int line = s->tok.line > 0 ? s->tok.line - 1 : 0;
    for (const char *p = s->as.module.src_start; p < s->as.module.src_end; ++p) {
      if (*p == '\n')
        line++;
    }
    return line;
  }
  return s->tok.line > 0 ? s->tok.line - 1 : 0;
}

static char *build_fn_detail(const char *name, const ny_param_list *params, const char *ret,
                             bool variadic, bool is_extern) {
  sbuf_t b = {0};
  sb_appendf(&b, "%sfn %s(", is_extern ? "extern " : "", name ? name : "<anon>");
  if (params) {
    for (size_t i = 0; i < params->len; ++i) {
      param_t *p = &params->data[i];
      if (i)
        sb_append(&b, ", ");
      if (p->type && *p->type)
        sb_appendf(&b, "%s: %s", p->type, p->name ? p->name : "_");
      else
        sb_append(&b, p->name ? p->name : "_");
    }
  }
  if (variadic) {
    if (params && params->len)
      sb_append(&b, ", ");
    sb_append(&b, "...");
  }
  sb_append(&b, ")");
  if (ret && *ret)
    sb_appendf(&b, ": %s", ret);
  return b.data ? b.data : ny_strdup(name ? name : "");
}

static char *build_layout_detail(const char *kind, const char *name,
                                 const ny_layout_field_list *fields) {
  sbuf_t b = {0};
  sb_appendf(&b, "%s %s", kind, name ? name : "<anon>");
  if (fields && fields->len) {
    sb_append(&b, " { ");
    size_t limit = fields->len < 6 ? fields->len : 6;
    for (size_t i = 0; i < limit; ++i) {
      layout_field_t *f = &fields->data[i];
      if (i)
        sb_append(&b, ", ");
      sb_appendf(&b, "%s: %s", f->name ? f->name : "_", f->type_name ? f->type_name : "?");
    }
    if (fields->len > limit)
      sb_append(&b, ", ...");
    sb_append(&b, " }");
  }
  return b.data ? b.data : ny_strdup(name ? name : "");
}

static char *build_enum_detail(stmt_t *s) {
  sbuf_t b = {0};
  sb_appendf(&b, "enum %s", s->as.enu.name ? s->as.enu.name : "<anon>");
  if (s->as.enu.items.len) {
    sb_append(&b, " { ");
    size_t limit = s->as.enu.items.len < 8 ? s->as.enu.items.len : 8;
    for (size_t i = 0; i < limit; ++i) {
      if (i)
        sb_append(&b, ", ");
      sb_append(&b, s->as.enu.items.data[i].name);
    }
    if (s->as.enu.items.len > limit)
      sb_append(&b, ", ...");
    sb_append(&b, " }");
  }
  return b.data ? b.data : ny_strdup(s->as.enu.name ? s->as.enu.name : "");
}

static char *build_operator_detail(stmt_t *s) {
  sbuf_t b = {0};
  sb_appendf(&b, "operator %s %s %s: %s = %s",
             s->as.oper.left_type ? s->as.oper.left_type : "?",
             s->as.oper.op ? s->as.oper.op : "?",
             s->as.oper.right_type ? s->as.oper.right_type : "?",
             s->as.oper.return_type ? s->as.oper.return_type : "?",
             s->as.oper.target ? s->as.oper.target : "?");
  return b.data ? b.data : ny_strdup("operator");
}

static void collect_stmt_symbols(lsp_doc_t *doc, ny_stmt_list *body) {
  if (!doc || !body)
    return;
  for (size_t i = 0; i < body->len; ++i) {
    stmt_t *s = body->data[i];
    if (!s)
      continue;
    switch (s->kind) {
    case NY_S_FUNC: {
      char *detail = build_fn_detail(s->as.fn.name, &s->as.fn.params, s->as.fn.return_type,
                                     s->as.fn.is_variadic, s->as.fn.is_extern);
      doc_add_symbol(doc, s->as.fn.name, detail, s->as.fn.doc, LSP_SK_FUNCTION, s->tok,
                     stmt_end_line(s), s->tok.col + (int)strlen(s->as.fn.name ? s->as.fn.name : ""));
      free(detail);
      break;
    }
    case NY_S_EXTERN: {
      char *detail = build_fn_detail(s->as.ext.name, &s->as.ext.params, s->as.ext.return_type,
                                     s->as.ext.is_variadic, true);
      doc_add_symbol(doc, s->as.ext.name, detail, NULL, LSP_SK_FUNCTION, s->tok,
                     stmt_end_line(s), s->tok.col + (int)strlen(s->as.ext.name ? s->as.ext.name : ""));
      free(detail);
      break;
    }
    case NY_S_VAR: {
      for (size_t j = 0; j < s->as.var.names.len; ++j) {
        const char *name = s->as.var.names.data[j];
        const char *type = j < s->as.var.types.len ? s->as.var.types.data[j] : NULL;
        char detail[256];
        snprintf(detail, sizeof(detail), "%s %s%s%s", s->as.var.is_mut ? "mut" : "def",
                 name ? name : "_", type ? ": " : "", type ? type : "");
        doc_add_symbol(doc, name, detail, NULL, s->as.var.is_mut ? LSP_SK_VARIABLE : LSP_SK_CONSTANT,
                       s->tok, stmt_end_line(s), s->tok.col + (int)strlen(name ? name : ""));
      }
      break;
    }
    case NY_S_LAYOUT: {
      char *detail = build_layout_detail("layout", s->as.layout.name, &s->as.layout.fields);
      doc_add_symbol(doc, s->as.layout.name, detail, NULL, LSP_SK_STRUCT, s->tok,
                     stmt_end_line(s), s->tok.col + (int)strlen(s->as.layout.name ? s->as.layout.name : ""));
      free(detail);
      collect_stmt_symbols(doc, &s->as.layout.methods);
      break;
    }
    case NY_S_STRUCT: {
      char *detail = build_layout_detail("struct", s->as.struc.name, &s->as.struc.fields);
      doc_add_symbol(doc, s->as.struc.name, detail, NULL, LSP_SK_STRUCT, s->tok,
                     stmt_end_line(s), s->tok.col + (int)strlen(s->as.struc.name ? s->as.struc.name : ""));
      free(detail);
      collect_stmt_symbols(doc, &s->as.struc.methods);
      break;
    }
    case NY_S_ENUM: {
      char *detail = build_enum_detail(s);
      doc_add_symbol(doc, s->as.enu.name, detail, NULL, LSP_SK_ENUM, s->tok, stmt_end_line(s),
                     s->tok.col + (int)strlen(s->as.enu.name ? s->as.enu.name : ""));
      free(detail);
      break;
    }
    case NY_S_OPERATOR: {
      char *detail = build_operator_detail(s);
      doc_add_symbol(doc, detail, detail, NULL, LSP_SK_OPERATOR, s->tok, stmt_end_line(s),
                     s->tok.col + (int)strlen("operator"));
      free(detail);
      break;
    }
    case NY_S_MODULE:
      doc_add_symbol(doc, s->as.module.name, "module", "Module", LSP_SK_MODULE, s->tok,
                     stmt_end_line(s), s->tok.col + (int)strlen(s->as.module.name ? s->as.module.name : ""));
      collect_stmt_symbols(doc, &s->as.module.body);
      break;
    case NY_S_BLOCK:
      collect_stmt_symbols(doc, &s->as.block.body);
      break;
    case NY_S_IMPL:
      collect_stmt_symbols(doc, &s->as.impl.methods);
      break;
    default:
      break;
    }
  }
}

static void doc_rebuild_symbols(lsp_doc_t *doc) {
  if (!doc)
    return;
  doc_clear_symbols(doc);
  if (!doc->text)
    return;
  parser_t parser;
  parser_init_quiet(&parser, doc->text, "<lsp>");
  parser.error_limit = 0;
  program_t prog = parse_program(&parser);
  collect_stmt_symbols(doc, &prog.body);
  program_free(&prog, parser.arena);
}

static void stdlib_symbol_hit_set(const lsp_symbol_t *src) {
  symbol_copy(&g_stdlib_symbol_hit, src);
}

static void stdlib_symbol_push(const lsp_symbol_t *src) {
  if (!src) {
    return;
  }
  if (g_stdlib_symbols_len == g_stdlib_symbols_cap) {
    size_t nc = g_stdlib_symbols_cap ? g_stdlib_symbols_cap * 2 : 512;
    lsp_symbol_t *next = realloc(g_stdlib_symbols, nc * sizeof(*next));
    if (!next) {
      return;
    }
    memset(next + g_stdlib_symbols_cap, 0, (nc - g_stdlib_symbols_cap) * sizeof(*next));
    g_stdlib_symbols = next;
    g_stdlib_symbols_cap = nc;
  }
  symbol_copy(&g_stdlib_symbols[g_stdlib_symbols_len++], src);
}

static void stdlib_index_ensure(void) {
  if (g_stdlib_loaded) {
    return;
  }
  g_stdlib_loaded = true;
  for (size_t i = 0; i < ny_std_module_count(); ++i) {
    const char *mod_path = ny_std_module_path(i);
    if (!mod_path || !*mod_path) {
      continue;
    }
    char full[PATH_MAX];
    if (mod_path[0] == '/') {
      snprintf(full, sizeof(full), "%s", mod_path);
    } else {
      ny_join_path(full, sizeof(full), ny_src_root(), mod_path);
    }
    char *text = ny_read_file(full);
    if (!text) {
      continue;
    }
    char *uri = path_to_file_uri(full);
    lsp_doc_t doc = {0};
    doc.uri = uri;
    doc.text = text;
    doc_rebuild_symbols(&doc);
    for (size_t j = 0; j < doc.symbols_len; ++j) {
      stdlib_symbol_push(&doc.symbols[j]);
    }
    doc_clear_symbols(&doc);
    free(uri);
    free(text);
  }
}

static lsp_symbol_t *find_symbol_in_stdlib(const char *word) {
  if (!word || !*word)
    return NULL;
  stdlib_index_ensure();
  symbol_free(&g_stdlib_symbol_hit);
  for (size_t i = 0; i < g_stdlib_symbols_len; ++i) {
    if (symbol_name_matches(&g_stdlib_symbols[i], word)) {
      stdlib_symbol_hit_set(&g_stdlib_symbols[i]);
      return &g_stdlib_symbol_hit;
    }
  }
  return NULL;
}

typedef struct {
  const char *name;
  const char *detail;
  const char *doc;
  int kind;
} lsp_builtin_t;

static const lsp_builtin_t g_core_builtins[] = {
    {"print", "fn print(...)", "Prints values to stdout.", LSP_SK_FUNCTION},
    {"assert", "fn assert(condition, message=\"\")", "Fails execution when condition is false.",
     LSP_SK_FUNCTION},
    {"len", "fn len(value)", "Returns the length of a list, string, dict, or compatible value.",
     LSP_SK_FUNCTION},
    {"dict", "fn dict(capacity=0)", "Creates a dictionary.", LSP_SK_FUNCTION},
    {"list", "fn list(cap=0)", "Creates an empty list with reserved capacity.", LSP_SK_FUNCTION},
    {"str", "fn str(value)", "Converts a value to a string.", LSP_SK_FUNCTION},
    {"int", "fn int(value)", "Converts a value to an integer.", LSP_SK_FUNCTION},
    {"float", "fn float(value)", "Converts a value to a float.", LSP_SK_FUNCTION},
    {"bool", "fn bool(value)", "Converts a value to a boolean.", LSP_SK_FUNCTION},
    {"__main", "fn __main()", "Returns true when the current file is the main script.",
     LSP_SK_FUNCTION},
};

static const lsp_builtin_t g_rt_builtins[] = {
#define RT_DEF(name, p, args, sig, doc) {name, sig, doc, LSP_SK_FUNCTION},
#define RT_GV(name, p, t, doc) {name, "global", doc, LSP_SK_VARIABLE},
#include "rt/defs.h"
#undef RT_DEF
#undef RT_GV
};

static const lsp_builtin_t *find_builtin(const char *name) {
  if (!name || !*name)
    return NULL;
  for (size_t i = 0; i < sizeof(g_core_builtins) / sizeof(g_core_builtins[0]); ++i) {
    if (strcmp(g_core_builtins[i].name, name) == 0)
      return &g_core_builtins[i];
  }
  for (size_t i = 0; i < sizeof(g_rt_builtins) / sizeof(g_rt_builtins[0]); ++i) {
    if (strcmp(g_rt_builtins[i].name, name) == 0)
      return &g_rt_builtins[i];
  }
  return NULL;
}

static bool symbol_name_matches(const lsp_symbol_t *s, const char *word) {
  if (!s || !s->name || !word)
    return false;
  const char *short_word = ny_tail_name(word);
  const char *short_sym = ny_tail_name(s->name);
  return strcmp(s->name, word) == 0 || (short_word && short_sym && strcmp(short_sym, short_word) == 0);
}

static lsp_symbol_t *find_symbol(const char *word, lsp_doc_t *current) {
  if (!word || !*word)
    return NULL;
  if (current) {
    for (size_t i = 0; i < current->symbols_len; ++i) {
      if (symbol_name_matches(&current->symbols[i], word))
        return &current->symbols[i];
    }
  }
  for (size_t d = 0; d < g_docs_len; ++d) {
    if (&g_docs[d] == current)
      continue;
    for (size_t i = 0; i < g_docs[d].symbols_len; ++i) {
      if (symbol_name_matches(&g_docs[d].symbols[i], word))
        return &g_docs[d].symbols[i];
    }
  }
  return NULL;
}

static size_t line_offset_for(const char *text, int line) {
  if (!text || line <= 0)
    return 0;
  int cur = 0;
  for (size_t i = 0; text[i]; ++i) {
    if (cur == line)
      return i;
    if (text[i] == '\n')
      cur++;
  }
  return strlen(text);
}

static char *word_at_position(const char *text, int line, int ch, int *out_start, int *out_end) {
  if (!text || line < 0 || ch < 0)
    return NULL;
  size_t off = line_offset_for(text, line);
  size_t line_end = off;
  while (text[line_end] && text[line_end] != '\n')
    line_end++;
  size_t pos = off + (size_t)ch;
  if (pos > line_end)
    pos = line_end;
  size_t start = pos;
  while (start > off) {
    unsigned char c = (unsigned char)text[start - 1];
    if (!(isalnum(c) || c == '_' || c == '.'))
      break;
    start--;
  }
  size_t end = pos;
  while (end < line_end) {
    unsigned char c = (unsigned char)text[end];
    if (!(isalnum(c) || c == '_' || c == '.'))
      break;
    end++;
  }
  if (end <= start)
    return NULL;
  if (out_start)
    *out_start = (int)(start - off);
  if (out_end)
    *out_end = (int)(end - off);
  return ny_strndup(text + start, end - start);
}

static int split_comment_index(const char *line) {
  if (!line)
    return -1;
  int quote = 0;
  int esc = 0;
  for (int i = 0; line[i]; ++i) {
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
      return i;
  }
  return -1;
}

static int use_import_star_col(const char *line) {
  if (!line)
    return -1;
  const char *p = line;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (strncmp(p, "use", 3) != 0 || isalnum((unsigned char)p[3]) || p[3] == '_')
    return -1;
  p += 3;
  if (!isspace((unsigned char)*p))
    return -1;
  if (strchr(p, '(') || strstr(p, " as "))
    return -1;
  const char *end = line + strlen(line);
  while (end > line && isspace((unsigned char)end[-1]))
    end--;
  if (end <= line || end[-1] != '*')
    return -1;
  const char *star = end - 1;
  const char *cut = star;
  while (cut > line && isspace((unsigned char)cut[-1]))
    cut--;
  if (cut == star)
    return -1;
  return (int)(star - line);
}

static int use_import_list_compact_col(const char *line) {
  if (!line)
    return -1;
  const char *p = line;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (strncmp(p, "use", 3) != 0 || isalnum((unsigned char)p[3]) || p[3] == '_')
    return -1;
  const char *open = strchr(p, '(');
  if (!open || open == p || isspace((unsigned char)open[-1]))
    return -1;
  return (int)(open - line);
}

static int line_contains_col(const char *line, const char *needle) {
  const char *p = strstr(line, needle);
  return p ? (int)(p - line) : -1;
}

static int line_contains_any_col(const char *line, const char **needles, size_t n) {
  int best = -1;
  for (size_t i = 0; i < n; i++) {
    int col = line_contains_col(line, needles[i]);
    if (col >= 0 && (best < 0 || col < best))
      best = col;
  }
  return best;
}

static int count_substr_occurrences(const char *line, const char *needle) {
  int count = 0;
  size_t n = strlen(needle);
  if (n == 0)
    return 0;
  const char *p = line;
  while ((p = strstr(p, needle))) {
    count++;
    p += n;
  }
  return count;
}

static bool line_starts_keyword(const char *line, const char *kw) {
  while (*line && isspace((unsigned char)*line))
    line++;
  size_t n = strlen(kw);
  return strncmp(line, kw, n) == 0 && !isalnum((unsigned char)line[n]) && line[n] != '_';
}

static void analyze_style_hints(const char *text, lsp_diag_vec_t *out) {
  if (!text || !out)
    return;
  int line = 0;
  const char *cur = text;
  int dict_lookup_count = 0, dict_lookup_line = -1, dict_lookup_col = 0;
  int layout_probe_count = 0, layout_probe_line = -1, layout_probe_col = 0;
  int parser_emit_count = 0, parser_emit_line = -1, parser_emit_col = 0;
  int alloc_count = 0, alloc_line = -1, alloc_col = 0;
  while (*cur) {
    const char *line_end = cur;
    while (*line_end && *line_end != '\n')
      line_end++;
    size_t raw_len = (size_t)(line_end - cur);
    char buf[4096];
    size_t use_len = raw_len < sizeof(buf) - 1 ? raw_len : sizeof(buf) - 1;
    memcpy(buf, cur, use_len);
    buf[use_len] = '\0';
    int comment = split_comment_index(buf);
    if (comment >= 0)
      buf[comment] = '\0';
    size_t trimmed = strlen(buf);
    while (trimmed > 0 && isspace((unsigned char)buf[trimmed - 1]))
      buf[--trimmed] = '\0';

    int star_col = use_import_star_col(buf);
    if (star_col >= 0) {
      diag_vec_push(out, line, star_col, line, star_col + 1, 2, "NYSYN1001",
                    "legacy import spelling: use module *",
                    "prefer bare 'use module'; it already imports exported names and keeps the module leaf alias",
                    "nytrix");
    }
    int list_col = use_import_list_compact_col(buf);
    if (list_col >= 0) {
      diag_vec_push(out, line, list_col, line, list_col + 1, 3, "NYSYN1002",
                    "missing space before import list",
                    "prefer 'use module (name)' so formatter, docs, and editor diagnostics stay aligned",
                    "nytrix");
    }

    const char *compile_probes[] = {"__os_name", "__arch_name", "__main", "os()", "arch()",
                                    "sizeof(", "__layout_size", "__layout_align",
                                    "__layout_offset"};
    int probe_col = line_contains_any_col(buf, compile_probes,
                                          sizeof(compile_probes) / sizeof(compile_probes[0]));
    bool branch_line = line_starts_keyword(buf, "if") || line_starts_keyword(buf, "elif") ||
                       line_starts_keyword(buf, "case") || line_starts_keyword(buf, "match");
    if (probe_col >= 0 && branch_line && !strstr(buf, "comptime")) {
      diag_vec_push(out, line, probe_col, line, probe_col + 1, 3, "NYAUD3102",
                    "specialization candidate: runtime branch uses compile-time probe",
                    "ny-fmt --specialize would prefer moving this decision into comptime",
                    "nytrix-audit");
    }

    const char *layout_probes[] = {"__layout_size", "__layout_align", "__layout_offset",
                                   "sizeof("};
    int layout_col = line_contains_any_col(buf, layout_probes,
                                           sizeof(layout_probes) / sizeof(layout_probes[0]));
    if (layout_col >= 0) {
      if (layout_probe_line < 0) {
        layout_probe_line = line;
        layout_probe_col = layout_col;
      }
      layout_probe_count++;
      if (!strstr(buf, "comptime")) {
        diag_vec_push(out, line, layout_col, line, layout_col + 1, 3, "NYAUD3104",
                      "layout probe can usually be folded at compile time",
                      "wrap stable ABI-size decisions in comptime so generated code carries constants",
                      "nytrix-audit");
      }
    }

    int dict_here = count_substr_occurrences(buf, ".get(") +
                    count_substr_occurrences(buf, "dict_get(") +
                    count_substr_occurrences(buf, "contains(");
    if (dict_here > 0) {
      if (dict_lookup_line < 0) {
        dict_lookup_line = line;
        dict_lookup_col = line_contains_col(buf, ".get(");
        if (dict_lookup_col < 0)
          dict_lookup_col = line_contains_col(buf, "dict_get(");
        if (dict_lookup_col < 0)
          dict_lookup_col = line_contains_col(buf, "contains(");
      }
      dict_lookup_count += dict_here;
    }

    int parser_col = line_contains_any_col(
        buf, (const char *[]){"parser_error", "ny_diag_error", "issue_push", "comptime emit"},
        4);
    if (parser_col >= 0) {
      if (parser_emit_line < 0) {
        parser_emit_line = line;
        parser_emit_col = parser_col;
      }
      parser_emit_count++;
    }

    int alloc_col_here =
        line_contains_any_col(buf, (const char *[]){"malloc(", "zalloc(", "realloc("}, 3);
    if (alloc_col_here >= 0) {
      if (alloc_line < 0) {
        alloc_line = line;
        alloc_col = alloc_col_here;
      }
      alloc_count++;
    }

    if (*line_end == '\n')
      line_end++;
    cur = line_end;
    line++;
  }
  if (dict_lookup_count >= 8 && dict_lookup_line >= 0) {
    diag_vec_push(out, dict_lookup_line, dict_lookup_col, dict_lookup_line, dict_lookup_col + 1, 3,
                  "NYAUD4101", "metaprogramming candidate: repeated dynamic map lookups",
                  "ny-fmt --metaprog would suggest a generated table or cached layout for this lookup cluster",
                  "nytrix-audit");
  }
  if (layout_probe_count >= 3 && layout_probe_line >= 0) {
    diag_vec_push(out, layout_probe_line, layout_probe_col, layout_probe_line,
                  layout_probe_col + 1, 3, "NYAUD4201",
                  "specialization candidate: repeated layout/sizeof probes",
                  "consider a comptime table so ABI facts are generated once", "nytrix-audit");
  }
  if (parser_emit_count >= 5 && parser_emit_line >= 0) {
    diag_vec_push(out, parser_emit_line, parser_emit_col, parser_emit_line, parser_emit_col + 1, 3,
                  "NYAUD4501", "metaprogramming candidate: repeated parser/diagnostic emission",
                  "ny-fmt --metaprog would suggest a generated diagnostic table or template",
                  "nytrix-audit");
  }
  if (alloc_count >= 8 && layout_probe_count == 0 && alloc_line >= 0) {
    diag_vec_push(out, alloc_line, alloc_col, alloc_line, alloc_col + 1, 3, "NYAUD4301",
                  "specialization candidate: repeated raw allocation shape",
                  "consider a layout, arena, or generated allocator wrapper for this allocation cluster",
                  "nytrix-audit");
  }
}

static void publish_diagnostics(const char *uri, const lsp_diag_vec_t *diags) {
  if (!uri)
    return;
  sbuf_t body = {0};
  sb_append(&body, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
  sb_append_json(&body, uri);
  sb_append(&body, ",\"diagnostics\":[");
  for (size_t i = 0; diags && i < diags->len; ++i) {
    const lsp_diag_t *d = &diags->items[i];
    if (i)
      sb_append(&body, ",");
    sb_append(&body, "{\"range\":");
    append_range(&body, d->line, d->col, d->end_line, d->end_col);
    sb_appendf(&body, ",\"severity\":%d", d->severity);
    sb_append(&body, ",\"source\":");
    sb_append_json(&body, d->source ? d->source : "nytrix");
    sb_append(&body, ",\"code\":");
    sb_append_json(&body, d->code ? d->code : "NYLSP0000");
    sb_append(&body, ",\"message\":");
    sb_append_json(&body, d->message ? d->message : "diagnostic");
    if (d->hint && *d->hint) {
      char hint_msg[1152];
      snprintf(hint_msg, sizeof(hint_msg), "hint: %s", d->hint);
      sb_append(&body, ",\"relatedInformation\":[{\"location\":{\"uri\":");
      sb_append_json(&body, uri);
      sb_append(&body, ",\"range\":");
      append_range(&body, d->line, d->col, d->end_line, d->end_col);
      sb_append(&body, "},\"message\":");
      sb_append_json(&body, hint_msg);
      sb_append(&body, "}]");
    }
    sb_append(&body, "}");
  }
  sb_append(&body, "]}}");
  send_response(body.data ? body.data : "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":\"\",\"diagnostics\":[]}}");
  free(body.data);
}

static bool analyze_text(const char *text, lsp_diag_vec_t *out) {
  if (!text || !out)
    return false;
  parser_t parser;
  parser_init_quiet(&parser, text, "<lsp>");
  parser.error_limit = 0;
  program_t prog = parse_program(&parser);
  (void)prog;
  program_free(&prog, parser.arena);
  if (parser.error_count > 0) {
    int line = parser.last_error_line > 0 ? parser.last_error_line - 1 : 0;
    int col = parser.last_error_col > 0 ? parser.last_error_col - 1 : 0;
    int end_col = parser.last_error_end_col > 0 ? parser.last_error_end_col - 1 : col + 1;
    diag_vec_push(out, line, col, line, end_col, 1, "NYPARSE1001",
                  parser.last_error_msg[0] ? parser.last_error_msg : "parse error",
                  parser.last_error_hint[0] ? parser.last_error_hint : NULL, "nytrix");
  }
  analyze_style_hints(text, out);
  return out->len > 0;
}

static void check_document(const char *uri, const char *text) {
  if (!uri)
    return;
  lsp_diag_vec_t diags = {0};
  analyze_text(text, &diags);
  publish_diagnostics(uri, &diags);
  diag_vec_free(&diags);
}

static void send_result(const char *id, const char *result_json) {
  sbuf_t b = {0};
  sb_append(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  sb_append(&b, id ? id : "null");
  sb_append(&b, ",\"result\":");
  sb_append(&b, result_json ? result_json : "null");
  sb_append(&b, "}");
  send_response(b.data);
  free(b.data);
}

static void append_range(sbuf_t *b, int line, int col, int end_line, int end_col) {
  sb_appendf(b,
             "{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,"
             "\"character\":%d}}",
             line < 0 ? 0 : line, col < 0 ? 0 : col, end_line < 0 ? 0 : end_line,
             end_col < 0 ? 0 : end_col);
}

static void append_symbol_location(sbuf_t *b, const char *uri, const lsp_symbol_t *s) {
  sb_append(b, "{\"uri\":");
  sb_append_json(b, uri);
  sb_append(b, ",\"range\":");
  append_range(b, s->line, s->col, s->end_line, s->end_col);
  sb_append(b, "}");
}

static const char *hover_kind_label(int kind) {
  switch (kind) {
  case LSP_SK_MODULE:
    return "Module";
  case LSP_SK_STRUCT:
    return "Struct";
  case LSP_SK_ENUM:
    return "Enum";
  case LSP_SK_OPERATOR:
    return "Operator";
  case LSP_SK_VARIABLE:
    return "Variable";
  case LSP_SK_CONSTANT:
    return "Constant";
  default:
    return "Function";
  }
}

static const char *uri_basename(const char *uri) {
  if (!uri || !*uri)
    return "";
  const char *slash = strrchr(uri, '/');
  return slash ? slash + 1 : uri;
}

static char *trimmed_copy(const char *start, size_t len) {
  if (!start)
    return strdup("");
  while (len && isspace((unsigned char)*start)) {
    start++;
    len--;
  }
  while (len && isspace((unsigned char)start[len - 1])) {
    len--;
  }
  char *out = malloc(len + 1);
  if (!out)
    return NULL;
  memcpy(out, start, len);
  out[len] = '\0';
  return out;
}

static void append_hover_summary(sbuf_t *value, const char *detail, int kind, const char *uri,
                                 int line) {
  if (!value)
    return;
  sb_append(value, "\n\n- **Kind**: ");
  sb_append(value, hover_kind_label(kind));
  if (detail && strncmp(detail, "fn ", 3) == 0) {
    const char *open = strchr(detail, '(');
    const char *close = open ? strrchr(open, ')') : NULL;
    if (open && close && close > open) {
      char *params = trimmed_copy(open + 1, (size_t)(close - open - 1));
      if (params) {
        sb_append(value, "\n- **Inputs**: ");
        if (*params)
          sb_appendf(value, "`%s`", params);
        else
          sb_append(value, "_none_");
        free(params);
      }
      const char *colon = close + 1;
      while (*colon && isspace((unsigned char)*colon))
        colon++;
      if (*colon == ':') {
        colon++;
        while (*colon && isspace((unsigned char)*colon))
          colon++;
      } else {
        colon = NULL;
      }
      sb_append(value, "\n- **Output**: ");
      if (colon && *colon) {
        sb_append(value, "`");
        sb_append(value, colon);
        sb_append(value, "`");
      } else {
        sb_append(value, "_inferred_");
      }
    }
  }
  if (uri && *uri) {
    sb_appendf(value, "\n- **Source**: `%s:%d`", uri_basename(uri), line + 1);
  }
}

static void handle_hover(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  int line = 0, ch = 0, start = 0, end = 0;
  json_extract_int_near(body, "\"position\"", "line", &line);
  json_extract_int_near(body, "\"position\"", "character", &ch);
  lsp_doc_t *doc = doc_find(uri);
  char *word = word_at_position(doc ? doc->text : NULL, line, ch, &start, &end);
  lsp_symbol_t *sym = find_symbol(word, doc);
  if (!sym)
    sym = find_symbol_in_stdlib(word);
  const lsp_builtin_t *bi = sym ? NULL : find_builtin(word);
  if (!sym && !bi) {
    send_result(id, "null");
    free(word);
    free(uri);
    return;
  }
  sbuf_t value = {0};
  sb_append(&value, "```ny\n");
  sb_append(&value, sym ? sym->detail : bi->detail);
  sb_append(&value, "\n```");
  append_hover_summary(&value, sym ? sym->detail : bi->detail, sym ? sym->kind : bi->kind,
                       sym ? sym->uri : NULL, sym ? sym->line : line);
  const char *docstr = sym ? sym->doc : bi->doc;
  if (docstr && *docstr) {
    sb_append(&value, "\n\n");
    sb_append(&value, docstr);
  }
  sbuf_t result = {0};
  sb_append(&result, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
  sb_append_json(&result, value.data ? value.data : "");
  sb_append(&result, "},\"range\":");
  append_range(&result, line, start, line, end);
  sb_append(&result, "}");
  send_result(id, result.data);
  free(result.data);
  free(value.data);
  free(word);
  free(uri);
}

static void handle_definition(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  int line = 0, ch = 0;
  json_extract_int_near(body, "\"position\"", "line", &line);
  json_extract_int_near(body, "\"position\"", "character", &ch);
  lsp_doc_t *doc = doc_find(uri);
  char *word = word_at_position(doc ? doc->text : NULL, line, ch, NULL, NULL);
  lsp_symbol_t *sym = find_symbol(word, doc);
  if (!sym)
    sym = find_symbol_in_stdlib(word);
  if (!sym) {
    send_result(id, "null");
  } else {
    sbuf_t result = {0};
    append_symbol_location(&result, sym->uri ? sym->uri : uri, sym);
    send_result(id, result.data);
    free(result.data);
  }
  free(word);
  free(uri);
}

static void handle_document_symbols(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  lsp_doc_t *doc = doc_find(uri);
  sbuf_t result = {0};
  sb_append(&result, "[");
  if (doc) {
    for (size_t i = 0; i < doc->symbols_len; ++i) {
      lsp_symbol_t *s = &doc->symbols[i];
      const char *sel_name = ny_tail_name(s->name);
      int sel_end_col = s->col + (int)strlen(sel_name && *sel_name ? sel_name : (s->name ? s->name : ""));
      int full_end_line = s->end_line;
      int full_end_col = s->end_col;
      if (full_end_line < s->line) {
        full_end_line = s->line;
      }
      if (full_end_line == s->line && full_end_col < sel_end_col) {
        full_end_col = sel_end_col;
      }
      if (i)
        sb_append(&result, ",");
      sb_append(&result, "{\"name\":");
      sb_append_json(&result, s->name);
      sb_append(&result, ",\"detail\":");
      sb_append_json(&result, s->detail);
      sb_appendf(&result, ",\"kind\":%d,\"range\":", s->kind);
      append_range(&result, s->line, s->col, full_end_line, full_end_col);
      sb_append(&result, ",\"selectionRange\":");
      append_range(&result, s->line, s->col, s->line, sel_end_col);
      sb_append(&result, "}");
    }
  }
  sb_append(&result, "]");
  send_result(id, result.data);
  free(result.data);
  free(uri);
}

static bool str_contains_casefold(const char *hay, const char *needle) {
  if (!needle || !*needle)
    return true;
  if (!hay)
    return false;
  size_t nlen = strlen(needle);
  for (const char *p = hay; *p; ++p) {
    size_t i = 0;
    while (i < nlen && p[i] &&
           tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
      i++;
    if (i == nlen)
      return true;
  }
  return false;
}

static void handle_workspace_symbols(const char *id, const char *body) {
  char *query = json_extract_string(body, "query");
  sbuf_t result = {0};
  sb_append(&result, "[");
  bool first = true;
  size_t emitted = 0;
  for (size_t d = 0; d < g_docs_len && emitted < 400; ++d) {
    for (size_t i = 0; i < g_docs[d].symbols_len && emitted < 400; ++i) {
      lsp_symbol_t *s = &g_docs[d].symbols[i];
      if (!str_contains_casefold(s->name, query))
        continue;
      if (!first)
        sb_append(&result, ",");
      sb_append(&result, "{\"name\":");
      sb_append_json(&result, s->name);
      sb_appendf(&result, ",\"kind\":%d,\"location\":", s->kind);
      append_symbol_location(&result, s->uri ? s->uri : g_docs[d].uri, s);
      sb_append(&result, "}");
      first = false;
      emitted++;
    }
  }
  stdlib_index_ensure();
  for (size_t i = 0; i < g_stdlib_symbols_len && emitted < 400; ++i) {
    lsp_symbol_t *s = &g_stdlib_symbols[i];
    if (!str_contains_casefold(s->name, query))
      continue;
    if (!first)
      sb_append(&result, ",");
    sb_append(&result, "{\"name\":");
    sb_append_json(&result, s->name);
    sb_appendf(&result, ",\"kind\":%d,\"location\":", s->kind);
    append_symbol_location(&result, s->uri, s);
    sb_append(&result, "}");
    first = false;
    emitted++;
  }
  sb_append(&result, "]");
  send_result(id, result.data);
  free(result.data);
  free(query);
}

static int completion_kind_for_symbol(int sk) {
  switch (sk) {
  case LSP_SK_MODULE:
    return 9;
  case LSP_SK_ENUM:
    return 13;
  case LSP_SK_STRUCT:
    return 22;
  case LSP_SK_OPERATOR:
    return 24;
  case LSP_SK_VARIABLE:
    return 6;
  case LSP_SK_CONSTANT:
    return 21;
  default:
    return 3;
  }
}

static void append_completion_item(sbuf_t *b, const char *label, int kind, const char *detail,
                                   const char *doc) {
  sb_append(b, "{\"label\":");
  sb_append_json(b, label);
  sb_appendf(b, ",\"kind\":%d", kind);
  if (detail) {
    sb_append(b, ",\"detail\":");
    sb_append_json(b, detail);
  }
  if (doc && *doc) {
    sb_append(b, ",\"documentation\":{\"kind\":\"markdown\",\"value\":");
    sb_append_json(b, doc);
    sb_append(b, "}");
  }
  sb_append(b, "}");
}

static void handle_completion(const char *id) {
  sbuf_t result = {0};
  sb_append(&result, "{\"isIncomplete\":false,\"items\":[");
  bool first = true;
  for (size_t i = 0; i < sizeof(g_core_builtins) / sizeof(g_core_builtins[0]); ++i) {
    if (!first)
      sb_append(&result, ",");
    append_completion_item(&result, g_core_builtins[i].name, 3, g_core_builtins[i].detail,
                           g_core_builtins[i].doc);
    first = false;
  }
  for (size_t i = 0; i < sizeof(g_rt_builtins) / sizeof(g_rt_builtins[0]); ++i) {
    if (!first)
      sb_append(&result, ",");
    append_completion_item(&result, g_rt_builtins[i].name, completion_kind_for_symbol(g_rt_builtins[i].kind),
                           g_rt_builtins[i].detail, g_rt_builtins[i].doc);
    first = false;
  }
  for (size_t d = 0; d < g_docs_len; ++d) {
    for (size_t i = 0; i < g_docs[d].symbols_len; ++i) {
      if (!first)
        sb_append(&result, ",");
      lsp_symbol_t *s = &g_docs[d].symbols[i];
      append_completion_item(&result, s->name, completion_kind_for_symbol(s->kind), s->detail,
                             s->doc);
      first = false;
    }
  }
  stdlib_index_ensure();
  for (size_t i = 0; i < g_stdlib_symbols_len; ++i) {
    if (!first)
      sb_append(&result, ",");
    lsp_symbol_t *s = &g_stdlib_symbols[i];
    append_completion_item(&result, s->name, completion_kind_for_symbol(s->kind), s->detail,
                           s->doc);
    first = false;
  }
  sb_append(&result, "]}");
  send_result(id, result.data);
  free(result.data);
}

static void handle_signature(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  int line = 0, ch = 0;
  json_extract_int_near(body, "\"position\"", "line", &line);
  json_extract_int_near(body, "\"position\"", "character", &ch);
  lsp_doc_t *doc = doc_find(uri);
  int start = 0, end = 0;
  char *word = NULL;
  if (doc && doc->text) {
    size_t off = line_offset_for(doc->text, line) + (size_t)ch;
    while (off > 0 && doc->text[off - 1] != '(' && doc->text[off - 1] != '\n')
      off--;
    if (off > 0 && doc->text[off - 1] == '(')
      word = word_at_position(doc->text, line, (int)(off - line_offset_for(doc->text, line)) - 1,
                              &start, &end);
  }
  (void)start;
  (void)end;
  lsp_symbol_t *sym = find_symbol(word, doc);
  if (!sym)
    sym = find_symbol_in_stdlib(word);
  const lsp_builtin_t *bi = sym ? NULL : find_builtin(word);
  const char *detail = sym ? sym->detail : (bi ? bi->detail : NULL);
  const char *docstr = sym ? sym->doc : (bi ? bi->doc : NULL);
  if (!detail) {
    send_result(id, "null");
  } else {
    sbuf_t result = {0};
    sb_append(&result, "{\"signatures\":[{\"label\":");
    sb_append_json(&result, detail);
    if (docstr && *docstr) {
      sb_append(&result, ",\"documentation\":{\"kind\":\"markdown\",\"value\":");
      sb_append_json(&result, docstr);
      sb_append(&result, "}");
    }
    sb_append(&result, "}],\"activeSignature\":0,\"activeParameter\":0}");
    send_result(id, result.data);
    free(result.data);
  }
  free(word);
  free(uri);
}

static void append_text_references(sbuf_t *result, bool *first, const char *uri, const char *text,
                                   const char *word) {
  if (!result || !first || !uri || !text || !word || !*word)
    return;
  size_t wlen = strlen(word);
  int line = 0;
  int col = 0;
  for (size_t i = 0; text[i]; ++i) {
    if (text[i] == '\n') {
      line++;
      col = 0;
      continue;
    }
    if ((i == 0 || !ny_symbol_path_char((unsigned char)text[i - 1])) &&
        strncmp(text + i, word, wlen) == 0 && !ny_symbol_path_char((unsigned char)text[i + wlen])) {
      if (!*first)
        sb_append(result, ",");
      sb_append(result, "{\"uri\":");
      sb_append_json(result, uri);
      sb_append(result, ",\"range\":");
      append_range(result, line, col, line, col + (int)wlen);
      sb_append(result, "}");
      *first = false;
    }
    col++;
  }
}

static void handle_references(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  int line = 0;
  int ch = 0;
  json_extract_int_near(body, "\"position\"", "line", &line);
  json_extract_int_near(body, "\"position\"", "character", &ch);
  lsp_doc_t *doc = doc_find(uri);
  char *word = word_at_position(doc ? doc->text : NULL, line, ch, NULL, NULL);
  sbuf_t result = {0};
  bool first = true;
  sb_append(&result, "[");
  for (size_t i = 0; i < g_docs_len; ++i) {
    append_text_references(&result, &first, g_docs[i].uri, g_docs[i].text, word);
  }
  sb_append(&result, "]");
  send_result(id, result.data);
  free(result.data);
  free(word);
  free(uri);
}

static int semantic_token_type(const char *text, size_t start, size_t len, char next_sig) {
  if (!text || len == 0)
    return 8;
  if (len == 2 && text[start] == ';' && text[start + 1] == ';')
    return 17;
  if (text[start] == '"' || text[start] == '\'')
    return 18;
  if (isdigit((unsigned char)text[start]))
    return 19;
  const char *keywords[] = {"use", "module", "fn", "extern", "def", "mut", "if", "elif",
                            "else", "while", "for", "return", "layout", "struct", "enum",
                            "impl", "operator", "match", "case", "comptime", "true", "false",
                            "nil", "and", "or", "not"};
  for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
    if (strlen(keywords[i]) == len && strncmp(text + start, keywords[i], len) == 0)
      return 15;
  }
  if (next_sig == '(')
    return 12;
  if (isupper((unsigned char)text[start]))
    return 1;
  return 8;
}

static void append_semantic_token(sbuf_t *b, bool *first, int *last_line, int *last_col,
                                  int line, int col, int len, int type, int mods) {
  if (!b || !first || len <= 0)
    return;
  int dl = line - *last_line;
  int dc = dl == 0 ? col - *last_col : col;
  if (!*first)
    sb_append(b, ",");
  sb_appendf(b, "%d,%d,%d,%d,%d", dl, dc, len, type, mods);
  *first = false;
  *last_line = line;
  *last_col = col;
}

static void handle_semantic_tokens(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  lsp_doc_t *doc = doc_find(uri);
  sbuf_t result = {0};
  sb_append(&result, "{\"data\":[");
  bool first = true;
  int line = 0, col = 0, last_line = 0, last_col = 0, emitted = 0;
  const char *text = doc ? doc->text : NULL;
  for (size_t i = 0; text && text[i] && emitted < 6000;) {
    if (text[i] == '\n') {
      line++;
      col = 0;
      i++;
      continue;
    }
    if (isspace((unsigned char)text[i])) {
      col++;
      i++;
      continue;
    }
    if (text[i] == ';' && text[i + 1] == ';') {
      size_t start = i;
      int start_col = col;
      while (text[i] && text[i] != '\n') {
        i++;
        col++;
      }
      append_semantic_token(&result, &first, &last_line, &last_col, line, start_col,
                            (int)(i - start), 17, 0);
      emitted++;
      continue;
    }
    if (text[i] == '"' || text[i] == '\'') {
      char quote = text[i++];
      int start_col = col++;
      size_t start = i - 1;
      while (text[i] && text[i] != '\n') {
        char c = text[i++];
        col++;
        if (c == '\\' && text[i]) {
          i++;
          col++;
          continue;
        }
        if (c == quote)
          break;
      }
      append_semantic_token(&result, &first, &last_line, &last_col, line, start_col,
                            (int)(i - start), 18, 0);
      emitted++;
      continue;
    }
    if (isalnum((unsigned char)text[i]) || text[i] == '_') {
      size_t start = i;
      int start_col = col;
      while (isalnum((unsigned char)text[i]) || text[i] == '_' || text[i] == '.') {
        i++;
        col++;
      }
      size_t end = i;
      while (text[i] == ' ' || text[i] == '\t')
        i++;
      int type = semantic_token_type(text, start, end - start, text[i]);
      append_semantic_token(&result, &first, &last_line, &last_col, line, start_col,
                            (int)(end - start), type, 0);
      emitted++;
      col += (int)(i - end);
      continue;
    }
    col++;
    i++;
  }
  sb_append(&result, "]}");
  send_result(id, result.data);
  free(result.data);
  free(uri);
}

static const char *literal_type_hint(const char *expr) {
  if (!expr)
    return NULL;
  while (*expr && isspace((unsigned char)*expr))
    expr++;
  if (*expr == '"')
    return ": str";
  if (*expr == '[')
    return ": list";
  if (*expr == '{' || strncmp(expr, "dict(", 5) == 0)
    return ": dict";
  if (strncmp(expr, "true", 4) == 0 || strncmp(expr, "false", 5) == 0)
    return ": bool";
  if (isdigit((unsigned char)*expr) || ((*expr == '-' || *expr == '+') && isdigit((unsigned char)expr[1]))) {
    return strchr(expr, '.') ? ": f64" : ": int";
  }
  return NULL;
}

static void handle_inlay_hints(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  lsp_doc_t *doc = doc_find(uri);
  sbuf_t result = {0};
  sb_append(&result, "[");
  bool first = true;
  int line_no = 0;
  const char *p = doc ? doc->text : NULL;
  while (p && *p) {
    const char *line = p;
    const char *line_end = strchr(p, '\n');
    size_t len = line_end ? (size_t)(line_end - line) : strlen(line);
    char buf[2048];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, line, n);
    buf[n] = '\0';
    char *s = buf;
    while (*s && isspace((unsigned char)*s))
      s++;
    bool binding = false;
    if (strncmp(s, "def ", 4) == 0) {
      s += 4;
      binding = true;
    } else if (strncmp(s, "mut ", 4) == 0) {
      s += 4;
      binding = true;
    }
    if (binding) {
      char *name = s;
      while (*s && (isalnum((unsigned char)*s) || *s == '_'))
        s++;
      int end_col = (int)(s - buf);
      while (*s && isspace((unsigned char)*s))
        s++;
      if (*s != ':' && *s == '=') {
        const char *hint = literal_type_hint(s + 1);
        if (hint && name != s) {
          if (!first)
            sb_append(&result, ",");
          sb_appendf(&result, "{\"position\":{\"line\":%d,\"character\":%d},\"label\":",
                     line_no, end_col);
          sb_append_json(&result, hint);
          sb_append(&result, ",\"kind\":1,\"paddingLeft\":true,\"paddingRight\":true}");
          first = false;
        }
      }
    }
    if (!line_end)
      break;
    p = line_end + 1;
    line_no++;
  }
  sb_append(&result, "]");
  send_result(id, result.data);
  free(result.data);
  free(uri);
}

static void handle_folding_ranges(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  lsp_doc_t *doc = doc_find(uri);
  sbuf_t result = {0};
  sb_append(&result, "[");
  bool first = true;
  int stack[256];
  int sp = 0;
  int line = 0;
  for (const char *p = doc ? doc->text : NULL; p && *p; p++) {
    if (*p == '{' && sp < 256)
      stack[sp++] = line;
    else if (*p == '}' && sp > 0) {
      int start = stack[--sp];
      if (line > start) {
        if (!first)
          sb_append(&result, ",");
        sb_appendf(&result, "{\"startLine\":%d,\"endLine\":%d,\"kind\":\"region\"}", start, line);
        first = false;
      }
    } else if (*p == '\n') {
      line++;
    }
  }
  sb_append(&result, "]");
  send_result(id, result.data);
  free(result.data);
  free(uri);
}

static void handle_code_actions(const char *id, const char *body) {
  (void)body;
  send_result(id,
              "[{\"title\":\"Format document with ny fmt\",\"kind\":\"source.format\","
              "\"command\":{\"title\":\"ny fmt\",\"command\":\"nytrix.format\"}},"
              "{\"title\":\"Organize imports\",\"kind\":\"source.organizeImports\","
              "\"command\":{\"title\":\"Organize imports\",\"command\":\"nytrix.organizeImports\"}},"
              "{\"title\":\"Run compiler check\",\"kind\":\"quickfix\","
              "\"command\":{\"title\":\"ny check\",\"command\":\"nytrix.check\"}}]");
}

static void handle_rename(const char *id, const char *body) {
  char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
  char *new_name = json_extract_string(body, "newName");
  int line = 0, ch = 0;
  json_extract_int_near(body, "\"position\"", "line", &line);
  json_extract_int_near(body, "\"position\"", "character", &ch);
  lsp_doc_t *doc = doc_find(uri);
  char *word = word_at_position(doc ? doc->text : NULL, line, ch, NULL, NULL);
  sbuf_t result = {0};
  sb_append(&result, "{\"changes\":{");
  bool first_uri = true;
  for (size_t d = 0; word && new_name && *new_name && d < g_docs_len; ++d) {
    sbuf_t edits = {0};
    bool first_edit = true;
    append_text_references(&edits, &first_edit, g_docs[d].uri, g_docs[d].text, word);
    if (edits.len > 0) {
      if (!first_uri)
        sb_append(&result, ",");
      sb_append_json(&result, g_docs[d].uri);
      sb_append(&result, ":[");
      const char *p = edits.data;
      bool first = true;
      while (p && *p) {
        const char *range = strstr(p, "\"range\":");
        if (!range)
          break;
        range += 8;
        const char *end = strstr(range, "}");
        end = end ? strstr(end + 1, "}") : NULL;
        if (!end)
          break;
        end++;
        if (!first)
          sb_append(&result, ",");
        sb_append(&result, "{\"range\":");
        sb_append_n(&result, range, (size_t)(end - range));
        sb_append(&result, ",\"newText\":");
        sb_append_json(&result, new_name);
        sb_append(&result, "}");
        first = false;
        p = end;
      }
      sb_append(&result, "]");
      first_uri = false;
    }
    free(edits.data);
  }
  sb_append(&result, "}}");
  send_result(id, result.data);
  free(result.data);
  free(word);
  free(new_name);
  free(uri);
}

static void handle_request(const char *body) {
  if (!body)
    return;
  char *method = json_extract_string(body, "method");
  char *id = json_extract_id(body);
  if (!method) {
    free(id);
    return;
  }
  if (strcmp(method, "initialize") == 0) {
    send_result(id,
                "{\"capabilities\":{"
                "\"textDocumentSync\":{\"openClose\":true,\"change\":1,"
                "\"save\":{\"includeText\":true}},"
                "\"hoverProvider\":true,\"definitionProvider\":true,"
                "\"referencesProvider\":true,\"renameProvider\":{\"prepareProvider\":false},"
                "\"documentSymbolProvider\":true,\"workspaceSymbolProvider\":true,"
                "\"codeActionProvider\":{\"codeActionKinds\":[\"quickfix\",\"source.format\",\"source.organizeImports\"]},"
                "\"foldingRangeProvider\":true,\"inlayHintProvider\":true,"
                "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":[\"namespace\",\"type\",\"class\",\"enum\",\"interface\",\"struct\",\"typeParameter\",\"parameter\",\"variable\",\"property\",\"enumMember\",\"event\",\"function\",\"method\",\"macro\",\"keyword\",\"modifier\",\"comment\",\"string\",\"number\",\"operator\",\"decorator\"],\"tokenModifiers\":[\"declaration\",\"definition\",\"readonly\",\"static\",\"deprecated\",\"abstract\",\"async\",\"modification\",\"documentation\",\"defaultLibrary\"]},\"full\":true},"
                "\"completionProvider\":{\"resolveProvider\":false,\"triggerCharacters\":[\".\"]},"
                "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]}}}");
  } else if (strcmp(method, "textDocument/didOpen") == 0 ||
             strcmp(method, "textDocument/didChange") == 0) {
    char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
    char *text = NULL;
    const char *changes = strstr(body, "\"contentChanges\"");
    if (changes) {
      text = json_extract_string(changes, "text");
    } else {
      text = json_extract_string_near(body, "\"textDocument\"", "text");
    }
    if (uri && text)
      doc_put(uri, text);
    lsp_doc_t *doc = uri ? doc_find(uri) : NULL;
    check_document(uri, text ? text : (doc ? doc->text : NULL));
    free(text);
    free(uri);
  } else if (strcmp(method, "textDocument/didSave") == 0) {
    char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
    char *text = json_extract_string(body, "text");
    if (!text)
      text = read_uri_text(uri);
    if (uri && text)
      doc_put(uri, text);
    lsp_doc_t *doc = uri ? doc_find(uri) : NULL;
    check_document(uri, text ? text : (doc ? doc->text : NULL));
    free(text);
    free(uri);
  } else if (strcmp(method, "textDocument/didClose") == 0) {
    char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
    lsp_diag_vec_t clear = {0};
    publish_diagnostics(uri, &clear);
    doc_remove(uri);
    free(uri);
  } else if (strcmp(method, "textDocument/hover") == 0) {
    handle_hover(id, body);
  } else if (strcmp(method, "textDocument/definition") == 0) {
    handle_definition(id, body);
  } else if (strcmp(method, "textDocument/references") == 0) {
    handle_references(id, body);
  } else if (strcmp(method, "textDocument/rename") == 0) {
    handle_rename(id, body);
  } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
    handle_document_symbols(id, body);
  } else if (strcmp(method, "workspace/symbol") == 0) {
    handle_workspace_symbols(id, body);
  } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
    handle_semantic_tokens(id, body);
  } else if (strcmp(method, "textDocument/inlayHint") == 0) {
    handle_inlay_hints(id, body);
  } else if (strcmp(method, "textDocument/foldingRange") == 0) {
    handle_folding_ranges(id, body);
  } else if (strcmp(method, "textDocument/codeAction") == 0) {
    handle_code_actions(id, body);
  } else if (strcmp(method, "textDocument/completion") == 0) {
    handle_completion(id);
  } else if (strcmp(method, "textDocument/signatureHelp") == 0) {
    handle_signature(id, body);
  } else if (strcmp(method, "shutdown") == 0) {
    char response[128];
    snprintf(response, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":null}",
             id ? id : "null");
    send_response(response);
  } else if (strcmp(method, "exit") == 0) {
    free(method);
    free(id);
    exit(0);
  }
  free(method);
  free(id);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ny_intern_init();
  while (1) {
    char *msg = read_message();
    if (!msg)
      break;
    handle_request(msg);
    free(msg);
  }
  doc_clear_all();
  ny_intern_cleanup();
  return 0;
}
