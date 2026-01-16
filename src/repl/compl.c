#include "base/loader.h"
#include "parse/parser.h"
#include "priv.h"
#include "repl/types.h"
#include <ctype.h>
#include <readline/readline.h>
#include <stdlib.h>
#include <string.h>

// Context-aware completion types
typedef enum {
  REPL_CTX_NORMAL,    // Normal code context
  REPL_CTX_AFTER_DOT, // After a dot (member access)
  REPL_CTX_IMPORT,    // In a 'use' statement
  REPL_CTX_COMMAND,   // REPL command
} repl_context_t;

// Structure to hold completion state
typedef struct {
  char **items;
  size_t len;
  size_t cap;
  size_t idx;
  repl_context_t context;
  char *parent;
} repl_completion_state_t;

static repl_completion_state_t g_completion_state = {0};

// Duplicate a name for completion
static char *completion_dup(const char *name) {
  if (!name)
    return NULL;
  size_t n = strlen(name);
  char *out = malloc(n + 1);
  if (!out)
    return NULL;
  memcpy(out, name, n);
  out[n] = '\0';
  return out;
}

// Add item to completion list
static void completion_add(const char *name) {
  if (!name || !*name)
    return;
  // Check for duplicates
  for (size_t i = 0; i < g_completion_state.len; i++) {
    if (strcmp(g_completion_state.items[i], name) == 0) {
      return; // Already in list
    }
  }
  if (g_completion_state.len == g_completion_state.cap) {
    size_t new_cap = g_completion_state.cap ? g_completion_state.cap * 2 : 64;
    char **new_items =
        realloc(g_completion_state.items, new_cap * sizeof(char *));
    if (!new_items)
      return;
    g_completion_state.items = new_items;
    g_completion_state.cap = new_cap;
  }
  g_completion_state.items[g_completion_state.len++] = completion_dup(name);
}

// Clear completion state
static void completion_clear(void) {
  if (g_completion_state.items) {
    for (size_t i = 0; i < g_completion_state.len; i++) {
      free(g_completion_state.items[i]);
    }
    free(g_completion_state.items);
  }
  if (g_completion_state.parent) {
    free(g_completion_state.parent);
    g_completion_state.parent = NULL;
  }
  g_completion_state.items = NULL;
  g_completion_state.len = 0;
  g_completion_state.cap = 0;
  g_completion_state.idx = 0;
}

// Detect context from line buffer
static repl_context_t detect_completion_context(const char *line, int pos) {
  if (!line || pos <= 0)
    return REPL_CTX_NORMAL;
  // Check for REPL command
  if (line[0] == ':')
    return REPL_CTX_COMMAND;
  // Check for 'use' statement
  const char *use_pos = strstr(line, "use ");
  if (use_pos && (use_pos - line) < pos) {
    return REPL_CTX_IMPORT;
  }
  // Check for dot (member access)
  // Scan backwards from pos to find dot
  for (int i = pos - 1; i >= 0; i--) {
    if (line[i] == '.') {
      // Make sure it's not in a number (like 3.14)
      if (i > 0 && !isdigit((unsigned char)line[i - 1])) {
        // Extract parent (from start of word to dot)
        int start = i - 1;
        while (start >= 0 && (isalnum((unsigned char)line[start]) ||
                              line[start] == '_' || line[start] == '.')) {
          start--;
        }
        start++;
        size_t len = i - start;
        if (len > 0) {
          g_completion_state.parent = malloc(len + 1);
          memcpy(g_completion_state.parent, line + start, len);
          g_completion_state.parent[len] = '\0';
        }
        return REPL_CTX_AFTER_DOT;
      }
    }
    if (!isalnum((unsigned char)line[i]) && line[i] != '_' && line[i] != '.') {
      break;
    }
  }
  return REPL_CTX_NORMAL;
}

// Add standard library modules for 'use' completion
static void add_stdlib_completions(const char *prefix) {
  // Add "std" itself
  if (!prefix || strncmp("std", prefix, strlen(prefix)) == 0) {
    completion_add("std");
  }
  // Add packages
  size_t pkg_count = ny_std_package_count();
  for (size_t i = 0; i < pkg_count; i++) {
    const char *pkg = ny_std_package_name(i);
    if (pkg && (!prefix || strncmp(pkg, prefix, strlen(prefix)) == 0)) {
      completion_add(pkg);
    }
  }
  // Add modules
  size_t mod_count = ny_std_module_count();
  for (size_t i = 0; i < mod_count; i++) {
    const char *mod = ny_std_module_name(i);
    if (mod && (!prefix || strncmp(mod, prefix, strlen(prefix)) == 0)) {
      completion_add(mod);
    }
  }
}

