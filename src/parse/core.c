#include "base/common.h"
#include "base/util.h"
#include "priv.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>

typedef struct {
  char *key;
  int count;
} parse_diag_entry_t;

static parse_diag_entry_t *g_parse_diag_tbl = NULL;
static size_t g_parse_diag_cap = 0;
static size_t g_parse_diag_len = 0;

static uint64_t parse_diag_hash(const char *s) { return ny_hash64_cstr(s); }

static char *g_parse_cached_file = NULL;
static char *g_parse_cached_src = NULL;

__attribute__((unused)) static const char *
parse_load_source(const char *filename) {
  if (!filename || filename[0] == '<')
    return NULL;
  if (g_parse_cached_file && strcmp(g_parse_cached_file, filename) == 0)
    return g_parse_cached_src;
  free(g_parse_cached_file);
  free(g_parse_cached_src);
  g_parse_cached_file = ny_strdup(filename);
  g_parse_cached_src = ny_read_file(filename);
  return g_parse_cached_src;
}

static void parse_print_snippet(parser_t *p, int real_line, int col,
                                size_t len) {
  if (!p || !p->src || real_line <= 0 || col <= 0)
    return;
  ny_print_snippet(p->src, real_line, col, len, NY_CLR_RED);
}

static bool parse_diag_grow(void) {
  size_t old_cap = g_parse_diag_cap;
  parse_diag_entry_t *old_tbl = g_parse_diag_tbl;
  g_parse_diag_cap = g_parse_diag_cap ? g_parse_diag_cap * 2 : 1024;
  g_parse_diag_tbl = calloc(g_parse_diag_cap, sizeof(parse_diag_entry_t));
  if (!g_parse_diag_tbl)
    return false;
  size_t mask = g_parse_diag_cap - 1;
  for (size_t i = 0; i < old_cap; ++i) {
    if (!old_tbl[i].key)
      continue;
    uint64_t h = parse_diag_hash(old_tbl[i].key);
    size_t idx = (size_t)h & mask;
    while (g_parse_diag_tbl[idx].key)
      idx = (idx + 1) & mask;
    g_parse_diag_tbl[idx] = old_tbl[i];
  }
  free(old_tbl);
  return true;
}

static bool parser_diag_should_emit(const char *filename, int line, int col,
                                    const char *msg, const char *got) {
  char key[1024];
  snprintf(key, sizeof(key), "%s|%d|%d|%s|%s", filename ? filename : "<input>",
           line, col, msg ? msg : "", got ? got : "");
  if (g_parse_diag_cap == 0 ||
      (g_parse_diag_len + 1) * 3 >= g_parse_diag_cap * 2) {
    if (!parse_diag_grow())
      return true;
  }
  size_t mask = g_parse_diag_cap - 1;
  size_t idx = (size_t)parse_diag_hash(key) & mask;
  while (g_parse_diag_tbl[idx].key) {
    if (strcmp(g_parse_diag_tbl[idx].key, key) == 0) {
      g_parse_diag_tbl[idx].count++;
      return g_parse_diag_tbl[idx].count <= 3;
    }
    idx = (idx + 1) & mask;
  }
  g_parse_diag_tbl[idx].key = ny_strdup(key);
  g_parse_diag_tbl[idx].count = 1;
  g_parse_diag_len++;
  return true;
}

static bool parser_intern_grow(parser_t *p) {
  size_t new_cap = p->intern_cap ? p->intern_cap * 2 : 1024;
  parser_intern_entry *new_tbl = (parser_intern_entry *)arena_alloc(
      p->arena, new_cap * sizeof(parser_intern_entry));
  if (!new_tbl)
    return false;
  memset(new_tbl, 0, new_cap * sizeof(parser_intern_entry));
  for (size_t i = 0; i < p->intern_cap; ++i) {
    parser_intern_entry *e = &p->intern_table[i];
    if (!e->str)
      continue;
    size_t mask = new_cap - 1;
    size_t idx = (size_t)e->hash & mask;
    while (new_tbl[idx].str)
      idx = (idx + 1) & mask;
    new_tbl[idx] = *e;
  }
  p->intern_table = new_tbl;
  p->intern_cap = new_cap;
  return true;
}

