#include "base/util.h"
#include "base/common.h"
#include "priv.h"
#include <ctype.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *g_repl_user_source = NULL;
size_t g_repl_user_source_len = 0;
static size_t g_repl_user_source_cap = 0;
int repl_indent_next = 0;

char *ltrim(char *s) {
  while (*s && isspace((unsigned char)*s))
    s++;
  return s;
}

void rtrim_inplace(char *s) {
  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1])) {
    s[--len] = '\0';
  }
}

char *repl_read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc(size + 1);
  if (buf) {
    size_t n = fread(buf, 1, size, f);
    buf[n] = '\0';
  }
  fclose(f);
  return buf;
}

char **repl_split_lines(const char *src, size_t *out_count) {
  size_t cap = 16, count = 0;
  char **lines = malloc(cap * sizeof(char *));
  char *copy = ny_strdup(src);
  char *line = strtok(copy, "\n");
  while (line) {
    if (count >= cap) {
      cap *= 2;
      lines = realloc(lines, cap * sizeof(char *));
    }
    lines[count++] = ny_strdup(line);
    line = strtok(NULL, "\n");
  }
  free(copy);
  *out_count = count;
  return lines;
}

void repl_append_user_source(const char *src) {
  if (!src || !*src)
    return;
  size_t slen = strlen(src);
  int needs_newline = (src[slen - 1] != '\n');
  int needs_prefix_newline =
      (g_repl_user_source_len > 0 &&
       g_repl_user_source[g_repl_user_source_len - 1] != '\n');

  size_t required = g_repl_user_source_len + slen + (needs_newline ? 1 : 0) +
                    (needs_prefix_newline ? 1 : 0) + 1;

  if (required >= g_repl_user_source_cap) {
    g_repl_user_source_cap = required + 1024;
    g_repl_user_source = realloc(g_repl_user_source, g_repl_user_source_cap);
  }
  if (!g_repl_user_source)
    return;

  if (needs_prefix_newline) {
    g_repl_user_source[g_repl_user_source_len++] = '\n';
  }

  memcpy(g_repl_user_source + g_repl_user_source_len, src, slen);
  g_repl_user_source_len += slen;

  if (needs_newline) {
    g_repl_user_source[g_repl_user_source_len++] = '\n';
  }
  g_repl_user_source[g_repl_user_source_len] = '\0';
}

void repl_remove_def(const char *name) {
  // Basic implementation for now
  (void)name;
}

char *repl_assignment_target(const char *src) {
  char *trimmed = ltrim((char *)src);
  if (strncmp(trimmed, "def ", 4) == 0) {
    char *p = ltrim(trimmed + 4);
    char *end = p;
    while (*end && (isalnum((unsigned char)*end) || *end == '_'))
      end++;
    if (end > p)
      return ny_strndup(p, (size_t)(end - p));
  }
  // Check for 'x = ...' or 'x += ...' etc.
  char *eq = strchr(trimmed, '=');
  if (eq && eq != trimmed) {
    char *end = eq - 1;
    // Handle compound operators: += -= *= /= %= &= |= ^= <<= >>= != == <= >=
    if (end >= trimmed && strchr("+-*/%&|^<>!=", *end)) {
      // It's a compound op or a comparison.
      // If it's ==, !=, <=, >=, it's a comparison (expression).
      // If it's +=, -=, etc, it's an assignment (statement).
      if ((*end == '=' || *end == '!' || *end == '<' || *end == '>') &&
          (end == trimmed || *(end - 1) != *end)) {
        // Likely == or != or <= or >=. But wait, <<= has two <.
        // Simple heuristic: if it's == or !=, it's an expression.
        if ((*end == '=' || *end == '!') ||
            (end > trimmed && strchr("<>", *(end - 1)) == 0)) {
          // Check for '==' or '!='
          if (*end == '!')
            return NULL;
          if (*end == '=' && end > trimmed && *(end - 1) == '=')
            return NULL;
          // Comparisons are expressions unless they are used in assignments.
          // For now, let's keep it simple.
        }
      }
      end--;
    }
    while (end >= trimmed && isspace((unsigned char)*end))
      end--;
    if (end < trimmed)
      return NULL;
    char *ident_end = end;
    while (end >= trimmed &&
           (isalnum((unsigned char)*end) || *end == '_' || *end == '.'))
      end--;
    char *ident_start = end + 1;
    if (ident_end >= ident_start) {
      // Check if it's a valid identifier (roughly)
      if (isalnum((unsigned char)*ident_start) || *ident_start == '_') {
        return ny_strndup(ident_start, (size_t)(ident_end - ident_start + 1));
      }
    }
  }
  return NULL;
}

