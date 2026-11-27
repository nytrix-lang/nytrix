/*
 * NYR text frontend: lexes, parses, type-checks and lowers the Ny-flavoured
 * NYR source language (etc/docs/spec/nyr-text.md) to verified NYIR, writes
 * its data as GNU assembly, and prints NYIR back in the same syntax.
 */
#include <nyr/text.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ---------------------------------------------------------------- lexer
 */

typedef enum {
  TK_EOF,
  TK_IDENT,
  TK_INT,
  TK_FLOAT,
  TK_STRING,
  TK_PUNCT,
} tk_kind_t;

typedef struct {
  tk_kind_t kind;
  const char *start;
  size_t len;
  int line, col;
  bool line_start; /* first token on its line */
  int64_t ival;
  double fval;
  bool f32; /* float literal with an `f` suffix */
  char *sval;
  size_t slen;
} tk_t;

typedef enum { TY_VOID, TY_INT, TY_F64, TY_F32 } ty_t;
/*
 * Extern-only result widths; values are `int` after extension.
 */
typedef enum { RW_NONE, RW_I32, RW_U32, RW_U8, RW_U16 } rw_t;

typedef struct {
  char *name;
  char *symbol;
  ty_t ret;
  rw_t ret_width;
  ty_t *params;
  char **pnames;
  size_t nparams;
  bool variadic;
  bool is_extern;
  bool exported;
  size_t body;  /* token index of `{` */
  size_t index; /* function index in the module */
} fsig_t;

typedef enum { DATA_STRING, DATA_GLOBAL, DATA_CONST, DATA_RAW } data_kind_t;

typedef struct {
  char *name;   /* source name; NULL for an anonymous literal */
  char *symbol; /* assembly symbol */
  data_kind_t kind;
  ty_t ty;
  char *bytes;
  size_t len;
  int64_t ival;
  double fval;
} data_t;

typedef struct {
  char *name;
  char *symbol;
  nyr_func_t func;
} mfunc_t;

struct nyr_text_module {
  mfunc_t *funcs;
  size_t nfuncs;
  data_t *data;
  size_t ndata, cdata;
  char *asm_text; /* top-level `asm` and `data` lines, verbatim GNU as */
  size_t asm_len, asm_cap;
};

typedef struct {
  const char *src;
  size_t len;
  const char *path;
  tk_t *t;
  size_t nt, ct, pos;
  char *err;
  size_t err_len;
  bool failed;
  fsig_t *sigs;
  size_t nsigs, csigs;
  nyr_text_module_t *mod;
  size_t anon;
} px_t;

static void px_error_at(px_t *p, const tk_t *at, const char *fmt, ...) {
  if (p->failed)
    return;
  p->failed = true;
  if (!p->err || !p->err_len)
    return;
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  int line = at ? at->line : 1, col = at ? at->col : 1;
  const char *ls = p->src;
  for (int l = 1; l < line && ls < p->src + p->len; ++ls)
    if (*ls == '\n')
      ++l;
  const char *le = ls;
  while (le < p->src + p->len && *le != '\n')
    ++le;
  int width = (int)(le - ls);
  if (width > 200)
    width = 200;
  snprintf(p->err, p->err_len, "%s:%d:%d: error: %s\n  %.*s\n  %*s^",
           p->path ? p->path : "<nyr>", line, col, msg, width, ls,
           col > 1 ? col - 1 : 0, "");
}

#define FAIL(at, ...)                                                          \
  do {                                                                         \
    px_error_at(p, (at), __VA_ARGS__);                                         \
    return false;                                                              \
  } while (0)

static char *xstrndup(const char *s, size_t n) {
  char *r = malloc(n + 1);
  if (r) {
    memcpy(r, s, n);
    r[n] = 0;
  }
  return r;
}

static bool tk_push(px_t *p, tk_t t) {
  if (p->nt == p->ct) {
    size_t nc = p->ct ? p->ct * 2 : 256;
    tk_t *n = realloc(p->t, nc * sizeof(*n));
    if (!n)
      return false;
    p->t = n;
    p->ct = nc;
  }
  p->t[p->nt++] = t;
  return true;
}

static const char *const PUNCTS[] = {
    "<<=", ">>=", "...", "->", "==", "!=", "<=", ">=", "<<", ">>",
    "&&",  "||",  "+=",  "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "..",  "++",  "--",  "(",  ")",  "[",  "]",  "{",  "}",  ",",
    ":",   "=",   "<",   ">",  "+",  "-",  "*",  "/",  "%",  "&",
    "|",   "^",   "~",   "!",  "?",  ";",  "@",  "$",  "#",  NULL};

static int lex_escape(const char **s, const char *end) {
  char c = **s;
  ++*s;
  switch (c) {
  case 'n':
    return '\n';
  case 't':
    return '\t';
  case 'r':
    return '\r';
  case '0':
    return 0;
  case 'e':
    return 27;
  case '\\':
    return '\\';
  case '\'':
    return '\'';
  case '"':
    return '"';
  case 'x': {
    int v = 0;
    for (int k = 0; k < 2 && *s < end && isxdigit((unsigned char)**s); ++k) {
      char h = **s;
      v = v * 16 + (isdigit((unsigned char)h)
                        ? h - '0'
                        : (tolower((unsigned char)h) - 'a' + 10));
      ++*s;
    }
    return v;
  }
  default:
    return -1;
  }
}

static bool lex(px_t *p) {
  const char *s = p->src, *end = p->src + p->len;
  int line = 1;
  const char *line_begin = s;
  bool at_line_start = true;
  while (s < end) {
    char c = *s;
    if (c == '\n') {
      ++s;
      ++line;
      line_begin = s;
      at_line_start = true;
      continue;
    }
    if (isspace((unsigned char)c)) {
      ++s;
      continue;
    }
    if (c == ';' && s + 1 < end && s[1] == ';') {
      while (s < end && *s != '\n')
        ++s;
      continue;
    }
    if (c == '#' && s == line_begin && s + 1 < end && s[1] == '!') {
      while (s < end && *s != '\n')
        ++s;
      continue;
    }
    tk_t t = {.start = s,
              .line = line,
              .col = (int)(s - line_begin) + 1,
              .line_start = at_line_start};
    at_line_start = false;
    if (isalpha((unsigned char)c) || c == '_') {
      while (s < end && (isalnum((unsigned char)*s) || *s == '_' ||
                         (*s == '.' && s + 1 < end &&
                          (isalnum((unsigned char)s[1]) || s[1] == '_'))))
        ++s;
      t.kind = TK_IDENT;
    } else if (isdigit((unsigned char)c) ||
               (c == '.' && s + 1 < end && isdigit((unsigned char)s[1]))) {
      char buf[128];
      size_t n = 0;
      bool is_float = false;
      int base = 10;
      if (c == '0' && s + 1 < end && (s[1] == 'x' || s[1] == 'X'))
        base = 16, s += 2;
      else if (c == '0' && s + 1 < end && (s[1] == 'b' || s[1] == 'B'))
        base = 2, s += 2;
      else if (c == '0' && s + 1 < end && (s[1] == 'o' || s[1] == 'O'))
        base = 8, s += 2;
      while (s < end && n + 1 < sizeof(buf)) {
        char d = *s;
        if (d == '_') {
          ++s;
          continue;
        }
        if (base == 16 ? isxdigit((unsigned char)d)
                       : isdigit((unsigned char)d)) {
          buf[n++] = d;
          ++s;
        } else if (base == 10 && d == '.' && !(s + 1 < end && s[1] == '.') &&
                   !is_float) {
          is_float = true;
          buf[n++] = d;
          ++s;
        } else if (base == 10 && (d == 'e' || d == 'E') && s + 1 < end &&
                   (isdigit((unsigned char)s[1]) ||
                    ((s[1] == '-' || s[1] == '+') && s + 2 < end &&
                     isdigit((unsigned char)s[2])))) {
          is_float = true;
          buf[n++] = d;
          buf[n++] = s[1];
          s += 2;
        } else
          break;
      }
      buf[n] = 0;
      if (base == 10 && s < end && (*s == 'f' || *s == 'F') &&
          !(s + 1 < end && (isalnum((unsigned char)s[1]) || s[1] == '_'))) {
        is_float = true;
        t.f32 = true;
        ++s;
      }
      if (s < end && (isalnum((unsigned char)*s) || *s == '_')) {
        t.len = (size_t)(s - t.start);
        px_error_at(p, &t, "malformed number");
        return false;
      }
      if (is_float) {
        t.kind = TK_FLOAT;
        t.fval = strtod(buf, NULL);
      } else {
        t.kind = TK_INT;
        errno = 0;
        unsigned long long v = strtoull(buf, NULL, base);
        if (errno || !n) {
          px_error_at(p, &t, "integer literal out of range");
          return false;
        }
        t.ival = (int64_t)v;
      }
    } else if (c == '"') {
      ++s;
      size_t cap = 32, n = 0;
      char *buf = malloc(cap);
      if (!buf)
        return false;
      while (s < end && *s != '"' && *s != '\n') {
        int ch = (unsigned char)*s++;
        if (ch == '\\') {
          if (s >= end)
            break;
          ch = lex_escape(&s, end);
          if (ch < 0) {
            free(buf);
            px_error_at(p, &t, "unknown escape in string literal");
            return false;
          }
        }
        if (n + 1 >= cap) {
          cap *= 2;
          char *nb = realloc(buf, cap);
          if (!nb) {
            free(buf);
            return false;
          }
          buf = nb;
        }
        buf[n++] = (char)ch;
      }
      if (s >= end || *s != '"') {
        free(buf);
        px_error_at(p, &t, "unterminated string literal");
        return false;
      }
      ++s;
      buf[n] = 0;
      t.kind = TK_STRING;
      t.sval = buf;
      t.slen = n;
    } else if (c == '\'') {
      ++s;
      int ch = s < end ? (unsigned char)*s++ : -1;
      if (ch == '\\')
        ch = s < end ? lex_escape(&s, end) : -1;
      if (ch < 0 || s >= end || *s != '\'') {
        px_error_at(p, &t, "malformed character literal");
        return false;
      }
      ++s;
      t.kind = TK_INT;
      t.ival = ch;
    } else {
      size_t k = 0;
      for (; PUNCTS[k]; ++k) {
        size_t pl = strlen(PUNCTS[k]);
        if ((size_t)(end - s) >= pl && memcmp(s, PUNCTS[k], pl) == 0)
          break;
      }
      if (!PUNCTS[k]) {
        px_error_at(p, &t, "unexpected character '%c'", c);
        return false;
      }
      s += strlen(PUNCTS[k]);
      t.kind = TK_PUNCT;
    }
    t.len = (size_t)(s - t.start);
    if (!tk_push(p, t))
      return false;
  }
  tk_t eof = {.kind = TK_EOF,
              .start = end,
              .line = line,
              .col = (int)(end - line_begin) + 1,
              .line_start = true};
  return tk_push(p, eof);
}

static tk_t *peek(px_t *p) { return &p->t[p->pos]; }
static tk_t *peek_at(px_t *p, size_t k) {
  size_t i = p->pos + k;
  return &p->t[i < p->nt ? i : p->nt - 1];
}
static bool is_punct(const tk_t *t, const char *s) {
  return t->kind == TK_PUNCT && t->len == strlen(s) &&
         memcmp(t->start, s, t->len) == 0;
}
static bool is_word(const tk_t *t, const char *s) {
  return t->kind == TK_IDENT && t->len == strlen(s) &&
         memcmp(t->start, s, t->len) == 0;
}
static bool accept(px_t *p, const char *s) {
  if (is_punct(peek(p), s)) {
    ++p->pos;
    return true;
  }
  return false;
}
static bool accept_word(px_t *p, const char *s) {
  if (is_word(peek(p), s)) {
    ++p->pos;
    return true;
  }
  return false;
}
static bool expect(px_t *p, const char *s) {
  if (accept(p, s))
    return true;
  tk_t *t = peek(p);
  FAIL(t, "expected '%s', found '%.*s'", s,
       (int)(t->kind == TK_EOF ? 3 : t->len),
       t->kind == TK_EOF ? "EOF" : t->start);
}

static const char *const KEYWORDS[] = {
    "fn",  "def",   "mut",      "if",     "elif",   "else", "while", "for",
    "in",  "break", "continue", "return", "extern", "pub",  "true",  "false",
    "nil", "goto",  "and",      "or",     "not",    NULL};

static bool is_keyword(const tk_t *t) {
  for (size_t k = 0; KEYWORDS[k]; ++k)
    if (is_word(t, KEYWORDS[k]))
      return true;
  return false;
}

static bool expect_ident(px_t *p, char **out) {
  tk_t *t = peek(p);
  if (t->kind != TK_IDENT || is_keyword(t))
    FAIL(t, "expected a name, found '%.*s'",
         (int)(t->kind == TK_EOF ? 3 : t->len),
         t->kind == TK_EOF ? "EOF" : t->start);
  *out = xstrndup(t->start, t->len);
  ++p->pos;
  return *out != NULL;
}

/*
 * Types: int (i64, ptr, bool, str are its aliases), f64, f32, void.
 * Extern result types may also be i32, u32, u8 or u16.
 */
static bool parse_type_word(const tk_t *t, ty_t *ty, rw_t *rw) {
  static const struct {
    const char *name;
    ty_t ty;
    rw_t rw;
  } TYPES[] = {{"int", TY_INT, RW_NONE},  {"i64", TY_INT, RW_NONE},
               {"ptr", TY_INT, RW_NONE},  {"bool", TY_INT, RW_NONE},
               {"str", TY_INT, RW_NONE},  {"u64", TY_INT, RW_NONE},
               {"i32", TY_INT, RW_I32},   {"u32", TY_INT, RW_U32},
               {"u8", TY_INT, RW_U8},     {"u16", TY_INT, RW_U16},
               {"f64", TY_F64, RW_NONE},  {"f32", TY_F32, RW_NONE},
               {"void", TY_VOID, RW_NONE}};
  for (size_t k = 0; k < sizeof(TYPES) / sizeof(TYPES[0]); ++k)
    if (is_word(t, TYPES[k].name)) {
      *ty = TYPES[k].ty;
      if (rw)
        *rw = TYPES[k].rw;
      return true;
    }
  return false;
}

static const char *ty_name(ty_t t) {
  return t == TY_F64    ? "f64"
         : t == TY_F32  ? "f32"
         : t == TY_VOID ? "void"
                        : "int";
}

/*
 * ---------------------------------------------------------- module items
 */

static fsig_t *find_sig(px_t *p, const char *name, size_t len) {
  for (size_t i = 0; i < p->nsigs; ++i)
    if (strlen(p->sigs[i].name) == len &&
        memcmp(p->sigs[i].name, name, len) == 0)
      return &p->sigs[i];
  return NULL;
}

