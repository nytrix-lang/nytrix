#ifndef NY_OPTIONS_H
#define NY_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  NY_MODE_RUN,
  NY_MODE_REPL,
  NY_MODE_BUILD,
  NY_MODE_VERSION,
  NY_MODE_HELP,
  NY_MODE_BUNDLE,
  NY_MODE_CLEAN_CACHE
} ny_mode;

typedef enum {
  NY_DUMP_SCOPE_PROGRAM = 0,
  NY_DUMP_SCOPE_LIB = 1,
  NY_DUMP_SCOPE_BOTH = 2
} ny_dump_scope_t;

typedef enum {
  NY_HEAP_MANUAL = 0,
  NY_HEAP_RAII = 1,
  NY_HEAP_RC = 2,
  NY_HEAP_GC = 3
} ny_heap_policy_t;

typedef enum {
  NY_RUNTIME_MODE_DEFAULT = 0,
  NY_RUNTIME_MODE_SAFE,
  NY_RUNTIME_MODE_FAST,
  NY_RUNTIME_MODE_BARE
} ny_runtime_mode_t;

typedef enum {
  NY_STOP_AFTER_NONE = 0,
  NY_STOP_AFTER_PARSE,
  NY_STOP_AFTER_HM,
  NY_STOP_AFTER_TRAIT,
  NY_STOP_AFTER_FLOW,
  NY_STOP_AFTER_ABI,
  NY_STOP_AFTER_OPT,
} ny_stop_after_stage_t;

typedef enum {
  NY_TYPE_SOLVER_AUTO = 0,
  NY_TYPE_SOLVER_HM = 1,
  NY_TYPE_SOLVER_Z3 = 2,
} ny_type_solver_t;

typedef enum {
  NY_NATIVE_BACKEND_LLVM = 0,
  NY_NATIVE_BACKEND_X86_64 = 1,
  NY_NATIVE_BACKEND_X86 = 2,
  NY_NATIVE_BACKEND_AARCH64 = 3,
  NY_NATIVE_BACKEND_AMDGPU = 4,
  NY_NATIVE_BACKEND_ARM = 5,
  NY_NATIVE_BACKEND_AVR = 6,
  NY_NATIVE_BACKEND_BPF = 7,
  NY_NATIVE_BACKEND_MIPS = 8,
  NY_NATIVE_BACKEND_POWERPC = 9,
  NY_NATIVE_BACKEND_RISCV = 10,
  NY_NATIVE_BACKEND_WASM = 11,
} ny_native_backend_t;

typedef enum {
  NY_NATIVE_ABI_AUTO = 0,
  NY_NATIVE_ABI_SYSV = 1,
  NY_NATIVE_ABI_WIN64 = 2,
  NY_NATIVE_ABI_AAPCS = 3,
} ny_native_abi_t;

typedef enum {
  NY_C_FRONTEND_AUTO = 0,
  NY_C_FRONTEND_LIBCLANG = 1,
  NY_C_FRONTEND_NYTRIX = 2,
} ny_c_frontend_t;

typedef enum ny_opt_profile_kind_t {
  NY_OPT_PROFILE_DEFAULT = 0,
  NY_OPT_PROFILE_SPEED,
  NY_OPT_PROFILE_PEAK,
  NY_OPT_PROFILE_BALANCED,
  NY_OPT_PROFILE_COMPILE,
  NY_OPT_PROFILE_NONE,
  NY_OPT_PROFILE_SIZE,
  NY_OPT_PROFILE_CUSTOM,
} ny_opt_profile_kind_t;

/* Runtime resource limits for --safe-run.
 * Each field is the configured limit; 0 means "not set" (no limit applied).
 * Positive values are in the units indicated by the field comment. */
typedef struct {
  int cpu_seconds;            /* CPU time limit in seconds (RLIMIT_CPU) */
  int wall_seconds;           /* Elapsed wall-clock limit in seconds */
  uint64_t max_rss_bytes;     /* Max address space / RSS in bytes (RLIMIT_AS) */
  int max_files;              /* Open file descriptor limit (RLIMIT_NOFILE) */
  int max_processes;          /* Child process limit (RLIMIT_NPROC) */
  uint64_t max_output_bytes;  /* Combined stdout/stderr byte limit */
  bool telemetry;             /* Bounded supervisor sampling */
  int telemetry_interval_ms;  /* Sampling interval */
  int telemetry_window;       /* Consecutive samples before warning */
  bool contain_process_group; /* Use setpgid to contain descendants */
} ny_safe_run_t;

#include "base/loader.h"

