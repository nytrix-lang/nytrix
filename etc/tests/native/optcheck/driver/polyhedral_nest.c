/*
 * Driver for nyir_affine_nest: adjacent counted-loop fusion.
 *
 * Uses the shared two-loop fixture (util.h).  Golden 209: both
 * accumulators start at 100, loop1 adds its index three times
 * (100+0+1+2 = 103), loop2 adds twice its index (100+0+2+4 = 106), and
 * the exit sums the two header phis.
 */
#include "util.h"
#include "code/ir/opt/loop.h"

int main(void) {
  /*
   * Positive: equal trip counts fuse and keep the program intact.
   */
  nyir_func_t f;
  ny_build_two_loops(&f, 3, 3);
  if (!ny_verify(&f, "fuse/verify-before"))
    return 1;
  long long before = ny_eval(&f, "fuse/before");
  if (ny_want_dump())
    nyir_dump(stdout, &f, "before-fuse");
  if (!nyir_affine_nest(&f)) {
    fprintf(stderr, "[fuse] pass reported failure\n");
    return 1;
  }
  if (!ny_verify(&f, "fuse/verify-after"))
    return 1;
  long long after = ny_eval(&f, "fuse/after");
  if (ny_want_dump())
    nyir_dump(stdout, &f, "after-fuse");
  nyir_func_free(&f);
  ny_check("fuse", before, after, 209);

  /*
   * Negative: mismatched trip counts must not fuse (and must not
   * corrupt anything on the way out).
   */
  nyir_func_t g;
  ny_build_two_loops(&g, 3, 5);
  if (!ny_verify(&g, "nofuse/verify-before"))
    return 1;
  before = ny_eval(&g, "nofuse/before");
  if (!nyir_affine_nest(&g)) {
    fprintf(stderr, "[nofuse] pass reported failure\n");
    return 1;
  }
  if (!ny_verify(&g, "nofuse/verify-after"))
    return 1;
  after = ny_eval(&g, "nofuse/after");
  nyir_func_free(&g);
  /*
   * m=5: s1 stays 103, s2 becomes 100+0+2+4+6+8 = 120
   */
  ny_check("nofuse", before, after, 223);
  return 0;
}
