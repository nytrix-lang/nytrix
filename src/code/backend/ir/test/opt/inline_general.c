/*
 * Regression driver for bounded NYIR general-inliner expansion.
 */
#include "support.h"

static int ny_load_local(nyr_func_t *f, int slot) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = slot});
}

static int ny_call(nyr_func_t *f, const char *symbol, int arg) {
  int dst = f->next_value;
  if (nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
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

static void ny_set_i64_param(nyr_func_t *f) {
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
}

static void ny_build_leaf(nyr_func_t *f) {
  nyr_opt_begin(f);
  ny_set_i64_param(f);
  int value = ny_load_local(f, 0);
  int one = nyr_opt_const(f, 1);
  nyr_opt_ret(f, nyr_opt_binop(f, NYIR_ADD_I64, value, one));
}

static void ny_build_wrapper(nyr_func_t *f, const char *callee) {
  nyr_opt_begin(f);
  ny_set_i64_param(f);
  nyr_opt_ret(f, ny_call(f, callee, ny_load_local(f, 0)));
}

static void ny_build_caller(nyr_func_t *f, const char *callee) {
  nyr_opt_begin(f);
  nyr_opt_ret(f, ny_call(f, callee, nyr_opt_const(f, 41)));
}

static void ny_build_branching_leaf(nyr_func_t *f) {
  nyr_opt_begin(f);
  ny_set_i64_param(f);
  int arg = ny_load_local(f, 0);
  int two = nyr_opt_const(f, 2);
  int ten = nyr_opt_const(f, 10);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int is_small = nyr_opt_cmp(f, arg, two, NYIR_CMP_LT);
  nyr_opt_br_if(f, is_small, 11);
  nyr_opt_label(f, 12);
  int large = nyr_opt_binop(f, NYIR_MUL_I64, arg, two);
  nyr_opt_br(f, 13);
  nyr_opt_label(f, 11);
  int small = nyr_opt_binop(f, NYIR_ADD_I64, arg, ten);
  nyr_opt_br(f, 13);
  nyr_opt_label(f, 13);
  nyr_opt_phi(f, 12, large, 11, small);
  nyr_opt_ret(f, f->next_value - 1);
}

static void ny_build_loop_caller(nyr_func_t *f) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int limit = nyr_opt_const(f, 4);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 20);
  int i = f->next_value;
  nyr_opt_phi(f, -1, zero, 21, zero);
  int sum = f->next_value;
  nyr_opt_phi(f, -1, zero, 21, zero);
  int more = nyr_opt_cmp(f, i, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, more, 21);
  nyr_opt_label(f, 22);
  nyr_opt_ret(f, sum);
  nyr_opt_label(f, 21);
  int value = ny_call(f, "branching_leaf", i);
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, value);
  int next_i = nyr_opt_binop(f, NYIR_ADD_I64, i, one);
  nyr_opt_br(f, 20);
  size_t phi_count = 0;
  for (size_t at = 0; at < f->len; ++at) {
    if (f->data[at].op != NYIR_PHI)
      continue;
    f->data[at].phi_incoming[1] = (nyr_phi_incoming_t){
        .predecessor_label = 21, .value = phi_count++ == 0 ? next_i : next_sum};
  }
}

static size_t ny_count_calls(const nyr_func_t *f) {
  size_t calls = 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_CALL)
      ++calls;
  return calls;
}

static bool ny_verify_after_inline(nyr_func_t *f, const char *name) {
  if (!nyr_opt_verify(f, name))
    return false;
  if (!nyr_inline_general(f)) {
    fprintf(stderr, "[%s] general inliner failed\n", name);
    return false;
  }
  return nyr_opt_verify(f, name);
}

static bool ny_check_wrapper_chain(void) {
  nyr_func_t caller, outer, middle, leaf;
  ny_build_caller(&caller, "outer");
  ny_build_wrapper(&outer, "middle");
  ny_build_wrapper(&middle, "leaf");
  ny_build_leaf(&leaf);
  const nyr_inline_callee_t callees[] = {
      {"outer", &outer},
      {"middle", &middle},
      {"leaf", &leaf},
  };
  nyr_set_inline_callees(callees, sizeof(callees) / sizeof(callees[0]));
  bool ok = ny_verify_after_inline(&caller, "inline-wrapper-chain") &&
            ny_count_calls(&caller) == 0;
  if (!ok)
    fprintf(stderr,
            "[inline-wrapper-chain] expected every wrapper to inline\n");
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&outer);
  nyr_ir_func_free(&middle);
  nyr_ir_func_free(&leaf);
  return ok;
}

