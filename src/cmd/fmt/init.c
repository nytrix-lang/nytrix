/*
 * ny-fmt entry point: CLI dispatch for the formatter, source converter
 * (c2ny/py2ny), and code extraction tools bundled under `ny fmt`.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "../tools/repo.h"
#include "base/args.h"
#include "base/process.h"
#include "base/util.h"
#include "fmt.h"
#include "ui.h"
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "base/dirent.h"
#else
#include <dirent.h>
#endif
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#endif
#ifdef NYTRIX_EMBEDDED_TOOLS
#include <nytrix/nytrix.h>
#endif

static int fmt_collect_via_library(const char *const *roots, size_t root_count,
                                   const char *mode, StrVec *out);
#include "core.c"

/*
 * Deterministic Nytrix formatter property smoke tests.  The formatter is used
 * on partially typed editor buffers as well as complete source files, so its
 * strongest always-on contract is: formatting must not crash, a second format
 * must be a no-op, and comments / multiline string contents must survive.
 *
 * This is intentionally generated from a fixed PRNG seed rather than depending
 * on a fuzzing framework: `ny fmt --selftest` remains reproducible on every
 * platform while still exercising hundreds of whitespace/layout variants.
 */
static uint32_t ny_fmt_selftest_rand(uint32_t *state) {
  uint32_t x = *state ? *state : UINT32_C(0x9e3779b9);
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static void ny_fmt_selftest_ws(char *out, size_t cap, uint32_t *state) {
  if (!out || cap == 0)
    return;
  size_t n = (size_t)(ny_fmt_selftest_rand(state) % 5u);
  if (n + 1 > cap)
    n = cap - 1;
  for (size_t i = 0; i < n; ++i)
    out[i] = (ny_fmt_selftest_rand(state) & 1u) ? ' ' : '\t';
  out[n] = '\0';
}

static int ny_fmt_ny_selftest(void) {
  static const char *const comptime_aliases =
      "use core\n"
      "def legacy = comptime { 2^5 }\n"
      "def scoped = #comptime { return legacy + 1 }\n"
      "def concise = #run 2^5\n"
      "#template make_axis(axis) {\n"
      "  fn generated_axis_${axis}() { return axis }\n"
      "}\n"
      "for axis in #run [\"x\", \"y\"] {\n"
      "  #insert make_axis(axis)\n"
      "}\n";
  static const char *const malformed[] = {
      "def broken = {\n",
      "if true {\n  print(1)\n",
      "def x = \"unterminated\n",
      "def x = \"\"\"unterminated multiline\n",
      ";; comment-only editor buffer\n}\n}\n",
      "[1, 2, 3\n",
      "def f(x) = {\n  if x { return (x + 1\n}\n",
  };
  char *alias_once = format_ny_text(comptime_aliases);
  char *alias_twice = alias_once ? format_ny_text(alias_once) : NULL;
  if (!alias_once || !alias_twice || strcmp(alias_once, alias_twice) != 0 ||
      !strstr(alias_once, "comptime") || !strstr(alias_once, "#comptime") ||
      !strstr(alias_once, "#run") || !strstr(alias_once, "#insert")) {
    fprintf(stderr, "ny-fmt Nytrix selftest: comptime alias "
                    "formatting/idempotence failed\n");
    free(alias_once);
    free(alias_twice);
    return 1;
  }
  free(alias_once);
  free(alias_twice);
  uint32_t state = UINT32_C(0x4e595452); /* 'NYTR' */
  for (size_t i = 0; i < 256; ++i) {
    char a[16], b[16], c[16], d[16], e[16], f[16];
    ny_fmt_selftest_ws(a, sizeof(a), &state);
    ny_fmt_selftest_ws(b, sizeof(b), &state);
    ny_fmt_selftest_ws(c, sizeof(c), &state);
    ny_fmt_selftest_ws(d, sizeof(d), &state);
    ny_fmt_selftest_ws(e, sizeof(e), &state);
    ny_fmt_selftest_ws(f, sizeof(f), &state);
    char src[4096];
    int n =
        snprintf(src, sizeof(src),
                 ";; ny-fmt-selftest comment %zu\n"
                 "def%svalue%s=%s1\n"
                 "def%smessage%s=%s\"\"\"doc-%zu\\nkept\"\"\"\n"
                 "if%svalue%s>%s0%s{%s\n"
                 "%sdef%snext%s=%svalue%s+%s1\n"
                 "%s}\n",
                 i, a, b, c, d, e, f, i, a, b, c, d, e, (i & 1u) ? "\t" : "  ",
                 a, b, c, d, e, (i & 2u) ? "\t" : "  ");
    if (n < 0 || (size_t)n >= sizeof(src)) {
      fprintf(stderr, "ny-fmt Nytrix selftest: generated probe overflow\n");
      return 1;
    }
    char *once = format_ny_text(src);
    char *twice = once ? format_ny_text(once) : NULL;
    char comment[96], doc[96];
    snprintf(comment, sizeof(comment), ";; ny-fmt-selftest comment %zu", i);
    snprintf(doc, sizeof(doc), "doc-%zu\\nkept", i);
    if (!once || !twice || strcmp(once, twice) != 0 || !strstr(once, comment) ||
        !strstr(once, doc)) {
      fprintf(stderr,
              "ny-fmt Nytrix selftest: idempotence/comment preservation failed "
              "at seed-case %zu\n",
              i);
      free(once);
      free(twice);
      return 1;
    }
    free(once);
    free(twice);
  }
  for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
    char *once = format_ny_text(malformed[i]);
    char *twice = once ? format_ny_text(once) : NULL;
    if (!once || !twice || strcmp(once, twice) != 0) {
      fprintf(stderr,
              "ny-fmt Nytrix selftest: malformed-buffer recovery failed at "
              "case %zu\n",
              i);
      free(once);
      free(twice);
      return 1;
    }
    free(once);
    free(twice);
  }
  printf("ny-fmt selftest: Nytrix idempotence/recovery generated corpus: ok "
         "(264 cases)\n");
  return 0;
}
static void usage(void) {
  nyt_heading("Nytrix Format And Audit");
  nyt_subheading("Usage");
  printf("%susage:%s %sny fmt%s %s[mode] [options] [paths ...]%s\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN),
         nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny fmt --cloc%s %s[--full] [--top N] [paths ...]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("       %sny fmt --dupes%s %s[--dupes-min N] [--dupes-emit] [--json] "
         "[paths ...]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("       %sny fmt --conv%s %s--input file.texi --name NAME [--format "
         "man|md] [-o out]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("       %sny fmt --migrate-operators --assume-legacy-operators%s "
         "%s[paths ...]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  nyt_subheading("Modes");
  printf("  %s--check --fix --analyze --audit --selftest --trim --syntax "
         "--types --dead%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf(
      "  Formatting is read-only by default; pass --fix to rewrite files.\n");
  printf("  %s--smart --overhaul --bugs --checks --bloat --modules --profiles "
         "--layouts --loops%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--contracts --ffi --constants --specialize --metaprog "
         "--constfold%s\n\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  nyt_subheading("Options");
  printf("  %s--json --tidy --optimize --apply --diff%s\n", nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("  %s--color MODE --limit N --threshold N --root DIR --dirs DIR%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--min-sev CRIT|HIGH|MED|LOW --types-strict -v%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  audit modes compose; use %s--audit=loops,trim%s to find "
         "continue/guard-loop flattening wins\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  accept justified smells with %sny-fmt: accept NYAUDxxxx reason%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--selftest%s validates C range analysis and high-confidence "
         "language-pattern checks\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
}

static char *str_replace_all(const char *in, const char *pat, const char *rep) {
  if (!in || !pat || !*pat || !rep)
    return in ? strdup(in) : NULL;
  size_t in_n = strlen(in), p_n = strlen(pat), r_n = strlen(rep);
  size_t count = 0;
  for (const char *p = strstr(in, pat); p; p = strstr(p + p_n, pat))
    count++;
  size_t out_n = in_n + count * (r_n - p_n) + 1;
  char *out = (char *)malloc(out_n);
  if (!out)
    return NULL;
  char *dst = out;
  const char *cur = in;
  while (1) {
    const char *p = strstr(cur, pat);
    if (!p) {
      size_t tail = strlen(cur);
      memmove(dst, cur, tail);
      dst += tail;
      break;
    }
    size_t chunk = (size_t)(p - cur);
    memmove(dst, cur, chunk);
    dst += chunk;
    memmove(dst, rep, r_n);
    dst += r_n;
    cur = p + p_n;
  }
  *dst = '\0';
  return out;
}

typedef struct {
  const char *pat;
  const char *rep;
} ReplaceRule;

static void replace_rules_owned(char **s, const ReplaceRule *rules) {
  if (!s || !*s || !rules)
    return;
  for (int i = 0; rules[i].pat; i++) {
    char *tmp =
        str_replace_all(*s, rules[i].pat, rules[i].rep ? rules[i].rep : "");
    free(*s);
    *s = tmp ? tmp : strdup("");
  }
}

static char *convert_texi_basic(const char *input, const char *name,
                                const char *fmt, const char *section) {
  char *s = strdup(input ? input : "");
  if (!s)
    return NULL;
  const ReplaceRule drops[] = {
      {"@contents", ""},    {"@appendix", ""},  {"@printindex", ""},
      {"@node", ""},        {"@menu", ""},      {"@dircategory", ""},
      {"@direntry", ""},    {"@titlepage", ""}, {"\\input texinfo", ""},
      {"@setfilename", ""}, {"@settitle", ""},  {NULL, NULL}};
  replace_rules_owned(&s, drops);
  if (strcmp(fmt, "md") == 0) {
    const ReplaceRule md_rules[] = {
        {"@chapter ", "# "}, {"@section ", "## "}, {"@subsection ", "### "},
        {"@code{", "`"},     {"}", "`"},           {NULL, NULL}};
    replace_rules_owned(&s, md_rules);
    char head[256];
    snprintf(head, sizeof(head), "# %s\n\n", name ? name : "Nytrix");
    size_t n = strlen(head) + strlen(s) + 2;
    char *out = (char *)malloc(n);
    if (!out) {
      free(s);
      return NULL;
    }
    snprintf(out, n, "%s%s", head, s);
    free(s);
    return out;
  }

  char header[512];
  snprintf(header, sizeof(header), ".TH %s %s \"\" \"\" \"Nytrix\"\n",
           name ? name : "nytrix", section ? section : "1");
  const ReplaceRule man_rules[] = {
      {"@chapter ", ".SH "}, {"@section ", ".SH "}, {"@subsection ", ".SS "},
      {"@code{", "\\fB"},    {"}", "\\fP"},         {NULL, NULL}};
  replace_rules_owned(&s, man_rules);
  size_t n = strlen(header) + strlen(s) + 2;
  char *out = (char *)malloc(n);
  if (!out) {
    free(s);
    return NULL;
  }
  snprintf(out, n, "%s%s", header, s);
  free(s);
  return out;
}

#include "c2ny.c"
#include "py2ny.c"

static int run_py2ny(const char *input_path, const char *output_path) {
  size_t n = 0;
  char *src = ny_read_file_raw(input_path, &n);
  if (!src) {
    nyt_err("ny-fmt", "py2ny: failed to read %s", input_path);
    return 1;
  }

  pny_sb_t out = {0};
  pny_sb_add(&out, ";; Generated by ny-fmt --py2ny from ");
  pny_sb_add(&out, input_path);
  pny_sb_add(&out, "\nuse core\n\n");

  int rc = pny_convert(src, n, &out);

  free(src);

  if (rc != 0) {
    nyt_err("ny-fmt",
            "py2ny: unsupported constructs remain in %s; see markers in output",
            input_path);
  }

  if (ny_write_file(output_path, out.data, out.len) != 0) {
    nyt_err("ny-fmt", "py2ny: failed to write %s", output_path);
    free(out.data);
    return 1;
  }

  printf("py2ny: %s -> %s (%zu bytes)\n", input_path, output_path, out.len);
  free(out.data);
  return rc;
}

static int run_c2ny(const char *input_path, const char *output_path) {
  size_t n = 0;
  char *src = ny_read_file_raw(input_path, &n);
  if (!src) {
    nyt_err("ny-fmt", "c2ny: failed to read %s", input_path);
    return 1;
  }

  cny_sb_t out = {0};
  cny_sb_add(&out, ";; Generated by ny-fmt --c2ny from ");
  cny_sb_add(&out, input_path);
  cny_sb_add(&out, "\nuse core\n\n");

  int rc = cny_convert(src, n, &out);

  free(src);

  if (rc != 0) {
    nyt_err("ny-fmt",
            "c2ny: unsupported constructs remain in %s; see markers in output",
            input_path);
  }

  if (ny_write_file(output_path, out.data, out.len) != 0) {
    nyt_err("ny-fmt", "c2ny: failed to write %s", output_path);
    free(out.data);
    return 1;
  }

  printf("c2ny: %s -> %s (%zu bytes)\n", input_path, output_path, out.len);
  free(out.data);
  return rc;
}

static int run_conv(const FmtOpts *o) {
  if (!o->conv_input || !o->conv_name) {
    nyt_err("ny-fmt", "--conv requires --input and --name");
    return 2;
  }
  size_t n = 0;
  char *src = ny_read_file_raw(o->conv_input, &n);
  if (!src) {
    nyt_err("ny-fmt", "conv: failed to read %s", o->conv_input);
    return 1;
  }
  const char *fmt = o->conv_format ? o->conv_format : "man";
  char *out = convert_texi_basic(src, o->conv_name, fmt,
                                 o->conv_section ? o->conv_section : "1");
  free(src);
  if (!out) {
    nyt_err("ny-fmt", "conv: conversion failed");
    return 1;
  }
  int rc = 0;
  if (o->conv_output) {
    if (ny_write_file(o->conv_output, out, strlen(out)) != 0) {
      nyt_err("ny-fmt", "conv: failed to write %s", o->conv_output);
      rc = 1;
    }
  } else {
    fputs(out, stdout);
  }
  free(out);
  return rc;
}

typedef struct {
  const char *arg;
  const char *mode;
} FmtAuditAlias;

static const FmtAuditAlias k_fmt_audit_aliases[] = {
    {"audit", "all"},
    {"all", "all"},
    {"bloat", "bloat"},
    {"modules", "modules"},
    {"profiles", "profiles"},
    {"batteries", "batteries"},
    {"bugs", "bugs"},
    {"bug", "bugs"},
    {"correctness", "bugs"},
    {"lint", "bugs"},
    {"checks", "bugs"},
    {"bugchecks", "bugs"},
    {"sanity", "bugs"},
    {"trim", "trim"},
    {"layouts", "layouts"},
    {"layout", "layouts"},
    {"contracts", "contracts"},
    {"backend-contracts", "contracts"},
    {"specialize", "specialize"},
    {"specialization", "specialize"},
    {"constfold", "specialize"},
    {"partial", "specialize"},
    {"metaprog", "metaprog"},
    {"meta", "metaprog"},
    {"roadmap", "metaprog"},
    {"codebase", "metaprog"},
    {"features", "metaprog"},
    {"ffi", "ffi"},
    {"dead", "dead"},
    {"calls", "calls"},
    {"similarities", "calls"},
    {"types", "types"},
    {"legacy", "legacy"},
    {"methods", "methods"},
    {"method-syntax", "methods"},
    {"syntax", "methods"},
    {"smart", "smart"},
    {"overhaul", "smart"},
    {"constants", "constants"},
    {"consts", "constants"},
};

static const char *fmt_audit_mode_for_arg(const char *arg) {
  if (!arg || !*arg)
    return NULL;
  if (arg[0] == '-' && arg[1] == '-')
    arg += 2;
  for (size_t i = 0;
       i < sizeof(k_fmt_audit_aliases) / sizeof(k_fmt_audit_aliases[0]); i++) {
    if (strcmp(arg, k_fmt_audit_aliases[i].arg) == 0)
      return k_fmt_audit_aliases[i].mode;
  }
  return NULL;
}

static void fmt_audit_mode_set(FmtOpts *o, const char *mode) {
  if (!o)
    return;
  snprintf(o->audit_mode_buf, sizeof(o->audit_mode_buf), "%s",
           (mode && *mode) ? mode : "all");
  o->audit_mode = o->audit_mode_buf;
}

static void fmt_audit_mode_add(FmtOpts *o, const char *mode) {
  if (!o || !mode || !*mode)
    return;
  if (strcmp(mode, "all") == 0) {
    fmt_audit_mode_set(o, "all");
    return;
  }
  if (strcmp(o->audit_mode_buf, "all") == 0)
    o->audit_mode_buf[0] = '\0';
  if (token_list_contains(o->audit_mode_buf, mode)) {
    o->audit_mode = o->audit_mode_buf;
    return;
  }
  size_t used = strlen(o->audit_mode_buf);
  if (used > 0 && used + 1 < sizeof(o->audit_mode_buf)) {
    o->audit_mode_buf[used++] = '|';
    o->audit_mode_buf[used] = '\0';
  }
  if (used < sizeof(o->audit_mode_buf) - 1) {
    strncat(o->audit_mode_buf, mode, sizeof(o->audit_mode_buf) - used - 1);
  }
  o->audit_mode = o->audit_mode_buf[0] ? o->audit_mode_buf : "all";
}

static int parse_args_c(int argc, char **argv, FmtOpts *o) {
  memset(o, 0, sizeof(*o));
  o->c2ny_output = "out.ny";
  o->min_sev = "LOW";
  o->conv_format = "man";
  o->conv_section = "1";
  o->color = -2;
  o->limit = 80;
  o->cloc_top = 20;
  o->dupes_min = 30;
  o->audit_mode = "all";
  char err[256];
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage();
      return 1;
    }
    int color_mode = -2;
    int color_idx = i;
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode,
                                        err, sizeof(err));
    if (color_rc < 0) {
      nyt_err("ny-fmt", "%s", err);
      return 0;
    }
    if (color_rc > 0) {
      o->color = color_mode;
      i = color_idx;
      continue;
    }

    const char *audit_mode = fmt_audit_mode_for_arg(a);
    if (audit_mode) {
      o->audit = 1;
      fmt_audit_mode_add(o, audit_mode);
    } else if (strcmp(a, "--selftest") == 0) {
      o->selftest = 1;
    } else if (strcmp(a, "--analyze") == 0) {
      o->analyze = 1;
    } else if (strcmp(a, "--migrate-operators") == 0) {
      o->route = FMT_ROUTE_MIGRATE_OPERATORS;
    } else if (strcmp(a, "--assume-legacy-operators") == 0) {
      o->legacy_operators = 1;
    } else if (strcmp(a, "--cloc") == 0 || strcmp(a, "cloc") == 0) {
      o->cloc = 1;
    } else if (strcmp(a, "--dupes") == 0 || strcmp(a, "--duplicates") == 0 ||
               strcmp(a, "dupes") == 0 || strcmp(a, "duplicates") == 0) {
      o->dupes = 1;
    } else if (strcmp(a, "--dupes-emit") == 0 || strcmp(a, "dupes-emit") == 0) {
      o->dupes = 1;
      o->dupes_emit = 1;
    } else if (strcmp(a, "--dupes-min") == 0 && i + 1 < argc) {
      o->dupes = 1;
      ny_parse_int(argv[++i], &o->dupes_min);
    } else if (strncmp(a, "--dupes-min=", 12) == 0) {
      o->dupes = 1;
      ny_parse_int(a + 12, &o->dupes_min);
    } else if (strcmp(a, "--full") == 0 || strcmp(a, "-f") == 0) {
      o->cloc_full = 1;
    } else if (strcmp(a, "--top") == 0 && i + 1 < argc) {
      ny_parse_int(argv[++i], &o->cloc_top);
    } else if (strncmp(a, "--top=", 6) == 0) {
      ny_parse_int(a + 6, &o->cloc_top);
    } else if (strcmp(a, "--audit-mode") == 0 && i + 1 < argc) {
      o->audit = 1;
      fmt_audit_mode_set(o, argv[++i]);
    } else if (strncmp(a, "--audit-mode=", 13) == 0) {
      o->audit = 1;
      fmt_audit_mode_set(o, a + 13);
    } else if (strcmp(a, "--check") == 0) {
      o->check = 1;
    } else if (strcmp(a, "--fix") == 0) {
      o->fix = 1;
    } else if (strcmp(a, "--json") == 0) {
      o->json = 1;
    } else if (strcmp(a, "--types-strict") == 0) {
      o->audit = 1;
      fmt_audit_mode_add(o, "types");
      o->types_strict = 1;
    } else if (strcmp(a, "--limit") == 0 && i + 1 < argc) {
      ny_parse_int(argv[++i], &o->limit);
    } else if (strncmp(a, "--limit=", 8) == 0) {
      ny_parse_int(a + 8, &o->limit);
    } else if (strcmp(a, "--threshold") == 0 && i + 1 < argc) {
      i++;
    } else if (strncmp(a, "--threshold=", 12) == 0) {

    } else if (strcmp(a, "--root") == 0 && i + 1 < argc) {
      i++;
    } else if (strncmp(a, "--root=", 7) == 0) {

    } else if (strcmp(a, "--dirs") == 0 && i + 1 < argc) {
      sv_push(&o->paths, argv[++i]);
    } else if (strncmp(a, "--dirs=", 7) == 0) {
      sv_push(&o->paths, a + 7);
    } else if (strcmp(a, "--tidy") == 0) {
      o->tidy = 1;
    } else if (strcmp(a, "--optimize") == 0) {
      o->optimize = 1;
    } else if (strcmp(a, "--apply") == 0) {
      o->apply = 1;
    } else if (strcmp(a, "--diff") == 0) {
      o->diff = 1;
    } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
      o->verbose = 1;
    } else if (strcmp(a, "--align") == 0 || strcmp(a, "--align-macros") == 0) {
      o->align_macros = 1;
    } else if (strcmp(a, "--c2ny") == 0) {
      o->c2ny = 1;
    } else if (strcmp(a, "--py2ny") == 0) {
      o->py2ny = 1;
    } else if (strcmp(a, "--conv") == 0) {
      o->conv = 1;
    } else if (strcmp(a, "--input") == 0 && i + 1 < argc) {
      o->conv_input = argv[++i];
    } else if (strcmp(a, "--name") == 0 && i + 1 < argc) {
      o->conv_name = argv[++i];
    } else if (strcmp(a, "--format") == 0 && i + 1 < argc) {
      o->conv_format = argv[++i];
    } else if (strcmp(a, "--section") == 0 && i + 1 < argc) {
      o->conv_section = argv[++i];
    } else if ((strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) &&
               i + 1 < argc) {
      o->conv_output = argv[++i];
    } else if (strcmp(a, "--min-sev") == 0 && i + 1 < argc) {
      o->min_sev = argv[++i];
    } else if (strncmp(a, "--min-sev=", 10) == 0) {
      o->min_sev = a + 10;
    } else if (a[0] == '-') {
      nyt_err("ny-fmt", "unknown option: %s", a);
      return 0;
    } else {
      sv_push(&o->paths, a);
    }
  }
  if (o->legacy_operators && o->route != FMT_ROUTE_MIGRATE_OPERATORS) {
    nyt_err("ny-fmt", "--assume-legacy-operators requires --migrate-operators");
    return 0;
  }
  return 2;
}

static int parse_args(int argc, char **argv, FmtOpts *opts) {
  int state = parse_args_c(argc, argv, opts);
  if (state == 2) {
    if (opts->route == FMT_ROUTE_NORMAL) {
      if (opts->selftest)
        opts->route = FMT_ROUTE_SELFTEST;
      else if (opts->c2ny)
        opts->route = FMT_ROUTE_C2NY;
      else if (opts->py2ny)
        opts->route = FMT_ROUTE_PY2NY;
      else if (opts->align_macros)
        opts->route = FMT_ROUTE_ALIGN;
      else if (opts->conv)
        opts->route = FMT_ROUTE_CONV;
      else if (opts->cloc)
        opts->route = FMT_ROUTE_CLOC;
      else if (opts->dupes)
        opts->route = FMT_ROUTE_DUPES;
    }
    if (opts->fix || opts->tidy)
      opts->plan |= FMT_PLAN_REWRITE;
    if (opts->check || opts->tidy)
      opts->plan |= FMT_PLAN_CHECK;
    if (opts->analyze || opts->optimize || opts->tidy)
      opts->plan |= FMT_PLAN_ANALYZE;
    if (opts->audit)
      opts->plan |= FMT_PLAN_AUDIT;
    if (opts->optimize && (opts->apply || opts->diff))
      opts->plan |= FMT_PLAN_OPTIMIZE;
    if (opts->plan == 0 && opts->route == FMT_ROUTE_NORMAL && !opts->dupes)
      opts->plan |= FMT_PLAN_READ_ONLY_NOTICE;
  }
  return state;
}

#ifdef NYTRIX_EMBEDDED_TOOLS
static int fmt_source_append(char **buf, size_t *len, size_t *cap,
                             const char *text, size_t text_len) {
  if (!buf || !len || !cap || (!text && text_len))
    return 0;
  if (text_len > SIZE_MAX - *len - 1)
    return 0;
  size_t need = *len + text_len + 1;
  if (need > *cap) {
    size_t next = *cap ? *cap : 512;
    while (next < need) {
      if (next > SIZE_MAX / 2) {
        next = need;
        break;
      }
      next *= 2;
    }
    char *grown = (char *)realloc(*buf, next);
    if (!grown)
      return 0;
    *buf = grown;
    *cap = next;
  }
  if (text_len)
    memcpy(*buf + *len, text, text_len);
  *len += text_len;
  (*buf)[*len] = '\0';
  return 1;
}

static int fmt_source_append_str(char **buf, size_t *len, size_t *cap,
                                 const char *value) {
  return fmt_source_append(buf, len, cap, value ? value : "",
                           value ? strlen(value) : 0);
}

static int fmt_source_append_quoted(char **buf, size_t *len, size_t *cap,
                                    const char *value) {
  if (!fmt_source_append_str(buf, len, cap, "\""))
    return 0;
  for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p;
       ++p) {
    char escaped[5];
    const char *text = (const char *)p;
    size_t text_len = 1;
    if (*p == '"' || *p == '\\') {
      escaped[0] = '\\';
      escaped[1] = (char)*p;
      text = escaped;
      text_len = 2;
    } else if (*p == '\n' || *p == '\r' || *p == '\t') {
      escaped[0] = '\\';
      escaped[1] = *p == '\n' ? 'n' : *p == '\r' ? 'r' : 't';
      text = escaped;
      text_len = 2;
    } else if (*p < 32 || *p == 127) {
      int n = snprintf(escaped, sizeof(escaped), "\\x%02x", *p);
      if (n != 4)
        return 0;
      text = escaped;
      text_len = 4;
    }
    if (!fmt_source_append(buf, len, cap, text, text_len))
      return 0;
  }
  return fmt_source_append_str(buf, len, cap, "\"");
}
#endif

static int fmt_collect_wanted(const char *path, const char *mode) {
  if (nyt_ends_with(path, ".ny"))
    return strcmp(mode, "ny") == 0 || strcmp(mode, "fmt") == 0 ||
           strcmp(mode, "code") == 0 || strcmp(mode, "cloc") == 0;
  if (nyt_ends_with(path, ".nshape"))
    return strcmp(mode, "nshape") == 0;
  if (nyt_ends_with(path, ".nyt"))
    return strcmp(mode, "cloc") == 0;
  if (nyt_ends_with(path, ".c") || nyt_ends_with(path, ".h"))
    return strcmp(mode, "fmt") == 0 || strcmp(mode, "code") == 0 ||
           strcmp(mode, "c") == 0 || strcmp(mode, "cloc") == 0;
  if (nyt_ends_with(path, ".cc") || nyt_ends_with(path, ".cpp") ||
      nyt_ends_with(path, ".cxx") || nyt_ends_with(path, ".hh") ||
      nyt_ends_with(path, ".hpp") || nyt_ends_with(path, ".hxx") ||
      nyt_ends_with(path, ".py") || nyt_ends_with(path, ".md"))
    return strcmp(mode, "fmt") == 0;
  if (nyt_ends_with(path, ".py"))
    return strcmp(mode, "code") == 0;
  return 0;
}

static int fmt_collect_skip_dir(const char *name, const char *mode) {
  if (!name || !*name || name[0] == '.')
    return 1;
  static const char *const cloc_skip[] = {
      "build", "node_modules", "__pycache__", ".venv", "dist", "out", ".cache"};
  static const char *const code_skip[] = {"tmp", "build", "node_modules"};
  if (strcmp(mode, "cloc") == 0) {
    for (size_t i = 0; i < sizeof(cloc_skip) / sizeof(cloc_skip[0]); ++i)
      if (strcmp(name, cloc_skip[i]) == 0)
        return 1;
  } else if (strcmp(mode, "code") == 0 || strcmp(mode, "c") == 0) {
    for (size_t i = 0; i < sizeof(code_skip) / sizeof(code_skip[0]); ++i)
      if (strcmp(name, code_skip[i]) == 0)
        return 1;
  } else if (strcmp(name, "tmp") == 0) {
    return 1;
  }
  return 0;
}

static int fmt_collect_tree(const char *path, const char *mode, int depth,
                            size_t *visited, StrVec *out) {
  if (!path || !*path || !mode || !visited || !out || depth > 256)
    return 0;
  DIR *dir = opendir(path);
  if (!dir) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      return 0;
    if (fmt_collect_wanted(path, mode)) {
      size_t old_len = out->len;
      sv_push(out, path);
      return out->len == old_len + 1;
    }
    return 1;
  }
  int ok = 1;
  size_t path_len = strlen(path);
  size_t separator_len =
      path_len && path[path_len - 1] != '/' && path[path_len - 1] != '\\' ? 1
                                                                          : 0;
  struct dirent *entry = NULL;
  while (ok) {
    /*
     * Keep readdir's error state separate from stat/open failures deeper in
     * the walk.  On filesystems that provide d_type, the common directory and
     * regular-file cases below also avoid one metadata syscall per entry.
     */
    errno = 0;
    entry = readdir(dir);
    if (!entry) {
      if (errno != 0)
        ok = 0;
      break;
    }
    if (++(*visited) > 100000) {
      ok = 0;
      break;
    }
    const char *name = entry->d_name;
    if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        fmt_collect_skip_dir(name, mode))
      continue;
    char child[PATH_MAX];
    size_t name_len = strlen(name);
    if (path_len >= sizeof(child) ||
        separator_len > sizeof(child) - 1 - path_len ||
        name_len > sizeof(child) - 1 - path_len - separator_len) {
      ok = 0;
      break;
    }
    memcpy(child, path, path_len);
    if (separator_len)
      child[path_len] = '/';
    memcpy(child + path_len + separator_len, name, name_len + 1);

#if defined(__linux__) && defined(_DIRENT_HAVE_D_TYPE) && defined(DT_DIR) &&   \
    defined(DT_REG) && defined(DT_UNKNOWN) && defined(DT_LNK)
    /*
     * d_type is only a hint on some filesystems.  Preserve the old stat
     * behavior for symlinks and unknown entries; those can point at files or
     * directories that should still be followed.
     */
    if (entry->d_type == DT_DIR) {
      ok = fmt_collect_tree(child, mode, depth + 1, visited, out);
      continue;
    }
    if (entry->d_type == DT_REG) {
      if (fmt_collect_wanted(child, mode)) {
        size_t old_len = out->len;
        sv_push(out, child);
        ok = out->len == old_len + 1;
      }
      continue;
    }
    if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_LNK)
      continue;
