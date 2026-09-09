/*
 * Machine-form IR: target-specific intermediate representation
 * between NYIR and object code with register operands and addressing modes.
 */
#include "code/ir/machine.h"
#include "code/ir/internal.h"
#include "code/ir/opt/util.h"
#include "code/native/internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool mach_grow(void **data, size_t *cap, size_t elem_size) {
  size_t next = *cap ? *cap * 2 : 8;
  size_t bytes = 0;
  if (next < *cap || !ny_native_size_mul_ok(next, elem_size, &bytes))
    return false;
  void *grown = realloc(*data, bytes);
  if (!grown)
    return false;
  *data = grown;
  *cap = next;
  return true;
}

void ny_mach_func_free(ny_mach_func_t *func) {
  if (!func)
    return;
  for (size_t i = 0; i < func->inst_len; ++i)
    { free(func->insts[i].args); free(func->insts[i].arg_sizes); }
  free(func->insts);
  free(func->blocks);
  free(func->frame_slots);
  free(func->vreg_classes);
  free(func->vreg_types);
  *func = (ny_mach_func_t){0};
}

static ny_mach_reg_class_t mach_type_reg_class(ny_mach_type_t type) {
  switch (type) {
  case NY_MACH_TYPE_I64:
  case NY_MACH_TYPE_PTR: return NY_MACH_REGCLASS_GPR;
  case NY_MACH_TYPE_F32:
  case NY_MACH_TYPE_F64: return NY_MACH_REGCLASS_FPR;
  case NY_MACH_TYPE_V128_I64:
  case NY_MACH_TYPE_V128_F64:
  case NY_MACH_TYPE_V128_F32:
  case NY_MACH_TYPE_V256_I64: return NY_MACH_REGCLASS_VECTOR;
  case NY_MACH_TYPE_FLAGS: return NY_MACH_REGCLASS_FLAGS;
  default: return NY_MACH_REGCLASS_NONE;
  }
}

static bool mach_is_v128_type(ny_mach_type_t type) {
  return type == NY_MACH_TYPE_V128_I64 || type == NY_MACH_TYPE_V128_F64 ||
         type == NY_MACH_TYPE_V128_F32 || type == NY_MACH_TYPE_V256_I64;
}

static bool mach_type_valid(ny_mach_type_t type) {
  return type > NY_MACH_TYPE_NONE && type <= NY_MACH_TYPE_FLAGS;
}

static int mach_u32_cmp(const void *left, const void *right) {
  uint32_t a = *(const uint32_t *)left;
  uint32_t b = *(const uint32_t *)right;
  return a < b ? -1 : a > b;
}

bool ny_mach_alloc_typed_vreg(ny_mach_func_t *func, ny_mach_type_t type,
                             ny_mach_reg_t *out) {
  ny_mach_reg_class_t reg_class = mach_type_reg_class(type);
  if (!func || !out || reg_class == NY_MACH_REGCLASS_NONE ||
      func->vreg_len >= UINT32_MAX)
    return false;
  if (func->vreg_len == func->vreg_cap) {
    size_t next = func->vreg_cap ? func->vreg_cap * 2 : 8;
    if (next < func->vreg_cap || next > SIZE_MAX / sizeof(*func->vreg_classes))
      return false;
    ny_mach_reg_class_t *classes =
        ny_realloc_array(func->vreg_classes, next, sizeof(*func->vreg_classes));
    if (!classes)
      return false;
    ny_mach_type_t *types = ny_realloc_array(func->vreg_types, next, sizeof(*func->vreg_types));
    if (!types) {
      func->vreg_classes = classes;
      return false;
    }
    func->vreg_classes = classes;
    func->vreg_types = types;
    func->vreg_cap = next;
  }
  *out = (ny_mach_reg_t)func->vreg_len;
  func->vreg_classes[func->vreg_len] = reg_class;
  func->vreg_types[func->vreg_len++] = type;
  return true;
}

bool ny_mach_alloc_vreg(ny_mach_func_t *func, ny_mach_reg_class_t reg_class,
                       ny_mach_reg_t *out) {
  ny_mach_type_t type = NY_MACH_TYPE_NONE;
  switch (reg_class) {
  case NY_MACH_REGCLASS_GPR: type = NY_MACH_TYPE_I64; break;
  case NY_MACH_REGCLASS_FPR: type = NY_MACH_TYPE_F64; break;
  case NY_MACH_REGCLASS_VECTOR: type = NY_MACH_TYPE_V128; break;
  case NY_MACH_REGCLASS_FLAGS: type = NY_MACH_TYPE_FLAGS; break;
  default: break;
  }
  return ny_mach_alloc_typed_vreg(func, type, out);
}

bool ny_mach_alloc_typed_frame_slot(ny_mach_func_t *func, size_t size,
                                   size_t align, ny_mach_type_t type,
                                   bool address_taken, uint32_t *out) {
  if (!func || !out || mach_type_reg_class(type) == NY_MACH_REGCLASS_NONE ||
      size == 0 || align == 0 || (align & (align - 1)) != 0 ||
      func->frame_slot_len >= UINT32_MAX)
    return false;
  if (func->frame_slot_len == func->frame_slot_cap &&
      !mach_grow((void **)&func->frame_slots, &func->frame_slot_cap,
                sizeof(*func->frame_slots)))
    return false;
  *out = (uint32_t)func->frame_slot_len;
  func->frame_slots[func->frame_slot_len++] =
      (ny_mach_frame_slot_t){.size = size, .align = align,
                            .type = type,
                            .address_taken = address_taken};
  return true;
}

bool ny_mach_alloc_frame_slot(ny_mach_func_t *func, size_t size, size_t align,
                             bool address_taken, uint32_t *out) {
  return ny_mach_alloc_typed_frame_slot(func, size, align, NY_MACH_TYPE_I64,
                                       address_taken, out);
}

bool ny_mach_begin_block(ny_mach_func_t *func, uint32_t label, uint32_t *out) {
  if (!func || !out || func->block_len >= UINT32_MAX)
    return false;
  for (size_t i = 0; i < func->block_len; ++i)
    if (func->blocks[i].label == label)
      return false;
  if (func->block_len == func->block_cap &&
      !mach_grow((void **)&func->blocks, &func->block_cap,
                sizeof(*func->blocks)))
    return false;
  *out = (uint32_t)func->block_len;
  func->blocks[func->block_len++] =
      (ny_mach_block_t){.first_inst = (uint32_t)func->inst_len,
                        .inst_count = 0, .label = label,
                        .source_pc = UINT32_MAX};
  return true;
}

bool ny_mach_emit(ny_mach_func_t *func, ny_mach_inst_t inst) {
  if (!func || func->block_len == 0 || func->inst_len >= UINT32_MAX)
    return false;
  if (func->inst_len == func->inst_cap &&
      !mach_grow((void **)&func->insts, &func->inst_cap, sizeof(*func->insts)))
    return false;
  func->insts[func->inst_len++] = inst;
  ++func->blocks[func->block_len - 1].inst_count;
  return true;
}

static bool mach_verify_operand(const ny_mach_func_t *func,
                               const ny_mach_operand_t *operand) {
  if (operand->kind > NY_MACH_OPERAND_SYMBOL ||
      operand->reg_class > NY_MACH_REGCLASS_FLAGS ||
      operand->relocation > NY_MACH_RELOC_CALL)
    return false;
  switch (operand->kind) {
  case NY_MACH_OPERAND_NONE:
    return operand->reg_class == NY_MACH_REGCLASS_NONE &&
           operand->relocation == NY_MACH_RELOC_NONE && operand->addend == 0;
  case NY_MACH_OPERAND_IMM:
    return operand->reg_class == NY_MACH_REGCLASS_NONE &&
           operand->relocation == NY_MACH_RELOC_NONE;
  case NY_MACH_OPERAND_VREG:
    return operand->as.reg < func->vreg_len &&
           func->vreg_classes && func->vreg_types &&
           operand->relocation == NY_MACH_RELOC_NONE && operand->addend == 0 &&
           func->vreg_classes[operand->as.reg] == operand->reg_class &&
           mach_type_reg_class(func->vreg_types[operand->as.reg]) ==
               operand->reg_class;
  case NY_MACH_OPERAND_PREG:
    return operand->reg_class != NY_MACH_REGCLASS_NONE &&
           operand->relocation == NY_MACH_RELOC_NONE && operand->addend == 0;
  case NY_MACH_OPERAND_FRAME:
    return operand->as.frame_index < func->frame_slot_len &&
           operand->reg_class == NY_MACH_REGCLASS_NONE &&
           operand->relocation == NY_MACH_RELOC_NONE && operand->addend == 0;
  case NY_MACH_OPERAND_BLOCK:
    return operand->as.block_index < func->block_len &&
           operand->reg_class == NY_MACH_REGCLASS_NONE &&
           operand->relocation == NY_MACH_RELOC_NONE && operand->addend == 0;
  case NY_MACH_OPERAND_SYMBOL:
    return operand->reg_class == NY_MACH_REGCLASS_NONE && operand->as.symbol &&
           operand->as.symbol[0];
  }
  return false;
}

static bool mach_is_none(const ny_mach_operand_t *operand) {
  return operand && operand->kind == NY_MACH_OPERAND_NONE &&
         operand->reg_class == NY_MACH_REGCLASS_NONE &&
         operand->relocation == NY_MACH_RELOC_NONE;
}

static bool mach_is_value(const ny_mach_operand_t *operand) {
  return operand && (operand->kind == NY_MACH_OPERAND_VREG ||
                     operand->kind == NY_MACH_OPERAND_PREG);
}

static ny_mach_type_t mach_value_type(const ny_mach_func_t *func,
                                    const ny_mach_operand_t *operand) {
  if (!func || !operand || operand->kind != NY_MACH_OPERAND_VREG ||
      operand->as.reg >= func->vreg_len || !func->vreg_types)
    return NY_MACH_TYPE_NONE;
  return func->vreg_types[operand->as.reg];
}

static bool mach_same_value_type(const ny_mach_func_t *func,
                                const ny_mach_operand_t *left,
                                const ny_mach_operand_t *right) {
  if (!mach_is_value(left) || !mach_is_value(right) ||
      left->reg_class != right->reg_class)
    return false;
  /*
   * Physical registers carry a class but not a complete type in machine form.
   * Keep manually constructed target-specific machine form legal while requiring exact
   * types whenever both operands are virtual registers.
   */
  ny_mach_type_t left_type = mach_value_type(func, left);
  ny_mach_type_t right_type = mach_value_type(func, right);
  return left_type == NY_MACH_TYPE_NONE || right_type == NY_MACH_TYPE_NONE ||
         left_type == right_type;
}

static bool mach_is_integer_value(const ny_mach_func_t *func,
                                 const ny_mach_operand_t *operand) {
  if (!mach_is_value(operand) || operand->reg_class != NY_MACH_REGCLASS_GPR)
    return false;
  ny_mach_type_t type = mach_value_type(func, operand);
  return type == NY_MACH_TYPE_NONE || type == NY_MACH_TYPE_I64 ||
         type == NY_MACH_TYPE_PTR;
}

static bool mach_type_is_float(ny_mach_type_t type) {
  return type == NY_MACH_TYPE_F32 || type == NY_MACH_TYPE_F64;
}

static bool mach_is_scalar_value(const ny_mach_func_t *func,
                                const ny_mach_operand_t *operand) {
  ny_mach_type_t type = mach_value_type(func, operand);
  return mach_is_value(operand) &&
         (type == NY_MACH_TYPE_NONE || type == NY_MACH_TYPE_I64 ||
          type == NY_MACH_TYPE_PTR || type == NY_MACH_TYPE_F32 ||
          type == NY_MACH_TYPE_F64);
}

static bool mach_is_v128_value(const ny_mach_func_t *func,
                               const ny_mach_operand_t *operand) {
  ny_mach_type_t type = mach_value_type(func, operand);
  return mach_is_value(operand) &&
         (type == NY_MACH_TYPE_NONE || mach_is_v128_type(type));
}
static bool mach_is_scalar_value_or_frame(const ny_mach_func_t *func,
                                         const ny_mach_operand_t *operand) {
  ny_mach_type_t type = mach_value_type(func, operand);
  if (operand->kind == NY_MACH_OPERAND_VREG) {
    return mach_is_value(operand) &&
           (type == NY_MACH_TYPE_NONE || type == NY_MACH_TYPE_I64 ||
            type == NY_MACH_TYPE_PTR || type == NY_MACH_TYPE_F32 ||
            type == NY_MACH_TYPE_F64);
  }
  if (operand->kind == NY_MACH_OPERAND_FRAME) {
    return operand->as.frame_index < 1024 &&
           (type == NY_MACH_TYPE_NONE || type == NY_MACH_TYPE_I64 ||
            type == NY_MACH_TYPE_PTR || type == NY_MACH_TYPE_F32 ||
            type == NY_MACH_TYPE_F64);
  }
  return false;
}

