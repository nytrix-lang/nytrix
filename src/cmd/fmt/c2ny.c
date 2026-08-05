/*
 * c2ny.c - real C to Nytrix converter for ny-fmt.
 *
 * A tokenizer plus recursive-descent parser that translates a practical C
 * subset into Nytrix. Unsupported constructs become explicit
 * `;; c2ny: unsupported:` marker comments (with line numbers) so the emitted
 * file stays readable and never drops a construct silently.
 *
 * Included by src/cmd/fmt/init.c; all symbols are file-local.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* string builder                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} cny_sb_t;

static int cny_sb_grow(cny_sb_t *b, size_t need) {
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

static void cny_sb_add(cny_sb_t *b, const char *s) {
  size_t n = strlen(s);
  if (cny_sb_grow(b, b->len + n + 1)) {
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
  }
}

static void cny_sb_addn(cny_sb_t *b, const char *s, size_t n) {
  if (cny_sb_grow(b, b->len + n + 1)) {
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
  }
}

static void cny_sb_clear(cny_sb_t *b) {
  if (b->data)
    b->data[0] = 0;
  b->len = 0;
}

/* ------------------------------------------------------------------ */
/* tokenizer                                                           */
/* ------------------------------------------------------------------ */

enum {
  CNTK_EOF = 0,
  CNTK_IDENT,
  CNTK_INT,
  CNTK_FLT,
  CNTK_STR,
  CNTK_CHAR,
  CNTK_PUNCT,
  CNTK_DIRECTIVE
};

typedef struct {
  int kind;
  const char *s; /* start in source */
  size_t n;      /* length */
  int line;
} cny_tok_t;

typedef struct {
  cny_tok_t *items;
  size_t len;
  size_t cap;
} cny_toks_t;

static const char *const cny_pp_ops[] = {
    "...", ">>=", "<<=", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
    "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", NULL};

static int cny_is_ppop(const char *s, size_t n) {
  for (int i = 0; cny_pp_ops[i]; i++) {
    size_t l = strlen(cny_pp_ops[i]);
    if (n == l && memcmp(s, cny_pp_ops[i], l) == 0)
      return 1;
  }
  return 0;
}

/* lex a C string/char literal escape; returns value or -1, advances *q */
static long long cny_esc(const char **q) {
  const char *p = *q;
  char c = *p++;
  switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return '\v';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case '?': return '?';
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': {
      long long v = c - '0';
      int k = 0;
      while (k < 2 && *p >= '0' && *p <= '7') {
        v = v * 8 + (*p - '0');
        p++;
        k++;
      }
      *q = p;
      return v;
    }
    case 'x': {
      long long v = 0;
      int any = 0;
      while (isxdigit((unsigned char)*p)) {
        char d = *p++;
        v = v * 16 + (d <= '9' ? d - '0' : (tolower((unsigned char)d) - 'a' + 10));
        any = 1;
      }
      if (!any)
        return -1;
      *q = p;
      return v;
    }
    default:
      return (unsigned char)c;
  }
}

/* Evaluate a C character literal body (without quotes). */
static long long cny_char_value(const char *s, size_t n) {
  if (n == 0)
    return -1;
  if (s[0] == '\\') {
    const char *q = s + 1;
    long long v = cny_esc(&q);
    return v;
  }
  if (n >= 2 && s[0] == '\\') /* multi-char handled above */
    return -1;
  if (n == 1)
    return (unsigned char)s[0];
  /* multi-char literal: keep the last byte's value, like C */
  return (unsigned char)s[n - 1];
}

static void cny_toks_push(cny_toks_t *t, int kind, const char *s, size_t n,
                          int line) {
  if (t->len == t->cap) {
    size_t cap = t->cap ? t->cap * 2 : 256;
    cny_tok_t *p = (cny_tok_t *)realloc(t->items, cap * sizeof(*t->items));
    if (!p)
      return;
    t->items = p;
    t->cap = cap;
  }
  cny_tok_t *tk = &t->items[t->len++];
  tk->kind = kind;
  tk->s = s;
  tk->n = n;
  tk->line = line;
}

static void cny_toks_free(cny_toks_t *t) {
  free(t->items);
  memset(t, 0, sizeof(*t));
}

/*
 * Tokenize C source into a token array. Comments, strings, character
 * literals, and line continuations are consumed; preprocessor directive
 * lines become a single CNTK_DIRECTIVE token holding the text after '#'.
 */
static void cny_lex(const char *src, size_t n, cny_toks_t *out) {
  size_t i = 0;
  int line = 1;
  int bol = 1; /* beginning of (non-directive) line */
  while (i < n) {
    char c = src[i];
    if (c == '\n') {
      line++;
      bol = 1;
      i++;
      continue;
    }
    if (isspace((unsigned char)c)) {
      i++;
      continue;
    }
    if (c == '/' && i + 1 < n && src[i + 1] == '/') {
      while (i < n && src[i] != '\n')
        i++;
      continue;
    }
    if (c == '/' && i + 1 < n && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
        if (src[i] == '\n')
          line++;
        i++;
      }
      if (i + 1 < n)
        i += 2;
      continue;
    }
    if (bol && c == '#') {
      /* directive: consume through end of line, honoring continuations */
      size_t start = i;
      i++;
      while (i < n && src[i] != '\n') {
        if (src[i] == '\\' && i + 1 < n && src[i + 1] == '\n') {
          i += 2;
          line++;
        } else {
          i++;
        }
      }
      cny_toks_push(out, CNTK_DIRECTIVE, src + start, i - start, line);
      bol = 0;
      continue;
    }
    bol = 0;
    if (c == '"' || c == '\'') {
      char quote = c;
      size_t start = i;
      i++;
      while (i < n) {
        if (src[i] == '\\' && i + 1 < n) {
          i += 2;
          continue;
        }
        if (src[i] == quote) {
          i++;
          break;
        }
        if (src[i] == '\n') {
          line++;
          break;
        }
        i++;
      }
      cny_toks_push(out, quote == '"' ? CNTK_STR : CNTK_CHAR, src + start,
                    i - start, line);
      continue;
    }
    if (isalpha((unsigned char)c) || c == '_') {
      size_t start = i;
      while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '_'))
        i++;
      cny_toks_push(out, CNTK_IDENT, src + start, i - start, line);
      continue;
    }
    if (isdigit((unsigned char)c)) {
      size_t start = i;
      while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '.'))
        i++;
      int kind = CNTK_INT;
      for (size_t k = start; k < i; k++) {
        if (src[k] == '.' || src[k] == 'e' || src[k] == 'E' ||
            src[k] == 'p' || src[k] == 'P')
          kind = CNTK_FLT;
      }
      cny_toks_push(out, kind, src + start, i - start, line);
      continue;
    }
    /* punctuator: longest match first */
    {
      size_t best = 0;
      for (size_t l = 3; l >= 1; l--) {
        if (i + l <= n && l == 1) {
          best = 1;
          break;
        }
        if (i + l <= n && cny_is_ppop(src + i, l)) {
          best = l;
          break;
        }
      }
      /* single-char puncts not in the pp table */
      if (best == 0)
        best = 1;
      cny_toks_push(out, CNTK_PUNCT, src + i, best, line);
      i += best;
    }
  }
  cny_toks_push(out, CNTK_EOF, src + n, 0, line);
}

/* ------------------------------------------------------------------ */
/* type mapping                                                        */
/* ------------------------------------------------------------------ */

static int cny_word_eq(const char *s, size_t n, const char *w) {
  size_t l = strlen(w);
  return l == n && memcmp(s, w, l) == 0;
}

static int cny_is_type_kw(const char *s, size_t n) {
  static const char *const kws[] = {"void",  "char", "short", "int",
                                    "long",  "float", "double", "signed",
                                    "unsigned", "bool", "_Bool", "struct",
                                    "enum",  "union", "const", "volatile",
                                    "register", "static", "extern", "inline",
                                    "restrict", "_Atomic", NULL};
  for (int i = 0; kws[i]; i++)
    if (cny_word_eq(s, n, kws[i]))
      return 1;
  return 0;
}

static int cny_is_known_type(const char *s, size_t n) {
  static const char *const types[] = {
      "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
      "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t",
      "int64_t", "uint64_t", "wchar_t", NULL};
  for (int i = 0; types[i]; i++)
    if (cny_word_eq(s, n, types[i]))
      return 1;
  return 0;
}

/* pragmatic scalar mapping for values */
static const char *cny_scalar_type(const char *s, size_t n) {
  if (cny_word_eq(s, n, "void"))
    return "any";
  if (cny_word_eq(s, n, "char") || cny_word_eq(s, n, "short") ||
      cny_word_eq(s, n, "int") || cny_word_eq(s, n, "long") ||
      cny_word_eq(s, n, "signed") || cny_word_eq(s, n, "unsigned") ||
      cny_is_known_type(s, n))
    return "int";
  if (cny_word_eq(s, n, "float") || cny_word_eq(s, n, "double"))
    return "f64";
  if (cny_word_eq(s, n, "bool") || cny_word_eq(s, n, "_Bool"))
    return "bool";
  return "any";
}

/* faithful C-width mapping for layout fields */
static const char *cny_layout_type(const char *s, size_t n) {
  if (cny_word_eq(s, n, "char") || cny_word_eq(s, n, "signed char"))
    return "i8";
  if (cny_word_eq(s, n, "unsigned char"))
    return "u8";
  if (cny_word_eq(s, n, "short"))
    return "i16";
  if (cny_word_eq(s, n, "unsigned short"))
    return "u16";
  if (cny_word_eq(s, n, "int") || cny_word_eq(s, n, "signed"))
    return "i32";
  if (cny_word_eq(s, n, "unsigned") || cny_word_eq(s, n, "unsigned int"))
    return "u32";
  if (cny_word_eq(s, n, "long") || cny_word_eq(s, n, "long long"))
    return "i64";
  if (cny_word_eq(s, n, "unsigned long") ||
      cny_word_eq(s, n, "unsigned long long"))
    return "u64";
  if (cny_word_eq(s, n, "float"))
    return "f32";
  if (cny_word_eq(s, n, "double"))
    return "f64";
  if (cny_word_eq(s, n, "bool") || cny_word_eq(s, n, "_Bool"))
    return "u8";
  if (cny_word_eq(s, n, "size_t"))
    return "u64";
  if (cny_word_eq(s, n, "int8_t")) return "i8";
  if (cny_word_eq(s, n, "uint8_t")) return "u8";
  if (cny_word_eq(s, n, "int16_t")) return "i16";
  if (cny_word_eq(s, n, "uint16_t")) return "u16";
  if (cny_word_eq(s, n, "int32_t")) return "i32";
  if (cny_word_eq(s, n, "uint32_t")) return "u32";
  if (cny_word_eq(s, n, "int64_t")) return "i64";
  if (cny_word_eq(s, n, "uint64_t")) return "u64";
  return NULL;
}

/* ------------------------------------------------------------------ */
/* parser context                                                      */
/* ------------------------------------------------------------------ */

enum {
  CNY_NOTE_PRINTF = 1,
  CNY_NOTE_MAIN_RET = 2,
  CNY_NOTE_MAIN_ARGS = 4,
  CNY_NOTE_STRING = 8,
  CNY_NOTE_SIZEOF = 16
};

typedef struct {
  const cny_tok_t *toks;
  size_t nt;
  size_t ti;
  cny_sb_t *out;   /* final output */
  cny_sb_t ln;     /* current line buffer */
  cny_sb_t warn;   /* pending warning comment text (without ';;') */
  int indent;
  int errors;
  int in_main;
  int in_func;
  int notes;

  char **mac_name;
  char **mac_val;
  size_t nmac;
  size_t cmac;

  char **td_name;
  char **td_typ;
  size_t ntd;
  size_t ctd;

  /* #if stack */
  char *if_taken;      /* current branch taken? */
  char *if_ever;       /* any branch taken yet? */
  char *if_parent;     /* was parent active when #if pushed? */
  size_t nif;
  size_t cif;
} cny_t;

static const cny_tok_t *cny_cur(const cny_t *p) { return &p->toks[p->ti]; }
static const cny_tok_t *cny_peek(const cny_t *p, size_t k) {
  size_t i = p->ti + k;
  return i < p->nt ? &p->toks[i] : &p->toks[p->nt - 1];
}

static int cny_at(const cny_t *p, const char *text) {
  const cny_tok_t *t = cny_cur(p);
  return t->kind == CNTK_PUNCT && t->n == 1 && *t->s == text[0] &&
         text[1] == 0;
}

static int cny_at_word(const cny_t *p, const char *w) {
  const cny_tok_t *t = cny_cur(p);
  return t->kind == CNTK_IDENT && cny_word_eq(t->s, t->n, w);
}

static void cny_advance(cny_t *p) {
  if (p->ti + 1 < p->nt)
    p->ti++;
}

static int cny_active(const cny_t *p) {
  for (size_t i = 0; i < p->nif; i++)
    if (!p->if_taken[i])
      return 0;
  return 1;
}

/* emit helpers */

static void cny_emit(cny_t *p, const char *s) { cny_sb_add(&p->ln, s); }

static void cny_emit_node(cny_t *p, const cny_sb_t *b) {
  cny_sb_add(&p->ln, b->data ? b->data : "");
}

