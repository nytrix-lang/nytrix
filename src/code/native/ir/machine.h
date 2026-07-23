#ifndef NY_NATIVE_MACHINE_H
#define NY_NATIVE_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "code/native/ir.h"

/*
 * ABI-neutral machine IR boundary.  NYIR is semantic SSA; this layer owns
 * target selection, virtual registers, frame references, and relocations.
 * It intentionally has no encoder-facing fields so asm, object, and JIT
 * serializers can consume the same selected instruction stream later.
 */

typedef uint32_t ny_mach_reg_t;

#define NY_MACH_REG_INVALID UINT32_MAX

typedef enum {
  NY_MACH_REGCLASS_NONE = 0,
  NY_MACH_REGCLASS_GPR,
  NY_MACH_REGCLASS_FPR,
  NY_MACH_REGCLASS_VECTOR,
  NY_MACH_REGCLASS_FLAGS,
} ny_mach_reg_class_t;

/* Value identity is deliberately separate from register class.  Both f32 and
 * f64 use the FPR class, for example, but ABI lowering and instruction
 * selection must still know which width they carry. */
typedef enum {
  NY_MACH_TYPE_NONE = 0,
  NY_MACH_TYPE_I64,
  NY_MACH_TYPE_F32,
  NY_MACH_TYPE_F64,
  NY_MACH_TYPE_PTR,
  /* 128-bit vectors distinguished by element kind so encoders select
   * paddq vs addpd vs addps (and NEON 2d/4s) without guessing. */
  NY_MACH_TYPE_V128_I64,
  NY_MACH_TYPE_V128_F64,
  NY_MACH_TYPE_V128_F32,
  NY_MACH_TYPE_FLAGS,
} ny_mach_type_t;

/* Compatibility alias: untyped V128 means packed i64 (historical default). */
#define NY_MACH_TYPE_V128 NY_MACH_TYPE_V128_I64

typedef enum {
  NY_MACH_OPERAND_NONE = 0,
  NY_MACH_OPERAND_VREG,
  NY_MACH_OPERAND_PREG,
  NY_MACH_OPERAND_IMM,
  NY_MACH_OPERAND_FRAME,
  NY_MACH_OPERAND_BLOCK,
  NY_MACH_OPERAND_SYMBOL,
} ny_mach_operand_kind_t;

typedef enum {
  NY_MACH_RELOC_NONE = 0,
  NY_MACH_RELOC_ABS64,
  NY_MACH_RELOC_PC_REL32,
  NY_MACH_RELOC_CALL,
} ny_mach_reloc_kind_t;

typedef enum {
  NY_MACH_COND_ALWAYS = 0,
  NY_MACH_COND_EQ,
  NY_MACH_COND_NE,
  NY_MACH_COND_LT,
  NY_MACH_COND_LE,
  NY_MACH_COND_GT,
  NY_MACH_COND_GE,
} ny_mach_cond_t;

typedef struct {
  ny_mach_operand_kind_t kind;
  ny_mach_reg_class_t reg_class;
  union {
    ny_mach_reg_t reg;
    int64_t imm;
    uint32_t frame_index;
    uint32_t block_index;
    const char *symbol;
  } as;
  ny_mach_reloc_kind_t relocation;
  int64_t addend;
} ny_mach_operand_t;

typedef enum {
  NY_MACH_NOP = 0,
  NY_MACH_COPY,
  NY_MACH_CONVERT,
  NY_MACH_LOAD,
  NY_MACH_STORE,
  NY_MACH_LEA,
  NY_MACH_ADD,
  NY_MACH_SUB,
  NY_MACH_MUL,
  NY_MACH_DIV,
  NY_MACH_MOD,
  NY_MACH_AND,
  NY_MACH_OR,
  NY_MACH_XOR,
  NY_MACH_SHL,
  NY_MACH_SAR,
  NY_MACH_FMA,     /* dst = src0 * src1 + src2 (scalar or packed) */
  NY_MACH_SHUFFLE, /* dst = shuffle(src0, imm in src1.IMM) */
  NY_MACH_CMP,
  NY_MACH_CALL,
  NY_MACH_RET,
  NY_MACH_BR,
  NY_MACH_BR_IF,
  /* Target-legal placeholders enabled only when the backend advertises the
   * matching capability. Selection must not invent encodings without a gate. */
  NY_MACH_INLINE_ASM,
  NY_MACH_INTRINSIC,
} ny_mach_opcode_t;

typedef enum {
  NY_MACH_EFFECT_NONE = 0,
  NY_MACH_EFFECT_READ_MEMORY = 1u << 0,
  NY_MACH_EFFECT_WRITE_MEMORY = 1u << 1,
  NY_MACH_EFFECT_CALL = 1u << 2,
  NY_MACH_EFFECT_CONTROL = 1u << 3,
} ny_mach_effect_t;