static bool ny_check_deep_acyclic_chain(void) {
  enum { DEPTH = 12 };
  static const char *names[DEPTH] = {
      "deep00", "deep01", "deep02", "deep03", "deep04", "deep05",
      "deep06", "deep07", "deep08", "deep09", "deep10", "deep11",
  };
  nyr_func_t caller = {0};
  nyr_func_t funcs[DEPTH];
  memset(funcs, 0, sizeof(funcs));
  ny_build_caller(&caller, names[0]);
  for (size_t i = 0; i + 1 < DEPTH; ++i)
    ny_build_wrapper(&funcs[i], names[i + 1]);
  ny_build_leaf(&funcs[DEPTH - 1]);

  nyr_inline_callee_t callees[DEPTH];
  for (size_t i = 0; i < DEPTH; ++i)
    callees[i] = (nyr_inline_callee_t){names[i], &funcs[i]};
  nyr_set_inline_callees(callees, DEPTH);
  bool ok = ny_verify_after_inline(&caller, "inline-deep-acyclic") &&
            ny_count_calls(&caller) == 0;
  if (!ok)
    fprintf(
        stderr,
        "[inline-deep-acyclic] expected a >6-call acyclic chain to inline\n");
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  for (size_t i = 0; i < DEPTH; ++i)
    nyr_ir_func_free(&funcs[i]);
  return ok;
}

static bool ny_check_cfg_inline_preserves_loop_phis(void) {
  nyr_func_t caller, callee;
  ny_build_loop_caller(&caller);
  ny_build_branching_leaf(&callee);
  const nyr_inline_callee_t callees[] = {{"branching_leaf", &callee}};
  bool ok = nyr_opt_verify(&callee, "inline-loop-phi/callee") &&
            nyr_opt_verify(&caller, "inline-loop-phi/before");
  nyr_set_inline_callees(callees, 1);
  ok = ok && nyr_inline_general(&caller) &&
       nyr_opt_verify(&caller, "inline-loop-phi/after") &&
       ny_count_calls(&caller) == 0 &&
       nyr_opt_eval(&caller, "inline-loop-phi") == 31;
  if (!ok)
    nyr_dump(stderr, &caller, "inline-loop-phi/after");
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&callee);
  return ok;
}

static void ny_build_large_leaf(nyr_func_t *f) {
  nyr_opt_begin(f);
  ny_set_i64_param(f);
  int value = ny_load_local(f, 0);
  int zero = nyr_opt_const(f, 0);
  for (int i = 0; i < 80; ++i)
    value = nyr_opt_binop(f, NYIR_ADD_I64, value, zero);
  nyr_opt_ret(f, value);
}

static bool ny_check_growth_budget_stops_rounds(void) {
  enum { CALLS = 100, CALLEES = 128 };
  nyr_func_t caller = {0};
  nyr_func_t callee = {0};
  nyr_opt_begin(&caller);
  int arg = nyr_opt_const(&caller, 1);
  for (int i = 0; i < CALLS; ++i)
    if (ny_call(&caller, "large_leaf", arg) < 0)
      return false;
  nyr_opt_ret(&caller, caller.next_value - 1);
  ny_build_large_leaf(&callee);

  nyr_inline_callee_t callees[CALLEES] = {0};
  callees[0] = (nyr_inline_callee_t){"large_leaf", &callee};
  for (size_t i = 1; i < CALLEES; ++i)
    callees[i] = (nyr_inline_callee_t){"unused", &callee};
  nyr_set_inline_callees(callees, CALLEES);
  bool ok = ny_verify_after_inline(&caller, "inline-growth-budget");
  size_t remaining_calls = ny_count_calls(&caller);
  ok = ok && remaining_calls > 0 && remaining_calls < CALLS;
  if (!ok)
    fprintf(stderr,
            "[inline-growth-budget] expected bounded partial inlining, "
            "remaining calls=%zu\n",
            remaining_calls);

  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&callee);
  return ok;
}

static bool ny_check_mutual_recursion(void) {
  nyr_func_t caller, a, b;
  ny_build_caller(&caller, "a");
  ny_build_wrapper(&a, "b");
  ny_build_wrapper(&b, "a");
  const nyr_inline_callee_t callees[] = {
      {"a", &a},
      {"b", &b},
  };
  nyr_set_inline_callees(callees, sizeof(callees) / sizeof(callees[0]));
  bool ok = ny_verify_after_inline(&caller, "inline-mutual-recursion") &&
            ny_count_calls(&caller) == 1;
  if (!ok)
    fprintf(
        stderr,
        "[inline-mutual-recursion] expected one recursive call to remain\n");
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&a);
  nyr_ir_func_free(&b);
  return ok;
}

