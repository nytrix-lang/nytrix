#ifndef NYTRIX_TEST_FUZZ_CORE_H
#define NYTRIX_TEST_FUZZ_CORE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "base/util.h"
#include "cbridge.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/utime.h>
#include <windows.h>

#define setenv ny_setenv
#define unsetenv ny_unsetenv
#define realpath ny_realpath
#define lstat stat
#define sleep(seconds) Sleep((DWORD)((seconds) * 1000u))
#ifndef O_WRONLY
#define O_WRONLY _O_WRONLY
#endif
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

static inline int ny_win_utimensat(int dirfd, const char *path,
                                   const struct timespec times[2], int flags) {
  (void)dirfd;
  (void)flags;
  if (!path || !times)
    return -1;
  struct __utimbuf64 value;
  value.actime = (__time64_t)times[0].tv_sec;
  value.modtime = (__time64_t)times[1].tv_sec;
  return _utime64(path, &value);
}
#define utimensat ny_win_utimensat

/* MinGW does not ship dirent.h.  Keep the fuzzer's directory walker API
 * stable and map it to FindFirstFile/FindNextFile. */
struct dirent {
  char d_name[MAX_PATH];
};
typedef struct {
  HANDLE handle;
  WIN32_FIND_DATAA first;
  bool have_first;
  struct dirent entry;
} DIR;

static inline DIR *ny_win_opendir(const char *path) {
  if (!path || !*path)
    return NULL;
  char pattern[MAX_PATH * 4];
  size_t n = strlen(path);
  if (n + 3 >= sizeof(pattern))
    return NULL;
  memcpy(pattern, path, n);
  while (n && (pattern[n - 1] == '/' || pattern[n - 1] == '\\'))
    --n;
  pattern[n++] = '\\';
  pattern[n++] = '*';
  pattern[n] = '\0';
  DIR *out = (DIR *)calloc(1, sizeof(*out));
  if (!out)
    return NULL;
  out->handle = FindFirstFileA(pattern, &out->first);
  if (out->handle == INVALID_HANDLE_VALUE) {
    free(out);
    return NULL;
  }
  out->have_first = true;
  return out;
}

static inline struct dirent *ny_win_readdir(DIR *dir) {
  if (!dir)
    return NULL;
  WIN32_FIND_DATAA data;
  if (dir->have_first) {
    data = dir->first;
    dir->have_first = false;
  } else if (!FindNextFileA(dir->handle, &data)) {
    return NULL;
  }
  snprintf(dir->entry.d_name, sizeof(dir->entry.d_name), "%s", data.cFileName);
  return &dir->entry;
}

static inline int ny_win_closedir(DIR *dir) {
  if (!dir)
    return -1;
  FindClose(dir->handle);
  free(dir);
  return 0;
}

#define opendir ny_win_opendir
#define readdir ny_win_readdir
#define closedir ny_win_closedir

static inline char *ny_win_strndup(const char *s, size_t n) {
  if (!s)
    return NULL;
  char *out = (char *)malloc(n + 1u);
  if (!out)
    return NULL;
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}
#define strndup ny_win_strndup
#else
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NYTRIX_PRINTF_FMT(fmt_index, first_arg)                                \
  __attribute__((format(printf, fmt_index, first_arg)))
#else
#define NYTRIX_PRINTF_FMT(fmt_index, first_arg)
#endif