static bool mach_verify_opcode_shape(const ny_mach_func_t *func,
                                    const ny_mach_inst_t *inst) {
  if (!inst)
    return false;
  switch (inst->opcode) {
  case NY_MACH_NOP:
    return inst->effects == NY_MACH_EFFECT_NONE && mach_is_none(&inst->dst) &&
           mach_is_none(&inst->src0) && mach_is_none(&inst->src1) &&
           mach_is_none(&inst->src2);
  case NY_MACH_COPY: {
    /*
     * Same-type move, IMM materialize, or scalar→V128 broadcast (SET1).
     */
    if (inst->effects != NY_MACH_EFFECT_NONE || !mach_is_value(&inst->dst) ||
        !mach_is_none(&inst->src1) || !mach_is_none(&inst->src2))
      return false;
    if (inst->src0.kind == NY_MACH_OPERAND_IMM)
      return true;
    if (!mach_is_value(&inst->src0))
      return false;
    if (mach_same_value_type(func, &inst->dst, &inst->src0))
      return true;
    ny_mach_type_t dt = mach_value_type(func, &inst->dst);
    ny_mach_type_t st = mach_value_type(func, &inst->src0);
    if (((dt == NY_MACH_TYPE_I64 || dt == NY_MACH_TYPE_PTR) &&
         (st == NY_MACH_TYPE_I64 || st == NY_MACH_TYPE_PTR ||
          st == NY_MACH_TYPE_F64)) ||
        (dt == NY_MACH_TYPE_F64 &&
         (st == NY_MACH_TYPE_I64 || st == NY_MACH_TYPE_PTR)))
      return true;
    if (dt == NY_MACH_TYPE_V128_I64 &&
        (st == NY_MACH_TYPE_I64 || st == NY_MACH_TYPE_NONE))
      return true;
    if (dt == NY_MACH_TYPE_V128_F64 &&
        (st == NY_MACH_TYPE_F64 || st == NY_MACH_TYPE_NONE))
      return true;
    if (dt == NY_MACH_TYPE_V128_F32 &&
        (st == NY_MACH_TYPE_F32 || st == NY_MACH_TYPE_NONE))
      return true;
    return false;
  }
  case NY_MACH_CONVERT:
    return inst->effects == NY_MACH_EFFECT_NONE && mach_is_scalar_value(func, &inst->dst) &&
           mach_is_scalar_value(func, &inst->src0) && mach_is_none(&inst->src1) &&
           mach_is_none(&inst->src2) &&
           !mach_same_value_type(func, &inst->dst, &inst->src0);
  case NY_MACH_SQRT:
  case NY_MACH_SIN:
  case NY_MACH_COS:
    return inst->effects == NY_MACH_EFFECT_NONE && mach_is_scalar_value_or_frame(func, &inst->dst) &&
           mach_is_scalar_value_or_frame(func, &inst->src0) && mach_is_none(&inst->src1) &&
           mach_is_none(&inst->src2);
  case NY_MACH_LOAD:
    return inst->effects == NY_MACH_EFFECT_READ_MEMORY && mach_is_value(&inst->dst) &&
           (inst->src0.kind == NY_MACH_OPERAND_FRAME ||
            mach_is_integer_value(func, &inst->src0)) &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2);
  case NY_MACH_STORE:
    return inst->effects == NY_MACH_EFFECT_WRITE_MEMORY &&
           (inst->dst.kind == NY_MACH_OPERAND_FRAME ||
            mach_is_integer_value(func, &inst->dst)) &&
           mach_is_value(&inst->src0) && mach_is_none(&inst->src1) &&
           mach_is_none(&inst->src2);
  case NY_MACH_LEA:
    return inst->effects == NY_MACH_EFFECT_NONE && mach_is_integer_value(func, &inst->dst) &&
           (inst->src0.kind == NY_MACH_OPERAND_FRAME ||
            inst->src0.kind == NY_MACH_OPERAND_SYMBOL) &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2);
  case NY_MACH_ADD: case NY_MACH_SUB: case NY_MACH_MUL:  case NY_MACH_DIV: {
    if (inst->effects != NY_MACH_EFFECT_NONE || !mach_is_none(&inst->src2))
      return false;
    if (mach_is_v128_value(func, &inst->dst))
      return mach_same_value_type(func, &inst->dst, &inst->src0) &&
             mach_same_value_type(func, &inst->dst, &inst->src1);
    if (!mach_is_scalar_value(func, &inst->dst))
      return false;
    /*
     * Pointer arithmetic: ptr ± i64 → ptr (or i64).
     */
    if ((inst->opcode == NY_MACH_ADD || inst->opcode == NY_MACH_SUB) &&
        mach_is_integer_value(func, &inst->dst) &&
        mach_is_integer_value(func, &inst->src0) &&
        mach_is_integer_value(func, &inst->src1))
      return true;
    /*
     * Integer ops (div, mod, and, or, xor, shl, sar) accept any combination
     * of integer-value types (i64 or ptr) without requiring exact match.
     * Pointer-typed values flow from list.get() etc. into arithmetic.
     */
    if (mach_is_integer_value(func, &inst->dst) &&
        mach_is_integer_value(func, &inst->src0) &&
        mach_is_integer_value(func, &inst->src1))
      return true;
    return mach_same_value_type(func, &inst->dst, &inst->src0) &&
           mach_same_value_type(func, &inst->dst, &inst->src1);
  }
  case NY_MACH_MOD: case NY_MACH_AND: case NY_MACH_OR: case NY_MACH_XOR:
  case NY_MACH_SHL: case NY_MACH_SAR: case NY_MACH_ROR:
  case NY_MACH_ROR32:
    if (inst->effects != NY_MACH_EFFECT_NONE || !mach_is_none(&inst->src2))
      return false;
    if (inst->opcode == NY_MACH_ROR32)
      return mach_value_type(func, &inst->dst) == NY_MACH_TYPE_I64 &&
             mach_value_type(func, &inst->src0) == NY_MACH_TYPE_I64 &&
             mach_value_type(func, &inst->src1) == NY_MACH_TYPE_I64;
    if (mach_is_v128_value(func, &inst->dst))
      return mach_value_type(func, &inst->dst) == NY_MACH_TYPE_V128_I64 &&
             mach_same_value_type(func, &inst->dst, &inst->src0) &&
             (inst->src1.kind == NY_MACH_OPERAND_IMM ||
              mach_same_value_type(func, &inst->dst, &inst->src1) ||
              mach_is_integer_value(func, &inst->src1));
    /*
     * Integer-value mixing for MOD/AND/OR/XOR/SHL/SAR (same as DIV above).
     */
    if (mach_is_integer_value(func, &inst->dst) &&
        mach_is_integer_value(func, &inst->src0) &&
        mach_is_integer_value(func, &inst->src1))
      return true;
    return mach_same_value_type(func, &inst->dst, &inst->src0) &&
           mach_same_value_type(func, &inst->dst, &inst->src1);
  case NY_MACH_FMA:
    return inst->effects == NY_MACH_EFFECT_NONE &&
           (mach_is_scalar_value(func, &inst->dst) ||
            mach_is_v128_value(func, &inst->dst)) &&
           mach_same_value_type(func, &inst->dst, &inst->src0) &&
           mach_same_value_type(func, &inst->dst, &inst->src1) &&
           mach_same_value_type(func, &inst->dst, &inst->src2);
  case NY_MACH_SHUFFLE:
    return inst->effects == NY_MACH_EFFECT_NONE &&
           mach_is_v128_value(func, &inst->dst) &&
           mach_same_value_type(func, &inst->dst, &inst->src0) &&
           inst->src1.kind == NY_MACH_OPERAND_IMM && mach_is_none(&inst->src2);
  case NY_MACH_SELECT:
    return inst->effects == NY_MACH_EFFECT_NONE &&
           inst->condition == NY_MACH_COND_NE &&
           mach_value_type(func, &inst->dst) == NY_MACH_TYPE_I64 &&
           mach_value_type(func, &inst->src0) == NY_MACH_TYPE_I64 &&
           mach_value_type(func, &inst->src1) == NY_MACH_TYPE_I64 &&
           mach_value_type(func, &inst->src2) == NY_MACH_TYPE_I64;
  case NY_MACH_CMP:
    return inst->effects == NY_MACH_EFFECT_NONE && mach_is_integer_value(func, &inst->dst) &&
           mach_is_scalar_value(func, &inst->src0) &&
           (mach_same_value_type(func, &inst->src0, &inst->src1) ||
            (mach_is_integer_value(func, &inst->src0) &&
             mach_is_integer_value(func, &inst->src1))) &&
           mach_is_none(&inst->src2);
  case NY_MACH_CALL:
    return inst->effects == NY_MACH_EFFECT_CALL &&
           (mach_is_none(&inst->dst) || mach_is_value(&inst->dst)) &&
           (inst->src0.kind == NY_MACH_OPERAND_SYMBOL || mach_is_value(&inst->src0)) &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2);
  case NY_MACH_RET:
    return inst->effects == NY_MACH_EFFECT_CONTROL && mach_is_none(&inst->dst) &&
           (mach_is_none(&inst->src0) || mach_is_value(&inst->src0)) &&
           mach_is_none(&inst->src1) && mach_is_none(&inst->src2);
  case NY_MACH_BR:
    return inst->effects == NY_MACH_EFFECT_CONTROL && mach_is_none(&inst->dst) &&
           mach_is_none(&inst->src0) && inst->src1.kind == NY_MACH_OPERAND_BLOCK &&
           mach_is_none(&inst->src2);
  case NY_MACH_BR_IF:
    return inst->effects == NY_MACH_EFFECT_CONTROL && mach_is_none(&inst->dst) &&
           mach_is_integer_value(func, &inst->src0) &&
           inst->src1.kind == NY_MACH_OPERAND_BLOCK &&
           mach_is_none(&inst->src2);
  case NY_MACH_TRAP:
    return inst->effects == NY_MACH_EFFECT_CONTROL && mach_is_none(&inst->dst) &&
           mach_is_none(&inst->src0) && mach_is_none(&inst->src1) &&
           mach_is_none(&inst->src2);
  case NY_MACH_INLINE_ASM:
    return (inst->effects & NY_MACH_EFFECT_CALL) != 0 &&
           inst->src0.kind == NY_MACH_OPERAND_SYMBOL &&
           (mach_is_none(&inst->dst) || mach_is_value(&inst->dst));
  case NY_MACH_REDUCE_ADD: {
    ny_mach_type_t dt = mach_value_type(func, &inst->dst);
    ny_mach_type_t at = mach_value_type(func, &inst->src0);
    ny_mach_type_t vt = mach_value_type(func, &inst->src1);
    bool integer_reduce = mach_is_integer_value(func, &inst->dst) &&
                          mach_is_integer_value(func, &inst->src0) &&
                          mach_is_v128_value(func, &inst->src1) &&
                          (vt == NY_MACH_TYPE_NONE ||
                           vt == NY_MACH_TYPE_V128_I64 ||
                           vt == NY_MACH_TYPE_V256_I64);
    bool f64_reduce = mach_is_value(&inst->dst) &&
                      mach_is_value(&inst->src0) &&
                      mach_is_v128_value(func, &inst->src1) &&
                      (dt == NY_MACH_TYPE_NONE || dt == NY_MACH_TYPE_F64) &&
                      (at == NY_MACH_TYPE_NONE || at == NY_MACH_TYPE_F64) &&
                      (vt == NY_MACH_TYPE_NONE || vt == NY_MACH_TYPE_V128_F64);
    return inst->effects == NY_MACH_EFFECT_NONE &&
           (integer_reduce || f64_reduce) && mach_is_none(&inst->src2);
  }
  case NY_MACH_INTRINSIC:
    /*
     * (1) named call-like: src0=symbol + CALL effect
     * (2) CAPTURE_RET: dst=vreg, src0=IMM selector
     * (3) COPY_STRUCT: src0/src1=ptr vregs, src2=IMM size, memory effects
     */
    if (inst->src0.kind == NY_MACH_OPERAND_SYMBOL)
      return (inst->effects & NY_MACH_EFFECT_CALL) != 0 &&
             (mach_is_none(&inst->dst) || mach_is_value(&inst->dst));
    if (inst->src0.kind == NY_MACH_OPERAND_IMM && mach_is_value(&inst->dst) &&
        mach_is_none(&inst->src1) && mach_is_none(&inst->src2))
      return true; /* CAPTURE_RET */
    if (mach_is_value(&inst->src0) && mach_is_value(&inst->src1) &&
        inst->src2.kind == NY_MACH_OPERAND_IMM)
      return true; /* COPY_STRUCT */
    return false;
  default:
    return false;
  }
}

const char *ny_mach_opcode_name(ny_mach_opcode_t op) {
  switch (op) {
  case NY_MACH_NOP:        return "nop";
  case NY_MACH_COPY:       return "copy";
  case NY_MACH_CONVERT:    return "convert";
  case NY_MACH_LOAD:       return "load";
  case NY_MACH_STORE:      return "store";
  case NY_MACH_LEA:        return "lea";
  case NY_MACH_ADD:        return "add";
  case NY_MACH_SUB:        return "sub";
  case NY_MACH_MUL:        return "mul";
  case NY_MACH_DIV:        return "div";
  case NY_MACH_MOD:        return "mod";
  case NY_MACH_AND:        return "and";
  case NY_MACH_OR:         return "or";
  case NY_MACH_XOR:        return "xor";
  case NY_MACH_SHL:        return "shl";
  case NY_MACH_SAR:        return "sar";
  case NY_MACH_ROR:        return "ror";
  case NY_MACH_ROR32:      return "ror32";
  case NY_MACH_FMA:        return "fma";
  case NY_MACH_SQRT:       return "sqrt";
  case NY_MACH_SIN:        return "sin";
  case NY_MACH_COS:        return "cos";
  case NY_MACH_SHUFFLE:    return "shuffle";
  case NY_MACH_SELECT:     return "select";
  case NY_MACH_CMP:        return "cmp";
  case NY_MACH_CALL:       return "call";
  case NY_MACH_RET:        return "ret";
  case NY_MACH_BR:         return "br";
  case NY_MACH_BR_IF:      return "br_if";
  case NY_MACH_TRAP:       return "trap";
  case NY_MACH_INLINE_ASM: return "inline_asm";
  case NY_MACH_INTRINSIC:  return "intrinsic";
  case NY_MACH_REDUCE_ADD: return "reduce_add";
  }
  return "???";
}

static const char *mach_type_name(ny_mach_type_t t) {
  switch (t) {
  case NY_MACH_TYPE_NONE:    return "none";
  case NY_MACH_TYPE_I64:     return "i64";
  case NY_MACH_TYPE_F32:     return "f32";
  case NY_MACH_TYPE_F64:     return "f64";
  case NY_MACH_TYPE_PTR:     return "ptr";
  case NY_MACH_TYPE_V128_I64: return "v128.i64";
  case NY_MACH_TYPE_V256_I64: return "v256.i64";
  case NY_MACH_TYPE_V128_F64: return "v128.f64";
  case NY_MACH_TYPE_V128_F32: return "v128.f32";
  case NY_MACH_TYPE_FLAGS:   return "flags";
  }
  return "???";
}

static const char *mach_cond_name(ny_mach_cond_t c) {
  switch (c) {
  case NY_MACH_COND_ALWAYS: return "";
  case NY_MACH_COND_EQ:     return ".eq";
  case NY_MACH_COND_NE:     return ".ne";
  case NY_MACH_COND_LT:     return ".lt";
  case NY_MACH_COND_LE:     return ".le";
  case NY_MACH_COND_GT:     return ".gt";
  case NY_MACH_COND_GE:     return ".ge";
  case NY_MACH_COND_B:      return ".b";
  case NY_MACH_COND_AE:     return ".ae";
  case NY_MACH_COND_BE:     return ".be";
  case NY_MACH_COND_A:      return ".a";
  }
  return ".???";
}

static void mach_dump_operand(const ny_mach_operand_t *op, FILE *out) {
  if (!out) return;
  switch (op->kind) {
  case NY_MACH_OPERAND_VREG:
    fprintf(out, "v%u", op->as.reg);
    break;
  case NY_MACH_OPERAND_PREG:
    fprintf(out, "p%u", op->as.reg);
    break;
  case NY_MACH_OPERAND_IMM:
    fprintf(out, "#%ld", (long)op->as.imm);
    break;
  case NY_MACH_OPERAND_FRAME:
    fprintf(out, "[fp+%u]", op->as.frame_index);
    break;
  case NY_MACH_OPERAND_BLOCK:
    fprintf(out, "BB%u", op->as.block_index);
    break;
  case NY_MACH_OPERAND_SYMBOL:
    fprintf(out, "%s", op->as.symbol ? op->as.symbol : "(null)");
    break;
  case NY_MACH_OPERAND_NONE:
  default:
    fprintf(out, "_");
    break;
  }
  if (op->relocation != NY_MACH_RELOC_NONE) {
    const char *rname = "reloc";
    switch (op->relocation) {
    case NY_MACH_RELOC_ABS64:    rname = "abs64"; break;
    case NY_MACH_RELOC_PC_REL32: rname = "pcrel32"; break;
    case NY_MACH_RELOC_CALL:     rname = "call"; break;
    default: break;
    }
    fprintf(out, "(%s+%ld)", rname, (long)op->addend);
  }
}

