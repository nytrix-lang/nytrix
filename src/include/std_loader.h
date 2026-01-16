#pragma once

#include <stddef.h>

typedef enum nt_std_mode {
	NT_STD_NONE = 0,
	NT_STD_USE_LIST = 1,
	NT_STD_FULL = 2,
	NT_STD_PRELUDE = 3,
	NT_STD_LAZY = 4,
} nt_std_mode;

// Build a std bundle in dependency-safe order.
// modules: list of names (e.g., "std", "core", "io", "core.core", "std.io")
// mode: NONE / USE_LIST / FULL / PRELUDE
// Returns malloc'd string or NULL if none.
char *nt_build_std_bundle(const char **modules, size_t module_count, nt_std_mode mode, int verbose, const char *entry_path);

// Default REPL prelude list.
const char **nt_std_prelude(size_t *count);

// Generate function prototypes for stdlib mode.
char *nt_std_generate_header(nt_std_mode mode);

// Stdlib module/package names for tooling (REPL completion, etc).
size_t nt_std_module_count(void);
const char *nt_std_module_name(size_t idx);
const char *nt_std_module_path(size_t idx);
int nt_std_find_module_by_name(const char *name);
size_t nt_std_package_count(void);
const char *nt_std_package_name(size_t idx);
