#ifndef NY_OPTIONS_H
#define NY_OPTIONS_H

#include <nyr/target.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  NY_MODE_RUN,
  NY_MODE_REPL,
  NY_MODE_BUILD,
  NY_MODE_VERSION,
  NY_MODE_VERSION_ID,
  NY_MODE_HELP,
  NY_MODE_BUNDLE,
  NY_MODE_CLEAN_CACHE
} ny_mode;

typedef enum {
  NY_DUMP_SCOPE_PROGRAM = 0,
  NY_DUMP_SCOPE_LIB = 1,
  NY_DUMP_SCOPE_BOTH = 2
} ny_dump_scope_t;

/* Implementation selector for the differential Ny frontend pass
 * (--ny-desugar=ny|c).  Off keeps the ordinary single-path pipeline. */
typedef enum {
  NY_DESUGAR_OFF = 0,
  NY_DESUGAR_C = 1,
  NY_DESUGAR_NY = 2
} ny_desugar_mode_t;

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
  NY_PROOF_SOLVER_NONE = 0,
  NY_PROOF_SOLVER_PRESBURGER = 1,
  NY_PROOF_SOLVER_Z3 = 2,
  NY_PROOF_SOLVER_AUTO = 3,
} ny_proof_solver_t;

typedef enum {
  NY_NATIVE_TIER_AUTO = 0,
  NY_NATIVE_TIER_BASELINE,
  NY_NATIVE_TIER_STENCIL,
  NY_NATIVE_TIER_FAST,
  NY_NATIVE_TIER_OPT,
  NY_NATIVE_TIER_LLVM,
} nyr_tier_t;

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
  bool opt_size;
  const char *opt_profile;
  const char *opt_pipeline;
  const char *emit_ir_pre_path;
  const char *emit_ir_path;
  const char *emit_bc_path;
  const char *emit_asm_path;
  const char *emit_wasm_path;
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
  bool report_peak_rss;
  bool dump_ast;
  const char *dump_ast_filter;
  bool expand;
  bool expand_json;
  ny_desugar_mode_t ny_desugar;
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
  const char *verify_artifact_path;
  const char *use_artifact_path;
  const char *emit_refinement_interface_path;
  const char *use_refinement_interface_path;
  const char *emit_effect_interface_path;
  const char *use_effect_interface_path;
  bool extract_code;
  bool extract_json;
  const char *extract_lang;
  int extract_line;
  int extract_col;
  const char *stop_after_raw;
  ny_stop_after_stage_t stop_after;
  const char *type_solver_raw;
  ny_type_solver_t type_solver;
  const char *proof_solver_raw;
  ny_proof_solver_t proof_solver;
  bool collect_errors;
  bool emit_shapes;
  ny_dump_scope_t dump_scope;
  bool verify_module;
  bool trace_exec;
  bool safe_mode;
  bool strict_types;
  bool strict_types_explicit;
  bool list_fallbacks;
  bool warn_shadow;
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
  nyr_backend_t native_backend;
  nyr_abi_t native_abi;
  const char *native_tier_raw;
  nyr_tier_t native_tier;
  int native_tier_budget;
  int native_hot_threshold;
  int native_cold_threshold;
  int native_cache_score;
  bool native_prefer_vm;
  bool native_prefer_asm;
  bool native_only;
  /* Run LLVM optimization after independently compiled module bitcode is
   * linked. This is an explicit whole-program/LTO build mode. */
  bool llvm_lto;
  bool native_backend_explicit;
  bool native_enable_cf_mem2reg;
  bool callgraph;
  bool callgraph_json;
  int callgraph_depth;
  bool native_tier_report;
  const char *native_tier_report_path;
  const char *emit_report_path;
  const char *profiler_output_mode;
  bool native_result_oracle;
  bool native_oracle_per_pass;
  int native_oracle_fuzz_count;
  int native_tv_seed_trials;
  bool watch_files;
  bool hot_reload;
  int watch_poll_ms;
  const char *native_result_oracle_expected;
  ny_c_frontend_t c_frontend;
  bool native_dump_ir;
  bool nyr_dump_text;
  bool nyr_dump_raw;
  bool nyr_dump_cfg;
  bool nyr_dump_stats;
  bool nyr_pass_stats;
  bool nyr_ir_verify;
  bool nyr_bisect;
  bool nyr_fingerprint;
  bool nyr_flaky_check;
  bool nyr_rollback_on_verify_fail;
  size_t nyr_global_inst_cap;
  double nyr_pass_growth_factor;
  int verbosity;
  int nyr_track_value;
  const char *dbg_filter;
  bool dbg_delta_only;
  bool dbg_unify_trace;
  bool dbg_trait_resolve;
  bool dbg_format_json;
  bool dbg_abi;
  bool dbg_elf_dump;
  bool dbg_regalloc_heat;
  bool dbg_stack_layout;
  const char *nyr_disable_pass;
  const char *nyr_stop_after;
  bool nyr_dump_bin;
  const char *nyr_dump_bin_path;
  bool nyr_metadata_report;
  const char *nyr_metadata_report_path;
  const char *nyr_metadata_bin_path;
  bool nyr_run;
  const char *nyr_run_path;
  const char *nyr_run_bin_path;
  bool nyr_run_profile;
  const char *nyr_run_profile_path;
  const char *native_profile_use_path;
  int nyr_run_max_steps;
  int nyr_run_recursion_limit;
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
  int fast_math;
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
  const char *sanitize;   /* --sanitize=address|undefined|thread|leak */
  const char *jit_engine; /* --jit-engine=orc|mcjit (default: mcjit) */
  bool jit_fast_isel;     /* --jit-fast-isel: JIT fast instruction selection */
  const char *jit_code_model; /* --jit-code-model=small|kernel|medium|large */
  int jit_opt_level; /* --jit-opt-level=N: JIT optimization level (0..3) */
  bool jit_opt_level_explicit;
  bool jit_perf_map;   /* --jit-perf-map: emit /tmp/perf-<pid>.map */
  bool zero_init;      /* --zero-init: zero all managed heap allocations */
  bool trace_tokens;   /* --trace-tokens: lexer token stream */
  bool trace_parse;    /* --trace-parse: parser rule execution trace */
  bool trace_scope;    /* --trace-scope: binding resolution trace */
  bool dump_ast_typed; /* --dump-ast-typed: AST dump with inferred types */
  bool trace_hm;       /* --trace-hm: HM type inference trace */
  bool explain; /* --explain: semantic explanations of compiler decisions */
  const char *explain_topic; /* --explain=topic: specific explanation topic */
  bool explain_any;          /* --explain-any: explain types degrading to any */
  bool dump_escapes;         /* --dump-escapes: escape analysis verdict */
  bool dump_proofs;          /* --dump-proofs: proven bounds and properties */
  bool trace_llvm;           /* --trace-llvm: codegen LLVM IR trace */
  bool dump_llvm_passes;     /* --dump-llvm-passes: LLVM IR after each pass */
  bool trace_regalloc;       /* --trace-regalloc: machine vreg coloring trace */
  bool dump_obj_full;        /* --dump-obj-full: full object file summary */
  bool compile_profile_fn;   /* --compile-profile-fn: per-fn compile time
                                breakdown */
  bool debug_everything;     /* --debug-everything: enable all debug traces */
} ny_options;

static inline bool ny_options_target_request(const ny_options *opt,
                                             nyr_target_request_t *request) {
  if (!opt || !request)
    return false;
  *request = nyr_target_request_make(opt->native_backend, opt->native_abi,
                                     opt->host_triple, opt->opt_size);
  return true;
}

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
