#include "base/options.h"
#include "base/common.h"
#include "base/util.h"
#include <limits.h>
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

static bool ny_parse_nonneg_int(const char *s, int *out) {
  if (!s || !*s || !out)
    return false;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < 0 || v > INT_MAX)
    return false;
  *out = (int)v;
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
  NY_LOG_WARN("inline -c code uses ';', which starts a line comment in Nytrix; "
              "text after it is ignored. Use real newlines between statements, "
              "for example: ny -c $'use std.core\\nassert(true, \"ok\")'\n");
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
  opt->opt_level_explicit = true;
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

  ny_opt_profile_kind_t profile = ny_opt_profile_kind_from_name(opt->opt_profile);
  bool native_emit = !opt->run_jit && (opt->emit_only || opt->run_aot);
  bool standalone_emit_check =
      opt->emit_only && !opt->output_file && !opt->run_aot && !opt->run_jit;
  if (!opt->opt_level_explicit) {
    switch (profile) {
    case NY_OPT_PROFILE_PEAK:
      opt->opt_level = 3;
      break;
    case NY_OPT_PROFILE_SPEED:
      opt->opt_level = 2;
      break;
    case NY_OPT_PROFILE_BALANCED:
    case NY_OPT_PROFILE_SIZE:
      opt->opt_level = 2;
      break;
    case NY_OPT_PROFILE_COMPILE:
      opt->opt_level = 0;
      break;
    case NY_OPT_PROFILE_NONE:
      opt->opt_level = 0;
      break;
    case NY_OPT_PROFILE_CUSTOM:
    case NY_OPT_PROFILE_DEFAULT:
    default:
      if (standalone_emit_check)
        opt->opt_level = 0;
      else if (native_emit)
        opt->opt_level = 0;
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
    ny_setenv("NYTRIX_RAW_INT_EXPR_FAST_OPS", "add,sub,mul,div,mod", 0);
    ny_setenv("NYTRIX_MONO_LIST_ARGS", "1", 0);
    ny_setenv("NYTRIX_MONO_IMPERATIVE", "1", 0);
  }
  if (profile == NY_OPT_PROFILE_SPEED || profile == NY_OPT_PROFILE_PEAK) {
    ny_setenv("NYTRIX_SIMPLE_RAW_INT_CALL_FAST", "1", 0);
    ny_setenv("NYTRIX_PROVEN_INT_CAST_FAST", "1", 0);
    ny_setenv("NYTRIX_PROVEN_INT_BRANCH_FAST", "1", 0);
    ny_setenv("NYTRIX_PROVEN_INT_MOD_FAST", "1", 0);
  }
  if (profile == NY_OPT_PROFILE_PEAK) {
    ny_setenv("NYTRIX_MONO_IMPERATIVE", "1", 0);
    ny_setenv("NYTRIX_PROVEN_RAW_INT_EXPR_FAST", "1", 0);
    ny_setenv("NYTRIX_RAW_INT_EXPR_FAST", "1", 0);
    ny_setenv("NYTRIX_RAW_INT_SLOT_EXPR_FAST", "1", 0);
    ny_setenv("NYTRIX_RAW_INT_EXPR_FAST_OPS", "add,sub,mul,div,mod", 0);
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
  if (*argi + 1 < argc)
    return argv[++(*argi)];
  fprintf(stderr, "missing argument for %s\n", flag);
  ny_options_usage(argv0);
  exit(1);
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

static bool ny_options_apply_exec_option(ny_options *opt, const char *arg,
                                         int *argi, int argc, char **argv,
                                         const char *argv0) {
  const char *value = NULL;
  if ((value = ny_option_value_or_die(arg, "--eval", argi, argc, argv, argv0)) !=
      NULL) {
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
    fprintf(stderr, "unknown accel target for %s: %s\n", flag,
            target ? target : "(null)");
    ny_options_usage(argv0);
    exit(1);
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
    fprintf(stderr, "unknown accel object kind: %s\n", kind ? kind : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  const char *canon = ny_canonical_accel_object(kind);
  opt->accel_object = canon;
}

static void ny_set_gpu_target_compat_or_die(ny_options *opt, const char *target,
                                            const char *argv0) {
  const char *backend = ny_gpu_backend_from_target(target);
  const char *accel = ny_canonical_accel_target(target);
  if (!backend && !accel) {
    fprintf(stderr, "unknown gpu target: %s\n", target ? target : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  if (backend && ny_is_gpu_backend(backend))
    opt->gpu_backend = backend;
  if (accel)
    opt->accel_target = accel;
}

static void ny_set_default_output(ny_options *opt) {
#ifdef _WIN32
  opt->output_file = "a.exe";
#else
  opt->output_file = "a.out";
#endif
}

static void ny_set_stop_after_or_die(ny_options *opt, const char *value,
                                     const char *argv0) {
  ny_stop_after_stage_t stage = NY_STOP_AFTER_NONE;
  if (!ny_parse_stop_after_stage(value, &stage)) {
    fprintf(stderr,
            "invalid stop stage: %s (expected parse|hm|trait|flow|abi|opt)\n",
            value ? value : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  opt->stop_after = stage;
  opt->stop_after_raw = value;
  opt->emit_only = true;
  opt->run_jit = false;
}

static void ny_set_type_solver_or_die(ny_options *opt, const char *value,
                                      const char *argv0) {
  ny_type_solver_t solver = NY_TYPE_SOLVER_AUTO;
  if (!ny_parse_type_solver(value, &solver)) {
    fprintf(stderr, "invalid type solver: %s (expected auto|hm|z3)\n",
            value ? value : "(null)");
    ny_options_usage(argv0);
    exit(1);
  }
  opt->type_solver = solver;
  opt->type_solver_raw = value;
}

static bool ny_options_apply_common_codegen_option(ny_options *opt,
                                                   const char *a, int *i,
                                                   int argc, char **argv,
                                                   const char *argv0) {
  const char *value = NULL;
  if (strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) {
    if (*i + 1 < argc && argv[*i + 1][0] != '-')
      opt->output_file = argv[++(*i)];
    else
      ny_set_default_output(opt);
    return true;
  }
  if (strncmp(a, "--output=", 9) == 0) {
    opt->output_file = a + 9;
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
  if ((value = ny_option_value_or_die(a, "--emit-artifact", i, argc, argv,
                                      argv0)) != NULL) {
    opt->emit_artifact_path = value;
    return true;
  }
  if (strcmp(a, "--collect-errors") == 0) {
    opt->collect_errors = true;
    return true;
  }
  if (strcmp(a, "--emit-shapes") == 0) {
    opt->emit_shapes = true;
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

static const char *ny_gpu_backend_option_value(const char *backend) {
  return backend && strcmp(backend, "off") == 0 ? "none" : backend;
}

static void ny_set_option_choice_or_die(const char **slot, const char *value,
                                        ny_option_choice_pred_t valid,
                                        const char *label,
                                        const char *argv0) {
  if (valid(value)) {
    *slot = value;
    return;
  }
  fprintf(stderr, "unknown %s: %s\n", label, value ? value : "(null)");
  ny_options_usage(argv0);
  exit(1);
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
      {"-strip", offsetof(ny_options, strip_override), 1, NY_OPT_TOGGLE_INT},
      {"--strip", offsetof(ny_options, strip_override), 1, NY_OPT_TOGGLE_INT},
      {"-no-strip", offsetof(ny_options, strip_override), 0, NY_OPT_TOGGLE_INT},
      {"--no-strip", offsetof(ny_options, strip_override), 0,
       NY_OPT_TOGGLE_INT},
      {"-time", offsetof(ny_options, do_timing), 1, NY_OPT_TOGGLE_BOOL},
      {"-dump-ast", offsetof(ny_options, dump_ast), 1, NY_OPT_TOGGLE_BOOL},
      {"--extract-json", offsetof(ny_options, extract_json), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--collect-errors", offsetof(ny_options, collect_errors), 1,
       NY_OPT_TOGGLE_BOOL},
      {"--emit-shapes", offsetof(ny_options, emit_shapes), 1,
       NY_OPT_TOGGLE_BOOL},
      {"-dump-llvm", offsetof(ny_options, dump_llvm), 1, NY_OPT_TOGGLE_BOOL},
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
      {"-dump-tokens", offsetof(ny_options, dump_tokens), 1,
       NY_OPT_TOGGLE_BOOL},
      {"-dump-docs", offsetof(ny_options, dump_docs), 1, NY_OPT_TOGGLE_BOOL},
      {"-dump-funcs", offsetof(ny_options, dump_funcs), 1, NY_OPT_TOGGLE_BOOL},
      {"-dump-symbols", offsetof(ny_options, dump_symbols), 1,
       NY_OPT_TOGGLE_BOOL},
      {"-dump-stats", offsetof(ny_options, dump_stats), 1, NY_OPT_TOGGLE_BOOL},
      {"--dump-on-error", offsetof(ny_options, dump_on_error), 1,
       NY_OPT_TOGGLE_BOOL},
      {"-trace", offsetof(ny_options, trace_exec), 1, NY_OPT_TOGGLE_BOOL},
      {"-verify", offsetof(ny_options, verify_module), 1, NY_OPT_TOGGLE_BOOL},
      {"-g", offsetof(ny_options, debug_symbols), 1, NY_OPT_TOGGLE_BOOL},
      {"--plain-repl", offsetof(ny_options, repl_plain), 1, NY_OPT_TOGGLE_BOOL},
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
    return true;
  }
  return false;
}

void ny_options_init(ny_options *opt) {
  memset(opt, 0, sizeof(ny_options));
  opt->mode = NY_MODE_RUN;
  opt->opt_level = 0;
  opt->opt_profile = NULL;
  opt->opt_dce = 1;
  opt->strip_override = -1;
  opt->color_mode = -1;
  opt->gpu_async = -1;
  opt->gpu_fast_math = -1;
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
}

typedef struct ny_usage_entry_t {
  const char *color;
  const char *flag;
  const char *desc;
} ny_usage_entry_t;

static void ny_usage_section(const char *name) {
  fprintf(stderr, "%s%s:%s\n", clr(NY_CLR_BOLD), name, clr(NY_CLR_RESET));
}

static void ny_usage_item(const char *color, const char *flag,
                          const char *desc) {
  size_t width = 31;
  if (strlen(flag) <= width) {
    fprintf(stderr, "  %s%-31s%s %s\n", clr(color), flag, clr(NY_CLR_RESET),
            desc);
  } else {
    fprintf(stderr, "  %s%s%s\n", clr(color), flag, clr(NY_CLR_RESET));
    fprintf(stderr, "  %-31s %s\n", "", desc);
  }
}

static void ny_usage_items(const ny_usage_entry_t *items) {
  for (const ny_usage_entry_t *it = items; it && it->flag; ++it)
    ny_usage_item(it->color, it->flag, it->desc ? it->desc : "");
  fputc('\n', stderr);
}

static void ny_usage_example(int width, const char *cmd, const char *desc) {
  fprintf(stderr, "  %s%-*s%s # %s\n", clr(NY_CLR_CYAN), width, cmd,
          clr(NY_CLR_RESET), desc);
}

static void ny_options_usage_impl(const char *prog, bool show_env) {
  fprintf(
      stderr,
      "\n\033[1;36mNytrix Compiler\033[0m - Small core with stdlib in .ny\n\n");
  ny_usage_section("GENERAL");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_MAGENTA, "--color=WHEN", "WHEN: auto | always | never"},
      {NY_CLR_MAGENTA, "-safe-mode",
       "Enable strict safety/effect/alias checks"},
      {NY_CLR_MAGENTA, "--mode=MODE", "Runtime preset: safe | fast | bare"},
      {NY_CLR_MAGENTA, "--strict",
       "Default type checks plus ownership/borrow diagnostics"},
      {NY_CLR_MAGENTA, "--strict-types",
       "Reject dynamic type cliffs at compile time"},
      {NY_CLR_MAGENTA, "--no-strict-types",
       "Allow legacy dynamic type cliffs"},
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
  ny_usage_section("EXPERT MEMORY");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_MAGENTA, "--heap=MODE", "Heap policy: manual | raii | rc | gc"},
      {NY_CLR_MAGENTA, "-gc / -no-gc", "Enable/disable GC nursery"},
      {NY_CLR_MAGENTA, "--rc-gc", "Alias for --heap=rc"},
      {NY_CLR_MAGENTA, "--ownership", "Alias for --heap=raii"},
      {NY_CLR_MAGENTA, "--ownership-strict", "Strict ownership diagnostics"},
      {NY_CLR_MAGENTA, "--borrow-check", "Borrow/ownership diagnostics without RAII cleanup"},
      {NY_CLR_MAGENTA, "--raii", "Alias for --ownership"},
      {NY_CLR_MAGENTA, "--owbership", "Typo-compatible alias for --ownership"},
      {NY_CLR_MAGENTA, "Note",
       "Typed contracts are non-null by default (nil requires ?T or *T)"},
      {NULL, NULL, NULL}});
  ny_usage_section("OPTIMIZATION");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "-O1/-O2/-O3",
       "Optimization level (default: -O0 for fast development builds)"},
      {NY_CLR_GREEN, "--profile=MODE",
       "Optimization profile: speed | balanced | compile | size | none | "
       "peak"},
      {NY_CLR_GREEN, "-passes=PIPE",
       "Custom LLVM pass pipeline (e.g., 'default<O2>')"},
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
  ny_usage_section("CACHE");
  ny_usage_items(
      (const ny_usage_entry_t[]){{NY_CLR_GREEN, "--clean-cache",
                                  "Remove Nytrix JIT/std/AOT cache artifacts"},
                                 {NULL, NULL, NULL}});
  ny_usage_section("EXECUTION");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "-run", "Build a native executable and run it (AOT auto)"},
      {NY_CLR_GREEN, "--run=MODE", "MODE: auto | aot | jit"},
      {NY_CLR_GREEN, "--jit", "Run through MCJIT instead of native AOT"},
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
      {NULL, NULL, NULL}});
  ny_usage_section("STDLIB");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "-std [none|minimal|full]",
       "Stdlib inclusion mode (default: minimal)"},
      {NY_CLR_GREEN, "-timeout <seconds>", "Set a hard execution limit"},
      {NY_CLR_GREEN, "-no-std", "Disable stdlib loading"},
      {NY_CLR_GREEN, "--std-path=<path>", "Use custom std.ny"},
      {NY_CLR_GREEN, "--full-mod", "Alias for -std full"},
      {NY_CLR_GREEN, "--bundle-std=P", "Generate std.ny at P"},
      {NY_CLR_GREEN, "--bundle-symbols=P", "Generate std_symbols.h at P"},
      {NULL, NULL, NULL}});
  ny_usage_section("LINKING");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "-L<dir>", "Add a linker search path (repeatable)"},
      {NY_CLR_GREEN, "-l<lib>", "Link against library (also accepts -l <lib>)"},
      {NULL, NULL, NULL}});
  ny_usage_section("REPL");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_GREEN, "-i, --interactive",
       "Interactive REPL with line editing"},
      {NY_CLR_GREEN, "--repl", "Read source from stdin (one-shot)"},
      {NULL, NULL, NULL}});
  ny_usage_section("DEBUGGING");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_BLUE, "-v, --verbose", "Show high-level phases and decisions"},
      {NY_CLR_BLUE, "--debug",
       "Max verbosity, keep symbols, keep binary unstripped"},
      {NY_CLR_BLUE, "-v/-vv/-vvv",
       "Verbosity tiers: steps | commands | internals (or --verbose=N)"},
      {NY_CLR_BLUE, "-time", "Show timing for each phase"},
      {NY_CLR_BLUE, "-dump-ast", "Dump parsed AST"},
      {NY_CLR_BLUE, "--expand",
       "Print expansion/desugar report and stop after parse"},
      {NY_CLR_BLUE, "--expand-only=NAME",
       "Filter expansion report to a template/function/layout"},
      {NY_CLR_BLUE, "--expand-json",
       "Include raw lowered AST JSON after the compact graph"},
      {NY_CLR_BLUE, "--explain-specialization=NAME",
       "Explain static specialization candidates"},
      {NY_CLR_BLUE, "--stop-after=STAGE",
       "Stop after parse|hm|trait|flow|abi|opt"},
      {NY_CLR_BLUE, "--type-solver=MODE", "Type solver: auto | hm | z3"},
      {NY_CLR_BLUE, "--emit-artifact=PATH",
       "Write the selected stage artifact JSON"},
      {NY_CLR_BLUE, "--collect-errors",
       "Emit stage-tagged errors.v1.json on failures"},
      {NY_CLR_BLUE, "--emit-shapes",
       "Include type/function/layout/operator shapes in artifacts"},
      {NY_CLR_BLUE, "--meta-trace",
       "Include expression/call trace in expansion report"},
      {NY_CLR_BLUE, "-dump-llvm", "Print LLVM IR to stdout"},
      {NY_CLR_BLUE, "-dump-tokens", "Dump lexer_t tokens"},
      {NY_CLR_BLUE, "--extract-code",
       "Extract fenced Nytrix code blocks from Markdown and exit"},
      {NY_CLR_BLUE, "--extract-json",
       "Emit extracted code block metadata as JSON"},
      {NY_CLR_BLUE, "--extract-at=L[:C]",
       "Select the fenced code block containing a source line"},
      {NY_CLR_BLUE, "--extract-lang=LIST",
       "Fence languages for --extract-code (default ny,nytrix)"},
      {NY_CLR_BLUE, "-dump-docs", "Extract and print function docstrings"},
      {NY_CLR_BLUE, "-dump-funcs", "List all compiled functions"},
      {NY_CLR_BLUE, "-dump-symbols", "Show runtime symbol table"},
      {NY_CLR_BLUE, "-dump-stats", "Print compilation statistics"},
      {NY_CLR_BLUE, "--warn=LEVEL", "Warning level: none | useful | all"},
      {NY_CLR_BLUE, "--diag-compact", "Compact one-line diagnostics"},
      {NY_CLR_BLUE, "--diag-rich", "Rich diagnostics with snippets (default)"},
      {NY_CLR_BLUE, "-prof, --prof",
       "Enable compiler/runtime profiling outputs (timings + stats)"},
      {NY_CLR_BLUE, "-verify", "Verify LLVM module"},
      {NY_CLR_BLUE, "-g", "Emit debug symbols"},
      {NY_CLR_BLUE, "--emit-ir=<path>", "Emit LLVM IR to file"},
      {NY_CLR_BLUE, "--emit-bc=<path>", "Emit LLVM Bitcode to file"},
      {NY_CLR_BLUE, "--entry-name=<n>", "Rename entry point (default: main)"},
      {NY_CLR_BLUE, "--ir-full", "Keep std/lib definitions in IR output"},
      {NY_CLR_BLUE, "--ir-no-std",
       "Omit std/lib definitions in IR output (default)"},
      {NY_CLR_BLUE, "--emit-asm=<path>", "Emit assembly to file"},
      {NY_CLR_BLUE, "--dump-on-error",
       "Write build/debug/last_source.ny and last_ir.ll on errors"},
      {NY_CLR_BLUE, "--dump-diagnose",
       "Emit diagnostics bundle (source + pre/post IR + asm + bc + stats)"},
      {NY_CLR_BLUE, "--dump-dir=PATH",
       "Diagnostics bundle directory (default: build/debug)"},
      {NY_CLR_BLUE, "--dump-scope=S", "Dump scope: program | lib | both"},
      {NY_CLR_BLUE, "-trace", "Enable execution tracing"},
      {NULL, NULL, NULL}});
  ny_usage_section("INFO");
  ny_usage_items((const ny_usage_entry_t[]){
      {NY_CLR_BLUE, "-h, -help, --help", "Show this help message"},
      {NY_CLR_BLUE, "--help env", "Show supported environment variables"},
      {NY_CLR_BLUE, "-version", "Show version info"},
      {NULL, NULL, NULL}});
  if (show_env) {
    ny_usage_section("ENVIRONMENT");
    fprintf(stderr, "  Policy env overrides are disabled by default.\n");
    fprintf(stderr,
            "  Use CLI flags for GPU/parallel/effect/debug behavior.\n");
    ny_usage_items((const ny_usage_entry_t[]){
        {NY_CLR_YELLOW, "NYTRIX_STD_PATH", "Override std.ny path"},
        {NY_CLR_YELLOW, "NYTRIX_STD_PREBUILT", "Use prebuilt stdlib source"},
        {NY_CLR_YELLOW, "NYTRIX_BUILD_STD_PATH",
         "Override generated std.ny path"},
        {NY_CLR_YELLOW, "NYTRIX_HOST_TRIPLE / NYTRIX_ARM_FLOAT_ABI",
         "Host target and ARM ABI tuning"},
        {NY_CLR_YELLOW, "NYTRIX_HOST_CFLAGS / NYTRIX_HOST_LDFLAGS",
         "Host build tuning"},
        {NY_CLR_YELLOW, "NYTRIX_JIT_CACHE_FORMAT",
         "JIT cache format: bc (default) | ir"},
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
        {NULL, NULL, NULL}});
  }
  ny_usage_section("EXAMPLES");
  char cmd[512];
  const int example_width = 67;
  snprintf(cmd, sizeof(cmd), "%s etc/tests/rt/control.ny", prog);
  ny_usage_example(example_width, cmd, "compile and run via JIT");
  snprintf(cmd, sizeof(cmd), "%s -O2 -run etc/tests/rt/control.ny", prog);
  ny_usage_example(example_width, cmd, "build native ELF and run");
  snprintf(cmd, sizeof(cmd), "%s -O2 -o app etc/tests/rt/control.ny", prog);
  ny_usage_example(example_width, cmd, "emit native executable");
  snprintf(cmd, sizeof(cmd), "%s -v -time -verify etc/tests/rt/control.ny", prog);
  ny_usage_example(example_width, cmd, "debug compilation");
  snprintf(cmd, sizeof(cmd), "%s", prog);
  ny_usage_example(example_width, cmd, "interactive REPL");
  snprintf(cmd, sizeof(cmd), "%s -c 'print(\"hello\")'", prog);
  ny_usage_example(example_width, cmd, "run inline");
  snprintf(cmd, sizeof(cmd), "%s -ic 'a=1337'", prog);
  ny_usage_example(example_width, cmd, "run inline, then REPL");
}

