/*
 * Guard loop interchange against induction values escaping through exit PHIs.
 */
#include "support.h"
#include <limits.h>

extern bool nyr_loop_interchange(nyr_func_t *f);

static bool build_exit_phi_escape(nyr_func_t *f) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int outer_limit = nyr_opt_const(f, 2);
  int inner_limit = nyr_opt_const(f, 4);
  int take_left = nyr_opt_const(f, 1);
  int fallback = nyr_opt_const(f, 99);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t outer_phi_at = f->len;
  int outer_iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 20, zero);
  int outer_cond = nyr_opt_cmp(f, outer_iv, outer_limit, NYIR_CMP_LE);
  nyr_opt_br_if(f, outer_cond, 11);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 11);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 12);
  size_t inner_phi_at = f->len;
  int inner_iv = f->next_value;
  nyr_opt_phi(f, 11, zero, 13, zero);
  int inner_cond = nyr_opt_cmp(f, inner_iv, inner_limit, NYIR_CMP_LE);
  nyr_opt_br_if(f, inner_cond, 13);
  nyr_opt_br(f, 20);

  nyr_opt_label(f, 13);
  int next_inner = nyr_opt_binop(f, NYIR_ADD_I64, inner_iv, one);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 20);
  int next_outer = nyr_opt_binop(f, NYIR_ADD_I64, outer_iv, one);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 30);
  nyr_opt_br_if(f, take_left, 31);
  nyr_opt_br(f, 32);
  nyr_opt_label(f, 31);
  nyr_opt_br(f, 33);
  nyr_opt_label(f, 32);
  nyr_opt_br(f, 33);
  nyr_opt_label(f, 33);
  size_t result_phi_at = f->len;
  int result = f->next_value;
  nyr_opt_phi(f, 31, outer_iv, 32, fallback);
  nyr_opt_ret(f, result);

  f->data[outer_phi_at].phi_incoming[1].value = next_outer;
  f->data[inner_phi_at].phi_incoming[1].value = next_inner;
  f->data[result_phi_at].phi_incoming[0].value = outer_iv;
  return nyr_opt_verify(f, "loop-interchange-exit-phi-before");
}

static bool build_local_access_nest(nyr_func_t *f, bool write_in_loop,
                                    bool extra_carried_phi,
                                    size_t *outer_cmp_at,
                                    size_t *inner_cmp_at) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int outer_limit = nyr_opt_const(f, 2);
  int inner_limit = nyr_opt_const(f, 4);
  int local_value = nyr_opt_const(f, write_in_loop ? 0 : 5);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = local_value,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 0});
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t outer_phi_at = f->len;
  int outer_iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 20, zero);
  *outer_cmp_at = f->len;
  int outer_cond = nyr_opt_cmp(f, outer_iv, outer_limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, outer_cond, 11);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 11);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 12);
  size_t inner_phi_at = f->len;
  int inner_iv = f->next_value;
  nyr_opt_phi(f, 11, zero, 13, zero);
  size_t carried_phi_at = SIZE_MAX;
  int carried = -1;
  if (extra_carried_phi) {
    carried_phi_at = f->len;
    carried = f->next_value;
    nyr_opt_phi(f, 11, zero, 13, zero);
  }
  *inner_cmp_at = f->len;
  int inner_cond = nyr_opt_cmp(f, inner_iv, inner_limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, inner_cond, 13);
  nyr_opt_br(f, 20);

  nyr_opt_label(f, 13);
  int local_read = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                              .dst = -1,
                                              .a = -1,
                                              .b = -1,
                                              .c = -1,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1,
                                              .imm = 0});
  if (write_in_loop) {
    int local_next = nyr_opt_binop(f, NYIR_ADD_I64, local_read, one);
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                               .dst = -1,
                               .a = local_next,
                               .b = -1,
                               .c = -1,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .imm = 0});
  }
  int carried_next = -1;
  if (extra_carried_phi)
    carried_next = nyr_opt_binop(f, NYIR_ADD_I64, carried, one);
  int next_inner = nyr_opt_binop(f, NYIR_ADD_I64, inner_iv, one);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 20);
  int next_outer = nyr_opt_binop(f, NYIR_ADD_I64, outer_iv, one);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 30);
  int result = write_in_loop ? nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                                          .dst = -1,
                                                          .a = -1,
                                                          .b = -1,
                                                          .c = -1,
                                                          .d = -1,
                                                          .e = -1,
                                                          .f = -1,
                                                          .imm = 0})
                             : outer_iv;
  nyr_opt_ret(f, result);
  f->data[outer_phi_at].phi_incoming[1].value = next_outer;
  f->data[inner_phi_at].phi_incoming[1].value = next_inner;
  if (extra_carried_phi)
    f->data[carried_phi_at].phi_incoming[1].value = carried_next;
  const char *before_name =
      write_in_loop       ? "loop-interchange-local-write-before"
      : extra_carried_phi ? "loop-interchange-carried-phi-before"
                          : "loop-interchange-read-only-local-before";
  return nyr_opt_verify(f, before_name);
}

static bool build_nonterminating_nest(nyr_func_t *f, size_t *outer_cmp_at,
                                      size_t *inner_cmp_at) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int negative_one = nyr_opt_const(f, -1);
  int outer_limit = nyr_opt_const(f, 0);
  int inner_limit = nyr_opt_const(f, 1);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t outer_phi_at = f->len;
  int outer_iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 20, zero);
  *outer_cmp_at = f->len;
  int outer_cond = nyr_opt_cmp(f, outer_iv, outer_limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, outer_cond, 11);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 11);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 12);
  size_t inner_phi_at = f->len;
  int inner_iv = f->next_value;
  nyr_opt_phi(f, 11, zero, 13, zero);
  *inner_cmp_at = f->len;
  int inner_cond = nyr_opt_cmp(f, inner_iv, inner_limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, inner_cond, 13);
  nyr_opt_br(f, 20);

  nyr_opt_label(f, 13);
  int next_inner = nyr_opt_binop(f, NYIR_ADD_I64, inner_iv, negative_one);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 20);
  int next_outer = nyr_opt_binop(f, NYIR_ADD_I64, outer_iv, negative_one);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 30);
  nyr_opt_ret(f, outer_iv);
  f->data[outer_phi_at].phi_incoming[1].value = next_outer;
  f->data[inner_phi_at].phi_incoming[1].value = next_inner;
  return nyr_opt_verify(f, "loop-interchange-nonterminating-before");
}