#endif
    struct stat st;
    if (stat(child, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      ok = fmt_collect_tree(child, mode, depth + 1, visited, out);
    } else if (S_ISREG(st.st_mode) && fmt_collect_wanted(child, mode)) {
      size_t old_len = out->len;
      sv_push(out, child);
      ok = out->len == old_len + 1;
    }
  }
  closedir(dir);
  return ok;
}

static int fmt_collect_via_library(const char *const *roots, size_t root_count,
                                   const char *mode, StrVec *out) {
  if (!roots || root_count == 0 || !mode || !out ||
      (strcmp(mode, "ny") != 0 && strcmp(mode, "nshape") != 0 &&
       strcmp(mode, "fmt") != 0 && strcmp(mode, "code") != 0 &&
       strcmp(mode, "c") != 0 && strcmp(mode, "cloc") != 0))
    return 0;
  size_t before = out->len;
  size_t visited = 0;
  for (size_t i = 0; i < root_count; ++i) {
    if (!roots[i] || !*roots[i] || ++visited > 100000 ||
        !fmt_collect_tree(roots[i], mode, 0, &visited, out)) {
      while (out->len > before)
        free(out->items[--out->len]);
      return 0;
    }
  }
  sv_sort(out);
  sv_dedup_sorted(out);
  return 1;
}

#ifdef NYTRIX_EMBEDDED_TOOLS
static int fmt_payload_path(char *path, size_t cap) {
  const char *tmp = ny_get_temp_dir();
  if (!tmp || !*tmp || !path || cap == 0)
    return 0;
#ifdef _WIN32
  for (int attempt = 0; attempt < 64; ++attempt) {
    int n = snprintf(path, cap, "%s\\ny-fmt-%u-%d.tmp", tmp,
                     (unsigned)GetCurrentProcessId(), attempt);
    if (n < 0 || (size_t)n >= cap)
      return 0;
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      return 1;
    }
  }
  return 0;
#else
  int n = snprintf(path, cap, "%s/ny-fmt-XXXXXX", tmp);
  if (n < 0 || (size_t)n >= cap)
    return 0;
  int fd = mkstemp(path);
  if (fd < 0)
    return 0;
  close(fd);
  return 1;
#endif
}

static void fmt_remove_payload(const char *path) {
  if (!path || !*path)
    return;
#ifdef _WIN32
  DeleteFileA(path);
#else
  unlink(path);
#endif
}
#endif

