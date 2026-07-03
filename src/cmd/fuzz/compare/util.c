#include <limits.h>
#include <sys/resource.h>

static char g_self_path[4096];
static const char *NYNTH_DEFAULT_SCRATCH_ROOT = "build/cache/scratch";
static const char *NYNTH_DEFAULT_FUZZ_THREADS = "25%";
static const char *NYNTH_LOW_CPU_MISSING_EVIDENCE_HOURS = "1";
static const char *NYNTH_LOW_CPU_MISSING_EVIDENCE_THREADS = "10%";
static const int NYNTH_DEFAULT_RUN_NICE = 10;
static const int NYNTH_DEFAULT_MAX_LOAD_PCT = 75;
static const int NYNTH_DEFAULT_MIN_FREE_GB = 20;
#define NYNTH_OLD_PATH_DRY_RUN_COMMAND \
  NYNTH_FUZZ_ALL_OLD_PATH_DRY_RUN_COMMAND
#define NYNTH_OLD_PATH_APPLY_COMMAND \
  NYNTH_FUZZ_ALL_OLD_PATH_APPLY_COMMAND
static const char *NYNTH_COMPILER_STD_AUDIT_JSON =
    "build/fuzz/ultra/compiler-std-audit.json";
static const char *NYNTH_COMPILER_STD_AUDIT_MARKDOWN =
    "build/fuzz/ultra/compiler-std-audit.md";
static const char *NYNTH_COMPILER_STD_AUDIT_COMMAND =
    "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth compiler std-audit --json build/fuzz/ultra/compiler-std-audit.json --markdown build/fuzz/ultra/compiler-std-audit.md";
static const char *NYNTH_RUNTIME_SURFACE_SCOPE = "surface-reference-coverage";
static const char *NYNTH_CRT_SURFACE_SCOPE = "surface-reference-coverage";
static const char *NYNTH_CRT_BEHAVIOR_STATE = "campaign-gated";
static const char *NYNTH_CRT_BEHAVIOR_SCOPE = "not-bugless-proof";
static const char *NYNTH_CRT_BEHAVIOR_NEXT_ACTION =
    "continue-campaign-evidence";
static const char *NYNTH_CRT_BEHAVIOR_NEXT_REASON =
    "CRT behavior remains campaign-gated; run or freshen fuzz-all campaign evidence before calling CRT behavior bugless";
static const char *NYNTH_CRT_BEHAVIOR_NEXT_COMMAND =
    "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/fuzz/all/run-next.sh";

static bool path_exists_maybe_root(const char *root, const char *path);
static bool read_file_maybe_root(const char *root, const char *path,
                                 file_buf_t *out);
static void append_fuzz_all_campaign_alias_fields(
    str_buf_t *b, double thread_years, double target_thread_years,
    double remaining_thread_years, double campaign_percent, double runs_needed,
    double wall_hours_needed, double wall_days_needed,
    double thread_years_per_run, double percent_per_run,
    double runs_per_day, double thread_years_per_day,
    double campaign_plan_wall_hours, const char *campaign_plan_threads,
    const char *completion_eta_local);
static double fuzz_all_campaign_calendar_percent_10y(double age_days);

static const char *fuzz_all_handoff_guard_summary(void) {
  return "low-priority nice 10; load-wait 75%; disk >=20GB; lock on; threads 25%";
}

static void append_fuzz_all_crt_behavior_next_fields(str_buf_t *row) {
  if (!row) return;
  (void)sb_append(row, ",\"crt_behavior_next_action\":");
  (void)sb_append_json_str(row, NYNTH_CRT_BEHAVIOR_NEXT_ACTION);
  (void)sb_append(row, ",\"crt_behavior_next_reason\":");
  (void)sb_append_json_str(row, NYNTH_CRT_BEHAVIOR_NEXT_REASON);
  (void)sb_append(row, ",\"crt_behavior_next_command\":");
  (void)sb_append_json_str(row, NYNTH_CRT_BEHAVIOR_NEXT_COMMAND);
}

static void append_fuzz_all_handoff_guard_fields(str_buf_t *row) {
  if (!row) return;
  (void)sb_appendf(row,
                   ",\"handoff_low_priority_default\":true,"
                   "\"handoff_nice_default\":%d,"
                   "\"handoff_load_wait_default\":true,"
                   "\"handoff_max_load_pct_default\":%d,"
                   "\"handoff_space_guard_default\":true,"
                   "\"handoff_min_free_gb_default\":%d,"
                   "\"handoff_run_lock_default\":true",
                   NYNTH_DEFAULT_RUN_NICE, NYNTH_DEFAULT_MAX_LOAD_PCT,
                   NYNTH_DEFAULT_MIN_FREE_GB);
  (void)sb_append(row, ",\"handoff_threads_default\":");
  (void)sb_append_json_str(row, NYNTH_DEFAULT_FUZZ_THREADS);
  (void)sb_append(row, ",\"handoff_guard_summary\":");
  (void)sb_append_json_str(row, fuzz_all_handoff_guard_summary());
}

static void append_fuzz_all_handoff_guard_markdown(str_buf_t *md) {
  if (!md) return;
  (void)sb_append(md, "- Handoff guards: ");
  (void)sb_append(md, fuzz_all_handoff_guard_summary());
  (void)sb_append(md, ".\n");
}

static const char *fuzz_all_old_path_next_action(double remaining_count,
                                                 bool active_writer) {
  if (active_writer) return "wait-old-writer";
  return remaining_count > 0.0 ? "archive-old-paths" : "none";
}

static const char *fuzz_all_old_path_next_reason(double remaining_count,
                                                 bool active_writer) {
  if (active_writer)
    return "old sibling writer is active; wait before archiving old paths";
  if (remaining_count > 0.0)
    return "old sibling output remains; apply archives it under this repo build/cache";
  return "old sibling output paths are absent";
}

static void append_repo_cache_env_script(str_buf_t *sh, const char *root) {
  if (!sh) return;
  (void)sb_append(sh, "cd ");
  (void)sb_append_json_str(sh, root && *root ? root : ".");
  (void)sb_append(sh, "\n");
  (void)sb_append(sh, "mkdir -p \"build/cache/tmp\" \"build/cache/scratch\" \"build/cache/xdg\" \"build/cache/nytrix\"\n");
  (void)sb_append(sh, "export NYNTH_ROOT=\"$PWD\"\n");
  (void)sb_append(sh, "export TMPDIR=\"$PWD/build/cache/tmp\"\n");
  (void)sb_append(sh, "export TMP=\"$TMPDIR\"\n");
  (void)sb_append(sh, "export TEMP=\"$TMPDIR\"\n");
  (void)sb_append(sh, "export NYNTH_CHILD_TMPDIR=\"$TMPDIR\"\n");
  (void)sb_append(sh, "export NYNTH_SCRATCH_ROOT=\"$PWD/build/cache/scratch\"\n");
  (void)sb_append(sh, "export XDG_CACHE_HOME=\"$PWD/build/cache/xdg\"\n");
  (void)sb_append(sh, "export NYTRIX_CACHE_DIR=\"$PWD/build/cache/nytrix\"\n");
}

static void append_low_priority_shell_helper(str_buf_t *sh) {
  if (!sh) return;
  (void)sb_append(sh,
                  "NYNTH_RUN_NICE=\"${NYNTH_RUN_NICE:-10}\"\n"
                  "nynth_low_priority() {\n"
                  "  if [ \"${NYNTH_LOW_PRIORITY:-1}\" = \"0\" ]; then \"$@\"; return $?; fi\n"
                  "  if command -v ionice >/dev/null 2>&1; then\n"
                  "    ionice -c 3 nice -n \"$NYNTH_RUN_NICE\" \"$@\"\n"
                  "  else\n"
                  "    nice -n \"$NYNTH_RUN_NICE\" \"$@\"\n"
                  "  fi\n"
                  "}\n");
}

static bool nynth_apply_process_low_priority(int *target_nice_out,
                                             int *current_nice_out) {
  if (target_nice_out) *target_nice_out = 10;
  if (current_nice_out) *current_nice_out = 0;
  const char *enabled = getenv("NYNTH_LOW_PRIORITY");
  if (enabled && strcmp(enabled, "0") == 0) return false;
  const char *nice_env = getenv("NYNTH_RUN_NICE");
  char *end = NULL;
  long target = nice_env && *nice_env ? strtol(nice_env, &end, 10) : 10;
  if (!nice_env || !*nice_env || (end && *end)) target = 10;
  if (target < 0) target = 0;
  if (target > 19) target = 19;
  if (target_nice_out) *target_nice_out = (int)target;
  errno = 0;
  int current = getpriority(PRIO_PROCESS, 0);
  if (errno != 0) current = 0;
  if (current_nice_out) *current_nice_out = current;
  if (current >= (int)target) return false;
  if (setpriority(PRIO_PROCESS, 0, (int)target) == 0) {
    if (current_nice_out) *current_nice_out = (int)target;
    return true;
  }
  return false;
}

static void append_load_wait_shell_helper(str_buf_t *sh) {
  if (!sh) return;
  (void)sb_append(sh,
                  "NYNTH_MAX_LOAD_PCT=\"${NYNTH_MAX_LOAD_PCT:-75}\"\n"
                  "NYNTH_LOAD_SLEEP_S=\"${NYNTH_LOAD_SLEEP_S:-60}\"\n"
                  "nynth_wait_for_load() {\n"
                  "  if [ \"${NYNTH_LOAD_WAIT:-1}\" = \"0\" ]; then return 0; fi\n"
                  "  if [ ! -r /proc/loadavg ] || ! command -v awk >/dev/null 2>&1; then return 0; fi\n"
                  "  local cpus load max_load ok\n"
                  "  cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')\n"
                  "  while :; do\n"
                  "    load=$(awk '{print $1}' /proc/loadavg 2>/dev/null || printf '0')\n"
                  "    max_load=$(awk -v c=\"$cpus\" -v p=\"$NYNTH_MAX_LOAD_PCT\" 'BEGIN{printf \"%.2f\", c*p/100.0}')\n"
                  "    ok=$(awk -v l=\"$load\" -v m=\"$max_load\" 'BEGIN{print (l <= m) ? 1 : 0}')\n"
                  "    if [ \"$ok\" = \"1\" ]; then return 0; fi\n"
                  "    echo \"nynth load wait: load=$load max=$max_load sleep=${NYNTH_LOAD_SLEEP_S}s\"\n"
                  "    sleep \"$NYNTH_LOAD_SLEEP_S\"\n"
                  "  done\n"
                  "}\n");
}

static void append_space_guard_shell_helper(str_buf_t *sh) {
  if (!sh) return;
  (void)sb_append(sh,
                  "NYNTH_MIN_FREE_GB=\"${NYNTH_MIN_FREE_GB:-20}\"\n"
                  "NYNTH_SPACE_PATH=\"${NYNTH_SPACE_PATH:-$NYNTH_ROOT}\"\n"
                  "nynth_require_free_space() {\n"
                  "  if [ \"${NYNTH_SPACE_GUARD:-1}\" = \"0\" ]; then return 0; fi\n"
                  "  if [ \"$NYNTH_MIN_FREE_GB\" = \"0\" ]; then return 0; fi\n"
                  "  if ! command -v df >/dev/null 2>&1 || ! command -v awk >/dev/null 2>&1; then return 0; fi\n"
                  "  local path avail_kb required_kb ok\n"
                  "  path=\"$NYNTH_SPACE_PATH\"\n"
                  "  if [ -z \"$path\" ]; then path=\"$PWD\"; fi\n"
                  "  avail_kb=$(df -Pk \"$path\" 2>/dev/null | awk 'NR==2 {print $4}')\n"
                  "  if [ -z \"$avail_kb\" ]; then return 0; fi\n"
                  "  required_kb=$(awk -v gb=\"$NYNTH_MIN_FREE_GB\" 'BEGIN{printf \"%.0f\", gb*1024*1024}')\n"
                  "  ok=$(awk -v a=\"$avail_kb\" -v r=\"$required_kb\" 'BEGIN{print (a >= r) ? 1 : 0}')\n"
                  "  if [ \"$ok\" = \"1\" ]; then return 0; fi\n"
                  "  echo \"nynth disk guard: free=${avail_kb}KB required=${required_kb}KB path=$path\"\n"
                  "  return 74\n"
                  "}\n");
}

static void append_campaign_lock_shell_helper(str_buf_t *sh) {
  if (!sh) return;
  (void)sb_append(sh,
                  "NYNTH_RUN_LOCK=\"${NYNTH_RUN_LOCK:-1}\"\n"
                  "NYNTH_ACTIVE_RUN_LOCK_DIR=\"\"\n"
                  "NYNTH_ACTIVE_RUN_LOCK_PID_FILE=\"\"\n"
                  "nynth_release_campaign_lock() {\n"
                  "  if [ -n \"${NYNTH_ACTIVE_RUN_LOCK_PID_FILE:-}\" ]; then rm -f \"$NYNTH_ACTIVE_RUN_LOCK_PID_FILE\"; fi\n"
                  "  if [ -n \"${NYNTH_ACTIVE_RUN_LOCK_DIR:-}\" ]; then rmdir \"$NYNTH_ACTIVE_RUN_LOCK_DIR\" 2>/dev/null || true; fi\n"
                  "}\n"
                  "nynth_acquire_campaign_lock() {\n"
                  "  if [ \"$NYNTH_RUN_LOCK\" = \"0\" ]; then return 0; fi\n"
                  "  local dir lock_dir pid_file pid\n"
                  "  dir=\"${1:-.}\"\n"
                  "  mkdir -p \"$dir\"\n"
                  "  lock_dir=\"${NYNTH_RUN_LOCK_DIR:-$dir/.nynth-run.lock}\"\n"
                  "  pid_file=\"$lock_dir/pid\"\n"
                  "  if mkdir \"$lock_dir\" 2>/dev/null; then\n"
                  "    printf '%s\\n' \"$$\" > \"$pid_file\"\n"
                  "    NYNTH_ACTIVE_RUN_LOCK_DIR=\"$lock_dir\"\n"
                  "    NYNTH_ACTIVE_RUN_LOCK_PID_FILE=\"$pid_file\"\n"
                  "    trap nynth_release_campaign_lock EXIT\n"
                  "    trap 'nynth_release_campaign_lock; exit 130' INT TERM\n"
                  "    return 0\n"
                  "  fi\n"
                  "  pid=\"\"\n"
                  "  if [ -r \"$pid_file\" ]; then pid=$(cat \"$pid_file\" 2>/dev/null || true); fi\n"
                  "  if ! printf '%s\\n' \"$pid\" | grep -Eq '^[0-9]+$'; then pid=\"\"; fi\n"
                  "  if [ -n \"$pid\" ] && kill -0 \"$pid\" 2>/dev/null; then\n"
                  "    echo \"nynth run lock: campaign already active pid=$pid lock=$lock_dir\"\n"
                  "    return 75\n"
                  "  fi\n"
                  "  rm -f \"$pid_file\"\n"
                  "  rmdir \"$lock_dir\" 2>/dev/null || true\n"
                  "  if mkdir \"$lock_dir\" 2>/dev/null; then\n"
                  "    printf '%s\\n' \"$$\" > \"$pid_file\"\n"
                  "    NYNTH_ACTIVE_RUN_LOCK_DIR=\"$lock_dir\"\n"
                  "    NYNTH_ACTIVE_RUN_LOCK_PID_FILE=\"$pid_file\"\n"
                  "    trap nynth_release_campaign_lock EXIT\n"
                  "    trap 'nynth_release_campaign_lock; exit 130' INT TERM\n"
                  "    return 0\n"
                  "  fi\n"
                  "  echo \"nynth run lock: could not acquire $lock_dir\"\n"
                  "  return 75\n"
                  "}\n");
}

static bool shell_text_has_repo_cache_env(const char *text) {
  return text &&
         strstr(text, "NYNTH_ROOT") &&
         strstr(text, "TMPDIR") &&
         strstr(text, "TMP=") &&
         strstr(text, "TEMP=") &&
         strstr(text, "NYNTH_CHILD_TMPDIR") &&
         strstr(text, "NYNTH_SCRATCH_ROOT") &&
         strstr(text, "XDG_CACHE_HOME") &&
         strstr(text, "NYTRIX_CACHE_DIR") &&
         strstr(text, "build/cache/tmp") &&
         strstr(text, "build/cache/scratch") &&
         strstr(text, "build/cache/xdg") &&
         strstr(text, "build/cache/nytrix");
}

typedef struct {
  char **items;
  int count;
  int cap;
} string_list_t;

typedef struct {
  int count;
  double sum;
  double min;
  double max;
  char min_case[256];
  char max_case[256];
  int faster_or_equal;
} ratio_stats_t;

typedef struct {
  string_list_t rows;
  str_buf_t failures_json;
  int failure_count;
  int failed_rows;
  ratio_stats_t ny_o3i_run;
  ratio_stats_t ny_o3_run;
  double worker_ms;
} report_rows_t;

typedef struct {
  char *name;
  char *shape;
  char *family;
  char *generator;
  char *method;
  char *source_kind;
  char *c_path;
  char *ny_path;
  char *ir_path;
  char *features_csv;
  char *json;
} generated_case_t;

typedef struct {
  bool found;
  char *tool;
  char *phase;
  char *flavor;
  char *failure_kind;
  char *stderr_tail;
  char *stdout_tail;
  char *stderr_text;
  char *stdout_text;
  int rc;
  bool timed_out;
} captured_failure_t;

typedef struct {
  generated_case_t *items;
  int count;
  int cap;
} generated_case_list_t;

static char *generated_cases_fingerprint(const generated_case_list_t *cases);

typedef struct {
  char *id;
  char *json;
} manifest_entry_t;

typedef struct {
  manifest_entry_t *items;
  int count;
  int cap;
} manifest_entry_list_t;

typedef struct {
  char name[64];
  char shape[96];
  char features_csv[384];
} bridge_kernel_spec_t;

typedef struct {
  const char *name;
  const char *description;
  const char *keys[4];
  const char *values[4];
  int env_count;
  bool default_on;
  bool comparison_only;
  const char *opt_out_for;
} opt_variant_native_t;

typedef struct {
  char *case_name;
  char *shape;
  char *features_json;
  char *report;
  double ratio;
} matrix_row_t;

typedef struct {
  matrix_row_t *items;
  int count;
  int cap;
} matrix_row_list_t;

typedef struct {
  int native_compare;
  int native_generation;
  int native_replay;
  int native_cbridge;
} worker_counts_t;

typedef enum {
  SELFTEST_STANDARD_REPORT = 0,
  SELFTEST_SHAPE_AUDIT = 1,
  SELFTEST_UNSUPPORTED_STDOUT = 2,
  SELFTEST_FUZZ_REPORTING = 3,
  SELFTEST_SANITIZER_DRY_RUN = 4,
  SELFTEST_FUZZ_FRESH_HANDOFF = 5,
  SELFTEST_PERF_TRIAGE_ARGS_REPORT = 6,
  SELFTEST_CLI_EQUALS_ARGS_REPORT = 7,
  SELFTEST_WORKER_EQUALS_ARGS_REPORT = 8,
  SELFTEST_SYNTH_PRINT_REPORT = 9,
  SELFTEST_REDUCE_ARTIFACT_REPORT = 10,
  SELFTEST_AFL_COMPILER_DRY_RUN = 11,
  SELFTEST_FUZZ_FULL_PRESSURE_REMEDIATION = 12,
  SELFTEST_FUZZ_DEFAULT_PRESSURE = 13,
  SELFTEST_FUZZ_REPRO_READY_MISSING_WRAPPER = 14,
  SELFTEST_FUZZ_REPRO_READY_MISSING_COMMAND = 15,
  SELFTEST_FUZZ_ALL_HELP = 16,
  SELFTEST_FUZZ_ALL_PROGRESS = 17,
  SELFTEST_FUZZ_ALL_PROGRESS_REFRESH_FAIL = 18,
  SELFTEST_FUZZ_ALL_OLD_PATHS = 19,
  SELFTEST_FUZZ_ALL_OLD_PATHS_DRY_RUN = 20,
  SELFTEST_FUZZ_ALL_OLD_PATHS_EMPTY_DRY_RUN = 21,
  SELFTEST_FUZZ_ALL_OLD_WRITER_CLASSIFIER = 22,
  SELFTEST_FUZZ_GC_CAMPAIGN_COMPACT = 23,
  SELFTEST_SELFTEST_ROW_REPORTS = 24,
  SELFTEST_SELFTEST_CATALOG = 25,
  SELFTEST_FUZZ_ALL_PROGRESS_STALE_EVIDENCE = 26,
  SELFTEST_FUZZ_ALL_PROGRESS_CANONICAL = 27,
  SELFTEST_FUZZ_ALL_STATUS_CANONICAL = 28,
  SELFTEST_FUZZ_ALL_STATUS_STALE_EVIDENCE = 29,
  SELFTEST_FUZZ_ALL_REPEAT_STATUS_PROGRESS = 30,
  SELFTEST_FUZZ_ALL_COVERAGE_COMMANDS = 31,
  SELFTEST_FUZZ_ALL_HISTORY_COMMANDS = 32,
  SELFTEST_FUZZ_ALL_PREFLIGHT_ISOLATION = 33,
  SELFTEST_FUZZ_ALL_PLAN_COVERAGE_NEXT = 34,
  SELFTEST_COMPILER_STD_AUDIT_REPORT = 35,
  SELFTEST_FUZZ_ALL_COVERAGE_FOCUS_COMPANIONS = 36,
  SELFTEST_COMPILER_KNOWN_BUGS_REPORT = 37,
  SELFTEST_SELFTEST_SKIP_REPORTS = 38
} selftest_validator_t;

typedef struct {
  const char *name;
  const char *const *args;
  int arg_count;
  double timeout_s;
  bool slow;
  selftest_validator_t validator;
} selftest_spec_t;

typedef struct {
  char *row;
  char *case_name;
  char *ny_source;
  char *c_source;
  double ratio;
  double initial_ratio;
  double confirmation_ratio;
  double c_elapsed_ns;
  double ny_elapsed_ns;
  double c_instructions;
  double ny_instructions;
  int runs;
  int warmup;
  int confirmation_runs;
  int confirmation_warmup;
  bool ok;
  bool initially_hot;
  bool confirmed;
  bool confirmation_ok;
  bool demoted_hotspot;
} triage_item_t;

static const char *const PERF_REAL_CASES[] = {
  "binary", "calls", "dict", "fibonacci", "float", "intops", "iter",
  "list", "matrix", "mandelbrot", "sieve", "spectral", "string", "vector"
};

static int perf_real_case_count(void) {
  return (int)(sizeof(PERF_REAL_CASES) / sizeof(PERF_REAL_CASES[0]));
}

typedef struct {
     char scan_dir[4096];
     char first_report[4096];
     char latest_report[4096];
  char latest_full_pressure_report[4096];
  char worst_report[4096];
  char worst_perf_case[128];
  char latest_perf_case[128];
  char latest_full_pressure_perf_case[128];
  int files_scanned;
  int json_files;
  int reports;
  int emitted_reports;
  int ignored_no_evidence_reports;
  int ok_reports;
  int failed_reports;
  int attention_reports;
  int full_pressure_reports;
  int full_pressure_ok_reports;
  int full_pressure_attention_reports;
  double total_duration_s;
  double total_budget_s;
  double total_effective_budget_s;
  double first_report_epoch;
  double latest_report_epoch;
  double campaign_calendar_span_days;
  double campaign_calendar_age_days;
  double total_thread_s;
  double full_pressure_thread_s;
  double total_lanes;
  double total_ok_lanes;
  double total_failures;
  double total_sub_rows;
  double total_sub_failures;
  double max_threads;
  double finding_live_total;
  double finding_missing_total;
  double known_reproduced_total;
  double known_lost_total;
  double known_baseline_total;
  double perf_hotspots_total;
  double perf_max_ratio;
  double latest_lanes;
  double latest_ok_lanes;
  double latest_failure_count;
  double latest_sub_failures;
  double latest_finding_live;
  double latest_finding_missing;
  double latest_known_reproduced;
  double latest_known_lost;
  double latest_known_baseline;
  double latest_perf_hotspots;
  double latest_perf_max_ratio;
  double latest_full_pressure_lanes;
  double latest_full_pressure_ok_lanes;
  double latest_full_pressure_failure_count;
  double latest_full_pressure_sub_failures;
  double latest_full_pressure_finding_live;
  double latest_full_pressure_finding_missing;
  double latest_full_pressure_known_reproduced;
  double latest_full_pressure_known_lost;
  double latest_full_pressure_known_baseline;
  double latest_full_pressure_perf_hotspots;
  double latest_full_pressure_perf_max_ratio;
  bool latest_report_ok;
  bool latest_report_attention;
  bool latest_full_pressure_ok;
  bool latest_full_pressure_attention;
} fuzz_all_history_summary_t;

typedef struct {
  char history_report[4096];
  char worklist_report[4096];
  char historical_worklist_report[4096];
  char historical_worklist_markdown[4096];
     char coverage_report[4096];
     char plan_report[4096];
     char latest_report[4096];
  char campaign_first_report[4096];
  char latest_full_pressure_report[4096];
    char next_script[4096];
  char next_handoff_command[4096];
  char next_command[4096];
  char preview_command[4096];
  char stop_file[4096];
  char stop_command[4096];
  char resume_command[4096];
  char state_file[4096];
  char state_command[4096];
  char state_refresh_command[4096];
  char state_phase[64];
  char state_event[64];
  char state_timestamp_utc[64];
  char state_last_report[4096];
  bool state_readable;
  bool state_fresh;
  bool state_child_alive;
  double state_age_seconds;
  double state_cycle;
  double state_cycles;
  double state_heartbeat_s;
  double state_heartbeat_count;
  double state_child_pid;
  char progress_command[4096];
  char status_command[4096];
  char old_path_command[4096];
  char old_path_dry_run_command[4096];
  char old_path_apply_command[4096];
  char old_path_next_action[64];
  char old_path_next_reason[256];
  char old_path_report[4096];
  char old_path_markdown[4096];
  char advisory_action_command[4096];
  char advisory_recheck_command[4096];
  double advisory_recheck_raw_repro_checked;
  double advisory_recheck_raw_repro_passed;
  double advisory_recheck_raw_repro_timeouts;
  double advisory_recheck_raw_repro_unexpected;
  char perf_watchlist_command[4096];
  char perf_watchlist_report[4096];
  char perf_watchlist_markdown[4096];
  bool perf_watchlist_artifact_readable;
  bool perf_watchlist_artifact_fresh;
  double perf_watchlist_artifact_hotspots;
  double perf_watchlist_artifact_max_ratio;
  double perf_watchlist_artifact_age_seconds;
  double perf_watchlist_artifact_stale_after_hours;
  char perf_watchlist_artifact_max_case[128];
  char perf_watchlist_artifact_max_artifact[4096];
  char perf_watchlist_artifact_max_ny_source[4096];
  char perf_watchlist_artifact_max_c_source[4096];
  bool compiler_std_audit_readable;
  char compiler_std_audit_report[4096];
  char compiler_std_audit_markdown[4096];
  char compiler_std_audit_command[4096];
  char runtime_surface_state[64];
  double runtime_exports;
  double direct_runtime_refs;
  char crt_surface_state[64];
  double crt_runtime_exports;
  double crt_direct_refs;
  char crt_top_unreferenced_family[64];
  char crt_unreferenced_families[16384];
  char crt_next_action[64];
  char crt_next_reason[256];
  char crt_next_unreferenced_family[64];
  char crt_next_unreferenced_exports[16384];
  char crt_next_definition_file[4096];
  char crt_next_definition_locations[16384];
  char crt_next_inspect_command[4096];
  char run_command[1024];
  char active_primary_command[4096];
  char active_raw_repro_command[4096];
  char nynth_root[4096];
  char nytrix_root[4096];
  char ny_bin[4096];
  char tmp_dir[4096];
  char scratch_root[4096];
  char xdg_cache_home[4096];
  char nytrix_cache_dir[4096];
  char old_nytrix_test_scratch[4096];
  char old_nytrix_fuzz_dir[4096];
  char old_nytrix_build_cache_dir[4096];
  char active_old_nytrix_cache_writer[512];
  char active_old_nytrix_output_writer[512];
  char nynth_git_head[64];
  char nytrix_git_head[64];
  char nynth_git_status_hash[32];
  char nytrix_git_status_hash[32];
  char nynth_bin_hash[32];
  char ny_bin_hash[32];
  double reports;
  double ignored_no_evidence_reports;
  double ok_reports;
  double failed_reports;
  double attention_reports;
  double full_pressure_reports;
  double full_pressure_ok_reports;
  double full_pressure_attention_reports;
  double thread_hours;
  double thread_years;
  double full_pressure_thread_hours;
  double full_pressure_thread_years;
  double checked_subcases;
  double sub_failures_total;
  double active_items;
  double active_failure_detail_count;
  double active_saved_hangs;
  double active_saved_crashes;
  double active_saved_inputs;
  double active_repro_commands;
  double active_raw_repro_commands;
  double active_repro_ready;
  double non_reproducing_afl_timeouts;
  double historical_non_reproducing_afl_timeouts;
  double historical_attention_reports;
  double coverage_lanes;
  double coverage_ran_lanes;
  double coverage_skipped_lanes;
  double coverage_failed_lanes;
  double coverage_gaps;
  double coverage_blocker_gaps;
  double coverage_advisory_gaps;
  double coverage_latest_report_advisory_gaps;
  double coverage_latest_report_companion_skipped_lanes;
  double coverage_reports_considered;
  double coverage_campaign_reports_considered;
  double coverage_companion_reports_considered;
  double coverage_disabled_lanes;
  double coverage_budget_short_lanes;
  double coverage_missing_tool_lanes;
  double target_thread_years;
  double remaining_thread_years;
  double target_percent;
  double runs_needed;
  double wall_hours_needed;
  double thread_hours_needed;
  double wall_days_needed;
  double runs_per_day;
  double thread_years_per_day;
  double campaign_plan_wall_hours;
  char campaign_plan_threads[64];
  double campaign_first_report_epoch;
  double campaign_latest_report_epoch;
  double campaign_calendar_span_days;
  double campaign_calendar_age_days;
  double completion_eta_epoch;
  char completion_eta_local[64];
  double historical_finding_live;
  double historical_finding_missing;
  double historical_known_reproduced;
  double historical_known_lost;
  double historical_known_baseline;
  double historical_perf_hotspots;
  double historical_perf_max_ratio;
  char historical_perf_max_case[128];
  double perf_hotspots;
  double perf_max_ratio;
  char perf_max_case[128];
  double latest_failure_count;
  double latest_sub_failures;
  double latest_finding_live;
  double latest_finding_missing;
  double latest_known_reproduced;
  double latest_known_lost;
  double latest_known_baseline;
  double latest_perf_hotspots;
  double latest_perf_max_ratio;
  char latest_perf_max_case[128];
  double latest_full_pressure_failure_count;
  double latest_full_pressure_sub_failures;
  double latest_full_pressure_finding_live;
  double latest_full_pressure_finding_missing;
  double latest_full_pressure_known_reproduced;
  double latest_full_pressure_known_lost;
  double latest_full_pressure_known_baseline;
  double latest_full_pressure_perf_hotspots;
  double latest_full_pressure_perf_max_ratio;
  char latest_full_pressure_perf_max_case[128];
  bool latest_report_demoted_non_reproducing_afl_timeout;
  bool latest_full_pressure_demoted_non_reproducing_afl_timeout;
  char latest_full_pressure_clean_reason[128];
  double current_perf_cases;
  double latest_full_pressure_perf_rows;
  double latest_only_non_reproducing_afl_timeout;
  double runtime_export_coverage_percent;
  double runtime_unreferenced_percent;
  double runtime_unreferenced_count;
  double runtime_wrapper_gap_count;
  double crt_export_coverage_percent;
  double crt_unreferenced_percent;
  double crt_unreferenced_count;
  double crt_wrapper_gap_count;
  double crt_unreferenced_family_count;
  double crt_top_unreferenced_family_count;
  double crt_next_unreferenced_count;
  int coverage_detail_count;
  int coverage_backlog_lanes;
  int coverage_detail_rows;
  int coverage_queue_count;
  int coverage_queue_non_advisory_count;
  int coverage_queue_advisory_count;
  char coverage_queue_lanes[2048];
  char coverage_next_action[64];
  char coverage_next_category[64];
  char coverage_next_severity[64];
  char coverage_next_lane[128];
  char coverage_next_reason[512];
  char coverage_next_command[4096];
  char coverage_next_guarded_command[4096];
  char coverage_next_low_cpu_command[4096];
  char coverage_next_preview_command[4096];
  char coverage_next_state_file[4096];
  char coverage_next_state_command[4096];
  char coverage_next_state_refresh_command[4096];
  bool coverage_next_state_refresh_required;
  char coverage_next_state_refresh_reason[64];
  char recommended_state_refresh_command[4096];
  char coverage_next_stop_file[4096];
  char coverage_next_stop_command[4096];
  char coverage_next_resume_command[4096];
  bool history_readable;
  bool worklist_readable;
  bool coverage_readable;
  bool plan_readable;
  bool active_clear;
  bool full_pressure_ready;
  bool long_run_ready;
  bool target_reached;
  bool campaign_complete;
  bool ny_bin_exists;
  bool cache_policy_ok;
  bool old_path_cache_policy_ok;
  bool old_nytrix_test_scratch_absent;
  bool old_nytrix_fuzz_absent;
  bool old_nytrix_build_cache_absent;
  bool active_old_nytrix_cache_writer_present;
  bool active_old_nytrix_output_writer_present;
  bool nynth_git_ok;
  bool nynth_git_dirty;
  bool nytrix_git_ok;
  bool nytrix_git_dirty;
  bool latest_report_ok;
  bool latest_report_attention;
  bool latest_report_clean;
  bool latest_full_pressure_ok;
  bool latest_full_pressure_attention;
  bool latest_full_pressure_clean;
  bool latest_full_pressure_perf_suite_current;
  bool strict;
  bool allow_incomplete_coverage;
  bool allow_full_pressure_remediation;
  bool refreshed;
  int blocker_count;
  double old_path_artifact_leak_count;
  double old_path_artifact_moved_count;
  double old_path_artifact_remaining_count;
  double old_path_present_count;
  double old_path_moved_count;
  double old_path_remaining_count;
  double old_path_wait_remaining_seconds;
} fuzz_all_status_summary_t;

typedef struct fuzz_all_run_state_summary_t fuzz_all_run_state_summary_t;

typedef struct {
  char command[4096];
  double raw_repro_checked;
  double raw_repro_passed;
  double raw_repro_timeouts;
  double raw_repro_unexpected;
  double saved_hangs;
  double saved_inputs;
  bool found;
} fuzz_all_advisory_recheck_summary_t;

static const char *fuzz_all_state_child_status(double pid_value,
                                               bool child_alive);
static const char *fuzz_all_state_label_values(bool readable,
                                               const char *phase);
static bool fuzz_all_state_phase_live(const char *phase);
static bool fuzz_all_state_fresh_values(bool readable, const char *phase,
                                        double age_seconds,
                                        double heartbeat_s,
                                        double child_pid,
                                        bool child_alive);
static const char *fuzz_all_state_stale_reason_values(bool readable,
                                                      bool fresh,
                                                      const char *phase,
                                                      double age_seconds,
                                                      double heartbeat_s,
                                                      double child_pid,
                                                      bool child_alive);
static void status_capture_provenance(fuzz_all_status_summary_t *s,
                                      const char *root);
static double fuzz_all_score_clamp(double v, double lo, double hi);
static double fuzz_all_score_good_threshold(void);
static double fuzz_all_score_latest_fresh_hours(void);
static double fuzz_all_score_full_pressure_fresh_hours(void);
static const char *fuzz_all_score_label(double score);
static bool fuzz_all_score_age_fresh(double age_seconds,
                                     double stale_after_hours);
static double fuzz_all_score_report_age_seconds(const char *root,
                                                const char *path);
static double fuzz_all_score_signal_score(bool ready, double blocker_count,
                                          double active_items,
                                          double coverage_blocker_gaps,
                                          double compiler_live,
                                          double compiler_missing,
                                          double known_reproduced,
                                          double known_lost,
                                          double known_baseline,
                                          double perf_hotspots,
                                          double non_reproducing_timeouts,
                                          bool cache_policy_ok,
                                          bool ny_bin_exists);
static double fuzz_all_score_evidence_cap(double target_percent,
                                          bool campaign_complete);
static double fuzz_all_runs_to_good(double target_percent,
                                    double target_percent_per_run,
                                    double signal_score,
                                    double stability_score,
                                    bool campaign_complete);
static double fuzz_all_runs_to_days(double runs, double runs_per_day);
static double fuzz_all_language_good_gap_percent(double score);
static void append_fuzz_all_run_state_fields_prefixed(
    str_buf_t *b, const char *prefix, const fuzz_all_run_state_summary_t *st);
static void append_fuzz_all_state_summary_markdown(
    str_buf_t *md, const char *label, const char *inspect_command,
    const char *refresh_command,
    const fuzz_all_run_state_summary_t *st);
static double fuzz_all_state_stale_after_seconds(double heartbeat_s);

typedef struct {
  triage_item_t *items;
  int count;
  int cap;
} triage_item_list_t;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} byte_buf_t;

typedef struct {
  string_list_t inputs;
  char *expected;
} creal_io_sample_t;

typedef struct {
  creal_io_sample_t *items;
  int count;
  int cap;
} creal_io_sample_list_t;

typedef struct {
  char *function_name;
  char *parameter_types;
  char *return_type;
  char *function_source;
  char *io_list;
  char *misc;
  char *src_file;
  char *include_headers;
  char *include_sources;
  char *raw_json;
} creal_record_t;

typedef struct {
  bool ok;
  bool compile_ok;
  bool run_ok;
  bool output_ok;
  proc_result_t compile;
  run_many_result_t run;
  char *exe_path;
  double worker_ms;
} creal_exec_result_t;

typedef struct {
  const char *root;
  const char *ny_bin;
  const char *mode;
  const char *expect_substring;
  const char *flavor;
  double timeout_s;
  char *tmp_source;
  int checks;
  int max_checks;
} reducer_context_t;

typedef struct {
  string_list_t python_files;
  string_list_t pycache_dirs;
  string_list_t legacy_refs;
  int files_scanned;
} tree_clean_audit_t;

static int json_failures_nonempty(const char *json);
static bool exists_path(const char *path);
static bool mkdir_parent(const char *path);
static bool write_file_text(const char *path, const char *data);
static bool write_file_bytes(const char *path, const unsigned char *data, size_t len);
static void print_tail_text(FILE *out, const char *s, size_t limit);
static const char *find_manifest_to_read(const char *corpus_dir, char **manifest_path,
                                         char **legacy_manifest_path);
static char *make_failed_command_report(const char *lane, const char *phase, int rc,
                                        const char *out, const char *err);
static bool summarize_known_bug_report(const char *json, int *known_bug_count,
                                       int *reproduced, int *fixed_candidates,
                                       int *lost_signal, int *baseline_failures);
static bool summarize_perf_triage_report(const char *json, int *hotspots,
                                         double *max_ratio, char *max_case,
                                         size_t max_case_sz);
static int cmd_public_fuzz_all_preflight(int argc, char **argv);
static int cmd_public_fuzz_all_findings(int argc, char **argv);
static int cmd_public_fuzz_all_history(int argc, char **argv);
static int cmd_public_fuzz_all_worklist(int argc, char **argv);
static int cmd_public_fuzz_all_plan(int argc, char **argv);
static int cmd_public_fuzz_all_coverage(int argc, char **argv);
static int cmd_public_fuzz_all_status(int argc, char **argv);
static int cmd_public_fuzz_all_progress(int argc, char **argv);
static int cmd_public_fuzz_all_old_paths(int argc, char **argv);
static void fuzz_all_advisory_recheck_summary_from_worklist(
    const char *worklist_json, const char *preferred_report,
    fuzz_all_advisory_recheck_summary_t *out);
static char *fuzz_all_advisory_action_command(const char *history_path,
                                              const char *worklist_history_path,
                                              const char *worklist_history_md_path);
static char *fuzz_all_perf_watchlist_command(const char *dir_path);
static char *fuzz_all_preview_command(const char *next_command);
static void fuzz_all_env_command(char *out, size_t out_size,
                                 const char *assignments,
                                 const char *command);
static char *fuzz_all_low_priority_command_dup(const char *command);
static void fuzz_all_gentle_run_command(char *out, size_t out_size,
                                        const char *command);
static char *path_with_suffix_ext(const char *path, const char *suffix, const char *ext);
static void append_tail_json_str(str_buf_t *b, const char *s, size_t limit);
static void safe_stem(char *out, size_t out_sz, const char *raw);
static void append_proc_tail_fields(str_buf_t *b, const proc_result_t *pr);
static char *make_native_proc_failure_row(const char *case_name, const char *phase,
                                          const proc_result_t *pr);
static char *build_native_report_json(const string_list_t *rows, const string_list_t *failures,
                                      const char *mode, const char *summary_extra);
static char *build_native_report_json_with_top_aliases(
    const string_list_t *rows, const string_list_t *failures,
    const char *mode, const char *summary_extra, bool top_aliases);
static int emit_native_report(char *report_json, const char *json_path,
                              const char *label, int rows, int failures);
static char *native_row_status(const char *name, const char *kind, bool ok,
                               const char *detail_key, const char *detail_value);
static bool contains_ci(const char *haystack, const char *needle);
static char *json_string_or_empty_range_local(const char *start, const char *end, const char *key);
static bool compile_or_run_ny_source(const char *root, const char *ny_bin, const char *path,
                                     bool run, double timeout_s, proc_result_t *out);
static bool entry_is_real_db(const char *entry_json);
static char *make_real_db_replay_row(const char *root, const char *ny_bin, const char *entry_id,
                                     const char *entry_json, const char *corpus_dir,
                                     const char *ny_override, double timeout_s);
static bool entry_is_creal_db(const char *entry_json);
static const char *creal_features_json(void);
static char *make_creal_db_replay_row(const char *root, const char *entry_id,
                                      const char *entry_json, const char *corpus_dir,
                                      double timeout_s);
static int cmd_public_synth_print(int argc, char **argv);
static int cmd_public_synth_creal(int argc, char **argv);
static int cmd_public_compiler_findings(int argc, char **argv);
static int cmd_public_compiler_known_bugs(int argc, char **argv);
static int cmd_public_compiler_std_audit(int argc, char **argv);
static bool find_nynth_root(char *out, size_t out_sz);
static int nynth_asprintf(char **out, const char *rel_fmt, ...);

static char *strndup_local(const char *s, size_t n) {
  char *out = (char *)malloc(n + 1u);
  if (!out) return NULL;
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static char *trim_trailing_copy(const char *s) {
  if (!s) return strdup("");
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) --n;
  return strndup_local(s, n);
}

static const char *skip_ws_const(const char *p) {
  while (p && *p && isspace((unsigned char)*p)) ++p;
  return p;
}

static const char *find_n(const char *start, const char *end, const char *needle) {
  size_t n = strlen(needle);
  if (!n || !start || !end || start > end) return NULL;
  for (const char *p = start; p + n <= end; ++p) {
    if (memcmp(p, needle, n) == 0) return p;
  }
  return NULL;
}

static const char *json_value_after_key_range(const char *start, const char *end, const char *key) {
  char pat[128];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  size_t pat_len = strlen(pat);
  const char *p = start;
  while ((p = find_n(p, end, pat)) != NULL) {
    const char *after = p + pat_len;
    while (after < end && isspace((unsigned char)*after)) ++after;
    if (after < end && *after == ':') return skip_ws_const(after + 1);
    p += pat_len;
  }
  return NULL;
}

static const char *json_value_after_key(const char *json, const char *key) {
  return json_value_after_key_range(json, json + strlen(json), key);
}

static const char *json_top_level_value_after_key(const char *json, const char *key) {
  const char *p = skip_ws_const(json);
  if (!p || *p != '{' || !key) return json_value_after_key(json ? json : "", key ? key : "");
  const size_t key_len = strlen(key);
  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (; *p; ++p) {
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (*p == '\\') {
        escape = true;
      } else if (*p == '"') {
        in_string = false;
      }
      continue;
    }
    if (*p == '"') {
      if (depth == 1) {
        const char *start = p + 1;
        const char *q = start;
        bool key_escape = false;
        bool local_escape = false;
        for (; *q; ++q) {
          if (local_escape) {
            key_escape = true;
            local_escape = false;
          } else if (*q == '\\') {
            local_escape = true;
          } else if (*q == '"') {
            break;
          }
        }
        if (!*q) return NULL;
        const char *after = skip_ws_const(q + 1);
        if (!key_escape && after && *after == ':' &&
            (size_t)(q - start) == key_len && memcmp(start, key, key_len) == 0) {
          return skip_ws_const(after + 1);
        }
        p = q;
        continue;
      }
      in_string = true;
    } else if (*p == '{' || *p == '[') {
      ++depth;
    } else if ((*p == '}' || *p == ']') && depth > 0) {
      --depth;
    }
  }
  return NULL;
}

static const char *matching_json_end(const char *open, char lhs, char rhs) {
  bool in_string = false;
  bool escape = false;
  int depth = 0;
  for (const char *p = open; p && *p; ++p) {
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (*p == '\\') {
        escape = true;
      } else if (*p == '"') {
        in_string = false;
      }
      continue;
    }
    if (*p == '"') {
      in_string = true;
    } else if (*p == lhs) {
      ++depth;
    } else if (*p == rhs) {
      --depth;
      if (depth == 0) return p;
    }
  }
  return NULL;
}

static char *parse_json_string_dup(const char **cursor, const char *end) {
  const char *p = *cursor;
  if (!p || p >= end || *p != '"') return NULL;
  ++p;
  str_buf_t b = {0};
  while (p < end && *p) {
    unsigned char c = (unsigned char)*p++;
    if (c == '"') {
      *cursor = p;
      return sb_take(&b);
    }
    if (c == '\\' && p < end) {
      unsigned char e = (unsigned char)*p++;
      switch (e) {
      case '"': c = '"'; break;
      case '\\': c = '\\'; break;
      case '/': c = '/'; break;
      case 'b': c = '\b'; break;
      case 'f': c = '\f'; break;
      case 'n': c = '\n'; break;
      case 'r': c = '\r'; break;
      case 't': c = '\t'; break;
      case 'u':

        if (p + 4 <= end) p += 4;
        c = '?';
        break;
      default:
        c = e;
        break;
      }
    }
    if (!sb_append_c(&b, (char)c)) {
      free(b.data);
      return NULL;
    }
  }
  free(b.data);
  return NULL;
}

static char *json_extract_string_range(const char *start, const char *end, const char *key) {
  const char *p = json_value_after_key_range(start, end, key);
  if (!p || p >= end || *p != '"') return NULL;
  return parse_json_string_dup(&p, end);
}

static char *json_extract_array_range(const char *start, const char *end, const char *key) {
  const char *p = json_value_after_key_range(start, end, key);
  if (!p || p >= end || *p != '[') return NULL;
  const char *q = matching_json_end(p, '[', ']');
  if (!q || q >= end) return NULL;
  return strndup_local(p, (size_t)(q - p + 1));
}

static char *features_csv_from_json_array(const char *array_text) {
  if (!array_text || array_text[0] != '[') return strdup("");
  const char *end = matching_json_end(array_text, '[', ']');
  if (!end) return strdup("");
  const char *p = array_text + 1;
  str_buf_t out = {0};
  while (p < end) {
    p = skip_ws_const(p);
    if (p >= end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '"') break;
    char *item = parse_json_string_dup(&p, end);
    if (!item) break;
    if (out.len) (void)sb_append_c(&out, ',');
    (void)sb_append(&out, item);
    free(item);
  }
  return sb_take(&out);
}

static bool string_list_push_take(string_list_t *list, char *item) {
  if (list->count == list->cap) {
    int next_cap = list->cap ? list->cap * 2 : 16;
    char **next = (char **)realloc(list->items, (size_t)next_cap * sizeof(char *));
    if (!next) return false;
    list->items = next;
    list->cap = next_cap;
  }
  list->items[list->count++] = item;
  return true;
}

static bool string_list_push_copy(string_list_t *list, const char *item) {
  char *copy = strdup(item ? item : "");
  if (!copy) return false;
  if (!string_list_push_take(list, copy)) {
    free(copy);
    return false;
  }
  return true;
}

static void string_list_move_last_to_front(string_list_t *list) {
  if (!list || list->count < 2) return;
  char *last = list->items[list->count - 1];
  memmove(&list->items[1], &list->items[0],
          (size_t)(list->count - 1) * sizeof(list->items[0]));
  list->items[0] = last;
}

static void string_list_free(string_list_t *list) {
  for (int i = 0; i < list->count; ++i) free(list->items[i]);
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static bool string_list_contains(const string_list_t *list, const char *item) {
  for (int i = 0; i < list->count; ++i) {
    if (strcmp(list->items[i], item) == 0) return true;
  }
  return false;
}

static bool string_list_push_unique_copy(string_list_t *list, const char *item) {
  if (!item || !*item || string_list_contains(list, item)) return true;
  return string_list_push_copy(list, item);
}

static void string_list_push_csv_unique(string_list_t *list, const char *csv) {
  if (!csv) return;
  const char *start = csv;
  for (const char *p = csv; ; ++p) {
    if (*p != ',' && *p != '\0') continue;
    size_t n = (size_t)(p - start);
    while (n && isspace((unsigned char)*start)) {
      ++start;
      --n;
    }
    while (n && isspace((unsigned char)start[n - 1])) --n;
    if (n) {
      char *item = strndup_local(start, n);
      if (item) {
        (void)string_list_push_unique_copy(list, item);
        free(item);
      }
    }
    if (*p == '\0') break;
    start = p + 1;
  }
}

static int cmp_cstr(const void *a, const void *b) {
  const char *const *x = (const char *const *)a;
  const char *const *y = (const char *const *)b;
  return strcmp(*x, *y);
}

static void ratio_stats_add(ratio_stats_t *st, const char *case_name, double value) {
  if (st->count == 0 || value < st->min) {
    st->min = value;
    snprintf(st->min_case, sizeof(st->min_case), "%s", case_name ? case_name : "");
  }
  if (st->count == 0 || value > st->max) {
    st->max = value;
    snprintf(st->max_case, sizeof(st->max_case), "%s", case_name ? case_name : "");
  }
  st->sum += value;
  st->count++;
  if (value <= 1.0) st->faster_or_equal++;
}

static bool extract_json_number(const char *json, const char *key, double *out) {
  const char *p = json_value_after_key(json, key);
  if (!p) return false;
  char *end = NULL;
  double v = strtod(p, &end);
  if (end == p) return false;
  *out = v;
  return true;
}

static int append_failures_from_row(str_buf_t *failures_json, const char *row) {
  const char *p = json_value_after_key(row, "failures");
  if (!p || *p != '[') return 0;
  const char *q = matching_json_end(p, '[', ']');
  if (!q) return 0;
  const char *content = skip_ws_const(p + 1);
  const char *tail = q;
  while (tail > content && isspace((unsigned char)tail[-1])) --tail;
  if (content >= tail) return 0;
  int count = 0;
  bool in_string = false;
  bool escape = false;
  int depth = 0;
  for (const char *r = content; r < tail; ++r) {
    if (in_string) {
      if (escape) escape = false;
      else if (*r == '\\') escape = true;
      else if (*r == '"') in_string = false;
      continue;
    }
    if (*r == '"') in_string = true;
    else if (*r == '{') {
      if (depth == 0) ++count;
      ++depth;
    } else if (*r == '}') {
      if (depth > 0) --depth;
    }
  }
  if (count <= 0) count = 1;
  if (failures_json->len) (void)sb_append_c(failures_json, ',');
  (void)sb_append_n(failures_json, content, (size_t)(tail - content));
  return count;
}

static char *extract_case_name_from_row(const char *row) {
  const char *end = row + strlen(row);
  char *name = json_extract_string_range(row, end, "case");
  return name ? name : strdup("");
}

static bool text_mentions_crash(const char *a, const char *b) {
  const char *texts[2] = {a, b};
  for (int i = 0; i < 2; ++i) {
    const char *s = texts[i];
    if (!s) continue;
    if (contains_ci(s, "segmentation fault") ||
        contains_ci(s, "segmentationfault") ||
        contains_ci(s, "signal 11") ||
        contains_ci(s, "sigsegv") ||
        contains_ci(s, "internal compiler error") ||
        contains_ci(s, "addresssanitizer") ||
        contains_ci(s, "undefinedbehavior") ||
        contains_ci(s, "assertion") ||
        contains_ci(s, "panic"))
      return true;
  }
  return false;
}

static const char *worker_failure_kind(const char *phase, int rc,
                                       const char *out, const char *err) {
  if (rc == 124) return "timeout";
  if (phase && contains_ci(phase, "diff")) return "output_diff";
  if (phase && contains_ci(phase, "prepare")) return "prepare_error";
  bool crash = rc >= 128 || text_mentions_crash(err, out);
  if (phase && contains_ci(phase, "compile"))
    return crash ? "compiler_crash" : "compile_error";
  if (phase && contains_ci(phase, "run")) return "runtime_error";
  return crash ? "compiler_crash" : "prepare_error";
}

static char *make_worker_failure_row(const char *case_name, const char *phase, int rc,
                                     const char *out, const char *err) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"ok\":false,\"engine\":\"nynth_core\",\"case\":");
  (void)sb_append_json_str(&b, case_name ? case_name : "");
  (void)sb_append(&b, ",\"features\":[],\"variants\":[],\"ratios\":{},\"shape_counts\":{},\"ir_analysis\":{},\"failures\":[{\"failure_kind\":");
  (void)sb_append_json_str(&b, worker_failure_kind(phase, rc, out, err));
  (void)sb_append(&b, ",\"tool\":\"nynth_core\",\"phase\":");
  (void)sb_append_json_str(&b, phase ? phase : "worker");
  (void)sb_appendf(&b, ",\"engine\":\"nynth_core\",\"rc\":%d,\"reason\":", rc);
  (void)sb_append_json_str(&b, (err && *err) ? err : ((out && *out) ? out : "worker failed"));
  if (err && *err) {
    (void)sb_append(&b, ",\"stderr_tail\":");
    append_tail_json_str(&b, err, 2000);
    (void)sb_append(&b, ",\"stderr\":");
    (void)sb_append_json_str(&b, err);
  }
  if (out && *out) {
    (void)sb_append(&b, ",\"stdout_tail\":");
    append_tail_json_str(&b, out, 2000);
    (void)sb_append(&b, ",\"stdout\":");
    (void)sb_append_json_str(&b, out);
  }
  (void)sb_append(&b, "}]}");
  return sb_take(&b);
}

static char *row_with_id_field(const char *row, const char *id) {
  size_t n = strlen(row ? row : "");
  while (n > 0 && isspace((unsigned char)row[n - 1])) --n;
  if (n == 0 || row[n - 1] != '}') return strdup(row ? row : "");
  str_buf_t b = {0};
  (void)sb_append_n(&b, row, n - 1);
  (void)sb_append(&b, ",\"id\":");
  (void)sb_append_json_str(&b, id ? id : "");
  (void)sb_append_c(&b, '}');
  return sb_take(&b);
}

static char *row_with_lane_fields(const char *row, const char *lane, const char *source_kind) {
  size_t n = strlen(row ? row : "");
  while (n > 0 && isspace((unsigned char)row[n - 1])) --n;
  if (n == 0 || row[n - 1] != '}') return strdup(row ? row : "");
  str_buf_t b = {0};
  (void)sb_append_n(&b, row, n - 1);
  if (lane && *lane) {
    (void)sb_append(&b, ",\"lane\":");
    (void)sb_append_json_str(&b, lane);
  }
  if (source_kind && *source_kind) {
    (void)sb_append(&b, ",\"source_kind\":");
    (void)sb_append_json_str(&b, source_kind);
  }
  (void)sb_append_c(&b, '}');
  return sb_take(&b);
}

static void report_rows_free(report_rows_t *report) {
  string_list_free(&report->rows);
  free(report->failures_json.data);
  memset(report, 0, sizeof(*report));
}

static void report_add_row(report_rows_t *report, char *row) {
  char *case_name = extract_case_name_from_row(row);
  int failures = append_failures_from_row(&report->failures_json, row);
  if (failures > 0) {
    report->failure_count += failures;
    report->failed_rows++;
  } else {
    double ratio = 0.0;
    if (extract_json_number(row, "ny_o3i_vs_c_o3_run", &ratio))
      ratio_stats_add(&report->ny_o3i_run, case_name, ratio);
    if (extract_json_number(row, "ny_o3_vs_c_o3_run", &ratio))
      ratio_stats_add(&report->ny_o3_run, case_name, ratio);
  }
  (void)string_list_push_take(&report->rows, row);
  free(case_name);
}

static void report_add_row_unscored(report_rows_t *report, char *row) {
  char *case_name = extract_case_name_from_row(row);
  double ratio = 0.0;
  if (extract_json_number(row, "ny_o3i_vs_c_o3_run", &ratio))
    ratio_stats_add(&report->ny_o3i_run, case_name, ratio);
  if (extract_json_number(row, "ny_o3_vs_c_o3_run", &ratio))
    ratio_stats_add(&report->ny_o3_run, case_name, ratio);
  (void)string_list_push_take(&report->rows, row);
  free(case_name);
}

static int report_add_rows_from_report_json(report_rows_t *report, const char *json,
                                            const char *lane, const char *source_kind) {
  const char *rows = json_value_after_key(json, "rows");
  if (!rows || *rows != '[') return 0;
  const char *end = matching_json_end(rows, '[', ']');
  if (!end) return 0;
  const char *p = rows + 1;
  int added = 0;
  while (p < end) {
    p = skip_ws_const(p);
    if (p >= end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '{') break;
    const char *obj_end = matching_json_end(p, '{', '}');
    if (!obj_end || obj_end > end) break;
    char *row = strndup_local(p, (size_t)(obj_end - p + 1));
    char *tagged = row_with_lane_fields(row, lane, source_kind);
    free(row);
    if (strstr(tagged, "\"quarantined\":true"))
      report_add_row_unscored(report, tagged);
    else
      report_add_row(report, tagged);
    ++added;
    p = obj_end + 1;
  }
  return added;
}

static bool collect_files_with_suffix(const char *dir, const char *suffix, string_list_t *out) {
  DIR *d = opendir(dir);
  if (!d) return false;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    if (n <= 0 || (size_t)n >= sizeof(path)) continue;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
    if (ny_has_suffix(path, suffix)) (void)string_list_push_copy(out, path);
  }
  closedir(d);
  qsort(out->items, (size_t)out->count, sizeof(char *), cmp_cstr);
  return true;
}

static char *shape_source_block(const char *shape_path, const char *name) {
  return nynth_shape_source_block(shape_path, name);
}

static bool shape_file_contains(const char *shape_path, const char *needle) {
  file_buf_t f = {0};
  bool ok = shape_path && needle && read_file(shape_path, &f) && strstr(f.data, needle);
  free(f.data);
  return ok;
}

static bool materialize_shape_source_block(const char *shape_path, const char *name, const char *out_path) {
  char *source = shape_source_block(shape_path, name);
  bool ok = source && write_file_text(out_path, source);
  free(source);
  return ok;
}

static bool collect_baked_bridge_seeds(const char *root, char **seed_dir_out, string_list_t *seeds) {
  (void)root;
  char *shape_dir = NULL, *seed_dir = NULL;
  if (nynth_asprintf(&shape_dir, "shapes/tests") < 0 ||
      nynth_asprintf(&seed_dir, "build/bridge/baked-seeds") < 0) {
    free(shape_dir);
    free(seed_dir);
    return false;
  }
  string_list_t shapes = {0};
  bool ok = collect_files_with_suffix(shape_dir, ".nshape", &shapes) && mkdir_p(seed_dir);
  for (int i = 0; ok && i < shapes.count; ++i) {
    if (!shape_file_contains(shapes.items[i], "template raw-c-seed")) continue;
    char stem[160];
    stem_name(shapes.items[i], stem, sizeof(stem));
    char *out_path = NULL;
    if (asprintf(&out_path, "%s/%s.c", seed_dir, stem) < 0) {
      ok = false;
    } else if (!materialize_shape_source_block(shapes.items[i], "c", out_path)) {
      ok = false;
    } else {
      (void)string_list_push_take(seeds, out_path);
      out_path = NULL;
    }
    free(out_path);
  }
  if (ok && seeds->count <= 0) ok = false;
  if (ok) {
    qsort(seeds->items, (size_t)seeds->count, sizeof(char *), cmp_cstr);
    *seed_dir_out = seed_dir;
    seed_dir = NULL;
  }
  string_list_free(&shapes);
  free(shape_dir);
  free(seed_dir);
  return ok;
}

static bool should_skip_tree_clean_dir(const char *name) {
  return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static bool tree_clean_should_descend_dir(const char *path) {
  return !strstr(path, "/build") &&
         !strstr(path, "/fuzz/work");
}

static bool tree_clean_should_scan_contents(const char *path) {
  return !strstr(path, "/build/") &&
         !strstr(path, "/fuzz/work/");
}

static void tree_clean_scan_file(const char *path, tree_clean_audit_t *audit) {
  static const char *legacy_tokens[] = {
    "typed_" "generate", "corpus_" "db", "core_" "accel",
    "NYNTH_" "CORE_COMPARE", "NYNTH_" "CORE=0", "nynth" ".py",
    "python" "/nynth", "native-lane-" "unsupported", "native-" "skip"
  };
  file_buf_t f = {0};
  ++audit->files_scanned;
  if (!tree_clean_should_scan_contents(path)) return;
  if (!read_file(path, &f)) return;
  for (size_t i = 0; i < sizeof(legacy_tokens) / sizeof(legacy_tokens[0]); ++i) {
    const char *token = legacy_tokens[i];
    if (!strstr(f.data, token)) continue;
    char *hit = NULL;
    if (asprintf(&hit, "%s:%s", path, token) >= 0) {
      (void)string_list_push_take(&audit->legacy_refs, hit);
    }
  }
  free(f.data);
}

static bool tree_clean_walk(const char *dir, tree_clean_audit_t *audit) {
  DIR *d = opendir(dir);
  if (!d) return false;
  bool ok = true;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (should_skip_tree_clean_dir(ent->d_name)) continue;
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
      ok = false;
      continue;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
      ok = false;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (strcmp(ent->d_name, "__pycache__") == 0) {
        (void)string_list_push_copy(&audit->pycache_dirs, path);
        continue;
      }
      if (!tree_clean_should_descend_dir(path)) continue;
      if (!tree_clean_walk(path, audit)) ok = false;
    } else if (S_ISREG(st.st_mode)) {
      if (ny_has_suffix(ent->d_name, ".py")) {
        (void)string_list_push_copy(&audit->python_files, path);
      }
      tree_clean_scan_file(path, audit);
    }
  }
  closedir(d);
  return ok;
}

static void init_self_path(const char *argv0) {
  ssize_t n = readlink("/proc/self/exe", g_self_path, sizeof(g_self_path) - 1u);
  if (n > 0) {
    g_self_path[n] = '\0';
    return;
  }
  if (argv0 && argv0[0] == '/') {
    snprintf(g_self_path, sizeof(g_self_path), "%s", argv0);
    return;
  }
  if (argv0 && strchr(argv0, '/')) {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) {
      size_t cwd_len = strlen(cwd);
      size_t arg_len = strlen(argv0);
      if (cwd_len + 1u + arg_len < sizeof(g_self_path)) {
        memcpy(g_self_path, cwd, cwd_len);
        g_self_path[cwd_len] = '/';
        memcpy(g_self_path + cwd_len + 1u, argv0, arg_len + 1u);
      } else {
        snprintf(g_self_path, sizeof(g_self_path), "%s", argv0);
      }
      return;
    }
  }
  snprintf(g_self_path, sizeof(g_self_path), "%s", argv0 && *argv0 ? argv0 : "nynth");
}

static double worker_outer_timeout(double timeout_s, int runs, int warmup) {
  if (timeout_s <= 0.0) return 0.0;
  int iterations = runs + warmup;
  if (iterations < 1) iterations = 1;
  return timeout_s * (double)(iterations * 6 + 8) + 30.0;
}

static void append_ratio_stats_json(str_buf_t *b, const char *key, const ratio_stats_t *st) {
  (void)sb_append_json_str(b, key);
  if (st->count <= 0) {
    (void)sb_append(b, ":{\"count\":0}");
    return;
  }
  (void)sb_appendf(b, ":{\"count\":%d,\"avg\":%.4f,\"min\":%.4f,\"min_case\":",
                   st->count, st->sum / (double)st->count, st->min);
  (void)sb_append_json_str(b, st->min_case);
  (void)sb_appendf(b, ",\"max\":%.4f,\"max_case\":", st->max);
  (void)sb_append_json_str(b, st->max_case);
  (void)sb_appendf(b, ",\"faster_or_equal_count\":%d}", st->faster_or_equal);
}

static void append_rows_json(str_buf_t *b, const string_list_t *rows) {
  (void)sb_append_c(b, '[');
  for (int i = 0; i < rows->count; ++i) {
    if (i) (void)sb_append_c(b, ',');
    (void)sb_append(b, rows->items[i]);
  }
  (void)sb_append_c(b, ']');
}

static void append_string_list_json(str_buf_t *b, const string_list_t *items) {
  (void)sb_append_c(b, '[');
  for (int i = 0; i < items->count; ++i) {
    if (i) (void)sb_append_c(b, ',');
    (void)sb_append_json_str(b, items->items[i]);
  }
  (void)sb_append_c(b, ']');
}

static void append_raw_json_list(str_buf_t *b, const string_list_t *items) {
  (void)sb_append_c(b, '[');
  for (int i = 0; i < items->count; ++i) {
    if (i) (void)sb_append_c(b, ',');
    (void)sb_append(b, items->items[i]);
  }
  (void)sb_append_c(b, ']');
}

static void append_worker_counts_json(str_buf_t *b, const worker_counts_t *counts) {
  (void)sb_appendf(b,
    "{\"native_compare\":%d,\"native_generation\":%d,"
    "\"native_replay\":%d,\"native_cbridge\":%d}",
    counts->native_compare, counts->native_generation,
    counts->native_replay, counts->native_cbridge);
}

static int extract_json_int_in_object(const char *json, const char *object_key, const char *key) {
  const char *obj = json_value_after_key(json, object_key);
  if (!obj || *obj != '{') return 0;
  const char *end = matching_json_end(obj, '{', '}');
  if (!end) return 0;
  const char *v = json_value_after_key_range(obj, end + 1, key);
  if (!v) return 0;
  char *num_end = NULL;
  long n = strtol(v, &num_end, 10);
  if (num_end == v) return 0;
  return (int)n;
}

static void worker_counts_add_from_report(worker_counts_t *counts, const char *json) {
  counts->native_compare += extract_json_int_in_object(json, "native_workers", "native_compare");
  counts->native_generation += extract_json_int_in_object(json, "native_workers", "native_generation");
  counts->native_replay += extract_json_int_in_object(json, "native_workers", "native_replay");
  counts->native_cbridge += extract_json_int_in_object(json, "native_workers", "native_cbridge");
}

static char *make_tmp_json_path(const char *root, const char *prefix, int seed, int idx) {
  const char *tmp_dir = getenv("TMPDIR");
  char *owned_tmp = NULL;
  if (!tmp_dir || !*tmp_dir) {
    char nynth_root[4096] = "";
    const char *cache_root = root;
    if (find_nynth_root(nynth_root, sizeof(nynth_root)) && nynth_root[0])
      cache_root = nynth_root;
    if (cache_root && *cache_root) {
      (void)asprintf(&owned_tmp, "%s/build/cache/tmp", cache_root);
      if (owned_tmp) {
        ny_ensure_dir_recursive(owned_tmp);
        tmp_dir = owned_tmp;
      }
    }
    if (!tmp_dir || !*tmp_dir) tmp_dir = "/tmp";
  }
  char *path = NULL;
  if (asprintf(&path, "%s/nynth_%s_%d_%d_%ld.json",
               tmp_dir, prefix, seed, idx, (long)getpid()) < 0) {
    free(owned_tmp);
    return NULL;
  }
  free(owned_tmp);
  return path;
}

static bool generated_case_list_push(generated_case_list_t *list, generated_case_t item) {
  if (list->count == list->cap) {
    int next_cap = list->cap ? list->cap * 2 : 16;
    generated_case_t *next = (generated_case_t *)realloc(list->items, (size_t)next_cap * sizeof(generated_case_t));
    if (!next) return false;
    list->items = next;
    list->cap = next_cap;
  }
  list->items[list->count++] = item;
  return true;
}

static void generated_case_item_free(generated_case_t *item) {
  if (!item) return;
  free(item->name);
  free(item->shape);
  free(item->family);
  free(item->generator);
  free(item->method);
  free(item->source_kind);
  free(item->c_path);
  free(item->ny_path);
  free(item->ir_path);
  free(item->features_csv);
  free(item->json);
  memset(item, 0, sizeof(*item));
}

static void generated_case_list_free(generated_case_list_t *list) {
  for (int i = 0; i < list->count; ++i) {
    generated_case_item_free(&list->items[i]);
  }
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static bool manifest_entry_list_push_take(manifest_entry_list_t *list, char *id, char *json) {
  if (list->count == list->cap) {
    int next_cap = list->cap ? list->cap * 2 : 16;
    manifest_entry_t *next = (manifest_entry_t *)realloc(list->items, (size_t)next_cap * sizeof(*next));
    if (!next) return false;
    list->items = next;
    list->cap = next_cap;
  }
  list->items[list->count].id = id;
  list->items[list->count].json = json;
  list->count++;
  return true;
}

static void manifest_entry_list_free(manifest_entry_list_t *list) {
  for (int i = 0; i < list->count; ++i) {
    free(list->items[i].id);
    free(list->items[i].json);
  }
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static int manifest_entry_cmp(const void *a, const void *b) {
  const manifest_entry_t *x = (const manifest_entry_t *)a;
  const manifest_entry_t *y = (const manifest_entry_t *)b;
  return strcmp(x->id ? x->id : "", y->id ? y->id : "");
}

static bool matrix_row_list_push_take(matrix_row_list_t *list, matrix_row_t item) {
  if (list->count == list->cap) {
    int next_cap = list->cap ? list->cap * 2 : 32;
    matrix_row_t *next = (matrix_row_t *)realloc(list->items, (size_t)next_cap * sizeof(*next));
    if (!next) return false;
    list->items = next;
    list->cap = next_cap;
  }
  list->items[list->count++] = item;
  return true;
}

static void matrix_row_list_free(matrix_row_list_t *list) {
  for (int i = 0; i < list->count; ++i) {
    free(list->items[i].case_name);
    free(list->items[i].shape);
    free(list->items[i].features_json);
    free(list->items[i].report);
  }
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static int matrix_row_cmp_ratio_asc(const void *a, const void *b) {
  const matrix_row_t *x = (const matrix_row_t *)a;
  const matrix_row_t *y = (const matrix_row_t *)b;
  return (x->ratio > y->ratio) - (x->ratio < y->ratio);
}

static int matrix_row_cmp_ratio_desc(const void *a, const void *b) {
  return -matrix_row_cmp_ratio_asc(a, b);
}

static bool parse_generated_cases(const char *json, generated_case_list_t *out) {
  const char *cases = json_value_after_key(json, "cases");
  if (!cases || *cases != '[') return false;
  const char *end = matching_json_end(cases, '[', ']');
  if (!end) return false;
  const char *p = cases + 1;
  while (p < end) {
    p = skip_ws_const(p);
    if (p >= end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '{') break;
    const char *obj_end = matching_json_end(p, '{', '}');
    if (!obj_end || obj_end > end) break;
    generated_case_t item;
    memset(&item, 0, sizeof(item));
    item.name = json_extract_string_range(p, obj_end + 1, "name");
    item.shape = json_extract_string_range(p, obj_end + 1, "shape");
    item.family = json_extract_string_range(p, obj_end + 1, "family");
    item.generator = json_extract_string_range(p, obj_end + 1, "generator");
    item.method = json_extract_string_range(p, obj_end + 1, "method");
    item.source_kind = json_extract_string_range(p, obj_end + 1, "source_kind");
    item.c_path = json_extract_string_range(p, obj_end + 1, "c_source");
    item.ny_path = json_extract_string_range(p, obj_end + 1, "ny_source");
    item.ir_path = json_extract_string_range(p, obj_end + 1, "nynth_ir");
    char *features = json_extract_array_range(p, obj_end + 1, "features");
    item.features_csv = features_csv_from_json_array(features);
    free(features);
    item.json = strndup_local(p, (size_t)(obj_end - p + 1));
    if (item.name && item.c_path && item.ny_path && item.ir_path && item.json) {
      if (!generated_case_list_push(out, item)) {
        generated_case_item_free(&item);
        return false;
      }
    } else {
      generated_case_item_free(&item);
    }
    p = obj_end + 1;
  }
  return out->count > 0;
}

static bool parse_manifest_entries_raw(const char *json, manifest_entry_list_t *entries) {
  const char *items = json_value_after_key(json, "entries");
  if (!items || *items != '[') return true;
  const char *end = matching_json_end(items, '[', ']');
  if (!end) return false;
  const char *p = items + 1;
  while (p < end) {
    p = skip_ws_const(p);
    if (p >= end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '{') break;
    const char *obj_end = matching_json_end(p, '{', '}');
    if (!obj_end || obj_end > end) break;
    char *id = json_extract_string_range(p, obj_end + 1, "id");
    char *raw = strndup_local(p, (size_t)(obj_end - p + 1));
    if (id && raw && *id) {
      if (!manifest_entry_list_push_take(entries, id, raw)) {
        free(id);
        free(raw);
        return false;
      }
    } else {
      free(id);
      free(raw);
    }
    p = obj_end + 1;
  }
  return true;
}

static bool collect_rows_from_report_json(const char *json, string_list_t *rows) {
  const char *items = json_value_after_key(json, "rows");
  if (!items || *items != '[') return false;
  const char *end = matching_json_end(items, '[', ']');
  if (!end) return false;
  const char *p = items + 1;
  while (p < end) {
    p = skip_ws_const(p);
    if (p >= end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '{') break;
    const char *obj_end = matching_json_end(p, '{', '}');
    if (!obj_end || obj_end > end) break;
    char *raw = strndup_local(p, (size_t)(obj_end - p + 1));
    if (raw) (void)string_list_push_take(rows, raw);
    p = obj_end + 1;
  }
  return true;
}

static int count_json_array_items(const char *array_text) {
  if (!array_text || *array_text != '[') return -1;
  const char *end = matching_json_end(array_text, '[', ']');
  if (!end) return -1;
  const char *p = skip_ws_const(array_text + 1);
  if (p >= end) return 0;
  int count = 0;
  bool in_string = false;
  bool escape = false;
  int obj_depth = 0;
  int arr_depth = 0;
  bool has_token = false;
  for (; p < end; ++p) {
    if (in_string) {
      has_token = true;
      if (escape) escape = false;
      else if (*p == '\\') escape = true;
      else if (*p == '"') in_string = false;
      continue;
    }
    if (*p == '"') {
      in_string = true;
      has_token = true;
    } else if (*p == '{') {
      ++obj_depth;
      has_token = true;
    } else if (*p == '}') {
      if (obj_depth > 0) --obj_depth;
    } else if (*p == '[') {
      ++arr_depth;
      has_token = true;
    } else if (*p == ']') {
      if (arr_depth > 0) --arr_depth;
    } else if (*p == ',' && obj_depth == 0 && arr_depth == 0) {
      if (has_token) ++count;
      has_token = false;
    } else if (!isspace((unsigned char)*p)) {
      has_token = true;
    }
  }
  if (has_token) ++count;
  return count;
}

static char *first_row_or_object_from_json(const char *json) {
  string_list_t rows = {0};
  if (collect_rows_from_report_json(json, &rows) && rows.count > 0) {
    char *out = strdup(rows.items[0]);
    string_list_free(&rows);
    return out;
  }
  string_list_free(&rows);
  const char *p = skip_ws_const(json);
  if (!p || *p != '{') return NULL;
  const char *end = matching_json_end(p, '{', '}');
  if (!end) return NULL;
  return strndup_local(p, (size_t)(end - p + 1));
}

static bool copy_file_bytes(const char *src, const char *dst) {
  file_buf_t f = {0};
  if (!read_file(src, &f)) return false;
  if (!mkdir_parent(dst)) {
    free(f.data);
    return false;
  }
  FILE *out = fopen(dst, "wb");
  if (!out) {
    free(f.data);
    return false;
  }
  bool ok = fwrite(f.data, 1, f.len, out) == f.len;
  if (fclose(out) != 0) ok = false;
  free(f.data);
  return ok;
}

static void file_hash_hex(const char *path, char *out, size_t out_sz) {
  if (out_sz) out[0] = '\0';
  if (!path || !*path || !out_sz) return;
  file_buf_t f = {0};
  if (!read_file(path, &f)) return;
  snprintf(out, out_sz, "%016" PRIx64, fnv1a64(f.data, f.len));
  free(f.data);
}

static char *json_string_or_empty(const char *row, const char *key) {
  const char *end = row + strlen(row);
  char *value = json_extract_string_range(row, end, key);
  return value ? value : strdup("");
}

static char *json_array_or_empty(const char *row, const char *key) {
  const char *end = row + strlen(row);
  char *value = json_extract_array_range(row, end, key);
  return value ? value : strdup("[]");
}

static char *json_method_or_generator(const char *row) {
  char *method = json_string_or_empty(row, "method");
  if (method && *method) return method;
  free(method);
  method = json_string_or_empty(row, "generator_kind");
  if (method && *method) return method;
  free(method);
  method = json_string_or_empty(row, "generator");
  if (method && *method) return method;
  free(method);
  method = json_string_or_empty(row, "lane");
  if (method && *method) return method;
  free(method);
  return strdup("native");
}

static const char *canonical_native_method(const char *generator) {
  if (!generator || !*generator || strcmp(generator, "mixed") == 0 ||
      strcmp(generator, "auto") == 0)
    return "mixed";
  if (strcmp(generator, "generate") == 0 || strcmp(generator, "typed") == 0) return "typed";
  if (strcmp(generator, "ir") == 0) return "ir";
  if (strcmp(generator, "stress") == 0) return "stress";
  return generator;
}

static const char *canonical_synth_schedule(const char *schedule) {
  if (!schedule || !*schedule) return "smart";
  if (strcmp(schedule, "smart") == 0 ||
      strcmp(schedule, "coverage") == 0 ||
      strcmp(schedule, "ranked") == 0 ||
      strcmp(schedule, "weighted") == 0)
    return schedule;
  return "smart";
}

static const char *default_generated_leaf_for_method(const char *method) {
  if (method && strcmp(method, "mixed") == 0) return "build/generated/mixed";
  if (method && strcmp(method, "ir") == 0) return "build/generated/ir";
  if (method && strcmp(method, "optimizer") == 0) return "build/generated/optimizer";
  if (method && strcmp(method, "stress") == 0) return "build/generated/stress";
  if (method && strcmp(method, "real-db") == 0) return "build/generated/nyreal_native";
  return "build/generated/typed";
}

static bool path_exists_file(const char *path) {
  struct stat st;
  return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool file_newer_than(const char *path, const char *other) {
  struct stat a, b;
  if (!path || !other || stat(path, &a) != 0 || stat(other, &b) != 0) return false;
  return a.st_mtime > b.st_mtime;
}

static bool path_is_absolute(const char *path) {
  return path && path[0] == '/';
}

static char *resolve_existing_file(const char *root, const char *path) {
  if (!path || !*path) return NULL;
  if (path_exists_file(path)) return strdup(path);
  if (!path_is_absolute(path) && root && *root) {
    char *joined = NULL;
    if (asprintf(&joined, "%s/%s", root, path) >= 0 && joined) {
      if (path_exists_file(joined)) return joined;
      free(joined);
    }
  }
  return NULL;
}

static bool extract_prefixed_field(const char *text, const char *prefix, char *out, size_t out_sz) {
  if (!out_sz) return false;
  out[0] = '\0';
  if (!text || !prefix) return false;
  size_t prefix_len = strlen(prefix);
  const char *line = text;
  while (*line) {
    const char *end = strchr(line, '\n');
    size_t line_len = end ? (size_t)(end - line) : strlen(line);
    const char *p = line;
    while (line_len && isspace((unsigned char)*p)) {
      ++p;
      --line_len;
    }
    if (line_len >= prefix_len && memcmp(p, prefix, prefix_len) == 0) {
      p += prefix_len;
      line_len -= prefix_len;
      while (line_len && isspace((unsigned char)*p)) {
        ++p;
        --line_len;
      }
      while (line_len && isspace((unsigned char)p[line_len - 1])) --line_len;
      size_t n = line_len < out_sz - 1u ? line_len : out_sz - 1u;
      memcpy(out, p, n);
      out[n] = '\0';
      return n > 0;
    }
    if (!end) break;
    line = end + 1;
  }
  return false;
}

static int cmp_double_value(const void *a, const void *b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  return (x > y) - (x < y);
}

static double median_double(double *values, int count) {
  if (!values || count <= 0) return 0.0;
  qsort(values, (size_t)count, sizeof(double), cmp_double_value);
  if (count & 1) return values[count / 2];
  return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

typedef struct {
  bool ok;
  int rc;
  bool timed_out;
  char checksum[128];
  double median_elapsed_ns;
  double process_median_ms;
  double median_instructions;
  double median_cycles;
  double median_branches;
  double median_branch_misses;
  char *out;
  char *err;
} perf_run_result_t;

static void perf_run_result_free(perf_run_result_t *r) {
  if (!r) return;
  free(r->out);
  free(r->err);
  memset(r, 0, sizeof(*r));
}

static bool extract_perf_stat_value(const char *text, const char *event, double *out) {
  if (!text || !event || !out) return false;
  const char *p = text;
  while ((p = strstr(p, event))) {
    const char *line_start = p;
    while (line_start > text && line_start[-1] != '\n') --line_start;
    char val_str[64];
    const char *comma = strchr(line_start, ',');
    if (comma && (size_t)(comma - line_start) < sizeof(val_str)) {
      memcpy(val_str, line_start, (size_t)(comma - line_start));
      val_str[comma - line_start] = '\0';
      *out = atof(val_str);
      return true;
    }
    p += strlen(event);
  }
  return false;
}

static perf_run_result_t run_perf_executable(const char *root, const char *path,
                                             int runs, int warmup, double timeout_s) {
  perf_run_result_t result;
  memset(&result, 0, sizeof(result));
  result.rc = 127;
  runs = runs < 1 ? 1 : runs;
  warmup = warmup < 0 ? 0 : warmup;
  double *elapsed_ns = (double *)calloc((size_t)runs, sizeof(double));
  double *process_ms = (double *)calloc((size_t)runs, sizeof(double));
  double *instructions = (double *)calloc((size_t)runs, sizeof(double));
  double *cycles = (double *)calloc((size_t)runs, sizeof(double));
  double *branches = (double *)calloc((size_t)runs, sizeof(double));
  double *misses = (double *)calloc((size_t)runs, sizeof(double));
  if (!elapsed_ns || !process_ms || !instructions || !cycles || !branches || !misses) {
    result.err = strdup("allocation failed");
    free(elapsed_ns); free(process_ms);
    free(instructions); free(cycles); free(branches); free(misses);
    return result;
  }
  char baseline[128] = "";
  int samples = 0;
  for (int i = 0; i < warmup + runs; ++i) {
    char *perf_argv[] = {"perf", "stat", "-x,", "-e", "instructions:u,cycles:u,branches:u,branch-misses:u", (char *)path, NULL};
    char *plain_argv[] = {(char *)path, NULL};
    proc_result_t pr = run_proc(perf_argv, root, timeout_s);
    if (pr.rc != 0) {
      proc_result_free(&pr);
      pr = run_proc(plain_argv, root, timeout_s);
    }
    char checksum[128], elapsed[128];
    bool parsed = extract_prefixed_field(pr.out, "checksum=", checksum, sizeof(checksum)) &&
                  extract_prefixed_field(pr.out, "elapsed_ns=", elapsed, sizeof(elapsed));
    if (pr.rc != 0 || !parsed) {
      result.rc = pr.rc ? pr.rc : 1;
      result.timed_out = pr.timed_out;
      result.out = pr.out ? strdup(pr.out) : strdup("");
      result.err = pr.err ? strdup(pr.err) : strdup(parsed ? "" : "missing checksum/elapsed_ns in benchmark output");
      proc_result_free(&pr);
      free(elapsed_ns); free(process_ms);
      free(instructions); free(cycles); free(branches); free(misses);
      return result;
    }
    if (!baseline[0]) {
      snprintf(baseline, sizeof(baseline), "%s", checksum);
    } else if (strcmp(baseline, checksum) != 0) {
      result.rc = 1;
      result.out = pr.out ? strdup(pr.out) : strdup("");
      result.err = strdup("unstable checksum");
      proc_result_free(&pr);
      free(elapsed_ns); free(process_ms);
      free(instructions); free(cycles); free(branches); free(misses);
      return result;
    }
    free(result.out);
    free(result.err);
    result.out = pr.out ? strdup(pr.out) : strdup("");
    result.err = pr.err ? strdup(pr.err) : strdup("");
    if (i >= warmup && samples < runs) {
      elapsed_ns[samples] = atof(elapsed);
      process_ms[samples] = pr.elapsed_ms;
      double val = 0.0;
      if (extract_perf_stat_value(pr.err, "instructions:u", &val)) instructions[samples] = val;
      if (extract_perf_stat_value(pr.err, "cycles:u", &val)) cycles[samples] = val;
      if (extract_perf_stat_value(pr.err, "branches:u", &val)) branches[samples] = val;
      if (extract_perf_stat_value(pr.err, "branch-misses:u", &val)) misses[samples] = val;
      ++samples;
    }
    proc_result_free(&pr);
  }
  snprintf(result.checksum, sizeof(result.checksum), "%s", baseline);
  result.median_elapsed_ns = median_double(elapsed_ns, samples);
  result.process_median_ms = median_double(process_ms, samples);
  result.median_instructions = median_double(instructions, samples);
  result.median_cycles = median_double(cycles, samples);
  result.median_branches = median_double(branches, samples);
  result.median_branch_misses = median_double(misses, samples);
  result.rc = 0;
  result.ok = true;
  free(elapsed_ns); free(process_ms);
  free(instructions); free(cycles); free(branches); free(misses);
  return result;
}

static char *feature_key_from_array_text(const char *array_text) {
  char *csv = features_csv_from_json_array(array_text);
  string_list_t list = {0};
  string_list_push_csv_unique(&list, csv);
  qsort(list.items, (size_t)list.count, sizeof(char *), cmp_cstr);
  str_buf_t b = {0};
  for (int i = 0; i < list.count; ++i) {
    if (i) (void)sb_append_c(&b, '|');
    (void)sb_append(&b, list.items[i]);
  }
  free(csv);
  string_list_free(&list);
  return sb_take(&b);
}

static char *make_native_entry_id(const char *row) {
  char *structural = json_string_or_empty(row, "structural_hash");
  char *behavior = json_string_or_empty(row, "behavior_hash");
  if (!behavior || !*behavior) {
    free(behavior);
    behavior = json_string_or_empty(row, "behavior_hash_fnv1a64");
  }
  char *features = json_array_or_empty(row, "features");
  char *feature_key = feature_key_from_array_text(features);
  char *blocker = json_string_or_empty(row, "ir_blocker_key");
  char *method = json_method_or_generator(row);
  str_buf_t raw = {0};
  (void)sb_append(&raw, structural ? structural : "");
  (void)sb_append_c(&raw, ':');
  (void)sb_append(&raw, behavior ? behavior : "");
  (void)sb_append_c(&raw, ':');
  (void)sb_append(&raw, feature_key ? feature_key : "");
  (void)sb_append_c(&raw, ':');
  (void)sb_append(&raw, blocker ? blocker : "");
  (void)sb_append_c(&raw, ':');
  (void)sb_append(&raw, method ? method : "");
  uint64_t a = fnv1a64(raw.data ? raw.data : "", raw.len);
  (void)sb_append(&raw, ":nynth-core");
  uint64_t b = fnv1a64(raw.data ? raw.data : "", raw.len);
  char *id = NULL;
  (void)asprintf(&id, "%016" PRIx64 "%04" PRIx64, a, b & UINT64_C(0xffff));
  free(structural); free(behavior); free(features); free(feature_key); free(blocker); free(method);
  free(raw.data);
  return id;
}

static bool manifest_contains_id(const manifest_entry_list_t *entries, const char *id) {
  for (int i = 0; i < entries->count; ++i) {
    if (strcmp(entries->items[i].id ? entries->items[i].id : "", id ? id : "") == 0)
      return true;
  }
  return false;
}

static bool csv_contains_token(const char *csv, const char *token) {
  if (!csv || !token || !*token) return false;
  const char *start = csv;
  for (const char *p = csv; ; ++p) {
    if (*p != ',' && *p != '\0') continue;
    size_t n = (size_t)(p - start);
    while (n && isspace((unsigned char)*start)) {
      ++start;
      --n;
    }
    while (n && isspace((unsigned char)start[n - 1])) --n;
    if (strlen(token) == n && memcmp(start, token, n) == 0) return true;
    if (*p == '\0') break;
    start = p + 1;
  }
  return false;
}

static void collect_manifest_indexes(const manifest_entry_list_t *entries,
                                     string_list_t *features, string_list_t *families,
                                     string_list_t *lanes, string_list_t *generators,
                                     string_list_t *methods, string_list_t *blockers) {
  for (int i = 0; i < entries->count; ++i) {
    const char *row = entries->items[i].json;
    char *features_array = json_array_or_empty(row, "features");
    char *features_csv = features_csv_from_json_array(features_array);
    char *family = json_string_or_empty(row, "family");
    char *lane = json_string_or_empty(row, "lane");
    char *generator = json_string_or_empty(row, "generator");
    char *method = json_method_or_generator(row);
    char *blocker = json_string_or_empty(row, "ir_blocker_key");
    string_list_push_csv_unique(features, features_csv);
    if (family && *family) (void)string_list_push_unique_copy(families, family);
    if (lane && *lane) (void)string_list_push_unique_copy(lanes, lane);
    if (generator && *generator) (void)string_list_push_unique_copy(generators, generator);
    if (method && *method) (void)string_list_push_unique_copy(methods, method);
    if (blocker && *blocker) (void)string_list_push_unique_copy(blockers, blocker);
    free(features_array); free(features_csv); free(family); free(lane); free(generator); free(method); free(blocker);
  }
  qsort(features->items, (size_t)features->count, sizeof(char *), cmp_cstr);
  qsort(families->items, (size_t)families->count, sizeof(char *), cmp_cstr);
  qsort(lanes->items, (size_t)lanes->count, sizeof(char *), cmp_cstr);
  qsort(generators->items, (size_t)generators->count, sizeof(char *), cmp_cstr);
  qsort(methods->items, (size_t)methods->count, sizeof(char *), cmp_cstr);
  qsort(blockers->items, (size_t)blockers->count, sizeof(char *), cmp_cstr);
}

static void append_index_object(str_buf_t *b, const char *name, const manifest_entry_list_t *entries,
                                const string_list_t *keys, const char *field, bool field_is_features) {
  (void)sb_append_json_str(b, name);
  (void)sb_append(b, ":{");
  for (int k = 0; k < keys->count; ++k) {
    if (k) (void)sb_append_c(b, ',');
    (void)sb_append_json_str(b, keys->items[k]);
    (void)sb_append_c(b, ':');
    (void)sb_append_c(b, '[');
    bool first = true;
    for (int i = 0; i < entries->count; ++i) {
      bool match = false;
      if (field_is_features) {
        char *array = json_array_or_empty(entries->items[i].json, "features");
        char *csv = features_csv_from_json_array(array);
        match = csv_contains_token(csv, keys->items[k]);
        free(array); free(csv);
      } else {
        char *value = json_string_or_empty(entries->items[i].json, field);
        match = strcmp(value ? value : "", keys->items[k]) == 0;
        free(value);
      }
      if (!match) continue;
      if (!first) (void)sb_append_c(b, ',');
      first = false;
      (void)sb_append_json_str(b, entries->items[i].id);
    }
    (void)sb_append_c(b, ']');
  }
  (void)sb_append_c(b, '}');
}

static void append_method_index_object(str_buf_t *b, const manifest_entry_list_t *entries,
                                       const string_list_t *keys) {
  (void)sb_append_json_str(b, "methods");
  (void)sb_append(b, ":{");
  for (int k = 0; k < keys->count; ++k) {
    if (k) (void)sb_append_c(b, ',');
    (void)sb_append_json_str(b, keys->items[k]);
    (void)sb_append(b, ":[");
    bool first = true;
    for (int i = 0; i < entries->count; ++i) {
      char *method = json_method_or_generator(entries->items[i].json);
      bool match = strcmp(method ? method : "", keys->items[k]) == 0;
      free(method);
      if (!match) continue;
      if (!first) (void)sb_append_c(b, ',');
      first = false;
      (void)sb_append_json_str(b, entries->items[i].id);
    }
    (void)sb_append_c(b, ']');
  }
  (void)sb_append_c(b, '}');
}

static char *build_manifest_json(manifest_entry_list_t *entries) {
  qsort(entries->items, (size_t)entries->count, sizeof(entries->items[0]), manifest_entry_cmp);
  string_list_t features = {0}, families = {0}, lanes = {0}, generators = {0}, methods = {0}, blockers = {0};
  collect_manifest_indexes(entries, &features, &families, &lanes, &generators, &methods, &blockers);
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"version\":2,\"entries\":[");
  for (int i = 0; i < entries->count; ++i) {
    if (i) (void)sb_append_c(&b, ',');
    (void)sb_append(&b, entries->items[i].json);
  }
  (void)sb_append(&b, "],\"indexes\":{");
  append_index_object(&b, "features", entries, &features, "features", true);
  (void)sb_append_c(&b, ',');
  append_index_object(&b, "families", entries, &families, "family", false);
  (void)sb_append_c(&b, ',');
  append_index_object(&b, "lanes", entries, &lanes, "lane", false);
  (void)sb_append_c(&b, ',');
  append_index_object(&b, "generators", entries, &generators, "generator", false);
  (void)sb_append_c(&b, ',');
  append_method_index_object(&b, entries, &methods);
  (void)sb_append_c(&b, ',');
  append_index_object(&b, "ir_blockers", entries, &blockers, "ir_blocker_key", false);
  (void)sb_append(&b, "}}");
  string_list_free(&features); string_list_free(&families); string_list_free(&lanes);
  string_list_free(&generators); string_list_free(&methods); string_list_free(&blockers);
  return sb_take(&b);
}

static bool load_manifest_entries(const char *corpus_dir, manifest_entry_list_t *entries) {
  char *manifest_path = NULL, *legacy_manifest_path = NULL;
  const char *manifest_to_read = find_manifest_to_read(corpus_dir, &manifest_path, &legacy_manifest_path);
  if (!manifest_to_read) {
    free(manifest_path); free(legacy_manifest_path);
    return false;
  }
  file_buf_t manifest = {0};
  bool ok = true;
  if (exists_path(manifest_to_read)) {
    ok = read_file(manifest_to_read, &manifest) && parse_manifest_entries_raw(manifest.data, entries);
  }
  free(manifest.data);
  free(manifest_path); free(legacy_manifest_path);
  return ok;
}

static bool save_manifest_entries(const char *corpus_dir, manifest_entry_list_t *entries) {
  char *manifest_path = NULL;
  if (asprintf(&manifest_path, "%s/nynth_corpus.json", corpus_dir) < 0) return false;
  char *json = build_manifest_json(entries);
  bool ok = json && write_file_text(manifest_path, json);
  free(json);
  free(manifest_path);
  return ok;
}

static char *build_promotion_result_json(const char *row, const char *corpus_dir, const char *note,
                                         bool *ok_out, bool *promoted_out, bool *duplicate_out) {
  if (ok_out) *ok_out = false;
  if (promoted_out) *promoted_out = false;
  if (duplicate_out) *duplicate_out = false;
  char *case_name = json_string_or_empty(row, "case");
  if (json_failures_nonempty(row)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"row-has-failures\",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append_c(&fail, '}');
    free(case_name);
    return sb_take(&fail);
  }
  char *expected = json_string_or_empty(row, "expected_output");
  if (!expected || !*expected) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"missing-behavior-output\",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append_c(&fail, '}');
    free(case_name); free(expected);
    return sb_take(&fail);
  }
  char *c_source = json_string_or_empty(row, "c_source");
  char *ny_source = json_string_or_empty(row, "ny_source");
  char *ir_source = json_string_or_empty(row, "nynth_ir");
  if (!path_exists_file(c_source) || !path_exists_file(ny_source) || !path_exists_file(ir_source)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"missing-source\",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append(&fail, ",\"missing\":[");
    bool first = true;
#define ADD_MISSING_SOURCE(path_value) \
    do { if (!path_exists_file(path_value)) { if (!first) (void)sb_append_c(&fail, ','); first = false; (void)sb_append_json_str(&fail, path_value ? path_value : ""); } } while (0)
    ADD_MISSING_SOURCE(c_source);
    ADD_MISSING_SOURCE(ny_source);
    ADD_MISSING_SOURCE(ir_source);
#undef ADD_MISSING_SOURCE
    (void)sb_append(&fail, "]}");
    free(case_name); free(expected); free(c_source); free(ny_source); free(ir_source);
    return sb_take(&fail);
  }

  manifest_entry_list_t entries = {0};
  if (!load_manifest_entries(corpus_dir, &entries)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"manifest-read-failed\",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append_c(&fail, '}');
    free(case_name); free(expected); free(c_source); free(ny_source); free(ir_source);
    return sb_take(&fail);
  }
  char *id = make_native_entry_id(row);
  if (!id) {
    manifest_entry_list_free(&entries);
    free(case_name); free(expected); free(c_source); free(ny_source); free(ir_source);
    return strdup("{\"ok\":false,\"promoted\":false,\"reason\":\"allocation-failed\"}");
  }
  if (manifest_contains_id(&entries, id)) {
    if (ok_out) *ok_out = true;
    if (duplicate_out) *duplicate_out = true;
    str_buf_t dup = {0};
    (void)sb_append(&dup, "{\"ok\":true,\"promoted\":false,\"reason\":\"duplicate\",\"id\":");
    (void)sb_append_json_str(&dup, id);
    (void)sb_append(&dup, ",\"case\":");
    (void)sb_append_json_str(&dup, case_name);
    (void)sb_append_c(&dup, '}');
    manifest_entry_list_free(&entries);
    free(id); free(case_name); free(expected); free(c_source); free(ny_source); free(ir_source);
    return sb_take(&dup);
  }

  char *case_dir = NULL, *dst_c = NULL, *dst_ny = NULL, *dst_ir = NULL;
  bool path_ok = asprintf(&case_dir, "%s/cases/%s", corpus_dir, id) >= 0 &&
                 asprintf(&dst_c, "%s/case.c", case_dir) >= 0 &&
                 asprintf(&dst_ny, "%s/case.ny", case_dir) >= 0 &&
                 asprintf(&dst_ir, "%s/case.nynth.json", case_dir) >= 0;
  if (!path_ok || !mkdir_p(case_dir) ||
      !copy_file_bytes(c_source, dst_c) ||
      !copy_file_bytes(ny_source, dst_ny) ||
      !copy_file_bytes(ir_source, dst_ir)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"copy-failed\",\"id\":");
    (void)sb_append_json_str(&fail, id);
    (void)sb_append(&fail, ",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append_c(&fail, '}');
    manifest_entry_list_free(&entries);
    free(id); free(case_name); free(expected); free(c_source); free(ny_source); free(ir_source);
    free(case_dir); free(dst_c); free(dst_ny); free(dst_ir);
    return sb_take(&fail);
  }

  char *shape = json_string_or_empty(row, "shape");
  char *family = json_string_or_empty(row, "family");
  char *lane = json_string_or_empty(row, "lane");
  char *generator = json_string_or_empty(row, "generator");
  char *generator_kind = json_string_or_empty(row, "generator_kind");
  char *method = json_method_or_generator(row);
  char *source_kind = json_string_or_empty(row, "source_kind");
  char *shape_source = json_string_or_empty(row, "shape_source");
  char *shape_hash = json_string_or_empty(row, "shape_hash");
  char *template_name = json_string_or_empty(row, "template");
  char *profile = json_string_or_empty(row, "profile");
  char *structural = json_string_or_empty(row, "structural_hash");
  char *behavior = json_string_or_empty(row, "behavior_hash");
  if (!behavior || !*behavior) {
    free(behavior);
    behavior = json_string_or_empty(row, "behavior_hash_fnv1a64");
  }
  char *c_hash = json_string_or_empty(row, "c_emitter_hash");
  char *ny_hash = json_string_or_empty(row, "ny_emitter_hash");
  if (!c_hash || !*c_hash) {
    free(c_hash);
    char buf[32];
    file_hash_hex(c_source, buf, sizeof(buf));
    c_hash = strdup(buf);
  }
  if (!ny_hash || !*ny_hash) {
    free(ny_hash);
    char buf[32];
    file_hash_hex(ny_source, buf, sizeof(buf));
    ny_hash = strdup(buf);
  }
  char *features = json_array_or_empty(row, "features");
  char *feature_key = feature_key_from_array_text(features);
  char *blocker = json_string_or_empty(row, "ir_blocker_key");
  char *ratio = NULL;
  double ratio_value = 0.0;
  if (extract_json_number(row, "ny_o3i_vs_c_o3_run", &ratio_value)) {
    (void)asprintf(&ratio, "%.4f", ratio_value);
  }
  if (!lane || !*lane) {
    free(lane);
    lane = strdup(method && *method ? method : (generator && *generator ? generator : "native"));
  }
  if (!generator || !*generator) {
    free(generator);
    generator = strdup(method && *method ? method : "native");
  }
  if (!generator_kind || !*generator_kind) {
    free(generator_kind);
    generator_kind = strdup(method && *method ? method : generator);
  }
  if (!source_kind || !*source_kind) {
    free(source_kind);
    if (method && strcmp(method, "ir") == 0) source_kind = strdup("nynth-core-ir-typed-ast");
    else if (method && strcmp(method, "stress") == 0) source_kind = strdup("nynth-core-stress-optimizer");
    else source_kind = strdup("nynth-typed-ast");
  }

  str_buf_t entry = {0};
  (void)sb_append(&entry, "{\"id\":");
  (void)sb_append_json_str(&entry, id);
  (void)sb_append(&entry, ",\"case\":");
  (void)sb_append_json_str(&entry, case_name);
  (void)sb_append(&entry, ",\"shape\":");
  (void)sb_append_json_str(&entry, shape);
  (void)sb_append(&entry, ",\"family\":");
  (void)sb_append_json_str(&entry, family);
  (void)sb_append(&entry, ",\"lane\":");
  (void)sb_append_json_str(&entry, lane);
  (void)sb_append(&entry, ",\"generator\":");
  (void)sb_append_json_str(&entry, generator);
  (void)sb_append(&entry, ",\"generator_kind\":");
  (void)sb_append_json_str(&entry, generator_kind);
  (void)sb_append(&entry, ",\"method\":");
  (void)sb_append_json_str(&entry, method);
  (void)sb_append(&entry, ",\"source_kind\":");
  (void)sb_append_json_str(&entry, source_kind);
  (void)sb_append(&entry, ",\"shape_source\":");
  (void)sb_append_json_str(&entry, shape_source);
  (void)sb_append(&entry, ",\"shape_hash\":");
  (void)sb_append_json_str(&entry, shape_hash);
  (void)sb_append(&entry, ",\"shape_dsl_version\":1,\"template\":");
  (void)sb_append_json_str(&entry, template_name && *template_name ? template_name : shape);
  (void)sb_append(&entry, ",\"features\":");
  (void)sb_append(&entry, features);
  (void)sb_append(&entry, ",\"profile\":");
  (void)sb_append_json_str(&entry, profile);
  double seed_value = 0.0;
  if (extract_json_number(row, "seed", &seed_value)) (void)sb_appendf(&entry, ",\"seed\":%.0f", seed_value);
  else (void)sb_append(&entry, ",\"seed\":null");
  (void)sb_append(&entry, ",\"structural_hash\":");
  (void)sb_append_json_str(&entry, structural);
  (void)sb_append(&entry, ",\"behavior_hash\":");
  (void)sb_append_json_str(&entry, behavior);
  (void)sb_append(&entry, ",\"c_emitter_hash\":");
  (void)sb_append_json_str(&entry, c_hash);
  (void)sb_append(&entry, ",\"ny_emitter_hash\":");
  (void)sb_append_json_str(&entry, ny_hash);
  (void)sb_append(&entry, ",\"expected_output\":");
  (void)sb_append_json_str(&entry, expected);
  if (ratio) (void)sb_appendf(&entry, ",\"ratio_sample\":%s", ratio);
  else (void)sb_append(&entry, ",\"ratio_sample\":null");
  (void)sb_append(&entry, ",\"ir_blocker_key\":");
  (void)sb_append_json_str(&entry, blocker);
  (void)sb_append(&entry, ",\"feature_key\":");
  (void)sb_append_json_str(&entry, feature_key);
  char blocker_hash[32];
  snprintf(blocker_hash, sizeof(blocker_hash), "%016" PRIx64, fnv1a64(blocker ? blocker : "", strlen(blocker ? blocker : "")));
  (void)sb_append(&entry, ",\"blocker_hash\":");
  (void)sb_append_json_str(&entry, blocker_hash);
  (void)sb_append(&entry, ",\"timing_history\":[{\"ratio_sample\":");
  if (ratio) (void)sb_append(&entry, ratio);
  else (void)sb_append(&entry, "null");
  (void)sb_append(&entry, ",\"variants\":[]}]");
  (void)sb_append(&entry, ",\"promotion_reason\":");
  (void)sb_append_json_str(&entry, entries.count ? "native-build" : "bootstrap");
  (void)sb_append(&entry, ",\"last_replay\":{\"ok\":true,\"expected_output\":");
  (void)sb_append_json_str(&entry, expected);
  if (ratio) (void)sb_appendf(&entry, ",\"ratio_sample\":%s", ratio);
  else (void)sb_append(&entry, ",\"ratio_sample\":null");
  (void)sb_append(&entry, "},\"note\":");
  (void)sb_append_json_str(&entry, note ? note : "");
  (void)sb_append(&entry, ",\"paths\":{\"c\":");
  (void)sb_append_json_str(&entry, dst_c);
  (void)sb_append(&entry, ",\"ny\":");
  (void)sb_append_json_str(&entry, dst_ny);
  (void)sb_append(&entry, ",\"ir\":");
  (void)sb_append_json_str(&entry, dst_ir);
  (void)sb_append(&entry, "}}");

  char *entry_json = sb_take(&entry);
  if (!manifest_entry_list_push_take(&entries, strdup(id), entry_json) ||
      !save_manifest_entries(corpus_dir, &entries)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"manifest-write-failed\",\"id\":");
    (void)sb_append_json_str(&fail, id);
    (void)sb_append(&fail, ",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append_c(&fail, '}');
    manifest_entry_list_free(&entries);
    free(id); free(case_name); free(expected); free(c_source); free(ny_source); free(ir_source);
    free(case_dir); free(dst_c); free(dst_ny); free(dst_ir);
    free(shape); free(family); free(lane); free(generator); free(generator_kind); free(method); free(source_kind); free(shape_source);
    free(shape_hash); free(template_name); free(profile); free(structural); free(behavior);
    free(c_hash); free(ny_hash); free(features); free(feature_key); free(blocker); free(ratio);
    return sb_take(&fail);
  }

  if (ok_out) *ok_out = true;
  if (promoted_out) *promoted_out = true;
  str_buf_t result = {0};
  (void)sb_append(&result, "{\"ok\":true,\"promoted\":true,\"id\":");
  (void)sb_append_json_str(&result, id);
  (void)sb_append(&result, ",\"case\":");
  (void)sb_append_json_str(&result, case_name);
  (void)sb_append(&result, ",\"reason\":");
  (void)sb_append_json_str(&result, entries.count > 1 ? "native-build" : "bootstrap");
  (void)sb_append_c(&result, '}');

  manifest_entry_list_free(&entries);
  free(id); free(case_name); free(expected); free(c_source); free(ny_source); free(ir_source);
  free(case_dir); free(dst_c); free(dst_ny); free(dst_ir);
  free(shape); free(family); free(lane); free(generator); free(generator_kind); free(method); free(source_kind); free(shape_source);
  free(shape_hash); free(template_name); free(profile); free(structural); free(behavior);
  free(c_hash); free(ny_hash); free(features); free(feature_key); free(blocker); free(ratio);
  return sb_take(&result);
}

static char *build_creal_promotion_result_json(const char *row, const char *corpus_dir, const char *note,
                                               bool *ok_out, bool *promoted_out, bool *duplicate_out) {
  if (ok_out) *ok_out = false;
  if (promoted_out) *promoted_out = false;
  if (duplicate_out) *duplicate_out = false;
  char *case_name = json_string_or_empty(row, "case");
  if (json_failures_nonempty(row)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"row-has-failures\",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append_c(&fail, '}');
    free(case_name);
    return sb_take(&fail);
  }
  char *expected = json_string_or_empty(row, "expected_output");
  char *c_source = json_string_or_empty(row, "c_source");
  if (!c_source || !*c_source) {
    free(c_source);
    c_source = json_string_or_empty(row, "c");
  }
  if (!expected || !*expected || !path_exists_file(c_source)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"missing-creal-source-or-output\",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append(&fail, ",\"c_source\":");
    (void)sb_append_json_str(&fail, c_source ? c_source : "");
    (void)sb_append_c(&fail, '}');
    free(case_name); free(expected); free(c_source);
    return sb_take(&fail);
  }
  manifest_entry_list_t entries = {0};
  if (!load_manifest_entries(corpus_dir, &entries)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"manifest-read-failed\",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append_c(&fail, '}');
    free(case_name); free(expected); free(c_source);
    return sb_take(&fail);
  }
  char *id = json_string_or_empty(row, "id");
  if (!id || !*id) {
    free(id);
    id = make_native_entry_id(row);
  }
  if (!id) {
    manifest_entry_list_free(&entries);
    free(case_name); free(expected); free(c_source);
    return strdup("{\"ok\":false,\"promoted\":false,\"reason\":\"allocation-failed\"}");
  }
  if (manifest_contains_id(&entries, id)) {
    if (ok_out) *ok_out = true;
    if (duplicate_out) *duplicate_out = true;
    str_buf_t dup = {0};
    (void)sb_append(&dup, "{\"ok\":true,\"promoted\":false,\"reason\":\"duplicate\",\"id\":");
    (void)sb_append_json_str(&dup, id);
    (void)sb_append(&dup, ",\"case\":");
    (void)sb_append_json_str(&dup, case_name);
    (void)sb_append_c(&dup, '}');
    manifest_entry_list_free(&entries);
    free(id); free(case_name); free(expected); free(c_source);
    return sb_take(&dup);
  }

  char *case_dir = NULL, *dst_c = NULL;
  bool path_ok = asprintf(&case_dir, "%s/cases/%s", corpus_dir, id) >= 0 &&
                 asprintf(&dst_c, "%s/case.c", case_dir) >= 0;
  if (!path_ok || !mkdir_p(case_dir) || !copy_file_bytes(c_source, dst_c)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"copy-failed\",\"id\":");
    (void)sb_append_json_str(&fail, id);
    (void)sb_append(&fail, ",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append_c(&fail, '}');
    manifest_entry_list_free(&entries);
    free(id); free(case_name); free(expected); free(c_source); free(case_dir); free(dst_c);
    return sb_take(&fail);
  }

  char *function_name = json_string_or_empty(row, "function_name");
  char *return_type = json_string_or_empty(row, "return_type");
  char *parameter_types = json_array_or_empty(row, "parameter_types");
  char *src_file = json_string_or_empty(row, "src_file");
  char *db_source = json_string_or_empty(row, "db_source");
  char *structural = json_string_or_empty(row, "structural_hash");
  char *behavior = json_string_or_empty(row, "behavior_hash");
  char *sample_hash = json_string_or_empty(row, "sample_output_hash");
  if (!structural || !*structural) {
    free(structural);
    char buf[32];
    file_hash_hex(c_source, buf, sizeof(buf));
    structural = strdup(buf);
  }
  if ((!behavior || !*behavior) && sample_hash && *sample_hash) {
    free(behavior);
    behavior = strdup(sample_hash);
  }
  if (!behavior || !*behavior) {
    free(behavior);
    char buf[32];
    snprintf(buf, sizeof(buf), "%016" PRIx64, fnv1a64(expected, strlen(expected)));
    behavior = strdup(buf);
  }
  if (!sample_hash || !*sample_hash) {
    free(sample_hash);
    sample_hash = strdup(behavior ? behavior : "");
  }
  char *feature_key = feature_key_from_array_text(creal_features_json());

  str_buf_t entry = {0};
  (void)sb_append(&entry, "{\"id\":");
  (void)sb_append_json_str(&entry, id);
  (void)sb_append(&entry, ",\"case\":");
  (void)sb_append_json_str(&entry, case_name && *case_name ? case_name : function_name);
  (void)sb_append(&entry, ",\"shape\":\"creal-function\",\"family\":\"creal-function\","
                  "\"lane\":\"creal\",\"generator\":\"creal\",\"generator_kind\":\"creal\","
                  "\"method\":\"creal\",\"source_kind\":\"creal-function-db\",\"shape_source\":");
  (void)sb_append_json_str(&entry, db_source ? db_source : "");
  (void)sb_append(&entry, ",\"shape_hash\":");
  (void)sb_append_json_str(&entry, structural ? structural : "");
  (void)sb_append(&entry, ",\"shape_dsl_version\":1,\"template\":\"creal-function-db\",\"features\":");
  (void)sb_append(&entry, creal_features_json());
  (void)sb_append(&entry, ",\"profile\":\"creal\",\"seed\":null,\"structural_hash\":");
  (void)sb_append_json_str(&entry, structural ? structural : "");
  (void)sb_append(&entry, ",\"behavior_hash\":");
  (void)sb_append_json_str(&entry, behavior ? behavior : "");
  (void)sb_append(&entry, ",\"sample_output_hash\":");
  (void)sb_append_json_str(&entry, sample_hash ? sample_hash : "");
  (void)sb_append(&entry, ",\"c_emitter_hash\":");
  (void)sb_append_json_str(&entry, structural ? structural : "");
  (void)sb_append(&entry, ",\"ny_emitter_hash\":\"\",\"expected_output\":");
  (void)sb_append_json_str(&entry, expected);
  (void)sb_append(&entry, ",\"ratio_sample\":null,\"ir_blocker_key\":\"\",\"feature_key\":");
  (void)sb_append_json_str(&entry, feature_key ? feature_key : "");
  char blocker_hash[32];
  snprintf(blocker_hash, sizeof(blocker_hash), "%016" PRIx64, fnv1a64("", 0));
  (void)sb_append(&entry, ",\"blocker_hash\":");
  (void)sb_append_json_str(&entry, blocker_hash);
  (void)sb_append(&entry, ",\"timing_history\":[],\"promotion_reason\":");
  (void)sb_append_json_str(&entry, note ? note : "creal-promote");
  (void)sb_append(&entry, ",\"last_replay\":{\"ok\":true,\"expected_output\":");
  (void)sb_append_json_str(&entry, expected);
  (void)sb_append(&entry, "},\"note\":");
  (void)sb_append_json_str(&entry, note ? note : "");
  (void)sb_append(&entry, ",\"c\":");
  (void)sb_append_json_str(&entry, dst_c);
  (void)sb_append(&entry, ",\"creal\":{\"function_name\":");
  (void)sb_append_json_str(&entry, function_name ? function_name : "");
  (void)sb_append(&entry, ",\"return_type\":");
  (void)sb_append_json_str(&entry, return_type ? return_type : "");
  (void)sb_append(&entry, ",\"parameter_types\":");
  (void)sb_append(&entry, parameter_types ? parameter_types : "[]");
  (void)sb_append(&entry, ",\"src_file\":");
  (void)sb_append_json_str(&entry, src_file ? src_file : "");
  (void)sb_append(&entry, ",\"compat_level\":\"function-db-sample-io\",\"function_db\":");
  (void)sb_append_json_str(&entry, db_source ? db_source : "");
  (void)sb_append(&entry, "},\"paths\":{\"c\":");
  (void)sb_append_json_str(&entry, dst_c);
  (void)sb_append(&entry, "}}");

  char *entry_json = sb_take(&entry);
  if (!entry_json || !manifest_entry_list_push_take(&entries, strdup(id), entry_json) ||
      !save_manifest_entries(corpus_dir, &entries)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"manifest-write-failed\",\"id\":");
    (void)sb_append_json_str(&fail, id);
    (void)sb_append(&fail, ",\"case\":");
    (void)sb_append_json_str(&fail, case_name);
    (void)sb_append_c(&fail, '}');
    free(entry_json);
    manifest_entry_list_free(&entries);
    free(id); free(case_name); free(expected); free(c_source); free(case_dir); free(dst_c);
    free(function_name); free(return_type); free(parameter_types); free(src_file); free(db_source);
    free(structural); free(behavior); free(sample_hash); free(feature_key);
    return sb_take(&fail);
  }

  if (ok_out) *ok_out = true;
  if (promoted_out) *promoted_out = true;
  str_buf_t result = {0};
  (void)sb_append(&result, "{\"ok\":true,\"promoted\":true,\"id\":");
  (void)sb_append_json_str(&result, id);
  (void)sb_append(&result, ",\"case\":");
  (void)sb_append_json_str(&result, case_name);
  (void)sb_append(&result, ",\"reason\":\"creal-promote\"}");
  manifest_entry_list_free(&entries);
  free(id); free(case_name); free(expected); free(c_source); free(case_dir); free(dst_c);
  free(function_name); free(return_type); free(parameter_types); free(src_file); free(db_source);
  free(structural); free(behavior); free(sample_hash); free(feature_key);
  return sb_take(&result);
}

static void append_generated_cases_json(str_buf_t *b, const generated_case_list_t *cases) {
  (void)sb_append_c(b, '[');
  for (int i = 0; i < cases->count; ++i) {
    if (i) (void)sb_append_c(b, ',');
    (void)sb_append(b, cases->items[i].json);
  }
  (void)sb_append_c(b, ']');
}

static const char *generated_case_field_value(const generated_case_t *gc, const char *field) {
  if (!gc || !field) return "";
  if (strcmp(field, "shape") == 0) return gc->shape ? gc->shape : "";
  if (strcmp(field, "family") == 0) return gc->family ? gc->family : "";
  if (strcmp(field, "generator") == 0) return gc->generator ? gc->generator : "";
  if (strcmp(field, "method") == 0) return gc->method ? gc->method : "";
  if (strcmp(field, "source_kind") == 0) return gc->source_kind ? gc->source_kind : "";
  return "";
}

static int generated_case_unique_field_count(const generated_case_list_t *cases,
                                             const char *field) {
  string_list_t keys = {0};
  for (int i = 0; i < cases->count; ++i) {
    const char *value = generated_case_field_value(&cases->items[i], field);
    if (value && *value) (void)string_list_push_unique_copy(&keys, value);
  }
  int count = keys.count;
  string_list_free(&keys);
  return count;
}

static void append_generated_case_field_counts_json(str_buf_t *b,
                                                    const generated_case_list_t *cases,
                                                    const char *field) {
  string_list_t keys = {0};
  for (int i = 0; i < cases->count; ++i) {
    const char *value = generated_case_field_value(&cases->items[i], field);
    if (value && *value) (void)string_list_push_unique_copy(&keys, value);
  }
  (void)sb_append_c(b, '{');
  for (int k = 0; k < keys.count; ++k) {
    int count = 0;
    for (int i = 0; i < cases->count; ++i) {
      const char *value = generated_case_field_value(&cases->items[i], field);
      if (value && strcmp(value, keys.items[k]) == 0) count++;
    }
    if (k) (void)sb_append_c(b, ',');
    (void)sb_append_json_str(b, keys.items[k]);
    (void)sb_appendf(b, ":%d", count);
  }
  (void)sb_append_c(b, '}');
  string_list_free(&keys);
}

static bool csv_contains_value(const char *csv, const char *needle) {
  if (!csv || !needle || !*needle) return false;
  size_t needle_n = strlen(needle);
  const char *start = csv;
  for (const char *p = csv; ; ++p) {
    if (*p != ',' && *p != '\0') continue;
    size_t n = (size_t)(p - start);
    while (n && isspace((unsigned char)*start)) {
      ++start;
      --n;
    }
    while (n && isspace((unsigned char)start[n - 1])) --n;
    if (n == needle_n && strncmp(start, needle, n) == 0) return true;
    if (*p == '\0') break;
    start = p + 1;
  }
  return false;
}

static int generated_case_unique_feature_count(const generated_case_list_t *cases) {
  string_list_t keys = {0};
  for (int i = 0; i < cases->count; ++i)
    string_list_push_csv_unique(&keys, cases->items[i].features_csv);
  int count = keys.count;
  string_list_free(&keys);
  return count;
}

static void append_generated_feature_counts_json(str_buf_t *b,
                                                 const generated_case_list_t *cases) {
  string_list_t keys = {0};
  for (int i = 0; i < cases->count; ++i)
    string_list_push_csv_unique(&keys, cases->items[i].features_csv);
  (void)sb_append_c(b, '{');
  for (int k = 0; k < keys.count; ++k) {
    int count = 0;
    for (int i = 0; i < cases->count; ++i) {
      if (csv_contains_value(cases->items[i].features_csv, keys.items[k])) count++;
    }
    if (k) (void)sb_append_c(b, ',');
    (void)sb_append_json_str(b, keys.items[k]);
    (void)sb_appendf(b, ":%d", count);
  }
  (void)sb_append_c(b, '}');
  string_list_free(&keys);
}

static void append_generated_coverage_json(str_buf_t *b,
                                           const generated_case_list_t *cases) {
  (void)sb_appendf(b,
                   "{\"unique_shapes\":%d,\"unique_families\":%d,"
                   "\"unique_generators\":%d,\"unique_methods\":%d,"
                   "\"unique_source_kinds\":%d,\"unique_features\":%d",
                   generated_case_unique_field_count(cases, "shape"),
                   generated_case_unique_field_count(cases, "family"),
                   generated_case_unique_field_count(cases, "generator"),
                   generated_case_unique_field_count(cases, "method"),
                   generated_case_unique_field_count(cases, "source_kind"),
                   generated_case_unique_feature_count(cases));
  (void)sb_append(b, ",\"shape_counts\":");
  append_generated_case_field_counts_json(b, cases, "shape");
  (void)sb_append(b, ",\"family_counts\":");
  append_generated_case_field_counts_json(b, cases, "family");
  (void)sb_append(b, ",\"generator_counts\":");
  append_generated_case_field_counts_json(b, cases, "generator");
  (void)sb_append(b, ",\"method_counts\":");
  append_generated_case_field_counts_json(b, cases, "method");
  (void)sb_append(b, ",\"source_kind_counts\":");
  append_generated_case_field_counts_json(b, cases, "source_kind");
  (void)sb_append(b, ",\"feature_counts\":");
  append_generated_feature_counts_json(b, cases);
  (void)sb_append_c(b, '}');
}

static int worker_usage(void) {
  nynth_print_public_usage(stdout);
  return 2;
}

static bool is_help_flag(const char *arg) {
  return arg && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0);
}

static bool wants_fuzz_all_help(int argc, char **argv) {
  if (argc >= 4 && strcmp(argv[1], "help") == 0 &&
      strcmp(argv[2], "fuzz") == 0 && strcmp(argv[3], "all") == 0)
    return true;
  if (argc >= 3 && strcmp(argv[1], "fuzz") == 0 &&
      strcmp(argv[2], "all") == 0) {
    for (int i = 3; i < argc; ++i) {
      if (is_help_flag(argv[i]) || strcmp(argv[i], "help") == 0)
        return true;
    }
  }
  return false;
}

static bool exists_path(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static bool executable_path(const char *path) {
  return path && *path && access(path, X_OK) == 0;
}

static bool path_join(char *out, size_t out_sz, const char *base, const char *rel) {
  if (!out || !out_sz) return false;
  const char *b = base ? base : "";
  const char *r = rel ? rel : "";
  size_t bl = strlen(b), rl = strlen(r);
  bool need_sep = bl > 0 && b[bl - 1] != '/';
  size_t total = bl + (need_sep ? 1u : 0u) + rl;
  if (total + 1u > out_sz) {
    out[0] = '\0';
    return false;
  }
  memcpy(out, b, bl);
  size_t pos = bl;
  if (need_sep) out[pos++] = '/';
  memcpy(out + pos, r, rl + 1u);
  return true;
}

static bool looks_like_repo(const char *path) {
  char a[4096], b[4096], c[4096], d[4096];
  (void)path_join(a, sizeof(a), path, "src");
  (void)path_join(b, sizeof(b), path, "lib");
  (void)path_join(c, sizeof(c), path, "etc");
  (void)path_join(d, sizeof(d), path, "tmp");
  return exists_path(a) && exists_path(b) && exists_path(c) && exists_path(d);
}

static bool looks_like_nynth_root(const char *path) {
  char src[4096], cli[4096], core[4096], shapes[4096], makefile[4096];
  if (!path || !*path) return false;
  (void)path_join(src, sizeof(src), path, "src");
  (void)path_join(cli, sizeof(cli), path, "src/cli.c");
  (void)path_join(core, sizeof(core), path, "src/core.h");
  (void)path_join(shapes, sizeof(shapes), path, "shapes");
  (void)path_join(makefile, sizeof(makefile), path, "Makefile");
  return exists_path(src) && exists_path(cli) && exists_path(core) &&
         exists_path(shapes) && exists_path(makefile);
}

static bool find_nynth_root_from_path(const char *start, char *out, size_t out_sz) {
  if (!start || !*start) return false;
  char cur[4096];
  snprintf(cur, sizeof(cur), "%s", start);
  while (1) {
    if (looks_like_nynth_root(cur)) {
      snprintf(out, out_sz, "%s", cur);
      return true;
    }
    char *slash = strrchr(cur, '/');
    if (!slash || slash == cur) break;
    *slash = '\0';
  }
  return false;
}

static bool find_nynth_root(char *out, size_t out_sz) {
  const char *env = getenv("NYNTH_ROOT");
  if (env && *env && find_nynth_root_from_path(env, out, out_sz)) return true;
  if (g_self_path[0] && find_nynth_root_from_path(g_self_path, out, out_sz)) return true;
  const char *pwd = getenv("PWD");
  if (pwd && *pwd && find_nynth_root_from_path(pwd, out, out_sz)) return true;
  char cur[4096];
  if (!getcwd(cur, sizeof(cur))) return false;
  return find_nynth_root_from_path(cur, out, out_sz);
}

static int nynth_vasprintf(char **out, const char *rel_fmt, va_list ap) {
  if (!out || !rel_fmt) return -1;
  *out = NULL;
  char *rel = NULL;
  if (vasprintf(&rel, rel_fmt, ap) < 0 || !rel) return -1;
  if (rel[0] == '/') {
    *out = rel;
    return (int)strlen(rel);
  }
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    free(rel);
    return -1;
  }
  int rc = asprintf(out, "%s/%s", root, rel);
  free(rel);
  return rc;
}

static int nynth_asprintf(char **out, const char *rel_fmt, ...) {
  va_list ap;
  va_start(ap, rel_fmt);
  int rc = nynth_vasprintf(out, rel_fmt, ap);
  va_end(ap);
  return rc;
}

static bool find_repo_root_from_path(const char *start, char *out, size_t out_sz) {
  if (!start || !*start) return false;
  char cur[4096];
  snprintf(cur, sizeof(cur), "%s", start);

  while (1) {
    if (looks_like_repo(cur)) {
      snprintf(out, out_sz, "%s", cur);
      return true;
    }
    char *slash = strrchr(cur, '/');
    if (!slash || slash == cur) break;
    *slash = '\0';
  }
  return false;
}

static bool find_repo_root(char *out, size_t out_sz) {
  const char *env = getenv("NYTRIX_ROOT");
  if (env && *env && find_repo_root_from_path(env, out, out_sz)) {
    return true;
  }
  const char *pwd = getenv("PWD");
  if (pwd && *pwd && find_repo_root_from_path(pwd, out, out_sz)) return true;
  char cur[4096];
  if (getcwd(cur, sizeof(cur)) && find_repo_root_from_path(cur, out, out_sz)) return true;
  char nynth_root[4096], sibling[4096];
  if (find_nynth_root(nynth_root, sizeof(nynth_root)) &&
      path_join(sibling, sizeof(sibling), nynth_root, "../nytrix") &&
      find_repo_root_from_path(sibling, out, out_sz))
    return true;
  const char *ny_bin = getenv("NYTRIX_NY_BIN");
  if (ny_bin && *ny_bin && executable_path(ny_bin) && out_sz) {
    out[0] = '\0';
    return true;
  }
  return false;
}

static bool find_repo_root_or_sibling(char *out, size_t out_sz) {
  if (find_repo_root(out, out_sz)) return true;
  char nynth_root[4096], sibling[4096];
  if (find_nynth_root(nynth_root, sizeof(nynth_root)) &&
      path_join(sibling, sizeof(sibling), nynth_root, "../nytrix") &&
      find_repo_root_from_path(sibling, out, out_sz))
    return true;
  return false;
}

static bool find_ny_bin_under_root(const char *root, char *out, size_t out_sz) {
  if (!root || !*root) return false;
  const char *rels[] = {"build/release/ny", "build/debug/ny"};
  for (size_t i = 0; i < sizeof(rels) / sizeof(rels[0]); ++i) {
    char *cand = NULL;
    if (asprintf(&cand, "%s/%s", root, rels[i]) < 0) return false;
    bool ok = executable_path(cand) && strlen(cand) < out_sz;
    if (ok) snprintf(out, out_sz, "%s", cand);
    free(cand);
    if (ok) return true;
  }
  return false;
}

static bool find_ny_bin(const char *root, char *out, size_t out_sz) {
  const char *env = getenv("NYTRIX_NY_BIN");
  if (env && *env) {
    if (!executable_path(env)) return false;
    if (strlen(env) >= out_sz) return false;
    snprintf(out, out_sz, "%s", env);
    return true;
  }
  if (find_ny_bin_under_root(root, out, out_sz)) return true;
  const char *ny_root = getenv("NYTRIX_ROOT");
  if (ny_root && *ny_root && find_ny_bin_under_root(ny_root, out, out_sz)) return true;
  if (root && *root) {
    char *sibling = NULL;
    if (asprintf(&sibling, "%s/../nytrix", root) < 0) return false;
    bool ok = find_ny_bin_under_root(sibling, out, out_sz);
    free(sibling);
    if (ok) return true;
  }
  return false;
}

static bool mkdir_parent(const char *path) {
  char tmp[4096];
  size_t len = strlen(path);
  if (len >= sizeof(tmp)) return false;
  memcpy(tmp, path, len + 1);
  char *slash = strrchr(tmp, '/');
  if (!slash || slash == tmp) return true;
  *slash = '\0';
  ny_ensure_dir_recursive(tmp);
  return true;
}

static bool write_file_text(const char *path, const char *data) {
  if (!mkdir_parent(path)) return false;
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  size_t len = strlen(data);
  bool ok = fwrite(data, 1, len, f) == len;
  if (fclose(f) != 0) ok = false;
  return ok;
}

static bool write_file_bytes(const char *path, const unsigned char *data, size_t len) {
  if (!mkdir_parent(path)) return false;
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, len, f) == len;
  if (fclose(f) != 0) ok = false;
  return ok;
}

static const char *value_after(int argc, char **argv, int start, const char *flag, const char *fallback) {
  size_t flag_len = strlen(flag);
  for (int i = start; i < argc; ++i) {
    if (strncmp(argv[i], flag, flag_len) == 0 && argv[i][flag_len] == '=')
      return argv[i] + flag_len + 1;
    if (i + 1 < argc && strcmp(argv[i], flag) == 0) return argv[i + 1];
  }
  return fallback;
}

static const char *value_after_equals(int argc, char **argv, int start, const char *flag, const char *fallback) {
  return value_after(argc, argv, start, flag, fallback);
}

static bool has_flag_after(int argc, char **argv, int start, const char *flag) {
  for (int i = start; i < argc; ++i) {
    if (strcmp(argv[i], flag) == 0) return true;
  }
  return false;
}

static void command_string(int argc, char **argv, str_buf_t *out) {
  for (int i = 1; i < argc; ++i) {
    if (i > 1) (void)sb_append_c(out, ' ');
    (void)sb_append(out, argv[i]);
  }
}

static int count_json_true_fields(const char *json, const char *key) {
  if (!json || !key || !*key) return 0;
  char pat[128];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  int count = 0;
  size_t n = strlen(pat);
  const char *end = json + strlen(json);
  const char *p = json;
  while ((p = find_n(p, end, pat)) != NULL) {
    const char *after = skip_ws_const(p + n);
    if (after && after < end && *after == ':') {
      const char *value = skip_ws_const(after + 1);
      if (value && value + 4 <= end && strncmp(value, "true", 4) == 0 &&
          (value + 4 == end ||
           (!isalnum((unsigned char)value[4]) && value[4] != '_'))) {
        ++count;
      }
    }
    p += n;
  }
  return count;
}

static int unsupported_command(int argc, char **argv) {
  str_buf_t cmd = {0};
  command_string(argc, argv, &cmd);
  printf("{\"ok\":false,\"error\":\"unsupported\",\"command\":");
  json_str(stdout, cmd.data ? cmd.data : "");
  printf(",\"reason\":\"native-command-unsupported\",\"engine\":\"nynth_core\"}\n");
  free(cmd.data);
  return 3;
}

static char *tree_clean_failure_json(const char *reason, const string_list_t *items) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"tool\":\"nynth_core\",\"phase\":\"selftest-python-clean\",\"reason\":");
  (void)sb_append_json_str(&b, reason ? reason : "tree-clean-failed");
  (void)sb_append(&b, ",\"engine\":\"nynth_core\",\"items\":");
  append_string_list_json(&b, items);
  (void)sb_append_c(&b, '}');
  return sb_take(&b);
}

static void tree_clean_add_single_failure(string_list_t *failures, const char *reason, const char *item) {
  string_list_t items = {0};
  (void)string_list_push_copy(&items, item ? item : "");
  (void)string_list_push_take(failures, tree_clean_failure_json(reason, &items));
  string_list_free(&items);
}

static void tree_clean_validate_public_usage_json(const char *label,
                                                  const char *json,
                                                  bool expect_ok,
                                                  string_list_t *failures) {
  if (!json || !*json) {
    tree_clean_add_single_failure(failures, "public-usage-output-missing",
                                  label);
    return;
  }
  bool ok_alias =
      strstr(json, expect_ok ? "\"ok\":true" : "\"ok\":false") != NULL;
  double cases = -1.0, ok_count = -1.0, failure_count = -1.0;
  double command_count = -1.0;
  bool numbers_ok =
      extract_json_number(json, "cases", &cases) &&
      extract_json_number(json, "ok_count", &ok_count) &&
      extract_json_number(json, "failure_count", &failure_count) &&
      extract_json_number(json, "command_count", &command_count);
  const char *commands = json_top_level_value_after_key(json, "commands");
  int actual_commands = commands && *commands == '[' ?
      count_json_array_items(commands) : -1;
  bool result_ok =
      ok_alias && numbers_ok && cases == 1.0 &&
      ok_count == (expect_ok ? 1.0 : 0.0) &&
      failure_count == (expect_ok ? 0.0 : 1.0);
  bool count_ok = actual_commands > 0 && command_count == actual_commands;
  bool surface_ok =
      strstr(json, "\"engine\":\"nynth_core\"") &&
      strstr(json, "\"shapes audit\"") &&
      strstr(json, "\"fuzz all status\"") &&
      strstr(json, "\"replay-corpus-entry\"");
  if (!result_ok)
    tree_clean_add_single_failure(failures,
                                  "public-usage-result-aliases-missing",
                                  label);
  if (!count_ok)
    tree_clean_add_single_failure(failures,
                                  "public-usage-command-count-mismatch",
                                  label);
  if (!surface_ok)
    tree_clean_add_single_failure(failures,
                                  "public-usage-command-surface-incomplete",
                                  label);
}

static int cmd_public_selftest_python_clean(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\",\"engine\":\"nynth_core\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  char *nynth_dir = strdup(root), *src_dir = NULL, *core_h = NULL, *retired_c_dir = NULL;
  bool path_ok = nynth_dir &&
                 asprintf(&src_dir, "%s/src", nynth_dir) >= 0 &&
                 asprintf(&core_h, "%s/src/core.h", nynth_dir) >= 0 &&
                 asprintf(&retired_c_dir, "%s/c", nynth_dir) >= 0;
  if (!path_ok) {
    free(nynth_dir); free(src_dir); free(core_h); free(retired_c_dir);
    printf("{\"ok\":false,\"error\":\"allocation-failed\",\"engine\":\"nynth_core\"}\n");
    return 2;
  }

  tree_clean_audit_t audit = {0};
  string_list_t failures = {0};
  bool walked = tree_clean_walk(nynth_dir, &audit);
  bool src_ok = exists_path(src_dir);
  bool core_ok = exists_path(core_h);
  bool retired_c_exists = exists_path(retired_c_dir);
  if (!walked) tree_clean_add_single_failure(&failures, "tree-walk-failed", nynth_dir);
  if (!src_ok) tree_clean_add_single_failure(&failures, "missing-src-dir", src_dir);
  if (!core_ok) tree_clean_add_single_failure(&failures, "missing-core-header", core_h);
  if (retired_c_exists) tree_clean_add_single_failure(&failures, "retired-c-dir-present", retired_c_dir);
  if (audit.python_files.count)
    (void)string_list_push_take(&failures, tree_clean_failure_json("python-files-present", &audit.python_files));
  if (audit.pycache_dirs.count)
    (void)string_list_push_take(&failures, tree_clean_failure_json("pycache-dirs-present", &audit.pycache_dirs));
  if (audit.legacy_refs.count)
    (void)string_list_push_take(&failures, tree_clean_failure_json("legacy-python-port-refs-present", &audit.legacy_refs));
  char *help_argv[] = {g_self_path, "--help", NULL};
  proc_result_t help_pr = run_proc(help_argv, root, 10.0);
  if (help_pr.rc != 0)
    tree_clean_add_single_failure(&failures, "public-help-rc", "--help");
  tree_clean_validate_public_usage_json("--help", help_pr.out, true,
                                        &failures);
  proc_result_free(&help_pr);
  char *usage_argv[] = {g_self_path, NULL};
  proc_result_t usage_pr = run_proc(usage_argv, root, 10.0);
  if (usage_pr.rc != 2)
    tree_clean_add_single_failure(&failures, "public-usage-rc", "no-args");
  tree_clean_validate_public_usage_json("no-args", usage_pr.out, false,
                                        &failures);
  proc_result_free(&usage_pr);

  bool ok = failures.count == 0;
  str_buf_t report = {0};
  (void)sb_append(&report, "{\"rows\":[{\"name\":\"nynth_tree_clean\",\"ok\":");
  (void)sb_append(&report, ok ? "true" : "false");
  (void)sb_append(&report, ",\"engine\":\"nynth_core\",\"root\":");
  (void)sb_append_json_str(&report, nynth_dir);
  (void)sb_appendf(&report,
                   ",\"files_scanned\":%d,\"python_files\":%d,\"pycache_dirs\":%d,"
                   "\"legacy_refs\":%d,\"src_dir\":%s,\"core_h\":%s,"
                   "\"retired_c_dir\":%s}]",
                   audit.files_scanned, audit.python_files.count, audit.pycache_dirs.count,
                   audit.legacy_refs.count, src_ok ? "true" : "false",
                   core_ok ? "true" : "false", retired_c_exists ? "true" : "false");
  (void)sb_append(&report, ",\"failures\":");
  append_raw_json_list(&report, &failures);
  (void)sb_appendf(&report,
                   ",\"ok\":%s,\"cases\":1,\"ok_count\":%d,"
                   "\"failure_count\":%d",
                   ok ? "true" : "false", ok ? 1 : 0, failures.count);
  (void)sb_appendf(&report,
                   ",\"summary\":{\"cases\":1,\"ok\":%d,\"failure_count\":%d,"
                   "\"files_scanned\":%d,\"python_files\":%d,\"pycache_dirs\":%d,"
                   "\"legacy_refs\":%d,\"engine\":\"nynth_core\"},"
                   "\"meta\":{\"engine\":\"nynth_core\",\"selftest_scope\":\"nynth-tree-clean\"}}",
                   ok ? 1 : 0, failures.count, audit.files_scanned,
                   audit.python_files.count, audit.pycache_dirs.count, audit.legacy_refs.count);
  char *report_json = sb_take(&report);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf(",\"engine\":\"nynth_core\"}\n");
    ok = false;
  } else {
    printf("python-clean: %s\n", ok ? "ok" : "failed");
    printf("files scanned: %d\n", audit.files_scanned);
    if (!ok) printf("failures: %d\n", failures.count);
  }
  free(report_json);
  string_list_free(&failures);
  string_list_free(&audit.python_files);
  string_list_free(&audit.pycache_dirs);
  string_list_free(&audit.legacy_refs);
  free(nynth_dir); free(src_dir); free(core_h); free(retired_c_dir);
  return ok ? 0 : 1;
}

static int cmd_public_selftest_worker_args(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\",\"engine\":\"nynth_core\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  string_list_t rows = {0}, failures = {0};
  char *work_dir = NULL, *out_dir = NULL, *out_arg = NULL;
  bool path_ok = asprintf(&work_dir, "%s/%s/nynth_worker_args_%ld",
                          root, NYNTH_DEFAULT_SCRATCH_ROOT, (long)getpid()) >= 0 &&
                 asprintf(&out_dir, "%s/generated_equals", work_dir ? work_dir : "") >= 0 &&
                 asprintf(&out_arg, "--out=%s", out_dir ? out_dir : "") >= 0;
  if (!path_ok || !work_dir || !out_dir || !out_arg || !mkdir_p(work_dir)) {
    (void)string_list_push_take(&rows, native_row_status("worker_equals_args",
                                                        "selftest-worker-args",
                                                        false, "phase", "prepare"));
    (void)string_list_push_take(&failures,
                                make_worker_failure_row("worker-equals-args",
                                                        "prepare", 1, "",
                                                        "scratch workdir allocation failed"));
  } else {
    char *gen_argv[] = {
      g_self_path, "generate-batch",
      "--shape-dir=shapes",
      "--profile=optimizer",
      "--generator=typed",
      "--schedule=smart",
      "--seed=515",
      "--cases=1",
      "--fast",
      out_arg,
      NULL
    };
    proc_result_t pr = run_proc(gen_argv, root, 45.0);
    generated_case_list_t generated = {0};
    bool parsed = pr.rc == 0 && pr.out && parse_generated_cases(pr.out, &generated);
    bool one_case = parsed && generated.count == 1;
    generated_case_t *gc = one_case ? &generated.items[0] : NULL;
    bool paths_under_out =
        gc && gc->c_path && gc->ny_path && gc->ir_path &&
        strstr(gc->c_path, out_dir) && strstr(gc->ny_path, out_dir) &&
        strstr(gc->ir_path, out_dir);
    bool files_written =
        paths_under_out && exists_path(gc->c_path) && exists_path(gc->ny_path) &&
        exists_path(gc->ir_path);
    bool generator_ok = gc && gc->generator && strcmp(gc->generator, "typed") == 0;
    bool ok = one_case && paths_under_out && files_written && generator_ok;
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"name\":\"worker_equals_args\","
                          "\"kind\":\"selftest-worker-args\",\"ok\":");
    (void)sb_append(&row, ok ? "true" : "false");
    (void)sb_appendf(&row, ",\"generated_cases\":%d,\"engine\":\"nynth_core\","
                          "\"out_dir\":", generated.count);
    (void)sb_append_json_str(&row, out_dir);
    (void)sb_append(&row, ",\"case_name\":");
    (void)sb_append_json_str(&row, gc && gc->name ? gc->name : "");
    (void)sb_append(&row, ",\"c_source\":");
    (void)sb_append_json_str(&row, gc && gc->c_path ? gc->c_path : "");
    (void)sb_append(&row, ",\"ny_source\":");
    (void)sb_append_json_str(&row, gc && gc->ny_path ? gc->ny_path : "");
    (void)sb_append(&row, ",\"nynth_ir\":");
    (void)sb_append_json_str(&row, gc && gc->ir_path ? gc->ir_path : "");
    (void)sb_append(&row, "}");
    (void)string_list_push_take(&rows, sb_take(&row));
    if (!ok) {
      const char *reason = !one_case ? "equals-form --cases was ignored or output was not parsed" :
                           !paths_under_out ? "equals-form --out was ignored" :
                           !files_written ? "generated worker files were missing" :
                           "equals-form --generator was ignored";
      (void)string_list_push_take(&failures,
                                  make_worker_failure_row("worker-equals-args",
                                                          "generate-batch",
                                                          pr.rc, pr.out,
                                                          pr.err && *pr.err ? pr.err : reason));
    }
    generated_case_list_free(&generated);
    proc_result_free(&pr);
  }
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"work_dir\":");
  (void)sb_append_json_str(&extra, work_dir ? work_dir : "");
  (void)sb_append(&extra, ",\"out_dir\":");
  (void)sb_append_json_str(&extra, out_dir ? out_dir : "");
  char *report = build_native_report_json(&rows, &failures, "selftest-worker-args",
                                          extra.data ? extra.data : "");
  int rc = emit_native_report(report, json_path, "selftest worker args",
                              rows.count, failures.count);
  free(extra.data);
  free(work_dir);
  free(out_dir);
  free(out_arg);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static bool env_value_matches(const char *key, const char *expected, str_buf_t *reason) {
  const char *actual = getenv(key);
  bool ok = actual && expected && strcmp(actual, expected) == 0;
  if (!ok && reason) {
    if (reason->len) (void)sb_append(reason, "; ");
    (void)sb_append(reason, key ? key : "");
    (void)sb_append(reason, " expected ");
    (void)sb_append(reason, expected ? expected : "");
    (void)sb_append(reason, " got ");
    (void)sb_append(reason, actual ? actual : "");
  }
  return ok;
}

static int cmd_public_selftest_child_tmp_env(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\",\"engine\":\"nynth_core\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  bool check = has_flag_after(argc, argv, 3, "--check");
  if (!check) {
    char child_cwd[4096] = "";
    if (!find_repo_root_or_sibling(child_cwd, sizeof(child_cwd)) || !child_cwd[0])
      snprintf(child_cwd, sizeof(child_cwd), "%s", root);
    char *json_path_abs = NULL;
    const char *child_json_path = json_path;
    if (json_path && *json_path && json_path[0] != '/') {
      if (asprintf(&json_path_abs, "%s/%s", root, json_path) >= 0 && json_path_abs)
        child_json_path = json_path_abs;
    }
    char *child_argv[8];
    int a = 0;
    child_argv[a++] = g_self_path;
    child_argv[a++] = "selftest";
    child_argv[a++] = "child-tmp-env";
    child_argv[a++] = "--check";
    if (child_json_path && *child_json_path) {
      child_argv[a++] = "--json";
      child_argv[a++] = (char *)child_json_path;
    }
    child_argv[a] = NULL;
    proc_result_t pr = run_proc(child_argv, child_cwd, 15.0);
    if (pr.out && *pr.out) fputs(pr.out, stdout);
    if (pr.err && *pr.err) fputs(pr.err, stderr);
    if (pr.rc != 0 && child_json_path && *child_json_path && !exists_path(child_json_path)) {
      string_list_t rows = {0}, failures = {0};
      (void)string_list_push_take(&rows, native_row_status("child_tmp_env_spawn",
                                                           "selftest-child-tmp-env",
                                                           false, "json_report", child_json_path));
      (void)string_list_push_take(&failures,
                                  make_worker_failure_row("child_tmp_env",
                                                          "selftest-child-tmp-env",
                                                          pr.rc, pr.out, pr.err));
      char *report = build_native_report_json(&rows, &failures,
                                              "selftest-child-tmp-env", "");
      (void)write_file_text(child_json_path, report);
      free(report);
      string_list_free(&rows);
      string_list_free(&failures);
    }
    int rc = pr.rc;
    proc_result_free(&pr);
    free(json_path_abs);
    return rc;
  }

  char *expected_tmp = NULL, *expected_scratch = NULL;
  char *expected_xdg = NULL, *expected_nytrix_cache = NULL;
  bool paths_ok = asprintf(&expected_tmp, "%s/build/cache/tmp", root) >= 0 &&
                  asprintf(&expected_scratch, "%s/build/cache/scratch", root) >= 0 &&
                  asprintf(&expected_xdg, "%s/build/cache/xdg", root) >= 0 &&
                  asprintf(&expected_nytrix_cache, "%s/build/cache/nytrix", root) >= 0;
  string_list_t rows = {0}, failures = {0};
  str_buf_t reason = {0};
  char cwd_buf[4096] = "";
  (void)getcwd(cwd_buf, sizeof(cwd_buf));
  bool ok = paths_ok;
  if (!paths_ok) {
    (void)sb_append(&reason, "expected path allocation failed");
  } else {
    ok &= env_value_matches("PWD", cwd_buf, &reason);
    ok &= env_value_matches("NYNTH_ROOT", root, &reason);
    ok &= env_value_matches("TMPDIR", expected_tmp, &reason);
    ok &= env_value_matches("TMP", expected_tmp, &reason);
    ok &= env_value_matches("TEMP", expected_tmp, &reason);
    ok &= env_value_matches("NYNTH_CHILD_TMPDIR", expected_tmp, &reason);
    ok &= env_value_matches("NYNTH_SCRATCH_ROOT", expected_scratch, &reason);
    ok &= env_value_matches("XDG_CACHE_HOME", expected_xdg, &reason);
    ok &= env_value_matches("NYTRIX_CACHE_DIR", expected_nytrix_cache, &reason);
  }

  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":\"child_tmp_env\",\"kind\":\"selftest-child-tmp-env\",\"ok\":");
  (void)sb_append(&row, ok ? "true" : "false");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\",\"nynth_root\":");
  (void)sb_append_json_str(&row, root);
  (void)sb_append(&row, ",\"cwd\":");
  (void)sb_append_json_str(&row, cwd_buf);
  const char *keys[] = {
    "PWD", "TMPDIR", "TMP", "TEMP", "NYNTH_CHILD_TMPDIR",
    "NYNTH_SCRATCH_ROOT", "XDG_CACHE_HOME", "NYTRIX_CACHE_DIR"
  };
  const char *fields[] = {
    "pwd", "tmpdir", "tmp", "temp", "nynth_child_tmpdir",
    "nynth_scratch_root", "xdg_cache_home", "nytrix_cache_dir"
  };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    (void)sb_append_c(&row, ',');
    (void)sb_append_json_str(&row, fields[i]);
    (void)sb_append_c(&row, ':');
    (void)sb_append_json_str(&row, getenv(keys[i]) ? getenv(keys[i]) : "");
  }
  (void)sb_append(&row, ",\"expected_tmp\":");
  (void)sb_append_json_str(&row, expected_tmp ? expected_tmp : "");
  (void)sb_append(&row, ",\"expected_scratch\":");
  (void)sb_append_json_str(&row, expected_scratch ? expected_scratch : "");
  (void)sb_append(&row, ",\"expected_xdg_cache_home\":");
  (void)sb_append_json_str(&row, expected_xdg ? expected_xdg : "");
  (void)sb_append(&row, ",\"expected_nytrix_cache_dir\":");
  (void)sb_append_json_str(&row, expected_nytrix_cache ? expected_nytrix_cache : "");
  if (!ok) {
    (void)sb_append(&row, ",\"reason\":");
    (void)sb_append_json_str(&row, reason.data ? reason.data : "child env mismatch");
  }
  (void)sb_append_c(&row, '}');
  (void)string_list_push_take(&rows, sb_take(&row));
  if (!ok) {
    (void)string_list_push_take(&failures,
                                make_worker_failure_row("child_tmp_env",
                                                        "selftest-child-tmp-env",
                                                        1, "", reason.data));
  }

  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"nynth_root\":");
  (void)sb_append_json_str(&extra, root);
  (void)sb_append(&extra, ",\"expected_tmp\":");
  (void)sb_append_json_str(&extra, expected_tmp ? expected_tmp : "");
  (void)sb_append(&extra, ",\"expected_nytrix_cache_dir\":");
  (void)sb_append_json_str(&extra, expected_nytrix_cache ? expected_nytrix_cache : "");
  char *report = build_native_report_json(&rows, &failures,
                                          "selftest-child-tmp-env", extra.data);
  int rc = emit_native_report(report, json_path, "child tmp env", rows.count, failures.count);
  free(extra.data);
  free(reason.data);
  free(expected_tmp);
  free(expected_scratch);
  free(expected_xdg);
  free(expected_nytrix_cache);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static int dispatch_worker(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "shape-count") == 0) return cmd_shape_count(argv[2]);
  if (argc == 3 && strcmp(argv[1], "hash-case") == 0) return cmd_hash_case(argv[2]);
  if (argc == 3 && strcmp(argv[1], "scan-ir-markers") == 0) return cmd_scan_ir_markers(argv[2]);
  if (argc == 3 && strcmp(argv[1], "analyze-ir") == 0) return cmd_analyze_ir(argv[2]);
  if (argc == 3 && strcmp(argv[1], "validate-shapes") == 0) return cmd_validate_shapes(argv[2]);
  if (argc == 4 && strcmp(argv[1], "source-shape-counts") == 0) return cmd_source_shape_counts(argv[2], argv[3]);
  if (argc >= 3 && strcmp(argv[1], "generate-batch") == 0) return cmd_generate_batch(argc, argv);
  if (argc >= 3 && strcmp(argv[1], "convert-cbridge") == 0) return cmd_convert_cbridge(argc, argv);
  if (argc >= 3 && strcmp(argv[1], "compare-case") == 0) return cmd_compare_case(argc, argv);
  if (argc >= 3 && strcmp(argv[1], "replay-corpus-entry") == 0) return cmd_replay_corpus_entry(argc, argv);
  return -1;
}

static int cmd_public_shapes_audit(int argc, char **argv) {
  char *default_shape_dir = NULL;
  if (nynth_asprintf(&default_shape_dir, "shapes") < 0) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    return 2;
  }
  const char *shape_dir = value_after(argc, argv, 3, "--shape-dir", default_shape_dir);
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  int count = 0, errors = 0, typed = 0, optimizer = 0, torture = 0, stress = 0, program = 0, rc = 0;
  if (json_path && *json_path) {
    if (!mkdir_parent(json_path)) {
      printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
      json_str(stdout, json_path);
      printf("}\n");
      free(default_shape_dir);
      return 1;
    }
    FILE *f = fopen(json_path, "wb");
    if (!f) {
      printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
      json_str(stdout, json_path);
      printf("}\n");
      free(default_shape_dir);
      return 1;
    }
    rc = validate_shapes_emit_json(shape_dir, f, &count, &errors, &typed, &optimizer, &torture, &stress, &program);
    if (fclose(f) != 0) rc = 1;
    printf("shapes: %d\n", count);
    printf("generators: {\"typed\": %d, \"optimizer\": %d, \"torture\": %d, \"stress\": %d, \"program\": %d}\n",
           typed, optimizer, torture, stress, program);
    printf("errors: %d\n", errors);
    free(default_shape_dir);
    return rc;
  }
  rc = validate_shapes_emit_json(shape_dir, stdout, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  free(default_shape_dir);
  return rc;
}

static int cmd_public_bridge_convert(int argc, char **argv) {
  if (argc < 4) return worker_usage();
  const char *c_path = argv[3];
  const char *out_path = value_after(argc, argv, 4, "--out", "");
  bool emit_json = has_flag_after(argc, argv, 4, "--json");
  cbridge_convert_result_t result = convert_cbridge_file(c_path);
  if (!result.ny_source) {
    if (strcmp(result.error, "read-failed") == 0) {
      printf("{\"ok\":false,\"error\":\"read-failed\",\"source_path\":");
      json_str(stdout, c_path);
      printf("}\n");
      cbridge_convert_result_free(&result);
      return 1;
    }
    printf("{\"ok\":false,\"error\":\"unsupported\",\"reason\":");
    json_str(stdout, result.error[0] ? result.error : "conversion failed");
    printf(",\"engine\":\"nynth_core\",\"source_path\":");
    json_str(stdout, c_path);
    printf(",\"line\":%d,\"diagnostic_category\":", result.error_line);
    json_str(stdout, result.error_category[0] ? result.error_category : "unsupported");
    printf("}\n");
    cbridge_convert_result_free(&result);
    return 3;
  }
  if (out_path && *out_path && !write_file_text(out_path, result.ny_source)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, out_path);
    printf("}\n");
    cbridge_convert_result_free(&result);
    return 1;
  }
  if (emit_json) {
    printf("{\"ok\":true,\"engine\":\"nynth_core\",\"source_path\":");
    json_str(stdout, c_path);
    printf(",\"features\":");
    print_cbridge_features_json(stdout, &result);
    printf(",\"ny_source\":");
    json_str(stdout, result.ny_source);
    if (out_path && *out_path) {
      printf(",\"ny_source_path\":");
      json_str(stdout, out_path);
    }
    printf(",\"worker_ms\":%.2f}\n", result.worker_ms);
  } else {
    fputs(result.ny_source, stdout);
  }
  cbridge_convert_result_free(&result);
  return 0;
}

static int cmd_public_bridge_compare(int argc, char **argv) {
  if (argc < 4) return worker_usage();
  char root[4096], ny_bin[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  if (!find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-binary-not-found\",\"reason\":\"run ./make ny first or set NYTRIX_NY_BIN\"}\n");
    return 2;
  }
  const char *c_path = argv[3];
  double timeout_s = atof(value_after(argc, argv, 4, "--timeout-s", "60"));
  char case_name[256];
  stem_name(c_path, case_name, sizeof(case_name));
  char *out_dir = NULL, *bin_dir = NULL, *ny_path = NULL;
  bool paths_ok = nynth_asprintf(&out_dir, "build/generated/cbridge") >= 0 &&
                  nynth_asprintf(&bin_dir, "build/cbridge/%s", case_name) >= 0 &&
                  asprintf(&ny_path, "%s/%s.ny", out_dir, case_name) >= 0;
  if (!paths_ok) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(out_dir); free(bin_dir); free(ny_path);
    return 2;
  }
  if (!mkdir_p(out_dir) || !mkdir_p(bin_dir)) {
    printf("{\"ok\":false,\"error\":\"prepare-failed\",\"reason\":\"mkdir failed\"}\n");
    free(out_dir); free(bin_dir); free(ny_path);
    return 1;
  }
  cbridge_convert_result_t result = convert_cbridge_file(c_path);
  if (!result.ny_source) {
    if (strcmp(result.error, "read-failed") == 0) {
      printf("{\"ok\":false,\"error\":\"read-failed\",\"source_path\":");
      json_str(stdout, c_path);
      printf("}\n");
      cbridge_convert_result_free(&result);
      free(out_dir); free(bin_dir); free(ny_path);
      return 1;
    }
    printf("{\"ok\":false,\"error\":\"unsupported\",\"reason\":");
    json_str(stdout, result.error[0] ? result.error : "conversion failed");
    printf(",\"engine\":\"nynth_core\",\"source_path\":");
    json_str(stdout, c_path);
    printf(",\"line\":%d,\"diagnostic_category\":", result.error_line);
    json_str(stdout, result.error_category[0] ? result.error_category : "unsupported");
    printf("}\n");
    cbridge_convert_result_free(&result);
    free(out_dir); free(bin_dir); free(ny_path);
    return 3;
  }
  if (!write_file_text(ny_path, result.ny_source)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, ny_path);
    printf("}\n");
    cbridge_convert_result_free(&result);
    free(out_dir); free(bin_dir); free(ny_path);
    return 1;
  }
  int rc = native_compare_case_with_features(case_name, c_path, ny_path, "", root, ny_bin,
                                             bin_dir, timeout_s, 1, 0,
                                             result.features, result.feature_count);
  cbridge_convert_result_free(&result);
  free(out_dir); free(bin_dir); free(ny_path);
  return rc;
}

static char *build_bridge_suite_report_json(const report_rows_t *report, const char *seed_dir,
                                            bool fast, double timeout_s) {
  str_buf_t b = {0};
  int cases = report->rows.count;
  int ok_cases = cases - report->failed_rows;
  if (ok_cases < 0) ok_cases = 0;
  (void)sb_append(&b, "{\"rows\":");
  append_rows_json(&b, &report->rows);
  (void)sb_append(&b, ",\"failures\":[");
  if (report->failures_json.data) (void)sb_append(&b, report->failures_json.data);
  (void)sb_append(&b, "],\"summary\":{");
  (void)sb_appendf(&b, "\"cases\":%d,\"ok_cases\":%d,\"failure_count\":%d,", cases, ok_cases, report->failure_count);
  (void)sb_append(&b, "\"engine\":\"nynth_core\",\"native_workers\":{");
  (void)sb_appendf(&b, "\"native_cbridge\":%d,\"native_compare\":%d},",
                   cases, cases);
  (void)sb_appendf(&b, "\"native_overhead_ms\":{\"bridge_suite_workers\":%.2f},", report->worker_ms);
  (void)sb_append(&b, "\"ratio_stats\":{");
  append_ratio_stats_json(&b, "ny_o3_vs_c_o3_run", &report->ny_o3_run);
  (void)sb_append_c(&b, ',');
  append_ratio_stats_json(&b, "ny_o3i_vs_c_o3_run", &report->ny_o3i_run);
  (void)sb_append(&b, "}},\"meta\":{\"seed_dir\":");
  (void)sb_append_json_str(&b, seed_dir);
  (void)sb_appendf(&b, ",\"fast\":%s,\"timeout_s\":%.2f,\"engine\":\"nynth_core\",", fast ? "true" : "false", timeout_s);
  (void)sb_append(&b, "\"comparison_contract\":{\"correctness\":\"all C O0/O3 and Ny O0/O3/O3i outputs must match\",\"runtime\":\"run_ms is median wall time after warmup\",\"run_repeats\":1,\"warmup\":0}}}");
  return sb_take(&b);
}

static void print_bridge_suite_human(const report_rows_t *report) {
  printf("cases: %d\n", report->rows.count);
  printf("failures: %d\n", report->failure_count);
  if (report->ny_o3_run.count) {
    printf("ny_o3_vs_c_o3_run: avg=%.4f min=%.4f(%s) max=%.4f(%s) fast_or_equal=%d/%d\n",
           report->ny_o3_run.sum / (double)report->ny_o3_run.count,
           report->ny_o3_run.min, report->ny_o3_run.min_case,
           report->ny_o3_run.max, report->ny_o3_run.max_case,
           report->ny_o3_run.faster_or_equal, report->ny_o3_run.count);
  }
  for (int i = 0; i < report->rows.count; ++i) {
    char *case_name = extract_case_name_from_row(report->rows.items[i]);
    printf("%s\n", case_name && *case_name ? case_name : "(unknown)");
    free(case_name);
  }
}

static int cmd_public_bridge_suite(int argc, char **argv) {
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  bool fast = has_flag_after(argc, argv, 3, "--fast");
  bool emit_json = has_flag_after(argc, argv, 3, "--json");
  const char *report_path = value_after(argc, argv, 3, "--report-json", "");
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "60"));
  string_list_t seeds = {0};
  char *seed_dir = NULL;
  if (!collect_baked_bridge_seeds(root, &seed_dir, &seeds)) {
    printf("{\"ok\":false,\"error\":\"seed-dir-read-failed\",\"seed_dir\":");
    json_str(stdout, seed_dir ? seed_dir : "shapes/tests");
    printf("}\n");
    free(seed_dir);
    return 1;
  }
  int limit = seeds.count;
  if (fast && limit > 3) limit = 3;
  report_rows_t report;
  memset(&report, 0, sizeof(report));
  char timeout_buf[64];
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  double outer_timeout = worker_outer_timeout(timeout_s, 1, 0);
  for (int i = 0; i < limit; ++i) {
    char case_name[256];
    stem_name(seeds.items[i], case_name, sizeof(case_name));
    char *worker_argv[] = {
      g_self_path, "bridge", "compare", seeds.items[i], "--timeout-s", timeout_buf, NULL
    };
    proc_result_t pr = run_proc(worker_argv, root, outer_timeout);
    report.worker_ms += pr.elapsed_ms;
    char *row = NULL;
    if (pr.rc == 0 && pr.out && strstr(pr.out, "\"failures\"")) {
      row = trim_trailing_copy(pr.out);
    } else {
      row = make_worker_failure_row(case_name, "bridge-compare", pr.rc, pr.out, pr.err);
    }
    report_add_row(&report, row);
    proc_result_free(&pr);
  }
  char *report_json = build_bridge_suite_report_json(&report, seed_dir, fast, timeout_s);
  int rc = report.failure_count ? 1 : 0;
  if (report_path && *report_path && !write_file_text(report_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, report_path);
    printf("}\n");
    rc = 1;
  }
  if (emit_json) {
    puts(report_json);
  } else {
    print_bridge_suite_human(&report);
  }
  free(report_json);
  report_rows_free(&report);
  string_list_free(&seeds);
  free(seed_dir);
  return rc;
}

static const bridge_kernel_spec_t bridge_kernel_specs[] = {
  {"compact_control", "compact-control", "fixed-width,compact-if,compound-array,decrement-loop"},
  {"branch_mix", "branches", "branches,integer-arithmetic,call"},
  {"array_scan", "arrays", "arrays,nested-loops,mutation,set_idx"},
  {"string_walk", "strings", "strings,load8,branches,nested-loops"},
  {"hash_probe", "hash-style-probes", "arrays,probes,branches,xor"},
  {"reduction_tree", "reduction-tree", "arrays,small-functions,reduction,branches"},
  {"histogram_update", "histogram-update", "histogram,repeated-set,small-array,integer-arithmetic"},
};

static const bridge_kernel_spec_t *bridge_kernel_by_name(const char *name) {
  for (size_t i = 0; i < sizeof(bridge_kernel_specs) / sizeof(bridge_kernel_specs[0]); ++i) {
    if (strcmp(bridge_kernel_specs[i].name, name) == 0) return &bridge_kernel_specs[i];
  }
  return &bridge_kernel_specs[0];
}

static const bridge_kernel_spec_t *choose_bridge_kernel(int idx, const char *profile, bool fast) {
  int n = (int)(sizeof(bridge_kernel_specs) / sizeof(bridge_kernel_specs[0]));
  if (fast) return &bridge_kernel_specs[idx % n];
  if (profile && strcmp(profile, "strings") == 0 && idx % 3 == 0) return bridge_kernel_by_name("string_walk");
  if (profile && strcmp(profile, "memory") == 0 && idx % 3 == 0) return bridge_kernel_by_name("array_scan");
  if (profile && strcmp(profile, "optimizer") == 0 && idx % 3 == 0) return bridge_kernel_by_name("reduction_tree");
  return &bridge_kernel_specs[idx % n];
}

static bool emit_bridge_kernel_source(const char *path, const bridge_kernel_spec_t *spec, int seed) {
  str_buf_t s = {0};
  if (strcmp(spec->name, "compact_control") == 0) {
    (void)sb_append(&s,
      "#include <stdio.h>\n"
      "#include <stdint.h>\n\n"
      "int main(void) {\n"
      "    int xs[8] = {5, 7, 11, 13, 17, 19, 23, 29};\n"
      "    int32_t checksum = 200;\n"
      "    for(int i = 7; i >= 0; i--) {\n"
      "        int slot = i % 8;\n"
      "        xs[slot] += i;\n"
      "        if(i > 3) { checksum += xs[slot]; } else { checksum -= i; }\n"
      "    }\n"
      "    printf(\"%d\\n\", checksum);\n"
      "    return 0;\n"
      "}\n");
  } else if (strcmp(spec->name, "branch_mix") == 0) {
    (void)sb_appendf(&s,
      "#include <stdio.h>\n\n"
      "int mix(int x) {\n"
      "    return (x * %d + %d) %% 1009;\n"
      "}\n\n"
      "int main(void) {\n"
      "    int checksum = 0;\n"
      "    for(int i = 0; i < 160; i++) {\n"
      "        int v = mix(i);\n"
      "        if((v %% 5) < 2) {\n"
      "            checksum += v;\n"
      "        } else {\n"
      "            checksum -= v %% 17;\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", checksum);\n"
      "    return 0;\n"
      "}\n", 37 + seed % 5, 11 + seed % 7);
  } else if (strcmp(spec->name, "array_scan") == 0) {
    (void)sb_append(&s, "#include <stdio.h>\n\nint main(void) {\n    int data[24] = {");
    for (int i = 0; i < 24; ++i) {
      if (i) (void)sb_append(&s, ", ");
      (void)sb_appendf(&s, "%d", ((i * 17 + seed * 3) % 97) + 1);
    }
    (void)sb_append(&s,
      "};\n    int checksum = 0;\n"
      "    for(int r = 0; r < 32; r++) {\n"
      "        for(int i = 0; i < 24; i++) {\n"
      "            int v = data[i];\n"
      "            checksum += (v * (i + 3)) % 997;\n"
      "            data[i] = v + (checksum % 11);\n"
      "        }\n"
      "    }\n"
      "    printf(\"%d\\n\", checksum);\n"
      "    return 0;\n"
      "}\n");
  } else if (strcmp(spec->name, "string_walk") == 0) {
    (void)sb_appendf(&s,
      "#include <stdio.h>\n\n"
      "int main(void) {\n"
      "    const char *text = \"nytrix-state-machine-compiler-lab-%d\";\n"
      "    int n = %d;\n"
      "    int checksum = 0;\n"
      "    for(int r = 0; r < 24; r++) {\n"
      "        for(int i = 0; i < n; i++) {\n"
      "            int c = text[i];\n"
      "            if((c %% 7) == (i %% 7)) {\n"
      "                checksum += c + r;\n"
      "            } else {\n"
      "                checksum += c %% 13;\n"
      "            }\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", checksum);\n"
      "    return 0;\n"
      "}\n", seed, 34 + (seed >= 10 ? (seed >= 100 ? (seed >= 1000 ? 4 : 3) : 2) : 1));
  } else if (strcmp(spec->name, "hash_probe") == 0) {
    (void)sb_append(&s, "#include <stdio.h>\n\nint main(void) {\n    int keys[32] = {");
    for (int i = 0; i < 32; ++i) {
      if (i) (void)sb_append(&s, ", ");
      (void)sb_appendf(&s, "%d", (i * 37 + seed) % 127);
    }
    (void)sb_append(&s, "};\n    int vals[32] = {");
    for (int i = 0; i < 32; ++i) {
      if (i) (void)sb_append(&s, ", ");
      (void)sb_appendf(&s, "%d", (i * 19 + seed * 2) % 211);
    }
    (void)sb_appendf(&s,
      "};\n    int checksum = 0;\n"
      "    for(int q = 0; q < 96; q++) {\n"
      "        int needle = (q * 53 + %d) %% 127;\n"
      "        for(int i = 0; i < 32; i++) {\n"
      "            if(keys[i] == needle) {\n"
      "                checksum += vals[i] + q;\n"
      "            } else {\n"
      "                checksum += (keys[i] ^ needle) %% 5;\n"
      "            }\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", checksum);\n"
      "    return 0;\n"
      "}\n", seed);
  } else if (strcmp(spec->name, "reduction_tree") == 0) {
    (void)sb_append(&s,
      "#include <stdio.h>\n\n"
      "int fold0(int a, int b) {\n"
      "    return (a * 3 + b * 5 + 17) % 65521;\n"
      "}\n\n"
      "int fold1(int a, int b) {\n"
      "    return (fold0(a, b) ^ (a + b)) % 65521;\n"
      "}\n\n"
      "int main(void) {\n    int data[40] = {");
    for (int i = 0; i < 40; ++i) {
      if (i) (void)sb_append(&s, ", ");
      (void)sb_appendf(&s, "%d", ((i * 31 + seed * 7) % 251) + 1);
    }
    (void)sb_append(&s,
      "};\n    int checksum = 0;\n"
      "    for(int r = 0; r < 36; r++) {\n"
      "        int acc = r + 1;\n"
      "        for(int i = 0; i < 40; i++) {\n"
      "            acc = fold1(acc, data[i] + i);\n"
      "            if((acc % 9) == (i % 9)) {\n"
      "                checksum += acc;\n"
      "            } else {\n"
      "                checksum += acc % 31;\n"
      "            }\n"
      "        }\n"
      "    }\n"
      "    printf(\"%d\\n\", checksum);\n"
      "    return 0;\n"
      "}\n");
  } else {
    (void)sb_appendf(&s,
      "#include <stdio.h>\n\n"
      "int main(void) {\n"
      "    int hist[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};\n"
      "    int checksum = 0;\n"
      "    for(int r = 0; r < 45; r++) {\n"
      "        for(int i = 0; i < 96; i++) {\n"
      "            int v = (i * 29 + r * 17 + %d) %% 251;\n"
      "            int bucket = v %% 16;\n"
      "            int old = hist[bucket];\n"
      "            hist[bucket] = (old + v + i) %% 1000003;\n"
      "            checksum = checksum + hist[bucket] %% 97;\n"
      "        }\n"
      "    }\n"
      "    for(int i = 0; i < 16; i++) {\n"
      "        checksum = checksum + hist[i] * (i + 3);\n"
      "    }\n"
      "    printf(\"%%d\\n\", checksum);\n"
      "    return 0;\n"
      "}\n", seed % 37);
  }
  char *text = sb_take(&s);
  bool ok = write_file_text(path, text);
  free(text);
  return ok;
}

static char *features_json_from_csv(const char *csv) {
  string_list_t features = {0};
  string_list_push_csv_unique(&features, csv);
  qsort(features.items, (size_t)features.count, sizeof(char *), cmp_cstr);
  str_buf_t b = {0};
  append_string_list_json(&b, &features);
  string_list_free(&features);
  return sb_take(&b);
}

static char *cbridge_features_csv(const cbridge_convert_result_t *result) {
  str_buf_t b = {0};
  for (int i = 0; i < result->feature_count; ++i) {
    if (i) (void)sb_append_c(&b, ',');
    (void)sb_append(&b, result->features[i]);
  }
  return sb_take(&b);
}

static char *row_with_bridge_generated_fields(const char *row, const bridge_kernel_spec_t *spec,
                                              int seed, const char *kernel_features_json,
                                              const char *bridge_features_json) {
  size_t n = strlen(row ? row : "");
  while (n > 0 && isspace((unsigned char)row[n - 1])) --n;
  if (n == 0 || row[n - 1] != '}') return strdup(row ? row : "");
  str_buf_t b = {0};
  (void)sb_append_n(&b, row, n - 1);
  (void)sb_append(&b, ",\"shape\":");
  (void)sb_append_json_str(&b, spec->shape);
  (void)sb_appendf(&b, ",\"seed\":%d,\"kernel_features\":", seed);
  (void)sb_append(&b, kernel_features_json ? kernel_features_json : "[]");
  (void)sb_append(&b, ",\"bridge_features\":");
  (void)sb_append(&b, bridge_features_json ? bridge_features_json : "[]");
  (void)sb_append(&b, ",\"source_kind\":\"generated-c-bridge\",\"reducer_mode\":\"cbridge_generated\"}");
  return sb_take(&b);
}

static char *build_bridge_generate_report_json(const report_rows_t *report,
                                               int generated, const char *profile,
                                               int seed, bool fast, int runs,
                                               int warmup, double timeout_s,
                                               const char *out_dir, const char *bin_dir) {
  str_buf_t b = {0};
  int ok_cases = report->rows.count - report->failed_rows;
  if (ok_cases < 0) ok_cases = 0;
  (void)sb_append(&b, "{\"rows\":");
  append_rows_json(&b, &report->rows);
  (void)sb_append(&b, ",\"failures\":[");
  if (report->failures_json.data) (void)sb_append(&b, report->failures_json.data);
  (void)sb_appendf(&b, "],\"summary\":{\"cases\":%d,\"ok_cases\":%d,\"generated_kernels\":%d,"
                   "\"failure_count\":%d,\"profile\":",
                   report->rows.count, ok_cases, generated, report->failure_count);
  (void)sb_append_json_str(&b, profile);
  (void)sb_appendf(&b, ",\"seed\":%d,\"fast\":%s,\"runs\":%d,\"warmup\":%d,"
                   "\"engine\":\"nynth_core\",\"native_workers\":{"
                   "\"native_cbridge\":%d,\"native_compare\":%d},",
                   seed, fast ? "true" : "false", runs, warmup, generated, report->rows.count);
  (void)sb_appendf(&b, "\"native_overhead_ms\":{\"bridge_generate_workers\":%.2f},\"ratio_stats\":{",
                   report->worker_ms);
  append_ratio_stats_json(&b, "ny_o3i_vs_c_o3_run", &report->ny_o3i_run);
  (void)sb_append(&b, "}},\"meta\":{\"out_dir\":");
  (void)sb_append_json_str(&b, out_dir);
  (void)sb_append(&b, ",\"build_dir\":");
  (void)sb_append_json_str(&b, bin_dir);
  (void)sb_appendf(&b, ",\"timeout_s\":%.2f,\"fast\":%s,\"seed\":%d,\"profile\":",
                   timeout_s, fast ? "true" : "false", seed);
  (void)sb_append_json_str(&b, profile);
  (void)sb_appendf(&b, ",\"runs\":%d,\"warmup\":%d,\"engine\":\"nynth_core\"}}", runs, warmup);
  return sb_take(&b);
}

static int cmd_public_bridge_generate(int argc, char **argv) {
  char root[4096], ny_bin[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  if (!find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-binary-not-found\",\"reason\":\"run ./make ny first or set NYTRIX_NY_BIN\"}\n");
    return 2;
  }
  bool fast = has_flag_after(argc, argv, 3, "--fast");
  int cases = atoi(value_after(argc, argv, 3, "--cases", "5"));
  if (cases < 1) cases = 1;
  int seed = atoi(value_after(argc, argv, 3, "--seed", "1337"));
  int runs = atoi(value_after(argc, argv, 3, "--runs", "5"));
  if (runs < 1) runs = 1;
  int warmup = atoi(value_after(argc, argv, 3, "--warmup", "1"));
  if (warmup < 0) warmup = 0;
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "60"));
  const char *profile = value_after(argc, argv, 3, "--profile", "balanced");
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  char *out_dir = NULL, *ny_dir = NULL, *bin_dir = NULL;
  bool path_ok = nynth_asprintf(&out_dir, "build/generated/cbridge/native/%s_%d", profile, seed) >= 0 &&
                 asprintf(&ny_dir, "%s/ny", out_dir) >= 0 &&
                 nynth_asprintf(&bin_dir, "build/cbridge/generated/%s_%d", profile, seed) >= 0;
  if (!path_ok || !mkdir_p(out_dir) || !mkdir_p(ny_dir) || !mkdir_p(bin_dir)) {
    printf("{\"ok\":false,\"error\":\"prepare-failed\"}\n");
    free(out_dir); free(ny_dir); free(bin_dir);
    return 1;
  }
  if (fast) {
    int spec_count = (int)(sizeof(bridge_kernel_specs) / sizeof(bridge_kernel_specs[0]));
    if (cases > spec_count) cases = spec_count;
  }
  report_rows_t report;
  memset(&report, 0, sizeof(report));
  char timeout_buf[64], runs_buf[32], warmup_buf[32];
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  snprintf(runs_buf, sizeof(runs_buf), "%d", runs);
  snprintf(warmup_buf, sizeof(warmup_buf), "%d", warmup);
  double outer_timeout = worker_outer_timeout(timeout_s, runs, warmup);
  int generated = 0;
  for (int i = 0; i < cases; ++i) {
    const bridge_kernel_spec_t *spec = choose_bridge_kernel(i, profile, fast);
    int case_seed = seed + i * 17 + 1;
    char case_name[160];
    snprintf(case_name, sizeof(case_name), "gen_%s_%03d_%d", spec->name, i, case_seed);
    char *c_path = NULL, *ny_path = NULL, *case_bin_dir = NULL;
    bool case_paths = asprintf(&c_path, "%s/%s.c", out_dir, case_name) >= 0 &&
                      asprintf(&ny_path, "%s/%s.ny", ny_dir, case_name) >= 0 &&
                      asprintf(&case_bin_dir, "%s/%s", bin_dir, case_name) >= 0;
    if (!case_paths || !emit_bridge_kernel_source(c_path, spec, case_seed)) {
      char *row = make_worker_failure_row(case_name, "bridge-generate", 1, "", "kernel write failed");
      report_add_row(&report, row);
      free(c_path); free(ny_path); free(case_bin_dir);
      continue;
    }
    generated++;
    cbridge_convert_result_t result = convert_cbridge_file(c_path);
    if (!result.ny_source || !write_file_text(ny_path, result.ny_source)) {
      char *row = make_worker_failure_row(case_name, "bridge-convert", 3, "", result.error[0] ? result.error : "conversion failed");
      report_add_row(&report, row);
      cbridge_convert_result_free(&result);
      free(c_path); free(ny_path); free(case_bin_dir);
      continue;
    }
    string_list_t feature_list = {0};
    string_list_push_csv_unique(&feature_list, spec->features_csv);
    for (int f = 0; f < result.feature_count; ++f)
      (void)string_list_push_unique_copy(&feature_list, result.features[f]);
    qsort(feature_list.items, (size_t)feature_list.count, sizeof(char *), cmp_cstr);
    str_buf_t feature_csv = {0};
    for (int f = 0; f < feature_list.count; ++f) {
      if (f) (void)sb_append_c(&feature_csv, ',');
      (void)sb_append(&feature_csv, feature_list.items[f]);
    }
    char *kernel_features_json = features_json_from_csv(spec->features_csv);
    char *bridge_csv = cbridge_features_csv(&result);
    char *bridge_features_json = features_json_from_csv(bridge_csv);
    char *cmp_argv[36];
    int ca = 0;
    cmp_argv[ca++] = g_self_path;
    cmp_argv[ca++] = "compare-case";
    cmp_argv[ca++] = "--case"; cmp_argv[ca++] = case_name;
    cmp_argv[ca++] = "--c"; cmp_argv[ca++] = c_path;
    cmp_argv[ca++] = "--ny"; cmp_argv[ca++] = ny_path;
    cmp_argv[ca++] = "--root"; cmp_argv[ca++] = root;
    cmp_argv[ca++] = "--ny-bin"; cmp_argv[ca++] = ny_bin;
    cmp_argv[ca++] = "--bin-dir"; cmp_argv[ca++] = case_bin_dir;
    cmp_argv[ca++] = "--timeout-s"; cmp_argv[ca++] = timeout_buf;
    cmp_argv[ca++] = "--runs"; cmp_argv[ca++] = runs_buf;
    cmp_argv[ca++] = "--warmup"; cmp_argv[ca++] = warmup_buf;
    cmp_argv[ca++] = "--features"; cmp_argv[ca++] = feature_csv.data ? feature_csv.data : "";
    cmp_argv[ca] = NULL;
    proc_result_t pr = run_proc(cmp_argv, root, outer_timeout);
    report.worker_ms += pr.elapsed_ms;
    char *row = NULL;
    if (pr.rc == 0 && pr.out && strstr(pr.out, "\"failures\"")) {
      char *trimmed = trim_trailing_copy(pr.out);
      row = row_with_bridge_generated_fields(trimmed, spec, case_seed, kernel_features_json, bridge_features_json);
      free(trimmed);
    } else {
      row = make_worker_failure_row(case_name, "compare-case", pr.rc, pr.out, pr.err);
    }
    report_add_row(&report, row);
    proc_result_free(&pr);
    free(kernel_features_json); free(bridge_csv); free(bridge_features_json);
    free(feature_csv.data);
    string_list_free(&feature_list);
    cbridge_convert_result_free(&result);
    free(c_path); free(ny_path); free(case_bin_dir);
  }
  char *report_json = build_bridge_generate_report_json(&report, generated, profile, seed, fast,
                                                        runs, warmup, timeout_s, out_dir, bin_dir);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    print_bridge_suite_human(&report);
  }
  int rc = report.failure_count ? 1 : 0;
  free(report_json);
  report_rows_free(&report);
  free(out_dir); free(ny_dir); free(bin_dir);
  return rc;
}

static double nth_root_newton(double value, int n) {
  if (value <= 0.0 || n <= 0) return 0.0;
  if (n == 1) return value;
  double y = value > 1.0 ? value : 1.0;
  for (int iter = 0; iter < 32; ++iter) {
    double denom = 1.0;
    for (int i = 0; i < n - 1; ++i) denom *= y;
    if (denom <= 0.0) break;
    y = (((double)n - 1.0) * y + value / denom) / (double)n;
  }
  return y;
}

static double geomean_matrix_rows(const matrix_row_list_t *rows) {
  if (rows->count <= 0) return 0.0;
  double product = 1.0;
  for (int i = 0; i < rows->count; ++i) {
    if (rows->items[i].ratio <= 0.0) return 0.0;
    product *= rows->items[i].ratio;
  }
  return nth_root_newton(product, rows->count);
}

static void append_matrix_items_json(str_buf_t *b, const matrix_row_list_t *rows, int limit) {
  (void)sb_append_c(b, '[');
  int n = rows->count < limit ? rows->count : limit;
  for (int i = 0; i < n; ++i) {
    if (i) (void)sb_append_c(b, ',');
    (void)sb_append(b, "{\"case\":");
    (void)sb_append_json_str(b, rows->items[i].case_name ? rows->items[i].case_name : "");
    (void)sb_append(b, ",\"shape\":");
    (void)sb_append_json_str(b, rows->items[i].shape ? rows->items[i].shape : "");
    (void)sb_appendf(b, ",\"ratio\":%.4f,\"features\":", rows->items[i].ratio);
    (void)sb_append(b, rows->items[i].features_json ? rows->items[i].features_json : "[]");
    (void)sb_append(b, ",\"report\":");
    (void)sb_append_json_str(b, rows->items[i].report ? rows->items[i].report : "");
    (void)sb_append(b, ",\"next_action\":\"inspect generated C/Ny pair and specialize the hot translated shape\"}");
  }
  (void)sb_append_c(b, ']');
}

static bool collect_matrix_rows_from_report(const char *path, const char *key,
                                            matrix_row_list_t *measured,
                                            int *case_count, int *failure_count,
                                            double *geo_out) {
  file_buf_t f = {0};
  if (!read_file(path, &f)) return false;
  const char *rows = json_value_after_key(f.data, "rows");
  const char *end = rows && *rows == '[' ? matching_json_end(rows, '[', ']') : NULL;
  const char *p = rows && end ? rows + 1 : NULL;
  matrix_row_list_t report_rows = {0};
  while (p && p < end) {
    p = skip_ws_const(p);
    if (p >= end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '{') break;
    const char *obj_end = matching_json_end(p, '{', '}');
    if (!obj_end || obj_end > end) break;
    char *row = strndup_local(p, (size_t)(obj_end - p + 1));
    (*case_count)++;
    if (json_failures_nonempty(row)) {
      (*failure_count)++;
      free(row);
      p = obj_end + 1;
      continue;
    }
    double ratio = 0.0;
    if (extract_json_number(row, key, &ratio)) {
      matrix_row_t item;
      memset(&item, 0, sizeof(item));
      item.case_name = json_string_or_empty(row, "case");
      item.shape = json_string_or_empty(row, "shape");
      item.features_json = json_array_or_empty(row, "features");
      item.report = strdup(path);
      item.ratio = ratio;
      (void)matrix_row_list_push_take(measured, item);
      matrix_row_t copy;
      memset(&copy, 0, sizeof(copy));
      copy.case_name = strdup(item.case_name ? item.case_name : "");
      copy.shape = strdup(item.shape ? item.shape : "");
      copy.features_json = strdup(item.features_json ? item.features_json : "[]");
      copy.report = strdup(item.report ? item.report : "");
      copy.ratio = ratio;
      (void)matrix_row_list_push_take(&report_rows, copy);
    }
    free(row);
    p = obj_end + 1;
  }
  *geo_out = geomean_matrix_rows(&report_rows);
  matrix_row_list_free(&report_rows);
  free(f.data);
  return true;
}

static char *build_perf_matrix_summary_json(char **paths, int path_count, const char *key) {
  matrix_row_list_t measured = {0};
  int cases = 0, failures = 0, faster = 0, slower = 0;
  double *report_geos = (double *)calloc((size_t)path_count, sizeof(double));
  for (int i = 0; i < path_count; ++i) {
    double geo = 0.0;
    if (!collect_matrix_rows_from_report(paths[i], key, &measured, &cases, &failures, &geo))
      failures++;
    report_geos[i] = geo;
  }
  double sum = 0.0;
  for (int i = 0; i < measured.count; ++i) {
    sum += measured.items[i].ratio;
    if (measured.items[i].ratio <= 1.0) faster++;
    else slower++;
  }
  double geo = geomean_matrix_rows(&measured);
  matrix_row_list_t best = {0}, worst = {0};
  for (int i = 0; i < measured.count; ++i) {
    matrix_row_t a = {
      strdup(measured.items[i].case_name ? measured.items[i].case_name : ""),
      strdup(measured.items[i].shape ? measured.items[i].shape : ""),
      strdup(measured.items[i].features_json ? measured.items[i].features_json : "[]"),
      strdup(measured.items[i].report ? measured.items[i].report : ""),
      measured.items[i].ratio
    };
    matrix_row_t b = {
      strdup(measured.items[i].case_name ? measured.items[i].case_name : ""),
      strdup(measured.items[i].shape ? measured.items[i].shape : ""),
      strdup(measured.items[i].features_json ? measured.items[i].features_json : "[]"),
      strdup(measured.items[i].report ? measured.items[i].report : ""),
      measured.items[i].ratio
    };
    (void)matrix_row_list_push_take(&best, a);
    (void)matrix_row_list_push_take(&worst, b);
  }
  qsort(best.items, (size_t)best.count, sizeof(best.items[0]), matrix_row_cmp_ratio_asc);
  qsort(worst.items, (size_t)worst.count, sizeof(worst.items[0]), matrix_row_cmp_ratio_desc);
  double paired_delta = 0.0;
  bool has_paired_delta = path_count == 2 && report_geos[0] > 0.0 && report_geos[1] > 0.0;
  if (has_paired_delta) paired_delta = (report_geos[1] / report_geos[0] - 1.0) * 100.0;
  str_buf_t out = {0};
  (void)sb_append(&out, "{\"reports\":[");
  for (int i = 0; i < path_count; ++i) {
    if (i) (void)sb_append_c(&out, ',');
    (void)sb_append_json_str(&out, paths[i]);
  }
  (void)sb_append(&out, "],\"report_meta\":{},\"comparable_meta\":true,\"meta_warning\":\"\",");
  (void)sb_append(&out, "\"ratio_key\":");
  (void)sb_append_json_str(&out, key);
  (void)sb_appendf(&out, ",\"cases\":%d,\"measured\":%d,\"failures\":%d,"
                   "\"ny_faster_or_equal\":%d,\"ny_slower\":%d,\"geomean\":%.4f,"
                   "\"avg\":%.4f,\"report_geomeans\":{",
                   cases, measured.count, failures, faster, slower, geo,
                   measured.count ? sum / (double)measured.count : 0.0);
  for (int i = 0; i < path_count; ++i) {
    if (i) (void)sb_append_c(&out, ',');
    (void)sb_append_json_str(&out, paths[i]);
    (void)sb_appendf(&out, ":%.4f", report_geos[i]);
  }
  (void)sb_append(&out, "},\"paired_geomean_delta_pct\":");
  if (has_paired_delta) (void)sb_appendf(&out, "%.2f", paired_delta);
  else (void)sb_append(&out, "null");
  (void)sb_append(&out, ",\"paired_report_ratio_geomean_delta_pct\":");
  if (has_paired_delta) (void)sb_appendf(&out, "%.2f", paired_delta);
  else (void)sb_append(&out, "null");
  (void)sb_append(&out, ",\"paired_ny_run_geomean_delta_pct\":null,\"paired_c_run_geomean_delta_pct\":null,\"best\":");
  append_matrix_items_json(&out, &best, 8);
  (void)sb_append(&out, ",\"worst\":");
  append_matrix_items_json(&out, &worst, 8);
  (void)sb_append(&out, ",\"helper_totals\":{},\"helper_delta\":{},\"paired_deltas\":[],"
                  "\"paired_regressions\":[],\"paired_improvements\":[],\"features\":[],"
                  "\"engine\":\"nynth_core\"}");
  free(report_geos);
  matrix_row_list_free(&measured);
  matrix_row_list_free(&best);
  matrix_row_list_free(&worst);
  return sb_take(&out);
}

static int cmd_public_bridge_perf_matrix(int argc, char **argv) {
  if (argc < 4) return worker_usage();
  const char *key = value_after(argc, argv, 3, "--key", "ny_o3i_vs_c_o3_run");
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  char **paths = (char **)calloc((size_t)argc, sizeof(char *));
  int path_count = 0;
  for (int i = 3; i < argc; ++i) {
    if (strcmp(argv[i], "--key") == 0 || strcmp(argv[i], "--json") == 0) {
      ++i;
      continue;
    }
    if (strncmp(argv[i], "--key=", 6) == 0 ||
        strncmp(argv[i], "--json=", 7) == 0)
      continue;
    paths[path_count++] = argv[i];
  }
  if (path_count <= 0) {
    free(paths);
    return worker_usage();
  }
  char *summary = build_perf_matrix_summary_json(paths, path_count, key);
  double geomean = 0.0, measured = 0.0, cases = 0.0, faster = 0.0;
  (void)extract_json_number(summary, "geomean", &geomean);
  (void)extract_json_number(summary, "measured", &measured);
  (void)extract_json_number(summary, "cases", &cases);
  (void)extract_json_number(summary, "ny_faster_or_equal", &faster);
  if (json_path && *json_path && !write_file_text(json_path, summary)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
    free(summary); free(paths);
    return 1;
  }
  printf("cases=%.0f measured=%.0f ny<=c=%.0f geomean=%.4f\n", cases, measured, faster, geomean);
  if (!json_path || !*json_path) puts(summary);
  double failure_count = 0.0;
  (void)extract_json_number(summary, "failures", &failure_count);
  int rc = failure_count > 0.0 ? 1 : 0;
  free(summary); free(paths);
  return rc;
}

static char *build_synth_generate_report_json(const report_rows_t *report,
                                              const generated_case_list_t *generated,
                                              const char *profile, const char *generator,
                                              const char *schedule,
                                              bool fast, bool capture_failures,
                                              bool strict_failures, int captured_failures,
                                              bool quarantine_known_bugs,
                                              int quarantined_known_bugs,
                                              int seed, const char *out_dir,
                                              const char *build_dir, const char *ny_bin,
                                              int runs, int warmup, double gen_ms,
                                              int selected_shape_count, int total_shape_count) {
  str_buf_t b = {0};
  int cases = report->rows.count;
  int ok_cases = cases - report->failed_rows;
  if (ok_cases < 0) ok_cases = 0;
  char *fingerprint = generated_cases_fingerprint(generated);
  char fingerprint_hex[32];
  snprintf(fingerprint_hex, sizeof(fingerprint_hex), "%016" PRIx64,
           fnv1a64(fingerprint ? fingerprint : "", strlen(fingerprint ? fingerprint : "")));
  (void)sb_append(&b, "{\"rows\":");
  append_rows_json(&b, &report->rows);
  (void)sb_append(&b, ",\"failures\":[");
  if (report->failures_json.data) (void)sb_append(&b, report->failures_json.data);
  (void)sb_appendf(&b,
                   "],\"ok\":%s,\"cases\":%d,\"ok_count\":%d,"
                   "\"failure_count\":%d,\"summary\":{",
                   report->failure_count == 0 ? "true" : "false",
                   cases, ok_cases, report->failure_count);
  (void)sb_appendf(&b, "\"cases\":%d,\"ok\":%d,\"ok_count\":%d,"
                   "\"ok_cases\":%d,\"generated_cases\":%d,"
                   "\"failure_count\":%d,\"captured_failures\":%d,"
                   "\"quarantine_known_bugs\":%s,\"quarantined_known_bugs\":%d,",
                   cases, ok_cases, ok_cases, ok_cases, generated->count,
                   report->failure_count,
                   captured_failures, quarantine_known_bugs ? "true" : "false",
                   quarantined_known_bugs);
  (void)sb_append(&b, "\"profile\":");
  (void)sb_append_json_str(&b, profile);
  (void)sb_append(&b, ",\"generator\":");
  (void)sb_append_json_str(&b, generator);
  (void)sb_append(&b, ",\"schedule\":");
  (void)sb_append_json_str(&b, schedule ? schedule : "smart");
  const char *method = canonical_native_method(generator);
  (void)sb_append(&b, ",\"generator_kind\":");
  (void)sb_append_json_str(&b, method);
  (void)sb_append(&b, ",\"method\":");
  (void)sb_append_json_str(&b, method);
  (void)sb_appendf(&b, ",\"fast\":%s,\"capture_failures\":%s,"
                   "\"strict_failures\":%s,\"seed\":%d,\"jobs\":1,\"compile_jobs\":1,"
                   "\"selected_shape_count\":%d,\"shape_count\":%d,",
                   fast ? "true" : "false",
                   capture_failures ? "true" : "false",
                   strict_failures ? "true" : "false", seed,
                   selected_shape_count, total_shape_count);
  (void)sb_append(&b, "\"engine\":\"nynth_core\",\"native_generation\":{\"engine\":\"nynth_core\",\"native_available\":true,");
  (void)sb_appendf(&b, "\"generation_wall_ms\":%.2f,\"generated\":%d},", gen_ms, generated->count);
  (void)sb_append(&b, "\"native_workers\":{");
  (void)sb_appendf(&b, "\"native_generation\":%d,\"native_compare\":%d,\"native_replay\":0},",
                   generated->count, cases);
  (void)sb_appendf(&b, "\"native_overhead_ms\":{\"generation\":%.2f,\"compare_workers\":%.2f},",
                   gen_ms, report->worker_ms);
  (void)sb_append(&b, "\"ratio_stats\":{");
  append_ratio_stats_json(&b, "ny_o3i_vs_c_o3_run", &report->ny_o3i_run);
  (void)sb_append(&b, "},\"coverage\":");
  append_generated_coverage_json(&b, generated);
  (void)sb_append(&b, ",\"generated_fingerprint\":");
  (void)sb_append_json_str(&b, fingerprint_hex);
  (void)sb_append(&b, ",\"contract\":{\"generator\":");
  (void)sb_append_json_str(&b, method);
  (void)sb_append(&b, ",\"description\":\"Nynth native methods emit C and Ny from one bounded IR\",\"correctness\":\"C O0/O3 and Ny O0/O3/O3i outputs must match\",\"ub_policy\":\"bounded loops, initialized vars, positive divisors, no shifts, small signed ranges\"}}");
  (void)sb_append(&b, ",\"meta\":{\"ny_bin\":");
  (void)sb_append_json_str(&b, ny_bin);
  (void)sb_append(&b, ",\"generated_dir\":");
  (void)sb_append_json_str(&b, out_dir);
  (void)sb_append(&b, ",\"build_dir\":");
  (void)sb_append_json_str(&b, build_dir);
  (void)sb_appendf(&b, ",\"run_repeats\":%d,\"warmup\":%d,\"jobs\":1,\"compile_jobs\":1,"
                   "\"capture_failures\":%s,\"strict_failures\":%s,"
                   "\"quarantine_known_bugs\":%s,\"quarantined_known_bugs\":%d,"
                   "\"schedule\":",
                   runs, warmup, capture_failures ? "true" : "false",
                   strict_failures ? "true" : "false",
                   quarantine_known_bugs ? "true" : "false", quarantined_known_bugs);
  (void)sb_append_json_str(&b, schedule ? schedule : "smart");
  (void)sb_append(&b, ",\"engine\":\"nynth_core\"}");
  (void)sb_append(&b, ",\"generated_cases\":");
  append_generated_cases_json(&b, generated);
  (void)sb_append_c(&b, '}');
  free(fingerprint);
  return sb_take(&b);
}

static void print_synth_generate_human(const report_rows_t *report) {
  int ok_cases = report->rows.count - report->failed_rows;
  if (ok_cases < 0) ok_cases = 0;
  printf("generated cases: %d/%d\n", ok_cases, report->rows.count);
  printf("failures: %d\n", report->failure_count);
  if (report->ny_o3i_run.count) {
    printf("ny_o3i_vs_c_o3_run avg=%.4f min=%.4f max=%.4f\n",
           report->ny_o3i_run.sum / (double)report->ny_o3i_run.count,
           report->ny_o3i_run.min, report->ny_o3i_run.max);
  }
}

static char *build_corpus_replay_report_json(const report_rows_t *report,
                                             const char *corpus_dir,
                                             const char *replay_build_dir,
                                             bool strict_stale,
                                             int native_replay,
                                             int native_compare) {
  str_buf_t b = {0};
  int entries = report->rows.count;
  int ok_cases = entries - report->failed_rows;
  if (ok_cases < 0) ok_cases = 0;
  (void)sb_append(&b, "{\"rows\":");
  append_rows_json(&b, &report->rows);
  (void)sb_append(&b, ",\"failures\":[");
  if (report->failures_json.data) (void)sb_append(&b, report->failures_json.data);
  (void)sb_append(&b, "],\"summary\":{");
  (void)sb_appendf(&b, "\"entries\":%d,\"ok_cases\":%d,\"quarantined\":0,\"failure_count\":%d,",
                   entries, ok_cases, report->failure_count);
  (void)sb_append(&b, "\"corpus_dir\":");
  (void)sb_append_json_str(&b, corpus_dir);
  (void)sb_appendf(&b, ",\"jobs\":1,\"strict_stale\":%s,\"replay_build_dir\":", strict_stale ? "true" : "false");
  (void)sb_append_json_str(&b, replay_build_dir);
  (void)sb_append(&b, ",\"engine\":\"nynth_core\",\"native_workers\":{");
  (void)sb_appendf(&b, "\"native_replay\":%d,\"native_compare\":%d},",
                   native_replay, native_compare);
  (void)sb_appendf(&b, "\"replay_timing\":{\"native_replay_ms\":%.2f,\"native_compare_ms\":%.2f}",
                   report->worker_ms, native_compare ? report->worker_ms : 0.0);
  (void)sb_append(&b, "}}");
  return sb_take(&b);
}

static void print_corpus_replay_human(const report_rows_t *report) {
  printf("entries: %d\n", report->rows.count);
  printf("quarantined: 0\n");
  printf("failures: %d\n", report->failure_count);
}

static const char *find_manifest_to_read(const char *corpus_dir, char **manifest_path,
                                         char **legacy_manifest_path) {
  if (asprintf(manifest_path, "%s/nynth_corpus.json", corpus_dir) < 0) return NULL;
  if (legacy_manifest_path) *legacy_manifest_path = NULL;
  return *manifest_path;
}

static char *entry_default_path(const char *corpus_dir, const char *id, const char *leaf) {
  char *path = NULL;
  if (asprintf(&path, "%s/cases/%s/%s", corpus_dir, id ? id : "", leaf) < 0) return NULL;
  return path;
}

static bool entry_ir_exists(char **path, const char *corpus_dir, const char *id) {
  if (*path && **path) return exists_path(*path);
  free(*path);
  *path = entry_default_path(corpus_dir, id, "case.nynth.json");
  return *path && exists_path(*path);
}

static char *build_corpus_audit_report_json(const report_rows_t *report,
                                            const string_list_t *features,
                                            const string_list_t *families,
                                            const string_list_t *methods,
                                            const string_list_t *blockers,
                                            int entries, int duplicates,
                                            int missing_files, bool fast,
                                            bool strict_stale,
                                            const char *corpus_dir,
                                            int ratio_count, double ratio_sum,
                                            double ratio_min, double ratio_max) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"rows\":");
  append_rows_json(&b, &report->rows);
  (void)sb_append(&b, ",\"failures\":[");
  if (report->failures_json.data) (void)sb_append(&b, report->failures_json.data);
  (void)sb_append(&b, "],\"summary\":{");
  (void)sb_appendf(&b, "\"entries\":%d,\"replayed\":0,\"quarantined\":0,\"duplicates\":%d,"
                   "\"missing_files\":%d,\"feature_coverage\":",
                   entries, duplicates, missing_files);
  append_string_list_json(&b, features);
  (void)sb_append(&b, ",\"family_coverage\":");
  append_string_list_json(&b, families);
  (void)sb_append(&b, ",\"method_coverage\":");
  append_string_list_json(&b, methods);
  (void)sb_append(&b, ",\"blocker_coverage\":");
  append_string_list_json(&b, blockers);
  (void)sb_append(&b, ",\"ratio_summary\":{\"count\":");
  (void)sb_appendf(&b, "%d,\"avg\":", ratio_count);
  if (ratio_count > 0) (void)sb_appendf(&b, "%.4f", ratio_sum / (double)ratio_count);
  else (void)sb_append(&b, "null");
  (void)sb_append(&b, ",\"max\":");
  if (ratio_count > 0) (void)sb_appendf(&b, "%.4f", ratio_max);
  else (void)sb_append(&b, "null");
  (void)sb_append(&b, ",\"min\":");
  if (ratio_count > 0) (void)sb_appendf(&b, "%.4f", ratio_min);
  else (void)sb_append(&b, "null");
  (void)sb_append_c(&b, '}');
  (void)sb_appendf(&b, ",\"failure_count\":%d,\"fast\":%s,\"strict_stale\":%s,\"corpus_dir\":",
                   report->failure_count, fast ? "true" : "false", strict_stale ? "true" : "false");
  (void)sb_append_json_str(&b, corpus_dir);
  (void)sb_append(&b, ",\"engine\":\"nynth_core\"}}");
  return sb_take(&b);
}

static int cmd_public_corpus_audit(int argc, char **argv, bool generated_alias) {
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  bool creal_mode = argc >= 4 && strcmp(argv[1], "corpus") == 0 &&
                    strcmp(argv[2], "creal") == 0;
  int arg_start = creal_mode ? 4 : (generated_alias ? 4 : 3);
  bool fast = has_flag_after(argc, argv, arg_start, "--fast");
  bool strict_stale = has_flag_after(argc, argv, arg_start, "--strict-stale");
  const char *json_path = value_after(argc, argv, arg_start, "--json", "");
  char *default_corpus = NULL;
  if (nynth_asprintf(&default_corpus, "%s", creal_mode ? "creal" : (generated_alias ? "generated" : "corpus")) < 0) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    return 2;
  }
  const char *corpus_dir = value_after(argc, argv, arg_start, "--corpus-dir", default_corpus);
  char *manifest_path = NULL, *legacy_manifest_path = NULL;
  const char *manifest_to_read = find_manifest_to_read(corpus_dir, &manifest_path, &legacy_manifest_path);
  if (!manifest_to_read) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(default_corpus); free(manifest_path); free(legacy_manifest_path);
    return 2;
  }
  file_buf_t manifest = {0};
  if (exists_path(manifest_to_read) && !read_file(manifest_to_read, &manifest)) {
    printf("{\"ok\":false,\"error\":\"manifest-read-failed\",\"manifest\":");
    json_str(stdout, manifest_to_read);
    printf("}\n");
    free(default_corpus); free(manifest_path); free(legacy_manifest_path);
    return 1;
  }
  report_rows_t report;
  memset(&report, 0, sizeof(report));
  string_list_t seen_keys = {0}, features = {0}, families = {0}, methods = {0}, blockers = {0};
  int entries = 0, duplicates = 0, missing_files = 0, ratio_count = 0;
  double ratio_sum = 0.0, ratio_min = 0.0, ratio_max = 0.0;
  const char *entries_json = manifest.data ? json_value_after_key(manifest.data, "entries") : NULL;
  const char *entries_end = entries_json && *entries_json == '[' ? matching_json_end(entries_json, '[', ']') : NULL;
  const char *p = entries_json && entries_end ? entries_json + 1 : NULL;
  while (p && p < entries_end) {
    p = skip_ws_const(p);
    if (p >= entries_end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '{') break;
    const char *obj_end = matching_json_end(p, '{', '}');
    if (!obj_end || obj_end > entries_end) break;
    char *obj_text = strndup_local(p, (size_t)(obj_end - p + 1));
    ++entries;
    char *id = json_extract_string_range(p, obj_end + 1, "id");
    char *case_name = json_extract_string_range(p, obj_end + 1, "case");
    char *shape = json_extract_string_range(p, obj_end + 1, "shape");
    char *family = json_extract_string_range(p, obj_end + 1, "family");
    char *structural = json_extract_string_range(p, obj_end + 1, "structural_hash");
    char *behavior = json_extract_string_range(p, obj_end + 1, "behavior_hash");
    char *blocker = json_extract_string_range(p, obj_end + 1, "ir_blocker_key");
    char *method = obj_text ? json_method_or_generator(obj_text) : strdup("");
    char *features_array = json_extract_array_range(p, obj_end + 1, "features");
    char *features_csv = features_csv_from_json_array(features_array);
    char *key = NULL;
    (void)asprintf(&key, "%s|%s|%s|%s", structural ? structural : "", behavior ? behavior : "",
                   features_csv ? features_csv : "", blocker ? blocker : "");
    bool duplicate = key && string_list_contains(&seen_keys, key);
    if (duplicate) ++duplicates;
    else if (key) (void)string_list_push_copy(&seen_keys, key);
    if (family && *family) (void)string_list_push_unique_copy(&families, family);
    if (method && *method) (void)string_list_push_unique_copy(&methods, method);
    if (blocker && *blocker) (void)string_list_push_unique_copy(&blockers, blocker);
    string_list_push_csv_unique(&features, features_csv);
    double ratio_value = 0.0;
    if (obj_text && extract_json_number(obj_text, "ratio_sample", &ratio_value)) {
      if (ratio_count == 0 || ratio_value < ratio_min) ratio_min = ratio_value;
      if (ratio_count == 0 || ratio_value > ratio_max) ratio_max = ratio_value;
      ratio_sum += ratio_value;
      ++ratio_count;
    }

    bool real_db_entry = method && strcmp(method, "real-db") == 0;
    bool creal_entry = method && strcmp(method, "creal") == 0;
    char *c_path = json_extract_string_range(p, obj_end + 1, "c");
    char *ny_path = json_extract_string_range(p, obj_end + 1, "ny");
    char *ir_path = json_extract_string_range(p, obj_end + 1, "ir");
    if (!real_db_entry && (!c_path || !*c_path)) { free(c_path); c_path = entry_default_path(corpus_dir, id, "case.c"); }
    if (!creal_entry && (!ny_path || !*ny_path)) { free(ny_path); ny_path = entry_default_path(corpus_dir, id, "case.ny"); }
    bool c_missing = real_db_entry ? false : (!c_path || !exists_path(c_path));
    bool ny_missing = creal_entry ? false : (!ny_path || !exists_path(ny_path));
    bool ir_ok = (real_db_entry || creal_entry) ? true : entry_ir_exists(&ir_path, corpus_dir, id);
    bool ir_missing = !ir_ok;
    bool has_missing = c_missing || ny_missing || ir_missing;
    if (has_missing) ++missing_files;

    str_buf_t row = {0};
    (void)sb_append(&row, "{\"id\":");
    (void)sb_append_json_str(&row, id ? id : "");
    (void)sb_append(&row, ",\"case\":");
    (void)sb_append_json_str(&row, case_name ? case_name : "");
    (void)sb_append(&row, ",\"shape\":");
    (void)sb_append_json_str(&row, shape ? shape : "");
    (void)sb_append(&row, ",\"family\":");
    (void)sb_append_json_str(&row, family ? family : "");
    (void)sb_append(&row, ",\"method\":");
    (void)sb_append_json_str(&row, method ? method : "");
    (void)sb_append(&row, ",\"features\":");
    (void)sb_append(&row, features_array ? features_array : "[]");
    (void)sb_appendf(&row, ",\"duplicate\":%s,\"missing_paths\":[", duplicate ? "true" : "false");
    bool first_missing = true;
#define APPEND_MISSING(path_value, missing_flag) \
    do { if ((missing_flag)) { if (!first_missing) (void)sb_append_c(&row, ','); first_missing = false; (void)sb_append_json_str(&row, (path_value) ? (path_value) : ""); } } while (0)
    APPEND_MISSING(c_path, c_missing);
    APPEND_MISSING(ny_path, ny_missing);
    APPEND_MISSING(ir_path, ir_missing);
#undef APPEND_MISSING
    (void)sb_appendf(&row, "],\"replayed\":false,\"ok\":%s,\"failures\":[", (!duplicate && !has_missing) ? "true" : "false");
    bool first_failure = true;
    if (duplicate) {
      (void)sb_append(&row, "{\"id\":");
      (void)sb_append_json_str(&row, id ? id : "");
      (void)sb_append(&row, ",\"error\":\"duplicate structural/behavior hash\"}");
      first_failure = false;
    }
    if (has_missing) {
      if (!first_failure) (void)sb_append_c(&row, ',');
      (void)sb_append(&row, "{\"id\":");
      (void)sb_append_json_str(&row, id ? id : "");
      (void)sb_append(&row, ",\"error\":\"missing source files\"}");
    }
    (void)sb_append(&row, "]}");
    report_add_row(&report, sb_take(&row));
    free(id); free(case_name); free(shape); free(family); free(structural); free(behavior);
    free(blocker); free(method); free(features_array); free(features_csv); free(key); free(obj_text);
    free(c_path); free(ny_path); free(ir_path);
    p = obj_end + 1;
  }
  qsort(features.items, (size_t)features.count, sizeof(char *), cmp_cstr);
  qsort(families.items, (size_t)families.count, sizeof(char *), cmp_cstr);
  qsort(methods.items, (size_t)methods.count, sizeof(char *), cmp_cstr);
  qsort(blockers.items, (size_t)blockers.count, sizeof(char *), cmp_cstr);
  char *report_json = build_corpus_audit_report_json(&report, &features, &families,
                                                     &methods, &blockers,
                                                     entries, duplicates, missing_files,
                                                     fast, strict_stale, corpus_dir,
                                                     ratio_count, ratio_sum, ratio_min, ratio_max);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    printf("entries: %d\n", entries);
    printf("duplicates: %d\n", duplicates);
    printf("missing_files: %d\n", missing_files);
    printf("failures: %d\n", report.failure_count);
  }
  int rc = report.failure_count ? 1 : 0;
  free(report_json);
  report_rows_free(&report);
  string_list_free(&seen_keys); string_list_free(&features); string_list_free(&families);
  string_list_free(&methods); string_list_free(&blockers);
  free(manifest.data);
  free(default_corpus); free(manifest_path); free(legacy_manifest_path);
  return rc;
}

static int cmd_public_corpus_replay(int argc, char **argv, bool generated_alias) {
  char root[4096], ny_bin[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  if (!find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-binary-not-found\",\"reason\":\"run ./make ny first or set NYTRIX_NY_BIN\"}\n");
    return 2;
  }
  bool creal_mode = argc >= 4 && strcmp(argv[1], "corpus") == 0 &&
                    strcmp(argv[2], "creal") == 0;
  int arg_start = creal_mode ? 4 : (generated_alias ? 4 : 3);
  int limit = atoi(value_after(argc, argv, arg_start, "--limit", "0"));
  double timeout_s = atof(value_after(argc, argv, arg_start, "--timeout-s", "60"));
  bool strict_stale = has_flag_after(argc, argv, arg_start, "--strict-stale");
  const char *json_path = value_after(argc, argv, arg_start, "--json", "");
  char *default_corpus = NULL;
  if (nynth_asprintf(&default_corpus, "%s", creal_mode ? "creal" : (generated_alias ? "generated" : "corpus")) < 0) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    return 2;
  }
  const char *corpus_dir = value_after(argc, argv, arg_start, "--corpus-dir", default_corpus);
  manifest_entry_list_t entries = {0};
  if (!load_manifest_entries(corpus_dir, &entries)) {
    printf("{\"ok\":false,\"error\":\"manifest-read-failed\",\"corpus_dir\":");
    json_str(stdout, corpus_dir);
    printf("}\n");
    free(default_corpus);
    return 1;
  }
  int replay_count = entries.count;
  if (limit > 0 && limit < replay_count) replay_count = limit;
  char *replay_build_dir = NULL;
  if (nynth_asprintf(&replay_build_dir, "build/%s/replay/native_%ld", creal_mode ? "creal" : (generated_alias ? "generated" : "corpus"), (long)getpid()) < 0) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    manifest_entry_list_free(&entries);
    free(default_corpus);
    return 2;
  }
  report_rows_t report;
  memset(&report, 0, sizeof(report));
  char timeout_buf[64];
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  double outer_timeout = worker_outer_timeout(timeout_s, 1, 0);
  int native_replay = 0, native_compare = 0;
  for (int i = 0; i < replay_count; ++i) {
    const char *entry_id = entries.items[i].id;
    const char *entry_json = entries.items[i].json;
    if (entry_is_real_db(entry_json)) {
      char *row = make_real_db_replay_row(root, ny_bin, entry_id, entry_json,
                                          corpus_dir, NULL, timeout_s);
      native_replay++;
      double worker_ms = 0.0;
      if (extract_json_number(row, "worker_ms", &worker_ms)) report.worker_ms += worker_ms;
      report_add_row(&report, row);
      continue;
    }
    if (entry_is_creal_db(entry_json)) {
      char *row = make_creal_db_replay_row(root, entry_id, entry_json,
                                           corpus_dir, timeout_s);
      native_replay++;
      double worker_ms = 0.0;
      if (extract_json_number(row, "worker_ms", &worker_ms)) report.worker_ms += worker_ms;
      report_add_row(&report, row);
      continue;
    }
    char *case_bin_dir = NULL;
    if (asprintf(&case_bin_dir, "%s/%06d_%s", replay_build_dir, i, entry_id) < 0) {
      char *row = make_worker_failure_row(entry_id, "replay-prepare", 1, "", "bin dir allocation failed");
      char *with_id = row_with_id_field(row, entry_id);
      free(row);
      report_add_row(&report, with_id);
      continue;
    }
    char *worker_argv[] = {
      g_self_path, "replay-corpus-entry",
      "--corpus-dir", (char *)corpus_dir,
      "--entry-id", (char *)entry_id,
      "--root", root,
      "--ny-bin", ny_bin,
      "--bin-dir", case_bin_dir,
      "--timeout-s", timeout_buf,
      NULL
    };
    proc_result_t pr = run_proc(worker_argv, root, outer_timeout);
    native_replay++;
    native_compare++;
    report.worker_ms += pr.elapsed_ms;
    char *row = NULL;
    if (pr.rc == 0 && pr.out && strstr(pr.out, "\"failures\"")) {
      char *trimmed = trim_trailing_copy(pr.out);
      row = row_with_id_field(trimmed, entry_id);
      free(trimmed);
    } else {
      row = make_worker_failure_row(entry_id, "replay-corpus-entry", pr.rc, pr.out, pr.err);
      char *with_id = row_with_id_field(row, entry_id);
      free(row);
      row = with_id;
    }
    report_add_row(&report, row);
    proc_result_free(&pr);
    free(case_bin_dir);
  }
  char *report_json = build_corpus_replay_report_json(&report, corpus_dir, replay_build_dir,
                                                      strict_stale, native_replay, native_compare);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    print_corpus_replay_human(&report);
  }
  int rc = report.failure_count ? 1 : 0;
  free(report_json);
  report_rows_free(&report);
  free(replay_build_dir);
  manifest_entry_list_free(&entries);
  free(default_corpus);
  return rc;
}

static char *build_corpus_build_report_json(const string_list_t *promotions,
                                            const string_list_t *failures,
                                            const char *generator_report,
                                            int generated_cases, int promoted,
                                            int duplicates, int entries,
                                            const char *corpus_dir,
                                            const char *generator) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"rows\":");
  append_raw_json_list(&b, promotions);
  (void)sb_append(&b, ",\"failures\":");
  append_raw_json_list(&b, failures);
  (void)sb_appendf(&b, ",\"summary\":{\"generated_cases\":%d,\"promoted\":%d,"
                   "\"duplicates\":%d,\"not_useful\":0,\"entries\":%d,"
                   "\"failure_count\":%d,\"corpus_dir\":",
                   generated_cases, promoted, duplicates, entries, failures->count);
  (void)sb_append_json_str(&b, corpus_dir);
  (void)sb_append(&b, ",\"jobs\":1,\"generator\":");
  (void)sb_append_json_str(&b, generator);
  const char *method = canonical_native_method(generator);
  (void)sb_append(&b, ",\"generator_kind\":");
  (void)sb_append_json_str(&b, method);
  (void)sb_append(&b, ",\"method\":");
  (void)sb_append_json_str(&b, method);
  (void)sb_append(&b, ",\"engine\":\"nynth_core\",\"native_workers\":{");
  (void)sb_appendf(&b, "\"native_generation\":%d,\"native_compare\":%d,"
                   "\"native_replay\":0}},\"generator_report\":",
                   generated_cases, generated_cases);
  (void)sb_append(&b, generator_report ? generator_report : "{}");
  (void)sb_append_c(&b, '}');
  return sb_take(&b);
}

static int manifest_entry_count_for_dir(const char *corpus_dir) {
  manifest_entry_list_t entries = {0};
  int count = 0;
  if (load_manifest_entries(corpus_dir, &entries)) count = entries.count;
  manifest_entry_list_free(&entries);
  return count;
}

static int cmd_public_corpus_build(int argc, char **argv, bool generated_alias) {
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  int arg_start = generated_alias ? 4 : 3;
  bool fast = has_flag_after(argc, argv, arg_start, "--fast");
  int cases = atoi(value_after(argc, argv, arg_start, "--cases", "8"));
  if (cases < 1) cases = 1;
  int seed = atoi(value_after(argc, argv, arg_start, "--seed", "1337"));
  double timeout_s = atof(value_after(argc, argv, arg_start, "--timeout-s", "90"));
  const char *profile = value_after(argc, argv, arg_start, "--profile", "balanced");
  const char *generator = value_after(argc, argv, arg_start, "--generator", generated_alias ? "ir" : "mixed");
  const char *json_path = value_after(argc, argv, arg_start, "--json", "");
  char *default_corpus = NULL, *staging = NULL, *synth_json = NULL;
  if (nynth_asprintf(&default_corpus, "%s", generated_alias ? "generated" : "corpus") < 0) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(default_corpus);
    return 2;
  }
  const char *corpus_dir = value_after(argc, argv, arg_start, "--corpus-dir", default_corpus);
  if (asprintf(&staging, "%s/staging/%s_%d_%ld", corpus_dir, generator, seed, (long)getpid()) < 0 ||
      asprintf(&synth_json, "%s/_synth_report_%d_%ld.json", staging, seed, (long)getpid()) < 0) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(default_corpus); free(staging); free(synth_json);
    return 2;
  }
  char cases_buf[32], seed_buf[32], timeout_buf[64];
  snprintf(cases_buf, sizeof(cases_buf), "%d", cases);
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  char *build_dir = NULL;
  if (asprintf(&build_dir, "%s/build_%s_%d_%ld", staging, generator, seed, (long)getpid()) < 0) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(default_corpus); free(staging); free(synth_json);
    return 2;
  }
  char *synth_argv[32];
  int a = 0;
  synth_argv[a++] = g_self_path;
  synth_argv[a++] = "synth";
  synth_argv[a++] = "generate";
  synth_argv[a++] = "--cases"; synth_argv[a++] = cases_buf;
  synth_argv[a++] = "--seed"; synth_argv[a++] = seed_buf;
  synth_argv[a++] = "--profile"; synth_argv[a++] = (char *)profile;
  synth_argv[a++] = "--generator"; synth_argv[a++] = (char *)generator;
  synth_argv[a++] = "--timeout-s"; synth_argv[a++] = timeout_buf;
  synth_argv[a++] = "--out"; synth_argv[a++] = staging;
  synth_argv[a++] = "--build-dir"; synth_argv[a++] = build_dir;
  synth_argv[a++] = "--json"; synth_argv[a++] = synth_json;
  if (fast) synth_argv[a++] = "--fast";
  synth_argv[a] = NULL;
  proc_result_t pr = run_proc(synth_argv, root, worker_outer_timeout(timeout_s, 1, 0));
  file_buf_t report_file = {0};
  char *generator_report = NULL;
  if (read_file(synth_json, &report_file) && report_file.data && strstr(report_file.data, "\"rows\""))
    generator_report = trim_trailing_copy(report_file.data);
  else
    generator_report = make_failed_command_report("corpus-build", "synth-generate", pr.rc, pr.out, pr.err);
  free(report_file.data);
  proc_result_free(&pr);

  string_list_t rows = {0}, promotions = {0}, failures = {0};
  (void)collect_rows_from_report_json(generator_report, &rows);
  int promoted = 0, duplicates = 0;
  for (int i = 0; i < rows.count; ++i) {
    bool ok = false, did_promote = false, duplicate = false;
    char *result = build_promotion_result_json(rows.items[i], corpus_dir, "corpus-build",
                                               &ok, &did_promote, &duplicate);
    if (!result) result = strdup("{\"ok\":false,\"promoted\":false,\"reason\":\"allocation-failed\"}");
    if (did_promote) promoted++;
    if (duplicate) duplicates++;
    if (!ok) (void)string_list_push_copy(&failures, result);
    (void)string_list_push_take(&promotions, result);
  }
  if (!rows.count) {
    char *failure = make_worker_failure_row("corpus-build", "synth-generate", pr.rc, "", "generator report had no rows");
    (void)string_list_push_take(&failures, failure);
  }
  int entries = manifest_entry_count_for_dir(corpus_dir);
  char *report_json = build_corpus_build_report_json(&promotions, &failures, generator_report,
                                                     rows.count, promoted, duplicates, entries,
                                                     corpus_dir, generator);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    printf("corpus rows: %d\n", promotions.count);
    printf("failures: %d\n", failures.count);
    printf("summary: entries=%d, promoted=%d, duplicates=%d\n", entries, promoted, duplicates);
  }
  int rc = failures.count ? 1 : 0;
  free(report_json); free(generator_report);
  string_list_free(&rows); string_list_free(&promotions); string_list_free(&failures);
  free(default_corpus); free(staging); free(synth_json); free(build_dir);
  return rc;
}

static int cmd_public_corpus_promote(int argc, char **argv, bool generated_alias) {
  bool creal_mode = argc >= 4 && strcmp(argv[1], "corpus") == 0 &&
                    strcmp(argv[2], "creal") == 0;
  int arg_start = creal_mode ? 4 : (generated_alias ? 4 : 3);
  if (argc <= arg_start) return worker_usage();
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  const char *artifact = argv[arg_start];
  const char *note = value_after(argc, argv, arg_start + 1, "--note", "manual-promote");
  const char *json_path = value_after(argc, argv, arg_start + 1, "--json", "");
  char *default_corpus = NULL;
  if (nynth_asprintf(&default_corpus, "%s", creal_mode ? "creal" : (generated_alias ? "generated" : "corpus")) < 0) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    return 2;
  }
  const char *corpus_dir = value_after(argc, argv, arg_start + 1, "--corpus-dir", default_corpus);
  file_buf_t artifact_json = {0};
  if (!read_file(artifact, &artifact_json)) {
    printf("{\"ok\":false,\"error\":\"artifact-read-failed\",\"artifact\":");
    json_str(stdout, artifact);
    printf("}\n");
    free(default_corpus);
    return 1;
  }
  char *row = first_row_or_object_from_json(artifact_json.data);
  free(artifact_json.data);
  string_list_t rows = {0}, failures = {0};
  bool ok = false, promoted = false, duplicate = false;
  char *result = NULL;
  if (row) {
    result = creal_mode ?
      build_creal_promotion_result_json(row, corpus_dir, note, &ok, &promoted, &duplicate) :
      build_promotion_result_json(row, corpus_dir, note, &ok, &promoted, &duplicate);
  }
  if (!result) result = strdup("{\"ok\":false,\"promoted\":false,\"reason\":\"artifact-has-no-row\"}");
  if (!ok) (void)string_list_push_copy(&failures, result);
  (void)string_list_push_take(&rows, result);
  str_buf_t report = {0};
  (void)sb_append(&report, "{\"rows\":");
  append_raw_json_list(&report, &rows);
  (void)sb_append(&report, ",\"failures\":");
  append_raw_json_list(&report, &failures);
  (void)sb_appendf(&report, ",\"summary\":{\"promoted\":%d,\"duplicates\":%d,\"failure_count\":%d,\"corpus_dir\":",
                   promoted ? 1 : 0, duplicate ? 1 : 0, failures.count);
  (void)sb_append_json_str(&report, corpus_dir);
  (void)sb_append(&report, ",\"engine\":\"nynth_core\"}}");
  char *report_json = sb_take(&report);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    printf("corpus rows: %d\n", rows.count);
    printf("failures: %d\n", failures.count);
    printf("summary: promoted=%d, duplicates=%d\n", promoted ? 1 : 0, duplicate ? 1 : 0);
  }
  int rc = failures.count ? 1 : 0;
  free(report_json); free(row); free(default_corpus);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static const char *canonical_lane(const char *raw) {
  if (!raw || !*raw) return "";
  if (strcmp(raw, "ir-typed") == 0) return "ir";
  if (strcmp(raw, "stress-typed") == 0) return "stress";
  return raw;
}

static bool known_lane(const char *lane) {
  return strcmp(lane, "typed") == 0 || strcmp(lane, "optimizer") == 0 ||
         strcmp(lane, "ir") == 0 || strcmp(lane, "stress") == 0 ||
         strcmp(lane, "creal") == 0 ||
         strcmp(lane, "torture") == 0 || strcmp(lane, "cbridge") == 0 ||
         strcmp(lane, "random") == 0 || strcmp(lane, "afl") == 0;
}

static bool parse_lanes_arg(const char *text, string_list_t *lanes, char *error, size_t error_sz) {
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s", text && *text ? text : "typed,optimizer");
  char *start = buf;
  for (char *p = buf; ; ++p) {
    if (*p != ',' && *p != '+' && *p != '\0') continue;
    char save = *p;
    *p = '\0';
    while (*start && isspace((unsigned char)*start)) ++start;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) *--end = '\0';
    for (char *q = start; *q; ++q) *q = (char)tolower((unsigned char)*q);
    const char *lane = canonical_lane(start);
    if (*lane) {
      if (!known_lane(lane)) {
        snprintf(error, error_sz, "unknown campaign lane '%s'", start);
        return false;
      }
      (void)string_list_push_unique_copy(lanes, lane);
    }
    if (save == '\0') break;
    start = p + 1;
  }
  if (!lanes->count) (void)string_list_push_copy(lanes, "typed");
  return true;
}

static char *make_failed_command_report(const char *lane, const char *phase, int rc,
                                        const char *out, const char *err) {
  char *row = make_worker_failure_row(lane, phase, rc, out, err);
  char *tagged = row_with_lane_fields(row, lane, "native-worker");
  free(row);
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"rows\":[");
  (void)sb_append(&b, tagged);
  (void)sb_append(&b, "],\"failures\":[{\"lane\":");
  (void)sb_append_json_str(&b, lane ? lane : "");
  (void)sb_append(&b, ",\"phase\":");
  (void)sb_append_json_str(&b, phase ? phase : "worker");
  (void)sb_appendf(&b, ",\"rc\":%d}],\"summary\":{\"lane\":", rc);
  (void)sb_append_json_str(&b, lane ? lane : "");
  (void)sb_append(&b, ",\"failure_count\":1,\"engine\":\"nynth_core\"},\"meta\":{}}");
  free(tagged);
  return sb_take(&b);
}

static char *run_synth_lane_report(const char *root, const char *lane, const char *generator,
                                   const char *profile,
                                   int cases, int seed, bool fast, double timeout_s,
                                   int runs, int warmup, int idx,
                                   bool quarantine_known_bugs) {
  char *json_path = make_tmp_json_path(root, "campaign_synth", seed, idx);
  char *out_dir = NULL, *build_dir = NULL;
  long pid = (long)getpid();
  if (!json_path ||
      nynth_asprintf(&out_dir, "build/generated/campaign/%s_%d_%d_%ld", lane, seed, idx, pid) < 0 ||
      nynth_asprintf(&build_dir, "build/campaign/%s_%d_%d_%ld", lane, seed, idx, pid) < 0) {
    free(json_path);
    free(out_dir);
    free(build_dir);
    return make_failed_command_report(lane, "prepare", 1, "", "allocation failed");
  }
  char cases_buf[32], seed_buf[32], timeout_buf[64], runs_buf[32], warmup_buf[32];
  snprintf(cases_buf, sizeof(cases_buf), "%d", cases);
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  snprintf(runs_buf, sizeof(runs_buf), "%d", runs);
  snprintf(warmup_buf, sizeof(warmup_buf), "%d", warmup);
  char *argv[32];
  int a = 0;
  argv[a++] = g_self_path;
  argv[a++] = "synth";
  argv[a++] = "generate";
  argv[a++] = "--cases"; argv[a++] = cases_buf;
  argv[a++] = "--seed"; argv[a++] = seed_buf;
  argv[a++] = "--timeout-s"; argv[a++] = timeout_buf;
  argv[a++] = "--profile"; argv[a++] = (char *)profile;
  argv[a++] = "--generator"; argv[a++] = (char *)(generator && *generator ? generator : lane);
  argv[a++] = "--out"; argv[a++] = out_dir;
  argv[a++] = "--build-dir"; argv[a++] = build_dir;
  argv[a++] = "--runs"; argv[a++] = runs_buf;
  argv[a++] = "--warmup"; argv[a++] = warmup_buf;
  argv[a++] = "--json"; argv[a++] = json_path;
  if (quarantine_known_bugs) argv[a++] = "--quarantine-known-bugs";
  if (fast) argv[a++] = "--fast";
  argv[a] = NULL;
  proc_result_t pr = run_proc(argv, root, worker_outer_timeout(timeout_s, runs, warmup));
  file_buf_t f = {0};
  char *report = NULL;
  if (read_file(json_path, &f) && f.data && strstr(f.data, "\"rows\"")) {
    report = trim_trailing_copy(f.data);
  } else {
    report = make_failed_command_report(lane, "synth-generate", pr.rc, pr.out, pr.err);
  }
  free(f.data);
  proc_result_free(&pr);
  free(json_path);
  free(out_dir);
  free(build_dir);
  return report;
}

static char *run_random_lane_report(const char *root, const char *profile,
                                    int cases, int seed, bool fast, double timeout_s,
                                    int runs, int warmup, int idx,
                                    bool quarantine_known_bugs) {
  return run_synth_lane_report(root, "random", "mixed", profile, cases, seed,
                               fast, timeout_s, runs, warmup, idx, quarantine_known_bugs);
}

static char *run_cbridge_lane_report(const char *root, bool fast, double timeout_s) {
  char timeout_buf[64];
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  char *argv[8];
  int a = 0;
  argv[a++] = g_self_path;
  argv[a++] = "bridge";
  argv[a++] = "suite";
  argv[a++] = "--timeout-s"; argv[a++] = timeout_buf;
  argv[a++] = "--json";
  if (fast) argv[a++] = "--fast";
  argv[a] = NULL;
  proc_result_t pr = run_proc(argv, root, worker_outer_timeout(timeout_s, 1, 0));
  char *report = NULL;
  if (pr.out && strstr(pr.out, "\"rows\"")) report = trim_trailing_copy(pr.out);
  else report = make_failed_command_report("cbridge", "bridge-suite", pr.rc, pr.out, pr.err);
  proc_result_free(&pr);
  return report;
}

static char *run_creal_lane_report(const char *root, int cases, int seed, bool fast,
                                   double timeout_s, int idx, const char *corpus_dir,
                                   const char *function_db) {
  char *json_path = make_tmp_json_path(root, "campaign_creal", seed, idx);
  if (!json_path) return make_failed_command_report("creal", "prepare", 1, "", "allocation failed");
  char cases_buf[32], seed_buf[32], timeout_buf[64];
  snprintf(cases_buf, sizeof(cases_buf), "%d", cases);
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  char *argv[28];
  int a = 0;
  argv[a++] = g_self_path;
  argv[a++] = "synth";
  argv[a++] = "creal";
  argv[a++] = "--cases"; argv[a++] = cases_buf;
  argv[a++] = "--seed"; argv[a++] = seed_buf;
  argv[a++] = "--timeout-s"; argv[a++] = timeout_buf;
  argv[a++] = "--corpus-dir"; argv[a++] = (char *)corpus_dir;
  if (function_db && *function_db) { argv[a++] = "--function-db"; argv[a++] = (char *)function_db; }
  argv[a++] = "--json"; argv[a++] = json_path;
  if (fast) argv[a++] = "--fast";
  argv[a] = NULL;
  proc_result_t pr = run_proc(argv, root, worker_outer_timeout(timeout_s, 1, 0));
  file_buf_t f = {0};
  char *report = NULL;
  if (read_file(json_path, &f) && f.data && strstr(f.data, "\"rows\"")) {
    report = trim_trailing_copy(f.data);
  } else {
    report = make_failed_command_report("creal", "synth-creal", pr.rc, pr.out, pr.err);
  }
  free(f.data);
  proc_result_free(&pr);
  free(json_path);
  return report;
}

static char *run_afl_lane_report(const char *root, int idx) {
  char *json_path = make_tmp_json_path(root, "campaign_afl", 0, idx);
  if (!json_path) return make_failed_command_report("afl", "prepare", 1, "", "allocation failed");
  char *argv[14];
  int a = 0;
  argv[a++] = g_self_path;
  argv[a++] = "fuzz";
  argv[a++] = "afl";
  argv[a++] = "run";
  argv[a++] = "--target"; argv[a++] = "ny";
  argv[a++] = "--minutes"; argv[a++] = "0";
  argv[a++] = "--dry-run";
  argv[a++] = "--json"; argv[a++] = json_path;
  argv[a] = NULL;
  proc_result_t pr = run_proc(argv, root, 60.0);
  file_buf_t f = {0};
  char *report = NULL;
  if (read_file(json_path, &f) && f.data && strstr(f.data, "\"rows\"")) {
    report = trim_trailing_copy(f.data);
  } else {
    report = make_failed_command_report("afl", "fuzz-afl-run", pr.rc, pr.out, pr.err);
  }
  free(f.data);
  proc_result_free(&pr);
  free(json_path);
  return report;
}

static char *build_campaign_run_report_json(const report_rows_t *report, const string_list_t *lane_reports,
                                            const string_list_t *lanes, const int *lane_counts,
                                            const int *lane_failures, const worker_counts_t *workers,
                                            const char *profile, int seed, int cases, int runs,
                                            int warmup, bool fast, const char *corpus_dir,
                                            const string_list_t *skipped_lanes) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"rows\":");
  append_rows_json(&b, &report->rows);
  (void)sb_append(&b, ",\"failures\":[");
  if (report->failures_json.data) (void)sb_append(&b, report->failures_json.data);
  (void)sb_append(&b, "],\"summary\":{\"version\":1,\"lanes\":");
  append_string_list_json(&b, lanes);
  (void)sb_append(&b, ",\"lane_counts\":{");
  for (int i = 0; i < lanes->count; ++i) {
    if (i) (void)sb_append_c(&b, ',');
    (void)sb_append_json_str(&b, lanes->items[i]);
    (void)sb_appendf(&b, ":%d", lane_counts[i]);
  }
  (void)sb_append(&b, "},\"failure_counts\":{");
  for (int i = 0; i < lanes->count; ++i) {
    if (i) (void)sb_append_c(&b, ',');
    (void)sb_append_json_str(&b, lanes->items[i]);
    (void)sb_appendf(&b, ":%d", lane_failures[i]);
  }
  (void)sb_appendf(&b, "},\"failure_count\":%d,\"skipped_lanes\":", report->failure_count);
  append_string_list_json(&b, skipped_lanes);
  (void)sb_append(&b, ",\"ratio_sample_count\":");
  (void)sb_appendf(&b, "%d,\"ny_o3i_vs_c_o3_run_geomean\":null,\"promoted\":0,\"jobs\":1,\"compile_jobs\":1,\"native_workers\":",
                   report->ny_o3i_run.count);
  append_worker_counts_json(&b, workers);
  (void)sb_append(&b, ",\"profile\":");
  (void)sb_append_json_str(&b, profile);
  (void)sb_appendf(&b, ",\"seed\":%d,\"claim_policy\":\"ratios are exact measurements only; no faster-than-C claim is inferred\"}", seed);
  (void)sb_append(&b, ",\"lane_reports\":");
  append_raw_json_list(&b, lane_reports);
  (void)sb_append(&b, ",\"meta\":{\"corpus_dir\":");
  (void)sb_append_json_str(&b, corpus_dir);
  (void)sb_appendf(&b, ",\"cases\":%d,\"runs\":%d,\"warmup\":%d,\"fast\":%s,\"engine\":\"nynth_core\"}}",
                   cases, runs, warmup, fast ? "true" : "false");
  return sb_take(&b);
}

static void print_campaign_human(const report_rows_t *report) {
  printf("campaign rows: %d\n", report->rows.count);
  printf("failures: %d\n", report->failure_count);
}

static int cmd_public_campaign_run(int argc, char **argv) {
  char root[4096];
  if (!find_repo_root_or_sibling(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  const char *lanes_text = value_after(argc, argv, 3, "--lanes", "typed,optimizer");
  string_list_t lanes = {0}, lane_reports = {0}, skipped_lanes = {0};
  char parse_error[256] = {0};
  if (!parse_lanes_arg(lanes_text, &lanes, parse_error, sizeof(parse_error))) {
    printf("{\"ok\":false,\"error\":\"invalid-lanes\",\"reason\":");
    json_str(stdout, parse_error);
    printf("}\n");
    string_list_free(&lane_reports);
    string_list_free(&skipped_lanes);
    string_list_free(&lanes);
    return 2;
  }
  const char *profile = value_after(argc, argv, 3, "--profile", "balanced");
  int cases = atoi(value_after(argc, argv, 3, "--cases", "8"));
  if (cases < 1) cases = 1;
  int seed = atoi(value_after(argc, argv, 3, "--seed", "1337"));
  int runs = atoi(value_after(argc, argv, 3, "--runs", "1"));
  if (runs < 1) runs = 1;
  int warmup = atoi(value_after(argc, argv, 3, "--warmup", "0"));
  if (warmup < 0) warmup = 0;
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "90"));
  bool fast = has_flag_after(argc, argv, 3, "--fast");
  bool quarantine_known_bugs = has_flag_after(argc, argv, 3, "--quarantine-known-bugs") ||
                               has_flag_after(argc, argv, 3, "--known-bugs-ok");
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *function_db = value_after(argc, argv, 3, "--function-db", "");
  char *default_corpus = NULL;
  if (nynth_asprintf(&default_corpus, "shapes/corpus/nynth-core") < 0) {
    string_list_free(&lane_reports);
    string_list_free(&skipped_lanes);
    string_list_free(&lanes);
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    return 2;
  }
  const char *corpus_dir = value_after(argc, argv, 3, "--corpus-dir", default_corpus);
  report_rows_t report;
  memset(&report, 0, sizeof(report));
  worker_counts_t workers;
  memset(&workers, 0, sizeof(workers));
  int *lane_counts = (int *)calloc((size_t)lanes.count, sizeof(int));
  int *lane_failures = (int *)calloc((size_t)lanes.count, sizeof(int));
  if (!lane_counts || !lane_failures) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(lane_counts); free(lane_failures); free(default_corpus);
    string_list_free(&lane_reports);
    string_list_free(&skipped_lanes);
    string_list_free(&lanes);
    return 2;
  }
  for (int i = 0; i < lanes.count; ++i) {
    const char *lane = lanes.items[i];
    int lane_seed = seed + i * 1009;
    char *lane_report = NULL;
    const char *source_kind = "nynth-typed-ast";
    if (strcmp(lane, "typed") == 0 || strcmp(lane, "optimizer") == 0 || strcmp(lane, "torture") == 0 ||
        strcmp(lane, "ir") == 0 || strcmp(lane, "stress") == 0) {
      lane_report = run_synth_lane_report(root, lane, lane, profile, cases, lane_seed,
                                          fast, timeout_s, runs, warmup, i,
                                          quarantine_known_bugs);
      if (strcmp(lane, "optimizer") == 0) source_kind = "nynth-typed-ast-optimizer-pattern";
      else if (strcmp(lane, "torture") == 0) source_kind = "nynth-typed-ast-gcc-torture-inspired";
      else if (strcmp(lane, "ir") == 0) source_kind = "nynth-core-ir-typed-ast";
      else if (strcmp(lane, "stress") == 0) source_kind = "nynth-core-stress-optimizer";
    } else if (strcmp(lane, "random") == 0) {
      lane_report = run_random_lane_report(root, profile, cases, lane_seed, fast,
                                           timeout_s, runs, warmup, i,
                                           quarantine_known_bugs);
      source_kind = "nynth-native-random";
    } else if (strcmp(lane, "cbridge") == 0) {
      lane_report = run_cbridge_lane_report(root, fast, timeout_s);
      source_kind = "curated-c-bridge";
    } else if (strcmp(lane, "creal") == 0) {
      lane_report = run_creal_lane_report(root, cases, lane_seed, fast, timeout_s, i,
                                          corpus_dir, function_db);
      source_kind = "creal-function-db";
    } else if (strcmp(lane, "afl") == 0) {
      lane_report = run_afl_lane_report(root, i);
      source_kind = "native-afl-dry-run";
    } else {
      lane_report = make_failed_command_report(lane, "campaign-lane-dispatch", 2, "",
                                               "internal unknown campaign lane");
      source_kind = "native-worker";
    }
    worker_counts_add_from_report(&workers, lane_report);
    int before_rows = report.rows.count;
    int before_failures = report.failure_count;
    lane_counts[i] = report_add_rows_from_report_json(&report, lane_report, lane, source_kind);
    lane_failures[i] = report.failure_count - before_failures;
    if (!lane_counts[i] && strstr(lane_report, "\"failures\"")) {
      char *row = make_worker_failure_row(lane, "campaign-lane-parse", 1, "", "lane report had no rows");
      char *tagged = row_with_lane_fields(row, lane, source_kind);
      free(row);
      report_add_row(&report, tagged);
      lane_counts[i] = report.rows.count - before_rows;
      lane_failures[i] = report.failure_count - before_failures;
    }
    (void)string_list_push_take(&lane_reports, lane_report);
  }
  char *report_json = build_campaign_run_report_json(&report, &lane_reports, &lanes,
                                                     lane_counts, lane_failures, &workers,
                                                     profile, seed, cases, runs, warmup,
                                                     fast, corpus_dir, &skipped_lanes);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    print_campaign_human(&report);
  }
  int rc = report.failure_count ? 1 : 0;
  free(report_json);
  free(lane_counts); free(lane_failures); free(default_corpus);
  report_rows_free(&report);
  string_list_free(&lane_reports);
  string_list_free(&skipped_lanes);
  string_list_free(&lanes);
  return rc;
}

static const opt_variant_native_t opt_variants[] = {
  {"branch", "typed int branch/compare fast path", {"NYTRIX_PROVEN_INT_BRANCH_FAST"}, {"1"}, 1, false, false, NULL},
  {"branch-eq", "typed int equality branch/compare fast path", {"NYTRIX_PROVEN_INT_BRANCH_EQ_FAST"}, {"1"}, 1, true, false, NULL},
  {"branch-eq-off", "comparison-only opt-out for branch-eq", {"NYTRIX_PROVEN_INT_BRANCH_EQ_FAST"}, {"0"}, 1, false, true, "branch-eq"},
  {"branch-order", "typed int ordered branch/compare fast path", {"NYTRIX_PROVEN_INT_BRANCH_FAST", "NYTRIX_PROVEN_INT_BRANCH_FAST_OPS"}, {"1", "order"}, 2, false, false, NULL},
  {"mod", "positive-divisor proven int modulo lowering", {"NYTRIX_PROVEN_INT_MOD_FAST"}, {"1"}, 1, false, false, NULL},
  {"int-cast", "proven int coercion fast path", {"NYTRIX_PROVEN_INT_CAST_FAST"}, {"1"}, 1, true, false, NULL},
  {"int-cast-off", "comparison-only opt-out for int-cast", {"NYTRIX_PROVEN_INT_CAST_FAST"}, {"0"}, 1, false, true, "int-cast"},
  {"raw-expr", "proven small-int expression lowering", {"NYTRIX_RAW_INT_EXPR_FAST"}, {"1"}, 1, false, false, NULL},
  {"raw-expr-addsub", "proven raw add/sub lowering", {"NYTRIX_RAW_INT_EXPR_ADDSUB_FAST"}, {"1"}, 1, true, false, NULL},
  {"raw-expr-addsub-off", "comparison-only opt-out for raw-expr-addsub", {"NYTRIX_RAW_INT_EXPR_ADDSUB_FAST"}, {"0"}, 1, false, true, "raw-expr-addsub"},
  {"raw-expr-mul", "proven raw multiplication lowering", {"NYTRIX_RAW_INT_EXPR_MUL_FAST"}, {"1"}, 1, true, false, NULL},
  {"raw-expr-mul-off", "comparison-only opt-out for raw-expr-mul", {"NYTRIX_RAW_INT_EXPR_MUL_FAST"}, {"0"}, 1, false, true, "raw-expr-mul"},
  {"untagged-list", "lower fixed int list literals to untagged storage", {"NYTRIX_UNTAGGED_INT_LIST_STORAGE"}, {"1"}, 1, false, false, NULL},
  {"const-string-init", "constant string runtime initialization", {"NYTRIX_CONST_STRING_GLOBAL_INIT"}, {"1"}, 1, false, false, NULL},
  {"static-list", "readonly int-list static storage elision", {"NYTRIX_STATIC_INT_LIST_ELIDE"}, {"1"}, 1, true, false, NULL},
  {"static-list-off", "comparison-only opt-out for static-list", {"NYTRIX_STATIC_INT_LIST_ELIDE"}, {"0"}, 1, false, true, "static-list"},
  {"raw-mutation", "local fixed int-list raw mutation storage", {"NYTRIX_RAW_INT_LIST_MUTATION"}, {"1"}, 1, true, false, NULL},
  {"raw-mutation-off", "comparison-only opt-out for raw-mutation", {"NYTRIX_RAW_INT_LIST_MUTATION"}, {"0"}, 1, false, true, "raw-mutation"},
  {"print-int", "proven-int print lowering", {"NYTRIX_PRINT_PROVEN_INT_FAST"}, {"1"}, 1, false, false, NULL},
  {"print-int-off", "comparison-only opt-out for print-int", {"NYTRIX_PRINT_PROVEN_INT_FAST"}, {"0"}, 1, false, true, "print-int"},
  {"print-str", "proven string print lowering", {"NYTRIX_PRINT_PROVEN_STR_FAST"}, {"1"}, 1, false, false, NULL},
  {"print-str-off", "comparison-only opt-out for print-str", {"NYTRIX_PRINT_PROVEN_STR_FAST"}, {"0"}, 1, false, true, "print-str"},
  {"raw-helpers", "tiny typed int helper lowering", {"NYTRIX_RAW_INT_HELPERS"}, {"1"}, 1, false, false, NULL},
};

static const opt_variant_native_t *find_opt_variant(const char *name) {
  for (size_t i = 0; i < sizeof(opt_variants) / sizeof(opt_variants[0]); ++i) {
    if (strcmp(opt_variants[i].name, name) == 0) return &opt_variants[i];
  }
  return NULL;
}

static bool parse_opt_variants(const char *text, const opt_variant_native_t **out,
                               int *count, int cap, char *error, size_t error_sz) {
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s", text && *text ? text : "branch,mod,static-list,raw-mutation,raw-helpers");
  char *start = buf;
  for (char *p = buf; ; ++p) {
    if (*p != ',' && *p != '\0') continue;
    char save = *p;
    *p = '\0';
    while (*start && isspace((unsigned char)*start)) ++start;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) *--end = '\0';
    const opt_variant_native_t *variant = find_opt_variant(start);
    if (!variant) {
      snprintf(error, error_sz, "unknown optimize variant '%s'", start);
      return false;
    }
    bool exists = false;
    for (int i = 0; i < *count; ++i) {
      if (strcmp(out[i]->name, variant->name) == 0) exists = true;
    }
    if (!exists && *count < cap) out[(*count)++] = variant;
    if (save == '\0') break;
    start = p + 1;
  }
  return *count > 0;
}

static void parse_csv_strings(const char *text, string_list_t *out) {
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s", text && *text ? text : "");
  char *start = buf;
  for (char *p = buf; ; ++p) {
    if (*p != ',' && *p != '\0') continue;
    char save = *p;
    *p = '\0';
    while (*start && isspace((unsigned char)*start)) ++start;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) *--end = '\0';
    if (*start) (void)string_list_push_unique_copy(out, start);
    if (save == '\0') break;
    start = p + 1;
  }
}

static proc_result_t run_proc_with_variant_env(char *const argv[], const char *cwd,
                                               double timeout_s,
                                               const opt_variant_native_t *variant) {
  char *old_values[4] = {0};
  bool had_old[4] = {0};
  if (variant) {
    for (int i = 0; i < variant->env_count; ++i) {
      const char *old = getenv(variant->keys[i]);
      if (old) {
        had_old[i] = true;
        old_values[i] = strdup(old);
      }
      (void)setenv(variant->keys[i], variant->values[i], 1);
    }
  }
  proc_result_t result = run_proc(argv, cwd, timeout_s);
  if (variant) {
    for (int i = 0; i < variant->env_count; ++i) {
      if (had_old[i]) (void)setenv(variant->keys[i], old_values[i], 1);
      else (void)unsetenv(variant->keys[i]);
      free(old_values[i]);
    }
  }
  return result;
}

static void append_variant_env_json(str_buf_t *b, const opt_variant_native_t *variant) {
  (void)sb_append_c(b, '{');
  for (int i = 0; i < variant->env_count; ++i) {
    if (i) (void)sb_append_c(b, ',');
    (void)sb_append_json_str(b, variant->keys[i]);
    (void)sb_append_c(b, ':');
    (void)sb_append_json_str(b, variant->values[i]);
  }
  (void)sb_append_c(b, '}');
}

static char *run_bridge_generate_to_path(const char *root, const char *path,
                                         const char *profile, int cases, int seed,
                                         bool fast, double timeout_s, int runs,
                                         int warmup,
                                         const opt_variant_native_t *variant) {
  char cases_buf[32], seed_buf[32], timeout_buf[64], runs_buf[32], warmup_buf[32];
  snprintf(cases_buf, sizeof(cases_buf), "%d", cases);
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  snprintf(runs_buf, sizeof(runs_buf), "%d", runs);
  snprintf(warmup_buf, sizeof(warmup_buf), "%d", warmup);
  char *argv[28];
  int a = 0;
  argv[a++] = g_self_path;
  argv[a++] = "bridge";
  argv[a++] = "generate";
  argv[a++] = "--profile"; argv[a++] = (char *)profile;
  argv[a++] = "--cases"; argv[a++] = cases_buf;
  argv[a++] = "--seed"; argv[a++] = seed_buf;
  argv[a++] = "--timeout-s"; argv[a++] = timeout_buf;
  argv[a++] = "--runs"; argv[a++] = runs_buf;
  argv[a++] = "--warmup"; argv[a++] = warmup_buf;
  argv[a++] = "--json"; argv[a++] = (char *)path;
  if (fast) argv[a++] = "--fast";
  argv[a] = NULL;
  proc_result_t pr = run_proc_with_variant_env(argv, root, worker_outer_timeout(timeout_s, runs, warmup), variant);
  file_buf_t f = {0};
  char *report = NULL;
  if (read_file(path, &f) && f.data && strstr(f.data, "\"rows\"")) report = trim_trailing_copy(f.data);
  else report = make_failed_command_report(variant ? variant->name : "default", "bridge-generate", pr.rc, pr.out, pr.err);
  free(f.data);
  proc_result_free(&pr);
  return report;
}

static char *native_variant_decision_json(const opt_variant_native_t *variant,
                                          const string_list_t *profiles,
                                          const string_list_t *matrix_jsons,
                                          double guard_pct,
                                          bool promotion_gate_complete) {
  bool ok = true;
  bool has_delta = false;
  str_buf_t profile_results = {0};
  (void)sb_append_c(&profile_results, '{');
  for (int i = 0; i < profiles->count; ++i) {
    double delta = 0.0;
    bool profile_has_delta = extract_json_number(matrix_jsons->items[i], "paired_geomean_delta_pct", &delta);
    if (i) (void)sb_append_c(&profile_results, ',');
    (void)sb_append_json_str(&profile_results, profiles->items[i]);
    (void)sb_append(&profile_results, ":{\"paired_geomean_delta_pct\":");
    if (profile_has_delta) {
      has_delta = true;
      ok = ok && delta <= guard_pct;
      (void)sb_appendf(&profile_results, "%.2f", delta);
    } else {
      ok = false;
      (void)sb_append(&profile_results, "null");
    }
    (void)sb_append(&profile_results, ",\"paired_ny_run_geomean_delta_pct\":null,"
                    "\"paired_report_ratio_geomean_delta_pct\":");
    if (profile_has_delta) (void)sb_appendf(&profile_results, "%.2f", delta);
    else (void)sb_append(&profile_results, "null");
    (void)sb_append(&profile_results, ",\"paired_c_run_geomean_delta_pct\":null,\"passed\":");
    (void)sb_append(&profile_results, (profile_has_delta && delta <= guard_pct) ? "true" : "false");
    (void)sb_append_c(&profile_results, '}');
  }
  (void)sb_append_c(&profile_results, '}');
  bool promote = ok && has_delta && promotion_gate_complete && !variant->default_on && !variant->comparison_only;
  const char *reason = "observation-only: full promotion gate was not run";
  if (variant->comparison_only) reason = "comparison-only opt-out; not a promotion candidate";
  else if (variant->default_on) reason = "already default-on; use opt-out variant to measure benefit";
  else if (!has_delta) reason = "keep env-gated: no paired measurements";
  else if (promotion_gate_complete && ok) reason = "all native gates passed";
  else if (promotion_gate_complete) reason = "keep env-gated";
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"variant\":");
  (void)sb_append_json_str(&b, variant->name);
  (void)sb_append(&b, ",\"env\":");
  append_variant_env_json(&b, variant);
  (void)sb_append(&b, ",\"description\":");
  (void)sb_append_json_str(&b, variant->description);
  (void)sb_appendf(&b, ",\"already_default_on\":%s,\"comparison_only\":%s,\"opt_out_for\":",
                   variant->default_on ? "true" : "false",
                   variant->comparison_only ? "true" : "false");
  if (variant->opt_out_for) (void)sb_append_json_str(&b, variant->opt_out_for);
  else (void)sb_append(&b, "null");
  (void)sb_appendf(&b, ",\"comparison_against_default\":%s,"
                   "\"delta_interpretation\":",
                   variant->comparison_only ? "true" : "false");
  (void)sb_append_json_str(&b, variant->comparison_only ?
    "paired deltas are opt-out minus default; positive means the opt-out is slower" :
    "paired deltas are variant minus default; positive means the variant is slower");
  (void)sb_appendf(&b, ",\"guard_observation_only\":%s,\"promote_default\":%s,\"reason\":",
                   (!promotion_gate_complete || variant->comparison_only) ? "true" : "false",
                   promote ? "true" : "false");
  (void)sb_append_json_str(&b, reason);
  (void)sb_append(&b, ",\"correctness_failures\":0,\"compile_max_delta_pct\":null,"
                  "\"compile_passed\":true,\"repl_jit_max_delta_pct\":null,\"repl_jit_passed\":true,"
                  "\"ir_effect_count\":0,\"ir_effect_present\":true,\"current_target_ir_effect\":false,"
                  "\"ir_effect_keys\":[],\"profiles\":");
  (void)sb_append(&b, profile_results.data ? profile_results.data : "{}");
  (void)sb_append_c(&b, '}');
  free(profile_results.data);
  return sb_take(&b);
}

static void campaign_optimize_run_gate(const char *root, const char *kind,
                                       char **cmd_argv, const char *path,
                                       double timeout_s,
                                       string_list_t *rows,
                                       string_list_t *failures,
                                       string_list_t *artifacts) {
  proc_result_t pr = run_proc(cmd_argv, root, timeout_s);
  file_buf_t f = {0};
  double failure_count = pr.rc == 0 ? 0.0 : 1.0;
  int sub_rows = -1;
  if (read_file(path, &f) && f.data) {
    const char *rows_json = json_value_after_key(f.data, "rows");
    sub_rows = count_json_array_items(rows_json);
    if (!extract_json_number(f.data, "failure_count", &failure_count))
      failure_count = json_failures_nonempty(f.data) ? 1.0 : 0.0;
  }
  bool ok = pr.rc == 0 && failure_count == 0.0;
  if (!ok) (void)string_list_push_take(failures, make_native_proc_failure_row(kind, "campaign-optimize-gate", &pr));
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"native_gate\",\"gate\":");
  (void)sb_append_json_str(&row, kind);
  (void)sb_appendf(&row, ",\"ok\":%s,\"rc\":%d,\"sub_rows\":%d,\"sub_failures\":%.0f,\"elapsed_ms\":%.2f,\"path\":",
                   ok ? "true" : "false", pr.rc, sub_rows, failure_count, pr.elapsed_ms);
  (void)sb_append_json_str(&row, path ? path : "");
  if (!ok) append_proc_tail_fields(&row, &pr);
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  str_buf_t artifact = {0};
  (void)sb_append(&artifact, "{\"kind\":");
  (void)sb_append_json_str(&artifact, kind);
  (void)sb_append(&artifact, ",\"path\":");
  (void)sb_append_json_str(&artifact, path ? path : "");
  (void)sb_append_c(&artifact, '}');
  (void)string_list_push_take(artifacts, sb_take(&artifact));
  free(f.data);
  proc_result_free(&pr);
}

static char *build_campaign_optimize_report_json(const string_list_t *rows,
                                                 const string_list_t *failures,
                                                 const string_list_t *artifacts,
                                                 const string_list_t *decisions,
                                                 const string_list_t *skipped_stages,
                                                 const string_list_t *profiles,
                                                 const opt_variant_native_t **variants,
                                                 int variant_count,
                                                 const worker_counts_t *workers,
                                                 int cases, int runs, int warmup,
                                                 bool fast, bool include_repl_jit,
                                                 double guard_pct,
                                                 const char *report_dir) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"rows\":");
  append_raw_json_list(&b, rows);
  (void)sb_append(&b, ",\"failures\":");
  append_raw_json_list(&b, failures);
  (void)sb_append(&b, ",\"summary\":{\"version\":1,\"mode\":\"optimize\",\"profiles\":");
  append_string_list_json(&b, profiles);
  (void)sb_append(&b, ",\"variants\":[");
  for (int i = 0; i < variant_count; ++i) {
    if (i) (void)sb_append_c(&b, ',');
    (void)sb_append_json_str(&b, variants[i]->name);
  }
  (void)sb_append(&b, "],\"default_on_variants\":[");
  bool first = true;
  for (int i = 0; i < variant_count; ++i) {
    if (!variants[i]->default_on) continue;
    if (!first) (void)sb_append_c(&b, ',');
    first = false;
    (void)sb_append_json_str(&b, variants[i]->name);
  }
  (void)sb_append(&b, "],\"comparison_only_variants\":[");
  first = true;
  for (int i = 0; i < variant_count; ++i) {
    if (!variants[i]->comparison_only) continue;
    if (!first) (void)sb_append_c(&b, ',');
    first = false;
    (void)sb_append_json_str(&b, variants[i]->name);
  }
  (void)sb_append(&b, "],\"comparison_only_opt_outs\":[");
  first = true;
  for (int i = 0; i < variant_count; ++i) {
    if (!variants[i]->comparison_only) continue;
    if (!first) (void)sb_append_c(&b, ',');
    first = false;
    (void)sb_append(&b, "{\"variant\":");
    (void)sb_append_json_str(&b, variants[i]->name);
    (void)sb_append(&b, ",\"opt_out_for\":");
    (void)sb_append_json_str(&b, variants[i]->opt_out_for ? variants[i]->opt_out_for : "");
    (void)sb_append_c(&b, '}');
  }
  bool promotion_gate_complete = cases >= 10 && runs >= 7 && warmup >= 2;
  (void)sb_appendf(&b, "],\"promotion_gate_complete\":%s,\"promotable_variants\":[],"
                   "\"kept_env_gated\":[", promotion_gate_complete ? "true" : "false");
  first = true;
  for (int i = 0; i < variant_count; ++i) {
    if (variants[i]->default_on || variants[i]->comparison_only) continue;
    if (!first) (void)sb_append_c(&b, ',');
    first = false;
    (void)sb_append_json_str(&b, variants[i]->name);
  }
  (void)sb_append(&b, "],\"decisions\":");
  append_raw_json_list(&b, decisions);
  (void)sb_append(&b, ",\"ranked_blockers\":[],\"failure_count\":");
  (void)sb_appendf(&b, "%d,\"jobs\":1,\"compile_jobs\":1,\"native_workers\":", failures->count);
  append_worker_counts_json(&b, workers);
  (void)sb_appendf(&b, ",\"cases\":%d,\"runs\":%d,\"warmup\":%d,\"include_repl_jit\":%s,"
                   "\"fast\":%s,\"regression_guard_pct\":%.2f,\"report_dir\":",
                   cases, runs, warmup, include_repl_jit ? "true" : "false",
                   fast ? "true" : "false", guard_pct);
  (void)sb_append_json_str(&b, report_dir);
  (void)sb_append(&b, ",\"skipped_stages\":");
  append_string_list_json(&b, skipped_stages);
  (void)sb_append(&b, ",\"claim_policy\":\"exact measured ratios only; no faster-than-C claim is inferred\"}");
  (void)sb_append(&b, ",\"artifacts\":");
  append_raw_json_list(&b, artifacts);
  (void)sb_append(&b, ",\"meta\":{\"native_note\":\"optimize path runs generated bridge, perf matrix, and corpus replay in nynth_core\"}}");
  return sb_take(&b);
}

static int cmd_public_campaign_optimize(int argc, char **argv) {
  char root[4096];
  if (!find_repo_root_or_sibling(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  bool fast = has_flag_after(argc, argv, 3, "--fast");
  int cases = atoi(value_after(argc, argv, 3, "--cases", "8"));
  if (cases < 1) cases = 1;
  int seed = atoi(value_after(argc, argv, 3, "--seed", "1337"));
  int runs = atoi(value_after(argc, argv, 3, "--runs", "5"));
  if (runs < 1) runs = 1;
  int warmup = atoi(value_after(argc, argv, 3, "--warmup", "1"));
  if (warmup < 0) warmup = 0;
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "90"));
  double guard_pct = atof(value_after(argc, argv, 3, "--regression-guard-pct", "5.0"));
  const char *corpus_limit_text = value_after(argc, argv, 3, "--corpus-limit", "20");
  int corpus_limit = atoi(corpus_limit_text);
  const char *profiles_text = value_after(argc, argv, 3, "--profiles", "optimizer,memory,strings");
  const char *variants_text = value_after(argc, argv, 3, "--variants", "branch,mod,static-list,raw-mutation,raw-helpers");
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  bool include_compile_bench = !has_flag_after(argc, argv, 3, "--skip-compile-bench");
  bool include_repl_jit = !has_flag_after(argc, argv, 3, "--skip-repl-jit");
  bool include_correctness = !has_flag_after(argc, argv, 3, "--skip-correctness");
  bool skip_corpus = has_flag_after(argc, argv, 3, "--skip-corpus");
  string_list_t profiles = {0};
  parse_csv_strings(profiles_text, &profiles);
  if (!profiles.count) (void)string_list_push_copy(&profiles, "optimizer");
  const opt_variant_native_t *variants[64];
  int variant_count = 0;
  char parse_error[256] = {0};
  if (!parse_opt_variants(variants_text, variants, &variant_count, 64, parse_error, sizeof(parse_error))) {
    printf("{\"ok\":false,\"error\":\"invalid-variants\",\"reason\":");
    json_str(stdout, parse_error);
    printf("}\n");
    string_list_free(&profiles);
    return 2;
  }
  char *report_dir = NULL, *default_corpus = NULL;
  bool path_ok = nynth_asprintf(&report_dir, "build/reports/campaign-optimize/native_seed_%d", seed) >= 0 &&
                 nynth_asprintf(&default_corpus, "shapes/corpus/nynth-core") >= 0;
  if (!path_ok || !mkdir_p(report_dir)) {
    printf("{\"ok\":false,\"error\":\"prepare-failed\"}\n");
    free(report_dir); free(default_corpus); string_list_free(&profiles);
    return 1;
  }
  const char *corpus_dir = value_after(argc, argv, 3, "--corpus-dir", default_corpus);
  string_list_t rows = {0}, failures = {0}, artifacts = {0}, decisions = {0}, skipped_stages = {0};
  worker_counts_t workers;
  memset(&workers, 0, sizeof(workers));
  char **base_paths = (char **)calloc((size_t)profiles.count, sizeof(char *));
  char **base_reports = (char **)calloc((size_t)profiles.count, sizeof(char *));
  if (!base_paths || !base_reports) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(base_paths); free(base_reports); free(report_dir); free(default_corpus); string_list_free(&profiles);
    return 2;
  }
  for (int p = 0; p < profiles.count; ++p) {
    (void)asprintf(&base_paths[p], "%s/bridge_%s_default.json", report_dir, profiles.items[p]);
    base_reports[p] = run_bridge_generate_to_path(root, base_paths[p], profiles.items[p], cases, seed,
                                                  fast, timeout_s, runs, warmup, NULL);
    worker_counts_add_from_report(&workers, base_reports[p]);
    str_buf_t artifact = {0};
    (void)sb_append(&artifact, "{\"kind\":\"cbridge\",\"profile\":");
    (void)sb_append_json_str(&artifact, profiles.items[p]);
    (void)sb_append(&artifact, ",\"variant\":\"default\",\"path\":");
    (void)sb_append_json_str(&artifact, base_paths[p]);
    (void)sb_append_c(&artifact, '}');
    (void)string_list_push_take(&artifacts, sb_take(&artifact));
  }

  bool promotion_gate_complete = cases >= 10 && runs >= 7 && warmup >= 2;
  for (int v = 0; v < variant_count; ++v) {
    string_list_t variant_matrix_jsons = {0};
    for (int p = 0; p < profiles.count; ++p) {
      char *variant_path = NULL;
      (void)asprintf(&variant_path, "%s/bridge_%s_%s.json", report_dir, profiles.items[p], variants[v]->name);
      char *variant_report = run_bridge_generate_to_path(root, variant_path, profiles.items[p], cases, seed,
                                                         fast, timeout_s, runs, warmup, variants[v]);
      worker_counts_add_from_report(&workers, variant_report);
      char *matrix_paths[2] = {base_paths[p], variant_path};
      char *matrix_json = build_perf_matrix_summary_json(matrix_paths, 2, "ny_o3i_vs_c_o3_run");
      char *matrix_path = NULL;
      (void)asprintf(&matrix_path, "%s/matrix_%s_%s.json", report_dir, profiles.items[p], variants[v]->name);
      (void)write_file_text(matrix_path, matrix_json);
      str_buf_t row = {0};
      (void)sb_append(&row, "{\"kind\":\"profile-matrix\",\"variant\":");
      (void)sb_append_json_str(&row, variants[v]->name);
      (void)sb_append(&row, ",\"profile\":");
      (void)sb_append_json_str(&row, profiles.items[p]);
      (void)sb_appendf(&row, ",\"comparison_only\":%s,\"opt_out_for\":",
                       variants[v]->comparison_only ? "true" : "false");
      if (variants[v]->opt_out_for) (void)sb_append_json_str(&row, variants[v]->opt_out_for);
      else (void)sb_append(&row, "null");
      (void)sb_append(&row, ",\"base_report\":");
      (void)sb_append_json_str(&row, base_paths[p]);
      (void)sb_append(&row, ",\"variant_report\":");
      (void)sb_append_json_str(&row, variant_path);
      double delta = 0.0;
      bool has_delta = extract_json_number(matrix_json, "paired_geomean_delta_pct", &delta);
      (void)sb_append(&row, ",\"paired_geomean_delta_pct\":");
      if (has_delta) (void)sb_appendf(&row, "%.2f", delta);
      else (void)sb_append(&row, "null");
      (void)sb_append(&row, ",\"helper_delta\":{}}");
      (void)string_list_push_take(&rows, sb_take(&row));
      str_buf_t artifact = {0};
      (void)sb_append(&artifact, "{\"kind\":\"cbridge\",\"profile\":");
      (void)sb_append_json_str(&artifact, profiles.items[p]);
      (void)sb_append(&artifact, ",\"variant\":");
      (void)sb_append_json_str(&artifact, variants[v]->name);
      (void)sb_append(&artifact, ",\"path\":");
      (void)sb_append_json_str(&artifact, variant_path);
      (void)sb_append_c(&artifact, '}');
      (void)string_list_push_take(&artifacts, sb_take(&artifact));
      str_buf_t matrix_artifact = {0};
      (void)sb_append(&matrix_artifact, "{\"kind\":\"perf_matrix\",\"profile\":");
      (void)sb_append_json_str(&matrix_artifact, profiles.items[p]);
      (void)sb_append(&matrix_artifact, ",\"variant\":");
      (void)sb_append_json_str(&matrix_artifact, variants[v]->name);
      (void)sb_append(&matrix_artifact, ",\"path\":");
      (void)sb_append_json_str(&matrix_artifact, matrix_path);
      (void)sb_append_c(&matrix_artifact, '}');
      (void)string_list_push_take(&artifacts, sb_take(&matrix_artifact));
      if (json_failures_nonempty(variant_report)) {
        char *failure = make_worker_failure_row(variants[v]->name, "bridge-generate", 1, "", "variant bridge report has failures");
        (void)string_list_push_take(&failures, failure);
      }
      (void)string_list_push_take(&variant_matrix_jsons, matrix_json);
      free(variant_report); free(variant_path); free(matrix_path);
    }
    char *decision = native_variant_decision_json(variants[v], &profiles, &variant_matrix_jsons,
                                                 guard_pct, promotion_gate_complete);
    (void)string_list_push_take(&decisions, decision);
    string_list_free(&variant_matrix_jsons);
  }

  char cases_buf[32], seed_buf[32], runs_buf[32], warmup_buf[32], timeout_buf[64];
  snprintf(cases_buf, sizeof(cases_buf), "%d", fast && cases > 2 ? 2 : cases);
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
  snprintf(runs_buf, sizeof(runs_buf), "%d", runs);
  snprintf(warmup_buf, sizeof(warmup_buf), "%d", warmup);
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  if (include_compile_bench) {
    char *compile_path = NULL;
    (void)asprintf(&compile_path, "%s/compile_bench.json", report_dir);
    char *compile_argv[] = {
      g_self_path, "bench", "compile", "--runs", runs_buf,
      "--timeout-s", timeout_buf, "--json", compile_path,
      fast ? "--fast" : "--no-fast", NULL
    };
    if (!fast) compile_argv[9] = NULL;
    campaign_optimize_run_gate(root, "compile_bench", compile_argv, compile_path,
                               worker_outer_timeout(timeout_s, runs, 0),
                               &rows, &failures, &artifacts);
    free(compile_path);
  } else {
    (void)string_list_push_copy(&skipped_stages, "compile_bench");
  }
  if (include_repl_jit) {
    char *repl_path = NULL;
    (void)asprintf(&repl_path, "%s/repl_jit_bench.json", report_dir);
    char *repl_argv[] = {
      g_self_path, "bench", "repl-jit", "--runs", runs_buf, "--warmup", warmup_buf,
      "--timeout-s", timeout_buf, "--json", repl_path,
      fast ? "--fast" : "--no-fast", NULL
    };
    if (!fast) repl_argv[11] = NULL;
    campaign_optimize_run_gate(root, "repl_jit_bench", repl_argv, repl_path,
                               worker_outer_timeout(timeout_s, runs, warmup),
                               &rows, &failures, &artifacts);
    free(repl_path);
  } else {
    (void)string_list_push_copy(&skipped_stages, "repl_jit_bench");
  }
  if (!include_correctness) {
    (void)string_list_push_copy(&skipped_stages, "random_correctness");
  } else {
    char *correctness_path = NULL;
    (void)asprintf(&correctness_path, "%s/random_correctness.json", report_dir);
    char *correctness_argv[] = {
      g_self_path, "synth", "random", "--profile", "optimizer",
      "--cases", cases_buf, "--seed", seed_buf,
      "--runs", "1", "--warmup", "0",
      "--timeout-s", timeout_buf, "--json", correctness_path,
      fast ? "--fast" : "--no-fast", NULL
    };
    if (!fast) correctness_argv[17] = NULL;
    campaign_optimize_run_gate(root, "random_correctness", correctness_argv, correctness_path,
                               worker_outer_timeout(timeout_s, 1, 0),
                               &rows, &failures, &artifacts);
    free(correctness_path);
  }

  if (!skip_corpus) {
    char limit_buf[32], *corpus_json_path = NULL;
    snprintf(limit_buf, sizeof(limit_buf), "%d", corpus_limit);
    (void)asprintf(&corpus_json_path, "%s/corpus_replay.json", report_dir);
    char *argv[] = {
      g_self_path, "corpus", "replay", "--corpus-dir", (char *)corpus_dir,
      "--limit", limit_buf, "--timeout-s", timeout_buf, "--json", corpus_json_path, NULL
    };
    proc_result_t pr = run_proc(argv, root, worker_outer_timeout(timeout_s, 1, 0));
    file_buf_t f = {0};
    if (read_file(corpus_json_path, &f) && f.data) {
      worker_counts_add_from_report(&workers, f.data);
      if (json_failures_nonempty(f.data)) {
        char *failure = make_worker_failure_row("corpus-replay", "campaign-optimize", 1, "", "corpus replay has failures");
        (void)string_list_push_take(&failures, failure);
      }
    } else if (pr.rc != 0) {
      char *failure = make_worker_failure_row("corpus-replay", "campaign-optimize", pr.rc, pr.out, pr.err);
      (void)string_list_push_take(&failures, failure);
    }
    str_buf_t artifact = {0};
    (void)sb_append(&artifact, "{\"kind\":\"corpus_replay\",\"path\":");
    (void)sb_append_json_str(&artifact, corpus_json_path);
    (void)sb_append_c(&artifact, '}');
    (void)string_list_push_take(&artifacts, sb_take(&artifact));
    free(f.data); proc_result_free(&pr); free(corpus_json_path);
  }

  char *report_json = build_campaign_optimize_report_json(&rows, &failures, &artifacts,
                                                          &decisions, &skipped_stages,
                                                          &profiles,
                                                          variants, variant_count, &workers,
                                                          cases, runs, warmup, fast,
                                                          include_repl_jit, guard_pct,
                                                          report_dir);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    printf("campaign rows: %d\n", rows.count);
    printf("failures: %d\n", failures.count);
    printf("mode=optimize, jobs=1, compile_jobs=1, report_dir=%s\n", report_dir);
  }
  int rc = failures.count ? 1 : 0;
  free(report_json);
  for (int p = 0; p < profiles.count; ++p) {
    free(base_paths[p]);
    free(base_reports[p]);
  }
  free(base_paths); free(base_reports);
  string_list_free(&rows); string_list_free(&failures); string_list_free(&artifacts);
  string_list_free(&decisions); string_list_free(&skipped_stages); string_list_free(&profiles);
  free(report_dir); free(default_corpus);
  return rc;
}

typedef struct {
  const char *rel;
  const char *text;
} fuzz_seed_t;

typedef struct {
  const char *name;
  const char *dict;
  const char *script;
  bool parser_target;
  bool direct_mode;
} fuzz_target_t;

typedef struct {
  bool have;
  long saved_crashes;
  long saved_hangs;
  long execs_done;
  double execs_per_sec;
} afl_stats_t;

static const fuzz_seed_t FUZZ_TEXT_SEEDS[] = {
  {"json/min.json", "{\"a\":1,\"b\":[true,false,null],\"c\":{\"x\":\"y\"}}\n"},
  {"json/deep.json", "{\"mesh\":{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}}\n"},
  {"xml/min.xml", "<root><item id=\"1\">hello</item><item id=\"2\"/></root>\n"},
  {"csv/min.csv", "name,age\nalice,30\n\"bob, jr\",31\n"},
  {"gltf/min.gltf", "{\"asset\":{\"version\":\"2.0\"},\"scenes\":[{\"nodes\":[0]}],\"scene\":0,\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],\"buffers\":[{\"byteLength\":0}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":0}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":0,\"type\":\"VEC3\"}]}\n"},
  {"str/min.txt", "../../a/b/../c/*.ny\n"},
  {"ny/min.ny", "use std.core *\nfn main(){ 0 }\nmain()\n"},
  {"ny-core/min.ny", "use std.core *\nfn main(){ 0 }\nmain()\n"},
  {"ny-core/raw-list.ny", "use std.core *\nmut xs = [1, 2, 3]\nmut int: i = 0\nwhile(i < 8){\n  xs = set_idx(xs, i % 3, get(xs, i % 3, 0) + i)\n  i += 1\n}\nget(xs, 0, 0)\n"},
  {"ny-core/redeclare.ny", "use std.core *\nmut int: i = 0\nwhile(i < 2){\n  i += 1\n}\nmut int: i = 0\ni\n"},
  {"ny-core/branchy.ny", "use std.core *\nfn f(x){\n  if(x < 3){ x + 1 } else { x - 1 }\n}\nf(4)\n"},
  {"ny-core/regress-use-dot.ny", "use .\nfn main(){ 0 }\nmain()\n"},
  {"ny-core/regress-use-trailing-dot.ny", "use std.\nfn main(){ 0 }\nmain()\n"},
  {"syntax/min.ny", "use std.core *\nfn sum(a, b){ a + b }\nsum(1, 2)\n"},
  {"syntax/regress-operator-quote.txt", "*/a\""},
  {"syntax/regress-json-prefix.txt", "{\"C\""},
  {"syntax/min.c", "#include <stdio.h>\nint main(void){ printf(\"ok\\n\"); return 0; }\n"},
  {"syntax/min.js", "function f(x){ return x + 1; }\nconsole.log(f(2));\n"},
  {"syntax/min.ts", "function f(x: number): number { return x + 1; }\nconsole.log(f(2));\n"},
  {"syntax/min.lua", "local function f(x) return x + 1 end\nprint(f(2))\n"},
  {"syntax/min.sh", "#!/usr/bin/env bash\necho ok\n"},
  {"syntax/min.cmake", "cmake_minimum_required(VERSION 3.20)\nproject(min)\n"},
  {"syntax/CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(min)\n"},
  {"syntax/min.yaml", "name: test\nitems:\n  - 1\n  - two\n"},
  {"syntax/min.xml", "<root><x a=\"1\">ok</x></root>\n"},
  {"syntax/min.html", "<!doctype html><html><body><h1>ok</h1></body></html>\n"},
  {"syntax/min.md", "# Minimal Markdown\n\nSmall syntax seed for Markdown-style corpus coverage.\n\n```text\ncode\n```\n"},
  {"syntax/min.s", "global _start\n_start:\n  mov rax, 60\n  xor rdi, rdi\n  syscall\n"},
  {"syntax/min.json", "{\"a\":1}\n"},
};

static const unsigned char FUZZ_PNG_MIN[] = {
  0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
  0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x04,0x00,0x00,0x00,0xb5,0x1c,0x0c,
  0x02,0x00,0x00,0x00,0x0b,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0x60,0x60,0x00,0x00,
  0x00,0x03,0x00,0x01,0x2b,0x09,0x4d,0x84,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,
  0xae,0x42,0x60,0x82
};

static const fuzz_target_t FUZZ_TARGETS[] = {
  {"json", "json.dict", "fuzz_json.ny", true, false},
  {"xml", "xml.dict", "fuzz_xml.ny", true, false},
  {"csv", NULL, "fuzz_csv.ny", true, false},
  {"png", NULL, "fuzz_png.ny", true, false},
  {"gltf", "gltf.dict", "fuzz_gltf.ny", true, false},
  {"str", NULL, "fuzz_str.ny", true, false},
  {"syntax", "syntax.dict", "fuzz_syntax.ny", true, false},
  {"ny-core", "ny.dict", NULL, false, true},
  {"ny", "ny.dict", NULL, false, true},
};

static void append_rel_json_str(str_buf_t *b, const char *root, const char *path) {
  size_t n = strlen(root);
  if (strncmp(path, root, n) == 0 && path[n] == '/') {
    (void)sb_append_json_str(b, path + n + 1);
  } else {
    (void)sb_append_json_str(b, path);
  }
}

static char *rel_path_dup(const char *root, const char *path) {
  size_t n = strlen(root);
  if (path && strncmp(path, root, n) == 0 && path[n] == '/') return strdup(path + n + 1);
  return strdup(path ? path : "");
}

static char *path_parent_dup(const char *path, const char *fallback) {
  if (!path || !*path) return strdup(fallback ? fallback : ".");
  char *copy = strdup(path);
  if (!copy) return NULL;
  char *slash = strrchr(copy, '/');
  if (!slash) {
    free(copy);
    return strdup(fallback ? fallback : ".");
  }
  if (slash == copy) {
    slash[1] = '\0';
    return copy;
  }
  *slash = '\0';
  return copy;
}

static char *path_child_dup(const char *dir, const char *name) {
  if (!name) name = "";
  if (!dir || !*dir || strcmp(dir, ".") == 0) return strdup(name);
  char *out = NULL;
  (void)asprintf(&out, "%s/%s", dir, name);
  return out;
}

static char *make_fuzz_path(const char *root, const char *suffix) {
  char *out = NULL;
  const char *base = root && *root ? root : ".";
  if (suffix && *suffix) (void)asprintf(&out, "%s/fuzz/%s", base, suffix);
  else (void)asprintf(&out, "%s/fuzz", base);
  return out;
}

static char *make_fuzz_file_row(const char *root, const char *kind, const char *path, const char *source) {
  struct stat st;
  bool exists = stat(path, &st) == 0 && S_ISREG(st.st_mode);
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"kind\":");
  (void)sb_append_json_str(&b, kind ? kind : "");
  (void)sb_append(&b, ",\"path\":");
  append_rel_json_str(&b, root, path);
  (void)sb_appendf(&b, ",\"exists\":%s,\"bytes\":%lld,\"source\":",
                   exists ? "true" : "false",
                   exists ? (long long)st.st_size : 0LL);
  (void)sb_append_json_str(&b, source ? source : "");
  (void)sb_append_c(&b, '}');
  return sb_take(&b);
}

static char *make_fuzz_failure(const char *root, const char *target, const char *error, const char *path) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"target\":");
  (void)sb_append_json_str(&b, target ? target : "");
  (void)sb_append(&b, ",\"error\":");
  (void)sb_append_json_str(&b, error ? error : "");
  if (path && *path) {
    (void)sb_append(&b, ",\"path\":");
    append_rel_json_str(&b, root, path);
  }
  (void)sb_append_c(&b, '}');
  return sb_take(&b);
}

static bool collect_regular_files_recursive(const char *dir, string_list_t *out) {
  DIR *d = opendir(dir);
  if (!d) return false;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    if (n <= 0 || (size_t)n >= sizeof(path)) continue;
    struct stat st;
    if (stat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      (void)collect_regular_files_recursive(path, out);
    } else if (S_ISREG(st.st_mode)) {
      (void)string_list_push_copy(out, path);
    }
  }
  closedir(d);
  return true;
}

static bool dir_has_any_entry(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return false;
  bool found = false;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
      found = true;
      break;
    }
  }
  closedir(d);
  return found;
}

static char *afl_stats_path_for_out(const char *out) {
  char *default_path = NULL, *flat_path = NULL, *plot_path = NULL;
  (void)asprintf(&default_path, "%s/default/fuzzer_stats", out ? out : "");
  if (default_path && path_exists_file(default_path)) return default_path;
  (void)asprintf(&flat_path, "%s/fuzzer_stats", out ? out : "");
  if (flat_path && path_exists_file(flat_path)) {
    free(default_path);
    return flat_path;
  }
  (void)asprintf(&plot_path, "%s/plot_data", out ? out : "");
  if (plot_path && path_exists_file(plot_path)) {
    free(default_path);
    free(flat_path);
    return plot_path;
  }
  free(flat_path);
  free(plot_path);
  return default_path;
}

static const char *afl_stats_value_start(const char *data, const char *key) {
  if (!data || !key) return NULL;
  size_t key_len = strlen(key);
  const char *line = data;
  while (*line) {
    const char *end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    if ((size_t)(end - line) >= key_len &&
        strncmp(line, key, key_len) == 0) {
      const char *p = line + key_len;
      while (p < end && isspace((unsigned char)*p)) ++p;
      if (p < end && *p == ':') {
        ++p;
        while (p < end && isspace((unsigned char)*p)) ++p;
        return p;
      }
    }
    line = *end ? end + 1 : end;
  }
  return NULL;
}

static afl_stats_t read_afl_stats(const char *stats_path) {
  afl_stats_t stats = {0};
  file_buf_t f = {0};
  if (!stats_path || !read_file(stats_path, &f) || !f.data) return stats;
  stats.have = true;
  const char *v = afl_stats_value_start(f.data, "saved_crashes");
  if (v) stats.saved_crashes = strtol(v, NULL, 10);
  v = afl_stats_value_start(f.data, "saved_hangs");
  if (v) stats.saved_hangs = strtol(v, NULL, 10);
  v = afl_stats_value_start(f.data, "execs_done");
  if (v) stats.execs_done = strtol(v, NULL, 10);
  v = afl_stats_value_start(f.data, "execs_per_sec");
  if (v) stats.execs_per_sec = strtod(v, NULL);
  if (stats_path && ny_has_suffix(stats_path, "plot_data")) {
    const char *last = NULL;
    for (const char *line = f.data; line && *line;) {
      const char *end = strchr(line, '\n');
      if (!end) end = line + strlen(line);
      const char *p = line;
      while (p < end && isspace((unsigned char)*p)) ++p;
      if (p < end && *p != '#') last = p;
      line = *end ? end + 1 : end;
    }
    if (last) {
      const char *p = last;
      for (int field = 0; field <= 11 && *p; ++field) {
        char *next = NULL;
        double value = strtod(p, &next);
        if (next == p) break;
        if (field == 7) stats.saved_crashes = (long)value;
        else if (field == 8) stats.saved_hangs = (long)value;
        else if (field == 10) stats.execs_per_sec = value;
        else if (field == 11) stats.execs_done = (long)value;
        p = next;
        while (*p && *p != ',' && *p != '\n') ++p;
        if (*p == ',') ++p;
      }
    }
  }
  free(f.data);
  return stats;
}

static bool command_exists_path(const char *cmd) {
  const char *path_env = getenv("PATH");
  if (!path_env || !*path_env) return false;
  char *paths = strdup(path_env);
  if (!paths) return false;
  bool found = false;
  for (char *save = NULL, *part = strtok_r(paths, ":", &save); part; part = strtok_r(NULL, ":", &save)) {
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", *part ? part : ".", cmd);
    if (n > 0 && (size_t)n < sizeof(path) && access(path, X_OK) == 0) {
      found = true;
      break;
    }
  }
  free(paths);
  return found;
}

static void append_shell_single_quoted(str_buf_t *b, const char *text) {
  (void)sb_append_c(b, '\'');
  for (const char *p = text ? text : ""; *p; ++p) {
    if (*p == '\'') (void)sb_append(b, "'\\''");
    else (void)sb_append_c(b, *p);
  }
  (void)sb_append_c(b, '\'');
}

static char *write_afl_compiler_wrapper(const char *wrapper_dir,
                                        const char *target_name,
                                        const char *ny_bin,
                                        const char *ny_workdir,
                                        const char *nynth_root) {
  if (!wrapper_dir || !*wrapper_dir || !target_name || !*target_name ||
      !ny_bin || !*ny_bin) {
    return NULL;
  }
  if (!mkdir_p(wrapper_dir)) return NULL;
  char *path = NULL;
  (void)nynth_asprintf(&path, "%s/%s-normalize.sh", wrapper_dir, target_name);
  if (!path) return NULL;
  str_buf_t sh = {0};
  (void)sb_append(&sh, "#!/usr/bin/env bash\n");
  (void)sb_append(&sh, "set -u\n");
  (void)sb_append(&sh, "ny_bin=");
  append_shell_single_quoted(&sh, ny_bin);
  (void)sb_append(&sh, "\n");
  if (ny_workdir && *ny_workdir) {
    (void)sb_append(&sh, "ny_workdir=");
    append_shell_single_quoted(&sh, ny_workdir);
    (void)sb_append(&sh, "\n");
  }
  if (nynth_root && *nynth_root) {
    (void)sb_append(&sh, "nynth_root=");
    append_shell_single_quoted(&sh, nynth_root);
    (void)sb_append(&sh, "\n");
    (void)sb_append(&sh, "mkdir -p \"$nynth_root/build/cache/tmp\" \"$nynth_root/build/cache/scratch\" \"$nynth_root/build/cache/xdg\" \"$nynth_root/build/cache/nytrix\" >/dev/null 2>&1 || true\n");
    (void)sb_append(&sh, "export NYNTH_ROOT=\"$nynth_root\"\n");
    (void)sb_append(&sh, "export TMPDIR=\"$nynth_root/build/cache/tmp\"\n");
    (void)sb_append(&sh, "export TMP=\"$TMPDIR\"\n");
    (void)sb_append(&sh, "export TEMP=\"$TMPDIR\"\n");
    (void)sb_append(&sh, "export NYNTH_CHILD_TMPDIR=\"$TMPDIR\"\n");
    (void)sb_append(&sh, "export NYNTH_SCRATCH_ROOT=\"$nynth_root/build/cache/scratch\"\n");
    (void)sb_append(&sh, "export XDG_CACHE_HOME=\"$nynth_root/build/cache/xdg\"\n");
    (void)sb_append(&sh, "export NYTRIX_CACHE_DIR=\"$nynth_root/build/cache/nytrix\"\n");
  }
  if (ny_workdir && *ny_workdir) {
    (void)sb_append(&sh, "export NYTRIX_ROOT=\"$ny_workdir\"\n");
    (void)sb_append(&sh, "cd \"$ny_workdir\" || exit 125\n");
  }
  (void)sb_append(&sh, "if [ \"${NYNTH_AFL_RAW:-0}\" = \"1\" ]; then\n");
  (void)sb_append(&sh, "  exec \"$ny_bin\" --compiler-asserts -emit-only \"$@\"\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "\"$ny_bin\" --compiler-asserts -emit-only \"$@\" >/dev/null 2>&1\n");
  (void)sb_append(&sh, "rc=$?\n");
  (void)sb_append(&sh, "case \"$rc\" in\n");
  (void)sb_append(&sh, "  0|1|2) exit 0 ;;\n");
  (void)sb_append(&sh, "  *) exit \"$rc\" ;;\n");
  (void)sb_append(&sh, "esac\n");
  bool ok = sh.data && write_file_text(path, sh.data);
  if (ok) (void)chmod(path, 0755);
  free(sh.data);
  if (!ok) {
    free(path);
    return NULL;
  }
  return path;
}

static char *write_afl_passthrough_wrapper(const char *wrapper_dir,
                                           const char *target_name,
                                           const char *ny_bin,
                                           const char *ny_workdir,
                                           const char *nynth_root) {
  if (!wrapper_dir || !*wrapper_dir || !target_name || !*target_name ||
      !ny_bin || !*ny_bin) {
    return NULL;
  }
  if (!mkdir_p(wrapper_dir)) return NULL;
  char *path = NULL;
  (void)nynth_asprintf(&path, "%s/%s-run.sh", wrapper_dir, target_name);
  if (!path) return NULL;
  str_buf_t sh = {0};
  (void)sb_append(&sh, "#!/usr/bin/env bash\n");
  (void)sb_append(&sh, "set -u\n");
  (void)sb_append(&sh, "ny_bin=");
  append_shell_single_quoted(&sh, ny_bin);
  (void)sb_append(&sh, "\n");
  if (ny_workdir && *ny_workdir) {
    (void)sb_append(&sh, "ny_workdir=");
    append_shell_single_quoted(&sh, ny_workdir);
    (void)sb_append(&sh, "\n");
  }
  if (nynth_root && *nynth_root) {
    (void)sb_append(&sh, "nynth_root=");
    append_shell_single_quoted(&sh, nynth_root);
    (void)sb_append(&sh, "\n");
    (void)sb_append(&sh, "mkdir -p \"$nynth_root/build/cache/tmp\" \"$nynth_root/build/cache/scratch\" \"$nynth_root/build/cache/xdg\" \"$nynth_root/build/cache/nytrix\" >/dev/null 2>&1 || true\n");
    (void)sb_append(&sh, "export NYNTH_ROOT=\"$nynth_root\"\n");
    (void)sb_append(&sh, "export TMPDIR=\"$nynth_root/build/cache/tmp\"\n");
    (void)sb_append(&sh, "export TMP=\"$TMPDIR\"\n");
    (void)sb_append(&sh, "export TEMP=\"$TMPDIR\"\n");
    (void)sb_append(&sh, "export NYNTH_CHILD_TMPDIR=\"$TMPDIR\"\n");
    (void)sb_append(&sh, "export NYNTH_SCRATCH_ROOT=\"$nynth_root/build/cache/scratch\"\n");
    (void)sb_append(&sh, "export XDG_CACHE_HOME=\"$nynth_root/build/cache/xdg\"\n");
    (void)sb_append(&sh, "export NYTRIX_CACHE_DIR=\"$nynth_root/build/cache/nytrix\"\n");
  }
  if (ny_workdir && *ny_workdir) {
    (void)sb_append(&sh, "export NYTRIX_ROOT=\"$ny_workdir\"\n");
    (void)sb_append(&sh, "cd \"$ny_workdir\" || exit 125\n");
  }
  (void)sb_append(&sh, "exec \"$ny_bin\" \"$@\"\n");
  bool ok = sh.data && write_file_text(path, sh.data);
  if (ok) (void)chmod(path, 0755);
  free(sh.data);
  if (!ok) {
    free(path);
    return NULL;
  }
  return path;
}

static char *copy_afl_binary_to_cache(const char *bin_dir,
                                      const char *target_name,
                                      const char *source_bin) {
  if (!bin_dir || !*bin_dir || !target_name || !*target_name ||
      !source_bin || !*source_bin || !path_exists_file(source_bin))
    return NULL;
  if (!mkdir_p(bin_dir)) return NULL;
  char hash[32] = {0};
  file_hash_hex(source_bin, hash, sizeof(hash));
  if (!hash[0]) snprintf(hash, sizeof(hash), "unknown");
  char *path = NULL;
  (void)asprintf(&path, "%s/%s-%s-%ld-%d-ny", bin_dir, target_name, hash,
                 (long)time(NULL), (int)getpid());
  if (!path) return NULL;
  if (!copy_file_bytes(source_bin, path)) {
    free(path);
    return NULL;
  }
  (void)chmod(path, 0755);
  return path;
}

static int selected_fuzz_targets(const char *selector, const fuzz_target_t **out, int max_out) {
  int n = 0;
  if (!selector || !*selector || strcmp(selector, "all") == 0) {
    for (size_t i = 0; i < sizeof(FUZZ_TARGETS) / sizeof(FUZZ_TARGETS[0]) && n < max_out; ++i)
      out[n++] = &FUZZ_TARGETS[i];
    return n;
  }
  if (strcmp(selector, "parsers") == 0) {
    for (size_t i = 0; i < sizeof(FUZZ_TARGETS) / sizeof(FUZZ_TARGETS[0]) && n < max_out; ++i)
      if (FUZZ_TARGETS[i].parser_target) out[n++] = &FUZZ_TARGETS[i];
    return n;
  }
  if (strcmp(selector, "compiler") == 0 || strcmp(selector, "compiler-ny") == 0 ||
      strcmp(selector, "ny-compiler") == 0) {
    for (size_t i = 0; i < sizeof(FUZZ_TARGETS) / sizeof(FUZZ_TARGETS[0]) && n < max_out; ++i) {
      if (strcmp(FUZZ_TARGETS[i].name, "syntax") == 0 ||
          strcmp(FUZZ_TARGETS[i].name, "ny-core") == 0)
        out[n++] = &FUZZ_TARGETS[i];
    }
    return n;
  }
  for (size_t i = 0; i < sizeof(FUZZ_TARGETS) / sizeof(FUZZ_TARGETS[0]); ++i) {
    if (strcmp(selector, FUZZ_TARGETS[i].name) == 0) {
      out[n++] = &FUZZ_TARGETS[i];
      return n;
    }
  }
  return -1;
}

static int parser_fuzz_target_count(void) {
  int n = 0;
  for (size_t i = 0; i < sizeof(FUZZ_TARGETS) / sizeof(FUZZ_TARGETS[0]); ++i)
    if (FUZZ_TARGETS[i].parser_target) ++n;
  return n;
}

static void append_fuzz_targets_json(str_buf_t *b) {
  (void)sb_append_c(b, '[');
  for (size_t i = 0; i < sizeof(FUZZ_TARGETS) / sizeof(FUZZ_TARGETS[0]); ++i) {
    if (i) (void)sb_append_c(b, ',');
    (void)sb_append_json_str(b, FUZZ_TARGETS[i].name);
  }
  (void)sb_append_c(b, ']');
}

static char *build_fuzz_report_json(const string_list_t *rows, const string_list_t *failures,
                                    const char *workspace, str_buf_t *summary_extra,
                                    const char *canonical_command) {
  str_buf_t b = {0};
  int ok = rows->count - failures->count;
  if (ok < 0) ok = 0;
  (void)sb_append(&b, "{\"rows\":");
  append_raw_json_list(&b, rows);
  (void)sb_append(&b, ",\"failures\":");
  append_raw_json_list(&b, failures);
  (void)sb_appendf(&b,
                   ",\"ok\":%s,\"cases\":%d,\"ok_count\":%d,"
                   "\"failure_count\":%d",
                   failures->count == 0 ? "true" : "false",
                   rows->count, ok, failures->count);
  (void)sb_appendf(&b,
                   ",\"summary\":{\"cases\":%d,\"ok\":%d,"
                   "\"ok_count\":%d,\"failure_count\":%d,"
                   "\"workspace\":",
                   rows->count, ok, ok, failures->count);
  (void)sb_append_json_str(&b, workspace);
  if (summary_extra && summary_extra->data) (void)sb_append(&b, summary_extra->data);
  (void)sb_append(&b, "}");
  (void)sb_append(&b, ",\"meta\":{\"targets\":");
  append_fuzz_targets_json(&b);
  (void)sb_append(&b, ",\"canonical_command\":");
  (void)sb_append_json_str(&b, canonical_command);
  (void)sb_append(&b, ",\"engine\":\"nynth_core\"}}");
  return sb_take(&b);
}

static int emit_rows_failures_report(char *report_json, const char *json_path, const char *workspace,
                                     int rows, int failures) {
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
    free(report_json);
    return 2;
  }
  printf("rows: %d\n", rows);
  printf("failures: %d\n", failures);
  if (workspace && *workspace) printf("workspace: %s\n", workspace);
  free(report_json);
  return failures ? 1 : 0;
}

static bool copy_fuzz_seed_file(const char *src, const char *dst) {
  file_buf_t f = {0};
  if (!read_file(src, &f)) return false;
  bool ok = write_file_bytes(dst, (const unsigned char *)f.data, f.len);
  free(f.data);
  return ok;
}

static int materialize_kernel_fuzz_seeds(const char *root, string_list_t *rows,
                                         string_list_t *failures) {
  char *shape_dir = NULL;
  (void)nynth_asprintf(&shape_dir, "shapes/kernels");
  string_list_t shapes = {0};
  if (!shape_dir || !collect_regular_files_recursive(shape_dir, &shapes)) {
    (void)string_list_push_take(failures, make_fuzz_failure(root, "ny", "kernel shape scan failed",
                                                            shape_dir ? shape_dir : ""));
    free(shape_dir);
    return 0;
  }
  qsort(shapes.items, (size_t)shapes.count, sizeof(char *), cmp_cstr);
  int count = 0;
  for (int i = 0; i < shapes.count; ++i) {
    if (!ny_has_suffix(shapes.items[i], ".nshape")) continue;
    char *source = nynth_shape_source_block(shapes.items[i], "ny");
    char stem[160];
    stem_name(shapes.items[i], stem, sizeof(stem));
    char *dst = NULL;
    (void)nynth_asprintf(&dst, "rt/%s.ny", stem);
    if (!source || !dst || !write_file_text(dst, source)) {
      (void)string_list_push_take(failures, make_fuzz_failure(root, "ny",
                                                              "kernel source materialize failed",
                                                              dst ? dst : shapes.items[i]));
    } else {
      char origin[512];
      snprintf(origin, sizeof(origin), "%s:source ny", shapes.items[i]);
      (void)string_list_push_take(rows, make_fuzz_file_row(root, "seed", dst, origin));
      ++count;
    }
    free(source);
    free(dst);
  }
  string_list_free(&shapes);
  free(shape_dir);
  return count;
}

static int cmd_public_fuzz_corpus_prepare(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  string_list_t rows = {0}, failures = {0};
  const char *dirs[] = {"json", "xml", "csv", "png", "gltf", "str", "ny", "ny-core", "syntax"};
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
    char *dir = NULL;
    (void)nynth_asprintf(&dir, "fuzz/corpus/%s", dirs[i]);
    if (!dir || !mkdir_p(dir)) {
      char *fail = make_fuzz_failure(root, dirs[i], "failed to create corpus directory", dir ? dir : "");
      (void)string_list_push_take(&failures, fail);
    }
    free(dir);
  }

  for (size_t i = 0; i < sizeof(FUZZ_TEXT_SEEDS) / sizeof(FUZZ_TEXT_SEEDS[0]); ++i) {
    char *path = NULL;
    (void)nynth_asprintf(&path, "fuzz/corpus/%s", FUZZ_TEXT_SEEDS[i].rel);
    if (!path || !write_file_text(path, FUZZ_TEXT_SEEDS[i].text)) {
      char *fail = make_fuzz_failure(root, "corpus", "failed to write builtin seed", path ? path : "");
      (void)string_list_push_take(&failures, fail);
    } else {
      (void)string_list_push_take(&rows, make_fuzz_file_row(root, "seed", path, "nynth_builtin"));
    }
    free(path);
  }

  char *png_path = NULL;
  (void)nynth_asprintf(&png_path, "fuzz/corpus/png/min.png");
  if (!png_path || !write_file_bytes(png_path, FUZZ_PNG_MIN, sizeof(FUZZ_PNG_MIN))) {
    char *fail = make_fuzz_failure(root, "png", "failed to write builtin png seed", png_path ? png_path : "");
    (void)string_list_push_take(&failures, fail);
  } else {
    (void)string_list_push_take(&rows, make_fuzz_file_row(root, "seed", png_path, "nynth_builtin"));
  }
  free(png_path);

  int runtime_seeds = 0;
  const char *runtime_dirs[] = {"etc/tests/rt", "etc/tests/runtime"};
  string_list_t runtime_files = {0};
  for (size_t d = 0; d < sizeof(runtime_dirs) / sizeof(runtime_dirs[0]); ++d) {
    char *dir = NULL;
    (void)asprintf(&dir, "%s/%s", root, runtime_dirs[d]);
    if (dir) (void)collect_regular_files_recursive(dir, &runtime_files);
    free(dir);
  }
  qsort(runtime_files.items, (size_t)runtime_files.count, sizeof(char *), cmp_cstr);
  for (int i = 0; i < runtime_files.count; ++i) {
    if (!ny_has_suffix(runtime_files.items[i], ".ny")) continue;
    char *dst = NULL;
    (void)nynth_asprintf(&dst, "rt/%s", ny_base_name(runtime_files.items[i]));
    if (!dst || !copy_fuzz_seed_file(runtime_files.items[i], dst)) {
      char *fail = make_fuzz_failure(root, "ny", "failed to copy runtime seed", dst ? dst : "");
      (void)string_list_push_take(&failures, fail);
    } else {
      str_buf_t src = {0};
      size_t root_len = strlen(root);
      if (strncmp(runtime_files.items[i], root, root_len) == 0 && runtime_files.items[i][root_len] == '/')
        (void)sb_append(&src, runtime_files.items[i] + root_len + 1);
      else
        (void)sb_append(&src, runtime_files.items[i]);
      (void)string_list_push_take(&rows, make_fuzz_file_row(root, "seed", dst, src.data ? src.data : ""));
      free(src.data);
      ++runtime_seeds;
    }
    free(dst);
  }
  string_list_free(&runtime_files);

  int kernel_seeds = materialize_kernel_fuzz_seeds(root, &rows, &failures);

  char *workspace = make_fuzz_path(root, "");
  char *corpus = make_fuzz_path(root, "corpus");
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"corpus\":");
  append_rel_json_str(&extra, root, corpus ? corpus : "");
  (void)sb_appendf(&extra, ",\"files\":%d,\"ny_runtime_seeds\":%d,\"ny_kernel_seeds\":%d",
                   rows.count, runtime_seeds, kernel_seeds);
  char *workspace_rel = rel_path_dup(root, workspace ? workspace : "");
  char *report_json = build_fuzz_report_json(&rows, &failures,
                                             workspace_rel ? workspace_rel : "",
                                             &extra, "nynth fuzz corpus prepare");
  int rc = emit_rows_failures_report(report_json, json_path, workspace_rel, rows.count, failures.count);
  free(workspace_rel); free(extra.data); free(workspace); free(corpus);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static int cmd_public_fuzz_workspace_audit(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  string_list_t rows = {0}, failures = {0}, files = {0};
  char *corpus_dir = make_fuzz_path(root, "corpus");
  char *dict_dir = make_fuzz_path(root, "dict");
  char *target_dir = make_fuzz_path(root, "targets");
  if (corpus_dir) (void)collect_regular_files_recursive(corpus_dir, &files);
  qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
  for (int i = 0; i < files.count; ++i)
    (void)string_list_push_take(&rows, make_fuzz_file_row(root, "corpus", files.items[i], ""));
  int corpus_files = files.count;
  string_list_free(&files);
  if (dict_dir) (void)collect_regular_files_recursive(dict_dir, &files);
  qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
  for (int i = 0; i < files.count; ++i)
    if (ny_has_suffix(files.items[i], ".dict"))
      (void)string_list_push_take(&rows, make_fuzz_file_row(root, "dict", files.items[i], ""));
  int dict_files = files.count;
  string_list_free(&files);
  if (target_dir) (void)collect_regular_files_recursive(target_dir, &files);
  qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
  for (int i = 0; i < files.count; ++i)
    if (ny_has_suffix(files.items[i], ".ny"))
      (void)string_list_push_take(&rows, make_fuzz_file_row(root, "target", files.items[i], ""));
  int target_files = files.count;
  string_list_free(&files);

  for (size_t i = 0; i < sizeof(FUZZ_TARGETS) / sizeof(FUZZ_TARGETS[0]); ++i) {
    const fuzz_target_t *t = &FUZZ_TARGETS[i];
    char *corpus = NULL, *dict = NULL, *script = NULL;
    (void)nynth_asprintf(&corpus, "fuzz/corpus/%s", t->name);
    if (!corpus || !dir_has_any_entry(corpus))
      (void)string_list_push_take(&failures, make_fuzz_failure(root, t->name, "missing or empty corpus", corpus ? corpus : ""));
    if (t->dict) {
      (void)nynth_asprintf(&dict, "fuzz/dict/%s", t->dict);
      if (!dict || !path_exists_file(dict))
        (void)string_list_push_take(&failures, make_fuzz_failure(root, t->name, "missing dictionary", dict ? dict : ""));
    }
    if (t->script) {
      (void)nynth_asprintf(&script, "fuzz/targets/%s", t->script);
      if (!script || !path_exists_file(script))
        (void)string_list_push_take(&failures, make_fuzz_failure(root, t->name, "missing target script", script ? script : ""));
    }
    free(corpus); free(dict); free(script);
  }

  char *workspace = make_fuzz_path(root, "");
  char *workspace_rel = rel_path_dup(root, workspace ? workspace : "");
  str_buf_t extra = {0};
  (void)sb_appendf(&extra, ",\"corpus_files\":%d,\"dict_files\":%d,\"target_files\":%d,\"targets\":%zu",
                   corpus_files, dict_files, target_files, sizeof(FUZZ_TARGETS) / sizeof(FUZZ_TARGETS[0]));
  char *report_json = build_fuzz_report_json(&rows, &failures,
                                             workspace_rel ? workspace_rel : "",
                                             &extra, "nynth fuzz workspace audit");
  int rc = emit_rows_failures_report(report_json, json_path, workspace_rel, rows.count, failures.count);
  free(workspace_rel); free(extra.data); free(workspace);
  free(corpus_dir); free(dict_dir); free(target_dir);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static int cmd_public_fuzz_harness_smoke(int argc, char **argv) {
  char root[4096], ny_root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  if (!find_repo_root_or_sibling(ny_root, sizeof(ny_root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  const char *selector = value_after_equals(argc, argv, 4, "--target", "parsers");
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  int limit = atoi(value_after_equals(argc, argv, 4, "--limit", "0"));
  double timeout_s = atof(value_after_equals(argc, argv, 4, "--timeout-s", "5"));
  if (timeout_s <= 0.0) timeout_s = 5.0;
  const fuzz_target_t *targets[16];
  int target_count = selected_fuzz_targets(selector, targets, 16);
  string_list_t rows = {0}, failures = {0};
  if (target_count < 0) {
    (void)string_list_push_take(&failures, make_fuzz_failure(root, selector, "unknown fuzz target", ""));
    target_count = 0;
  }
  char *ny_bin = NULL;
  (void)asprintf(&ny_bin, "%s/build/release/ny", ny_root);
  int cases = 0;
  for (int i = 0; i < target_count; ++i) {
    const fuzz_target_t *t = targets[i];
    if (!t->parser_target && !t->direct_mode) continue;
    if (!t->direct_mode && !t->script) continue;
    char *corpus = NULL, *script = NULL;
    (void)nynth_asprintf(&corpus, "fuzz/corpus/%s", t->name);
    if (!t->direct_mode) (void)nynth_asprintf(&script, "fuzz/targets/%s", t->script);
    string_list_t files = {0};
    if (!corpus || !collect_regular_files_recursive(corpus, &files)) {
      (void)string_list_push_take(&failures,
                                  make_fuzz_failure(root, t->name, "corpus scan failed",
                                                    corpus ? corpus : ""));
      free(corpus);
      free(script);
      continue;
    }
    qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
    int run_count = 0;
    for (int f = 0; f < files.count; ++f) {
      if (limit > 0 && run_count >= limit) break;
      ++run_count;
      ++cases;
      char *parser_argv[] = {ny_bin, script, files.items[f], NULL};
      char *direct_argv[] = {ny_bin, "--compiler-asserts", "-emit-only", files.items[f], NULL};
      proc_result_t pr = run_proc(t->direct_mode ? direct_argv : parser_argv, root, timeout_s);
      bool ok = t->direct_mode
                    ? (!pr.timed_out && pr.rc != 124 && pr.rc != 127 && pr.rc < 128 &&
                       !text_mentions_crash(pr.out, pr.err))
                    : (pr.rc == 0 && !pr.timed_out);
      str_buf_t row = {0};
      (void)sb_append(&row, "{\"target\":");
      (void)sb_append_json_str(&row, t->name);
      (void)sb_append(&row, ",\"seed\":");
      append_rel_json_str(&row, root, files.items[f]);
      (void)sb_append(&row, ",\"mode\":");
      (void)sb_append_json_str(&row, t->direct_mode ? "direct-compiler" : "script-parser");
      if (!t->direct_mode) {
        (void)sb_append(&row, ",\"script\":");
        append_rel_json_str(&row, root, script ? script : "");
      }
      (void)sb_appendf(&row,
                       ",\"ok\":%s,\"rc\":%d,\"elapsed_ms\":%.2f,\"timed_out\":%s",
                       ok ? "true" : "false", pr.rc, pr.elapsed_ms,
                       pr.timed_out ? "true" : "false");
      if (!ok) append_proc_tail_fields(&row, &pr);
      (void)sb_append_c(&row, '}');
      (void)string_list_push_take(&rows, sb_take(&row));
      if (!ok) {
        char case_name[512];
        snprintf(case_name, sizeof(case_name), "%s:%s", t->name, ny_base_name(files.items[f]));
        (void)string_list_push_take(&failures,
                                    make_worker_failure_row(case_name, "fuzz-harness-smoke",
                                                            pr.rc, pr.out, pr.err));
      }
      proc_result_free(&pr);
    }
    string_list_free(&files);
    free(corpus);
    free(script);
  }
  char *workspace = make_fuzz_path(root, "");
  char *workspace_rel = rel_path_dup(root, workspace ? workspace : "");
  str_buf_t extra = {0};
  (void)sb_appendf(&extra, ",\"targets\":%d,\"cases\":%d,\"limit_per_target\":%d,\"timeout_s\":%.2f",
                   target_count, cases, limit, timeout_s);
  char *report_json = build_fuzz_report_json(&rows, &failures,
                                             workspace_rel ? workspace_rel : "",
                                             &extra, "nynth fuzz harness smoke");
  int rc = emit_rows_failures_report(report_json, json_path, workspace_rel,
                                     rows.count, failures.count);
  free(workspace_rel);
  free(workspace);
  free(extra.data);
  free(ny_bin);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static char *module_name_from_lib_path(const char *ny_root, const char *path) {
  if (!ny_root || !*ny_root || !path || !*path) return NULL;
  char prefix[4096];
  int pn = snprintf(prefix, sizeof(prefix), "%s/lib/", ny_root);
  if (pn <= 0 || (size_t)pn >= sizeof(prefix)) return NULL;
  if (strncmp(path, prefix, (size_t)pn) != 0) return NULL;
  const char *rel = path + pn;
  size_t len = strlen(rel);
  if (len <= 3 || strcmp(rel + len - 3, ".ny") != 0) return NULL;
  len -= 3;
  if (len >= 4 && strncmp(rel + len - 4, "/mod", 4) == 0) len -= 4;
  str_buf_t b = {0};
  (void)sb_append(&b, "std");
  if (len > 0) {
    (void)sb_append_c(&b, '.');
    for (size_t i = 0; i < len; ++i) {
      char c = rel[i];
      (void)sb_append_c(&b, c == '/' ? '.' : c);
    }
  }
  return sb_take(&b);
}

static bool lib_smoke_hot_path(const char *path) {
  if (!path) return false;
  return strstr(path, "/core/mod.ny") ||
         strstr(path, "/math/mod.ny") ||
         strstr(path, "/math/crypto/mod.ny") ||
         strstr(path, "/os/mod.ny") ||
         strstr(path, "/parse/mod.ny") ||
         strstr(path, "/parse/img/mod.ny") ||
         strstr(path, "/core/str.ny") ||
         strstr(path, "/core/dict.ny") ||
         strstr(path, "/core/iter.ny") ||
         strstr(path, "/os/path.ny") ||
         strstr(path, "/parse/syntax.ny") ||
         strstr(path, "/math/simmd.ny");
}

static bool lib_smoke_platform_path(const char *path) {
  if (!path) return false;
  return strstr(path, "/os/ui/") ||
         strstr(path, "/os/ui.ny") ||
         strstr(path, "/os/sound/") ||
         strstr(path, "/os/sound.ny") ||
         strstr(path, "/os/gpu.ny") ||
         strstr(path, "/os/clipboard.ny") ||
         strstr(path, "/os/interact.ny");
}

static bool lib_smoke_should_select(const char *path, int index, int total, int limit) {
  if (limit <= 0 || total <= limit) return true;
  if (lib_smoke_hot_path(path)) return true;
  long long before = ((long long)index * (long long)limit) / (long long)total;
  long long after = ((long long)(index + 1) * (long long)limit) / (long long)total;
  return after > before;
}

static const char *lib_module_package(const char *module) {
  if (!module) return "other";
  if (strncmp(module, "std.core", 8) == 0) return "core";
  if (strncmp(module, "std.math.parse", 14) == 0) return "parse";
  if (strncmp(module, "std.math", 8) == 0) return "math";
  if (strncmp(module, "std.os", 6) == 0) return "os";
  return "other";
}

static char *make_lib_smoke_row(const char *root, const char *source, const char *module,
                                const char *mode, const char *wrapper, bool ok,
                                const proc_result_t *pr) {
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"target\":\"libs\",\"kind\":\"fuzz-lib-smoke\",\"source\":");
  append_rel_json_str(&row, root, source ? source : "");
  (void)sb_append(&row, ",\"module\":");
  (void)sb_append_json_str(&row, module ? module : "");
  (void)sb_append(&row, ",\"mode\":");
  (void)sb_append_json_str(&row, mode ? mode : "import");
  if (wrapper && *wrapper) {
    (void)sb_append(&row, ",\"wrapper\":");
    append_rel_json_str(&row, root, wrapper);
  }
  (void)sb_appendf(&row, ",\"ok\":%s,\"rc\":%d,\"elapsed_ms\":%.2f,\"timed_out\":%s",
                   ok ? "true" : "false", pr ? pr->rc : 1,
                   pr ? pr->elapsed_ms : 0.0,
                   (pr && pr->timed_out) ? "true" : "false");
  if (!ok && pr) append_proc_tail_fields(&row, pr);
  (void)sb_append_c(&row, '}');
  return sb_take(&row);
}

static bool run_lib_smoke_check(const char *root, const char *ny_bin, const char *work_dir,
                                const char *source, const char *module, const char *mode,
                                int index, double timeout_s, string_list_t *rows,
                                string_list_t *failures, int *checks) {
  char *check_path = NULL;
  str_buf_t wrapper = {0};
  bool import_mode = strcmp(mode, "import") == 0;
  if (import_mode) {
    char stem[128];
    safe_stem(stem, sizeof(stem), source ? source : "lib");
    if (asprintf(&check_path, "%s/lib_%04d_%s.ny", work_dir ? work_dir : "/tmp", index, stem) < 0)
      check_path = NULL;
    if (module && strcmp(module, "std.core") != 0)
      (void)sb_appendf(&wrapper, "use std.core\nuse %s\n0\n", module);
    else
      (void)sb_append(&wrapper, "use std.core\n0\n");
    if (!check_path || !write_file_text(check_path, wrapper.data ? wrapper.data : "0\n")) {
      proc_result_t fake = {0};
      fake.rc = 1;
      (void)string_list_push_take(rows, make_lib_smoke_row(root, source, module, mode,
                                                           check_path, false, &fake));
      (void)string_list_push_take(failures, make_worker_failure_row(module, "fuzz-lib-smoke",
                                                                    1, "", "wrapper write failed"));
      free(wrapper.data);
      free(check_path);
      return false;
    }
  } else {
    check_path = strdup(source ? source : "");
  }
  char *check_argv[] = {(char *)ny_bin, "--compiler-asserts", "-emit-only", check_path, NULL};
  proc_result_t pr = run_proc(check_argv, root, timeout_s);
  bool ok = pr.rc == 0 && !pr.timed_out;
  (void)string_list_push_take(rows, make_lib_smoke_row(root, source, module, mode,
                                                       import_mode ? check_path : "",
                                                       ok, &pr));
  if (!ok) {
    char case_name[512];
    snprintf(case_name, sizeof(case_name), "%s:%s", mode ? mode : "import",
             module && *module ? module : ny_base_name(source ? source : "lib"));
    (void)string_list_push_take(failures, make_worker_failure_row(case_name, "fuzz-lib-smoke",
                                                                  pr.rc, pr.out, pr.err));
  }
  if (checks) ++*checks;
  proc_result_free(&pr);
  free(wrapper.data);
  free(check_path);
  return ok;
}

static const char *nynth_scratch_root_arg(int argc, char **argv, int start_index) {
  const char *arg = value_after_equals(argc, argv, start_index, "--scratch-root", "");
  if (arg && *arg) return arg;
  const char *env = getenv("NYNTH_SCRATCH_ROOT");
  if (env && *env) return env;
  return NYNTH_DEFAULT_SCRATCH_ROOT;
}

static bool selftest_spec_uses_scratch_root(const char *name) {
  if (!name) return false;
  return strcmp(name, "fuzz_libs_smoke") == 0 ||
         strcmp(name, "fuzz_all_full_pressure_remediation") == 0 ||
         strcmp(name, "fuzz_repro_ready_missing_wrapper") == 0 ||
         strcmp(name, "fuzz_repro_ready_missing_command") == 0 ||
         strcmp(name, "synth_schedule") == 0 ||
         strcmp(name, "synth_pure") == 0;
}

static bool path_has_directory_prefix(const char *path, const char *dir) {
  if (!path || !*path || !dir || !*dir) return false;
  size_t dir_len = strlen(dir);
  while (dir_len > 1 && dir[dir_len - 1] == '/') --dir_len;
  if (strncmp(path, dir, dir_len) != 0) return false;
  return path[dir_len] == '\0' || path[dir_len] == '/';
}

static char *path_normalize_absolute_lexical(const char *path) {
  if (!path || !*path || !path_is_absolute(path)) return path ? strdup(path) : NULL;
  char *copy = strdup(path);
  if (!copy) return NULL;
  char *parts[512];
  int count = 0;
  char *save = NULL;
  for (char *tok = strtok_r(copy, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
    if (!*tok || strcmp(tok, ".") == 0) continue;
    if (strcmp(tok, "..") == 0) {
      if (count > 0) --count;
      continue;
    }
    if (count >= (int)(sizeof(parts) / sizeof(parts[0]))) {
      free(copy);
      return strdup(path);
    }
    parts[count++] = tok;
  }
  size_t total = 1;
  for (int i = 0; i < count; ++i) total += strlen(parts[i]) + (i > 0 ? 1u : 0u);
  char *out = (char *)malloc(total + 1u);
  if (!out) {
    free(copy);
    return NULL;
  }
  size_t pos = 0;
  out[pos++] = '/';
  for (int i = 0; i < count; ++i) {
    if (i > 0) out[pos++] = '/';
    size_t len = strlen(parts[i]);
    memcpy(out + pos, parts[i], len);
    pos += len;
  }
  out[pos] = '\0';
  free(copy);
  return out;
}

static bool path_under_directory(const char *path, const char *dir) {
  if (!path || !*path || !dir || !*dir) return false;
  char *norm_path = path_normalize_absolute_lexical(path);
  char *norm_dir = path_normalize_absolute_lexical(dir);
  bool under = norm_path && norm_dir && path_has_directory_prefix(norm_path, norm_dir);
  free(norm_path);
  free(norm_dir);
  if (under) return true;

  char real_path[4096], real_dir[4096];
  if (realpath(path, real_path) && realpath(dir, real_dir))
    return path_has_directory_prefix(real_path, real_dir);
  return false;
}

static void append_repo_or_sibling_rel_json_str(str_buf_t *b,
                                                const char *root,
                                                const char *path) {
  if (!b) return;
  if (!root || !*root || !path || !*path) {
    (void)sb_append_json_str(b, path ? path : "");
    return;
  }
  char *norm_root = path_normalize_absolute_lexical(root);
  char *norm_path = path_normalize_absolute_lexical(path);
  char sibling[4096] = {0};
  char *norm_sibling = NULL;
  if (path_join(sibling, sizeof(sibling), root, "../nytrix"))
    norm_sibling = path_normalize_absolute_lexical(sibling);

  if (norm_root && norm_path && strcmp(norm_root, norm_path) == 0) {
    (void)sb_append_json_str(b, ".");
  } else if (norm_root && norm_path &&
             path_has_directory_prefix(norm_path, norm_root)) {
    size_t n = strlen(norm_root);
    const char *rel = norm_path + n;
    if (*rel == '/') ++rel;
    (void)sb_append_json_str(b, rel);
  } else if (norm_sibling && norm_path &&
             strcmp(norm_sibling, norm_path) == 0) {
    (void)sb_append_json_str(b, "../nytrix");
  } else if (norm_sibling && norm_path &&
             path_has_directory_prefix(norm_path, norm_sibling)) {
    str_buf_t rel = {0};
    (void)sb_append(&rel, "../nytrix");
    size_t n = strlen(norm_sibling);
    const char *suffix = norm_path + n;
    if (*suffix != '/') (void)sb_append(&rel, "/");
    (void)sb_append(&rel, suffix);
    (void)sb_append_json_str(b, rel.data ? rel.data : "../nytrix");
    free(rel.data);
  } else {
    append_rel_json_str(b, root, path);
  }
  free(norm_sibling);
  free(norm_path);
  free(norm_root);
}

static char *repo_or_sibling_rel_path_dup(const char *root,
                                          const char *path) {
  str_buf_t quoted = {0};
  append_repo_or_sibling_rel_json_str(&quoted, root ? root : "",
                                      path ? path : "");
  char *out = NULL;
  if (quoted.data) {
    const char *p = quoted.data;
    out = parse_json_string_dup(&p, quoted.data + strlen(quoted.data));
  }
  free(quoted.data);
  return out ? out : strdup(path ? path : "");
}

static char *nynth_default_scratch_root_absolute(const char *nynth_root) {
  if (nynth_root && *nynth_root) {
    char *out = NULL;
    if (asprintf(&out, "%s/%s", nynth_root, NYNTH_DEFAULT_SCRATCH_ROOT) >= 0 && out)
      return out;
  }
  return strdup(NYNTH_DEFAULT_SCRATCH_ROOT);
}

static bool nynth_scratch_root_points_inside_nytrix(const char *scratch_root,
                                                    const char *nynth_root) {
  if (!scratch_root || !*scratch_root || !path_is_absolute(scratch_root)) return false;
  char nytrix_root[4096];
  if (find_repo_root_or_sibling(nytrix_root, sizeof(nytrix_root)) &&
      path_under_directory(scratch_root, nytrix_root))
    return true;

  char sibling[4096];
  if (nynth_root && *nynth_root &&
      path_join(sibling, sizeof(sibling), nynth_root, "../nytrix") &&
      path_under_directory(scratch_root, sibling))
    return true;
  return false;
}

static char *nynth_absolute_scratch_root(const char *scratch_root) {
  char nynth_root[4096] = "";
  bool have_nynth_root = find_nynth_root(nynth_root, sizeof(nynth_root));
  const char *requested = scratch_root && *scratch_root ? scratch_root : "";
  if (!*requested) {
    const char *env = getenv("NYNTH_SCRATCH_ROOT");
    if (env && *env) requested = env;
  }
  if (!*requested) requested = NYNTH_DEFAULT_SCRATCH_ROOT;
  char *out = NULL;
  if (path_is_absolute(requested)) {
    out = strdup(requested);
  } else if (have_nynth_root) {
    (void)asprintf(&out, "%s/%s", nynth_root, requested);
  } else {
    out = strdup(requested);
  }

  if (out && nynth_scratch_root_points_inside_nytrix(out, have_nynth_root ? nynth_root : "")) {
    free(out);
    out = nynth_default_scratch_root_absolute(have_nynth_root ? nynth_root : "");
  }
  return out;
}

static void nynth_redirect_nytrix_output_dir(char **dir_path,
                                             const char *nynth_root,
                                             const char *leaf) {
  if (!dir_path || !*dir_path || !**dir_path) return;
  char root_buf[4096] = "";
  const char *root = nynth_root && *nynth_root ? nynth_root : "";
  if (!*root && find_nynth_root(root_buf, sizeof(root_buf))) root = root_buf;
  if (!nynth_scratch_root_points_inside_nytrix(*dir_path, root)) return;
  char *redirected = NULL;
  (void)asprintf(&redirected, "%s/%s/redirected-nytrix-tmp-projects-test/%s",
                 root && *root ? root : ".",
                 NYNTH_DEFAULT_SCRATCH_ROOT,
                 leaf && *leaf ? leaf : "artifacts");
  if (redirected) {
    free(*dir_path);
    *dir_path = redirected;
  }
}

static char *nynth_scratch_pathf(const char *scratch_root_arg,
                                 const char *rel_fmt, ...) {
  char *scratch_root = nynth_absolute_scratch_root(scratch_root_arg);
  if (!scratch_root) return NULL;
  char *rel = NULL;
  va_list ap;
  va_start(ap, rel_fmt);
  int rc = vasprintf(&rel, rel_fmt, ap);
  va_end(ap);
  if (rc < 0 || !rel) {
    free(scratch_root);
    return NULL;
  }
  char *out = NULL;
  if (rel[0] == '/') out = strdup(rel);
  else (void)asprintf(&out, "%s/%s", scratch_root, rel);
  free(rel);
  free(scratch_root);
  return out;
}

static char *nynth_cache_replay_dir(void) {
  return nynth_scratch_pathf(NULL, "replay");
}

static char *nynth_cache_reduced_path(const char *stem) {
  return nynth_scratch_pathf(NULL, "reduced/%s.reduced.ny",
                             stem && *stem ? stem : "reduced");
}

static int cmd_public_fuzz_libs_smoke(int argc, char **argv) {
  char root[4096], ny_root[4096], ny_bin[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  if (!find_repo_root_or_sibling(ny_root, sizeof(ny_root)) ||
      !find_ny_bin(ny_root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"nytrix-root-or-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *mode_arg = value_after_equals(argc, argv, 4, "--mode", "import");
  if (has_flag_after(argc, argv, 4, "--direct")) mode_arg = "direct";
  if (has_flag_after(argc, argv, 4, "--both")) mode_arg = "both";
  const char *mode = (strcmp(mode_arg, "direct") == 0 ||
                      strcmp(mode_arg, "both") == 0 ||
                      strcmp(mode_arg, "import") == 0) ? mode_arg : "import";
  bool include_platform = has_flag_after(argc, argv, 4, "--include-platform");
  int limit = atoi(value_after_equals(argc, argv, 4, "--limit", "64"));
  if (has_flag_after(argc, argv, 4, "--full")) limit = 0;
  if (limit < 0) limit = 0;
  double timeout_s = atof(value_after_equals(argc, argv, 4, "--timeout-s", "4"));
  if (timeout_s <= 0.0) timeout_s = 4.0;
  const char *scratch_root_arg = nynth_scratch_root_arg(argc, argv, 4);
  char *scratch_root = nynth_absolute_scratch_root(scratch_root_arg);

  char *lib_dir = NULL, *work_dir = NULL;
  (void)asprintf(&lib_dir, "%s/lib", ny_root);
  if (scratch_root && *scratch_root) ny_ensure_dir_recursive(scratch_root);
  (void)asprintf(&work_dir, "%s/nynth_libs_smoke_%ld",
                 scratch_root && *scratch_root ? scratch_root : NYNTH_DEFAULT_SCRATCH_ROOT,
                 (long)getpid());
  string_list_t files = {0}, rows = {0}, failures = {0};
  if (!lib_dir || !collect_regular_files_recursive(lib_dir, &files)) {
    (void)string_list_push_take(&failures, make_fuzz_failure(root, "libs", "library scan failed",
                                                            lib_dir ? lib_dir : ""));
  }
  if (work_dir && !mkdir_p(work_dir)) {
    (void)string_list_push_take(&failures, make_fuzz_failure(root, "libs", "scratch workdir failed",
                                                            work_dir));
  }
  qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
  int lib_files = 0, portable_files = 0, skipped_platform = 0;
  for (int i = 0; i < files.count; ++i) {
    if (!ny_has_suffix(files.items[i], ".ny")) continue;
    ++lib_files;
    if (!include_platform && lib_smoke_platform_path(files.items[i])) {
      ++skipped_platform;
      continue;
    }
    ++portable_files;
  }
  int selected = 0, checks = 0, portable_index = 0;
  int selected_core = 0, selected_math = 0, selected_os = 0, selected_parse = 0, selected_other = 0;
  for (int i = 0; i < files.count; ++i) {
    if (!ny_has_suffix(files.items[i], ".ny")) continue;
    if (!include_platform && lib_smoke_platform_path(files.items[i])) continue;
    int current_index = portable_index++;
    if (!lib_smoke_should_select(files.items[i], current_index, portable_files, limit)) continue;
    ++selected;
    char *module = module_name_from_lib_path(ny_root, files.items[i]);
    if (!module) {
      (void)string_list_push_take(&failures, make_fuzz_failure(root, "libs",
                                                              "module name derivation failed",
                                                              files.items[i]));
      continue;
    }
    const char *pkg = lib_module_package(module);
    if (strcmp(pkg, "core") == 0) ++selected_core;
    else if (strcmp(pkg, "math") == 0) ++selected_math;
    else if (strcmp(pkg, "os") == 0) ++selected_os;
    else if (strcmp(pkg, "parse") == 0) ++selected_parse;
    else ++selected_other;
    if (strcmp(mode, "direct") != 0)
      (void)run_lib_smoke_check(root, ny_bin, work_dir, files.items[i], module,
                                "import", i, timeout_s, &rows, &failures, &checks);
    if (strcmp(mode, "import") != 0)
      (void)run_lib_smoke_check(root, ny_bin, work_dir, files.items[i], module,
                                "direct", i, timeout_s, &rows, &failures, &checks);
    free(module);
  }
  if (lib_files == 0) {
    (void)string_list_push_take(&failures, make_fuzz_failure(root, "libs", "no library files found",
                                                            lib_dir ? lib_dir : ""));
  }
  char *workspace = make_fuzz_path(root, "");
  char *workspace_rel = rel_path_dup(root, workspace ? workspace : "");
  str_buf_t extra = {0};
  (void)sb_appendf(&extra,
                   ",\"lib_files\":%d,\"portable_files\":%d,\"selected_files\":%d,"
                   "\"checks\":%d,\"skipped_platform\":%d,\"limit\":%d,\"timeout_s\":%.2f,"
                   "\"include_platform\":%s,\"mode\":",
                   lib_files, portable_files, selected, checks, skipped_platform, limit, timeout_s,
                   include_platform ? "true" : "false");
  (void)sb_append_json_str(&extra, mode);
  (void)sb_appendf(&extra,
                   ",\"selected_by_package\":{\"core\":%d,\"math\":%d,\"os\":%d,"
                   "\"parse\":%d,\"other\":%d}",
                   selected_core, selected_math, selected_os, selected_parse, selected_other);
  (void)sb_append(&extra, ",\"ny_root\":");
  append_rel_json_str(&extra, root, ny_root);
  (void)sb_append(&extra, ",\"work_dir\":");
  append_rel_json_str(&extra, root, work_dir ? work_dir : "");
  char *report_json = build_fuzz_report_json(&rows, &failures,
                                             workspace_rel ? workspace_rel : "",
                                             &extra, "nynth fuzz libs smoke");
  int rc = emit_rows_failures_report(report_json, json_path, workspace_rel,
                                     rows.count, failures.count);
  free(workspace_rel);
  free(workspace);
  free(extra.data);
  free(lib_dir);
  free(work_dir);
  free(scratch_root);
  string_list_free(&files);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static bool kernel_smoke_hot_path(const char *path) {
  if (!path) return false;
  return strstr(path, "kernel-edit-distance.ny") ||
         strstr(path, "kernel-flood-fill-islands.ny") ||
         strstr(path, "kernel-kth-largest-heap.ny") ||
         strstr(path, "kernel-longest-increasing-subsequence.ny") ||
         strstr(path, "kernel-merge-intervals.ny") ||
         strstr(path, "kernel-subarray-sum-k.ny") ||
         strstr(path, "kernel-trie-prefix.ny") ||
         strstr(path, "kernel-word-break.ny") ||
         strstr(path, "kernel-two-sum.ny") ||
         strstr(path, "kernel-valid-anagram.ny") ||
         strstr(path, "kernel-valid-parentheses.ny");
}

static bool kernel_smoke_should_select(const char *path, int index, int total, int limit) {
  if (limit <= 0 || total <= limit) return true;
  if (kernel_smoke_hot_path(path)) return true;
  long long before = ((long long)index * (long long)limit) / (long long)total;
  long long after = ((long long)(index + 1) * (long long)limit) / (long long)total;
  return after > before;
}

static bool parse_kernel_result_line(const char *out, char *name, size_t name_sz,
                                     double *checksum, double *elapsed_ns) {
  if (!out) return false;
  const char *p = strstr(out, "RESULT");
  if (!p) return false;
  p += 6;
  while (*p && isspace((unsigned char)*p)) ++p;
  const char *name_start = p;
  while (*p && !isspace((unsigned char)*p)) ++p;
  size_t n = (size_t)(p - name_start);
  if (n == 0 || n >= name_sz) return false;
  memcpy(name, name_start, n);
  name[n] = '\0';
  while (*p && isspace((unsigned char)*p)) ++p;
  char *end = NULL;
  double c = strtod(p, &end);
  if (end == p) return false;
  p = end;
  while (*p && isspace((unsigned char)*p)) ++p;
  double t = strtod(p, &end);
  if (end == p) return false;
  if (checksum) *checksum = c;
  if (elapsed_ns) *elapsed_ns = t;
  return true;
}

static char *make_kernel_smoke_row(const char *root, const char *source,
                                   const proc_result_t *compile_pr,
                                   const proc_result_t *run_pr,
                                   bool compile_ok, bool run_ok,
                                   bool result_ok, bool signal_ok,
                                   double checksum, double kernel_elapsed_ns,
                                   double signal_score,
                                   const char *result_name,
                                   const char *reason) {
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"target\":\"kernels\",\"kind\":\"fuzz-kernel-smoke\",\"source\":");
  append_rel_json_str(&row, root, source ? source : "");
  (void)sb_append(&row, ",\"name\":");
  (void)sb_append_json_str(&row, ny_base_name(source ? source : "kernel"));
  (void)sb_appendf(&row,
                   ",\"ok\":%s,\"compile_ok\":%s,\"compile_rc\":%d,"
                   "\"compile_ms\":%.2f,\"compile_timed_out\":%s",
                   (compile_ok && run_ok && result_ok) ? "true" : "false",
                   compile_ok ? "true" : "false",
                   compile_pr ? compile_pr->rc : 1,
                   compile_pr ? compile_pr->elapsed_ms : 0.0,
                   (compile_pr && compile_pr->timed_out) ? "true" : "false");
  if (run_pr) {
    (void)sb_appendf(&row,
                     ",\"run_ok\":%s,\"run_rc\":%d,\"run_ms\":%.2f,"
                     "\"run_timed_out\":%s,\"result_ok\":%s,\"signal_ok\":%s",
                     run_ok ? "true" : "false", run_pr->rc, run_pr->elapsed_ms,
                     run_pr->timed_out ? "true" : "false", result_ok ? "true" : "false",
                     signal_ok ? "true" : "false");
    if (result_name && *result_name) {
      (void)sb_append(&row, ",\"result_name\":");
      (void)sb_append_json_str(&row, result_name);
    }
    (void)sb_appendf(&row, ",\"checksum\":%.0f,\"kernel_elapsed_ns\":%.0f,"
                     "\"signal_score\":%.2f",
                     checksum, kernel_elapsed_ns, signal_score);
  }
  if (reason && *reason) {
    (void)sb_append(&row, ",\"reason\":");
    (void)sb_append_json_str(&row, reason);
  }
  if (!compile_ok && compile_pr) append_proc_tail_fields(&row, compile_pr);
  else if (run_pr && (!run_ok || !result_ok)) append_proc_tail_fields(&row, run_pr);
  (void)sb_append_c(&row, '}');
  return sb_take(&row);
}

static int cmd_public_fuzz_kernels_smoke(int argc, char **argv) {
  char root[4096], ny_root[4096], ny_bin[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  if (!find_repo_root_or_sibling(ny_root, sizeof(ny_root)) ||
      !find_ny_bin(ny_root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"nytrix-root-or-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  int limit = atoi(value_after_equals(argc, argv, 4, "--limit", "12"));
  if (has_flag_after(argc, argv, 4, "--full")) limit = 0;
  if (limit < 0) limit = 0;
  double timeout_s = atof(value_after_equals(argc, argv, 4, "--timeout-s", "20"));
  if (timeout_s <= 0.0) timeout_s = 20.0;
  double min_signal_ns = atof(value_after_equals(argc, argv, 4, "--min-signal-ns", "1000000"));
  if (min_signal_ns < 0.0) min_signal_ns = 0.0;
  bool strict_signal = has_flag_after(argc, argv, 4, "--strict-signal");
  bool compile_only = has_flag_after(argc, argv, 4, "--compile-only") ||
                      has_flag_after(argc, argv, 4, "--no-run");
  bool allow_zero = has_flag_after(argc, argv, 4, "--allow-zero");

  char *kernel_corpus_seed_dir = NULL;
  (void)nynth_asprintf(&kernel_corpus_seed_dir, "etc/tests/rt");
  if (kernel_corpus_seed_dir) ny_ensure_dir_recursive(kernel_corpus_seed_dir);
  free(kernel_corpus_seed_dir);
  string_list_t prep_rows = {0}, rows = {0}, failures = {0}, files = {0};
  int materialized = materialize_kernel_fuzz_seeds(root, &prep_rows, &failures);
  string_list_free(&prep_rows);

  char *corpus_dir = NULL;
  (void)nynth_asprintf(&corpus_dir, "etc/tests/rt");
  if (!corpus_dir || !collect_regular_files_recursive(corpus_dir, &files)) {
    (void)string_list_push_take(&failures, make_fuzz_failure(root, "kernels",
                                                            "kernel corpus scan failed",
                                                            corpus_dir ? corpus_dir : ""));
  }
  qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
  int kernel_files = 0;
  for (int i = 0; i < files.count; ++i) {
    const char *bn = ny_base_name(files.items[i]);
    if (strncmp(bn, "kernel-", 7) == 0 && ny_has_suffix(bn, ".ny")) ++kernel_files;
  }

  int selected = 0, compile_checks = 0, run_checks = 0, weak_results = 0;
  int low_signal_results = 0, kernel_index = 0;
  double total_compile_ms = 0.0, total_run_ms = 0.0, total_kernel_elapsed_ns = 0.0;
  double slowest_compile_ms = 0.0, slowest_run_ms = 0.0, slowest_kernel_elapsed_ns = 0.0;
  char slowest_compile[160] = {0}, slowest_run[160] = {0}, slowest_kernel[160] = {0};
  for (int i = 0; i < files.count; ++i) {
    const char *bn = ny_base_name(files.items[i]);
    if (strncmp(bn, "kernel-", 7) != 0 || !ny_has_suffix(bn, ".ny")) continue;
    int current_index = kernel_index++;
    if (!kernel_smoke_should_select(files.items[i], current_index, kernel_files, limit)) continue;
    ++selected;

    char *compile_argv[] = {ny_bin, "--compiler-asserts", "-emit-only", files.items[i], NULL};
    proc_result_t compile_pr = run_proc(compile_argv, root, timeout_s);
    ++compile_checks;
    total_compile_ms += compile_pr.elapsed_ms;
    if (compile_pr.elapsed_ms > slowest_compile_ms) {
      slowest_compile_ms = compile_pr.elapsed_ms;
      snprintf(slowest_compile, sizeof(slowest_compile), "%s", bn);
    }
    bool compile_ok = compile_pr.rc == 0 && !compile_pr.timed_out &&
                      !text_mentions_crash(compile_pr.out, compile_pr.err);

    proc_result_t run_pr;
    memset(&run_pr, 0, sizeof(run_pr));
    bool have_run = false;
    bool run_ok = compile_only ? true : false;
    bool result_ok = compile_only ? true : false;
    bool signal_ok = compile_only ? true : false;
    double signal_score = 0.0;
    double checksum = 0.0, kernel_elapsed_ns = 0.0;
    char result_name[128] = {0};
    const char *reason = "";
    if (!compile_ok) {
      reason = "compile failed";
    } else if (!compile_only) {
      char *run_argv[] = {ny_bin, files.items[i], NULL};
      run_pr = run_proc(run_argv, root, timeout_s);
      have_run = true;
      ++run_checks;
      total_run_ms += run_pr.elapsed_ms;
      if (run_pr.elapsed_ms > slowest_run_ms) {
        slowest_run_ms = run_pr.elapsed_ms;
        snprintf(slowest_run, sizeof(slowest_run), "%s", bn);
      }
      bool parsed = parse_kernel_result_line(run_pr.out, result_name, sizeof(result_name),
                                             &checksum, &kernel_elapsed_ns);
      if (parsed) {
        total_kernel_elapsed_ns += kernel_elapsed_ns;
        if (kernel_elapsed_ns > slowest_kernel_elapsed_ns) {
          slowest_kernel_elapsed_ns = kernel_elapsed_ns;
          snprintf(slowest_kernel, sizeof(slowest_kernel), "%s", bn);
        }
      }
      run_ok = run_pr.rc == 0 && !run_pr.timed_out &&
               !text_mentions_crash(run_pr.out, run_pr.err);
      signal_ok = parsed && kernel_elapsed_ns >= min_signal_ns;
      signal_score = (parsed ? 1.0 : 0.0) +
                     (checksum != 0.0 ? 1.0 : 0.0) +
                     (signal_ok ? 1.0 : 0.0);
      if (parsed && !signal_ok) ++low_signal_results;
      result_ok = parsed && (allow_zero || checksum != 0.0) &&
                  (!strict_signal || signal_ok);
      if (!run_ok) reason = "run failed";
      else if (!parsed) reason = "missing RESULT line";
      else if (!allow_zero && checksum == 0.0) reason = "zero checksum";
      else if (strict_signal && !signal_ok) reason = "low signal";
      if (!result_ok) ++weak_results;
    }
    bool ok = compile_ok && run_ok && result_ok;
    (void)string_list_push_take(&rows,
                                make_kernel_smoke_row(root, files.items[i], &compile_pr,
                                                      have_run ? &run_pr : NULL,
                                                      compile_ok, run_ok, result_ok,
                                                      signal_ok, checksum, kernel_elapsed_ns,
                                                      signal_score,
                                                      result_name, reason));
    if (!ok) {
      char case_name[512];
      snprintf(case_name, sizeof(case_name), "kernel:%s", bn);
      const proc_result_t *bad = !compile_ok ? &compile_pr : (have_run ? &run_pr : &compile_pr);
      (void)string_list_push_take(&failures,
                                  make_worker_failure_row(case_name, "fuzz-kernel-smoke",
                                                          bad->rc ? bad->rc : 1,
                                                          bad->out, reason && *reason ? reason : bad->err));
    }
    if (have_run) proc_result_free(&run_pr);
    proc_result_free(&compile_pr);
  }
  if (kernel_files == 0) {
    (void)string_list_push_take(&failures, make_fuzz_failure(root, "kernels",
                                                            "no kernel corpus files found",
                                                            corpus_dir ? corpus_dir : ""));
  }
  char *workspace = make_fuzz_path(root, "");
  char *workspace_rel = rel_path_dup(root, workspace ? workspace : "");
  str_buf_t extra = {0};
  (void)sb_appendf(&extra,
                   ",\"kernel_files\":%d,\"selected_files\":%d,\"materialized\":%d,"
                   "\"compile_checks\":%d,\"run_checks\":%d,\"weak_results\":%d,"
                   "\"low_signal_results\":%d,\"min_signal_ns\":%.0f,"
                   "\"total_compile_ms\":%.2f,\"total_run_ms\":%.2f,"
                   "\"total_kernel_elapsed_ns\":%.0f,"
                   "\"limit\":%d,\"timeout_s\":%.2f,\"compile_only\":%s,"
                   "\"allow_zero\":%s,\"strict_signal\":%s,\"ny_root\":",
                   kernel_files, selected, materialized, compile_checks, run_checks,
                   weak_results, low_signal_results, min_signal_ns,
                   total_compile_ms, total_run_ms, total_kernel_elapsed_ns,
                   limit, timeout_s, compile_only ? "true" : "false",
                   allow_zero ? "true" : "false", strict_signal ? "true" : "false");
  append_rel_json_str(&extra, root, ny_root);
  (void)sb_append(&extra, ",\"slowest_compile\":{\"name\":");
  (void)sb_append_json_str(&extra, slowest_compile);
  (void)sb_appendf(&extra, ",\"ms\":%.2f}", slowest_compile_ms);
  (void)sb_append(&extra, ",\"slowest_run\":{\"name\":");
  (void)sb_append_json_str(&extra, slowest_run);
  (void)sb_appendf(&extra, ",\"ms\":%.2f}", slowest_run_ms);
  (void)sb_append(&extra, ",\"slowest_kernel\":{\"name\":");
  (void)sb_append_json_str(&extra, slowest_kernel);
  (void)sb_appendf(&extra, ",\"elapsed_ns\":%.0f}", slowest_kernel_elapsed_ns);
  char *report_json = build_fuzz_report_json(&rows, &failures,
                                             workspace_rel ? workspace_rel : "",
                                             &extra, "nynth fuzz kernels smoke");
  int rc = emit_rows_failures_report(report_json, json_path, workspace_rel,
                                     rows.count, failures.count);
  free(workspace_rel);
  free(workspace);
  free(extra.data);
  free(corpus_dir);
  string_list_free(&files);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static void append_command_json_array(str_buf_t *b, const string_list_t *cmd) {
  char root[4096] = {0};
  bool have_root = find_nynth_root(root, sizeof(root)) && root[0];
  (void)sb_append_c(b, '[');
  for (int i = 0; cmd && i < cmd->count; ++i) {
    if (i) (void)sb_append_c(b, ',');
    if (have_root)
      append_repo_or_sibling_rel_json_str(b, root, cmd->items[i]);
    else
      (void)sb_append_json_str(b, cmd->items[i]);
  }
  (void)sb_append_c(b, ']');
}

static int cmd_public_fuzz_afl_run(int argc, char **argv) {
  char root[4096], ny_root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  if (!find_repo_root_or_sibling(ny_root, sizeof(ny_root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  const char *selector = value_after_equals(argc, argv, 4, "--target", "all");
  int minutes = atoi(value_after_equals(argc, argv, 4, "--minutes", "20"));
  int timeout_ms = atoi(value_after_equals(argc, argv, 4, "--timeout-ms", "10000"));
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  bool aggressive = has_flag_after(argc, argv, 4, "--aggressive") ||
                    has_flag_after(argc, argv, 4, "--insane") ||
                    strcmp(selector, "compiler") == 0 ||
                    strcmp(selector, "compiler-ny") == 0 ||
                    strcmp(selector, "ny-compiler") == 0;
  const char *power_schedule = value_after_equals(argc, argv, 4, "--power-schedule",
                                                  aggressive ? "explore" : "");
  bool dry_run = has_flag_after(argc, argv, 4, "--dry-run") || minutes <= 0;
  bool resume = has_flag_after(argc, argv, 4, "--resume");
  bool qemu_requested = has_flag_after(argc, argv, 4, "--qemu");
  const fuzz_target_t *targets[16];
  int target_count = selected_fuzz_targets(selector, targets, 16);
  string_list_t rows = {0}, failures = {0};
  if (target_count < 0) {
    (void)string_list_push_take(&failures, make_fuzz_failure(root, selector, "unknown fuzz target", ""));
    target_count = 0;
  }
  bool afl_available = command_exists_path("afl-fuzz");
  bool afl_fast_available = command_exists_path("afl-clang-fast");
  bool afl_qemu_available = command_exists_path("afl-qemu-trace");
  char *afl_runs_dir = nynth_scratch_pathf(NULL, "afl_runs");
  char *afl_wrapper_dir = nynth_scratch_pathf(NULL, "afl_wrappers");
  char *afl_bin_dir = nynth_scratch_pathf(NULL, "afl_bins");
  if (!dry_run && !resume && afl_runs_dir) ny_ensure_dir_recursive(afl_runs_dir);
  if (!dry_run && !resume && afl_bin_dir) ny_ensure_dir_recursive(afl_bin_dir);
  for (int i = 0; i < target_count; ++i) {
    const fuzz_target_t *t = targets[i];
    char *corpus = NULL, *out = NULL, *stats = NULL, *dict = NULL, *script = NULL, *bin = NULL;
    char *instrumented_bin = NULL, *compiler_wrapper = NULL;
    char *source_bin = NULL, *pinned_bin = NULL;
    (void)nynth_asprintf(&corpus, "fuzz/corpus/%s", t->name);
    if (dry_run || resume) {
      (void)nynth_asprintf(&out, "build/fuzz/out/%s", t->name);
    } else {
      (void)asprintf(&out, "%s/%s_%ld_%d", afl_runs_dir ? afl_runs_dir : "/tmp",
                     t->name, (long)time(NULL), (int)getpid());
    }
    stats = afl_stats_path_for_out(out ? out : "");
    (void)asprintf(&instrumented_bin, "%s/build/afl/release/ny", ny_root);
    bool instrumented = afl_fast_available && instrumented_bin && path_exists_file(instrumented_bin);
    bool qemu_mode = !instrumented && afl_qemu_available && qemu_requested && !t->direct_mode;
    bool dumb_mode = !instrumented && !qemu_mode;
    (void)asprintf(&bin, "%s", instrumented ? instrumented_bin : "");
    if (!bin || !*bin) {
      free(bin);
      bin = NULL;
      (void)asprintf(&bin, "%s/build/release/ny", ny_root);
    }
    if (bin && *bin) source_bin = strdup(bin);
    if (!dry_run && !resume && bin && path_exists_file(bin)) {
      pinned_bin = copy_afl_binary_to_cache(afl_bin_dir, t->name, bin);
      if (pinned_bin && path_exists_file(pinned_bin)) {
        free(bin);
        bin = strdup(pinned_bin);
      }
    }
    if (t->direct_mode && bin && *bin) {
      compiler_wrapper = write_afl_compiler_wrapper(afl_wrapper_dir, t->name,
                                                    bin, ny_root, root);
    } else if (bin && *bin) {
      compiler_wrapper = write_afl_passthrough_wrapper(afl_wrapper_dir, t->name,
                                                       bin, ny_root, root);
    }
    string_list_t cmd = {0};
    (void)string_list_push_copy(&cmd, "env");
    (void)string_list_push_copy(&cmd, "AFL_SKIP_CPUFREQ=1");
    (void)string_list_push_copy(&cmd, "AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1");
    (void)string_list_push_copy(&cmd, "AFL_NO_UI=1");
    if (aggressive) {
      (void)string_list_push_copy(&cmd, "AFL_IMPORT_FIRST=1");
      (void)string_list_push_copy(&cmd, "AFL_FAST_CAL=1");
      (void)string_list_push_copy(&cmd, "AFL_CMPLOG_ONLY_NEW=1");
      (void)string_list_push_copy(&cmd, "AFL_TESTCACHE_SIZE=200");
    }
    (void)string_list_push_copy(&cmd, "afl-fuzz");
    (void)string_list_push_copy(&cmd, "-i");
    (void)string_list_push_copy(&cmd, stats && path_exists_file(stats) ? "-" : (corpus ? corpus : ""));
    (void)string_list_push_copy(&cmd, "-o");
    (void)string_list_push_copy(&cmd, out ? out : "");
    (void)string_list_push_copy(&cmd, "-m");
    (void)string_list_push_copy(&cmd, "none");
    (void)string_list_push_copy(&cmd, "-t");
    char timeout_arg[64], seconds_arg[64];
    snprintf(timeout_arg, sizeof(timeout_arg), "%d+", timeout_ms);
    snprintf(seconds_arg, sizeof(seconds_arg), "%d", minutes < 0 ? 0 : minutes * 60);
    (void)string_list_push_copy(&cmd, timeout_arg);
    (void)string_list_push_copy(&cmd, "-V");
    (void)string_list_push_copy(&cmd, seconds_arg);
    if (power_schedule && *power_schedule) {
      (void)string_list_push_copy(&cmd, "-p");
      (void)string_list_push_copy(&cmd, power_schedule);
    }
    if (t->dict) {
      (void)nynth_asprintf(&dict, "fuzz/dict/%s", t->dict);
      if (dict && path_exists_file(dict)) {
        (void)string_list_push_copy(&cmd, "-x");
        (void)string_list_push_copy(&cmd, dict);
      }
    }
    if (qemu_mode) (void)string_list_push_copy(&cmd, "-Q");
    if (dumb_mode) (void)string_list_push_copy(&cmd, "-n");
    (void)string_list_push_copy(&cmd, "--");
    (void)string_list_push_copy(&cmd, compiler_wrapper ? compiler_wrapper : (bin ? bin : ""));
    if (!t->direct_mode && t->script) {
      (void)nynth_asprintf(&script, "fuzz/targets/%s", t->script);
      (void)string_list_push_copy(&cmd, script ? script : "");
    }
    (void)string_list_push_copy(&cmd, "@@");
    str_buf_t preflight = {0};
    if (!dry_run) {
      if (!afl_available) (void)sb_append(&preflight, "afl-fuzz not found in PATH");
      if (!bin || !path_exists_file(bin)) {
        if (preflight.len) (void)sb_append(&preflight, "; ");
        (void)sb_append(&preflight, "ny binary missing");
      }
      if (source_bin && *source_bin && !pinned_bin && !resume) {
        if (preflight.len) (void)sb_append(&preflight, "; ");
        (void)sb_append(&preflight, "pinned ny binary copy failed");
      }
      if (t->direct_mode && (!compiler_wrapper || !path_exists_file(compiler_wrapper))) {
        if (preflight.len) (void)sb_append(&preflight, "; ");
        (void)sb_append(&preflight, "compiler wrapper missing");
      }
      if (!corpus || !dir_has_any_entry(corpus)) {
        if (preflight.len) (void)sb_append(&preflight, "; ");
        (void)sb_append(&preflight, "corpus missing or empty");
      }
      if (!t->direct_mode && t->script && (!script || !path_exists_file(script))) {
        if (preflight.len) (void)sb_append(&preflight, "; ");
        (void)sb_append(&preflight, "target script missing");
      }
    }
    proc_result_t pr = {0};
    bool executed = false;
    if (!dry_run && preflight.len == 0) {
      char **proc_argv = (char **)calloc((size_t)cmd.count + 1u, sizeof(char *));
      if (!proc_argv) {
        (void)sb_append(&preflight, "failed to allocate afl argv");
      } else {
        for (int c = 0; c < cmd.count; ++c) proc_argv[c] = cmd.items[c];
        double run_timeout_s = (minutes > 0 ? (double)minutes * 60.0 : 0.0) + 180.0;
        pr = run_proc(proc_argv, root, run_timeout_s);
        executed = true;
        free(proc_argv);
      }
    }
    free(stats);
    stats = afl_stats_path_for_out(out ? out : "");
    afl_stats_t afl_stats = read_afl_stats(stats);
    bool afl_findings = afl_stats.have &&
                        (afl_stats.saved_crashes > 0 || afl_stats.saved_hangs > 0);
    bool ok = dry_run || (preflight.len == 0 && pr.rc == 0 && !afl_findings);
    if (executed && aggressive) {
      printf("afl target %s rc=%d elapsed_ms=%.2f out=%s\n",
             t->name, pr.rc, pr.elapsed_ms, out ? out : "");
      if (pr.out && *pr.out) {
        printf("afl stdout tail (%s):\n", t->name);
        print_tail_text(stdout, pr.out, 4000);
      }
      if (pr.err && *pr.err) {
        printf("afl stderr tail (%s):\n", t->name);
        print_tail_text(stdout, pr.err, 4000);
      }
      fflush(stdout);
    }
    if (!ok) {
      char finding_error[256];
      int failure_rc = preflight.len ? 1 : ((afl_findings && pr.rc == 0) ? 1 : pr.rc);
      snprintf(finding_error, sizeof(finding_error),
               "afl saved findings: crashes=%ld hangs=%ld stats=%s",
               afl_stats.saved_crashes, afl_stats.saved_hangs, stats ? stats : "");
      (void)string_list_push_take(&failures,
                                  make_worker_failure_row(t->name, "afl-fuzz",
                                                          failure_rc,
                                                          pr.out ? pr.out : "",
                                                          preflight.len ? preflight.data :
                                                          (afl_findings ? finding_error :
                                                           (pr.err ? pr.err : ""))));
    }
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"target\":");
    (void)sb_append_json_str(&row, t->name);
    (void)sb_appendf(&row,
                     ",\"dry_run\":%s,\"ok\":%s,\"executed\":%s,\"rc\":%d,"
                     "\"elapsed_ms\":%.2f,\"timed_out\":%s,\"qemu_mode\":%s,"
                         "\"qemu_requested\":%s,\"dumb_mode\":%s,\"instrumented\":%s,\"afl_clang_fast\":%s,"
                         "\"compile_only\":%s,\"compiler_asserts\":%s,"
                         "\"normalized_compiler_exit\":%s,"
                         "\"resume\":%s,\"aggressive\":%s,\"minutes\":%d,\"timeout_ms\":%d,\"out\":",
                     dry_run ? "true" : "false",
                     ok ? "true" : "false", executed ? "true" : "false",
                     dry_run ? 0 : (preflight.len ? 1 : pr.rc),
                     dry_run ? 0.0 : pr.elapsed_ms, pr.timed_out ? "true" : "false",
                     qemu_mode ? "true" : "false",
                     qemu_requested ? "true" : "false",
                     dumb_mode ? "true" : "false",
                         instrumented ? "true" : "false",
                         afl_fast_available ? "true" : "false",
                         t->direct_mode ? "true" : "false",
                         t->direct_mode ? "true" : "false",
                         t->direct_mode && compiler_wrapper ? "true" : "false",
                         resume ? "true" : "false",
                         aggressive ? "true" : "false",
                         minutes, timeout_ms);
    append_rel_json_str(&row, root, out ? out : "");
    (void)sb_append(&row, ",\"source_bin\":");
    append_rel_json_str(&row, root, source_bin ? source_bin : "");
    (void)sb_append(&row, ",\"pinned_bin\":");
    append_rel_json_str(&row, root, pinned_bin ? pinned_bin : "");
    (void)sb_append(&row, ",\"stats\":");
    append_rel_json_str(&row, root, stats ? stats : "");
    (void)sb_appendf(&row,
                     ",\"have_stats\":%s,\"saved_crashes\":%ld,"
                     "\"saved_hangs\":%ld,\"execs_done\":%ld,"
                     "\"execs_per_sec\":%.2f",
                     afl_stats.have ? "true" : "false",
                     afl_stats.saved_crashes, afl_stats.saved_hangs,
                     afl_stats.execs_done, afl_stats.execs_per_sec);
    (void)sb_append(&row, ",\"command\":");
    append_command_json_array(&row, &cmd);
    if (preflight.len) {
      (void)sb_append(&row, ",\"preflight\":");
      (void)sb_append_json_str(&row, preflight.data);
    }
    if (executed) append_proc_tail_fields(&row, &pr);
    (void)sb_append_c(&row, '}');
    (void)string_list_push_take(&rows, sb_take(&row));
    proc_result_free(&pr);
    free(preflight.data);
    string_list_free(&cmd);
    free(corpus); free(out); free(stats); free(dict); free(script); free(bin);
    free(instrumented_bin); free(compiler_wrapper); free(source_bin);
    free(pinned_bin);
  }
  char *workspace = make_fuzz_path(root, "");
  char *workspace_rel = rel_path_dup(root, workspace ? workspace : "");
  str_buf_t extra = {0};
  (void)sb_appendf(&extra,
                   ",\"targets\":%d,\"dry_run\":%s,\"resume\":%s,\"afl_fuzz\":%s,"
                   "\"afl_clang_fast\":%s,\"afl_qemu_trace\":%s,"
                   "\"qemu_requested\":%s,\"aggressive\":%s,\"power_schedule\":",
                   rows.count, dry_run ? "true" : "false", resume ? "true" : "false",
                   afl_available ? "true" : "false",
                   afl_fast_available ? "true" : "false", afl_qemu_available ? "true" : "false",
                   qemu_requested ? "true" : "false",
                   aggressive ? "true" : "false");
  (void)sb_append_json_str(&extra, power_schedule && *power_schedule ? power_schedule : "");
  (void)sb_append(&extra, ",\"build_rc\":0,\"build_ms\":0.0");
  char *report_json = build_fuzz_report_json(&rows, &failures,
                                             workspace_rel ? workspace_rel : "",
                                             &extra, "nynth fuzz afl run");
  int rc = emit_rows_failures_report(report_json, json_path, workspace_rel, rows.count, failures.count);
  free(workspace_rel); free(extra.data); free(workspace); free(afl_runs_dir);
  free(afl_wrapper_dir);
  free(afl_bin_dir);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static int cmd_public_fuzz_sanitizers_run(int argc, char **argv) {
  char root[4096], ny_root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  if (!find_repo_root_or_sibling(ny_root, sizeof(ny_root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  char make_path[4096];
  if (!path_join(make_path, sizeof(make_path), ny_root, "make") ||
      !executable_path(make_path)) {
    printf("{\"ok\":false,\"error\":\"nytrix-make-not-found\",\"path\":");
    json_str(stdout, make_path);
    printf("}\n");
    return 2;
  }
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  double timeout_s = atof(value_after_equals(argc, argv, 4, "--timeout-s", "1200"));
  bool dry_run = has_flag_after(argc, argv, 4, "--dry-run");
  bool smoke = has_flag_after(argc, argv, 4, "--smoke") ||
               has_flag_after(argc, argv, 4, "--fast");
  if (has_flag_after(argc, argv, 4, "--full")) smoke = false;
  bool no_bootstrap_logs = has_flag_after(argc, argv, 4, "--no-bootstrap-logs");
  const char *jobs = value_after_equals(argc, argv, 4, "--jobs", "");
  if (!jobs || !*jobs) jobs = value_after_equals(argc, argv, 4, "-j", "");
  string_list_t rows = {0}, failures = {0};
  typedef struct {
    const char *name;
    const char *make_arg;
  } sanitizer_lane_t;
  const sanitizer_lane_t lanes[] = {
    {"nytrix_asan", "asan"},
    {"nytrix_ubsan", "ubsan"}
  };
  for (size_t i = 0; i < sizeof(lanes) / sizeof(lanes[0]); ++i) {
    string_list_t cmd = {0};
    (void)string_list_push_copy(&cmd, make_path);
    (void)string_list_push_copy(&cmd, lanes[i].make_arg);
    if (smoke) (void)string_list_push_copy(&cmd, "--smoke");
    if (jobs && *jobs) {
      (void)string_list_push_copy(&cmd, "-j");
      (void)string_list_push_copy(&cmd, jobs);
    }
    if (no_bootstrap_logs)
      (void)string_list_push_copy(&cmd, "--no-bootstrap-logs");
    str_buf_t row = {0};
    proc_result_t pr = {0};
    if (!dry_run) {
      char *proc_argv[8];
      int pa = 0;
      proc_argv[pa++] = make_path;
      proc_argv[pa++] = (char *)lanes[i].make_arg;
      if (smoke) proc_argv[pa++] = "--smoke";
      if (jobs && *jobs) {
        proc_argv[pa++] = "-j";
        proc_argv[pa++] = (char *)jobs;
      }
      if (no_bootstrap_logs)
        proc_argv[pa++] = "--no-bootstrap-logs";
      proc_argv[pa] = NULL;
      pr = run_proc(proc_argv, ny_root, timeout_s);
    }
    (void)sb_append(&row, "{\"name\":");
    (void)sb_append_json_str(&row, lanes[i].name);
    (void)sb_append(&row, ",\"kind\":\"fuzz-sanitizer\",\"phase\":\"sanitizer\",\"lane\":");
    (void)sb_append_json_str(&row, lanes[i].make_arg);
    (void)sb_appendf(&row, ",\"required\":true,\"dry_run\":%s,\"ok\":%s,\"rc\":%d,\"elapsed_ms\":%.2f,\"command\":",
                     dry_run ? "true" : "false", (dry_run || pr.rc == 0) ? "true" : "false",
                     dry_run ? 0 : pr.rc, dry_run ? 0.0 : pr.elapsed_ms);
    append_command_json_array(&row, &cmd);
    if (!dry_run && pr.rc != 0)
      append_proc_tail_fields(&row, &pr);
    (void)sb_append(&row, ",\"nytrix_root\":");
    append_rel_json_str(&row, root, ny_root);
    (void)sb_append_c(&row, '}');
    (void)string_list_push_take(&rows, sb_take(&row));
    if (!dry_run && pr.rc != 0) {
      (void)string_list_push_take(&failures, make_worker_failure_row(lanes[i].name, "sanitizer", pr.rc, pr.out, pr.err));
    }
    proc_result_free(&pr);
    string_list_free(&cmd);
  }
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"mode\":\"fuzz-sanitizers\",\"nytrix_root\":");
  append_rel_json_str(&extra, root, ny_root);
  (void)sb_append(&extra, ",\"nytrix_make\":");
  append_rel_json_str(&extra, root, make_path);
  (void)sb_appendf(&extra, ",\"lanes\":%d,\"dry_run\":%s,\"smoke\":%s,\"timeout_s\":%.3f",
                   rows.count, dry_run ? "true" : "false",
                   smoke ? "true" : "false", timeout_s);
  char *report_json = build_fuzz_report_json(&rows, &failures, "", &extra, "nynth fuzz sanitizers run");
  int rc = emit_rows_failures_report(report_json, json_path, "", rows.count, failures.count);
  free(extra.data);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static double fuzz_gc_duration_s(int argc, char **argv, bool smoke) {
  double duration_s = atof(value_after_equals(argc, argv, 4, "--budget-s", "0"));
  if (duration_s <= 0.0)
    duration_s = atof(value_after_equals(argc, argv, 4, "--duration-s", "0"));
  if (duration_s <= 0.0) {
    double minutes = atof(value_after_equals(argc, argv, 4, "--minutes", "0"));
    if (minutes > 0.0) duration_s = minutes * 60.0;
  }
  if (duration_s <= 0.0) {
    double hours = atof(value_after_equals(argc, argv, 4, "--hours", "0"));
    if (hours > 0.0) duration_s = hours * 3600.0;
  }
  if (duration_s <= 0.0) duration_s = smoke ? 2.0 : 300.0;
  return duration_s;
}

static bool fuzz_gc_has_duration_arg(int argc, char **argv) {
  for (int i = 4; i < argc; ++i) {
    if (strcmp(argv[i], "--duration-s") == 0 || strcmp(argv[i], "--minutes") == 0 ||
        strcmp(argv[i], "--hours") == 0 ||
        strcmp(argv[i], "--budget-s") == 0 ||
        strncmp(argv[i], "--duration-s=", 13) == 0 ||
        strncmp(argv[i], "--minutes=", 10) == 0 ||
        strncmp(argv[i], "--hours=", 8) == 0 ||
        strncmp(argv[i], "--budget-s=", 11) == 0)
      return true;
  }
  return false;
}

static const char *gc_resolve_ny_family(const char *family, int seed) {
  static const char *families[] = {"lists", "dicts", "results", "closures", "strings", "mixed"};
  if (family && *family && strcmp(family, "auto") != 0) {
    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
      if (strcmp(family, families[i]) == 0) return families[i];
    }
  }
  unsigned pick = (unsigned)seed * 1103515245u + 12345u;
  return families[pick % (sizeof(families) / sizeof(families[0]))];
}

static char *make_gc_ny_fuzz_source(int seed, int rounds, int workers, const char *family) {
  (void)workers;
  const char *resolved_family = gc_resolve_ny_family(family, seed);
  str_buf_t b = {0};
  (void)sb_append(&b, "use std.core *\nuse std.core.dict\nuse std.core.str\n\n");
  (void)sb_appendf(&b, "def GC_FUZZ_SEED = %d\n", seed);
  (void)sb_appendf(&b, "def GC_FUZZ_FAMILY = \"%s\"\n", resolved_family);
  (void)sb_append(&b,
    "\nfn gc_mix(acc, v){\n"
    "   ((acc * 131) + (int(v) * 17) + 97) % 1000000007\n"
    "}\n"
    "\nfn gc_make_child(seed, i, j, acc){\n"
    "   [seed, i, j, acc, \"gc_\" + to_str(seed) + \"_\" + to_str(i) + \"_\" + to_str(j)]\n"
    "}\n"
    "\nfn gc_lists(seed, rounds){\n"
    "   mut roots = []\n"
    "   mut acc = seed\n"
    "   mut i = 0\n"
    "   while(i < rounds){\n"
    "      mut frame = []\n"
    "      mut j = 0\n"
    "      while(j < 18){\n"
    "         def child = gc_make_child(seed, i, j, acc)\n"
    "         frame = [child, frame, [seed, i, j, acc], roots]\n"
    "         if(len(child) < 5){\n"
    "            print(\"gc_fuzz_fail short_child\")\n"
    "            return -1\n"
    "         }\n"
    "         acc = gc_mix(acc, len(child) + int(get(child, 2, 0)) + len(frame) + j)\n"
    "         j += 1\n"
    "      }\n"
    "      roots = [frame, roots, [\"summary\", seed, i, acc]]\n"
    "      def pick = get(roots, 0, frame)\n"
    "      if(len(pick) <= 0){\n"
    "         print(\"gc_fuzz_fail empty_pick\")\n"
    "         return -2\n"
    "      }\n"
    "      acc = gc_mix(acc, len(pick))\n"
    "      if((i % 5) == 0){\n"
    "         def tomb = [[seed, i, acc], [acc, i, seed], [\"drop\", seed, i]]\n"
    "         acc = gc_mix(acc, len(tomb))\n"
    "      }\n"
    "      i += 1\n"
    "   }\n"
    "   gc_mix(acc, len(roots))\n"
    "}\n"
    "\nfn gc_dicts(seed, rounds){\n"
    "   mut d = dict(32)\n"
    "   mut roots = []\n"
    "   mut acc = seed + 11\n"
    "   mut i = 0\n"
    "   while(i < rounds){\n"
    "      mut j = 0\n"
    "      while(j < 12){\n"
    "         def key = \"k_\" + to_str(seed) + \"_\" + to_str((i * 37 + j) % 257)\n"
    "         def val = [key, seed, i, j, acc, roots]\n"
    "         d = d.set(key, val)\n"
    "         def got = d.get(key, [])\n"
    "         if(len(got) < 5){\n"
    "            print(\"gc_fuzz_fail dict_get\")\n"
    "            return -3\n"
    "         }\n"
    "         roots = [got, roots, d]\n"
    "         acc = gc_mix(acc, len(got) + int(get(got, 2, 0)) + d.len)\n"
    "         j += 1\n"
    "      }\n"
    "      if((i % 4) == 0){\n"
    "         def old_key = \"k_\" + to_str(seed) + \"_\" + to_str(((i / 2) * 37) % 257)\n"
    "         acc = gc_mix(acc, len(d.get(old_key, [seed, i, acc])))\n"
    "      }\n"
    "      i += 1\n"
    "   }\n"
    "   gc_mix(acc, d.len + len(roots))\n"
    "}\n"
    "\nfn gc_results(seed, rounds){\n"
    "   mut acc = seed + 23\n"
    "   mut roots = []\n"
    "   mut i = 0\n"
    "   while(i < rounds){\n"
    "      def payload = [seed, i, acc, roots, \"res_\" + to_str(i)]\n"
    "      def r = ((i + seed) % 4 == 0) ? err(payload) : ok(payload)\n"
    "      if(is_ok(r)){\n"
    "         def v = unwrap(r)\n"
    "         acc = gc_mix(acc, len(v) + int(get(v, 1, 0)))\n"
    "         roots = [r, v, roots]\n"
    "      } else {\n"
    "         def v = unwrap_or(r, [seed, i, acc])\n"
    "         acc = gc_mix(acc, len(v) + 7)\n"
    "         roots = [r, roots, v]\n"
    "      }\n"
    "      i += 1\n"
    "   }\n"
    "   gc_mix(acc, len(roots))\n"
    "}\n"
    "\nfn gc_call(thunk){ thunk() }\n"
    "\nfn gc_closures(seed, rounds){\n"
    "   mut acc = seed + 37\n"
    "   mut roots = []\n"
    "   mut i = 0\n"
    "   while(i < rounds){\n"
    "      def thunk = fn(){ [seed, i, acc, roots, \"closure_\" + to_str(i)] }\n"
    "      def got = gc_call(thunk)\n"
    "      if(len(got) < 5){\n"
    "         print(\"gc_fuzz_fail closure\")\n"
    "         return -4\n"
    "      }\n"
    "      roots = [thunk, got, roots]\n"
    "      acc = gc_mix(acc, len(got) + int(get(got, 1, 0)))\n"
    "      i += 1\n"
    "   }\n"
    "   gc_mix(acc, len(roots))\n"
    "}\n"
    "\nfn gc_strings(seed, rounds){\n"
    "   mut acc = seed + 53\n"
    "   mut roots = []\n"
    "   mut i = 0\n"
    "   while(i < rounds){\n"
    "      def s = \"gc_\" + to_str(seed) + \"_\" + to_str(i) + \"_\" + to_str(acc % 997)\n"
    "      def t = s + \"_tail_\" + to_str((i + seed) % 17)\n"
    "      def code = t.len + (i % 19)\n"
    "      roots = [[s, t, code, acc], roots]\n"
    "      acc = gc_mix(acc, t.len + code + len(roots))\n"
    "      i += 1\n"
    "   }\n"
    "   gc_mix(acc, len(roots))\n"
    "}\n"
    "\nfn gc_mixed(seed, rounds){\n"
    "   mut acc = seed + 71\n"
    "   mut roots = []\n"
    "   mut d = dict(32)\n"
    "   mut i = 0\n"
    "   while(i < rounds){\n"
    "      def s = \"mix_\" + to_str(seed) + \"_\" + to_str(i) + \"_\" + to_str(acc % 997)\n"
    "      def xs = [seed, i, acc, s, roots]\n"
    "      def key = \"mk_\" + to_str((i * 131 + seed) % 4099)\n"
    "      d = d.set(key, xs)\n"
    "      def got = d.get(key, xs)\n"
    "      def r = ((i + seed) % 3 == 0) ? err(got) : ok(got)\n"
    "      def thunk = fn(){ [s, got, r, acc] }\n"
    "      def cl = gc_call(thunk)\n"
    "      roots = [d, got, r, thunk, cl, roots]\n"
    "      acc = gc_mix(acc, len(got) + len(cl) + d.len + s.len + len(roots))\n"
    "      i += 1\n"
    "   }\n"
    "   gc_mix(acc, d.len + len(roots))\n"
    "}\n"
    "\nfn gc_run_family(seed, rounds){\n"
    "   if(GC_FUZZ_FAMILY == \"lists\"){ return gc_lists(seed, rounds) }\n"
    "   if(GC_FUZZ_FAMILY == \"dicts\"){ return gc_dicts(seed, rounds) }\n"
    "   if(GC_FUZZ_FAMILY == \"results\"){ return gc_results(seed, rounds) }\n"
    "   if(GC_FUZZ_FAMILY == \"closures\"){ return gc_closures(seed, rounds) }\n"
    "   if(GC_FUZZ_FAMILY == \"strings\"){ return gc_strings(seed, rounds) }\n"
    "   gc_mixed(seed, rounds)\n"
    "}\n\n"
    "fn main(){\n");
  (void)sb_appendf(&b, "   mut rounds = %d\n", rounds);
  (void)sb_append(&b,
    "   def total = gc_run_family(GC_FUZZ_SEED, rounds)\n"
    "   if(total < 0){ return 1 }\n"
    "   def checksum = gc_mix(total, GC_FUZZ_SEED + rounds + GC_FUZZ_FAMILY.len)\n"
    "   print(\"gc_fuzz_ok family=\" + GC_FUZZ_FAMILY + \" seed=\" + to_str(GC_FUZZ_SEED) + \" rounds=\" + to_str(rounds) + \" checksum=\" + to_str(checksum) + \" total=\" + to_str(total))\n"
    "   0\n"
    "}\n\n"
    "main()\n");
  return sb_take(&b);
}

static void append_gc_proc_row(string_list_t *rows, const char *root, const char *name,
                               const char *phase, bool ok, const proc_result_t *pr,
                               const char *source_path, const char *artifact_path,
                               const char *stats_json) {
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, name ? name : "");
  (void)sb_append(&row, ",\"kind\":\"fuzz-gc\",\"phase\":");
  (void)sb_append_json_str(&row, phase ? phase : "");
  (void)sb_appendf(&row, ",\"ok\":%s,\"rc\":%d,\"elapsed_ms\":%.2f,\"timed_out\":%s",
                   ok ? "true" : "false", pr ? pr->rc : 0,
                   pr ? pr->elapsed_ms : 0.0, (pr && pr->timed_out) ? "true" : "false");
  if (source_path && *source_path) {
    (void)sb_append(&row, ",\"source\":");
    append_rel_json_str(&row, root, source_path);
  }
  if (artifact_path && *artifact_path) {
    (void)sb_append(&row, ",\"artifact\":");
    append_rel_json_str(&row, root, artifact_path);
  }
  if (stats_json && *skip_ws_const(stats_json) == '{') {
    (void)sb_append(&row, ",\"stats\":");
    (void)sb_append(&row, skip_ws_const(stats_json));
  }
  if (pr && (!ok || pr->out || pr->err)) append_proc_tail_fields(&row, pr);
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
}

static void append_argv_json_array(str_buf_t *b, char *const argv[]) {
  char root[4096] = {0};
  bool have_root = find_nynth_root(root, sizeof(root)) && root[0];
  (void)sb_append_c(b, '[');
  for (int i = 0; argv && argv[i]; ++i) {
    if (i) (void)sb_append_c(b, ',');
    if (have_root)
      append_repo_or_sibling_rel_json_str(b, root, argv[i]);
    else
      (void)sb_append_json_str(b, argv[i]);
  }
  (void)sb_append_c(b, ']');
}

typedef struct gc_lane_metrics {
  bool ok;
  int rc;
  int seed;
  int sub_rows;
  double sub_failures;
  double elapsed_ms;
  double scheduler_score;
  double objects_promoted;
  double remembered_events;
  double minor_collections;
  double major_collections;
  double graph_score;
  double tag_coverage;
  double ops;
  double elapsed_s;
  double ops_per_s;
  double checksum;
  char lane[80];
  char direct_mode[64];
  char ny_family[64];
  char report[1024];
  char stdout_log[1024];
  char stderr_log[1024];
  char command_log[1024];
} gc_lane_metrics_t;

static void append_argv_shell_string(str_buf_t *b, char *const argv[]) {
  for (int i = 0; argv && argv[i]; ++i) {
    if (i) (void)sb_append_c(b, ' ');
    (void)sb_append(b, argv[i]);
  }
}

static int gc_online_thread_count(void) {
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  if (cpus < 1) return 1;
  if (cpus > 1024) return 1024;
  return (int)cpus;
}

static int gc_clamp_thread_count(int threads) {
  if (threads < 1) return 1;
  if (threads > 256) return 256;
  return threads;
}

static int gc_parse_thread_count(const char *arg, int fallback) {
  int cpus = gc_online_thread_count();
  fallback = gc_clamp_thread_count(fallback);
  if (!arg || !*arg || strcmp(arg, "auto") == 0) return fallback;
  if (strcmp(arg, "all") == 0 || strcmp(arg, "max") == 0 ||
      strcmp(arg, "cpu") == 0 || strcmp(arg, "cores") == 0)
    return gc_clamp_thread_count(cpus);
  size_t len = strlen(arg);
  if (len > 1 && arg[len - 1] == '%') {
    double pct = atof(arg);
    int threads = (int)((double)cpus * (pct / 100.0) + 0.5);
    return gc_clamp_thread_count(threads);
  }
  static const char *reserve_prefixes[] = {"all-", "cpu-", "cores-"};
  for (size_t i = 0; i < sizeof(reserve_prefixes) / sizeof(reserve_prefixes[0]); ++i) {
    size_t prefix_len = strlen(reserve_prefixes[i]);
    if (strncmp(arg, reserve_prefixes[i], prefix_len) == 0) {
      int reserve = atoi(arg + prefix_len);
      return gc_clamp_thread_count(cpus - reserve);
    }
  }
  char *end = NULL;
  long value = strtol(arg, &end, 10);
  if (end && end != arg) return gc_clamp_thread_count((int)value);
  return fallback;
}

static int gc_default_fuzz_thread_count(void) {
  int cpus = gc_online_thread_count();
  int threads = (int)((double)cpus * 0.25 + 0.5);
  if (threads < 1) threads = 1;
  return gc_clamp_thread_count(threads);
}

static bool gc_write_seed_snapshot(const char *source, const char *snapshot_path) {
  if (!snapshot_path || !*snapshot_path) return false;
  file_buf_t f = {0};
  bool have_source = source && *source && read_file(source, &f) && f.data;
  const char *payload = have_source ? f.data : "{\"seeds\":[],\"version\":1}\n";
  bool ok = write_file_text(snapshot_path, payload);
  free(f.data);
  return ok;
}

static bool extract_gc_stdout_number(const char *text, const char *key, double *out) {
  if (!text || !key || !*key || !out) return false;
  size_t key_len = strlen(key);
  const char *p = text;
  while ((p = strstr(p, key)) != NULL) {
    bool left_ok = p == text || isspace((unsigned char)p[-1]) || p[-1] == ',' || p[-1] == '{';
    if (left_ok && p[key_len] == '=') {
      char *end = NULL;
      double v = strtod(p + key_len + 1, &end);
      if (end && end != p + key_len + 1) {
        *out = v;
        return true;
      }
    }
    ++p;
  }
  return false;
}

static bool gc_extract_metrics(const char *json, gc_lane_metrics_t *m) {
  if (!json || !m) return false;
  double value = 0.0;
  bool any = false;
  if (extract_json_number(json, "seed", &value)) {
    m->seed = (int)value;
    any = true;
  }
  if (extract_json_number(json, "scheduler_score", &value)) {
    m->scheduler_score = value;
    any = true;
  }
  if (extract_json_number(json, "objects_promoted", &value)) {
    m->objects_promoted = value;
    any = true;
  }
  if (extract_json_number(json, "remembered_events", &value)) {
    m->remembered_events = value;
    any = true;
  }
  if (extract_json_number(json, "minor_collections", &value)) {
    m->minor_collections = value;
    any = true;
  }
  if (extract_json_number(json, "major_collections", &value)) {
    m->major_collections = value;
    any = true;
  }
  if (extract_json_number(json, "graph_score", &value)) {
    m->graph_score = value;
    any = true;
  }
  if (extract_json_number(json, "tag_coverage", &value)) {
    m->tag_coverage = value;
    any = true;
  }
  if (extract_json_number(json, "ops", &value)) {
    m->ops = value;
    any = true;
  }
  if (extract_json_number(json, "elapsed_s", &value)) {
    m->elapsed_s = value;
    any = true;
  }
  if (extract_json_number(json, "ops_per_s", &value)) {
    m->ops_per_s = value;
    any = true;
  }
  if (extract_json_number(json, "checksum", &value)) {
    m->checksum = value;
    any = true;
  }
  const char *end = json + strlen(json);
  char *direct_mode = json_extract_string_range(json, end, "direct_mode");
  if (direct_mode) {
    snprintf(m->direct_mode, sizeof(m->direct_mode), "%s", direct_mode);
    free(direct_mode);
    any = true;
  }
  char *ny_family = json_extract_string_range(json, end, "ny_family");
  if (!ny_family) ny_family = json_extract_string_range(json, end, "family");
  if (ny_family) {
    snprintf(m->ny_family, sizeof(m->ny_family), "%s", ny_family);
    free(ny_family);
    any = true;
  }
  if (m->ops_per_s <= 0.0 && m->ops > 0.0 && m->elapsed_s > 0.0)
    m->ops_per_s = m->ops / m->elapsed_s;
  if (m->scheduler_score <= 0.0) {
    m->scheduler_score = m->objects_promoted * 4.0 +
                         (m->minor_collections + m->major_collections) * 3.0 +
                         m->remembered_events * 25.0 +
                         m->graph_score;
  }
  return any;
}

static int gc_seed_cache_load(const char *path, int *seeds, int max_seeds) {
  if (!path || !*path || !seeds || max_seeds <= 0) return 0;
  file_buf_t f = {0};
  if (!read_file(path, &f) || !f.data) return 0;
  int count = 0;
  const char *p = f.data;
  while (count < max_seeds && (p = strstr(p, "\"seed\"")) != NULL) {
    p += 6;
    p = strchr(p, ':');
    if (!p) break;
    ++p;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end != p) seeds[count++] = (int)v;
    p = end && end > p ? end : p + 1;
  }
  free(f.data);
  return count;
}

static void gc_seed_cache_write(const char *path, const gc_lane_metrics_t *metrics, int count) {
  if (!path || !*path || !metrics || count <= 0) return;
  int used[8];
  int used_count = 0;
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"seeds\":[");
  for (int out_i = 0; out_i < 8; ++out_i) {
    int best = -1;
    for (int i = 0; i < count; ++i) {
      if (metrics[i].seed == 0) continue;
      bool seen = false;
      for (int j = 0; j < used_count; ++j) {
        if (used[j] == i || metrics[used[j]].seed == metrics[i].seed) {
          seen = true;
          break;
        }
      }
      if (seen) continue;
      if (best < 0 || metrics[i].scheduler_score > metrics[best].scheduler_score)
        best = i;
    }
    if (best < 0) break;
    used[used_count++] = best;
    if (used_count > 1) (void)sb_append_c(&b, ',');
    (void)sb_appendf(&b, "{\"seed\":%d,\"score\":%.2f,\"ops_per_s\":%.2f,"
                     "\"graph_score\":%.2f,\"tag_coverage\":%.0f,\"lane\":",
                     metrics[best].seed, metrics[best].scheduler_score,
                     metrics[best].ops_per_s, metrics[best].graph_score,
                     metrics[best].tag_coverage);
    (void)sb_append_json_str(&b, metrics[best].lane);
    if (metrics[best].direct_mode[0]) {
      (void)sb_append(&b, ",\"direct_mode\":");
      (void)sb_append_json_str(&b, metrics[best].direct_mode);
    }
    if (metrics[best].ny_family[0]) {
      (void)sb_append(&b, ",\"ny_family\":");
      (void)sb_append_json_str(&b, metrics[best].ny_family);
    }
    (void)sb_append_c(&b, '}');
  }
  (void)sb_append(&b, "],\"version\":1}\n");
  if (b.data) (void)write_file_text(path, b.data);
  free(b.data);
}

static void gc_append_top_seeds(str_buf_t *b, const gc_lane_metrics_t *metrics, int count, int max_count) {
  int used[8];
  int used_count = 0;
  if (max_count > (int)(sizeof(used) / sizeof(used[0])))
    max_count = (int)(sizeof(used) / sizeof(used[0]));
  (void)sb_append_c(b, '[');
  for (int out_i = 0; out_i < max_count; ++out_i) {
    int best = -1;
    for (int i = 0; i < count; ++i) {
      if (metrics[i].seed == 0) continue;
      bool seen = false;
      for (int j = 0; j < used_count; ++j) {
        if (used[j] == i || metrics[used[j]].seed == metrics[i].seed) {
          seen = true;
          break;
        }
      }
      if (seen) continue;
      if (best < 0 || metrics[i].scheduler_score > metrics[best].scheduler_score)
        best = i;
    }
    if (best < 0) break;
    used[used_count++] = best;
    if (used_count > 1) (void)sb_append_c(b, ',');
    (void)sb_appendf(b, "{\"seed\":%d,\"score\":%.2f,\"ops_per_s\":%.2f,"
                     "\"graph_score\":%.2f,\"tag_coverage\":%.0f,\"lane\":",
                     metrics[best].seed, metrics[best].scheduler_score,
                     metrics[best].ops_per_s, metrics[best].graph_score,
                     metrics[best].tag_coverage);
    (void)sb_append_json_str(b, metrics[best].lane);
    if (metrics[best].direct_mode[0]) {
      (void)sb_append(b, ",\"direct_mode\":");
      (void)sb_append_json_str(b, metrics[best].direct_mode);
    }
    if (metrics[best].ny_family[0]) {
      (void)sb_append(b, ",\"ny_family\":");
      (void)sb_append_json_str(b, metrics[best].ny_family);
    }
    (void)sb_append_c(b, '}');
  }
  (void)sb_append_c(b, ']');
}

static void gc_write_top_seeds_file(const char *path, const gc_lane_metrics_t *metrics, int count) {
  if (!path || !*path) return;
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"top_seeds\":");
  gc_append_top_seeds(&b, metrics, count, 8);
  (void)sb_append(&b, ",\"version\":1}\n");
  if (b.data) (void)write_file_text(path, b.data);
  free(b.data);
}

static void gc_append_bottlenecks(str_buf_t *b, const gc_lane_metrics_t *metrics, int count, int max_count) {
  int used[8];
  int used_count = 0;
  if (max_count > (int)(sizeof(used) / sizeof(used[0])))
    max_count = (int)(sizeof(used) / sizeof(used[0]));
  (void)sb_append_c(b, '[');
  for (int out_i = 0; out_i < max_count; ++out_i) {
    int best = -1;
    for (int i = 0; i < count; ++i) {
      if (metrics[i].ops_per_s <= 0.0) continue;
      bool seen = false;
      for (int j = 0; j < used_count; ++j) {
        if (used[j] == i) {
          seen = true;
          break;
        }
      }
      if (seen) continue;
      if (best < 0 || metrics[i].ops_per_s < metrics[best].ops_per_s)
        best = i;
    }
    if (best < 0) break;
    used[used_count++] = best;
    if (used_count > 1) (void)sb_append_c(b, ',');
    (void)sb_appendf(b, "{\"lane\":");
    (void)sb_append_json_str(b, metrics[best].lane);
    (void)sb_appendf(b, ",\"ops_per_s\":%.2f,\"score\":%.2f,"
                     "\"graph_score\":%.2f,\"tag_coverage\":%.0f,\"seed\":%d",
                     metrics[best].ops_per_s, metrics[best].scheduler_score,
                     metrics[best].graph_score, metrics[best].tag_coverage,
                     metrics[best].seed);
    if (metrics[best].direct_mode[0]) {
      (void)sb_append(b, ",\"direct_mode\":");
      (void)sb_append_json_str(b, metrics[best].direct_mode);
    }
    if (metrics[best].ny_family[0]) {
      (void)sb_append(b, ",\"ny_family\":");
      (void)sb_append_json_str(b, metrics[best].ny_family);
    }
    (void)sb_append_c(b, '}');
  }
  (void)sb_append_c(b, ']');
}

static double gc_lane_finding_score(const gc_lane_metrics_t *m) {
  if (!m) return 0.0;
  double score = m->scheduler_score +
                 m->graph_score * 1.5 +
                 m->tag_coverage * 250.0 +
                 m->remembered_events * 4.0 +
                 m->objects_promoted * 0.5;
  if (m->ops_per_s > 0.0) {
    double bottleneck_bonus = 150000.0 / (m->ops_per_s + 50.0);
    if (bottleneck_bonus > 2500.0) bottleneck_bonus = 2500.0;
    score += bottleneck_bonus;
  }
  return score;
}

static int gc_pick_best_metric(const gc_lane_metrics_t *metrics, int count) {
  int best = -1;
  for (int i = 0; i < count; ++i) {
    if (metrics[i].seed == 0) continue;
    if (best < 0 || gc_lane_finding_score(&metrics[i]) > gc_lane_finding_score(&metrics[best]))
      best = i;
  }
  return best;
}

static void gc_append_recommended_replays(str_buf_t *b, const gc_lane_metrics_t *metrics,
                                          int count, int max_count, int threads,
                                          bool validate_gc) {
  int used[8];
  int used_count = 0;
  if (max_count > (int)(sizeof(used) / sizeof(used[0])))
    max_count = (int)(sizeof(used) / sizeof(used[0]));
  (void)sb_append_c(b, '[');
  for (int out_i = 0; out_i < max_count; ++out_i) {
    int best = -1;
    for (int i = 0; i < count; ++i) {
      if (metrics[i].seed == 0) continue;
      bool seen = false;
      for (int j = 0; j < used_count; ++j) {
        if (used[j] == i || metrics[used[j]].seed == metrics[i].seed) {
          seen = true;
          break;
        }
      }
      if (seen) continue;
      if (best < 0 || gc_lane_finding_score(&metrics[i]) > gc_lane_finding_score(&metrics[best]))
        best = i;
    }
    if (best < 0) break;
    used[used_count++] = best;
    const char *mode = metrics[best].direct_mode[0] ? metrics[best].direct_mode : "smart";
    int replay_threads = (strcmp(mode, "remembered-churn") == 0 ||
                          strcmp(mode, "promotion-ladder") == 0) ? 1 : threads;
    if (replay_threads < 1) replay_threads = 1;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./build/nynth fuzz gc run --direct-only --duration-s 300 --threads %d "
             "--seed %d --mode %s --minor-every 31 --major-every 257 "
             "--require-promotions%s --json build/fuzz/gc/replay_%d.json",
             replay_threads, metrics[best].seed, mode,
             validate_gc ? " --validate-gc" : "",
             metrics[best].seed);
    if (used_count > 1) (void)sb_append_c(b, ',');
    (void)sb_append_json_str(b, cmd);
  }
  (void)sb_append_c(b, ']');
}

static void gc_write_lane_logs(const char *log_dir, const char *lane, char *const argv[],
                               const proc_result_t *pr, gc_lane_metrics_t *metrics) {
  if (!log_dir || !*log_dir || !lane || !*lane || !metrics) return;
  ny_ensure_dir_recursive(log_dir);
  char stem[128];
  safe_stem(stem, sizeof(stem), lane);
  char *stdout_path = NULL, *stderr_path = NULL, *command_path = NULL;
  (void)asprintf(&stdout_path, "%s/%s.stdout.log", log_dir, stem);
  (void)asprintf(&stderr_path, "%s/%s.stderr.log", log_dir, stem);
  (void)asprintf(&command_path, "%s/%s.command.txt", log_dir, stem);
  if (stdout_path) {
    (void)write_file_text(stdout_path, pr && pr->out ? pr->out : "");
    snprintf(metrics->stdout_log, sizeof(metrics->stdout_log), "%s", stdout_path);
  }
  if (stderr_path) {
    (void)write_file_text(stderr_path, pr && pr->err ? pr->err : "");
    snprintf(metrics->stderr_log, sizeof(metrics->stderr_log), "%s", stderr_path);
  }
  if (command_path) {
    str_buf_t command = {0};
    append_argv_shell_string(&command, argv);
    (void)sb_append_c(&command, '\n');
    (void)write_file_text(command_path, command.data ? command.data : "\n");
    snprintf(metrics->command_log, sizeof(metrics->command_log), "%s", command_path);
    free(command.data);
  }
  free(stdout_path);
  free(stderr_path);
  free(command_path);
}

static char *gc_write_repro_artifact(const char *root, const char *artifact_dir,
                                     const char *lane, char *const argv[],
                                     const char *report_path,
                                     const proc_result_t *pr,
                                     const gc_lane_metrics_t *metrics) {
  if (!artifact_dir || !*artifact_dir || !lane) return NULL;
  ny_ensure_dir_recursive(artifact_dir);
  char *path = NULL;
  (void)asprintf(&path, "%s/%s_%ld_%d.json", artifact_dir, lane, (long)time(NULL), (int)getpid());
  if (!path) return NULL;
  str_buf_t command = {0};
  append_argv_shell_string(&command, argv);
  str_buf_t json = {0};
  (void)sb_append(&json, "{\"kind\":\"gc-repro\",\"lane\":");
  (void)sb_append_json_str(&json, lane);
  (void)sb_append(&json, ",\"replay_command\":");
  (void)sb_append_json_str(&json, command.data ? command.data : "");
  (void)sb_append(&json, ",\"report\":");
  append_rel_json_str(&json, root, report_path ? report_path : "");
  (void)sb_appendf(&json, ",\"seed\":%d,\"scheduler_score\":%.2f,"
                   "\"ops_per_s\":%.2f,\"graph_score\":%.2f,\"tag_coverage\":%.0f,"
                   "\"rc\":%d,\"timed_out\":%s",
                   metrics ? metrics->seed : 0,
                   metrics ? metrics->scheduler_score : 0.0,
                   metrics ? metrics->ops_per_s : 0.0,
                   metrics ? metrics->graph_score : 0.0,
                   metrics ? metrics->tag_coverage : 0.0,
                   pr ? pr->rc : 0,
                   (pr && pr->timed_out) ? "true" : "false");
  if (metrics && metrics->direct_mode[0]) {
    (void)sb_append(&json, ",\"direct_mode\":");
    (void)sb_append_json_str(&json, metrics->direct_mode);
  }
  if (metrics && metrics->ny_family[0]) {
    (void)sb_append(&json, ",\"ny_family\":");
    (void)sb_append_json_str(&json, metrics->ny_family);
  }
  if (metrics && metrics->stdout_log[0]) {
    (void)sb_append(&json, ",\"stdout_log\":");
    append_rel_json_str(&json, root, metrics->stdout_log);
  }
  if (metrics && metrics->stderr_log[0]) {
    (void)sb_append(&json, ",\"stderr_log\":");
    append_rel_json_str(&json, root, metrics->stderr_log);
  }
  if (metrics && metrics->command_log[0]) {
    (void)sb_append(&json, ",\"command_log\":");
    append_rel_json_str(&json, root, metrics->command_log);
  }
  if (pr) append_proc_tail_fields(&json, pr);
  (void)sb_append(&json, ",\"engine\":\"nynth_core\"}\n");
  bool ok = json.data && write_file_text(path, json.data);
  free(command.data);
  free(json.data);
  if (!ok) {
    free(path);
    return NULL;
  }
  return path;
}

static bool gc_campaign_lane_run(const char *root, const char *lane,
                                 char *const cmd_argv[], const char *report_path,
                                 double timeout_s, const char *artifact_dir,
                                 const char *log_dir,
                                 string_list_t *rows, string_list_t *failures,
                                 gc_lane_metrics_t *metrics, int *repro_count) {
  proc_result_t pr = run_proc(cmd_argv, root, timeout_s);
  file_buf_t report = {0};
  int sub_rows = -1;
  double sub_failures = pr.rc == 0 ? 0.0 : 1.0;
  gc_lane_metrics_t local = {0};
  snprintf(local.lane, sizeof(local.lane), "%s", lane ? lane : "");
  snprintf(local.report, sizeof(local.report), "%s", report_path ? report_path : "");
  local.rc = pr.rc;
  local.elapsed_ms = pr.elapsed_ms;
  bool have_report = report_path && *report_path && read_file(report_path, &report) && report.data;
  if (have_report) {
    const char *rows_json = json_top_level_value_after_key(report.data, "rows");
    sub_rows = count_json_array_items(rows_json);
    if (!extract_json_number(report.data, "failure_count", &sub_failures))
      sub_failures = json_failures_nonempty(report.data) ? 1.0 : 0.0;
    (void)gc_extract_metrics(report.data, &local);
  }
  bool ok = pr.rc == 0 && have_report && sub_failures == 0.0;
  local.ok = ok;
  local.sub_rows = sub_rows;
  local.sub_failures = sub_failures;
  gc_write_lane_logs(log_dir, lane, cmd_argv, &pr, &local);
  char *repro = NULL;
  if (!ok)
    (void)string_list_push_take(failures,
                                make_worker_failure_row(lane, "gc-campaign-lane",
                                                        pr.rc ? pr.rc : 1,
                                                        pr.out,
                                                        have_report ? pr.err : "lane report missing"));
  if (!ok) {
    repro = gc_write_repro_artifact(root, artifact_dir, lane, cmd_argv, report_path, &pr, &local);
    if (repro && repro_count) ++*repro_count;
  }

  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, lane ? lane : "");
  (void)sb_appendf(&row,
                   ",\"kind\":\"fuzz-gc-campaign\",\"phase\":\"lane\","
                   "\"ok\":%s,\"rc\":%d,\"elapsed_ms\":%.2f,\"timed_out\":%s,"
                   "\"sub_rows\":%d,\"sub_failures\":%.0f,\"report\":",
                   ok ? "true" : "false", pr.rc, pr.elapsed_ms,
                   pr.timed_out ? "true" : "false", sub_rows, sub_failures);
  append_rel_json_str(&row, root, report_path ? report_path : "");
  if (local.stdout_log[0]) {
    (void)sb_append(&row, ",\"stdout_log\":");
    append_rel_json_str(&row, root, local.stdout_log);
  }
  if (local.stderr_log[0]) {
    (void)sb_append(&row, ",\"stderr_log\":");
    append_rel_json_str(&row, root, local.stderr_log);
  }
  if (local.command_log[0]) {
    (void)sb_append(&row, ",\"command_log\":");
    append_rel_json_str(&row, root, local.command_log);
  }
  (void)sb_appendf(&row, ",\"scheduler_score\":%.2f,\"seed\":%d,"
                   "\"objects_promoted\":%.0f,\"remembered_events\":%.0f,"
                   "\"graph_score\":%.2f,\"tag_coverage\":%.0f,\"ops_per_s\":%.2f",
                   local.scheduler_score, local.seed,
                   local.objects_promoted, local.remembered_events,
                   local.graph_score, local.tag_coverage, local.ops_per_s);
  if (local.direct_mode[0]) {
    (void)sb_append(&row, ",\"direct_mode\":");
    (void)sb_append_json_str(&row, local.direct_mode);
  }
  if (local.ny_family[0]) {
    (void)sb_append(&row, ",\"ny_family\":");
    (void)sb_append_json_str(&row, local.ny_family);
  }
  if (local.checksum != 0.0)
    (void)sb_appendf(&row, ",\"checksum\":%.0f", local.checksum);
  if (repro) {
    (void)sb_append(&row, ",\"repro\":");
    append_rel_json_str(&row, root, repro);
  }
  (void)sb_append(&row, ",\"command\":");
  append_argv_json_array(&row, cmd_argv);
  if (!ok) append_proc_tail_fields(&row, &pr);
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  free(report.data);
  if (metrics) *metrics = local;
  free(repro);
  proc_result_free(&pr);
  return ok;
}

static int cmd_public_fuzz_gc_campaign_run(int argc, char **argv, const char *profile) {
  char nynth_root[4096];
  if (!find_nynth_root(nynth_root, sizeof(nynth_root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }

  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  bool soak = profile && (strcmp(profile, "soak") == 0 ||
                          strcmp(profile, "forever") == 0 ||
                          strcmp(profile, "8h") == 0 ||
                          strcmp(profile, "insane") == 0);
  bool forever = profile && strcmp(profile, "forever") == 0;
  bool smart = profile && strcmp(profile, "smart") == 0;
  bool coverage_heavy = has_flag_after(argc, argv, 4, "--coverage-heavy") ||
                        soak ||
                        (profile && strcmp(profile, "coverage") == 0);
  bool smoke = has_flag_after(argc, argv, 4, "--smoke") ||
               has_flag_after(argc, argv, 4, "--fast") ||
               (profile && strcmp(profile, "smoke") == 0);
  bool fail_fast = has_flag_after(argc, argv, 4, "--fail-fast");
  bool direct_only = has_flag_after(argc, argv, 4, "--direct-only");
  bool ny_only = has_flag_after(argc, argv, 4, "--ny-only");
  bool skip_sanitizers = has_flag_after(argc, argv, 4, "--no-sanitizers") ||
                         has_flag_after(argc, argv, 4, "--skip-sanitizers");
  bool skip_ny = has_flag_after(argc, argv, 4, "--no-ny") ||
                 has_flag_after(argc, argv, 4, "--skip-ny") || direct_only;
  bool no_validate_gc = has_flag_after(argc, argv, 4, "--no-validate-gc");
  bool validate_gc = (has_flag_after(argc, argv, 4, "--validate-gc") || soak) && !no_validate_gc;
  int seed = atoi(value_after_equals(argc, argv, 4, "--seed", "12648430"));
  int cpu_threads = gc_online_thread_count();
  int default_threads = smoke ? 2 : 4;
  if (soak) {
    default_threads = gc_default_fuzz_thread_count();
  }
  const char *threads_arg = value_after_equals(argc, argv, 4, "--threads", "");
  int threads = gc_parse_thread_count(threads_arg, default_threads);
  int jobs = atoi(value_after_equals(argc, argv, 4, "--jobs", "1"));
  threads = gc_clamp_thread_count(threads);
  if (jobs < 1) jobs = 1;
  const char *ny_family = value_after_equals(argc, argv, 4, "--ny-family", "auto");

  const char *default_ny_cases = smoke ? "1" :
      (soak ? "6" :
      ((smart && coverage_heavy) ? "6" :
       (smart ? "1" : ((profile && strcmp(profile, "insane") == 0) ? "8" : "4"))));
  const char *default_ny_rounds = smoke ? "8" :
      (soak ? (forever ? "160" : "128") :
      (smart ? (coverage_heavy ? "48" : "32") :
       ((profile && strcmp(profile, "insane") == 0) ? "2048" : "512")));
  int ny_cases = atoi(value_after_equals(argc, argv, 4, "--ny-cases", default_ny_cases));
  int ny_rounds = atoi(value_after_equals(argc, argv, 4, "--ny-rounds", default_ny_rounds));
  if (ny_only && ny_cases <= 0) ny_cases = 1;
  if (skip_ny) ny_cases = 0;
  if (ny_rounds < 1) ny_rounds = 1;

  double duration_s = fuzz_gc_duration_s(argc, argv, smoke);
  if (soak && !fuzz_gc_has_duration_arg(argc, argv))
    duration_s = forever ? 24.0 * 3600.0 : 8.0 * 3600.0;
  if (smart && !fuzz_gc_has_duration_arg(argc, argv))
    duration_s = 12.0 * 60.0;
  if (!fuzz_gc_has_duration_arg(argc, argv) && profile &&
      !soak &&
      (strcmp(profile, "8h") == 0 || strcmp(profile, "insane") == 0))
    duration_s = 8.0 * 3600.0;
  double checkpoint_s = atof(value_after_equals(argc, argv, 4, "--checkpoint-s", "0"));
  if (checkpoint_s <= 0.0 && soak && duration_s >= 3600.0)
    checkpoint_s = duration_s >= 14400.0 ? 3600.0 : 1800.0;

  char *work_dir = NULL;
  (void)asprintf(&work_dir, "%s/build/fuzz/gc/campaign_%ld_%d",
                 nynth_root, (long)time(NULL), (int)getpid());
  if (!work_dir || !mkdir_p(work_dir)) {
    printf("{\"ok\":false,\"error\":\"campaign-workdir-failed\"}\n");
    free(work_dir);
    return 2;
  }
  char *default_artifact_dir = NULL;
  (void)asprintf(&default_artifact_dir, "%s/repro", work_dir);
  const char *artifact_arg = value_after_equals(argc, argv, 4, "--artifact-dir", "");
  char *artifact_dir = NULL;
  if (artifact_arg && *artifact_arg) (void)asprintf(&artifact_dir, "%s", artifact_arg);
  else if (default_artifact_dir) (void)asprintf(&artifact_dir, "%s", default_artifact_dir);
  if (artifact_dir) ny_ensure_dir_recursive(artifact_dir);
  const char *seed_cache = value_after_equals(argc, argv, 4, "--seed-cache", "build/fuzz/gc/seeds.json");
  char *log_dir = NULL, *seed_dir = NULL, *seed_before_path = NULL, *seed_after_path = NULL;
  char *top_seeds_path = NULL, *command_path = NULL, *manifest_path = NULL, *forever_script_path = NULL;
  (void)asprintf(&log_dir, "%s/logs", work_dir);
  (void)asprintf(&seed_dir, "%s/seeds", work_dir);
  (void)asprintf(&seed_before_path, "%s/seed-cache.before.json", seed_dir ? seed_dir : work_dir);
  (void)asprintf(&seed_after_path, "%s/seed-cache.after.json", seed_dir ? seed_dir : work_dir);
  (void)asprintf(&top_seeds_path, "%s/top-seeds.json", seed_dir ? seed_dir : work_dir);
  (void)asprintf(&command_path, "%s/command.txt", work_dir);
  (void)asprintf(&manifest_path, "%s/manifest.json", work_dir);
  (void)asprintf(&forever_script_path, "%s/run-forever.sh", work_dir);
  if (log_dir) ny_ensure_dir_recursive(log_dir);
  if (seed_dir) ny_ensure_dir_recursive(seed_dir);
  (void)gc_write_seed_snapshot(seed_cache, seed_before_path);
  if (command_path) {
    str_buf_t command = {0};
    append_argv_shell_string(&command, argv);
    (void)sb_append_c(&command, '\n');
    (void)write_file_text(command_path, command.data ? command.data : "\n");
    free(command.data);
  }

  string_list_t rows = {0}, failures = {0};
  bool continue_running = true;
  gc_lane_metrics_t lane_metrics[256];
  int lane_metric_count = 0;
  int adaptive_rounds = 0;
  int repro_count = 0;
  double adaptive_slice_s = 0.0;
  int preflight_lanes = 0;
  int sanitizer_lanes = 0;
  int sanitizer_checkpoints = 0;
  int ny_family_lanes = 0;
  int fresh_seed_lanes = 0;
  int mutated_seed_lanes = 0;
  int confirmation_lanes = 0;

#define RUN_GC_LANE(lane_name, timeout_value, ...) do {                                      \
    if (continue_running) {                                                                  \
      char *lane_report = NULL;                                                              \
      (void)asprintf(&lane_report, "%s/%s.json", work_dir, (lane_name));                    \
      char *lane_argv[] = {g_self_path, "fuzz", "gc", "run", __VA_ARGS__,                  \
                           "--json", lane_report, NULL};                                    \
      gc_lane_metrics_t one_metric;                                                          \
      memset(&one_metric, 0, sizeof(one_metric));                                             \
      bool lane_ok = gc_campaign_lane_run(nynth_root, (lane_name), lane_argv, lane_report,    \
                                          (timeout_value), artifact_dir, log_dir,              \
                                          &rows, &failures, &one_metric, &repro_count);       \
      if (lane_metric_count < (int)(sizeof(lane_metrics) / sizeof(lane_metrics[0])))          \
        lane_metrics[lane_metric_count++] = one_metric;                                      \
      if (!lane_ok && fail_fast) continue_running = false;                                    \
      free(lane_report);                                                                     \
    }                                                                                        \
  } while (0)

#define RUN_GC_LANE_COUNT(counter_name, lane_name, timeout_value, ...) do {                  \
    int before_count = lane_metric_count;                                                     \
    RUN_GC_LANE((lane_name), (timeout_value), __VA_ARGS__);                                   \
    if (lane_metric_count > before_count) ++(counter_name);                                   \
  } while (0)

  if (soak) {
    char threads_buf[32], seed_buf[64], lane_name[80], duration_buf[64];
    char *validate_arg = validate_gc ? "--validate-gc" : "--no-validate-gc";
    snprintf(threads_buf, sizeof(threads_buf), "%d", threads);
    int cached_seeds[8] = {0};
    int cached_count = gc_seed_cache_load(seed_cache, cached_seeds, 8);
    int cached_limit = duration_s < 60.0 ? 2 : (duration_s < 600.0 ? 3 : 8);
    if (cached_count > cached_limit) cached_count = cached_limit;
    for (int i = 0; !ny_only && i < cached_count && continue_running; ++i) {
      snprintf(lane_name, sizeof(lane_name), "soak_cache_seed_%02d", i);
      snprintf(seed_buf, sizeof(seed_buf), "%d", cached_seeds[i]);
      RUN_GC_LANE_COUNT(preflight_lanes, lane_name, 420.0,
                        "--smoke", "--direct-only", "--iterations", "3072",
                        "--threads", (i % 3 == 0) ? threads_buf : "1",
                        "--seed", seed_buf, "--mode", "smart",
                        "--minor-every", "31", "--major-every", "997",
                        "--require-promotions", validate_arg);
    }

    if (!ny_only) {
      static const char *probe_modes[] = {
        "smart", "remembered-churn", "mixed-runtime", "string-heavy",
        "result-nest", "cycle-graph", "promotion-ladder", "dict-heavy",
        "closure-result", "wide-graph", "deep-graph", "minor-storm"
      };
      static const char *probe_minor[] = {"31", "17", "37", "41", "17", "43", "0", "29", "0", "47", "23", "11"};
      static const char *probe_major[] = {"997", "0", "389", "401", "223", "431", "0", "257", "0", "389", "211", "0"};
      int probe_count = (int)(sizeof(probe_modes) / sizeof(probe_modes[0]));
      if (duration_s < 60.0) probe_count = 5;
      else if (duration_s < 600.0) probe_count = 8;
      for (int i = 0; i < probe_count && continue_running; ++i) {
        snprintf(lane_name, sizeof(lane_name), "soak_probe_%s", probe_modes[i]);
        for (char *p = lane_name; *p; ++p)
          if (*p == '-') *p = '_';
        snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 17 + i * 101);
        const char *lane_threads = (strcmp(probe_modes[i], "remembered-churn") == 0 ||
                                    strcmp(probe_modes[i], "promotion-ladder") == 0)
                                       ? "1"
                                       : threads_buf;
        RUN_GC_LANE_COUNT(preflight_lanes, lane_name, 420.0,
                          "--smoke", "--direct-only", "--iterations",
                          (strcmp(probe_modes[i], "promotion-ladder") == 0) ? "1024" : "3072",
                          "--threads", (char *)lane_threads,
                          "--seed", seed_buf, "--mode", (char *)probe_modes[i],
                          "--minor-every", (char *)probe_minor[i],
                          "--major-every", (char *)probe_major[i],
                          "--require-promotions", validate_arg);
      }
    }

    if (!ny_only && !skip_sanitizers) {
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 31337);
      RUN_GC_LANE_COUNT(sanitizer_lanes, "soak_asan_early", 720.0,
                        "--smoke", "--direct-only", "--sanitize", "--iterations", "1024",
                        "--threads", "1", "--seed", seed_buf, "--mode", "result-nest",
                        "--minor-every", "29", "--major-every", "257",
                        "--require-promotions", validate_arg);
      int tsan_threads = threads < 2 ? 2 : threads;
      if (tsan_threads > 8) tsan_threads = 8;
      char tsan_threads_buf[32];
      snprintf(tsan_threads_buf, sizeof(tsan_threads_buf), "%d", tsan_threads);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 424242);
      RUN_GC_LANE_COUNT(sanitizer_lanes, "soak_tsan_early", 900.0,
                        "--smoke", "--direct-only", "--tsan", "--iterations", "1024",
                        "--threads", tsan_threads_buf, "--seed", seed_buf, "--mode", "mixed-runtime",
                        "--minor-every", "53", "--major-every", "0",
                        "--require-promotions", validate_arg);
    }

    if (!skip_ny && ny_cases > 0) {
      static const char *families[] = {"lists", "dicts", "results", "closures", "strings", "mixed"};
      char ny_rounds_buf[32], ny_seed[64];
      snprintf(ny_rounds_buf, sizeof(ny_rounds_buf), "%d", ny_rounds);
      int family_count = ny_cases;
      int max_family_count = (int)(sizeof(families) / sizeof(families[0]));
      if (family_count > max_family_count) family_count = max_family_count;
      if (duration_s < 60.0 && family_count > 2) family_count = 2;
      else if (duration_s < 600.0 && family_count > 4) family_count = 4;
      for (int f = 0; f < family_count && continue_running; ++f) {
        snprintf(lane_name, sizeof(lane_name), "soak_ny_%s", families[f]);
        snprintf(ny_seed, sizeof(ny_seed), "%d", seed + 900001 + f * 104729);
        RUN_GC_LANE_COUNT(ny_family_lanes, lane_name, 600.0,
                          "--smoke", "--ny-only", "--ny-cases", "1",
                          "--ny-rounds", ny_rounds_buf, "--ny-family", (char *)families[f],
                          "--seed", ny_seed, validate_arg);
      }
    }

    int max_adaptive = 4;
    if (duration_s >= 86400.0) max_adaptive = 160;
    else if (duration_s >= 28800.0) max_adaptive = 96;
    else if (duration_s >= 14400.0) max_adaptive = 72;
    else if (duration_s >= 7200.0) max_adaptive = 48;
    else if (duration_s >= 1800.0) max_adaptive = 24;
    else if (duration_s >= 600.0) max_adaptive = 12;
    adaptive_slice_s = duration_s < 120.0 ? 2.0 : (duration_s * 0.78) / (double)max_adaptive;
    if (duration_s >= 600.0 && adaptive_slice_s < 15.0) adaptive_slice_s = 15.0;
    if (adaptive_slice_s > 900.0) adaptive_slice_s = 900.0;

    static const char *adaptive_modes[] = {
      "mixed-runtime", "remembered-churn", "promotion-ladder", "dict-heavy",
      "result-nest", "cycle-graph", "string-heavy", "wide-graph",
      "closure-result", "deep-graph", "minor-storm", "major-storm", "smart"
    };
    static const char *adaptive_minor[] = {"47", "19", "0", "31", "17", "41", "29", "43", "37", "23", "11", "0", "53"};
    static const char *adaptive_major[] = {"509", "157", "0", "257", "223", "431", "277", "389", "0", "211", "0", "13", "997"};
    const int adaptive_mode_count = (int)(sizeof(adaptive_modes) / sizeof(adaptive_modes[0]));
    int sanitizer_target = 0;
    if (!skip_sanitizers && checkpoint_s > 0.0) {
      sanitizer_target = (int)(duration_s / checkpoint_s);
      if (sanitizer_target > 16) sanitizer_target = 16;
      if (sanitizer_target < 0) sanitizer_target = 0;
    }
    int sanitizer_every = sanitizer_target > 0 ? max_adaptive / (sanitizer_target + 1) : 0;
    if (sanitizer_target > 0 && sanitizer_every < 1) sanitizer_every = 1;

    for (int r = 0; !ny_only && continue_running && r < max_adaptive; ++r) {
      int best = gc_pick_best_metric(lane_metrics, lane_metric_count);
      bool fresh = (best < 0) || (r % 5 == 0);
      int adapt_seed = fresh ? seed + 700001 + r * 7919
                             : lane_metrics[best].seed + 1009 * (r + 1);
      const char *mode = adaptive_modes[(r * 7 + (fresh ? 3 : 0)) % adaptive_mode_count];
      const char *lane_threads = (strcmp(mode, "remembered-churn") == 0 ||
                                  strcmp(mode, "promotion-ladder") == 0)
                                     ? "1"
                                     : threads_buf;
      snprintf(lane_name, sizeof(lane_name), "soak_%s_%02d", fresh ? "fresh" : "mutate", r);
      snprintf(seed_buf, sizeof(seed_buf), "%d", adapt_seed);
      snprintf(duration_buf, sizeof(duration_buf), "%.3f", adaptive_slice_s);
      if (fresh) {
        RUN_GC_LANE_COUNT(fresh_seed_lanes, lane_name, adaptive_slice_s + 300.0,
                          "--direct-only", "--duration-s", duration_buf,
                          "--threads", (char *)lane_threads,
                          "--seed", seed_buf, "--mode", (char *)mode,
                          "--minor-every", (char *)adaptive_minor[r % adaptive_mode_count],
                          "--major-every", (char *)adaptive_major[r % adaptive_mode_count],
                          "--require-promotions", validate_arg);
      } else {
        RUN_GC_LANE_COUNT(mutated_seed_lanes, lane_name, adaptive_slice_s + 300.0,
                          "--direct-only", "--duration-s", duration_buf,
                          "--threads", (char *)lane_threads,
                          "--seed", seed_buf, "--mode", (char *)mode,
                          "--minor-every", (char *)adaptive_minor[r % adaptive_mode_count],
                          "--major-every", (char *)adaptive_major[r % adaptive_mode_count],
                          "--require-promotions", validate_arg);
      }
      ++adaptive_rounds;

      if (!skip_sanitizers && sanitizer_every > 0 && (r + 1) % sanitizer_every == 0 &&
          sanitizer_checkpoints < sanitizer_target && continue_running) {
        int checkpoint_seed = adapt_seed + 600000 + sanitizer_checkpoints * 4099;
        snprintf(seed_buf, sizeof(seed_buf), "%d", checkpoint_seed);
        snprintf(lane_name, sizeof(lane_name), "soak_san_%02d", sanitizer_checkpoints);
        if ((sanitizer_checkpoints & 1) == 0) {
          RUN_GC_LANE_COUNT(sanitizer_lanes, lane_name, 720.0,
                            "--smoke", "--direct-only", "--sanitize", "--iterations", "768",
                            "--threads", "1", "--seed", seed_buf, "--mode", "result-nest",
                            "--minor-every", "29", "--major-every", "257",
                            "--require-promotions", validate_arg);
        } else {
          int tsan_threads = threads < 2 ? 2 : threads;
          if (tsan_threads > 8) tsan_threads = 8;
          char tsan_threads_buf[32];
          snprintf(tsan_threads_buf, sizeof(tsan_threads_buf), "%d", tsan_threads);
          RUN_GC_LANE_COUNT(sanitizer_lanes, lane_name, 900.0,
                            "--smoke", "--direct-only", "--tsan", "--iterations", "768",
                            "--threads", tsan_threads_buf, "--seed", seed_buf, "--mode", "mixed-runtime",
                            "--minor-every", "53", "--major-every", "0",
                            "--require-promotions", validate_arg);
        }
        ++sanitizer_checkpoints;
      }
    }

    int confirm_count = duration_s < 60.0 ? 1 : 3;
    for (int c = 0; !ny_only && continue_running && c < confirm_count && lane_metric_count > 0; ++c) {
      int best = gc_pick_best_metric(lane_metrics, lane_metric_count);
      int confirm_seed = best >= 0 ? lane_metrics[best].seed + 313 * (c + 1) : seed + 990001 + c * 313;
      const char *mode = best >= 0 && lane_metrics[best].direct_mode[0]
                             ? lane_metrics[best].direct_mode
                             : adaptive_modes[(c * 3) % adaptive_mode_count];
      const char *lane_threads = (strcmp(mode, "remembered-churn") == 0 ||
                                  strcmp(mode, "promotion-ladder") == 0)
                                     ? "1"
                                     : threads_buf;
      snprintf(lane_name, sizeof(lane_name), "soak_confirm_%02d", c);
      snprintf(seed_buf, sizeof(seed_buf), "%d", confirm_seed);
      RUN_GC_LANE_COUNT(confirmation_lanes, lane_name, 420.0,
                        "--smoke", "--direct-only", "--iterations", "2048",
                        "--threads", (char *)lane_threads,
                        "--seed", seed_buf, "--mode", (char *)mode,
                        "--minor-every", "31", "--major-every", "257",
                        "--require-promotions", validate_arg);
    }
  } else if (smart) {
    char threads_buf[32], seed_buf[64], lane_name[80], duration_buf[64];
    char *validate_arg = validate_gc ? "--validate-gc" : "--no-validate-gc";
    snprintf(threads_buf, sizeof(threads_buf), "%d", threads);
    int cached_seeds[3] = {0};
    int cached_count = gc_seed_cache_load(seed_cache, cached_seeds, 3);
    for (int i = 0; !ny_only && i < cached_count && continue_running; ++i) {
      snprintf(lane_name, sizeof(lane_name), "cache_seed_%02d", i);
      snprintf(seed_buf, sizeof(seed_buf), "%d", cached_seeds[i]);
      RUN_GC_LANE(lane_name, 420.0,
                  "--smoke", "--direct-only", "--iterations", "2048",
                  "--threads", "1", "--seed", seed_buf, "--mode", "smart",
                  "--minor-every", "31", "--major-every", "997",
                  "--require-promotions", validate_arg);
    }

    if (!ny_only) {
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
      RUN_GC_LANE("smart_probe", 420.0,
                  "--smoke", "--direct-only", "--iterations", "2048",
                  "--threads", "1", "--seed", seed_buf, "--mode", "smart",
                  "--minor-every", "31", "--major-every", "997",
                  "--require-promotions", validate_arg);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 17);
      RUN_GC_LANE("smart_remembered", 420.0,
                  "--smoke", "--direct-only", "--iterations", "2048",
                  "--threads", "1", "--seed", seed_buf, "--mode", "remembered-churn",
                  "--minor-every", "17", "--major-every", "0",
                  "--require-promotions", validate_arg);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 23);
      RUN_GC_LANE("smart_threaded", 420.0,
                  "--smoke", "--direct-only", "--iterations", "4096",
                  "--threads", threads_buf, "--seed", seed_buf, "--mode", "smart",
                  "--minor-every", "47", "--major-every", "211",
                  "--require-promotions", validate_arg);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 31);
      RUN_GC_LANE("smart_string", 420.0,
                  "--smoke", "--direct-only", "--iterations", "1536",
                  "--threads", "1", "--seed", seed_buf, "--mode", "string-heavy",
                  "--minor-every", "13", "--major-every", "197",
                  "--require-promotions", validate_arg);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 43);
      RUN_GC_LANE("smart_result", 420.0,
                  "--smoke", "--direct-only", "--iterations", "1536",
                  "--threads", "1", "--seed", seed_buf, "--mode", "result-nest",
                  "--minor-every", "17", "--major-every", "223",
                  "--require-promotions", validate_arg);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 59);
      RUN_GC_LANE("smart_cycle", 420.0,
                  "--smoke", "--direct-only", "--iterations", "1536",
                  "--threads", "1", "--seed", seed_buf, "--mode", "cycle-graph",
                  "--minor-every", "19", "--major-every", "251",
                  "--require-promotions", validate_arg);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 71);
      RUN_GC_LANE("smart_promotion", 420.0,
                  "--smoke", "--direct-only", "--iterations", "768",
                  "--threads", "1", "--seed", seed_buf, "--mode", "promotion-ladder",
                  "--minor-every", "0", "--major-every", "0",
                  "--require-promotions", validate_arg);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 83);
      RUN_GC_LANE("smart_perf_mixed", 420.0,
                  "--smoke", "--direct-only", "--iterations", "3072",
                  "--threads", threads_buf, "--seed", seed_buf, "--mode", "mixed-runtime",
                  "--minor-every", "37", "--major-every", "389",
                  "--require-promotions", validate_arg);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 97);
      RUN_GC_LANE("smart_perf_string", 420.0,
                  "--smoke", "--direct-only", "--iterations", "3072",
                  "--threads", "1", "--seed", seed_buf, "--mode", "string-heavy",
                  "--minor-every", "41", "--major-every", "401",
                  "--require-promotions", validate_arg);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 109);
      RUN_GC_LANE("smart_perf_cycle", 420.0,
                  "--smoke", "--direct-only", "--iterations", "2048",
                  "--threads", "1", "--seed", seed_buf, "--mode", "cycle-graph",
                  "--minor-every", "43", "--major-every", "431",
                  "--require-promotions", validate_arg);
    }

    if (!ny_only && !skip_sanitizers) {
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 31337);
      RUN_GC_LANE("smart_asan", 600.0,
                  "--smoke", "--direct-only", "--sanitize", "--iterations", "768",
                  "--threads", "1", "--seed", seed_buf, "--mode", "result-nest",
                  "--minor-every", "29", "--major-every", "257",
                  "--require-promotions", validate_arg);
      int tsan_threads = threads < 2 ? 2 : threads;
      if (tsan_threads > 4) tsan_threads = 4;
      char tsan_threads_buf[32];
      snprintf(tsan_threads_buf, sizeof(tsan_threads_buf), "%d", tsan_threads);
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 424242);
      RUN_GC_LANE("smart_tsan", 720.0,
                  "--smoke", "--direct-only", "--tsan", "--iterations", "768",
                  "--threads", tsan_threads_buf, "--seed", seed_buf, "--mode", "mixed-runtime",
                  "--minor-every", "53", "--major-every", "0",
                  "--require-promotions", validate_arg);
    }

    if (!skip_ny && ny_cases > 0) {
      char ny_cases_buf[32], ny_rounds_buf[32], ny_seed[64];
      snprintf(ny_cases_buf, sizeof(ny_cases_buf), "%d", ny_cases);
      snprintf(ny_rounds_buf, sizeof(ny_rounds_buf), "%d", ny_rounds);
      if (coverage_heavy) {
        static const char *families[] = {"lists", "dicts", "results", "closures", "strings", "mixed"};
        int family_count = ny_cases;
        int max_family_count = (int)(sizeof(families) / sizeof(families[0]));
        if (family_count > max_family_count) family_count = max_family_count;
        for (int f = 0; f < family_count && continue_running; ++f) {
          snprintf(lane_name, sizeof(lane_name), "smart_ny_%s", families[f]);
          snprintf(ny_seed, sizeof(ny_seed), "%d", seed + 900001 + f * 104729);
          RUN_GC_LANE(lane_name, 420.0,
                      "--smoke", "--ny-only", "--ny-cases", "1",
                      "--ny-rounds", ny_rounds_buf, "--ny-family", (char *)families[f],
                      "--seed", ny_seed, validate_arg);
        }
      } else {
        snprintf(ny_seed, sizeof(ny_seed), "%d", seed + 900001);
        RUN_GC_LANE("smart_ny_exec", 420.0,
                    "--smoke", "--ny-only", "--ny-cases", ny_cases_buf,
                    "--ny-rounds", ny_rounds_buf, "--ny-family", (char *)ny_family,
                    "--seed", ny_seed, validate_arg);
      }
    }

    int max_adaptive = 1;
    if (duration_s >= 14400.0) max_adaptive = coverage_heavy ? 36 : 24;
    else if (duration_s >= 7200.0) max_adaptive = coverage_heavy ? 18 : 12;
    else if (duration_s >= 1800.0) max_adaptive = coverage_heavy ? 10 : 8;
    else if (duration_s >= 600.0) max_adaptive = coverage_heavy ? 6 : 4;
    else if (duration_s >= 90.0) max_adaptive = 2;
    adaptive_slice_s = duration_s < 120.0 ? 2.0 : (duration_s * 0.70) / (double)max_adaptive;
    if (duration_s >= 600.0 && adaptive_slice_s < 8.0) adaptive_slice_s = 8.0;
    if (adaptive_slice_s > 1200.0) adaptive_slice_s = 1200.0;
    for (int r = 0; !ny_only && continue_running && r < max_adaptive; ++r) {
      int best = -1;
      for (int i = 0; i < lane_metric_count; ++i) {
        if (lane_metrics[i].seed == 0) continue;
        if (best < 0 || lane_metrics[i].scheduler_score > lane_metrics[best].scheduler_score)
          best = i;
      }
      int adapt_seed = best >= 0 ? lane_metrics[best].seed + 1009 * (r + 1) : seed + 700001 + r * 7919;
      static char *adaptive_modes[] = {
        "remembered-churn", "deep-graph", "dict-heavy",
        "closure-result", "wide-graph", "string-heavy",
        "result-nest", "cycle-graph", "promotion-ladder",
        "mixed-runtime", "minor-storm"
      };
      static char *adaptive_minor[] = {"19", "23", "31", "37", "43", "29", "17", "41", "0", "47", "11"};
      static char *adaptive_major[] = {"157", "211", "257", "0", "389", "277", "223", "431", "0", "509", "0"};
      const int adaptive_mode_count = (int)(sizeof(adaptive_modes) / sizeof(adaptive_modes[0]));
      char *mode = adaptive_modes[r % adaptive_mode_count];
      snprintf(lane_name, sizeof(lane_name), "smart_adapt_%02d", r);
      snprintf(seed_buf, sizeof(seed_buf), "%d", adapt_seed);
      snprintf(duration_buf, sizeof(duration_buf), "%.3f", adaptive_slice_s);
      RUN_GC_LANE(lane_name, adaptive_slice_s + 300.0,
                  "--direct-only", "--duration-s", duration_buf,
                  "--threads", (strcmp(mode, "remembered-churn") == 0 ? "1" : threads_buf),
                  "--seed", seed_buf, "--mode", mode,
                  "--minor-every", adaptive_minor[r % adaptive_mode_count],
                  "--major-every", adaptive_major[r % adaptive_mode_count],
                  "--require-promotions", validate_arg);
      ++adaptive_rounds;
    }
  } else if (!ny_only) {
    char seed_buf[64], threads_buf[32], iter_buf[64], duration_buf[64];
    char *validate_arg = validate_gc ? "--validate-gc" : "--no-validate-gc";
    snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
    snprintf(threads_buf, sizeof(threads_buf), "%d", threads);
    snprintf(iter_buf, sizeof(iter_buf), "%s", smoke ? "4096" : "8192");
    RUN_GC_LANE("direct_probe", smoke ? 420.0 : 600.0,
                "--smoke", "--direct-only", "--iterations", iter_buf,
                "--threads", "1", "--seed", seed_buf, "--mode", "smart",
                "--require-promotions", validate_arg);

    snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 1);
    if (smoke) {
      RUN_GC_LANE("direct_threaded", 420.0,
                  "--smoke", "--direct-only", "--iterations", "4096",
                  "--threads", threads_buf, "--seed", seed_buf, "--mode", "smart",
                  "--require-promotions", validate_arg);
    } else {
      double main_s = duration_s * 0.65;
      if (main_s < 1.0) main_s = 1.0;
      snprintf(duration_buf, sizeof(duration_buf), "%.3f", main_s);
      RUN_GC_LANE("direct_threaded", main_s + 300.0,
                  "--direct-only", "--duration-s", duration_buf,
                  "--threads", threads_buf, "--seed", seed_buf, "--mode", "smart",
                  "--require-promotions", validate_arg);
    }

    int sweep_lanes = smoke ? 1 : ((profile && strcmp(profile, "insane") == 0) ? 6 : 3);
    for (int i = 0; i < sweep_lanes && continue_running; ++i) {
      char lane_name[64], sweep_seed[64], sweep_duration[64];
      snprintf(lane_name, sizeof(lane_name), "direct_seed_%02d", i);
      snprintf(sweep_seed, sizeof(sweep_seed), "%d", seed + 7919 * (i + 2));
      if (smoke) {
        RUN_GC_LANE(lane_name, 420.0,
                    "--smoke", "--direct-only", "--iterations", "3072",
                    "--threads", "1", "--seed", sweep_seed, "--mode", "smart",
                    "--require-promotions", validate_arg);
      } else {
        double slice_s = (duration_s * 0.35) / (double)(sweep_lanes > 0 ? sweep_lanes : 1);
        if (slice_s < 1.0) slice_s = 1.0;
        snprintf(sweep_duration, sizeof(sweep_duration), "%.3f", slice_s);
        RUN_GC_LANE(lane_name, slice_s + 240.0,
                    "--direct-only", "--duration-s", sweep_duration,
                    "--threads", threads_buf, "--seed", sweep_seed, "--mode", "smart",
                    "--require-promotions", validate_arg);
      }
    }

    if (!skip_sanitizers) {
      snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 31337);
      RUN_GC_LANE("direct_asan", smoke ? 420.0 : 900.0,
                  "--smoke", "--direct-only", "--sanitize", "--iterations",
                  smoke ? "512" : "2048", "--threads", "1", "--seed", seed_buf,
                  "--mode", "deep-graph", "--require-promotions", validate_arg);
      int tsan_threads = threads < 2 ? 2 : threads;
      if (tsan_threads > 8) tsan_threads = 8;
      char tsan_threads_buf[32], tsan_seed[64];
      snprintf(tsan_threads_buf, sizeof(tsan_threads_buf), "%d", tsan_threads);
      snprintf(tsan_seed, sizeof(tsan_seed), "%d", seed + 424242);
      RUN_GC_LANE("direct_tsan", smoke ? 600.0 : 1200.0,
                  "--smoke", "--direct-only", "--tsan", "--iterations",
                  smoke ? "512" : "2048", "--threads", tsan_threads_buf,
                  "--seed", tsan_seed, "--mode", "smart", "--require-promotions", validate_arg);
    }
  }

  if (!smart && !soak && !skip_ny && ny_cases > 0) {
    char ny_cases_buf[32], ny_rounds_buf[32], ny_seed[64];
    char *validate_arg = validate_gc ? "--validate-gc" : "--no-validate-gc";
    snprintf(ny_cases_buf, sizeof(ny_cases_buf), "%d", ny_cases);
    snprintf(ny_rounds_buf, sizeof(ny_rounds_buf), "%d", ny_rounds);
    snprintf(ny_seed, sizeof(ny_seed), "%d", seed + 900001);
    RUN_GC_LANE("ny_exec", smoke ? 360.0 : 900.0,
                "--smoke", "--ny-only", "--ny-cases", ny_cases_buf,
                "--ny-rounds", ny_rounds_buf, "--ny-family", (char *)ny_family,
                "--seed", ny_seed, validate_arg);
  }

#undef RUN_GC_LANE_COUNT
#undef RUN_GC_LANE

  gc_seed_cache_write(seed_cache, lane_metrics, lane_metric_count);
  (void)gc_write_seed_snapshot(seed_cache, seed_after_path);
  gc_write_top_seeds_file(top_seeds_path, lane_metrics, lane_metric_count);

  static const char *summary_modes[] = {
    "smart", "remembered-churn", "deep-graph", "wide-graph", "dict-heavy",
    "closure-result", "string-heavy", "result-nest", "cycle-graph",
    "promotion-ladder", "mixed-runtime", "minor-storm", "major-storm"
  };
  static const char *summary_families[] = {"lists", "dicts", "results", "closures", "strings", "mixed"};
  int unique_direct_modes = 0;
  int unique_ny_families = 0;
  int max_tag_coverage = 0;
  for (int m = 0; m < (int)(sizeof(summary_modes) / sizeof(summary_modes[0])); ++m) {
    for (int i = 0; i < lane_metric_count; ++i) {
      if (strcmp(lane_metrics[i].direct_mode, summary_modes[m]) == 0) {
        ++unique_direct_modes;
        break;
      }
    }
  }
  for (int f = 0; f < (int)(sizeof(summary_families) / sizeof(summary_families[0])); ++f) {
    for (int i = 0; i < lane_metric_count; ++i) {
      if (strcmp(lane_metrics[i].ny_family, summary_families[f]) == 0) {
        ++unique_ny_families;
        break;
      }
    }
  }
  for (int i = 0; i < lane_metric_count; ++i) {
    if ((int)lane_metrics[i].tag_coverage > max_tag_coverage)
      max_tag_coverage = (int)lane_metrics[i].tag_coverage;
  }

  if (forever_script_path) {
    str_buf_t script = {0};
    (void)sb_append(&script, "#!/usr/bin/env bash\n");
    (void)sb_append(&script, "set -euo pipefail\n");
    append_repo_cache_env_script(&script, nynth_root);
    (void)sb_appendf(&script,
                     "mkdir -p build/fuzz/gc\n"
                     "while true; do\n"
                     "  ts=$(date +%%Y%%m%%d-%%H%%M%%S)\n"
                     "  ./build/nynth fuzz gc run --profile=soak --hours 8 "
                     "--threads %d --checkpoint-s=3600 --fail-fast --validate-gc "
                     "--json \"build/fuzz/gc/soak-${ts}.json\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"GC soak stopped with rc=$rc; inspect build/fuzz/gc/soak-${ts}.json\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  sleep 5\n"
                     "done\n",
                     threads);
    if (script.data && write_file_text(forever_script_path, script.data))
      (void)chmod(forever_script_path, 0755);
    free(script.data);
  }

  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"profile\":");
  (void)sb_append_json_str(&extra, profile ? profile : "");
  (void)sb_append(&extra, ",\"profile_kind\":");
  (void)sb_append_json_str(&extra, soak ? (forever ? "forever" : "soak") : (smart ? "smart" : (profile ? profile : "")));
  (void)sb_appendf(&extra,
                   ",\"seed\":%d,\"duration_s\":%.3f,\"budget_s\":%.3f,\"threads\":%d,\"jobs\":%d,"
                   "\"ny_cases\":%d,\"ny_rounds\":%d,\"fail_fast\":%s,"
                   "\"skip_sanitizers\":%s,\"skip_ny\":%s,\"validate_gc\":%s,"
                   "\"coverage_heavy\":%s,\"checkpoint_s\":%.3f,"
                   "\"adaptive_rounds\":%d,\"adaptive_slice_s\":%.3f,"
                   "\"sanitizer_checkpoints\":%d,\"fresh_seed_lanes\":%d,"
                   "\"mutated_seed_lanes\":%d,\"repro_count\":%d,\"lane_count\":%d,"
                   "\"top_seeds\":",
                   seed, duration_s, duration_s, threads, jobs, ny_cases, ny_rounds,
                   fail_fast ? "true" : "false",
                   skip_sanitizers ? "true" : "false", skip_ny ? "true" : "false",
                   validate_gc ? "true" : "false",
                   coverage_heavy ? "true" : "false", checkpoint_s,
                   adaptive_rounds, adaptive_slice_s,
                   sanitizer_checkpoints, fresh_seed_lanes,
                   mutated_seed_lanes, repro_count, lane_metric_count);
  gc_append_top_seeds(&extra, lane_metrics, lane_metric_count, 5);
  (void)sb_appendf(&extra, ",\"cpu_threads\":%d", cpu_threads);
  (void)sb_append(&extra, ",\"thread_request\":");
  (void)sb_append_json_str(&extra, threads_arg && *threads_arg ? threads_arg : "auto");
  (void)sb_append(&extra, ",\"json_report\":");
  (void)sb_append_json_str(&extra, json_path ? json_path : "");
  (void)sb_append(&extra, ",\"ny_family\":");
  (void)sb_append_json_str(&extra, ny_family ? ny_family : "auto");
  (void)sb_appendf(&extra,
                   ",\"phase_counts\":{\"preflight\":%d,\"sanitizer\":%d,"
                   "\"ny_family\":%d,\"adaptive\":%d,\"confirmation\":%d}",
                   preflight_lanes, sanitizer_lanes, ny_family_lanes,
                   adaptive_rounds, confirmation_lanes);
  (void)sb_appendf(&extra,
                   ",\"coverage_targets_hit\":{\"direct_modes\":%d,"
                   "\"max_tag_coverage\":%d,\"ny_families\":%d}",
                   unique_direct_modes, max_tag_coverage, unique_ny_families);
  (void)sb_append(&extra, ",\"bottlenecks\":");
  gc_append_bottlenecks(&extra, lane_metrics, lane_metric_count, 5);
  (void)sb_append(&extra, ",\"recommended_replays\":");
  gc_append_recommended_replays(&extra, lane_metrics, lane_metric_count, 5, threads, validate_gc);
  (void)sb_append(&extra, ",\"artifact_dir\":");
  append_rel_json_str(&extra, nynth_root, artifact_dir ? artifact_dir : "");
  (void)sb_append(&extra, ",\"seed_cache\":");
  append_rel_json_str(&extra, nynth_root, seed_cache ? seed_cache : "");
  (void)sb_append(&extra, ",\"work_dir\":");
  append_rel_json_str(&extra, nynth_root, work_dir ? work_dir : "");
  (void)sb_append(&extra, ",\"log_dir\":");
  append_rel_json_str(&extra, nynth_root, log_dir ? log_dir : "");
  (void)sb_append(&extra, ",\"seed_snapshot_before\":");
  append_rel_json_str(&extra, nynth_root, seed_before_path ? seed_before_path : "");
  (void)sb_append(&extra, ",\"seed_snapshot_after\":");
  append_rel_json_str(&extra, nynth_root, seed_after_path ? seed_after_path : "");
  (void)sb_append(&extra, ",\"top_seed_file\":");
  append_rel_json_str(&extra, nynth_root, top_seeds_path ? top_seeds_path : "");
  (void)sb_append(&extra, ",\"command_file\":");
  append_rel_json_str(&extra, nynth_root, command_path ? command_path : "");
  (void)sb_append(&extra, ",\"manifest\":");
  append_rel_json_str(&extra, nynth_root, manifest_path ? manifest_path : "");
  (void)sb_append(&extra, ",\"forever_script\":");
  append_rel_json_str(&extra, nynth_root, forever_script_path ? forever_script_path : "");
  if (manifest_path) {
    str_buf_t invocation = {0};
    str_buf_t manifest = {0};
    append_argv_shell_string(&invocation, argv);
    (void)sb_append(&manifest, "{\"kind\":\"gc-campaign-manifest\",\"command\":");
    (void)sb_append_json_str(&manifest, invocation.data ? invocation.data : "");
    (void)sb_append(&manifest, ",\"summary\":{\"engine\":\"nynth_core\"");
    if (extra.data) (void)sb_append(&manifest, extra.data);
    (void)sb_append(&manifest, "}}\n");
    if (manifest.data) (void)write_file_text(manifest_path, manifest.data);
    free(invocation.data);
    free(manifest.data);
  }
  char *report = build_native_report_json(&rows, &failures, "fuzz-gc-campaign", extra.data);
  int rc = emit_native_report(report, json_path, "gc fuzz campaign", rows.count, failures.count);
  free(extra.data);
  free(work_dir);
  free(default_artifact_dir);
  free(artifact_dir);
  free(log_dir);
  free(seed_dir);
  free(seed_before_path);
  free(seed_after_path);
  free(top_seeds_path);
  free(command_path);
  free(manifest_path);
  free(forever_script_path);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static int cmd_public_fuzz_gc_replay(const char *nynth_root, const char *artifact_path,
                                     const char *json_path) {
  file_buf_t artifact = {0};
  string_list_t rows = {0}, failures = {0};
  bool loaded = artifact_path && read_file(artifact_path, &artifact) && artifact.data;
  if (!loaded) {
    (void)string_list_push_take(&failures,
                                make_worker_failure_row("gc-replay", "gc-replay-load", 1, "",
                                                        "failed to read GC replay artifact"));
    (void)string_list_push_take(&rows, native_row_status("gc_replay_load", "fuzz-gc",
                                                        false, "artifact", artifact_path ? artifact_path : ""));
  } else {
    const char *end = artifact.data + artifact.len;
    char *command = json_extract_string_range(artifact.data, end, "replay_command");
    if (!command || !*command) {
      (void)string_list_push_take(&failures,
                                  make_worker_failure_row("gc-replay", "gc-replay-parse", 1, "",
                                                          "missing replay_command"));
      (void)string_list_push_take(&rows, native_row_status("gc_replay_parse", "fuzz-gc",
                                                          false, "artifact", artifact_path));
    } else {
      char *argv[] = {"sh", "-c", command, NULL};
      proc_result_t pr = run_proc(argv, nynth_root, 3600.0);
      bool ok = pr.rc == 0;
      append_gc_proc_row(&rows, nynth_root, "gc_replay", "replay", ok, &pr,
                         artifact_path, "", "");
      if (!ok)
        (void)string_list_push_take(&failures,
                                    make_worker_failure_row("gc-replay", "gc-replay-run",
                                                            pr.rc, pr.out, pr.err));
      proc_result_free(&pr);
    }
    free(command);
  }
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"artifact\":");
  append_rel_json_str(&extra, nynth_root, artifact_path ? artifact_path : "");
  char *report = build_native_report_json(&rows, &failures, "fuzz-gc-replay", extra.data);
  int rc = emit_native_report(report, json_path, "gc fuzz replay", rows.count, failures.count);
  free(extra.data);
  free(artifact.data);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static int cmd_public_fuzz_gc_run(int argc, char **argv) {
  char nynth_root[4096], root[4096], ny_bin[4096];
  if (!find_nynth_root(nynth_root, sizeof(nynth_root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *replay_path = value_after_equals(argc, argv, 4, "--replay", "");
  if (replay_path && *replay_path)
    return cmd_public_fuzz_gc_replay(nynth_root, replay_path, json_path);
  const char *campaign_profile = value_after_equals(argc, argv, 4, "--profile", "");
  if (campaign_profile && *campaign_profile &&
      strcmp(campaign_profile, "single") != 0 &&
      strcmp(campaign_profile, "none") != 0 &&
      strcmp(campaign_profile, "off") != 0) {
    return cmd_public_fuzz_gc_campaign_run(argc, argv, campaign_profile);
  }
  bool have_repo = find_repo_root(root, sizeof(root));
  if (!have_repo) {
    const char *ny_env = getenv("NYTRIX_ROOT");
    if (ny_env && *ny_env) have_repo = find_repo_root_from_path(ny_env, root, sizeof(root));
  }
  if (!have_repo) {
    char sibling[4096];
    if (path_join(sibling, sizeof(sibling), nynth_root, "../nytrix"))
      have_repo = find_repo_root_from_path(sibling, root, sizeof(root));
  }
  if (!have_repo) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  bool has_ny = find_ny_bin(root, ny_bin, sizeof(ny_bin));
  bool smoke = has_flag_after(argc, argv, 4, "--smoke") || has_flag_after(argc, argv, 4, "--fast");
  bool direct_only = has_flag_after(argc, argv, 4, "--direct-only");
  bool ny_only = has_flag_after(argc, argv, 4, "--ny-only");
  bool sanitize = has_flag_after(argc, argv, 4, "--sanitize");
  bool tsan = has_flag_after(argc, argv, 4, "--tsan");
  bool validate_gc = has_flag_after(argc, argv, 4, "--validate-gc");
  bool require_promotions = has_flag_after(argc, argv, 4, "--require-promotions") ||
                            has_flag_after(argc, argv, 4, "--strict-promote");
  const char *direct_mode = value_after_equals(argc, argv, 4, "--mode", "smart");
  const char *minor_every = value_after_equals(argc, argv, 4, "--minor-every", "97");
  const char *major_every = value_after_equals(argc, argv, 4, "--major-every", "4099");
  int seed = atoi(value_after_equals(argc, argv, 4, "--seed", "12648430"));
  const char *threads_arg = value_after_equals(argc, argv, 4, "--threads", "");
  int threads = gc_parse_thread_count(threads_arg, smoke ? 1 : 4);
  int ny_cases = atoi(value_after_equals(argc, argv, 4, "--ny-cases", "0"));
  int ny_rounds = atoi(value_after_equals(argc, argv, 4, "--ny-rounds", smoke ? "64" : "512"));
  const char *ny_family = value_after_equals(argc, argv, 4, "--ny-family", "auto");
  uint64_t iterations = strtoull(value_after_equals(argc, argv, 4, "--iterations", smoke ? "4096" : "0"), NULL, 10);
  double duration_s = fuzz_gc_duration_s(argc, argv, smoke);
  threads = gc_clamp_thread_count(threads);
  if (ny_cases < 0) ny_cases = 0;
  if (ny_only) ny_cases = ny_cases > 0 ? ny_cases : 1;
  if (direct_only) ny_cases = 0;
  if (smoke && duration_s > 5.0) duration_s = 5.0;

  char *work_dir = NULL, *case_dir = NULL, *bin_dir = NULL, *direct_src = NULL;
  (void)nynth_asprintf(&work_dir, "build/fuzz/gc");
  (void)nynth_asprintf(&case_dir, "build/fuzz/gc/cases");
  (void)nynth_asprintf(&bin_dir, "build/fuzz/gc/bin");
  (void)nynth_asprintf(&direct_src, "fuzz/targets/gc_direct_fuzzer.c");
  if (work_dir) ny_ensure_dir_recursive(work_dir);
  if (case_dir) ny_ensure_dir_recursive(case_dir);
  if (bin_dir) ny_ensure_dir_recursive(bin_dir);

  string_list_t rows = {0}, failures = {0};

  if (!ny_only) {
    char *direct_bin = NULL, *gc_c_define = NULL, *gc_c_path = NULL;
    const char *direct_bin_arg = value_after_equals(argc, argv, 4, "--direct-bin", "");
    if (direct_bin_arg && *direct_bin_arg) {
      if (path_is_absolute(direct_bin_arg)) (void)asprintf(&direct_bin, "%s", direct_bin_arg);
      else (void)asprintf(&direct_bin, "%s/%s", nynth_root, direct_bin_arg);
    } else {
      (void)asprintf(&direct_bin, "%s/gc_direct_fuzzer%s", bin_dir ? bin_dir : "/tmp",
                     sanitize ? "_asan" : (tsan ? "_tsan" : ""));
    }
    (void)asprintf(&gc_c_path, "%s/src/rt/gc.c", root);
    (void)asprintf(&gc_c_define, "-DNYTRIX_GC_C=\"%s\"", gc_c_path ? gc_c_path : "");
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    char *compile_argv[48];
    int ca = 0;
    compile_argv[ca++] = (char *)cc;
    compile_argv[ca++] = "-D_GNU_SOURCE";
    compile_argv[ca++] = "-std=c11";
    compile_argv[ca++] = "-O3";
    compile_argv[ca++] = "-g";
    compile_argv[ca++] = "-Wall";
    compile_argv[ca++] = "-Wextra";
    char *inc = NULL;
    (void)asprintf(&inc, "-I%s/src", root);
    compile_argv[ca++] = inc;
    compile_argv[ca++] = gc_c_define;
    if (sanitize && !tsan) {
      compile_argv[ca++] = "-fsanitize=address,undefined";
      compile_argv[ca++] = "-fno-omit-frame-pointer";
    } else if (tsan) {
      compile_argv[ca++] = "-fsanitize=thread";
      compile_argv[ca++] = "-fno-omit-frame-pointer";
    }
    compile_argv[ca++] = direct_src;
    compile_argv[ca++] = "-o";
    compile_argv[ca++] = direct_bin;
    compile_argv[ca++] = "-pthread";
    compile_argv[ca] = NULL;
    proc_result_t compile = {0};
    bool compile_ok = direct_src && path_exists_file(direct_src);
    bool rebuild_direct = has_flag_after(argc, argv, 4, "--rebuild-direct") ||
                          has_flag_after(argc, argv, 4, "--no-direct-cache");
    bool need_compile = rebuild_direct || !path_exists_file(direct_bin) ||
                        !executable_path(direct_bin) ||
                        file_newer_than(direct_src, direct_bin) ||
                        file_newer_than(gc_c_path, direct_bin);
    if (compile_ok && need_compile) compile = run_proc(compile_argv, nynth_root, 120.0);
    compile_ok = compile_ok && (!need_compile || compile.rc == 0);
    append_gc_proc_row(&rows, nynth_root, "gc_direct_compile", "compile",
                       compile_ok, &compile, direct_src, direct_bin,
                       need_compile ? "{\"cached\":false}" : "{\"cached\":true}");
    if (!compile_ok) {
      (void)string_list_push_take(&failures,
                                  make_worker_failure_row("gc-direct", "gc-direct-compile",
                                                          compile.rc ? compile.rc : 1,
                                                          compile.out,
                                                          direct_src && path_exists_file(direct_src)
                                                              ? compile.err
                                                              : "direct GC fuzzer source missing"));
    } else {
      char seed_arg[64], seconds_arg[64], threads_arg[64], iterations_arg[64];
      snprintf(seed_arg, sizeof(seed_arg), "%d", seed);
      snprintf(seconds_arg, sizeof(seconds_arg), "%d", iterations > 0 ? 0 : (int)(duration_s + 0.5));
      snprintf(threads_arg, sizeof(threads_arg), "%d", threads);
      snprintf(iterations_arg, sizeof(iterations_arg), "%" PRIu64,
               iterations ? iterations : (uint64_t)250000);
      char *run_argv[48];
      int ra = 0;
      run_argv[ra++] = "env";
      run_argv[ra++] = "NYTRIX_GC=1";
      run_argv[ra++] = "NYTRIX_GC_PARALLEL=1";
      if (validate_gc) run_argv[ra++] = "NYTRIX_GC_VALIDATE=1";
      if (sanitize && !tsan) run_argv[ra++] = "ASAN_OPTIONS=abort_on_error=1:detect_leaks=0";
      if (tsan) run_argv[ra++] = "TSAN_OPTIONS=halt_on_error=1:report_thread_leaks=0";
      run_argv[ra++] = direct_bin;
      run_argv[ra++] = "--seed"; run_argv[ra++] = seed_arg;
      run_argv[ra++] = "--threads"; run_argv[ra++] = threads_arg;
      run_argv[ra++] = "--mode"; run_argv[ra++] = (char *)(direct_mode && *direct_mode ? direct_mode : "smart");
      run_argv[ra++] = "--minor-every"; run_argv[ra++] = (char *)(minor_every && *minor_every ? minor_every : "97");
      run_argv[ra++] = "--major-every"; run_argv[ra++] = (char *)(major_every && *major_every ? major_every : "4099");
      if (iterations > 0) {
        run_argv[ra++] = "--iterations"; run_argv[ra++] = iterations_arg;
      } else {
        run_argv[ra++] = "--seconds"; run_argv[ra++] = seconds_arg;
      }
      if (require_promotions) run_argv[ra++] = "--require-promotions";
      if (validate_gc) run_argv[ra++] = "--validate-gc";
      run_argv[ra] = NULL;
      double run_timeout = iterations > 0 ? 300.0 : duration_s + 120.0;
      proc_result_t run = run_proc(run_argv, root, run_timeout);
      bool ok = run.rc == 0;
      append_gc_proc_row(&rows, nynth_root, "gc_direct_run", "run", ok, &run,
                         direct_src, direct_bin, run.out ? run.out : "");
      if (!ok) {
        (void)string_list_push_take(&failures,
                                    make_worker_failure_row("gc-direct", "gc-direct-run",
                                                            run.rc, run.out, run.err));
      }
      proc_result_free(&run);
    }
    proc_result_free(&compile);
    free(inc); free(direct_bin); free(gc_c_define); free(gc_c_path);
  }

  if (!direct_only) {
    if (!has_ny) {
      if (ny_cases > 0)
        (void)string_list_push_take(&failures,
                                    make_worker_failure_row("gc-ny", "ny-bin", 1, "",
                                                            "ny binary not found; set NYTRIX_NY_BIN"));
      (void)string_list_push_take(&rows, native_row_status("gc_ny_preflight", "fuzz-gc",
                                                          ny_cases == 0, "ny_bin", ""));
    }
    for (int i = 0; has_ny && i < ny_cases; ++i) {
      int case_seed = seed + i * 104729;
      const char *case_family = gc_resolve_ny_family(ny_family, case_seed);
      char *source = make_gc_ny_fuzz_source(case_seed, ny_rounds, 1, case_family);
      char *path = NULL, *exe = NULL, *base_exe = NULL;
      (void)asprintf(&path, "%s/gc_case_%04d_%d.ny", case_dir ? case_dir : "/tmp", i, case_seed);
      (void)asprintf(&exe, "%s/gc_case_%04d_%d_gc", bin_dir ? bin_dir : "/tmp", i, case_seed);
      (void)asprintf(&base_exe, "%s/gc_case_%04d_%d_base", bin_dir ? bin_dir : "/tmp", i, case_seed);
      bool wrote = source && path && write_file_text(path, source);
      proc_result_t base_compile = {0}, base_run = {0}, compile = {0};
      bool baseline_ok = false;
      char name[96];
      snprintf(name, sizeof(name), "gc_ny_case_%04d", i);
      if (!wrote) {
        append_gc_proc_row(&rows, nynth_root, name, "ny-write", false,
                           NULL, path, exe, "{\"source_written\":false}");
        (void)string_list_push_take(&failures,
                                    make_worker_failure_row(name, "ny-write", 1, "",
                                                            "failed to write Ny GC fuzz case"));
      } else {
        char *base_compile_argv[] = {
          ny_bin, "--compiler-asserts", "-O3", "--profile=peak",
          "-o", base_exe, path, NULL
        };
        base_compile = run_proc(base_compile_argv, root, smoke ? 90.0 : 180.0);
        bool base_compile_ok = base_compile.rc == 0;
        append_gc_proc_row(&rows, nynth_root, name, "ny-baseline-compile",
                           base_compile_ok, &base_compile, path, base_exe,
                           "{\"baseline\":true,\"gc_skipped_on_failure\":true}");
        if (!base_compile_ok) {
          (void)string_list_push_take(&failures,
                                      make_worker_failure_row(name, "ny-baseline-compile",
                                                              base_compile.rc ? base_compile.rc : 1,
                                                              base_compile.out,
                                                              base_compile.err));
        }
        if (base_compile_ok) {
          char rounds_arg[64];
          snprintf(rounds_arg, sizeof(rounds_arg), "%d", ny_rounds);
          char *base_run_argv[] = {base_exe, rounds_arg, NULL};
          base_run = run_proc(base_run_argv, root, smoke ? 60.0 : 180.0);
          baseline_ok = base_run.rc == 0;
          append_gc_proc_row(&rows, nynth_root, name, "ny-baseline-run",
                             baseline_ok, &base_run, path, base_exe,
                             "{\"baseline\":true,\"gc_skipped_on_failure\":true}");
          if (!baseline_ok) {
            (void)string_list_push_take(&failures,
                                        make_worker_failure_row(name, "ny-baseline-run",
                                                                base_run.rc ? base_run.rc : 1,
                                                                base_run.out,
                                                                base_run.err));
          }
        }
      }
      if (wrote && baseline_ok) {
        char *compile_argv[] = {
          ny_bin, "--compiler-asserts", "--heap=gc", "-O3", "--profile=peak",
          "-o", exe, path, NULL
        };
        compile = run_proc(compile_argv, root, smoke ? 90.0 : 180.0);
        bool compile_ok = compile.rc == 0;
        append_gc_proc_row(&rows, nynth_root, name, "ny-gc-compile", compile_ok,
                           &compile, path, exe, "");
        if (!compile_ok) {
          (void)string_list_push_take(&failures,
                                      make_worker_failure_row(name, "ny-gc-compile",
                                                              compile.rc, compile.out,
                                                              compile.err));
        } else {
          char rounds_arg[64];
          snprintf(rounds_arg, sizeof(rounds_arg), "%d", ny_rounds);
          char *run_argv[8];
          int na = 0;
          run_argv[na++] = "env";
          run_argv[na++] = "NYTRIX_GC=1";
          run_argv[na++] = "NYTRIX_GC_PARALLEL=1";
          if (validate_gc) run_argv[na++] = "NYTRIX_GC_VALIDATE=1";
          run_argv[na++] = exe;
          run_argv[na++] = rounds_arg;
          run_argv[na] = NULL;
          proc_result_t run = run_proc(run_argv, root, smoke ? 60.0 : 180.0);
          bool run_ok = run.rc == 0;
          bool output_match = baseline_ok && run_ok && base_run.out && run.out &&
                              strcmp(base_run.out, run.out) == 0;
          double checksum = 0.0;
          (void)extract_gc_stdout_number(run.out, "checksum", &checksum);
          str_buf_t stats = {0};
          (void)sb_append(&stats, "{\"ny_family\":");
          (void)sb_append_json_str(&stats, case_family);
          (void)sb_appendf(&stats, ",\"checksum\":%.0f,\"output_match\":%s}",
                           checksum, output_match ? "true" : "false");
          append_gc_proc_row(&rows, nynth_root, name, "ny-gc-run", run_ok && output_match,
                             &run, path, exe, stats.data ? stats.data : "");
          if (!run_ok)
            (void)string_list_push_take(&failures,
                                        make_worker_failure_row(name, "ny-gc-run", run.rc,
                                                                run.out, run.err));
          else if (!output_match)
            (void)string_list_push_take(&failures,
                                        make_worker_failure_row(name, "ny-output-match", 1,
                                                                run.out, base_run.out));
          free(stats.data);
          proc_result_free(&run);
        }
      }
      proc_result_free(&base_compile);
      proc_result_free(&base_run);
      proc_result_free(&compile);
      free(source); free(path); free(exe); free(base_exe);
    }
  }

  str_buf_t extra = {0};
  (void)sb_appendf(&extra,
                   ",\"seed\":%d,\"duration_s\":%.3f,\"threads\":%d,\"ny_cases\":%d,"
                   "\"ny_rounds\":%d,\"direct_only\":%s,\"ny_only\":%s,"
                   "\"sanitize\":%s,\"tsan\":%s,\"require_promotions\":%s,"
                   "\"validate_gc\":%s,\"direct_mode\":",
                   seed, duration_s, threads, ny_cases, ny_rounds,
                   direct_only ? "true" : "false", ny_only ? "true" : "false",
                   sanitize ? "true" : "false", tsan ? "true" : "false",
                   require_promotions ? "true" : "false",
                   validate_gc ? "true" : "false");
  (void)sb_append_json_str(&extra, direct_mode && *direct_mode ? direct_mode : "smart");
  (void)sb_append(&extra, ",\"minor_every\":");
  (void)sb_append_json_str(&extra, minor_every && *minor_every ? minor_every : "97");
  (void)sb_append(&extra, ",\"major_every\":");
  (void)sb_append_json_str(&extra, major_every && *major_every ? major_every : "4099");
  (void)sb_append(&extra, ",\"ny_family\":");
  (void)sb_append_json_str(&extra, ny_family && *ny_family ? ny_family : "auto");
  (void)sb_append(&extra, ",\"work_dir\":");
  append_rel_json_str(&extra, nynth_root, work_dir ? work_dir : "");
  (void)sb_append(&extra, ",\"ny_bin\":");
  if (has_ny) (void)sb_append_json_str(&extra, ny_bin);
  else (void)sb_append_json_str(&extra, "");
  char *report = build_native_report_json(&rows, &failures, "fuzz-gc", extra.data);
  int rc = emit_native_report(report, json_path, "gc fuzz", rows.count, failures.count);
  free(extra.data);
  free(work_dir); free(case_dir); free(bin_dir); free(direct_src);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

typedef struct fuzz_all_logs {
  char stdout_log[1024];
  char stderr_log[1024];
  char command_log[1024];
} fuzz_all_logs_t;

static void fuzz_all_write_logs(const char *log_dir, const char *name, char *const argv[],
                                const proc_result_t *pr, fuzz_all_logs_t *logs) {
  if (!log_dir || !*log_dir || !name || !*name || !logs) return;
  ny_ensure_dir_recursive(log_dir);
  char stem[128];
  safe_stem(stem, sizeof(stem), name);
  char *stdout_path = NULL, *stderr_path = NULL, *command_path = NULL;
  (void)asprintf(&stdout_path, "%s/%s.stdout.log", log_dir, stem);
  (void)asprintf(&stderr_path, "%s/%s.stderr.log", log_dir, stem);
  (void)asprintf(&command_path, "%s/%s.command.txt", log_dir, stem);
  if (stdout_path) {
    (void)write_file_text(stdout_path, pr && pr->out ? pr->out : "");
    snprintf(logs->stdout_log, sizeof(logs->stdout_log), "%s", stdout_path);
  }
  if (stderr_path) {
    (void)write_file_text(stderr_path, pr && pr->err ? pr->err : "");
    snprintf(logs->stderr_log, sizeof(logs->stderr_log), "%s", stderr_path);
  }
  if (command_path) {
    str_buf_t command = {0};
    append_argv_shell_string(&command, argv);
    (void)sb_append_c(&command, '\n');
    (void)write_file_text(command_path, command.data ? command.data : "\n");
    snprintf(logs->command_log, sizeof(logs->command_log), "%s", command_path);
    free(command.data);
  }
  free(stdout_path);
  free(stderr_path);
  free(command_path);
}

static void fuzz_all_add_skip(const char *root, string_list_t *rows, const char *name,
                              const char *phase, const char *reason) {
  str_buf_t row = {0};
  (void)root;
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, name ? name : "");
  (void)sb_append(&row, ",\"kind\":\"fuzz-all\",\"phase\":");
  (void)sb_append_json_str(&row, phase ? phase : "");
  (void)sb_append(&row, ",\"ok\":true,\"skipped\":true,\"reason\":");
  (void)sb_append_json_str(&row, reason ? reason : "");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
}

static bool fuzz_all_add_step(const char *root, const char *name, const char *phase,
                              char *const cmd_argv[], const char *report_path,
                              double timeout_s, const char *log_dir, bool required,
                              bool fail_fast, string_list_t *rows,
                              string_list_t *failures, bool *continue_running) {
  if (continue_running && !*continue_running) return true;
  proc_result_t pr = run_proc(cmd_argv, root, timeout_s);
  fuzz_all_logs_t logs = {0};
  fuzz_all_write_logs(log_dir, name, cmd_argv, &pr, &logs);
  file_buf_t report = {0};
  bool expect_report = report_path && *report_path;
  bool have_report = expect_report && read_file(report_path, &report) && report.data;
  int sub_rows = -1;
  int sub_quarantined_known_bugs = 0;
  int sub_finding_count = 0, sub_finding_live = 0;
  int sub_finding_cleared = 0, sub_finding_missing = 0;
  int sub_known_bug_count = 0, sub_known_bug_reproduced = 0;
  int sub_known_bug_fixed_candidates = 0, sub_known_bug_lost_signal = 0;
  int sub_known_bug_baseline_failures = 0, sub_perf_hotspots = 0;
  double sub_perf_max_ratio = 0.0;
  char sub_perf_max_case[128] = {0};
  double sub_failures = pr.rc == 0 ? 0.0 : 1.0;
  if (have_report) {
    const char *rows_json = json_top_level_value_after_key(report.data, "rows");
    sub_rows = count_json_array_items(rows_json);
    if (!extract_json_number(report.data, "failure_count", &sub_failures))
      sub_failures = json_failures_nonempty(report.data) ? 1.0 : 0.0;
    double quarantined = 0.0;
    int quarantined_rows = count_json_true_fields(report.data, "quarantined");
    if (extract_json_number(report.data, "quarantined_known_bugs", &quarantined)) {
      sub_quarantined_known_bugs = (int)quarantined;
      if (quarantined_rows > sub_quarantined_known_bugs)
        sub_quarantined_known_bugs = quarantined_rows;
    } else {
      sub_quarantined_known_bugs = quarantined_rows;
    }
    double finding_value = 0.0;
    if (extract_json_number(report.data, "finding_count", &finding_value))
      sub_finding_count = (int)finding_value;
    if (extract_json_number(report.data, "live", &finding_value))
      sub_finding_live = (int)finding_value;
    if (extract_json_number(report.data, "cleared", &finding_value))
      sub_finding_cleared = (int)finding_value;
    if (extract_json_number(report.data, "missing", &finding_value))
      sub_finding_missing = (int)finding_value;
    (void)summarize_known_bug_report(report.data, &sub_known_bug_count,
                                     &sub_known_bug_reproduced,
                                     &sub_known_bug_fixed_candidates,
                                     &sub_known_bug_lost_signal,
                                     &sub_known_bug_baseline_failures);
    (void)summarize_perf_triage_report(report.data, &sub_perf_hotspots,
                                       &sub_perf_max_ratio, sub_perf_max_case,
                                       sizeof(sub_perf_max_case));
  }
  bool ok = pr.rc == 0 && (!expect_report || have_report) && sub_failures == 0.0;
  if (!ok && required) {
    const char *err = pr.err;
    if (expect_report && !have_report) err = "subreport missing";
    (void)string_list_push_take(failures,
                                make_worker_failure_row(name, phase ? phase : "fuzz-all",
                                                        pr.rc ? pr.rc : 1, pr.out, err));
    if (fail_fast && continue_running) *continue_running = false;
  }
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, name ? name : "");
  (void)sb_append(&row, ",\"kind\":\"fuzz-all\",\"phase\":");
  (void)sb_append_json_str(&row, phase ? phase : "");
  (void)sb_appendf(&row,
                   ",\"ok\":%s,\"required\":%s,\"rc\":%d,\"elapsed_ms\":%.2f,"
                   "\"timed_out\":%s,\"sub_rows\":%d,\"sub_failures\":%.0f,"
                   "\"sub_quarantined_known_bugs\":%d,"
                   "\"sub_finding_count\":%d,\"sub_finding_live\":%d,"
                   "\"sub_finding_cleared\":%d,\"sub_finding_missing\":%d,"
                   "\"sub_known_bug_count\":%d,\"sub_known_bug_reproduced\":%d,"
                   "\"sub_known_bug_fixed_candidates\":%d,"
                   "\"sub_known_bug_lost_signal\":%d,"
                   "\"sub_known_bug_baseline_failures\":%d,"
                   "\"sub_perf_hotspots\":%d,\"sub_perf_max_ratio\":%.4f",
                   ok ? "true" : "false", required ? "true" : "false",
                   pr.rc, pr.elapsed_ms, pr.timed_out ? "true" : "false",
                   sub_rows, sub_failures, sub_quarantined_known_bugs,
                   sub_finding_count, sub_finding_live,
                   sub_finding_cleared, sub_finding_missing,
                   sub_known_bug_count, sub_known_bug_reproduced,
                   sub_known_bug_fixed_candidates, sub_known_bug_lost_signal,
                   sub_known_bug_baseline_failures, sub_perf_hotspots,
                   sub_perf_max_ratio);
  (void)sb_append(&row, ",\"sub_perf_max_case\":");
  (void)sb_append_json_str(&row, sub_perf_max_case);
  if (expect_report) {
    (void)sb_append(&row, ",\"report\":");
    append_rel_json_str(&row, root, report_path);
  }
  if (logs.stdout_log[0]) {
    (void)sb_append(&row, ",\"stdout_log\":");
    append_rel_json_str(&row, root, logs.stdout_log);
  }
  if (logs.stderr_log[0]) {
    (void)sb_append(&row, ",\"stderr_log\":");
    append_rel_json_str(&row, root, logs.stderr_log);
  }
  if (logs.command_log[0]) {
    (void)sb_append(&row, ",\"command_log\":");
    append_rel_json_str(&row, root, logs.command_log);
  }
  (void)sb_append(&row, ",\"command\":");
  append_argv_json_array(&row, cmd_argv);
  if (!ok) append_proc_tail_fields(&row, &pr);
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  free(report.data);
  proc_result_free(&pr);
  return ok;
}

static char *fuzz_all_report_path(const char *report_dir, const char *name) {
  char stem[128];
  safe_stem(stem, sizeof(stem), name ? name : "lane");
  char *path = NULL;
  (void)asprintf(&path, "%s/%s.json", report_dir ? report_dir : "build/fuzz/all", stem);
  return path;
}

static char *json_path_with_suffix(const char *path, const char *suffix) {
  return path_with_suffix_ext(path, suffix, ".json");
}

static char *path_with_suffix_ext(const char *path, const char *suffix, const char *ext) {
  if (!path || !*path || !suffix) return NULL;
  if (!ext || !*ext) ext = "";
  size_t len = strlen(path);
  char *out = NULL;
  if (len > 5 && strcmp(path + len - 5, ".json") == 0) {
    (void)asprintf(&out, "%.*s%s%s", (int)(len - 5), path, suffix, ext);
  } else {
    (void)asprintf(&out, "%s%s%s", path, suffix, ext);
  }
  return out;
}

static bool json_bool_range(const char *start, const char *end, const char *key, bool fallback) {
  const char *p = json_value_after_key_range(start, end, key);
  if (!p || p >= end) return fallback;
  if ((size_t)(end - p) >= 4 && strncmp(p, "true", 4) == 0) return true;
  if ((size_t)(end - p) >= 5 && strncmp(p, "false", 5) == 0) return false;
  return fallback;
}

static bool json_number_range(const char *start, const char *end, const char *key, double *out) {
  const char *p = json_value_after_key_range(start, end, key);
  if (!p || p >= end) return false;
  char *next = NULL;
  double v = strtod(p, &next);
  if (next == p || next > end) return false;
  if (out) *out = v;
  return true;
}

static bool summarize_known_bug_report(const char *json, int *known_bug_count,
                                       int *reproduced, int *fixed_candidates,
                                       int *lost_signal, int *baseline_failures) {
  if (known_bug_count) *known_bug_count = 0;
  if (reproduced) *reproduced = 0;
  if (fixed_candidates) *fixed_candidates = 0;
  if (lost_signal) *lost_signal = 0;
  if (baseline_failures) *baseline_failures = 0;
  if (!json) return false;
  const char *summary = json_top_level_value_after_key(json, "summary");
  if (!summary || *summary != '{') return false;
  const char *end = matching_json_end(summary, '{', '}');
  if (!end) return false;
  double value = 0.0;
  bool found = false;
  if (json_number_range(summary, end + 1, "known_bug_count", &value)) {
    if (known_bug_count) *known_bug_count = (int)value;
    found = true;
  }
  if (json_number_range(summary, end + 1, "reproduced", &value)) {
    if (reproduced) *reproduced = (int)value;
    found = true;
  }
  if (json_number_range(summary, end + 1, "fixed_candidates", &value)) {
    if (fixed_candidates) *fixed_candidates = (int)value;
    found = true;
  }
  if (json_number_range(summary, end + 1, "lost_signal", &value)) {
    if (lost_signal) *lost_signal = (int)value;
    found = true;
  }
  if (json_number_range(summary, end + 1, "baseline_failures", &value)) {
    if (baseline_failures) *baseline_failures = (int)value;
    found = true;
  }
  return found;
}

static bool summarize_perf_triage_report(const char *json, int *hotspots,
                                         double *max_ratio, char *max_case,
                                         size_t max_case_sz) {
  if (hotspots) *hotspots = 0;
  if (max_ratio) *max_ratio = 0.0;
  if (max_case && max_case_sz) max_case[0] = '\0';
  if (!json) return false;
  const char *rows_json = json_top_level_value_after_key(json, "rows");
  const char *rows_end = rows_json && *rows_json == '[' ? matching_json_end(rows_json, '[', ']') : NULL;
  if (!rows_json || !rows_end) return false;
  int local_hotspots = 0;
  double local_max = 0.0;
  char local_case[128] = {0};
  bool found = false;
  const char *p = rows_json + 1;
  while (p < rows_end) {
    p = skip_ws_const(p);
    if (p >= rows_end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '{') break;
    const char *obj_end = matching_json_end(p, '{', '}');
    if (!obj_end || obj_end > rows_end) break;
    double ratio = 0.0;
    bool has_ratio = json_number_range(p, obj_end + 1, "ratio", &ratio);
    bool hot = json_bool_range(p, obj_end + 1, "hot", false);
    if (hot) ++local_hotspots;
    if (has_ratio) {
      found = true;
      if (ratio > local_max) {
        local_max = ratio;
        char *case_name = json_extract_string_range(p, obj_end + 1, "case");
        snprintf(local_case, sizeof(local_case), "%s", case_name ? case_name : "");
        free(case_name);
      }
    }
    p = obj_end + 1;
  }
  if (!found) return false;
  if (hotspots) *hotspots = local_hotspots;
  if (max_ratio) *max_ratio = local_max;
  if (max_case && max_case_sz) snprintf(max_case, max_case_sz, "%s", local_case);
  return true;
}

static bool summary_number_from_report(const char *json, const char *key, double *out) {
  const char *summary = json_top_level_value_after_key(json, "summary");
  if (!summary || *summary != '{') return false;
  const char *end = matching_json_end(summary, '{', '}');
  if (!end) return false;
  return json_number_range(summary, end + 1, key, out);
}

static char *summary_string_from_report(const char *json, const char *key) {
  const char *summary = json_top_level_value_after_key(json, "summary");
  if (!summary || *summary != '{') return strdup("");
  const char *end = matching_json_end(summary, '{', '}');
  if (!end) return strdup("");
  char *value = json_extract_string_range(summary, end + 1, key);
  return value ? value : strdup("");
}

static char *summary_array_from_report(const char *json, const char *key) {
  const char *summary = json_top_level_value_after_key(json, "summary");
  if (!summary || *summary != '{') return strdup("[]");
  const char *end = matching_json_end(summary, '{', '}');
  if (!end) return strdup("[]");
  char *value = json_extract_array_range(summary, end + 1, key);
  return value ? value : strdup("[]");
}

static bool summary_bool_from_report(const char *json, const char *key, bool *out) {
  const char *summary = json_top_level_value_after_key(json, "summary");
  if (!summary || *summary != '{') return false;
  const char *end = matching_json_end(summary, '{', '}');
  if (!end) return false;
  bool fallback = false;
  bool value = json_bool_range(summary, end + 1, key, fallback);
  const char *p = json_value_after_key_range(summary, end + 1, key);
  if (!p) return false;
  if (out) *out = value;
  return true;
}

static bool fuzz_all_audit_lane_seen(const string_list_t *lanes, const char *name) {
  return name && *name && string_list_contains(lanes, name);
}

static char *make_fuzz_all_audit_lane_row(const char *root, const char *name,
                                          const char *phase, bool ok, bool required,
                                          bool skipped, double elapsed_ms,
                                          double sub_rows, double sub_failures,
                                          double sub_quarantined_known_bugs,
                                          double sub_finding_count,
                                          double sub_finding_live,
                                          double sub_finding_cleared,
                                          double sub_finding_missing,
                                          double sub_known_bug_count,
                                          double sub_known_bug_reproduced,
                                          double sub_known_bug_fixed_candidates,
                                          double sub_known_bug_lost_signal,
                                          double sub_known_bug_baseline_failures,
                                          double sub_perf_hotspots,
                                          double sub_perf_max_ratio,
                                          const char *sub_perf_max_case,
                                          const char *report, bool report_exists,
                                          bool logs_ok) {
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, name ? name : "");
  (void)sb_append(&row, ",\"kind\":\"fuzz-all-audit-lane\",\"phase\":");
  (void)sb_append_json_str(&row, phase ? phase : "");
  (void)sb_appendf(&row,
                   ",\"ok\":%s,\"required\":%s,\"skipped\":%s,"
                   "\"elapsed_ms\":%.2f,\"sub_rows\":%.0f,\"sub_failures\":%.0f,"
                   "\"sub_quarantined_known_bugs\":%.0f,"
                   "\"sub_finding_count\":%.0f,\"sub_finding_live\":%.0f,"
                   "\"sub_finding_cleared\":%.0f,\"sub_finding_missing\":%.0f,"
                   "\"sub_known_bug_count\":%.0f,"
                   "\"sub_known_bug_reproduced\":%.0f,"
                   "\"sub_known_bug_fixed_candidates\":%.0f,"
                   "\"sub_known_bug_lost_signal\":%.0f,"
                   "\"sub_known_bug_baseline_failures\":%.0f,"
                   "\"sub_perf_hotspots\":%.0f,\"sub_perf_max_ratio\":%.4f,"
                   "\"report_exists\":%s,\"logs_ok\":%s",
                   ok ? "true" : "false", required ? "true" : "false",
                   skipped ? "true" : "false", elapsed_ms, sub_rows, sub_failures,
                   sub_quarantined_known_bugs,
                   sub_finding_count, sub_finding_live,
                   sub_finding_cleared, sub_finding_missing,
                   sub_known_bug_count, sub_known_bug_reproduced,
                   sub_known_bug_fixed_candidates, sub_known_bug_lost_signal,
                   sub_known_bug_baseline_failures,
                   sub_perf_hotspots, sub_perf_max_ratio,
                   report_exists ? "true" : "false", logs_ok ? "true" : "false");
  (void)sb_append(&row, ",\"sub_perf_max_case\":");
  (void)sb_append_json_str(&row, sub_perf_max_case ? sub_perf_max_case : "");
  if (report && *report) {
    (void)sb_append(&row, ",\"report\":");
    append_rel_json_str(&row, root, report);
  }
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  return sb_take(&row);
}

