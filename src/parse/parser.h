#ifndef NY_PARSER_H
#define NY_PARSER_H

#include "parse/ast.h"
#include "parse/lexer.h"
#include <stdint.h>

typedef struct parser_intern_entry {
  uint64_t hash;
  uint32_t len;
  const char *str;
} parser_intern_entry;

typedef struct parser_ct_layout_meta {
  const char *name;
  ny_layout_field_list fields;
} parser_ct_layout_meta;
typedef VEC(parser_ct_layout_meta) parser_ct_layout_meta_list;

typedef struct parser_ct_module_meta {
  const char *name;
  VEC(const char *) exports;
} parser_ct_module_meta;
typedef VEC(parser_ct_module_meta) parser_ct_module_meta_list;

typedef struct parser_ct_template_meta {
  const char *name;
  VEC(const char *) params;
  ny_stmt_list body;
} parser_ct_template_meta;
typedef VEC(parser_ct_template_meta) parser_ct_template_meta_list;

typedef struct parser_t {
  lexer_t lex;
  token_t cur;
  token_t prev;
  arena_t *arena;
  const char *src;
  const char *filename;
  char *current_module;
  const char *current_impl_owner;
  struct parser_intern_entry *intern_table;
  size_t intern_cap;
  size_t intern_len;
  int error_count;
  int lex_error_count_seen;
  int error_limit;
  bool had_error;
  bool skipped_newline;
  int last_error_line;
  int last_error_col;
  int last_error_end_col;
  char last_error_msg[256];
  char last_error_hint[256];
  const char *error_ctx;
  int block_depth;
  int loop_depth;
  bool quiet;
  bool exit_on_limit;
  parser_ct_layout_meta_list ct_layouts;
  parser_ct_module_meta_list ct_modules;
  parser_ct_template_meta_list ct_templates;
  ny_diag_rule_list ct_diag_rules;
} parser_t;

void parser_init(parser_t *p, const char *src, const char *filename);
void parser_init_with_arena(parser_t *p, const char *src, const char *filename, arena_t *arena);
void parser_init_with_arena_quiet(parser_t *p, const char *src, const char *filename,
                                  arena_t *arena);
void parser_init_quiet(parser_t *p, const char *src, const char *filename);
void parser_global_cleanup(void);
program_t parse_program(parser_t *p);

#endif
