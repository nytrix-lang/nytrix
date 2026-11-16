/* Target-specific machine IR consumed by register allocation and encoders. */
#ifndef NYR_NATIVE_MACHINE_H
#define NYR_NATIVE_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "code/backend/core/abi.h"
#include <nyr/ir.h>

/*
 * Target-specific machine IR between NYIR and object emission. It owns virtual
 * registers, frame references, relocations, and machine-form control flow;
 * encoders consume this representation without reconstructing source meaning.
 */

typedef uint32_t nyr_mach_reg_t;

#define NYR_MACH_REG_INVALID UINT32_MAX

typedef enum {
  NYR_MACH_REGCLASS_NONE = 0,
  NYR_MACH_REGCLASS_GPR,
  NYR_MACH_REGCLASS_FPR,
  NYR_MACH_REGCLASS_VECTOR,
  NYR_MACH_REGCLASS_FLAGS,
} nyr_mach_reg_class_t;

typedef enum {
  NYR_MACH_TYPE_NONE = 0,
  NYR_MACH_TYPE_I64,
  NYR_MACH_TYPE_F32,
  NYR_MACH_TYPE_F64,
  NYR_MACH_TYPE_PTR,
  NYR_MACH_TYPE_V128_I64,
  NYR_MACH_TYPE_V128_F64,
  NYR_MACH_TYPE_V128_F32,
  NYR_MACH_TYPE_V256_I64,
  NYR_MACH_TYPE_V256_F64,
  NYR_MACH_TYPE_FLAGS,
} nyr_mach_type_t;

#define NYR_MACH_TYPE_V128 NYR_MACH_TYPE_V128_I64

typedef enum {
  NYR_MACH_OPERAND_NONE = 0,
  NYR_MACH_OPERAND_VREG,
  NYR_MACH_OPERAND_PREG,
  NYR_MACH_OPERAND_IMM,
  NYR_MACH_OPERAND_FRAME,
  NYR_MACH_OPERAND_BLOCK,
  NYR_MACH_OPERAND_SYMBOL,
} nyr_mach_operand_kind_t;

typedef enum {
  NYR_MACH_RELOC_NONE = 0,
  NYR_MACH_RELOC_ABS64,
  NYR_MACH_RELOC_PC_REL32,
  NYR_MACH_RELOC_CALL,
} nyr_mach_reloc_kind_t;

typedef enum {
  NYR_MACH_COND_ALWAYS = 0,
  NYR_MACH_COND_EQ,
  NYR_MACH_COND_NE,
  NYR_MACH_COND_LT,
  NYR_MACH_COND_LE,
  NYR_MACH_COND_GT,
  NYR_MACH_COND_GE,
  NYR_MACH_COND_B,
  NYR_MACH_COND_AE,
  NYR_MACH_COND_BE,
  NYR_MACH_COND_A,
} nyr_mach_cond_t;

typedef struct {
  nyr_mach_operand_kind_t kind;
  nyr_mach_reg_class_t reg_class;
  union {
    nyr_mach_reg_t reg;
    int64_t imm;
    uint32_t frame_index;
    uint32_t block_index;
    const char *symbol;
  } as;
  nyr_mach_reloc_kind_t relocation;
  int64_t addend;
} nyr_mach_operand_t;

