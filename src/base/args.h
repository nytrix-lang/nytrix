#ifndef NYTRIX_BASE_ARGS_H
#define NYTRIX_BASE_ARGS_H

#include "base/compat.h"

#include <stddef.h>

int ny_arg_match(const char *arg, const char *long_name, const char *short_name);
int ny_arg_match_with_value(const char *arg, const char *long_name);
int ny_arg_take_value(const char *arg, int *idx, int argc, char **argv, const char **out_value,
                      char *err, size_t err_sz);
int ny_arg_parse_int(const char *raw, int min_v, int max_v, int *out, const char *label, char *err,
                     size_t err_sz);
int ny_arg_take_int(const char *arg, int *idx, int argc, char **argv, int min_v, int max_v,
                    int *out, const char *label, char *err, size_t err_sz);
int ny_arg_parse_strict_bool(const char *raw, int *out);
int ny_arg_parse_color_mode(const char *raw, int *out_mode);
int ny_arg_consume_color(int *idx, int argc, char **argv, int *out_mode, char *err, size_t err_sz);
void ny_arg_apply_color_mode(int mode);
void ny_arg_unknown(const char *tool, const char *arg);

#endif
