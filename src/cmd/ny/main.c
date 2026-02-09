#include "base/common.h"
#include "base/loader.h"
#include "base/options.h"
#include "base/util.h"
#include "code/jit.h"
#include "rt/shared.h"
#include "wire/pipe.h"
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int64_t __trace_dump(int64_t n);

static void print_trace_snippet(const char *file, int line, int col) {
  if (!file || file[0] == '<' || line <= 0 || col <= 0)
    return;
  char *src = ny_read_file(file);
  if (!src)
    return;
  const char *cur = src;
  int cur_line = 1;
  while (*cur && cur_line < line) {
    if (*cur == '\n')
      cur_line++;
    cur++;
  }
  if (cur_line != line) {
    free(src);
    return;
  }
  const char *line_start = cur;
  while (*cur && *cur != '\n')
    cur++;
  size_t line_len = (size_t)(cur - line_start);
  if (line_len == 0) {
    free(src);
    return;
  }
  size_t caret_col = (size_t)(col - 1);
  if (caret_col > line_len)
    caret_col = line_len;
  size_t caret_len = 1;
  const size_t max_len = 200;
  size_t start = 0;
  size_t end = line_len;
  bool prefix = false, suffix = false;
  if (line_len > max_len) {
    if (caret_col > max_len / 2)
      start = caret_col - max_len / 2;
    if (start + max_len > line_len)
      start = line_len - max_len;
    end = start + max_len;
    prefix = start > 0;
    suffix = end < line_len;
  }
  size_t show_len = end - start;
  char *buf = malloc(show_len + 1);
  if (!buf) {
    free(src);
    return;
  }
  for (size_t i = 0; i < show_len; i++) {
    char c = line_start[start + i];
    buf[i] = (c == '\t') ? ' ' : c;
  }
  buf[show_len] = '\0';
  int width = 1;
  for (int tmp = line; tmp >= 10; tmp /= 10)
    width++;
  fprintf(stderr, "  %s%*d%s | %s%s%s\n", clr(NY_CLR_GRAY), width, line,
          clr(NY_CLR_RESET), prefix ? "..." : "", buf, suffix ? "..." : "");
  fprintf(stderr, "  %s%*s%s | ", clr(NY_CLR_GRAY), width, "",
          clr(NY_CLR_RESET));
  size_t caret_pad = caret_col - start + (prefix ? 3 : 0);
  for (size_t i = 0; i < caret_pad; i++)
    fputc(' ', stderr);
  fputs(clr(NY_CLR_RED), stderr);
  for (size_t i = 0; i < caret_len; i++)
    fputc('^', stderr);
  fputs(clr(NY_CLR_RESET), stderr);
  fputc('\n', stderr);
  free(buf);
  free(src);
}

static void print_last_trace(void) {
  int64_t file = __trace_last_file();
  if (!is_v_str(file))
    return;
  const char *fname = (const char *)(uintptr_t)file;
  int64_t ftagged = *(int64_t *)((char *)(uintptr_t)file - 16);
  size_t flen = is_int(ftagged) ? (size_t)(ftagged >> 1) : 0;
  int64_t tline = __trace_last_line();
  int64_t tcol = __trace_last_col();
  int64_t line = is_int(tline) ? rt_untag_v(tline) : 0;
  int64_t col = is_int(tcol) ? rt_untag_v(tcol) : 0;
  fprintf(stderr, "%sLast Nytrix location:%s %.*s:%ld:%ld", clr(NY_CLR_CYAN),
          clr(NY_CLR_RESET), (int)flen, fname, (long)line, (long)col);
  int64_t fn = __trace_last_func();
  if (is_v_str(fn)) {
    const char *fnname = (const char *)(uintptr_t)fn;
    int64_t fntagged = *(int64_t *)((char *)(uintptr_t)fn - 16);
    size_t fnlen = is_int(fntagged) ? (size_t)(fntagged >> 1) : 0;
    fprintf(stderr, " (fn %.*s)", (int)fnlen, fnname);
  }
  fputc('\n', stderr);
  print_trace_snippet(fname, (int)line, (int)col);
}

static void handle_segv(int sig) {
  void *bt[64];
  int n = backtrace(bt, 64);
  fprintf(stderr, "%sCaught signal %d (%s), backtrace:%s\n",
          clr(NY_CLR_RED), sig, strsignal(sig), clr(NY_CLR_RESET));
  backtrace_symbols_fd(bt, n, STDERR_FILENO);
  print_last_trace();
  fprintf(stderr, "%sRecent Nytrix trace:%s\n", clr(NY_CLR_CYAN),
          clr(NY_CLR_RESET));
  __trace_dump(((int64_t)8 << 1) | 1);
  fprintf(stderr,
          "%sHint:%s re-run with %s-trace -g --dump-on-error --emit-asm=build/"
          "debug/last_asm.s --emit-ir=build/debug/last_ir.ll%s\n",
          clr(NY_CLR_YELLOW), clr(NY_CLR_RESET), clr(NY_CLR_BOLD),
          clr(NY_CLR_RESET));
  exit(128 + sig);
}

int main(int argc, char **argv, char **envp) {
  signal(SIGSEGV, handle_segv);
  signal(SIGABRT, handle_segv);
  signal(SIGFPE, handle_segv);
  signal(SIGILL, handle_segv);

  ny_options opt;
  ny_options_init(&opt);
  ny_options_parse(&opt, argc, argv);

  // Set runtime args (host -> JIT bridge)
  // If we have an input file, script args start there.
  if (opt.input_file) {
    int s_argc = argc - opt.file_arg_idx;
    char **s_argv = &argv[opt.file_arg_idx];
    __set_args((int64_t)s_argc, (int64_t)(uintptr_t)s_argv,
               (int64_t)(uintptr_t)envp);
  } else if (opt.command_string) {
    static char *eval_argv[] = {(char *)"nytrix", NULL};
    __set_args(1, (int64_t)(uintptr_t)eval_argv, (int64_t)(uintptr_t)envp);
  } else {
    // Default to all args (REPL or empty)
    __set_args((int64_t)argc, (int64_t)(uintptr_t)argv,
               (int64_t)(uintptr_t)envp);
  }

  if (argc < 2 && opt.mode != NY_MODE_REPL && !opt.command_string &&
      !opt.input_file) {
    ny_options_usage(argv[0]);
    return 0;
  }

  int exit_code = ny_pipeline_run(&opt);
  // Cleanup
  ny_std_free_modules();
  __cleanup_args();
  return exit_code;
}