typedef struct {
  ny_mach_opcode_t opcode;
  ny_mach_operand_t dst;
  ny_mach_operand_t src0;
  ny_mach_operand_t src1;
  ny_mach_operand_t src2;
  /* Owned by the instruction. Calls use this for their complete ordered
   * argument list; src0 remains the callee symbol/target. */
  ny_mach_operand_t *args;
  /* Per-argument ABI metadata preserved from NYIR. A zero entry means a
   * normal scalar argument; non-zero entries describe an aggregate ABI
   * payload. Owned by the instruction alongside args. */
  uint32_t *arg_sizes;
  size_t args_len;
  bool call_is_extern;
  bool call_has_sret;
  ny_mach_cond_t condition;
  /* Bitset of physical registers clobbered by a call. Target lowering owns
   * its register numbering; zero is valid for non-call instructions. */
  uint64_t call_clobbers;
  unsigned effects;
} ny_mach_inst_t;

typedef struct {
  uint32_t first_inst;
  uint32_t inst_count;
  uint32_t label;
} ny_mach_block_t;

typedef struct {
  size_t size;
  size_t align;
  ny_mach_type_t type;
  bool address_taken;
} ny_mach_frame_slot_t;

typedef struct {
  ny_mach_inst_t *insts;
  size_t inst_len;
  size_t inst_cap;
  ny_mach_block_t *blocks;
  size_t block_len;
  size_t block_cap;
  ny_mach_frame_slot_t *frame_slots;
  size_t frame_slot_len;
  size_t param_count;
  size_t frame_slot_cap;
  ny_mach_reg_class_t *vreg_classes;
  ny_mach_type_t *vreg_types;
  size_t vreg_len;
  size_t vreg_cap;
} ny_mach_func_t;

void ny_mach_func_free(ny_mach_func_t *func);
bool ny_mach_alloc_vreg(ny_mach_func_t *func, ny_mach_reg_class_t reg_class,
                       ny_mach_reg_t *out);
bool ny_mach_alloc_typed_vreg(ny_mach_func_t *func, ny_mach_type_t type,
                             ny_mach_reg_t *out);
bool ny_mach_alloc_frame_slot(ny_mach_func_t *func, size_t size, size_t align,
                             bool address_taken, uint32_t *out);
bool ny_mach_alloc_typed_frame_slot(ny_mach_func_t *func, size_t size,
                                   size_t align, ny_mach_type_t type,
                                   bool address_taken, uint32_t *out);
bool ny_mach_begin_block(ny_mach_func_t *func, uint32_t label, uint32_t *out);
/* Takes ownership of inst.args on success; the caller keeps ownership on
 * failure. Other operands are immediate values. */
bool ny_mach_emit(ny_mach_func_t *func, ny_mach_inst_t inst);
bool ny_mach_verify(const ny_mach_func_t *func, char *err, size_t err_len);
const char *ny_mach_opcode_name(ny_mach_opcode_t op);
void ny_mach_dump(const ny_mach_func_t *func, FILE *out);
bool ny_mach_lower_nir(const nyir_func_t *nyir, ny_mach_func_t *out,
                      char *err, size_t err_len);
bool ny_mach_opcode_supported(nyir_op_t op);
void ny_mach_opcode_coverage(size_t *supported, size_t *total);
typedef struct {
  uint32_t vreg;
  uint32_t block;
  size_t start;
  size_t end;
  int color;
  bool reload;
  bool spill;
} ny_mach_live_segment_t;

typedef struct {
  ny_mach_live_segment_t *segments;
  size_t segment_len;
  size_t *vreg_offsets;
  size_t vreg_len;
  size_t color_count;
  size_t bit_words;
  size_t *live_in;
  size_t *live_out;
  size_t block_count;
} ny_mach_regalloc_t;

/* CFG-aware split linear scan. Stack homes are canonical at joins and split
 * boundaries; single-predecessor forward edges may keep the same register. */
bool ny_mach_regalloc_build(const ny_mach_func_t *mach, size_t color_count,
                            ny_mach_regalloc_t *out);
void ny_mach_regalloc_free(ny_mach_regalloc_t *alloc);
const ny_mach_live_segment_t *
ny_mach_regalloc_segment_at(const ny_mach_regalloc_t *alloc, uint32_t vreg,
                            size_t inst);
bool ny_mach_regalloc_live_in(const ny_mach_regalloc_t *alloc, size_t block,
                              uint32_t vreg);
bool ny_mach_regalloc_live_out(const ny_mach_regalloc_t *alloc, size_t block,
                               uint32_t vreg);

/* Compatibility summary: returns the longest colored segment per vreg. */
bool ny_mach_linear_scan(ny_mach_func_t *mach, int *colors_out, size_t colors_cap);
bool ny_mach_linear_scan_ranges(ny_mach_func_t *mach, int *colors_out,
                                size_t *live_start, size_t *live_end,
                                size_t colors_cap);
/** Shared ISLE identity folds on machine form (bytes path; same table as NYIR). */
bool ny_isle_apply_mach(ny_mach_func_t *mach);

#endif
