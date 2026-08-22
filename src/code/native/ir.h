#ifndef NY_NATIVE_IR_H
#define NY_NATIVE_IR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

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
  NYIR_ADDR_SYMBOL,  /* leaq symbol(%rip), dst — RIP-relative address of a named symbol */
  NYIR_ALLOCA,       /* allocate stack space for byval/sret */
  NYIR_COPY_STRUCT,  /* copy aggregate data */
  NYIR_CAPTURE_RET,  /* capture a secondary ABI return register */
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
  NYIR_OP_COUNT,
} nyir_op_t;

typedef enum {
  NYIR_CMP_EQ = 0,
  NYIR_CMP_NE,
  NYIR_CMP_LT,
  NYIR_CMP_LE,
  NYIR_CMP_GT,
  NYIR_CMP_GE,
} nyir_cmp_t;

typedef enum {
  NYIR_EFFECT_NONE = 0,
  NYIR_EFFECT_READ_LOCAL = 1u << 0,
  NYIR_EFFECT_WRITE_LOCAL = 1u << 1,
  NYIR_EFFECT_CALL = 1u << 2,
  NYIR_EFFECT_CONTROL = 1u << 3,
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
} nyir_effect_t;

#define NYIR_INST_F_EXTERN 1u
#define NYIR_INST_F_RET_F64 2u
#define NYIR_INST_F_RET_F32 4u
#define NYIR_INST_F_SRET 8u
#define NYIR_INST_F_MEM_F64 16u
#define NYIR_INST_F_MEM_BYTE 32u
/* Set by nyir_narrow() on scalar-int ALU ops (add/sub/and/or/xor/mul) whose
 * operands and destination are provably non-negative and within
 * [0, INT32_MAX]. x86-64's 32-bit register ALU forms zero-extend the upper
 * 32 bits of the destination on write, so a 32-bit load + 32-bit op +
 * full 64-bit store round-trips to the identical 64-bit value as the
 * 64-bit form, letting the emitter drop the REX.W prefix. NOTE: only
 * sound for non-negative ranges -- a value that can go negative would
 * need explicit sign-extension after a 32-bit op, which this flag does
 * not provide, so nyir_narrow() must never set it otherwise. */
#define NYIR_INST_F_NARROW32 64u
/*
 * Set on NYIR_CALL by the native builder after interprocedural effect
 * inference when the callee is a user function whose full observable effect
 * set is known (see ny_native_nir_patch_call_effects).  When set,
 * nyir_effective_effects() trusts inst->effects instead of the
 * symbol-table default (CALL|FFI|UNKNOWN_SIDE_EFFECT), which lets LICM and
 * friends treat provably pure user calls like any other pure operation.
 */
#define NYIR_INST_F_EFFECTS_KNOWN 128u

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
#define NYIR_ARG_AGG_CLASS(v, n)                                         \
  (((v) >> ((n) ? NYIR_ARG_AGG_CLASS1_SHIFT                              \
                  : NYIR_ARG_AGG_CLASS0_SHIFT)) &                        \
   NYIR_ARG_AGG_CLASS_MASK)

/* Calls with more than 6 args carry args[6..] out-of-line in extra_args,
 * covering the SysV/Win64 stack-passed portion of the call ABI. The cap is
 * an implementation sanity bound, not an ABI limit. */
#define NYIR_CALL_MAX_ARGS 64

typedef struct {
  const char *file;
  uint32_t line;
  uint32_t column;
} nyir_debug_loc_t;

typedef struct {
  bool has_min;
  bool has_max;
  int64_t min;
  int64_t max;
} nyir_range_t;

typedef struct {
  bool known_const;
  int64_t const_value;
  nyir_range_t range;
  size_t use_count;
  unsigned effects;
} nyir_value_fact_t;

/* Compact rebuilt use-def index. `offsets[v]..offsets[v + 1]` indexes the
 * instruction indices in `users` that consume value v. One instruction may
 * appear more than once when it uses a value in multiple operand positions. */
typedef struct {
  size_t value_count;
  size_t use_count;
  size_t *offsets;
  size_t *users;
} nyir_use_def_t;

typedef struct {
  int64_t predecessor_label;
  int value;
} nyir_phi_incoming_t;