void ny_mach_dump(const ny_mach_func_t *func, FILE *out) {
  if (!func || !out)
    return;
  fprintf(out, "  vregs: %zu  frame_slots: %zu  blocks: %zu  insts: %zu\n",
          func->vreg_len, func->frame_slot_len, func->block_len, func->inst_len);
  for (size_t i = 0; i < func->vreg_len; i++) {
    fprintf(out, "  v%zu: %s %s\n", i,
            mach_type_name(func->vreg_types[i]),
            func->vreg_classes[i] == NY_MACH_REGCLASS_GPR ? "gpr" :
            func->vreg_classes[i] == NY_MACH_REGCLASS_FPR ? "fpr" :
            func->vreg_classes[i] == NY_MACH_REGCLASS_VECTOR ? "vec" :
            func->vreg_classes[i] == NY_MACH_REGCLASS_FLAGS ? "flg" : "???");
  }
  for (size_t s = 0; s < func->frame_slot_len; s++) {
    const ny_mach_frame_slot_t *slot = &func->frame_slots[s];
    fprintf(out, "  slot%zu: %s align=%zu%s\n", s,
            mach_type_name(slot->type), slot->align,
            slot->address_taken ? " addr_taken" : "");
  }
  for (size_t b = 0; b < func->block_len; b++) {
    const ny_mach_block_t *blk = &func->blocks[b];
    fprintf(out, "  BB%u:\n", blk->label);
    for (uint32_t ii = 0; ii < blk->inst_count; ii++) {
      uint32_t idx = blk->first_inst + ii;
      if (idx >= func->inst_len)
        break;
      const ny_mach_inst_t *inst = &func->insts[idx];
      fprintf(out, "    %s%s", ny_mach_opcode_name(inst->opcode),
              mach_cond_name(inst->condition));
      if (inst->source_pc != UINT32_MAX)
        fprintf(out, "  [nir=%u]", inst->source_pc);
      if (mach_is_value(&inst->dst)) {
        fprintf(out, "  ");
        mach_dump_operand(&inst->dst, out);
      }
      if (mach_is_value(&inst->src0) || inst->src0.kind == NY_MACH_OPERAND_IMM ||
          inst->src0.kind == NY_MACH_OPERAND_FRAME ||
          inst->src0.kind == NY_MACH_OPERAND_SYMBOL ||
          inst->src0.kind == NY_MACH_OPERAND_BLOCK) {
        fprintf(out, ", ");
        mach_dump_operand(&inst->src0, out);
      }
      if (mach_is_value(&inst->src1) || inst->src1.kind == NY_MACH_OPERAND_IMM ||
          inst->src1.kind == NY_MACH_OPERAND_BLOCK) {
        fprintf(out, ", ");
        mach_dump_operand(&inst->src1, out);
      }
      if (inst->src2.kind != NY_MACH_OPERAND_NONE) {
        fprintf(out, ", ");
        mach_dump_operand(&inst->src2, out);
      }
      if (inst->opcode == NY_MACH_CALL && inst->args_len > 1) {
        fprintf(out, " [");
        for (size_t a = 1; a < inst->args_len; a++) {
          if (a > 1) fprintf(out, ", ");
          mach_dump_operand(&inst->args[a], out);
        }
        fprintf(out, "]");
      }
      if (inst->opcode == NY_MACH_CALL && inst->call_clobbers)
        fprintf(out, "  clobbers=0x%lx", (unsigned long)inst->call_clobbers);
      fprintf(out, "\n");
    }
  }
}

bool ny_mach_verify(const ny_mach_func_t *func, unsigned caps, char *err, size_t err_len) {
  (void)caps;
  if (!func)
    return nyir_err(err, err_len, "machine IR verify: missing function");
  if ((func->inst_len && !func->insts) || (func->block_len && !func->blocks) ||
      (func->frame_slot_len && !func->frame_slots))
    return nyir_err(err, err_len,
                   "machine IR verify: instruction, block, or frame storage is missing");
  if (func->vreg_len && (!func->vreg_classes || !func->vreg_types))
    return nyir_err(err, err_len,
                   "machine IR verify: virtual-register metadata is missing");
  for (size_t reg = 0; reg < func->vreg_len; ++reg) {
    if (!mach_type_valid(func->vreg_types[reg]) ||
        mach_type_reg_class(func->vreg_types[reg]) != func->vreg_classes[reg])
      return nyir_err(err, err_len,
                     "machine IR verify: virtual register %zu has invalid type metadata",
                     reg);
  }
  for (size_t slot = 0; slot < func->frame_slot_len; ++slot) {
    const ny_mach_frame_slot_t *frame = &func->frame_slots[slot];
    if (frame->size == 0 || frame->align == 0 ||
        (frame->align & (frame->align - 1)) != 0 || !mach_type_valid(frame->type))
      return nyir_err(err, err_len,
                     "machine IR verify: frame slot %zu has invalid layout", slot);
  }
  if (func->block_len) {
    if (func->block_len > SIZE_MAX / sizeof(uint32_t))
      return nyir_err(err, err_len, "machine IR verify: block label table is too large");
    uint32_t *labels = ny_malloc_array(func->block_len, sizeof(*labels));
    if (!labels)
      return nyir_err(err, err_len, "machine IR verify: out of memory");
    for (size_t block = 0; block < func->block_len; ++block)
      labels[block] = func->blocks[block].label;
    qsort(labels, func->block_len, sizeof(*labels), mach_u32_cmp);
    for (size_t block = 1; block < func->block_len; ++block) {
      if (labels[block - 1] == labels[block]) {
        uint32_t duplicate = labels[block];
        free(labels);
        return nyir_err(err, err_len,
                       "machine IR verify: duplicate block label %u", duplicate);
      }
    }
    free(labels);
  }
  size_t next_inst = 0;
  for (size_t block = 0; block < func->block_len; ++block) {
    const ny_mach_block_t *current = &func->blocks[block];
    if (current->first_inst != next_inst ||
        current->inst_count > func->inst_len - next_inst)
      return nyir_err(err, err_len,
                     "machine IR verify: block %zu instruction range is invalid",
                     block);
    next_inst += current->inst_count;
  }
  if (next_inst != func->inst_len)
    return nyir_err(err, err_len,
                   "machine IR verify: instructions are outside blocks");
  for (size_t i = 0; i < func->inst_len; ++i) {
    const ny_mach_inst_t *inst = &func->insts[i];
    if (inst->opcode > NY_MACH_REDUCE_ADD ||
        inst->condition > NY_MACH_COND_GE ||
        (inst->effects & ~(NY_MACH_EFFECT_READ_MEMORY | NY_MACH_EFFECT_WRITE_MEMORY |
                           NY_MACH_EFFECT_CALL | NY_MACH_EFFECT_CONTROL)) != 0 ||
        !mach_verify_operand(func, &inst->dst) ||
        !mach_verify_operand(func, &inst->src0) ||
        !mach_verify_operand(func, &inst->src1) ||
        !mach_verify_operand(func, &inst->src2))
      return nyir_err(err, err_len,
                     "machine IR verify: instruction %zu is malformed", i);
    if (inst->opcode != NY_MACH_CALL &&
        (inst->args_len != 0 || inst->arg_sizes != NULL || inst->call_is_extern ||
         inst->call_has_sret ||
         inst->call_clobbers != 0))
      return nyir_err(err, err_len,
                     "machine IR verify: non-call instruction %zu has arguments", i);
    if (inst->args_len && !inst->args)
      return nyir_err(err, err_len,
                       "machine IR verify: call instruction %zu has no argument storage", i);
    if (inst->arg_sizes && !inst->args)
      return nyir_err(err, err_len,
                       "machine IR verify: call instruction %zu has ABI metadata without arguments", i);
    if (inst->opcode == NY_MACH_CMP) {
      if (inst->condition == NY_MACH_COND_ALWAYS)
        return nyir_err(err, err_len,
                       "machine IR verify: comparison instruction %zu has no predicate", i);
    } else if (inst->opcode == NY_MACH_SELECT) {
      if (inst->condition != NY_MACH_COND_NE)
        return nyir_err(err, err_len,
                       "machine IR verify: select instruction %zu must test nonzero", i);
    } else if (inst->condition != NY_MACH_COND_ALWAYS) {
      return nyir_err(err, err_len,
                     "machine IR verify: non-comparison instruction %zu has a predicate", i);
    }
    for (size_t arg = 0; arg < inst->args_len; ++arg)
      if (!mach_verify_operand(func, &inst->args[arg]) ||
          !mach_is_value(&inst->args[arg]))
        return nyir_err(err, err_len,
                       "machine IR verify: call instruction %zu has an invalid argument", i);
    if (!mach_verify_opcode_shape(func, inst))
      return nyir_err(err, err_len,
                     "machine IR verify: instruction %zu (%s) has an invalid opcode shape"
                     " [dst=%s src0=%s src1=%s]",
                     i, ny_mach_opcode_name(inst->opcode),
                     mach_type_name(mach_value_type(func, &inst->dst)),
                     mach_type_name(mach_value_type(func, &inst->src0)),
                     mach_type_name(mach_value_type(func, &inst->src1)));
  }
  for (size_t block = 0; block < func->block_len; ++block) {
    const ny_mach_block_t *current = &func->blocks[block];
    for (size_t n = 0; n < current->inst_count; ++n) {
      const ny_mach_inst_t *inst = &func->insts[current->first_inst + n];
      size_t remaining = current->inst_count - n - 1;
      if ((inst->opcode == NY_MACH_RET || inst->opcode == NY_MACH_BR ||
           inst->opcode == NY_MACH_TRAP) && remaining)
        return nyir_err(err, err_len,
                       "machine IR verify: block %zu has instructions after a terminator",
                       block);
      /*
       * BR_IF transfers only its taken edge; following instructions are its
       * legal fallthrough.  Bounds checks deliberately use that form before
       * the rest of the source CFG block.
       */
    }
  }
  if (err && err_len)
    err[0] = '\0';
  return true;
}

static ny_mach_operand_t mach_vreg(const ny_mach_func_t *func, ny_mach_reg_t reg) {
  return (ny_mach_operand_t){.kind = NY_MACH_OPERAND_VREG,
                            .reg_class = func->vreg_classes[reg],
                            .as.reg = reg};
}

static ny_mach_operand_t mach_frame(uint32_t index) {
  return (ny_mach_operand_t){.kind = NY_MACH_OPERAND_FRAME,
                            .as.frame_index = index};
}

typedef struct {
  int64_t label;
  uint32_t block;
} mach_label_block_t;

static int mach_label_block_cmp(const void *left, const void *right) {
  const mach_label_block_t *a = left;
  const mach_label_block_t *b = right;
  return a->label < b->label ? -1 : a->label > b->label;
}

static int mach_find_nir_block(const mach_label_block_t *labels, size_t len,
                              int64_t label) {
  size_t low = 0;
  size_t high = len;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (labels[mid].label == label)
      return (int)labels[mid].block;
    if (labels[mid].label < label)
      low = mid + 1;
    else
      high = mid;
  }
  return -1;
}


typedef struct {
  uint32_t pred;
  uint32_t succ;
  uint32_t block;
  ny_mach_reg_t *dst;
  ny_mach_reg_t *src;
  size_t len;
  bool explicit_branch;
} mach_phi_edge_t;

static void mach_phi_edges_free(mach_phi_edge_t *edges, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    free(edges[i].dst);
    free(edges[i].src);
  }
  free(edges);
}

static mach_phi_edge_t *mach_phi_edge_find(mach_phi_edge_t *edges, size_t len,
                                           uint32_t pred, uint32_t succ) {
  for (size_t i = 0; i < len; ++i)
    if (edges[i].pred == pred && edges[i].succ == succ)
      return &edges[i];
  return NULL;
}

static bool mach_emit_parallel_copies(ny_mach_func_t *out,
                                      const mach_phi_edge_t *edge) {
  if (!edge || edge->len == 0)
    return true;
  ny_mach_reg_t *dst = ny_malloc_array(edge->len, sizeof(*dst));
  ny_mach_reg_t *src = ny_malloc_array(edge->len, sizeof(*src));
  bool *pending = ny_calloc_array(edge->len, sizeof(*pending));
  if (!dst || !src || !pending) {
    free(dst); free(src); free(pending);
    return false;
  }
  memcpy(dst, edge->dst, edge->len * sizeof(*dst));
  memcpy(src, edge->src, edge->len * sizeof(*src));
  size_t left = 0;
  /*
   * Preserve an identity PHI copy. It encodes as a no-op once source and
   * destination receive the same preg, but it is the explicit entry-edge
   * seed required by liveness and loop-carried register allocation.
   */
  for (size_t i = 0; i < edge->len; ++i) {
    pending[i] = true;
    ++left;
  }
  while (left) {
    bool progressed = false;
    for (size_t i = 0; i < edge->len; ++i) {
      if (!pending[i])
        continue;
      bool dst_is_source = false;
      for (size_t j = 0; j < edge->len; ++j) {
        if (i != j && pending[j] && src[j] == dst[i]) {
          dst_is_source = true;
          break;
        }
      }
      if (dst_is_source)
        continue;
      ny_mach_type_t dt = out->vreg_types[dst[i]];
      ny_mach_type_t st = out->vreg_types[src[i]];
      bool bitwise64 =
          (dt == NY_MACH_TYPE_I64 || dt == NY_MACH_TYPE_PTR ||
           dt == NY_MACH_TYPE_F64) &&
          (st == NY_MACH_TYPE_I64 || st == NY_MACH_TYPE_PTR ||
           st == NY_MACH_TYPE_F64);
      ny_mach_inst_t copy = {
          .opcode = (dt == st || bitwise64) ? NY_MACH_COPY : NY_MACH_CONVERT,
          .source_pc = UINT32_MAX,
          .dst = mach_vreg(out, dst[i]),
          .src0 = mach_vreg(out, src[i])};
      if (!ny_mach_emit(out, copy)) {
        free(dst); free(src); free(pending);
        return false;
      }
      pending[i] = false;
      --left;
      progressed = true;
    }
    if (progressed)
      continue;

    size_t cycle = 0;
    while (cycle < edge->len && !pending[cycle])
      ++cycle;
    if (cycle == edge->len)
      break;
    ny_mach_reg_t old_src = src[cycle];
    ny_mach_reg_t temp = NY_MACH_REG_INVALID;
    if (old_src >= out->vreg_len ||
        !ny_mach_alloc_typed_vreg(out, out->vreg_types[old_src], &temp)) {
      free(dst); free(src); free(pending);
      return false;
    }
    ny_mach_inst_t save = {.opcode = NY_MACH_COPY,
                           .source_pc = UINT32_MAX,
                           .dst = mach_vreg(out, temp),
                           .src0 = mach_vreg(out, old_src)};
    if (!ny_mach_emit(out, save)) {
      free(dst); free(src); free(pending);
      return false;
    }
    for (size_t i = 0; i < edge->len; ++i)
      if (pending[i] && src[i] == old_src)
        src[i] = temp;
  }
  free(dst); free(src); free(pending);
  return true;
}

static bool mach_copy_metadata(const ny_mach_func_t *src, ny_mach_func_t *dst) {
  dst->param_count = src->param_count;
  if (src->frame_slot_len) {
    dst->frame_slots = ny_malloc_array(src->frame_slot_len, sizeof(*dst->frame_slots));
    if (!dst->frame_slots)
      return false;
    memcpy(dst->frame_slots, src->frame_slots,
           src->frame_slot_len * sizeof(*dst->frame_slots));
    dst->frame_slot_len = dst->frame_slot_cap = src->frame_slot_len;
  }
  if (src->vreg_len) {
    dst->vreg_classes = ny_malloc_array(src->vreg_len, sizeof(*dst->vreg_classes));
    dst->vreg_types = ny_malloc_array(src->vreg_len, sizeof(*dst->vreg_types));
    if (!dst->vreg_classes || !dst->vreg_types)
      return false;
    memcpy(dst->vreg_classes, src->vreg_classes,
           src->vreg_len * sizeof(*dst->vreg_classes));
    memcpy(dst->vreg_types, src->vreg_types,
           src->vreg_len * sizeof(*dst->vreg_types));
    dst->vreg_len = dst->vreg_cap = src->vreg_len;
  }
  return true;
}

