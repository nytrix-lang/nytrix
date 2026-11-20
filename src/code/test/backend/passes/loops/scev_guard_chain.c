/*
 * Predicated loop guards: `i < n` plus a dominating `n <= cap` proves a check
 * against `cap`, because the body only runs while `i < n <= cap`.
 *
 * The relations come from branches that dominate the check, and are closed
 * transitively. Every rejected case is a direction or dominance mistake that
 * would delete a live guard if the proof trusted it.
 */
#include "code/backend/passes/verify/loop.h"
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

typedef struct {
  /*
   * 0: n <= cap, 1: cap <= n (wrong direction), 2: n < cap, 3: no guard,
   * 4: n <= m then m <= cap (transitive), 5: guard in a sibling arm.
   */
  int mode;
} chain_case_t;

/*
 *   L1: cap = len(base); n = len(base2); [n <= cap] br L2 else L5
 *   L2: iv = phi(L1: 0, L3: next); if (iv < n) br L3 else L4
 *   L3: BOUNDS_CHECK(base, iv, cap); next = iv + 1; br L2
 *   L4: ret iv
 *   L5: ret 0
 */
static void build_case(nyr_func_t *f, const chain_case_t *c) {
  nyr_opt_begin(f);
  nyr_opt_label(f, 1);
  int capacity = nyr_opt_const(f, 8);
  int base = call(f, "rt_list_new", capacity);
  int other = call(f, "rt_list_new", capacity);
  int cap = call(f, "rt_tbuf_len_raw", base);
  int n = call(f, "rt_tbuf_len_raw", other);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);

  int preheader = 6;
  if (c->mode == 5) {
    /*
     * Guard lives in an arm that does not dominate the loop.
     */
    int probe = nyr_opt_cmp(f, n, cap, NYIR_CMP_LT);
    nyr_opt_br_if(f, probe, 6);
    nyr_opt_br(f, 7);
    nyr_opt_label(f, 7);
    nyr_opt_br(f, 2);
    nyr_opt_label(f, 6);
    nyr_opt_br(f, 8);
    preheader = 7;
  } else if (c->mode == 4) {
    int m = call(f, "rt_tbuf_len_raw", other);
    int first = nyr_opt_cmp(f, n, m, NYIR_CMP_LE);
    nyr_opt_br_if(f, first, 6);
    nyr_opt_br(f, 5);
    nyr_opt_label(f, 6);
    int second = nyr_opt_cmp(f, m, cap, NYIR_CMP_LE);
    nyr_opt_br_if(f, second, 2);
    nyr_opt_br(f, 5);
  } else if (c->mode == 3) {
    nyr_opt_br(f, 6);
    nyr_opt_label(f, 6);
    nyr_opt_br(f, 2);
  } else {
    nyr_cmp_t pred = c->mode == 2 ? NYIR_CMP_LT : NYIR_CMP_LE;
    int guard = c->mode == 1 ? nyr_opt_cmp(f, cap, n, pred)
                             : nyr_opt_cmp(f, n, cap, pred);
    nyr_opt_br_if(f, guard, 6);
    nyr_opt_br(f, 5);
    nyr_opt_label(f, 6);
    nyr_opt_br(f, 2);
  }

  nyr_opt_label(f, 2);
  int iv = f->next_value;
  nyr_opt_phi(f, preheader, zero, 3, zero);
  size_t phi = f->len - 1;
  int cond = nyr_opt_cmp(f, iv, n, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 3);
  nyr_opt_br(f, 4);
  nyr_opt_label(f, 3);
  nyr_opt_emit_abc(f, NYIR_BOUNDS_CHECK, base, iv, cap);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 2);
  nyr_opt_label(f, 4);
  nyr_opt_ret(f, iv);
  nyr_opt_label(f, 5);
  nyr_opt_ret(f, zero);
  if (c->mode == 5) {
    nyr_opt_label(f, 8);
    nyr_opt_ret(f, zero);
  }
  f->data[phi].phi_incoming[1].value = next;
}

static bool run_case(const char *what, int mode, bool want_removed) {
  nyr_func_t f;
  build_case(&f, &(chain_case_t){.mode = mode});
  bool ok = nyr_opt_verify(&f, what) && nyr_bounds_check_elim(&f) &&
            nyr_opt_verify(&f, "chain-after");
  size_t checks = 0;
  for (size_t i = 0; i < f.len; ++i)
    checks += f.data[i].op == NYIR_BOUNDS_CHECK;
  size_t want = want_removed ? 0u : 1u;
  if (!ok || checks != want) {
    fprintf(stderr, "[scev-chain] %s: checks=%zu want=%zu\n", what, checks,
            want);
    ++failures;
    ok = false;
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  run_case("le-chain", 0, true);
  run_case("lt-chain", 2, true);
  run_case("transitive-chain", 4, true);
  run_case("wrong-direction", 1, false);
  run_case("no-guard", 3, false);
  run_case("non-dominating-guard", 5, false);
  if (failures) {
    fprintf(stderr, "[scev-chain] %d case(s) failed\n", failures);
    return 1;
  }
  printf("[scev-chain] 3 chained-guard checks eliminated, 3 correctly kept\n");
  return 0;
}
