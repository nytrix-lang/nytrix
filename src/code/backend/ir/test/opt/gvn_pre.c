#include "code/lang/parse/ast.h"
#include "support.h"

#include <inttypes.h>
#include <stdint.h>

extern bool nyr_globaltab_add(const char *name);

static int64_t gvn_pre_cell = 41;
static int64_t gvn_pre_counter = 0;

static int emit_load(nyr_func_t *f, int address) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                    .dst = -1,
                                    .a = address,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1});
}

static int emit_load_local(nyr_func_t *f, int slot) {
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

static void emit_store(nyr_func_t *f, int address, int value) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = value,
                             .d = -1,
                             .e = -1,
                             .f = -1});
}

static int emit_symbol_address(nyr_func_t *f, const char *name) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .symbol = name});
}

static int emit_address_offset(nyr_func_t *f, int address, int64_t offset) {
  if (offset == 0)
    return address;
  int amount = nyr_opt_const(f, offset);
  if (amount < 0)
    return -1;
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ADD_I64,
                                    .dst = -1,
                                    .a = address,
                                    .b = amount,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1});
}

static void emit_local_store(nyr_func_t *f, int slot, int value) {
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

static void emit_unknown_call(nyr_func_t *f) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                             .dst = -1,
                             .a = -1,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 0,
                             .symbol = "unknown_memory_effect"});
}

static void build_diamond(nyr_func_t *f, bool store, bool unknown_call) {
  nyr_opt_begin(f);
  int address = nyr_opt_const(f, (int64_t)(intptr_t)&gvn_pre_cell);
  int false_value = nyr_opt_const(f, 0);
  int replacement = nyr_opt_const(f, 99);
  nyr_opt_br_if(f, false_value, 10);

  nyr_opt_label(f, 20);
  emit_load(f, address);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 10);
  emit_load(f, address);
  if (store)
    emit_store(f, address, replacement);
  if (unknown_call)
    emit_unknown_call(f);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 30);
  int result = emit_load(f, address);
  nyr_opt_ret(f, result);
}

