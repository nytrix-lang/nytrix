/*
 * Symbolic-start/limit counted loops: bounds derived from operand range facts.
 *
 * A loop such as `for (i = 0; i != n; ++i)` or `for (i = 0; i < (n & 7); ++i)`
 * has no literal limit, so no exact trip count exists.  When the limit is loop
 * invariant and its range (and the start's range) are known, every value the
 * induction variable can take is still bounded.  That interval is what lets
 * downstream range queries and bounds-check elimination prove guards without
 * an exact iteration count.
 *
 * The last two cases are the soundness half: an unbounded limit must export
 * nothing, and a limit whose range would let the recurrence wrap past INT64_MAX
 * must be rejected rather than turned into an interval.
 */
#include "code/backend/passes/analysis/loop.h"
#include "code/backend/passes/util.h"
#include "opt.h"

static int failures;

/*
 *   entry: n = load_local 1; limit = masked ? (n & mask) : n
 *   L1:    br L2
 *   L2:    iv = phi(L1: init, L3: next); if (iv pred limit) br L3 else L4
 *   L3:    next = iv + step; br L2
 *   L4:    ret iv
 */
static size_t build_loop(nyr_func_t *f, int64_t init_value, int64_t step,
                         nyr_cmp_t pred, bool masked, int64_t mask) {
  nyr_opt_begin(f);
  int init = nyr_opt_const(f, init_value);
  int runtime = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 1);
  int limit = masked ? nyr_opt_binop(f, NYIR_AND_I64, runtime,
                                     nyr_opt_const(f, mask))
                     : runtime;
  nyr_opt_label(f, 1);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 2);
  int iv = f->next_value;
  nyr_opt_phi(f, 1, init, 3, init); /* latch value patched below */
  size_t phi = f->len - 1;
  int cond = nyr_opt_cmp(f, iv, limit, pred);
  nyr_opt_br_if(f, cond, 3);
  nyr_opt_br(f, 4);
  nyr_opt_label(f, 3);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, nyr_opt_const(f, step));
  f->data[phi].phi_incoming[1].value = next;
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

static size_t def_index_of(const nyr_func_t *f, int value) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst == value)
      return i;
  return SIZE_MAX;
}

static bool fail(const char *what, const char *why) {
  fprintf(stderr, "[scev-range] %s: %s\n", what, why);
  ++failures;
  return false;
}

/* Upward or downward loop with a masked limit: bounds must be exported. */
static bool check_bounded(const char *what, int64_t init_value, int64_t step,
                          nyr_cmp_t pred, int64_t want_body_min,
                          int64_t want_body_max, int64_t want_iv_min,
                          int64_t want_iv_max, uint64_t want_trip_bound) {
  nyr_func_t f;
  size_t phi = build_loop(&f, init_value, step, pred, true, 7);
  int iv = f.data[phi].dst;
  nyr_scev_info_t info = {0};
  bool ok = nyr_opt_verify(&f, what) && nyr_scev_analyze(&f, &info);
  const nyr_scev_loop_t *loop = ok ? find_loop(&info, iv) : NULL;
  if (!ok || !loop) {
    fail(what, "loop not analyzed");
    nyr_scev_free(&info);
    nyr_ir_func_free(&f);
    return false;
  }
  ok = loop->body_bounds_known && loop->body_min == want_body_min &&
       loop->body_max == want_body_max && loop->iv_bounds_known &&
       loop->iv_min == want_iv_min && loop->iv_max == want_iv_max &&
       loop->trip_bound_known && loop->trip_bound == want_trip_bound &&
       !loop->trip_count_known;
  if (!ok) {
    fprintf(stderr,
            "[scev-range] %s: body=%d[%lld..%lld] iv=%d[%lld..%lld] "
            "trip_bound=%d(%llu) exact=%d\n",
            what, (int)loop->body_bounds_known, (long long)loop->body_min,
            (long long)loop->body_max, (int)loop->iv_bounds_known,
            (long long)loop->iv_min, (long long)loop->iv_max,
            (int)loop->trip_bound_known,
            (unsigned long long)loop->trip_bound, (int)loop->trip_count_known);
    ++failures;
  }
  /*
   * The in-body query is the consumer path: bounds-check elimination asks for
   * the IV's interval at an instruction inside the body.
   */
  int *defs = nyr_build_defs(&f);
  size_t next_at = SIZE_MAX;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].op == NYIR_ADD_I64 && f.data[i].a == iv)
      next_at = i;
  nyr_scev_affine_t aff = {0};
  if (!defs || next_at == SIZE_MAX ||
      !nyr_scev_affine_at(&f, &info, defs, iv, next_at, &aff)) {
    fail(what, "in-body affine bounds unavailable");
  } else if (!aff.range.has_min || !aff.range.has_max ||
             aff.range.min != want_body_min || aff.range.max != want_body_max) {
    fprintf(stderr, "[scev-range] %s: in-body range=[%lld..%lld]\n", what,
            (long long)aff.range.min, (long long)aff.range.max);
    ++failures;
  }
  free(defs);
  /* The phi's range fact must cover the exit-block observation too. */
  ok = nyr_scev_lite(&f) && nyr_opt_verify(&f, "bounded-lite") &&
       f.data[phi].range.has_min && f.data[phi].range.has_max &&
       f.data[phi].range.min == want_iv_min &&
       f.data[phi].range.max == want_iv_max;
  if (!ok) {
    fprintf(stderr, "[scev-range] %s: phi range=[%d:%lld..%lld]\n", what,
            (int)f.data[phi].range.has_min, (long long)f.data[phi].range.min,
            (long long)f.data[phi].range.max);
    ++failures;
  }
  nyr_scev_free(&info);
  nyr_ir_func_free(&f);
  return ok;
}