static int ny_addr_symbol(nyr_func_t *f, const char *symbol) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .symbol = symbol,
                                     .scalar_class = NYIR_SCALAR_POINTER});
}

static bool ny_check_inlining_rebuilds_alias_classes(void) {
  nyr_func_t caller, callee;
  nyr_opt_begin(&callee);
  nyr_opt_ret(&callee, ny_addr_symbol(&callee, "callee_global"));
  nyr_opt_begin(&caller);
  int caller_addr = ny_addr_symbol(&caller, "caller_global");
  int result =
      nyr_ir_emit(&caller, (nyr_inst_t){.op = NYIR_CALL,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 0,
                                        .symbol = "alias_leaf",
                                        .scalar_class = NYIR_SCALAR_POINTER});
  nyr_opt_ret(&caller, result);
  const nyr_inline_callee_t callees[] = {{"alias_leaf", &callee}};
  bool ok = caller_addr >= 0 && result >= 0 && nyr_alias_analysis(&callee) &&
            nyr_alias_analysis(&caller) &&
            nyr_opt_verify(&callee, "inline-alias/callee") &&
            nyr_opt_verify(&caller, "inline-alias/caller");
  nyr_set_inline_callees(callees, 1);
  if (ok)
    ok = nyr_inline_general(&caller) &&
         nyr_opt_verify(&caller, "inline-alias/inlined") &&
         ny_count_calls(&caller) == 0;
  uint32_t caller_class = 0, callee_class = 0;
  for (size_t i = 0; ok && i < caller.len; ++i) {
    const nyr_inst_t *in = &caller.data[i];
    if (in->op != NYIR_ADDR_SYMBOL || !in->symbol)
      continue;
    if (strcmp(in->symbol, "caller_global") == 0)
      caller_class = in->alias_class;
    if (strcmp(in->symbol, "callee_global") == 0)
      callee_class = in->alias_class;
  }
  ok = ok && caller_class != 0 && callee_class != 0 &&
       caller_class != callee_class;
  if (!ok)
    fprintf(stderr,
            "[inline-alias] caller/callee allocation classes collided: %u/%u\n",
            caller_class, callee_class);
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&callee);
  return ok;
}

static bool ny_check_inlining_preserves_pointer_write(void) {
  nyr_func_t caller, callee;
  nyr_opt_begin(&callee);
  ny_set_i64_param(&callee);
  int address = ny_load_local(&callee, 0);
  int value = nyr_opt_const(&callee, 73);
  nyr_ir_emit(&callee, (nyr_inst_t){.op = NYIR_STORE_I64,
                                    .dst = -1,
                                    .a = address,
                                    .b = -1,
                                    .c = value,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .effects = NYIR_EFFECT_WRITE_MEMORY |
                                               NYIR_EFFECT_MAY_TRAP});
  nyr_opt_ret(&callee, nyr_opt_const(&callee, 0));

  nyr_opt_begin(&caller);
  int global = ny_addr_symbol(&caller, "inline_pointer_write_global");
  size_t call_at = caller.len;
  int ignored = ny_call(&caller, "write_pointer", global);
  caller.data[call_at].flags = NYIR_INST_F_EFFECTS_KNOWN;
  caller.data[call_at].effects =
      NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP;
  int loaded = nyr_ir_emit(&caller, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                 .dst = -1,
                                                 .a = global,
                                                 .b = -1,
                                                 .c = -1,
                                                 .d = -1,
                                                 .e = -1,
                                                 .f = -1});
  nyr_opt_ret(&caller, loaded);

  const nyr_inline_callee_t callees[] = {{"write_pointer", &callee}};
  nyr_set_inline_callees(callees, 1);
  bool ok = ignored >= 0 && global >= 0 &&
            nyr_opt_verify(&callee, "inline-pointer-write/callee") &&
            nyr_opt_verify(&caller, "inline-pointer-write/before") &&
            nyr_inline_general(&caller) &&
            nyr_opt_verify(&caller, "inline-pointer-write/after") &&
            ny_count_calls(&caller) == 0;
  size_t store_at = SIZE_MAX, load_at = SIZE_MAX;
  bool store_targets_global = false, stores_expected_value = false;
  for (size_t i = 0; ok && i < caller.len; ++i) {
    const nyr_inst_t *in = &caller.data[i];
    if (in->op == NYIR_STORE_I64 && in->effects & NYIR_EFFECT_WRITE_MEMORY) {
      store_at = i;
      store_targets_global = in->a == global;
      for (size_t j = 0; j < caller.len; ++j)
        stores_expected_value |= caller.data[j].op == NYIR_CONST_I64 &&
                                 caller.data[j].dst == in->c &&
                                 caller.data[j].imm == 73;
    }
    if (in->op == NYIR_LOAD_I64 && in->a == global)
      load_at = i;
  }
  ok = ok && store_at != SIZE_MAX && load_at != SIZE_MAX &&
       store_at < load_at && store_targets_global && stores_expected_value;
  if (!ok)
    nyr_dump(stderr, &caller, "inline-pointer-write/after");
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&callee);
  return ok;
}

