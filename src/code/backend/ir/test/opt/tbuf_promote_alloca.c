/*
 * Stack promotion must reject constant out-of-bounds accesses to typed buffers.
 */
#include "support.h"

static void build_typed_buffer_access(nyr_func_t *f, int64_t offset,
                                      bool overflowing_offset, bool store,
                                      size_t *allocation_index) {
  nyr_opt_begin(f);
  int count = nyr_opt_const(f, 1);
  int width = nyr_opt_const(f, 8);
  *allocation_index = f->len;
  int buffer = nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                                           .dst = -1,
                                           .a = count,
                                           .b = width,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = 2,
                                           .symbol = "rt_tbuf_new_typed_raw"});
  int byte_offset = nyr_opt_const(f, offset);
  if (overflowing_offset) {
    int max = nyr_opt_const(f, INT64_MAX);
    int one = nyr_opt_const(f, 1);
    byte_offset = nyr_opt_binop(f, NYIR_ADD_I64, max, one);
  }
  int address = nyr_opt_binop(f, NYIR_ADD_I64, buffer, byte_offset);
  int result = -1;
  if (store) {
    int value = nyr_opt_const(f, 42);
    nyr_opt_emit_abc(f, NYIR_STORE_I64, address, -1, value);
    result = value;
  } else {
    result = nyr_opt_emit_abc(f, NYIR_LOAD_I64, address, -1, -1);
  }
  nyr_opt_ret(f, result);
}

static int check_offset(int64_t offset, bool overflowing_offset, bool store,
                        bool expected_promotion) {
  nyr_func_t f;
  size_t allocation_index = 0;
  build_typed_buffer_access(&f, offset, overflowing_offset, store,
                            &allocation_index);
  bool ok = nyr_tbuf_promote_alloca(&f);
  bool promoted = f.data[allocation_index].op == NYIR_ALLOCA;
  if (!ok || promoted != expected_promotion) {
    fprintf(stderr,
            "[tbuf-promote-alloca] offset=%lld expected promotion=%d "
            "actual=%d (store=%d)\n",
            (long long)offset, (int)expected_promotion, (int)promoted,
            (int)store);
    nyr_dump(stderr, &f, "tbuf-promote-alloca/access-bounds");
    nyr_ir_func_free(&f);
    return 1;
  }
  nyr_ir_func_free(&f);
  return 0;
}

int main(void) {
  for (int store = 0; store <= 1; ++store) {
    if (check_offset(0, false, store != 0, true) ||
        check_offset(8, false, store != 0, false) ||
        check_offset(-8, false, store != 0, false) ||
        check_offset(0, true, store != 0, false))
      return 1;
  }
  puts("[tbuf-promote-alloca] load/store bounds preserved");
  return 0;
}
