/*
 * Machine lowering and x86-64 object encoding preserve float-to-i64 truncation.
 */
#include "code/backend/ir/machine.h"
#include "code/backend/object/internal.h"
#include "support.h"

static int64_t f64_bits(double value) {
  int64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static int64_t f32_bits(float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return (int64_t)bits;
}

static int emit_const(nyr_func_t *ir, nyr_op_t op, int64_t bits) {
  return nyr_ir_emit(ir, (nyr_inst_t){.op = op,
                                      .dst = -1,
                                      .a = -1,
                                      .b = -1,
                                      .c = -1,
                                      .d = -1,
                                      .e = -1,
                                      .f = -1,
                                      .imm = bits});
}

static int emit_unary(nyr_func_t *ir, nyr_op_t op, int value) {
  return nyr_ir_emit(ir, (nyr_inst_t){.op = op,
                                      .dst = -1,
                                      .a = value,
                                      .b = -1,
                                      .c = -1,
                                      .d = -1,
                                      .e = -1,
                                      .f = -1});
}

int main(void) {
#if !defined(__x86_64__)
  return 0;
#else
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_writer_t assembly = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  nyr_opt_begin(&ir);

  int f64 = emit_const(&ir, NYIR_CONST_F64, f64_bits(12.75));
  int f64_i64 = f64 < 0 ? -1 : emit_unary(&ir, NYIR_F64_TO_I64, f64);
  int f32 = emit_const(&ir, NYIR_CONST_F32, f32_bits(5.75f));
  int f32_i64 = f32 < 0 ? -1 : emit_unary(&ir, NYIR_F32_TO_I64, f32);
  int sum = f64_i64 < 0 || f32_i64 < 0
                ? -1
                : nyr_opt_binop(&ir, NYIR_ADD_I64, f64_i64, f32_i64);
  if (sum >= 0)
    nyr_opt_ret(&ir, sum);

  ny_options opt = {0};
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  opt.native_abi = NY_NATIVE_ABI_SYSV;
  opt.host_triple = "x86_64-linux-gnu";
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  bool ok =
      f64 >= 0 && f64_i64 >= 0 && f32 >= 0 && f32_i64 >= 0 && sum >= 0 &&
      nyr_opt_verify(&ir, "machine-float-to-i64") &&
      nyr_target_info_init(&target, &request) &&
      nyr_mach_opcode_supported(NYIR_F64_TO_I64) &&
      nyr_mach_opcode_supported(NYIR_F32_TO_I64) &&
      nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err)) &&
      nyr_mach_verify(&mach, target.caps, err, sizeof(err)) &&
      nyr_x86_64_emit_mach_scalar(&assembly, &target, &mach, "float_to_i64",
                                  false, err, sizeof(err)) &&
      nyr_x64_mach_append_function(&code, defs, &def_count, relocs,
                                   &reloc_count, &mach, &target, "float_to_i64",
                                   false, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count, "float_to_i64") : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  void *mapping = MAP_FAILED;
  if (ok) {
    mapping = nyr_opt_execmem_alloc(code.len);
    ok = mapping != MAP_FAILED;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = nyr_opt_execmem_protect(mapping, code.len) == 0;
  }
  if (ok) {
    typedef int64_t (*fn_t)(void);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    ok = fn() == 17;
  }
  if (!ok)
    fprintf(stderr, "[machine-float-to-i64] failed: %s\n",
            err[0] ? err : "conversion result or emission mismatch");
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  free(assembly.data);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok ? 0 : 1;
#endif
}
