/*
 * Regression coverage for memory-address alias classes and pointer escapes.
 */
#include "code/backend/ir/opt/loop.h"
#include "code/backend/ir/opt/util.h"
#include "code/lang/parse/ast.h"
#include "support.h"

static bool run_memory_address_classes(void) {
  static const nyr_op_t accesses[] = {
      NYIR_LOAD_I64,       NYIR_STORE_I64,      NYIR_VEC4_LOAD_F64,
      NYIR_VEC4_STORE_F64, NYIR_VEC8_LOAD_F32,  NYIR_VEC8_STORE_F32,
      NYIR_VEC4_LOAD_I64,  NYIR_VEC4_STORE_I64, NYIR_VEC8_LOAD_I64,
      NYIR_VEC8_STORE_I64,
  };
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_opt_const(&f, 9);
  int global_a = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                             .dst = -1,
                                             .a = -1,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .symbol = "shared_global"});
  int global_a_again = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .c = -1,
                                                   .d = -1,
                                                   .e = -1,
                                                   .f = -1,
                                                   .symbol = "shared_global"});
  int global_b = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                             .dst = -1,
                                             .a = -1,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .symbol = "other_global"});
  size_t access_at[sizeof(accesses) / sizeof(accesses[0])];
  for (size_t i = 0; i < sizeof(accesses) / sizeof(accesses[0]); ++i) {
    int addr = (i & 1u) ? global_a_again : global_a;
    access_at[i] = f.len;
    nyr_ir_emit(&f, (nyr_inst_t){.op = accesses[i],
                                .dst = -1,
                                .a = addr,
                                .b = value,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .imm = 8});
  }
  size_t other_load_at = f.len;
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                              .dst = -1,
                              .a = global_b,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  size_t unknown_load_at = f.len;
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                              .dst = -1,
                              .a = value,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1});

  bool ok = nyr_alias_analysis(&f);
  uint32_t shared_class = f.data[access_at[0]].alias_class;
  ok = ok && shared_class != 0 &&
       f.data[(size_t)global_a].alias_class == shared_class &&
       f.data[(size_t)global_a_again].alias_class == shared_class;
  for (size_t i = 0; i < sizeof(access_at) / sizeof(access_at[0]); ++i)
    ok = ok && f.data[access_at[i]].alias_class == shared_class;
  ok = ok && f.data[other_load_at].alias_class != 0 &&
       f.data[other_load_at].alias_class != shared_class &&
       f.data[unknown_load_at].alias_class == 0;
  if (!ok)
    fprintf(
        stderr,
        "[alias-contracts] vector/scalar memory classes were not propagated\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_local_address_escape(size_t argument_index) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int local_addr = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = 0});
  int filler = nyr_opt_const(&f, 0);
  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = -1,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = (int64_t)argument_index + 1,
                      .symbol = "unknown_pointer_consumer"};
  int args[6] = {filler, filler, filler, filler, filler, filler};
  if (argument_index < 6) {
    args[argument_index] = local_addr;
    call.a = args[0];
    call.b = args[1];
    call.c = args[2];
    call.d = args[3];
    call.e = args[4];
    call.f = args[5];
  } else {
    call.a = filler;
    call.b = filler;
    call.c = filler;
    call.d = filler;
    call.e = filler;
    call.f = filler;
    call.extra_args_len = argument_index - 5;
    call.extra_args = malloc(call.extra_args_len * sizeof(*call.extra_args));
    if (!call.extra_args) {
      nyr_ir_func_free(&f);
      return false;
    }
    for (size_t i = 0; i < call.extra_args_len; ++i)
      call.extra_args[i] = filler;
    call.extra_args[call.extra_args_len - 1] = local_addr;
  }
  nyr_ir_emit(&f, call);
  bool ok =
      nyr_alias_analysis(&f) && f.data[(size_t)local_addr].alias_class == 0;
  if (!ok)
    fprintf(stderr,
            "[alias-contracts] local address in call argument %zu was not "
            "escaped\n",
            argument_index);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_derived_local_address_escape(nyr_op_t derive_op,
                                             size_t argument_index) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int local_addr = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = 0});
  int derived_addr = local_addr;
  if (derive_op == NYIR_COPY) {
    derived_addr =
        nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                    .dst = -1,
                                    .a = local_addr,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .scalar_class = NYIR_SCALAR_POINTER});
  } else {
    int offset = nyr_opt_const(&f, 8);
    derived_addr =
        nyr_ir_emit(&f, (nyr_inst_t){.op = derive_op,
                                    .dst = -1,
                                    .a = local_addr,
                                    .b = offset,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .scalar_class = NYIR_SCALAR_POINTER});
  }
  int filler = nyr_opt_const(&f, 0);
  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = filler,
                      .b = filler,
                      .c = filler,
                      .d = filler,
                      .e = filler,
                      .f = filler,
                      .imm = (int64_t)argument_index + 1,
                      .symbol = "unknown_pointer_consumer"};
  if (argument_index < 6) {
    int *args[] = {&call.a, &call.b, &call.c, &call.d, &call.e, &call.f};
    *args[argument_index] = derived_addr;
  } else {
    call.extra_args_len = argument_index - 5;
    call.extra_args = malloc(call.extra_args_len * sizeof(*call.extra_args));
    if (!call.extra_args) {
      nyr_ir_func_free(&f);
      return false;
    }
    for (size_t i = 0; i < call.extra_args_len; ++i)
      call.extra_args[i] = filler;
    call.extra_args[call.extra_args_len - 1] = derived_addr;
  }
  nyr_ir_emit(&f, call);
  bool ok = nyr_alias_analysis(&f) &&
            f.data[(size_t)local_addr].alias_class == 0 &&
            f.data[(size_t)derived_addr].alias_class == 0;
  if (!ok)
    fprintf(stderr,
            "[alias-contracts] local address escaped through op=%s at arg=%zu "
            "but kept a private class\n",
            nyr_op_name(derive_op), argument_index);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_local_address_escape_through_sink(nyr_op_t derive_op,
                                                  bool store_value) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int local_addr = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = 0});
  int escaped_value = local_addr;
  if (derive_op == NYIR_COPY) {
    escaped_value =
        nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                    .dst = -1,
                                    .a = local_addr,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .scalar_class = NYIR_SCALAR_POINTER});
  } else if (derive_op == NYIR_ADD_I64 || derive_op == NYIR_SUB_I64) {
    int offset = nyr_opt_const(&f, 8);
    escaped_value =
        nyr_ir_emit(&f, (nyr_inst_t){.op = derive_op,
                                    .dst = -1,
                                    .a = local_addr,
                                    .b = offset,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .scalar_class = NYIR_SCALAR_POINTER});
  }
  if (store_value) {
    int global_addr = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                  .dst = -1,
                                                  .a = -1,
                                                  .b = -1,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1,
                                                  .symbol = "escape_sink"});
    nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                .dst = -1,
                                .a = global_addr,
                                .b = -1,
                                .c = escaped_value,
                                .d = -1,
                                .e = -1,
                                .f = -1});
  } else {
    nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_RET,
                                .dst = -1,
                                .a = escaped_value,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1});
  }
  bool ok = nyr_alias_analysis(&f) &&
            f.data[(size_t)local_addr].alias_class == 0 &&
            f.data[(size_t)escaped_value].alias_class == 0;
  if (!ok)
    fprintf(stderr,
            "[alias-contracts] local address escaped through %s sink "
            "(derive=%s) but kept a private class\n",
            store_value ? "store" : "return", nyr_op_name(derive_op));
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_recomputed_facts_clear_stale_classes(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  size_t address_at = f.len;
  int address = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                            .dst = -1,
                                            .a = -1,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .symbol = "was_a_pointer"});
  size_t load_at = f.len;
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                              .dst = -1,
                              .a = address,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  bool ok = nyr_alias_analysis(&f) && f.data[address_at].alias_class != 0 &&
            f.data[load_at].alias_class == f.data[address_at].alias_class;
  /*
   * Simulate an IR rewrite that changes the producer's provenance.
   */
  f.data[address_at].op = NYIR_CONST_I64;
  f.data[address_at].imm = 11;
  ok = nyr_alias_analysis(&f) && ok && f.data[address_at].alias_class == 0 &&
       f.data[load_at].alias_class == 0;
  if (!ok)
    fprintf(stderr,
            "[alias-contracts] recomputation retained a stale alias class\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_rewrite_clears_derived_alias_class(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = 0,
                                  .scalar_class = NYIR_SCALAR_POINTER});
  size_t folded_at = f.len;
  int folded = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_SUB_I64,
                                           .dst = -1,
                                           .a = address,
                                           .b = address,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  bool ok = nyr_alias_analysis(&f) && f.data[folded_at].alias_class != 0;
  nir_make_const(&f.data[folded_at], 0);
  ok = ok && f.data[folded_at].op == NYIR_CONST_I64 &&
       f.data[folded_at].alias_class == 0 && f.data[folded_at].range.has_min &&
       f.data[folded_at].range.has_max && f.data[folded_at].range.min == 0 &&
       f.data[folded_at].range.max == 0;

  /*
   * A COPY rewrite also starts with unknown alias provenance, even when the
   * old operation had a concrete class that may belong to another operand.
   */
  f.data[folded_at] = (nyr_inst_t){
      .op = NYIR_ADD_I64,
      .dst = folded,
      .a = address,
      .b = address,
      .c = -1,
      .d = -1,
      .e = -1,
      .f = -1,
      .alias_class = f.data[(size_t)address].alias_class,
      .range = {.has_min = true, .has_max = true, .min = 123, .max = 123}};
  nir_make_copy(&f.data[folded_at], address);
  ok = ok && f.data[folded_at].op == NYIR_COPY &&
       f.data[folded_at].alias_class == 0 && !f.data[folded_at].range.has_min &&
       !f.data[folded_at].range.has_max;

  /*
   * Floating constant rewrites share the same derived-alias invalidation
   * contract while retaining semantic representation and ownership facts.
   */
  f.data[folded_at] = (nyr_inst_t){.op = NYIR_ADD_I64,
                                    .dst = folded,
                                    .a = address,
                                    .b = address,
                                    .alias_class = 17,
                                    .scalar_class = NYIR_SCALAR_F64,
                                    .semantic_ownership = NY_SEM_OWN_OWNED,
                                    .semantic_mutable = true};
  nir_make_f64_const(&f.data[folded_at], 0);
  ok = ok && f.data[folded_at].op == NYIR_CONST_F64 &&
       f.data[folded_at].alias_class == 0 &&
       f.data[folded_at].scalar_class == NYIR_SCALAR_F64 &&
       f.data[folded_at].semantic_ownership == NY_SEM_OWN_OWNED &&
       f.data[folded_at].semantic_mutable;

  f.data[folded_at] =
      (nyr_inst_t){.op = NYIR_ADD_I64,
                    .dst = folded,
                    .a = address,
                    .b = address,
                    .alias_class = 17,
                    .scalar_class = NYIR_SCALAR_F32,
                    .semantic_ownership = NY_SEM_OWN_BORROWED,
                    .semantic_mutable = true,
                    .debug = (nyr_debug_loc_t){
                        .file = "<f32-rewrite>", .line = 19, .column = 7}};
  nir_make_f32_const(&f.data[folded_at], 0);
  ok = ok && f.data[folded_at].op == NYIR_CONST_F32 &&
       f.data[folded_at].alias_class == 0 &&
       f.data[folded_at].scalar_class == NYIR_SCALAR_F32 &&
       f.data[folded_at].semantic_ownership == NY_SEM_OWN_BORROWED &&
       f.data[folded_at].semantic_mutable && f.data[folded_at].a == -1 &&
       f.data[folded_at].b == -1 && f.data[folded_at].c == -1 &&
       f.data[folded_at].d == -1 && f.data[folded_at].e == -1 &&
       f.data[folded_at].f == -1 && f.data[folded_at].debug.file &&
       strcmp(f.data[folded_at].debug.file, "<f32-rewrite>") == 0 &&
       f.data[folded_at].debug.line == 19 &&
       f.data[folded_at].debug.column == 7;
  if (!ok)
    fprintf(stderr, "[alias-contracts] instruction rewrite retained stale "
                    "alias provenance\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_private_global_promotion_invalidates_alias_classes(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                            .dst = -1,
                                            .a = -1,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .symbol = "private_global"});
  int loaded =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                  .dst = -1,
                                  .a = address,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .scalar_class = NYIR_SCALAR_I64_RAW});
  f.data[1].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = 0, .max = 0};
  int one = nyr_opt_const(&f, 1);
  int updated = nyr_opt_binop(&f, NYIR_ADD_I64, loaded, one);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = address,
                              .b = -1,
                              .c = updated,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  int f64_address =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .symbol = "private_f64_global"});
  int f64_initial = nyr_opt_const_f64(&f, 1.0);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = f64_address,
                              .b = -1,
                              .c = f64_initial,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .scalar_class = NYIR_SCALAR_F64});
  int f64_loaded =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                  .dst = -1,
                                  .a = f64_address,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .scalar_class = NYIR_SCALAR_F64});
  int f64_two = nyr_opt_const_f64(&f, 2.0);
  int f64_updated =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADD_F64,
                                  .dst = -1,
                                  .a = f64_loaded,
                                  .b = f64_two,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .scalar_class = NYIR_SCALAR_F64});
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = f64_address,
                              .b = -1,
                              .c = f64_updated,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .scalar_class = NYIR_SCALAR_F64});
  nyr_opt_ret(&f, updated);

  bool ok = nyr_alias_analysis(&f) && f.data[0].alias_class != 0 &&
            f.data[1].alias_class != 0 && f.data[4].alias_class != 0 &&
            nyr_promote_private_entry_globals(&f);
  bool saw_nop = false, saw_local_load = false, saw_local_store = false;
  bool saw_f64_global_load = false, saw_f64_global_store = false;
  for (size_t i = 0; i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->op == NYIR_NOP && in->dst < 0 && !in->symbol &&
        in->alias_class == 0)
      saw_nop = true;
    if (in->op == NYIR_LOAD_LOCAL) {
      saw_local_load = true;
      ok = ok && in->alias_class == 0 && !in->range.has_min &&
           !in->range.has_max;
      if (in->scalar_class == NYIR_SCALAR_F64)
        ok = false;
    }
    if (in->op == NYIR_STORE_LOCAL) {
      saw_local_store = true;
      ok = ok && in->alias_class == 0;
      if (in->scalar_class == NYIR_SCALAR_F64)
        ok = false;
    }
    if (in->op == NYIR_LOAD_I64 && in->scalar_class == NYIR_SCALAR_F64) {
      saw_f64_global_load = true;
      ok = ok && in->alias_class != 0;
    }
    if (in->op == NYIR_STORE_I64 && in->scalar_class == NYIR_SCALAR_F64) {
      saw_f64_global_store = true;
      ok = ok && in->alias_class != 0;
    }
  }
  ok = ok && saw_nop && saw_local_load && saw_local_store &&
       saw_f64_global_load && saw_f64_global_store;
  if (!ok)
    fprintf(stderr, "[alias-contracts] private-global promotion retained stale "
                    "alias facts\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_const_fold_invalidates_alias_classes(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int six = nyr_opt_const(&f, 6);
  int seven = nyr_opt_const(&f, 7);
  int sum = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADD_I64,
                                        .dst = -1,
                                        .a = six,
                                        .b = seven,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .alias_class = 41,
                                        .scalar_class = NYIR_SCALAR_I64_RAW,
                                        .semantic_ownership = NY_SEM_OWN_OWNED,
                                        .semantic_mutable = true});
  int fone = nyr_opt_const_f64(&f, 1.0);
  int ftwo = nyr_opt_const_f64(&f, 2.0);
  int fsum =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADD_F64,
                                  .dst = -1,
                                  .a = fone,
                                  .b = ftwo,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .alias_class = 42,
                                  .scalar_class = NYIR_SCALAR_F64,
                                  .semantic_ownership = NY_SEM_OWN_BORROWED,
                                  .semantic_mutable = true});
  nyr_opt_ret(&f, sum);

  bool ok =
      nyr_const_fold(&f) && f.data[(size_t)sum].op == NYIR_CONST_I64 &&
      f.data[(size_t)sum].imm == 13 && f.data[(size_t)sum].alias_class == 0 &&
      f.data[(size_t)sum].scalar_class == NYIR_SCALAR_I64_RAW &&
      f.data[(size_t)sum].semantic_ownership == NY_SEM_OWN_OWNED &&
      f.data[(size_t)sum].semantic_mutable &&
      f.data[(size_t)sum].range.has_min && f.data[(size_t)sum].range.has_max &&
      f.data[(size_t)sum].range.min == 13 &&
      f.data[(size_t)sum].range.max == 13 &&
      f.data[(size_t)fsum].op == NYIR_CONST_F64 &&
      f.data[(size_t)fsum].alias_class == 0 &&
      f.data[(size_t)fsum].scalar_class == NYIR_SCALAR_F64 &&
      f.data[(size_t)fsum].semantic_ownership == NY_SEM_OWN_BORROWED &&
      f.data[(size_t)fsum].semantic_mutable;
  if (!ok)
    fprintf(
        stderr,
        "[alias-contracts] constant folding retained stale alias metadata\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_cvp_invalidates_alias_classes(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int source = nyr_opt_const(&f, 19);
  int copy = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                         .dst = -1,
                                         .a = source,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .alias_class = 43,
                                         .scalar_class = NYIR_SCALAR_I64_RAW,
                                         .semantic_ownership = NY_SEM_OWN_OWNED,
                                         .semantic_mutable = true});
  nyr_opt_ret(&f, copy);

  bool ok = nyr_cvp(&f) && f.data[(size_t)copy].op == NYIR_CONST_I64 &&
            f.data[(size_t)copy].imm == 19 &&
            f.data[(size_t)copy].alias_class == 0 &&
            f.data[(size_t)copy].scalar_class == NYIR_SCALAR_I64_RAW &&
            f.data[(size_t)copy].semantic_ownership == NY_SEM_OWN_OWNED &&
            f.data[(size_t)copy].semantic_mutable &&
            f.data[(size_t)copy].range.has_min &&
            f.data[(size_t)copy].range.has_max &&
            f.data[(size_t)copy].range.min == 19 &&
            f.data[(size_t)copy].range.max == 19;
  if (!ok)
    fprintf(stderr,
            "[alias-contracts] CVP rewrite: op=%d imm=%" PRId64
            " alias=%u class=%d own=%u mut=%d range=%d:%" PRId64 "..%d:%" PRId64
            "\n",
            f.data[(size_t)copy].op, f.data[(size_t)copy].imm,
            f.data[(size_t)copy].alias_class, f.data[(size_t)copy].scalar_class,
            f.data[(size_t)copy].semantic_ownership,
            f.data[(size_t)copy].semantic_mutable,
            f.data[(size_t)copy].range.has_min, f.data[(size_t)copy].range.min,
            f.data[(size_t)copy].range.has_max, f.data[(size_t)copy].range.max);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_redundant_load_forwarding_invalidates_range(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int local_seed = nyr_opt_const(&f, 0);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = local_seed,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0});
  int address = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                            .dst = -1,
                                            .a = -1,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .symbol = "shared"});
  int first = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = address,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
  size_t second_at = f.len;
  nyr_ir_emit(
      &f,
      (nyr_inst_t){
          .op = NYIR_LOAD_I64,
          .dst = -1,
          .a = address,
          .b = -1,
          .c = -1,
          .d = -1,
          .e = -1,
          .f = -1,
          .alias_class = 91,
          .scalar_class = NYIR_SCALAR_I64_RAW,
          .range = {.has_min = true, .has_max = true, .min = 123, .max = 123}});
  bool ok =
      nyr_redundant_load_elim(&f) && f.data[second_at].op == NYIR_COPY &&
      f.data[second_at].a == first && f.data[second_at].alias_class == 0 &&
      !f.data[second_at].range.has_min && !f.data[second_at].range.has_max;
  if (!ok)
    fprintf(stderr,
            "[alias-contracts] redundant-load: op=%d src=%d expected=%d "
            "alias=%u range=%d:%" PRId64 "..%d:%" PRId64 "\n",
            f.data[second_at].op, f.data[second_at].a, first,
            f.data[second_at].alias_class, f.data[second_at].range.has_min,
            f.data[second_at].range.min, f.data[second_at].range.has_max,
            f.data[second_at].range.max);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_sroa_invalidates_address_alias_classes(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = 0,
                                  .scalar_class = NYIR_SCALAR_POINTER});
  int value = nyr_opt_const(&f, 7);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = address,
                              .b = -1,
                              .c = value,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  int loaded =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                  .dst = -1,
                                  .a = address,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .scalar_class = NYIR_SCALAR_I64_RAW});
  nyr_opt_ret(&f, loaded);
  bool alias_ok = nyr_alias_analysis(&f);
  /*
   * Model address facts produced by an earlier analysis before SROA changes
   * the memory operations into local-slot operations.
   */
  f.data[0].alias_class = 17;
  f.data[2].alias_class = 17;
  f.data[3].alias_class = 17;
  bool seed_ok =
      alias_ok && f.data[2].alias_class != 0 && f.data[3].alias_class != 0;
  bool pass_ok = nyr_sroa_scalar(&f);
  bool ok = alias_ok && seed_ok && pass_ok;
  bool stale_alias = false;
  bool retained_access = false;
  for (size_t i = 0; i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->op != NYIR_ADDR_LOCAL)
      stale_alias |= in->alias_class != 0;
    retained_access |= in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64;
  }
  ok = ok && !stale_alias && !retained_access;
  if (!ok) {
    fprintf(stderr,
            "[alias-contracts] SROA alias=%d seed=%d pass=%d stale=%d "
            "retained_access=%d len=%zu\n",
            alias_ok, seed_ok, pass_ok, stale_alias, retained_access, f.len);
    for (size_t i = 0; i < f.len; ++i)
      fprintf(stderr,
              "  i%zu %-16s alias=%u dst=%d a=%d b=%d c=%d imm=%" PRId64 "\n",
              i, nyr_op_name(f.data[i].op), f.data[i].alias_class,
              f.data[i].dst, f.data[i].a, f.data[i].b, f.data[i].c,
              f.data[i].imm);
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_phi_elim_invalidates_alias_class(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int condition = nyr_opt_const(&f, 1);
  int left = nyr_opt_const(&f, 9);
  int right = nyr_opt_const(&f, 17);
  nyr_opt_label(&f, 10);
  nyr_opt_br_if(&f, condition, 20);
  nyr_opt_label(&f, 30);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 20);
  nyr_opt_br(&f, 40);
  nyr_opt_label(&f, 40);
  nyr_opt_phi(&f, 20, left, 30, right);
  int result = f.next_value - 1;
  nyr_opt_ret(&f, result);
  size_t phi_at = f.len - 2;
  f.data[phi_at].alias_class = 73;
  f.data[phi_at].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = 9, .max = 17};
  f.data[phi_at].semantic_ownership = 1;
  f.data[phi_at].semantic_mutable = true;
  static const char phi_source[] = "phi-elim-origin.ny";
  f.data[phi_at].debug =
      (nyr_debug_loc_t){.file = phi_source, .line = 21, .column = 8};
  bool ok = nyr_opt_verify(&f, "alias-contracts/phi-before") &&
            nyr_phi_elim(&f) &&
            nyr_opt_verify(&f, "alias-contracts/phi-after");
  bool saw_result = false;
  size_t located_stores = 0;
  for (size_t i = 0; i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->op == NYIR_STORE_LOCAL && in->debug.file == phi_source &&
        in->debug.line == 21 && in->debug.column == 8)
      ++located_stores;
    if (in->op != NYIR_LOAD_LOCAL || in->dst != result)
      continue;
    saw_result = true;
    ok = ok && in->alias_class == 0 && in->semantic_ownership == 1 &&
         in->semantic_mutable && in->range.has_min && in->range.has_max &&
         in->range.min == 9 && in->range.max == 17 &&
         in->debug.file == phi_source && in->debug.line == 21 &&
         in->debug.column == 8;
  }
  ok = ok && saw_result && located_stores == 2;
  if (!ok) {
    fprintf(stderr,
            "[alias-contracts] PHI elimination retained stale metadata\n");
    nyr_dump(stderr, &f, "phi-elim-alias-contract");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_strength_reduce_preserves_result_facts(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int input = nyr_opt_const(&f, 23);
  int factor = nyr_opt_const(&f, 5);
  int result = nyr_opt_binop(&f, NYIR_MUL_I64, input, factor);
  nyr_opt_ret(&f, result);
  size_t result_at = f.len - 2;
  f.data[result_at].scalar_class = NYIR_SCALAR_I64_RAW;
  f.data[result_at].semantic_ownership = 1;
  f.data[result_at].semantic_mutable = true;
  f.data[result_at].alias_class = 91;
  f.data[result_at].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = 115, .max = 115};
  f.data[result_at].debug =
      (nyr_debug_loc_t){.file = "<strength-test>", .line = 7, .column = 11};
  bool ok = nyr_strength_reduce(&f) && nyr_opt_verify(&f, "strength-metadata");
  bool saw_result = false;
  for (size_t i = 0; i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->dst != result)
      continue;
    saw_result = true;
    ok = ok && in->op == NYIR_ADD_I64 && in->alias_class == 0 &&
         in->scalar_class == NYIR_SCALAR_I64_RAW &&
         in->semantic_ownership == 1 && in->semantic_mutable &&
         in->range.has_min && in->range.has_max && in->range.min == 115 &&
         in->range.max == 115 && in->debug.line == 7 && in->debug.column == 11;
  }
  ok = ok && saw_result;
  if (!ok) {
    fprintf(stderr, "[alias-contracts] strength reduction lost or retained "
                    "invalid result facts\n");
    nyr_dump(stderr, &f, "strength-reduce-result-facts");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_null_check_fold_preserves_result_facts(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int nonnull = nyr_opt_const(&f, 5);
  int zero = nyr_opt_const(&f, 0);
  int result = nyr_opt_cmp(&f, nonnull, zero, NYIR_CMP_NE);
  nyr_opt_ret(&f, result);
  size_t result_inst = f.len - 2;
  f.data[result_inst].scalar_class = NYIR_SCALAR_I64_RAW;
  f.data[result_inst].semantic_ownership = 1;
  f.data[result_inst].semantic_mutable = true;
  f.data[result_inst].alias_class = 91;
  f.data[result_inst].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = 0, .max = 1};
  f.data[result_inst].debug =
      (nyr_debug_loc_t){.file = "<null-check-test>", .line = 9, .column = 5};
  bool ok = nyr_opt_verify(&f, "null-check-result-facts") &&
            nyr_null_check_elim(&f) &&
            nyr_opt_verify(&f, "null-check-result-facts");
  bool saw_result = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->dst != result)
      continue;
    saw_result = true;
    ok = in->op == NYIR_CONST_I64 && in->imm == 1 && in->alias_class == 0 &&
         in->scalar_class == NYIR_SCALAR_I64_RAW &&
         in->semantic_ownership == 1 && in->semantic_mutable &&
         in->range.has_min && in->range.has_max && in->range.min == 1 &&
         in->range.max == 1 && in->debug.line == 9 && in->debug.column == 5;
  }
  ok = ok && saw_result;
  if (!ok) {
    fprintf(stderr, "[alias-contracts] null-check elimination lost or retained "
                    "invalid result facts\n");
    nyr_dump(stderr, &f, "null-check-result-facts");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_isle_rewrites_reconcile_derived_facts(void) {
  static const char debug_file[] = "<isle-facts>";
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .imm = 0,
                                  .scalar_class = NYIR_SCALAR_POINTER});
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  size_t add_zero_at = f.len;
  int add_zero = nyr_ir_emit(
      &f,
      (nyr_inst_t){.op = NYIR_ADD_I64,
                    .dst = -1,
                    .a = address,
                    .b = zero,
                    .c = -1,
                    .d = -1,
                    .e = -1,
                    .f = -1,
                    .scalar_class = NYIR_SCALAR_POINTER,
                    .semantic_ownership = NY_SEM_OWN_BORROWED,
                    .semantic_mutable = true,
                    .range = {.has_min = true,
                              .has_max = true,
                              .min = INT64_MIN,
                              .max = INT64_MAX},
                    .debug = {.file = debug_file, .line = 23, .column = 5}});
  size_t add_self_at = f.len;
  int add_self =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADD_I64,
                                  .dst = -1,
                                  .a = address,
                                  .b = address,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .scalar_class = NYIR_SCALAR_POINTER,
                                  .range = {.has_min = true,
                                            .has_max = true,
                                            .min = INT64_MIN,
                                            .max = INT64_MAX}});
  nyr_opt_ret(&f, zero);
  bool ok = address >= 0 && zero >= 0 && one >= 0 && add_zero >= 0 &&
            add_self >= 0 && nyr_opt_verify(&f, "isle-facts-before") &&
            nyr_alias_analysis(&f) && f.data[add_zero_at].alias_class != 0 &&
            f.data[add_self_at].alias_class != 0 && ny_isle_apply_nir(&f) &&
            nyr_opt_verify(&f, "isle-facts-after");
  if (ok) {
    const nyr_inst_t *copy = &f.data[add_zero_at];
    const nyr_inst_t *shift = &f.data[add_self_at];
    ok = copy->op == NYIR_COPY && copy->alias_class == 0 &&
         copy->scalar_class == NYIR_SCALAR_POINTER &&
         copy->semantic_ownership == NY_SEM_OWN_BORROWED &&
         copy->semantic_mutable && !copy->range.has_min &&
         !copy->range.has_max && copy->debug.file == debug_file &&
         copy->debug.line == 23 && copy->debug.column == 5 &&
         shift->op == NYIR_SHL_I64 && shift->alias_class == 0 &&
         !shift->range.has_min && !shift->range.has_max;
  }
  if (!ok) {
    fprintf(stderr, "[alias-contracts] ISLE rewrite lost semantic metadata "
                    "or retained derived facts\n");
    nyr_dump(stderr, &f, "isle-facts");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_double_neg_rewrites_reconcile_derived_facts(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0);
  int minus_one = nyr_opt_const(&f, -1);
  int input = nyr_opt_const(&f, 42);
  int xor_inner = nyr_opt_binop(&f, NYIR_XOR_I64, input, minus_one);
  size_t xor_outer_at = f.len;
  int xor_outer = nyr_opt_binop(&f, NYIR_XOR_I64, xor_inner, minus_one);
  int neg_inner = nyr_opt_binop(&f, NYIR_SUB_I64, zero, input);
  size_t neg_outer_at = f.len;
  int neg_outer = nyr_opt_binop(&f, NYIR_SUB_I64, zero, neg_inner);
  nyr_opt_ret(&f, xor_outer);
  const size_t rewritten[] = {xor_outer_at, neg_outer_at};
  for (size_t k = 0; k < sizeof(rewritten) / sizeof(rewritten[0]); ++k) {
    nyr_inst_t *in = &f.data[rewritten[k]];
    in->scalar_class = NYIR_SCALAR_I64_RAW;
    in->semantic_ownership = NY_SEM_OWN_OWNED;
    in->semantic_mutable = true;
    in->alias_class = 91;
    in->range =
        (nyr_range_t){.has_min = true, .has_max = true, .min = 42, .max = 42};
  }
  bool ok = xor_outer >= 0 && neg_outer >= 0 &&
            nyr_opt_verify(&f, "double-neg-facts-before") &&
            nyr_opt_eval(&f, "double-neg-facts-before") == 42 &&
            nyr_double_neg(&f) &&
            nyr_opt_verify(&f, "double-neg-facts-after") &&
            nyr_opt_eval(&f, "double-neg-facts-after") == 42;
  for (size_t k = 0; ok && k < sizeof(rewritten) / sizeof(rewritten[0]); ++k) {
    const nyr_inst_t *in = &f.data[rewritten[k]];
    ok = in->op == NYIR_COPY && in->alias_class == 0 && !in->range.has_min &&
         !in->range.has_max && in->scalar_class == NYIR_SCALAR_I64_RAW &&
         in->semantic_ownership == NY_SEM_OWN_OWNED && in->semantic_mutable;
  }
  if (!ok) {
    fprintf(stderr, "[alias-contracts] double-neg rewrite retained stale facts "
                    "or lost semantic metadata\n");
    nyr_dump(stderr, &f, "double-neg-facts");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_phi_pruning_reconciles_derived_facts(void) {
  static const char debug_file[] = "<phi-prune-facts>";
  nyr_func_t f;
  nyr_opt_begin(&f);
  nyr_opt_label(&f, 1);
  int value = nyr_opt_const(&f, 42);
  nyr_opt_br(&f, 2);
  nyr_opt_label(&f, 2);
  size_t phi_at = f.len;
  nyr_opt_phi(&f, 1, value, 99, value);
  int result = f.data[phi_at].dst;
  f.data[phi_at].scalar_class = NYIR_SCALAR_I64_RAW;
  f.data[phi_at].semantic_ownership = NY_SEM_OWN_OWNED;
  f.data[phi_at].semantic_mutable = true;
  f.data[phi_at].alias_class = 91;
  f.data[phi_at].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = 42, .max = 42};
  f.data[phi_at].debug =
      (nyr_debug_loc_t){.file = debug_file, .line = 37, .column = 3};
  nyr_opt_ret(&f, result);

  bool ok = value >= 0 && result >= 0 && nyr_prune_phis(&f) &&
            nyr_opt_verify(&f, "phi-prune-facts-after") &&
            nyr_opt_eval(&f, "phi-prune-facts-after") == 42;
  bool found = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->dst != result)
      continue;
    found = true;
    ok = in->op == NYIR_COPY && in->a == value && in->alias_class == 0 &&
         !in->range.has_min && !in->range.has_max &&
         in->scalar_class == NYIR_SCALAR_I64_RAW &&
         in->semantic_ownership == NY_SEM_OWN_OWNED && in->semantic_mutable &&
         in->debug.file == debug_file && in->debug.line == 37 &&
         in->debug.column == 3;
  }
  ok = ok && found;
  if (!ok) {
    fprintf(stderr, "[alias-contracts] PHI pruning lost semantic metadata or "
                    "retained derived facts\n");
    nyr_dump(stderr, &f, "phi-prune-facts");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_inline_constant_rewrite_preserves_facts(void) {
  static const char callee_name[] = "alias_contracts_constant_callee";
  static const char debug_file[] = "<inline-const-facts>";
  nyr_func_t callee;
  nyr_opt_begin(&callee);
  int constant = nyr_opt_const(&callee, 42);
  nyr_opt_ret(&callee, constant);

  nyr_func_t caller;
  nyr_opt_begin(&caller);
  size_t call_at = caller.len;
  int result = nyr_ir_emit(&caller, (nyr_inst_t){.op = NYIR_CALL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1,
                                                .imm = 0,
                                                .symbol = callee_name});
  nyr_opt_ret(&caller, result);
  caller.data[call_at].scalar_class = NYIR_SCALAR_I64_RAW;
  caller.data[call_at].semantic_ownership = NY_SEM_OWN_BORROWED;
  caller.data[call_at].semantic_mutable = true;
  caller.data[call_at].alias_class = 91;
  caller.data[call_at].range = (nyr_range_t){
      .has_min = true, .has_max = true, .min = INT64_MIN, .max = INT64_MAX};
  caller.data[call_at].debug =
      (nyr_debug_loc_t){.file = debug_file, .line = 43, .column = 7};

  nyr_inline_callee_t entry = {.name = callee_name, .func = &callee};
  nyr_set_inline_callees(&entry, 1);
  bool inlined = nyr_inline_small(&caller);
  nyr_set_inline_callees(NULL, 0);
  bool ok = result >= 0 && inlined &&
            nyr_opt_verify(&caller, "inline-const-facts-after") &&
            nyr_opt_eval(&caller, "inline-const-facts-after") == 42;
  const nyr_inst_t *folded = NULL;
  for (size_t i = 0; ok && i < caller.len; ++i)
    if (caller.data[i].dst == result)
      folded = &caller.data[i];
  ok = ok && folded && folded->op == NYIR_CONST_I64 && folded->imm == 42 &&
       folded->alias_class == 0 && folded->range.has_min &&
       folded->range.has_max && folded->range.min == 42 &&
       folded->range.max == 42 && folded->scalar_class == NYIR_SCALAR_I64_RAW &&
       folded->semantic_ownership == NY_SEM_OWN_BORROWED &&
       folded->semantic_mutable && folded->debug.file == debug_file &&
       folded->debug.line == 43 && folded->debug.column == 7;
  if (!ok) {
    fprintf(stderr, "[alias-contracts] inlined constant lost semantic "
                    "metadata or retained stale facts\n");
    nyr_dump(stderr, &caller, "inline-const-facts");
  }
  nyr_ir_func_free(&caller);
  nyr_ir_func_free(&callee);
  return ok;
}

static bool check_local_load_rewrite(const char *name,
                                     bool (*rewrite)(nyr_func_t *)) {
  static const char debug_file[] = "<local-load-facts>";
  char before[96];
  char after[96];
  snprintf(before, sizeof(before), "%s-before", name);
  snprintf(after, sizeof(after), "%s-after", name);
  nyr_func_t f;
  nyr_opt_begin(&f);
  int value = nyr_opt_const(&f, 42);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = value,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 0});
  size_t load_at = f.len;
  int loaded = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                           .dst = -1,
                                           .a = -1,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = 0});
  f.data[load_at].scalar_class = NYIR_SCALAR_I64_RAW;
  f.data[load_at].semantic_ownership = NY_SEM_OWN_OWNED;
  f.data[load_at].semantic_mutable = true;
  f.data[load_at].alias_class = 91;
  f.data[load_at].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = 42, .max = 42};
  f.data[load_at].debug =
      (nyr_debug_loc_t){.file = debug_file, .line = 31, .column = 9};
  nyr_opt_ret(&f, loaded);

  bool ok = value >= 0 && loaded >= 0 && nyr_opt_verify(&f, before) &&
            nyr_opt_eval(&f, before) == 42 && rewrite(&f) &&
            nyr_opt_verify(&f, after) && nyr_opt_eval(&f, after) == 42;
  bool found = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->dst != loaded)
      continue;
    found = true;
    ok = in->op == NYIR_COPY && in->alias_class == 0 && !in->range.has_min &&
         !in->range.has_max && in->scalar_class == NYIR_SCALAR_I64_RAW &&
         in->semantic_ownership == NY_SEM_OWN_OWNED && in->semantic_mutable &&
         in->debug.file == debug_file && in->debug.line == 31 &&
         in->debug.column == 9;
  }
  ok = ok && found;
  if (!ok) {
    fprintf(stderr,
            "[alias-contracts] %s retained stale derived facts "
            "or lost semantic metadata\n",
            name);
    nyr_dump(stderr, &f, name);
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_memory_ssa_forward_reconciles_derived_facts(void) {
  return check_local_load_rewrite("memory-ssa-facts", nyr_memory_ssa_forward);
}

static bool run_mem2reg_rewrites_reconcile_derived_facts(void) {
  return check_local_load_rewrite("mem2reg-facts", nyr_mem2reg);
}

int main(void) {
  bool ok = run_memory_address_classes() &&
            run_recomputed_facts_clear_stale_classes() &&
            run_rewrite_clears_derived_alias_class() &&
            run_private_global_promotion_invalidates_alias_classes() &&
            run_const_fold_invalidates_alias_classes() &&
            run_cvp_invalidates_alias_classes() &&
            run_redundant_load_forwarding_invalidates_range() &&
            run_sroa_invalidates_address_alias_classes() &&
            run_phi_elim_invalidates_alias_class() &&
            run_strength_reduce_preserves_result_facts() &&
            run_null_check_fold_preserves_result_facts() &&
            run_isle_rewrites_reconcile_derived_facts() &&
            run_double_neg_rewrites_reconcile_derived_facts() &&
            run_phi_pruning_reconciles_derived_facts() &&
            run_inline_constant_rewrite_preserves_facts() &&
            run_memory_ssa_forward_reconciles_derived_facts() &&
            run_mem2reg_rewrites_reconcile_derived_facts() &&
            run_derived_local_address_escape(NYIR_COPY, 0) &&
            run_derived_local_address_escape(NYIR_ADD_I64, 0) &&
            run_derived_local_address_escape(NYIR_SUB_I64, 0) &&
            run_derived_local_address_escape(NYIR_COPY, 7) &&
            run_derived_local_address_escape(NYIR_ADD_I64, 7) &&
            run_derived_local_address_escape(NYIR_SUB_I64, 7) &&
            run_local_address_escape_through_sink(NYIR_ADDR_LOCAL, false) &&
            run_local_address_escape_through_sink(NYIR_COPY, false) &&
            run_local_address_escape_through_sink(NYIR_ADD_I64, false) &&
            run_local_address_escape_through_sink(NYIR_SUB_I64, false) &&
            run_local_address_escape_through_sink(NYIR_ADDR_LOCAL, true) &&
            run_local_address_escape_through_sink(NYIR_COPY, true) &&
            run_local_address_escape_through_sink(NYIR_ADD_I64, true) &&
            run_local_address_escape_through_sink(NYIR_SUB_I64, true);
  for (size_t arg = 0; arg <= 7; ++arg)
    ok = run_local_address_escape(arg) && ok;
  return ok ? 0 : 1;
}
