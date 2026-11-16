/*
 * Symbolic trip counts for loops whose start and limit are SSA values.
 *
 * `for (i = init; i < limit; ++i)` with masked parameters has no constant
 * trip count, but on an entered iteration the count is exactly
 * `limit - init`. The analysis records that as an expression over values and
 * only when a dominating guard proves the loop runs at least once, which is
 * what makes it usable outside the loop.
 *
 * The consumer under test is IRCE's invariant-check hoist: a bounds check on
 * loop-invariant operands may move to the preheader only if the loop cannot
 * be zero-trip. With a constant trip count that is already handled; this test
 * covers the symbolic expression, and asserts that the same check stays in
 * the body when no entry guard exists.
 */
#include "code/backend/passes/analysis/loop.h"
#include "code/backend/passes/util.h"
#include "opt.h"

static int failures;

/*
 * entry: a = load 1; b = load 2; init = a & 15; limit = b & 15
 *        [guard init < limit -> P, else exit]
 * P:     br H
 * H:     iv = phi(P: init, B: next); if (iv < limit) br B else br X
 * B:     next = iv + 1
 *        bounds_check(locals[0], 0, len = load 3, cap = 64)
 *        br H
 * X:     ret iv
 */
static size_t build_loop(nyr_func_t *f, bool entry_guard) {
  nyr_opt_begin(f);
  int a = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 1);
  int b = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 2);
  int mask = nyr_opt_const(f, 15);
  int init = nyr_opt_binop(f, NYIR_AND_I64, a, mask);
  int limit = nyr_opt_binop(f, NYIR_AND_I64, b, mask);
  /*
   * Check operands must be loop invariant for IRCE to hoist them; a base or
   * length defined in the body is a legitimate reason to leave the check
   * where it is.
   */
  int base = nyr_opt_emit_imm(f, NYIR_ADDR_LOCAL, 0);
  int offset = nyr_opt_const(f, 0);
  int length = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 3);
  if (entry_guard) {
    int guard = nyr_opt_cmp(f, init, limit, NYIR_CMP_LT);
    nyr_opt_br_if(f, guard, 1);
    nyr_opt_br(f, 9);
    nyr_opt_label(f, 9);
    nyr_opt_ret(f, nyr_opt_const(f, 0));
  } else {
    nyr_opt_br(f, 1);
  }
  nyr_opt_label(f, 1); /* preheader */
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 2); /* header */
  int iv = f->next_value;
  nyr_opt_phi(f, 1, init, 3, init); /* latch value patched below */
  size_t phi = f->len - 1;
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 3);
  nyr_opt_br(f, 4);
  nyr_opt_label(f, 3); /* body */
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, nyr_opt_const(f, 1));
  f->data[phi].phi_incoming[1].value = next;
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                              .dst = -1,
                              .a = base,
                              .b = offset,
                              .c = length,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 64});
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 4);
  nyr_opt_ret(f, iv);
  return phi;
}

static const nyr_scev_loop_t *find_loop(const nyr_scev_info_t *info, int iv) {
  for (size_t i = 0; i < info->count; ++i)
    if (info->loops[i].iv == iv)
      return &info->loops[i];
  return NULL;
}

static bool fail(const char *what, const char *why) {
  fprintf(stderr, "[scev-trip-expr] %s: %s\n", what, why);
  ++failures;
  return false;
}

/* Where is the surviving bounds check: inside the natural loop or not? */
static bool check_placement(nyr_func_t *f, size_t header_block,
                            size_t latch_block, bool *inside, bool *found) {
  nyr_cfg_t cfg = {0};
  *inside = false;
  *found = false;
  if (!nyr_cfg_build(f, &cfg))
    return false;
  bool *members = calloc(cfg.block_count, sizeof(*members));
  if (!members) {
    nyr_cfg_free(&cfg);
    return false;
  }
  bool have_loop = nyr_cfg_natural_loop_blocks(&cfg, latch_block, header_block,
                                               members, cfg.block_count);
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_BOUNDS_CHECK)
      continue;
    *found = true;
    size_t block = cfg.inst_block[i];
    *inside = have_loop && block < cfg.block_count && members[block];
    break;
  }
  free(members);
  nyr_cfg_free(&cfg);
  return true;
}

