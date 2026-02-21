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

// Global State
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
  strcpy(buf, prefix);
  strcat(buf, arg);
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

static bool ny_is_gpu_offload_mode(const char *mode) {
  return mode && (strcmp(mode, "off") == 0 || strcmp(mode, "auto") == 0 ||
                  strcmp(mode, "on") == 0 || strcmp(mode, "force") == 0);
}

static bool ny_is_parallel_mode(const char *mode) {
  return mode && (strcmp(mode, "off") == 0 || strcmp(mode, "auto") == 0 ||
                  strcmp(mode, "threads") == 0);
}

void ny_options_init(ny_options *opt) {
  memset(opt, 0, sizeof(ny_options));
  opt->mode = NY_MODE_RUN;
  opt->opt_level = 2;
  opt->strip_override = -1;
  opt->color_mode = -1;
  opt->gpu_async = -1;
  opt->gpu_fast_math = -1;
  opt->gprof = -1;
  opt->std_builtin_ops = -1;
  opt->debug_locals = -1;
  opt->dwarf_version = 0;
  opt->dwarf_split_inlining = -1;
  opt->dwarf_profile_info = -1;
  opt->std_mode = STD_MODE_MINIMAL;
  /* Static guarantees default on; explicit flags can relax these. */
  opt->effect_require_known = true;
  opt->alias_require_known = true;
  /* Keep IR dumps focused on user code by default. */
  opt->ir_include_std = false;
  /* Optimization controls default to -1 (use smart defaults/env). */
  opt->opt_dce = -1;
  opt->opt_internalize = -1;
  opt->opt_loops = -1;
  opt->opt_autotune = -1;
}

