/*
 * SROA Escape Stage:
 * Delegates to the unified multi-tier SROA engine.
 */
#include "code/ir/opt/util.h"

bool nyir_sroa_unified(nyir_func_t *f);

bool nyir_escape_sroa(nyir_func_t *f) {
  return nyir_sroa_unified(f);
}