static void count_memory_ops(const nyr_func_t *f, int *loads, int *phis) {
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
  nyr_func_t f;
  build_diamond(&f, store, unknown_call);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long before = nyr_opt_eval(&f, name);
  if (!nyr_gvn_pre(&f) || !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long after = nyr_opt_eval(&f, name);
  int loads, phis;
  count_memory_ops(&f, &loads, &phis);
  if (loads != want_loads || phis != want_phis) {
    fprintf(stderr, "[%s] loads=%d phis=%d want loads=%d phis=%d\n", name,
            loads, phis, want_loads, want_phis);
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_ir_func_free(&f);
  nyr_opt_check(name, before, after, 41);
  return true;
}

static bool run_local_store_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = nyr_opt_const(&f, (int64_t)(intptr_t)&gvn_pre_cell);
  int replacement = nyr_opt_const(&f, 99);
  int first = emit_load(&f, address);
  emit_local_store(&f, 0, replacement);
  int second = emit_load(&f, address);
  nyr_opt_ret(&f, second);
  if (!nyr_opt_verify(&f, "gvn-pre-local-store")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long before = nyr_opt_eval(&f, "gvn-pre-local-store");
  if (!nyr_gvn_pre(&f) || !nyr_opt_verify(&f, "gvn-pre-local-store")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long after = nyr_opt_eval(&f, "gvn-pre-local-store");
  int loads, phis;
  count_memory_ops(&f, &loads, &phis);
  bool ok = loads == 1 && first >= 0 && phis == 0 &&
            before == (long long)gvn_pre_cell && after == before;
  if (!ok)
    fprintf(stderr, "[gvn-pre-local-store] loads=%d before=%lld after=%lld\n",
            loads, before, after);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_unrelated_local_store_case(void) {
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
                                            .symbol = "gvn_pre_global_cell"});
  address = nyr_opt_binop(&f, NYIR_ADD_I64, address, nyr_opt_const(&f, 0));
  int first = emit_load(&f, address);
  emit_local_store(&f, 0, nyr_opt_const(&f, 99));
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                              .dst = -1,
                              .a = -1,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .imm = 1});
  int second = emit_load(&f, address);
  nyr_opt_ret(&f, second);
  bool ok = nyr_globaltab_add("gvn_pre_global_cell") &&
            nyr_opt_verify(&f, "gvn-pre-unrelated-local-store");
  long long before = ok ? nyr_opt_eval(&f, "gvn-pre-unrelated-local-store") : 0;
  ok = ok && nyr_gvn_pre(&f) &&
       nyr_opt_verify(&f, "gvn-pre-unrelated-local-store") &&
       nyr_opt_eval(&f, "gvn-pre-unrelated-local-store") == before;
  int loads = 0, phis = 0;
  count_memory_ops(&f, &loads, &phis);
  if (ok && (first < 0 || second < 0 || loads != 1 || phis != 0 ||
             f.data[f.len - 2].op != NYIR_COPY || f.data[f.len - 2].a != first))
    ok = false;
  if (!ok) {
    fprintf(stderr, "[gvn-pre-unrelated-local-store] loads=%d before=%lld\n",
            loads, before);
    nyr_dump(stderr, &f, "gvn-pre-unrelated-local-store");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_unknown_offset_store_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int read_address = emit_symbol_address(&f, "gvn_pre_global_cell");
  int write_base = emit_symbol_address(&f, "gvn_pre_other_cell");
  int offset = nyr_opt_const(&f, 8);
  int write_address = nyr_opt_binop(&f, NYIR_ADD_I64, write_base, offset);
  int first = emit_load(&f, read_address);
  int value = nyr_opt_const(&f, 99);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = write_address,
                              .b = -1,
                              .c = value,
                              .d = -1,
                              .e = -1,
                              .f = -1});
  int second = emit_load(&f, read_address);
  nyr_opt_ret(&f, second);
  bool ok = read_address >= 0 && write_base >= 0 && offset >= 0 &&
            write_address >= 0 && first >= 0 && value >= 0 && second >= 0 &&
            nyr_opt_verify(&f, "gvn-pre-unknown-offset-store/before") &&
            nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, "gvn-pre-unknown-offset-store/after");
  int loads = 0, phis = 0;
  count_memory_ops(&f, &loads, &phis);
  ok = ok && loads == 2 && phis == 0;
  if (!ok) {
    fprintf(stderr, "[gvn-pre-unknown-offset-store] loads=%d\n", loads);
    nyr_dump(stderr, &f, "gvn-pre-unknown-offset-store");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_same_root_offset_store_case(int64_t read_offset,
                                            int64_t write_offset,
                                            int expected_loads,
                                            const char *case_name) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int base = emit_symbol_address(&f, "gvn_pre_offset_global");
  int read_address = base;
  int write_address = base;
  if (read_offset != 0) {
    int offset = nyr_opt_const(&f, read_offset);
    read_address =
        offset < 0 ? -1 : nyr_opt_binop(&f, NYIR_ADD_I64, base, offset);
  }
  if (write_offset != 0) {
    int offset = nyr_opt_const(&f, write_offset);
    write_address =
        offset < 0 ? -1 : nyr_opt_binop(&f, NYIR_ADD_I64, base, offset);
  }
  int first = read_address < 0 ? -1 : emit_load(&f, read_address);
  int replacement = nyr_opt_const(&f, 99);
  if (write_address >= 0 && replacement >= 0)
    emit_store(&f, write_address, replacement);
  int second = read_address < 0 ? -1 : emit_load(&f, read_address);
  if (second >= 0)
    nyr_opt_ret(&f, second);
  bool ok = base >= 0 && first >= 0 && replacement >= 0 && second >= 0 &&
            nyr_globaltab_add("gvn_pre_offset_global") &&
            nyr_opt_verify(&f, case_name) && nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, case_name);
  int loads = 0, phis = 0;
  if (ok) {
    count_memory_ops(&f, &loads, &phis);
    ok = loads == expected_loads && phis == 0;
  }
  if (!ok) {
    fprintf(stderr,
            "[%s] read_offset=%" PRId64 " write_offset=%" PRId64
            " loads=%d expected=%d\n",
            case_name, read_offset, write_offset, loads, expected_loads);
    nyr_dump(stderr, &f, case_name);
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_post_call_load_cse_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = nyr_opt_const(&f, (int64_t)(intptr_t)&gvn_pre_cell);
  int before_call = emit_load(&f, address);
  emit_unknown_call(&f);
  int after_call = emit_load(&f, address);
  int repeated_after_call = emit_load(&f, address);
  nyr_opt_ret(&f, repeated_after_call);
  if (!nyr_opt_verify(&f, "gvn-pre-post-call-load-cse")) {
    fprintf(stderr, "[gvn-pre-post-call-load-cse] initial verify failed\n");
    nyr_ir_func_free(&f);
    return false;
  }
  if (!nyr_gvn_pre(&f) || !nyr_opt_verify(&f, "gvn-pre-post-call-load-cse")) {
    fprintf(stderr, "[gvn-pre-post-call-load-cse] optimized verify failed\n");
    nyr_dump(stderr, &f, "gvn-pre-post-call-load-cse");
    nyr_ir_func_free(&f);
    return false;
  }
  int loads, phis;
  count_memory_ops(&f, &loads, &phis);
  bool ok = before_call >= 0 && after_call >= 0 && loads == 2 && phis == 0 &&
            f.data[f.len - 2].op == NYIR_COPY &&
            f.data[f.len - 2].a == after_call;
  if (!ok)
    fprintf(stderr, "[gvn-pre-post-call-load-cse] loads=%d phis=%d\n", loads,
            phis);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_address_taken_local_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int replacement = nyr_opt_const(&f, 99);
  int replacement2 = nyr_opt_const(&f, 100);
  int local_address = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                                  .dst = -1,
                                                  .a = -1,
                                                  .b = -1,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1,
                                                  .imm = 0});
  emit_local_store(&f, 0, replacement);
  int first = emit_load(&f, local_address);
  emit_local_store(&f, 0, replacement2);
  int second = emit_load(&f, local_address);
  nyr_opt_ret(&f, second);
  if (!nyr_opt_verify(&f, "gvn-pre-address-taken-local")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long before = nyr_opt_eval(&f, "gvn-pre-address-taken-local");
  if (!nyr_gvn_pre(&f) || !nyr_opt_verify(&f, "gvn-pre-address-taken-local")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long after = nyr_opt_eval(&f, "gvn-pre-address-taken-local");
  int loads, phis;
  count_memory_ops(&f, &loads, &phis);
  bool ok = first >= 0 && loads == 2 && before == 100 && after == before;
  if (!ok)
    fprintf(stderr,
            "[gvn-pre-address-taken-local] loads=%d before=%lld after=%lld\n",
            loads, before, after);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_forwarded_pointer_provenance_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = nyr_opt_const(&f, (int64_t)(intptr_t)&gvn_pre_cell);
  int first = emit_load(&f, address);
  int second = emit_load(&f, address);
  int result = second;
  nyr_opt_ret(&f, result);
  size_t first_inst = f.len, second_inst = f.len;
  for (size_t i = 0; i < f.len; ++i) {
    if (f.data[i].dst == first)
      first_inst = i;
    if (f.data[i].dst == second)
      second_inst = i;
  }
  bool ok = first_inst < f.len && second_inst < f.len;
  if (ok) {
    f.data[first_inst].alias_class = 17;
    f.data[second_inst].alias_class = 29;
    f.data[second_inst].scalar_class = NYIR_SCALAR_I64_TAGGED;
    f.data[second_inst].semantic_ownership = 1;
    f.data[second_inst].semantic_mutable = true;
    f.data[second_inst].range =
        (nyr_range_t){.has_min = true, .has_max = true, .min = 41, .max = 41};
    f.data[second_inst].debug =
        (nyr_debug_loc_t){.file = "<gvn-pre-test>", .line = 8, .column = 13};
  }
  ok = ok && nyr_opt_verify(&f, "gvn-pre-pointer-provenance") &&
       nyr_gvn_pre(&f) && nyr_opt_verify(&f, "gvn-pre-pointer-provenance");
  bool saw_copy = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->dst != result)
      continue;
    saw_copy = true;
    ok = in->op == NYIR_COPY && in->a == first && in->alias_class == 17 &&
         in->scalar_class == NYIR_SCALAR_I64_TAGGED &&
         in->semantic_ownership == 1 && in->semantic_mutable &&
         in->range.has_min && in->range.has_max && in->range.min == 41 &&
         in->range.max == 41 && in->debug.line == 8 && in->debug.column == 13;
  }
  ok = ok && saw_copy;
  if (!ok) {
    fprintf(
        stderr,
        "[gvn-pre] forwarded pointer lost source provenance or result facts\n");
    nyr_dump(stderr, &f, "gvn-pre-pointer-provenance");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_loop_local_load_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = nyr_opt_const(&f, (int64_t)(intptr_t)&gvn_pre_cell);
  int counter_address = nyr_opt_const(&f, (int64_t)(intptr_t)&gvn_pre_counter);
  int replacement = nyr_opt_const(&f, 99);
  int one = nyr_opt_const(&f, 1);
  int zero = nyr_opt_const(&f, 0);
  emit_store(&f, counter_address, one);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  int first = emit_load(&f, address);
  int counter = emit_load(&f, counter_address);
  int more = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CMP_I64,
                                         .dst = -1,
                                         .a = counter,
                                         .b = zero,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .cmp = NYIR_CMP_GT});
  nyr_opt_br_if(&f, more, 20);
  nyr_opt_label(&f, 30);
  int second = emit_load(&f, address);
  nyr_opt_ret(&f, second);
  nyr_opt_label(&f, 20);
  emit_store(&f, address, replacement);
  int next = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_SUB_I64,
                                         .dst = -1,
                                         .a = counter,
                                         .b = one,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1});
  emit_store(&f, counter_address, next);
  nyr_opt_br(&f, 10);
  /*
   * One real backedge executes before the final header load and this use.
   */
  if (!nyr_opt_verify(&f, "gvn-pre-loop-local-load")) {
    nyr_ir_func_free(&f);
    return false;
  }
  int64_t initial_cell = gvn_pre_cell;
  long long before = nyr_opt_eval(&f, "gvn-pre-loop-local-load");
  bool ok = first >= 0 && second >= 0 && nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, "gvn-pre-loop-local-load");
  gvn_pre_cell = initial_cell;
  long long after = ok ? nyr_opt_eval(&f, "gvn-pre-loop-local-load") : 0;
  int loads, phis;
  count_memory_ops(&f, &loads, &phis);
  ok = ok && loads == 2 && phis == 0 && before == after && after == 99;
  if (!ok)
    fprintf(stderr,
            "[gvn-pre-loop-local-load] loads=%d before=%lld after=%lld\n",
            loads, before, after);
  if (!ok)
    nyr_dump(stderr, &f, "gvn-pre-loop-local-load");
  gvn_pre_cell = initial_cell;
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_loop_reexecuted_load_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = emit_symbol_address(&f, "gvn_pre_loop_stable_global");
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int limit = nyr_opt_const(&f, 3);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  int counter = f.next_value;
  nyr_opt_phi(&f, -1, zero, 0, zero);
  int loaded = emit_load(&f, address);
  int more = nyr_opt_cmp(&f, counter, limit, NYIR_CMP_LT);
  nyr_opt_br_if(&f, more, 20);
  nyr_opt_label(&f, 30);
  int after_loop = emit_load(&f, address);
  nyr_opt_ret(&f, after_loop);
  nyr_opt_label(&f, 20);
  int next = nyr_opt_binop(&f, NYIR_ADD_I64, counter, one);
  nyr_opt_br(&f, 10);
  for (size_t i = 0; i < f.len; ++i) {
    if (f.data[i].op == NYIR_PHI) {
      f.data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 20, .value = next};
      break;
    }
  }
  const char *name = "gvn-pre-loop-reexecuted-stable-load";
  bool ok = nyr_globaltab_add("gvn_pre_loop_stable_global") &&
            nyr_opt_verify(&f, name);
  long long before = ok ? nyr_opt_eval(&f, name) : 0;
  ok = ok && nyr_gvn_pre(&f) && nyr_opt_verify(&f, name);
  long long after = ok ? nyr_opt_eval(&f, name) : 0;
  int loads = 0, phis = 0;
  count_memory_ops(&f, &loads, &phis);
  ok = ok && loaded >= 0 && after_loop >= 0 && loads == 1 && phis == 1 &&
       before == after;
  if (!ok) {
    fprintf(stderr, "[%s] loads=%d phis=%d before=%lld after=%lld\n", name,
            loads, phis, before, after);
    nyr_dump(stderr, &f, name);
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_loop_header_write_before_reentry_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = nyr_opt_const(&f, (int64_t)(intptr_t)&gvn_pre_cell);
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  emit_store(&f, address, zero);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  int loop_load = emit_load(&f, address);
  int empty = nyr_opt_cmp(&f, loop_load, zero, NYIR_CMP_EQ);
  nyr_opt_br_if(&f, empty, 20);
  nyr_opt_br(&f, 30);
  nyr_opt_label(&f, 20);
  emit_store(&f, address, one);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 30);
  int result = emit_load(&f, address);
  nyr_opt_ret(&f, result);

  const char *name = "gvn-pre-loop-header-write-before-reentry";
  int64_t saved = gvn_pre_cell;
  bool ok = loop_load >= 0 && result >= 0 && nyr_opt_verify(&f, name);
  long long before = ok ? nyr_opt_eval(&f, name) : 0;
  ok = ok && nyr_gvn_pre(&f) && nyr_opt_verify(&f, name);
  long long after = ok ? nyr_opt_eval(&f, name) : 0;
  bool loop_load_preserved = false;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].dst == loop_load)
      loop_load_preserved = f.data[i].op == NYIR_LOAD_I64;
  ok = ok && before == 1 && after == before && loop_load_preserved;
  if (!ok) {
    fprintf(stderr, "[%s] before=%lld after=%lld loop-load-preserved=%s\n",
            name, before, after, loop_load_preserved ? "yes" : "no");
    nyr_dump(stderr, &f, name);
  }
  gvn_pre_cell = saved;
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_disjoint_global_store_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int read_address = emit_symbol_address(&f, "gvn_pre_read_global");
  int write_address = emit_symbol_address(&f, "gvn_pre_other_global");
  int replacement = nyr_opt_const(&f, 99);
  int first = emit_load(&f, read_address);
  emit_store(&f, write_address, replacement);
  int second = emit_load(&f, read_address);
  nyr_opt_ret(&f, second);
  const char *name = "gvn-pre-disjoint-global-store";
  bool ok = nyr_globaltab_add("gvn_pre_read_global") &&
            nyr_globaltab_add("gvn_pre_other_global") &&
            nyr_opt_verify(&f, name);
  long long before = ok ? nyr_opt_eval(&f, name) : 0;
  ok = ok && nyr_gvn_pre(&f) && nyr_opt_verify(&f, name);
  long long after = ok ? nyr_opt_eval(&f, name) : 0;
  int loads = 0, phis = 0;
  count_memory_ops(&f, &loads, &phis);
  ok = ok && first >= 0 && second >= 0 && loads == 1 && phis == 0 &&
       before == after;
  if (!ok) {
    fprintf(stderr, "[%s] loads=%d before=%lld after=%lld\n", name, loads,
            before, after);
    nyr_dump(stderr, &f, name);
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_copy_operand_cse_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int left = nyr_opt_const(&f, 17);
  int right = nyr_opt_const(&f, 25);
  int copy_a = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                           .dst = -1,
                                           .a = left,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  int first = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADD_I64,
                                          .dst = -1,
                                          .a = copy_a,
                                          .b = right,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  int copy_b = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                           .dst = -1,
                                           .a = left,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  int second = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADD_I64,
                                           .dst = -1,
                                           .a = copy_b,
                                           .b = right,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  nyr_opt_ret(&f, second);
  if (!nyr_opt_verify(&f, "gvn-pre-copy-operands")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long before = nyr_opt_eval(&f, "gvn-pre-copy-operands");
  bool ok = first >= 0 && second >= 0 && nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, "gvn-pre-copy-operands");
  long long after = ok ? nyr_opt_eval(&f, "gvn-pre-copy-operands") : 0;
  size_t adds = 0;
  for (size_t i = 0; i < f.len; ++i)
    adds += f.data[i].op == NYIR_ADD_I64;
  ok = ok && adds == 1 && before == after && after == 42;
  if (!ok)
    fprintf(stderr, "[gvn-pre-copy-operands] adds=%zu before=%lld after=%lld\n",
            adds, before, after);
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_rewritten_copy_chain_load_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int base = nyr_opt_const(&f, (int64_t)(intptr_t)&gvn_pre_cell);
  int zero = nyr_opt_const(&f, 0);
  int address_a = nyr_opt_binop(&f, NYIR_ADD_I64, base, zero);
  int address_b = nyr_opt_binop(&f, NYIR_ADD_I64, base, zero);
  int first = emit_load(&f, address_a);
  int second = emit_load(&f, address_b);
  nyr_opt_ret(&f, second);
  bool ok = address_a >= 0 && address_b >= 0 && first >= 0 && second >= 0 &&
            nyr_opt_verify(&f, "gvn-pre-rewritten-copy-chain/before");
  long long before = ok ? nyr_opt_eval(&f, "gvn-pre-rewritten-copy-chain") : 0;
  ok = ok && nyr_gvn_pre(&f) &&
       nyr_opt_verify(&f, "gvn-pre-rewritten-copy-chain/after");
  long long after = ok ? nyr_opt_eval(&f, "gvn-pre-rewritten-copy-chain") : 0;
  int adds = 0, loads = 0, phis = 0;
  count_memory_ops(&f, &loads, &phis);
  for (size_t i = 0; i < f.len; ++i)
    adds += f.data[i].op == NYIR_ADD_I64;
  ok = ok && adds == 1 && loads == 1 && phis == 0 &&
       f.data[3].op == NYIR_COPY && f.data[3].a == address_a &&
       f.data[5].op == NYIR_COPY && f.data[5].a == first && before == after &&
       after == gvn_pre_cell;
  if (!ok) {
    fprintf(stderr,
            "[gvn-pre-rewritten-copy-chain] adds=%d loads=%d before=%lld "
            "after=%lld\n",
            adds, loads, before, after);
    nyr_dump(stderr, &f, "gvn-pre-rewritten-copy-chain");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_local_load_cse_case(bool same_slot_store, bool other_slot_store,
                                    bool unknown_call, int want_loads) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int initial = nyr_opt_const(&f, 41);
  int replacement = nyr_opt_const(&f, 99);
  emit_local_store(&f, 0, initial);
  if (other_slot_store)
    emit_local_store(&f, 1, initial);
  int first = emit_load_local(&f, 0);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  if (same_slot_store)
    emit_local_store(&f, 0, replacement);
  if (other_slot_store)
    emit_local_store(&f, 1, replacement);
  if (unknown_call)
    emit_unknown_call(&f);
  int second = emit_load_local(&f, 0);
  nyr_opt_ret(&f, second);
  const char *name = same_slot_store    ? "gvn-pre-local-load-same-slot-store"
                     : other_slot_store ? "gvn-pre-local-load-other-slot-store"
                     : unknown_call     ? "gvn-pre-local-load-call"
                                        : "gvn-pre-local-load-cse";
  if (first < 0 || second < 0 || !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long before = unknown_call ? 0 : nyr_opt_eval(&f, name);
  bool ok = nyr_gvn_pre(&f) && nyr_opt_verify(&f, name);
  long long after = ok && !unknown_call ? nyr_opt_eval(&f, name) : 0;
  int loads = 0;
  for (size_t i = 0; i < f.len; ++i)
    loads += f.data[i].op == NYIR_LOAD_LOCAL;
  ok = ok && loads == want_loads &&
       (unknown_call ||
        (before == after && after == (same_slot_store ? 99 : 41)));
  if (!ok) {
    fprintf(stderr, "[%s] loads=%d before=%lld after=%lld expected=%d\n", name,
            loads, before, after, want_loads);
    nyr_dump(stderr, &f, name);
  }
  nyr_ir_func_free(&f);
  return ok;
}

static int emit_known_call(nyr_func_t *f, const char *symbol, int arg,
                           unsigned effects, nyr_scalar_class_t result_class) {
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
                                       .scalar_class = result_class});
  if (dst >= 0) {
    f->data[f->len - 1].flags |= NYIR_INST_F_EFFECTS_KNOWN;
    f->data[f->len - 1].effects = effects;
  }
  return dst;
}