void ny_options_usage(const char *prog) { ny_options_usage_impl(prog, false); }

void ny_options_usage_env(const char *prog) {
  ny_options_usage_impl(prog, true);
}

static const char *ny_default_std_path(void) {
  const char *env_std = getenv("NYTRIX_STD_PATH");
  if (env_std)
    return env_std;
  if (ny_access("./build/std.ny", R_OK) == 0)
    return "./build/std.ny";

  char *exe_dir = ny_get_executable_dir();
  if (exe_dir && *exe_dir) {
    static char exe_stdp[4096];
    ny_join_path(exe_stdp, sizeof(exe_stdp), exe_dir, "std.ny");
    if (ny_access(exe_stdp, R_OK) == 0)
      return exe_stdp;
  }
#ifdef _WIN32
  const char *pd = getenv("PROGRAMDATA");
  if (pd && *pd) {
    static char stdp[4096];
    char tmp[4096];
    ny_join_path(tmp, sizeof(tmp), pd, "nytrix");
    ny_join_path(stdp, sizeof(stdp), tmp, "std.ny");
    if (ny_access(stdp, R_OK) == 0)
      return stdp;
  }
  return "C:/ProgramData/nytrix/std.ny";
#else
#ifdef NYTRIX_STD_PATH
  if (ny_access(NYTRIX_STD_PATH, R_OK) == 0)
    return NYTRIX_STD_PATH;
#endif
  static const char *paths[] = {
      "/usr/share/nytrix/std.ny",
      "/usr/local/share/nytrix/std.ny",
      "/opt/homebrew/share/nytrix/std.ny",
      "/opt/nytrix/share/std.ny",
  };
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    if (ny_access(paths[i], R_OK) == 0)
      return paths[i];
  }
  return paths[0];
