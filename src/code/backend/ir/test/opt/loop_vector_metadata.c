/*
 * Vector rewrites must not retain scalar pointer-alias facts.
 */
#include "code/backend/ir/opt/loop.h"
#include "support.h"

static int64_t values[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static int64_t nested_values[32] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                    23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
static size_t nested_effect_calls;

static int emit_alloca(nyr_func_t *f, int64_t bytes) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .imm = bytes,
                                     .scalar_class = NYIR_SCALAR_POINTER});
}

static bool build_sum(nyr_func_t *f, size_t *update_inst, bool copy_iv,
                      int64_t seed) {
  nyr_opt_begin(f);
  int base = emit_alloca(f, 64);
  int limit = nyr_opt_const(f, 8);
  int zero = nyr_opt_const(f, 0);
  int sum_seed = nyr_opt_const(f, seed);
  int one = nyr_opt_const(f, 1);
  int eight = nyr_opt_const(f, 8);
  for (int i = 0; i < 8; ++i) {
    int offset = nyr_opt_const(f, (int64_t)i * 8);
    int address = nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
    int value = nyr_opt_const(f, values[i]);
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                .dst = -1,
                                .a = address,
                                .b = -1,
                                .c = value,
                                .d = -1,
                                .e = -1,
                                .f = -1});
  }
  nyr_opt_label(f, 9);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, 9, zero, 11, zero);
  size_t sum_phi = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, 9, sum_seed, 11, sum_seed);
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_br(f, 12);
  nyr_opt_label(f, 11);
  int offset = nyr_opt_binop(f, NYIR_MUL_I64, iv, eight);
  int address = nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
  int loaded = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                           .dst = -1,
                                           .a = address,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  *update_inst = f->len;
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, loaded);
  int iv_operand = iv;
  if (copy_iv)
    iv_operand = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_COPY,
                                             .dst = -1,
                                             .a = iv,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1});
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv_operand, one);
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[sum_phi].phi_incoming[1].value = next_sum;
  nyr_opt_label(f, 12);
  nyr_opt_ret(f, sum);
  return nyr_opt_verify(f, "loop-vector-metadata-before");
}

static bool build_f64_memory_sum(nyr_func_t *f) {
  /*
   * Regrouping these additions changes the final bit pattern from 1.0.
   */
  static const double terms[8] = {1.0e16, 1.0, -1.0e16, 1.0,
                                  1.0e16, 1.0, -1.0e16, 1.0};
  nyr_opt_begin(f);
  int base = emit_alloca(f, 64);
  int limit = nyr_opt_const(f, 8);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int eight = nyr_opt_const(f, 8);
  int fzero = nyr_opt_const_f64(f, 0.0);
  if (base < 0 || limit < 0 || zero < 0 || one < 0 || eight < 0 || fzero < 0)
    return false;
  for (int i = 0; i < 8; ++i) {
    int offset = nyr_opt_const(f, (int64_t)i * 8);
    int address =
        offset < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
    int value = nyr_opt_const_f64(f, terms[i]);
    if (address < 0 || value < 0)
      return false;
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                .dst = -1,
                                .a = address,
                                .b = -1,
                                .c = value,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .scalar_class = NYIR_SCALAR_F64});
  }
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 20);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 21, zero);
  size_t sum_phi = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, -1, fzero, 21, fzero);
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 21);
  nyr_opt_br(f, 22);
  nyr_opt_label(f, 21);
  int offset = nyr_opt_binop(f, NYIR_MUL_I64, iv, eight);
  int address = offset < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
  int loaded =
      address < 0
          ? -1
          : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                        .dst = -1,
                                        .a = address,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .scalar_class = NYIR_SCALAR_F64});
  int next_sum = loaded < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, sum, loaded);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  if (next_sum < 0 || next_iv < 0)
    return false;
  nyr_opt_br(f, 20);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[sum_phi].phi_incoming[1].value = next_sum;
  nyr_opt_label(f, 22);
  int result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_F64_TO_I64,
                                           .dst = -1,
                                           .a = sum,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  if (result < 0)
    return false;
  nyr_opt_ret(f, result);
  return nyr_opt_verify(f, "loop-vector-f64-memory-before");
}