typedef struct {
  size_t from_pc;
  size_t to_pc;
  uint64_t count;
} nyir_profile_edge_t;

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
  nyir_profile_edge_t *edges;
  size_t edge_count;
  size_t edge_cap;
} nyir_eval_result_t;

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
} nyir_metadata_summary_t;

typedef bool (*nyir_call_resolver_t)(void *ctx, const char *symbol,
                                       const int64_t *args, size_t arg_count,
                                       int64_t *result, char *err,
                                       size_t err_len);

typedef struct {
  nyir_op_t op;
  int dst;
  int a;
  int b;
  int c;
  int d;
  int e;
  int f;
  int64_t imm;
  nyir_cmp_t cmp;
  const char *symbol;
  unsigned flags;
  unsigned effects;
  nyir_debug_loc_t debug;
  nyir_range_t range;
  /* NYIR_CALL args beyond the 6 carried in a..f (stack-passed ABI args).
   * Owned by the instruction; freed by nyir_func_free and by any pass
   * that discards the instruction. NULL/0 when unused. */
  int *extra_args;
  size_t extra_args_len;
  /* For NYIR_CALL: if non-NULL, an array of length imm (the call arity)
   * containing packed by-value aggregate size and SysV eightbyte classes.
   * Zero marks a scalar argument. Owned by the instruction. */
  uint32_t *arg_sizes;
  /* NYIR_PHI incoming edges.  Each predecessor label occurs exactly once.
   * The array is owned by the instruction. */
  nyir_phi_incoming_t *phi_incoming;
  size_t phi_incoming_len;
} nyir_inst_t;

/* Decode and validate the positional value IDs carried by a call instruction.
 * Backends share this boundary so a..f/extra_args cannot drift by target. */
bool nyir_call_args(const nyir_inst_t *in, int value_count, int *args,
                      size_t args_cap, int *argc_out, char *err,
                      size_t err_len);

typedef struct {
  bool *value_f64;
  bool *value_f32;
  bool *value_v128_i64;
  bool *value_v256_i64;
  bool *value_v128_f64;
  bool *value_v128_f32;
  bool *local_f64;
  bool *local_f32;
  bool *local_v128_i64;
  bool *local_v256_i64;
  bool *local_v128_f64;
  bool *local_v128_f32;
  size_t value_count;
  size_t local_count;
} nyir_type_map_t;

typedef enum {
  NYIR_PARAM_I64 = 0,
  NYIR_PARAM_F32,
  NYIR_PARAM_F64,
} nyir_param_type_t;

typedef struct {
  nyir_inst_t *data;
  size_t len;
  size_t cap;
  int next_value;
  /* Optimization diagnostics retained with the function so tier/performance
   * reports can distinguish loops that remained scalar from loops that were
   * successfully widened. These counters are non-semantic and are not part
   * of the NYIR binary format. */
  size_t vectorize_attempted_loops;
  size_t vectorize_rejected_loops;
  size_t vectorized_loops;
  nyir_param_type_t *param_types;
  size_t param_count;
  char **owned_symbols;
  size_t owned_symbols_len;
  size_t owned_symbols_cap;
} nyir_func_t;

/**
 * Reusable control-flow and dominance view of one NYIR function.
 *
 * Topology is compact CSR adjacency. Dominance-related fact fields are packed
 * per-block bitsets. The view borrows no NYIR storage and must be released
 * with nyir_cfg_free().
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
   * Query through nyir_cfg_dominates rather than indexing this storage. */
  uint64_t *dominators;
  size_t dominator_words;
  uint64_t *frontiers;
  size_t frontier_words;
  int *idom;
} nyir_cfg_t;

bool nyir_type_map_init(nyir_type_map_t *map, const nyir_func_t *nyir,
                          size_t local_count);
void nyir_type_map_free(nyir_type_map_t *map);

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
} nyir_pass_stat_t;

typedef struct {
  size_t before_insts;
  size_t after_insts;
  int before_values;
  int after_values;
  size_t before_ops[NYIR_OP_COUNT];
  size_t after_ops[NYIR_OP_COUNT];
  nyir_pass_stat_t passes[NYIR_MAX_PASS_STATS];
  size_t pass_count;
  double total_time_ms;
} nyir_opt_stats_t;

void nyir_func_free(nyir_func_t *f);
/* Deep-copy `src` into empty `dst`. Caller owns `dst` and must free it. */
bool nyir_func_clone(const nyir_func_t *src, nyir_func_t *dst);
int nyir_emit(nyir_func_t *f, nyir_inst_t inst);
/* Resets *in to a NOP, freeing all instruction-owned metadata. Used by
 * optimizer passes that discard an instruction in place. */
