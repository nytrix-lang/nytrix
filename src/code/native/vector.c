#include "code/native/internal.h"
#include "code/native/object/internal.h"

#include <stdio.h>
#include <string.h>

static ny_mach_operand_t selftest_vreg(ny_mach_reg_t reg) {
  ny_mach_operand_t op = {0};
  op.kind = NY_MACH_OPERAND_VREG;
  op.reg_class = NY_MACH_REGCLASS_VECTOR;
  op.as.reg = reg;
  return op;
}

static ny_mach_operand_t selftest_fpr_vreg(ny_mach_reg_t reg) {
  ny_mach_operand_t op = {0};
  op.kind = NY_MACH_OPERAND_VREG;
  op.reg_class = NY_MACH_REGCLASS_FPR;
  op.as.reg = reg;
  return op;
}

static ny_mach_operand_t selftest_frame(uint32_t slot) {
  ny_mach_operand_t op = {0};
  op.kind = NY_MACH_OPERAND_FRAME;
  op.as.frame_index = slot;
  return op;
}

static ny_mach_operand_t selftest_block(uint32_t block) {
  ny_mach_operand_t op = {0};
  op.kind = NY_MACH_OPERAND_BLOCK;
  op.as.block_index = block;
  return op;
}

static ny_mach_operand_t selftest_symbol(const char *symbol) {
  ny_mach_operand_t op = {0};
  op.kind = NY_MACH_OPERAND_SYMBOL;
  op.as.symbol = symbol;
  return op;
}

typedef bool (*vector_bundle_builder_t)(
    const ny_mach_func_t *, const ny_mach_func_t *, const char *const *,
    size_t, const ny_native_target_info_t *, const char *, bool, ny_obj_buf_t *,
    ny_x64_obj_symbol_def_t *, size_t *, ny_x64_obj_reloc_t *, size_t *, char *,
    size_t);

static bool vector_backend_selftest(ny_native_backend_t backend,
                                    vector_bundle_builder_t build,
                                    const char *label, char *err,
                                    size_t err_len) {
  ny_options options = {0};
  options.native_backend = backend;
  options.native_abi = NY_NATIVE_ABI_SYSV;
  options.host_triple = backend == NY_NATIVE_BACKEND_AARCH64
                            ? "aarch64-linux-gnu"
                            : "x86_64-linux-gnu";
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, &options)) {
    ny_native_set_err(err, err_len, "%s vector selftest: target initialization failed",
                      label);
    return false;
  }

  ny_mach_func_t mach = {0};
  uint32_t block = 0, a_slot = 0, b_slot = 0, out_slot = 0;
  ny_mach_reg_t a = 0, b = 0, sum = 0;
  bool ok = ny_mach_begin_block(&mach, 0, &block) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &a_slot) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &b_slot) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &out_slot) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &a) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &b) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &sum);
  if (ok)
    ok = ny_mach_emit(&mach, (ny_mach_inst_t){
        .opcode = NY_MACH_LOAD,
        .dst = selftest_vreg(a), .src0 = selftest_frame(a_slot),
        .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
        .opcode = NY_MACH_LOAD,
        .dst = selftest_vreg(b), .src0 = selftest_frame(b_slot),
        .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
        .opcode = NY_MACH_ADD,
        .dst = selftest_vreg(sum), .src0 = selftest_vreg(a),
        .src1 = selftest_vreg(b)}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
        .opcode = NY_MACH_STORE,
        .dst = selftest_frame(out_slot), .src0 = selftest_vreg(sum),
        .effects = NY_MACH_EFFECT_WRITE_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
        .opcode = NY_MACH_RET, .effects = NY_MACH_EFFECT_CONTROL});
  if (ok && !ny_mach_verify(&mach, err, err_len))
    ok = false;

  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[16] = {0};
  ny_x64_obj_reloc_t relocs[16] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !build(&mach, NULL, NULL, 0, &target, "vector_selftest", false,
                   &code, defs, &def_count, relocs, &reloc_count, err,
                   err_len))
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
      /* add v2.2d,v0.2d,v1.2d: the non-zero destination proves this was
       * the allocated Q-register path rather than the q0 scratch fallback. */
      if ((word & 0xffe0fc00u) == 0x4e60d400u && (word & 31u) != 0)
        has_add = true;
    }
  }
  if (!ok)
  {
    ny_obj_free(&code);
    ny_mach_func_free(&mach);
    return false;
  }
  if (!has_load || !has_add) {
    char detail[128] = {0};
    size_t used = 0;
    for (size_t i = 0; i + 4 <= code.len && used + 12 < sizeof(detail); i += 4) {
      uint32_t word = 0;
      memcpy(&word, code.data + i, sizeof(word));
      int n = snprintf(detail + used, sizeof(detail) - used, "%08x ", word);
      if (n < 0) break;
      used += (size_t)n;
    }
    ny_native_set_err(err, err_len,
                      "%s vector selftest: allocated vector load/add encoding missing (%s)",
                      label, detail);
    ny_obj_free(&code);
    ny_mach_func_free(&mach);
    return false;
  }
  ny_obj_free(&code);
  ny_mach_func_free(&mach);
  return true;
}

