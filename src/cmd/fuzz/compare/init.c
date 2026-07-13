#include "../core.h"

#include "util.c"
#include "audit.c"
#include "status.c"
#include "tools.c"
#include "selftest.c"

typedef struct {
  const char *a1, *a2, *a3;
  int (*fn)(int, char**);
} CmdEntry;

static const CmdEntry kCmds[] = {
  {"etc/tests/fuzz/shapes", "audit", NULL, cmd_public_shapes_audit},
  {"bridge", "convert", NULL, cmd_public_bridge_convert},
  {"bridge", "compare", NULL, cmd_public_bridge_compare},
  {"bridge", "suite", NULL, cmd_public_bridge_suite},
  {"bridge", "generate", NULL, cmd_public_bridge_generate},
  {"bridge", "perf-matrix", NULL, cmd_public_bridge_perf_matrix},
  {"selftest", "run", NULL, cmd_public_selftest_run},
  {"selftest", "child-tmp-env", NULL, cmd_public_selftest_child_tmp_env},
  {"selftest", "worker-args", NULL, cmd_public_selftest_worker_args},
  {"selftest", "synth-print", NULL, cmd_public_selftest_synth_print},
  {"selftest", "synth-schedule", NULL, cmd_public_selftest_synth_schedule},
  {"selftest", "synth-pure", NULL, cmd_public_selftest_synth_pure},
  {"selftest", "python-clean", NULL, cmd_public_selftest_python_clean},
  {"selftest", "compiler-cache-classifier", NULL, cmd_public_selftest_compiler_cache_classifier},
  {"stress", "run", NULL, cmd_public_stress_run},
  {"compiler", "smoke", NULL, cmd_public_compiler_smoke},
  {"compiler", "findings", NULL, cmd_public_compiler_findings},
  {"compiler", "known-bugs", NULL, cmd_public_compiler_known_bugs},
  {"compiler", "std-audit", NULL, cmd_public_compiler_std_audit},
  {"bench", "compile", NULL, cmd_public_bench_compile},
  {"bench", "repl-jit", NULL, cmd_public_bench_repl_jit},
  {"bench", "real", NULL, cmd_public_bench_real},
  {"perf", "triage", NULL, cmd_public_perf_triage},
  {"fuzz", "frontend", NULL, cmd_public_fuzz_frontend},
  {"fuzz", "snippets", NULL, cmd_public_fuzz_snippets},
  {"synth", "generate", NULL, cmd_public_synth_generate},
  {"synth", "print", NULL, cmd_public_synth_print},
  {"synth", "creal", NULL, cmd_public_synth_creal},
  {"synth", "random", NULL, cmd_public_synth_random},
  {"synth", "real", NULL, cmd_public_synth_real},
  {"campaign", "run", NULL, cmd_public_campaign_run},
  {"campaign", "audit", NULL, cmd_public_campaign_audit},
  {"campaign", "optimize", NULL, cmd_public_campaign_optimize},
  {"prove", "lab", NULL, cmd_public_prove_lab},
  {"replay", "list", NULL, cmd_public_replay_list},
  {"replay", "promote", NULL, cmd_public_replay_promote},
  {"reduce", "artifact", NULL, cmd_public_reduce_artifact},
  {"fuzz", "corpus", "prepare", cmd_public_fuzz_corpus_prepare},
  {"fuzz", "workspace", "audit", cmd_public_fuzz_workspace_audit},
  {"fuzz", "harness", "smoke", cmd_public_fuzz_harness_smoke},
  {"fuzz", "libs", "smoke", cmd_public_fuzz_libs_smoke},
  {"fuzz", "kernels", "smoke", cmd_public_fuzz_kernels_smoke},
  {"fuzz", "all", "audit", cmd_public_fuzz_all_audit},
  {"fuzz", "all", "findings", cmd_public_fuzz_all_findings},
  {"fuzz", "all", "history", cmd_public_fuzz_all_history},
  {"fuzz", "all", "worklist", cmd_public_fuzz_all_worklist},
  {"fuzz", "all", "plan", cmd_public_fuzz_all_plan},
  {"fuzz", "all", "coverage", cmd_public_fuzz_all_coverage},
  {"fuzz", "all", "status", cmd_public_fuzz_all_status},
  {"fuzz", "all", "progress", cmd_public_fuzz_all_progress},
  {"fuzz", "all", "preflight", cmd_public_fuzz_all_preflight},
  {"fuzz", "all", "run", cmd_public_fuzz_all_run},
  {"fuzz", "afl", "run", cmd_public_fuzz_afl_run},
  {"fuzz", "gc", "run", cmd_public_fuzz_gc_run},
  {"fuzz", "sanitizers", "run", cmd_public_fuzz_sanitizers_run},
};