int is_repl_stmt(const char *src) {
  char *trimmed = ltrim((char *)src);
  if (!*trimmed)
    return 0;
  if (strchr(src, '{'))
    return 1;
  const char *kw[] = {"def ",   "fn ",      "use ",    "module ", "undef ",
                      "if ",    "while ",   "for ",    "try ",    "return ",
                      "break",  "continue", "goto ",   "defer ",  "struct ",
                      "class ", "layout ",  "extern ", NULL};
  for (int i = 0; kw[i]; i++) {
    size_t len = strlen(kw[i]);
    if (strncmp(trimmed, kw[i], len) == 0) {
      if (kw[i][len - 1] == ' ' || trimmed[len] == '\0' ||
          isspace((unsigned char)trimmed[len]) || trimmed[len] == '(')
        return 1;
    }
  }
  char *an = repl_assignment_target(src);
  if (an) {
    free(an);
    return 1;
  }
  return 0;
}

void count_unclosed(const char *src, int *out_paren, int *out_brack,
                    int *out_brace) {
  int p = 0, b = 0, c = 0;
  int in_str = 0;
  for (const char *s = src; *s; s++) {
    if (*s == '"' && (s == src || *(s - 1) != '\\'))
      in_str = !in_str;
    if (in_str)
      continue;
    if (*s == '(')
      p++;
    else if (*s == ')')
      p--;
    if (*s == '[')
      b++;
    else if (*s == ']')
      b--;
    if (*s == '{')
      c++;
    else if (*s == '}')
      c--;
  }
  *out_paren = p;
  *out_brack = b;
  *out_brace = c;
}

int is_input_complete(const char *src) {
  int p, b, c;
  count_unclosed(src, &p, &b, &c);
  return (p <= 0 && b <= 0 && c <= 0);
}

void print_incomplete_hint(const char *src) {
  int p, b, c;
  count_unclosed(src, &p, &b, &c);
  if (p > 0 || b > 0 || c > 0) {
    printf("%s  ", clr(NY_CLR_GRAY));
    if (p > 0)
      printf("(missing %d ')') ", p);
    if (b > 0)
      printf("(missing %d ']') ", b);
    if (c > 0)
      printf("(missing %d '}') ", c);
    printf("%s\n", clr(NY_CLR_RESET));
  }
}

#include <sys/ioctl.h>
int repl_is_input_pending(void) {
  int n = 0;
  if (ioctl(STDIN_FILENO, FIONREAD, &n) < 0)
    return 0;
  return n > 0;
}

int repl_calc_indent(const char *src) {
  int p, b, c;
  count_unclosed(src, &p, &b, &c);
  int level = p + b + c;
  return level > 0 ? level * 2 : 0;
}

int repl_pre_input_hook(void) {
  if (repl_is_input_pending())
    return 0;
  if (repl_indent_next > 0) {
    for (int i = 0; i < repl_indent_next; i++) {
      rl_insert_text(" ");
    }
  }
  return 0;
}

void repl_print_error_snippet(const char *src, int line, int col) {
  (void)src;
  fprintf(stderr, "Error at %d:%d\n", line, col);
}

int is_persistent_def(const char *src) {
  char *trimmed = ltrim((char *)src);
  return (!strncmp(trimmed, "def ", 4) || !strncmp(trimmed, "fn ", 3) ||
          !strncmp(trimmed, "use ", 4) || !strncmp(trimmed, "module ", 7) ||
          !strncmp(trimmed, "extern ", 7) || strchr(trimmed, '=') != NULL);
}

void repl_update_docs(doc_list_t *dl, const char *src) {
  char *trimmed = ltrim((char *)src);
  if (!strncmp(trimmed, "fn ", 3)) {
    char *p = ltrim(trimmed + 3);
    char *end = p;
    while (*end && (isalnum((unsigned char)*end) || *end == '_'))
      end++;
    if (end > p) {
      char *name = ny_strndup(p, (size_t)(end - p));
      doclist_set(dl, name, "REPL defined function", NULL, NULL, 3);
      free(name);
    }
  } else if (!strncmp(trimmed, "def ", 4)) {
    char *p = ltrim(trimmed + 4);
    char *end = p;
    while (*end && (isalnum((unsigned char)*end) || *end == '_'))
      end++;
    if (end > p) {
      char *name = ny_strndup(p, (size_t)(end - p));
      doclist_set(dl, name, "REPL defined variable", NULL, NULL, 4);
      free(name);
    }
  } else {
    char *an = repl_assignment_target(src);
    if (an) {
      doclist_set(dl, an, "REPL defined variable", NULL, NULL, 4);
      free(an);
    }
  }
}
