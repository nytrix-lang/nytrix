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
  bool ir_include_std;

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
  bool help_env;
  bool debug_symbols;
  bool no_std;
  bool effect_require_known;
  bool alias_require_known;
  bool alias_require_no_escape;
  int gprof; // -1 default/env, 0 off, 1 on
  int std_builtin_ops; // -1 default/env, 0 off, 1 on
  int debug_locals; // -1 default/env, 0 off, 1 on
  int dwarf_version; // 0 default/env, otherwise 2..5
  int dwarf_split_inlining; // -1 default/env, 0 off, 1 on
  int dwarf_profile_info; // -1 default/env, 0 off, 1 on
  const char *host_cflags;
  const char *host_ldflags;
  const char *host_triple;
  const char *arm_float_abi;
  const char *std_path;
  std_mode_t std_mode;
  int strip_override; // -1 default, 0 keep, 1 strip
  int color_mode;     // -1 auto, 0 never, 1 always
  
  // Optimization controls (-1 = default/env, 0 = off, 1 = on)
  int opt_dce;            // Dead code elimination
  int opt_internalize;    // Symbol internalization  
  int opt_loops;          // Loop vectorization/unrolling
  int opt_autotune;       // Adaptive O1/O2 based on IR size

  bool repl_plain;
  const char *gpu_mode;      // off|auto|opencl
  const char *gpu_backend;   // none|auto|opencl|cuda|hip|metal
  const char *gpu_offload;   // off|auto|on|force
  int gpu_min_work;          // 0 = runtime default/auto
  int gpu_async;             // -1 default, 0 off, 1 on
  int gpu_fast_math;         // -1 default, 0 off, 1 on
  const char *accel_target;  // auto|none|nvptx|amdgpu|spirv|hsaco (+aliases)
  const char *accel_object;  // auto|none|ptx|o|spv|hsaco
  const char *parallel_mode; // off|auto|threads
  int thread_count;          // 0 = runtime default/auto
  int parallel_min_work;     // 0 = runtime default/auto
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
void ny_options_usage_env(const char *argv0);

#endif