static int table_dispatch(int argc, char **argv) {
  for (int i = 0; i < (int)(sizeof(kCmds) / sizeof(kCmds[0])); i++) {
    int need = kCmds[i].a3 ? 4 : 3;
    if (argc >= need && !strcmp(argv[1], kCmds[i].a1) &&
        !strcmp(argv[2], kCmds[i].a2) &&
        (!kCmds[i].a3 || !strcmp(argv[3], kCmds[i].a3)))
      return kCmds[i].fn(argc, argv);
  }
  return -1;
}

int main(int argc, char **argv) {
  init_self_path(argc > 0 ? argv[0] : "nytrix");
  if (wants_fuzz_all_help(argc, argv)) {
    nytrix_print_fuzz_all_help(stdout);
    return 0;
  }
  if (selftest_run_wants_catalog(argc, argv))
    return cmd_public_selftest_run(argc, argv);
  if (argc >= 2 && (is_help_flag(argv[1]) || !strcmp(argv[1], "help"))) {
    nytrix_print_public_help(stdout);
    return 0;
  }
  for (int i = 2; i < argc; i++)
    if (is_help_flag(argv[i])) { nytrix_print_public_help(stdout); return 0; }
  if (argc >= 3 && !strcmp(argv[2], "help")) {
    nytrix_print_public_help(stdout);
    return 0;
  }
  if (argc < 2) return worker_usage();
  int rc = dispatch_worker(argc, argv);
  if (rc != -1) return rc;
  if (argc >= 2 && !strcmp(argv[1], "fuzz") &&
      (argc == 2 || (argc >= 3 && strncmp(argv[2], "--", 2) == 0)))
    return cmd_public_fuzz_auto(argc, argv);
  if (argc >= 4 && !strcmp(argv[1], "fuzz") && !strcmp(argv[2], "all") &&
      (!strcmp(argv[3], "old-paths") || !strcmp(argv[3], "old-path")))
    return cmd_public_fuzz_all_old_paths(argc, argv);
  rc = table_dispatch(argc, argv);
  if (rc >= 0) return rc;
  if (argc >= 4 && !strcmp(argv[1], "corpus") && !strcmp(argv[2], "creal")) {
    if (!strcmp(argv[3], "build")) return cmd_public_corpus_creal_build(argc, argv);
    if (!strcmp(argv[3], "audit")) return cmd_public_corpus_audit(argc, argv, false);
    if (!strcmp(argv[3], "replay")) return cmd_public_corpus_replay(argc, argv, false);
    if (!strcmp(argv[3], "promote")) return cmd_public_corpus_promote(argc, argv, false);
  }
  if (argc >= 4 && !strcmp(argv[1], "fuzz") && !strcmp(argv[2], "gc") &&
      !strcmp(argv[3], "campaign"))
    return cmd_public_fuzz_gc_campaign_run(argc, argv, NULL);
  if (argc >= 3) {
    if (!strcmp(argv[1], "corpus")) {
      if (!strcmp(argv[2], "build")) return cmd_public_corpus_build(argc, argv, false);
      if (!strcmp(argv[2], "replay")) return cmd_public_corpus_replay(argc, argv, false);
      if (!strcmp(argv[2], "audit")) return cmd_public_corpus_audit(argc, argv, false);
      if (!strcmp(argv[2], "promote")) return cmd_public_corpus_promote(argc, argv, false);
      if (!strcmp(argv[2], "build-functions")) return cmd_public_corpus_real_db(argc, argv, "functions");
      if (!strcmp(argv[2], "build-hosts")) return cmd_public_corpus_real_db(argc, argv, "hosts");
      if (!strcmp(argv[2], "mine-hosts")) return cmd_public_corpus_real_db(argc, argv, "mined-hosts");
    }
    if (!strcmp(argv[1], "campaign") && !strcmp(argv[2], "replay"))
      return cmd_public_corpus_replay(argc, argv, false);
  }
  return unsupported_command(argc, argv);
}
