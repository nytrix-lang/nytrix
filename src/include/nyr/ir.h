#ifndef NYR_IR_H
#define NYR_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Language-neutral ownership fact transported on NYIR values and call args.
 * Frontends may derive it from their own type systems; backends consume the
 * explicit fact without depending on a source-language AST. */
typedef enum ny_sem_ownership_t {
  NY_SEM_OWN_UNKNOWN = 0,
  NY_SEM_OWN_BORROWED,
  NY_SEM_OWN_OWNED,
  NY_SEM_OWN_SHARED,
} ny_sem_ownership_t;

/* Stable opaque identity for a function symbol while NYIR is interpreted.
 * Keep it within the payload bits available to the dynamic-callable marker. */
static inline uintptr_t nyr_vm_symbol_handle(const char *symbol) {
  if (!symbol || !*symbol)
    return 0;
  uint64_t hash = UINT64_C(1469598103934665603);
  for (const unsigned char *p = (const unsigned char *)symbol; *p; ++p)
    hash = (hash ^ *p) * UINT64_C(1099511628211);
  uint64_t mask = sizeof(uintptr_t) >= 8 ? ((UINT64_C(1) << 57) - 1)
                                         : ((UINT64_C(1) << 30) - 1);
  uintptr_t handle = (uintptr_t)(hash & mask);
  return handle ? handle : 1;
}

static inline uintptr_t nyr_vm_prefixed_symbol_handle(const char *prefix,
                                                       const char *symbol) {
  if (!prefix || !symbol)
    return 0;
  uint64_t hash = UINT64_C(1469598103934665603);
  for (const unsigned char *p = (const unsigned char *)prefix; *p; ++p)
    hash = (hash ^ *p) * UINT64_C(1099511628211);
  for (const unsigned char *p = (const unsigned char *)symbol; *p; ++p)
    hash = (hash ^ *p) * UINT64_C(1099511628211);
  uint64_t mask = sizeof(uintptr_t) >= 8 ? ((UINT64_C(1) << 57) - 1)
                                         : ((UINT64_C(1) << 30) - 1);
  uintptr_t handle = (uintptr_t)(hash & mask);
  return handle ? handle : 1;
}

typedef enum {
  NYIR_NOP = 0,
  NYIR_CONST_I64,
  NYIR_COPY,
  /* SSA join. Incoming pairs are owned by the instruction. */
  NYIR_PHI,
  NYIR_ADD_I64,
  NYIR_SUB_I64,
  NYIR_MUL_I64,
  NYIR_DIV_I64,
  NYIR_MOD_I64,
  NYIR_AND_I64,
  NYIR_OR_I64,
  NYIR_XOR_I64,
  NYIR_SHL_I64,
  NYIR_SAR_I64,
  NYIR_ROR_I64,
  NYIR_CMP_I64,
  NYIR_LABEL,
  NYIR_LOAD_LOCAL,
  NYIR_STORE_LOCAL,
  NYIR_CALL,
  NYIR_RET,
  NYIR_BR,
  NYIR_BR_IF,
  NYIR_CONST_F64,
  NYIR_ADD_F64,
  NYIR_SUB_F64,
  NYIR_MUL_F64,
  NYIR_DIV_F64,
  NYIR_I64_TO_F64,
  NYIR_F64_TO_I64,
  NYIR_F32_TO_I64,
  NYIR_CMP_F64,
  NYIR_SQRT_F64,
  NYIR_CONST_F32,
  NYIR_ADD_F32,
  NYIR_SUB_F32,
  NYIR_MUL_F32,
  NYIR_DIV_F32,
  NYIR_I64_TO_F32,
  NYIR_F64_TO_F32,
  NYIR_F32_TO_F64,
  NYIR_CMP_F32,
  NYIR_ADDR_LOCAL,
  NYIR_LOAD_I64,
  NYIR_STORE_I64,
  NYIR_ADDR_SYMBOL, /* leaq symbol(%rip), dst — RIP-relative address of a named
                       symbol */
  NYIR_ALLOCA,      /* allocate stack space for byval/sret */
  NYIR_COPY_STRUCT, /* copy aggregate data */
  NYIR_CAPTURE_RET, /* capture a secondary ABI return register */
  /* SIMD vec4 packed f64 (maps to AVX2 ymm or SSE2 xmm pairs) */
  NYIR_VEC4_LOAD_F64,    /* dst = vec4_load(addr) */
  NYIR_VEC4_STORE_F64,   /* vec4_store(addr, val) */
  NYIR_VEC4_ADD_F64,     /* dst = a + b (packed) */
  NYIR_VEC4_SUB_F64,     /* dst = a - b (packed) */
  NYIR_VEC4_MUL_F64,     /* dst = a * b (packed) */
  NYIR_VEC4_DIV_F64,     /* dst = a / b (packed) */
  NYIR_VEC4_FMA_F64,     /* dst = a * b + c (packed fused multiply-add) */
  NYIR_VEC4_SET1_F64,    /* dst = {a, a, a, a} (broadcast) */
  NYIR_VEC4_SHUFFLE_F64, /* dst = shuffle(a, imm8) */
  /* SIMD vec8 packed f32 (maps to AVX2 ymm or SSE xmm) */
  NYIR_VEC8_LOAD_F32,
  NYIR_VEC8_STORE_F32,
  NYIR_VEC8_ADD_F32,
  NYIR_VEC8_SUB_F32,
  NYIR_VEC8_MUL_F32,
  NYIR_VEC8_DIV_F32,
  NYIR_VEC8_FMA_F32,
  NYIR_VEC8_SET1_F32,
  NYIR_VEC8_SHUFFLE_F32,
  /* SIMD vec4 packed i64 */
  NYIR_VEC4_LOAD_I64,
  NYIR_VEC4_STORE_I64,
  NYIR_VEC4_ADD_I64,
  NYIR_VEC4_SUB_I64,
  NYIR_VEC4_AND_I64,
  NYIR_VEC4_OR_I64,
  NYIR_VEC4_XOR_I64,
  NYIR_VEC4_SHL_I64,
  NYIR_VEC4_SAR_I64,
  NYIR_VEC4_REDUCE_ADD_I64, /* dst = scalar a + sum of four lanes in b */
  /* SIMD vec8 packed i64 (256-bit, 4--i64).  Verifier/type-map/vectorizer
   * are fully wired.  Object encoders lower these through the NIR-fallback
   * path until native ymm (x86-64) / SVE (AArch64) emission is added. */
  NYIR_VEC8_LOAD_I64,
  NYIR_VEC8_STORE_I64,
  NYIR_VEC8_ADD_I64,
  NYIR_VEC8_SUB_I64,
  NYIR_VEC8_AND_I64,
  NYIR_VEC8_OR_I64,
  NYIR_VEC8_XOR_I64,
  NYIR_VEC8_REDUCE_ADD_I64, /* dst = scalar a + sum of eight lanes in b */
  /* Bounds check: verifies addr + offset within [base, base+len).
   * .a = base pointer, .b = offset, .imm = byte length.
   * Elided at lowering time when the index type is Fin<N> and
   * N matches a comptime-known buffer length. */
  NYIR_BOUNDS_CHECK,
  NYIR_SIN_F64,
  NYIR_COS_F64,
  /* Appended to preserve serialized opcode numbers from older NYIR binaries. */
  NYIR_VEC4_REDUCE_ADD_F64, /* dst = scalar a + sum of packed lanes in b */
  NYIR_VEC4_SET1_I64,       /* dst = {a, a} (128-bit i64 broadcast) */
  /* Evaluated i64 boolean: dst = a ? b : c. */
  NYIR_SELECT_I64,
  NYIR_ROR32_I64,
  NYIR_BSWAP_I64,
  NYIR_BSWAP32_I64,
  NYIR_OP_COUNT,
} nyr_op_t;

