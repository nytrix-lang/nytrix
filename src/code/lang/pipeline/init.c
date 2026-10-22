/*
 * Compiler pipeline entry: source-to-artifact orchestration,
 * phase sequencing, error collection, and artifact emission.
 */
#define _CRT_RAND_S
#include "base/common.h"
#include "base/debug.h"
#include "base/hash.h"
#include "base/loader.h"
#include "base/progress.h"
#include "base/time.h"
#include "base/trace.h"
#include "base/util.h"
#include "base/version.h"
#include "cmd/ny/repl/repl.h"
#include "code/backend/ir/module.h"
#include "code/backend/llvm/bridge.h"
#include "code/backend/native.h"
#include "code/backend/wire/bundle.h"
#include "code/backend/wire/cache.h"
#include "code/backend/wire/incremental/incremental.h"
#include "code/lang/code.h"
#include "code/lang/llvm/jit.h"
#include "code/lang/lower.h"
#include "code/lang/parse/frontend/visitor.h"
#include "code/lang/parse/json.h"
#include "code/lang/parse/parser.h"
#include "code/lang/pipeline.h"
#include "code/lang/pipeline/build.h"
#include "code/lang/pipeline/native.h"
#include "code/lang/priv.h"
#include "code/lang/types/pipeline.h"
#include <limits.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Error.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Support.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm/Config/llvm-config.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dlfcn.h>
#include <sys/wait.h>
#endif
#if defined(__linux__) && defined(__x86_64__)
#include <dlfcn.h>
#include <elf.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif
#if defined(__linux__) && defined(__x86_64__)
#include <elf.h>
#endif
#include <ctype.h>
#include <stdarg.h>

static bool ny_pipeline_write_callgraph_for_program(const program_t *prog,
                                                    const ny_options *opt,
                                                    char *err, size_t err_len) {
  if (!prog || !opt || !opt->callgraph)
    return true;
  nyr_func_t rt_main = {0};
  nyr_func_t funcs[256] = {0};
  const char *func_names[256] = {0};
  size_t func_count = 0;
  char local_err[512] = {0};
  if (!nyr_build_ir(prog, opt, &rt_main, funcs, &func_count, func_names, 256,
                    local_err, sizeof(local_err))) {
    if (err && err_len)
      snprintf(err, err_len, "callgraph: %s",
               local_err[0] ? local_err : "failed to build NYIR");
    return false;
  }
  const nyr_backend_module_t module = {&rt_main, funcs, func_names, func_count,
                                       NULL};
  const nyr_callgraph_options_t options = {opt->callgraph_json,
                                           opt->callgraph_depth};
  bool ok = nyr_write_callgraph_for_ir(&module, &options, err, err_len);
  nyr_ir_func_free(&rt_main);
  for (size_t i = 0; i < func_count; ++i)
    nyr_ir_func_free(&funcs[i]);
  return ok;
}

typedef struct ny_member_splice_check_t {
  bool found;
} ny_member_splice_check_t;

static bool ny_pipe_check_member_splice_expr(ny_visitor_t *visitor,
                                             expr_t *expr) {
  ny_member_splice_check_t *check = visitor ? visitor->ctx : NULL;
  if (check && expr && expr->kind == NY_E_MEMBER &&
      expr->as.member.computed_name) {
    ny_diag_error(expr->tok,
                  "member splice requires a compile-time field descriptor "
                  "inside `#fields(Layout)`");
    check->found = true;
  }
  return true;
}

static bool ny_pipe_check_member_splices(program_t *prog) {
  ny_member_splice_check_t check = {0};
  ny_visitor_t visitor = {
      .ctx = &check,
      .visit_expr_pre = ny_pipe_check_member_splice_expr,
  };
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    ny_visit_stmt(&visitor, prog->body.data[i]);
  return check.found;
}

static void maybe_log_phase_time(bool enabled, const char *label,
                                 ny_tick_t start_time);

/*
 * Opt-in pipeline memory checkpoints for investigating compile-time spikes.
 * VmSize helps distinguish allocator/address-space reservations from resident
 * memory; VmHWM records the process high-water mark. Keep the normal path to
 * one trace-gate check and do not retain per-phase state.
 */
static void ny_pipe_trace_phase_memory(const char *phase) {
  if (!ny_trace_enabled("NY_TRACE_PHASE_MEMORY"))
    return;
#if defined(__linux__)
  FILE *status = fopen("/proc/self/status", "r");
  if (!status) {
    fprintf(stderr, "[resource] phase=%s memory=unavailable\n", phase);
    fflush(stderr);
    return;
  }
  unsigned long vm_size_kb = 0, vm_rss_kb = 0, vm_hwm_kb = 0;
  char line[256];
  while (fgets(line, sizeof(line), status)) {
    if (sscanf(line, "VmSize: %lu kB", &vm_size_kb) == 1)
      continue;
    if (sscanf(line, "VmRSS: %lu kB", &vm_rss_kb) == 1)
      continue;
    if (sscanf(line, "VmHWM: %lu kB", &vm_hwm_kb) == 1)
      continue;
  }
  fclose(status);
  fprintf(stderr,
          "[resource] "
          "phase=%s rss=%luMiB peak_rss=%luMiB virtual=%luMiB",
          phase, vm_rss_kb / 1024ul, vm_hwm_kb / 1024ul, vm_size_kb / 1024ul);
  fputc('\n', stderr);
  fflush(stderr);
#else
  fprintf(stderr, "[resource] phase=%s memory=unsupported\n", phase);
  fflush(stderr);
#endif
}

extern int64_t rt_free(int64_t ptr);
extern void rt_print_flush(void);

typedef struct {
  bool function_profile;
  bool native_oracle;
  int tv_trials;
} ny_main_observability_t;

static bool ny_pipe_expr_calls(const expr_t *expr, const char *name) {
  if (!expr)
    return false;
  if (expr->kind == NY_E_MEMCALL)
    return expr->as.memcall.name && strcmp(expr->as.memcall.name, name) == 0;
  if (expr->kind != NY_E_CALL)
    return false;
  const expr_t *callee = expr->as.call.callee;
  if (ny_expr_ident_is_name(callee, name))
    return true;
  return callee && callee->kind == NY_E_MEMBER && callee->as.member.name &&
         strcmp(callee->as.member.name, name) == 0;
}

static bool ny_pipe_is_main_guard(const stmt_t *stmt) {
  if (!stmt || stmt->kind != NY_S_IF || !stmt->as.iff.test ||
      stmt->as.iff.test->kind != NY_E_COMPTIME)
    return false;
  const stmt_t *body = stmt->as.iff.test->as.comptime_expr.body;
  if (!body || body->kind != NY_S_BLOCK || body->as.block.body.len != 1)
    return false;
  const stmt_t *ret = body->as.block.body.data[0];
  return ret && ret->kind == NY_S_RETURN &&
         ny_expr_is_zero_arg_call_named(ret->as.ret.value, "__main");
}

static void ny_pipe_scan_request_expr(const expr_t *expr,
                                      ny_main_observability_t *requests) {
  if (!expr || !requests)
    return;
  if (ny_pipe_expr_calls(expr, "request_native_oracle"))
    requests->native_oracle = true;
  if (ny_pipe_expr_calls(expr, "request_tv_checks")) {
    requests->tv_trials = 16;
    const ny_call_arg_list *args = expr->kind == NY_E_MEMCALL
                                       ? &expr->as.memcall.args
                                       : &expr->as.call.args;
    if (args->len && args->data[0].val &&
        args->data[0].val->kind == NY_E_LITERAL &&
        args->data[0].val->as.literal.kind == NY_LIT_INT) {
      int64_t trials = args->data[0].val->as.literal.as.i;
      if (trials >= 0 && trials <= 10000)
        requests->tv_trials = (int)trials;
    }
  }
}

static void ny_pipe_scan_main_requests(const stmt_t *stmt,
                                       ny_main_observability_t *requests,
                                       unsigned depth) {
  if (!stmt || !requests || depth > 128)
    return;
  switch (stmt->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      ny_pipe_scan_main_requests(stmt->as.block.body.data[i], requests,
                                 depth + 1);
    break;
  case NY_S_EXPR:
    ny_pipe_scan_request_expr(stmt->as.expr.expr, requests);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < stmt->as.var.exprs.len; ++i)
      ny_pipe_scan_request_expr(stmt->as.var.exprs.data[i], requests);
    break;
  case NY_S_IF:
    ny_pipe_scan_request_expr(stmt->as.iff.test, requests);
    ny_pipe_scan_main_requests(stmt->as.iff.conseq, requests, depth + 1);
    ny_pipe_scan_main_requests(stmt->as.iff.alt, requests, depth + 1);
    break;
  case NY_S_WHILE:
    ny_pipe_scan_request_expr(stmt->as.whl.test, requests);
    ny_pipe_scan_main_requests(stmt->as.whl.body, requests, depth + 1);
    ny_pipe_scan_main_requests(stmt->as.whl.update, requests, depth + 1);
    break;
  case NY_S_FOR:
    ny_pipe_scan_main_requests(stmt->as.fr.init, requests, depth + 1);
    ny_pipe_scan_main_requests(stmt->as.fr.body, requests, depth + 1);
    ny_pipe_scan_main_requests(stmt->as.fr.update, requests, depth + 1);
    break;
  case NY_S_RETURN:
    ny_pipe_scan_request_expr(stmt->as.ret.value, requests);
    break;
  case NY_S_DEFER:
    ny_pipe_scan_main_requests(stmt->as.de.body, requests, depth + 1);
    break;
  case NY_S_TRY:
    ny_pipe_scan_main_requests(stmt->as.tr.body, requests, depth + 1);
    ny_pipe_scan_main_requests(stmt->as.tr.handler, requests, depth + 1);
    break;
  default:
    break;
  }
}

static void ny_pipe_scan_modules(const stmt_t *stmt,
                                 ny_main_observability_t *requests,
                                 unsigned depth) {
  if (!stmt || depth > 128)
    return;
  if (ny_pipe_is_main_guard(stmt)) {
    ny_pipe_scan_main_requests(stmt->as.iff.conseq, requests, 0);
  } else if (stmt->kind == NY_S_MODULE) {
    for (size_t i = 0; i < stmt->as.module.body.len; ++i)
      ny_pipe_scan_modules(stmt->as.module.body.data[i], requests, depth + 1);
  } else if (stmt->kind == NY_S_USE && stmt->as.use.module &&
             strcmp(stmt->as.use.module, "core.os.profile") == 0) {
    requests->function_profile = true;
  }
}

static void ny_pipe_apply_main_requests(const program_t *prog,
                                        ny_options *opt) {
  if (!prog || !opt)
    return;
  ny_main_observability_t requests = {0};
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_pipe_scan_modules(prog->body.data[i], &requests, 0);
  if (requests.function_profile) {
    opt->trace_exec = true;
    ny_setenv("NYTRIX_TRACE", "1", 1);
  }
  if (requests.native_oracle) {
    opt->native_result_oracle = true;
    opt->native_backend = NY_NATIVE_BACKEND_X86_64;
    opt->native_backend_raw = "x86_64";
  }
  if (requests.tv_trials > 0) {
    opt->native_tv_seed_trials = requests.tv_trials;
    opt->native_result_oracle = true;
    opt->native_backend = NY_NATIVE_BACKEND_X86_64;
    opt->native_backend_raw = "x86_64";
  }
}

static bool ny_safe_run_requested(const ny_safe_run_t *sr) {
  return sr && (sr->cpu_seconds > 0 || sr->wall_seconds > 0 ||
                sr->max_rss_bytes > 0 || sr->max_files > 0 ||
                sr->max_processes > 0 || sr->max_output_bytes > 0 ||
                sr->telemetry || sr->contain_process_group);
}

static void ny_sanitize_setup_env(const char *sanitize_kind) {
  if (!sanitize_kind || !*sanitize_kind)
    return;
  if (strcmp(sanitize_kind, "address") == 0) {
    ny_setenv("ASAN_OPTIONS",
              "abort_on_error=1:detect_leaks=1:print_stacktrace=1:color=never"
              ":detect_odr_violation=0",
              0);
  } else if (strcmp(sanitize_kind, "undefined") == 0) {
    ny_setenv("UBSAN_OPTIONS",
              "abort_on_error=1:print_stacktrace=1:color=never"
              ":halt_on_error=1",
              0);
  } else if (strcmp(sanitize_kind, "leak") == 0) {
    ny_setenv("LSAN_OPTIONS", "print_suppressions=0", 0);
  } else if (strcmp(sanitize_kind, "thread") == 0) {
    ny_setenv("TSAN_OPTIONS",
              "abort_on_error=1:report_thread_leaks=0:color=never", 0);
  }
}

typedef struct {
  uint64_t script_addr;
  uint64_t main_addr;
} ny_jit_safe_call_t;

static bool ny_jit_prepare_call(uint64_t script_addr, uint64_t main_addr) {
  return script_addr && ny_jit_prepare_execution(script_addr) &&
         (!main_addr || ny_jit_prepare_execution(main_addr));
}

static int ny_jit_safe_child(void *raw) {
  ny_jit_safe_call_t *call = (ny_jit_safe_call_t *)raw;
  if (!call || !ny_jit_prepare_call(call->script_addr, call->main_addr))
    return 1;
  ((void (*)(void))call->script_addr)();
  if (call->main_addr)
    (void)((int64_t (*)(void))call->main_addr)();
  rt_print_flush();
  return 0;
}

