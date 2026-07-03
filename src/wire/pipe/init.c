#define _CRT_RAND_S
#include "wire/pipe.h"
#include "base/common.h"
#include "base/hash.h"
#include "base/loader.h"
#include "base/progress.h"
#include "base/time.h"
#include "base/util.h"
#include "code/code.h"
#include "code/jit.h"
#include "code/llvm.h"
#include "code/native/native.h"
#include "code/priv.h"
#include "code/typepipeline.h"
#include "parse/json.h"
#include "parse/parser.h"
#include "repl/repl.h"
#include "wire/build.h"
#include "wire/bundle.h"
#include "wire/cache.h"
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
#include <ctype.h>
#include <stdarg.h>

extern int64_t rt_free(int64_t ptr);

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

#include "tiny.c"
#include "stage.c"
#include "cache.c"
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
  fprintf(stderr, "%-12s %.4fs\n", label, ny_ticks_elapsed_sec(start_time));
}

static bool handle_non_compile_modes(ny_options *opt, int *exit_code) {
  if (opt->mode == NY_MODE_VERSION) {
    printf("Nytrix %s\n", VERSION);
    *exit_code = 0;
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
      if ((!env_std || !*env_std) && (!env_no_std || !*env_no_std)) {
        repl_std_mode = STD_MODE_NONE;
      }
    }
    char *repl_stdin_src = NULL;
    if (repl_batch && !opt->command_string) {
      char *stdin_src = ny_read_stdin_all();
      if (!stdin_src) {
        NY_LOG_ERR("Failed to read REPL stdin\n");
        *exit_code = 1;
        return true;
      }
      if (ny_env_enabled("NYTRIX_REPL_TRACE"))
        fprintf(stderr, "[repl-batch] bytes=%zu fast_candidate=%d\n",
                strlen(stdin_src),
                ny_repl_batch_can_fast_run(stdin_src) ? 1 : 0);
      if (ny_repl_batch_can_fast_run(stdin_src)) {
        if (ny_env_enabled("NYTRIX_REPL_TRACE"))
          fprintf(stderr, "[repl-batch] dispatch=run\n");
        ny_options run_opt = *opt;
        run_opt.mode = NY_MODE_RUN;
        run_opt.command_string = stdin_src;
        run_opt.input_file = NULL;
        int rc = ny_pipeline_run(&run_opt);
        free(stdin_src);
        *exit_code = rc;
        return true;
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
                repl_batch);
    free(repl_stdin_src);
    *exit_code = 0;
    return true;
  }
  return false;
}

static char *ny_normalize_command_source(const char *src) {
  if (!src)
    return ny_strdup("");

  size_t len = strlen(src);
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
                                      LLVMModuleRef module, int opt_level) {
  if (!opt || !opt->dump_diagnose || !module)
    return;
  ny_ensure_dir_recursive(ny_dump_dir(opt));
  char asm_path[4096];
  char bc_path[4096];
  char summary_path[4096];
  ny_dump_path(asm_path, sizeof(asm_path), opt, "diag.s");
  ny_dump_path(bc_path, sizeof(bc_path), opt, "diag.bc");
  ny_dump_path(summary_path, sizeof(summary_path), opt, "diag.summary.txt");
  LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, module);
  LLVMModuleRef art_mod = dump_mod ? dump_mod : module;
  (void)ny_llvm_emit_file(art_mod, asm_path, LLVMAssemblyFile, opt_level);
  (void)ny_reemit_bitcode_via_ir(art_mod, bc_path);
  ny_write_ir_stats_file(opt, "diag.stats.txt", art_mod);
  {
    const char *scope =
        (opt->dump_scope == NY_DUMP_SCOPE_LIB)
            ? "lib"
            : ((opt->dump_scope == NY_DUMP_SCOPE_BOTH) ? "both" : "program");
    char summary[1024];
    int n = snprintf(
        summary, sizeof(summary),
        "dump_dir=%s\nscope=%s\nwarn_level=%d\ndiag_compact=%d\nopt_level=%d\n",
        ny_dump_dir(opt), scope, opt->warn_level, opt->diag_compact ? 1 : 0,
        opt_level);
    if (n > 0)
      ny_write_file(summary_path, summary, (size_t)n);
  }
  if (dump_mod)
    LLVMDisposeModule(dump_mod);
}

