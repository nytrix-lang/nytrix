/*
 * Regression driver for SCEV-backed affine native-tbuf bounds elimination.
 */
#include <stdint.h>

#include "code/backend/ir/opt/loop.h"
#include "support.h"

static int emit_tbuf_new(nyr_func_t *f, int count, int width) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                    .dst = -1,
                                    .a = count,
                                    .b = width,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .symbol = "rt_tbuf_new_raw",
                                    .imm = 2});
}

static void emit_bounds_check(nyr_func_t *f, int base, int offset, int bytes) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                             .dst = -1,
                             .a = base,
                             .b = offset,
                             .c = bytes,
                             .d = -1,
                             .e = -1,
                             .f = -1});
}
static void build_matrix(nyr_func_t *f, bool overflow_adjacent) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int two = nyr_opt_const(f, 2);
  int three = nyr_opt_const(f, 3);
  int six = nyr_opt_const(f, 6);
  int width = nyr_opt_const(f, 8);
  int row_bytes = nyr_opt_const(f, overflow_adjacent ? INT64_MAX : 24);
  int base = emit_tbuf_new(f, six, width);

  /*
   * Build a list-length recurrence. Its exit value is the tbuf byte length.
   */
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int length = f->next_value;
  nyr_opt_phi(f, -1, zero, 0, zero); /* backedge patched below */
  int fill = nyr_opt_cmp(f, length, six, NYIR_CMP_LT);
  nyr_opt_br_if(f, fill, 11);
  nyr_opt_label(f, 20);
  int bytes = nyr_opt_binop(f, NYIR_MUL_I64, length, width);
  nyr_opt_br(f, 30);
  nyr_opt_label(f, 11);
  int length_next = nyr_opt_binop(f, NYIR_ADD_I64, length, one);
  nyr_opt_br(f, 10);

  /*
   * Nested i*width + j traversal.
   */
  nyr_opt_label(f, 30);
  int row = f->next_value;
  nyr_opt_phi(f, 20, zero, 0, zero); /* backedge patched below */
  int row_ok = nyr_opt_cmp(f, row, two, NYIR_CMP_LT);
  nyr_opt_br_if(f, row_ok, 31);
  nyr_opt_label(f, 50);
  nyr_opt_ret(f, zero);
  nyr_opt_label(f, 31);
  nyr_opt_br(f, 40);
  nyr_opt_label(f, 40);
  int column = f->next_value;
  nyr_opt_phi(f, 31, zero, 0, zero); /* backedge patched below */
  int column_ok =
      nyr_opt_cmp(f, column, overflow_adjacent ? two : three, NYIR_CMP_LT);
  nyr_opt_br_if(f, column_ok, 41);
  nyr_opt_label(f, 42);
  int row_next = nyr_opt_binop(f, NYIR_ADD_I64, row, one);
  nyr_opt_br(f, 30);
  nyr_opt_label(f, 41);
  int row_offset = nyr_opt_binop(f, NYIR_MUL_I64, row, row_bytes);
  int column_offset = nyr_opt_binop(f, NYIR_MUL_I64, column, width);
  int offset = nyr_opt_binop(f, NYIR_ADD_I64, row_offset, column_offset);
  emit_bounds_check(f, base, offset, bytes);
  int column_next = nyr_opt_binop(f, NYIR_ADD_I64, column, one);
  nyr_opt_br(f, 40);

  size_t phi = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    if (phi++ == 0)
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 11, .value = length_next};
    else if (phi == 2)
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 42, .value = row_next};
    else
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 41, .value = column_next};
  }
}

static void build_asserted_nested_tbuf_matrix(nyr_func_t *f,
                                              int64_t column_limit) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    abort();
  f->param_count = 1;
  f->param_types[0] = NYIR_PARAM_I64;
  int count = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
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
  int rows = nyr_opt_const(f, 100);
  int columns = nyr_opt_const(f, column_limit);
  int width = nyr_opt_const(f, 8);
  int base = emit_tbuf_new(f, count, width);
  int length = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                          .dst = -1,
                                          .a = base,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .symbol = "rt_len_strict",
                                          .imm = 1});
  int minimum = nyr_opt_const(f, 10000);
  int enough = nyr_opt_cmp(f, length, minimum, NYIR_CMP_GE);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                             .dst = -1,
                             .a = enough,
                             .b = zero,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .symbol = "rt_assert_cstr",
                             .imm = 2});

  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int row = f->next_value;
  nyr_opt_phi(f, -1, zero, 0, zero);
  int row_ok = nyr_opt_cmp(f, row, rows, NYIR_CMP_LT);
  nyr_opt_br_if(f, row_ok, 11);
  nyr_opt_br(f, 50);

  nyr_opt_label(f, 11);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 20);
  int column = f->next_value;
  nyr_opt_phi(f, 11, zero, 0, zero);
  int column_ok = nyr_opt_cmp(f, column, columns, NYIR_CMP_LT);
  nyr_opt_br_if(f, column_ok, 21);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 21);
  int row_offset = nyr_opt_binop(f, NYIR_MUL_I64, row, rows);
  int element_index = nyr_opt_binop(f, NYIR_ADD_I64, row_offset, column);
  int offset = nyr_opt_binop(f, NYIR_MUL_I64, element_index, width);
  int length_bytes = nyr_opt_binop(f, NYIR_MUL_I64, length, width);
  emit_bounds_check(f, base, offset, length_bytes);
  int column_next = nyr_opt_binop(f, NYIR_ADD_I64, column, one);
  nyr_opt_br(f, 20);

  nyr_opt_label(f, 30);
  int row_next = nyr_opt_binop(f, NYIR_ADD_I64, row, one);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 50);
  nyr_opt_ret(f, zero);

  size_t phi = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    if (phi++ == 0)
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 30, .value = row_next};
    else
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 21, .value = column_next};
  }
}

