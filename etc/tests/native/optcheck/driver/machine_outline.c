/*
 * Regression driver for x86-64 size-tier machine-function outlining.
 */
#include "code/native/object/internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool build_constant(ny_mach_func_t *func, int64_t value) {
  ny_mach_reg_t dst;
  uint32_t block;
  return ny_mach_begin_block(func, 0, &block) &&
         ny_mach_alloc_typed_vreg(func, NY_MACH_TYPE_I64, &dst) &&
         ny_mach_emit(func, (ny_mach_inst_t){
             .opcode = NY_MACH_COPY,
             .dst = {.kind = NY_MACH_OPERAND_VREG,
                     .reg_class = NY_MACH_REGCLASS_GPR,
                     .as.reg = dst},
             .src0 = {.kind = NY_MACH_OPERAND_IMM, .as.imm = value}}) &&
         ny_mach_emit(func, (ny_mach_inst_t){.opcode = NY_MACH_RET,
                                             .effects = NY_MACH_EFFECT_CONTROL});
}

int main(void) {
  ny_options opt = {0};
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  opt.native_abi = NY_NATIVE_ABI_SYSV;
  opt.host_triple = "x86_64-linux-gnu";
  opt.opt_size = true;
  ny_native_target_info_t target = {0};
  ny_mach_func_t main_func = {0};
  ny_mach_func_t funcs[2] = {{0}};
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[8] = {0};
  ny_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};

  bool ok = ny_native_target_info_init(&target, &opt) &&
            build_constant(&main_func, 0) && build_constant(&funcs[0], 7) &&
            build_constant(&funcs[1], 7) &&
            ny_x64_mach_build_bundle(&main_func, funcs,
                                     (const char *const[]){"first", "second"},
                                     2, &target, "rt_main", false, &code, defs,
                                     &def_count, relocs, &reloc_count, err,
                                     sizeof(err));
  int first = ok ? ny_x64_obj_def_index(defs, def_count, "ny_fn_first") : -1;
  int second = ok ? ny_x64_obj_def_index(defs, def_count, "ny_fn_second") : -1;
  int thunk = first >= 0 && defs[first].size == 5 ? first : second;
  int body = thunk == first ? second : first;
  int32_t displacement = 0;
  if (ok && thunk >= 0 && body >= 0 && defs[first].off != defs[second].off &&
      code.data[defs[thunk].off] == 0xe9)
    memcpy(&displacement, code.data + defs[thunk].off + 1, sizeof(displacement));
  else
    ok = false;
  if (ok && (defs[body].size <= defs[thunk].size || reloc_count != 0 ||
             (int64_t)defs[thunk].off + 5 + displacement !=
                 (int64_t)defs[body].off))
    ok = false;

  ny_mach_func_free(&main_func);
  ny_mach_func_free(&funcs[0]);
  ny_mach_func_free(&funcs[1]);
  ny_obj_free(&code);
  if (!ok) {
    fprintf(stderr, "[machine-outline] %s\n", err[0] ? err : "bad thunk");
    return 1;
  }
  printf("[machine-outline] distinct symbols share one body through a thunk\n");
  return 0;
}