void nyir_inst_discard(nyir_inst_t *in);
/* Replace every operand use of `old_value` with `new_value`. Definitions are
 * never rewritten. Both values must belong to `f`; returns false otherwise. */
bool nyir_replace_all_uses(nyir_func_t *f, int old_value, int new_value);
/* Erase an instruction in place as a NOP, releasing its owned operands. */
bool nyir_erase_instruction(nyir_func_t *f, size_t index);
bool nyir_build_use_def(const nyir_func_t *f, nyir_use_def_t *out);
void nyir_use_def_free(nyir_use_def_t *uses);
bool nyir_verify(const nyir_func_t *f, char *err, size_t err_len);
/** Build CFG, dominators, immediate dominators, and dominance frontiers.
 * Reinitializes `cfg`; returns false without a partially usable analysis. */
bool nyir_cfg_build(const nyir_func_t *f, nyir_cfg_t *cfg);
/* Builds block topology, predecessor/successor lists, and reachability only.
 * Use this for transformations that do not consume dominance facts. */
bool nyir_cfg_build_topology(const nyir_func_t *f, nyir_cfg_t *cfg);
void nyir_cfg_free(nyir_cfg_t *cfg);
/* Allocates the entry-reachable blocks in deterministic reverse-postorder.
 * The caller owns the returned block array and releases it with free(). */
bool nyir_cfg_reverse_postorder(const nyir_cfg_t *cfg,
                                  size_t **out_blocks, size_t *out_len);
bool nyir_cfg_dominates(const nyir_cfg_t *cfg, size_t dominator,
                          size_t block);
bool nyir_cfg_is_backedge(const nyir_cfg_t *cfg, size_t predecessor,
                            size_t successor);
/* Compute the natural-loop block set for a dominance backedge. `member`
 * must have at least cfg->block_count entries and is cleared before use. */
bool nyir_cfg_natural_loop_blocks(const nyir_cfg_t *cfg, size_t latch,
                                    size_t header, bool *member,
                                    size_t member_count);
/**
 * Promote initialized, non-address-taken scalar locals to SSA values.
 *
 * On success `f` contains owned NYIR_PHI metadata. Optimizers preserve
 * those PHIs; machine lowering destroys them with predecessor-edge copies.
 */
bool nyir_mem2reg(nyir_func_t *f);
bool nyir_validate_constraints(const nyir_func_t *f, char *err,
                                 size_t err_len);
bool nyir_metadata_summary(const nyir_func_t *f,
                             nyir_metadata_summary_t *summary, char *err,
                             size_t err_len);
void nyir_metadata_summary_dump(FILE *out, const char *name,
                                  const nyir_metadata_summary_t *summary);
bool nyir_analyze_values(const nyir_func_t *f, nyir_value_fact_t *facts,
                           size_t fact_count, char *err, size_t err_len);
bool nyir_eval(const nyir_func_t *f, int64_t *locals, size_t local_count,
                 size_t max_steps, nyir_eval_result_t *result, char *err,
                 size_t err_len);
void nyir_eval_result_dump(FILE *out, const char *name,
                              const nyir_eval_result_t *result);
void nyir_eval_result_free(nyir_eval_result_t *result);
bool nyir_eval_with_calls(const nyir_func_t *f, int64_t *locals,
                            size_t local_count, size_t max_steps,
                            nyir_eval_result_t *result,
                            nyir_call_resolver_t resolver, void *resolver_ctx,
                            char *err, size_t err_len);
void nyir_refresh_metadata(nyir_func_t *f);
unsigned nyir_inst_effects(const nyir_inst_t *inst);
void nyir_dump(FILE *out, const nyir_func_t *f, const char *name);
/* Compact one-line-per-change dump: before → after. Skips unchanged/nops. */
void nyir_dump_diff(FILE *out, const nyir_func_t *before,
                      const nyir_func_t *after, const char *pass_tag);