static void build_affine_constant(nyr_func_t *f, int64_t bound, bool use_shift,
                                  bool list_wrappers) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int eight = nyr_opt_const(f, 8);
  int three = nyr_opt_const(f, 3);
  int base;
  if (list_wrappers) {
    base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                      .dst = -1,
                                      .a = three,
                                      .b = -1,
                                      .c = -1,
                                      .d = -1,
                                      .e = -1,
                                      .f = -1,
                                      .symbol = "rt_list_new_raw",
                                      .imm = 1});
    int sized = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                           .dst = -1,
                                           .a = base,
                                           .b = three,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .symbol = "rt_list_set_len_raw",
                                           .imm = 2});
    base = sized;
  } else {
    base = emit_tbuf_new(f, three, eight);
  }
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 0, zero);
  int keep_going = nyr_opt_cmp(f, iv, three, NYIR_CMP_LT);
  nyr_opt_br_if(f, keep_going, 11);
  nyr_opt_ret(f, zero);
  nyr_opt_label(f, 11);
  int offset = use_shift ? nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                      .dst = -1,
                                                      .a = iv,
                                                      .b = nyr_opt_const(f, 3),
                                                      .c = -1,
                                                      .d = -1,
                                                      .e = -1,
                                                      .f = -1})
                         : nyr_opt_binop(f, NYIR_MUL_I64, iv, eight);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                             .dst = -1,
                             .a = base,
                             .b = offset,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = bound});
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_PHI) {
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 11, .value = next};
      break;
    }
  }
}

static int bounds_check_count(const nyr_func_t *f) {
  int count = 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_BOUNDS_CHECK)
      ++count;
  return count;
}

static bool check_case(const char *name, bool overflow_adjacent, int expected) {
  nyr_func_t f;
  build_matrix(&f, overflow_adjacent);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_bounds_check_elim(&f);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  int got = bounds_check_count(&f);
  if (got != expected) {
    fprintf(stderr, "[%s] bounds checks=%d want=%d\n", name, got, expected);
    nyr_dump(stderr, &f, name);
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_ir_func_free(&f);
  return true;
}

static bool check_affine_constant(const char *name, int64_t bound,
                                  bool use_shift, bool list_wrappers,
                                  int expected) {
  nyr_func_t f;
  build_affine_constant(&f, bound, use_shift, list_wrappers);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_bounds_check_elim(&f);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  int got = bounds_check_count(&f);
  if (got != expected) {
    fprintf(stderr, "[%s] bounds checks=%d want=%d\n", name, got, expected);
    nyr_dump(stderr, &f, name);
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_ir_func_free(&f);
  return true;
}

static void build_nested_dynamic_init(nyr_func_t *f, int64_t outer_start,
                                      int64_t outer_limit,
                                      bool check_false_edge) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int start = nyr_opt_const(f, outer_start);
  int limit = nyr_opt_const(f, outer_limit);
  int eight = nyr_opt_const(f, 8);
  int bytes = nyr_opt_const(f, 32);
  int base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 32,
                                        .scalar_class = NYIR_SCALAR_POINTER});
  for (int64_t i = 0; i < 4; ++i) {
    int offset = nyr_opt_const(f, i * 8);
    int value = nyr_opt_const(f, i + 1);
    int address = nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = address,
                               .b = -1,
                               .c = value,
                               .d = -1,
                               .e = -1,
                               .f = -1});
  }
  int initial_sum = nyr_opt_const(f, 0);
  nyr_ir_emit(f,
            (nyr_inst_t){
                .op = NYIR_STORE_LOCAL, .dst = -1, .a = initial_sum, .imm = 0});

  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  size_t outer_phi_at = f->len;
  int outer = f->next_value;
  nyr_opt_phi(f, -1, start, 30, zero);
  int outer_cond = nyr_opt_cmp(f, outer, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, outer_cond, 11);
  nyr_opt_br(f, 50);

  nyr_opt_label(f, 11);
  int inner_start = nyr_opt_binop(f, NYIR_SUB_I64, outer, one);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 20);
  size_t inner_phi_at = f->len;
  int inner = f->next_value;
  nyr_opt_phi(f, 11, inner_start, 21, zero);
  int inner_cond = nyr_opt_cmp(f, inner, zero, NYIR_CMP_GE);
  nyr_opt_br_if(f, inner_cond, 21);
  nyr_opt_br(f, check_false_edge ? 31 : 30);

  nyr_opt_label(f, 21);
  int offset = nyr_opt_binop(f, NYIR_MUL_I64, inner, eight);
  if (!check_false_edge)
    emit_bounds_check(f, base, offset, bytes);
  if (!check_false_edge) {
    int address = nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
    int value = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                           .dst = -1,
                                           .a = address,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
    int sum =
        nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .imm = 0});
    int next_sum = nyr_opt_binop(f, NYIR_ADD_I64, sum, value);
    nyr_ir_emit(f,
              (nyr_inst_t){
                  .op = NYIR_STORE_LOCAL, .dst = -1, .a = next_sum, .imm = 0});
  }
  int inner_next = nyr_opt_binop(f, NYIR_SUB_I64, inner, one);
  nyr_opt_br(f, 20);

  if (check_false_edge) {
    nyr_opt_label(f, 31);
    int false_offset = nyr_opt_binop(f, NYIR_MUL_I64, inner, eight);
    emit_bounds_check(f, base, false_offset, bytes);
    nyr_opt_br(f, 30);
  }
  nyr_opt_label(f, 30);
  int outer_next = nyr_opt_binop(f, NYIR_ADD_I64, outer, one);
  nyr_opt_br(f, 10);
  nyr_opt_label(f, 50);
  int result =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .imm = 0});
  nyr_opt_ret(f, result);

  f->data[outer_phi_at].phi_incoming[1] =
      (nyr_phi_incoming_t){.predecessor_label = 30, .value = outer_next};
  f->data[inner_phi_at].phi_incoming[1] =
      (nyr_phi_incoming_t){.predecessor_label = 21, .value = inner_next};
}