typedef enum {
  NYIR_CMP_EQ = 0,
  NYIR_CMP_NE,
  NYIR_CMP_LT,
  NYIR_CMP_LE,
  NYIR_CMP_GT,
  NYIR_CMP_GE,
} nyr_cmp_t;

typedef enum {
  NYIR_EFFECT_NONE = 0,
  NYIR_EFFECT_READ_LOCAL = 1u << 0,
  NYIR_EFFECT_WRITE_LOCAL = 1u << 1,
  NYIR_EFFECT_CALL = 1u << 2,
  NYIR_EFFECT_CONTROL = 1u << 3,
  /*
   * Memory effects are intentionally location- and extent-agnostic. READ
   * means a call may observe any existing memory, including storage reachable
   * from its arguments; WRITE means it may modify any such memory over an
   * unknown extent. No alias set, pointer argument, or length proof is carried
   * by these bits. Optimizers must treat them as global memory barriers unless
   * they independently prove the candidate location disjoint.
   */
  NYIR_EFFECT_READ_MEMORY = 1u << 4,
  NYIR_EFFECT_WRITE_MEMORY = 1u << 5,
  NYIR_EFFECT_MAY_TRAP = 1u << 6,
  NYIR_EFFECT_VOLATILE = 1u << 7,
  NYIR_EFFECT_ALLOCATION = 1u << 8,
  NYIR_EFFECT_UNKNOWN_SIDE_EFFECT = 1u << 9,
  NYIR_EFFECT_IO = 1u << 10,
  NYIR_EFFECT_THREAD = 1u << 11,
  NYIR_EFFECT_FFI = 1u << 12,
  NYIR_EFFECT_FENV = 1u << 13,
} nyr_effect_t;

#define NYIR_INST_F_EXTERN 1u
#define NYIR_INST_F_SRET 8u
#define NYIR_INST_F_MEM_BYTE 32u
/* Set by nyr_narrow() on scalar-int ALU ops with a proven non-negative u32
 * result, exact `& 0xffffffff` masks, and operations in a low-word expression
 * whose uses are all consumed modulo 2^32. The emitter may use 32-bit
 * registers, which zero-extend the result. For range-proven operations this
 * preserves the full value; inside a low-word region it preserves the value
 * observed at the mask boundary. Do not set on a potentially negative value
 * unless a proven mask makes the 32-bit result exact. */
#define NYIR_INST_F_NARROW32 64u
/*
 * Set on NYIR_CALL by the native builder after interprocedural effect
 * inference when the callee is a user function whose full observable effect
 * set is known (see nyr_patch_call_effects).  When set,
 * nyr_effective_effects() trusts inst->effects instead of the
 * symbol-table default (CALL|FFI|UNKNOWN_SIDE_EFFECT), which lets LICM and
 * friends treat provably pure user calls like any other pure operation.
 */
#define NYIR_INST_F_EFFECTS_KNOWN 128u

/* The strict ownership checker proved this call returns an exclusive borrow
 * of exactly one argument. The verifier checks the call's return-borrow
 * contract before accepting the fact. */
#define NYIR_INST_F_NOALIAS 2048u

/*
 * Set on integer ALU operations (add/sub/mul/shl) when range analysis proves
 * the operation cannot overflow. Allows codegen to emit non-checking variants.
 */
#define NYIR_INST_F_NO_OVERFLOW 256u
/* Address value produced by a compiler-lowered typed-buffer index after the
 * index has been checked/proved in bounds.  The proof survives later BCE so
 * escape/SROA passes can distinguish payload addressing from arbitrary raw
 * pointer arithmetic without retaining the runtime check. */
#define NYIR_INST_F_IN_BOUNDS_ADDR 512u
/* Temporary marker used only while nyr_mem2reg renames its own PHIs. */
#define NYIR_INST_F_MEM2REG_PHI 1024u