static int fmt_migrate_operator_files(const StrVec *files, int assume_legacy,
                                      size_t *changed_files) {
  if (!files || !changed_files)
    return 0;
  *changed_files = 0;
  if (!assume_legacy || files->len == 0)
    return 1;
  for (size_t i = 0; i < files->len; ++i) {
    const char *path = files->items[i];
    size_t input_len = 0;
    char *input = ny_read_file_raw(path, &input_len);
    if (!input) {
      nyt_err("ny-fmt", "could not read source for operator migration: %s",
              path);
      return 0;
    }
    /*
     * This C implementation runs without compiling or invoking Ny code.
     */
    if (input_len > (SIZE_MAX - 1) / 2) {
      free(input);
      nyt_err("ny-fmt", "source is too large for operator migration: %s", path);
      return 0;
    }
    char *output = (char *)malloc(input_len * 2 + 1);
    if (!output) {
      free(input);
      return 0;
    }
    size_t out_len = 0;
    int in_nshape = ny_has_suffix(path, ".nshape");
    int in_ny_block = !in_nshape;
    int skip_ny_block = !in_nshape && strstr(input, "**") != NULL;
    char marker[128] = {0};
    size_t pos = 0;
    int quote = 0, quote_triple = 0, escape = 0, block_comment = 0;
    while (pos < input_len) {
      size_t line_end = pos;
      while (line_end < input_len && input[line_end] != '\n')
        ++line_end;
      size_t line_len = line_end - pos;
      if (in_nshape && !in_ny_block) {
        const char *start = strstr(input + pos, "source ny <<'");
        if (start == input + pos ||
            (start && (size_t)(start - (input + pos)) < line_len)) {
          const char *q = start + strlen("source ny <<'");
          const char *endq = memchr(q, '\'', (size_t)((input + line_len) - q));
          size_t mlen = endq ? (size_t)(endq - q) : 0;
          if (mlen > 0 && mlen < sizeof(marker)) {
            memcpy(marker, q, mlen);
            marker[mlen] = '\0';
            in_ny_block = 1;
            size_t scan = line_end + (line_end < input_len);
            while (scan < input_len) {
              size_t scan_end = scan;
              while (scan_end < input_len && input[scan_end] != '\n')
                ++scan_end;
              size_t scan_len = scan_end - scan;
              if (scan_len == mlen && memcmp(input + scan, marker, mlen) == 0)
                break;
              for (size_t k = 0; k + 1 < scan_len; ++k)
                if (input[scan + k] == '*' && input[scan + k + 1] == '*') {
                  skip_ny_block = 1;
                  break;
                }
              scan = scan_end + (scan_end < input_len);
            }
          }
        }
        memcpy(output + out_len, input + pos,
               line_end - pos + (line_end < input_len));
        out_len += line_end - pos + (line_end < input_len);
        pos = line_end + (line_end < input_len);
        continue;
      }
      if (in_nshape && in_ny_block && line_len == strlen(marker) &&
          memcmp(input + pos, marker, line_len) == 0) {
        in_ny_block = 0;
        skip_ny_block = 0;
        memcpy(output + out_len, input + pos,
               line_end - pos + (line_end < input_len));
        out_len += line_end - pos + (line_end < input_len);
        pos = line_end + (line_end < input_len);
        continue;
      }
      if (in_ny_block && !skip_ny_block) {
        /*
         * Scan one source line at a time so nshape metadata is never treated as
         * Ny code. Strings and comments are copied byte-for-byte.
         */
        size_t j = pos;
        int line_comment = 0;
        while (j < line_end) {
          char c = input[j];
          if (line_comment) {
            output[out_len++] = c;
            ++j;
            continue;
          }
          if (block_comment) {
            output[out_len++] = c;
            if (c == '*' && j + 1 < line_end && input[j + 1] == '/') {
              output[out_len++] = input[j + 1];
              j += 2;
              block_comment = 0;
            } else
              ++j;
            continue;
          }
          if (quote) {
            if (!escape && quote_triple && c == quote && j + 2 < line_end &&
                input[j + 1] == quote && input[j + 2] == quote) {
              output[out_len++] = c;
              output[out_len++] = c;
              output[out_len++] = c;
              j += 3;
              quote = quote_triple = 0;
              continue;
            }
            output[out_len++] = c;
            ++j;
            if (escape)
              escape = 0;
            else if (c == '\\')
              escape = 1;
            else if (!quote_triple && c == quote)
              quote = 0;
            continue;
          }
          if (c == '/' && j + 1 < line_end && input[j + 1] == '*') {
            output[out_len++] = c;
            output[out_len++] = input[j + 1];
            j += 2;
            block_comment = 1;
            continue;
          }
          if ((c == '"' || c == '\'') && j + 2 < line_end &&
              input[j + 1] == c && input[j + 2] == c) {
            quote = c;
            quote_triple = 1;
            output[out_len++] = c;
            output[out_len++] = c;
            output[out_len++] = c;
            j += 3;
            continue;
          }
          if (c == '"' || c == '\'') {
            quote = c;
            output[out_len++] = c;
            ++j;
            continue;
          }
          if (c == ';' && j + 1 < line_end && input[j + 1] == ';')
            line_comment = 1;
          if (c == '^' && j + 1 < line_end && input[j + 1] == '^') {
            int assign = j + 2 < line_end && input[j + 2] == '=';
            output[out_len++] = '^';
            if (assign)
              output[out_len++] = '=';
            j += assign ? 3 : 2;
            continue;
          }
          if (c == '^') {
            int assign = j + 1 < line_end && input[j + 1] == '=';
            output[out_len++] = '*';
            output[out_len++] = '*';
            if (assign) {
              output[out_len++] = '=';
              ++j;
            }
            ++j;
            continue;
          }
          output[out_len++] = c;
          ++j;
        }
      } else {
        memcpy(output + out_len, input + pos, line_end - pos);
        out_len += line_end - pos;
      }
      if (line_end < input_len)
        output[out_len++] = '\n';
      pos = line_end + (line_end < input_len);
    }
    output[out_len] = '\0';
    int ok = out_len == input_len ? memcmp(input, output, input_len) == 0 : 0;
    if (!ok && ny_write_file(path, output, out_len) != 0) {
      nyt_err("ny-fmt", "could not write migrated source: %s", path);
      free(output);
      free(input);
      return 0;
    }
    if (!ok)
      ++*changed_files;
    free(output);
    free(input);
  }
  return 1;
}

#ifdef NYTRIX_EMBEDDED_TOOLS
static int fmt_append_hex(char **buf, size_t *len, size_t *cap,
                          const unsigned char *bytes, size_t n) {
  static const char digits[] = "0123456789abcdef";
  if (n > (SIZE_MAX - 1) / 2)
    return 0;
  for (size_t i = 0; i < n; ++i) {
    char pair[2] = {digits[bytes[i] >> 4], digits[bytes[i] & 15]};
    if (!fmt_source_append(buf, len, cap, pair, sizeof(pair)))
      return 0;
  }
  return 1;
}

static char *fmt_unhex(const char *hex) {
  if (!hex)
    return NULL;
  size_t n = strlen(hex);
  if (n & 1)
    return NULL;
  char *out = (char *)malloc(n / 2 + 1);
  if (!out)
    return NULL;
  for (size_t i = 0; i < n; i += 2) {
    int hi = hex[i] >= '0' && hex[i] <= '9'   ? hex[i] - '0'
             : hex[i] >= 'a' && hex[i] <= 'f' ? hex[i] - 'a' + 10
             : hex[i] >= 'A' && hex[i] <= 'F' ? hex[i] - 'A' + 10
                                              : -1;
    int lo = hex[i + 1] >= '0' && hex[i + 1] <= '9'   ? hex[i + 1] - '0'
             : hex[i + 1] >= 'a' && hex[i + 1] <= 'f' ? hex[i + 1] - 'a' + 10
             : hex[i + 1] >= 'A' && hex[i + 1] <= 'F' ? hex[i + 1] - 'A' + 10
                                                      : -1;
    if (hi < 0 || lo < 0) {
      free(out);
      return NULL;
    }
    out[i / 2] = (char)((hi << 4) | lo);
  }
  out[n / 2] = '\0';
  return out;
}
#endif

typedef struct {
  const StrVec *files;
  const size_t *selected_files;
  char **original;
  char **formatted;
  size_t count;
  atomic_size_t next_file;
  atomic_size_t total_bytes;
  atomic_bool failed;
} FmtBatchWork;

static void *fmt_source_batch_worker(void *arg) {
  FmtBatchWork *batch = (FmtBatchWork *)arg;
  while (!atomic_load_explicit(&batch->failed, memory_order_relaxed)) {
    size_t index =
        atomic_fetch_add_explicit(&batch->next_file, 1, memory_order_relaxed);
    if (index >= batch->count)
      break;
    const char *path = batch->files->items[batch->selected_files[index]];
    size_t n = 0;
    char *source = ny_read_file_raw(path, &n);
    size_t prior =
        atomic_fetch_add_explicit(&batch->total_bytes, n, memory_order_relaxed);
    if (!source || memchr(source, '\0', n) || n > 32u * 1024u * 1024u ||
        prior > 128u * 1024u * 1024u - n) {
      free(source);
      atomic_store_explicit(&batch->failed, 1, memory_order_relaxed);
      break;
    }
    batch->original[index] = source;
    /*
     * Most repository files already use LF. Avoid a full-size duplicate
     * allocation and copy on that common path; normalize only CR-bearing
     * inputs such as CRLF files.
     */
    const char *normalized = source;
    char *normalized_owned = NULL;
    if (memchr(source, '\r', n)) {
      normalize_line_endings(source, &normalized_owned);
      if (!normalized_owned) {
        atomic_store_explicit(&batch->failed, 1, memory_order_relaxed);
        break;
      }
      normalized = normalized_owned;
    }
    if (is_ny(path))
      batch->formatted[index] = format_ny_text(normalized);
    else if (is_c_source(path))
      batch->formatted[index] = format_c_text(normalized);
    else
      batch->formatted[index] = tidy_c_text(normalized);
    free(normalized_owned);
    if (!batch->formatted[index])
      atomic_store_explicit(&batch->failed, 1, memory_order_relaxed);
  }
  return NULL;
}

/*
 * Format the supported source batch in C. Keep input order aligned with the
 * caller's file list while avoiding an embedded compiler session and its
 * temporary-file serialization on every `ny fmt --fix` / `--optimize` run.
 */
static int fmt_source_batch_c(const StrVec *files, char ***formatted_out,
                              char ***original_out, size_t *count_out) {
  if (formatted_out)
    *formatted_out = NULL;
  if (original_out)
    *original_out = NULL;
  if (count_out)
    *count_out = 0;
  if (!files || !formatted_out || !original_out || !count_out)
    return 0;

  size_t count = 0;
  for (size_t i = 0; i < files->len; ++i) {
    if (is_ny(files->items[i]) || is_c_source(files->items[i]) ||
        is_c_tidy_source(files->items[i]))
      ++count;
  }
  char **formatted = (char **)calloc(count ? count : 1, sizeof(char *));
  char **original = (char **)calloc(count ? count : 1, sizeof(char *));
  size_t *selected_files =
      (size_t *)malloc((count ? count : 1) * sizeof(*selected_files));
  if (!formatted || !original || !selected_files) {
    free(formatted);
    free(original);
    free(selected_files);
    return 0;
  }

  size_t selected_count = 0;
  for (size_t i = 0; i < files->len; ++i) {
    const char *path = files->items[i];
    if (is_ny(path) || is_c_source(path) || is_c_tidy_source(path))
      selected_files[selected_count++] = i;
  }

  FmtBatchWork work = {.files = files,
                       .selected_files = selected_files,
                       .original = original,
                       .formatted = formatted,
                       .count = selected_count};
  atomic_init(&work.next_file, 0);
  atomic_init(&work.total_bytes, 0);
  atomic_init(&work.failed, 0);

  long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
  if (online_cpus < 1)
    online_cpus = 1;
  if (online_cpus > 8)
    online_cpus = 8;
  /*
   * Thread startup costs more than formatting a handful of files. Keep the
   * common small invocation on the caller thread and fan out larger batches.
   */
  size_t worker_count =
      selected_count < 4
          ? 1
          : (selected_count < 8 ? selected_count : (size_t)online_cpus);
  if (worker_count < 1)
    worker_count = 1;
  if (worker_count == 1) {
    fmt_source_batch_worker(&work);
  } else {
    pthread_t workers[8];
    size_t started = 0;
    for (; started < worker_count; ++started) {
      if (pthread_create(&workers[started], NULL, fmt_source_batch_worker,
                         &work) != 0)
        break;
    }
    if (started < worker_count)
      fmt_source_batch_worker(&work);
    for (size_t i = 0; i < started; ++i)
      pthread_join(workers[i], NULL);
  }
  free(selected_files);
  if (atomic_load_explicit(&work.failed, memory_order_relaxed))
    goto fail;
  *formatted_out = formatted;
  *original_out = original;
  *count_out = count;
  return 1;

fail:
  for (size_t i = 0; i < count; ++i)
    free(formatted[i]);
  for (size_t i = 0; i < count; ++i)
    free(original[i]);
  free(original);
  free(formatted);
  return 0;
}

#ifdef NYTRIX_EMBEDDED_TOOLS
static int fmt_parse_nonnegative(const char *text, int *value) {
  if (!text || !*text || !value)
    return 0;
  char *end = NULL;
  long parsed = strtol(text, &end, 10);
  if (!end || *end || parsed < 0 || parsed > INT_MAX)
    return 0;
  *value = (int)parsed;
  return 1;
}

/*
 * Batched Ny-owned hygiene checks run in one session; structural analysis
 * and language-specific metadata remain with their existing scanners.
 */
static int fmt_analyze_hygiene_embedded(const StrVec *files, IssueVec *issues,
                                        AnalyzeStats *stats) {
  if (!files || !issues || !stats || files->len > 10000)
    return 0;
  size_t total = 0;
  for (size_t i = 0; i < files->len; ++i) {
    size_t n = 0;
    char *text = ny_read_file_raw(files->items[i], &n);
    if (!text || memchr(text, '\0', n) || n > 32u * 1024u * 1024u ||
        total > 128u * 1024u * 1024u - n) {
      free(text);
      return 0;
    }
    total += n;
    free(text);
  }
  char input_path[PATH_MAX] = {0}, output_path[PATH_MAX] = {0};
  if (!fmt_payload_path(input_path, sizeof(input_path)) ||
      !fmt_payload_path(output_path, sizeof(output_path))) {
    fmt_remove_payload(input_path);
    fmt_remove_payload(output_path);
    return 0;
  }
  char *payload = NULL;
  size_t payload_len = 0, payload_cap = 0;
  int ok = 1;
  for (size_t i = 0; ok && i < files->len; ++i) {
    size_t n = 0;
    char *text = ny_read_file_raw(files->items[i], &n);
    ok = text && !memchr(text, '\0', n) &&
         fmt_append_hex(&payload, &payload_len, &payload_cap,
                        (const unsigned char *)files->items[i],
                        strlen(files->items[i])) &&
         fmt_source_append_str(&payload, &payload_len, &payload_cap, "\t") &&
         fmt_append_hex(&payload, &payload_len, &payload_cap,
                        (const unsigned char *)text, n) &&
         fmt_source_append_str(&payload, &payload_len, &payload_cap, "\n");
    free(text);
    if (payload_len > 256u * 1024u * 1024u)
      ok = 0;
  }
  if (ok)
    ok = ny_write_file(input_path, payload ? payload : "", payload_len) == 0;
  free(payload);

  static const char source_prefix[] =
      "use core\n"
      "use core.compiler.tools.fmt.analyze (scan_hygiene_batch)\n"
      "use core.os (file_read, file_write, exit)\n"
      "def input_path = ";
  char *source = NULL;
  size_t source_len = 0, source_cap = 0;
  ok =
      ok &&
      fmt_source_append(&source, &source_len, &source_cap, source_prefix,
                        sizeof(source_prefix) - 1) &&
      fmt_source_append_quoted(&source, &source_len, &source_cap, input_path) &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            "\ndef output_path = ") &&
      fmt_source_append_quoted(&source, &source_len, &source_cap,
                               output_path) &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            "\ndef raw = match file_read(input_path) { ok(v) "
                            "-> v err(_) -> exit(2) }\n"
                            "def rendered = scan_hygiene_batch(raw)\n"
                            "match file_write(output_path, rendered) { ok(_) "
                            "-> 0 err(_) -> exit(2) }\n");
  if (ok)
    ok = nytrix_embed_run_source(source, NULL) == 0;
  free(source);
  fmt_remove_payload(input_path);
  size_t raw_len = 0;
  char *raw = ok ? ny_read_file_raw(output_path, &raw_len) : NULL;
  fmt_remove_payload(output_path);
  if (!raw || raw_len > 128u * 1024u * 1024u) {
    free(raw);
    return 0;
  }

  IssueVec added = {0};
  int trailing = 0, tabs = 0, todos = 0, long_lines = 0;
  char *cursor = raw;
  int first = 1;
  while (*cursor) {
    char *end = strchr(cursor, '\n');
    if (!end) {
      ok = 0;
      break;
    }
    *end = '\0';
    if (first) {
      char *fields[11] = {0};
      fields[0] = cursor;
      size_t field_count = 1;
      for (char *p = cursor; p < end; ++p) {
        if (*p == '\t') {
          *p = '\0';
          if (field_count >= 11) {
            ok = 0;
            break;
          }
          fields[field_count++] = p + 1;
        }
      }
      int unsafe_c = 0, functions = 0, public_functions = 0;
      int missing_doc = 0, duplicate_names = 0, long_functions = 0;
      ok = ok && field_count == 11 && strcmp(fields[0], "counts") == 0 &&
           fmt_parse_nonnegative(fields[1], &trailing) &&
           fmt_parse_nonnegative(fields[2], &tabs) &&
           fmt_parse_nonnegative(fields[3], &todos) &&
           fmt_parse_nonnegative(fields[4], &long_lines) &&
           fmt_parse_nonnegative(fields[5], &unsafe_c) &&
           fmt_parse_nonnegative(fields[6], &functions) &&
           fmt_parse_nonnegative(fields[7], &public_functions) &&
           fmt_parse_nonnegative(fields[8], &missing_doc) &&
           fmt_parse_nonnegative(fields[9], &duplicate_names) &&
           fmt_parse_nonnegative(fields[10], &long_functions);
      if (ok) {
        stats->unsafe_c += unsafe_c;
        stats->functions += functions;
        stats->ny_functions += functions;
        stats->public_functions += public_functions;
        stats->missing_doc += missing_doc;
        stats->duplicate_names += duplicate_names;
        stats->long_functions += long_functions;
      }
      first = 0;
    } else {
      char *fields[9] = {0};
      fields[0] = cursor;
      size_t field_count = 1;
      for (char *p = cursor; p < end; ++p) {
        if (*p == '\t') {
          *p = '\0';
          if (field_count >= 9) {
            ok = 0;
            break;
          }
          fields[field_count++] = p + 1;
        }
      }
      int file_order = 0, line = 0, col = 0;
      char *path = NULL, *message = NULL, *note = NULL;
      if (ok)
        ok = field_count == 9 && strcmp(fields[0], "issue") == 0 &&
             fmt_parse_nonnegative(fields[1], &file_order) &&
             (size_t)file_order < files->len &&
             fmt_parse_nonnegative(fields[3], &line) && line > 0 &&
             fmt_parse_nonnegative(fields[4], &col) && col > 0;
      if (ok) {
        path = fmt_unhex(fields[2]);
        message = fmt_unhex(fields[7]);
        note = fmt_unhex(fields[8]);
        ok = path && message && note && strlen(path) < PATH_MAX &&
             strlen(fields[5]) < sizeof(((Issue *)0)->code) &&
             strlen(fields[6]) < sizeof(((Issue *)0)->sev);
      }
      if (ok) {
        issue_push(&added, path, line, col, fields[5], fields[6], message,
                   note);
        if (added.len)
          added.items[added.len - 1].file_order =
              (size_t)file_order +
              (strcmp(fields[5], "NYFMT2100") == 0 ? files->len : 0);
      }
      free(path);
      free(message);
      free(note);
    }
    cursor = end + 1;
    if (!ok)
      break;
  }
  ok = ok && !first && *cursor == '\0';
  free(raw);
  if (!ok) {
    free(added.items);
    return 0;
  }
  stats->trailing_ws += trailing;
  stats->tabs += tabs;
  stats->todos += todos;
  stats->long_lines += long_lines;
  for (size_t i = 0; i < added.len; ++i) {
    const Issue *it = &added.items[i];
    issue_push(issues, it->file, it->line, it->col, it->code, it->sev, it->msg,
               it->note);
    if (issues->len)
      issues->items[issues->len - 1].file_order = it->file_order;
  }
  free(added.items);
  return 1;
}