static void ny_options_usage_impl(const char *prog, bool show_env) {
  fprintf(
      stderr,
      "\n\033[1;36mNytrix Compiler\033[0m - Small core with stdlib in .ny\n\n");
  fprintf(stderr, "\033[1mGENERAL:\033[0m\n");
  fprintf(stderr,
          "  \033[35m--color=WHEN\033[0m       WHEN: auto | always | never\n");
  fprintf(stderr, "  \033[35m-safe-mode\033[0m         Enable strict "
                  "safety/effect/alias checks\n\n");
  fprintf(stderr, "  \033[35m--effect-require-known\033[0m Enforce known "
                  "effects for user functions (default on)\n");
  fprintf(stderr,
          "  \033[35m--no-effect-require-known\033[0m Allow unknown effects\n");
  fprintf(stderr, "  \033[35m--alias-require-known\033[0m Enforce known "
                  "alias/escape facts (default on)\n");
  fprintf(stderr, "  \033[35m--no-alias-require-known\033[0m Allow unknown "
                  "alias/escape facts\n");
  fprintf(stderr, "  \033[35m--alias-require-no-escape\033[0m Enforce no "
                  "arg-escape / return-alias\n\n");
  fprintf(stderr, "  \033[35mNote:\033[0m              Typed contracts are "
                  "non-null by default (nil requires ?T or *T)\n\n");
  fprintf(stderr, "\033[1mOPTIMIZATION:\033[0m\n");
  fprintf(stderr, "  \033[32m-O1/-O2/-O3\033[0m        Optimization level "
                  "(default: -O2)\n");
  fprintf(stderr, "  \033[32m-passes=PIPE\033[0m       Custom LLVM pass "
                  "pipeline (e.g., 'default<O2>')\n\n");
  fprintf(stderr, "\033[1mPARALLELISM:\033[0m\n");
  fprintf(
      stderr,
      "  \033[32m--gpu=MODE\033[0m         GPU mode: off | auto | opencl\n");
  fprintf(stderr, "  \033[32m--gpu-backend=B\033[0m    GPU backend: none | "
                  "auto | opencl | cuda | hip | metal\n");
  fprintf(stderr, "  \033[32m--gpu-offload=M\033[0m    Offload policy: off | "
                  "auto | on | force\n");
  fprintf(stderr, "  \033[32m--gpu-min-work=N\033[0m   Minimum work-items "
                  "before GPU offload\n");
  fprintf(stderr,
          "  \033[32m--gpu-async\033[0m        Prefer async GPU dispatch\n");
  fprintf(stderr, "  \033[32m--gpu-fast-math\033[0m    Allow relaxed GPU math "
                  "optimizations\n");
  fprintf(stderr, "  \033[32m--accel-target=T\033[0m   Device target: auto | "
                  "none | nvptx | amdgpu | spirv | hsaco\n");
  fprintf(stderr, "  \033[32m--accel-object=K\033[0m   Device artifact: auto | "
                  "none | ptx | o | spv | hsaco\n");
  fprintf(stderr, "  \033[32m--gpu-target=T\033[0m     Compatibility alias "
                  "(maps to backend/accel target)\n");
  fprintf(stderr, "  \033[32m--parallel=MODE\033[0m    Parallel mode: off | "
                  "auto | threads\n");
  fprintf(stderr, "  \033[32m--threads=N\033[0m        Thread budget for "
                  "parallel work\n\n");
  fprintf(stderr, "  \033[32m--parallel-min-work=N\033[0m Minimum work-items "
                  "before threading\n\n");
  fprintf(stderr, "\033[1mBUILD TUNING:\033[0m\n");
  fprintf(stderr, "  \033[32m--host-triple=T\033[0m     Host target triple for "
                  "native build\n");
  fprintf(
      stderr,
      "  \033[32m--arm-float-abi=A\033[0m   ARM ABI: soft | softfp | hard\n");
  fprintf(stderr,
          "  \033[32m--host-cflags=F\033[0m     Extra host compiler flags\n");
  fprintf(stderr,
          "  \033[32m--host-ldflags=F\033[0m    Extra host linker flags\n");
  fprintf(stderr, "  \033[32m--gprof/--no-gprof\033[0m Enable/disable -pg in "
                  "native build path\n");
  fprintf(
      stderr,
      "  "
      "\033[32m--std-builtin-ops\033[0m/\033[32m--no-std-builtin-ops\033[0m\n");
  fprintf(stderr, "      Enable/disable builtin std operator lowering\n");
  fprintf(stderr,
          "  \033[32m--debug-locals\033[0m/\033[32m--no-debug-locals\033[0m\n");
  fprintf(stderr, "      Enable/disable local/global debug records\n");
  fprintf(stderr,
          "  \033[32m--dwarf-version=N\033[0m   DWARF version (2..5)\n");
  fprintf(stderr, "  "
                  "\033[32m--dwarf-split-inlining\033[0m/"
                  "\033[32m--no-dwarf-split-inlining\033[0m\n");
  fprintf(stderr, "      Enable/disable split-inline callsite info\n");
  fprintf(stderr, "  "
                  "\033[32m--dwarf-profile-info\033[0m/"
                  "\033[32m--no-dwarf-profile-info\033[0m\n");
  fprintf(stderr, "      Enable/disable profiling-friendly location info\n\n");
  fprintf(stderr, "\033[1mEXECUTION:\033[0m\n");
  fprintf(stderr, "  \033[32m-run\033[0m               JIT execute main() "
                  "after compilation\n");
  fprintf(stderr, "  \033[32m-emit-only\033[0m         Only emit IR, don't "
                  "execute (default)\n");
#ifdef _WIN32
  fprintf(stderr, "  \033[32m-o [path]\033[0m          Emit native binary at "
                  "[path] (default: a.exe; implies -emit-only)\n");
#else
  fprintf(stderr, "  \033[32m-o [path]\033[0m          Emit native binary at "
                  "[path] (default: a.out; implies -emit-only)\n");
#endif
  fprintf(stderr, "  \033[32m--output=<path>\033[0m    Same as -o\n");
  fprintf(stderr, "  \033[32m-strip, -no-strip\033[0m  Create "
                  "stripped/unstripped binary (default: strip)\n");
  fprintf(stderr,
          "  \033[32m-c <code>\033[0m          Execute inline code\n\n");
  fprintf(stderr, "\033[1mSTDLIB:\033[0m\n");
  fprintf(stderr, "  \033[32m-std [none|minimal|full]\033[0m Stdlib inclusion "
                  "mode (default: minimal)\n");
  fprintf(stderr,
          "  \033[32m-no-std\033[0m            Disable stdlib loading\n");
  fprintf(stderr,
          "  \033[32m--std-path=<path>\033[0m  Use custom std_bundle.ny\n\n");
  fprintf(stderr,
          "  \033[32m--full-mod\033[0m         Alias for -std full\n\n");
  fprintf(stderr, "\033[1mLINKING:\033[0m\n");
  fprintf(stderr, "  \033[32m-L<dir>\033[0m           Add a linker search path "
                  "(repeatable)\n");
  fprintf(stderr, "  \033[32m-l<lib>\033[0m           Link against library "
                  "(also accepts -l <lib>)\n\n");
  fprintf(stderr, "\033[1mREPL:\033[0m\n");
#ifdef _WIN32
  fprintf(stderr, "  \033[32m-i, -interactive\033[0m   Interactive REPL with "
                  "line editing\n");
#else
  fprintf(
      stderr,
      "  \033[32m-i, -interactive\033[0m   Interactive REPL with readline\n");
#endif
  fprintf(stderr, "  \033[32m-repl\033[0m              Read source from stdin "
                  "(one-shot)\n\n");
  fprintf(stderr, "\033[1mDEBUGGING:\033[0m\n");
  fprintf(stderr,
          "  \033[34m-v, -verbose\033[0m       Show compilation steps\n");
  fprintf(stderr,
          "  \033[34m--debug\033[0m            Enable verbose + debug logs\n");
  fprintf(stderr, "  \033[34m-v/-vv/-vvv\033[0m        Verbosity levels 1..3 "
                  "(or --verbose=N)\n");
  fprintf(stderr,
          "  \033[34m-time\033[0m              Show timing for each phase\n");
  fprintf(stderr, "  \033[34m-dump-ast\033[0m          Dump parsed AST\n");
  fprintf(stderr,
          "  \033[34m-dump-llvm\033[0m         Print LLVM IR to stdout\n");
  fprintf(stderr, "  \033[34m-dump-tokens\033[0m       Dump lexer_t tokens\n");
  fprintf(stderr, "  \033[34m-dump-docs\033[0m         Extract and print "
                  "function docstrings\n");
  fprintf(stderr,
          "  \033[34m-dump-funcs\033[0m        List all compiled functions\n");
  fprintf(stderr,
          "  \033[34m-dump-symbols\033[0m      Show runtime symbol table\n");
  fprintf(stderr,
          "  \033[34m-dump-stats\033[0m        Print compilation statistics\n");
  fprintf(stderr, "  \033[34m-verify\033[0m            Verify LLVM module\n");
  fprintf(stderr, "  \033[34m-g\033[0m                 Emit debug symbols\n");
  fprintf(stderr, "  \033[34m--emit-ir=<path>\033[0m   Emit LLVM IR to file\n");
  fprintf(stderr, "  \033[34m--ir-full\033[0m          Keep std/lib "
                  "definitions in IR output\n");
  fprintf(stderr, "  \033[34m--ir-no-std\033[0m        Omit std/lib "
                  "definitions in IR output (default)\n");
  fprintf(stderr,
          "  \033[34m--emit-asm=<path>\033[0m  Emit assembly to file\n");
  fprintf(stderr, "  \033[34m--dump-on-error\033[0m    Write "
                  "build/debug/last_source.ny and last_ir.ll on errors\n");
  fprintf(stderr,
          "  \033[34m-trace\033[0m             Enable execution tracing\n\n");
  fprintf(stderr, "\033[1mINFO:\033[0m\n");
  fprintf(stderr,
          "  \033[34m-h, -help, --help\033[0m  Show this help message\n");
  fprintf(stderr, "  \033[34m--help env\033[0m         Show supported "
                  "environment variables\n");
  fprintf(stderr, "  \033[34m-version\033[0m           Show version info\n\n");
  if (show_env) {
    fprintf(stderr, "\033[1mENVIRONMENT:\033[0m\n");
    fprintf(stderr, "  Policy env overrides are disabled by default.\n");
    fprintf(stderr,
            "  Use CLI flags for GPU/parallel/effect/debug behavior.\n");
    fprintf(stderr,
            "  \033[33mNYTRIX_STD_PATH\033[0m   Override std_bundle.ny path\n");
    fprintf(
        stderr,
        "  \033[33mNYTRIX_STD_PREBUILT\033[0m Use prebuilt stdlib source\n");
    fprintf(
        stderr,
        "  \033[33mNYTRIX_BUILD_STD_PATH\033[0m Override bundled std path\n");
    fprintf(stderr, "  \033[33mNYTRIX_HOST_TRIPLE\033[0m / "
                    "\033[33mNYTRIX_ARM_FLOAT_ABI\033[0m\n");
    fprintf(stderr, "  \033[33mNYTRIX_HOST_CFLAGS\033[0m / "
                    "\033[33mNYTRIX_HOST_LDFLAGS\033[0m\n");
    fprintf(stderr,
            "      Host build tuning (still accepted for tooling workflows)\n");
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "\033[1mEXAMPLES:\033[0m\n");
  fprintf(stderr,
          "  \033[1;36m%s etc/tests/benchmark/sieve.ny          # compile "
          "only\033[0m\n",
          prog);
  fprintf(
      stderr,
      "  \033[1;36m%s -O2 -run etc/tests/benchmark/sieve.ny  # compile & run "
      "optimized\033[0m\n",
      prog);
  fprintf(
      stderr,
      "  \033[1;36m%s -v -time -verify etc/tests/runtime/control.ny # debug "
      "compilation\033[0m\n",
      prog);
  fprintf(stderr,
          "  \033[1;36m%s -c 'print(\"hello\")'                # run "
          "inline\033[0m\n",
          prog);
  fprintf(stderr,
          "  \033[1;36m%s -i                                 # interactive "
          "REPL\033[0m\n",
          prog);
}