/* Packed NYIR_CALL aggregate-argument metadata. */
#define NYIR_ARG_AGG_SIZE_MASK 0x00ffffffu
#define NYIR_ARG_AGG_CLASS0_SHIFT 24u
#define NYIR_ARG_AGG_CLASS1_SHIFT 28u
#define NYIR_ARG_AGG_CLASS_MASK 0x0fu
#define NYIR_ARG_CLASS_NONE 0u
#define NYIR_ARG_CLASS_INTEGER 1u
#define NYIR_ARG_CLASS_SSE 2u
#define NYIR_ARG_CLASS_MEMORY 3u
#define NYIR_ARG_CLASS_UNSUPPORTED 4u
#define NYIR_ARG_CLASS_HFA_F32 5u
#define NYIR_ARG_CLASS_HFA_F64 6u
#define NYIR_ARG_CLASS_HVA_V128 7u
#define NYIR_ARG_CLASS_AAPCS_INTEGER_A16 8u
#define NYIR_ARG_AGG_SIZE(v) ((v) & NYIR_ARG_AGG_SIZE_MASK)
#define NYIR_ARG_AGG_CLASS(v, n)                                               \
  (((v) >> ((n) ? NYIR_ARG_AGG_CLASS1_SHIFT : NYIR_ARG_AGG_CLASS0_SHIFT)) &    \
   NYIR_ARG_AGG_CLASS_MASK)

/* Calls with more than 6 args carry args[6..] out-of-line in extra_args,
 * covering the SysV/Win64 stack-passed portion of the call ABI. The cap is
 * an implementation sanity bound, not an ABI limit. */
#define NYIR_CALL_MAX_ARGS 64

/* Formal ownership disposition attached to one logical call argument. These
 * values describe the callee's parameter contract, never the actual value's
 * ownership (which is recorded separately in arg_ownership). A returned
 * borrow necessarily carries the borrow relation. */
enum {
  NYIR_ARG_CONTRACT_UNKNOWN = 0,
  NYIR_ARG_CONTRACT_BORROWS = 1,
  NYIR_ARG_CONTRACT_CONSUMES = 2,
  NYIR_ARG_CONTRACT_RETURNS_BORROW = 3,
};

typedef struct {
  /* Borrowed by the function's instructions. Keep the path valid until the
   * function is freed; nyr_ir_func_clone copies non-empty paths into its own
   * storage. Use nyr_func_own_symbol_copy when the function must own a path.
   */
  const char *file;
  uint32_t line;
  uint32_t column;
} nyr_debug_loc_t;

typedef struct {
  bool has_min;
  bool has_max;
  int64_t min;
  int64_t max;
} nyr_range_t;

typedef struct {
  bool known_const;
  int64_t const_value;
  nyr_range_t range;
  size_t use_count;
  unsigned effects;
} nyr_value_fact_t;

/* Compact rebuilt use-def index. `offsets[v]..offsets[v + 1]` indexes the
 * instruction indices in `users` that consume value v. One instruction may
 * appear more than once when it uses a value in multiple operand positions. */
typedef struct {
  size_t value_count;
  size_t use_count;
  size_t *offsets;
  size_t *users;
} nyr_use_def_t;

typedef struct {
  int64_t predecessor_label;
  int value;
} nyr_phi_incoming_t;

typedef struct {
  size_t from_pc;
  size_t to_pc;
  uint64_t count;
} nyr_profile_edge_t;

typedef struct {
  bool returned;
  int64_t result;
  size_t steps;
  size_t op_counts[NYIR_OP_COUNT];
  size_t branch_taken;
  size_t branch_not_taken;
  size_t call_count;
  size_t max_value_index;
  size_t max_local_index;
  size_t max_pc;
  uint64_t *pc_counts;
  size_t pc_count_len;
  nyr_profile_edge_t *edges;
  size_t edge_count;
  size_t edge_cap;
} nyr_eval_result_t;

typedef struct {
  size_t instructions;
  size_t values;
  size_t locals;
  size_t labels;
  size_t branches;
  size_t conditional_branches;
  size_t calls;
  size_t returns;
  size_t range_facts;
  size_t debug_locs;
  size_t vectorize_attempted_loops;
  size_t vectorize_rejected_loops;
  size_t vectorized_loops;
  unsigned effect_mask;
  size_t ops[NYIR_OP_COUNT];
} nyr_metadata_summary_t;

/* Synchronous host-call hook used by NYIR evaluation. `symbol`, the read-only
 * `args` array, and the error buffer are borrowed for this invocation; copy
 * anything that must outlive the callback. On success, write the raw NYIR
 * result through `result`. `ctx` is borrowed for the full evaluation. */
typedef bool (*nyr_call_resolver_t)(void *ctx, const char *symbol,
                                     const int64_t *args, size_t arg_count,
                                     int64_t *result, char *err,
                                     size_t err_len);

/* Lowered scalar ABI class. Source semantic reps are mapped to this enum at
 * the AST-to-NYIR boundary; emitters and type maps consume this fact. */
typedef enum {
  NYIR_SCALAR_UNKNOWN = 0,
  NYIR_SCALAR_I64_RAW,
  NYIR_SCALAR_F32,
  NYIR_SCALAR_F64,
  NYIR_SCALAR_I64_TAGGED,
  NYIR_SCALAR_STRING,
  NYIR_SCALAR_TYPED_BUFFER,
  NYIR_SCALAR_OBJECT,
  NYIR_SCALAR_POINTER,
  NYIR_SCALAR_CLOSURE,
  NYIR_SCALAR_AGGREGATE,
} nyr_scalar_class_t;

