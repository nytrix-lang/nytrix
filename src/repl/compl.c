#include "base/loader.h"
#include "parse/parser.h"
#include "priv.h"
#include "repl/types.h"
#include <ctype.h>
#include <dirent.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

  // Check if inside string
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

// Simple fuzzy score (higher is better)
static int fuzzy_score(const char *cand, const char *text) {
  if (!text || !*text)
    return 1;
  if (strcmp(cand, text) == 0)
    return 100;
  if (strncmp(cand, text, strlen(text)) == 0)
    return 50;
  if (strcasestr(cand, text))
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
  matches[matches_len++] = strdup(s);
}

// File completion helper
static void add_files(const char *text) {
  char dir_path[512] = ".";
  const char *prefix = text;
  const char *last_slash = strrchr(text, '/');
  if (last_slash) {
    size_t dlen = (size_t)(last_slash - text);
    if (dlen < sizeof(dir_path)) {
      memcpy(dir_path, text, dlen);
      dir_path[dlen] = '\0';
      if (dir_path[0] == '\0')
        strcpy(dir_path, "/");
    }
    prefix = last_slash + 1;
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
        snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
      }

      struct stat st;
      if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        strcat(full, "/");
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
  static int idx = 0;
  if (state == 0) {
    if (matches) {
      for (int i = 0; i < matches_len; i++)
        free(matches[i]);
      free(matches);
    }
    matches = NULL;
    matches_len = 0;
    matches_cap = 0;
    idx = 0;

    compl_ctx_t ctx = get_context(rl_line_buffer, rl_point);

    if (ctx == CTX_COMMAND) {
      static const char *cmds[] = {":help",    ":exit", ":quit", ":clear",
                                   ":reset",   ":time", ":vars", ":env",
                                   ":history", ":pwd",  ":ls",   ":cd",
                                   ":load",    ":save", ":std",  NULL};
      for (int i = 0; cmds[i]; i++) {
        if (strncmp(cmds[i], text, strlen(text)) == 0)
          add_match(cmds[i]);
      }
    } else if (ctx == CTX_STRING ||
               (ctx == CTX_COMMAND && (strstr(rl_line_buffer, ":load") ||
                                       strstr(rl_line_buffer, ":cd")))) {
      add_files(text);
    } else {
      add_normal_completions(text);
    }
  }

  if (matches && idx < matches_len) {
    return strdup(matches[idx++]);
  }
  return NULL;
}

char **repl_enhanced_completion(const char *text, int start, int end) {
  (void)start;
  (void)end;
  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, repl_enhanced_completion_generator);
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

void nytrix_free_completions(char **completions, size_t count) {
  if (!completions)
    return;
  for (size_t i = 0; i < count; i++)
    free(completions[i]);
  free(completions);
}
