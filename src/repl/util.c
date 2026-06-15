#include "base/util.h"
#include "base/common.h"
#include "priv.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

char *g_repl_user_source = NULL;
size_t g_repl_user_source_len = 0;
static size_t g_repl_user_source_cap = 0;
int repl_indent_next = 0;

static bool repl_match_main_guard(const char *src, size_t i, const char *needle);
static bool repl_is_ident_char(unsigned char ch);

static char *repl_doc_normalize_module_name(const char *raw) {
  if (!raw)
    return NULL;
  if (strchr(raw, '/')) {
    const char *last_slash = strrchr(raw, '/');
    const char *start = last_slash ? last_slash + 1 : raw;
    char *name = ny_strdup(start);
    char *dot = name ? strrchr(name, '.') : NULL;
    if (dot && dot != name)
      *dot = '\0';
    return name;
  }
  return ny_strdup(raw);
}

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

int repl_head_is_number(const char *s) {
  if (!s || !*s)
    return 0;
  if ((*s == '+' || *s == '-') && isdigit((unsigned char)s[1]))
    s++;
  int digits = 0;
  int last_digit = 0;
  while (isdigit((unsigned char)*s) || *s == '_') {
    if (*s == '_') {
      if (!last_digit || !isdigit((unsigned char)s[1]))
        return 0;
      last_digit = 0;
    } else {
      digits = 1;
      last_digit = 1;
    }
    s++;
  }
  if (*s == '.') {
    s++;
    last_digit = 0;
    while (isdigit((unsigned char)*s) || *s == '_') {
      if (*s == '_') {
        if (!last_digit || !isdigit((unsigned char)s[1]))
          return 0;
        last_digit = 0;
      } else {
        digits = 1;
        last_digit = 1;
      }
      s++;
    }
  }
  if (!digits)
    return 0;
  if (*s == 'e' || *s == 'E') {
    s++;
    if (*s == '+' || *s == '-')
      s++;
    int exp_digits = 0;
    last_digit = 0;
    while (isdigit((unsigned char)*s) || *s == '_') {
      if (*s == '_') {
        if (!last_digit || !isdigit((unsigned char)s[1]))
          return 0;
        last_digit = 0;
      } else {
        exp_digits = 1;
        last_digit = 1;
      }
      s++;
    }
    if (!exp_digits)
      return 0;
  }
  return *s == '\0';
}

char *repl_read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) == 0) {
    long size = ftell(f);
    if (size >= 0 && fseek(f, 0, SEEK_SET) == 0) {
      char *buf = malloc((size_t)size + 1);
      if (!buf) {
        fclose(f);
        return NULL;
      }
      size_t n = fread(buf, 1, (size_t)size, f);
      if (ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
      }
      buf[n] = '\0';
      fclose(f);
      return buf;
    }
  }
  clearerr(f);
  if (fseek(f, 0, SEEK_SET) != 0)
    clearerr(f);
  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  while (1) {
    if (len + 2049 > cap) {
      if (cap > ((size_t)-1) / 2) {
        free(buf);
        fclose(f);
        errno = EOVERFLOW;
        return NULL;
      }
      cap *= 2;
      char *grown = realloc(buf, cap);
      if (!grown) {
        free(buf);
        fclose(f);
        return NULL;
      }
      buf = grown;
    }
    size_t n = fread(buf + len, 1, 2048, f);
    len += n;
    if (n < 2048) {
      if (ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
      }
      break;
    }
  }
  buf[len] = '\0';
  fclose(f);
  return buf;
}

int repl_write_session_source(const char *path) {
  if (!path || !*path)
    return -1;
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  const char *src = (g_repl_user_source && *g_repl_user_source) ? g_repl_user_source : "";
  size_t len = strlen(src);
  int ok = fwrite(src, 1, len, f) == len;
  if (ok && len > 0 && src[len - 1] != '\n')
    ok = fputc('\n', f) != EOF;
  if (fclose(f) != 0)
    ok = 0;
  return ok ? 0 : -1;
}