static bool eval_bounds_failure(nyr_func_t *f, const char *tag) {
  nyr_eval_result_t result = {0};
  int64_t locals[8] = {0};
  char err[256] = {0};
  bool ok = nyr_ir_eval(f, locals, 8, 500, &result, err, sizeof(err));
  nyr_eval_result_free(&result);
  if (ok || !strstr(err, "bounds check failed")) {
    fprintf(stderr, "[%s] expected an out-of-range bounds-check failure\n",
            tag);
    return false;
  }
  return true;
}

static bool check_nested_dynamic_init(const char *name, int64_t outer_start,
                                      int64_t outer_limit,
                                      bool check_false_edge, int expected,
                                      bool expect_bounds_failure,
                                      long long expected_result) {
  nyr_func_t f;
  build_nested_dynamic_init(&f, outer_start, outer_limit, check_false_edge);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  int *defs = malloc((size_t)f.next_value * sizeof(*defs));
  if (defs)
    for (size_t v = 0; v < (size_t)f.next_value; ++v)
      defs[v] = -1;
  for (size_t i = 0; defs && i < f.len; ++i)
    if (f.data[i].dst >= 0 && (size_t)f.data[i].dst < (size_t)f.next_value)
      defs[f.data[i].dst] = (int)i;
  nyr_cfg_t cfg = {0};
  nyr_scev_info_t scev = {0};
  nyr_scev_affine_t offset_range = {0};
  size_t check_at = SIZE_MAX;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].op == NYIR_BOUNDS_CHECK) {
      check_at = i;
      break;
    }
  bool analyzed =
      defs && nyr_cfg_build(&f, &cfg) && nyr_scev_analyze(&f, &scev) &&
      check_at < f.len &&
      nyr_scev_affine_at_cfg(&f, &scev, defs, &cfg, f.data[check_at].b,
                              check_at, &offset_range);
  free(defs);
  nyr_scev_free(&scev);
  nyr_cfg_free(&cfg);
  if ((expected == 0 &&
       (!analyzed || !offset_range.range.has_min ||
        !offset_range.range.has_max || offset_range.range.min < 0 ||
        offset_range.range.max >= 32)) ||
      (expected != 0 && analyzed)) {
    fprintf(stderr, "[%s] unexpected nested affine-range proof\n", name);
    nyr_ir_func_free(&f);
    return false;
  }
  long long before = 0;
  if (expect_bounds_failure) {
    if (!eval_bounds_failure(&f, name)) {
      nyr_ir_func_free(&f);
      return false;
    }
  } else {
    before = nyr_opt_eval(&f, name);
    if (before != expected_result) {
      fprintf(stderr, "[%s] before result=%lld want=%lld\n", name, before,
              expected_result);
      nyr_ir_func_free(&f);
      return false;
    }
  }
  if (!nyr_bounds_check_elim(&f) || !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  if (expect_bounds_failure) {
    if (!eval_bounds_failure(&f, name)) {
      nyr_ir_func_free(&f);
      return false;
    }
  } else {
    long long after = nyr_opt_eval(&f, name);
    if (after != before) {
      fprintf(stderr, "[%s] after result=%lld before=%lld\n", name, after,
              before);
      nyr_ir_func_free(&f);
      return false;
    }
  }
  int got = bounds_check_count(&f);
  if (got != expected) {
    fprintf(stderr, "[%s] bounds checks=%d want=%d\n", name, got, expected);
    nyr_dump(stderr, &f, name);
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_ir_func_free(&f);
  return true;
}

static void build_set_len_header_loop(nyr_func_t *f, bool mutate_length,
                                      bool borrow_wrapper, bool local_root,
                                      bool local_conflict, bool mutating_borrow,
                                      bool local_escape, bool mutating_append) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int three = nyr_opt_const(f, 3);
  int eight = nyr_opt_const(f, 8);
  int neg24 = nyr_opt_const(f, -24);
  int base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                        .dst = -1,
                                        .a = three,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .symbol = "rt_list_new_raw",
                                        .imm = 1});
  if (local_root) {
    (void)nyr_ir_emit(
        f,
        (nyr_inst_t){.op = NYIR_STORE_LOCAL, .dst = -1, .a = base, .imm = 0});
    base =
        nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .imm = 0});
  }
  if (local_escape) {
    (void)nyr_ir_emit(f,
                    (nyr_inst_t){.op = NYIR_ADDR_LOCAL, .dst = -1, .imm = 0});
  }
  if (borrow_wrapper) {
    nyr_inst_t borrow = {.op = NYIR_CALL,
                          .dst = -1,
                          .a = base,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .symbol = "wrap_borrow",
                          .imm = 1,
                          .flags = NYIR_INST_F_EFFECTS_KNOWN,
                          .effects = NYIR_EFFECT_NONE};
    nyr_call_arg_contract_set(&borrow, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW);
    base = nyr_ir_emit(f, borrow);
    f->data[f->len - 1].effects = NYIR_EFFECT_CALL;
  }
  if (mutating_borrow) {
    nyr_inst_t borrow = {.op = NYIR_CALL,
                          .dst = -1,
                          .a = base,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .symbol = "mutating_borrow",
                          .imm = 1,
                          .flags = NYIR_INST_F_EFFECTS_KNOWN,
                          .effects = NYIR_EFFECT_WRITE_MEMORY};
    nyr_call_arg_contract_set(&borrow, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW);
    base = nyr_ir_emit(f, borrow);
    f->data[f->len - 1].effects = NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY;
  }
  if (local_root) {
    (void)nyr_ir_emit(
        f,
        (nyr_inst_t){.op = NYIR_STORE_LOCAL, .dst = -1, .a = base, .imm = 0});
    if (local_conflict) {
      int other = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                             .dst = -1,
                                             .a = three,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .symbol = "rt_list_new_raw",
                                             .imm = 1});
      (void)nyr_ir_emit(
          f, (nyr_inst_t){
                 .op = NYIR_STORE_LOCAL, .dst = -1, .a = other, .imm = 0});
    }
    base =
        nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .imm = 0});
  }
  base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                    .dst = -1,
                                    .a = base,
                                    .b = three,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .symbol = "rt_list_set_len_raw",
                                    .imm = 2});
  /*
   * The read-only, trapping length observer cannot change the following
   * header value; keeping it explicit exercises effect-based call handling.
   */
  (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                   .dst = -1,
                                   .a = base,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .symbol = "rt_tbuf_len_raw",
                                   .imm = 1});
  if (mutating_append) {
    nyr_inst_t append = {.op = NYIR_CALL,
                          .dst = -1,
                          .a = base,
                          .b = one,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .symbol = "rt_tbuf_append_i64_raw",
                          .imm = 2,
                          .flags = NYIR_INST_F_EFFECTS_KNOWN,
                          .effects =
                              NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                              NYIR_EFFECT_WRITE_MEMORY |
                              NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP};
    nyr_call_arg_contract_set(&append, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW);
    base = nyr_ir_emit(f, append);
  }
  if (mutate_length) {
    nyr_inst_t mutate = {.op = NYIR_CALL,
                          .dst = -1,
                          .a = base,
                          .b = three,
                          .c = one,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .symbol = "rt_tbuf_set_i64_raw",
                          .imm = 3,
                          .flags = NYIR_INST_F_EFFECTS_KNOWN,
                          .effects = NYIR_EFFECT_WRITE_MEMORY};
    nyr_call_arg_contract_set(&mutate, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW);
    base = nyr_ir_emit(f, mutate);
    f->data[f->len - 1].effects = NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY;
  }

  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 0, zero);
  int keep_going = nyr_opt_cmp(f, iv, three, NYIR_CMP_LT);
  nyr_opt_br_if(f, keep_going, 11);
  nyr_opt_ret(f, zero);
  nyr_opt_label(f, 11);
  int header = nyr_opt_binop(f, NYIR_ADD_I64, base, neg24);
  int length = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = header,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  int bytes = nyr_opt_binop(f, NYIR_MUL_I64, length, eight);
  int offset = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_SHL_I64,
                                          .dst = -1,
                                          .a = iv,
                                          .b = nyr_opt_const(f, 3),
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  emit_bounds_check(f, base, offset, bytes);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_PHI) {
      f->data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 11, .value = next};
      break;
    }
}