static data_t *find_data(px_t *p, const char *name, size_t len) {
  for (size_t i = 0; i < p->mod->ndata; ++i) {
    data_t *d = &p->mod->data[i];
    if (d->name && strlen(d->name) == len && memcmp(d->name, name, len) == 0)
      return d;
  }
  return NULL;
}

static data_t *add_data(px_t *p) {
  nyr_text_module_t *m = p->mod;
  if (m->ndata == m->cdata) {
    size_t nc = m->cdata ? m->cdata * 2 : 16;
    data_t *n = realloc(m->data, nc * sizeof(*n));
    if (!n)
      return NULL;
    m->data = n;
    m->cdata = nc;
  }
  data_t *d = &m->data[m->ndata++];
  memset(d, 0, sizeof(*d));
  return d;
}

static bool asm_append(px_t *p, const char *fmt, ...) {
  nyr_text_module_t *m = p->mod;
  va_list ap;
  va_start(ap, fmt);
  char tmp[512];
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0)
    return false;
  char *big = NULL;
  const char *text = tmp;
  if ((size_t)n >= sizeof(tmp)) {
    big = malloc((size_t)n + 1);
    if (!big)
      return false;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    text = big;
  }
  if (m->asm_len + (size_t)n + 1 > m->asm_cap) {
    size_t nc = m->asm_cap ? m->asm_cap * 2 : 1024;
    while (nc < m->asm_len + (size_t)n + 1)
      nc *= 2;
    char *nb = realloc(m->asm_text, nc);
    if (!nb) {
      free(big);
      return false;
    }
    m->asm_text = nb;
    m->asm_cap = nc;
  }
  memcpy(m->asm_text + m->asm_len, text, (size_t)n + 1);
  m->asm_len += (size_t)n;
  free(big);
  return true;
}

/*
 * Address of an extern (or exported) function: a .quad stub in the data
 * section, since NYIR addresses only module symbols directly.
 */
static const char *extern_stub(px_t *p, const char *name) {
  char sym[200];
  snprintf(sym, sizeof(sym), "__nyc_data_nyr_addr_%s", name);
  for (size_t i = 0; i < p->mod->ndata; ++i)
    if (p->mod->data[i].kind == DATA_RAW && !p->mod->data[i].name &&
        !strcmp(p->mod->data[i].symbol, sym))
      return p->mod->data[i].symbol;
  data_t *d = add_data(p);
  if (!d)
    return NULL;
  d->kind = DATA_RAW;
  d->symbol = strdup(sym);
  if (!d->symbol ||
      !asm_append(p,
                  "\t.section .data.rel.ro,\"aw\",@progbits\n\t.p2align 3\n"
                  "\t.globl %s\n\t.hidden %s\n%s:\n\t.quad %s\n",
                  sym, sym, sym, name))
    return NULL;
  return d->symbol;
}

static data_t *string_data(px_t *p, const char *bytes, size_t len,
                           const char *name) {
  if (!name)
    for (size_t i = 0; i < p->mod->ndata; ++i) {
      data_t *d = &p->mod->data[i];
      if (d->kind == DATA_STRING && !d->name && d->len == len &&
          memcmp(d->bytes, bytes, len) == 0)
        return d;
    }
  data_t *d = add_data(p);
  if (!d)
    return NULL;
  d->kind = DATA_STRING;
  d->name = name ? strdup(name) : NULL;
  d->bytes = malloc(len + 1);
  if (!d->bytes)
    return NULL;
  memcpy(d->bytes, bytes, len);
  d->bytes[len] = 0;
  d->len = len;
  char sym[96];
  /*
   * ny_str_ and __nyc_data_ names reach the object unprefixed
   */
  snprintf(sym, sizeof(sym), "ny_str_nyr%zu", p->anon++);
  d->symbol = strdup(sym);
  return d;
}

static bool skip_block(px_t *p) {
  int depth = 0;
  do {
    tk_t *t = peek(p);
    if (t->kind == TK_EOF)
      FAIL(t, "unterminated block: missing '}'");
    if (is_punct(t, "{"))
      ++depth;
    else if (is_punct(t, "}"))
      --depth;
    ++p->pos;
  } while (depth > 0);
  return true;
}

static bool add_sig(px_t *p, fsig_t *s, const tk_t *at) {
  if (find_sig(p, s->name, strlen(s->name)) ||
      find_data(p, s->name, strlen(s->name)))
    FAIL(at, "'%s' is already defined", s->name);
  if (p->nsigs == p->csigs) {
    size_t nc = p->csigs ? p->csigs * 2 : 16;
    fsig_t *n = realloc(p->sigs, nc * sizeof(*n));
    if (!n)
      return false;
    p->sigs = n;
    p->csigs = nc;
  }
  p->sigs[p->nsigs++] = *s;
  return true;
}

/*
 * `fn name(type a, type b, ...) rettype` with Ny's `type name` order; extern
 * parameters may omit their names.
 */
static bool parse_signature(px_t *p, fsig_t *s, bool is_extern) {
  memset(s, 0, sizeof(*s));
  s->is_extern = is_extern;
  tk_t *at = peek(p);
  if (!expect_ident(p, &s->name) || !expect(p, "("))
    return false;
  size_t cap = 0;
  while (!accept(p, ")")) {
    if (s->nparams && !expect(p, ","))
      return false;
    if (accept(p, "...")) {
      if (!is_extern)
        FAIL(peek_at(p, -1 + 0), "only extern functions can be variadic");
      s->variadic = true;
      if (!expect(p, ")"))
        return false;
      break;
    }
    ty_t ty;
    if (!parse_type_word(peek(p), &ty, NULL) || ty == TY_VOID)
      FAIL(peek(p), "expected a parameter type (int, ptr, f64, ...)");
    ++p->pos;
    char *pn = NULL;
    if (peek(p)->kind == TK_IDENT && !is_keyword(peek(p))) {
      if (!expect_ident(p, &pn))
        return false;
    } else if (!is_extern)
      FAIL(peek(p), "expected a parameter name after its type");
    if (s->nparams == cap) {
      cap = cap ? cap * 2 : 4;
      ty_t *np = realloc(s->params, cap * sizeof(*np));
      char **nn = realloc(s->pnames, cap * sizeof(*nn));
      if (!np || !nn)
        return false;
      s->params = np;
      s->pnames = nn;
    }
    s->params[s->nparams] = ty;
    s->pnames[s->nparams++] = pn;
  }
  s->ret = TY_VOID;
  if (parse_type_word(peek(p), &s->ret, &s->ret_width)) {
    ++p->pos;
    if (!is_extern && s->ret_width != RW_NONE)
      FAIL(peek_at(p, 0), "narrow result types are for extern functions");
  }
  if (accept_word(p, "as")) {
    tk_t *t = peek(p);
    if (t->kind != TK_STRING)
      FAIL(t, "expected a symbol string after 'as'");
    s->symbol = strdup(t->sval);
    ++p->pos;
  }
  (void)at;
  return true;
}

/*
 * data NAME [align N] [section "s"] = { TYPE v, v, TYPE v, zero N, "str", ptr
 * sym } Symbol items (`ptr fib`, `ptr other`) resolve after every item is
 * known.
 */
static bool parse_data(px_t *p, bool exported) {
  tk_t *nt = peek(p);
  char *name = NULL;
  if (!expect_ident(p, &name))
    return false;
  if (find_sig(p, name, strlen(name)) || find_data(p, name, strlen(name))) {
    free(name);
    FAIL(nt, "'%.*s' is already defined", (int)nt->len, nt->start);
  }
  int64_t align = 8;
  char *section = NULL;
  for (;;) {
    if (accept_word(p, "align")) {
      tk_t *at = peek(p);
      if (at->kind != TK_INT || at->ival <= 0 || (at->ival & (at->ival - 1))) {
        free(name);
        free(section);
        FAIL(at, "align takes a power of two");
      }
      align = at->ival;
      ++p->pos;
    } else if (accept_word(p, "section")) {
      tk_t *st = peek(p);
      if (st->kind != TK_STRING) {
        free(name);
        free(section);
        FAIL(st, "section takes a string");
      }
      free(section);
      section = strdup(st->sval);
      ++p->pos;
    } else
      break;
  }
  data_t *d = add_data(p);
  bool ok = d != NULL;
  char sym[200];
  snprintf(sym, sizeof(sym), "__nyc_data_nyr_%s", name);
  if (ok) {
    d->kind = DATA_RAW;
    d->name = name;
    d->symbol = strdup(sym);
    name = NULL;
  }
  int lg = 0;
  while ((1ll << lg) < align)
    ++lg;
  ok = ok && asm_append(p,
                        "\t.section %s,\"aw\",@progbits\n\t.p2align %d\n"
                        "\t.globl %s\n\t.hidden %s\n%s:\n",
                        section ? section : ".data", lg, sym, sym, sym);
  if (ok && exported)
    ok = asm_append(p, "\t.globl %s\n\t.set %s, %s\n", d->name, d->name, sym);
  free(section);
  free(name);
  ok = ok && expect(p, "=") && expect(p, "{");
  const char *dir = ".quad";
  bool is_float = false, is_f32 = false;
  bool first = true;
  while (ok && !accept(p, "}")) {
    if (!first)
      (void)accept(p, ","); /* commas between items are optional */
    first = false;
    if (accept(p, "}"))
      break;
    tk_t *t = peek(p);
    ty_t ty;
    rw_t rw;
    if (parse_type_word(t, &ty, &rw) || is_word(t, "i16") || is_word(t, "i8")) {
      ++p->pos;
      is_float = ty == TY_F64 || ty == TY_F32;
      is_f32 = ty == TY_F32;
      dir = is_word(t, "i8") || rw == RW_U8     ? ".byte"
            : is_word(t, "i16") || rw == RW_U16 ? ".short"
            : rw == RW_I32 || rw == RW_U32      ? ".long"
            : is_f32                            ? ".float"
            : ty == TY_F64                      ? ".double"
                                                : ".quad";
      continue;
    }
    if (accept_word(p, "zero")) {
      tk_t *n = peek(p);
      if (n->kind != TK_INT)
        FAIL(n, "zero takes a byte count");
      ++p->pos;
      ok = asm_append(p, "\t.zero %" PRId64 "\n", n->ival);
      continue;
    }
    if (t->kind == TK_STRING) {
      ++p->pos;
      ok = asm_append(p, "\t.byte ");
      for (size_t k = 0; ok && k < t->slen; ++k)
        ok = asm_append(p, "%s%u", k ? "," : "",
                        (unsigned)(unsigned char)t->sval[k]);
      if (ok && !t->slen)
        ok = asm_append(p, "0");
      ok = ok && asm_append(p, "\n");
      continue;
    }
    bool neg = accept(p, "-");
    t = peek(p);
    if (t->kind == TK_INT || t->kind == TK_FLOAT) {
      ++p->pos;
      if (is_float)
        ok = asm_append(p, "\t%s %.17g\n", is_f32 ? ".float" : ".double",
                        (neg ? -1 : 1) *
                            (t->kind == TK_FLOAT ? t->fval : (double)t->ival));
      else if (t->kind == TK_FLOAT)
        FAIL(t, "float item in an integer field");
      else
        ok = asm_append(p, "\t%s %" PRId64 "\n", dir,
                        neg ? (int64_t)(0ull - (uint64_t)t->ival) : t->ival);
      continue;
    }
    if (t->kind == TK_IDENT && !neg) {
      ++p->pos;
      /*
       * resolved by resolve_data_refs once every item is known
       */
      ok = asm_append(p, "\t.quad \x01%.*s\x01\n", (int)t->len, t->start);
      continue;
    }
    FAIL(t, "expected a data item (type, number, string, zero N, or a symbol)");
  }
  return ok;
}

/*
 * Replace \x01name\x01 markers with the object symbol of `name`.
 */
static bool resolve_data_refs(px_t *p) {
  nyr_text_module_t *m = p->mod;
  if (!m->asm_text || !memchr(m->asm_text, 1, m->asm_len))
    return true;
  char *old = m->asm_text;
  size_t old_len = m->asm_len;
  m->asm_text = NULL;
  m->asm_len = m->asm_cap = 0;
  bool ok = true;
  for (size_t i = 0; ok && i < old_len;) {
    char *mk = memchr(old + i, 1, old_len - i);
    if (!mk) {
      ok = asm_append(p, "%.*s", (int)(old_len - i), old + i);
      break;
    }
    ok = asm_append(p, "%.*s", (int)(mk - (old + i)), old + i);
    char *e = memchr(mk + 1, 1, (size_t)(old + old_len - (mk + 1)));
    size_t nl = (size_t)(e - (mk + 1));
    const char *sym = NULL;
    data_t *d = find_data(p, mk + 1, nl);
    fsig_t *s = find_sig(p, mk + 1, nl);
    if (d)
      sym = d->symbol;
    else if (s)
      sym = s->symbol;
    if (!sym) {
      px_error_at(p, NULL, "data refers to unknown symbol '%.*s'", (int)nl,
                  mk + 1);
      ok = false;
    } else
      ok = asm_append(p, "%s", sym);
    i = (size_t)(e + 1 - old);
  }
  free(old);
  return ok;
}