static void cny_warn_at(cny_t *p, int line, const char *kind,
                        const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  cny_sb_add(&p->warn, " c2ny: ");
  cny_sb_add(&p->warn, kind);
  cny_sb_add(&p->warn, ": ");
  cny_sb_add(&p->warn, buf);
  char linebuf[32];
  snprintf(linebuf, sizeof(linebuf), " (line %d)", line);
  cny_sb_add(&p->warn, linebuf);
}

/* flush current line and pending warnings to output with indent */
static void cny_flush(cny_t *p) {
  if (p->ln.len > 0 || p->warn.len > 0) {
    if (p->warn.len > 0) {
      for (int i = 0; i < p->indent; i++)
        cny_sb_add(p->out, "   ");
      cny_sb_add(p->out, ";;");
      cny_sb_add(p->out, p->warn.data);
      cny_sb_add(p->out, "\n");
    }
    for (int i = 0; i < p->indent; i++)
      cny_sb_add(p->out, "   ");
    cny_sb_add(p->out, p->ln.data ? p->ln.data : "");
    cny_sb_add(p->out, "\n");
  }
  cny_sb_clear(&p->ln);
  cny_sb_clear(&p->warn);
}

/* macro table */

static void cny_mac_set(cny_t *p, const char *name, const char *val) {
  for (size_t i = 0; i < p->nmac; i++) {
    if (strcmp(p->mac_name[i], name) == 0) {
      free(p->mac_val[i]);
      p->mac_val[i] = strdup(val ? val : "");
      return;
    }
  }
  if (p->nmac == p->cmac) {
    size_t cap = p->cmac ? p->cmac * 2 : 16;
    char **n = (char **)realloc(p->mac_name, cap * sizeof(char *));
    if (!n)
      return;
    p->mac_name = n;
    char **v = (char **)realloc(p->mac_val, cap * sizeof(char *));
    if (!v)
      return;
    p->mac_val = v;
    p->cmac = cap;
  }
  p->mac_name[p->nmac] = strdup(name);
  p->mac_val[p->nmac] = strdup(val ? val : "");
  p->nmac++;
}

static const char *cny_mac_get(const cny_t *p, const char *name) {
  for (size_t i = 0; i < p->nmac; i++)
    if (strcmp(p->mac_name[i], name) == 0)
      return p->mac_val[i];
  return NULL;
}

/* typedef table */

static void cny_td_set(cny_t *p, const char *name, const char *typ) {
  for (size_t i = 0; i < p->ntd; i++) {
    if (strcmp(p->td_name[i], name) == 0) {
      free(p->td_typ[i]);
      p->td_typ[i] = strdup(typ);
      return;
    }
  }
  if (p->ntd == p->ctd) {
    size_t cap = p->ctd ? p->ctd * 2 : 16;
    char **n = (char **)realloc(p->td_name, cap * sizeof(char *));
    if (!n)
      return;
    p->td_name = n;
    char **v = (char **)realloc(p->td_typ, cap * sizeof(char *));
    if (!v)
      return;
    p->td_typ = v;
    p->ctd = cap;
  }
  p->td_name[p->ntd] = strdup(name);
  p->td_typ[p->ntd] = strdup(typ);
  p->ntd++;
}

/* length-aware lookup: token text is not NUL-terminated */
static const char *cny_td_get_n(const cny_t *p, const char *name, size_t n) {
  for (size_t i = 0; i < p->ntd; i++)
    if (cny_word_eq(name, n, p->td_name[i]))
      return p->td_typ[i];
  return NULL;
}

/* number literal emission */

/*
 * Emit a single token into `b` using the C-to-Nytrix literal mapping.
 * Used for init/cond/update clause reconstruction.
 */
static void cny_emit_tok_value(cny_sb_t *b, const cny_tok_t *t) {
  if (t->kind == CNTK_INT) {
    size_t n = t->n;
    const char *s = t->s;
    while (n > 0 && (s[n - 1] == 'u' || s[n - 1] == 'U' || s[n - 1] == 'l' ||
                     s[n - 1] == 'L'))
      n--;
    if (n >= 2 && s[0] == '0' && s[1] != 'x' && s[1] != 'X' && s[1] != 'b' &&
        s[1] != 'B') {
      /* octal literal -> decimal */
      long long v = 0;
      int ok = 1;
      for (size_t k = 1; k < n; k++) {
        if (s[k] < '0' || s[k] > '7') {
          ok = 0;
          break;
        }
        v = v * 8 + (s[k] - '0');
      }
      if (ok) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", v);
        cny_sb_add(b, buf);
        return;
      }
    }
    cny_sb_addn(b, s, n);
    return;
  }
  if (t->kind == CNTK_FLT) {
    size_t n = t->n;
    const char *s = t->s;
    while (n > 0 && (s[n - 1] == 'f' || s[n - 1] == 'F' || s[n - 1] == 'l' ||
                     s[n - 1] == 'L'))
      n--;
    cny_sb_addn(b, s, n);
    return;
  }
  if (t->kind == CNTK_CHAR) {
    long long v = cny_char_value(t->s + 1, t->n - 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", v);
    cny_sb_add(b, buf);
    return;
  }
  if (t->kind == CNTK_IDENT && cny_word_eq(t->s, t->n, "NULL")) {
    cny_sb_add(b, "nil");
    return;
  }
  if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == '^') {
    cny_sb_add(b, "^^");
    return;
  }
  cny_sb_addn(b, t->s, t->n);
}

/* forward declarations */
static void cny_parse_statement(cny_t *p);
static long long cny_const_eval_range(cny_t *p, size_t from, size_t to,
                                      int *ok);

/* ------------------------------------------------------------------ */
/* constant expression evaluator (used for #if, case labels, enums)    */
/* ------------------------------------------------------------------ */

typedef struct {
  const cny_tok_t *t;
  size_t n;
  size_t i;
  const cny_t *p;
  int ok;
} cny_cexpr_t;

static int cny_ce_peek_is_punct(const cny_cexpr_t *e, const char *op) {
  const cny_tok_t *t = &e->t[e->i];
  return t->kind == CNTK_PUNCT && cny_word_eq(t->s, t->n, op);
}

static long long cny_ce_num(cny_cexpr_t *e, long long v) {
  e->i++;
  return v;
}

static long long cny_ce_primary(cny_cexpr_t *e);
static long long cny_ce_unary(cny_cexpr_t *e);
static long long cny_ce_add(cny_cexpr_t *e);
static long long cny_ce_shift(cny_cexpr_t *e);
static long long cny_ce_rel(cny_cexpr_t *e);
static long long cny_ce_eq(cny_cexpr_t *e);
static long long cny_ce_band(cny_cexpr_t *e);
static long long cny_ce_xor(cny_cexpr_t *e);
static long long cny_ce_bor(cny_cexpr_t *e);
static long long cny_ce_land(cny_cexpr_t *e);
static long long cny_ce_lor(cny_cexpr_t *e);
static long long cny_ce_tern(cny_cexpr_t *e);

static long long cny_ce_number_atom(const cny_tok_t *t, int *ok) {
  if (t->kind == CNTK_INT) {
    const char *s = t->s;
    size_t n = t->n;
    while (n > 0 && (s[n - 1] == 'u' || s[n - 1] == 'U' || s[n - 1] == 'l' ||
                     s[n - 1] == 'L'))
      n--;
    long long v = 0;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      for (size_t k = 2; k < n; k++) {
        char d = s[k];
        if (d >= '0' && d <= '9')
          v = v * 16 + (d - '0');
        else if (d >= 'a' && d <= 'f')
          v = v * 16 + (d - 'a' + 10);
        else if (d >= 'A' && d <= 'F')
          v = v * 16 + (d - 'A' + 10);
        else {
          *ok = 0;
          return 0;
        }
      }
      return v;
    }
    if (n >= 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
      for (size_t k = 2; k < n; k++) {
        if (s[k] != '0' && s[k] != '1') {
          *ok = 0;
          return 0;
        }
        v = v * 2 + (s[k] - '0');
      }
      return v;
    }
    if (n >= 2 && s[0] == '0') {
      for (size_t k = 1; k < n; k++) {
        if (s[k] < '0' || s[k] > '7') {
          *ok = 0;
          return 0;
        }
        v = v * 8 + (s[k] - '0');
      }
      return v;
    }
    for (size_t k = 0; k < n; k++) {
      if (s[k] < '0' || s[k] > '9') {
        *ok = 0;
        return 0;
      }
      v = v * 10 + (s[k] - '0');
    }
    return v;
  }
  if (t->kind == CNTK_CHAR) {
    return cny_char_value(t->s + 1, t->n - 2);
  }
  *ok = 0;
  return 0;
}

static long long cny_ce_primary(cny_cexpr_t *e) {
  const cny_tok_t *t = &e->t[e->i];
  if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == '(') {
    e->i++;
    long long v = cny_ce_tern(e);
    if (e->i >= e->n || !cny_ce_peek_is_punct(e, ")"))
      e->ok = 0;
    else
      e->i++;
    return v;
  }
  if (t->kind == CNTK_IDENT) {
    if (cny_word_eq(t->s, t->n, "defined")) {
      e->i++;
      int defined = 0;
      if (e->i < e->n && cny_ce_peek_is_punct(e, "(")) {
        e->i++;
        if (e->i < e->n && e->t[e->i].kind == CNTK_IDENT) {
          char name[256];
          size_t nl = e->t[e->i].n;
          if (nl >= sizeof(name))
            nl = sizeof(name) - 1;
          memcpy(name, e->t[e->i].s, nl);
          name[nl] = 0;
          defined = cny_mac_get(e->p, name) != NULL;
          e->i++;
        } else {
          e->ok = 0;
        }
        if (e->i < e->n && cny_ce_peek_is_punct(e, ")"))
          e->i++;
      } else if (e->i < e->n && e->t[e->i].kind == CNTK_IDENT) {
        char name[256];
        size_t nl = e->t[e->i].n;
        if (nl >= sizeof(name))
          nl = sizeof(name) - 1;
        memcpy(name, e->t[e->i].s, nl);
        name[nl] = 0;
        defined = cny_mac_get(e->p, name) != NULL;
        e->i++;
      }
      return defined ? 1 : 0;
    }
    if (cny_word_eq(t->s, t->n, "true"))
      return cny_ce_num(e, 1);
    if (cny_word_eq(t->s, t->n, "false"))
      return cny_ce_num(e, 0);
    /* macro value substitution */
    {
      char name[256];
      size_t nl = t->n;
      if (nl >= sizeof(name))
        nl = sizeof(name) - 1;
      memcpy(name, t->s, nl);
      name[nl] = 0;
      const char *val = cny_mac_get(e->p, name);
      if (val && *val) {
        cny_toks_t sub = {0};
        cny_lex(val, strlen(val), &sub);
        cny_cexpr_t se = {sub.items, sub.len, 0, e->p, e->ok};
        long long v = cny_ce_tern(&se);
        if (!se.ok)
          e->ok = 0;
        cny_toks_free(&sub);
        e->i++;
        return v;
      }
      e->ok = 0;
      return 0;
    }
  }
  if (t->kind == CNTK_INT || t->kind == CNTK_CHAR) {
    e->i++;
    return cny_ce_number_atom(t, &e->ok);
  }
  e->ok = 0;
  return 0;
}

static long long cny_ce_unary(cny_cexpr_t *e) {
  if (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT && t->n == 1) {
      char op = *t->s;
      if (op == '!' || op == '~' || op == '-' || op == '+') {
        e->i++;
        long long v = cny_ce_unary(e);
        if (op == '!')
          return v == 0 ? 1 : 0;
        if (op == '~')
          return ~v;
        if (op == '-')
          return -v;
        return v;
      }
    }
  }
  return cny_ce_primary(e);
}

static long long cny_ce_mul2(cny_cexpr_t *e) {
  long long v = cny_ce_unary(e);
  while (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT && t->n == 1 &&
        (*t->s == '*' || *t->s == '/' || *t->s == '%')) {
      char op = *t->s;
      e->i++;
      long long r = cny_ce_unary(e);
      if (!e->ok)
        return 0;
      if (op == '*')
        v = v * r;
      else if (op == '/')
        v = r ? v / r : 0;
      else
        v = r ? v % r : 0;
    } else
      break;
  }
  return v;
}

static long long cny_ce_add(cny_cexpr_t *e) {
  long long v = cny_ce_mul2(e);
  while (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT && t->n == 1 && (*t->s == '+' || *t->s == '-')) {
      char op = *t->s;
      e->i++;
      long long r = cny_ce_mul2(e);
      if (!e->ok)
        return 0;
      v = op == '+' ? v + r : v - r;
    } else
      break;
  }
  return v;
}

static long long cny_ce_shift(cny_cexpr_t *e) {
  long long v = cny_ce_add(e);
  while (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT && (cny_word_eq(t->s, t->n, "<<") ||
                                  cny_word_eq(t->s, t->n, ">>"))) {
      int left = *t->s == '<';
      e->i++;
      long long r = cny_ce_add(e);
      if (!e->ok)
        return 0;
      v = left ? (long long)((unsigned long long)v << r)
               : (long long)((unsigned long long)v >> r);
    } else
      break;
  }
  return v;
}

static long long cny_ce_rel(cny_cexpr_t *e) {
  long long v = cny_ce_shift(e);
  while (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT && t->n == 1 &&
        (*t->s == '<' || *t->s == '>')) {
      int lt = *t->s == '<';
      e->i++;
      long long r = cny_ce_shift(e);
      if (!e->ok)
        return 0;
      v = lt ? v < r : v > r;
    } else if (t->kind == CNTK_PUNCT &&
               (cny_word_eq(t->s, t->n, "<=") || cny_word_eq(t->s, t->n, ">="))) {
      int le = *t->s == '<';
      e->i++;
      long long r = cny_ce_shift(e);
      if (!e->ok)
        return 0;
      v = le ? v <= r : v >= r;
    } else
      break;
  }
  return v;
}

