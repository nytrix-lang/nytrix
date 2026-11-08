/*
 * Tbuf inline pass: replaces core.tbuf function calls with direct memory operations.
 */
#ifndef NYR_TBUF_INLINE_H
#define NYR_TBUF_INLINE_H

#include "code/backend/ir/internal.h"

bool nyr_tbuf_inline(nyr_func_t *f);

#endif
