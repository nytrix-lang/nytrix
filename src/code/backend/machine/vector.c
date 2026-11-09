/*
 * Native vector codegen: packed SIMD (v128) lowering for x86-64 SSE
 * and AArch64 NEON, including register allocation and instruction selection.
 */
#include "code/backend/internal.h"
#include "code/backend/object/internal.h"
#include <stdio.h>
#include <string.h>

static nyr_mach_operand_t selftest_vreg(nyr_mach_reg_t reg) {
  nyr_mach_operand_t op = {0};
  op.kind = NYR_MACH_OPERAND_VREG;
  op.reg_class = NYR_MACH_REGCLASS_VECTOR;
  op.as.reg = reg;
  return op;
}

static nyr_mach_operand_t selftest_gpr_vreg(nyr_mach_reg_t reg) {
  nyr_mach_operand_t op = {0};
  op.kind = NYR_MACH_OPERAND_VREG;
  op.reg_class = NYR_MACH_REGCLASS_GPR;
  op.as.reg = reg;
  return op;
}

static nyr_mach_operand_t selftest_fpr_vreg(nyr_mach_reg_t reg) {
  nyr_mach_operand_t op = {0};
  op.kind = NYR_MACH_OPERAND_VREG;
  op.reg_class = NYR_MACH_REGCLASS_FPR;
  op.as.reg = reg;
  return op;
}

static nyr_mach_operand_t selftest_frame(uint32_t slot) {
  nyr_mach_operand_t op = {0};
  op.kind = NYR_MACH_OPERAND_FRAME;
  op.as.frame_index = slot;
  return op;
}

static nyr_mach_operand_t selftest_block(uint32_t block) {
  nyr_mach_operand_t op = {0};
  op.kind = NYR_MACH_OPERAND_BLOCK;
  op.as.block_index = block;
  return op;
}

static nyr_mach_operand_t selftest_symbol(const char *symbol) {
  nyr_mach_operand_t op = {0};
  op.kind = NYR_MACH_OPERAND_SYMBOL;
  op.as.symbol = symbol;
  return op;
}

typedef bool (*vector_bundle_builder_t)(
    const nyr_mach_func_t *, const nyr_mach_func_t *, const char *const *, size_t,
    const ny_native_target_info_t *, const char *, bool, nyr_obj_buf_t *,
    nyr_x64_obj_symbol_def_t *, size_t *, nyr_x64_obj_reloc_t *, size_t *, char *,
    size_t);

static bool vector_target_init(ny_native_target_info_t *target,
                               nyr_backend_t backend, const char *triple) {
  const nyr_target_request_t request =
      nyr_target_request_make(backend, NY_NATIVE_ABI_SYSV, triple, false);
  return nyr_target_info_init(target, &request);
}

/*
 * Exercise the same live vector across a straight-line store, CFG edge,
 * or scalar call. Keep target-specific opcode and reload checks in callers.
 */
static bool vector_sum_program(nyr_mach_func_t *mach,
                               nyr_mach_opcode_t barrier) {
  uint32_t block = 0, slots[3] = {0};
  nyr_mach_reg_t values[3] = {0};
  if (!nyr_mach_begin_block(mach, 0, &block))
    return false;
  for (size_t i = 0; i < 3; ++i)
    if (!nyr_mach_alloc_typed_frame_slot(mach, 16, 16, NYR_MACH_TYPE_V128_F64,
                                         false, &slots[i]))
      return false;
  for (size_t i = 0; i < 3; ++i)
    if (!nyr_mach_alloc_typed_vreg(mach, NYR_MACH_TYPE_V128_F64, &values[i]))
      return false;
  for (size_t i = 0; i < 2; ++i)
    if (!nyr_mach_emit(
            mach, (nyr_mach_inst_t){.opcode = NYR_MACH_LOAD,
                                    .dst = selftest_vreg(values[i]),
                                    .src0 = selftest_frame(slots[i]),
                                    .effects = NYR_MACH_EFFECT_READ_MEMORY}))
      return false;
  if (!nyr_mach_emit(mach, (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                                             .dst = selftest_vreg(values[2]),
                                             .src0 = selftest_vreg(values[0]),
                                             .src1 = selftest_vreg(values[1])}))
    return false;
  if (barrier == NYR_MACH_BR) {
    if (!nyr_mach_emit(mach,
                       (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                         .src1 = selftest_block(1),
                                         .effects = NYR_MACH_EFFECT_CONTROL}) ||
        !nyr_mach_begin_block(mach, 1, &block) || block != 1)
      return false;
  } else if (barrier == NYR_MACH_CALL) {
    if (!nyr_mach_emit(
            mach, (nyr_mach_inst_t){.opcode = NYR_MACH_CALL,
                                    .src0 = selftest_symbol("rt_test_noop"),
                                    .call_is_extern = true,
                                    .effects = NYR_MACH_EFFECT_CALL}))
      return false;
  }
  return nyr_mach_emit(
             mach,
             (nyr_mach_inst_t){.opcode = NYR_MACH_STORE,
                               .dst = selftest_frame(slots[2]),
                               .src0 = selftest_vreg(values[2]),
                               .effects = NYR_MACH_EFFECT_WRITE_MEMORY}) &&
         nyr_mach_emit(mach,
                       (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                         .effects = NYR_MACH_EFFECT_CONTROL});
}

