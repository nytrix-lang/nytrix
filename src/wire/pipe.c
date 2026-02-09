#include "wire/pipe.h"
#include "base/common.h"
#include "base/loader.h"
#include "base/util.h"
#include "code/code.h"
#include "code/jit.h"
#include "code/llvm.h"
#include "parse/parser.h"
#include "repl/repl.h"
#include "wire/build.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Support.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void dump_debug_bundle(const ny_options *opt, const char *source,
                              LLVMModuleRef module) {
  if (!opt || !opt->dump_on_error)
    return;
  ny_ensure_dir("build");
  ny_ensure_dir("build/debug");
  if (source) {
    ny_write_file("build/debug/last_source.ny", source, strlen(source));
  }
  if (module) {
    char *err = NULL;
    if (LLVMPrintModuleToFile(module, "build/debug/last_ir.ll", &err) != 0) {
      if (err) {
        NY_LOG_ERR("Failed to write IR dump: %s\n", err);
        LLVMDisposeMessage(err);
      }
    }
    ny_llvm_emit_file(module, "build/debug/last_asm.s", LLVMAssemblyFile);
  }
  NY_LOG_ERR("Debug bundle saved under build/debug/\n");
  {
    const size_t max_lines = 14;
    const char *paths[] = {"build/debug/last_ir.ll", "build/debug/last_asm.s"};
    const char *labels[] = {"IR snippet", "ASM snippet"};
    for (size_t i = 0; i < 2; i++) {
      char *content = ny_read_file(paths[i]);
      if (!content)
        continue;
      NY_LOG_ERR("--- %s (%s) ---\n", labels[i], paths[i]);
      size_t lines = 0;
      for (char *p = content; *p && lines < max_lines; p++) {
        fputc(*p, stderr);
        if (*p == '\n')
          lines++;
      }
      if (lines >= max_lines)
        NY_LOG_ERR("...\n");
      free(content);
    }
  }
}
// Helper functions for module scanning
static char *dup_string_token(token_t t) {
  if (t.len < 2)
    return NULL;
  size_t head = 1, tail = 1;
  if (t.len >= 6 && t.lexeme[0] == t.lexeme[1] && t.lexeme[1] == t.lexeme[2]) {
    head = 3;
    tail = 3;
  }
  if (t.len < head + tail)
    return NULL;
  size_t out_len = t.len - head - tail;
  char *out = malloc(out_len + 1);
  if (!out)
    return NULL;
  memcpy(out, t.lexeme + head, out_len);
  out[out_len] = '\0';
  return out;
}

static char *parse_use_name(lexer_t *lx, token_t *entry_tok,
                            token_t *out_last_tok) {
  token_t t = *entry_tok;
  if (t.kind == NY_T_STRING) {
    char *name = dup_string_token(t);
    if (out_last_tok)
      *out_last_tok = lexer_next(lx);
    return name;
  }
  if (t.kind != NY_T_IDENT)
    return NULL;
  size_t cap = 64, len = 0;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  memcpy(buf, t.lexeme, t.len);
  len += t.len;
  for (;;) {
    token_t tok = lexer_next(lx);
    if (tok.kind == NY_T_DOT) {
      token_t id = lexer_next(lx);
      if (id.kind != NY_T_IDENT) {
        free(buf);
        return NULL;
      }
      if (len + 1 + id.len + 1 > cap) {
        cap = (len + 1 + id.len + 1) * 2;
        char *nb = realloc(buf, cap);
        if (!nb) {
          free(buf);
          return NULL;
        }
        buf = nb;
      }
      buf[len++] = '.';
      memcpy(buf + len, id.lexeme, id.len);
      len += id.len;
    } else {
      if (out_last_tok)
        *out_last_tok = tok;
      break;
    }
  }
  buf[len] = '\0';
  return buf;
}

static void append_use(char ***uses, size_t *len, size_t *cap,
                       const char *name) {
  for (size_t i = 0; i < *len; ++i) {
    if (strcmp((*uses)[i], name) == 0)
      return;
  }
  if (*len == *cap) {
    size_t new_cap = *cap ? (*cap * 2) : 8;
    char **tmp = realloc(*uses, new_cap * sizeof(char *));
    if (!tmp)
      return;
    *uses = tmp;
    *cap = new_cap;
  }
  (*uses)[(*len)++] = ny_strdup(name);
}

