/*
 * Regression driver for SCEV-backed affine native-tbuf bounds elimination.
 */
#include <stdint.h>

#include "util.h"

static int emit_tbuf_new(nyir_func_t *f, int count, int width) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_CALL, .dst = -1,
                                    .a = count, .b = width, .c = -1,
                                    .d = -1, .e = -1, .f = -1,
                                    .symbol = "rt_tbuf_new_raw", .imm = 2});
}

static void emit_bounds_check(nyir_func_t *f, int base, int offset, int bytes) {
  nyir_emit(f, (nyir_inst_t){.op = NYIR_BOUNDS_CHECK, .dst = -1,
                             .a = base, .b = offset, .c = bytes,
                             .d = -1, .e = -1, .f = -1});
}
static void build_matrix(nyir_func_t *f, bool overflow_adjacent) {
  ny_begin(f);
  int zero = ny_const(f, 0);
  int one = ny_const(f, 1);
  int two = ny_const(f, 2);
  int three = ny_const(f, 3);
  int six = ny_const(f, 6);
  int width = ny_const(f, 8);
  int row_bytes = ny_const(f, overflow_adjacent ? INT64_MAX : 24);
  int base = emit_tbuf_new(f, six, width);

  /*
   * Build a list-length recurrence. Its exit value is the tbuf byte length.
   */
  ny_br(f, 10);
  ny_label(f, 10);
  int length = f->next_value;
  ny_phi(f, -1, zero, 0, zero); /* backedge patched below */
  int fill = ny_cmp(f, length, six, NYIR_CMP_LT);
  ny_br_if(f, fill, 11);
  ny_label(f, 20);
  int bytes = ny_binop(f, NYIR_MUL_I64, length, width);
  ny_br(f, 30);
  ny_label(f, 11);
  int length_next = ny_binop(f, NYIR_ADD_I64, length, one);
  ny_br(f, 10);

  /*
   * Nested i*width + j traversal.
   */
  ny_label(f, 30);
  int row = f->next_value;
  ny_phi(f, 20, zero, 0, zero); /* backedge patched below */
  int row_ok = ny_cmp(f, row, two, NYIR_CMP_LT);
  ny_br_if(f, row_ok, 31);
  ny_label(f, 50);
  ny_ret(f, zero);
  ny_label(f, 31);
  ny_br(f, 40);
  ny_label(f, 40);
  int column = f->next_value;
  ny_phi(f, 31, zero, 0, zero); /* backedge patched below */
  int column_ok = ny_cmp(f, column, overflow_adjacent ? two : three,
                         NYIR_CMP_LT);
  ny_br_if(f, column_ok, 41);
  ny_label(f, 42);
  int row_next = ny_binop(f, NYIR_ADD_I64, row, one);
  ny_br(f, 30);
  ny_label(f, 41);
  int row_offset = ny_binop(f, NYIR_MUL_I64, row, row_bytes);
  int column_offset = ny_binop(f, NYIR_MUL_I64, column, width);
  int offset = ny_binop(f, NYIR_ADD_I64, row_offset, column_offset);
  emit_bounds_check(f, base, offset, bytes);
  int column_next = ny_binop(f, NYIR_ADD_I64, column, one);
  ny_br(f, 40);

  size_t phi = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    if (phi++ == 0)
      f->data[i].phi_incoming[1] =
          (nyir_phi_incoming_t){.predecessor_label = 11, .value = length_next};
    else if (phi == 2)
      f->data[i].phi_incoming[1] =
          (nyir_phi_incoming_t){.predecessor_label = 42, .value = row_next};
    else
      f->data[i].phi_incoming[1] = (nyir_phi_incoming_t){
          .predecessor_label = 41, .value = column_next};
  }
}

static int bounds_check_count(const nyir_func_t *f) {
  int count = 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_BOUNDS_CHECK)
      ++count;
  return count;
}

static bool check_case(const char *name, bool overflow_adjacent, int expected) {
  nyir_func_t f;
  build_matrix(&f, overflow_adjacent);
  if (!ny_verify(&f, name)) {
    nyir_func_free(&f);
    return false;
  }
  nyir_bounds_check_elim(&f);
  if (!ny_verify(&f, name)) {
    nyir_func_free(&f);
    return false;
  }
  int got = bounds_check_count(&f);
  if (got != expected) {
    fprintf(stderr, "[%s] bounds checks=%d want=%d\n", name, got, expected);
    nyir_dump(stderr, &f, name);
    nyir_func_free(&f);
    return false;
  }
  nyir_func_free(&f);
  return true;
}

int main(void) {
  return check_case("bce-matrix/affine", false, 0) &&
                 check_case("bce-matrix/overflow", true, 1)
             ? 0
             : 1;
}
