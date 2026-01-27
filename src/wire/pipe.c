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
  if (access("build/std_bundle.ny", R_OK) == 0) {
    strcpy(path, "build/std_bundle.ny");
    return path;
  }
  // 1. Check relative to binary
  char *exe_dir = ny_get_executable_dir();
  if (exe_dir) {
    snprintf(path, sizeof(path), "%s/std_bundle.ny", exe_dir);
    if (access(path, R_OK) == 0)
      return path;
    snprintf(path, sizeof(path), "%s/../share/nytrix/std_bundle.ny", exe_dir);
    if (access(path, R_OK) == 0)
      return path;
  }

  // 2. Check source root
  const char *root = ny_src_root();
  snprintf(path, sizeof(path), "%s/build/std_bundle.ny", root);
  if (access(path, R_OK) == 0)
    return path;
  snprintf(path, sizeof(path), "%s/std_bundle.ny", root);
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

int ny_pipeline_run(ny_options *opt) {
  int exit_code = 0;
  if (opt->mode == NY_MODE_VERSION) {
    printf("Nytrix v0.1.25\n");
    return 0;
  }
  if (opt->mode == NY_MODE_HELP) {
    ny_options_usage(opt->argv0 ? opt->argv0 : "ny");
    return 0;
  }

  if (opt->mode == NY_MODE_REPL) {
    LLVMLinkInMCJIT();
    ny_llvm_init_native();
    LLVMLoadLibraryPermanently(NULL);
    ny_repl_run(opt->opt_level, opt->opt_pipeline, opt->command_string, 0);
    return 0;
  }

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

  if (opt->command_string) {
    user_src = ny_strdup(opt->command_string);
  } else if (opt->input_file) {
    user_src = ny_read_file(opt->input_file);
    if (!user_src) {
      NY_LOG_ERR("Failed to read file '%s'\n", opt->input_file);
      return 1;
    }
  } else {
    user_src = ny_strdup("fn main() { return 0\n }");
  }
  if (opt->do_timing)
    fprintf(stderr, "Read file:    %.4fs\n",
            (double)(clock() - t_start) / CLOCKS_PER_SEC);

  clock_t t_scan = clock();
  size_t use_cap = 0;
  uses = collect_use_modules(user_src, &use_count);
  use_cap = use_count;

  if (opt->do_timing)
    fprintf(stderr, "Scan imports: %.4fs\n",
            (double)(clock() - t_scan) / CLOCKS_PER_SEC);

  std_mode_t std_mode = opt->std_mode;

  if (!opt->no_std) {
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
  if (opt->do_timing)
    fprintf(stderr, "Stdlib load:  %.4fs\n",
            (double)(clock() - t_std) / CLOCKS_PER_SEC);

  // 4. Construct final source with prelude + std + user
  size_t plen = 0;
  char *prelude = NULL;
  if (!opt->no_std) {
    size_t count = 0;
    const char **p_list = ny_std_prelude(&count);
    size_t cap = 1024;
    prelude = malloc(cap);
    prelude[0] = '\0';
    for (size_t i = 0; i < count; ++i) {
      char line[256];
      if (strcmp(p_list[i], "std.core") == 0 ||
          strcmp(p_list[i], "std.io") == 0) {
        sprintf(line, "use %s *;\n", p_list[i]);
      } else {
        sprintf(line, "use %s;\n", p_list[i]);
      }
      size_t llen = strlen(line);
      if (plen + llen + 1 > cap) {
        cap *= 2;
        prelude = realloc(prelude, cap);
      }
      strcpy(prelude + plen, line);
      plen += llen;
    }
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
  if (opt->do_timing)
    fprintf(stderr, "Parsing:      %.4fs\n",
            (double)(clock() - t_parse) / CLOCKS_PER_SEC);

  if (parser.had_error) {
    NY_LOG_ERR("Compilation failed: %d errors\n", parser.error_count);
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
  cg.source_string = source;
  cg.prog_owned = false; // prog is on stack
  NY_LOG_V2("Emitting IR...\n");
  codegen_emit(&cg);
  if (cg.had_error) {
    NY_LOG_ERR("Codegen failed\n");
    exit_code = 1;
    goto exit_success;
  }
  NY_LOG_V2("Emitting script entry point...\n");
  LLVMValueRef script_fn = codegen_emit_script(&cg, "__script_top");
  if (cg.had_error) {
    NY_LOG_ERR("Codegen script entry failed\n");
    exit_code = 1;
    goto exit_success;
  }
  if (opt->do_timing)
    fprintf(stderr, "Codegen:      %.4fs\n",
            (double)(clock() - t_codegen) / CLOCKS_PER_SEC);

  if (opt->dump_llvm)
    LLVMDumpModule(cg.module);

  clock_t t_ver = clock();
  if (opt->verify_module) {
    char *err = NULL;
    if (LLVMVerifyModule(cg.module, LLVMPrintMessageAction, &err)) {
      NY_LOG_ERR("Verification failed: %s\n", err);
      LLVMDisposeMessage(err);
      exit_code = 1;
      goto exit_success;
    }
  }
  if (opt->do_timing && opt->verify_module)
    fprintf(stderr, "Verify:       %.4fs\n",
            (double)(clock() - t_ver) / CLOCKS_PER_SEC);

  clock_t t_opt = clock();
  if (opt->opt_level > 0 || opt->opt_pipeline) {
    const char *passes = opt->opt_pipeline;
    char buf[32];
    if (!passes) {
      sprintf(buf, "default<O%d>", opt->opt_level);
      passes = buf;
    }
    NY_LOG_V3("Running passes: %s\n", passes);
    LLVMPassBuilderOptionsRef popt = LLVMCreatePassBuilderOptions();
    LLVMRunPasses(cg.module, passes, NULL, popt);
    LLVMDisposePassBuilderOptions(popt);
  }
  if (opt->do_timing && (opt->opt_level > 0 || opt->opt_pipeline))
    fprintf(stderr, "Optimization: %.4fs\n",
            (double)(clock() - t_opt) / CLOCKS_PER_SEC);

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
        exit_code = 1;
        goto exit_success;
      }
      bool link_strip = opt->strip_override == 1 ||
                        (opt->strip_override == -1 && !opt->debug_symbols);
      NY_LOG_V2("Linking executable %s (strip=%d, debug=%d)...\n",
                opt->output_file, link_strip, opt->debug_symbols);
      if (!ny_builder_link(cc, obj, rto, NULL, NULL, 0,
                           (const char *const *)opt->link_dirs.data,
                           opt->link_dirs.len,
                           (const char *const *)opt->link_libs.data,
                           opt->link_libs.len, opt->output_file,
                           link_strip, opt->debug_symbols)) {
        unlink(obj);
        unlink(rto);
        exit_code = 1;
        goto exit_success;
      }
      unlink(obj);
      unlink(rto);
      NY_LOG_SUCCESS("Saved ELF: %s\n", opt->output_file);
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
    if (opt->do_timing)
      fprintf(stderr, "JIT Init:     %.4fs\n",
              (double)(clock() - t_jit) / CLOCKS_PER_SEC);

    clock_t t_exec = clock();
    // Execution
    uint64_t saddr = LLVMGetFunctionAddress(ee, "__script_top");
    if (saddr) {
      if (verbose_enabled)
        fprintf(stderr, "TRACE: Executing script...\n");
      ((void (*)(void))saddr)();
      if (verbose_enabled)
        fprintf(stderr, "TRACE: Script finished.\n");
    } else {
      if (verbose_enabled)
        fprintf(stderr, "TRACE: __script_top NOT FOUND\n");
    }

    // NOTE: Do not auto-invoke main() here. Script top-level controls execution.
    if (opt->do_timing)
      fprintf(stderr, "JIT Exec:     %.4fs\n",
              (double)(clock() - t_exec) / CLOCKS_PER_SEC);

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

  if (opt->do_timing) {
    double total = (double)(clock() - t_start) / CLOCKS_PER_SEC;
    fprintf(stderr, "Total time:   %.4fs\n", total);
  }
  return exit_code;

  if (opt->do_timing) {
    double total = (double)(clock() - t_start) / CLOCKS_PER_SEC;
    fprintf(stderr, "Total time:   %.4fs\n", total);
  }
  return exit_code;
}
