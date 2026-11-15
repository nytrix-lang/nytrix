/*
 * Focused repro: a vectorized reduction loop (VEC8_LOAD_I64 +
 * VEC8_REDUCE_ADD_I64 over a loop-carried scalar accumulator) executed
 * through the x86-64 machine form. The VM evaluates the same NYIR
 * correctly; this checks whether the machine lowering agrees.
 */
#if defined(__linux__) && defined(__x86_64__)

#include "code/backend/core/internal.h"
#include "code/backend/mach/object.h"
#include "opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int emit(nyr_func_t *f, nyr_op_t op, int a, int b, int c, int64_t imm) {
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

static nyr_mach_operand_t m_vreg(nyr_mach_reg_t reg,
                                 nyr_mach_reg_class_t cls) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = cls,
                              .as.reg = reg};
}

static nyr_mach_operand_t m_imm(int64_t value) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = value};
}

static nyr_mach_operand_t m_block(uint32_t index) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_BLOCK,
                              .as.block_index = index};
}

static bool m_emit(nyr_mach_func_t *mach, nyr_mach_opcode_t opcode,
                   nyr_mach_operand_t dst, nyr_mach_operand_t src0,
                   nyr_mach_operand_t src1) {
  return nyr_mach_emit(mach, (nyr_mach_inst_t){.opcode = opcode,
                                               .dst = dst,
                                               .src0 = src0,
                                               .src1 = src1});
}

static bool check_vec8_set1_machine(void) {
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int scalar = nyr_opt_const(&ir, 9);
  int zero = nyr_opt_const(&ir, 0);
  int splat = emit(&ir, NYIR_VEC8_SET1_I64, scalar, -1, -1, 0);
  int sum = emit(&ir, NYIR_VEC8_REDUCE_ADD_I64, zero, splat, -1, 0);
  nyr_opt_ret(&ir, sum);
  bool ok = nyr_opt_verify(&ir, "vec8-set1-machine");

  nyr_mach_func_t mach = {0};
  char err[256] = {0};
  ny_options opt;
  ny_options_init(&opt);
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  ok = ok && nyr_target_info_init(&target, &request) &&
       nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err));

  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  ny_native_target_info_t avx_only = target;
  avx_only.caps &= ~NY_NATIVE_CAP_MACH_AVX2;
  ok = ok && nyr_x64_mach_append_function(
                 &code, defs, &def_count, relocs, &reloc_count, &mach,
                 &avx_only, "vec8_set1_machine", false, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count,
                                           "vec8_set1_machine")
                  : -1;
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
  int64_t result = -1;
  if (ok) {
    typedef int64_t (*fn_t)(void);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    result = fn();
    ok = result == 36;
  }
  if (!ok)
    fprintf(stderr, "[vec8-set1-machine] result=%lld (%s)\n",
            (long long)result, err[0] ? err : "expected 36");
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  if (ok)
    puts("[vec8-set1-machine] AVX-only splat and reduction execute correctly");
  return ok;
}

static bool check_vec8_bitwise_avx_machine(void) {
  ny_options opt;
  ny_options_init(&opt);
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  if (!nyr_target_info_init(&target, &request) ||
      !(target.caps & NY_NATIVE_CAP_MACH_AVX)) {
    puts("[vec8-bitwise-avx] skipped: host has no AVX");
    return true;
  }

  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int nine = nyr_opt_const(&ir, 9);
  int three = nyr_opt_const(&ir, 3);
  int zero = nyr_opt_const(&ir, 0);
  int a = emit(&ir, NYIR_VEC8_SET1_I64, nine, -1, -1, 0);
  int b = emit(&ir, NYIR_VEC8_SET1_I64, three, -1, -1, 0);
  int and_v = emit(&ir, NYIR_VEC8_AND_I64, a, b, -1, 0);
  int or_v = emit(&ir, NYIR_VEC8_OR_I64, a, b, -1, 0);
  int xor_v = emit(&ir, NYIR_VEC8_XOR_I64, a, b, -1, 0);
  int and_sum = emit(&ir, NYIR_VEC8_REDUCE_ADD_I64, zero, and_v, -1, 0);
  int or_sum = emit(&ir, NYIR_VEC8_REDUCE_ADD_I64, and_sum, or_v, -1, 0);
  int result = emit(&ir, NYIR_VEC8_REDUCE_ADD_I64, or_sum, xor_v, -1, 0);
  nyr_opt_ret(&ir, result);

  char err[256] = {0};
  nyr_mach_func_t mach = {0};
  bool ok = nyr_opt_verify(&ir, "vec8-bitwise-avx") &&
            nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err));
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  ny_native_target_info_t avx_only = target;
  avx_only.caps &= ~NY_NATIVE_CAP_MACH_AVX2;
  ok = ok && nyr_x64_mach_append_function(
                 &code, defs, &def_count, relocs, &reloc_count, &mach,
                 &avx_only, "vec8_bitwise_avx", false, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count,
                                           "vec8_bitwise_avx")
                  : -1;
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
  int64_t actual = -1;
  if (ok) {
    typedef int64_t (*fn_t)(void);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    actual = fn();
    ok = actual == 88;
  }
  if (!ok)
    fprintf(stderr, "[vec8-bitwise-avx] result=%lld (%s)\n",
            (long long)actual, err[0] ? err : "expected 88");
  if (!ok)
    nyr_mach_dump(&mach, stderr);
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  if (ok)
    puts("[vec8-bitwise-avx] AVX1 bitwise ops preserve all i64 lane bits");
  return ok;
}

