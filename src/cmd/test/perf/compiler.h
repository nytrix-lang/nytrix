#ifndef NYT_PERF_COMPILER_H
#define NYT_PERF_COMPILER_H

#include <stdbool.h>
#include <stddef.h>

/* Creates a private executable beside source, preserving sibling resources.
 * The caller owns the returned path and removes it after all workers finish. */
bool nyt_compiler_snapshot(const char *source, char *path, size_t capacity,
                           char *error, size_t error_capacity);
int nyt_compiler_snapshot_selftest(void);

#endif
