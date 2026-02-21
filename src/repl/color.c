#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/loader.h"
#include "base/util.h"
#include "lex/lexer.h"
#include "priv.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)
#if defined(__APPLE__)
#if __has_include(<readline/readline.h>)
#include <readline/readline.h>
#elif __has_include(<editline/readline.h>)
#include <editline/readline.h>
#define NYTRIX_LIBEDIT 1
#else
#define NYTRIX_NO_READLINE 1
#endif
#else
#include <readline/readline.h>
#endif
#endif

#if !defined(NYTRIX_DISABLE_READLINE_EXTRA)
#if defined(NYTRIX_LIBEDIT)
#define NYTRIX_DISABLE_READLINE_EXTRA 1
#else
#define NYTRIX_DISABLE_READLINE_EXTRA 0
#endif
#endif

static const char *CLR_RESET = "\033[0m";
static const char *CLR_KEYWORD = "\033[1;35m";  // Bold Magenta
static const char *CLR_BUILTIN = "\033[1;36m";  // Bold Cyan
static const char *CLR_STRING = "\033[0;32m";   // Green (dim)
static const char *CLR_NUMBER = "\033[33m";     // Yellow
static const char *CLR_COMMENT = "\033[90m";    // Gray
static const char *CLR_FUNCTION = "\033[1;34m"; // Bold Blue
static const char *CLR_OPERATOR = "\033[1;37m"; // Bold White
static const char *CLR_PAREN = "\033[37m";      // White
static const char *CLR_MEMBER = "\033[36m";     // Cyan
static const char *CLR_MATCH = "\033[1;33m";    // Bold Yellow
static const char *CLR_TYPE = "\033[1;32m";     // Bold Green

static int is_color_enabled(void) {
  const char *plain = getenv("NYTRIX_REPL_PLAIN");
  if (!color_enabled())
    return 0;
  return !(plain && plain[0] != '0');
}

static int find_matching_paren(const char *line, int pos) {
  if (pos < 0 || !line[pos])
    return -1;
  char c = line[pos];
  char open, close;
  int dir;
  if (c == '(') {
    open = '(';
    close = ')';
    dir = 1;
  } else if (c == ')') {
    open = ')';
    close = '(';
    dir = -1;
  } else if (c == '[') {
    open = '[';
    close = ']';
    dir = 1;
  } else if (c == ']') {
    open = ']';
    close = '[';
    dir = -1;
  } else if (c == '{') {
    open = '{';
    close = '}';
    dir = 1;
  } else if (c == '}') {
    open = '}';
    close = '{';
    dir = -1;
  } else
    return -1;

  int depth = 1;
  int i = pos + dir;
  while (i >= 0 && line[i]) {
    if (line[i] == open)
      depth++;
    else if (line[i] == close)
      depth--;
    if (depth == 0)
      return i;
    i += dir;
  }
  return -1;
}

static int is_fn_call(const char *line, int pos) {
  while (line[pos] && (line[pos] == ' ' || line[pos] == '\t'))
    pos++;
  return line[pos] == '(';
}

static int is_known_name(const char *name, size_t len, int *out_kind) {
  if (!g_repl_docs)
    return 0;

  int suffix_match_idx = -1;

  for (size_t i = 0; i < g_repl_docs->len; i++) {
    const char *en = g_repl_docs->data[i].name;
    size_t elen = strlen(en);

    // Exact match
    if (elen == len && memcmp(en, name, len) == 0) {
      if (out_kind)
        *out_kind = g_repl_docs->data[i].kind;
      return 1;
    }

    // Suffix match (e.g. "add" matching "std.core.add")
    if (elen > len && en[elen - len - 1] == '.' &&
        memcmp(en + elen - len, name, len) == 0) {
      suffix_match_idx = (int)i;
    }

    // Prefix match (e.g. "std.core.add" matching "add" if 'name' is the full
    // one)
    if (len > elen && name[len - elen - 1] == '.' &&
        memcmp(name + len - elen, en, elen) == 0) {
      suffix_match_idx = (int)i;
    }
  }

  if (suffix_match_idx != -1) {
    if (out_kind)
      *out_kind = g_repl_docs->data[suffix_match_idx].kind;
    return 1;
  }

  return 0;
}