static int fmt_collect_c_function_metadata(const StrVec *files, FnVec *fns) {
  if (!files || !fns)
    return 0;
  size_t total = 0;
  for (size_t i = 0; i < files->len; ++i) {
    const char *path = files->items[i];
    if (!nyt_ends_with(path, ".c") && !nyt_ends_with(path, ".h"))
      continue;
    size_t n = 0;
    char *src = ny_read_file_raw(path, &n);
    if (!src || n > 32u * 1024u * 1024u || total > 128u * 1024u * 1024u - n ||
        memchr(src, '\0', n)) {
      free(src);
      return 0;
    }
    total += n;
    CScanFunctions found = {0};
    int ok = cscan_functions(src, n, &found);
    free(src);
    if (!ok) {
      cscan_functions_free(&found);
      return 0;
    }
    for (size_t j = 0; j < found.len; ++j) {
      Fn fn;
      memset(&fn, 0, sizeof(fn));
      snprintf(fn.name, sizeof(fn.name), "%s", found.items[j].name);
      snprintf(fn.file, sizeof(fn.file), "%s", path);
      fn.line = found.items[j].start_line;
      fn.end_line = found.items[j].end_line;
      fn.is_private = found.items[j].is_static;
      fn.is_c = 1;
      fv_push(fns, &fn);
    }
    cscan_functions_free(&found);
  }
  return 1;
}

static int fmt_fn_metadata_order(const void *a, const void *b) {
  const Fn *left = (const Fn *)a;
  const Fn *right = (const Fn *)b;
  int by_file = strcmp(left->file, right->file);
  if (by_file)
    return by_file;
  if (left->line != right->line)
    return left->line < right->line ? -1 : 1;
  return strcmp(left->name, right->name);
}

static int fmt_collect_function_metadata_embedded(const StrVec *files,
                                                  FnVec *fns) {
  if (!files || !fns || files->len > 10000)
    return 0;
  size_t initial_len = fns->len;
  int has_ny = 0;
  if (!fmt_collect_c_function_metadata(files, fns)) {
    fns->len = initial_len;
    return 0;
  }
  for (size_t i = 0; i < files->len; ++i)
    has_ny |= nyt_ends_with(files->items[i], ".ny");
  if (!has_ny) {
    if (fns->len > initial_len + 1)
      qsort(fns->items + initial_len, fns->len - initial_len, sizeof(Fn),
            fmt_fn_metadata_order);
    return 1;
  }
  size_t total = 0;
  char *payload = NULL;
  size_t payload_len = 0, payload_cap = 0;
  int ok = 1;
  for (size_t i = 0; ok && i < files->len; ++i) {
    const char *path = files->items[i];
    if (nyt_ends_with(path, ".c") || nyt_ends_with(path, ".h")) {
      ok = fmt_append_hex(&payload, &payload_len, &payload_cap,
                          (const unsigned char *)path, strlen(path)) &&
           fmt_source_append_str(&payload, &payload_len, &payload_cap, "\t\n");
      continue;
    }
    int is_source = nyt_ends_with(path, ".ny") || nyt_ends_with(path, ".c") ||
                    nyt_ends_with(path, ".h");
    if (!is_source) {
      ok = fmt_append_hex(&payload, &payload_len, &payload_cap,
                          (const unsigned char *)path, strlen(path)) &&
           fmt_source_append_str(&payload, &payload_len, &payload_cap, "\t\n");
      continue;
    }
    size_t n = 0;
    char *text = ny_read_file_raw(path, &n);
    if (!text || memchr(text, '\0', n) || n > 32u * 1024u * 1024u ||
        total > 128u * 1024u * 1024u - n) {
      free(text);
      ok = 0;
      break;
    }
    total += n;
    ok = fmt_append_hex(&payload, &payload_len, &payload_cap,
                        (const unsigned char *)path, strlen(path)) &&
         fmt_source_append_str(&payload, &payload_len, &payload_cap, "\t") &&
         fmt_append_hex(&payload, &payload_len, &payload_cap,
                        (const unsigned char *)text, n) &&
         fmt_source_append_str(&payload, &payload_len, &payload_cap, "\n");
    free(text);
    if (payload_len > 256u * 1024u * 1024u)
      ok = 0;
  }

  char input_path[PATH_MAX] = {0}, output_path[PATH_MAX] = {0};
  if (!fmt_payload_path(input_path, sizeof(input_path)) ||
      !fmt_payload_path(output_path, sizeof(output_path)))
    ok = 0;
  if (ok)
    ok = ny_write_file(input_path, payload ? payload : "", payload_len) == 0;
  free(payload);

  static const char source_prefix[] =
      "use core\n"
      "use core.compiler.tools.fmt.analyze (scan_function_metadata_batch)\n"
      "use core.os (file_read, file_write, exit)\n"
      "def input_path = ";
  char *source = NULL;
  size_t source_len = 0, source_cap = 0;
  ok =
      ok &&
      fmt_source_append(&source, &source_len, &source_cap, source_prefix,
                        sizeof(source_prefix) - 1) &&
      fmt_source_append_quoted(&source, &source_len, &source_cap, input_path) &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            "\ndef output_path = ") &&
      fmt_source_append_quoted(&source, &source_len, &source_cap,
                               output_path) &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            "\ndef raw = match file_read(input_path) { ok(v) "
                            "-> v err(_) -> exit(2) }\n"
                            "def rendered = scan_function_metadata_batch(raw)\n"
                            "match file_write(output_path, rendered) { ok(_) "
                            "-> 0 err(_) -> exit(2) }\n");
  if (ok)
    ok = nytrix_embed_run_source(source, NULL) == 0;
  free(source);
  fmt_remove_payload(input_path);
  size_t raw_len = 0;
  char *raw = ok ? ny_read_file_raw(output_path, &raw_len) : NULL;
  fmt_remove_payload(output_path);
  if (!raw || raw_len > 128u * 1024u * 1024u) {
    free(raw);
    fns->len = initial_len;
    return 0;
  }

  char *cursor = raw;
  while (*cursor) {
    char *end = strchr(cursor, '\n');
    if (!end) {
      ok = 0;
      break;
    }
    *end = '\0';
    char *fields[12] = {0};
    fields[0] = cursor;
    size_t field_count = 1;
    for (char *p = cursor; p < end; ++p) {
      if (*p == '\t') {
        *p = '\0';
        if (field_count >= 12) {
          ok = 0;
          break;
        }
        fields[field_count++] = p + 1;
      }
    }
    int file_order = 0, line = 0, end_line = 0, is_method = 0;
    int is_private = 0, has_doc = 0, untyped_params = 0;
    int missing_return = 0, type_inferred = 0;
    char *name = NULL, *params = NULL;
    if (ok)
      ok = field_count == 12 && strcmp(fields[0], "function") == 0 &&
           fmt_parse_nonnegative(fields[1], &file_order) &&
           (size_t)file_order < files->len &&
           fmt_parse_nonnegative(fields[2], &line) && line > 0 &&
           fmt_parse_nonnegative(fields[3], &end_line) && end_line >= line &&
           fmt_parse_nonnegative(fields[4], &is_method) && is_method <= 1 &&
           fmt_parse_nonnegative(fields[5], &is_private) && is_private <= 1 &&
           fmt_parse_nonnegative(fields[6], &has_doc) && has_doc <= 1 &&
           fmt_parse_nonnegative(fields[7], &untyped_params) &&
           fmt_parse_nonnegative(fields[8], &missing_return) &&
           missing_return <= 1 &&
           fmt_parse_nonnegative(fields[9], &type_inferred) &&
           type_inferred <= 1;
    if (ok) {
      name = fmt_unhex(fields[10]);
      params = fmt_unhex(fields[11]);
      ok = name && params && *name && strlen(name) < sizeof(((Fn *)0)->name) &&
           strlen(params) < sizeof(((Fn *)0)->params) &&
           (nyt_ends_with(files->items[file_order], ".ny") ||
            nyt_ends_with(files->items[file_order], ".c") ||
            nyt_ends_with(files->items[file_order], ".h"));
    }
    if (ok) {
      Fn fn;
      memset(&fn, 0, sizeof(fn));
      snprintf(fn.name, sizeof(fn.name), "%s", name);
      snprintf(fn.params, sizeof(fn.params), "%s", params);
      snprintf(fn.file, sizeof(fn.file), "%s", files->items[file_order]);
      fn.line = line;
      fn.end_line = end_line;
      fn.is_method = is_method;
      fn.is_private = is_private;
      fn.has_doc = has_doc;
      fn.untyped_params = untyped_params;
      fn.missing_return = missing_return;
      fn.type_inferred_intent = type_inferred;
      fn.is_c = nyt_ends_with(files->items[file_order], ".c") ||
                nyt_ends_with(files->items[file_order], ".h");
      fv_push(fns, &fn);
      ok = fns->len > initial_len;
    }
    free(name);
    free(params);
    cursor = end + 1;
    if (!ok)
      break;
  }
  free(raw);
  if (!ok) {
    fns->len = initial_len;
    return 0;
  }
  if (fns->len > initial_len + 1)
    qsort(fns->items + initial_len, fns->len - initial_len, sizeof(Fn),
          fmt_fn_metadata_order);
  return 1;
}

static int fmt_collect_type_findings_embedded(const StrVec *files,
                                              IssueVec *issues,
                                              int *finding_count,
                                              int *suggestion_count) {
  if (!files || !issues || !finding_count || !suggestion_count ||
      files->len > 10000)
    return 0;
  size_t total = 0;
  char *payload = NULL;
  size_t payload_len = 0, payload_cap = 0;
  int ok = 1;
  for (size_t i = 0; ok && i < files->len; ++i) {
    const char *path = files->items[i];
    ok = fmt_append_hex(&payload, &payload_len, &payload_cap,
                        (const unsigned char *)path, strlen(path)) &&
         fmt_source_append_str(&payload, &payload_len, &payload_cap, "\t");
    if (ok && nyt_ends_with(path, ".ny")) {
      size_t n = 0;
      char *text = ny_read_file_raw(path, &n);
      if (!text || memchr(text, '\0', n) || n > 32u * 1024u * 1024u ||
          total > 128u * 1024u * 1024u - n) {
        free(text);
        ok = 0;
        break;
      }
      total += n;
      ok = fmt_append_hex(&payload, &payload_len, &payload_cap,
                          (const unsigned char *)text, n);
      free(text);
    }
    if (ok)
      ok = fmt_source_append_str(&payload, &payload_len, &payload_cap, "\n");
    if (payload_len > 256u * 1024u * 1024u)
      ok = 0;
  }
  char input_path[PATH_MAX] = {0}, output_path[PATH_MAX] = {0};
  if (!fmt_payload_path(input_path, sizeof(input_path)) ||
      !fmt_payload_path(output_path, sizeof(output_path)))
    ok = 0;
  if (ok)
    ok = ny_write_file(input_path, payload ? payload : "", payload_len) == 0;
  free(payload);
  static const char prefix[] =
      "use core\n"
      "use core.compiler.tools.fmt.analyze (scan_type_findings_batch)\n"
      "use core.os (file_read, file_write, exit)\n"
      "def input_path = ";
  char *source = NULL;
  size_t source_len = 0, source_cap = 0;
  ok =
      ok &&
      fmt_source_append(&source, &source_len, &source_cap, prefix,
                        sizeof(prefix) - 1) &&
      fmt_source_append_quoted(&source, &source_len, &source_cap, input_path) &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            "\ndef output_path = ") &&
      fmt_source_append_quoted(&source, &source_len, &source_cap,
                               output_path) &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            "\ndef raw = match file_read(input_path) { ok(v) "
                            "-> v err(_) -> exit(2) }\n"
                            "def rendered = scan_type_findings_batch(raw)\n"
                            "match file_write(output_path, rendered) { ok(_) "
                            "-> 0 err(_) -> exit(2) }\n");
  if (ok)
    ok = nytrix_embed_run_source(source, NULL) == 0;
  free(source);
  fmt_remove_payload(input_path);
  size_t raw_len = 0;
  char *raw = ok ? ny_read_file_raw(output_path, &raw_len) : NULL;
  fmt_remove_payload(output_path);
  if (!raw || raw_len > 128u * 1024u * 1024u) {
    free(raw);
    return 0;
  }

  size_t initial_len = issues->len;
  int initial_findings = *finding_count;
  int initial_suggestions = *suggestion_count;
  char *cursor = raw;
  while (*cursor) {
    char *end = strchr(cursor, '\n');
    if (!end) {
      ok = 0;
      break;
    }
    *end = '\0';
    char *fields[9] = {0};
    fields[0] = cursor;
    size_t field_count = 1;
    for (char *p = cursor; p < end; ++p) {
      if (*p == '\t') {
        *p = '\0';
        if (field_count >= 9) {
          ok = 0;
          break;
        }
        fields[field_count++] = p + 1;
      }
    }
    int file_order = 0, line = 0;
    char *path = NULL, *message = NULL, *note = NULL;
    int summary = ok && strcmp(fields[0], "summary") == 0;
    if (ok)
      ok =
          field_count == 8 && (strcmp(fields[0], "type") == 0 || summary) &&
          fmt_parse_nonnegative(fields[1], &file_order) &&
          (size_t)file_order < files->len &&
          fmt_parse_nonnegative(fields[2], &line) && line > 0 &&
          ((summary && strcmp(fields[3], "NYAUD9001") == 0) ||
           (!summary && (strcmp(fields[3], "NYAUD9002") == 0 ||
                         strcmp(fields[3], "NYAUD9003") == 0))) &&
          (strcmp(fields[4], "warning") == 0 || strcmp(fields[4], "note") == 0);
    if (ok) {
      path = fmt_unhex(fields[5]);
      message = fmt_unhex(fields[6]);
      note = fmt_unhex(fields[7]);
      ok = path && message && note &&
           strcmp(path, files->items[file_order]) == 0 &&
           strlen(path) < sizeof(((Issue *)0)->file) &&
           strlen(message) < sizeof(((Issue *)0)->msg) &&
           strlen(note) < sizeof(((Issue *)0)->note) &&
           nyt_ends_with(path, ".ny");
    }
    if (ok) {
      issue_push(issues, path, line, 1, fields[3], fields[4], message, note);
      if (issues->len <= initial_len) {
        ok = 0;
      } else {
        issues->items[issues->len - 1].file_order = (size_t)file_order;
        ++*finding_count;
        if (strstr(note, "confidence=high"))
          ++*suggestion_count;
      }
    }
    free(path);
    free(message);
    free(note);
    cursor = end + 1;
    if (!ok)
      break;
  }
  free(raw);
  if (!ok) {
    issues->len = initial_len;
    *finding_count = initial_findings;
    *suggestion_count = initial_suggestions;
    return 0;
  }
  return 1;
}

static int fmt_audit_issue_is_battery(const char *code) {
  return code &&
         (strcmp(code, "NYAUD2001") == 0 || strcmp(code, "NYAUD2002") == 0 ||
          strcmp(code, "NYAUD2003") == 0 || strcmp(code, "NYAUD2004") == 0 ||
          strcmp(code, "NYAUD2005") == 0 || strcmp(code, "NYAUD2006") == 0);
}

static int fmt_audit_issue_is_specialize(const char *code) {
  return code &&
         (strcmp(code, "NYAUD3101") == 0 || strcmp(code, "NYAUD3102") == 0 ||
          strcmp(code, "NYAUD3103") == 0 || strcmp(code, "NYAUD3104") == 0 ||
          strcmp(code, "NYAUD3105") == 0);
}

static int fmt_audit_issue_is_contract(const char *code) {
  return code && strcmp(code, "NYAUD6101") == 0;
}

static int fmt_audit_issue_is_literal(const char *code) {
  return code && strcmp(code, "NYAUD3006") == 0;
}

