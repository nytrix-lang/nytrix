/*
 * Machine object encoding regression: a constant crosses a block boundary and
 * two COPY moves before DIV/MOD consume it.  The synthetic dynamic divisor
 * must retain the idivq fallback.
 */
#include "code/backend/object/internal.h"
#include "support.h"

#include <stdio.h>
#include <string.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
}

static nyr_mach_operand_t imm(int64_t value) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = value};
}

static bool emit(nyr_mach_func_t *mach, nyr_mach_opcode_t opcode,
                 nyr_mach_operand_t dst, nyr_mach_operand_t src0,
                 nyr_mach_operand_t src1, unsigned effects) {
  return nyr_mach_emit(mach, (nyr_mach_inst_t){.opcode = opcode,
                                               .dst = dst,
                                               .src0 = src0,
                                               .src1 = src1,
                                               .effects = effects});
}

static bool verify_fixed_register_result_is_colorable(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t dividend, quotient, one, sum;
  uint32_t block = 0;
  char err[256] = {0};
  bool ok = nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &dividend) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &quotient) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &one) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &sum) &&
            emit(&mach, NYR_MACH_COPY, vreg(dividend), imm(123),
                 (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE) &&
            emit(&mach, NYR_MACH_DIV, vreg(quotient), vreg(dividend), imm(2),
                 NYR_MACH_EFFECT_NONE) &&
            emit(&mach, NYR_MACH_COPY, vreg(one), imm(1),
                 (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE) &&
            emit(&mach, NYR_MACH_ADD, vreg(sum), vreg(quotient), vreg(one),
                 NYR_MACH_EFFECT_NONE) &&
            emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(sum),
                 (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_CONTROL) &&
            nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
            nyr_mach_regalloc_build(&mach, 12, &alloc);
  const nyr_mach_live_segment_t *result =
      ok ? nyr_mach_regalloc_segment_at(&alloc, quotient, 1) : NULL;
  ok = ok && result && result->color >= 0 && !result->spill && !result->reload;
  if (!ok)
    fprintf(stderr,
            "[magic-div] fixed-register result should stay in a colored "
            "register: %s\n",
            err[0] ? err : "result interval was spilled");
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool verify_nonclobbered_value_stays_resident_across_div(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t carry, dividend, divisor, quotient, result;
  uint32_t block = 0;
  char err[256] = {0};
  bool ok = nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &carry) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &dividend) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &divisor) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &quotient) &&
            nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &result) &&
            emit(&mach, NYR_MACH_COPY, vreg(carry), imm(41),
                 (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE) &&
            emit(&mach, NYR_MACH_COPY, vreg(dividend), imm(100),
                 (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE) &&
            emit(&mach, NYR_MACH_COPY, vreg(divisor), imm(4),
                 (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE) &&
            emit(&mach, NYR_MACH_DIV, vreg(quotient), vreg(dividend),
                 vreg(divisor), NYR_MACH_EFFECT_NONE) &&
            emit(&mach, NYR_MACH_ADD, vreg(result), vreg(carry), vreg(quotient),
                 NYR_MACH_EFFECT_NONE) &&
            emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(result),
                 (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_CONTROL) &&
            nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
            nyr_mach_regalloc_build(&mach, 12, &alloc);
  const nyr_mach_live_segment_t *before =
      ok ? nyr_mach_regalloc_segment_at(&alloc, carry, 3) : NULL;
  const nyr_mach_live_segment_t *after =
      ok ? nyr_mach_regalloc_segment_at(&alloc, carry, 4) : NULL;
  ok = ok && before && after && before == after && before->color >= 0 &&
       before->color != 11 && !before->spill && !before->reload;
  if (!ok) {
    fprintf(stderr,
            "[magic-div] callee-saved value should stay resident "
            "across idiv: %s\n",
            err[0] ? err : "register allocation split or homed the value");
    if (before)
      fprintf(stderr,
              "[magic-div] before segment color=%d range=%zu..%zu spill=%d "
              "reload=%d\n",
              before->color, before->start, before->end, (int)before->spill,
              (int)before->reload);
    if (after)
      fprintf(stderr,
              "[magic-div] after segment color=%d range=%zu..%zu spill=%d "
              "reload=%d\n",
              after->color, after->start, after->end, (int)after->spill,
              (int)after->reload);
  }
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool verify_magic_division_splits_scratch_color_live_ranges(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t live[12], dividend, divisor, remainder, sum, next;
  uint32_t block = 0;
  char err[256] = {0};
  bool ok = nyr_mach_begin_block(&mach, 0, &block) && block == 0;
  for (size_t i = 0; ok && i < 12; ++i)
    ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &live[i]) &&
         emit(&mach, NYR_MACH_COPY, vreg(live[i]), imm((int64_t)i + 1),
              (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE);
  ok = ok && nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &dividend) &&
       nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &divisor) &&
       nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &remainder) &&
       nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &sum) &&
       emit(&mach, NYR_MACH_COPY, vreg(dividend), imm(123456),
            (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE) &&
       emit(&mach, NYR_MACH_COPY, vreg(divisor), imm(1000),
            (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE);
  size_t mod_index = mach.inst_len;
  ok = ok && emit(&mach, NYR_MACH_MOD, vreg(remainder), vreg(dividend),
                  vreg(divisor), NYR_MACH_EFFECT_NONE);
  if (ok) {
    ok = emit(&mach, NYR_MACH_COPY, vreg(sum), vreg(live[0]),
              (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE);
    for (size_t i = 1; ok && i < 12; ++i) {
      ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &next) &&
           emit(&mach, NYR_MACH_ADD, vreg(next), vreg(sum), vreg(live[i]),
                NYR_MACH_EFFECT_NONE);
      sum = next;
    }
  }
  ok = ok && nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &next) &&
       emit(&mach, NYR_MACH_ADD, vreg(next), vreg(sum), vreg(remainder),
            NYR_MACH_EFFECT_NONE) &&
       emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(next),
            (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_CONTROL) &&
       nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
       nyr_mach_regalloc_build(&mach, 12, &alloc);
  for (size_t i = 0; ok && i < 12; ++i) {
    const nyr_mach_live_segment_t *before =
        nyr_mach_regalloc_segment_at(&alloc, live[i], mod_index - 1);
    const nyr_mach_live_segment_t *after =
        nyr_mach_regalloc_segment_at(&alloc, live[i], mod_index + 1);
    if (before && after && before == after && before->color >= 8 &&
        before->color <= 11) {
      ok = false;
      break;
    }
  }
  if (!ok)
    fprintf(stderr,
            "[magic-div] magic division must split live ranges in its "
            "R8/R9/RCX/RDX scratch colors: %s\n",
            err[0] ? err : "a value remained resident across MOD");
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool verify_call_clobbers_are_limited_to_live_through_values(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t carry, values[6], acc, next;
  uint32_t block = 0;
  char err[256] = {0};
  bool ok =
      nyr_mach_begin_block(&mach, 0, &block) && block == 0 &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &carry) &&
      emit(&mach, NYR_MACH_COPY, vreg(carry), imm(41), (nyr_mach_operand_t){0},
           NYR_MACH_EFFECT_NONE) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_CALL,
                                      .src0 = {.kind = NYR_MACH_OPERAND_SYMBOL,
                                               .as.symbol = "allocator_noop"},
                                      .effects = NYR_MACH_EFFECT_CALL}) &&
      nyr_mach_verify(&mach, 0, err, sizeof(err));
  for (size_t i = 0; ok && i < 6; ++i)
    ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &values[i]) &&
         emit(&mach, NYR_MACH_COPY, vreg(values[i]), imm((int64_t)i + 1),
              (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE);
  ok = ok && nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &acc) &&
       emit(&mach, NYR_MACH_COPY, vreg(acc), vreg(values[0]),
            (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE);
  for (size_t i = 1; ok && i < 6; ++i) {
    ok = nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &next) &&
         emit(&mach, NYR_MACH_ADD, vreg(next), vreg(acc), vreg(values[i]),
              NYR_MACH_EFFECT_NONE);
    acc = next;
  }
  ok = ok && nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &next) &&
       emit(&mach, NYR_MACH_ADD, vreg(next), vreg(acc), vreg(carry),
            NYR_MACH_EFFECT_NONE) &&
       emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(next),
            (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_CONTROL) &&
       nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
       nyr_mach_regalloc_build_callee_saved_gpr(&mach, 12, &alloc);
  const nyr_mach_live_segment_t *carry_seg =
      ok ? nyr_mach_regalloc_segment_at(&alloc, carry, 2) : NULL;
  size_t caller_saved_locals = 0;
  for (size_t i = 0; ok && i < 6; ++i) {
    const nyr_mach_live_segment_t *seg =
        nyr_mach_regalloc_segment_at(&alloc, values[i], 7);
    if (!seg || seg->spill || seg->reload || seg->color < 0) {
      ok = false;
      break;
    }
    if (seg->color >= 5)
      ++caller_saved_locals;
  }
  ok = ok && carry_seg && carry_seg->color >= 0 && carry_seg->color < 5 &&
       !carry_seg->spill && caller_saved_locals >= 2;
  if (!ok)
    fprintf(stderr,
            "[magic-div] call clobbers should constrain only live-through "
            "values: %s\n",
            err[0] ? err : "call-block locals stayed restricted or spilled");
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool verify_nir_constant_pow2_division_lowers_immediate(void) {
  const char *name = "magic-div-constant-pow2-lowering";
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_mach_func_t fallback = {0};
  char err[256] = {0};
  nyr_opt_begin(&ir);
  int dividend = nyr_opt_const(&ir, -3);
  int divisor = nyr_opt_const(&ir, 2);
  int quotient = nyr_opt_binop(&ir, NYIR_DIV_I64, dividend, divisor);
  nyr_opt_ret(&ir, quotient);

  bool ok = nyr_opt_verify(&ir, name) && nyr_opt_eval(&ir, name) == -1 &&
            nyr_mach_lower_nir(&ir, &mach, NY_NATIVE_CAP_MACH_SDIV_POW2, err,
                               sizeof(err)) &&
            nyr_mach_verify(&mach, 0, err, sizeof(err));
  bool found = false, fallback_kept_register_divisor = false;
  for (size_t i = 0; ok && i < mach.inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach.insts[i];
    if (in->opcode != NYR_MACH_DIV)
      continue;
    found = true;
    ok = in->src1.kind == NYR_MACH_OPERAND_IMM && in->src1.as.imm == 2;
  }
  ok = ok && nyr_mach_lower_nir(&ir, &fallback, 0, err, sizeof(err));
  for (size_t i = 0; ok && i < fallback.inst_len; ++i) {
    const nyr_mach_inst_t *in = &fallback.insts[i];
    if (in->opcode == NYR_MACH_DIV) {
      fallback_kept_register_divisor = in->src1.kind == NYR_MACH_OPERAND_VREG;
      break;
    }
  }
  ok = ok && fallback_kept_register_divisor;
  if (!ok || !found)
    fprintf(stderr,
            "[magic-div] constant signed power-of-two divisor was not "
            "lowered as an immediate: %s\n",
            err[0] ? err : "expected DIV with immediate 2");
  nyr_mach_func_free(&mach);
  nyr_mach_func_free(&fallback);
  nyr_ir_func_free(&ir);
  return ok && found;
}

