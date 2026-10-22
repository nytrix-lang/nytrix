/*
 * Execute a high-pressure store through the actual x86-64 object encoder.
 */
#include "code/backend/ir/machine.h"
#include "code/backend/object/internal.h"
#include "support.h"
#include <sys/mman.h>

typedef int64_t (*gpr_kernel_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                                int64_t, int64_t, int64_t, int64_t, int64_t,
                                int64_t);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int64_t call_sysv_kernel(gpr_kernel_t kernel, int64_t *stored) {
  return kernel(1, 2, 3, 4, 5, 6, 7, 8, 9, (int64_t)(uintptr_t)stored, 11);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int64_t call_win64_kernel(gpr_kernel_t kernel, int64_t *stored) {
  typedef int64_t(__attribute__((ms_abi)) * win_kernel_t)(
      int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
      int64_t, int64_t, int64_t);
  win_kernel_t win_kernel = (win_kernel_t)kernel;
  return win_kernel(1, 2, 3, 4, 5, 6, 7, 8, 9, (int64_t)(uintptr_t)stored, 11);
}

static int check(nyr_abi_t abi) {
#if !defined(__x86_64__)
  return 0;
#else
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  ir.param_count = 11;
  ir.param_types = malloc(ir.param_count * sizeof(*ir.param_types));
  if (!ir.param_types)
    return 2;
  for (size_t i = 0; i < ir.param_count; ++i)
    ir.param_types[i] = NYIR_PARAM_I64;
  int values[11];
  for (int i = 0; i < 11; ++i)
    values[i] = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                             .dst = -1,
                                             .a = -1,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .imm = i});
  nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = values[9],
                               .b = -1,
                               .c = values[10],
                               .d = -1,
                               .e = -1,
                               .f = -1});
  int sum = values[10];
  for (int i = 0; i < 9; ++i)
    sum = nyr_opt_binop(&ir, NYIR_ADD_I64, sum, values[i]);
  nyr_opt_ret(&ir, sum);
  ny_options opt = {0};
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  opt.native_abi = abi;
  opt.host_triple = "x86_64-linux-gnu";
  const nyr_target_request_t target_request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  nyr_mach_func_t mach = {0};
  nyr_mach_func_t entry = {0};
  nyr_func_t entry_ir;
  nyr_opt_begin(&entry_ir);
  nyr_opt_ret(&entry_ir, nyr_opt_const(&entry_ir, 0));
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[8] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  bool ok =
      nyr_opt_verify(&ir, "gpr-scratch") &&
      nyr_target_info_init(&target, &target_request) &&
      nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err)) &&
      nyr_mach_lower_nir(&entry_ir, &entry, target.caps, err, sizeof(err)) &&
      nyr_x64_mach_build_bundle(&entry, &mach, (const char *const[]){"pressure"},
                               1, &target, "rt_main", false, &code, defs,
                               &def_count, relocs, &reloc_count, err,
                               sizeof(err));
  int symbol =
      ok ? nyr_x64_obj_def_index(defs, def_count, "ny_fn_pressure") : -1;
  ok = ok && symbol >= 0;
  void *mapping = MAP_FAILED;
  if (ok && !reloc_count) {
    mapping = mmap(NULL, code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = mapping != MAP_FAILED;
  } else {
    ok = false;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = mprotect(mapping, code.len, PROT_READ | PROT_EXEC) == 0;
  }
  if (ok) {
    gpr_kernel_t kernel =
        (gpr_kernel_t)((unsigned char *)mapping + defs[symbol].off);
    int64_t stored = 0;
    int64_t result = abi == NY_NATIVE_ABI_WIN64
                         ? call_win64_kernel(kernel, &stored)
                         : call_sysv_kernel(kernel, &stored);
    ok = result == 56 && stored == 11;
    if (!ok)
      fprintf(stderr, "gpr scratch: result=%lld stored=%lld\n",
              (long long)result, (long long)stored);
  }
  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_mach_func_free(&entry);
  nyr_ir_func_free(&entry_ir);
  nyr_ir_func_free(&ir);
  if (!ok)
    fprintf(stderr, "gpr scratch: %s\n", err);
  return ok ? 0 : 1;
#endif
}

int main(void) {
  return check(NY_NATIVE_ABI_SYSV) || check(NY_NATIVE_ABI_WIN64);
}