static int fmt_audit_issue_is_repeated_get(const char *code) {
  return code && strcmp(code, "NYAUD3005") == 0;
}

static int fmt_audit_issue_is_case_dispatch(const char *code) {
  return code && strcmp(code, "NYAUD3002") == 0;
}

static int fmt_audit_issue_is_trim_pressure(const char *code) {
  return code && strcmp(code, "NYAUD3001") == 0;
}

static int fmt_audit_issue_is_trivial_main(const char *code) {
  return code && strcmp(code, "NYAUD4902") == 0;
}

static int fmt_audit_issue_is_hot_file(const char *code) {
  return code && strcmp(code, "NYAUD1001") == 0;
}

static int fmt_audit_issue_is_hot_function(const char *code) {
  return code && strcmp(code, "NYAUD1002") == 0;
}

static int fmt_audit_issue_is_append_builder(const char *code) {
  return code &&
         (strcmp(code, "NYAUD3004") == 0 || strcmp(code, "NYAUD3009") == 0);
}

static int fmt_audit_issue_is_loop_guard(const char *code) {
  return code &&
         (strcmp(code, "NYAUD3007") == 0 || strcmp(code, "NYAUD3008") == 0);
}

static int fmt_audit_issue_is_dead_helper(const char *code) {
  return code && strcmp(code, "NYAUD7001") == 0;
}

static int fmt_audit_issue_is_extern_group(const char *code) {
  return code && strcmp(code, "NYAUD5001") == 0;
}

static int fmt_audit_issue_is_profile_helper(const char *code) {
  return code && strcmp(code, "NYAUD7201") == 0;
}

static int fmt_audit_issue_is_layout(const char *code) {
  return code && strcmp(code, "NYAUD6001") == 0;
}

static int fmt_audit_issue_is_metaprog_trim(const char *code) {
  return code &&
         (strcmp(code, "NYAUD4101") == 0 || strcmp(code, "NYAUD4102") == 0 ||
          strcmp(code, "NYAUD4103") == 0 || strcmp(code, "NYAUD4104") == 0 ||
          strcmp(code, "NYAUD4105") == 0 || strcmp(code, "NYAUD4201") == 0 ||
          strcmp(code, "NYAUD4202") == 0 || strcmp(code, "NYAUD4203") == 0 ||
          strcmp(code, "NYAUD4301") == 0 || strcmp(code, "NYAUD4302") == 0 ||
          strcmp(code, "NYAUD4401") == 0 || strcmp(code, "NYAUD4501") == 0 ||
          strcmp(code, "NYAUD4502") == 0 || strcmp(code, "NYAUD4503") == 0 ||
          strcmp(code, "NYAUD4601") == 0 || strcmp(code, "NYAUD4801") == 0 ||
          strcmp(code, "NYAUD4901") == 0 || strcmp(code, "NYAUD4701") == 0);
}

static int fmt_audit_issue_is_constant(const char *code) {
  return code &&
         (strcmp(code, "NYAUD7301") == 0 || strcmp(code, "NYAUD7302") == 0);
}

static int fmt_audit_issue_is_method_syntax(const char *code) {
  return code && strcmp(code, "NYAUD8101") == 0;
}

static int fmt_audit_issue_is_receiver_rewrite(const char *code) {
  return code && strcmp(code, "NYAUD8102") == 0;
}

static int fmt_audit_issue_is_legacy_call(const char *code) {
  return code && strcmp(code, "NYAUD8001") == 0;
}

static int fmt_audit_issue_is_indexed_write(const char *code) {
  return code && strcmp(code, "NYAUD3003") == 0;
}

static int fmt_audit_issue_is_variadic_free(const char *code) {
  return code && strcmp(code, "NYAUD4204") == 0;
}

static int fmt_audit_issue_has_metric(const char *code) {
  return fmt_audit_issue_is_method_syntax(code) ||
         fmt_audit_issue_is_receiver_rewrite(code) ||
         fmt_audit_issue_is_legacy_call(code);
}

static int fmt_audit_issue_is_advisory(const char *code) {
  return fmt_audit_issue_is_battery(code) ||
         fmt_audit_issue_is_specialize(code) ||
         fmt_audit_issue_is_append_builder(code) ||
         fmt_audit_issue_is_loop_guard(code) ||
         fmt_audit_issue_is_case_dispatch(code) ||
         fmt_audit_issue_is_trim_pressure(code) ||
         fmt_audit_issue_is_trivial_main(code) ||
         fmt_audit_issue_is_hot_file(code) ||
         fmt_audit_issue_is_hot_function(code) ||
         fmt_audit_issue_is_contract(code) ||
         fmt_audit_issue_is_literal(code) ||
         fmt_audit_issue_is_repeated_get(code) ||
         fmt_audit_issue_is_dead_helper(code) ||
         fmt_audit_issue_is_extern_group(code) ||
         fmt_audit_issue_is_profile_helper(code) ||
         fmt_audit_issue_is_layout(code) ||
         fmt_audit_issue_is_metaprog_trim(code) ||
         fmt_audit_issue_is_constant(code) ||
         fmt_audit_issue_is_method_syntax(code) ||
         fmt_audit_issue_is_receiver_rewrite(code) ||
         fmt_audit_issue_is_legacy_call(code) ||
         fmt_audit_issue_is_indexed_write(code) ||
         fmt_audit_issue_is_variadic_free(code);
}

static const char *fmt_audit_issue_mode(const char *code) {
  if (fmt_audit_issue_is_battery(code))
    return "batteries";
  if (fmt_audit_issue_is_hot_file(code) ||
      fmt_audit_issue_is_hot_function(code))
    return "bloat";
  if (fmt_audit_issue_is_specialize(code))
    return "specialize";
  if (fmt_audit_issue_is_append_builder(code) ||
      fmt_audit_issue_is_literal(code) ||
      fmt_audit_issue_is_repeated_get(code) ||
      fmt_audit_issue_is_case_dispatch(code) ||
      fmt_audit_issue_is_trim_pressure(code) ||
      fmt_audit_issue_is_trivial_main(code) ||
      fmt_audit_issue_is_variadic_free(code))
    return "trim";
  if (fmt_audit_issue_is_contract(code))
    return "contracts";
  if (fmt_audit_issue_is_dead_helper(code))
    return "dead";
  if (fmt_audit_issue_is_extern_group(code))
    return "ffi";
  if (fmt_audit_issue_is_profile_helper(code))
    return "profiles";
  if (fmt_audit_issue_is_metaprog_trim(code))
    return "meta";
  if (fmt_audit_issue_is_layout(code))
    return "layout";
  if (fmt_audit_issue_is_constant(code))
    return "constants";
  if (fmt_audit_issue_is_method_syntax(code))
    return "methods";
  if (fmt_audit_issue_is_indexed_write(code))
    return "trim";
  return "bugs";
}

static int fmt_audit_issue_wants_mode(const char *code, const char *mode) {
  if (fmt_audit_issue_is_loop_guard(code))
    return audit_wants(mode, "trim") || audit_wants(mode, "loops");
  if (fmt_audit_issue_is_constant(code))
    return audit_wants(mode, "constants") || audit_wants(mode, "trim") ||
           audit_wants(mode, "dead") || audit_wants(mode, "meta");
  if (fmt_audit_issue_is_receiver_rewrite(code) ||
      fmt_audit_issue_is_legacy_call(code) ||
      fmt_audit_issue_is_variadic_free(code))
    return audit_wants(mode, "legacy") || audit_wants(mode, "methods");
  return audit_wants(mode, fmt_audit_issue_mode(code));
}

static int fmt_audit_issue_allowed(const char *path, const char *code,
                                   const char *severity) {
  if (!path || !code || !severity)
    return 0;
  int is_ny = nyt_ends_with(path, ".ny");
  int is_c_family = nyt_ends_with(path, ".c") || nyt_ends_with(path, ".h");
  if (is_ny) {
    return (strcmp(code, "NYAUD1101") == 0 &&
            strcmp(severity, "warning") == 0) ||
           (strcmp(code, "NYAUD1102") == 0 && strcmp(severity, "note") == 0) ||
           ((strcmp(code, "NYAUD1105") == 0 || strcmp(code, "NYAUD1106") == 0 ||
             strcmp(code, "NYAUD1107") == 0 || strcmp(code, "NYAUD1108") == 0 ||
             strcmp(code, "NYAUD1109") == 0 || strcmp(code, "NYAUD1110") == 0 ||
             strcmp(code, "NYAUD1111") == 0 || strcmp(code, "NYAUD1112") == 0 ||
             strcmp(code, "NYAUD1113") == 0 || strcmp(code, "NYAUD1114") == 0 ||
             strcmp(code, "NYAUD1115") == 0 || strcmp(code, "NYAUD1116") == 0 ||
             strcmp(code, "NYAUD1117") == 0 || strcmp(code, "NYAUD1118") == 0 ||
             strcmp(code, "NYAUD1119") == 0 ||
             strcmp(code, "NYAUD1120") == 0) &&
            strcmp(severity, "warning") == 0) ||
           ((strcmp(code, "NYAUD2001") == 0 || strcmp(code, "NYAUD2002") == 0 ||
             strcmp(code, "NYAUD2005") == 0 ||
             strcmp(code, "NYAUD2006") == 0) &&
            strcmp(severity, "warning") == 0) ||
           ((strcmp(code, "NYAUD2003") == 0 ||
             strcmp(code, "NYAUD2004") == 0) &&
            strcmp(severity, "note") == 0) ||
           ((strcmp(code, "NYAUD3101") == 0 || strcmp(code, "NYAUD3102") == 0 ||
             strcmp(code, "NYAUD3103") == 0 ||
             strcmp(code, "NYAUD3104") == 0) &&
            strcmp(severity, "note") == 0) ||
           (strcmp(code, "NYAUD3105") == 0 &&
            strcmp(severity, "warning") == 0) ||
           ((strcmp(code, "NYAUD3004") == 0 &&
             (strcmp(severity, "note") == 0 ||
              strcmp(severity, "warning") == 0)) ||
            (strcmp(code, "NYAUD3009") == 0 && strcmp(severity, "note") == 0) ||
            (fmt_audit_issue_is_loop_guard(code) &&
             strcmp(severity, "note") == 0)) ||
           ((fmt_audit_issue_is_contract(code) ||
             fmt_audit_issue_is_literal(code)) &&
            (strcmp(severity, "note") == 0 ||
             strcmp(severity, "warning") == 0)) ||
           (fmt_audit_issue_is_dead_helper(code) &&
            strcmp(severity, "note") == 0) ||
           (fmt_audit_issue_is_extern_group(code) &&
            (strcmp(severity, "note") == 0 ||
             strcmp(severity, "warning") == 0)) ||
           (fmt_audit_issue_is_profile_helper(code) &&
            strcmp(severity, "note") == 0) ||
           (fmt_audit_issue_is_layout(code) &&
            (strcmp(severity, "note") == 0 ||
             strcmp(severity, "warning") == 0)) ||
           (fmt_audit_issue_is_metaprog_trim(code) &&
            (strcmp(severity, "note") == 0 ||
             strcmp(severity, "warning") == 0)) ||
           ((fmt_audit_issue_is_method_syntax(code) ||
             fmt_audit_issue_is_receiver_rewrite(code) ||
             fmt_audit_issue_is_legacy_call(code)) &&
            (strcmp(severity, "note") == 0 ||
             strcmp(severity, "warning") == 0)) ||
           (fmt_audit_issue_is_indexed_write(code) &&
            strcmp(severity, "note") == 0) ||
           (fmt_audit_issue_is_repeated_get(code) &&
            (strcmp(severity, "note") == 0 ||
             strcmp(severity, "warning") == 0)) ||
           (fmt_audit_issue_is_case_dispatch(code) &&
            strcmp(severity, "note") == 0) ||
           (fmt_audit_issue_is_trim_pressure(code) &&
            strcmp(severity, "note") == 0) ||
           (fmt_audit_issue_is_trivial_main(code) &&
            strcmp(severity, "note") == 0) ||
           ((fmt_audit_issue_is_hot_file(code) ||
             fmt_audit_issue_is_hot_function(code)) &&
            strcmp(severity, "warning") == 0) ||
           (strcmp(code, "NYAUD7301") == 0 &&
            (strcmp(severity, "note") == 0 ||
             strcmp(severity, "warning") == 0)) ||
           (strcmp(code, "NYAUD7302") == 0 &&
            strcmp(severity, "warning") == 0) ||
           (fmt_audit_issue_is_variadic_free(code) &&
            strcmp(severity, "note") == 0);
  }
  if (!is_c_family)
    return 0;
  return (fmt_audit_issue_is_battery(code) &&
          (strcmp(severity, "note") == 0 ||
           strcmp(severity, "warning") == 0)) ||
         (strcmp(code, "NYAUD1102") == 0 && strcmp(severity, "note") == 0) ||
         ((strcmp(code, "NYAUD4501") == 0 || strcmp(code, "NYAUD4502") == 0 ||
           strcmp(code, "NYAUD4503") == 0 || strcmp(code, "NYAUD4701") == 0) &&
          strcmp(severity, "note") == 0) ||
         (fmt_audit_issue_is_layout(code) &&
          (strcmp(severity, "note") == 0 ||
           strcmp(severity, "warning") == 0)) ||
         ((fmt_audit_issue_is_append_builder(code) ||
           fmt_audit_issue_is_trim_pressure(code)) &&
          (strcmp(severity, "note") == 0 ||
           strcmp(severity, "warning") == 0)) ||
         ((fmt_audit_issue_is_hot_file(code) ||
           fmt_audit_issue_is_hot_function(code)) &&
          strcmp(severity, "warning") == 0) ||
         (fmt_audit_issue_is_method_syntax(code) &&
          (strcmp(severity, "note") == 0 ||
           strcmp(severity, "warning") == 0)) ||
         ((fmt_audit_issue_is_receiver_rewrite(code) ||
           fmt_audit_issue_is_legacy_call(code)) &&
          (strcmp(severity, "note") == 0 ||
           strcmp(severity, "warning") == 0)) ||
         ((strcmp(code, "NYAUD1103") == 0 || strcmp(code, "NYAUD1104") == 0 ||
           strcmp(code, "NYAUD1108") == 0 || strcmp(code, "NYAUD1109") == 0 ||
           strcmp(code, "NYAUD1110") == 0 || strcmp(code, "NYAUD1111") == 0 ||
           strcmp(code, "NYAUD1112") == 0 || strcmp(code, "NYAUD1115") == 0 ||
           strcmp(code, "NYAUD1118") == 0) &&
          strcmp(severity, "warning") == 0) ||
         (fmt_audit_issue_is_loop_guard(code) && strcmp(severity, "note") == 0);
}