static bool run_pure_call_cse_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int arg = nyr_opt_const(&f, 9);
  unsigned pure = NYIR_EFFECT_CALL | NYIR_EFFECT_MAY_TRAP;
  int first =
      emit_known_call(&f, "pure_scalar", arg, pure, NYIR_SCALAR_I64_RAW);
  int second =
      emit_known_call(&f, "pure_scalar", arg, pure, NYIR_SCALAR_I64_RAW);
  nyr_opt_ret(&f, second);
  bool ok = first >= 0 && second >= 0 &&
            nyr_opt_verify(&f, "gvn-pre-pure-call/before") &&
            nyr_gvn_pre(&f) && nyr_opt_verify(&f, "gvn-pre-pure-call/after") &&
            f.data[1].op == NYIR_CALL && f.data[2].op == NYIR_COPY &&
            f.data[2].a == first;
  if (!ok)
    fprintf(stderr,
            "[gvn-pre-pure-call] known pure calls not safely commoned\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_read_call_memory_barrier_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int arg = nyr_opt_const(&f, 9);
  unsigned reader = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY;
  int first =
      emit_known_call(&f, "reads_memory", arg, reader, NYIR_SCALAR_I64_RAW);
  int repeated =
      emit_known_call(&f, "reads_memory", arg, reader, NYIR_SCALAR_I64_RAW);
  emit_known_call(&f, "writes_memory", arg,
                  NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY,
                  NYIR_SCALAR_I64_RAW);
  int after_write =
      emit_known_call(&f, "reads_memory", arg, reader, NYIR_SCALAR_I64_RAW);
  nyr_opt_ret(&f, after_write);
  bool ok = first >= 0 && repeated >= 0 && after_write >= 0 &&
            nyr_opt_verify(&f, "gvn-pre-read-call-write/before") &&
            nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, "gvn-pre-read-call-write/after") &&
            f.data[1].op == NYIR_CALL && f.data[2].op == NYIR_COPY &&
            f.data[2].a == first && f.data[3].op == NYIR_CALL &&
            f.data[4].op == NYIR_CALL;
  if (!ok)
    fprintf(stderr,
            "[gvn-pre-read-call-write] read-call value crossed a write\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_read_only_call_load_reuse_case(void) {
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
                                            .symbol = "gvn_pre_global_cell"});
  int arg = nyr_opt_const(&f, 9);
  int first = emit_load(&f, address);
  int reader = emit_known_call(&f, "reads_memory", arg,
                               NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY,
                               NYIR_SCALAR_I64_RAW);
  int second = emit_load(&f, address);
  nyr_opt_ret(&f, second);
  bool ok = address >= 0 && first >= 0 && reader >= 0 && second >= 0 &&
            nyr_opt_verify(&f, "gvn-pre-load-readonly-call/before") &&
            nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, "gvn-pre-load-readonly-call/after") &&
            f.data[2].op == NYIR_LOAD_I64 && f.data[3].op == NYIR_CALL &&
            f.data[4].op == NYIR_COPY && f.data[4].a == first;
  if (!ok)
    fprintf(
        stderr,
        "[gvn-pre-load-readonly-call] read-only call blocked a stable load\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_raw_memcmp_load_reuse_case(void) {
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
                                            .symbol = "gvn_pre_global_cell"});
  int compare_address = nyr_opt_const(&f, 0x2000);
  int length = nyr_opt_const(&f, 8);
  int first = emit_load(&f, address);
  int compare = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                            .dst = -1,
                                            .a = address,
                                            .b = compare_address,
                                            .c = length,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .imm = 3,
                                            .symbol = "rt_memcmp_raw"});
  int second = emit_load(&f, address);
  nyr_opt_ret(&f, second);
  bool ok =
      address >= 0 && compare_address >= 0 && length >= 0 && first >= 0 &&
      compare >= 0 && second >= 0 &&
      nyr_call_effect_summary(&f.data[compare]) ==
          (NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP) &&
      nyr_effective_effects(&f.data[compare]) ==
          (NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP) &&
      nyr_opt_verify(&f, "gvn-pre-raw-memcmp/before") && nyr_gvn_pre(&f) &&
      nyr_opt_verify(&f, "gvn-pre-raw-memcmp/after") &&
      f.data[(size_t)first].op == NYIR_LOAD_I64 &&
      f.data[(size_t)compare].op == NYIR_CALL &&
      f.data[(size_t)second].op == NYIR_COPY &&
      f.data[(size_t)second].a == first;
  if (!ok)
    fprintf(
        stderr,
        "[gvn-pre-raw-memcmp] read-only runtime call blocked a stable load\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_raw_memory_write_barrier_case(bool alias_destination,
                                              bool complete_call, bool copy,
                                              int want_loads) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int read_address = emit_symbol_address(&f, "gvn_pre_global_cell");
  int write_address = emit_symbol_address(
      &f, alias_destination ? "gvn_pre_global_cell" : "gvn_pre_other_cell");
  int value = nyr_opt_const(&f, 0x5a);
  int length = nyr_opt_const(&f, 8);
  int first = emit_load(&f, read_address);
  int fill = nyr_ir_emit(
      &f, (nyr_inst_t){.op = NYIR_CALL,
                        .dst = -1,
                        .a = write_address,
                        .b = copy ? read_address : value,
                        .c = complete_call ? length : -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = complete_call ? 3 : 2,
                        .symbol = copy ? "rt_memcpy_raw" : "rt_memset_raw"});
  int second = emit_load(&f, read_address);
  nyr_opt_ret(&f, second);
  int loads = 0, phis = 0;
  bool ok = read_address >= 0 && write_address >= 0 && value >= 0 &&
            length >= 0 && first >= 0 && fill >= 0 && second >= 0 &&
            nyr_call_effect_summary(&f.data[fill]) ==
                (NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY |
                 (copy ? NYIR_EFFECT_READ_MEMORY : 0) | NYIR_EFFECT_MAY_TRAP) &&
            nyr_opt_verify(&f, "gvn-pre-raw-memory-write/before") &&
            nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, "gvn-pre-raw-memory-write/after");
  if (ok) {
    count_memory_ops(&f, &loads, &phis);
    ok = loads == want_loads && phis == 0;
  }
  if (!ok) {
    fprintf(stderr,
            "[gvn-pre-raw-memory-write] alias=%d complete=%d copy=%d "
            "loads=%d want=%d\n",
            alias_destination, complete_call, copy, loads, want_loads);
    nyr_dump(stderr, &f, "gvn-pre-raw-memory-write");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_raw_memory_write_range_case(int64_t read_offset,
                                            int64_t write_offset,
                                            int64_t length, bool dynamic_length,
                                            bool copy, int want_loads) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int read_base = emit_symbol_address(&f, "gvn_pre_range_cell");
  int write_base = emit_symbol_address(&f, "gvn_pre_range_cell");
  int read_address = emit_address_offset(&f, read_base, read_offset);
  int write_address = emit_address_offset(&f, write_base, write_offset);
  int source_or_value = copy ? read_address : nyr_opt_const(&f, 0x6b);
  int call_length =
      dynamic_length ? emit_load_local(&f, 0) : nyr_opt_const(&f, length);
  int first = emit_load(&f, read_address);
  int call = nyr_ir_emit(
      &f, (nyr_inst_t){.op = NYIR_CALL,
                        .dst = -1,
                        .a = write_address,
                        .b = source_or_value,
                        .c = call_length,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = 3,
                        .symbol = copy ? "rt_memcpy_raw" : "rt_memset_raw"});
  int second = emit_load(&f, read_address);
  nyr_opt_ret(&f, second);
  int loads = 0, phis = 0;
  bool ok = read_base >= 0 && write_base >= 0 && read_address >= 0 &&
            write_address >= 0 && source_or_value >= 0 && call_length >= 0 &&
            first >= 0 && call >= 0 && second >= 0 &&
            nyr_opt_verify(&f, "gvn-pre-raw-memory-range/before") &&
            nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, "gvn-pre-raw-memory-range/after");
  if (ok) {
    count_memory_ops(&f, &loads, &phis);
    ok = loads == want_loads && phis == 0;
  }
  if (!ok) {
    fprintf(stderr,
            "[gvn-pre-raw-memory-range] read=%" PRId64 " write=%" PRId64
            " length=%" PRId64 " dynamic=%d copy=%d loads=%d want=%d\n",
            read_offset, write_offset, length, dynamic_length, copy, loads,
            want_loads);
    nyr_dump(stderr, &f, "gvn-pre-raw-memory-range");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_load_across_nonwriting_effects_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .symbol = "gvn_pre_clock_stable_global"});
  int value = nyr_opt_const(&f, 17);
  int first = emit_load(&f, address);
  int len = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                        .dst = -1,
                                        .a = value,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 1,
                                        .symbol = "rt_len_strict"});
  int clock = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .imm = 0,
                                          .symbol = "rt_monotonic_ns_raw"});
  int second = emit_load(&f, address);
  nyr_opt_ret(&f, second);
  bool ok =
      address >= 0 && first >= 0 && len >= 0 && clock >= 0 && second >= 0 &&
      nyr_call_effect_summary(&f.data[3]) ==
          (NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_ALLOCATION |
           NYIR_EFFECT_MAY_TRAP) &&
      nyr_call_effect_summary(&f.data[4]) ==
          (NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_IO) &&
      nyr_opt_verify(&f, "gvn-pre-load-across-nonwriting-effects/before") &&
      nyr_gvn_pre(&f) &&
      nyr_opt_verify(&f, "gvn-pre-load-across-nonwriting-effects/after") &&
      f.data[2].op == NYIR_LOAD_I64 && f.data[3].op == NYIR_CALL &&
      f.data[4].op == NYIR_CALL && f.data[5].op == NYIR_COPY &&
      f.data[5].a == first;
  if (!ok) {
    fprintf(stderr, "[gvn-pre-load-across-nonwriting-effects] failed to "
                    "preserve calls and forward stable load\n");
    nyr_dump(stderr, &f, "gvn-pre-load-across-nonwriting-effects");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_load_across_allocation_unknown_alias_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int slot = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .symbol = "gvn_pre_pointer_holder"});
  int pointer = emit_load(&f, slot);
  int arg = nyr_opt_const(&f, 0);
  int first = emit_load(&f, pointer);
  int alloc = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_CALL,
                                          .dst = -1,
                                          .a = arg,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .imm = 1,
                                          .symbol = "rt_len_strict"});
  int second = emit_load(&f, pointer);
  nyr_opt_ret(&f, second);
  bool ok =
      slot >= 0 && pointer >= 0 && arg >= 0 && first >= 0 && alloc >= 0 &&
      second >= 0 &&
      nyr_opt_verify(&f, "gvn-pre-load-allocation-unknown-alias/before") &&
      nyr_gvn_pre(&f) &&
      nyr_opt_verify(&f, "gvn-pre-load-allocation-unknown-alias/after") &&
      f.data[3].op == NYIR_LOAD_I64 && f.data[4].op == NYIR_CALL &&
      f.data[5].op == NYIR_LOAD_I64;
  if (!ok) {
    fprintf(stderr, "[gvn-pre-load-allocation-unknown-alias] allocation "
                    "crossed an unknown heap read\n");
    nyr_dump(stderr, &f, "gvn-pre-load-allocation-unknown-alias");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static int emit_raw_load8(nyr_func_t *f, int address, int offset) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                    .dst = -1,
                                    .a = address,
                                    .b = offset,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = 2,
                                    .symbol = "rt_load8_idx"});
}

