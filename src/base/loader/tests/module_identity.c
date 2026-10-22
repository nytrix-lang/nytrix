#include "base/loader.h"
#include "code/lang/parse/parser.h"

#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

int main(void) {
  int core_index = ny_std_find_module_by_name("core");
  if (core_index < 0) {
    fprintf(stderr, "[module_identity] core is not registered\n");
    return 1;
  }

  ny_module_id_t expected = ny_std_module_id((size_t)core_index);
  const char *registered = ny_std_module_path((size_t)core_index);
  if (expected == NY_MODULE_ID_UNKNOWN || !registered || !*registered) {
    fprintf(stderr, "[module_identity] invalid registered core record\n");
    return 1;
  }
  if (ny_std_module_path_by_id(expected) != registered ||
      ny_std_module_path_by_id(NY_MODULE_ID_UNKNOWN) != NULL) {
    fprintf(stderr, "[module_identity] registered module ID did not resolve "
                    "its source path\n");
    return 1;
  }
  if (ny_std_find_module_id_by_path(registered) != expected) {
    fprintf(stderr, "[module_identity] exact path lookup failed\n");
    return 1;
  }

  char normalized_registered[4096];
  if (strlen(registered) >= sizeof(normalized_registered))
    return 1;
  for (size_t i = 0; registered[i]; ++i)
    normalized_registered[i] = registered[i] == '\\' ? '/' : registered[i];
  normalized_registered[strlen(registered)] = '\0';
  const char *relative_suffix = strstr(normalized_registered, "/lib/mod.ny");
  if (!relative_suffix) {
    fprintf(
        stderr,
        "[module_identity] registered core path has no source-root suffix\n");
    return 1;
  }
  relative_suffix++;
  if (ny_std_find_module_id_by_path(relative_suffix) != expected ||
      ny_std_find_module_id_by_path("lib/./mod.ny") != expected) {
    fprintf(
        stderr,
        "[module_identity] source-root-relative paths lost module identity\n");
    return 1;
  }

  char alternate_separators[4096];
  memcpy(alternate_separators, registered, strlen(registered) + 1);
  for (size_t i = 0; alternate_separators[i]; ++i)
    if (alternate_separators[i] == '/')
      alternate_separators[i] = '\\';
  if (ny_std_find_module_id_by_path(alternate_separators) != expected) {
    fprintf(
        stderr,
        "[module_identity] alternate path separators lost module identity\n");
    return 1;
  }

#ifdef _WIN32
  char case_alias[4096];
  memcpy(case_alias, normalized_registered, strlen(normalized_registered) + 1);
  for (size_t i = 0; case_alias[i]; ++i) {
    if (case_alias[i] >= 'a' && case_alias[i] <= 'z')
      case_alias[i] = (char)(case_alias[i] - 'a' + 'A');
    else if (case_alias[i] >= 'A' && case_alias[i] <= 'Z')
      case_alias[i] = (char)(case_alias[i] - 'A' + 'a');
  }
  if (ny_std_find_module_id_by_path(case_alias) != expected) {
    fprintf(stderr,
            "[module_identity] case-insensitive path lost module identity\n");
    return 1;
  }
#endif

  if (ny_std_find_module_id_by_path(
          "src/base/loader/tests/stdlib-debug-path.ny") !=
      NY_MODULE_ID_UNKNOWN) {
    fprintf(stderr,
            "[module_identity] user fixture path inherited stdlib identity\n");
    return 1;
  }

  parser_t parser;
  parser_init(&parser, "module core()", "<stdlib>");
  parser_add_module_features(&parser, NY_MODULE_FEATURE_STDLIB);
  token_t synthetic_module = parser.cur;
  parser_module_origin_scope_t previous =
      parser_enter_module_origin(&parser, "core", &synthetic_module);
  if (parser.current_module_id != expected ||
      synthetic_module.module_id != expected ||
      synthetic_module.module_features != NY_MODULE_FEATURE_STDLIB) {
    fprintf(stderr, "[module_identity] synthetic stdlib source did not resolve "
                    "its registered module ID\n");
    return 1;
  }
  parser_leave_module_origin(&parser, previous);
  parser_global_cleanup();

  char parent[4096];
  char alias[4096];
  size_t path_len = strlen(registered);
  if (path_len >= sizeof(parent))
    return 1;
  memcpy(parent, registered, path_len + 1);
  char *slash = strrchr(parent, '/');
  if (!slash)
    slash = strrchr(parent, '\\');
  if (!slash || slash == parent)
    return 1;
  char filename[256];
  size_t filename_len = strlen(slash + 1);
  if (filename_len >= sizeof(filename))
    return 1;
  memcpy(filename, slash + 1, filename_len + 1);
  *slash = '\0';

  char *parent_slash = strrchr(parent, '/');
  if (!parent_slash)
    parent_slash = strrchr(parent, '\\');
  if (!parent_slash || !parent_slash[1])
    return 1;
  char directory[256];
  size_t directory_len = strlen(parent_slash + 1);
  if (directory_len >= sizeof(directory))
    return 1;
  memcpy(directory, parent_slash + 1, directory_len + 1);

  int n = snprintf(alias, sizeof(alias), "%s/../%s/%s", parent, directory,
                   filename);
  if (n < 0 || (size_t)n >= sizeof(alias) ||
      ny_std_find_module_id_by_path(alias) != expected) {
    fprintf(stderr,
            "[module_identity] dot-segment path lost module identity\n");
    return 1;
  }

#ifndef _WIN32
  char symlink_path[4096];
  n = snprintf(symlink_path, sizeof(symlink_path),
               "/tmp/nytrix-module-identity-%ld.ny", (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(symlink_path) ||
      symlink(registered, symlink_path) != 0) {
    fprintf(stderr, "[module_identity] could not create path alias symlink\n");
    return 1;
  }
  ny_module_id_t symlink_id = ny_std_find_module_id_by_path(symlink_path);
  unlink(symlink_path);
  if (symlink_id != expected) {
    fprintf(stderr, "[module_identity] symlink path lost module identity\n");
    return 1;
  }
#endif

  char overlong[8192];
  memset(overlong, 'x', sizeof(overlong) - 1);
  overlong[sizeof(overlong) - 1] = '\0';
  if (ny_std_find_module_id_by_path(overlong) != NY_MODULE_ID_UNKNOWN) {
    fprintf(stderr, "[module_identity] overlong path was partially matched\n");
    return 1;
  }

  puts("[module_identity] ok exact, relative, separator, alias, user, "
       "overlong, and synthetic-source identity");
  return 0;
}
