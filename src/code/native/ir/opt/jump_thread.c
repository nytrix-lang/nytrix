/*
 * Jump threading is intentionally disabled until redirecting an edge also
 * rewrites successor PHI predecessor labels.
 */
#include "code/native/ir/opt/util.h"

bool nyir_jump_thread(nyir_func_t *f) {
  (void)f;
  return true;
}