#endif
}

void ny_options_parse(ny_options *opt, int argc, char **argv) {
  if (argc > 0)
    opt->argv0 = argv[0];
  if (argc < 2)
    return;
  int i = 1;
  while (i < argc) {
    const char *a = argv[i];
    const char *value = NULL;
    if (strcmp(a, "--") == 0) {
      i++;
      if (!opt->file_arg_idx)
        opt->file_arg_idx = i;
      break;
    }
    if (a[0] == '-' && !opt->input_file && !opt->command_string) {
      if (strcmp(a, "-safe-mode") == 0 || strcmp(a, "--safe-mode") == 0 ||
          strcmp(a, "--safe") == 0) {
        opt->safe_mode = true;
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
        fprintf(stderr, "--profile now requires a mode: "
                        "--profile=speed|balanced|compile|size|none|peak\n"
                        "Use --gprof for gprof instrumentation.\n");
        ny_options_usage(argv[0]);
        exit(1);
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
      } else if ((value = ny_option_value_or_die(a, "--host-triple", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->host_triple = value;
      } else if ((value = ny_option_value_or_die(a, "--arm-float-abi", &i, argc,
                                                 argv, argv[0])) != NULL) {
        if (!ny_is_arm_float_abi(value)) {
          fprintf(stderr,
                  "invalid arm float ABI: %s (expected soft|softfp|hard)\n",
                  value);
          ny_options_usage(argv[0]);
          exit(1);
        }
        opt->arm_float_abi = value;
      } else if (strcmp(a, "-O") == 0 || strcmp(a, "-O2") == 0) {
        ny_set_opt_level(opt, 2);
      } else if ((value = ny_option_value_or_die(a, "--profile", &i, argc, argv,
                                                 argv[0])) != NULL) {
        if (!ny_is_opt_profile_name(value)) {
          fprintf(stderr, "invalid optimization profile: %s\n", value);
          ny_options_usage(argv[0]);
          exit(1);
        }
        opt->opt_profile = value;
      } else if (strcmp(a, "-O1") == 0) {
        ny_set_opt_level(opt, 1);
      } else if (strcmp(a, "-O0") == 0) {
        ny_set_opt_level(opt, 0);
      } else if (strcmp(a, "-O3") == 0) {
        ny_set_opt_level(opt, 3);
      } else if (strcmp(a, "--fast") == 0) {
        if (!opt->opt_level)
          opt->opt_level = 2;
        opt->verify_module = 0;
        opt->strip_override = 1;
      } else if (strncmp(a, "-passes=", 8) == 0)
        opt->opt_pipeline = a + 8;
      else if ((value = ny_option_value_or_die(a, "--gpu", &i, argc, argv,
                                               argv[0])) != NULL) {
        ny_set_option_choice_or_die(&opt->gpu_mode, value, ny_is_gpu_mode,
                                    "gpu mode", argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--gpu-backend", &i, argc,
                                                 argv, argv[0])) != NULL) {
        value = ny_gpu_backend_option_value(value);
        ny_set_option_choice_or_die(&opt->gpu_backend, value, ny_is_gpu_backend,
                                    "gpu backend", argv[0]);
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
                                    ny_is_gpu_offload_mode,
                                    "gpu offload mode", argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--gpu-min-work", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->gpu_min_work =
            ny_parse_nonneg_int_or_die(value, "gpu min work", argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--parallel", &i, argc,
                                                 argv, argv[0])) != NULL) {
        ny_set_option_choice_or_die(&opt->parallel_mode, value,
                                    ny_is_parallel_mode, "parallel mode",
                                    argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--threads", &i, argc, argv,
                                                 argv[0])) != NULL) {
        opt->thread_count =
            ny_parse_nonneg_int_or_die(value, "thread count", argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--parallel-min-work", &i,
                                                 argc, argv, argv[0])) !=
                 NULL) {
        opt->parallel_min_work =
            ny_parse_nonneg_int_or_die(value, "parallel min work", argv[0]);
      } else if (strncmp(a, "--bundle-std=", 13) == 0) {
        opt->bundle_std_path = a + 13;
        opt->mode = NY_MODE_BUNDLE;
      } else if (strncmp(a, "--bundle-symbols=", 17) == 0) {
        opt->bundle_symbols_path = a + 17;
        opt->mode = NY_MODE_BUNDLE;
      } else if (strcmp(a, "--run") == 0 && i + 1 < argc &&
                 (strcmp(argv[i + 1], "auto") == 0 ||
                  strcmp(argv[i + 1], "aot") == 0 ||
                  strcmp(argv[i + 1], "jit") == 0)) {
        const char *mode = argv[++i];
        opt->run_aot = strcmp(mode, "jit") != 0;
        opt->run_jit = strcmp(mode, "jit") == 0;
        opt->emit_only = false;
      } else if (strcmp(a, "-run") == 0 || strcmp(a, "--run") == 0 ||
                 strcmp(a, "--run-aot") == 0 ||
                 strcmp(a, "--run-native") == 0 ||
                 strcmp(a, "--run=auto") == 0 || strcmp(a, "--run=aot") == 0) {
        opt->run_aot = true;
        opt->run_jit = false;
        opt->emit_only = false;
      } else if (strcmp(a, "--jit") == 0 || strcmp(a, "-jit") == 0 ||
                 strcmp(a, "--run-jit") == 0 || strcmp(a, "--run=jit") == 0) {
        opt->run_jit = true;
        opt->run_aot = false;
      } else if (strncmp(a, "--run=", 6) == 0) {
        fprintf(stderr, "invalid run mode: %s (expected auto|aot|jit)\n",
                a + 6);
        ny_options_usage(argv[0]);
        exit(1);
      } else if (strcmp(a, "-emit-only") == 0) {
        opt->emit_only = true;
        opt->run_aot = false;
      } else if (ny_options_apply_exec_option(opt, a, &i, argc, argv, argv[0])) {
      } else if (strcmp(a, "--debug") == 0) {
        opt->verbose = 3;
        opt->debug_symbols = true;
        opt->strip_override = 0;
        opt->dump_on_error = true;
        debug_enabled = 1;
      } else if (strcmp(a, "-verbose") == 0 || strcmp(a, "--verbose") == 0) {
        if (opt->verbose < 1)
          opt->verbose = 1;
      } else if (strncmp(a, "--verbose=", 10) == 0) {
        int lvl = atoi(a + 10);
        if (lvl < 0)
          lvl = 0;
        if (lvl > 3)
          lvl = 3;
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
          if (lvl > 3)
            lvl = 3;
          if (opt->verbose < lvl)
            opt->verbose = lvl;
        } else {
          fprintf(stderr, "unknown option: %s\n", a);
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if (strcmp(a, "-prof") == 0 || strcmp(a, "--prof") == 0) {
        opt->profiler_mode = true;
        opt->do_timing = true;
        opt->dump_stats = true;
      } else if (strcmp(a, "--expand") == 0)
        opt->expand = true;
      else if ((value = ny_option_value_or_die(a, "--expand-only", &i, argc,
                                               argv, argv[0])) != NULL) {
        opt->expand = true;
        opt->expand_only = value;
      } else if (strcmp(a, "--expand-json") == 0) {
        opt->expand = true;
        opt->expand_json = true;
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
                  "invalid warning level: %s (expected none|useful|all)\n",
                  value);
          ny_options_usage(argv[0]);
          exit(1);
        }
        opt->warn_level = lvl;
      } else if ((value = ny_option_value_or_die(a, "--dump-scope", &i, argc,
                                                 argv, argv[0])) != NULL) {
        ny_dump_scope_t scope = NY_DUMP_SCOPE_PROGRAM;
        if (!ny_parse_dump_scope(value, &scope)) {
          fprintf(stderr,
                  "invalid dump scope: %s (expected program|lib|both)\n",
                  value);
          ny_options_usage(argv[0]);
          exit(1);
        }
        opt->dump_scope = scope;
      } else if (strncmp(a, "--dump-dir=", 11) == 0) {
        opt->dump_dir = a + 11;
      } else if (strcmp(a, "--dump-diagnose") == 0) {
        opt->dump_diagnose = true;
        opt->dump_on_error = true;
      } else if (strncmp(a, "--entry-name=", 13) == 0)
        opt->entry_name = a + 13;
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
      } else if (strcmp(a, "--ownership") == 0 ||
                 strcmp(a, "--owbership") == 0 || strcmp(a, "--raii") == 0) {
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
        opt->borrow_check = true;
        opt->ownership_strict = true;
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
      } else if (strcmp(a, "-timeout") == 0 || strcmp(a, "--timeout") == 0) {
        if (i + 1 < argc) {
          opt->timeout = atof(argv[++i]);
        } else {
          fprintf(stderr, "missing argument for %s\n", a);
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if (strcmp(a, "-std") == 0) {
        if (i + 1 < argc) {
          opt->std_mode = ny_parse_std_mode_or_die(argv[++i], argv[0]);
          opt->std_mode_explicit = true;
        } else {
          fprintf(stderr, "missing argument for -std\n");
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if (strcmp(a, "--full-mod") == 0) {
        opt->std_mode = STD_MODE_FULL;
        opt->std_mode_explicit = true;
      } else if (strcmp(a, "-no-std") == 0) {
        opt->no_std = true;
        opt->std_mode_explicit = true;
      } else if (strncmp(a, "--std-path=", 11) == 0)
        opt->std_path = a + 11;
      else if (strncmp(a, "--std-bc=", 9) == 0) {
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
      } else if (strncmp(a, "--color=", 8) == 0) {
        const char *mode = a + 8;
        if (strcmp(mode, "auto") == 0)
          opt->color_mode = -1;
        else if (strcmp(mode, "always") == 0)
          opt->color_mode = 1;
        else if (strcmp(mode, "never") == 0)
          opt->color_mode = 0;
      } else if (strcmp(a, "--help-env") == 0 || strcmp(a, "--env-help") == 0) {
        opt->mode = NY_MODE_HELP;
        opt->help_env = true;
      } else if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0 ||
                 strcmp(a, "--help") == 0) {
        opt->mode = NY_MODE_HELP;
        if (i + 1 < argc && strcmp(argv[i + 1], "env") == 0) {
          opt->help_env = true;
          i++;
        }
      } else if (strcmp(a, "-version") == 0 || strcmp(a, "-verison") == 0 ||
                 strcmp(a, "--version") == 0)
        opt->mode = NY_MODE_VERSION;
      else {
        ny_options_usage(argv[0]);
        exit(1);
      }
    } else {
      bool handled_post_command_option = false;
      if (opt->command_string && !opt->input_file && a[0] == '-') {
        handled_post_command_option = ny_options_apply_common_codegen_option(
            opt, a, &i, argc, argv, argv[0]);
      }
      if (handled_post_command_option) {
        i++;
        continue;
      }
      if (!opt->input_file && !opt->command_string) {
        if (opt->pending_command_string) {
          opt->command_string = a;
          opt->pending_command_string = false;
        } else {
          opt->input_file = a;
          opt->file_arg_idx = i;
        }
      } else {
        if (!opt->file_arg_idx)
          opt->file_arg_idx = i;
        break;
      }
    }
    i++;
  }
  if (opt->output_file)
    opt->emit_only = true;
  if (opt->run_aot)
    opt->run_jit = false;
  else if (opt->emit_only)
    opt->run_jit = false;
  else if (opt->input_file || opt->command_string || opt->mode == NY_MODE_REPL)
    opt->run_jit = true;
  ny_sync_opt_profile_env(opt);
  if (opt->run_jit && opt->mode != NY_MODE_REPL && !opt->opt_level_explicit &&
      ny_opt_profile_kind_from_name(opt->opt_profile) == NY_OPT_PROFILE_DEFAULT) {
    opt->opt_level = 0;
  }
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
      fprintf(stderr,
              "--safe-mode cannot be combined with -gc or --heap=gc\n");
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
    opt->opt_dce = 0;
    opt->opt_internalize = 0;
    opt->opt_loops = 0;
    opt->opt_autotune = 0;
  }
  ny_warn_inline_code_semicolon(opt);
  if (!opt->std_path)
    opt->std_path = ny_default_std_path();
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
}
