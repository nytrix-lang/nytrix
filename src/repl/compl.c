#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/loader.h"
#ifdef _WIN32
#include "base/compat.h"
#endif
#include "base/util.h"
#include "parse/parser.h"
#include "priv.h"
#include "repl/types.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <sys/stat.h>

// Context detection
typedef enum {
  CTX_NORMAL,
  CTX_MEMBER,  // after .
  CTX_STRING,  // inside "..."
  CTX_COMMAND, // starts with :
  CTX_USE      // after 'use '
} compl_ctx_t;

static compl_ctx_t get_context(const char *line, int pos) {
  if (!line || pos < 0)
    return CTX_NORMAL;
  if (line[0] == ':')
    return CTX_COMMAND;

  int in_str = 0;
  for (int i = 0; i < pos; i++) {
    if (line[i] == '"' && (i == 0 || line[i - 1] != '\\'))
      in_str = !in_str;
  }
  if (in_str)
    return CTX_STRING;

  // Check 'use'
  if (pos >= 4 && strncmp(line, "use ", 4) == 0)
    return CTX_USE;

  // Check member
  if (pos > 0 && line[pos - 1] == '.')
    return CTX_MEMBER;
  for (int i = pos - 1; i >= 0; i--) {
    if (isspace((unsigned char)line[i]))
      break;
    if (line[i] == '.')
      return CTX_MEMBER;
  }

  return CTX_NORMAL;
}

static const char *strcasestr_impl(const char *haystack, const char *needle) {
  if (!needle || !*needle)
    return haystack;
  for (; *haystack; ++haystack) {
    if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
      const char *h, *n;
      for (h = haystack, n = needle; *h && *n; ++h, ++n) {
        if (tolower((unsigned char)*h) != tolower((unsigned char)*n)) {
          break;
        }
      }
      if (!*n) {
        return haystack;
      }
    }
  }
  return NULL;
}

static int is_break_char(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\\' ||
         c == '\'' || c == '`' || c == '@' || c == '$' || c == '>' ||
         c == '<' || c == '=' || c == ';' || c == '|' || c == '&' || c == '{' ||
         c == '}' || c == '.' || c == '(';
}

static void extract_prefix(const char *line, int pos, char *out,
                           size_t out_cap) {
  if (!out || out_cap == 0) {
    return;
  }
  out[0] = '\0';
  if (!line || pos <= 0) {
    return;
  }
  if ((size_t)pos > strlen(line))
    pos = (int)strlen(line);
  int start = pos;
  while (start > 0 && !is_break_char(line[start - 1])) {
    start--;
  }
  int len = pos - start;
  if (len <= 0)
    return;
  if ((size_t)len >= out_cap)
    len = (int)out_cap - 1;
  memcpy(out, line + start, (size_t)len);
  out[len] = '\0';
}

// Simple fuzzy score (higher is better)
static int fuzzy_score(const char *cand, const char *text) {
  if (!text || !*text)
    return 1;
  if (strcmp(cand, text) == 0)
    return 100;
  if (strncmp(cand, text, strlen(text)) == 0)
    return 50;
  if (strcasestr_impl(cand, text))
    return 10;
  return 0;
}

static char **matches = NULL;
static int matches_len = 0;
static int matches_cap = 0;

static void add_match(const char *s) {
  for (int i = 0; i < matches_len; i++) {
    if (strcmp(matches[i], s) == 0)
      return;
  }
  if (matches_len >= matches_cap) {
    matches_cap = matches_cap ? matches_cap * 2 : 64;
    matches = realloc(matches, matches_cap * sizeof(char *));
  }
  matches[matches_len++] = ny_strdup(s);
}

static void add_files(const char *text) {
  char dir_path[512] = ".";
  const char *prefix = text;
  const char *last_slash = strrchr(text, '/');
  const char *last_bslash = strrchr(text, '\\');
  const char *last_sep =
      (!last_slash)
          ? last_bslash
          : (!last_bslash
                 ? last_slash
                 : (last_bslash > last_slash ? last_bslash : last_slash));
  char sep = last_sep && *last_sep == '\\' ? '\\' : '/';
  if (last_sep) {
    size_t dlen = (size_t)(last_sep - text);
    if (dlen < sizeof(dir_path)) {
      memcpy(dir_path, text, dlen);
      dir_path[dlen] = '\0';
      if (dir_path[0] == '\0')
        strcpy(dir_path, "/");
      if (dlen == 2 && dir_path[1] == ':' && dlen + 1 < sizeof(dir_path)) {
        dir_path[dlen] = '\\';
        dir_path[dlen + 1] = '\0';
      }
    }
    prefix = last_sep + 1;
  }

  DIR *d = opendir(dir_path);
  if (!d)
    return;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.' && (prefix[0] != '.'))
      continue;
    if (strncmp(de->d_name, prefix, strlen(prefix)) == 0) {
      char full[1024];
      if (strcmp(dir_path, ".") == 0) {
        snprintf(full, sizeof(full), "%s", de->d_name);
      } else if (strcmp(dir_path, "/") == 0) {
        snprintf(full, sizeof(full), "/%s", de->d_name);
      } else {
        snprintf(full, sizeof(full), "%s%c%s", dir_path, sep, de->d_name);
      }

      struct stat st;
      if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t fl = strlen(full);
        if (fl + 2 < sizeof(full)) {
          full[fl] = sep;
          full[fl + 1] = '\0';
        }
      }
      add_match(full);
    }
  }
  closedir(d);
}

