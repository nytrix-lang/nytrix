#pragma once

#include "base/common.h"
#include <stddef.h>

char *ny_build_std_bundle(const char **modules, size_t module_count,
                          std_mode_t mode, int verbose, const char *entry_path);
const char **ny_std_prelude(size_t *count);
char *ny_std_generate_header(std_mode_t mode);

size_t ny_std_module_count(void);
const char *ny_std_module_name(size_t idx);
const char *ny_std_module_path(size_t idx);
int ny_std_find_module_by_name(const char *name);
size_t ny_std_package_count(void);
const char *ny_std_package_name(size_t idx);
void ny_std_free_modules(void);