static bool check_vec8_add_avx1_fallback(void) {
  ny_options opt;
  ny_options_init(&opt);
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  if (!nyr_target_info_init(&target, &request) ||
      !(target.caps & NY_NATIVE_CAP_MACH_AVX)) {
    puts("[vec8-avx1-add] skipped: host has no AVX");
    return true;
  }

  nyr_func_t ir;
  nyr_opt_begin(&ir);
  int scalar = nyr_opt_const(&ir, 1);
  int other_scalar = nyr_opt_const(&ir, 2);
  int zero = nyr_opt_const(&ir, 0);
  int a = emit(&ir, NYIR_VEC8_SET1_I64, scalar, -1, -1, 0);
  int b = emit(&ir, NYIR_VEC8_SET1_I64, other_scalar, -1, -1, 0);
  int sum = emit(&ir, NYIR_VEC8_ADD_I64, a, b, -1, 0);
  int diff = emit(&ir, NYIR_VEC8_SUB_I64, b, a, -1, 0);
  int reduced = emit(&ir, NYIR_VEC8_REDUCE_ADD_I64, zero, sum, -1, 0);
  int diff_reduced =
      emit(&ir, NYIR_VEC8_REDUCE_ADD_I64, zero, diff, -1, 0);
  int result = emit(&ir, NYIR_ADD_I64, reduced, diff_reduced, -1, 0);
  nyr_opt_ret(&ir, result);

  char err[256] = {0};
  nyr_mach_func_t mach = {0};
  bool lowered = nyr_opt_verify(&ir, "vec8-avx1-add") &&
                 nyr_mach_lower_nir(&ir, &mach, target.caps, err,
                                    sizeof(err));
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  ny_native_target_info_t avx_only = target;
  avx_only.caps &= ~NY_NATIVE_CAP_MACH_AVX2;
  bool ok = lowered && nyr_x64_mach_append_function(
      &code, defs, &def_count, relocs, &reloc_count, &mach, &avx_only,
      "vec8_add_avx1_fallback", false, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count,
                                           "vec8_add_avx1_fallback")
                  : -1;
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
  int64_t actual = -1;
  if (ok) {
    typedef int64_t (*fn_t)(void);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    actual = fn();
    ok = actual == 16;
  }
  if (!ok)
    fprintf(stderr, "[vec8-avx1-add] result=%lld (%s)\n",
            (long long)actual, err[0] ? err : "expected 16");
  if (!ok && lowered)
    nyr_mach_dump(&mach, stderr);
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  if (ok)
    puts("[vec8-avx1-add] AVX1 plus SSE2 add/sub fallback executes correctly");
  return ok;
}