static bool ny_ir_is_std_symbol(const char *name) {
  if (!name || !*name)
    return false;
  return (strncmp(name, "std.", 4) == 0 || strncmp(name, "lib.", 4) == 0 ||
          strncmp(name, "src.std.", 8) == 0 ||
          strncmp(name, "src.lib.", 8) == 0);
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
  const char *name = LLVMGetValueName(v);
  return ny_ir_is_std_symbol(name);
}

static void ny_ir_externalize_std_definitions(const ny_options *opt,
                                              LLVMModuleRef module) {
  (void)opt;
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;) {
    LLVMValueRef next_fn = LLVMGetNextFunction(fn);
    if (ny_ir_is_std_value(fn) && LLVMCountBasicBlocks(fn) > 0) {
      for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;) {
        LLVMBasicBlockRef next_bb = LLVMGetNextBasicBlock(bb);
        LLVMDeleteBasicBlock(bb);
        bb = next_bb;
      }
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
      for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;) {
        LLVMBasicBlockRef next_bb = LLVMGetNextBasicBlock(bb);
        LLVMDeleteBasicBlock(bb);
        bb = next_bb;
      }
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
  ny_dump_scope_t scope = NY_DUMP_SCOPE_PROGRAM;
  if (opt)
    scope = opt->dump_scope;
  if (scope == NY_DUMP_SCOPE_PROGRAM)
    ny_ir_externalize_std_definitions(opt, dump_mod);
  else if (scope == NY_DUMP_SCOPE_LIB)
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

