#ifndef NY_LANG_RUNTIME_ALIASES_H
#define NY_LANG_RUNTIME_ALIASES_H

#include <stddef.h>

typedef struct {
  const char *name;
  const char *symbol;
} nyr_runtime_alias_t;

extern const nyr_runtime_alias_t nyr_runtime_aliases[];
extern const size_t nyr_runtime_alias_count;
const char *nyr_runtime_symbol(const char *name);

#endif
