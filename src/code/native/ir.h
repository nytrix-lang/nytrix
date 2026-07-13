#ifndef NY_NATIVE_IR_H
#define NY_NATIVE_IR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
  NY_NIR_NOP = 0,
  NY_NIR_CONST_I64,
  NY_NIR_COPY,
  NY_NIR_ADD_I64,
  NY_NIR_SUB_I64,
  NY_NIR_MUL_I64,
  NY_NIR_DIV_I64,
  NY_NIR_MOD_I64,
  NY_NIR_AND_I64,
  NY_NIR_OR_I64,
  NY_NIR_XOR_I64,
  NY_NIR_SHL_I64,
  NY_NIR_SAR_I64,
  NY_NIR_CMP_I64,
  NY_NIR_LABEL,
  NY_NIR_LOAD_LOCAL,
  NY_NIR_STORE_LOCAL,
  NY_NIR_CALL,
  NY_NIR_RET,
  NY_NIR_BR,
  NY_NIR_BR_IF,
  NYIR_CONST_F64,
  NYIR_ADD_F64,
  NYIR_SUB_F64,
  NYIR_MUL_F64,
  NYIR_DIV_F64,
  NYIR_I64_TO_F64,
  NYIR_CMP_F64,
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
  NYIR_OP_COUNT,
} ny_nir_op_t;

typedef enum {
  NY_NIR_CMP_EQ = 0,
  NY_NIR_CMP_NE,
  NY_NIR_CMP_LT,
  NY_NIR_CMP_LE,
  NY_NIR_CMP_GT,
  NY_NIR_CMP_GE,
} ny_nir_cmp_t;

typedef enum {
  NY_NIR_EFFECT_NONE = 0,
  NY_NIR_EFFECT_READ_LOCAL = 1u << 0,
  NY_NIR_EFFECT_WRITE_LOCAL = 1u << 1,
  NY_NIR_EFFECT_CALL = 1u << 2,
  NY_NIR_EFFECT_CONTROL = 1u << 3,
} ny_nir_effect_t;

#define NY_NIR_INST_F_EXTERN 1u
#define NY_NIR_INST_F_RET_F64 2u
#define NY_NIR_INST_F_RET_F32 4u

/* Packed NY_NIR_CALL aggregate-argument metadata. */
#define NY_NIR_ARG_AGG_SIZE_MASK 0x00ffffffu
#define NY_NIR_ARG_AGG_CLASS0_SHIFT 24u
#define NY_NIR_ARG_AGG_CLASS1_SHIFT 28u
#define NY_NIR_ARG_AGG_CLASS_MASK 0x0fu
#define NY_NIR_ARG_CLASS_NONE 0u
#define NY_NIR_ARG_CLASS_INTEGER 1u
#define NY_NIR_ARG_CLASS_SSE 2u
#define NY_NIR_ARG_CLASS_MEMORY 3u
#define NY_NIR_ARG_CLASS_UNSUPPORTED 4u
#define NY_NIR_ARG_AGG_SIZE(v) ((v) & NY_NIR_ARG_AGG_SIZE_MASK)
#define NY_NIR_ARG_AGG_CLASS(v, n)                                         \
  (((v) >> ((n) ? NY_NIR_ARG_AGG_CLASS1_SHIFT                              \
                  : NY_NIR_ARG_AGG_CLASS0_SHIFT)) &                        \
   NY_NIR_ARG_AGG_CLASS_MASK)

/* Calls with more than 6 args carry args[6..] out-of-line in extra_args,
 * covering the SysV/Win64 stack-passed portion of the call ABI. The cap is
 * an implementation sanity bound, not an ABI limit. */
#define NY_NIR_CALL_MAX_ARGS 64

typedef struct {
  const char *file;
  uint32_t line;
  uint32_t column;
} ny_nir_debug_loc_t;

typedef struct {
  bool has_min;
  bool has_max;
  int64_t min;
  int64_t max;
} ny_nir_range_t;

typedef struct {
  bool known_const;
  int64_t const_value;
  ny_nir_range_t range;
  size_t use_count;
  unsigned effects;
} ny_nir_value_fact_t;

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
} ny_nir_eval_result_t;

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
  unsigned effect_mask;
  size_t ops[NYIR_OP_COUNT];
} ny_nir_metadata_summary_t;

typedef bool (*ny_nir_call_resolver_t)(void *ctx, const char *symbol,
                                       const int64_t *args, size_t arg_count,
                                       int64_t *result, char *err,
                                       size_t err_len);

