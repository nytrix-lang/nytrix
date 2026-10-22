/*
 * Nested joins promote locals and preserve evaluation across repeated runs.
 */
#include "code/backend/ir/opt/util.h"
#include "support.h"

static void store(nyr_func_t *f, int slot, int value) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = value,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = slot});
}
static int load(nyr_func_t *f, int slot) {
  int value = f->next_value++;
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                             .dst = value,
                             .a = -1,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = slot});
  return value;
}
static int less(nyr_func_t *f, int a, int b) {
  int v = f->next_value++;
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CMP_I64,
                             .dst = v,
                             .a = a,
                             .b = b,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .cmp = NYIR_CMP_LT});
  return v;
}
static bool check_loop(int trips) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0), one = nyr_opt_const(&f, 1);
  int three = nyr_opt_const(&f, 3), ten = nyr_opt_const(&f, 10);
  int bound = nyr_opt_const(&f, trips);
  store(&f, 0, zero);
  store(&f, 1, zero);
  nyr_opt_label(&f, 10);
  int i = load(&f, 0);
  nyr_opt_br_if(&f, less(&f, i, bound), 20);
  nyr_opt_br(&f, 50);
  nyr_opt_label(&f, 20);
  nyr_opt_br_if(&f, less(&f, i, three), 30);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 30);
  store(&f, 1, nyr_opt_binop(&f, NYIR_ADD_I64, load(&f, 1), one));
  nyr_opt_br(&f, 45);
  nyr_opt_label(&f, 40);
  store(&f, 1, nyr_opt_binop(&f, NYIR_ADD_I64, load(&f, 1), ten));
  nyr_opt_br(&f, 45);
  nyr_opt_label(&f, 45);
  store(&f, 0, nyr_opt_binop(&f, NYIR_ADD_I64, i, one));
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 50);
  nyr_opt_ret(&f, load(&f, 1));
  long long expected = trips < 3 ? trips : 3 + (trips - 3) * 10;
  bool ok = nyr_opt_verify(&f, "mem2reg-loop/before") &&
            nyr_opt_eval(&f, "mem2reg-loop/before") == expected &&
            nyr_mem2reg(&f) && nyr_opt_verify(&f, "mem2reg-loop/after") &&
            nyr_opt_eval(&f, "mem2reg-loop/after") == expected;
  for (size_t k = 0; k < f.len; ++k)
    if (f.data[k].op == NYIR_STORE_LOCAL || f.data[k].op == NYIR_LOAD_LOCAL)
      ok = false;
  if (!ok) {
    fprintf(stderr, "nested loop trips=%d expected=%lld\n", trips, expected);
    nyr_dump(stderr, &f, "nested-loop-failure");
  }
  nyr_ir_func_free(&f);
  return ok;
}

/*
 * A fallback zero must dominate every edge that can consume it. The first
 * source zero here lives only on L10; L20 reads the still-uninitialized local
 * before storing it, then both paths join at L30.
 */