typedef struct {
  nyr_op_t op;
  int dst;
  int a;
  int b;
  int c;
  int d;
  int e;
  int f;
  int64_t imm;
  nyr_cmp_t cmp;
  const char *symbol;
  unsigned flags;
  unsigned effects;
  /* Resolved source-to-runtime scalar ABI fact for the produced value. */
  nyr_scalar_class_t scalar_class;
  /* NYIR_CALL formal scalar classes, packed as sixteen four-bit entries per
   * word. Kept inline so ordinary instructions do not own another buffer. */
  uint64_t arg_scalar_classes[NYIR_CALL_MAX_ARGS / 16];
  /* NYIR_CALL actual-value ownership at this call site: thirty-two two-bit
   * entries per word. Zero is UNKNOWN; known entries use ny_sem_ownership_t
   * values from ast.h. This is not the callee formal's borrow/consume
   * contract; that contract must remain separate metadata. */
  uint64_t arg_ownership[NYIR_CALL_MAX_ARGS / 32];
  /* Formal borrow/consume/result-alias disposition, packed as thirty-two
   * two-bit entries per word. Kept distinct from actual-value ownership. */
  uint64_t arg_contracts[NYIR_CALL_MAX_ARGS / 32];
  uint8_t semantic_ownership;
  bool semantic_mutable;
  /* Function-local points-to class. Zero means unknown and may alias any
   * address. On ADDR_* and pointer-derived values this is the value's
   * provenance; on scalar/vector LOAD/STORE it describes the address in .a.
   * Operations with multiple independent addresses (COPY_STRUCT, CALL) keep
   * zero unless a richer per-access summary is available. Recompute after an
   * IR mutation that changes address provenance or call arguments. */
  uint32_t alias_class;
  nyr_debug_loc_t debug;
  nyr_range_t range;
  /* NYIR_CALL args beyond the 6 carried in a..f (stack-passed ABI args).
   * Owned by the instruction; freed by nyr_ir_func_free and by any pass
   * that discards the instruction. NULL/0 when unused. */
  int *extra_args;
  size_t extra_args_len;
  /* For NYIR_CALL: if non-NULL, an array of length imm (the call arity)
   * containing packed by-value aggregate size and SysV eightbyte classes.
   * Zero marks a scalar argument. Owned by the instruction. */
  uint32_t *arg_sizes;
  /* For NYIR_CALL aggregate results: source byte size and the ABI class of
   * each eightbyte.  Zero means a scalar result. */
  uint32_t ret_aggregate_size;
  uint8_t ret_aggregate_classes[2];
  /* NYIR_PHI incoming edges.  Each predecessor label occurs exactly once.
   * The array is owned by the instruction. */
  nyr_phi_incoming_t *phi_incoming;
  size_t phi_incoming_len;
} nyr_inst_t;

static inline void
nyr_call_arg_class_set(uint64_t packed[NYIR_CALL_MAX_ARGS / 16], size_t arg,
                        nyr_scalar_class_t scalar_class) {
  if (!packed || arg >= NYIR_CALL_MAX_ARGS || scalar_class > 15)
    return;
  size_t word = arg / 16;
  unsigned shift = (unsigned)(arg % 16) * 4u;
  packed[word] = (packed[word] & ~((uint64_t)0xf << shift)) |
                 ((uint64_t)scalar_class << shift);
}

static inline nyr_scalar_class_t
nyr_call_arg_class_get(const nyr_inst_t *inst, size_t arg) {
  if (!inst || arg >= NYIR_CALL_MAX_ARGS)
    return NYIR_SCALAR_UNKNOWN;
  unsigned shift = (unsigned)(arg % 16) * 4u;
  return (nyr_scalar_class_t)((inst->arg_scalar_classes[arg / 16] >> shift) &
                               0xfu);
}

static inline bool nyr_call_arg_classes_present(const nyr_inst_t *inst) {
  if (!inst || inst->op != NYIR_CALL)
    return false;
  for (size_t i = 0; i < NYIR_CALL_MAX_ARGS / 16; ++i)
    if (inst->arg_scalar_classes[i])
      return true;
  return false;
}

static inline uint8_t nyr_call_arg_ownership_get(const nyr_inst_t *inst,
                                                  size_t arg) {
  if (!inst || inst->op != NYIR_CALL || arg >= NYIR_CALL_MAX_ARGS)
    return 0;
  unsigned shift = (unsigned)(arg % 32) * 2u;
  return (uint8_t)((inst->arg_ownership[arg / 32] >> shift) & 0x3u);
}

static inline void
nyr_call_arg_ownership_set_packed(uint64_t packed[NYIR_CALL_MAX_ARGS / 32],
                                   size_t arg, uint8_t ownership) {
  if (!packed || arg >= NYIR_CALL_MAX_ARGS || ownership > 3)
    return;
  unsigned shift = (unsigned)(arg % 32) * 2u;
  uint64_t mask = UINT64_C(0x3) << shift;
  packed[arg / 32] =
      (packed[arg / 32] & ~mask) | ((uint64_t)ownership << shift);
}

static inline void nyr_call_arg_ownership_set(nyr_inst_t *inst, size_t arg,
                                               uint8_t ownership) {
  if (!inst || inst->op != NYIR_CALL || arg >= NYIR_CALL_MAX_ARGS)
    return;
  nyr_call_arg_ownership_set_packed(inst->arg_ownership, arg, ownership);
}

static inline uint8_t nyr_call_arg_contract_get(const nyr_inst_t *inst,
                                                 size_t arg) {
  if (!inst || inst->op != NYIR_CALL || arg >= NYIR_CALL_MAX_ARGS)
    return 0;
  unsigned shift = (unsigned)(arg % 32) * 2u;
  return (uint8_t)((inst->arg_contracts[arg / 32] >> shift) & 0x3u);
}

static inline void
nyr_call_arg_contract_set_packed(uint64_t packed[NYIR_CALL_MAX_ARGS / 32],
                                  size_t arg, uint8_t contract) {
  if (!packed || arg >= NYIR_CALL_MAX_ARGS || contract > 3u)
    return;
  unsigned shift = (unsigned)(arg % 32) * 2u;
  uint64_t mask = UINT64_C(0x3) << shift;
  packed[arg / 32] = (packed[arg / 32] & ~mask) | ((uint64_t)contract << shift);
}

