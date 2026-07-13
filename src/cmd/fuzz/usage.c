#include "core.h"

#define NYTRIX_FUZZ_ALL_RUN_ONE_SHOT \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix fuzz all run --profile insane --hours H --threads 25% --target-thread-years N --dir build/fuzz/all --fail-fast"
#define NYTRIX_FUZZ_ALL_RUN_NEXT_PREVIEW \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 NYTRIX_RUN_DRY_RUN=1 nice -n 10 ./build/fuzz/all/run-next.sh"
#define NYTRIX_FUZZ_ALL_RUN_NEXT_LOW_CPU \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/fuzz/all/run-next.sh"
#define NYTRIX_FUZZ_ALL_RUN_NEXT_GENTLE \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 NYTRIX_RUN_HOURS=1 NYTRIX_RUN_THREADS=10% nice -n 10 ./build/fuzz/all/run-next.sh"
#define NYTRIX_FUZZ_ALL_RUN_NEXT_GENTLE_PREVIEW \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 NYTRIX_RUN_DRY_RUN=1 NYTRIX_RUN_HOURS=1 NYTRIX_RUN_THREADS=10% nice -n 10 ./build/fuzz/all/run-next.sh"
#define NYTRIX_FUZZ_ALL_STATUS_REFRESH \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix fuzz all status --refresh --strict --allow-full-pressure-remediation --dir build/fuzz/all --history build/fuzz/all/history.json --worklist build/fuzz/all/worklist.json --coverage build/fuzz/all/coverage.json --plan build/fuzz/all/plan.json --target-thread-years N --hours H --threads 25% --profile insane --json build/fuzz/all/status.json --markdown build/fuzz/all/status.md"
#define NYTRIX_FUZZ_ALL_PROGRESS_REFRESH \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix fuzz all progress --refresh --strict --allow-full-pressure-remediation --dir build/fuzz/all --status build/fuzz/all/status.json --history build/fuzz/all/history.json --worklist build/fuzz/all/worklist.json --coverage build/fuzz/all/coverage.json --plan build/fuzz/all/plan.json --target-thread-years N --hours H --threads 25% --profile insane --json build/fuzz/all/progress.json --markdown build/fuzz/all/progress.md"
#define NYTRIX_FUZZ_ALL_STATUS_REFRESH_EXAMPLE \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix fuzz all status --refresh --strict --allow-full-pressure-remediation --dir build/fuzz/all --history build/fuzz/all/history.json --worklist build/fuzz/all/worklist.json --coverage build/fuzz/all/coverage.json --plan build/fuzz/all/plan.json --target-thread-years 10 --hours 8 --threads 25% --profile insane --json build/fuzz/all/status.json --markdown build/fuzz/all/status.md"
#define NYTRIX_FUZZ_ALL_PROGRESS_REFRESH_EXAMPLE \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix fuzz all progress --refresh --strict --allow-full-pressure-remediation --dir build/fuzz/all --status build/fuzz/all/status.json --history build/fuzz/all/history.json --worklist build/fuzz/all/worklist.json --coverage build/fuzz/all/coverage.json --plan build/fuzz/all/plan.json --target-thread-years 10 --hours 8 --threads 25% --profile insane --json build/fuzz/all/progress.json --markdown build/fuzz/all/progress.md"
void nytrix_print_worker_usage(FILE *out) {
  fputs("{\"ok\":false,\"error\":\"usage\",\"engine\":\"nytrix_core\",\"commands\":["
        "\"shape-count <file>\",\"hash-case <file>\",\"scan-ir-markers <file>\","
        "\"analyze-ir <file>\",\"source-shape-counts <c-file> <ny-file>\","
        "\"validate-shapes <dir>\",\"generate-batch --shape-dir DIR --out DIR --cases N --seed S --generator auto|typed|ir|stress|mixed [--schedule smart|coverage|ranked|weighted] [--shape NAME] [--insane] [--list]\","
        "\"convert-cbridge --c C --out NY\","
        "\"compare-case --case NAME --c C --ny NY --ir IR --root DIR --ny-bin NY --bin-dir DIR\","
        "\"replay-corpus-entry --corpus-dir DIR --entry-id ID --root DIR --ny-bin NY --bin-dir DIR\"]}\n",
        out);
}