static long long cny_ce_eq(cny_cexpr_t *e) {
  long long v = cny_ce_rel(e);
  while (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT &&
        (cny_word_eq(t->s, t->n, "==") || cny_word_eq(t->s, t->n, "!="))) {
      int eq = *t->s == '=';
      e->i++;
      long long r = cny_ce_rel(e);
      if (!e->ok)
        return 0;
      v = eq ? v == r : v != r;
    } else
      break;
  }
  return v;
}

static long long cny_ce_band(cny_cexpr_t *e) {
  long long v = cny_ce_eq(e);
  while (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == '&') {
      e->i++;
      long long r = cny_ce_eq(e);
      if (!e->ok)
        return 0;
      v = v & r;
    } else
      break;
  }
  return v;
}

static long long cny_ce_xor(cny_cexpr_t *e) {
  long long v = cny_ce_band(e);
  while (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == '^') {
      e->i++;
      long long r = cny_ce_band(e);
      if (!e->ok)
        return 0;
      v = v ^ r;
    } else
      break;
  }
  return v;
}

static long long cny_ce_bor(cny_cexpr_t *e) {
  long long v = cny_ce_xor(e);
  while (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == '|') {
      e->i++;
      long long r = cny_ce_xor(e);
      if (!e->ok)
        return 0;
      v = v | r;
    } else
      break;
  }
  return v;
}

static long long cny_ce_land(cny_cexpr_t *e) {
  long long v = cny_ce_bor(e);
  while (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT && cny_word_eq(t->s, t->n, "&&")) {
      e->i++;
      long long r = cny_ce_bor(e);
      if (!e->ok)
        return 0;
      v = (v && r) ? 1 : 0;
    } else
      break;
  }
  return v;
}

static long long cny_ce_lor(cny_cexpr_t *e) {
  long long v = cny_ce_land(e);
  while (e->i < e->n) {
    const cny_tok_t *t = &e->t[e->i];
    if (t->kind == CNTK_PUNCT && cny_word_eq(t->s, t->n, "||")) {
      e->i++;
      long long r = cny_ce_land(e);
      if (!e->ok)
        return 0;
      v = (v || r) ? 1 : 0;
    } else
      break;
  }
  return v;
}

static long long cny_ce_tern(cny_cexpr_t *e) {
  long long c = cny_ce_lor(e);
  if (e->i < e->n && cny_ce_peek_is_punct(e, "?")) {
    e->i++;
    long long a = cny_ce_tern(e);
    if (e->i < e->n && cny_ce_peek_is_punct(e, ":")) {
      e->i++;
      long long b = cny_ce_tern(e);
      return c ? a : b;
    }
    e->ok = 0;
  }
  return c;
}

static long long cny_const_eval_range(cny_t *p, size_t from, size_t to,
                                      int *ok) {
  cny_cexpr_t e;
  e.t = p->toks + from;
  e.n = to - from;
  e.i = 0;
  e.p = p;
  e.ok = 1;
  long long v = cny_ce_tern(&e);
  if (e.i != e.n || !e.ok) {
    *ok = 0;
    return 0;
  }
  *ok = 1;
  return v;
}

/* ------------------------------------------------------------------ */
/* expression parser                                                   */
/* ------------------------------------------------------------------ */

enum {
  CNP_ASSIGN = 2,
  CNP_TERN = 3,
  CNP_LOR = 4,
  CNP_LAND = 5,
  CNP_BOR = 6,
  CNP_XOR = 7,
  CNP_BAND = 8,
  CNP_EQ = 9,
  CNP_REL = 10,
  CNP_SHIFT = 11,
  CNP_ADD = 12,
  CNP_MUL = 13,
  CNP_UNARY = 14,
  CNP_POST = 15,
  CNP_ATOM = 16
};

static int cny_binop(cny_t *p, const char **optext) {
  const cny_tok_t *t = cny_cur(p);
  if (t->kind != CNTK_PUNCT)
    return -1;
  static const struct {
    const char *op;
    int prec;
    int right_prec;
  } bins[] = {
      {"||", CNP_LOR, CNP_LOR},
      {"&&", CNP_LAND, CNP_LAND},
      {"|", CNP_BOR, CNP_BOR},
      {"^^", CNP_XOR, CNP_XOR},
      {"^", CNP_XOR, CNP_XOR},
      {"&", CNP_BAND, CNP_BAND},
      {"==", CNP_EQ, CNP_EQ},
      {"!=", CNP_EQ, CNP_EQ},
      {"<", CNP_REL, CNP_REL},
      {">", CNP_REL, CNP_REL},
      {"<=", CNP_REL, CNP_REL},
      {">=", CNP_REL, CNP_REL},
      {"<<", CNP_SHIFT, CNP_SHIFT},
      {">>", CNP_SHIFT, CNP_SHIFT},
      {"+", CNP_ADD, CNP_ADD},
      {"-", CNP_ADD, CNP_ADD},
      {"*", CNP_MUL, CNP_MUL},
      {"/", CNP_MUL, CNP_MUL},
      {"%", CNP_MUL, CNP_MUL},
      {NULL, 0, 0},
  };
  for (int i = 0; bins[i].op; i++) {
    if (cny_word_eq(t->s, t->n, bins[i].op)) {
      *optext = bins[i].op;
      return bins[i].prec;
    }
  }
  return -1;
}

static void cny_expr_node(cny_t *p, cny_sb_t *n, int *level, int minprec,
                          int root);

static int cny_type_token(const cny_t *p) {
  const cny_tok_t *t = cny_cur(p);
  if (t->kind != CNTK_IDENT)
    return 0;
  if (cny_is_type_kw(t->s, t->n))
    return 1;
  if (cny_is_known_type(t->s, t->n))
    return 1;
  if (cny_td_get_n(p, t->s, t->n))
    return 1;
  return 0;
}