static int fmt_collect_audit_findings_embedded(const StrVec *files,
                                               IssueVec *issues,
                                               AuditStats *stats,
                                               const char *mode) {
  if (!files || !issues || !stats || files->len > 10000)
    return 0;
  char *payload = NULL;
  size_t payload_len = 0, payload_cap = 0, total = 0;
  int ok = 1;
  for (size_t i = 0; ok && i < files->len; ++i) {
    const char *path = files->items[i];
    if (!nyt_ends_with(path, ".ny"))
      continue;
    ok = fmt_append_hex(&payload, &payload_len, &payload_cap,
                        (const unsigned char *)path, strlen(path)) &&
         fmt_source_append_str(&payload, &payload_len, &payload_cap, "\t");
    if (ok && (nyt_ends_with(path, ".ny") || nyt_ends_with(path, ".c") ||
               nyt_ends_with(path, ".h"))) {
      size_t n = 0;
      char *source = ny_read_file_raw(path, &n);
      if (!source || memchr(source, '\0', n) || n > 32u * 1024u * 1024u ||
          total > 128u * 1024u * 1024u - n) {
        free(source);
        ok = 0;
        break;
      }
      total += n;
      ok = fmt_append_hex(&payload, &payload_len, &payload_cap,
                          (const unsigned char *)source, n);
      free(source);
    }
    if (ok)
      ok = fmt_source_append_str(&payload, &payload_len, &payload_cap, "\n");
    if (payload_len > 256u * 1024u * 1024u)
      ok = 0;
  }
  if (ok && payload_len == 0) {
    free(payload);
    return 1;
  }

  char input_path[PATH_MAX] = {0}, output_path[PATH_MAX] = {0};
  if (!fmt_payload_path(input_path, sizeof(input_path)) ||
      !fmt_payload_path(output_path, sizeof(output_path)))
    ok = 0;
  if (ok)
    ok = ny_write_file(input_path, payload ? payload : "", payload_len) == 0;
  free(payload);
  static const char prefix[] =
      "use core\n"
      "use core.compiler.tools.fmt.analyze (scan_audit_batch)\n"
      "use core.os (file_read, file_write, exit)\n"
      "def input_path = ";
  char *source = NULL;
  size_t source_len = 0, source_cap = 0;
  ok =
      ok &&
      fmt_source_append(&source, &source_len, &source_cap, prefix,
                        sizeof(prefix) - 1) &&
      fmt_source_append_quoted(&source, &source_len, &source_cap, input_path) &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            "\ndef output_path = ") &&
      fmt_source_append_quoted(&source, &source_len, &source_cap,
                               output_path) &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            "\ndef raw = match file_read(input_path) { ok(v) "
                            "-> v err(_) -> exit(2) }\n"
                            "def rendered = scan_audit_batch(raw)\n"
                            "match file_write(output_path, rendered) { ok(_) "
                            "-> 0 err(_) -> exit(2) }\n");
  const nytrix_embed_run_config_t run_config = {.opt_level = 2, .reserved = 0};
  if (ok)
    ok = nytrix_embed_run_source(source, &run_config) == 0;
  free(source);
  fmt_remove_payload(input_path);
  size_t raw_len = 0;
  char *raw = ok ? ny_read_file_raw(output_path, &raw_len) : NULL;
  fmt_remove_payload(output_path);
  if (!raw || raw_len > 128u * 1024u * 1024u) {
    free(raw);
    return 0;
  }

  size_t initial_len = issues->len;
  int initial_findings = stats->findings;
  int initial_trim_targets = stats->trim_targets;
  int initial_method_syntax = stats->method_syntax;
  int initial_append_builders = stats->append_builders;
  int initial_repeated_get_shapes = stats->repeated_get_shapes;
  int initial_legacy_calls = stats->legacy_calls;
  int initial_receiver_rewrites = stats->receiver_rewrites;
  char *cursor = raw;
  while (*cursor) {
    char *end = strchr(cursor, '\n');
    if (!end) {
      ok = 0;
      break;
    }
    *end = '\0';
    char *fields[9] = {0};
    fields[0] = cursor;
    size_t field_count = 1;
    for (char *p = cursor; p < end; ++p) {
      if (*p == '\t') {
        *p = '\0';
        if (field_count >= 9) {
          ok = 0;
          break;
        }
        fields[field_count++] = p + 1;
      }
    }
    int file_order = 0, line = 0, column = 1;
    char *path = NULL;
    int metric_issue = ok && strcmp(fields[0], "issue_metric") == 0;
    int column_issue = ok && strcmp(fields[0], "issue_column") == 0;
    int enriched_issue =
        ok && (strcmp(fields[0], "issue") == 0 || metric_issue || column_issue);
    size_t code_field = column_issue ? 5u : 4u;
    size_t severity_field = column_issue ? 6u : 5u;
    size_t message_field = column_issue ? 7u : 6u;
    size_t note_field = column_issue ? 8u : 7u;
    int active_finding =
        enriched_issue && fmt_audit_issue_wants_mode(fields[code_field], mode);
    if (ok)
      ok = field_count == (metric_issue || column_issue ? 9u : 8u) &&
           enriched_issue && fmt_parse_nonnegative(fields[1], &file_order) &&
           (size_t)file_order < files->len &&
           fmt_parse_nonnegative(fields[3], &line) && line > 0 &&
           (nyt_ends_with(files->items[file_order], ".ny") ||
            nyt_ends_with(files->items[file_order], ".c") ||
            nyt_ends_with(files->items[file_order], ".h"));
    if (ok && column_issue)
      ok = fmt_parse_nonnegative(fields[4], &column) && column > 0;
    if (ok && enriched_issue)
      ok = fmt_audit_issue_allowed(files->items[file_order], fields[code_field],
                                   fields[severity_field]);
    char *issue_message = NULL;
    char *issue_note = NULL;
    int metric_count = 0;
    if (ok && enriched_issue) {
      ok = fmt_audit_issue_allowed(files->items[file_order], fields[code_field],
                                   fields[severity_field]);
      if (ok) {
        size_t message_hex_len = strlen(fields[message_field]);
        size_t note_hex_len = strlen(fields[note_field]);
        ok = !(message_hex_len & 1) && !(note_hex_len & 1) &&
             message_hex_len / 2 < sizeof(((Issue *)0)->msg) &&
             note_hex_len / 2 < sizeof(((Issue *)0)->note);
      }
      if (ok) {
        issue_message = fmt_unhex(fields[message_field]);
        issue_note = fmt_unhex(fields[note_field]);
        ok = issue_message && issue_note && issue_message[0] && issue_note[0] &&
             strlen(issue_message) == strlen(fields[message_field]) / 2 &&
             strlen(issue_note) == strlen(fields[note_field]) / 2;
      }
      if (ok)
        ok = fmt_audit_issue_has_metric(fields[code_field]) == metric_issue;
      if (ok && metric_issue)
        ok =
            fmt_parse_nonnegative(fields[8], &metric_count) && metric_count > 0;
    }
    if (ok) {
      path = fmt_unhex(fields[2]);
      ok = path && strcmp(path, files->items[file_order]) == 0 &&
           strlen(path) < sizeof(((Issue *)0)->file);
    }
    if (ok && active_finding) {
      issue_push(issues, path, line, column, fields[code_field],
                 fields[severity_field], issue_message, issue_note);
      if (issues->len <= initial_len) {
        ok = 0;
      } else {
        issues->items[issues->len - 1].file_order = (size_t)file_order;
        stats->findings++;
        if (!fmt_audit_issue_is_advisory(fields[code_field]))
          stats->bug_findings++;
        if (fmt_audit_issue_is_hot_file(fields[code_field]))
          stats->hot_files++;
        if (fmt_audit_issue_is_hot_function(fields[code_field]))
          stats->hot_functions++;
        if (fmt_audit_issue_is_method_syntax(fields[code_field]) ||
            fmt_audit_issue_is_receiver_rewrite(fields[code_field]) ||
            fmt_audit_issue_is_indexed_write(fields[code_field]) ||
            fmt_audit_issue_is_variadic_free(fields[code_field]))
          stats->trim_targets++;
        if (fmt_audit_issue_is_specialize(fields[code_field]))
          stats->trim_targets++;
        if (fmt_audit_issue_is_contract(fields[code_field]) ||
            fmt_audit_issue_is_literal(fields[code_field]) ||
            fmt_audit_issue_is_repeated_get(fields[code_field]) ||
            fmt_audit_issue_is_case_dispatch(fields[code_field]) ||
            fmt_audit_issue_is_trim_pressure(fields[code_field]) ||
            fmt_audit_issue_is_trivial_main(fields[code_field]) ||
            fmt_audit_issue_is_metaprog_trim(fields[code_field]))
          stats->trim_targets++;
        if (fmt_audit_issue_is_trivial_main(fields[code_field]))
          stats->trivial_main_wrappers++;
        if (fmt_audit_issue_is_literal(fields[code_field]))
          stats->literal_tables++;
        if (fmt_audit_issue_is_repeated_get(fields[code_field]))
          stats->repeated_get_shapes++;
        if (fmt_audit_issue_is_append_builder(fields[code_field])) {
          stats->trim_targets++;
          stats->append_builders++;
        }
        if (fmt_audit_issue_is_loop_guard(fields[code_field]))
          stats->trim_targets++;
        if (fmt_audit_issue_is_profile_helper(fields[code_field]))
          stats->trim_targets++;
        if (fmt_audit_issue_is_constant(fields[code_field]))
          stats->trim_targets++;
      }
    }
    if (ok && metric_issue) {
      if (fmt_audit_issue_is_method_syntax(fields[code_field]) &&
          active_finding)
        stats->method_syntax += metric_count;
      if (fmt_audit_issue_is_receiver_rewrite(fields[code_field]))
        stats->receiver_rewrites += metric_count;
      if (fmt_audit_issue_is_legacy_call(fields[code_field]))
        stats->legacy_calls += metric_count;
    }
    free(path);
    free(issue_message);
    free(issue_note);
    cursor = end + 1;
    if (!ok)
      break;
  }
  free(raw);
  if (!ok) {
    issues->len = initial_len;
    stats->findings = initial_findings;
    stats->trim_targets = initial_trim_targets;
    stats->method_syntax = initial_method_syntax;
    stats->append_builders = initial_append_builders;
    stats->repeated_get_shapes = initial_repeated_get_shapes;
    stats->legacy_calls = initial_legacy_calls;
    stats->receiver_rewrites = initial_receiver_rewrites;
    return 0;
  }
  return 1;
}

typedef struct {
  int line;
  int col;
  int net;
  size_t offset;
  char context[160];
} FmtStructureSwallow;

typedef struct {
  char path[PATH_MAX];
  int file_order;
  int depth;
  int extra;
  int declared_swallows;
  int swallow_count;
  int fix_count;
  int report_seen;
  char report[8192];
  FmtStructureSwallow swallows[64];
  size_t fix_offsets[64];
} FmtStructureResult;

typedef struct {
  char path[PATH_MAX];
  int count;
  size_t offsets[64];
} FmtStructureFix;

static int fmt_structure_validate_result(const FmtStructureResult *result) {
  if (!result || result->swallow_count != result->declared_swallows ||
      result->fix_count != result->declared_swallows ||
      result->report_seen != 1 ||
      (!(result->depth || result->extra) && result->declared_swallows != 0))
    return 0;
  for (int i = 0; i < result->declared_swallows; ++i) {
    if (result->fix_offsets[i] != result->swallows[i].offset ||
        (i > 0 && result->swallows[i - 1].offset >= result->swallows[i].offset))
      return 0;
  }
  return 1;
}

static int fmt_structure_emit(const FmtStructureResult *result) {
  if (!result)
    return 0;
  if (is_expected_error_fixture(result->path))
    return 0;
  int real_swallows =
      (result->depth || result->extra) ? result->swallow_count : 0;
  if (result->report[0])
    fputs(result->report, stdout);
  return result->depth > 0 || result->extra > 0 || real_swallows > 0;
}

static int fmt_structure_apply_fix(const FmtStructureFix *entry) {
  if (!entry || entry->count <= 0 || entry->count > 64)
    return 0;
  size_t n = 0;
  char *text = ny_read_file_raw(entry->path, &n);
  char *fixed = text ? (char *)malloc(n + 1) : NULL;
  if (!fixed) {
    free(text);
    return 0;
  }
  memcpy(fixed, text, n);
  fixed[n] = '\0';
  for (int i = entry->count - 1; i >= 0; --i) {
    size_t offset = entry->offsets[i];
    if (offset >= n || fixed[offset] != ';') {
      free(text);
      free(fixed);
      return 0;
    }
    memmove(fixed + offset, fixed + offset + 1, n - offset);
    --n;
  }
  int ok = ny_write_file(entry->path, fixed, n) == 0;
  if (ok)
    printf("  %sfixed%s semicolon removed\n", nyt_clr(NYT_GREEN),
           nyt_clr(NYT_RESET));
  free(text);
  free(fixed);
  return ok;
}

static int fmt_check_structure_embedded(const StrVec *files, int fix,
                                        int verbose, int *failed) {
  if (!files || !failed || files->len > 10000)
    return 0;
  size_t total = 0;
  char *payload = NULL;
  size_t payload_len = 0, payload_cap = 0;
  int ok = 1;
  size_t check_count = 0;
  for (size_t i = 0; ok && i < files->len; ++i) {
    int skip = is_expected_error_fixture(files->items[i]);
    size_t n = 0;
    char *text = skip ? strdup("") : ny_read_file_raw(files->items[i], &n);
    if (!text || memchr(text, '\0', n) || n > 32u * 1024u * 1024u ||
        total > 128u * 1024u * 1024u - n || (!skip && check_count == 10000)) {
      free(text);
      ok = 0;
      break;
    }
    total += n;
    if (!skip)
      ++check_count;
    ok = fmt_append_hex(&payload, &payload_len, &payload_cap,
                        (const unsigned char *)files->items[i],
                        strlen(files->items[i])) &&
         fmt_source_append_str(&payload, &payload_len, &payload_cap, "\t") &&
         fmt_append_hex(&payload, &payload_len, &payload_cap,
                        (const unsigned char *)text, n) &&
         fmt_source_append_str(&payload, &payload_len, &payload_cap, "\n");
    free(text);
    if (payload_len > 256u * 1024u * 1024u)
      ok = 0;
  }

  char input_path[PATH_MAX] = {0}, output_path[PATH_MAX] = {0};
  if (!fmt_payload_path(input_path, sizeof(input_path)) ||
      !fmt_payload_path(output_path, sizeof(output_path)))
    ok = 0;
  if (ok)
    ok = ny_write_file(input_path, payload ? payload : "", payload_len) == 0;
  free(payload);
  static const char prefix[] =
      "use core\n"
      "use core.compiler.tools.fmt.analyze (scan_structure_batch)\n"
      "use core.os (file_read, file_write, exit)\n"
      "def input_path = ";
  char *source = NULL;
  size_t source_len = 0, source_cap = 0;
  ok =
      ok &&
      fmt_source_append(&source, &source_len, &source_cap, prefix,
                        sizeof(prefix) - 1) &&
      fmt_source_append_quoted(&source, &source_len, &source_cap, input_path) &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            "\ndef output_path = ") &&
      fmt_source_append_quoted(&source, &source_len, &source_cap,
                               output_path) &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            "\ndef raw = match file_read(input_path) { ok(v) "
                            "-> v err(_) -> exit(2) }\n"
                            "def rendered = scan_structure_batch(raw, ") &&
      fmt_source_append_str(&source, &source_len, &source_cap,
                            verbose ? "1, [" : "0, [");
  const char *structure_colors[] = {nyt_clr(NYT_RED), nyt_clr(NYT_YELLOW),
                                    nyt_clr(NYT_CYAN), nyt_clr(NYT_GREEN),
                                    nyt_clr(NYT_RESET)};
  for (size_t i = 0;
       ok && i < sizeof(structure_colors) / sizeof(structure_colors[0]); ++i) {
    if (i > 0)
      ok = fmt_source_append_str(&source, &source_len, &source_cap, ", ");
    if (ok)
      ok = fmt_source_append_quoted(&source, &source_len, &source_cap,
                                    structure_colors[i]);
  }
  ok = ok && fmt_source_append_str(&source, &source_len, &source_cap,
                                   "])\n"
                                   "match file_write(output_path, rendered) { "
                                   "ok(_) -> 0 err(_) -> exit(2) }\n");
  if (ok)
    ok = nytrix_embed_run_source(source, NULL) == 0;
  free(source);
  fmt_remove_payload(input_path);
  size_t raw_len = 0;
  char *raw = ok ? ny_read_file_raw(output_path, &raw_len) : NULL;
  fmt_remove_payload(output_path);
  if (!raw || raw_len > 128u * 1024u * 1024u) {
    free(raw);
    return 0;
  }

  int initial_failed = *failed;
  FmtStructureResult current = {0};
  int have_current = 0;
  int last_file_order = -1;
  FmtStructureFix *pending_fixes =
      fix ? (FmtStructureFix *)calloc(files->len ? files->len : 1,
                                      sizeof(*pending_fixes))
          : NULL;
  size_t pending_count = 0;
  if (fix && !pending_fixes) {
    free(raw);
    return 0;
  }
  char *cursor = raw;
  while (*cursor) {
    char *end = strchr(cursor, '\n');
    if (!end) {
      ok = 0;
      break;
    }
    *end = '\0';
    char *fields[7] = {0};
    fields[0] = cursor;
    size_t field_count = 1;
    for (char *p = cursor; p < end; ++p) {
      if (*p == '\t') {
        *p = '\0';
        if (field_count >= 7) {
          ok = 0;
          break;
        }
        fields[field_count++] = p + 1;
      }
    }
    if (!ok)
      break;
    if (strcmp(fields[0], "file") == 0) {
      if (have_current && !fmt_structure_validate_result(&current)) {
        ok = 0;
        break;
      }
      if (have_current) {
        *failed += fmt_structure_emit(&current);
        if (fix && current.fix_count > 0) {
          FmtStructureFix *entry = &pending_fixes[pending_count++];
          snprintf(entry->path, sizeof(entry->path), "%s", current.path);
          entry->count = current.fix_count;
          for (int i = 0; i < current.fix_count; ++i)
            entry->offsets[i] = current.fix_offsets[i];
        }
      }
      int order = 0;
      char *path = NULL;
      ok = field_count == 6 && fmt_parse_nonnegative(fields[1], &order) &&
           (size_t)order < files->len && order > last_file_order &&
           fmt_parse_nonnegative(fields[2], &current.depth) &&
           fmt_parse_nonnegative(fields[3], &current.extra) &&
           fmt_parse_nonnegative(fields[4], &current.declared_swallows) &&
           current.declared_swallows <= 64;
      if (ok) {
        path = fmt_unhex(fields[5]);
        ok = path && strlen(path) < sizeof(current.path) &&
             strcmp(path, files->items[order]) == 0 &&
             !is_expected_error_fixture(path);
      }
      if (ok) {
        current.file_order = order;
        last_file_order = order;
        memset(current.path, 0, sizeof(current.path));
        snprintf(current.path, sizeof(current.path), "%s", path);
        current.swallow_count = 0;
        current.fix_count = 0;
        current.report_seen = 0;
        current.report[0] = '\0';
        have_current = 1;
      }
      free(path);
    } else if (strcmp(fields[0], "swallow") == 0) {
      int order = 0, line = 0, col = 0, net = 0;
      char *context = NULL;
      ok = have_current && field_count == 7 &&
           fmt_parse_nonnegative(fields[1], &order) &&
           order < (int)files->len && order == current.file_order &&
           strcmp(files->items[order], current.path) == 0 &&
           fmt_parse_nonnegative(fields[2], &line) && line > 0 &&
           fmt_parse_nonnegative(fields[3], &col) && col > 0 &&
           fmt_parse_nonnegative(fields[4], &net) && net > 0;
      size_t offset = 0;
      if (ok) {
        int parsed_offset = 0;
        ok = fmt_parse_nonnegative(fields[5], &parsed_offset);
        offset = (size_t)parsed_offset;
        context = ok ? fmt_unhex(fields[6]) : NULL;
        ok = context && strlen(context) < sizeof(current.swallows[0].context) &&
             current.swallow_count < 64;
      }
      if (ok) {
        FmtStructureSwallow *sw = &current.swallows[current.swallow_count++];
        sw->line = line;
        sw->col = col;
        sw->net = net;
        sw->offset = offset;
        snprintf(sw->context, sizeof(sw->context), "%s", context);
      }
      free(context);
    } else if (strcmp(fields[0], "fix") == 0) {
      int order = 0, parsed_offset = 0;
      ok = have_current && field_count == 3 &&
           fmt_parse_nonnegative(fields[1], &order) &&
           (size_t)order < files->len && order == current.file_order &&
           strcmp(files->items[order], current.path) == 0 &&
           (current.depth > 0 || current.extra > 0) && current.fix_count < 64 &&
           fmt_parse_nonnegative(fields[2], &parsed_offset);
      if (ok)
        current.fix_offsets[current.fix_count++] = (size_t)parsed_offset;
    } else if (strcmp(fields[0], "report") == 0) {
      int order = 0;
      char *report = NULL;
      ok = have_current && field_count == 3 &&
           fmt_parse_nonnegative(fields[1], &order) &&
           (size_t)order < files->len && order == current.file_order &&
           strcmp(files->items[order], current.path) == 0 &&
           current.report_seen == 0;
      if (ok) {
        size_t report_hex_len = strlen(fields[2]);
        ok = report_hex_len <= (sizeof(current.report) - 1) * 2 &&
             (report_hex_len & 1) == 0;
      }
      if (ok) {
        report = fmt_unhex(fields[2]);
        ok = report && strlen(report) == strlen(fields[2]) / 2 &&
             strlen(report) < sizeof(current.report);
      }
      if (ok) {
        memcpy(current.report, report, strlen(report) + 1);
        current.report_seen = 1;
      }
      free(report);
    } else {
      ok = 0;
    }
    cursor = end + 1;
    if (!ok)
      break;
  }
  if (ok && have_current && !fmt_structure_validate_result(&current))
    ok = 0;
  if (ok && have_current)
    *failed += fmt_structure_emit(&current);
  if (ok && have_current && fix) {
    if (current.fix_count > 0) {
      FmtStructureFix *entry = &pending_fixes[pending_count++];
      snprintf(entry->path, sizeof(entry->path), "%s", current.path);
      entry->count = current.fix_count;
      for (int i = 0; i < current.fix_count; ++i)
        entry->offsets[i] = current.fix_offsets[i];
    }
  }
  free(raw);
  if (!ok) {
    *failed = initial_failed;
    free(pending_fixes);
    return 0;
  }
  for (size_t i = 0; i < pending_count; ++i) {
    if (!fmt_structure_apply_fix(&pending_fixes[i])) {
      free(pending_fixes);
      *failed = initial_failed;
      return 0;
    }
  }
  free(pending_fixes);
  return 1;
}