const char *parser_intern_hash(parser_t *p, const char *s, size_t len,
                               uint64_t hash) {
  if (!p || !s)
    return s;
  if (!p->intern_cap && !parser_intern_grow(p))
    return arena_strndup(p->arena, s, len);
  if ((p->intern_len + 1) * 3 >= p->intern_cap * 2) {
    if (!parser_intern_grow(p))
      return arena_strndup(p->arena, s, len);
  }
  if (!hash)
    hash = ny_hash64(s, len);
  size_t mask = p->intern_cap - 1;
  size_t idx = (size_t)hash & mask;
  while (p->intern_table[idx].str) {
    parser_intern_entry *e = &p->intern_table[idx];
    if (e->hash == hash && e->len == (uint32_t)len &&
        memcmp(e->str, s, len) == 0) {
      return e->str;
    }
    idx = (idx + 1) & mask;
  }
  const char *dup = arena_strndup(p->arena, s, len);
  p->intern_table[idx] =
      (parser_intern_entry){.hash = hash, .len = (uint32_t)len, .str = dup};
  p->intern_len++;
  return dup;
}

const char *parser_token_name(token_kind k) {
  switch (k) {
  case NY_T_EOF:
    return "EOF";
  case NY_T_IDENT:
    return "identifier";
  case NY_T_NUMBER:
    return "number";
  case NY_T_STRING:
    return "string";
  case NY_T_FN:
    return "fn";
  case NY_T_RETURN:
    return "return";
  case NY_T_IF:
    return "if";
  case NY_T_ELSE:
    return "else";
  case NY_T_WHILE:
    return "while";
  case NY_T_FOR:
    return "for";
  case NY_T_IN:
    return "in";
  case NY_T_TRUE:
    return "true";
  case NY_T_FALSE:
    return "false";
  case NY_T_TRY:
    return "try";
  case NY_T_CATCH:
    return "catch";
  case NY_T_USE:
    return "use";
  case NY_T_STRUCT:
    return "struct";
  case NY_T_GOTO:
    return "goto";
  case NY_T_LAMBDA:
    return "lambda";
  case NY_T_DEFER:
    return "defer";
  case NY_T_DEF:
    return "def";
  case NY_T_NIL:
    return "nil";
  case NY_T_UNDEF:
    return "undef";
  case NY_T_BREAK:
    return "break";
  case NY_T_CONTINUE:
    return "continue";
  case NY_T_ELIF:
    return "elif";
  case NY_T_ASM:
    return "asm";
  case NY_T_AS:
    return "as";
  case NY_T_MATCH:
    return "match";
  case NY_T_EMBED:
    return "embed";
  case NY_T_SIZEOF:
    return "sizeof";
  case NY_T_EXTERN:
    return "extern";
  case NY_T_MUT:
    return "mut";
  case NY_T_MODULE:
    return "module";
  case NY_T_COMPTIME:
    return "comptime";
  case NY_T_PLUS:
    return "+";
  case NY_T_MINUS:
    return "-";
  case NY_T_STAR:
    return "*";
  case NY_T_SLASH:
    return "/";
  case NY_T_PERCENT:
    return "%";
  case NY_T_EQ:
    return "==";
  case NY_T_NEQ:
    return "!=";
  case NY_T_LT:
    return "<";
  case NY_T_GT:
    return ">";
  case NY_T_LE:
    return "<=";
  case NY_T_GE:
    return ">=";
  case NY_T_AND:
    return "&&";
  case NY_T_OR:
    return "||";
  case NY_T_NOT:
    return "!";
  case NY_T_ASSIGN:
    return "=";
  case NY_T_PLUS_EQ:
    return "+=";
  case NY_T_MINUS_EQ:
    return "-=";
  case NY_T_STAR_EQ:
    return "*=";
  case NY_T_SLASH_EQ:
    return "/=";
  case NY_T_PERCENT_EQ:
    return "%=";
  case NY_T_ARROW:
    return "->";
  case NY_T_LPAREN:
    return "(";
  case NY_T_RPAREN:
    return ")";
  case NY_T_LBRACE:
    return "{";
  case NY_T_RBRACE:
    return "}";
  case NY_T_LBRACK:
    return "[";
  case NY_T_RBRACK:
    return "]";
  case NY_T_COMMA:
    return ",";
  case NY_T_DOT:
    return ".";
  case NY_T_COLON:
    return ":";
  case NY_T_SEMI:
    return ";";
  case NY_T_QUESTION:
    return "?";
  case NY_T_AT:
    return "@";
  case NY_T_PLUS_PLUS:
    return "++";
  case NY_T_MINUS_MINUS:
    return "--";
  default:
    return "unknown";
  }
}

