#include "base/loader.h"
#include "base/options.h"
#include "code/jit.h"
#include "rt/shared.h"
#include "wire/pipe.h"
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void handle_segv(int sig) {
  void *bt[64];
  int n = backtrace(bt, 64);
  fprintf(stderr, "\033[1;31mCaught signal %d, backtrace:\033[0m\n", sig);
  backtrace_symbols_fd(bt, n, STDERR_FILENO);
  exit(128 + sig);
}

int main(int argc, char **argv, char **envp) {
  signal(SIGSEGV, handle_segv);

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