static bool parse_top(px_t *p) {
  while (peek(p)->kind != TK_EOF) {
    tk_t *t = peek(p);
    if (accept(p, ";"))
      continue;
    if (accept_word(p, "use"))
      FAIL(t, "NYR text has no modules; 'use' is not supported");
    if (accept_word(p, "extern")) {
      if (peek(p)->kind == TK_STRING)
        ++p->pos; /* Ny's library path; symbols resolve at link time */
      bool block = accept(p, "{");
      do {
        if (block && accept(p, "}"))
          break;
        tk_t *ft = peek(p);
        if (!accept_word(p, "fn"))
          FAIL(ft, "expected 'fn' in extern declaration");
        fsig_t s;
        if (!parse_signature(p, &s, true) || !add_sig(p, &s, ft))
          return false;
        if (!s.symbol)
          p->sigs[p->nsigs - 1].symbol = strdup(s.name);
      } while (block);
      continue;
    }
    if (accept_word(p, "asm")) {
      bool blockp = accept(p, "{");
      do {
        if (blockp && accept(p, "}"))
          break;
        tk_t *st = peek(p);
        if (st->kind != TK_STRING)
          FAIL(st, "asm takes string lines: asm \"...\" or asm { \"...\" }");
        ++p->pos;
        if (!asm_append(p, "%.*s\n", (int)st->slen, st->sval))
          return false;
      } while (blockp);
      continue;
    }
    bool exported = accept_word(p, "pub");
    if (accept_word(p, "data")) {
      if (!parse_data(p, exported))
        return false;
      continue;
    }
    if (accept_word(p, "fn")) {
      tk_t *ft = peek(p);
      fsig_t s;
      if (!parse_signature(p, &s, false))
        return false;
      s.exported = exported || strcmp(s.name, "main") == 0;
      if (s.symbol)
        FAIL(ft, "'as' names a symbol only on extern functions");
      size_t blen = strlen(s.name) + 8;
      s.symbol = malloc(blen);
      if (!s.symbol)
        return false;
      snprintf(s.symbol, blen, s.exported ? "%s" : "ny_fn_%s", s.name);
      if (!is_punct(peek(p), "{"))
        FAIL(peek(p), "expected '{' to start the body of '%s'", s.name);
      s.body = p->pos;
      if (!skip_block(p) || !add_sig(p, &s, ft))
        return false;
      continue;
    }
    if (exported)
      FAIL(peek(p), "'pub' applies to functions");
    bool is_mut = false;
    if ((is_mut = accept_word(p, "mut")) || accept_word(p, "def")) {
      ty_t ty = TY_VOID;
      if (parse_type_word(peek(p), &ty, NULL))
        ++p->pos;
      tk_t *nt = peek(p);
      char *name = NULL;
      if (!expect_ident(p, &name))
        return false;
      if (find_sig(p, name, strlen(name)) || find_data(p, name, strlen(name))) {
        free(name);
        FAIL(nt, "'%.*s' is already defined", (int)nt->len, nt->start);
      }
      int64_t iv = 0;
      double fv = 0;
      bool neg = false;
      tk_t *vt = NULL;
      if (accept(p, "=")) {
        neg = accept(p, "-");
        vt = peek(p);
        if (!(vt->kind == TK_INT || vt->kind == TK_FLOAT ||
              vt->kind == TK_STRING || is_word(vt, "true") ||
              is_word(vt, "false")) ||
            (neg && vt->kind == TK_STRING)) {
          free(name);
          FAIL(vt, "a top-level initializer must be a literal");
        }
        ++p->pos;
      }
      if (vt && vt->kind == TK_STRING) {
        if (is_mut) {
          free(name);
          FAIL(vt, "string data is read-only; use 'def'");
        }
        data_t *d = string_data(p, vt->sval, vt->slen, name);
        free(name);
        if (!d)
          return false;
        continue;
      }
      ty_t vty = !vt                    ? TY_INT
                 : vt->kind == TK_FLOAT ? (vt->f32 ? TY_F32 : TY_F64)
                                        : TY_INT;
      if (ty == TY_VOID)
        ty = vty;
      if (vt) {
        if (vt->kind == TK_FLOAT) {
          if (ty == TY_INT) {
            free(name);
            FAIL(vt, "float initializer for an int global");
          }
          fv = vt->fval;
        } else if (is_word(vt, "true"))
          iv = 1;
        else if (vt->kind == TK_INT)
          iv = vt->ival;
        if (ty != TY_INT && vt->kind == TK_INT)
          fv = (double)iv;
        if (neg)
          iv = -iv, fv = -fv;
      }
      data_t *d = add_data(p);
      if (!d) {
        free(name);
        return false;
      }
      d->name = name;
      d->kind = is_mut ? DATA_GLOBAL : DATA_CONST;
      d->ty = ty;
      d->ival = iv;
      d->fval = fv;
      char sym[160];
      snprintf(sym, sizeof(sym), "__nyc_data_nyr_%s", name);
      d->symbol = strdup(sym);
      continue;
    }
    FAIL(t,
         "expected 'fn', 'extern', 'def' or 'mut' at top level, found '%.*s'",
         (int)t->len, t->start);
  }
  return true;
}

/*
 * ------------------------------------------------------------- lowering
 */

typedef struct {
  int v;
  ty_t ty;
  bool is_const; /* integer constant known at parse time */
  int64_t cval;
} val_t;

typedef enum { B_DEF, B_MUT } bkind_t;

typedef struct {
  char *name;
  bkind_t kind;
  ty_t ty;
  int value;    /* B_DEF */
  int64_t slot; /* B_MUT */
  size_t depth;
} bind_t;

typedef struct {
  int64_t brk, cont;
} loop_t;

typedef struct {
  char *name;
  int64_t id;
  bool defined;
  size_t tok;
} label_t;

typedef struct {
  px_t *p;
  fsig_t *sig;
  nyr_func_t *f;
  bind_t *b;
  size_t nb, cb;
  size_t depth;
  loop_t loops[64];
  size_t nloops;
  label_t *labels;
  size_t nlabels, clabels;
  int64_t next_label;
  int64_t next_slot;
  bool dead;       /* last instruction ended its block (br, br.if, ret) */
  bool terminated; /* ... and control cannot fall through (br, ret) */
  ty_t *vty;
  size_t cvty;
  struct ssa_s {
    char *name;
    int v;
    ty_t ty;
  } *ssa;
  size_t nssa, cssa;
  struct fix_s {
    char *name;
    size_t inst, k;
    size_t tok;
  } *fix;
  size_t nfix, cfix;
} fx_t;

static nyr_scalar_class_t cls(ty_t t) {
  return t == TY_F64   ? NYIR_SCALAR_F64
         : t == TY_F32 ? NYIR_SCALAR_F32
                       : NYIR_SCALAR_I64_RAW;
}

static bool fx_note_type(fx_t *x, int v, ty_t t) {
  if (v < 0)
    return true;
  if ((size_t)v >= x->cvty) {
    size_t nc = x->cvty ? x->cvty * 2 : 64;
    while (nc <= (size_t)v)
      nc *= 2;
    ty_t *n = realloc(x->vty, nc * sizeof(*n));
    if (!n)
      return false;
    memset(n + x->cvty, 0, (nc - x->cvty) * sizeof(*n));
    x->vty = n;
    x->cvty = nc;
  }
  x->vty[v] = t;
  return true;
}

static bool fx_raw_emit(fx_t *x, nyr_inst_t in, int *out) {
  char e[256] = {0};
  int v = -1;
  if (!nyr_emit(x->f, in, &v, e, sizeof(e))) {
    px_error_at(x->p, peek(x->p), "internal: %s", e);
    return false;
  }
  if (out)
    *out = v;
  return true;
}

static bool fx_label(fx_t *x, int64_t id) {
  nyr_inst_t in = nyr_inst_init(NYIR_LABEL);
  in.imm = id;
  x->dead = false;
  x->terminated = false;
  return fx_raw_emit(x, in, NULL);
}

static bool fx_emit(fx_t *x, nyr_inst_t in, ty_t ty, int *out) {
  if (x->dead && !fx_label(x, x->next_label++))
    return false;
  /*
   * The x86 encoders do not apply a load/store displacement in every form
   * (an ADDR_SYMBOL base drops it), so fold it into the address.
   */
  if ((in.op == NYIR_LOAD_I64 || in.op == NYIR_STORE_I64) && in.imm) {
    nyr_inst_t k = nyr_inst_init(NYIR_CONST_I64),
               add = nyr_inst_init(NYIR_ADD_I64);
    k.imm = in.imm;
    k.scalar_class = add.scalar_class = NYIR_SCALAR_I64_RAW;
    add.a = in.a;
    if (!fx_raw_emit(x, k, &add.b) || !fx_raw_emit(x, add, &in.a))
      return false;
    in.imm = 0;
  }
  int v = -1;
  if (!fx_raw_emit(x, in, &v))
    return false;
  if (in.op == NYIR_BR || in.op == NYIR_RET || in.op == NYIR_BR_IF)
    x->dead = true;
  if (in.op == NYIR_BR || in.op == NYIR_RET)
    x->terminated = true;
  if (out)
    *out = v;
  return fx_note_type(x, v, ty);
}

static bool fx_br(fx_t *x, int64_t label) {
  if (x->terminated)
    return true; /* already left this block */
  nyr_inst_t in = nyr_inst_init(NYIR_BR);
  in.imm = label;
  return fx_emit(x, in, TY_VOID, NULL);
}

static bool fx_br_if(fx_t *x, int cond, int64_t label) {
  nyr_inst_t in = nyr_inst_init(NYIR_BR_IF);
  in.a = cond;
  in.imm = label;
  return fx_emit(x, in, TY_VOID, NULL);
}

static bool fx_const_i(fx_t *x, int64_t v, val_t *out) {
  nyr_inst_t in = nyr_inst_init(NYIR_CONST_I64);
  in.imm = v;
  in.scalar_class = NYIR_SCALAR_I64_RAW;
  out->ty = TY_INT;
  out->is_const = true;
  out->cval = v;
  return fx_emit(x, in, TY_INT, &out->v);
}

static bool fx_const_f(fx_t *x, double v, ty_t ty, val_t *out) {
  nyr_inst_t in = nyr_inst_init(ty == TY_F32 ? NYIR_CONST_F32 : NYIR_CONST_F64);
  if (ty == TY_F32) {
    float f = (float)v;
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    in.imm = (int64_t)bits;
  } else
    memcpy(&in.imm, &v, sizeof(v));
  in.scalar_class = cls(ty);
  out->ty = ty;
  out->is_const = false;
  return fx_emit(x, in, ty, &out->v);
}

static bool fx_unop(fx_t *x, nyr_op_t op, int a, ty_t ty, val_t *out) {
  nyr_inst_t in = nyr_inst_init(op);
  in.a = a;
  in.scalar_class = cls(ty);
  out->ty = ty;
  out->is_const = false;
  return fx_emit(x, in, ty, &out->v);
}

static bool fx_bin(fx_t *x, nyr_op_t op, int a, int b, ty_t ty, val_t *out) {
  nyr_inst_t in = nyr_inst_init(op);
  in.a = a;
  in.b = b;
  in.scalar_class = cls(ty);
  out->ty = ty;
  out->is_const = false;
  return fx_emit(x, in, ty, &out->v);
}

static bool fx_cmp(fx_t *x, nyr_cmp_t c, val_t a, val_t b, val_t *out) {
  nyr_inst_t in = nyr_inst_init(a.ty == TY_F64   ? NYIR_CMP_F64
                                : a.ty == TY_F32 ? NYIR_CMP_F32
                                                 : NYIR_CMP_I64);
  in.a = a.v;
  in.b = b.v;
  in.cmp = c;
  in.scalar_class = NYIR_SCALAR_I64_RAW;
  out->ty = TY_INT;
  out->is_const = false;
  return fx_emit(x, in, TY_INT, &out->v);
}

static bool fx_load_slot(fx_t *x, int64_t slot, ty_t ty, val_t *out) {
  nyr_inst_t in = nyr_inst_init(NYIR_LOAD_LOCAL);
  in.imm = slot;
  in.scalar_class = cls(ty);
  out->ty = ty;
  out->is_const = false;
  return fx_emit(x, in, ty, &out->v);
}

static bool fx_store_slot(fx_t *x, int64_t slot, val_t v) {
  nyr_inst_t in = nyr_inst_init(NYIR_STORE_LOCAL);
  in.imm = slot;
  in.a = v.v;
  in.scalar_class = cls(v.ty);
  return fx_emit(x, in, TY_VOID, NULL);
}

static bool fx_addr_symbol(fx_t *x, const char *symbol, val_t *out) {
  nyr_inst_t in = nyr_inst_init(NYIR_ADDR_SYMBOL);
  in.symbol = symbol;
  in.scalar_class = NYIR_SCALAR_I64_RAW;
  out->ty = TY_INT;
  out->is_const = false;
  return fx_emit(x, in, TY_INT, &out->v);
}

/*
 * Width flags for load8..load64 / store8..store64.
 */
static unsigned width_flags(int bits) {
  return bits == 8    ? NYIR_INST_F_MEM_BYTE
         : bits == 16 ? NYIR_INST_F_MEM_U16
         : bits == 32 ? NYIR_INST_F_MEM_I32
                      : 0u;
}

static bool fx_load(fx_t *x, val_t addr, int64_t off, int bits, val_t *out) {
  nyr_inst_t in = nyr_inst_init(NYIR_LOAD_I64);
  in.a = addr.v;
  in.imm = off;
  in.flags = width_flags(bits);
  in.scalar_class = NYIR_SCALAR_I64_RAW;
  out->ty = TY_INT;
  out->is_const = false;
  return fx_emit(x, in, TY_INT, &out->v);
}

static bool fx_store(fx_t *x, val_t addr, val_t v, int64_t off, int bits) {
  nyr_inst_t in = nyr_inst_init(NYIR_STORE_I64);
  in.a = addr.v;
  in.c = v.v;
  in.imm = off;
  in.flags = width_flags(bits);
  in.scalar_class = NYIR_SCALAR_I64_RAW;
  return fx_emit(x, in, TY_VOID, NULL);
}

static bind_t *lookup(fx_t *x, const char *name, size_t len) {
  for (size_t i = x->nb; i-- > 0;)
    if (strlen(x->b[i].name) == len && memcmp(x->b[i].name, name, len) == 0)
      return &x->b[i];
  return NULL;
}

static bool bind(fx_t *x, const char *name, bkind_t kind, ty_t ty, int value,
                 const tk_t *at) {
  px_t *p = x->p;
  for (size_t i = x->nb; i-- > 0 && x->b[i].depth == x->depth;)
    if (strcmp(x->b[i].name, name) == 0)
      FAIL(at, "'%s' is already bound in this block", name);
  if (x->nb == x->cb) {
    size_t nc = x->cb ? x->cb * 2 : 16;
    bind_t *n = realloc(x->b, nc * sizeof(*n));
    if (!n)
      return false;
    x->b = n;
    x->cb = nc;
  }
  bind_t *b = &x->b[x->nb++];
  b->name = strdup(name);
  b->kind = kind;
  b->ty = ty;
  b->value = value;
  b->slot = kind == B_MUT ? x->next_slot++ : -1;
  b->depth = x->depth;
  return b->name != NULL;
}

static void scope_close(fx_t *x) {
  while (x->nb && x->b[x->nb - 1].depth == x->depth)
    free(x->b[--x->nb].name);
  --x->depth;
}

static label_t *label_get(fx_t *x, const tk_t *t) {
  for (size_t i = 0; i < x->nlabels; ++i)
    if (strlen(x->labels[i].name) == t->len &&
        memcmp(x->labels[i].name, t->start, t->len) == 0)
      return &x->labels[i];
  if (x->nlabels == x->clabels) {
    size_t nc = x->clabels ? x->clabels * 2 : 8;
    label_t *n = realloc(x->labels, nc * sizeof(*n));
    if (!n)
      return NULL;
    x->labels = n;
    x->clabels = nc;
  }
  label_t *l = &x->labels[x->nlabels++];
  l->name = xstrndup(t->start, t->len);
  l->id = x->next_label++;
  l->defined = false;
  l->tok = (size_t)(t - x->p->t);
  return l;
}

/*
 * Literal operands adapt to the other side's float type.
 */
static bool coerce_literal(fx_t *x, val_t *v, ty_t want) {
  if (v->ty == want)
    return true;
  if (v->is_const && v->ty == TY_INT && want != TY_INT && want != TY_VOID)
    return fx_const_f(x, (double)v->cval, want, v);
  return false;
}