static bool mach_lower_phi_edges(const nyir_func_t *nyir,
                                 const nyir_cfg_t *cfg,
                                 const ny_mach_reg_t *values,
                                 ny_mach_func_t *base, char *err,
                                 size_t err_len) {
  size_t edge_count = 0;
  for (size_t succ = 0; succ < cfg->block_count; ++succ) {
    size_t at = cfg->block_start[succ];
    if (at < cfg->block_end[succ] && nyir->data[at].op == NYIR_LABEL)
      ++at;
    if (at >= cfg->block_end[succ] || nyir->data[at].op != NYIR_PHI)
      continue;
    edge_count += cfg->pred_offsets[succ + 1] - cfg->pred_offsets[succ];
  }
  if (!edge_count)
    return true;
  mach_phi_edge_t *edges = ny_calloc_array(edge_count, sizeof(*edges));
  if (!edges)
    return nyir_err(err, err_len, "machine lower: out of memory for PHI edges");

  size_t ei = 0;
  for (size_t succ = 0; succ < cfg->block_count; ++succ) {
    size_t first = cfg->block_start[succ];
    if (first < cfg->block_end[succ] && nyir->data[first].op == NYIR_LABEL)
      ++first;
    size_t phi_count = 0;
    while (first + phi_count < cfg->block_end[succ] &&
           nyir->data[first + phi_count].op == NYIR_PHI)
      ++phi_count;
    if (!phi_count)
      continue;
    for (size_t pi = cfg->pred_offsets[succ]; pi < cfg->pred_offsets[succ + 1]; ++pi) {
      size_t pred = cfg->pred_blocks[pi];
      mach_phi_edge_t *edge = &edges[ei];
      edge->pred = (uint32_t)pred;
      edge->succ = (uint32_t)succ;
      edge->block = (uint32_t)(base->block_len + ei);
      edge->len = phi_count;
      edge->dst = ny_malloc_array(phi_count, sizeof(*edge->dst));
      edge->src = ny_malloc_array(phi_count, sizeof(*edge->src));
      if (!edge->dst || !edge->src) {
        mach_phi_edges_free(edges, edge_count);
        return nyir_err(err, err_len, "machine lower: out of memory for PHI copies");
      }
      int64_t pred_label = cfg->block_label[pred];
      for (size_t k = 0; k < phi_count; ++k) {
        const nyir_inst_t *phi = &nyir->data[first + k];
        int incoming = -1;
        for (size_t n = 0; n < phi->phi_incoming_len; ++n) {
          if (phi->phi_incoming[n].predecessor_label == pred_label) {
            incoming = phi->phi_incoming[n].value;
            break;
          }
        }
        if (incoming < 0 || phi->dst < 0) {
          mach_phi_edges_free(edges, edge_count);
          return nyir_err(err, err_len,
                            "machine lower: PHI incoming edge is missing");
        }
        edge->dst[k] = values[phi->dst];
        edge->src[k] = values[incoming];
      }
      ++ei;
    }
  }

  ny_mach_func_t rebuilt = {0};
  if (!mach_copy_metadata(base, &rebuilt))
    goto oom;
  for (size_t b = 0; b < base->block_len; ++b) {
    uint32_t emitted = 0;
    if (!ny_mach_begin_block(&rebuilt, base->blocks[b].label, &emitted) || emitted != b)
      goto oom;
    rebuilt.blocks[emitted].source_pc = base->blocks[b].source_pc;
    const ny_mach_block_t *block = &base->blocks[b];
    for (size_t n = 0; n < block->inst_count; ++n) {
      size_t index = block->first_inst + n;
      ny_mach_inst_t inst = base->insts[index];
      if ((inst.opcode == NY_MACH_BR || inst.opcode == NY_MACH_BR_IF) &&
          inst.src1.kind == NY_MACH_OPERAND_BLOCK) {
        uint32_t target = inst.src1.as.block_index;
        mach_phi_edge_t *edge = mach_phi_edge_find(edges, edge_count,
                                                   (uint32_t)b, target);
        if (edge) {
          inst.src1.as.block_index = edge->block;
          edge->explicit_branch = true;
        }
      }
      if (!ny_mach_emit(&rebuilt, inst))
        goto oom;
      base->insts[index].args = NULL;
      base->insts[index].arg_sizes = NULL;
      base->insts[index].args_len = 0;
    }
    mach_phi_edge_t *implicit = NULL;
    for (size_t e = 0; e < edge_count; ++e) {
      if (edges[e].pred != b || edges[e].explicit_branch)
        continue;
      if (implicit) {
        nyir_err(err, err_len,
                   "machine lower: multiple implicit PHI successors");
        goto fail;
      }
      implicit = &edges[e];
    }
    if (implicit) {
      ny_mach_inst_t br = {
          .opcode = NY_MACH_BR,
          .source_pc = UINT32_MAX,
          .src1 = {.kind = NY_MACH_OPERAND_BLOCK,
                   .as.block_index = implicit->block},
          .effects = NY_MACH_EFFECT_CONTROL};
      if (!ny_mach_emit(&rebuilt, br))
        goto oom;
    }
  }
  for (size_t e = 0; e < edge_count; ++e) {
    uint32_t emitted = 0;
    if (!ny_mach_begin_block(&rebuilt, edges[e].block, &emitted) ||
        emitted != edges[e].block || !mach_emit_parallel_copies(&rebuilt, &edges[e]))
      goto oom;
    ny_mach_inst_t br = {
        .opcode = NY_MACH_BR,
        .source_pc = UINT32_MAX,
        .src1 = {.kind = NY_MACH_OPERAND_BLOCK,
                 .as.block_index = edges[e].succ},
        .effects = NY_MACH_EFFECT_CONTROL};
    if (!ny_mach_emit(&rebuilt, br))
      goto oom;
  }
  mach_phi_edges_free(edges, edge_count);
  ny_mach_func_free(base);
  *base = rebuilt;
  return true;

oom:
  nyir_err(err, err_len, "machine lower: out of memory lowering PHI edges");
fail:
  mach_phi_edges_free(edges, edge_count);
  ny_mach_func_free(&rebuilt);
  return false;
}

static bool mach_opcode_from_nir(nyir_op_t op, ny_mach_opcode_t *out) {
  if (!out)
    return false;
  switch (op) {
  case NYIR_COPY: *out = NY_MACH_COPY; return true;
  case NYIR_ADD_I64: *out = NY_MACH_ADD; return true;
  case NYIR_SUB_I64: *out = NY_MACH_SUB; return true;
  case NYIR_MUL_I64: *out = NY_MACH_MUL; return true;
  case NYIR_DIV_I64: *out = NY_MACH_DIV; return true;
  case NYIR_MOD_I64: *out = NY_MACH_MOD; return true;
  case NYIR_AND_I64: *out = NY_MACH_AND; return true;
  case NYIR_OR_I64: *out = NY_MACH_OR; return true;
  case NYIR_XOR_I64: *out = NY_MACH_XOR; return true;
  case NYIR_SHL_I64: *out = NY_MACH_SHL; return true;
  case NYIR_SAR_I64: *out = NY_MACH_SAR; return true;
  case NYIR_ROR_I64: *out = NY_MACH_ROR; return true;
  case NYIR_ROR32_I64: *out = NY_MACH_ROR32; return true;
  case NYIR_CMP_I64: *out = NY_MACH_CMP; return true;
  case NYIR_SELECT_I64: *out = NY_MACH_SELECT; return true;
  case NYIR_I64_TO_F64:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64: *out = NY_MACH_CONVERT; return true;
  case NYIR_SQRT_F64: *out = NY_MACH_SQRT; return true;
  case NYIR_SIN_F64: *out = NY_MACH_SIN; return true;
  case NYIR_COS_F64: *out = NY_MACH_COS; return true;
  case NYIR_ADD_F64:
  case NYIR_ADD_F32: *out = NY_MACH_ADD; return true;
  case NYIR_SUB_F64:
  case NYIR_SUB_F32: *out = NY_MACH_SUB; return true;
  case NYIR_MUL_F64:
  case NYIR_MUL_F32: *out = NY_MACH_MUL; return true;
  case NYIR_DIV_F64:
  case NYIR_DIV_F32: *out = NY_MACH_DIV; return true;
  case NYIR_CMP_F64:
  case NYIR_CMP_F32: *out = NY_MACH_CMP; return true;
  default: return false;
  }
}

static bool mach_condition_from_nir(int64_t condition, ny_mach_cond_t *out) {
  if (!out || condition < NYIR_CMP_EQ || condition > NYIR_CMP_GE)
    return false;
  switch ((nyir_cmp_t)condition) {
  case NYIR_CMP_EQ: *out = NY_MACH_COND_EQ; return true;
  case NYIR_CMP_NE: *out = NY_MACH_COND_NE; return true;
  case NYIR_CMP_LT: *out = NY_MACH_COND_LT; return true;
  case NYIR_CMP_LE: *out = NY_MACH_COND_LE; return true;
  case NYIR_CMP_GT: *out = NY_MACH_COND_GT; return true;
  case NYIR_CMP_GE: *out = NY_MACH_COND_GE; return true;
  }
  return false;
}

static bool mach_nir_value_is_cmp_result(const nyir_func_t *nyir,
                                         const int *definitions, int value,
                                         unsigned depth) {
  if (!nyir || !definitions || value < 0 || value >= nyir->next_value ||
      depth > 16)
    return false;
  int di = definitions[value];
  if (di < 0 || (size_t)di >= nyir->len)
    return false;
  const nyir_inst_t *in = &nyir->data[(size_t)di];
  if (in->op == NYIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
      in->op == NYIR_CMP_F32)
    return true;
  if (in->op == NYIR_COPY)
    return mach_nir_value_is_cmp_result(nyir, definitions, in->a, depth + 1);
  return false;
}

static ny_mach_type_t mach_nir_value_type(const nyir_type_map_t *types,
                                          const bool *pointer_values,
                                          const int *definitions, int value,
                                          const nyir_func_t *nyir) {
  if (pointer_values && value >= 0 && pointer_values[value])
    return NY_MACH_TYPE_PTR;
  if (types && value >= 0 && (size_t)value < types->value_count) {
    if (mach_nir_value_is_cmp_result(nyir, definitions, value, 0))
      return NY_MACH_TYPE_I64;
    if (types->value_v128_f64 && types->value_v128_f64[value])
      return NY_MACH_TYPE_V128_F64;
    if (types->value_v128_f32 && types->value_v128_f32[value])
      return NY_MACH_TYPE_V128_F32;
    if (types->value_v256_i64 && types->value_v256_i64[value])
      return NY_MACH_TYPE_V256_I64;
    if (types->value_v128_i64 && types->value_v128_i64[value])
      return NY_MACH_TYPE_V128_I64;
    if (types->value_f64[value])
      return NY_MACH_TYPE_F64;
    if (types->value_f32[value])
      return NY_MACH_TYPE_F32;
  }
  return NY_MACH_TYPE_I64;
}


bool ny_mach_opcode_supported(nyir_op_t op) {
  switch (op) {
  case NYIR_NOP:
  case NYIR_BOUNDS_CHECK:
  case NYIR_CONST_I64:
  case NYIR_COPY:
  case NYIR_PHI:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_ROR_I64:
  case NYIR_ROR32_I64:
  case NYIR_SELECT_I64:
  case NYIR_CMP_I64:
  case NYIR_LABEL:
  case NYIR_LOAD_LOCAL:
  case NYIR_STORE_LOCAL:
  case NYIR_CALL:
  case NYIR_RET:
  case NYIR_BR:
  case NYIR_BR_IF:
  case NYIR_CONST_F64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_SQRT_F64:
  case NYIR_SIN_F64:
  case NYIR_COS_F64:
  case NYIR_I64_TO_F64:
  case NYIR_CMP_F64:
  case NYIR_CONST_F32:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_CMP_F32:
  case NYIR_ADDR_LOCAL:
  case NYIR_LOAD_I64:
  case NYIR_STORE_I64:
  case NYIR_ADDR_SYMBOL:
  case NYIR_ALLOCA:
  case NYIR_COPY_STRUCT:
  case NYIR_CAPTURE_RET:
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC4_ADD_F64:
  case NYIR_VEC4_SUB_F64:
  case NYIR_VEC4_MUL_F64:
  case NYIR_VEC4_DIV_F64:
  case NYIR_VEC4_FMA_F64:
  case NYIR_VEC4_SET1_F64:
  case NYIR_VEC4_SHUFFLE_F64:
  case NYIR_VEC4_REDUCE_ADD_F64:
  case NYIR_VEC8_LOAD_F32:
  case NYIR_VEC8_STORE_F32:
  case NYIR_VEC8_ADD_F32:
  case NYIR_VEC8_SUB_F32:
  case NYIR_VEC8_MUL_F32:
  case NYIR_VEC8_DIV_F32:
  case NYIR_VEC8_FMA_F32:
  case NYIR_VEC8_SET1_F32:
  case NYIR_VEC8_SHUFFLE_F32:
  case NYIR_VEC4_LOAD_I64:
  case NYIR_VEC4_SET1_I64:
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC4_ADD_I64:
  case NYIR_VEC4_SUB_I64:
  case NYIR_VEC4_AND_I64:
  case NYIR_VEC4_OR_I64:
  case NYIR_VEC4_XOR_I64:
  case NYIR_VEC4_SHL_I64:
  case NYIR_VEC4_SAR_I64:
  case NYIR_VEC8_LOAD_I64:
  case NYIR_VEC8_STORE_I64:
  case NYIR_VEC8_ADD_I64:
  case NYIR_VEC8_SUB_I64:
  case NYIR_VEC8_AND_I64:
  case NYIR_VEC8_OR_I64:
  case NYIR_VEC8_XOR_I64:
  case NYIR_VEC4_REDUCE_ADD_I64:
  case NYIR_VEC8_REDUCE_ADD_I64:
    return true;
  case NYIR_F64_TO_I64:
  case NYIR_F32_TO_I64:
  case NYIR_OP_COUNT:
    return false;
  }
  return false;
}

void ny_mach_opcode_coverage(size_t *supported, size_t *total) {
  size_t n = 0;
  for (int op = 0; op < NYIR_OP_COUNT; ++op)
    n += ny_mach_opcode_supported((nyir_op_t)op) ? 1u : 0u;
  if (supported)
    *supported = n;
  if (total)
    *total = NYIR_OP_COUNT;
}