// Add REPL commands
static void add_command_completions(const char *prefix) {
  static const char *commands[] = {
      ":help",  ":h",    ":exit", ":quit",    ":q",    ":clear", ":cls",
      ":reset", ":vars", ":env",  ":history", ":hist", ":pwd",   ":ls",
      ":cd",    ":load", ":save", ":std",     ":doc",  ":time",  NULL};
  size_t prefix_len = prefix ? strlen(prefix) : 0;
  for (int i = 0; commands[i]; i++) {
    if (!prefix || strncmp(commands[i], prefix, prefix_len) == 0) {
      completion_add(commands[i]);
    }
  }
}

// Add keywords and builtins
static void add_keywords_and_builtins(const char *prefix) {
  static const char *keywords[] = {
      "fn",       "if",     "else",  "elif", "while", "for",
      "in",       "return", "use",   "try",  "catch", "break",
      "continue", "lambda", "defer", "true", "false", "goto",
      "and",      "or",     "not",   "nil",  NULL};
  size_t prefix_len = prefix ? strlen(prefix) : 0;
  for (int i = 0; keywords[i]; i++) {
    if (!prefix || strncmp(keywords[i], prefix, prefix_len) == 0) {
      completion_add(keywords[i]);
    }
  }
}

// Add standard library functions from docs
static void add_stdlib_functions(const char *prefix) {
  if (g_repl_docs) {
    const doc_list_t *docs = (const doc_list_t *)g_repl_docs;
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    for (size_t i = 0; i < docs->len; ++i) {
      if (!prefix || strncmp(docs->data[i].name, prefix, prefix_len) == 0) {
        completion_add(docs->data[i].name);
      }
    }
  } else {
    // Fallback if docs not loaded
    static const char *common_funcs[] = {"print", "println", "input", "len",
                                         "range", "int",     "float", "str",
                                         "bool",  "type",    NULL};
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    for (int i = 0; common_funcs[i]; i++) {
      if (!prefix || strncmp(common_funcs[i], prefix, prefix_len) == 0) {
        completion_add(common_funcs[i]);
      }
    }
  }
}

// Add user-defined functions/variables
static void add_user_definitions(const char *prefix) {
  // Use g_repl_user_source from repl.h (defined in repl.c / util.c)
  if (!g_repl_user_source || !*g_repl_user_source)
    return;
  parser_t parser;
  parser_init(&parser, g_repl_user_source, "<repl_user>");
  parser.error_limit = 0; // Don't spam errors during completion
  // We need to parse the whole user code to find definitions
  program_t prog = parse_program(&parser);
  if (!parser.had_error) {
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    for (size_t i = 0; i < prog.body.len; i++) {
      stmt_t *s = prog.body.data[i];
      if (s->kind == NY_S_FUNC) {
        if (!prefix || strncmp(s->as.fn.name, prefix, prefix_len) == 0) {
          completion_add(s->as.fn.name);
        }
      } else if (s->kind == NY_S_VAR) {
        for (size_t j = 0; j < s->as.var.names.len; j++) {
          const char *name = s->as.var.names.data[j];
          if (!prefix || strncmp(name, prefix, prefix_len) == 0) {
            completion_add(name);
          }
        }
      }
    }
  }
  program_free(&prog, parser.arena);
}

static void add_member_completions(const char *parent, const char *prefix) {
  if (!parent)
    return;
  size_t parent_len = strlen(parent);
  size_t prefix_len = prefix ? strlen(prefix) : 0;
  // Check packages/modules
  size_t mod_count = ny_std_module_count();
  for (size_t i = 0; i < mod_count; ++i) {
    const char *mod = ny_std_module_name(i);
    // Check if mod starts with parent + "."
    if (strncmp(mod, parent, parent_len) == 0 && mod[parent_len] == '.') {
      const char *rest = mod + parent_len + 1;
      // Check if rest matches prefix
      if (!prefix || strncmp(rest, prefix, prefix_len) == 0) {
        // We only want the next segment
        const char *dot = strchr(rest, '.');
        if (dot) {
          size_t seg_len = dot - rest;
          char *seg = malloc(seg_len + 1);
          if (seg) {
            memcpy(seg, rest, seg_len);
            seg[seg_len] = 0;
            completion_add(seg);
            free(seg);
          }
        } else {
          completion_add(rest);
        }
      }
    }
  }
}