void nyir_dump_cfg(FILE *out, const nyir_func_t *f, const char *name);
void nyir_dump_stats(FILE *out, const nyir_opt_stats_t *stats);
bool nyir_dump_binary(FILE *out, const nyir_func_t *f, const char *name);
/** Load a versioned NYIR binary into `out`, freeing its old contents.
 * Current output is v11; v1--v7 are normalized into the current opcode table,
 * v8 effect metadata is normalized to the current effect model, v10 adds
 * parameter-type serialization, and v11 appends f64 reduction opcodes without
 * renumbering the older serialized opcode table.
 * Rejects incompatible format versions and writes a diagnostic to `err`. */
bool nyir_load_binary(FILE *in, nyir_func_t *out, char *name,
                        size_t name_len, char *err, size_t err_len);
bool nyir_const_fold(nyir_func_t *f);
/* Sparse conditional constant propagation for SSA/PHI NYIR. */
bool nyir_sccp(nyir_func_t *f);
bool nyir_copy_prop(nyir_func_t *f);
bool nyir_peephole(nyir_func_t *f);
bool nyir_dce(nyir_func_t *f);
bool nyir_cfg_simplify(nyir_func_t *f);
bool nyir_compact(nyir_func_t *f);
bool nyir_cse(nyir_func_t *f);
bool nyir_strength_reduce(nyir_func_t *f);
bool nyir_dead_store_elim(nyir_func_t *f);
bool nyir_redundant_load_elim(nyir_func_t *f);
bool nyir_licm(nyir_func_t *f);
bool nyir_jump_thread(nyir_func_t *f);
/** Optimize `f` at O0--O3. `stats` may be NULL; when supplied it receives
 * before/after counts and per-pass timings. O2+ preserve SSA PHIs until
 * machine lowering. */
void nyir_set_cf_mem2reg_enabled(bool enable);
void nyir_set_preserve_phis(bool preserve);
bool nyir_get_preserve_phis(void);
/* Optional diagnostic controls. Names are target-independent NYIR pass names
 * such as "const_fold", "dce", or "cfg_simplify". */
void nyir_set_pass_controls(const char *disable_pass, const char *stop_after);
/* Require verifier checkpoints after every normal optimization pass. */
void nyir_set_verify_each_pass(bool enable);
/* Optional per-pass oracle callback.  Called after every successful verifier
 * checkpoint with the current NYIR function (usually rt_main).  The callback
 * owns any comparison state (VM, native, expected values, auxiliary functions).
 * Returning false aborts the optimization pipeline. */
typedef bool (*nyir_per_pass_oracle_fn)(const nyir_func_t *f,
                                          const char *pass_name, void *userdata);
void nyir_set_per_pass_oracle(bool enable, nyir_per_pass_oracle_fn fn,
                                void *userdata);
/* Translation-validation seed: after each pass, when both before/after are
 * pure integer straight-line NYIR, compare results across `trials` local
 * input vectors. trials<=0 disables. Diagnostic mode only. */
void nyir_set_tv_seed(int trials);
/* True when f is pure i64 straight-line (locals + arithmetic/cmp/ret only). */
bool nyir_is_pure_i64_straightline(const nyir_func_t *f);
/* Differential interpreter equivalence for pure i64 straight-line before/after.
 * Returns true when ineligible (not pure straight-line) or all trials match.
 * Returns false with err filled on a concrete mismatch. */
bool nyir_tv_equiv_straightline(const nyir_func_t *before,
                                  const nyir_func_t *after, int trials,
                                  char *err, size_t err_len);
/* Z3 bitvector equivalence for pure i64 straight-line; falls back to
 * multi-input interpreter TV for branches or when Z3 is unavailable. */
bool nyir_tv_smt_equiv(const nyir_func_t *before, const nyir_func_t *after,
                         char *err, size_t err_len);
bool nyir_memory_ssa_forward(nyir_func_t *f);

/* Roadmap seed passes (bounded, correctness-first). */
/*
 * Return the audited effects for a known runtime call. Unknown calls retain
 * the conservative CALL|UNKNOWN_SIDE_EFFECT summary.
 */
unsigned nyir_call_effect_summary(const nyir_inst_t *inst);
/* Return the analysis-visible effect set, replacing a CALL's conservative
 * stored CALL|UNKNOWN summary with its audited runtime-helper summary. */
unsigned nyir_effective_effects(const nyir_inst_t *inst);
bool nyir_effect_summary(const nyir_func_t *f, unsigned *out_mask,
                           size_t *out_reads, size_t *out_writes,
                           size_t *out_calls);
