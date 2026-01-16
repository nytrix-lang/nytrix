#include "lex/lexer.h"
#include "priv.h"
#include <readline/readline.h>
#include <stdio.h>
#include <string.h>

// Color definitions
static const char *CLR_RESET = "\033[0m";
static const char *CLR_KEYWORD = "\033[1;36m";  // Bright cyan for keywords
static const char *CLR_BUILTIN = "\033[1;35m";  // Bright magenta for builtins
static const char *CLR_STRING = "\033[33m";     // Yellow for strings
static const char *CLR_NUMBER = "\033[32m";     // Green for numbers
static const char *CLR_OPERATOR = "\033[35m";   // Magenta for operators
static const char *CLR_FUNCTION = "\033[1;34m"; // Bright blue for functions
static const char *CLR_PAREN = "\033[37m";      // White for parens/brackets

// Check if identifier looks like a function call
static int is_function_call(const char *line, size_t pos) {
  // Look ahead for '('
  while (line[pos] && (line[pos] == ' ' || line[pos] == '\t'))
    pos++;
  return line[pos] == '(';
}

// Enhanced syntax highlighter using lexer_t
void repl_highlight_line(const char *line) {
  if (!line || !*line) {
    fputs(line ? line : "", stdout);
    return;
  }
  // Handle REPL commands specially
  if (line[0] == ':') {
    printf("%s%s%s", CLR_BUILTIN, line, CLR_RESET);
    return;
  }
  lexer_t lx;
  lexer_init(&lx, line, "<repl>");
  size_t pos = 0;
  token_t tok;
  while (1) {
    tok = lexer_next(&lx);
    // Print any whitespace/text before this token_t
    while (pos < (size_t)(tok.lexeme - line)) {
      fputc(line[pos++], stdout);
    }
    if (tok.kind == NY_T_EOF)
      break;
    const char *color = NULL;
    // Determine color based on token_t type
    switch (tok.kind) {
    case NY_T_STRING:
      color = CLR_STRING;
      break;
    case NY_T_NUMBER:
      color = CLR_NUMBER;
      break;
    // Keywords
    case NY_T_FN:
    case NY_T_IF:
    case NY_T_ELSE:
    case NY_T_ELIF:
    case NY_T_WHILE:
    case NY_T_FOR:
    case NY_T_IN:
    case NY_T_RETURN:
    case NY_T_USE:
    case NY_T_TRY:
    case NY_T_CATCH:
    case NY_T_BREAK:
    case NY_T_CONTINUE:
    case NY_T_LAMBDA:
    case NY_T_DEFER:
    case NY_T_UNDEF:
    case NY_T_NIL:
    case NY_T_TRUE:
    case NY_T_FALSE:
    case NY_T_GOTO:
      color = CLR_KEYWORD;
      break;
    case NY_T_IDENT:
      // Check if it looks like a function call
      if (is_function_call(line, pos + tok.len)) {
        color = CLR_FUNCTION;
      }
      break;
    // Operators
    case NY_T_PLUS:
    case NY_T_MINUS:
    case NY_T_STAR:
    case NY_T_SLASH:
    case NY_T_PERCENT:
    case NY_T_BITOR:
    case NY_T_BITAND:
    case NY_T_BITXOR:
    case NY_T_BITNOT:
    case NY_T_LSHIFT:
    case NY_T_RSHIFT:
    case NY_T_EQ:
    case NY_T_NEQ:
    case NY_T_LT:
    case NY_T_LE:
    case NY_T_GT:
    case NY_T_GE:
    case NY_T_ASSIGN:
    case NY_T_AND:
    case NY_T_OR:
    case NY_T_NOT:
    case NY_T_ARROW:
      color = CLR_OPERATOR;
      break;
    // Brackets
    case NY_T_LPAREN:
    case NY_T_RPAREN:
    case NY_T_LBRACK:
    case NY_T_RBRACK:
    case NY_T_LBRACE:
    case NY_T_RBRACE:
      color = CLR_PAREN;
      break;
    default:
      break;
    }
    // Print the token_t with color
    if (color) {
      printf("%s", color);
    }
    fwrite(tok.lexeme, 1, tok.len, stdout);
    if (color) {
      printf("%s", CLR_RESET);
    }
    pos += tok.len;
  }
  // Print any remaining text
  while (line[pos]) {
    fputc(line[pos++], stdout);
  }
}
