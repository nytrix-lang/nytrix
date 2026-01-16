#include "base/options.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global State
int debug_enabled = 0;
int verbose_enabled = 0;
int color_mode = -1;

void ny_options_init(ny_options *opt) {
  memset(opt, 0, sizeof(ny_options));
  opt->mode = NY_MODE_RUN;
  opt->strip_override = -1;
  opt->color_mode = -1;
  opt->std_mode = STD_MODE_DEFAULT;
}

void ny_options_usage(const char *prog) {
  fprintf(
      stderr,
      "\n\033[1;36mNytrix Compiler\033[0m - Small core with stdlib in .ny\n\n");
  fprintf(stderr, "\033[1mUSAGE:\033[0m %s [OPTIONS] file.ny\n\n", prog);
  fprintf(stderr, "\033[1mOPTIMIZATION:\033[0m\n");
  fprintf(stderr, "  \033[32m-O1/-O2/-O3\033[0m        Optimization level "
                  "(default: -O0)\n");
  fprintf(stderr, "  \033[32m-passes=PIPE\033[0m       Custom LLVM pass "
                  "pipeline (e.g., 'default<O2>')\n\n");
  fprintf(stderr, "\033[1mEXECUTION:\033[0m\n");
  fprintf(stderr, "  \033[32m-run\033[0m               JIT execute main() "
                  "after compilation\n");
  fprintf(stderr, "  \033[32m-emit-only\033[0m         Only emit IR, don't "
                  "execute (default)\n");
  fprintf(stderr, "  \033[32m-o [path]\033[0m          Emit ELF at [path] "
                  "(default: a.out; implies -emit-only)\n");
  fprintf(stderr, "  \033[32m--output=<path>\033[0m    Same as -o\n");
  fprintf(stderr, "  \033[32m-strip, -no-strip\033[0m  Create "
                  "stripped/unstripped binary\n");
  fprintf(stderr,
          "  \033[32m-c <code>\033[0m          Execute inline code\n\n");
  fprintf(stderr, "\033[1mREPL:\033[0m\n");
  fprintf(
      stderr,
      "  \033[32m-i, -interactive\033[0m   Interactive REPL with readline\n");
  fprintf(stderr, "  \033[32m-repl\033[0m              Read source from stdin "
                  "(one-shot)\n\n");
  fprintf(stderr, "\033[1mDEBUGGING:\033[0m\n");
  fprintf(stderr,
          "  \033[34m-v, -verbose\033[0m       Show compilation steps\n");
  fprintf(stderr,
          "  \033[34m--debug\033[0m            Enable verbose + debug logs\n");
  fprintf(stderr,
          "  \033[34m-vv, -vvv\033[0m          Increased verbosity levels\n");
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
  fprintf(stderr, "  \033[34m--dump-on-error\033[0m    Write "
                  "build/debug/last_source.ny and last_ir.ll on errors\n");
  fprintf(stderr,
          "  \033[34m-trace\033[0m             Enable execution tracing\n\n");
  fprintf(stderr,
          "  \033[35m--color=WHEN\033[0m       WHEN: auto | always | never\n");
  fprintf(stderr,
          "  \033[35m-safe-mode\033[0m         Enable all safety checks\n\n");
  fprintf(stderr, "\033[1mINFO:\033[0m\n");
  fprintf(stderr,
          "  \033[34m-h, -help, --help\033[0m  Show this help message\n");
  fprintf(stderr, "  \033[34m-version\033[0m           Show version info\n\n");
  fprintf(stderr, "\033[1mENVIRONMENT:\033[0m\n");
  fprintf(stderr, "  \033[33mNYTRIX_RUN\033[0m         Same as -run flag\n");
  fprintf(stderr,
          "  \033[33mNYTRIX_DUMP_TOKENS\033[0m Dump tokens during lexing\n\n");
  fprintf(stderr, "\033[1mEXAMPLES:\033[0m\n");
  fprintf(stderr,
          "  \033[1;36m%s examples/quicksort.ny              # compile "
          "only\033[0m\n",
          prog);
  fprintf(stderr,
          "  \033[1;36m%s -O2 -run examples/quicksort.ny     # compile & run "
          "optimized\033[0m\n",
          prog);
  fprintf(stderr,
          "  \033[1;36m%s -v -time -verify examples/file.ny  # debug "
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

void ny_options_parse(ny_options *opt, int argc, char **argv) {
  if (argc > 0)
    opt->argv0 = argv[0];
  if (argc < 2)
    return;

  int i = 1;
  while (i < argc) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      if (!opt->file_arg_idx)
        opt->file_arg_idx = i;
      break;
    }

    if (a[0] == '-') {
      // Optimization
      if (strcmp(a, "-O") == 0 || strcmp(a, "-O2") == 0)
        opt->opt_level = 2;
      else if (strcmp(a, "-O1") == 0)
        opt->opt_level = 1;
      else if (strcmp(a, "-O3") == 0)
        opt->opt_level = 3;
      else if (strcmp(a, "--fast") == 0) {
        if (!opt->opt_level)
          opt->opt_level = 2;
        opt->verify_module = 0;
        opt->strip_override = 1;
      } else if (strcmp(a, "-O0") == 0)
        opt->opt_level = 0;
      else if (strncmp(a, "-passes=", 8) == 0)
        opt->opt_pipeline = a + 8;
      else if (strcmp(a, "-strip") == 0 || strcmp(a, "--strip") == 0)
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
          opt->output_file = "a.out";
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
      } else if (strcmp(a, "-v") == 0 || strcmp(a, "-verbose") == 0)
        opt->verbose = 1;
      else if (strcmp(a, "-vv") == 0)
        opt->verbose = 2;
      else if (strcmp(a, "-vvv") == 0)
        opt->verbose = 3;
      else if (strcmp(a, "-time") == 0)
        opt->do_timing = true;
      else if (strcmp(a, "-dump-ast") == 0)
        opt->dump_ast = true;
      else if (strcmp(a, "-dump-llvm") == 0)
        opt->dump_llvm = true;
      else if (strncmp(a, "--emit-ir=", 10) == 0)
        opt->emit_ir_path = a + 10;
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
      else if (strcmp(a, "-verify") == 0)
        opt->verify_module = true;
      else if (strcmp(a, "-g") == 0)
        opt->debug_symbols = true;

      // Std
      else if (strcmp(a, "-std") == 0) {
        if (i + 1 < argc) {
          const char *mode = argv[++i];
          if (strcmp(mode, "none") == 0)
            opt->std_mode = STD_MODE_NONE;
          else if (strcmp(mode, "full") == 0)
            opt->std_mode = STD_MODE_FULL;
          else if (strcmp(mode, "minimal") == 0)
            opt->std_mode = STD_MODE_MINIMAL;
          else {
            fprintf(stderr, "unknown std mode: %s\n", mode);
            ny_options_usage(argv[0]);
            exit(1);
          }
        } else {
          fprintf(stderr, "missing argument for -std\n");
          ny_options_usage(argv[0]);
          exit(1);
        }
      } else if (strcmp(a, "-no-std") == 0)
        opt->no_std = true;
      else if (strncmp(a, "--std-path=", 11) == 0)
        opt->std_path = a + 11;
      else if (strcmp(a, "--plain-repl") == 0)
        opt->repl_plain = true;

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
      else if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0 ||
               strcmp(a, "--help") == 0)
        opt->mode = NY_MODE_HELP;
      else if (strcmp(a, "-version") == 0 || strcmp(a, "--version") == 0)
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
}