static bool expr(fx_t *x, val_t *out);

static bool expect_type(fx_t *x, val_t *v, ty_t want, const tk_t *at,
                        const char *what) {
  px_t *p = x->p;
  if (v->ty == TY_VOID)
    FAIL(at, "%s has no value (the function returns void)", what);
  if (!coerce_literal(x, v, want))
    FAIL(at, "%s is %s but %s is expected; convert with %s(...)", what,
         ty_name(v->ty), ty_name(want), ty_name(want));
  return true;
}

static bool call_args(fx_t *x, val_t *args, size_t *n, tk_t **ats) {
  px_t *p = x->p;
  *n = 0;
  while (!accept(p, ")")) {
    if (*n && !expect(p, ","))
      return false;
    if (*n >= NYIR_CALL_MAX_ARGS)
      FAIL(peek(p), "too many call arguments");
    ats[*n] = peek(p);
    if (!expr(x, &args[*n]))
      return false;
    ++*n;
  }
  return true;
}

static bool call_fn(fx_t *x, fsig_t *s, tk_t *at, val_t *out) {
  px_t *p = x->p;
  val_t args[NYIR_CALL_MAX_ARGS];
  tk_t *ats[NYIR_CALL_MAX_ARGS];
  size_t n = 0;
  if (!call_args(x, args, &n, ats))
    return false;
  if (n < s->nparams || (n > s->nparams && !s->variadic))
    FAIL(at, "'%s' takes %zu argument%s, %zu given", s->name, s->nparams,
         s->nparams == 1 ? "" : "s", n);
  int ids[NYIR_CALL_MAX_ARGS];
  nyr_inst_t in = nyr_inst_init(NYIR_CALL);
  for (size_t i = 0; i < n; ++i) {
    if (i < s->nparams) {
      char what[64];
      snprintf(what, sizeof(what), "argument %zu of '%s'", i + 1, s->name);
      if (!expect_type(x, &args[i], s->params[i], ats[i], what))
        return false;
    } else if (args[i].ty == TY_F32) {
      /*
       * C default promotion for variadic floats
       */
      if (!fx_unop(x, NYIR_F32_TO_F64, args[i].v, TY_F64, &args[i]))
        return false;
    } else if (args[i].ty == TY_VOID)
      FAIL(ats[i], "argument %zu has no value", i + 1);
    ids[i] = args[i].v;
    nyr_call_arg_class_set(in.arg_scalar_classes, i, cls(args[i].ty));
  }
  int *slots[] = {&in.a, &in.b, &in.c, &in.d, &in.e, &in.f};
  for (size_t i = 0; i < n && i < 6; ++i)
    *slots[i] = ids[i];
  if (n > 6) {
    in.extra_args = ids + 6;
    in.extra_args_len = n - 6;
  }
  in.imm = (int64_t)n;
  in.symbol = s->is_extern || s->exported ? s->symbol : s->name;
  in.flags = (s->is_extern || s->exported ? NYIR_INST_F_EXTERN : 0u) |
             (s->variadic ? NYIR_INST_F_VARIADIC : 0u);
  in.scalar_class = cls(s->ret == TY_VOID ? TY_INT : s->ret);
  if (!fx_emit(x, in, s->ret == TY_VOID ? TY_INT : s->ret, &out->v))
    return false;
  out->ty = s->ret;
  out->is_const = false;
  /*
   * Narrow C results: the upper register bits are unspecified.
   */
  if (s->ret_width != RW_NONE) {
    val_t k;
    if (s->ret_width == RW_I32) {
      if (!fx_const_i(x, 32, &k) ||
          !fx_bin(x, NYIR_SHL_I64, out->v, k.v, TY_INT, out) ||
          !fx_bin(x, NYIR_SAR_I64, out->v, k.v, TY_INT, out))
        return false;
    } else {
      int64_t mask = s->ret_width == RW_U8    ? 0xff
                     : s->ret_width == RW_U16 ? 0xffff
                                              : 0xffffffffll;
      if (!fx_const_i(x, mask, &k) ||
          !fx_bin(x, NYIR_AND_I64, out->v, k.v, TY_INT, out))
        return false;
    }
  }
  return true;
}

static bool builtin_call(fx_t *x, tk_t *name, val_t *out, bool *handled) {
  px_t *p = x->p;
  *handled = true;
  val_t a[NYIR_CALL_MAX_ARGS];
  tk_t *ats[NYIR_CALL_MAX_ARGS];
  size_t n = 0;
  int bits = 0;
  bool load = false, store = false;
  if (name->len >= 5 && memcmp(name->start, "load", 4) == 0)
    load = true, bits = atoi(name->start + 4);
  else if (name->len >= 6 && memcmp(name->start, "store", 5) == 0)
    store = true, bits = atoi(name->start + 5);
  if ((load || store) && bits != 8 && bits != 16 && bits != 32 && bits != 64)
    load = store = false;
  if (load || store) {
    char check[16];
    snprintf(check, sizeof(check), "%s%d", load ? "load" : "store", bits);
    if (!is_word(name, check))
      load = store = false;
  }
  ty_t conv = TY_VOID;
  bool is_conv = false;
  if (is_word(name, "int") || is_word(name, "f64") || is_word(name, "f32") ||
      is_word(name, "ptr")) {
    is_conv = true;
    parse_type_word(name, &conv, NULL);
  }
  bool unary_f =
      is_word(name, "sqrt") || is_word(name, "sin") || is_word(name, "cos");
  bool sel = is_word(name, "select");
  bool bsw = is_word(name, "bswap");
  if (!load && !store && !is_conv && !unary_f && !sel && !bsw) {
    *handled = false;
    return true;
  }
  if (!call_args(x, a, &n, ats))
    return false;
  if (load) {
    if (n < 1 || n > 2)
      FAIL(name, "load%d(addr[, offset]) takes 1 or 2 arguments", bits);
    if (!expect_type(x, &a[0], TY_INT, ats[0], "the address"))
      return false;
    int64_t off = 0;
    if (n == 2) {
      if (!a[1].is_const)
        FAIL(ats[1], "the offset must be an integer constant");
      off = a[1].cval;
    }
    return fx_load(x, a[0], off, bits, out);
  }
  if (store) {
    if (n < 2 || n > 3)
      FAIL(name, "store%d(addr, value[, offset]) takes 2 or 3 arguments", bits);
    if (!expect_type(x, &a[0], TY_INT, ats[0], "the address") ||
        !expect_type(x, &a[1], TY_INT, ats[1], "the stored value"))
      return false;
    int64_t off = 0;
    if (n == 3) {
      if (!a[2].is_const)
        FAIL(ats[2], "the offset must be an integer constant");
      off = a[2].cval;
    }
    out->ty = TY_VOID;
    out->v = -1;
    out->is_const = false;
    return fx_store(x, a[0], a[1], off, bits);
  }
  if (n != (sel ? 3u : 1u))
    FAIL(name, "'%.*s' takes %d argument%s", (int)name->len, name->start,
         sel ? 3 : 1, sel ? "s" : "");
  if (a[0].ty == TY_VOID)
    FAIL(ats[0], "argument has no value");
  if (is_conv) {
    ty_t from = a[0].ty;
    if (from == conv) {
      *out = a[0];
      return true;
    }
    nyr_op_t op =
        from == TY_INT   ? (conv == TY_F64 ? NYIR_I64_TO_F64 : NYIR_I64_TO_F32)
        : from == TY_F64 ? (conv == TY_INT ? NYIR_F64_TO_I64 : NYIR_F64_TO_F32)
                         : (conv == TY_INT ? NYIR_F32_TO_I64 : NYIR_F32_TO_F64);
    return fx_unop(x, op, a[0].v, conv, out);
  }
  if (unary_f) {
    if (!expect_type(x, &a[0], TY_F64, ats[0], "the argument"))
      return false;
    return fx_unop(x,
                   is_word(name, "sqrt")  ? NYIR_SQRT_F64
                   : is_word(name, "sin") ? NYIR_SIN_F64
                                          : NYIR_COS_F64,
                   a[0].v, TY_F64, out);
  }
  if (bsw) {
    if (!expect_type(x, &a[0], TY_INT, ats[0], "the argument"))
      return false;
    return fx_unop(x, NYIR_BSWAP_I64, a[0].v, TY_INT, out);
  }
  /*
   * select(cond, a, b): both arms are evaluated
   */
  if (!expect_type(x, &a[0], TY_INT, ats[0], "the condition") ||
      !expect_type(x, &a[1], TY_INT, ats[1], "select's first arm") ||
      !expect_type(x, &a[2], TY_INT, ats[2], "select's second arm"))
    return false;
  nyr_inst_t in = nyr_inst_init(NYIR_SELECT_I64);
  in.a = a[0].v;
  in.b = a[1].v;
  in.c = a[2].v;
  in.scalar_class = NYIR_SCALAR_I64_RAW;
  out->ty = TY_INT;
  out->is_const = false;
  return fx_emit(x, in, TY_INT, &out->v);
}

/*
 * ------------------------------------------------------------ IR layer
 *
 * Any NYIR instruction can be written directly and mixed with the Ny-style
 * statements above:
 *
 *   %x = add.i64 a, %y          SSA values are %names; plain names read
 *   %c = cmp.i64 lt %x, 10      bindings, literals become immediates
 *   br.if %c, @loop             @labels name blocks
 *   %v = load.i64 %p, 8 !u32    !flags set instruction flags
 *   %r = call $sin(%v) -> f64   $symbols are raw object symbols
 *   %m = phi [%a, @entry], [%b, @loop]
 */

static bool read_name(fx_t *x, tk_t *t, val_t *out);

static bool adjacent(const tk_t *a, const tk_t *b) {
  return a->start + a->len == b->start;
}

/*
 * `%name`: '%' directly followed by a name or number.
 */
static bool at_ssa(px_t *p) {
  tk_t *t = peek(p), *n = peek_at(p, 1);
  return is_punct(t, "%") && adjacent(t, n) &&
         (n->kind == TK_IDENT || n->kind == TK_INT);
}
static bool at_sigil(px_t *p, const char *sigil) {
  tk_t *t = peek(p), *n = peek_at(p, 1);
  return is_punct(t, sigil) && adjacent(t, n) && n->kind == TK_IDENT;
}

static struct ssa_s *ssa_find(fx_t *x, const tk_t *n) {
  for (size_t i = 0; i < x->nssa; ++i)
    if (strlen(x->ssa[i].name) == n->len &&
        memcmp(x->ssa[i].name, n->start, n->len) == 0)
      return &x->ssa[i];
  return NULL;
}

static bool ssa_define(fx_t *x, const tk_t *n, int v, ty_t ty) {
  px_t *p = x->p;
  if (ssa_find(x, n))
    FAIL(n, "%%%.*s is already defined (SSA values are assigned once)",
         (int)n->len, n->start);
  if (x->nssa == x->cssa) {
    size_t nc = x->cssa ? x->cssa * 2 : 32;
    struct ssa_s *s = realloc(x->ssa, nc * sizeof(*s));
    if (!s)
      return false;
    x->ssa = s;
    x->cssa = nc;
  }
  x->ssa[x->nssa++] = (struct ssa_s){xstrndup(n->start, n->len), v, ty};
  return true;
}

static int op_by_name(const tk_t *t) {
  if (t->kind != TK_IDENT)
    return -1;
  for (int op = 0; op < (int)NYIR_OP_COUNT; ++op) {
    const char *name = nyr_op_name((nyr_op_t)op);
    if (name && strlen(name) == t->len && memcmp(name, t->start, t->len) == 0)
      return op;
  }
  return -1;
}

/*
 * An identifier starts an IR instruction when it names an opcode and is
 * not shadowed by a binding or function (`copy`, `phi` and friends).
 */
static int ir_op_at(fx_t *x, const tk_t *t) {
  int op = op_by_name(t);
  if (op < 0)
    return -1;
  if (memchr(t->start, '.', t->len))
    return op;
  if (lookup(x, t->start, t->len) || find_sig(x->p, t->start, t->len))
    return -1;
  return op;
}

static const struct {
  const char *name;
  unsigned flag;
} IR_FLAGS[] = {
    {"extern", NYIR_INST_F_EXTERN},
    {"sret", NYIR_INST_F_SRET},
    {"byte", NYIR_INST_F_MEM_BYTE},
    {"u8", NYIR_INST_F_MEM_BYTE},
    {"u16", NYIR_INST_F_MEM_U16},
    {"u32", NYIR_INST_F_MEM_I32},
    {"volatile", NYIR_INST_F_MEM_VOLATILE},
    {"fs", NYIR_INST_F_MEM_SEG_FS},
    {"gs", NYIR_INST_F_MEM_SEG_GS},
    {"noalias", NYIR_INST_F_NOALIAS},
    {"indirect", NYIR_INST_F_INDIRECT},
    {"variadic", NYIR_INST_F_VARIADIC},
    {"nonpreemptible", NYIR_INST_F_SYMBOL_NONPREEMPTIBLE},
    {"nooverflow", NYIR_INST_F_NO_OVERFLOW},
    {"inbounds", NYIR_INST_F_IN_BOUNDS_ADDR},
    {"assume", NYIR_INST_F_ASSUME},
    {"address_taken", NYIR_INST_F_LABEL_ADDRESS_TAKEN},
};

static const char *const CMP_NAMES[] = {"eq", "ne", "lt", "le", "gt", "ge"};

typedef enum {
  IO_VALUE,
  IO_INT,
  IO_FLOAT,
  IO_LABEL,
  IO_SYMBOL,
  IO_IMM
} io_kind_t;

typedef struct {
  io_kind_t kind;
  val_t v;
  int64_t i;
  double f;
  label_t *label;
  char *symbol; /* owned by the function's symbol table */
  tk_t *tok;
  tk_t *fwd; /* forward %name (phi only) */
} iop_t;

/*
 * `$name`: the raw symbol `name`. A module function's `$name` is its
 * internal symbol.
 */
static const char *raw_symbol(fx_t *x, const tk_t *n) {
  char *tmp = xstrndup(n->start, n->len);
  const char *own = tmp ? nyr_func_own_symbol_copy(x->f, tmp) : NULL;
  free(tmp);
  return own;
}