#ifndef _WIN32
static int ny_native_cache_safe_child(void *raw) {
  void (*entry)(void) = (void (*)(void))raw;
  entry();
  rt_print_flush();
  return 0;
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NY_UNUSED_FUNC __attribute__((unused))
#else
#define NY_UNUSED_FUNC
#endif

typedef struct ny_ir_stats_t {
  uint64_t funcs;
  uint64_t blocks;
  uint64_t insts;
  uint64_t allocas;
  uint64_t phis;
} ny_ir_stats_t;

static LLVMModuleRef ny_prepare_ir_dump_module(const ny_options *opt,
                                               LLVMModuleRef module);
static bool ny_is_llvm_special_global(const char *name);
static void ny_ensure_parent_dir_for_path(const char *path);

typedef struct {
  int64_t (*entry)(void);
} ny_native_jit_call_t;

typedef struct {
  const char *items[256];
  size_t len;
  bool overflow;
} ny_native_link_list_t;

static void ny_native_link_list_add(const char *library, void *raw) {
  ny_native_link_list_t *list = (ny_native_link_list_t *)raw;
  if (!list || !library || !*library)
    return;
  for (size_t i = 0; i < list->len; ++i)
    if (strcmp(list->items[i], library) == 0)
      return;
  if (list->len >= sizeof(list->items) / sizeof(list->items[0])) {
    list->overflow = true;
    return;
  }
  list->items[list->len++] = library;
}

static int ny_native_jit_safe_child(void *raw) {
  ny_native_jit_call_t *call = (ny_native_jit_call_t *)raw;
  if (!call || !call->entry)
    return 1;
  if (!ny_jit_prepare_execution((uint64_t)(uintptr_t)call->entry))
    return 1;
  int64_t result = call->entry();
  rt_print_flush();
  return (int)(result & 0xff);
}

static int ny_run_native_only(const program_t *prog, const ny_options *opt,
                              const char *output_path, bool execute,
                              bool remove_output) {
  if (!execute && opt && opt->emit_only && output_path && *output_path) {
    char err[512] = {0};
    if (!ny_pipeline_emit_program_object(prog, opt, output_path, "main", false,
                                         err, sizeof(err))) {
      NY_LOG_ERR("Native-only NYIR/object emission failed: %s\n",
                 err[0] ? err : "unsupported native shape");
      return 1;
    }
    return 0;
  }
  if (execute) {
    ny_tick_t t_jit_compile = opt->do_timing ? ny_ticks_now() : 0;
#ifndef _WIN32
    /*
     * Stencil cache: try to load previously-persisted self-contained code.
     */
    void *cached_handle = NULL;
    void (*cached_entry)(void) = NULL;
    if (ny_env_enabled_default_on("NYTRIX_STENCIL_CACHE") && prog->raw_src &&
        prog->raw_src_len > 0) {
      char *cache_path = ny_pipeline_native_cache_path(prog->raw_src, opt);
      if (cache_path) {
        ny_jit_stencil_cache_load(cache_path, &cached_handle, &cached_entry);
        free(cache_path);
      }
    }
    if (cached_entry) {
      if (opt->do_timing) {
        if (ny_dbg_json_enabled())
          ny_dbg_heuristic_event("PHASE_TIME",
                                 "JIT Compile 0.0000s (cached stencil)");
        else
          fprintf(stderr, "JIT Compile: 0.0000s (cached stencil)\n");
      }
      ny_native_jit_call_t call = {
          .entry = (int64_t (*)(void))cached_entry,
      };
      ny_tick_t t_jit_run = opt->do_timing ? ny_ticks_now() : 0;
      int rc = ny_safe_run_requested(&opt->safe_run)
                   ? ny_safe_run_call(&opt->safe_run, ny_native_jit_safe_child,
                                      &call,
                                      opt->input_file ? opt->input_file
                                                      : "native JIT workload")
                   : ny_native_jit_safe_child(&call);
      if (opt->do_timing)
        maybe_log_phase_time(true, "JIT Run:", t_jit_run);
      dlclose(cached_handle);
      return rc;
    }
#endif
    nyr_jit_image_t image = {0};
    char jit_err[512] = {0};
    if (!ny_pipeline_jit_compile(prog, opt, &image, jit_err, sizeof(jit_err))) {
      NY_LOG_ERR("Native in-memory JIT failed: %s\n",
                 jit_err[0] ? jit_err : "unsupported native shape");
      return 1;
    }
    if (opt->do_timing)
      maybe_log_phase_time(true, "JIT Compile:", t_jit_compile);
    ny_native_jit_call_t call = {
        .entry = (int64_t (*)(void))image.entry,
    };
    ny_tick_t t_jit_run = opt->do_timing ? ny_ticks_now() : 0;
    int rc =
        ny_safe_run_requested(&opt->safe_run)
            ? ny_safe_run_call(&opt->safe_run, ny_native_jit_safe_child, &call,
                               opt->input_file ? opt->input_file
                                               : "native JIT workload")
            : ny_native_jit_safe_child(&call);
    if (opt->do_timing)
      maybe_log_phase_time(true, "JIT Run:", t_jit_run);
    nyr_jit_image_free(&image);
    return rc;
  }
  char obj[4096], rto[4096], err[512] = {0};
  char obj_name[96], rto_name[96];
  snprintf(obj_name, sizeof(obj_name), "ny_native_only_%ld_%llu.o",
           (long)getpid(), (unsigned long long)ny_ticks_now());
  snprintf(rto_name, sizeof(rto_name), "ny_native_only_rt_%ld_%llu.o",
           (long)getpid(), (unsigned long long)ny_ticks_now());
  ny_join_path(obj, sizeof(obj), ny_get_temp_dir(), obj_name);
  ny_join_path(rto, sizeof(rto), ny_get_temp_dir(), rto_name);

  if (!ny_pipeline_emit_program_object(prog, opt, obj, "main", false, err,
                                       sizeof(err))) {
    NY_LOG_ERR("Native-only NYIR/object emission failed: %s\n",
               err[0] ? err : "unsupported native shape");
    return 1;
  }
  const char *cc = ny_builder_choose_cc();
  int rt_speed = opt->opt_size ? -1 : 0;
  if (!ny_builder_compile_runtime(cc, rto, NULL, opt->debug_symbols,
                                  opt->gprof == 1, rt_speed, false,
                                  opt->sanitize)) {
    NY_LOG_ERR("Native-only runtime compilation failed\n");
    unlink(obj);
    return 1;
  }
  ny_native_link_list_t links = {0};
  for (size_t i = 0; i < opt->link_libs.len; ++i)
    ny_native_link_list_add(opt->link_libs.data[i], &links);
  ny_pipeline_visit_program_links_for_options(prog, opt,
                                              ny_native_link_list_add, &links);
  if (links.overflow) {
    NY_LOG_ERR("Native-only link failed: too many source-level libraries\n");
    unlink(obj);
    unlink(rto);
    return 1;
  }
  bool linked = ny_builder_link(
      cc, obj, rto, NULL, NULL, 0, (const char *const *)opt->link_dirs.data,
      opt->link_dirs.len, links.items, links.len, output_path,
      opt->strip_override == 1, opt->debug_symbols, opt->gprof == 1,
      opt->sanitize);
  unlink(obj);
  unlink(rto);
  if (!linked) {
    NY_LOG_ERR("Native-only link failed\n");
    return 1;
  }

  if (!execute)
    return 0;

  const char *argv_exec[] = {output_path, NULL};
  int rc = ny_safe_run_requested(&opt->safe_run)
               ? ny_safe_run_spawn(&opt->safe_run, argv_exec,
                                   opt->input_file ? opt->input_file
                                                   : "native-only workload")
               : ny_exec_spawn(argv_exec);
  if (remove_output)
    unlink(output_path);
  return rc;
}

static const char *ny_dump_dir(const ny_options *opt);
static void ny_dump_path(char *out, size_t out_len, const ny_options *opt,
                         const char *name);
static void ny_write_ir_stats_file(const ny_options *opt, const char *name,
                                   LLVMModuleRef module);
static void ny_collect_ir_stats(LLVMModuleRef module, ny_ir_stats_t *out);

#include "cache.c"
#include "desugar.c"
#include "stage.c"
#include "tiny.c"
/*
 * Diagnostic and proof requests must execute their requested compiler stages.
 * Ordinary warm execution can reuse a compiler/configuration-keyed image.
 */
char *ny_pipeline_native_cache_path(const char *source, const ny_options *opt) {
#ifndef _WIN32
  if (!opt || !ny_should_use_jit_cache(opt) ||
      !ny_env_enabled_default_on("NYTRIX_STENCIL_CACHE") || opt->emit_only ||
      opt->output_file || opt->strict_types || opt->ownership_strict ||
      opt->trace_exec || opt->dump_tokens || opt->dump_ast || opt->expand ||
      opt->dump_llvm || opt->dump_funcs || opt->dump_symbols ||
      opt->dump_stats || opt->emit_ir_path || opt->emit_bc_path ||
      opt->emit_asm_path || opt->native_dump_ir || opt->callgraph ||
      opt->native_tier_report || opt->native_result_oracle ||
      opt->native_oracle_per_pass || opt->native_tv_seed_trials ||
      opt->nyr_run || opt->nyr_metadata_report || opt->nyr_pass_stats ||
      opt->nyr_ir_verify || opt->nyr_bisect || opt->nyr_fingerprint ||
      opt->nyr_flaky_check || opt->nyr_disable_pass || opt->nyr_stop_after ||
      opt->nyr_global_inst_cap || opt->nyr_pass_growth_factor ||
      opt->native_profile_use_path || opt->emit_report_path ||
      opt->verbosity >= 2)
    return NULL;
  char *path = ny_jit_stencil_cache_path(
      source, opt->opt_level, opt->native_backend_raw, opt->native_tier_raw,
      ny_stage_semantic_config_fingerprint(opt));
  if (ny_env_enabled("NYTRIX_TRACE_CACHE") && path)
    fprintf(stderr, "native image cache key: %s\n", path);
  return path;
#else
  (void)source;
  (void)opt;
  return NULL;
#endif
}

#ifndef _WIN32
static void ny_pipeline_promote_jit_cache(const ny_options *opt,
                                          const char *path, codegen_t *cg) {
  if (!opt || !cg || !cg->module || !path || !opt->run_jit ||
      !ny_env_enabled_default_on("NYTRIX_JIT_NATIVE_CACHE") ||
      ny_access(path, R_OK) == 0)
    return;
  ny_tick_t started = opt->do_timing ? ny_ticks_now() : 0;
  bool saved = ny_jit_native_cache_save(path, cg->module, opt->opt_level,
                                        (const char *const *)cg->links.data,
                                        cg->links.len);
  if (opt->verbose || ny_env_enabled("NYTRIX_TRACE_CACHE"))
    fprintf(stderr, "JIT native cache %s: %s\n", saved ? "saved" : "not saved",
            path);
  maybe_log_phase_time(opt->do_timing, "Native Cache:", started);
}
#endif

static void ensure_aot_entry(codegen_t *cg, LLVMValueRef script_fn) {
  if (!cg || !cg->module || !script_fn)
    return;
  LLVMTypeRef i32 = LLVMInt32TypeInContext(cg->ctx);
  LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);
  LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
  LLVMTypeRef ptrptr = LLVMPointerType(ptr, 0);
  LLVMTypeRef main_ty =
      LLVMFunctionType(i32, (LLVMTypeRef[]){i32, ptrptr, ptrptr}, 3, 0);
  LLVMValueRef existing_main = LLVMGetNamedFunction(cg->module, "main");
  LLVMValueRef user_main = NULL;
  fun_sig *user_main_sig = NULL;
  bool explicit_main_entry = ny_program_has_explicit_main_entry(cg, cg->prog);
  if (existing_main) {
    LLVMTypeRef existing_ty = LLVMGlobalGetValueType(existing_main);
    unsigned paramc = LLVMCountParamTypes(existing_ty);
    LLVMTypeRef param_types[3] = {0};
    if (paramc == 3)
      LLVMGetParamTypes(existing_ty, param_types);
    bool already_c_main = LLVMGetReturnType(existing_ty) == i32 &&
                          paramc == 3 && param_types[0] == i32 &&
                          param_types[1] == ptrptr && param_types[2] == ptrptr;
    if (already_c_main)
      return;
    user_main_sig = lookup_fun(cg, "main", 0);
    LLVMSetValueName2(existing_main, "_ny_user_main", strlen("_ny_user_main"));
    if (!explicit_main_entry)
      user_main = existing_main;
  }
  LLVMValueRef main_fn = LLVMAddFunction(cg->module, "main", main_ty);
  LLVMBasicBlockRef entry =
      LLVMAppendBasicBlockInContext(cg->ctx, main_fn, "entry");
  LLVMBuilderRef builder = LLVMCreateBuilderInContext(cg->ctx);
  LLVMPositionBuilderAtEnd(builder, entry);
  LLVMValueRef argc = LLVMGetParam(main_fn, 0);
  LLVMValueRef argv = LLVMGetParam(main_fn, 1);
  LLVMValueRef envp = LLVMGetParam(main_fn, 2);

  LLVMTypeRef set_args_ty =
      LLVMFunctionType(i64, (LLVMTypeRef[]){i32, ptrptr, ptrptr}, 3, 0);
  LLVMValueRef set_args_fn =
      LLVMGetNamedFunction(cg->module, "_ny_aot_set_args");
  if (!set_args_fn) {
    set_args_fn = LLVMAddFunction(cg->module, "_ny_aot_set_args", set_args_ty);
    LLVMSetLinkage(set_args_fn, LLVMExternalLinkage);
  }
  LLVMBuildCall2(builder, set_args_ty, set_args_fn,
                 (LLVMValueRef[]){argc, argv, envp}, 3, "");
  LLVMValueRef script_res = LLVMBuildCall2(
      builder, LLVMGlobalGetValueType(script_fn), script_fn, NULL, 0, "");
  LLVMValueRef status_i32 = NULL;
  LLVMTypeRef script_ret_ty =
      LLVMGetReturnType(LLVMGlobalGetValueType(script_fn));
  if (LLVMGetTypeKind(script_ret_ty) == LLVMIntegerTypeKind) {
    LLVMValueRef script_int =
        LLVMGetIntTypeWidth(script_ret_ty) == 64
            ? script_res
            : LLVMBuildSExtOrBitCast(builder, script_res, i64, "script_i64");
    LLVMValueRef script_status =
        LLVMBuildAShr(builder, script_int, LLVMConstInt(i64, 1, 0), "");
    status_i32 = LLVMBuildTrunc(builder, script_status, i32, "");
  } else {
    status_i32 = LLVMConstInt(i32, 0, false);
  }
  if (user_main) {
    LLVMTypeRef user_ty = LLVMGlobalGetValueType(user_main);
    unsigned user_argc = LLVMCountParamTypes(user_ty);
    if (user_argc == 0) {
      LLVMTypeRef user_ret_ty = LLVMGetReturnType(user_ty);
      LLVMValueRef user_res =
          LLVMBuildCall2(builder, user_ty, user_main, NULL, 0, "");
      LLVMTypeKind user_ret_kind = LLVMGetTypeKind(user_ret_ty);
      if (user_ret_kind == LLVMVoidTypeKind) {
        status_i32 = LLVMConstInt(i32, 0, false);
      } else if (user_ret_kind == LLVMIntegerTypeKind) {
        LLVMValueRef user_i64 =
            LLVMGetIntTypeWidth(user_ret_ty) == 64
                ? user_res
                : LLVMBuildSExtOrBitCast(builder, user_res, i64,
                                         "user_main_i64");
        bool raw_status =
            user_main_sig && user_main_sig->return_type &&
            *user_main_sig->return_type &&
            ny_is_native_abi_type_name(user_main_sig->return_type) &&
            !ny_type_is_tagged(user_main_sig->return_type);
        LLVMValueRef exit_i64 =
            raw_status
                ? user_i64
                : LLVMBuildAShr(builder, user_i64, LLVMConstInt(i64, 1, 0), "");
        status_i32 = LLVMBuildTrunc(builder, exit_i64, i32, "");
      } else if (user_ret_kind == LLVMPointerTypeKind) {
        LLVMValueRef raw =
            LLVMBuildPtrToInt(builder, user_res, i64, "user_main_ptr_i64");
        status_i32 = LLVMBuildTrunc(builder, raw, i32, "");
      } else if (user_ret_kind == LLVMFloatTypeKind ||
                 user_ret_kind == LLVMDoubleTypeKind) {
        status_i32 =
            LLVMBuildFPToSI(builder, user_res, i32, "user_main_fp_i32");
      }
    }
  }

  LLVMTypeRef flush_ty = LLVMFunctionType(i64, NULL, 0, 0);
  LLVMValueRef flush_fn = LLVMGetNamedFunction(cg->module, "rt_print_flush");
  if (!flush_fn)
    flush_fn = LLVMAddFunction(cg->module, "rt_print_flush", flush_ty);
  bool skip_cleanup = ny_env_enabled("NYTRIX_AOT_SKIP_CLEANUP") ||
                      (ny_codegen_speed_profile_enabled(cg) &&
                       !ny_env_enabled("NYTRIX_AOT_KEEP_CLEANUP"));
  if (skip_cleanup) {
    LLVMBuildCall2(builder, flush_ty, flush_fn, NULL, 0, "");
  } else {
    LLVMValueRef cleanup_fn =
        LLVMGetNamedFunction(cg->module, "rt_runtime_cleanup");
    if (!cleanup_fn) {
      LLVMTypeRef cleanup_ty = LLVMFunctionType(i64, NULL, 0, 0);
      cleanup_fn =
          LLVMAddFunction(cg->module, "rt_runtime_cleanup", cleanup_ty);
    }
    LLVMBuildCall2(builder, LLVMGlobalGetValueType(cleanup_fn), cleanup_fn,
                   NULL, 0, "");
  }
  LLVMBuildRet(builder, status_i32 ? status_i32 : LLVMConstInt(i32, 0, false));
  LLVMDisposeBuilder(builder);
}

static void maybe_log_phase_time(bool enabled, const char *label,
                                 ny_tick_t start_time) {
  if (!enabled)
    return;
  double seconds = ny_ticks_elapsed_sec(start_time);
  if (ny_dbg_json_enabled())
    ny_dbg_heuristic_event("PHASE_TIME", "%s %.9fs", label, seconds);
  else
    fprintf(stderr, "%-12s %.9fs\n", label, seconds);
}

typedef struct {
  ny_options *opt;
  bool supervised;
} ny_repl_session_ctx_t;

static int ny_run_repl_session(ny_options *opt, bool supervised) {
  ny_jit_init_native_once();
  LLVMLoadLibraryPermanently(NULL);
  int repl_batch = 0;
#ifdef _WIN32
  repl_batch = (_isatty(_fileno(stdin)) == 0);
#else
  repl_batch = (isatty(STDIN_FILENO) == 0);
#endif
  std_mode_t repl_std_mode = opt->no_std ? STD_MODE_NONE : opt->std_mode;
  if (repl_batch && ny_env_enabled("NYTRIX_REPL_BATCH_NO_STD") &&
      !opt->repl_explicit && !opt->std_mode_explicit) {
    const char *env_std = getenv("NYTRIX_REPL_STD");
    const char *env_no_std = getenv("NYTRIX_REPL_NO_STD");
    if ((!env_std || !*env_std) && (!env_no_std || !*env_no_std))
      repl_std_mode = STD_MODE_NONE;
  }
  char *repl_stdin_src = NULL;
  if (repl_batch && !opt->command_string) {
    char *stdin_src = ny_read_stdin_all();
    if (!stdin_src) {
      NY_LOG_ERR("Failed to read REPL stdin\n");
      return 1;
    }
    if (ny_env_enabled("NYTRIX_REPL_TRACE"))
      fprintf(stderr, "[repl-batch] bytes=%zu fast_candidate=%d\n",
              strlen(stdin_src), ny_repl_batch_can_fast_run(stdin_src) ? 1 : 0);
    if (ny_repl_batch_can_fast_run(stdin_src)) {
      if (ny_env_enabled("NYTRIX_REPL_TRACE"))
        fprintf(stderr, "[repl-batch] dispatch=run\n");
      ny_options run_opt = *opt;
      run_opt.mode = NY_MODE_RUN;
      run_opt.command_string = stdin_src;
      run_opt.input_file = NULL;
      if (supervised)
        memset(&run_opt.safe_run, 0, sizeof(run_opt.safe_run));
      int rc = ny_pipeline_run(&run_opt);
      free(stdin_src);
      return rc;
    }
    if (ny_env_enabled("NYTRIX_REPL_TRACE"))
      fprintf(stderr, "[repl-batch] dispatch=repl\n");
    repl_stdin_src = stdin_src;
  }
  ny_repl_set_std_mode(repl_std_mode);
  ny_repl_set_plain(opt->repl_plain ? 1 : 0);
  ny_repl_set_max_errors(opt->max_errors);
  ny_repl_run(opt->opt_level, opt->opt_pipeline,
              opt->command_string ? opt->command_string : repl_stdin_src,
              repl_batch, opt);
  free(repl_stdin_src);
  return 0;
}

static int ny_repl_safe_child(void *raw) {
  ny_repl_session_ctx_t *ctx = raw;
  return ny_run_repl_session(ctx->opt, ctx->supervised);
}

static bool handle_non_compile_modes(ny_options *opt, int *exit_code) {
  if (opt->mode == NY_MODE_VERSION) {
    printf("Nytrix %s\n", VERSION);
    *exit_code = 0;
    return true;
  }
  if (opt->mode == NY_MODE_VERSION_ID) {
    *exit_code = ny_print_version_id();
    return true;
  }
  if (opt->mode == NY_MODE_BUNDLE) {
    *exit_code = ny_bundle_save(opt);
    return true;
  }
  if (opt->mode == NY_MODE_CLEAN_CACHE) {
    int rc = ny_cache_clean();
    if (rc == 0)
      printf("Removed Nytrix cache: %s\n", ny_cache_root_dir());
    else
      fprintf(
          stderr,
          "warning: some Nytrix cache artifacts could not be removed from %s\n",
          ny_cache_root_dir());
    *exit_code = rc == 0 ? 0 : 1;
    return true;
  }
  if (opt->mode == NY_MODE_HELP) {
    if (opt->help_env)
      ny_options_usage_env(opt->argv0 ? opt->argv0 : "ny");
    else
      ny_options_usage(opt->argv0 ? opt->argv0 : "ny");
    *exit_code = 0;
    return true;
  }
  if (opt->mode == NY_MODE_REPL) {
    bool sanitize_active = opt->sanitize && *opt->sanitize;
    if (opt->command_string &&
        (opt->native_only || ny_safe_run_requested(&opt->safe_run) ||
         sanitize_active)) {
      ny_options run_opt = *opt;
      run_opt.mode = NY_MODE_RUN;
      if (opt->native_only) {
        /*
         * -c is represented as a one-shot REPL command by option parsing.
         * Native-only redirects it through the ordinary run pipeline, where
         * it must be executed rather than treated as a precompile request.
         */
        run_opt.emit_only = false;
        run_opt.run_aot = true;
        run_opt.run_jit = false;
      }
      *exit_code = ny_pipeline_run(&run_opt);
      return true;
    }
    if (ny_safe_run_requested(&opt->safe_run)) {
      ny_repl_session_ctx_t ctx = {.opt = opt, .supervised = true};
      *exit_code = ny_safe_run_call(&opt->safe_run, ny_repl_safe_child, &ctx,
                                    "interactive REPL");
      return true;
    }
    *exit_code = ny_run_repl_session(opt, false);
    return true;
  }
  return false;
}

static char *ny_normalize_command_source(const char *src) {
  if (!src)
    return ny_strdup("");

  size_t len = strlen(src);
  if (len > SIZE_MAX - 2)
    return NULL;
  char *out = (char *)malloc(len + 2);
  if (!out)
    return NULL;
  memcpy(out, src, len);
  size_t j = len;
  if (j == 0 || out[j - 1] != '\n')
    out[j++] = '\n';
  out[j] = '\0';
  return out;
}

static char *load_user_source(const ny_options *opt) {
  if (opt->command_string)
    return ny_normalize_command_source(opt->command_string);
  if (opt->input_file) {
    if (strncmp(opt->input_file, "http://", 7) == 0 ||
        strncmp(opt->input_file, "https://", 8) == 0) {
      return ny_read_url(opt->input_file);
    }
    return ny_read_file(opt->input_file);
  }
  return ny_strdup("fn main() { return 0\n }");
}

static bool ny_md_extract_name_eq(const char *a, size_t a_len, const char *b,
                                  size_t b_len) {
  if (!a || !b || a_len != b_len)
    return false;
  for (size_t i = 0; i < a_len; ++i) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
      return false;
  }
  return true;
}

static bool ny_md_extract_lang_matches(const char *langs, const char *lang,
                                       size_t lang_len) {
  if (!lang || lang_len == 0)
    return false;
  if (!langs || !*langs)
    langs = "ny,nytrix";
  const char *p = langs;
  while (*p) {
    while (*p == ',' || *p == ';' || isspace((unsigned char)*p))
      p++;
    const char *start = p;
    while (*p && *p != ',' && *p != ';' && !isspace((unsigned char)*p))
      p++;
    size_t len = (size_t)(p - start);
    if ((len == 3 && ny_md_extract_name_eq(start, len, "all", 3)) ||
        ny_md_extract_name_eq(start, len, lang, lang_len))
      return true;
  }
  return false;
}

static bool ny_md_parse_open_fence(const char *line, size_t line_len,
                                   char *out_ch, size_t *out_len,
                                   const char **out_lang,
                                   size_t *out_lang_len) {
  if (!line || !out_ch || !out_len || !out_lang || !out_lang_len)
    return false;
  size_t i = 0;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
    i++;
  if (i >= line_len || (line[i] != '`' && line[i] != '~'))
    return false;
  char ch = line[i];
  size_t n = 0;
  while (i + n < line_len && line[i + n] == ch)
    n++;
  if (n < 3)
    return false;
  i += n;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
    i++;
  size_t lang_start = i;
  while (i < line_len && !isspace((unsigned char)line[i]) && line[i] != '`' &&
         line[i] != '~')
    i++;
  *out_ch = ch;
  *out_len = n;
  *out_lang = line + lang_start;
  *out_lang_len = i - lang_start;
  return true;
}

static bool ny_md_is_close_fence(const char *line, size_t line_len, char ch,
                                 size_t fence_len) {
  if (!line || fence_len < 3)
    return false;
  size_t i = 0;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
    i++;
  size_t n = 0;
  while (i + n < line_len && line[i + n] == ch)
    n++;
  if (n < fence_len)
    return false;
  i += n;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
    i++;
  return i >= line_len;
}

static void ny_md_append_code_block_json(char **json, size_t *json_len,
                                         size_t *json_cap, bool *first,
                                         const char *lang, size_t lang_len,
                                         int start_line, int end_line,
                                         const char *code, size_t code_len) {
  if (!json || !json_len || !json_cap || !first)
    return;
  if (!*first)
    ny_stage_append(json, json_len, json_cap, ",");
  *first = false;
  ny_stage_append(json, json_len, json_cap, "{\"lang\":");
  char *lang_copy = ny_strndup(lang ? lang : "", lang_len);
  ny_stage_json_str(json, json_len, json_cap, lang_copy ? lang_copy : "");
  free(lang_copy);
  ny_stage_append(json, json_len, json_cap,
                  ",\"start_line\":%d,\"end_line\":%d,\"code\":", start_line,
                  end_line);
  char *code_copy = ny_strndup(code ? code : "", code_len);
  ny_stage_json_str(json, json_len, json_cap, code_copy ? code_copy : "");
  free(code_copy);
  ny_stage_append(json, json_len, json_cap, "}");
}