static bool vector_backend_selftest(nyr_backend_t backend,
                                    vector_bundle_builder_t build,
                                    const char *label, char *err,
                                    size_t err_len) {
  const char *triple = backend == NY_NATIVE_BACKEND_AARCH64
                           ? "aarch64-linux-gnu"
                           : "x86_64-linux-gnu";
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, backend, triple)) {
    nyr_set_error(err, err_len,
                      "%s vector selftest: target initialization failed",
                      label);
    return false;
  }

  nyr_mach_func_t mach = {0};
  bool ok = vector_sum_program(&mach, NYR_MACH_NOP);
  if (ok && !nyr_mach_verify(&mach, 0, err, err_len))
    ok = false;

  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[16] = {0};
  nyr_x64_obj_reloc_t relocs[16] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !build(&mach, NULL, NULL, 0, &target, "vector_selftest", false,
                   &code, defs, &def_count, relocs, &reloc_count, err, err_len))
    ok = false;

  bool has_load = false, has_add = false;
  if (backend == NY_NATIVE_BACKEND_X86_64) {
    for (size_t i = 0; ok && i + 2 < code.len; ++i) {
      if (code.data[i] == 0xf3 && code.data[i + 1] == 0x0f &&
          code.data[i + 2] == 0x6f)
        has_load = true;
      if (code.data[i] == 0x66 && code.data[i + 1] == 0x0f &&
          code.data[i + 2] == 0x58)
        has_add = true;
    }
  } else {
    for (size_t i = 0; ok && i + 4 <= code.len; i += 4) {
      uint32_t word = 0;
      memcpy(&word, code.data + i, sizeof(word));
      if ((word & 0xffe00000u) == 0x3cc00000u)
        has_load = true;
      /*
       * add v2.2d,v0.2d,v1.2d: the non-zero destination proves this was
       * the allocated Q-register path rather than the q0 scratch fallback.
       */
      if ((word & 0xffe0fc00u) == 0x4e60d400u && (word & 31u) != 0)
        has_add = true;
    }
  }
  if (!ok) {
    nyr_obj_free(&code);
    nyr_mach_func_free(&mach);
    return false;
  }
  if (!has_load || !has_add) {
    char detail[128] = {0};
    size_t used = 0;
    for (size_t i = 0; i + 4 <= code.len && used + 12 < sizeof(detail);
         i += 4) {
      uint32_t word = 0;
      memcpy(&word, code.data + i, sizeof(word));
      int n = snprintf(detail + used, sizeof(detail) - used, "%08x ", word);
      if (n < 0)
        break;
      used += (size_t)n;
    }
    nyr_set_error(
        err, err_len,
        "%s vector selftest: allocated vector load/add encoding missing (%s)",
        label, detail);
    nyr_obj_free(&code);
    nyr_mach_func_free(&mach);
    return false;
  }
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return true;
}

/*
 * A straight-line scalar chain must use colored GPRs rather than materialize
 * each arithmetic temporary's stack home. The two constant COPY homes and
 * final frame store are intentional.
 */