typedef enum {
  NYR_MACH_NOP = 0,
  NYR_MACH_COPY,
  NYR_MACH_CONVERT,
  NYR_MACH_LOAD,
  NYR_MACH_STORE,
  NYR_MACH_LEA,
  NYR_MACH_ADD,
  NYR_MACH_SUB,
  NYR_MACH_MUL,
  NYR_MACH_DIV,
  NYR_MACH_MOD,
  NYR_MACH_AND,
  NYR_MACH_OR,
  NYR_MACH_XOR,
  NYR_MACH_SHL,
  NYR_MACH_SAR,
  /* Scalar logical i64 shift right. */
  NYR_MACH_SHR,
  NYR_MACH_ROR,
  NYR_MACH_ROR32,
  NYR_MACH_BSWAP64,
  NYR_MACH_BSWAP32,
  NYR_MACH_FMA,
  NYR_MACH_SQRT,
  NYR_MACH_SIN,
  NYR_MACH_COS,
  NYR_MACH_SHUFFLE,
  NYR_MACH_SELECT,
  NYR_MACH_CMP,
  NYR_MACH_CALL,
  NYR_MACH_RET,
  NYR_MACH_BR,
  NYR_MACH_BR_IF,
  NYR_MACH_TRAP,
  NYR_MACH_INLINE_ASM,
  NYR_MACH_INTRINSIC,
  NYR_MACH_REDUCE_ADD,
  NYR_MACH_VA_SAVE,
  NYR_MACH_VA_START,
  NYR_MACH_VA_ARG,
  /* Packed logical i64 shift right by an immediate (V256_I64 only). */
  NYR_MACH_VSHR,
  /* POPCNT of a value already masked to 32 bits. */
  NYR_MACH_POPCOUNT64,
} nyr_mach_opcode_t;

typedef enum {
  NYR_MACH_EFFECT_NONE = 0,
  NYR_MACH_EFFECT_READ_MEMORY = 1u << 0,
  NYR_MACH_EFFECT_WRITE_MEMORY = 1u << 1,
  NYR_MACH_EFFECT_CALL = 1u << 2,
  NYR_MACH_EFFECT_CONTROL = 1u << 3,
} nyr_mach_effect_t;

typedef struct {
  nyr_mach_opcode_t opcode;
  nyr_mach_operand_t dst;
  nyr_mach_operand_t src0;
  nyr_mach_operand_t src1;
  nyr_mach_operand_t src2;
  int64_t immediate;
  nyr_mach_operand_t *args;
  uint32_t *arg_sizes;
  size_t args_len;
  bool call_is_extern;
  bool call_has_sret;
  bool call_is_variadic;
  /* Resolved once during NYIR legalization; machine encoders must not
   * rediscover runtime operations from symbol names. */
  ny_native_runtime_op_t runtime_operation;
  nyr_mach_cond_t condition;
  uint64_t call_clobbers;
  /* Set for every sub-word access, so width-unaware folds stay away. */
  bool byte_width;
  /* Access size when byte_width is set: 0/1 byte, 2 = u16, 4 = sign-extended
   * i32, 5 = zero-extended u32. Only the object encoder implements 2 to 5. */
  uint8_t mem_size;
  /* SAR by 32 of a value whose low word is sign-extended: movsxd. */
  bool sext32;
  /* LOAD sign-extends its sub-word access to 64 bits: movsx. */
  bool sext_load;
  /* DIV/MOD treat both operands as unsigned 64-bit values. */
  bool unsigned_arith;
  bool narrow32;
  /* DIV/MOD dividend is proven in [0, 2^31). */
  bool dividend_u31;
  unsigned effects;
  uint32_t source_pc;
} nyr_mach_inst_t;

typedef struct {
  uint32_t first_inst;
  uint32_t inst_count;
  uint32_t label;
  uint32_t source_pc;
} nyr_mach_block_t;

typedef struct {
  size_t size;
  size_t align;
  nyr_mach_type_t type;
  bool address_taken;
} nyr_mach_frame_slot_t;

typedef struct {
  nyr_mach_inst_t *insts;
  size_t inst_len;
  size_t inst_cap;
  nyr_mach_block_t *blocks;
  size_t block_len;
  size_t block_cap;
  nyr_mach_frame_slot_t *frame_slots;
  size_t frame_slot_len;
  size_t param_count;
  /* Function-entry C ABI facts retained for target prologues. These arrays
   * exist only when at least one parameter is an aggregate. The home-slot
   * index is UINT32_MAX for scalar parameters and points to the first
   * eightbyte slot for aggregate parameters. */
  nyr_param_type_t *param_types;
  uint32_t *param_abi;
  uint32_t *param_aggregate_home_slots;
  uint32_t return_aggregate_size;
  uint8_t return_aggregate_classes[2];
  size_t frame_slot_cap;
  nyr_mach_reg_class_t *vreg_classes;
  nyr_mach_type_t *vreg_types;
  size_t vreg_len;
  size_t vreg_cap;
  uint32_t *home_slots;
  size_t home_slot_count;
  /* C translation unit: plain `main`, no language runtime startup. */
  bool c_abi;
  /* Largest block label so far; labels above it need no duplicate scan. */
  uint32_t max_block_label;
} nyr_mach_func_t;