static bool ir_operand(fx_t *x, iop_t *o, bool allow_forward) {
  px_t *p = x->p;
  memset(o, 0, sizeof(*o));
  o->tok = peek(p);
  if (at_ssa(p)) {
    ++p->pos;
    tk_t *n = peek(p);
    ++p->pos;
    struct ssa_s *s = ssa_find(x, n);
    o->kind = IO_VALUE;
    if (!s && lookup(x, n->start, n->len))
      return read_name(x, n, &o->v); /* %name also reads a def/mut binding */
    if (!s) {
      if (!allow_forward)
        FAIL(n, "%%%.*s is used before it is defined", (int)n->len, n->start);
      o->fwd = n;
      o->v = (val_t){.v = -1, .ty = TY_INT};
      return true;
    }
    o->v = (val_t){.v = s->v, .ty = s->ty};
    return true;
  }
  if (at_sigil(p, "@")) {
    ++p->pos;
    o->kind = IO_LABEL;
    o->label = label_get(x, peek(p));
    ++p->pos;
    return o->label != NULL;
  }
  if (is_punct(peek(p), "#") && adjacent(peek(p), peek_at(p, 1)) &&
      (peek_at(p, 1)->kind == TK_INT ||
       (is_punct(peek_at(p, 1), "-") && peek_at(p, 2)->kind == TK_INT))) {
    ++p->pos; /* #N: a raw immediate for any opcode */
    bool neg = accept(p, "-");
    o->kind = IO_IMM;
    o->i = neg ? (int64_t)(0ull - (uint64_t)peek(p)->ival) : peek(p)->ival;
    ++p->pos;
    return true;
  }
  if (at_sigil(p, "$")) {
    ++p->pos;
    o->kind = IO_SYMBOL;
    o->symbol = (char *)raw_symbol(x, peek(p));
    ++p->pos;
    return o->symbol != NULL;
  }
  tk_t *t = peek(p);
  bool neg = is_punct(t, "-") &&
             (peek_at(p, 1)->kind == TK_INT || peek_at(p, 1)->kind == TK_FLOAT);
  tk_t *lit = neg ? peek_at(p, 1) : t;
  if (lit->kind == TK_INT) {
    p->pos += neg ? 2 : 1;
    o->kind = IO_INT;
    o->i = neg ? (int64_t)(0ull - (uint64_t)lit->ival) : lit->ival;
    return true;
  }
  if (lit->kind == TK_FLOAT) {
    p->pos += neg ? 2 : 1;
    o->kind = IO_FLOAT;
    o->f = neg ? -lit->fval : lit->fval;
    return true;
  }
  if (t->kind == TK_STRING) {
    ++p->pos;
    data_t *d = string_data(p, t->sval, t->slen, NULL);
    o->kind = IO_SYMBOL;
    o->symbol = d ? (char *)nyr_func_own_symbol_copy(x->f, d->symbol) : NULL;
    return o->symbol != NULL;
  }
  /*
   * anything else is a Ny-style expression: a binding, (a + b), f(x)...
   */
  o->kind = IO_VALUE;
  return expr(x, &o->v);
}

static bool ir_value(fx_t *x, iop_t *o, val_t *out) {
  if (o->kind == IO_VALUE) {
    *out = o->v;
    return true;
  }
  if (o->kind == IO_INT)
    return fx_const_i(x, o->i, out);
  if (o->kind == IO_FLOAT)
    return fx_const_f(x, o->f, TY_F64, out);
  if (o->kind == IO_SYMBOL) {
    fsig_t *s = find_sig(x->p, o->symbol, strlen(o->symbol));
    if (s && (s->is_extern || s->exported)) {
      const char *stub = extern_stub(x->p, s->symbol);
      val_t a;
      return stub && fx_addr_symbol(x, stub, &a) && fx_load(x, a, 0, 64, out);
    }
    data_t *d = find_data(x->p, o->symbol, strlen(o->symbol));
    return fx_addr_symbol(x, d ? d->symbol : o->symbol, out);
  }
  px_t *p = x->p;
  FAIL(o->tok, "a label is not a value");
}

static ty_t op_result_type(nyr_op_t op) {
  const char *n = nyr_op_name(op);
  if (!n || !strncmp(n, "vec", 3))
    return TY_INT;
  size_t l = strlen(n);
  if (l > 4 && !strcmp(n + l - 4, ".f64"))
    return TY_F64;
  if (l > 4 && !strcmp(n + l - 4, ".f32"))
    return TY_F32;
  return TY_INT;
}

/*
 * Parse one IR instruction starting at its opcode; `dst` names its result.
 */
static bool ir_inst(fx_t *x, int op, tk_t *dst) {
  px_t *p = x->p;
  tk_t *opt = peek(p);
  ++p->pos;
  nyr_inst_t in = nyr_inst_init((nyr_op_t)op);
  ty_t rty = op_result_type((nyr_op_t)op);
  bool raw_bits = false, have_imm = false;
  int64_t else_label = -1;
  iop_t o;
  int nvals = 0;
  int *vslots[] = {&in.a, &in.b, &in.c, &in.d, &in.e, &in.f};
  nyr_phi_incoming_t *inc = NULL;
  size_t ninc = 0;
  int args[NYIR_CALL_MAX_ARGS];
  size_t argc = 0;
  bool ok = true;
  size_t line = (size_t)opt->line;
#define MORE()                                                                 \
  (peek(p)->line == (int)line && !is_punct(peek(p), "}") &&                    \
   !is_punct(peek(p), ";") && !is_punct(peek(p), "!") &&                       \
   !is_punct(peek(p), "->") && peek(p)->kind != TK_EOF)
  switch ((nyr_op_t)op) {
  case NYIR_CMP_I64:
  case NYIR_CMP_F64:
  case NYIR_CMP_F32: {
    tk_t *c = peek(p);
    size_t k = 0;
    for (; k < 6; ++k)
      if (is_word(c, CMP_NAMES[k]))
        break;
    if (k == 6)
      FAIL(c, "expected a condition (eq, ne, lt, le, gt, ge)");
    ++p->pos;
    in.cmp = (nyr_cmp_t)k;
    rty = TY_INT;
    val_t a, b;
    if (!ir_operand(x, &o, false) || !ir_value(x, &o, &a) || !expect(p, ",") ||
        !ir_operand(x, &o, false) || !ir_value(x, &o, &b))
      return false;
    in.a = a.v;
    in.b = b.v;
    break;
  }
  case NYIR_PHI: {
    size_t cap = 0;
    while (accept(p, "[")) {
      iop_t v, l;
      if (!ir_operand(x, &v, true) || !expect(p, ",") ||
          !ir_operand(x, &l, false) || !expect(p, "]"))
        return false;
      if (l.kind != IO_LABEL)
        FAIL(l.tok, "a phi input names its predecessor with @label");
      val_t vv = v.v;
      if (!v.fwd && !ir_value(x, &v, &vv))
        return false;
      if (ninc == cap) {
        cap = cap ? cap * 2 : 4;
        nyr_phi_incoming_t *n2 = realloc(inc, cap * sizeof(*n2));
        if (!n2) {
          free(inc);
          return false;
        }
        inc = n2;
      }
      inc[ninc] =
          (nyr_phi_incoming_t){.predecessor_label = l.label->id, .value = vv.v};
      if (v.fwd) {
        if (x->nfix == x->cfix) {
          size_t nc = x->cfix ? x->cfix * 2 : 8;
          struct fix_s *f2 = realloc(x->fix, nc * sizeof(*f2));
          if (!f2) {
            free(inc);
            return false;
          }
          x->fix = f2;
          x->cfix = nc;
        }
        x->fix[x->nfix++] =
            (struct fix_s){xstrndup(v.fwd->start, v.fwd->len), SIZE_MAX, ninc,
                           (size_t)(v.fwd - p->t)};
      } else if (vv.ty != TY_INT)
        rty = vv.ty;
      ++ninc;
      if (!accept(p, ","))
        break;
    }
    if (!ninc)
      FAIL(peek(p), "phi needs [value, @label] inputs");
    in.phi_incoming = inc;
    in.phi_incoming_len = ninc;
    break;
  }
  case NYIR_CALL: {
    iop_t callee;
    if (!ir_operand(x, &callee, false))
      return false;
    bool indirect = callee.kind == IO_VALUE;
    if (callee.kind == IO_SYMBOL) {
      fsig_t *s = find_sig(p, callee.symbol, strlen(callee.symbol));
      in.symbol = callee.symbol;
      if (s && (s->is_extern || s->exported)) {
        in.symbol = nyr_func_own_symbol_copy(x->f, s->symbol);
        in.flags |= NYIR_INST_F_EXTERN;
      }
      if (s && s->variadic)
        in.flags |= NYIR_INST_F_VARIADIC;
      if (s && s->ret != TY_VOID)
        rty = s->ret;
    } else if (!indirect)
      FAIL(callee.tok, "call takes $symbol(...) or %%value(...)");
    if (indirect) {
      args[argc++] = callee.v.v;
      in.flags |= NYIR_INST_F_INDIRECT;
    }
    if (!expect(p, "("))
      return false;
    while (!accept(p, ")")) {
      if (argc > (indirect ? 1u : 0u) && !expect(p, ","))
        return false;
      val_t a;
      if (!ir_operand(x, &o, false) || !ir_value(x, &o, &a))
        return false;
      if (argc >= NYIR_CALL_MAX_ARGS)
        FAIL(o.tok, "too many call arguments");
      nyr_call_arg_class_set(in.arg_scalar_classes, argc, cls(a.ty));
      args[argc++] = a.v;
    }
    for (size_t i = 0; i < argc && i < 6; ++i)
      *vslots[i] = args[i];
    if (argc > 6) {
      in.extra_args = args + 6;
      in.extra_args_len = argc - 6;
    }
    in.imm = (int64_t)argc;
    have_imm = true;
    break;
  }
  default:
    while (MORE()) {
      if (nvals || have_imm || in.symbol)
        if (!expect(p, ","))
          return false;
      if (!ir_operand(x, &o, false))
        return false;
      if (o.kind == IO_LABEL) {
        if ((nyr_op_t)op == NYIR_BR_IF && have_imm) {
          else_label = o.label->id;
          continue;
        }
        if ((nyr_op_t)op == NYIR_LABEL) {
          if (o.label->defined)
            FAIL(o.tok, "label '%s' is defined twice", o.label->name);
          o.label->defined = true;
        }
        in.imm = o.label->id;
        have_imm = true;
      } else if (o.kind == IO_SYMBOL && !in.symbol &&
                 ((nyr_op_t)op == NYIR_ADDR_SYMBOL)) {
        in.symbol = o.symbol;
      } else if (o.kind == IO_IMM && !have_imm) {
        in.imm = o.i;
        have_imm = true;
      } else if ((o.kind == IO_INT || o.kind == IO_FLOAT) && !have_imm &&
                 ((nyr_op_t)op == NYIR_CONST_I64 ||
                  (nyr_op_t)op == NYIR_CONST_F64 ||
                  (nyr_op_t)op == NYIR_CONST_F32 ||
                  (nyr_op_t)op == NYIR_LOAD_LOCAL ||
                  (nyr_op_t)op == NYIR_ADDR_LOCAL ||
                  (nyr_op_t)op == NYIR_STORE_LOCAL ||
                  (nyr_op_t)op == NYIR_ALLOCA ||
                  /*
                   * memory offsets follow the address (and value)
                   */
                  (nvals > 0 &&
                   ((nyr_op_t)op == NYIR_LOAD_I64 ||
                    (nyr_op_t)op == NYIR_STORE_I64 ||
                    !strncmp(nyr_op_name((nyr_op_t)op), "vec", 3))))) {
        if ((nyr_op_t)op == NYIR_CONST_F64) {
          double d = o.kind == IO_FLOAT ? o.f : (double)o.i;
          memcpy(&in.imm, &d, sizeof(d));
        } else if ((nyr_op_t)op == NYIR_CONST_F32) {
          float fv = (float)(o.kind == IO_FLOAT ? o.f : (double)o.i);
          uint32_t bits;
          memcpy(&bits, &fv, sizeof(bits));
          in.imm = (int64_t)bits;
        } else {
          if (o.kind == IO_FLOAT)
            FAIL(o.tok, "this immediate must be an integer");
          in.imm = o.i;
        }
        if (o.kind == IO_INT &&
            ((nyr_op_t)op == NYIR_CONST_F64 || (nyr_op_t)op == NYIR_CONST_F32))
          in.imm = o.i, raw_bits = true; /* refined by !bits below */
        have_imm = true;
      } else {
        val_t v;
        if (!ir_value(x, &o, &v))
          return false;
        if (nvals >= 6)
          FAIL(o.tok, "too many operands for '%s'", nyr_op_name(in.op));
        /*
         * stores keep the value in c (a = address, b = unused)
         */
        int slot = nvals;
        if ((nyr_op_t)op == NYIR_STORE_I64 && nvals == 1)
          slot = 2;
        *vslots[slot] = v.v;
        if ((nyr_op_t)op == NYIR_RET || (nyr_op_t)op == NYIR_STORE_LOCAL ||
            (nyr_op_t)op == NYIR_COPY ||
            ((nyr_op_t)op == NYIR_STORE_I64 && slot == 2))
          rty = v.ty;
        ++nvals;
        if ((nyr_op_t)op == NYIR_STORE_I64 && nvals == 2)
          nvals = 3;
      }
    }
    break;
  }
  /*
   * !flags, -> type
   */
  bool bits_flag = false;
  while (is_punct(peek(p), "!") && adjacent(peek(p), peek_at(p, 1))) {
    ++p->pos;
    tk_t *f = peek(p);
    ++p->pos;
    if (f->kind == TK_INT) {
      in.flags |= (unsigned)f->ival;
      continue;
    }
    if (is_word(f, "bits")) {
      bits_flag = true;
      continue;
    }
    size_t k = 0, nf = sizeof(IR_FLAGS) / sizeof(IR_FLAGS[0]);
    for (; k < nf; ++k)
      if (is_word(f, IR_FLAGS[k].name))
        break;
    if (k == nf)
      FAIL(f, "unknown flag '!%.*s'", (int)f->len, f->start);
    in.flags |= IR_FLAGS[k].flag;
  }
  if (raw_bits && !bits_flag) {
    /*
     * `const.f64 3` means 3.0 unless marked !bits
     */
    double d = (double)in.imm;
    if ((nyr_op_t)op == NYIR_CONST_F64)
      memcpy(&in.imm, &d, sizeof(d));
    else {
      float fv = (float)d;
      uint32_t b32;
      memcpy(&b32, &fv, sizeof(b32));
      in.imm = (int64_t)b32;
    }
  }
  if (accept(p, "->")) {
    ty_t t;
    if (!parse_type_word(peek(p), &t, NULL))
      FAIL(peek(p), "expected a result type after '->'");
    ++p->pos;
    rty = t;
  }
  if ((nyr_op_t)op != NYIR_LABEL && (nyr_op_t)op != NYIR_NOP &&
      strncmp(nyr_op_name((nyr_op_t)op), "vec", 3) != 0)
    in.scalar_class = cls(rty);
  if ((nyr_op_t)op == NYIR_LABEL) {
    if (!have_imm)
      FAIL(opt, "label needs an @name");
    ok = fx_label(x, in.imm);
    free(inc);
    return ok;
  }
  int v = -1;
  ok = fx_emit(x, in, rty, &v);
  free(inc);
  for (size_t i = 0; ok && i < x->nfix; ++i)
    if (x->fix[i].inst == SIZE_MAX)
      x->fix[i].inst = x->f->len - 1;
  if (ok && (nyr_op_t)op == NYIR_BR_IF && else_label >= 0)
    ok = fx_br(x, else_label);
  if (!ok)
    return false;
  if (dst) {
    if (v < 0)
      FAIL(opt, "'%s' produces no value to bind", nyr_op_name(in.op));
    return ssa_define(x, dst, v, rty);
  }
  return true;
#undef MORE
}