static bool x64_gpr_residency_selftest(char *err, size_t err_len) {
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, NY_NATIVE_BACKEND_X86_64,
                          "x86_64-linux-gnu")) {
    nyr_set_error(
        err, err_len,
        "x86-64 GPR residency selftest: target initialization failed");
    return false;
  }

  nyr_mach_func_t mach = {0};
  uint32_t block = 0, a_slot = 0, out_slot = 0;
  nyr_mach_reg_t a = 0, imm7 = 0, sum = 0, imm11 = 0, chain = 0;
  bool ok = nyr_mach_begin_block(&mach, 0, &block) &&
            nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_I64, false,
                                           &a_slot) &&
            nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_I64, false,
                                           &out_slot) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &a) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &imm7) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &sum) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &imm11) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &chain);
  if (ok)
    ok = nyr_mach_emit(
             &mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_LOAD,
                                     .dst = selftest_gpr_vreg(a),
                                     .src0 = selftest_frame(a_slot),
                                     .effects = NYR_MACH_EFFECT_READ_MEMORY }) &&
         nyr_mach_emit(
             &mach,
             (nyr_mach_inst_t){ .opcode = NYR_MACH_COPY,
             .dst = selftest_gpr_vreg(imm7),
             .src0 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM,
                                         .as.imm = 7} }) &&
         nyr_mach_emit(&mach,
                      (nyr_mach_inst_t){ .opcode = NYR_MACH_ADD,
                                       .dst = selftest_gpr_vreg(sum),
                                       .src0 = selftest_gpr_vreg(a),
                                       .src1 = selftest_gpr_vreg(imm7) }) &&
         nyr_mach_emit(
             &mach,
             (nyr_mach_inst_t){ .opcode = NYR_MACH_COPY,
             .dst = selftest_gpr_vreg(imm11),
             .src0 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM,
                                         .as.imm = 11} }) &&
         nyr_mach_emit(&mach,
                      (nyr_mach_inst_t){ .opcode = NYR_MACH_ADD,
                                       .dst = selftest_gpr_vreg(chain),
                                       .src0 = selftest_gpr_vreg(sum),
                                       .src1 = selftest_gpr_vreg(imm11) }) &&
         nyr_mach_emit(
             &mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_STORE,
                                     .dst = selftest_frame(out_slot),
                                     .src0 = selftest_gpr_vreg(chain),
                                     .effects = NYR_MACH_EFFECT_WRITE_MEMORY }) &&
         nyr_mach_emit(&mach,
                      (nyr_mach_inst_t){ .opcode = NYR_MACH_RET,
                                       .effects = NYR_MACH_EFFECT_CONTROL });
  if (ok && !nyr_mach_verify(&mach, 0, err, err_len))
    ok = false;

  nyr_native_regalloc_metrics_t gpr = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[8] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok &&
      (!nyr_native_x64_regalloc_metrics(&mach, &target, &gpr, NULL, NULL) ||
       !nyr_x64_mach_build_bundle(
           &mach, NULL, NULL, 0, &target, "gpr_residency_selftest", false,
           &code, defs, &def_count, relocs, &reloc_count, err, err_len)))
    ok = false;

  /*
   * The existing regalloc metric is the stable residency marker: every value
   * in this chain must be colored and none may require a spill home. Do not
   * couple this regression to prologue/epilogue byte counts.
   */
  if (ok && (gpr.colored < 5 || gpr.spilled != 0)) {
    nyr_set_error(err, err_len,
                      "x86-64 GPR residency selftest: colored=%zu spilled=%zu",
                      gpr.colored, gpr.spilled);
    ok = false;
  }
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

/*
 * Values crossing a CFG edge must be homed before the branch and reloaded in
 * the destination block. The extra movdqu proves the x64 allocator took that
 * path instead of retaining a volatile XMM value.
 */
static bool x64_vector_cfg_selftest(char *err, size_t err_len) {
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, NY_NATIVE_BACKEND_X86_64,
                          "x86_64-linux-gnu")) {
    nyr_set_error(
        err, err_len,
        "x86-64 vector CFG selftest: target initialization failed");
    return false;
  }

  nyr_mach_func_t mach = {0};
  bool ok = vector_sum_program(&mach, NYR_MACH_BR);
  if (ok && !nyr_mach_verify(&mach, 0, err, err_len))
    ok = false;

  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[8] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !nyr_x64_mach_build_bundle(
                &mach, NULL, NULL, 0, &target, "vector_cfg_selftest", false,
                &code, defs, &def_count, relocs, &reloc_count, err, err_len))
    ok = false;

  size_t vector_loads = 0;
  bool has_jump = false;
  for (size_t i = 0; ok && i < code.len; ++i) {
    if (code.data[i] == 0xe9)
      has_jump = true;
    if (i + 2 < code.len && code.data[i] == 0xf3 && code.data[i + 1] == 0x0f &&
        code.data[i + 2] == 0x6f)
      ++vector_loads;
  }
  if (ok && (!has_jump || vector_loads < 3)) {
    nyr_set_error(
        err, err_len,
        "x86-64 vector CFG selftest: branch homing/reload encoding missing");
    ok = false;
  }
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

/*
 * A scalar call may overwrite every caller-saved XMM register. This checks
 * that vector values live across the call are written to homes and reloaded
 * afterwards; vector argument/return ABI lowering is deliberately separate.
 */
