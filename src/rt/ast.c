#include "ast/json.h"
#include "parse/parser.h"
#include <stdlib.h>
#include <string.h>

extern int64_t __malloc(int64_t n);

#ifndef NYTRIX_RUNTIME_ONLY
int64_t __parse_ast(int64_t source_ptr) {
  const char *source = (const char *)source_ptr;
  if (!source)
    return 0;
  parser_t parser;
  parser_init(&parser, source, "<parse_ast>");
  program_t prog = parse_program(&parser);
  char *json = ny_ast_to_json(&prog);
  if (!json) {
    arena_free(parser.arena);
    return 0;
  }
  size_t len = strlen(json);
  int64_t tagged_size = ((int64_t)(len + 1) << 1) | 1;
  int64_t res = __malloc(tagged_size);
  *(int64_t *)((char *)res - 8) = 241;                      // TAG_STR (241)
  *(int64_t *)((char *)res - 16) = ((int64_t)len << 1) | 1; // Length
  memcpy((void *)res, json, len + 1);
  arena_free(parser.arena);
  // free(json); // json is likely in arena_t
  return res;
}
#else
int64_t __parse_ast(int64_t source_ptr) {
  (void)source_ptr;
  return 0; // Return None in AOT mode without compiler
}
#endif