bool nyir_null_align_facts(nyir_func_t *f);
bool nyir_iv_simplify(nyir_func_t *f);
bool nyir_bounds_check_elim(nyir_func_t *f);
bool nyir_egraph_local(nyir_func_t *f);
bool nyir_apply_rules(nyir_func_t *f);
bool nyir_inline_small(nyir_func_t *f);
bool nyir_inline_general(nyir_func_t *f);
bool nyir_loop_unroll(nyir_func_t *f);
bool nyir_scev_lite(nyir_func_t *f);
bool nyir_irce(nyir_func_t *f);
bool nyir_loop_idiom(nyir_func_t *f);
bool nyir_loop_rotate(nyir_func_t *f);
bool nyir_loop_interchange(nyir_func_t *f);
bool nyir_loop_versioning(nyir_func_t *f);
bool nyir_loop_unswitch(nyir_func_t *f);
bool nyir_iv_elim(nyir_func_t *f);
bool nyir_gvn_pre(nyir_func_t *f);
bool nyir_loop_vectorize(nyir_func_t *f);
bool nyir_slp_vectorize(nyir_func_t *f);
bool nyir_alias_store_sink(nyir_func_t *f);
bool nyir_narrow(nyir_func_t *f);
bool nyir_phi_elim(nyir_func_t *f);
bool nyir_tbuf_scalar_len(nyir_func_t *f);
bool nyir_tbuf_private_object(nyir_func_t *f);
bool nyir_points_to_sroa(nyir_func_t *f);
bool nyir_store_sink(nyir_func_t *f);
bool nyir_aggregate_sroa(nyir_func_t *f);
bool nyir_polyhedral_nest(nyir_func_t *f);
bool nyir_kernel_hint(nyir_func_t *f);
bool nyir_block_layout(nyir_func_t *f);
bool nyir_escape_sroa(nyir_func_t *f);
bool nyir_sroa_scalar(nyir_func_t *f);


size_t ny_isle_rule_count(void);
const char *ny_isle_rule_name(size_t i);
int ny_isle_rule_cost(size_t i);
/** Shared ISLE applicator for NYIR (text/optimizer path). */
bool ny_isle_apply_nir(nyir_func_t *f);
bool nyir_algebraic_combine(nyir_func_t *f);
bool nyir_double_neg(nyir_func_t *f);
bool nyir_reassoc_add(nyir_func_t *f);
bool nyir_rewrite_fuel(nyir_func_t *f);

typedef struct {
  const char *name;
  const nyir_func_t *func;
} nyir_inline_callee_t;
void nyir_set_inline_callees(const nyir_inline_callee_t *callees,
                               size_t count);

bool ny_native_profile_write(FILE *out, uint64_t edge_count, uint64_t steps,
                             uint64_t hash);
bool ny_native_profile_read(FILE *in, uint64_t *edge_count, uint64_t *steps,
                            uint64_t *hash);
void ny_native_profile_set_runtime(uint64_t edges, uint64_t steps);
bool ny_native_profile_should_inline(size_t callee_insts);
bool nyir_icp_profile(nyir_func_t *f);
uint64_t ny_native_profile_edges(void);
uint64_t ny_native_profile_steps(void);
bool ny_native_profile_load_path(const char *path, char *err, size_t err_len);
void ny_native_profile_clear(void);
void ny_native_profile_select(const char *name);
uint64_t ny_native_profile_block_hot(size_t pc);
uint64_t ny_native_profile_edge_hot(size_t from_pc, size_t to_pc);
uint64_t ny_native_profile_loop_hot(size_t header_pc);

bool nyir_optimize_with_stats(nyir_func_t *f, nyir_opt_stats_t *stats, int opt_level);
bool nyir_optimize(nyir_func_t *f, int opt_level);
bool nyir_optimize_debug(nyir_func_t *f, FILE *dump, nyir_opt_stats_t *stats, int opt_level);
const char *nyir_op_name(nyir_op_t op);

/* Count of local slots referenced in f (LOAD_LOCAL / STORE_LOCAL / ADDR_LOCAL).
 * Returns 0 when no locals are used. */
size_t nyir_max_local(const nyir_func_t *f);

/* True when op is a control-flow barrier (label, branch, call). */
static inline bool nyir_is_barrier(nyir_op_t op) {
  return op == NYIR_LABEL || op == NYIR_BR || op == NYIR_BR_IF ||
         op == NYIR_CALL;
}

#endif
