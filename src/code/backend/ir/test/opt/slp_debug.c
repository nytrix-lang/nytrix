/*
 * SLP vector nodes retain a useful source anchor from their scalar group.
 */
#include "code/backend/ir/opt/util.h"
#include "support.h"

static int emit_symbol_address(nyr_func_t *f, const char *symbol) {
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

static int emit_offset_address(nyr_func_t *f, int base, int offset) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_ADD_I64,
                                    .dst = -1,
                                    .a = base,
                                    .b = offset,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .scalar_class = NYIR_SCALAR_POINTER});
}

int main(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int source0 = emit_symbol_address(&f, "source");
  int eight = nyr_opt_const(&f, 8);
  int source1 = emit_offset_address(&f, source0, eight);
  int dest0 = emit_symbol_address(&f, "dest");
  int dest1 = emit_offset_address(&f, dest0, eight);
  int load0 = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = source0,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .debug = {"slp_test.ny", 10, 3}});
  int load1 = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = source1,
                                          .b = -1,
                                          .c = -1,
                                          .d = -1,
                                          .e = -1,
                                          .f = -1,
                                          .debug = {"slp_test.ny", 11, 3}});
  size_t store0_at = f.len;
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = dest0,
                              .b = -1,
                              .c = load0,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .debug = {"slp_test.ny", 20, 3}});
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = dest1,
                              .b = -1,
                              .c = load1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .debug = {"slp_test.ny", 21, 3}});
  int zero = nyr_opt_const(&f, 0);
  nyr_opt_ret(&f, zero);

  bool ok = nyr_opt_verify(&f, "slp-debug/before") && nyr_slp_vectorize(&f) &&
            nyr_opt_verify(&f, "slp-debug/after");
  bool saw_load = false, saw_store = false;
  for (size_t i = 0; ok && i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->op == NYIR_VEC4_LOAD_I64)
      saw_load = in->debug.file && strcmp(in->debug.file, "slp_test.ny") == 0 &&
                 in->debug.line == 10 && in->debug.column == 3;
    if (in->op == NYIR_VEC4_STORE_I64)
      saw_store = in->debug.file &&
                  strcmp(in->debug.file, "slp_test.ny") == 0 &&
                  in->debug.line == 20 && in->debug.column == 3;
  }
  ok = ok && saw_load && saw_store;
  if (!ok) {
    fprintf(stderr, "[slp-debug] vector memory op lost source location\n");
    nyr_dump(stderr, &f, "slp-debug/failure");
  } else {
    (void)store0_at;
    puts("[slp-debug] vector load/store retain source locations");
  }
  nyr_ir_func_free(&f);
  return ok ? 0 : 1;
}
