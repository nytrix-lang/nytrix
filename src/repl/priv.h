#ifndef REPL_INTERNAL_H
#define REPL_INTERNAL_H

#include "parse/parser.h"
#include "repl/repl.h"
#include "base/compat.h"
#include <ctype.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

#include "repl/types.h"
#include <stddef.h>

extern const doc_list_t *g_repl_docs;
extern char *g_repl_user_source;
extern size_t g_repl_user_source_len;
extern int repl_indent_next;

static inline int repl_starts_with_ci(const char *s, const char *prefix) {
  if (!s || !prefix)
    return 0;
  while (*prefix) {
    if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
      return 0;
    s++;
    prefix++;
  }
  return 1;
}

static inline const char *repl_strcasestr(const char *hay, const char *needle) {
  if (!hay || !needle)
    return NULL;
  if (!*needle)
    return hay;
  size_t nlen = strlen(needle);
  for (const char *p = hay; *p; ++p) {
    if (tolower((unsigned char)*p) == tolower((unsigned char)needle[0]) &&
        strncasecmp(p, needle, nlen) == 0)
      return p;
  }
  return NULL;
}

static inline int repl_fuzzy_boundary_bonus(char prev) {
  if (prev == '\0')
    return 35;
  if (prev == '.' || prev == '_' || prev == '-' || prev == '/' || prev == '\\')
    return 28;
  if (isspace((unsigned char)prev) || prev == ':' || prev == '(' || prev == '[')
    return 18;
  return 0;
}

static inline int repl_fuzzy_score(const char *cand, const char *query,
                                   int allow_substring) {
  if (!cand)
    return 0;
  if (!query || !*query)
    return 1;
  if (strcmp(cand, query) == 0)
    return 2000;
  if (strcasecmp(cand, query) == 0)
    return 1900;
  size_t qlen = strlen(query);
  if (strncasecmp(cand, query, qlen) == 0)
    return 1600 - (int)(strlen(cand) > 120 ? 120 : strlen(cand));

  int score = 0;
  if (allow_substring) {
    const char *sub = repl_strcasestr(cand, query);
    if (sub) {
      int off = (int)(sub - cand);
      score = 900 - (off > 120 ? 120 : off);
      score += repl_fuzzy_boundary_bonus(off > 0 ? cand[off - 1] : '\0');
    }
  }

  int ci = 0, last = -1, first = -1, fuzzy = 0;
  for (int qi = 0; query[qi]; ++qi) {
    unsigned char q = (unsigned char)tolower((unsigned char)query[qi]);
    int found = -1;
    while (cand[ci]) {
      unsigned char c = (unsigned char)tolower((unsigned char)cand[ci]);
      if (c == q) {
        found = ci++;
        break;
      }
      ci++;
    }
    if (found < 0)
      return allow_substring ? score : 0;
    if (first < 0)
      first = found;
    fuzzy += 35;
    if (last >= 0 && found == last + 1)
      fuzzy += 55;
    if (!allow_substring && found == qi)
      fuzzy += 25;
    fuzzy += repl_fuzzy_boundary_bonus(found > 0 ? cand[found - 1] : '\0');
    fuzzy -= found > 120 ? 120 : found;
    last = found;
  }
  if (first == 0)
    fuzzy += 120;
  size_t clen = strlen(cand);
  fuzzy -= clen > 160 ? 160 : (int)(clen / 2);
  return fuzzy > score ? fuzzy : score;
}

void doclist_set(doc_list_t *dl, const char *name, const char *doc, const char *def,
                 const char *src, int kind);
void doclist_add_recursive(doc_list_t *dl, ny_stmt_list *body, const char *prefix);
void doclist_add_from_prog(doc_list_t *dl, program_t *prog);
void add_builtin_docs(doc_list_t *docs);
int doclist_print(const doc_list_t *dl, const char *name);
void doclist_free(doc_list_t *dl);
void repl_load_module_docs(doc_list_t *docs, const char *name);
void repl_ensure_docs_for_query(doc_list_t *docs, const char *query);

char *repl_read_file(const char *path);
int repl_write_session_source(const char *path);
char *repl_skip_leading_noncode(char *src);
char *repl_mask_main_guards(const char *src);
char **repl_split_lines(const char *src, size_t *out_count);
char *ltrim(char *s);
void rtrim_inplace(char *s);
int repl_head_is_number(const char *s);
int repl_is_input_pending(void);
int repl_wait_input_brief(int ms);
void repl_flush_pending_stdin(void);
void repl_append_user_source(const char *src);
void repl_set_user_source(const char *src);
char *repl_extract_persistent_source(const char *src);
void repl_remove_def(const char *name);
char *repl_assignment_target(const char *src);
int is_input_complete(const char *src);
void count_unclosed(const char *src, int *out_paren, int *out_brack, int *out_brace,
                    int *out_in_str);
void print_incomplete_hint(const char *src);
int repl_calc_indent(const char *src);
int is_repl_stmt(const char *src);
int repl_pre_input_hook(void);
enum {
  REPL_SEL_NONE = 0,
  REPL_SEL_LINEAR = 1,
  REPL_SEL_BLOCK = 2,
};

void repl_highlight_line(const char *line);
void repl_highlight_line_ex(const char *line, int cursor_pos, const char *ml_prompt, int sel_start,
                            int sel_end, int sel_mode);
void repl_redisplay(void);
void repl_reset_redisplay(void);
void repl_display_match_list(char **matches, int len, int max);
char **repl_enhanced_completion(const char *text, int start, int end);
int is_persistent_def(const char *src);
void repl_update_docs(doc_list_t *dl, const char *src);
void repl_print_error_snippet(const char *src, int line, int col);

char **nytrix_get_completions_for_prefix(const char *prefix, size_t *out_count);
char **nytrix_get_completions_for_line(const char *line, int cursor, size_t *out_count);
void nytrix_free_completions(char **completions, size_t count);

int repl_handle_command(const char *cmd);

#endif