static bool x64_vector_call_selftest(char *err, size_t err_len) {
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, NY_NATIVE_BACKEND_X86_64,
                          "x86_64-linux-gnu")) {
    nyr_set_error(
        err, err_len,
        "x86-64 vector call selftest: target initialization failed");
    return false;
  }

  nyr_mach_func_t mach = {0};
  bool ok = vector_sum_program(&mach, NYR_MACH_CALL);
  if (ok && !nyr_mach_verify(&mach, 0, err, err_len))
    ok = false;

  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[8] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !nyr_x64_mach_build_bundle(
                &mach, NULL, NULL, 0, &target, "vector_call_selftest", false,
                &code, defs, &def_count, relocs, &reloc_count, err, err_len))
    ok = false;

  size_t vector_loads = 0;
  bool has_call = false;
  for (size_t i = 0; ok && i < code.len; ++i) {
    if (code.data[i] == 0xe8)
      has_call = true;
    if (i + 2 < code.len && code.data[i] == 0xf3 && code.data[i + 1] == 0x0f &&
        code.data[i + 2] == 0x6f)
      ++vector_loads;
  }
  if (ok && (!has_call || reloc_count != 1 || vector_loads < 3)) {
    nyr_set_error(
        err, err_len,
        "x86-64 vector call selftest: call spill/reload encoding missing");
    ok = false;
  }
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool aarch64_vector_cfg_selftest(char *err, size_t err_len) {
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, NY_NATIVE_BACKEND_AARCH64,
                          "aarch64-linux-gnu")) {
    nyr_set_error(
        err, err_len,
        "AArch64 vector CFG selftest: target initialization failed");
    return false;
  }

  nyr_mach_func_t mach = {0};
  bool ok = vector_sum_program(&mach, NYR_MACH_BR);
  if (ok && !nyr_mach_verify(&mach, 0, err, err_len))
    ok = false;

  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[8] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !nyr_a64_mach_build_bundle(
                &mach, NULL, NULL, 0, &target, "a64_vector_cfg_selftest", false,
                &code, defs, &def_count, relocs, &reloc_count, err, err_len))
    ok = false;

  size_t vector_loads = 0;
  bool has_jump = false;
  for (size_t i = 0; ok && i + 4 <= code.len; i += 4) {
    uint32_t word = 0;
    memcpy(&word, code.data + i, sizeof(word));
    if ((word & 0x7c000000u) == 0x14000000u)
      has_jump = true;
    if ((word & 0xffe00000u) == 0x3cc00000u)
      ++vector_loads;
  }
  if (ok && (!has_jump || vector_loads < 3)) {
    nyr_set_error(
        err, err_len,
        "AArch64 vector CFG selftest: branch homing/reload encoding missing");
    ok = false;
  }
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

/*
 * A scalar call may overwrite every caller-saved Q register. This checks
 * that vector values live across the call are written to homes and reloaded
 * afterwards; vector argument/return ABI lowering is deliberately separate.
 */
static bool aarch64_vector_call_selftest(char *err, size_t err_len) {
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, NY_NATIVE_BACKEND_AARCH64,
                          "aarch64-linux-gnu")) {
    nyr_set_error(
        err, err_len,
        "AArch64 vector call selftest: target initialization failed");
    return false;
  }

  nyr_mach_func_t mach = {0};
  bool ok = vector_sum_program(&mach, NYR_MACH_CALL);
  if (ok && !nyr_mach_verify(&mach, 0, err, err_len))
    ok = false;

  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[8] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok &&
      !nyr_a64_mach_build_bundle(&mach, NULL, NULL, 0, &target,
                                "a64_vector_call_selftest", false, &code, defs,
                                &def_count, relocs, &reloc_count, err, err_len))
    ok = false;

  size_t vector_loads = 0;
  bool has_call = false;
  for (size_t i = 0; ok && i + 4 <= code.len; i += 4) {
    uint32_t word = 0;
    memcpy(&word, code.data + i, sizeof(word));
    if ((word & 0xfc000000u) == 0x94000000u)
      has_call = true;
    if ((word & 0xffe00000u) == 0x3cc00000u)
      ++vector_loads;
  }
  if (ok && (!has_call || reloc_count != 1 || vector_loads < 2)) {
    nyr_set_error(
        err, err_len,
        "AArch64 vector call selftest: call spill/reload encoding missing");
    ok = false;
  }
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool aarch64_fpr_selftest(char *err, size_t err_len) {
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, NY_NATIVE_BACKEND_AARCH64,
                          "aarch64-linux-gnu")) {
    nyr_set_error(err, err_len,
                      "AArch64 FPR selftest: target initialization failed");
    return false;
  }
  nyr_mach_func_t mach = {0};
  uint32_t a_slot = 0, b_slot = 0, out_slot = 0, block = 0;
  nyr_mach_reg_t a = 0, b = 0, sum = 0;
  bool ok = nyr_mach_begin_block(&mach, 0, &block) &&
            nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_F64, false,
                                           &a_slot) &&
            nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_F64, false,
                                           &b_slot) &&
            nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_F64, false,
                                           &out_slot) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_F64, &a) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_F64, &b) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_F64, &sum);
  if (ok)
    ok = nyr_mach_emit(
             &mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_LOAD,
                                     .dst = selftest_fpr_vreg(a),
                                     .src0 = selftest_frame(a_slot),
                                     .effects = NYR_MACH_EFFECT_READ_MEMORY }) &&
         nyr_mach_emit(
             &mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_LOAD,
                                     .dst = selftest_fpr_vreg(b),
                                     .src0 = selftest_frame(b_slot),
                                     .effects = NYR_MACH_EFFECT_READ_MEMORY }) &&
         nyr_mach_emit(&mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_ADD,
                                              .dst = selftest_fpr_vreg(sum),
                                              .src0 = selftest_fpr_vreg(a),
                                              .src1 = selftest_fpr_vreg(b) }) &&
         nyr_mach_emit(
             &mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_STORE,
                                     .dst = selftest_frame(out_slot),
                                     .src0 = selftest_fpr_vreg(sum),
                                     .effects = NYR_MACH_EFFECT_WRITE_MEMORY }) &&
         nyr_mach_emit(&mach,
                      (nyr_mach_inst_t){ .opcode = NYR_MACH_RET,
                                       .effects = NYR_MACH_EFFECT_CONTROL });
  if (ok && !nyr_mach_verify(&mach, 0, err, err_len))
    ok = false;
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[8] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !nyr_a64_mach_build_bundle(
                &mach, NULL, NULL, 0, &target, "fpr_selftest", false, &code,
                defs, &def_count, relocs, &reloc_count, err, err_len))
    ok = false;
  bool has_load = false, has_add = false;
  for (size_t i = 0; ok && i + 4 <= code.len; i += 4) {
    uint32_t word = 0;
    memcpy(&word, code.data + i, sizeof(word));
    if ((word & 0xffc00000u) == 0xfc400000u)
      has_load = true;
    if ((word & 0xffe0fc00u) == 0x1e602800u && (word & 31u) != 0)
      has_add = true;
  }
  if (ok && (!has_load || !has_add)) {
    nyr_set_error(
        err, err_len,
        "AArch64 FPR selftest: allocated scalar FP load/add encoding missing");
    ok = false;
  }
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