bool ny_mach_lower_nir(const nyir_func_t *nyir, ny_mach_func_t *out,
                      unsigned caps, char *err, size_t err_len) {
  if (ny_trace_enabled("NY_TRACE_EMIT"))
    fprintf(stderr, "[mach enter] len=%zu\n", nyir ? nyir->len : 0);
  if (!nyir || !out)
    return nyir_err(err, err_len, "machine lower: missing input or output");
  char verify_err[256] = {0};
  if (!nyir_verify(nyir, verify_err, sizeof(verify_err)))
    return nyir_err(err, err_len, "machine lower: invalid NYIR: %s", verify_err);
  ny_mach_func_t lowered = {.param_count = nyir->param_count};
  ny_mach_reg_t *values = NULL;
  int *definitions = NULL;
  bool *pointer_values = NULL;
  mach_label_block_t *label_blocks = NULL;
  size_t label_block_len = 0;
  nyir_type_map_t types = {0};
  nyir_cfg_t cfg = {0};
  size_t local_count = nyir->param_count;
  bool *local_address_taken = NULL;
  bool has_bounds_checks = false;
  bool ok = false;
  if (nyir->next_value > 0) {
    definitions = nyir_build_defs(nyir);
    if (!definitions)
      return nyir_err(err, err_len, "machine lower: out of memory");
  }
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyir_inst_t *in = &nyir->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= 0 && (size_t)in->imm + 1 > local_count)
      local_count = (size_t)in->imm + 1;
  }
  if (!nyir_cfg_build(nyir, &cfg)) {
    nyir_err(err, err_len, "machine lower: failed to build CFG");
    goto done;
  }
  if (cfg.block_count > SIZE_MAX / sizeof(*label_blocks))
    goto oom;
  if (cfg.block_count) {
    label_blocks = ny_calloc_array(cfg.block_count, sizeof(*label_blocks));
    if (!label_blocks)
      goto oom;
    for (size_t block = 0; block < cfg.block_count; ++block) {
      if (cfg.block_label[block] < 0)
        continue;
      label_blocks[label_block_len++] = (mach_label_block_t){
          .label = cfg.block_label[block], .block = (uint32_t)block};
    }
    qsort(label_blocks, label_block_len, sizeof(*label_blocks),
          mach_label_block_cmp);
  }
  if (!nyir_type_map_init(&types, nyir, local_count))
    goto oom;
  if (local_count) {
    local_address_taken = ny_calloc_array(local_count, sizeof(*local_address_taken));
    if (!local_address_taken)
      goto oom;
    for (size_t i = 0; i < nyir->len; ++i) {
      const nyir_inst_t *in = &nyir->data[i];
      if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
          (size_t)in->imm < local_count)
        local_address_taken[in->imm] = true;
    }
  }
  {
    /*
     * Keep a valid base even for degenerate zero-value functions.  Normal
     * instructions still require verifier-valid SSA indices; the one-slot
     * allocation prevents a malformed/empty function from indexing NULL.
     */
    size_t value_count = nyir->next_value > 0 ? (size_t)nyir->next_value : 1u;
    values = ny_calloc_array(value_count, sizeof(*values));
    pointer_values = ny_calloc_array(value_count, sizeof(*pointer_values));
    if (!values || !pointer_values)
      goto oom;
    /*
     * Pointer values may cross SSA copies and PHIs before entering integer
     * address arithmetic.  Propagate that class with the existing use-def
     * graph rather than rescanning the complete function to a fixed point.
     */
    if (nyir->next_value > 0) {
    nyir_use_def_t pointer_uses = {0};
    int *pointer_queue =
        ny_malloc_array((size_t)nyir->next_value, sizeof(*pointer_queue));
    if (!pointer_queue || !nyir_build_use_def(nyir, &pointer_uses)) {
      free(pointer_queue);
      nyir_use_def_free(&pointer_uses);
      goto oom;
    }
    size_t pointer_head = 0, pointer_tail = 0;
    for (size_t i = 0; i < nyir->len; ++i) {
      const nyir_inst_t *in = &nyir->data[i];
      if ((in->op == NYIR_ADDR_LOCAL || in->op == NYIR_ADDR_SYMBOL ||
           in->op == NYIR_ALLOCA) &&
          in->dst >= 0 && in->dst < nyir->next_value &&
          !pointer_values[in->dst]) {
        pointer_values[in->dst] = true;
        pointer_queue[pointer_tail++] = in->dst;
      }
    }
    while (pointer_head < pointer_tail) {
      int value = pointer_queue[pointer_head++];
      if (value < 0 || (size_t)value >= pointer_uses.value_count)
        continue;
      for (size_t u = pointer_uses.offsets[value];
           u < pointer_uses.offsets[value + 1]; ++u) {
        size_t user_idx = pointer_uses.users[u];
        if (user_idx >= nyir->len)
          continue;
        const nyir_inst_t *user = &nyir->data[user_idx];
        if ((user->op != NYIR_COPY && user->op != NYIR_PHI) ||
            user->dst < 0 || user->dst >= nyir->next_value ||
            pointer_values[user->dst])
          continue;
        pointer_values[user->dst] = true;
        pointer_queue[pointer_tail++] = user->dst;
      }
    }
    free(pointer_queue);
    nyir_use_def_free(&pointer_uses);
    for (int value = 0; value < nyir->next_value; ++value) {
      if (!ny_mach_alloc_typed_vreg(
              &lowered, mach_nir_value_type(&types, pointer_values, definitions,
                                            value, nyir),
              &values[value]))
        goto oom;
    }
    for (size_t i = 0; i < nyir->len; ++i) {
      const nyir_inst_t *in = &nyir->data[i];
      if ((in->op == NYIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
           in->op == NYIR_CMP_F32) &&
          in->dst >= 0 && in->dst < nyir->next_value) {
        ny_mach_reg_t reg = values[in->dst];
        lowered.vreg_classes[reg] = NY_MACH_REGCLASS_GPR;
        lowered.vreg_types[reg] = NY_MACH_TYPE_I64;
      }
    }
    }
  }
  for (size_t local = 0; local < local_count; ++local) {
    uint32_t slot = 0;
    ny_mach_type_t type = NY_MACH_TYPE_I64;
    size_t slot_size = 8;
    if (types.local_v256_i64 && types.local_v256_i64[local]) {
      type = NY_MACH_TYPE_V256_I64;
      slot_size = 32;
    } else if (types.local_v128_f64 && types.local_v128_f64[local]) {
      type = NY_MACH_TYPE_V128_F64;
      slot_size = 16;
    } else if (types.local_v128_f32 && types.local_v128_f32[local]) {
      type = NY_MACH_TYPE_V128_F32;
      slot_size = 16;
    } else if (types.local_v128_i64 && types.local_v128_i64[local]) {
      type = NY_MACH_TYPE_V128_I64;
      slot_size = 16;
    } else if (types.local_f64[local]) {
      type = NY_MACH_TYPE_F64;
    } else if (types.local_f32[local]) {
      type = NY_MACH_TYPE_F32;
    }
    if (!ny_mach_alloc_typed_frame_slot(&lowered, slot_size, slot_size, type,
                                       local_address_taken[local], &slot) ||
        slot != local)
      goto oom;
  }
  for (size_t i = 0, block = 0; i < nyir->len; ++i) {
    const nyir_inst_t *in = &nyir->data[i];
    if ((in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) &&
        (in->flags & NYIR_INST_F_MEM_BYTE) &&
        !(caps & NY_NATIVE_CAP_MACH_BYTE))
      goto done;
    ny_mach_inst_t inst = {0};
    inst.source_pc = (uint32_t)i;
    if (block < cfg.block_count && cfg.block_start[block] == i) {
      uint32_t emitted_block = 0;
      /*
       * Machine blocks use stable dense IDs. Source labels remain NYIR
       * metadata; branches below resolve them to these canonical IDs.
       */
      if (!ny_mach_begin_block(&lowered, (uint32_t)block, &emitted_block) ||
          emitted_block != block)
        goto oom;
      lowered.blocks[emitted_block].source_pc = (uint32_t)i;
      ++block;
    }
    if (in->op == NYIR_LABEL || in->op == NYIR_PHI) {
      continue;
    }
    if (in->op == NYIR_CONST_I64 || in->op == NYIR_CONST_F64 ||
        in->op == NYIR_CONST_F32) {
      inst.opcode = NY_MACH_COPY;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = (ny_mach_operand_t){.kind = NY_MACH_OPERAND_IMM, .as.imm = in->imm};
    } else if (in->op == NYIR_LOAD_LOCAL) {
      inst.opcode = NY_MACH_LOAD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_frame((uint32_t)in->imm);
      inst.effects = NY_MACH_EFFECT_READ_MEMORY;
    } else if (in->op == NYIR_STORE_LOCAL) {
      inst.opcode = NY_MACH_STORE;
      inst.dst = mach_frame((uint32_t)in->imm);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.effects = NY_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_ADDR_LOCAL) {
      inst.opcode = NY_MACH_LEA;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_frame((uint32_t)in->imm);
    } else if (in->op == NYIR_ALLOCA) {
      /*
       * Stack allocation: N dense 8-byte frame slots. Slot index grows toward
       * lower addresses (slot i at -8*(base+i+1)), so LEA must use the *last*
       * slot: then [last, last+8*N) covers the N slots contiguously upward.
       * LEA(first) + sequential stores would write into earlier slots / CS.
       */
      size_t nbytes = in->imm > 0 ? (size_t)in->imm : 8;
      size_t nslots = (nbytes + 7) / 8;
      if (nslots == 0)
        nslots = 1;
      uint32_t last = 0;
      for (size_t k = 0; k < nslots; ++k) {
        uint32_t slot = 0;
        if (!ny_mach_alloc_typed_frame_slot(&lowered, 8, 8, NY_MACH_TYPE_I64,
                                           true, &slot))
          goto oom;
        last = slot;
      }
      if (in->dst < 0) {
        nyir_err(err, err_len,
                 "machine lower: NYIR opcode %s has no destination",
                 nyir_op_name(in->op));
        goto done;
      }
      inst.opcode = NY_MACH_LEA;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_frame(last);
    } else if (in->op == NYIR_CAPTURE_RET) {
      /*
       * Secondary ABI return register capture after CALL (SysV selectors).
       * Shape: INTRINSIC dst=vreg, src0=IMM selector (0=rdx,1=rax,2=xmm0,3=xmm1).
       */
      if (in->dst < 0) {
        nyir_err(err, err_len,
                 "machine lower: NYIR opcode %s has no destination",
                 nyir_op_name(in->op));
        goto done;
      }
      inst.opcode = NY_MACH_INTRINSIC;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = (ny_mach_operand_t){.kind = NY_MACH_OPERAND_IMM,
                                     .as.imm = in->imm};
    } else if (in->op == NYIR_COPY_STRUCT) {
      /*
       * Byte copy *dst ← *src of imm bytes.
       * Shape: INTRINSIC src0=dst-ptr, src1=src-ptr, src2=IMM size.
       */
      if (in->a < 0 || in->b < 0 || in->imm <= 0) {
        nyir_err(err, err_len,
                 "machine lower: NYIR opcode %s has invalid size or operands",
                 nyir_op_name(in->op));
        goto done;
      }
      inst.opcode = NY_MACH_INTRINSIC;
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
      inst.src2 = (ny_mach_operand_t){.kind = NY_MACH_OPERAND_IMM,
                                     .as.imm = in->imm};
      inst.effects = NY_MACH_EFFECT_READ_MEMORY | NY_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_ADDR_SYMBOL) {
      inst.opcode = NY_MACH_LEA;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = (ny_mach_operand_t){.kind = NY_MACH_OPERAND_SYMBOL,
                                      .as.symbol = in->symbol,
                                      .relocation = NY_MACH_RELOC_PC_REL32};
    } else if (in->op == NYIR_LOAD_I64) {
      inst.opcode = NY_MACH_LOAD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.byte_width = (in->flags & NYIR_INST_F_MEM_BYTE) != 0;
      inst.effects = NY_MACH_EFFECT_READ_MEMORY;
    } else if (in->op == NYIR_STORE_I64) {
      inst.opcode = NY_MACH_STORE;
      inst.dst = mach_vreg(&lowered, values[in->a]);
      inst.src0 = mach_vreg(&lowered, values[in->c]);
      inst.byte_width = (in->flags & NYIR_INST_F_MEM_BYTE) != 0;
      inst.effects = NY_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_RET) {
      inst.opcode = NY_MACH_RET;
      if (in->a >= 0)
        inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.effects = NY_MACH_EFFECT_CONTROL;
    } else if (in->op == NYIR_BR || in->op == NYIR_BR_IF) {
      int target = mach_find_nir_block(label_blocks, label_block_len, in->imm);
      if (target < 0) {
        nyir_err(err, err_len, "machine lower: branch target is missing");
        goto done;
      }
      inst.opcode = in->op == NYIR_BR ? NY_MACH_BR : NY_MACH_BR_IF;
      if (in->op == NYIR_BR_IF)
        inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = (ny_mach_operand_t){.kind = NY_MACH_OPERAND_BLOCK,
                                     .as.block_index = (uint32_t)target};
      inst.effects = NY_MACH_EFFECT_CONTROL;
    } else if (in->op == NYIR_CALL) {
      inst.opcode = NY_MACH_CALL;
      if (in->dst >= 0)
        inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = (ny_mach_operand_t){.kind = NY_MACH_OPERAND_SYMBOL,
                                     .as.symbol = in->symbol,
                                     .relocation = NY_MACH_RELOC_CALL};
      if (in->imm > 0) {
        int args[NYIR_CALL_MAX_ARGS] = {0};
        int arg_count = 0;
        char args_err[128] = {0};
        if (!nyir_call_args(in, nyir->next_value, args,
                              sizeof(args) / sizeof(args[0]), &arg_count,
                              args_err, sizeof(args_err))) {
          nyir_err(err, err_len, "machine lower: invalid call arguments: %s",
                  args_err[0] ? args_err : NY_NATIVE_UNKNOWN_ERR);
          goto done;
        }
        inst.args_len = (size_t)arg_count;
        inst.args = ny_calloc_array(inst.args_len, sizeof(*inst.args));
        if (!inst.args)
          goto oom;
        if (in->arg_sizes) {
          inst.arg_sizes = ny_calloc_array(inst.args_len, sizeof(*inst.arg_sizes));
          if (!inst.arg_sizes) {
            free(inst.args);
            goto oom;
          }
        }
        for (size_t arg = 0; arg < inst.args_len; ++arg)
          inst.args[arg] = mach_vreg(&lowered, values[args[arg]]);
        if (inst.arg_sizes)
          memcpy(inst.arg_sizes, in->arg_sizes,
                 inst.args_len * sizeof(*inst.arg_sizes));
      }
      inst.call_is_extern = (in->flags & NYIR_INST_F_EXTERN) != 0;
      inst.call_has_sret = (in->flags & NYIR_INST_F_SRET) != 0;
      inst.effects = NY_MACH_EFFECT_CALL;
    } else if (in->op == NYIR_SELECT_I64) {
      inst.opcode = NY_MACH_SELECT;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
      inst.src2 = mach_vreg(&lowered, values[in->c]);
      inst.condition = NY_MACH_COND_NE;
    } else if (in->op == NYIR_SQRT_F64 || in->op == NYIR_SIN_F64 || in->op == NYIR_COS_F64) {
      if (getenv("NY_DUMP_MACH")) {
        fprintf(stderr, "[mach_lower] handling trig: op=%s\n", nyir_op_name(in->op));
      }
      /*
       * x87 trig functions require memory operands; spill src to frame if needed
       */
      ny_mach_inst_t inst = {0};
      inst.source_pc = (uint32_t)i;
      ny_mach_operand_t src0 = mach_vreg(&lowered, values[in->a]);
      bool src0_is_vreg = (src0.kind == NY_MACH_OPERAND_VREG);
      uint32_t src0_frame = 0;
      if (src0_is_vreg) {
        if (!ny_mach_alloc_typed_frame_slot(&lowered, 8, 8, NY_MACH_TYPE_F64,
                                           false, &src0_frame))
          goto oom;
        ny_mach_inst_t spill = {.opcode = NY_MACH_STORE,
                                .source_pc = (uint32_t)i,
                                .dst = mach_frame(src0_frame),
                                .src0 = mach_vreg(&lowered, values[in->a]),
                                .effects = NY_MACH_EFFECT_WRITE_MEMORY};
        if (!ny_mach_emit(&lowered, spill))
          goto oom;
        src0 = mach_frame(src0_frame);
      } else {
        src0 = mach_vreg(&lowered, values[in->a]);
      }
      ny_mach_operand_t dst = mach_vreg(&lowered, values[in->dst]);
      bool dst_is_vreg = (dst.kind == NY_MACH_OPERAND_VREG);
      uint32_t dst_frame = 0;
      if (dst_is_vreg) {
        if (!ny_mach_alloc_typed_frame_slot(&lowered, 8, 8, NY_MACH_TYPE_F64,
                                           false, &dst_frame))
          goto oom;
        dst = mach_frame(dst_frame);
      }
      inst.opcode = (in->op == NYIR_SQRT_F64) ? NY_MACH_SQRT :
                    (in->op == NYIR_SIN_F64) ? NY_MACH_SIN : NY_MACH_COS;
      inst.src0 = src0;
      inst.dst = mach_frame(dst_frame);
      inst.effects = NY_MACH_EFFECT_NONE;
      if (!ny_mach_emit(&lowered, inst))
        goto oom;
      if (dst_is_vreg) {
        /*
         * Reload result from frame to vreg
         */
        ny_mach_inst_t reload = {.opcode = NY_MACH_LOAD,
                                 .source_pc = (uint32_t)i,
                                 .dst = mach_vreg(&lowered, values[in->dst]),
                                 .src0 = mach_frame(dst_frame),
                                 .effects = NY_MACH_EFFECT_READ_MEMORY};
        if (!ny_mach_emit(&lowered, reload))
          goto oom;
      }
      continue;
    } else if (mach_opcode_from_nir(in->op, &inst.opcode)) {
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.narrow32 = (in->flags & NYIR_INST_F_NARROW32) != 0;
      /*
       * Untyped source parameters may already acquire the destination's
       * floating type through their call sites.  A conversion between equal
       * machine types is a move, not an invalid f64-to-f64 conversion.
       */
      if (inst.opcode == NY_MACH_CONVERT &&
          mach_same_value_type(&lowered, &inst.dst, &inst.src0))
        inst.opcode = NY_MACH_COPY;
      if (in->op != NYIR_COPY && in->op != NYIR_I64_TO_F64 &&
          in->op != NYIR_I64_TO_F32 && in->op != NYIR_F64_TO_F32 &&
          in->op != NYIR_F32_TO_F64 && in->op != NYIR_SQRT_F64 &&
          in->op != NYIR_SIN_F64 && in->op != NYIR_COS_F64)
        inst.src1 = mach_vreg(&lowered, values[in->b]);
      if (inst.opcode == NY_MACH_CMP) {
        inst.dst = mach_vreg(&lowered, values[in->dst]);
        if (in->a >= 0 && in->a < nyir->next_value) {
          int di = definitions ? definitions[in->a] : -1;
          bool cmp_result = di >= 0 &&
              (nyir->data[di].op == NYIR_CMP_I64 ||
               nyir->data[di].op == NYIR_CMP_F64 ||
               nyir->data[di].op == NYIR_CMP_F32);
          if (cmp_result) {
            lowered.vreg_classes[values[in->a]] = NY_MACH_REGCLASS_GPR;
            lowered.vreg_types[values[in->a]] = NY_MACH_TYPE_I64;
          }
          inst.src0 = mach_vreg(&lowered, values[in->a]);
        }
        if (in->b >= 0 && in->b < nyir->next_value) {
          int di = definitions ? definitions[in->b] : -1;
          bool cmp_result = di >= 0 &&
              (nyir->data[di].op == NYIR_CMP_I64 ||
               nyir->data[di].op == NYIR_CMP_F64 ||
               nyir->data[di].op == NYIR_CMP_F32);
          if (cmp_result) {
            lowered.vreg_classes[values[in->b]] = NY_MACH_REGCLASS_GPR;
            lowered.vreg_types[values[in->b]] = NY_MACH_TYPE_I64;
          }
          inst.src1 = mach_vreg(&lowered, values[in->b]);
        }
      }
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.narrow32 = (in->flags & NYIR_INST_F_NARROW32) != 0;
      /*
       * Untyped source parameters may already acquire the destination's
       * floating type through their call sites.  A conversion between equal
       * machine types is a move, not an invalid f64-to-f64 conversion.
       */
      if (inst.opcode == NY_MACH_CONVERT &&
          mach_same_value_type(&lowered, &inst.dst, &inst.src0))
        inst.opcode = NY_MACH_COPY;
      if (in->op != NYIR_COPY && in->op != NYIR_I64_TO_F64 &&
          in->op != NYIR_I64_TO_F32 && in->op != NYIR_F64_TO_F32 &&
          in->op != NYIR_F32_TO_F64 && in->op != NYIR_SQRT_F64 &&
          in->op != NYIR_SIN_F64 && in->op != NYIR_COS_F64)
        inst.src1 = mach_vreg(&lowered, values[in->b]);
      if (inst.opcode == NY_MACH_CMP) {
        if (in->a >= 0 && in->a < nyir->next_value) {
          int di = definitions ? definitions[in->a] : -1;
          bool cmp_result = di >= 0 &&
              (nyir->data[di].op == NYIR_CMP_I64 ||
               nyir->data[di].op == NYIR_CMP_F64 ||
               nyir->data[di].op == NYIR_CMP_F32);
          if (cmp_result) {
            lowered.vreg_classes[values[in->a]] = NY_MACH_REGCLASS_GPR;
            lowered.vreg_types[values[in->a]] = NY_MACH_TYPE_I64;
            inst.src0 = mach_vreg(&lowered, values[in->a]);
          }
        }
        if (in->b >= 0 && in->b < nyir->next_value) {
          int di = definitions ? definitions[in->b] : -1;
          bool cmp_result = di >= 0 &&
              (nyir->data[di].op == NYIR_CMP_I64 ||
               nyir->data[di].op == NYIR_CMP_F64 ||
               nyir->data[di].op == NYIR_CMP_F32);
          if (cmp_result) {
            lowered.vreg_classes[values[in->b]] = NY_MACH_REGCLASS_GPR;
            lowered.vreg_types[values[in->b]] = NY_MACH_TYPE_I64;
            inst.src1 = mach_vreg(&lowered, values[in->b]);
          }
        }
      }
      if (inst.opcode == NY_MACH_CMP &&
          !mach_condition_from_nir(in->cmp, &inst.condition)) {
        nyir_err(err, err_len, "machine lower: invalid comparison condition");
        goto done;
      }
    } else if (in->op == NYIR_VEC4_REDUCE_ADD_F64 ||
               in->op == NYIR_VEC4_REDUCE_ADD_I64 ||
               in->op == NYIR_VEC8_REDUCE_ADD_I64) {
      inst.opcode = NY_MACH_REDUCE_ADD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
    } else if (in->op == NYIR_VEC4_ADD_I64 || in->op == NYIR_VEC4_SUB_I64 ||
               in->op == NYIR_VEC4_AND_I64 || in->op == NYIR_VEC4_OR_I64 ||
               in->op == NYIR_VEC4_XOR_I64 || in->op == NYIR_VEC4_ADD_F64 ||
               in->op == NYIR_VEC4_SUB_F64 || in->op == NYIR_VEC4_MUL_F64 ||
               in->op == NYIR_VEC4_DIV_F64 || in->op == NYIR_VEC8_ADD_F32 ||
               in->op == NYIR_VEC8_SUB_F32 || in->op == NYIR_VEC8_MUL_F32 ||
               in->op == NYIR_VEC8_DIV_F32 ||               in->op == NYIR_VEC4_SHL_I64 ||
               in->op == NYIR_VEC4_SAR_I64 ||
               in->op == NYIR_VEC8_ADD_I64 || in->op == NYIR_VEC8_SUB_I64 ||
               in->op == NYIR_VEC8_AND_I64 || in->op == NYIR_VEC8_OR_I64 ||
               in->op == NYIR_VEC8_XOR_I64) {
      /*
       * Packed vector ALU: one machine op, element type on the vreg.
       */
      if (in->op == NYIR_VEC4_SUB_I64 || in->op == NYIR_VEC4_SUB_F64 ||
          in->op == NYIR_VEC8_SUB_F32)
        inst.opcode = NY_MACH_SUB;
      else if (in->op == NYIR_VEC4_MUL_F64 || in->op == NYIR_VEC8_MUL_F32)
        inst.opcode = NY_MACH_MUL;
      else if (in->op == NYIR_VEC4_DIV_F64 || in->op == NYIR_VEC8_DIV_F32)
        inst.opcode = NY_MACH_DIV;
      else if (in->op == NYIR_VEC4_AND_I64)
        inst.opcode = NY_MACH_AND;
      else if (in->op == NYIR_VEC4_OR_I64)
        inst.opcode = NY_MACH_OR;
      else if (in->op == NYIR_VEC4_XOR_I64)
        inst.opcode = NY_MACH_XOR;
      else if (in->op == NYIR_VEC4_SHL_I64)
        inst.opcode = NY_MACH_SHL;
      else if (in->op == NYIR_VEC4_SAR_I64)
        inst.opcode = NY_MACH_SAR;
      else
        inst.opcode = NY_MACH_ADD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
    } else if (in->op == NYIR_VEC4_FMA_F64 || in->op == NYIR_VEC8_FMA_F32) {
      inst.opcode = NY_MACH_FMA;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 = mach_vreg(&lowered, values[in->b]);
      inst.src2 = mach_vreg(&lowered, values[in->c]);
    } else if (in->op == NYIR_VEC4_SHUFFLE_F64 ||
               in->op == NYIR_VEC8_SHUFFLE_F32) {
      inst.opcode = NY_MACH_SHUFFLE;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.src1 =
          (ny_mach_operand_t){.kind = NY_MACH_OPERAND_IMM, .as.imm = in->imm};
    } else if (in->op == NYIR_VEC4_LOAD_I64 || in->op == NYIR_VEC4_LOAD_F64 ||
               in->op == NYIR_VEC8_LOAD_F32 || in->op == NYIR_VEC8_LOAD_I64) {
      inst.opcode = NY_MACH_LOAD;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
      inst.effects = NY_MACH_EFFECT_READ_MEMORY;
    } else if (in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC4_STORE_F64 ||
               in->op == NYIR_VEC8_STORE_F32 ||
               in->op == NYIR_VEC8_STORE_I64) {
      inst.opcode = NY_MACH_STORE;
      inst.dst = mach_vreg(&lowered, values[in->a]);
      inst.src0 = mach_vreg(&lowered, values[in->b >= 0 ? in->b : in->c]);
      inst.effects = NY_MACH_EFFECT_WRITE_MEMORY;
    } else if (in->op == NYIR_VEC4_SET1_F64 ||
               in->op == NYIR_VEC8_SET1_F32 ||
               in->op == NYIR_VEC4_SET1_I64) {
      /*
       * Broadcast scalar → packed vector via COPY; encoder expands.
       */
      inst.opcode = NY_MACH_COPY;
      inst.dst = mach_vreg(&lowered, values[in->dst]);
      inst.src0 = mach_vreg(&lowered, values[in->a]);
    } else if (in->op == NYIR_BOUNDS_CHECK) {
      if (!(caps & NY_NATIVE_CAP_MACH_TRAP)) {
        nyir_err(err, err_len,
                 "machine lower: NYIR opcode %s requires machine trap capability",
                 nyir_op_name(in->op));
        goto done;
      }
      /*
       * Compare offset against byte_len; branch to trap block on violation.
       * .a=base(ignored here), .b=offset(vreg), .imm=byte_len
       */
      ny_mach_reg_t len_vreg;
      if (!ny_mach_alloc_typed_vreg(&lowered, NY_MACH_TYPE_I64, &len_vreg))
        goto oom;
      ny_mach_inst_t copy_len = {
          .opcode = NY_MACH_COPY,
          .source_pc = (uint32_t)i,
          .dst = mach_vreg(&lowered, len_vreg),
          .src0 = in->c >= 0
                      ? mach_vreg(&lowered, values[in->c])
                      : (ny_mach_operand_t){.kind = NY_MACH_OPERAND_IMM,
                                            .as.imm = in->imm}};
      if (!ny_mach_emit(&lowered, copy_len))
        goto oom;
      ny_mach_reg_t cmp_vreg;
      if (!ny_mach_alloc_typed_vreg(&lowered, NY_MACH_TYPE_I64, &cmp_vreg))
        goto oom;
      ny_mach_inst_t cmp = {
          .opcode = NY_MACH_CMP,
          .source_pc = (uint32_t)i,
          .dst = mach_vreg(&lowered, cmp_vreg),
          .src0 = mach_vreg(&lowered, values[in->b]),
          .src1 = mach_vreg(&lowered, len_vreg),
          .condition = NY_MACH_COND_GE};
      if (!ny_mach_emit(&lowered, cmp))
        goto oom;
      /*
       * Bounds checks are emitted while the source block layout is still
       * being lowered.  Their target is patched to one trap block appended
       * after every source block, so it cannot perturb source CFG order.
       * Register allocation runs only after this complete machine function
       * has been verified; the trap has no value operands and therefore adds
       * no live range.
       */
      ny_mach_inst_t br = {
          .opcode = NY_MACH_BR_IF,
          .source_pc = (uint32_t)i,
          .src0 = mach_vreg(&lowered, cmp_vreg),
          .src1 = {.kind = NY_MACH_OPERAND_BLOCK,
                   .as.block_index = UINT32_MAX},
          .effects = NY_MACH_EFFECT_CONTROL};
      if (!ny_mach_emit(&lowered, br))
        goto oom;
      has_bounds_checks = true;
      continue;
    } else if (in->op == NYIR_NOP) {
      continue;
    } else {
      nyir_err(err, err_len,
               "machine lower: unsupported NYIR opcode %s",
               nyir_op_name(in->op));
      goto done;
    }
    if (inst.opcode == NY_MACH_ADD || inst.opcode == NY_MACH_SUB ||
        inst.opcode == NY_MACH_MUL || inst.opcode == NY_MACH_DIV ||
        inst.opcode == NY_MACH_CMP) {
      ny_mach_type_t t0 = mach_value_type(&lowered, &inst.src0);
      ny_mach_type_t t1 = mach_value_type(&lowered, &inst.src1);
      if (mach_type_is_float(t0) && mach_is_integer_value(&lowered, &inst.src1)) {
        ny_mach_reg_t conv_reg;
        if (!ny_mach_alloc_typed_vreg(&lowered, t0, &conv_reg))
          goto oom;
        ny_mach_inst_t conv = {.opcode = NY_MACH_CONVERT,
                               .source_pc = (uint32_t)i,
                               .dst = mach_vreg(&lowered, conv_reg),
                               .src0 = inst.src1};
        if (!ny_mach_emit(&lowered, conv))
          goto oom;
        inst.src1 = mach_vreg(&lowered, conv_reg);
      } else if (mach_type_is_float(t1) &&
                 mach_is_integer_value(&lowered, &inst.src0)) {
        ny_mach_reg_t conv_reg;
        if (!ny_mach_alloc_typed_vreg(&lowered, t1, &conv_reg))
          goto oom;
        ny_mach_inst_t conv = {.opcode = NY_MACH_CONVERT,
                               .source_pc = (uint32_t)i,
                               .dst = mach_vreg(&lowered, conv_reg),
                               .src0 = inst.src0};
        if (!ny_mach_emit(&lowered, conv))
          goto oom;
        inst.src0 = mach_vreg(&lowered, conv_reg);
      }
    }
    if (!mach_verify_opcode_shape(&lowered, &inst)) {
      const char *a_def = "none";
      if (in->a >= 0 && in->a < nyir->next_value && definitions) {
        int di = definitions[in->a];
        if (di >= 0)
          a_def = nyir_op_name(nyir->data[di].op);
      }
      const char *file = in->debug.file ? in->debug.file : "<unknown>";
      if (ny_trace_enabled("NY_TRACE_MACHINE_FAIL"))
        nyir_dump_compact(stderr, nyir, "machine-fail");
      nyir_err(err, err_len,
               "machine lower: NYIR instruction %zu (%s v%d v%d -> v%d) has invalid machine shape"
               " [dst=%s src0=%s src1=%s a_def=%s at %s:%d]",
               i, nyir_op_name(in->op), in->a, in->b, in->dst,
               mach_type_name(mach_value_type(&lowered, &inst.dst)),
               mach_type_name(mach_value_type(&lowered, &inst.src0)),
               mach_type_name(mach_value_type(&lowered, &inst.src1)),
               a_def, file, in->debug.line);
      free(inst.args);
      free(inst.arg_sizes);
      goto done;
    }
    if (!ny_mach_emit(&lowered, inst)) {
      free(inst.args);
      free(inst.arg_sizes);
      goto oom;
    }
  }
  if (!mach_lower_phi_edges(nyir, &cfg, values, &lowered, err, err_len))
    goto done;
  if (has_bounds_checks) {
    uint32_t trap_block_idx = 0;
    if (!ny_mach_begin_block(&lowered, UINT32_MAX, &trap_block_idx) ||
        trap_block_idx != (uint32_t)lowered.block_len - 1) {
      nyir_err(err, err_len, "machine lower: failed to create bounds-trap block");
      goto done;
    }
    /*
     * Patch all sentinel block references to the real trap block.
     */
    for (size_t ii = 0; ii < lowered.inst_len; ++ii) {
      if (lowered.insts[ii].opcode == NY_MACH_BR_IF &&
          lowered.insts[ii].src1.kind == NY_MACH_OPERAND_BLOCK &&
          lowered.insts[ii].src1.as.block_index == UINT32_MAX)
        lowered.insts[ii].src1.as.block_index = trap_block_idx;
    }
    ny_mach_inst_t trap = {.opcode = NY_MACH_TRAP,
                           .source_pc = UINT32_MAX,
                           .effects = NY_MACH_EFFECT_CONTROL};
    if (!ny_mach_emit(&lowered, trap))
      goto oom;
    /*
     * Bounds-check traps are lowered to CMP+BR_IF+TRAP for backends
     * with NY_NATIVE_CAP_MACH_TRAP (x86-64 ud2, AArch64 brk #0).
     * Other backends skip the machine-form lowering and retain the
     * NYIR VM guard path (eval.c bounds_violation).
     */
  }
  if (!ny_mach_verify(&lowered, 0, err, err_len))
    goto done;
  /*
   * Publish only the scheduled stream. Register allocation is constructed by
   * encoders after this function returns, so no position-sensitive allocation
   * view exists while `lowered` is still mutable.
   */
  if (!ny_mach_schedule_post_ra(&lowered))
    goto oom;
  ny_mach_func_free(out);
  *out = lowered;
  lowered = (ny_mach_func_t){0};
  ok = true;
