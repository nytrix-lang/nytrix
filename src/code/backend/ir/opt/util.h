#ifndef NYIR_OPT_UTIL_H
#define NYIR_OPT_UTIL_H

#include "base/common.h"
#include "base/compat.h"
#include "base/trace.h"
#include "code/backend/ir/internal.h"
#include <nyr/ir.h>

/* Float folding helpers used by const_fold and sccp. */
bool nyr_float_fold_binary(nyr_op_t op, int64_t a_imm, int64_t b_imm,
                            unsigned char kind, int64_t *out_imm,
                            unsigned char *out_kind);

/* Constant collection used by peephole and cfg_simplify. */
bool nir_collect_consts(const nyr_func_t *f, bool *known, int64_t *value);
/* Kind-aware variant: kind[v] is 1=i64, 2=f64, 3=f32, 0=unknown. */
bool nir_collect_consts_kind(const nyr_func_t *f, bool *known, int64_t *value,
                             unsigned char *kind);

/* Instruction shape constructors used by peephole and strength_reduce. */
void nir_make_copy(nyr_inst_t *in, int src);
void nir_make_const(nyr_inst_t *in, int64_t value);
void nir_make_f64_const(nyr_inst_t *in, int64_t bitcast);
void nir_make_f32_const(nyr_inst_t *in, int64_t bitcast);

/* Drop analysis-derived facts when a rewrite changes an instruction's
 * operation or operands. Semantic/debug metadata is preserved. */
void nyr_clear_derived_facts(nyr_inst_t *in);
bool nyr_inst_payload_free(const nyr_inst_t *in);

/* Compare/range helpers used by peephole. */
bool nir_cmp_same_value(nyr_cmp_t cmp, int64_t *out);
nyr_cmp_t nyr_cmp_swap_operands(nyr_cmp_t cmp);
bool nir_cmp_range_fold(nyr_cmp_t cmp, const nyr_range_t *a,
                        const nyr_range_t *b, int64_t *out);
bool nir_range_excludes_zero(const nyr_range_t *r);
bool nir_range_excludes_int64_min(const nyr_range_t *r);
bool nyr_value_fact_singleton(const nyr_value_fact_t *fact, int64_t *value);
bool nyr_const_i64(const nyr_func_t *f, const int *defs, int value,
                    int64_t *out);

/* Value range recovery used by peephole and strength_reduce. */
bool nir_recover_load_local_range(const nyr_func_t *f,
                                  const nyr_value_fact_t *facts, int value,
                                  size_t at, nyr_range_t *out);
bool nir_value_range_at(const nyr_func_t *f, const nyr_value_fact_t *facts,
                        int value, size_t at, nyr_range_t *out);
bool nir_operands_same_value(const nyr_func_t *f, int a, int b, size_t at);

/* Rewriting helpers used by peephole and strength_reduce. */
int nir_find_block_const0(const nyr_func_t *f, size_t at);
bool nir_rewrite_neg(nyr_func_t *f, size_t *i, int src);
bool nir_rewrite_shl(nyr_func_t *f, size_t *i, int src, int shift);
bool nir_ensure_inst_space(nyr_func_t *f, size_t extra);
size_t nir_next_non_nop(const nyr_func_t *f, size_t start);

/* Value remapping used by compact. */
bool nir_remap_value(const int *map, int map_len, int value, int *out);

typedef struct {
  bool address_taken;
  bool passed_to_call;
  bool returned;
  bool stored_to_memory;
  bool ffi_escape;
  bool thread_escape;
  bool unknown_call_escape;
  bool escapes;
  size_t first_escape_pc;
} nyr_local_escape_info_t;

/* Shared local-slot escape classification for SROA/alias/reporting passes. */
size_t nyr_local_slot_count(const nyr_func_t *f);
bool nyr_analyze_local_escapes(const nyr_func_t *f,
                                nyr_local_escape_info_t *info,
                                size_t slot_count);

/* Function-level queries used by the optimization pipeline. */
bool nyr_has_control_flow(const nyr_func_t *f);
bool nyr_has_loop(const nyr_func_t *f);
bool nyr_has_local_mem(const nyr_func_t *f);
/* SSA def index shared by loop/vectorizer passes. */
int *nyr_build_defs(const nyr_func_t *f);
size_t nyr_count_nops(const nyr_func_t *f);
bool nyr_compact_if_sparse(nyr_func_t *f);
size_t nyr_block_count(const nyr_func_t *f);
/* One-past-the-last non-NOP instruction index in a CFG block. */
size_t nyr_block_last_non_nop(const nyr_func_t *f, const nyr_cfg_t *cfg,
                               size_t block);
int64_t nyr_fresh_label(const nyr_func_t *f);
uint64_t nyr_debug_fingerprint(const nyr_func_t *f);
void nyr_pass_tag(char *buf, size_t n, const char *pass_name);
bool nyr_sroa_unified(nyr_func_t *f);

#endif /* NYIR_OPT_UTIL_H */
