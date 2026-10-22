/*
 * An unchanged SSA scalar may remain resident through a diamond and join.
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
      mach, (nyr_mach_inst_t){.opcode = opcode,
                              .dst = dst,
                              .src0 = src0,
                              .src1 = src1,
                              .effects = (opcode == NYR_MACH_BR ||
                                          opcode == NYR_MACH_BR_IF ||
                                          opcode == NYR_MACH_RET)
                                             ? NYR_MACH_EFFECT_CONTROL
                                             : NYR_MACH_EFFECT_NONE});
}

static bool rejects_loop_definition_carry(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t seed, value, condition, body_use;
  uint32_t entry, header, body, exit;
  char err[256] = {0};
  bool ok =
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &seed) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &value) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &condition) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &body_use) &&
      nyr_mach_begin_block(&mach, 0, &entry) && entry == 0 &&
      emit(&mach, NYR_MACH_COPY, vreg(seed), imm(7), (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(condition), imm(1),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(1)) &&
      nyr_mach_begin_block(&mach, 1, &header) && header == 1 &&
      emit(&mach, NYR_MACH_COPY, vreg(value), vreg(seed),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR_IF, (nyr_mach_operand_t){0}, vreg(condition),
           block(3)) &&
      nyr_mach_begin_block(&mach, 2, &body) && body == 2 &&
      emit(&mach, NYR_MACH_ADD, vreg(body_use), vreg(value), vreg(seed)) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(1)) &&
      nyr_mach_begin_block(&mach, 3, &exit) && exit == 3 &&
      emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(value),
           (nyr_mach_operand_t){0}) &&
      nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
      nyr_mach_regalloc_build_callee_saved_gpr(&mach, 5, &alloc);
  const nyr_mach_live_segment_t *header_value =
      ok ? nyr_mach_regalloc_segment_at(&alloc, value,
                                        mach.blocks[header].first_inst)
         : NULL;
  const nyr_mach_live_segment_t *body_value =
      ok ? nyr_mach_regalloc_segment_at(&alloc, value,
                                        mach.blocks[body].first_inst)
         : NULL;
  const nyr_mach_live_segment_t *exit_value =
      ok ? nyr_mach_regalloc_segment_at(&alloc, value,
                                        mach.blocks[exit].first_inst)
         : NULL;
  ok = ok && header_value && body_value && exit_value &&
       !header_value->acyclic_carried && !body_value->acyclic_carried &&
       !exit_value->acyclic_carried;
  if (!ok)
    fprintf(stderr,
            "[regalloc-unchanged-dag-carry] loop-defined value entered "
            "acyclic carry: header=%d body=%d exit=%d error=%s\n",
            header_value ? header_value->acyclic_carried : -1,
            body_value ? body_value->acyclic_carried : -1,
            exit_value ? exit_value->acyclic_carried : -1,
            err[0] ? err : "loop-origin exclusion failed");
  else
    puts("[regalloc-unchanged-dag-carry] loop-defined value is excluded");
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok;
}

int main(void) {
  nyr_mach_func_t mach = {0};
  nyr_mach_regalloc_t alloc = {0};
  nyr_mach_reg_t value, condition, one, two, left_result, right_result;
  uint32_t entry, left, right, join;
  char err[256] = {0};
  const char *prior_env = getenv("NYTRIX_GPR_DAG_CARRY");
  char *saved_env = prior_env ? strdup(prior_env) : NULL;
  bool had_prior_env = prior_env != NULL;

  bool ok =
      (!had_prior_env || saved_env) &&
      setenv("NYTRIX_GPR_DAG_CARRY", "1", 1) == 0 &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &value) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &condition) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &one) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &two) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &left_result) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &right_result) &&
      nyr_mach_begin_block(&mach, 0, &entry) && entry == 0 &&
      emit(&mach, NYR_MACH_COPY, vreg(value), imm(42),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(condition), imm(1),
           (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(one), imm(1), (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_COPY, vreg(two), imm(2), (nyr_mach_operand_t){0}) &&
      emit(&mach, NYR_MACH_BR_IF, (nyr_mach_operand_t){0}, vreg(condition),
           block(2)) &&
      nyr_mach_begin_block(&mach, 1, &left) && left == 1 &&
      emit(&mach, NYR_MACH_ADD, vreg(left_result), vreg(value), vreg(one)) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(3)) &&
      nyr_mach_begin_block(&mach, 2, &right) && right == 2 &&
      emit(&mach, NYR_MACH_ADD, vreg(right_result), vreg(value), vreg(two)) &&
      emit(&mach, NYR_MACH_BR, (nyr_mach_operand_t){0}, (nyr_mach_operand_t){0},
           block(3)) &&
      nyr_mach_begin_block(&mach, 3, &join) && join == 3 &&
      emit(&mach, NYR_MACH_RET, (nyr_mach_operand_t){0}, vreg(value),
           (nyr_mach_operand_t){0}) &&
      nyr_mach_verify(&mach, 0, err, sizeof(err)) &&
      nyr_mach_regalloc_build_callee_saved_gpr(&mach, 5, &alloc);

  const nyr_mach_live_segment_t *entry_value =
      ok ? nyr_mach_regalloc_segment_at(&alloc, value,
                                        mach.blocks[entry].first_inst)
         : NULL;
  const nyr_mach_live_segment_t *left_value =
      ok ? nyr_mach_regalloc_segment_at(&alloc, value,
                                        mach.blocks[left].first_inst)
         : NULL;
  const nyr_mach_live_segment_t *right_value =
      ok ? nyr_mach_regalloc_segment_at(&alloc, value,
                                        mach.blocks[right].first_inst)
         : NULL;
  const nyr_mach_live_segment_t *join_value =
      ok ? nyr_mach_regalloc_segment_at(&alloc, value,
                                        mach.blocks[join].first_inst)
         : NULL;
  ok = ok && entry_value && left_value && right_value && join_value &&
       entry_value->color >= 0 && entry_value->color == left_value->color &&
       entry_value->color == right_value->color &&
       entry_value->color == join_value->color && left_value->carried &&
       right_value->carried && join_value->carried &&
       left_value->acyclic_carried && right_value->acyclic_carried &&
       join_value->acyclic_carried && !entry_value->spill &&
       !left_value->spill && !right_value->spill && !join_value->spill &&
       !left_value->reload && !right_value->reload && !join_value->reload;
  if (!ok)
    fprintf(stderr,
            "[regalloc-unchanged-dag-carry] diamond lost resident value: "
            "entry=%p(%d,%d) left=%p(%d,%d,%d,%d) "
            "right=%p(%d,%d,%d,%d) join=%p(%d,%d,%d,%d) error=%s\n",
            (const void *)entry_value, entry_value ? entry_value->color : -2,
            entry_value ? entry_value->spill : -1, (const void *)left_value,
            left_value ? left_value->color : -2,
            left_value ? left_value->reload : -1,
            left_value ? left_value->spill : -1,
            left_value ? left_value->carried : -1, (const void *)right_value,
            right_value ? right_value->color : -2,
            right_value ? right_value->reload : -1,
            right_value ? right_value->spill : -1,
            right_value ? right_value->carried : -1, (const void *)join_value,
            join_value ? join_value->color : -2,
            join_value ? join_value->reload : -1,
            join_value ? join_value->spill : -1,
            join_value ? join_value->carried : -1,
            err[0] ? err : "cross-block carry oracle failed");
  else
    puts("[regalloc-unchanged-dag-carry] diamond stays in one register");
  ok = ok && rejects_loop_definition_carry();

  if (had_prior_env)
    setenv("NYTRIX_GPR_DAG_CARRY", saved_env, 1);
  else
    unsetenv("NYTRIX_GPR_DAG_CARRY");
  free(saved_env);
  nyr_mach_regalloc_free(&alloc);
  nyr_mach_func_free(&mach);
  return ok ? 0 : 1;
}
