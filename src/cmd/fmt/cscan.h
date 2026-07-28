#ifndef NY_FMT_CSCAN_H
#define NY_FMT_CSCAN_H

#include <stddef.h>

typedef struct {
  char name[128];
  size_t start_offset;
  size_t end_offset;
  int start_line;
  int end_line;
  int is_static;
} CScanFunction;

typedef struct {
  CScanFunction *items;
  size_t len;
  size_t cap;
  int malformed;
} CScanFunctions;

/**
 * Scan C source for complete function definitions.
 *
 * The input is borrowed and need not be NUL-terminated beyond `len`. Results
 * own their storage and describe half-open source ranges. Comments, strings,
 * character literals, and preprocessor directives (including continuations)
 * are ignored. Malformed input yields the complete ranges found before the
 * error and sets `malformed`; it never invents an unterminated function.
 */
int cscan_functions(const char *src, size_t len, CScanFunctions *out);

/** Release storage owned by a result initialized by cscan_functions. */
void cscan_functions_free(CScanFunctions *out);

#endif