/* Unknown or wrap-prone limits must not produce any interval. */
static bool check_unbounded(const char *what, nyr_cmp_t pred, bool masked,
                            int64_t mask) {
  nyr_func_t f;
  size_t phi = build_loop(&f, 0, 1, pred, masked, mask);
  int iv = f.data[phi].dst;
  nyr_scev_info_t info = {0};
  bool ok = nyr_opt_verify(&f, what) && nyr_scev_analyze(&f, &info);
  const nyr_scev_loop_t *loop = ok ? find_loop(&info, iv) : NULL;
  if (!ok || !loop) {
    fail(what, "loop not analyzed");
    nyr_scev_free(&info);
    nyr_ir_func_free(&f);
    return false;
  }
  ok = !loop->body_bounds_known && !loop->iv_bounds_known &&
       !loop->trip_count_known;
  if (!ok) {
    fprintf(stderr,
            "[scev-range] %s: body=%d iv=%d exact=%d (must all be false)\n",
            what, (int)loop->body_bounds_known, (int)loop->iv_bounds_known,
            (int)loop->trip_count_known);
    ++failures;
  }
  /* And no range fact may be exported for the induction variable. */
  ok = nyr_scev_lite(&f) && !f.data[phi].range.has_min;
  if (!ok) {
    fprintf(stderr, "[scev-range] %s: phi exported range=[%lld..%lld]\n", what,
            (long long)f.data[phi].range.min,
            (long long)f.data[phi].range.max);
    ++failures;
  }
  nyr_scev_free(&info);
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  /* (n & 7) bounds an upward `<` loop to [0,6] in the body, [0,7] overall. */
  check_bounded("up-lt-mask", 0, 1, NYIR_CMP_LT, 0, 6, 0, 7, 7);
  /* `<=` includes the limit itself in the body. */
  check_bounded("up-le-mask", 0, 2, NYIR_CMP_LE, 0, 7, 0, 9, 4);
  /* Downward loop with a symbolic lower bound. */
  check_bounded("down-gt-mask", 7, -1, NYIR_CMP_GT, 1, 7, 0, 7, 7);
  /* No range fact for the limit: nothing may be claimed. */
  check_unbounded("no-limit-range", NYIR_CMP_LT, false, 0);
  /* Range reaching INT64_MAX with `<=`: body + step would wrap, so reject. */
  check_unbounded("wrap-reject", NYIR_CMP_LE, true, INT64_MAX);
  if (failures) {
    fprintf(stderr, "[scev-range] %d case(s) failed\n", failures);
    return 1;
  }
  printf("[scev-range] 3 range-derived loops bounded, 2 correctly rejected\n");
  return 0;
}