static bool build_overflowing_nest(nyr_func_t *f, bool underflow,
                                   size_t *outer_cmp_at, size_t *inner_cmp_at) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int outer_start = nyr_opt_const(f, underflow ? INT64_MIN : INT64_MAX);
  int outer_step = nyr_opt_const(f, underflow ? -1 : 1);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t outer_phi_at = f->len;
  int outer_iv = f->next_value;
  nyr_opt_phi(f, -1, outer_start, 20, outer_start);
  *outer_cmp_at = f->len;
  int outer_cond = nyr_opt_cmp(f, outer_iv, outer_start,
                               underflow ? NYIR_CMP_GE : NYIR_CMP_LE);
  nyr_opt_br_if(f, outer_cond, 11);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 11);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 12);
  size_t inner_phi_at = f->len;
  int inner_iv = f->next_value;
  nyr_opt_phi(f, 11, zero, 13, zero);
  *inner_cmp_at = f->len;
  int inner_cond = nyr_opt_cmp(f, inner_iv, one, NYIR_CMP_LT);
  nyr_opt_br_if(f, inner_cond, 13);
  nyr_opt_br(f, 20);

  nyr_opt_label(f, 13);
  int next_inner = nyr_opt_binop(f, NYIR_ADD_I64, inner_iv, one);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 20);
  int next_outer = nyr_opt_binop(f, NYIR_ADD_I64, outer_iv, outer_step);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 30);
  nyr_opt_ret(f, outer_iv);
  f->data[outer_phi_at].phi_incoming[1].value = next_outer;
  f->data[inner_phi_at].phi_incoming[1].value = next_inner;
  return nyr_opt_verify(f, underflow ? "loop-interchange-underflow-before"
                                     : "loop-interchange-overflow-before");
}

static bool build_memory_read_loop(nyr_func_t *f, bool initialize, bool escape,
                                   bool conditional_initialize,
                                   bool conditional_overwrite, bool loop_write,
                                   bool unknown_address, size_t *outer_cmp_at,
                                   size_t *inner_cmp_at) {
  nyr_opt_begin(f);
  int base;
  if (unknown_address) {
    nyr_inst_t address_call = {.op = NYIR_CALL,
                                .dst = -1,
                                .a = -1,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .e = -1,
                                .f = -1,
                                .imm = 0,
                                .flags = NYIR_INST_F_EFFECTS_KNOWN,
                                .effects = NYIR_EFFECT_CALL |
                                           NYIR_EFFECT_UNKNOWN_SIDE_EFFECT,
                                .scalar_class = NYIR_SCALAR_POINTER};
    address_call.symbol =
        nyr_func_own_symbol_copy(f, "unknown_interchange_address");
    if (!address_call.symbol)
      return false;
    base = nyr_ir_emit(f, address_call);
  } else {
    base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                      .dst = -1,
                                      .a = -1,
                                      .b = -1,
                                      .c = -1,
                                      .d = -1,
                                      .e = -1,
                                      .f = -1,
                                      .imm = 8,
                                      .scalar_class = NYIR_SCALAR_POINTER});
  }
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int outer_limit = nyr_opt_const(f, 2);
  int inner_limit = nyr_opt_const(f, 4);
  int overwritten_value = nyr_opt_const(f, 99);
  int stack_value = nyr_opt_const(f, 7);
  if (initialize && conditional_initialize) {
    nyr_opt_br_if(f, zero, 5);
    nyr_opt_br(f, 6);
    nyr_opt_label(f, 5);
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = base,
                               .b = -1,
                               .c = stack_value,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP});
    nyr_opt_br(f, 9);
    nyr_opt_label(f, 6);
    nyr_opt_br(f, 9);
    nyr_opt_label(f, 9);
    if (conditional_overwrite)
      nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                                 .dst = -1,
                                 .a = base,
                                 .b = -1,
                                 .c = stack_value,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .effects = NYIR_EFFECT_WRITE_MEMORY |
                                            NYIR_EFFECT_MAY_TRAP});
  } else if (initialize) {
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = base,
                               .b = -1,
                               .c = overwritten_value,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP});
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = base,
                               .b = -1,
                               .c = stack_value,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP});
  }
  if (escape) {
    nyr_inst_t call = {.op = NYIR_CALL,
                        .dst = -1,
                        .a = base,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = 1,
                        .flags = NYIR_INST_F_EFFECTS_KNOWN,
                        .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_FFI |
                                   NYIR_EFFECT_THREAD |
                                   NYIR_EFFECT_UNKNOWN_SIDE_EFFECT};
    call.symbol = nyr_func_own_symbol_copy(f, "unknown_stack_escape");
    if (!call.symbol)
      return false;
    nyr_ir_emit(f, call);
  }
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t outer_phi_at = f->len;
  int outer_iv = f->next_value;
  nyr_opt_phi(f, conditional_initialize ? 9 : -1, zero, 20, zero);
  *outer_cmp_at = f->len;
  int outer_cond = nyr_opt_cmp(f, outer_iv, outer_limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, outer_cond, 11);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 11);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 12);
  size_t inner_phi_at = f->len;
  int inner_iv = f->next_value;
  nyr_opt_phi(f, 11, zero, 13, zero);
  *inner_cmp_at = f->len;
  int inner_cond = nyr_opt_cmp(f, inner_iv, inner_limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, inner_cond, 13);
  nyr_opt_br(f, 20);

  nyr_opt_label(f, 13);
  if (loop_write)
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = base,
                               .b = -1,
                               .c = stack_value,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP});
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                             .dst = -1,
                             .a = base,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_MAY_TRAP});
  int next_inner = nyr_opt_binop(f, NYIR_ADD_I64, inner_iv, one);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 20);
  int next_outer = nyr_opt_binop(f, NYIR_ADD_I64, outer_iv, one);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 30);
  nyr_opt_ret(f, outer_iv);
  f->data[outer_phi_at].phi_incoming[1].value = next_outer;
  f->data[inner_phi_at].phi_incoming[1].value = next_inner;
  return nyr_opt_verify(f, "loop-interchange-memory-read-before");
}

