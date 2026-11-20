/*
 * Static command summaries and usage text for the repository make tool.
 */
#include "base/ui.h"
#include "make.h"
#include "nyc_help.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const char *name;
  const char *usage;
  const char *summary;
  const char *rows;
} MakeHelp;

#define HELP(name, usage, summary, rows) {name, usage, summary, rows}

static const MakeHelp commands[] = {
    HELP("bare-payload", "./make bare-payload [--check]",
         "Regenerate or verify the embedded freestanding runtime payloads.",
         "[Requirements]\nclang, readelf, ld.lld, nm, llvm-objcopy|Required "
         "for maintainer regeneration.\n[Examples]\n./make bare-payload "
         "--check|Verify checked-in payloads.\n./make bare-payload|Regenerate "
         "both architectures."),
    HELP("unicode", "./make unicode",
         "Regenerate Unicode 18.0 tables in place.",
         "[Outputs]\ncore/unicode.ny|Generated core.unicode module.\n"
         "src/code/lang/ny/parser/unicode.c, unicode.h|Lexer XID_Start/Continue tables.\n"
         "[Sources]\netc/assets/dict/unicode/18.0.0/|Cached UCD.zip and UTS #39 "
         "security files verified by manifest.json.\nunzip|Required to read UCD members.\n"
         "[Examples]\n./make unicode|Regenerate tables when unchanged files are kept as-is."),
    HELP("all", "./make all",
         "Build the compiler, standard library, and tools.",
         "[Build]\nny, std, ny-fmt, ny-perf, ny-test, ny-doc, ny-make, and "
         "ny-lsp|Full developer build.\n"
         "[Options]\n-j, --jobs N|Parallel build jobs.\n-v, --verbose|Repeat "
         "for more build detail.\n--debug|Use the debug build directory.\n"
         "[Environment]\nNYTRIX_HOST_CFLAGS / NYTRIX_HOST_LDFLAGS|Extra host "
         "compiler/linker flags.\nNYTRIX_BUILD_JOBS|Default build "
         "parallelism.\nCC / LLVM_CONFIG|Select the host compiler and LLVM "
         "installation.\n"
         "[Examples]\n./make all|Build the release toolchain.\n./make --debug "
         "all|Build the debug toolchain."),
    HELP("bin", "./make bin", "Build compiler and command-line tools.",
         "[Build]\nResult|Tools are written under build/<kind>/.\n-j, --jobs "
         "N|Parallel build jobs.\n--debug|Use the debug build directory.\n"
         "[Examples]\n./make bin|Build the release tools.\n./make --debug "
         "bin|Build debug tools."),
    HELP("static", "./make static <subcommand> [args]",
         "Build or inspect portable artifacts.",
         "[Subcommands]\nbin|Build compiler/tools and bundle runtime "
         "libraries.\nlibs [path]|Bundle shared libraries beside a "
         "build.\ncheck <binary>|Report ELF linkage.\nny <file.ny> "
         "[flags]|Compile with static-link flags.\nbinstatic|Build an "
         "arbitrary C program with the bundled toolchain.\n"
         "[Examples]\n./make static bin|Build the portable compiler "
         "bundle.\n./make static check build/release/ny|Inspect the compiler "
         "binary."),
    HELP("bin-static", "./make bin-static", "Alias for ./make static bin.",
         "[Example]\n./make bin-static|Build compiler/tools and bundle runtime "
         "libraries."),
    HELP(
        "tar", "./make tar [--source | --with-binaries] [--bench]",
        "Create a source or binary release archive.",
        "[Options]\n--source, --source-only|Create a source-only "
        "archive.\n--with-binaries|Include portable binaries.\n--bench|Package "
        "native benchmark and NYR/LLVM/NYC assembly evidence.\n--out "
        "PATH|Select an output path.\n"
        "[Benchmark evidence]\nDefault|1 run, 4 representative ASM fixtures, "
        "cached ASM, high parallelism.\nNYTRIX_TAR_BENCH_RUNS / _TIMEOUT / "
        "_PATTERN|Control benchmark runs, timeout, and fixture filter.\n"
        "NYTRIX_TAR_BENCH_JOBS / NYTRIX_TAR_ASM_JOBS|Control worker "
        "counts.\nNYTRIX_TAR_BENCH_ALL_ASM=1 / "
        "NYTRIX_TAR_ASM_REFRESH=1|Assemble "
        "all fixtures or refresh cached ASM.\n"
        "[Output]\nDefault|build/dist/"
        "nytrix-source.tar.gz.\n--with-binaries|build/dist/"
        "nytrix-static.tar.gz."),
    HELP("vendor", "./make vendor",
         "Bundle shared build dependencies under build/vendor/.",
         "[What it does]\nBundle|Collects detected LLVM/Clang/Z3 libraries and "
         "headers for portable builds."),
    HELP("fmt", "./make fmt [ny-fmt args]",
         "Format or check source with ny-fmt.",
         "[Options]\n--check|Check formatting without rewriting.\n--audit|Run "
         "the deep source audit.\n--bugs|Report suspicious patterns.\n--limit "
         "N|Limit findings.\n"
         "[Examples]\n./make fmt|Format sources.\n./make fmt --check|Check "
         "formatting."),
    HELP("analyze", "./make analyze [path] [ny-fmt args]",
         "Analyze source without rewriting it.",
         "[Options]\npath|Limit the report to a file or directory.\n--limit "
         "N|Limit findings."),
    HELP("check", "./make check [test options] [--no-test-suite]",
         "Run hygiene, source checks, build, and tests.",
         "[Stages]\n1|Reject NUL bytes and temporary print-debugging "
         "probes.\n2|Run tidy, format, and source audits.\n3|Build required "
         "tools.\n4|Run the cold test gate with stdlib, replay, and "
         "benchmarks.\n--no-test-suite|Skip the full fixture suite.\n"
         "[Environment]\nNYTRIX_CHECK_COMPACT=1|Show failing fixture rows "
         "only.\nNYTRIX_CHECK_REPORT_DIR|Select the report directory.\n"
         "[Examples]\n./make check|Run the full gate.\n./make check "
         "--no-test-suite|Run source checks and build only."),
    HELP("tidy", "./make tidy [ny-fmt args]", "Run ny-fmt --tidy.",
         "[Example]\n./make tidy|Tidy supported source files."),
    HELP("audit", "./make audit [ny-fmt args]",
         "Run the deep ny-fmt source audit.",
         "[What it checks]\nAudit|Finds suspicious patterns, unused bindings, "
         "and source issues."),
    HELP("test", "./make test [ny-test options] [patterns]",
         "Run the Nytrix test suite.",
         "[Options]\n--cold|Disable result and execution "
         "caches.\n--with-bench|Append timed benchmarks.\n--with-preflight|Run "
         "auxiliary backend checks.\n--with-stdlib|Include library "
         "fixtures.\n--with-replay|Print failure replays.\n--pattern PATH|Run "
         "matching fixtures only.\n--jobs N|Parallel fixture "
         "workers.\n--fail-fast|Stop queueing after the first "
         "failure.\n--timeout SEC|Set per-fixture "
         "timeout.\n--debug-failures|Produce trace and debugger artifacts.\n"
         "[Examples]\n./make test|Run the suite.\n./make test --cold "
         "--with-stdlib|Run cold tests including the library.\n./make test "
         "--pattern runtime|Run matching fixtures."),
    HELP(
        "bench", "./make bench [ny-test bench options]",
        "Run the C-versus-Ny benchmark suite.",
        "[Options]\n--list-bench|List benchmark fixtures.\n--bench-run N|Timed "
        "samples per fixture.\n--bench-warmup N|Warm-up runs.\n--bench-opt "
        "LEVEL|Select compiler optimization level.\n--bench-target "
        "TARGET|Restrict fixture targets.\n--bench-show-ir / "
        "--bench-show-asm|Print compiler outputs.\n--no-build|Use current "
        "benchmark binaries.\n--bench-out-csv PATH|Write CSV "
        "results.\n--bench-out-json PATH|Write JSON results.\n"
        "Full suite|Refresh docs/learn/measurements.md automatically.\n"
        "[Example]\n./make bench|Run public benchmark shapes."),
    HELP("fuzz", "./make fuzz [subcommand] [args]",
         "Validate shapes or run smoke fuzzing.",
         "[Subcommands]\nvalidate-shapes DIR|Validate deterministic test "
         "shapes.\nafl -- ARGS|Launch afl-fuzz.\n<args>|Pass arguments to "
         "ny-test fuzz.\n"
         "[Examples]\n./make fuzz|Validate default shapes.\n./make fuzz "
         "validate-shapes etc/tests/code/probes|Select a shape directory."),
    HELP("asan", "./make asan", "Build with AddressSanitizer and run tests.",
         ""),
    HELP("ubsan", "./make ubsan",
         "Build with UndefinedBehaviorSanitizer and run tests.", ""),
    HELP("coverage", "./make coverage [file.ny ... | --test [ny-test args]]",
         "Run the coverage-instrumented compiler.",
         "[Modes]\nDefault|Run the memory-safety smoke.\n--test|Run ny-test "
         "against the instrumented compiler.\nOutput|Write lcov HTML under "
         "build/coverage/coverage-html.\n"
         "[Example]\n./make coverage --test --pattern runtime|Collect suite "
         "coverage for matching fixtures."),
    HELP("perf", "./make perf [ny-perf args]", "Run performance tooling.",
         "[Commands]\nprofile|Collect a runtime profile.\nreport|Summarize a "
         "stored profile."),
    HELP("gprof", "./make gprof [file.ny]",
         "Build and run a source with gprof instrumentation.",
         "[What it does]\nProfile|Enables gprof instrumentation and runs the "
         "selected source."),
    HELP("profile", "./make profile <mode> [options]",
         "Run time, perf, debugger, sanitizer, or fuzz workflows.",
         "[Modes]\ntime <file.ny>|Measure compile-and-run.\ncompile "
         "<file.ny>|Measure compilation.\nperf <file.ny>|Run under Linux "
         "perf.\ngdb <file.ny>|Run under GDB.\ngprof <file.ny>|Profile with "
         "gprof.\nasan / ubsan|Run sanitizer builds.\nfuzz [DIR]|Validate "
         "shapes.\n"
         "[Example]\n./make profile time --runs 5 -- file.ny|Repeat a timed "
         "source run."),
    HELP("optcheck", "./make optcheck",
         "Run native optimizer correctness fixtures.",
         "[Fixtures]\nDirectory|etc/tests/code/intermediate/opt/.\nOracle|Add "
         ".nshape cases with --native-result-oracle."),
    HELP("docs", "./make docs [ny-doc args]", "Build the documentation portal.",
         "[Options]\n-o DIR|Select the output directory (default build/docs).\n"
         "[Example]\n./make docs -o build/docs/out|Build to a custom "
         "directory."),
    HELP("web", "./make web <file.ny> [options]",
         "Build one Ny source as a deployable WebGL2 app.",
         "[Options]\n--out PATH|Select the deployment directory.\n--renderer "
         "NAME|Choose a supported renderer.\n--assets DIR|Add an asset "
         "directory.\n--target TARGET|Select a web target.\n"
         "[Example]\n./make web etc/projects/ui/pong.ny|Build the Pong browser "
         "app."),
    HELP("web-demos", "./make web-demos [options]",
         "Build the browser WebGL/Wasm demo portal.",
         "[Options]\n--out DIR|Select the output directory.\n--headless|Skip "
         "lifecycle fixtures that require a live browser.\n"),
    HELP("web-check", "./make web-check <file.ny> [options]",
         "Check a source file against the supported browser host APIs.",
         "[Options]\n--target TARGET|Select the web target.\n--timeout SEC|Set "
         "the check timeout."),
    HELP("web-test", "./make web-test [--headless]",
         "Build and verify the browser demo and Pong app.",
         "[Options]\n--headless|Skip manifest fixtures that require a live "
         "browser lifecycle."),
    HELP("wasm", "./make wasm <file.ny> [--out PATH]",
         "Compile a Ny source file to WebAssembly.",
         "[Options]\n--out PATH|Select the output .wasm file."),
    HELP("c2ny", "./make c2ny <file.c> [-o out.ny]",
         "Translate C source through ny-fmt --c2ny.",
         "[Example]\n./make c2ny hello.c -o hello.ny|Translate a C file."),
    HELP("py2ny", "./make py2ny <file.py> [-o out.ny]",
         "Translate Python source through ny-fmt --py2ny.",
         "[Example]\n./make py2ny hello.py -o hello.ny|Translate a Python "
         "file."),
    HELP("ny", "./make ny [file.ny] [ny flags] [program args]",
         "Launch the compiler or REPL.",
         "[Arguments]\nfile.ny|Source to run; omit to open the REPL.\nny "
         "flags|Compiler options passed through to ny.\n-- [args]|Pass "
         "remaining values as program arguments.\n"
         "[Examples]\n./make ny|Open the REPL.\n./make ny hello.ny|Run a "
         "source.\n./make ny hello.ny -- arg|Pass an argument to the program."),
    HELP("repl", "./make repl", "Launch the interactive REPL.",
         "[Example]\n./make repl|Start the line-editing REPL."),
    HELP("run", "./make run <file.ny> [args]", "Alias for ./make ny.",
         "[Example]\n./make run hello.ny|Run a source."),
    HELP("install", "./make install",
         "Build and install Nytrix into the configured prefix.",
         "[Tools]\nInstalled tools|The compiler tools are available through ny "
         "<tool>."),
    HELP("uninstall", "./make uninstall",
         "Remove paths from the install manifest.",
         "[Safety]\nManifest|Only files recorded by CMake's install manifest "
         "are removed."),
    HELP("clean", "./make clean", "Remove the selected build directory.",
         "[Environment]\nBUILD_DIR|Selects the directory to remove; defaults "
         "to build/."),
    HELP("deps", "./make deps",
         "Check and install required build dependencies.",
         "[Behavior]\nPlatform|Uses the current platform's supported package "
         "and toolchain setup."),
    HELP("env", "./make env", "Print effective paths, tools, and overrides.",
         "[Includes]\nPaths|Repository root, build directory, and "
         "caches.\nTools|Compiler, CMake, Ninja, LLVM, and related "
         "tools.\nOverrides|Active NYTRIX_* environment variables."),
    HELP("targets", "./make targets",
         "Show cross-compilation presets and runner guidance.",
         "[Usage]\nny --cross=<preset>|Compile for a preset.\nRaw triples|Pass "
         "an explicit target triple."),
    HELP("doctor", "./make doctor [ny --doctor args]",
         "Report toolchain and host diagnostics.",
         "[Example]\n./make doctor|Check required and optional tools."),
    HELP("prepare", "./make prepare",
         "Run repository setup checks before build commands.",
         "[Behavior]\nSetup|Checks generated project scripts and required "
         "build prerequisites."),
    HELP("hooks", "./make hooks", "Install the repository-managed Git hooks.",
         "[What it changes]\nGit config|Sets this repository's core.hooksPath "
         "to .githooks.\nPre-commit|Checks staged whitespace, source hygiene, "
         "and formatting.\nPre-push|Runs ./make check."),
    HELP("cross", "./make cross <triple-or-preset> [file.ny] [-- flags]",
         "Cross-compile with ny.",
         "[Example]\n./make cross aarch64-linux-gnu file.ny|Compile for "
         "AArch64."),
    HELP("cross-run", "./make cross-run <triple-or-preset> [file.ny] [-- args]",
         "Cross-compile and run through a configured emulator.",
         "[Example]\n./make cross-run linux-arm64 file.ny|Run the AArch64 "
         "build through QEMU."),
    HELP("stats", "./make stats [--cloc | --no-cloc] [--json]",
         "Summarize Git state and source size.",
         "[Options]\n--cloc|Include per-language and largest-file physical "
         "line counts (default).\n--no-cloc|Skip source counting.\n--json|Emit "
         "machine-readable output.\n"
         "[Examples]\n./make stats|Show Git and source summary.\n./make stats "
         "--no-cloc|Show Git-only summary.\n./make stats --cloc --json|Emit "
         "JSON."),
    HELP("static bin", "./make static bin",
         "Build and bundle portable compiler tools.", ""),
    HELP("static libs", "./make static libs [path]",
         "Bundle shared libraries beside a build.", ""),
    HELP("static check", "./make static check <binary>",
         "Inspect ELF static/dynamic linkage.", ""),
    HELP("core", "./make core", "Build the standard library source bundle.",
         ""),
    HELP("core_bc", "./make core_bc",
         "Build the shared standard-library bitcode artifact.", ""),
    HELP("std", "./make std", "Build the standard library.", ""),
    HELP("std_bc", "./make std_bc",
         "Build the standard-library bitcode artifact.", ""),
    HELP("release", "./make release", "Select the release build configuration.",
         ""),
    HELP("debug", "./make debug", "Select the debug build configuration.", ""),
    HELP("bench-compare", "./make bench-compare [options]",
         "Compare benchmark samples against a saved baseline.", ""),
    HELP("fb", "./make fb",
         "Optimization correctness and fuzz command guidance.",
         "[Alternatives]\n./make test|Run optimizer correctness tests.\n./make "
         "fuzz|Validate shapes.\n./make bench|Run benchmarks."),
};