static bool check_v256_f64_requires_avx(void) {
  ny_options opt;
  ny_options_init(&opt);
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  if (!nyr_target_info_init(&target, &request))
    return false;

  nyr_mach_func_t mach = {0};
  nyr_mach_reg_t address, vector;
  uint32_t block;
  char err[256] = {0};
  bool ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_PTR, &address) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V256_F64, &vector) &&
            nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_COPY,
                                      .dst = {.kind = NYR_MACH_OPERAND_VREG,
                                              .reg_class = NYR_MACH_REGCLASS_GPR,
                                              .as.reg = address},
                                      .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                               .as.imm = 0},
                                  }) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_LOAD,
                                      .dst = {.kind = NYR_MACH_OPERAND_VREG,
                                              .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                              .as.reg = vector},
                                      .src0 = {.kind = NYR_MACH_OPERAND_VREG,
                                               .reg_class = NYR_MACH_REGCLASS_GPR,
                                               .as.reg = address},
                                  }) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_RET,
                                      .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                               .as.imm = 0},
                                      .effects = NYR_MACH_EFFECT_CONTROL,
                                  });
  if (!ok) {
    nyr_mach_func_free(&mach);
    return false;
  }
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  ny_native_target_info_t sse_only = target;
  sse_only.caps &= ~(NY_NATIVE_CAP_MACH_AVX | NY_NATIVE_CAP_MACH_AVX2);
  bool accepted = nyr_x64_mach_append_function(
      &code, defs, &def_count, relocs, &reloc_count, &mach, &sse_only,
      "v256_f64_without_avx", false, err, sizeof(err));
  ok = !accepted && strstr(err, "requires AVX") != NULL;
  if (!ok)
    fprintf(stderr, "[vec8-avx-gate] V256_F64 accepted without AVX (%s)\n",
            err[0] ? err : "no diagnostic");
  else
    puts("[vec8-avx-gate] V256_F64 rejected without AVX");
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool check_v256_fma_requires_fma(void) {
  ny_options opt;
  ny_options_init(&opt);
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  if (!nyr_target_info_init(&target, &request))
    return false;

  nyr_mach_func_t mach = {0};
  nyr_mach_reg_t a, b, c, result;
  uint32_t block;
  char err[256] = {0};
  bool ok =
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V256_F64, &a) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V256_F64, &b) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V256_F64, &c) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V256_F64, &result) &&
      nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                .opcode = NYR_MACH_FMA,
                                .dst = {.kind = NYR_MACH_OPERAND_VREG,
                                        .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                        .as.reg = result},
                                .src0 = {.kind = NYR_MACH_OPERAND_VREG,
                                         .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                         .as.reg = a},
                                .src1 = {.kind = NYR_MACH_OPERAND_VREG,
                                         .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                         .as.reg = b},
                                .src2 = {.kind = NYR_MACH_OPERAND_VREG,
                                         .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                         .as.reg = c},
                            }) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                .opcode = NYR_MACH_RET,
                                .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                         .as.imm = 0},
                                .effects = NYR_MACH_EFFECT_CONTROL,
                            });
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  ny_native_target_info_t no_fma = target;
  no_fma.caps |= NY_NATIVE_CAP_MACH_AVX;
  no_fma.caps &= ~NY_NATIVE_CAP_MACH_FMA;
  bool accepted = ok && nyr_x64_mach_append_function(
                            &code, defs, &def_count, relocs, &reloc_count,
                            &mach, &no_fma, "v256_fma_without_fma", false,
                            err, sizeof(err));
  ok = ok && !accepted && strstr(err, "requires FMA") != NULL;
  if (!ok)
    fprintf(stderr, "[vec8-fma-gate] V256_F64 FMA accepted without FMA (%s)\n",
            err[0] ? err : "no diagnostic");
  else
    puts("[vec8-fma-gate] V256_F64 FMA rejected without FMA");
  nyr_obj_free(&code);
  memset(&code, 0, sizeof(code));
  ny_native_target_info_t no_avx = target;
  no_avx.caps &= ~(NY_NATIVE_CAP_MACH_AVX | NY_NATIVE_CAP_MACH_AVX2);
  no_avx.caps |= NY_NATIVE_CAP_MACH_FMA;
  err[0] = '\0';
  accepted = ok && nyr_x64_mach_append_function(
                        &code, defs, &def_count, relocs, &reloc_count, &mach,
                        &no_avx, "v256_fma_without_avx", false, err,
                        sizeof(err));
  ok = ok && !accepted && strstr(err, "requires AVX") != NULL;
  if (!ok)
    fprintf(stderr, "[vec8-fma-gate] vector FMA accepted without AVX (%s)\n",
            err[0] ? err : "no diagnostic");
  else
    puts("[vec8-fma-gate] vector FMA rejected without AVX");
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

/*
 * Preserve every lane of a vector PHI across a real loop backedge. The four
 * distinct input lanes make a low-lane-only carry visibly incorrect.
 */