static void cny_prefix(cny_t *p, cny_sb_t *n, int *level, int root) {
  const cny_tok_t *t = cny_cur(p);
  if (t->kind == CNTK_INT) {
    cny_emit_tok_value(n, t);
    *level = CNP_ATOM;
    cny_advance(p);
    return;
  }
  if (t->kind == CNTK_FLT) {
    cny_emit_tok_value(n, t);
    *level = CNP_ATOM;
    cny_advance(p);
    return;
  }
  if (t->kind == CNTK_STR) {
    cny_sb_addn(n, t->s, t->n);
    *level = CNP_ATOM;
    cny_advance(p);
    return;
  }
  if (t->kind == CNTK_CHAR) {
    long long v = cny_char_value(t->s + 1, t->n - 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", v);
    cny_sb_add(n, buf);
    *level = CNP_ATOM;
    cny_advance(p);
    return;
  }
  if (t->kind == CNTK_IDENT) {
    if (cny_word_eq(t->s, t->n, "NULL")) {
      cny_sb_add(n, "nil");
      *level = CNP_ATOM;
      cny_advance(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "true") || cny_word_eq(t->s, t->n, "false")) {
      cny_sb_addn(n, t->s, t->n);
      *level = CNP_ATOM;
      cny_advance(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "sizeof")) {
      cny_advance(p);
      if (cny_at(p, "(") && cny_type_token(p)) {
        cny_sb_add(n, "sizeof(");
        cny_advance(p);
        /* consume type tokens until ')' */
        while (!cny_at(p, ")") && cny_cur(p)->kind != CNTK_EOF) {
          const cny_tok_t *ct = cny_cur(p);
          if (ct->kind == CNTK_IDENT)
            cny_sb_add(n, cny_scalar_type(ct->s, ct->n));
          else if (ct->kind == CNTK_PUNCT && ct->n == 1 && *ct->s == '*')
            cny_sb_add(n, "ptr");
          cny_advance(p);
        }
        cny_sb_add(n, ")");
        cny_advance(p); /* ')' */
      } else {
        if (!(p->notes & CNY_NOTE_SIZEOF)) {
          p->notes |= CNY_NOTE_SIZEOF;
          cny_warn_at(p, t->line, "note",
                      "sizeof on a non-type operand approximated as 0");
        }
        cny_sb_add(n, "0");
      }
      *level = CNP_ATOM;
      return;
    }
    if (cny_word_eq(t->s, t->n, "printf")) {
      if (!(p->notes & CNY_NOTE_PRINTF)) {
        p->notes |= CNY_NOTE_PRINTF;
        cny_warn_at(p, t->line, "note",
                    "printf converted to print; format string is printed "
                    "literally");
      }
      cny_sb_add(n, "print");
      *level = CNP_ATOM;
      cny_advance(p);
      return;
    }
    cny_sb_addn(n, t->s, t->n);
    *level = CNP_ATOM;
    cny_advance(p);
    return;
  }
  if (t->kind == CNTK_PUNCT) {
    char c = *t->s;
    if (c == '(') {
      /* possible cast or grouping */
      cny_advance(p);
      if (cny_type_token(p)) {
        /* cast */
        cny_sb_t tmp = {0};
        int tlevel = CNP_ATOM;
        /* read type tokens */
        cny_sb_clear(&tmp);
        while (!cny_at(p, ")") && cny_cur(p)->kind != CNTK_EOF) {
          const cny_tok_t *ct = cny_cur(p);
          if (ct->kind == CNTK_IDENT) {
            const char *sc = cny_scalar_type(ct->s, ct->n);
            cny_sb_add(&tmp, sc);
          } else if (ct->kind == CNTK_PUNCT && ct->n == 1 && *ct->s == '*') {
            cny_sb_add(&tmp, "any");
          } else if (ct->kind == CNTK_PUNCT && ct->n == 1 && *ct->s == ']') {
            /* array type in cast - ignore */
          }
          cny_advance(p);
        }
        cny_advance(p); /* ')' */
        cny_expr_node(p, &tmp, &tlevel, CNP_UNARY, 0);
        (void)root;
        cny_sb_add(n, tmp.data ? tmp.data : "");
        free(tmp.data);
        *level = CNP_UNARY;
        return;
      }
      /* grouping */
      cny_sb_add(n, "(");
      cny_expr_node(p, n, level, CNP_ASSIGN, 0);
      if (!cny_at(p, ")"))
        p->errors++;
      else
        cny_advance(p);
      cny_sb_add(n, ")");
      *level = CNP_POST;
      return;
    }
    if (c == '!' || c == '-' || c == '~' || c == '+') {
      char op = c;
      cny_advance(p);
      if (op == '-') {
        cny_sb_add(n, "-");
      } else if (op == '+') {
        cny_sb_add(n, "+");
      } else if (op == '!') {
        cny_sb_add(n, "!(");
        cny_expr_node(p, n, level, CNP_UNARY, 0);
        cny_sb_add(n, ")");
        *level = CNP_UNARY;
        return;
      } else {
        cny_sb_add(n, "~");
      }
      cny_expr_node(p, n, level, CNP_UNARY, 0);
      *level = CNP_UNARY;
      return;
    }
    if (c == '&') {
      cny_advance(p);
      cny_sb_add(n, "addr_of(");
      cny_expr_node(p, n, level, CNP_UNARY, 0);
      cny_sb_add(n, ")");
      *level = CNP_UNARY;
      return;
    }
    if (c == '*') {
      int line = t->line;
      cny_advance(p);
      cny_warn_at(p, line, "unsupported", "pointer dereference");
      cny_sb_add(n, "0");
      cny_expr_node(p, n, level, CNP_UNARY, 0);
      *level = CNP_UNARY;
      return;
    }
    if (c == '+' && t->n == 2) {
      /* ++ prefix */
      int line = t->line;
      cny_advance(p);
      cny_sb_add(n, "(");
      cny_expr_node(p, n, level, CNP_UNARY, 0);
      cny_sb_add(n, ")");
      if (root) {
        cny_sb_add(n, " += 1");
        *level = CNP_ASSIGN;
      } else {
        cny_warn_at(p, line, "unsupported",
                    "++ inside an expression; increment dropped");
        *level = CNP_POST;
      }
      return;
    }
    if (c == '-' && t->n == 2) {
      /* -- prefix */
      int line = t->line;
      cny_advance(p);
      cny_sb_add(n, "(");
      cny_expr_node(p, n, level, CNP_UNARY, 0);
      cny_sb_add(n, ")");
      if (root) {
        cny_sb_add(n, " -= 1");
        *level = CNP_ASSIGN;
      } else {
        cny_warn_at(p, line, "unsupported",
                    "-- inside an expression; decrement dropped");
        *level = CNP_POST;
      }
      return;
    }
    /* unexpected punct */
    p->errors++;
    cny_warn_at(p, t->line, "unsupported", "unexpected token '%.*s'",
                (int)t->n, t->s);
    *level = CNP_ATOM;
    return;
  }
  if (t->kind == CNTK_DIRECTIVE) {
    p->errors++;
    cny_warn_at(p, t->line, "unsupported", "preprocessor directive inside a "
                "function body");
    cny_advance(p);
    *level = CNP_ATOM;
    return;
  }
  p->errors++;
  *level = CNP_ATOM;
}

static void cny_postfix(cny_t *p, cny_sb_t *n, int *level, int root) {
  cny_prefix(p, n, level, root);
  for (;;) {
    const cny_tok_t *t = cny_cur(p);
    if (t->kind == CNTK_PUNCT) {
      if (t->n == 1 && *t->s == '[') {
        cny_advance(p);
        cny_sb_add(n, "[");
        int ilevel = 0;
        cny_expr_node(p, n, &ilevel, CNP_ASSIGN, 0);
        cny_sb_add(n, "]");
        if (!cny_at(p, "]"))
          p->errors++;
        else
          cny_advance(p);
        *level = CNP_POST;
        continue;
      }
      if (t->n == 1 && *t->s == '(') {
        /* function call */
        if (cny_word_eq(t->s, t->n, "printf") && 0) {
        }
        cny_advance(p);
        cny_sb_add(n, "(");
        if (!cny_at(p, ")")) {
          for (;;) {
            int ilevel = 0;
            cny_expr_node(p, n, &ilevel, CNP_ASSIGN, 0);
            if (cny_at(p, ",")) {
              cny_advance(p);
              cny_sb_add(n, ", ");
              continue;
            }
            break;
          }
        }
        cny_sb_add(n, ")");
        if (!cny_at(p, ")"))
          p->errors++;
        else
          cny_advance(p);
        *level = CNP_POST;
        continue;
      }
      if (t->n == 1 && *t->s == '.') {
        cny_advance(p);
        cny_sb_add(n, ".");
        if (cny_cur(p)->kind == CNTK_IDENT) {
          cny_sb_addn(n, cny_cur(p)->s, cny_cur(p)->n);
          cny_advance(p);
        }
        *level = CNP_POST;
        continue;
      }
      if (cny_word_eq(t->s, t->n, "->")) {
        int line = t->line;
        cny_advance(p);
        if (cny_cur(p)->kind == CNTK_IDENT) {
          cny_warn_at(p, line, "unsupported", "pointer member access '->'");
          cny_sb_add(n, ".");
          cny_sb_addn(n, cny_cur(p)->s, cny_cur(p)->n);
          cny_advance(p);
        }
        *level = CNP_POST;
        continue;
      }
      if (cny_word_eq(t->s, t->n, "++") || cny_word_eq(t->s, t->n, "--")) {
        int line = t->line;
        int is_inc = *t->s == '+';
        cny_advance(p);
        if (root) {
          cny_sb_add(n, is_inc ? " += 1" : " -= 1");
          *level = CNP_ASSIGN;
        } else {
          cny_warn_at(p, line, "unsupported",
                      "++/-- inside an expression; increment dropped");
        }
        continue;
      }
    }
    break;
  }
}

static void cny_expr_node(cny_t *p, cny_sb_t *n, int *level, int minprec,
                          int root) {
  cny_postfix(p, n, level, root);
  for (;;) {
    /* assignment? */
    if (root) {
      const cny_tok_t *t = cny_cur(p);
      if (t->kind == CNTK_PUNCT &&
          (t->n == 1 && (*t->s == '=' || *t->s == '&' || *t->s == '|' ||
                         *t->s == '^')) &&
          !(t->n == 2)) {
        char op = *t->s;
        if (op == '=' || (op == '&' && !cny_at(p, "&")) ||
            (op == '|' && !cny_at(p, "|")) || (op == '^' && !cny_at(p, "^"))) {
          cny_advance(p);
          cny_sb_add(n, " = ");
          int rlevel = 0;
          cny_expr_node(p, n, &rlevel, CNP_ASSIGN, 0);
          *level = CNP_ASSIGN;
          continue;
        }
      } else if (t->kind == CNTK_PUNCT && t->n == 2 &&
                 (cny_word_eq(t->s, t->n, "+=") ||
                  cny_word_eq(t->s, t->n, "-=") ||
                  cny_word_eq(t->s, t->n, "*=") ||
                  cny_word_eq(t->s, t->n, "/=") ||
                  cny_word_eq(t->s, t->n, "%="))) {
        cny_sb_add(n, " ");
        cny_sb_addn(n, t->s, t->n);
        cny_advance(p);
        cny_sb_add(n, " ");
        int rlevel = 0;
        cny_expr_node(p, n, &rlevel, CNP_ASSIGN, 0);
        *level = CNP_ASSIGN;
        continue;
      } else if (t->kind == CNTK_PUNCT && t->n == 2 &&
                 (cny_word_eq(t->s, t->n, "<<=") ||
                  cny_word_eq(t->s, t->n, ">>=") ||
                  cny_word_eq(t->s, t->n, "&=") ||
                  cny_word_eq(t->s, t->n, "|=") ||
                  cny_word_eq(t->s, t->n, "^="))) {
        char op = *t->s;
        int is_shift_left = cny_word_eq(t->s, t->n, "<<=");
        int is_shift_right = cny_word_eq(t->s, t->n, ">>=");
        const char *op2 = NULL;
        if (is_shift_left) op2 = "<<";
        else if (is_shift_right) op2 = ">>";
        else if (op == '&') op2 = "&";
        else if (op == '|') op2 = "|";
        else if (op == '^') op2 = "^^";
        cny_advance(p);
        int rlevel = 0;
        cny_expr_node(p, n, &rlevel, CNP_ASSIGN, 0);
        cny_sb_t rhs = {0};
        /* re-emit as x = x op2 rhs: rebuild node text */
        (void)op2;
        (void)rhs;
        cny_sb_add(n, " = ");
        /* we need the lvalue again; simplest: leave the compound as-is plus marker */
        p->errors++;
        cny_warn_at(p, t->line, "unsupported", "compound assignment '%.*s'",
                    (int)t->n, t->s);
        *level = CNP_ASSIGN;
        continue;
      }
    }
    const char *optext = NULL;
    int prec = cny_binop(p, &optext);
    if (prec < 0 || prec < minprec)
      break;
    cny_advance(p);
    /* rewrite ^ to ^^ */
    int is_xor = strcmp(optext, "^") == 0;
    if (is_xor)
      optext = "^^";
    cny_sb_add(n, " ");
    cny_sb_add(n, optext);
    cny_sb_add(n, " ");
    int rlevel = 0;
    cny_expr_node(p, n, &rlevel, prec + 1, 0);
    if (is_xor) {
      /* Ny ^^ precedence is unknown; parenthesize compound operands */
      /* operand parens are added by the caller when needed via level checks */
    }
    *level = prec;
  }
  /* ternary */
  if (minprec <= CNP_TERN && cny_at(p, "?")) {
    cny_advance(p);
    cny_sb_add(n, " ? ");
    int mlevel = 0;
    cny_expr_node(p, n, &mlevel, CNP_TERN, 0);
    if (cny_at(p, ":")) {
      cny_advance(p);
      cny_sb_add(n, " : ");
      int blevel = 0;
      cny_expr_node(p, n, &blevel, CNP_TERN, 0);
    }
    *level = CNP_TERN;
  }
}

/* Parse an expression at the current position; emit into p->ln. */
static void cny_parse_expr_stmt_value(cny_t *p, int *level) {
  int lv = 0;
  cny_sb_t n = {0};
  cny_expr_node(p, &n, &lv, CNP_ASSIGN, 1);
  cny_emit_node(p, &n);
  free(n.data);
  if (level)
    *level = lv;
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static int cny_decl_start(const cny_t *p) {
  const cny_tok_t *t = cny_cur(p);
  if (t->kind != CNTK_IDENT)
    return 0;
  return cny_is_type_kw(t->s, t->n) || cny_is_known_type(t->s, t->n) ||
         cny_td_get_n(p, t->s, t->n) || cny_word_eq(t->s, t->n, "struct") ||
         cny_word_eq(t->s, t->n, "enum");
}

/* parse declarator; returns 1 and sets name/pointer if it is `NAME` */
static int cny_parse_name(cny_t *p, cny_sb_t *name, int *is_ptr) {
  *is_ptr = 0;
  /* skip pointer stars and qualifiers */
  for (;;) {
    const cny_tok_t *t = cny_cur(p);
    if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == '*') {
      *is_ptr = 1;
      cny_advance(p);
      continue;
    }
    if (t->kind == CNTK_IDENT &&
        (cny_word_eq(t->s, t->n, "const") ||
         cny_word_eq(t->s, t->n, "volatile") ||
         cny_word_eq(t->s, t->n, "restrict") ||
         cny_word_eq(t->s, t->n, "register"))) {
      cny_advance(p);
      continue;
    }
    break;
  }
  if (cny_cur(p)->kind == CNTK_IDENT) {
    const cny_tok_t *t = cny_cur(p);
    cny_sb_addn(name, t->s, t->n);
    cny_advance(p);
    return 1;
  }
  return 0;
}

static void cny_skip_balanced(cny_t *p);

static void cny_parse_expr_statement(cny_t *p) {
  cny_parse_expr_stmt_value(p, NULL);
  /* consume up to ';' */
  while (!cny_at(p, ";") && cny_cur(p)->kind != CNTK_EOF) {
    const cny_tok_t *t = cny_cur(p);
    if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == '}') {
      cny_warn_at(p, t->line, "unsupported",
                  "missing ';' before '}'");
      return;
    }
    cny_advance(p);
  }
  if (cny_at(p, ";"))
    cny_advance(p);
  cny_flush(p);
}

static void cny_parse_local_decl(cny_t *p);

static void cny_parse_block(cny_t *p) {
  /* bare `{...}` at statement level is flattened: Ny has no block-as-statement */
  if (!cny_at(p, "{"))
    return;
  cny_advance(p);
  while (!cny_at(p, "}") && cny_cur(p)->kind != CNTK_EOF) {
    cny_parse_statement(p);
  }
  if (cny_at(p, "}"))
    cny_advance(p);
}

/*
 * Parse the body of a control-flow construct whose opening '{' has already
 * been emitted. If the body is a compound statement, consume its braces;
 * otherwise parse the single statement. Statements are emitted indented.
 */
static void cny_parse_body(cny_t *p) {
  if (cny_at(p, "{")) {
    cny_advance(p);
    p->indent++;
    while (!cny_at(p, "}") && cny_cur(p)->kind != CNTK_EOF) {
      cny_parse_statement(p);
    }
    if (cny_at(p, "}"))
      cny_advance(p);
    p->indent--;
  } else {
    p->indent++;
    cny_parse_statement(p);
    p->indent--;
  }
}

static void cny_parse_if(cny_t *p) {
  cny_advance(p); /* if */
  if (cny_at(p, "("))
    cny_advance(p);
  cny_emit(p, "if (");
  cny_parse_expr_stmt_value(p, NULL);
  if (cny_at(p, ")"))
    cny_advance(p);
  cny_emit(p, ") {");
  cny_flush(p);
  cny_parse_body(p);
  while (cny_at_word(p, "else")) {
    cny_advance(p);
    if (cny_at_word(p, "if")) {
      cny_advance(p);
      if (cny_at(p, "("))
        cny_advance(p);
      cny_emit(p, "} elif (");
      cny_parse_expr_stmt_value(p, NULL);
      if (cny_at(p, ")"))
        cny_advance(p);
      cny_emit(p, ") {");
      cny_flush(p);
      cny_parse_body(p);
    } else {
      cny_emit(p, "} else {");
      cny_flush(p);
      cny_parse_body(p);
      break;
    }
  }
  cny_emit(p, "}");
  cny_flush(p);
}

static void cny_parse_while(cny_t *p) {
  cny_advance(p);
  if (cny_at(p, "("))
    cny_advance(p);
  cny_emit(p, "while (");
  cny_parse_expr_stmt_value(p, NULL);
  if (cny_at(p, ")"))
    cny_advance(p);
  cny_emit(p, ") {");
  cny_flush(p);
  cny_parse_body(p);
  cny_emit(p, "}");
  cny_flush(p);
}

static void cny_parse_do(cny_t *p) {
  cny_advance(p); /* do */
  cny_emit(p, "while (true) {");
  cny_flush(p);
  cny_parse_body(p);
  /* expect while (cond) ; */
  if (cny_at_word(p, "while")) {
    cny_advance(p);
    if (cny_at(p, "("))
      cny_advance(p);
    p->indent++;
    cny_emit(p, "if !(");
    cny_parse_expr_stmt_value(p, NULL);
    if (cny_at(p, ")"))
      cny_advance(p);
    cny_emit(p, ") { break }");
    cny_flush(p);
    p->indent--;
  }
  cny_emit(p, "}");
  cny_flush(p);
  while (cny_at(p, ";"))
    cny_advance(p);
}

/* gather one case/default label arm value into buf */
static int cny_case_label(cny_t *p, cny_sb_t *buf, int *is_default) {
  *is_default = 0;
  cny_sb_clear(buf);
  if (cny_at_word(p, "default")) {
    cny_advance(p);
    *is_default = 1;
    if (cny_at(p, ":"))
      cny_advance(p);
    return 1;
  }
  if (!cny_at_word(p, "case")) {
    return 0;
  }
  cny_advance(p);
  /* collect label expression tokens up to ':' (or '...' for range) */
  size_t start = p->ti;
  int range = 0;
  while (cny_cur(p)->kind != CNTK_EOF && !cny_at(p, ":")) {
    if (cny_cur(p)->kind == CNTK_PUNCT &&
        cny_word_eq(cny_cur(p)->s, cny_cur(p)->n, "...")) {
      range = 1;
      break;
    }
    cny_advance(p);
  }
  size_t end = p->ti;
  int ok = 1;
  long long v = cny_const_eval_range(p, start, end, &ok);
  char vbuf[32];
  snprintf(vbuf, sizeof(vbuf), "%lld", v);
  if (!ok) {
    cny_warn_at(p, cny_cur(p)->line, "unsupported",
                "case label is not a constant expression");
    cny_sb_add(buf, "0");
  } else {
    cny_sb_add(buf, vbuf);
  }
  if (range) {
    cny_advance(p); /* ... */
    size_t rstart = p->ti;
    while (cny_cur(p)->kind != CNTK_EOF && !cny_at(p, ":"))
      cny_advance(p);
    size_t rend = p->ti;
    long long rv = cny_const_eval_range(p, rstart, rend, &ok);
    char rbuf[32];
    snprintf(rbuf, sizeof(rbuf), "%lld", ok ? rv : 0);
    cny_sb_add(buf, "..");
    cny_sb_add(buf, rbuf);
  }
  if (cny_at(p, ":"))
    cny_advance(p);
  return 1;
}