static int fmt_render_issues_embedded(IssueVec *issues, int limit,
                                      const char *min_sev) {
  if (!issues)
    return 0;
  size_t visible = 0;
  for (size_t i = 0; i < issues->len; ++i)
    visible += issue_visible_for_min(&issues->items[i], min_sev) != 0;
  if (visible == 0)
    return 1;
  if (visible > 10000)
    return 0;

  char output_path[PATH_MAX] = {0};
  if (!fmt_payload_path(output_path, sizeof(output_path)))
    return 0;
  char *source = NULL;
  size_t len = 0, cap = 0;
  int ok = fmt_source_append_str(
      &source, &len, &cap,
      "use core\n"
      "use core.compiler.tools.fmt.report (render_issues)\n"
      "use core.os (file_write, exit)\n"
      "def rows = [");
  size_t emitted = 0;
  for (size_t i = 0; ok && i < issues->len; ++i) {
    const Issue *it = &issues->items[i];
    if (!issue_visible_for_min(it, min_sev))
      continue;
    if (emitted++)
      ok = fmt_source_append_str(&source, &len, &cap, ",");
    if (ok)
      ok = fmt_source_append_str(&source, &len, &cap, "[") &&
           fmt_source_append_quoted(&source, &len, &cap, it->file);
    char line_text[32], col_text[32];
    snprintf(line_text, sizeof(line_text), "%d", it->line);
    snprintf(col_text, sizeof(col_text), "%d", it->col);
    if (ok)
      ok = fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, line_text) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, col_text) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, it->code) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, it->sev) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, it->msg) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, it->note) &&
           fmt_source_append_str(&source, &len, &cap, ",");
    if (ok && issue_is_audit(it)) {
      char id[512];
      audit_issue_id(it, id, sizeof(id));
      ok = fmt_source_append_quoted(&source, &len, &cap, id) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap,
                                    audit_rewrite_shape_for_code(it->code)) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap,
                                    audit_syntax_hint_for_code(it->code));
    } else if (ok) {
      ok = fmt_source_append_str(&source, &len, &cap, "\"\",\"\",\"\"");
    }
    if (ok)
      ok = fmt_source_append_str(&source, &len, &cap, "]");
    if (len > 32u * 1024u * 1024u)
      ok = 0;
  }
  if (ok)
    ok = fmt_source_append_str(&source, &len, &cap, "]\ndef colors = [");
  const char *colors[] = {nyt_clr(NYT_CYAN), nyt_clr(NYT_MAGENTA),
                          nyt_clr(NYT_RED),  nyt_clr(NYT_YELLOW),
                          nyt_clr(NYT_CYAN), nyt_clr(NYT_GRAY),
                          nyt_clr(NYT_RESET)};
  for (size_t i = 0; ok && i < sizeof(colors) / sizeof(colors[0]); ++i) {
    if (i)
      ok = fmt_source_append_str(&source, &len, &cap, ",");
    if (ok)
      ok = fmt_source_append_quoted(&source, &len, &cap, colors[i]);
  }
  char limit_text[32];
  snprintf(limit_text, sizeof(limit_text), "%d", limit);
  if (ok)
    ok = fmt_source_append_str(&source, &len, &cap,
                               "]\ndef rendered = render_issues(rows, ") &&
         fmt_source_append_str(&source, &len, &cap, limit_text) &&
         fmt_source_append_str(&source, &len, &cap,
                               ", colors)\ndef output_path = ") &&
         fmt_source_append_quoted(&source, &len, &cap, output_path) &&
         fmt_source_append_str(&source, &len, &cap,
                               "\nmatch file_write(output_path, rendered) { "
                               "ok(_) -> 0 err(_) -> exit(2) }\n");
  if (ok)
    ok = nytrix_embed_run_source(source, NULL) == 0;
  free(source);
  char *rendered = ok ? ny_read_file_raw(output_path, NULL) : NULL;
  fmt_remove_payload(output_path);
  if (!rendered)
    return 0;
  fputs(rendered, stdout);
  free(rendered);
  return 1;
}

static int fmt_render_analyze_json_embedded(const AnalyzeStats *stats,
                                            IssueVec *issues) {
  if (!stats || !issues || issues->len > 10000)
    return 0;
  char output_path[PATH_MAX] = {0};
  if (!fmt_payload_path(output_path, sizeof(output_path)))
    return 0;
  char *source = NULL;
  size_t len = 0, cap = 0;
  int ok = fmt_source_append_str(
      &source, &len, &cap,
      "use core\n"
      "use core.compiler.tools.fmt.report (render_analyze_json)\n"
      "use core.os (file_write, exit)\n"
      "def counts = [");
  const int counts[] = {
      stats->files,          stats->ny_files,    stats->c_files,
      stats->h_files,        stats->functions,   stats->ny_functions,
      stats->c_functions,    stats->missing_doc, stats->duplicate_names,
      stats->long_functions, stats->long_lines,  stats->tabs,
      stats->trailing_ws,    stats->todos,       stats->unsafe_c};
  for (size_t i = 0; ok && i < sizeof(counts) / sizeof(counts[0]); ++i) {
    char number[32];
    snprintf(number, sizeof(number), "%s%d", i ? "," : "", counts[i]);
    ok = fmt_source_append_str(&source, &len, &cap, number);
  }
  if (ok)
    ok = fmt_source_append_str(&source, &len, &cap, "]\ndef rows = [");
  for (size_t i = 0; ok && i < issues->len; ++i) {
    const Issue *it = &issues->items[i];
    if (i)
      ok = fmt_source_append_str(&source, &len, &cap, ",");
    char line_text[32], col_text[32];
    snprintf(line_text, sizeof(line_text), "%d", it->line);
    snprintf(col_text, sizeof(col_text), "%d", it->col);
    if (ok)
      ok = fmt_source_append_str(&source, &len, &cap, "[") &&
           fmt_source_append_quoted(&source, &len, &cap, it->file) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, line_text) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, col_text) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, it->code) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, it->sev) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, it->msg) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, it->note) &&
           fmt_source_append_str(&source, &len, &cap, "]");
    if (len > 32u * 1024u * 1024u)
      ok = 0;
  }
  if (ok)
    ok = fmt_source_append_str(
             &source, &len, &cap,
             "]\ndef rendered = render_analyze_json(counts, rows)\n"
             "def output_path = ") &&
         fmt_source_append_quoted(&source, &len, &cap, output_path) &&
         fmt_source_append_str(&source, &len, &cap,
                               "\nmatch file_write(output_path, rendered) { "
                               "ok(_) -> 0 err(_) -> exit(2) }\n");
  if (ok)
    ok = nytrix_embed_run_source(source, NULL) == 0;
  free(source);
  char *rendered = ok ? ny_read_file_raw(output_path, NULL) : NULL;
  fmt_remove_payload(output_path);
  if (!rendered)
    return 0;
  fputs(rendered, stdout);
  free(rendered);
  return 1;
}

static int fmt_render_key_values_embedded(const char *const *keys,
                                          const char *const *values,
                                          size_t count, int width) {
  if (!keys || !values || count == 0 || count > 128 || width < 1 ||
      width > 1000)
    return 0;
  char output_path[PATH_MAX] = {0};
  if (!fmt_payload_path(output_path, sizeof(output_path)))
    return 0;
  char *source = NULL;
  size_t len = 0, cap = 0;
  int ok = fmt_source_append_str(
      &source, &len, &cap,
      "use core\n"
      "use core.compiler.tools.fmt.report (render_key_values)\n"
      "use core.os (file_write, exit)\n"
      "def labels = [");
  for (size_t i = 0; ok && i < count; ++i) {
    if (i)
      ok = fmt_source_append_str(&source, &len, &cap, ",");
    if (ok)
      ok = fmt_source_append_quoted(&source, &len, &cap, keys[i]);
  }
  if (ok)
    ok = fmt_source_append_str(&source, &len, &cap, "]\ndef values = [");
  for (size_t i = 0; ok && i < count; ++i) {
    if (i)
      ok = fmt_source_append_str(&source, &len, &cap, ",");
    if (ok)
      ok = fmt_source_append_quoted(&source, &len, &cap, values[i]);
  }
  if (ok)
    ok = fmt_source_append_str(&source, &len, &cap, "]\ndef colors = [");
  const char *colors[] = {nyt_clr(NYT_BOLD), nyt_clr(NYT_GRAY),
                          nyt_clr(NYT_RESET)};
  for (size_t i = 0; ok && i < sizeof(colors) / sizeof(colors[0]); ++i) {
    if (i)
      ok = fmt_source_append_str(&source, &len, &cap, ",");
    if (ok)
      ok = fmt_source_append_quoted(&source, &len, &cap, colors[i]);
  }
  char width_text[32];
  snprintf(width_text, sizeof(width_text), "%d", width);
  if (ok)
    ok = fmt_source_append_str(
             &source, &len, &cap,
             "]\ndef rendered = render_key_values(labels, values, ") &&
         fmt_source_append_str(&source, &len, &cap, width_text) &&
         fmt_source_append_str(&source, &len, &cap,
                               ", colors)\ndef output_path = ") &&
         fmt_source_append_quoted(&source, &len, &cap, output_path) &&
         fmt_source_append_str(&source, &len, &cap,
                               "\nmatch file_write(output_path, rendered) { "
                               "ok(_) -> 0 err(_) -> exit(2) }\n");
  if (ok)
    ok = nytrix_embed_run_source(source, NULL) == 0;
  free(source);
  char *rendered = ok ? ny_read_file_raw(output_path, NULL) : NULL;
  fmt_remove_payload(output_path);
  if (!rendered)
    return 0;
  fputs(rendered, stdout);
  free(rendered);
  return 1;
}

static int fmt_render_audit_json_embedded(const AuditStats *stats,
                                          FilePressureVec *files,
                                          FunctionPressureVec *functions,
                                          IssueVec *issues, int limit,
                                          const char *min_sev) {
  if (!stats || !files || !functions || !issues || files->len > 10000 ||
      functions->len > 100000 || issues->len > 10000 || limit < 0 ||
      limit > 100000)
    return 0;
  char output_path[PATH_MAX] = {0};
  if (!fmt_payload_path(output_path, sizeof(output_path)))
    return 0;
  char *source = NULL;
  size_t len = 0, cap = 0;
  int ok = fmt_source_append_str(
      &source, &len, &cap,
      "use core\n"
      "use core.compiler.tools.fmt.report (render_audit_json)\n"
      "use core.os (file_write, exit)\n"
      "def counts = [");
  const int counts[] = {stats->files,
                        stats->findings,
                        stats->hot_files,
                        stats->hot_functions,
                        stats->repeated_lines,
                        stats->repeated_calls,
                        stats->trim_targets,
                        stats->bug_findings,
                        stats->legacy_calls,
                        stats->method_syntax,
                        stats->receiver_rewrites,
                        stats->untyped_params,
                        stats->missing_returns,
                        stats->type_suggestions,
                        stats->append_builders,
                        stats->literal_tables,
                        stats->repeated_get_shapes,
                        stats->trivial_main_wrappers,
                        stats->accepted_findings};
  for (size_t i = 0; ok && i < sizeof(counts) / sizeof(counts[0]); ++i) {
    char number[32];
    snprintf(number, sizeof(number), "%s%d", i ? "," : "", counts[i]);
    ok = fmt_source_append_str(&source, &len, &cap, number);
  }
  if (ok)
    ok = fmt_source_append_str(&source, &len, &cap, "]\ndef file_rows = [");
  for (size_t i = 0; ok && i < files->len; ++i) {
    const FilePressure *f = &files->items[i];
    if (i)
      ok = fmt_source_append_str(&source, &len, &cap, ",");
    char values[8][32];
    snprintf(values[0], sizeof(values[0]), "%d", f->score);
    snprintf(values[1], sizeof(values[1]), "%d", f->lines);
    snprintf(values[2], sizeof(values[2]), "%d", f->bytes);
    snprintf(values[3], sizeof(values[3]), "%d", f->dict_gets);
    snprintf(values[4], sizeof(values[4]), "%d", f->legacy_calls);
    snprintf(values[5], sizeof(values[5]), "%d", f->untyped_params);
    snprintf(values[6], sizeof(values[6]), "%d", f->missing_returns);
    snprintf(values[7], sizeof(values[7]), "%d", f->repeated_lines);
    if (ok)
      ok = fmt_source_append_str(&source, &len, &cap, "[") &&
           fmt_source_append_quoted(&source, &len, &cap, f->file);
    for (size_t j = 0; ok && j < 8; ++j) {
      ok = fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, values[j]);
    }
    if (ok)
      ok = fmt_source_append_str(&source, &len, &cap, "]");
    if (len > 32u * 1024u * 1024u)
      ok = 0;
  }
  if (ok)
    ok = fmt_source_append_str(&source, &len, &cap, "]\ndef function_rows = [");
  for (size_t i = 0; ok && i < functions->len; ++i) {
    const FunctionPressure *f = &functions->items[i];
    if (i)
      ok = fmt_source_append_str(&source, &len, &cap, ",");
    char values[7][32];
    snprintf(values[0], sizeof(values[0]), "%d", f->line);
    snprintf(values[1], sizeof(values[1]), "%d", f->score);
    snprintf(values[2], sizeof(values[2]), "%d", f->lines);
    snprintf(values[3], sizeof(values[3]), "%d", f->dict_gets);
    snprintf(values[4], sizeof(values[4]), "%d", f->legacy_calls);
    snprintf(values[5], sizeof(values[5]), "%d", f->untyped_params);
    snprintf(values[6], sizeof(values[6]), "%d", f->missing_returns);
    if (ok)
      ok = fmt_source_append_str(&source, &len, &cap, "[") &&
           fmt_source_append_quoted(&source, &len, &cap, f->file) &&
           fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, f->name);
    for (size_t j = 0; ok && j < 7; ++j) {
      ok = fmt_source_append_str(&source, &len, &cap, ",") &&
           fmt_source_append_quoted(&source, &len, &cap, values[j]);
    }
    if (ok)
      ok = fmt_source_append_str(&source, &len, &cap, "]");
    if (len > 32u * 1024u * 1024u)
      ok = 0;
  }
  if (ok)
    ok = fmt_source_append_str(&source, &len, &cap, "]\ndef issue_rows = [");
  size_t visible = 0;
  for (size_t i = 0; ok && i < issues->len; ++i) {
    const Issue *it = &issues->items[i];
    if (!issue_visible_for_min(it, min_sev))
      continue;
    if (visible++)
      ok = fmt_source_append_str(&source, &len, &cap, ",");
    char id[512], line[32], col[32];
    audit_issue_id(it, id, sizeof(id));
    snprintf(line, sizeof(line), "%d", it->line);
    snprintf(col, sizeof(col), "%d", it->col);
    const char *fields[] = {id,
                            it->code,
                            it->sev,
                            it->file,
                            line,
                            col,
                            it->msg,
                            audit_rewrite_shape_for_code(it->code),
                            audit_syntax_hint_for_code(it->code),
                            it->file,
                            it->msg,
                            it->note};
    if (ok)
      ok = fmt_source_append_str(&source, &len, &cap, "[");
    for (size_t j = 0; ok && j < sizeof(fields) / sizeof(fields[0]); ++j) {
      if (j)
        ok = fmt_source_append_str(&source, &len, &cap, ",");
      if (ok)
        ok = fmt_source_append_quoted(&source, &len, &cap, fields[j]);
    }
    if (ok)
      ok = fmt_source_append_str(&source, &len, &cap, "]");
    if (len > 32u * 1024u * 1024u)
      ok = 0;
  }
  char limit_text[32];
  snprintf(limit_text, sizeof(limit_text), "%d", limit);
  if (ok)
    ok = fmt_source_append_str(&source, &len, &cap,
                               "]\ndef rendered = render_audit_json(counts, "
                               "file_rows, function_rows, issue_rows, ") &&
         fmt_source_append_str(&source, &len, &cap, limit_text) &&
         fmt_source_append_str(&source, &len, &cap, ")\ndef output_path = ") &&
         fmt_source_append_quoted(&source, &len, &cap, output_path) &&
         fmt_source_append_str(&source, &len, &cap,
                               "\nmatch file_write(output_path, rendered) { "
                               "ok(_) -> 0 err(_) -> exit(2) }\n");
  if (ok)
    ok = nytrix_embed_run_source(source, NULL) == 0;
  free(source);
  char *rendered = ok ? ny_read_file_raw(output_path, NULL) : NULL;
  fmt_remove_payload(output_path);
  if (!rendered)
    return 0;
  fputs(rendered, stdout);
  free(rendered);
  return 1;
}
#endif

