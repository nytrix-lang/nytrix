/*
 * Regression driver for bounded NYIR general-inliner expansion.
 */
#include "util.h"

static int ny_load_local(nyir_func_t *f, int slot) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = slot});
}

static int ny_call(nyir_func_t *f, const char *symbol, int arg) {
  int dst = f->next_value;
  if (nyir_emit(f, (nyir_inst_t){.op = NYIR_CALL,
                                 .dst = dst,
                                 .a = arg,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .symbol = symbol,
                                 .imm = 1}) != dst)
    return -1;
  f->next_value = dst + 1;
  return dst;
}

static void ny_set_i64_param(nyir_func_t *f) {
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
}

static void ny_build_leaf(nyir_func_t *f) {
  ny_begin(f);
  ny_set_i64_param(f);
  int value = ny_load_local(f, 0);
  int one = ny_const(f, 1);
  ny_ret(f, ny_binop(f, NYIR_ADD_I64, value, one));
}

static void ny_build_wrapper(nyir_func_t *f, const char *callee) {
  ny_begin(f);
  ny_set_i64_param(f);
  ny_ret(f, ny_call(f, callee, ny_load_local(f, 0)));
}

static void ny_build_caller(nyir_func_t *f, const char *callee) {
  ny_begin(f);
  ny_ret(f, ny_call(f, callee, ny_const(f, 41)));
}

static size_t ny_count_calls(const nyir_func_t *f) {
  size_t calls = 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_CALL)
      ++calls;
  return calls;
}

static bool ny_verify_after_inline(nyir_func_t *f, const char *name) {
  if (!ny_verify(f, name))
    return false;
  if (!nyir_inline_general(f)) {
    fprintf(stderr, "[%s] general inliner failed\n", name);
    return false;
  }
  return ny_verify(f, name);
}

static bool ny_check_wrapper_chain(void) {
  nyir_func_t caller, outer, middle, leaf;
  ny_build_caller(&caller, "outer");
  ny_build_wrapper(&outer, "middle");
  ny_build_wrapper(&middle, "leaf");
  ny_build_leaf(&leaf);
  const nyir_inline_callee_t callees[] = {
      {"outer", &outer}, {"middle", &middle}, {"leaf", &leaf},
  };
  nyir_set_inline_callees(callees, sizeof(callees) / sizeof(callees[0]));
  bool ok = ny_verify_after_inline(&caller, "inline-wrapper-chain") &&
            ny_count_calls(&caller) == 0;
  if (!ok)
    fprintf(stderr, "[inline-wrapper-chain] expected every wrapper to inline\n");
  nyir_set_inline_callees(NULL, 0);
  nyir_func_free(&caller);
  nyir_func_free(&outer);
  nyir_func_free(&middle);
  nyir_func_free(&leaf);
  return ok;
}


static bool ny_check_deep_acyclic_chain(void) {
  enum { DEPTH = 12 };
  static const char *names[DEPTH] = {
      "deep00", "deep01", "deep02", "deep03", "deep04", "deep05",
      "deep06", "deep07", "deep08", "deep09", "deep10", "deep11",
  };
  nyir_func_t caller = {0};
  nyir_func_t funcs[DEPTH];
  memset(funcs, 0, sizeof(funcs));
  ny_build_caller(&caller, names[0]);
  for (size_t i = 0; i + 1 < DEPTH; ++i)
    ny_build_wrapper(&funcs[i], names[i + 1]);
  ny_build_leaf(&funcs[DEPTH - 1]);

  nyir_inline_callee_t callees[DEPTH];
  for (size_t i = 0; i < DEPTH; ++i)
    callees[i] = (nyir_inline_callee_t){names[i], &funcs[i]};
  nyir_set_inline_callees(callees, DEPTH);
  bool ok = ny_verify_after_inline(&caller, "inline-deep-acyclic") &&
            ny_count_calls(&caller) == 0;
  if (!ok)
    fprintf(stderr,
            "[inline-deep-acyclic] expected a >6-call acyclic chain to inline\n");
  nyir_set_inline_callees(NULL, 0);
  nyir_func_free(&caller);
  for (size_t i = 0; i < DEPTH; ++i)
    nyir_func_free(&funcs[i]);
  return ok;
}

static bool ny_check_mutual_recursion(void) {
  nyir_func_t caller, a, b;
  ny_build_caller(&caller, "a");
  ny_build_wrapper(&a, "b");
  ny_build_wrapper(&b, "a");
  const nyir_inline_callee_t callees[] = {
      {"a", &a},
      {"b", &b},
  };
  nyir_set_inline_callees(callees, sizeof(callees) / sizeof(callees[0]));
  bool ok = ny_verify_after_inline(&caller, "inline-mutual-recursion") &&
            ny_count_calls(&caller) == 1;
  if (!ok)
    fprintf(stderr,
            "[inline-mutual-recursion] expected one recursive call to remain\n");
  nyir_set_inline_callees(NULL, 0);
  nyir_func_free(&caller);
  nyir_func_free(&a);
  nyir_func_free(&b);
  return ok;
}

int main(void) {
  return ny_check_wrapper_chain() && ny_check_deep_acyclic_chain() &&
                 ny_check_mutual_recursion()
             ? 0
             : 1;
}
