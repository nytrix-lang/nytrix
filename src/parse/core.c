#include "base/common.h"
#include "base/util.h"
#include "priv.h"
#include <stdarg.h>
#include <stdint.h>

static char **g_parse_diag_tbl = NULL;
static size_t g_parse_diag_cap = 0;
static size_t g_parse_diag_len = 0;
static char *g_parse_cached_file = NULL;
static char *g_parse_cached_src = NULL;

static const char *parse_load_source(const char *filename) {
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

static bool parse_extract_line(const char *src, int line,
                               const char **out_start, size_t *out_len) {
  if (!src || line <= 0 || !out_start || !out_len)
    return false;
  const char *cur = src;
  int cur_line = 1;
  while (*cur && cur_line < line) {
    if (*cur == '\n')
      cur_line++;
    cur++;
  }
  if (cur_line != line)
    return false;
  const char *start = cur;
  while (*cur && *cur != '\n')
    cur++;
  *out_start = start;
  *out_len = (size_t)(cur - start);
  return true;
}

static void parse_print_snippet(const char *filename, int line, int col,
                                size_t len) {
  if (!filename || filename[0] == '<' || line <= 0 || col <= 0)
    return;
  const char *src = parse_load_source(filename);
  if (!src)
    return;
  const char *line_start = NULL;
  size_t line_len = 0;
  if (!parse_extract_line(src, line, &line_start, &line_len))
    return;
  if (line_len == 0)
    return;

  size_t caret_col = (size_t)(col - 1);
  if (caret_col > line_len)
    caret_col = line_len;
  size_t caret_len = len ? len : 1;
  if (caret_col + caret_len > line_len)
    caret_len = line_len > caret_col ? (line_len - caret_col) : 1;

  const size_t max_len = 200;
  size_t start = 0;
  size_t end = line_len;
  bool prefix = false;
  bool suffix = false;
  if (line_len > max_len) {
    if (caret_col > max_len / 2)
      start = caret_col - max_len / 2;
    if (start + max_len > line_len)
      start = line_len - max_len;
    end = start + max_len;
    prefix = start > 0;
    suffix = end < line_len;
  }

  size_t show_len = end - start;
  char *buf = malloc(show_len + 1);
  if (!buf)
    return;
  for (size_t i = 0; i < show_len; i++) {
    char c = line_start[start + i];
    buf[i] = (c == '\t') ? ' ' : c;
  }
  buf[show_len] = '\0';

  int width = 1;
  for (int tmp = line; tmp >= 10; tmp /= 10)
    width++;

  fprintf(stderr, "  %s%*d%s | %s%s%s\n", clr(NY_CLR_GRAY), width, line,
          clr(NY_CLR_RESET), prefix ? "..." : "", buf, suffix ? "..." : "");
  size_t caret_pad = caret_col - start + (prefix ? 3 : 0);
  fprintf(stderr, "  %s%*s%s | ", clr(NY_CLR_GRAY), width, "",
          clr(NY_CLR_RESET));
  for (size_t i = 0; i < caret_pad; i++)
    fputc(' ', stderr);
  fputs(clr(NY_CLR_RED), stderr);
  for (size_t i = 0; i < caret_len; i++)
    fputc('^', stderr);
  fputs(clr(NY_CLR_RESET), stderr);
  fputc('\n', stderr);
  free(buf);
}

static uint64_t parse_diag_hash(const char *s) {
  uint64_t h = 1469598103934665603ULL;
  for (; s && *s; ++s) {
    h ^= (unsigned char)*s;
    h *= 1099511628211ULL;
  }
  return h;
}

static bool parse_diag_grow(void) {
  size_t new_cap = g_parse_diag_cap ? g_parse_diag_cap * 2 : 512;
  char **new_tbl = calloc(new_cap, sizeof(char *));
  if (!new_tbl)
    return false;
  for (size_t i = 0; i < g_parse_diag_cap; ++i) {
    char *entry = g_parse_diag_tbl[i];
    if (!entry)
      continue;
    size_t mask = new_cap - 1;
    size_t idx = (size_t)parse_diag_hash(entry) & mask;
    while (new_tbl[idx])
      idx = (idx + 1) & mask;
    new_tbl[idx] = entry;
  }
  free(g_parse_diag_tbl);
  g_parse_diag_tbl = new_tbl;
  g_parse_diag_cap = new_cap;
  return true;
}

static bool parser_diag_should_emit(const char *filename, int line, int col,
                                    const char *msg, const char *got) {
  char key[1024];
  snprintf(key, sizeof(key), "%s|%d|%d|%s|%s", filename ? filename : "<input>",
           line, col, msg ? msg : "", got ? got : "");
  if (g_parse_diag_cap == 0 && !parse_diag_grow())
    return true; // fail open
  if ((g_parse_diag_len + 1) * 3 >= g_parse_diag_cap * 2 && !parse_diag_grow())
    return true; // fail open

  size_t mask = g_parse_diag_cap - 1;
  size_t idx = (size_t)parse_diag_hash(key) & mask;
  while (g_parse_diag_tbl[idx]) {
    if (strcmp(g_parse_diag_tbl[idx], key) == 0)
      return false;
    idx = (idx + 1) & mask;
  }
  g_parse_diag_tbl[idx] = ny_strdup(key);
  g_parse_diag_len++;
  return true;
}

void parser_advance(parser_t *p) {
  p->prev = p->cur;
  p->cur = lexer_next(&p->lex);
}

bool parser_match(parser_t *p, token_kind kind) {
  if (p->cur.kind == kind) {
    parser_advance(p);
    return true;
  }
  return false;
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
  case NY_T_COLON:
    return ":";
  case NY_T_SEMI:
    return ";";
  case NY_T_DOT:
    return ".";
  case NY_T_BITOR:
    return "|";
  case NY_T_BITAND:
    return "&";
  case NY_T_BITXOR:
    return "^";
  case NY_T_LSHIFT:
    return "<<";
  case NY_T_RSHIFT:
    return ">>";
  case NY_T_BITNOT:
    return "~";
  default:
    return "?";
  }
}