static void build_loop_carried_append_unknown_length(nyr_func_t *f) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int three = nyr_opt_const(f, 3);
  int eight = nyr_opt_const(f, 8);
  int neg24 = nyr_opt_const(f, -24);
  int two = nyr_opt_const(f, 2);
  int initial = emit_tbuf_new(f, zero, eight);

  nyr_opt_br(f, 10);
  nyr_opt_label(f, 10);
  int iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 0, zero);
  int current = f->next_value;
  nyr_opt_phi(f, -1, initial, 0, initial);
  int keep_going = nyr_opt_cmp(f, iv, three, NYIR_CMP_LT);
  nyr_opt_br_if(f, keep_going, 11);

  nyr_opt_label(f, 20);
  int header = nyr_opt_binop(f, NYIR_ADD_I64, current, neg24);
  int length = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = header,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  int bytes = nyr_opt_binop(f, NYIR_MUL_I64, length, eight);
  int offset = nyr_opt_binop(f, NYIR_MUL_I64, two, eight);
  emit_bounds_check(f, current, offset, bytes);
  nyr_opt_ret(f, zero);

  nyr_opt_label(f, 11);
  nyr_inst_t append = {.op = NYIR_CALL,
                        .dst = -1,
                        .a = current,
                        .b = iv,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .symbol = "rt_tbuf_append_i64_raw",
                        .imm = 2,
                        .flags = NYIR_INST_F_EFFECTS_KNOWN,
                        .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_WRITE_MEMORY |
                                   NYIR_EFFECT_ALLOCATION |
                                   NYIR_EFFECT_MAY_TRAP};
  nyr_call_arg_contract_set(&append, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW);
  int appended = nyr_ir_emit(f, append);
  int next = nyr_opt_binop(f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(f, 10);

  size_t phi = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    int backedge = phi++ == 0 ? next : appended;
    f->data[i].phi_incoming[1] =
        (nyr_phi_incoming_t){.predecessor_label = 11, .value = backedge};
  }
}

static void build_post_load_append(nyr_func_t *f) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int three = nyr_opt_const(f, 3);
  int eight = nyr_opt_const(f, 8);
  int neg24 = nyr_opt_const(f, -24);
  int base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                        .dst = -1,
                                        .a = three,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .symbol = "rt_list_new_raw",
                                        .imm = 1});
  base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                    .dst = -1,
                                    .a = base,
                                    .b = three,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .symbol = "rt_list_set_len_raw",
                                    .imm = 2});
  int header = nyr_opt_binop(f, NYIR_ADD_I64, base, neg24);
  int length = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = header,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1});
  int bytes = nyr_opt_binop(f, NYIR_MUL_I64, length, eight);
  nyr_inst_t append = {.op = NYIR_CALL,
                        .dst = -1,
                        .a = base,
                        .b = one,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .symbol = "rt_tbuf_append_i64_raw",
                        .imm = 2,
                        .flags = NYIR_INST_F_EFFECTS_KNOWN,
                        .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_WRITE_MEMORY |
                                   NYIR_EFFECT_ALLOCATION |
                                   NYIR_EFFECT_MAY_TRAP};
  nyr_call_arg_contract_set(&append, 0, NYIR_ARG_CONTRACT_RETURNS_BORROW);
  (void)nyr_ir_emit(f, append);

  int in_bounds = nyr_opt_cmp(f, zero, three, NYIR_CMP_LT);
  nyr_opt_br_if(f, in_bounds, 11);
  nyr_opt_label(f, 20);
  nyr_opt_ret(f, zero);
  nyr_opt_label(f, 11);
  int offset = nyr_opt_binop(f, NYIR_MUL_I64, zero, eight);
  emit_bounds_check(f, base, offset, bytes);
  nyr_opt_ret(f, zero);
}