typedef struct {
  ny_nir_op_t op;
  int dst;
  int a;
  int b;
  int c;
  int d;
  int e;
  int f;
  int64_t imm;
  ny_nir_cmp_t cmp;
  const char *symbol;
  unsigned flags;
  unsigned effects;
  ny_nir_debug_loc_t debug;
  ny_nir_range_t range;
  /* NY_NIR_CALL args beyond the 6 carried in a..f (stack-passed ABI args).
   * Owned by the instruction; freed by ny_nir_func_free and by any pass
   * that discards the instruction. NULL/0 when unused. */
  int *extra_args;
  size_t extra_args_len;
  /* For NY_NIR_CALL: if non-NULL, an array of length imm (the call arity)
   * containing packed by-value aggregate size and SysV eightbyte classes.
   * Zero marks a scalar argument. Owned by the instruction. */
  uint32_t *arg_sizes;
} ny_nir_inst_t;

/* Decode and validate the positional value IDs carried by a call instruction.
 * Backends share this boundary so a..f/extra_args cannot drift by target. */
bool ny_nir_call_args(const ny_nir_inst_t *in, int value_count, int *args,
                      size_t args_cap, int *argc_out, char *err,
                      size_t err_len);

typedef struct {
  bool *value_f64;
  bool *value_f32;
  bool *local_f64;
  bool *local_f32;
  size_t value_count;
  size_t local_count;
} ny_nir_type_map_t;

typedef struct {
  ny_nir_inst_t *data;
  size_t len;
  size_t cap;
  int next_value;
  char **owned_symbols;
  size_t owned_symbols_len;
  size_t owned_symbols_cap;
} ny_nir_func_t;

bool ny_nir_type_map_init(ny_nir_type_map_t *map, const ny_nir_func_t *nir,
                          size_t local_count);
void ny_nir_type_map_free(ny_nir_type_map_t *map);

typedef struct {
  size_t before_insts;
  size_t after_insts;
  int before_values;
  int after_values;
  size_t before_ops[NYIR_OP_COUNT];
  size_t after_ops[NYIR_OP_COUNT];
  double pass_time_ms[9];  /* timing for each of the 9 optimizer passes */
} ny_nir_opt_stats_t;

void ny_nir_func_free(ny_nir_func_t *f);
int ny_nir_emit(ny_nir_func_t *f, ny_nir_inst_t inst);
/* Resets *in to a NOP, freeing all instruction-owned metadata. Used by
 * optimizer passes that discard an instruction in place. */
void ny_nir_inst_discard(ny_nir_inst_t *in);
bool ny_nir_verify(const ny_nir_func_t *f, char *err, size_t err_len);
bool ny_nir_validate_constraints(const ny_nir_func_t *f, char *err,
                                 size_t err_len);
bool ny_nir_metadata_summary(const ny_nir_func_t *f,
                             ny_nir_metadata_summary_t *summary, char *err,
                             size_t err_len);
void ny_nir_metadata_summary_dump(FILE *out, const char *name,
                                  const ny_nir_metadata_summary_t *summary);
bool ny_nir_analyze_values(const ny_nir_func_t *f, ny_nir_value_fact_t *facts,
                           size_t fact_count, char *err, size_t err_len);
bool ny_nir_eval(const ny_nir_func_t *f, int64_t *locals, size_t local_count,
                 size_t max_steps, ny_nir_eval_result_t *result, char *err,
                 size_t err_len);
void ny_nir_eval_result_dump(FILE *out, const char *name,
                              const ny_nir_eval_result_t *result);
bool ny_nir_eval_with_calls(const ny_nir_func_t *f, int64_t *locals,
                            size_t local_count, size_t max_steps,
                            ny_nir_eval_result_t *result,
                            ny_nir_call_resolver_t resolver, void *resolver_ctx,
                            char *err, size_t err_len);
void ny_nir_refresh_metadata(ny_nir_func_t *f);
unsigned ny_nir_inst_effects(const ny_nir_inst_t *inst);
void ny_nir_dump(FILE *out, const ny_nir_func_t *f, const char *name);
void ny_nir_dump_stats(FILE *out, const ny_nir_opt_stats_t *stats);
bool ny_nir_dump_binary(FILE *out, const ny_nir_func_t *f, const char *name);
/* Loads into an initialized function; previous contents are freed. */
bool ny_nir_load_binary(FILE *in, ny_nir_func_t *out, char *name,
                        size_t name_len, char *err, size_t err_len);
bool ny_nir_const_fold(ny_nir_func_t *f);
bool ny_nir_copy_prop(ny_nir_func_t *f);
bool ny_nir_peephole(ny_nir_func_t *f);
bool ny_nir_dce(ny_nir_func_t *f);
bool ny_nir_cfg_simplify(ny_nir_func_t *f);
bool ny_nir_compact(ny_nir_func_t *f);
bool ny_nir_optimize_with_stats(ny_nir_func_t *f, ny_nir_opt_stats_t *stats);
bool ny_nir_optimize(ny_nir_func_t *f);
bool ny_nir_optimize_debug(ny_nir_func_t *f, FILE *dump, ny_nir_opt_stats_t *stats);
const char *ny_nir_opt_pass_name(int pass);
const char *ny_nir_op_name(ny_nir_op_t op);

#endif
