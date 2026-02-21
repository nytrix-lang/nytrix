#include "base/common.h"
#include "base/loader.h"
#include "base/options.h"
#include "base/util.h"
#include "code/jit.h"
#include "rt/shared.h"
#include "wire/pipe.h"
#ifndef _WIN32
#include <execinfo.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int64_t __trace_dump(int64_t n);
#ifdef _WIN32
extern int64_t __enable_vt(void);
#endif

#ifndef _WIN32
extern char **environ;
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define NY_HAS_ASAN 1
#endif
#endif

#ifdef NY_HAS_ASAN
const char *__lsan_default_suppressions() {
  return "leak:__malloc\n"
         "leak:__to_str\n"
         "leak:ny_strdup\n"
         "leak:vec_push_impl\n"
         "leak:LLVM\n"
         "leak:libnytrixrt.so\n"
         "leak:^__malloc$\n"
         "leak:^__flt_box_val$\n"
         "leak:^__add$\n";
}
#endif

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
#ifdef _WIN32
  fprintf(stderr, "%sCaught signal %d%s\n", clr(NY_CLR_RED), sig,
          clr(NY_CLR_RESET));
  print_last_trace();
  fprintf(stderr, "%sRecent Nytrix trace:%s\n", clr(NY_CLR_CYAN),
          clr(NY_CLR_RESET));
  __trace_dump(((int64_t)8 << 1) | 1);
  exit(128 + sig);
#else
  void *bt[64];
  int n = backtrace(bt, 64);
  fprintf(stderr, "%sCaught signal %d (%s), backtrace:%s\n", clr(NY_CLR_RED),
          sig, strsignal(sig), clr(NY_CLR_RESET));
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
#endif
}

static void ny_setenv_force(const char *key, const char *value) {
  if (!key || !*key || !value)
    return;
#ifdef _WIN32
  (void)_putenv_s(key, value);
#else
  (void)setenv(key, value, 1);
#endif
}

static void ny_unsetenv_force(const char *key) {
  if (!key || !*key)
    return;
#ifdef _WIN32
  (void)_putenv_s(key, "");
#else
  (void)unsetenv(key);
#endif
}

static void ny_clear_policy_env_overrides(void) {
  static const char *const keys[] = {"NYTRIX_EFFECT_REQUIRE_KNOWN",
                                     "NYTRIX_ALIAS_REQUIRE_KNOWN",
                                     "NYTRIX_ALIAS_REQUIRE_NO_ESCAPE",
                                     "NYTRIX_GPU_MODE",
                                     "NYTRIX_GPU_BACKEND",
                                     "NYTRIX_GPU_OFFLOAD",
                                     "NYTRIX_GPU_MIN_WORK",
                                     "NYTRIX_GPU_ASYNC",
                                     "NYTRIX_GPU_FAST_MATH",
                                     "NYTRIX_ACCEL_TARGET",
                                     "NYTRIX_ACCEL_OBJECT",
                                     "NYTRIX_PARALLEL_MODE",
                                     "NYTRIX_PARALLEL_THREADS",
                                     "NYTRIX_PARALLEL_MIN_WORK",
                                     "NYTRIX_GPROF",
                                     "NYTRIX_STD_BUILTIN_OPS",
                                     "NYTRIX_DEBUG_LOCALS",
                                     "NYTRIX_DWARF_VERSION",
                                     "NYTRIX_DWARF_SPLIT_INLINING",
                                     "NYTRIX_DWARF_PROFILE_INFO",
                                     NULL};
  for (size_t i = 0; keys[i]; ++i)
    ny_unsetenv_force(keys[i]);
}

static char **ny_current_envp(char **fallback_envp) {
#ifdef _WIN32
  return _environ ? _environ : fallback_envp;
#else
  return environ ? environ : fallback_envp;
#endif
}

static bool ny_argv_has_flag(int argc, char **argv, const char *flag) {
  if (!flag || !*flag)
    return false;
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!a)
      continue;
    if (strcmp(a, "--") == 0)
      break;
    if (strcmp(a, flag) == 0)
      return true;
  }
  return false;
}

