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
int64_t ny_nir_f64_to_bits(double v);
double ny_nir_bits_to_f64(int64_t bits);
int64_t ny_nir_f32_to_bits(float v);
float ny_nir_bits_to_f32(int64_t bits);

/* init.c — error helpers used by verify, eval, and opt. */
bool ny_nir_err(char *err, size_t err_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
bool ny_nir_inst_err(char *err, size_t err_len, const ny_nir_inst_t *in,
                     size_t index, const char *reason);

/* init.c — stats collection used by optimize. */
void ny_nir_collect_stats(const ny_nir_func_t *f, size_t *insts,
                          int *values, size_t *ops, size_t op_count);

/* verify.c — helpers used by eval and opt. */
bool ny_nir_analyze_binary_fold(ny_nir_op_t op, int64_t a, int64_t b,
                                int64_t *out);
bool ny_nir_analyze_cmp_fold(ny_nir_cmp_t cmp, int64_t a, int64_t b,
                             int64_t *out);
bool ny_nir_label_referenced(const ny_nir_func_t *f, int64_t label);

#endif
