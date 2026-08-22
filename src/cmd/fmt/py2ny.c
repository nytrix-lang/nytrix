/*
 * py2ny.c - Python to Nytrix converter for ny-fmt.
 *
 * A tokenizer (with Python INDENT/DEDENT/NEWLINE) plus a recursive-descent
 * parser that translates a practical Python subset into Nytrix. Unsupported
 * constructs become explicit `;; py2ny: unsupported:` marker comments (with
 * line numbers) so the emitted file stays readable and never drops a
 * construct silently.
 *
 * Included by src/cmd/fmt/init.c; all symbols are file-local.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * string builder
 */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} pny_sb_t;

static int pny_sb_grow(pny_sb_t *b, size_t need) {
  if (need <= b->cap)
    return 1;
  size_t nc = b->cap ? b->cap : 256;
  while (nc < need)
    nc *= 2;
  char *p = (char *)realloc(b->data, nc);
  if (!p)
    return 0;
  b->data = p;
  b->cap = nc;
  return 1;
}

static void pny_sb_add(pny_sb_t *b, const char *s) {
  size_t n = strlen(s);
  if (pny_sb_grow(b, b->len + n + 1)) {
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
  }
}

static void pny_sb_addn(pny_sb_t *b, const char *s, size_t n) {
  if (pny_sb_grow(b, b->len + n + 1)) {
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
  }
}

static void pny_sb_clear(pny_sb_t *b) {
  if (b->data)
    b->data[0] = 0;
  b->len = 0;
}

/*
 * tokenizer
 */

#define PN_IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define PN_IS_ALPHA(c) \
  (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || (c) == '_')