static int is_keyword(const char *s, size_t len) {
  // Optimization: check first char
  switch (s[0]) {
  case 'a':
    if (len == 2 && !memcmp(s, "as", 2))
      return 1;
    if (len == 3 && !memcmp(s, "asm", 3))
      return 1;
    break;
  case 'b':
    if (len == 5 && !memcmp(s, "break", 5))
      return 1;
    break;
  case 'c':
    if (len == 5 && !memcmp(s, "catch", 5))
      return 1;
    if (len == 8 && !memcmp(s, "continue", 8))
      return 1;
    if (len == 8 && !memcmp(s, "comptime", 8))
      return 1;
    break;
  case 'd':
    if (len == 3 && !memcmp(s, "def", 3))
      return 1;
    if (len == 5 && !memcmp(s, "defer", 5))
      return 1;
    if (len == 5 && !memcmp(s, "class", 5))
      return 1;
    break;
  case 'e':
    if (len == 4 && !memcmp(s, "else", 4))
      return 1;
    if (len == 4 && !memcmp(s, "elif", 4))
      return 1;
    if (len == 5 && !memcmp(s, "embed", 5))
      return 1;
    if (len == 4 && !memcmp(s, "enum", 4))
      return 1;
    if (len == 6 && !memcmp(s, "extern", 6))
      return 1;
    break;
  case 'f':
    if (len == 2 && !memcmp(s, "fn", 2))
      return 1;
    if (len == 3 && !memcmp(s, "for", 3))
      return 1;
    if (len == 5 && !memcmp(s, "false", 5))
      return 1;
    break;
  case 'g':
    if (len == 4 && !memcmp(s, "goto", 4))
      return 1;
    break;
  case 'i':
    if (len == 2 && !memcmp(s, "if", 2))
      return 1;
    if (len == 2 && !memcmp(s, "in", 2))
      return 1;
    break;
  case 'l':
    if (len == 6 && !memcmp(s, "lambda", 6))
      return 1;
    if (len == 6 && !memcmp(s, "layout", 6))
      return 1;
    break;
  case 'm':
    if (len == 5 && !memcmp(s, "match", 5))
      return 1;
    if (len == 6 && !memcmp(s, "module", 6))
      return 1;
    if (len == 3 && !memcmp(s, "mut", 3))
      return 1;
    break;
  case 'n':
    if (len == 3 && !memcmp(s, "nil", 3))
      return 1;
    break;
  case 'r':
    if (len == 6 && !memcmp(s, "return", 6))
      return 1;
    break;
  case 't':
    if (len == 3 && !memcmp(s, "try", 3))
      return 1;
    if (len == 4 && !memcmp(s, "true", 4))
      return 1;
    break;
  case 'u':
    if (len == 3 && !memcmp(s, "use", 3))
      return 1;
    if (len == 5 && !memcmp(s, "undef", 5))
      return 1;
    break;
  case 'w':
    if (len == 5 && !memcmp(s, "while", 5))
      return 1;
    break;
  case 's':
    if (len == 6 && !memcmp(s, "struct", 6))
      return 1;
    break;
  }
  return 0;
}

static void append_str(char **buf, size_t *size, size_t *cap, const char *s) {
  size_t len = strlen(s);
  if (*size + len >= *cap) {
    *cap = (*size + len + 1024) * 2;
    *buf = realloc(*buf, *cap);
  }
  memcpy(*buf + *size, s, len);
  *size += len;
  (*buf)[*size] = '\0';
}

static void append_n(char **buf, size_t *size, size_t *cap, const char *s,
                     size_t n) {
  if (*size + n >= *cap) {
    *cap = (*size + n + 1024) * 2;
    *buf = realloc(*buf, *cap);
  }
  memcpy(*buf + *size, s, n);
  *size += n;
  (*buf)[*size] = '\0';
}