static inline void nyr_call_arg_contract_set(nyr_inst_t *inst, size_t arg,
                                              uint8_t contract) {
  if (!inst || inst->op != NYIR_CALL || arg >= NYIR_CALL_MAX_ARGS)
    return;
  nyr_call_arg_contract_set_packed(inst->arg_contracts, arg, contract);
}

/* Decode and validate the positional value IDs carried by a call instruction.
 * Backends share this boundary so a..f/extra_args cannot drift by target. */
bool nyr_call_args(const nyr_inst_t *in, int value_count, int *args,
                    size_t args_cap, int *argc_out, char *err, size_t err_len);

typedef struct {
  bool *value_f64;
  bool *value_f32;
  bool *value_v128_i64;
  bool *value_v256_i64;
  bool *value_v128_f64;
  bool *value_v128_f32;
  bool *value_v256_f64;
  bool *local_f64;
  bool *local_f32;
  bool *local_v128_i64;
  bool *local_v256_i64;
  bool *local_v128_f64;
  bool *local_v128_f32;
  bool *local_v256_f64;
  size_t value_count;
  size_t local_count;
  bool *storage;
} nyr_type_map_t;

typedef enum {
  NYIR_PARAM_I64 = 0,
  NYIR_PARAM_F32,
  NYIR_PARAM_F64,
  /* By-value aggregate parameter. Its ABI payload lives in param_abi[i] as
   * one packed NYIR_ARG_AGG_* word (byte size plus two eightbyte classes).
   * The parameter's local slot holds a pointer to the callee-side home
   * storage that the backend prologue fills from the incoming registers or
   * stack area. */
  NYIR_PARAM_AGGREGATE,
} nyr_param_type_t;

typedef struct {
  nyr_inst_t *data;
  size_t len;
  size_t cap;
  int next_value;
  /* Conservative hint maintained by NYIR construction and transforms. Once
   * true it may stay true after calls are removed, but it must never miss a
   * symbol-backed call. Lets the inliner skip call-free functions entirely. */
  bool has_symbol_calls;
  /* Optimization diagnostics retained with the function so tier/performance
   * reports can distinguish loops that remained scalar from loops that were
   * successfully widened. These counters are non-semantic and are not part
   * of the NYIR binary format. */
  size_t vectorize_attempted_loops;
  size_t vectorize_rejected_loops;
  size_t vectorized_loops;
  nyr_param_type_t *param_types;
  size_t param_count;
  /* Function-entry aggregate ABI: when param_types[i] is
   * NYIR_PARAM_AGGREGATE, param_abi[i] carries the packed NYIR_ARG_AGG_*
   * word (byte size plus two eightbyte classes) describing how the caller
   * passes that parameter. NULL or a zero entry means a scalar parameter.
   * Owned by the function. */
  uint32_t *param_abi;
  /* Function-level aggregate return ABI. A zero size is a scalar return.
   * Otherwise the RET pieces carry the eightbytes: RET.a is eightbyte 0
   * (rax for INTEGER, xmm0 for SSE) and RET.b eightbyte 1 (rdx for
   * INTEGER, xmm1 for SSE). classes[0] == NYIR_ARG_CLASS_MEMORY selects
   * the hidden-pointer convention instead: the first parameter is the
   * caller-provided result buffer and the function returns that pointer
   * in rax. */
  uint32_t return_aggregate_size;
  uint8_t return_aggregate_classes[2];
  /* Source-level callback arity expansion: each language parameter occupies
   * one value slot plus its hidden list/string/dynamic metadata slots. */
  uint8_t *source_param_widths;
  size_t source_param_count;
  char **owned_symbols;
  size_t owned_symbols_len;
  size_t owned_symbols_cap;
} nyr_func_t;

/* Copy a symbol into function-owned storage. The returned pointer remains
 * valid until nyr_ir_func_free and can be used for names exposed alongside it. */
const char *nyr_func_own_symbol_copy(nyr_func_t *f, const char *symbol);

/**
 * Reusable control-flow and dominance view of one NYIR function.
 *
 * Topology is compact CSR adjacency. Dominance-related fact fields are packed
 * per-block bitsets. The view borrows no NYIR storage and must be released
 * with nyr_cfg_free().
 */
typedef struct {
  size_t block_count;
  size_t *block_start;
  size_t *block_end;
  int64_t *block_label;
  size_t *inst_block;
  /* Successor lists are in block order and predecessor lists are in
   * predecessor order, making CFG traversal deterministic. */
  size_t *succ_offsets;
  size_t *succ_blocks;
  size_t *pred_offsets;
  size_t *pred_blocks;
  /* Entry-reachability computed from edges. */
  bool *reachable;
  /* Parallel to succ_blocks: this edge is a natural backedge when its
   * successor dominates its source block. */
  bool *backedge_edges;
  /* Packed row-major dominance bitsets: row `block` contains its dominators.
   * Query through nyr_cfg_dominates rather than indexing this storage. */
  uint64_t *dominators;
  size_t dominator_words;
  uint64_t *frontiers;
  size_t frontier_words;
  int *idom;
} nyr_cfg_t;

bool nyr_type_map_init(nyr_type_map_t *map, const nyr_func_t *nyir,
                        size_t local_count);
void nyr_type_map_free(nyr_type_map_t *map);

#define NYIR_MAX_PASS_STATS 96

typedef struct {
  const char *name;
  double time_ms;
  size_t before_insts;
  size_t after_insts;
  int before_values;
  int after_values;
  size_t before_blocks;
  size_t after_blocks;
} nyr_pass_stat_t;

typedef struct {
  size_t before_insts;
  size_t after_insts;
  int before_values;
  int after_values;
  size_t before_ops[NYIR_OP_COUNT];
  size_t after_ops[NYIR_OP_COUNT];
  nyr_pass_stat_t passes[NYIR_MAX_PASS_STATS];
  size_t pass_count;
  double total_time_ms;
} nyr_opt_stats_t;

