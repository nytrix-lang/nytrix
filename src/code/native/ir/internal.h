#ifndef NY_NATIVE_IR_INTERNAL_H
#define NY_NATIVE_IR_INTERNAL_H

#include "code/native/ir.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* init.c — bit cast helpers shared by eval and display. */
int64_t nyir_f64_to_bits(double v);
double nyir_bits_to_f64(int64_t bits);
int64_t nyir_f32_to_bits(float v);
float nyir_bits_to_f32(int64_t bits);

/* init.c — error helpers used by verify, eval, and opt. */
bool nyir_err(char *err, size_t err_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
bool nyir_inst_err(char *err, size_t err_len, const nyir_inst_t *in,
                     size_t index, const char *reason);

/* init.c — stats collection used by optimize. */
void nyir_collect_stats(const nyir_func_t *f, size_t *insts,
                          int *values, size_t *ops, size_t op_count);

/* verify.c — helpers used by eval and opt. */
bool nyir_analyze_binary_fold(nyir_op_t op, int64_t a, int64_t b,
                                int64_t *out);
bool nyir_analyze_cmp_fold(nyir_cmp_t cmp, int64_t a, int64_t b,
                             int64_t *out);
bool nyir_label_referenced(const nyir_func_t *f, int64_t label);

/* ssa.c — repair PHIs after CFG edge deletion. */
bool nyir_prune_phis(nyir_func_t *f);

#endif