done:
  free(definitions);
  free(values);
  free(pointer_values);
  free(label_blocks);
  free(local_address_taken);
  nyir_type_map_free(&types);
  nyir_cfg_free(&cfg);
  ny_mach_func_free(&lowered);
  return ok;
oom:
  nyir_err(err, err_len, "machine lower: out of memory");
  goto done;
}
/*
 * Post-register-allocation scheduling is deliberately a small, boring pass.
 * It only touches a block when every instruction is a side-effect-free
 * machine operation with virtual-register operands.  In particular, a block
 * containing memory, control flow, calls, flags, or a physical register is a
 * scheduling barrier.  This keeps the pass useful for encoders that retain
 * machine values after allocation while making the unsafe cases an explicit
 * no-op rather than a guessed alias analysis.
 */

static bool mach_schedule_value_equal(const ny_mach_operand_t *left,
                                      const ny_mach_operand_t *right) {
  return left && right && mach_is_value(left) && mach_is_value(right) &&
         left->kind == right->kind && left->as.reg == right->as.reg &&
         left->reg_class == right->reg_class;
}

static bool mach_schedule_operand_is_fixed(const ny_mach_operand_t *operand) {
  /*
   * PREG is intentionally treated as a fixed-register barrier.  Machine form
   * does not carry target register allocation metadata, so assuming that a
   * physical register is freely renameable would make shifts using %cl,
   * implicit div/mod registers, and ABI sequences reorderable by accident.
   */
  return operand && (operand->kind == NY_MACH_OPERAND_PREG ||
                     operand->reg_class == NY_MACH_REGCLASS_FLAGS);
}

