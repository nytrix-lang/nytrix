/*
 * `i != limit` counted loops: exact trip counts, and the cases where no count
 * may be claimed.
 *
 * SCEV derives a count only when the recurrence lands exactly on the limit:
 * the distance must be a whole multiple of the step and the step must travel
 * toward the limit. A loop that misses the limit keeps running until the i64
 * recurrence wraps, so treating the comparison alone as a count would let
 * bounds-check elimination drop live guards. The unknown cases below are as
 * important as the known ones, and they assert that NO range fact is exported.
 */
#include "code/backend/passes/verify/loop.h"
#include "opt.h"

static int failures;

/*
 *   L1: init; br L2
 *   L2: iv = phi(L1: init, L3: next); if (iv != limit) br L3 else br L4
 *   L3: next = iv + step; br L2
 *   L4: ret iv
 */
static size_t build_loop(nyr_func_t *f, int64_t start, int64_t limit,
                         int64_t step) {
  nyr_opt_begin(f);
  int init = nyr_opt_const(f, start);
  int lim = nyr_opt_const(f, limit);
  nyr_opt_label(f, 1);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 2);
  int iv = f->next_value;
  nyr_opt_phi(f, 1, init, 3, init); /* incoming value patched below */
  size_t phi = f->len - 1;
  int cond = nyr_opt_cmp(f, iv, lim, NYIR_CMP_NE);
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

static bool check_known(int64_t start, int64_t limit, int64_t step,
                        uint64_t want_trip) {
  nyr_func_t f;
  size_t phi = build_loop(&f, start, limit, step);
  int iv = f.data[phi].dst;
  bool ok = nyr_opt_verify(&f, "ne-known") && nyr_scev_analyze(&f, &(nyr_scev_info_t){0});
  nyr_scev_info_t info = {0};
  ok = nyr_opt_verify(&f, "ne-known") && nyr_scev_analyze(&f, &info);
  const nyr_scev_loop_t *loop = ok ? find_loop(&info, iv) : NULL;
  ok = ok && loop && loop->trip_count_known && loop->trip_count == want_trip;
  /*
   * The exit block returns the value that satisfied `iv == limit`.
   */
  int64_t locals[8] = {0};
  int64_t value = 0;
  ok = ok && nyr_opt_eval_locals(&f, "ne-known", locals, 8, &value) &&
       value == limit;
  /*
   * And the exported range is the closed interval [init, limit].
   */
  ok = ok && nyr_scev_lite(&f) && nyr_opt_verify(&f, "ne-known-lite");
  int64_t lo = start < limit ? start : limit;
  int64_t hi = start < limit ? limit : start;
  ok = ok && f.data[phi].range.has_min && f.data[phi].range.has_max &&
       f.data[phi].range.min == lo && f.data[phi].range.max == hi;
  if (!ok) {
    fprintf(stderr,
            "[scev-ne] start=%lld limit=%lld step=%lld: expected trip=%llu, "
            "got known=%d trip=%llu range=[%d:%lld..%lld]\n",
            (long long)start, (long long)limit, (long long)step,
            (unsigned long long)want_trip, loop ? (int)loop->trip_count_known : -1,
            loop ? (unsigned long long)loop->trip_count : 0ull,
            (int)f.data[phi].range.has_min, (long long)f.data[phi].range.min,
            (long long)f.data[phi].range.max);
    nyr_dump(stderr, &f, "ne-known-failure");
    ++failures;
  }
  nyr_scev_free(&info);
  nyr_ir_func_free(&f);
  return ok;
}

/*
 * No exact count: the loop misses the limit and wraps instead.
 */
static bool check_unknown(int64_t start, int64_t limit, int64_t step,
                          const char *why) {
  nyr_func_t f;
  size_t phi = build_loop(&f, start, limit, step);
  int iv = f.data[phi].dst;
  nyr_scev_info_t info = {0};
  bool ok = nyr_opt_verify(&f, "ne-unknown") && nyr_scev_analyze(&f, &info);
  const nyr_scev_loop_t *loop = ok ? find_loop(&info, iv) : NULL;
  ok = ok && loop != NULL && !loop->trip_count_known;
  /*
   * scev_lite must not invent a range fact for a loop with no exact count.
   */
  ok = ok && nyr_scev_lite(&f) && !f.data[phi].range.has_min;
  if (!ok) {
    fprintf(stderr,
            "[scev-ne] start=%lld limit=%lld step=%lld should be unknown "
            "(%s)\n",
            (long long)start, (long long)limit, (long long)step, why);
    ++failures;
  }
  nyr_scev_free(&info);
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  check_known(0, 5, 1, 5);   /* unit step, exits on equality */
  check_known(0, 6, 2, 3);   /* step divides the distance */
  check_known(6, 0, -2, 3);  /* downward loop with a divisible step */
  check_unknown(0, 5, 2, "step does not divide the distance");
  check_unknown(5, 0, -2, "downward step misses the limit");
  check_unknown(0, -3, 1, "step travels away from the limit");
  check_unknown(0, 0, 1, "entry already satisfies the exit condition");
  if (failures) {
    fprintf(stderr, "[scev-ne] %d case(s) failed\n", failures);
    return 1;
  }
  printf("[scev-ne] 3 exact `!=` trip counts and 4 correctly-unknown cases\n");
  return 0;
}
