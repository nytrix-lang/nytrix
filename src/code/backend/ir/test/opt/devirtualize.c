#include "code/lang/parse/ast.h"
#include "support.h"

typedef struct {
  const char *expected_symbol;
  size_t expected_arg_count;
  int64_t expected_args[NYIR_CALL_MAX_ARGS];
  int64_t expected_result;
  bool saw_call;
} devirt_call_context_t;

static bool resolve_test_call(void *userdata, const char *symbol,
                              const int64_t *args, size_t arg_count,
                              int64_t *result, char *err, size_t err_len) {
  devirt_call_context_t *ctx = userdata;
  if (!ctx || !args || !result || arg_count != ctx->expected_arg_count ||
      args[0] == 0 || !symbol || strcmp(symbol, ctx->expected_symbol) != 0) {
    if (err && err_len)
      snprintf(err, err_len, "unexpected devirtualized call arguments");
    return false;
  }
  for (size_t i = 1; i < arg_count; ++i)
    if (ctx->expected_args[i] != 0 && args[i] != ctx->expected_args[i]) {
      if (err && err_len)
        snprintf(err, err_len, "devirtualized argument %zu changed", i);
      return false;
    }
  ctx->saw_call = true;
  *result = ctx->expected_result;
  return true;
}

static void build_indirect_call(nyr_func_t *f) {
  nyr_opt_begin(f);
  int target = nyr_ir_emit(f, (nyr_inst_t){
                                .op = NYIR_ADDR_SYMBOL,
                                .dst = -1,
                                .a = -1,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .symbol = "__ny_lambda_devirt_target",
                            });
  int argument = nyr_opt_const(f, 37);
  int result = nyr_ir_emit(f, (nyr_inst_t){
                                .op = NYIR_CALL,
                                .dst = -1,
                                .a = argument,
                                .b = target,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .imm = 2,
                                .effects = NYIR_EFFECT_CALL,
                            });
  nyr_call_arg_class_set(f->data[f->len - 1].arg_scalar_classes, 0,
                          NYIR_SCALAR_I64_RAW);
  nyr_call_arg_class_set(f->data[f->len - 1].arg_scalar_classes, 1,
                          NYIR_SCALAR_CLOSURE);
  nyr_call_arg_ownership_set(&f->data[f->len - 1], 0, NY_SEM_OWN_OWNED);
  nyr_call_arg_ownership_set(&f->data[f->len - 1], 1, NY_SEM_OWN_BORROWED);
  nyr_call_arg_contract_set(&f->data[f->len - 1], 0,
                             NYIR_ARG_CONTRACT_CONSUMES);
  nyr_call_arg_contract_set(&f->data[f->len - 1], 1,
                             NYIR_ARG_CONTRACT_BORROWS);
  nyr_opt_ret(f, result);
}

static void build_extra_arg_indirect_call(nyr_func_t *f) {
  nyr_opt_begin(f);
  int args[7];
  for (size_t i = 0; i < 6; ++i)
    args[i] = nyr_opt_const(f, (int64_t)(11 + i * 11));
  args[6] = nyr_ir_emit(f, (nyr_inst_t){
                             .op = NYIR_ADDR_SYMBOL,
                             .dst = -1,
                             .a = -1,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .symbol = "__ny_lambda_devirt_target",
                         });
  int *extra_args = malloc(sizeof(*extra_args));
  if (!extra_args)
    exit(2);
  extra_args[0] = args[6];
  int result = nyr_ir_emit(f, (nyr_inst_t){
                                .op = NYIR_CALL,
                                .dst = -1,
                                .a = args[0],
                                .b = args[1],
                                .c = args[2],
                                .d = args[3],
                                .e = args[4],
                                .f = args[5],
                                .imm = 7,
                                .extra_args = extra_args,
                                .extra_args_len = 1,
                                .effects = NYIR_EFFECT_CALL,
                            });
  nyr_opt_ret(f, result);
}