/*
 * Stack-home coloring must not collapse overlapping values or address-taken
 * locals.  The machine allocator currently exposes only register intervals;
 * keep this driver at that seam so a future home-coloring consumer cannot
 * silently weaken either invariant.
 */
static bool x64_stack_home_safety_selftest(char *err, size_t err_len) {
  nyr_mach_func_t mach = {0};
  uint32_t block = 0, first_slot = 0, second_slot = 0, escaped_slot = 0;
  nyr_mach_reg_t first = 0, second = 0, sum = 0, address = 0;
  bool ok = nyr_mach_begin_block(&mach, 0, &block) &&
            nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_I64, false,
                                           &first_slot) &&
            nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_I64, false,
                                           &second_slot) &&
            nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_I64, true,
                                           &escaped_slot) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &first) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &second) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &sum) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_PTR, &address);
  if (ok)
    ok = nyr_mach_emit(
             &mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_LOAD,
                                     .dst = selftest_gpr_vreg(first),
                                     .src0 = selftest_frame(first_slot),
                                     .effects = NYR_MACH_EFFECT_READ_MEMORY }) &&
         nyr_mach_emit(
             &mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_LOAD,
                                     .dst = selftest_gpr_vreg(second),
                                     .src0 = selftest_frame(second_slot),
                                     .effects = NYR_MACH_EFFECT_READ_MEMORY }) &&
         nyr_mach_emit(&mach,
                      (nyr_mach_inst_t){ .opcode = NYR_MACH_ADD,
                                       .dst = selftest_gpr_vreg(sum),
                                       .src0 = selftest_gpr_vreg(first),
                                       .src1 = selftest_gpr_vreg(second) }) &&
         nyr_mach_emit(
             &mach, (nyr_mach_inst_t){ .opcode = NYR_MACH_STORE,
                                     .dst = selftest_frame(escaped_slot),
                                     .src0 = selftest_gpr_vreg(sum),
                                     .effects = NYR_MACH_EFFECT_WRITE_MEMORY }) &&
         nyr_mach_emit(&mach,
                      (nyr_mach_inst_t){ .opcode = NYR_MACH_LEA,
                                       .dst = selftest_gpr_vreg(address),
                                       .src0 = selftest_frame(escaped_slot) }) &&
         nyr_mach_emit(&mach,
                      (nyr_mach_inst_t){ .opcode = NYR_MACH_RET,
                                       .effects = NYR_MACH_EFFECT_CONTROL });
  char verify_err[128] = {0};
  if (ok && !nyr_mach_verify(&mach, 0, verify_err, sizeof(verify_err))) {
    nyr_set_error(
        err, err_len,
        "x86-64 stack-home safety selftest: invalid machine IR: %s",
        verify_err);
    ok = false;
  }
  nyr_mach_regalloc_t alloc = {0};
  const nyr_mach_live_segment_t *first_seg = NULL;
  const nyr_mach_live_segment_t *second_seg = NULL;
  if (ok && !nyr_mach_regalloc_build(&mach, 8, &alloc)) {
    nyr_set_error(
        err, err_len,
        "x86-64 stack-home safety selftest: interval build failed");
    ok = false;
  }
  if (ok) {
    first_seg = nyr_mach_regalloc_segment_at(&alloc, first, 2);
    second_seg = nyr_mach_regalloc_segment_at(&alloc, second, 2);
    if (!first_seg || !second_seg || first_seg->color < 0 ||
        second_seg->color < 0 || first_seg->color == second_seg->color) {
      nyr_set_error(err, err_len,
                        "x86-64 stack-home safety selftest: overlapping "
                        "intervals were co-colored");
      ok = false;
    }
  }
  if (ok && (!mach.frame_slots || escaped_slot >= mach.frame_slot_len ||
             !mach.frame_slots[escaped_slot].address_taken ||
             mach.frame_slots[first_slot].address_taken ||
             mach.frame_slots[second_slot].address_taken)) {
    nyr_set_error(err, err_len,
                      "x86-64 stack-home safety selftest: address-taken slot "
                      "metadata changed");
    ok = false;
  }
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