static int ny_run_code_extractor(const ny_options *opt) {
  char *src = load_user_source(opt);
  if (!src) {
    NY_LOG_ERR("Failed to read input for --extract-code\n");
    return 1;
  }

  const char *source_name =
      opt && opt->input_file ? opt->input_file : "<inline>";
  char *json = NULL;
  size_t json_len = 0, json_cap = 0;
  bool first_json = true;
  size_t match_count = 0;
  bool wrote_raw = false;
  if (opt && opt->extract_json) {
    ny_stage_append(&json, &json_len, &json_cap,
                    "{\"schema\":\"code_blocks.v1\",\"source\":");
    ny_stage_json_str(&json, &json_len, &json_cap, source_name);
    ny_stage_append(&json, &json_len, &json_cap, ",\"blocks\":[");
  }

  bool in_block = false;
  bool block_lang_ok = false;
  char fence_ch = 0;
  size_t fence_len = 0;
  const char *block_lang = NULL;
  size_t block_lang_len = 0;
  const char *code_start = NULL;
  int block_start_line = 0;

  const char *p = src;
  int line_no = 1;
  while (*p) {
    const char *line_start = p;
    while (*p && *p != '\n')
      p++;
    const char *line_end = p;
    size_t line_len = (size_t)(line_end - line_start);
    const char *next = (*p == '\n') ? p + 1 : p;

    if (!in_block) {
      const char *lang = NULL;
      size_t lang_len = 0;
      char open_ch = 0;
      size_t open_len = 0;
      if (ny_md_parse_open_fence(line_start, line_len, &open_ch, &open_len,
                                 &lang, &lang_len)) {
        in_block = true;
        fence_ch = open_ch;
        fence_len = open_len;
        block_lang = lang;
        block_lang_len = lang_len;
        block_lang_ok = ny_md_extract_lang_matches(
            opt ? opt->extract_lang : NULL, lang, lang_len);
        code_start = next;
        block_start_line = line_no;
      }
    } else if (ny_md_is_close_fence(line_start, line_len, fence_ch,
                                    fence_len)) {
      int block_end_line = line_no;
      bool selected =
          block_lang_ok && (!opt || opt->extract_line <= 0 ||
                            (opt->extract_line >= block_start_line &&
                             opt->extract_line <= block_end_line));
      if (selected) {
        size_t code_len = (size_t)(line_start - code_start);
        match_count++;
        if (opt && opt->extract_json) {
          ny_md_append_code_block_json(&json, &json_len, &json_cap, &first_json,
                                       block_lang, block_lang_len,
                                       block_start_line, block_end_line,
                                       code_start, code_len);
        } else {
          if (wrote_raw)
            fputc('\n', stdout);
          fwrite(code_start, 1, code_len, stdout);
          wrote_raw = true;
        }
      }
      in_block = false;
      block_lang_ok = false;
      fence_ch = 0;
      fence_len = 0;
      block_lang = NULL;
      block_lang_len = 0;
      code_start = NULL;
      block_start_line = 0;
    }

    if (*p == '\n') {
      p = next;
      line_no++;
    }
  }

  if (in_block && block_lang_ok) {
    bool selected =
        !opt || opt->extract_line <= 0 || opt->extract_line >= block_start_line;
    if (selected) {
      size_t code_len = strlen(code_start ? code_start : "");
      match_count++;
      if (opt && opt->extract_json) {
        ny_md_append_code_block_json(
            &json, &json_len, &json_cap, &first_json, block_lang,
            block_lang_len, block_start_line, line_no, code_start, code_len);
      } else {
        if (wrote_raw)
          fputc('\n', stdout);
        fwrite(code_start, 1, code_len, stdout);
        wrote_raw = true;
      }
    }
  }

  if (opt && opt->extract_json) {
    ny_stage_append(&json, &json_len, &json_cap, "],\"count\":%zu}\n",
                    match_count);
    fputs(json ? json
               : "{\"schema\":\"code_blocks.v1\",\"blocks\":[],\"count\":0}\n",
          stdout);
    free(json);
  }
  free(src);
  return (opt && opt->extract_json) || match_count > 0 ? 0 : 1;
}

static bool verify_module_if_needed(const ny_options *opt,
                                    LLVMModuleRef module) {
  if (!opt->verify_module)
    return true;
  char *err = NULL;
  if (LLVMVerifyModule(module, LLVMPrintMessageAction, &err)) {
    NY_LOG_ERR("Verification failed: %s\n", err);
    LLVMDisposeMessage(err);
    return false;
  }
  return true;
}

static NY_UNUSED_FUNC void ny_dump_ir_if_requested(LLVMModuleRef module,
                                                   const char *path,
                                                   const char *stage) {
  if (!module || !path || !*path)
    return;
  char *err = NULL;
  if (LLVMPrintModuleToFile(module, path, &err) != 0) {
    NY_LOG_WARN("failed to write %s IR to %s: %s\n", stage ? stage : "module",
                path, err ? err : "<unknown>");
    if (err)
      LLVMDisposeMessage(err);
    return;
  }
  if (err)
    LLVMDisposeMessage(err);
}

static void ny_dump_diagnose_ir_stage(const ny_options *opt,
                                      LLVMModuleRef module,
                                      const char *file_name,
                                      const char *stage) {
  if (!opt || !opt->dump_diagnose || !module || !file_name || !*file_name)
    return;
  ny_ensure_dir_recursive(ny_dump_dir(opt));
  char out_path[4096];
  ny_dump_path(out_path, sizeof(out_path), opt, file_name);
  LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, module);
  ny_dump_ir_if_requested(dump_mod ? dump_mod : module, out_path, stage);
  if (dump_mod)
    LLVMDisposeModule(dump_mod);
}

static void ny_dump_diagnose_finalize(const ny_options *opt,
                                      LLVMModuleRef module, int opt_level,
                                      const char *user_source) {
  if (!opt || !opt->dump_diagnose || !module)
    return;
  ny_ensure_dir_recursive(ny_dump_dir(opt));
  char asm_path[4096];
  char bc_path[4096];
  char summary_path[4096];
  char source_path[4096];
  char pre_ir_path[4096];
  char post_ir_path[4096];
  char stats_path[4096];
  ny_dump_path(asm_path, sizeof(asm_path), opt, "diag.s");
  ny_dump_path(bc_path, sizeof(bc_path), opt, "diag.bc");
  ny_dump_path(summary_path, sizeof(summary_path), opt, "diag.summary.txt");
  ny_dump_path(source_path, sizeof(source_path), opt, "diag.source.ny");
  ny_dump_path(pre_ir_path, sizeof(pre_ir_path), opt, "diag.pre.ll");
  ny_dump_path(post_ir_path, sizeof(post_ir_path), opt, "diag.post.ll");
  ny_dump_path(stats_path, sizeof(stats_path), opt, "diag.stats.txt");
  bool source_ok = user_source && ny_write_file(source_path, user_source,
                                                strlen(user_source)) == 0;
  LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, module);
  LLVMModuleRef art_mod = dump_mod ? dump_mod : module;
  bool asm_ok =
      ny_llvm_emit_file(art_mod, asm_path, LLVMAssemblyFile, opt_level);
  bool bc_ok = ny_reemit_bitcode_via_ir(art_mod, bc_path);
  ny_write_ir_stats_file(opt, "diag.stats.txt", art_mod);
  {
    const char *scope_name =
        (opt->dump_scope == NY_DUMP_SCOPE_LIB)
            ? "lib"
            : ((opt->dump_scope == NY_DUMP_SCOPE_BOTH) ? "both" : "program");
    char summary[1024];
    int n = snprintf(
        summary, sizeof(summary),
        "status=complete\ndump_dir=%s\ninput=%s\nbackend=%s\nscope=%s\nwarn_"
        "level=%d\n"
        "diag_compact=%d\nopt_level=%d\nsource=%s\npre_ir=%s\n"
        "post_ir=%s\nassembly=%s\nbitcode=%s\nstats=%s\n",
        ny_dump_dir(opt), opt->input_file ? opt->input_file : "<inline>",
        opt->native_backend_raw ? opt->native_backend_raw : "llvm", scope_name,
        opt->warn_level, opt->diag_compact ? 1 : 0, opt_level,
        source_ok ? "written" : "failed-or-unavailable",
        ny_access(pre_ir_path, R_OK) == 0 ? "written" : "unavailable",
        ny_access(post_ir_path, R_OK) == 0 ? "written" : "unavailable",
        asm_ok ? "written" : "failed", bc_ok ? "written" : "failed",
        ny_access(stats_path, R_OK) == 0 ? "written" : "failed");
    if (n > 0 && (size_t)n < sizeof(summary))
      ny_write_file(summary_path, summary, (size_t)n);
  }
  if (dump_mod)
    LLVMDisposeModule(dump_mod);
}

static void ny_dump_diagnose_reset(const ny_options *opt) {
  if (!opt || !opt->dump_diagnose)
    return;
  static const char *const stale[] = {
      "diag.source.ny", "diag.pre.ll",    "diag.post.ll",    "diag.s",
      "diag.bc",        "diag.stats.txt", "diag.summary.txt"};
  for (size_t i = 0; i < sizeof(stale) / sizeof(stale[0]); ++i) {
    char path[4096];
    ny_dump_path(path, sizeof(path), opt, stale[i]);
    (void)remove(path);
  }
}

static void ny_dump_diagnose_failure(const ny_options *opt,
                                     const char *user_source, int exit_code) {
  if (!opt || !opt->dump_diagnose)
    return;
  ny_ensure_dir_recursive(ny_dump_dir(opt));
  char source_path[4096];
  char summary_path[4096];
  ny_dump_path(source_path, sizeof(source_path), opt, "diag.source.ny");
  ny_dump_path(summary_path, sizeof(summary_path), opt, "diag.summary.txt");
  bool source_ok = user_source && ny_write_file(source_path, user_source,
                                                strlen(user_source)) == 0;
  char summary[1024];
  int n = snprintf(summary, sizeof(summary),
                   "status=failed\ninput=%s\nbackend=%s\nexit_code=%d\n"
                   "source=%s\n"
                   "hint=see compiler stderr for the failing stage\n",
                   opt->input_file ? opt->input_file : "<inline>",
                   opt->native_backend_raw ? opt->native_backend_raw : "llvm",
                   exit_code, source_ok ? "written" : "failed-or-unavailable");
  if (n > 0 && (size_t)n < sizeof(summary))
    (void)ny_write_file(summary_path, summary, (size_t)n);
}

static void ny_sanitize_platform_sections(LLVMModuleRef module) {
#ifdef __APPLE__
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    const char *sec = LLVMGetSection(fn);
    if (sec && strcmp(sec, "ny.std") == 0)
      LLVMSetSection(fn, "__TEXT,ny_std");
    else if (sec && strcmp(sec, "ny.user") == 0)
      LLVMSetSection(fn, "__TEXT,ny_user");
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
       gv = LLVMGetNextGlobal(gv)) {
    const char *sec = LLVMGetSection(gv);
    if (sec && strcmp(sec, "ny.std") == 0)
      LLVMSetSection(gv, "__DATA,ny_std");
    else if (sec && strcmp(sec, "ny.user") == 0)
      LLVMSetSection(gv, "__DATA,ny_user");
  }
#else
  (void)module;
#endif
}

static void ny_clear_origin_sections(LLVMModuleRef module) {
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    const char *sec = LLVMGetSection(fn);
    if (sec && (strcmp(sec, "ny.std") == 0 || strcmp(sec, "ny.user") == 0 ||
                strcmp(sec, "__TEXT,ny_std") == 0 ||
                strcmp(sec, "__TEXT,ny_user") == 0))
      LLVMSetSection(fn, "");
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
       gv = LLVMGetNextGlobal(gv)) {
    const char *sec = LLVMGetSection(gv);
    if (sec && (strcmp(sec, "ny.std") == 0 || strcmp(sec, "ny.user") == 0 ||
                strcmp(sec, "__TEXT,ny_std") == 0 ||
                strcmp(sec, "__TEXT,ny_user") == 0 ||
                strcmp(sec, "__DATA,ny_std") == 0 ||
                strcmp(sec, "__DATA,ny_user") == 0))
      LLVMSetSection(gv, "");
  }
}

static bool ny_ir_is_std_value(LLVMValueRef v) {
  if (!v)
    return false;
  const char *sec = LLVMGetSection(v);
  if (sec && *sec) {

    if (strcmp(sec, "ny.std") == 0 || strcmp(sec, "__TEXT,ny_std") == 0 ||
        strcmp(sec, "__DATA,ny_std") == 0)
      return true;
    if (strcmp(sec, "ny.user") == 0 || strcmp(sec, "__TEXT,ny_user") == 0 ||
        strcmp(sec, "__DATA,ny_user") == 0)
      return false;
  }
  /*
   * Source functions and globals carry explicit origin sections. A generated
   * symbol without provenance is unclassified; its spelling is not identity.
   */
  return false;
}

static void ny_ir_externalize_std_definitions(const ny_options *opt,
                                              LLVMModuleRef module) {
  (void)opt;
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;) {
    LLVMValueRef next_fn = LLVMGetNextFunction(fn);
    if (ny_ir_is_std_value(fn) && LLVMCountBasicBlocks(fn) > 0) {
      ny_llvm_clear_function(fn);
      LLVMSetLinkage(fn, LLVMExternalLinkage);
      LLVMSetVisibility(fn, LLVMDefaultVisibility);
    }
    fn = next_fn;
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;) {
    LLVMValueRef next_gv = LLVMGetNextGlobal(gv);
    if (ny_ir_is_std_value(gv) && !LLVMIsDeclaration(gv)) {
      LLVMSetInitializer(gv, NULL);
      LLVMSetLinkage(gv, LLVMExternalLinkage);
      LLVMSetVisibility(gv, LLVMDefaultVisibility);
      LLVMSetGlobalConstant(gv, false);
    }
    gv = next_gv;
  }
}

static void ny_ir_externalize_user_definitions(const ny_options *opt,
                                               LLVMModuleRef module) {
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;) {
    LLVMValueRef next_fn = LLVMGetNextFunction(fn);
    if (!ny_ir_is_std_value(fn) && LLVMCountBasicBlocks(fn) > 0) {
      ny_llvm_clear_function(fn);
      LLVMSetLinkage(fn, LLVMExternalLinkage);
      LLVMSetVisibility(fn, LLVMDefaultVisibility);
    }
    fn = next_fn;
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;) {
    LLVMValueRef next_gv = LLVMGetNextGlobal(gv);
    const char *name = LLVMGetValueName(gv);
    if (!ny_ir_is_std_value(gv) && !ny_is_llvm_special_global(name) &&
        !LLVMIsDeclaration(gv)) {
      LLVMSetInitializer(gv, NULL);
      LLVMSetLinkage(gv, LLVMExternalLinkage);
      LLVMSetVisibility(gv, LLVMDefaultVisibility);
      LLVMSetGlobalConstant(gv, false);
    }
    gv = next_gv;
  }
  LLVMPassBuilderOptionsRef popt = LLVMCreatePassBuilderOptions();
  if (popt) {
    bool enable_dce = true;
    if (opt && opt->opt_level == 0)
      enable_dce = false;
    if (enable_dce) {
      LLVMErrorRef perr = LLVMRunPasses(module, "globaldce", NULL, popt);
      if (perr) {
        char *msg = LLVMGetErrorMessage(perr);
        NY_LOG_WARN("IR user-prune pass failed: %s\n", msg ? msg : "<unknown>");
        if (msg)
          LLVMDisposeErrorMessage(msg);
      }
    }
    LLVMDisposePassBuilderOptions(popt);
  }
}

static LLVMModuleRef ny_prepare_ir_dump_module(const ny_options *opt,
                                               LLVMModuleRef module) {
  if (!module)
    return NULL;
  LLVMModuleRef dump_mod = LLVMCloneModule(module);
  if (!dump_mod)
    return NULL;
  if (!opt || !opt->debug_symbols)
    LLVMStripModuleDebugInfo(dump_mod);
  ny_dump_scope_t dump_scope = NY_DUMP_SCOPE_PROGRAM;
  if (opt)
    dump_scope = opt->dump_scope;
  if (dump_scope == NY_DUMP_SCOPE_PROGRAM)
    ny_ir_externalize_std_definitions(opt, dump_mod);
  else if (dump_scope == NY_DUMP_SCOPE_LIB)
    ny_ir_externalize_user_definitions(opt, dump_mod);
  return dump_mod;
}

static bool ny_is_llvm_special_global(const char *name) {
  return name && strncmp(name, "llvm.", 5) == 0;
}

static bool ny_should_preserve_symbol(const codegen_t *cg, const char *name,
                                      bool is_jit) {
  if (!name || !*name)
    return false;
  if (strcmp(name, "main") == 0 || strcmp(name, "_ny_top_entry") == 0)
    return true;
  if (strncmp(name, "__std_init", 10) == 0)
    return true;
  if (name[0] == '.')
    return true;
  if (is_jit)
    return false;

  const char *dot = strchr(name, '.');
  if (dot) {
    if (!cg)
      return true;
    for (size_t i = 0; i < cg->link_allowed_modules.len; i++) {
      const char *use_name = cg->link_allowed_modules.data[i];
      if (!use_name)
        continue;
      size_t use_len = strlen(use_name);
      if (strncmp(name, use_name, use_len) == 0 && name[use_len] == '.') {
        return true;
      }
    }
    return false;
  }
  return false;
}

static bool ny_should_preserve_aot_symbol(const codegen_t *cg,
                                          const char *name) {
  return ny_should_preserve_symbol(cg, name, false);
}

static bool ny_should_preserve_jit_symbol(const codegen_t *cg,
                                          const char *name) {
  return ny_should_preserve_symbol(cg, name, true);
}

static void ny_build_llvm_used(LLVMModuleRef module, const LLVMValueRef *values,
                               size_t count) {
  if (!module || !values || count == 0)
    return;
  LLVMTypeRef i8ptr =
      LLVMPointerType(LLVMInt8TypeInContext(LLVMGetModuleContext(module)), 0);
  VEC(LLVMValueRef) entries;
  vec_init(&entries);

  LLVMValueRef used = LLVMGetNamedGlobal(module, "llvm.used");
  if (used) {
    LLVMValueRef init = LLVMGetInitializer(used);
    if (init && LLVMIsAConstantArray(init)) {
      unsigned n = LLVMGetNumOperands(init);
      for (unsigned i = 0; i < n; i++) {
        LLVMValueRef op = LLVMGetOperand(init, i);
        if (op)
          vec_push(&entries, op);
      }
    }
  }

  for (size_t i = 0; i < count; i++) {
    LLVMValueRef v = values[i];
    if (!v)
      continue;
    LLVMValueRef cast = LLVMConstBitCast(v, i8ptr);
    vec_push(&entries, cast);
  }

  if (entries.len == 0) {
    vec_free(&entries);
    return;
  }
  if (entries.len > UINT_MAX) {
    vec_free(&entries);
    return;
  }

  LLVMTypeRef arr_ty = LLVMArrayType(i8ptr, (unsigned)entries.len);
  LLVMValueRef arr = LLVMConstArray(i8ptr, entries.data, (unsigned)entries.len);
  if (used)
    LLVMDeleteGlobal(used);
  used = LLVMAddGlobal(module, arr_ty, "llvm.used");
  LLVMSetLinkage(used, LLVMAppendingLinkage);
  LLVMSetSection(used, "llvm.metadata");
  LLVMSetGlobalConstant(used, true);
  LLVMSetInitializer(used, arr);
  vec_free(&entries);
}

static void ny_drop_jit_llvm_used_metadata(LLVMModuleRef module) {
  ny_drop_llvm_used_globals(module);
}

static void ny_prepare_internalize(LLVMModuleRef module, const ny_options *opt,
                                   const codegen_t *cg, bool is_jit) {
  if (!module || !opt)
    return;
  VEC(LLVMValueRef) preserve;
  vec_init(&preserve);
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (LLVMIsDeclaration(fn))
      continue;
    size_t name_len = 0;
    const char *name = LLVMGetValueName2(fn, &name_len);
    if (!name || name_len == 0)
      continue;
    if (is_jit ? ny_should_preserve_jit_symbol(cg, name)
               : ny_should_preserve_aot_symbol(cg, name)) {
      vec_push(&preserve, fn);
    }
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
       gv = LLVMGetNextGlobal(gv)) {
    if (LLVMIsDeclaration(gv))
      continue;
    size_t name_len = 0;
    const char *name = LLVMGetValueName2(gv, &name_len);
    if (!name || name_len == 0)
      continue;
    if (ny_is_llvm_special_global(name))
      continue;
    if (is_jit ? ny_should_preserve_jit_symbol(cg, name)
               : ny_should_preserve_aot_symbol(cg, name)) {
      vec_push(&preserve, gv);
    }
  }
  if (preserve.len > 0)
    ny_build_llvm_used(module, preserve.data, preserve.len);
  vec_free(&preserve);
}

static void run_dead_strip_if_needed(const ny_options *opt, codegen_t *cg,
                                     LLVMModuleRef module) {
  if (!opt || !module)
    return;
  bool is_aot = (opt->output_file != NULL);
  bool is_jit = opt->run_jit && (opt->mode != NY_MODE_REPL);
  if (!is_aot && !is_jit)
    return;

  if (is_jit && ny_jit_module_is_apple_arm64(module) &&
      !ny_env_enabled("NYTRIX_JIT_APPLE_ARM64_DCE")) {
    if (verbose_enabled >= 1)
      NY_LOG_INFO("%s", "JIT dead-strip: disabled for Apple arm64 MCJIT\n");
    return;
  }

  bool dce_enabled = false;
  if (is_aot || is_jit) {
    dce_enabled = (opt->opt_dce != 0) && !ny_env_enabled("NYTRIX_JIT_NO_DCE");
  }

  if (!dce_enabled)
    return;

  if (is_jit && !ny_env_enabled("NYTRIX_JIT_FORCE_DCE")) {
    size_t definitions = 0;
    for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
         fn = LLVMGetNextFunction(fn)) {
      if (!LLVMIsDeclaration(fn))
        definitions++;
    }
    size_t threshold = 512;
    const char *threshold_env = ny_env_str_nonempty("NYTRIX_JIT_DCE_MAX_FUNCS");
    if (threshold_env) {
      char *end = NULL;
      unsigned long parsed = strtoul(threshold_env, &end, 10);
      if (end && *end == '\0')
        threshold = (size_t)parsed;
    }
    if (definitions > threshold) {
      if (verbose_enabled >= 1)
        NY_LOG_INFO("JIT dead-strip: skipped for %zu definitions (limit=%zu); "
                    "lazy materialization is faster\n",
                    definitions, threshold);
      return;
    }
  }

  bool internalize_enabled = false;
  if (is_aot || is_jit) {
    internalize_enabled =
        (opt->opt_internalize != 0) && !ny_env_enabled("NYTRIX_JIT_NO_DCE");
  }

  if (internalize_enabled) {
    ny_prepare_internalize(module, opt, cg, is_jit);
    if (verbose_enabled >= 1)
      NY_LOG_INFO("%s internalize: enabled via llvm.used\n",
                  is_aot ? "AOT" : "JIT");
  } else if (verbose_enabled >= 1) {
    NY_LOG_INFO("%s internalize: DISABLED (opt->opt_internalize=%d)\n",
                is_jit ? "JIT" : "AOT", opt->opt_internalize);
  }
  const char *pipeline = NULL;
  if (internalize_enabled) {
    pipeline = getenv(is_aot ? "NYTRIX_AOT_INTERNALIZE_PIPELINE"
                             : "NYTRIX_JIT_INTERNALIZE_PIPELINE");
    if (!pipeline || !*pipeline)
      pipeline = "internalize,globaldce";
  } else {
    pipeline =
        getenv(is_aot ? "NYTRIX_AOT_DCE_PIPELINE" : "NYTRIX_JIT_DCE_PIPELINE");
    if (!pipeline || !*pipeline)
      pipeline = "globaldce";
  }
  LLVMPassBuilderOptionsRef popt = LLVMCreatePassBuilderOptions();
  if (!popt)
    return;
  LLVMErrorRef perr = LLVMRunPasses(module, pipeline, NULL, popt);
  if (perr) {
    char *msg = LLVMGetErrorMessage(perr);
    NY_LOG_WARN("AOT dead-strip pipeline '%s' failed: %s\n", pipeline,
                msg ? msg : "<unknown>");
    if (msg)
      LLVMDisposeErrorMessage(msg);
    if (strcmp(pipeline, "globaldce") != 0) {
      LLVMErrorRef ferr = LLVMRunPasses(module, "globaldce", NULL, popt);
      if (ferr) {
        char *fmsg = LLVMGetErrorMessage(ferr);
        NY_LOG_WARN("%s dead-strip fallback 'globaldce' failed: %s\n",
                    is_aot ? "AOT" : "JIT", fmsg ? fmsg : "<unknown>");
        if (fmsg)
          LLVMDisposeErrorMessage(fmsg);
      } else if (verbose_enabled >= 2) {
        NY_LOG_V2("%s dead-strip passes: globaldce\n", is_aot ? "AOT" : "JIT");
      }
    }
  } else if (verbose_enabled >= 2) {
    NY_LOG_V2("%s dead-strip passes: %s\n", is_aot ? "AOT" : "JIT", pipeline);
  }
  LLVMDisposePassBuilderOptions(popt);
}