/* Values crossing a CFG edge must be homed before the branch and reloaded in
 * the destination block. The extra movdqu proves the x64 allocator took that
 * path instead of retaining a volatile XMM value. */
static bool x64_vector_cfg_selftest(char *err, size_t err_len) {
  ny_options options = {0};
  options.native_backend = NY_NATIVE_BACKEND_X86_64;
  options.native_abi = NY_NATIVE_ABI_SYSV;
  options.host_triple = "x86_64-linux-gnu";
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, &options)) {
    ny_native_set_err(err, err_len,
                      "x86-64 vector CFG selftest: target initialization failed");
    return false;
  }

  ny_mach_func_t mach = {0};
  uint32_t first = 0, second = 0, a_slot = 0, b_slot = 0, out_slot = 0;
  ny_mach_reg_t a = 0, b = 0, sum = 0;
  bool ok = ny_mach_begin_block(&mach, 0, &first) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &a_slot) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &b_slot) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &out_slot) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &a) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &b) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &sum);
  if (ok)
    ok = ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_LOAD, .dst = selftest_vreg(a),
             .src0 = selftest_frame(a_slot),
             .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_LOAD, .dst = selftest_vreg(b),
             .src0 = selftest_frame(b_slot),
             .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_ADD, .dst = selftest_vreg(sum),
             .src0 = selftest_vreg(a), .src1 = selftest_vreg(b)}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_BR, .src1 = selftest_block(1),
             .effects = NY_MACH_EFFECT_CONTROL}) &&
         ny_mach_begin_block(&mach, 1, &second) && second == 1 &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_STORE, .dst = selftest_frame(out_slot),
             .src0 = selftest_vreg(sum),
             .effects = NY_MACH_EFFECT_WRITE_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_RET, .effects = NY_MACH_EFFECT_CONTROL});
  if (ok && !ny_mach_verify(&mach, err, err_len))
    ok = false;

  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[8] = {0};
  ny_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !ny_x64_mach_build_bundle(&mach, NULL, NULL, 0, &target,
                                      "vector_cfg_selftest", false, &code,
                                      defs, &def_count, relocs, &reloc_count,
                                      err, err_len))
    ok = false;

  size_t vector_loads = 0;
  bool has_jump = false;
  for (size_t i = 0; ok && i < code.len; ++i) {
    if (code.data[i] == 0xe9)
      has_jump = true;
    if (i + 2 < code.len && code.data[i] == 0xf3 &&
        code.data[i + 1] == 0x0f && code.data[i + 2] == 0x6f)
      ++vector_loads;
  }
  if (ok && (!has_jump || vector_loads < 3)) {
    ny_native_set_err(err, err_len,
                      "x86-64 vector CFG selftest: branch homing/reload encoding missing");
    ok = false;
  }
  ny_obj_free(&code);
  ny_mach_func_free(&mach);
  return ok;
}