static void emit_raw_store8(nyr_func_t *f, int address, int offset,
                            int value) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                             .dst = -1,
                             .a = address,
                             .b = offset,
                             .c = value,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 3,
                             .symbol = "rt_store8_idx"});
}

static bool run_raw_load8_effect_contract_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int address = nyr_opt_const(&f, 4096);
  int offset = nyr_opt_const(&f, 7);
  int value = nyr_opt_const(&f, 42);
  int first = emit_raw_load8(&f, address, offset);
  int repeated = emit_raw_load8(&f, address, offset);
  emit_raw_store8(&f, address, offset, value);
  int after_store = emit_raw_load8(&f, address, offset);
  nyr_opt_ret(&f, after_store);
  bool ok =
      first >= 0 && repeated >= 0 && after_store >= 0 &&
      nyr_call_effect_summary(&f.data[3]) ==
          (NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP) &&
      nyr_call_effect_summary(&f.data[5]) ==
          (NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY |
           NYIR_EFFECT_MAY_TRAP) &&
      nyr_opt_verify(&f, "gvn-pre-raw-load8/before") && nyr_gvn_pre(&f) &&
      nyr_opt_verify(&f, "gvn-pre-raw-load8/after");
  if (ok) {
    ok = f.data[3].op == NYIR_CALL && f.data[4].op == NYIR_COPY &&
         f.data[4].a == first && f.data[5].op == NYIR_CALL &&
         f.data[6].op == NYIR_CALL && f.data[6].dst == after_store;
  }
  if (!ok) {
    fprintf(stderr, "[gvn-pre-raw-load8] read reuse crossed a write or missed "
                    "a stable read\n");
    nyr_dump(stderr, &f, "gvn-pre-raw-load8");
  }
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_call_contract_key_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int arg = nyr_opt_const(&f, 9);
  int first = emit_known_call(&f, "contract_sensitive", arg, NYIR_EFFECT_CALL,
                              NYIR_SCALAR_I64_RAW);
  int second = emit_known_call(&f, "contract_sensitive", arg,
                               NYIR_EFFECT_CALL | NYIR_EFFECT_MAY_TRAP,
                               NYIR_SCALAR_I64_TAGGED);
  nyr_opt_ret(&f, second);
  bool ok = first >= 0 && second >= 0 &&
            nyr_opt_verify(&f, "gvn-pre-call-contract/before") &&
            nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, "gvn-pre-call-contract/after") &&
            f.data[1].op == NYIR_CALL && f.data[2].op == NYIR_CALL;
  if (!ok)
    fprintf(
        stderr,
        "[gvn-pre-call-contract] distinct effect/result contracts merged\n");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_call_ownership_key_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int arg = nyr_opt_const(&f, 9);
  unsigned pure = NYIR_EFFECT_CALL | NYIR_EFFECT_MAY_TRAP;
  int borrowed = emit_known_call(&f, "ownership_sensitive", arg, pure,
                                 NYIR_SCALAR_I64_RAW);
  nyr_call_arg_ownership_set(&f.data[f.len - 1], 0, NY_SEM_OWN_BORROWED);
  int consumed = emit_known_call(&f, "ownership_sensitive", arg, pure,
                                 NYIR_SCALAR_I64_RAW);
  nyr_call_arg_ownership_set(&f.data[f.len - 1], 0, NY_SEM_OWN_OWNED);
  nyr_opt_ret(&f, consumed);
  bool ok = borrowed >= 0 && consumed >= 0 &&
            nyr_opt_verify(&f, "gvn-pre-call-ownership/before") &&
            nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, "gvn-pre-call-ownership/after") &&
            f.data[1].op == NYIR_CALL && f.data[2].op == NYIR_CALL;
  if (!ok)
    fprintf(stderr, "[gvn-pre-call-ownership] distinct actual ownership "
                    "contracts merged\n");
  if (!ok)
    nyr_dump(stderr, &f, "gvn-pre-call-ownership");
  nyr_ir_func_free(&f);

  nyr_opt_begin(&f);
  arg = nyr_opt_const(&f, 9);
  int borrows = emit_known_call(&f, "formal_contract_sensitive", arg, pure,
                                NYIR_SCALAR_I64_RAW);
  nyr_call_arg_contract_set(&f.data[f.len - 1], 0, NYIR_ARG_CONTRACT_BORROWS);
  int consumes = emit_known_call(&f, "formal_contract_sensitive", arg, pure,
                                 NYIR_SCALAR_I64_RAW);
  nyr_call_arg_contract_set(&f.data[f.len - 1], 0, NYIR_ARG_CONTRACT_CONSUMES);
  nyr_opt_ret(&f, consumes);
  ok = ok && borrows >= 0 && consumes >= 0 &&
       nyr_opt_verify(&f, "gvn-pre-formal-contract/before") &&
       nyr_gvn_pre(&f) &&
       nyr_opt_verify(&f, "gvn-pre-formal-contract/after") &&
       f.data[1].op == NYIR_CALL && f.data[2].op == NYIR_CALL;
  if (!ok)
    fprintf(stderr, "[gvn-pre-call-ownership] distinct formal ownership "
                    "contracts merged\n");
  if (!ok)
    nyr_dump(stderr, &f, "gvn-pre-formal-contract");
  nyr_ir_func_free(&f);
  return ok;
}

