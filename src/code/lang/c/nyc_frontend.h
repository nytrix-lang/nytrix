#ifndef NYC_FRONTEND_H
#define NYC_FRONTEND_H

#include <nyr/nyr.h>
#include <stddef.h>
#include "code/lang/c/c_abi.h"

int nyc_frontend_compile(const char *source, size_t source_len,
                         const char *symbol, nyc_abi_t abi,
                         nyr_func_t *out, char *err, size_t err_len);

#endif
