#include "support.h"

static int64_t ipa_pointer_write_cell;
static int64_t ipa_pointer_other_cell;

static int call_known(nyr_func_t *f, const char *name, int arg) {
  int dst = f->next_value;
  nyr_inst_t in = {.op = NYIR_CALL,
                    .dst = dst,
                    .a = arg,
                    .b = -1,
                    .c = -1,
                    .d = -1,
                    .e = -1,
                    .f = -1,
                    .imm = 1,
                    .symbol = name,
                    .flags = NYIR_INST_F_EFFECTS_KNOWN,
                    .effects = NYIR_EFFECT_CALL};
  if (nyr_ir_emit(f, in) != dst)
    return -1;
  f->data[f->len - 1].flags |= NYIR_INST_F_EFFECTS_KNOWN;
  f->data[f->len - 1].effects = NYIR_EFFECT_CALL;
  f->next_value = dst + 1;
  return dst;
}

static int call_known_two(nyr_func_t *f, const char *name, int first,
                          int second) {
  int dst = f->next_value;
  if (nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                 .dst = dst,
                                 .a = first,
                                 .b = second,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .imm = 2,
                                 .symbol = name,
                                 .flags = NYIR_INST_F_EFFECTS_KNOWN,
                                 .effects = NYIR_EFFECT_CALL}) != dst)
    return -1;
  f->data[f->len - 1].flags |= NYIR_INST_F_EFFECTS_KNOWN;
  f->data[f->len - 1].effects = NYIR_EFFECT_CALL;
  f->next_value = dst + 1;
  return dst;
}

static void build_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_count = 1;
  int x = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = 0});
  int one = nyr_opt_const(f, 1);
  int sum = nyr_opt_binop(f, NYIR_ADD_I64, x, one);
  nyr_opt_ret(f, sum);
}

static void build_budget_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  for (size_t i = 0; i < 50001; ++i)
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_NOP,
                               .dst = -1,
                               .a = -1,
                               .b = -1,
                               .c = -1,
                               .d = -1,
                               .e = -1,
                               .f = -1});
  nyr_opt_ret(f, nyr_opt_const(f, 7));
}

static void build_loop_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  int limit = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 0});
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  size_t sum_phi = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  int keep_going = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, keep_going, 11);
  nyr_opt_label(f, 12);
  nyr_opt_ret(f, sum);
  nyr_opt_label(f, 11);
  int sum_next = nyr_opt_binop(f, NYIR_ADD_I64, sum, iv);
  int iv_next = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = iv_next;
  f->data[sum_phi].phi_incoming[1].value = sum_next;
}

static void build_select_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  int input = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 0});
  int zero = nyr_opt_const(f, 0);
  int negative = nyr_opt_const(f, 10);
  int nonnegative = nyr_opt_const(f, 20);
  int is_negative = nyr_opt_cmp(f, input, zero, NYIR_CMP_LT);
  int selected = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_SELECT_I64,
                                            .dst = -1,
                                            .a = is_negative,
                                            .b = negative,
                                            .c = nonnegative,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1});
  nyr_opt_ret(f, selected);
}

static void build_unbounded_recursive_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  int x = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = 0});
  int result = call_known(f, "unbounded_recursive", x);
  nyr_opt_ret(f, result);
}

static void build_bounded_recursive_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  int x = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = 0});
  int zero = nyr_opt_const(f, 0);
  int is_zero = nyr_opt_cmp(f, x, zero, NYIR_CMP_EQ);
  nyr_opt_br_if(f, is_zero, 10);
  int one = nyr_opt_const(f, 1);
  int next = nyr_opt_binop(f, NYIR_SUB_I64, x, one);
  int result = call_known(f, "bounded_recursive", next);
  nyr_opt_ret(f, result);
  nyr_opt_label(f, 10);
  nyr_opt_ret(f, zero);
}