static bool verify_strength_reduced_signed_pow2_division_fuses(void) {
  const char *name = "magic-div-strength-reduced-signed-pow2";
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  char err[256] = {0};
  nyr_opt_begin(&ir);
  int dividend =
      nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = 0,
                                    .scalar_class = NYIR_SCALAR_I64_RAW});
  int divisor = nyr_opt_const(&ir, 2);
  int quotient = nyr_opt_binop(&ir, NYIR_DIV_I64, dividend, divisor);
  nyr_opt_ret(&ir, quotient);
  bool ok = nyr_opt_verify(&ir, name) && nyr_strength_reduce(&ir) &&
            nyr_opt_verify(&ir, "magic-div/strength-reduced");
  for (size_t i = 0; ok && i < ir.len; ++i)
    ok = ir.data[i].op != NYIR_DIV_I64;
  ok = ok &&
       nyr_mach_lower_nir(&ir, &mach, NY_NATIVE_CAP_MACH_SDIV_POW2, err,
                          sizeof(err)) &&
       nyr_mach_verify(&mach, 0, err, sizeof(err));
  size_t div_count = 0;
  for (size_t i = 0; ok && i < mach.inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach.insts[i];
    if (in->opcode == NYR_MACH_DIV) {
      ++div_count;
      ok = in->src1.kind == NYR_MACH_OPERAND_IMM && in->src1.as.imm == 2;
    } else {
      ok = in->opcode != NYR_MACH_SAR && in->opcode != NYR_MACH_AND &&
           in->opcode != NYR_MACH_ADD;
    }
  }
  ok = ok && div_count == 1;
  if (!ok)
    fprintf(stderr,
            "[magic-div] signed power-of-two quotient did not fuse to one "
            "immediate machine DIV: %s\n",
            err[0] ? err : "expected one signed DIV by immediate 2");
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok;
}

