/* Native C/Nytrix clone and refactoring-intelligence engine for ny fmt. */
#ifndef NY_FMT_CLONE_H
#define NY_FMT_CLONE_H

#include <stddef.h>

typedef struct {
  /* Inputs selected by the formatter CLI.  Empty paths use repository roots. */
  const char *const *paths;
  size_t path_count;
  /* "all", "c" or "ny". */
  const char *lang;
  /* "exact", "renamed", "literal", "near", "structural" or "all". */
  const char *match;
  size_t min_tokens;
  int min_lines;
  double similarity;
  int explain;
  int emit;
  int json;
  int limit;
  const char *baseline;
  int fail_on_new;
  int cross_lang;
  int verbose;
  /* Legacy --dupes-min: minimum normalized (token-spelling) bytes. */
  int legacy_min_bytes;
  int legacy_min_set;
} CloneOptions;

/*
 * Run a read-only clone scan.  Returns 0 on success, 1 when source
 * collection/reading fails or --dupes-fail-on-new finds a new family, and 2
 * for invalid options.
 */
int clone_run(const CloneOptions *opts);

/* Clone-engine regression suite, invoked by `ny fmt --selftest`. */
int clone_selftest(void);

#endif
