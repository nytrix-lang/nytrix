#include "code/lang/c/c.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *test, ny_ctok_t tok, const char *expected) {
  fprintf(stderr, "%s: expected %s, got %s '%.*s' at %u:%u%s%s\n", test,
          expected, ny_ctok_kind_name(tok.kind), (int)tok.len, tok.start,
          tok.line, tok.col, tok.error ? ": " : "", tok.error ? tok.error : "");
  return 0;
}

static int expect(ny_lexer_t *lexer, ny_ctok_kind_t kind, const char *text,
                  unsigned line, unsigned col, const char *test) {
  ny_ctok_t tok = ny_lex_next(lexer);
  if (tok.kind != kind || !text || tok.len != strlen(text) ||
      memcmp(tok.start, text, tok.len) != 0 || tok.line != line ||
      tok.col != col)
    return fail(test, tok, text ? text : "token");
  return 1;
}

static int test_literals_numbers_and_punctuators(void) {
  const char source[] = "u8\"a\\\"b\" 0x1.fp+2 1e-3 42UL >>= ... %:%: <:";
  ny_lexer_t lexer;
  ny_lex_init(&lexer, source, sizeof(source) - 1);
  if (!expect(&lexer, NY_CTOK_STRING, "u8\"a\\\"b\"", 1, 1, "u8 string") ||
      !expect(&lexer, NY_CTOK_NUMBER, "0x1.fp+2", 1, 10, "hex float") ||
      !expect(&lexer, NY_CTOK_NUMBER, "1e-3", 1, 19, "decimal exponent") ||
      !expect(&lexer, NY_CTOK_NUMBER, "42UL", 1, 24, "integer suffix") ||
      !expect(&lexer, NY_CTOK_PUNCT, ">>=", 1, 29, "shift assignment") ||
      !expect(&lexer, NY_CTOK_PUNCT, "...", 1, 33, "ellipsis") ||
      !expect(&lexer, NY_CTOK_PUNCT, "%:%:", 1, 37, "four-byte digraph") ||
      !expect(&lexer, NY_CTOK_PUNCT, "<:", 1, 42, "two-byte digraph"))
    return 0;
  return expect(&lexer, NY_CTOK_EOF, "", 1, 44, "end of input");
}

static int test_digit_separators(void) {
  const char source[] = "1'000 0x1'0000 0b1'0101 3.14'159 1'x'";
  ny_lexer_t lexer;
  ny_lex_init(&lexer, source, sizeof(source) - 1);
  if (!expect(&lexer, NY_CTOK_NUMBER, "1'000", 1, 1, "decimal separator") ||
      !expect(&lexer, NY_CTOK_NUMBER, "0x1'0000", 1, 7, "hex separator") ||
      !expect(&lexer, NY_CTOK_NUMBER, "0b1'0101", 1, 16, "binary separator") ||
      !expect(&lexer, NY_CTOK_NUMBER, "3.14'159", 1, 25, "float separator") ||
      !expect(&lexer, NY_CTOK_NUMBER, "1", 1, 34, "before quote-char") ||
      !expect(&lexer, NY_CTOK_CHAR, "'x'", 1, 35, "quote stays a char literal"))
    return 0;
  return expect(&lexer, NY_CTOK_EOF, "", 1, 38, "end of input");
}

static int test_comments_and_newline_locations(void) {
  const char source[] = "a/*x\n*/\r\nb//tail\rc";
  ny_lexer_t lexer;
  ny_lex_init(&lexer, source, sizeof(source) - 1);
  return expect(&lexer, NY_CTOK_IDENT, "a", 1, 1, "first identifier") &&
         expect(&lexer, NY_CTOK_IDENT, "b", 3, 1, "CRLF location") &&
         expect(&lexer, NY_CTOK_IDENT, "c", 4, 1, "CR location") &&
         expect(&lexer, NY_CTOK_EOF, "", 4, 2, "end after CR");
}

static int test_preprocessor_line_rules(void) {
  const char source[] =
      "token # inline\n /*comment*/ #define VAL \\\r\n 7\r\nnext";
  ny_lexer_t lexer;
  ny_lex_init(&lexer, source, sizeof(source) - 1);
  if (!expect(&lexer, NY_CTOK_IDENT, "token", 1, 1, "code before hash") ||
      !expect(&lexer, NY_CTOK_PUNCT, "#", 1, 7, "inline hash") ||
      !expect(&lexer, NY_CTOK_IDENT, "inline", 1, 9, "inline text"))
    return 0;
  ny_ctok_t directive = ny_lex_next(&lexer);
  if (directive.kind != NY_CTOK_PREPROC || directive.line != 2 ||
      directive.col != 14 || !strstr(directive.start, "#define VAL") ||
      !strstr(directive.start, " 7"))
    return fail("continued preprocessor line", directive, "#define VAL ... 7");
  return expect(&lexer, NY_CTOK_IDENT, "next", 4, 1, "after directive");
}

static int test_bounded_punctuator_and_errors(void) {
  const char op[] = {'<', '<', '='};
  ny_lexer_t lexer;
  ny_lex_init(&lexer, op, sizeof(op));
  if (!expect(&lexer, NY_CTOK_PUNCT, "<<=", 1, 1, "bounded shift token"))
    return 0;

  const char comment[] = "/* not closed";
  ny_lex_init(&lexer, comment, sizeof(comment) - 1);
  ny_ctok_t tok = ny_lex_next(&lexer);
  if (tok.kind != NY_CTOK_ERROR || !tok.error ||
      strcmp(tok.error, "unterminated block comment") != 0 || tok.line != 1 ||
      tok.col != 1)
    return fail("unterminated comment", tok, "unterminated block comment");

  const char string[] = "\"line one\nline two";
  ny_lex_init(&lexer, string, sizeof(string) - 1);
  tok = ny_lex_next(&lexer);
  if (tok.kind != NY_CTOK_ERROR || !tok.error ||
      strcmp(tok.error, "newline in string literal") != 0)
    return fail("newline in string", tok, "newline in string literal");

  const char nul[] = {'x', '\0', 'y'};
  ny_lex_init(&lexer, nul, sizeof(nul));
  if (!expect(&lexer, NY_CTOK_IDENT, "x", 1, 1, "before NUL"))
    return 0;
  tok = ny_lex_next(&lexer);
  if (tok.kind != NY_CTOK_ERROR || !tok.error ||
      strcmp(tok.error, "NUL byte in C source") != 0 || tok.col != 2)
    return fail("embedded NUL", tok, "NUL byte in C source");
  return expect(&lexer, NY_CTOK_IDENT, "y", 1, 3, "after NUL");
}

int main(void) {
  if (!test_literals_numbers_and_punctuators() ||
      !test_digit_separators() ||
      !test_comments_and_newline_locations() ||
      !test_preprocessor_line_rules() || !test_bounded_punctuator_and_errors())
    return 1;
  return 0;
}