static bool check_post_load_append(void) {
  nyr_func_t f;
  build_post_load_append(&f);
  if (!nyr_opt_verify(&f, "bce/post-load-append/before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  if (!nyr_bounds_check_elim(&f) ||
      !nyr_opt_verify(&f, "bce/post-load-append/after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  int got = bounds_check_count(&f);
  if (got != 0)
    nyr_dump(stderr, &f, "bce/post-load-append/after");
  nyr_ir_func_free(&f);
  if (got != 0) {
    fprintf(stderr,
            "[bce/post-load-append] bounds checks=%d want=0; append occurs "
            "after the exact length load consumed by the guard\n",
            got);
    return false;
  }
  return true;
}

static bool check_loop_carried_append_stays_checked(void) {
  nyr_func_t f;
  build_loop_carried_append_unknown_length(&f);
  if (!nyr_opt_verify(&f, "bce/loop-carried-append/before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  if (!nyr_bounds_check_elim(&f) ||
      !nyr_opt_verify(&f, "bce/loop-carried-append/after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  int got = bounds_check_count(&f);
  nyr_ir_func_free(&f);
  if (got != 1) {
    fprintf(stderr,
            "[bce/loop-carried-append] bounds checks=%d want=1 for an "
            "append-built dynamic length\n",
            got);
    return false;
  }
  return true;
}

static void build_runtime_length_guard(nyr_func_t *f, int allocation_length,
                                       int guard_minimum, int index,
                                       bool false_edge, int mutation) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int neg24 = nyr_opt_const(f, -24);
  int count;
  if ((mutation >= 5 && mutation <= 10) || (mutation >= 11 && mutation <= 19)) {
    f->param_types = malloc(sizeof(*f->param_types));
    if (!f->param_types)
      abort();
    f->param_count = 1;
    f->param_types[0] = NYIR_PARAM_I64;
    count = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                       .dst = -1,
                                       .a = -1,
                                       .b = -1,
                                       .c = -1,
                                       .d = -1,
                                       .e = -1,
                                       .f = -1,
                                       .imm = 0});
  } else {
    count = nyr_opt_const(f, allocation_length);
  }
  int minimum = nyr_opt_const(f, guard_minimum);
  int index_constant = nyr_opt_const(f, index);
  int index_value = nyr_opt_binop(f, NYIR_ADD_I64, zero, index_constant);
  int width = nyr_opt_const(f, 8);
  int bytes_per_element = nyr_opt_const(f, 8);
  int base = emit_tbuf_new(f, count, width);
  bool strict_length = mutation >= 13 && mutation <= 19;
  int length =
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                 .dst = -1,
                                 .a = base,
                                 .b = -1,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .symbol = strict_length ? "rt_len_strict"
                                                         : "rt_tbuf_len_raw",
                                 .imm = 1});
  nyr_cmp_t guard_cmp = NYIR_CMP_GE;
  if (false_edge) {
    guard_cmp = mutation == 5   ? NYIR_CMP_EQ
                : mutation == 6 ? NYIR_CMP_LE
                : mutation == 7 ? NYIR_CMP_NE
                                : NYIR_CMP_LT;
  } else if (mutation == 8) {
    guard_cmp = NYIR_CMP_GT;
  } else if (mutation == 9) {
    guard_cmp = NYIR_CMP_EQ;
  }
  int guarded = mutation == 10 && false_edge
                    ? nyr_opt_cmp(f, minimum, length, NYIR_CMP_GT)
                    : nyr_opt_cmp(f, length, minimum, guard_cmp);
  bool assertion_guard = mutation >= 17;
  if (assertion_guard) {
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                     .dst = -1,
                                     .a = guarded,
                                     .b = zero,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .symbol = "rt_assert_cstr",
                                     .imm = 2});
  } else {
    nyr_opt_br_if(f, guarded, false_edge ? 20 : 11);
    if (!false_edge) {
      nyr_opt_label(f, 20);
      if (mutation == 4)
        nyr_opt_br(f, 11);
      else
        nyr_opt_ret(f, zero);
      nyr_opt_label(f, 11);
    } else {
      nyr_opt_label(f, 11);
    }
  }
  if (assertion_guard)
    nyr_opt_label(f, 30);
  if (mutation == 1) {
    base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                      .dst = -1,
                                      .a = base,
                                      .b = zero,
                                      .c = -1,
                                      .d = -1,
                                      .e = -1,
                                      .f = -1,
                                      .symbol = "rt_list_set_len_raw",
                                      .imm = 2});
  } else if (mutation == 2 || mutation == 16 || mutation == 19) {
    base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                      .dst = -1,
                                      .a = base,
                                      .b = one,
                                      .c = -1,
                                      .d = -1,
                                      .e = -1,
                                      .f = -1,
                                      .symbol = "rt_tbuf_append_i64_raw",
                                      .imm = 2});
  } else if (mutation == 3 || mutation == 15 || mutation == 18) {
    (void)nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                     .flags = NYIR_INST_F_EFFECTS_KNOWN,
                                     .effects = NYIR_EFFECT_CALL |
                                                NYIR_EFFECT_WRITE_MEMORY |
                                                NYIR_EFFECT_UNKNOWN_SIDE_EFFECT,
                                     .dst = -1,
                                     .a = base,
                                     .b = zero,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1,
                                     .symbol = "unknown_mutator",
                                     .imm = 2});
  }
  int header = nyr_opt_binop(f, NYIR_ADD_I64, base, neg24);
  int length_now = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                              .dst = -1,
                                              .a = header,
                                              .b = -1,
                                              .c = -1,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1});
  int bytes = nyr_opt_binop(f, NYIR_MUL_I64, length_now, bytes_per_element);
  int offset = nyr_opt_binop(f, NYIR_MUL_I64, index_value, width);
  emit_bounds_check(f, base, offset, bytes);
  nyr_opt_ret(f, zero);
  if (false_edge) {
    nyr_opt_label(f, 20);
    nyr_opt_ret(f, zero);
  }
}