/* A scalar call may overwrite every caller-saved XMM register. This checks
 * that vector values live across the call are written to homes and reloaded
 * afterwards; vector argument/return ABI lowering is deliberately separate. */
static bool x64_vector_call_selftest(char *err, size_t err_len) {
  ny_options options = {0};
  options.native_backend = NY_NATIVE_BACKEND_X86_64;
  options.native_abi = NY_NATIVE_ABI_SYSV;
  options.host_triple = "x86_64-linux-gnu";
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, &options)) {
    ny_native_set_err(err, err_len,
                      "x86-64 vector call selftest: target initialization failed");
    return false;
  }

  ny_mach_func_t mach = {0};
  uint32_t block = 0, a_slot = 0, b_slot = 0, out_slot = 0;
  ny_mach_reg_t a = 0, b = 0, sum = 0;
  bool ok = ny_mach_begin_block(&mach, 0, &block) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &a_slot) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &b_slot) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &out_slot) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &a) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &b) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &sum);
  if (ok)
    ok = ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_LOAD, .dst = selftest_vreg(a),
             .src0 = selftest_frame(a_slot),
             .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_LOAD, .dst = selftest_vreg(b),
             .src0 = selftest_frame(b_slot),
             .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_ADD, .dst = selftest_vreg(sum),
             .src0 = selftest_vreg(a), .src1 = selftest_vreg(b)}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_CALL, .src0 = selftest_symbol("rt_test_noop"),
             .call_is_extern = true, .effects = NY_MACH_EFFECT_CALL}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_STORE, .dst = selftest_frame(out_slot),
             .src0 = selftest_vreg(sum),
             .effects = NY_MACH_EFFECT_WRITE_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_RET, .effects = NY_MACH_EFFECT_CONTROL});
  if (ok && !ny_mach_verify(&mach, err, err_len))
    ok = false;

  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[8] = {0};
  ny_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !ny_x64_mach_build_bundle(&mach, NULL, NULL, 0, &target,
                                      "vector_call_selftest", false, &code,
                                      defs, &def_count, relocs, &reloc_count,
                                      err, err_len))
    ok = false;

  size_t vector_loads = 0;
  bool has_call = false;
  for (size_t i = 0; ok && i < code.len; ++i) {
    if (code.data[i] == 0xe8)
      has_call = true;
    if (i + 2 < code.len && code.data[i] == 0xf3 &&
        code.data[i + 1] == 0x0f && code.data[i + 2] == 0x6f)
      ++vector_loads;
  }
  if (ok && (!has_call || reloc_count != 1 || vector_loads < 3)) {
    ny_native_set_err(err, err_len,
                      "x86-64 vector call selftest: call spill/reload encoding missing");
    ok = false;
  }
  ny_obj_free(&code);
  ny_mach_func_free(&mach);
  return ok;
}