static void cny_parse_switch(cny_t *p) {
  cny_advance(p); /* switch */
  if (cny_at(p, "("))
    cny_advance(p);
  cny_emit(p, "case (");
  cny_parse_expr_stmt_value(p, NULL);
  if (cny_at(p, ")"))
    cny_advance(p);
  cny_emit(p, ") {");
  cny_flush(p);
  if (cny_at(p, "{"))
    cny_advance(p);
  p->indent++;

  cny_sb_t labels = {0};
  cny_sb_t hdr = {0};
  int have_arm = 0;
  int is_default = 0;
  int arm_stmts = 0;
  int hdr_emitted = 0;
  while (!cny_at(p, "}") && cny_cur(p)->kind != CNTK_EOF) {
    if (cny_at_word(p, "case") || cny_at_word(p, "default")) {
      cny_sb_t newlab = {0};
      int new_default = 0;
      if (!cny_case_label(p, &newlab, &new_default)) {
        cny_warn_at(p, cny_cur(p)->line, "unsupported", "malformed switch label");
        cny_advance(p);
        continue;
      }
      if (have_arm && arm_stmts == 0 && hdr_emitted == 0) {
        /* empty previous arm: C fall-through, merge into this arm's labels */
        if (!new_default && !is_default) {
          cny_sb_add(&labels, ", ");
          cny_sb_add(&labels, newlab.data);
          cny_sb_clear(&hdr);
          cny_sb_add(&hdr, labels.data);
          cny_sb_add(&hdr, " {");
          free(newlab.data);
          continue;
        }
        cny_warn_at(p, cny_cur(p)->line, "unsupported",
                    "fall-through from a case into default is not representable");
      }
      /* close current arm */
      if (have_arm) {
        if (hdr_emitted)
          p->indent--;
        cny_emit(p, "}");
        cny_flush(p);
      }
      cny_sb_clear(&labels);
      cny_sb_clear(&hdr);
      if (new_default)
        cny_sb_add(&labels, "_");
      else
        cny_sb_add(&labels, newlab.data);
      cny_sb_add(&hdr, labels.data);
      cny_sb_add(&hdr, " {");
      is_default = new_default;
      arm_stmts = 0;
      hdr_emitted = 0;
      free(newlab.data);
      have_arm = 1;
      continue;
    }
    if (!have_arm) {
      cny_warn_at(p, cny_cur(p)->line, "unsupported",
                  "statement outside a switch label");
      cny_advance(p);
      continue;
    }
    /* trailing break detection: drop a `break;` right before a label or '}' */
    if (cny_at_word(p, "break") &&
        (cny_peek(p, 1)->kind == CNTK_PUNCT &&
         cny_peek(p, 1)->n == 1 && *cny_peek(p, 1)->s == ';') &&
        (cny_peek(p, 2)->kind == CNTK_IDENT &&
         (cny_word_eq(cny_peek(p, 2)->s, cny_peek(p, 2)->n, "case") ||
          cny_word_eq(cny_peek(p, 2)->s, cny_peek(p, 2)->n, "default")))) {
      cny_advance(p); /* break */
      cny_advance(p); /* ; */
      continue;
    }
    if (cny_at_word(p, "break") &&
        (cny_peek(p, 1)->kind == CNTK_PUNCT &&
         cny_peek(p, 1)->n == 1 && *cny_peek(p, 1)->s == ';') &&
        (cny_peek(p, 2)->kind == CNTK_PUNCT &&
         cny_peek(p, 2)->n == 1 && *cny_peek(p, 2)->s == '}')) {
      cny_advance(p); /* break */
      cny_advance(p); /* ; */
      continue;
    }
    if (!hdr_emitted) {
      cny_emit(p, hdr.data);
      cny_flush(p);
      hdr_emitted = 1;
      p->indent++;
    }
    cny_parse_statement(p);
    arm_stmts++;
  }
  if (have_arm) {
    if (hdr_emitted)
      p->indent--;
    cny_emit(p, "}");
    cny_flush(p);
  }
  if (cny_at(p, "}"))
    cny_advance(p);
  p->indent--;
  cny_emit(p, "}");
  cny_flush(p);
  free(labels.data);
  free(hdr.data);
}

static void cny_parse_for(cny_t *p) {
  int line = cny_cur(p)->line;
  cny_advance(p); /* for */
  if (cny_at(p, "("))
    cny_advance(p);
  /* init clause: up to first ';' at depth 0 */
  size_t init_start = p->ti;
  int depth = 0;
  while (cny_cur(p)->kind != CNTK_EOF) {
    const cny_tok_t *t = cny_cur(p);
    if (t->kind == CNTK_PUNCT && t->n == 1) {
      char c = *t->s;
      if (c == '(' || c == '[')
        depth++;
      else if (c == ')' || c == ']') {
        if (depth == 0 && c == ')')
          break;
        if (depth > 0)
          depth--;
      } else if (c == ';' && depth == 0)
        break;
    }
    cny_advance(p);
  }
  size_t init_end = p->ti;
  if (cny_at(p, ";"))
    cny_advance(p);
  /* cond clause: up to next ';' at depth 0 */
  size_t cond_start = p->ti;
  depth = 0;
  while (cny_cur(p)->kind != CNTK_EOF) {
    const cny_tok_t *t = cny_cur(p);
    if (t->kind == CNTK_PUNCT && t->n == 1) {
      char c = *t->s;
      if (c == '(' || c == '[')
        depth++;
      else if (c == ')' || c == ']') {
        if (depth == 0 && c == ')')
          break;
        if (depth > 0)
          depth--;
      } else if (c == ';' && depth == 0)
        break;
    }
    cny_advance(p);
  }
  size_t cond_end = p->ti;
  if (cny_at(p, ";"))
    cny_advance(p);
  /* update clause: up to ')' at depth 0 */
  size_t upd_start = p->ti;
  depth = 0;
  while (cny_cur(p)->kind != CNTK_EOF) {
    const cny_tok_t *t = cny_cur(p);
    if (t->kind == CNTK_PUNCT && t->n == 1) {
      char c = *t->s;
      if (c == '(' || c == '[')
        depth++;
      else if (c == ')' || c == ']') {
        if (depth == 0 && c == ')')
          break;
        if (depth > 0)
          depth--;
      }
    }
    cny_advance(p);
  }
  size_t upd_end = p->ti;
  if (cny_at(p, ")"))
    cny_advance(p);

  int init_is_decl = 0;
  cny_sb_t initvar = {0};
  cny_sb_t initval = {0};
  if (init_end > init_start) {
    const cny_tok_t *t0 = &p->toks[init_start];
    if (cny_is_type_kw(t0->s, t0->n) || cny_is_known_type(t0->s, t0->n) ||
        cny_td_get_n(p, t0->s, t0->n) || cny_word_eq(t0->s, t0->n, "struct")) {
      init_is_decl = 1;
      /* find the variable name: first IDENT after the type keywords/stars */
      size_t j = init_start;
      while (j < init_end && cny_is_type_kw(p->toks[j].s, p->toks[j].n))
        j++;
      while (j < init_end && p->toks[j].kind == CNTK_PUNCT && p->toks[j].n == 1 &&
             *p->toks[j].s == '*')
        j++;
      if (j < init_end && p->toks[j].kind == CNTK_IDENT) {
        cny_sb_addn(&initvar, p->toks[j].s, p->toks[j].n);
        j++;
      }
      /* optional '= value' */
      if (j < init_end && cny_word_eq(p->toks[j].s, p->toks[j].n, "=")) {
        for (size_t k = j + 1; k < init_end; k++) {
          if (k > j + 1)
            cny_sb_add(&initval, " ");
          cny_emit_tok_value(&initval, &p->toks[k]);
        }
      }
    }
  }

  /* cond / update text */
  cny_sb_t cond = {0};
  for (size_t k = cond_start; k < cond_end; k++) {
    if (k > cond_start)
      cny_sb_add(&cond, " ");
    cny_emit_tok_value(&cond, &p->toks[k]);
  }
  cny_sb_t upd = {0};
  for (size_t k = upd_start; k < upd_end; k++) {
    if (k > upd_start)
      cny_sb_add(&upd, " ");
    cny_emit_tok_value(&upd, &p->toks[k]);
  }
  /* normalize ++/-- in update */
  {
    char *u = upd.data;
    if (u) {
      size_t ul = strlen(u);
      char *repl = (char *)malloc(ul * 2 + 8);
      if (repl) {
        size_t w = 0;
        for (size_t k = 0; k < ul;) {
          if (u[k] == '+' && k + 1 < ul && u[k + 1] == '+') {
            /* find the identifier before */
            size_t s = k;
            while (s > 0 && u[s - 1] != ' ') s--;
            /* remove trailing ++ and add += 1 */
            size_t idlen = k - s;
            memcpy(repl + w, u + s, idlen);
            w += idlen;
            memcpy(repl + w, " += 1", 5);
            w += 5;
            k += 2;
            /* skip spaces after */
            while (k < ul && u[k] == ' ')
              k++;
          } else if (u[k] == '-' && k + 1 < ul && u[k + 1] == '-') {
            size_t s = k;
            while (s > 0 && u[s - 1] != ' ')
              s--;
            size_t idlen = k - s;
            memcpy(repl + w, u + s, idlen);
            w += idlen;
            memcpy(repl + w, " -= 1", 5);
            w += 5;
            k += 2;
            while (k < ul && u[k] == ' ')
              k++;
          } else {
            repl[w++] = u[k++];
          }
        }
        repl[w] = 0;
        free(upd.data);
        upd.data = repl;
        upd.len = w;
        upd.cap = 0;
      }
    }
  }

  int use_header =
      init_is_decl && initvar.len > 0 && cond.len > 0 && upd.len > 0;
  if (use_header) {
    cny_emit(p, "for (mut ");
    cny_emit(p, initvar.data);
    cny_emit(p, " = ");
    if (initval.len > 0)
      cny_emit(p, initval.data);
    else
      cny_emit(p, "0");
    cny_emit(p, " ");
    cny_emit(p, cond.data);
    if (upd.len > 0) {
      cny_emit(p, " ");
      cny_emit(p, upd.data);
    }
    cny_emit(p, ") {");
    cny_flush(p);
    cny_parse_body(p);
    cny_emit(p, "}");
    cny_flush(p);
  } else {
    size_t body = p->ti; /* position after the for-header ')' */
    /* while expansion */
    if (initval.len > 0 && initvar.len > 0 && init_is_decl) {
      cny_emit(p, "mut ");
      cny_emit(p, initvar.data);
      cny_emit(p, " = ");
      cny_emit(p, initval.data);
      cny_flush(p);
    } else if (init_end > init_start && !init_is_decl) {
      /* expression init statement, e.g. for (i = 0; ...) */
      p->ti = init_start;
      cny_parse_expr_stmt_value(p, NULL);
      p->ti = body;
      cny_flush(p);
    }
    if (cond.len > 0) {
      cny_emit(p, "while (");
      cny_emit(p, cond.data);
      cny_emit(p, ") {");
    } else {
      cny_emit(p, "while (true) {");
    }
    cny_flush(p);
    cny_parse_body(p);
    if (upd.len > 0) {
      cny_emit(p, upd.data);
      cny_flush(p);
    }
    cny_emit(p, "}");
    cny_flush(p);
    if (cond.len == 0 && upd.len == 0) {
      cny_warn_at(p, line, "unsupported",
                  "for(;;) has no exit condition");
    }
  }
  free(initvar.data);
  free(initval.data);
  free(cond.data);
  free(upd.data);
}

static void cny_parse_return(cny_t *p) {
  int line = cny_cur(p)->line;
  cny_advance(p);
  cny_emit(p, "return");
  if (!cny_at(p, ";") && cny_cur(p)->kind != CNTK_EOF &&
      !cny_at(p, "}")) {
    cny_emit(p, " ");
    cny_parse_expr_stmt_value(p, NULL);
  }
  while (!cny_at(p, ";") && cny_cur(p)->kind != CNTK_EOF && !cny_at(p, "}")) {
    const cny_tok_t *t = cny_cur(p);
    if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == '}')
      break;
    cny_advance(p);
  }
  if (cny_at(p, ";"))
    cny_advance(p);
  if (p->in_main) {
    if (!(p->notes & CNY_NOTE_MAIN_RET)) {
      p->notes |= CNY_NOTE_MAIN_RET;
      cny_warn_at(p, line, "note",
                  "return in main is dropped (Ny #main has no return value)");
    }
    cny_sb_clear(&p->ln);
  } else {
    cny_flush(p);
  }
}

