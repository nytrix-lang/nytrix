/* Shared internal NYIR verification, formatting, and optimization helpers. */
#ifndef NY_NATIVE_IR_INTERNAL_H
#define NY_NATIVE_IR_INTERNAL_H

#include <nyr/ir.h>

bool nyr_err(char *err, size_t err_len, const char *fmt, ...);
bool nyr_fmt_inst(const nyr_inst_t *in, char *buf, size_t n);
bool nyr_inst_err(char *err, size_t err_len, const nyr_inst_t *inst,
                  size_t index, const char *reason);
bool nyr_local_label_symbol_id(const char *symbol, int64_t *label);
bool nyr_label_referenced(const nyr_func_t *f, int64_t label);
bool nyr_prune_phis(nyr_func_t *f);
bool nyr_promote_private_entry_globals(nyr_func_t *f);
bool nyr_recursive_add_to_loop(nyr_func_t *f, const char *func_name);
const char *nyr_scalar_class_name(nyr_scalar_class_t scalar_class);

int64_t nyr_f64_to_bits(double value);
double nyr_bits_to_f64(int64_t bits);
int64_t nyr_f32_to_bits(float value);
float nyr_bits_to_f32(int64_t bits);

bool nyr_analyze_binary_fold(nyr_op_t op, int64_t lhs, int64_t rhs,
                             int64_t *out);
bool nyr_analyze_cmp_fold(nyr_cmp_t cmp, int64_t lhs, int64_t rhs,
                          int64_t *out);

void nyr_eval_globals_clear(void);
void nyr_collect_stats(const nyr_func_t *f, size_t *insts, int *values,
                       size_t *ops, size_t op_count);
/*
 * Evaluate a function that the caller has already verified and will keep
 * immutable for the duration of this evaluation.
 */
bool nyr_ir_eval_with_calls_profile_verified(
    const nyr_func_t *f, int64_t *locals, size_t local_count, size_t max_steps,
    nyr_eval_result_t *result, bool collect_profile,
    nyr_call_resolver_t resolver, void *resolver_ctx, char *err,
    size_t err_len);

#endif
