/* Lightweight C-source function scanner used by formatter tooling. */
#ifndef NY_FMT_CSCAN_H
#define NY_FMT_CSCAN_H

#include <stddef.h>

/*
 * One complete C function definition.
 *
 * All offsets are half-open byte ranges into the scanned buffer.  `start_offset`
 * covers the declaration prefix (storage class, return type, declarator and
 * attributes); `end_offset` is the byte just past the closing brace.
 * `body_open_offset` points at `{` and `body_close_offset` at `}`.
 */
typedef struct {
  char name[128];
  size_t start_offset;
  size_t end_offset;
  int start_line;
  int end_line;
  int is_static;
  size_t name_offset;
  size_t body_open_offset;
  size_t body_close_offset;
  int nesting_depth;
  int uncertain;
} CScanFunction;

typedef struct {
  CScanFunction *items;
  size_t len;
  size_t cap;
  /* Set when the buffer ended inside a comment, string, or function body. */
  int malformed;
  /* Set when raw conditional-compilation branches leave braces unbalanced. */
  int preprocessing_ambiguous;
} CScanFunctions;

/**
 * Scan C source for complete function definitions.
 *
 * The input is borrowed and need not be NUL-terminated beyond `len`. Results
 * own their storage and describe half-open source ranges. Comments, strings,
 * character literals, and preprocessor directives (including continuations)
 * are ignored. Malformed input yields the complete ranges found before the
 * error and sets `malformed`; it never invents an unterminated function.
 *
 * Declaration recognition handles GNU/Clang attributes, multiline
 * declarations, parenthesized declarators, function-pointer return types,
 * static inline definitions and nested (GNU) function definitions.  This is a
 * lightweight scanner, not a replacement for a complete C parser: syntax it
 * cannot attribute confidently is skipped rather than guessed at.
 */
int cscan_functions(const char *src, size_t len, CScanFunctions *out);

/** Release storage owned by a result initialized by cscan_functions. */
void cscan_functions_free(CScanFunctions *out);

#endif
