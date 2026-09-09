#include "util.h"

#include <stdint.h>

static int64_t gvn_pre_cell = 41;

static int emit_load(nyir_func_t *f, int address) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_I64, .dst = -1,
                                    .a = address, .b = -1, .c = -1,
                                    .d = -1, .e = -1, .f = -1});
}

static void emit_store(nyir_func_t *f, int address, int value) {
  nyir_emit(f, (nyir_inst_t){.op = NYIR_STORE_I64, .dst = -1,
                             .a = address, .b = -1, .c = value,
                             .d = -1, .e = -1, .f = -1});
}

static void emit_unknown_call(nyir_func_t *f) {
  nyir_emit(f, (nyir_inst_t){.op = NYIR_CALL, .dst = -1,
                             .a = -1, .b = -1, .c = -1, .d = -1,
                             .e = -1, .f = -1, .imm = 0,
                             .symbol = "unknown_memory_effect"});
}

static void build_diamond(nyir_func_t *f, bool store, bool unknown_call) {
  ny_begin(f);
  int address = ny_const(f, (int64_t)(intptr_t)&gvn_pre_cell);
  int false_value = ny_const(f, 0);
  int replacement = ny_const(f, 99);
  ny_br_if(f, false_value, 10);

  ny_label(f, 20);
  emit_load(f, address);
  ny_br(f, 30);

  ny_label(f, 10);
  emit_load(f, address);
  if (store)
    emit_store(f, address, replacement);
  if (unknown_call)
    emit_unknown_call(f);
  ny_br(f, 30);

  ny_label(f, 30);
  int result = emit_load(f, address);
  ny_ret(f, result);
}

static void count_memory_ops(const nyir_func_t *f, int *loads, int *phis) {
  *loads = 0;
  *phis = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_LOAD_I64)
      ++*loads;
    if (f->data[i].op == NYIR_PHI)
      ++*phis;
  }
}

static bool run_case(const char *name, bool store, bool unknown_call,
                     int want_loads, int want_phis) {
  nyir_func_t f;
  build_diamond(&f, store, unknown_call);
  if (!ny_verify(&f, name)) {
    nyir_func_free(&f);
    return false;
  }
  long long before = ny_eval(&f, name);
  if (!nyir_gvn_pre(&f) || !ny_verify(&f, name)) {
    nyir_func_free(&f);
    return false;
  }
  long long after = ny_eval(&f, name);
  int loads, phis;
  count_memory_ops(&f, &loads, &phis);
  if (loads != want_loads || phis != want_phis) {
    fprintf(stderr, "[%s] loads=%d phis=%d want loads=%d phis=%d\n", name,
            loads, phis, want_loads, want_phis);
    nyir_func_free(&f);
    return false;
  }
  nyir_func_free(&f);
  ny_check(name, before, after, 41);
  return true;
}

int main(void) {
  return run_case("gvn-pre-load-diamond", false, false, 2, 1) &&
                 run_case("gvn-pre-load-store", true, false, 3, 0) &&
                 run_case("gvn-pre-load-unknown-call", false, true, 3, 0)
             ? 0
             : 1;
}
