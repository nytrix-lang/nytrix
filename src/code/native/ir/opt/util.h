#ifndef NYIR_OPT_UTIL_H
#define NYIR_OPT_UTIL_H

#include "code/native/ir.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"

/* Float folding helpers used by const_fold and sccp. */
bool nyir_float_fold_binary(nyir_op_t op, int64_t a_imm, int64_t b_imm,
                              unsigned char kind, int64_t *out_imm,
                              unsigned char *out_kind);

/* Constant collection used by peephole and cfg_simplify. */
bool nir_collect_consts(const nyir_func_t *f, bool *known, int64_t *value);

/* Instruction shape constructors used by peephole and strength_reduce. */
void nir_make_copy(nyir_inst_t *in, int src);
void nir_make_const(nyir_inst_t *in, int64_t value);
void nir_make_f64_const(nyir_inst_t *in, int64_t bitcast);
void nir_make_f32_const(nyir_inst_t *in, int64_t bitcast);

/* Compare/range helpers used by peephole. */
bool nir_cmp_same_value(nyir_cmp_t cmp, int64_t *out);
bool nir_cmp_range_fold(nyir_cmp_t cmp, const nyir_range_t *a,
                        const nyir_range_t *b, int64_t *out);
bool nir_range_excludes_zero(const nyir_range_t *r);
bool nir_range_excludes_int64_min(const nyir_range_t *r);

/* Value range recovery used by peephole and strength_reduce. */
bool nir_recover_load_local_range(const nyir_func_t *f,
                                  const nyir_value_fact_t *facts, int value,
                                  size_t at, nyir_range_t *out);
bool nir_value_range_at(const nyir_func_t *f,
                        const nyir_value_fact_t *facts, int value, size_t at,
                        nyir_range_t *out);
bool nir_operands_same_value(const nyir_func_t *f, int a, int b, size_t at);

/* Rewriting helpers used by peephole and strength_reduce. */
int nir_find_block_const0(const nyir_func_t *f, size_t at);
bool nir_rewrite_neg(nyir_func_t *f, size_t *i, int src);
bool nir_ensure_inst_space(nyir_func_t *f, size_t extra);
size_t nir_next_non_nop(const nyir_func_t *f, size_t start);

/* Value remapping used by compact. */
bool nir_remap_value(const int *map, int map_len, int value, int *out);

/* Function-level queries used by the optimization pipeline. */
bool nyir_has_control_flow(const nyir_func_t *f);
bool nyir_has_loop(const nyir_func_t *f);
bool nyir_has_local_mem(const nyir_func_t *f);
size_t nyir_count_nops(const nyir_func_t *f);
bool nyir_compact_if_sparse(nyir_func_t *f);
size_t nyir_block_count(const nyir_func_t *f);
uint64_t nyir_debug_fingerprint(const nyir_func_t *f);
void nyir_pass_tag(char *buf, size_t n, const char *pass_name);

#endif /* NYIR_OPT_UTIL_H */
