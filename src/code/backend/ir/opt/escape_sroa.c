/*
 * SROA Escape Stage:
 * Delegates to the unified multi-tier SROA engine.
 */
#include "code/backend/ir/opt/util.h"
bool nyr_sroa_unified(nyr_func_t *f);

bool nyr_escape_sroa(nyr_func_t *f) { return nyr_sroa_unified(f); }