void ny_options_usage(const char *prog) { ny_options_usage_impl(prog, false); }

void ny_options_usage_env(const char *prog) {
  ny_options_usage_impl(prog, true);
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

    if (a[0] == '-') {
      if (strcmp(a, "-safe-mode") == 0 || strcmp(a, "--safe-mode") == 0 ||
          strcmp(a, "--safe") == 0) {
        opt->safe_mode = true;
      } else if (strcmp(a, "--effect-require-known") == 0) {
        opt->effect_require_known = true;
      } else if (strcmp(a, "--no-effect-require-known") == 0) {
        opt->effect_require_known = false;
      } else if (strcmp(a, "--alias-require-known") == 0) {
        opt->alias_require_known = true;
      } else if (strcmp(a, "--no-alias-require-known") == 0) {
        opt->alias_require_known = false;
      } else if (strcmp(a, "--alias-require-no-escape") == 0) {
        opt->alias_require_no_escape = true;
      } else if (strcmp(a, "--gprof") == 0 || strcmp(a, "-pg") == 0 ||
                 strcmp(a, "--profile") == 0) {
        opt->gprof = 1;
        opt->debug_symbols = true;
      } else if (strcmp(a, "--no-gprof") == 0) {
        opt->gprof = 0;
      } else if (strcmp(a, "--std-builtin-ops") == 0) {
        opt->std_builtin_ops = 1;
      } else if (strcmp(a, "--no-std-builtin-ops") == 0) {
        opt->std_builtin_ops = 0;
      } else if (strcmp(a, "--debug-locals") == 0) {
        opt->debug_locals = 1;
      } else if (strcmp(a, "--no-debug-locals") == 0) {
        opt->debug_locals = 0;
      } else if ((value = ny_option_value_or_die(a, "--dwarf-version", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->dwarf_version = ny_parse_dwarf_version_or_die(value, argv[0]);
      } else if (strcmp(a, "--dwarf-split-inlining") == 0) {
        opt->dwarf_split_inlining = 1;
      } else if (strcmp(a, "--no-dwarf-split-inlining") == 0) {
        opt->dwarf_split_inlining = 0;
      } else if (strcmp(a, "--dwarf-profile-info") == 0) {
        opt->dwarf_profile_info = 1;
      } else if (strcmp(a, "--no-dwarf-profile-info") == 0) {
        opt->dwarf_profile_info = 0;
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
      }
      // Optimization
      else if (strcmp(a, "-O") == 0 || strcmp(a, "-O2") == 0)
        opt->opt_level = 2;
      else if (strcmp(a, "-O1") == 0)
        opt->opt_level = 1;
      else if (strcmp(a, "-O0") == 0)
        opt->opt_level = 0;
      else if (strcmp(a, "-O3") == 0)
        opt->opt_level = 3;
      else if (strcmp(a, "--fast") == 0) {
        if (!opt->opt_level)
          opt->opt_level = 2;
        opt->verify_module = 0;
        opt->strip_override = 1;
      } else if (strncmp(a, "-passes=", 8) == 0)
        opt->opt_pipeline = a + 8;
      // Optimization control flags
      else if (strcmp(a, "--opt-dce") == 0 || strcmp(a, "--dce") == 0)
        opt->opt_dce = 1;
      else if (strcmp(a, "--no-opt-dce") == 0 || strcmp(a, "--no-dce") == 0)
        opt->opt_dce = 0;
      else if (strcmp(a, "--opt-internalize") == 0)
        opt->opt_internalize = 1;
      else if (strcmp(a, "--no-opt-internalize") == 0)
        opt->opt_internalize = 0;
      else if (strcmp(a, "--opt-loops") == 0)
        opt->opt_loops = 1;
      else if (strcmp(a, "--no-opt-loops") == 0)
        opt->opt_loops = 0;
      else if (strcmp(a, "--opt-autotune") == 0)
        opt->opt_autotune = 1;
      else if (strcmp(a, "--no-opt-autotune") == 0)
        opt->opt_autotune = 0;
      else if ((value = ny_option_value_or_die(a, "--gpu", &i, argc, argv,
                                               argv[0])) != NULL) {
        if (ny_is_gpu_mode(value)) {
          opt->gpu_mode = value;
        } else {
          fprintf(stderr, "unknown gpu mode: %s\n", value);
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if ((value = ny_option_value_or_die(a, "--gpu-backend", &i, argc,
                                                 argv, argv[0])) != NULL) {
        const char *backend = value;
        if (strcmp(backend, "off") == 0)
          backend = "none";
        if (ny_is_gpu_backend(backend)) {
          opt->gpu_backend = backend;
        } else {
          fprintf(stderr, "unknown gpu backend: %s\n", backend);
          ny_options_usage(argv[0]);
          exit(1);
        }
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
        if (ny_is_gpu_offload_mode(value)) {
          opt->gpu_offload = value;
        } else {
          fprintf(stderr, "unknown gpu offload mode: %s\n", value);
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if ((value = ny_option_value_or_die(a, "--gpu-min-work", &i, argc,
                                                 argv, argv[0])) != NULL) {
        opt->gpu_min_work =
            ny_parse_nonneg_int_or_die(value, "gpu min work", argv[0]);
      } else if (strcmp(a, "--gpu-async") == 0) {
        opt->gpu_async = 1;
      } else if (strcmp(a, "--no-gpu-async") == 0) {
        opt->gpu_async = 0;
      } else if (strcmp(a, "--gpu-fast-math") == 0) {
        opt->gpu_fast_math = 1;
      } else if (strcmp(a, "--no-gpu-fast-math") == 0) {
        opt->gpu_fast_math = 0;
      } else if ((value = ny_option_value_or_die(a, "--parallel", &i, argc,
                                                 argv, argv[0])) != NULL) {
        if (ny_is_parallel_mode(value)) {
          opt->parallel_mode = value;
        } else {
          fprintf(stderr, "unknown parallel mode: %s\n", value);
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if ((value = ny_option_value_or_die(a, "--threads", &i, argc, argv,
                                                 argv[0])) != NULL) {
        opt->thread_count =
            ny_parse_nonneg_int_or_die(value, "thread count", argv[0]);
      } else if ((value = ny_option_value_or_die(a, "--parallel-min-work", &i,
                                                 argc, argv, argv[0])) !=
                 NULL) {
        opt->parallel_min_work =
            ny_parse_nonneg_int_or_die(value, "parallel min work", argv[0]);
      } else if (strcmp(a, "-strip") == 0 || strcmp(a, "--strip") == 0)
        opt->strip_override = 1;
      else if (strcmp(a, "-no-strip") == 0 || strcmp(a, "--no-strip") == 0)
        opt->strip_override = 0;

      // Execution
      else if (strcmp(a, "-run") == 0)
        opt->run_jit = true;
      else if (strcmp(a, "-emit-only") == 0)
        opt->emit_only = true;
      else if (strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) {
        if (i + 1 < argc && argv[i + 1][0] != '-')
          opt->output_file = argv[++i];
        else
#ifdef _WIN32
          opt->output_file = "a.exe";
#else
          opt->output_file = "a.out";
#endif
      } else if (strncmp(a, "--output=", 9) == 0)
        opt->output_file = a + 9;
      else if (strcmp(a, "-c") == 0 || strcmp(a, "-e") == 0 ||
               strcmp(a, "--eval") == 0) {
        if (i + 1 < argc && argv[i + 1][0] != '-')
          opt->command_string = argv[++i];
        else if (opt->input_file) {
          opt->command_string = opt->input_file;
          opt->input_file = NULL;
        }
      }

      // REPL
      else if (strcmp(a, "-i") == 0 || strcmp(a, "-interactive") == 0)
        opt->mode = NY_MODE_REPL;
      else if (strcmp(a, "-repl") == 0) {
        opt->mode = NY_MODE_REPL;
      }

      // Debug
      else if (strcmp(a, "--debug") == 0) {
        opt->verbose = 3;
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
      } else if (strcmp(a, "-time") == 0)
        opt->do_timing = true;
      else if (strcmp(a, "-dump-ast") == 0)
        opt->dump_ast = true;
      else if (strcmp(a, "-dump-llvm") == 0)
        opt->dump_llvm = true;
      else if (strncmp(a, "--emit-ir=", 10) == 0)
        opt->emit_ir_path = a + 10;
      else if (strcmp(a, "--ir-full") == 0)
        opt->ir_include_std = true;
      else if (strcmp(a, "--ir-no-std") == 0)
        opt->ir_include_std = false;
      else if (strncmp(a, "--emit-asm=", 11) == 0)
        opt->emit_asm_path = a + 11;
      else if (strcmp(a, "-dump-tokens") == 0)
        opt->dump_tokens = true;
      else if (strcmp(a, "-dump-docs") == 0)
        opt->dump_docs = true;
      else if (strcmp(a, "-dump-funcs") == 0)
        opt->dump_funcs = true;
      else if (strcmp(a, "-dump-symbols") == 0)
        opt->dump_symbols = true;
      else if (strcmp(a, "-dump-stats") == 0)
        opt->dump_stats = true;
      else if (strcmp(a, "--dump-on-error") == 0)
        opt->dump_on_error = true;
      else if (strcmp(a, "-trace") == 0)
        opt->trace_exec = true;
      else if (strcmp(a, "-verify") == 0)
        opt->verify_module = true;
      else if (strcmp(a, "-g") == 0)
        opt->debug_symbols = true;

      // Std
      else if (strcmp(a, "-std") == 0) {
        if (i + 1 < argc) {
          opt->std_mode = ny_parse_std_mode_or_die(argv[++i], argv[0]);
        } else {
          fprintf(stderr, "missing argument for -std\n");
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if (strcmp(a, "--full-mod") == 0) {
        opt->std_mode = STD_MODE_FULL;
      } else if (strcmp(a, "-no-std") == 0)
        opt->no_std = true;
      else if (strncmp(a, "--std-path=", 11) == 0)
        opt->std_path = a + 11;
      else if (strcmp(a, "--plain-repl") == 0)
        opt->repl_plain = true;

      // Linker flags
      else if (a[1] == 'L' && a[2] != '\0') {
        vec_push(&opt->link_dirs, ny_strdup(a));
      } else if (strcmp(a, "-L") == 0) {
        ny_push_link_flag_or_die(opt, true, "-L", &i, argc, argv, argv[0]);
      } else if (a[1] == 'l' && a[2] != '\0') {
        vec_push(&opt->link_libs, ny_strdup(a));
      } else if (strcmp(a, "-l") == 0) {
        ny_push_link_flag_or_die(opt, false, "-l", &i, argc, argv, argv[0]);
      }

      // Color
      else if (strncmp(a, "--color=", 8) == 0) {
        const char *mode = a + 8;
        if (strcmp(mode, "auto") == 0)
          opt->color_mode = -1;
        else if (strcmp(mode, "always") == 0)
          opt->color_mode = 1;
        else if (strcmp(mode, "never") == 0)
          opt->color_mode = 0;
      }

      // Info
      else if (strcmp(a, "--help-env") == 0 || strcmp(a, "--env-help") == 0) {
        opt->mode = NY_MODE_HELP;
        opt->help_env = true;
      } else if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0 ||
                 strcmp(a, "--help") == 0) {
        opt->mode = NY_MODE_HELP;
        if (i + 1 < argc && strcmp(argv[i + 1], "env") == 0) {
          opt->help_env = true;
          i++;
        }
      } else if (strcmp(a, "-version") == 0 || strcmp(a, "--version") == 0)
        opt->mode = NY_MODE_VERSION;

      else {
        // Unknown flag
        ny_options_usage(argv[0]);
        exit(1);
      }
    } else {
      if (!opt->input_file && !opt->command_string) {
        opt->input_file = a;
        opt->file_arg_idx = i;
      } else {
        // Second positional or already have command_string
        if (!opt->file_arg_idx)
          opt->file_arg_idx = i;
        break;
      }
    }
    i++;
  }

  // Post-process logic
  if (opt->command_string && opt->input_file) {
    // Interleaved -c and string: 'ny code -c'
    opt->input_file = NULL;
  }

  if (opt->output_file)
    opt->emit_only = true;
  if (opt->emit_only)
    opt->run_jit = false;
  else if (opt->input_file || opt->command_string || opt->mode == NY_MODE_REPL)
    opt->run_jit = true;

  // AUTO-DETECT STANDARD LIBRARY PATH
  if (!opt->std_path) {
    const char *env_std = getenv("NYTRIX_STD_PATH");
    if (env_std) {
      opt->std_path = env_std;
    } else {
      if (access("./build/std_bundle.ny", R_OK) == 0) {
        opt->std_path = "./build/std_bundle.ny";
      } else {
#ifdef NYTRIX_STD_PATH
        opt->std_path = NYTRIX_STD_PATH;
#else
#ifdef _WIN32
        const char *pd = getenv("PROGRAMDATA");
        if (pd && *pd) {
          static char stdp[4096];
          char tmp[4096];
          ny_join_path(tmp, sizeof(tmp), pd, "nytrix");
          ny_join_path(stdp, sizeof(stdp), tmp, "std_bundle.ny");
          if (access(stdp, R_OK) == 0) {
            opt->std_path = stdp;
            return;
          }
        }
        opt->std_path = "C:/ProgramData/nytrix/std_bundle.ny";
#else
        static const char *paths[] = {
            "/usr/share/nytrix/std_bundle.ny",
            "/usr/local/share/nytrix/std_bundle.ny",
            "/opt/homebrew/share/nytrix/std_bundle.ny",
            "/opt/nytrix/share/std_bundle.ny",
        };
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
          if (access(paths[i], R_OK) == 0) {
            opt->std_path = paths[i];
            return;
          }
        }
        opt->std_path = paths[0];
#endif
#endif
      }
    }
  }
}
