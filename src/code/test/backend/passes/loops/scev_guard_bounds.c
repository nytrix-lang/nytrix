/*
 * Loop-guard bounds-check elimination.
 *
 * `for (i = start; i < n; ++i) check(buf, i * S, n * S)` needs no constant
 * bound on `n`: the exit test itself proves the check, because the body runs
 * only while `i < n` and both sides are scaled by the same factor. These cases
 * pin that proof, and pin the shapes where it must NOT fire so a dropped guard
 * cannot follow from a misread comparison.
 */
#include "code/backend/passes/analysis/loop.h"
#include "opt.h"

static int failures;

static int call(nyr_func_t *f, const char *symbol, int a, int b,
                unsigned flags) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                     .dst = -1,
                                     .a = a,
                                     .b = b,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .symbol = symbol,
                                     .imm = b >= 0 ? 2 : 1,
                                     .flags = flags});
}

typedef struct {
  bool scaled;      /* check byte offset and byte length both scaled by 8 */
  bool use_le;      /* exit test is `<=` instead of `<` */
  bool descending;  /* `>` loop with step -1 */
  int64_t init;     /* induction start */
  int64_t delta;    /* element offset added to the IV before scaling */
  bool other_length;
} guard_case_t;

/*
 *   L1: base = rt_list_new(8); len = rt_tbuf_len_raw(base)
 *   L2: iv = phi(L1: init, L3: next); if (iv <cmp> len) br L3 else br L4
 *   L3: BOUNDS_CHECK(base, (iv + delta) [* 8], len [* 8]); next = iv + step
 *   L4: ret iv
 */
static void build_case(nyr_func_t *f, const guard_case_t *c) {
  nyr_opt_begin(f);
  nyr_opt_label(f, 1);
  int capacity = nyr_opt_const(f, 8);
  int base = call(f, "rt_list_new", capacity, -1, 0);
  int length = call(f, "rt_tbuf_len_raw", base, -1, 0);
  int length_bytes = length;
  if (c->scaled)
    length_bytes = nyr_opt_binop(f, NYIR_SHL_I64, length, nyr_opt_const(f, 3));
  if (c->other_length) {
    int other = call(f, "rt_list_new", capacity, -1, 0);
    length_bytes = call(f, "rt_tbuf_len_raw", other, -1, 0);
    if (c->scaled)
      length_bytes =
          nyr_opt_binop(f, NYIR_SHL_I64, length_bytes, nyr_opt_const(f, 3));
  }
  int start = nyr_opt_const(f, c->init);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 2);
  int iv = f->next_value;
  nyr_opt_phi(f, 1, start, 3, start); /* latch value patched below */
  size_t phi = f->len - 1;
  int cond = nyr_opt_cmp(f, iv, length, c->use_le ? NYIR_CMP_LE : NYIR_CMP_LT);
  if (c->descending)
    cond = nyr_opt_cmp(f, iv, length, NYIR_CMP_GT);
  nyr_opt_br_if(f, cond, 3);
  nyr_opt_br(f, 4);
  nyr_opt_label(f, 3);
  int offset = iv;
  if (c->delta > 0)
    offset = nyr_opt_binop(f, NYIR_ADD_I64, iv, nyr_opt_const(f, c->delta));
  else if (c->delta < 0)
    offset = nyr_opt_binop(f, NYIR_SUB_I64, iv, nyr_opt_const(f, -c->delta));
  if (c->scaled)
    offset = nyr_opt_binop(f, NYIR_SHL_I64, offset, nyr_opt_const(f, 3));
  nyr_opt_emit_abc(f, NYIR_BOUNDS_CHECK, base, offset, length_bytes);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv,
                           nyr_opt_const(f, c->descending ? -1 : 1));
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 4);
  nyr_opt_ret(f, iv);
  f->data[phi].phi_incoming[1].value = next;
}

static bool run_case(const char *what, const guard_case_t *c, bool want_removed) {
  nyr_func_t f;
  build_case(&f, c);
  bool ok = nyr_opt_verify(&f, what) && nyr_bounds_check_elim(&f) &&
            nyr_opt_verify(&f, "guard-after");
  size_t checks = 0;
  for (size_t i = 0; i < f.len; ++i)
    checks += f.data[i].op == NYIR_BOUNDS_CHECK;
  size_t want = want_removed ? 0u : 1u;
  if (!ok || checks != want) {
    fprintf(stderr, "[scev-guard] %s: checks=%zu want=%zu\n", what, checks,
            want);
    ++failures;
    ok = false;
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  /*
   * Element-indexed check: the exit test proves it directly.
   */
  run_case("lt-unscaled", &(guard_case_t){.scaled = false}, true);
  /*
   * `i - 1` below the IV is still inside the guard, and stays non-negative.
   */
  run_case("lt-offset-minus-one",
           &(guard_case_t){.delta = -1, .init = 4}, true);
  /*
   * `<=` allows the body at `i == n`, so only a strictly smaller offset.
   */
  run_case("le-offset-minus-one",
           &(guard_case_t){.use_le = true, .delta = -1, .init = 4}, true);
  /*
   * Byte-scaled check against the byte-scaled length.
   */
  run_case("lt-scaled", &(guard_case_t){.scaled = true}, true);

  /*
   * Rejected shapes.
   */
  run_case("le-offset-zero", &(guard_case_t){.use_le = true}, false);
  run_case("offset-plus-one", &(guard_case_t){.delta = 1}, false);
  run_case("other-length", &(guard_case_t){.other_length = true}, false);
  run_case("negative-lowest", &(guard_case_t){.delta = -1, .init = -4}, false);
  /*
   * `i - 1` starting at 0 really can be -1, so the check must stay.
   */
  run_case("negative-lowest-at-zero",
           &(guard_case_t){.use_le = true, .delta = -1}, false);
  run_case("descending", &(guard_case_t){.descending = true}, false);
  if (failures) {
    fprintf(stderr, "[scev-guard] %d case(s) failed\n", failures);
    return 1;
  }
  printf("[scev-guard] 4 guard-proven checks eliminated, 6 correctly kept\n");
  return 0;
}
