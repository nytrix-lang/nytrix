#ifndef NY_OPTIONS_H
#define NY_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  NY_MODE_RUN,
  NY_MODE_REPL,
  NY_MODE_BUILD,
  NY_MODE_VERSION,
  NY_MODE_HELP
} ny_mode;

#include "base/loader.h"

typedef struct {
  ny_mode mode;
  const char *input_file;
  const char *output_file;
  const char *command_string;
  const char *argv0;

  // Config
  int opt_level;
  const char *opt_pipeline;
  const char *emit_ir_path;
  const char *emit_asm_path;

  // Flags
  int verbose;
  bool run_jit;
  bool emit_only;
  bool do_timing;
  bool dump_ast;
  bool dump_llvm;
  bool dump_tokens;
  bool dump_docs;
  bool dump_funcs;
  bool dump_symbols;
  bool dump_stats;
  bool dump_on_error;
  bool verify_module;
  bool trace_exec;
  bool safe_mode;
  bool debug_symbols;
  bool no_std;
  const char *std_path;
  std_mode_t std_mode;
  int strip_override; // -1 default, 0 keep, 1 strip
  int color_mode;     // -1 auto, 0 never, 1 always

  bool repl_plain;
  VEC(char *) link_dirs;
  VEC(char *) link_libs;

  // Runtime args
  char **args;
  int argc;
  int file_arg_idx; // Index in argv where script args start
} ny_options;

void ny_options_init(ny_options *opt);
void ny_options_parse(ny_options *opt, int argc, char **argv);
void ny_options_usage(const char *argv0);

#endif