static bool run_scalar_class_key_case(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int raw = nyr_ir_emit(&f, (nyr_inst_t){
                              .op = NYIR_CONST_I64,
                              .dst = -1,
                              .imm = 100,
                              .scalar_class = NYIR_SCALAR_I64_RAW,
                          });
  int tagged = nyr_ir_emit(&f, (nyr_inst_t){
                                 .op = NYIR_CONST_I64,
                                 .dst = -1,
                                 .imm = 100,
                                 .scalar_class = NYIR_SCALAR_I64_TAGGED,
                             });
  nyr_opt_ret(&f, tagged);
  bool ok = raw >= 0 && tagged >= 0 &&
            nyr_opt_verify(&f, "gvn-pre-scalar-class/before") &&
            nyr_gvn_pre(&f) &&
            nyr_opt_verify(&f, "gvn-pre-scalar-class/after") &&
            f.data[0].op == NYIR_CONST_I64 && f.data[1].op == NYIR_CONST_I64 &&
            f.data[0].scalar_class == NYIR_SCALAR_I64_RAW &&
            f.data[1].scalar_class == NYIR_SCALAR_I64_TAGGED;
  if (!ok) {
    fprintf(stderr,
            "[gvn-pre-scalar-class] equal words with distinct ABIs merged\n");
    nyr_dump(stderr, &f, "gvn-pre-scalar-class");
  }
  nyr_ir_func_free(&f);
  return ok;
}