static bool build_affine_store_nest(
    nyr_func_t *f, size_t *outer_cmp_at, size_t *inner_cmp_at,
    int64_t inner_bound, int64_t row_stride_value, bool multiple_stores,
    bool overlapping_stores, bool transposed_stores, bool disjoint_stores) {
  nyr_opt_begin(f);
  int base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = disjoint_stores      ? 128
                                               : overlapping_stores ? 72
                                                                    : 64,
                                        .scalar_class = NYIR_SCALAR_POINTER});
  int second_base = -1;
  if (multiple_stores && !overlapping_stores && !disjoint_stores)
    second_base =
        nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .imm = 64,
                                   .scalar_class = NYIR_SCALAR_POINTER});
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int outer_limit = nyr_opt_const(f, 2);
  int inner_limit = nyr_opt_const(f, inner_bound);
  int row_stride = nyr_opt_const(f, row_stride_value);
  int value_stride = nyr_opt_const(f, 10);
  int byte_shift = nyr_opt_const(f, 3);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t outer_phi_at = f->len;
  int outer_iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 20, zero);
  *outer_cmp_at = f->len;
  int outer_cond = nyr_opt_cmp(f, outer_iv, outer_limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, outer_cond, 11);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 11);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 12);
  size_t inner_phi_at = f->len;
  int inner_iv = f->next_value;
  nyr_opt_phi(f, 11, zero, 13, zero);
  *inner_cmp_at = f->len;
  int inner_cond = nyr_opt_cmp(f, inner_iv, inner_limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, inner_cond, 13);
  nyr_opt_br(f, 20);

  nyr_opt_label(f, 13);
  int row = nyr_opt_binop(f, NYIR_MUL_I64, outer_iv, row_stride);
  int element = nyr_opt_binop(f, NYIR_ADD_I64, row, inner_iv);
  if (inner_bound == 8 || row_stride_value == INT64_MAX)
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                               .dst = -1,
                               .a = element,
                               .b = element,
                               .c = -1,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .imm = 8});
  int byte_offset = nyr_opt_binop(f, NYIR_SHL_I64, element, byte_shift);
  int address = nyr_opt_binop(f, NYIR_ADD_I64, base, byte_offset);
  int value_row = nyr_opt_binop(f, NYIR_MUL_I64, outer_iv, value_stride);
  int value = nyr_opt_binop(f, NYIR_ADD_I64, value_row, inner_iv);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = value,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_MAY_TRAP,
                             .scalar_class = NYIR_SCALAR_I64_RAW});
  if (multiple_stores) {
    int second_address = -1;
    if (transposed_stores) {
      int two = nyr_opt_const(f, 2);
      int twice_inner = nyr_opt_binop(f, NYIR_MUL_I64, inner_iv, two);
      int transposed_element =
          nyr_opt_binop(f, NYIR_ADD_I64, outer_iv, twice_inner);
      int transposed_offset =
          nyr_opt_binop(f, NYIR_SHL_I64, transposed_element, byte_shift);
      second_address =
          transposed_offset < 0
              ? -1
              : nyr_opt_binop(f, NYIR_ADD_I64, base, transposed_offset);
    } else if (disjoint_stores) {
      int separate_range = nyr_opt_const(f, 64);
      int separate_offset =
          separate_range < 0
              ? -1
              : nyr_opt_binop(f, NYIR_ADD_I64, byte_offset, separate_range);
      second_address =
          separate_offset < 0
              ? -1
              : nyr_opt_binop(f, NYIR_ADD_I64, base, separate_offset);
    } else if (overlapping_stores) {
      int one_element = nyr_opt_const(f, 8);
      second_address = one_element < 0 ? -1
                                       : nyr_opt_binop(f, NYIR_ADD_I64, address,
                                                       one_element);
    } else {
      second_address = nyr_opt_binop(f, NYIR_ADD_I64, second_base, byte_offset);
    }
    if (second_address < 0)
      return false;
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = second_address,
                               .b = -1,
                               .c = value,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP,
                               .scalar_class = NYIR_SCALAR_I64_RAW});
  }
  int next_inner = nyr_opt_binop(f, NYIR_ADD_I64, inner_iv, one);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 20);
  int next_outer = nyr_opt_binop(f, NYIR_ADD_I64, outer_iv, one);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 30);
  int result_offset = nyr_opt_const(f, transposed_stores    ? 8
                                       : overlapping_stores ? 32
                                                            : 48);
  int result_address = nyr_opt_binop(f, NYIR_ADD_I64, base, result_offset);
  int result = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = result_address,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .effects = NYIR_EFFECT_READ_MEMORY |
                                                     NYIR_EFFECT_MAY_TRAP,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
  if (multiple_stores && !overlapping_stores && !disjoint_stores) {
    int second_address =
        nyr_opt_binop(f, NYIR_ADD_I64, second_base, result_offset);
    int second_result =
        nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                   .dst = -1,
                                   .a = second_address,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .effects = NYIR_EFFECT_READ_MEMORY |
                                              NYIR_EFFECT_MAY_TRAP,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    result = nyr_opt_binop(f, NYIR_ADD_I64, result, second_result);
  }
  nyr_opt_ret(f, result);
  f->data[outer_phi_at].phi_incoming[1].value = next_outer;
  f->data[inner_phi_at].phi_incoming[1].value = next_inner;
  return nyr_opt_verify(f, "loop-interchange-affine-store-before");
}

