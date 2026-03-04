#ifndef NY_OPTIONS_H
#define NY_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  NY_MODE_RUN,
  NY_MODE_REPL,
  NY_MODE_BUILD,
  NY_MODE_VERSION,
  NY_MODE_HELP,
  NY_MODE_BUNDLE
} ny_mode;

#include "base/loader.h"

typedef struct {
  ny_mode mode;
  const char *input_file;
  const char *output_file;
  const char *command_string;
  const char *entry_name;
  const char *argv0;
  int opt_level;
  const char *opt_pipeline;
  const char *emit_ir_pre_path;
  const char *emit_ir_path;
  const char *emit_bc_path;
  const char *emit_asm_path;
  const char *emit_module;
  bool ir_include_std;
  int verbose;
  bool run_jit;
  bool run_aot;
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
  bool help_env;
  bool debug_symbols;
  bool no_std;
  bool effect_require_known;
  bool alias_require_known;
  bool alias_require_no_escape;
  int gprof;
  int std_builtin_ops;
  int debug_locals;
  int dwarf_version;
  int dwarf_split_inlining;
  int dwarf_profile_info;
  const char *host_cflags;
  const char *host_ldflags;
  const char *host_triple;
  const char *arm_float_abi;
  const char *std_path;
  const char *bundle_std_path;
  const char *bundle_symbols_path;
  std_mode_t std_mode;
  bool std_mode_explicit;
  int strip_override;
  int color_mode;
  int opt_dce;
  int opt_internalize;
  int opt_loops;
  int opt_autotune;
  bool opt_level_explicit;
  bool repl_plain;
  const char *gpu_mode;
  const char *gpu_backend;
  const char *gpu_offload;
  int gpu_min_work;
  int gpu_async;
  int gpu_fast_math;
  const char *accel_target;
  const char *accel_object;
  const char *parallel_mode;
  int thread_count;
  int parallel_min_work;
  VEC(char *) link_dirs;
  VEC(char *) link_libs;
  char **args;
  int argc;
  int file_arg_idx;
} ny_options;

void ny_options_init(ny_options *opt);
void ny_options_parse(ny_options *opt, int argc, char **argv);
void ny_options_free(ny_options *opt);
void ny_options_usage(const char *argv0);
void ny_options_usage_env(const char *argv0);

#endif