static void cny_skip_balanced(cny_t *p) {
  int depth = 0;
  while (cny_cur(p)->kind != CNTK_EOF) {
    const cny_tok_t *t = cny_cur(p);
    if (t->kind == CNTK_PUNCT && t->n == 1) {
      if (*t->s == '{') {
        depth++;
        cny_advance(p);
        continue;
      }
      if (*t->s == '}') {
        if (depth == 0) {
          return;
        }
        depth--;
        cny_advance(p);
        continue;
      }
      if (*t->s == ';' && depth == 0) {
        cny_advance(p);
        return;
      }
    }
    cny_advance(p);
  }
}

static void cny_parse_statement(cny_t *p) {
  const cny_tok_t *t = cny_cur(p);
  if (t->kind == CNTK_EOF)
    return;
  if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == ';') {
    cny_advance(p);
    return;
  }
  if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == '{') {
    cny_parse_block(p);
    return;
  }
  if (t->kind == CNTK_IDENT) {
    if (cny_word_eq(t->s, t->n, "if")) {
      cny_parse_if(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "for")) {
      cny_parse_for(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "while")) {
      cny_parse_while(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "do")) {
      cny_parse_do(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "switch")) {
      cny_parse_switch(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "return")) {
      cny_parse_return(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "break")) {
      cny_advance(p);
      cny_emit(p, "break");
      cny_flush(p);
      while (cny_at(p, ";"))
        cny_advance(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "continue")) {
      cny_advance(p);
      cny_emit(p, "continue");
      cny_flush(p);
      while (cny_at(p, ";"))
        cny_advance(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "goto")) {
      int line = t->line;
      cny_advance(p);
      cny_warn_at(p, line, "unsupported", "goto");
      cny_skip_balanced(p);
      return;
    }
    if (cny_word_eq(t->s, t->n, "typedef") ||
        cny_word_eq(t->s, t->n, "struct") ||
        cny_word_eq(t->s, t->n, "enum")) {
      /* nested type declaration */
      int line = t->line;
      cny_warn_at(p, line, "unsupported", "nested type declaration");
      cny_skip_balanced(p);
      return;
    }
    if (cny_decl_start(p)) {
      cny_parse_local_decl(p);
      return;
    }
  }
  if (t->kind == CNTK_DIRECTIVE) {
    int line = t->line;
    cny_warn_at(p, line, "unsupported",
                "preprocessor directive inside a function body");
    cny_advance(p);
    return;
  }
  cny_parse_expr_statement(p);
}

static void cny_emit_list_fill(cny_t *p, const char *name, size_t n) {
  /* emit `mut name = [0,0,...]` for constant n */
  cny_emit(p, "mut ");
  cny_emit(p, name);
  cny_emit(p, " = [");
  for (size_t i = 0; i < n; i++) {
    if (i)
      cny_emit(p, ", ");
    cny_emit(p, "0");
  }
  cny_emit(p, "]");
  cny_flush(p);
}

static void cny_parse_local_decl(cny_t *p) {
  int line = cny_cur(p)->line;
  /* consume declaration specifiers */
  cny_sb_t spec = {0};
  int is_ptr = 0;
  int struct_ty = 0;
  while (cny_cur(p)->kind == CNTK_IDENT && cny_type_token(p)) {
    const cny_tok_t *t = cny_cur(p);
    if (cny_word_eq(t->s, t->n, "struct") ||
        cny_word_eq(t->s, t->n, "union") ||
        cny_word_eq(t->s, t->n, "enum")) {
      struct_ty = 1;
      if (cny_word_eq(t->s, t->n, "union"))
        cny_warn_at(p, line, "unsupported", "union type");
      cny_advance(p);
      if (cny_cur(p)->kind == CNTK_IDENT) {
        cny_sb_add(&spec, cny_cur(p)->s);
        cny_sb_add(&spec, " ");
        cny_advance(p);
      }
      continue;
    }
    if (cny_is_type_kw(t->s, t->n) && !cny_word_eq(t->s, t->n, "const") &&
        !cny_word_eq(t->s, t->n, "volatile") &&
        !cny_word_eq(t->s, t->n, "register") &&
        !cny_word_eq(t->s, t->n, "static") &&
        !cny_word_eq(t->s, t->n, "extern") &&
        !cny_word_eq(t->s, t->n, "inline")) {
      cny_sb_add(&spec, cny_scalar_type(t->s, t->n));
      cny_sb_add(&spec, " ");
    }
    cny_advance(p);
  }
  cny_sb_t name = {0};
  cny_parse_name(p, &name, &is_ptr);

  /* array suffix? */
  int array_n = -1;
  if (cny_at(p, "[")) {
    cny_advance(p);
    size_t start = p->ti;
    while (!cny_at(p, "]") && cny_cur(p)->kind != CNTK_EOF)
      cny_advance(p);
    size_t end = p->ti;
    int ok = 0;
    long long v = 0;
    if (end > start)
      v = cny_const_eval_range(p, start, end, &ok);
    if (ok && v > 0 && v < 1000000)
      array_n = (int)v;
    if (cny_at(p, "]"))
      cny_advance(p);
  }

  /* optional initializer */
  cny_sb_t init = {0};
  int has_init = 0;
  int init_is_list = 0;
  if (cny_at(p, "=")) {
    cny_advance(p);
    has_init = 1;
    if (cny_at(p, "{")) {
      /* aggregate initializer { ... } */
      init_is_list = 1;
      cny_advance(p);
      for (;;) {
        if (cny_at(p, "}")) {
          cny_advance(p);
          break;
        }
        if (init.len)
          cny_sb_add(&init, ", ");
        int level = 0;
        cny_sb_t one = {0};
        cny_expr_node(p, &one, &level, CNP_ASSIGN, 1);
        cny_sb_add(&init, one.data ? one.data : "");
        free(one.data);
        if (cny_at(p, ",")) {
          cny_advance(p);
          continue;
        }
        if (cny_at(p, "}")) {
          cny_advance(p);
          break;
        }
        break;
      }
    } else {
      int level = 0;
      cny_sb_t one = {0};
      cny_expr_node(p, &one, &level, CNP_ASSIGN, 1);
      cny_sb_add(&init, one.data ? one.data : "");
      free(one.data);
    }
  }

  if (struct_ty || is_ptr) {
    cny_warn_at(p, line, "unsupported",
                is_ptr ? "pointer declaration" : "struct variable declaration");
    if (name.len > 0) {
      cny_emit(p, "mut ");
      cny_emit(p, name.data);
      cny_emit(p, " = ");
      cny_emit(p, has_init && init.len ? init.data : "0");
      cny_flush(p);
    }
  } else if (array_n >= 0) {
    if (has_init && init_is_list) {
      cny_emit(p, "mut ");
      cny_emit(p, name.data);
      cny_emit(p, " = [");
      cny_emit(p, init.data);
      cny_emit(p, "]");
      cny_flush(p);
      if (array_n > 0) {
        /* pad with zeros to array_n */
        size_t cnt = 1;
        for (size_t k = 0; init.data && init.data[k]; k++)
          if (init.data[k] == ',')
            cnt++;
        for (size_t k = cnt; k < (size_t)array_n; k++) {
          cny_emit(p, " ");
          cny_emit(p, name.data);
          cny_emit(p, " = ");
          cny_emit(p, name.data);
          cny_emit(p, ".extend([0])");
          cny_flush(p);
        }
      }
    } else {
      cny_emit_list_fill(p, name.data, (size_t)array_n);
    }
  } else {
    cny_emit(p, "mut ");
    cny_emit(p, name.data);
    cny_emit(p, " = ");
    if (has_init && init.len)
      cny_emit(p, init.data);
    else
      cny_emit(p, "0");
    cny_flush(p);
  }
  /* multiple declarators `int a, b;` */
  if (cny_at(p, ",")) {
    cny_warn_at(p, line, "unsupported", "multiple declarators in one "
                "declaration; only the first is converted");
    while (!cny_at(p, ";") && cny_cur(p)->kind != CNTK_EOF)
      cny_advance(p);
  }
  while (cny_at(p, ";"))
    cny_advance(p);
  free(spec.data);
  free(name.data);
  free(init.data);
}

/* ------------------------------------------------------------------ */
/* top level: typedef, struct, enum, functions, globals, directives    */
/* ------------------------------------------------------------------ */

static void cny_skip_to_semi(cny_t *p) {
  int depth = 0;
  while (cny_cur(p)->kind != CNTK_EOF) {
    const cny_tok_t *t = cny_cur(p);
    if (t->kind == CNTK_PUNCT && t->n == 1) {
      if (*t->s == '{') {
        depth++;
        cny_advance(p);
        continue;
      }
      if (*t->s == '}') {
        if (depth == 0)
          return;
        depth--;
        cny_advance(p);
        continue;
      }
      if (*t->s == ';' && depth == 0) {
        cny_advance(p);
        return;
      }
    }
    cny_advance(p);
  }
}

/*
 * Parse struct fields starting after '{'; fills `fields` with
 * "TYPE name, TYPE name, ...". Consumes through '}'.
 */
static void cny_parse_struct_fields(cny_t *p, cny_sb_t *fields, int line) {
  int field_err = 0;
  while (!cny_at(p, "}") && cny_cur(p)->kind != CNTK_EOF) {
    if (cny_at(p, ";")) {
      cny_advance(p);
      continue;
    }
    if (cny_at(p, ":")) {
      /* bitfield */
      if (!field_err) {
        field_err = 1;
        cny_warn_at(p, line, "unsupported", "bitfields");
      }
      cny_advance(p);
      continue;
    }
    if (cny_at(p, ",")) {
      cny_advance(p);
      continue;
    }
    if (cny_cur(p)->kind == CNTK_PUNCT && cny_cur(p)->n == 1 &&
        (*cny_cur(p)->s == ':')) {
      continue;
    }
    /* field: type name */
    cny_sb_t ftype = {0};
    int is_ptr_field = 0;
    while (cny_cur(p)->kind == CNTK_IDENT && cny_type_token(p)) {
      const cny_tok_t *t = cny_cur(p);
      if (cny_is_type_kw(t->s, t->n) && !cny_word_eq(t->s, t->n, "const") &&
          !cny_word_eq(t->s, t->n, "volatile")) {
        const char *lt = cny_layout_type(t->s, t->n);
        if (lt)
          cny_sb_add(&ftype, lt);
        else if (cny_word_eq(t->s, t->n, "enum"))
          cny_sb_add(&ftype, "i32");
      } else if (cny_is_known_type(t->s, t->n)) {
        const char *lt = cny_layout_type(t->s, t->n);
        cny_sb_add(&ftype, lt ? lt : "any");
      } else if (cny_word_eq(t->s, t->n, "const") ||
                 cny_word_eq(t->s, t->n, "volatile")) {
        /* skip */
      }
      cny_advance(p);
    }
    while (cny_cur(p)->kind == CNTK_PUNCT && cny_cur(p)->n == 1 &&
           *cny_cur(p)->s == '*') {
      is_ptr_field = 1;
      cny_advance(p);
    }
    if (cny_cur(p)->kind == CNTK_IDENT) {
      const cny_tok_t *t = cny_cur(p);
      if (fields->len)
        cny_sb_add(fields, ", ");
      if (is_ptr_field)
        cny_sb_add(fields, "ptr ");
      else if (ftype.len)
        cny_sb_add(fields, ftype.data);
      else
        cny_sb_add(fields, "any");
      cny_sb_add(fields, " ");
      cny_sb_addn(fields, t->s, t->n);
      cny_advance(p);
      if (cny_at(p, "[")) {
        cny_warn_at(p, line, "unsupported", "array field in struct");
        while (!cny_at(p, "]") && cny_cur(p)->kind != CNTK_EOF)
          cny_advance(p);
        if (cny_at(p, "]"))
          cny_advance(p);
      }
    } else {
      if (!field_err) {
        field_err = 1;
        cny_warn_at(p, line, "unsupported", "complex struct field");
      }
      cny_advance(p);
    }
    free(ftype.data);
  }
  if (cny_at(p, "}"))
    cny_advance(p);
}

static void cny_emit_struct_def(cny_t *p, const char *name,
                                const cny_sb_t *fields, int line) {
  (void)line;
  cny_emit(p, "struct ");
  cny_emit(p, name);
  cny_emit(p, " { ");
  cny_emit(p, fields->len ? fields->data : "int __pad");
  cny_emit(p, " }");
  cny_flush(p);
}

static void cny_parse_struct(cny_t *p) {
  int line = cny_cur(p)->line;
  cny_advance(p); /* struct */
  cny_sb_t tag = {0};
  int have_tag = 0;
  if (cny_cur(p)->kind == CNTK_IDENT) {
    cny_sb_addn(&tag, cny_cur(p)->s, cny_cur(p)->n);
    have_tag = 1;
    cny_advance(p);
  }
  if (!cny_at(p, "{")) {
    /* forward declaration or usage */
    cny_warn_at(p, line, "unsupported", "incomplete struct");
    if (cny_at(p, ";"))
      cny_advance(p);
    free(tag.data);
    return;
  }
  cny_advance(p);
  /* collect field types and names until '}' */
  cny_sb_t fields = {0};
  cny_parse_struct_fields(p, &fields, line);
  if (cny_at(p, ";"))
    cny_advance(p);

  if (have_tag) {
    cny_emit_struct_def(p, tag.data, &fields, line);
  } else {
    cny_warn_at(p, line, "unsupported", "anonymous struct (no tag)");
  }
  free(tag.data);
  free(fields.data);
}