static bool evaluate(nyr_func_t *f, const char *expected_symbol,
                     size_t expected_arg_count, const int64_t *expected_args,
                     int64_t expected_result, int64_t *out) {
  nyr_eval_result_t result = {0};
  devirt_call_context_t ctx = {
      .expected_symbol = expected_symbol,
      .expected_arg_count = expected_arg_count,
      .expected_result = expected_result,
  };
  if (expected_args)
    memcpy(ctx.expected_args, expected_args,
           expected_arg_count * sizeof(*expected_args));
  char err[256] = {0};
  bool ok = nyr_ir_eval_with_calls(f, NULL, 0, 500, &result, resolve_test_call,
                                 &ctx, err, sizeof(err));
  if (!ok || !ctx.saw_call) {
    fprintf(stderr, "[devirtualize] execution failed: %s\n",
            err[0] ? err : "call resolver was not reached");
    nyr_eval_result_free(&result);
    return false;
  }
  *out = result.result;
  nyr_eval_result_free(&result);
  return true;
}

static bool test_pipeline_level(int opt_level) {
  nyr_func_t f = {0};
  build_indirect_call(&f);
  nyr_opt_stats_t stats = {0};
  bool ok = nyr_optimize_with_stats(&f, &stats, opt_level);
  bool ran_devirt = false;
  for (size_t i = 0; i < stats.pass_count; ++i)
    ran_devirt |= stats.passes[i].name &&
                  strcmp(stats.passes[i].name, "devirtualize") == 0;

  int64_t after = 0;
  const int64_t expected_args[] = {0, 37};
  if (ok)
    ok = ran_devirt && nyr_opt_verify(&f, "devirtualize/after") &&
         evaluate(&f, "__ny_lambda_devirt_target", 2, expected_args, 79,
                  &after) &&
         after == 79;
  if (ok) {
    const nyr_inst_t *call = NULL;
    for (size_t i = 0; i < f.len; ++i)
      if (f.data[i].op == NYIR_CALL)
        call = &f.data[i];
    ok = call && nyr_call_arg_class_get(call, 0) == NYIR_SCALAR_CLOSURE &&
         nyr_call_arg_class_get(call, 1) == NYIR_SCALAR_I64_RAW &&
         nyr_call_arg_ownership_get(call, 0) == NY_SEM_OWN_BORROWED &&
         nyr_call_arg_ownership_get(call, 1) == NY_SEM_OWN_OWNED &&
         nyr_call_arg_contract_get(call, 0) == NYIR_ARG_CONTRACT_BORROWS &&
         nyr_call_arg_contract_get(call, 1) == NYIR_ARG_CONTRACT_CONSUMES;
  }
  if (!ok) {
    fprintf(stderr, "[devirtualize] O%d pipeline failed\n", opt_level);
    if (nyr_opt_want_dump())
      nyr_dump(stderr, &f, "devirtualize/failure");
  } else {
    printf("[devirtualize] O%d indirect call resolved; result=%lld\n",
           opt_level, (long long)after);
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool test_extra_arg_target(int opt_level) {
  nyr_func_t f = {0};
  build_extra_arg_indirect_call(&f);
  nyr_opt_stats_t stats = {0};
  bool ok = nyr_optimize_with_stats(&f, &stats, opt_level);
  bool ran_devirt = false;
  for (size_t i = 0; i < stats.pass_count; ++i)
    ran_devirt |= stats.passes[i].name &&
                  strcmp(stats.passes[i].name, "devirtualize") == 0;

  const int64_t expected_args[] = {0, 22, 33, 44, 55, 66, 11};
  int64_t result = 0;
  if (ok)
    ok = ran_devirt && nyr_opt_verify(&f, "devirtualize/extra-arg-after") &&
         evaluate(&f, "__ny_lambda_devirt_target", 7, expected_args, 33,
                  &result) &&
         result == 33;
  if (!ok) {
    fprintf(stderr, "[devirtualize] O%d extra-argument target failed\n",
            opt_level);
    if (nyr_opt_want_dump())
      nyr_dump(stderr, &f, "devirtualize/extra-arg-failure");
  } else {
    printf("[devirtualize] O%d extra-argument target resolved; result=%lld\n",
           opt_level, (long long)result);
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  return test_pipeline_level(1) && test_pipeline_level(2) &&
                 test_pipeline_level(3) && test_extra_arg_target(1) &&
                 test_extra_arg_target(2) && test_extra_arg_target(3)
             ? 0
             : 1;
}
