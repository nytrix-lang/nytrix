/*
 * Latch guard duplication preserves zero trips and live-out PHI values.
 */
#include "code/backend/ir/machine.h"
#include "code/backend/object/internal.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool check(const ny_native_target_info_t *target, unsigned guard_mode) {
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  ir.param_count = 1;
  ir.param_types = malloc(sizeof(*ir.param_types));
  if (!ir.param_types)
    return false;
  ir.param_types[0] = NYIR_PARAM_I64;
  int limit = nyr_opt_emit_imm(&ir, NYIR_LOAD_LOCAL, 0);
  int zero = nyr_opt_const(&ir, 0), one = nyr_opt_const(&ir, 1);
  nyr_opt_br(&ir, 10);
  nyr_opt_label(&ir, 10);
  size_t iv_pc = ir.len;
  nyr_opt_phi(&ir, -1, zero, 20, zero);
  int iv = ir.data[iv_pc].dst;
  size_t sum_pc = ir.len;
  nyr_opt_phi(&ir, -1, zero, 20, zero);
  int sum = ir.data[sum_pc].dst;
  if (guard_mode == 1)
    limit = nyr_opt_emit_imm(&ir, NYIR_LOAD_LOCAL, 0);
  else if (guard_mode == 2)
    limit = nyr_opt_binop(&ir, NYIR_DIV_I64, nyr_opt_const(&ir, 16), limit);
  int cond = nyr_opt_cmp(&ir, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(&ir, cond, 20);
  nyr_opt_label(&ir, 15);
  nyr_opt_br(&ir, 30);
  nyr_opt_label(&ir, 20);
  int next_sum = nyr_opt_binop(&ir, NYIR_ADD_I64, sum, iv);
  int next_iv = nyr_opt_binop(&ir, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&ir, 10);
  ir.data[iv_pc].phi_incoming[1].value = next_iv;
  ir.data[sum_pc].phi_incoming[1].value = next_sum;
  nyr_opt_label(&ir, 30);
  int result = nyr_opt_binop(&ir, NYIR_ADD_I64, sum, iv);
  nyr_opt_ret(&ir, result);

  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  bool ok = nyr_opt_verify(&ir, "machine-loop-rotate") &&
            nyr_mach_lower_nir(&ir, &mach, target->caps, err, sizeof(err));
  bool self_branch = false;
  for (size_t b = 0; ok && b < mach.block_len; ++b) {
    const nyr_mach_block_t *block = &mach.blocks[b];
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &mach.insts[block->first_inst + n];
      self_branch |= in->opcode == NYR_MACH_BR_IF &&
                     in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
                     in->src1.as.block_index == b;
    }
  }
  ok = ok && self_branch == (guard_mode == 0);
  if (ok)
    ok = nyr_x64_mach_append_function(
        &code, defs, &def_count, relocs, &reloc_count, &mach, target,
        "rotate", false, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count, "rotate") : -1;
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
    const int64_t inputs[] = {INT64_MIN, -1, 0, 1, 2, 3, 17, 1000};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
      int64_t n = inputs[i];
      if (guard_mode == 2 && n == 0)
        continue; /* Deliberately trapping input; guard must remain un-cloned. */
      int64_t trips = guard_mode == 2 ? 16 / n : n;
      int64_t want = trips <= 0 ? 0 : trips * (trips + 1) / 2;
      int64_t actual = fn(n);
      if (actual != want) {
        fprintf(stderr, "[machine-loop-rotate] guard=%u n=%lld got=%lld want=%lld\n",
                guard_mode, (long long)n, (long long)actual, (long long)want);
        ok = false;
      }
    }
  }
  if (!ok) {
    fprintf(stderr, "[machine-loop-rotate] guard=%u self-branch=%d error=%s\n",
            guard_mode, self_branch, err);
    nyr_mach_dump(&mach, stderr);
  }
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
  nyr_target_request_t request = nyr_target_request_make(
      NY_NATIVE_BACKEND_X86_64, NY_NATIVE_ABI_SYSV, "x86_64-linux-gnu", 0);
  ny_native_target_info_t target = {0};
  bool ok = nyr_target_info_init(&target, &request) &&
            check(&target, 0) && check(&target, 1) && check(&target, 2);
  if (ok)
    puts("[machine-loop-rotate] zero/one/many trips and exit PHIs pass; memory and trapping guards retained");
  return ok ? 0 : 1;
#endif
}