int main(int argc, char **argv, char **envp) {
  signal(SIGSEGV, handle_segv);
  signal(SIGABRT, handle_segv);
  signal(SIGFPE, handle_segv);
  signal(SIGILL, handle_segv);

  ny_options opt;
  ny_options_init(&opt);
  ny_options_parse(&opt, argc, argv);
  if (argc < 2 && !opt.command_string && !opt.input_file &&
      opt.mode != NY_MODE_HELP && opt.mode != NY_MODE_VERSION &&
      isatty(STDIN_FILENO)) {
    opt.mode = NY_MODE_REPL;
  }
  if (opt.mode == NY_MODE_REPL && !opt.safe_mode) {
    bool effect_policy_explicit =
        ny_argv_has_flag(argc, argv, "--effect-require-known") ||
        ny_argv_has_flag(argc, argv, "--no-effect-require-known");
    bool alias_policy_explicit =
        ny_argv_has_flag(argc, argv, "--alias-require-known") ||
        ny_argv_has_flag(argc, argv, "--no-alias-require-known");
    bool alias_escape_explicit =
        ny_argv_has_flag(argc, argv, "--alias-require-no-escape");
    if (!effect_policy_explicit)
      opt.effect_require_known = false;
    if (!alias_policy_explicit)
      opt.alias_require_known = false;
    if (!alias_escape_explicit)
      opt.alias_require_no_escape = false;
  }
  ny_clear_policy_env_overrides();
  if (opt.safe_mode) {
    opt.verify_module = true;
    ny_setenv_force("NYTRIX_STRICT_DIAGNOSTICS", "1");
    ny_setenv_force("NYTRIX_AUTO_PURITY", "1");
    ny_setenv_force("NYTRIX_EFFECT_REQUIRE_KNOWN", "1");
    ny_setenv_force("NYTRIX_ALIAS_REQUIRE_KNOWN", "1");
    ny_setenv_force("NYTRIX_ALIAS_REQUIRE_NO_ESCAPE", "1");
  }
  if (opt.effect_require_known)
    ny_setenv_force("NYTRIX_EFFECT_REQUIRE_KNOWN", "1");
  if (opt.alias_require_known)
    ny_setenv_force("NYTRIX_ALIAS_REQUIRE_KNOWN", "1");
  if (opt.alias_require_no_escape)
    ny_setenv_force("NYTRIX_ALIAS_REQUIRE_NO_ESCAPE", "1");
  if (opt.gprof >= 0)
    ny_setenv_force("NYTRIX_GPROF", opt.gprof ? "1" : "0");
  if (opt.std_builtin_ops >= 0)
    ny_setenv_force("NYTRIX_STD_BUILTIN_OPS", opt.std_builtin_ops ? "1" : "0");
  if (opt.debug_locals >= 0)
    ny_setenv_force("NYTRIX_DEBUG_LOCALS", opt.debug_locals ? "1" : "0");
  if (opt.dwarf_version >= 2 && opt.dwarf_version <= 5) {
    char dbuf[8];
    snprintf(dbuf, sizeof(dbuf), "%d", opt.dwarf_version);
    ny_setenv_force("NYTRIX_DWARF_VERSION", dbuf);
  }
  if (opt.dwarf_split_inlining >= 0)
    ny_setenv_force("NYTRIX_DWARF_SPLIT_INLINING",
                    opt.dwarf_split_inlining ? "1" : "0");
  if (opt.dwarf_profile_info >= 0)
    ny_setenv_force("NYTRIX_DWARF_PROFILE_INFO",
                    opt.dwarf_profile_info ? "1" : "0");
  if (opt.host_cflags)
    ny_setenv_force("NYTRIX_HOST_CFLAGS", opt.host_cflags);
  if (opt.host_ldflags)
    ny_setenv_force("NYTRIX_HOST_LDFLAGS", opt.host_ldflags);
  if (opt.host_triple)
    ny_setenv_force("NYTRIX_HOST_TRIPLE", opt.host_triple);
  if (opt.arm_float_abi)
    ny_setenv_force("NYTRIX_ARM_FLOAT_ABI", opt.arm_float_abi);
  if (opt.gpu_mode)
    ny_setenv_force("NYTRIX_GPU_MODE", opt.gpu_mode);
  if (opt.gpu_backend)
    ny_setenv_force("NYTRIX_GPU_BACKEND", opt.gpu_backend);
  if (opt.gpu_offload)
    ny_setenv_force("NYTRIX_GPU_OFFLOAD", opt.gpu_offload);
  if (opt.gpu_min_work > 0) {
    char gwb[32];
    snprintf(gwb, sizeof(gwb), "%d", opt.gpu_min_work);
    ny_setenv_force("NYTRIX_GPU_MIN_WORK", gwb);
  }
  if (opt.gpu_async >= 0)
    ny_setenv_force("NYTRIX_GPU_ASYNC", opt.gpu_async ? "1" : "0");
  if (opt.gpu_fast_math >= 0)
    ny_setenv_force("NYTRIX_GPU_FAST_MATH", opt.gpu_fast_math ? "1" : "0");
  if (opt.accel_target)
    ny_setenv_force("NYTRIX_ACCEL_TARGET", opt.accel_target);
  if (opt.accel_object)
    ny_setenv_force("NYTRIX_ACCEL_OBJECT", opt.accel_object);
  if (opt.parallel_mode)
    ny_setenv_force("NYTRIX_PARALLEL_MODE", opt.parallel_mode);
  if (opt.thread_count > 0) {
    char tb[32];
    snprintf(tb, sizeof(tb), "%d", opt.thread_count);
    ny_setenv_force("NYTRIX_PARALLEL_THREADS", tb);
  }
  if (opt.parallel_min_work > 0) {
    char pb[32];
    snprintf(pb, sizeof(pb), "%d", opt.parallel_min_work);
    ny_setenv_force("NYTRIX_PARALLEL_MIN_WORK", pb);
  }
#ifdef _WIN32
  if (opt.color_mode != 0)
    (void)__enable_vt();
#endif

  // Set runtime args (host -> JIT bridge)
  // If we have an input file, script args start there.
  char **runtime_envp = ny_current_envp(envp);
  if (opt.input_file) {
    int s_argc = argc - opt.file_arg_idx;
    char **s_argv = &argv[opt.file_arg_idx];
    __set_args((int64_t)s_argc, (int64_t)(uintptr_t)s_argv,
               (int64_t)(uintptr_t)runtime_envp);
  } else if (opt.command_string) {
    static char *eval_argv[] = {(char *)"nytrix", NULL};
    __set_args(1, (int64_t)(uintptr_t)eval_argv,
               (int64_t)(uintptr_t)runtime_envp);
  } else if (opt.mode == NY_MODE_REPL) {
    if (opt.file_arg_idx > 0 && opt.file_arg_idx < argc) {
      int r_argc = argc - opt.file_arg_idx;
      char **r_argv = &argv[opt.file_arg_idx];
      __set_args((int64_t)r_argc, (int64_t)(uintptr_t)r_argv,
                 (int64_t)(uintptr_t)runtime_envp);
    } else {
      static char *repl_argv[] = {(char *)"nytrix", NULL};
      __set_args(1, (int64_t)(uintptr_t)repl_argv,
                 (int64_t)(uintptr_t)runtime_envp);
    }
  } else {
    // Default to host argv.
    __set_args((int64_t)argc, (int64_t)(uintptr_t)argv,
               (int64_t)(uintptr_t)runtime_envp);
  }

  if (argc < 2 && opt.mode != NY_MODE_REPL && !opt.command_string &&
      !opt.input_file) {
    ny_options_usage(argv[0]);
    return 0;
  }

  int exit_code = ny_pipeline_run(&opt);
  // Cleanup
  ny_std_free_modules();
  __runtime_cleanup();
  return exit_code;
}
