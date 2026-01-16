#pragma once

#include "base/common.h"
#include <stddef.h>

// Build a std bundle in dependency-safe order.
// modules: list of names (e.g., "std", "core", "io", "core.core", "std.io")
// mode: NONE / USE_LIST / FULL / PRELUDE
// Returns malloc'd string or NULL if none.
char *ny_build_std_bundle(const char **modules, size_t module_count,
                          std_mode_t mode, int verbose, const char *entry_path);

// Default REPL prelude list.
const char **ny_std_prelude(size_t *count);

// Generate function prototypes for stdlib mode.
char *ny_std_generate_header(std_mode_t mode);

// Stdlib module/package names for tooling (REPL completion, etc).
size_t ny_std_module_count(void);
const char *ny_std_module_name(size_t idx);
const char *ny_std_module_path(size_t idx);
int ny_std_find_module_by_name(const char *name);
size_t ny_std_package_count(void);
const char *ny_std_package_name(size_t idx);