static void build_effectful_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  int x = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = 0});
  nyr_ir_emit(
      f, (nyr_inst_t){.op = NYIR_CALL,
                       .dst = -1,
                       .a = x,
                       .b = -1,
                       .c = -1,
                       .d = -1,
                       .e = -1,
                       .f = -1,
                       .imm = 1,
                       .symbol = "unknown_mutator",
                       .flags = NYIR_INST_F_EFFECTS_KNOWN,
                       .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY});
  nyr_opt_ret(f, x);
}

static void build_pointer_write_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  int address = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                           .dst = -1,
                                           .a = -1,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = 0});
  int value = nyr_opt_const(f, 99);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = value,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_MAY_TRAP});
  nyr_opt_ret(f, nyr_opt_const(f, 7));
}

static void build_pointer_write_second_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(2 * sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_types[1] = NYIR_PARAM_I64;
  f->param_count = 2;
  int address = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                           .dst = -1,
                                           .a = -1,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = 1});
  int value = nyr_opt_const(f, 99);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = value,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_MAY_TRAP});
  nyr_opt_ret(f, nyr_opt_const(f, 7));
}

static void build_pointer_read_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  int address = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                           .dst = -1,
                                           .a = -1,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = 0});
  int value = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
  nyr_opt_ret(f, value);
}

static bool check_pointer_write_call_barrier(nyr_func_t *callee) {
  nyr_func_t caller;
  nyr_opt_begin(&caller);
  int address =
      nyr_opt_const(&caller, (int64_t)(intptr_t)&ipa_pointer_write_cell);
  int other_address =
      nyr_opt_const(&caller, (int64_t)(intptr_t)&ipa_pointer_other_cell);
  int before = nyr_ir_emit(&caller, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                .dst = -1,
                                                .a = address,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
  size_t call_at = caller.len;
  int call_result = call_known(&caller, "write_through_arg", address);
  caller.data[call_at].effects =
      NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP;
  int after = nyr_ir_emit(&caller, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                               .dst = -1,
                                               .a = address,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1});
  int other_after = nyr_ir_emit(&caller, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                     .dst = -1,
                                                     .a = other_address,
                                                     .b = -1,
                                                     .c = -1,
                                                     .d = -1,
                                                     .e = -1,
                                                     .f = -1});
  int sum = nyr_opt_binop(&caller, NYIR_ADD_I64, before, after);
  sum = nyr_opt_binop(&caller, NYIR_ADD_I64, sum, other_after);
  int result = nyr_opt_binop(&caller, NYIR_ADD_I64, sum, call_result);
  nyr_opt_ret(&caller, result);

  bool ok =
      nyr_opt_verify(callee, "ipa-pointer-write-callee") &&
      nyr_opt_verify(&caller, "ipa-pointer-write-before") &&
      nyr_ipa_constprop(&caller) && nyr_dce(&caller) &&
      nyr_opt_verify(&caller, "ipa-pointer-write-after") &&
      caller.data[call_at].op == NYIR_CALL && caller.data[call_at].dst == -1 &&
      caller.data[call_at].effects ==
          (NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP);
  bool saw_before = false, saw_after_load = false, saw_after_value = false;
  bool saw_other_load = false, saw_result = false;
  for (size_t i = 0; i < caller.len; ++i) {
    const nyr_inst_t *in = &caller.data[i];
    saw_before |= in->dst == before && in->op == NYIR_LOAD_I64;
    saw_after_load |= in->dst == after && in->op == NYIR_LOAD_I64;
    saw_after_value |=
        in->dst == after && in->op == NYIR_CONST_I64 && in->imm == 99;
    saw_other_load |= in->dst == other_after && in->op == NYIR_LOAD_I64 &&
                      in->a == other_address;
    saw_result |=
        in->dst == call_result && in->op == NYIR_CONST_I64 && in->imm == 7;
  }
  ok = ok && saw_before && !saw_after_load && saw_after_value &&
       saw_other_load && saw_result;
  if (!ok)
    nyr_dump(stderr, &caller, "ipa-pointer-write-after");
  nyr_ir_func_free(&caller);
  return ok;
}