static bool mach_schedule_inst_has_fixed_operand(const ny_mach_inst_t *inst) {
  if (!inst)
    return true;
  const ny_mach_operand_t *ops[] = {
      &inst->dst, &inst->src0, &inst->src1, &inst->src2};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
    if (mach_schedule_operand_is_fixed(ops[i]))
      return true;
  for (size_t i = 0; i < inst->args_len; ++i)
    if (mach_schedule_operand_is_fixed(&inst->args[i]))
      return true;
  return false;
}

static bool mach_schedule_opcode_allowed(ny_mach_opcode_t opcode) {
  switch (opcode) {
  case NY_MACH_NOP:
  case NY_MACH_COPY:
  case NY_MACH_CONVERT:
  case NY_MACH_LEA:
  case NY_MACH_ADD:
  case NY_MACH_SUB:
  case NY_MACH_MUL:
  case NY_MACH_AND:
  case NY_MACH_OR:
  case NY_MACH_XOR:
  case NY_MACH_FMA:
  case NY_MACH_SQRT:
  case NY_MACH_SIN:
  case NY_MACH_COS:
  case NY_MACH_SHUFFLE:
  case NY_MACH_SELECT:
  case NY_MACH_REDUCE_ADD:
    return true;
  default:
    return false;
  }
}

static bool mach_schedule_inst_safe(const ny_mach_inst_t *inst) {
  if (!inst || !mach_schedule_opcode_allowed(inst->opcode) ||
      inst->effects != NY_MACH_EFFECT_NONE || inst->args_len != 0 ||
      inst->args != NULL || inst->arg_sizes != NULL ||
      inst->call_clobbers != 0 || inst->call_is_extern ||
      inst->call_has_sret || mach_schedule_inst_has_fixed_operand(inst))
    return false;
  /*
   * Shift-count lowering may select %cl even for a virtual source. Without
   * target constraint metadata, retain every shift block unchanged.
   */
  return inst->opcode != NY_MACH_CMP && inst->opcode != NY_MACH_DIV &&
         inst->opcode != NY_MACH_MOD && inst->opcode != NY_MACH_SHL &&
         inst->opcode != NY_MACH_SAR && inst->opcode != NY_MACH_ROR &&
         inst->opcode != NY_MACH_ROR32;
}

static bool mach_schedule_inst_reads(const ny_mach_inst_t *inst,
                                     const ny_mach_operand_t *value) {
  if (!inst || !value)
    return false;
  const ny_mach_operand_t *ops[] = {
      &inst->src0, &inst->src1, &inst->src2};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
    if (mach_schedule_value_equal(ops[i], value))
      return true;
  for (size_t i = 0; i < inst->args_len; ++i)
    if (mach_schedule_value_equal(&inst->args[i], value))
      return true;
  return false;
}

static bool mach_schedule_inst_writes(const ny_mach_inst_t *inst,
                                      const ny_mach_operand_t *value) {
  return inst && value && mach_schedule_value_equal(&inst->dst, value);
}

static unsigned mach_schedule_latency(ny_mach_opcode_t opcode) {
  /*
   * These are intentionally coarse x86-64-oriented classes.  The scheduler
   * only uses them to prefer a ready long-latency producer; correctness comes
   * solely from the dependency graph and barriers.
   */
  switch (opcode) {
  case NY_MACH_MUL:
  case NY_MACH_FMA:
    return 3;
  case NY_MACH_CONVERT:
    return 3;
  case NY_MACH_SQRT:
    return 12;
  case NY_MACH_SIN:
  case NY_MACH_COS:
    return 20;
  case NY_MACH_SHUFFLE:
  case NY_MACH_REDUCE_ADD:
    return 3;
  case NY_MACH_COPY:
  case NY_MACH_LEA:
    return 1;
  default:
    return 1;
  }
}

static bool mach_schedule_block(ny_mach_func_t *func,
                                const ny_mach_block_t *block) {
  if (!func || !block || block->first_inst > func->inst_len ||
      block->inst_count > func->inst_len - block->first_inst ||
      block->inst_count < 2)
    return true;
  const size_t count = block->inst_count;
  ny_mach_inst_t *insts = func->insts + block->first_inst;
  for (size_t i = 0; i < count; ++i)
    if (!mach_schedule_inst_safe(&insts[i]))
      return true;

  if (count > SIZE_MAX / count)
    return false;
  const size_t edge_count = count * count;
  unsigned char *edges = calloc(edge_count, sizeof(*edges));
  unsigned *indegree = calloc(count, sizeof(*indegree));
  unsigned *critical = calloc(count, sizeof(*critical));
  bool *selected = calloc(count, sizeof(*selected));
  size_t *order = malloc(count * sizeof(*order));
  ny_mach_inst_t *ordered = malloc(count * sizeof(*ordered));
  if (!edges || !indegree || !critical || !selected || !order || !ordered) {
    free(edges);
    free(indegree);
    free(critical);
    free(selected);
    free(order);
    free(ordered);
    return false;
  }

  /*
   * Preserve every value dependency and every write/write ordering.  The
   * original order is otherwise not a dependency, which is what lets the
   * ready-list choose independent work from later in the block.
   */
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = i + 1; j < count; ++j) {
      bool dependency = false;
      if (mach_is_value(&insts[i].dst)) {
        dependency = mach_schedule_inst_reads(&insts[j], &insts[i].dst) ||
                     mach_schedule_inst_writes(&insts[j], &insts[i].dst);
      }
      if (!dependency && mach_is_value(&insts[j].dst))
        dependency = mach_schedule_inst_reads(&insts[i], &insts[j].dst);
      if (dependency) {
        edges[i * count + j] = 1;
        ++indegree[j];
      }
    }
  }

  for (size_t i = count; i-- > 0;) {
    unsigned latency = mach_schedule_latency(insts[i].opcode);
    unsigned successor = 0;
    for (size_t j = i + 1; j < count; ++j)
      if (edges[i * count + j] && critical[j] > successor)
        successor = critical[j];
    critical[i] = latency + successor;
  }

  for (size_t out = 0; out < count; ++out) {
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < count; ++i) {
      if (selected[i] || indegree[i] != 0)
        continue;
      if (best == SIZE_MAX || critical[i] > critical[best] ||
          (critical[i] == critical[best] && i < best))
        best = i;
    }
    if (best == SIZE_MAX) {
      free(edges);
      free(indegree);
      free(critical);
      free(selected);
      free(order);
      free(ordered);
      return false;
    }
    selected[best] = true;
    order[out] = best;
    ordered[out] = insts[best];
    for (size_t j = best + 1; j < count; ++j)
      if (edges[best * count + j])
        --indegree[j];
  }

  bool changed = false;
  for (size_t i = 0; i < count; ++i)
    if (order[i] != i) {
      changed = true;
      break;
    }
  if (changed) {
    for (size_t i = 0; i < count; ++i)
      insts[i] = ordered[i];
  }

  free(edges);
  free(indegree);
  free(critical);
  free(selected);
  free(order);
  free(ordered);
  return true;
}

static uint64_t mach_outline_mix(uint64_t hash, uint64_t value) {
  hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
  return hash * UINT64_C(0x100000001b3);
}

static bool mach_outline_operand_ok(const ny_mach_func_t *func,
                                    const ny_mach_operand_t *operand) {
  if (!func || !operand || operand->relocation != NY_MACH_RELOC_NONE ||
      operand->addend != 0)
    return false;
  if (operand->kind == NY_MACH_OPERAND_NONE)
    return operand->reg_class == NY_MACH_REGCLASS_NONE;
  if (operand->kind == NY_MACH_OPERAND_IMM)
    return operand->reg_class == NY_MACH_REGCLASS_NONE;
  if (operand->kind != NY_MACH_OPERAND_VREG ||
      operand->as.reg >= func->vreg_len || !func->vreg_types ||
      !func->vreg_classes || operand->reg_class == NY_MACH_REGCLASS_FLAGS)
    return false;
  return func->vreg_classes[operand->as.reg] == operand->reg_class &&
         func->vreg_types[operand->as.reg] != NY_MACH_TYPE_FLAGS;
}

static bool mach_outline_inst_ok(const ny_mach_func_t *func,
                                 const ny_mach_inst_t *inst) {
  if (!func || !inst || inst->effects != NY_MACH_EFFECT_NONE ||
      inst->condition != NY_MACH_COND_ALWAYS || inst->args ||
      inst->arg_sizes || inst->args_len || inst->call_is_extern ||
      inst->call_has_sret || inst->call_clobbers != 0)
    return false;
  switch (inst->opcode) {
  case NY_MACH_COPY:
  case NY_MACH_CONVERT:
  case NY_MACH_ADD:
  case NY_MACH_SUB:
  case NY_MACH_MUL:
  case NY_MACH_AND:
  case NY_MACH_OR:
  case NY_MACH_XOR:
  case NY_MACH_SHL:
  case NY_MACH_SAR:
  case NY_MACH_ROR:
  case NY_MACH_ROR32:
  case NY_MACH_FMA:
  case NY_MACH_SQRT:
  case NY_MACH_SIN:
  case NY_MACH_COS:
  case NY_MACH_SHUFFLE:
    break;
  default:
    return false;
  }
  return mach_outline_operand_ok(func, &inst->dst) &&
         mach_outline_operand_ok(func, &inst->src0) &&
         mach_outline_operand_ok(func, &inst->src1) &&
         mach_outline_operand_ok(func, &inst->src2);
}

static size_t mach_outline_body_len(const ny_mach_func_t *func) {
  if (!func || !func->insts || !func->blocks || func->block_len != 1 ||
      func->inst_len < 2 || func->blocks[0].inst_count != func->inst_len ||
      func->insts[func->inst_len - 1].opcode != NY_MACH_RET)
    return 0;
  for (size_t i = 0; i + 1 < func->inst_len; ++i)
    if (func->insts[i].opcode == NY_MACH_RET)
      return 0;
  return func->inst_len - 1;
}

static bool mach_outline_operand_hash(const ny_mach_func_t *func,
                                      const ny_mach_operand_t *operand,
                                      uint32_t *map, uint32_t *next,
                                      uint64_t *hash) {
  if (!mach_outline_operand_ok(func, operand) || !next || !hash)
    return false;
  *hash = mach_outline_mix(*hash, operand->kind);
  *hash = mach_outline_mix(*hash, operand->reg_class);
  if (operand->kind == NY_MACH_OPERAND_IMM) {
    *hash = mach_outline_mix(*hash, (uint64_t)operand->as.imm);
  } else if (operand->kind == NY_MACH_OPERAND_VREG) {
    uint32_t id = operand->as.reg;
    uint32_t ordinal = map[id];
    if (ordinal == UINT32_MAX)
      map[id] = ordinal = (*next)++;
    *hash = mach_outline_mix(*hash, func->vreg_types[id]);
    *hash = mach_outline_mix(*hash, ordinal);
  }
  return true;
}

