/*
 * Runtime AST bridge: JSON-serializable AST representation used by
 * tooling, LSP, and debug introspection of the parse tree at runtime.
 */
#include "code/lang/parse/json.h"
#include "code/lang/parse/parser.h"
#include "code/lang/runtime/shared.h"
#include <stdlib.h>
#include <string.h>

#define NY_AST_PARSE_MAX_BYTES (1u << 20)

static int64_t rt_ast_json_string(const char *json) {
  size_t len = strlen(json);
  int64_t tagged_size = ((int64_t)(len + 1) << 1) | 1;
  int64_t res = rt_malloc(tagged_size);
  *(int64_t *)((char *)res - 8) = TAG_STR;
  *(int64_t *)((char *)res - 16) = ((int64_t)len << 1) | 1;
  memcpy((void *)res, json, len + 1);
  return res;
}

static int64_t rt_parse_ast_impl(int64_t source_ptr, bool versioned) {
  const char *source = (const char *)source_ptr;
  if (!source)
    return NY_IMM_NIL;
  /*
   * Runtime callers can pass either a tagged managed string or one of the
   * native string representations.  rt_cstr_len understands both layouts;
   * strlen alone can stop before the logical end of a native counted string.
   */
  int64_t source_len = rt_cstr_len(source_ptr);
  if (source_len < 0 || (uint64_t)source_len > NY_AST_PARSE_MAX_BYTES)
    return NY_IMM_NIL;
  parser_t parser;
  /*
   * Parsing is an inspection API: malformed input must not terminate the
   * running program or expose the parser's partially recovered tree.
   */
  parser_init_quiet(&parser, source, "<parse_ast>");
  parser.exit_on_limit = false;
  program_t prog = parse_program(&parser);
  if (parser.had_error) {
    program_free(&prog, parser.arena);
    return NY_IMM_NIL;
  }
  char *json =
      versioned ? ny_ast_to_json_versioned(&prog) : ny_ast_to_json(&prog);
  if (!json) {
    program_free(&prog, parser.arena);
    return NY_IMM_NIL;
  }
  int64_t res = rt_ast_json_string(json);
  program_free(&prog, parser.arena);
  rt_free((int64_t)(uintptr_t)json);
  return res;
}

int64_t rt_parse_ast(int64_t source_ptr) {
  return rt_parse_ast_impl(source_ptr, false);
}

int64_t rt_parse_ast_snapshot(int64_t source_ptr) {
  return rt_parse_ast_impl(source_ptr, true);
}

int64_t rt_parse_ast_raw(int64_t source_ptr) {
  const char *source = (const char *)source_ptr;
  if (!source)
    return NY_IMM_NIL;
  int64_t source_len = rt_cstr_len(source_ptr);
  if (source_len < 0 || (uint64_t)source_len > NY_AST_PARSE_MAX_BYTES)
    return NY_IMM_NIL;
  parser_t parser;
  /*
   * Inspection API: malformed input must not terminate the running
   * program.  Raw mode keeps compound assignments undesugared so an
   * out-of-parser pass can perform the desugar itself.
   */
  parser_init_quiet(&parser, source, "<parse_ast_raw>");
  parser.exit_on_limit = false;
  parser.raw_compound_assign = true;
  program_t prog = parse_program(&parser);
  if (parser.had_error) {
    program_free(&prog, parser.arena);
    return NY_IMM_NIL;
  }
  char *json = ny_ast_to_json(&prog);
  if (!json) {
    program_free(&prog, parser.arena);
    return NY_IMM_NIL;
  }
  int64_t res = rt_ast_json_string(json);
  program_free(&prog, parser.arena);
  rt_free((int64_t)(uintptr_t)json);
  return res;
}