typedef VEC(char *) ny_link_lib_vec;

static bool ny_link_lib_basename(const char *lib, const char **base_out,
                                 size_t *len_out) {
  if (!lib || strncmp(lib, "lib", 3) != 0 || !base_out || !len_out)
    return false;
  const char *base = lib + 3;
  size_t len = strlen(base);
  const char *dot = strstr(base, ".so");
#ifdef __APPLE__
  const char *dylib = strstr(base, ".dylib");
  if (dylib && dylib > base && (!dot || dylib < dot))
    dot = dylib;
#endif
  if (dot && dot > base)
    len = (size_t)(dot - base);
  if (len == 0)
    return false;
  *base_out = base;
  *len_out = len;
  return true;
}

static bool ny_link_lib_vec_contains_exact(const ny_link_lib_vec *libs,
                                           const char *name) {
  if (!libs || !name)
    return false;
  for (size_t i = 0; i < libs->len; i++) {
    const char *existing = libs->data[i];
    if (existing && strcmp(existing, name) == 0)
      return true;
  }
  return false;
}

static bool ny_link_lib_vec_contains_dash_l(const ny_link_lib_vec *libs,
                                            const char *name, size_t len) {
  if (!libs || !name)
    return false;
  for (size_t i = 0; i < libs->len; i++) {
    const char *existing = libs->data[i];
    if (existing && existing[0] == '-' && existing[1] == 'l' &&
        strncmp(existing + 2, name, len) == 0 && existing[2 + len] == '\0')
      return true;
  }
  return false;
}

static void ny_link_lib_vec_add_option(ny_link_lib_vec *libs, const char *lib) {
  const char *name = NULL;
  size_t len = 0;
  if (ny_link_lib_basename(lib, &name, &len) && len < 256) {
    char buf[260];
    snprintf(buf, sizeof(buf), "-l%.*s", (int)len, name);
    vec_push(libs, ny_strdup(buf));
    return;
  }
  vec_push(libs, ny_strdup(lib));
}

static void ny_link_lib_vec_add_codegen(ny_link_lib_vec *libs,
                                        const char *name) {
  if (!name)
    return;
  if (name[0] == '-' || strchr(name, '/') || strchr(name, '\\')) {
    if (!ny_link_lib_vec_contains_exact(libs, name))
      vec_push(libs, ny_strdup(name));
    return;
  }
  const char *lib_name = name;
  size_t lib_len = strlen(name);
  const char *base = NULL;
  size_t base_len = 0;
  if (ny_link_lib_basename(name, &base, &base_len)) {
    lib_name = base;
    lib_len = base_len;
  }
  if (!ny_link_lib_vec_contains_dash_l(libs, lib_name, lib_len)) {
    char buf[260];
    snprintf(buf, sizeof(buf), "-l%.*s", (int)lib_len, lib_name);
    vec_push(libs, ny_strdup(buf));
  }
}

static void ny_link_lib_vec_merge(ny_link_lib_vec *libs, const ny_options *opt,
                                  const codegen_t *cg) {
  for (size_t i = 0; opt && i < opt->link_libs.len; i++)
    ny_link_lib_vec_add_option(libs, opt->link_libs.data[i]);
  for (size_t i = 0; cg && i < cg->links.len; i++)
    ny_link_lib_vec_add_codegen(libs, cg->links.data[i]);
}

static void ny_link_lib_vec_dispose(ny_link_lib_vec *libs) {
  if (!libs)
    return;
  for (size_t i = 0; i < libs->len; i++)
    free(libs->data[i]);
  vec_free(libs);
}

static bool ny_pipeline_configure_fast_compiler(ny_options *opt) {
  if (!opt)
    return false;
  bool low_overhead = ny_env_enabled("NYTRIX_FAST_COMPILE");
  bool fast_compiler = low_overhead || ny_env_enabled("NYTRIX_FAST_COMPILER");
  if (!fast_compiler)
    return false;
  opt->opt_level = low_overhead ? 0 : 1;
  opt->opt_pipeline = NULL;
  opt->opt_loops = 0;
  opt->opt_autotune = 0;
  opt->verify_module = false;
  opt->debug_symbols = false;
  ny_setenv("NYTRIX_JIT_CACHE", "1", 0);
  ny_setenv("NYTRIX_JIT_OPT_LEVEL", low_overhead ? "0" : "1", 0);
  if (!ny_jit_module_is_apple_arm64(NULL))
    ny_setenv("NYTRIX_JIT_FAST_ISEL", "1", 0);
  return true;
}

static void ny_pipeline_configure_worker(ny_options *opt) {
  if (!opt || !getenv("NYTRIX_WORKER") || getenv("NYTRIX_WORKER_OPT"))
    return;
  opt->opt_level = 0;
  opt->opt_pipeline = NULL;
  opt->opt_dce = 0;
}

static bool ny_pipeline_prepare_aot_run_output(ny_options *opt,
                                               const char **output_path,
                                               char *path, size_t path_len) {
  if (!opt || !output_path || !path || path_len == 0 || !opt->run_aot)
    return false;
  if (*output_path && **output_path)
    return false;
  /*
   * Native-only execution runs in-memory JIT and needs no output file.
   * Forcing the temp output here also forces emit_only, which hijacks the
   * native-only branch into emit-object-and-return: programs silently do
   * nothing with exit 0.  Leave output unset so native-only executes.
   */
  if (opt->native_only)
    return false;
#ifdef _WIN32
  snprintf(path, path_len, "%s/ny_aot_run_%d.exe", ny_get_temp_dir(),
           (int)getpid());
#else
  snprintf(path, path_len, "%s/ny_aot_run_%d", ny_get_temp_dir(),
           (int)getpid());
#endif
  opt->output_file = path;
  *output_path = opt->output_file;
  opt->emit_only = true;
  opt->run_jit = false;
  return true;
}

typedef struct {
  std_mode_t mode;
  const char *prebuilt_path;
  char *src;
  char *auto_bc_cache;
  const char *bc_cache;
  bool use_bc_cache;
  bool auto_bc_cache_needs_links;
  bool has_local;
} ny_pipeline_std_load;

static void ny_pipeline_scan_std_imports(char **uses, size_t use_count,
                                         bool *has_local,
                                         bool *has_project_std) {
  if (has_local)
    *has_local = false;
  if (has_project_std)
    *has_project_std = false;
  for (size_t i = 0; uses && i < use_count; i++) {
    const char *u = uses[i];
    bool is_registered_std_or_lib = ny_std_has_module_or_package(u);
    if (has_project_std && ny_use_name_is_project_core_module(u))
      *has_project_std = true;
    if (has_local && !is_registered_std_or_lib) {
      *has_local = true;
      break;
    }
  }
}

static bool ny_pipeline_load_stdlib(ny_options *opt, char **uses,
                                    size_t use_count, char *std_cache_path,
                                    size_t std_cache_path_len,
                                    ny_pipeline_std_load *std) {
  if (!opt || !std)
    return false;
  memset(std, 0, sizeof(*std));
  std->mode = opt->std_mode;
  const char *configured_core_path = "build/core.ny";
#ifdef NYTRIX_CORE_PATH
  configured_core_path = NYTRIX_CORE_PATH;
#endif
  std->prebuilt_path =
      resolve_std_path(opt->std_path ? opt->std_path : configured_core_path);
  if (opt->no_std)
    std->mode = STD_MODE_NONE;
  /*
   * A shared bytecode path is a cache source, not an instruction to load the
   * whole stdlib into programs that import no std modules.
   */
  if ((std->mode == STD_MODE_DEFAULT || std->mode == STD_MODE_BC) &&
      use_count == 0)
    std->mode = STD_MODE_NONE;

  bool has_project_std = false;
  ny_pipeline_scan_std_imports(uses, use_count, &std->has_local,
                               &has_project_std);
  bool std_sources_ok = ny_std_sources_available();
  bool prebuilt_ok =
      std->prebuilt_path && ny_access(std->prebuilt_path, R_OK) == 0;
  bool prefer_prebuilt = ny_env_enabled("NYTRIX_STD_PREFER_PREBUILT");
  bool prebuilt_preferred =
      (std->mode == STD_MODE_FULL ||
       (prefer_prebuilt && std->mode == STD_MODE_DEFAULT)) &&
      !std->has_local;
  bool prebuilt_required = !std_sources_ok;

  if (std->mode != STD_MODE_NONE && opt->std_bc_path &&
      ny_access(opt->std_bc_path, R_OK) == 0) {
    std->bc_cache = opt->std_bc_path;
    std->use_bc_cache = true;
  } else {
    const char *env_bc = getenv("NYTRIX_STD_BC_CACHE");
    if (std->mode != STD_MODE_NONE && env_bc && *env_bc &&
        ny_access(env_bc, R_OK) == 0) {
      std->bc_cache = env_bc;
      std->use_bc_cache = true;
    } else if (opt->native_backend == NY_NATIVE_BACKEND_LLVM &&
               std->mode != STD_MODE_NONE && !std->has_local &&
               !has_project_std &&
               ny_env_enabled_default_on("NYTRIX_STD_BC_CACHE_AUTO")) {
      std->auto_bc_cache = ny_std_bc_cache_path(
          std->prebuilt_path, (const char *const *)uses, use_count,
          (int)std->mode, opt->debug_symbols,
          (unsigned long)ny_std_latest_mtime(), opt->argv0);
      if (std->auto_bc_cache && ny_access(std->auto_bc_cache, R_OK) == 0) {
        bool cache_ok = true;
        if (ny_std_bc_cache_preverify_enabled()) {
          LLVMContextRef cache_ctx = LLVMContextCreate();
          cache_ok =
              cache_ctx && ny_verify_bitcode(cache_ctx, std->auto_bc_cache);
          if (cache_ctx)
            LLVMContextDispose(cache_ctx);
        }
        if (cache_ok) {
          bool needs_link_sidecar =
              opt->output_file && !ny_output_path_is_object(opt->output_file);
          if (needs_link_sidecar &&
              !ny_std_bc_cache_has_links(std->auto_bc_cache)) {
            std->auto_bc_cache_needs_links = true;
          } else {
            std->bc_cache = std->auto_bc_cache;
            std->use_bc_cache = true;
          }
        } else if (!cache_ok) {
          (void)unlink(std->auto_bc_cache);
        }
      }
    }
  }

  /*
   * A selected stdlib bitcode module already represents the prebuilt source.
   * Loading core.ny as well defeats the shared cache: the full bundle is parsed
   * and code-generated for every fixture even though codegen later links the
   * bitcode. Keep the source fallback only when no usable bitcode was chosen.
   */
  if (!std->use_bc_cache && std->mode != STD_MODE_NONE && prebuilt_ok &&
      (prebuilt_preferred || prebuilt_required)) {
    if (verbose_enabled)
      NY_LOG_INFO("Using prebuilt core.ny: %s\n", std->prebuilt_path);
    std->src = ny_read_file(std->prebuilt_path);
    if (!std->src && verbose_enabled) {
      NY_LOG_WARN("Failed to read prebuilt core.ny: %s (falling back)\n",
                  std->prebuilt_path);
    }
  }

  /*
   * `-no-std` disables the standard library, but explicit local module
   * imports still need to be bundled into the parsed source.  The loader
   * already skips registered stdlib dependencies when mode is NONE; keep
   * user modules available so local `use "./module.ny"` behaves the same
   * with and without the standard library.
   */
  if ((std->mode != STD_MODE_NONE || std->has_local) && !std->src) {
    bool use_std_cache =
        !std->has_local && ny_env_enabled_default_on("NYTRIX_STD_CACHE");
    uint64_t std_cache_sig = 0;
    if (use_std_cache && std_cache_path && std_cache_path_len > 0) {
      std_cache_sig = ny_build_std_cache_path(
          opt, (const char *const *)uses, use_count, std->mode,
          std->prebuilt_path, std_cache_path, std_cache_path_len);
      if (std_cache_path[0] != '\0' && ny_access(std_cache_path, R_OK) == 0) {
        std->src = ny_read_file(std_cache_path);
        if (std->src && std->src[0] == '\0') {
          free(std->src);
          std->src = NULL;
          (void)unlink(std_cache_path);
        }
        if (std->src && std_cache_sig) {
          char expect[128];
          int nw =
              snprintf(expect, sizeof(expect), "; ny_std_cache_v11 %016llx\n",
                       (unsigned long long)std_cache_sig);
          if (nw <= 0 || (size_t)nw >= sizeof(expect) ||
              strncmp(std->src, expect, (size_t)nw) != 0) {
            free(std->src);
            std->src = NULL;
            (void)unlink(std_cache_path);
          } else {
            /*
             * The on-disk header is cache metadata, not Nytrix source. Keep
             * the parser and downstream artifact keys identical whether the
             * stdlib was generated just now or restored from this cache.
             */
            memmove(std->src, std->src + nw, strlen(std->src + nw) + 1);
          }
        }
        if (std->src && verbose_enabled >= 1)
          NY_DBG(NY_DBG_PASS, "PHASE stdlib-source-cache hit bytes=%zu path=%s",
                 strlen(std->src), std_cache_path);
      }
    }
    if (!std->src) {
      if (use_std_cache && std_cache_path && std_cache_path[0] != '\0')
        NY_DBG(NY_DBG_PASS, "PHASE stdlib-source-cache miss path=%s",
               std_cache_path);
      std->src = ny_build_std_source((const char **)uses, use_count, std->mode,
                                     opt->verbose, opt->input_file);
      if (std->src && use_std_cache && std_cache_path &&
          std_cache_path[0] != '\0' && std_cache_sig) {
        char header[128];
        int hn =
            snprintf(header, sizeof(header), "; ny_std_cache_v11 %016llx\n",
                     (unsigned long long)std_cache_sig);
        if (hn > 0 && (size_t)hn < sizeof(header)) {
          size_t sl = strlen(std->src);
          if ((size_t)hn > SIZE_MAX - sl - 1)
            return false;
          char *wrapped = malloc((size_t)hn + sl + 1);
          if (wrapped) {
            memcpy(wrapped, header, (size_t)hn);
            memcpy(wrapped + hn, std->src, sl + 1);
            (void)ny_write_file_atomic(std_cache_path, wrapped,
                                       (size_t)hn + sl);
            free(wrapped);
          }
        }
      }
    }
  }

  if (std->mode == STD_MODE_NONE || std->src)
    return true;

  NY_LOG_ERR("Could not load core.ny or standard library source files.\n");
  NY_LOG_ERR("Checked paths: %s and %s/std\n",
             std->prebuilt_path ? std->prebuilt_path : "NULL", ny_src_root());
  free(std->auto_bc_cache);
  std->auto_bc_cache = NULL;
  return false;
}

static char *ny_pipeline_join_sources(const char *std_src, const char *user_src,
                                      const char *parse_name,
                                      size_t *user_len_out,
                                      size_t *split_pos_out) {
  if (user_len_out)
    *user_len_out = 0;
  if (split_pos_out)
    *split_pos_out = 0;
  if (!user_src)
    return NULL;

  size_t slen = std_src ? strlen(std_src) : 0;
  size_t ulen = strlen(user_src);
  size_t line_directive_len = 0;
  if (parse_name && parse_name[0] != '<') {
    size_t parse_len = strlen(parse_name);
    if (parse_len > SIZE_MAX - (sizeof("#line 1 \"\"\n") - 1)) {
      NY_LOG_ERR("Source file name too large for #line directive\n");
      return NULL;
    }
    line_directive_len = parse_len + (sizeof("#line 1 \"\"\n") - 1);
  }

  size_t total = slen;
  if (ulen > SIZE_MAX - total || line_directive_len > SIZE_MAX - total - ulen ||
      4 > SIZE_MAX - total - ulen - line_directive_len) {
    NY_LOG_ERR("Source code too large to concatenate\n");
    return NULL;
  }
  total += ulen + line_directive_len + 4;

  char *source = malloc(total);
  if (!source) {
    NY_LOG_ERR("Failed to allocate combined source input\n");
    return NULL;
  }

  char *ptr = source;
  if (std_src) {
    memcpy(ptr, std_src, slen);
    ptr += slen;
    if (ptr > source && ptr[-1] != '\n')
      *ptr++ = '\n';
    if (split_pos_out)
      *split_pos_out = (size_t)(ptr - source);
  }
  if (line_directive_len > 0) {
    int n =
        snprintf(ptr, line_directive_len + 1, "#line 1 \"%s\"\n", parse_name);
    if (n < 0 || (size_t)n != line_directive_len) {
      free(source);
      NY_LOG_ERR("Failed to build #line directive\n");
      return NULL;
    }
    ptr += line_directive_len;
  }
  memcpy(ptr, user_src, ulen + 1);
  if (user_len_out)
    *user_len_out = ulen;
  return source;
}

/*
 * Env-guarded incremental recompilation inventory for an already-loaded
 * program.  Walks the top-level module declarations and runs the file-level
 * incremental driver over their on-disk source paths.  Default-off so the
 * well-tested compile path is entirely unchanged; enabled with
 * NYTRIX_INCREMENTAL=1.
 */
#if !defined(_WIN32)
static bool ny_pipeline_incremental_add_file(char ***files, size_t *count,
                                             size_t *cap, const char *path) {
  if (!files || !count || !cap || !path || !*path ||
      strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0)
    return true;
  for (size_t i = 0; i < *count; ++i)
    if (strcmp((*files)[i], path) == 0)
      return true;
  if (*count == *cap) {
    size_t next = *cap ? *cap * 2 : 8;
    if (next <= *cap) /* overflow */
      return false;
    char **grown = realloc(*files, next * sizeof(*grown));
    if (!grown)
      return false;
    *files = grown;
    *cap = next;
  }
  (*files)[*count] = ny_strdup(path);
  if (!(*files)[*count])
    return false;
  ++*count;
  return true;
}

static void ny_pipeline_run_incremental(program_t *prog, ny_options *opt) {
  if (!prog || !opt || !ny_env_enabled("NYTRIX_INCREMENTAL"))
    return;
  size_t count = 0;
  size_t cap = 0;
  char **files = NULL;
  if (opt->input_file &&
      !ny_pipeline_incremental_add_file(&files, &count, &cap, opt->input_file))
    goto cleanup;
  for (size_t i = 0; i < prog->body.len; i++) {
    const stmt_t *s = prog->body.data[i];
    if (!s || s->kind != NY_S_MODULE)
      continue;
    if (!ny_pipeline_incremental_add_file(&files, &count, &cap,
                                          s->tok.filename))
      goto cleanup;
  }
  if (count == 0)
    goto cleanup;
  ny_incremental_config_t cfg = {0};
  char project_root[4096];
  cfg.enable_incremental = true;
  ny_dir_name(project_root, sizeof(project_root),
              opt->input_file ? opt->input_file : ".");
  cfg.project_root = project_root;
  ny_incremental_result_t res =
      ny_incremental_compile((const char **)files, count, &cfg);
  for (size_t i = 0; i < res.recompile_count; i++)
    fprintf(stderr, "[incremental] recompile: %s\n",
            res.recompile_files[i] ? res.recompile_files[i] : "?");
  fprintf(stderr, "[incremental] graph nodes=%zu recompile=%zu success=%d\n",
          count, res.recompile_count, (int)res.success);
  ny_incremental_free_result(&res);
cleanup:
  for (size_t i = 0; i < count; i++)
    free(files[i]);
  free(files);
  ny_incremental_shutdown();
}
#else
static void ny_pipeline_run_incremental(program_t *prog, ny_options *opt) {
  (void)prog;
  (void)opt;
}
#endif