static bool aarch64_vector_cfg_selftest(char *err, size_t err_len) {
  ny_options options = {0};
  options.native_backend = NY_NATIVE_BACKEND_AARCH64;
  options.native_abi = NY_NATIVE_ABI_SYSV;
  options.host_triple = "aarch64-linux-gnu";
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, &options)) {
    ny_native_set_err(err, err_len,
                      "AArch64 vector CFG selftest: target initialization failed");
    return false;
  }

  ny_mach_func_t mach = {0};
  uint32_t first = 0, second = 0, a_slot = 0, b_slot = 0, out_slot = 0;
  ny_mach_reg_t a = 0, b = 0, sum = 0;
  bool ok = ny_mach_begin_block(&mach, 0, &first) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &a_slot) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &b_slot) &&
            ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                           NY_MACH_TYPE_V128_F64, false,
                                           &out_slot) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &a) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &b) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &sum);
  if (ok)
    ok = ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_LOAD, .dst = selftest_vreg(a),
             .src0 = selftest_frame(a_slot),
             .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_LOAD, .dst = selftest_vreg(b),
             .src0 = selftest_frame(b_slot),
             .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_ADD, .dst = selftest_vreg(sum),
             .src0 = selftest_vreg(a), .src1 = selftest_vreg(b)}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_BR, .src1 = selftest_block(1),
             .effects = NY_MACH_EFFECT_CONTROL}) &&
         ny_mach_begin_block(&mach, 1, &second) && second == 1 &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_STORE, .dst = selftest_frame(out_slot),
             .src0 = selftest_vreg(sum),
             .effects = NY_MACH_EFFECT_WRITE_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_RET, .effects = NY_MACH_EFFECT_CONTROL});
  if (ok && !ny_mach_verify(&mach, err, err_len))
    ok = false;

  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[8] = {0};
  ny_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !ny_a64_mach_build_bundle(&mach, NULL, NULL, 0, &target,
                                      "a64_vector_cfg_selftest", false,
                                      &code, defs, &def_count, relocs,
                                      &reloc_count, err, err_len))
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
    ny_native_set_err(err, err_len,
                      "AArch64 vector CFG selftest: branch homing/reload encoding missing");
    ok = false;
  }
  ny_obj_free(&code);
  ny_mach_func_free(&mach);
  return ok;
}

/* A scalar call may overwrite every caller-saved Q register. This checks
 * that vector values live across the call are written to homes and reloaded
 * afterwards; vector argument/return ABI lowering is deliberately separate. */
static bool aarch64_vector_call_selftest(char *err, size_t err_len) {
  ny_options options = {0};
  options.native_backend = NY_NATIVE_BACKEND_AARCH64;
  options.native_abi = NY_NATIVE_ABI_SYSV;
  options.host_triple = "aarch64-linux-gnu";
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, &options)) {
    ny_native_set_err(err, err_len,
                      "AArch64 vector call selftest: target initialization failed");
    return false;
  }

  ny_mach_func_t mach = {0};
  uint32_t block = 0, a_slot = 0, b_slot = 0, out_slot = 0;
  ny_mach_reg_t a = 0, b = 0, sum = 0;
  bool ok = ny_mach_begin_block(&mach, 0, &block) &&
          ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                         NY_MACH_TYPE_V128_F64, false,
                                         &a_slot) &&
          ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                         NY_MACH_TYPE_V128_F64, false,
                                         &b_slot) &&
          ny_mach_alloc_typed_frame_slot(&mach, 16, 16,
                                         NY_MACH_TYPE_V128_F64, false,
                                         &out_slot) &&
          ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &a) &&
          ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &b) &&
          ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_V128_F64, &sum);
  if (ok)
    ok = ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_LOAD, .dst = selftest_vreg(a),
             .src0 = selftest_frame(a_slot),
             .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
        ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_LOAD, .dst = selftest_vreg(b),
             .src0 = selftest_frame(b_slot),
             .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
        ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_ADD, .dst = selftest_vreg(sum),
             .src0 = selftest_vreg(a), .src1 = selftest_vreg(b)}) &&
        ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_CALL, .src0 = selftest_symbol("rt_test_noop"),
             .call_is_extern = true, .effects = NY_MACH_EFFECT_CALL}) &&
        ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_STORE, .dst = selftest_frame(out_slot),
             .src0 = selftest_vreg(sum),
             .effects = NY_MACH_EFFECT_WRITE_MEMORY}) &&
        ny_mach_emit(&mach, (ny_mach_inst_t){
             .opcode = NY_MACH_RET, .effects = NY_MACH_EFFECT_CONTROL});
  if (ok && !ny_mach_verify(&mach, err, err_len))
    ok = false;

  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[8] = {0};
  ny_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !ny_a64_mach_build_bundle(&mach, NULL, NULL, 0, &target,
                                      "a64_vector_call_selftest", false,
                                      &code, defs, &def_count, relocs,
                                      &reloc_count, err, err_len))
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
  if (ok && (!has_call || reloc_count != 1 || vector_loads < 3)) {
    ny_native_set_err(err, err_len,
                      "AArch64 vector call selftest: call spill/reload encoding missing");
    ok = false;
  }
  ny_obj_free(&code);
  ny_mach_func_free(&mach);
  return ok;
}