static char **collect_use_modules(const char *src, size_t *out_count) {
  lexer_t lx;
  lexer_init(&lx, src, "<collect_use>");
  int depth = 0;
  char **uses = NULL;
  size_t len = 0, cap = 0;
  token_t t = lexer_next(&lx);
  for (;;) {
    if (t.kind == NY_T_EOF)
      break;
    if (t.kind == NY_T_LBRACE) {
      depth++;
      t = lexer_next(&lx);
    } else if (t.kind == NY_T_RBRACE) {
      if (depth > 0)
        depth--;
      t = lexer_next(&lx);
    } else if (t.kind == NY_T_USE && depth == 0) {
      t = lexer_next(&lx);
      token_t next_tok;
      char *name = parse_use_name(&lx, &t, &next_tok);
      if (name) {
        append_use(&uses, &len, &cap, name);
        free(name);
      }
      t = next_tok;
    } else {
      t = lexer_next(&lx);
    }
  }
  if (out_count)
    *out_count = len;
  return uses;
}

static void append_std_prelude(char ***uses, size_t *len, size_t *cap) {
  size_t count = 0;
  const char **prelude = ny_std_prelude(&count);
  for (size_t i = 0; i < count; ++i) {
    append_use(uses, len, cap, prelude[i]);
  }
}

static const char *resolve_std_bundle(const char *compile_time_path) {
  const char *env = getenv("NYTRIX_STD_PREBUILT");
  if (env && *env && access(env, R_OK) == 0)
    return env;

  static char path[4096];
  // 0. Check relative to current directory
  if (access("build/std.ny", R_OK) == 0) {
    strcpy(path, "build/std.ny");
    return path;
  }
  // 1. Check relative to binary
  char *exe_dir = ny_get_executable_dir();
  if (exe_dir) {
    snprintf(path, sizeof(path), "%s/std.ny", exe_dir);
    if (access(path, R_OK) == 0)
      return path;
    snprintf(path, sizeof(path), "%s/../share/nytrix/std.ny", exe_dir);
    if (access(path, R_OK) == 0)
      return path;
  }

  // 2. Check source root
  const char *root = ny_src_root();
  snprintf(path, sizeof(path), "%s/build/std.ny", root);
  if (access(path, R_OK) == 0)
    return path;
  snprintf(path, sizeof(path), "%s/std.ny", root);
  if (access(path, R_OK) == 0)
    return path;

  // 3. Fallback to compile-time path
  if (compile_time_path && access(compile_time_path, R_OK) == 0)
    return compile_time_path;

  // 4. Hardcoded common paths
  const char *common[] = {"/usr/share/nytrix/std_bundle.ny",
                          "/usr/local/share/nytrix/std_bundle.ny"};
  for (int i = 0; i < 2; i++)
    if (access(common[i], R_OK) == 0)
      return common[i];

  return NULL;
}