static bool build_f64_elementwise(nyr_func_t *f) {
  nyr_opt_begin(f);
  int input = emit_alloca(f, 64);
  int output = emit_alloca(f, 64);
  int limit = nyr_opt_const(f, 8);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int eight = nyr_opt_const(f, 8);
  int last_offset = nyr_opt_const(f, 56);
  int fone = nyr_opt_const_f64(f, 1.0);
  if (input < 0 || output < 0 || limit < 0 || zero < 0 || one < 0 ||
      eight < 0 || last_offset < 0 || fone < 0)
    return false;
  for (int i = 0; i < 8; ++i) {
    int offset = nyr_opt_const(f, (int64_t)i * 8);
    int address =
        offset < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, input, offset);
    int value = nyr_opt_const_f64(f, (double)(i + 1));
    if (address < 0 || value < 0)
      return false;
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                .dst = -1,
                                .a = address,
                                .b = -1,
                                .c = value,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .scalar_class = NYIR_SCALAR_F64});
  }
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 20);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 21, zero);
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 21);
  nyr_opt_br(f, 22);
  nyr_opt_label(f, 21);
  int offset = nyr_opt_binop(f, NYIR_MUL_I64, iv, eight);
  int input_address =
      offset < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, input, offset);
  int loaded =
      input_address < 0
          ? -1
          : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                        .dst = -1,
                                        .a = input_address,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .scalar_class = NYIR_SCALAR_F64});
  int mapped = loaded < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_F64, loaded, fone);
  int output_address =
      offset < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, output, offset);
  if (mapped < 0 || output_address < 0)
    return false;
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = output_address,
                              .b = -1,
                              .c = mapped,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .scalar_class = NYIR_SCALAR_F64});
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  if (next_iv < 0)
    return false;
  nyr_opt_br(f, 20);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  nyr_opt_label(f, 22);
  int result_address = nyr_opt_binop(f, NYIR_ADD_I64, output, last_offset);
  int result =
      result_address < 0
          ? -1
          : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                        .dst = -1,
                                        .a = result_address,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .scalar_class = NYIR_SCALAR_F64});
  int integer_result = result < 0
                           ? -1
                           : nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_F64_TO_I64,
                                                         .dst = -1,
                                                         .a = result,
                                                         .b = -1,
                                                         .c = -1,
                                                         .d = -1,
                                                         .e = -1,
                                                         .f = -1});
  if (integer_result < 0)
    return false;
  nyr_opt_ret(f, integer_result);
  return nyr_opt_verify(f, "loop-vector-f64-elementwise-before");
}

static bool resolve_nested_tbuf(void *ctx, const char *symbol,
                                const int64_t *args, size_t arg_count,
                                int64_t *result, char *err, size_t err_len) {
  (void)ctx;
  if (symbol && strcmp(symbol, "test_external_tbuf") == 0 && arg_count == 0 &&
      result) {
    *result = (int64_t)(uintptr_t)nested_values;
    return true;
  }
  if (symbol && strcmp(symbol, "test_effect") == 0 && arg_count == 0 &&
      result) {
    ++nested_effect_calls;
    *result = 0;
    return true;
  }
  if (!symbol || strcmp(symbol, "rt_tbuf_new_raw") != 0 || arg_count != 2 ||
      (args[0] != 16 && args[0] != 32) || args[1] != 8 || !result) {
    snprintf(err, err_len, "unexpected test allocation call");
    return false;
  }
  *result = (int64_t)(uintptr_t)nested_values;
  return true;
}