/* MIR shifts operate on explicit machine bits on both language and C routes.
 * A language function must shift in its allocated destination rather than
 * shuttle the result through RAX. Use a frame input to prevent folding. */
static bool x64_destination_shift_selftest(char *err, size_t err_len) {
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, NY_NATIVE_BACKEND_X86_64,
                          "x86_64-linux-gnu"))
    return false;
  nyr_mach_func_t mach = {0};
  uint32_t block = 0, slot = 0;
  nyr_mach_reg_t input = 0, shifted = 0;
  bool ok = nyr_mach_begin_block(&mach, 0, &block) &&
            nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_I64,
                                           false, &slot) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &input) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &shifted) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                .opcode = NYR_MACH_LOAD, .dst = selftest_gpr_vreg(input),
                .src0 = selftest_frame(slot),
                .effects = NYR_MACH_EFFECT_READ_MEMORY}) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                .opcode = NYR_MACH_SHL, .dst = selftest_gpr_vreg(shifted),
                .src0 = selftest_gpr_vreg(input),
                .src1 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 3}}) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                .opcode = NYR_MACH_RET, .src0 = selftest_gpr_vreg(shifted),
                .effects = NYR_MACH_EFFECT_CONTROL});
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[8] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok)
    ok = nyr_mach_verify(&mach, 0, err, err_len) &&
         nyr_x64_mach_build_bundle(
             &mach, NULL, NULL, 0, &target, "destination_shift_selftest", false,
             &code, defs, &def_count, relocs, &reloc_count, err, err_len);
  bool direct_shift = false;
  for (size_t i = 0; ok && i + 3 < code.len; ++i)
    if ((code.data[i] & 0xfe) == 0x48 && code.data[i + 1] == 0xc1 &&
        (code.data[i + 2] & 0xf8) == 0xe0 && code.data[i + 3] == 3 &&
        ((code.data[i] & 1) || (code.data[i + 2] & 7)))
      direct_shift = true;
  if (ok && !direct_shift) {
    nyr_set_error(err, err_len,
                  "x86-64 destination shift selftest: shift staged through RAX");
    ok = false;
  }
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

/* More live inputs than GPR colors force canonical RHS homes. The running
 * accumulator should still use ADD rdst,[home], without RAX staging. */
static bool x64_alu_home_selftest(char *err, size_t err_len) {
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, NY_NATIVE_BACKEND_X86_64,
                          "x86_64-linux-gnu"))
    return false;
  nyr_mach_func_t mach = {0};
  uint32_t block = 0;
  nyr_mach_reg_t inputs[16], acc = 0;
  bool ok = nyr_mach_begin_block(&mach, 0, &block);
  for (size_t i = 0; ok && i < 16; ++i) {
    uint32_t slot = 0;
    ok = nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_I64,
                                        false, &slot) &&
         nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &inputs[i]) &&
         nyr_mach_emit(&mach, (nyr_mach_inst_t){
             .opcode = NYR_MACH_LOAD, .dst = selftest_gpr_vreg(inputs[i]),
             .src0 = selftest_frame(slot),
             .effects = NYR_MACH_EFFECT_READ_MEMORY});
  }
  if (ok)
    acc = inputs[0];
  for (size_t i = 1; ok && i < 16; ++i) {
    nyr_mach_reg_t sum = 0;
    ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &sum) &&
         nyr_mach_emit(&mach, (nyr_mach_inst_t){
             .opcode = NYR_MACH_ADD, .dst = selftest_gpr_vreg(sum),
             .src0 = selftest_gpr_vreg(acc),
             .src1 = selftest_gpr_vreg(inputs[i])});
    acc = sum;
  }
  if (ok)
    ok = nyr_mach_emit(&mach, (nyr_mach_inst_t){
        .opcode = NYR_MACH_RET, .src0 = selftest_gpr_vreg(acc),
        .effects = NYR_MACH_EFFECT_CONTROL});
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[8] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok)
    ok = nyr_mach_verify(&mach, 0, err, err_len) &&
         nyr_x64_mach_build_bundle(
             &mach, NULL, NULL, 0, &target, "alu_home_selftest", false,
             &code, defs, &def_count, relocs, &reloc_count, err, err_len);
  bool direct_add = false;
  for (size_t i = 0; ok && i + 2 < code.len; ++i)
    if ((code.data[i] & 0xfb) == 0x48 && code.data[i + 1] == 0x03 &&
        ((code.data[i + 2] & 0xc7) == 0x45 ||
         (code.data[i + 2] & 0xc7) == 0x85) &&
        ((code.data[i] & 4) || (code.data[i + 2] & 0x38)))
      direct_add = true;
  if (ok && !direct_add) {
    nyr_set_error(err, err_len,
                  "x86-64 ALU home selftest: spilled RHS staged through RAX");
    ok = false;
  }
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

