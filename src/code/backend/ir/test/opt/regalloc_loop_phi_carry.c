/*
 * A loop phi copy chain may stay resident only when every incoming edge
 * agrees on the same physical register.
 */
#include "code/backend/ir/machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nyr_mach_operand_t vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
}

static nyr_mach_operand_t imm(int64_t value) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = value};
}

static nyr_mach_operand_t block(size_t index) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_BLOCK,
                              .as.block_index = (uint32_t)index};
}

static bool emit(nyr_mach_func_t *mach, nyr_mach_opcode_t opcode,
                 nyr_mach_operand_t dst, nyr_mach_operand_t src0,
                 nyr_mach_operand_t src1) {
  return nyr_mach_emit(
      mach, (nyr_mach_inst_t){
                .opcode = opcode,
                .dst = dst,
                .src0 = src0,
                .src1 = src1,
                .effects = (opcode == NYR_MACH_BR || opcode == NYR_MACH_RET)
                               ? NYR_MACH_EFFECT_CONTROL
                               : NYR_MACH_EFFECT_NONE});
}

static bool preheader_call_keeps_phi_source_out_of_clobbered_color(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t initial, phi, next, step;
  uint32_t entry, preheader, header, latch;
  char err[256] = {0};
  const char *prior_env = getenv("NYTRIX_GPR_LOOP_CARRY");
  char *saved_env = prior_env ? strdup(prior_env) : NULL;
  bool had_prior_env = prior_env != NULL;
  nyr_mach_operand_t callee = {
      .kind = NYR_MACH_OPERAND_SYMBOL,
      .as.symbol = "preheader_helper",
  };

  bool ok =
      (!had_prior_env || saved_env) &&
      setenv("NYTRIX_GPR_LOOP_CARRY", "1", 1) == 0 &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &initial) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &phi) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &next) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &step) &&
      nyr_mach_begin_block(&mach, 0, &entry) && entry == 0 &&
      emit(&mach, NYR_MACH_COPY, vreg(initial), imm(0),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(step), imm(1), (nyr_mach_operand_t){0}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_CALL,
                        .src0 = callee,
                        .effects = NYR_MACH_EFFECT_CALL,
                    }) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(1)) &&
      nyr_mach_begin_block(&mach, 1, &preheader) && preheader == 1 &&
      emit(&mach, NYR_MACH_COPY, vreg(phi), vreg(initial),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(2)) &&
      nyr_mach_begin_block(&mach, 2, &header) && header == 2 &&
      emit(&mach, NYR_MACH_ADD, vreg(next), vreg(phi), vreg(step)) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(3)) &&
      nyr_mach_begin_block(&mach, 3, &latch) && latch == 3 &&
      emit(&mach, NYR_MACH_COPY, vreg(phi), vreg(next),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(2)) &&
      nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
      nyr_mach_regalloc_build_callee_saved_gpr(&mach, 8, &alloc);

  const nyr_mach_live_segment_t *source =
      ok ? nyr_mach_regalloc_segment_at(&alloc, initial,
                                        mach.blocks[entry].first_inst)
         : NULL;
  const nyr_mach_live_segment_t *phi_source =
      ok ? nyr_mach_regalloc_segment_at(&alloc, initial,
                                        mach.blocks[preheader].first_inst)
         : NULL;
  const nyr_mach_live_segment_t *header_phi =
      ok ? nyr_mach_regalloc_segment_at(&alloc, phi,
                                        mach.blocks[header].first_inst)
         : NULL;
  ok = ok && source && phi_source && header_phi && source->color >= 0 &&
       source->color < 5 && phi_source->color < 5 && header_phi->color < 5;
  if (!ok)
    fprintf(stderr,
            "[regalloc-loop-phi-carry] preheader call clobbered carried "
            "source: source=%d/%d phi-source=%d/%d header=%d/%d error=%s\n",
            source ? source->color : -2, source ? source->carried : -1,
            phi_source ? phi_source->color : -2,
            phi_source ? phi_source->carried : -1,
            header_phi ? header_phi->color : -2,
            header_phi ? header_phi->carried : -1,
            err[0] ? err : "call-clobbered source register");

  if (had_prior_env)
    setenv("NYTRIX_GPR_LOOP_CARRY", saved_env, 1);
  else
    unsetenv("NYTRIX_GPR_LOOP_CARRY");
  free(saved_env);
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  if (ok)
    puts("[regalloc-loop-phi-carry] preheader call preserves phi source");
  return ok;
}
static bool call_in_loop_disables_carry(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t initial, phi, next, step;
  uint32_t entry, preheader, header, latch;
  char err[256] = {0};
  const char *prior_env = getenv("NYTRIX_GPR_LOOP_CARRY");
  char *saved_env = prior_env ? strdup(prior_env) : NULL;
  bool had_prior_env = prior_env != NULL;
  nyr_mach_operand_t callee = {
      .kind = NYR_MACH_OPERAND_SYMBOL,
      .as.symbol = "loop_body_helper",
  };

  bool ok =
      (!had_prior_env || saved_env) &&
      setenv("NYTRIX_GPR_LOOP_CARRY", "1", 1) == 0 &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &initial) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &phi) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &next) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &step) &&
      nyr_mach_begin_block(&mach, 0, &entry) && entry == 0 &&
      emit(&mach, NYR_MACH_COPY, vreg(initial), imm(0),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(step), imm(1), (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(1)) &&
      nyr_mach_begin_block(&mach, 1, &preheader) && preheader == 1 &&
      emit(&mach, NYR_MACH_COPY, vreg(phi), vreg(initial),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(2)) &&
      nyr_mach_begin_block(&mach, 2, &header) && header == 2 &&
      emit(&mach, NYR_MACH_ADD, vreg(next), vreg(phi), vreg(step)) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_CALL,
                        .src0 = callee,
                        .effects = NYR_MACH_EFFECT_CALL,
                    }) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(3)) &&
      nyr_mach_begin_block(&mach, 3, &latch) && latch == 3 &&
      emit(&mach, NYR_MACH_COPY, vreg(phi), vreg(next),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(2)) &&
      nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
      nyr_mach_regalloc_build_callee_saved_gpr(&mach, 8, &alloc);

  const nyr_mach_live_segment_t *segment =
      ok ? nyr_mach_regalloc_segment_at(&alloc, phi,
                                        mach.blocks[header].first_inst)
         : NULL;
  ok = ok && segment && !segment->carried;
  if (!ok)
    fprintf(stderr,
            "[regalloc-loop-phi-carry] call-containing loop carried phi "
            "state: segment=%p carried=%d error=%s\n",
            (const void *)segment, segment ? segment->carried : -1,
            err[0] ? err : "call-containing loop carried its phi");

  if (had_prior_env)
    setenv("NYTRIX_GPR_LOOP_CARRY", saved_env, 1);
  else
    unsetenv("NYTRIX_GPR_LOOP_CARRY");
  free(saved_env);
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  if (ok)
    puts(
        "[regalloc-loop-phi-carry] call-containing loop keeps phi home-backed");
  return ok;
}