void nyr_mach_func_free(nyr_mach_func_t *func);
/* Marks parameter slots in a caller-zeroed frame_slot_len bitmap. */
bool nyr_mach_param_slots(const nyr_mach_func_t *mach, bool *params);

/* SysV aggregates consume either all required registers or one stack area.
 * Register counters are advanced only for a register-passed aggregate. */
typedef struct {
  uint32_t size;
  unsigned chunks;
  unsigned classes[2];
  bool in_registers;
} nyr_mach_sysv_aggregate_t;
bool nyr_mach_sysv_aggregate_parameter(uint32_t packed, size_t gp_limit,
                                       size_t fp_limit, size_t *gp, size_t *fp,
                                       nyr_mach_sysv_aggregate_t *out);

bool nyr_mach_alloc_vreg(nyr_mach_func_t *func, nyr_mach_reg_class_t reg_class,
                         nyr_mach_reg_t *out);
bool nyr_mach_alloc_typed_vreg(nyr_mach_func_t *func, nyr_mach_type_t type,
                               nyr_mach_reg_t *out);
bool nyr_mach_alloc_frame_slot(nyr_mach_func_t *func, size_t size, size_t align,
                               bool address_taken, uint32_t *out);
bool nyr_mach_alloc_typed_frame_slot(nyr_mach_func_t *func, size_t size,
                                     size_t align, nyr_mach_type_t type,
                                     bool address_taken, uint32_t *out);
bool nyr_mach_begin_block(nyr_mach_func_t *func, uint32_t label, uint32_t *out);
/* Takes ownership of inst.args on success; the caller owns it on failure. */
bool nyr_mach_emit(nyr_mach_func_t *func, nyr_mach_inst_t inst);
bool nyr_mach_verify(const nyr_mach_func_t *func, unsigned caps, char *err,
                     size_t err_len);
/* Return the verified x64 allocator-color clobbers for one machine op. */
/* Color mask an instruction clobbers through fixed registers or calls, for a
 * function the caller has already verified. Same contract as below. */
uint64_t nyr_mach_inst_fixed_clobbers(const nyr_mach_func_t *mach,
                                      const nyr_mach_inst_t *in,
                                      nyr_mach_reg_class_t reg_class);
bool nyr_mach_regalloc_fixed_clobbers(const nyr_mach_func_t *func, size_t pc,
                                      nyr_mach_reg_class_t reg_class,
                                      bool preserve_gpr_calls, uint64_t *out);
const char *nyr_mach_opcode_name(nyr_mach_opcode_t op);
void nyr_mach_dump(const nyr_mach_func_t *func, FILE *out);
/*
 * Correlated dump: NYIR source group -> MIR inst -> native bytes.  When
 * `mach` is set, `inst_offsets[pc]` must be the machine-encoder map from MIR
 * instruction pc to function-relative native offset (SIZE_MAX if unmapped).
 * When `mach` is NULL the map indexes the NYIR encoder instead.  Requires
 * nyr_set_mixed_disasm() to be enabled; no-op otherwise.
 */
void nyr_mach_set_mixed_note(const char *reason);
void nyr_mach_dump_mixed(const nyr_func_t *nyir, const nyr_mach_func_t *mach,
                         const unsigned char *native_code, size_t native_len,
                         const size_t *inst_offsets, size_t inst_offsets_len,
                         const char *name, FILE *out);
/* Enables --mixed-disasm output for the bundler; filter may be NULL/empty. */
void nyr_set_mixed_disasm(bool enabled, const char *filter);
bool nyr_mach_schedule_post_ra(nyr_mach_func_t *func);
bool nyr_mach_schedule_post_ra_selftest(void);
bool nyr_mach_sink_edge_adds_selftest(void);

typedef struct {
  size_t eligible_functions;
  size_t duplicate_pairs;
  size_t estimated_saved_insts;
} nyr_mach_outline_audit_t;

