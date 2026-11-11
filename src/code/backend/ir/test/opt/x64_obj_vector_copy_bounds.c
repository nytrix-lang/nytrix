/*
 * Integer vector fallback copies exactly 128 or 256 bits.
 */
#include "code/backend/object/internal.h"
#include "support.h"
#include "code/backend/ir/machine.h"

static bool check(unsigned lanes, bool omit_frame, bool convert) {
#if !defined(__x86_64__)
  (void)lanes;
  (void)omit_frame;
  (void)convert;
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
  nyr_ir_emit(&ir, (nyr_inst_t){.op = convert ? NYIR_VEC4_ADD_I64 : NYIR_COPY,
                                .dst = copied,
                                .a = vector, .b = convert ? vector : -1, .c = -1,
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
  nyr_mach_func_t mach = {0};
  bool ok = nyr_opt_verify(&ir, "vector-copy-bounds") &&
            nyr_target_info_init(&target, &request);
  if (ok && convert) {
    ok = nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err));
    bool found = false;
    for (size_t i = 0; ok && i < mach.inst_len; ++i) {
      nyr_mach_inst_t *in = &mach.insts[i];
      if (in->opcode != NYR_MACH_ADD ||
          in->dst.kind != NYR_MACH_OPERAND_VREG ||
          mach.vreg_types[in->dst.as.reg] != NYR_MACH_TYPE_V128_I64)
        continue;
      in->opcode = NYR_MACH_CONVERT;
      in->src1 = (nyr_mach_operand_t){0};
      mach.vreg_types[in->dst.as.reg] = NYR_MACH_TYPE_V128_F64;
      found = true;
    }
    ok = ok && found && nyr_x64_mach_append_function(
        &code, defs, &def_count, relocs, &reloc_count, &mach, &target,
        "vector_copy", false, err, sizeof(err));
  } else if (ok) {
    ok = nyr_x64_obj_append_function_with_frame(
                &code, defs, &def_count, relocs, &reloc_count, &ir, &target,
                "vector_copy", omit_frame, true, false, false, err, sizeof(err));
  }
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
    if (convert) {
      src_values[0] = INT64_MIN;
      src_values[1] = INT64_MAX;
    }
    for (unsigned i = 0; i < 10; ++i)
      dst_values[i] = -777;
    typedef int64_t (*copy_fn)(int64_t *, int64_t *);
    copy_fn fn = (copy_fn)((unsigned char *)mapping + defs[symbol].off);
    ok = fn(src_values, dst_values + 1) == 0;
    for (unsigned i = 0; i < 10; ++i) {
      int64_t expected = i >= 1 && i <= lanes ? src_values[i - 1] : -777;
      if (convert && i >= 1 && i <= lanes) {
        double lane = (double)src_values[i - 1];
        memcpy(&expected, &lane, sizeof(expected));
      }
      if (dst_values[i] != expected)
        ok = false;
    }
    if (convert) {
      const int64_t cases[][2] = {
          {0, -1}, {1, -2}, {INT64_MIN, INT64_MAX},
          {INT64_C(9007199254740991), INT64_C(9007199254740993)},
          {-INT64_C(9007199254740993), -INT64_C(9007199254740995)},
          {INT64_C(4503599627370495), -INT64_C(4503599627370495)}};
      for (size_t c = 0; ok && c < sizeof(cases) / sizeof(cases[0]); ++c) {
        memcpy(src_values, cases[c], sizeof(cases[c]));
        ok = fn(src_values, dst_values + 1) == 0;
        for (unsigned lane = 0; ok && lane < 2; ++lane) {
          double expected = (double)src_values[lane], actual;
          memcpy(&actual, &dst_values[lane + 1], sizeof(actual));
          ok = actual == expected;
        }
        ok = ok && dst_values[0] == -777 && dst_values[3] == -777;
      }
    }
  }
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  if (!ok)
    fprintf(stderr, "vector-copy-bounds lanes=%u omit-frame=%d: %s\n",
            lanes, omit_frame, err);
  return ok;
#endif
}

int main(void) {
  return check(2, false, false) && check(4, false, false) &&
         check(2, true, false) && check(4, true, false) &&
         check(2, false, true)
             ? 0 : 1;
}