int ny_pipeline_run(ny_options *opt) {
  int exit_code = 0;
  bool debug_bundle_written = false;
  ny_tick_t pipeline_prof_t0 = ny_ticks_now();
  ny_lookup_prof_register_atexit();
  if (handle_non_compile_modes(opt, &exit_code))
    return exit_code;
  if (opt && opt->emit_report_path)
    ny_setenv("NYTRIX_EMIT_REPORT", opt->emit_report_path, 1);
  ny_diag_configure(opt ? opt->warn_level : 1, opt ? opt->diag_compact : false);
  if (opt && opt->warn_shadow)
    ny_diag_set_shadow_warnings(true);
  /*
   * --warn-all is warn level 2 (the table-driven option sets warn_level):
   * full lint output, shadow warnings un-suppressed, and every warning
   * counts as an error for the exit code (CI mode).
   */
  if (opt && opt->warn_level >= 2) {
    ny_diag_set_shadow_warnings(true);
    ny_diag_set_warn_as_error(true);
  }
  if (opt && opt->dump_diagnose)
    ny_ensure_dir_recursive(ny_dump_dir(opt));
  ny_dump_diagnose_reset(opt);
  ny_pipeline_configure_worker(opt);
  bool fast_compiler = ny_pipeline_configure_fast_compiler(opt);
  verbose_enabled = opt->verbose;
  ny_tick_t t_start = 0;
  ny_tick_t t0 = 0;
  if (opt->do_timing) {
    t_start = ny_ticks_now();
    t0 = ny_ticks_now();
  }
  if (opt->extract_code)
    return ny_run_code_extractor(opt);
  if (!ny_safe_run_requested(&opt->safe_run) &&
      ny_try_fast_command_string(opt, t_start))
    return 0;
  bool debug_output = opt->trace_exec || debug_enabled;
  /*
   * Debug verbosity tells a story line-by-line; an animated progress bar
   * overwrites it. Keep the bar for quiet tiers, defer to the trace
   * narrative from `-v` up.
   */
  bool show_progress = !opt->no_progress && !debug_output &&
                       !ny_env_enabled("NYTRIX_NO_PROGRESS") &&
                       !ny_dbg_verbosity() &&
                       (opt->progress || ny_progress_enabled_from_env()
#ifndef _WIN32
                        || isatty(STDERR_FILENO)
#endif
                       );
  long progress_total = 8;
  if (opt->output_file)
    progress_total += 2;
  else if (opt->run_jit)
    progress_total += 1;
  if (show_progress) {
    if (opt->progress)
      ny_progress_force();
    ny_progress_start("nytrix compile", progress_total);
  }
  char *user_src = NULL;
  char *std_src = NULL;
  char *source = NULL;
  char **uses = NULL;
  size_t use_count = 0;
  arena_t *arena = NULL;
  char aot_cache_path[4096] = {0};
  char std_cache_path[4096] = {0};
  program_t prog = {0};
  codegen_t cg;
  memset(&cg, 0, sizeof(cg));
  const char *parse_name = opt->input_file ? opt->input_file : "<inline>";
  const char *output_path = opt->output_file;
  LLVMValueRef script_fn = NULL;
  char aot_run_path[4096] = {0};
  bool aot_run_temp = false;
  bool loaded_from_cache = false;
  char *jit_cache_file = NULL;
  char *type_errors_json = NULL;
#ifndef _WIN32
  char *native_cache_file = NULL;
  void *native_cache_handle = NULL;
  void (*native_cache_entry)(void) = NULL;
#endif
  aot_run_temp = ny_pipeline_prepare_aot_run_output(
      opt, &output_path, aot_run_path, sizeof(aot_run_path));
#ifdef _WIN32
  char output_win[4096];
  if (output_path)
    output_path =
        ny_windows_output_path(output_path, output_win, sizeof(output_win));
#endif
  ny_progress_node_t progress_node = ny_progress_task_begin("read source", 1);
  user_src = load_user_source(opt);
  if (!user_src) {
    if (opt->input_file) {
      if (strncmp(opt->input_file, "http://", 7) == 0 ||
          strncmp(opt->input_file, "https://", 8) == 0) {
        NY_LOG_ERR("Failed to fetch URL '%s'\n", opt->input_file);
      } else {
        NY_LOG_ERR("Failed to read file '%s'\n", opt->input_file);
      }
    } else {
      NY_LOG_ERR("Failed to allocate source input\n");
    }
    if (show_progress)
      ny_progress_finish();
    return 1;
  }
  ny_progress_task_end(progress_node);
  if (!ny_pipeline_apply_ny_desugar(opt, &user_src, parse_name)) {
    if (show_progress)
      ny_progress_finish();
    return 1;
  }
  maybe_log_phase_time(opt->do_timing, "Read file:", t0);
  if (opt->do_timing)
    t0 = ny_ticks_now();
  progress_node = ny_progress_task_begin("scan imports", 1);
  uses = ny_collect_import_names(user_src, opt->input_file, &use_count);
  ny_progress_task_end(progress_node);
  maybe_log_phase_time(opt->do_timing, "Scan imports:", t0);
  if (opt->do_timing)
    t0 = ny_ticks_now();
  if (ny_env_enabled("NYTRIX_TRACE_IMPORTS")) {
    ny_dbg_heuristic_event("IMPORTS", "discovered=%zu", use_count);
    for (size_t i = 0; i < use_count; i++) {
      ny_dbg_heuristic_event("IMPORT", "%s", uses[i] ? uses[i] : "<null>");
    }
  }
  ny_tick_t t_std = opt->do_timing ? ny_ticks_now() : 0;
  ny_pipeline_std_load std_load = {0};
  std_mode_t std_mode = STD_MODE_NONE;
  const char *prebuilt_path = NULL;
  char *auto_std_bc_cache = NULL;
  const char *std_bc_cache = NULL;
  bool use_std_bc_cache = false;
  bool auto_std_bc_cache_needs_links = false;
  bool has_local = false;
  bool auto_std_bc_cache_saved = false;
  bool reuse_semantic_artifact = false;
  bool write_compile_caches = ny_should_write_compile_caches(opt);
  progress_node = ny_progress_task_begin("load stdlib", 1);
  if (!ny_pipeline_load_stdlib(opt, uses, use_count, std_cache_path,
                               sizeof(std_cache_path), &std_load)) {
    ny_progress_task_end(progress_node);
    exit_code = 1;
    goto exit_success;
  }
  ny_progress_task_end(progress_node);
  std_mode = std_load.mode;
  prebuilt_path = std_load.prebuilt_path;
  std_src = std_load.src;
  auto_std_bc_cache = std_load.auto_bc_cache;
  std_bc_cache = std_load.bc_cache;
  use_std_bc_cache = std_load.use_bc_cache;
  auto_std_bc_cache_needs_links = std_load.auto_bc_cache_needs_links;
  has_local = std_load.has_local;
  maybe_log_phase_time(opt->do_timing, "Core load:", t_std);
  size_t ulen = 0;
  size_t split_pos = 0;
  source = ny_pipeline_join_sources(std_src, user_src, parse_name, &ulen,
                                    &split_pos);
  if (!source) {
    exit_code = 1;
    goto exit_success;
  }
  /*
   * `--native-only` promises the Nytrix-owned NYIR/object execution path.
   * LLVM is a selectable backend for ordinary JIT/AOT compilation, but it
   * cannot satisfy that promise. Reject this contradictory pair before any
   * AOT cache lookup can return an artifact built by another backend.
   */
  if (opt->native_only && opt->native_backend == NY_NATIVE_BACKEND_LLVM) {
    NY_LOG_ERR("--native-only requires a Nytrix-owned backend; choose x86_64, "
               "aarch64, or another supported native backend\n");
    exit_code = 1;
    goto exit_success;
  }
  if (opt->use_artifact_path && *opt->use_artifact_path) {
    char artifact_err[256] = {0};
    if (!ny_stage_artifact_reusable_for_semantics(opt->use_artifact_path,
                                                  source, opt, artifact_err,
                                                  sizeof(artifact_err))) {
      NY_LOG_ERR("Semantic artifact reuse failed: %s\n", artifact_err);
      exit_code = 1;
      goto exit_success;
    }
    reuse_semantic_artifact = true;
    if (opt->verbose)
      fprintf(stderr, "Semantic artifact reuse: %s\n", opt->use_artifact_path);
  }
  if (opt->verify_artifact_path && *opt->verify_artifact_path) {
    char artifact_err[256] = {0};
    if (!ny_stage_artifact_matches_source_file(opt->verify_artifact_path,
                                               source, artifact_err,
                                               sizeof(artifact_err))) {
      NY_LOG_ERR("Semantic artifact validation failed: %s\n", artifact_err);
      exit_code = 1;
    } else {
      printf("Semantic artifact verified: %s\n", opt->verify_artifact_path);
    }
    goto exit_success;
  }
#ifndef _WIN32
  if (opt->run_jit && ny_should_use_jit_cache(opt) &&
      ny_jit_native_cache_enabled() && !opt->dump_ast && !opt->expand &&
      !opt->dump_llvm && !opt->emit_ir_path && !opt->emit_bc_path &&
      !opt->dump_tokens && !opt->dump_on_error && !opt->dump_diagnose) {
    char *early_bc = ny_jit_cache_path(
        source, prebuilt_path, 0, opt->opt_level, opt->opt_dce,
        opt->opt_internalize, opt->debug_symbols,
        (unsigned long)ny_std_latest_mtime(),
        ny_stage_semantic_config_fingerprint(opt));
    if (early_bc && ny_cache_artifact_manifest_valid(early_bc, source)) {
      char *early_so = ny_jit_native_cache_path(early_bc);
      if (early_so) {
        if (ny_jit_native_cache_load(early_so, &native_cache_handle,
                                     &native_cache_entry)) {
          if (opt->verbose || ny_env_enabled("NYTRIX_TRACE_CACHE"))
            fprintf(stderr, "JIT native cache hit (early): %s\n", early_so);
          loaded_from_cache = true;
          free(early_so);
          free(early_bc);
          goto skip_compilation;
        }
        if (ny_env_enabled("NYTRIX_JIT_NATIVE_CACHE"))
          fprintf(stderr, "JIT native cache miss (early): native artifact is "
                          "absent or invalid\n");
        free(early_so);
      } else {
        fprintf(stderr, "JIT native cache miss (early): cannot derive native "
                        "artifact path\n");
      }
    } else {
      if (ny_env_enabled("NYTRIX_JIT_NATIVE_CACHE"))
        fprintf(
            stderr,
            "JIT native cache miss (early): no validated bitcode manifest\n");
    }
    if (early_bc)
      free(early_bc);
  }
#endif
  if (ny_should_use_aot_cache(opt)) {
    ny_build_aot_cache_path(opt, source, parse_name, prebuilt_path, output_path,
                            aot_cache_path, sizeof(aot_cache_path));
    /*
     * A valid executable alone is insufficient: its sidecar proves it was
     * produced from these exact post-stdlib source bytes.
     */
    if (aot_cache_path[0] != '\0' && ny_access(aot_cache_path, R_OK) == 0 &&
        ny_cache_artifact_manifest_valid(aot_cache_path, source) &&
        strcmp(aot_cache_path, output_path) != 0) {
      if (ny_valid_native_artifact(aot_cache_path)) {
        if (ny_copy_file(aot_cache_path, output_path) == 0 &&
            ny_valid_native_artifact(output_path)) {
          NY_LOG_V2("AOT cache hit: %s\n", aot_cache_path);
#ifdef _WIN32
          NY_LOG_SUCCESS("Saved EXE: %s\n", output_path);
#else
          NY_LOG_SUCCESS("Saved ELF: %s\n", output_path);
#endif
          ny_trace_file_size("emit_native_cache_hit", output_path);
          if (opt->run_aot) {
            const char *argv_exec[] = {output_path, NULL};
            int rc = ny_exec_spawn(argv_exec);
            if (rc != 0)
              exit_code = rc;
            if (aot_run_temp)
              (void)unlink(output_path);
          }
          goto exit_success;
        }
      } else {
        (void)unlink(aot_cache_path);
      }
    }
  }
#ifndef _WIN32
  /*
   * Stencil cache early check: skip parse/semantic/lower when a cached
   * self-contained .so already exists for this source+opt_level.
   */
  if (ny_env_enabled_default_on("NYTRIX_STENCIL_CACHE") && opt->native_only &&
      !opt->strict_types && !opt->emit_ir_path && !opt->emit_only &&
      !opt->dump_tokens && !opt->dump_ast && !opt->expand && !opt->dump_llvm &&
      !opt->emit_bc_path && !opt->dump_on_error && !opt->dump_diagnose) {
    char *stencil_path = ny_pipeline_native_cache_path(source, opt);
    if (stencil_path) {
      nyr_jit_image_t cached_image = {0};
      char cache_err[256] = {0};
      if (nyr_jit_cache_load(stencil_path, ny_pipeline_runtime_services_get(),
                             &cached_image, cache_err, sizeof(cache_err))) {
        ny_tick_t t_run = opt->do_timing ? ny_ticks_now() : 0;
        ny_native_jit_call_t call = {
            .entry = (int64_t (*)(void))cached_image.entry,
        };
        int rc =
            ny_safe_run_requested(&opt->safe_run)
                ? ny_safe_run_call(&opt->safe_run, ny_native_jit_safe_child,
                                   &call, parse_name)
                : ny_native_jit_safe_child(&call);
        if (opt->do_timing) {
          fprintf(stderr, "JIT Compile: 0.0000s (cached native image)\n");
          fprintf(stderr, "JIT Run:     %.6fs\n", ny_ticks_elapsed_sec(t_run));
        }
        if (ny_env_enabled("NYTRIX_TRACE_CACHE"))
          fprintf(stderr, "native image cache hit: %s.obj\n", stencil_path);
        nyr_jit_image_free(&cached_image);
        free(stencil_path);
        exit_code = rc;
        goto exit_success;
      }
      void *cached_handle = NULL;
      void (*cached_entry)(void) = NULL;
      if (ny_jit_stencil_cache_load(stencil_path, &cached_handle,
                                    &cached_entry)) {
        ny_native_jit_call_t call = {
            .entry = (int64_t (*)(void))cached_entry,
        };
        int rc = ny_safe_run_requested(&opt->safe_run)
                     ? ny_safe_run_call(&opt->safe_run,
                                        ny_native_jit_safe_child, &call,
                                        opt->input_file ? opt->input_file
                                                        : "native JIT workload")
                     : ny_native_jit_safe_child(&call);
        dlclose(cached_handle);
        free(stencil_path);
        exit_code = rc;
        goto exit_success;
      }
      free(stencil_path);
    }
  }
#endif
  if (opt->dump_tokens) {
    /*
     * Compact one-flow token dump: `line:col kind 'lexeme'` packed per
     * line, keywords/operators named, counts summarized at the end.
     */
    lexer_t lx;
    lexer_init(&lx, source, parse_name);
    unsigned tcount = 0, tlines = 0, tcol = 0;
    for (;;) {
      token_t t = lexer_next(&lx);
      const char *kind = "tok";
      char kindbuf[24];
      switch (t.kind) {
      case NY_T_IDENT:
        kind = "ident";
        break;
      case NY_T_NUMBER:
        kind = "number";
        break;
      case NY_T_STRING:
        kind = "string";
        break;
      case NY_T_FSTRING:
        kind = "fstring";
        break;
      case NY_T_FN:
        kind = "fn";
        break;
      case NY_T_RETURN:
        kind = "return";
        break;
      case NY_T_IF:
        kind = "if";
        break;
      case NY_T_ELSE:
        kind = "else";
        break;
      case NY_T_WHILE:
        kind = "while";
        break;
      case NY_T_FOR:
        kind = "for";
        break;
      case NY_T_IN:
        kind = "in";
        break;
      case NY_T_TRUE:
        kind = "true";
        break;
      case NY_T_FALSE:
        kind = "false";
        break;
      case NY_T_USE:
        kind = "use";
        break;
      case NY_T_DEF:
        kind = "def";
        break;
      case NY_T_MUT:
        kind = "mut";
        break;
      case NY_T_NIL:
        kind = "nil";
        break;
      case NY_T_MODULE:
        kind = "module";
        break;
      case NY_T_LPAREN:
        kind = "(";
        break;
      case NY_T_RPAREN:
        kind = ")";
        break;
      case NY_T_LBRACE:
        kind = "{";
        break;
      case NY_T_RBRACE:
        kind = "}";
        break;
      case NY_T_LBRACK:
        kind = "[";
        break;
      case NY_T_RBRACK:
        kind = "]";
        break;
      case NY_T_COMMA:
        kind = ",";
        break;
      case NY_T_COLON:
        kind = ":";
        break;
      case NY_T_SEMI:
        kind = ";";
        break;
      case NY_T_DOT:
        kind = ".";
        break;
      case NY_T_PLUS:
        kind = "+";
        break;
      case NY_T_MINUS:
        kind = "-";
        break;
      case NY_T_STAR:
        kind = "*";
        break;
      case NY_T_SLASH:
        kind = "/";
        break;
      case NY_T_EQ:
        kind = "==";
        break;
      case NY_T_ASSIGN:
        kind = "=";
        break;
      case NY_T_EOF:
        kind = "eof";
        break;
      default:
        snprintf(kindbuf, sizeof(kindbuf), "kind%d", t.kind);
        kind = kindbuf;
        break;
      }
      if (tcount && tcol + 24 > 100) {
        fputc('\n', stderr);
        fputs("  ", stderr);
        tcol = 2;
      }
      if (t.kind == NY_T_EOF) {
        fprintf(stderr, "eof");
        tcount++;
        break;
      }
      /*
       * Keywords and punctuation name themselves; only value-carrying
       * tokens (ident/number/string) need the lexeme spelled out.
       */
      int written;
      if (t.kind == NY_T_IDENT || t.kind == NY_T_NUMBER ||
          t.kind == NY_T_STRING || t.kind == NY_T_FSTRING)
        written = fprintf(stderr, "%d:%d %s '%.*s'  ", t.line, t.col, kind,
                          (int)t.len, t.lexeme);
      else
        written = fprintf(stderr, "%d:%d %s  ", t.line, t.col, kind);
      if (written > 0)
        tcol += (unsigned)written;
      tcount++;
    }
    fputc('\n', stderr);
    fprintf(stderr, "%u tokens\n", tcount);
    (void)tlines;
    (void)tlines;
    (void)tlines;
    goto exit_success;
  }
  ny_tick_t t_parse = opt->do_timing ? ny_ticks_now() : 0;
  parser_t parser;
  arena = (arena_t *)malloc(sizeof(arena_t));
  memset(arena, 0, sizeof(arena_t));
  parser_init_with_arena(&parser, source, std_src ? "<stdlib>" : parse_name,
                         arena);
  if (std_src)
    parser_add_module_features(&parser, NY_MODULE_FEATURE_STDLIB);
  if (opt->max_errors >= 0)
    parser.error_limit = opt->max_errors;
  if (std_src) {
    parser.lex.split_pos = split_pos;
    ny_sym_id split_file_id = ny_intern_cstr(parse_name);
    parser.lex.split_filename =
        split_file_id ? ny_intern_get(split_file_id) : parse_name;
  }
  size_t parser_bytes = strlen(source);
  size_t stdlib_bytes = std_src ? strlen(std_src) : 0;
  const char *action = opt->emit_only ? "emit"
                       : opt->run_jit ? "jit"
                       : opt->run_aot ? "aot"
                                      : "compile";
  if (opt->native_only) {
    NY_DBG(NY_DBG_PASS,
           "PIPELINE plan action=%s route=native "
           "stages=parse→NYIR→machine-code→link/run semantic=%s",
           action, "shared-type-trait");
  } else {
    NY_DBG(NY_DBG_PASS,
           "PIPELINE plan action=%s "
           "stages=parse→semantic-prepare→HM→trait→ABI→IR→backend→link/run",
           action);
  }
  NY_DBG(NY_DBG_PASS,
         "PHASE parse begin file=%s user_bytes=%zu stdlib_bytes=%zu "
         "parser_bytes=%zu",
         parse_name ? parse_name : "<input>", ulen, stdlib_bytes, parser_bytes);
  ny_pipe_trace_phase_memory("parse.begin");
  progress_node = ny_progress_task_begin("parse", 1);
  prog = parse_program(&parser);
  ny_progress_task_end(progress_node);
  NY_DBG(NY_DBG_PASS,
         "PHASE parse complete ast=program statements=%zu errors=%d",
         prog.body.len, parser.error_count);
  ny_pipe_trace_phase_memory("parse.complete");
  maybe_log_phase_time(opt->do_timing, "Parsing:", t_parse);
  if (parser.had_error) {
    NY_LOG_ERR("Compilation failed: %d errors\n", parser.error_count);
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_PARSE, parse_name,
                               "parse failed", parser.error_count);
    dump_debug_bundle(opt, user_src, NULL, "parse", "parse failed");
    debug_bundle_written = opt->dump_on_error;
    exit_code = 1;
    goto exit_success;
  }
  if (ny_pipe_check_member_splices(&prog)) {
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_PARSE, parse_name,
                               "parse failed", 1);
    dump_debug_bundle(opt, user_src, NULL, "parse", "parse failed");
    debug_bundle_written = opt->dump_on_error;
    exit_code = 1;
    goto exit_success;
  }
  ny_ast_verify_program(&prog, "parse");
  ny_pipe_apply_main_requests(&prog, opt);
  /*
   * Run the opt-in dependency fingerprint pass after parsing, while the
   * parser still owns the canonical module filenames.  The pass only updates
   * the graph and invalidates stale artifacts; it does not alter the normal
   * semantic/codegen path.
   */
  ny_pipeline_run_incremental(&prog, opt);
  if (ny_stop_after_is(opt, NY_STOP_AFTER_PARSE)) {
    ny_stage_emit_artifact(opt, NY_STOP_AFTER_PARSE, &prog, NULL, parse_name,
                           NULL, true);
    goto exit_success;
  }
  if (opt->expand) {
    char *report = ny_ast_expand_report(&prog, parse_name, opt->expand_only,
                                        opt->explain_specialization,
                                        opt->meta_trace, opt->expand_json);
    if (report) {
      fputs(report, stdout);
      rt_free((int64_t)(uintptr_t)report);
    }
    goto exit_success;
  }
  if (opt->dump_ast && !opt->dump_ast_typed) {
    ny_ast_dump_tree(stdout, &prog, parse_name, opt->dump_ast_filter, false);
    goto exit_success;
  }
  if (!ny_validate_loop_compile_ranges(&prog)) {
    dump_debug_bundle(opt, user_src, NULL, "proof",
                      "loop range proof validation failed");
    debug_bundle_written = opt->dump_on_error;
    exit_code = 1;
    goto exit_success;
  }
  if (opt->safe_mode && !ny_safe_mode_validate_raw_memory(&prog)) {
    dump_debug_bundle(opt, user_src, NULL, "safety",
                      "safe-mode validation failed");
    debug_bundle_written = opt->dump_on_error;
    exit_code = 1;
    goto exit_success;
  }

  /*
   * Native-only avoids LLVM code emission, not source-language validation.
   * Run shared type and trait checks before selecting the native backend so
   * strict types, operator declarations, and trait laws have the same contract.
   */
  if (opt->native_only) {
    codegen_init(&cg, &prog, arena, "nytrix-native-semantics");
    cg.source_main_file = parse_name;
    cg.type_solver = opt->type_solver_raw ? opt->type_solver_raw : "auto";
    cg.proof_solver = opt->proof_solver_raw ? opt->proof_solver_raw : "auto";
    cg.strict_types = opt->strict_types;
    if (opt->use_refinement_interface_path) {
      char interface_error[256] = {0};
      if (!ny_refinement_interface_import(
              &cg, opt->use_refinement_interface_path, interface_error,
              sizeof(interface_error))) {
        NY_LOG_ERR("Refinement interface import failed: %s\n", interface_error);
        codegen_dispose(&cg);
        exit_code = 1;
        goto exit_success;
      }
    }
    if (opt->use_effect_interface_path) {
      char interface_error[256] = {0};
      if (!ny_effect_interface_import(&cg, opt->use_effect_interface_path,
                                      interface_error,
                                      sizeof(interface_error))) {
        NY_LOG_ERR("Effect interface import failed: %s\n", interface_error);
        codegen_dispose(&cg);
        exit_code = 1;
        goto exit_success;
      }
    }
    codegen_collect_links(&cg, &prog);
    codegen_prepare(&cg);
    if (cg.had_error) {
      codegen_dispose(&cg);
      exit_code = 1;
      goto exit_success;
    }
    int semantic_errors = ny_type_pipeline_validate_semantics(
        &prog, &cg, parse_name, false, NY_TYPE_PIPELINE_STAGE_TRAIT, NULL, true,
        NULL);
    if (semantic_errors == 0 && opt->emit_refinement_interface_path) {
      char interface_error[256] = {0};
      if (!ny_refinement_interface_export(
              &cg, opt->emit_refinement_interface_path, interface_error,
              sizeof(interface_error))) {
        NY_LOG_ERR("Refinement interface export failed: %s\n", interface_error);
        semantic_errors = 1;
      }
    }
    if (semantic_errors == 0 && opt->emit_effect_interface_path) {
      char interface_error[256] = {0};
      if (!ny_effect_interface_export(&cg, opt->emit_effect_interface_path,
                                      interface_error,
                                      sizeof(interface_error))) {
        NY_LOG_ERR("Effect interface export failed: %s\n", interface_error);
        semantic_errors = 1;
      }
    }
    if (semantic_errors == 0 && opt->dump_ast_typed)
      ny_ast_dump_tree(stdout, &prog, parse_name, opt->dump_ast_filter, true);
    codegen_dispose(&cg);
    memset(&cg, 0, sizeof(cg));
    if (semantic_errors > 0) {
      exit_code = 1;
      goto exit_success;
    }
    if (opt->dump_ast)
      goto exit_success;
  }

  /*
   * The shared semantic context has been disposed above. Native-only now
   * lowers without LLVM IR emission, MCJIT, or LLVM object fallback.
   */
  if (opt->native_backend != NY_NATIVE_BACKEND_LLVM && opt->emit_bc_path &&
      !opt->emit_ir_path) {
    ny_options native_bc = *opt;
    native_bc.native_dump_ir = true;
    native_bc.nyr_dump_bin = true;
    native_bc.nyr_dump_text = false;
    native_bc.nyr_dump_bin_path = opt->emit_bc_path;
    char native_bc_err[512] = {0};
    if (!ny_pipeline_dump_ir_for_program(&prog, &native_bc, native_bc_err,
                                         sizeof(native_bc_err))) {
      NY_LOG_ERR("Native bytecode emission failed: %s\n",
                 native_bc_err[0] ? native_bc_err : "unknown error");
      exit_code = 1;
      goto exit_success;
    }
    if (!opt->run_aot) {
      exit_code = 0;
      goto exit_success;
    }
    exit_code = ny_run_native_only(&prog, opt, output_path, true, aot_run_temp);
    goto exit_success;
  }

  if (opt->native_only && !opt->emit_ir_path) {
    NY_DBG(NY_DBG_PASS, "ROUTE native-only: NYIR lowering and native backend; "
                        "LLVM codegen skipped");
    if (opt->emit_asm_path) {
      ny_ensure_parent_dir_for_path(opt->emit_asm_path);
      char native_asm_err[512] = {0};
      if (!ny_pipeline_emit_asm(&prog, opt, opt->emit_asm_path, native_asm_err,
                                sizeof(native_asm_err))) {
        NY_LOG_ERR("Failed to write native assembly to %s: %s\n",
                   opt->emit_asm_path,
                   native_asm_err[0] ? native_asm_err : "unknown error");
        exit_code = 1;
        goto exit_success;
      }
      ny_trace_file_size("emit_asm", opt->emit_asm_path);
    }
    if (opt->native_dump_ir) {
      char native_dump_err[512] = {0};
      if (!ny_pipeline_dump_ir_for_program(&prog, opt, native_dump_err,
                                           sizeof(native_dump_err))) {
        NY_LOG_ERR("Native-only precompile failed: %s\n",
                   native_dump_err[0] ? native_dump_err : "unknown error");
        exit_code = 1;
        goto exit_success;
      }
    }
    /*
     * `--nyir-run` has already executed the program in the NYIR VM while
     * preparing the dump. Treat that result as final instead of running the
     * same user code again through the native JIT (and repeating side effects).
     * Keep dump requests composable with native execution, matching the
     * ordinary pipeline's behavior.
     */
    if (opt->nyr_run && !(opt->nyr_dump_text || opt->nyr_dump_bin ||
                          (opt->nyr_run_bin_path && opt->nyr_run_bin_path[0] &&
                           opt->nyr_dump_bin))) {
      exit_code = 0;
      goto exit_success;
    }
    /*
     * A tier/callgraph report is itself a successful cross-target compile
     * request: the report plan is built and written from NYIR lowering, so do
     * not let a later link/object step for a non-host target fail the run.
     */
    nyr_target_request_t target_request = {0};
    bool host_executable = ny_options_target_request(opt, &target_request) &&
                           nyr_target_is_host_executable(&target_request);
    if (opt->native_tier_report || opt->callgraph) {
      char report_err[512] = {0};
      bool report_ok = true;
      if (opt->native_tier_report)
        report_ok = ny_pipeline_write_tier_report(&prog, opt, report_err,
                                                  sizeof(report_err));
      if (report_ok && opt->callgraph && !host_executable) {
        report_ok = ny_pipeline_write_callgraph_for_program(
            &prog, opt, report_err, sizeof(report_err));
      }
      if (!report_ok) {
        NY_LOG_ERR("Native-only diagnostic report failed: %s\n",
                   report_err[0] ? report_err : "unknown error");
        exit_code = 1;
        goto exit_success;
      }
      if (!host_executable) {
        exit_code = 0;
        goto exit_success;
      }
    }
    /*
     * `-emit-only` is a compile/object request, even when the test harness
     * also selected --native-only.  Do not execute the freshly emitted
     * program (which is especially important for extern-only fixtures).
     */
    if (opt->emit_only) {
      exit_code = output_path
                      ? ny_run_native_only(&prog, opt, output_path, false, true)
                      : 0;
      goto exit_success;
    }
    if (opt->native_result_oracle) {
      char native_oracle_err[512] = {0};
      if (!ny_pipeline_result_oracle(&prog, opt, native_oracle_err,
                                     sizeof(native_oracle_err))) {
        NY_LOG_ERR("Native result oracle failed: %s\n",
                   native_oracle_err[0] ? native_oracle_err : "unknown error");
        exit_code = 1;
        goto exit_success;
      }
      /*
       * The oracle has already executed both the NYIR reference and native
       * result harness. Running the emitted program again would execute user
       * side effects twice and leak the script's tail value into the process
       * status (for example an f64-returning expression exits with 8).
       */
      exit_code = 0;
      goto exit_success;
    }
    exit_code = ny_run_native_only(&prog, opt, output_path, true, true);
    goto exit_success;
  }

  if ((opt->nyr_dump_text || opt->nyr_dump_bin || opt->nyr_ir_verify) &&
      !opt->native_only) {
    NY_LOG_WARN("--nyir-dump/--nyir-verify operate on the native NIR pipeline "
                "and are ignored on the JIT/LLVM path; add --native-only "
                "(or --native-backend x86_64 --native-only) to see them\n");
  }

  ny_tick_t t_codegen = opt->do_timing ? ny_ticks_now() : 0;
  NY_LOG_V2("Initializing codegen_t for module 'nytrix'\n");
  codegen_init(&cg, &prog, arena, "nytrix");
  cg.source_main_file = parse_name;
  cg.type_solver = opt->type_solver_raw ? opt->type_solver_raw : "auto";
  cg.proof_solver = opt->proof_solver_raw ? opt->proof_solver_raw : "auto";
  if (opt->use_refinement_interface_path) {
    char interface_error[256] = {0};
    if (!ny_refinement_interface_import(&cg, opt->use_refinement_interface_path,
                                        interface_error,
                                        sizeof(interface_error))) {
      NY_LOG_ERR("Refinement interface import failed: %s\n", interface_error);
      exit_code = 1;
      goto exit_success;
    }
  }
  if (opt->use_effect_interface_path) {
    char interface_error[256] = {0};
    if (!ny_effect_interface_import(&cg, opt->use_effect_interface_path,
                                    interface_error, sizeof(interface_error))) {
      NY_LOG_ERR("Effect interface import failed: %s\n", interface_error);
      exit_code = 1;
      goto exit_success;
    }
  }
  cg.c_frontend = opt->c_frontend_raw ? opt->c_frontend_raw : "auto";
  cg.strict_types = opt->strict_types;
  /*
   * Advisory provenance checking is deliberately separate from hard errors
   * and automatic cleanup. This keeps ordinary compilation informative without
   * changing the program's lifetime or heap policy.
   */
  cg.ownership_enabled =
      opt->ownership || opt->ownership_strict || opt->borrow_check;
  cg.ownership_strict = opt->ownership_strict;
  cg.ownership_runtime_cleanup = opt->ownership;
  if (opt->safe_mode) {
    cg.strict_diagnostics = true;
    cg.ownership_enabled = true;
    cg.ownership_strict = true;
    cg.ownership_runtime_cleanup = true;
  }
  codegen_collect_links(&cg, &prog);

  cg.debug_symbols =
      opt->debug_symbols &&
      (opt->output_file || opt->emit_ir_path || opt->emit_bc_path ||
       opt->emit_asm_path || opt->emit_only);
  cg.debug_opt_level = opt->opt_level;
  if (cg.debug_symbols && parse_name && *parse_name)
    cg.debug_main_file = parse_name;

  cg.user_source = user_src;
  cg.user_source_len = ulen;
  if ((opt->run_jit || opt->output_file) && opt->mode != NY_MODE_REPL &&
      ny_should_use_jit_cache(opt) && !opt->dump_ast && !opt->expand &&
      !opt->dump_llvm && !opt->emit_ir_path && !opt->emit_bc_path &&
      !opt->dump_on_error && !opt->dump_diagnose) {
    jit_cache_file = ny_jit_cache_path(
        source, prebuilt_path, 0, opt->opt_level, opt->opt_dce,
        opt->opt_internalize, opt->debug_symbols,
        (unsigned long)ny_std_latest_mtime(),
        ny_stage_semantic_config_fingerprint(opt));
#ifndef _WIN32
    /*
     * Reserve the native path even on a cold compile: the newly generated
     * bitcode can be promoted to this faster process-startup tier below.
     * Loading remains gated by the bitcode manifest because both artifacts
     * represent the same source fingerprint.
     */
    if (jit_cache_file && opt->run_jit && ny_jit_native_cache_enabled()) {
      native_cache_file = ny_jit_native_cache_path(jit_cache_file);
      if (native_cache_file &&
          ny_cache_artifact_manifest_valid(jit_cache_file, source) &&
          ny_jit_native_cache_load(native_cache_file, &native_cache_handle,
                                   &native_cache_entry)) {
        if (opt->verbose || ny_env_enabled("NYTRIX_TRACE_CACHE"))
          fprintf(stderr, "JIT native cache hit: %s\n", native_cache_file);
        loaded_from_cache = true;
        goto skip_compilation;
      }
    }
#endif
    /*
     * LLVM parsing happens only after the provenance sidecar is accepted.
     */
    if (jit_cache_file &&
        ny_cache_artifact_manifest_valid(jit_cache_file, source)) {
      LLVMModuleRef cached_mod = NULL;
      if (ny_jit_cache_load(jit_cache_file, cg.ctx, &cached_mod)) {
        if (opt->verbose)
          fprintf(stderr, "JIT cache hit: %s\n", jit_cache_file);
        loaded_from_cache = true;
        LLVMDisposeModule(cg.module);
        cg.module = cached_mod;
        script_fn = LLVMGetNamedFunction(cg.module, "_ny_top_entry");
        if (!script_fn) {
          if (opt->verbose)
            fprintf(stderr, "JIT cache corrupt (missing entry): %s\n",
                    jit_cache_file);
          loaded_from_cache = false;
          cg.module = LLVMModuleCreateWithNameInContext("nytrix", cg.ctx);
          ny_llvm_prepare_module(cg.module, 3);
        } else {
          codegen_repopulate_interns(&cg);
#ifndef _WIN32
          ny_pipeline_promote_jit_cache(opt, native_cache_file, &cg);
#endif
          goto skip_compilation;
        }
      }
    }
  }

  bool parallel_modules = false;