static void print_error_line(parser_t *p, const char *filename, int line,
                             int real_line, int col, const char *msg,
                             const char *got, const char *hint) {
  const char *out_file =
      filename ? filename : (p->filename ? p->filename : "<input>");
  if (!parser_diag_should_emit(out_file, line, col, msg, got))
    return;
  p->had_error = true;
  p->error_count++;
  p->last_error_line = line;
  p->last_error_col = col;
  snprintf(p->last_error_msg, sizeof(p->last_error_msg), "%s", msg);
  fprintf(stderr, "%s:%d:%d: %serror:%s %s (got %s)\n", out_file, line, col,
          clr(NY_CLR_RED), clr(NY_CLR_RESET), msg, got);
  if (hint) {
    fprintf(stderr, "%s:%d:%d: %snote:%s %s\n", out_file, line, col,
            clr(NY_CLR_YELLOW), clr(NY_CLR_RESET), hint);
    fprintf(stderr, "  %sfix:%s %s\n", clr(NY_CLR_GREEN), clr(NY_CLR_RESET),
            hint);
  }
  parse_print_snippet(p, real_line, col, 1);
  if (p->error_limit > 0 && p->error_count >= p->error_limit) {
    fprintf(stderr, "Too many errors, aborting.\n");
    exit(1);
  }
}

static const char *token_desc(token_t tok, char *buf, size_t cap) {
  const char *kind = parser_token_name(tok.kind);
  if (tok.kind == NY_T_IDENT || tok.kind == NY_T_NUMBER ||
      tok.kind == NY_T_STRING) {
    if (!tok.lexeme) {
      snprintf(buf, cap, "%s '<null>'", kind);
      return buf;
    }
    size_t n = tok.len < 24 ? tok.len : 24;
    if (n > tok.len)
      n = tok.len;
    snprintf(buf, cap, "%s '%.*s'%s", kind, (int)n, tok.lexeme,
             tok.len > n ? "..." : "");
    return buf;
  }
  return kind;
}

void parser_error(parser_t *p, token_t tok, const char *msg, const char *hint) {
  if (!hint && tok.kind == NY_T_EOF)
    hint = "check for missing ';' or unmatched brace";
  char buf[64];
  const char *got = token_desc(tok, buf, sizeof(buf));
  print_error_line(p, tok.filename, tok.line, tok.real_line, tok.col, msg, got,
                   hint);
}

