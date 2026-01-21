#include "lex/lexer.h"
#include "priv.h"
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Standard 16-Color Palette
static const char *CLR_RESET = "\033[0m";
static const char *CLR_KEYWORD = "\033[1;36m";  // Bold Cyan
static const char *CLR_BUILTIN = "\033[1;35m";  // Bold Magenta
static const char *CLR_STRING = "\033[32m";     // Green
static const char *CLR_NUMBER = "\033[33m";     // Yellow
static const char *CLR_COMMENT = "\033[90m";    // Gray
static const char *CLR_FUNCTION = "\033[1;34m"; // Bold Blue
static const char *CLR_OPERATOR = "\033[31m";   // Red
static const char *CLR_PAREN = "\033[37m";      // White
static const char *CLR_MEMBER = "\033[35m";     // Magenta
static const char *CLR_MATCH = "\033[1;33m";    // Bold Yellow
static const char *CLR_VAR = "\033[34m";        // Blue

// Helper to check if colorization is enabled
static int is_color_enabled(void) {
  const char *plain = getenv("NYTRIX_REPL_PLAIN");
  return !(plain && plain[0] != '0');
}

// Find matching parenthesis
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

// Check if identifier looks like a function call
static int is_function_call(const char *line, size_t pos) {
  while (line[pos] && (line[pos] == ' ' || line[pos] == '\t'))
    pos++;
  return line[pos] == '(';
}

// Check if name is known in docs
static int is_known_name(const char *name, size_t len, int *out_kind) {
  if (!g_repl_docs)
    return 0;

  // Single loop for both exact and suffix matches
  int suffix_match_idx = -1;
  for (size_t i = 0; i < g_repl_docs->len; i++) {
    const char *en = g_repl_docs->data[i].name;
    size_t elen = strlen(en);

    if (elen == len && memcmp(en, name, len) == 0) {
      if (out_kind)
        *out_kind = g_repl_docs->data[i].kind;
      return 1;
    }

    // Suffix match (lower priority, stored if no exact match found yet)
    if (suffix_match_idx == -1 && elen > len && en[elen - len - 1] == '.' &&
        memcmp(en + elen - len, name, len) == 0) {
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
    break;
  case 'e':
    if (len == 4 && !memcmp(s, "else", 4))
      return 1;
    if (len == 4 && !memcmp(s, "elif", 4))
      return 1;
    if (len == 5 && !memcmp(s, "embed", 5))
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

void repl_highlight_line_ex(const char *line, int cursor_pos) {
  if (!line || !*line)
    return;

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

    if (line[i] == ';') {
      append_str(&db, &db_size, &db_cap, CLR_COMMENT);
      append_str(&db, &db_size, &db_cap, line + i);
      append_str(&db, &db_size, &db_cap, CLR_RESET);
      break;
    }

    if (line[i] == '"' || (line[i] == 'f' && line[i + 1] == '"')) {
      append_str(&db, &db_size, &db_cap, CLR_STRING);
      if (line[i] == 'f')
        append_n(&db, &db_size, &db_cap, line + i++, 1);
      append_n(&db, &db_size, &db_cap, line + i++, 1);
      while (line[i] && line[i] != '"') {
        if (line[i] == '\\' && line[i + 1])
          append_n(&db, &db_size, &db_cap, line + i++, 1);
        append_n(&db, &db_size, &db_cap, line + i++, 1);
      }
      if (line[i] == '"')
        append_n(&db, &db_size, &db_cap, line + i++, 1);
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
      while (line[i] && (isalnum((unsigned char)line[i]) || line[i] == '_'))
        i++;
      size_t len = i - start;
      const char *color = NULL;
      int kind = 0;
      if (is_keyword(line + start, len))
        color = CLR_KEYWORD;
      else if (is_function_call(line, i))
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

    if (strchr("+-*/%=&|^<>!.~", line[i])) {
      append_str(&db, &db_size, &db_cap, CLR_OPERATOR);
      append_n(&db, &db_size, &db_cap, line + i++, 1);
      append_str(&db, &db_size, &db_cap, CLR_RESET);
      continue;
    }

    if (line[i] == '\t') {
      append_str(&db, &db_size, &db_cap, "  ");
      i++;
      continue;
    }

    if (strchr("()[]{},", line[i])) {
      append_str(&db, &db_size, &db_cap, CLR_PAREN);
      append_n(&db, &db_size, &db_cap, line + i++, 1);
      append_str(&db, &db_size, &db_cap, CLR_RESET);
      continue;
    }

    append_n(&db, &db_size, &db_cap, line + i++, 1);
  }
  fputs(db, stdout);
}

void repl_highlight_line(const char *line) { repl_highlight_line_ex(line, -1); }

// Optimization: cache visible prompt length
static char *last_prompt = NULL;
static int last_visible_len = 0;

void repl_redisplay(void) {
  if (!is_color_enabled()) {
    rl_redisplay();
    return;
  }

  // Calculate visible prompt length if it changed and prepare a clean prompt
  // for display
  static char *clean_prompt = NULL;
  if (!last_prompt || strcmp(last_prompt, rl_display_prompt) != 0) {
    if (last_prompt)
      free(last_prompt);
    if (clean_prompt)
      free(clean_prompt);
    last_prompt = strdup(rl_display_prompt);
    clean_prompt = malloc(strlen(rl_display_prompt) + 1);

    last_visible_len = 0;
    int in_invisible = 0;
    char *out = clean_prompt;
    for (const char *p = rl_display_prompt; *p; p++) {
      if (*p == '\001')
        in_invisible = 1;
      else if (*p == '\002')
        in_invisible = 0;
      else {
        *out++ = *p;
        if (!in_invisible)
          last_visible_len++;
      }
    }
    *out = '\0';
  }

  // Move to start of line, print prompt and highlighted buffer
  fputc('\r', stdout);
  fputs(clean_prompt, stdout);
  repl_highlight_line_ex(rl_line_buffer, rl_point);
  fputs("\033[K", stdout); // Clear rest of line

  // Calculate actual visible distance to move back.
  // We must account for any character expansions (like tabs -> spaces)
  // both before and after the cursor (rl_point).
  int visible_total = 0;
  int visible_point = 0;
  for (int i = 0; rl_line_buffer[i]; i++) {
    int width = (rl_line_buffer[i] == '\t') ? 2 : 1;
    visible_total += width;
    if (i < rl_point) {
      visible_point += width;
    }
  }

  int move_back = visible_total - visible_point;
  if (move_back > 0) {
    printf("\033[%dD", move_back);
  }

  fflush(stdout);
}

void repl_display_match_list(char **matches, int len, int max) {
  (void)max;
  printf("\n");
  int cols = 0;
  for (int i = 1; i <= len; i++) {
    const char *m = matches[i];
    const char *color = CLR_RESET;
    int kind = 0;
    if (is_keyword(m, strlen(m)))
      color = CLR_KEYWORD;
    else if (is_known_name(m, strlen(m), &kind)) {
      if (kind == 3)
        color = CLR_FUNCTION;
      else if (kind == 4)
        color = CLR_VAR;
      else if (kind == 1 || kind == 2)
        color = CLR_MEMBER;
    } else if (m[0] == ':') {
      color = CLR_BUILTIN;
    }
    printf("%s%-20s%s", color, m, CLR_RESET);
    if (++cols % 4 == 0)
      printf("\n");
  }
  if (cols % 4 != 0)
    printf("\n");
  rl_forced_update_display();
}