#ifndef _WIN32
  ny_module_list mods = {0};
  bool module_mode_enabled = ny_parallel_modules_enabled(opt);
  if (ny_env_enabled("NYTRIX_TRACE_CACHE") && module_mode_enabled &&
      (fast_compiler || use_std_bc_cache))
    fprintf(stderr, "module cache scan skipped: fast=%d std-cache=%d\n",
            fast_compiler ? 1 : 0, use_std_bc_cache ? 1 : 0);
  if (!fast_compiler && !use_std_bc_cache && module_mode_enabled) {
    ny_collect_top_modules(&prog, &mods);
    ny_module_list_fingerprint(&prog, &mods);
    if (ny_env_enabled("NYTRIX_TRACE_CACHE"))
      fprintf(stderr, "module cache scan: modules=%zu\n", mods.len);
    if (mods.len > 0)
      parallel_modules = true;
  }
#endif

  if (opt->std_bc_path && ny_access(opt->std_bc_path, R_OK) == 0) {
    cg.skip_stdlib = true;
    use_std_bc_cache = true;
    std_bc_cache = opt->std_bc_path;
  }
  if (use_std_bc_cache)
    cg.skip_stdlib = true;
  cg.emit_cached_stdlib_init = use_std_bc_cache;
  if (use_std_bc_cache) {
    NY_LOG_INFO("linking core library bitcode cache: %s\n", std_bc_cache);
    (void)ny_std_bc_cache_load_links(std_bc_cache, &cg);
  }
  if (opt->emit_module) {
    cg.emit_module_name = opt->emit_module;
    cg.emit_module_decls_only = !ny_env_enabled("NYTRIX_STD_BC_EMIT_BODIES");
    cg.emit_script = false;
  }
#ifndef _WIN32
  if (!opt->emit_module && parallel_modules) {
    cg.emit_module_name = "";
    cg.emit_module_decls_only = true;
  }
#endif
  cg.source_string = source;
  cg.prog_owned = false;
  NY_DBG(NY_DBG_PASS, "PHASE semantic-prepare begin statements=%zu",
         prog.body.len);
  ny_pipe_trace_phase_memory("semantic-prepare.begin");
  NY_LOG_V2("Preparing codegen (analysis & links)...\n");
  ny_tick_t t_codegen_prepare = opt->do_timing ? ny_ticks_now() : 0;
  progress_node = ny_progress_task_begin("prepare codegen", 1);
  codegen_prepare(&cg);
  ny_progress_task_end(progress_node);
  NY_DBG(NY_DBG_PASS, "PHASE semantic-prepare complete had_error=%d",
         cg.had_error ? 1 : 0);
  ny_pipe_trace_phase_memory("semantic-prepare.complete");
  maybe_log_phase_time(opt->do_timing, "Codegen prepare:", t_codegen_prepare);
  if (cg.had_error) {
    NY_LOG_ERR("Codegen prepare failed\n");
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_TRAIT, parse_name,
                               "codegen prepare failed", 1);
    exit_code = 1;
    goto exit_success;
  }
  ny_type_pipeline_stage_t max_type_stage = NY_TYPE_PIPELINE_STAGE_ABI;
  if (opt->stop_after == NY_STOP_AFTER_HM)
    max_type_stage = NY_TYPE_PIPELINE_STAGE_HM;
  else if (opt->stop_after == NY_STOP_AFTER_TRAIT ||
           opt->stop_after == NY_STOP_AFTER_FLOW)
    max_type_stage = NY_TYPE_PIPELINE_STAGE_TRAIT;
  ny_type_pipeline_stage_t failed_type_stage = NY_TYPE_PIPELINE_STAGE_OK;
  NY_DBG(NY_DBG_PASS, "PHASE type-validation begin stages=HM,trait,ABI");
  ny_pipe_trace_phase_memory("type-validation.begin");
  progress_node = ny_progress_task_begin("validate types", 1);
  ny_tick_t t_type_validation = opt->do_timing ? ny_ticks_now() : 0;
  int type_errors = 0;
  bool type_include_std =
      (opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM) ||
      (opt && opt->native_backend != NY_NATIVE_BACKEND_LLVM);
  if (opt && opt->list_fallbacks && !reuse_semantic_artifact) {
    ny_type_pipeline_list_fallbacks(&prog, &cg, parse_name, type_include_std);
    ny_progress_task_end(progress_node);
    codegen_dispose(&cg);
    program_free(&prog, arena);
    return 0;
  }
  if (opt && opt->explain && !reuse_semantic_artifact) {
    ny_type_pipeline_explain_semantics(&prog, &cg, parse_name, type_include_std,
                                       opt->explain_topic);
    ny_progress_task_end(progress_node);
    codegen_dispose(&cg);
    program_free(&prog, arena);
    return 0;
  }
  if (!reuse_semantic_artifact) {
    type_errors = ny_type_pipeline_validate_semantics(
        &prog, &cg, parse_name, type_include_std, max_type_stage,
        &failed_type_stage, true, &type_errors_json);
  } else if (!ny_type_pipeline_verify_proof_claims(&prog, &cg, parse_name)) {
    type_errors = 1;
    failed_type_stage = NY_TYPE_PIPELINE_STAGE_TRAIT;
  }
  NY_DBG(NY_DBG_PASS,
         "PHASE type-validation complete errors=%d failed_stage=%d",
         type_errors, (int)failed_type_stage);
  ny_pipe_trace_phase_memory("type-validation.complete");
  ny_progress_task_end(progress_node);
  if (reuse_semantic_artifact && opt->do_timing)
    maybe_log_phase_time(
        true, "Proof validation (reused type artifact):", t_type_validation);
  else
    maybe_log_phase_time(opt->do_timing, "Type validation:", t_type_validation);
  if (type_errors > 0) {
    ny_stop_after_stage_t error_stage = NY_STOP_AFTER_HM;
    const char *error_message = "HM type validation failed";
    if (failed_type_stage == NY_TYPE_PIPELINE_STAGE_TRAIT) {
      error_stage = NY_STOP_AFTER_TRAIT;
      error_message = "trait validation failed";
      NY_LOG_ERR("Trait/type validation failed\n");
    } else if (failed_type_stage == NY_TYPE_PIPELINE_STAGE_ABI) {
      error_stage = NY_STOP_AFTER_ABI;
      error_message = "ABI validation failed";
      NY_LOG_ERR("ABI/layout validation failed\n");
    } else {
      NY_LOG_ERR("HM type validation failed\n");
    }
    if ((failed_type_stage == NY_TYPE_PIPELINE_STAGE_HM &&
         (opt->stop_after == NY_STOP_AFTER_HM ||
          opt->stop_after == NY_STOP_AFTER_TRAIT ||
          opt->stop_after == NY_STOP_AFTER_FLOW ||
          opt->stop_after == NY_STOP_AFTER_ABI)) ||
        (failed_type_stage == NY_TYPE_PIPELINE_STAGE_TRAIT &&
         (opt->stop_after == NY_STOP_AFTER_TRAIT ||
          opt->stop_after == NY_STOP_AFTER_FLOW ||
          opt->stop_after == NY_STOP_AFTER_ABI)) ||
        (failed_type_stage == NY_TYPE_PIPELINE_STAGE_ABI &&
         opt->stop_after == NY_STOP_AFTER_ABI)) {
      ny_stage_emit_artifact(opt, opt->stop_after, &prog, &cg, parse_name,
                             cg.module, true);
    }
    if (opt && opt->collect_errors && type_errors_json)
      ny_stage_write_default_artifact(opt, "errors.v1.json", type_errors_json);
    else
      ny_stage_maybe_emit_errors(opt, error_stage, parse_name, error_message,
                                 type_errors);
    exit_code = 1;
    goto exit_success;
  }

  if (opt->dump_ast_typed) {
    ny_ast_dump_tree(stdout, &prog, parse_name, opt->dump_ast_filter, true);
    if (opt->dump_ast)
      goto exit_success;
  }
  if (opt->emit_refinement_interface_path) {
    char interface_error[256] = {0};
    if (!ny_refinement_interface_export(
            &cg, opt->emit_refinement_interface_path, interface_error,
            sizeof(interface_error))) {
      NY_LOG_ERR("Refinement interface export failed: %s\n", interface_error);
      exit_code = 1;
      goto exit_success;
    }
  }
  if (opt->emit_effect_interface_path) {
    char interface_error[256] = {0};
    if (!ny_effect_interface_export(&cg, opt->emit_effect_interface_path,
                                    interface_error, sizeof(interface_error))) {
      NY_LOG_ERR("Effect interface export failed: %s\n", interface_error);
      exit_code = 1;
      goto exit_success;
    }
  }

  if (opt->stop_after == NY_STOP_AFTER_HM ||
      opt->stop_after == NY_STOP_AFTER_TRAIT ||
      opt->stop_after == NY_STOP_AFTER_FLOW ||
      opt->stop_after == NY_STOP_AFTER_ABI) {
    ny_stage_emit_artifact(opt, opt->stop_after, &prog, &cg, parse_name,
                           cg.module, true);
    goto exit_success;
  }