/* Keep both inputs live past a subtraction so the allocated result must
 * differ from both. VEX must name the LHS independently of the destination. */
static bool x64_avx_scalar_selftest(char *err, size_t err_len) {
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, NY_NATIVE_BACKEND_X86_64,
                          "x86_64-linux-gnu"))
    return false;
  target.caps |= NY_NATIVE_CAP_MACH_AVX;
  for (unsigned f32 = 0; f32 < 2; ++f32) {
    nyr_mach_type_t type = f32 ? NYR_MACH_TYPE_F32 : NYR_MACH_TYPE_F64;
    nyr_mach_func_t mach = {0};
    uint32_t block = 0, slots[2];
    nyr_mach_reg_t values[5];
    bool ok = nyr_mach_begin_block(&mach, 0, &block);
    for (size_t i = 0; ok && i < 5; ++i)
      ok = nyr_mach_alloc_typed_vreg(&mach, type, &values[i]);
    for (size_t i = 0; ok && i < 2; ++i)
      ok = nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, type, false, &slots[i]) &&
           nyr_mach_emit(&mach, (nyr_mach_inst_t){
               .opcode = NYR_MACH_LOAD, .dst = selftest_fpr_vreg(values[i]),
               .src0 = selftest_frame(slots[i]),
               .effects = NYR_MACH_EFFECT_READ_MEMORY});
    for (size_t i = 2; ok && i < 5; ++i)
      ok = nyr_mach_emit(&mach, (nyr_mach_inst_t){
          .opcode = i == 2 ? NYR_MACH_SUB : i == 3 ? NYR_MACH_MUL : NYR_MACH_ADD,
          .dst = selftest_fpr_vreg(values[i]),
          .src0 = selftest_fpr_vreg(values[i == 4 ? 2 : 0]),
          .src1 = selftest_fpr_vreg(values[i == 4 ? 3 : 1])});
    if (ok)
      ok = nyr_mach_emit(&mach, (nyr_mach_inst_t){
          .opcode = NYR_MACH_RET, .src0 = selftest_fpr_vreg(values[4]),
          .effects = NYR_MACH_EFFECT_CONTROL});
    nyr_obj_buf_t code = {0};
    nyr_x64_obj_symbol_def_t defs[8] = {0};
    nyr_x64_obj_reloc_t relocs[8] = {0};
    size_t def_count = 0, reloc_count = 0;
    if (ok)
      ok = nyr_mach_verify(&mach, 0, err, err_len) &&
           nyr_x64_mach_build_bundle(
               &mach, NULL, NULL, 0, &target, "avx_scalar_selftest", false,
               &code, defs, &def_count, relocs, &reloc_count, err, err_len);
    bool independent_lhs = false;
    for (size_t i = 0; ok && i + 3 < code.len; ++i) {
      bool vex2 = code.data[i] == 0xc5;
      bool vex3 = code.data[i] == 0xc4 && i + 4 < code.len;
      if (!vex2 && !vex3)
        continue;
      size_t op = i + (vex2 ? 2 : 3);
      if (code.data[op] != 0x5c || (code.data[op + 1] & 0xc0) != 0xc0)
        continue;
      unsigned lhs = ((~code.data[op - 1]) >> 3) & 15;
      unsigned dst = ((code.data[op + 1] >> 3) & 7) |
                     ((code.data[i + 1] & 0x80) ? 0 : 8);
      independent_lhs |= lhs != dst;
    }
    if (ok && !independent_lhs) {
      nyr_set_error(err, err_len,
                    "x86-64 AVX scalar selftest: destructive f%u subtraction",
                    f32 ? 32 : 64);
      ok = false;
    }
    nyr_obj_free(&code);
    nyr_mach_func_free(&mach);
    if (!ok)
      return false;
  }
  return true;
}