int main(void) {
  /* Entered loop: expression is `limit - init`, and IRCE hoists the check. */
  nyr_func_t f;
  size_t phi = build_loop(&f, true);
  int iv = f.data[phi].dst;
  nyr_scev_info_t info = {0};
  bool ok = nyr_opt_verify(&f, "trip-expr") && nyr_scev_analyze(&f, &info);
  const nyr_scev_loop_t *loop = ok ? find_loop(&info, iv) : NULL;
  if (!ok || !loop) {
    fail("entered", "loop not analyzed");
    return 1;
  }
  /* init is symbolic here, so no constant count may exist next to it. */
  if (!loop->trip_expr_known || loop->trip_count_known)
    fail("entered", "symbolic trip expression missing");
  if (loop->trip_expr_pos != loop->limit_value ||
      loop->trip_expr_neg != loop->init_value || loop->trip_expr_const != 0)
    fail("entered", "trip expression is not limit - init");
  /* Every observed value is 2..6, so the count is 5 when the guard holds. */
  int64_t locals[8] = {0, 2, 7, 64};
  int64_t before = 0;
  ok = nyr_opt_eval_locals(&f, "trip-expr-before", locals, 8, &before);
  ok = ok && before == 7 && nyr_irce(&f) && nyr_opt_verify(&f, "trip-expr-irce");
  int64_t after = 0;
  ok = ok && nyr_opt_eval_locals(&f, "trip-expr-after", locals, 8, &after) &&
       after == before;
  bool inside = false, found = false;
  ok = ok && check_placement(&f, loop->header_block, loop->latch_block, &inside,
                             &found);
  if (!ok || !found || inside) {
    fprintf(stderr, "[scev-trip-expr] debug ok=%d found=%d inside=%d\n",
            (int)ok, (int)found, (int)inside);
    nyr_dump(stderr, &f, "trip-expr-irce");
    fail("entered", "invariant check was not hoisted out of the loop");
  }
  nyr_scev_free(&info);
  nyr_ir_func_free(&f);

  /* Zero-trip guard: no expression and no hoist may appear. */
  phi = build_loop(&f, false);
  iv = f.data[phi].dst;
  nyr_scev_info_t no_info = {0};
  ok = nyr_opt_verify(&f, "no-guard") && nyr_scev_analyze(&f, &no_info);
  const nyr_scev_loop_t *no_loop = ok ? find_loop(&no_info, iv) : NULL;
  if (!ok || !no_loop) {
    fail("no-guard", "loop not analyzed");
    return 1;
  }
  if (no_loop->trip_expr_known || no_loop->trip_count_known)
    fail("no-guard", "trip count claimed without a provable entry");
  int64_t empty[8] = {0, 5, 4, 64};
  int64_t result = 0;
  ok = nyr_opt_eval_locals(&f, "no-guard-before", empty, 8, &result);
  ok = ok && result == 5 && nyr_irce(&f) &&
       nyr_opt_verify(&f, "no-guard-irce");
  ok = ok && nyr_opt_eval_locals(&f, "no-guard-after", empty, 8, &result) &&
       result == 5;
  inside = false;
  found = false;
  ok = ok && check_placement(&f, no_loop->header_block, no_loop->latch_block,
                             &inside, &found);
  if (!ok || !found || !inside)
    fail("no-guard", "check moved out of a possibly zero-trip loop");
  nyr_scev_free(&no_info);
  nyr_ir_func_free(&f);

  if (failures) {
    fprintf(stderr, "[scev-trip-expr] %d case(s) failed\n", failures);
    return 1;
  }
  printf("[scev-trip-expr] symbolic count limit-init exposed; hoist gated on "
         "entry proof\n");
  return 0;
}