#define NYTRIX_FUZZ_ALL_QUICK_JQ_EXPR                                          \
  "{gate:{ready,blockers,active:.active_count},score:{pct:.language_score_"    \
  "percent,label:.language_score_label,state:.completion_state},campaign:{"    \
  "pct:.campaign_done_percent,years:.campaign_thread_years,target:.campaign_"  \
  "target_thread_years,rem:.campaign_remaining_thread_years,runs:.campaign_"   \
  "runs_needed,days:.campaign_wall_days_needed,per:.campaign_percent_per_run," \
  "per_day:.campaign_percent_per_wall_day,eq_days:.campaign_equivalent_wall_"  \
  "days,src:.thread_years_per_run_source,plan_h:.campaign_plan_wall_hours,"    \
  "plan_threads:.campaign_plan_threads,span_days:.campaign_calendar_span_"     \
  "days,age_days:.campaign_calendar_age_days,calendar_pct:.campaign_calendar_" \
  "percent_10y,eta:.campaign_eta_local},fresh:{ok:.evidence_fresh,penalty:."   \
  "freshness_penalty,latest_h:.latest_report_age_hours,latest_win_h:.latest_"  \
  "report_stale_after_hours,latest_over_h:.latest_report_freshness_overdue_"   \
  "hours,full_h:.latest_full_pressure_report_age_hours,full_win_h:.latest_"    \
  "full_pressure_report_stale_after_hours,full_over_h:.latest_full_pressure_"  \
  "report_freshness_overdue_hours,over_h:.evidence_freshness_overdue_hours},"  \
  "next:{action:.recommended_action,reason:.recommended_reason,script:.next_"  \
  "script,handoff:.next_handoff_command,run:.next_command,recommended:."       \
  "recommended_command,preview:.recommended_preview_command,low:.recommended_" \
  "low_cpu_command,gentle:.run_next_gentle_command,gentle_preview:.run_next_"  \
  "gentle_preview_command,state_refresh:.recommended_state_refresh_command,"   \
  "freshen:.freshness_action_command,stop:.stop_command,resume:.resume_"       \
  "command},runstate:{state:.state,event:.state_event,fresh:.state_fresh,age:" \
  ".state_age_seconds,age_h:(.state_age_seconds/"                              \
  "3600),stale_after:.state_stale_after_seconds,stale_after_h:(.state_stale_"  \
  "after_seconds/"                                                             \
  "3600),over_s:(.state_age_seconds-.state_stale_after_seconds),reason:."      \
  "state_stale_reason,dry_h:.state_dry_run_wall_hours,dry_gain_pct:.state_"    \
  "dry_run_campaign_gain_percent,dry_years:.state_dry_run_thread_years,"       \
  "threads:.state_handoff_threads},surfaces:{compiler:.compiler_findings,"     \
  "known:.known_bug_replay_findings,perf:{hotspots:.perf_hotspots_open,watch:" \
  ".perf_watchlist_state,worst:{case:.perf_worst_case,ratio:.perf_worst_"      \
  "ratio,slow:.perf_worst_slowdown_percent},opt:{action:.optimization_action," \
  "case:.optimization_case,ratio:.optimization_ratio,cmd:.optimization_"       \
  "command,target:.optimization_target_command}},rt:{state:.runtime_surface_"  \
  "state,done:.runtime_coverage_done,total:.runtime_coverage_total},crt:{"     \
  "state:.crt_surface_state,scope:.crt_surface_scope,behavior:.crt_behavior_"  \
  "state,next:.crt_behavior_next_action,done:.crt_coverage_done,total:.crt_"   \
  "coverage_total,families:.crt_unreferenced_family_count}},paths:{scratch:."  \
  "scratch_root,tmp:.tmp_dir,xdg:.xdg_cache_home,nytrix_cache:.nytrix_cache_"  \
  "dir,old_test:.old_nytrix_test_scratch_absent,old_fuzz:.old_nytrix_fuzz_"    \
  "absent,old_cache:.old_nytrix_build_cache_absent,old_writer:.active_old_"    \
  "nytrix_output_writer_present,old_policy:.old_path_cache_policy_ok,old_"     \
  "action:.old_path_next_action,old_seen:.old_path_present_count,old_moved:."  \
  "old_path_moved_count,old_current:.old_path_remaining_count,old_wait_s:."    \
  "old_path_wait_remaining_seconds,old_leaks:.old_path_artifact_leak_count,"   \
  "artifact_remaining:.old_path_artifact_remaining_count}}"
#define NYTRIX_FUZZ_ALL_QUICK_JQ_STATUS                                        \
  "jq " NYTRIX_FUZZ_ALL_QUICK_JQ_EXPR " build/fuzz/all/status.json"
#define NYTRIX_FUZZ_ALL_STATE_JQ_EXPR                                          \
  "{state,event,live,child_status,stale_after_seconds,repeat_mode,repeat_"     \
  "count,handoff_low_priority,handoff_nice,handoff_load_wait,handoff_max_"     \
  "load_pct,handoff_space_guard,handoff_min_free_gb,handoff_run_lock,handoff_" \
  "threads,heartbeat_s,heartbeat_count,child_pid,cycle,cycles,max_cycles,"     \
  "cooldown_s,timestamp_utc,updated_at,started_at,finished_at,pid,campaign_"   \
  "dir,stop_file,status_report,status_json,progress_report,progress_json,dry_" \
  "run_exceeds_max,dry_run_wall_hours,dry_run_wall_days,dry_run_thread_years," \
  "dry_run_campaign_gain_percent,dry_run_target_percent_per_run,dry_run_"      \
  "thread_years_per_run,canonical_status_report,canonical_progress_report,"    \
  "last_report}"
