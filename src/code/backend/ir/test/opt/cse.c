/*
 * Focused proof boundary for effect-summary-gated call CSE.
 */
#include "code/backend/ir/opt/util.h"
#include "code/lang/parse/ast.h"
#include "support.h"

static int emit_known_call(nyr_func_t *f, const char *symbol, int arg,
                           unsigned effects) {
  int dst = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                       .dst = -1,
                                       .a = arg,
                                       .b = -1,
                                       .c = -1,
                                       .d = -1,
                                       .e = -1,
                                       .f = -1,
                                       .imm = 1,
                                       .symbol = symbol,
                                       .scalar_class = NYIR_SCALAR_OBJECT,
                                       .semantic_ownership = NY_SEM_OWN_OWNED});
  if (dst >= 0) {
    f->data[f->len - 1].flags |= NYIR_INST_F_EFFECTS_KNOWN;
    f->data[f->len - 1].effects = effects;
  }
  return dst;
}

static bool cse_keeps_distinct_argument_ownership_contracts(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int arg = nyr_opt_const(&f, 9);
  unsigned pure = NYIR_EFFECT_CALL | NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP;
  emit_known_call(&f, "ownership_sensitive", arg, pure);
  nyr_call_arg_ownership_set(&f.data[f.len - 1], 0, NY_SEM_OWN_BORROWED);
  emit_known_call(&f, "ownership_sensitive", arg, pure);
  nyr_call_arg_ownership_set(&f.data[f.len - 1], 0, NY_SEM_OWN_OWNED);
  nyr_opt_ret(&f, f.data[2].dst);
  bool ok = nyr_opt_verify(&f, "cse-ownership/before") && nyr_cse(&f) &&
            nyr_opt_verify(&f, "cse-ownership/after") &&
            f.data[1].op == NYIR_CALL && f.data[2].op == NYIR_CALL &&
            nyr_call_arg_ownership_get(&f.data[1], 0) == NY_SEM_OWN_BORROWED &&
            nyr_call_arg_ownership_get(&f.data[2], 0) == NY_SEM_OWN_OWNED;
  if (!ok)
    fprintf(stderr,
            "[cse] distinct argument ownership contracts were merged\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool cse_keeps_distinct_formal_ownership_contracts(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int arg = nyr_opt_const(&f, 9);
  unsigned pure = NYIR_EFFECT_CALL | NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP;
  emit_known_call(&f, "formal_contract_sensitive", arg, pure);
  nyr_call_arg_contract_set(&f.data[f.len - 1], 0, NYIR_ARG_CONTRACT_BORROWS);
  emit_known_call(&f, "formal_contract_sensitive", arg, pure);
  nyr_call_arg_contract_set(&f.data[f.len - 1], 0, NYIR_ARG_CONTRACT_CONSUMES);
  nyr_opt_ret(&f, f.data[2].dst);
  bool ok =
      nyr_opt_verify(&f, "cse-formal-contract/before") && nyr_cse(&f) &&
      nyr_opt_verify(&f, "cse-formal-contract/after") &&
      f.data[1].op == NYIR_CALL && f.data[2].op == NYIR_CALL &&
      nyr_call_arg_contract_get(&f.data[1], 0) == NYIR_ARG_CONTRACT_BORROWS &&
      nyr_call_arg_contract_get(&f.data[2], 0) == NYIR_ARG_CONTRACT_CONSUMES;
  if (!ok)
    fprintf(stderr, "[cse] distinct formal ownership contracts were merged\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool cse_pure_call_and_preserves_result_facts(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int arg = nyr_opt_const(&f, 9);
  unsigned pure = NYIR_EFFECT_CALL | NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP;
  int first = emit_known_call(&f, "pure_scalar", arg, pure);
  int second = emit_known_call(&f, "pure_scalar", arg, pure);
  nyr_opt_ret(&f, second);
  if (first < 0 || second < 0 || !nyr_opt_verify(&f, "cse-pure-call/before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  bool ok = nyr_cse(&f) && nyr_opt_verify(&f, "cse-pure-call/after") &&
            f.data[1].op == NYIR_CALL && f.data[2].op == NYIR_COPY &&
            f.data[2].a == first &&
            f.data[2].scalar_class == NYIR_SCALAR_OBJECT &&
            f.data[2].semantic_ownership == NY_SEM_OWN_OWNED;
  if (!ok)
    fprintf(stderr, "[cse] pure call was not safely deduplicated/preserved\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool cse_reuses_local_inputs_across_pure_recursive_calls(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int first_load =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .imm = 0});
  int one = nyr_opt_const(&f, 1);
  int first_arg = nyr_ir_emit(
      &f,
      (nyr_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = first_load, .b = one});
  unsigned pure = NYIR_EFFECT_CALL | NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP;
  int first_call = emit_known_call(&f, "recursive_pure", first_arg, pure);
  int second_load =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .imm = 0});
  int second_arg = nyr_ir_emit(
      &f,
      (nyr_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = second_load, .b = one});
  int second_call = emit_known_call(&f, "recursive_pure", second_arg, pure);
  int sum = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADD_I64,
                                        .dst = -1,
                                        .a = first_call,
                                        .b = second_call});
  nyr_opt_ret(&f, sum);
  if (first_load < 0 || one < 0 || first_arg < 0 || first_call < 0 ||
      second_load < 0 || second_arg < 0 || second_call < 0 || sum < 0 ||
      !nyr_opt_verify(&f, "cse-recursive-call/before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  bool ok = nyr_cse(&f) && nyr_opt_verify(&f, "cse-recursive-call/after") &&
            f.data[0].op == NYIR_LOAD_LOCAL && f.data[4].op == NYIR_COPY &&
            f.data[4].a == first_load && f.data[5].op == NYIR_COPY &&
            f.data[5].a == first_arg && f.data[6].op == NYIR_COPY &&
            f.data[6].a == first_call && f.data[7].a == first_call &&
            f.data[7].b == first_call;
  if (!ok)
    fprintf(stderr, "[cse] repeated recursive pure-call tree was not merged\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool cse_rejects_memory_observing_call(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int arg = nyr_opt_const(&f, 9);
  unsigned read_memory = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY;
  emit_known_call(&f, "reads_argument_memory", arg, read_memory);
  emit_known_call(&f, "reads_argument_memory", arg, read_memory);
  nyr_opt_ret(&f, 0);
  bool ok = nyr_opt_verify(&f, "cse-memory-call/before") && nyr_cse(&f) &&
            nyr_opt_verify(&f, "cse-memory-call/after") &&
            f.data[1].op == NYIR_CALL && f.data[2].op == NYIR_CALL;
  if (!ok)
    fprintf(stderr,
            "[cse] memory-observing call was incorrectly deduplicated\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool cse_clears_facts_at_writing_call(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int arg = nyr_opt_const(&f, 9);
  unsigned pure = NYIR_EFFECT_CALL;
  int first = emit_known_call(&f, "pure_scalar", arg, pure);
  emit_known_call(&f, "writes_memory", arg,
                  NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY);
  int after_write = emit_known_call(&f, "pure_scalar", arg, pure);
  nyr_opt_ret(&f, after_write);
  bool ok = first >= 0 && after_write >= 0 &&
            nyr_opt_verify(&f, "cse-write-call/before") && nyr_cse(&f) &&
            nyr_opt_verify(&f, "cse-write-call/after") &&
            f.data[1].op == NYIR_CALL && f.data[3].op == NYIR_CALL;
  if (!ok)
    fprintf(stderr, "[cse] write boundary failed to invalidate call facts\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool cse_preserves_runtime_memory_calls(void) {
  static const struct {
    const char *symbol;
    unsigned effects;
  } cases[] = {
      {"rt_memcpy_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
      {"rt_memset_raw",
       NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
      {"rt_memcmp_raw",
       NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    nyr_func_t f;
    nyr_opt_begin(&f);
    int destination = nyr_opt_const(&f, 0x1000);
    int source_or_value = nyr_opt_const(&f, 0x2000);
    int length = nyr_opt_const(&f, 8);
    int first = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                            .dst = -1,
                                            .a = destination,
                                            .b = source_or_value,
                                            .c = length,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .imm = 3,
                                            .symbol = cases[i].symbol});
    int second = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                             .dst = -1,
                                             .a = destination,
                                             .b = source_or_value,
                                             .c = length,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .imm = 3,
                                             .symbol = cases[i].symbol});
    nyr_opt_ret(&f, second);
    bool ok =
        destination >= 0 && source_or_value >= 0 && length >= 0 && first >= 0 &&
        second >= 0 &&
        nyr_call_effect_summary(&f.data[(size_t)first]) == cases[i].effects &&
        nyr_effective_effects(&f.data[(size_t)first]) == cases[i].effects &&
        nyr_opt_verify(&f, "cse-runtime-memory-write/before") && nyr_cse(&f) &&
        nyr_opt_verify(&f, "cse-runtime-memory-write/after") &&
        f.data[(size_t)first].op == NYIR_CALL &&
        f.data[(size_t)second].op == NYIR_CALL;
    if (!ok)
      fprintf(stderr,
              "[cse] runtime memory call %s was merged or mis-summarized\n",
              cases[i].symbol);
    nyr_ir_func_free(&f);
    if (!ok)
      return false;
  }
  return true;
}

int main(void) {
  if (!cse_pure_call_and_preserves_result_facts() ||
      !cse_reuses_local_inputs_across_pure_recursive_calls() ||
      !cse_keeps_distinct_argument_ownership_contracts() ||
      !cse_keeps_distinct_formal_ownership_contracts() ||
      !cse_rejects_memory_observing_call() ||
      !cse_clears_facts_at_writing_call() ||
      !cse_preserves_runtime_memory_calls())
    return 1;
  puts("[cse] effect-summary calls gated; result metadata preserved");
  return 0;
}
