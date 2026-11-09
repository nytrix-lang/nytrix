#ifndef NYC_PARSE_H
#define NYC_PARSE_H

#include "code/lang/parse/c/aggregate.h"
#include <nyr/nyr.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  size_t offset;
  char *symbol;
  long long addend;
  size_t width;
  unsigned type;
} nyc_data_reloc_t;

enum {
  NYC_DATA_RELOC_ABS64 = 0,
  NYC_DATA_RELOC_PC32 = 1,
  NYC_DATA_RELOC_ABS32 = 2,
  NYC_DATA_RELOC_ABS32S = 3,
};

typedef struct {
  char *name;
  unsigned char *bytes;
  size_t len;
  /* Owned `__attribute__((__section__("...")))` spelling; NULL keeps the
   * default section. */
  char *section;
  uint64_t section_flags;
  size_t alignment;
  size_t section_entsize;
  bool function_symbol;
  bool is_local;
  nyc_data_reloc_t *relocs;
  size_t reloc_count;
} nyc_data_t;

typedef struct {
  char *name;
  char *target;
  bool target_local;
} nyc_function_alias_t;

typedef struct {
  char *name;
  char *target;
} nyc_variable_alias_t;

typedef struct {
  nyr_func_t *funcs;
  char **names;
  char **function_sections;
  bool *function_local;
  bool *function_inline;
  bool *function_always_inline;
  bool *function_noinline;
  bool *function_no_emit;
  bool *function_discardable;
  size_t count;
  nyc_data_t *data;
  size_t data_count;
  nyc_function_alias_t *aliases;
  size_t alias_count;
  nyc_variable_alias_t *variable_aliases;
  size_t variable_alias_count;
  char *raw_file_scope_asm;
} nyc_unit_t;

bool nyc_unit_prune_static(nyc_unit_t *unit, char *err, size_t err_len);

typedef struct {
  /* NAME[=REPLACEMENT] for -D; NAME for -U. Applied in argument order. */
  const char *text;
  bool undefine;
} nyc_macro_option_t;

typedef struct {
  const char *from;
  const char *to;
} nyc_path_map_t;

/* Input text and search paths are borrowed for the synchronous compilation.
 * Quoted includes first search the containing file's directory, then -I paths.
 * The native driver can opt into host system headers after modeled headers;
 * embedded and cross-target callers keep explicit-only search by default.
 */
typedef struct {
  const char *text;
  size_t length;
  const char *path;
  const char *base_path;
  const char *const *include_dirs;
  size_t include_dir_count;
  const nyc_macro_option_t *macros;
  size_t macro_count;
  const nyc_path_map_t *path_maps;
  size_t path_map_count;
  bool short_wchar;
  bool unsigned_char;
  bool no_builtin;
  bool ms_extensions;
  bool plan9_extensions;
  bool system_includes;
  unsigned i386_regparm;
} nyc_source_t;

typedef int (*nyc_dependency_visitor_t)(const char *path, void *context);

int nyc_frontend_preprocess(const nyc_source_t *source, nyc_abi_t abi,
                            FILE *output, bool line_markers, char *err,
                            size_t err_len);
int nyc_frontend_dump_macros(const nyc_source_t *source, nyc_abi_t abi,
                             FILE *output, char *err, size_t err_len);
int nyc_frontend_visit_dependencies(const nyc_source_t *source, nyc_abi_t abi,
                                    nyc_dependency_visitor_t visitor,
                                    void *context, char *err, size_t err_len);
int nyc_frontend_compile(const nyc_source_t *source, const char *symbol,
                         nyc_abi_t abi, nyr_func_t *out, char *err,
                         size_t err_len);
int nyc_frontend_compile_unit(const nyc_source_t *source, nyc_abi_t abi,
                              nyc_unit_t *out, char *err, size_t err_len);
void nyc_unit_destroy(nyc_unit_t *unit);

#endif