static const char global_rows[] =
    "[Build]\nall|Build compiler, standard library, and tools.\nbin|Build "
    "compiler and command-line tools.\nnyc|Build the standalone C compiler.\n"
    "static bin / bin-static|Build portable "
    "compiler artifacts.\ntar|Create source or binary release "
    "archives.\nvendor|Bundle shared build dependencies.\ncore / core_bc|Build "
    "standard library source or bitcode.\ninstall / uninstall|Install or "
    "remove Nytrix.\nclean|Remove generated build output.\n"
    "[Check]\nfmt / analyze / tidy / audit|Format and inspect "
    "source.\ncheck|Run the fail-closed quality gate.\ntest / fuzz|Run tests "
    "or shape validation.\nbench|Run C-versus-Ny benchmarks.\nasan / ubsan / "
    "coverage|Run instrumented compiler workflows.\nperf / gprof / "
    "profile|Profile compilation and execution.\n"
    "[Run]\nny / repl / run|Run the compiler or REPL.\ndocs|Build the "
    "documentation portal.\nweb / web-demos / web-check / web-test|Build or "
    "verify browser targets.\nwasm|Compile WebAssembly.\nc2ny / "
    "py2ny|Translate source to Nytrix.\n"
    "[Inspect]\nenv / targets / doctor / stats|Inspect configuration, targets, "
    "tools, and repository state.\nprepare|Run repository setup checks.\n"
    "hooks|Install repository Git hooks.\n"
    "[Cross]\ncross / cross-run|Cross-compile or run under an emulator.\n"
    "[Global options]\n-j, --jobs N|Choose build parallelism.\n-v, "
    "--verbose|Print underlying build commands.\n--debug|Select debug build "
    "outputs.\n--color MODE|auto, always, or never.\n--glyphs MODE|unicode or "
    "ascii.\n--bootstrap-logs|Show build bootstrap status.\n--version|Print "
    "build-tool version.\n--no-color|Disable colored output.\n"
    "--no-bootstrap-logs|Hide bootstrap status.\n-h, --help|Show help.\n"
    "[Examples]\n./make doctor|Check the local toolchain.\n./make targets|List "
    "cross-compilation presets.\n./make cross aarch64-linux-gnu "
    "file.ny|Cross-compile a source.\n./make ny file.ny -- arg|Run a source "
    "with program arguments.\n./make web-demos|Build browser demos.\n./make "
    "help test|Show focused command help.";