static bool check_pointer_read_call_barrier(nyr_func_t *callee) {
  nyr_func_t caller;
  nyr_opt_begin(&caller);
  int address =
      nyr_opt_const(&caller, (int64_t)(intptr_t)&ipa_pointer_write_cell);
  size_t call_at = caller.len;
  int result = call_known(&caller, "read_through_arg", address);
  caller.data[call_at].effects =
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP;
  nyr_opt_ret(&caller, result);

  bool ok =
      nyr_opt_verify(callee, "ipa-pointer-read-callee") &&
      nyr_opt_verify(&caller, "ipa-pointer-read-before") &&
      nyr_ipa_constprop(&caller) &&
      nyr_opt_verify(&caller, "ipa-pointer-read-after") &&
      caller.data[call_at].op == NYIR_CALL &&
      caller.data[call_at].effects ==
          (NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP);
  if (!ok)
    nyr_dump(stderr, &caller, "ipa-pointer-read-after");
  nyr_ir_func_free(&caller);
  return ok;
}

static bool check_pointer_alias_write_forwarding(void) {
  nyr_func_t caller, callee;
  build_pointer_write_second_callee(&callee);
  nyr_opt_begin(&caller);
  int first_address =
      nyr_opt_const(&caller, (int64_t)(intptr_t)&ipa_pointer_write_cell);
  int second_address =
      nyr_opt_const(&caller, (int64_t)(intptr_t)&ipa_pointer_write_cell);
  size_t call_at = caller.len;
  int call_result = call_known_two(&caller, "write_second_arg", first_address,
                                   second_address);
  caller.data[call_at].effects =
      NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP;
  int loaded = nyr_ir_emit(&caller, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                .dst = -1,
                                                .a = first_address,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
  int result = nyr_opt_binop(&caller, NYIR_ADD_I64, loaded, call_result);
  nyr_opt_ret(&caller, result);
  const nyr_inline_callee_t entry = {"write_second_arg", &callee};
  nyr_set_inline_callees(&entry, 1);

  bool ok =
      nyr_opt_verify(&callee, "ipa-pointer-alias-callee") &&
      nyr_opt_verify(&caller, "ipa-pointer-alias-before") &&
      nyr_ipa_constprop(&caller) && nyr_dce(&caller) &&
      nyr_opt_verify(&caller, "ipa-pointer-alias-after") &&
      caller.data[call_at].op == NYIR_CALL && caller.data[call_at].dst == -1 &&
      caller.data[call_at].effects ==
          (NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP);
  bool saw_forwarded_load = false, saw_call_result = false;
  for (size_t i = 0; i < caller.len; ++i) {
    const nyr_inst_t *in = &caller.data[i];
    saw_forwarded_load |=
        in->dst == loaded && in->op == NYIR_CONST_I64 && in->imm == 99;
    saw_call_result |=
        in->dst == call_result && in->op == NYIR_CONST_I64 && in->imm == 7;
  }
  ok = ok && saw_forwarded_load && saw_call_result;
  if (!ok)
    nyr_dump(stderr, &caller, "ipa-pointer-alias-after");
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&callee);
  return ok;
}