static void ensure_aot_entry(codegen_t *cg, LLVMValueRef script_fn) {
  if (!cg || !cg->module || !script_fn)
    return;
  if (LLVMGetNamedFunction(cg->module, "main"))
    return;
  // Generate: int main(int argc, char **argv, char **envp) {
  //   __set_args((int64_t)argc, (int64_t)argv, (int64_t)envp);
  //   return (int)script_fn();
  // }
  LLVMTypeRef i32 = LLVMInt32TypeInContext(cg->ctx);
  LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);
  LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
  LLVMTypeRef ptrptr = LLVMPointerType(ptr, 0);

  LLVMTypeRef main_ty =
      LLVMFunctionType(i32, (LLVMTypeRef[]){i32, ptrptr, ptrptr}, 3, 0);
  LLVMValueRef main_fn = LLVMAddFunction(cg->module, "main", main_ty);
  LLVMBasicBlockRef entry =
      LLVMAppendBasicBlockInContext(cg->ctx, main_fn, "entry");
  LLVMBuilderRef builder = LLVMCreateBuilderInContext(cg->ctx);
  LLVMPositionBuilderAtEnd(builder, entry);

  LLVMValueRef argc = LLVMGetParam(main_fn, 0);
  LLVMValueRef argv = LLVMGetParam(main_fn, 1);
  LLVMValueRef envp = LLVMGetParam(main_fn, 2);

  LLVMValueRef argc_i64 = LLVMBuildSExt(builder, argc, i64, "");
  LLVMValueRef argv_i64 = LLVMBuildPtrToInt(builder, argv, i64, "");
  LLVMValueRef envp_i64 = LLVMBuildPtrToInt(builder, envp, i64, "");

  // Call __set_args
  LLVMValueRef set_args_fn = LLVMGetNamedFunction(cg->module, "__set_args");
  if (!set_args_fn) {
    // Look it up from builtin defs/internal declarations if possible, or
    // declare it
    LLVMTypeRef set_args_ty =
        LLVMFunctionType(i64, (LLVMTypeRef[]){i64, i64, i64}, 3, 0);
    set_args_fn = LLVMAddFunction(cg->module, "__set_args", set_args_ty);
  }
  LLVMBuildCall2(builder, LLVMGlobalGetValueType(set_args_fn), set_args_fn,
                 (LLVMValueRef[]){argc_i64, argv_i64, envp_i64}, 3, "");

  // Call script
  LLVMValueRef res_raw = LLVMBuildCall2(
      builder, LLVMGlobalGetValueType(script_fn), script_fn, NULL, 0, "");
  // result is tagged int64. convert to exit code (int32)
  LLVMValueRef res_int =
      LLVMBuildAShr(builder, res_raw, LLVMConstInt(i64, 1, 0), "");
  LLVMValueRef res_i32 = LLVMBuildTrunc(builder, res_int, i32, "");

  LLVMBuildRet(builder, res_i32);
  LLVMDisposeBuilder(builder);
}

static void maybe_log_phase_time(bool enabled, const char *label,
                                 clock_t start_time) {
  if (!enabled)
    return;
  fprintf(stderr, "%-12s %.4fs\n", label,
          (double)(clock() - start_time) / CLOCKS_PER_SEC);
}

static bool handle_non_compile_modes(const ny_options *opt, int *exit_code) {
  if (opt->mode == NY_MODE_VERSION) {
    printf("Nytrix v0.1.25\n");
    *exit_code = 0;
    return true;
  }
  if (opt->mode == NY_MODE_HELP) {
    ny_options_usage(opt->argv0 ? opt->argv0 : "ny");
    *exit_code = 0;
    return true;
  }
  if (opt->mode == NY_MODE_REPL) {
    LLVMLinkInMCJIT();
    ny_llvm_init_native();
    LLVMLoadLibraryPermanently(NULL);
    ny_repl_run(opt->opt_level, opt->opt_pipeline, opt->command_string, 0);
    *exit_code = 0;
    return true;
  }
  return false;
}

static char *load_user_source(const ny_options *opt) {
  if (opt->command_string)
    return ny_strdup(opt->command_string);
  if (opt->input_file)
    return ny_read_file(opt->input_file);
  return ny_strdup("fn main() { return 0\n }");
}

