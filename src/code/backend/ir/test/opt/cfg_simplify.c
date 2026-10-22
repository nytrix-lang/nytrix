/*
 * Driver for nyr_cfg_simplify: CFG cleanup.
 *
 * Regression guard for the PHI-safety gate on trampoline threading:
 * the fixture's X block (label 30) is a branch-only trampoline sitting
 * between loop1's guard fallthrough and P2 — exactly the shape the
 * threading path wants to collapse.  Collapsing must never reroute an
 * edge into a block whose PHIs would gain a wrong predecessor label,
 * so both runs below (before and after fusion) must stay verifier-clean
 * and evaluate identically.
 */
#include "code/backend/ir/opt/loop.h"
#include "support.h"

static bool false_branch_preserves_debug(void) {
  static const char source[] = "cfg-simplify-origin.ny";
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0);
  size_t branch_at = f.len;
  nyr_opt_br_if(&f, zero, 10);
  f.data[branch_at].debug =
      (nyr_debug_loc_t){.file = source, .line = 5, .column = 9};
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 10);
  int taken = nyr_opt_const(&f, 17);
  nyr_opt_ret(&f, taken);
  nyr_opt_label(&f, 20);
  int fallthrough = nyr_opt_const(&f, 29);
  nyr_opt_ret(&f, fallthrough);

  bool ok = nyr_opt_verify(&f, "cfg/debug-before") &&
            nyr_opt_eval(&f, "cfg/debug-before") == 29 &&
            nyr_cfg_simplify(&f) && nyr_opt_verify(&f, "cfg/debug-after") &&
            nyr_opt_eval(&f, "cfg/debug-after") == 29 &&
            f.data[branch_at].op == NYIR_NOP &&
            f.data[branch_at].debug.file == source &&
            f.data[branch_at].debug.line == 5 &&
            f.data[branch_at].debug.column == 9;
  if (!ok)
    fprintf(stderr,
            "[cfg] false-branch rewrite lost debug location or result\n");
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  if (!false_branch_preserves_debug())
    return 1;
  /*
   * Before fusion: trampolines exist, headers still have PHIs.
   */
  nyr_func_t f;
  nyr_opt_build_two_loops(&f, 3, 3);
  if (!nyr_opt_verify(&f, "cfg/verify-before"))
    return 1;
  long long before = nyr_opt_eval(&f, "cfg/before");
  nyr_cfg_simplify(&f);
  if (!nyr_opt_verify(&f, "cfg/verify-mid"))
    return 1;
  long long mid = nyr_opt_eval(&f, "cfg/mid");

  /*
   * After fusion: moved PHIs make the merged header extra sensitive to
   * predecessor-label rewrites.
   */
  if (!nyr_affine_nest(&f)) {
    fprintf(stderr, "[cfg] fusion setup failed\n");
    return 1;
  }
  nyr_cfg_simplify(&f);
  if (!nyr_opt_verify(&f, "cfg/verify-after"))
    return 1;
  long long after = nyr_opt_eval(&f, "cfg/after");
  if (nyr_opt_want_dump())
    nyr_dump(stdout, &f, "after-cfg");
  nyr_ir_func_free(&f);
  nyr_opt_check("cfg", before, mid, 209);
  nyr_opt_check("cfg-fused", before, after, 209);
  return 0;
}
