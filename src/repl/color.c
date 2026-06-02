#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/loader.h"
#include "base/util.h"
#include "parse/lexer.h"
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

#ifndef NYTRIX_DISABLE_READLINE_EXTRA
#define NYTRIX_DISABLE_READLINE_EXTRA 0
#endif

static const char *CLR_RESET = "\033[0m";
static const char *CLR_KEYWORD = "\033[1;35m";
static const char *CLR_BUILTIN = "\033[1;36m";
static const char *CLR_STRING = "\033[0;32m";
static const char *CLR_NUMBER = "\033[33m";
static const char *CLR_COMMENT = "\033[90m";
static const char *CLR_FUNCTION = "\033[1;34m";
static const char *CLR_OPERATOR = "\033[1;37m";
static const char *CLR_PAREN = "\033[37m";
static const char *CLR_MEMBER = "\033[36m";
static const char *CLR_MATCH = "\033[1;33m";
static const char *CLR_TYPE = "\033[1;32m";
static const char *CLR_SELECT = "\033[7m";
enum { REPL_TAB_VISUAL_WIDTH = 2 };

static int repl_visual_advance(int col, unsigned char ch) {
  if (ch == '\t')
    return col + REPL_TAB_VISUAL_WIDTH;
  if ((ch & 0xc0) == 0x80)
    return col;
  return col + 1;
}