static bool ny_check_inlining_preserves_aggregate_return_abi(void) {
  nyr_func_t caller, callee;
  nyr_opt_begin(&callee);
  int object_dst = callee.next_value;
  int object =
      nyr_ir_emit(&callee, (nyr_inst_t){.op = NYIR_CALL,
                                        .dst = object_dst,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 0,
                                        .symbol = "make_object",
                                        .scalar_class = NYIR_SCALAR_OBJECT});
  callee.next_value = object_dst + 1;
  nyr_opt_ret(&callee, object);

  nyr_opt_begin(&caller);
  int aggregate_dst = caller.next_value;
  int aggregate =
      nyr_ir_emit(&caller, (nyr_inst_t){.op = NYIR_CALL,
                                        .dst = aggregate_dst,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 0,
                                        .symbol = "aggregate_factory",
                                        .scalar_class = NYIR_SCALAR_AGGREGATE});
  caller.next_value = aggregate_dst + 1;
  nyr_inst_t consume = {.op = NYIR_CALL,
                        .dst = -1,
                        .a = aggregate,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = 1,
                        .symbol = "consume_aggregate"};
  nyr_call_arg_class_set(consume.arg_scalar_classes, 0, NYIR_SCALAR_AGGREGATE);
  size_t before_consume = caller.len;
  nyr_ir_emit(&caller, consume);
  bool ok = object >= 0 && aggregate >= 0 && caller.len == before_consume + 1;
  nyr_opt_ret(&caller, nyr_opt_const(&caller, 0));
  const nyr_inline_callee_t callees[] = {{"aggregate_factory", &callee}};
  nyr_set_inline_callees(callees, 1);
  ok = ok && nyr_opt_verify(&callee, "inline-aggregate-return/callee") &&
       nyr_opt_verify(&caller, "inline-aggregate-return/before") &&
       nyr_inline_general(&caller) &&
       nyr_opt_verify(&caller, "inline-aggregate-return/after");
  if (!ok)
    nyr_dump(stderr, &caller, "inline-aggregate-return/after");
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&callee);
  return ok;
}

static bool ny_check_inlining_preserves_unrepresented_call_effects(void) {
  nyr_func_t caller, callee;
  ny_build_leaf(&callee);
  ny_build_caller(&caller, "pure_leaf");
  caller.data[1].flags = NYIR_INST_F_EFFECTS_KNOWN;
  caller.data[1].effects = NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY;

  const nyr_inline_callee_t callees[] = {{"pure_leaf", &callee}};
  nyr_set_inline_callees(callees, 1);
  bool ok =
      nyr_opt_verify(&callee, "inline-unrepresented-effect/callee") &&
      nyr_opt_verify(&caller, "inline-unrepresented-effect/before") &&
      nyr_inline_small(&caller) &&
      nyr_opt_verify(&caller, "inline-unrepresented-effect/small") &&
      ny_count_calls(&caller) == 1 && nyr_inline_general(&caller) &&
      nyr_opt_verify(&caller, "inline-unrepresented-effect/general") &&
      ny_count_calls(&caller) == 1 && caller.data[1].op == NYIR_CALL &&
      caller.data[1].effects == (NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY);
  if (!ok)
    nyr_dump(stderr, &caller, "inline-unrepresented-effect/general");
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&callee);
  return ok;
}

int main(void) {
  return ny_check_wrapper_chain() && ny_check_deep_acyclic_chain() &&
                 ny_check_cfg_inline_preserves_loop_phis() &&
                 ny_check_growth_budget_stops_rounds() &&
                 ny_check_mutual_recursion() &&
                 ny_check_inlining_rebuilds_alias_classes() &&
                 ny_check_inlining_preserves_pointer_write() &&
                 ny_check_inlining_preserves_aggregate_return_abi() &&
                 ny_check_inlining_preserves_unrepresented_call_effects()
             ? 0
             : 1;
}