static bool check_vector_phi_carry(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t pointer, initial, phi, iv, next_vec, next_iv, one, limit;
  nyr_mach_reg_t condition, result, zero;
  uint32_t entry, header, body, latch, exit;
  char err[256] = {0};
  nyr_mach_operand_t lanes_operand;
  int64_t lanes[4] = {1, 2, 3, 4};
  lanes_operand = (nyr_mach_operand_t){
      .kind = NYR_MACH_OPERAND_IMM,
      .as.imm = (int64_t)(intptr_t)lanes,
  };
  bool ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_PTR, &pointer) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V256_I64, &initial) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V256_I64, &phi) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &iv) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V256_I64, &next_vec) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &next_iv) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &one) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &limit) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &condition) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &result) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &zero) &&
            nyr_mach_begin_block(&mach, 0, &entry) && entry == 0 &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_COPY,
                                      .dst = m_vreg(pointer, NYR_MACH_REGCLASS_GPR),
                                      .src0 = lanes_operand,
                                  }) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_LOAD,
                                      .dst = {.kind = NYR_MACH_OPERAND_VREG,
                                              .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                              .as.reg = initial},
                                      .src0 = m_vreg(pointer, NYR_MACH_REGCLASS_GPR),
                                      .effects = NYR_MACH_EFFECT_READ_MEMORY,
                                  }) &&
            m_emit(&mach, NYR_MACH_COPY, m_vreg(iv, NYR_MACH_REGCLASS_GPR),
                   m_imm(0), (nyr_mach_operand_t){0}) &&
            m_emit(&mach, NYR_MACH_COPY, m_vreg(one, NYR_MACH_REGCLASS_GPR),
                   m_imm(1), (nyr_mach_operand_t){0}) &&
            m_emit(&mach, NYR_MACH_COPY, m_vreg(limit, NYR_MACH_REGCLASS_GPR),
                   m_imm(5), (nyr_mach_operand_t){0}) &&
            m_emit(&mach, NYR_MACH_COPY, m_vreg(zero, NYR_MACH_REGCLASS_GPR),
                   m_imm(0), (nyr_mach_operand_t){0}) &&
            m_emit(&mach, NYR_MACH_COPY,
                   m_vreg(phi, NYR_MACH_REGCLASS_VECTOR),
                   m_vreg(initial, NYR_MACH_REGCLASS_VECTOR),
                   (nyr_mach_operand_t){0}) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_BR,
                                      .src1 = m_block(1),
                                      .effects = NYR_MACH_EFFECT_CONTROL,
                                  }) &&
            nyr_mach_begin_block(&mach, 1, &header) && header == 1 &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_CMP,
                                      .dst = m_vreg(condition, NYR_MACH_REGCLASS_GPR),
                                      .src0 = m_vreg(iv, NYR_MACH_REGCLASS_GPR),
                                      .src1 = m_vreg(limit, NYR_MACH_REGCLASS_GPR),
                                      .condition = NYR_MACH_COND_LT,
                                  }) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_BR_IF,
                                      .src0 = m_vreg(condition, NYR_MACH_REGCLASS_GPR),
                                      .src1 = m_block(2),
                                      .effects = NYR_MACH_EFFECT_CONTROL,
                                  }) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_BR,
                                      .src1 = m_block(4),
                                      .effects = NYR_MACH_EFFECT_CONTROL,
                                  }) &&
            nyr_mach_begin_block(&mach, 2, &body) && body == 2 &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_ADD,
                                      .dst = {.kind = NYR_MACH_OPERAND_VREG,
                                              .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                              .as.reg = next_vec},
                                      .src0 = {.kind = NYR_MACH_OPERAND_VREG,
                                               .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                               .as.reg = phi},
                                      .src1 = {.kind = NYR_MACH_OPERAND_VREG,
                                               .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                               .as.reg = initial},
                                  }) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_ADD,
                                      .dst = m_vreg(next_iv, NYR_MACH_REGCLASS_GPR),
                                      .src0 = m_vreg(iv, NYR_MACH_REGCLASS_GPR),
                                      .src1 = m_vreg(one, NYR_MACH_REGCLASS_GPR),
                                  }) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_BR,
                                      .src1 = m_block(3),
                                      .effects = NYR_MACH_EFFECT_CONTROL,
                                  }) &&
            nyr_mach_begin_block(&mach, 3, &latch) && latch == 3 &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_COPY,
                                      .dst = {.kind = NYR_MACH_OPERAND_VREG,
                                              .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                              .as.reg = phi},
                                      .src0 = {.kind = NYR_MACH_OPERAND_VREG,
                                               .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                               .as.reg = next_vec},
                                  }) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_COPY,
                                      .dst = m_vreg(iv, NYR_MACH_REGCLASS_GPR),
                                      .src0 = m_vreg(next_iv, NYR_MACH_REGCLASS_GPR),
                                  }) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_BR,
                                      .src1 = m_block(1),
                                      .effects = NYR_MACH_EFFECT_CONTROL,
                                  }) &&
            nyr_mach_begin_block(&mach, 4, &exit) && exit == 4 &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_REDUCE_ADD,
                                      .dst = m_vreg(result, NYR_MACH_REGCLASS_GPR),
                                      .src0 = m_vreg(zero, NYR_MACH_REGCLASS_GPR),
                                      .src1 = {.kind = NYR_MACH_OPERAND_VREG,
                                               .reg_class = NYR_MACH_REGCLASS_VECTOR,
                                               .as.reg = phi},
                                  }) &&
            nyr_mach_emit(&mach, (nyr_mach_inst_t){
                                      .opcode = NYR_MACH_RET,
                                      .src0 = m_vreg(result, NYR_MACH_REGCLASS_GPR),
                                      .effects = NYR_MACH_EFFECT_CONTROL,
                                  }) &&
            nyr_mach_verify(&mach, 0, err, sizeof(err));
  ny_options opt;
  ny_options_init(&opt);
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  ok = ok && nyr_target_info_init(&target, &request);
  if (ok && !(target.caps & NY_NATIVE_CAP_MACH_AVX)) {
    puts("[vec8-vector-phi-carry] skipped: host has no AVX");
    nyr_mach_func_free(&mach);
    return true;
  }

  const char *prior = getenv("NYTRIX_VECTOR_LOOP_CARRY");
  char *saved = prior ? strdup(prior) : NULL;
  bool had_prior = prior != NULL;
  ok = ok && (!had_prior || saved) &&
       nyr_opt_unsetenv("NYTRIX_VECTOR_LOOP_CARRY") == 0 &&
       nyr_mach_regalloc_build_class(&mach, NYR_MACH_REGCLASS_VECTOR, 14,
                                     &alloc);
  const nyr_mach_live_segment_t *carry = NULL;
  for (uint32_t v = 0; ok && v < mach.vreg_len; ++v) {
    if (mach.vreg_types[v] != NYR_MACH_TYPE_V256_I64 || v != phi)
      continue;
    const nyr_mach_live_segment_t *candidate =
        nyr_mach_regalloc_segment_at(&alloc, v, mach.blocks[1].first_inst);
    if (candidate && candidate->carried) {
      carry = candidate;
      break;
    }
  }
  ok = ok && carry && carry->carried && carry->color >= 0;

  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  ok = ok && nyr_x64_mach_append_function(
                 &code, defs, &def_count, relocs, &reloc_count, &mach,
                 &target, "vec8_vector_phi_carry", false, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count,
                                           "vec8_vector_phi_carry")
                  : -1;
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
  int64_t actual = -1;
  if (ok) {
    typedef int64_t (*fn_t)(void);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    actual = fn();
    ok = actual == 60;
  }
  if (had_prior)
    nyr_opt_setenv("NYTRIX_VECTOR_LOOP_CARRY", saved);
  else
    nyr_opt_unsetenv("NYTRIX_VECTOR_LOOP_CARRY");
  if (!ok)
    fprintf(stderr,
            "[vec8-vector-phi-carry] result=%lld carry=%p/%d/%d (%s)\n",
            (long long)actual, (const void *)carry,
            carry ? carry->carried : -1, carry ? carry->spill : -1,
            err[0] ? err : "expected all four lanes preserved (sum 60)");
  if (!ok) {
    nyr_mach_dump(&mach, stderr);
    for (size_t i = 0; i < alloc.segment_len; ++i) {
      const nyr_mach_live_segment_t *seg = &alloc.segments[i];
      fprintf(stderr, "  alloc v%u type=%d b%u [%zu,%zu] c%d carried=%d "
                      "spill=%d reload=%d\n",
              seg->vreg, mach.vreg_types[seg->vreg], seg->block, seg->start,
              seg->end, seg->color, seg->carried, seg->spill, seg->reload);
    }
  }
  else
    puts("[vec8-vector-phi-carry] all four lanes survive five loop backedges");
  free(saved);
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