static bool check_runtime_length_guard(const char *name, int allocation_length,
                                       int guard_minimum, int index,
                                       bool false_edge, int mutation,
                                       int expected_checks) {
  nyr_func_t f;
  build_runtime_length_guard(&f, allocation_length, guard_minimum, index,
                             false_edge, mutation);
  if (!nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_cfg_t cfg = {0};
  bool cfg_ok = nyr_cfg_build(&f, &cfg);
  nyr_cfg_free(&cfg);
  if (!cfg_ok) {
    fprintf(stderr, "[%s] expected a valid CFG for guard analysis\n", name);
    nyr_ir_func_free(&f);
    return false;
  }
  if (!nyr_bounds_check_elim(&f) || !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  int got = bounds_check_count(&f);
  if (got != expected_checks) {
    fprintf(stderr, "[%s] bounds checks=%d want=%d\n", name, got,
            expected_checks);
    nyr_dump(stderr, &f, name);
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_ir_func_free(&f);
  return true;
}

static bool check_asserted_nested_tbuf_matrix(const char *name,
                                              int64_t column_limit,
                                              int expected_checks) {
  nyr_func_t f;
  build_asserted_nested_tbuf_matrix(&f, column_limit);
  if (!nyr_opt_verify(&f, name) || !nyr_bounds_check_elim(&f) ||
      !nyr_opt_verify(&f, name)) {
    nyr_ir_func_free(&f);
    return false;
  }
  int got = bounds_check_count(&f);
  if (got != expected_checks) {
    fprintf(stderr, "[%s] bounds checks=%d want=%d\n", name, got,
            expected_checks);
    nyr_dump(stderr, &f, name);
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_ir_func_free(&f);
  return true;
}

static bool check_set_len_header_forwarding(void) {
  for (int mutate = 0; mutate <= 1; ++mutate) {
    for (int borrow = 0; borrow <= 1; ++borrow) {
      for (int local = 0; local <= 1; ++local) {
        for (int conflict = 0; conflict <= local; ++conflict) {
          for (int mutating_borrow = 0; mutating_borrow <= 1;
               ++mutating_borrow) {
            for (int local_escape = 0; local_escape <= local; ++local_escape) {
              for (int mutating_append = 0; mutating_append <= 1;
                   ++mutating_append) {
                nyr_func_t f;
                build_set_len_header_loop(
                    &f, mutate != 0, borrow != 0, local != 0, conflict != 0,
                    mutating_borrow != 0, local_escape != 0,
                    mutating_append != 0);
                if (!nyr_opt_verify(&f, "bce/set-len-header/before")) {
                  nyr_ir_func_free(&f);
                  return false;
                }
                if (!nyr_bounds_check_elim(&f) ||
                    !nyr_opt_verify(&f, "bce/set-len-header/after")) {
                  nyr_ir_func_free(&f);
                  return false;
                }
                int got = bounds_check_count(&f);
                int expected = mutate || conflict || mutating_borrow ||
                                       local_escape || mutating_append
                                   ? 1
                                   : 0;
                nyr_ir_func_free(&f);
                if (got != expected) {
                  fprintf(stderr,
                          "[bce/set-len-header/%s/%s/local=%d/conflict=%d/"
                          "mutating-borrow=%d/escape=%d/append=%d] "
                          "bounds checks=%d want=%d\n",
                          mutate ? "mutated" : "private",
                          borrow ? "borrowed" : "direct", local, conflict,
                          mutating_borrow, local_escape, mutating_append, got,
                          expected);
                  return false;
                }
              }
            }
          }
        }
      }
    }
  }
  return true;
}

/*
 * Native lowering copies a managed-buffer base inside a loop before
 * constructing its indexed address.  IRCE must look through that harmless
 * copy and hoist the checked maximum offset to the loop preheader.
 */
static bool check_copied_base_reverse_index(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int three = nyr_opt_const(&f, 3);
  int four = nyr_opt_const(&f, 4);
  int eight = nyr_opt_const(&f, 8);
  int base = emit_tbuf_new(&f, four, eight);
  int header_offset = nyr_opt_const(&f, -24);
  int header = nyr_opt_binop(&f, NYIR_ADD_I64, base, header_offset);
  int length = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                           .dst = -1,
                                           .a = header,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1});
  int bytes = nyr_opt_binop(&f, NYIR_MUL_I64, length, eight);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  int iv = f.next_value;
  nyr_opt_phi(&f, -1, zero, 11, zero);
  int keep_going = nyr_opt_cmp(&f, iv, four, NYIR_CMP_LT);
  nyr_opt_br_if(&f, keep_going, 11);
  nyr_opt_label(&f, 20);
  nyr_opt_ret(&f, zero);
  nyr_opt_label(&f, 11);
  int copied_base = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_COPY,
                                                .dst = -1,
                                                .a = base,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1});
  int reverse = nyr_opt_binop(&f, NYIR_SUB_I64, three, iv);
  int offset = nyr_opt_binop(&f, NYIR_SHL_I64, reverse, nyr_opt_const(&f, 3));
  emit_bounds_check(&f, copied_base, offset, bytes);
  int next = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&f, 10);
  for (size_t i = 0; i < f.len; ++i) {
    if (f.data[i].op == NYIR_PHI) {
      f.data[i].phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = 11, .value = next};
      break;
    }
  }
  if (!nyr_opt_verify(&f, "bce/copied-base-reverse-before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  if (!nyr_irce(&f)) {
    nyr_ir_func_free(&f);
    return false;
  }
  if (!nyr_opt_verify(&f, "bce/copied-base-reverse-after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  bool hoisted = false;
  size_t loop_header = SIZE_MAX;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].op == NYIR_LABEL && f.data[i].imm == 10) {
      loop_header = i;
      break;
    }
  for (size_t i = 0; i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->op != NYIR_BOUNDS_CHECK)
      continue;
    int64_t max_offset = -1;
    for (size_t d = 0; d < f.len; ++d)
      if (in->b >= 0 && f.data[d].dst == in->b &&
          f.data[d].op == NYIR_CONST_I64)
        max_offset = f.data[d].imm;
    if (in->a == base && in->c == bytes && max_offset == 24 && i < loop_header)
      hoisted = true;
  }
  if (!hoisted) {
    fprintf(stderr, "[bce/copied-base-reverse] invariant check not hoisted\n");
    nyr_dump(stderr, &f, "bce/copied-base-reverse");
  }
  nyr_ir_func_free(&f);
  return hoisted;
}

