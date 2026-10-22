#ifndef NY_PARSER_H
#define NY_PARSER_H

#include "code/lang/parse/ast.h"
#include "code/lang/parse/lexer.h"
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
  /* Optional `code<expr|stmt|decl|ident>` boundary contracts. */
  VEC(const char *) param_categories;
  ny_stmt_list body;
} parser_ct_template_meta;
typedef VEC(parser_ct_template_meta) parser_ct_template_meta_list;

typedef struct parser_ct_decl_meta {
  const char *name;
  stmt_t *decl;
} parser_ct_decl_meta;
typedef VEC(parser_ct_decl_meta) parser_ct_decl_meta_list;

typedef struct parser_gensym_scope {
  const char *module;
  uint32_t counter;
  struct parser_gensym_scope *next;
} parser_gensym_scope;

typedef struct parser_t {
  lexer_t lex;
  token_t cur;
  token_t prev;
  arena_t *arena;
  const char *src;
  const char *filename;
  char *current_module;
  uint64_t current_module_id;
  uint32_t current_module_features;
  const char *current_impl_owner;
  struct parser_intern_entry *intern_table;
  size_t intern_cap;
  size_t intern_len;
  int error_count;
  int lex_error_count_seen;
  int error_limit;
  bool had_error;
  bool skipped_newline;
  bool stop_expr_at_newline;
  /* While parsing proof<proposition>, a closing '>' is also a comparison
   * token.  The Pratt parser uses this depth to leave the type delimiter for
   * the type parser once the following parameter/return syntax proves it is
   * not an operator. */
  int proof_type_depth;
  int last_error_line;
  int last_error_col;
  int last_error_end_col;
  char last_error_msg[256];
  char last_error_hint[256];
  const char *error_ctx;
  int block_depth;
  int loop_depth;
  int expr_depth;
  bool quiet;
  bool exit_on_limit;
  /* Parse compound assignments into NY_S_AUG statements instead of
   * desugaring them inline.  Used by the __parse_ast_raw inspection API so
   * an out-of-parser pass can perform the desugar; the normal compile path
   * keeps the in-parser desugar. */
  bool raw_compound_assign;
  parser_ct_layout_meta_list ct_layouts;
  parser_ct_module_meta_list ct_modules;
  parser_ct_template_meta_list ct_templates;
  parser_ct_decl_meta_list ct_decls;
  ny_diag_rule_list ct_diag_rules;
  uint32_t current_syntax_ctx;
  uint32_t next_syntax_ctx;
  unsigned ct_value_eval_depth;
  uint32_t gensym_counter;
  parser_gensym_scope *gensym_scopes;
  uint32_t syntax_ctx_stack[64];
  size_t syntax_ctx_depth;
} parser_t;

typedef struct parser_module_origin_scope_t {
  char *module;
  uint64_t module_id;
  uint32_t module_features;
} parser_module_origin_scope_t;

const char *ny_parser_gensym(parser_t *p, const char *prefix);
parser_ct_layout_meta *parser_find_layout_meta(parser_t *p, const char *name);
void parser_record_ct_decl(parser_t *p, stmt_t *stmt);

void parser_init(parser_t *p, const char *src, const char *filename);
void parser_add_module_features(parser_t *p, uint32_t features);
parser_module_origin_scope_t
parser_enter_module_origin(parser_t *p, const char *name, token_t *module_tok);
void parser_leave_module_origin(parser_t *p,
                                parser_module_origin_scope_t scope);
void parser_init_with_arena(parser_t *p, const char *src, const char *filename,
                            arena_t *arena);
void parser_init_with_arena_quiet(parser_t *p, const char *src,
                                  const char *filename, arena_t *arena);
void parser_init_quiet(parser_t *p, const char *src, const char *filename);
void parser_global_cleanup(void);
program_t parse_program(parser_t *p);

#endif