static bool build_nested_sum_config(nyr_func_t *f, bool multi_latch,
                                    bool bypass_guard, bool overflow_exit,
                                    bool strided_rows, bool effect_in_inner,
                                    bool different_latch_step, bool store_next,
                                    bool inclusive_guard) {
  nyr_opt_begin(f);
  int count = nyr_opt_const(f, strided_rows ? 32 : 16);
  int width = nyr_opt_const(f, 8);
  int base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                         .dst = -1,
                                         .a = count,
                                         .b = width,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .symbol = "rt_tbuf_new_raw",
                                         .imm = 2});
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int four = nyr_opt_const(f, store_next ? 3 : 4);
  int two = nyr_opt_const(f, 2);
  int64_t row_start_value = overflow_exit ? INT64_MAX - 1 : 0;
  int row_start = nyr_opt_const(f, row_start_value);
  int64_t row_limit_value = overflow_exit     ? INT64_MAX
                            : inclusive_guard ? (strided_rows ? 6 : 3)
                            : strided_rows    ? 8
                                              : 4;
  int row_limit = nyr_opt_const(f, row_limit_value);
  int row_exit = nyr_opt_const(f, overflow_exit ? INT64_MIN : 0);
  int row_bytes = nyr_opt_const(f, 32);
  int elem_bytes = nyr_opt_const(f, 8);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t row_phi = f->len;
  int row = f->next_value;
  nyr_opt_phi(f, -1, row_start, 20, zero);
  size_t outer_sum_phi = f->len;
  int outer_sum = f->next_value;
  nyr_opt_phi(f, -1, zero, 20, zero);
  if (bypass_guard) {
    int bypass = nyr_opt_cmp(f, row, row_start, NYIR_CMP_EQ);
    nyr_opt_br_if(f, bypass, 11);
    nyr_opt_label(f, 19);
  }
  int row_cond = nyr_opt_cmp(f, row, row_limit,
                             inclusive_guard ? NYIR_CMP_LE : NYIR_CMP_LT);
  nyr_opt_br_if(f, row_cond, 11);
  nyr_opt_br(f, 30);
  nyr_opt_label(f, 11);
  nyr_opt_br(f, 12);
  nyr_opt_label(f, 12);
  size_t col_phi = f->len;
  int col = f->next_value;
  nyr_opt_phi(f, 11, zero, 14, zero);
  size_t sum_phi = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, 11, zero, 14, zero);
  int col_cond = nyr_opt_cmp(f, col, four, NYIR_CMP_LT);
  nyr_opt_br_if(f, col_cond, 14);
  nyr_opt_br(f, 15);
  nyr_opt_label(f, 14);
  int row_index = nyr_opt_binop(f, NYIR_SUB_I64, row, row_start);
  int row_offset = nyr_opt_binop(f, NYIR_MUL_I64, row_index, row_bytes);
  int col_offset = nyr_opt_binop(f, NYIR_MUL_I64, col, elem_bytes);
  int offset = nyr_opt_binop(f, NYIR_ADD_I64, row_offset, col_offset);
  int address = nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
  if (effect_in_inner)
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                .dst = -1,
                                .a = -1,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .symbol = "test_effect",
                                .imm = 0,
                                .effects = NYIR_EFFECT_CALL |
                                           NYIR_EFFECT_UNKNOWN_SIDE_EFFECT});
  int loaded = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                           .dst = -1,
                                           .a = address,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  if (store_next) {
    int store_address = nyr_opt_binop(f, NYIR_ADD_I64, address, elem_bytes);
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                .dst = -1,
                                .a = store_address,
                                .b = -1,
                                .c = loaded,
                                .d = -1,
                                .e = -1,
                                .f = -1});
  }
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, loaded);
  int next_col = nyr_opt_binop(f, NYIR_ADD_I64, col, one);
  nyr_opt_br(f, 12);
  f->data[col_phi].phi_incoming[1].value = next_col;
  f->data[sum_phi].phi_incoming[1].value = next_sum;
  nyr_opt_label(f, 15);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 20);
  int accumulated = nyr_opt_binop(f, NYIR_ADD_I64, outer_sum, sum);
  int row_step = overflow_exit || strided_rows ? two : one;
  int next_row = nyr_opt_binop(f, NYIR_ADD_I64, row, row_step);
  int alternate_row = different_latch_step
                          ? nyr_opt_binop(f, NYIR_ADD_I64, row, two)
                          : next_row;
  if (overflow_exit) {
    int last_row = nyr_opt_cmp(f, row, row_exit, NYIR_CMP_EQ);
    nyr_opt_br_if(f, last_row, 31);
    nyr_opt_label(f, 22);
  } else if (multi_latch) {
    int alternate_mask = strided_rows ? two : one;
    int alternate = nyr_opt_binop(f, NYIR_AND_I64, row, alternate_mask);
    nyr_opt_br_if(f, alternate, 21);
    nyr_opt_label(f, 22);
  }
  nyr_opt_br(f, 10);
  f->data[row_phi].phi_incoming[1].value = next_row;
  f->data[outer_sum_phi].phi_incoming[1].value = accumulated;
  if (overflow_exit) {
    f->data[row_phi].phi_incoming[1].predecessor_label = 22;
    f->data[outer_sum_phi].phi_incoming[1].predecessor_label = 22;
  }
  if (multi_latch) {
    nyr_opt_label(f, 21);
    nyr_opt_br(f, 10);
    const size_t phis[] = {row_phi, outer_sum_phi};
    for (size_t k = 0; k < 2; ++k) {
      nyr_inst_t *phi = &f->data[phis[k]];
      void *grown = realloc(phi->phi_incoming, 3 * sizeof(*phi->phi_incoming));
      if (!grown)
        return false;
      phi->phi_incoming = grown;
      phi->phi_incoming[1].predecessor_label = 22;
      phi->phi_incoming[2] = phi->phi_incoming[1];
      phi->phi_incoming[2].predecessor_label = 21;
      if (different_latch_step && k == 0)
        phi->phi_incoming[2].value = alternate_row;
      phi->phi_incoming_len = 3;
    }
  }
  if (overflow_exit) {
    nyr_opt_label(f, 31);
    nyr_opt_ret(f, accumulated);
  }
  nyr_opt_label(f, 30);
  nyr_opt_ret(f, outer_sum);
  return nyr_opt_verify(f, "loop-vector-guarded-tbuf-before");
}