static void pos_to_linecol(const char *line, int pos, int *out_row, int *out_col) {
  int row = 0;
  int col = 0;
  if (pos < 0)
    pos = 0;
  for (int i = 0; line[i] && i < pos; i++) {
    if (line[i] == '\n') {
      row++;
      col = 0;
    } else {
      col = repl_visual_advance(col, (unsigned char)line[i]);
    }
  }
  *out_row = row;
  *out_col = col;
}

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
    if (elen == len && memcmp(en, name, len) == 0) {
      if (out_kind)
        *out_kind = g_repl_docs->data[i].kind;
      return 1;
    }
    if (elen > len && en[elen - len - 1] == '.' && memcmp(en + elen - len, name, len) == 0) {
      suffix_match_idx = (int)i;
    }
    if (len > elen && name[len - elen - 1] == '.' && memcmp(name + len - elen, en, elen) == 0) {
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
    if (len == 4 && !memcmp(s, "case", 4))
      return 1;
    if (len == 5 && !memcmp(s, "catch", 5))
      return 1;
    if (len == 5 && !memcmp(s, "class", 5))
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
    if (len == 3 && !memcmp(s, "del", 3))
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
    if (len == 6 && !memcmp(s, "export", 6))
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
    if (len == 5 && !memcmp(s, "guard", 5))
      return 1;
    if (len == 4 && !memcmp(s, "goto", 4))
      return 1;
    break;
  case 'i':
    if (len == 2 && !memcmp(s, "if", 2))
      return 1;
    if (len == 4 && !memcmp(s, "impl", 4))
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
  case 'o':
    if (len == 8 && !memcmp(s, "operator", 8))
      return 1;
    if (len == 5 && !memcmp(s, "owned", 5))
      return 1;
    break;
  case 'r':
    if (len == 6 && !memcmp(s, "return", 6))
      return 1;
    break;
  case 't':
    if (len == 5 && !memcmp(s, "trait", 5))
      return 1;
    if (len == 3 && !memcmp(s, "try", 3))
      return 1;
    if (len == 4 && !memcmp(s, "true", 4))
      return 1;
    break;
  case 'u':
    if (len == 3 && !memcmp(s, "use", 3))
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

static int is_builtin_type_name(const char *s, size_t len) {
  switch (s[0]) {
  case 'b':
    return (len == 4 && !memcmp(s, "bool", 4)) ||
           (len == 6 && !memcmp(s, "bigint", 6)) ||
           (len == 5 && !memcmp(s, "bytes", 5));
  case 'd':
    return len == 4 && !memcmp(s, "dict", 4);
  case 'f':
    return len == 5 && !memcmp(s, "float", 5);
  case 'i':
    return (len == 3 && !memcmp(s, "int", 3)) ||
           (len == 3 && !memcmp(s, "i64", 3));
  case 'l':
    return len == 4 && !memcmp(s, "list", 4);
  case 's':
    return (len == 3 && !memcmp(s, "str", 3)) ||
           (len == 3 && !memcmp(s, "set", 3));
  case 't':
    return len == 5 && !memcmp(s, "tuple", 5);
  default:
    return 0;
  }
}

enum {
  ST_NONE = 0,
  ST_KEYWORD,
  ST_BUILTIN,
  ST_STRING,
  ST_NUMBER,
  ST_COMMENT,
  ST_FUNCTION,
  ST_OPERATOR,
  ST_PAREN,
  ST_MEMBER,
  ST_MATCH,
  ST_TYPE
};

static const char *style_ansi(int style) {
  switch (style) {
  case ST_KEYWORD:
    return CLR_KEYWORD;
  case ST_BUILTIN:
    return CLR_BUILTIN;
  case ST_STRING:
    return CLR_STRING;
  case ST_NUMBER:
    return CLR_NUMBER;
  case ST_COMMENT:
    return CLR_COMMENT;
  case ST_FUNCTION:
    return CLR_FUNCTION;
  case ST_OPERATOR:
    return CLR_OPERATOR;
  case ST_PAREN:
    return CLR_PAREN;
  case ST_MEMBER:
    return CLR_MEMBER;
  case ST_MATCH:
    return CLR_MATCH;
  case ST_TYPE:
    return CLR_TYPE;
  default:
    return "";
  }
}

static void style_mark(unsigned char *styles, size_t start, size_t end, unsigned char style) {
  for (size_t i = start; i < end; i++)
    styles[i] = style;
}

void repl_highlight_line_ex(const char *line, int cursor_pos, const char *ml_prompt, int sel_start,
                            int sel_end, int sel_mode) {
  if (!line || !*line)
    return;
  if (!is_color_enabled()) {
    fputs(line, stdout);
    return;
  }
  if (sel_start < 0)
    sel_start = -1;
  if (sel_end < sel_start)
    sel_end = sel_start;
  int target_pos = -1, match_pos = -1;
  size_t line_len = strlen(line);
  unsigned char *styles = calloc(line_len ? line_len : 1, 1);
  if (!styles) {
    fputs(line, stdout);
    return;
  }
  if (line[0] == ':') {
    style_mark(styles, 0, line_len, ST_BUILTIN);
  }
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
      styles[i] = ST_MATCH;
      i++;
      continue;
    }
    if (line[i] == '"' ||
        (line[i] == 'f' && line[i + 1] == '"' &&
         (i == 0 || !isalnum((unsigned char)line[i - 1]))) ||
        line[i] == '\'') {
      size_t start = i;
      char q = line[i];
      if (q == 'f')
        q = '"';
      if (line[i] == 'f')
        i++;
      i++;
      while (line[i] && line[i] != q) {
        if (line[i] == '\\' && line[i + 1])
          i++;
        i++;
      }
      if (line[i] == q)
        i++;
      style_mark(styles, start, i, ST_STRING);
      continue;
    }
    if (line[i] == ';') {
      const char *eol = strchr(line + i, '\n');
      if (eol) {
        size_t comment_len = (size_t)(eol - (line + i));
        style_mark(styles, i, i + comment_len, ST_COMMENT);
        i += comment_len;
      } else {
        style_mark(styles, i, line_len, ST_COMMENT);
        break;
      }
      continue;
    }
    if (isdigit((unsigned char)line[i])) {
      size_t start = i;
      while (line[i] && (isalnum((unsigned char)line[i]) || line[i] == '.'))
        i++;
      style_mark(styles, start, i, ST_NUMBER);
      continue;
    }
    if (isalpha((unsigned char)line[i]) || line[i] == '_') {
      size_t start = i;
      while (line[i] && (isalnum((unsigned char)line[i]) || line[i] == '_' || line[i] == '.'))
        i++;
      size_t len = i - start;
      unsigned char style = ST_NONE;
      int kind = 0;
      if (is_keyword(line + start, len))
        style = ST_KEYWORD;
      else if (is_builtin_type_name(line + start, len))
        style = ST_TYPE;
      else if (is_fn_call(line, i))
        style = ST_FUNCTION;
      else if (is_known_name(line + start, len, &kind)) {
        if (kind == 2 || kind == 1)
          style = ST_MEMBER;
        else if (kind == 3 || kind == 5)
          style = ST_FUNCTION;
      }
      style_mark(styles, start, i, style);
      continue;
    }
    if (strchr("+-*%/%=&|^<>!.~", line[i])) {
      styles[i++] = ST_OPERATOR;
      continue;
    }
    if (strchr("()[]{};,", line[i])) {
      styles[i++] = ST_PAREN;
      continue;
    }
    i++;
  }
  if (target_pos >= 0 && (size_t)target_pos < line_len)
    styles[target_pos] = ST_MATCH;
  if (match_pos >= 0 && (size_t)match_pos < line_len)
    styles[match_pos] = ST_MATCH;

  int fst = 1;
  int active_style = ST_NONE;
  int active_sel = 0;
  int block_r0 = 0, block_c0 = 0, block_r1 = 0, block_c1 = 0;
  int row = 0;
  int col = 0;
  if (sel_mode == REPL_SEL_BLOCK && sel_start >= 0 && sel_end >= 0) {
    pos_to_linecol(line, sel_start, &block_r0, &block_c0);
    pos_to_linecol(line, sel_end, &block_r1, &block_c1);
  }
  for (size_t j = 0; j < line_len; j++) {
    if (fst)
      fst = 0;
    else if (line[j - 1] == '\n' && ml_prompt)
      fputs(ml_prompt, stdout);
    if (line[j] == '\n') {
      if (active_sel || active_style != ST_NONE) {
        fputs(CLR_RESET, stdout);
        active_sel = 0;
        active_style = ST_NONE;
      }
      printf("\033[K\n");
      row++;
      col = 0;
      continue;
    }
    int want_sel = 0;
    if (sel_mode == REPL_SEL_BLOCK && sel_start >= 0 && sel_end >= 0) {
      int rmin = block_r0 < block_r1 ? block_r0 : block_r1;
      int rmax = block_r0 > block_r1 ? block_r0 : block_r1;
      int cmin = block_c0 < block_c1 ? block_c0 : block_c1;
      int cmax = block_c0 > block_c1 ? block_c0 : block_c1;
      want_sel = (row >= rmin && row <= rmax && col >= cmin && col < cmax);
    } else {
      want_sel = (sel_start >= 0 && (int)j >= sel_start && (int)j < sel_end);
    }
    int want_style = styles[j];
    if (want_sel != active_sel || want_style != active_style) {
      fputs(CLR_RESET, stdout);
      if (want_sel)
        fputs(CLR_SELECT, stdout);
      if (want_style != ST_NONE)
        fputs(style_ansi(want_style), stdout);
      active_sel = want_sel;
      active_style = want_style;
    }
    if (line[j] == '\t') {
      for (int k = 0; k < REPL_TAB_VISUAL_WIDTH; ++k)
        fputc(' ', stdout);
      col = repl_visual_advance(col, (unsigned char)line[j]);
    } else {
      fwrite(line + j, 1, 1, stdout);
      col = repl_visual_advance(col, (unsigned char)line[j]);
    }
  }
  if (active_sel || active_style != ST_NONE)
    fputs(CLR_RESET, stdout);
  printf("\033[K");
  free(styles);
}