typedef struct {
  size_t body_len;
  uint64_t hash;
  bool eligible;
} ny_mach_outline_shape_t;

static bool mach_outline_shape(const ny_mach_func_t *func,
                               ny_mach_outline_shape_t *out) {
  if (!out)
    return false;
  *out = (ny_mach_outline_shape_t){0};
  size_t body_len = mach_outline_body_len(func);
  if (!body_len)
    return true;
  if (func->vreg_len > SIZE_MAX / sizeof(uint32_t))
    return false;
  uint32_t *map = func->vreg_len
                      ? malloc(func->vreg_len * sizeof(*map))
                      : NULL;
  if (func->vreg_len && !map)
    return false;
  for (size_t i = 0; i < func->vreg_len; ++i)
    map[i] = UINT32_MAX;
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  uint32_t next = 0;
  for (size_t i = 0; i < body_len; ++i) {
    const ny_mach_inst_t *inst = &func->insts[i];
    if (!mach_outline_inst_ok(func, inst)) {
      free(map);
      return true;
    }
    hash = mach_outline_mix(hash, inst->opcode);
    hash = mach_outline_mix(hash, inst->narrow32);
    hash = mach_outline_mix(hash, inst->condition);
    hash = mach_outline_mix(hash, inst->byte_width);
    const ny_mach_operand_t *operands[] = {&inst->dst, &inst->src0,
                                           &inst->src1, &inst->src2};
    for (size_t j = 0; j < 4; ++j)
      if (!mach_outline_operand_hash(func, operands[j], map, &next, &hash)) {
        free(map);
        return true;
      }
  }
  free(map);
  out->body_len = body_len;
  out->hash = hash;
  out->eligible = true;
  return true;
}

static bool mach_outline_operand_equal(const ny_mach_func_t *left,
                                       const ny_mach_operand_t *a,
                                       const ny_mach_func_t *right,
                                       const ny_mach_operand_t *b,
                                       uint32_t *left_map, uint32_t *right_map,
                                       uint32_t *left_next, uint32_t *right_next) {
  if (!a || !b || a->kind != b->kind || a->reg_class != b->reg_class ||
      a->relocation != b->relocation || a->addend != b->addend)
    return false;
  if (a->kind == NY_MACH_OPERAND_NONE)
    return true;
  if (a->kind == NY_MACH_OPERAND_IMM)
    return a->as.imm == b->as.imm;
  if (a->kind != NY_MACH_OPERAND_VREG ||
      !mach_outline_operand_ok(left, a) || !mach_outline_operand_ok(right, b))
    return false;
  uint32_t al = left_map[a->as.reg], br = right_map[b->as.reg];
  if (al == UINT32_MAX)
    left_map[a->as.reg] = al = (*left_next)++;
  if (br == UINT32_MAX)
    right_map[b->as.reg] = br = (*right_next)++;
  return al == br && left->vreg_types[a->as.reg] == right->vreg_types[b->as.reg];
}

static bool mach_outline_equal(const ny_mach_func_t *left,
                               const ny_mach_func_t *right,
                               size_t body_len) {
  if (left->vreg_len > SIZE_MAX / sizeof(uint32_t) ||
      right->vreg_len > SIZE_MAX / sizeof(uint32_t))
    return false;
  uint32_t *left_map = left->vreg_len
                           ? malloc(left->vreg_len * sizeof(*left_map))
                           : NULL;
  uint32_t *right_map = right->vreg_len
                            ? malloc(right->vreg_len * sizeof(*right_map))
                            : NULL;
  if ((left->vreg_len && !left_map) || (right->vreg_len && !right_map)) {
    free(left_map);
    free(right_map);
    return false;
  }
  for (size_t i = 0; i < left->vreg_len; ++i)
    left_map[i] = UINT32_MAX;
  for (size_t i = 0; i < right->vreg_len; ++i)
    right_map[i] = UINT32_MAX;
  uint32_t left_next = 0, right_next = 0;
  bool equal = true;
  for (size_t i = 0; equal && i < body_len; ++i) {
    const ny_mach_inst_t *a = &left->insts[i];
    const ny_mach_inst_t *b = &right->insts[i];
    if (a->opcode != b->opcode || a->narrow32 != b->narrow32 ||
        a->condition != b->condition || a->byte_width != b->byte_width)
      equal = false;
    const ny_mach_operand_t *ao[] = {&a->dst, &a->src0, &a->src1, &a->src2};
    const ny_mach_operand_t *bo[] = {&b->dst, &b->src0, &b->src1, &b->src2};
    for (size_t j = 0; equal && j < 4; ++j)
      equal = mach_outline_operand_equal(left, ao[j], right, bo[j],
                                         left_map, right_map, &left_next,
                                         &right_next);
  }
  free(left_map);
  free(right_map);
  return equal;
}

bool ny_mach_audit_size_candidates(const ny_mach_func_t *funcs,
                                   size_t func_count,
                                   ny_mach_outline_audit_t *out) {
  if (!out || (func_count && !funcs))
    return false;
  *out = (ny_mach_outline_audit_t){0};
  if (func_count > SIZE_MAX / sizeof(ny_mach_outline_shape_t))
    return false;
  ny_mach_outline_shape_t *shapes =
      func_count ? calloc(func_count, sizeof(*shapes)) : NULL;
  if (func_count && !shapes)
    return false;
  bool ok = true;
  for (size_t i = 0; i < func_count; ++i) {
    if (!mach_outline_shape(&funcs[i], &shapes[i])) {
      ok = false;
      break;
    }
    if (shapes[i].eligible)
      out->eligible_functions++;
  }
  for (size_t i = 0; ok && i < func_count; ++i) {
    if (!shapes[i].eligible)
      continue;
    for (size_t j = i + 1; j < func_count; ++j) {
      if (!shapes[j].eligible || shapes[i].body_len != shapes[j].body_len ||
          shapes[i].hash != shapes[j].hash ||
          !mach_outline_equal(&funcs[i], &funcs[j], shapes[i].body_len))
        continue;
      out->duplicate_pairs++;
      if (out->estimated_saved_insts <= SIZE_MAX - shapes[i].body_len)
        out->estimated_saved_insts += shapes[i].body_len;
    }
  }
  free(shapes);
  return ok;
}

static bool mach_outline_selftest_func(ny_mach_func_t *func, bool pad_vreg,
                                       bool relocated) {
  ny_mach_reg_t src, dst, ignored;
  uint32_t block;
  bool built = !pad_vreg ||
               ny_mach_alloc_typed_vreg(func, NY_MACH_TYPE_I64, &ignored);
  built = built &&
          ny_mach_alloc_typed_vreg(func, NY_MACH_TYPE_I64, &src) &&
          ny_mach_alloc_typed_vreg(func, NY_MACH_TYPE_I64, &dst) &&
          ny_mach_begin_block(func, 0, &block) &&
          ny_mach_emit(func, (ny_mach_inst_t){
                                .opcode = NY_MACH_COPY,
                                .dst = {.kind = NY_MACH_OPERAND_VREG,
                                        .reg_class = NY_MACH_REGCLASS_GPR,
                                        .as.reg = src},
                                .src0 = {.kind = NY_MACH_OPERAND_IMM,
                                         .as.imm = 7,
                                         .relocation = relocated
                                                           ? NY_MACH_RELOC_ABS64
                                                           : NY_MACH_RELOC_NONE}}) &&
          ny_mach_emit(func, (ny_mach_inst_t){
                                .opcode = NY_MACH_ADD,
                                .dst = {.kind = NY_MACH_OPERAND_VREG,
                                        .reg_class = NY_MACH_REGCLASS_GPR,
                                        .as.reg = dst},
                                .src0 = {.kind = NY_MACH_OPERAND_VREG,
                                         .reg_class = NY_MACH_REGCLASS_GPR,
                                         .as.reg = src},
                                .src1 = {.kind = NY_MACH_OPERAND_IMM,
                                         .as.imm = 5}}) &&
          ny_mach_emit(func, (ny_mach_inst_t){.opcode = NY_MACH_RET,
                                              .effects = NY_MACH_EFFECT_CONTROL});
  return built;
}

bool ny_mach_audit_size_candidates_selftest(void) {
  ny_mach_func_t funcs[3] = {{0}};
  ny_mach_inst_t before[3][3];
  bool built = mach_outline_selftest_func(&funcs[0], false, false) &&
               mach_outline_selftest_func(&funcs[1], true, false) &&
               mach_outline_selftest_func(&funcs[2], false, true);
  bool unchanged = built;
  if (built)
    for (size_t i = 0; i < 3; ++i) {
      if (funcs[i].inst_len != 3) {
        unchanged = false;
        break;
      }
      memcpy(before[i], funcs[i].insts, sizeof(before[i]));
    }

  ny_mach_outline_audit_t first, second;
  bool ok = built && unchanged &&
            ny_mach_audit_size_candidates(funcs, 3, &first) &&
            ny_mach_audit_size_candidates(funcs, 3, &second);
  if (ok) {
    for (size_t i = 0; i < 3; ++i)
      if (memcmp(before[i], funcs[i].insts, sizeof(before[i])) != 0)
        ok = false;
    ok = ok && first.eligible_functions == 2 && first.duplicate_pairs == 1 &&
         first.estimated_saved_insts == 2 &&
         memcmp(&first, &second, sizeof(first)) == 0;
  }
  for (size_t i = 0; i < 3; ++i)
    ny_mach_func_free(&funcs[i]);
  return ok;
}

bool ny_mach_schedule_post_ra(ny_mach_func_t *func) {
  if (!func || !func->insts || !func->blocks)
    return true;
  for (size_t block = 0; block < func->block_len; ++block)
    if (!mach_schedule_block(func, &func->blocks[block]))
      return false;
  return true;
}

static ny_mach_operand_t mach_schedule_selftest_vreg(ny_mach_reg_t reg) {
  return (ny_mach_operand_t){.kind = NY_MACH_OPERAND_VREG,
                             .reg_class = NY_MACH_REGCLASS_GPR,
                             .as.reg = reg};
}

bool ny_mach_schedule_post_ra_selftest(void) {
  ny_mach_func_t mach = {0};
  ny_mach_reg_t regs[8];
  uint32_t safe_block, call_block, branch_block, div_block, shift_block;
  bool built = true;
  for (size_t i = 0; built && i < sizeof(regs) / sizeof(regs[0]); ++i)
    built = ny_mach_alloc_vreg(&mach, NY_MACH_REGCLASS_GPR, &regs[i]);
  built = built && ny_mach_begin_block(&mach, 0, &safe_block) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_COPY, .dst = mach_schedule_selftest_vreg(regs[0]),
              .src0 = {.kind = NY_MACH_OPERAND_IMM, .as.imm = 1}}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_COPY, .dst = mach_schedule_selftest_vreg(regs[1]),
              .src0 = {.kind = NY_MACH_OPERAND_IMM, .as.imm = 4}}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_SQRT, .dst = mach_schedule_selftest_vreg(regs[2]),
              .src0 = mach_schedule_selftest_vreg(regs[1])}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_ADD, .dst = mach_schedule_selftest_vreg(regs[3]),
              .src0 = mach_schedule_selftest_vreg(regs[0]),
              .src1 = mach_schedule_selftest_vreg(regs[2])});
  built = built && ny_mach_begin_block(&mach, 1, &call_block) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_COPY, .dst = mach_schedule_selftest_vreg(regs[4]),
              .src0 = {.kind = NY_MACH_OPERAND_IMM, .as.imm = 1}}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_CALL, .effects = NY_MACH_EFFECT_CALL}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_COPY, .dst = mach_schedule_selftest_vreg(regs[5]),
              .src0 = {.kind = NY_MACH_OPERAND_IMM, .as.imm = 2}});
  built = built && ny_mach_begin_block(&mach, 2, &branch_block) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_COPY, .dst = mach_schedule_selftest_vreg(regs[4]),
              .src0 = {.kind = NY_MACH_OPERAND_IMM, .as.imm = 1}}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_SQRT, .dst = mach_schedule_selftest_vreg(regs[6]),
              .src0 = mach_schedule_selftest_vreg(regs[7])}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_BR, .effects = NY_MACH_EFFECT_CONTROL,
              .src1 = {.kind = NY_MACH_OPERAND_BLOCK,
                       .as.block_index = branch_block}});
  built = built && ny_mach_begin_block(&mach, 3, &div_block) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_COPY, .dst = mach_schedule_selftest_vreg(regs[4]),
              .src0 = {.kind = NY_MACH_OPERAND_IMM, .as.imm = 12}}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_DIV, .dst = mach_schedule_selftest_vreg(regs[5]),
              .src0 = mach_schedule_selftest_vreg(regs[4]),
              .src1 = {.kind = NY_MACH_OPERAND_IMM, .as.imm = 3}}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_SQRT, .dst = mach_schedule_selftest_vreg(regs[6]),
              .src0 = mach_schedule_selftest_vreg(regs[7])});
  built = built && ny_mach_begin_block(&mach, 4, &shift_block) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_COPY, .dst = mach_schedule_selftest_vreg(regs[4]),
              .src0 = {.kind = NY_MACH_OPERAND_IMM, .as.imm = 12}}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_SHL, .dst = mach_schedule_selftest_vreg(regs[5]),
              .src0 = mach_schedule_selftest_vreg(regs[4]),
              .src1 = {.kind = NY_MACH_OPERAND_IMM, .as.imm = 1}}) &&
          ny_mach_emit(&mach, (ny_mach_inst_t){
              .opcode = NY_MACH_SQRT, .dst = mach_schedule_selftest_vreg(regs[6]),
              .src0 = mach_schedule_selftest_vreg(regs[7])});

  ny_mach_inst_t call_before[3], branch_before[3], div_before[3], shift_before[3];
  if (built) {
    memcpy(call_before, mach.insts + mach.blocks[call_block].first_inst,
           sizeof(call_before));
    memcpy(branch_before, mach.insts + mach.blocks[branch_block].first_inst,
           sizeof(branch_before));
    memcpy(div_before, mach.insts + mach.blocks[div_block].first_inst,
           sizeof(div_before));
    memcpy(shift_before, mach.insts + mach.blocks[shift_block].first_inst,
           sizeof(shift_before));
  }
  bool ok = built && ny_mach_schedule_post_ra(NULL) &&
            ny_mach_schedule_post_ra(&mach);
  if (ok) {
    const ny_mach_inst_t *safe =
        mach.insts + mach.blocks[safe_block].first_inst;
    ok = safe[0].dst.as.reg == regs[1] && safe[1].dst.as.reg == regs[2] &&
         safe[2].dst.as.reg == regs[0] && safe[3].dst.as.reg == regs[3] &&
         memcmp(call_before, mach.insts + mach.blocks[call_block].first_inst,
                sizeof(call_before)) == 0 &&
         memcmp(branch_before, mach.insts + mach.blocks[branch_block].first_inst,
                sizeof(branch_before)) == 0 &&
         memcmp(div_before, mach.insts + mach.blocks[div_block].first_inst,
                sizeof(div_before)) == 0 &&
         memcmp(shift_before, mach.insts + mach.blocks[shift_block].first_inst,
                sizeof(shift_before)) == 0;
  }
  ny_mach_func_free(&mach);
  return ok;
}