void nyr_ir_func_free(nyr_func_t *f);
/* Deep-copy `src` into empty `dst`. Caller owns `dst` and must free it. */
bool nyr_ir_func_clone(const nyr_func_t *src, nyr_func_t *dst);
/* Replace dst with src while preserving function-level ABI metadata omitted
 * by a transforming pass. Both values remain safely freeable. */
void nyr_func_replace(nyr_func_t *dst, nyr_func_t *src);
int nyr_ir_emit(nyr_func_t *f, nyr_inst_t inst);
/* Resets *in to a NOP, freeing all instruction-owned metadata. Used by
 * optimizer passes that discard an instruction in place. */
void nyr_inst_discard(nyr_inst_t *in);
/* Replace every operand use of `old_value` with `new_value`. Definitions are
 * never rewritten. Both values must belong to `f`; returns false otherwise. */
bool nyr_replace_all_uses(nyr_func_t *f, int old_value, int new_value);
/* Erase an instruction in place as a NOP, releasing its owned operands. */
bool nyr_erase_instruction(nyr_func_t *f, size_t index);
bool nyr_build_use_def(const nyr_func_t *f, nyr_use_def_t *out);
void nyr_use_def_free(nyr_use_def_t *uses);
bool nyr_ir_verify(const nyr_func_t *f, char *err, size_t err_len);
/** Build CFG, dominators, immediate dominators, and dominance frontiers.
 * Reinitializes `cfg`; returns false without a partially usable analysis. */
bool nyr_cfg_build(const nyr_func_t *f, nyr_cfg_t *cfg);
/* Builds block topology, predecessor/successor lists, and reachability only.
 * Use this for transformations that do not consume dominance facts. */
bool nyr_cfg_build_topology(const nyr_func_t *f, nyr_cfg_t *cfg);
void nyr_cfg_free(nyr_cfg_t *cfg);
/* Allocates the entry-reachable blocks in deterministic reverse-postorder.
 * The caller owns the returned block array and releases it with free(). */
bool nyr_cfg_reverse_postorder(const nyr_cfg_t *cfg, size_t **out_blocks,
                                size_t *out_len);
bool nyr_cfg_dominates(const nyr_cfg_t *cfg, size_t dominator, size_t block);
bool nyr_cfg_is_backedge(const nyr_cfg_t *cfg, size_t predecessor,
                          size_t successor);
/* Compute the natural-loop block set for a dominance backedge. `member`
 * must have at least cfg->block_count entries and is cleared before use. */
bool nyr_cfg_natural_loop_blocks(const nyr_cfg_t *cfg, size_t latch,
                                  size_t header, bool *member,
                                  size_t member_count);
/**
 * Promote initialized, non-address-taken scalar locals to SSA values.
 *
 * On success `f` contains owned NYIR_PHI metadata. Optimizers preserve
 * those PHIs; machine lowering destroys them with predecessor-edge copies.
 */
bool nyr_mem2reg(nyr_func_t *f);
/* Audit/test override; production nested-SSA promotion remains gated. */
void nyr_set_nested_mem2reg_enabled(bool enabled);
bool nyr_validate_constraints(const nyr_func_t *f, char *err, size_t err_len);
bool nyr_metadata_summary(const nyr_func_t *f,
                           nyr_metadata_summary_t *summary, char *err,
                           size_t err_len);
void nyr_metadata_summary_dump(FILE *out, const char *name,
                                const nyr_metadata_summary_t *summary);
bool nyr_analyze_values(const nyr_func_t *f, nyr_value_fact_t *facts,
                         size_t fact_count, char *err, size_t err_len);
bool nyr_ir_eval(const nyr_func_t *f, int64_t *locals, size_t local_count,
               size_t max_steps, nyr_eval_result_t *result, char *err,
               size_t err_len);
void nyr_eval_result_dump(FILE *out, const char *name,
                           const nyr_eval_result_t *result);
void nyr_eval_result_free(nyr_eval_result_t *result);
bool nyr_ir_eval_with_calls(const nyr_func_t *f, int64_t *locals,
                          size_t local_count, size_t max_steps,
                          nyr_eval_result_t *result,
                          nyr_call_resolver_t resolver, void *resolver_ctx,
                          char *err, size_t err_len);
void nyr_refresh_metadata(nyr_func_t *f);
unsigned nyr_inst_effects(const nyr_inst_t *inst);
void nyr_dump(FILE *out, const nyr_func_t *f, const char *name);
void nyr_dump_compact(FILE *out, const nyr_func_t *f, const char *name);
/* Compact one-line-per-change dump: before → after. Skips unchanged/nops. */
void nyr_dump_diff(FILE *out, const nyr_func_t *before,
                    const nyr_func_t *after, const char *pass_tag);
/* True when the compact diff contains no instruction changes. */
bool nyr_diff_is_empty(const nyr_func_t *before, const nyr_func_t *after);
void nyr_dump_cfg(FILE *out, const nyr_func_t *f, const char *name);
void nyr_dump_stats(FILE *out, const nyr_opt_stats_t *stats);
bool nyr_dump_binary(FILE *out, const nyr_func_t *f, const char *name);
/** Load a versioned NYIR binary into `out`, freeing its old contents.
 * Current output is v11; v1--v7 are normalized into the current opcode table,
 * v8 effect metadata is normalized to the current effect model, v10 adds
 * parameter-type serialization, and v11 appends f64 reduction opcodes without
 * renumbering the older serialized opcode table.
 * Rejects incompatible format versions and writes a diagnostic to `err`. */
bool nyr_load_binary(FILE *in, nyr_func_t *out, char *name, size_t name_len,
                      char *err, size_t err_len);