#define NYTRIX_FUZZ_ALL_STATE_JQ_DEFAULT                                       \
  "jq " NYTRIX_FUZZ_ALL_STATE_JQ_EXPR " build/fuzz/all/run-next-state.json"
#define NYTRIX_FUZZ_ALL_KNOWN_BUGS_REPORT                                      \
  "build/fuzz/ultra/compiler-known-bugs.json"
#define NYTRIX_FUZZ_ALL_KNOWN_BUGS_COMMAND                                     \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix "    \
  "compiler known-bugs --timeout-s 15 "                                        \
  "--json " NYTRIX_FUZZ_ALL_KNOWN_BUGS_REPORT
#define NYTRIX_FUZZ_ALL_KNOWN_BUGS_PROBE                                       \
  "jq "                                                                        \
  "{ok,cases,ok_count,failure_count,known_bug_count,known_bug_reproduced,"     \
  "known_bug_fixed_candidates,known_bug_lost_signal,known_bug_baseline_"       \
  "failures} " NYTRIX_FUZZ_ALL_KNOWN_BUGS_REPORT
#define NYTRIX_FUZZ_ALL_PERF_TRIAGE_REPORT                                     \
  "build/fuzz/ultra/perf-triage-current.json"
#define NYTRIX_FUZZ_ALL_PERF_TRIAGE_MARKDOWN                                   \
  "build/fuzz/ultra/perf-triage-current.md"
#define NYTRIX_FUZZ_ALL_PERF_TRIAGE_COMMAND                                    \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix "    \
  "perf triage --fast --limit 5 --threshold 1.50 "                             \
  "--json " NYTRIX_FUZZ_ALL_PERF_TRIAGE_REPORT                                 \
  " --markdown " NYTRIX_FUZZ_ALL_PERF_TRIAGE_MARKDOWN
#define NYTRIX_FUZZ_ALL_PERF_TRIAGE_PROBE                                      \
  "jq "                                                                        \
  "{ok,cases,ok_count,failure_count,perf_hotspots,perf_worst_ratio,perf_"      \
  "worst_case,perf_worst_slowdown_percent}"                                    \
  " " NYTRIX_FUZZ_ALL_PERF_TRIAGE_REPORT
#define NYTRIX_FUZZ_ALL_SELFTEST_FOCUSED_COMMAND                               \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix "    \
  "selftest run --only fuzz_all_help --json "                                  \
  "build/fuzz/all/selftest-fuzz_all_help.json --markdown "                     \
  "build/fuzz/all/selftest-fuzz_all_help.md"
#define NYTRIX_FUZZ_ALL_SELFTEST_TEMPLATE_COMMAND                              \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix "    \
  "selftest run --only NAME --json build/fuzz/all/selftest-NAME.json "         \
  "--markdown build/fuzz/all/selftest-NAME.md"
#define NYTRIX_FUZZ_ALL_SELFTEST_FULL_COMMAND                                  \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix "    \
  "selftest run --full --json build/fuzz/all/selftest-full.json --markdown "   \
  "build/fuzz/all/selftest-full.md"
#define NYTRIX_FUZZ_ALL_SELFTEST_CATALOG                                       \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix "    \
  "selftest run --list --json build/fuzz/all/selftest-catalog.json "           \
  "--markdown build/fuzz/all/selftest-catalog.md"
#define NYTRIX_FUZZ_ALL_SELFTEST_PROBE                                         \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix "    \
  "selftest run --list --probe --json build/fuzz/all/selftest-catalog.json "   \
  "--markdown build/fuzz/all/selftest-catalog.md"