static void print_error_line(parser_t *p, const char *filename, int line,
                             int col, const char *msg, const char *got,
                             const char *hint) {
  const char *out_file =
      filename ? filename : (p->filename ? p->filename : "<input>");
  if (!parser_diag_should_emit(out_file, line, col, msg, got))
    return;
  p->had_error = true;
  p->error_count++;
  p->last_error_line = line;
  p->last_error_col = col;
  snprintf(p->last_error_msg, sizeof(p->last_error_msg), "%s", msg);

  // Emacs format: filename:line:col: type: message
  fprintf(stderr, "%s:%d:%d: %serror:%s %s (got %s)\n", out_file, line, col,
          clr(NY_CLR_RED), clr(NY_CLR_RESET), msg, got);
  if (hint) {
    fprintf(stderr, "%s:%d:%d: %snote:%s %s\n", out_file, line, col,
            clr(NY_CLR_YELLOW), clr(NY_CLR_RESET), hint);
    fprintf(stderr, "  %sfix:%s %s\n", clr(NY_CLR_GREEN),
            clr(NY_CLR_RESET), hint);
  }
  parse_print_snippet(out_file, line, col, 1);

  if (p->error_limit > 0 && p->error_count >= p->error_limit) {
    fprintf(stderr, "Too many errors, aborting.\n");
    exit(1);
  }
}

static const char *token_desc(token_t tok, char *buf, size_t cap) {
  const char *kind = parser_token_name(tok.kind);
  if (tok.kind == NY_T_IDENT || tok.kind == NY_T_NUMBER ||
      tok.kind == NY_T_STRING) {
    if (!tok.lexeme) { // Add null check for lexeme
      snprintf(buf, cap, "%s '<null>'", kind);
      return buf;
    }
    size_t n = tok.len < 24 ? tok.len : 24;
    if (n > tok.len)
      n = tok.len; // Ensure n does not exceed tok.len
    snprintf(buf, cap, "%s '%.*s'%s", kind, (int)n, tok.lexeme,
             tok.len > n ? "..." : "");
    return buf;
  }
  return kind;
}

