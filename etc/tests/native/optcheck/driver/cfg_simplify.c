/*
 * Driver for nyir_cfg_simplify: CFG cleanup.
 *
 * Regression guard for the PHI-safety gate on trampoline threading:
 * the fixture's X block (label 30) is a branch-only trampoline sitting
 * between loop1's guard fallthrough and P2 — exactly the shape the
 * threading path wants to collapse.  Collapsing must never reroute an
 * edge into a block whose PHIs would gain a wrong predecessor label,
 * so both runs below (before and after fusion) must stay verifier-clean
 * and evaluate identically.
 */
#include "util.h"
#include "code/ir/opt/loop.h"

int main(void) {
  /*
   * Before fusion: trampolines exist, headers still have PHIs.
   */
  nyir_func_t f;
  ny_build_two_loops(&f, 3, 3);
  if (!ny_verify(&f, "cfg/verify-before"))
    return 1;
  long long before = ny_eval(&f, "cfg/before");
  nyir_cfg_simplify(&f);
  if (!ny_verify(&f, "cfg/verify-mid"))
    return 1;
  long long mid = ny_eval(&f, "cfg/mid");

  /*
   * After fusion: moved PHIs make the merged header extra sensitive to
   * predecessor-label rewrites.
   */
  if (!nyir_affine_nest(&f)) {
    fprintf(stderr, "[cfg] fusion setup failed\n");
    return 1;
  }
  nyir_cfg_simplify(&f);
  if (!ny_verify(&f, "cfg/verify-after"))
    return 1;
  long long after = ny_eval(&f, "cfg/after");
  if (ny_want_dump())
    nyir_dump(stdout, &f, "after-cfg");
  nyir_func_free(&f);
  ny_check("cfg", before, mid, 209);
  ny_check("cfg-fused", before, after, 209);
  return 0;
}