static int fmt_collect_selftest(void) {
  const char *roots[] = {"lib"};
  StrVec files = {0};
  if (!fmt_collect_via_library(roots, 1, "ny", &files) || files.len == 0) {
    fprintf(stderr, "ny-fmt selftest: Ny file collection failed\n");
    sv_free(&files);
    return 1;
  }
  int found_probe = 0;
  for (size_t i = 0; i < files.len; ++i) {
    if (!is_ny(files.items[i])) {
      fprintf(stderr, "ny-fmt selftest: collector returned non-Ny path: %s\n",
              files.items[i]);
      sv_free(&files);
      return 1;
    }
    if (strcmp(files.items[i], "lib/compiler/tools/doc/markdown.ny") == 0)
      found_probe = 1;
  }
  sv_free(&files);
  if (!found_probe) {
    fprintf(stderr, "ny-fmt selftest: collector omitted its library source\n");
    return 1;
  }
  const char *c_roots[] = {"src/cmd/fmt/init.c"};
  if (!fmt_collect_via_library(c_roots, 1, "c", &files) || files.len != 1 ||
      strcmp(files.items[0], c_roots[0]) != 0) {
    fprintf(stderr, "ny-fmt selftest: C/H file collection failed\n");
    sv_free(&files);
    return 1;
  }
  sv_free(&files);
  const char *cloc_roots[] = {"lib/compiler/tools/doc/markdown.ny"};
  if (!fmt_collect_via_library(cloc_roots, 1, "cloc", &files) ||
      files.len != 1 || strcmp(files.items[0], cloc_roots[0]) != 0) {
    fprintf(stderr, "ny-fmt selftest: cloc file collection failed\n");
    sv_free(&files);
    return 1;
  }
  sv_free(&files);
  const char *code_roots[] = {"lib/compiler/tools/doc/markdown.ny"};
  if (!fmt_collect_via_library(code_roots, 1, "code", &files) ||
      files.len != 1 || strcmp(files.items[0], code_roots[0]) != 0) {
    fprintf(stderr, "ny-fmt selftest: code file collection failed\n");
    sv_free(&files);
    return 1;
  }
  sv_free(&files);
  const char *fmt_roots[] = {"docs/CONTRIBUTE.md"};
  if (!fmt_collect_via_library(fmt_roots, 1, "fmt", &files) || files.len != 1 ||
      strcmp(files.items[0], fmt_roots[0]) != 0) {
    fprintf(stderr, "ny-fmt selftest: formatter file collection failed\n");
    sv_free(&files);
    return 1;
  }
  sv_free(&files);
  return 0;
}

static int run_check_mode(const FmtOpts *opts) {
  StrVec files = {0};
  const char **roots = NULL;
  size_t root_count = opts->paths.len ? opts->paths.len : 2;
  roots = (const char **)calloc(root_count, sizeof(*roots));
  if (roots) {
    if (opts->paths.len) {
      for (size_t i = 0; i < root_count; ++i)
        roots[i] = opts->paths.items[i];
    } else {
      roots[0] = "lib";
      roots[1] = "etc/tests";
    }
  }
  int collected =
      roots && fmt_collect_via_library(roots, root_count, "ny", &files);
  free(roots);
  if (!collected) {
    sv_free(&files);
    nyt_err("ny-fmt", "Ny source collection failed; check aborted");
    return 1;
  }

  size_t check_count = 0;
  for (size_t i = 0; i < files.len; i++) {
    if (!is_expected_error_fixture(files.items[i]))
      check_count++;
  }
  nyt_msg("CHECK", NYT_CYAN, "scanning %zu files for parse bugs", check_count);

  int failed = 0;
  for (size_t i = 0; i < files.len; i++) {
    if (is_expected_error_fixture(files.items[i]))
      continue;
    int issue = 0;
    brace_check_file(files.items[i], opts->fix, opts->verbose, &issue);
    if (issue)
      failed++;
  }
  if (failed == 0)
    nyt_msg("OK", NYT_GREEN, "check complete: all %zu files OK", check_count);
  else
    nyt_msg("CHECK", NYT_RED, "%d file(s) with issues", failed);
  sv_free(&files);
  return failed != 0;
}

static int run_align_macros_mode(const FmtOpts *opts) {
  StrVec files = {0};
  if (opts->paths.len == 0) {
    const char *roots[] = {"src"};
    if (!fmt_collect_via_library(roots, 1, "c", &files)) {
      nyt_err("ny-fmt", "Ny source collection failed; macro alignment aborted");
      sv_free(&files);
      return 1;
    }
  } else {
    if (!fmt_collect_via_library((const char *const *)opts->paths.items,
                                 opts->paths.len, "c", &files)) {
      nyt_err("ny-fmt", "Ny source collection failed; macro alignment aborted");
      sv_free(&files);
      return 1;
    }
  }
  int changed = 0;
  for (size_t i = 0; i < files.len; i++) {
    size_t n = 0;
    char *src = ny_read_file_raw(files.items[i], &n);
    if (!src)
      continue;
    char *dst = malloc(n * 2 + 1);
    if (!dst) {
      free(src);
      continue;
    }
    size_t di = 0, si = 0;
    int block_changed = 0;
    while (si < n) {
      const char *line_start = src + si;
      const char *nl = memchr(line_start, '\n', n - si);
      size_t line_len = nl ? (size_t)(nl - line_start) : n - si;
      const char *trimmed = line_start;
      while (trimmed < line_start + line_len &&
             (*trimmed == ' ' || *trimmed == '\t'))
        trimmed++;
      if (strncmp(trimmed, "#define ", 8) == 0 && nl && si + line_len + 1 < n) {

        const char *next = src + si + line_len + 1;
        const char *next_trim = next;
        while (next_trim < src + n && (*next_trim == ' ' || *next_trim == '\t'))
          next_trim++;
        const char *next_nl = memchr(next, '\n', n - (next - src));

        const char *bs =
            memchr(next, '\\',
                   next_nl ? (size_t)(next_nl - next) : (n - (next - src)));
        if (bs && strncmp(next_trim, "do {", 4) == 0) {

          memmove(dst + di, line_start, line_len);
          di += line_len;
          si += line_len + 1;
          if (dst[di - 1] != '\n')
            dst[di++] = '\n';

          while (si < n) {
            const char *bl = src + si;
            const char *bnl = memchr(bl, '\n', n - si);
            size_t bl_len = bnl ? (size_t)(bnl - bl) : n - si;
            memmove(dst + di, bl, bl_len);
            di += bl_len;
            si += bl_len + 1;
            if (!bnl)
              break;

            if (strstr(bl, "while (0)") || strstr(bl, "while(0)")) {
              if (dst[di - 1] != '\n')
                dst[di++] = '\n';
              break;
            }
          }
          continue;
        }
      }

      memmove(dst + di, line_start, line_len);
      di += line_len;
      si += line_len + 1;
      if (si < n && dst[di - 1] != '\n')
        dst[di++] = '\n';
    }
    dst[di] = '\0';
    if (block_changed) {
      if (ny_write_file(files.items[i], dst, di) == 0)
        changed++;
    }
    free(dst);
    free(src);
  }
  nyt_msg("ALIGN", changed ? NYT_GREEN : NYT_GRAY,
          "aligned macros in %d file(s)", changed);
  sv_free(&files);
  return 0;
}

int ny_fmt_main(int argc, char **argv) {
  char root[PATH_MAX];
  if (!ensure_repo_root(root, sizeof(root))) {
    nyt_err("ny-fmt", "could not locate repository root");
    return 1;
  }
  if (chdir(root) != 0) {
    nyt_err("ny-fmt", "failed to chdir to root: %s", root);
    return 1;
  }

  FmtOpts opts;
  int ps = parse_args(argc, argv, &opts);
  if (ps == 0) {
    sv_free(&opts.paths);
    return 2;
  }
  if (ps == 1) {
    sv_free(&opts.paths);
    return 0;
  }
  if (opts.json)
    ny_setenv("NYTRIX_TOOL_COLOR", "never", 1);
  else if (opts.color == 1)
    ny_setenv("NYTRIX_TOOL_COLOR", "always", 1);
  else if (opts.color == 0)
    ny_setenv("NYTRIX_TOOL_COLOR", "never", 1);
  else if (opts.color == -1)
    ny_setenv("NYTRIX_TOOL_COLOR", "auto", 1);
  if (opts.limit < 0)
    opts.limit = 0;

  if (opts.route == FMT_ROUTE_SELFTEST) {
    sv_free(&opts.paths);
    int rc = cscan_selftest();
    if (rc != 0)
      return rc;
    rc = ny_fmt_ny_selftest();
    if (rc != 0)
      return rc;
    rc = fmt_collect_selftest();
    if (rc != 0)
      return rc;
    rc = c2ny_selftest();
    if (rc != 0)
      return rc;
    return py2ny_selftest();
  }

  if (opts.route == FMT_ROUTE_C2NY) {
    const char *in = opts.paths.len > 0 ? opts.paths.items[0] : NULL;
    const char *out = opts.conv_output ? opts.conv_output : "out.ny";
    if (!in) {
      nyt_err("ny-fmt", "--c2ny requires an input C file");
      sv_free(&opts.paths);
      return 2;
    }
    int rc = run_c2ny(in, out);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.route == FMT_ROUTE_PY2NY) {
    const char *in = opts.paths.len > 0 ? opts.paths.items[0] : NULL;
    const char *out = opts.conv_output ? opts.conv_output : "out.ny";
    if (!in) {
      nyt_err("ny-fmt", "--py2ny requires an input Python file");
      sv_free(&opts.paths);
      return 2;
    }
    int rc = run_py2ny(in, out);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.route == FMT_ROUTE_ALIGN) {
    int rc = run_align_macros_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.route == FMT_ROUTE_CONV) {
    int rc = run_conv(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.route == FMT_ROUTE_CLOC) {
    int rc = run_cloc_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.route == FMT_ROUTE_DUPES) {
    int rc = run_dupes_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.route == FMT_ROUTE_MIGRATE_OPERATORS) {
    int rc = run_migrate_operators_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.plan & FMT_PLAN_READ_ONLY_NOTICE) {
    nyt_msg("FMT", NYT_GRAY, "read-only; pass --fix to rewrite files");
  }

  if (opts.plan & FMT_PLAN_REWRITE) {
    StrVec files = {0};
    if (opts.paths.len == 0) {
      const char *roots[] = {"src", "lib", "etc/tests"};
      if (!fmt_collect_via_library(roots, 3, "fmt", &files)) {
        nyt_err("ny-fmt", "Ny source collection failed; formatting aborted");
        sv_free(&files);
        sv_free(&opts.paths);
        return 1;
      }
    } else {
      if (!fmt_collect_via_library((const char *const *)opts.paths.items,
                                   opts.paths.len, "fmt", &files)) {
        nyt_err("ny-fmt", "Ny source collection failed; formatting aborted");
        sv_free(&files);
        sv_free(&opts.paths);
        return 1;
      }
    }
    char **ny_formatted = NULL;
    char **ny_original = NULL;
    size_t ny_count = 0;
    if (!fmt_source_batch_c(&files, &ny_formatted, &ny_original, &ny_count)) {
      nyt_err("ny-fmt", "Ny formatter pass failed; formatting aborted");
      sv_free(&files);
      sv_free(&opts.paths);
      return 1;
    }
    int changed = 0;
    size_t ny_index = 0;
    for (size_t i = 0; i < files.len; i++) {
      int chg = 0;
      if (is_ny(files.items[i]) || is_c_source(files.items[i]) ||
          is_c_tidy_source(files.items[i])) {
        char *before = ny_index < ny_count ? ny_original[ny_index] : NULL;
        if (!before) {
          for (size_t j = 0; j < ny_count; ++j)
            free(ny_formatted[j]);
          for (size_t j = 0; j < ny_count; ++j)
            free(ny_original[j]);
          free(ny_formatted);
          free(ny_original);
          nyt_err("ny-fmt", "Ny formatter result mismatch; formatting aborted");
          sv_free(&files);
          sv_free(&opts.paths);
          return 1;
        }
        if (strcmp(before, ny_formatted[ny_index]) != 0) {
          if (ny_write_file(files.items[i], ny_formatted[ny_index],
                            strlen(ny_formatted[ny_index])) != 0) {
            for (size_t j = 0; j < ny_count; ++j)
              free(ny_formatted[j]);
            for (size_t j = 0; j < ny_count; ++j)
              free(ny_original[j]);
            free(ny_formatted);
            free(ny_original);
            nyt_err("ny-fmt", "could not write formatted file: %s",
                    files.items[i]);
            sv_free(&files);
            sv_free(&opts.paths);
            return 1;
          }
          chg = 1;
        }
        ++ny_index;
        if (chg)
          changed++;
        continue;
      }
      if (format_file(files.items[i], &chg) && chg)
        changed++;
    }
    for (size_t i = 0; i < ny_count; ++i)
      free(ny_formatted[i]);
    for (size_t i = 0; i < ny_count; ++i)
      free(ny_original[i]);
    free(ny_formatted);
    free(ny_original);
    nyt_msg("FMT", changed ? NYT_GREEN : NYT_GRAY,
            "complete (%d files updated)", changed);
    sv_free(&files);
  }

  int command_rc = 0;
  if (opts.plan & FMT_PLAN_CHECK)
    command_rc = run_check_mode(&opts);

  if (opts.plan & FMT_PLAN_ANALYZE)
    if (run_analyze_simple(&opts.paths, opts.json, opts.limit) != 0)
      command_rc = 1;

  int audit_rc = 0;
  if (opts.plan & FMT_PLAN_AUDIT)
    audit_rc = run_audit_simple(&opts.paths, opts.audit_mode, opts.json,
                                opts.limit, opts.min_sev, opts.types_strict);
  if (audit_rc != 0)
    command_rc = 1;

  if (opts.plan & FMT_PLAN_OPTIMIZE) {
    StrVec files = {0};
    if (opts.paths.len == 0) {
      const char *roots[] = {"src", "lib", "etc/tests"};
      if (!fmt_collect_via_library(roots, 3, "ny", &files)) {
        nyt_err("ny-fmt", "Ny source collection failed; optimization aborted");
        sv_free(&files);
        sv_free(&opts.paths);
        return 1;
      }
    } else {
      if (!fmt_collect_via_library((const char *const *)opts.paths.items,
                                   opts.paths.len, "ny", &files)) {
        nyt_err("ny-fmt", "Ny source collection failed; optimization aborted");
        sv_free(&files);
        sv_free(&opts.paths);
        return 1;
      }
    }
    char **ny_formatted = NULL;
    char **ny_original = NULL;
    size_t ny_count = 0;
    if (!fmt_source_batch_c(&files, &ny_formatted, &ny_original, &ny_count)) {
      nyt_err("ny-fmt", "Ny formatter pass failed; optimization aborted");
      sv_free(&files);
      sv_free(&opts.paths);
      return 1;
    }
    size_t ny_index = 0;
    int changed = 0;
    int optimize_ok = 1;
    for (size_t i = 0; i < files.len; i++) {
      int chg = 0;
      int ok = 1;
      if (is_ny(files.items[i])) {
        char *before = ny_index < ny_count ? ny_original[ny_index] : NULL;
        if (!before) {
          optimize_ok = 0;
          break;
        }
        const char *after = ny_formatted[ny_index++];
        if (strcmp(before, after) != 0) {
          if (opts.diff)
            fmt_emit_unified_diff(files.items[i], before, after);
          if (opts.apply &&
              ny_write_file(files.items[i], after, strlen(after)) != 0) {
            optimize_ok = 0;
            break;
          }
          chg = 1;
        }
      } else {
        ok = opts.diff ? format_file_diff(files.items[i], &chg, opts.apply)
                       : format_file(files.items[i], &chg);
      }
      if (ok) {
        if (chg)
          changed++;
      } else {
        optimize_ok = 0;
        break;
      }
    }
    for (size_t i = 0; i < ny_count; ++i)
      free(ny_formatted[i]);
    for (size_t i = 0; i < ny_count; ++i)
      free(ny_original[i]);
    free(ny_formatted);
    free(ny_original);
    nyt_msg("OPT", NYT_GREEN, "%s %d file(s)",
            opts.diff && !opts.apply ? "previewed" : "applied updates to",
            changed);
    sv_free(&files);
    if (!optimize_ok) {
      nyt_err("ny-fmt",
              "optimization rewrite failed; stopped before completion");
      sv_free(&opts.paths);
      return 1;
    }
  }

  sv_free(&opts.paths);
  return command_rc;
}