int main(void) {
  if (!check_v256_f64_requires_avx() || !check_v256_fma_requires_fma() ||
      !check_vector_phi_carry())
    return 1;
  {
    ny_options opt;
    ny_options_init(&opt);
    opt.native_backend = NY_NATIVE_BACKEND_X86_64;
    const nyr_target_request_t request = nyr_target_request_make(
        opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
    ny_native_target_info_t target = {0};
    if (!nyr_target_info_init(&target, &request) ||
        !(target.caps & NY_NATIVE_CAP_MACH_AVX)) {
      puts("[vec8-reduce-loop] skipped: host has no AVX");
      return 0;
    }
  }
  if (!check_vec8_set1_machine() || !check_vec8_bitwise_avx_machine() ||
      !check_vec8_add_avx1_fallback())
    return 1;
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
  ny_native_target_info_t target = {0};
  bool target_ok = nyr_target_info_init(&target, &target_request);
  bool lowered = target_ok &&
                 nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err));
  bool appended =
      lowered && nyr_x64_mach_append_function(
                     &code, defs, &def_count, relocs, &reloc_count, &mach,
                     &target, "vec8_reduce_loop", false, err, sizeof(err));
  ok = appended;
  /*
   * The load/reduction subset uses AVX moves/extract plus SSE2 reduction and
   * must remain available to AVX-only targets.
   */
  nyr_obj_buf_t avx_only_code = {0};
  nyr_x64_obj_symbol_def_t avx_only_defs[2] = {0};
  nyr_x64_obj_reloc_t avx_only_relocs[2] = {0};
  size_t avx_only_def_count = 0, avx_only_reloc_count = 0;
  ny_native_target_info_t avx_only_target = target;
  avx_only_target.caps &= ~NY_NATIVE_CAP_MACH_AVX2;
  char avx_only_err[256] = {0};
  bool avx_only_appended =
      appended && nyr_x64_mach_append_function(
                      &avx_only_code, avx_only_defs, &avx_only_def_count,
                      avx_only_relocs, &avx_only_reloc_count, &mach,
                      &avx_only_target, "vec8_reduce_loop", false,
                      avx_only_err, sizeof(avx_only_err));
  int avx_only_symbol =
      avx_only_appended
          ? nyr_x64_obj_def_index(avx_only_defs, avx_only_def_count,
                                  "vec8_reduce_loop")
          : -1;
  void *avx_only_mapping = MAP_FAILED;
  if (avx_only_appended && avx_only_symbol >= 0 &&
      avx_only_reloc_count == 0) {
    avx_only_mapping = nyr_opt_execmem_alloc(avx_only_code.len);
    avx_only_appended = avx_only_mapping != MAP_FAILED;
  } else {
    avx_only_appended = false;
  }
  if (avx_only_appended) {
    memcpy(avx_only_mapping, avx_only_code.data, avx_only_code.len);
    avx_only_appended =
        nyr_opt_execmem_protect(avx_only_mapping, avx_only_code.len) == 0;
  }
  int64_t avx_only_result = -1;
  if (avx_only_appended) {
    int64_t cells[32];
    for (int i = 0; i < 32; ++i)
      cells[i] = i;
    typedef int64_t (*fn_t)(int64_t);
    fn_t fn = (fn_t)((unsigned char *)avx_only_mapping +
                     avx_only_defs[avx_only_symbol].off);
    avx_only_result = fn((int64_t)(intptr_t)cells);
    avx_only_appended = avx_only_result == 496;
  }
  if (!avx_only_appended) {
    fprintf(stderr, "[vec8-reduce-loop] AVX-only result=%lld (%s)\n",
            (long long)avx_only_result,
            avx_only_err[0] ? avx_only_err : "runtime mismatch");
    ok = false;
  }
  if (avx_only_mapping != MAP_FAILED)
    nyr_opt_execmem_free(avx_only_mapping, avx_only_code.len);
  nyr_obj_free(&avx_only_code);
  int symbol =
      ok ? nyr_x64_obj_def_index(defs, def_count, "vec8_reduce_loop") : -1;
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
    nyr_opt_execmem_free(mapping, code.len);
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