int main(void) {
  if (!verify_fixed_register_result_is_colorable() ||
      !verify_nonclobbered_value_stays_resident_across_div() ||
      !verify_magic_division_splits_scratch_color_live_ranges() ||
      !verify_call_clobbers_are_limited_to_live_through_values() ||
      !verify_nir_constant_pow2_division_lowers_immediate() ||
      !verify_strength_reduced_signed_pow2_division_fuses())
    return 1;
  nyr_mach_func_t mach = {0};
  nyr_mach_reg_t dividend, divisor, copy0, copy1, quotient, remainder;
  nyr_mach_reg_t dynamic_divisor, dynamic_quotient;
  uint32_t block = 0;
  char err[256] = {0};
  if (!nyr_mach_begin_block(&mach, 0, &block) || block != 0 ||
      !nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &dividend) ||
      !nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &divisor) ||
      !nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &copy0) ||
      !nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &copy1) ||
      !nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &quotient) ||
      !nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &remainder) ||
      !nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &dynamic_divisor) ||
      !nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &dynamic_quotient) ||
      !emit(&mach, NYR_MACH_COPY, vreg(dividend), imm(123),
            (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE) ||
      !emit(&mach, NYR_MACH_COPY, vreg(divisor), imm(97),
            (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE) ||
      !emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0},
            (nyr_mach_operand_t){0},
            (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_BLOCK,
                                 .as.block_index = 1},
            NYR_MACH_EFFECT_CONTROL) ||
      !nyr_mach_begin_block(&mach, 1, &block) || block != 1 ||
      !emit(&mach, NYR_MACH_COPY, vreg(copy0), vreg(divisor),
            (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE) ||
      !emit(&mach, NYR_MACH_COPY, vreg(copy1), vreg(copy0),
            (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_NONE) ||
      !emit(&mach, NYR_MACH_DIV, vreg(quotient), vreg(dividend), vreg(copy1),
            NYR_MACH_EFFECT_NONE) ||
      !emit(&mach, NYR_MACH_MOD, vreg(remainder), vreg(dividend), vreg(copy1),
            NYR_MACH_EFFECT_NONE) ||
      !emit(&mach, NYR_MACH_DIV, vreg(dynamic_quotient), vreg(dividend),
            vreg(dynamic_divisor), NYR_MACH_EFFECT_NONE) ||
      !emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(quotient),
            (nyr_mach_operand_t){0}, NYR_MACH_EFFECT_CONTROL) ||
      !nyr_mach_verify(&mach, 0, err, sizeof(err))) {
    fprintf(stderr, "[magic-div] setup failed: %s\n", err);
    nyr_mach_func_free(&mach);
    return 1;
  }

  unsigned long long before_magic = 0, before_idiv = 0;
  unsigned long long after_magic = 0, after_idiv = 0;
  nyr_native_mach_div_stats(&before_magic, &before_idiv);
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  /*
   * The fixture has no external calls or symbol references.
   */
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64,
      .abi = NY_NATIVE_ABI_SYSV,
      .symbol_prefix = "",
  };
  bool encoded = nyr_x64_mach_append_function(
      &code, defs, &def_count, relocs, &reloc_count, &mach, &target,
      "magic_div", false, err, sizeof(err));
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  if (!encoded) {
    fprintf(stderr, "[magic-div] encode failed: %s\n", err);
    return 1;
  }
  nyr_native_mach_div_stats(&after_magic, &after_idiv);
  unsigned long long magic = after_magic - before_magic;
  unsigned long long idiv = after_idiv - before_idiv;
  if (magic != 2 || idiv != 1) {
    fprintf(stderr,
            "[magic-div] want magic=2 idiv=1 got magic=%llu idiv=%llu\n", magic,
            idiv);
    return 1;
  }
  printf("[magic-div] magic=2 idiv=1\n");
  return 0;
}