/*
 * `%x = <IR instruction | expression>`
 */
static bool ssa_stmt(fx_t *x) {
  px_t *p = x->p;
  ++p->pos;
  tk_t *n = peek(p);
  ++p->pos;
  if (!expect(p, "="))
    return false;
  int op = ir_op_at(x, peek(p));
  if (op >= 0)
    return ir_inst(x, op, n);
  val_t v;
  tk_t *at = peek(p);
  if (!expr(x, &v))
    return false;
  if (v.ty == TY_VOID)
    FAIL(at, "the expression has no value");
  return ssa_define(x, n, v.v, v.ty);
}

static bool fix_phis(fx_t *x) {
  px_t *p = x->p;
  for (size_t i = 0; i < x->nfix; ++i) {
    struct fix_s *fx = &x->fix[i];
    tk_t *t = &p->t[fx->tok];
    struct ssa_s *s = ssa_find(x, t);
    if (!s)
      FAIL(t, "%%%s is never defined", fx->name);
    x->f->data[fx->inst].phi_incoming[fx->k].value = s->v;
  }
  return true;
}

static bool read_name(fx_t *x, tk_t *t, val_t *out) {
  px_t *p = x->p;
  bind_t *b = lookup(x, t->start, t->len);
  if (b) {
    if (b->kind == B_DEF) {
      out->v = b->value;
      out->ty = b->ty;
      out->is_const = false;
      return true;
    }
    return fx_load_slot(x, b->slot, b->ty, out);
  }
  data_t *d = find_data(p, t->start, t->len);
  if (d) {
    if (d->kind == DATA_STRING || d->kind == DATA_RAW)
      return fx_addr_symbol(x, d->symbol, out);
    if (d->kind == DATA_CONST)
      return d->ty == TY_INT ? fx_const_i(x, d->ival, out)
                             : fx_const_f(x, d->fval, d->ty, out);
    val_t addr;
    if (!fx_addr_symbol(x, d->symbol, &addr))
      return false;
    if (d->ty == TY_INT)
      return fx_load(x, addr, 0, 64, out);
    nyr_inst_t in = nyr_inst_init(NYIR_LOAD_I64);
    in.a = addr.v;
    in.flags = d->ty == TY_F32 ? NYIR_INST_F_MEM_I32 : 0u;
    in.scalar_class = cls(d->ty);
    out->ty = d->ty;
    out->is_const = false;
    return fx_emit(x, in, d->ty, &out->v);
  }
  if (find_sig(p, t->start, t->len))
    FAIL(t, "'%.*s' is a function; call it with (...)", (int)t->len, t->start);
  FAIL(t, "unknown name '%.*s'", (int)t->len, t->start);
}

static bool primary(fx_t *x, val_t *out) {
  px_t *p = x->p;
  tk_t *t = peek(p);
  out->is_const = false;
  if (t->kind == TK_INT) {
    ++p->pos;
    return fx_const_i(x, t->ival, out);
  }
  if (t->kind == TK_FLOAT) {
    ++p->pos;
    return fx_const_f(x, t->fval, t->f32 ? TY_F32 : TY_F64, out);
  }
  if (t->kind == TK_STRING) {
    ++p->pos;
    data_t *d = string_data(p, t->sval, t->slen, NULL);
    return d && fx_addr_symbol(x, d->symbol, out);
  }
  if (accept(p, "(")) {
    return expr(x, out) && expect(p, ")");
  }
  if (at_ssa(p) || at_sigil(p, "$")) {
    iop_t o;
    return ir_operand(x, &o, false) && ir_value(x, &o, out);
  }
  if (is_word(t, "true") || is_word(t, "false") || is_word(t, "nil")) {
    ++p->pos;
    return fx_const_i(x, is_word(t, "true") ? 1 : 0, out);
  }
  if (t->kind == TK_IDENT && !is_keyword(t)) {
    ++p->pos;
    tk_t *nx = peek(p);
    if (is_punct(nx, "(") && nx->line == t->line) {
      ++p->pos;
      bool handled = false;
      if (!lookup(x, t->start, t->len) && !find_sig(p, t->start, t->len) &&
          !builtin_call(x, t, out, &handled))
        return false;
      if (handled)
        return true;
      fsig_t *s = find_sig(p, t->start, t->len);
      if (!s)
        FAIL(t, "unknown function '%.*s'", (int)t->len, t->start);
      return call_fn(x, s, t, out);
    }
    return read_name(x, t, out);
  }
  FAIL(t, "expected an expression, found '%.*s'",
       (int)(t->kind == TK_EOF ? 3 : t->len),
       t->kind == TK_EOF ? "EOF" : t->start);
}

static bool unary(fx_t *x, val_t *out) {
  px_t *p = x->p;
  tk_t *t = peek(p);
  if (accept(p, "-")) {
    if (!unary(x, out))
      return false;
    if (out->is_const)
      return fx_const_i(x, (int64_t)(0ull - (uint64_t)out->cval), out);
    if (out->ty == TY_VOID)
      FAIL(t, "negating a value-less expression");
    val_t z;
    if (out->ty == TY_INT)
      return fx_const_i(x, 0, &z) &&
             fx_bin(x, NYIR_SUB_I64, z.v, out->v, TY_INT, out);
    nyr_op_t mul = out->ty == TY_F64 ? NYIR_MUL_F64 : NYIR_MUL_F32;
    return fx_const_f(x, -1.0, out->ty, &z) &&
           fx_bin(x, mul, out->v, z.v, out->ty, out);
  }
  if (accept(p, "~")) {
    if (!unary(x, out) || !expect_type(x, out, TY_INT, t, "'~' operand"))
      return false;
    if (out->is_const)
      return fx_const_i(x, ~out->cval, out);
    val_t m;
    return fx_const_i(x, -1, &m) &&
           fx_bin(x, NYIR_XOR_I64, out->v, m.v, TY_INT, out);
  }
  if (accept(p, "!") || accept_word(p, "not")) {
    if (!unary(x, out) || !expect_type(x, out, TY_INT, t, "'!' operand"))
      return false;
    val_t z;
    return fx_const_i(x, 0, &z) && fx_cmp(x, NYIR_CMP_EQ, *out, z, out);
  }
  return primary(x, out);
}

typedef struct {
  const char *op;
  int prec;
} binop_t;

static const binop_t BINOPS[] = {
    {"|", 3},  {"^", 4},  {"&", 5},  {"==", 6}, {"!=", 6}, {"<", 7},
    {"<=", 7}, {">", 7},  {">=", 7}, {"<<", 8}, {">>", 8}, {"+", 9},
    {"-", 9},  {"*", 10}, {"/", 10}, {"%", 10}, {NULL, 0}};

static const binop_t *binop_at(const tk_t *t) {
  if (t->kind != TK_PUNCT)
    return NULL;
  for (size_t k = 0; BINOPS[k].op; ++k)
    if (is_punct(t, BINOPS[k].op))
      return &BINOPS[k];
  return NULL;
}

static bool fold(const char *op, int64_t a, int64_t b, int64_t *r) {
  uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
  if (!strcmp(op, "+"))
    *r = (int64_t)(ua + ub);
  else if (!strcmp(op, "-"))
    *r = (int64_t)(ua - ub);
  else if (!strcmp(op, "*"))
    *r = (int64_t)(ua * ub);
  else if (!strcmp(op, "<<") && b >= 0 && b < 64)
    *r = (int64_t)(ua << b);
  else if (!strcmp(op, "&"))
    *r = a & b;
  else if (!strcmp(op, "|"))
    *r = a | b;
  else if (!strcmp(op, "^"))
    *r = a ^ b;
  else
    return false;
  return true;
}

static bool apply_binop(fx_t *x, const char *op, tk_t *at, val_t l, val_t r,
                        val_t *out) {
  px_t *p = x->p;
  if (l.ty == TY_VOID || r.ty == TY_VOID)
    FAIL(at, "operand of '%s' has no value", op);
  if (l.ty != r.ty) {
    if (!coerce_literal(x, &l, r.ty) && !coerce_literal(x, &r, l.ty))
      FAIL(at, "'%s' mixes %s and %s; convert one side explicitly", op,
           ty_name(l.ty), ty_name(r.ty));
  }
  ty_t ty = l.ty;
  int64_t folded;
  if (ty == TY_INT && l.is_const && r.is_const &&
      fold(op, l.cval, r.cval, &folded))
    return fx_const_i(x, folded, out);
  static const struct {
    const char *op;
    nyr_cmp_t cmp;
  } CMPS[] = {{"==", NYIR_CMP_EQ}, {"!=", NYIR_CMP_NE}, {"<", NYIR_CMP_LT},
              {"<=", NYIR_CMP_LE}, {">", NYIR_CMP_GT},  {">=", NYIR_CMP_GE}};
  for (size_t k = 0; k < 6; ++k)
    if (!strcmp(op, CMPS[k].op))
      return fx_cmp(x, CMPS[k].cmp, l, r, out);
  static const struct {
    const char *op;
    nyr_op_t i, f, s;
  } ARITH[] = {
      {"+", NYIR_ADD_I64, NYIR_ADD_F64, NYIR_ADD_F32},
      {"-", NYIR_SUB_I64, NYIR_SUB_F64, NYIR_SUB_F32},
      {"*", NYIR_MUL_I64, NYIR_MUL_F64, NYIR_MUL_F32},
      {"/", NYIR_DIV_I64, NYIR_DIV_F64, NYIR_DIV_F32},
      {"%", NYIR_MOD_I64, NYIR_OP_COUNT, NYIR_OP_COUNT},
      {"&", NYIR_AND_I64, NYIR_OP_COUNT, NYIR_OP_COUNT},
      {"|", NYIR_OR_I64, NYIR_OP_COUNT, NYIR_OP_COUNT},
      {"^", NYIR_XOR_I64, NYIR_OP_COUNT, NYIR_OP_COUNT},
      {"<<", NYIR_SHL_I64, NYIR_OP_COUNT, NYIR_OP_COUNT},
      {">>", NYIR_SAR_I64, NYIR_OP_COUNT, NYIR_OP_COUNT},
  };
  for (size_t k = 0; k < sizeof(ARITH) / sizeof(ARITH[0]); ++k)
    if (!strcmp(op, ARITH[k].op)) {
      nyr_op_t o = ty == TY_INT   ? ARITH[k].i
                   : ty == TY_F64 ? ARITH[k].f
                                  : ARITH[k].s;
      if (o == NYIR_OP_COUNT)
        FAIL(at, "'%s' needs int operands, not %s", op, ty_name(ty));
      return fx_bin(x, o, l.v, r.v, ty, out);
    }
  FAIL(at, "unknown operator '%s'", op);
}

static bool binary(fx_t *x, int min_prec, val_t *out) {
  px_t *p = x->p;
  if (!unary(x, out))
    return false;
  for (;;) {
    tk_t *t = peek(p);
    const binop_t *b = binop_at(t);
    if (!b || b->prec < min_prec)
      return true;
    /*
     * An operator that starts a line starts a new statement (`-y`,
     * `%x = ...`); continue an expression by ending the line with it.
     */
    if (t->line_start)
      return true;
    ++p->pos;
    val_t r;
    if (!binary(x, b->prec + 1, &r))
      return false;
    char op[4];
    snprintf(op, sizeof(op), "%.*s", (int)t->len, t->start);
    if (!apply_binop(x, op, t, *out, r, out))
      return false;
  }
}

/*
 * `a && b`, `a || b` (also `and`, `or`) short-circuit through a temp slot.
 */
static bool logic(fx_t *x, int level, val_t *out) {
  px_t *p = x->p;
  if (!(level == 0 ? logic(x, 1, out) : binary(x, 3, out)))
    return false;
  for (;;) {
    tk_t *t = peek(p);
    bool is_or = level == 0 && (is_punct(t, "||") || is_word(t, "or"));
    bool is_and = level == 1 && (is_punct(t, "&&") || is_word(t, "and"));
    if (!is_or && !is_and)
      return true;
    ++p->pos;
    if (!expect_type(x, out, TY_INT, t, "a logical operand"))
      return false;
    int64_t slot = x->next_slot++;
    val_t z, b;
    int64_t done = x->next_label++, rhs = x->next_label++;
    if (!fx_const_i(x, 0, &z) || !fx_cmp(x, NYIR_CMP_NE, *out, z, &b) ||
        !fx_store_slot(x, slot, b))
      return false;
    /*
     * || skips the right side when true, && when false
     */
    if (is_or) {
      if (!fx_br_if(x, b.v, done) || !fx_br(x, rhs))
        return false;
    } else {
      if (!fx_br_if(x, b.v, rhs) || !fx_br(x, done))
        return false;
    }
    val_t r;
    if (!fx_label(x, rhs) ||
        !(level == 0 ? logic(x, 1, &r) : binary(x, 3, &r)) ||
        !expect_type(x, &r, TY_INT, t, "a logical operand") ||
        !fx_const_i(x, 0, &z) || !fx_cmp(x, NYIR_CMP_NE, r, z, &b) ||
        !fx_store_slot(x, slot, b) || !fx_br(x, done) || !fx_label(x, done) ||
        !fx_load_slot(x, slot, TY_INT, out))
      return false;
  }
}

/*
 * `cond ? a : b` lowers to branches, so only the chosen arm runs.
 */
static bool expr(fx_t *x, val_t *out) {
  px_t *p = x->p;
  if (!logic(x, 0, out))
    return false;
  tk_t *q = peek(p);
  if (!accept(p, "?"))
    return true;
  if (!expect_type(x, out, TY_INT, q, "the condition"))
    return false;
  int64_t yes = x->next_label++, no = x->next_label++, end = x->next_label++;
  int64_t slot = x->next_slot++;
  if (!fx_br_if(x, out->v, yes) || !fx_br(x, no) || !fx_label(x, yes))
    return false;
  val_t a, b;
  tk_t *at = peek(p);
  if (!expr(x, &a) || !expect(p, ":"))
    return false;
  if (a.ty == TY_VOID)
    FAIL(at, "'?' arm has no value");
  if (!fx_store_slot(x, slot, a) || !fx_br(x, end) || !fx_label(x, no))
    return false;
  tk_t *bt = peek(p);
  if (!expr(x, &b) || !expect_type(x, &b, a.ty, bt, "the ':' arm") ||
      !fx_store_slot(x, slot, b) || !fx_br(x, end) || !fx_label(x, end))
    return false;
  return fx_load_slot(x, slot, a.ty, out);
}

