/*
 * Known multiples of a counted loop's induction values, and their consumer.
 *
 * An unwrapped recurrence `{init,+,step}` only ever visits values congruent
 * to `init` modulo `step`, so every value is a multiple of gcd(|init|,
 * |step|). SCEV records that divisor for loops whose exact trip count or
 * range bounds prove the recurrence cannot wrap, and `nyr_scev_lite` folds a
 * body `iv % d` to zero when the divisor divides the known multiple.
 *
 * The negative cases are the interesting half: wrapping is not proven for a
 * missing-loop-bound shape (the exact count covers these), an indivisible
 * divisor must keep the modulo, and a start that is not a multiple leaves the
 * multiple at one.
 */
#include "code/backend/passes/verify/loop.h"
#include "opt.h"

static int failures;

/*
 *   L1: br L2
 *   L2: iv = phi(L1: init, L3: next); acc = phi(L1: 0, L3: acc')
 *       if (iv < limit) br L3 else br L4
 *   L3: rem = iv % divisor; acc' = acc + rem; next = iv + step; br L2
 *   L4: ret acc
 */
static size_t build_loop(nyr_func_t *f, int64_t init, int64_t step,
                         int64_t limit, int64_t divisor) {
  nyr_opt_begin(f);
  int start = nyr_opt_const(f, init);
  int lim = nyr_opt_const(f, limit);
  int zero = nyr_opt_const(f, 0);
  nyr_opt_br(f, 1);
  nyr_opt_label(f, 1);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 2);
  int iv = f->next_value;
  nyr_opt_phi(f, 1, start, 3, start);
  size_t iv_phi = f->len - 1;
  int acc = f->next_value;
  nyr_opt_phi(f, 1, zero, 3, zero);
  size_t acc_phi = f->len - 1;
  int cond = nyr_opt_cmp(f, iv, lim, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 3);
  nyr_opt_br(f, 4);
  nyr_opt_label(f, 3);
  int rem = nyr_opt_binop(f, NYIR_MOD_I64, iv, nyr_opt_const(f, divisor));
  int sum = nyr_opt_binop(f, NYIR_ADD_I64, acc, rem);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, nyr_opt_const(f, step));
  f->data[iv_phi].phi_incoming[1].value = next;
  f->data[acc_phi].phi_incoming[1].value = sum;
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 4);
  nyr_opt_ret(f, acc);
  return iv_phi;
}

static const nyr_scev_loop_t *find_loop(const nyr_scev_info_t *info, int iv) {
  for (size_t i = 0; i < info->count; ++i)
    if (info->loops[i].iv == iv)
      return &info->loops[i];
  return NULL;
}

static size_t count_op(const nyr_func_t *f, nyr_op_t op) {
  size_t n = 0;
  for (size_t i = 0; i < f->len; ++i)
    n += f->data[i].op == op;
  return n;
}

static bool check_case(const char *what, int64_t init, int64_t step,
                       int64_t limit, int64_t divisor, bool want_multiple,
                       int64_t want_multiple_value, bool want_fold,
                       long long want_result) {
  nyr_func_t f;
  size_t phi = build_loop(&f, init, step, limit, divisor);
  int iv = f.data[phi].dst;
  nyr_scev_info_t info = {0};
  bool ok = nyr_opt_verify(&f, what) && nyr_scev_analyze(&f, &info);
  const nyr_scev_loop_t *loop = ok ? find_loop(&info, iv) : NULL;
  if (!ok || !loop) {
    fprintf(stderr, "[scev-multiple] %s: loop not analyzed\n", what);
    ++failures;
    nyr_scev_free(&info);
    nyr_ir_func_free(&f);
    return false;
  }
  bool multiple_ok = loop->iv_multiple_known == want_multiple;
  if (want_multiple)
    multiple_ok = multiple_ok && loop->iv_multiple == want_multiple_value;
  size_t mods_before = count_op(&f, NYIR_MOD_I64);
  long long before = nyr_opt_eval(&f, what);
  ok = multiple_ok && mods_before == 1 && nyr_scev_lite(&f) &&
       nyr_opt_verify(&f, "multiple-lite");
  size_t mods_after = count_op(&f, NYIR_MOD_I64);
  long long after = nyr_opt_eval(&f, "multiple-after");
  ok = ok && mods_after == (want_fold ? 0u : 1u) &&
       before == want_result && after == want_result;
  if (!ok) {
    fprintf(stderr,
            "[scev-multiple] %s: multiple=%d(%lld) mods=%zu->%zu result "
            "%lld/%lld want %lld\n",
            what, (int)loop->iv_multiple_known,
            (long long)loop->iv_multiple, mods_before, mods_after, before,
            after, want_result);
    nyr_dump(stderr, &f, what);
    ++failures;
  }
  nyr_scev_free(&info);
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  /*
   * {0,+,3}: every value is a multiple of 3, so `iv % 3` is zero.
   */
  check_case("zero-start-three", 0, 3, 30, 3, true, 3, true, 0);
  /*
   * {1,+,3}: gcd(1,3) == 1, so the modulo stays.
   */
  check_case("odd-start", 1, 3, 30, 3, false, 0, false, 10);
  /*
   * {4,+,6}: gcd(4,6) == 2, so only an even divisor folds.
   */
  check_case("gcd-two-div-two", 4, 6, 40, 2, true, 2, true, 0);
  /*
   * 4,10,16,22,28,34 modulo 3 is 1 each: six iterations.
   */
  check_case("gcd-two-div-three", 4, 6, 40, 3, true, 2, false, 6);
  /*
   * Multiple 3 does not imply divisibility by 2.
   */
  check_case("three-mod-two", 0, 3, 30, 2, true, 3, false, 5);
  if (failures) {
    fprintf(stderr, "[scev-multiple] %d case(s) failed\n", failures);
    return 1;
  }
  printf("[scev-multiple] 3 folds from known multiples, 2 correctly kept\n");
  return 0;
}