static bool repl_is_ident_char(unsigned char ch) { return isalnum(ch) || ch == '_'; }

static bool repl_match_main_guard(const char *src, size_t i, const char *needle) {
  size_t nlen = strlen(needle);
  if (strncmp(src + i, needle, nlen) != 0)
    return false;
  if (i > 0 && repl_is_ident_char((unsigned char)src[i - 1]))
    return false;
  if (repl_is_ident_char((unsigned char)src[i + nlen]))
    return false;
  return true;
}

char *repl_mask_main_guards(const char *src) {
  static const char main_guard[] = "__main()";
  static const char replacement[] = "false";
  size_t src_len = strlen(src);
  size_t out_cap = src_len + 1;
  char *out = malloc(out_cap);
  if (!out)
    return NULL;
  size_t i = 0;
  size_t out_len = 0;
  while (i < src_len) {
    size_t match_len = 0;
    if (repl_match_main_guard(src, i, main_guard))
      match_len = sizeof(main_guard) - 1;
    if (match_len != 0) {
      size_t repl_len = sizeof(replacement) - 1;
      if (out_len + repl_len + 1 > out_cap) {
        out_cap = (out_len + repl_len + 1) * 2;
        out = realloc(out, out_cap);
        if (!out)
          return NULL;
      }
      memcpy(out + out_len, replacement, repl_len);
      out_len += repl_len;
      i += match_len;
      continue;
    }
    if (out_len + 2 > out_cap) {
      out_cap *= 2;
      out = realloc(out, out_cap);
      if (!out)
        return NULL;
    }
    out[out_len++] = src[i++];
  }
  out[out_len] = '\0';
  return out;
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
      (g_repl_user_source_len > 0 && g_repl_user_source[g_repl_user_source_len - 1] != '\n');
  size_t required =
      g_repl_user_source_len + slen + (needs_newline ? 1 : 0) + (needs_prefix_newline ? 1 : 0) + 1;
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

void repl_set_user_source(const char *src) {
  free(g_repl_user_source);
  g_repl_user_source = NULL;
  g_repl_user_source_len = 0;
  g_repl_user_source_cap = 0;
  if (src && *src)
    repl_append_user_source(src);
}

static int repl_stmt_is_boundary(const char *src, size_t i, int paren, int brack, int brace,
                                 int in_str) {
  return src[i] == '\n' && paren == 0 && brack == 0 && brace == 0 && !in_str;
}

static int repl_ident_char_at(char ch) {
  return isalnum((unsigned char)ch) || ch == '_';
}

static int repl_word_at(const char *s, const char *word) {
  size_t len = strlen(word);
  return strncmp(s, word, len) == 0 && !repl_ident_char_at(s[len]);
}

static const char *repl_skip_inline_ws(const char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r')
    s++;
  return s;
}

static const char *repl_find_top_level_assign(const char *s) {
  int paren = 0, brack = 0, brace = 0, in_str = 0;
  char quote = '\0';
  for (const char *p = s; *p; ++p) {
    char ch = *p;
    if (in_str) {
      if (ch == '\\' && p[1]) {
        ++p;
        continue;
      }
      if (ch == quote) {
        in_str = 0;
        quote = '\0';
      }
      continue;
    }
    if (ch == '"' || ch == '\'') {
      in_str = 1;
      quote = ch;
      continue;
    }
    if (ch == ';' || ch == '#')
      break;
    if (ch == '(')
      paren++;
    else if (ch == ')' && paren > 0)
      paren--;
    else if (ch == '[')
      brack++;
    else if (ch == ']' && brack > 0)
      brack--;
    else if (ch == '{')
      brace++;
    else if (ch == '}' && brace > 0)
      brace--;
    else if (ch == '=' && paren == 0 && brack == 0 && brace == 0) {
      char prev = (p > s) ? p[-1] : '\0';
      char next = p[1];
      if (prev == '=' || prev == '!' || prev == '<' || prev == '>' || next == '=')
        continue;
      return p;
    }
  }
  return NULL;
}

static int repl_expr_can_end_with(char ch) {
  return isalnum((unsigned char)ch) || ch == '_' || ch == ')' || ch == ']' ||
         ch == '}' || ch == '"' || ch == '\'';
}

static int repl_expr_can_start_with(char ch) {
  return isalpha((unsigned char)ch) || isdigit((unsigned char)ch) || ch == '_' ||
         ch == '(' || ch == '[' || ch == '{' || ch == '.' || ch == '"' || ch == '\'';
}

static int repl_keep_rhs_keyword(const char *s) {
  return repl_word_at(s, "else") || repl_word_at(s, "elif") || repl_word_at(s, "catch");
}

static char *repl_trim_persistent_assignment(const char *stmt) {
  char *copy = ny_strdup(stmt);
  if (!copy)
    return NULL;
  char *code = repl_skip_leading_noncode(copy);
  if (!*code)
    return copy;
  char *target = repl_assignment_target(code);
  int starts_mutating = !strncmp(code, "def ", 4) || !strncmp(code, "mut ", 4) || target != NULL;
  free(target);
  if (!starts_mutating)
    return copy;
  const char *eq = repl_find_top_level_assign(code);
  if (!eq)
    return copy;
  const char *p = eq + 1;
  int paren = 0, brack = 0, brace = 0, in_str = 0;
  char quote = '\0';
  const char *last = p;
  while (*p) {
    char ch = *p;
    if (in_str) {
      if (ch == '\\' && p[1]) {
        p += 2;
        last = p;
        continue;
      }
      if (ch == quote) {
        in_str = 0;
        quote = '\0';
      }
      p++;
      last = p;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      in_str = 1;
      quote = ch;
      p++;
      last = p;
      continue;
    }
    if ((ch == ';' || ch == '#') && paren == 0 && brack == 0 && brace == 0)
      break;
    if (ch == '\n' && paren == 0 && brack == 0 && brace == 0)
      break;
    if (ch == '(')
      paren++;
    else if (ch == ')' && paren > 0)
      paren--;
    else if (ch == '[')
      brack++;
    else if (ch == ']' && brack > 0)
      brack--;
    else if (ch == '{')
      brace++;
    else if (ch == '}' && brace > 0)
      brace--;
    if (paren == 0 && brack == 0 && brace == 0 && isspace((unsigned char)ch)) {
      const char *next = repl_skip_inline_ws(p + 1);
      char prev = '\0';
      for (const char *q = p; q > eq + 1; --q) {
        if (!isspace((unsigned char)q[-1])) {
          prev = q[-1];
          break;
        }
      }
      if (*next && *next != '\n' && repl_expr_can_end_with(prev) &&
          repl_expr_can_start_with(*next) && !repl_keep_rhs_keyword(next))
        break;
    }
    p++;
    last = p;
  }
  while (last > code && isspace((unsigned char)last[-1]))
    last--;
  code[last - code] = '\0';
  char *out = ny_strdup(code);
  free(copy);
  return out;
}

char *repl_extract_persistent_source(const char *src) {
  if (!src || !*src)
    return NULL;
  size_t src_len = strlen(src);
  size_t out_cap = src_len + 1;
  char *out = malloc(out_cap);
  if (!out)
    return NULL;
  out[0] = '\0';
  size_t out_len = 0;

  size_t stmt_start = 0;
  int paren = 0, brack = 0, brace = 0, in_str = 0;
  char quote = '\0';

  for (size_t i = 0;; i++) {
    char ch = src[i];
    int at_end = (ch == '\0');
    if (!at_end) {
      if (in_str) {
        if (ch == '\\' && src[i + 1] != '\0') {
          i++;
        } else if (ch == quote) {
          in_str = 0;
          quote = '\0';
        }
      } else if (ch == '"' || ch == '\'') {
        in_str = 1;
        quote = ch;
      } else if (ch == ';' || ch == '#') {
        while (src[i] != '\0' && src[i] != '\n')
          i++;
        ch = src[i];
        at_end = (ch == '\0');
      } else {
        if (ch == '(')
          paren++;
        else if (ch == ')' && paren > 0)
          paren--;
        else if (ch == '[')
          brack++;
        else if (ch == ']' && brack > 0)
          brack--;
        else if (ch == '{')
          brace++;
        else if (ch == '}' && brace > 0)
          brace--;
      }
    }

    if (at_end || repl_stmt_is_boundary(src, i, paren, brack, brace, in_str)) {
      size_t stmt_end = at_end ? i : i + 1;
      if (stmt_end > stmt_start) {
        char *stmt = ny_strndup(src + stmt_start, stmt_end - stmt_start);
        if (stmt) {
          char *trimmed = repl_skip_leading_noncode(stmt);
          if (*trimmed && is_persistent_def(trimmed)) {
            char *persist = repl_trim_persistent_assignment(stmt);
            if (!persist)
              persist = ny_strdup(stmt);
            size_t stmt_len = persist ? strlen(persist) : 0;
            int needs_nl = (stmt_len > 0 && persist[stmt_len - 1] != '\n');
            size_t need = out_len + stmt_len + (needs_nl ? 1 : 0) + 1;
            if (need > out_cap) {
              out_cap = need + 256;
              out = realloc(out, out_cap);
              if (!out) {
                free(persist);
                free(stmt);
                return NULL;
              }
            }
            if (stmt_len > 0) {
              memcpy(out + out_len, persist, stmt_len);
              out_len += stmt_len;
              if (needs_nl)
                out[out_len++] = '\n';
              out[out_len] = '\0';
            }
            free(persist);
          }
          free(stmt);
        }
      }
      stmt_start = at_end ? i : i + 1;
    }

    if (at_end)
      break;
  }

  if (out_len == 0) {
    free(out);
    return NULL;
  }
  return out;
}

void repl_remove_def(const char *name) { (void)name; }

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
  int paren = 0, brack = 0, brace = 0;
  int in_str = 0;
  char quote = '\0';
  char *eq = NULL;
  for (char *p = trimmed; *p; ++p) {
    char ch = *p;
    if (in_str) {
      if (ch == '\\' && p[1]) {
        ++p;
        continue;
      }
      if (ch == quote) {
        in_str = 0;
        quote = '\0';
      }
      continue;
    }
    if (ch == '"' || ch == '\'') {
      in_str = 1;
      quote = ch;
      continue;
    }
    if (ch == '(')
      paren++;
    else if (ch == ')' && paren > 0)
      paren--;
    else if (ch == '[')
      brack++;
    else if (ch == ']' && brack > 0)
      brack--;
    else if (ch == '{')
      brace++;
    else if (ch == '}' && brace > 0)
      brace--;
    else if (ch == '=' && paren == 0 && brack == 0 && brace == 0) {
      char prev = (p > trimmed) ? *(p - 1) : '\0';
      char next = *(p + 1);
      if (prev == '=' || prev == '!' || prev == '<' || prev == '>' || next == '=')
        continue;
      eq = p;
      break;
    }
  }
  if (eq && eq != trimmed) {
    char *end = eq - 1;
    while (end >= trimmed && isspace((unsigned char)*end))
      end--;
    while (end >= trimmed && strchr("+-*/%&|^<>", *end))
      end--;
    while (end >= trimmed && isspace((unsigned char)*end))
      end--;
    if (end < trimmed)
      return NULL;
    char *ident_end = end;
    while (end >= trimmed && (isalnum((unsigned char)*end) || *end == '_' || *end == '.'))
      end--;
    char *ident_start = end + 1;
    if (ident_end >= ident_start && (*ident_start == '_' || isalpha((unsigned char)*ident_start))) {
      return ny_strndup(ident_start, (size_t)(ident_end - ident_start + 1));
    }
  }
  return NULL;
}