static void nytrix_print_public_usage_impl(FILE *out, bool help) {
  fputs(help ?
        "{\"ok\":true,\"cases\":1,\"ok_count\":1,\"failure_count\":0,"
        "\"command_count\":81,\"engine\":\"nytrix_core\",\"commands\":[" :
        "{\"ok\":false,\"error\":\"usage\",\"cases\":1,\"ok_count\":0,"
        "\"failure_count\":1,\"command_count\":81,\"engine\":\"nytrix_core\","
        "\"commands\":[",
        out);
  fputs("\"shapes audit\",\"bridge convert\",\"bridge compare\",\"bridge suite\","
        "\"bridge generate\",\"bridge perf-matrix\",\"selftest run\",\"selftest run --list\",\"selftest child-tmp-env\",\"selftest python-clean\",\"selftest compiler-cache-classifier\",\"selftest synth-print\",\"selftest synth-schedule\",\"selftest synth-pure\","
        "\"synth random\",\"synth print --lang c|ny|ir|both [--shape NAME|--generator auto|kernel] [--insane] [--list]\",\"synth real --corpus-dir DIR\",\"synth creal --function-db DB\","
        "\"synth generate [--fast] [--capture-failures] [--strict-failures] [--insane]\",\"synth generate --generator ir\",\"synth generate --generator stress\",\"stress run\",\"compiler smoke\",\"compiler findings\",\"compiler known-bugs\",\"compiler std-audit\",\"bench compile\","
        "\"bench repl-jit\",\"bench real\",\"perf triage [--threshold N] [--findings-dir DIR] [--markdown PATH]\",\"fuzz [--target-thread-years N] [--run-hours H] [--threads N|25%] [--dir DIR]\",\"fuzz frontend\","
        "\"fuzz snippets\",\"fuzz corpus prepare\",\"fuzz workspace audit\",\"fuzz harness smoke\",\"fuzz libs smoke\",\"fuzz kernels smoke\","
        "\"fuzz afl run\",\"fuzz all run\",\"fuzz all audit\",\"fuzz all findings\",\"fuzz all history\",\"fuzz all worklist\",\"fuzz all plan\",\"fuzz all coverage\",\"fuzz all status\",\"fuzz all progress\",\"fuzz all old-paths\",\"fuzz all preflight\",\"fuzz gc run\",\"fuzz sanitizers run\",\"campaign run\","
        "\"campaign audit\",\"campaign replay\",\"campaign optimize\","
        "\"corpus build-functions --corpus-dir DIR\",\"corpus build-hosts --corpus-dir DIR\",\"corpus mine-hosts --corpus-dir DIR\","
        "\"corpus creal build --function-db DB\",\"corpus creal audit\",\"corpus creal replay\",\"corpus creal promote\","
        "\"corpus build\",\"corpus audit\",\"corpus replay\",\"corpus promote\","
        "\"replay list\",\"replay promote\","
        "\"reduce artifact\",\"prove lab\",\"shape-count\",\"hash-case\","
        "\"scan-ir-markers\",\"analyze-ir\",\"source-shape-counts\","
        "\"validate-shapes\",\"generate-batch\",\"convert-cbridge\","
        "\"compare-case\",\"replay-corpus-entry\"]}\n",
        out);
}

void nytrix_print_public_usage(FILE *out) {
  nytrix_print_public_usage_impl(out, false);
}

void nytrix_print_public_help(FILE *out) {
  nytrix_print_public_usage_impl(out, true);
}