static void cny_parse_enum(cny_t *p) {
  int line = cny_cur(p)->line;
  cny_advance(p); /* enum */
  if (cny_cur(p)->kind == CNTK_IDENT)
    cny_advance(p); /* tag */
  if (cny_at(p, "{"))
    cny_advance(p);
  long long next = 0;
  int had_any = 0;
  while (!cny_at(p, "}") && cny_cur(p)->kind != CNTK_EOF) {
    if (cny_cur(p)->kind == CNTK_IDENT) {
      const cny_tok_t *t = cny_cur(p);
      cny_sb_t nm = {0};
      cny_sb_addn(&nm, t->s, t->n);
      cny_advance(p);
      cny_sb_t val = {0};
      char buf[32];
      if (cny_at(p, "=")) {
        cny_advance(p);
        size_t start = p->ti;
        while (!cny_at(p, ",") && !cny_at(p, "}") &&
               cny_cur(p)->kind != CNTK_EOF)
          cny_advance(p);
        size_t end = p->ti;
        int ok = 0;
        long long v = cny_const_eval_range(p, start, end, &ok);
        if (ok)
          next = v;
        else
          cny_warn_at(p, line, "unsupported", "enum value is not constant");
      }
      snprintf(buf, sizeof(buf), "%lld", next);
      cny_sb_add(&val, buf);
      next++;
      had_any = 1;
      cny_emit(p, "def ");
      cny_emit(p, nm.data);
      cny_emit(p, " = ");
      cny_emit(p, val.data);
      cny_flush(p);
      free(nm.data);
      free(val.data);
    } else {
      cny_advance(p);
    }
  }
  if (cny_at(p, "}"))
    cny_advance(p);
  while (cny_at(p, ";"))
    cny_advance(p);
  if (!had_any)
    cny_warn_at(p, line, "unsupported", "empty enum");
}

/* parse declaration specifiers into `spec` (mapped scalar type), return 1 if any */
static int cny_parse_decl_specs(cny_t *p, cny_sb_t *spec, int *is_struct,
                                int *is_void, int *is_ptr) {
  *is_struct = 0;
  *is_void = 0;
  *is_ptr = 0;
  int any = 0;
  while (cny_cur(p)->kind == CNTK_IDENT && cny_type_token(p)) {
    const cny_tok_t *t = cny_cur(p);
    if (cny_word_eq(t->s, t->n, "struct") ||
        cny_word_eq(t->s, t->n, "union") ||
        cny_word_eq(t->s, t->n, "enum")) {
      *is_struct = 1;
      any = 1;
      cny_advance(p);
      if (cny_cur(p)->kind == CNTK_IDENT)
        cny_advance(p);
      continue;
    }
    if (cny_word_eq(t->s, t->n, "void")) {
      *is_void = 1;
      any = 1;
      cny_sb_add(spec, "any");
      cny_sb_add(spec, " ");
      cny_advance(p);
      continue;
    }
    if (cny_is_known_type(t->s, t->n)) {
      cny_sb_add(spec, cny_scalar_type(t->s, t->n));
      cny_sb_add(spec, " ");
      any = 1;
      cny_advance(p);
      continue;
    }
    {
      const char *td = cny_td_get_n(p, t->s, t->n);
      if (td) {
        cny_sb_add(spec, td);
        cny_sb_add(spec, " ");
        any = 1;
        cny_advance(p);
        continue;
      }
    }
    if (!cny_word_eq(t->s, t->n, "const") &&
        !cny_word_eq(t->s, t->n, "volatile") &&
        !cny_word_eq(t->s, t->n, "register") &&
        !cny_word_eq(t->s, t->n, "static") &&
        !cny_word_eq(t->s, t->n, "extern") &&
        !cny_word_eq(t->s, t->n, "inline") &&
        !cny_word_eq(t->s, t->n, "restrict") &&
        !cny_word_eq(t->s, t->n, "_Atomic")) {
      cny_sb_add(spec, cny_scalar_type(t->s, t->n));
      cny_sb_add(spec, " ");
      any = 1;
    }
    cny_advance(p);
  }
  return any;
}

static void cny_parse_function(cny_t *p, const cny_sb_t *spec, int is_void) {
  const cny_tok_t *name_t = cny_cur(p);
  int line = name_t->line;
  cny_sb_t name = {0};
  int fnptr = 0;
  cny_parse_name(p, &name, &fnptr);
  (void)fnptr;
  if (name.len == 0) {
    cny_warn_at(p, line, "unsupported", "could not parse function name");
    cny_skip_to_semi(p);
    return;
  }
  int is_main = cny_word_eq(name_t->s, name_t->n, "main");
  cny_sb_t params = {0};
  cny_sb_t rtype = {0};
  /* params */
  if (cny_at(p, "(")) {
    cny_advance(p);
    if (cny_at(p, ")")) {
      cny_advance(p);
    } else if (cny_at_word(p, "void") && cny_peek(p, 1)->kind == CNTK_PUNCT &&
               cny_peek(p, 1)->n == 1 && *cny_peek(p, 1)->s == ')') {
      cny_advance(p);
      cny_advance(p);
    } else {
      for (;;) {
        if (cny_at(p, "...")) {
          cny_warn_at(p, cny_cur(p)->line, "unsupported", "variadic parameters");
          cny_advance(p);
          break;
        }
        cny_sb_t pspec = {0};
        int pstruct = 0, pvoid = 0, pptr = 0;
        cny_parse_decl_specs(p, &pspec, &pstruct, &pvoid, &pptr);
        cny_sb_t pname = {0};
        int pname_ok = cny_parse_name(p, &pname, &pptr);
        if (params.len)
          cny_sb_add(&params, ", ");
        if (pvoid) {
          /* lone void param */
        } else if (pname_ok && pname.len) {
          cny_sb_add(&params, pptr ? "any " : (pspec.len ? pspec.data : "any "));
          cny_sb_add(&params, pname.data);
        } else {
          cny_sb_add(&params, "any");
          cny_warn_at(p, line, "unsupported", "unnamed parameter");
        }
        /* array/param suffix [N] */
        if (cny_at(p, "[")) {
          cny_advance(p);
          while (!cny_at(p, "]") && cny_cur(p)->kind != CNTK_EOF)
            cny_advance(p);
          if (cny_at(p, "]"))
            cny_advance(p);
        }
        if (cny_at(p, ",")) {
          cny_advance(p);
          continue;
        }
        if (cny_at(p, ")")) {
          cny_advance(p);
          break;
        }
        if (cny_cur(p)->kind == CNTK_EOF)
          break;
        cny_advance(p);
      }
    }
  }
  if (cny_at(p, "{")) {
    /* function definition */
    if (is_main) {
      cny_emit(p, "#main {");
      cny_flush(p);
      p->in_main = 1;
      if (params.len > 0 && !(p->notes & CNY_NOTE_MAIN_ARGS)) {
        p->notes |= CNY_NOTE_MAIN_ARGS;
        cny_warn_at(p, line, "note",
                    "main parameters (argc/argv) are not exposed in Ny #main");
      }
      p->indent++;
      cny_parse_body(p);
      p->indent--;
      cny_emit(p, "}");
      cny_flush(p);
      p->in_main = 0;
    } else {
      cny_emit(p, "fn ");
      cny_emit(p, name.data);
      cny_emit(p, "(");
      cny_emit(p, params.len ? params.data : "");
      cny_emit(p, ") ");
      if (is_void)
        cny_emit(p, "any");
      else if (spec->len)
        cny_emit(p, spec->data);
      else
        cny_emit(p, "any");
      cny_emit(p, "{");
      cny_flush(p);
      p->in_func = 1;
      p->indent++;
      cny_parse_body(p);
      p->indent--;
      cny_emit(p, "}");
      cny_flush(p);
      p->in_func = 0;
    }
  } else {
    cny_warn_at(p, line, "unsupported", "function prototype for %s",
                name.data);
    cny_skip_to_semi(p);
  }
  free(params.data);
  free(rtype.data);
  free(name.data);
}

static void cny_parse_typedef(cny_t *p) {
  int line = cny_cur(p)->line;
  cny_advance(p); /* typedef */
  cny_sb_t spec = {0};
  int is_struct = 0, is_void = 0, is_ptr = 0;

  if (cny_at_word(p, "struct")) {
    /* typedef struct [Tag] { fields } Name; or typedef struct Tag Name; */
    cny_advance(p);
    cny_sb_t tag = {0};
    int have_tag = 0;
    if (cny_cur(p)->kind == CNTK_IDENT) {
      cny_sb_addn(&tag, cny_cur(p)->s, cny_cur(p)->n);
      have_tag = 1;
      cny_advance(p);
    }
    cny_sb_t fields = {0};
    if (cny_at(p, "{")) {
      cny_advance(p);
      cny_parse_struct_fields(p, &fields, line);
    }
    cny_sb_t tdname = {0};
    while (cny_cur(p)->kind == CNTK_PUNCT && cny_cur(p)->n == 1 &&
           *cny_cur(p)->s == '*')
      cny_advance(p);
    if (cny_cur(p)->kind == CNTK_IDENT) {
      cny_sb_addn(&tdname, cny_cur(p)->s, cny_cur(p)->n);
      cny_advance(p);
    }
    if (cny_at(p, ";"))
      cny_advance(p);
    const char *defname = tdname.len ? tdname.data : tag.data;
    if (defname[0]) {
      if (fields.len) {
        cny_emit_struct_def(p, defname, &fields, line);
      } else if (have_tag) {
        cny_emit(p, "struct ");
        cny_emit(p, tag.data);
        cny_emit(p, " { int __pad }");
        cny_flush(p);
      }
      cny_td_set(p, defname, "any");
    }
    free(tag.data);
    free(fields.data);
    free(tdname.data);
    free(spec.data);
    return;
  }

  if (cny_at_word(p, "enum")) {
    /* typedef enum { ... } Name; -> defs then Name is an int alias */
    cny_parse_enum(p);
    cny_sb_t tdname = {0};
    if (cny_cur(p)->kind == CNTK_IDENT) {
      cny_sb_addn(&tdname, cny_cur(p)->s, cny_cur(p)->n);
      cny_advance(p);
    }
    if (cny_at(p, ";"))
      cny_advance(p);
    if (tdname.len)
      cny_td_set(p, tdname.data, "int ");
    free(tdname.data);
    free(spec.data);
    return;
  }

  /* typedef <scalar type> Name; */
  if (cny_parse_decl_specs(p, &spec, &is_struct, &is_void, &is_ptr)) {
    cny_sb_t tdname = {0};
    int tptr = 0;
    cny_parse_name(p, &tdname, &tptr);
    if (cny_at(p, ";"))
      cny_advance(p);
    if (tdname.len) {
      const char *sc = spec.len ? spec.data : "any";
      cny_td_set(p, tdname.data, sc);
      cny_warn_at(p, line, "note", "typedef %s is an alias only", tdname.data);
      free(spec.data);
      free(tdname.data);
      return;
    }
    free(tdname.data);
  }
  free(spec.data);
  cny_warn_at(p, line, "unsupported", "typedef");
  cny_skip_to_semi(p);
}