static void ny_link_lib_vec_add_option(ny_link_lib_vec *libs,
                                       const char *lib) {
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
    bool is_std = (strcmp(u, "std") == 0 || strncmp(u, "std.", 4) == 0);
    bool is_lib = (strcmp(u, "lib") == 0 || strncmp(u, "lib.", 4) == 0);
    if (has_project_std && ny_use_name_is_project_std_module(u))
      *has_project_std = true;
    if (has_local && !is_std && !is_lib) {
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
  std->prebuilt_path = resolve_std_path(
      opt->std_path ? opt->std_path
                    : (NYTRIX_STD_PATH ? NYTRIX_STD_PATH : "build/std.ny"));
  if (opt->no_std)
    std->mode = STD_MODE_NONE;
  if (std->mode == STD_MODE_DEFAULT && use_count == 0)
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

  if (opt->std_bc_path && ny_access(opt->std_bc_path, R_OK) == 0) {
    std->bc_cache = opt->std_bc_path;
    std->use_bc_cache = true;
  } else {
    const char *env_bc = getenv("NYTRIX_STD_BC_CACHE");
    if (std->mode != STD_MODE_NONE && env_bc && *env_bc &&
        ny_access(env_bc, R_OK) == 0) {
      std->bc_cache = env_bc;
      std->use_bc_cache = true;
    } else if (std->mode != STD_MODE_NONE && !opt->run_jit &&
               !std->has_local && !has_project_std &&
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
        } else {
          (void)unlink(std->auto_bc_cache);
        }
      }
    }
  }

  if (std->mode != STD_MODE_NONE && prebuilt_ok &&
      (prebuilt_preferred || prebuilt_required)) {
    if (verbose_enabled)
      NY_LOG_INFO("Using prebuilt std.ny: %s\n", std->prebuilt_path);
    std->src = ny_read_file(std->prebuilt_path);
    if (!std->src && verbose_enabled) {
      NY_LOG_WARN("Failed to read prebuilt std.ny: %s (falling back)\n",
                  std->prebuilt_path);
    }
  }

  if (std->mode != STD_MODE_NONE && !std->src) {
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
              snprintf(expect, sizeof(expect), "; ny_std_cache_v10 %016llx\n",
                       (unsigned long long)std_cache_sig);
          if (nw <= 0 || (size_t)nw >= sizeof(expect) ||
              strncmp(std->src, expect, (size_t)nw) != 0) {
            free(std->src);
            std->src = NULL;
            (void)unlink(std_cache_path);
          }
        }
        if (std->src && verbose_enabled >= 2)
          NY_LOG_INFO("Using std cache: %s\n", std_cache_path);
      }
    }
    if (!std->src) {
      std->src = ny_build_std_source((const char **)uses, use_count, std->mode,
                                     opt->verbose, opt->input_file);
      if (std->src && use_std_cache && std_cache_path &&
          std_cache_path[0] != '\0' && std_cache_sig) {
        char header[128];
        int hn = snprintf(header, sizeof(header), "; ny_std_cache_v10 %016llx\n",
                          (unsigned long long)std_cache_sig);
        if (hn > 0 && (size_t)hn < sizeof(header)) {
          size_t sl = strlen(std->src);
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

  NY_LOG_ERR("Could not load std.ny or standard library source files.\n");
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
  if (ulen > SIZE_MAX - total ||
      line_directive_len > SIZE_MAX - total - ulen ||
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
    int n = snprintf(ptr, line_directive_len + 1, "#line 1 \"%s\"\n",
                     parse_name);
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

int ny_pipeline_run(ny_options *opt) {
  int exit_code = 0;
  ny_tick_t pipeline_prof_t0 = ny_ticks_now();
  ny_lookup_prof_register_atexit();
  if (handle_non_compile_modes(opt, &exit_code))
    return exit_code;
  ny_diag_configure(opt ? opt->warn_level : 1, opt ? opt->diag_compact : false);
  if (opt && opt->dump_diagnose)
    ny_ensure_dir_recursive(ny_dump_dir(opt));
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
  if (ny_try_fast_command_string(opt, t_start))
    return 0;
  bool show_progress = opt->progress ||
#ifdef _WIN32
                       ny_progress_enabled_from_env();
#else
                       (!opt->no_progress && isatty(STDERR_FILENO)) ||
                       ny_progress_enabled_from_env();
#endif
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
    fprintf(stderr, "[trace] imports (%zu):", use_count);
    for (size_t i = 0; i < use_count; i++) {
      fprintf(stderr, " %s", uses[i] ? uses[i] : "<null>");
    }
    fprintf(stderr, "\n");
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
  maybe_log_phase_time(opt->do_timing, "Stdlib load:", t_std);
  size_t ulen = 0;
  size_t split_pos = 0;
  source = ny_pipeline_join_sources(std_src, user_src, parse_name, &ulen,
                                    &split_pos);
  if (!source) {
    exit_code = 1;
    goto exit_success;
  }
#ifndef _WIN32
  if (opt->run_jit && !opt->command_string && ny_should_use_jit_cache(opt) &&
      ny_jit_native_cache_enabled() && !opt->dump_ast && !opt->expand &&
      !opt->dump_llvm && !opt->emit_ir_path && !opt->emit_bc_path &&
      !opt->dump_tokens && !opt->dump_diagnose) {
    char *early_bc = ny_jit_cache_path(source, prebuilt_path, 0, opt->opt_level,
                                       opt->opt_dce, opt->opt_internalize,
                                       opt->debug_symbols,
                                       (unsigned long)ny_std_latest_mtime());
    if (early_bc) {
      char *early_so = ny_jit_native_cache_path(early_bc);
      if (early_so) {
        if (ny_jit_native_cache_load(early_so, &native_cache_handle,
                                     &native_cache_entry)) {
          if (opt->verbose)
            fprintf(stderr, "JIT native cache hit (early): %s\n", early_so);
          loaded_from_cache = true;
          free(early_so);
          free(early_bc);
          goto skip_compilation;
        }
        free(early_so);
      }
      free(early_bc);
    }
  }
#endif
  if (ny_should_use_aot_cache(opt)) {
    ny_build_aot_cache_path(opt, source, parse_name, prebuilt_path, output_path,
                            aot_cache_path, sizeof(aot_cache_path));
    if (aot_cache_path[0] != '\0' && ny_access(aot_cache_path, R_OK) == 0 &&
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
  if (opt->dump_tokens) {
    lexer_t lx;
    lexer_init(&lx, source, parse_name);
    for (;;) {
      token_t t = lexer_next(&lx);
      printf("%d:%d kind=%d lexeme='%.*s'\n", t.line, t.col, t.kind, (int)t.len,
             t.lexeme);
      if (t.kind == NY_T_EOF)
        break;
    }
    goto exit_success;
  }
  ny_tick_t t_parse = opt->do_timing ? ny_ticks_now() : 0;
  parser_t parser;
  arena = (arena_t *)malloc(sizeof(arena_t));
  memset(arena, 0, sizeof(arena_t));
  parser_init_with_arena(&parser, source, std_src ? "<stdlib>" : parse_name,
                         arena);
  if (opt->max_errors >= 0)
    parser.error_limit = opt->max_errors;
  if (std_src) {
    parser.lex.split_pos = split_pos;
    ny_sym_id split_file_id = ny_intern_cstr(parse_name);
    parser.lex.split_filename =
        split_file_id ? ny_intern_get(split_file_id) : parse_name;
  }
  progress_node = ny_progress_task_begin("parse", 1);
  prog = parse_program(&parser);
  ny_progress_task_end(progress_node);
  maybe_log_phase_time(opt->do_timing, "Parsing:", t_parse);
  if (parser.had_error) {
    NY_LOG_ERR("Compilation failed: %d errors\n", parser.error_count);
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_PARSE, parse_name,
                               "parse failed", parser.error_count);
    dump_debug_bundle(opt, source, NULL);
    exit_code = 1;
    goto exit_success;
  }
  ny_ast_verify_program(&prog, "parse");
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
  if (opt->safe_mode && !ny_safe_mode_validate_raw_memory(&prog)) {
    dump_debug_bundle(opt, source, NULL);
    exit_code = 1;
    goto exit_success;
  }

  ny_tick_t t_codegen = opt->do_timing ? ny_ticks_now() : 0;
  NY_LOG_V2("Initializing codegen_t for module 'nytrix'\n");
  codegen_init(&cg, &prog, arena, "nytrix");
  cg.source_main_file = parse_name;
  cg.type_solver = opt->type_solver_raw ? opt->type_solver_raw : "auto";
  cg.c_frontend = opt->c_frontend_raw ? opt->c_frontend_raw : "auto";
  cg.strict_types = opt->strict_types;
  cg.ownership_enabled = opt->ownership || opt->ownership_strict || opt->borrow_check;
  cg.ownership_strict = opt->ownership_strict || opt->borrow_check;
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
      !opt->dump_diagnose) {
    jit_cache_file = ny_jit_cache_path(source, prebuilt_path, 0, opt->opt_level,
                                       opt->opt_dce, opt->opt_internalize,
                                       opt->debug_symbols,
                                       (unsigned long)ny_std_latest_mtime());
#ifndef _WIN32
    if (jit_cache_file && opt->run_jit && !opt->command_string &&
        ny_jit_native_cache_enabled()) {
      native_cache_file = ny_jit_native_cache_path(jit_cache_file);
      if (native_cache_file &&
          ny_jit_native_cache_load(native_cache_file, &native_cache_handle,
                                   &native_cache_entry)) {
        if (opt->verbose)
          fprintf(stderr, "JIT native cache hit: %s\n", native_cache_file);
        loaded_from_cache = true;
        goto skip_compilation;
      }
    }
#endif
    if (jit_cache_file) {
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
          goto skip_compilation;
        }
      }
    }
  }

  if (opt->dump_ast) {
    for (size_t i = 0; i < prog.body.len; i++) {
      stmt_t *s = prog.body.data[i];
      printf("  [%zu] Kind=%d\n", i, s->kind);
    }
  }
  bool parallel_modules = false;
#ifndef _WIN32
  ny_module_list mods = {0};
  if (!fast_compiler && !use_std_bc_cache && ny_parallel_modules_enabled(opt)) {
    ny_collect_top_modules(&prog, &mods);
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
    NY_LOG_INFO("linking stdlib bitcode cache: %s\n", std_bc_cache);
    (void)ny_std_bc_cache_load_links(std_bc_cache, &cg);
  }
  if (opt->emit_module) {
    cg.emit_module_name = opt->emit_module;
    cg.emit_module_decls_only = true;
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
  NY_LOG_V2("Preparing codegen (analysis & links)...\n");
  progress_node = ny_progress_task_begin("prepare codegen", 1);
  codegen_prepare(&cg);
  ny_progress_task_end(progress_node);
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
  progress_node = ny_progress_task_begin("validate types", 1);
  int type_errors = ny_type_pipeline_validate_semantics(
      &prog, &cg, parse_name, opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM,
      max_type_stage, &failed_type_stage, true, &type_errors_json);
  ny_progress_task_end(progress_node);
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
            if (!ny_spawn_module_job(opt, mods.names[started], tmp_dir,
                                     &mod_jobs[started])) {
              parallel_modules = false;
              break;
            }
            started++;
            running++;
          }
          if (!parallel_modules)
            break;
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
            if (mod_jobs[i].exit_code != 0)
              parallel_modules = false;
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
      if (mod_jobs[i].bc_path)
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
  codegen_emit(&cg);
  ny_progress_task_end(progress_node);
  fflush(stderr);
  if (cg.had_error) {
    NY_LOG_ERR("Codegen failed\n");
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_ABI, parse_name,
                               "codegen failed", 1);
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  fflush(stderr);
  if (use_std_bc_cache) {
    ny_ir_externalize_std_definitions(opt, cg.module);
    if (!ny_link_module_cache(cg.ctx, cg.module, std_bc_cache)) {
      NY_LOG_ERR("Failed to link std cache: %s\n", std_bc_cache);
      exit_code = 1;
      goto exit_success;
    }
    codegen_rebind_llvm_symbols(&cg);
    ny_drop_jit_llvm_used_metadata(cg.module);
  }
  if (cg.emit_script) {
    fflush(stderr);
    NY_LOG_V2("Emitting script entry point...\n");
    script_fn = codegen_emit_script(&cg, opt->entry_name ? opt->entry_name
                                                         : "_ny_top_entry");
    if (cg.had_error) {
      NY_LOG_ERR("Codegen script entry failed\n");
      ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_ABI, parse_name,
                                 "codegen script entry failed", 1);
      dump_debug_bundle(opt, source, cg.module);
      exit_code = 1;
      goto exit_success;
    }
  }
#ifndef _WIN32
  if (parallel_modules && mod_jobs) {
    fflush(stderr);
    for (size_t i = 0; i < mod_job_count; i++) {
      if (!mod_jobs[i].bc_path)
        continue;
      if (!ny_link_module_cache(cg.ctx, cg.module, mod_jobs[i].bc_path)) {
        NY_LOG_ERR("Failed to link module cache for %s\n",
                   mod_jobs[i].name ? mod_jobs[i].name : "<module>");
        exit_code = 1;
        goto exit_success;
      }
      codegen_rebind_llvm_symbols(&cg);
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
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  if (opt->do_timing && opt->verify_module)
    fprintf(stderr, "Verify:       %.4fs\n", ny_ticks_elapsed_sec(t_ver));
  if (write_compile_caches && !use_std_bc_cache && auto_std_bc_cache &&
      cg.module &&
      !loaded_from_cache && !opt->emit_module && std_mode != STD_MODE_NONE &&
      !has_local) {
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

  if (!fast_compiler) {
    int eff_opt = opt->opt_level;
    if (parallel_modules && !ny_env_enabled("NYTRIX_PARALLEL_OPT_LINK")) {
      eff_opt = 0;
    }
    if (opt->run_jit) {
      eff_opt = ny_jit_effective_ir_opt_level(opt, cg.module, eff_opt);
    }

    if (cg.di_builder) {
      codegen_debug_finalize(&cg);
    }
    progress_node = ny_progress_task_begin("optimize llvm", 1);
    ny_llvm_optimize_module(cg.module, eff_opt, opt->opt_loops,
                            opt->opt_pipeline);
    ny_progress_task_end(progress_node);
    ny_trace_ir_stats("post_opt", cg.module);
    if (opt->do_timing && (opt->opt_level > 0 || opt->opt_pipeline))
      fprintf(stderr, "Optimization: %.4fs\n", ny_ticks_elapsed_sec(t_opt));
  }
  ny_dump_diagnose_ir_stage(opt, cg.module, "diag.post.ll", "post-opt");
  if (opt->stop_after == NY_STOP_AFTER_OPT) {
    ny_stage_emit_artifact(opt, NY_STOP_AFTER_OPT, &prog, &cg, parse_name,
                           cg.module, true);
    goto exit_success;
  }
  if ((opt->emit_artifact_path || opt->emit_shapes) &&
      opt->stop_after == NY_STOP_AFTER_NONE) {
    ny_stage_emit_artifact(opt, NY_STOP_AFTER_OPT, &prog, &cg, parse_name,
                           cg.module, false);
  }
  if (write_compile_caches && jit_cache_file && !loaded_from_cache) {
#ifndef _WIN32
    if (native_cache_file && opt->run_jit && !opt->command_string) {
      ny_tick_t t_native = opt->do_timing ? ny_ticks_now() : 0;
      if (ny_jit_native_cache_save(native_cache_file, cg.module, opt->opt_level,
                                   (const char *const *)cg.links.data,
                                   cg.links.len)) {
        if (opt->verbose)
          fprintf(stderr, "JIT native cache saved: %s\n", native_cache_file);
      }
      maybe_log_phase_time(opt->do_timing, "Native Cache:", t_native);
    }
#endif
    if (ny_jit_cache_save(jit_cache_file, cg.module)) {
      if (opt->verbose)
        fprintf(stderr, "JIT cache saved: %s\n", jit_cache_file);
    }
  }
skip_compilation:
  if (opt->native_tier_report) {
    char native_tier_err[512] = {0};
    if (!ny_native_write_tier_report_for_program(&prog, opt, native_tier_err,
                                                 sizeof(native_tier_err))) {
      NY_LOG_ERR("Failed to write native tier report: %s\n",
                 native_tier_err[0] ? native_tier_err : "unknown error");
      exit_code = 1;
      goto exit_success;
    }
  }
  if (opt->native_result_oracle) {
    char native_oracle_err[512] = {0};
    if (!ny_native_result_oracle_for_program(&prog, opt, native_oracle_err,
                                             sizeof(native_oracle_err))) {
      NY_LOG_ERR("Native result oracle failed: %s\n",
                 native_oracle_err[0] ? native_oracle_err : "unknown error");
      exit_code = 1;
      goto exit_success;
    }
  }
  if (opt->native_dump_ir &&
      (opt->native_backend != NY_NATIVE_BACKEND_LLVM || opt->nyir_run ||
       opt->nyir_metadata_report)) {
    char native_dump_err[512] = {0};
    if (!ny_native_dump_ir_for_program(&prog, opt, native_dump_err,
                                       sizeof(native_dump_err))) {
      NY_LOG_ERR("Failed to dump native NYIR: %s\n",
                 native_dump_err[0] ? native_dump_err : "unknown error");
      exit_code = 1;
      goto exit_success;
    }
  }
  if (jit_cache_file) {
    free(jit_cache_file);
    jit_cache_file = NULL;
  }
  if (write_compile_caches && !auto_std_bc_cache_saved && !use_std_bc_cache &&
      auto_std_bc_cache &&
      cg.module &&
      !loaded_from_cache && !opt->emit_module && std_mode != STD_MODE_NONE &&
      !has_local) {
    bool need_bc = ny_access(auto_std_bc_cache, R_OK) != 0;
    bool can_save_links = !need_bc || ny_save_std_bc_cache_from_module(
                                          cg.module, auto_std_bc_cache);
    if (can_save_links &&
        (need_bc || auto_std_bc_cache_needs_links ||
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
    ny_trace_file_size("emit_ir", opt->emit_ir_path);
  }
  if (opt->emit_bc_path) {
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
  if (opt->emit_asm_path) {
    ny_ensure_parent_dir_for_path(opt->emit_asm_path);
    if (opt->native_backend != NY_NATIVE_BACKEND_LLVM) {
      char native_err[512] = {0};
      if (!ny_native_emit_asm(&prog, opt, opt->emit_asm_path, native_err,
                              sizeof(native_err))) {
        NY_LOG_ERR("Failed to write native assembly to %s: %s\n",
                   opt->emit_asm_path, native_err[0] ? native_err : "unknown error");
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
    ny_trace_file_size("emit_asm", opt->emit_asm_path);
  }
  ny_dump_diagnose_finalize(opt, cg.module, opt->opt_level);
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
      bool obj_ok = use_native_object
                        ? ny_native_emit_object(&prog, opt, output_path,
                                                "rt_main", false,
                                                native_obj_err,
                                                sizeof(native_obj_err))
                        : ny_llvm_emit_object(cg.module, output_path,
                                             opt->opt_level);
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
      bool emitted_obj = use_native_object
                             ? ny_native_emit_object(&prog, opt, obj,
                                                     "main", false,
                                                     native_obj_err,
                                                     sizeof(native_obj_err))
                             : ny_llvm_emit_object(cg.module, obj,
                                                  opt->opt_level);
      ny_progress_task_end(progress_node);
      maybe_log_phase_time(opt->do_timing, "Emit obj:", t_emit_obj);
      if (!emitted_obj) {
        NY_LOG_ERR("Failed to emit object file%s%s\n",
                   native_obj_err[0] ? ": " : "", native_obj_err);
        dump_debug_bundle(opt, source, cg.module);
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
      int runtime_speed_level = runtime_speed ? 3 : 0;
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
                                      runtime_native)) {
        maybe_log_phase_time(opt->do_timing, "Runtime obj:", t_runtime_obj);
        unlink(obj);
        dump_debug_bundle(opt, source, cg.module);
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
              output_path, link_strip, opt->debug_symbols, opt->gprof == 1)) {
        ny_progress_task_end(progress_node);
        maybe_log_phase_time(opt->do_timing, "Link:", t_link);
        unlink(obj);
        unlink(rto);
        dump_debug_bundle(opt, source, cg.module);
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
        (void)ny_copy_file(output_path, aot_cache_path);
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
      const char *argv_exec[] = {output_path, NULL};
      int rc = ny_exec_spawn(argv_exec);
      if (rc != 0)
        exit_code = rc;
      if (aot_run_temp)
        (void)unlink(output_path);
    }
  }
  if (opt->run_jit) {
#ifndef _WIN32
    if (native_cache_entry) {
      progress_node = ny_progress_task_begin("jit cache", 1);
      ny_progress_task_end(progress_node);
      if (show_progress)
        ny_progress_finish();
      ny_tick_t t_run = opt->do_timing ? ny_ticks_now() : 0;
      native_cache_entry();
      extern void rt_print_flush(void);
      rt_print_flush();
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
      if (LLVMCreateMCJITCompilerForModule(&ee, jmod, &jopt, sizeof(jopt),
                                           &err)) {
        ny_progress_task_end(progress_node);
        NY_LOG_ERR("JIT failed: %s\n", err);
        dump_debug_bundle(opt, source, jmod);
        exit_code = 1;
        goto exit_success;
      }
      cg.module = NULL;
      {
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
      }
      register_jit_symbols(ee, jmod, &cg);
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
        ny_jit_prepare_execution();
        if (verbose_enabled >= 3)
          fprintf(stderr, "TRACE: Executing script...\n");
        ((void (*)(void))saddr)();
        uint64_t main_addr =
            (jit_main_fn && !ny_program_has_explicit_main_entry(&cg, cg.prog))
                ? (uint64_t)LLVMGetPointerToGlobal(ee, jit_main_fn)
                : 0;
        if (main_addr) {
          if (verbose_enabled >= 3)
            fprintf(stderr, "TRACE: Executing main...\n");
          (void)((int64_t (*)(void))main_addr)();
        }
        extern void rt_print_flush(void);
        rt_print_flush();
        if (verbose_enabled >= 3)
          fprintf(stderr, "TRACE: Script finished.\n");
      } else {
        if (verbose_enabled >= 3)
          fprintf(stderr, "TRACE: __script_top NOT FOUND\n");
      }
      maybe_log_phase_time(opt->do_timing, "JIT Run:", t_run);
      LLVMDisposeExecutionEngine(ee);
    }
  }
exit_success:
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
  maybe_log_phase_time(opt->do_timing, "Total time:", t_start);
  return exit_code;
}