static int build_normalized_local(nyr_func_t *f, int64_t initial) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int len = nyr_opt_const(f, 8);
  int seed = nyr_opt_const(f, initial);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = seed,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 0});
  int index = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 0});
  int negative = nyr_opt_cmp(f, index, zero, NYIR_CMP_LT);
  int wrapped = nyr_opt_binop(f, NYIR_ADD_I64, index, len);
  int normalized = nyr_opt_select(f, negative, wrapped, index);
  nyr_opt_ret(f, normalized);
  return normalized;
}

static void build_guarded_normalization(nyr_func_t *f, int *normalized_out) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int len = nyr_opt_const(f, 8);
  int negative_result = nyr_opt_const(f, -77);
  int guard_index = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = 0});
  int nonnegative = nyr_opt_cmp(f, guard_index, zero, NYIR_CMP_GE);
  nyr_opt_br_if(f, nonnegative, 10);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 10);
  int index = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 0});
  int is_negative = nyr_opt_cmp(f, index, zero, NYIR_CMP_LT);
  int wrapped = nyr_opt_binop(f, NYIR_ADD_I64, index, len);
  int normalized = nyr_opt_select(f, is_negative, wrapped, index);
  nyr_opt_ret(f, normalized);
  nyr_opt_label(f, 20);
  nyr_opt_ret(f, negative_result);
  *normalized_out = normalized;
}

static int build_normalization_with_alternate_pred(nyr_func_t *f) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int len = nyr_opt_const(f, 8);
  int index = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 0});
  int nonnegative = nyr_opt_cmp(f, index, zero, NYIR_CMP_GE);
  nyr_opt_br_if(f, nonnegative, 10);
  nyr_opt_br(f, 20);
  nyr_opt_label(f, 20);
  nyr_opt_br(f, 10); /* Bypass the guard into the same select block. */
  nyr_opt_label(f, 10);
  int selected_index = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                                  .dst = -1,
                                                  .a = -1,
                                                  .b = -1,
                                                  .c = -1,
                                                  .d = -1,
                                                  .e = -1,
                                                  .f = -1,
                                                  .imm = 0});
  int is_negative = nyr_opt_cmp(f, selected_index, zero, NYIR_CMP_LT);
  int wrapped = nyr_opt_binop(f, NYIR_ADD_I64, selected_index, len);
  int normalized = nyr_opt_select(f, is_negative, wrapped, selected_index);
  nyr_opt_ret(f, normalized);
  return normalized;
}

static bool eval_with_local0(nyr_func_t *f, int64_t local0, long long *out) {
  int64_t locals[8] = {local0};
  nyr_eval_result_t result = {0};
  char err[256] = {0};
  if (!nyr_ir_eval(f, locals, 8, 500, &result, err, sizeof(err))) {
    fprintf(stderr, "[bce/guarded-normalization] eval failed: %s\n", err);
    return false;
  }
  *out = (long long)result.result;
  nyr_eval_result_free(&result);
  return true;
}