static bool build_external_store_nest(nyr_func_t *f, bool mark_in_bounds,
                                      bool alias_second_root,
                                      size_t *outer_cmp_at,
                                      size_t *inner_cmp_at) {
  nyr_opt_begin(f);
  int base = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .imm = 0,
                                        .scalar_class = NYIR_SCALAR_POINTER});
  int second_base = -1;
  if (alias_second_root)
    second_base =
        nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .imm = 1,
                                   .scalar_class = NYIR_SCALAR_POINTER});
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int two = nyr_opt_const(f, 2);
  int three = nyr_opt_const(f, 3);
  int ten = nyr_opt_const(f, 10);
  int shift = nyr_opt_const(f, 3);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  size_t outer_phi_at = f->len;
  int outer_iv = f->next_value;
  nyr_opt_phi(f, -1, zero, 20, zero);
  *outer_cmp_at = f->len;
  int outer_cond = nyr_opt_cmp(f, outer_iv, two, NYIR_CMP_LT);
  nyr_opt_br_if(f, outer_cond, 11);
  nyr_opt_br(f, 30);

  nyr_opt_label(f, 11);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 12);
  size_t inner_phi_at = f->len;
  int inner_iv = f->next_value;
  nyr_opt_phi(f, 11, zero, 13, zero);
  *inner_cmp_at = f->len;
  int inner_cond = nyr_opt_cmp(f, inner_iv, three, NYIR_CMP_LT);
  nyr_opt_br_if(f, inner_cond, 13);
  nyr_opt_br(f, 20);

  nyr_opt_label(f, 13);
  int row = nyr_opt_binop(f, NYIR_MUL_I64, outer_iv, three);
  int element = nyr_opt_binop(f, NYIR_ADD_I64, row, inner_iv);
  int byte_offset = nyr_opt_binop(f, NYIR_SHL_I64, element, shift);
  int address = nyr_opt_binop(f, NYIR_ADD_I64, base, byte_offset);
  if (mark_in_bounds)
    f->data[f->len - 1].flags |= NYIR_INST_F_IN_BOUNDS_ADDR;
  int value_row = nyr_opt_binop(f, NYIR_MUL_I64, outer_iv, ten);
  int value = nyr_opt_binop(f, NYIR_ADD_I64, value_row, inner_iv);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = value,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_MAY_TRAP,
                             .scalar_class = NYIR_SCALAR_I64_RAW});
  if (alias_second_root) {
    int two_inner = nyr_opt_binop(f, NYIR_MUL_I64, inner_iv, two);
    int transposed = nyr_opt_binop(f, NYIR_ADD_I64, outer_iv, two_inner);
    int second_offset = nyr_opt_binop(f, NYIR_SHL_I64, transposed, shift);
    int second_address =
        nyr_opt_binop(f, NYIR_ADD_I64, second_base, second_offset);
    if (mark_in_bounds)
      f->data[f->len - 1].flags |= NYIR_INST_F_IN_BOUNDS_ADDR;
    int second_value = nyr_opt_binop(f, NYIR_ADD_I64, value, ten);
    nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = second_address,
                               .b = -1,
                               .c = second_value,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP,
                               .scalar_class = NYIR_SCALAR_I64_RAW});
  }
  int next_inner = nyr_opt_binop(f, NYIR_ADD_I64, inner_iv, one);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 20);
  int next_outer = nyr_opt_binop(f, NYIR_ADD_I64, outer_iv, one);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 30);
  int total = zero;
  for (int index = 0; index < 6; ++index) {
    int element_index = nyr_opt_const(f, index);
    int byte_index = nyr_opt_binop(f, NYIR_SHL_I64, element_index, shift);
    int result_address = nyr_opt_binop(f, NYIR_ADD_I64, base, byte_index);
    int loaded =
        nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                   .dst = -1,
                                   .a = result_address,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .effects = NYIR_EFFECT_READ_MEMORY |
                                              NYIR_EFFECT_MAY_TRAP,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    total = nyr_opt_binop(f, NYIR_ADD_I64, total, loaded);
  }
  nyr_opt_ret(f, total);
  f->data[outer_phi_at].phi_incoming[1].value = next_outer;
  f->data[inner_phi_at].phi_incoming[1].value = next_inner;
  return nyr_opt_verify(f, "loop-interchange-external-store-before");
}

static bool eval_external_store(nyr_func_t *f, int64_t memory[6],
                                bool alias_second_root, int64_t *result) {
  int64_t locals[8] = {0};
  locals[0] = (int64_t)(uintptr_t)memory;
  if (alias_second_root)
    locals[1] = (int64_t)(uintptr_t)memory;
  return nyr_opt_eval_locals(f, "loop-interchange-external-store", locals, 8,
                             result);
}