static bool check_pointer_write_forwarding_stops_at_mutator(void) {
  nyr_func_t caller, callee;
  build_pointer_write_callee(&callee);
  nyr_opt_begin(&caller);
  int address =
      nyr_opt_const(&caller, (int64_t)(intptr_t)&ipa_pointer_write_cell);
  int call_result = call_known(&caller, "write_through_arg", address);
  caller.data[caller.len - 1].effects =
      NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP;
  nyr_ir_emit(&caller, (nyr_inst_t){.op = NYIR_CALL,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .imm = 0,
                                   .symbol = "unknown_memory_mutator",
                                   .flags = NYIR_INST_F_EFFECTS_KNOWN,
                                   .effects = NYIR_EFFECT_CALL |
                                              NYIR_EFFECT_WRITE_MEMORY |
                                              NYIR_EFFECT_UNKNOWN_SIDE_EFFECT});
  int loaded = nyr_ir_emit(&caller, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                .dst = -1,
                                                .a = address,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
  int result = nyr_opt_binop(&caller, NYIR_ADD_I64, loaded, call_result);
  nyr_opt_ret(&caller, result);
  const nyr_inline_callee_t entry = {"write_through_arg", &callee};
  nyr_set_inline_callees(&entry, 1);

  bool ok = nyr_opt_verify(&callee, "ipa-pointer-mutator-callee") &&
            nyr_opt_verify(&caller, "ipa-pointer-mutator-before") &&
            nyr_ipa_constprop(&caller) && nyr_dce(&caller) &&
            nyr_opt_verify(&caller, "ipa-pointer-mutator-after");
  bool saw_load = false, saw_mutator = false;
  for (size_t i = 0; i < caller.len; ++i) {
    saw_load |=
        caller.data[i].dst == loaded && caller.data[i].op == NYIR_LOAD_I64;
    saw_mutator |= caller.data[i].op == NYIR_CALL && caller.data[i].symbol &&
                   strcmp(caller.data[i].symbol, "unknown_memory_mutator") == 0;
  }
  ok = ok && saw_load && saw_mutator;
  if (!ok)
    nyr_dump(stderr, &caller, "ipa-pointer-mutator-after");
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&callee);
  return ok;
}

static bool check_pointer_write_summary_mismatch(nyr_func_t *callee) {
  nyr_func_t caller;
  nyr_opt_begin(&caller);
  int address =
      nyr_opt_const(&caller, (int64_t)(intptr_t)&ipa_pointer_write_cell);
  size_t call_at = caller.len;
  int result = call_known(&caller, "write_through_arg", address);
  /*
   * Store may trap; this incomplete summary cannot authorize specialization.
   */
  caller.data[call_at].effects = NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY;
  nyr_opt_ret(&caller, result);

  bool ok = nyr_opt_verify(callee, "ipa-pointer-write-mismatch-callee") &&
            nyr_opt_verify(&caller, "ipa-pointer-write-mismatch-before") &&
            nyr_ipa_constprop(&caller) &&
            nyr_opt_verify(&caller, "ipa-pointer-write-mismatch-after") &&
            caller.data[call_at].op == NYIR_CALL &&
            caller.data[call_at].dst == result;
  for (size_t i = 0; ok && i < caller.len; ++i)
    if (caller.data[i].op == NYIR_CONST_I64 && caller.data[i].dst == result)
      ok = false;
  if (!ok)
    nyr_dump(stderr, &caller, "ipa-pointer-write-mismatch-after");
  nyr_ir_func_free(&caller);
  return ok;
}

static bool check_declared_effect_call_barrier(void) {
  nyr_func_t caller;
  nyr_opt_begin(&caller);
  int argument = nyr_opt_const(&caller, 41);
  size_t call_at = caller.len;
  int result = call_known(&caller, "pure_add_one", argument);
  caller.data[call_at].effects = NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY;
  nyr_opt_ret(&caller, result);

  bool ok = nyr_opt_verify(&caller, "ipa-declared-effect-before") &&
            nyr_ipa_constprop(&caller) &&
            nyr_opt_verify(&caller, "ipa-declared-effect-after") &&
            caller.data[call_at].op == NYIR_CALL &&
            caller.data[call_at].effects ==
                (NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY);
  if (!ok)
    nyr_dump(stderr, &caller, "ipa-declared-effect-after");
  nyr_ir_func_free(&caller);
  return ok;
}

static void build_trapping_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  int divisor = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                           .dst = -1,
                                           .a = -1,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = 0});
  int one = nyr_opt_const(f, 1);
  int quotient = nyr_opt_binop(f, NYIR_DIV_I64, one, divisor);
  nyr_opt_ret(f, quotient);
}