#define NYTRIX_FUZZ_ALL_SELFTEST_RUN                                           \
  "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix "    \
  "selftest run --only fuzz_all_help --only fuzz_all_audit --only "            \
  "fuzz_all_default_pressure --only fuzz_all_coverage_commands --only "        \
  "fuzz_all_coverage_focus_companions --only fuzz_all_history_commands "       \
  "--only fuzz_all_preflight_isolation --only fuzz_all_status_canonical "      \
  "--only fuzz_all_status_stale_evidence --only "                              \
  "fuzz_all_repeat_status_progress --only fuzz_all_reporting --only "          \
  "fuzz_all_fresh_handoff --only fuzz_all_progress_canonical --only "          \
  "fuzz_all_progress --only fuzz_all_progress_stale_evidence --only "          \
  "fuzz_all_progress_refresh_fail --only fuzz_all_full_pressure_remediation "  \
  "--only fuzz_repro_ready_missing_wrapper --only "                            \
  "fuzz_repro_ready_missing_command --only fuzz_all_plan_coverage_next "       \
  "--only fuzz_all_old_paths --only fuzz_all_old_paths_dry_run --only "        \
  "fuzz_all_old_paths_empty_dry_run --only fuzz_all_old_writer_classifier "    \
  "--only compiler_findings --only compiler_known_bugs --only "                \
  "compiler_std_audit --only perf_triage_args --json "                         \
  "build/fuzz/all/selftest-cockpit.json --markdown "                           \
  "build/fuzz/all/selftest-cockpit.md"
#define NYTRIX_FUZZ_ALL_SELFTEST_COCKPIT_PROBE                                 \
  "jq "                                                                        \
  "{ok,cases,ok_count,failure_count,requested_cases,executed_cases,skipped_"   \
  "slow_count,all_requested_executed} build/fuzz/all/selftest-cockpit.json"

void nytrix_print_worker_usage(FILE *out);
void nytrix_print_public_usage(FILE *out);
void nytrix_print_public_help(FILE *out);
void nytrix_print_fuzz_all_help(FILE *out);
bool read_file(const char *path, file_buf_t *out);
char *nytrix_shape_source_block(const char *shape_path, const char *name);
uint64_t fnv1a64(const char *data, size_t len);
double now_ms(void);
int count_sub(const char *data, size_t len, const char *needle);
bool ident_char(char c);
int count_word_call(const char *data, size_t len, const char *word);
int count_regexish_assign_list(const char *data, size_t len, const char *kw);
int count_lines(const char *data, size_t len);
bool has_suffix(const char *path, const char *suffix);
const char *arg_value(int argc, char **argv, const char *name,
                      const char *fallback);
bool arg_flag(int argc, char **argv, const char *name);
const char *base_name(const char *path);
void stem_name(const char *path, char *out, size_t out_sz);

proc_result_t run_proc(char *const argv[], const char *cwd, double timeout_s);
void proc_result_free(proc_result_t *r);
char *normalize_output_pair(const char *out, const char *err);
void run_many_result_free(run_many_result_t *r);
run_many_result_t run_binary_many_native(const char *root, const char *path,
                                         double timeout_s, int runs,
                                         int warmup);

bool compute_source_shape_counts(const char *c_path, const char *ny_path,
                                 source_counts_t *out);
void print_source_counts_json(FILE *out, const source_counts_t *c);
int validate_shapes_emit_json(const char *dir, FILE *out, int *count,
                              int *errors, int *typed, int *optimizer,
                              int *torture, int *stress, int *program);
cbridge_convert_result_t convert_cbridge_file(const char *c_path);
void cbridge_convert_result_free(cbridge_convert_result_t *result);
void print_cbridge_features_json(FILE *out,
                                 const cbridge_convert_result_t *result);
int native_compare_case_with_features(const char *case_name, const char *c_path,
                                      const char *ny_path, const char *ir_path,
                                      const char *root_dir,
                                      const char *ny_bin_path,
                                      const char *bin_dir, double timeout_s,
                                      int run_repeats, int warmup,
                                      const char **features, int feature_count);

int ny_fuzz_main(int argc, char **argv);

int cmd_hash_case(const char *path);
int cmd_shape_count(const char *path);
int cmd_scan_ir_markers(const char *path);
int cmd_source_shape_counts(const char *c_path, const char *ny_path);
int cmd_analyze_ir(const char *path);
int cmd_validate_shapes(const char *dir);
int cmd_generate_batch(int argc, char **argv);
int nytrix_synth_print_c_program(FILE *out, const char *shape_dir,
                                 const char *generator, const char *profile,
                                 const char *shape_name, int seed, bool fast,
                                 bool insane);
int cmd_convert_cbridge(int argc, char **argv);
int cmd_compare_case(int argc, char **argv);
int cmd_replay_corpus_entry(int argc, char **argv);

#endif
