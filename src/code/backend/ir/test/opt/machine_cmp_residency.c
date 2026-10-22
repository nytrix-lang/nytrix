/*
 * Exercise direct compares through the x86-64 object machine encoder.
 */
#include "code/backend/ir/machine.h"
#include "code/backend/object/internal.h"
#include "support.h"
#include <sys/mman.h>

typedef int64_t (*cmp_fn_t)(int64_t, int64_t);

static bool build_cmp(const ny_native_target_info_t *target, int64_t constant,
                      bool compare_params, nyr_obj_buf_t *code,
                      nyr_x64_obj_symbol_def_t *defs,
                      nyr_x64_obj_reloc_t *relocs, size_t *def_count,
                      size_t *reloc_count, char *err, size_t err_size) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_opt_begin(&ir);
  ir.param_count = 2;
  ir.param_types = malloc(2 * sizeof(*ir.param_types));
  if (!ir.param_types)
    return false;
  ir.param_types[0] = ir.param_types[1] = NYIR_PARAM_I64;
  int lhs = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1, .a = -1, .b = -1,
                                         .c = -1, .d = -1, .e = -1,
                                         .f = -1, .imm = 0});
  int rhs = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1, .a = -1, .b = -1,
                                         .c = -1, .d = -1, .e = -1,
                                         .f = -1, .imm = 1});
  int bound = nyr_opt_const(&ir, constant);
  int result = nyr_opt_cmp(&ir, lhs, compare_params ? rhs : bound,
                           NYIR_CMP_LT);
  nyr_opt_ret(&ir, result);
  bool ok = nyr_opt_verify(&ir, "machine-cmp-residency") &&
            nyr_mach_lower_nir(&ir, &mach, target->caps, err, err_size) &&
            nyr_x64_mach_append_function(code, defs, def_count, relocs,
                                         reloc_count, &mach, target,
                                         "cmp_residency", false, err,
                                         err_size);
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
  if (!nyr_target_info_init(&target, &request))
    return 2;

  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  bool ok = build_cmp(&target, 200, false, &code, defs, relocs, &def_count,
                      &reloc_count, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count, "cmp_residency") : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  size_t direct_imm32 = 0;
  if (ok) {
    const unsigned char *bytes = code.data + defs[symbol].off;
    for (size_t i = 0; i + 6 < defs[symbol].size; ++i)
      if ((bytes[i] & 0xf8) == 0x48 && bytes[i + 1] == 0x81 &&
          (bytes[i + 2] & 0xf8) == 0xf8 && bytes[i + 3] == 200 &&
          bytes[i + 4] == 0 && bytes[i + 5] == 0 && bytes[i + 6] == 0)
        ++direct_imm32;
    ok = direct_imm32 == 1;
  }

  void *mapping = MAP_FAILED;
  if (ok) {
    mapping = mmap(NULL, code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = mapping != MAP_FAILED;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = mprotect(mapping, code.len, PROT_READ | PROT_EXEC) == 0;
  }
  if (ok) {
    cmp_fn_t fn = (cmp_fn_t)((unsigned char *)mapping + defs[symbol].off);
    ok = fn(199, 0) == 1 && fn(200, 0) == 0 && fn(201, 0) == 0;
  }
  if (!ok)
    fprintf(stderr,
            "[machine-cmp-residency] failed: direct_imm32=%zu error=%s\n",
            direct_imm32, err[0] ? err : "runtime or assertion");
  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  return ok ? 0 : 1;
#endif
}
