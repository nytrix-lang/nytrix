/*
 * A sole unconditional edge to the next block can be merged without changing
 * control-flow results; edges with another predecessor must remain distinct.
 */
#include "code/backend/ir/machine.h"
#include "code/backend/object/internal.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool build_function(nyr_func_t *ir, bool extra_predecessor) {
  nyr_opt_begin(ir);
  ir->param_count = 1;
  ir->param_types = malloc(sizeof(*ir->param_types));
  if (!ir->param_types)
    return false;
  ir->param_types[0] = NYIR_PARAM_I64;

  int arg = nyr_ir_emit(ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 0});
  if (arg < 0)
    return false;
  if (extra_predecessor) {
    int zero = nyr_opt_const(ir, 0);
    int take_target = nyr_opt_cmp(ir, arg, zero, NYIR_CMP_NE);
    nyr_opt_br_if(ir, take_target, 20);
  } else {
    nyr_opt_br(ir, 10);
  }
  nyr_opt_label(ir, 10);
  nyr_opt_br(ir, 20);
  nyr_opt_label(ir, 20);
  nyr_opt_ret(ir, arg);
  return nyr_opt_verify(ir, "machine-fallthrough-merge");
}

static bool run_case(const ny_native_target_info_t *target,
                     bool extra_predecessor) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  bool ok = build_function(&ir, extra_predecessor);
  if (ok)
    ok = nyr_mach_lower_nir(&ir, &mach, target->caps, err, sizeof(err));
  if (ok && !extra_predecessor && mach.block_len != 1) {
    fprintf(stderr,
            "[machine-fallthrough-merge] unique forward edge left %zu blocks\n",
            mach.block_len);
    nyr_mach_dump(&mach, stderr);
    ok = false;
  }
  if (ok)
    ok = nyr_x64_mach_append_function(
        &code, defs, &def_count, relocs, &reloc_count, &mach, target,
        "fallthrough_merge", false, err, sizeof(err));
  int symbol =
      ok ? nyr_x64_obj_def_index(defs, def_count, "fallthrough_merge") : -1;
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
    typedef int64_t (*fn_t)(int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    const int64_t inputs[] = {-1, 0, 1, INT64_MAX};
    for (size_t i = 0; ok && i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
      int64_t actual = fn(inputs[i]);
      ok = actual == inputs[i];
      if (!ok)
        fprintf(stderr,
                "[machine-fallthrough-merge] extra_predecessor=%d "
                "input=%lld result=%lld\n",
                extra_predecessor, (long long)inputs[i], (long long)actual);
    }
  }
  if (!ok && err[0])
    fprintf(stderr, "[machine-fallthrough-merge] %s\n", err);
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
  ny_options opt = {0};
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  opt.native_abi = NY_NATIVE_ABI_SYSV;
  opt.host_triple = "x86_64-linux-gnu";
  nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  bool ok = nyr_target_info_init(&target, &request) &&
            run_case(&target, false) && run_case(&target, true);
  if (ok)
    puts("[machine-fallthrough-merge] unique forward edges merge; duplicate "
         "predecessors preserve results");
  return ok ? 0 : 1;
#endif
}
