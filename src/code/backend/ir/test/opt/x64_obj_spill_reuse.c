/*
 * Spilled NYIR values with disjoint live intervals may share x86-64 stack
 * homes without changing the function's result.
 */
#include "code/backend/object/internal.h"
#include "support.h"

typedef int64_t (*spill_reuse_fn_t)(int64_t);

static int check(void) {
#if !defined(__x86_64__)
  return 0;
#else
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  ir.param_count = 1;
  ir.param_types = malloc(sizeof(*ir.param_types));
  if (!ir.param_types)
    return 2;
  ir.param_types[0] = NYIR_PARAM_I64;

  int first[12], second[12];
  for (int i = 0; i < 12; ++i)
    first[i] = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                             .dst = -1,
                                             .a = -1,
                                             .b = -1,
                                             .c = -1,
                                             .d = -1,
                                             .e = -1,
                                             .f = -1,
                                             .imm = 0});
  int first_sum = first[0];
  for (int i = 1; i < 12; ++i)
    first_sum = nyr_opt_binop(&ir, NYIR_ADD_I64, first_sum, first[i]);
  for (int i = 0; i < 12; ++i)
    second[i] = nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                              .dst = -1,
                                              .a = -1,
                                              .b = -1,
                                              .c = -1,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1,
                                              .imm = 0});
  int second_sum = second[0];
  for (int i = 1; i < 12; ++i)
    second_sum = nyr_opt_binop(&ir, NYIR_ADD_I64, second_sum, second[i]);
  int result = nyr_opt_binop(&ir, NYIR_ADD_I64, first_sum, second_sum);
  nyr_opt_ret(&ir, result);

  ny_options opt = {0};
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  opt.native_abi = NY_NATIVE_ABI_SYSV;
  opt.host_triple = "x86_64-linux-gnu";
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  bool ok = nyr_opt_verify(&ir, "x64-obj-spill-reuse") &&
            nyr_target_info_init(&target, &request) &&
            nyr_x64_obj_append_function_with_frame(
                &code, defs, &def_count, relocs, &reloc_count, &ir, &target,
                "spill_reuse", false, true, false, false, err, sizeof(err));
  int symbol = -1;
  for (size_t i = 0; ok && i < def_count; ++i) {
    if (strcmp(defs[i].name, "spill_reuse") == 0) {
      symbol = (int)i;
      break;
    }
  }
  void *mapping = MAP_FAILED;
  if (ok && symbol >= 0 && reloc_count == 0) {
    mapping = nyr_opt_execmem_alloc(code.len);
    ok = mapping != MAP_FAILED;
  } else {
    ok = false;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = nyr_opt_execmem_protect(mapping, code.len) == 0;
  }
  if (ok) {
    spill_reuse_fn_t fn =
        (spill_reuse_fn_t)((unsigned char *)mapping + defs[symbol].off);
    ok = fn(21) == 504;
    if (!ok)
      fprintf(stderr, "x64 object spill reuse: result=%lld\n",
              (long long)fn(21));
  }
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_obj_free(&code);
  nyr_ir_func_free(&ir);
  if (!ok)
    fprintf(stderr, "x64 object spill reuse: %s\n", err);
  return ok ? 0 : 1;
#endif
}

int main(void) { return check(); }