static bool build_nested_sum(nyr_func_t *f, bool multi_latch, bool bypass_guard,
                             bool overflow_exit, bool strided_rows,
                             bool effect_in_inner, bool different_latch_step,
                             bool store_next) {
  return build_nested_sum_config(f, multi_latch, bypass_guard, overflow_exit,
                                 strided_rows, effect_in_inner,
                                 different_latch_step, store_next, false);
}

static bool eval_nested_sum(nyr_func_t *f, const char *stage, int64_t *out) {
  nyr_eval_result_t result = {0};
  char err[256] = {0};
  bool ok = nyr_ir_eval_with_calls(f, NULL, 0, 100000, &result,
                                   resolve_nested_tbuf, NULL, err, sizeof(err));
  if (!ok)
    fprintf(stderr, "[loop-vector-guarded-tbuf] %s: %s\n", stage, err);
  else
    *out = result.result;
  nyr_eval_result_free(&result);
  return ok;
}

static void reset_nested_values(void) {
  for (size_t i = 0; i < sizeof(nested_values) / sizeof(nested_values[0]); ++i)
    nested_values[i] = (int64_t)i + 1;
}

static bool build_checked_external_sum(nyr_func_t *f, bool mark_in_bounds,
                                       bool store_in_loop) {
  nyr_opt_begin(f);
  int base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .symbol = "test_external_tbuf",
                                         .imm = 0,
                                         .scalar_class = NYIR_SCALAR_POINTER});
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int limit = nyr_opt_const(f, 8);
  int shift = nyr_opt_const(f, 3);
  if (base < 0 || zero < 0 || one < 0 || limit < 0 || shift < 0)
    return false;
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t iv_phi = f->len;
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  size_t sum_phi = f->len;
  int sum = f->next_value;
  nyr_opt_phi(f, -1, zero, 11, zero);
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_br(f, 12);
  nyr_opt_label(f, 11);
  int byte_offset = nyr_opt_binop(f, NYIR_SHL_I64, iv, shift);
  int address =
      byte_offset < 0 ? -1 : nyr_opt_binop(f, NYIR_ADD_I64, base, byte_offset);
  if (address < 0)
    return false;
  if (mark_in_bounds)
    f->data[f->len - 1].flags |= NYIR_INST_F_IN_BOUNDS_ADDR;
  int loaded = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                           .dst = -1,
                                           .a = address,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  if (loaded < 0)
    return false;
  if (store_in_loop)
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                .dst = -1,
                                .a = address,
                                .b = -1,
                                .c = loaded,
                                .d = -1,
                                .e = -1,
                                .f = -1});
  int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, loaded);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  if (next_sum < 0 || next_iv < 0)
    return false;
  nyr_opt_br(f, 10);
  f->data[iv_phi].phi_incoming[1].value = next_iv;
  f->data[sum_phi].phi_incoming[1].value = next_sum;
  nyr_opt_label(f, 12);
  nyr_opt_ret(f, sum);
  return nyr_opt_verify(f, "loop-vector-checked-external-before");
}