static bool aarch64_fpr_selftest(char *err, size_t err_len) {
  ny_options options = {0};
  options.native_backend = NY_NATIVE_BACKEND_AARCH64;
  options.native_abi = NY_NATIVE_ABI_SYSV;
  options.host_triple = "aarch64-linux-gnu";
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, &options)) {
    ny_native_set_err(err, err_len, "AArch64 FPR selftest: target initialization failed");
    return false;
  }
  ny_mach_func_t mach = {0};
  uint32_t a_slot = 0, b_slot = 0, out_slot = 0, block = 0;
  ny_mach_reg_t a = 0, b = 0, sum = 0;
  bool ok = ny_mach_begin_block(&mach, 0, &block) &&
            ny_mach_alloc_typed_frame_slot(&mach, 8, 8, NY_MACH_TYPE_F64,
                                           false, &a_slot) &&
            ny_mach_alloc_typed_frame_slot(&mach, 8, 8, NY_MACH_TYPE_F64,
                                           false, &b_slot) &&
            ny_mach_alloc_typed_frame_slot(&mach, 8, 8, NY_MACH_TYPE_F64,
                                           false, &out_slot) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_F64, &a) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_F64, &b) &&
            ny_mach_alloc_typed_vreg(&mach, NY_MACH_TYPE_F64, &sum);
  if (ok)
    ok = ny_mach_emit(&mach, (ny_mach_inst_t){
        .opcode = NY_MACH_LOAD, .dst = selftest_fpr_vreg(a),
        .src0 = selftest_frame(a_slot), .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
        .opcode = NY_MACH_LOAD, .dst = selftest_fpr_vreg(b),
        .src0 = selftest_frame(b_slot), .effects = NY_MACH_EFFECT_READ_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
        .opcode = NY_MACH_ADD, .dst = selftest_fpr_vreg(sum),
        .src0 = selftest_fpr_vreg(a), .src1 = selftest_fpr_vreg(b)}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
        .opcode = NY_MACH_STORE, .dst = selftest_frame(out_slot),
        .src0 = selftest_fpr_vreg(sum), .effects = NY_MACH_EFFECT_WRITE_MEMORY}) &&
         ny_mach_emit(&mach, (ny_mach_inst_t){
        .opcode = NY_MACH_RET, .effects = NY_MACH_EFFECT_CONTROL});
  if (ok && !ny_mach_verify(&mach, err, err_len))
    ok = false;
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[8] = {0};
  ny_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  if (ok && !ny_a64_mach_build_bundle(&mach, NULL, NULL, 0, &target,
                                     "fpr_selftest", false, &code, defs,
                                     &def_count, relocs, &reloc_count, err,
                                     err_len))
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
    ny_native_set_err(err, err_len,
                      "AArch64 FPR selftest: allocated scalar FP load/add encoding missing");
    ok = false;
  }
  ny_obj_free(&code);
  ny_mach_func_free(&mach);
  return ok;
}

bool ny_native_vector_selftest(char *err, size_t err_len) {
  if (!vector_backend_selftest(NY_NATIVE_BACKEND_X86_64,
                               ny_x64_mach_build_bundle, "x86-64", err,
                               err_len))
    return false;
  if (!vector_backend_selftest(NY_NATIVE_BACKEND_AARCH64,
                               ny_a64_mach_build_bundle, "AArch64", err,
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
  return true;
}