int main(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t initial, phi, next, step;
  uint32_t entry, preheader, header, latch;
  char err[256] = {0};
  const char *prior_env = getenv("NYTRIX_GPR_LOOP_CARRY");
  char *saved_env = prior_env ? strdup(prior_env) : NULL;
  bool had_prior_env = prior_env != NULL;

  bool ok =
      (!had_prior_env || saved_env) &&
      setenv("NYTRIX_GPR_LOOP_CARRY", "1", 1) == 0 &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &initial) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &phi) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &next) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &step) &&
      nyr_mach_begin_block(&mach, 0, &entry) && entry == 0 &&
      emit(&mach, NYR_MACH_COPY, vreg(initial), imm(0),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(step), imm(1), (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(1)) &&
      nyr_mach_begin_block(&mach, 1, &preheader) && preheader == 1 &&
      emit(&mach, NYR_MACH_COPY, vreg(phi), vreg(initial),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(2)) &&
      nyr_mach_begin_block(&mach, 2, &header) && header == 2 &&
      emit(&mach, NYR_MACH_ADD, vreg(next), vreg(phi), vreg(step)) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(3)) &&
      nyr_mach_begin_block(&mach, 3, &latch) && latch == 3 &&
      emit(&mach, NYR_MACH_COPY, vreg(phi), vreg(next),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(2)) &&
      nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
      nyr_mach_regalloc_build(&mach, 8, &alloc);

  const nyr_mach_live_segment_t *header_phi =
      ok ? nyr_mach_regalloc_segment_at(&alloc, phi,
                                        mach.blocks[header].first_inst)
         : NULL;
  const nyr_mach_live_segment_t *preheader_phi =
      ok ? nyr_mach_regalloc_segment_at(&alloc, phi,
                                        mach.blocks[preheader].first_inst + 1)
         : NULL;
  const nyr_mach_live_segment_t *latch_phi =
      ok ? nyr_mach_regalloc_segment_at(&alloc, phi,
                                        mach.blocks[latch].first_inst)
         : NULL;
  const nyr_mach_live_segment_t *latch_source =
      ok ? nyr_mach_regalloc_segment_at(&alloc, next,
                                        mach.blocks[header].first_inst)
         : NULL;
  const nyr_mach_live_segment_t *preheader_source =
      ok ? nyr_mach_regalloc_segment_at(&alloc, initial,
                                        mach.blocks[entry].first_inst + 2)
         : NULL;
  ok = ok && header_phi && preheader_phi && latch_phi && latch_source &&
       preheader_source && header_phi->carried && preheader_phi->carried &&
       latch_phi->carried && !header_phi->spill && !preheader_phi->spill &&
       !latch_phi->spill && header_phi->color >= 0 &&
       header_phi->color == preheader_phi->color &&
       header_phi->color == latch_phi->color &&
       preheader_source->color == latch_phi->color &&
       latch_source->color == latch_phi->color;
  if (!ok)
    fprintf(stderr,
            "[regalloc-loop-phi-carry] loop incoming copies do not share a "
            "verified color: header=%d/%d preheader=%d/%d latch=%d/%d "
            "pre-source=%d/%d source=%d/%d error=%s\n",
            header_phi ? header_phi->color : -2,
            header_phi ? header_phi->carried : -1,
            preheader_phi ? preheader_phi->color : -2,
            preheader_phi ? preheader_phi->carried : -1,
            latch_phi ? latch_phi->color : -2,
            latch_phi ? latch_phi->carried : -1,
            preheader_source ? preheader_source->color : -2,
            preheader_source ? preheader_source->carried : -1,
            latch_source ? latch_source->color : -2,
            latch_source ? latch_source->carried : -1,
            err[0] ? err : "carry-color contract not satisfied");
  if (!ok) {
    for (size_t i = 0; i < alloc.segment_len; ++i) {
      const nyr_mach_live_segment_t *s = &alloc.segments[i];
      if (s->vreg == phi || s->vreg == initial || s->vreg == next)
        fprintf(stderr,
                "  v%u B%u %zu-%zu preg=%d reload=%d spill=%d carried=%d\n",
                s->vreg, s->block, s->start, s->end, s->color, s->reload,
                s->spill, s->carried);
    }
  } else
    puts("[regalloc-loop-phi-carry] all loop phi edges share one preg");

  if (had_prior_env)
    setenv("NYTRIX_GPR_LOOP_CARRY", saved_env, 1);
  else
    unsetenv("NYTRIX_GPR_LOOP_CARRY");
  free(saved_env);
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  if (ok)
    ok = call_in_loop_disables_carry();
  if (ok)
    ok = preheader_call_keeps_phi_source_out_of_clobbered_color();
  return ok ? 0 : 1;
}