static bool block(fx_t *x);

static bool ret_stmt(fx_t *x, tk_t *at) {
  px_t *p = x->p;
  val_t v = {.v = -1, .ty = TY_VOID};
  tk_t *nx = peek(p);
  bool has_value = !is_punct(nx, "}") && !is_punct(nx, ";") &&
                   nx->kind != TK_EOF && nx->line == at->line;
  if (has_value && !expr(x, &v))
    return false;
  ty_t want = x->sig->ret;
  if (want == TY_VOID && v.ty != TY_VOID)
    FAIL(at, "'%s' returns void; 'return' takes no value", x->sig->name);
  if (want != TY_VOID && v.ty == TY_VOID)
    FAIL(at, "'%s' must return %s", x->sig->name, ty_name(want));
  if (want != TY_VOID && !expect_type(x, &v, want, nx, "the returned value"))
    return false;
  if (want == TY_VOID && !fx_const_i(x, 0, &v))
    return false;
  nyr_inst_t in = nyr_inst_init(NYIR_RET);
  in.a = v.v;
  in.scalar_class = cls(v.ty);
  return fx_emit(x, in, TY_VOID, NULL);
}

static bool decl_stmt(fx_t *x, bool is_mut) {
  px_t *p = x->p;
  ty_t ty = TY_VOID;
  if (parse_type_word(peek(p), &ty, NULL)) {
    if (ty == TY_VOID)
      FAIL(peek(p), "a binding cannot be void");
    ++p->pos;
  }
  tk_t *nt = peek(p);
  char *name = NULL;
  if (!expect_ident(p, &name))
    return false;
  val_t v = {.v = -1, .ty = TY_VOID};
  tk_t *vt = peek(p);
  if (accept(p, "=")) {
    vt = peek(p);
    if (!expr(x, &v)) {
      free(name);
      return false;
    }
    if (ty == TY_VOID)
      ty = v.ty;
    if (!expect_type(x, &v, ty, vt, "the initializer")) {
      free(name);
      return false;
    }
  } else {
    if (!is_mut) {
      free(name);
      FAIL(vt, "'def %s' needs a value", nt->len ? name : "");
    }
    if (ty == TY_VOID)
      ty = TY_INT;
    if (!(ty == TY_INT ? fx_const_i(x, 0, &v) : fx_const_f(x, 0.0, ty, &v))) {
      free(name);
      return false;
    }
  }
  bool ok = bind(x, name, is_mut ? B_MUT : B_DEF, ty, v.v, nt);
  if (ok && is_mut)
    ok = fx_store_slot(x, x->b[x->nb - 1].slot, v);
  free(name);
  return ok;
}

/*
 * `name = e`, `name op= e`, `name++`, `name--`.
 */
static bool assign_stmt(fx_t *x, tk_t *nt) {
  px_t *p = x->p;
  tk_t *op = peek(p);
  ++p->pos;
  bind_t *b = lookup(x, nt->start, nt->len);
  data_t *d = b ? NULL : find_data(p, nt->start, nt->len);
  if (!b && !d)
    FAIL(nt, "unknown name '%.*s'; declare it with 'mut'", (int)nt->len,
         nt->start);
  if ((b && b->kind == B_DEF) || (d && d->kind != DATA_GLOBAL))
    FAIL(nt, "'%.*s' is immutable; declare it with 'mut'", (int)nt->len,
         nt->start);
  ty_t ty = b ? b->ty : d->ty;
  val_t v;
  tk_t *vt = peek(p);
  if (is_punct(op, "++") || is_punct(op, "--")) {
    val_t cur, one;
    if (!read_name(x, nt, &cur) || !fx_const_i(x, 1, &one) ||
        !apply_binop(x, is_punct(op, "++") ? "+" : "-", op, cur, one, &v))
      return false;
  } else {
    if (!expr(x, &v))
      return false;
    if (!is_punct(op, "=")) {
      char bop[4];
      snprintf(bop, sizeof(bop), "%.*s", (int)op->len - 1, op->start);
      val_t cur;
      if (!read_name(x, nt, &cur) || !apply_binop(x, bop, op, cur, v, &v))
        return false;
    }
  }
  if (!expect_type(x, &v, ty, vt, "the assigned value"))
    return false;
  if (b)
    return fx_store_slot(x, b->slot, v);
  val_t addr;
  if (!fx_addr_symbol(x, d->symbol, &addr))
    return false;
  nyr_inst_t in = nyr_inst_init(NYIR_STORE_I64);
  in.a = addr.v;
  in.c = v.v;
  in.flags = ty == TY_F32 ? NYIR_INST_F_MEM_I32 : 0u;
  in.scalar_class = cls(ty);
  return fx_emit(x, in, TY_VOID, NULL);
}

static bool cond_branch(fx_t *x, int64_t yes, int64_t no) {
  px_t *p = x->p;
  tk_t *at = peek(p);
  val_t c;
  if (!expr(x, &c) || !expect_type(x, &c, TY_INT, at, "the condition"))
    return false;
  return fx_br_if(x, c.v, yes) && fx_br(x, no);
}

static bool if_stmt(fx_t *x) {
  px_t *p = x->p;
  int64_t end = x->next_label++;
  for (;;) {
    int64_t yes = x->next_label++, no = x->next_label++;
    if (!cond_branch(x, yes, no) || !fx_label(x, yes) || !block(x) ||
        !fx_br(x, end) || !fx_label(x, no))
      return false;
    if (accept_word(p, "elif"))
      continue;
    if (accept_word(p, "else")) {
      if (accept_word(p, "if"))
        continue;
      if (!block(x))
        return false;
    }
    return fx_br(x, end) && fx_label(x, end);
  }
}

static bool loop_body(fx_t *x, int64_t brk, int64_t cont, tk_t *at) {
  px_t *p = x->p;
  if (x->nloops >= sizeof(x->loops) / sizeof(x->loops[0]))
    FAIL(at, "loops nested too deeply");
  x->loops[x->nloops++] = (loop_t){brk, cont};
  bool ok = block(x);
  --x->nloops;
  return ok;
}

static bool while_stmt(fx_t *x, tk_t *at) {
  int64_t head = x->next_label++, body = x->next_label++, end = x->next_label++;
  return fx_br(x, head) && fx_label(x, head) && cond_branch(x, body, end) &&
         fx_label(x, body) && loop_body(x, end, head, at) && fx_br(x, head) &&
         fx_label(x, end);
}

static bool simple_stmt(fx_t *x);

/*
 * `for (init cond update) { }` and `for i in lo..hi { }` (inclusive).
 */
static bool for_stmt(fx_t *x, tk_t *at) {
  px_t *p = x->p;
  ++x->depth;
  int64_t head = x->next_label++, body = x->next_label++,
          step = x->next_label++, end = x->next_label++;
  bool ok;
  if (accept(p, "(")) {
    ok = (accept(p, ";") || simple_stmt(x)) && (accept(p, ";"), true) &&
         fx_br(x, head) && fx_label(x, head);
    if (ok && is_punct(peek(p), ")")) {
      ok = fx_br(x, body);
    } else
      ok = ok && cond_branch(x, body, end);
    ok = ok && (accept(p, ";"), true);
    /*
     * the update is parsed now but emitted after the body
     */
    size_t upd = p->pos;
    if (ok && !is_punct(peek(p), ")")) {
      int depth = 0;
      while (peek(p)->kind != TK_EOF &&
             !(depth == 0 && is_punct(peek(p), ")"))) {
        if (is_punct(peek(p), "("))
          ++depth;
        else if (is_punct(peek(p), ")"))
          --depth;
        ++p->pos;
      }
    }
    ok = ok && expect(p, ")") && fx_label(x, body) &&
         loop_body(x, end, step, at) && fx_br(x, step) && fx_label(x, step);
    if (ok) {
      size_t after = p->pos;
      p->pos = upd;
      ok = (is_punct(peek(p), ")") || simple_stmt(x));
      p->pos = after;
    }
    ok = ok && fx_br(x, head) && fx_label(x, end);
  } else {
    tk_t *nt = peek(p);
    char *name = NULL;
    val_t lo, hi;
    ok = expect_ident(p, &name) &&
         (accept_word(p, "in") ||
          (px_error_at(p, peek(p), "expected 'in'"), false));
    tk_t *lt = peek(p);
    ok = ok && binary(x, 3, &lo) &&
         expect_type(x, &lo, TY_INT, lt, "the range start") && expect(p, "..");
    tk_t *ht = peek(p);
    ok = ok && binary(x, 3, &hi) &&
         expect_type(x, &hi, TY_INT, ht, "the range end");
    int64_t hslot = x->next_slot++;
    ok = ok && bind(x, name, B_MUT, TY_INT, -1, nt);
    free(name);
    if (ok) {
      int64_t slot = x->b[x->nb - 1].slot;
      val_t i, h, c, one;
      ok = fx_store_slot(x, slot, lo) && fx_store_slot(x, hslot, hi) &&
           fx_br(x, head) && fx_label(x, head) &&
           fx_load_slot(x, slot, TY_INT, &i) &&
           fx_load_slot(x, hslot, TY_INT, &h) &&
           fx_cmp(x, NYIR_CMP_LE, i, h, &c) && fx_br_if(x, c.v, body) &&
           fx_br(x, end) && fx_label(x, body) && loop_body(x, end, step, at) &&
           fx_br(x, step) && fx_label(x, step) &&
           fx_load_slot(x, slot, TY_INT, &i) && fx_const_i(x, 1, &one) &&
           fx_bin(x, NYIR_ADD_I64, i.v, one.v, TY_INT, &i) &&
           fx_store_slot(x, slot, i) && fx_br(x, head) && fx_label(x, end);
    }
  }
  scope_close(x);
  return ok;
}

/*
 * Declarations, assignments and expression statements (for headers too).
 */
static bool simple_stmt(fx_t *x) {
  px_t *p = x->p;
  tk_t *t = peek(p);
  if (accept_word(p, "mut"))
    return decl_stmt(x, true);
  if (accept_word(p, "def"))
    return decl_stmt(x, false);
  if (t->kind == TK_IDENT && !is_keyword(t)) {
    tk_t *n = peek_at(p, 1);
    static const char *const ASSIGN[] = {
        "=",  "+=", "-=",  "*=",  "/=", "%=", "&=",
        "|=", "^=", "<<=", ">>=", "++", "--", NULL};
    for (size_t k = 0; ASSIGN[k]; ++k)
      if (is_punct(n, ASSIGN[k])) {
        ++p->pos;
        return assign_stmt(x, t);
      }
  }
  val_t v;
  return expr(x, &v);
}

static bool stmt(fx_t *x) {
  px_t *p = x->p;
  tk_t *t = peek(p);
  if (accept(p, ";"))
    return true;
  if (is_punct(t, "{")) {
    return block(x);
  }
  if (t->kind == TK_IDENT && !is_keyword(t) && is_punct(peek_at(p, 1), ":") &&
      t->line_start) {
    p->pos += 2;
    label_t *l = label_get(x, t);
    if (!l)
      return false;
    if (l->defined)
      FAIL(t, "label '%.*s' is defined twice", (int)t->len, t->start);
    l->defined = true;
    return fx_br(x, l->id) && fx_label(x, l->id);
  }
  if (at_sigil(p, "@") && t->line_start) {
    p->pos += 2;
    label_t *l = label_get(x, peek_at(p, -1));
    if (!l)
      return false;
    if (l->defined)
      FAIL(t, "label '%s' is defined twice", l->name);
    l->defined = true;
    return fx_br(x, l->id) && fx_label(x, l->id);
  }
  if (at_ssa(p) && is_punct(peek_at(p, 2), "="))
    return ssa_stmt(x);
  if (t->kind == TK_IDENT) {
    int op = ir_op_at(x, t);
    if (op >= 0)
      return ir_inst(x, op, NULL);
  }
  if (accept_word(p, "goto")) {
    tk_t *lt = peek(p);
    if (lt->kind != TK_IDENT || is_keyword(lt))
      FAIL(lt, "expected a label name after 'goto'");
    ++p->pos;
    label_t *l = label_get(x, lt);
    return l && fx_br(x, l->id);
  }
  if (accept_word(p, "return"))
    return ret_stmt(x, t);
  if (accept_word(p, "if"))
    return if_stmt(x);
  if (accept_word(p, "while"))
    return while_stmt(x, t);
  if (accept_word(p, "for"))
    return for_stmt(x, t);
  if (accept_word(p, "break") || accept_word(p, "continue")) {
    if (!x->nloops)
      FAIL(t, "'%.*s' outside a loop", (int)t->len, t->start);
    loop_t l = x->loops[x->nloops - 1];
    return fx_br(x, is_word(t, "break") ? l.brk : l.cont);
  }
  if (is_word(t, "elif") || is_word(t, "else"))
    FAIL(t, "'%.*s' without a matching 'if'", (int)t->len, t->start);
  if (is_word(t, "fn") || is_word(t, "extern"))
    FAIL(t, "'%.*s' is only allowed at top level", (int)t->len, t->start);
  return simple_stmt(x);
}

static bool block(fx_t *x) {
  px_t *p = x->p;
  if (!expect(p, "{"))
    return false;
  ++x->depth;
  while (!accept(p, "}")) {
    if (peek(p)->kind == TK_EOF)
      FAIL(peek(p), "missing '}'");
    if (!stmt(x))
      return false;
  }
  scope_close(x);
  return true;
}