// Main completion generator function
char *repl_enhanced_completion_generator(const char *text, int state) {
  if (state == 0) {
    // Initialize completion state
    completion_clear();
    g_completion_state.idx = 0;
    // Detect context
    g_completion_state.context =
        detect_completion_context(rl_line_buffer, rl_point);
    // Build completion list based on context
    switch (g_completion_state.context) {
    case REPL_CTX_COMMAND:
      add_command_completions(text);
      break;
    case REPL_CTX_IMPORT:
      add_stdlib_completions(text);
      break;
    case REPL_CTX_AFTER_DOT:
      add_member_completions(g_completion_state.parent, text);
      break;
    case REPL_CTX_NORMAL:
    default:
      add_keywords_and_builtins(text);
      add_stdlib_functions(text);
      add_user_definitions(text);
      break;
    }
  }
  // Return next matching completion
  while (g_completion_state.idx < g_completion_state.len) {
    char *candidate = g_completion_state.items[g_completion_state.idx++];
    if (strncmp(candidate, text, strlen(text)) == 0) {
      return completion_dup(candidate);
    }
  }
  // No more completions
  if (state == 0) {
    completion_clear();
  }
  return NULL;
}

// Readline completion function wrapper
char **repl_enhanced_completion(const char *text, int start, int end) {
  (void)start;
  (void)end;
  // Disable filename completion by default
  rl_attempted_completion_over = 1;
  // Check for special contexts that need filename completion
  if (rl_line_buffer && rl_line_buffer[0] == ':') {
    if (strncmp(rl_line_buffer, ":load ", 6) == 0 ||
        strncmp(rl_line_buffer, ":save ", 6) == 0 ||
        strncmp(rl_line_buffer, ":cd ", 4) == 0 ||
        strncmp(rl_line_buffer, ":ls ", 4) == 0) {
      rl_attempted_completion_over = 0; // Enable default completion (filenames)
      return NULL;
    }
  }
  // Use our custom generator
  char **matches =
      rl_completion_matches(text, repl_enhanced_completion_generator);

  // Clean up completion state after generating all matches
  if (!matches || start > 0) {
    completion_clear();
  }

  return matches;
}

// Completion API for REPL commands (used by Emacs integration)
char **nytrix_get_completions_for_prefix(const char *prefix,
                                         size_t *out_count) {
  if (!prefix) {
    if (out_count)
      *out_count = 0;
    return NULL;
  }

  // Clear and initialize completion state
  completion_clear();
  g_completion_state.idx = 0;

  // Detect context - use simple heuristic since we don't have full line
  repl_context_t ctx = REPL_CTX_NORMAL;
  const char *actual_prefix = prefix;

  if (prefix[0] == ':') {
    ctx = REPL_CTX_COMMAND;
  } else if (strchr(prefix, '.')) {
    ctx = REPL_CTX_AFTER_DOT;
    // Extract parent for member access
    const char *dot = strrchr(prefix, '.');
    if (dot && dot > prefix) {
      size_t len = dot - prefix;
      g_completion_state.parent = malloc(len + 1);
      if (g_completion_state.parent) {
        memcpy(g_completion_state.parent, prefix, len);
        g_completion_state.parent[len] = '\0';
      }
      actual_prefix = dot + 1; // Only complete after the dot
    }
  }

  // Build completion list based on context
  switch (ctx) {
  case REPL_CTX_COMMAND:
    add_command_completions(actual_prefix);
    break;

  case REPL_CTX_IMPORT:
    add_stdlib_completions(actual_prefix);
    break;

  case REPL_CTX_AFTER_DOT:
    add_member_completions(g_completion_state.parent, actual_prefix);
    break;

  case REPL_CTX_NORMAL:
  default:
    add_keywords_and_builtins(actual_prefix);
    add_stdlib_functions(actual_prefix);
    add_user_definitions(actual_prefix);
    break;
  }

  // Copy results
  size_t count = g_completion_state.len;
  if (out_count)
    *out_count = count;

  if (count == 0) {
    completion_clear();
    return NULL;
  }

  // Allocate array for results (caller must free)
  char **results =
      malloc((count + 1) * sizeof(char *)); // +1 for NULL terminator
  if (!results) {
    completion_clear();
    if (out_count)
      *out_count = 0;
    return NULL;
  }

  // Copy completions
  for (size_t i = 0; i < count; i++) {
    results[i] = strdup(g_completion_state.items[i]);
    if (!results[i]) {
      // Handle allocation failure
      for (size_t j = 0; j < i; j++) {
        free(results[j]);
      }
      free(results);
      completion_clear();
      if (out_count)
        *out_count = 0;
      return NULL;
    }
  }
  results[count] = NULL; // NULL terminator for safety

  completion_clear();
  return results;
}

void nytrix_free_completions(char **completions, size_t count) {
  if (!completions)
    return;
  for (size_t i = 0; i < count; i++) {
    free(completions[i]);
  }
  free(completions);
}