void parser_expect_slow(parser_t *p, token_kind kind, const char *msg,
                        const char *hint) {
  if (p->cur.kind == kind) {
    parser_advance(p);
    return;
  }
  if (!msg) {
    char def_msg[128];
    snprintf(def_msg, sizeof(def_msg), "expected %s", parser_token_name(kind));
    if (!hint) {
      // Just a simple hint for now
      hint = "check syntax";
    }
    char buf[64];
    const char *got_desc = token_desc(p->cur, buf, sizeof(buf));
    print_error_line(p, p->cur.filename, p->cur.line, p->cur.real_line,
                     p->cur.col, def_msg, got_desc, hint);
  } else {
    parser_error(p, p->cur, msg, hint);
  }
  if (p->cur.kind != NY_T_EOF)
    parser_advance(p);
}

token_t parser_peek(parser_t *p) {
  lexer_t lx = p->lex;
  return lexer_next(&lx);
}

char *parser_unescape_string(arena_t *arena, const char *cur, size_t len,
                             size_t *out_len) {
  if (len == SIZE_MAX)
    return NULL;
  const char *end = cur + len;
  char *out = arena_alloc(arena, len + 1);
  size_t oi = 0;
  while (cur < end) {
    if (*cur == '\\' && cur + 1 < end) {
      cur++;
      switch (*cur) {
      case 'n':
        out[oi++] = '\n';
        break;
      case 't':
        out[oi++] = '\t';
        break;
      case 'r':
        out[oi++] = '\r';
        break;
      case 'v':
        out[oi++] = '\v';
        break;
      case 'f':
        out[oi++] = '\f';
        break;
      case 'a':
        out[oi++] = '\a';
        break;
      case 'b':
        out[oi++] = '\b';
        break;
      case 'e':
        out[oi++] = '\x1b';
        break;
      case '"':
        out[oi++] = '"';
        break;
      case '\'':
        out[oi++] = '\'';
        break;
      case '\\':
        out[oi++] = '\\';
        break;
      case 'x': {
        if (cur + 2 < end) {
          char hex[3] = {cur[1], cur[2], 0};
          out[oi++] = (char)strtol(hex, NULL, 16);
          cur += 2;
        }
        break;
      }
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7': {
        char oct_s[4] = {0};
        int oct_len = 0;
        while (oct_len < 3 && cur < end && *cur >= '0' && *cur <= '7') {
          oct_s[oct_len++] = *cur++;
        }
        int oct = (int)strtol(oct_s, NULL, 8);
        if (oct > 255) {
          // NY_LOG_WARN("octal escape sequence out of range: %s", oct_s);
        }
        out[oi++] = (char)oct;
        cur--;
        break;
      }
      default:
        out[oi++] = *cur;
        break;
      }
      cur++;
      continue;
    }
    out[oi++] = *cur++;
  }
  out[oi] = '\0';
  if (out_len)
    *out_len = oi;
  return out;
}

const char *parser_decode_string(parser_t *p, token_t tok, size_t *out_len) {
  const char *lex = tok.lexeme;
  size_t len = tok.len;
  bool triple = len >= 6 && lex[0] == lex[1] && lex[1] == lex[2];
  size_t head = triple ? 3 : 1;
  size_t tail = triple ? 3 : 1;
  if (len < head + tail) {
    if (out_len)
      *out_len = 0;
    return "";
  }
  const char *cur = lex + head;
  size_t inner_len = len - head - tail;
  return parser_unescape_string(p->arena, cur, inner_len, out_len);
}

void parser_init_with_arena(parser_t *p, const char *src, const char *filename,
                            arena_t *arena_ptr) {
  memset(p, 0, sizeof(parser_t));
  p->src = src;
  p->filename = filename ? filename : "<input>";
  lexer_init(&p->lex, src, p->filename);
  p->arena = arena_ptr;
  p->had_error = false;
  p->error_count = 0;
  p->error_limit = 10;
  p->error_ctx = NULL;
  p->last_error_line = 0;
  p->last_error_col = 0;
  p->last_error_msg[0] = '\0';
  p->block_depth = 0;
  p->loop_depth = 0;
  parser_advance(p);
}

void parser_init(parser_t *p, const char *src, const char *filename) {
  parser_init_with_arena(p, src, filename, NULL);
}
