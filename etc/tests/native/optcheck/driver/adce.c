/*
 * Driver for nyir_adce: aggressive dead code elimination.
 *
 * Regression guard for the PHI-blindness fix: liveness must propagate
 * through phi_incoming entries, including PHIs in unreachable blocks.
 * The input is fused two-loop IR (util.h fixture), which deliberately
 * contains dead definitions — the abandoned second comparison and its
 * body leftovers — next to moved PHIs whose backedge operands are the
 * only keepers of some values.  ADCE must reap exactly the dead defs
 * and leave evaluation untouched.
 */
#include "util.h"
#include "code/ir/opt/loop.h"

int main(void) {
  nyir_func_t f;
  ny_build_two_loops(&f, 3, 3);
  if (!nyir_affine_nest(&f)) {
    fprintf(stderr, "[adce] fusion setup failed\n");
    return 1;
  }
  if (!ny_verify(&f, "adce/verify-before"))
    return 1;
  long long before = ny_eval(&f, "adce/before");
  nyir_adce(&f);
  if (!ny_verify(&f, "adce/verify-after"))
    return 1;
  long long after = ny_eval(&f, "adce/after");
  if (ny_want_dump())
    nyir_dump(stdout, &f, "after-adce");
  nyir_func_free(&f);
  ny_check("adce", before, after, 209);
  return 0;
}