static bool x64_branch_trampoline_selftest(char *err, size_t err_len) {
  ny_native_target_info_t target = {0};
  if (!vector_target_init(&target, NY_NATIVE_BACKEND_X86_64,
                          "x86_64-linux-gnu"))
    return false;
  for (unsigned shared = 0; shared < 2; ++shared) {
    nyr_mach_func_t mach = {0};
    uint32_t block = 0, slot = 0;
    nyr_mach_reg_t cond = 0;
    bool ok = nyr_mach_begin_block(&mach, 0, &block) &&
              nyr_mach_alloc_typed_frame_slot(&mach, 8, 8, NYR_MACH_TYPE_I64,
                                             false, &slot) &&
              nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &cond) &&
              nyr_mach_emit(&mach, (nyr_mach_inst_t){
                  .opcode = NYR_MACH_LOAD, .dst = selftest_gpr_vreg(cond),
                  .src0 = selftest_frame(slot),
                  .effects = NYR_MACH_EFFECT_READ_MEMORY}) &&
              nyr_mach_emit(&mach, (nyr_mach_inst_t){
                  .opcode = NYR_MACH_BR_IF, .src0 = selftest_gpr_vreg(cond),
                  .src1 = selftest_block(2),
                  .effects = NYR_MACH_EFFECT_CONTROL}) &&
              nyr_mach_begin_block(&mach, 1, &block) &&
              nyr_mach_emit(&mach, (nyr_mach_inst_t){
                  .opcode = NYR_MACH_BR, .src1 = selftest_block(3),
                  .effects = NYR_MACH_EFFECT_CONTROL}) &&
              nyr_mach_begin_block(&mach, 2, &block) &&
              nyr_mach_emit(&mach, shared
                  ? (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                      .src1 = selftest_block(1),
                      .effects = NYR_MACH_EFFECT_CONTROL}
                  : (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
              nyr_mach_begin_block(&mach, 3, &block) &&
              nyr_mach_emit(&mach, (nyr_mach_inst_t){
                  .opcode = NYR_MACH_RET,
                  .effects = NYR_MACH_EFFECT_CONTROL});
    nyr_obj_buf_t code = {0};
    nyr_x64_obj_symbol_def_t defs[8] = {0};
    nyr_x64_obj_reloc_t relocs[8] = {0};
    size_t def_count = 0, reloc_count = 0;
    if (ok)
      ok = nyr_mach_verify(&mach, 0, err, err_len) &&
           nyr_x64_mach_build_bundle(
               &mach, NULL, NULL, 0, &target, "branch_trampoline_selftest", false,
               &code, defs, &def_count, relocs, &reloc_count, err, err_len);
    bool has_branch = false;
    for (size_t i = 0; ok && i + 1 < code.len; ++i)
      if (code.data[i] == 0x0f && code.data[i + 1] == (shared ? 0x85 : 0x84))
        has_branch = true;
    if (ok && !has_branch) {
      nyr_set_error(err, err_len,
                    "x86-64 branch trampoline selftest: shared=%u", shared);
      ok = false;
    }
    nyr_obj_free(&code);
    nyr_mach_func_free(&mach);
    if (!ok)
      return false;
  }
  return true;
}

bool nyr_vector_selftest(char *err, size_t err_len) {
  if (!x64_avx_scalar_selftest(err, err_len))
    return false;
  if (!x64_alu_home_selftest(err, err_len))
    return false;
  if (!x64_branch_trampoline_selftest(err, err_len))
    return false;
  if (!x64_destination_shift_selftest(err, err_len))
    return false;
  if (!vector_backend_selftest(NY_NATIVE_BACKEND_X86_64,
                               nyr_x64_mach_build_bundle, "x86-64", err,
                               err_len))
    return false;
  if (!x64_gpr_residency_selftest(err, err_len))
    return false;
  if (!vector_backend_selftest(NY_NATIVE_BACKEND_AARCH64,
                               nyr_a64_mach_build_bundle, "AArch64", err,
                               err_len))
    return false;
  if (!x64_vector_cfg_selftest(err, err_len))
    return false;
  if (!x64_vector_call_selftest(err, err_len))
    return false;
  if (!aarch64_vector_cfg_selftest(err, err_len))
    return false;
  if (!aarch64_vector_call_selftest(err, err_len))
    return false;
  if (!aarch64_fpr_selftest(err, err_len))
    return false;
  if (!x64_stack_home_safety_selftest(err, err_len))
    return false;
  if (!nyr_mach_audit_size_candidates_selftest()) {
    nyr_set_error(err, err_len, "machine outliner audit selftest failed");
    return false;
  }
  if (!nyr_mach_schedule_post_ra_selftest()) {
    nyr_set_error(err, err_len, "post-RA scheduler selftest failed");
    return false;
  }
  return true;
}