#define PN_IS_ALNUM(c) (PN_IS_ALPHA(c) || PN_IS_DIGIT(c))
#define PN_IS_XDIGIT(c) \
  (PN_IS_DIGIT(c) || ((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F'))
#define PN_IS_ODIGIT(c) ((c) >= '0' && (c) <= '7')

enum {
  PNTK_EOF = 0,
  PNTK_NEWLINE, /* end of logical line */
  PNTK_INDENT,
  PNTK_DEDENT,
  PNTK_IDENT, /* identifiers and keywords (dispatched on text) */
  PNTK_INT,
  PNTK_FLT,
  PNTK_STR, /* string literal, including any prefix */
  PNTK_OP    /* operator or punctuator */
};

typedef struct {
  int kind;
  const char *s; /* start in source */
  size_t n;      /* length */
  int line;
} pny_tok_t;

typedef struct {
  pny_tok_t *items;
  size_t len;
  size_t cap;
} pny_toks_t;

static void pny_toks_push(pny_toks_t *t, int kind, const char *s, size_t n,
                          int line) {
  if (t->len + 1 > t->cap) {
    size_t nc = t->cap ? t->cap * 2 : 64;
    pny_tok_t *p = (pny_tok_t *)realloc(t->items, nc * sizeof(pny_tok_t));
    if (!p)
      return;
    t->items = p;
    t->cap = nc;
  }
  t->items[t->len].kind = kind;
  t->items[t->len].s = s;
  t->items[t->len].n = n;
  t->items[t->len].line = line;
  t->len++;
}

static void pny_toks_free(pny_toks_t *t) {
  free(t->items);
  t->items = NULL;
  t->len = t->cap = 0;
}

static const char *const pny_ops[] = {
    "**=", "//=", ">>=", "<<=", "...",
    "**", "//", ">>", "<<", "<=", ">=", "==", "!=",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "->", ":=", "@=",
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "<", ">", "=",
    "(", ")", "[", "]", "{", "}", ",", ":", ".", "@",
    NULL};

static size_t pny_op_len(const char *s, size_t n) {
  for (size_t k = 0; pny_ops[k]; k++) {
    const char *op = pny_ops[k];
    size_t ol = strlen(op);
    if (ol <= n && memcmp(s, op, ol) == 0)
      return ol;
  }
  return 0;
}

static int pny_is_prefix(const char *s, size_t n) {
  if (n == 0 || n > 2)
    return 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c != 'r' && c != 'R' && c != 'b' && c != 'B' && c != 'u' && c != 'U' &&
        c != 'f' && c != 'F')
      return 0;
  }
  return 1;
}

/*
 * scan a string literal beginning at `start` (which may include a prefix).
 * emits one PNTK_STR token; returns the index past the closing quote.
 * updates *line for multiline (triple-quoted) strings.
 */
static size_t pny_scan_string(const char *src, size_t start, size_t n,
                              int *line, pny_toks_t *out) {
  int start_line = *line;
  size_t i = start;
  while (i < n && PN_IS_ALPHA(src[i]))
    i++; /* skip prefix letters */
  if (i >= n) {
    pny_toks_push(out, PNTK_STR, src + start, i - start, start_line);
    return i;
  }
  char q = src[i];
  int triple = (i + 2 < n && src[i + 1] == q && src[i + 2] == q);
  if (triple) {
    i += 3;
    while (i < n) {
      if (src[i] == '\\' && i + 1 < n) {
        if (src[i + 1] == '\n')
          (*line)++;
        i += 2;
        continue;
      }
      if (src[i] == q && i + 2 < n && src[i + 1] == q && src[i + 2] == q) {
        i += 3;
        break;
      }
      if (src[i] == q && i + 2 >= n) {
        i++;
        break;
      }
      if (src[i] == '\n')
        (*line)++;
      i++;
    }
  } else {
    i++;
    while (i < n) {
      if (src[i] == '\\' && i + 1 < n) {
        i += 2;
        continue;
      }
      if (src[i] == q) {
        i++;
        break;
      }
      if (src[i] == '\n' || src[i] == '\r')
        break; /* unterminated single-line string */
      i++;
    }
  }
  pny_toks_push(out, PNTK_STR, src + start, i - start, start_line);
  return i;
}

static void pny_lex(const char *src, size_t n, pny_toks_t *out) {
  int indent_stack[256];
  int isp = 0;
  indent_stack[0] = 0;
  int bracket = 0;
  size_t i = 0;
  int line = 1;
  int at_line_start = 1;

  while (i < n) {
    if (at_line_start && bracket == 0) {
      /*
       * peek: is this line blank or comment-only?
       */
      size_t j = i;
      while (j < n && (src[j] == ' ' || src[j] == '\t'))
        j++;
      if (j >= n || src[j] == '\n' || src[j] == '\r' || src[j] == '#') {
        i = j;
        if (i < n && src[i] == '#')
          while (i < n && src[i] != '\n' && src[i] != '\r')
            i++;
        if (i < n && src[i] == '\r')
          i++;
        if (i < n && src[i] == '\n') {
          i++;
          line++;
        }
        continue; /* stay at_line_start */
      }
      /*
       * non-blank line: compute indentation (tab = 8, column-aligned)
       */
      int ind = 0;
      while (i < n && src[i] == ' ') {
        ind++;
        i++;
      }
      while (i < n && src[i] == '\t') {
        ind += 8 - (ind % 8);
        i++;
      }
      if (ind > indent_stack[isp]) {
        if (isp + 1 < (int)(sizeof(indent_stack) / sizeof(int))) {
          isp++;
          indent_stack[isp] = ind;
          pny_toks_push(out, PNTK_INDENT, src + i, 0, line);
        }
      } else if (ind < indent_stack[isp]) {
        while (isp > 0 && indent_stack[isp] > ind) {
          isp--;
          pny_toks_push(out, PNTK_DEDENT, src + i, 0, line);
        }
      }
      at_line_start = 0;
      continue; /* re-process from the first non-ws char */
    }

    char c = src[i];
    if (c == '\r') {
      i++;
      continue;
    }
    if (c == '\\' && i + 1 < n && (src[i + 1] == '\n' || src[i + 1] == '\r')) {
      i++;
      if (i < n && src[i] == '\r')
        i++;
      if (i < n && src[i] == '\n') {
        i++;
        line++;
      }
      continue; /* explicit line continuation */
    }
    if (c == '\n') {
      if (bracket == 0) {
        pny_toks_push(out, PNTK_NEWLINE, src + i, 0, line);
        i++;
        line++;
        at_line_start = 1;
      } else {
        i++;
        line++;
      }
      continue;
    }
    if (c == '#') {
      while (i < n && src[i] != '\n' && src[i] != '\r')
        i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\f') {
      i++;
      continue;
    }
    if (PN_IS_ALPHA(c)) {
      size_t start = i;
      while (i < n && PN_IS_ALNUM(src[i]))
        i++;
      if (i < n && (src[i] == '"' || src[i] == '\'') &&
          pny_is_prefix(src + start, i - start)) {
        i = pny_scan_string(src, start, n, &line, out);
        continue;
      }
      pny_toks_push(out, PNTK_IDENT, src + start, i - start, line);
      continue;
    }
    if (PN_IS_DIGIT(c) || (c == '.' && i + 1 < n && PN_IS_DIGIT(src[i + 1]))) {
      size_t start = i;
      int isflt = 0;
      if (c == '0' && i + 1 < n &&
          (src[i + 1] == 'x' || src[i + 1] == 'X')) {
        i += 2;
        while (i < n && (PN_IS_XDIGIT(src[i]) || src[i] == '_'))
          i++;
      } else if (c == '0' && i + 1 < n &&
                 (src[i + 1] == 'o' || src[i + 1] == 'O')) {
        i += 2;
        while (i < n && (PN_IS_ODIGIT(src[i]) || src[i] == '_'))
          i++;
      } else if (c == '0' && i + 1 < n &&
                 (src[i + 1] == 'b' || src[i + 1] == 'B')) {
        i += 2;
        while (i < n && (src[i] == '0' || src[i] == '1' || src[i] == '_'))
          i++;
      } else {
        while (i < n && (PN_IS_DIGIT(src[i]) || src[i] == '_'))
          i++;
        if (i < n && src[i] == '.') {
          isflt = 1;
          i++;
          while (i < n && (PN_IS_DIGIT(src[i]) || src[i] == '_'))
            i++;
        }
        if (i < n && (src[i] == 'e' || src[i] == 'E')) {
          isflt = 1;
          i++;
          if (i < n && (src[i] == '+' || src[i] == '-'))
            i++;
          while (i < n && (PN_IS_DIGIT(src[i]) || src[i] == '_'))
            i++;
        }
        if (i < n && (src[i] == 'j' || src[i] == 'J')) {
          isflt = 1;
          i++;
        }
      }
      pny_toks_push(out, isflt ? PNTK_FLT : PNTK_INT, src + start, i - start,
                    line);
      continue;
    }
    if (c == '"' || c == '\'') {
      i = pny_scan_string(src, i, n, &line, out);
      continue;
    }
    size_t ol = pny_op_len(src + i, n - i);
    if (ol > 0) {
      if (c == '(' || c == '[' || c == '{')
        bracket++;
      else if (c == ')' || c == ']' || c == '}') {
        if (bracket > 0)
          bracket--;
      }
      pny_toks_push(out, PNTK_OP, src + i, ol, line);
      i += ol;
      continue;
    }
    /*
     * unknown byte: skip
     */
    i++;
  }

  if (out->len == 0 || out->items[out->len - 1].kind != PNTK_NEWLINE)
    pny_toks_push(out, PNTK_NEWLINE, src + n, 0, line);
  while (isp > 0) {
    isp--;
    pny_toks_push(out, PNTK_DEDENT, src + n, 0, line);
  }
  pny_toks_push(out, PNTK_EOF, src + n, 0, line);
}

/*
 * parser / emitter
 */

typedef struct {
  const pny_tok_t *toks;
  size_t nt;
  size_t ti;
  pny_sb_t *out; /* current destination (top output or main_buf) */
  pny_sb_t ln;   /* current line buffer */
  pny_sb_t warn; /* pending warning text (without leading ';;') */
  int indent;
  int errors;
  pny_sb_t main_buf;       /* collects top-level executable statements */
  char **decls;            /* names declared in current scope */
  size_t ndecls, cdecls;
  int noted_truediv;       /* emitted the Python `/` vs Ny int-division note */
} pny_t;

static const pny_tok_t *pny_cur(const pny_t *p) {
  return &p->toks[p->ti];
}

static int pny_at_kind(const pny_t *p, int kind) {
  return p->toks[p->ti].kind == kind;
}

static int pny_word_eq(const pny_tok_t *t, const char *w) {
  size_t n = strlen(w);
  return t->kind == PNTK_IDENT && t->n == n && memcmp(t->s, w, n) == 0;
}

static int pny_at_word(const pny_t *p, const char *w) {
  return pny_word_eq(pny_cur(p), w);
}

static int pny_at_op(const pny_t *p, const char *w) {
  const pny_tok_t *t = pny_cur(p);
  size_t n = strlen(w);
  return t->kind == PNTK_OP && t->n == n && memcmp(t->s, w, n) == 0;
}

static void pny_advance(pny_t *p) {
  if (p->ti < p->nt - 1)
    p->ti++;
}

static void pny_emit(pny_t *p, const char *s) {
  pny_sb_add(&p->ln, s);
}

static void pny_emit_n(pny_t *p, const char *s, size_t n) {
  pny_sb_addn(&p->ln, s, n);
}

static void pny_emit_sb(pny_t *p, const pny_sb_t *b) {
  pny_sb_add(&p->ln, b->data ? b->data : "");
}

/*
 * append to either a buffer or the current line
 */
static void pny_ebuf(pny_t *p, pny_sb_t *out, const char *s) {
  if (out)
    pny_sb_add(out, s);
  else
    pny_sb_add(&p->ln, s);
}

static void pny_ebuf_n(pny_t *p, pny_sb_t *out, const char *s, size_t n) {
  if (out)
    pny_sb_addn(out, s, n);
  else
    pny_sb_addn(&p->ln, s, n);
}

static void pny_warn_at(pny_t *p, int line, const char *kind, const char *fmt,
                        ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  pny_sb_add(&p->warn, " py2ny: ");
  pny_sb_add(&p->warn, kind);
  pny_sb_add(&p->warn, ": ");
  pny_sb_add(&p->warn, buf);
  char lb[32];
  snprintf(lb, sizeof(lb), " (line %d)", line);
  pny_sb_add(&p->warn, lb);
  if (strcmp(kind, "unsupported") == 0)
    p->errors++;
}

static void pny_flush(pny_t *p) {
  if (p->ln.len > 0 || p->warn.len > 0) {
    if (p->warn.len > 0) {
      for (int i = 0; i < p->indent; i++)
        pny_sb_add(p->out, "   ");
      pny_sb_add(p->out, ";;");
      pny_sb_add(p->out, p->warn.data);
      pny_sb_add(p->out, "\n");
    }
    for (int i = 0; i < p->indent; i++)
      pny_sb_add(p->out, "   ");
    pny_sb_add(p->out, p->ln.data ? p->ln.data : "");
    pny_sb_add(p->out, "\n");
  }
  pny_sb_clear(&p->ln);
  pny_sb_clear(&p->warn);
}

/*
 * declared names tracking (per-scope)
 */

static int pny_decl_known(pny_t *p, const char *name) {
  for (size_t i = 0; i < p->ndecls; i++)
    if (strcmp(p->decls[i], name) == 0)
      return 1;
  return 0;
}

static void pny_decl_add(pny_t *p, const char *name) {
  if (pny_decl_known(p, name))
    return;
  if (p->ndecls == p->cdecls) {
    size_t nc = p->cdecls ? p->cdecls * 2 : 8;
    char **np = (char **)realloc(p->decls, nc * sizeof(char *));
    if (!np)
      return;
    p->decls = np;
    p->cdecls = nc;
  }
  p->decls[p->ndecls++] = strdup(name);
}

static void pny_decl_clear(pny_t *p) {
  for (size_t i = 0; i < p->ndecls; i++)
    free(p->decls[i]);
  p->ndecls = 0;
}

/*
 * forward declarations
 */
static void pny_expr(pny_t *p, pny_sb_t *out);
static void pny_factor(pny_t *p, pny_sb_t *out);
static void pny_binexpr(pny_t *p, pny_sb_t *out, int minprec);
static void pny_parse_stmt(pny_t *p);
static void pny_parse_simple_stmt(pny_t *p);
static void pny_parse_suite(pny_t *p);

/*
 * atom emission helpers
 */

static void pny_emit_number(pny_t *p, pny_sb_t *out, const pny_tok_t *t) {
  size_t n = t->n;
  int dropped_complex = 0;
  if (n > 0 && (t->s[n - 1] == 'j' || t->s[n - 1] == 'J')) {
    n--;
    dropped_complex = 1;
  }
  for (size_t k = 0; k < n; k++)
    if (t->s[k] != '_')
      pny_ebuf_n(p, out, &t->s[k], 1);
  if (dropped_complex)
    pny_warn_at(p, t->line, "note", "complex number suffix dropped");
}

static void pny_emit_str(pny_t *p, pny_sb_t *out, const pny_tok_t *t) {
  const char *s = t->s;
  size_t n = t->n;
  size_t i = 0;
  int is_f = 0, other = 0;
  while (i < n && PN_IS_ALPHA(s[i])) {
    char c = (char)tolower((unsigned char)s[i]);
    if (c == 'f')
      is_f = 1;
    else
      other = 1;
    i++;
  }
  if (other)
    pny_warn_at(p, t->line, "note", "string prefix normalized");
  if (is_f)
    pny_ebuf(p, out, "f");
  pny_ebuf_n(p, out, s + i, n - i); /* quotes + body, verbatim */
}

/*
 * expression parser
 */

static void pny_atom(pny_t *p, pny_sb_t *out);

static void pny_postfix(pny_t *p, pny_sb_t *out) {
  pny_atom(p, out);
  for (;;) {
    if (pny_at_op(p, "(")) {
      pny_advance(p);
      pny_ebuf(p, out, "(");
      if (!pny_at_op(p, ")")) {
        for (;;) {
          if (pny_at_op(p, "**")) {
            pny_ebuf(p, out, "**");
            pny_advance(p);
          } else if (pny_at_op(p, "*")) {
            pny_ebuf(p, out, "*");
            pny_advance(p);
          }
          pny_expr(p, out);
          if (pny_at_op(p, "=")) {
            pny_ebuf(p, out, " = ");
            pny_advance(p);
            pny_expr(p, out);
          }
          if (pny_at_op(p, ",")) {
            pny_ebuf(p, out, ", ");
            pny_advance(p);
            if (pny_at_op(p, ")"))
              break;
          } else {
            break;
          }
        }
      }
      if (pny_at_op(p, ")"))
        pny_advance(p);
      pny_ebuf(p, out, ")");
    } else if (pny_at_op(p, "[")) {
      pny_advance(p);
      pny_ebuf(p, out, "[");
      if (!pny_at_op(p, "]") && !pny_at_op(p, ":"))
        pny_expr(p, out);
      if (pny_at_op(p, ":")) {
        pny_ebuf(p, out, ":");
        pny_advance(p);
        if (!pny_at_op(p, "]") && !pny_at_op(p, ":"))
          pny_expr(p, out);
        if (pny_at_op(p, ":")) {
          pny_ebuf(p, out, ":");
          pny_advance(p);
          if (!pny_at_op(p, "]"))
            pny_expr(p, out);
        }
      }
      if (pny_at_op(p, "]"))
        pny_advance(p);
      pny_ebuf(p, out, "]");
    } else if (pny_at_op(p, ".")) {
      pny_advance(p);
      pny_ebuf(p, out, ".");
      const pny_tok_t *t = pny_cur(p);
      if (t->kind == PNTK_IDENT || t->kind == PNTK_INT) {
        pny_ebuf_n(p, out, t->s, t->n);
        pny_advance(p);
      }
    } else {
      break;
    }
  }
}

static void pny_power(pny_t *p, pny_sb_t *out) {
  pny_sb_t base = {0};
  pny_postfix(p, &base);
  if (pny_at_op(p, "**")) {
    pny_advance(p);
    pny_sb_t r = {0};
    /*
     * Python `**` is right-associative; its right operand is a unary/factor
     * (which itself may contain `**`), NOT a full additive expression.
     * Parsing `factor` here keeps `x ** 2 + 3` as `(x ** 2) + 3`.
     */
    pny_factor(p, &r);
    /*
     * Wrap the whole power in parens: Ny `^` is exponentiation with its own
     * parser-defined precedence (looser than `+`/`*`), so without outer
     * grouping `(x) ^ (2) + 3` would parse as `x ^ (2 + 3)`. Parenthesizing
     * both the whole expression and each operand preserves Python's
     * tighter-than-multiply binding for `**`.
     */
    pny_ebuf(p, out, "((");
    pny_ebuf(p, out, base.data ? base.data : "");
    pny_ebuf(p, out, ") ^ (");
    pny_ebuf(p, out, r.data ? r.data : "");
    pny_ebuf(p, out, "))");
    free(r.data);
    free(base.data);
    return;
  }
  pny_ebuf(p, out, base.data ? base.data : "");
  free(base.data);
}

static void pny_factor(pny_t *p, pny_sb_t *out) {
  if (pny_at_op(p, "+") || pny_at_op(p, "-") || pny_at_op(p, "~")) {
    char op[2] = {pny_cur(p)->s[0], 0};
    pny_advance(p);
    pny_sb_t operand = {0};
    pny_factor(p, &operand);
    pny_ebuf(p, out, op);
    pny_ebuf(p, out, operand.data ? operand.data : "");
    free(operand.data);
    return;
  }
  pny_power(p, out);
}

struct pny_opinfo {
  const char *py;
  const char *ny;
  int prec;
};

static const struct pny_opinfo pny_bin_ops[] = {
    {"|", "|", 4},    {"^", "^^", 5},  {"&", "&", 6},
    {"<<", "<<", 7},  {">>", ">>", 7}, {"+", "+", 8},
    {"-", "-", 8},    {"*", "*", 9},   {"/", "/", 9},
    {"//", "/", 9},   {"%", "%", 9},   {NULL, NULL, 0},
};

static const struct pny_opinfo *pny_bin_lookup(const pny_tok_t *t) {
  if (t->kind != PNTK_OP)
    return NULL;
  for (int k = 0; pny_bin_ops[k].py; k++) {
    size_t n = strlen(pny_bin_ops[k].py);
    if (t->n == n && memcmp(t->s, pny_bin_ops[k].py, n) == 0)
      return &pny_bin_ops[k];
  }
  return NULL;
}

static void pny_binexpr(pny_t *p, pny_sb_t *out, int minprec) {
  pny_sb_t left = {0};
  pny_factor(p, &left);
  for (;;) {
    const struct pny_opinfo *oi = pny_bin_lookup(pny_cur(p));
    if (!oi || oi->prec < minprec)
      break;
    /*
     * Python `/` is true (float) division; Ny `/` on ints is integer
     * division. Flag once so the user can review the affected expressions
     * rather than silently producing a different result.
     */
    if (oi->py[0] == '/' && oi->py[1] == 0 && !p->noted_truediv) {
      p->noted_truediv = 1;
      pny_warn_at(p, pny_cur(p)->line, "note",
                  "Python `/` is true division; Ny `/` on ints truncates — "
                  "verify operand types");
    }
    pny_advance(p);
    pny_sb_t right = {0};
    pny_binexpr(p, &right, oi->prec + 1);
    pny_sb_add(&left, " ");
    pny_sb_add(&left, oi->ny);
    pny_sb_add(&left, " ");
    pny_sb_add(&left, right.data ? right.data : "");
    free(right.data);
  }
  pny_ebuf(p, out, left.data ? left.data : "");
  free(left.data);
}

static int pny_word_at_off(const pny_t *p, size_t off, const char *w) {
  if (p->ti + off >= p->nt)
    return 0;
  return pny_word_eq(&p->toks[p->ti + off], w);
}

/*
 * Take one comparison operator.
 * Returns:
 *   0  = no comparison operator here
 *   1  = a relational/equality op; its Ny text is in opbuf (<, ==, etc.)
 *   2  = `in`  membership; caller emits contains(right, left)
 *   3  = `not in` membership; caller emits !contains(right, left)
 */
static int pny_take_comp_op(pny_t *p, char *opbuf, size_t opsz) {
  if (pny_at_word(p, "is")) {
    pny_advance(p);
    if (pny_at_word(p, "not")) {
      pny_advance(p);
      snprintf(opbuf, opsz, "!=");
    } else {
      snprintf(opbuf, opsz, "==");
    }
    return 1;
  }
  if (pny_at_word(p, "in")) {
    pny_advance(p);
    return 2;
  }
  if (pny_word_at_off(p, 0, "not") && pny_word_at_off(p, 1, "in")) {
    pny_advance(p);
    pny_advance(p);
    return 3;
  }
  static const char *const cmps[] = {"<=", ">=", "==", "!=", "<", ">", NULL};
  for (int k = 0; cmps[k]; k++) {
    if (pny_at_op(p, cmps[k])) {
      pny_advance(p);
      snprintf(opbuf, opsz, "%s", cmps[k]);
      return 1;
    }
  }
  return 0;
}

/*
 * Python `x in xs` / `x not in xs` have no operator form in Ny; lower them to
 * contains(container, item). `left` is the item, the following operand is the
 * container. negate selects `not in`.
 */
static void pny_emit_membership(pny_t *p, pny_sb_t *out, pny_sb_t *left,
                                int negate) {
  pny_sb_t right = {0};
  pny_binexpr(p, &right, 4);
  if (negate)
    pny_ebuf(p, out, "!");
  pny_ebuf(p, out, "contains(");
  pny_ebuf(p, out, right.data ? right.data : "");
  pny_ebuf(p, out, ", ");
  pny_ebuf(p, out, left->data ? left->data : "");
  pny_ebuf(p, out, ")");
  free(right.data);
}

static void pny_comparison(pny_t *p, pny_sb_t *out) {
  pny_sb_t first = {0};
  pny_binexpr(p, &first, 4);
  char opbuf[8];
  int kind = pny_take_comp_op(p, opbuf, sizeof(opbuf));
  if (kind == 0) {
    pny_ebuf(p, out, first.data ? first.data : "");
    free(first.data);
    return;
  }
  if (kind == 2 || kind == 3) {
    pny_emit_membership(p, out, &first, kind == 3);
    free(first.data);
    return;
  }
  /*
   * one or more comparisons: chain with && (a < b && b < c)
   */
  pny_sb_t acc = {0};
  pny_sb_t prev = {0};
  pny_sb_add(&prev, first.data ? first.data : "");
  free(first.data);
  int firstc = 1;
  do {
    if (!firstc)
      pny_sb_add(&acc, " && ");
    firstc = 0;
    pny_sb_add(&acc, prev.data ? prev.data : "");
    pny_sb_add(&acc, " ");
    pny_sb_add(&acc, opbuf);
    pny_sb_add(&acc, " ");
    pny_sb_t right = {0};
    pny_binexpr(p, &right, 4);
    pny_sb_add(&acc, right.data ? right.data : "");
    pny_sb_clear(&prev);
    pny_sb_add(&prev, right.data ? right.data : "");
    free(right.data);
    kind = pny_take_comp_op(p, opbuf, sizeof(opbuf));
    if (kind == 2 || kind == 3) {
      /*
       * trailing membership in a chain: emit contains() for the last link
       */
      pny_sb_t last = {0};
      pny_sb_add(&last, prev.data ? prev.data : "");
      pny_sb_clear(&prev);
      pny_sb_add(&acc, " && ");
      pny_sb_t mb = {0};
      pny_emit_membership(p, &mb, &last, kind == 3);
      pny_sb_add(&acc, mb.data ? mb.data : "");
      free(last.data);
      free(mb.data);
      break;
    }
  } while (kind == 1);
  free(prev.data);
  pny_ebuf(p, out, acc.data ? acc.data : "");
  free(acc.data);
}

static void pny_not_test(pny_t *p, pny_sb_t *out) {
  /*
   * Check for "not in" - this is a single comparison operator in Python.
   * If we see "not" followed by "in", don't treat it as unary "not",
   * fall through to pny_comparison which will handle "not in" as a unit.
   */
  if (pny_at_word(p, "not") && pny_word_at_off(p, 1, "in")) {
    pny_comparison(p, out);
    return;
  }
  if (pny_at_word(p, "not")) {
    int line = pny_cur(p)->line;
    pny_advance(p);
    pny_ebuf(p, out, "!(");
    pny_sb_t inner = {0};
    pny_not_test(p, &inner);
    pny_ebuf(p, out, inner.data ? inner.data : "");
    pny_ebuf(p, out, ")");
    free(inner.data);
    (void)line;
    return;
  }
  pny_comparison(p, out);
}

static void pny_and_test(pny_t *p, pny_sb_t *out) {
  pny_sb_t left = {0};
  pny_not_test(p, &left);
  pny_ebuf(p, out, left.data ? left.data : "");
  free(left.data);
  while (pny_at_word(p, "and")) {
    pny_advance(p);
    pny_sb_t r = {0};
    pny_not_test(p, &r);
    pny_ebuf(p, out, " && ");
    pny_ebuf(p, out, r.data ? r.data : "");
    free(r.data);
  }
}

static void pny_or_test(pny_t *p, pny_sb_t *out) {
  pny_sb_t left = {0};
  pny_and_test(p, &left);
  pny_ebuf(p, out, left.data ? left.data : "");
  free(left.data);
  while (pny_at_word(p, "or")) {
    pny_advance(p);
    pny_sb_t r = {0};
    pny_and_test(p, &r);
    pny_ebuf(p, out, " || ");
    pny_ebuf(p, out, r.data ? r.data : "");
    free(r.data);
  }
}

/*
 * conditional expression: or_test ['if' or_test 'else' expr]
 */
static void pny_ternary(pny_t *p, pny_sb_t *out) {
  pny_sb_t body = {0};
  pny_or_test(p, &body);
  if (pny_at_word(p, "if")) {
    pny_advance(p);
    pny_sb_t cond = {0};
    pny_or_test(p, &cond);
    if (!pny_at_word(p, "else")) {
      pny_warn_at(p, pny_cur(p)->line, "unsupported",
                  "conditional expression without 'else'");
      pny_ebuf(p, out, body.data ? body.data : "");
      free(body.data);
      free(cond.data);
      return;
    }
    pny_advance(p);
    pny_sb_t els = {0};
    pny_ternary(p, &els);
    pny_ebuf(p, out, cond.data ? cond.data : "");
    pny_ebuf(p, out, " ? ");
    pny_ebuf(p, out, body.data ? body.data : "");
    pny_ebuf(p, out, " : ");
    pny_ebuf(p, out, els.data ? els.data : "");
    free(cond.data);
    free(body.data);
    free(els.data);
    return;
  }
  pny_ebuf(p, out, body.data ? body.data : "");
  free(body.data);
}

static void pny_parse_lambda(pny_t *p, pny_sb_t *out) {
  int line = pny_cur(p)->line;
  pny_advance(p); /* lambda */
  pny_ebuf(p, out, "fn(");
  int first = 1;
  while (!pny_at_op(p, ":") && !pny_at_kind(p, PNTK_EOF) &&
         !pny_at_kind(p, PNTK_NEWLINE)) {
    if (!first)
      pny_ebuf(p, out, ", ");
    first = 0;
    if (pny_cur(p)->kind == PNTK_IDENT) {
      pny_ebuf_n(p, out, pny_cur(p)->s, pny_cur(p)->n);
      pny_advance(p);
      if (pny_at_op(p, "=")) {
        pny_ebuf(p, out, " = ");
        pny_advance(p);
        pny_expr(p, out);
      }
    } else {
      pny_advance(p);
    }
    if (pny_at_op(p, ","))
      pny_advance(p);
    else
      break;
  }
  pny_ebuf(p, out, "){ ");
  if (pny_at_op(p, ":"))
    pny_advance(p);
  else
    pny_warn_at(p, line, "unsupported", "lambda without ':'");
  pny_expr(p, out);
  pny_ebuf(p, out, " }");
}

static void pny_expr(pny_t *p, pny_sb_t *out) {
  if (pny_at_word(p, "lambda")) {
    pny_parse_lambda(p, out);
    return;
  }
  pny_ternary(p, out);
}

/*
 * detect a comprehension `for` after the first element; consume to closer
 */
static void pny_skip_comprehension(pny_t *p, char closer) {
  pny_warn_at(p, pny_cur(p)->line, "unsupported", "comprehension");
  int depth = 1;
  while (depth > 0 && !pny_at_kind(p, PNTK_EOF)) {
    if (pny_at_op(p, "[") || pny_at_op(p, "(") || pny_at_op(p, "{"))
      depth++;
    else if (pny_cur(p)->kind == PNTK_OP && pny_cur(p)->n == 1 &&
             pny_cur(p)->s[0] == closer)
      depth--;
    if (depth > 0)
      pny_advance(p);
  }
}

static void pny_atom(pny_t *p, pny_sb_t *out) {
  const pny_tok_t *t = pny_cur(p);
  if (t->kind == PNTK_INT || t->kind == PNTK_FLT) {
    pny_emit_number(p, out, t);
    pny_advance(p);
    return;
  }
  if (t->kind == PNTK_STR) {
    pny_emit_str(p, out, t);
    pny_advance(p);
    return;
  }
  if (t->kind == PNTK_IDENT) {
    if (pny_word_eq(t, "True"))
      pny_ebuf(p, out, "true");
    else if (pny_word_eq(t, "False"))
      pny_ebuf(p, out, "false");
    else if (pny_word_eq(t, "None"))
      pny_ebuf(p, out, "nil");
    else
      pny_ebuf_n(p, out, t->s, t->n);
    pny_advance(p);
    return;
  }
  if (pny_at_op(p, "(")) {
    pny_advance(p);
    pny_ebuf(p, out, "(");
    if (!pny_at_op(p, ")")) {
      pny_expr(p, out);
      if (pny_at_word(p, "for")) {
        pny_skip_comprehension(p, ')');
      } else {
        while (pny_at_op(p, ",")) {
          pny_ebuf(p, out, ", ");
          pny_advance(p);
          if (pny_at_op(p, ")"))
            break;
          pny_expr(p, out);
        }
      }
    }
    if (pny_at_op(p, ")"))
      pny_advance(p);
    pny_ebuf(p, out, ")");
    return;
  }
  if (pny_at_op(p, "[")) {
    pny_advance(p);
    pny_ebuf(p, out, "[");
    if (!pny_at_op(p, "]")) {
      pny_expr(p, out);
      if (pny_at_word(p, "for")) {
        pny_skip_comprehension(p, ']');
      } else {
        while (pny_at_op(p, ",")) {
          pny_ebuf(p, out, ", ");
          pny_advance(p);
          if (pny_at_op(p, "]"))
            break;
          pny_expr(p, out);
        }
      }
    }
    if (pny_at_op(p, "]"))
      pny_advance(p);
    pny_ebuf(p, out, "]");
    return;
  }
  if (pny_at_op(p, "{")) {
    pny_advance(p);
    pny_ebuf(p, out, "{");
    if (!pny_at_op(p, "}")) {
      pny_expr(p, out);
      if (pny_at_word(p, "for")) {
        pny_skip_comprehension(p, '}');
      } else {
        if (pny_at_op(p, ":")) {
          pny_ebuf(p, out, ": ");
          pny_advance(p);
          pny_expr(p, out);
        }
        while (pny_at_op(p, ",")) {
          pny_ebuf(p, out, ", ");
          pny_advance(p);
          if (pny_at_op(p, "}"))
            break;
          pny_expr(p, out);
          if (pny_at_op(p, ":")) {
            pny_ebuf(p, out, ": ");
            pny_advance(p);
            pny_expr(p, out);
          }
        }
      }
    }
    if (pny_at_op(p, "}"))
      pny_advance(p);
    pny_ebuf(p, out, "}");
    return;
  }
  /*
   * unexpected
   */
  pny_warn_at(p, t->line, "unsupported", "unexpected token '%.*s' in expression",
              (int)t->n, t->s);
  pny_advance(p);
}

/*
 * type annotation parsing
 */

static void pny_parse_type(pny_t *p, char *buf, size_t sz) {
  pny_sb_t tb = {0};
  int depth = 0;
  while (!pny_at_kind(p, PNTK_EOF)) {
    const pny_tok_t *t = pny_cur(p);
    if (depth == 0) {
      if (t->kind == PNTK_OP && (pny_at_op(p, ",") || pny_at_op(p, ")") ||
                                 pny_at_op(p, "=") || pny_at_op(p, ":")))
        break;
      if (t->kind == PNTK_NEWLINE || t->kind == PNTK_INDENT ||
          t->kind == PNTK_DEDENT)
        break;
    }
    if (pny_at_op(p, "[") || pny_at_op(p, "("))
      depth++;
    else if (pny_at_op(p, "]") || pny_at_op(p, ")"))
      depth--;
    if (tb.len > 0 && t->kind == PNTK_IDENT)
      pny_sb_add(&tb, " ");
    pny_sb_addn(&tb, t->s, t->n);
    pny_advance(p);
    if (depth < 0)
      break;
  }
  const char *m = "any";
  if (tb.data) {
    if (strcmp(tb.data, "int") == 0)
      m = "int";
    else if (strcmp(tb.data, "float") == 0)
      m = "f64";
    else if (strcmp(tb.data, "bool") == 0)
      m = "bool";
    else if (strcmp(tb.data, "str") == 0)
      m = "string";
    else if (strcmp(tb.data, "None") == 0 || strcmp(tb.data, "NoneType") == 0)
      m = "any";
    if (strchr(tb.data, '[') || strchr(tb.data, '|') || strchr(tb.data, '(') ||
        strchr(tb.data, '.'))
      m = "any";
  }
  snprintf(buf, sz, "%s", m);
  free(tb.data);
}

/*
 * skip helpers
 */

static void pny_skip_to_newline(pny_t *p) {
  while (!pny_at_kind(p, PNTK_NEWLINE) && !pny_at_kind(p, PNTK_EOF) &&
         !pny_at_kind(p, PNTK_INDENT) && !pny_at_kind(p, PNTK_DEDENT))
    pny_advance(p);
  if (pny_at_kind(p, PNTK_NEWLINE))
    pny_advance(p);
}

static void pny_skip_suite(pny_t *p) {
  while (!pny_at_op(p, ":") && !pny_at_kind(p, PNTK_NEWLINE) &&
         !pny_at_kind(p, PNTK_EOF))
    pny_advance(p);
  if (pny_at_op(p, ":"))
    pny_advance(p);
  if (pny_at_kind(p, PNTK_NEWLINE)) {
    pny_advance(p);
    while (pny_at_kind(p, PNTK_NEWLINE))
      pny_advance(p);
    if (pny_at_kind(p, PNTK_INDENT)) {
      pny_advance(p);
      int depth = 1;
      while (depth > 0 && !pny_at_kind(p, PNTK_EOF)) {
        if (pny_at_kind(p, PNTK_INDENT)) {
          pny_advance(p);
          depth++;
        } else if (pny_at_kind(p, PNTK_DEDENT)) {
          pny_advance(p);
          depth--;
        } else {
          pny_advance(p);
        }
      }
    }
  } else {
    pny_skip_to_newline(p);
  }
}

/*
 * statement parsers
 */

static void pny_parse_suite(pny_t *p) {
  while (pny_at_kind(p, PNTK_NEWLINE))
    pny_advance(p);
  if (pny_at_kind(p, PNTK_INDENT)) {
    pny_advance(p);
    p->indent++;
    while (!pny_at_kind(p, PNTK_DEDENT) && !pny_at_kind(p, PNTK_EOF)) {
      if (pny_at_kind(p, PNTK_NEWLINE)) {
        pny_advance(p);
        continue;
      }
      pny_parse_stmt(p);
    }
    if (pny_at_kind(p, PNTK_DEDENT))
      pny_advance(p);
    p->indent--;
  } else {
    p->indent++;
    pny_parse_simple_stmt(p);
    p->indent--;
  }
}

static void pny_parse_if(pny_t *p) {
  pny_advance(p); /* if */
  pny_emit(p, "if (");
  pny_expr(p, NULL);
  pny_emit(p, ") {");
  pny_flush(p);
  if (pny_at_op(p, ":"))
    pny_advance(p);
  pny_parse_suite(p);
  while (pny_at_word(p, "elif")) {
    pny_advance(p);
    pny_emit(p, "} elif (");
    pny_expr(p, NULL);
    pny_emit(p, ") {");
    pny_flush(p);
    if (pny_at_op(p, ":"))
      pny_advance(p);
    pny_parse_suite(p);
  }
  if (pny_at_word(p, "else")) {
    pny_advance(p);
    pny_emit(p, "} else {");
    pny_flush(p);
    if (pny_at_op(p, ":"))
      pny_advance(p);
    pny_parse_suite(p);
  }
  pny_emit(p, "}");
  pny_flush(p);
}

static void pny_parse_while(pny_t *p) {
  pny_advance(p); /* while */
  pny_emit(p, "while (");
  pny_expr(p, NULL);
  pny_emit(p, ") {");
  pny_flush(p);
  if (pny_at_op(p, ":"))
    pny_advance(p);
  pny_parse_suite(p);
  if (pny_at_word(p, "else")) {
    pny_warn_at(p, pny_cur(p)->line, "note", "while-else clause dropped");
    pny_advance(p);
    if (pny_at_op(p, ":"))
      pny_advance(p);
    pny_skip_suite(p);
  }
  pny_emit(p, "}");
  pny_flush(p);
}

/*
 * parse a comma-separated list of target names; returns count
 */
static int pny_parse_targets(pny_t *p, char names[][64], int maxn) {
  int n = 0;
  int had_paren = 0;
  if (pny_at_op(p, "(")) {
    had_paren = 1;
    pny_advance(p);
  }
  while (n < maxn) {
    const pny_tok_t *t = pny_cur(p);
    if (t->kind != PNTK_IDENT)
      break;
    snprintf(names[n], 64, "%.*s", (int)t->n, t->s);
    n++;
    pny_advance(p);
    if (pny_at_op(p, ","))
      pny_advance(p);
    else
      break;
  }
  if (had_paren && pny_at_op(p, ")"))
    pny_advance(p);
  return n;
}

static void pny_parse_for(pny_t *p) {
  int line = pny_cur(p)->line;
  pny_advance(p); /* for */
  char targets[4][64];
  int nt = pny_parse_targets(p, targets, 4);
  if (!pny_at_word(p, "in")) {
    pny_warn_at(p, line, "unsupported", "malformed for loop");
    p->errors++;
    pny_skip_suite(p);
    return;
  }
  pny_advance(p); /* in */

  if (pny_at_word(p, "range") && p->toks[p->ti + 1].kind == PNTK_OP &&
      p->toks[p->ti + 1].n == 1 && p->toks[p->ti + 1].s[0] == '(') {
    if (nt != 1) {
      pny_warn_at(p, line, "unsupported",
                  "for-range with multiple targets");
      p->errors++;
      pny_skip_suite(p);
      return;
    }
    pny_advance(p); /* range */
    pny_advance(p); /* ( */
    pny_sb_t a = {0}, b = {0}, s = {0};
    if (!pny_at_op(p, ")"))
      pny_expr(p, &a);
    if (pny_at_op(p, ",")) {
      pny_advance(p);
      if (!pny_at_op(p, ")"))
        pny_expr(p, &b);
      if (pny_at_op(p, ",")) {
        pny_advance(p);
        if (!pny_at_op(p, ")"))
          pny_expr(p, &s);
      }
    }
    if (pny_at_op(p, ")"))
      pny_advance(p);
    const char *start = a.data ? a.data : "0";
    char stopbuf[64];
    const char *stop;
    if (b.data) {
      stop = b.data;
    } else {
      /*
       * single-arg range: stop = a, start = 0
       */
      snprintf(stopbuf, sizeof(stopbuf), "%s", a.data ? a.data : "0");
      stop = stopbuf;
      start = "0";
    }
    const char *step = s.data ? s.data : "1";
    pny_emit(p, "for (mut ");
    pny_emit(p, targets[0]);
    pny_emit(p, " = ");
    pny_emit(p, start);
    pny_emit(p, " ");
    pny_emit(p, targets[0]);
    pny_emit(p, " < ");
    pny_emit(p, stop);
    pny_emit(p, " ");
    pny_emit(p, targets[0]);
    pny_emit(p, " += ");
    pny_emit(p, step);
    pny_emit(p, ") {");
    pny_flush(p);
    if (pny_at_op(p, ":"))
      pny_advance(p);
    if (!pny_decl_known(p, targets[0]))
      pny_decl_add(p, targets[0]);
    pny_parse_suite(p);
    if (pny_at_word(p, "else")) {
      pny_warn_at(p, pny_cur(p)->line, "note", "for-else clause dropped");
      pny_advance(p);
      if (pny_at_op(p, ":"))
        pny_advance(p);
      pny_skip_suite(p);
    }
    pny_emit(p, "}");
    pny_flush(p);
    free(a.data);
    free(b.data);
    free(s.data);
    return;
  }

  if (pny_at_word(p, "enumerate") && p->toks[p->ti + 1].kind == PNTK_OP &&
      p->toks[p->ti + 1].n == 1 && p->toks[p->ti + 1].s[0] == '(') {
    if (nt != 2) {
      pny_warn_at(p, line, "unsupported",
                  "for-enumerate needs two targets");
      p->errors++;
      pny_skip_suite(p);
      return;
    }
    pny_advance(p); /* enumerate */
    pny_advance(p); /* ( */
    pny_sb_t it = {0};
    pny_expr(p, &it);
    if (pny_at_op(p, ")"))
      pny_advance(p);
    /*
     * Python: for idx, val in enumerate(it); Nytrix for binds (val, idx).
     * Swap targets to preserve user's intended meaning.
     */
    pny_emit(p, "for ");
    pny_emit(p, targets[1]); /* value */
    pny_emit(p, ", ");
    pny_emit(p, targets[0]); /* index */
    pny_emit(p, " in ");
    pny_emit(p, it.data ? it.data : "");
    pny_emit(p, " {");
    pny_flush(p);
    if (!pny_decl_known(p, targets[0]))
      pny_decl_add(p, targets[0]);
    if (!pny_decl_known(p, targets[1]))
      pny_decl_add(p, targets[1]);
    free(it.data);
    if (pny_at_op(p, ":"))
      pny_advance(p);
    pny_parse_suite(p);
    if (pny_at_word(p, "else")) {
      pny_warn_at(p, pny_cur(p)->line, "note", "for-else clause dropped");
      pny_advance(p);
      if (pny_at_op(p, ":"))
        pny_advance(p);
      pny_skip_suite(p);
    }
    pny_emit(p, "}");
    pny_flush(p);
    return;
  }

  /*
   * general for-in
   */
  pny_sb_t it = {0};
  pny_expr(p, &it);
  pny_emit(p, "for ");
  for (int k = 0; k < nt; k++) {
    if (k)
      pny_emit(p, ", ");
    pny_emit(p, targets[k]);
    if (!pny_decl_known(p, targets[k]))
      pny_decl_add(p, targets[k]);
  }
  pny_emit(p, " in ");
  pny_emit(p, it.data ? it.data : "");
  pny_emit(p, " {");
  pny_flush(p);
  free(it.data);
  if (pny_at_op(p, ":"))
    pny_advance(p);
  pny_parse_suite(p);
  if (pny_at_word(p, "else")) {
    pny_warn_at(p, pny_cur(p)->line, "note", "for-else clause dropped");
    pny_advance(p);
    if (pny_at_op(p, ":"))
      pny_advance(p);
    pny_skip_suite(p);
  }
  pny_emit(p, "}");
  pny_flush(p);
}

static void pny_parse_def(pny_t *p) {
  int line = pny_cur(p)->line;
  pny_advance(p); /* def */
  const pny_tok_t *nm = pny_cur(p);
  char name[128];
  if (nm->kind == PNTK_IDENT) {
    snprintf(name, sizeof(name), "%.*s", (int)nm->n, nm->s);
    pny_advance(p);
  } else {
    snprintf(name, sizeof(name), "_anon");
  }
  pny_emit(p, "fn ");
  pny_emit(p, name);
  pny_emit(p, "(");
  if (pny_at_op(p, "(")) {
    pny_advance(p);
    int first = 1;
    /*
     * save + reset scope for this function
     */
    char **sd = p->decls;
    size_t sn = p->ndecls, sc = p->cdecls;
    p->decls = NULL;
    p->ndecls = p->cdecls = 0;
    while (!pny_at_op(p, ")") && !pny_at_kind(p, PNTK_EOF)) {
      if (pny_at_op(p, ",")) { /* separator, not a param */
        pny_advance(p);
        continue;
      }
      if (!first)
        pny_emit(p, ", ");
      first = 0;
      if (pny_at_op(p, "**")) {
        pny_advance(p);
        pny_warn_at(p, line, "unsupported", "**kwargs parameter");
        if (pny_cur(p)->kind == PNTK_IDENT)
          pny_advance(p);
        continue;
      }
      if (pny_at_op(p, "*")) {
        pny_advance(p);
        pny_emit(p, "...");
        if (pny_cur(p)->kind == PNTK_IDENT) {
          pny_emit_n(p, pny_cur(p)->s, pny_cur(p)->n);
          pny_decl_add(p, name); /* placeholder; real name below */
          /*
           * overwrite last added if it was the placeholder
           */
          (void)0;
          char pn[128];
          snprintf(pn, sizeof(pn), "%.*s", (int)pny_cur(p)->n, pny_cur(p)->s);
          pny_decl_add(p, pn);
          pny_advance(p);
        }
        continue;
      }
      if (pny_cur(p)->kind != PNTK_IDENT) {
        pny_advance(p);
        continue;
      }
      char pname[128];
      snprintf(pname, sizeof(pname), "%.*s", (int)pny_cur(p)->n,
               pny_cur(p)->s);
      pny_advance(p);
      char ty[64];
      ty[0] = 0;
      if (pny_at_op(p, ":")) {
        pny_advance(p);
        pny_parse_type(p, ty, sizeof(ty));
      }
      pny_sb_t defsb = {0};
      if (pny_at_op(p, "=")) {
        pny_advance(p);
        pny_expr(p, &defsb);
      }
      if (ty[0]) {
        pny_emit(p, ty);
        pny_emit(p, " ");
      }
      pny_emit(p, pname);
      if (defsb.data) {
        pny_emit(p, " = ");
        pny_emit(p, defsb.data);
        free(defsb.data);
      }
      pny_decl_add(p, pname);
    }
    if (pny_at_op(p, ")"))
      pny_advance(p);
    pny_emit(p, ") ");
    char ret[64];
    strcpy(ret, "any");
    if (pny_at_op(p, "->")) {
      pny_advance(p);
      pny_parse_type(p, ret, sizeof(ret));
    }
    pny_emit(p, ret);
    pny_emit(p, " {");
    pny_flush(p);
    if (pny_at_op(p, ":"))
      pny_advance(p);
    pny_parse_suite(p);
    pny_emit(p, "}");
    pny_flush(p);
    pny_decl_clear(p);
    free(p->decls);
    p->decls = sd;
    p->ndecls = sn;
    p->cdecls = sc;
  } else {
    pny_emit(p, ") any {");
    pny_flush(p);
    if (pny_at_op(p, ":"))
      pny_advance(p);
    pny_parse_suite(p);
    pny_emit(p, "}");
    pny_flush(p);
  }
}

static void pny_parse_return(pny_t *p) {
  pny_advance(p); /* return */
  pny_emit(p, "return");
  if (!pny_at_kind(p, PNTK_NEWLINE) && !pny_at_kind(p, PNTK_EOF) &&
      !pny_at_kind(p, PNTK_DEDENT) && !pny_at_op(p, ";")) {
    pny_emit(p, " ");
    pny_sb_t first = {0};
    pny_expr(p, &first);
    if (pny_at_op(p, ",")) {
      /*
       * multiple return values -> list literal
       */
      pny_sb_t val = {0};
      pny_sb_add(&val, "[");
      pny_sb_add(&val, first.data ? first.data : "");
      while (pny_at_op(p, ",")) {
        pny_advance(p);
        if (pny_at_kind(p, PNTK_NEWLINE) || pny_at_kind(p, PNTK_DEDENT) ||
            pny_at_kind(p, PNTK_EOF) || pny_at_op(p, ";"))
          break;
        pny_sb_t e = {0};
        pny_expr(p, &e);
        pny_sb_add(&val, ", ");
        pny_sb_add(&val, e.data ? e.data : "");
        free(e.data);
      }
      pny_sb_add(&val, "]");
      pny_emit_sb(p, &val);
      free(val.data);
    } else {
      pny_emit_sb(p, &first);
    }
    free(first.data);
  }
  while (pny_at_op(p, ";"))
    pny_advance(p);
  if (pny_at_kind(p, PNTK_NEWLINE))
    pny_advance(p);
  pny_flush(p);
}

/*
 * augmented assignment? fills opbuf with the Ny form (with spaces)
 */
static int pny_take_aug(pny_t *p, char *opbuf, size_t opsz) {
  const pny_tok_t *t = pny_cur(p);
  if (t->kind != PNTK_OP)
    return 0;
  static const char *const aug[] = {"+=", "-=", "*=", "/=", "%=", "&=",
                                    "|=", "^=", NULL};
  for (int k = 0; aug[k]; k++) {
    if (t->n == strlen(aug[k]) && memcmp(t->s, aug[k], t->n) == 0) {
      snprintf(opbuf, opsz, " %s ", aug[k]);
      pny_advance(p);
      return 1;
    }
  }
  if (t->n == 3 && memcmp(t->s, "//=", 3) == 0) {
    snprintf(opbuf, opsz, " /= ");
    pny_advance(p);
    return 1;
  }
  if (t->n == 3 && memcmp(t->s, "**=", 3) == 0) {
    pny_warn_at(p, t->line, "unsupported", "**= augmented assignment");
    pny_advance(p);
    return 1;
  }
  if ((t->n == 3 && (memcmp(t->s, ">>=", 3) == 0 || memcmp(t->s, "<<=", 3) == 0)) ||
      (t->n == 2 && memcmp(t->s, "@=", 2) == 0)) {
    pny_warn_at(p, t->line, "unsupported", "'%.*s' augmented assignment",
                (int)t->n, t->s);
    pny_advance(p);
    return 1;
  }
  return 0;
}

static int pny_is_bare_ident(const pny_sb_t *b) {
  if (!b->data || b->len == 0)
    return 0;
  if (!PN_IS_ALPHA((unsigned char)b->data[0]))
    return 0;
  for (size_t i = 1; i < b->len; i++)
    if (!PN_IS_ALNUM((unsigned char)b->data[i]))
      return 0;
  return 1;
}

static void pny_parse_simple_stmt(pny_t *p) {
  pny_sb_t first = {0};
  pny_expr(p, &first);

  if (pny_at_op(p, "=")) {
    /*
     * plain assignment
     */
    pny_advance(p);
    pny_sb_t val = {0};
    pny_expr(p, &val);
    if (pny_is_bare_ident(&first)) {
      if (!pny_decl_known(p, first.data)) {
        pny_emit(p, "mut ");
        pny_decl_add(p, first.data);
      }
    }
    pny_emit_sb(p, &first);
    pny_emit(p, " = ");
    pny_emit_sb(p, &val);
    free(val.data);
  } else {
    char opbuf[8];
    if (pny_take_aug(p, opbuf, sizeof(opbuf))) {
      pny_sb_t val = {0};
      pny_expr(p, &val);
      pny_emit_sb(p, &first);
      pny_emit(p, opbuf);
      pny_emit_sb(p, &val);
      free(val.data);
    } else if (pny_at_op(p, ",")) {
      /*
       * tuple unpacking: target_list = expr_list
       * Collect LHS target names, then parse the RHS as a full tuple
       * expression so `a, b = b, a` captures both RHS values.
       */
      pny_sb_t tup = {0};
      pny_sb_add(&tup, first.data ? first.data : "");
      int ntargets = 1;
      int all_new = pny_is_bare_ident(&first);
      while (pny_at_op(p, ",")) {
        pny_advance(p);
        if (pny_at_op(p, "="))
          break;
        pny_sb_t e = {0};
        pny_expr(p, &e);
        pny_sb_add(&tup, ", ");
        pny_sb_add(&tup, e.data ? e.data : "");
        ntargets++;
        if (!pny_is_bare_ident(&e) || pny_decl_known(p, e.data))
          all_new = 0;
        free(e.data);
      }
      if (pny_at_op(p, "=") && ntargets > 1) {
        pny_advance(p);
        /*
         * RHS: parse the full comma-separated expression list
         */
        pny_sb_t val = {0};
        pny_expr(p, &val);
        while (pny_at_op(p, ",")) {
          pny_advance(p);
          pny_sb_t e = {0};
          pny_expr(p, &e);
          pny_sb_add(&val, ", ");
          pny_sb_add(&val, e.data ? e.data : "");
          free(e.data);
        }
        /*
         * Ny: `mut a, b = ...` declares; bare `a, b = ...` reassigns.
         * Emit `mut` only when every target is a fresh bare identifier.
         */
        if (all_new) {
          pny_emit(p, "mut ");
          /*
           * register each target name
           */
          for (char *s = tup.data ? tup.data : ""; *s;) {
            char name[128];
            size_t k = 0;
            while (*s == ' ')
              s++;
            while (*s && *s != ',' && k + 1 < sizeof(name))
              name[k++] = *s++;
            name[k] = 0;
            while (*s == ' ')
              s++;
            if (*s == ',')
              s++;
            if (k)
              pny_decl_add(p, name);
          }
        }
        pny_emit(p, tup.data ? tup.data : "");
        pny_emit(p, " = ");
        pny_emit(p, val.data ? val.data : "");
        free(val.data);
      } else {
        pny_emit(p, tup.data ? tup.data : "");
      }
      free(tup.data);
    } else {
      /*
       * expression statement
       */
      pny_emit_sb(p, &first);
    }
  }
  free(first.data);
  while (pny_at_op(p, ";"))
    pny_advance(p);
  if (pny_at_kind(p, PNTK_NEWLINE))
    pny_advance(p);
  pny_flush(p);
}

static void pny_parse_stmt(pny_t *p) {
  while (pny_at_kind(p, PNTK_NEWLINE))
    pny_advance(p);
  const pny_tok_t *t = pny_cur(p);
  if (t->kind == PNTK_EOF || t->kind == PNTK_DEDENT)
    return;
  if (t->kind == PNTK_IDENT) {
    if (pny_word_eq(t, "if")) {
      pny_parse_if(p);
      return;
    }
    if (pny_word_eq(t, "while")) {
      pny_parse_while(p);
      return;
    }
    if (pny_word_eq(t, "for")) {
      pny_parse_for(p);
      return;
    }
    if (pny_word_eq(t, "def")) {
      pny_parse_def(p);
      return;
    }
    if (pny_word_eq(t, "return")) {
      pny_parse_return(p);
      return;
    }
    if (pny_word_eq(t, "break")) {
      pny_advance(p);
      pny_emit(p, "break");
      while (pny_at_op(p, ";"))
        pny_advance(p);
      if (pny_at_kind(p, PNTK_NEWLINE))
        pny_advance(p);
      pny_flush(p);
      return;
    }
    if (pny_word_eq(t, "continue")) {
      pny_advance(p);
      pny_emit(p, "continue");
      while (pny_at_op(p, ";"))
        pny_advance(p);
      if (pny_at_kind(p, PNTK_NEWLINE))
        pny_advance(p);
      pny_flush(p);
      return;
    }
    if (pny_word_eq(t, "pass")) {
      pny_advance(p);
      pny_emit(p, "; pass");
      while (pny_at_op(p, ";"))
        pny_advance(p);
      if (pny_at_kind(p, PNTK_NEWLINE))
        pny_advance(p);
      pny_flush(p);
      return;
    }
    if (pny_word_eq(t, "class")) {
      pny_warn_at(p, t->line, "unsupported", "class definition");
      pny_skip_suite(p);
      return;
    }
    if (pny_word_eq(t, "try") || pny_word_eq(t, "with") ||
        pny_word_eq(t, "raise")) {
      pny_warn_at(p, t->line, "unsupported", "'%.*s' statement", (int)t->n,
                  t->s);
      pny_skip_suite(p);
      return;
    }
    if (pny_word_eq(t, "import") || pny_word_eq(t, "from")) {
      pny_warn_at(p, t->line, "note", "import dropped");
      pny_skip_to_newline(p);
      return;
    }
    if (pny_word_eq(t, "global") || pny_word_eq(t, "nonlocal")) {
      pny_warn_at(p, t->line, "note", "'%.*s' dropped", (int)t->n, t->s);
      pny_skip_to_newline(p);
      return;
    }
    if (pny_word_eq(t, "assert")) {
      pny_warn_at(p, t->line, "note", "assert dropped");
      pny_skip_to_newline(p);
      return;
    }
    if (pny_word_eq(t, "del")) {
      pny_advance(p);
      pny_emit(p, "del ");
      if (!pny_at_kind(p, PNTK_NEWLINE))
        pny_expr(p, NULL);
      while (pny_at_op(p, ";"))
        pny_advance(p);
      if (pny_at_kind(p, PNTK_NEWLINE))
        pny_advance(p);
      pny_flush(p);
      return;
    }
    if (pny_word_eq(t, "elif") || pny_word_eq(t, "else") ||
        pny_word_eq(t, "except") || pny_word_eq(t, "finally")) {
      pny_skip_to_newline(p);
      return;
    }
  }
  pny_parse_simple_stmt(p);
}

/*
 * top level + entry
 */

static void pny_parse_top(pny_t *p) {
  while (!pny_at_kind(p, PNTK_EOF)) {
    if (pny_at_kind(p, PNTK_NEWLINE) || pny_at_kind(p, PNTK_INDENT) ||
        pny_at_kind(p, PNTK_DEDENT)) {
      pny_advance(p);
      continue;
    }
    const pny_tok_t *t = pny_cur(p);
    if (t->kind == PNTK_IDENT) {
      if (pny_word_eq(t, "def")) {
        pny_parse_def(p);
        continue;
      }
      if (pny_word_eq(t, "class")) {
        pny_warn_at(p, t->line, "unsupported", "class definition");
        pny_skip_suite(p);
        continue;
      }
      if (pny_word_eq(t, "import") || pny_word_eq(t, "from")) {
        pny_warn_at(p, t->line, "note", "import dropped");
        pny_skip_to_newline(p);
        continue;
      }
      if (pny_word_eq(t, "global") || pny_word_eq(t, "nonlocal")) {
        pny_warn_at(p, t->line, "note", "'%.*s' dropped", (int)t->n, t->s);
        pny_skip_to_newline(p);
        continue;
      }
    }
    /*
     * executable statement -> collect into #main
     */
    pny_sb_t *saved = p->out;
    int si = p->indent;
    p->out = &p->main_buf;
    p->indent = 1;
    pny_parse_stmt(p);
    p->out = saved;
    p->indent = si;
  }
}

static int pny_convert(const char *src, size_t n, pny_sb_t *out) {
  pny_toks_t toks = {0};
  pny_lex(src, n, &toks);
  pny_t p;
  memset(&p, 0, sizeof(p));
  p.toks = toks.items;
  p.nt = toks.len;
  p.out = out;
  p.indent = 0;
  pny_parse_top(&p);
  if (p.main_buf.len > 0) {
    pny_sb_add(out, "#main {\n");
    if (p.main_buf.data)
      pny_sb_add(out, p.main_buf.data);
    pny_sb_add(out, "}\n");
  }
  free(p.main_buf.data);
  free(p.ln.data);
  free(p.warn.data);
  pny_decl_clear(&p);
  free(p.decls);
  pny_toks_free(&toks);
  return p.errors == 0 ? 0 : 1;
}

/*
 * selftest
 */

static int py2ny_selftest(void) {
  static const char probe[] =
      "def classify(x):\n"
      "    if x < 0:\n"
      "        return -1\n"
      "    elif x == 0:\n"
      "        return 0\n"
      "    else:\n"
      "        return 1\n"
      "\n"
      "def factorial(n):\n"
      "    r = 1\n"
      "    for i in range(1, n + 1):\n"
      "        r = r * i\n"
      "    return r\n"
      "\n"
      "def first_even(xs):\n"
      "    for x in xs:\n"
      "        if x % 2 == 0:\n"
      "            return x\n"
      "    return -1\n"
      "\n"
      "def has_item(xs, x):\n"
      "    return x in xs\n"
      "\n"
      "def lacks_item(xs, x):\n"
      "    return x not in xs\n"
      "\n"
      "def swap(a, b):\n"
      "    a, b = b, a\n"
      "    return a, b\n"
      "\n"
      "print(classify(5))\n"
      "print(factorial(5))\n"
      "print(first_even([1, 3, 4, 5]))\n"
      "print(has_item([1, 2, 3], 2))\n"
      "print(lacks_item([1, 2, 3], 9))\n";
  pny_sb_t out = {0};
  if (pny_convert(probe, sizeof(probe) - 1, &out) != 0) {
    fprintf(stderr, "ny-fmt py2ny selftest: pny_convert reported unsupported "
                    "constructs\n");
    free(out.data);
    return 1;
  }
  const char *o = out.data ? out.data : "";
  static const char *const need[] = {
      "fn classify(x) any {\n",
      "if (x < 0) {\n",
      "} elif (x == 0) {\n",
      "} else {\n",
      "return -1\n",
      "fn factorial(n) any {\n",
      "mut r = 1\n",
      "for (mut i = 1 i < n + 1 i += 1) {\n",
      "r = r * i\n",
      "for x in xs {\n",
      "if (x % 2 == 0) {\n",
      "return contains(xs, x)\n",
      "return !contains(xs, x)\n",
      "a, b = b, a\n",
      "return [a, b]\n",
      "#main {\n",
      "print(classify(5))\n",
      "print(first_even([1, 3, 4, 5]))\n",
  };
  int fail = 0;
  for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
    if (!strstr(o, need[i])) {
      fprintf(stderr, "ny-fmt py2ny selftest: missing %s", need[i]);
      fail = 1;
    }
  }
  /*
   * leak / merge regression checks
   */
  if (strstr(o, "}elif") || strstr(o, "}else") || strstr(o, "r = r * ireturn") ||
      strstr(o, "fn classify(x)any") || strstr(o, "1 i<") ||
      strstr(o, "mut r = 1for") || strstr(o, "__py2ny_")) {
    fprintf(stderr, "ny-fmt py2ny selftest: emission leak detected\n");
    fail = 1;
  }
  free(out.data);
  if (fail)
    return 1;
  printf("ny-fmt selftest: py2ny round-trip checks: ok\n");
  return 0;
}