#ifndef _WIN32
  ny_module_job *mod_jobs = NULL;
  size_t mod_job_count = 0;
  ny_tick_t t_parallel = 0;
  size_t mods_len = 0;
  if (!fast_compiler && !use_std_bc_cache && ny_parallel_modules_enabled(opt)) {
    if (opt->do_timing)
      t_parallel = ny_ticks_now();
    if (parallel_modules && mods.len > 0) {
      mods_len = mods.len;
      mod_jobs = calloc(mods.len, sizeof(ny_module_job));
      if (mod_jobs) {
        mod_job_count = mods.len;
        int max_jobs = ny_parallel_module_jobs(opt, mods.len);
        size_t started = 0;
        size_t finished = 0;
        size_t running = 0;
        const char *tmp_dir = ny_get_temp_dir();
        while (finished < mods.len) {
          while (started < mods.len && running < (size_t)max_jobs) {
            if (!ny_spawn_module_job(opt, mods.entries[started].name,
                                     mods.entries[started].source_fingerprint,
                                     tmp_dir, &mod_jobs[started])) {
              parallel_modules = false;
              break;
            }
            started++;
            if (mod_jobs[started - 1].pid > 0)
              running++;
            else
              finished++;
          }
          if (!parallel_modules)
            break;
          if (finished == mods.len)
            break;
          if (running == 0) {
            parallel_modules = false;
            break;
          }
          int status = 0;
          pid_t pid = wait(&status);
          if (pid < 0) {
            parallel_modules = false;
            break;
          }
          for (size_t i = 0; i < started; i++) {
            if (mod_jobs[i].pid != pid)
              continue;
            if (WIFEXITED(status))
              mod_jobs[i].exit_code = WEXITSTATUS(status);
            else
              mod_jobs[i].exit_code = 1;
            if (mod_jobs[i].exit_code == 0 &&
                !ny_module_job_publish_cache(&mod_jobs[i]))
              mod_jobs[i].exit_code = 1;
            if (mod_jobs[i].exit_code != 0) {
              if (ny_env_enabled("NYTRIX_TRACE_CACHE"))
                fprintf(stderr, "module worker failed: %s (exit=%d)\n",
                        mod_jobs[i].name ? mod_jobs[i].name : "<module>",
                        mod_jobs[i].exit_code);
              parallel_modules = false;
            }
            break;
          }
          running--;
          finished++;
        }
        if (!parallel_modules) {
          for (size_t i = 0; i < started; i++) {
            if (mod_jobs[i].pid <= 0)
              continue;
            int st = 0;
            (void)waitpid(mod_jobs[i].pid, &st, 0);
          }
        }
      } else {
        parallel_modules = false;
      }
    } else {
      parallel_modules = false;
    }
  }
  ny_free_module_list(&mods);
  if (mods_len > 0 && !parallel_modules) {
    NY_LOG_ERR("Parallel module build failed\n");
    exit_code = 1;
    goto exit_success;
  }
  if (!parallel_modules && mod_jobs) {
    for (size_t i = 0; i < mod_job_count; i++) {
      if (mod_jobs[i].bc_path && !mod_jobs[i].persistent)
        (void)unlink(mod_jobs[i].bc_path);
      ny_module_job_free(&mod_jobs[i]);
    }
    free(mod_jobs);
    mod_jobs = NULL;
    mod_job_count = 0;
  }
#endif
  NY_LOG_V2("Emitting IR...\n");
  progress_node = ny_progress_task_begin("emit llvm", 1);
  /*
   * Unified NYIR → LLVM IR path: lower the full program through the
   * optimized NYIR pipeline and emit directly into the LLVM module.
   *
   * This path reuses all NYIR optimization passes (alias analysis,
   * store sinking, load forwarding, tag/bounds check elimination,
   * proof-carrying transforms) before LLVM machine codegen.
   */
  char nyr_llvm_err[512] = {0};
  bool llvm_emit_ok = false;
  ny_pipe_trace_phase_memory("nyir-to-llvm.begin");
  llvm_emit_ok = ny_pipeline_emit_llvm_program(&cg, &prog, opt, nyr_llvm_err,
                                               sizeof(nyr_llvm_err));
  ny_pipe_trace_phase_memory("nyir-to-llvm.complete");
  if (!llvm_emit_ok) {
    ny_progress_task_end(progress_node);
    fflush(stderr);
    NY_LOG_ERR("%s LLVM emission failed: %s\n", "NYIR to",
               nyr_llvm_err[0] ? nyr_llvm_err : "unknown error");
    exit_code = 1;
    goto exit_success;
  }
  if (!script_fn)
    script_fn = LLVMGetNamedFunction(cg.module, "_ny_top_entry");
  if (!script_fn && !cg.emit_module_decls_only && !opt->emit_module) {
    ny_progress_task_end(progress_node);
    NY_LOG_ERR("NYIR to LLVM emission failed: missing script entry\n");
    exit_code = 1;
    goto exit_success;
  }
  ny_progress_task_end(progress_node);
  fflush(stderr);
  if (cg.had_error) {
    NY_LOG_ERR("Codegen failed\n");
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_ABI, parse_name,
                               "codegen failed", 1);
    dump_debug_bundle(opt, user_src, cg.module, "codegen", "codegen failed");
    debug_bundle_written = opt->dump_on_error;
    exit_code = 1;
    goto exit_success;
  }
  fflush(stderr);
  if (use_std_bc_cache) {
    ny_ir_externalize_std_definitions(opt, cg.module);
    bool auto_cache_path =
        auto_std_bc_cache && strcmp(std_bc_cache, auto_std_bc_cache) == 0;
    if (!ny_link_module_cache(cg.ctx, cg.module, std_bc_cache, auto_cache_path,
                              true)) {
      NY_LOG_ERR("Failed to link core cache: %s\n", std_bc_cache);
      exit_code = 1;
      goto exit_success;
    }
    codegen_rebind_llvm_symbols(&cg);
    ny_drop_jit_llvm_used_metadata(cg.module);
  }
#ifndef _WIN32
  if (parallel_modules && mod_jobs) {
    fflush(stderr);
    for (size_t i = 0; i < mod_job_count; i++) {
      if (!mod_jobs[i].bc_path)
        continue;
      if (!ny_link_module_cache(cg.ctx, cg.module, mod_jobs[i].bc_path, true,
                                false)) {
        NY_LOG_ERR("Failed to link module cache for %s\n",
                   mod_jobs[i].name ? mod_jobs[i].name : "<module>");
        exit_code = 1;
        goto exit_success;
      }
      codegen_rebind_llvm_symbols(&cg);
      if (!mod_jobs[i].persistent)
        (void)unlink(mod_jobs[i].bc_path);
      ny_module_job_free(&mod_jobs[i]);
    }
    free(mod_jobs);
    mod_jobs = NULL;
    mod_job_count = 0;
    if (opt->do_timing && t_parallel)
      fprintf(stderr, "Parallel modules: %.4fs\n",
              ny_ticks_elapsed_sec(t_parallel));
  }
#endif
  fflush(stderr);
  codegen_debug_finalize(&cg);
  maybe_log_phase_time(opt->do_timing, "Codegen:", t_codegen);
  ny_trace_ir_stats("post_codegen", cg.module);
  ny_sanitize_platform_sections(cg.module);

  if (opt->dump_llvm) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    LLVMDumpModule(dump_mod ? dump_mod : cg.module);
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }
  if (ny_env_enabled("NY_IR_DUMP")) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    LLVMDumpModule(dump_mod ? dump_mod : cg.module);
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }
  ny_tick_t t_ver = (opt->do_timing && opt->verify_module) ? ny_ticks_now() : 0;
  fflush(stderr);
  if (!verify_module_if_needed(opt, cg.module)) {
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_ABI, parse_name,
                               "LLVM verification failed", 1);
    dump_debug_bundle(opt, user_src, cg.module, "verify",
                      "LLVM module verification failed");
    debug_bundle_written = opt->dump_on_error;
    exit_code = 1;
    goto exit_success;
  }
  if (opt->do_timing && opt->verify_module)
    fprintf(stderr, "Verify:       %.4fs\n", ny_ticks_elapsed_sec(t_ver));
  if (write_compile_caches && !use_std_bc_cache && auto_std_bc_cache &&
      cg.module && !loaded_from_cache && !opt->emit_module &&
      std_mode != STD_MODE_NONE && !has_local) {
    bool need_bc = ny_access(auto_std_bc_cache, R_OK) != 0;
    bool need_links = auto_std_bc_cache_needs_links ||
                      !ny_std_bc_cache_has_links(auto_std_bc_cache);
    bool cache_ok = true;
    if (need_bc) {
      cache_ok = ny_save_std_bc_cache_from_module(cg.module, auto_std_bc_cache);
      if (cache_ok && opt->verbose)
        fprintf(stderr, "Stdlib bitcode cache saved: %s\n", auto_std_bc_cache);
    }
    if (cache_ok && need_links)
      (void)ny_std_bc_cache_save_links(auto_std_bc_cache, &cg);
    auto_std_bc_cache_saved = cache_ok;
  }
  ny_tick_t t_opt = opt->do_timing ? ny_ticks_now() : 0;
  if (!(opt->run_jit && ny_jit_module_is_apple_arm64(cg.module)) ||
      ny_env_enabled("NYTRIX_JIT_HOST_ATTRS")) {
    ny_llvm_apply_host_attrs(cg.module);
  }
  run_dead_strip_if_needed(opt, &cg, cg.module);
  ny_dump_diagnose_ir_stage(opt, cg.module, "diag.pre.ll", "pre-opt");
  if (opt->emit_ir_pre_path) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    ny_dump_ir_if_requested(dump_mod ? dump_mod : cg.module,
                            opt->emit_ir_pre_path, "pre-opt");
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }

  /*
   * JIT cache artifacts are written below only after this exact optimization
   * stage. Re-running LLVM's pipeline on a validated hit both burns most of
   * the warm-start budget and can make cache behavior depend on pass details
   * outside the cache key.
   */
  if (!fast_compiler && !loaded_from_cache) {
    int eff_opt = opt->opt_level;
    if (parallel_modules && !opt->llvm_lto &&
        !ny_env_enabled("NYTRIX_PARALLEL_OPT_LINK")) {
      eff_opt = 0;
    }
    if (opt->run_jit) {
      eff_opt = ny_jit_effective_ir_opt_level(opt, cg.module, eff_opt);
    }

    if (cg.di_builder) {
      codegen_debug_finalize(&cg);
    }
    progress_node = ny_progress_task_begin("optimize", 1);
    ny_llvm_optimize_module(cg.module, eff_opt, opt->opt_loops,
                            opt->opt_pipeline);
    ny_progress_task_end(progress_node);
    ny_trace_ir_stats("post_opt", cg.module);
    if (opt->do_timing && (opt->opt_level > 0 || opt->opt_pipeline))
      fprintf(stderr, "Optimization: %.4fs\n", ny_ticks_elapsed_sec(t_opt));
    if (opt->sanitize && *opt->sanitize) {
      NY_LOG_V2("Applying sanitizer instrumentation: %s\n", opt->sanitize);
      if (!ny_llvm_apply_sanitize(cg.module, opt->sanitize)) {
        NY_LOG_WARN("LLVM sanitizer pass for '%s' failed; continuing without "
                    "sanitizer instrumentation\n",
                    opt->sanitize);
      }
    }
  }
  ny_dump_diagnose_ir_stage(opt, cg.module, "diag.post.ll", "post-opt");
  if (opt->stop_after == NY_STOP_AFTER_OPT) {
    /*
     * When stopping after optimization for validation (e.g. test runner),
     * skip the full typed/resolved artifact JSON which invokes Z3 and can
     * trigger heap-corruption detection in large stdlib compilations.  Just
     * emit a lightweight pass-through artifact so the caller sees success.
     */
    bool emit_full = opt->emit_artifact_path || opt->emit_shapes;
    ny_stage_emit_artifact(opt,
                           emit_full ? NY_STOP_AFTER_OPT : NY_STOP_AFTER_PARSE,
                           &prog, &cg, parse_name, cg.module, true);
    goto exit_success;
  }
  if ((opt->emit_artifact_path || opt->emit_shapes) &&
      opt->stop_after == NY_STOP_AFTER_NONE) {
    ny_stage_emit_artifact(opt, NY_STOP_AFTER_OPT, &prog, &cg, parse_name,
                           cg.module, false);
  }
  if (write_compile_caches && jit_cache_file) {
#ifndef _WIN32
    ny_pipeline_promote_jit_cache(opt, native_cache_file, &cg);
#endif
    if (!loaded_from_cache && ny_jit_cache_save(jit_cache_file, cg.module) &&
        ny_cache_artifact_manifest_save(jit_cache_file, source)) {
      if (opt->verbose)
        fprintf(stderr, "JIT cache saved: %s\n", jit_cache_file);
    }
  }