int is_repl_stmt(const char *src) {
  char *trimmed = ltrim((char *)src);
  if (!*trimmed)
    return 0;
  if (!strncmp(trimmed, "#include", 8) || !strncmp(trimmed, "#define", 7) ||
      !strncmp(trimmed, "#link", 5)) {
    return 1;
  }
  if (strchr(src, '{'))
    return 1;
  const char *kw[] = {"def ",    "fn ",    "use ",    "module ", "del ",     "if ",   "while ",
                      "for ",    "try ",   "return ", "break",   "continue", "goto ", "defer ",
                      "struct ", "class ", "layout ", "extern ", "operator ", "impl ", "macro ",
                      NULL};
  for (int i = 0; kw[i]; i++) {
    size_t len = strlen(kw[i]);
    if (strncmp(trimmed, kw[i], len) == 0) {
      if (kw[i][len - 1] == ' ' || trimmed[len] == '\0' || isspace((unsigned char)trimmed[len]) ||
          trimmed[len] == '(')
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

void count_unclosed(const char *src, int *out_paren, int *out_brack, int *out_brace,
                    int *out_in_str) {
  int p = 0, b = 0, c = 0, in_str = 0;
  char quote = 0;
  const char *s = src;
  while (*s) {
    if (in_str) {
      if (*s == '\\' && s[1]) {
        s += 2;
      } else if (*s == quote) {
        in_str = 0;
        quote = 0;
        s++;
      } else {
        s++;
      }
    } else {
      if (*s == '"' || *s == '\'') {
        in_str = 1;
        quote = *s;
        s++;
      } else if (*s == ';' || *s == '#') {
        while (*s && *s != '\n')
          s++;
      } else {
        if (*s == '(')
          p++;
        else if (*s == ')')
          p--;
        else if (*s == '[')
          b++;
        else if (*s == ']')
          b--;
        else if (*s == '{')
          c++;
        else if (*s == '}')
          c--;
        if (*s)
          s++;
      }
    }
  }
  *out_paren = p;
  *out_brack = b;
  *out_brace = c;
  if (out_in_str)
    *out_in_str = in_str;
}

int is_input_complete(const char *src) {
  int p, b, c, s;
  count_unclosed(src, &p, &b, &c, &s);
  return (p == 0 && b == 0 && c == 0 && s == 0);
}

void print_incomplete_hint(const char *src) {
  int p, b, c;
  count_unclosed(src, &p, &b, &c, NULL);
  if (p > 0 || b > 0 || c > 0) {
    printf("%s  ", clr(NY_CLR_GRAY));
    if (p > 0)
      printf("(missing %d ')') ", p);
    if (b > 0)
      printf("(missing %d ']') ", b);
    if (c > 0)
      printf("(missing %d '}') ", c);
    printf("(use :cancel to abort)");
    printf("%s\n", clr(NY_CLR_RESET));
  }
}

#ifndef _WIN32
#include <sys/ioctl.h>
#endif
#ifdef _WIN32
#include <windows.h>
int repl_is_input_pending(void) {
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  DWORD count = 0;
  if (GetNumberOfConsoleInputEvents(hIn, &count) && count > 0) {
    INPUT_RECORD recs[128];
    DWORD readCount = 0;
    DWORD toRead = count > 128 ? 128 : count;
    if (PeekConsoleInputA(hIn, recs, toRead, &readCount)) {
      for (DWORD i = 0; i < readCount; i++) {
        if (recs[i].EventType == KEY_EVENT && recs[i].Event.KeyEvent.bKeyDown) {
          return 1;
        }
      }
    }
  }
  return 0;
}
#else
int repl_is_input_pending(void) {
  int n = 0;
  if (ioctl(STDIN_FILENO, FIONREAD, &n) < 0)
    return 0;
  return n > 0;
}
#endif

int repl_calc_indent(const char *src) {
  int p, b, c;
  count_unclosed(src, &p, &b, &c, NULL);
  int level = p + b + c;
  return level > 0 ? level * 2 : 0;
}

int repl_pre_input_hook(void) { return 0; }

void repl_print_error_snippet(const char *src, int line, int col) {
  fprintf(stderr, "Error at %d:%d\n", line, col);
  if (!src || line <= 0)
    return;
  const char *line_start = NULL;
  size_t line_len = 0;
  int shown_line = line;
  if (!ny_extract_line(src, line, &line_start, &line_len) || !line_start) {
    if (line <= 1 || !ny_extract_line(src, line - 1, &line_start, &line_len) || !line_start)
      return;
    shown_line = line - 1;
    col = (int)line_len + 1;
  }
  while (line_len > 0 && (line_start[line_len - 1] == '\n' || line_start[line_len - 1] == '\r'))
    line_len--;
  fprintf(stderr, "%4d | %.*s\n", shown_line, (int)line_len, line_start);
  fprintf(stderr, "     | ");
  int caret_col = col > 0 ? col : 1;
  if (caret_col > (int)line_len + 1)
    caret_col = (int)line_len + 1;
  for (int i = 1; i < caret_col; ++i)
    fputc((i <= (int)line_len && line_start[i - 1] == '\t') ? '\t' : ' ', stderr);
  fputs("^\n", stderr);
}

char *repl_skip_leading_noncode(char *src) {
  char *p = src;
  while (p && *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
    if (*p == ';') {
      while (*p && *p != '\n')
        p++;
      continue;
    }
    break;
  }
  return p;
}

int is_persistent_def(const char *src) {
  char *trimmed = repl_skip_leading_noncode((char *)src);
  if (!strncmp(trimmed, "#include", 8) || !strncmp(trimmed, "#define", 7) ||
      !strncmp(trimmed, "#link", 5)) {
    return 1;
  }
  if (!strncmp(trimmed, "def ", 4) || !strncmp(trimmed, "mut ", 4) || !strncmp(trimmed, "fn ", 3) ||
      !strncmp(trimmed, "use ", 4) || !strncmp(trimmed, "module ", 7) ||
      !strncmp(trimmed, "extern ", 7) || !strncmp(trimmed, "enum ", 5) ||
      !strncmp(trimmed, "struct ", 7) || !strncmp(trimmed, "class ", 6) ||
      !strncmp(trimmed, "layout ", 7) || !strncmp(trimmed, "operator ", 9) ||
      !strncmp(trimmed, "impl ", 5) || !strncmp(trimmed, "macro ", 6)) {
    return 1;
  }
  char *an = repl_assignment_target(trimmed);
  if (an) {
    free(an);
    return 1;
  }
  return 0;
}

void repl_update_docs(doc_list_t *dl, const char *src) {
  if (!dl || !src || !*src)
    return;
  int handled_stmt_docs = 0;
  int handled_use_docs = 0;
  parser_t ps;
  parser_init(&ps, src, "<repl_docs>");
  program_t pr = parse_program(&ps);
  if (!ps.had_error) {
    for (size_t i = 0; i < pr.body.len; ++i) {
      stmt_t *s = pr.body.data[i];
      if (s->kind == NY_S_FUNC || s->kind == NY_S_MODULE) {
        handled_stmt_docs = 1;
      } else if (s->kind == NY_S_USE && s->as.use.module) {
        char *mod = repl_doc_normalize_module_name(s->as.use.module);
        if (mod) {
          repl_load_module_docs(dl, mod);
          free(mod);
        }
        handled_use_docs = 1;
      }
    }
    if (handled_stmt_docs)
      doclist_add_from_prog(dl, &pr);
  }
  program_free(&pr, ps.arena);

  char *trimmed = repl_skip_leading_noncode((char *)src);
  if (handled_stmt_docs &&
      (!strncmp(trimmed, "fn ", 3) || !strncmp(trimmed, "module ", 7))) {
    return;
  }
  if (handled_use_docs && !strncmp(trimmed, "use ", 4)) {
    return;
  }
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