void repl_highlight_line_ex(const char *line, int cursor_pos,
                            const char *ml_prompt) {
  if (!line || !*line)
    return;
  if (!is_color_enabled()) {
    fputs(line, stdout);
    return;
  }

  if (line[0] == ':') {
    printf("%s%s%s", CLR_BUILTIN, line, CLR_RESET);
    return;
  }

  static char *db = NULL;
  static size_t db_cap = 0;
  size_t db_size = 0;
  if (!db) {
    db_cap = 4096;
    db = malloc(db_cap);
  }

  int target_pos = -1, match_pos = -1;
  size_t line_len = strlen(line);
  if (cursor_pos >= 0 && (size_t)cursor_pos < line_len) {
    target_pos = cursor_pos;
    match_pos = find_matching_paren(line, target_pos);
  }
  if (match_pos == -1 && cursor_pos > 0) {
    target_pos = cursor_pos - 1;
    match_pos = find_matching_paren(line, target_pos);
  }

  size_t i = 0;
  while (line[i]) {
    if ((int)i == target_pos || (int)i == match_pos) {
      append_str(&db, &db_size, &db_cap, CLR_MATCH);
      append_n(&db, &db_size, &db_cap, line + i, 1);
      append_str(&db, &db_size, &db_cap, CLR_RESET);
      i++;
      continue;
    }

    if (line[i] == '"' ||
        (line[i] == 'f' && line[i + 1] == '"' &&
         (i == 0 || !isalnum((unsigned char)line[i - 1]))) ||
        line[i] == '\'') {
      char q = line[i];
      if (q == 'f')
        q = '"';
      append_str(&db, &db_size, &db_cap, CLR_STRING);
      if (line[i] == 'f')
        append_n(&db, &db_size, &db_cap, line + i++, 1);
      append_n(&db, &db_size, &db_cap, line + i++, 1);
      while (line[i] && line[i] != q) {
        if (line[i] == '\\' && line[i + 1])
          append_n(&db, &db_size, &db_cap, line + i++, 1);
        append_n(&db, &db_size, &db_cap, line + i++, 1);
      }
      if (line[i] == q)
        append_n(&db, &db_size, &db_cap, line + i++, 1);
      append_str(&db, &db_size, &db_cap, CLR_RESET);
      continue;
    }

    if (line[i] == ';') {
      append_str(&db, &db_size, &db_cap, CLR_COMMENT);
      const char *eol = strchr(line + i, '\n');
      if (eol) {
        size_t comment_len = (size_t)(eol - (line + i));
        append_n(&db, &db_size, &db_cap, line + i, comment_len);
        i += comment_len;
      } else {
        append_str(&db, &db_size, &db_cap, line + i);
        append_str(&db, &db_size, &db_cap, CLR_RESET);
        break;
      }
      append_str(&db, &db_size, &db_cap, CLR_RESET);
      continue;
    }

    if (isdigit((unsigned char)line[i])) {
      append_str(&db, &db_size, &db_cap, CLR_NUMBER);
      while (line[i] && (isalnum((unsigned char)line[i]) || line[i] == '.'))
        append_n(&db, &db_size, &db_cap, line + i++, 1);
      append_str(&db, &db_size, &db_cap, CLR_RESET);
      continue;
    }

    if (isalpha((unsigned char)line[i]) || line[i] == '_') {
      size_t start = i;
      while (line[i] && (isalnum((unsigned char)line[i]) || line[i] == '_' ||
                         line[i] == '.'))
        i++;
      size_t len = i - start;
      const char *color = NULL;
      int kind = 0;
      if (is_keyword(line + start, len))
        color = CLR_KEYWORD;
      else if (is_fn_call(line, i))
        color = CLR_FUNCTION;
      else if (is_known_name(line + start, len, &kind)) {
        if (kind == 2 || kind == 1)
          color = CLR_MEMBER;
        else if (kind == 3)
          color = CLR_FUNCTION;
      }
      if (color) {
        append_str(&db, &db_size, &db_cap, color);
        append_n(&db, &db_size, &db_cap, line + start, len);
        append_str(&db, &db_size, &db_cap, CLR_RESET);
      } else {
        append_n(&db, &db_size, &db_cap, line + start, len);
      }
      continue;
    }

    if (strchr("+-*%/%=&|^<>!.~", line[i])) {
      append_str(&db, &db_size, &db_cap, CLR_OPERATOR);
      append_n(&db, &db_size, &db_cap, line + i++, 1);
      append_str(&db, &db_size, &db_cap, CLR_RESET);
      continue;
    }

    if (strchr("()[]{};,", line[i])) {
      append_str(&db, &db_size, &db_cap, CLR_PAREN);
      append_n(&db, &db_size, &db_cap, line + i++, 1);
      append_str(&db, &db_size, &db_cap, CLR_RESET);
      continue;
    }

    if (line[i] == '\t') {
      append_str(&db, &db_size, &db_cap, "  ");
      i++;
      continue;
    }

    append_n(&db, &db_size, &db_cap, line + i++, 1);
  }

  const char *p = db;
  int fst = 1;
  while (1) {
    if (fst)
      fst = 0;
    else if (ml_prompt)
      fputs(ml_prompt, stdout);
    const char *nl = strchr(p, '\n');
    if (nl) {
      fwrite(p, 1, (size_t)(nl - p), stdout);
      printf("\033[K\n");
      p = nl + 1;
    } else {
      fputs(p, stdout);
      printf("\033[K");
      break;
    }
  }
}

