/*
 * Post-loop symbolic exit value.
 *
 * After `for (i = 0; i < limit; ++i)`, the induction variable observed in the
 * exit block is exactly `limit` when the loop ran at least once. A check of
 * `(i - 1) * 8` against `i * 8` after the loop is therefore `8*limit - 8 <
 * 8*limit`: true for every entered iteration, with no constant range on the
 * limit. The proof needs three facts the analysis now records: the symbolic
 * exit value, a lower bound of 1 for it (from the entry proof), and a single
 * exit dominated by the check.
 *
 * The negative cases are the soundness half: no entry proof means the exit
 * value may be the start (offset -8), and `i * 8` is not below `i * 8`.
 */
#include "code/backend/passes/analysis/loop.h"
#include "opt.h"

static int failures;

static int call(nyr_func_t *f, const char *symbol, int a) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                     .dst = -1,
                                     .a = a,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .symbol = symbol,
                                     .imm = 1});
}

/*
 *   entry: base = rt_list_new(8); limit = load 1 & 7
 *          [guard 0 < limit -> P, else ret 0]
 *   P:     br H
 *   H:     iv = phi(P: 0, B: next); if (iv < limit) br B else br X
 *   B:     next = iv + 1; br H
 *   X:     index = last_index ? iv - 1 : iv
 *          bounds_check(base, index * 8, iv * 8); ret iv
 */
static void build_case(nyr_func_t *f, bool entry_guard, bool last_index) {
  nyr_opt_begin(f);
  int capacity = nyr_opt_const(f, 8);
  int base = call(f, "rt_list_new", capacity);
  int raw = nyr_opt_emit_imm(f, NYIR_LOAD_LOCAL, 1);
  int limit = nyr_opt_binop(f, NYIR_AND_I64, raw, nyr_opt_const(f, 7));
  int zero = nyr_opt_const(f, 0);
  if (entry_guard) {
    int guard = nyr_opt_cmp(f, zero, limit, NYIR_CMP_LT);
    nyr_opt_br_if(f, guard, 1);
    nyr_opt_br(f, 9);
    nyr_opt_label(f, 9);
    nyr_opt_ret(f, zero);
  } else {
    nyr_opt_br(f, 1);
  }
  nyr_opt_label(f, 1);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 2);
  int iv = f->next_value;
  nyr_opt_phi(f, 1, zero, 3, zero);
  size_t phi = f->len - 1;
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 3);
  nyr_opt_br(f, 4);
  nyr_opt_label(f, 3);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, nyr_opt_const(f, 1));
  f->data[phi].phi_incoming[1].value = next;
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 4);
  int index = iv;
  if (last_index)
    index = nyr_opt_binop(f, NYIR_SUB_I64, iv, nyr_opt_const(f, 1));
  int offset = nyr_opt_binop(f, NYIR_SHL_I64, index, nyr_opt_const(f, 3));
  int length = nyr_opt_binop(f, NYIR_SHL_I64, iv, nyr_opt_const(f, 3));
  nyr_opt_emit_abc(f, NYIR_BOUNDS_CHECK, base, offset, length);
  nyr_opt_ret(f, iv);
}

static bool check_case(const char *what, bool entry_guard, bool last_index,
                       bool want_removed) {
  nyr_func_t f;
  build_case(&f, entry_guard, last_index);
  bool ok = nyr_opt_verify(&f, what) && nyr_bounds_check_elim(&f) &&
            nyr_opt_verify(&f, "exit-after");
  size_t checks = 0;
  for (size_t i = 0; i < f.len; ++i)
    checks += f.data[i].op == NYIR_BOUNDS_CHECK;
  size_t want = want_removed ? 0u : 1u;
  if (!ok || checks != want) {
    fprintf(stderr, "[scev-exit] %s: checks=%zu want=%zu\n", what, checks, want);
    nyr_dump(stderr, &f, what);
    ++failures;
    ok = false;
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  check_case("entered-last-index", true, true, true);
  check_case("possibly-zero-trip", false, true, false);
  check_case("offset-equals-length", true, false, false);
  if (failures) {
    fprintf(stderr, "[scev-exit] %d case(s) failed\n", failures);
    return 1;
  }
  printf("[scev-exit] post-loop (i-1)*8 < i*8 proved; zero-trip and equal "
         "offsets kept\n");
  return 0;
}
