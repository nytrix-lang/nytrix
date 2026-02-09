#ifndef REPL_INTERNAL_H
#define REPL_INTERNAL_H

#include "parse/parser.h"
#include "repl/repl.h"
#include "repl/types.h"
#include <stddef.h>

// Globals
extern const doc_list_t *g_repl_docs;
extern char *g_repl_user_source;
extern size_t g_repl_user_source_len;
extern int repl_indent_next;

// Doc Functions
void doclist_set(doc_list_t *dl, const char *name, const char *doc,
                 const char *def, const char *src, int kind);
void doclist_add_recursive(doc_list_t *dl, ny_stmt_list *body,
                           const char *prefix);
void doclist_add_from_prog(doc_list_t *dl, program_t *prog);
void add_builtin_docs(doc_list_t *docs);
int doclist_print(const doc_list_t *dl, const char *name);
void doclist_free(doc_list_t *dl);
void repl_load_module_docs(doc_list_t *docs, const char *name);

// Util Functions
char *repl_read_file(const char *path);
char **repl_split_lines(const char *src, size_t *out_count);
char *ltrim(char *s);
void rtrim_inplace(char *s);
int repl_is_input_pending(void);
void repl_append_user_source(const char *src);
void repl_remove_def(const char *name);
char *repl_assignment_target(const char *src);
int is_input_complete(const char *src);
void count_unclosed(const char *src, int *out_paren, int *out_brack,
                    int *out_brace, int *out_in_str);
void print_incomplete_hint(const char *src);
int repl_calc_indent(const char *src);
int is_repl_stmt(const char *src);
int repl_pre_input_hook(void);
void repl_highlight_line(const char *line);
void repl_highlight_line_ex(const char *line, int cursor_pos, int indent);
void repl_redisplay(void);
void repl_reset_redisplay(void);
void repl_display_match_list(char **matches, int len, int max);
char **repl_enhanced_completion(const char *text, int start, int end);
int is_persistent_def(const char *src);
void repl_update_docs(doc_list_t *dl, const char *src);
void repl_print_error_snippet(const char *src, int line, int col);

char **nytrix_get_completions_for_prefix(const char *prefix, size_t *out_count);
void nytrix_free_completions(char **completions, size_t count);

// Commands
int repl_handle_command(const char *cmd);

#endif