int main(void) {
  nyr_func_t f;
  if (!build_f64_memory_sum(&f)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  int64_t f64_before = nyr_opt_eval(&f, "loop-vector-f64-memory-before");
  bool ok = f64_before == 1 && nyr_loop_vectorize(&f) &&
            nyr_opt_verify(&f, "loop-vector-f64-memory-after") &&
            nyr_opt_eval(&f, "loop-vector-f64-memory-after") == f64_before;
  bool saw_f64_vector_load = false;
  bool saw_f64_vector_reduce = false;
  for (size_t i = 0; ok && i < f.len; ++i)
    if (f.data[i].op == NYIR_VEC4_LOAD_F64)
      saw_f64_vector_load = true;
    else if (f.data[i].op == NYIR_VEC4_REDUCE_ADD_F64)
      saw_f64_vector_reduce = true;
  ok = ok && saw_f64_vector_load && saw_f64_vector_reduce;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-f64-memory-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-f64-memory] strict floating reduction vectorizes in "
         "order");
  if (!ok)
    return 1;

  if (!build_f64_elementwise(&f)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  int64_t elementwise_before =
      nyr_opt_eval(&f, "loop-vector-f64-elementwise-before");
  ok = elementwise_before == 9 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-f64-elementwise-after") &&
       nyr_opt_eval(&f, "loop-vector-f64-elementwise-after") ==
           elementwise_before;
  bool saw_f64_load = false;
  bool saw_f64_store = false;
  bool saw_f64_add = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    saw_f64_load |= f.data[i].op == NYIR_VEC4_LOAD_F64;
    saw_f64_store |= f.data[i].op == NYIR_VEC4_STORE_F64;
    saw_f64_add |= f.data[i].op == NYIR_VEC4_ADD_F64;
  }
  ok = ok && saw_f64_load && saw_f64_store && saw_f64_add;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-f64-elementwise-after");
  nyr_ir_func_free(&f);
  if (!ok) {
    fputs("[loop-vector-f64-elementwise] four-lane result mismatch or "
          "missing vector operation\n",
          stderr);
    return 1;
  }
  puts("[loop-vector-f64-elementwise] four-lane memory and arithmetic "
       "preserve result");

  size_t update_inst = 0;
  if (!build_sum(&f, &update_inst, false, 7)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  int64_t before = nyr_opt_eval(&f, "loop-vector-metadata-before");
  f.data[update_inst].alias_class = 89;
  f.data[update_inst].range =
      (nyr_range_t){.has_min = true, .has_max = true, .min = 7, .max = 43};
  ok = before == 43 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-metadata-after") &&
       nyr_opt_eval(&f, "loop-vector-metadata-after") == 43;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-metadata-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts(
        "[loop-vector-metadata] seeded integer reduction preserves its result");
  else
    return 1;

  nyr_ir_func_free(&f);

  /*
   * Candidate recognition follows COPY chains, so IV-step rewriting must too.
   */
  if (!build_sum(&f, &update_inst, true, 7)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  before = nyr_opt_eval(&f, "loop-vector-copy-iv-before");
  ok = nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-copy-iv-after") &&
       nyr_opt_eval(&f, "loop-vector-copy-iv-after") == before;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-copy-iv-after");
  nyr_ir_func_free(&f);
  if (ok)
    printf("[loop-vector-copy-iv] copied induction keeps the vector-width "
           "step; result=%lld\n",
           (long long)before);
  if (!ok)
    return 1;

  if (!build_checked_external_sum(&f, true, false))
    return 1;
  reset_nested_values();
  int64_t checked_before = 0, checked_after = 0;
  ok = eval_nested_sum(&f, "checked-external-before", &checked_before) &&
       checked_before == 36 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-checked-external-after") &&
       eval_nested_sum(&f, "checked-external-after", &checked_after) &&
       checked_after == checked_before;
  bool saw_checked_vector_load = false;
  for (size_t i = 0; ok && i < f.len; ++i)
    saw_checked_vector_load |= f.data[i].op == NYIR_VEC4_LOAD_I64 ||
                               f.data[i].op == NYIR_VEC8_LOAD_I64;
  ok = ok && saw_checked_vector_load;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-checked-external-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-checked-external] checked read-only address vectorizes");
  if (!ok)
    return 1;

  if (!build_checked_external_sum(&f, true, true))
    return 1;
  ok = nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-checked-write-after");
  bool saw_write_vector_load = false;
  for (size_t i = 0; ok && i < f.len; ++i)
    saw_write_vector_load |= f.data[i].op == NYIR_VEC4_LOAD_I64 ||
                             f.data[i].op == NYIR_VEC8_LOAD_I64;
  ok = ok && !saw_write_vector_load;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-checked-write-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-checked-write] checked read fallback rejects writes");
  if (!ok)
    return 1;

  if (!build_checked_external_sum(&f, false, false))
    return 1;
  ok = nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-unchecked-external-after");
  bool saw_unchecked_vector_load = false;
  for (size_t i = 0; ok && i < f.len; ++i)
    saw_unchecked_vector_load |= f.data[i].op == NYIR_VEC4_LOAD_I64 ||
                                 f.data[i].op == NYIR_VEC8_LOAD_I64;
  ok = ok && !saw_unchecked_vector_load;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-unchecked-external-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-unchecked-external] unchecked address stays scalar");
  if (!ok)
    return 1;

  if (!build_nested_sum(&f, false, false, false, false, false, false, false))
    return 1;
  int64_t nested_before = 0, nested_after = 0;
  if (!eval_nested_sum(&f, "before", &nested_before) || nested_before != 136) {
    nyr_ir_func_free(&f);
    return 1;
  }
  ok = nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-guarded-tbuf-after") &&
       eval_nested_sum(&f, "after", &nested_after) &&
       nested_after == nested_before;
  bool saw_vector_load = false;
  for (size_t i = 0; ok && i < f.len; ++i)
    saw_vector_load |= f.data[i].op == NYIR_VEC4_LOAD_I64 ||
                       f.data[i].op == NYIR_VEC8_LOAD_I64;
  ok = ok && saw_vector_load;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-guarded-tbuf-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-guarded-tbuf] outer guard proves nested load range");
  if (!ok)
    return 1;
  if (!build_nested_sum(&f, false, false, false, true, false, false, false))
    return 1;
  ok = eval_nested_sum(&f, "strided-before", &nested_before) &&
       nested_before == 232 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-strided-after") &&
       eval_nested_sum(&f, "strided-after", &nested_after) &&
       nested_after == nested_before;
  saw_vector_load = false;
  for (size_t i = 0; ok && i < f.len; ++i)
    saw_vector_load |= f.data[i].op == NYIR_VEC4_LOAD_I64 ||
                       f.data[i].op == NYIR_VEC8_LOAD_I64;
  ok = ok && saw_vector_load;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-strided-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-strided-row] step-two rows keep inner loads vectorized");
  if (!ok)
    return 1;
  if (!build_nested_sum_config(&f, false, false, false, true, false, false,
                               false, true))
    return 1;
  ok = eval_nested_sum(&f, "inclusive-before", &nested_before) &&
       nested_before == 232 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-inclusive-after") &&
       eval_nested_sum(&f, "inclusive-after", &nested_after) &&
       nested_after == nested_before;
  saw_vector_load = false;
  for (size_t i = 0; ok && i < f.len; ++i)
    saw_vector_load |= f.data[i].op == NYIR_VEC4_LOAD_I64 ||
                       f.data[i].op == NYIR_VEC8_LOAD_I64;
  ok = ok && saw_vector_load;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-inclusive-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-inclusive-guard] <= outer bound proves strided rows");
  if (!ok)
    return 1;
  if (!build_nested_sum_config(&f, true, false, false, false, false, false,
                               false, true))
    return 1;
  ok = eval_nested_sum(&f, "inclusive-multi-before", &nested_before) &&
       nested_before == 136 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-inclusive-multi-after") &&
       eval_nested_sum(&f, "inclusive-multi-after", &nested_after) &&
       nested_after == nested_before;
  saw_vector_load = false;
  for (size_t i = 0; ok && i < f.len; ++i)
    saw_vector_load |= f.data[i].op == NYIR_VEC4_LOAD_I64 ||
                       f.data[i].op == NYIR_VEC8_LOAD_I64;
  ok = ok && saw_vector_load;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-inclusive-multi-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-inclusive-multi-latch] <= guard preserves both edges");
  if (!ok)
    return 1;
  if (!build_nested_sum_config(&f, true, false, false, true, false, false,
                               false, true))
    return 1;
  ok = eval_nested_sum(&f, "inclusive-strided-multi-before", &nested_before) &&
       nested_before == 232 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-inclusive-strided-multi-after") &&
       eval_nested_sum(&f, "inclusive-strided-multi-after", &nested_after) &&
       nested_after == nested_before;
  saw_vector_load = false;
  for (size_t i = 0; ok && i < f.len; ++i)
    saw_vector_load |= f.data[i].op == NYIR_VEC4_LOAD_I64 ||
                       f.data[i].op == NYIR_VEC8_LOAD_I64;
  ok = ok && saw_vector_load;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-inclusive-strided-multi-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-inclusive-strided-multi] uniform +2 edges vectorize");
  if (!ok)
    return 1;
  if (!build_nested_sum(&f, false, false, false, false, true, false, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  nested_effect_calls = 0;
  ok = eval_nested_sum(&f, "effect-before", &nested_before) &&
       nested_before == 136 && nested_effect_calls == 16;
  nested_effect_calls = 0;
  ok = ok && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-effect-after") &&
       eval_nested_sum(&f, "effect-after", &nested_after) &&
       nested_after == nested_before && nested_effect_calls == 16;
  for (size_t i = 0; ok && i < f.len; ++i)
    ok = f.data[i].op != NYIR_VEC4_LOAD_I64 &&
         f.data[i].op != NYIR_VEC8_LOAD_I64;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-effect-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-effect] unknown inner-loop effect keeps loads scalar");
  if (!ok)
    return 1;
  if (!build_nested_sum(&f, true, false, false, false, false, false, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  ok = eval_nested_sum(&f, "multi-latch-before", &nested_before) &&
       nested_before == 136 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-multi-latch-after") &&
       eval_nested_sum(&f, "multi-latch-after", &nested_after) &&
       nested_after == nested_before;
  saw_vector_load = false;
  for (size_t i = 0; ok && i < f.len; ++i)
    saw_vector_load |= f.data[i].op == NYIR_VEC4_LOAD_I64 ||
                       f.data[i].op == NYIR_VEC8_LOAD_I64;
  ok = ok && saw_vector_load;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-multi-latch-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts(
        "[loop-vector-multi-latch] uniform edge updates keep loads vectorized");
  if (!ok)
    return 1;
  if (!build_nested_sum(&f, true, false, false, false, false, true, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  ok = eval_nested_sum(&f, "mixed-latch-before", &nested_before) &&
       nested_before == 94 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-mixed-latch-after") &&
       eval_nested_sum(&f, "mixed-latch-after", &nested_after) &&
       nested_after == nested_before;
  for (size_t i = 0; ok && i < f.len; ++i)
    ok = f.data[i].op != NYIR_VEC4_LOAD_I64 &&
         f.data[i].op != NYIR_VEC8_LOAD_I64;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-mixed-latch-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-mixed-latch] differing edge updates stay scalar");
  if (!ok)
    return 1;
  if (!build_nested_sum(&f, false, false, false, false, false, false, true)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  reset_nested_values();
  ok = eval_nested_sum(&f, "alias-before", &nested_before) &&
       nested_before == 84;
  reset_nested_values();
  ok = ok && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-alias-after") &&
       eval_nested_sum(&f, "alias-after", &nested_after) &&
       nested_after == nested_before;
  for (size_t i = 0; ok && i < f.len; ++i)
    ok = f.data[i].op != NYIR_VEC4_LOAD_I64 &&
         f.data[i].op != NYIR_VEC8_LOAD_I64;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-alias-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-alias] shifted store cannot overlap vector loads");
  if (!ok)
    return 1;
  if (!build_nested_sum(&f, false, true, false, false, false, false, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  reset_nested_values();
  ok = eval_nested_sum(&f, "guard-bypass-before", &nested_before) &&
       nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-guard-bypass-after") &&
       eval_nested_sum(&f, "guard-bypass-after", &nested_after) &&
       nested_before == 136 && nested_after == nested_before;
  for (size_t i = 0; ok && i < f.len; ++i)
    ok = f.data[i].op != NYIR_VEC4_LOAD_I64 &&
         f.data[i].op != NYIR_VEC8_LOAD_I64;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-guard-bypass-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-guard-bypass] unguarded entry cannot borrow row bounds");
  if (!ok)
    return 1;
  if (!build_nested_sum(&f, false, false, true, false, false, false, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  reset_nested_values();
  ok = eval_nested_sum(&f, "overflow-before", &nested_before) &&
       nested_before == 52 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-overflow-after") &&
       eval_nested_sum(&f, "overflow-after", &nested_after) &&
       nested_after == nested_before;
  for (size_t i = 0; ok && i < f.len; ++i)
    ok = f.data[i].op != NYIR_VEC4_LOAD_I64 &&
         f.data[i].op != NYIR_VEC8_LOAD_I64;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-overflow-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-overflow] executed wrapping outer step stays scalar");
  if (!ok)
    return 1;
  if (!build_nested_sum_config(&f, false, false, true, false, false, false,
                               false, true))
    return 1;
  reset_nested_values();
  ok = eval_nested_sum(&f, "inclusive-overflow-before", &nested_before) &&
       nested_before == 52 && nyr_loop_vectorize(&f) &&
       nyr_opt_verify(&f, "loop-vector-inclusive-overflow-after") &&
       eval_nested_sum(&f, "inclusive-overflow-after", &nested_after) &&
       nested_after == nested_before;
  for (size_t i = 0; ok && i < f.len; ++i)
    ok = f.data[i].op != NYIR_VEC4_LOAD_I64 &&
         f.data[i].op != NYIR_VEC8_LOAD_I64;
  if (!ok)
    nyr_dump(stderr, &f, "loop-vector-inclusive-overflow-after");
  nyr_ir_func_free(&f);
  if (ok)
    puts("[loop-vector-inclusive-overflow] inclusive guard wrapping stays "
         "scalar");
  return ok ? 0 : 1;
}