static const char *expect_hint(token_kind expected, token_t got) {
  if (expected == NY_T_SEMI && got.kind == NY_T_RBRACE)
    return "did you forget a ';' before '}'?";
  if (expected == NY_T_RPAREN && got.kind == NY_T_RBRACE)
    return "did you forget a ')' before '}'?";
  if (expected == NY_T_RBRACE && got.kind == NY_T_EOF)
    return "missing '}' before end of file";
  if (expected == NY_T_RPAREN && got.kind == NY_T_EOF)
    return "missing ')' before end of file";
  if (expected == NY_T_RBRACK && got.kind == NY_T_EOF)
    return "missing ']' before end of file";
  if (expected == NY_T_COLON && got.kind == NY_T_IDENT)
    return "use ':' after 'case'/'default' or for slices";
  if (expected == NY_T_LBRACE &&
      (got.kind == NY_T_ARROW || got.kind == NY_T_COLON))
    return "return type belongs before the opening '{'";
  if (expected == NY_T_RPAREN && got.lexeme && strcmp(got.lexeme, "->") == 0)
    return "did you forget to close the parameter list ')' before '->'?";
  if (expected == NY_T_LBRACE && got.kind == NY_T_IDENT)
    return "did you forget the '{' before the function body?";
  return NULL;
}

void parser_error(parser_t *p, token_t tok, const char *msg, const char *hint) {
  if (!hint && tok.kind == NY_T_EOF)
    hint = "check for missing ';' or unmatched brace";
  char buf[64];
  const char *got = token_desc(tok, buf, sizeof(buf));
  print_error_line(p, tok.filename, tok.line, tok.col, msg, got, hint);
}

void parser_expect(parser_t *p, token_kind kind, const char *msg,
                   const char *hint) {
  if (p->cur.kind == kind) {
    parser_advance(p);
    return;
  }
  if (!msg) {
    char def_msg[128];
    snprintf(def_msg, sizeof(def_msg), "expected %s", parser_token_name(kind));
    if (!hint)
      hint = expect_hint(kind, p->cur);
    char buf[64];
    const char *got_desc = token_desc(p->cur, buf, sizeof(buf));
    print_error_line(p, p->cur.filename, p->cur.line, p->cur.col, def_msg,
                     got_desc, hint);
  } else {
    parser_error(p, p->cur, msg, hint);
  }
}

token_t parser_peek(parser_t *p) {
  lexer_t lx = p->lex;
  return lexer_next(&lx);
}

char *parser_unescape_string(arena_t *arena, const char *cur, size_t len,
                             size_t *out_len) {
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
      case '\\':
        out[oi++] = '\\';
        break;
      case '\'':
        out[oi++] = '\'';
        break;
      case '"':
        out[oi++] = '"';
        break;
      case 'x': {
        if (cur + 2 < end) {
          char hex[3] = {cur[1], cur[2], 0};
          out[oi++] = (char)strtol(hex, NULL, 16);
          cur += 2;
        } else {
          out[oi++] = 'x';
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
        int oct = 0;
        int count = 0;
        while (count < 3 && cur < end && *cur >= '0' && *cur <= '7') {
          oct = oct * 8 + (*cur - '0');
          cur++;
          count++;
        }
        out[oi++] = (char)oct;
        cur--; // adjustment
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
  p->current_module = NULL;
  p->block_depth = 0;
  p->loop_depth = 0;
  parser_advance(p);
}

void parser_init(parser_t *p, const char *src, const char *filename) {
  arena_t *arena = (arena_t *)malloc(sizeof(arena_t));
  if (!arena) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  memset(arena, 0, sizeof(arena_t));
  parser_init_with_arena(p, src, filename, arena);
}
