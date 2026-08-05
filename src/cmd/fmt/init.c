#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "fmt.h"
#include "base/args.h"
#include "base/process.h"
#include "base/util.h"
#include "../tools/repo.h"
#include "../tools/tool.h"


#include "core.c"
static void usage(void) {
  nyt_heading("Nytrix Format And Audit");
  printf("%susage:%s %sny fmt%s %s[mode] [options] [paths ...]%s\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET),
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny fmt --cloc%s %s[--full] [--top N] [paths ...]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny fmt --dupes%s %s[--dupes-min N] [--dupes-emit] [--json] [paths ...]%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny fmt --conv%s %s--input file.texi --name NAME [--format man|md] [-o out]%s\n\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("%smodes:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %s--check --fix --analyze --audit --selftest --trim --syntax --types --dead%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  Formatting is read-only by default; pass --fix to rewrite files.\n");
  printf("  %s--smart --overhaul --bugs --checks --bloat --modules --profiles --layouts --loops%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--contracts --ffi --constants --specialize --metaprog --constfold%s\n\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("%soptions:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %s--json --tidy --optimize --apply --diff%s\n", nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--color MODE --limit N --threshold N --root DIR --dirs DIR%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--min-sev CRIT|HIGH|MED|LOW --types-strict -v%s\n", nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("  audit modes compose; use %s--audit=loops,trim%s to find continue/guard-loop flattening wins\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  accept justified smells with %sny-fmt: accept NYAUDxxxx reason%s\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--selftest%s validates C range analysis and high-confidence language-pattern checks\n",
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
      memcpy(dst, cur, tail);
      dst += tail;
      break;
    }
    size_t chunk = (size_t)(p - cur);
    memcpy(dst, cur, chunk);
    dst += chunk;
    memcpy(dst, rep, r_n);
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
    char *tmp = str_replace_all(*s, rules[i].pat, rules[i].rep ? rules[i].rep : "");
    free(*s);
    *s = tmp ? tmp : strdup("");
  }
}

static char *convert_texi_basic(const char *input, const char *name, const char *fmt, const char *section) {
  char *s = strdup(input ? input : "");
  if (!s)
    return NULL;
  const ReplaceRule drops[] = {{"@contents", ""},      {"@appendix", ""},
                               {"@printindex", ""},    {"@node", ""},
                               {"@menu", ""},          {"@dircategory", ""},
                               {"@direntry", ""},      {"@titlepage", ""},
                               {"\\input texinfo", ""}, {"@setfilename", ""},
                               {"@settitle", ""},      {NULL, NULL}};
  replace_rules_owned(&s, drops);
  if (strcmp(fmt, "md") == 0) {
    const ReplaceRule md_rules[] = {{"@chapter ", "# "}, {"@section ", "## "},
                                    {"@subsection ", "### "}, {"@code{", "`"},
                                    {"}", "`"}, {NULL, NULL}};
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
  snprintf(header, sizeof(header), ".TH %s %s \"\" \"\" \"Nytrix\"\n", name ? name : "nytrix",
           section ? section : "1");
  const ReplaceRule man_rules[] = {{"@chapter ", ".SH "}, {"@section ", ".SH "},
                                   {"@subsection ", ".SS "}, {"@code{", "\\fB"},
                                   {"}", "\\fP"}, {NULL, NULL}};
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
  pny_sb_add(&out, "\nuse std.core\n\n");

  int rc = pny_convert(src, n, &out);

  free(src);

  if (rc != 0) {
    nyt_err("ny-fmt", "py2ny: unsupported constructs remain in %s; see markers in output", input_path);
  }

  if (!write_file(output_path, out.data, out.len)) {
    nyt_err("ny-fmt", "py2ny: failed to write %s", output_path);
    free(out.data);
    return 1;
  }

  printf("py2ny: %s -> %s (%zu bytes)\n", input_path, output_path, out.len);
  free(out.data);
  return 0;
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
  cny_sb_add(&out, "\nuse std.core\n\n");

  int rc = cny_convert(src, n, &out);

  free(src);

  if (rc != 0) {
    nyt_err("ny-fmt", "c2ny: unsupported constructs remain in %s; see markers in output", input_path);
  }

  if (!write_file(output_path, out.data, out.len)) {
    nyt_err("ny-fmt", "c2ny: failed to write %s", output_path);
    free(out.data);
    return 1;
  }

  printf("c2ny: %s -> %s (%zu bytes)\n", input_path, output_path, out.len);
  free(out.data);
  return 0;
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
  char *out = convert_texi_basic(src, o->conv_name, fmt, o->conv_section ? o->conv_section : "1");
  free(src);
  if (!out) {
    nyt_err("ny-fmt", "conv: conversion failed");
    return 1;
  }
  int rc = 0;
  if (o->conv_output) {
    if (!write_file(o->conv_output, out, strlen(out))) {
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
    {"audit", "all"},      {"all", "all"},             {"bloat", "bloat"},
    {"modules", "modules"}, {"profiles", "profiles"},   {"batteries", "batteries"},
    {"bugs", "bugs"},      {"bug", "bugs"},             {"correctness", "bugs"},
    {"lint", "bugs"},      {"checks", "bugs"},          {"bugchecks", "bugs"},
    {"sanity", "bugs"},
    {"trim", "trim"},      {"layouts", "layouts"},     {"layout", "layouts"},
    {"contracts", "contracts"}, {"backend-contracts", "contracts"},
    {"specialize", "specialize"}, {"specialization", "specialize"},
    {"constfold", "specialize"}, {"partial", "specialize"},
    {"metaprog", "metaprog"}, {"meta", "metaprog"}, {"roadmap", "metaprog"},
    {"codebase", "metaprog"}, {"features", "metaprog"},
    {"ffi", "ffi"},        {"dead", "dead"},           {"calls", "calls"},
    {"similarities", "calls"}, {"types", "types"},      {"legacy", "legacy"},
    {"methods", "methods"}, {"method-syntax", "methods"}, {"syntax", "methods"},
    {"smart", "smart"},    {"overhaul", "smart"},      {"constants", "constants"},
    {"consts", "constants"},
};

static const char *fmt_audit_mode_for_arg(const char *arg) {
  if (!arg || !*arg)
    return NULL;
  if (arg[0] == '-' && arg[1] == '-')
    arg += 2;
  for (size_t i = 0; i < sizeof(k_fmt_audit_aliases) / sizeof(k_fmt_audit_aliases[0]); i++) {
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

static int parse_args(int argc, char **argv, FmtOpts *o) {
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
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode, err, sizeof(err));
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
    } else if ((strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) && i + 1 < argc) {
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
  return 2;
}

static void run_check_mode(const FmtOpts *opts) {
  StrVec files = {0};
  if (opts->paths.len == 0) {
    collect_files_rec("lib", &files, 1);
    collect_files_rec("etc/tests", &files, 1);
  } else {
    for (size_t i = 0; i < opts->paths.len; i++)
      collect_files_rec(opts->paths.items[i], &files, 1);
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
}

static int run_align_macros_mode(const FmtOpts *opts) {
  StrVec files = {0};
  if (opts->paths.len == 0) {
    collect_c_files_rec("src", &files);
  } else {
    for (size_t i = 0; i < opts->paths.len; i++)
      collect_c_files_rec(opts->paths.items[i], &files);
  }
  int changed = 0;
  for (size_t i = 0; i < files.len; i++) {
    size_t n = 0;
    char *src = ny_read_file_raw(files.items[i], &n);
    if (!src) continue;
    char *dst = malloc(n * 2 + 1);
    if (!dst) { free(src); continue; }
    size_t di = 0, si = 0;
    int block_changed = 0;
    while (si < n) {
      const char *line_start = src + si;
      const char *nl = memchr(line_start, '\n', n - si);
      size_t line_len = nl ? (size_t)(nl - line_start) : n - si;
      const char *trimmed = line_start;
      while (trimmed < line_start + line_len && (*trimmed == ' ' || *trimmed == '\t'))
        trimmed++;
      if (strncmp(trimmed, "#define ", 8) == 0 && nl && si + line_len + 1 < n) {

        const char *next = src + si + line_len + 1;
        const char *next_trim = next;
        while (next_trim < src + n && (*next_trim == ' ' || *next_trim == '\t'))
          next_trim++;
        const char *next_nl = memchr(next, '\n', n - (next - src));

        const char *bs = memchr(next, '\\', next_nl ? (size_t)(next_nl - next) : (n - (next - src)));
        if (bs && strncmp(next_trim, "do {", 4) == 0) {

          memcpy(dst + di, line_start, line_len);
          di += line_len; si += line_len + 1;
          if (dst[di - 1] != '\n') dst[di++] = '\n';

          while (si < n) {
            const char *bl = src + si;
            const char *bnl = memchr(bl, '\n', n - si);
            size_t bl_len = bnl ? (size_t)(bnl - bl) : n - si;
            memcpy(dst + di, bl, bl_len);
            di += bl_len; si += bl_len + 1;
            if (!bnl) break;

            if (strstr(bl, "while (0)") || strstr(bl, "while(0)")) {
              if (dst[di - 1] != '\n') dst[di++] = '\n';
              break;
            }
          }
          continue;
        }
      }

      memcpy(dst + di, line_start, line_len);
      di += line_len;
      si += line_len + 1;
      if (si < n && dst[di - 1] != '\n') dst[di++] = '\n';
    }
    dst[di] = '\0';
    if (block_changed) {
      if (write_file(files.items[i], dst, di))
        changed++;
    }
    free(dst);
    free(src);
  }
  nyt_msg("ALIGN", changed ? NYT_GREEN : NYT_GRAY, "aligned macros in %d file(s)", changed);
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

  if (opts.selftest) {
    sv_free(&opts.paths);
    int rc = cscan_selftest();
    if (rc != 0)
      return rc;
    rc = c2ny_selftest();
    if (rc != 0)
      return rc;
    return py2ny_selftest();
  }

  if (opts.tidy) {
    opts.check = 1;
    opts.analyze = 1;
  }

  if (opts.c2ny) {
    const char *in = opts.paths.len > 0 ? opts.paths.items[0] : NULL;
    const char *out = opts.conv_output ? opts.conv_output : "out.ny";
    if (!in) { nyt_err("ny-fmt", "--c2ny requires an input C file"); sv_free(&opts.paths); return 2; }
    int rc = run_c2ny(in, out);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.py2ny) {
    const char *in = opts.paths.len > 0 ? opts.paths.items[0] : NULL;
    const char *out = opts.conv_output ? opts.conv_output : "out.ny";
    if (!in) { nyt_err("ny-fmt", "--py2ny requires an input Python file"); sv_free(&opts.paths); return 2; }
    int rc = run_py2ny(in, out);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.align_macros) {
    int rc = run_align_macros_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.conv) {
    int rc = run_conv(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.cloc) {
    int rc = run_cloc_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  if (opts.dupes) {
    int rc = run_dupes_mode(&opts);
    sv_free(&opts.paths);
    return rc;
  }

  int rewrite_format = opts.fix || opts.tidy;

  if (!rewrite_format &&
      !(opts.analyze || opts.audit || opts.check || opts.optimize || opts.dupes)) {
    nyt_msg("FMT", NYT_GRAY, "read-only; pass --fix to rewrite files");
  }

  if (rewrite_format) {
    StrVec files = {0};
    if (opts.paths.len == 0) {
      collect_files_rec("src", &files, 0);
      collect_files_rec("lib", &files, 0);
      collect_files_rec("etc/tests", &files, 0);
    } else {
      for (size_t i = 0; i < opts.paths.len; i++)
        collect_files_rec(opts.paths.items[i], &files, 0);
    }
    int changed = 0;
    for (size_t i = 0; i < files.len; i++) {
      int chg = 0;
      if (format_file(files.items[i], &chg) && chg)
        changed++;
    }
    nyt_msg("FMT", changed ? NYT_GREEN : NYT_GRAY, "complete (%d files updated)", changed);
    sv_free(&files);
  }

  if (opts.check || opts.tidy)
    run_check_mode(&opts);

  if (opts.analyze || opts.optimize || opts.tidy)
    run_analyze_simple(&opts.paths, opts.json, opts.limit);

  int audit_rc = 0;
  if (opts.audit)
    audit_rc = run_audit_simple(&opts.paths, opts.audit_mode, opts.json, opts.limit,
                                opts.min_sev, opts.types_strict);

  if (opts.optimize && opts.apply) {
    StrVec files = {0};
    if (opts.paths.len == 0) {
      collect_files_rec("src", &files, 1);
      collect_files_rec("lib", &files, 1);
      collect_files_rec("etc/tests", &files, 1);
    } else {
      for (size_t i = 0; i < opts.paths.len; i++)
        collect_files_rec(opts.paths.items[i], &files, 1);
    }
    int changed = 0;
    for (size_t i = 0; i < files.len; i++) {
      int chg = 0;
      if (format_file(files.items[i], &chg) && chg)
        changed++;
    }
    if (opts.diff)
      nyt_warn("ny-fmt", "optimize --diff is not yet implemented in C mode");
    nyt_msg("OPT", NYT_GREEN, "applied updates to %d file(s)", changed);
    sv_free(&files);
  }

  sv_free(&opts.paths);
  return audit_rc;
}
