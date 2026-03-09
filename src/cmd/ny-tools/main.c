#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "tool.h"

#ifdef NY_TOOL_HAS_FMT
#include "fmt.h"
#endif
#ifdef NY_TOOL_HAS_PERF
#include "perf.h"
#endif
#ifdef NY_TOOL_HAS_TEST
#include "test.h"
#endif
#ifdef NY_TOOL_HAS_WEB
#include "web.h"
#endif
#ifdef NY_TOOL_HAS_MAKE
#include "make.h"
#endif

#ifndef NY_TOOL_ENTRY
#define NY_TOOL_ENTRY ""
#endif

static const char *ny_basename(const char *p) {
  if (!p || !*p)
    return "";
  const char *s1 = strrchr(p, '/');
  const char *s2 = strrchr(p, '\\');
  const char *s = s1;
  if (s2 && (!s || s2 > s))
    s = s2;
  return s ? s + 1 : p;
}

static const char *ny_tool_from_name(const char *argv0, int argc, char **argv) {
  if (NY_TOOL_ENTRY[0] != '\0')
    return NY_TOOL_ENTRY;
  const char *base = ny_basename(argv0);
  if (strcmp(base, "ny-fmt") == 0)
    return "fmt";
  if (strcmp(base, "ny-perf") == 0)
    return "perf";
  if (strcmp(base, "ny-test") == 0)
    return "test";
  if (strcmp(base, "ny-doc") == 0)
    return "doc";
  if (strcmp(base, "ny-make") == 0)
    return "make";
  if (argc > 1 && argv[1] && strncmp(argv[1], "ny-", 3) == 0)
    return argv[1] + 3;
  return "";
}

static int ny_tool_filter_global_args(int *argc, char **argv) {
  char err[256];
  int w = 1;
  for (int r = 1; r < *argc; r++) {
    int color_mode = -2;
    int color_idx = r;
    int color_rc = ny_arg_consume_color(&color_idx, *argc, argv, &color_mode, err, sizeof(err));
    if (color_rc < 0) {
      nyt_err("ny-tools", "%s", err);
      return 0;
    }
    if (color_rc > 0) {
      ny_arg_apply_color_mode(color_mode);
      r = color_idx;
      continue;
    }
    argv[w++] = argv[r];
  }
  *argc = w;
  argv[w] = NULL;
  return 1;
}

int main(int argc, char **argv) {
  if (!ny_tool_filter_global_args(&argc, argv))
    return 2;
  const char *tool = ny_tool_from_name(argv[0], argc, argv);
  if (!tool || !*tool) {
    nyt_err("ny-tools", "unknown tool entry");
    return 2;
  }

  int extra = 0;
  if (NY_TOOL_ENTRY[0] == '\0' && argc > 1 && argv[1] && strncmp(argv[1], "ny-", 3) == 0)
    extra = 1; /* consume subcommand when invoked as `ny-tools ny-fmt ...` */

  if (strcmp(tool, "fmt") == 0) {
#ifdef NY_TOOL_HAS_FMT
    return ny_fmt_main(argc - extra, argv + extra);
#else
    nyt_err("ny-fmt", "this binary was built without native fmt support");
    return 1;
#endif
  }
  if (strcmp(tool, "perf") == 0) {
#ifdef NY_TOOL_HAS_PERF
    return ny_perf_main(argc - extra, argv + extra);
#else
    nyt_err("ny-perf", "this binary was built without native perf support");
    return 1;
#endif
  }
  if (strcmp(tool, "test") == 0) {
#ifdef NY_TOOL_HAS_TEST
    return ny_test_main(argc - extra, argv + extra);
#else
    nyt_err("ny-test", "this binary was built without native test support");
    return 1;
#endif
  }
  if (strcmp(tool, "doc") == 0) {
#ifdef NY_TOOL_HAS_WEB
    return ny_web_main(argc - extra, argv + extra);
#else
    nyt_err("ny-doc", "this binary was built without native docs support");
    return 1;
#endif
  }
  if (strcmp(tool, "make") == 0) {
#ifdef NY_TOOL_HAS_MAKE
    return ny_make_main(argc - extra, argv + extra);
#else
    nyt_err("ny-make", "this binary was built without native make support");
    return 1;
#endif
  }

  nyt_err("ny-tools", "unsupported tool '%s'", tool);
  return 2;
}