static void build_malformed_callee(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  int condition = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                             .dst = -1,
                                             .a = -1,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .imm = 0});
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_BR_IF,
                             .dst = -1,
                             .a = condition,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 999});
  nyr_opt_ret(f, nyr_opt_const(f, 9));
}

int main(void) {
  nyr_func_t caller, callee, overflow_caller, recursive_caller, recursive,
      bounded_caller, bounded, effectful_caller, effectful, malformed_caller,
      malformed, trapping_caller, trapping_callee, budget_caller, budget_callee,
      loop_caller, loop_callee, select_caller, select_callee, select_arg_caller;
  nyr_func_t pointer_write_callee, pointer_read_callee;
  nyr_opt_begin(&caller);
  int arg = nyr_opt_const(&caller, 41);
  int result = call_known(&caller, "pure_add_one", arg);
  nyr_opt_ret(&caller, result);

  nyr_opt_begin(&overflow_caller);
  int max = nyr_opt_const(&overflow_caller, INT64_MAX);
  int overflow_result = call_known(&overflow_caller, "pure_add_one", max);
  nyr_opt_ret(&overflow_caller, overflow_result);

  build_callee(&callee);
  build_unbounded_recursive_callee(&recursive);
  nyr_opt_begin(&recursive_caller);
  int recursive_arg = nyr_opt_const(&recursive_caller, 1);
  int recursive_result =
      call_known(&recursive_caller, "unbounded_recursive", recursive_arg);
  nyr_opt_ret(&recursive_caller, recursive_result);
  build_bounded_recursive_callee(&bounded);
  nyr_opt_begin(&bounded_caller);
  int bounded_arg = nyr_opt_const(&bounded_caller, 3);
  int bounded_result =
      call_known(&bounded_caller, "bounded_recursive", bounded_arg);
  nyr_opt_ret(&bounded_caller, bounded_result);
  nyr_opt_begin(&effectful_caller);
  int effectful_arg = nyr_opt_const(&effectful_caller, 17);
  int effectful_result =
      call_known(&effectful_caller, "effectful", effectful_arg);
  nyr_opt_ret(&effectful_caller, effectful_result);
  build_effectful_callee(&effectful);
  build_pointer_write_callee(&pointer_write_callee);
  build_pointer_read_callee(&pointer_read_callee);
  nyr_opt_begin(&malformed_caller);
  int malformed_arg = nyr_opt_const(&malformed_caller, 1);
  int malformed_result =
      call_known(&malformed_caller, "malformed", malformed_arg);
  nyr_opt_ret(&malformed_caller, malformed_result);
  build_malformed_callee(&malformed);
  build_budget_callee(&budget_callee);
  nyr_opt_begin(&budget_caller);
  int budget_arg1 = nyr_opt_const(&budget_caller, 1);
  int budget_result1 = call_known(&budget_caller, "budgeted", budget_arg1);
  int budget_arg2 = nyr_opt_const(&budget_caller, 2);
  int budget_result2 = call_known(&budget_caller, "budgeted", budget_arg2);
  nyr_opt_ret(&budget_caller, budget_result2);
  build_loop_callee(&loop_callee);
  nyr_opt_begin(&loop_caller);
  int loop_arg = nyr_opt_const(&loop_caller, 4);
  int loop_result = call_known(&loop_caller, "sum_to_n", loop_arg);
  nyr_opt_ret(&loop_caller, loop_result);
  build_select_callee(&select_callee);
  nyr_opt_begin(&select_caller);
  int select_arg = nyr_opt_const(&select_caller, -1);
  int select_result = call_known(&select_caller, "select_value", select_arg);
  nyr_opt_ret(&select_caller, select_result);
  nyr_opt_begin(&select_arg_caller);
  int select_condition = nyr_opt_const(&select_arg_caller, 0);
  int select_true = nyr_opt_const(&select_arg_caller, 41);
  int select_false = nyr_opt_const(&select_arg_caller, 1);
  int selected_arg =
      nyr_ir_emit(&select_arg_caller, (nyr_inst_t){.op = NYIR_SELECT_I64,
                                                  .dst = -1,
                                                  .a = select_condition,
                                                  .b = select_true,
                                                  .c = select_false,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1});
  int select_arg_result =
      call_known(&select_arg_caller, "pure_add_one", selected_arg);
  nyr_opt_ret(&select_arg_caller, select_arg_result);
  nyr_inline_callee_t entries[11] = {
      {"pure_add_one", &callee},
      {"unbounded_recursive", &recursive},
      {"bounded_recursive", &bounded},
      {"effectful", &effectful},
      {"malformed", &malformed},
      {"budgeted", &budget_callee},
      {"sum_to_n", &loop_callee},
      {"select_value", &select_callee},
      {"write_through_arg", &pointer_write_callee},
      {"read_through_arg", &pointer_read_callee}};
  nyr_set_inline_callees(entries, 1);
  bool ok = nyr_opt_verify(&caller, "ipa-before") &&
            nyr_ipa_constprop(&caller) &&
            nyr_opt_verify(&caller, "ipa-after") &&
            caller.data[1].op == NYIR_CONST_I64 && caller.data[1].imm == 42 &&
            nyr_opt_eval(&caller, "ipa-eval") == 42;
  /*
   * The evaluator uses the IR's wrapping integer semantics for scalar calls.
   * The recursive case has no base case; the optimizer must exhaust its
   * bounded evaluation and retain the call rather than rewriting it.
   */
  nyr_set_inline_callees(entries, 5);
  ok = ok && nyr_opt_verify(&overflow_caller, "ipa-overflow-before") &&
       nyr_ipa_constprop(&overflow_caller) &&
       nyr_opt_verify(&overflow_caller, "ipa-overflow-after") &&
       overflow_caller.data[1].op == NYIR_CONST_I64 &&
       overflow_caller.data[1].imm == INT64_MIN &&
       nyr_opt_verify(&recursive, "ipa-recursive-callee") &&
       nyr_opt_verify(&recursive_caller, "ipa-recursive-before") &&
       nyr_ipa_constprop(&recursive_caller) &&
       nyr_opt_verify(&recursive_caller, "ipa-recursive-after") &&
       recursive_caller.data[1].op == NYIR_CALL &&
       nyr_opt_verify(&bounded, "ipa-bounded-callee") &&
       nyr_opt_verify(&bounded_caller, "ipa-bounded-before") &&
       nyr_ipa_constprop(&bounded_caller) &&
       nyr_opt_verify(&bounded_caller, "ipa-bounded-after") &&
       bounded_caller.data[1].op == NYIR_CONST_I64 &&
       bounded_caller.data[1].imm == 0 &&
       nyr_opt_verify(&effectful, "ipa-effectful-callee") &&
       nyr_opt_verify(&effectful_caller, "ipa-effectful-before") &&
       nyr_ipa_constprop(&effectful_caller) &&
       nyr_opt_verify(&effectful_caller, "ipa-effectful-after") &&
       effectful_caller.data[1].op == NYIR_CALL &&
       !nyr_opt_verify(&malformed, "ipa-malformed-callee") &&
       nyr_opt_verify(&malformed_caller, "ipa-malformed-before") &&
       nyr_ipa_constprop(&malformed_caller) &&
       nyr_opt_verify(&malformed_caller, "ipa-malformed-after") &&
       malformed_caller.data[1].op == NYIR_CALL;
  /*
   * The evaluator budget is shared by all call sites in one caller. Each
   * callee needs just over half of the 100,000-instruction budget, so only the
   * first result may be folded; the second call must remain as the fallback.
   */
  nyr_set_inline_callees(entries, 8);
  ok = ok && nyr_opt_verify(&budget_callee, "ipa-budget-callee") &&
       nyr_opt_verify(&budget_caller, "ipa-budget-before") &&
       nyr_ipa_constprop(&budget_caller) &&
       nyr_opt_verify(&budget_caller, "ipa-budget-after") &&
       budget_caller.data[1].dst == budget_result1 &&
       budget_caller.data[1].op == NYIR_CONST_I64 &&
       budget_caller.data[1].imm == 7 &&
       budget_caller.data[3].dst == budget_result2 &&
       budget_caller.data[3].op == NYIR_CALL;
  ok = ok && nyr_opt_verify(&loop_callee, "ipa-loop-callee") &&
       nyr_opt_verify(&loop_caller, "ipa-loop-before") &&
       nyr_ipa_constprop(&loop_caller) &&
       nyr_opt_verify(&loop_caller, "ipa-loop-after") &&
       loop_caller.data[1].op == NYIR_CONST_I64 && loop_caller.data[1].imm == 6;
  ok = ok && nyr_opt_verify(&select_callee, "ipa-select-callee") &&
       nyr_opt_verify(&select_caller, "ipa-select-before") &&
       nyr_ipa_constprop(&select_caller) &&
       nyr_opt_verify(&select_caller, "ipa-select-after") &&
       select_caller.data[1].op == NYIR_CONST_I64 &&
       select_caller.data[1].imm == 10 &&
       nyr_opt_verify(&select_arg_caller, "ipa-select-arg-before") &&
       nyr_ipa_constprop(&select_arg_caller) &&
       nyr_opt_verify(&select_arg_caller, "ipa-select-arg-after") &&
       select_arg_caller.data[4].dst == select_arg_result &&
       select_arg_caller.data[4].op == NYIR_CONST_I64 &&
       select_arg_caller.data[4].imm == 2;
  build_trapping_callee(&trapping_callee);
  nyr_opt_begin(&trapping_caller);
  int zero_divisor = nyr_opt_const(&trapping_caller, 0);
  int trapping_result = call_known(&trapping_caller, "trapping", zero_divisor);
  nyr_opt_ret(&trapping_caller, trapping_result);
  entries[9] = (nyr_inline_callee_t){"trapping", &trapping_callee};
  nyr_set_inline_callees(entries, 11);
  ok = ok && nyr_opt_verify(&trapping_callee, "ipa-trapping-callee") &&
       nyr_opt_verify(&trapping_caller, "ipa-trapping-before") &&
       nyr_ipa_constprop(&trapping_caller) &&
       nyr_opt_verify(&trapping_caller, "ipa-trapping-after") &&
       trapping_caller.data[1].op == NYIR_CALL &&
       check_pointer_write_call_barrier(&pointer_write_callee) &&
       check_pointer_write_summary_mismatch(&pointer_write_callee) &&
       check_pointer_read_call_barrier(&pointer_read_callee) &&
       check_pointer_alias_write_forwarding() &&
       check_pointer_write_forwarding_stops_at_mutator() &&
       check_declared_effect_call_barrier();
  nyr_set_inline_callees(NULL, 0);
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&callee);
  nyr_ir_func_free(&overflow_caller);
  nyr_ir_func_free(&recursive_caller);
  nyr_ir_func_free(&recursive);
  nyr_ir_func_free(&bounded_caller);
  nyr_ir_func_free(&bounded);
  nyr_ir_func_free(&effectful_caller);
  nyr_ir_func_free(&effectful);
  nyr_ir_func_free(&malformed_caller);
  nyr_ir_func_free(&malformed);
  nyr_ir_func_free(&trapping_caller);
  nyr_ir_func_free(&trapping_callee);
  nyr_ir_func_free(&budget_caller);
  nyr_ir_func_free(&budget_callee);
  nyr_ir_func_free(&loop_caller);
  nyr_ir_func_free(&loop_callee);
  nyr_ir_func_free(&select_caller);
  nyr_ir_func_free(&select_callee);
  nyr_ir_func_free(&select_arg_caller);
  nyr_ir_func_free(&pointer_write_callee);
  nyr_ir_func_free(&pointer_read_callee);
  if (!ok)
    fprintf(stderr, "[ipa-constprop] regression failed\n");
  return ok ? 0 : 1;
}