int main(void) {
  nyr_func_t f;
  size_t outer_cmp_at = 0, inner_cmp_at = 0;
  if (!build_nonterminating_nest(&f, &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  int64_t outcome_before = 0, outcome_after = 0;
  char outcome_error_before[256] = {0}, outcome_error_after[256] = {0};
  bool nonterminating_before =
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_before,
                           outcome_error_before, sizeof(outcome_error_before));
  bool nonterminating_safe =
      nonterminating_before && nyr_loop_interchange(&f) &&
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_after,
                           outcome_error_after, sizeof(outcome_error_after)) &&
      outcome_before == outcome_after;
  nyr_ir_func_free(&f);
  if (!nonterminating_safe) {
    fputs(
        "[loop-interchange] potentially nonterminating loops were reordered\n",
        stderr);
    return 1;
  }
  puts("[loop-interchange] potentially nonterminating loops stay ordered");

  outer_cmp_at = 0;
  inner_cmp_at = 0;
  if (!build_overflowing_nest(&f, false, &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  memset(outcome_error_before, 0, sizeof(outcome_error_before));
  memset(outcome_error_after, 0, sizeof(outcome_error_after));
  bool overflow_before =
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_before,
                           outcome_error_before, sizeof(outcome_error_before));
  bool overflow_safe =
      !overflow_before && nyr_loop_interchange(&f) &&
      !nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_after,
                            outcome_error_after, sizeof(outcome_error_after)) &&
      strcmp(outcome_error_before, outcome_error_after) == 0;
  nyr_ir_func_free(&f);
  if (!overflow_safe) {
    fputs("[loop-interchange] overflowing induction nest was reordered\n",
          stderr);
    return 1;
  }
  puts("[loop-interchange] overflowing induction nests stay ordered");

  outer_cmp_at = 0;
  inner_cmp_at = 0;
  if (!build_overflowing_nest(&f, true, &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  memset(outcome_error_before, 0, sizeof(outcome_error_before));
  memset(outcome_error_after, 0, sizeof(outcome_error_after));
  overflow_before =
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_before,
                           outcome_error_before, sizeof(outcome_error_before));
  overflow_safe =
      !overflow_before && nyr_loop_interchange(&f) &&
      !nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_after,
                            outcome_error_after, sizeof(outcome_error_after)) &&
      strcmp(outcome_error_before, outcome_error_after) == 0;
  nyr_ir_func_free(&f);
  if (!overflow_safe) {
    fputs("[loop-interchange] underflowing induction nest was reordered\n",
          stderr);
    return 1;
  }
  puts("[loop-interchange] underflowing induction nests stay ordered");

  if (!build_exit_phi_escape(&f))
    return 1;
  long long before = nyr_opt_eval(&f, "loop-interchange-exit-phi-before");
  if (!nyr_loop_interchange(&f) ||
      !nyr_opt_verify(&f, "loop-interchange-exit-phi-after")) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long after = nyr_opt_eval(&f, "loop-interchange-exit-phi-after");
  nyr_ir_func_free(&f);
  if (before != 3 || after != before) {
    fprintf(stderr,
            "[loop-interchange] escaping IV changed: before=%lld after=%lld\n",
            before, after);
    return 1;
  }
  puts("[loop-interchange] exit PHI escape rejected");

  outer_cmp_at = 0;
  inner_cmp_at = 0;
  if (!build_local_access_nest(&f, false, false, &outer_cmp_at,
                               &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long local_before =
      nyr_opt_eval(&f, "loop-interchange-read-only-local-before");
  bool pass_ok = nyr_loop_interchange(&f);
  bool verify_ok =
      pass_ok && nyr_opt_verify(&f, "loop-interchange-read-only-local-after");
  long long local_after =
      verify_ok ? nyr_opt_eval(&f, "loop-interchange-read-only-local-after")
                : 0;
  bool interchanged = verify_ok && local_after == local_before;
  if (!interchanged) {
    fputs("[loop-interchange] read-only local nest was not interchanged with "
          "its result preserved\n",
          stderr);
    fprintf(stderr, "[loop-interchange] pass=%d verify=%d result=%lld/%lld\n",
            (int)pass_ok, (int)verify_ok, local_before, local_after);
    nyr_dump(stderr, &f, "loop-interchange-read-only-local-failure");
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  puts("[loop-interchange] read-only local rectangular nest interchanged");

  if (!build_affine_store_nest(&f, &outer_cmp_at, &inner_cmp_at, 4, 4, false,
                               false, false, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long affine_before =
      nyr_opt_eval(&f, "loop-interchange-affine-store-before");
  bool affine_store_ok =
      nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-affine-store-after") &&
      nyr_opt_eval(&f, "loop-interchange-affine-store-after") ==
          affine_before &&
      affine_before == 12;
  nyr_ir_func_free(&f);
  if (!affine_store_ok) {
    fputs("[loop-interchange] affine private store changed memory results or "
          "was not interchanged\n",
          stderr);
    return 1;
  }
  puts("[loop-interchange] injective in-bounds affine private store "
       "interchanged");

  if (!build_affine_store_nest(&f, &outer_cmp_at, &inner_cmp_at, 4, 4, true,
                               false, false, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long multiple_before =
      nyr_opt_eval(&f, "loop-interchange-multiple-stores-before");
  bool multiple_store_ok =
      nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-multiple-stores-after") &&
      nyr_opt_eval(&f, "loop-interchange-multiple-stores-after") ==
          multiple_before &&
      multiple_before == 24;
  nyr_ir_func_free(&f);
  if (!multiple_store_ok) {
    fputs(
        "[loop-interchange] independent affine stores failed to interchange\n",
        stderr);
    return 1;
  }
  puts("[loop-interchange] multiple independent affine private stores "
       "interchanged");

  if (!build_affine_store_nest(&f, &outer_cmp_at, &inner_cmp_at, 4, 4, true,
                               false, false, true)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long disjoint_before =
      nyr_opt_eval(&f, "loop-interchange-disjoint-stores-before");
  bool disjoint_pass_ok = nyr_loop_interchange(&f);
  bool disjoint_verify_ok =
      disjoint_pass_ok &&
      nyr_opt_verify(&f, "loop-interchange-disjoint-stores-after");
  long long disjoint_after =
      disjoint_verify_ok
          ? nyr_opt_eval(&f, "loop-interchange-disjoint-stores-after")
          : 0;
  bool disjoint_store_ok = disjoint_verify_ok &&
                           disjoint_after == disjoint_before &&
                           disjoint_before == 12;
  nyr_ir_func_free(&f);
  if (!disjoint_store_ok) {
    fprintf(stderr,
            "[loop-interchange] disjoint affine stores in one allocation "
            "failed: pass=%d verify=%d result=%lld/%lld\n",
            (int)disjoint_pass_ok, (int)disjoint_verify_ok, disjoint_before,
            disjoint_after);
    return 1;
  }
  puts("[loop-interchange] disjoint affine stores in one allocation "
       "interchanged");

  if (!build_affine_store_nest(&f, &outer_cmp_at, &inner_cmp_at, 4, 4, true,
                               true, false, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long overlapping_before =
      nyr_opt_eval(&f, "loop-interchange-overlapping-multiple-stores-before");
  bool overlapping_pass_ok = nyr_loop_interchange(&f);
  bool overlapping_verify_ok =
      overlapping_pass_ok &&
      nyr_opt_verify(&f, "loop-interchange-overlapping-multiple-stores-after");
  long long overlapping_after =
      overlapping_verify_ok
          ? nyr_opt_eval(&f,
                         "loop-interchange-overlapping-multiple-stores-after")
          : 0;
  bool overlapping_safe = overlapping_verify_ok &&
                          overlapping_before == overlapping_after &&
                          overlapping_before == overlapping_after;
  nyr_ir_func_free(&f);
  if (!overlapping_safe) {
    fprintf(stderr,
            "[loop-interchange] cross-store affine overlaps were reordered "
            "or changed result: pass=%d verify=%d result=%lld/%lld\n",
            (int)overlapping_pass_ok, (int)overlapping_verify_ok,
            overlapping_before, overlapping_after);
    return 1;
  }
  puts("[loop-interchange] cross-store affine overlaps stay ordered");

  if (!build_affine_store_nest(&f, &outer_cmp_at, &inner_cmp_at, 3, 3, true,
                               true, true, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long transposed_before =
      nyr_opt_eval(&f, "loop-interchange-transposed-stores-before");
  bool transposed_pass_ok = nyr_loop_interchange(&f);
  bool transposed_verify_ok =
      transposed_pass_ok &&
      nyr_opt_verify(&f, "loop-interchange-transposed-stores-after");
  long long transposed_after =
      transposed_verify_ok
          ? nyr_opt_eval(&f, "loop-interchange-transposed-stores-after")
          : 0;
  bool transposed_safe = transposed_verify_ok && transposed_before == 10 &&
                         transposed_after == transposed_before;
  nyr_ir_func_free(&f);
  if (!transposed_safe) {
    fprintf(stderr,
            "[loop-interchange] transposed affine stores changed "
            "cross-iteration overwrite order: pass=%d verify=%d "
            "result=%lld/%lld\n",
            (int)transposed_pass_ok, (int)transposed_verify_ok,
            transposed_before, transposed_after);
    return 1;
  }
  puts("[loop-interchange] transposed cross-store dependences stay ordered");

  if (!build_affine_store_nest(&f, &outer_cmp_at, &inner_cmp_at, 4, 1, false,
                               false, false, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long dependent_before =
      nyr_opt_eval(&f, "loop-interchange-overlapping-store-before");
  bool dependent_store_safe =
      nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-overlapping-store-after") &&
      nyr_opt_eval(&f, "loop-interchange-overlapping-store-after") ==
          dependent_before;
  nyr_ir_func_free(&f);
  if (!dependent_store_safe) {
    fputs("[loop-interchange] overlapping affine stores were reordered\n",
          stderr);
    return 1;
  }
  puts("[loop-interchange] overlapping affine stores stay ordered");

  if (!build_affine_store_nest(&f, &outer_cmp_at, &inner_cmp_at, 8, 4, false,
                               false, false, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  int64_t eval_locals_before[8] = {0}, eval_locals_after[8] = {0};
  int64_t out_of_bounds_before = 0, out_of_bounds_after = 0;
  char out_of_bounds_error_before[256] = {0};
  char out_of_bounds_error_after[256] = {0};
  bool out_of_bounds_before_ok = nyr_opt_eval_outcome(
      &f, eval_locals_before, 8, 500, &out_of_bounds_before,
      out_of_bounds_error_before, sizeof(out_of_bounds_error_before));
  bool out_of_bounds_store_safe =
      !out_of_bounds_before_ok && nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-out-of-bounds-store-after") &&
      !nyr_opt_eval_outcome(&f, eval_locals_after, 8, 500, &out_of_bounds_after,
                            out_of_bounds_error_after,
                            sizeof(out_of_bounds_error_after)) &&
      strcmp(out_of_bounds_error_before, out_of_bounds_error_after) == 0;
  nyr_ir_func_free(&f);
  if (!out_of_bounds_store_safe) {
    fputs("[loop-interchange] out-of-bounds affine stores were reordered\n",
          stderr);
    return 1;
  }
  puts("[loop-interchange] out-of-bounds affine stores stay ordered");

  if (!build_affine_store_nest(&f, &outer_cmp_at, &inner_cmp_at, 4, INT64_MAX,
                               false, false, false, false)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  memset(eval_locals_before, 0, sizeof(eval_locals_before));
  memset(eval_locals_after, 0, sizeof(eval_locals_after));
  int64_t overflowing_before = 0, overflowing_after = 0;
  char overflowing_error_before[256] = {0};
  char overflowing_error_after[256] = {0};
  bool overflowing_before_ok = nyr_opt_eval_outcome(
      &f, eval_locals_before, 8, 500, &overflowing_before,
      overflowing_error_before, sizeof(overflowing_error_before));
  bool overflowing_store_safe =
      !overflowing_before_ok && nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-overflowing-store-after") &&
      !nyr_opt_eval_outcome(&f, eval_locals_after, 8, 500, &overflowing_after,
                            overflowing_error_after,
                            sizeof(overflowing_error_after));
  nyr_ir_func_free(&f);
  if (!overflowing_store_safe) {
    fputs("[loop-interchange] overflowing affine stores were reordered\n",
          stderr);
    return 1;
  }
  puts("[loop-interchange] overflowing affine store addresses stay ordered");

  if (!build_local_access_nest(&f, false, true, &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long carried_before =
      nyr_opt_eval(&f, "loop-interchange-carried-phi-before");
  bool carried_safe =
      nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-carried-phi-after") &&
      nyr_opt_eval(&f, "loop-interchange-carried-phi-after") == carried_before;
  nyr_ir_func_free(&f);
  if (!carried_safe) {
    fputs("[loop-interchange] loop-carried PHI body was reordered\n", stderr);
    return 1;
  }
  puts("[loop-interchange] extra loop-carried PHI stays ordered");

  if (!build_local_access_nest(&f, false, false, &outer_cmp_at,
                               &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  int zero = -1;
  int inner_iv = -1;
  size_t trap_at = SIZE_MAX;
  for (size_t i = 0; i < f.len; ++i) {
    if (f.data[i].op == NYIR_CONST_I64 && f.data[i].imm == 0)
      zero = f.data[i].dst;
    if (f.data[i].op == NYIR_PHI && f.data[i].phi_incoming_len > 0 &&
        f.data[i].phi_incoming[0].predecessor_label == 11)
      inner_iv = f.data[i].dst;
    if (f.data[i].op == NYIR_LOAD_LOCAL)
      trap_at = i;
  }
  if (zero < 0 || inner_iv < 0 || trap_at == SIZE_MAX) {
    nyr_ir_func_free(&f);
    fputs("[loop-interchange] failed to construct arithmetic-trap case\n",
          stderr);
    return 1;
  }
  f.data[trap_at].op = NYIR_DIV_I64;
  f.data[trap_at].a = zero;
  f.data[trap_at].b = inner_iv;
  f.data[trap_at].effects = NYIR_EFFECT_MAY_TRAP;
  bool trap_effect =
      (nyr_effective_effects(&f.data[trap_at]) & NYIR_EFFECT_MAY_TRAP) != 0;
  memset(outcome_error_before, 0, sizeof(outcome_error_before));
  memset(outcome_error_after, 0, sizeof(outcome_error_after));
  bool trap_before =
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_before,
                           outcome_error_before, sizeof(outcome_error_before));
  bool trap_order_safe =
      trap_effect && !trap_before &&
      nyr_opt_verify(&f, "loop-interchange-div-trap-before") &&
      nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-div-trap-after") &&
      !nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_after,
                            outcome_error_after, sizeof(outcome_error_after)) &&
      strcmp(outcome_error_before, outcome_error_after) == 0;
  nyr_ir_func_free(&f);
  if (!trap_order_safe) {
    fputs("[loop-interchange] trapping division body was reordered\n", stderr);
    return 1;
  }
  puts("[loop-interchange] trapping division bodies stay ordered");

  if (!build_local_access_nest(&f, false, false, &outer_cmp_at,
                               &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  size_t call_at = SIZE_MAX;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].op == NYIR_LOAD_LOCAL)
      call_at = i;
  if (call_at == SIZE_MAX) {
    nyr_ir_func_free(&f);
    fputs("[loop-interchange] failed to construct effectful-call case\n",
          stderr);
    return 1;
  }
  nyr_inst_t *effectful_call = &f.data[call_at];
  effectful_call->op = NYIR_CALL;
  f.has_symbol_calls = true;
  effectful_call->dst = -1;
  effectful_call->a = effectful_call->b = effectful_call->c = -1;
  effectful_call->d = effectful_call->e = effectful_call->f = -1;
  effectful_call->imm = 0;
  effectful_call->symbol =
      nyr_func_own_symbol_copy(&f, "unknown_interchange_effect");
  effectful_call->flags = NYIR_INST_F_EFFECTS_KNOWN;
  effectful_call->effects =
      NYIR_EFFECT_CALL | NYIR_EFFECT_FFI | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  nyr_effect_t call_effects = nyr_effective_effects(effectful_call);
  bool call_order_safe = effectful_call->symbol &&
                         (call_effects & (NYIR_EFFECT_CALL | NYIR_EFFECT_FFI |
                                          NYIR_EFFECT_UNKNOWN_SIDE_EFFECT)) ==
                             (NYIR_EFFECT_CALL | NYIR_EFFECT_FFI |
                              NYIR_EFFECT_UNKNOWN_SIDE_EFFECT) &&
                         nyr_opt_verify(&f, "loop-interchange-call-before") &&
                         nyr_loop_interchange(&f) &&
                         nyr_opt_verify(&f, "loop-interchange-call-after");
  nyr_ir_func_free(&f);
  if (!call_order_safe) {
    fputs("[loop-interchange] effectful call body was reordered\n", stderr);
    return 1;
  }
  puts("[loop-interchange] unknown-effect call bodies stay ordered");

  if (!build_local_access_nest(&f, true, false, &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long local_write_before =
      nyr_opt_eval(&f, "loop-interchange-local-write-before");
  bool local_write_safe =
      nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-local-write-after") &&
      nyr_opt_eval(&f, "loop-interchange-local-write-after") ==
          local_write_before;
  nyr_ir_func_free(&f);
  if (!local_write_safe || local_write_before != 8) {
    fprintf(stderr,
            "[loop-interchange] local write changed order/result: "
            "safe=%d result=%lld expected=8\n",
            (int)local_write_safe, local_write_before);
    return 1;
  }
  puts("[loop-interchange] local writes inside the nest stay ordered");

  outer_cmp_at = 0;
  inner_cmp_at = 0;
  if (!build_memory_read_loop(&f, true, false, false, false, false, false,
                              &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long memory_before =
      nyr_opt_eval(&f, "loop-interchange-memory-read-before");
  bool memory_pass_ok = nyr_loop_interchange(&f);
  bool memory_verify_ok =
      memory_pass_ok &&
      nyr_opt_verify(&f, "loop-interchange-memory-read-after");
  long long memory_after =
      memory_verify_ok ? nyr_opt_eval(&f, "loop-interchange-memory-read-after")
                       : 0;
  bool memory_read_safe = memory_verify_ok && memory_after == memory_before;
  if (!memory_read_safe) {
    fputs("[loop-interchange] initialized invariant stack read did not "
          "interchange\n",
          stderr);
    fprintf(stderr, "[loop-interchange] pass=%d verify=%d result=%lld/%lld\n",
            (int)memory_pass_ok, (int)memory_verify_ok, memory_before,
            memory_after);
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  puts("[loop-interchange] initialized invariant stack reads interchange");

  if (!build_memory_read_loop(&f, true, false, true, true, false, false,
                              &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long overwrite_before =
      nyr_opt_eval(&f, "loop-interchange-conditional-overwrite-before");
  bool overwrite_pass_ok = nyr_loop_interchange(&f);
  bool overwrite_verify_ok =
      overwrite_pass_ok &&
      nyr_opt_verify(&f, "loop-interchange-conditional-overwrite-after");
  long long overwrite_after =
      overwrite_verify_ok
          ? nyr_opt_eval(&f, "loop-interchange-conditional-overwrite-after")
          : 0;
  bool overwrite_read_safe =
      overwrite_verify_ok && overwrite_after == overwrite_before;
  nyr_ir_func_free(&f);
  if (!overwrite_read_safe) {
    fprintf(stderr,
            "[loop-interchange] dominating overwrite failed to make reads "
            "invariant: pass=%d verify=%d result=%lld/%lld\n",
            (int)overwrite_pass_ok, (int)overwrite_verify_ok, overwrite_before,
            overwrite_after);
    return 1;
  }
  puts("[loop-interchange] dominating overwrite makes conditional store safe");

  if (!build_memory_read_loop(&f, true, false, false, false, true, false,
                              &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  long long write_before =
      nyr_opt_eval(&f, "loop-interchange-stack-write-before");
  bool write_pass_ok = nyr_loop_interchange(&f);
  bool write_verify_ok =
      write_pass_ok && nyr_opt_verify(&f, "loop-interchange-stack-write-after");
  long long write_after =
      write_verify_ok ? nyr_opt_eval(&f, "loop-interchange-stack-write-after")
                      : 0;
  bool stack_write_safe = write_verify_ok && write_before == write_after;
  nyr_ir_func_free(&f);
  if (!stack_write_safe) {
    fprintf(stderr,
            "[loop-interchange] stack write changed behavior/schedule: "
            "pass=%d verify=%d before=%lld after=%lld\n",
            (int)write_pass_ok, (int)write_verify_ok, write_before,
            write_after);
    return 1;
  }
  puts("[loop-interchange] stack writes inside the nest stay ordered");

  if (!build_memory_read_loop(&f, false, false, false, false, false, false,
                              &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  memset(outcome_error_before, 0, sizeof(outcome_error_before));
  memset(outcome_error_after, 0, sizeof(outcome_error_after));
  bool uninitialized_before =
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_before,
                           outcome_error_before, sizeof(outcome_error_before));
  bool uninitialized_safe =
      nyr_opt_verify(&f, "loop-interchange-uninitialized-before") &&
      nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-uninitialized-after") &&
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_after,
                           outcome_error_after, sizeof(outcome_error_after)) ==
          uninitialized_before &&
      (uninitialized_before
           ? outcome_before == outcome_after
           : strcmp(outcome_error_before, outcome_error_after) == 0);
  nyr_ir_func_free(&f);
  if (!uninitialized_safe) {
    fputs("[loop-interchange] uninitialized stack read was reordered\n",
          stderr);
    return 1;
  }
  puts("[loop-interchange] uninitialized stack reads stay ordered");

  if (!build_memory_read_loop(&f, true, false, true, false, false, false,
                              &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  memset(outcome_error_before, 0, sizeof(outcome_error_before));
  memset(outcome_error_after, 0, sizeof(outcome_error_after));
  bool conditional_before =
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_before,
                           outcome_error_before, sizeof(outcome_error_before));
  bool conditional_init_safe =
      nyr_opt_verify(&f, "loop-interchange-conditional-init-before") &&
      nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-conditional-init-after") &&
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_after,
                           outcome_error_after,
                           sizeof(outcome_error_after)) == conditional_before &&
      (conditional_before
           ? outcome_before == outcome_after
           : strcmp(outcome_error_before, outcome_error_after) == 0);
  nyr_ir_func_free(&f);
  if (!conditional_init_safe) {
    fputs("[loop-interchange] non-dominating stack initialization was "
          "accepted\n",
          stderr);
    return 1;
  }
  puts("[loop-interchange] non-dominating stack initialization stays ordered");

  if (!build_memory_read_loop(&f, true, true, false, false, false, false,
                              &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  memset(outcome_error_before, 0, sizeof(outcome_error_before));
  memset(outcome_error_after, 0, sizeof(outcome_error_after));
  bool escape_before =
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_before,
                           outcome_error_before, sizeof(outcome_error_before));
  bool escaped_pointer_safe =
      nyr_opt_verify(&f, "loop-interchange-escaped-stack-before") &&
      nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-escaped-stack-after") &&
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_after,
                           outcome_error_after,
                           sizeof(outcome_error_after)) == escape_before &&
      (escape_before ? outcome_before == outcome_after
                     : strcmp(outcome_error_before, outcome_error_after) == 0);
  nyr_ir_func_free(&f);
  if (!escaped_pointer_safe) {
    fputs("[loop-interchange] escaped stack pointer read was reordered\n",
          stderr);
    return 1;
  }
  puts("[loop-interchange] escaped stack pointers stay ordered");

  if (!build_memory_read_loop(&f, false, false, false, false, false, true,
                              &outer_cmp_at, &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  memset(outcome_error_before, 0, sizeof(outcome_error_before));
  memset(outcome_error_after, 0, sizeof(outcome_error_after));
  bool unknown_before =
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_before,
                           outcome_error_before, sizeof(outcome_error_before));
  bool unknown_address_safe =
      nyr_opt_verify(&f, "loop-interchange-unknown-address-before") &&
      nyr_loop_interchange(&f) &&
      nyr_opt_verify(&f, "loop-interchange-unknown-address-after") &&
      nyr_opt_eval_outcome(&f, NULL, 0, 500, &outcome_after,
                           outcome_error_after,
                           sizeof(outcome_error_after)) == unknown_before &&
      (unknown_before ? outcome_before == outcome_after
                      : strcmp(outcome_error_before, outcome_error_after) == 0);
  nyr_ir_func_free(&f);
  if (!unknown_address_safe) {
    fputs("[loop-interchange] unknown trapping address was reordered\n",
          stderr);
    return 1;
  }
  puts("[loop-interchange] unknown trapping addresses stay ordered");

  int64_t external_before_memory[6] = {-1, -1, -1, -1, -1, -1};
  int64_t external_after_memory[6] = {-1, -1, -1, -1, -1, -1};
  int64_t external_before = 0, external_after = 0;
  if (!build_external_store_nest(&f, true, false, &outer_cmp_at,
                                 &inner_cmp_at) ||
      !eval_external_store(&f, external_before_memory, false,
                           &external_before) ||
      external_before != 36 ||
      !nyr_opt_verify(&f, "loop-interchange-external-store-verified") ||
      !nyr_loop_interchange(&f) ||
      !nyr_opt_verify(&f, "loop-interchange-external-store-after") ||
      !eval_external_store(&f, external_after_memory, false, &external_after) ||
      external_after != external_before ||
      memcmp(external_before_memory, external_after_memory,
             sizeof(external_before_memory)) != 0) {
    fputs("[loop-interchange] proven external affine stores changed memory\n",
          stderr);
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  puts("[loop-interchange] in-bounds external affine stores preserve memory");

  if (!build_external_store_nest(&f, false, false, &outer_cmp_at,
                                 &inner_cmp_at)) {
    nyr_ir_func_free(&f);
    return 1;
  }
  int64_t unknown_before_memory[6] = {-1, -1, -1, -1, -1, -1};
  int64_t unknown_after_memory[6] = {-1, -1, -1, -1, -1, -1};
  int64_t unknown_before_result = 0, unknown_after_result = 0;
  if (!eval_external_store(&f, unknown_before_memory, false,
                           &unknown_before_result) ||
      !nyr_loop_interchange(&f) ||
      !nyr_opt_verify(&f, "loop-interchange-unknown-external-store-after") ||
      !eval_external_store(&f, unknown_after_memory, false,
                           &unknown_after_result) ||
      unknown_after_result != unknown_before_result ||
      memcmp(unknown_before_memory, unknown_after_memory,
             sizeof(unknown_before_memory)) != 0) {
    fputs("[loop-interchange] unproved external accesses changed behavior\n",
          stderr);
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  puts("[loop-interchange] unproved external addresses preserve behavior");

  int64_t alias_before_memory[6] = {-1, -1, -1, -1, -1, -1};
  int64_t alias_after_memory[6] = {-1, -1, -1, -1, -1, -1};
  int64_t alias_before = 0, alias_after = 0;
  if (!build_external_store_nest(&f, true, true, &outer_cmp_at,
                                 &inner_cmp_at) ||
      !eval_external_store(&f, alias_before_memory, true, &alias_before) ||
      !nyr_loop_interchange(&f) ||
      !nyr_opt_verify(&f, "loop-interchange-external-alias-after") ||
      !eval_external_store(&f, alias_after_memory, true, &alias_after) ||
      alias_after != alias_before ||
      memcmp(alias_before_memory, alias_after_memory,
             sizeof(alias_before_memory)) != 0) {
    fputs("[loop-interchange] external aliasing stores changed memory\n",
          stderr);
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  puts("[loop-interchange] possibly aliased external store roots preserve "
       "order");
  return 0;
}
