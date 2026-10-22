/*
 * Focused repro: a vectorized reduction loop (VEC8_LOAD_I64 +
 * VEC8_REDUCE_ADD_I64 over a loop-carried scalar accumulator) executed
 * through the x86-64 machine form. The VM evaluates the same NYIR
 * correctly; this checks whether the machine lowering agrees.
 */
#if defined(__linux__) && defined(__x86_64__)

#include "code/backend/internal.h"
#include "code/backend/object/internal.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int emit(nyr_func_t *f, nyr_op_t op, int a, int b, int c,
                int64_t imm) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = op,
                                    .dst = -1,
                                    .a = a,
                                    .b = b,
                                    .c = c,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = imm});
}

int main(void) {
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  ir.param_count = 1;
  ir.param_types = malloc(sizeof(*ir.param_types));
  if (ir.param_types)
    ir.param_types[0] = NYIR_PARAM_I64;

  /*
   * L0 (label 0):
   *   v_ck = phi [0, entry], [red, latch]
   *   v_i  = phi [0, entry], [next, latch]   ; step 4
   *   if v_i < 32 goto L1 else goto L2
   * L1 (label 1):
   *   addr = dk + (v_i << 3)
   *   vec  = vec8.load.i64 addr
   *   red  = vec8.reduce_add.i64 v_ck, vec
   *   next = v_i + 4
   *   goto L0
   * L2 (label 2): ret v_ck
   */
  int zero = nyr_opt_const(&ir, 0);
  int limit = nyr_opt_const(&ir, 32);
  int three = nyr_opt_const(&ir, 3);
  int four = nyr_opt_const(&ir, 4);
  int dk = emit(&ir, NYIR_LOAD_LOCAL, -1, -1, -1, 0);
  nyr_opt_br(&ir, 10);
  /*
   * Header with the accumulator and step-4 induction PHIs.
   */
  nyr_opt_label(&ir, 10);
  int iv = ir.next_value;
  nyr_opt_phi(&ir, -1, zero, 0, zero); /* backedge patched below */
  int acc = ir.next_value;
  nyr_opt_phi(&ir, -1, zero, 0, zero); /* backedge patched below */
  int c = nyr_opt_cmp(&ir, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(&ir, c, 11);
  nyr_opt_br(&ir, 12);
  /*
   * Vector latch: one four-lane load and reduce per iteration.
   */
  nyr_opt_label(&ir, 11);
  int scaled = nyr_opt_binop(&ir, NYIR_SHL_I64, iv, three);
  int addr = nyr_opt_binop(&ir, NYIR_ADD_I64, dk, scaled);
  int vec = emit(&ir, NYIR_VEC8_LOAD_I64, addr, -1, -1, 0);
  int red = emit(&ir, NYIR_VEC8_REDUCE_ADD_I64, acc, vec, -1, 0);
  int next_i = nyr_opt_binop(&ir, NYIR_ADD_I64, iv, four);
  nyr_opt_br(&ir, 10);
  nyr_opt_label(&ir, 12);
  nyr_opt_ret(&ir, acc);

  /*
   * Backpatch the latch inputs now that the body values exist.
   */
  for (size_t k = 0; k < ir.len; ++k) {
    if (ir.data[k].op != NYIR_PHI)
      continue;
    ir.data[k].phi_incoming[1].predecessor_label = 11;
    ir.data[k].phi_incoming[1].value = ir.data[k].dst == iv ? next_i : red;
  }

  char err[256] = {0};
  bool ok = nyr_opt_verify(&ir, "vec8-reduce-loop");
  if (!ok)
    return 1;

  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  ny_options opt;
  ny_options_init(&opt);
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  const nyr_target_request_t target_request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target;
  bool target_ok = nyr_target_info_init(&target, &target_request);
  bool lowered =
      target_ok && nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err));
  bool appended =
      lowered && nyr_x64_mach_append_function(
                     &code, defs, &def_count, relocs, &reloc_count, &mach,
                     &target, "vec8_reduce_loop", false, err, sizeof(err));
  ok = appended;
  int symbol =
      ok ? nyr_x64_obj_def_index(defs, def_count, "vec8_reduce_loop") : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
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
  int64_t machine_result = -1;
  if (ok) {
    int64_t cells[32];
    for (int i = 0; i < 32; ++i)
      cells[i] = i;
    typedef int64_t (*fn_t)(int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    machine_result = fn((int64_t)(intptr_t)cells);
    ok = machine_result == 496;
  }
  if (!ok)
    fprintf(stderr, "[vec8-reduce-loop] machine=%lld expected=496 (%s)\n",
            (long long)machine_result, err[0] ? err : "runtime mismatch");
  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok ? 0 : 1;
}

#else

#include <stdio.h>

int main(void) {
  puts("[vec8-reduce-loop] skipped outside Linux x86-64");
  return 0;
}

#endif