typedef struct {
  ny_mode mode;
  const char *input_file;
  const char *output_file;
  const char *command_string;
  const char *entry_name;
  const char *argv0;
  int opt_level;
  const char *opt_profile;
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
  bool progress;
  bool no_progress;
  bool profiler_mode;
  bool dump_ast;
  bool expand;
  bool expand_json;
  bool meta_trace;
  bool dump_llvm;
  bool dump_tokens;
  bool dump_docs;
  bool dump_funcs;
  bool dump_symbols;
  bool dump_stats;
  bool dump_on_error;
  bool dump_diagnose;
  bool diag_compact;
  int warn_level;
  int max_errors;
  const char *dump_dir;
  const char *expand_only;
  const char *explain_specialization;
  const char *emit_artifact_path;
  bool extract_code;
  bool extract_json;
  const char *extract_lang;
  int extract_line;
  int extract_col;
  const char *stop_after_raw;
  ny_stop_after_stage_t stop_after;
  const char *type_solver_raw;
  ny_type_solver_t type_solver;
  bool collect_errors;
  bool emit_shapes;
  ny_dump_scope_t dump_scope;
  bool verify_module;
  bool trace_exec;
  bool safe_mode;
  bool strict_types;
  bool strict_types_explicit;
  bool help_env;
  bool debug_symbols;
  bool no_std;
  bool effect_require_known;
  bool alias_require_known;
  bool alias_require_no_escape;
  int gprof;
  int std_builtin_ops;
  int compiler_asserts;
  int debug_locals;
  int dwarf_version;
  int dwarf_split_inlining;
  int dwarf_profile_info;
  const char *host_cflags;
  const char *host_ldflags;
  const char *host_triple;
  const char *native_backend_raw;
  const char *native_abi_raw;
  const char *c_frontend_raw;
  ny_native_backend_t native_backend;
  ny_native_abi_t native_abi;
  int native_tier_budget;
  int native_hot_threshold;
  int native_cold_threshold;
  int native_cache_score;
  bool native_prefer_vm;
  bool native_prefer_asm;
  bool native_only;
  bool native_tier_report;
  const char *native_tier_report_path;
  bool native_result_oracle;
  bool watch_files;
  bool hot_reload;
  int watch_poll_ms;
  const char *native_result_oracle_expected;
  ny_c_frontend_t c_frontend;
  bool native_dump_ir;
  bool nyir_dump_text;
  bool nyir_dump_raw;
  bool nyir_dump_stats;
  bool nyir_dump_bin;
  const char *nyir_dump_bin_path;
  bool nyir_metadata_report;
  const char *nyir_metadata_report_path;
  const char *nyir_metadata_bin_path;
  bool nyir_run;
  const char *nyir_run_path;
  const char *nyir_run_bin_path;
  bool nyir_run_profile;
  const char *nyir_run_profile_path;
  int nyir_run_max_steps;
  int nyir_run_recursion_limit;
  const char *native_dump_ir_path;
  const char *arm_float_abi;
  const char *std_path;
  const char *bundle_std_path;
  const char *bundle_symbols_path;
  const char *std_bc_path;
  std_mode_t std_mode;
  bool std_mode_explicit;
  bool repl_explicit;
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
  double timeout;
  bool pending_command_string;
  bool enable_gc;
  bool ownership;
  bool ownership_strict;
  bool borrow_check;
  bool heap_policy_explicit;
  bool gc_flag_seen;
  ny_heap_policy_t heap_policy;
  ny_runtime_mode_t runtime_mode;
  const char *runtime_mode_raw;
  ny_safe_run_t safe_run;
  const char *sanitize;  /* --sanitize=address|undefined|thread|leak */
  const char *jit_engine; /* --jit-engine=orc|mcjit (default: mcjit) */
} ny_options;

void ny_options_init(ny_options *opt);
void ny_options_parse(ny_options *opt, int argc, char **argv);
void ny_options_free(ny_options *opt);
void ny_options_usage(const char *argv0);
void ny_options_usage_env(const char *argv0);
const char *ny_heap_policy_name(ny_heap_policy_t policy);
const char *ny_runtime_mode_name(ny_runtime_mode_t mode);
ny_opt_profile_kind_t ny_opt_profile_kind_from_name(const char *profile_name);
ny_opt_profile_kind_t ny_opt_profile_kind_from_env(void);
int ny_opt_profile_name_is_valid(const char *profile_name);

/* Apply POSIX resource limits for safe execution.  Call once before user
 * code starts (JIT or AOT).  Returns 0 on success, -1 if any limit failed
 * to apply (in which case a diagnostic is printed to stderr).  No-op if
 * all fields in sr are zero / unset. */
int ny_safe_run_apply_limits(const ny_safe_run_t *sr);
int ny_safe_run_spawn(const ny_safe_run_t *sr, const char *const argv[],
                      const char *workload);
typedef int (*ny_safe_run_child_fn)(void *ctx);
int ny_safe_run_call(const ny_safe_run_t *sr, ny_safe_run_child_fn fn,
                     void *ctx, const char *workload);

/* Parse a --safe-run limit specification string into the struct. */
void ny_safe_run_parse(const char *spec, ny_safe_run_t *sr, const char *argv0);

#endif