bool nyr_mach_audit_size_candidates(const nyr_mach_func_t *funcs,
                                    size_t func_count,
                                    nyr_mach_outline_audit_t *out);
bool nyr_mach_audit_size_candidates_selftest(void);
bool nyr_mach_lower_nir(const nyr_func_t *nyir, nyr_mach_func_t *out,
                        unsigned caps, char *err, size_t err_len);
bool nyr_mach_opcode_supported(nyr_op_t op);
void nyr_mach_opcode_coverage(size_t *supported, size_t *total);

typedef enum {
  NYR_MACH_SPILL_NONE = 0,
  NYR_MACH_SPILL_FIXED_CLOBBER,
  NYR_MACH_SPILL_LIVE_OUT,
  NYR_MACH_SPILL_SEGMENT_BOUND,
  NYR_MACH_SPILL_REGISTER_PRESSURE,
  NYR_MACH_SPILL_BLOCK_CONFLICT,
} nyr_mach_spill_reason_t;

typedef struct {
  nyr_mach_spill_reason_t spill_reason;
  size_t seg;
  uint32_t vreg;
  uint32_t block;
  size_t start;
  size_t end;
  int color;
  bool reload;
  bool spill;
  bool carried;
  bool acyclic_carried;
} nyr_mach_live_segment_t;

typedef struct {
  nyr_mach_live_segment_t *segments;
  size_t segment_len;
  size_t segment_cap;
  size_t *vreg_offsets;
  size_t vreg_len;
  size_t color_count;
  size_t bit_words;
  size_t *live_in;
  size_t *live_out;
  size_t block_count;
  bool x64_rhs_tie;
} nyr_mach_regalloc_t;

bool nyr_mach_regalloc_build(const nyr_mach_func_t *mach, size_t color_count,
                             nyr_mach_regalloc_t *out);
bool nyr_mach_regalloc_build_callee_saved_gpr(const nyr_mach_func_t *mach,
                                              size_t color_count,
                                              nyr_mach_regalloc_t *out);
bool nyr_mach_regalloc_build_class(const nyr_mach_func_t *mach,
                                   nyr_mach_reg_class_t reg_class,
                                   size_t color_count,
                                   nyr_mach_regalloc_t *out);
/* x86 two-address encoders can preserve selected dying rhs operands. */
bool nyr_mach_regalloc_build_x64_class(const nyr_mach_func_t *mach,
                                       nyr_mach_reg_class_t reg_class,
                                       size_t color_count,
                                       nyr_mach_regalloc_t *out);
size_t nyr_mach_regalloc_recolor_locals(
    const nyr_mach_func_t *mach, nyr_mach_regalloc_t *alloc,
    nyr_mach_reg_class_t reg_class, const unsigned char *candidate,
    bool (*color_blocked)(void *ctx, int color, size_t start, size_t end),
    void *ctx);
bool nyr_mach_regalloc_verify(const nyr_mach_func_t *mach,
                              const nyr_mach_regalloc_t *alloc,
                              nyr_mach_reg_class_t reg_class);
void nyr_mach_regalloc_free(nyr_mach_regalloc_t *alloc);
const nyr_mach_live_segment_t *
nyr_mach_regalloc_segment_at(const nyr_mach_regalloc_t *alloc, uint32_t vreg,
                             size_t inst);
bool nyr_mach_regalloc_live_in(const nyr_mach_regalloc_t *alloc, size_t block,
                               uint32_t vreg);
bool nyr_mach_regalloc_live_out(const nyr_mach_regalloc_t *alloc, size_t block,
                                uint32_t vreg);
size_t nyr_mach_regalloc_peak_live(const nyr_mach_regalloc_t *alloc,
                                   size_t inst_len);
bool nyr_mach_linear_scan(nyr_mach_func_t *mach, int *colors_out,
                          size_t colors_cap);
bool nyr_mach_linear_scan_ranges(nyr_mach_func_t *mach, int *colors_out,
                                 size_t *live_start, size_t *live_end,
                                 size_t colors_cap);
bool nyr_isle_apply_mach(nyr_mach_func_t *mach);

#endif