void nytrix_print_fuzz_all_help(FILE *out) {
  fputs("{\"ok\":true,\"engine\":\"nytrix_core\",\"topic\":\"fuzz all\","
        "\"cases\":1,\"ok_count\":1,\"failure_count\":0,"
        "\"command_count\":19,\"example_count\":16,"
        "\"purpose\":\"repo-local long-run Nytrix compiler, runtime, AFL, proof, and C-vs-Ny perf campaign\","
        "\"quick_probe_command\":\"" NYTRIX_FUZZ_ALL_QUICK_JQ_STATUS "\","
        "\"state_probe_command\":\"" NYTRIX_FUZZ_ALL_STATE_JQ_DEFAULT "\","
        "\"status_command\":\"" NYTRIX_FUZZ_ALL_STATUS_REFRESH_EXAMPLE "\","
        "\"progress_command\":\"" NYTRIX_FUZZ_ALL_PROGRESS_REFRESH_EXAMPLE "\","
        "\"run_next_command\":\"" NYTRIX_FUZZ_ALL_RUN_NEXT_LOW_CPU "\","
        "\"run_next_preview_command\":\"" NYTRIX_FUZZ_ALL_RUN_NEXT_PREVIEW "\","
        "\"run_next_low_cpu_command\":\"" NYTRIX_FUZZ_ALL_RUN_NEXT_LOW_CPU "\","
        "\"run_next_gentle_command\":\"" NYTRIX_FUZZ_ALL_RUN_NEXT_GENTLE "\","
        "\"run_next_gentle_preview_command\":\"" NYTRIX_FUZZ_ALL_RUN_NEXT_GENTLE_PREVIEW "\","
        "\"old_path_probe_command\":\"" NYTRIX_FUZZ_ALL_OLD_PATH_PROBE_COMMAND "\","
        "\"old_path_dry_run_command\":\"" NYTRIX_FUZZ_ALL_OLD_PATH_DRY_RUN_COMMAND "\","
        "\"old_path_apply_command\":\"" NYTRIX_FUZZ_ALL_OLD_PATH_APPLY_COMMAND "\","
        "\"selftest_catalog_command\":\"" NYTRIX_FUZZ_ALL_SELFTEST_CATALOG "\","
        "\"selftest_result_probe_command\":\"" NYTRIX_FUZZ_ALL_SELFTEST_PROBE "\","
        "\"selftest_cockpit_run_command\":\"" NYTRIX_FUZZ_ALL_SELFTEST_RUN "\","
        "\"selftest_cockpit_result_probe_command\":\"" NYTRIX_FUZZ_ALL_SELFTEST_COCKPIT_PROBE "\","
        "\"known_bugs_command\":\"" NYTRIX_FUZZ_ALL_KNOWN_BUGS_COMMAND "\","
        "\"known_bugs_report\":\"" NYTRIX_FUZZ_ALL_KNOWN_BUGS_REPORT "\","
        "\"known_bugs_result_probe_command\":\"" NYTRIX_FUZZ_ALL_KNOWN_BUGS_PROBE "\","
        "\"perf_triage_command\":\"" NYTRIX_FUZZ_ALL_PERF_TRIAGE_COMMAND "\","
        "\"perf_triage_report\":\"" NYTRIX_FUZZ_ALL_PERF_TRIAGE_REPORT "\","
        "\"perf_triage_markdown\":\"" NYTRIX_FUZZ_ALL_PERF_TRIAGE_MARKDOWN "\","
        "\"perf_triage_result_probe_command\":\"" NYTRIX_FUZZ_ALL_PERF_TRIAGE_PROBE "\","
        "\"commands\":["
        "\"fuzz all preflight --dir build/fuzz/all --target-thread-years N --hours H --threads 25% --profile insane\","
        "\"" NYTRIX_FUZZ_ALL_STATUS_REFRESH "\","
        "\"" NYTRIX_FUZZ_ALL_PROGRESS_REFRESH "\","
        "\"" NYTRIX_FUZZ_ALL_QUICK_JQ_STATUS "\","
        "\"" NYTRIX_FUZZ_ALL_STATE_JQ_DEFAULT "\","
        "\"" NYTRIX_FUZZ_ALL_OLD_PATH_DRY_RUN_COMMAND "\","
        "\"" NYTRIX_FUZZ_ALL_OLD_PATH_APPLY_COMMAND "\","
        "\"" NYTRIX_FUZZ_ALL_OLD_PATH_PROBE_COMMAND "\","
        "\"" NYTRIX_FUZZ_ALL_KNOWN_BUGS_COMMAND "\","
        "\"" NYTRIX_FUZZ_ALL_PERF_TRIAGE_COMMAND "\","
        "\"" NYTRIX_FUZZ_ALL_SELFTEST_CATALOG "\","
        "\"" NYTRIX_FUZZ_ALL_SELFTEST_PROBE "\","
        "\"" NYTRIX_FUZZ_ALL_SELFTEST_RUN "\","
        "\"" NYTRIX_FUZZ_ALL_RUN_NEXT_PREVIEW "\","
        "\"" NYTRIX_FUZZ_ALL_RUN_NEXT_LOW_CPU "\","
        "\"" NYTRIX_FUZZ_ALL_RUN_NEXT_GENTLE "\","
        "\"" NYTRIX_FUZZ_ALL_RUN_NEXT_GENTLE_PREVIEW "\","
        "\"" NYTRIX_FUZZ_ALL_RUN_ONE_SHOT "\","
        "\"fuzz all history|coverage|worklist|plan|findings|audit\"],"
        "\"core_flags\":[\"--dir DIR\",\"--target-thread-years N\",\"--hours H\",\"--threads N|25%\",\"--profile smoke|deep|insane\",\"--json PATH\",\"--markdown PATH\",\"--strict\",\"--allow-full-pressure-remediation\",\"--allow-incomplete-coverage\",\"--allow-nytrix\",\"--wait-writers-s N\"],"
        "\"guardrails\":[\"repo-local build/cache scratch and Nytrix cache\",\"Nytrix-owned lanes require --allow-nytrix\",\"default --threads 25%\",\"NYTRIX_RUN_HOURS/NYTRIX_RUN_THREADS run-next override\",\"dry-run preview handoff\",\"run-next state heartbeat\",\"nice/ionice low priority handoff\",\"load wait\",\"free-space guard\",\"campaign lock\",\"max-cycle repeat guard\",\"inter-cycle cooldown\",\"stop-file graceful pause/resume\",\"preflight smoke isolated under build/cache/scratch/fuzz_all_preflight\"],"
        "\"reports\":[\"build/fuzz/all/progress.md\",\"build/fuzz/all/status.md\",\"build/fuzz/all/worklist.md\",\"build/fuzz/all/history.md\",\"build/fuzz/all/coverage.md\",\"build/fuzz/all/plan.md\",\"BUGS.md\",\"FINDINGS.md\"],"
        "\"details\":\"Use build/fuzz/all/status.md and build/fuzz/all/progress.md for exhaustive fields; help stays TLDR.\","
        "\"examples\":["
        "\"" NYTRIX_FUZZ_ALL_PROGRESS_REFRESH_EXAMPLE "\","
        "\"" NYTRIX_FUZZ_ALL_STATUS_REFRESH_EXAMPLE "\","
        "\"" NYTRIX_FUZZ_ALL_QUICK_JQ_STATUS "\","
        "\"" NYTRIX_FUZZ_ALL_STATE_JQ_DEFAULT "\","
        "\"" NYTRIX_FUZZ_ALL_OLD_PATH_DRY_RUN_COMMAND "\","
        "\"" NYTRIX_FUZZ_ALL_OLD_PATH_APPLY_COMMAND "\","
        "\"" NYTRIX_FUZZ_ALL_OLD_PATH_PROBE_COMMAND "\","
        "\"" NYTRIX_FUZZ_ALL_KNOWN_BUGS_COMMAND "\","
        "\"" NYTRIX_FUZZ_ALL_PERF_TRIAGE_COMMAND "\","
        "\"" NYTRIX_FUZZ_ALL_SELFTEST_CATALOG "\","
        "\"" NYTRIX_FUZZ_ALL_SELFTEST_PROBE "\","
        "\"" NYTRIX_FUZZ_ALL_SELFTEST_RUN "\","
        "\"" NYTRIX_FUZZ_ALL_RUN_NEXT_PREVIEW "\","
        "\"" NYTRIX_FUZZ_ALL_RUN_NEXT_LOW_CPU "\","
        "\"" NYTRIX_FUZZ_ALL_RUN_NEXT_GENTLE "\","
        "\"" NYTRIX_FUZZ_ALL_RUN_NEXT_GENTLE_PREVIEW "\"],"
        "\"cache_policy\":{\"reports\":\"build/\",\"scratch\":\"build/cache/\",\"forbidden\":[\"old-sibling-test-scratch\",\"old-sibling-fuzz-dir\",\"old-sibling-build-cache\"]}}\n",
        out);
}
