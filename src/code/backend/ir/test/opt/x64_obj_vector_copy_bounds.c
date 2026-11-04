/*
 * Integer vector fallback copies exactly 128 or 256 bits.
 */
#include "code/backend/object/internal.h"
#include "support.h"

static bool check(unsigned lanes, bool omit_frame) {
#if !defined(__x86_64__)
  (void)lanes;
  (void)omit_frame;
  return true;
#else
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  ir.param_count = 2;
  ir.param_types = calloc(2, sizeof(*ir.param_types));
  if (!ir.param_types)
    return false;
  int src = nyr_opt_emit_imm(&ir, NYIR_LOAD_LOCAL, 0);
  int dst = nyr_opt_emit_imm(&ir, NYIR_LOAD_LOCAL, 1);
  int vector = ir.next_value++;
  nyr_ir_emit(&ir, (nyr_inst_t){.op = lanes == 2 ? NYIR_VEC4_LOAD_I64
                                               : NYIR_VEC8_LOAD_I64,
                                .dst = vector, .a = src, .b = -1,
                                .c = -1, .d = -1, .e = -1, .f = -1});
  int copied = ir.next_value++;
  nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_COPY, .dst = copied,
                                .a = vector, .b = -1, .c = -1,
                                .d = -1, .e = -1, .f = -1});
  nyr_ir_emit(&ir, (nyr_inst_t){.op = lanes == 2 ? NYIR_VEC4_STORE_I64
                                               : NYIR_VEC8_STORE_I64,
                                .dst = -1, .a = dst, .b = copied,
                                .c = -1, .d = -1, .e = -1, .f = -1});
  nyr_opt_ret(&ir, nyr_opt_const(&ir, 0));
  nyr_target_request_t request = nyr_target_request_make(
      NY_NATIVE_BACKEND_X86_64, NY_NATIVE_ABI_SYSV, "x86_64-linux-gnu", false);
  ny_native_target_info_t target = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  bool ok = nyr_opt_verify(&ir, "vector-copy-bounds") &&
            nyr_target_info_init(&target, &request) &&
            nyr_x64_obj_append_function_with_frame(
                &code, defs, &def_count, relocs, &reloc_count, &ir, &target,
                "vector_copy", omit_frame, true, false, false, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count, "vector_copy") : -1;
  void *mapping = MAP_FAILED;
  if (ok && symbol >= 0 && reloc_count == 0)
    mapping = nyr_opt_execmem_alloc(code.len);
  ok = ok && mapping != MAP_FAILED;
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = nyr_opt_execmem_protect(mapping, code.len) == 0;
  }
  if (ok) {
    int64_t src_values[8], dst_values[10];
    for (unsigned i = 0; i < 8; ++i)
      src_values[i] = 100 + i;
    for (unsigned i = 0; i < 10; ++i)
      dst_values[i] = -777;
    typedef int64_t (*copy_fn)(int64_t *, int64_t *);
    copy_fn fn = (copy_fn)((unsigned char *)mapping + defs[symbol].off);
    ok = fn(src_values, dst_values + 1) == 0;
    for (unsigned i = 0; i < 10; ++i) {
      int64_t expected = i >= 1 && i <= lanes ? src_values[i - 1] : -777;
      if (dst_values[i] != expected)
        ok = false;
    }
  }
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_obj_free(&code);
  nyr_ir_func_free(&ir);
  if (!ok)
    fprintf(stderr, "vector-copy-bounds lanes=%u omit-frame=%d: %s\n",
            lanes, omit_frame, err);
  return ok;
#endif
}

int main(void) {
  return check(2, false) && check(4, false) && check(2, true) && check(4, true)
             ? 0 : 1;
}
