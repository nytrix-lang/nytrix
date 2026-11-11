/* x86-64 machine-form target checks; included by object.c. */
/*
 * Loop homes span blocks, so conservatively keep their optimization disabled
 * when an indirect store or an opaque memory effect may alias those homes.
 */
static bool mach_has_indirect_memory(const nyr_mach_func_t *mach) {
  if (!mach)
    return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    const nyr_mach_operand_t *address = NULL;
    if (in->opcode == NYR_MACH_LOAD)
      address = &in->src0;
    else if (in->opcode == NYR_MACH_STORE)
      address = &in->dst;
    if (mach_is_known_copy(in))
      return true;
    if (address && address->kind == NYR_MACH_OPERAND_VREG)
      return true;
  }
  return false;
}

static bool mach_has_indirect_store(const nyr_mach_func_t *mach) {
  if (!mach)
    return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG)
      return true;
    if (mach_is_known_copy(in))
      return true;
  }
  return false;
}

static bool mach_has_unknown_memory_effect(const nyr_mach_func_t *mach) {
  if (!mach)
    return false;
  for (size_t i = 0; i < mach->inst_len; ++i)
    if ((mach->insts[i].opcode == NYR_MACH_INTRINSIC &&
         !mach_is_known_copy(&mach->insts[i])) ||
        mach->insts[i].opcode == NYR_MACH_INLINE_ASM)
      return true;
  return false;
}

static bool mach_validate_target_features(nyr_x64_mach_enc_t *e) {
  const nyr_mach_func_t *mach = e->mach;
  /*
   * Check the actual machine operations and call operands, not just the
   * vreg table: hand-built machine IR can otherwise bypass target lowering's
   * feature selection.
   */
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NYR_MACH_FMA && mach_is_v128(mach, &in->dst)) {
      if (!e->target || !(e->target->caps & NY_NATIVE_CAP_MACH_AVX))
        return mach_err(e, "x64: vector FMA operation requires AVX");
      if (!(e->target->caps & NY_NATIVE_CAP_MACH_FMA))
        return mach_err(e, "x64: vector FMA operation requires FMA");
    }
    if (!mach_inst_uses_v256(mach, in))
      continue;
    if (!e->target || !(e->target->caps & NY_NATIVE_CAP_MACH_AVX))
      return mach_err(e, "x64: V256 operation requires AVX");
    if (mach_inst_uses_v256_i64(mach, in) &&
        !(e->target->caps & NY_NATIVE_CAP_MACH_AVX2)) {
      bool avx_only_op =
          in->opcode == NYR_MACH_LOAD || in->opcode == NYR_MACH_STORE ||
          in->opcode == NYR_MACH_REDUCE_ADD || in->opcode == NYR_MACH_COPY ||
          in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_SUB ||
          in->opcode == NYR_MACH_AND || in->opcode == NYR_MACH_OR ||
          in->opcode == NYR_MACH_XOR;
      if (!avx_only_op)
        return mach_err(
            e, "x64: V256_I64 operation requires AVX2 (fallback unavailable)");
    }
  }
  return true;
}

static bool mach_configure_function_features(nyr_x64_mach_enc_t *e) {
  const nyr_mach_func_t *mach = e->mach;
  bool vector_fast_eligible = mach_vector_fast_eligible(mach);
  /*
   * AVX1's i64 add/sub expansion uses xmm0-xmm3 as temporary lanes; keep
   * values in canonical homes so those temporaries cannot clobber residents.
   */
  if (vector_fast_eligible && e->target &&
      !(e->target->caps & NY_NATIVE_CAP_MACH_AVX2)) {
    for (size_t i = 0; i < mach->inst_len; ++i) {
      const nyr_mach_inst_t *in = &mach->insts[i];
      if ((in->opcode == NYR_MACH_ADD || in->opcode == NYR_MACH_SUB) &&
          mach_inst_uses_v256_i64(mach, in)) {
        vector_fast_eligible = false;
        break;
      }
    }
  }
  e->vec_fast_path = vector_fast_eligible;
  e->fp_fast_path = mach_float_fast_eligible(mach);
  for (size_t i = 0; e->fp_fast_path && i < mach->inst_len; ++i)
    if (mach->insts[i].opcode == NYR_MACH_SIN ||
        mach->insts[i].opcode == NYR_MACH_COS)
      e->fp_fast_path = false;
  /*
   * Four-lane f64 shuffles use xmm0-xmm3 as scratch while scalar float
   * colors span xmm0-xmm13; the two fast paths must not coexist.
   */
  for (size_t i = 0; e->fp_fast_path && i < mach->inst_len; ++i)
    if (mach->insts[i].opcode == NYR_MACH_SHUFFLE &&
        mach->insts[i].dst.kind == NYR_MACH_OPERAND_VREG &&
        mach->insts[i].dst.as.reg < mach->vreg_len &&
        mach->vreg_types[mach->insts[i].dst.as.reg] == NYR_MACH_TYPE_V256_F64)
      e->fp_fast_path = false;
  /*
   * Slow-path vector scratch sequences use xmm0-xmm3, so scalar float
   * residency yields whenever vector fast lowering is unavailable.
   */
  if (!e->vec_fast_path && mach_vector_types_present(mach))
    e->fp_fast_path = false;
  e->fp_color_limit = e->vec_fast_path ? 8 : MFP_COLOR_N;
  e->vec_color_n = MVEC_COLOR_N;
  e->uses_ymm = false;
  e->vex = e->target && (e->target->caps & NY_NATIVE_CAP_MACH_AVX) &&
           !getenv("NYTRIX_NO_VEX_SSE");
  for (size_t i = 0; mach->vreg_types && i < mach->vreg_len && !e->uses_ymm;
       ++i)
    e->uses_ymm = mach->vreg_types[i] == NYR_MACH_TYPE_V256_I64 ||
                  mach->vreg_types[i] == NYR_MACH_TYPE_V256_F64;
  if (e->vec_fast_path && e->target && e->target->abi == NY_NATIVE_ABI_SYSV &&
      (e->target->caps & NY_NATIVE_CAP_MACH_AVX2) &&
      !mach_scalar_float_types_present(mach))
    e->vec_color_n = MVEC_COLOR_WIDE_N;
  return vector_fast_eligible;
}