static void render_rows(const char *text) {
  char line[2048];
  const char *p = text;
  while (p && *p) {
    size_t len = 0;
    while (p[len] && p[len] != '\n')
      len++;
    if (len >= sizeof(line))
      len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    p += p[len] ? len + 1 : len;
    if (!line[0])
      continue;
    if (line[0] == '[') {
      size_t n = strlen(line);
      if (n > 2 && line[n - 1] == ']') {
        line[n - 1] = '\0';
        nyt_subheading(line + 1);
      }
      continue;
    }
    char *separator = strchr(line, '|');
    if (separator) {
      *separator++ = '\0';
      nyt_kv(line, "%s", separator);
    } else {
      nyt_wrap_value(line, 2, ny_ui_width() - 4);
    }
  }
}

int ny_make_print_help(const char *command) {
  if (!command || !*command) {
    nyt_heading("Build Tool");
    nyt_kv("Usage", "./make <command> [options]");
    nyt_wrap_value("Use ./make help <command> for focused command help.", 2,
                   ny_ui_width() - 4);
    render_rows(global_rows);
    return 0;
  }
  if (strcmp(command, "help") == 0)
    return ny_make_print_help(NULL);
  if (strcmp(command, "nyc") == 0) {
    nyt_heading("NYC Compiler");
    nyt_kv("Build", "./make nyc [build options]");
    nyt_wrap_value("Output: build/release/nyc; ./make debug nyc selects "
                   "build/debug/nyc.",
                   2, ny_ui_width() - 4);
    nyt_wrap_value("Build options: -j N / --jobs N, -v / --verbose, --debug, "
                   "-h / --help. BUILD_DIR selects the build root; CC selects "
                   "the host compiler.",
                   2, ny_ui_width() - 4);
    fputc('\n', stdout);
    ny_help_heading(stdout, "C compiler (NYC)");
    ny_help_print(stdout, nyc_compiler_help,
                  sizeof(nyc_compiler_help) / sizeof(nyc_compiler_help[0]));
    return 0;
  }
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
    const MakeHelp *entry = &commands[i];
    if (strcmp(command, entry->name) != 0)
      continue;
    nyt_heading("Build Tool");
    nyt_kv("Command", "%s", entry->name);
    nyt_kv("Usage", "%s", entry->usage);
    nyt_wrap_value(entry->summary, 2, ny_ui_width() - 4);
    if (entry->rows && *entry->rows)
      render_rows(entry->rows);
    render_rows(
        "[Global options]\n-j, --jobs N|Parallel build jobs.\n-v, "
        "--verbose|Print underlying build commands.\n--color MODE|auto, "
        "always, or "
        "never.\n--no-color|Disable colored output.\n--glyphs MODE|unicode "
        "or ascii.\n--debug|Select debug build outputs.\n--bootstrap-logs|Show "
        "bootstrap status.\n--no-bootstrap-logs|Hide bootstrap status.\n"
        "--version|Print build-tool version.\n-h, --help|Show command help.");
    return 0;
  }
  nyt_heading("Build Tool");
  nyt_kv("Command", "%s", command);
  nyt_wrap_value(
      "No focused help is registered. Run ./make --help for commands.", 2,
      ny_ui_width() - 4);
  return 0;
}