static bool check_nonnegative_normalization(void) {
  nyr_func_t f;
  int normalized = build_normalized_local(&f, 0);
  if (!nyr_opt_verify(&f, "bce/nonnegative-normalization-before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long before = nyr_opt_eval(&f, "bce/nonnegative-normalization-before");
  nyr_bounds_check_elim(&f);
  if (!nyr_opt_verify(&f, "bce/nonnegative-normalization-after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long after = nyr_opt_eval(&f, "bce/nonnegative-normalization-after");
  bool simplified = false;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].dst == normalized && f.data[i].op == NYIR_COPY)
      simplified = true;
  nyr_ir_func_free(&f);
  if (before != 0 || after != before || !simplified) {
    fprintf(stderr,
            "[bce/nonnegative-normalization] before=%lld after=%lld "
            "simplified=%d\n",
            before, after, simplified ? 1 : 0);
    return false;
  }

  normalized = build_normalized_local(&f, -1);
  if (!nyr_opt_verify(&f, "bce/negative-normalization-before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  before = nyr_opt_eval(&f, "bce/negative-normalization-before");
  nyr_bounds_check_elim(&f);
  if (!nyr_opt_verify(&f, "bce/negative-normalization-after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  after = nyr_opt_eval(&f, "bce/negative-normalization-after");
  simplified = false;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].dst == normalized && f.data[i].op == NYIR_COPY)
      simplified = true;
  nyr_ir_func_free(&f);
  if (before != 7 || after != before || simplified) {
    fprintf(
        stderr,
        "[bce/negative-normalization] before=%lld after=%lld simplified=%d\n",
        before, after, simplified ? 1 : 0);
    return false;
  }

  int guarded_normalized = -1;
  build_guarded_normalization(&f, &guarded_normalized);
  if (!nyr_opt_verify(&f, "bce/guarded-normalization-before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long positive_before = 0, negative_before = 0;
  if (!eval_with_local0(&f, 5, &positive_before) ||
      !eval_with_local0(&f, -1, &negative_before)) {
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_bounds_check_elim(&f);
  if (!nyr_opt_verify(&f, "bce/guarded-normalization-after")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long positive_after = 0, negative_after = 0;
  if (!eval_with_local0(&f, 5, &positive_after) ||
      !eval_with_local0(&f, -1, &negative_after)) {
    nyr_ir_func_free(&f);
    return false;
  }
  simplified = false;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].dst == guarded_normalized && f.data[i].op == NYIR_COPY)
      simplified = true;
  nyr_ir_func_free(&f);
  if (positive_before != 5 || positive_after != positive_before ||
      negative_before != -77 || negative_after != negative_before ||
      !simplified) {
    fprintf(stderr,
            "[bce/guarded-normalization] positive=%lld/%lld negative=%lld/%lld "
            "simplified=%d\n",
            positive_before, positive_after, negative_before, negative_after,
            simplified ? 1 : 0);
    return false;
  }

  int alternate_normalized = build_normalization_with_alternate_pred(&f);
  if (!nyr_opt_verify(&f, "bce/alternate-pred-normalization-before")) {
    nyr_ir_func_free(&f);
    return false;
  }
  long long alternate_before = 0, alternate_after = 0;
  if (!eval_with_local0(&f, -1, &alternate_before)) {
    nyr_ir_func_free(&f);
    return false;
  }
  nyr_bounds_check_elim(&f);
  if (!nyr_opt_verify(&f, "bce/alternate-pred-normalization-after") ||
      !eval_with_local0(&f, -1, &alternate_after)) {
    nyr_ir_func_free(&f);
    return false;
  }
  simplified = false;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].dst == alternate_normalized && f.data[i].op == NYIR_COPY)
      simplified = true;
  nyr_ir_func_free(&f);
  if (alternate_before != 7 || alternate_after != alternate_before ||
      simplified) {
    fprintf(stderr,
            "[bce/alternate-pred-normalization] before=%lld after=%lld "
            "simplified=%d\n",
            alternate_before, alternate_after, simplified ? 1 : 0);
    return false;
  }
  return true;
}

int main(void) {
  return check_case("bce-matrix/affine", false, 0) &&
                 check_case("bce-matrix/overflow", true, 1) &&
                 check_affine_constant("bce-matrix/fixed-bound", 24, false,
                                       false, 0) &&
                 check_affine_constant("bce-matrix/fixed-bound-edge", 16, false,
                                       false, 1) &&
                 check_nested_dynamic_init("bce/nested-dynamic-init", 1, 4,
                                           false, 0, false, 10) &&
                 check_nested_dynamic_init("bce/nested-negative-init", 0, 2,
                                           false, 0, false, 1) &&
                 check_nested_dynamic_init("bce/nested-init-overflow",
                                           INT64_MIN, INT64_MIN + 2, false, 1,
                                           true, 0) &&
                 check_nested_dynamic_init("bce/nested-false-edge", 1, 4, true,
                                           1, true, 0) &&
                 check_affine_constant("bce-matrix/fixed-bound-shift", 24, true,
                                       true, 0) &&
                 check_affine_constant("bce-matrix/fixed-bound-shift-edge", 16,
                                       true, true, 1) &&
                 check_set_len_header_forwarding() &&
                 check_loop_carried_append_stays_checked() &&
                 check_runtime_length_guard("bce/runtime-length-guard", 4, 4, 3,
                                            false, 0, 0) &&
                 check_runtime_length_guard("bce/runtime-length-guard-edge", 3,
                                            3, 3, false, 1, 1) &&
                 check_runtime_length_guard("bce/runtime-length-guard-false", 3,
                                            3, 2, true, 0, 0) &&
                 check_runtime_length_guard("bce/runtime-length-false-le", 3, 2,
                                            2, true, 6, 0) &&
                 check_runtime_length_guard("bce/runtime-length-false-ne", 3, 3,
                                            2, true, 7, 0) &&
                 check_runtime_length_guard(
                     "bce/runtime-length-reversed-false-gt", 3, 3, 2, true, 10,
                     0) &&
                 check_runtime_length_guard(
                     "bce/runtime-length-guard-equal-false", 3, 4, 2, true, 5,
                     1) &&
                 check_runtime_length_guard("bce/runtime-length-true-gt", 3, 2,
                                            2, false, 8, 0) &&
                 check_runtime_length_guard("bce/runtime-length-true-eq", 3, 3,
                                            2, false, 9, 0) &&
                 check_runtime_length_guard("bce/runtime-length-shrink", 4, 4,
                                            3, false, 1, 1) &&
                 check_runtime_length_guard("bce/runtime-length-append", 4, 4,
                                            3, false, 2, 0) &&
                 check_runtime_length_guard(
                     "bce/runtime-length-both-edges-reach", 4, 4, 3, false, 4,
                     1) &&
                 check_runtime_length_guard("bce/runtime-length-unknown-alias",
                                            4, 4, 3, false, 3, 1) &&
                 check_runtime_length_guard(
                     "bce/runtime-length-dynamic-lower-bound", 0, 8, 7, false,
                     11, 0) &&
                 check_runtime_length_guard(
                     "bce/runtime-length-dynamic-boundary", 0, 8, 8, false, 12,
                     1) &&
                 check_runtime_length_guard(
                     "bce/runtime-length-strict-lower-bound", 0, 8, 7, false,
                     13, 0) &&
                 check_runtime_length_guard(
                     "bce/runtime-length-strict-boundary", 0, 8, 8, false, 14,
                     1) &&
                 check_runtime_length_guard(
                     "bce/runtime-length-strict-false-edge", 0, 8, 7, true, 13,
                     0) &&
                 check_runtime_length_guard(
                     "bce/runtime-length-strict-unknown-mutation", 0, 8, 7,
                     false, 15, 1) &&
                 check_runtime_length_guard("bce/runtime-length-strict-append",
                                            0, 8, 7, false, 16, 0) &&
                 check_runtime_length_guard("bce/runtime-length-assert", 0, 8,
                                            7, false, 17, 0) &&
                 check_runtime_length_guard(
                     "bce/runtime-length-assert-unknown-mutation", 0, 8, 7,
                     false, 18, 1) &&
                 check_runtime_length_guard("bce/runtime-length-assert-append",
                                            0, 8, 7, false, 19, 0) &&
                 check_asserted_nested_tbuf_matrix(
                     "bce/runtime-length-assert-nested-matrix", 100, 0) &&
                 check_asserted_nested_tbuf_matrix(
                     "bce/runtime-length-assert-nested-boundary", 101, 1) &&
                 check_post_load_append() &&
                 check_copied_base_reverse_index() &&
                 check_nonnegative_normalization()
             ? 0
             : 1;
}
