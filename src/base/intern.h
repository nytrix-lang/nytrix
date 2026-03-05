#ifndef NY_INTERN_H
#define NY_INTERN_H

#include "base/common.h"
#include <stdint.h>

/**
 * A dense ID representing an interned string.
 * Valid IDs are > 0. 0 means invalid/none.
 */
typedef uint32_t ny_sym_id;

void ny_intern_init(void);
ny_sym_id ny_intern_str(const char *str, size_t len);
ny_sym_id ny_intern_cstr(const char *str);
const char *ny_intern_get(ny_sym_id id);
bool ny_intern_contains_ptr(const char *str);
void ny_intern_cleanup(void);

#endif
