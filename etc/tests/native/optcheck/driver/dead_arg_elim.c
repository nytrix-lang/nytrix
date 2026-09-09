/*
 * Regression driver for pure-call dead-result cleanup.
 */
#include "util.h"

extern bool nyir_dead_arg_elim(nyir_func_t *f);

static int ny_call(nyir_func_t *f, const char *symbol, unsigned flags,
                   unsigned effects, int arg) {
  int dst = f->next_value;
  nyir_inst_t in = {.op = NYIR_CALL,
                    .dst = dst,
                    .a = arg,
                    .b = -1,
                    .c = -1,
                    .d = -1,
                    .e = -1,
                    .f = -1,
                    .imm = 1,
                    .symbol = symbol,
                    .flags = flags,
                    .effects = effects};
  if (nyir_emit(f, in) != dst)
    return -1;
  f->data[f->len - 1].flags |= flags;
  f->data[f->len - 1].effects = effects;
  f->next_value = dst + 1;
  return dst;
}

static bool resolve(void *ctx, const char *symbol, const int64_t *args,
                    size_t argc, int64_t *result, char *err, size_t err_len) {
  (void)ctx;
  (void)args;
  (void)err;
  (void)err_len;
  if (!symbol || argc != 1 || !result)
    return false;
  *result = 99;
  return true;
}

static long long eval_calls(nyir_func_t *f) {
  nyir_eval_result_t result = {0};
  int64_t locals[8] = {0};
  char err[256] = {0};
  if (!nyir_eval_with_calls(f, locals, 8, 500, &result, resolve, NULL, err,
                            sizeof(err))) {
    fprintf(stderr, "[dead-arg-elim] eval failed: %s\n", err);
    exit(1);
  }
  long long value = (long long)result.result;
  nyir_eval_result_free(&result);
  return value;
}

static size_t count_calls(const nyir_func_t *f) {
  size_t count = 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_CALL)
      ++count;
  return count;
}

static bool run_dead_known(void) {
  nyir_func_t f;
  ny_begin(&f);
  int arg = ny_const(&f, 7);
  (void)ny_call(&f, "local_pure", NYIR_INST_F_EFFECTS_KNOWN,
                NYIR_EFFECT_CALL, arg);
  ny_ret(&f, arg);
  long long before = eval_calls(&f);
  bool verified_before = ny_verify(&f, "dead-arg-before");
  bool transformed = verified_before && nyir_dead_arg_elim(&f);
  bool verified_after = transformed && ny_verify(&f, "dead-arg-after");
  long long after = verified_after ? eval_calls(&f) : -1;
  size_t calls = count_calls(&f);
  bool ok = verified_after && after == before && calls == 0;
  if (!ok)
    fprintf(stderr, "[dead-arg-elim] known before=%lld after=%lld calls=%zu verify=%d/%d\n",
            before, after, calls, verified_before, verified_after);
  nyir_func_free(&f);
  return ok;
}

static bool run_rejected_foreign(void) {
  nyir_func_t f;
  ny_begin(&f);
  int arg = ny_const(&f, 7);
  (void)ny_call(&f, "foreign_unknown", NYIR_INST_F_EXTERN, 0, arg);
  ny_ret(&f, arg);
  bool ok = nyir_dead_arg_elim(&f) && count_calls(&f) == 1;
  nyir_func_free(&f);
  return ok;
}

int main(void) { return run_dead_known() && run_rejected_foreign() ? 0 : 1; }