bool nyr_const_fold(nyr_func_t *f);
/* Sparse conditional constant propagation for SSA/PHI NYIR. */
bool nyr_sccp(nyr_func_t *f);
bool nyr_copy_prop(nyr_func_t *f);
bool nyr_peephole(nyr_func_t *f);
bool nyr_dce(nyr_func_t *f);
bool nyr_cfg_simplify(nyr_func_t *f);
/* Convert exact, short, side-effect-free i64 diamonds to NYIR_SELECT_I64. */
bool nyr_if_convert(nyr_func_t *f);
/* Fold boolean PHIs in proven-constant, side-effect-free diamonds. */
bool nyr_phiopt(nyr_func_t *f);
bool nyr_compact(nyr_func_t *f);
bool nyr_cse(nyr_func_t *f);
bool nyr_strength_reduce(nyr_func_t *f);
bool nyr_dead_store_elim(nyr_func_t *f);
bool nyr_redundant_load_elim(nyr_func_t *f);
bool nyr_licm(nyr_func_t *f);
bool nyr_jump_thread(nyr_func_t *f);
/** Optimize `f` at O0--O3. `stats` may be NULL; when supplied it receives
 * before/after counts and per-pass timings. O2+ preserve SSA PHIs until
 * machine lowering. */
void nyr_set_cf_mem2reg_enabled(bool enable);
void nyr_set_preserve_phis(bool preserve);
bool nyr_get_preserve_phis(void);
/* Optional diagnostic controls. Names are target-independent NYIR pass names
 * such as "const_fold", "dce", or "cfg_simplify". */
void nyr_set_pass_controls(const char *disable_pass, const char *stop_after);
/* Require verifier checkpoints after every normal optimization pass. */
void nyr_set_verify_each_pass(bool enable);
/* On verify failure, restore the pre-pass IR (requires verify_each_pass). */
void nyr_set_rollback_on_verify_fail(bool enable);
/* Global instruction cap: abort pipeline when total IR instructions exceed cap.
 */
void nyr_set_global_inst_cap(size_t cap);
/* Per-pass growth cap: abort pass when instruction growth exceeds factor
 * (>=1.0). */
void nyr_set_pass_growth_factor(double factor);
/* Reconcile scalar classes on local loads/stores with finalized producer facts
 * at backend boundaries after any registry/inliner transforms. */
void nyr_reconcile_local_scalar_classes(nyr_func_t *f);
/* Set the nyr_track_value diagnostic (>=1 traces the named SSA value). */
void nyr_set_track_value(int value);
/* Read the nyr_track_value diagnostic; false when tracing is disabled. */
bool nyr_get_track_value(int *out);

/* Reserved resolver error that reports a language-level raise (compiled
 * rt_panic: throw, divide-by-zero guards). The NYIR VM unwinds it to the
 * enclosing try's catch arm instead of aborting the evaluation; dispatch
 * wrappers must propagate it verbatim. Not a user-visible diagnostic. */
#define NYIR_VM_LANGUAGE_PANIC_ERR "nyir-vm\x1f:language-panic"
/* Optional per-pass oracle callback.  Called after every successful verifier
 * checkpoint with the current NYIR function (usually rt_main).  The callback
 * owns any comparison state (VM, native, expected values, auxiliary functions).
 * Returning false aborts the optimization pipeline. */
typedef bool (*nyr_per_pass_oracle_fn)(const nyr_func_t *f,
                                        const char *pass_name, void *userdata);
void nyr_set_per_pass_oracle(bool enable, nyr_per_pass_oracle_fn fn,
                              void *userdata);
/* Translation-validation seed: after each pass, when both before/after are
 * pure integer straight-line NYIR, compare results across `trials` local
 * input vectors. trials<=0 disables. Diagnostic mode only. */
void nyr_set_tv_seed(int trials);
/* True when f is pure i64 straight-line (locals + arithmetic/cmp/ret only). */
bool nyr_is_pure_i64_straightline(const nyr_func_t *f);
/* Differential interpreter equivalence for pure i64 straight-line before/after.
 * Returns true when ineligible (not pure straight-line) or all trials match.
 * Returns false with err filled on a concrete mismatch. */
bool nyr_tv_equiv_straightline(const nyr_func_t *before,
                                const nyr_func_t *after, int trials, char *err,
                                size_t err_len);
/* Z3 equivalence for supported NYIR; returns false with an empty error when
 * unsupported so the caller can use the bounded interpreter fallback. */
bool nyr_tv_smt_equiv(const nyr_func_t *before, const nyr_func_t *after,
                       char *err, size_t err_len);
bool nyr_memory_ssa_forward(nyr_func_t *f);

/* Roadmap seed passes (bounded, correctness-first). */
/*
 * Return the audited effects for a known runtime call. Unknown calls retain
 * the conservative CALL|UNKNOWN_SIDE_EFFECT summary. The memory bits in a
 * returned summary have the coarse NYIR_EFFECT_READ/WRITE_MEMORY semantics:
 * they do not identify argument-reachable objects or bound an access extent.
 */
unsigned nyr_call_effect_summary(const nyr_inst_t *inst);
/* Return the analysis-visible effect set, replacing a CALL's conservative
 * stored CALL|UNKNOWN summary with its audited runtime-helper summary. */
unsigned nyr_effective_effects(const nyr_inst_t *inst);
bool nyr_effect_summary(const nyr_func_t *f, unsigned *out_mask,
                         size_t *out_reads, size_t *out_writes,
                         size_t *out_calls);