static void cny_handle_directive(cny_t *p) {
  const cny_tok_t *t = cny_cur(p);
  int line = t->line;
  const char *s = t->s + 1; /* skip '#' */
  size_t n = t->n - 1;
  while (n > 0 && (*s == ' ' || *s == '\t')) {
    s++;
    n--;
  }
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t' ||
                   s[n - 1] == '\r'))
    n--;

  if (n >= 6 && memcmp(s, "endif", 5) == 0) {
    if (p->nif > 0) {
      p->nif--;
    }
    cny_advance(p);
    return;
  }
  if (n >= 6 && memcmp(s, "else", 4) == 0) {
    if (p->nif > 0 && !p->if_taken[p->nif - 1] &&
        !p->if_ever[p->nif - 1]) {
      p->if_taken[p->nif - 1] = p->if_parent[p->nif - 1];
      p->if_ever[p->nif - 1] = 1;
    } else {
      p->if_taken[p->nif - 1] = 0;
    }
    cny_advance(p);
    return;
  }
  if (n >= 7 && memcmp(s, "elif", 4) == 0) {
    if (p->nif > 0) {
      int cond = 0;
      if (!p->if_ever[p->nif - 1]) {
        size_t estart = s + 4 - t->s;
        size_t eoff = 0;
        (void)estart;
        /* tokenize the condition after 'elif' */
        cny_toks_t c = {0};
        const char *cs = s + 4;
        size_t cn = n - 4;
        /* strip leading space */
        while (cn > 0 && (*cs == ' ' || *cs == '\t')) {
          cs++;
          cn--;
        }
        cny_lex(cs, cn, &c);
        int ok = 0;
        cny_cexpr_t e;
        e.t = c.items;
        e.n = c.len;
        e.i = 0;
        e.p = p;
        e.ok = 1;
        long long v = cny_ce_tern(&e);
        ok = e.ok && e.i == e.n;
        cond = ok ? (v != 0) : 1;
        if (!ok)
          cny_warn_at(p, line, "unsupported",
                      "#elif condition not evaluable; taking this branch");
        cny_toks_free(&c);
        (void)eoff;
      }
      if (!p->if_ever[p->nif - 1] && p->if_parent[p->nif - 1] && cond) {
        p->if_taken[p->nif - 1] = 1;
        p->if_ever[p->nif - 1] = 1;
      } else {
        p->if_taken[p->nif - 1] = 0;
      }
    }
    cny_advance(p);
    return;
  }
  if ((n >= 4 && memcmp(s, "if", 2) == 0) &&
      (n == 2 || s[2] == ' ' || s[2] == '\t' || s[2] == '(' || s[2] == 'd' ||
       s[2] == 'n')) {
    int is_ifdef = 0, is_ifndef = 0;
    if (n >= 6 && memcmp(s, "ifdef", 5) == 0)
      is_ifdef = 1;
    else if (n >= 7 && memcmp(s, "ifndef", 6) == 0)
      is_ifndef = 1;

    int parent_active = cny_active(p);
    int cond = 0;
    int ok = 1;
    if (is_ifdef || is_ifndef) {
      size_t k = is_ifdef ? 5 : 6;
      while (k < n && (s[k] == ' ' || s[k] == '\t'))
        k++;
      char name[256];
      size_t nl = 0;
      while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '_') &&
             nl < sizeof(name) - 1)
        name[nl++] = s[k++];
      name[nl] = 0;
      cond = cny_mac_get(p, name) != NULL;
      if (is_ifndef)
        cond = !cond;
    } else {
      const char *cs = s + 2;
      size_t cn = n - 2;
      if (cn > 0 && *cs == '(') {
        cs++;
        cn--;
        if (cn > 0 && cs[cn - 1] == ')')
          cn--;
      }
      cny_toks_t c = {0};
      cny_lex(cs, cn, &c);
      cny_cexpr_t e;
      e.t = c.items;
      e.n = c.len;
      e.i = 0;
      e.p = p;
      e.ok = 1;
      long long v = cny_ce_tern(&e);
      ok = e.ok && e.i == e.n;
      cond = ok ? (v != 0) : 1;
      if (!ok)
        cny_warn_at(p, line, "unsupported",
                    "#if condition not evaluable; taking this branch");
      cny_toks_free(&c);
    }
    /* push frame */
    if (p->nif == p->cif) {
      size_t cap = p->cif ? p->cif * 2 : 8;
      char *ta = (char *)realloc(p->if_taken, cap);
      char *ev = (char *)realloc(p->if_ever, cap);
      char *pa = (char *)realloc(p->if_parent, cap);
      if (!ta || !ev || !pa) {
        free(ta);
        free(ev);
        free(pa);
      } else {
        p->if_taken = ta;
        p->if_ever = ev;
        p->if_parent = pa;
        p->cif = cap;
      }
    }
    if (p->nif < p->cif) {
      p->if_parent[p->nif] = parent_active ? 1 : 0;
      p->if_taken[p->nif] = parent_active && cond ? 1 : 0;
      p->if_ever[p->nif] = p->if_taken[p->nif];
      p->nif++;
    }
    cny_advance(p);
    return;
  }
  if (n >= 8 && memcmp(s, "include", 7) == 0) {
    cny_emit(p, "#include");
    cny_emit(p, s + 7);
    cny_flush(p);
    cny_advance(p);
    return;
  }
  if (n >= 7 && memcmp(s, "define", 6) == 0) {
    /* #define NAME [value] or #define NAME(args) value */
    const char *ds = s + 6;
    size_t dn = n - 6;
    while (dn > 0 && (*ds == ' ' || *ds == '\t')) {
      ds++;
      dn--;
    }
    size_t k = 0;
    while (k < dn && (isalnum((unsigned char)ds[k]) || ds[k] == '_'))
      k++;
    if (k == 0) {
      cny_advance(p);
      return;
    }
    char name[256];
    size_t nl = k < sizeof(name) - 1 ? k : sizeof(name) - 1;
    memcpy(name, ds, nl);
    name[nl] = 0;
    size_t vstart = k;
    while (vstart < dn && (ds[vstart] == ' ' || ds[vstart] == '\t'))
      vstart++;
    if (vstart < dn && ds[vstart] == '(') {
      cny_warn_at(p, line, "unsupported", "function-like #define %s", name);
      cny_advance(p);
      return;
    }
    const char *val = ds + vstart;
    size_t vlen = dn - vstart;
    /* strip trailing comments */
    for (size_t q = 0; q + 1 < vlen; q++) {
      if (val[q] == '/' && val[q + 1] == '/') {
        vlen = q;
        break;
      }
      if (val[q] == '/' && val[q + 1] == '*') {
        vlen = q;
        break;
      }
    }
    while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
      vlen--;
    if (vlen == 0) {
      cny_mac_set(p, name, "1");
      cny_emit(p, "def ");
      cny_emit(p, name);
      cny_emit(p, " = 1");
      cny_flush(p);
    } else {
      char valbuf[512];
      if (vlen >= sizeof(valbuf))
        vlen = sizeof(valbuf) - 1;
      memcpy(valbuf, val, vlen);
      valbuf[vlen] = 0;
      cny_mac_set(p, name, valbuf);
      /* try to evaluate the value as a constant */
      cny_toks_t c = {0};
      cny_lex(valbuf, vlen, &c);
      cny_cexpr_t e;
      e.t = c.items;
      e.n = c.len > 0 ? c.len - 1 : 0; /* exclude trailing EOF token */
      e.i = 0;
      e.p = p;
      e.ok = 1;
      long long vv = cny_ce_tern(&e);
      int ok = e.ok && e.i == e.n;
      cny_toks_free(&c);
      if (ok) {
        char buf2[32];
        snprintf(buf2, sizeof(buf2), "%lld", vv);
        cny_emit(p, "def ");
        cny_emit(p, name);
        cny_emit(p, " = ");
        cny_emit(p, buf2);
        cny_flush(p);
      } else {
        cny_warn_at(p, line, "unsupported",
                    "#define %s value is not a constant expression", name);
      }
    }
    cny_advance(p);
    return;
  }
  if (n >= 6 && memcmp(s, "undef", 5) == 0) {
    cny_warn_at(p, line, "note", "#undef is not representable; the def stays");
    cny_advance(p);
    return;
  }
  cny_warn_at(p, line, "unsupported", "directive '#%.*s'", (int)(n), s);
  cny_advance(p);
}

static void cny_parse_top(cny_t *p) {
  while (cny_cur(p)->kind != CNTK_EOF) {
    const cny_tok_t *t = cny_cur(p);
    if (t->kind == CNTK_DIRECTIVE) {
      cny_handle_directive(p);
      continue;
    }
    if (!cny_active(p)) {
      cny_advance(p);
      continue;
    }
    if (t->kind == CNTK_PUNCT && t->n == 1 && *t->s == ';') {
      cny_advance(p);
      continue;
    }
    if (t->kind == CNTK_IDENT) {
      if (cny_word_eq(t->s, t->n, "typedef")) {
        cny_parse_typedef(p);
        continue;
      }
      if (cny_word_eq(t->s, t->n, "struct")) {
        cny_parse_struct(p);
        continue;
      }
      if (cny_word_eq(t->s, t->n, "enum")) {
        cny_parse_enum(p);
        continue;
      }
      if (cny_word_eq(t->s, t->n, "union")) {
        int line = t->line;
        cny_warn_at(p, line, "unsupported", "union");
        cny_advance(p);
        if (cny_cur(p)->kind == CNTK_IDENT)
          cny_advance(p);
        cny_skip_to_semi(p);
        continue;
      }
      if (cny_type_token(p)) {
        /* function or variable declaration */
        cny_sb_t spec = {0};
        int is_struct = 0, is_void = 0, is_ptr = 0;
        cny_parse_decl_specs(p, &spec, &is_struct, &is_void, &is_ptr);
        /* peek: after optional stars, expect IDENT then '(' or ';'/=/[ */
        size_t save = p->ti;
        int pptr = 0;
        cny_sb_t nm = {0};
        int has_name = cny_parse_name(p, &nm, &pptr);
        (void)pptr;
        if (has_name && cny_at(p, "(")) {
          p->ti = save;
          cny_parse_function(p, &spec, is_void);
        } else if (has_name) {
          /* global variable */
          p->ti = save;
          cny_sb_t gname = {0};
          cny_sb_t ginit = {0};
          int gis_ptr = 0;
          cny_parse_name(p, &gname, &gis_ptr);
          int extern_ = 0;
          /* check extern in spec was consumed already; re-scan */
          (void)extern_;
          if (cny_at(p, "=")) {
            cny_advance(p);
            if (cny_at(p, "{")) {
              cny_sb_t init = {0};
              cny_advance(p);
              for (;;) {
                if (cny_at(p, "}")) {
                  cny_advance(p);
                  break;
                }
                if (init.len)
                  cny_sb_add(&init, ", ");
                int lvl = 0;
                cny_sb_t one = {0};
                cny_expr_node(p, &one, &lvl, CNP_ASSIGN, 1);
                cny_sb_add(&init, one.data ? one.data : "");
                free(one.data);
                if (cny_at(p, ",")) {
                  cny_advance(p);
                  continue;
                }
                if (cny_at(p, "}")) {
                  cny_advance(p);
                  break;
                }
                break;
              }
              if (gname.len) {
                cny_emit(p, "mut ");
                cny_emit(p, gname.data);
                cny_emit(p, " = [");
                cny_emit(p, init.data);
                cny_emit(p, "]");
                cny_flush(p);
              }
              free(init.data);
            } else {
              int lvl = 0;
              cny_sb_t one = {0};
              cny_expr_node(p, &one, &lvl, CNP_ASSIGN, 1);
              cny_sb_add(&ginit, one.data ? one.data : "");
              free(one.data);
            }
          }
          if (gname.len && ginit.len == 0 && !(cny_at(p, "="))) {
            /* no initializer -> mut name = 0 */
          }
          if (gname.len) {
            cny_emit(p, "mut ");
            cny_emit(p, gname.data);
            cny_emit(p, " = ");
            cny_emit(p, ginit.len ? ginit.data : "0");
            cny_flush(p);
          }
          free(gname.data);
          free(ginit.data);
        } else {
          cny_warn_at(p, t->line, "unsupported",
                      "unrecognized top-level declaration");
          cny_skip_to_semi(p);
        }
        free(nm.data);
        free(spec.data);
        continue;
      }
    }
    cny_warn_at(p, t->line, "unsupported", "unrecognized top-level token");
    cny_advance(p);
    cny_skip_to_semi(p);
  }
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

static int cny_convert(const char *src, size_t n, cny_sb_t *out) {
  cny_toks_t toks = {0};
  cny_lex(src, n, &toks);

  cny_t p;
  memset(&p, 0, sizeof(p));
  p.toks = toks.items;
  p.nt = toks.len;
  p.out = out;

  cny_parse_top(&p);

  cny_toks_free(&toks);
  free(p.mac_name);
  free(p.mac_val);
  free(p.td_name);
  free(p.td_typ);
  free(p.if_taken);
  free(p.if_ever);
  free(p.if_parent);
  free(p.ln.data);
  free(p.warn.data);
  return p.errors == 0 ? 0 : 1;
}

/* focused round-trip regression: catches literal leaks, brace doubling,
 * missing statement newlines, broken const evaluation, and case fall-through */
static int c2ny_selftest(void) {
  static const char probe[] =
      "#define BASE 10\n"
      "int fact(int n) {\n"
      "  int r = 1;\n"
      "  for (int i = 1; i <= n; i += 1) {\n"
      "    r = r * i;\n"
      "  }\n"
      "  return r;\n"
      "}\n"
      "enum Color { RED, GREEN = 5, BLUE };\n"
      "int classify(int x) {\n"
      "  int c = 0;\n"
      "  switch (x) {\n"
      "    case 1:\n"
      "      c = 10;\n"
      "      break;\n"
      "    case 2:\n"
      "    case 3:\n"
      "      c = 20;\n"
      "      break;\n"
      "    default:\n"
      "      c = -1;\n"
      "      break;\n"
      "  }\n"
      "  return c;\n"
      "}\n"
      "int sizes(void) {\n"
      "  int a[4];\n"
      "  return a[3] + 2;\n"
      "}\n";
  cny_sb_t out = {0};
  if (cny_convert(probe, sizeof(probe) - 1, &out) != 0) {
    fprintf(stderr, "ny-fmt c2ny selftest: cny_convert failed\n");
    free(out.data);
    return 1;
  }
  const char *o = out.data ? out.data : "";
  static const char *const need[] = {
      "def BASE = 10\n",
      "fn fact(int n) int {\n",
      "for (mut i = 1 i <= n i += 1) {\n",
      "r = r * i\n",
      "def GREEN = 5\n",
      "def BLUE = 6\n",
      "case (x) {\n",
      "1 {\n",
      "2, 3 {\n",
      "_ {\n",
      "mut a = [0, 0, 0, 0]\n",
  };
  int fail = 0;
  for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
    if (!strstr(o, need[i])) {
      fprintf(stderr, "ny-fmt c2ny selftest: missing %s", need[i]);
      fail = 1;
    }
  }
  /* statements must not leak literals into the line prefix or merge lines */
  if (strstr(o, "1mut ") || strstr(o, "}int ") || strstr(o, "}i = ") ||
      strstr(o, "fn fact(int n) int  {")) {
    fprintf(stderr, "ny-fmt c2ny selftest: emission leak detected\n");
    fail = 1;
  }
  free(out.data);
  if (fail)
    return 1;
  printf("ny-fmt selftest: c2ny round-trip checks: ok\n");
  return 0;
}