static bool check_non_dominating_zero_fallback(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int yes = nyr_opt_const(&f, 1);
  nyr_opt_br_if(&f, yes, 10);
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 10);
  int branch_zero = nyr_opt_const(&f, 0);
  store(&f, 0, branch_zero);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 20);
  int undef = load(&f, 0);
  store(&f, 0, undef);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 30);
  nyr_opt_ret(&f, load(&f, 0));

  bool ok = nyr_opt_verify(&f, "mem2reg-zero-dominance/before") &&
            nyr_opt_eval(&f, "mem2reg-zero-dominance/before") == 0 &&
            nyr_mem2reg(&f) &&
            nyr_opt_verify(&f, "mem2reg-zero-dominance/after") &&
            nyr_opt_eval(&f, "mem2reg-zero-dominance/after") == 0;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].op == NYIR_LOAD_LOCAL || f.data[i].op == NYIR_STORE_LOCAL)
      ok = false;
  if (!ok) {
    fprintf(stderr, "non-dominating mem2reg zero fallback\n");
    nyr_dump(stderr, &f, "mem2reg-zero-dominance-failure");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool check_address_taken_nested_join(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0), one = nyr_opt_const(&f, 1),
      seven = nyr_opt_const(&f, 7);
  store(&f, 0, zero);
  int addr = f.next_value++;
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                              .dst = addr,
                              .a = -1,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0});
  nyr_opt_br_if(&f, one, 10);
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 10);
  nyr_opt_br_if(&f, one, 30);
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 30);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = addr,
                              .b = -1,
                              .c = seven,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 20);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 40);
  nyr_opt_ret(&f, load(&f, 0));

  bool ok = nyr_opt_verify(&f, "mem2reg-address/before") &&
            nyr_opt_eval(&f, "mem2reg-address/before") == 7 &&
            nyr_mem2reg(&f) && nyr_opt_verify(&f, "mem2reg-address/after") &&
            nyr_opt_eval(&f, "mem2reg-address/after") == 7;
  bool kept_local_memory = false;
  for (size_t i = 0; i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        in->imm == 0)
      kept_local_memory = true;
  }
  ok = ok && kept_local_memory;
  if (!ok) {
    fprintf(stderr, "address-taken nested join was not preserved\n");
    nyr_dump(stderr, &f, "mem2reg-address-failure");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool check_irreducible_join(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0), one = nyr_opt_const(&f, 1);
  int nine = nyr_opt_const(&f, 9), eleven = nyr_opt_const(&f, 11);
  store(&f, 0, zero);
  nyr_opt_br_if(&f, one, 10);
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 10);
  nyr_opt_br_if(&f, one, 30);
  nyr_opt_br(&f, 20);
  nyr_opt_label(&f, 20);
  store(&f, 0, nine);
  nyr_opt_br_if(&f, zero, 30);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 30);
  store(&f, 0, eleven);
  nyr_opt_br_if(&f, zero, 20);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 40);
  nyr_opt_ret(&f, load(&f, 0));

  bool ok = nyr_opt_verify(&f, "mem2reg-irreducible/before") &&
            nyr_opt_eval(&f, "mem2reg-irreducible/before") == 11 &&
            nyr_mem2reg(&f) &&
            nyr_opt_verify(&f, "mem2reg-irreducible/after") &&
            nyr_opt_eval(&f, "mem2reg-irreducible/after") == 11;
  if (!ok) {
    fprintf(stderr, "irreducible nested join changed behavior\n");
    nyr_dump(stderr, &f, "mem2reg-irreducible-failure");
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  nyr_set_nested_mem2reg_enabled(true);
  if (!check_loop(0) || !check_loop(1) || !check_loop(3) || !check_loop(6) ||
      !check_non_dominating_zero_fallback() ||
      !check_address_taken_nested_join() || !check_irreducible_join())
    return 1;
  nyr_func_t f;
  nyr_opt_begin(&f);
  int yes = nyr_opt_const(&f, 1);
  int answer = nyr_opt_const(&f, 42);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = answer,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0});
  nyr_opt_br_if(&f, yes, 10);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 10);
  nyr_opt_br_if(&f, yes, 20);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 20);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 30);
  int loaded = f.next_value++;
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                              .dst = loaded,
                              .a = -1,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0});
  nyr_opt_ret(&f, loaded);
  if (!nyr_opt_verify(&f, "mem2reg-gate/before"))
    return 1;
  nyr_set_nested_mem2reg_enabled(false);
  uint64_t rejected = nyr_debug_fingerprint(&f);
  for (int k = 0; k < 3; ++k)
    if (!nyr_mem2reg(&f) || nyr_debug_fingerprint(&f) != rejected ||
        !nyr_opt_verify(&f, "mem2reg-gate/rejected") ||
        nyr_opt_eval(&f, "mem2reg-gate/rejected") != 42)
      return 1;
  nyr_set_nested_mem2reg_enabled(true);
  if (!nyr_mem2reg(&f) || !nyr_opt_verify(&f, "mem2reg-nested/after") ||
      nyr_opt_eval(&f, "mem2reg-nested") != 42)
    return 1;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].op == NYIR_LOAD_LOCAL || f.data[i].op == NYIR_STORE_LOCAL)
      return 1;
  uint64_t promoted = nyr_debug_fingerprint(&f);
  for (int i = 0; i < 3; ++i)
    if (!nyr_mem2reg(&f) || !nyr_opt_verify(&f, "mem2reg-nested/repeated") ||
        nyr_debug_fingerprint(&f) != promoted ||
        nyr_opt_eval(&f, "mem2reg-nested") != 42)
      return 1;
  nyr_ir_func_free(&f);
  return 0;
}