bool nyr_null_align_facts(nyr_func_t *f);
bool nyr_iv_simplify(nyr_func_t *f);
bool nyr_bounds_check_elim(nyr_func_t *f);
bool nyr_null_check_elim(nyr_func_t *f);
bool nyr_overflow_check_elim(nyr_func_t *f);
bool nyr_egraph_local(nyr_func_t *f);
bool nyr_apply_rules(nyr_func_t *f);
bool nyr_inline_small(nyr_func_t *f);
bool nyr_inline_general(nyr_func_t *f);
/* Propagate constants through conservative local pure calls. */
bool nyr_ipa_constprop(nyr_func_t *f);
bool nyr_loop_unroll(nyr_func_t *f);
bool nyr_scev_lite(nyr_func_t *f);
bool nyr_irce(nyr_func_t *f);
bool nyr_loop_idiom(nyr_func_t *f);
/* Remove pure counted loops whose only live carried value is overwritten. */
bool nyr_loop_rotate(nyr_func_t *f);
bool nyr_loop_interchange(nyr_func_t *f);
bool nyr_memcombine(nyr_func_t *f);
bool nyr_loop_versioning(nyr_func_t *f);
bool nyr_loop_unswitch(nyr_func_t *f);
bool nyr_iv_elim(nyr_func_t *f);
bool nyr_gvn_pre(nyr_func_t *f);
bool nyr_loop_vectorize(nyr_func_t *f);
bool nyr_slp_vectorize(nyr_func_t *f);
bool nyr_alias_store_sink(nyr_func_t *f);
bool nyr_narrow(nyr_func_t *f);
bool nyr_phi_elim(nyr_func_t *f);
bool nyr_tbuf_scalar_len(nyr_func_t *f);
bool nyr_tbuf_private_object(nyr_func_t *f);
bool nyr_tbuf_promote_alloca(nyr_func_t *f);
bool nyr_points_to_sroa(nyr_func_t *f);
bool nyr_store_sink(nyr_func_t *f);
bool nyr_aggregate_sroa(nyr_func_t *f);
bool nyr_polyhedral_nest(nyr_func_t *f);
bool nyr_kernel_hint(nyr_func_t *f);
bool nyr_block_layout(nyr_func_t *f);
bool nyr_escape_sroa(nyr_func_t *f);
bool nyr_sroa_scalar(nyr_func_t *f);
bool nyr_alias_analysis(nyr_func_t *f);
bool nyr_cvp(nyr_func_t *f);
bool nyr_devirtualize(nyr_func_t *f);
bool nyr_func_specialize(nyr_func_t *f);
/* Self-tail-call conversion. `func_name` is the name of the function
 * currently being optimized (supplied by the builder); without a matching
 * name the pass proves nothing and must leave `f` unchanged, since NYIR
 * instructions carry no callee identity for self-recursion detection.
 * Argument rebinding relies on the ABI convention that parameters live in
 * local slots 0..param_count-1, spilled by the machine prologue. */
bool nyr_tail_call(nyr_func_t *f, const char *func_name);
bool nyr_adce(nyr_func_t *f);
bool nyr_dead_arg_elim(nyr_func_t *f);
bool nyr_vrp(nyr_func_t *f);
bool nyr_prefetch_insert(nyr_func_t *f);
bool nyr_polyhedral_tiling(nyr_func_t *f);

size_t ny_isle_rule_count(void);
const char *ny_isle_rule_name(size_t i);
int ny_isle_rule_cost(size_t i);
/** Shared ISLE applicator for NYIR (text/optimizer path). */
bool ny_isle_apply_nir(nyr_func_t *f);
bool nyr_algebraic_combine(nyr_func_t *f);
bool nyr_double_neg(nyr_func_t *f);
bool nyr_reassoc_add(nyr_func_t *f);
bool nyr_rewrite_fuel(nyr_func_t *f);
/** Equality-saturation e-graph over pure scalar integer ops (see opt/egraph.c).
 */
bool nyr_egraph_rewrite(nyr_func_t *f);
void nyr_egraph_stats(unsigned long long *runs,
                       unsigned long long *fuel_exhausted);

typedef struct {
  const char *name;
  const nyr_func_t *func;
} nyr_inline_callee_t;
void nyr_set_inline_callees(const nyr_inline_callee_t *callees, size_t count);
/** Whether a collected callee can observe a global cell. */
bool nyr_inline_callees_reference_symbol(const char *symbol,
                                          const nyr_func_t *exclude);
/** Promote private raw-int globals used only by the synthesized entry body. */
bool nyr_promote_private_entry_globals(nyr_func_t *f);

bool ny_native_profile_write(FILE *out, uint64_t edge_count, uint64_t steps,
                             uint64_t hash);
bool ny_native_profile_read(FILE *in, uint64_t *edge_count, uint64_t *steps,
                            uint64_t *hash);
void ny_native_profile_set_runtime(uint64_t edges, uint64_t steps);
bool ny_native_profile_should_inline(size_t callee_insts);
bool nyr_icp_profile(nyr_func_t *f);
uint64_t ny_native_profile_edges(void);
uint64_t ny_native_profile_steps(void);
bool ny_native_profile_load_path(const char *path, char *err, size_t err_len);
void ny_native_profile_clear(void);
void ny_native_profile_select(const char *name);
uint64_t ny_native_profile_block_hot(size_t pc);
uint64_t ny_native_profile_edge_hot(size_t from_pc, size_t to_pc);
uint64_t ny_native_profile_loop_hot(size_t header_pc);

/*
 * Fast-math mode: when enabled (NYTRIX_FAST_MATH=1), the optimizer may
 * reorder floating-point operations, fold NaN/Inf checks, and use
 * reciprocal-based strength reduction.
 */
void nyr_set_fast_math(bool on);
bool nyr_fast_math(void);

bool nyr_optimize_with_stats(nyr_func_t *f, nyr_opt_stats_t *stats,
                              int opt_level);
bool nyr_ir_optimize(nyr_func_t *f, int opt_level);
/* A NULL dump suppresses optimizer IR dumps while preserving optimization. */
bool nyr_optimize_debug(nyr_func_t *f, FILE *dump, nyr_opt_stats_t *stats,
                         int opt_level);
const char *nyr_op_name(nyr_op_t op);

/* Count of local slots referenced in f (LOAD_LOCAL / STORE_LOCAL / ADDR_LOCAL).
 * Returns 0 when no locals are used. */
size_t nyr_max_local(const nyr_func_t *f);

/* True when op is a control-flow barrier (label, branch, call). */
static inline bool nyr_is_barrier(nyr_op_t op) {
  return op == NYIR_LABEL || op == NYIR_BR || op == NYIR_BR_IF ||
         op == NYIR_CALL;
}

#endif