static bool lower_function(px_t *p, fsig_t *s, nyr_func_t *f) {
  fx_t x = {.p = p,
            .sig = s,
            .f = f,
            .next_label = 1,
            .next_slot = (int64_t)s->nparams};
  bool ok = true;
  if (s->nparams) {
    nyr_param_desc_t *pd = calloc(s->nparams, sizeof(*pd));
    if (!pd)
      return false;
    for (size_t i = 0; i < s->nparams; ++i)
      pd[i].type = s->params[i] == TY_F64   ? NYIR_PARAM_F64
                   : s->params[i] == TY_F32 ? NYIR_PARAM_F32
                                            : NYIR_PARAM_I64;
    char e[256] = {0};
    ok = nyr_func_set_signature(f, pd, s->nparams, 0, NULL, e, sizeof(e));
    free(pd);
    if (!ok) {
      px_error_at(p, &p->t[s->body], "internal: %s", e);
      return false;
    }
  }
  x.depth = 1;
  for (size_t i = 0; ok && i < s->nparams; ++i) {
    /*
     * parameters are mutable slots 0..n-1, as in Ny
     */
    for (size_t j = 0; j < i; ++j)
      if (!strcmp(s->pnames[i], s->pnames[j])) {
        px_error_at(p, &p->t[s->body], "parameter '%s' is repeated",
                    s->pnames[i]);
        ok = false;
      }
    if (!ok || x.nb == x.cb) {
      size_t nc = x.cb ? x.cb * 2 : 16;
      bind_t *n = ok ? realloc(x.b, nc * sizeof(*n)) : NULL;
      if (!n) {
        ok = false;
        break;
      }
      x.b = n;
      x.cb = nc;
    }
    x.b[x.nb++] = (bind_t){.name = strdup(s->pnames[i]),
                           .kind = B_MUT,
                           .ty = s->params[i],
                           .slot = (int64_t)i,
                           .depth = 1};
  }
  p->pos = s->body;
  ok = ok && block(&x);
  if (ok && !x.terminated) {
    val_t v;
    nyr_inst_t in = nyr_inst_init(NYIR_RET);
    ok = s->ret == TY_F64 || s->ret == TY_F32 ? fx_const_f(&x, 0.0, s->ret, &v)
                                              : fx_const_i(&x, 0, &v);
    in.a = v.v;
    in.scalar_class = cls(v.ty);
    ok = ok && fx_emit(&x, in, TY_VOID, NULL);
  }
  ok = ok && fix_phis(&x);
  for (size_t i = 0; ok && i < x.nlabels; ++i)
    if (!x.labels[i].defined) {
      px_error_at(p, &p->t[x.labels[i].tok], "label '%s' is never defined",
                  x.labels[i].name);
      ok = false;
    }
  if (ok) {
    char e[512] = {0};
    if (!nyr_verify(f, e, sizeof(e))) {
      px_error_at(p, &p->t[s->body], "NYIR verifier rejected '%s': %s", s->name,
                  e);
      ok = false;
    }
  }
  while (x.nb)
    free(x.b[--x.nb].name);
  free(x.b);
  for (size_t i = 0; i < x.nlabels; ++i)
    free(x.labels[i].name);
  free(x.labels);
  free(x.vty);
  for (size_t i = 0; i < x.nssa; ++i)
    free(x.ssa[i].name);
  free(x.ssa);
  for (size_t i = 0; i < x.nfix; ++i)
    free(x.fix[i].name);
  free(x.fix);
  return ok;
}

/*
 * ---------------------------------------------------------------- API
 */

void nyr_text_module_free(nyr_text_module_t *m) {
  if (!m)
    return;
  for (size_t i = 0; i < m->nfuncs; ++i) {
    free(m->funcs[i].name);
    free(m->funcs[i].symbol);
    nyr_func_destroy(&m->funcs[i].func);
  }
  free(m->funcs);
  for (size_t i = 0; i < m->ndata; ++i) {
    free(m->data[i].name);
    free(m->data[i].symbol);
    free(m->data[i].bytes);
  }
  free(m->data);
  free(m->asm_text);
  free(m);
}

nyr_text_module_t *nyr_text_parse(const char *source, size_t length,
                                  const char *path, char *err, size_t err_len) {
  if (err && err_len)
    err[0] = 0;
  px_t p = {.src = source ? source : "",
            .len = source ? length : 0,
            .path = path,
            .err = err,
            .err_len = err_len};
  p.mod = calloc(1, sizeof(*p.mod));
  bool ok = p.mod && lex(&p) && parse_top(&p) && resolve_data_refs(&p);
  size_t nfn = 0;
  for (size_t i = 0; ok && i < p.nsigs; ++i)
    if (!p.sigs[i].is_extern)
      p.sigs[i].index = nfn++;
  if (ok && nfn) {
    p.mod->funcs = calloc(nfn, sizeof(*p.mod->funcs));
    ok = p.mod->funcs != NULL;
  }
  for (size_t i = 0; ok && i < p.nsigs; ++i) {
    fsig_t *s = &p.sigs[i];
    if (s->is_extern)
      continue;
    mfunc_t *mf = &p.mod->funcs[p.mod->nfuncs++];
    nyr_func_init(&mf->func);
    mf->name = strdup(s->name);
    mf->symbol = strdup(s->symbol);
    ok = lower_function(&p, s, &mf->func);
  }
  if (ok && p.failed)
    ok = false;
  for (size_t i = 0; i < p.nsigs; ++i) {
    free(p.sigs[i].name);
    free(p.sigs[i].symbol);
    free(p.sigs[i].params);
    for (size_t k = 0; k < p.sigs[i].nparams; ++k)
      free(p.sigs[i].pnames[k]);
    free(p.sigs[i].pnames);
  }
  free(p.sigs);
  for (size_t i = 0; i < p.nt; ++i)
    free(p.t[i].sval);
  free(p.t);
  if (!ok) {
    if (err && err_len && !err[0])
      snprintf(err, err_len, "%s: error: out of memory", path ? path : "<nyr>");
    nyr_text_module_free(p.mod);
    return NULL;
  }
  return p.mod;
}

size_t nyr_text_function_count(const nyr_text_module_t *m) {
  return m ? m->nfuncs : 0;
}
const char *nyr_text_function_name(const nyr_text_module_t *m, size_t i) {
  return m && i < m->nfuncs ? m->funcs[i].name : NULL;
}
const char *nyr_text_function_symbol(const nyr_text_module_t *m, size_t i) {
  return m && i < m->nfuncs ? m->funcs[i].symbol : NULL;
}
nyr_func_t *nyr_text_function(nyr_text_module_t *m, size_t i) {
  return m && i < m->nfuncs ? &m->funcs[i].func : NULL;
}
long nyr_text_function_find(const nyr_text_module_t *m, const char *name) {
  for (size_t i = 0; m && name && i < m->nfuncs; ++i)
    if (!strcmp(m->funcs[i].name, name))
      return (long)i;
  return -1;
}

bool nyr_text_has_data(const nyr_text_module_t *m) {
  if (m && m->asm_len)
    return true;
  for (size_t i = 0; m && i < m->ndata; ++i)
    if (m->data[i].kind != DATA_CONST)
      return true;
  return false;
}

bool nyr_text_write_data(const nyr_text_module_t *m, FILE *out) {
  if (!m || !out)
    return false;
  fprintf(out, "# NYR text module data\n");
  for (size_t i = 0; i < m->ndata; ++i) {
    const data_t *d = &m->data[i];
    if (d->kind == DATA_CONST || d->kind == DATA_RAW)
      continue;
    bool str = d->kind == DATA_STRING;
    fprintf(out,
            "\t.section %s\n\t.globl %s\n\t.hidden %s\n\t.p2align 3\n%s:\n",
            str ? ".rodata" : ".data", d->symbol, d->symbol, d->symbol);
    if (str) {
      fprintf(out, "\t.byte ");
      for (size_t k = 0; k <= d->len; ++k)
        fprintf(out, "%s%u", k ? "," : "",
                k < d->len ? (unsigned)(unsigned char)d->bytes[k] : 0u);
      fprintf(out, "\n");
    } else if (d->ty == TY_F64) {
      uint64_t bits;
      memcpy(&bits, &d->fval, sizeof(bits));
      fprintf(out, "\t.quad 0x%016" PRIx64 "\n", bits);
    } else if (d->ty == TY_F32) {
      float f = (float)d->fval;
      uint32_t bits;
      memcpy(&bits, &f, sizeof(bits));
      fprintf(out, "\t.long 0x%08" PRIx32 "\n\t.long 0\n", bits);
    } else
      fprintf(out, "\t.quad %" PRId64 "\n", d->ival);
  }
  if (m->asm_len)
    fwrite(m->asm_text, 1, m->asm_len, out);
  fprintf(out, "\t.section .note.GNU-stack,\"\",@progbits\n");
  return !ferror(out);
}

/*
 * ---------------------------------------------------------------- printer
 */

static void print_val(FILE *out, int v) {
  if (v >= 0)
    fprintf(out, "%%v%d", v);
  else
    fprintf(out, "0");
}

static void print_flags(FILE *out, unsigned flags, nyr_op_t op) {
  /*
   * NYIR_INST_F_* bits overlap between instruction families; print the
   * names that apply to this opcode and the rest as raw numbers.
   */
  bool mem = op == NYIR_LOAD_I64 || op == NYIR_STORE_I64;
  bool call = op == NYIR_CALL;
  unsigned rest = flags;
  for (size_t k = 0; k < sizeof(IR_FLAGS) / sizeof(IR_FLAGS[0]); ++k) {
    unsigned f = IR_FLAGS[k].flag;
    const char *n = IR_FLAGS[k].name;
    bool fits = !strcmp(n, "byte") || !strcmp(n, "u16") || !strcmp(n, "u32") ||
                        !strcmp(n, "volatile") || !strcmp(n, "fs") ||
                        !strcmp(n, "gs")
                    ? mem
                : !strcmp(n, "extern") || !strcmp(n, "sret") ||
                        !strcmp(n, "indirect") || !strcmp(n, "variadic")
                    ? call
                    : false;
    if (!strcmp(n, "u8"))
      fits = false; /* alias of byte */
    if (fits && (rest & f) == f) {
      fprintf(out, " !%s", n);
      rest &= ~f;
    }
  }
  if (rest)
    fprintf(out, " !%u", rest);
}

static void print_f(FILE *out, double d) {
  char num[64];
  snprintf(num, sizeof(num), "%.17g", d);
  fprintf(out, "%s%s", num, strpbrk(num, ".en") ? "" : ".0");
}

bool nyr_text_print_function(FILE *out, const nyr_func_t *f, const char *name) {
  if (!out || !f)
    return false;
  fprintf(out, "fn %s(", name ? name : "f");
  for (size_t i = 0; i < f->param_count; ++i)
    fprintf(out, "%s%s p%zu", i ? ", " : "",
            f->param_types[i] == NYIR_PARAM_F64   ? "f64"
            : f->param_types[i] == NYIR_PARAM_F32 ? "f32"
                                                  : "int",
            i);
  const char *ret = "int";
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_RET)
      ret = f->data[i].scalar_class == NYIR_SCALAR_F64   ? "f64"
            : f->data[i].scalar_class == NYIR_SCALAR_F32 ? "f32"
                                                         : "int";
  fprintf(out, ") %s {\n", ret);
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    nyr_op_t op = in->op;
    if (op == NYIR_NOP)
      continue;
    if (op == NYIR_LABEL) {
      fprintf(out, "@L%" PRId64 "\n", in->imm);
      continue;
    }
    fprintf(out, "  ");
    if (in->dst >= 0)
      fprintf(out, "%%v%d = ", in->dst);
    fprintf(out, "%s", nyr_op_name(op));
    switch (op) {
    case NYIR_CMP_I64:
    case NYIR_CMP_F64:
    case NYIR_CMP_F32:
      fprintf(out, " %s ", (unsigned)in->cmp < 6 ? CMP_NAMES[in->cmp] : "eq");
      print_val(out, in->a);
      fprintf(out, ", ");
      print_val(out, in->b);
      break;
    case NYIR_PHI:
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        fprintf(out, "%s[", k ? ", " : " ");
        print_val(out, in->phi_incoming[k].value);
        fprintf(out, ", @L%" PRId64 "]", in->phi_incoming[k].predecessor_label);
      }
      break;
    case NYIR_CALL: {
      int args[NYIR_CALL_MAX_ARGS];
      int argc = 0;
      char e[64];
      bool ind = (in->flags & NYIR_INST_F_INDIRECT) != 0;
      if (!nyr_call_args(in, f->next_value, args, NYIR_CALL_MAX_ARGS, &argc, e,
                         sizeof(e)))
        argc = 0;
      int k = 0;
      if (ind && argc) {
        fprintf(out, " ");
        print_val(out, args[k++]);
      } else
        fprintf(out, " $%s", in->symbol ? in->symbol : "?");
      fprintf(out, "(");
      for (int a = k; a < argc; ++a) {
        fprintf(out, "%s", a > k ? ", " : "");
        print_val(out, args[a]);
      }
      fprintf(out, ")");
      break;
    }
    case NYIR_BR:
      fprintf(out, " @L%" PRId64, in->imm);
      break;
    case NYIR_BR_IF:
      fprintf(out, " ");
      print_val(out, in->a);
      fprintf(out, ", @L%" PRId64, in->imm);
      break;
    case NYIR_CONST_I64:
      fprintf(out, " %" PRId64, in->imm);
      break;
    case NYIR_CONST_F64: {
      double d;
      memcpy(&d, &in->imm, sizeof(d));
      if (d == d && d - d == 0) {
        fprintf(out, " ");
        print_f(out, d);
      } else
        fprintf(out, " 0x%016" PRIx64 " !bits", (uint64_t)in->imm);
      break;
    }
    case NYIR_CONST_F32: {
      uint32_t bits = (uint32_t)in->imm;
      float fv;
      memcpy(&fv, &bits, sizeof(fv));
      if (fv == fv && fv - fv == 0) {
        fprintf(out, " ");
        print_f(out, fv);
      } else
        fprintf(out, " 0x%08" PRIx32 " !bits", bits);
      break;
    }
    case NYIR_LOAD_LOCAL:
    case NYIR_ADDR_LOCAL:
      fprintf(out, " %" PRId64, in->imm);
      break;
    case NYIR_STORE_LOCAL:
      fprintf(out, " %" PRId64 ", ", in->imm);
      print_val(out, in->a);
      break;
    case NYIR_STORE_I64:
      fprintf(out, " ");
      print_val(out, in->a);
      fprintf(out, ", ");
      print_val(out, in->c);
      if (in->imm)
        fprintf(out, ", %" PRId64, in->imm);
      break;
    case NYIR_ADDR_SYMBOL:
      fprintf(out, " $%s", in->symbol ? in->symbol : "?");
      break;
    case NYIR_RET:
      if (in->a >= 0) {
        fprintf(out, " ");
        print_val(out, in->a);
      }
      break;
    default: {
      const int ops[] = {in->a, in->b, in->c, in->d, in->e, in->f};
      int last = -1;
      for (int k = 0; k < 6; ++k)
        if (ops[k] >= 0)
          last = k;
      for (int k = 0; k <= last; ++k) {
        fprintf(out, "%s", k ? ", " : " ");
        print_val(out, ops[k]);
      }
      if (in->imm)
        fprintf(out, "%s%s%" PRId64, last >= 0 ? ", " : " ",
                op == NYIR_LOAD_I64 ? "" : "#", in->imm);
      break;
    }
    }
    print_flags(out, in->flags, op);
    const char *n = nyr_op_name(op);
    size_t nl = strlen(n);
    bool named_float =
        nl > 4 && (!strcmp(n + nl - 4, ".f64") || !strcmp(n + nl - 4, ".f32"));
    if (!named_float && in->dst >= 0 && strncmp(n, "vec", 3) &&
        (in->scalar_class == NYIR_SCALAR_F64 ||
         in->scalar_class == NYIR_SCALAR_F32))
      fprintf(out, " -> %s",
              in->scalar_class == NYIR_SCALAR_F64 ? "f64" : "f32");
    fprintf(out, "\n");
  }
  fprintf(out, "}\n");
  return !ferror(out);
}