void repl_highlight_line(const char *line) {
  repl_highlight_line_ex(line, -1, NULL, -1, -1, REPL_SEL_NONE);
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
  int display_len = len;
  if (max_arg > 0 && display_len > max_arg)
    display_len = max_arg;
  int col_width = max_len + 2;
  if (col_width > term_width)
    col_width = term_width;
  int num_cols = term_width / col_width;
  if (num_cols < 1)
    num_cols = 1;
  int rows = (display_len + num_cols - 1) / num_cols;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < num_cols; c++) {
      int idx = c * rows + r;
      if (idx >= display_len)
        break;
      const char *m = matches[idx];
      const char *color = CLR_RESET;
      int kind = 0;
      if (m[0] == ':') {
        color = CLR_BUILTIN;
      } else if (is_keyword(m, strlen(m))) {
        color = CLR_KEYWORD;
      } else if (!strcmp(m, "int") || !strcmp(m, "float") || !strcmp(m, "str") ||
                 !strcmp(m, "list") || !strcmp(m, "dict") || !strcmp(m, "set") ||
                 !strcmp(m, "tuple") || !strcmp(m, "bytes") || !strcmp(m, "bool")) {
        color = CLR_TYPE;
      } else if (strncmp(m, "__", 2) == 0) {
        color = CLR_BUILTIN;
      } else if (is_known_name(m, strlen(m), &kind)) {
        if (kind == 3 || kind == 5)
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
  if (display_len < len)
    printf("... %d more\n", len - display_len);
}

void repl_reset_redisplay(void) {}
void repl_redisplay(void) {}
