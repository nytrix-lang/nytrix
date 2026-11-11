/*
 * CLI option definitions, parsing, validation, and --help output for
 * the ny compiler and all bundled tools (test, perf, fmt, doc, make).
 *
 * All public performance/runtime flags are now documented CLI options with
 * --help coverage (--zero-init, --plain-repl, --hot-reload, --watch,
 * --watch-poll, --jit-fast-isel, --jit-opt-level, --jit-code-model,
 * --jit-perf-map).  The remaining NYTRIX_* environment variables are
 * deliberately internal: build-time toggles, diagnostics, or experimental
 * probes that carry no stability contract and are not intended as public
 * API surfaces.
 *
 * Argument-matrix coverage: invalid values must fail with their diagnostic;
 * valid values must compile and run. Cases are grouped by runner metadata
 * rather than folder name and exercised by `./make test`.
 */
#include "base/options.h"
#include "base/common.h"
#include "base/help.h"
#include "base/util.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define access _access
#else
#include <unistd.h>
#endif

int debug_enabled = 0;

int verbose_enabled = 0;
int color_mode = -1;

static void ny_enable_debug_everything_env(void) {
  static const char *const names[] = {
      "NYTRIX_TRACE",         "NYTRIX_TRACE_CALLS",
      "NYTRIX_TRACE_VALUES",  "NYTRIX_TRACE_VERBOSE",
      "NYTRIX_TRACE_IMPORTS", "NYTRIX_TRACE_RESOLVE",
      "NYTRIX_DEBUG_INFER",   "NYTRIX_HM_DEBUG",
      "NYTRIX_PROOF_DEBUG",   "NYTRIX_MEM_TRACE",
      "NY_TRACE_LOWER",       "NY_TRACE_ALIAS",
      "NY_TRACE_DEVIRT",      "NY_TRACE_NCE",
      "NY_TRACE_BCE",         "NY_TRACE_ADCE",
      "NY_TRACE_OCE",         "NY_TRACE_TCO",
      "NY_TRACE_PF",          "NY_TRACE_IPA_CP",
      "NY_TRACE_INLINE",      "NY_TRACE_VECTORIZE",
      "NY_TRACE_PASS_FAIL",   "NY_TRACE_MACHINE_FAIL",
      "NY_TRACE_EMIT",        "NY_TRACE_NATIVE_REACHABLE",
      "NY_TRACE_PURE_CALLS",  "NY_NATIVE_TRACE",
      "NY_TRACE_CONSTTAB",    "NY_TRACE_O3",
      "NY_EGRAPH_TRACE",      "NY_DEBUG_DOM",
      "NY_DUMP_MACH",         "NY_DUMP_OBJ_NYIR"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    ny_setenv(names[i], "1", 0);
  /*
   * Keep lazy-stdlib diagnostics useful without flooding multi-megabyte logs.
   */
  ny_setenv("NYTRIX_TRACE_LAZY_STDLIB_USE_LIMIT", "200", 0);
}

static bool ny_parse_nonneg_int(const char *s, int *out) {
  if (!s || !*s || !out)
    return false;
  char *end = NULL;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0' || errno == ERANGE || v < 0 || v > INT_MAX)
    return false;
  *out = (int)v;
  return true;
}

static bool ny_parse_nonneg_f64(const char *s, double *out) {
  if (!s || !*s || !out)
    return false;
  char *end = NULL;
  errno = 0;
  double v = strtod(s, &end);
  if (end == s || *end != '\0' || errno == ERANGE || !isfinite(v) || v < 0.0)
    return false;
  *out = v;
  return true;
}

static bool ny_parse_line_col(const char *raw, int *out_line, int *out_col) {
  if (!raw || !*raw || !out_line || !out_col)
    return false;
  char *end = NULL;
  long line = strtol(raw, &end, 10);
  if (end == raw || line <= 0 || line > INT_MAX)
    return false;
  long col = 0;
  if (*end == ':' || *end == ',') {
    const char *cstart = end + 1;
    char *cend = NULL;
    col = strtol(cstart, &cend, 10);
    if (cend == cstart || *cend != '\0' || col < 0 || col > INT_MAX)
      return false;
  } else if (*end != '\0') {
    return false;
  }
  *out_line = (int)line;
  *out_col = (int)col;
  return true;
}

static bool ny_parse_warn_level(const char *raw, int *out) {
  if (!raw || !out)
    return false;
  if (strcmp(raw, "none") == 0 || strcmp(raw, "off") == 0 ||
      strcmp(raw, "0") == 0) {
    *out = 0;
    return true;
  }
  if (strcmp(raw, "useful") == 0 || strcmp(raw, "default") == 0 ||
      strcmp(raw, "1") == 0) {
    *out = 1;
    return true;
  }
  if (strcmp(raw, "all") == 0 || strcmp(raw, "2") == 0) {
    *out = 2;
    return true;
  }
  if (strcmp(raw, "error") == 0) {
    *out = 2;
    return true;
  }
  return false;
}

static bool ny_inline_code_semicolon_can_swallow(const char *src) {
  if (!src)
    return false;
  bool line_has_code = false;
  char quote = '\0';
  bool escaped = false;
  for (const char *p = src; *p; ++p) {
    char ch = *p;
    if (quote) {
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == quote)
        quote = '\0';
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      line_has_code = true;
      continue;
    }
    if (ch == '\n') {
      line_has_code = false;
      continue;
    }
    if (ch == ';') {
      const char *q = p + 1;
      while (*q == ' ' || *q == '\t')
        q++;
      if (line_has_code && *q && *q != '\n')
        return true;
      continue;
    }
    if (ch != ' ' && ch != '\t' && ch != '\r')
      line_has_code = true;
  }
  return false;
}

static void ny_warn_inline_code_semicolon(const ny_options *opt) {
  if (!opt || !opt->command_string)
    return;
  if (!ny_inline_code_semicolon_can_swallow(opt->command_string))
    return;
  NY_LOG_WARN("';' is a comment in Nytrix, not a statement separator, so "
              "everything after it on this line is ignored. To split "
              "statements just use a space (or a newline), for example: "
              "ny -c $'use core\\nassert(true, \"ok\")'\n");
}

static bool ny_parse_dump_scope(const char *raw, ny_dump_scope_t *out) {
  if (!raw || !out)
    return false;
  if (strcmp(raw, "program") == 0 || strcmp(raw, "user") == 0) {
    *out = NY_DUMP_SCOPE_PROGRAM;
    return true;
  }
  if (strcmp(raw, "lib") == 0 || strcmp(raw, "stdlib") == 0 ||
      strcmp(raw, "std") == 0) {
    *out = NY_DUMP_SCOPE_LIB;
    return true;
  }
  if (strcmp(raw, "both") == 0 || strcmp(raw, "all") == 0) {
    *out = NY_DUMP_SCOPE_BOTH;
    return true;
  }
  return false;
}

static bool ny_parse_stop_after_stage(const char *raw,
                                      ny_stop_after_stage_t *out) {
  if (!raw || !out)
    return false;
  if (strcasecmp(raw, "parse") == 0 || strcasecmp(raw, "ast") == 0) {
    *out = NY_STOP_AFTER_PARSE;
    return true;
  }
  if (strcasecmp(raw, "hm") == 0 || strcasecmp(raw, "type") == 0 ||
      strcasecmp(raw, "types") == 0 || strcasecmp(raw, "typed") == 0) {
    *out = NY_STOP_AFTER_HM;
    return true;
  }
  if (strcasecmp(raw, "trait") == 0 || strcasecmp(raw, "traits") == 0 ||
      strcasecmp(raw, "resolve") == 0 || strcasecmp(raw, "resolved") == 0) {
    *out = NY_STOP_AFTER_TRAIT;
    return true;
  }
  if (strcasecmp(raw, "flow") == 0 || strcasecmp(raw, "refine") == 0 ||
      strcasecmp(raw, "refined") == 0) {
    *out = NY_STOP_AFTER_FLOW;
    return true;
  }
  if (strcasecmp(raw, "abi") == 0 || strcasecmp(raw, "layout") == 0 ||
      strcasecmp(raw, "lower") == 0 || strcasecmp(raw, "lowered") == 0) {
    *out = NY_STOP_AFTER_ABI;
    return true;
  }
  if (strcasecmp(raw, "opt") == 0 || strcasecmp(raw, "optimized") == 0) {
    *out = NY_STOP_AFTER_OPT;
    return true;
  }
  return false;
}

static bool ny_parse_type_solver(const char *raw, ny_type_solver_t *out) {
  if (!raw || !*raw || !out)
    return false;
  if (strcmp(raw, "auto") == 0) {
    *out = NY_TYPE_SOLVER_AUTO;
    return true;
  }
  if (strcmp(raw, "hm") == 0 || strcmp(raw, "global") == 0) {
    *out = NY_TYPE_SOLVER_HM;
    return true;
  }
  if (strcmp(raw, "z3") == 0) {
    *out = NY_TYPE_SOLVER_Z3;
    return true;
  }
  return false;
}

static bool ny_parse_proof_solver(const char *raw, ny_proof_solver_t *out) {
  if (!raw || !*raw || !out)
    return false;
  if (strcmp(raw, "none") == 0 || strcmp(raw, "off") == 0) {
    *out = NY_PROOF_SOLVER_NONE;
    return true;
  }
  if (strcmp(raw, "presburger") == 0 || strcmp(raw, "fm") == 0 ||
      strcmp(raw, "fourier-motzkin") == 0) {
    *out = NY_PROOF_SOLVER_PRESBURGER;
    return true;
  }
  if (strcmp(raw, "z3") == 0) {
    *out = NY_PROOF_SOLVER_Z3;
    return true;
  }
  if (strcmp(raw, "auto") == 0) {
    *out = NY_PROOF_SOLVER_AUTO;
    return true;
  }
  return false;
}

static bool ny_parse_native_backend(const char *raw, nyr_backend_t *out) {
  if (!raw || !*raw || !out)
    return false;
  if (strcmp(raw, "llvm") == 0 || strcmp(raw, "default") == 0) {
    *out = NY_NATIVE_BACKEND_LLVM;
    return true;
  }
  if (strcmp(raw, "x86_64") == 0 || strcmp(raw, "x64") == 0 ||
      strcmp(raw, "amd64") == 0) {
    *out = NY_NATIVE_BACKEND_X86_64;
    return true;
  }
  if (strcmp(raw, "x86") == 0 || strcmp(raw, "i386") == 0 ||
      strcmp(raw, "i686") == 0) {
    *out = NY_NATIVE_BACKEND_X86;
    return true;
  }
  if (strcmp(raw, "aarch64") == 0 || strcmp(raw, "arm64") == 0) {
    *out = NY_NATIVE_BACKEND_AARCH64;
    return true;
  }
  if (strcmp(raw, "amdgpu") == 0 || strcmp(raw, "amd-gpu") == 0) {
    *out = NY_NATIVE_BACKEND_AMDGPU;
    return true;
  }
  if (strcmp(raw, "arm") == 0 || strcmp(raw, "arm32") == 0) {
    *out = NY_NATIVE_BACKEND_ARM;
    return true;
  }
  if (strcmp(raw, "avr") == 0) {
    *out = NY_NATIVE_BACKEND_AVR;
    return true;
  }
  if (strcmp(raw, "bpf") == 0 || strcmp(raw, "ebpf") == 0) {
    *out = NY_NATIVE_BACKEND_BPF;
    return true;
  }
  if (strcmp(raw, "mips") == 0 || strcmp(raw, "mipsel") == 0) {
    *out = NY_NATIVE_BACKEND_MIPS;
    return true;
  }
  if (strcmp(raw, "powerpc") == 0 || strcmp(raw, "ppc") == 0 ||
      strcmp(raw, "ppc64") == 0) {
    *out = NY_NATIVE_BACKEND_POWERPC;
    return true;
  }
  if (strcmp(raw, "riscv") == 0 || strcmp(raw, "riscv32") == 0 ||
      strcmp(raw, "riscv64") == 0) {
    *out = NY_NATIVE_BACKEND_RISCV;
    return true;
  }
  if (strcmp(raw, "wasm") == 0 || strcmp(raw, "wasm32") == 0 ||
      strcmp(raw, "webassembly") == 0) {
    *out = NY_NATIVE_BACKEND_WASM;
    return true;
  }
  return false;
}

static const char *nyr_backend_name(nyr_backend_t backend) {
  switch (backend) {
  case NY_NATIVE_BACKEND_X86_64:
    return "x86_64";
  case NY_NATIVE_BACKEND_X86:
    return "x86";
  case NY_NATIVE_BACKEND_AARCH64:
    return "aarch64";
  case NY_NATIVE_BACKEND_AMDGPU:
    return "amdgpu";
  case NY_NATIVE_BACKEND_ARM:
    return "arm";
  case NY_NATIVE_BACKEND_AVR:
    return "avr";
  case NY_NATIVE_BACKEND_BPF:
    return "bpf";
  case NY_NATIVE_BACKEND_MIPS:
    return "mips";
  case NY_NATIVE_BACKEND_POWERPC:
    return "powerpc";
  case NY_NATIVE_BACKEND_RISCV:
    return "riscv";
  case NY_NATIVE_BACKEND_WASM:
    return "wasm";
  default:
    return "llvm";
  }
}

static bool ny_parse_native_abi(const char *raw, nyr_abi_t *out) {
  if (!raw || !*raw || !out)
    return false;
  if (strcmp(raw, "auto") == 0) {
    *out = NY_NATIVE_ABI_AUTO;
    return true;
  }
  if (strcmp(raw, "sysv") == 0 || strcmp(raw, "linux") == 0 ||
      strcmp(raw, "darwin") == 0 || strcmp(raw, "macos") == 0) {
    *out = NY_NATIVE_ABI_SYSV;
    return true;
  }
  if (strcmp(raw, "win64") == 0 || strcmp(raw, "windows") == 0 ||
      strcmp(raw, "ms") == 0) {
    *out = NY_NATIVE_ABI_WIN64;
    return true;
  }
  if (strcmp(raw, "aapcs") == 0 || strcmp(raw, "arm") == 0 ||
      strcmp(raw, "arm32") == 0 || strcmp(raw, "eabi") == 0) {
    *out = NY_NATIVE_ABI_AAPCS;
    return true;
  }
  return false;
}

static const char *nyr_abi_name(nyr_abi_t abi) {
  switch (abi) {
  case NY_NATIVE_ABI_SYSV:
    return "sysv";
  case NY_NATIVE_ABI_WIN64:
    return "win64";
  case NY_NATIVE_ABI_AAPCS:
    return "aapcs";
  default:
    return "auto";
  }
}

static bool ny_parse_c_frontend(const char *raw, ny_c_frontend_t *out) {
  if (!raw || !*raw || !out)
    return false;
  if (strcmp(raw, "auto") == 0 || strcmp(raw, "default") == 0) {
    *out = NY_C_FRONTEND_AUTO;
    return true;
  }
  if (strcmp(raw, "libclang") == 0 || strcmp(raw, "clang") == 0) {
    *out = NY_C_FRONTEND_LIBCLANG;
    return true;
  }
  if (strcmp(raw, "nytrix") == 0 || strcmp(raw, "ny") == 0 ||
      strcmp(raw, "internal") == 0) {
    *out = NY_C_FRONTEND_NYTRIX;
    return true;
  }
  return false;
}

static const char *ny_c_frontend_name(ny_c_frontend_t frontend) {
  switch (frontend) {
  case NY_C_FRONTEND_LIBCLANG:
    return "libclang";
  case NY_C_FRONTEND_NYTRIX:
    return "nytrix";
  default:
    return "auto";
  }
}

static void ny_set_native_backend_or_die(ny_options *opt, const char *value,
                                         const char *argv0) {
  if (!ny_parse_native_backend(value, &opt->native_backend)) {
    fprintf(
        stderr,
        "invalid native backend: %s (expected "
        "llvm|x86_64|x86|aarch64|arm|riscv|wasm|bpf|mips|powerpc|avr|amdgpu)\n",
        value ? value : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  opt->native_backend_raw = nyr_backend_name(opt->native_backend);
  opt->native_backend_explicit = true;
}

/*
 * Produced host artifacts use the owned native encoder unless the caller
 * explicitly selected a backend. Interactive execution remains a managed/JIT
 * workflow because it supports source shapes outside the native contract.
 */
static void ny_select_host_native_backend_for_artifact(ny_options *opt,
                                                       const char *argv0) {
  if (!opt || opt->native_backend_explicit)
    return;
#if defined(__x86_64__) || defined(_M_X64)
  ny_set_native_backend_or_die(opt, "x86_64", argv0);
#elif defined(__aarch64__) || defined(_M_ARM64)
  ny_set_native_backend_or_die(opt, "aarch64", argv0);
#endif
}

static void ny_set_native_abi_or_die(ny_options *opt, const char *value,
                                     const char *argv0) {
  if (!ny_parse_native_abi(value, &opt->native_abi)) {
    fprintf(stderr, "invalid native ABI: %s (expected auto|sysv|win64|aapcs)\n",
            value ? value : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  opt->native_abi_raw = nyr_abi_name(opt->native_abi);
}

static bool ny_parse_native_tier(const char *raw, nyr_tier_t *out) {
  if (!raw || !*raw || !out)
    return false;
  if (strcmp(raw, "auto") == 0 || strcmp(raw, "default") == 0) {
    *out = NY_NATIVE_TIER_AUTO;
    return true;
  }
  if (strcmp(raw, "baseline") == 0) {
    *out = NY_NATIVE_TIER_BASELINE;
    return true;
  }
  if (strcmp(raw, "stencil") == 0) {
    *out = NY_NATIVE_TIER_STENCIL;
    return true;
  }
  if (strcmp(raw, "fast") == 0) {
    *out = NY_NATIVE_TIER_FAST;
    return true;
  }
  if (strcmp(raw, "opt") == 0 || strcmp(raw, "optimize") == 0) {
    *out = NY_NATIVE_TIER_OPT;
    return true;
  }
  if (strcmp(raw, "llvm") == 0 || strcmp(raw, "o2") == 0 ||
      strcmp(raw, "o3") == 0) {
    *out = NY_NATIVE_TIER_LLVM;
    return true;
  }
  return false;
}

static const char *nyr_tier_name(nyr_tier_t tier) {
  switch (tier) {
  case NY_NATIVE_TIER_BASELINE:
    return "baseline";
  case NY_NATIVE_TIER_STENCIL:
    return "stencil";
  case NY_NATIVE_TIER_FAST:
    return "fast";
  case NY_NATIVE_TIER_OPT:
    return "opt";
  case NY_NATIVE_TIER_LLVM:
    return "llvm";
  default:
    return "auto";
  }
}

static void ny_set_native_tier_or_die(ny_options *opt, const char *value,
                                      const char *argv0) {
  if (!ny_parse_native_tier(value, &opt->native_tier)) {
    fprintf(stderr,
            "invalid native tier: %s (expected "
            "auto|baseline|stencil|fast|opt|llvm)\n",
            value ? value : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  opt->native_tier_raw = nyr_tier_name(opt->native_tier);
}

static void ny_set_c_frontend_or_die(ny_options *opt, const char *value,
                                     const char *argv0) {
  if (!ny_parse_c_frontend(value, &opt->c_frontend)) {
    fprintf(stderr, "invalid C frontend: %s (expected auto|libclang|nytrix)\n",
            value ? value : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  opt->c_frontend_raw = ny_c_frontend_name(opt->c_frontend);
}

const char *ny_heap_policy_name(ny_heap_policy_t policy) {
  switch (policy) {
  case NY_HEAP_MANUAL:
    return "manual";
  case NY_HEAP_RAII:
    return "raii";
  case NY_HEAP_RC:
    return "rc";
  case NY_HEAP_GC:
    return "gc";
  default:
    return "manual";
  }
}

const char *ny_runtime_mode_name(ny_runtime_mode_t mode) {
  switch (mode) {
  case NY_RUNTIME_MODE_SAFE:
    return "safe";
  case NY_RUNTIME_MODE_FAST:
    return "fast";
  case NY_RUNTIME_MODE_BARE:
    return "bare";
  default:
    return "default";
  }
}

static bool ny_parse_runtime_mode(const char *raw, ny_runtime_mode_t *out) {
  if (!raw || !out)
    return false;
  if (strcmp(raw, "safe") == 0) {
    *out = NY_RUNTIME_MODE_SAFE;
    return true;
  }
  if (strcmp(raw, "fast") == 0) {
    *out = NY_RUNTIME_MODE_FAST;
    return true;
  }
  if (strcmp(raw, "bare") == 0) {
    *out = NY_RUNTIME_MODE_BARE;
    return true;
  }
  if (strcmp(raw, "default") == 0) {
    *out = NY_RUNTIME_MODE_DEFAULT;
    return true;
  }
  return false;
}

static void ny_apply_runtime_mode_or_die(ny_options *opt, const char *raw,
                                         const char *argv0) {
  ny_runtime_mode_t mode = NY_RUNTIME_MODE_DEFAULT;
  if (!ny_parse_runtime_mode(raw, &mode)) {
    fprintf(stderr, "invalid runtime mode: %s (expected safe|fast|bare)\n",
            raw ? raw : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  opt->runtime_mode = mode;
  opt->runtime_mode_raw = ny_runtime_mode_name(mode);
  opt->heap_policy_explicit = false;
  opt->gc_flag_seen = false;
  switch (mode) {
  case NY_RUNTIME_MODE_SAFE:
    opt->safe_mode = true;
    opt->strict_types = true;
    opt->heap_policy = NY_HEAP_RC;
    opt->enable_gc = false;
    opt->ownership = true;
    opt->ownership_strict = true;
    break;
  case NY_RUNTIME_MODE_FAST:
    opt->safe_mode = false;
    opt->heap_policy = NY_HEAP_MANUAL;
    opt->enable_gc = false;
    opt->ownership = false;
    opt->ownership_strict = false;
    break;
  case NY_RUNTIME_MODE_BARE:
    opt->safe_mode = false;
    opt->strict_types = false;
    opt->heap_policy = NY_HEAP_MANUAL;
    opt->enable_gc = false;
    opt->ownership = false;
    opt->ownership_strict = false;
    break;
  default:
    break;
  }
}

ny_opt_profile_kind_t ny_opt_profile_kind_from_name(const char *profile_name) {
  if (!profile_name || !*profile_name)
    return NY_OPT_PROFILE_DEFAULT;
  if (strcasecmp(profile_name, "default") == 0)
    return NY_OPT_PROFILE_DEFAULT;
  if (strcasecmp(profile_name, "speed") == 0)
    return NY_OPT_PROFILE_SPEED;
  if (strcasecmp(profile_name, "peak") == 0)
    return NY_OPT_PROFILE_PEAK;
  if (strcasecmp(profile_name, "balanced") == 0)
    return NY_OPT_PROFILE_BALANCED;
  if (strcasecmp(profile_name, "compile") == 0)
    return NY_OPT_PROFILE_COMPILE;
  if (strcasecmp(profile_name, "none") == 0)
    return NY_OPT_PROFILE_NONE;
  if (strcasecmp(profile_name, "size") == 0)
    return NY_OPT_PROFILE_SIZE;
  return NY_OPT_PROFILE_CUSTOM;
}

ny_opt_profile_kind_t ny_opt_profile_kind_from_env(void) {
  return ny_opt_profile_kind_from_name(getenv("NYTRIX_OPT_PROFILE"));
}

int ny_opt_profile_name_is_valid(const char *profile_name) {
  return ny_opt_profile_kind_from_name(profile_name) != NY_OPT_PROFILE_CUSTOM;
}

static bool ny_parse_heap_policy(const char *raw, ny_heap_policy_t *out) {
  if (!raw || !out)
    return false;
  if (strcmp(raw, "manual") == 0) {
    *out = NY_HEAP_MANUAL;
    return true;
  }
  if (strcmp(raw, "raii") == 0 || strcmp(raw, "ownership") == 0) {
    *out = NY_HEAP_RAII;
    return true;
  }
  if (strcmp(raw, "rc") == 0 || strcmp(raw, "refcount") == 0 ||
      strcmp(raw, "refcounting") == 0) {
    *out = NY_HEAP_RC;
    return true;
  }
  if (strcmp(raw, "gc") == 0) {
    *out = NY_HEAP_GC;
    return true;
  }
  return false;
}

static void ny_set_heap_policy_or_die(ny_options *opt, const char *raw,
                                      const char *argv0) {
  ny_heap_policy_t policy = NY_HEAP_MANUAL;
  if (!ny_parse_heap_policy(raw, &policy)) {
    fprintf(stderr, "invalid heap policy: %s (expected manual|raii|rc|gc)\n",
            raw ? raw : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  opt->heap_policy = policy;
  opt->heap_policy_explicit = true;
  opt->enable_gc = (policy == NY_HEAP_GC);
  opt->ownership = (policy == NY_HEAP_RAII || policy == NY_HEAP_RC);
}

static bool ny_is_opt_profile_name(const char *name) {
  return name && ny_opt_profile_name_is_valid(name);
}

static void ny_set_opt_level(ny_options *opt, int level) {
  opt->opt_level = level;
  opt->opt_size = false;
  opt->opt_level_explicit = true;
  /*
   * -O alone must be sufficient: an explicit -O level also drives the LLVM
   * JIT codegen level unless --jit-opt-level was explicitly given (in which
   * case the explicit JIT level wins regardless of flag order).
   */
  if (!opt->jit_opt_level_explicit)
    opt->jit_opt_level = level > 3 ? 3 : (level < 0 ? 0 : level);
}

static void ny_set_opt_level_value_or_die(ny_options *opt, const char *value) {
  if (value && strcmp(value, "s") == 0) {
    ny_set_opt_level(opt, 2);
    opt->opt_size = true;
    return;
  }
  if (value && strcmp(value, "z") == 0) {
    ny_set_opt_level(opt, 0);
    opt->opt_size = true;
    return;
  }
  int level = 0;
  if (!ny_parse_nonneg_int(value, &level) || level > 3) {
    fprintf(stderr, "ny: invalid --opt-level '%s' (expected 0|1|2|3|s|z)\n",
            value ? value : "");
    exit(2);
  }
  ny_set_opt_level(opt, level);
}

static void ny_sync_opt_profile_env(ny_options *opt) {
  if (!opt)
    return;
  if (opt->opt_profile && *opt->opt_profile) {
    ny_setenv("NYTRIX_OPT_PROFILE", opt->opt_profile, 1);
  } else {
    const char *env_profile = getenv("NYTRIX_OPT_PROFILE");
    if (env_profile && *env_profile && ny_is_opt_profile_name(env_profile))
      opt->opt_profile = env_profile;
  }

  ny_opt_profile_kind_t profile =
      ny_opt_profile_kind_from_name(opt->opt_profile);
  bool native_emit = !opt->run_jit && (opt->emit_only || opt->run_aot);
  bool standalone_emit_check =
      opt->emit_only && !opt->output_file && !opt->run_aot && !opt->run_jit;
  if (!opt->opt_level_explicit) {
    switch (profile) {
    case NY_OPT_PROFILE_PEAK:
      opt->opt_level = 3;
      opt->opt_size = false;
      break;
    case NY_OPT_PROFILE_SPEED:
      opt->opt_level = 2;
      opt->opt_size = false;
      break;
    case NY_OPT_PROFILE_BALANCED:
    case NY_OPT_PROFILE_SIZE:
      opt->opt_level = 2;
      opt->opt_size = profile == NY_OPT_PROFILE_SIZE;
      break;
    case NY_OPT_PROFILE_COMPILE:
      opt->opt_level = 0;
      opt->opt_size = false;
      break;
    case NY_OPT_PROFILE_NONE:
      opt->opt_level = 0;
      opt->opt_size = false;
      break;
    case NY_OPT_PROFILE_CUSTOM:
    case NY_OPT_PROFILE_DEFAULT:
    default:
      if (standalone_emit_check)
        opt->opt_level = 2;
      else if (native_emit)
        opt->opt_level = 2;
      if (standalone_emit_check || native_emit)
        opt->opt_size = true;
      break;
    }
  }

  if (profile == NY_OPT_PROFILE_DEFAULT && native_emit &&
      !standalone_emit_check) {
    ny_setenv("NYTRIX_SIMPLE_RAW_INT_CALL_FAST", "1", 0);
    ny_setenv("NYTRIX_PROVEN_INT_BRANCH_FAST", "1", 0);
    ny_setenv("NYTRIX_PROVEN_INT_MOD_FAST", "1", 0);
    ny_setenv("NYTRIX_PRINT_PROVEN_INT_FAST", "1", 0);
    ny_setenv("NYTRIX_PRINT_PROVEN_STR_FAST", "1", 0);
    ny_setenv("NYTRIX_RAW_INT_EXPR_FAST", "1", 0);
    ny_setenv("NYTRIX_RAW_INT_EXPR_FAST_OPS",
              "add,sub,mul,div,mod,and,or,xor,shl,shr,lt,le,gt,ge,eq,ne", 0);
    ny_setenv("NYTRIX_MONO_LIST_ARGS", "1", 0);
    ny_setenv("NYTRIX_MONO_IMPERATIVE", "1", 0);
  }
  if (profile == NY_OPT_PROFILE_SPEED || profile == NY_OPT_PROFILE_PEAK) {
    ny_setenv("NYTRIX_SIMPLE_RAW_INT_CALL_FAST", "1", 0);
    ny_setenv("NYTRIX_PROVEN_INT_CAST_FAST", "1", 0);
    ny_setenv("NYTRIX_PROVEN_INT_BRANCH_FAST", "1", 0);
    ny_setenv("NYTRIX_PROVEN_INT_MOD_FAST", "1", 0);
    /*
     * Raw tagged-int expression lowering (elides the dynamic tag check and
     * its slow BigInt fallback NIR for proven-int operands) is valuable at
     * every runtime-oriented tier, not just Z3-proven peak.
     */
    ny_setenv("NYTRIX_RAW_INT_EXPR_FAST", "1", 0);
    ny_setenv("NYTRIX_RAW_INT_EXPR_FAST_OPS",
              "add,sub,mul,div,mod,and,or,xor,shl,shr,lt,le,gt,ge,eq,ne", 0);
  }
  if (profile == NY_OPT_PROFILE_PEAK) {
    ny_setenv("NYTRIX_MONO_IMPERATIVE", "1", 0);
    /*
     * The separate PROVEN path lacks a complete small-int proof.
     */
    ny_setenv("NYTRIX_RAW_INT_EXPR_FAST", "1", 0);
    ny_setenv("NYTRIX_RAW_INT_SLOT_EXPR_FAST", "1", 0);
    ny_setenv("NYTRIX_RAW_INT_EXPR_FAST_OPS",
              "add,sub,mul,div,mod,and,or,xor,shl,shr,lt,le,gt,ge,eq,ne", 0);
  }
}

static const char *ny_option_value_or_die(const char *arg, const char *flag,
                                          int *argi, int argc, char **argv,
                                          const char *argv0) {
  size_t flag_len = strlen(flag);
  if (strncmp(arg, flag, flag_len) == 0 && arg[flag_len] == '=')
    return arg + flag_len + 1;
  if (strcmp(arg, flag) != 0)
    return NULL;
  if (*argi + 1 < argc) {
    const char *next = argv[*argi + 1];
    if (next[0] != '-' || next[1] == '\0')
      return argv[++(*argi)];
    fprintf(stderr,
            "ny: missing argument for %s before option %s; use %s=VALUE\n",
            flag, next, flag);
    exit(2);
  }
  (void)argv0;
  fprintf(stderr, "ny: missing argument for %s; use %s=VALUE\n", flag, flag);
  exit(2);
}

/*
 * Parse a --safe-run value string:
 * "cpu:10,wall:15,rss:256m,output:16m,files:128,nproc:32"
 * Units: seconds (default), k/K=KiB, m/M=MiB, g/G=GiB.  Unknown keys
 * produce a warning and are skipped.  An empty string sets safe defaults
 * (cpu=30s, rss=512m, files=256, nproc=64, process group containment).
 */
void ny_safe_run_parse(const char *spec, ny_safe_run_t *sr, const char *argv0) {
  (void)argv0;
  /*
   * Apply safe defaults when called with empty/NULL or "defaults"
   */
  if (!spec || !*spec || strcmp(spec, "defaults") == 0) {
    sr->cpu_seconds = 30;
    sr->wall_seconds = 30;
    sr->max_rss_bytes = 512ULL * 1024 * 1024; /* 512 MiB */
    sr->max_files = 256;
    sr->max_processes = 64;
    sr->max_output_bytes = 16ULL * 1024 * 1024;
    sr->telemetry = true;
    sr->telemetry_interval_ms = 250;
    sr->telemetry_window = 3;
    sr->contain_process_group = true;
    return;
  }

  sr->telemetry = true;
  if (sr->telemetry_interval_ms <= 0)
    sr->telemetry_interval_ms = 250;
  if (sr->telemetry_window <= 0)
    sr->telemetry_window = 3;

  /*
   * Make a mutable copy for strtok_r.
   */
  size_t len = strlen(spec);
  char *buf = (char *)malloc(len + 1);
  if (!buf) {
    fprintf(stderr, "safe-run: out of memory parsing limit spec\n");
    exit(1);
  }
  memcpy(buf, spec, len + 1);

  char *save = NULL;
  for (char *tok = strtok_r(buf, ",", &save); tok;
       tok = strtok_r(NULL, ",", &save)) {
    char *key = tok;
    char *val = strchr(tok, ':');
    if (!val) {
      fprintf(stderr,
              "safe-run: invalid limit spec '%s' (expected key:value)\n", tok);
      free(buf);
      exit(1);
    }
    *val++ = '\0';

    /*
     * Trim whitespace from key
     */
    while (*key == ' ')
      key++;
    char *kend = key + strlen(key) - 1;
    while (kend > key && *kend == ' ')
      *kend-- = '\0';

    /*
     * Parse numeric value with optional suffix
     */
    char *end = NULL;
    errno = 0;
    unsigned long long n = strtoull(val, &end, 10);
    if (end == val || errno == ERANGE) {
      fprintf(stderr, "safe-run: invalid number '%s' for key '%s'\n", val, key);
      free(buf);
      exit(1);
    }
    /*
     * Check for unit suffix
     */
    uint64_t multiplier = 1;
    if (end && *end) {
      switch (*end) {
      case 'k':
      case 'K':
        multiplier = 1024ULL;
        break;
      case 'm':
      case 'M':
        multiplier = 1024ULL * 1024;
        break;
      case 'g':
      case 'G':
        multiplier = 1024ULL * 1024 * 1024;
        break;
      case 's':
      case 'S':
        multiplier = 1;
        break; /* seconds (explicit) */
      default:
        fprintf(stderr, "safe-run: unknown unit in '%s' for key '%s'\n", val,
                key);
        free(buf);
        exit(1);
      }
      if (end[1] != '\0') {
        fprintf(stderr, "safe-run: invalid unit in '%s' for key '%s'\n", val,
                key);
        free(buf);
        exit(1);
      }
    }

    if (n > UINT64_MAX / multiplier) {
      fprintf(stderr, "safe-run: value '%s' for key '%s' is too large\n", val,
              key);
      free(buf);
      exit(1);
    }
    uint64_t amount = (uint64_t)n * multiplier;

    bool has_unit = end && *end;
    bool byte_limit = strcmp(key, "rss") == 0 || strcmp(key, "as") == 0 ||
                      strcmp(key, "mem") == 0 || strcmp(key, "output") == 0 ||
                      strcmp(key, "out") == 0;
    bool time_limit = strcmp(key, "cpu") == 0 || strcmp(key, "wall") == 0 ||
                      strcmp(key, "time") == 0;
    if ((time_limit && has_unit && *end != 's' && *end != 'S') ||
        (byte_limit && has_unit && (*end == 's' || *end == 'S')) ||
        (!time_limit && !byte_limit && has_unit)) {
      fprintf(stderr, "safe-run: unit in '%s' is invalid for key '%s'\n", val,
              key);
      free(buf);
      exit(1);
    }

    if (strcmp(key, "cpu") == 0) {
      sr->cpu_seconds = (int)((amount < (uint64_t)INT_MAX) ? amount : INT_MAX);
    } else if (strcmp(key, "wall") == 0 || strcmp(key, "time") == 0) {
      sr->wall_seconds = (int)((amount < (uint64_t)INT_MAX) ? amount : INT_MAX);
    } else if (strcmp(key, "rss") == 0 || strcmp(key, "as") == 0 ||
               strcmp(key, "mem") == 0) {
      sr->max_rss_bytes = amount;
    } else if (strcmp(key, "files") == 0 || strcmp(key, "nofile") == 0) {
      sr->max_files = (int)((amount < (uint64_t)INT_MAX) ? amount : INT_MAX);
    } else if (strcmp(key, "nproc") == 0 || strcmp(key, "procs") == 0) {
      sr->max_processes =
          (int)((amount < (uint64_t)INT_MAX) ? amount : INT_MAX);
    } else if (strcmp(key, "output") == 0 || strcmp(key, "out") == 0) {
      sr->max_output_bytes = amount;
    } else if (strcmp(key, "group") == 0 || strcmp(key, "contain") == 0) {
      sr->contain_process_group = (n != 0);
    } else if (strcmp(key, "telemetry") == 0) {
      sr->telemetry = (n != 0);
    } else if (strcmp(key, "sample_ms") == 0) {
      sr->telemetry_interval_ms =
          (int)((amount < (uint64_t)INT_MAX) ? amount : INT_MAX);
    } else if (strcmp(key, "window") == 0) {
      sr->telemetry_window =
          (int)((amount < (uint64_t)INT_MAX) ? amount : INT_MAX);
    } else {
      fprintf(stderr, "safe-run: unknown limit key '%s'\n", key);
      free(buf);
      exit(1);
    }
  }

  free(buf);
}

static bool ny_short_exec_cluster(const char *arg, bool *want_repl,
                                  bool *want_eval) {
  if (!arg || arg[0] != '-' || arg[1] == '-' || arg[1] == '\0')
    return false;
  bool repl = false;
  bool eval = false;
  for (const char *p = arg + 1; *p; ++p) {
    switch (*p) {
    case 'i':
      repl = true;
      break;
    case 'c':
    case 'e':
      eval = true;
      break;
    default:
      return false;
    }
  }
  if (want_repl)
    *want_repl = repl;
  if (want_eval)
    *want_eval = eval;
  return repl || eval;
}

static void ny_options_enable_repl(ny_options *opt) {
  opt->mode = NY_MODE_REPL;
  opt->repl_explicit = true;
}

static void ny_options_set_command_string(ny_options *opt, const char *code) {
  opt->command_string = code;
  opt->pending_command_string = false;
}

static void ny_options_take_command_string(ny_options *opt, int *argi, int argc,
                                           char **argv) {
  if (*argi + 1 < argc) {
    ny_options_set_command_string(opt, argv[++(*argi)]);
  } else {
    opt->pending_command_string = true;
  }
}

static bool ny_prof_list_starts_with_mode(const char *value) {
  static const char *const modes[] = {"timing", "stats", "gprof", "compile",
                                      "peak-rss"};
  if (!value)
    return false;
  for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    size_t length = strlen(modes[i]);
    if (strncmp(value, modes[i], length) == 0 &&
        (value[length] == '\0' || value[length] == ','))
      return true;
  }
  return false;
}

static bool ny_options_apply_exec_option(ny_options *opt, const char *arg,
                                         int *argi, int argc, char **argv,
                                         const char *argv0) {
  const char *value = NULL;
  if ((value = ny_option_value_or_die(arg, "--eval", argi, argc, argv,
                                      argv0)) != NULL) {
    ny_options_set_command_string(opt, value);
    return true;
  }
  if ((value = ny_option_value_or_die(arg, "--eval-repl", argi, argc, argv,
                                      argv0)) != NULL ||
      (value = ny_option_value_or_die(arg, "--interactive-eval", argi, argc,
                                      argv, argv0)) != NULL ||
      (value = ny_option_value_or_die(arg, "--eval-interactive", argi, argc,
                                      argv, argv0)) != NULL) {
    ny_options_enable_repl(opt);
    ny_options_set_command_string(opt, value);
    return true;
  }
  if (strcmp(arg, "--interactive") == 0 || strcmp(arg, "-interactive") == 0 ||
      strcmp(arg, "--repl") == 0 || strcmp(arg, "-repl") == 0) {
    ny_options_enable_repl(opt);
    return true;
  }

  bool short_repl = false;
  bool short_eval = false;
  if (!ny_short_exec_cluster(arg, &short_repl, &short_eval))
    return false;
  if (short_repl)
    ny_options_enable_repl(opt);
  if (short_eval)
    ny_options_take_command_string(opt, argi, argc, argv);
  return true;
}

static int ny_parse_nonneg_int_or_die(const char *raw, const char *label,
                                      const char *argv0) {
  int n = 0;
  if (!ny_parse_nonneg_int(raw, &n)) {
    fprintf(stderr, "invalid %s: %s\n", label, raw ? raw : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  return n;
}

static double ny_parse_nonneg_f64_or_die(const char *raw, const char *label,
                                         const char *argv0) {
  double value = 0.0;
  if (!ny_parse_nonneg_f64(raw, &value)) {
    fprintf(stderr, "invalid %s: %s\n", label, raw ? raw : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  return value;
}

static int ny_parse_dwarf_version_or_die(const char *raw, const char *argv0) {
  int n = 0;
  if (!ny_parse_nonneg_int(raw, &n) || n < 2 || n > 5) {
    fprintf(stderr, "invalid DWARF version: %s (expected 2..5)\n",
            raw ? raw : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  return n;
}

static std_mode_t ny_parse_std_mode_or_die(const char *mode,
                                           const char *argv0) {
  if (strcmp(mode, "none") == 0)
    return STD_MODE_NONE;
  if (strcmp(mode, "full") == 0)
    return STD_MODE_FULL;
  if (strcmp(mode, "minimal") == 0)
    return STD_MODE_MINIMAL;
  if (strcmp(mode, "bc") == 0)
    return STD_MODE_BC;
  fprintf(stderr, "unknown std mode: %s\n", mode ? mode : "(null)");
  ny_options_usage(argv0);
  exit(1);
}

static std_mode_t ny_parse_canonical_std_mode_or_die(const char *mode) {
  if (mode && (strcmp(mode, "none") == 0 || strcmp(mode, "minimal") == 0 ||
               strcmp(mode, "full") == 0))
    return ny_parse_std_mode_or_die(mode, NULL);
  fprintf(stderr, "ny: invalid --std '%s' (expected none|minimal|full)\n",
          mode ? mode : "");
  exit(2);
}

static void ny_push_link_flag_or_die(ny_options *opt, bool is_dir,
                                     const char *flag, int *argi, int argc,
                                     char **argv, const char *argv0) {
  if (*argi + 1 >= argc) {
    fprintf(stderr, "missing argument for %s\n", flag);
    ny_options_usage(argv0);
    exit(1);
  }
  const char *arg = argv[++(*argi)];
  const char *prefix = is_dir ? "-L" : "-l";
  size_t len = strlen(prefix) + strlen(arg);
  char *buf = malloc(len + 1);
  if (!buf) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  size_t prefix_len = strlen(prefix);
  memcpy(buf, prefix, prefix_len);
  memcpy(buf + prefix_len, arg, strlen(arg) + 1);
  if (is_dir)
    vec_push(&opt->link_dirs, buf);
  else
    vec_push(&opt->link_libs, buf);
}

static bool ny_is_arm_float_abi(const char *abi) {
  return abi && (strcmp(abi, "soft") == 0 || strcmp(abi, "softfp") == 0 ||
                 strcmp(abi, "hard") == 0);
}

static bool ny_is_gpu_mode(const char *mode) {
  return mode && (strcmp(mode, "off") == 0 || strcmp(mode, "auto") == 0 ||
                  strcmp(mode, "opencl") == 0);
}

static bool ny_is_gpu_backend(const char *backend) {
  return backend &&
         (strcmp(backend, "none") == 0 || strcmp(backend, "auto") == 0 ||
          strcmp(backend, "opencl") == 0 || strcmp(backend, "cuda") == 0 ||
          strcmp(backend, "hip") == 0 || strcmp(backend, "metal") == 0);
}

static const char *ny_canonical_accel_target(const char *target) {
  if (!target || !*target)
    return NULL;
  if (strcmp(target, "off") == 0 || strcmp(target, "none") == 0)
    return "none";
  if (strcmp(target, "auto") == 0)
    return "auto";
  if (strcmp(target, "cuda") == 0 || strcmp(target, "ptx") == 0 ||
      strcmp(target, "nvptx") == 0)
    return "nvptx";
  if (strcmp(target, "hip") == 0 || strcmp(target, "rocm") == 0 ||
      strcmp(target, "amdgpu") == 0 || strcmp(target, "gcn") == 0 ||
      strcmp(target, "rdna") == 0)
    return "amdgpu";
  if (strcmp(target, "opencl") == 0 || strcmp(target, "spirv") == 0 ||
      strcmp(target, "vulkan") == 0 || strcmp(target, "spv") == 0)
    return "spirv";
  if (strcmp(target, "hsaco") == 0 || strcmp(target, "hsa") == 0 ||
      strcmp(target, "hsa-code-object") == 0 ||
      strcmp(target, "hsa_code_object") == 0 || strcmp(target, "rocm_hsa") == 0)
    return "hsaco";
  return NULL;
}

static const char *ny_gpu_backend_from_target(const char *target) {
  if (!target || !*target)
    return NULL;
  if (strcmp(target, "off") == 0 || strcmp(target, "none") == 0)
    return "none";
  if (strcmp(target, "auto") == 0)
    return "auto";
  if (strcmp(target, "cuda") == 0 || strcmp(target, "nvptx") == 0 ||
      strcmp(target, "ptx") == 0)
    return "cuda";
  if (strcmp(target, "hip") == 0 || strcmp(target, "rocm") == 0 ||
      strcmp(target, "amdgpu") == 0 || strcmp(target, "hsaco") == 0 ||
      strcmp(target, "gcn") == 0 || strcmp(target, "rdna") == 0 ||
      strcmp(target, "hsa") == 0 || strcmp(target, "hsa-code-object") == 0 ||
      strcmp(target, "hsa_code_object") == 0)
    return "hip";
  if (strcmp(target, "opencl") == 0 || strcmp(target, "spirv") == 0 ||
      strcmp(target, "vulkan") == 0 || strcmp(target, "spv") == 0)
    return "opencl";
  if (strcmp(target, "metal") == 0)
    return "metal";
  return NULL;
}

static bool ny_is_accel_target(const char *target) {
  return ny_canonical_accel_target(target) != NULL;
}

static const char *ny_canonical_accel_object(const char *kind) {
  if (!kind || !*kind)
    return NULL;
  if (strcmp(kind, "obj") == 0)
    return "o";
  if (strcmp(kind, "cubin") == 0)
    return "ptx";
  if (strcmp(kind, "auto") == 0 || strcmp(kind, "none") == 0 ||
      strcmp(kind, "ptx") == 0 || strcmp(kind, "o") == 0 ||
      strcmp(kind, "spv") == 0 || strcmp(kind, "hsaco") == 0)
    return kind;
  return NULL;
}

static bool ny_is_accel_object(const char *kind) {
  return ny_canonical_accel_object(kind) != NULL;
}

static void ny_set_accel_target_or_die(ny_options *opt, const char *target,
                                       const char *argv0, const char *flag) {
  if (!ny_is_accel_target(target)) {
    fprintf(
        stderr,
        "ny: invalid %s '%s' (expected none|auto|nvptx|amdgpu|spirv|hsaco)\n",
        flag, target ? target : "(null)");
    (void)argv0;
    exit(2);
  }
  const char *canon = ny_canonical_accel_target(target);
  opt->accel_target = canon;
  const char *backend = ny_gpu_backend_from_target(target);
  if (backend && ny_is_gpu_backend(backend))
    opt->gpu_backend = backend;
}

static void ny_set_accel_object_or_die(ny_options *opt, const char *kind,
                                       const char *argv0) {
  if (!ny_is_accel_object(kind)) {
    fprintf(stderr,
            "ny: invalid --accel-object '%s' (expected "
            "auto|none|ptx|o|spv|hsaco)\n",
            kind ? kind : "(null)");
    (void)argv0;
    exit(2);
  }
  const char *canon = ny_canonical_accel_object(kind);
  opt->accel_object = canon;
}

static void ny_set_gpu_target_compat_or_die(ny_options *opt, const char *target,
                                            const char *argv0) {
  const char *backend = ny_gpu_backend_from_target(target);
  const char *accel = ny_canonical_accel_target(target);
  if (!backend && !accel) {
    fprintf(stderr,
            "ny: invalid --gpu-target '%s' (expected "
            "off|none|auto|cuda|nvptx|ptx|hip|rocm|amdgpu|opencl|spirv|vulkan|"
            "spv|metal|hsaco|hsa|hsa-code-object|hsa_code_object|gcn|rdna)\n",
            target ? target : "(null)");
    (void)argv0;
    exit(2);
  }
  if (backend && ny_is_gpu_backend(backend))
    opt->gpu_backend = backend;
  if (accel)
    opt->accel_target = accel;
}

static void ny_set_stop_after_or_die(ny_options *opt, const char *value,
                                     const char *argv0) {
  (void)argv0;
  ny_stop_after_stage_t stage = NY_STOP_AFTER_NONE;
  if (!ny_parse_stop_after_stage(value, &stage)) {
    fprintf(stderr,
            "ny: invalid --stop-after '%s' (expected "
            "parse|hm|trait|flow|abi|opt)\n",
            value ? value : "(null)");
    exit(2);
  }
  opt->stop_after = stage;
  opt->stop_after_raw = value;
  opt->emit_only = true;
  opt->run_jit = false;
}

static void ny_set_type_solver_or_die(ny_options *opt, const char *value,
                                      const char *argv0) {
  (void)argv0;
  ny_type_solver_t solver = NY_TYPE_SOLVER_AUTO;
  if (!ny_parse_type_solver(value, &solver)) {
    fprintf(stderr, "ny: invalid --type-solver '%s' (expected auto|hm|z3)\n",
            value ? value : "(null)");
    exit(2);
  }
  opt->type_solver = solver;
  opt->type_solver_raw = value;
}

static void ny_set_proof_solver_or_die(ny_options *opt, const char *value,
                                       const char *argv0) {
  ny_proof_solver_t solver = NY_PROOF_SOLVER_AUTO;
  if (!ny_parse_proof_solver(value, &solver)) {
    fprintf(stderr,
            "invalid proof solver: %s (expected none|presburger|z3|auto)\n",
            value ? value : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  opt->proof_solver = solver;
  opt->proof_solver_raw = value;
}

static const char *ny_options_choice_slice(const char *value, size_t length,
                                           const char *const *choices,
                                           size_t choice_count) {
  for (size_t i = 0; i < choice_count; ++i)
    if (strlen(choices[i]) == length && memcmp(value, choices[i], length) == 0)
      return choices[i];
  return NULL;
}

static void ny_options_apply_gpu_opt(ny_options *opt, const char *items,
                                     const char *argv0) {
  static const char *const offload_modes[] = {"off", "auto", "on", "force"};
  static const char *const accel_targets[] = {"none",   "auto",  "nvptx",
                                              "amdgpu", "spirv", "hsaco"};
  static const char *const accel_objects[] = {"auto", "none", "ptx",
                                              "o",    "spv",  "hsaco"};
  static const char expected[] =
      "offload=off|auto|on|force,min-work=N,async,no-async,fast-math,"
      "no-fast-math,target=none|auto|nvptx|amdgpu|spirv|hsaco,"
      "object=auto|none|ptx|o|spv|hsaco";
  const char *item = items ? items : "";
  if (!item || !*item)
    goto invalid;
  while (*item) {
    const char *end = strchr(item, ',');
    size_t length = end ? (size_t)(end - item) : strlen(item);
    const char *value = NULL;
    if (length == 5 && memcmp(item, "async", length) == 0) {
      opt->gpu_async = 1;
    } else if (length == 8 && memcmp(item, "no-async", length) == 0) {
      opt->gpu_async = 0;
    } else if (length == 9 && memcmp(item, "fast-math", length) == 0) {
      opt->gpu_fast_math = 1;
    } else if (length == 12 && memcmp(item, "no-fast-math", length) == 0) {
      opt->gpu_fast_math = 0;
    } else if (length > 8 && memcmp(item, "offload=", 8) == 0) {
      value = ny_options_choice_slice(item + 8, length - 8, offload_modes,
                                      sizeof(offload_modes) /
                                          sizeof(offload_modes[0]));
      if (!value)
        goto invalid;
      opt->gpu_offload = value;
    } else if (length > 9 && memcmp(item, "min-work=", 9) == 0) {
      char number[32];
      size_t number_length = length - 9;
      if (number_length >= sizeof(number))
        goto invalid;
      memcpy(number, item + 9, number_length);
      number[number_length] = '\0';
      if (!ny_parse_nonneg_int(number, &opt->gpu_min_work))
        goto invalid;
    } else if (length > 7 && memcmp(item, "target=", 7) == 0) {
      value = ny_options_choice_slice(item + 7, length - 7, accel_targets,
                                      sizeof(accel_targets) /
                                          sizeof(accel_targets[0]));
      if (!value)
        goto invalid;
      ny_set_accel_target_or_die(opt, value, argv0, "--gpu-opt target");
    } else if (length > 7 && memcmp(item, "object=", 7) == 0) {
      value = ny_options_choice_slice(item + 7, length - 7, accel_objects,
                                      sizeof(accel_objects) /
                                          sizeof(accel_objects[0]));
      if (!value)
        goto invalid;
      ny_set_accel_object_or_die(opt, value, argv0);
    } else {
      goto invalid;
    }
    if (!end)
      break;
    item = end + 1;
    if (!*item)
      goto invalid;
  }
  return;

invalid:
  fprintf(stderr, "ny: invalid --gpu-opt item '%.*s' (expected %s)\n",
          (int)(strchr(item, ',') ? (size_t)(strchr(item, ',') - item)
                                  : strlen(item)),
          item, expected);
  exit(2);
}

#define NY_TRACE_LIST_VALUES                                  \
  "tokens|parse|scope|hm|unify|trait|llvm|regalloc|exec|meta"
#define NY_DUMP_LIST_VALUES                                      \
  "ast|tokens|llvm|llvm-passes|nyr|vm|funcs|escapes|proofs|obj|" \
  "callgraph|abi|stack|regalloc|elf|expand"

static void ny_options_apply_trace_list(ny_options *opt, const char *items) {
  static const char expected[] = NY_TRACE_LIST_VALUES;
  const char *item = items ? items : "";
  if (!item || !*item)
    goto invalid;
  while (*item) {
    const char *end = strchr(item, ',');
    size_t length = end ? (size_t)(end - item) : strlen(item);
#define NY_TRACE_ITEM(name, field)                                             \
  if (length == sizeof(name) - 1 && memcmp(item, name, sizeof(name) - 1) == 0) \
  opt->field = true
    NY_TRACE_ITEM("tokens", trace_tokens);
    else NY_TRACE_ITEM("parse", trace_parse);
    else NY_TRACE_ITEM("scope", trace_scope);
    else NY_TRACE_ITEM("hm", trace_hm);
    else NY_TRACE_ITEM("unify", dbg_unify_trace);
    else NY_TRACE_ITEM("trait", dbg_trait_resolve);
    else NY_TRACE_ITEM("llvm", trace_llvm);
    else NY_TRACE_ITEM("regalloc", trace_regalloc);
    else NY_TRACE_ITEM("exec", trace_exec);
    else NY_TRACE_ITEM("meta", meta_trace);
    else goto invalid;
#undef NY_TRACE_ITEM
    if (!end)
      break;
    item = end + 1;
    if (!*item)
      goto invalid;
  }
  if (opt->meta_trace)
    opt->expand = true;
  return;

invalid:
  fprintf(stderr, "ny: invalid --trace item '%.*s' (expected %s)\n",
          (int)(strchr(item, ',') ? (size_t)(strchr(item, ',') - item)
                                  : strlen(item)),
          item ? item : "", expected);
  exit(2);
}

static bool ny_options_apply_dump_item(ny_options *opt, const char *item,
                                       size_t length) {
#define NY_DUMP_ITEM(name, body)                   \
  if (length == sizeof(name) - 1 &&                \
      memcmp(item, name, sizeof(name) - 1) == 0) { \
    body;                                          \
    return true;                                   \
  }
  NY_DUMP_ITEM("ast", opt->dump_ast = true)
  NY_DUMP_ITEM("tokens", opt->dump_tokens = true)
  NY_DUMP_ITEM("llvm", opt->dump_llvm = true)
  NY_DUMP_ITEM("llvm-passes", opt->dump_llvm_passes = true)
  NY_DUMP_ITEM("nyr", {
    opt->native_dump_ir = true;
    opt->nyr_dump_text = true;
  })
  NY_DUMP_ITEM("vm", {
    opt->native_dump_ir = true;
    opt->vm_disasm = true;
  })
  NY_DUMP_ITEM("funcs", opt->dump_funcs = true)
  NY_DUMP_ITEM("escapes", opt->dump_escapes = true)
  NY_DUMP_ITEM("proofs", opt->dump_proofs = true)
  NY_DUMP_ITEM("obj", opt->dump_obj_full = true)
  NY_DUMP_ITEM("callgraph", opt->callgraph = true)
  NY_DUMP_ITEM("abi", opt->dbg_abi = true)
  NY_DUMP_ITEM("stack", opt->dbg_stack_layout = true)
  NY_DUMP_ITEM("regalloc", opt->dbg_regalloc_heat = true)
  NY_DUMP_ITEM("elf", opt->dbg_elf_dump = true)
  NY_DUMP_ITEM("expand", opt->expand = true)
#undef NY_DUMP_ITEM
  return false;
}

static void ny_options_apply_dump_list(ny_options *opt, const char *items) {
  static const char expected[] = NY_DUMP_LIST_VALUES;
  const char *item = items ? items : "";
  if (!*item)
    goto invalid;
  while (*item) {
    const char *end = strchr(item, ',');
    size_t length = end ? (size_t)(end - item) : strlen(item);
    if (!ny_options_apply_dump_item(opt, item, length))
      goto invalid;
    if (!end)
      break;
    item = end + 1;
    if (!*item)
      goto invalid;
  }
  return;

invalid:
  fprintf(stderr, "ny: invalid --dump item '%.*s' (expected %s)\n",
          (int)(strchr(item, ',') ? (size_t)(strchr(item, ',') - item)
                                  : strlen(item)),
          item, expected);
  exit(2);
}

typedef struct ny_option_owned_string {
  struct ny_option_owned_string *next;
  char value[];
} ny_option_owned_string_t;

static char *ny_options_copy_owned_value(ny_options *opt, const char *value,
                                         size_t length) {
  ny_option_owned_string_t *storage = malloc(sizeof(*storage) + length + 1);
  if (!storage) {
    fprintf(stderr, "ny: out of memory parsing compiler option list\n");
    exit(1);
  }
  storage->next = (ny_option_owned_string_t *)opt->owned_option_values;
  memcpy(storage->value, value, length);
  storage->value[length] = '\0';
  opt->owned_option_values = storage;
  return storage->value;
}

static void ny_options_apply_use_list(ny_options *opt, const char *items,
                                      const char *argv0) {
  const char *source = items ? items : "";
  if (!*source) {
    fprintf(stderr, "ny: invalid --use value '' "
                    "(expected artifact=PATH|refinement-interface=PATH|"
                    "effect-interface=PATH|native-bundle=PATH)\n");
    exit(2);
  }
  char *item = ny_options_copy_owned_value(opt, source, strlen(source));
  while (*item) {
    char *end = strchr(item, ',');
    if (end)
      *end = '\0';
    char *equals = strchr(item, '=');
    if (!equals || equals == item || !equals[1])
      goto invalid;
    size_t key_length = (size_t)(equals - item);
    *equals = '\0';
    const char *path = equals + 1;
    if (key_length == sizeof("artifact") - 1 &&
        memcmp(item, "artifact", key_length) == 0) {
      opt->use_artifact_path = path;
    } else if (key_length == sizeof("refinement-interface") - 1 &&
               memcmp(item, "refinement-interface", key_length) == 0) {
      opt->use_refinement_interface_path = path;
    } else if (key_length == sizeof("effect-interface") - 1 &&
               memcmp(item, "effect-interface", key_length) == 0) {
      opt->use_effect_interface_path = path;
    } else if (key_length == sizeof("native-bundle") - 1 &&
               memcmp(item, "native-bundle", key_length) == 0) {
#if defined(__x86_64__) || defined(_M_X64)
      if (!opt->native_backend_explicit)
        ny_set_native_backend_or_die(opt, "x86_64", argv0);
#else
      fprintf(
          stderr,
          "ny: --use=native-bundle is currently supported on x86-64 hosts\n");
      exit(2);
#endif
      opt->native_from_nyir_path = path;
      opt->native_only = true;
      opt->emit_only = true;
      opt->run_aot = false;
      opt->run_jit = false;
    } else {
      goto invalid;
    }
    if (!end)
      return;
    item = end + 1;
    if (!*item)
      goto invalid;
  }
  return;

invalid:
  fprintf(stderr,
          "ny: invalid --use item '%s' "
          "(expected artifact=PATH|refinement-interface=PATH|"
          "effect-interface=PATH|native-bundle=PATH)\n",
          item);
  exit(2);
}

static void ny_options_apply_nyr_limit_list(ny_options *opt, const char *items,
                                            const char *argv0) {
  const char *item = items ? items : "";
  if (!*item)
    goto invalid;
  while (*item) {
    const char *end = strchr(item, ',');
    size_t length = end ? (size_t)(end - item) : strlen(item);
    const char *equals = memchr(item, '=', length);
    if (!equals || equals == item || equals == item + length - 1)
      goto invalid;
    size_t key_length = (size_t)(equals - item);
    size_t value_length = length - key_length - 1;
    char value_storage[128];
    if (value_length >= sizeof(value_storage))
      goto invalid;
    memcpy(value_storage, equals + 1, value_length);
    value_storage[value_length] = '\0';
    const char *value = value_storage;
    if (key_length == sizeof("global-inst-cap") - 1 &&
        memcmp(item, "global-inst-cap", key_length) == 0) {
      opt->nyr_global_inst_cap = (size_t)ny_parse_nonneg_int_or_die(
          value, "nyr-global-inst-cap", argv0);
    } else if (key_length == sizeof("pass-growth") - 1 &&
               memcmp(item, "pass-growth", key_length) == 0) {
      opt->nyr_pass_growth_factor =
          ny_parse_nonneg_f64_or_die(value, "nyr-pass-growth-factor", argv0);
    } else if (key_length == sizeof("run-steps") - 1 &&
               memcmp(item, "run-steps", key_length) == 0) {
      opt->nyr_run_max_steps =
          ny_parse_nonneg_int_or_die(value, "NYR VM max steps", argv0);
      opt->native_dump_ir = true;
      opt->nyr_run = true;
    } else if (key_length == sizeof("run-recursion") - 1 &&
               memcmp(item, "run-recursion", key_length) == 0) {
      opt->nyr_run_recursion_limit =
          ny_parse_nonneg_int_or_die(value, "NYR VM recursion limit", argv0);
      opt->native_dump_ir = true;
      opt->nyr_run = true;
    } else {
      goto invalid;
    }
    if (!end)
      return;
    item = end + 1;
    if (!*item)
      goto invalid;
  }
  return;

invalid:
  fprintf(stderr,
          "ny: invalid --nyr-limit item '%.*s' "
          "(expected global-inst-cap=N|pass-growth=X|run-steps=N|"
          "run-recursion=N)\n",
          (int)(strchr(item, ',') ? (size_t)(strchr(item, ',') - item)
                                  : strlen(item)),
          item);
  exit(2);
}

static void ny_options_apply_jit_opt_list(ny_options *opt, const char *items) {
  const char *item = items ? items : "";
  if (!*item)
    goto invalid;
  while (*item) {
    const char *end = strchr(item, ',');
    size_t length = end ? (size_t)(end - item) : strlen(item);
    if (length == sizeof("fast-isel") - 1 &&
        memcmp(item, "fast-isel", length) == 0) {
      opt->jit_fast_isel = true;
    } else if (length == sizeof("no-fast-isel") - 1 &&
               memcmp(item, "no-fast-isel", length) == 0) {
      opt->jit_fast_isel = false;
    } else if (length == sizeof("perf-map") - 1 &&
               memcmp(item, "perf-map", length) == 0) {
      opt->jit_perf_map = true;
    } else if (length == sizeof("no-perf-map") - 1 &&
               memcmp(item, "no-perf-map", length) == 0) {
      opt->jit_perf_map = false;
    } else {
      const char *equals = memchr(item, '=', length);
      if (!equals || equals == item || equals == item + length - 1)
        goto invalid;
      size_t key_length = (size_t)(equals - item);
      size_t value_length = length - key_length - 1;
      if (key_length == sizeof("engine") - 1 &&
          memcmp(item, "engine", key_length) == 0) {
        char *value_storage =
            ny_options_copy_owned_value(opt, equals + 1, value_length);
        if (strcmp(value_storage, "orc") != 0 &&
            strcmp(value_storage, "mcjit") != 0)
          goto invalid;
        opt->jit_engine = value_storage;
      } else if (key_length == sizeof("opt-level") - 1 &&
                 memcmp(item, "opt-level", key_length) == 0) {
        char value_storage[32];
        if (value_length >= sizeof(value_storage))
          goto invalid;
        memcpy(value_storage, equals + 1, value_length);
        value_storage[value_length] = '\0';
        int level = 0;
        if (!ny_parse_nonneg_int(value_storage, &level) || level > 3)
          goto invalid;
        opt->jit_opt_level = level;
        opt->jit_opt_level_explicit = true;
      } else if (key_length == sizeof("code-model") - 1 &&
                 memcmp(item, "code-model", key_length) == 0) {
        char *value_storage =
            ny_options_copy_owned_value(opt, equals + 1, value_length);
        if (strcmp(value_storage, "small") != 0 &&
            strcmp(value_storage, "kernel") != 0 &&
            strcmp(value_storage, "medium") != 0 &&
            strcmp(value_storage, "large") != 0)
          goto invalid;
        opt->jit_code_model = value_storage;
      } else {
        goto invalid;
      }
    }
    if (!end)
      return;
    item = end + 1;
    if (!*item)
      goto invalid;
  }
  return;

invalid:
  fprintf(stderr,
          "ny: invalid --jit-opt item '%.*s' "
          "(expected engine=orc|engine=mcjit|fast-isel|no-fast-isel|"
          "opt-level=0..3|code-model=small|kernel|medium|large|"
          "perf-map|no-perf-map)\n",
          (int)(strchr(item, ',') ? (size_t)(strchr(item, ',') - item)
                                  : strlen(item)),
          item);
  exit(2);
}

static void ny_options_apply_nyr_check_list(ny_options *opt,
                                            const char *items) {
  const char *item = items ? items : "";
  if (!*item)
    goto invalid;
  while (*item) {
    const char *end = strchr(item, ',');
    size_t length = end ? (size_t)(end - item) : strlen(item);
    if (length == sizeof("verify") - 1 && memcmp(item, "verify", length) == 0) {
      opt->nyr_ir_verify = true;
    } else if (length == sizeof("bisect") - 1 &&
               memcmp(item, "bisect", length) == 0) {
      opt->nyr_bisect = true;
      opt->nyr_ir_verify = true;
    } else if (length == sizeof("rollback") - 1 &&
               memcmp(item, "rollback", length) == 0) {
      opt->nyr_rollback_on_verify_fail = true;
      opt->nyr_ir_verify = true;
    } else if (length == sizeof("flaky") - 1 &&
               memcmp(item, "flaky", length) == 0) {
      opt->nyr_flaky_check = true;
    } else if (length == sizeof("fingerprint") - 1 &&
               memcmp(item, "fingerprint", length) == 0) {
      opt->nyr_fingerprint = true;
    } else if (length == sizeof("oracle") - 1 &&
               memcmp(item, "oracle", length) == 0) {
      opt->native_result_oracle = true;
      opt->native_backend = NY_NATIVE_BACKEND_X86_64;
      opt->native_backend_raw = "x86_64";
    } else if (length == sizeof("oracle-per-pass") - 1 &&
               memcmp(item, "oracle-per-pass", length) == 0) {
      opt->native_oracle_per_pass = true;
      opt->native_result_oracle = true;
      opt->native_backend = NY_NATIVE_BACKEND_X86_64;
      opt->native_backend_raw = "x86_64";
    } else {
      const char *equals = memchr(item, '=', length);
      if (!equals || equals == item || equals == item + length - 1)
        goto invalid;
      size_t key_length = (size_t)(equals - item);
      size_t value_length = length - key_length - 1;
      char value_storage[32];
      if (value_length >= sizeof(value_storage))
        goto invalid;
      memcpy(value_storage, equals + 1, value_length);
      value_storage[value_length] = '\0';
      int value = 0;
      if (!ny_parse_nonneg_int(value_storage, &value))
        goto invalid;
      if (key_length == sizeof("tv-seed") - 1 &&
          memcmp(item, "tv-seed", key_length) == 0) {
        opt->native_tv_seed_trials = value;
      } else if (key_length == sizeof("fuzz") - 1 &&
                 memcmp(item, "fuzz", key_length) == 0) {
        opt->native_oracle_fuzz_count = value;
        opt->native_result_oracle = true;
        opt->native_backend = NY_NATIVE_BACKEND_X86_64;
        opt->native_backend_raw = "x86_64";
      } else {
        goto invalid;
      }
    }
    if (!end)
      return;
    item = end + 1;
    if (!*item)
      goto invalid;
  }
  return;

invalid:
  fprintf(stderr,
          "ny: invalid --nyr-check item '%.*s' "
          "(expected verify|bisect|rollback|flaky|fingerprint|oracle|"
          "oracle-per-pass|tv-seed=N|fuzz=N)\n",
          (int)(strchr(item, ',') ? (size_t)(strchr(item, ',') - item)
                                  : strlen(item)),
          item);
  exit(2);
}

static void ny_options_apply_native_tuning(ny_options *opt, const char *items,
                                           const char *argv0) {
  const char *item = items ? items : "";
  if (!*item)
    goto invalid;
  while (*item) {
    const char *end = strchr(item, ',');
    size_t length = end ? (size_t)(end - item) : strlen(item);
    const char *equals = memchr(item, '=', length);
    if (!equals || equals == item || equals == item + length - 1)
      goto invalid;
    size_t key_length = (size_t)(equals - item);
    size_t value_length = length - key_length - 1;
    char value_storage[32];
    if (value_length >= sizeof(value_storage))
      goto invalid;
    memcpy(value_storage, equals + 1, value_length);
    value_storage[value_length] = '\0';
    int value;
    if (key_length == sizeof("prefer") - 1 &&
        memcmp(item, "prefer", key_length) == 0) {
      if (strcmp(value_storage, "vm") == 0) {
        opt->native_prefer_vm = true;
        opt->native_prefer_asm = false;
      } else if (strcmp(value_storage, "asm") == 0) {
        opt->native_prefer_asm = true;
        opt->native_prefer_vm = false;
      } else {
        goto invalid;
      }
    } else {
      if (!ny_parse_nonneg_int(value_storage, &value))
        goto invalid;
      if (key_length == sizeof("budget") - 1 &&
          memcmp(item, "budget", key_length) == 0) {
        opt->native_tier_budget = value;
      } else if (key_length == sizeof("hot") - 1 &&
                 memcmp(item, "hot", key_length) == 0) {
        opt->native_hot_threshold = value;
      } else if (key_length == sizeof("cold") - 1 &&
                 memcmp(item, "cold", key_length) == 0) {
        opt->native_cold_threshold = value;
      } else if (key_length == sizeof("cache-score") - 1 &&
                 memcmp(item, "cache-score", key_length) == 0) {
        opt->native_cache_score = value;
      } else {
        goto invalid;
      }
    }
    if (!end)
      return;
    item = end + 1;
    if (!*item)
      goto invalid;
  }
  return;

invalid:
  fprintf(stderr,
          "ny: invalid --native-tuning item '%.*s' "
          "(expected budget=N|hot=N|cold=N|cache-score=N|prefer=vm|asm)\n",
          (int)(strchr(item, ',') ? (size_t)(strchr(item, ',') - item)
                                  : strlen(item)),
          item);
  (void)argv0;
  exit(2);
}

static void ny_options_apply_dwarf_list(ny_options *opt, const char *items) {
  const char *item = items ? items : "";
  if (!*item)
    goto invalid;
  while (*item) {
    const char *end = strchr(item, ',');
    size_t length = end ? (size_t)(end - item) : strlen(item);
    const char *equals = memchr(item, '=', length);
    if (equals && equals != item && equals < item + length - 1 &&
        (size_t)(equals - item) == sizeof("version") - 1 &&
        memcmp(item, "version", sizeof("version") - 1) == 0) {
      char version[16];
      size_t version_length = length - (sizeof("version=") - 1);
      if (version_length >= sizeof(version))
        goto invalid;
      memcpy(version, equals + 1, version_length);
      version[version_length] = '\0';
      int parsed = 0;
      if (!ny_parse_nonneg_int(version, &parsed) || parsed < 2 || parsed > 5)
        goto invalid;
      opt->dwarf_version = parsed;
    } else if (length == sizeof("locals") - 1 &&
               memcmp(item, "locals", length) == 0) {
      opt->debug_locals = 1;
    } else if (length == sizeof("no-locals") - 1 &&
               memcmp(item, "no-locals", length) == 0) {
      opt->debug_locals = 0;
    } else if (length == sizeof("split-inlining") - 1 &&
               memcmp(item, "split-inlining", length) == 0) {
      opt->dwarf_split_inlining = 1;
    } else if (length == sizeof("no-split-inlining") - 1 &&
               memcmp(item, "no-split-inlining", length) == 0) {
      opt->dwarf_split_inlining = 0;
    } else if (length == sizeof("profile-info") - 1 &&
               memcmp(item, "profile-info", length) == 0) {
      opt->dwarf_profile_info = 1;
    } else if (length == sizeof("no-profile-info") - 1 &&
               memcmp(item, "no-profile-info", length) == 0) {
      opt->dwarf_profile_info = 0;
    } else {
      goto invalid;
    }
    if (!end)
      return;
    item = end + 1;
    if (!*item)
      goto invalid;
  }
  return;

invalid:
  fprintf(stderr,
          "ny: invalid --dwarf item '%.*s' "
          "(expected version=2..5|locals|no-locals|split-inlining|"
          "no-split-inlining|profile-info|no-profile-info)\n",
          (int)(strchr(item, ',') ? (size_t)(strchr(item, ',') - item)
                                  : strlen(item)),
          item);
  exit(2);
}

static void ny_options_apply_nyr_pass_list(ny_options *opt, const char *items) {
  const char *item = items ? items : "";
  if (!*item)
    goto invalid;
  while (*item) {
    const char *end = strchr(item, ',');
    size_t length = end ? (size_t)(end - item) : strlen(item);
    const char *colon = memchr(item, ':', length);
    if (!colon || colon == item || colon == item + length - 1)
      goto invalid;
    size_t key_length = (size_t)(colon - item);
    size_t name_length = length - key_length - 1;
    char *name = ny_options_copy_owned_value(opt, colon + 1, name_length);
    if (key_length == sizeof("disable") - 1 &&
        memcmp(item, "disable", key_length) == 0) {
      opt->nyr_disable_pass = name;
    } else if (key_length == sizeof("stop-after") - 1 &&
               memcmp(item, "stop-after", key_length) == 0) {
      opt->nyr_stop_after = name;
    } else {
      goto invalid;
    }
    if (!end)
      return;
    item = end + 1;
    if (!*item)
      goto invalid;
  }
  return;

invalid:
  fprintf(stderr,
          "ny: invalid --nyr-pass item '%.*s' "
          "(expected disable:NAME|stop-after:NAME)\n",
          (int)(strchr(item, ',') ? (size_t)(strchr(item, ',') - item)
                                  : strlen(item)),
          item);
  exit(2);
}

static const char ny_emit_default_path_marker[] = "\x1fny-default";

static bool ny_emit_assign(ny_options *opt, const char *kind, size_t kind_len,
                           const char *path, size_t path_len) {
#define NY_EMIT_KIND(name)                                                    \
  (kind_len == sizeof(name) - 1 && memcmp(kind, name, sizeof(name) - 1) == 0)
  bool known =
      NY_EMIT_KIND("asm") || NY_EMIT_KIND("ir") || NY_EMIT_KIND("bc") ||
      NY_EMIT_KIND("obj") || NY_EMIT_KIND("wasm") || NY_EMIT_KIND("nyr") ||
      NY_EMIT_KIND("nyr-metadata") || NY_EMIT_KIND("report") ||
      NY_EMIT_KIND("artifact") || NY_EMIT_KIND("refinement-interface") ||
      NY_EMIT_KIND("effect-interface") || NY_EMIT_KIND("shapes") ||
      NY_EMIT_KIND("native-bundle");
  bool path_forbidden = NY_EMIT_KIND("shapes");
  if (!known || (path_forbidden && path))
    return false;
  const char *owned_path =
      path ? ny_options_copy_owned_value(opt, path, path_len)
           : ny_emit_default_path_marker;
  if (NY_EMIT_KIND("asm")) {
    opt->emit_asm_path = owned_path;
  } else if (NY_EMIT_KIND("ir")) {
    opt->emit_ir_path = owned_path;
  } else if (NY_EMIT_KIND("bc")) {
    opt->emit_bc_path = owned_path;
  } else if (NY_EMIT_KIND("obj")) {
    opt->output_file = owned_path;
  } else if (NY_EMIT_KIND("wasm")) {
    opt->emit_wasm_path = owned_path;
  } else if (NY_EMIT_KIND("nyr")) {
    opt->native_dump_ir = true;
    opt->nyr_dump_text = true;
    if (path)
      opt->native_dump_ir_path = owned_path;
  } else if (NY_EMIT_KIND("nyr-metadata")) {
    opt->native_dump_ir = true;
    opt->nyr_metadata_report = true;
    if (path)
      opt->nyr_metadata_report_path = owned_path;
  } else if (NY_EMIT_KIND("report")) {
    opt->emit_report_path = owned_path;
    opt->native_tier_report = true;
    opt->native_tier_report_path = owned_path;
    opt->do_timing = true;
    opt->profiler_mode = true;
    opt->profiler_output_mode = "verbose";
    opt->trace_exec = true;
    if (!opt->native_backend_explicit) {
      opt->native_backend = NY_NATIVE_BACKEND_X86_64;
      opt->native_backend_raw = "x86_64";
    }
  } else if (NY_EMIT_KIND("artifact")) {
    opt->emit_artifact_path = owned_path;
  } else if (NY_EMIT_KIND("refinement-interface")) {
    opt->emit_refinement_interface_path = owned_path;
  } else if (NY_EMIT_KIND("effect-interface")) {
    opt->emit_effect_interface_path = owned_path;
  } else if (NY_EMIT_KIND("shapes")) {
    opt->emit_shapes = true;
  } else if (NY_EMIT_KIND("native-bundle")) {
#if defined(__x86_64__) || defined(_M_X64)
    opt->native_backend = NY_NATIVE_BACKEND_X86_64;
    opt->native_backend_raw = "x86_64";
    opt->native_backend_explicit = true;
#else
    return false;
#endif
    opt->native_only = true;
    opt->native_dump_ir = true;
    opt->nyr_dump_bin = true;
    opt->nyr_dump_bin_path = owned_path;
    opt->emit_only = true;
    opt->run_aot = false;
    opt->run_jit = false;
  } else {
    return false;
  }
#undef NY_EMIT_KIND
  return true;
}

static bool ny_options_apply_legacy_emit_path(ny_options *opt, const char *kind,
                                              const char *path) {
  return ny_emit_assign(opt, kind, strlen(kind), path, path ? strlen(path) : 0);
}

static void ny_options_apply_emit_list(ny_options *opt, const char *items) {
  const char *item = items ? items : "";
  if (!*item)
    goto invalid;
  while (*item) {
    const char *end = strchr(item, ',');
    size_t length = end ? (size_t)(end - item) : strlen(item);
    const char *equals = memchr(item, '=', length);
    size_t kind_len = equals ? (size_t)(equals - item) : length;
    const char *path = equals ? equals + 1 : NULL;
    size_t path_len = equals ? length - kind_len - 1 : 0;
    if (equals && (!path_len || memchr(path, ',', path_len)))
      goto invalid;
    if (!ny_emit_assign(opt, item, kind_len, path, path_len))
      goto invalid;
    if (!end)
      return;
    item = end + 1;
    if (!*item)
      goto invalid;
  }

invalid:
  fprintf(stderr,
          "ny: invalid --emit item '%.*s' "
          "(expected asm[=PATH]|ir[=PATH]|bc[=PATH]|obj[=PATH]|wasm[=PATH]|"
          "nyr|nyr-metadata[=PATH]|report[=PATH]|artifact[=PATH]|"
          "refinement-interface[=PATH]|effect-interface[=PATH]|shapes|"
          "native-bundle[=PATH])\n",
          (int)(strchr(item, ',') ? (size_t)(strchr(item, ',') - item)
                                  : strlen(item)),
          item);
  exit(2);
}

static bool ny_options_apply_common_codegen_option(ny_options *opt,
                                                   const char *a, int *i,
                                                   int argc, char **argv,
                                                   const char *argv0) {
  const char *value = NULL;
  if ((value = ny_option_value_or_die(a, "--emit", i, argc, argv, argv0)) !=
      NULL) {
    ny_options_apply_emit_list(opt, value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-tuning", i, argc, argv,
                                      argv0)) != NULL) {
    ny_options_apply_native_tuning(opt, value, argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--dwarf", i, argc, argv, argv0)) !=
      NULL) {
    ny_options_apply_dwarf_list(opt, value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-pass", i, argc, argv, argv0)) !=
      NULL) {
    ny_options_apply_nyr_pass_list(opt, value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-check", i, argc, argv,
                                      argv0)) != NULL) {
    ny_options_apply_nyr_check_list(opt, value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--jit-opt", i, argc, argv, argv0)) !=
      NULL) {
    ny_options_apply_jit_opt_list(opt, value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-limit", i, argc, argv,
                                      argv0)) != NULL) {
    ny_options_apply_nyr_limit_list(opt, value, argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--use", i, argc, argv, argv0)) !=
      NULL) {
    ny_options_apply_use_list(opt, value, argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--dump", i, argc, argv, argv0)) !=
      NULL) {
    ny_options_apply_dump_list(opt, value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--trace", i, argc, argv, argv0)) !=
      NULL) {
    ny_options_apply_trace_list(opt, value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--gpu-opt", i, argc, argv, argv0)) !=
      NULL) {
    ny_options_apply_gpu_opt(opt, value, argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-from-nyr", i, argc, argv,
                                      argv0)) != NULL) {
    if (!opt->native_backend_explicit)
      ny_set_native_backend_or_die(opt, "x86_64", argv0);
    opt->native_from_nyir_path = value;
    opt->native_only = true;
    opt->emit_only = true;
    opt->run_aot = false;
    opt->run_jit = false;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-precompile", i, argc, argv,
                                      argv0)) != NULL) {
#if defined(__x86_64__) || defined(_M_X64)
    ny_set_native_backend_or_die(opt, "x86_64", argv0);
#else
    fprintf(stderr,
            "--native-precompile is currently supported on x86-64 hosts\n");
    exit(1);
#endif
    opt->native_only = true;
    opt->native_dump_ir = true;
    opt->nyr_dump_bin = true;
    opt->nyr_dump_bin_path = value;
    opt->emit_only = true;
    opt->run_aot = false;
    opt->run_jit = false;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--output", i, argc, argv, argv0)) !=
          NULL ||
      (value = ny_option_value_or_die(a, "-o", i, argc, argv, argv0)) != NULL) {
    opt->output_file = value;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--stop-after", i, argc, argv,
                                      argv0)) != NULL) {
    ny_set_stop_after_or_die(opt, value, argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--type-solver", i, argc, argv,
                                      argv0)) != NULL) {
    ny_set_type_solver_or_die(opt, value, argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--proof-solver", i, argc, argv,
                                      argv0)) != NULL) {
    ny_set_proof_solver_or_die(opt, value, argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-backend", i, argc, argv,
                                      argv0)) != NULL) {
    ny_set_native_backend_or_die(opt, value, argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-abi", i, argc, argv,
                                      argv0)) != NULL) {
    ny_set_native_abi_or_die(opt, value, argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-tier", i, argc, argv,
                                      argv0)) != NULL) {
    ny_set_native_tier_or_die(opt, value, argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-tier-budget", i, argc, argv,
                                      argv0)) != NULL) {
    opt->native_tier_budget =
        ny_parse_nonneg_int_or_die(value, "native tier budget", argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--comptime-limit", i, argc, argv,
                                      argv0)) != NULL) {
    int limit = ny_parse_nonneg_int_or_die(value, "comptime limit", argv0);
    if (limit <= 0) {
      fprintf(stderr,
              "invalid comptime limit: %s (expected a positive integer)\n",
              value);
      ny_options_usage(argv0);
      exit(1);
    }
    char fuel[32];
    snprintf(fuel, sizeof(fuel), "%d", limit);
    ny_setenv("NYTRIX_COMPTIME_FUEL", fuel, 1);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-hot-threshold", i, argc,
                                      argv, argv0)) != NULL) {
    opt->native_hot_threshold =
        ny_parse_nonneg_int_or_die(value, "native hot threshold", argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-cold-threshold", i, argc,
                                      argv, argv0)) != NULL) {
    opt->native_cold_threshold =
        ny_parse_nonneg_int_or_die(value, "native cold threshold", argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-cache-score", i, argc, argv,
                                      argv0)) != NULL) {
    opt->native_cache_score =
        ny_parse_nonneg_int_or_die(value, "native cache score", argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--watch-poll", i, argc, argv,
                                      argv0)) != NULL) {
    int poll_ms =
        ny_parse_nonneg_int_or_die(value, "watch poll interval", argv0);
    if (poll_ms > 60000) {
      fprintf(stderr, "invalid watch poll interval: %d (max 60000 ms)\n",
              poll_ms);
      ny_options_usage(argv0);
      exit(1);
    }
    opt->watch_poll_ms = poll_ms;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--jit-opt-level", i, argc, argv,
                                      argv0)) != NULL) {
    int lvl = ny_parse_nonneg_int_or_die(value, "JIT opt level", argv0);
    if (lvl > 3) {
      fprintf(stderr, "ny: invalid --jit-opt-level '%d' (expected 0|1|2|3)\n",
              lvl);
      exit(2);
    }
    opt->jit_opt_level = lvl;
    opt->jit_opt_level_explicit = true;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--jit-code-model", i, argc, argv,
                                      argv0)) != NULL) {
    if (strcmp(value, "small") != 0 && strcmp(value, "kernel") != 0 &&
        strcmp(value, "medium") != 0 && strcmp(value, "large") != 0) {
      fprintf(stderr,
              "ny: invalid --jit-code-model '%s' (expected "
              "small|kernel|medium|large)\n",
              value);
      exit(2);
    }
    opt->jit_code_model = value;
    return true;
  }
  if (strcmp(a, "--native-prefer-vm") == 0) {
    opt->native_prefer_vm = true;
    opt->native_prefer_asm = false;
    return true;
  }
  if (strcmp(a, "--native-prefer-asm") == 0) {
    opt->native_prefer_asm = true;
    opt->native_prefer_vm = false;
    return true;
  }
  if (strcmp(a, "--native-tier-report") == 0) {
    opt->native_tier_report = true;
    return true;
  }
  if (strcmp(a, "--callgraph") == 0) {
    opt->callgraph = true;
    opt->callgraph_depth = -1;
    return true;
  }
  if (strncmp(a, "--callgraph=", 12) == 0) {
    const char *spec = a + 12;
    opt->callgraph = true;
    opt->callgraph_depth = -1;
    if (strcmp(spec, "json") == 0) {
      opt->callgraph_json = true;
    } else if (strncmp(spec, "json:", 5) == 0) {
      opt->callgraph_json = true;
      opt->callgraph_depth =
          ny_parse_nonneg_int_or_die(spec + 5, "callgraph depth", argv0);
    } else {
      opt->callgraph_depth =
          ny_parse_nonneg_int_or_die(spec, "callgraph depth", argv0);
    }
    return true;
  }
  if (strncmp(a, "--native-tier-report=", 21) == 0) {
    opt->native_tier_report = true;
    opt->native_tier_report_path = a + 21;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--emit-report", i, argc, argv,
                                      argv0)) != NULL) {
    (void)ny_options_apply_legacy_emit_path(opt, "report", value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--perf", i, argc, argv, argv0)) !=
      NULL) {
    const char *mode = value;
    if (strcmp(mode, "compact") && strcmp(mode, "smart") &&
        strcmp(mode, "verbose")) {
      fprintf(stderr,
              "ny: invalid --perf '%s' (expected compact|smart|verbose)\n",
              mode);
      exit(2);
    }
    opt->profiler_output_mode = mode;
    opt->profiler_mode = true;
    opt->do_timing = true;
    opt->trace_exec = true;
    return true;
  }
  if (strncmp(a, "--prof=", 7) == 0 || strncmp(a, "-prof=", 6) == 0 ||
      ((strcmp(a, "--prof") == 0 || strcmp(a, "-prof") == 0) && *i + 1 < argc &&
       ny_prof_list_starts_with_mode(argv[*i + 1]))) {
    static const char expected[] = "timing|stats|gprof|compile|peak-rss";
    const char *item = strncmp(a, "--prof=", 7) == 0  ? a + 7
                       : strncmp(a, "-prof=", 6) == 0 ? a + 6
                                                      : argv[++(*i)];
    if (!*item) {
      fprintf(stderr, "ny: invalid --prof '' (expected %s)\n", expected);
      exit(2);
    }
    while (*item) {
      const char *end = strchr(item, ',');
      size_t length = end ? (size_t)(end - item) : strlen(item);
      if (length == 6 && strncmp(item, "timing", length) == 0) {
        opt->do_timing = true;
      } else if (length == 5 && strncmp(item, "stats", length) == 0) {
        opt->profiler_mode = true;
        opt->do_timing = true;
        opt->trace_exec = true;
        opt->profiler_output_mode = "smart";
      } else if (length == 5 && strncmp(item, "gprof", length) == 0) {
        opt->gprof = 1;
        opt->debug_symbols = true;
      } else if (length == 7 && strncmp(item, "compile", length) == 0) {
        opt->compile_profile_fn = true;
      } else if (length == 8 && strncmp(item, "peak-rss", length) == 0) {
        opt->report_peak_rss = true;
      } else {
        fprintf(stderr, "ny: invalid --prof '%.*s' (expected %s)\n",
                (int)length, item, expected);
        exit(2);
      }
      if (!end)
        break;
      item = end + 1;
      if (!*item) {
        fprintf(stderr, "ny: invalid --prof '' (expected %s)\n", expected);
        exit(2);
      }
    }
    return true;
  }
  if (strcmp(a, "--native-enable-cf-mem2reg") == 0) {
    opt->native_enable_cf_mem2reg = true;
    return true;
  }
  if (strcmp(a, "-flto") == 0 || strcmp(a, "--llvm-lto") == 0) {
    opt->llvm_lto = true;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--opt", i, argc, argv, argv0)) !=
      NULL) {
    static const char expected[] =
        "dce|loops|internalize|autotune|lto|fast-math|no-dce|no-loops|"
        "no-internalize|no-autotune|no-lto|no-fast-math";
    if (!*value) {
      fprintf(stderr, "ny: invalid --opt '' (expected %s)\n", expected);
      exit(2);
    }
    const char *item = value;
    while (*item) {
      const char *end = strchr(item, ',');
      size_t length = end ? (size_t)(end - item) : strlen(item);
      bool enabled = true;
      const char *name = item;
      if (length > 3 && strncmp(item, "no-", 3) == 0) {
        enabled = false;
        name += 3;
        length -= 3;
      }
#define NY_OPT_ITEM(spelling, field)                                      \
  (length == sizeof(spelling) - 1 && strncmp(name, spelling, length) == 0 \
       ? (opt->field = enabled, true)                                     \
       : false)
      bool matched =
          NY_OPT_ITEM("dce", opt_dce) || NY_OPT_ITEM("loops", opt_loops) ||
          NY_OPT_ITEM("internalize", opt_internalize) ||
          NY_OPT_ITEM("autotune", opt_autotune) ||
          NY_OPT_ITEM("lto", llvm_lto) || NY_OPT_ITEM("fast-math", fast_math);
#undef NY_OPT_ITEM
      if (!matched) {
        fprintf(stderr, "ny: invalid --opt '%.*s' (expected %s)\n", (int)length,
                name, expected);
        exit(2);
      }
      if (!end)
        break;
      item = end + 1;
      if (!*item) {
        fprintf(stderr, "ny: invalid --opt '' (expected %s)\n", expected);
        exit(2);
      }
    }
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--check", i, argc, argv, argv0)) !=
      NULL) {
    static const char expected[] =
        "types|effects|alias|alias-escape|ownership|shadow|no-types|"
        "no-effects|no-alias|no-alias-escape|no-ownership|no-shadow";
    if (!*value) {
      fprintf(stderr, "ny: invalid --check '' (expected %s)\n", expected);
      exit(2);
    }
    const char *item = value;
    while (*item) {
      const char *end = strchr(item, ',');
      size_t length = end ? (size_t)(end - item) : strlen(item);
      bool enabled = true;
      const char *name = item;
      if (length > 3 && strncmp(item, "no-", 3) == 0) {
        enabled = false;
        name += 3;
        length -= 3;
      }
#define NY_CHECK_ITEM(spelling, field)                                    \
  (length == sizeof(spelling) - 1 && strncmp(name, spelling, length) == 0 \
       ? (opt->field = enabled, true)                                     \
       : false)
      bool matched = NY_CHECK_ITEM("types", strict_types) ||
                     NY_CHECK_ITEM("effects", effect_require_known) ||
                     NY_CHECK_ITEM("alias", alias_require_known) ||
                     NY_CHECK_ITEM("alias-escape", alias_require_no_escape) ||
                     NY_CHECK_ITEM("ownership", ownership) ||
                     NY_CHECK_ITEM("shadow", warn_shadow);
#undef NY_CHECK_ITEM
      if (!matched) {
        fprintf(stderr, "ny: invalid --check '%.*s' (expected %s)\n",
                (int)length, name, expected);
        exit(2);
      }
      if (!end)
        break;
      item = end + 1;
      if (!*item) {
        fprintf(stderr, "ny: invalid --check '' (expected %s)\n", expected);
        exit(2);
      }
    }
    return true;
  }
  if (strcmp(a, "--native-disable-cf-mem2reg") == 0) {
    opt->native_enable_cf_mem2reg = false;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--c-frontend", i, argc, argv,
                                      argv0)) != NULL) {
    ny_set_c_frontend_or_die(opt, value, argv0);
    return true;
  }
  if (strcmp(a, "--inspect") == 0 || strcmp(a, "--inspect=program") == 0 ||
      strcmp(a, "--inspect=runtime") == 0) {
    opt->native_dump_ir = true;
    opt->nyr_dump_text = true;
    opt->nyr_dump_raw = true;
    opt->nyr_pass_stats = true;
    opt->dump_scope = strcmp(a, "--inspect=runtime") == 0
                          ? NY_DUMP_SCOPE_BOTH
                          : NY_DUMP_SCOPE_PROGRAM;
    return true;
  }
  if (strncmp(a, "--inspect=", 10) == 0) {
    fprintf(stderr,
            "invalid --inspect scope '%s' (expected program or runtime)\n",
            a + 10);
    exit(1);
  }
  if (strcmp(a, "--nyr-dump") == 0) {
    opt->native_dump_ir = true;
    opt->nyr_dump_text = true;
    return true;
  }
  if (strncmp(a, "--nyr-dump=", 11) == 0) {
    const char *modes = a + 11;
    if (!*modes) {
      fprintf(stderr, "empty --nyr-dump option list\n");
      exit(1);
    }
    opt->native_dump_ir = true;
    opt->nyr_dump_text = true;
    while (*modes) {
      const char *end = strchr(modes, ',');
      size_t len = end ? (size_t)(end - modes) : strlen(modes);
      if (len == 3 && strncmp(modes, "raw", len) == 0) {
        opt->nyr_dump_raw = true;
      } else if (len == 3 && strncmp(modes, "cfg", len) == 0) {
        opt->nyr_dump_cfg = true;
      } else if (len == 5 && strncmp(modes, "stats", len) == 0) {
        opt->nyr_dump_stats = true;
      } else if (len == 6 && strncmp(modes, "passes", len) == 0) {
        opt->nyr_dump_raw = true;
        opt->nyr_pass_stats = true;
      } else {
        fprintf(stderr,
                "invalid --nyr-dump option '%.*s' (expected raw, cfg, "
                "stats, or passes)\n",
                (int)len, modes);
        exit(1);
      }
      if (!end)
        break;
      modes = end + 1;
      if (!*modes) {
        fprintf(stderr, "empty option in --nyr-dump option list\n");
        exit(1);
      }
    }
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-dump-to", i, argc, argv,
                                      argv0)) != NULL) {
    (void)ny_options_apply_legacy_emit_path(opt, "nyr", value);
    return true;
  }
  if (strcmp(a, "--nyr-verify") == 0) {
    opt->nyr_ir_verify = true;
    return true;
  }
  if (strcmp(a, "--nyr-bisect") == 0) {
    opt->nyr_bisect = true;
    opt->nyr_ir_verify = true;
    return true;
  }
  if (strcmp(a, "--nyr-fingerprint") == 0) {
    opt->nyr_fingerprint = true;
    return true;
  }
  if (strcmp(a, "--nyr-flaky-check") == 0) {
    opt->nyr_flaky_check = true;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--dbg-filter", i, argc, argv,
                                      argv0)) != NULL) {
    opt->dbg_filter = value;
    return true;
  }
  if (strcmp(a, "--delta-only") == 0) {
    opt->dbg_delta_only = true;
    return true;
  }
  if (strcmp(a, "--unify-trace") == 0) {
    opt->dbg_unify_trace = true;
    return true;
  }
  if (strcmp(a, "--trait-resolve") == 0) {
    opt->dbg_trait_resolve = true;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--dbg-format", i, argc, argv,
                                      argv0)) != NULL) {
    if (strcmp(value, "json") == 0)
      opt->dbg_format_json = true;
    else if (strcmp(value, "text") != 0) {
      fprintf(stderr, "ny: invalid --dbg-format '%s' (expected text|json)\n",
              value);
      exit(2);
    }
    return true;
  }
  if (strcmp(a, "--regalloc-heat") == 0) {
    opt->dbg_regalloc_heat = true;
    return true;
  }
  if (strcmp(a, "--elf-dump") == 0) {
    opt->dbg_elf_dump = true;
    return true;
  }
  if (strcmp(a, "--stack-frame") == 0) {
    opt->dbg_stack_layout = true;
    return true;
  }
  if (strcmp(a, "--abi") == 0) {
    opt->dbg_abi = true;
    return true;
  }
  if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
    opt->verbosity++;
    return true;
  }
  if (strcmp(a, "-vv") == 0) {
    opt->verbosity = 2;
    return true;
  }
  if (strcmp(a, "-vvv") == 0) {
    opt->verbosity = 3;
    return true;
  }
  if (strcmp(a, "-vvvv") == 0) {
    opt->verbosity = 4;
    return true;
  }
  if (strcmp(a, "-vvvvv") == 0) {
    opt->verbosity = 5;
    return true;
  }
  if (strcmp(a, "-vvvvvv") == 0) {
    opt->verbosity = 6;
    return true;
  }
  if (strcmp(a, "--nyr-rollback-on-verify-fail") == 0) {
    opt->nyr_rollback_on_verify_fail = true;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-global-inst-cap", i, argc, argv,
                                      argv0)) != NULL) {
    opt->nyr_global_inst_cap =
        (size_t)ny_parse_nonneg_int_or_die(value, "nyr-global-inst-cap", argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-pass-growth-factor", i, argc,
                                      argv, argv0)) != NULL) {
    opt->nyr_pass_growth_factor =
        ny_parse_nonneg_f64_or_die(value, "nyr-pass-growth-factor", argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-disable-pass", i, argc, argv,
                                      argv0)) != NULL) {
    opt->nyr_disable_pass = value;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-stop-after", i, argc, argv,
                                      argv0)) != NULL) {
    opt->nyr_stop_after = value;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-track-value", i, argc, argv,
                                      argv0)) != NULL) {
    opt->nyr_track_value =
        ny_parse_nonneg_int_or_die(value, "nyr-track-value", argv0);
    if (opt->verbosity < 2)
      opt->verbosity = 2;
    return true;
  }
  if (strcmp(a, "--dump-nyr") == 0) {
    opt->native_dump_ir = true;
    opt->nyr_dump_bin = true;
    return true;
  }
  if (strncmp(a, "--dump-nyr=", 11) == 0) {
    opt->native_dump_ir = true;
    opt->nyr_dump_bin = true;
    opt->nyr_dump_bin_path = a + 11;
    return true;
  }
  if (strcmp(a, "--nyr-metadata") == 0) {
    (void)ny_options_apply_legacy_emit_path(opt, "nyr-metadata", NULL);
    return true;
  }
  if (strncmp(a, "--nyr-metadata=", 15) == 0) {
    (void)ny_options_apply_legacy_emit_path(opt, "nyr-metadata", a + 15);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-metadata-bin", i, argc, argv,
                                      argv0)) != NULL) {
    opt->native_dump_ir = true;
    opt->nyr_metadata_report = true;
    opt->nyr_metadata_bin_path = value;
    return true;
  }
  if (strcmp(a, "--nyr-run") == 0) {
    opt->native_dump_ir = true;
    opt->nyr_run = true;
    return true;
  }
  if (strcmp(a, "--vm-disasm") == 0) {
    opt->vm_disasm = true;
    opt->native_dump_ir = true;
    return true;
  }
  if (strncmp(a, "--vm-disasm=", 12) == 0) {
    if (!a[12]) {
      fprintf(stderr, "empty --vm-disasm name filter\n");
      exit(1);
    }
    opt->vm_disasm = true;
    opt->vm_disasm_filter = a + 12;
    opt->native_dump_ir = true;
    return true;
  }
  if (strcmp(a, "--mixed-disasm") == 0) {
    opt->mixed_disasm = true;
    return true;
  }
  if (strncmp(a, "--mixed-disasm=", 15) == 0) {
    if (!a[15]) {
      fprintf(stderr, "empty --mixed-disasm name filter\n");
      exit(1);
    }
    opt->mixed_disasm = true;
    opt->mixed_disasm_filter = a + 15;
    return true;
  }
  if (strncmp(a, "--trace-filter=", 15) == 0) {
    /*
     * Surfaces the NYTRIX_TRACE_FILTER env contract as a flag: the value is
     * a comma-separated list of function-name tails to keep in -trace.
     */
    ny_setenv("NYTRIX_TRACE_FILTER", a + 15, 1);
    return true;
  }
  if (strcmp(a, "--trace-tokens") == 0) {
    opt->trace_tokens = true;
    return true;
  }
  if (strcmp(a, "--trace-parse") == 0) {
    opt->trace_parse = true;
    return true;
  }
  if (strcmp(a, "--trace-scope") == 0) {
    opt->trace_scope = true;
    return true;
  }
  if (strncmp(a, "--dump-ast=", 11) == 0) {
    const char *modes = a + 11;
    if (!*modes) {
      fprintf(stderr, "empty --dump-ast option list\n");
      exit(1);
    }
    opt->dump_ast = true;
    while (*modes) {
      const char *end = strchr(modes, ',');
      size_t len = end ? (size_t)(end - modes) : strlen(modes);
      if (len == 5 && strncmp(modes, "typed", len) == 0) {
        opt->dump_ast_typed = true;
      } else if (len >= 7 && strncmp(modes, "filter=", 7) == 0) {
        if (len == 7) {
          fprintf(stderr, "empty filter in --dump-ast option list\n");
          exit(1);
        }
        opt->dump_ast_filter = modes + 7;
        break;
      } else {
        fprintf(stderr,
                "invalid --dump-ast option '%.*s' (expected typed or "
                "filter=NAME)\n",
                (int)len, modes);
        exit(1);
      }
      if (!end)
        break;
      modes = end + 1;
      if (!*modes) {
        fprintf(stderr, "empty option in --dump-ast option list\n");
        exit(1);
      }
    }
    return true;
  }
  if (strcmp(a, "--trace-hm") == 0) {
    opt->trace_hm = true;
    return true;
  }
  if (strcmp(a, "--explain") == 0) {
    opt->explain = true;
    opt->explain_any = true;
    opt->dump_proofs = true;
    opt->dump_escapes = true;
    return true;
  }
  if (strncmp(a, "--explain=", 10) == 0) {
    opt->explain = true;
    opt->explain_topic = a + 10;
    opt->explain_any = true;
    return true;
  }
  if (strcmp(a, "--explain-any") == 0) {
    opt->explain_any = true;
    return true;
  }
  if (strcmp(a, "--dump-escapes") == 0) {
    opt->dump_escapes = true;
    return true;
  }
  if (strcmp(a, "--dump-proofs") == 0) {
    opt->dump_proofs = true;
    return true;
  }
  if (strcmp(a, "--trace-llvm") == 0) {
    opt->trace_llvm = true;
    return true;
  }
  if (strcmp(a, "--dump-llvm-passes") == 0) {
    opt->dump_llvm_passes = true;
    return true;
  }
  if (strcmp(a, "--trace-regalloc") == 0) {
    opt->trace_regalloc = true;
    return true;
  }
  if (strcmp(a, "--dump-obj-full") == 0) {
    opt->dump_obj_full = true;
    return true;
  }
  if (strcmp(a, "--compile-profile-fn") == 0) {
    opt->compile_profile_fn = true;
    return true;
  }
  if (strcmp(a, "--debug-everything") == 0) {
    ny_enable_debug_everything_env();
    opt->debug_everything = true;
    opt->trace_tokens = true;
    opt->trace_parse = true;
    opt->trace_scope = true;
    opt->dump_ast_typed = true;
    opt->trace_hm = true;
    opt->explain_any = true;
    opt->dump_escapes = true;
    opt->dump_proofs = true;
    opt->trace_llvm = true;
    opt->dump_llvm_passes = true;
    opt->trace_regalloc = true;
    opt->dump_obj_full = true;
    opt->compile_profile_fn = true;
    opt->native_dump_ir = true;
    opt->nyr_dump_text = true;
    opt->nyr_dump_raw = true;
    opt->nyr_dump_cfg = true;
    opt->nyr_dump_stats = true;
    opt->nyr_pass_stats = true;
    opt->nyr_ir_verify = true;
    opt->nyr_rollback_on_verify_fail = true;
    opt->dump_on_error = true;
    if (opt->timeout <= 0.0)
      opt->timeout = 30.0;
    return true;
  }
  if (strcmp(a, "--native-result-oracle") == 0) {
    opt->native_result_oracle = true;
    opt->native_backend = NY_NATIVE_BACKEND_X86_64;
    opt->native_backend_raw = "x86_64";
    return true;
  }
  if (strncmp(a, "--native-result-oracle=", 23) == 0) {
    opt->native_result_oracle = true;
    opt->native_result_oracle_expected = a + 23;
    opt->native_backend = NY_NATIVE_BACKEND_X86_64;
    opt->native_backend_raw = "x86_64";
    return true;
  }
  if (strcmp(a, "--native-oracle-per-pass") == 0) {
    opt->native_oracle_per_pass = true;
    opt->native_result_oracle = true;
    opt->native_backend = NY_NATIVE_BACKEND_X86_64;
    opt->native_backend_raw = "x86_64";
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-oracle-fuzz", i, argc, argv,
                                      argv0)) != NULL) {
    opt->native_oracle_fuzz_count =
        ny_parse_nonneg_int_or_die(value, "native oracle fuzz count", argv0);
    opt->native_result_oracle = true;
    opt->native_backend = NY_NATIVE_BACKEND_X86_64;
    opt->native_backend_raw = "x86_64";
    return true;
  }
  if (strcmp(a, "--native-tv-seed") == 0) {
    opt->native_tv_seed_trials = 16;
    return true;
  }
  if (strncmp(a, "--native-tv-seed=", 17) == 0) {
    opt->native_tv_seed_trials =
        ny_parse_nonneg_int_or_die(a + 17, "native TV seed trials", argv0);
    return true;
  }
  if (strncmp(a, "--nyr-run=", 10) == 0) {
    opt->native_dump_ir = true;
    opt->nyr_run = true;
    opt->nyr_run_path = a + 10;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-run-bin", i, argc, argv,
                                      argv0)) != NULL) {
    opt->native_dump_ir = true;
    opt->nyr_run = true;
    opt->nyr_run_bin_path = value;
    return true;
  }
  if (strcmp(a, "--nyr-run-profile") == 0) {
    opt->native_dump_ir = true;
    opt->nyr_run = true;
    opt->nyr_run_profile = true;
    return true;
  }
  if (strncmp(a, "--nyr-run-profile=", 18) == 0) {
    opt->native_dump_ir = true;
    opt->nyr_run = true;
    opt->nyr_run_profile = true;
    opt->nyr_run_profile_path = a + 18;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--native-profile-use", i, argc, argv,
                                      argv0)) != NULL) {
    opt->native_profile_use_path = value;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-run-max-steps", i, argc, argv,
                                      argv0)) != NULL) {
    opt->native_dump_ir = true;
    opt->nyr_run = true;
    opt->nyr_run_max_steps =
        ny_parse_nonneg_int_or_die(value, "NYR VM max steps", argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--nyr-run-recursion-limit", i, argc,
                                      argv, argv0)) != NULL) {
    opt->native_dump_ir = true;
    opt->nyr_run = true;
    opt->nyr_run_recursion_limit =
        ny_parse_nonneg_int_or_die(value, "NYR VM recursion limit", argv0);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--emit-artifact", i, argc, argv,
                                      argv0)) != NULL) {
    (void)ny_options_apply_legacy_emit_path(opt, "artifact", value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--verify-artifact", i, argc, argv,
                                      argv0)) != NULL) {
    opt->verify_artifact_path = value;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--use-artifact", i, argc, argv,
                                      argv0)) != NULL) {
    opt->use_artifact_path = value;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--emit-refinement-interface", i, argc,
                                      argv, argv0)) != NULL) {
    (void)ny_options_apply_legacy_emit_path(opt, "refinement-interface", value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--use-refinement-interface", i, argc,
                                      argv, argv0)) != NULL) {
    opt->use_refinement_interface_path = value;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--emit-effect-interface", i, argc,
                                      argv, argv0)) != NULL) {
    (void)ny_options_apply_legacy_emit_path(opt, "effect-interface", value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--use-effect-interface", i, argc,
                                      argv, argv0)) != NULL) {
    opt->use_effect_interface_path = value;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--emit-ir", i, argc, argv, argv0)) !=
      NULL) {
    (void)ny_options_apply_legacy_emit_path(opt, "ir", value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--emit-bc", i, argc, argv, argv0)) !=
      NULL) {
    (void)ny_options_apply_legacy_emit_path(opt, "bc", value);
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--emit-asm", i, argc, argv, argv0)) !=
      NULL) {
    (void)ny_options_apply_legacy_emit_path(opt, "asm", value);
    return true;
  }
  if (strncmp(a, "--emit-wasm=", 12) == 0) {
    (void)ny_options_apply_legacy_emit_path(opt, "wasm", a + 12);
    return true;
  }
  if (strcmp(a, "--emit-wasm") == 0) {
    (void)ny_options_apply_legacy_emit_path(opt, "wasm", NULL);
    return true;
  }
  if (strcmp(a, "-emit-only") == 0) {
    opt->emit_only = true;
    opt->run_aot = false;
    opt->run_jit = false;
    return true;
  }
  if (strcmp(a, "--collect-errors") == 0) {
    opt->collect_errors = true;
    return true;
  }
  if (strcmp(a, "--emit-shapes") == 0) {
    (void)ny_options_apply_legacy_emit_path(opt, "shapes", NULL);
    return true;
  }
  if (strcmp(a, "--strict") == 0) {
    opt->strict_types = true;
    opt->strict_types_explicit = true;
    opt->ownership = true;
    opt->ownership_strict = true;
    if (!opt->heap_policy_explicit)
      opt->heap_policy = NY_HEAP_RAII;
    return true;
  }
  if (strcmp(a, "--strict-types") == 0) {
    opt->strict_types = true;
    opt->strict_types_explicit = true;
    return true;
  }
  if (strcmp(a, "--no-strict-types") == 0) {
    opt->strict_types = false;
    opt->strict_types_explicit = true;
    return true;
  }
  if (strcmp(a, "--list-fallbacks") == 0) {
    opt->list_fallbacks = true;
    return true;
  }
  if (strcmp(a, "--warn-shadow") == 0) {
    opt->warn_shadow = true;
    return true;
  }
  if ((value = ny_option_value_or_die(a, "--max-errors", i, argc, argv,
                                      argv0)) != NULL) {
    opt->max_errors = ny_parse_nonneg_int_or_die(value, "max errors", argv0);
    return true;
  }
  return false;
}

static bool ny_is_gpu_offload_mode(const char *mode) {
  return mode && (strcmp(mode, "off") == 0 || strcmp(mode, "auto") == 0 ||
                  strcmp(mode, "on") == 0 || strcmp(mode, "force") == 0);
}

static bool ny_is_parallel_mode(const char *mode) {
  return mode && (strcmp(mode, "off") == 0 || strcmp(mode, "auto") == 0 ||
                  strcmp(mode, "threads") == 0 || strcmp(mode, "modules") == 0);
}

typedef bool (*ny_option_choice_pred_t)(const char *);

static const char *ny_registry_allowed_values_for(const char *option);

static const char *ny_gpu_backend_option_value(const char *backend) {
  return backend && strcmp(backend, "off") == 0 ? "none" : backend;
}

static void ny_set_option_choice_or_die(const char **slot, const char *value,
                                        ny_option_choice_pred_t valid,
                                        const char *option) {
  if (valid(value)) {
    *slot = value;
    return;
  }
  const char *allowed_values = ny_registry_allowed_values_for(option);
  fprintf(stderr, "ny: invalid %s '%s' (expected %s)\n", option,
          value ? value : "(null)",
          allowed_values ? allowed_values : "a registered value");
  exit(2);
}

typedef enum {
  NY_OPT_TOGGLE_BOOL,
  NY_OPT_TOGGLE_INT,
  NY_OPT_TOGGLE_DUMP_SCOPE
} ny_option_toggle_kind_t;

typedef struct {
  const char *name;
  size_t offset;
  int value;
  ny_option_toggle_kind_t kind;
} ny_option_toggle_t;

static bool ny_options_apply_toggle(ny_options *opt, const char *a) {
  static const ny_option_toggle_t toggles[] = {
      {"--effect-require-known", offsetof(ny_options, effect_require_known), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--no-effect-require-known", offsetof(ny_options, effect_require_known),
       0, NY_OPT_TOGGLE_BOOL},
      {"--alias-require-known", offsetof(ny_options, alias_require_known), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--no-alias-require-known", offsetof(ny_options, alias_require_known), 0,
       NY_OPT_TOGGLE_BOOL},
      {"--alias-require-no-escape",
       offsetof(ny_options, alias_require_no_escape), 1, NY_OPT_TOGGLE_BOOL},
      {"--std-builtin-ops", offsetof(ny_options, std_builtin_ops), 1,
       NY_OPT_TOGGLE_INT},
      {"--no-std-builtin-ops", offsetof(ny_options, std_builtin_ops), 0,
       NY_OPT_TOGGLE_INT},
      {"--compiler-asserts", offsetof(ny_options, compiler_asserts), 1,
       NY_OPT_TOGGLE_INT},
      {"--no-compiler-asserts", offsetof(ny_options, compiler_asserts), 0,
       NY_OPT_TOGGLE_INT},
      {"--debug-locals", offsetof(ny_options, debug_locals), 1,
       NY_OPT_TOGGLE_INT},
      {"--no-debug-locals", offsetof(ny_options, debug_locals), 0,
       NY_OPT_TOGGLE_INT},
      {"--dwarf-split-inlining", offsetof(ny_options, dwarf_split_inlining), 1,
       NY_OPT_TOGGLE_INT},
      {"--no-dwarf-split-inlining", offsetof(ny_options, dwarf_split_inlining),
       0, NY_OPT_TOGGLE_INT},
      {"--dwarf-profile-info", offsetof(ny_options, dwarf_profile_info), 1,
       NY_OPT_TOGGLE_INT},
      {"--no-dwarf-profile-info", offsetof(ny_options, dwarf_profile_info), 0,
       NY_OPT_TOGGLE_INT},
      {"--opt-dce", offsetof(ny_options, opt_dce), 1, NY_OPT_TOGGLE_INT},
      {"--dce", offsetof(ny_options, opt_dce), 1, NY_OPT_TOGGLE_INT},
      {"--no-opt-dce", offsetof(ny_options, opt_dce), 0, NY_OPT_TOGGLE_INT},
      {"--no-dce", offsetof(ny_options, opt_dce), 0, NY_OPT_TOGGLE_INT},
      {"--opt-internalize", offsetof(ny_options, opt_internalize), 1,
       NY_OPT_TOGGLE_INT},
      {"--no-opt-internalize", offsetof(ny_options, opt_internalize), 0,
       NY_OPT_TOGGLE_INT},
      {"--opt-loops", offsetof(ny_options, opt_loops), 1, NY_OPT_TOGGLE_INT},
      {"--no-opt-loops", offsetof(ny_options, opt_loops), 0, NY_OPT_TOGGLE_INT},
      {"--opt-autotune", offsetof(ny_options, opt_autotune), 1,
       NY_OPT_TOGGLE_INT},
      {"--no-opt-autotune", offsetof(ny_options, opt_autotune), 0,
       NY_OPT_TOGGLE_INT},
      {"--gpu-async", offsetof(ny_options, gpu_async), 1, NY_OPT_TOGGLE_INT},
      {"--no-gpu-async", offsetof(ny_options, gpu_async), 0, NY_OPT_TOGGLE_INT},
      {"--gpu-fast-math", offsetof(ny_options, gpu_fast_math), 1,
       NY_OPT_TOGGLE_INT},
      {"--no-gpu-fast-math", offsetof(ny_options, gpu_fast_math), 0,
       NY_OPT_TOGGLE_INT},
      {"--fast-math", offsetof(ny_options, fast_math), 1, NY_OPT_TOGGLE_INT},
      {"--no-fast-math", offsetof(ny_options, fast_math), 0, NY_OPT_TOGGLE_INT},
      {"-strip", offsetof(ny_options, strip_override), 1, NY_OPT_TOGGLE_INT},
      {"--strip", offsetof(ny_options, strip_override), 1, NY_OPT_TOGGLE_INT},
      {"-no-strip", offsetof(ny_options, strip_override), 0, NY_OPT_TOGGLE_INT},
      {"--no-strip", offsetof(ny_options, strip_override), 0,
       NY_OPT_TOGGLE_INT},
      {"-time", offsetof(ny_options, do_timing), 1, NY_OPT_TOGGLE_BOOL},
      {"--dump-ast", offsetof(ny_options, dump_ast), 1, NY_OPT_TOGGLE_BOOL},
      {"--extract-json", offsetof(ny_options, extract_json), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--collect-errors", offsetof(ny_options, collect_errors), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--emit-shapes", offsetof(ny_options, emit_shapes), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--dump-llvm", offsetof(ny_options, dump_llvm), 1, NY_OPT_TOGGLE_BOOL},
      {"--native-tier-report", offsetof(ny_options, native_tier_report), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--dump-nyr", offsetof(ny_options, nyr_dump_bin), 1, NY_OPT_TOGGLE_BOOL},
      {"--nyr-metadata", offsetof(ny_options, nyr_metadata_report), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--nyr-run", offsetof(ny_options, nyr_run), 1, NY_OPT_TOGGLE_BOOL},
      {"--nyr-run-profile", offsetof(ny_options, nyr_run_profile), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--warn-all", offsetof(ny_options, warn_level), 2, NY_OPT_TOGGLE_INT},
      {"--warn-useful", offsetof(ny_options, warn_level), 1, NY_OPT_TOGGLE_INT},
      {"--no-warn", offsetof(ny_options, warn_level), 0, NY_OPT_TOGGLE_INT},
      {"-w", offsetof(ny_options, warn_level), 0, NY_OPT_TOGGLE_INT},
      {"--diag-compact", offsetof(ny_options, diag_compact), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--diag-rich", offsetof(ny_options, diag_compact), 0,
       NY_OPT_TOGGLE_BOOL},
      {"--dump-program", offsetof(ny_options, dump_scope),
       NY_DUMP_SCOPE_PROGRAM, NY_OPT_TOGGLE_DUMP_SCOPE},
      {"--dump-lib", offsetof(ny_options, dump_scope), NY_DUMP_SCOPE_LIB,
       NY_OPT_TOGGLE_DUMP_SCOPE},
      {"--dump-both", offsetof(ny_options, dump_scope), NY_DUMP_SCOPE_BOTH,
       NY_OPT_TOGGLE_DUMP_SCOPE},
      {"--dump-tokens", offsetof(ny_options, dump_tokens), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--dump-funcs", offsetof(ny_options, dump_funcs), 1, NY_OPT_TOGGLE_BOOL},
      {"--report-peak-rss", offsetof(ny_options, report_peak_rss), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--dump-on-error", offsetof(ny_options, dump_on_error), 1,
       NY_OPT_TOGGLE_BOOL},
      {"-trace", offsetof(ny_options, trace_exec), 1, NY_OPT_TOGGLE_BOOL},
      {"-verify", offsetof(ny_options, verify_module), 1, NY_OPT_TOGGLE_BOOL},
      {"-g", offsetof(ny_options, debug_symbols), 1, NY_OPT_TOGGLE_BOOL},
      {"--plain-repl", offsetof(ny_options, repl_plain), 1, NY_OPT_TOGGLE_BOOL},
      {"--zero-init", offsetof(ny_options, zero_init), 1, NY_OPT_TOGGLE_BOOL},
      {"--no-zero-init", offsetof(ny_options, zero_init), 0,
       NY_OPT_TOGGLE_BOOL},
      {"--jit-fast-isel", offsetof(ny_options, jit_fast_isel), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--no-jit-fast-isel", offsetof(ny_options, jit_fast_isel), 0,
       NY_OPT_TOGGLE_BOOL},
      {"--jit-perf-map", offsetof(ny_options, jit_perf_map), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--no-jit-perf-map", offsetof(ny_options, jit_perf_map), 0,
       NY_OPT_TOGGLE_BOOL},
      {"--hot-reload", offsetof(ny_options, hot_reload), 1, NY_OPT_TOGGLE_BOOL},
      {"--watch", offsetof(ny_options, watch_files), 1, NY_OPT_TOGGLE_BOOL},
      {NULL, 0, 0, NY_OPT_TOGGLE_BOOL}};
  for (size_t i = 0; toggles[i].name; ++i) {
    if (strcmp(a, toggles[i].name) != 0)
      continue;
    char *base = (char *)opt + toggles[i].offset;
    if (toggles[i].kind == NY_OPT_TOGGLE_BOOL)
      *(bool *)base = toggles[i].value != 0;
    else if (toggles[i].kind == NY_OPT_TOGGLE_INT)
      *(int *)base = toggles[i].value;
    else
      *(ny_dump_scope_t *)base = (ny_dump_scope_t)toggles[i].value;
    if (toggles[i].offset == offsetof(ny_options, warn_level))
      opt->warn_all_mode = toggles[i].value == 2;
    return true;
  }
  return false;
}

void ny_options_init(ny_options *opt) {
  memset(opt, 0, sizeof(ny_options));
  opt->mode = NY_MODE_RUN;
  opt->opt_level = 2;
  opt->opt_size = true;
  opt->opt_profile = NULL;
  opt->opt_dce = 1;
  opt->strip_override = -1;
  opt->color_mode = -1;
  opt->gpu_async = -1;
  opt->gpu_fast_math = -1;
  opt->fast_math = -1;
  opt->gprof = -1;
  opt->std_builtin_ops = 1;
  opt->compiler_asserts = -1;
  opt->debug_locals = -1;
  opt->dwarf_version = 0;
  opt->dwarf_split_inlining = -1;
  opt->dwarf_profile_info = -1;
  opt->std_mode = STD_MODE_MINIMAL;
  opt->effect_require_known = true;
  opt->alias_require_known = true;
  opt->ir_include_std = false;
  opt->dump_scope = NY_DUMP_SCOPE_PROGRAM;
  opt->dump_dir = "build/debug";
  opt->dump_diagnose = false;
  opt->diag_compact = false;
  opt->warn_level = 1;
  opt->max_errors = -1;
  opt->stop_after = NY_STOP_AFTER_NONE;
  opt->type_solver = NY_TYPE_SOLVER_AUTO;
  opt->type_solver_raw = "auto";
  opt->proof_solver = NY_PROOF_SOLVER_AUTO;
  opt->proof_solver_raw = "auto";
  opt->native_backend = NY_NATIVE_BACKEND_LLVM;
  opt->native_backend_raw = "llvm";
  opt->native_abi = NY_NATIVE_ABI_AUTO;
  opt->native_abi_raw = "auto";
  opt->native_tier = NY_NATIVE_TIER_AUTO;
  opt->native_tier_raw = "auto";
  opt->native_tier_budget = -1;
  opt->native_hot_threshold = -1;
  opt->native_cold_threshold = -1;
  opt->native_cache_score = -1;
  opt->native_prefer_vm = false;
  opt->native_prefer_asm = false;
  opt->llvm_lto = false;
  /*
   * CF mem2reg is on by default: idom fix + RPO rename before SCCP
   * make loop-carried locals safe. Disable with --native-disable-cf-mem2reg.
   */
  opt->native_enable_cf_mem2reg = true;
  opt->callgraph = false;
  opt->callgraph_json = false;
  opt->callgraph_depth = -1;
  opt->nyr_run_max_steps = -1;
  opt->nyr_run_recursion_limit = -1;
  opt->nyr_global_inst_cap = 0;
  opt->nyr_pass_growth_factor = 0.0;
  opt->verbosity = 0;
  opt->nyr_track_value = -1;
  opt->c_frontend = NY_C_FRONTEND_AUTO;
  opt->c_frontend_raw = "auto";
  opt->extract_lang = "ny,nytrix";
  opt->strict_types = ny_env_enabled("NYTRIX_STRICT_TYPES");
  opt->strict_types_explicit = false;
  opt->parallel_mode = "auto";
  opt->opt_dce = -1;
  opt->opt_internalize = -1;
  opt->opt_loops = 0;
  opt->opt_autotune = 0;
  opt->opt_level_explicit = false;
  opt->profiler_mode = false;
  opt->profiler_output_mode = "smart";
  opt->report_peak_rss = false;
  opt->debug_symbols = false;
  opt->strip_override = -1;
  opt->timeout = 0.0;
  opt->enable_gc = false;
  opt->ownership = false;
  opt->ownership_strict = false;
  opt->borrow_check = false;
  opt->heap_policy = NY_HEAP_MANUAL;
  opt->heap_policy_explicit = false;
  opt->gc_flag_seen = false;
  opt->runtime_mode = NY_RUNTIME_MODE_DEFAULT;
  opt->runtime_mode_raw = "default";
  opt->no_progress = ny_env_enabled("NYTRIX_NO_PROGRESS");
}

typedef struct ny_usage_entry_t {
  const char *color;
  const char *flag;
  const char *desc;
} ny_usage_entry_t;

typedef enum {
  NY_REG_BOOL,
  NY_REG_ENUM,
  NY_REG_INT,
  NY_REG_PATH,
  NY_REG_LIST
} ny_registry_kind_t;

typedef struct {
  const char *name;
  const char *aliases[8];
  ny_registry_kind_t kind;
  const char *allowed_values;
  const char *group;
  const char *summary;
  const char *visibility;
  const char *deprecated_by;
  const char *conflicts;
  const char *requires;
  const char *short_form;
  const char *family;
  char item_letter;
  bool takes_value;
  const char *preset;
  bool config_trust;
  const char *scope;
} ny_option_registry_entry_t;

#define NY_REG_BOOL_OPTION(option_name, option_group, option_summary)     \
  {option_name, {NULL}, NY_REG_BOOL, NULL,  option_group, option_summary, \
   "advanced",  NULL,   NULL,        NULL,  NULL,         NULL,           \
   0,           false,  NULL,        false, "global"}
#define NY_REG_PATH_OPTION(option_name, option_group, option_summary)     \
  {option_name, {NULL}, NY_REG_PATH, NULL,  option_group, option_summary, \
   "advanced",  NULL,   NULL,        NULL,  NULL,         NULL,           \
   0,           true,   NULL,        false, "global"}
#define NY_REG_TRUSTED_PATH_OPTION(option_name, option_group, option_summary) \
  {option_name, {NULL}, NY_REG_PATH, NULL, option_group, option_summary,      \
   "advanced",  NULL,   NULL,        NULL, NULL,         NULL,                \
   0,           true,   NULL,        true, "global"}
#define NY_REG_TRUSTED_VALUE_OPTION(option_name, option_kind, values,      \
                                    option_group, option_summary)          \
  {option_name, {NULL}, option_kind, values, option_group, option_summary, \
   "advanced",  NULL,   NULL,        NULL,   NULL,         NULL,           \
   0,           true,   NULL,        true,   "global"}
#define NY_REG_OPTIONAL_PATH_OPTION(option_name, option_group, option_summary) \
  {option_name, {NULL}, NY_REG_PATH, NULL,  option_group, option_summary,      \
   "advanced",  NULL,   NULL,        NULL,  NULL,         NULL,                \
   0,           false,  NULL,        false, "global"}
#define NY_REG_TRUSTED_OPTIONAL_PATH_OPTION(option_name, option_group,   \
                                            option_summary)              \
  {option_name, {NULL}, NY_REG_PATH, NULL, option_group, option_summary, \
   "advanced",  NULL,   NULL,        NULL, NULL,         NULL,           \
   0,           false,  NULL,        true, "global"}
#define NY_REG_VALUE_OPTION(option_name, option_kind, values, option_group, \
                            option_summary)                                 \
  {option_name, {NULL}, option_kind, values, option_group, option_summary,  \
   "advanced",  NULL,   NULL,        NULL,   NULL,         NULL,            \
   0,           true,   NULL,        false,  "global"}
#define NY_REG_FAMILY_VALUE_OPTION(option_name, option_kind, values,         \
                                   option_group, option_summary, short_name, \
                                   family_spec)                              \
  {option_name, {NULL}, option_kind, values, option_group, option_summary,   \
   "advanced",  NULL,   NULL,        NULL,   short_name,   family_spec,      \
   0,           true,   NULL,        false,  "global"}
#define NY_REG_FAMILY_TRUSTED_VALUE_OPTION(option_name, option_kind, values, \
                                           option_group, option_summary,     \
                                           short_name, family_spec)          \
  {option_name, {NULL}, option_kind, values, option_group, option_summary,   \
   "advanced",  NULL,   NULL,        NULL,   short_name,   family_spec,      \
   0,           true,   NULL,        true,   "global"}
#define NY_REG_DEPRECATED_BOOL_OPTION(option_name, option_group,   \
                                      option_summary, replacement) \
  {option_name,    {NULL},       NY_REG_BOOL, NULL,  option_group, \
   option_summary, "deprecated", replacement, NULL,  NULL,         \
   NULL,           NULL,         0,           false, NULL,         \
   false,          "global"}
#define NY_REG_DEPRECATED_VALUE_OPTION(option_name, option_group,   \
                                       option_summary, replacement) \
  {option_name,    {NULL},       NY_REG_PATH, NULL, option_group,   \
   option_summary, "deprecated", replacement, NULL, NULL,           \
   NULL,           NULL,         0,           true, NULL,           \
   true,           "global"}
#define NY_REG_DEPRECATED_OPTIONAL_PATH_OPTION(option_name, option_group,   \
                                               option_summary, replacement) \
  {option_name,    {NULL},       NY_REG_PATH, NULL,  option_group,          \
   option_summary, "deprecated", replacement, NULL,  NULL,                  \
   NULL,           NULL,         0,           false, NULL,                  \
   true,           "global"}

static const struct {
  const char *name;
  const char *flags;
} ny_option_builtin_presets[] = {
    {"debug", "--debug --warn=useful"}, {"release", "--release"},
    {"ci", "--strict --warn=error"},    {"inspect", "--inspect"},
    {"perf", "--perf=compact"},
};

static const ny_option_registry_entry_t ny_option_registry[] = {
    {"--help",
     {"-h", NULL},
     NY_REG_BOOL,
     NULL,
     "info",
     "Show help or a topic reference",
     "common",
     NULL,
     NULL,
     NULL,
     "-h",
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    {"--version",
     {NULL},
     NY_REG_BOOL,
     NULL,
     "info",
     "Print the compiler version",
     "common",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    {"--presets",
     {NULL},
     NY_REG_BOOL,
     NULL,
     "info",
     "List built-in command-line presets",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    {"--print-flags",
     {"-n", NULL},
     NY_REG_BOOL,
     NULL,
     "info",
     "Print canonical compiler flags and the program-argument boundary",
     "advanced",
     NULL,
     NULL,
     NULL,
     "-n",
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    NY_REG_DEPRECATED_BOOL_OPTION("-help", "info", "Legacy help spelling",
                                  "--help"),
    NY_REG_DEPRECATED_BOOL_OPTION("-version", "info", "Legacy version spelling",
                                  "--version"),
    {"--run",
     {NULL},
     NY_REG_ENUM,
     "auto|aot|jit|native|none",
     "run",
     "Select the execution backend",
     "common",
     NULL,
     "--jit",
     NULL,
     "-R",
     "R:run:a=aot,j=jit,n=native,x=none,w=watch,s=safe-run",
     0,
     true,
     NULL,
     false,
     "global"},
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-run", "run", "Run through AOT (legacy spelling)", "--run=aot"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "--run-aot", "run", "Run through AOT (legacy spelling)", "--run=aot"),
    NY_REG_DEPRECATED_BOOL_OPTION("--run-native", "run",
                                  "Run through native code (legacy spelling)",
                                  "--run=native"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "--run-jit", "run", "Run through JIT (legacy spelling)", "--run=jit"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-jit", "run", "Run through JIT (legacy spelling)", "--run=jit"),
    NY_REG_DEPRECATED_BOOL_OPTION("--native", "run",
                                  "Run through native code (legacy spelling)",
                                  "--run=native"),
    NY_REG_DEPRECATED_BOOL_OPTION("--native-only", "run",
                                  "Run through native code (legacy spelling)",
                                  "--run=native"),
    {"--eval",
     {"-e", NULL},
     NY_REG_PATH,
     NULL,
     "run",
     "Execute inline source code",
     "advanced",
     NULL,
     NULL,
     NULL,
     "-e",
     NULL,
     0,
     true,
     NULL,
     true,
     "global"},
    {"--eval-repl",
     {"--interactive-eval", "--eval-interactive", "-ic", "-ci", NULL},
     NY_REG_PATH,
     NULL,
     "run",
     "Execute inline source code, then enter the REPL",
     "advanced",
     NULL,
     NULL,
     NULL,
     "-ic/-ci",
     NULL,
     0,
     true,
     NULL,
     true,
     "global"},

    {"--mode",
     {NULL},
     NY_REG_ENUM,
     "safe|fast|bare",
     "run",
     "Select runtime safety and compatibility mode",
     "common",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    {"--heap",
     {NULL},
     NY_REG_ENUM,
     "manual|raii|rc|gc",
     "memory",
     "Choose the heap ownership policy",
     "common",
     NULL,
     NULL,
     NULL,
     "-H",
     "H:heap:m=manual,r=raii,c=rc,g=gc,b=borrow,s=strict,z=zero-init",
     0,
     true,
     NULL,
     false,
     "global"},
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-gc", "memory", "Enable tracing GC (legacy spelling)", "--heap=gc"),
    NY_REG_DEPRECATED_BOOL_OPTION("-no-gc", "memory",
                                  "Disable tracing GC (legacy spelling)",
                                  "--heap=manual"),
    NY_REG_DEPRECATED_BOOL_OPTION("--rc-gc", "memory",
                                  "Select reference counting (legacy spelling)",
                                  "--heap=rc"),
    NY_REG_DEPRECATED_BOOL_OPTION("--ownership", "memory",
                                  "Select RAII ownership (legacy spelling)",
                                  "--heap=raii"),
    NY_REG_DEPRECATED_BOOL_OPTION("--raii", "memory",
                                  "Select RAII ownership (legacy spelling)",
                                  "--heap=raii"),
    {"--native-backend",
     {NULL},
     NY_REG_ENUM,
     "llvm|x86_64|x86|aarch64|arm|riscv|wasm|bpf|mips|powerpc|avr|amdgpu",
     "target",
     "Select native code generation backend",
     "common",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    NY_REG_VALUE_OPTION("--native-abi", NY_REG_ENUM, "auto|sysv|win64|aapcs",
                        "target", "Select the native calling convention"),
    NY_REG_VALUE_OPTION("--c-frontend", NY_REG_ENUM, "auto|libclang|nytrix",
                        "build", "Select the C translation frontend"),
    NY_REG_VALUE_OPTION("--type-solver", NY_REG_ENUM, "auto|hm|z3", "check",
                        "Select the type-inference solver"),
    NY_REG_VALUE_OPTION("--proof-solver", NY_REG_ENUM,
                        "none|presburger|z3|auto", "check",
                        "Select the refinement proof solver"),
    NY_REG_VALUE_OPTION("--dump-scope", NY_REG_ENUM, "program|lib|both", "dump",
                        "Choose which source scopes are dumped"),
    {"--opt-level",
     {"-O0", "-O1", "-O2", "-O3", "-Os", "-Oz"},
     NY_REG_ENUM,
     "0|1|2|3|s|z",
     "opt",
     "Set optimization level",
     "common",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     "+release",
     false,
     "global"},
    {"--opt-profile",
     {"--profile", NULL},
     NY_REG_ENUM,
     "speed|balanced|compile|size|none|peak",
     "profile",
     "Choose the optimization profile",
     "common",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     "+perf",
     false,
     "global"},
    NY_REG_FAMILY_TRUSTED_VALUE_OPTION(
        "--emit", NY_REG_LIST,
        "asm[=PATH]|ir[=PATH]|bc[=PATH]|obj[=PATH]|wasm[=PATH]|nyr|"
        "nyr-metadata[=PATH]|report[=PATH]|artifact[=PATH]|"
        "refinement-interface[=PATH]|effect-interface[=PATH]|shapes|"
        "native-bundle[=PATH]",
        "emit", "Emit one or more compiler artifacts", "-E",
        "E:emit:a=asm,i=ir,b=bc,o=obj,w=wasm,n=nyr,r=report,f=artifact,s="
        "shapes"),
    NY_REG_DEPRECATED_VALUE_OPTION(
        "--emit-asm", "emit", "Write native assembly to a path", "--emit=asm"),
    NY_REG_DEPRECATED_VALUE_OPTION(
        "--emit-ir", "emit", "Write intermediate representation to a path",
        "--emit=ir"),
    NY_REG_DEPRECATED_VALUE_OPTION(
        "--emit-bc", "emit", "Write backend bitcode to a path", "--emit=bc"),
    {"--output",
     {"-o"},
     NY_REG_PATH,
     NULL,
     "build",
     "Write the compiled output to a path",
     "common",
     NULL,
     NULL,
     NULL,
     "-o",
     NULL,
     0,
     true,
     "+release",
     true,
     "global"},
    {"--threads",
     {NULL},
     NY_REG_INT,
     NULL,
     "parallel",
     "Set compiler worker count",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    {"--gpu",
     {NULL},
     NY_REG_ENUM,
     "off|auto|opencl",
     "gpu",
     "Choose GPU execution policy",
     "advanced",
     NULL,
     NULL,
     NULL,
     "-G",
     "G:gpu:o=off,a=auto,l=opencl,c=cuda,h=hip,m=metal,f=offload=force,s=async,"
     "x=fast-math",
     0,
     true,
     NULL,
     false,
     "global"},
    {"--gpu-backend",
     {NULL},
     NY_REG_ENUM,
     "none|auto|opencl|cuda|hip|metal",
     "gpu",
     "Select the GPU code-generation backend",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    {"--gpu-offload",
     {NULL},
     NY_REG_ENUM,
     "off|auto|on|force",
     "gpu",
     "Choose when GPU work is offloaded",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    NY_REG_VALUE_OPTION("--gpu-min-work", NY_REG_INT, NULL, "gpu",
                        "Set the minimum workload for GPU offload"),
    NY_REG_VALUE_OPTION(
        "--gpu-opt", NY_REG_LIST,
        "offload=off|offload=auto|offload=on|offload=force|min-work=N|"
        "async|no-async|fast-math|no-fast-math|target=none|target=auto|"
        "target=nvptx|target=amdgpu|target=spirv|target=hsaco|object=auto|"
        "object=none|object=ptx|object=o|object=spv|object=hsaco",
        "gpu", "Set GPU offload and math options"),
    NY_REG_BOOL_OPTION("--gpu-async", "gpu",
                       "Prefer asynchronous GPU dispatch"),
    NY_REG_BOOL_OPTION("--no-gpu-async", "gpu",
                       "Disable asynchronous GPU dispatch"),
    NY_REG_BOOL_OPTION("--gpu-fast-math", "gpu",
                       "Allow relaxed GPU floating-point rules"),
    NY_REG_BOOL_OPTION("--no-gpu-fast-math", "gpu",
                       "Disable relaxed GPU floating-point rules"),
    {"--parallel",
     {NULL},
     NY_REG_ENUM,
     "off|auto|threads|modules",
     "parallel",
     "Select the parallel execution strategy",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    NY_REG_VALUE_OPTION("--parallel-min-work", NY_REG_INT, NULL, "parallel",
                        "Set the minimum workload for threaded execution"),
    {"--target",
     {"--host-triple", NULL},
     NY_REG_PATH,
     NULL,
     "target",
     "Select a target preset or host triple",
     "common",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    {"--sanitize",
     {NULL},
     NY_REG_ENUM,
     "address|undefined|thread|leak",
     "run",
     "Enable runtime sanitizers",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    {"--safe-run",
     {NULL},
     NY_REG_LIST,
     NULL,
     "run",
     "Run with defaults or optional --safe-run=key:value limits",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    {"--watch",
     {"--hot-reload"},
     NY_REG_BOOL,
     NULL,
     "run",
     "Re-run when source files change",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    {"--repl",
     {"-i", NULL},
     NY_REG_BOOL,
     NULL,
     "run",
     "Read source from stdin or start the interactive REPL",
     "common",
     NULL,
     NULL,
     NULL,
     "-i",
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    NY_REG_DEPRECATED_BOOL_OPTION("--interactive", "run",
                                  "Legacy REPL spelling", "--repl"),
    NY_REG_DEPRECATED_BOOL_OPTION("-interactive", "run", "Legacy REPL spelling",
                                  "--repl"),
    NY_REG_DEPRECATED_BOOL_OPTION("-repl", "run", "Legacy REPL spelling",
                                  "--repl"),
    {"--color",
     {NULL},
     NY_REG_ENUM,
     "auto|always|never",
     "info",
     "Choose color output policy",
     "common",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    {"--cross",
     {NULL},
     NY_REG_PATH,
     NULL,
     "target",
     "Cross-compile for a target triple or preset",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    NY_REG_TRUSTED_VALUE_OPTION(
        "--host-cflags", NY_REG_LIST, NULL, "build",
        "Extra host compiler flags (trusted config only)"),
    NY_REG_TRUSTED_VALUE_OPTION(
        "--host-ldflags", NY_REG_LIST, NULL, "build",
        "Extra host linker flags (trusted config only)"),
    {"--trust-config",
     {NULL},
     NY_REG_BOOL,
     NULL,
     "config",
     "Allow project configuration to set trusted path and execution options",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     true,
     "global"},
    {"--completions",
     {NULL},
     NY_REG_ENUM,
     "bash|zsh|fish",
     "info",
     "Generate shell completion definitions",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    {"--warn",
     {NULL},
     NY_REG_ENUM,
     "none|useful|all|error",
     "debug",
     "Set compiler warning level",
     "common",
     NULL,
     NULL,
     NULL,
     "-W",
     "W:warn:n=none,u=useful,a=all,e=error",
     0,
     true,
     NULL,
     false,
     "global"},
    NY_REG_DEPRECATED_BOOL_OPTION("--warn-all", "debug",
                                  "Enable all warnings (legacy spelling)",
                                  "--warn=all"),
    NY_REG_DEPRECATED_BOOL_OPTION("--warn-useful", "debug",
                                  "Enable useful warnings (legacy spelling)",
                                  "--warn=useful"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-warn", "debug",
                                  "Disable warnings (legacy spelling)",
                                  "--warn=none"),
    {"-c",
     {NULL},
     NY_REG_PATH,
     NULL,
     "run",
     "Run source code inline",
     "common",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     true,
     "global"},
    NY_REG_DEPRECATED_BOOL_OPTION(
        "--dce", "opt", "Legacy dead-code elimination toggle", "--opt=dce"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-dce", "opt",
                                  "Legacy dead-code elimination disable",
                                  "--opt=no-dce"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "--opt-dce", "opt", "Legacy dead-code elimination toggle", "--opt=dce"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-opt-dce", "opt",
                                  "Legacy dead-code elimination disable",
                                  "--opt=no-dce"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "--opt-loops", "opt", "Legacy loop optimization toggle", "--opt=loops"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-opt-loops", "opt",
                                  "Legacy loop optimization disable",
                                  "--opt=no-loops"),
    NY_REG_DEPRECATED_BOOL_OPTION("--opt-internalize", "opt",
                                  "Legacy internalization toggle",
                                  "--opt=internalize"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-opt-internalize", "opt",
                                  "Legacy internalization disable",
                                  "--opt=no-internalize"),
    NY_REG_DEPRECATED_BOOL_OPTION("--opt-autotune", "opt",
                                  "Legacy optimizer autotuning toggle",
                                  "--opt=autotune"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-opt-autotune", "opt",
                                  "Legacy optimizer autotuning disable",
                                  "--opt=no-autotune"),
    NY_REG_BOOL_OPTION("--fast", "opt", "Enable fast optimization mode"),
    NY_REG_BOOL_OPTION("--release", "opt",
                       "Enable the size-oriented AOT release preset"),
    NY_REG_BOOL_OPTION("--debug", "debug",
                       "Enable debug symbols and verbose diagnostics"),
    NY_REG_BOOL_OPTION("--debug-everything", "debug",
                       "Enable comprehensive compiler and NYR diagnostics"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-fast-math", "opt",
                                  "Legacy fast-math disable spelling",
                                  "--opt=no-fast-math"),
    NY_REG_DEPRECATED_BOOL_OPTION("-flto", "opt", "Legacy LTO toggle",
                                  "--opt=lto"),
    NY_REG_DEPRECATED_BOOL_OPTION("--llvm-lto", "opt", "Legacy LTO toggle",
                                  "--opt=lto"),
    NY_REG_BOOL_OPTION("--strip", "build", "Strip debug information"),
    NY_REG_BOOL_OPTION("--no-strip", "build", "Keep debug information"),
    NY_REG_DEPRECATED_BOOL_OPTION("-strip", "build",
                                  "Legacy stripped-output spelling", "--strip"),
    NY_REG_DEPRECATED_BOOL_OPTION("-no-strip", "build",
                                  "Legacy unstripped-output spelling",
                                  "--no-strip"),
    NY_REG_BOOL_OPTION("--std-builtin-ops", "build",
                       "Enable standard-library builtin operator lowering"),
    NY_REG_BOOL_OPTION("--no-std-builtin-ops", "build",
                       "Disable standard-library builtin operator lowering"),
    NY_REG_BOOL_OPTION("--compiler-asserts", "debug",
                       "Enable internal compiler invariant checks"),
    NY_REG_BOOL_OPTION("--no-compiler-asserts", "debug",
                       "Disable internal compiler invariant checks"),
    NY_REG_BOOL_OPTION("--debug-locals", "debug",
                       "Emit local and global debug records"),
    NY_REG_BOOL_OPTION("--no-debug-locals", "debug",
                       "Disable local and global debug records"),
    NY_REG_BOOL_OPTION("--dwarf-split-inlining", "debug",
                       "Emit DWARF split-inlining information"),
    NY_REG_BOOL_OPTION("--no-dwarf-split-inlining", "debug",
                       "Disable DWARF split-inlining information"),
    NY_REG_BOOL_OPTION("--dwarf-profile-info", "debug",
                       "Emit profiling-friendly DWARF location information"),
    NY_REG_BOOL_OPTION("--no-dwarf-profile-info", "debug",
                       "Disable profiling-friendly DWARF location information"),
    NY_REG_FAMILY_VALUE_OPTION("--trace", NY_REG_LIST, NY_TRACE_LIST_VALUES,
                               "trace", "Select compiler tracing categories",
                               "-T",
                               "T:trace:t=tokens,p=parse,s=scope,h=hm,u=unify,"
                               "r=trait,l=llvm,g=regalloc,x=exec,m=meta"),
    NY_REG_FAMILY_VALUE_OPTION(
        "--dump", NY_REG_LIST, NY_DUMP_LIST_VALUES, "dump",
        "Select compiler inspection outputs", "-D",
        "D:dump:t=tokens,a=ast,l=llvm,p=llvm-passes,n=nyr,v=vm,f=funcs,e="
        "escapes,q=proofs,o=obj,c=callgraph,b=abi,s=stack,r=regalloc,h=elf,x="
        "expand"),
    NY_REG_BOOL_OPTION("--trace-tokens", "trace", "Trace lexer tokens"),
    NY_REG_BOOL_OPTION("--trace-parse", "trace", "Trace parser rules"),
    NY_REG_BOOL_OPTION("--trace-scope", "trace", "Trace name resolution"),
    NY_REG_BOOL_OPTION("--trace-hm", "trace", "Trace Hindley-Milner inference"),
    NY_REG_BOOL_OPTION("--trace-llvm", "trace", "Trace LLVM lowering"),
    NY_REG_BOOL_OPTION("--trace-regalloc", "trace",
                       "Trace native register allocation"),
    NY_REG_PATH_OPTION("--trace-filter", "trace",
                       "Filter trace output by module or function"),
    NY_REG_BOOL_OPTION("--explain", "debug",
                       "Explain inferred refinement and effect facts"),
    NY_REG_BOOL_OPTION("--explain-any", "debug",
                       "Explain dynamic any-value flows"),
    NY_REG_DEPRECATED_BOOL_OPTION("--move-semantics", "memory",
                                  "Legacy ownership-check spelling",
                                  "--ownership-check=strict"),
    {"--legacy-llvm",
     {NULL},
     NY_REG_BOOL,
     NULL,
     "debug",
     "Removed; use the unified NYIR-to-LLVM backend",
     "deprecated",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    {"--legacy-llvm-dump",
     {NULL},
     NY_REG_BOOL,
     NULL,
     "debug",
     "Removed with the legacy LLVM compatibility backend",
     "deprecated",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    NY_REG_BOOL_OPTION("--dump-both", "dump", "Dump program and library scope"),
    NY_REG_BOOL_OPTION("--dump-lib", "dump", "Dump library scope"),
    NY_REG_BOOL_OPTION("--dump-program", "dump", "Dump program scope"),
    NY_REG_DEPRECATED_BOOL_OPTION("-emit-only", "run",
                                  "Legacy compile-only spelling", "--run=none"),
    NY_REG_TRUSTED_PATH_OPTION("--emit-module", "build",
                               "Emit a compiled module to a path"),
    {"--std-bc",
     {NULL},
     NY_REG_PATH,
     NULL,
     "stdlib",
     "Use a precompiled standard-library bitcode file",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     true,
     "global"},
    {"--core-bc",
     {NULL},
     NY_REG_PATH,
     NULL,
     "stdlib",
     "Legacy standard-library bitcode spelling",
     "deprecated",
     "--std-bc",
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     true,
     "global"},
    NY_REG_BOOL_OPTION("--expand", "dump", "Print AST expansion output"),
    {"--bundle-std",
     {NULL},
     NY_REG_PATH,
     NULL,
     "build",
     "Bundle the standard library in output",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     true,
     "global"},
    {"--bundle-core",
     {NULL},
     NY_REG_PATH,
     NULL,
     "build",
     "Legacy standard-library bundle spelling",
     "deprecated",
     "--bundle-std",
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     true,
     "global"},
    NY_REG_TRUSTED_PATH_OPTION("--bundle-symbols", "build",
                               "Write standard-library symbols to a path"),
    NY_REG_PATH_OPTION("-passes", "opt",
                       "Set a custom LLVM optimization pass pipeline"),
    NY_REG_TRUSTED_PATH_OPTION("--dump-dir", "dump",
                               "Write dump artifacts under a directory"),
    NY_REG_BOOL_OPTION("--compile-profile-fn", "profile",
                       "Include per-function compile profile metadata"),
    NY_REG_BOOL_OPTION("--progress", "info", "Show compiler progress"),
    NY_REG_BOOL_OPTION("--no-progress", "info",
                       "Disable compiler progress output"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "--help-env", "info", "Show environment-variable help", "--help env"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "--env-help", "info", "Show environment-variable help", "--help env"),
    NY_REG_VALUE_OPTION("--glyphs", NY_REG_ENUM, "unicode|ascii", "info",
                        "Choose Unicode or ASCII UI glyphs"),
    NY_REG_TRUSTED_PATH_OPTION("-L", "link", "Add a linker search directory"),
    NY_REG_TRUSTED_PATH_OPTION("-l", "link", "Link against a named library"),
    NY_REG_TRUSTED_PATH_OPTION("--nyr-metadata-bin", "nyr",
                               "Write binary NYR metadata to a path"),
    NY_REG_BOOL_OPTION("--version-id", "info",
                       "Print the build identifier only"),
    {"--std",
     {NULL},
     NY_REG_ENUM,
     "none|minimal|full",
     "stdlib",
     "Select standard-library loading mode",
     "advanced",
     NULL,
     NULL,
     NULL,
     "-S",
     "S:std:n=none,m=minimal,f=full",
     0,
     true,
     NULL,
     false,
     "global"},
    {"-std",
     {NULL},
     NY_REG_ENUM,
     "none|minimal|full",
     "stdlib",
     "Legacy standard-library mode spelling",
     "deprecated",
     "--std",
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    NY_REG_DEPRECATED_BOOL_OPTION("-no-std", "stdlib",
                                  "Legacy standard-library disable spelling",
                                  "--std=none"),
    NY_REG_DEPRECATED_BOOL_OPTION("--full-mod", "stdlib",
                                  "Legacy full standard-library spelling",
                                  "--std=full"),
    {"--std-path",
     {NULL},
     NY_REG_PATH,
     NULL,
     "stdlib",
     "Legacy standard-library path spelling",
     "deprecated",
     "--core-path",
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     true,
     "global"},
    {"--core-path",
     {NULL},
     NY_REG_PATH,
     NULL,
     "stdlib",
     "Set the standard-library source path",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     true,
     "global"},
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-debug", "debug", "Enable legacy debug compiler mode", "--debug"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-verbose", "debug", "Increase legacy compiler verbosity", "--verbose"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-strict-types", "check",
                                  "Legacy type check disable spelling",
                                  "--check=no-types"),
    NY_REG_BOOL_OPTION("--dump-ast", "dump", "Dump the parsed syntax tree"),
    NY_REG_BOOL_OPTION("--extract-json", "dump",
                       "Extract JSON metadata from a source file"),
    NY_REG_BOOL_OPTION("--extract-code", "dump",
                       "Extract code from a source file"),
    NY_REG_PATH_OPTION("--extract-lang", "dump",
                       "Select the language for code extraction"),
    NY_REG_PATH_OPTION("--extract-at", "dump",
                       "Select a line and column for code extraction"),
    NY_REG_PATH_OPTION("--expand-only", "dump",
                       "Filter expansion output by function name"),
    NY_REG_BOOL_OPTION("--expand-json", "dump",
                       "Emit expansion output as JSON"),
    NY_REG_BOOL_OPTION("--meta-trace", "trace",
                       "Trace compile-time metadata evaluation"),
    NY_REG_BOOL_OPTION("--dump-diagnose", "dump",
                       "Dump compiler diagnostics state"),
    NY_REG_BOOL_OPTION("--collect-errors", "debug",
                       "Continue compilation to collect diagnostics"),
    NY_REG_DEPRECATED_BOOL_OPTION("--emit-shapes", "emit",
                                  "Emit shape metadata", "--emit=shapes"),
    NY_REG_BOOL_OPTION("--dump-llvm", "dump", "Dump generated LLVM IR"),
    NY_REG_BOOL_OPTION("--native-tier-report", "native",
                       "Report native tier selection"),
    NY_REG_DEPRECATED_VALUE_OPTION(
        "--emit-report", "profile",
        "Write a compiler performance report to a path", "--emit=report"),
    NY_REG_TRUSTED_OPTIONAL_PATH_OPTION(
        "--dump-nyr", "nyr",
        "Write binary NYR to stdout, or use --dump-nyr=PATH for a file"),
    NY_REG_DEPRECATED_BOOL_OPTION("--nyr-metadata", "nyr",
                                  "Report NYR metadata", "--emit=nyr-metadata"),
    NY_REG_BOOL_OPTION("--nyr-run", "nyr", "Run the NYR interpreter"),
    NY_REG_BOOL_OPTION("--nyr-run-profile", "nyr",
                       "Profile NYR interpreter execution"),
    NY_REG_TRUSTED_PATH_OPTION("--nyr-run-bin", "nyr",
                               "Execute a validated binary NYR dump"),
    NY_REG_VALUE_OPTION("--nyr-run-max-steps", NY_REG_INT, NULL, "nyr",
                        "Limit NYR VM execution steps"),
    NY_REG_VALUE_OPTION("--nyr-run-recursion-limit", NY_REG_INT, NULL, "nyr",
                        "Limit NYR VM recursive call depth"),
    NY_REG_VALUE_OPTION("--nyr-disable-pass", NY_REG_PATH, NULL, "nyr",
                        "Skip one named NYR optimizer pass"),
    NY_REG_VALUE_OPTION("--nyr-stop-after", NY_REG_PATH, NULL, "nyr",
                        "Stop after one named NYR optimizer pass"),
    NY_REG_VALUE_OPTION("--nyr-global-inst-cap", NY_REG_INT, NULL, "nyr",
                        "Limit total NYR optimizer instructions"),
    NY_REG_VALUE_OPTION("--nyr-pass-growth-factor", NY_REG_PATH, NULL, "nyr",
                        "Limit NYR optimizer pass growth"),
    NY_REG_VALUE_OPTION(
        "--nyr-limit", NY_REG_LIST,
        "global-inst-cap=N|pass-growth=X|run-steps=N|run-recursion=N", "nyr",
        "Set NYR optimizer and interpreter resource limits"),
    NY_REG_FAMILY_VALUE_OPTION(
        "--nyr-check", NY_REG_LIST,
        "verify|bisect|rollback|flaky|fingerprint|oracle|oracle-per-pass|"
        "tv-seed=N|fuzz=N",
        "nyr", "Select NYR verification and translation-validation checks",
        "-N",
        "N:nyr-check:v=verify,b=bisect,r=rollback,f=flaky,p=fingerprint,o="
        "oracle,e=oracle-per-pass"),
    NY_REG_VALUE_OPTION("--nyr-pass", NY_REG_LIST,
                        "disable:NAME|stop-after:NAME", "nyr",
                        "Control an individual NYR optimization pass"),
    NY_REG_BOOL_OPTION("--diag-compact", "debug", "Use compact diagnostics"),
    NY_REG_BOOL_OPTION("--diag-rich", "debug", "Use rich diagnostics"),
    NY_REG_BOOL_OPTION("--dump-tokens", "dump", "Dump lexer tokens"),
    NY_REG_BOOL_OPTION("--dump-funcs", "dump", "Dump function summaries"),
    NY_REG_BOOL_OPTION("--callgraph", "dump",
                       "Emit the function call graph (supports JSON/depth)"),
    NY_REG_BOOL_OPTION("--inspect", "dump",
                       "Inspect optimized NYR and pass summaries"),
    NY_REG_DEPRECATED_BOOL_OPTION("--nyr-dump", "nyr",
                                  "Legacy NYR dump spelling", "--dump=nyr"),
    NY_REG_DEPRECATED_VALUE_OPTION("--nyr-dump-to", "nyr",
                                   "Write NYR text to a path", "--emit=nyr"),
    NY_REG_BOOL_OPTION("--report-peak-rss", "profile",
                       "Report peak resident memory"),
    NY_REG_BOOL_OPTION("--dump-on-error", "debug",
                       "Dump compiler state after an error"),
    NY_REG_BOOL_OPTION("-trace", "trace", "Trace program execution"),
    NY_REG_BOOL_OPTION("-verify", "check", "Verify compiled modules"),
    NY_REG_BOOL_OPTION("-g", "debug", "Emit debug symbols"),
    NY_REG_BOOL_OPTION("--plain-repl", "run", "Use plain REPL output"),
    NY_REG_BOOL_OPTION("--zero-init", "memory",
                       "Zero-initialize managed allocations"),
    NY_REG_BOOL_OPTION("--no-zero-init", "memory",
                       "Disable zero-initialization"),
    NY_REG_BOOL_OPTION("--jit-fast-isel", "jit",
                       "Use fast JIT instruction selection"),
    NY_REG_BOOL_OPTION("--no-jit-fast-isel", "jit",
                       "Disable fast JIT instruction selection"),
    NY_REG_BOOL_OPTION("--jit-perf-map", "jit",
                       "Write a Linux perf map for JIT code"),
    NY_REG_BOOL_OPTION("--no-jit-perf-map", "jit",
                       "Disable JIT perf-map output"),
    NY_REG_BOOL_OPTION("--vm-disasm", "dump",
                       "Disassemble virtual-machine bytecode"),
    NY_REG_BOOL_OPTION("--mixed-disasm", "dump",
                       "Disassemble NYIR, MIR, and native code (mixed view)"),
    NY_REG_BOOL_OPTION("--elf-dump", "native", "Dump native ELF details"),
    NY_REG_BOOL_OPTION("--stack-frame", "native",
                       "Report native stack-frame layout"),
    NY_REG_BOOL_OPTION("--regalloc-heat", "native",
                       "Report register-allocation pressure"),
    NY_REG_BOOL_OPTION("--abi", "native", "Report native ABI lowering"),
    NY_REG_VALUE_OPTION("--stop-after", NY_REG_ENUM,
                        "parse|hm|trait|flow|abi|opt", "debug",
                        "Stop after a compiler pipeline stage"),
    {"--timeout",
     {NULL},
     NY_REG_INT,
     NULL,
     "run",
     "Limit program runtime in seconds",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    {"-timeout",
     {NULL},
     NY_REG_INT,
     NULL,
     "run",
     "Legacy runtime timeout spelling",
     "deprecated",
     "--timeout",
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    NY_REG_VALUE_OPTION("--native-tier", NY_REG_ENUM,
                        "auto|baseline|stencil|fast|opt|llvm", "native",
                        "Choose the native execution tier"),
    NY_REG_VALUE_OPTION("--native-tier-budget", NY_REG_INT, NULL, "native",
                        "Set native tier compilation budget"),
    NY_REG_VALUE_OPTION("--native-tuning", NY_REG_LIST,
                        "budget=N|hot=N|cold=N|cache-score=N|prefer=vm|asm",
                        "native",
                        "Set native tiering thresholds and preference"),
    NY_REG_VALUE_OPTION("--comptime-limit", NY_REG_INT, NULL, "build",
                        "Limit compile-time evaluator instructions"),
    NY_REG_VALUE_OPTION("--native-hot-threshold", NY_REG_INT, NULL, "native",
                        "Set native hotness threshold"),
    NY_REG_VALUE_OPTION("--native-cold-threshold", NY_REG_INT, NULL, "native",
                        "Set native cold threshold"),
    NY_REG_VALUE_OPTION("--native-cache-score", NY_REG_INT, NULL, "native",
                        "Set native cache score threshold"),
    NY_REG_BOOL_OPTION("--native-prefer-vm", "native",
                       "Prefer VM execution for cold functions"),
    NY_REG_BOOL_OPTION("--native-prefer-asm", "native",
                       "Prefer native assembly for hot functions"),
    NY_REG_BOOL_OPTION("--native-enable-cf-mem2reg", "native",
                       "Enable control-flow mem2reg promotion"),
    NY_REG_BOOL_OPTION("--native-disable-cf-mem2reg", "native",
                       "Disable control-flow mem2reg promotion"),
    NY_REG_BOOL_OPTION("--native-result-oracle", "nyr",
                       "Compare native results with the reference executor"),
    NY_REG_BOOL_OPTION("--native-oracle-per-pass", "nyr",
                       "Run the native result oracle after every pass"),
    NY_REG_VALUE_OPTION("--native-oracle-fuzz", NY_REG_INT, NULL, "nyr",
                        "Run the native result oracle for N random inputs"),
    NY_REG_TRUSTED_PATH_OPTION("--native-profile-use", "profile",
                               "Load native optimization profile data"),
    {"--native-tv-seed",
     {NULL},
     NY_REG_INT,
     NULL,
     "nyr",
     "Set native translation-validation seed trials",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    NY_REG_VALUE_OPTION("--jit-opt-level", NY_REG_INT, "0|1|2|3", "jit",
                        "Set JIT optimization level"),
    NY_REG_VALUE_OPTION("--jit-code-model", NY_REG_ENUM,
                        "small|kernel|medium|large", "jit",
                        "Set the JIT code model"),
    NY_REG_VALUE_OPTION("--jit-engine", NY_REG_ENUM, "orc|mcjit", "jit",
                        "Select the JIT engine"),
    NY_REG_FAMILY_VALUE_OPTION(
        "--jit-opt", NY_REG_LIST,
        "engine=orc|engine=mcjit|fast-isel|no-fast-isel|opt-level=0|"
        "opt-level=1|opt-level=2|opt-level=3|code-model=small|"
        "code-model=kernel|code-model=medium|code-model=large|"
        "perf-map|no-perf-map",
        "jit", "Set JIT engine, optimization, and profiling options", "-J",
        "J:jit:o=engine=orc,m=engine=mcjit,f=fast-isel,p=perf-map"),
    NY_REG_VALUE_OPTION("--accel-target", NY_REG_ENUM,
                        "none|auto|nvptx|amdgpu|spirv|hsaco", "gpu",
                        "Select the accelerator code-generation target"),
    NY_REG_VALUE_OPTION("--accel-object", NY_REG_ENUM,
                        "auto|none|ptx|o|spv|hsaco", "gpu",
                        "Select the accelerator object format"),
    NY_REG_VALUE_OPTION(
        "--gpu-target", NY_REG_ENUM,
        "off|none|auto|cuda|nvptx|ptx|hip|rocm|amdgpu|opencl|spirv|vulkan|spv|"
        "metal|hsaco|hsa|hsa-code-object|hsa_code_object|gcn|rdna",
        "gpu", "Select a compatible GPU target"),
    NY_REG_VALUE_OPTION("--arm-float-abi", NY_REG_ENUM, "soft|softfp|hard",
                        "target",
                        "Select the ARM floating-point calling convention"),
    NY_REG_VALUE_OPTION("--dwarf-version", NY_REG_INT, "2|3|4|5", "debug",
                        "Select the DWARF version"),
    NY_REG_VALUE_OPTION(
        "--dwarf", NY_REG_LIST,
        "version=2..5|locals|no-locals|split-inlining|no-split-inlining|"
        "profile-info|no-profile-info",
        "debug", "Set DWARF version and debug-record options"),
    NY_REG_VALUE_OPTION("--dbg-format", NY_REG_ENUM, "text|json", "debug",
                        "Select debug trace output format"),
    NY_REG_VALUE_OPTION("--ny-desugar", NY_REG_ENUM, "ny|c|off", "debug",
                        "Select the source desugaring mode"),
    {"--verbose",
     {"-v", NULL},
     NY_REG_BOOL,
     NULL,
     "common",
     "Increase compiler verbosity",
     "common",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-vv", "debug", "Legacy double-verbosity spelling", "--verbose"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-vvv", "debug", "Legacy triple-verbosity spelling", "--verbose"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-vvvv", "debug", "Legacy deep-verbosity spelling", "--verbose"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-vvvvv", "debug", "Legacy deep-verbosity spelling", "--verbose"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-vvvvvv", "debug", "Legacy deep-verbosity spelling", "--verbose"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "--safe-mode", "check", "Legacy strict-safety spelling", "--mode=safe"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "-safe-mode", "check", "Legacy strict-safety spelling", "--mode=safe"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "--safe", "check", "Legacy strict-safety spelling", "--mode=safe"),
    NY_REG_BOOL_OPTION("--strict", "check", "Enable strict check presets"),
    NY_REG_DEPRECATED_BOOL_OPTION("--strict-types", "check",
                                  "Legacy strict type check spelling",
                                  "--check=types"),
    NY_REG_DEPRECATED_BOOL_OPTION("--effect-require-known", "check",
                                  "Legacy effect check spelling",
                                  "--check=effects"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-effect-require-known", "check",
                                  "Legacy effect check disable spelling",
                                  "--check=no-effects"),
    NY_REG_DEPRECATED_BOOL_OPTION("--alias-require-known", "check",
                                  "Legacy alias check spelling",
                                  "--check=alias"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-alias-require-known", "check",
                                  "Legacy alias check disable spelling",
                                  "--check=no-alias"),
    NY_REG_DEPRECATED_BOOL_OPTION("--alias-require-no-escape", "check",
                                  "Legacy alias escape check spelling",
                                  "--check=alias-escape"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-alias-require-no-escape", "check",
                                  "Legacy alias escape check disable spelling",
                                  "--check=no-alias-escape"),
    NY_REG_DEPRECATED_BOOL_OPTION("--ownership-strict", "memory",
                                  "Legacy strict ownership spelling",
                                  "--ownership-check=strict"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-ownership-strict", "memory",
                                  "Legacy strict ownership disable spelling",
                                  "--ownership-check=advisory"),
    NY_REG_DEPRECATED_BOOL_OPTION("--borrow-check", "memory",
                                  "Legacy advisory borrow spelling",
                                  "--ownership-check=advisory"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-borrow-check", "memory",
                                  "Legacy borrow disable spelling",
                                  "--ownership-check=off"),
    NY_REG_DEPRECATED_BOOL_OPTION("--no-ownership", "memory",
                                  "Legacy ownership disable spelling",
                                  "--check=no-ownership"),
    {"--ownership-check",
     {NULL},
     NY_REG_ENUM,
     "off|advisory|strict",
     "memory",
     "Set the ownership diagnostic level",
     "advanced",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    NY_REG_DEPRECATED_BOOL_OPTION("--warn-shadow", "check",
                                  "Legacy shadow-check spelling",
                                  "--check=shadow"),
    NY_REG_BOOL_OPTION(
        "--list-fallbacks", "info",
        "List fallback implementations selected by the compiler"),
    NY_REG_DEPRECATED_BOOL_OPTION("-w", "check", "Disable warnings",
                                  "--warn=none"),
    NY_REG_PATH_OPTION("--entry-name", "emit",
                       "Rename the program entry point"),
    NY_REG_DEPRECATED_OPTIONAL_PATH_OPTION(
        "--emit-wasm", "emit",
        "Write WebAssembly output (requires Wasm target mode)", "--emit=wasm"),
    NY_REG_BOOL_OPTION("--ir-full", "dump",
                       "Include standard-library definitions in IR output"),
    NY_REG_BOOL_OPTION("--ir-no-std", "dump",
                       "Exclude standard-library definitions from IR output"),
    NY_REG_BOOL_OPTION("--profile-time", "profile",
                       "Benchmark compiler time across repeated runs"),
    NY_REG_BOOL_OPTION("--no-gprof", "profile",
                       "Disable gprof instrumentation"),
    NY_REG_BOOL_OPTION("--clean-cache", "stdlib",
                       "Remove compiler, standard-library, and JIT caches"),
    NY_REG_PATH_OPTION("--wasm-info", "info",
                       "Inspect imports and exports in a WebAssembly module"),
    NY_REG_BOOL_OPTION("--nyr-verify", "nyr",
                       "Verify NYR after every optimization pass"),
    NY_REG_BOOL_OPTION("--nyr-bisect", "nyr",
                       "Bisect a failing NYR optimization sequence"),
    NY_REG_BOOL_OPTION("--nyr-fingerprint", "nyr",
                       "Print an NYR execution fingerprint"),
    NY_REG_BOOL_OPTION("--nyr-flaky-check", "nyr",
                       "Check NYR execution for nondeterministic results"),
    NY_REG_BOOL_OPTION("--nyr-rollback-on-verify-fail", "nyr",
                       "Roll back a pass that fails NYR verification"),
    NY_REG_BOOL_OPTION("--delta-only", "trace",
                       "Show only changed debug trace facts"),
    NY_REG_BOOL_OPTION("--unify-trace", "trace", "Trace unification decisions"),
    NY_REG_BOOL_OPTION("--trait-resolve", "trace", "Trace trait resolution"),
    NY_REG_TRUSTED_VALUE_OPTION(
        "--use", NY_REG_LIST,
        "artifact=PATH|refinement-interface=PATH|effect-interface=PATH|"
        "native-bundle=PATH",
        "artifact", "Load one or more validated compilation inputs"),
    NY_REG_DEPRECATED_VALUE_OPTION("--emit-artifact", "artifact",
                                   "Write a selected compiler-stage artifact",
                                   "--emit=artifact"),
    NY_REG_TRUSTED_PATH_OPTION(
        "--verify-artifact", "artifact",
        "Validate a stage artifact against expanded input"),
    NY_REG_TRUSTED_PATH_OPTION("--use-artifact", "artifact",
                               "Reuse a validated compiler-stage artifact"),
    NY_REG_DEPRECATED_VALUE_OPTION("--emit-refinement-interface", "artifact",
                                   "Write a versioned refinement interface",
                                   "--emit=refinement-interface"),
    NY_REG_TRUSTED_PATH_OPTION("--use-refinement-interface", "artifact",
                               "Load a versioned refinement interface"),
    NY_REG_DEPRECATED_VALUE_OPTION(
        "--emit-effect-interface",
        "artifact", "Write serialized effect rows for exported functions",
        "--emit=effect-interface"),
    NY_REG_TRUSTED_PATH_OPTION(
        "--use-effect-interface", "artifact",
        "Load serialized effect rows for imported functions"),
    {"--jit",
     {NULL},
     NY_REG_BOOL,
     NULL,
     "run",
     "Select LLVM JIT execution",
     "deprecated",
     "--run=jit",
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    {"--prof",
     {NULL},
     NY_REG_LIST,
     "timing|stats|gprof|compile|peak-rss",
     "profile",
     "Select compiler and runtime profiling features",
     "advanced",
     NULL,
     NULL,
     NULL,
     "-P",
     "P:prof:t=timing,s=stats,g=gprof,c=compile,r=peak-rss",
     0,
     true,
     NULL,
     false,
     "global"},
    {"-prof",
     {NULL},
     NY_REG_LIST,
     "timing|stats|gprof|compile|peak-rss",
     "profile",
     "Legacy profiling spelling",
     "deprecated",
     "--prof",
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     true,
     NULL,
     false,
     "global"},
    {"--profile-gprof",
     {"--gprof", "-pg", NULL},
     NY_REG_BOOL,
     NULL,
     "profile",
     "Enable gprof instrumentation",
     "deprecated",
     "--prof=gprof",
     NULL,
     NULL,
     NULL,
     NULL,
     0,
     false,
     NULL,
     false,
     "global"},
    NY_REG_BOOL_OPTION("-time", "profile",
                       "Show timing for each compiler phase"),
    NY_REG_VALUE_OPTION("--perf", NY_REG_ENUM, "compact|smart|verbose",
                        "profile",
                        "Select function timing and allocation profiling"),
    NY_REG_FAMILY_VALUE_OPTION(
        "--opt", NY_REG_LIST,
        "dce|loops|internalize|autotune|lto|fast-math|no-dce|no-loops|"
        "no-internalize|no-autotune|no-lto|no-fast-math",
        "opt", "Select optimizer passes and features", "-O",
        "O:opt:d=dce,l=loops,i=internalize,t=autotune,f=fast-math,L=lto"),
    NY_REG_FAMILY_VALUE_OPTION(
        "--check", NY_REG_LIST,
        "types|effects|alias|alias-escape|ownership|shadow|no-types|"
        "no-effects|no-alias|no-alias-escape|no-ownership|no-shadow",
        "check", "Select static checks and warnings", "-C",
        "C:check:t=types,e=effects,a=alias,x=alias-escape,o=ownership,s=shadow,"
        "S=strict,F=safe"),
    NY_REG_DEPRECATED_BOOL_OPTION(
        "--fast-math", "opt", "Legacy fast-math spelling", "--opt=fast-math"),
};

#undef NY_REG_BOOL_OPTION
#undef NY_REG_PATH_OPTION
#undef NY_REG_OPTIONAL_PATH_OPTION
#undef NY_REG_VALUE_OPTION
#undef NY_REG_FAMILY_VALUE_OPTION
#undef NY_REG_FAMILY_TRUSTED_VALUE_OPTION
#undef NY_TRACE_LIST_VALUES
#undef NY_DUMP_LIST_VALUES

static void ny_registry_write_json_string(FILE *out, const char *text) {
  fputc('"', out);
  for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;
       ++p) {
    if (*p == '"' || *p == '\\') {
      fputc('\\', out);
      fputc(*p, out);
    } else if (*p == '\n') {
      fputs("\\n", out);
    } else if (*p == '\r') {
      fputs("\\r", out);
    } else if (*p == '\t') {
      fputs("\\t", out);
    } else if (*p < 0x20) {
      fprintf(out, "\\u%04x", *p);
    } else {
      fputc(*p, out);
    }
  }
  fputc('"', out);
}

static const char *ny_registry_kind_name(ny_registry_kind_t kind) {
  switch (kind) {
  case NY_REG_BOOL:
    return "bool";
  case NY_REG_ENUM:
    return "enum";
  case NY_REG_INT:
    return "int";
  case NY_REG_PATH:
    return "path";
  case NY_REG_LIST:
    return "list";
  }
  return "unknown";
}

static bool ny_registry_has_duplicate_spellings(void) {
  size_t count = sizeof(ny_option_registry) / sizeof(ny_option_registry[0]);
  for (size_t i = 0; i < count; ++i) {
    for (size_t slot = 0; slot <= 8; ++slot) {
      const char *spelling = slot == 0
                                 ? ny_option_registry[i].name
                                 : ny_option_registry[i].aliases[slot - 1];
      if (!spelling)
        continue;
      for (size_t previous = 0; previous <= i; ++previous) {
        for (size_t prior_slot = 0; prior_slot <= 8; ++prior_slot) {
          if (previous == i && prior_slot >= slot)
            break;
          const char *prior =
              prior_slot == 0
                  ? ny_option_registry[previous].name
                  : ny_option_registry[previous].aliases[prior_slot - 1];
          if (prior && strcmp(spelling, prior) == 0) {
            fprintf(stderr, "ny: duplicate option registry spelling '%s'\n",
                    spelling);
            return true;
          }
        }
      }
    }
  }
  return false;
}

static const ny_option_registry_entry_t *
ny_registry_find_spelling(const char *spelling, size_t length);

static bool ny_registry_short_family(const ny_option_registry_entry_t *entry,
                                     char *family, const char **name,
                                     const char **items) {
  if (!entry || !entry->family || !entry->family[0] ||
      !isupper((unsigned char)entry->family[0]) || entry->family[1] != ':')
    return false;
  const char *name_start = entry->family + 2;
  const char *items_start = strchr(name_start, ':');
  if (!items_start || items_start == name_start || !items_start[1])
    return false;
  *family = entry->family[0];
  *name = name_start;
  *items = items_start + 1;
  return true;
}

static int ny_registry_short_item_next(const char **cursor, char *letter,
                                       char *value, size_t value_capacity) {
  const char *p = *cursor;
  if (!p || !*p)
    return 0;
  const char *start = p;
  if (!isalpha((unsigned char)*p++))
    return -1;
  *letter = start[0];
  if (*p++ != '=')
    return -1;
  const char *value_start = p;
  while (*p && *p != ',')
    ++p;
  size_t length = (size_t)(p - value_start);
  if (!length || length >= value_capacity)
    return -1;
  memcpy(value, value_start, length);
  value[length] = '\0';
  if (*p == ',') {
    ++p;
    if (!*p)
      return -1;
  }
  *cursor = p;
  return 1;
}

static bool ny_registry_has_invalid_short_items(void) {
  const size_t count =
      sizeof(ny_option_registry) / sizeof(ny_option_registry[0]);
  for (size_t i = 0; i < count; ++i) {
    const ny_option_registry_entry_t *entry = &ny_option_registry[i];
    if (!entry->family)
      continue;
    char family = '\0';
    const char *name = NULL;
    const char *items = NULL;
    if (!ny_registry_short_family(entry, &family, &name, &items) ||
        !entry->short_form || entry->short_form[0] != '-' ||
        entry->short_form[1] != family || entry->short_form[2]) {
      fprintf(stderr, "ny: invalid short-option metadata for '%s'\n",
              entry->name);
      return true;
    }
    for (size_t previous = 0; previous < i; ++previous) {
      char prior_family = '\0';
      const char *prior_name = NULL;
      const char *prior_items = NULL;
      if (ny_registry_short_family(&ny_option_registry[previous], &prior_family,
                                   &prior_name, &prior_items) &&
          prior_family == family) {
        fprintf(stderr, "ny: duplicate short-option family '%c'\n", family);
        return true;
      }
    }
    const char *cursor = items;
    char letter = '\0';
    char value[256];
    bool seen_items[256] = {false};
    while (true) {
      int status =
          ny_registry_short_item_next(&cursor, &letter, value, sizeof(value));
      if (status == 0)
        break;
      if (status < 0) {
        fprintf(stderr, "ny: malformed short-option family '%c'\n", family);
        return true;
      }
      unsigned char item_index = (unsigned char)letter;
      if (seen_items[item_index]) {
        fprintf(stderr, "ny: duplicate short-option item '%c%c'\n", family,
                letter);
        return true;
      }
      seen_items[item_index] = true;
    }
    char spelling[3] = {'-', family, '\0'};
    const ny_option_registry_entry_t *legacy =
        ny_registry_find_spelling(spelling, sizeof(spelling) - 1);
    if (legacy) {
      fprintf(stderr,
              "ny: short-option family '%c' collides with registered legacy "
              "spelling '%s'\n",
              family, legacy->name);
      return true;
    }
    (void)name;
  }
  return false;
}

static const ny_option_registry_entry_t *
ny_registry_find_spelling(const char *spelling, size_t length) {
  size_t count = sizeof(ny_option_registry) / sizeof(ny_option_registry[0]);
  for (size_t i = 0; i < count; ++i) {
    const ny_option_registry_entry_t *entry = &ny_option_registry[i];
    if (strlen(entry->name) == length &&
        strncmp(entry->name, spelling, length) == 0)
      return entry;
    for (size_t alias = 0;
         alias < sizeof(entry->aliases) / sizeof(entry->aliases[0]); ++alias) {
      if (entry->aliases[alias] && strlen(entry->aliases[alias]) == length &&
          strncmp(entry->aliases[alias], spelling, length) == 0)
        return entry;
    }
  }
  return NULL;
}

static const char *ny_registry_allowed_values_for(const char *option) {
  if (!option)
    return NULL;
  const ny_option_registry_entry_t *entry =
      ny_registry_find_spelling(option, strlen(option));
  return entry ? entry->allowed_values : NULL;
}

static void ny_registry_check_parsed_spelling(bool enabled,
                                              const char *spelling) {
  if (!enabled || !spelling || spelling[0] != '-' || strcmp(spelling, "-") == 0)
    return;
  const char *equals = strchr(spelling, '=');
  size_t length = equals ? (size_t)(equals - spelling) : strlen(spelling);
  const ny_option_registry_entry_t *entry =
      ny_registry_find_spelling(spelling, length);
  if (!entry && spelling[1] && (spelling[1] == 'L' || spelling[1] == 'l'))
    entry = ny_registry_find_spelling(spelling, 2);
  if (!entry) {
    fprintf(stderr,
            "ny: accepted option spelling is missing from the registry: %s\n",
            spelling);
    exit(70);
  }
}

static void ny_options_print_registry_json(void) {
  if (ny_registry_has_duplicate_spellings() ||
      ny_registry_has_invalid_short_items()) {
    fputs("ny: internal option registry has duplicate or invalid entries\n",
          stderr);
    exit(2);
  }
  FILE *out = stdout;
  fputs("{\"options\":[", out);
  size_t count = sizeof(ny_option_registry) / sizeof(ny_option_registry[0]);
  for (size_t i = 0; i < count; ++i) {
    const ny_option_registry_entry_t *entry = &ny_option_registry[i];
    if (i)
      fputc(',', out);
    fputs("{\"name\":", out);
    ny_registry_write_json_string(out, entry->name);
    fputs(",\"aliases\":[", out);
    bool first_alias = true;
    for (size_t alias = 0;
         alias < sizeof(entry->aliases) / sizeof(entry->aliases[0]); ++alias) {
      if (!entry->aliases[alias])
        continue;
      if (!first_alias)
        fputc(',', out);
      ny_registry_write_json_string(out, entry->aliases[alias]);
      first_alias = false;
    }
    fputs("],\"kind\":", out);
    ny_registry_write_json_string(out, ny_registry_kind_name(entry->kind));
    fputs(",\"allowed_values\":", out);
    if (entry->allowed_values)
      ny_registry_write_json_string(out, entry->allowed_values);
    else
      fputs("null", out);
    fputs(",\"group\":", out);
    ny_registry_write_json_string(out, entry->group);
    fputs(",\"summary\":", out);
    ny_registry_write_json_string(out, entry->summary);
    fputs(",\"visibility\":", out);
    ny_registry_write_json_string(out, entry->visibility);
    fputs(",\"deprecated_by\":", out);
    if (entry->deprecated_by)
      ny_registry_write_json_string(out, entry->deprecated_by);
    else
      fputs("null", out);
    fputs(",\"conflicts\":", out);
    if (entry->conflicts)
      ny_registry_write_json_string(out, entry->conflicts);
    else
      fputs("null", out);
    fputs(",\"requires\":", out);
    if (entry->requires)
      ny_registry_write_json_string(out, entry->requires);
    else
      fputs("null", out);
    fputs(",\"short\":", out);
    if (entry->short_form)
      ny_registry_write_json_string(out, entry->short_form);
    else
      fputs("null", out);
    fputs(",\"family\":", out);
    char family = '\0';
    const char *family_name = NULL;
    const char *items = NULL;
    if (ny_registry_short_family(entry, &family, &family_name, &items))
      ny_registry_write_json_string(out, (char[2]){family, '\0'});
    else
      fputs("null", out);
    (void)family_name;
    fputs(",\"short_items\":[", out);
    if (items) {
      const char *cursor = items;
      char letter = '\0';
      char value[256];
      bool first_item = true;
      while (ny_registry_short_item_next(&cursor, &letter, value,
                                         sizeof(value)) > 0) {
        if (!first_item)
          fputc(',', out);
        fputs("{\"letter\":", out);
        ny_registry_write_json_string(out, (char[2]){letter, '\0'});
        fputs(",\"value\":", out);
        ny_registry_write_json_string(out, value);
        fputc('}', out);
        first_item = false;
      }
    }
    fputc(']', out);
    fputs(",\"item_letter\":", out);
    if (entry->item_letter)
      ny_registry_write_json_string(out, (char[2]){entry->item_letter, '\0'});
    else
      fputs("null", out);
    fprintf(out, ",\"takes_value\":%s,\"preset\":",
            entry->takes_value ? "true" : "false");
    if (entry->preset)
      ny_registry_write_json_string(out, entry->preset);
    else
      fputs("null", out);
    fprintf(out, ",\"config_trust\":%s,\"scope\":",
            entry->config_trust ? "true" : "false");
    ny_registry_write_json_string(out, entry->scope);
    fputc('}', out);
  }
  fputs("]}\n", out);
}

static void ny_options_print_completions(const char *shell) {
  if (strcmp(shell, "bash") == 0) {
    fputs("_nytrix() {\n"
          "  local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
          "  local prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
          "  case \"$prev\" in\n",
          stdout);
    for (size_t i = 0;
         i < sizeof(ny_option_registry) / sizeof(ny_option_registry[0]); ++i) {
      const ny_option_registry_entry_t *entry = &ny_option_registry[i];
      if (strcmp(entry->visibility, "deprecated") == 0 || !entry->takes_value ||
          !entry->allowed_values)
        continue;
      fprintf(stdout, "    %s", entry->name);
      for (size_t j = 0; j < sizeof(entry->aliases) / sizeof(entry->aliases[0]);
           ++j)
        if (entry->aliases[j])
          fprintf(stdout, "|%s", entry->aliases[j]);
      fputs(")\n      COMPREPLY=( $(compgen -W '", stdout);
      for (const char *value = entry->allowed_values; *value; ++value)
        fputc(*value == '|' ? ' ' : *value, stdout);
      fputs("' -- \"$cur\") )\n      return;;\n", stdout);
    }
    fputs("  esac\n"
          "  COMPREPLY=( $(compgen -W '",
          stdout);
    for (size_t i = 0;
         i < sizeof(ny_option_registry) / sizeof(ny_option_registry[0]); ++i) {
      const ny_option_registry_entry_t *entry = &ny_option_registry[i];
      if (strcmp(entry->visibility, "deprecated") == 0)
        continue;
      fprintf(stdout, "%s ", entry->name);
      for (size_t j = 0; j < sizeof(entry->aliases) / sizeof(entry->aliases[0]);
           ++j)
        if (entry->aliases[j])
          fprintf(stdout, "%s ", entry->aliases[j]);
    }
    fputs("' -- \"$cur\") )\n"
          "}\n"
          "complete -F _nytrix ny\n",
          stdout);
  } else if (strcmp(shell, "zsh") == 0) {
    fputs("#compdef ny\n_arguments '*:option:(", stdout);
    for (size_t i = 0;
         i < sizeof(ny_option_registry) / sizeof(ny_option_registry[0]); ++i) {
      const ny_option_registry_entry_t *entry = &ny_option_registry[i];
      if (strcmp(entry->visibility, "deprecated") == 0)
        continue;
      fprintf(stdout, "%s ", ny_option_registry[i].name);
      for (size_t j = 0; j < sizeof(entry->aliases) / sizeof(entry->aliases[0]);
           ++j)
        if (entry->aliases[j])
          fprintf(stdout, "%s ", entry->aliases[j]);
    }
    fputs(")'\n", stdout);
  } else {
    for (size_t i = 0;
         i < sizeof(ny_option_registry) / sizeof(ny_option_registry[0]); ++i) {
      const ny_option_registry_entry_t *entry = &ny_option_registry[i];
      if (strcmp(entry->visibility, "deprecated") == 0)
        continue;
      if (strncmp(entry->name, "--", 2) == 0) {
        fprintf(stdout, "complete -c ny -l %s -d '%s'%s", entry->name + 2,
                entry->summary, entry->takes_value ? " -r" : "");
        if (entry->allowed_values) {
          fputs(" -f -a '", stdout);
          for (const char *value = entry->allowed_values; *value; ++value)
            fputc(*value == '|' ? ' ' : *value, stdout);
          fputc('\'', stdout);
        }
        fputc('\n', stdout);
      } else if (entry->name[0] == '-' && entry->name[1] && !entry->name[2])
        fprintf(stdout, "complete -c ny -s %c -d '%s'%s\n", entry->name[1],
                entry->summary, entry->takes_value ? " -r" : "");
      for (size_t j = 0; j < sizeof(entry->aliases) / sizeof(entry->aliases[0]);
           ++j) {
        const char *alias = entry->aliases[j];
        if (!alias)
          continue;
        if (strncmp(alias, "--", 2) == 0) {
          fprintf(stdout, "complete -c ny -l %s -d '%s'%s", alias + 2,
                  entry->summary, entry->takes_value ? " -r" : "");
          if (entry->allowed_values) {
            fputs(" -f -a '", stdout);
            for (const char *value = entry->allowed_values; *value; ++value)
              fputc(*value == '|' ? ' ' : *value, stdout);
            fputc('\'', stdout);
          }
          fputc('\n', stdout);
        } else if (alias[0] == '-' && alias[1] && !alias[2])
          fprintf(stdout, "complete -c ny -s %c -d '%s'%s\n", alias[1],
                  entry->summary, entry->takes_value ? " -r" : "");
      }
    }
  }
}

static void ny_options_print_short_help(const char *prog) {
  fprintf(stdout,
          "%sNytrix compiler%s\n"
          "%sUsage:%s %s [flags] SCRIPT [-- PROGRAM_ARGS...]\n"
          "       %s [flags] -c CODE [-- PROGRAM_ARGS...]\n"
          "       %s -h <topic> | --help <topic> | --help all | "
          "--help=json | --completions SHELL\n\n"
          "%sCommon flags%s\n",
          clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET),
          clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET), prog, prog, prog,
          clr(NY_CLR_BOLD NY_CLR_MAGENTA), clr(NY_CLR_RESET));
  for (size_t i = 0;
       i < sizeof(ny_option_registry) / sizeof(ny_option_registry[0]); ++i) {
    const ny_option_registry_entry_t *entry = &ny_option_registry[i];
    if (strcmp(entry->visibility, "common") != 0)
      continue;
    const char *value_suffix = entry->kind == NY_REG_PATH && !entry->takes_value
                                   ? "[=PATH]"
                               : entry->takes_value ? "=VALUE"
                                                    : "";
    const char *value_color =
        (entry->takes_value ||
         (entry->kind == NY_REG_PATH && !entry->takes_value))
            ? clr(NY_CLR_YELLOW)
            : "";
    fprintf(stdout, "  %s%s%s%s%s", clr(NY_CLR_CYAN), entry->name,
            clr(NY_CLR_RESET), value_color, value_suffix);
    if (entry->takes_value ||
        (entry->kind == NY_REG_PATH && !entry->takes_value))
      fputs(clr(NY_CLR_RESET), stdout);
    size_t option_width = strlen(entry->name) + strlen(value_suffix);
    for (size_t padding = option_width < 28 ? 28 - option_width : 0;
         padding > 0; --padding)
      fputc(' ', stdout);
    fprintf(stdout, " %s%s%s\n", clr(NY_CLR_GRAY), entry->summary,
            clr(NY_CLR_RESET));
  }
  fprintf(
      stdout,
      "\n%sHelp topics%s: %srun check memory opt gpu target emit dump trace "
      "debug\n"
      "              nyr native std profile args short env info\n"
      "              stdlib cache presets config all json completions%s\n"
      "Use '%sny --help <topic>%s' for accepted values and detailed "
      "references.\n"
      "After SCRIPT or -c CODE, arguments belong to the program; one -- "
      "is consumed.\n"
      "%sExamples%s:\n",
      clr(NY_CLR_BOLD NY_CLR_MAGENTA), clr(NY_CLR_RESET), clr(NY_CLR_CYAN),
      clr(NY_CLR_RESET), clr(NY_CLR_CYAN), clr(NY_CLR_RESET),
      clr(NY_CLR_BOLD NY_CLR_MAGENTA), clr(NY_CLR_RESET));
  fprintf(stdout, "  %s%s%s hello.ny\n", clr(NY_CLR_CYAN), prog,
          clr(NY_CLR_RESET));
  fprintf(stdout, "  %s%s%s --run=aot hello.ny\n", clr(NY_CLR_CYAN), prog,
          clr(NY_CLR_RESET));
  fprintf(stdout, "  %s%s%s -c 'print(\"hello\")'\n", clr(NY_CLR_CYAN), prog,
          clr(NY_CLR_RESET));
}

static bool ny_options_print_registry_entry(const char *name) {
  for (size_t i = 0;
       i < sizeof(ny_option_registry) / sizeof(ny_option_registry[0]); ++i) {
    const ny_option_registry_entry_t *entry = &ny_option_registry[i];
    bool matches = strcmp(name, entry->name) == 0;
    for (size_t alias = 0;
         !matches && alias < sizeof(entry->aliases) / sizeof(entry->aliases[0]);
         ++alias)
      matches =
          entry->aliases[alias] && strcmp(name, entry->aliases[alias]) == 0;
    if (!matches)
      continue;
    fprintf(stdout, "%s%s%s", clr(NY_CLR_BOLD NY_CLR_CYAN), entry->name,
            clr(NY_CLR_RESET));
    if (entry->takes_value)
      fprintf(stdout, "%s=VALUE%s", clr(NY_CLR_YELLOW), clr(NY_CLR_RESET));
    else if (entry->kind == NY_REG_PATH && !entry->takes_value)
      fprintf(stdout, "%s[=PATH]%s", clr(NY_CLR_YELLOW), clr(NY_CLR_RESET));
    fprintf(stdout, "\n  %s%s%s\n", clr(NY_CLR_GRAY), entry->summary,
            clr(NY_CLR_RESET));
    if (entry->allowed_values)
      fprintf(stdout, "  %sValues:%s %s%s%s\n", clr(NY_CLR_BOLD),
              clr(NY_CLR_RESET), clr(NY_CLR_YELLOW), entry->allowed_values,
              clr(NY_CLR_RESET));
    if (entry->deprecated_by)
      fprintf(stdout, "  Deprecated; use %s.\n", entry->deprecated_by);
    return true;
  }
  return false;
}

static void
ny_options_print_registry_short(const ny_option_registry_entry_t *entry) {
  char family = '\0';
  const char *name = NULL;
  const char *items = NULL;
  if (!ny_registry_short_family(entry, &family, &name, &items))
    return;
  (void)name;
  fprintf(stdout, "    %sShort:%s -%c", clr(NY_CLR_BOLD), clr(NY_CLR_RESET),
          family);
  const char *cursor = items;
  char letter = '\0';
  char value[256];
  while (ny_registry_short_item_next(&cursor, &letter, value, sizeof(value)) >
         0)
    fprintf(stdout, " %c=%s", letter, value);
  fputc('\n', stdout);
}

static void
ny_options_print_registry_row(const ny_option_registry_entry_t *entry) {
  size_t width = strlen(entry->name) + (entry->takes_value           ? 6u
                                        : entry->kind == NY_REG_PATH ? 7u
                                                                     : 0u);
  fprintf(stdout, "  %s%s%s", clr(NY_CLR_CYAN), entry->name, clr(NY_CLR_RESET));
  if (entry->takes_value)
    fprintf(stdout, "%s=VALUE%s", clr(NY_CLR_YELLOW), clr(NY_CLR_RESET));
  else if (entry->kind == NY_REG_PATH && !entry->takes_value)
    fprintf(stdout, "%s[=PATH]%s", clr(NY_CLR_YELLOW), clr(NY_CLR_RESET));
  for (size_t padding = width < 28 ? 28 - width : 1; padding > 0; --padding)
    fputc(' ', stdout);
  fprintf(stdout, "%s%s%s\n", clr(NY_CLR_GRAY), entry->summary,
          clr(NY_CLR_RESET));
}

static bool ny_options_print_registry_group(const char *topic) {
  const char *group = topic;
  if (strcmp(topic, "std") == 0 || strcmp(topic, "stdlib") == 0 ||
      strcmp(topic, "cache") == 0)
    group = "stdlib";
  else if (strcmp(topic, "parallel") == 0)
    group = "gpu";
  else if (strcmp(topic, "heap") == 0)
    group = "memory";
  else if (strcmp(topic, "execution") == 0)
    group = "run";
  else if (strcmp(topic, "optimization") == 0)
    group = "opt";
  else if (strcmp(topic, "build") == 0 || strcmp(topic, "target") == 0)
    group = "target";
  else if (strcmp(topic, "inspect") == 0)
    group = "dump";
  bool include_parallel =
      strcmp(topic, "gpu") == 0 || strcmp(topic, "parallel") == 0;
  size_t count = sizeof(ny_option_registry) / sizeof(ny_option_registry[0]);
  bool found = false;
  for (size_t i = 0; i < count; ++i) {
    const ny_option_registry_entry_t *entry = &ny_option_registry[i];
    if ((strcmp(entry->group, group) == 0 ||
         (include_parallel && strcmp(entry->group, "parallel") == 0)) &&
        strcmp(entry->visibility, "deprecated") != 0) {
      found = true;
      break;
    }
  }
  if (!found)
    return false;
  fprintf(stdout, "%sNytrix %s options%s\n", clr(NY_CLR_BOLD NY_CLR_CYAN),
          topic, clr(NY_CLR_RESET));
  for (size_t i = 0; i < count; ++i) {
    const ny_option_registry_entry_t *entry = &ny_option_registry[i];
    if ((strcmp(entry->group, group) != 0 &&
         !(include_parallel && strcmp(entry->group, "parallel") == 0)) ||
        strcmp(entry->visibility, "deprecated") == 0)
      continue;
    found = true;
    ny_options_print_registry_row(entry);
    ny_options_print_registry_short(entry);
    if (entry->allowed_values)
      fprintf(stdout, "    %sValues:%s %s%s%s\n", clr(NY_CLR_BOLD),
              clr(NY_CLR_RESET), clr(NY_CLR_YELLOW), entry->allowed_values,
              clr(NY_CLR_RESET));
  }
  return true;
}

static void ny_options_print_registry_all(void) {
  size_t count = sizeof(ny_option_registry) / sizeof(ny_option_registry[0]);
  for (size_t i = 0; i < count; ++i) {
    const char *group = ny_option_registry[i].group;
    bool seen = false;
    for (size_t previous = 0; previous < i; ++previous) {
      if (strcmp(ny_option_registry[previous].group, group) == 0) {
        seen = true;
        break;
      }
    }
    if (seen)
      continue;
    fprintf(stdout, "\n%s%s options%s\n", clr(NY_CLR_BOLD NY_CLR_MAGENTA),
            group, clr(NY_CLR_RESET));
    for (size_t j = 0; j < count; ++j) {
      const ny_option_registry_entry_t *entry = &ny_option_registry[j];
      if (strcmp(entry->group, group) != 0 ||
          strcmp(entry->visibility, "deprecated") == 0)
        continue;
      ny_options_print_registry_row(entry);
      if (entry->allowed_values)
        fprintf(stdout, "    %sValues:%s %s%s%s\n", clr(NY_CLR_BOLD),
                clr(NY_CLR_RESET), clr(NY_CLR_YELLOW), entry->allowed_values,
                clr(NY_CLR_RESET));
    }
  }
}

static FILE *ny_usage_out;
static bool ny_usage_section_enabled;
static const char *ny_usage_topic;

static bool ny_usage_topic_selects(const char *topic, const char *section) {
  if (!topic || strcmp(topic, "all") == 0)
    return true;
  if (strcmp(topic, "env") == 0 || strcmp(topic, "environment") == 0)
    return strcmp(section, "ENVIRONMENT") == 0;
  if (strcmp(topic, "gpu") == 0 || strcmp(topic, "parallel") == 0)
    return strcmp(section, "PARALLELISM") == 0;
  if (strcmp(topic, "memory") == 0 || strcmp(topic, "heap") == 0)
    return strcmp(section, "MEMORY") == 0;
  if (strcmp(topic, "run") == 0 || strcmp(topic, "execution") == 0)
    return strcmp(section, "EXECUTION") == 0;
  if (strcmp(topic, "opt") == 0 || strcmp(topic, "optimization") == 0)
    return strcmp(section, "OPTIMIZATION") == 0;
  if (strcmp(topic, "build") == 0 || strcmp(topic, "target") == 0)
    return strcmp(section, "BUILD TUNING") == 0;
  if (strcmp(topic, "debug") == 0 || strcmp(topic, "trace") == 0)
    return strcmp(section, "DEBUGGING") == 0;
  if (strcmp(topic, "profile") == 0)
    return strcmp(section, "PROFILING") == 0;
  if (strcmp(topic, "emit") == 0 || strcmp(topic, "inspect") == 0)
    return strcmp(section, "INSPECTION & DUMPS") == 0 ||
           strcmp(section, "COMPILER STAGES & ARTIFACTS") == 0;
  if (strcmp(topic, "stdlib") == 0 || strcmp(topic, "cache") == 0)
    return strcmp(section, "STDLIB & CACHE") == 0;
  if (strcmp(topic, "info") == 0)
    return strcmp(section, "INFO") == 0;
  return false;
}

static void ny_usage_section(const char *name) {
  ny_usage_section_enabled = ny_usage_topic_selects(ny_usage_topic, name);
  if (!ny_usage_section_enabled)
    return;
  ny_help_section(ny_usage_out, name);
}

static void ny_usage_items(const ny_usage_entry_t *items) {
  if (!ny_usage_section_enabled)
    return;
  for (const ny_usage_entry_t *it = items; it && it->flag; ++it)
    ny_help_item(ny_usage_out, it->color, it->flag, it->desc ? it->desc : "");
  fputc('\n', ny_usage_out);
}

static void ny_usage_example(int width, const char *cmd, const char *desc) {
  if (ny_usage_topic && strcmp(ny_usage_topic, "all") != 0)
    return;
  int terminal = ny_ui_width();
  int column = width;
  int available = terminal - 2 - column - 3;
  if (available < 18) {
    column = terminal - 2 - 3 - 18;
    if (column < 16)
      column = 16;
    available = terminal - 2 - column - 3;
  }
  if ((int)strlen(cmd) <= column) {
    fprintf(ny_usage_out, "  %s%-*s%s # ", clr(NY_CLR_CYAN), column, cmd,
            clr(NY_CLR_RESET));
    ny_help_wrap(ny_usage_out, desc, 2 + column + 3, available);
  } else {
    fprintf(ny_usage_out, "  %s%s%s\n    ", clr(NY_CLR_CYAN), cmd,
            clr(NY_CLR_RESET));
    ny_help_wrap(ny_usage_out, desc, 4, terminal - 4);
  }
}

static void ny_options_usage_impl(const char *prog, bool show_env,
                                  const char *topic) {
  FILE *previous_output = ny_usage_out;
  const char *previous_topic = ny_usage_topic;
  ny_usage_out = stdout;
  ny_usage_topic = topic;
  ny_help_heading(ny_usage_out, "Compiler");
  fprintf(ny_usage_out, "Usage: %s [options] [file.ny]\n", prog);
  fprintf(ny_usage_out, "       %s -c 'source'\n\n", prog);
  ny_usage_section("GENERAL");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_MAGENTA, "--color=WHEN", "WHEN: auto | always | never"},
      {NY_CLR_MAGENTA, "--glyphs=MODE", "MODE: unicode | ascii"},
      {NY_CLR_MAGENTA, "-safe-mode",
       "Enable strict safety/effect/alias checks"},
      {NY_CLR_MAGENTA, "--mode=MODE", "Runtime preset: safe | fast | bare"},
      {NY_CLR_MAGENTA, "--strict",
       "Default type checks plus ownership/borrow diagnostics"},
      {NY_CLR_MAGENTA, "--strict-types",
       "Reject dynamic type cliffs at compile time"},
      {NY_CLR_MAGENTA, "--no-strict-types", "Allow legacy dynamic type cliffs"},
      {NY_CLR_MAGENTA, "--list-fallbacks",
       "Dump every dynamic-any degradation (code, expr kind, line:col)"},
      {NY_CLR_MAGENTA, "--warn-shadow",
       "Warn when a declaration shadows an outer local (W2002)"},
      {NY_CLR_MAGENTA, "--warn-all",
       "Emit all lints and exit non-zero on any warning (CI mode)"},
      {NY_CLR_MAGENTA, "--max-errors=N",
       "Stop parsing after N errors (0 disables the cap)"},
      {NULL, NULL, NULL}});
  ny_usage_section("TOOLING");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_CYAN, "ny fmt|test|doc|web|perf|make",
       "Run bundled developer tools"},
      {NY_CLR_CYAN, "ny pkg|get|install",
       "Manage local, vendor, user, or system packages"},
      {NY_CLR_CYAN, "ny new <dir>", "Create a clean app template"},
      {NY_CLR_CYAN, "ny-lsp", "Separate language-server binary"},
      {NULL, NULL, NULL}});
  ny_usage_section("EFFECT & ALIAS POLICY");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_MAGENTA, "--effect-require-known",
       "Enforce known effects for user functions (default on)"},
      {NY_CLR_MAGENTA, "--no-effect-require-known", "Allow unknown effects"},
      {NY_CLR_MAGENTA, "--alias-require-known",
       "Enforce known alias/escape facts (default on)"},
      {NY_CLR_MAGENTA, "--no-alias-require-known",
       "Allow unknown alias/escape facts"},
      {NY_CLR_MAGENTA, "--alias-require-no-escape",
       "Enforce no arg-escape / return-alias"},
      {NULL, NULL, NULL}});
  ny_usage_section("MEMORY");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_MAGENTA, "--heap=MODE", "Heap policy: manual | raii | rc | gc"},
      {NY_CLR_MAGENTA, "-gc / -no-gc", "Enable/disable GC nursery"},
      {NY_CLR_MAGENTA, "--rc-gc", "Alias for --heap=rc"},
      {NY_CLR_MAGENTA, "--ownership", "Alias for --heap=raii"},
      {NY_CLR_MAGENTA, "--ownership-strict",
       "Turn ownership diagnostics into errors"},
      {NY_CLR_MAGENTA, "--borrow-check",
       "Enable advisory borrow/ownership diagnostics without RAII cleanup"},
      {NY_CLR_MAGENTA, "--no-borrow-check",
       "Disable borrow/ownership diagnostics"},
      {NY_CLR_MAGENTA, "--raii", "Alias for --ownership"},
      {NY_CLR_MAGENTA, "--zero-init/--no-zero-init",
       "Zero all managed heap allocations (default: off)"},
      {NY_CLR_MAGENTA, "Note",
       "Typed contracts are non-null by default (nil requires ?T or *T)"},
      {NULL, NULL, NULL}});
  ny_usage_section("OPTIMIZATION");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "-O1/-O2/-O3",
       "Optimization level (default: -O0 for fast development builds); an "
       "explicit -O level also sets --jit-opt-level unless given explicitly"},
      {NY_CLR_GREEN, "--profile=MODE",
       "Optimization profile: speed | balanced | compile | size | none | "
       "peak"},
      {NY_CLR_GREEN, "-passes=PIPE",
       "Custom LLVM pass pipeline (e.g., 'default<O2>')"},
      {NY_CLR_GREEN, "--release",
       "Size-optimized AOT preset (no verify, stripped); combine with "
       "-o/-run/--emit-only"},
      {NULL, NULL, NULL}});
  ny_usage_section("PARALLELISM");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "--gpu=MODE", "GPU mode: off | auto | opencl"},
      {NY_CLR_GREEN, "--gpu-backend=B",
       "GPU backend: none | auto | opencl | cuda | hip | metal"},
      {NY_CLR_GREEN, "--gpu-offload=M",
       "Offload policy: off | auto | on | force"},
      {NY_CLR_GREEN, "--gpu-min-work=N",
       "Minimum work-items before GPU offload"},
      {NY_CLR_GREEN, "--gpu-async", "Prefer async GPU dispatch"},
      {NY_CLR_GREEN, "--gpu-fast-math", "Allow relaxed GPU math optimizations"},
      {NY_CLR_GREEN, "--fast-math",
       "Allow relaxed CPU math optimizations (reassoc, no-nans, no-infs)"},
      {NY_CLR_GREEN, "--accel-target=T",
       "Device target: auto | none | nvptx | amdgpu | spirv | hsaco"},
      {NY_CLR_GREEN, "--accel-object=K",
       "Device artifact: auto | none | ptx | o | spv | hsaco"},
      {NY_CLR_GREEN, "--gpu-target=T",
       "Compatibility alias (maps to backend/accel target)"},
      {NY_CLR_GREEN, "--parallel=MODE",
       "Parallel mode: off | auto | threads | modules (default: auto)"},
      {NY_CLR_GREEN, "--threads=N", "Thread budget for parallel work"},
      {NY_CLR_GREEN, "--parallel-min-work=N",
       "Minimum work-items before threading"},
      {NULL, NULL, NULL}});
  ny_usage_section("BUILD TUNING");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "--host-triple=T", "Host target triple for native build"},
      {NY_CLR_GREEN, "--wasm",
       "Compile to WebAssembly (shorthand for --native-backend=wasm "
       "--host-triple=wasm32-unknown-unknown)"},
      {NY_CLR_GREEN, "--cross=<triple>",
       "Cross-compile with auto-detected toolchain (--cross-run to execute "
       "with QEMU)"},
      {NY_CLR_GREEN, "--emit-wasm[=PATH]",
       "Emit .wasm via clang (requires --wasm or --native-backend=wasm)"},
      {NY_CLR_GREEN, "--native-backend=B",
       "Native backend: llvm | x86_64 | x86 | aarch64 | arm | riscv | wasm | "
       "bpf | mips | powerpc | avr | amdgpu"},
      {NY_CLR_GREEN, "--native-tier=T",
       "Native tier: auto | baseline | stencil | fast | opt | llvm (default: "
       "auto, currently baseline)"},
      {NY_CLR_GREEN, "--native-tier-budget=N",
       "Override native tier compile budget"},
      {NY_CLR_GREEN, "--native-hot-threshold=N",
       "Override native tier hot-function threshold"},
      {NY_CLR_GREEN, "--native-cold-threshold=N",
       "Override native tier cold-function threshold"},
      {NY_CLR_GREEN, "--native-cache-score=N",
       "Override native tier cache score"},
      {NY_CLR_GREEN, "--native-prefer-vm / --native-prefer-asm",
       "Select the preferred native debug tier"},
      {NY_CLR_GREEN, "--native-abi=A",
       "Native ABI: auto | sysv | win64 | aapcs"},
      {NY_CLR_GREEN, "--c-frontend=C", "C frontend: auto | libclang | nytrix"},
      {NY_CLR_GREEN, "--arm-float-abi=A", "ARM ABI: soft | softfp | hard"},
      {NY_CLR_GREEN, "--host-cflags=F", "Extra host compiler flags"},
      {NY_CLR_GREEN, "--host-ldflags=F", "Extra host linker flags"},
      {NY_CLR_GREEN, "--gprof/--no-gprof",
       "Enable/disable -pg in native build path"},
      {NY_CLR_GREEN, "--std-builtin-ops/--no-std-builtin-ops",
       "Enable/disable builtin std operator lowering"},
      {NY_CLR_GREEN, "--compiler-asserts/--no-compiler-asserts",
       "Enable/disable internal compiler invariant checks"},
      {NY_CLR_GREEN, "--debug-locals/--no-debug-locals",
       "Enable/disable local/global debug records (default: off)"},
      {NY_CLR_GREEN, "--dwarf-version=N", "DWARF version (2..5)"},
      {NY_CLR_GREEN, "--dwarf-split-inlining/--no-dwarf-split-inlining",
       "Enable/disable split-inline callsite info"},
      {NY_CLR_GREEN, "--dwarf-profile-info/--no-dwarf-profile-info",
       "Enable/disable profiling-friendly location info"},
      {NULL, NULL, NULL}});
  ny_usage_section("EXECUTION");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "-run", "Build a native executable and run it (AOT auto)"},
      {NY_CLR_GREEN, "--run=MODE", "MODE: auto | aot | jit | native"},
      {NY_CLR_GREEN, "--native / --native-only",
       "Run through NYR/native object code without LLVM code generation"},
      {NY_CLR_GREEN, "--jit", "Run through LLVM JIT instead of native AOT"},
      {NY_CLR_GREEN, "--jit-engine=ENGINE",
       "LLVM JIT engine: orc | mcjit (default: mcjit)"},
      {NY_CLR_GREEN, "--jit-fast-isel/--no-jit-fast-isel",
       "Enable/disable JIT fast instruction selection"},
      {NY_CLR_GREEN, "--jit-opt-level=N",
       "JIT optimization level (0..3, default: 0, or the explicit -O level "
       "when -O is given without --jit-opt-level)"},
      {NY_CLR_GREEN, "--jit-code-model=M",
       "LLVM JIT code model: small | kernel | medium | large"},
      {NY_CLR_GREEN, "--jit-perf-map",
       "Emit /tmp/perf-<pid>.map for Linux perf profiling"},
      {NY_CLR_GREEN, "--safe-run[=LIMITS]",
       "Run with CPU/memory/file/process limits (off by default)"},
      {NY_CLR_GREEN, "--sanitize=KIND",
       "Compile with sanitizer and run under safe-exec supervisor\n"
       "                                        "
       "    KIND: address | undefined | thread | leak"},
      {NY_CLR_GREEN, "-emit-only", "Compile only; do not execute"},
#ifdef _WIN32
      {NY_CLR_GREEN, "-o [path]",
       "Emit native binary at [path] (default: a.exe; implies -emit-only)"},
#else
      {NY_CLR_GREEN, "-o [path]",
       "Emit native binary at [path] (default: a.out; implies -emit-only)"},
#endif
      {NY_CLR_GREEN, "--output=<path>", "Same as -o"},
      {NY_CLR_GREEN, "-strip, -no-strip",
       "Create stripped/unstripped binary (default: no-strip)"},
      {NY_CLR_GREEN, "-c/-e <code>, --eval <code>",
       "Execute inline code (`;` starts a comment; use shell newlines)"},
      {NY_CLR_GREEN, "-ic/-ci <code>, --eval-repl <code>",
       "Execute inline code, then enter the REPL"},
      {NY_CLR_GREEN, "--hot-reload / --watch",
       "Recompile and rerun on detected source changes"},
      {NY_CLR_GREEN, "--watch-poll=N",
       "File-watch polling interval in ms (default: 250)"},
      {NY_CLR_GREEN, "--comptime-limit=N",
       "Limit compile-time evaluator instructions (default: 1000000)"},
      {NULL, NULL, NULL}});
  ny_usage_section("STDLIB & CACHE");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "-std [none|minimal|full]",
       "Stdlib inclusion mode (default: minimal)"},
      {NY_CLR_GREEN, "-timeout <seconds>", "Set a hard execution limit"},
      {NY_CLR_GREEN, "-no-std", "Disable stdlib loading"},
      {NY_CLR_GREEN, "--core-path=<path>", "Use custom core.ny"},
      {NY_CLR_GREEN, "--std-path=<path>",
       "Compatibility alias for --core-path"},
      {NY_CLR_GREEN, "--full-mod", "Alias for -std full"},
      {NY_CLR_GREEN, "--bundle-core=P", "Generate core.ny at P"},
      {NY_CLR_GREEN, "--bundle-symbols=P", "Generate core_symbols.h at P"},
      {NY_CLR_GREEN, "--core-bc=<path>",
       "Reuse the compiled core library cache"},
      {NY_CLR_GREEN, "--clean-cache",
       "Remove Nytrix JIT/std/AOT cache artifacts"},
      {NULL, NULL, NULL}});
  ny_usage_section("LINKING");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "-flto / --llvm-lto",
       "Optimize linked LLVM module bitcode as a whole program"},
      {NY_CLR_GREEN, "-L<dir>", "Add a linker search path (repeatable)"},
      {NY_CLR_GREEN, "-l<lib>", "Link against library (also accepts -l <lib>)"},
      {NULL, NULL, NULL}});
  ny_usage_section("REPL");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "-i, --interactive", "Interactive REPL with line editing"},
      {NY_CLR_GREEN, "--repl", "Read source from stdin (one-shot)"},
      {NY_CLR_GREEN, "--plain-repl",
       "Disable line editing and ANSI escapes in the REPL"},
      {NULL, NULL, NULL}});
  ny_usage_section("INSPECTION & DUMPS");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_BLUE, "Source: --dump-ast[=OPTIONS], --dump-tokens",
       "Inspect source; AST options: typed, filter=NAME (last)"},
      {NY_CLR_BLUE, "--expand / --expand-only=NAME / --expand-json",
       "Print and filter the AST expansion report"},
      {NY_CLR_BLUE, "--inspect[=program|runtime]",
       "Preset: optimized NYR plus pass summaries (runtime includes "
       "libraries)"},
      {NY_CLR_BLUE, "--nyr-dump[=raw,cfg,stats,passes]",
       "Inspect optimized NYR; add raw snapshots, CFG, stats, or pass detail"},
      {NY_CLR_BLUE, "--vm-disasm[=NAME]",
       "List optimized NYR VM instructions by function (optional name filter)"},
      {NY_CLR_BLUE, "--mixed-disasm[=NAME]",
       "Mixed NYIR/MIR/native disassembly by function (optional name filter)"},
      {NY_CLR_BLUE, "--nyr-dump-to=PATH",
       "Write NYR text to PATH (default: stderr)"},
      {NY_CLR_BLUE, "Backend: --dump-llvm, --emit-ir=PATH, --emit-bc=PATH",
       "Print LLVM IR or write LLVM IR / backend bytecode"},
      {NY_CLR_BLUE, "--emit-asm=PATH / --dump-llvm-passes",
       "Write assembly or inspect LLVM pass output"},
      {NY_CLR_BLUE, "Artifacts: --dump-nyr[=PATH], --nyr-metadata[=PATH]",
       "Write reusable NYR bytecode or its metadata report"},
      {NY_CLR_BLUE, "--native-precompile=PATH",
       "Write an LLVM-free NYR bundle for later VM use or native emission"},
      {NY_CLR_BLUE, "--native-from-nyr=PATH",
       "Build a native executable from a saved NYR bundle"},
      {NY_CLR_BLUE, "--dump-funcs",
       "List compiled functions and their generated-code sizes"},
      {NY_CLR_BLUE, "--dump-escapes / --dump-proofs / --dump-obj-full",
       "Inspect compiler analysis and object files"},
      {NY_CLR_BLUE, "--dump-on-error / --dump-diagnose",
       "Save diagnostics or stage artifacts after failure"},
      {NY_CLR_BLUE, "--dump-dir=PATH / --dump-scope=S",
       "Choose diagnostic output directory and source/library scope"},
      {NULL, NULL, NULL}});
  ny_usage_section("DEBUGGING");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_BLUE, "-v/-vv/-vvv", "Show progressively more compiler detail"},
      {NY_CLR_BLUE, "-vvvv/-vvvvv/-vvvvvv",
       "Deep trace; -vvvvv omits imported libraries, -vvvvvv includes them"},
      {NY_CLR_BLUE, "--debug",
       "Max verbosity, keep symbols, keep binary unstripped"},
      {NY_CLR_BLUE, "--debug-everything",
       "Enable full-stack tracing with a 30s safety timeout (override with "
       "--timeout)"},
      {NY_CLR_BLUE, "-time", "Show timing for each phase"},
      {NY_CLR_BLUE, "-trace", "Enable execution tracing"},
      {NY_CLR_BLUE, "--progress",
       "Show live stderr compiler progress for long builds (default-on for "
       "TTY)"},
      {NY_CLR_BLUE, "--no-progress", "Disable live stderr compiler progress"},
      {NULL, NULL, NULL}});
  ny_usage_section("COMPILER STAGES & ARTIFACTS");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_BLUE, "--ny-desugar=ny|c",
       "Run the compound-assignment desugar pass via the Ny library (ny) or "
       "the in-parser C pass (c); off by default"},
      {NY_CLR_BLUE, "--explain-specialization=NAME",
       "Explain static specialization candidates"},
      {NY_CLR_BLUE, "--stop-after=STAGE",
       "Stop after parse|hm|trait|flow|abi|opt"},
      {NY_CLR_BLUE, "--type-solver=MODE", "Type solver: auto | hm | z3"},
      {NY_CLR_BLUE, "--proof-solver=MODE",
       "Proof solver: none | presburger | z3 | auto"},
      {NY_CLR_BLUE, "--emit-artifact=PATH",
       "Write the selected stage artifact JSON"},
      {NY_CLR_BLUE, "--verify-artifact=PATH",
       "Validate a stage artifact against expanded input and stop"},
      {NY_CLR_BLUE, "--use-artifact=PATH",
       "Reuse a validated ABI/opt artifact to skip semantic validation"},
      {NY_CLR_BLUE, "--emit-refinement-interface=PATH",
       "Write a versioned refinement interface after type validation"},
      {NY_CLR_BLUE, "--use-refinement-interface=PATH",
       "Load a versioned refinement interface before type validation"},
      {NY_CLR_BLUE, "--emit-effect-interface=PATH",
       "Write serialized effect rows for exported functions"},
      {NY_CLR_BLUE, "--use-effect-interface=PATH",
       "Load serialized effect rows for imported functions"},
      {NY_CLR_BLUE, "--collect-errors",
       "Emit stage-tagged errors.v1.json on failures"},
      {NY_CLR_BLUE, "--emit-shapes",
       "Include type/function/layout/operator shapes in artifacts"},
      {NY_CLR_BLUE, "--meta-trace",
       "Include expression/call trace in expansion report"},
      {NY_CLR_BLUE, "--extract-code",
       "Extract fenced Nytrix code blocks from Markdown and exit"},
      {NY_CLR_BLUE, "--extract-json",
       "Emit extracted code block metadata as JSON"},
      {NY_CLR_BLUE, "--extract-at=L[:C]",
       "Select the fenced code block containing a source line"},
      {NY_CLR_BLUE, "--extract-lang=LIST",
       "Fence languages for --extract-code (default ny,nytrix)"},
      {NULL, NULL, NULL}});
  ny_usage_section("DIAGNOSTICS");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_BLUE, "--warn=LEVEL", "Warning level: none | useful | all"},
      {NY_CLR_BLUE, "--diag-compact", "Compact one-line diagnostics"},
      {NY_CLR_BLUE, "--diag-rich", "Rich diagnostics with snippets (default)"},
      {NY_CLR_BLUE, "-verify", "Verify LLVM module"},
      {NY_CLR_BLUE, "-g", "Emit debug symbols"},
      {NY_CLR_BLUE, "--entry-name=<n>", "Rename entry point (default: main)"},
      {NY_CLR_BLUE, "--ir-full", "Keep std/lib definitions in IR output"},
      {NY_CLR_BLUE, "--ir-no-std",
       "Omit std/lib definitions in IR output (default)"},
      {NULL, NULL, NULL}});
  ny_usage_section("NYR VALIDATION");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_BLUE, "--native-result-oracle[=N]",
       "Compare NYR VM and x86-64 native raw scalar result"},
      {NY_CLR_BLUE, "--native-oracle-per-pass",
       "Run the VM/native result oracle after each optimization pass"},
      {NY_CLR_BLUE, "--native-oracle-fuzz=N",
       "Generate N deterministic scalar NYR programs and oracle-check each"},
      {NY_CLR_BLUE, "--native-tv-seed[=N]",
       "After each NYR pass, validate pure i64 NYR (default N=16; N=0 "
       "disables)"},
      {NY_CLR_BLUE, "--native-enable-cf-mem2reg",
       "Compatibility spelling for the default control-flow NYR mem2reg pass"},
      {NY_CLR_BLUE, "--nyr-fingerprint",
       "Show stable NYR fingerprints and flag large pass growth"},
      {NY_CLR_BLUE, "--nyr-flaky-check",
       "Run each optimizer pass twice on cloned input and compare output"},
      {NY_CLR_BLUE, "--nyr-track-value=N",
       "Track zero-based SSA value N through passes (enables value logs)"},
      {NY_CLR_BLUE, "--dbg-filter=FILTER",
       "Filter verbose diagnostics: pass:NAME;func:NAME;value:N;cat:LIST"},
      {NY_CLR_BLUE, "--delta-only",
       "Show only passes that changed NYR and their instruction deltas"},
      {NY_CLR_BLUE, "--unify-trace",
       "Show HM constraints and type-variable substitutions"},
      {NY_CLR_BLUE, "--trait-resolve",
       "Show attached method and trait implementation resolution"},
      {NY_CLR_BLUE, "--dbg-format=FORMAT",
       "Format verbose diagnostics as text or JSON Lines"},
      {NY_CLR_BLUE, "--nyr-verify", "Verify NYR after every optimization pass"},
      {NY_CLR_BLUE, "--nyr-bisect",
       "Verify every pass and stop at the first pass that breaks NYR"},
      {NY_CLR_BLUE, "--nyr-rollback-on-verify-fail",
       "Restore pre-pass IR on verifier failure (requires --nyr-verify)"},
      {NY_CLR_BLUE, "--nyr-global-inst-cap=N",
       "Abort pipeline when total IR instructions exceed N"},
      {NY_CLR_BLUE, "--nyr-pass-growth-factor=X",
       "Abort pass when instruction count grows by more than X (e.g. 2.0)"},
      {NY_CLR_BLUE, "--nyr-disable-pass=NAME",
       "Skip one named NYR optimizer pass for diagnostics"},
      {NY_CLR_BLUE, "--nyr-stop-after=NAME",
       "Stop the NYR optimizer after one named diagnostic pass"},
      {NY_CLR_BLUE, "--nyr-run[=PATH]",
       "Execute optimized rt_main NYR in the debug VM and print the result"},
      {NY_CLR_BLUE, "--vm-disasm[=NAME]",
       "List the optimized VM instruction stream for every compiled function"},
      {NY_CLR_BLUE, "--mixed-disasm[=NAME]",
       "Mixed NYIR/MIR/native disassembly for every compiled function"},
      {NY_CLR_BLUE, "--nyr-run-bin=PATH",
       "Execute a validated binary NYR dump directly through the debug VM"},
      {NY_CLR_BLUE, "--nyr-run-profile[=PATH]",
       "Write NYR VM step/op/branch profile data for tiering diagnostics"},
      {NY_CLR_BLUE, "--nyr-run-max-steps=N",
       "Limit NYR VM execution steps before failing"},
      {NY_CLR_BLUE, "--nyr-run-recursion-limit=N",
       "Limit in-program NYR VM recursive call depth"},
      {NULL, NULL, NULL}});
  ny_usage_section("NATIVE INSPECTION");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_BLUE, "--regalloc-heat",
       "Show per-block live pressure, spill, and reload counts"},
      {NY_CLR_BLUE, "--elf-dump",
       "Show ELF sections, relocations, and symbols while writing ELF objects"},
      {NY_CLR_BLUE, "--stack-frame",
       "Show stack frame size and local/value slot regions"},
      {NY_CLR_BLUE, "--abi",
       "Show source-level parameter and return scalar ABI classes"},
      {NULL, NULL, NULL}});
  ny_usage_section("PROFILING");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_BLUE, "-prof, --prof",
       "Enable compiler/runtime profiling outputs (timings + stats)"},
      {NY_CLR_BLUE, "--perf=compact|smart|verbose",
       "Profile function wall/CPU time, calls, allocations, and sampled RSS"},
      {NY_CLR_BLUE, "--native-profile-use=PATH",
       "Load NyP2 block/edge heat for layout, loop, and inline PGO"},
      {NY_CLR_BLUE, "--native-tier-report[=PATH]",
       "Write native tier/static-island cost facts and backend recommendation"},
      {NY_CLR_BLUE, "--emit-report=PATH",
       "Write compiler, optimizer, runtime, GC, and timing telemetry"},
      {NY_CLR_BLUE, "--callgraph[=DEPTH|json]",
       "Report reachable direct/indirect call edges (text or JSON)"},
      {NULL, NULL, NULL}});
  ny_usage_section("INFO");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_BLUE, "-h, -help, --help", "Show this help message"},
      {NY_CLR_BLUE, "--help env", "Show supported environment variables"},
      {NY_CLR_BLUE, "-version", "Show version info"},
      {NY_CLR_BLUE, "--doctor", "Run system toolchain diagnostics"},
      {NY_CLR_BLUE, "--env", "Print effective paths, toolchain, and overrides"},
      {NY_CLR_BLUE, "--targets", "List cross-compilation target presets"},
      {NY_CLR_BLUE, "--profile-time",
       "Profile compilation: run N times and show timing (use --runs=N)"},
      {NY_CLR_BLUE, "--report-peak-rss",
       "Report this compiler process's peak resident memory after compilation"},
      {NY_CLR_BLUE, "--wasm-info=<path>",
       "Show imports/exports of a .wasm binary"},
      {NULL, NULL, NULL}});
  if (show_env || (topic && strcmp(topic, "env") == 0)) {
    ny_usage_section("ENVIRONMENT");
    fprintf(ny_usage_out, "  Policy env overrides are disabled by default.\n");
    fprintf(ny_usage_out,
            "  Use CLI flags for GPU/parallel/effect/debug behavior.\n");
    ny_usage_items((const ny_usage_entry_t[]){
        {NY_CLR_YELLOW, "NYTRIX_CORE_PATH", "Override core.ny path"},
        {NY_CLR_YELLOW, "NYTRIX_STD_PATH", "Compatibility alias for core path"},
        {NY_CLR_YELLOW, "NYTRIX_STD_PREBUILT", "Use prebuilt stdlib source"},
        {NY_CLR_YELLOW, "NYTRIX_BUILD_STD_PATH",
         "Override generated core.ny path"},
        {NY_CLR_YELLOW, "NYTRIX_HOST_TRIPLE / NYTRIX_ARM_FLOAT_ABI",
         "Host target and ARM ABI tuning"},
        {NY_CLR_YELLOW, "NYTRIX_HOST_CFLAGS / NYTRIX_HOST_LDFLAGS",
         "Host build tuning"},
        {NY_CLR_YELLOW, "NYTRIX_JIT_CACHE_FORMAT",
         "JIT cache format: bc (default) | ir"},
        {NY_CLR_YELLOW, "NYTRIX_JIT_NATIVE_CACHE",
         "Promote validated JIT bitcode entries to reusable native shared "
         "objects"},
        {NY_CLR_YELLOW, "NYTRIX_AOT_IR_CACHE",
         "Allow AOT builds to reuse cached IR"},
        {NY_CLR_YELLOW, "NYTRIX_STD_BC_CACHE_VERIFY",
         "Pre-verify std bitcode cache hits before linking"},
        {NY_CLR_YELLOW, "NYTRIX_CACHE_STRICT_FILE_ID",
         "Include ctime/inode/device in cache keys"},
        {NY_CLR_YELLOW, "NYTRIX_GC",
         "Runtime GC switch set by -gc or --heap=gc"},
        {NY_CLR_YELLOW, "NYTRIX_GC_NURSERY_SIZE / NYTRIX_GC_TENURED_SIZE",
         "Enabled-GC heap sizes; accepts bytes, K, M, or G"},
        {NY_CLR_YELLOW, "NYTRIX_GC_LOS_THRESHOLD",
         "Large-object cutoff for enabled GC"},
        {NY_CLR_YELLOW, "NYTRIX_GC_VALIDATE",
         "Validate GC spaces before and after collection"},
        {NY_CLR_YELLOW, "NYTRIX_MONO_TYPES",
         "Demand-driven numeric monomorphization"},
        {NY_CLR_YELLOW, "NYTRIX_MONO_MAX_PER_FN / NYTRIX_MONO_MAX_GLOBAL",
         "Cap generated clones"},
        {NY_CLR_YELLOW, "NYTRIX_MONO_TRACE",
         "Trace generated monomorphized functions"},
        {NY_CLR_YELLOW, "NYTRIX_LAZY_STDLIB_CODEGEN",
         "Demand-emit imported stdlib bodies for user programs"},
        {NY_CLR_YELLOW, "NYTRIX_HM_DEBUG",
         "Trace HM diagnostics and fallback decisions"},
        {NY_CLR_YELLOW, "NYTRIX_PROOF_DEBUG / NY_PROOF_DEBUG",
         "Trace the proof system: 1 = lemma resolution + prove/assert path "
         "decisions, 2 = ranges/digests/solver, 3 = canon-decide recursion"},
        {NY_CLR_YELLOW, "NYTRIX_DYNAMIC_INT_BINOPS",
         "Guard unproven std integer operators with inline tag fast paths"},
        {NY_CLR_YELLOW, "NYTRIX_RUNTIME_OPT",
         "Runtime C optimization: size | 2 | 3 | speed"},
        {NY_CLR_YELLOW, "NYTRIX_RUNTIME_NATIVE",
         "Use -march=native for speed-profile runtime objects"},
        {NY_CLR_YELLOW, "NYTRIX_MAKE_EXEC_TOOL",
         "Let ./make ny replace itself with ny"},
        {NY_CLR_YELLOW, "NYTRIX_LINK_UI_DEFAULTS",
         "Link Linux X11/Wayland defaults for UI programs"},
        {NY_CLR_YELLOW, "NYTRIX_CC",
         "Override the C compiler (set by --cross=<triple>)"},
        {NY_CLR_YELLOW, "NYTRIX_CROSS_RUNNER",
         "QEMU or Wine runner for cross-compiled binaries (set by "
         "--cross-run)"},
        {NULL, NULL, NULL}});
  }
  ny_usage_section("EXAMPLES");
  char cmd[512];
  const int example_width = 67;
  snprintf(cmd, sizeof(cmd),
           "%s-test --bin %s --pattern runtime/language/control", prog, prog);
  ny_usage_example(example_width, cmd, "run an embedded .nshape test fixture");
  snprintf(cmd, sizeof(cmd), "%s", prog);
  ny_usage_example(example_width, cmd, "interactive REPL");
  snprintf(cmd, sizeof(cmd), "%s -c 'print(\"hello\")'", prog);
  ny_usage_example(example_width, cmd, "run inline");
  snprintf(cmd, sizeof(cmd), "%s -ic 'a=1337'", prog);
  ny_usage_example(example_width, cmd, "run inline, then REPL");
  fflush(ny_usage_out);
  ny_usage_out = previous_output;
  ny_usage_topic = previous_topic;
}

void ny_options_usage(const char *prog) {
  (void)prog;
  exit(2);
}

static const char *ny_cli_option_suggestion(const char *option);

void ny_options_print_help(const char *prog, const char *topic) {
  if (!topic) {
    ny_options_print_short_help(prog);
    return;
  }
  if (topic && strcmp(topic, "json") == 0) {
    ny_options_print_registry_json();
    return;
  }
  if (topic && strncmp(topic, "completions:", 12) == 0) {
    const char *shell = topic + 12;
    if (strcmp(shell, "bash") != 0 && strcmp(shell, "zsh") != 0 &&
        strcmp(shell, "fish") != 0) {
      fprintf(stderr,
              "ny: invalid completion shell '%s' (expected bash|zsh|fish)\n",
              shell);
      exit(2);
    }
    ny_options_print_completions(shell);
    return;
  }
  if (topic && strncmp(topic, "--", 2) == 0) {
    if (ny_options_print_registry_entry(topic))
      return;
    const char *suggestion = ny_cli_option_suggestion(topic);
    if (suggestion)
      fprintf(stderr, "ny: unknown option '%s' (did you mean '%s'?)\n", topic,
              suggestion);
    else
      fprintf(stderr, "ny: unknown option '%s'\n", topic);
    exit(2);
  }
  if (topic && strcmp(topic, "all") == 0) {
    ny_options_print_registry_all();
    return;
  }
  if (topic && strcmp(topic, "config") == 0) {
    fprintf(stdout,
            "%sConfiguration options%s\n"
            "  Configure paths and output policy with these flags.\n",
            clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET));
    ny_options_print_registry_entry("--color");
    ny_options_print_registry_entry("--glyphs");
    ny_options_print_registry_entry("--target");
    ny_options_print_registry_entry("--core-path");
    ny_options_print_registry_entry("--std");
    ny_options_print_registry_entry("--bundle-std");
    ny_options_print_registry_entry("--bundle-symbols");
    ny_options_print_registry_entry("--output");
    fputs("Environment path overrides: NYTRIX_CORE_PATH, NYTRIX_STD_PATH. "
          "Use 'ny --help env' for environment details.\n",
          stdout);
    return;
  }
  if (topic && ny_options_print_registry_group(topic))
    return;
  if (topic && strcmp(topic, "args") == 0) {
    fputs(
        "Program arguments\n"
        "  The first positional operand is the script or inline source.\n"
        "  A single -- after it is consumed; later -- tokens are program "
        "data.\n"
        "  Use core.os.args for argv, positionals, and option-value helpers.\n",
        stdout);
    return;
  }
  if (topic && strcmp(topic, "short") == 0) {
    fputs("Short options\n"
          "  Globals: -h -i -oPATH -cCODE -g -v -q -s -n -jN -tTRIPLE\n"
          "  Attached values: -OLEVEL[mods] -Ldir -llib\n"
          "  Legacy single-dash words are matched before short clusters.\n",
          stdout);
    for (size_t family_index = 0;
         family_index <
         sizeof(ny_option_registry) / sizeof(ny_option_registry[0]);
         ++family_index) {
      const ny_option_registry_entry_t *entry =
          &ny_option_registry[family_index];
      char family = '\0';
      const char *name = NULL;
      const char *items = NULL;
      if (!ny_registry_short_family(entry, &family, &name, &items))
        continue;
      const char *name_end = strchr(name, ':');
      int name_length = name_end ? (int)(name_end - name) : 0;
      fprintf(stdout, "  -%c %-9.*s", family, name_length, name);
      const char *cursor = items;
      char letter = '\0';
      char value[256];
      bool first_item = true;
      while (ny_registry_short_item_next(&cursor, &letter, value,
                                         sizeof(value)) > 0) {
        fprintf(stdout, "%s%c=%s", first_item ? "  " : " ", letter, value);
        first_item = false;
      }
      fputc('\n', stdout);
    }
    return;
  }
  if (topic && strcmp(topic, "completions") == 0) {
    fprintf(stdout,
            "%sShell completions%s\n"
            "  %s--completions SHELL%s\n"
            "  SHELL: bash | zsh | fish\n"
            "  Generate the selected shell's completion definitions.\n",
            clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET), clr(NY_CLR_CYAN),
            clr(NY_CLR_RESET));
    return;
  }
  if (topic && strcmp(topic, "presets") == 0) {
    fprintf(stdout,
            "%sCompiler presets%s\n"
            "  Prefix a command with one preset; later command-line flags "
            "override it.\n\n",
            clr(NY_CLR_BOLD NY_CLR_CYAN), clr(NY_CLR_RESET));
    fputs("  +debug    --debug --warn=useful\n"
          "  +release  --release\n"
          "  +ci       --strict --warn=error\n"
          "  +inspect  --inspect\n"
          "  +perf     --perf=compact\n\n",
          stdout);
    ny_options_print_registry_entry("--presets");
    ny_options_print_registry_entry("--release");
    ny_options_print_registry_entry("--debug");
    ny_options_print_registry_entry("--debug-everything");
    ny_options_print_registry_entry("--inspect");
    ny_options_print_registry_entry("--fast");
    return;
  }
  const char *topics[] = {"all",       "env",     "environment", "gpu",
                          "parallel",  "memory",  "heap",        "run",
                          "execution", "check",   "opt",         "optimization",
                          "build",     "target",  "debug",       "trace",
                          "profile",   "emit",    "dump",        "inspect",
                          "nyr",       "native",  "std",         "args",
                          "short",     "presets", "config",      "stdlib",
                          "cache",     "info",    "json",        "completions"};
  if (topic) {
    bool valid = false;
    for (size_t i = 0; i < sizeof(topics) / sizeof(topics[0]); ++i)
      valid |= strcmp(topic, topics[i]) == 0;
    if (!valid) {
      fprintf(stderr, "ny: unknown help topic '%s'\n", topic);
      exit(2);
    }
  }
  ny_options_usage_impl(prog, false, topic);
}

void ny_options_usage_env(const char *prog) {
  ny_options_print_help(prog, "env");
}

static const char *ny_default_std_path(void) {
  const char *env_core = getenv("NYTRIX_CORE_PATH");
  if (env_core && *env_core && ny_access(env_core, R_OK) == 0)
    return env_core;
  const char *env_std = getenv("NYTRIX_STD_PATH");
  if (env_std && *env_std && ny_access(env_std, R_OK) == 0)
    return env_std;
  char *exe_dir = ny_get_executable_dir();
  if (exe_dir && *exe_dir) {
    static char exe_stdp[4096];
    ny_join_path(exe_stdp, sizeof(exe_stdp), exe_dir, "core.ny");
    if (ny_access(exe_stdp, R_OK) == 0)
      return exe_stdp;
#ifdef NYTRIX_INSTALL_DATADIR
    snprintf(exe_stdp, sizeof(exe_stdp), "%s/../%s/nytrix/core.ny", exe_dir,
             NYTRIX_INSTALL_DATADIR);
    if (ny_access(exe_stdp, R_OK) == 0)
      return exe_stdp;
#endif
  }
  if (ny_access("./build/core.ny", R_OK) == 0)
    return "./build/core.ny";
#ifdef _WIN32
  const char *pd = getenv("PROGRAMDATA");
  if (pd && *pd) {
    static char stdp[4096];
    char tmp[4096];
    ny_join_path(tmp, sizeof(tmp), pd, "nytrix");
    ny_join_path(stdp, sizeof(stdp), tmp, "core.ny");
    if (ny_access(stdp, R_OK) == 0)
      return stdp;
  }
  return "C:/ProgramData/nytrix/core.ny";
#else
#ifdef NYTRIX_CORE_PATH
  if (ny_access(NYTRIX_CORE_PATH, R_OK) == 0)
    return NYTRIX_CORE_PATH;
#endif
  static const char *paths[] = {
      "/usr/share/nytrix/core.ny",
      "/usr/local/share/nytrix/core.ny",
      "/opt/homebrew/share/nytrix/core.ny",
      "/opt/nytrix/share/core.ny",
  };
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    if (ny_access(paths[i], R_OK) == 0)
      return paths[i];
  }
  return paths[0];
#endif
}

static bool ny_cli_takes_separate_value(const char *arg) {
  for (size_t i = 0;
       i < sizeof(ny_option_registry) / sizeof(ny_option_registry[0]); ++i) {
    const ny_option_registry_entry_t *entry = &ny_option_registry[i];
    if (!entry->takes_value)
      continue;
    if (strcmp(arg, entry->name) == 0)
      return true;
    for (size_t alias = 0;
         alias < sizeof(entry->aliases) / sizeof(entry->aliases[0]); ++alias)
      if (entry->aliases[alias] && strcmp(arg, entry->aliases[alias]) == 0)
        return true;
  }
  return false;
}

static bool ny_cli_has_flag(int argc, char **argv, const char *flag) {
  for (int i = 1; i < argc;) {
    const char *arg = argv[i] ? argv[i] : "";
    if (strcmp(arg, "--") == 0)
      break;
    if (strcmp(arg, "-c") == 0) {
      i += i + 1 < argc ? 2 : 1;
      continue;
    }
    if (arg[0] != '-' && arg[0] != '\0')
      break;
    if (strcmp(arg, flag) == 0)
      return true;
    i += ny_cli_takes_separate_value(arg) && i + 1 < argc ? 2 : 1;
  }
  return false;
}

static bool ny_cli_has_command_string(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i] ? argv[i] : "";
    if (strcmp(arg, "--") == 0)
      break;
    if (strcmp(arg, "-c") == 0)
      return true;
  }
  return false;
}

static bool ny_cli_option_is(int argc, char **argv, const char *option,
                             const char *expected) {
  size_t option_len = strlen(option);
  for (int i = 1; i < argc;) {
    const char *arg = argv[i] ? argv[i] : "";
    if (strcmp(arg, "--") == 0)
      break;
    if (strcmp(arg, "-c") == 0) {
      i += i + 1 < argc ? 2 : 1;
      continue;
    }
    if (arg[0] != '-' && arg[0] != '\0')
      break;
    if (strcmp(arg, option) == 0) {
      if (i + 1 < argc && strcmp(argv[i + 1], expected) == 0)
        return true;
    } else if (strncmp(arg, option, option_len) == 0 &&
               arg[option_len] == '=' &&
               strcmp(arg + option_len + 1, expected) == 0) {
      return true;
    }
    i += ny_cli_takes_separate_value(arg) && i + 1 < argc ? 2 : 1;
  }
  return false;
}

bool ny_options_cli_list_has_item(int argc, char **argv, const char *option,
                                  const char *wanted) {
  size_t option_len = strlen(option);
  for (int i = 1; i < argc;) {
    const char *arg = argv[i] ? argv[i] : "";
    if (strcmp(arg, "--") == 0)
      break;
    if (strcmp(arg, "-c") == 0) {
      i += i + 1 < argc ? 2 : 1;
      continue;
    }
    const char *list = NULL;
    if (strncmp(arg, option, option_len) == 0 && arg[option_len] == '=') {
      list = arg + option_len + 1;
      ++i;
    } else if (strcmp(arg, option) == 0 && i + 1 < argc) {
      list = argv[i + 1];
      i += 2;
    } else {
      i += ny_cli_takes_separate_value(arg) && i + 1 < argc ? 2 : 1;
      continue;
    }
    while (list && *list) {
      const char *end = strchr(list, ',');
      size_t length = end ? (size_t)(end - list) : strlen(list);
      if (strlen(wanted) == length && memcmp(list, wanted, length) == 0)
        return true;
      if (!end)
        break;
      list = end + 1;
    }
  }
  return false;
}

static size_t ny_cli_edit_distance(const char *left, const char *right) {
  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  if (right_len > 127)
    return 128;
  size_t previous[128];
  size_t current[128];
  for (size_t j = 0; j <= right_len; ++j)
    previous[j] = j;
  for (size_t i = 1; i <= left_len; ++i) {
    current[0] = i;
    for (size_t j = 1; j <= right_len; ++j) {
      size_t deletion = previous[j] + 1;
      size_t insertion = current[j - 1] + 1;
      size_t substitution = previous[j - 1] + (left[i - 1] != right[j - 1]);
      size_t best = deletion < insertion ? deletion : insertion;
      current[j] = substitution < best ? substitution : best;
    }
    memcpy(previous, current, (right_len + 1) * sizeof(*previous));
    if (i > right_len + 3)
      return 128;
  }
  return previous[right_len];
}

static const char *ny_cli_option_suggestion(const char *option) {
  size_t option_len = strlen(option);
  size_t max_distance = option_len > 12 ? 3 : 2;
  size_t best_distance = 129;
  const char *best = NULL;
  for (size_t i = 0;
       i < sizeof(ny_option_registry) / sizeof(ny_option_registry[0]); ++i) {
    const ny_option_registry_entry_t *entry = &ny_option_registry[i];
    size_t distance = ny_cli_edit_distance(option, entry->name);
    if (distance < best_distance) {
      best_distance = distance;
      best = entry->name;
    }
    for (size_t alias = 0;
         alias < sizeof(entry->aliases) / sizeof(entry->aliases[0]); ++alias) {
      if (!entry->aliases[alias])
        continue;
      distance = ny_cli_edit_distance(option, entry->aliases[alias]);
      if (distance < best_distance) {
        best_distance = distance;
        best = entry->aliases[alias];
      }
    }
  }
  return best_distance <= max_distance ? best : NULL;
}

static void ny_cli_reject_conflict(bool present_a, const char *name_a,
                                   bool present_b, const char *name_b) {
  if (!present_a || !present_b)
    return;
  fprintf(stderr, "ny: conflicting options '%s' and '%s'\n", name_a, name_b);
  exit(2);
}

static void ny_cli_validate_conflicts(int argc, char **argv) {
  ny_cli_reject_conflict(
      ny_cli_has_flag(argc, argv, "--wasm"), "--wasm",
      ny_cli_option_is(argc, argv, "--native-backend", "x86_64"),
      "--native-backend=x86_64");
  ny_cli_reject_conflict(
      ny_cli_has_flag(argc, argv, "--strict-types"), "--strict-types",
      ny_cli_has_flag(argc, argv, "--no-strict-types"), "--no-strict-types");
  /*
   * Repeated --heap= values stay last-wins. Naming a GC heap together with
   * an ownership heap, or a RAII heap together with RC, is a contradiction:
   * the finalizer used to drop one side and keep going.
   */
  ny_cli_reject_conflict(
      ny_cli_option_is(argc, argv, "--heap", "gc"), "--heap=gc",
      ny_cli_has_flag(argc, argv, "--ownership") ||
          ny_cli_has_flag(argc, argv, "--raii"),
      "--ownership");
  ny_cli_reject_conflict(
      ny_cli_option_is(argc, argv, "--heap", "raii"), "--heap=raii",
      ny_cli_has_flag(argc, argv, "--rc-gc"), "--rc-gc");
  bool jit = ny_cli_has_flag(argc, argv, "--jit") ||
             ny_cli_has_flag(argc, argv, "-jit");
  bool native = ny_cli_has_flag(argc, argv, "--native") ||
                ny_cli_has_flag(argc, argv, "-native");
  bool run_aot = ny_cli_option_is(argc, argv, "--run", "aot");
  bool run_native = ny_cli_option_is(argc, argv, "--run", "native");
  ny_cli_reject_conflict(jit, "--jit", native, "--native");
  ny_cli_reject_conflict(run_aot, "--run=aot", jit, "--jit");
  ny_cli_reject_conflict(run_native, "--run=native", jit, "--jit");
  ny_cli_reject_conflict(ny_cli_has_flag(argc, argv, "--release"), "--release",
                         ny_cli_has_flag(argc, argv, "--debug"), "--debug");
  ny_cli_reject_conflict(ny_cli_has_flag(argc, argv, "-run"), "-run",
                         ny_cli_has_flag(argc, argv, "-emit-only"),
                         "-emit-only");
  ny_cli_reject_conflict(ny_cli_has_flag(argc, argv, "-gc"), "-gc",
                         ny_cli_has_flag(argc, argv, "-no-gc"), "-no-gc");
  ny_cli_reject_conflict(ny_cli_has_flag(argc, argv, "--repl"), "--repl",
                         ny_cli_has_command_string(argc, argv), "-c");
  ny_cli_reject_conflict(ny_cli_option_is(argc, argv, "--gpu", "off"),
                         "--gpu=off",
                         ny_cli_option_is(argc, argv, "--gpu-offload", "force"),
                         "--gpu-offload=force");
  ny_cli_reject_conflict(
      ny_cli_option_is(argc, argv, "--gpu", "off"), "--gpu=off",
      ny_options_cli_list_has_item(argc, argv, "--gpu-opt", "offload=force"),
      "--gpu-opt=offload=force");
  ny_cli_reject_conflict(ny_cli_option_is(argc, argv, "--gpu-backend", "cuda"),
                         "--gpu-backend=cuda",
                         ny_cli_option_is(argc, argv, "--gpu-target", "nvptx"),
                         "--gpu-target=nvptx");
}

void ny_options_validate_conflicts(int argc, char **argv) {
  ny_cli_validate_conflicts(argc, argv);
}

static const char *ny_options_derive_emit_path(ny_options *opt,
                                               const char *extension) {
  const char *source = opt->input_file && strcmp(opt->input_file, "-") != 0
                           ? opt->input_file
                           : "a";
  size_t source_len = strlen(source);
  const char *slash = strrchr(source, '/');
  const char *dot = strrchr(source, '.');
  if (dot && (!slash || dot > slash))
    source_len = (size_t)(dot - source);
  char path[4096];
  int length = snprintf(path, sizeof(path), "%.*s%s", (int)source_len, source,
                        extension);
  if (length < 0 || (size_t)length >= sizeof(path)) {
    fprintf(stderr, "ny: derived output path is too long\n");
    exit(2);
  }
  return ny_options_copy_owned_value(opt, path, (size_t)length);
}

static void ny_options_resolve_emit_paths(ny_options *opt) {
#define NY_RESOLVE_EMIT_PATH(field, extension)                  \
  do {                                                          \
    if (opt->field == ny_emit_default_path_marker)              \
      opt->field = ny_options_derive_emit_path(opt, extension); \
  } while (0)
  NY_RESOLVE_EMIT_PATH(emit_asm_path, ".s");
  NY_RESOLVE_EMIT_PATH(emit_ir_path, ".ll");
  NY_RESOLVE_EMIT_PATH(emit_bc_path, ".bc");
  NY_RESOLVE_EMIT_PATH(output_file, ".o");
  NY_RESOLVE_EMIT_PATH(emit_wasm_path, ".wasm");
  NY_RESOLVE_EMIT_PATH(native_dump_ir_path, ".nyr");
  NY_RESOLVE_EMIT_PATH(nyr_metadata_report_path, ".nyr-meta.json");
  NY_RESOLVE_EMIT_PATH(emit_report_path, ".report.json");
  NY_RESOLVE_EMIT_PATH(emit_artifact_path, ".ny-artifact");
  NY_RESOLVE_EMIT_PATH(emit_refinement_interface_path, ".refinement.json");
  NY_RESOLVE_EMIT_PATH(emit_effect_interface_path, ".effect.json");
  NY_RESOLVE_EMIT_PATH(nyr_dump_bin_path, ".nyrb");
#undef NY_RESOLVE_EMIT_PATH
}

static void ny_options_format_cli_flag(int argc, char **argv, int *index,
                                       char *formatted, size_t capacity) {
  const char *arg = argv[*index];
  if (strcmp(arg, "-c") == 0 && *index + 1 < argc) {
    snprintf(formatted, capacity, "-c=%s", argv[++(*index)]);
    return;
  }
  if (strncmp(arg, "-O", 2) == 0 && arg[2] &&
      (arg[2] == '0' || arg[2] == '1' || arg[2] == '2' || arg[2] == '3' ||
       arg[2] == 's' || arg[2] == 'z')) {
    snprintf(formatted, capacity, "--opt-level=%s", arg + 2);
    return;
  }
  const char *equals = strchr(arg, '=');
  size_t name_length = equals ? (size_t)(equals - arg) : strlen(arg);
  const ny_option_registry_entry_t *entry =
      ny_registry_find_spelling(arg, name_length);
  if (!entry) {
    snprintf(formatted, capacity, "%s", arg);
    return;
  }
  if (entry->deprecated_by) {
    const char *replacement = entry->deprecated_by;
    if (strncmp(replacement, "--emit=", 7) == 0) {
      const char *value = equals ? equals + 1 : NULL;
      if (!value && entry->takes_value && *index + 1 < argc)
        value = argv[++(*index)];
      if (value)
        snprintf(formatted, capacity, "%s=%s", replacement, value);
      else
        snprintf(formatted, capacity, "%s", replacement);
      return;
    }
    if (entry->takes_value && !strchr(replacement, '=')) {
      const char *value = equals ? equals + 1 : NULL;
      if (!value && *index + 1 < argc)
        value = argv[++(*index)];
      snprintf(formatted, capacity, "%s=%s", replacement, value ? value : "");
    } else {
      snprintf(formatted, capacity, "%s", replacement);
    }
    return;
  }
  if (!entry->takes_value) {
    snprintf(formatted, capacity, "%s", entry->name);
    return;
  }
  const char *value = equals ? equals + 1 : NULL;
  if (!value && *index + 1 < argc)
    value = argv[++(*index)];
  snprintf(formatted, capacity, "%s=%s", entry->name, value ? value : "");
}

static void ny_options_print_provenance(bool json);

static void ny_options_print_flags(const ny_options *opt, int argc, char **argv,
                                   int program_arg_idx) {
  int end = program_arg_idx >= 0 ? program_arg_idx : argc;
  if (end > argc)
    end = argc;
  if (opt->print_flags_json)
    fputs("{\"flags\":[", stdout);
  else
    fputs("Flags:", stdout);
  bool first = true;
  for (int i = 1; i < end; ++i) {
    if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--print-flags") == 0 ||
        strcmp(argv[i], "--print-flags=json") == 0 ||
        strcmp(argv[i], "--") == 0)
      continue;
    char formatted[8192];
    ny_options_format_cli_flag(end, argv, &i, formatted, sizeof(formatted));
    if (opt->print_flags_json) {
      if (!first)
        fputc(',', stdout);
      ny_registry_write_json_string(stdout, formatted);
    } else {
      fprintf(stdout, " %s", formatted);
    }
    first = false;
  }
  if (opt->print_flags_json) {
    fputs("],\"provenance\":", stdout);
    ny_options_print_provenance(true);
    fputs(",\"program_args\":[", stdout);
    first = true;
    for (int i = end; i < argc; ++i) {
      if (!first)
        fputc(',', stdout);
      ny_registry_write_json_string(stdout, argv[i]);
      first = false;
    }
    fputs("]}\n", stdout);
    return;
  }
  fputc('\n', stdout);
  ny_options_print_provenance(false);
  fputs("-- program args: [", stdout);
  for (int i = end; i < argc; ++i) {
    if (i > end)
      fputs(", ", stdout);
    ny_registry_write_json_string(stdout, argv[i]);
  }
  fputs("]\n", stdout);
}

static const char *ny_options_short_item(char family, char item) {
  static char value[256];
  for (size_t i = 0;
       i < sizeof(ny_option_registry) / sizeof(ny_option_registry[0]); ++i) {
    char entry_family = '\0';
    const char *name = NULL;
    const char *items = NULL;
    if (!ny_registry_short_family(&ny_option_registry[i], &entry_family, &name,
                                  &items) ||
        entry_family != family)
      continue;
    const char *cursor = items;
    char letter = '\0';
    char candidate[256];
    while (ny_registry_short_item_next(&cursor, &letter, candidate,
                                       sizeof(candidate)) > 0) {
      if (letter == item) {
        memcpy(value, candidate, strlen(candidate) + 1);
        return value;
      }
    }
    break;
  }
  return NULL;
}

static bool ny_options_known_short_family(char family) {
  for (size_t i = 0;
       i < sizeof(ny_option_registry) / sizeof(ny_option_registry[0]); ++i) {
    char entry_family = '\0';
    const char *name = NULL;
    const char *items = NULL;
    if (ny_registry_short_family(&ny_option_registry[i], &entry_family, &name,
                                 &items) &&
        entry_family == family)
      return true;
  }
  return false;
}

static void ny_options_append_short_item(char *buffer, size_t capacity,
                                         size_t *length, const char *item) {
  size_t item_length = strlen(item);
  size_t separator = *length ? 1 : 0;
  if (*length + separator + item_length >= capacity) {
    fprintf(stderr, "ny: short option cluster is too long\n");
    exit(2);
  }
  if (separator)
    buffer[(*length)++] = ',';
  memcpy(buffer + *length, item, item_length);
  *length += item_length;
  buffer[*length] = '\0';
}

static void ny_options_push_owned_arg(ny_options *opt, char **expanded,
                                      size_t capacity, size_t *count,
                                      const char *arg) {
  if (*count >= capacity) {
    fprintf(stderr, "ny: too many expanded command-line options\n");
    exit(2);
  }
  expanded[(*count)++] = ny_options_copy_owned_value(opt, arg, strlen(arg));
}

static void ny_options_emit_family(ny_options *opt, char **expanded,
                                   size_t capacity, size_t *count, char family,
                                   const char *items, size_t item_count,
                                   const char *path) {
  char list[8192] = {0};
  size_t list_len = 0;
  char modes[128] = {0};
  size_t modes_len = 0;
  char gpu_opts[256] = {0};
  size_t gpu_opts_len = 0;
  for (size_t i = 0; i < item_count; ++i) {
    const char *value = ny_options_short_item(family, items[i]);
    if (!value) {
      fprintf(stderr, "ny: invalid -%c cluster item '%c'\n", family, items[i]);
      exit(2);
    }
    if (family == 'R') {
      if (strcmp(value, "watch") == 0) {
        ny_options_push_owned_arg(opt, expanded, capacity, count, "--watch");
      } else if (strcmp(value, "safe-run") == 0) {
        ny_options_push_owned_arg(opt, expanded, capacity, count, "--safe-run");
      } else {
        char option[64];
        snprintf(option, sizeof(option), "--run=%s", value);
        ny_options_push_owned_arg(opt, expanded, capacity, count, option);
      }
    } else if (family == 'H') {
      if (strcmp(value, "borrow") == 0) {
        ny_options_push_owned_arg(opt, expanded, capacity, count,
                                  "--ownership-check=advisory");
      } else if (strcmp(value, "strict") == 0) {
        ny_options_push_owned_arg(opt, expanded, capacity, count,
                                  "--ownership-check=strict");
      } else if (strcmp(value, "zero-init") == 0) {
        ny_options_push_owned_arg(opt, expanded, capacity, count,
                                  "--zero-init");
      } else {
        char option[64];
        snprintf(option, sizeof(option), "--heap=%s", value);
        ny_options_push_owned_arg(opt, expanded, capacity, count, option);
      }
    } else if (family == 'W') {
      char option[64];
      snprintf(option, sizeof(option), "--warn=%s", value);
      ny_options_push_owned_arg(opt, expanded, capacity, count, option);
    } else if (family == 'G') {
      if (strcmp(value, "off") == 0 || strcmp(value, "auto") == 0 ||
          strcmp(value, "opencl") == 0) {
        ny_options_append_short_item(modes, sizeof(modes), &modes_len, value);
      } else if (strcmp(value, "cuda") == 0 || strcmp(value, "hip") == 0 ||
                 strcmp(value, "metal") == 0) {
        char option[64];
        snprintf(option, sizeof(option), "--gpu-backend=%s", value);
        ny_options_push_owned_arg(opt, expanded, capacity, count, option);
      } else {
        ny_options_append_short_item(gpu_opts, sizeof(gpu_opts), &gpu_opts_len,
                                     value);
      }
    } else if (family == 'C' &&
               (strcmp(value, "strict") == 0 || strcmp(value, "safe") == 0)) {
      ny_options_push_owned_arg(opt, expanded, capacity, count,
                                strcmp(value, "strict") == 0 ? "--strict"
                                                             : "--safe");
    } else {
      ny_options_append_short_item(list, sizeof(list), &list_len, value);
    }
  }
  if (family == 'G') {
    if (modes_len) {
      char option[160];
      snprintf(option, sizeof(option), "--gpu=%s", modes);
      ny_options_push_owned_arg(opt, expanded, capacity, count, option);
    }
    if (gpu_opts_len) {
      char option[320];
      snprintf(option, sizeof(option), "--gpu-opt=%s", gpu_opts);
      ny_options_push_owned_arg(opt, expanded, capacity, count, option);
    }
    return;
  }
  if (!list_len)
    return;
  const char *prefix = family == 'C'   ? "--check="
                       : family == 'D' ? "--dump="
                       : family == 'E' ? "--emit="
                       : family == 'J' ? "--jit-opt="
                       : family == 'N' ? "--nyr-check="
                       : family == 'P' ? "--prof="
                       : family == 'S' ? "--std="
                       : family == 'T' ? "--trace="
                       : family == 'O' ? "--opt="
                                       : NULL;
  if (!prefix) {
    fprintf(stderr, "ny: unsupported short option family '-%c'\n", family);
    exit(2);
  }
  if (family == 'E' && path) {
    if (list_len + strlen(path) + 2 >= sizeof(list)) {
      fprintf(stderr, "ny: short option output path is too long\n");
      exit(2);
    }
    list[list_len++] = '=';
    memcpy(list + list_len, path, strlen(path) + 1);
  }
  char option[8192];
  int written = snprintf(option, sizeof(option), "%s%s", prefix, list);
  if (written < 0 || (size_t)written >= sizeof(option)) {
    fprintf(stderr, "ny: short option cluster is too long\n");
    exit(2);
  }
  if (family == 'C' || family == 'D' || family == 'E' || family == 'J' ||
      family == 'N' || family == 'O' || family == 'P' || family == 'T') {
    size_t prefix_length = strlen(prefix);
    for (size_t previous = 1; previous < *count; ++previous) {
      if (strncmp(expanded[previous], prefix, prefix_length) != 0)
        continue;
      char merged[8192];
      int merged_length = snprintf(merged, sizeof(merged), "%s%s,%s", prefix,
                                   expanded[previous] + prefix_length, list);
      if (merged_length < 0 || (size_t)merged_length >= sizeof(merged)) {
        fprintf(stderr, "ny: repeated short-option family is too long\n");
        exit(2);
      }
      expanded[previous] =
          ny_options_copy_owned_value(opt, merged, (size_t)merged_length);
      return;
    }
  }
  ny_options_push_owned_arg(opt, expanded, capacity, count, option);
}

static bool ny_options_expand_short_cluster(ny_options *opt, char **expanded,
                                            size_t capacity, size_t *count,
                                            const char *arg) {
  if (!arg || arg[0] != '-' || arg[1] == '-' || !arg[1])
    return false;
  const char *family_at = arg + 1;
  while (*family_at && !isupper((unsigned char)*family_at))
    ++family_at;
  if (!*family_at || !ny_options_known_short_family(*family_at))
    return false;
  for (const char *global = arg + 1; global < family_at; ++global) {
    const char *mapped = NULL;
    char option[32];
    switch (*global) {
    case 'v':
      mapped = "--verbose";
      break;
    case 'q':
      mapped = "--no-progress";
      break;
    case 's':
      mapped = "--strip";
      break;
    case 'g':
      mapped = "-g";
      break;
    case 'h':
      mapped = "--help";
      break;
    case 'i':
      mapped = "-i";
      break;
    default:
      fprintf(stderr, "ny: invalid global short option '%c' in '%s'\n", *global,
              arg);
      exit(2);
    }
    (void)option;
    ny_options_push_owned_arg(opt, expanded, capacity, count, mapped);
  }
  char family = *family_at++;
  const char *items = family_at;
  const char *equal = strchr(items, '=');
  size_t item_count = equal ? (size_t)(equal - items) : strlen(items);
  const char *path = equal ? equal + 1 : NULL;
  if (!item_count) {
    fprintf(stderr, "ny: short option family '-%c' requires an item\n", family);
    exit(2);
  }
  if (path && family != 'E') {
    fprintf(stderr, "ny: only the -E family accepts an output path\n");
    exit(2);
  }
  if (path && !*path) {
    fprintf(stderr, "ny: short option output path is empty\n");
    exit(2);
  }
  ny_options_emit_family(opt, expanded, capacity, count, family, items,
                         item_count, path);
  return true;
}

typedef struct {
  char **items;
  size_t len;
  size_t cap;
  bool stopped;
  bool inline_code;
  bool value_next;
  bool code_next;
  const char *stack[16];
  size_t depth;
} ny_response_state_t;

static void ny_response_push(ny_options *opt, ny_response_state_t *state,
                             const char *value, size_t length) {
  if (state->len == state->cap) {
    size_t next = state->cap ? state->cap * 2 : 16;
    if (next < state->cap || next > SIZE_MAX / sizeof(*state->items)) {
      fprintf(stderr, "ny: response-file argument list is too large\n");
      exit(2);
    }
    char **items = realloc(state->items, next * sizeof(*items));
    if (!items) {
      fprintf(stderr, "ny: out of memory expanding response files\n");
      exit(1);
    }
    state->items = items;
    state->cap = next;
  }
  state->items[state->len++] = ny_options_copy_owned_value(opt, value, length);
}

static void ny_response_process_token(ny_options *opt,
                                      ny_response_state_t *state,
                                      const char *token, size_t length);

static void ny_response_expand_file(ny_options *opt, ny_response_state_t *state,
                                    const char *path, size_t path_length) {
  if (state->depth >= sizeof(state->stack) / sizeof(state->stack[0])) {
    fprintf(stderr, "ny: response-file nesting exceeds 16 levels\n");
    exit(2);
  }
  char *owned_path = ny_options_copy_owned_value(opt, path, path_length);
  for (size_t i = 0; i < state->depth; ++i) {
    if (strcmp(state->stack[i], owned_path) == 0) {
      fprintf(stderr, "ny: response-file cycle at '%s'\n", owned_path);
      exit(2);
    }
  }
  FILE *file = fopen(owned_path, "rb");
  if (!file) {
    fprintf(stderr, "ny: cannot read response file '%s': %s\n", owned_path,
            strerror(errno));
    exit(2);
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fprintf(stderr, "ny: cannot seek response file '%s'\n", owned_path);
    fclose(file);
    exit(2);
  }
  long file_size = ftell(file);
  if (file_size < 0 || file_size > 1024 * 1024 ||
      fseek(file, 0, SEEK_SET) != 0) {
    fprintf(stderr, "ny: response file '%s' exceeds the 1 MiB limit\n",
            owned_path);
    fclose(file);
    exit(2);
  }
  char *contents = malloc((size_t)file_size + 1);
  if (!contents) {
    fprintf(stderr, "ny: out of memory reading response file '%s'\n",
            owned_path);
    fclose(file);
    exit(1);
  }
  size_t read_size = fread(contents, 1, (size_t)file_size, file);
  if (read_size != (size_t)file_size || ferror(file)) {
    fprintf(stderr, "ny: failed reading response file '%s'\n", owned_path);
    free(contents);
    fclose(file);
    exit(2);
  }
  fclose(file);
  contents[read_size] = '\0';
  state->stack[state->depth++] = owned_path;
  size_t position = 0;
  while (position < read_size) {
    while (position < read_size && isspace((unsigned char)contents[position]))
      ++position;
    if (position == read_size)
      break;
    char token[8192];
    size_t token_length = 0;
    char quote = '\0';
    bool started = false;
    while (position < read_size) {
      char ch = contents[position++];
      if (!quote && isspace((unsigned char)ch))
        break;
      if (ch == '\\' && position < read_size) {
        ch = contents[position++];
        started = true;
      } else if (quote) {
        if (ch == quote) {
          quote = '\0';
          started = true;
          continue;
        }
      } else if (ch == '\'' || ch == '"') {
        quote = ch;
        started = true;
        continue;
      }
      if (token_length + 1 >= sizeof(token)) {
        fprintf(stderr, "ny: response-file token exceeds 8191 bytes\n");
        free(contents);
        exit(2);
      }
      token[token_length++] = ch;
      started = true;
    }
    if (quote) {
      fprintf(stderr, "ny: unterminated quote in response file '%s'\n",
              owned_path);
      free(contents);
      exit(2);
    }
    if (started)
      ny_response_process_token(opt, state, token, token_length);
  }
  --state->depth;
  free(contents);
}

static void ny_response_process_token(ny_options *opt,
                                      ny_response_state_t *state,
                                      const char *token, size_t length) {
  if (state->stopped) {
    ny_response_push(opt, state, token, length);
    return;
  }
  if (state->value_next) {
    ny_response_push(opt, state, token, length);
    state->value_next = false;
    if (state->code_next) {
      state->inline_code = true;
      state->code_next = false;
    }
    return;
  }
  if (length == 2 && token[0] == '-' && token[1] == '-') {
    ny_response_push(opt, state, token, length);
    state->stopped = true;
    return;
  }
  if (state->inline_code && length && token[0] != '-') {
    ny_response_push(opt, state, token, length);
    state->stopped = true;
    return;
  }
  if (length > 1 && token[0] == '@') {
    ny_response_expand_file(opt, state, token + 1, length - 1);
    return;
  }
  if (length && token[0] != '-') {
    ny_response_push(opt, state, token, length);
    if (!state->inline_code)
      state->stopped = true;
    else
      state->stopped = true;
    return;
  }
  ny_response_push(opt, state, token, length);
  if (!length || token[0] != '-')
    return;
  char local[8192];
  if (length >= sizeof(local))
    return;
  memcpy(local, token, length);
  local[length] = '\0';
  const char *equals = strchr(local, '=');
  size_t option_length = equals ? (size_t)(equals - local) : length;
  const ny_option_registry_entry_t *entry =
      ny_registry_find_spelling(local, option_length);
  if ((strcmp(local, "-c") == 0 || strcmp(local, "-e") == 0 ||
       strcmp(local, "--eval") == 0 || strcmp(local, "--eval-repl") == 0 ||
       (entry && (strcmp(entry->name, "--eval") == 0 ||
                  strcmp(entry->name, "--eval-repl") == 0))) &&
      !equals) {
    state->value_next = true;
    state->code_next = true;
    return;
  }
  if ((strncmp(local, "-c", 2) == 0 || strncmp(local, "-e", 2) == 0) &&
      length > 2 && local[2] != '=' && !entry) {
    state->inline_code = true;
    return;
  }
  if (entry && entry->takes_value && !equals)
    state->value_next = true;
}

static bool ny_options_has_response_input(int argc, char **argv) {
  bool code_value = false;
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i] ? argv[i] : "";
    if (strcmp(arg, "--") == 0)
      return false;
    if (code_value) {
      code_value = false;
      continue;
    }
    if (arg[0] == '@' && arg[1])
      return true;
    if (strcmp(arg, "-c") == 0 || strcmp(arg, "-e") == 0 ||
        strcmp(arg, "--eval") == 0 || strcmp(arg, "--eval-repl") == 0) {
      code_value = true;
      continue;
    }
    if (arg[0] != '-') {
      /*
       * The first positional operand is the script or program input.
       */
      return false;
    }
    const char *equals = strchr(arg, '=');
    size_t length = equals ? (size_t)(equals - arg) : strlen(arg);
    const ny_option_registry_entry_t *entry =
        ny_registry_find_spelling(arg, length);
    if (entry && entry->takes_value && !equals)
      ++i;
  }
  return false;
}

static char **ny_options_expand_response_args(ny_options *opt, int argc,
                                              char **argv, int *expanded_argc) {
  ny_response_state_t state = {0};
  ny_response_push(opt, &state, argv[0], strlen(argv[0]));
  for (int i = 1; i < argc; ++i) {
    if (state.stopped)
      ny_response_push(opt, &state, argv[i], strlen(argv[i]));
    else
      ny_response_process_token(opt, &state, argv[i], strlen(argv[i]));
  }
  *expanded_argc = (int)state.len;
  return state.items;
}

typedef struct {
  char **items;
  size_t len;
  size_t cap;
} ny_cli_arg_list_t;

static ny_cli_arg_list_t ny_cli_provenance[4];

typedef struct {
  char *name;
  char *value;
} ny_cli_alias_t;

static ny_cli_alias_t *ny_cli_aliases;
static size_t ny_cli_alias_count;

static void ny_cli_print_source(const ny_cli_arg_list_t *source, bool json) {
  fputc('[', stdout);
  bool first = true;
  for (size_t i = 0; i < source->len; ++i) {
    const char *arg = source->items[i];
    if (strcmp(arg, "--") == 0)
      break;
    if (strcmp(arg, "-n") == 0 || strcmp(arg, "--print-flags") == 0 ||
        strcmp(arg, "--print-flags=json") == 0)
      continue;
    if (arg[0] != '-' && arg[0] != '+')
      break;
    int index = (int)i;
    char formatted[8192];
    ny_options_format_cli_flag((int)source->len, source->items, &index,
                               formatted, sizeof(formatted));
    i = (size_t)index;
    if (!first)
      fputs(json ? "," : ", ", stdout);
    if (json)
      ny_registry_write_json_string(stdout, formatted);
    else
      fputs(formatted, stdout);
    first = false;
  }
  fputc(']', stdout);
}

static void ny_options_print_provenance(bool json) {
  static const char *const names[] = {"user", "project", "env", "cli"};
  if (json) {
    fputs("{\"defaults\":[],", stdout);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
      if (i)
        fputc(',', stdout);
      ny_registry_write_json_string(stdout, names[i]);
      fputc(':', stdout);
      ny_cli_print_source(&ny_cli_provenance[i], true);
    }
    fputc('}', stdout);
    return;
  }
  fputs("Sources: defaults=[]", stdout);
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    fprintf(stdout, " %s=", names[i]);
    ny_cli_print_source(&ny_cli_provenance[i], false);
  }
  fputc('\n', stdout);
}

static void ny_cli_arg_push(ny_options *opt, ny_cli_arg_list_t *list,
                            const char *value) {
  if (list->len == list->cap) {
    size_t next = list->cap ? list->cap * 2 : 16;
    if (next < list->cap || next > SIZE_MAX / sizeof(*list->items)) {
      fprintf(stderr, "ny: configured argument list is too large\n");
      exit(2);
    }
    char **items = realloc(list->items, next * sizeof(*items));
    if (!items) {
      fprintf(stderr, "ny: out of memory loading CLI configuration\n");
      exit(1);
    }
    list->items = items;
    list->cap = next;
  }
  list->items[list->len++] =
      ny_options_copy_owned_value(opt, value, strlen(value));
}

static char *ny_cli_trim(char *text) {
  while (isspace((unsigned char)*text))
    ++text;
  size_t length = strlen(text);
  while (length && isspace((unsigned char)text[length - 1]))
    text[--length] = '\0';
  return text;
}

static bool ny_cli_tokenize(ny_options *opt, ny_cli_arg_list_t *list,
                            const char *text, bool options_only) {
  char token[8192];
  size_t token_length = 0;
  char quote = '\0';
  bool started = false;
  for (const char *p = text;; ++p) {
    char ch = *p;
    if (!quote && (ch == '\0' || isspace((unsigned char)ch))) {
      if (started) {
        token[token_length] = '\0';
        if (options_only && (strcmp(token, "--") == 0 ||
                             (token[0] != '-' && token[0] != '+'))) {
          fprintf(stderr,
                  "ny: CLI configuration may contain options only; got '%s'\n",
                  token);
          return false;
        }
        ny_cli_arg_push(opt, list, token);
        token_length = 0;
        started = false;
      }
      if (!ch)
        break;
      continue;
    }
    if (ch == '\0') {
      fprintf(stderr, "ny: unterminated quote in CLI configuration\n");
      return false;
    }
    if (ch == '\\' && quote != '\'') {
      if (!p[1]) {
        fprintf(stderr, "ny: trailing escape in CLI configuration\n");
        return false;
      }
      ch = *++p;
      started = true;
    } else if (quote) {
      if (ch == quote) {
        quote = '\0';
        started = true;
        continue;
      }
    } else if (ch == '\'' || ch == '"') {
      quote = ch;
      started = true;
      continue;
    }
    if (token_length + 1 >= sizeof(token)) {
      fprintf(stderr, "ny: CLI configuration token exceeds 8191 bytes\n");
      return false;
    }
    token[token_length++] = ch;
    started = true;
  }
  return true;
}

static const ny_option_registry_entry_t *
ny_cli_registry_option(const char *key) {
  char spelling[256];
  int n;
  if (key[0] == '-')
    n = snprintf(spelling, sizeof(spelling), "%s", key);
  else if (strcmp(key, "L") == 0 || strcmp(key, "l") == 0)
    n = snprintf(spelling, sizeof(spelling), "-%s", key);
  else
    n = snprintf(spelling, sizeof(spelling), "--%s", key);
  if (n < 0 || (size_t)n >= sizeof(spelling))
    return NULL;
  return ny_registry_find_spelling(spelling, (size_t)n);
}

static bool ny_cli_config_add_option(ny_options *opt, ny_cli_arg_list_t *list,
                                     const char *key, const char *raw_value,
                                     bool trusted, const char *source) {
  const ny_option_registry_entry_t *entry = ny_cli_registry_option(key);
  if (!entry) {
    fprintf(stderr, "ny: unknown option '%s' in %s configuration\n", key,
            source);
    return false;
  }
  if (strcmp(entry->name, "--trust-config") == 0 &&
      strcmp(source, "user") != 0) {
    fprintf(stderr, "ny: ignoring project configuration option '%s'\n",
            entry->name);
    return true;
  }
  if (entry->config_trust && !trusted) {
    fprintf(stderr,
            "ny: ignoring untrusted %s option '%s' (use --trust-config)\n",
            source, entry->name);
    return true;
  }
  char value[8192];
  size_t value_length = strlen(raw_value);
  if (value_length >= sizeof(value)) {
    fprintf(stderr, "ny: value for '%s' in %s configuration is too long\n",
            entry->name, source);
    return false;
  }
  memcpy(value, raw_value, value_length + 1);
  char *trimmed = ny_cli_trim(value);
  if (trimmed != value)
    memmove(value, trimmed, strlen(trimmed) + 1);
  value_length = strlen(value);
  if (value_length >= 2 &&
      ((value[0] == '"' && value[value_length - 1] == '"') ||
       (value[0] == '\'' && value[value_length - 1] == '\''))) {
    value[value_length - 1] = '\0';
    memmove(value, value + 1, value_length - 1);
  } else if (value_length >= 2 && value[0] == '[' &&
             value[value_length - 1] == ']') {
    char decoded[sizeof(value)];
    size_t decoded_length = 0;
    char quote = '\0';
    for (size_t i = 1; i + 1 < value_length; ++i) {
      char ch = value[i];
      if (quote) {
        if (ch == quote) {
          quote = '\0';
          continue;
        }
      } else if (ch == '"' || ch == '\'') {
        quote = ch;
        continue;
      } else if (ch == ',') {
        if (decoded_length && decoded[decoded_length - 1] != ',')
          decoded[decoded_length++] = ',';
        continue;
      } else if (isspace((unsigned char)ch))
        continue;
      if (decoded_length + 1 >= sizeof(decoded)) {
        fprintf(stderr, "ny: array value for '%s' is too long\n", entry->name);
        return false;
      }
      decoded[decoded_length++] = ch;
    }
    if (quote) {
      fprintf(stderr, "ny: unterminated string in array for '%s'\n",
              entry->name);
      return false;
    }
    if (decoded_length && decoded[decoded_length - 1] == ',')
      --decoded_length;
    decoded[decoded_length] = '\0';
    memcpy(value, decoded, decoded_length + 1);
  }
  if (strcmp(value, "true") == 0 && !entry->takes_value) {
    ny_cli_arg_push(opt, list, entry->name);
    return true;
  }
  if (strcmp(value, "false") == 0 && !entry->takes_value) {
    char negative[256];
    int n = snprintf(negative, sizeof(negative), "--no-%s", key);
    if (n < 0 || (size_t)n >= sizeof(negative) ||
        !ny_registry_find_spelling(negative, (size_t)n)) {
      fprintf(stderr, "ny: '%s' has no false form in CLI configuration\n",
              entry->name);
      return false;
    }
    ny_cli_arg_push(opt, list, negative);
    return true;
  }
  if (!entry->takes_value) {
    fprintf(stderr, "ny: '%s' in %s configuration requires true or false\n",
            entry->name, source);
    return false;
  }
  char argument[8448];
  int n = snprintf(argument, sizeof(argument), "%s=%s", entry->name, value);
  if (n < 0 || (size_t)n >= sizeof(argument)) {
    fprintf(stderr, "ny: value for '%s' in configuration is too long\n",
            entry->name);
    return false;
  }
  ny_cli_arg_push(opt, list, argument);
  return true;
}

static bool ny_cli_load_config(ny_options *opt, ny_cli_arg_list_t *list,
                               const char *path, bool trusted,
                               bool user_config) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    if (errno == ENOENT)
      return true;
    fprintf(stderr, "ny: cannot read CLI config '%s': %s\n", path,
            strerror(errno));
    return false;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fprintf(stderr, "ny: cannot seek CLI config '%s'\n", path);
    fclose(file);
    return false;
  }
  long size = ftell(file);
  if (size < 0 || size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
    fprintf(stderr, "ny: CLI config '%s' exceeds the 1 MiB limit\n", path);
    fclose(file);
    return false;
  }
  char *contents = malloc((size_t)size + 1);
  if (!contents) {
    fclose(file);
    fprintf(stderr, "ny: out of memory reading CLI config '%s'\n", path);
    return false;
  }
  size_t length = fread(contents, 1, (size_t)size, file);
  bool read_failed = length != (size_t)size || ferror(file);
  fclose(file);
  if (read_failed) {
    free(contents);
    fprintf(stderr, "ny: failed reading CLI config '%s'\n", path);
    return false;
  }
  contents[length] = '\0';
  enum {
    NY_CLI_CONFIG_NONE,
    NY_CLI_CONFIG_OPTIONS,
    NY_CLI_CONFIG_ALIASES
  } section = NY_CLI_CONFIG_NONE;
  char *line = contents;
  while (line && *line) {
    char *next = strchr(line, '\n');
    if (next)
      *next++ = '\0';
    char quote = '\0';
    for (char *p = line; *p; ++p) {
      if (quote) {
        if (*p == quote && (p == line || p[-1] != '\\'))
          quote = '\0';
      } else if (*p == '"' || *p == '\'')
        quote = *p;
      else if (*p == '#') {
        *p = '\0';
        break;
      }
    }
    char *item = ny_cli_trim(line);
    if (*item == '[') {
      char *close = strchr(item, ']');
      if (!close || *ny_cli_trim(close + 1)) {
        fprintf(stderr, "ny: invalid section in CLI config '%s'\n", path);
        free(contents);
        return false;
      }
      *close = '\0';
      const char *name = ny_cli_trim(item + 1);
      if (strcmp(name, "options") == 0 || strcmp(name, "flags") == 0)
        section = NY_CLI_CONFIG_OPTIONS;
      else if (strcmp(name, "alias") == 0 && user_config)
        section = NY_CLI_CONFIG_ALIASES;
      else {
        fprintf(stderr, "ny: unsupported section '[%s]' in CLI config '%s'\n",
                name, path);
        free(contents);
        return false;
      }
    } else if (*item) {
      char *equals = strchr(item, '=');
      if (!equals) {
        fprintf(stderr, "ny: expected key = value in CLI config '%s'\n", path);
        free(contents);
        return false;
      }
      *equals++ = '\0';
      char *key = ny_cli_trim(item);
      char *value = ny_cli_trim(equals);
      if (section == NY_CLI_CONFIG_OPTIONS) {
        if (!ny_cli_config_add_option(opt, list, key, value, trusted,
                                      user_config ? "user" : "project")) {
          free(contents);
          return false;
        }
      } else if (section == NY_CLI_CONFIG_ALIASES) {
        if (!*key || strlen(key) > 64) {
          fprintf(stderr, "ny: invalid alias name in CLI config '%s'\n", path);
          free(contents);
          return false;
        }
        bool valid_alias_name = true;
        for (const char *p = key; *p; ++p) {
          if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            valid_alias_name = false;
            break;
          }
        }
        if (!valid_alias_name) {
          fprintf(stderr, "ny: invalid alias name '%s' in CLI config '%s'\n",
                  key, path);
          free(contents);
          return false;
        }
        for (size_t preset = 0;
             preset < sizeof(ny_option_builtin_presets) /
                          sizeof(ny_option_builtin_presets[0]);
             ++preset) {
          if (strcmp(key, ny_option_builtin_presets[preset].name) == 0) {
            fprintf(stderr,
                    "ny: CLI alias '%s' conflicts with built-in preset '+%s'\n",
                    key, key);
            free(contents);
            return false;
          }
        }
        for (size_t alias = 0; alias < ny_cli_alias_count; ++alias) {
          if (strcmp(key, ny_cli_aliases[alias].name) == 0) {
            fprintf(stderr, "ny: duplicate CLI alias '%s' in '%s'\n", key,
                    path);
            free(contents);
            return false;
          }
        }
        ny_cli_alias_t *aliases = realloc(
            ny_cli_aliases, (ny_cli_alias_count + 1) * sizeof(*aliases));
        if (!aliases) {
          free(contents);
          fprintf(stderr, "ny: out of memory reading CLI aliases\n");
          return false;
        }
        ny_cli_aliases = aliases;
        ny_cli_aliases[ny_cli_alias_count].name =
            ny_options_copy_owned_value(opt, key, strlen(key));
        const char *alias_value = value;
        size_t alias_value_length = strlen(value);
        if (alias_value_length >= 2 &&
            ((alias_value[0] == '"' &&
              alias_value[alias_value_length - 1] == '"') ||
             (alias_value[0] == '\'' &&
              alias_value[alias_value_length - 1] == '\''))) {
          ++alias_value;
          alias_value_length -= 2;
        }
        ny_cli_aliases[ny_cli_alias_count].value =
            ny_options_copy_owned_value(opt, alias_value, alias_value_length);
        ++ny_cli_alias_count;
      } else {
        fprintf(stderr, "ny: values in CLI config '%s' need a section\n", path);
        free(contents);
        return false;
      }
    }
    line = next;
  }
  free(contents);
  return true;
}

static bool ny_cli_has_trust_flag(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i] ? argv[i] : "";
    if (strcmp(arg, "--") == 0)
      break;
    if (strcmp(arg, "--trust-config") == 0)
      return true;
    if (arg[0] != '-' && arg[0] != '+')
      break;
  }
  return false;
}

static char **ny_cli_merge_args(ny_options *opt, int argc, char **argv,
                                int *merged_argc) {
  ny_cli_arg_list_t user = {0}, project = {0}, environment = {0}, cli = {0};
  char path[4096];
  const char *home = getenv("HOME");
  if (!home || !*home)
    home = getenv("USERPROFILE");
  bool trusted_project = ny_cli_has_trust_flag(argc, argv);
  if (home && *home) {
    int n = snprintf(path, sizeof(path), "%s/.config/nytrix/ny.toml", home);
    if (n > 0 && (size_t)n < sizeof(path) &&
        !ny_cli_load_config(opt, &user, path, true, true))
      exit(2);
  }
  if (!ny_cli_load_config(opt, &project, "ny.toml", trusted_project, false))
    exit(2);
  const char *environment_flags = getenv("NYTRIX_FLAGS");
  if (!environment_flags || !*environment_flags)
    environment_flags = getenv("NY_FLAGS");
  if (environment_flags && *environment_flags &&
      !ny_cli_tokenize(opt, &environment, environment_flags, true))
    exit(2);
  bool code_next = false;
  bool positionals = false;
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i] ? argv[i] : "";
    if (positionals) {
      ny_cli_arg_push(opt, &cli, arg);
      continue;
    }
    if (strcmp(arg, "--") == 0) {
      positionals = true;
      ny_cli_arg_push(opt, &cli, arg);
      continue;
    }
    if (code_next) {
      code_next = false;
      positionals = true;
      ny_cli_arg_push(opt, &cli, arg);
      continue;
    }
    if (strcmp(arg, "-c") == 0 || strcmp(arg, "-e") == 0 ||
        strcmp(arg, "--eval") == 0 || strcmp(arg, "--eval-repl") == 0) {
      code_next = true;
      ny_cli_arg_push(opt, &cli, arg);
      continue;
    }
    if (arg[0] == '+' && arg[1]) {
      bool is_builtin_preset = false;
      for (size_t preset = 0; preset < sizeof(ny_option_builtin_presets) /
                                           sizeof(ny_option_builtin_presets[0]);
           ++preset) {
        if (strcmp(arg + 1, ny_option_builtin_presets[preset].name) == 0) {
          is_builtin_preset = true;
          break;
        }
      }
      if (is_builtin_preset) {
        ny_cli_arg_push(opt, &cli, arg);
        continue;
      }
      bool found = false;
      for (size_t alias = 0; alias < ny_cli_alias_count; ++alias) {
        if (strcmp(arg + 1, ny_cli_aliases[alias].name) != 0)
          continue;
        if (!ny_cli_tokenize(opt, &cli, ny_cli_aliases[alias].value, true))
          exit(2);
        found = true;
        break;
      }
      if (found)
        continue;
    }
    ny_cli_arg_push(opt, &cli, arg);
    if (arg[0] != '-')
      positionals = true;
  }
  size_t total = 1 + user.len + project.len + environment.len + cli.len;
  if (total > (size_t)INT_MAX || total > SIZE_MAX / sizeof(char *)) {
    fprintf(stderr, "ny: configured argument list is too large\n");
    exit(2);
  }
  char **merged = calloc(total, sizeof(*merged));
  if (!merged) {
    fprintf(stderr, "ny: out of memory merging CLI configuration\n");
    exit(1);
  }
  size_t out = 0;
  merged[out++] = argv[0];
  ny_cli_arg_list_t *layers[] = {&user, &project, &environment, &cli};
  for (size_t layer = 0; layer < sizeof(layers) / sizeof(layers[0]); ++layer)
    for (size_t i = 0; i < layers[layer]->len; ++i)
      merged[out++] = layers[layer]->items[i];
  *merged_argc = (int)out;
  for (size_t layer = 0; layer < sizeof(layers) / sizeof(layers[0]); ++layer) {
    ny_cli_provenance[layer] = *layers[layer];
    layers[layer]->items = NULL;
    layers[layer]->len = 0;
    layers[layer]->cap = 0;
  }
  return merged;
}

static char **ny_options_expand_short_args(ny_options *opt, int argc,
                                           char **argv, int *expanded_argc) {
  size_t capacity = (size_t)argc + 8;
  for (int i = 0; i < argc; ++i) {
    size_t length = argv[i] ? strlen(argv[i]) : 0;
    if (length > (SIZE_MAX - capacity) / 2) {
      fprintf(stderr, "ny: command line is too large\n");
      exit(2);
    }
    capacity += length * 2;
  }
  char **expanded = calloc(capacity, sizeof(*expanded));
  if (!expanded) {
    fprintf(stderr, "ny: out of memory expanding short options\n");
    exit(1);
  }
  size_t count = 0;
  expanded[count++] = argv[0];
  bool inline_code = false;
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i] ? argv[i] : "";
    if (strcmp(arg, "-s") == 0) {
      ny_options_push_owned_arg(opt, expanded, capacity, &count, "--strip");
      continue;
    }
    if (strcmp(arg, "-q") == 0) {
      ny_options_push_owned_arg(opt, expanded, capacity, &count,
                                "--no-progress");
      continue;
    }
    /*
     * Preserve the deprecated multi-character spelling before the legacy
     * -tTARGET shorthand can consume it as a target option.
     */
    if (strcmp(arg, "-timeout") == 0 ||
        strncmp(arg, "-timeout=", sizeof("-timeout=") - 1) == 0) {
      expanded[count++] = argv[i];
      if (strcmp(arg, "-timeout") == 0 && i + 1 < argc)
        expanded[count++] = argv[++i];
      continue;
    }
    if (strcmp(arg, "--") == 0) {
      while (i < argc)
        expanded[count++] = argv[i++];
      break;
    }
    if (strcmp(arg, "-c") == 0 || strcmp(arg, "-e") == 0 ||
        strcmp(arg, "--eval") == 0 || strcmp(arg, "--eval-repl") == 0) {
      expanded[count++] = argv[i];
      if (i + 1 < argc) {
        expanded[count++] = argv[++i];
        inline_code = true;
      }
      continue;
    }
    if (strncmp(arg, "--eval=", 7) == 0 ||
        strncmp(arg, "--eval-repl=", 12) == 0 || strncmp(arg, "-c=", 3) == 0 ||
        strncmp(arg, "-e=", 3) == 0) {
      expanded[count++] = argv[i];
      inline_code = true;
      continue;
    }
    if (strncmp(arg, "-c", 2) == 0 && arg[2] && arg[2] != '=' &&
        !ny_registry_find_spelling(arg, strlen(arg))) {
      ny_options_push_owned_arg(opt, expanded, capacity, &count, "-c");
      expanded[count++] = (char *)(arg + 2);
      inline_code = true;
      continue;
    }
    if (strncmp(arg, "-e", 2) == 0 && arg[2] && arg[2] != '=' &&
        !ny_registry_find_spelling(arg, strlen(arg))) {
      ny_options_push_owned_arg(opt, expanded, capacity, &count, "-e");
      expanded[count++] = (char *)(arg + 2);
      inline_code = true;
      continue;
    }
    if (arg[0] == '+' && arg[1]) {
      bool expanded_preset = false;
      for (size_t preset = 0; preset < sizeof(ny_option_builtin_presets) /
                                           sizeof(ny_option_builtin_presets[0]);
           ++preset) {
        if (strcmp(arg + 1, ny_option_builtin_presets[preset].name) != 0)
          continue;
        const char *flag = ny_option_builtin_presets[preset].flags;
        while (*flag) {
          while (*flag == ' ')
            ++flag;
          if (!*flag)
            break;
          const char *end = flag;
          while (*end && *end != ' ')
            ++end;
          size_t flag_length = (size_t)(end - flag);
          if (flag_length >= 128) {
            fprintf(stderr, "ny: built-in preset flag is too long\n");
            exit(2);
          }
          char flag_buffer[128];
          memcpy(flag_buffer, flag, flag_length);
          flag_buffer[flag_length] = '\0';
          ny_options_push_owned_arg(opt, expanded, capacity, &count,
                                    flag_buffer);
          flag = end;
        }
        expanded_preset = true;
        break;
      }
      if (expanded_preset)
        continue;
    }
    if (strcmp(arg, "-O") == 0) {
      fprintf(stderr, "ny: missing optimization level for -O\n");
      exit(2);
    }
    if (arg[0] != '-' || strcmp(arg, "-") == 0) {
      expanded[count++] = argv[i];
      if (inline_code) {
        inline_code = false;
        continue;
      }
      while (++i < argc)
        expanded[count++] = argv[i];
      break;
    }
    if (strcmp(arg, "-j") == 0 || strcmp(arg, "-t") == 0 ||
        strcmp(arg, "-o") == 0) {
      if (i + 1 >= argc ||
          (argv[i + 1][0] == '-' && strcmp(argv[i + 1], "-") != 0)) {
        fprintf(stderr, "ny: missing value for %s\n", arg);
        exit(2);
      }
      const char *name = strcmp(arg, "-j") == 0   ? "--threads="
                         : strcmp(arg, "-t") == 0 ? "--target="
                                                  : "--output=";
      char option[8192];
      int n = snprintf(option, sizeof(option), "%s%s", name, argv[++i]);
      if (n < 0 || (size_t)n >= sizeof(option)) {
        fprintf(stderr, "ny: value for %s is too long\n", arg);
        exit(2);
      }
      ny_options_push_owned_arg(opt, expanded, capacity, &count, option);
      continue;
    }
    if (strncmp(arg, "-j", 2) == 0 && arg[2]) {
      char option[8192];
      snprintf(option, sizeof(option), "--threads=%s", arg + 2);
      ny_options_push_owned_arg(opt, expanded, capacity, &count, option);
      continue;
    }
    if (strncmp(arg, "-t", 2) == 0 && arg[2]) {
      char option[8192];
      snprintf(option, sizeof(option), "--target=%s", arg + 2);
      ny_options_push_owned_arg(opt, expanded, capacity, &count, option);
      continue;
    }
    if (strncmp(arg, "-o", 2) == 0 && arg[2]) {
      char option[8192];
      snprintf(option, sizeof(option), "--output=%s", arg + 2);
      ny_options_push_owned_arg(opt, expanded, capacity, &count, option);
      continue;
    }
    if ((strncmp(arg, "-L", 2) == 0 && arg[2]) ||
        (strncmp(arg, "-l", 2) == 0 && arg[2])) {
      expanded[count++] = argv[i];
      continue;
    }
    if (strncmp(arg, "-O", 2) == 0 && arg[2] &&
        (arg[2] == '0' || arg[2] == '1' || arg[2] == '2' || arg[2] == '3' ||
         arg[2] == 's' || arg[2] == 'z')) {
      char level[32];
      snprintf(level, sizeof(level), "--opt-level=%c", arg[2]);
      ny_options_push_owned_arg(opt, expanded, capacity, &count, level);
      if (arg[3]) {
        char list[256] = {0};
        size_t length = 0;
        for (const char *p = arg + 3; *p; ++p) {
          const char *value = ny_options_short_item('O', *p);
          if (!value) {
            fprintf(stderr, "ny: invalid optimization cluster item '%c'\n", *p);
            exit(2);
          }
          ny_options_append_short_item(list, sizeof(list), &length, value);
        }
        char option[320];
        snprintf(option, sizeof(option), "--opt=%s", list);
        ny_options_push_owned_arg(opt, expanded, capacity, &count, option);
      }
      continue;
    }
    const char *equals = strchr(arg, '=');
    size_t option_length = equals ? (size_t)(equals - arg) : strlen(arg);
    const ny_option_registry_entry_t *entry =
        ny_registry_find_spelling(arg, option_length);
    if (!entry &&
        ny_options_expand_short_cluster(opt, expanded, capacity, &count, arg))
      continue;
    expanded[count++] = argv[i];
    if (entry && entry->takes_value && !equals && i + 1 < argc) {
      expanded[count++] = argv[++i];
    }
  }
  *expanded_argc = (int)count;
  return expanded;
}

/*
 * A post-script option hint is only useful when the program never inspects its
 * own argument list.  Keep the scan bounded and chunk-overlapped so a large
 * source cannot hide "argv" across a read boundary.
 */
static bool ny_source_reads_argv(const char *path) {
  if (!path || !*path)
    return false;
  FILE *file = fopen(path, "rb");
  if (!file)
    return false;
  char buffer[4096];
  size_t have = 0;
  bool found = false;
  for (;;) {
    size_t n = fread(buffer + have, 1, sizeof(buffer) - have, file);
    if (n == 0)
      break;
    have += n;
    for (size_t i = 0; i + 4 <= have; ++i) {
      if (memcmp(buffer + i, "argv", 4) == 0) {
        found = true;
        break;
      }
    }
    if (found || have < sizeof(buffer))
      break;
    memmove(buffer, buffer + have - 3, 3);
    have = 3;
  }
  fclose(file);
  return found;
}

static bool ny_options_needs_short_expansion(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i] ? argv[i] : "";
    if (strcmp(arg, "--") == 0)
      break;
    if (arg[0] == '+')
      return true;
    if (arg[0] != '-' || arg[1] == '-')
      continue;
    if (ny_registry_find_spelling(arg, strlen(arg)))
      continue;
    if (strcmp(arg, "-j") == 0 || strcmp(arg, "-t") == 0 ||
        strcmp(arg, "-q") == 0 || strcmp(arg, "-s") == 0 ||
        (arg[1] == 'j' && arg[2]) || (arg[1] == 't' && arg[2]) ||
        (arg[1] == 'o' && arg[2]) ||
        (arg[1] == 'O' && arg[2] &&
         (arg[2] == '0' || arg[2] == '1' || arg[2] == '2' || arg[2] == '3' ||
          arg[2] == 's' || arg[2] == 'z')) ||
        (arg[1] == 'c' && arg[2]) || (arg[1] == 'e' && arg[2]))
      return true;
    for (const char *letter = arg + 1; *letter; ++letter)
      if (ny_options_known_short_family(*letter))
        return true;
  }
  return false;
}

static void ny_options_parse_expanded(ny_options *opt, int argc, char **argv) {
  if (argc > 0)
    opt->argv0 = argv[0];
  if (argc < 2)
    return;
  bool check_option_registry = getenv("NYTRIX_CHECK_OPTION_REGISTRY") != NULL;
  ny_cli_validate_conflicts(argc, argv);
  int i = 1;
  int program_arg_idx = -1;
  while (i < argc) {
    const char *a = argv[i];
    const char *value = NULL;
    bool unknown_option = false;
    if (opt->command_string && !opt->input_file &&
        (a[0] != '-' || strcmp(a, "-") == 0)) {
      if (program_arg_idx < 0)
        program_arg_idx = i;
      break;
    }
    if (strcmp(a, "--") == 0) {
      i++;
      opt->program_args_explicitly_separated = true;
      if (opt->pending_command_string) {
        if (i < argc) {
          opt->command_string = argv[i];
          opt->pending_command_string = false;
          program_arg_idx = i + 1;
        }
      } else if (opt->mode == NY_MODE_REPL && !opt->input_file) {
        program_arg_idx = i;
      } else if (opt->input_file || opt->command_string) {
        program_arg_idx = i;
      } else if (i < argc) {
        opt->input_file = argv[i];
        opt->file_arg_idx = i;
        program_arg_idx = i + 1;
      }
      break;
    }
    if (a[0] == '-' && strcmp(a, "-") != 0 && !opt->input_file) {
      if (strcmp(a, "--trust-config") == 0) {
        opt->trust_config = true;
      } else if (strcmp(a, "--presets") == 0) {
        opt->mode = NY_MODE_HELP;
        opt->help_topic = "presets";
      } else if (strcmp(a, "-n") == 0 || strcmp(a, "--print-flags") == 0) {
        opt->print_flags = true;
      } else if (strcmp(a, "--print-flags=json") == 0) {
        opt->print_flags = true;
        opt->print_flags_json = true;
      } else if (strncmp(a, "--print-flags=", 14) == 0) {
        fprintf(stderr,
                "ny: invalid --print-flags value '%s' (expected json)\n",
                a + 14);
        exit(2);
      } else if (strcmp(a, "-safe-mode") == 0 ||
                 strcmp(a, "--safe-mode") == 0 || strcmp(a, "--safe") == 0) {
        opt->safe_mode = true;
      } else if (strcmp(a, "--legacy-llvm") == 0 ||
                 strncmp(a, "--legacy-llvm-dump", 18) == 0) {
        fprintf(stderr,
                "--legacy-llvm execution was removed; the unified NYIR-to-LLVM "
                "pipeline is the supported LLVM path\n");
        exit(1);
      } else if (strcmp(a, "--native") == 0 ||
                 strcmp(a, "--native-only") == 0) {
        /*
         * Preserve an explicitly selected backend regardless of flag order.
         * The pipeline diagnoses an incompatible LLVM/native-only pairing;
         * silently replacing it with the host encoder makes `--native-backend`
         * misleading and can select an unrelated cached artifact.
         */
        if (!opt->native_backend_explicit) {
#if defined(__x86_64__) || defined(_M_X64)
          ny_set_native_backend_or_die(opt, "x86_64", argv[0]);
#elif defined(__aarch64__) || defined(_M_ARM64)
          ny_set_native_backend_or_die(opt, "aarch64", argv[0]);
#else
          fprintf(stderr,
                  "--native-only requires an x86-64 or AArch64 host encoder\n");
          exit(1);
#endif
        }
        opt->native_only = true;
        opt->run_aot = true;
        opt->run_jit = false;
        opt->emit_only = false;
      } else if (strcmp(a, "--safe-run") == 0) {
        ny_safe_run_parse(NULL, &opt->safe_run, argv[0]);
      } else if (strncmp(a, "--safe-run=", 11) == 0) {
        ny_safe_run_parse(a + 11, &opt->safe_run, argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--sanitize", &i, argc,
                                                 argv, argv[0])) != NULL) {
        if (strcmp(value, "address") != 0 && strcmp(value, "undefined") != 0 &&
            strcmp(value, "thread") != 0 && strcmp(value, "leak") != 0) {
          fprintf(stderr,
                  "%s: --sanitize: expected address|undefined|thread|leak, "
                  "got '%s'\n",
                  argv[0], value);
          ny_options_usage(argv[0]);
          exit(1);
        }
        opt->sanitize = value;
      } else if ((value = ny_option_value_or_die(a, "--mode", &i, argc, argv,
                                                 argv[0])) != NULL) {
        ny_apply_runtime_mode_or_die(opt, value, argv[0]);
      } else if (ny_options_apply_common_codegen_option(opt, a, &i, argc, argv,
                                                        argv[0])) {
      } else if (ny_options_apply_toggle(opt, a)) {
      } else if (strcmp(a, "--gprof") == 0 || strcmp(a, "-pg") == 0 ||
                 strcmp(a, "--profile-gprof") == 0) {
        opt->gprof = 1;
        opt->debug_symbols = true;
      } else if (strcmp(a, "--profile") == 0) {
        fprintf(stderr, "ny: missing argument for --opt-profile; use "
                        "--opt-profile=VALUE (expected "
                        "speed|balanced|compile|size|none|peak)\n");
        exit(2);
      } else if (strcmp(a, "--no-gprof") == 0) {
        opt->gprof = 0;
      } else if ((value = ny_option_value_or_die(a, "--dwarf-version", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->dwarf_version = ny_parse_dwarf_version_or_die(value, argv[0]);
      } else if (strcmp(a, "--clean-cache") == 0) {
        opt->mode = NY_MODE_CLEAN_CACHE;
      } else if ((value = ny_option_value_or_die(a, "--host-cflags", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->host_cflags = value;
      } else if ((value = ny_option_value_or_die(a, "--host-ldflags", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->host_ldflags = value;
      } else if ((value = ny_option_value_or_die(a, "--target", &i, argc, argv,
                                                 argv[0])) != NULL) {
        if (strcmp(value, "host") == 0) {
          opt->host_triple = NULL;
        } else if (strcmp(value, "wasm") == 0) {
          ny_set_native_backend_or_die(opt, "wasm", argv[0]);
          opt->host_triple = "wasm32-unknown-unknown";
        } else {
          opt->host_triple = value;
        }
      } else if ((value = ny_option_value_or_die(a, "--host-triple", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->host_triple = value;
      } else if ((value = ny_option_value_or_die(a, "--nyr-disable-pass", &i,
                                                 argc, argv, argv[0])) !=
                 NULL) {
        opt->nyr_disable_pass = value;
      } else if ((value = ny_option_value_or_die(a, "--nyr-stop-after", &i,
                                                 argc, argv, argv[0])) !=
                 NULL) {
        opt->nyr_stop_after = value;
      } else if (strcmp(a, "--dump-nyr") == 0) {
        opt->native_dump_ir = true;
        opt->nyr_dump_bin = true;
      } else if (strncmp(a, "--dump-nyr=", 11) == 0) {
        opt->native_dump_ir = true;
        opt->nyr_dump_bin = true;
        opt->nyr_dump_bin_path = a + 11;
      } else if (strcmp(a, "--nyr-metadata") == 0) {
        (void)ny_options_apply_legacy_emit_path(opt, "nyr-metadata", NULL);
      } else if (strncmp(a, "--nyr-metadata=", 15) == 0) {
        (void)ny_options_apply_legacy_emit_path(opt, "nyr-metadata", a + 15);
      } else if ((value = ny_option_value_or_die(a, "--nyr-metadata-bin", &i,
                                                 argc, argv, argv[0])) !=
                 NULL) {
        opt->native_dump_ir = true;
        opt->nyr_metadata_report = true;
        opt->nyr_metadata_bin_path = value;
      } else if (strcmp(a, "--nyr-run") == 0) {
        opt->native_dump_ir = true;
        opt->nyr_run = true;
      } else if (strcmp(a, "--vm-disasm") == 0) {
        opt->vm_disasm = true;
        opt->native_dump_ir = true;
      } else if (strncmp(a, "--vm-disasm=", 12) == 0) {
        if (!a[12]) {
          fprintf(stderr, "empty --vm-disasm name filter\n");
          exit(1);
        }
        opt->vm_disasm = true;
        opt->vm_disasm_filter = a + 12;
        opt->native_dump_ir = true;
      } else if (strcmp(a, "--mixed-disasm") == 0) {
        opt->mixed_disasm = true;
      } else if (strncmp(a, "--mixed-disasm=", 15) == 0) {
        if (!a[15]) {
          fprintf(stderr, "empty --mixed-disasm name filter\n");
          exit(1);
        }
        opt->mixed_disasm = true;
        opt->mixed_disasm_filter = a + 15;
      } else if (strcmp(a, "--native-result-oracle") == 0) {
        opt->native_result_oracle = true;
        opt->native_backend = NY_NATIVE_BACKEND_X86_64;
        opt->native_backend_raw = "x86_64";
      } else if (strncmp(a, "--native-result-oracle=", 23) == 0) {
        opt->native_result_oracle = true;
        opt->native_result_oracle_expected = a + 23;
        opt->native_backend = NY_NATIVE_BACKEND_X86_64;
        opt->native_backend_raw = "x86_64";
      } else if (strcmp(a, "--native-oracle-per-pass") == 0) {
        opt->native_oracle_per_pass = true;
        opt->native_result_oracle = true;
        opt->native_backend = NY_NATIVE_BACKEND_X86_64;
        opt->native_backend_raw = "x86_64";
      } else if (strncmp(a, "--native-oracle-fuzz=", 21) == 0) {
        opt->native_oracle_fuzz_count = ny_parse_nonneg_int_or_die(
            a + 21, "native oracle fuzz count", argv[0]);
        opt->native_result_oracle = true;
        opt->native_backend = NY_NATIVE_BACKEND_X86_64;
        opt->native_backend_raw = "x86_64";
      } else if (strcmp(a, "--native-tv-seed") == 0) {
        opt->native_tv_seed_trials = 16;
      } else if (strncmp(a, "--native-tv-seed=", 17) == 0) {
        opt->native_tv_seed_trials = ny_parse_nonneg_int_or_die(
            a + 17, "native TV seed trials", argv[0]);
      } else if (strncmp(a, "--nyr-run=", 10) == 0) {
        opt->native_dump_ir = true;
        opt->nyr_run = true;
        opt->nyr_run_path = a + 10;
      } else if ((value = ny_option_value_or_die(a, "--nyr-run-bin", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->native_dump_ir = true;
        opt->nyr_run = true;
        opt->nyr_run_bin_path = value;
      } else if (strcmp(a, "--nyr-run-profile") == 0) {
        opt->native_dump_ir = true;
        opt->nyr_run = true;
        opt->nyr_run_profile = true;
      } else if (strncmp(a, "--nyr-run-profile=", 18) == 0) {
        opt->native_dump_ir = true;
        opt->nyr_run = true;
        opt->nyr_run_profile = true;
        opt->nyr_run_profile_path = a + 18;
      } else if (strncmp(a, "--native-profile-use=", 21) == 0) {
        opt->native_profile_use_path = a + 21;
      } else if ((value = ny_option_value_or_die(a, "--nyr-run-max-steps", &i,
                                                 argc, argv, argv[0])) !=
                 NULL) {
        opt->native_dump_ir = true;
        opt->nyr_run = true;
        opt->nyr_run_max_steps =
            ny_parse_nonneg_int_or_die(value, "NYR VM max steps", argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--nyr-run-recursion-limit",
                                                 &i, argc, argv, argv[0])) !=
                 NULL) {
        opt->native_dump_ir = true;
        opt->nyr_run = true;
        opt->nyr_run_recursion_limit = ny_parse_nonneg_int_or_die(
            value, "NYR VM recursion limit", argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--arm-float-abi", &i, argc,
                                                 argv, argv[0])) != NULL) {
        if (!ny_is_arm_float_abi(value)) {
          fprintf(stderr,
                  "ny: invalid --arm-float-abi '%s' "
                  "(expected soft|softfp|hard)\n",
                  value);
          exit(2);
        }
        opt->arm_float_abi = value;
      } else if ((value = ny_option_value_or_die(a, "--opt-level", &i, argc,
                                                 argv, argv[0])) != NULL) {
        ny_set_opt_level_value_or_die(opt, value);
      } else if (strcmp(a, "-O") == 0) {
        fprintf(stderr, "ny: missing optimization level for -O "
                        "(expected 0|1|2|3|s|z)\n");
        exit(2);
      } else if (strcmp(a, "-O2") == 0) {
        ny_set_opt_level(opt, 2);
      } else if ((value = ny_option_value_or_die(a, "--opt-profile", &i, argc,
                                                 argv, argv[0])) != NULL ||
                 (value = ny_option_value_or_die(a, "--profile", &i, argc, argv,
                                                 argv[0])) != NULL) {
        if (!ny_is_opt_profile_name(value)) {
          fprintf(stderr,
                  "ny: invalid --opt-profile '%s' (expected "
                  "speed|balanced|compile|size|none|peak)\n",
                  value);
          exit(2);
        }
        opt->opt_profile = value;
      } else if (strcmp(a, "-O1") == 0) {
        ny_set_opt_level(opt, 1);
      } else if (strcmp(a, "-O0") == 0) {
        ny_set_opt_level(opt, 0);
      } else if (strcmp(a, "-O3") == 0) {
        ny_set_opt_level(opt, 3);
      } else if (strcmp(a, "-Oz") == 0) {
        ny_set_opt_level(opt, 0);
        opt->opt_size = true;
      } else if (strcmp(a, "-Os") == 0) {
        ny_set_opt_level(opt, 2);
        opt->opt_size = true;
      } else if (strcmp(a, "--fast") == 0) {
        if (!opt->opt_level)
          opt->opt_level = 2;
        opt->opt_size = false;
        opt->verify_module = 0;
        opt->strip_override = 1;
      } else if (strcmp(a, "--release") == 0) {
        /*
         * -Os AOT preset: compact native artifact defaults (explicit -O2
         * so later -O flags still override), no verify pass, stripped.
         */
        ny_set_opt_level(opt, 2);
        opt->opt_size = true;
        opt->verify_module = 0;
        opt->strip_override = 1;
      } else if ((value = ny_option_value_or_die(a, "-passes", &i, argc, argv,
                                                 argv[0])) != NULL)
        opt->opt_pipeline = value;
      else if ((value = ny_option_value_or_die(a, "--gpu", &i, argc, argv,
                                               argv[0])) != NULL) {
        ny_set_option_choice_or_die(&opt->gpu_mode, value, ny_is_gpu_mode,
                                    "--gpu");
      } else if ((value = ny_option_value_or_die(a, "--gpu-backend", &i, argc,
                                                 argv, argv[0])) != NULL) {
        value = ny_gpu_backend_option_value(value);
        ny_set_option_choice_or_die(&opt->gpu_backend, value, ny_is_gpu_backend,
                                    "--gpu-backend");
      } else if ((value = ny_option_value_or_die(a, "--accel-target", &i, argc,
                                                 argv, argv[0])) != NULL) {
        ny_set_accel_target_or_die(opt, value, argv[0], "--accel-target");
      } else if ((value = ny_option_value_or_die(a, "--accel-object", &i, argc,
                                                 argv, argv[0])) != NULL) {
        ny_set_accel_object_or_die(opt, value, argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--gpu-target", &i, argc,
                                                 argv, argv[0])) != NULL) {
        ny_set_gpu_target_compat_or_die(opt, value, argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--gpu-offload", &i, argc,
                                                 argv, argv[0])) != NULL) {
        ny_set_option_choice_or_die(&opt->gpu_offload, value,
                                    ny_is_gpu_offload_mode, "--gpu-offload");
      } else if ((value = ny_option_value_or_die(a, "--gpu-min-work", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->gpu_min_work =
            ny_parse_nonneg_int_or_die(value, "gpu min work", argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--parallel", &i, argc,
                                                 argv, argv[0])) != NULL) {
        ny_set_option_choice_or_die(&opt->parallel_mode, value,
                                    ny_is_parallel_mode, "--parallel");
      } else if ((value = ny_option_value_or_die(a, "--threads", &i, argc, argv,
                                                 argv[0])) != NULL) {
        opt->thread_count =
            ny_parse_nonneg_int_or_die(value, "thread count", argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--parallel-min-work", &i,
                                                 argc, argv, argv[0])) !=
                 NULL) {
        opt->parallel_min_work =
            ny_parse_nonneg_int_or_die(value, "parallel min work", argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--bundle-core", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->bundle_std_path = value;
        opt->mode = NY_MODE_BUNDLE;
      } else if ((value = ny_option_value_or_die(a, "--bundle-std", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->bundle_std_path = value;
        opt->mode = NY_MODE_BUNDLE;
      } else if ((value = ny_option_value_or_die(a, "--bundle-symbols", &i,
                                                 argc, argv, argv[0])) !=
                 NULL) {
        opt->bundle_symbols_path = value;
        opt->mode = NY_MODE_BUNDLE;
      } else if (strcmp(a, "--run") == 0 && i + 1 < argc &&
                 (strcmp(argv[i + 1], "auto") == 0 ||
                  strcmp(argv[i + 1], "aot") == 0 ||
                  strcmp(argv[i + 1], "jit") == 0 ||
                  strcmp(argv[i + 1], "native") == 0 ||
                  strcmp(argv[i + 1], "none") == 0)) {
        const char *mode = argv[++i];
        if (strcmp(mode, "none") == 0) {
          opt->run_aot = false;
          opt->run_jit = false;
          opt->native_only = false;
          opt->emit_only = true;
        } else {
          opt->run_aot = strcmp(mode, "jit") != 0;
          opt->run_jit = strcmp(mode, "jit") == 0;
          opt->native_only = strcmp(mode, "native") == 0;
          opt->emit_only = false;
        }
      } else if (strcmp(a, "--run=none") == 0) {
        opt->run_aot = false;
        opt->run_jit = false;
        opt->native_only = false;
        opt->emit_only = true;
      } else if (strcmp(a, "-run") == 0 || strcmp(a, "--run") == 0 ||
                 strcmp(a, "--run-aot") == 0 ||
                 strcmp(a, "--run-native") == 0 ||
                 strcmp(a, "--run=auto") == 0 || strcmp(a, "--run=aot") == 0 ||
                 strcmp(a, "--run=native") == 0) {
        opt->run_aot = true;
        opt->run_jit = false;
        opt->native_only =
            strcmp(a, "--run=native") == 0 || strcmp(a, "--run-native") == 0 ||
            ((strcmp(a, "-run") == 0 || strcmp(a, "--run") == 0) &&
             opt->native_only);
        opt->emit_only = false;
      } else if (strcmp(a, "--jit") == 0 || strcmp(a, "-jit") == 0 ||
                 strcmp(a, "--run-jit") == 0 || strcmp(a, "--run=jit") == 0) {
        opt->run_jit = true;
        opt->run_aot = false;
        opt->native_only = false;
        opt->emit_only = false;
      } else if ((value = ny_option_value_or_die(a, "--jit-engine", &i, argc,
                                                 argv, argv[0])) != NULL) {
        if (strcmp(value, "orc") != 0 && strcmp(value, "mcjit") != 0) {
          fprintf(stderr,
                  "ny: invalid --jit-engine '%s' (expected orc|mcjit)\n",
                  value);
          exit(2);
        }
        opt->jit_engine = value;
      } else if (strncmp(a, "--run=", 6) == 0) {
        fprintf(stderr,
                "ny: invalid --run '%s' (expected auto|aot|jit|native|none)\n",
                a + 6);
        ny_options_usage(argv[0]);
        exit(2);
      } else if (strcmp(a, "-emit-only") == 0) {
        opt->emit_only = true;
        opt->run_aot = false;
      } else if (ny_options_apply_exec_option(opt, a, &i, argc, argv,
                                              argv[0])) {
      } else if (strcmp(a, "--debug") == 0 || strcmp(a, "-debug") == 0) {
        opt->verbose = 3;
        opt->debug_symbols = true;
        opt->strip_override = 0;
        opt->dump_on_error = true;
        opt->debug_locals = 1;
        ny_setenv("NYTRIX_TRACE", "1", 0);
        ny_setenv("NYTRIX_TRACE_CALLS", "1", 0);
        ny_setenv("NYTRIX_TRACE_VALUES", "1", 0);
        ny_setenv("NYTRIX_TRACE_VERBOSE", "1", 0);
        ny_setenv("NYTRIX_DEBUG_LOCALS", "1", 0);
        debug_enabled = 1;
      } else if (strcmp(a, "-verbose") == 0 || strcmp(a, "--verbose") == 0) {
        if (opt->verbose < 1)
          opt->verbose = 1;
      } else if (strncmp(a, "--verbose=", 10) == 0) {
        int lvl = ny_parse_nonneg_int_or_die(a + 10, "verbose level", argv[0]);
        if (lvl > 6)
          lvl = 6;
        if (opt->verbose < lvl)
          opt->verbose = lvl;
      } else if (a[0] == '-' && a[1] == 'v' && (a[2] == '\0' || a[2] == 'v')) {
        int only_vs = 1;
        int lvl = 0;
        for (const char *p = a + 1; *p; ++p) {
          if (*p != 'v') {
            only_vs = 0;
            break;
          }
          lvl++;
        }
        if (only_vs && lvl > 0) {
          if (lvl > 6)
            lvl = 6;
          if (opt->verbose < lvl)
            opt->verbose = lvl;
        } else {
          fprintf(stderr, "ny: unknown option '%s'\n", a);
          ny_options_usage(argv[0]);
        }
      } else if (strcmp(a, "-prof") == 0 || strcmp(a, "--prof") == 0) {
        opt->profiler_mode = true;
        opt->do_timing = true;
        opt->trace_exec = true;
        opt->profiler_output_mode = "smart";
      } else if (strcmp(a, "--progress") == 0) {
        opt->progress = true;
        opt->no_progress = false;
      } else if (strcmp(a, "--no-progress") == 0) {
        opt->no_progress = true;
        opt->progress = false;
      } else if (strcmp(a, "--expand") == 0)
        opt->expand = true;
      else if ((value = ny_option_value_or_die(a, "--expand-only", &i, argc,
                                               argv, argv[0])) != NULL) {
        opt->expand = true;
        opt->expand_only = value;
      } else if (strcmp(a, "--expand-json") == 0) {
        opt->expand = true;
        opt->expand_json = true;
      } else if ((value = ny_option_value_or_die(a, "--ny-desugar", &i, argc,
                                                 argv, argv[0])) != NULL) {
        if (strcmp(value, "ny") == 0)
          opt->ny_desugar = NY_DESUGAR_NY;
        else if (strcmp(value, "c") == 0)
          opt->ny_desugar = NY_DESUGAR_C;
        else if (strcmp(value, "off") == 0)
          opt->ny_desugar = NY_DESUGAR_OFF;
        else {
          fprintf(stderr, "ny: invalid --ny-desugar '%s' (expected ny|c|off)\n",
                  value);
          exit(2);
        }
      } else if ((value = ny_option_value_or_die(a, "--explain-specialization",
                                                 &i, argc, argv, argv[0])) !=
                 NULL) {
        opt->expand = true;
        opt->explain_specialization = value;
      } else if (strcmp(a, "--extract-code") == 0) {
        opt->extract_code = true;
        opt->emit_only = true;
        opt->run_jit = false;
      } else if ((value = ny_option_value_or_die(a, "--extract-lang", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->extract_lang = value;
      } else if ((value = ny_option_value_or_die(a, "--extract-at", &i, argc,
                                                 argv, argv[0])) != NULL) {
        if (!ny_parse_line_col(value, &opt->extract_line, &opt->extract_col)) {
          fprintf(
              stderr,
              "invalid extract location: %s (expected LINE or LINE:COLUMN)\n",
              value);
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if (strcmp(a, "--meta-trace") == 0) {
        opt->expand = true;
        opt->meta_trace = true;
      } else if ((value = ny_option_value_or_die(a, "--warn", &i, argc, argv,
                                                 argv[0])) != NULL) {
        int lvl = 0;
        if (!ny_parse_warn_level(value, &lvl)) {
          fprintf(stderr,
                  "ny: invalid --warn '%s' "
                  "(expected none|useful|all|error)\n",
                  value);
          exit(2);
        }
        opt->warn_level = lvl;
        opt->warn_all_mode =
            strcmp(value, "all") == 0 || strcmp(value, "2") == 0;
      } else if ((value = ny_option_value_or_die(a, "--dump-scope", &i, argc,
                                                 argv, argv[0])) != NULL) {
        ny_dump_scope_t scope = NY_DUMP_SCOPE_PROGRAM;
        if (!ny_parse_dump_scope(value, &scope)) {
          fprintf(stderr,
                  "ny: invalid --dump-scope '%s' "
                  "(expected program|lib|both)\n",
                  value);
          exit(2);
        }
        opt->dump_scope = scope;
      } else if ((value = ny_option_value_or_die(a, "--dump-dir", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->dump_dir = value;
      } else if (strcmp(a, "--dump-diagnose") == 0) {
        opt->dump_diagnose = true;
        opt->dump_on_error = true;
      } else if ((value = ny_option_value_or_die(a, "--entry-name", &i, argc,
                                                 argv, argv[0])) != NULL)
        opt->entry_name = value;
      else if (strncmp(a, "--emit-ir=", 10) == 0)
        opt->emit_ir_path = a + 10;
      else if (strncmp(a, "--emit-bc=", 10) == 0)
        opt->emit_bc_path = a + 10;
      else if (strcmp(a, "--ir-full") == 0) {
        opt->ir_include_std = true;
        opt->dump_scope = NY_DUMP_SCOPE_BOTH;
      } else if (strcmp(a, "--ir-no-std") == 0) {
        opt->ir_include_std = false;
        opt->dump_scope = NY_DUMP_SCOPE_PROGRAM;
      } else if (strncmp(a, "--emit-asm=", 11) == 0)
        opt->emit_asm_path = a + 11;
      else if (strncmp(a, "--emit-module=", 14) == 0)
        opt->emit_module = a + 14;
      else if (strcmp(a, "-gc") == 0) {
        opt->enable_gc = true;
        opt->gc_flag_seen = true;
        if (!opt->heap_policy_explicit)
          opt->heap_policy = NY_HEAP_GC;
      } else if (strcmp(a, "-no-gc") == 0) {
        opt->enable_gc = false;
        if (opt->heap_policy == NY_HEAP_GC)
          opt->heap_policy = NY_HEAP_MANUAL;
      } else if ((value = ny_option_value_or_die(a, "--heap", &i, argc, argv,
                                                 argv[0])) != NULL) {
        ny_set_heap_policy_or_die(opt, value, argv[0]);
      } else if (strcmp(a, "--rc-gc") == 0) {
        ny_set_heap_policy_or_die(opt, "rc", argv[0]);
      } else if (strcmp(a, "--ownership") == 0 || strcmp(a, "--raii") == 0) {
        opt->ownership = true;
        opt->heap_policy = NY_HEAP_RAII;
        opt->heap_policy_explicit = true;
      } else if (strcmp(a, "--ownership-strict") == 0 ||
                 strcmp(a, "--move-semantics") == 0) {
        opt->ownership = true;
        opt->ownership_strict = true;
        if (!opt->heap_policy_explicit)
          opt->heap_policy = NY_HEAP_RAII;
      } else if (strcmp(a, "--borrow-check") == 0) {
        /*
         * Explicitly retain the default advisory provenance pass.
         */
        opt->borrow_check = true;
      } else if (strcmp(a, "--no-ownership-strict") == 0 ||
                 strcmp(a, "--no-borrow-check") == 0) {
        opt->ownership_strict = false;
        opt->borrow_check = false;
      } else if (strcmp(a, "--no-ownership") == 0) {
        opt->ownership = false;
        opt->ownership_strict = false;
        opt->borrow_check = false;
        if (opt->heap_policy == NY_HEAP_RAII || opt->heap_policy == NY_HEAP_RC)
          opt->heap_policy = NY_HEAP_MANUAL;
      } else if ((value = ny_option_value_or_die(a, "--ownership-check", &i,
                                                 argc, argv, argv[0])) !=
                 NULL) {
        if (strcmp(value, "off") == 0) {
          opt->ownership = false;
          opt->ownership_strict = false;
          opt->borrow_check = false;
        } else if (strcmp(value, "advisory") == 0) {
          opt->ownership = false;
          opt->ownership_strict = false;
          opt->borrow_check = true;
        } else if (strcmp(value, "strict") == 0) {
          opt->ownership = true;
          opt->ownership_strict = true;
          opt->borrow_check = false;
          if (!opt->heap_policy_explicit)
            opt->heap_policy = NY_HEAP_RAII;
        } else {
          fprintf(stderr,
                  "ny: invalid --ownership-check '%s' "
                  "(expected off|advisory|strict)\n",
                  value);
          exit(2);
        }
      } else if ((value = ny_option_value_or_die(a, "--timeout", &i, argc, argv,
                                                 argv[0])) != NULL ||
                 (value = ny_option_value_or_die(a, "-timeout", &i, argc, argv,
                                                 argv[0])) != NULL) {
        opt->timeout = ny_parse_nonneg_f64_or_die(value, "timeout", argv[0]);
      } else if (strncmp(a, "--std=", 6) == 0) {
        opt->std_mode = ny_parse_canonical_std_mode_or_die(a + 6);
        opt->std_mode_explicit = true;
      } else if (strcmp(a, "--std") == 0) {
        if (i + 1 >= argc || argv[i + 1][0] == '-') {
          fprintf(stderr,
                  "ny: missing value for --std (expected none|minimal|full)\n");
          exit(2);
        }
        opt->std_mode = ny_parse_canonical_std_mode_or_die(argv[++i]);
        opt->std_mode_explicit = true;
      } else if ((value = ny_option_value_or_die(a, "-std", &i, argc, argv,
                                                 argv[0])) != NULL) {
        opt->std_mode = ny_parse_std_mode_or_die(value, argv[0]);
        opt->std_mode_explicit = true;
      } else if (strcmp(a, "--full-mod") == 0) {
        opt->std_mode = STD_MODE_FULL;
        opt->std_mode_explicit = true;
      } else if (strcmp(a, "-no-std") == 0) {
        opt->no_std = true;
        opt->std_mode_explicit = true;
      } else if (strncmp(a, "--core-path=", 12) == 0) {
        opt->std_path = a + 12;
      } else if (strncmp(a, "--std-path=", 11) == 0) {
        opt->std_path = a + 11;
      } else if (strcmp(a, "--core-bc") == 0 || strcmp(a, "--std-bc") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "missing argument for --core-bc\n");
          ny_options_usage(argv[0]);
          exit(1);
        }
        opt->std_bc_path = argv[++i];
        opt->std_mode = STD_MODE_BC;
        opt->std_mode_explicit = true;
      } else if (strncmp(a, "--core-bc=", 10) == 0) {
        opt->std_bc_path = a + 10;
        opt->std_mode = STD_MODE_BC;
        opt->std_mode_explicit = true;
      } else if (strncmp(a, "--std-bc=", 9) == 0) {
        opt->std_bc_path = a + 9;
        opt->std_mode = STD_MODE_BC;
        opt->std_mode_explicit = true;
      } else if (a[1] == 'L' && a[2] != '\0') {
        vec_push(&opt->link_dirs, ny_strdup(a));
      } else if (strcmp(a, "-L") == 0) {
        ny_push_link_flag_or_die(opt, true, "-L", &i, argc, argv, argv[0]);
      } else if (a[1] == 'l' && a[2] != '\0') {
        vec_push(&opt->link_libs, ny_strdup(a));
      } else if (strcmp(a, "-l") == 0) {
        ny_push_link_flag_or_die(opt, false, "-l", &i, argc, argv, argv[0]);
      } else if (strncmp(a, "--glyphs=", 9) == 0 ||
                 strcmp(a, "--glyphs") == 0) {
        const char *mode = a + 9;
        if (strcmp(a, "--glyphs") == 0) {
          if (i + 1 >= argc) {
            fprintf(stderr, "missing argument for --glyphs\n");
            ny_options_usage(argv[0]);
            exit(1);
          }
          mode = argv[++i];
        }
        if (strcmp(mode, "unicode") == 0 || strcmp(mode, "utf8") == 0 ||
            strcmp(mode, "utf-8") == 0 || strcmp(mode, "rich") == 0) {
          ny_setenv("NYTRIX_GLYPHS", "unicode", 1);
        } else if (strcmp(mode, "ascii") == 0 || strcmp(mode, "plain") == 0 ||
                   strcmp(mode, "fallback") == 0) {
          ny_setenv("NYTRIX_GLYPHS", "ascii", 1);
        } else {
          fprintf(stderr, "invalid glyph mode: %s (expected unicode|ascii)\n",
                  mode);
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if (strncmp(a, "--color=", 8) == 0) {
        const char *mode = a + 8;
        if (strcmp(mode, "auto") == 0)
          opt->color_mode = -1;
        else if (strcmp(mode, "always") == 0)
          opt->color_mode = 1;
        else if (strcmp(mode, "never") == 0)
          opt->color_mode = 0;
        else {
          fprintf(stderr,
                  "invalid color mode: %s (expected auto|always|never)\n",
                  mode);
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if (strncmp(a, "--completions=", 14) == 0 ||
                 strcmp(a, "--completions") == 0) {
        const char *shell = a[13] == '=' ? a + 14 : NULL;
        if (!shell && i + 1 < argc && argv[i + 1][0] != '-')
          shell = argv[++i];
        if (!shell ||
            (strcmp(shell, "bash") != 0 && strcmp(shell, "zsh") != 0 &&
             strcmp(shell, "fish") != 0)) {
          fprintf(stderr,
                  "ny: invalid --completions '%s' (expected bash|zsh|fish)\n",
                  shell ? shell : "");
          exit(2);
        }
        static char completions_topic[32];
        snprintf(completions_topic, sizeof(completions_topic), "completions:%s",
                 shell);
        opt->mode = NY_MODE_HELP;
        opt->help_topic = completions_topic;
      } else if (strncmp(a, "--help=", 7) == 0) {
        opt->mode = NY_MODE_HELP;
        opt->help_topic = a + 7;
      } else if (strcmp(a, "--help-env") == 0 || strcmp(a, "--env-help") == 0) {
        opt->mode = NY_MODE_HELP;
        opt->help_env = true;
        opt->help_topic = "env";
      } else if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0 ||
                 strcmp(a, "--help") == 0) {
        opt->mode = NY_MODE_HELP;
        if (i + 1 < argc &&
            (argv[i + 1][0] != '-' || strncmp(argv[i + 1], "--", 2) == 0)) {
          opt->help_topic = argv[++i];
          opt->help_env = strcmp(opt->help_topic, "env") == 0;
        }
      } else if (strcmp(a, "-version") == 0 || strcmp(a, "--version") == 0) {
        opt->mode = NY_MODE_VERSION;
      } else if (strcmp(a, "--version-id") == 0) {
        opt->mode = NY_MODE_VERSION_ID;
      } else {
        unknown_option = true;
        const char *suggestion = ny_cli_option_suggestion(a);
        if (suggestion)
          fprintf(stderr, "ny: unknown option '%s' (did you mean '%s'?)\n", a,
                  suggestion);
        else
          fprintf(stderr, "ny: unknown option '%s'\n", a);
        ny_options_usage(argv[0]);
      }
    } else {
      if (!opt->input_file && !opt->command_string) {
        if (opt->pending_command_string) {
          opt->command_string = a;
          opt->pending_command_string = false;
          program_arg_idx = i + 1;
        } else {
          opt->input_file = a;
          opt->file_arg_idx = i;
          program_arg_idx = i + 1;
        }
      } else {
        if (program_arg_idx < 0)
          program_arg_idx = i;
        break;
      }
    }
    if (!unknown_option)
      ny_registry_check_parsed_spelling(check_option_registry, a);
    i++;
  }
  if (opt->pending_command_string) {
    fprintf(stderr, "ny: -c requires source code\n");
    exit(2);
  }
  ny_options_resolve_emit_paths(opt);
  if (opt->warn_all_mode) {
    int option_end = program_arg_idx >= 0 ? program_arg_idx : argc;
    if (option_end > argc)
      option_end = argc;
    for (int arg = 1; arg < option_end; ++arg) {
      const char *spelling = argv[arg];
      if (!spelling || spelling[0] != '-')
        continue;
      const char *equals = strchr(spelling, '=');
      size_t length = equals ? (size_t)(equals - spelling) : strlen(spelling);
      const ny_option_registry_entry_t *entry =
          ny_registry_find_spelling(spelling, length);
      if (entry && strcmp(entry->visibility, "deprecated") == 0 &&
          entry->deprecated_by)
        fprintf(stderr, "ny: warning: '%.*s' is deprecated; use '%s'\n",
                (int)length, spelling, entry->deprecated_by);
    }
    /*
     * A compiler-looking option after the script path is forwarded to the
     * program.  Hint once per spelling under --warn=all unless the script
     * inspects argv itself, where the note would be misleading.
     */
    if (program_arg_idx >= 0 && opt->input_file &&
        !getenv("NYTRIX_NO_HINTS") &&
        !ny_source_reads_argv(opt->input_file)) {
      for (int arg = program_arg_idx; arg < argc; ++arg) {
        const char *spelling = argv[arg];
        if (!spelling || spelling[0] != '-' || spelling[1] == '\0')
          continue;
        const char *equals = strchr(spelling, '=');
        size_t length = equals ? (size_t)(equals - spelling) : strlen(spelling);
        if (!ny_registry_find_spelling(spelling, length))
          continue;
        printf("note: '%s' comes after %s\n", spelling, opt->input_file);
      }
    }
  }
  if (!opt->print_flags && opt->emit_wasm_path &&
      !ny_cli_has_flag(argc, argv, "--wasm") &&
      !ny_cli_option_is(argc, argv, "--native-backend", "wasm")) {
    fprintf(stderr,
            "ny: --emit-wasm requires --wasm or --native-backend=wasm\n");
    exit(2);
  }
  if (opt->input_file || opt->command_string || opt->mode == NY_MODE_REPL) {
    if (program_arg_idx < 0)
      program_arg_idx = opt->input_file ? opt->file_arg_idx + 1 : argc;
    if (program_arg_idx > argc)
      program_arg_idx = argc;
    opt->argc = 1 + argc - program_arg_idx;
    opt->args = (char **)calloc((size_t)opt->argc + 1, sizeof(char *));
    if (!opt->args) {
      fprintf(stderr, "ny: out of memory building program arguments\n");
      exit(1);
    }
    opt->args[0] = (char *)(opt->input_file       ? opt->input_file
                            : opt->command_string ? "-c"
                                                  : "-");
    for (int arg = program_arg_idx; arg < argc; ++arg)
      opt->args[1 + arg - program_arg_idx] = argv[arg];
  }
  if (opt->output_file)
    opt->emit_only = true;
  if (opt->output_file || opt->run_aot)
    ny_select_host_native_backend_for_artifact(opt,
                                               opt->argv0 ? opt->argv0 : "ny");
  if (opt->run_aot)
    opt->run_jit = false;
  else if (opt->emit_only)
    opt->run_jit = false;
  else if (opt->input_file || opt->command_string || opt->mode == NY_MODE_REPL)
    opt->run_jit = true;
  if (opt->sanitize && *opt->sanitize && opt->run_jit && !opt->output_file) {
    opt->run_jit = false;
    opt->run_aot = true;
  }
  ny_sync_opt_profile_env(opt);
  if ((opt->heap_policy == NY_HEAP_RAII || opt->heap_policy == NY_HEAP_RC) &&
      opt->gc_flag_seen) {
    fprintf(stderr, "--heap=%s cannot be combined with -gc\n",
            ny_heap_policy_name(opt->heap_policy));
    fprintf(stderr,
            "choose exactly one heap policy: manual, raii, rc, or gc.\n");
    exit(1);
  }
  if (opt->safe_mode) {
    if (opt->gc_flag_seen || opt->heap_policy == NY_HEAP_GC) {
      fprintf(stderr, "--safe-mode cannot be combined with -gc or --heap=gc\n");
      fprintf(stderr,
              "safe mode uses the default type checks, ownership checks, and "
              "RC/RAII cleanup; choose --mode=fast or --heap=gc explicitly.\n");
      exit(1);
    }
    opt->ownership = true;
    opt->ownership_strict = true;
    opt->effect_require_known = true;
    opt->alias_require_known = true;
    opt->alias_require_no_escape = true;
    if (!opt->heap_policy_explicit)
      opt->heap_policy = NY_HEAP_RC;
    if (!opt->strict_types_explicit)
      opt->strict_types = true;
  }
  if (opt->ownership && opt->heap_policy == NY_HEAP_MANUAL)
    opt->heap_policy = NY_HEAP_RAII;
  if (opt->heap_policy == NY_HEAP_GC) {
    opt->enable_gc = true;
    opt->ownership = false;
    opt->ownership_strict = false;
  } else if (opt->heap_policy == NY_HEAP_RAII ||
             opt->heap_policy == NY_HEAP_RC) {
    opt->enable_gc = false;
    opt->ownership = true;
  } else {
    opt->enable_gc = false;
    if (!opt->ownership && !opt->borrow_check)
      opt->ownership_strict = false;
  }
  if (opt->ownership && opt->enable_gc) {
    fprintf(stderr,
            "--ownership/--raii cannot be combined with -gc or --heap=gc\n");
    fprintf(stderr,
            "choose --heap=raii, --heap=rc, or --heap=gc explicitly.\n");
    exit(1);
  }
  if (opt->mode == NY_MODE_REPL && !opt->opt_level_explicit) {
    opt->opt_level = 0;
    opt->opt_size = false;
    opt->opt_dce = 0;
    opt->opt_internalize = 0;
    opt->opt_loops = 0;
    opt->opt_autotune = 0;
  }
  ny_warn_inline_code_semicolon(opt);
  if (!opt->std_path)
    opt->std_path = ny_default_std_path();
  if (opt->print_flags) {
    ny_options_print_flags(opt, argc, argv, program_arg_idx);
    exit(0);
  }
}

void ny_options_parse(ny_options *opt, int argc, char **argv) {
  if (!opt || argc <= 0 || !argv)
    return;
  for (size_t i = 0;
       i < sizeof(ny_cli_provenance) / sizeof(ny_cli_provenance[0]); ++i) {
    free(ny_cli_provenance[i].items);
    ny_cli_provenance[i] = (ny_cli_arg_list_t){0};
  }
  free(ny_cli_aliases);
  ny_cli_aliases = NULL;
  ny_cli_alias_count = 0;
  int configured_argc = argc;
  char **configured_argv = ny_cli_merge_args(opt, argc, argv, &configured_argc);
  argc = configured_argc;
  argv = configured_argv;
  int response_argc = argc;
  char **response_argv = NULL;
  if (ny_options_has_response_input(argc, argv)) {
    response_argv =
        ny_options_expand_response_args(opt, argc, argv, &response_argc);
    argc = response_argc;
    argv = response_argv;
    size_t cli_prefix = 1 + ny_cli_provenance[0].len +
                        ny_cli_provenance[1].len + ny_cli_provenance[2].len;
    if (cli_prefix <= (size_t)response_argc) {
      free(ny_cli_provenance[3].items);
      ny_cli_provenance[3] = (ny_cli_arg_list_t){0};
      for (size_t arg = cli_prefix; arg < (size_t)response_argc; ++arg)
        ny_cli_arg_push(opt, &ny_cli_provenance[3], argv[arg]);
    }
  }
  if (!ny_options_needs_short_expansion(argc, argv)) {
    ny_options_parse_expanded(opt, argc, argv);
    free(response_argv);
    free(configured_argv);
    for (size_t i = 0;
         i < sizeof(ny_cli_provenance) / sizeof(ny_cli_provenance[0]); ++i) {
      free(ny_cli_provenance[i].items);
      ny_cli_provenance[i] = (ny_cli_arg_list_t){0};
    }
    free(ny_cli_aliases);
    ny_cli_aliases = NULL;
    ny_cli_alias_count = 0;
    return;
  }
  int expanded_argc = 0;
  char **expanded =
      ny_options_expand_short_args(opt, argc, argv, &expanded_argc);
  ny_options_parse_expanded(opt, expanded_argc, expanded);
  free(expanded);
  free(response_argv);
  free(configured_argv);
  for (size_t i = 0;
       i < sizeof(ny_cli_provenance) / sizeof(ny_cli_provenance[0]); ++i) {
    free(ny_cli_provenance[i].items);
    ny_cli_provenance[i] = (ny_cli_arg_list_t){0};
  }
  free(ny_cli_aliases);
  ny_cli_aliases = NULL;
  ny_cli_alias_count = 0;
}

void ny_options_free(ny_options *opt) {
  if (!opt)
    return;
  for (size_t i = 0; i < opt->link_dirs.len; i++)
    free(opt->link_dirs.data[i]);
  vec_free(&opt->link_dirs);
  for (size_t i = 0; i < opt->link_libs.len; i++)
    free(opt->link_libs.data[i]);
  vec_free(&opt->link_libs);
  free(opt->args);
  ny_option_owned_string_t *owned =
      (ny_option_owned_string_t *)opt->owned_option_values;
  while (owned) {
    ny_option_owned_string_t *next = owned->next;
    free(owned);
    owned = next;
  }
}
