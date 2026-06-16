#pragma once

#include "base/common.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#define isatty _isatty
#endif

#define NYT_RESET "\033[0m"
#define NYT_BOLD "\033[1m"
#define NYT_RED "\033[31m"
#define NYT_GREEN "\033[32m"
#define NYT_YELLOW "\033[33m"
#define NYT_BLUE "\033[34m"
#define NYT_MAGENTA "\033[35m"
#define NYT_CYAN "\033[36m"
#define NYT_GRAY "\033[90m"
#define NYT_UNDER "\033[4m"

static inline int nyt_color_mode(const char *mode, int *out) {
  if (!mode || !*mode)
    return 0;
  if (strcmp(mode, "always") == 0 || strcmp(mode, "on") == 0 || strcmp(mode, "1") == 0 ||
      strcasecmp(mode, "true") == 0 || strcasecmp(mode, "yes") == 0) {
    *out = 1;
    return 1;
  }
  if (strcmp(mode, "never") == 0 || strcmp(mode, "off") == 0 || strcmp(mode, "0") == 0 ||
      strcasecmp(mode, "false") == 0 || strcasecmp(mode, "no") == 0) {
    *out = 0;
    return 1;
  }
  return 0;
}

static inline int nyt_color_enabled(void) {
  int out = 0;
  if (nyt_color_mode(getenv("NYTRIX_TOOL_COLOR"), &out))
    return out;
  if (nyt_color_mode(getenv("NYTRIX_COLOR"), &out))
    return out;
  if (getenv("NO_COLOR"))
    return 0;
  if (ny_env_truthy(getenv("CLICOLOR_FORCE")) || ny_env_truthy(getenv("FORCE_COLOR")))
    return 1;
  const char *term_program = getenv("TERM_PROGRAM");
  if (term_program && strcmp(term_program, "vscode") == 0)
    return 1;
  return isatty(STDOUT_FILENO) != 0;
}

static inline const char *nyt_clr(const char *code) { return nyt_color_enabled() ? code : ""; }
