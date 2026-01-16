#ifndef NYTRIX_REPL_COMPLETION_H
#define NYTRIX_REPL_COMPLETION_H

#include <stddef.h>

// Context-aware completion types
typedef enum {
	REPL_CTX_NORMAL,      // Normal code context
	REPL_CTX_AFTER_DOT,   // After a dot (member access)
	REPL_CTX_IMPORT,      // In a 'use' statement
	REPL_CTX_COMMAND,     // REPL command
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

extern repl_completion_state_t g_completion_state;

char *repl_enhanced_completion_generator(const char *text, int state);
char **repl_enhanced_completion(const char *text, int start, int end);

// Completion API for REPL commands
char **nytrix_get_completions_for_prefix(const char *prefix, size_t *out_count);
void nytrix_free_completions(char **completions, size_t count);
void completion_clear(void);
repl_context_t detect_completion_context(const char *line, int pos);
void add_command_completions(const char *prefix);
void add_stdlib_completions(const char *prefix);
void add_member_completions(const char *parent, const char *prefix);
void add_keywords_and_builtins(const char *prefix);
void add_stdlib_functions(const char *prefix);
void add_user_definitions(const char *prefix);

#endif // NYTRIX_REPL_COMPLETION_H
