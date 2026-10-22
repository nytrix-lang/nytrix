/*
 * Three-operand sums must preserve live inputs and wrapping i64 semantics.
 */
#include "code/backend/ir/machine.h"
#include "code/backend/object/internal.h"
#include "support.h"

static bool check_sum(const ny_native_target_info_t *target, bool registers,
                      int64_t constant) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  nyr_opt_begin(&ir);
  ir.param_count = 2;
  ir.param_types = malloc(2 * sizeof(*ir.param_types));
  if (!ir.param_types)
    return false;
  ir.param_types[0] = ir.param_types[1] = NYIR_PARAM_I64;
  int inputs[2];
  for (int i = 0; i < 2; ++i)
    inputs[i] = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                              .dst = -1,
                                              .a = -1,
                                              .b = -1,
                                              .c = -1,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1,
                                              .imm = i});
  int rhs = registers ? inputs[1] : nyr_opt_const(&ir, constant);
  int sum = nyr_opt_binop(&ir, NYIR_ADD_I64, inputs[0], rhs);
  int result = nyr_opt_binop(&ir, NYIR_XOR_I64, sum, inputs[0]);
  result = nyr_opt_binop(&ir, NYIR_XOR_I64, result, inputs[1]);
  nyr_opt_ret(&ir, result);
  bool ok = nyr_opt_verify(&ir, "machine-add-lea") &&
            nyr_mach_lower_nir(&ir, &mach, target->caps, err, sizeof(err)) &&
            nyr_x64_mach_append_function(&code, defs, &def_count, relocs,
                                         &reloc_count, &mach, target, "add_lea",
                                         false, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count, "add_lea") : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  bool saw_lea = false;
  if (ok) {
    const unsigned char *bytes = code.data + defs[symbol].off;
    for (size_t i = 0; i + 2 < defs[symbol].size; ++i)
      saw_lea |= (bytes[i] & 0xf8) == 0x48 && bytes[i + 1] == 0x8d;
    if (registers || constant == (int64_t)(int32_t)constant)
      ok = saw_lea;
  }
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
    typedef int64_t (*fn_t)(int64_t, int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    const int64_t cases[] = {0, 1, -1, INT64_MIN, INT64_MAX, 129, -129};
    for (size_t a = 0; ok && a < sizeof(cases) / sizeof(cases[0]); ++a)
      for (size_t b = 0; ok && b < sizeof(cases) / sizeof(cases[0]); ++b) {
        uint64_t x = (uint64_t)cases[a], y = (uint64_t)cases[b];
        uint64_t expected = (x + (registers ? y : (uint64_t)constant)) ^ x ^ y;
        ok = (uint64_t)fn(cases[a], cases[b]) == expected;
      }
  }
  if (!ok)
    fprintf(stderr, "[machine-add-lea] registers=%d constant=%lld lea=%d %s\n",
            registers, (long long)constant, saw_lea, err);
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok;
}

int main(void) {
#if !defined(__x86_64__)
  return 0;
#else
  const nyr_target_request_t request = nyr_target_request_make(
      NY_NATIVE_BACKEND_X86_64, NY_NATIVE_ABI_SYSV, "x86_64-linux-gnu", false);
  ny_native_target_info_t target = {0};
  bool ok =
      nyr_target_info_init(&target, &request) && check_sum(&target, true, 0);
  const int64_t constants[] = {0,         1,         -1,       127,
                               128,       -128,      -129,     INT32_MIN,
                               INT32_MAX, INT64_MIN, INT64_MAX};
  for (size_t i = 0; ok && i < sizeof(constants) / sizeof(constants[0]); ++i)
    ok = check_sum(&target, false, constants[i]);
  return ok ? 0 : 1;
#endif
}
