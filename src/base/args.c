#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "args.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int ny_arg_match(const char *arg, const char *long_name, const char *short_name) {
  if (!arg || !*arg)
    return 0;
  if (long_name && strcmp(arg, long_name) == 0)
    return 1;
  if (short_name && strcmp(arg, short_name) == 0)
    return 1;
  return 0;
}

int ny_arg_match_with_value(const char *arg, const char *long_name) {
  if (!arg || !long_name)
    return 0;
  if (strcmp(arg, long_name) == 0)
    return 1;
  size_t n = strlen(long_name);
  return strncmp(arg, long_name, n) == 0 && arg[n] == '=';
}

int ny_arg_take_value(const char *arg, int *idx, int argc, char **argv, const char **out_value,
                      char *err, size_t err_sz) {
  if (!arg || !idx || !out_value)
    return 0;
  const char *eq = strchr(arg, '=');
  if (eq && eq[1] != '\0') {
    *out_value = eq + 1;
    return 1;
  }
  if (*idx + 1 >= argc) {
    if (err && err_sz)
      snprintf(err, err_sz, "missing value for %s", arg);
    return 0;
  }
  *out_value = argv[++(*idx)];
  return 1;
}

int ny_arg_parse_int(const char *raw, int min_v, int max_v, int *out, const char *label, char *err,
                     size_t err_sz) {
  if (!raw || !*raw || !out)
    return 0;
  char *end = NULL;
  long v = strtol(raw, &end, 10);
  if (!end || *end != '\0') {
    if (err && err_sz)
      snprintf(err, err_sz, "invalid %s: %s", label ? label : "integer", raw);
    return 0;
  }
  if (v < min_v || v > max_v) {
    if (err && err_sz)
      snprintf(err, err_sz, "%s out of range [%d,%d]: %s", label ? label : "integer", min_v, max_v,
               raw);
    return 0;
  }
  *out = (int)v;
  return 1;
}

int ny_arg_take_int(const char *arg, int *idx, int argc, char **argv, int min_v, int max_v,
                    int *out, const char *label, char *err, size_t err_sz) {
  const char *v = NULL;
  return ny_arg_take_value(arg, idx, argc, argv, &v, err, err_sz) &&
         ny_arg_parse_int(v, min_v, max_v, out, label, err, err_sz);
}

int ny_arg_parse_strict_bool(const char *raw, int *out) {
  if (!raw || !out)
    return 0;
  if (strcmp(raw, "1") == 0 || strcmp(raw, "true") == 0 || strcmp(raw, "on") == 0 ||
      strcmp(raw, "yes") == 0) {
    *out = 1;
    return 1;
  }
  if (strcmp(raw, "0") == 0 || strcmp(raw, "false") == 0 || strcmp(raw, "off") == 0 ||
      strcmp(raw, "no") == 0) {
    *out = 0;
    return 1;
  }
  return 0;
}

int ny_arg_parse_color_mode(const char *raw, int *out_mode) {
  if (!raw || !*raw || !out_mode)
    return 0;
  if (strcmp(raw, "always") == 0 || strcmp(raw, "on") == 0 || strcmp(raw, "1") == 0 ||
      strcasecmp(raw, "true") == 0 || strcasecmp(raw, "yes") == 0) {
    *out_mode = 1;
    return 1;
  }
  if (strcmp(raw, "never") == 0 || strcmp(raw, "off") == 0 || strcmp(raw, "0") == 0 ||
      strcasecmp(raw, "false") == 0 || strcasecmp(raw, "no") == 0) {
    *out_mode = 0;
    return 1;
  }
  if (strcmp(raw, "auto") == 0 || strcasecmp(raw, "tty") == 0 || strcasecmp(raw, "default") == 0) {
    *out_mode = -1;
    return 1;
  }
  return 0;
}

int ny_arg_consume_color(int *idx, int argc, char **argv, int *out_mode, char *err, size_t err_sz) {
  if (!idx || !argv || *idx < 0 || *idx >= argc)
    return 0;
  const char *a = argv[*idx];
  if (!a || !*a)
    return 0;
  if (strcmp(a, "--no-color") == 0) {
    if (out_mode)
      *out_mode = 0;
    return 1;
  }
  if (!ny_arg_match_with_value(a, "--color"))
    return 0;

  const char *v = NULL;
  if (!ny_arg_take_value(a, idx, argc, argv, &v, err, err_sz))
    return -1;
  int mode = 0;
  if (!ny_arg_parse_color_mode(v, &mode)) {
    if (err && err_sz)
      snprintf(err, err_sz, "invalid color mode '%s' (expected auto|always|never)", v);
    return -1;
  }
  if (out_mode)
    *out_mode = mode;
  return 1;
}

void ny_arg_apply_color_mode(int mode) {
  if (mode > 0)
    ny_setenv("NYTRIX_TOOL_COLOR", "always", 1);
  else if (mode == 0)
    ny_setenv("NYTRIX_TOOL_COLOR", "never", 1);
  else
    ny_setenv("NYTRIX_TOOL_COLOR", "auto", 1);
}

void ny_arg_unknown(const char *tool, const char *arg) {
  fprintf(stderr, "%s: unknown option: %s\n", tool ? tool : "tool", arg ? arg : "(null)");
}