static char *build_prelude(bool no_std, bool implicit_prelude,
                           size_t *out_len) {
  *out_len = 0;
  if (no_std || !implicit_prelude)
    return NULL;

  size_t count = 0;
  const char **p_list = ny_std_prelude(&count);
  size_t cap = 1024;
  char *prelude = malloc(cap);
  if (!prelude)
    return NULL;
  prelude[0] = '\0';

  for (size_t i = 0; i < count; ++i) {
    char line[256];
    if (strcmp(p_list[i], "std.core") == 0 || strcmp(p_list[i], "std.io") == 0)
      sprintf(line, "use %s *;\n", p_list[i]);
    else
      sprintf(line, "use %s;\n", p_list[i]);
    size_t llen = strlen(line);
    if (*out_len + llen + 1 > cap) {
      cap *= 2;
      char *grown = realloc(prelude, cap);
      if (!grown) {
        free(prelude);
        return NULL;
      }
      prelude = grown;
    }
    strcpy(prelude + *out_len, line);
    *out_len += llen;
  }
  return prelude;
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

static void run_optimization_if_needed(const ny_options *opt,
                                       LLVMModuleRef module) {
  if (opt->opt_level <= 0 && !opt->opt_pipeline)
    return;
  const char *passes = opt->opt_pipeline;
  char buf[32];
  if (!passes) {
    sprintf(buf, "default<O%d>", opt->opt_level);
    passes = buf;
  }
  NY_LOG_V3("Running passes: %s\n", passes);
  LLVMPassBuilderOptionsRef popt = LLVMCreatePassBuilderOptions();
  LLVMRunPasses(module, passes, NULL, popt);
  LLVMDisposePassBuilderOptions(popt);
}

int ny_pipeline_run(ny_options *opt) {
  int exit_code = 0;
  if (handle_non_compile_modes(opt, &exit_code))
    return exit_code;

  verbose_enabled = opt->verbose;
  clock_t t_start = 0;
  if (opt->do_timing)
    t_start = clock();

  char *user_src = NULL;
  char *std_src = NULL;
  char *source = NULL;
  char **uses = NULL;
  size_t use_count = 0;
  arena_t *arena = NULL;
  program_t prog = {0};
  codegen_t cg;
  memset(&cg, 0, sizeof(cg));

  user_src = load_user_source(opt);
  if (!user_src) {
    if (opt->input_file)
      NY_LOG_ERR("Failed to read file '%s'\n", opt->input_file);
    else
      NY_LOG_ERR("Failed to allocate source input\n");
    return 1;
  }
  maybe_log_phase_time(opt->do_timing, "Read file:", t_start);

  clock_t t_scan = clock();
  size_t use_cap = 0;
  uses = collect_use_modules(user_src, &use_count);
  use_cap = use_count;

  maybe_log_phase_time(opt->do_timing, "Scan imports:", t_scan);

  std_mode_t std_mode = opt->std_mode;

  if (!opt->no_std && opt->implicit_prelude) {
    append_std_prelude(&uses, &use_count, &use_cap);
  }

  const char *prebuilt_path = resolve_std_bundle(
      opt->std_path
          ? opt->std_path
          : (NYTRIX_STD_PATH ? NYTRIX_STD_PATH : "build/std_bundle.ny"));

  if (opt->no_std) {
    std_mode = STD_MODE_NONE;
  }

  clock_t t_std = clock();
  bool has_local = false;
  for (size_t i = 0; i < use_count; i++) {
    if (strncmp(uses[i], "std.", 4) != 0 && strncmp(uses[i], "lib.", 4) != 0) {
      has_local = true;
      break;
    }
  }

  if (prebuilt_path && access(prebuilt_path, R_OK) == 0 &&
      (std_mode == STD_MODE_FULL || std_mode == STD_MODE_DEFAULT) &&
      !has_local) {
    if (verbose_enabled)
      NY_LOG_INFO("Using prebuilt std bundle: %s\n", prebuilt_path);
    std_src = ny_read_file(prebuilt_path);
  } else if (std_mode != STD_MODE_NONE) {
    // Fallback to building from individual files
    std_src = ny_build_std_bundle((const char **)uses, use_count, std_mode,
                                  opt->verbose, opt->input_file);
  }

  if (std_mode != STD_MODE_NONE && !std_src) {
    NY_LOG_ERR("Could not load standard library bundle or source files.\n");
    NY_LOG_ERR("Checked paths: %s and %s/std\n",
               prebuilt_path ? prebuilt_path : "NULL", ny_src_root());
    if (user_src)
      free(user_src);
    if (uses)
      ny_str_list_free(uses, use_count);
    return 1;
  }
  maybe_log_phase_time(opt->do_timing, "Stdlib load:", t_std);

  // 4. Construct final source with prelude + std + user
  size_t plen = 0;
  char *prelude = build_prelude(opt->no_std, opt->implicit_prelude, &plen);
  if (!opt->no_std && opt->implicit_prelude && !prelude) {
    NY_LOG_ERR("Failed to build std prelude\n");
    exit_code = 1;
    goto exit_success;
  }

  size_t slen = std_src ? strlen(std_src) : 0;
  size_t ulen = strlen(user_src);
  source = malloc(plen + slen + ulen + 3);
  char *ptr = source;
  if (prelude) {
    memcpy(ptr, prelude, plen);
    ptr += plen;
    *ptr++ = '\n';
    free(prelude);
  }
  if (std_src) {
    memcpy(ptr, std_src, slen);
    ptr += slen;
    *ptr++ = '\n';
  }
  memcpy(ptr, user_src, ulen + 1);

  const char *parse_name = opt->input_file ? opt->input_file : "<inline>";
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

  clock_t t_parse = clock();
  parser_t parser;
  arena = (arena_t *)malloc(sizeof(arena_t));
  memset(arena, 0, sizeof(arena_t));
  parser_init_with_arena(&parser, source, std_src ? "<stdlib>" : parse_name,
                         arena);
  if (std_src) {
    parser.lex.split_pos = (prelude ? plen + 1 : 0) + slen + 1;
    parser.lex.split_filename = parse_name;
  }
  prog = parse_program(&parser);
  maybe_log_phase_time(opt->do_timing, "Parsing:", t_parse);

  if (parser.had_error) {
    NY_LOG_ERR("Compilation failed: %d errors\n", parser.error_count);
    dump_debug_bundle(opt, source, NULL);
    exit_code = 1;
    goto exit_success;
  }

  if (opt->dump_ast) {
    for (size_t i = 0; i < prog.body.len; i++) {
      stmt_t *s = prog.body.data[i];
      printf("  [%zu] Kind=%d\n", i, s->kind);
    }
  }

  clock_t t_codegen = clock();
  NY_LOG_V2("Initializing codegen_t for module 'nytrix'\n");

  codegen_init(&cg, &prog, arena, "nytrix");
  cg.debug_symbols = opt->debug_symbols;
  cg.trace_exec = opt->trace_exec;
  if (cg.debug_symbols)
    codegen_debug_init(&cg, parse_name);
  cg.source_string = source;
  cg.implicit_prelude = opt->implicit_prelude;
  cg.prog_owned = false; // prog is on stack
  NY_LOG_V2("Emitting IR...\n");
  codegen_emit(&cg);
  if (cg.had_error) {
    NY_LOG_ERR("Codegen failed\n");
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  NY_LOG_V2("Emitting script entry point...\n");
  LLVMValueRef script_fn = codegen_emit_script(&cg, "__script_top");
  if (cg.had_error) {
    NY_LOG_ERR("Codegen script entry failed\n");
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  codegen_debug_finalize(&cg);
  maybe_log_phase_time(opt->do_timing, "Codegen:", t_codegen);

  if (opt->dump_llvm)
    LLVMDumpModule(cg.module);

  clock_t t_ver = clock();
  if (!verify_module_if_needed(opt, cg.module)) {
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  if (opt->do_timing && opt->verify_module)
    fprintf(stderr, "Verify:       %.4fs\n",
            (double)(clock() - t_ver) / CLOCKS_PER_SEC);

  clock_t t_opt = clock();
  run_optimization_if_needed(opt, cg.module);
  if (opt->do_timing && (opt->opt_level > 0 || opt->opt_pipeline))
    fprintf(stderr, "Optimization: %.4fs\n",
            (double)(clock() - t_opt) / CLOCKS_PER_SEC);

  if (opt->emit_ir_path) {
    char *err = NULL;
    if (LLVMPrintModuleToFile(cg.module, opt->emit_ir_path, &err) != 0) {
      NY_LOG_ERR("Failed to write IR to %s\n", opt->emit_ir_path);
      if (err) {
        NY_LOG_ERR("%s\n", err);
        LLVMDisposeMessage(err);
      }
      exit_code = 1;
      goto exit_success;
    }
  }

  if (opt->emit_asm_path) {
    if (!ny_llvm_emit_file(cg.module, opt->emit_asm_path, LLVMAssemblyFile)) {
      NY_LOG_ERR("Failed to write assembly to %s\n", opt->emit_asm_path);
      exit_code = 1;
      goto exit_success;
    }
  }

  if (opt->output_file) {
    char obj[4096];
    sprintf(obj, "/tmp/ny_tmp_%d.o", getpid());
    ensure_aot_entry(&cg, script_fn);
    if (ny_llvm_emit_object(cg.module, obj)) {
      const char *cc = ny_builder_choose_cc();
      char rto[4096];
      sprintf(rto, "/tmp/ny_rt_%d.o", getpid());
      NY_LOG_V2("Compiling runtime to %s using %s (debug=%d)...\n", rto, cc,
                opt->debug_symbols);
      if (!ny_builder_compile_runtime(cc, rto, NULL, opt->debug_symbols)) {
        unlink(obj);
        dump_debug_bundle(opt, source, cg.module);
        exit_code = 1;
        goto exit_success;
      }
      bool link_strip = opt->strip_override == 1 ||
                        (opt->strip_override == -1 && !opt->debug_symbols);
      NY_LOG_V2("Linking executable %s (strip=%d, debug=%d)...\n",
                opt->output_file, link_strip, opt->debug_symbols);
      if (!ny_builder_link(
              cc, obj, rto, NULL, NULL, 0,
              (const char *const *)opt->link_dirs.data, opt->link_dirs.len,
              (const char *const *)opt->link_libs.data, opt->link_libs.len,
              opt->output_file, link_strip, opt->debug_symbols)) {
        unlink(obj);
        unlink(rto);
        dump_debug_bundle(opt, source, cg.module);
        exit_code = 1;
        goto exit_success;
      }
      unlink(obj);
      unlink(rto);
      NY_LOG_SUCCESS("Saved ELF: %s\n", opt->output_file);
    } else {
      NY_LOG_ERR("Failed to emit object file\n");
      dump_debug_bundle(opt, source, cg.module);
      exit_code = 1;
      goto exit_success;
    }
  }

  if (opt->run_jit) {
    clock_t t_jit = clock();
    LLVMLinkInMCJIT();
    ny_llvm_init_native();
    LLVMExecutionEngineRef ee;
    char *err = NULL;
    struct LLVMMCJITCompilerOptions jopt;
    LLVMInitializeMCJITCompilerOptions(&jopt, sizeof(jopt));
    jopt.CodeModel = LLVMCodeModelLarge;
    // jopt.EnableFastISel = 1; // Try to speed up JIT compile time?
    LLVMModuleRef jmod = cg.module;
    if (LLVMCreateMCJITCompilerForModule(&ee, jmod, &jopt, sizeof(jopt),
                                         &err)) {
      NY_LOG_ERR("JIT failed: %s\n", err);
      dump_debug_bundle(opt, source, jmod);
      exit_code = 1;
      goto exit_success;
    }
    // Execution engine now owns the module
    cg.module = NULL;

    for (size_t i = 0; i < cg.interns.len; i++) {
      if (cg.interns.data[i].gv) {
        LLVMAddGlobalMapping(ee, cg.interns.data[i].gv,
                             (void *)((char *)cg.interns.data[i].data - 64));
      }
      if (cg.interns.data[i].val &&
          cg.interns.data[i].val != cg.interns.data[i].gv) {
        LLVMAddGlobalMapping(ee, cg.interns.data[i].val,
                             &cg.interns.data[i].data);
      }
    }

    register_jit_symbols(ee, jmod, &cg);
    maybe_log_phase_time(opt->do_timing, "JIT Init:", t_jit);

    clock_t t_exec = clock();
    // Execution
    uint64_t saddr = LLVMGetFunctionAddress(ee, "__script_top");
    if (saddr) {
      if (verbose_enabled >= 3)
        fprintf(stderr, "TRACE: Executing script...\n");
      ((void (*)(void))saddr)();
      if (verbose_enabled >= 3)
        fprintf(stderr, "TRACE: Script finished.\n");
    } else {
      if (verbose_enabled >= 3)
        fprintf(stderr, "TRACE: __script_top NOT FOUND\n");
    }

    // NOTE: Do not auto-invoke main() here. Script top-level controls
    // execution.
    maybe_log_phase_time(opt->do_timing, "JIT Exec:", t_exec);

    LLVMDisposeExecutionEngine(ee);
  }

exit_success:
  // Cleanup allocated memory
  if (user_src)
    free(user_src);
  if (std_src)
    free(std_src);
  if (source)
    free(source);
  if (uses)
    ny_str_list_free(uses, use_count);
  codegen_dispose(&cg);
  program_free(&prog, arena);

  maybe_log_phase_time(opt->do_timing, "Total time:", t_start);
  return exit_code;
}