void repl_highlight_line(const char *line) {
  repl_highlight_line_ex(line, -1, NULL);
}

void repl_display_match_list(char **matches, int len, int max_arg) {
  if (!matches || len <= 0)
    return;

  int term_width = 80;
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
    term_width = info.srWindow.Right - info.srWindow.Left + 1;
  }
#else
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    term_width = ws.ws_col;
  }
#endif

  int max_len = 0;
  for (int i = 0; i < len; i++) {
    int l = (int)strlen(matches[i]);
    if (l > max_len)
      max_len = l;
  }
  (void)max_arg;

  int col_width = max_len + 2;
  if (col_width > term_width)
    col_width = term_width;

  int num_cols = term_width / col_width;
  if (num_cols < 1)
    num_cols = 1;

  int rows = (len + num_cols - 1) / num_cols;

  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < num_cols; c++) {
      int idx = c * rows + r;
      if (idx >= len)
        break;

      const char *m = matches[idx];
      const char *color = CLR_RESET;
      int kind = 0;
      if (m[0] == ':') {
        color = CLR_BUILTIN;
      } else if (is_keyword(m, strlen(m))) {
        color = CLR_KEYWORD;
      } else if (!strcmp(m, "int") || !strcmp(m, "float") ||
                 !strcmp(m, "str") || !strcmp(m, "list") ||
                 !strcmp(m, "dict") || !strcmp(m, "set") ||
                 !strcmp(m, "tuple") || !strcmp(m, "bytes") ||
                 !strcmp(m, "bool")) {
        color = CLR_TYPE;
      } else if (strncmp(m, "__", 2) == 0) {
        color = CLR_BUILTIN;
      } else if (is_known_name(m, strlen(m), &kind)) {
        if (kind == 3)
          color = CLR_FUNCTION;
        else if (kind == 2 || kind == 1)
          color = CLR_MEMBER;
      } else {
        size_t mlen = strlen(m);
        if (mlen > 0 && (m[mlen - 1] == '/' || m[mlen - 1] == '\\')) {
          color = CLR_FUNCTION;
        } else if (strchr(m, '.')) {
          if (ny_std_find_module_by_name(m) >= 0)
            color = CLR_MEMBER;
        }
      }

      if (c == num_cols - 1 || (idx + rows) >= len) {
        printf("%s%s%s", color, m, CLR_RESET);
      } else {
        printf("%s%-*s%s", color, col_width, m, CLR_RESET);
      }
    }
    printf("\n");
  }
}

void repl_reset_redisplay(void) {}
void repl_redisplay(void) {}
