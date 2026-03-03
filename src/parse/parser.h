#ifndef NY_PARSER_H
#define NY_PARSER_H

#include "ast/ast.h"
#include "lex/lexer.h"
#include <stdint.h>

typedef struct parser_intern_entry {
  uint64_t hash;
  uint32_t len;
  const char *str;
} parser_intern_entry;

typedef struct parser_t {
  lexer_t lex;
  token_t cur;
  token_t prev;
  arena_t *arena;
  const char *src;
  const char *filename;
  char *current_module;
  struct parser_intern_entry *intern_table;
  size_t intern_cap;
  size_t intern_len;
  int error_count;
  int error_limit;
  bool had_error;
  bool skipped_newline;
  int last_error_line;
  int last_error_col;
  char last_error_msg[256];
  const char *error_ctx;
  int block_depth;
  int loop_depth;
} parser_t;

void parser_init(parser_t *p, const char *src, const char *filename);
void parser_init_with_arena(parser_t *p, const char *src, const char *filename,
                            arena_t *arena);
program_t parse_program(parser_t *p);

#endif