static void add_normal_completions(const char *text) {
  // Add stdlib
  size_t mod_count = ny_std_module_count();
  for (size_t i = 0; i < mod_count; i++) {
    const char *m = ny_std_module_name(i);
    if (fuzzy_score(m, text) > 0)
      add_match(m);
  }
  // Add keywords
  static const char *kws[] = {"fn",    "if",    "else",     "elif",   "while",
                              "for",   "in",    "return",   "use",    "try",
                              "catch", "break", "continue", "lambda", "defer",
                              "true",  "false", "nil",      "def",    "module",
                              "as",    NULL};
  for (int i = 0; kws[i]; i++) {
    if (strncmp(kws[i], text, strlen(text)) == 0)
      add_match(kws[i]);
  }
  // Add docs/definitions
  if (g_repl_docs) {
    const doc_list_t *d = (const doc_list_t *)g_repl_docs;
    for (size_t i = 0; i < d->len; i++) {
      if (fuzzy_score(d->data[i].name, text) > 0)
        add_match(d->data[i].name);
    }
  }
}

char *repl_enhanced_completion_generator(const char *text, int state) {
  (void)text;
  (void)state;
  return NULL;
}

char **repl_enhanced_completion(const char *text, int start, int end) {
  (void)text;
  (void)start;
  (void)end;
  return NULL;
}

// API for external completion requests (e.g. :complete command)
char **nytrix_get_completions_for_prefix(const char *prefix,
                                         size_t *out_count) {
  if (matches) {
    for (int i = 0; i < matches_len; i++)
      free(matches[i]);
    free(matches);
  }
  matches = NULL;
  matches_len = 0;
  matches_cap = 0;

  int is_cmd_pref = (prefix && prefix[0] == ':');
  int is_empty = (!prefix || !*prefix);

  if (is_cmd_pref || is_empty) {
    static const char *cmds[] = {
        ":help", ":exit", ":quit",    ":clear",    ":reset", ":time",
        ":vars", ":env",  ":history", ":pwd",      ":ls",    ":cd",
        ":load", ":save", ":std",     ":complete", NULL};
    for (int i = 0; cmds[i]; i++) {
      if (is_empty || strncmp(cmds[i], prefix, strlen(prefix)) == 0)
        add_match(cmds[i]);
    }
  }

  if (!is_cmd_pref) {
    add_normal_completions(prefix);
  }

  if (out_count)
    *out_count = (size_t)matches_len;

  char **res = matches;
  matches = NULL;
  matches_len = 0;
  matches_cap = 0;
  return res;
}

char **nytrix_get_completions_for_line(const char *line, int cursor,
                                       size_t *out_count) {
  if (matches) {
    for (int i = 0; i < matches_len; i++)
      free(matches[i]);
    free(matches);
  }
  matches = NULL;
  matches_len = 0;
  matches_cap = 0;

  char prefix[256];
  extract_prefix(line, cursor, prefix, sizeof(prefix));
  compl_ctx_t ctx = get_context(line, cursor);
  int is_cmd = (line && line[0] == ':');
  int wants_files = 0;
  if (ctx == CTX_STRING)
    wants_files = 1;
  if (ctx == CTX_COMMAND &&
      (strcasestr_impl(line, ":load") || strcasestr_impl(line, ":cd")))
    wants_files = 1;

  if ((ctx == CTX_COMMAND || is_cmd) && !wants_files) {
    static const char *cmds[] = {
        ":help", ":exit", ":quit",    ":clear",    ":reset", ":time",
        ":vars", ":env",  ":history", ":pwd",      ":ls",    ":cd",
        ":load", ":save", ":std",     ":complete", NULL};
    for (int i = 0; cmds[i]; i++) {
      if (!prefix[0] || strncmp(cmds[i], prefix, strlen(prefix)) == 0)
        add_match(cmds[i]);
    }
  } else if (wants_files) {
    add_files(prefix);
  } else {
    add_normal_completions(prefix);
  }

  if (out_count)
    *out_count = (size_t)matches_len;
  char **res = matches;
  matches = NULL;
  matches_len = 0;
  matches_cap = 0;
  return res;
}

void nytrix_free_completions(char **completions, size_t count) {
  if (!completions)
    return;
  for (size_t i = 0; i < count; i++)
    free(completions[i]);
  free(completions);
}