skip_compilation:
  /*
   * Runtime trampolines embed process-local function addresses.  Install
   * them only after the cache has been written, so serialized LLVM bitcode
   * never contains stale pointers from a previous compiler process.
   *
   * Runtime trampolines contain addresses in this compiler process and are
   * valid only while executing the LLVM JIT.  AOT output is linked and run
   * in a different process, where those addresses are meaningless; leave
   * the runtime declarations intact so the AOT runtime library resolves
   * them normally.  In particular, installing these for `-run` made list
   * length/index calls jump into stale compiler memory.
   */
  if (opt->native_backend == NY_NATIVE_BACKEND_LLVM && cg.module &&
      opt->run_jit && !opt->run_aot)
    ny_jit_define_runtime_trampolines(cg.module);
  if (opt->native_backend != NY_NATIVE_BACKEND_LLVM)
    ny_type_pipeline_persist_native_facts(&prog, &cg, parse_name);
  if (opt->native_tier_report) {
    char native_tier_err[512] = {0};
    if (!ny_pipeline_write_tier_report(&prog, opt, native_tier_err,
                                       sizeof(native_tier_err))) {
      NY_LOG_ERR("Failed to write native tier report: %s\n",
                 native_tier_err[0] ? native_tier_err : "unknown error");
      exit_code = 1;
      goto exit_success;
    }
  }
  if (opt->callgraph) {
    char callgraph_err[512] = {0};
    if (!ny_pipeline_write_callgraph_for_program(&prog, opt, callgraph_err,
                                                 sizeof(callgraph_err))) {
      NY_LOG_ERR("Failed to write callgraph: %s\n",
                 callgraph_err[0] ? callgraph_err : "unknown error");
      exit_code = 1;
      goto exit_success;
    }
  }
  if (opt->native_result_oracle) {
    char native_oracle_err[512] = {0};
    if (opt->native_oracle_fuzz_count > 0) {
      if (!nyr_oracle_fuzz(opt, opt->native_oracle_fuzz_count,
                           ny_pipeline_runtime_services_get(),
                           native_oracle_err, sizeof(native_oracle_err))) {
        NY_LOG_ERR("Native oracle fuzz failed: %s\n",
                   native_oracle_err[0] ? native_oracle_err : "unknown error");
        exit_code = 1;
        goto exit_success;
      }
    } else if (!ny_pipeline_result_oracle(&prog, opt, native_oracle_err,
                                          sizeof(native_oracle_err))) {
      NY_LOG_ERR("Native result oracle failed: %s\n",
                 native_oracle_err[0] ? native_oracle_err : "unknown error");
      exit_code = 1;
      goto exit_success;
    }
  }
  if (opt->native_dump_ir &&
      (opt->native_backend != NY_NATIVE_BACKEND_LLVM || opt->nyr_run ||
       opt->nyr_metadata_report || opt->nyr_dump_bin)) {
    char native_dump_err[512] = {0};
    ny_pipe_trace_phase_memory("native-ir-dump.begin");
    if (!ny_pipeline_dump_ir_for_program(&prog, opt, native_dump_err,
                                         sizeof(native_dump_err))) {
      const char *detail =
          native_dump_err[0] ? native_dump_err : "unknown error";
      /*
       * --nyir-run executes the program as part of preparing its verified
       * dump.  A language panic from that execution is the program result,
       * not a failure to produce a diagnostic artifact.
       */
      if (opt->nyr_run)
        NY_LOG_ERR("PanicError: %s\n", detail);
      else
        NY_LOG_ERR("Failed to dump native NYIR: %s\n", detail);
      exit_code = 1;
      goto exit_success;
    }
    ny_pipe_trace_phase_memory("native-ir-dump.complete");
    /*
     * --nyir-run executes the program inside the NYIR VM during the dump
     * above (unless a dumped binary will be run instead). The pipeline used
     * to fall through into JIT code generation, executing the program a
     * second time with all its side effects; the VM result is the answer,
     * so stop here.
     */
    if (opt->nyr_run && !(opt->nyr_dump_text || opt->nyr_dump_bin ||
                          (opt->nyr_run_bin_path && opt->nyr_run_bin_path[0] &&
                           opt->nyr_dump_bin))) {
      exit_code = 0;
      goto exit_success;
    }
  }
  if (jit_cache_file) {
    free(jit_cache_file);
    jit_cache_file = NULL;
  }
  if (write_compile_caches && !auto_std_bc_cache_saved && !use_std_bc_cache &&
      auto_std_bc_cache && cg.module && !loaded_from_cache &&
      !opt->emit_module && std_mode != STD_MODE_NONE && !has_local) {
    bool need_bc = ny_access(auto_std_bc_cache, R_OK) != 0;
    bool can_save_links = !need_bc || ny_save_std_bc_cache_from_module(
                                          cg.module, auto_std_bc_cache);
    if (can_save_links && (need_bc || auto_std_bc_cache_needs_links ||
                           !ny_std_bc_cache_has_links(auto_std_bc_cache))) {
      (void)ny_std_bc_cache_save_links(auto_std_bc_cache, &cg);
    }
    if (need_bc && can_save_links && opt->verbose) {
      fprintf(stderr, "Stdlib bitcode cache saved: %s\n", auto_std_bc_cache);
    }
  }
  free(auto_std_bc_cache);
  auto_std_bc_cache = NULL;
  if (loaded_from_cache && cg.module)
    codegen_prepare(&cg);
  ny_clear_origin_sections(cg.module);

  if (opt->emit_ir_path) {
    ny_ensure_parent_dir_for_path(opt->emit_ir_path);
    ny_pipe_trace_phase_memory("llvm-ir-emission.begin");
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    char *err = NULL;
    if (LLVMPrintModuleToFile(dump_mod ? dump_mod : cg.module,
                              opt->emit_ir_path, &err) != 0) {
      NY_LOG_ERR("Failed to write IR to %s\n", opt->emit_ir_path);
      if (err) {
        NY_LOG_ERR("%s\n", err);
        LLVMDisposeMessage(err);
      }
      exit_code = 1;
      goto exit_success;
    }
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
    ny_pipe_trace_phase_memory("llvm-ir-emission.complete");
    ny_trace_file_size("emit_ir", opt->emit_ir_path);
  }
  if (opt->emit_bc_path) {
    char *module_verify_error = NULL;
    if (LLVMVerifyModule(cg.module, LLVMReturnStatusAction,
                         &module_verify_error) != 0) {
      NY_LOG_ERR("Refusing to emit invalid LLVM bitcode: %s\n",
                 module_verify_error ? module_verify_error : "unknown error");
      if (module_verify_error)
        LLVMDisposeMessage(module_verify_error);
      exit_code = 1;
      goto exit_success;
    }
    ny_ensure_parent_dir_for_path(opt->emit_bc_path);
    bool wrote = (LLVMWriteBitcodeToFile(cg.module, opt->emit_bc_path) == 0);
    if (!wrote)
      wrote = ny_reemit_bitcode_via_ir(cg.module, opt->emit_bc_path);
    if (wrote && !ny_verify_bitcode(cg.ctx, opt->emit_bc_path)) {
      wrote = false;
    }
    if (!wrote) {
      NY_LOG_ERR("Failed to write bitcode to %s\n", opt->emit_bc_path);
      exit_code = 1;
      goto exit_success;
    }
    ny_trace_file_size("emit_bc", opt->emit_bc_path);
  }
  /*
   * Extern declarations with `as "cname"` must emit their linker symbol, not
   * the Nytrix name.  Rename before any LLVM assembly/object emission.
   */
  if (opt->native_backend == NY_NATIVE_BACKEND_LLVM)
    codegen_export_extern_link_names(&cg);
  if (opt->emit_asm_path) {
    ny_ensure_parent_dir_for_path(opt->emit_asm_path);
    ny_pipe_trace_phase_memory("assembly-emission.begin");
    if (opt->native_backend != NY_NATIVE_BACKEND_LLVM) {
      char native_err[512] = {0};
      if (!ny_pipeline_emit_asm(&prog, opt, opt->emit_asm_path, native_err,
                                sizeof(native_err))) {
        NY_LOG_ERR("Failed to write native assembly to %s: %s\n",
                   opt->emit_asm_path,
                   native_err[0] ? native_err : "unknown error");
        exit_code = 1;
        goto exit_success;
      }
    } else {
      if (!ny_llvm_emit_file(cg.module, opt->emit_asm_path, LLVMAssemblyFile,
                             opt->opt_level)) {
        NY_LOG_ERR("Failed to write assembly to %s\n", opt->emit_asm_path);
        exit_code = 1;
        goto exit_success;
      }
    }
    ny_pipe_trace_phase_memory("assembly-emission.complete");
    ny_trace_file_size("emit_asm", opt->emit_asm_path);
  }
  ny_dump_diagnose_finalize(opt, cg.module, opt->opt_level, user_src);
  if (opt->output_file) {
    char obj[4096];
    char obj_name[64];
    snprintf(obj_name, sizeof(obj_name), "ny_tmp_%ld_%llu.o", (long)getpid(),
             (unsigned long long)ny_ticks_now());
    ny_join_path(obj, sizeof(obj), ny_get_temp_dir(), obj_name);
    bool use_native_object = opt->native_backend != NY_NATIVE_BACKEND_LLVM;
    if (!use_native_object)
      ensure_aot_entry(&cg, script_fn);
    bool is_obj_only = ny_output_path_is_object(output_path);
    if (is_obj_only) {
      progress_node = ny_progress_task_begin("emit object", 1);
      ny_tick_t t_emit_obj = opt->do_timing ? ny_ticks_now() : 0;
      char native_obj_err[512] = {0};
      bool obj_ok =
          use_native_object
              ? ny_pipeline_emit_program_object(
                    &prog, opt, output_path, "rt_main", false, native_obj_err,
                    sizeof(native_obj_err))
              : ny_llvm_emit_object(cg.module, output_path, opt->opt_level);
      ny_progress_task_end(progress_node);
      if (obj_ok) {
        maybe_log_phase_time(opt->do_timing, "Emit obj:", t_emit_obj);
        NY_LOG_SUCCESS("Saved object: %s\n", output_path);
        ny_trace_file_size("emit_obj", output_path);
        if (opt->run_aot) {
          NY_LOG_ERR("Cannot run AOT from object file\n");
          exit_code = 1;
          goto exit_success;
        }
      } else {
        maybe_log_phase_time(opt->do_timing, "Emit obj:", t_emit_obj);
        NY_LOG_ERR("Failed to emit object file%s%s\n",
                   native_obj_err[0] ? ": " : "", native_obj_err);
        exit_code = 1;
        goto exit_success;
      }
      progress_node = ny_progress_task_begin("finalize object", 1);
      ny_progress_task_end(progress_node);
    } else {
      progress_node = ny_progress_task_begin("emit object", 1);
      ny_tick_t t_emit_obj = opt->do_timing ? ny_ticks_now() : 0;
      char native_obj_err[512] = {0};
      bool emitted_obj =
          use_native_object
              ? ny_pipeline_emit_program_object(&prog, opt, obj, "main", false,
                                                native_obj_err,
                                                sizeof(native_obj_err))
              : ny_llvm_emit_object(cg.module, obj, opt->opt_level);
      ny_progress_task_end(progress_node);
      maybe_log_phase_time(opt->do_timing, "Emit obj:", t_emit_obj);
      if (!emitted_obj) {
        NY_LOG_ERR("Failed to emit object file%s%s\n",
                   native_obj_err[0] ? ": " : "", native_obj_err);
        dump_debug_bundle(opt, user_src, cg.module, "object",
                          "native object emission failed");
        debug_bundle_written = opt->dump_on_error;
        exit_code = 1;
        goto exit_success;
      }
      ny_trace_file_size("emit_obj_tmp", obj);
      const char *cc = ny_builder_choose_cc();
      char rto[4096];
      char rto_name[64];
      snprintf(rto_name, sizeof(rto_name), "ny_rt_%ld_%llu.o", (long)getpid(),
               (unsigned long long)ny_ticks_now());
      ny_join_path(rto, sizeof(rto), ny_get_temp_dir(), rto_name);
      ny_opt_profile_kind_t runtime_profile =
          ny_opt_profile_kind_from_name(opt->opt_profile);
      bool runtime_speed = opt->opt_level >= 3 ||
                           runtime_profile == NY_OPT_PROFILE_SPEED ||
                           runtime_profile == NY_OPT_PROFILE_PEAK ||
                           ny_env_enabled("NYTRIX_RUNTIME_SPEED");
      const char *runtime_opt_env = ny_env_str_nonempty("NYTRIX_RUNTIME_OPT");
      int runtime_speed_level = runtime_speed ? 3 : (opt->opt_size ? -1 : 0);
      if (runtime_opt_env) {
        if (strcmp(runtime_opt_env, "0") == 0 ||
            strcmp(runtime_opt_env, "size") == 0)
          runtime_speed_level = 0;
        else if (strcmp(runtime_opt_env, "2") == 0)
          runtime_speed_level = 2;
        else if (strcmp(runtime_opt_env, "3") == 0 ||
                 strcmp(runtime_opt_env, "speed") == 0)
          runtime_speed_level = 3;
      }
      bool runtime_native = runtime_speed_level >= 3 &&
                            ny_env_enabled_default_on("NYTRIX_RUNTIME_NATIVE");
      NY_LOG_V2(
          "Compiling runtime to %s using %s (debug=%d speed=%d native=%d)...\n",
          rto, cc, opt->debug_symbols, runtime_speed_level,
          runtime_native ? 1 : 0);
      ny_tick_t t_runtime_obj = opt->do_timing ? ny_ticks_now() : 0;
      if (!ny_builder_compile_runtime(cc, rto, NULL, opt->debug_symbols,
                                      opt->gprof == 1, runtime_speed_level,
                                      runtime_native, opt->sanitize)) {
        maybe_log_phase_time(opt->do_timing, "Runtime obj:", t_runtime_obj);
        unlink(obj);
        dump_debug_bundle(opt, user_src, cg.module, "runtime-object",
                          "runtime object compilation failed");
        debug_bundle_written = opt->dump_on_error;
        exit_code = 1;
        goto exit_success;
      }
      maybe_log_phase_time(opt->do_timing, "Runtime obj:", t_runtime_obj);
      bool link_strip = (opt->strip_override == 1);
      if (output_path && *output_path) {
        char out_dir[1024];
        snprintf(out_dir, sizeof(out_dir), "%s", output_path);
        char *slash = strrchr(out_dir, '/');
        if (slash && slash != out_dir) {
          *slash = '\0';
          ny_ensure_dir_recursive(out_dir);
        }
      }
      NY_LOG_V2("Linking executable %s (strip=%d, debug=%d)...\n", output_path,
                link_strip, opt->debug_symbols);
      progress_node = ny_progress_task_begin("link executable", 1);
      ny_link_lib_vec merged_libs;
      vec_init(&merged_libs);
      ny_link_lib_vec_merge(&merged_libs, opt, &cg);
      ny_tick_t t_link = opt->do_timing ? ny_ticks_now() : 0;
      if (!ny_builder_link(
              cc, obj, rto, NULL, NULL, 0,
              (const char *const *)opt->link_dirs.data, opt->link_dirs.len,
              (const char *const *)merged_libs.data, merged_libs.len,
              output_path, link_strip, opt->debug_symbols, opt->gprof == 1,
              opt->sanitize)) {
        ny_progress_task_end(progress_node);
        maybe_log_phase_time(opt->do_timing, "Link:", t_link);
        unlink(obj);
        unlink(rto);
        dump_debug_bundle(opt, user_src, cg.module, "link",
                          "native link failed");
        debug_bundle_written = opt->dump_on_error;
        exit_code = 1;
        ny_link_lib_vec_dispose(&merged_libs);
        goto exit_success;
      }
      ny_progress_task_end(progress_node);
      maybe_log_phase_time(opt->do_timing, "Link:", t_link);
      ny_link_lib_vec_dispose(&merged_libs);
      unlink(obj);
      unlink(rto);
      if (aot_cache_path[0] != '\0' &&
          strcmp(aot_cache_path, output_path) != 0 &&
          ny_valid_native_artifact(output_path)) {
        /*
         * Publish the executable first, then its provenance marker. Readers
         * reject an artifact without a marker, including interrupted writes.
         */
        if (ny_copy_file(output_path, aot_cache_path) == 0)
          (void)ny_cache_artifact_manifest_save(aot_cache_path, source);
      }
#ifdef _WIN32
      NY_LOG_SUCCESS("Saved EXE: %s\n", output_path);
#else
      NY_LOG_SUCCESS("Saved ELF: %s\n", output_path);
#endif
      ny_trace_file_size("emit_native", output_path);
    }
    if (show_progress)
      ny_progress_finish();
    if (opt->run_aot) {
      ny_sanitize_setup_env(opt->sanitize);
      bool use_safe_run = ny_safe_run_requested(&opt->safe_run) ||
                          (opt->sanitize && *opt->sanitize);
      const char *argv_exec[] = {output_path, NULL};
      int rc = use_safe_run
                   ? ny_safe_run_spawn(&opt->safe_run, argv_exec,
                                       opt->input_file ? opt->input_file
                                                       : output_path)
                   : ny_exec_spawn(argv_exec);
      if (rc != 0)
        exit_code = rc;
      if (aot_run_temp)
        (void)unlink(output_path);
    }
  }
  /*
   * Lint runs after all codegen passes (including the lazy script-entry
   * emit, which fires shadowing warnings while emitting main) so that every
   * compile-time diagnostic has been counted.  --warn-all turns each
   * warning into a non-zero exit (CI mode).
   */
  ny_lint_unused_imports(&cg, &prog);
  if (ny_diag_warn_as_error_count() > 0) {
    exit_code = 1;
    goto exit_success;
  }
  fflush(stderr);
  if (opt->run_jit) {
    ny_sanitize_setup_env(opt->sanitize);
#ifndef _WIN32
    if (native_cache_entry) {
      progress_node = ny_progress_task_begin("jit cache", 1);
      ny_progress_task_end(progress_node);
      if (show_progress)
        ny_progress_finish();
      ny_tick_t t_run = opt->do_timing ? ny_ticks_now() : 0;
      bool jit_use_safe_run = ny_safe_run_requested(&opt->safe_run) ||
                              (opt->sanitize && *opt->sanitize);
      if (jit_use_safe_run) {
        int rc = ny_safe_run_call(&opt->safe_run, ny_native_cache_safe_child,
                                  (void *)native_cache_entry,
                                  opt->input_file ? opt->input_file
                                                  : "native JIT cache");
        if (rc != 0)
          exit_code = rc;
      } else {
        native_cache_entry();
        rt_print_flush();
      }
      maybe_log_phase_time(opt->do_timing, "JIT Run:", t_run);
    } else
#endif
    {
      progress_node = ny_progress_task_begin("jit compile", 1);
      ny_tick_t t_jit = opt->do_timing ? ny_ticks_now() : 0;
      ny_jit_init_native_once();
      LLVMExecutionEngineRef ee;
      char *err = NULL;
      LLVMModuleRef jmod = cg.module;
      LLVMValueRef jit_script_fn = LLVMGetNamedFunction(jmod, "_ny_top_entry");
      LLVMValueRef jit_main_fn = LLVMGetNamedFunction(jmod, "main");
      bool call_implicit_main =
          jit_main_fn && !ny_program_has_explicit_main_entry(&cg, cg.prog);

      ny_jit_canonicalize_internal_calls(jmod);

      const char *jit_engine = opt->jit_engine;
      if (!jit_engine)
        jit_engine = getenv("NYTRIX_JIT_ENGINE");
      if (jit_engine && strcmp(jit_engine, "orc") == 0 &&
          opt->native_backend == NY_NATIVE_BACKEND_LLVM) {
        if (opt->debug_symbols)
          LLVMStripModuleDebugInfo(jmod);
        uint64_t saddr = 0, main_addr = 0;
        char *orc_error = NULL;

        if (!ny_orc_jit_ensure_engine(&cg, &orc_error)) {
          ny_progress_task_end(progress_node);
          NY_LOG_ERR("ORC JIT ensure failed: %s\n",
                     orc_error ? orc_error : "unknown error");
          free(orc_error);
          exit_code = 1;
          goto exit_success;
        }

        void *rt = NULL;
        bool module_consumed = false;
        bool executed =
            ny_orc_jit_execute(&cg, jmod, cg.ctx, &saddr, &main_addr, &rt,
                               &module_consumed, &orc_error);

        if (module_consumed) {
          cg.module = NULL;
          cg.ctx = NULL;
          cg.llvm_ctx_owned = false;
        }
        if (!executed) {
          ny_progress_task_end(progress_node);
          NY_LOG_ERR("ORC JIT execute failed: %s\n",
                     orc_error ? orc_error : "unknown error");
          free(orc_error);
          exit_code = 1;
          goto exit_success;
        }
        free(orc_error);
        maybe_log_phase_time(opt->do_timing, "JIT Init:", t_jit);
        ny_progress_task_end(progress_node);
        if (show_progress)
          ny_progress_finish();
        ny_tick_t t_run = opt->do_timing ? ny_ticks_now() : 0;
        if (saddr) {
          bool use_safe_run = ny_safe_run_requested(&opt->safe_run) ||
                              (opt->sanitize && *opt->sanitize);
          if (use_safe_run) {
            ny_jit_safe_call_t call = {saddr,
                                       call_implicit_main ? main_addr : 0};
            int rc = ny_safe_run_call(
                &opt->safe_run, ny_jit_safe_child, &call,
                opt->input_file ? opt->input_file : "inline ORC JIT workload");
            if (rc != 0)
              exit_code = rc;
          } else if (!ny_jit_prepare_call(saddr,
                                          call_implicit_main ? main_addr : 0)) {
            NY_LOG_ERR("JIT code memory is not executable\n");
            exit_code = 1;
          } else {
            ((void (*)(void))saddr)();
            if (call_implicit_main && main_addr)
              (void)((int64_t (*)(void))main_addr)();
            rt_print_flush();
          }
        }
        maybe_log_phase_time(opt->do_timing, "JIT Run:", t_run);
        ny_orc_jit_remove_module(rt);
        goto jit_execution_done;
      }

      if (opt->debug_symbols)
        LLVMStripModuleDebugInfo(jmod);
      if (ny_jit_module_is_apple_arm64(jmod))
        ny_drop_jit_llvm_used_metadata(jmod);
      struct LLVMMCJITCompilerOptions jopt;
      ny_jit_init_options(&jopt, jmod);
      jopt.OptLevel = (unsigned)ny_jit_effective_codegen_opt_level(opt, jmod);
      {
        const char *fast_isel_env = getenv("NYTRIX_JIT_FAST_ISEL");
        if (fast_isel_env && *fast_isel_env) {
          jopt.EnableFastISel = ny_env_is_truthy(fast_isel_env) ? 1 : 0;
        } else if (!ny_jit_module_is_apple_arm64(jmod)) {
          jopt.EnableFastISel = 1;
        }
      }
      ny_jit_register_module_symbols(jmod, &cg);
      if (LLVMCreateMCJITCompilerForModule(&ee, jmod, &jopt, sizeof(jopt),
                                           &err)) {
        ny_progress_task_end(progress_node);
        NY_LOG_ERR("JIT failed: %s\n", err);
        dump_debug_bundle(opt, user_src, jmod, "jit",
                          err ? err : "MCJIT setup failed");
        debug_bundle_written = opt->dump_on_error;
        exit_code = 1;
        goto exit_success;
      }
      cg.module = NULL;
      if (ny_env_enabled("NYTRIX_JIT_MAP_STRINGS")) {
        for (size_t i = 0; i < cg.interns.len; i++) {
          if (cg.interns.data[i].gv) {
            LLVMAddGlobalMapping(
                ee, cg.interns.data[i].gv,
                (void *)((char *)cg.interns.data[i].data - 64));
          }
          if (cg.interns.data[i].val &&
              cg.interns.data[i].val != cg.interns.data[i].gv) {
            LLVMAddGlobalMapping(ee, cg.interns.data[i].val,
                                 &cg.interns.data[i].data);
          }
        }
      }
      register_jit_symbols(ee, jmod, &cg);
      if (cg.had_error) {
        ny_progress_task_end(progress_node);
        if (show_progress)
          ny_progress_finish();
        LLVMDisposeExecutionEngine(ee);
        exit_code = 1;
        goto exit_success;
      }
      ny_jit_map_unresolved_symbols(ee, jmod, NULL);
      ny_jit_write_perf_map(ee, jmod);
      maybe_log_phase_time(opt->do_timing, "JIT Init:", t_jit);
      ny_tick_t t_exec = opt->do_timing ? ny_ticks_now() : 0;
      uint64_t saddr = jit_script_fn
                           ? (uint64_t)LLVMGetPointerToGlobal(ee, jit_script_fn)
                           : 0;
      if (!saddr)
        saddr = LLVMGetFunctionAddress(ee, "_ny_top_entry");
      maybe_log_phase_time(opt->do_timing, "JIT Compile:", t_exec);
      ny_progress_task_end(progress_node);
      if (show_progress)
        ny_progress_finish();
      ny_tick_t t_run = opt->do_timing ? ny_ticks_now() : 0;
      if (saddr) {
        uint64_t main_addr =
            (jit_main_fn && !ny_program_has_explicit_main_entry(&cg, cg.prog))
                ? (uint64_t)LLVMGetPointerToGlobal(ee, jit_main_fn)
                : 0;
        bool jit_use_safe_run2 = ny_safe_run_requested(&opt->safe_run) ||
                                 (opt->sanitize && *opt->sanitize);
        if (jit_use_safe_run2) {
          ny_jit_safe_call_t call = {saddr, main_addr};
          int rc = ny_safe_run_call(&opt->safe_run, ny_jit_safe_child, &call,
                                    opt->input_file ? opt->input_file
                                                    : "inline JIT workload");
          if (rc != 0)
            exit_code = rc;
        } else if (!ny_jit_prepare_call(saddr, main_addr)) {
          NY_LOG_ERR("JIT code memory is not executable\n");
          exit_code = 1;
        } else {
          if (verbose_enabled >= 3)
            NY_LOG_V3("JIT: executing script\n");
          ((void (*)(void))saddr)();
          if (main_addr)
            (void)((int64_t (*)(void))main_addr)();
          rt_print_flush();
        }
        if (verbose_enabled >= 3)
          NY_LOG_V3("JIT: script finished\n");
      } else {
        if (verbose_enabled >= 3)
          NY_LOG_V3("JIT: script entry was not found\n");
      }
      maybe_log_phase_time(opt->do_timing, "JIT Run:", t_run);
      LLVMDisposeExecutionEngine(ee);
    jit_execution_done:;
    }
  }
exit_success:
  if (exit_code != 0 && opt && opt->dump_on_error && !debug_bundle_written)
    dump_debug_bundle(opt, user_src, cg.module, "pipeline",
                      "compilation or execution returned a failure status");
  if (exit_code != 0)
    ny_dump_diagnose_failure(opt, user_src, exit_code);
  if (show_progress)
    ny_progress_finish();
#ifndef _WIN32
  if (native_cache_handle)
    dlclose(native_cache_handle);
  if (native_cache_file)
    free(native_cache_file);
#endif
  if (user_src)
    free(user_src);
  if (std_src)
    free(std_src);
  if (source)
    free(source);
  if (type_errors_json)
    free(type_errors_json);
  if (uses)
    ny_str_list_free(uses, use_count);
  free(jit_cache_file);
  free(auto_std_bc_cache);
  codegen_dispose(&cg);
  program_free(&prog, arena);
  ny_lookup_prof_note_pipeline_ms(ny_ticks_elapsed_ms(pipeline_prof_t0));
  if (opt->emit_report_path) {
    FILE *report = fopen(opt->emit_report_path, "ab");
    if (!report) {
      NY_LOG_ERR("Failed to append timing report %s: %s\n",
                 opt->emit_report_path, strerror(errno));
      exit_code = 1;
      goto exit_success;
    }
    fprintf(report, "compile.pipeline_elapsed_ms=%.3f\n",
            ny_ticks_elapsed_ms(pipeline_prof_t0));
    fclose(report);
  }
  maybe_log_phase_time(opt->do_timing, "Total time:", t_start);
  return exit_code;
}