int main(void) {
  return run_case("gvn-pre-load-diamond", false, false, 2, 1) &&
                 run_case("gvn-pre-load-store", true, false, 3, 0) &&
                 run_case("gvn-pre-load-unknown-call", false, true, 3, 0) &&
                 run_post_call_load_cse_case() && run_local_store_case() &&
                 run_unrelated_local_store_case() &&
                 run_address_taken_local_case() &&
                 run_forwarded_pointer_provenance_case() &&
                 run_loop_local_load_case() &&
                 run_loop_reexecuted_load_case() &&
                 run_loop_header_write_before_reentry_case() &&
                 run_disjoint_global_store_case() &&
                 run_copy_operand_cse_case() &&
                 run_rewritten_copy_chain_load_case() &&
                 run_local_load_cse_case(false, false, false, 1) &&
                 run_local_load_cse_case(true, false, false, 2) &&
                 run_local_load_cse_case(false, true, false, 1) &&
                 run_local_load_cse_case(false, false, true, 2) &&
                 run_pure_call_cse_case() &&
                 run_read_call_memory_barrier_case() &&
                 run_read_only_call_load_reuse_case() &&
                 run_raw_memcmp_load_reuse_case() &&
                 run_raw_memory_write_barrier_case(false, true, false, 1) &&
                 run_raw_memory_write_barrier_case(true, true, false, 2) &&
                 run_raw_memory_write_barrier_case(false, false, false, 2) &&
                 run_raw_memory_write_barrier_case(false, true, true, 1) &&
                 run_raw_memory_write_barrier_case(true, true, true, 2) &&
                 run_raw_memory_write_range_case(0, 8, 8, false, false, 1) &&
                 run_raw_memory_write_range_case(0, 7, 1, false, false, 2) &&
                 run_raw_memory_write_range_case(0, 16, 1, false, true, 1) &&
                 run_raw_memory_write_range_case(0, 16, 0, false, false, 1) &&
                 run_raw_memory_write_range_case(0, 16, -1, false, false, 1) &&
                 run_raw_memory_write_range_case(0, 16, INT64_MIN, false, false,
                                                 1) &&
                 run_raw_memory_write_range_case(0, 16, -1, false, true, 1) &&
                 run_raw_memory_write_range_case(0, 16, INT64_MIN, false, true,
                                                 1) &&
                 run_raw_memory_write_range_case(0, 16, 0, true, false, 2) &&
                 run_raw_memory_write_range_case(INT64_MAX - 4, 0, 1, false,
                                                 false, 2) &&
                 run_raw_memory_write_range_case(0, INT64_MAX - 4, 8, false,
                                                 false, 2) &&
                 run_unknown_offset_store_case() &&
                 run_same_root_offset_store_case(0, 8, 1,
                                                 "gvn-pre-adjacent-offset") &&
                 run_same_root_offset_store_case(
                     0, 4, 2, "gvn-pre-overlapping-offset") &&
                 run_same_root_offset_store_case(
                     INT64_MIN, INT64_MAX, 2, "gvn-pre-offset-address-wrap") &&
                 run_load_across_nonwriting_effects_case() &&
                 run_load_across_allocation_unknown_alias_case() &&
                 run_raw_load8_effect_contract_case() &&
                 run_call_contract_key_case() &&
                 run_call_ownership_key_case() && run_scalar_class_key_case()
             ? 0
             : 1;
}
