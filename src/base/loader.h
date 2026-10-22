#pragma once

#include "base/common.h"
#include <stddef.h>
#include <stdint.h>

typedef uint64_t ny_module_id_t;
enum {
  NY_MODULE_FEATURE_NONE = 0,
  NY_MODULE_FEATURE_STDLIB = 1u << 0,
  NY_MODULE_FEATURE_USER = 1u << 1,
};
#define NY_MODULE_ID_UNKNOWN ((ny_module_id_t)0)

char *ny_build_std_source(const char **modules, size_t module_count, std_mode_t mode, int verbose,
                          const char *entry_path);
char *ny_build_std_source_ex(const char **modules, size_t module_count, std_mode_t mode,
                             int verbose, const char *entry_path, bool append_module_uses);
char *ny_std_generate_c_symbols_header(std_mode_t mode);

size_t ny_std_module_count(void);
ny_module_id_t ny_std_module_id(size_t idx);
uint32_t ny_std_module_features(size_t idx);
const char *ny_std_module_name(size_t idx);
const char *ny_std_module_path(size_t idx);
const char *ny_std_module_path_by_id(ny_module_id_t id);
int ny_std_find_module_by_name(const char *name);
bool ny_std_has_module_or_package(const char *name);
ny_module_id_t ny_std_find_module_id_by_path(const char *path);
ny_module_id_t ny_user_module_id(const char *name);
char *ny_read_declared_module_name(const char *path);
size_t ny_std_package_count(void);
const char *ny_std_package_name(size_t idx);
void ny_std_free_modules(void);
time_t ny_std_latest_source_mtime(void);
uint64_t ny_std_source_fingerprint(void);
