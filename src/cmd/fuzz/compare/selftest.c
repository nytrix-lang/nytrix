static bool selftest_command_uses_env_nice(const char *command) {
  return command &&
         strncmp(command, "env NYTRIX_LOW_PRIORITY=1", 24) == 0 &&
         strstr(command, "NYTRIX_RUN_NICE=10") &&
         strstr(command, "nice -n 10") &&
         strncmp(command, "NYTRIX_LOW_PRIORITY=", 19) != 0;
}

static void selftest_validate_fuzz_reporting(const char *json,
                                             string_list_t *errors,
                                             int *row_count,
                                             const char *expected_dir,
                                             bool canonical_pair,
                                             bool stale_evidence) {
  const char *cockpit_dir =
      expected_dir && *expected_dir ? expected_dir : "fuzz_reporting";
  char expected_status_json[512];
  char expected_status_md[512];
  char expected_coverage_json[512];
  char expected_coverage_md[512];
  char expected_old_paths_json[512];
  char expected_old_paths_md[512];
  char expected_progress_json[512];
  char expected_progress_md[512];
  char expected_worklist_history_json[512];
  char expected_worklist_history_md[512];
  char expected_run_next[512];
  char expected_stop_file[512];
  char expected_state_file[512];
  snprintf(expected_status_json, sizeof(expected_status_json), "%s/status.json",
           cockpit_dir);
  snprintf(expected_status_md, sizeof(expected_status_md), "%s/status.md",
           cockpit_dir);
  snprintf(expected_coverage_json, sizeof(expected_coverage_json),
           "%s/coverage.json", cockpit_dir);
  snprintf(expected_coverage_md, sizeof(expected_coverage_md),
           "%s/coverage.md", cockpit_dir);
  snprintf(expected_old_paths_json, sizeof(expected_old_paths_json),
           "%s/old-paths.json", cockpit_dir);
  snprintf(expected_old_paths_md, sizeof(expected_old_paths_md),
           "%s/old-paths.md", cockpit_dir);
  snprintf(expected_progress_json, sizeof(expected_progress_json),
           "%s/progress.json", cockpit_dir);
  snprintf(expected_progress_md, sizeof(expected_progress_md),
           "%s/progress.md", cockpit_dir);
  snprintf(expected_worklist_history_json,
           sizeof(expected_worklist_history_json),
           "%s/worklist-history.json", cockpit_dir);
  snprintf(expected_worklist_history_md, sizeof(expected_worklist_history_md),
           "%s/worklist-history.md", cockpit_dir);
  snprintf(expected_run_next, sizeof(expected_run_next), "%s/run-next.sh",
           cockpit_dir);
  snprintf(expected_stop_file, sizeof(expected_stop_file), "%s/stop",
           cockpit_dir);
  snprintf(expected_state_file, sizeof(expected_state_file),
           "%s/run-next-state.json", cockpit_dir);
  const char *expected_progress_status_json =
      canonical_pair ? expected_status_json : "fuzz_all_reporting.json";
  double expected_score_min = stale_evidence ? 64.99 : 84.99;
  double expected_score_max = stale_evidence ? 65.01 : 85.01;
  double expected_freshness_penalty = stale_evidence ? 20.0 : 0.0;
  double expected_ignored_no_evidence =
      strcmp(cockpit_dir, "fuzz_status_canonical") == 0 ? 1.0 : 0.0;
  const char *expected_label = stale_evidence ? "promising" : "good";
  selftest_validate_standard_report(json, errors, row_count);
  selftest_validate_fuzz_all_top_aliases(json, errors);
  selftest_expect_top_alias_string(json, "quick_probe_command", errors);
  selftest_expect_top_alias_string(json, "state_probe_command", errors);
  selftest_expect_top_alias_string(json, "selftest_catalog_command", errors);
  selftest_expect_top_alias_string(json, "selftest_result_probe_command",
                                   errors);
  selftest_expect_top_alias_string(json, "selftest_cockpit_run_command",
                                   errors);
  selftest_expect_top_alias_string(json,
                                   "selftest_cockpit_result_probe_command",
                                   errors);
  selftest_expect_top_alias_string(json, "known_bugs_command", errors);
  selftest_expect_top_alias_string(json, "known_bugs_report", errors);
  selftest_expect_top_alias_string(json, "known_bugs_markdown", errors);
  selftest_expect_top_alias_string(json, "known_bugs_result_probe_command",
                                   errors);
  selftest_expect_top_alias_bool(json, "known_bugs_readable", errors);
  selftest_expect_top_alias_number(json, "known_bug_count",
                                   "known_bug_count", errors);
  selftest_expect_top_alias_number(json, "known_bug_fixed_candidates",
                                   "known_bug_fixed_candidates", errors);
  selftest_expect_top_alias_string(json, "perf_triage_command", errors);
  selftest_expect_top_alias_string(json, "perf_triage_report", errors);
  selftest_expect_top_alias_string(json, "perf_triage_markdown", errors);
  selftest_expect_top_alias_string(json, "perf_triage_result_probe_command",
                                   errors);
  selftest_expect_top_alias_bool(json, "perf_triage_readable", errors);
  selftest_expect_top_alias_number(json, "perf_triage_cases",
                                   "perf_triage_cases", errors);
  selftest_expect_top_alias_number(json, "perf_triage_failure_count",
                                   "perf_triage_failure_count", errors);
  selftest_expect_top_alias_number(json, "perf_triage_hotspots",
                                   "perf_triage_hotspots", errors);
  selftest_expect_top_alias_number(json, "perf_triage_worst_ratio",
                                   "perf_triage_worst_ratio", errors);
  selftest_expect_top_alias_number(json, "perf_triage_worst_slowdown_percent",
                                   "perf_triage_worst_slowdown_percent",
                                   errors);
  selftest_expect_top_alias_string(json, "perf_triage_worst_case", errors);
  char *status_quick_probe_command =
      summary_string_from_report(json, "quick_probe_command");
  if (!status_quick_probe_command ||
      !strstr(status_quick_probe_command, expected_progress_status_json) ||
      !strstr(status_quick_probe_command, "paths:{scratch:.scratch_root"))
    (void)string_list_push_copy(errors,
                                "status quick probe alias missing");
  free(status_quick_probe_command);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-status") != 0)
    (void)string_list_push_copy(errors, "status report mode mismatch");
  const char *rows_start = strstr(json, "\"rows\":[");
  const char *status_row = rows_start ?
      strstr(rows_start, "\"kind\":\"fuzz-all-status\"") : NULL;
  const char *gap_row = rows_start ?
      strstr(rows_start, "\"kind\":\"fuzz-all-status-coverage-gap\"") : NULL;
  if (!rows_start || !status_row || status_row > rows_start + 16)
    (void)string_list_push_copy(errors, "status summary row is not first");
  if (gap_row && status_row && gap_row < status_row)
    (void)string_list_push_copy(errors,
                                "status coverage rows precede summary row");
  double status_target_percent = -1.0, campaign_percent = -1.0;
  double campaign_confidence = -1.0;
  if (!summary_number_from_report(json, "target_percent",
                                  &status_target_percent) ||
      status_target_percent < 0.0)
    (void)string_list_push_copy(errors, "status target percent is missing");
  if (!summary_number_from_report(json, "campaign_percent",
                                  &campaign_percent) ||
      campaign_percent < 0.0)
    (void)string_list_push_copy(errors, "status campaign percent is missing");
  if (!summary_number_from_report(json, "campaign_confidence_percent",
                                  &campaign_confidence) ||
      campaign_confidence < 0.0)
    (void)string_list_push_copy(errors,
                                "status campaign confidence is missing");
  double ignored_no_evidence = -1.0, ignored_no_evidence_alias = -2.0;
  if (!summary_number_from_report(json, "ignored_no_evidence_reports",
                                  &ignored_no_evidence) ||
      ignored_no_evidence != expected_ignored_no_evidence)
    (void)string_list_push_copy(errors,
                                "status ignored no-evidence count wrong");
  if (!summary_number_from_report(json,
                                  "evidence_ignored_no_evidence_reports",
                                  &ignored_no_evidence_alias) ||
      ignored_no_evidence_alias != ignored_no_evidence)
    (void)string_list_push_copy(errors,
                                "status ignored no-evidence alias wrong");
  double campaign_delta = campaign_percent - status_target_percent;
  if (campaign_delta < -0.0001 || campaign_delta > 0.0001)
    (void)string_list_push_copy(errors,
                                "status campaign percent diverges from target percent");
  double confidence_delta = campaign_confidence - status_target_percent;
  if (confidence_delta < -0.0001 || confidence_delta > 0.0001)
    (void)string_list_push_copy(errors,
                                "status campaign confidence diverges from target percent");
  double campaign_remaining = -1.0;
  if (!summary_number_from_report(json, "campaign_remaining_percent",
                                  &campaign_remaining) ||
      campaign_remaining < 0.0 || campaign_remaining > 100.0)
    (void)string_list_push_copy(errors,
                                "status campaign remaining percent is missing");
  double remaining_alias = -1.0;
  if (!summary_number_from_report(json, "remaining_percent",
                                  &remaining_alias) ||
      remaining_alias != campaign_remaining)
    (void)string_list_push_copy(errors,
                                "status remaining percent alias is wrong");
  double status_score = -1.0, stability_alias = -1.0;
  double language_score = -2.0, language_alias = -1.0;
  double good_threshold = -1.0;
  double signal_health = -1.0, evidence_cap = -1.0;
  double language_signal = -1.0, language_cap = -1.0;
  double freshness_penalty = -1.0;
  double language_gap = -1.0;
  double latest_age = -1.0, full_pressure_age = -1.0;
  bool latest_fresh = false, full_pressure_fresh = false, evidence_fresh = false;
  if (!summary_number_from_report(json, "stability_score_percent",
                                  &status_score) ||
      status_score < expected_score_min || status_score > expected_score_max)
    (void)string_list_push_copy(errors, "status stability score is wrong");
  double status_score_alias = -1.0;
  if (!summary_number_from_report(json, "score", &status_score_alias) ||
      status_score_alias != status_score)
    (void)string_list_push_copy(errors, "status compact score alias is wrong");
  if (!summary_number_from_report(json, "stability_score",
                                  &stability_alias) ||
      stability_alias != status_score)
    (void)string_list_push_copy(errors,
                                "status stability score alias is wrong");
  if (!summary_number_from_report(json, "language_score_percent",
                                  &language_score) ||
      language_score != status_score)
    (void)string_list_push_copy(errors, "status language score alias is wrong");
  if (!summary_number_from_report(json, "language_score",
                                  &language_alias) ||
      language_alias != language_score)
    (void)string_list_push_copy(errors,
                                "status language score compact alias is wrong");
  if (!summary_number_from_report(json,
                                  "language_score_good_threshold_percent",
                                  &good_threshold) ||
      good_threshold < 74.99 || good_threshold > 75.01)
    (void)string_list_push_copy(errors, "status language good threshold missing");
  if (!summary_number_from_report(json, "language_score_gap_percent",
                                  &language_gap) ||
      (stale_evidence ?
           (language_gap < 9.99 || language_gap > 10.01) :
           (language_gap < -0.01 || language_gap > 0.01)))
    (void)string_list_push_copy(errors, "status language score gap is wrong");
  if (!summary_number_from_report(json, "signal_health_percent",
                                  &signal_health) ||
      signal_health < 99.99 || signal_health > 100.01)
    (void)string_list_push_copy(errors, "status signal health is wrong");
  if (!summary_number_from_report(json, "evidence_cap_percent",
                                  &evidence_cap) ||
      evidence_cap < 84.99 || evidence_cap > 85.01)
    (void)string_list_push_copy(errors, "status evidence cap is wrong");
  if (!summary_number_from_report(json, "language_score_signal_percent",
                                  &language_signal) ||
      language_signal != signal_health)
    (void)string_list_push_copy(errors,
                                "status language signal alias is wrong");
  if (!summary_number_from_report(json,
                                  "language_score_evidence_cap_percent",
                                  &language_cap) ||
      language_cap != evidence_cap)
    (void)string_list_push_copy(errors,
                                "status language evidence cap alias is wrong");
  if (!summary_number_from_report(json, "freshness_penalty",
                                  &freshness_penalty) ||
      freshness_penalty != expected_freshness_penalty)
    (void)string_list_push_copy(errors, "status freshness penalty is wrong");
  double thread_years_per_run = -1.0, percent_per_run = -1.0;
  double next_stability_score = -1.0, next_stability_alias = -1.0;
  double next_language_score = -1.0, next_language_alias = -1.0;
  double next_language_delta = -999.0;
  double runs_to_good = -2.0, runs_to_good_days = -2.0;
  if (!summary_number_from_report(json, "thread_years_per_run",
                                  &thread_years_per_run) ||
      thread_years_per_run <= 0.0)
    (void)string_list_push_copy(errors,
                                "status thread-years-per-run missing");
  char *thread_years_per_run_source =
      summary_string_from_report(json, "thread_years_per_run_source");
  if (!thread_years_per_run_source ||
      strcmp(thread_years_per_run_source, "plan-rate") != 0)
    (void)string_list_push_copy(errors,
                                "status per-run projection source wrong");
  if (!summary_number_from_report(json, "target_percent_per_run",
                                  &percent_per_run) ||
      percent_per_run <= 0.0)
    (void)string_list_push_copy(errors, "status percent-per-run missing");
  if (!summary_number_from_report(json,
                                  "next_run_stability_score_percent",
                                  &next_stability_score) ||
      next_stability_score < status_score)
    (void)string_list_push_copy(errors,
                                "status next-run stability score missing");
  if (!summary_number_from_report(json,
                                  "next_run_stability_score",
                                  &next_stability_alias) ||
      next_stability_alias != next_stability_score)
    (void)string_list_push_copy(errors,
                                "status next-run stability alias missing");
  if (!summary_number_from_report(json,
                                  "next_run_language_score_percent",
                                  &next_language_score) ||
      next_language_score < status_score)
    (void)string_list_push_copy(errors,
                                "status next-run language score missing");
  if (!summary_number_from_report(json,
                                  "next_run_language_score",
                                  &next_language_alias) ||
      next_language_alias != next_language_score)
    (void)string_list_push_copy(errors,
                                "status next-run language alias missing");
  if (!summary_number_from_report(json,
                                  "next_run_language_score_delta_percent",
                                  &next_language_delta) ||
      (stale_evidence ? next_language_delta <= 0.0 :
           next_language_delta < -0.01))
    (void)string_list_push_copy(errors,
                                "status next-run language delta wrong");
  if (!summary_number_from_report(json, "runs_to_good_language_score",
                                  &runs_to_good) ||
      (stale_evidence ? runs_to_good != -1.0 : runs_to_good < 0.0))
    (void)string_list_push_copy(errors, "status runs-to-good missing");
  if (!summary_number_from_report(json, "runs_to_good_stability_score",
                                  &runs_to_good) ||
      (stale_evidence ? runs_to_good != -1.0 : runs_to_good < 0.0))
    (void)string_list_push_copy(errors,
                                "status runs-to-good stability alias missing");
  if (!summary_number_from_report(json, "runs_to_good_language_days",
                                  &runs_to_good_days) ||
      (stale_evidence ? runs_to_good_days != -1.0 : runs_to_good_days < 0.0))
    (void)string_list_push_copy(errors, "status days-to-good missing");
  if (!summary_number_from_report(json, "days_to_good_language_score",
                                  &runs_to_good_days) ||
      (stale_evidence ? runs_to_good_days != -1.0 : runs_to_good_days < 0.0))
    (void)string_list_push_copy(errors,
                                "status days-to-good language alias missing");
  if (!summary_number_from_report(json, "runs_to_good_stability_days",
                                  &runs_to_good_days) ||
      (stale_evidence ? runs_to_good_days != -1.0 : runs_to_good_days < 0.0))
    (void)string_list_push_copy(errors,
                                "status days-to-good stability alias missing");
  if (!summary_number_from_report(json, "days_to_good_stability",
                                  &runs_to_good_days) ||
      (stale_evidence ? runs_to_good_days != -1.0 : runs_to_good_days < 0.0))
    (void)string_list_push_copy(errors,
                                "status days-to-good stability short alias missing");
  if (!summary_number_from_report(json, "latest_report_age_seconds",
                                  &latest_age) ||
      (stale_evidence ?
           (latest_age < 24.0 * 3600.0 || latest_age > 90.0 * 3600.0) :
           (latest_age < 0.0 || latest_age > 60.0)))
    (void)string_list_push_copy(errors, "status latest report age is wrong");
  if (!summary_number_from_report(json,
                                  "latest_full_pressure_report_age_seconds",
                                  &full_pressure_age) ||
      (stale_evidence ?
           (full_pressure_age < 72.0 * 3600.0 ||
            full_pressure_age > 90.0 * 3600.0) :
           (full_pressure_age < 0.0 || full_pressure_age > 60.0)))
    (void)string_list_push_copy(errors,
                                "status full-pressure report age is wrong");
  double latest_h = -1.0, latest_over_h = -1.0;
  double full_h = -1.0, full_over_h = -1.0, over_h = -1.0;
  if (!summary_number_from_report(json, "latest_h", &latest_h) ||
      latest_h < 0.0)
    (void)string_list_push_copy(errors, "status compact latest_h missing");
  if (!summary_number_from_report(json, "latest_over_h", &latest_over_h) ||
      latest_over_h < 0.0)
    (void)string_list_push_copy(errors,
                                "status compact latest_over_h missing");
  if (!summary_number_from_report(json, "full_h", &full_h) || full_h < 0.0)
    (void)string_list_push_copy(errors, "status compact full_h missing");
  if (!summary_number_from_report(json, "full_over_h", &full_over_h) ||
      full_over_h < 0.0)
    (void)string_list_push_copy(errors,
                                "status compact full_over_h missing");
  if (!summary_number_from_report(json, "over_h", &over_h) ||
      over_h < latest_over_h || over_h < full_over_h)
    (void)string_list_push_copy(errors, "status compact over_h wrong");
  if (!summary_bool_from_report(json, "latest_report_fresh", &latest_fresh) ||
      latest_fresh != !stale_evidence ||
      !summary_bool_from_report(json,
                                "latest_full_pressure_report_fresh",
                                &full_pressure_fresh) ||
      full_pressure_fresh != !stale_evidence ||
      !summary_bool_from_report(json, "evidence_fresh", &evidence_fresh) ||
      evidence_fresh != !stale_evidence)
    (void)string_list_push_copy(errors, "status freshness flags are wrong");
  char *status_label = summary_string_from_report(json, "language_score_label");
  char *status_score_label = summary_string_from_report(json, "score_label");
  char *status_note = summary_string_from_report(json, "language_score_note");
  if (!status_label || strcmp(status_label, expected_label) != 0)
    (void)string_list_push_copy(errors, "status language score label is wrong");
  if (!status_score_label ||
      strcmp(status_score_label, expected_label) != 0)
    (void)string_list_push_copy(errors, "status compact score label wrong");
  if (!status_note ||
      (stale_evidence ?
           !strstr(status_note, "stale latest/full-pressure evidence") :
           (!strstr(status_note, "evidence cap") ||
            !strstr(status_note, "campaign evidence"))))
    (void)string_list_push_copy(errors, "status language score note is wrong");
  free(status_label);
  free(status_score_label);
  free(status_note);
  double coverage_lanes = -1.0, coverage_ran_lanes = -1.0;
  double coverage_depth = -1.0, coverage_percent = -1.0;
  double coverage_not_run = -1.0;
  double blocker_gaps = -1.0, coverage_failed_lanes = -1.0;
  double detail_count = -1.0, backlog_lanes = -1.0, detail_rows = -1.0;
  if (!summary_number_from_report(json, "coverage_lanes", &coverage_lanes) ||
      coverage_lanes < 0.0)
    (void)string_list_push_copy(errors, "coverage lane count is missing");
  if (!summary_number_from_report(json, "coverage_ran_lanes",
                                  &coverage_ran_lanes) ||
      coverage_ran_lanes < 0.0)
    (void)string_list_push_copy(errors, "coverage ran lane count is missing");
  if (!summary_number_from_report(json, "coverage_depth_percent",
                                  &coverage_depth) ||
      coverage_depth < 0.0)
    (void)string_list_push_copy(errors, "coverage depth percent is missing");
  if (!summary_number_from_report(json, "coverage_percent",
                                  &coverage_percent) ||
      coverage_percent != coverage_depth)
    (void)string_list_push_copy(errors,
                                "coverage percent alias is missing or wrong");
  if (!summary_number_from_report(json, "coverage_not_run_lanes",
                                  &coverage_not_run) ||
      coverage_not_run < 0.0)
    (void)string_list_push_copy(errors, "coverage not-run lane count is missing");
  if (coverage_lanes > 0.0 && coverage_ran_lanes >= 0.0 &&
      coverage_depth >= 0.0) {
    double expected_depth =
        fuzz_all_ratio_percent(coverage_ran_lanes, coverage_lanes);
    double depth_delta = coverage_depth - expected_depth;
    if (depth_delta < 0.0) depth_delta = -depth_delta;
    if (depth_delta > 0.02)
      (void)string_list_push_copy(errors, "coverage depth percent is wrong");
  }
  if (coverage_lanes >= 0.0 && coverage_ran_lanes >= 0.0 &&
      coverage_not_run >= 0.0 &&
      coverage_not_run != fuzz_all_not_run_lanes(coverage_ran_lanes,
                                                 coverage_lanes))
    (void)string_list_push_copy(errors, "coverage not-run lane count is wrong");
  if (!summary_number_from_report(json, "coverage_blocker_gaps", &blocker_gaps) ||
      blocker_gaps != 0.0)
    (void)string_list_push_copy(errors, "coverage blockers were not zero");
  if (!summary_number_from_report(json, "coverage_failed_lanes",
                                  &coverage_failed_lanes) ||
      coverage_failed_lanes != 0.0)
    (void)string_list_push_copy(errors, "coverage failed lanes were not zero");
  char *coverage_state = summary_string_from_report(json, "coverage_state");
  if (!coverage_state ||
      strcmp(coverage_state,
             fuzz_all_coverage_state(coverage_lanes, coverage_ran_lanes,
                                     blocker_gaps, coverage_failed_lanes)) != 0)
    (void)string_list_push_copy(errors, "coverage state is wrong");
  if (!summary_number_from_report(json, "coverage_detail_count", &detail_count) ||
      detail_count < 0.0)
    (void)string_list_push_copy(errors, "coverage detail rows were missing");
  else if (coverage_not_run > 0.0 && detail_count <= 0.0)
    (void)string_list_push_copy(errors,
                                "partial coverage had no detail rows");
  if (!summary_number_from_report(json, "coverage_backlog_lanes",
                                  &backlog_lanes) ||
      backlog_lanes < 0.0)
    (void)string_list_push_copy(errors,
                                "coverage backlog lane count is missing");
  else if (detail_count >= 0.0 && backlog_lanes != detail_count)
    (void)string_list_push_copy(errors,
                                "coverage backlog alias diverged");
  if (!summary_number_from_report(json, "coverage_detail_rows",
                                  &detail_rows) ||
      detail_rows < 0.0)
    (void)string_list_push_copy(errors,
                                "coverage raw detail row count is missing");
  else if (coverage_not_run > 0.0 && detail_rows < backlog_lanes)
    (void)string_list_push_copy(errors,
                                  "coverage raw detail rows below backlog lanes");
  double coverage_queue_count = -1.0, coverage_queue_primary = -1.0;
  double coverage_queue_advisory = -1.0;
  char *coverage_queue_lanes =
      summary_string_from_report(json, "coverage_queue_lanes");
  char *coverage_queue_json =
      summary_array_from_report(json, "coverage_queue");
  if (backlog_lanes > 0.0) {
    if (!summary_number_from_report(json, "coverage_queue_count",
                                    &coverage_queue_count) ||
        coverage_queue_count <= 0.0 ||
        coverage_queue_count != backlog_lanes)
      (void)string_list_push_copy(errors,
                                  "status coverage queue count missing");
    if (!summary_number_from_report(
            json, "coverage_queue_non_advisory_count",
            &coverage_queue_primary) ||
        coverage_queue_primary <= 0.0)
      (void)string_list_push_copy(errors,
                                  "status coverage primary queue missing");
    if (!summary_number_from_report(json, "coverage_queue_advisory_count",
                                    &coverage_queue_advisory) ||
        coverage_queue_advisory < 0.0)
      (void)string_list_push_copy(errors,
                                  "status coverage advisory queue missing");
    if (!coverage_queue_lanes || !*coverage_queue_lanes ||
        !strstr(coverage_queue_lanes, "afl"))
      (void)string_list_push_copy(errors,
                                  "status coverage queue lanes missing");
    if (!coverage_queue_json ||
        count_json_array_items(coverage_queue_json) <= 0 ||
        !strstr(coverage_queue_json, "\"lane\":\"afl\"") ||
        !strstr(coverage_queue_json, "\"command\":\"./build/nytrix"))
      (void)string_list_push_copy(errors,
                                  "status coverage queue array missing");
  }
  free(coverage_queue_lanes);
  free(coverage_queue_json);
  char *coverage_next_action =
      summary_string_from_report(json, "coverage_next_action");
  char *coverage_next_lane =
      summary_string_from_report(json, "coverage_next_lane");
  char *coverage_next_severity =
      summary_string_from_report(json, "coverage_next_severity");
  char *coverage_next_category =
      summary_string_from_report(json, "coverage_next_category");
  char *coverage_next_reason =
      summary_string_from_report(json, "coverage_next_reason");
  char *coverage_next_command =
      summary_string_from_report(json, "coverage_next_command");
  char *coverage_next_guarded_command =
      summary_string_from_report(json, "coverage_next_guarded_command");
  char *coverage_next_low_cpu_command =
      summary_string_from_report(json, "coverage_next_low_cpu_command");
  char *coverage_next_preview_command =
      summary_string_from_report(json, "coverage_next_preview_command");
  char *coverage_next_state_file =
      summary_string_from_report(json, "coverage_next_state_file");
  char *coverage_next_state_command =
      summary_string_from_report(json, "coverage_next_state_command");
  char *coverage_next_state_refresh_command =
      summary_string_from_report(json, "coverage_next_state_refresh_command");
  bool coverage_next_state_refresh_required = false;
  bool have_coverage_next_state_refresh_required =
      summary_bool_from_report(json,
                               "coverage_next_state_refresh_required",
                               &coverage_next_state_refresh_required);
  char *coverage_next_state_refresh_reason =
      summary_string_from_report(json,
                                 "coverage_next_state_refresh_reason");
  char *recommended_state_refresh_command =
      summary_string_from_report(json, "recommended_state_refresh_command");
  bool recommended_state_refresh_required = false;
  bool have_recommended_state_refresh_required =
      summary_bool_from_report(json,
                               "recommended_state_refresh_required",
                               &recommended_state_refresh_required);
  char *coverage_next_state =
      summary_string_from_report(json, "coverage_next_state");
  char *coverage_next_state_stale_reason =
      summary_string_from_report(json, "coverage_next_state_stale_reason");
  char *coverage_next_state_phase =
      summary_string_from_report(json, "coverage_next_state_phase");
  char *coverage_next_state_child_status =
      summary_string_from_report(json, "coverage_next_state_child_status");
  char *coverage_next_stop_file =
      summary_string_from_report(json, "coverage_next_stop_file");
  char *coverage_next_stop_command =
      summary_string_from_report(json, "coverage_next_stop_command");
  char *coverage_next_resume_command =
      summary_string_from_report(json, "coverage_next_resume_command");
  bool expected_coverage_next_state_refresh_required = false;
    if (backlog_lanes > 0.0) {
    if (!coverage_next_action ||
        strcmp(coverage_next_action, "run-missing-evidence") != 0)
      (void)string_list_push_copy(errors,
                                  "coverage next action was missing");
    if (!coverage_next_lane || !*coverage_next_lane)
      (void)string_list_push_copy(errors,
                                  "coverage next lane was missing");
    if (!coverage_next_severity || !*coverage_next_severity)
      (void)string_list_push_copy(errors,
                                  "coverage next severity was missing");
    if (!coverage_next_category || !*coverage_next_category)
      (void)string_list_push_copy(errors,
                                  "coverage next category was missing");
    if (!coverage_next_reason || !*coverage_next_reason)
      (void)string_list_push_copy(errors,
                                  "coverage next reason was missing");
    if (!coverage_next_command ||
        !strstr(coverage_next_command, "./build/nytrix"))
      (void)string_list_push_copy(errors,
                                  "coverage next command was missing");
    if (coverage_next_command &&
        strstr(coverage_next_command, "fuzz all run") &&
        coverage_next_lane && strcmp(coverage_next_lane, "afl") == 0 &&
        !strstr(coverage_next_command, "--only-lane afl"))
      (void)string_list_push_copy(errors,
                                  "coverage next AFL command was not focused");
    if (coverage_next_command &&
        strstr(coverage_next_command, "fuzz all run") &&
        (!coverage_next_guarded_command ||
         !strstr(coverage_next_guarded_command,
                 "run-missing-evidence.sh")))
      (void)string_list_push_copy(errors,
                                  "coverage next guarded command was missing");
       if (coverage_next_command &&
           strstr(coverage_next_command, "fuzz all run") &&
           (!coverage_next_low_cpu_command ||
            !strstr(coverage_next_low_cpu_command,
                    "env NYTRIX_LOW_PRIORITY=1") ||
            !strstr(coverage_next_low_cpu_command, "nice -n 10") ||
            strncmp(coverage_next_low_cpu_command,
                    "NYTRIX_LOW_PRIORITY=", 19) == 0 ||
            !strstr(coverage_next_low_cpu_command,
                    "NYTRIX_MISSING_EVIDENCE_HOURS=1") ||
            !strstr(coverage_next_low_cpu_command,
                    "NYTRIX_MISSING_EVIDENCE_THREADS=10%") ||
         !strstr(coverage_next_low_cpu_command,
                 "run-missing-evidence.sh")))
      (void)string_list_push_copy(errors,
                                  "coverage next low-cpu command was missing");
    if (coverage_next_command &&
        strstr(coverage_next_command, "fuzz all run") &&
        (!coverage_next_state_file ||
    !strstr(coverage_next_state_file,
            "run-missing-evidence-state.json") ||
    !coverage_next_state_command ||
    !strstr(coverage_next_state_command,
            "jq {state,event,live,fresh,child_status,") ||
            !strstr(coverage_next_state_command,
               "run-missing-evidence-state.json") ||
            !coverage_next_state_refresh_command ||
            !strstr(coverage_next_state_refresh_command,
                    "env NYTRIX_LOW_PRIORITY=1") ||
            !strstr(coverage_next_state_refresh_command, "nice -n 10") ||
            strncmp(coverage_next_state_refresh_command,
                    "NYTRIX_LOW_PRIORITY=", 19) == 0 ||
            !strstr(coverage_next_state_refresh_command,
                    "NYTRIX_RUN_DRY_RUN=1") ||
            !strstr(coverage_next_state_refresh_command,
                    "run-missing-evidence.sh") ||
         !coverage_next_stop_file ||
         !strstr(coverage_next_stop_file, "missing-evidence-stop") ||
         !coverage_next_stop_command ||
         !strstr(coverage_next_stop_command, "touch ") ||
         !strstr(coverage_next_stop_command, "missing-evidence-stop") ||
         !coverage_next_resume_command ||
         !strstr(coverage_next_resume_command, "rm -f ") ||
         !strstr(coverage_next_resume_command, "missing-evidence-stop")))
      (void)string_list_push_copy(
          errors, "coverage next guarded state or pause command was missing");
    if (coverage_next_command &&
        strstr(coverage_next_command, "fuzz all run") &&
        (!coverage_next_state || !*coverage_next_state ||
         !coverage_next_state_stale_reason ||
         !*coverage_next_state_stale_reason ||
         !coverage_next_state_child_status ||
         !*coverage_next_state_child_status ||
         !coverage_next_state_phase))
      (void)string_list_push_copy(
          errors, "coverage next parsed state fields were missing");
    bool coverage_next_state_live = false;
    bool have_coverage_next_state_live =
        summary_bool_from_report(json, "coverage_next_state_live",
                                 &coverage_next_state_live);
    expected_coverage_next_state_refresh_required =
        coverage_next_command && strstr(coverage_next_command, "fuzz all run") &&
        coverage_next_state_refresh_command &&
        *coverage_next_state_refresh_command &&
        (!have_coverage_next_state_live || !coverage_next_state_live) &&
        coverage_next_state_stale_reason &&
        *coverage_next_state_stale_reason &&
        strcmp(coverage_next_state_stale_reason, "none") != 0;
    if (expected_coverage_next_state_refresh_required &&
        (!have_coverage_next_state_refresh_required ||
         !coverage_next_state_refresh_required ||
         !coverage_next_state_refresh_reason ||
         strcmp(coverage_next_state_refresh_reason,
                coverage_next_state_stale_reason) != 0 ||
         !recommended_state_refresh_command ||
         !strstr(recommended_state_refresh_command,
                 coverage_next_state_refresh_command)))
      (void)string_list_push_copy(
          errors, "coverage next state refresh requirement was missing");
    if (coverage_next_command && coverage_next_guarded_command &&
        strstr(coverage_next_command, "fuzz all run") &&
        strstr(coverage_next_guarded_command, "run-missing-evidence.sh")) {
      char root[4096] = {0}, guarded_path[4096] = {0};
      if (find_nytrix_root(root, sizeof(root))) {
        const char *script_ref = coverage_next_guarded_command;
        if (strncmp(script_ref, "./", 2) == 0) script_ref += 2;
        if (path_is_absolute(script_ref))
          snprintf(guarded_path, sizeof(guarded_path), "%s", script_ref);
        else
          (void)path_join(guarded_path, sizeof(guarded_path), root,
                          script_ref);
      }
      file_buf_t guarded = {0};
      if (!guarded_path[0] || !read_file(guarded_path, &guarded) ||
          !guarded.data) {
        (void)string_list_push_copy(errors,
                                    "coverage next guarded script unreadable");
      } else if (!strstr(guarded.data, "nytrix_require_free_space") ||
                 !strstr(guarded.data, "nytrix_wait_for_load") ||
                 !strstr(guarded.data, "nytrix_acquire_campaign_lock") ||
                 !strstr(guarded.data, "nytrix_low_priority") ||
                 !strstr(guarded.data,
                         "NYTRIX_MISSING_EVIDENCE_STATE_FILE") ||
                 !strstr(guarded.data,
                         "NYTRIX_MISSING_EVIDENCE_STOP_FILE") ||
                 !strstr(guarded.data,
                         "NYTRIX_MISSING_EVIDENCE_HEARTBEAT_S") ||
                 !strstr(guarded.data,
                         "NYTRIX_MISSING_EVIDENCE_HOURS") ||
                 !strstr(guarded.data,
                         "NYTRIX_MISSING_EVIDENCE_THREADS") ||
                 !strstr(guarded.data,
                         "NYTRIX_MISSING_EVIDENCE_PROFILE") ||
                 !strstr(guarded.data,
                         "NYTRIX_MISSING_EVIDENCE_JSON") ||
                 !strstr(guarded.data,
                         "NYTRIX_MISSING_EVIDENCE_HEARTBEAT_S must be a non-negative integer") ||
                 !strstr(guarded.data, "invalid-budget") ||
                 !strstr(guarded.data, "\"state\"") ||
                 !strstr(guarded.data, "\"live\"") ||
                 !strstr(guarded.data, "\"fresh\"") ||
                 !strstr(guarded.data, "\"child_status\"") ||
                 !strstr(guarded.data, "\"low_priority\"") ||
                 !strstr(guarded.data, "\"nice\"") ||
                 !strstr(guarded.data, "\"load_wait\"") ||
                 !strstr(guarded.data, "\"space_guard\"") ||
                 !strstr(guarded.data, "\"run_lock\"") ||
                 !strstr(guarded.data, "\"hours\"") ||
                 !strstr(guarded.data, "\"threads\"") ||
                 !strstr(guarded.data, "\"profile\"") ||
                 !strstr(guarded.data, "\"json\"") ||
                 !strstr(guarded.data, "\"default_command\"") ||
                 !strstr(guarded.data, "nytrix_missing_command=(") ||
                 !strstr(guarded.data, "nytrix_missing_command_text") ||
                 !strstr(guarded.data, "nytrix_missing_write_state") ||
                 !strstr(guarded.data,
                         "nytrix_missing_run_with_heartbeat") ||
                 !strstr(guarded.data,
                         "nytrix_missing_run_with_heartbeat nytrix_low_priority \"${nytrix_missing_command[@]}\"") ||
                 !strstr(guarded.data, "stop requested before run") ||
                 !strstr(guarded.data, coverage_next_command) ||
                 !strstr(guarded.data,
                         "nytrix_low_priority ./build/nytrix fuzz all history") ||
                 !strstr(guarded.data,
                         "nytrix_low_priority ./build/nytrix fuzz all coverage --strict --history") ||
                 !strstr(guarded.data,
                         "nytrix_low_priority ./build/nytrix fuzz all worklist") ||
                 !strstr(guarded.data,
                         "nytrix_low_priority ./build/nytrix fuzz all plan") ||
                 !strstr(guarded.data,
                         "nytrix_low_priority ./build/nytrix fuzz all old-paths") ||
                 !strstr(guarded.data,
                         "nytrix_low_priority ./build/nytrix fuzz all status") ||
                 !strstr(guarded.data,
                         "nytrix_low_priority ./build/nytrix fuzz all progress") ||
                 !strstr(guarded.data, "fuzz all status --strict") ||
                 !strstr(guarded.data, "fuzz all progress --strict")) {
        (void)string_list_push_copy(errors,
                                    "coverage next guarded script lost guards or refresh");
      }
      free(guarded.data);
    }
    if (coverage_next_command &&
        strstr(coverage_next_command, "fuzz all run") &&
           (!coverage_next_preview_command ||
            !strstr(coverage_next_preview_command, "fuzz all preflight") ||
            (coverage_next_lane && strcmp(coverage_next_lane, "afl") == 0 &&
             !strstr(coverage_next_preview_command, "--only-lane afl")) ||
            !strstr(coverage_next_preview_command,
                    "env NYTRIX_LOW_PRIORITY=1") ||
            !strstr(coverage_next_preview_command, "nice -n 10") ||
            strncmp(coverage_next_preview_command,
                    "NYTRIX_LOW_PRIORITY=", 19) == 0 ||
            !strstr(coverage_next_preview_command,
                    "--allow-dirty-nytrix-baseline") ||
            !strstr(coverage_next_preview_command, "build/cache/scratch")))
      (void)string_list_push_copy(errors,
                                  "coverage next preflight preview was missing");
    } else if (coverage_next_action &&
               strcmp(coverage_next_action, "none") != 0) {
      (void)string_list_push_copy(errors,
                                  "coverage next action should be none");
    }
      double advisory_gaps = -1.0, disabled_lanes = -1.0;
  double budget_short_lanes = -1.0, missing_tool_lanes = -1.0;
  double reports_considered = -1.0, campaign_reports_considered = -1.0;
  double companion_reports_considered = -1.0;
  if (!summary_number_from_report(json, "coverage_advisory_gaps",
                                  &advisory_gaps) ||
      advisory_gaps < 0.0)
    (void)string_list_push_copy(errors, "coverage advisory gap count is missing");
  if (!summary_number_from_report(json, "coverage_disabled_lanes",
                                  &disabled_lanes) ||
      disabled_lanes < 0.0)
    (void)string_list_push_copy(errors, "coverage disabled lane count is missing");
  if (!summary_number_from_report(json, "coverage_budget_short_lanes",
                                  &budget_short_lanes) ||
      budget_short_lanes < 0.0)
    (void)string_list_push_copy(errors, "coverage budget-short lane count is missing");
  if (!summary_number_from_report(json, "coverage_missing_tool_lanes",
                                  &missing_tool_lanes) ||
      missing_tool_lanes < 0.0)
    (void)string_list_push_copy(errors, "coverage missing-tool lane count is missing");
  if (!summary_number_from_report(json, "coverage_reports_considered",
                                  &reports_considered) ||
      reports_considered < 0.0)
    (void)string_list_push_copy(errors, "coverage report count is missing");
  if (!summary_number_from_report(json,
                                  "coverage_campaign_reports_considered",
                                  &campaign_reports_considered) ||
      campaign_reports_considered < 0.0)
    (void)string_list_push_copy(errors,
                                "coverage campaign report count is missing");
  if (!summary_number_from_report(json,
                                  "coverage_companion_reports_considered",
                                  &companion_reports_considered) ||
      companion_reports_considered < 0.0)
    (void)string_list_push_copy(errors, "coverage companion report count is missing");
  double expected_total =
      campaign_reports_considered + companion_reports_considered;
  double total_delta = reports_considered - expected_total;
  if (total_delta < 0.0) total_delta = -total_delta;
  if (reports_considered >= 0.0 && expected_total >= 0.0 &&
      total_delta > 0.0001)
    (void)string_list_push_copy(errors,
                                "coverage report count split is inconsistent");
  char *coverage_report_path =
      summary_string_from_report(json, "coverage_report");
  if (!coverage_report_path || !strstr(coverage_report_path, expected_coverage_json)) {
    (void)string_list_push_copy(errors,
                                "status coverage report path lost expected cockpit");
  } else {
    char coverage_abs[4096] = {0};
    if (path_is_absolute(coverage_report_path)) {
      snprintf(coverage_abs, sizeof(coverage_abs), "%s", coverage_report_path);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(coverage_abs, sizeof(coverage_abs), root,
                        coverage_report_path);
    }
    file_buf_t coverage_report = {0};
    if (!coverage_abs[0] || !read_file(coverage_abs, &coverage_report) ||
        !coverage_report.data) {
      (void)string_list_push_copy(errors,
                                  "coverage report was not readable");
    } else {
      char *coverage_report_state =
          summary_string_from_report(coverage_report.data, "coverage_state");
      bool coverage_report_strict = false;
      double coverage_report_depth = -1.0, coverage_report_percent = -1.0;
      double coverage_report_not_run = -1.0;
      double coverage_report_advisory_gaps = -1.0;
      double coverage_report_blocker_gaps = -1.0;
      double coverage_report_disabled_lanes = -1.0;
      double coverage_report_budget_short_lanes = -1.0;
      double coverage_report_missing_tool_lanes = -1.0;
      double coverage_report_reports_considered = -1.0;
      double coverage_report_campaign_reports_considered = -1.0;
      double coverage_report_companion_reports_considered = -1.0;
      double coverage_report_detail_rows = -1.0;
      if (!summary_bool_from_report(coverage_report.data, "strict",
                                    &coverage_report_strict) ||
          !coverage_report_strict)
        (void)string_list_push_copy(errors,
                                    "coverage report strict flag was not preserved");
      if (!coverage_report_state || !coverage_state ||
          strcmp(coverage_report_state, coverage_state) != 0)
        (void)string_list_push_copy(errors,
                                    "coverage report state diverged from status");
      if (!summary_number_from_report(coverage_report.data,
                                      "coverage_depth_percent",
                                      &coverage_report_depth) ||
          coverage_report_depth < 0.0)
        (void)string_list_push_copy(errors,
                                    "coverage report depth alias missing");
      if (!summary_number_from_report(coverage_report.data,
                                      "coverage_percent",
                                      &coverage_report_percent) ||
          coverage_report_percent != coverage_report_depth)
        (void)string_list_push_copy(errors,
                                    "coverage report percent alias wrong");
      if (!summary_number_from_report(coverage_report.data,
                                      "coverage_not_run_lanes",
                                      &coverage_report_not_run) ||
          coverage_report_not_run < 0.0)
        (void)string_list_push_copy(errors,
                                    "coverage report not-run alias missing");
      if (!summary_number_from_report(coverage_report.data, "advisory_gaps",
                                      &coverage_report_advisory_gaps) ||
          coverage_report_advisory_gaps < 0.0)
        (void)string_list_push_copy(errors,
                                    "coverage report advisory gap alias missing");
      if (!summary_number_from_report(coverage_report.data,
                                      "coverage_blocker_gaps",
                                      &coverage_report_blocker_gaps) ||
          coverage_report_blocker_gaps < 0.0)
        (void)string_list_push_copy(errors,
                                    "coverage report blocker gap alias missing");
      if (!summary_number_from_report(coverage_report.data,
                                      "coverage_advisory_gaps",
                                      &coverage_report_advisory_gaps) ||
          coverage_report_advisory_gaps < 0.0)
        (void)string_list_push_copy(errors,
                                    "coverage report prefixed advisory gap alias missing");
      if (!summary_number_from_report(coverage_report.data,
                                      "coverage_disabled_lanes",
                                      &coverage_report_disabled_lanes) ||
          coverage_report_disabled_lanes < 0.0)
        (void)string_list_push_copy(errors,
                                    "coverage report disabled lane alias missing");
      if (!summary_number_from_report(coverage_report.data,
                                      "coverage_budget_short_lanes",
                                      &coverage_report_budget_short_lanes) ||
          coverage_report_budget_short_lanes < 0.0)
        (void)string_list_push_copy(errors,
                                    "coverage report budget-short lane alias missing");
      if (!summary_number_from_report(coverage_report.data,
                                      "coverage_missing_tool_lanes",
                                      &coverage_report_missing_tool_lanes) ||
          coverage_report_missing_tool_lanes < 0.0)
        (void)string_list_push_copy(errors,
                                    "coverage report missing-tool lane alias missing");
      if (!summary_number_from_report(coverage_report.data,
                                      "coverage_reports_considered",
                                      &coverage_report_reports_considered) ||
          coverage_report_reports_considered < 0.0)
        (void)string_list_push_copy(errors,
                                    "coverage report considered-count alias missing");
      if (!summary_number_from_report(
              coverage_report.data, "coverage_campaign_reports_considered",
              &coverage_report_campaign_reports_considered) ||
          coverage_report_campaign_reports_considered < 0.0)
        (void)string_list_push_copy(
            errors, "coverage report campaign-count alias missing");
      if (!summary_number_from_report(
              coverage_report.data, "coverage_companion_reports_considered",
              &coverage_report_companion_reports_considered) ||
          coverage_report_companion_reports_considered < 0.0)
        (void)string_list_push_copy(
            errors, "coverage report companion-count alias missing");
      if (!summary_number_from_report(coverage_report.data,
                                      "coverage_detail_rows",
                                      &coverage_report_detail_rows) ||
          coverage_report_detail_rows < 0.0)
        (void)string_list_push_copy(errors,
                                    "coverage report detail rows missing");
      else if (coverage_report_not_run > 0.0 &&
               (coverage_report_detail_rows <= 0.0 ||
                !strstr(coverage_report.data,
                        "\"kind\":\"fuzz-all-coverage-detail\"")))
        (void)string_list_push_copy(errors,
                                    "partial coverage report detail rows missing");
      double coverage_depth_delta = coverage_report_depth - coverage_depth;
      if (coverage_depth_delta < 0.0) coverage_depth_delta = -coverage_depth_delta;
      if (coverage_report_depth >= 0.0 && coverage_depth >= 0.0 &&
          coverage_depth_delta > 0.02)
        (void)string_list_push_copy(errors,
                                    "coverage report depth diverged from status");
      if (coverage_report_not_run >= 0.0 && coverage_not_run >= 0.0 &&
          coverage_report_not_run != coverage_not_run)
        (void)string_list_push_copy(errors,
                                    "coverage report not-run diverged from status");
      if (coverage_report_blocker_gaps >= 0.0 && blocker_gaps >= 0.0 &&
          coverage_report_blocker_gaps != blocker_gaps)
        (void)string_list_push_copy(errors,
                                    "coverage report blocker gaps diverged from status");
      if (coverage_report_advisory_gaps >= 0.0 && advisory_gaps >= 0.0 &&
          coverage_report_advisory_gaps != advisory_gaps)
        (void)string_list_push_copy(errors,
                                    "coverage report advisory gaps diverged from status");
      if (coverage_report_disabled_lanes >= 0.0 && disabled_lanes >= 0.0 &&
          coverage_report_disabled_lanes != disabled_lanes)
        (void)string_list_push_copy(errors,
                                    "coverage report disabled lanes diverged from status");
      if (coverage_report_budget_short_lanes >= 0.0 &&
          budget_short_lanes >= 0.0 &&
          coverage_report_budget_short_lanes != budget_short_lanes)
        (void)string_list_push_copy(
            errors, "coverage report budget-short lanes diverged from status");
      if (coverage_report_missing_tool_lanes >= 0.0 &&
          missing_tool_lanes >= 0.0 &&
          coverage_report_missing_tool_lanes != missing_tool_lanes)
        (void)string_list_push_copy(
            errors, "coverage report missing-tool lanes diverged from status");
      if (coverage_report_reports_considered >= 0.0 &&
          reports_considered >= 0.0 &&
          coverage_report_reports_considered != reports_considered)
        (void)string_list_push_copy(
            errors, "coverage report considered count diverged from status");
      if (coverage_report_campaign_reports_considered >= 0.0 &&
          campaign_reports_considered >= 0.0 &&
          coverage_report_campaign_reports_considered !=
              campaign_reports_considered)
        (void)string_list_push_copy(
            errors, "coverage report campaign count diverged from status");
      if (coverage_report_companion_reports_considered >= 0.0 &&
          companion_reports_considered >= 0.0 &&
          coverage_report_companion_reports_considered !=
              companion_reports_considered)
        (void)string_list_push_copy(
            errors, "coverage report companion count diverged from status");
      char *coverage_markdown_path =
          summary_string_from_report(coverage_report.data, "markdown");
      if (!coverage_markdown_path ||
          !strstr(coverage_markdown_path, expected_coverage_md)) {
        (void)string_list_push_copy(errors,
                                    "coverage markdown path lost expected cockpit");
      } else {
        char coverage_md_abs[4096] = {0};
        if (path_is_absolute(coverage_markdown_path)) {
          snprintf(coverage_md_abs, sizeof(coverage_md_abs), "%s",
                   coverage_markdown_path);
        } else {
          char root[4096];
          if (find_nytrix_root(root, sizeof(root)))
            (void)path_join(coverage_md_abs, sizeof(coverage_md_abs), root,
                            coverage_markdown_path);
        }
        file_buf_t coverage_md = {0};
        if (!coverage_md_abs[0] || !read_file(coverage_md_abs, &coverage_md) ||
            !coverage_md.data) {
          (void)string_list_push_copy(errors,
                                      "coverage markdown was not readable");
        } else if (!strstr(coverage_md.data, "Coverage:") ||
                   !strstr(coverage_md.data, coverage_report_state ?
                                            coverage_report_state : "") ||
                   !strstr(coverage_md.data, "not-run") ||
                   !strstr(coverage_md.data, "disabled") ||
                   !strstr(coverage_md.data, "budget-short") ||
                   !strstr(coverage_md.data, "missing-tool") ||
                   !strstr(coverage_md.data, "advisory gaps")) {
          (void)string_list_push_copy(errors,
                                      "coverage markdown omits compact state");
        } else if (coverage_report_not_run > 0.0 &&
                   (!strstr(coverage_md.data, "Coverage Backlog") ||
                    !strstr(coverage_md.data, "Command:"))) {
          (void)string_list_push_copy(errors,
                                      "partial coverage markdown omits backlog");
        } else if (!strstr(coverage_md.data,
                           "fuzz all coverage --strict --history") ||
                   !strstr(coverage_md.data, "--target-thread-years 0.001") ||
                   !strstr(coverage_md.data, "--hours 1") ||
                   !strstr(coverage_md.data, "--threads 1") ||
                   !strstr(coverage_md.data, expected_coverage_json) ||
                   !strstr(coverage_md.data, expected_coverage_md)) {
          (void)string_list_push_copy(errors,
                                      "coverage markdown omits self-refresh command");
        } else if (!strstr(coverage_md.data,
                           "fuzz all status --refresh --strict --allow-full-pressure-remediation --dir") ||
                   !strstr(coverage_md.data, "--history") ||
                   !strstr(coverage_md.data, "--worklist") ||
                   !strstr(coverage_md.data, "--coverage") ||
                   !strstr(coverage_md.data, "--plan") ||
                   !strstr(coverage_md.data, "--target-thread-years 0.001") ||
                   !strstr(coverage_md.data, "--hours 1") ||
                   !strstr(coverage_md.data, "--threads 1") ||
                   !strstr(coverage_md.data, expected_status_json) ||
                   !strstr(coverage_md.data, expected_status_md)) {
          (void)string_list_push_copy(errors,
                                      "coverage markdown omits full status refresh command");
        }
        free(coverage_md.data);
      }
      free(coverage_markdown_path);
      free(coverage_report_state);
    }
    free(coverage_report.data);
  }
  free(coverage_report_path);
  bool compiler_std_audit_readable = false;
  char *compiler_std_audit_report =
      summary_string_from_report(json, "compiler_std_audit_report");
  char *compiler_std_audit_markdown =
      summary_string_from_report(json, "compiler_std_audit_markdown");
  char *compiler_std_audit_command =
      summary_string_from_report(json, "compiler_std_audit_command");
  char *status_runtime_state =
      summary_string_from_report(json, "runtime_surface_state");
  char *status_crt_state =
      summary_string_from_report(json, "crt_surface_state");
  char *status_runtime_scope =
      summary_string_from_report(json, "runtime_surface_scope");
  char *status_crt_scope =
      summary_string_from_report(json, "crt_surface_scope");
  char *status_crt_behavior_state =
      summary_string_from_report(json, "crt_behavior_state");
  char *status_crt_behavior_scope =
      summary_string_from_report(json, "crt_behavior_scope");
  char *status_crt_behavior_next_action =
      summary_string_from_report(json, "crt_behavior_next_action");
  char *status_crt_behavior_next_reason =
      summary_string_from_report(json, "crt_behavior_next_reason");
  char *status_crt_behavior_next_command =
      summary_string_from_report(json, "crt_behavior_next_command");
  char *status_crt_top_family =
      summary_string_from_report(json, "crt_top_unreferenced_family");
  char *status_crt_families =
      summary_array_from_report(json, "crt_unreferenced_families");
  char *status_crt_next_action =
      summary_string_from_report(json, "crt_next_action");
  char *status_crt_next_reason =
      summary_string_from_report(json, "crt_next_reason");
  char *status_crt_next_family =
      summary_string_from_report(json, "crt_next_unreferenced_family");
  char *status_crt_next_exports =
      summary_array_from_report(json, "crt_next_unreferenced_exports");
  char *status_crt_next_definition_file =
      summary_string_from_report(json, "crt_next_definition_file");
  char *status_crt_next_definition_locations =
      summary_array_from_report(json, "crt_next_definition_locations");
  char *status_crt_next_inspect_command =
      summary_string_from_report(json, "crt_next_inspect_command");
  double status_runtime_exports = -1.0;
  double status_direct_runtime_refs = -1.0;
  double status_runtime_coverage_done = -1.0;
  double status_runtime_coverage_total = -1.0;
  double status_runtime_coverage = -1.0;
  double status_crt_runtime_exports = -1.0;
  double status_crt_direct_refs = -1.0;
  double status_crt_coverage_done = -1.0;
  double status_crt_coverage_total = -1.0;
  double status_crt_coverage = -1.0;
  double status_crt_unreferenced = -1.0;
  double status_crt_unreferenced_percent = -1.0;
  double status_crt_family_count = -1.0;
  double status_crt_top_family_count = -1.0;
  double status_crt_next_count = -1.0;
  if (!summary_bool_from_report(json, "compiler_std_audit_readable",
                                &compiler_std_audit_readable))
    (void)string_list_push_copy(errors,
                                "status compiler std-audit readable flag missing");
  if (!compiler_std_audit_report ||
      !strstr(compiler_std_audit_report, "compiler-std-audit.json"))
    (void)string_list_push_copy(errors,
                                "status compiler std-audit report path missing");
  if (!compiler_std_audit_markdown ||
      !strstr(compiler_std_audit_markdown, "compiler-std-audit.md"))
    (void)string_list_push_copy(errors,
                                "status compiler std-audit markdown path missing");
  if (!compiler_std_audit_command ||
      !selftest_command_uses_env_nice(compiler_std_audit_command) ||
      !strstr(compiler_std_audit_command, "compiler std-audit --json") ||
      !strstr(compiler_std_audit_command, "compiler-std-audit.json") ||
      !strstr(compiler_std_audit_command, "compiler-std-audit.md"))
    (void)string_list_push_copy(errors,
                                "status compiler std-audit command missing low-priority guard");
  if (!status_runtime_state || !*status_runtime_state ||
      !status_crt_state || !*status_crt_state)
    (void)string_list_push_copy(errors,
                                "status compiler std-audit states missing");
  if (!status_runtime_scope ||
      strcmp(status_runtime_scope, NYTRIX_RUNTIME_SURFACE_SCOPE) != 0 ||
      !status_crt_scope ||
      strcmp(status_crt_scope, NYTRIX_CRT_SURFACE_SCOPE) != 0 ||
      !status_crt_behavior_state ||
      strcmp(status_crt_behavior_state, NYTRIX_CRT_BEHAVIOR_STATE) != 0 ||
      !status_crt_behavior_scope ||
      strcmp(status_crt_behavior_scope, NYTRIX_CRT_BEHAVIOR_SCOPE) != 0)
    (void)string_list_push_copy(errors,
                                "status CRT claim scope missing");
  if (!status_crt_behavior_next_action ||
      strcmp(status_crt_behavior_next_action,
             NYTRIX_CRT_BEHAVIOR_NEXT_ACTION) != 0 ||
      !status_crt_behavior_next_reason ||
      !strstr(status_crt_behavior_next_reason, "campaign-gated") ||
      !status_crt_behavior_next_command ||
      !selftest_command_uses_env_nice(status_crt_behavior_next_command) ||
      !strstr(status_crt_behavior_next_command,
              "./build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "status CRT behavior next action missing");
  if (!summary_number_from_report(json, "runtime_export_coverage_percent",
                                  &status_runtime_coverage) ||
      status_runtime_coverage < 0.0)
    (void)string_list_push_copy(errors,
                                "status runtime coverage percent missing");
  bool status_runtime_counts_ok =
      summary_number_from_report(json, "runtime_exports",
                                 &status_runtime_exports) &&
      summary_number_from_report(json, "direct_runtime_refs",
                                 &status_direct_runtime_refs) &&
      summary_number_from_report(json, "runtime_coverage_done",
                                 &status_runtime_coverage_done) &&
      summary_number_from_report(json, "runtime_coverage_total",
                                 &status_runtime_coverage_total);
  if (!status_runtime_counts_ok ||
      status_runtime_coverage_done != status_direct_runtime_refs ||
      status_runtime_coverage_total != status_runtime_exports ||
      status_direct_runtime_refs < 0.0 ||
      status_runtime_exports < 0.0 ||
      status_direct_runtime_refs > status_runtime_exports ||
      (compiler_std_audit_readable && status_runtime_exports <= 0.0))
    (void)string_list_push_copy(errors,
                                "status runtime coverage counts missing");
  if (!summary_number_from_report(json, "crt_export_coverage_percent",
                                  &status_crt_coverage) ||
      status_crt_coverage < 0.0)
    (void)string_list_push_copy(errors, "status CRT coverage percent missing");
  bool status_crt_counts_ok =
      summary_number_from_report(json, "crt_runtime_exports",
                                 &status_crt_runtime_exports) &&
      summary_number_from_report(json, "crt_direct_refs",
                                 &status_crt_direct_refs) &&
      summary_number_from_report(json, "crt_coverage_done",
                                 &status_crt_coverage_done) &&
      summary_number_from_report(json, "crt_coverage_total",
                                 &status_crt_coverage_total);
  if (!status_crt_counts_ok ||
      status_crt_coverage_done != status_crt_direct_refs ||
      status_crt_coverage_total != status_crt_runtime_exports ||
      status_crt_direct_refs < 0.0 ||
      status_crt_runtime_exports < 0.0 ||
      status_crt_direct_refs > status_crt_runtime_exports ||
      (compiler_std_audit_readable && status_crt_runtime_exports <= 0.0))
    (void)string_list_push_copy(errors, "status CRT coverage counts missing");
  if (!summary_number_from_report(json, "crt_unreferenced_count",
                                  &status_crt_unreferenced) ||
      status_crt_unreferenced < 0.0)
    (void)string_list_push_copy(errors, "status CRT unreferenced count missing");
  if (!summary_number_from_report(json, "crt_unreferenced_percent",
                                  &status_crt_unreferenced_percent) ||
      status_crt_unreferenced_percent < 0.0)
    (void)string_list_push_copy(errors,
                                "status CRT unreferenced percent missing");
  if (status_crt_unreferenced > 0.0 &&
      (!status_runtime_state || strcmp(status_runtime_state, "partial") != 0 ||
       !status_crt_state || strcmp(status_crt_state, "partial") != 0))
    (void)string_list_push_copy(errors,
                                "status CRT surface state overclaimed clean");
  if (!summary_number_from_report(json, "crt_unreferenced_family_count",
                                  &status_crt_family_count) ||
      status_crt_family_count < 0.0)
    (void)string_list_push_copy(errors, "status CRT family count missing");
  if (!summary_number_from_report(json,
                                  "crt_top_unreferenced_family_count",
                                  &status_crt_top_family_count) ||
      status_crt_top_family_count < 0.0)
    (void)string_list_push_copy(errors,
                                "status CRT top family count missing");
  if (compiler_std_audit_readable && status_crt_family_count > 0.0 &&
      (!status_crt_top_family || !*status_crt_top_family ||
       !status_crt_families || !strstr(status_crt_families, "\"family\"") ||
       !strstr(status_crt_families, "\"count\"")))
    (void)string_list_push_copy(errors,
                                "status CRT family summary missing");
  if (compiler_std_audit_readable && status_crt_family_count > 0.0 &&
      (!status_crt_next_action ||
       strcmp(status_crt_next_action, "cover-unreferenced-family") != 0 ||
       !status_crt_next_reason || !strstr(status_crt_next_reason, "largest") ||
       !status_crt_next_family || !*status_crt_next_family ||
       (status_crt_top_family && *status_crt_top_family &&
        strcmp(status_crt_next_family, status_crt_top_family) != 0) ||
       !summary_number_from_report(json, "crt_next_unreferenced_count",
                                   &status_crt_next_count) ||
       status_crt_next_count != status_crt_top_family_count ||
       !status_crt_next_exports || !strstr(status_crt_next_exports, "\"__") ||
       !status_crt_next_definition_file ||
       !strstr(status_crt_next_definition_file, "src/rt/defs.h") ||
       !status_crt_next_definition_locations ||
       !strstr(status_crt_next_definition_locations, "\"line\"") ||
       !strstr(status_crt_next_definition_locations, "\"signature\"") ||
       !status_crt_next_inspect_command ||
       !strstr(status_crt_next_inspect_command, "sed -n") ||
       !strstr(status_crt_next_inspect_command, "rg -n")))
    (void)string_list_push_copy(errors,
                                "status CRT next action missing");
  if (status_crt_families &&
      (strstr(status_crt_families, "/home/e/nytrix/build/cache/projects/test") ||
       strstr(status_crt_families, "/home/e/nytrix/fuzz")))
    (void)string_list_push_copy(errors,
                                "status CRT family summary leaked stale paths");
  free(compiler_std_audit_report);
  free(compiler_std_audit_markdown);
  free(compiler_std_audit_command);
  free(status_runtime_state);
  free(status_crt_state);
  free(status_runtime_scope);
  free(status_crt_scope);
  free(status_crt_behavior_state);
  free(status_crt_behavior_scope);
  free(status_crt_behavior_next_action);
  free(status_crt_behavior_next_reason);
  free(status_crt_behavior_next_command);
  free(status_crt_top_family);
  free(status_crt_families);
  free(status_crt_next_action);
  free(status_crt_next_reason);
  free(status_crt_next_family);
  free(status_crt_next_exports);
  free(status_crt_next_definition_file);
  free(status_crt_next_definition_locations);
  free(status_crt_next_inspect_command);
  bool ready = false, full_pressure_ready = false, strict = false;
  bool target_reached = false, campaign_complete = false;
  bool allow_full_pressure_remediation = true;
  bool latest_report_clean = false, latest_full_pressure_clean = false;
  bool latest_full_pressure_ok = false;
  bool latest_report_demoted_non_reproducing_afl_timeout = false;
  bool latest_full_pressure_demoted_non_reproducing_afl_timeout = false;
  double latest_full_pressure_failure_count = -1.0;
  double status_blocker_count = -1.0, status_active_items = -1.0;
  double status_blockers_alias = -1.0, status_active_count_alias = -1.0;
  double status_active_runs_alias = -1.0;
  (void)summary_number_from_report(json, "blocker_count",
                                   &status_blocker_count);
  (void)summary_number_from_report(json, "active_items",
                                   &status_active_items);
  (void)summary_number_from_report(json, "blockers", &status_blockers_alias);
  (void)summary_number_from_report(json, "active_count",
                                   &status_active_count_alias);
  (void)summary_number_from_report(json, "active_runs",
                                   &status_active_runs_alias);
  if (!summary_bool_from_report(json, "ready", &ready) || !ready)
    (void)string_list_push_copy(errors, "status ready gate was false");
  if (!summary_bool_from_report(json, "target_reached", &target_reached))
    (void)string_list_push_copy(errors, "target reached flag is missing");
  if (!summary_bool_from_report(json, "campaign_complete", &campaign_complete))
    (void)string_list_push_copy(errors, "campaign complete flag is missing");
  if (campaign_complete != (ready && target_reached))
    (void)string_list_push_copy(errors, "campaign complete flag does not match ready target state");
  char *campaign_state = summary_string_from_report(json, "campaign_state");
  char *campaign_reason =
      summary_string_from_report(json, "campaign_incomplete_reason");
  char *completion_state = summary_string_from_report(json, "completion_state");
  char *completion_reason =
      summary_string_from_report(json, "completion_reason");
  if (!campaign_state ||
      strcmp(campaign_state,
             fuzz_all_campaign_state(ready, target_reached,
                                     campaign_complete)) != 0)
    (void)string_list_push_copy(errors, "status campaign state wrong");
  if (!campaign_reason ||
      strcmp(campaign_reason,
             fuzz_all_campaign_incomplete_reason(ready, status_blocker_count,
                                                 status_active_items,
                                                 target_reached,
                                                 campaign_complete)) != 0)
    (void)string_list_push_copy(errors,
                                "status campaign incomplete reason wrong");
  if (!completion_state || !campaign_state ||
      strcmp(completion_state, campaign_state) != 0)
    (void)string_list_push_copy(errors,
                                "status completion state alias wrong");
  if (!completion_reason || !campaign_reason ||
      strcmp(completion_reason, campaign_reason) != 0)
    (void)string_list_push_copy(errors,
                                "status completion reason alias wrong");
  if (status_blockers_alias != status_blocker_count)
    (void)string_list_push_copy(errors, "status blockers alias wrong");
  if (status_active_count_alias != status_active_items)
    (void)string_list_push_copy(errors, "status active_count alias wrong");
  if (status_active_runs_alias != status_active_items)
    (void)string_list_push_copy(errors, "status active_runs alias wrong");
  free(campaign_state);
  free(campaign_reason);
  free(completion_state);
  free(completion_reason);
  if (!summary_bool_from_report(json, "full_pressure_ready", &full_pressure_ready) ||
      !full_pressure_ready)
    (void)string_list_push_copy(errors, "full-pressure coverage gate was false");
  if (!summary_bool_from_report(json, "strict", &strict) || !strict)
    (void)string_list_push_copy(errors, "strict status flag was not preserved");
  if (!summary_bool_from_report(json, "allow_full_pressure_remediation",
                                &allow_full_pressure_remediation) ||
      allow_full_pressure_remediation)
    (void)string_list_push_copy(errors, "status full-pressure remediation allowance leaked into normal reporting");
  if (!summary_bool_from_report(json, "latest_report_clean",
                                &latest_report_clean) ||
      !latest_report_clean)
    (void)string_list_push_copy(errors, "latest report clean gate was false");
  double full_pressure_reports = -1.0;
  if (!summary_number_from_report(json, "full_pressure_reports",
                                  &full_pressure_reports) ||
      full_pressure_reports < 1.0)
    (void)string_list_push_copy(errors, "full-pressure report count is missing");
  if (!summary_bool_from_report(json, "latest_full_pressure_clean",
                                &latest_full_pressure_clean) ||
      !latest_full_pressure_clean)
    (void)string_list_push_copy(errors, "latest full-pressure clean gate was false");
  if (!summary_bool_from_report(json, "latest_full_pressure_ok",
                                &latest_full_pressure_ok))
    (void)string_list_push_copy(errors, "latest full-pressure raw ok gate is missing");
  bool latest_full_pressure_raw_ok_alias = false;
  bool latest_full_pressure_effective_clean_alias = false;
  if (!summary_bool_from_report(json, "latest_full_pressure_raw_ok",
                                &latest_full_pressure_raw_ok_alias) ||
      latest_full_pressure_raw_ok_alias != latest_full_pressure_ok)
    (void)string_list_push_copy(errors,
                                "latest full-pressure raw ok alias wrong");
  if (!summary_bool_from_report(json, "latest_full_pressure_effective_clean",
                                &latest_full_pressure_effective_clean_alias) ||
      latest_full_pressure_effective_clean_alias != latest_full_pressure_clean)
    (void)string_list_push_copy(errors,
                                "latest full-pressure effective clean alias wrong");
  if (!summary_number_from_report(json, "latest_full_pressure_failure_count",
                                  &latest_full_pressure_failure_count) ||
      latest_full_pressure_failure_count < 0.0)
    (void)string_list_push_copy(errors,
                                "latest full-pressure failure count is missing");
  if (!summary_bool_from_report(
          json, "latest_report_demoted_non_reproducing_afl_timeout",
          &latest_report_demoted_non_reproducing_afl_timeout))
    (void)string_list_push_copy(errors,
                                "latest report demotion flag is missing");
  if (!summary_bool_from_report(
          json, "latest_full_pressure_demoted_non_reproducing_afl_timeout",
          &latest_full_pressure_demoted_non_reproducing_afl_timeout))
    (void)string_list_push_copy(errors,
                                "latest full-pressure demotion flag is missing");
  char *latest_full_pressure_clean_reason =
      summary_string_from_report(json, "latest_full_pressure_clean_reason");
  if (!latest_full_pressure_clean_reason ||
      !*latest_full_pressure_clean_reason) {
    (void)string_list_push_copy(errors,
                                "latest full-pressure clean reason is missing");
  } else if (latest_full_pressure_clean && !latest_full_pressure_ok &&
             (!latest_full_pressure_demoted_non_reproducing_afl_timeout ||
              strcmp(latest_full_pressure_clean_reason,
                     "demoted-non-reproducing-afl-timeout") != 0)) {
    (void)string_list_push_copy(errors,
                                "latest full-pressure demotion reason is wrong");
  }
  (void)latest_report_demoted_non_reproducing_afl_timeout;
  free(latest_full_pressure_clean_reason);
  char *latest_full_pressure =
      summary_string_from_report(json, "latest_full_pressure_report");
  if (!latest_full_pressure || !*latest_full_pressure)
    (void)string_list_push_copy(errors, "latest full-pressure report path is missing");
  double advisory_timeouts = -1.0, current_advisory_timeouts = -1.0;
  double historical_advisory_timeouts = -1.0, non_reproducing_timeouts = -1.0;
  double historical_non_reproducing_timeouts = -1.0;
  if (!summary_number_from_report(json, "advisory_timeouts",
                                  &advisory_timeouts))
    (void)string_list_push_copy(errors, "status advisory timeout alias is missing");
  if (!summary_number_from_report(json, "current_advisory_timeouts",
                                  &current_advisory_timeouts))
    (void)string_list_push_copy(errors, "status current advisory timeout alias is missing");
  if (!summary_number_from_report(json, "historical_advisory_timeouts",
                                  &historical_advisory_timeouts))
    (void)string_list_push_copy(errors,
                                "status historical advisory timeout alias is missing");
  if (!summary_number_from_report(json, "non_reproducing_afl_timeouts",
                                  &non_reproducing_timeouts))
    (void)string_list_push_copy(errors,
                                "status non-reproducing timeout count is missing");
  if (!summary_number_from_report(json,
                                  "historical_non_reproducing_afl_timeouts",
                                  &historical_non_reproducing_timeouts))
    (void)string_list_push_copy(errors,
                                "status historical non-reproducing timeout count is missing");
  if (advisory_timeouts != current_advisory_timeouts ||
      current_advisory_timeouts != non_reproducing_timeouts)
    (void)string_list_push_copy(errors,
                                "status current advisory timeout aliases diverged");
  if (historical_advisory_timeouts != historical_non_reproducing_timeouts)
    (void)string_list_push_copy(errors,
                                "status historical advisory timeout aliases diverged");
  char *advisory_state = summary_string_from_report(json, "advisory_state");
  if (!advisory_state ||
      strcmp(advisory_state,
             fuzz_all_advisory_state(current_advisory_timeouts,
                                     historical_advisory_timeouts)) != 0)
    (void)string_list_push_copy(errors, "status advisory state wrong");
  char *advisory_action =
      summary_string_from_report(json, "advisory_action_command");
  if (!advisory_action) {
    (void)string_list_push_copy(errors,
                                "status advisory action command is missing");
  } else if ((current_advisory_timeouts > 0.0 ||
              historical_advisory_timeouts > 0.0) &&
             (!selftest_command_uses_env_nice(advisory_action) ||
              !strstr(advisory_action, "fuzz all worklist") ||
              !strstr(advisory_action, "--include-history") ||
              !strstr(advisory_action, "worklist-history.json") ||
              !strstr(advisory_action, "worklist-history.md"))) {
    (void)string_list_push_copy(errors,
                                "status advisory action command is incomplete");
  } else if (current_advisory_timeouts == 0.0 &&
             historical_advisory_timeouts == 0.0 &&
             advisory_action[0] != '\0') {
    (void)string_list_push_copy(errors,
                                "status advisory action command should be empty");
  }
  char *advisory_recheck =
      summary_string_from_report(json, "advisory_recheck_command");
  if (!advisory_recheck) {
    (void)string_list_push_copy(errors,
                                "status advisory recheck command is missing");
  } else if ((current_advisory_timeouts > 0.0 ||
              historical_advisory_timeouts > 0.0) &&
             (!strstr(advisory_recheck, "NYTRIX_AFL_RAW=1") ||
              !strstr(advisory_recheck, "timeout 15s"))) {
    (void)string_list_push_copy(errors,
                                "status advisory recheck command is incomplete");
  } else if (advisory_recheck && *advisory_recheck) {
    char nytrix_root[4096] = {0};
    char absolute_cache[4096] = {0};
    if (find_nytrix_root(nytrix_root, sizeof(nytrix_root)) &&
        path_join(absolute_cache, sizeof(absolute_cache), nytrix_root,
                  "build/cache/scratch") &&
        strstr(advisory_recheck, absolute_cache))
      (void)string_list_push_copy(
          errors,
          "status advisory recheck command leaked absolute cache paths");
  } else if (current_advisory_timeouts == 0.0 &&
             historical_advisory_timeouts == 0.0 &&
             advisory_recheck[0] != '\0') {
    (void)string_list_push_copy(errors,
                                "status advisory recheck command should be empty");
  }
  char *advisory_recheck_state =
      summary_string_from_report(json, "advisory_recheck_state");
  double advisory_recheck_checked = -1.0, advisory_recheck_passed = -1.0;
  double advisory_recheck_timeouts = -1.0, advisory_recheck_unexpected = -1.0;
  bool advisory_present = current_advisory_timeouts > 0.0 ||
                          historical_advisory_timeouts > 0.0;
  if (!advisory_recheck_state) {
    (void)string_list_push_copy(errors,
                                "status advisory recheck state is missing");
  } else if (advisory_present && advisory_recheck &&
             *advisory_recheck &&
             strcmp(advisory_recheck_state, "passed") != 0) {
    (void)string_list_push_copy(errors,
                                "status advisory recheck state should be passed");
  } else if (!advisory_present &&
             strcmp(advisory_recheck_state, "clear") != 0) {
    (void)string_list_push_copy(errors,
                                "status advisory recheck state should be clear");
  }
  if (!summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_checked",
                                  &advisory_recheck_checked) ||
      !summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_passed",
                                  &advisory_recheck_passed) ||
      !summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_timeouts",
                                  &advisory_recheck_timeouts) ||
      !summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_unexpected",
                                  &advisory_recheck_unexpected)) {
    (void)string_list_push_copy(errors,
                                "status advisory recheck counters are missing");
  } else if (advisory_present && advisory_recheck && *advisory_recheck &&
             (advisory_recheck_checked <= 0.0 ||
              advisory_recheck_passed != advisory_recheck_checked ||
              advisory_recheck_timeouts != 0.0 ||
              advisory_recheck_unexpected != 0.0)) {
    (void)string_list_push_copy(errors,
                                "status advisory recheck counters are wrong");
  } else if (!advisory_present &&
             (advisory_recheck_checked != 0.0 ||
              advisory_recheck_passed != 0.0 ||
              advisory_recheck_timeouts != 0.0 ||
              advisory_recheck_unexpected != 0.0)) {
    (void)string_list_push_copy(errors,
                                "status advisory recheck counters should be zero");
  }
  double effective_advisory_timeouts = -1.0;
  double advisory_effective_timeouts = -1.0;
  double expected_effective_advisory_timeouts =
      fuzz_all_effective_advisory_timeouts(
          current_advisory_timeouts, advisory_recheck_checked,
          advisory_recheck_passed, advisory_recheck_timeouts,
          advisory_recheck_unexpected, advisory_recheck);
  if (!summary_number_from_report(json, "effective_advisory_timeouts",
                                  &effective_advisory_timeouts) ||
      !summary_number_from_report(json, "advisory_effective_timeouts",
                                  &advisory_effective_timeouts) ||
      effective_advisory_timeouts != expected_effective_advisory_timeouts ||
      advisory_effective_timeouts != expected_effective_advisory_timeouts)
    (void)string_list_push_copy(errors,
                                "status effective advisory timeout aliases wrong");
  char *advisory_penalty_state =
      summary_string_from_report(json, "advisory_penalty_state");
  if (!advisory_penalty_state ||
      strcmp(advisory_penalty_state,
             fuzz_all_advisory_penalty_state(
                 expected_effective_advisory_timeouts)) != 0)
    (void)string_list_push_copy(errors,
                                "status advisory penalty state wrong");
  free(advisory_state);
  free(advisory_action);
  free(advisory_recheck);
  free(advisory_recheck_state);
  free(advisory_penalty_state);
  double active_perf_hotspots = -1.0, latest_perf_hotspots = -2.0;
  double historical_perf_hotspots = -1.0, active_legacy_perf_hotspots = -2.0;
  double latest_perf_ratio = -1.0, latest_full_pressure_perf_ratio = -1.0;
  double latest_full_pressure_perf_slowdown = -1.0;
  double latest_full_pressure_perf_rows = -1.0;
  double compact_perf_hotspots = -1.0, compact_perf_worst_ratio = -1.0;
  double perf_watchlist_open = -1.0, perf_watchlist_threshold = -1.0;
  bool latest_full_pressure_perf_current = false;
  bool expected_watchlist_artifact_readable = false;
  bool expected_watchlist_artifact_fresh = false;
  double expected_watchlist_artifact_hotspots = 0.0;
  double expected_watchlist_artifact_max_ratio = 0.0;
  if (!summary_number_from_report(json, "active_perf_hotspots",
                                  &active_perf_hotspots))
    (void)string_list_push_copy(errors, "active performance hotspot alias is missing");
  if (!summary_number_from_report(json, "latest_perf_hotspots",
                                  &latest_perf_hotspots))
    (void)string_list_push_copy(errors, "latest performance hotspot count is missing");
  if (!summary_number_from_report(json, "latest_perf_max_ratio",
                                  &latest_perf_ratio))
    (void)string_list_push_copy(errors, "latest performance worst ratio is missing");
  if (!summary_number_from_report(json,
                                  "latest_full_pressure_perf_max_ratio",
                                  &latest_full_pressure_perf_ratio))
    (void)string_list_push_copy(errors,
                                "latest full-pressure performance ratio is missing");
  if (!summary_number_from_report(
          json, "latest_full_pressure_perf_max_slowdown_percent",
          &latest_full_pressure_perf_slowdown))
    (void)string_list_push_copy(
        errors, "latest full-pressure performance slowdown is missing");
  if (!summary_number_from_report(json, "latest_full_pressure_perf_rows",
                                  &latest_full_pressure_perf_rows))
    (void)string_list_push_copy(
        errors, "latest full-pressure performance row count is missing");
  if (!summary_bool_from_report(json,
                                "latest_full_pressure_perf_suite_current",
                                &latest_full_pressure_perf_current))
    (void)string_list_push_copy(errors,
                                "full-pressure perf-suite freshness is missing");
  if (!summary_number_from_report(json, "historical_perf_hotspots",
                                  &historical_perf_hotspots))
    (void)string_list_push_copy(errors, "historical performance hotspot alias is missing");
  if (!summary_number_from_report(json, "perf_hotspots",
                                  &active_legacy_perf_hotspots))
    (void)string_list_push_copy(errors, "active performance hotspot count is missing");
  if (!summary_number_from_report(json, "perf_hotspots_open",
                                  &compact_perf_hotspots))
    (void)string_list_push_copy(errors, "compact performance hotspot count is missing");
  if (!summary_number_from_report(json, "perf_worst_ratio",
                                  &compact_perf_worst_ratio))
    (void)string_list_push_copy(errors, "compact performance worst ratio is missing");
  double compact_perf_worst_slowdown = -1.0;
  if (!summary_number_from_report(json, "perf_worst_slowdown_percent",
                                  &compact_perf_worst_slowdown))
    (void)string_list_push_copy(errors,
                                "compact performance worst slowdown is missing");
  if (!summary_number_from_report(json, "perf_watchlist_open",
                                  &perf_watchlist_open))
    (void)string_list_push_copy(errors, "performance watchlist count is missing");
  if (!summary_number_from_report(json, "perf_watchlist_threshold_ratio",
                                  &perf_watchlist_threshold))
    (void)string_list_push_copy(errors, "performance watchlist threshold is missing");
  (void)summary_bool_from_report(json, "perf_watchlist_artifact_readable",
                                 &expected_watchlist_artifact_readable);
  (void)summary_bool_from_report(json, "perf_watchlist_artifact_fresh",
                                 &expected_watchlist_artifact_fresh);
  (void)summary_number_from_report(json, "perf_watchlist_artifact_hotspots",
                                   &expected_watchlist_artifact_hotspots);
  (void)summary_number_from_report(json, "perf_watchlist_artifact_max_ratio",
                                   &expected_watchlist_artifact_max_ratio);
  if (active_perf_hotspots != latest_perf_hotspots)
    (void)string_list_push_copy(errors, "active performance hotspot alias does not match latest gate");
  if (active_perf_hotspots != active_legacy_perf_hotspots)
    (void)string_list_push_copy(errors, "primary performance hotspot count does not match active gate");
  if (compact_perf_hotspots != latest_perf_hotspots)
    (void)string_list_push_copy(errors, "compact performance hotspot count does not match latest gate");
  double expected_full_pressure_slowdown =
      fuzz_all_perf_slowdown_percent(latest_full_pressure_perf_ratio);
  double full_pressure_slowdown_delta =
      latest_full_pressure_perf_slowdown - expected_full_pressure_slowdown;
  if (full_pressure_slowdown_delta < 0.0)
    full_pressure_slowdown_delta = -full_pressure_slowdown_delta;
  if (full_pressure_slowdown_delta > 0.01)
    (void)string_list_push_copy(
        errors, "latest full-pressure performance slowdown is wrong");
  if (latest_full_pressure_perf_current &&
      latest_full_pressure_perf_rows <= 0.0)
    (void)string_list_push_copy(
        errors, "latest full-pressure performance row count is empty");
  double expected_perf_worst_ratio = latest_perf_ratio;
  if (latest_full_pressure_perf_current &&
      latest_full_pressure_perf_ratio > expected_perf_worst_ratio)
    expected_perf_worst_ratio = latest_full_pressure_perf_ratio;
  expected_perf_worst_ratio = fuzz_all_perf_effective_worst_ratio(
      expected_perf_worst_ratio, expected_watchlist_artifact_readable,
      expected_watchlist_artifact_fresh,
      expected_watchlist_artifact_max_ratio);
  double perf_ratio_delta = compact_perf_worst_ratio - expected_perf_worst_ratio;
  if (perf_ratio_delta < 0.0) perf_ratio_delta = -perf_ratio_delta;
  if (perf_ratio_delta > 0.0001)
    (void)string_list_push_copy(errors,
                                "compact performance worst ratio is wrong");
  double expected_perf_worst_slowdown =
      fuzz_all_perf_slowdown_percent(expected_perf_worst_ratio);
  double perf_slowdown_delta =
      compact_perf_worst_slowdown - expected_perf_worst_slowdown;
  if (perf_slowdown_delta < 0.0) perf_slowdown_delta = -perf_slowdown_delta;
  if (perf_slowdown_delta > 0.01)
    (void)string_list_push_copy(errors,
                                "compact performance worst slowdown is wrong");
  double expected_perf_watchlist =
      fuzz_all_perf_watchlist_effective_open(
          compact_perf_hotspots, expected_perf_worst_ratio,
          expected_watchlist_artifact_readable,
          expected_watchlist_artifact_fresh,
          expected_watchlist_artifact_hotspots);
  if (perf_watchlist_open != expected_perf_watchlist)
    (void)string_list_push_copy(errors,
                                "performance watchlist count is wrong");
  double watch_threshold_delta =
      perf_watchlist_threshold - fuzz_all_perf_watchlist_threshold();
  if (watch_threshold_delta < 0.0) watch_threshold_delta = -watch_threshold_delta;
  if (watch_threshold_delta > 0.0001)
    (void)string_list_push_copy(errors,
                                "performance watchlist threshold is wrong");
  if (!strstr(json, "\"latest_perf_max_case\"") ||
      !strstr(json, "\"active_perf_max_case\"") ||
      !strstr(json, "\"perf_worst_case\""))
    (void)string_list_push_copy(errors, "performance worst-case names are missing");
  char *latest_perf_case =
      summary_string_from_report(json, "latest_perf_max_case");
  char *latest_full_pressure_perf_case =
      summary_string_from_report(json, "latest_full_pressure_perf_max_case");
  char *expected_watchlist_artifact_case =
      summary_string_from_report(json, "perf_watchlist_artifact_max_case");
  char *compact_perf_case =
      summary_string_from_report(json, "perf_worst_case");
  const char *expected_perf_case =
      latest_perf_case ? latest_perf_case : "";
  if (latest_full_pressure_perf_current &&
      latest_full_pressure_perf_ratio > latest_perf_ratio)
    expected_perf_case =
        latest_full_pressure_perf_case ? latest_full_pressure_perf_case : "";
  expected_perf_case = fuzz_all_perf_effective_worst_case(
      expected_perf_case, expected_watchlist_artifact_readable,
      expected_watchlist_artifact_fresh,
      expected_watchlist_artifact_max_ratio,
      expected_watchlist_artifact_case);
  if (!compact_perf_case ||
      strcmp(compact_perf_case, expected_perf_case) != 0)
    (void)string_list_push_copy(errors,
                                "compact performance worst case is wrong");
  char *perf_watchlist_case =
      summary_string_from_report(json, "perf_watchlist_case");
  const char *expected_watchlist_case =
      expected_perf_watchlist > 0.0 ? expected_perf_case : "";
  if (!perf_watchlist_case ||
      strcmp(perf_watchlist_case, expected_watchlist_case) != 0)
    (void)string_list_push_copy(errors,
                                "performance watchlist case is wrong");
  char *perf_watchlist_command =
      summary_string_from_report(json, "perf_watchlist_command");
  if (!perf_watchlist_command ||
      !selftest_command_uses_env_nice(perf_watchlist_command) ||
      !strstr(perf_watchlist_command, "perf triage --fast") ||
      !strstr(perf_watchlist_command, "--threshold 1.25") ||
      !strstr(perf_watchlist_command, "perf-watchlist.json") ||
      !strstr(perf_watchlist_command, "--markdown") ||
      !strstr(perf_watchlist_command, "perf-watchlist.md"))
    (void)string_list_push_copy(errors,
                                "performance watchlist command is wrong");
  char *perf_watchlist_state =
      summary_string_from_report(json, "perf_watchlist_state");
  if (!fuzz_all_perf_watchlist_state_valid(perf_watchlist_state))
    (void)string_list_push_copy(errors,
                                "performance watchlist state is wrong");
  char *perf_watchlist_action =
      summary_string_from_report(json, "perf_watchlist_action");
  char *perf_watchlist_action_command =
      summary_string_from_report(json, "perf_watchlist_action_command");
  char *optimization_action =
      summary_string_from_report(json, "optimization_action");
  char *optimization_reason =
      summary_string_from_report(json, "optimization_reason");
  char *optimization_command =
      summary_string_from_report(json, "optimization_command");
  char *optimization_target_command =
      summary_string_from_report(json, "optimization_target_command");
  char *optimization_case =
      summary_string_from_report(json, "optimization_case");
  char *optimization_artifact =
      summary_string_from_report(json, "optimization_artifact");
  char *optimization_ny_source =
      summary_string_from_report(json, "optimization_ny_source");
  char *optimization_c_source =
      summary_string_from_report(json, "optimization_c_source");
  double optimization_ratio = -1.0;
  double optimization_slowdown = -1.0;
  if (!fuzz_all_perf_watchlist_action_valid(perf_watchlist_action))
    (void)string_list_push_copy(errors,
                                "performance watchlist action is wrong");
  if (!fuzz_all_perf_watchlist_action_valid(optimization_action) ||
      (perf_watchlist_action && optimization_action &&
       strcmp(optimization_action, perf_watchlist_action) != 0))
    (void)string_list_push_copy(errors,
                                "optimization action does not mirror watchlist action");
  if (!optimization_reason || !*optimization_reason)
    (void)string_list_push_copy(errors,
                                "optimization reason is missing");
  if (!optimization_command ||
      (perf_watchlist_action_command &&
       strcmp(optimization_command, perf_watchlist_action_command) != 0))
    (void)string_list_push_copy(errors,
                                "optimization command does not mirror watchlist command");
  if (!optimization_case)
    (void)string_list_push_copy(errors, "optimization case is missing");
  if (!strstr(json, "\"optimization_target_command\""))
    (void)string_list_push_copy(errors,
                                "optimization target command is missing");
  if (!strstr(json, "\"optimization_artifact\"") ||
      !strstr(json, "\"optimization_ny_source\"") ||
      !strstr(json, "\"optimization_c_source\""))
    (void)string_list_push_copy(errors,
                                "optimization target file fields are missing");
  if (!summary_number_from_report(json, "optimization_ratio",
                                  &optimization_ratio))
    (void)string_list_push_copy(errors, "optimization ratio is missing");
  if (!summary_number_from_report(json, "optimization_slowdown_percent",
                                  &optimization_slowdown))
    (void)string_list_push_copy(errors, "optimization slowdown is missing");
  if (expected_perf_watchlist > 0.0) {
    if (!optimization_case || !*optimization_case)
      (void)string_list_push_copy(errors, "optimization case is empty");
    if (optimization_ratio <= 0.0)
      (void)string_list_push_copy(errors, "optimization ratio is empty");
  }
  if (optimization_ratio >= 0.0 && optimization_slowdown >= 0.0) {
    double expected_optimization_slowdown =
        fuzz_all_perf_slowdown_percent(optimization_ratio);
    double optimization_slowdown_delta =
        optimization_slowdown - expected_optimization_slowdown;
    if (optimization_slowdown_delta < -0.01 ||
        optimization_slowdown_delta > 0.01)
      (void)string_list_push_copy(errors,
                                  "optimization slowdown does not match ratio");
  }
  bool expect_optimization_action_markdown =
      perf_watchlist_action && strcmp(perf_watchlist_action, "none") != 0;
  char expected_optimization_action[64] = {0};
  if (expect_optimization_action_markdown)
    snprintf(expected_optimization_action, sizeof(expected_optimization_action),
             "%s", perf_watchlist_action);
  if (perf_watchlist_action &&
      strcmp(perf_watchlist_action, "inspect-watchlist") == 0 &&
      (!perf_watchlist_action_command ||
       !strstr(perf_watchlist_action_command, "perf-watchlist.md")))
    (void)string_list_push_copy(errors,
                                "performance watchlist inspect command is wrong");
  if (perf_watchlist_action &&
      strcmp(perf_watchlist_action, "refresh-watchlist") == 0 &&
      (!perf_watchlist_action_command ||
       !selftest_command_uses_env_nice(perf_watchlist_action_command) ||
       !strstr(perf_watchlist_action_command, "perf triage --fast")))
    (void)string_list_push_copy(errors,
                                "performance watchlist refresh command is wrong");
  char *perf_watchlist_report =
      summary_string_from_report(json, "perf_watchlist_report");
  char *perf_watchlist_markdown =
      summary_string_from_report(json, "perf_watchlist_markdown");
  bool perf_watchlist_artifact_readable = false;
  bool perf_watchlist_artifact_fresh = false;
  double perf_watchlist_artifact_age = -2.0;
  double perf_watchlist_artifact_stale_after = -1.0;
  if (!perf_watchlist_report ||
      !strstr(perf_watchlist_report, "perf-watchlist.json"))
    (void)string_list_push_copy(errors,
                                "performance watchlist report path is wrong");
  if (!perf_watchlist_markdown ||
      !strstr(perf_watchlist_markdown, "perf-watchlist.md"))
    (void)string_list_push_copy(errors,
                                "performance watchlist markdown path is wrong");
  if (!summary_bool_from_report(json, "perf_watchlist_artifact_readable",
                                &perf_watchlist_artifact_readable))
    (void)string_list_push_copy(errors,
                                "performance watchlist artifact readable flag is missing");
  if (!summary_bool_from_report(json, "perf_watchlist_artifact_fresh",
                                &perf_watchlist_artifact_fresh))
    (void)string_list_push_copy(errors,
                                "performance watchlist artifact fresh flag is missing");
  if (!summary_number_from_report(json,
                                  "perf_watchlist_artifact_age_seconds",
                                  &perf_watchlist_artifact_age))
    (void)string_list_push_copy(errors,
                                "performance watchlist artifact age is missing");
  if (!summary_number_from_report(json,
                                  "perf_watchlist_artifact_stale_after_hours",
                                  &perf_watchlist_artifact_stale_after) ||
      perf_watchlist_artifact_stale_after < 23.9 ||
      perf_watchlist_artifact_stale_after > 24.1)
    (void)string_list_push_copy(errors,
                                "performance watchlist artifact freshness window is wrong");
  double perf_watchlist_artifact_slowdown = -1.0;
  if (!summary_number_from_report(
          json, "perf_watchlist_artifact_max_slowdown_percent",
          &perf_watchlist_artifact_slowdown))
    (void)string_list_push_copy(
        errors, "performance watchlist artifact slowdown is missing");
  if (perf_watchlist_artifact_readable && perf_watchlist_artifact_fresh &&
      optimization_action &&
      strcmp(optimization_action, "inspect-watchlist") == 0 &&
      optimization_ratio > 0.0 &&
      (!optimization_artifact || !*optimization_artifact ||
       !optimization_ny_source || !*optimization_ny_source ||
       !optimization_c_source || !*optimization_c_source))
    (void)string_list_push_copy(
        errors, "optimization target file fields are empty for fresh watchlist");
  if (optimization_artifact && *optimization_artifact &&
      (!optimization_target_command ||
       !strstr(optimization_target_command, optimization_artifact)))
    (void)string_list_push_copy(
        errors, "optimization target command omits artifact");
  if (optimization_ny_source && *optimization_ny_source &&
      (!optimization_target_command ||
       !strstr(optimization_target_command, optimization_ny_source)))
    (void)string_list_push_copy(
        errors, "optimization target command omits Ny source");
  if (optimization_c_source && *optimization_c_source &&
      (!optimization_target_command ||
       !strstr(optimization_target_command, optimization_c_source)))
    (void)string_list_push_copy(
        errors, "optimization target command omits C source");
  free(latest_perf_case);
  free(latest_full_pressure_perf_case);
  free(expected_watchlist_artifact_case);
  free(compact_perf_case);
  free(perf_watchlist_case);
  free(perf_watchlist_command);
  free(perf_watchlist_state);
  free(perf_watchlist_action);
  free(perf_watchlist_action_command);
  free(optimization_action);
  free(optimization_reason);
  free(optimization_command);
  free(optimization_target_command);
  free(optimization_case);
  free(optimization_artifact);
  free(optimization_ny_source);
  free(optimization_c_source);
  free(perf_watchlist_report);
  free(perf_watchlist_markdown);
  double active_finding_live = -1.0, active_finding_missing = -1.0;
  double latest_finding_live = -2.0, latest_finding_missing = -2.0;
  double historical_finding_live = -1.0, historical_finding_missing = -1.0;
  if (!summary_number_from_report(json, "active_compiler_finding_live",
                                  &active_finding_live))
    (void)string_list_push_copy(errors, "active compiler live alias is missing");
  if (!summary_number_from_report(json, "active_compiler_finding_missing",
                                  &active_finding_missing))
    (void)string_list_push_copy(errors, "active compiler missing alias is missing");
  if (!summary_number_from_report(json, "latest_compiler_finding_live",
                                  &latest_finding_live))
    (void)string_list_push_copy(errors, "latest compiler live count is missing");
  if (!summary_number_from_report(json, "latest_compiler_finding_missing",
                                  &latest_finding_missing))
    (void)string_list_push_copy(errors, "latest compiler missing count is missing");
  if (!summary_number_from_report(json, "historical_compiler_finding_live",
                                  &historical_finding_live))
    (void)string_list_push_copy(errors, "historical compiler live alias is missing");
  if (!summary_number_from_report(json, "historical_compiler_finding_missing",
                                  &historical_finding_missing))
    (void)string_list_push_copy(errors, "historical compiler missing alias is missing");
  if (active_finding_live != latest_finding_live ||
      active_finding_missing != latest_finding_missing)
    (void)string_list_push_copy(errors, "active compiler finding aliases do not match latest gate");
  if (historical_finding_live < 0.0 || historical_finding_missing < 0.0)
    (void)string_list_push_copy(errors, "historical compiler finding aliases are invalid");
  double active_known_reproduced = -1.0, active_known_lost = -1.0;
  double active_known_baseline = -1.0, latest_known_reproduced = -2.0;
  double latest_known_lost = -2.0, latest_known_baseline = -2.0;
  double historical_known_reproduced = -1.0, historical_known_lost = -1.0;
  double historical_known_baseline = -1.0;
  if (!summary_number_from_report(json, "active_known_bug_reproduced",
                                  &active_known_reproduced))
    (void)string_list_push_copy(errors, "active known-bug reproduced alias is missing");
  if (!summary_number_from_report(json, "active_known_bug_lost_signal",
                                  &active_known_lost))
    (void)string_list_push_copy(errors, "active known-bug lost-signal alias is missing");
  if (!summary_number_from_report(json, "active_known_bug_baseline_failures",
                                  &active_known_baseline))
    (void)string_list_push_copy(errors, "active known-bug baseline alias is missing");
  if (!summary_number_from_report(json, "latest_known_bug_reproduced",
                                  &latest_known_reproduced) ||
      !summary_number_from_report(json, "latest_known_bug_lost_signal",
                                  &latest_known_lost) ||
      !summary_number_from_report(json, "latest_known_bug_baseline_failures",
                                  &latest_known_baseline))
    (void)string_list_push_copy(errors, "latest known-bug counters are missing");
  if (!summary_number_from_report(json, "historical_known_bug_reproduced",
                                  &historical_known_reproduced))
    (void)string_list_push_copy(errors, "historical known-bug reproduced alias is missing");
  if (!summary_number_from_report(json, "historical_known_bug_lost_signal",
                                  &historical_known_lost))
    (void)string_list_push_copy(errors, "historical known-bug lost-signal alias is missing");
  if (!summary_number_from_report(json, "historical_known_bug_baseline_failures",
                                  &historical_known_baseline))
    (void)string_list_push_copy(errors, "historical known-bug baseline alias is missing");
  if (active_known_reproduced != latest_known_reproduced ||
      active_known_lost != latest_known_lost ||
      active_known_baseline != latest_known_baseline)
    (void)string_list_push_copy(errors, "active known-bug aliases do not match latest gate");
  if (historical_known_reproduced < 0.0 || historical_known_lost < 0.0 ||
      historical_known_baseline < 0.0)
    (void)string_list_push_copy(errors, "historical known-bug aliases are invalid");
  double compact_compiler_findings = -1.0;
  double compact_known_bug_findings = -1.0;
  double compact_correctness_findings = -1.0;
  double expected_compiler_findings =
      latest_finding_live + latest_finding_missing;
  double expected_known_bug_findings =
      latest_known_reproduced + latest_known_lost + latest_known_baseline;
  if (!summary_number_from_report(json, "compiler_findings",
                                  &compact_compiler_findings))
    (void)string_list_push_copy(errors, "compact compiler findings are missing");
  if (!summary_number_from_report(json, "known_bug_replay_findings",
                                  &compact_known_bug_findings))
    (void)string_list_push_copy(errors,
                                "compact known-bug findings are missing");
  if (!summary_number_from_report(json, "correctness_findings",
                                  &compact_correctness_findings))
    (void)string_list_push_copy(errors,
                                "compact correctness findings are missing");
  if (compact_compiler_findings != expected_compiler_findings)
    (void)string_list_push_copy(errors,
                                "compact compiler findings are wrong");
  if (compact_known_bug_findings != expected_known_bug_findings)
    (void)string_list_push_copy(errors,
                                "compact known-bug findings are wrong");
  if (compact_correctness_findings !=
      expected_compiler_findings + expected_known_bug_findings)
    (void)string_list_push_copy(errors,
                                "compact correctness findings are wrong");
  bool cache_policy_ok = false, old_scratch_absent = false;
  bool old_fuzz_absent = false, ny_bin_exists = false;
  bool old_build_cache_absent = false;
  bool active_old_writer_present = false;
  bool legacy_active_old_writer_present = false;
  if (!summary_bool_from_report(json, "cache_policy_ok", &cache_policy_ok) ||
      !cache_policy_ok)
    (void)string_list_push_copy(errors, "status cache policy was not ok");
  if (!summary_bool_from_report(json, "old_nytrix_test_scratch_absent",
                                &old_scratch_absent))
    (void)string_list_push_copy(errors, "old Nytrix test scratch diagnostic was missing");
  if (!summary_bool_from_report(json, "old_nytrix_fuzz_absent",
                                &old_fuzz_absent))
    (void)string_list_push_copy(errors, "old Nytrix fuzz directory diagnostic was missing");
  if (!summary_bool_from_report(json, "old_nytrix_build_cache_absent",
                                &old_build_cache_absent))
    (void)string_list_push_copy(errors, "old Nytrix build cache diagnostic was missing");
  if (!summary_bool_from_report(json,
                                "active_old_nytrix_output_writer_present",
                                &active_old_writer_present))
    (void)string_list_push_copy(errors, "active old Nytrix output writer diagnostic was missing");
  if (!summary_bool_from_report(json,
                                "active_old_nytrix_cache_writer_present",
                                &legacy_active_old_writer_present))
    (void)string_list_push_copy(errors, "legacy old Nytrix writer diagnostic was missing");
  if (active_old_writer_present != legacy_active_old_writer_present)
    (void)string_list_push_copy(errors, "old Nytrix writer aliases diverged");
  if (!summary_bool_from_report(json, "ny_bin_exists", &ny_bin_exists) ||
      !ny_bin_exists)
    (void)string_list_push_copy(errors, "Nytrix compiler binary provenance was missing");
  double status_thread_hours_needed = -1.0;
  if (!summary_number_from_report(json, "thread_hours_needed",
                                  &status_thread_hours_needed) ||
      status_thread_hours_needed < 0.0)
    (void)string_list_push_copy(errors, "status total thread-hours field is missing");
  char *recommended_action =
      summary_string_from_report(json, "recommended_action");
  char *recommended_reason =
      summary_string_from_report(json, "recommended_reason");
  char *recommended_command =
      summary_string_from_report(json, "recommended_command");
  char *recommended_low_cpu_command =
      summary_string_from_report(json, "recommended_low_cpu_command");
    char *recommended_preview_command =
        summary_string_from_report(json, "recommended_preview_command");
    char *recommended_repeat_mode =
        summary_string_from_report(json, "recommended_repeat_mode");
    double recommended_repeat_count = -1.0;
    double status_runs_needed = -1.0;
    (void)summary_number_from_report(json, "runs_needed", &status_runs_needed);
  bool expect_missing_evidence =
      !stale_evidence && coverage_next_command && *coverage_next_command;
  bool expect_missing_evidence_preview =
      expect_missing_evidence && coverage_next_preview_command &&
      *coverage_next_preview_command;
    const char *expected_recommended_action =
        stale_evidence ? "freshen-evidence" :
          (expect_missing_evidence ? "run-missing-evidence" :
              (runs_to_good > 0.0 ? "run-good" : "run-target"));
  const char *expected_recommended_repeat =
      strcmp(expected_recommended_action, "run-good") == 0 ? "good" :
          (strcmp(expected_recommended_action, "run-target") == 0 ? "target" :
                                                                    "");
  double expected_recommended_repeat_count =
      strcmp(expected_recommended_repeat, "good") == 0 ? runs_to_good :
          (strcmp(expected_recommended_repeat, "target") == 0 ?
               status_runs_needed : 0.0);
  char *markdown = summary_string_from_report(json, "markdown");
  if (!markdown || !strstr(markdown, expected_status_md))
    (void)string_list_push_copy(errors,
                                "status markdown path lost expected cockpit");
  char markdown_abs[4096] = {0};
  if (markdown && *markdown) {
    if (path_is_absolute(markdown)) {
      snprintf(markdown_abs, sizeof(markdown_abs), "%s", markdown);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(markdown_abs, sizeof(markdown_abs), root, markdown);
    }
  }
  file_buf_t md = {0};
  if (!markdown_abs[0] || !read_file(markdown_abs, &md) || !md.data) {
    (void)string_list_push_copy(errors, "status markdown was not readable");
  } else if (!strstr(md.data, "- Old Nytrix paths:") ||
             !strstr(md.data, "test scratch") ||
             !strstr(md.data, "build cache") ||
             !strstr(md.data, "active writer")) {
    (void)string_list_push_copy(errors, "status markdown omits old Nytrix path TLDR");
  } else if (!strstr(md.data, "fuzz all old-paths --dry-run")) {
    (void)string_list_push_copy(errors, "status markdown omits old-path cleanup command");
  } else if (!strstr(md.data, "- Confidence:") ||
             !strstr(md.data, "campaign evidence") ||
             !strstr(md.data, "remaining") ||
             !strstr(md.data, "lang score") ||
             !strstr(md.data, "good >= 75.00%") ||
             !strstr(md.data, "gap")) {
    (void)string_list_push_copy(errors, "status markdown omits campaign confidence");
  } else if (!strstr(md.data, "- Completion:") ||
             !strstr(md.data, "target-not-reached") ||
             !strstr(md.data, "target reached `false`")) {
    (void)string_list_push_copy(errors, "status markdown omits campaign completion state");
    } else if (!strstr(md.data, "- Recommended:") ||
               !strstr(md.data, expected_recommended_action) ||
               (expected_recommended_repeat[0] &&
                (!strstr(md.data, expected_recommended_repeat) ||
                 !strstr(md.data, "x"))) ||
             (expect_missing_evidence ?
                  (!strstr(md.data, coverage_next_command) ||
                   (coverage_next_guarded_command &&
                    *coverage_next_guarded_command &&
                    !strstr(md.data, coverage_next_guarded_command)) ||
                   (coverage_next_low_cpu_command &&
                    *coverage_next_low_cpu_command &&
                    !strstr(md.data, coverage_next_low_cpu_command))) :
                  !strstr(md.data, expected_run_next))) {
      (void)string_list_push_copy(errors, "status markdown omits recommendation");
  } else if (!strstr(md.data, "- Handoff guards:") ||
             !strstr(md.data, "low-priority nice 10") ||
             !strstr(md.data, "load-wait 75%") ||
             !strstr(md.data, "disk >=20GB") ||
             !strstr(md.data, "lock on") ||
             !strstr(md.data, "threads 25%")) {
    (void)string_list_push_copy(errors,
                                "status markdown omits handoff guards");
    } else if (!strstr(md.data, "- Coverage:") ||
               !coverage_state ||
               !strstr(md.data, coverage_state) ||
               !strstr(md.data, "lanes (") ||
               !strstr(md.data, "not-run") ||
               !strstr(md.data, "disabled") ||
               !strstr(md.data, "budget-short")) {
      (void)string_list_push_copy(errors, "status markdown omits coverage state");
  } else if (backlog_lanes > 0.0 &&
             (!strstr(md.data, "- Coverage next:") ||
              !coverage_next_lane ||
              !strstr(md.data, coverage_next_lane) ||
              !coverage_next_command ||
              !strstr(md.data, coverage_next_command) ||
              (coverage_next_guarded_command &&
               *coverage_next_guarded_command &&
               !strstr(md.data, coverage_next_guarded_command)) ||
              (coverage_next_low_cpu_command &&
               *coverage_next_low_cpu_command &&
               !strstr(md.data, coverage_next_low_cpu_command)) ||
              !strstr(md.data, "Coverage next state:") ||
              !strstr(md.data, "inspect ") ||
             (coverage_next_state_command &&
              *coverage_next_state_command &&
              !strstr(md.data, coverage_next_state_command)) ||
             (expected_coverage_next_state_refresh_required &&
              (!strstr(md.data, "Coverage next state refresh:") ||
               !strstr(md.data,
                       coverage_next_state_refresh_reason ?
                           coverage_next_state_refresh_reason : "") ||
               !strstr(md.data,
                       recommended_state_refresh_command ?
                           recommended_state_refresh_command : ""))))) {
    (void)string_list_push_copy(errors,
                                "status markdown omits coverage next action");
  } else if (reports_considered > 0.0 &&
             (!strstr(md.data, "- Coverage evidence:") ||
              !strstr(md.data, "reports") ||
              !strstr(md.data, "campaign +") ||
              !strstr(md.data, "companion") ||
              !strstr(md.data, "latest advisory") ||
              !strstr(md.data, "companion skips"))) {
    (void)string_list_push_copy(errors,
                                "status markdown omits coverage report split");
  } else if (expected_ignored_no_evidence > 0.0 &&
             (!strstr(md.data, "Evidence hygiene:") ||
              !strstr(md.data, "ignored 1 no-evidence attempt"))) {
    (void)string_list_push_copy(errors,
                                "status markdown omits ignored no-evidence count");
  } else if (!strstr(md.data, "- Score note:") ||
             !strstr(md.data, "- Source:") ||
             !strstr(md.data, "full-pressure")) {
    (void)string_list_push_copy(errors, "status markdown omits score source");
  } else if (!strstr(md.data, "Full-pressure gate:") ||
             !strstr(md.data, "effective clean `") ||
             !strstr(md.data, "raw ok `") ||
             !strstr(md.data, "reason `") ||
             !strstr(md.data, "failures")) {
    (void)string_list_push_copy(errors,
                                "status markdown omits full-pressure gate");
  } else if (!strstr(md.data, "Compiler std audit:") ||
             !strstr(md.data, "CRT") ||
             !strstr(md.data, "families") ||
             !strstr(md.data, "compiler-std-audit.md")) {
    (void)string_list_push_copy(errors,
                                "status markdown omits compiler std-audit CRT summary");
  } else if (latest_full_pressure_perf_ratio > 0.0 &&
             (!strstr(md.data, "Full-pressure perf provenance:") ||
              !strstr(md.data, "fixture-poly") ||
              !strstr(md.data, "rows 7/7") ||
              !strstr(md.data, "suite `current`"))) {
    (void)string_list_push_copy(errors,
                                "status markdown omits full-pressure perf provenance");
  } else if (!strstr(md.data, "- Budget:") ||
             !strstr(md.data, "wall-hours") ||
             !strstr(md.data, "thread-hours")) {
    (void)string_list_push_copy(errors, "status markdown omits split budget");
  } else if (!strstr(md.data, "- Next clean run:") ||
             !strstr(md.data, "plan-rate") ||
             !strstr(md.data, "lang score")) {
    (void)string_list_push_copy(errors, "status markdown omits next-run projection");
  } else if (!expect_missing_evidence_preview &&
             (!strstr(md.data, "fuzz all progress --refresh --strict") ||
             !strstr(md.data, "--allow-full-pressure-remediation") ||
             !strstr(md.data, "--json ") ||
             !strstr(md.data, "/progress.json") ||
             !strstr(md.data, "--markdown ") ||
             !strstr(md.data, "/progress.md"))) {
    (void)string_list_push_copy(errors, "status markdown omits fresh progress command");
  }
  if (md.data && canonical_pair &&
      (strstr(md.data, "fuzz_all_status_canonical.json") ||
       strstr(md.data, "fuzz_all_status_stale_evidence.json") ||
       strstr(md.data, "status-r222.json") ||
       !strstr(md.data, expected_status_json) ||
       !strstr(md.data, expected_status_md)))
    (void)string_list_push_copy(errors,
                                "canonical status markdown self-link wrong");
  if (md.data && stale_evidence &&
      (!strstr(md.data, "Freshen evidence:") ||
       !strstr(md.data, "NYTRIX_LOW_PRIORITY=1") ||
       !strstr(md.data, "NYTRIX_RUN_NICE=10") ||
       !strstr(md.data, expected_run_next) ||
       !strstr(md.data, "latest 80.") ||
       !strstr(md.data, "full-pressure 80.")))
    (void)string_list_push_copy(errors,
                                "stale status markdown omitted freshness action");
  if (md.data && !stale_evidence && strstr(md.data, "Freshen evidence:"))
    (void)string_list_push_copy(errors,
                                "fresh status markdown unexpectedly has freshness action");
  if (md.data && expected_recommended_repeat[0] &&
      (!strstr(md.data, "Preview recommended:") ||
       !strstr(md.data, "NYTRIX_RUN_DRY_RUN=1") ||
       !strstr(md.data, expected_recommended_repeat) ||
       !strstr(md.data, expected_run_next)))
    (void)string_list_push_copy(
        errors, "status markdown omits recommended dry-run preview");
  if (md.data && stale_evidence &&
      (!strstr(md.data, "Preview recommended:") ||
       !strstr(md.data, "NYTRIX_LOW_PRIORITY=1") ||
       !strstr(md.data, "NYTRIX_RUN_NICE=10") ||
       !strstr(md.data, "NYTRIX_RUN_DRY_RUN=1") ||
       !strstr(md.data, expected_run_next)))
    (void)string_list_push_copy(
        errors, "status markdown omits stale-evidence dry-run preview");
  if (md.data && expect_missing_evidence_preview &&
      (!strstr(md.data, "Preview recommended:") ||
       !strstr(md.data, "fuzz all preflight") ||
       !strstr(md.data, "NYTRIX_LOW_PRIORITY=1") ||
       !strstr(md.data, "--allow-dirty-nytrix-baseline") ||
       !strstr(md.data, "build/cache/scratch")))
    (void)string_list_push_copy(
        errors, "status markdown omits missing-evidence preflight preview");
  if (md.data && !expected_recommended_repeat[0] &&
      !expect_missing_evidence_preview &&
      !stale_evidence &&
      strstr(md.data, "Preview recommended:"))
    (void)string_list_push_copy(
        errors, "status markdown has unexpected recommended preview");
  if (md.data && !stale_evidence && !expect_missing_evidence_preview &&
      (!expected_recommended_repeat[0] ||
       strcmp(expected_recommended_repeat, "good") == 0) &&
      (!strstr(md.data, "Preview target:") ||
       !strstr(md.data, "NYTRIX_RUN_DRY_RUN=1") ||
       !strstr(md.data, "NYTRIX_RUN_REPEAT=target") ||
       !strstr(md.data, expected_run_next)))
    (void)string_list_push_copy(errors,
                                "status markdown omits target preview");
  if (md.data && !expect_missing_evidence_preview &&
      (!strstr(md.data, "State:") ||
       !strstr(md.data,
               "jq {state,event,live,child_status,stale_after_seconds,repeat_mode,repeat_count,") ||
       !strstr(md.data, expected_state_file)))
    (void)string_list_push_copy(errors,
                                "status markdown omits state inspect command");
  if (md.data && !expect_missing_evidence_preview &&
      strstr(md.data, "State:") &&
      !strstr(md.data, "; live;") &&
      !strstr(md.data, "; not-live;"))
    (void)string_list_push_copy(errors,
                                "status markdown omits state live marker");
  if (md.data && !expect_missing_evidence_preview &&
      (!strstr(md.data, "; not-live; stale (old-state)") ||
       !strstr(md.data, expected_state_file)))
    (void)string_list_push_copy(errors,
                                "status markdown omits stale dry-run state");
  if (md.data && !expect_missing_evidence_preview &&
      (!strstr(md.data, "Pause:") ||
       !strstr(md.data, expected_stop_file) ||
       !strstr(md.data, "touch ") ||
       !strstr(md.data, "rm -f ")))
    (void)string_list_push_copy(errors,
                                "status markdown omits stop-file pause command");
  if (md.data && expect_missing_evidence_preview &&
      (!strstr(md.data, "Recommended state:") ||
       !strstr(md.data, "run-missing-evidence-state.json") ||
       !strstr(md.data, "inspect ")))
    (void)string_list_push_copy(
        errors, "status markdown omits focused missing-evidence state");
  if (md.data && expect_missing_evidence_preview) {
    const char *coverage_gaps = strstr(md.data, "\n## Coverage Gaps\n");
    const char *next_section = strstr(md.data, "\n## Next\n");
    const char *guarded_in_gaps =
        coverage_gaps ? strstr(coverage_gaps, "  Guarded: ") : NULL;
    const char *preview_in_gaps =
        coverage_gaps ? strstr(coverage_gaps, "  Preview: ") : NULL;
    const char *state_in_gaps =
        coverage_gaps ? strstr(coverage_gaps, "  State: ") : NULL;
    const char *pause_in_gaps =
        coverage_gaps ? strstr(coverage_gaps, "  Pause: ") : NULL;
    if (!coverage_gaps || !next_section ||
        !guarded_in_gaps || guarded_in_gaps > next_section ||
        !preview_in_gaps || preview_in_gaps > next_section ||
        !state_in_gaps || state_in_gaps > next_section ||
        !pause_in_gaps || pause_in_gaps > next_section ||
        (coverage_next_guarded_command &&
         *coverage_next_guarded_command &&
         !strstr(coverage_gaps, coverage_next_guarded_command)) ||
        (coverage_next_low_cpu_command &&
         *coverage_next_low_cpu_command &&
         !strstr(coverage_gaps, coverage_next_low_cpu_command)) ||
        (coverage_next_preview_command &&
         *coverage_next_preview_command &&
         !strstr(coverage_gaps, coverage_next_preview_command)) ||
        (coverage_next_state_command &&
         *coverage_next_state_command &&
         !strstr(coverage_gaps, coverage_next_state_command)) ||
        (coverage_next_state_refresh_command &&
         *coverage_next_state_refresh_command &&
         !strstr(coverage_gaps,
                 coverage_next_state_refresh_command)))
      (void)string_list_push_copy(
          errors,
          "status coverage gap detail omits guarded missing-evidence handoff");
    const char *afl_parser_gap =
        coverage_gaps ? strstr(coverage_gaps, "`afl_parsers`") : NULL;
    if (afl_parser_gap && coverage_next_guarded_command &&
        *coverage_next_guarded_command && coverage_next_command &&
        *coverage_next_command) {
      const char *next_gap = strstr(afl_parser_gap + 1, "\n- ");
      const char *raw_command_after_parser =
          strstr(afl_parser_gap, coverage_next_command);
      const char *guarded_after_parser =
          strstr(afl_parser_gap, coverage_next_guarded_command);
      if (raw_command_after_parser &&
          (!next_gap || raw_command_after_parser < next_gap) &&
          (!guarded_after_parser ||
           (next_gap && guarded_after_parser > next_gap) ||
           (next_section && guarded_after_parser > next_section)))
        (void)string_list_push_copy(
            errors,
            "status AFL parser gap does not reuse guarded missing-evidence handoff");
    }
  }
  if (md.data) {
      const char *next_section = strstr(md.data, "\n## Next\n");
      const char *controls_section = strstr(md.data, "\n## Controls\n");
      const char *next_end = next_section ?
          (controls_section ? controls_section : md.data + strlen(md.data)) :
          NULL;
      const char *state_token =
          expect_missing_evidence_preview ?
              "run-missing-evidence-state.json" : expected_state_file;
      const char *stop_token =
          expect_missing_evidence_preview ?
              "missing-evidence-stop" : expected_stop_file;
      const char *state_after_next =
          next_section && state_token && *state_token ?
              strstr(next_section, state_token) : NULL;
      const char *stop_after_next =
          next_section && stop_token && *stop_token ?
              strstr(next_section, stop_token) : NULL;
      if (!controls_section || !state_after_next || !stop_after_next)
        (void)string_list_push_copy(errors,
                                    "status markdown omits Controls block");
      const char *quick_jq_line = controls_section ?
          strstr(controls_section,
                 "jq {gate:{ready,blockers,active:.active_count}") :
          NULL;
      const char *quick_jq_end =
          quick_jq_line ? strchr(quick_jq_line, '\n') : NULL;
      if (!quick_jq_line || !quick_jq_end ||
          !find_n(quick_jq_line, quick_jq_end, expected_status_json) ||
          !find_n(quick_jq_line, quick_jq_end, "script:.next_script") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "handoff:.next_handoff_command") ||
          !find_n(quick_jq_line, quick_jq_end, "run:.next_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "recommended:.recommended_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "state_refresh:.recommended_state_refresh_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "freshen:.freshness_action_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "runstate:{state:.state,event:.state_event,fresh:.state_fresh,age:.state_age_seconds,age_h:(.state_age_seconds/3600),stale_after:.state_stale_after_seconds,stale_after_h:(.state_stale_after_seconds/3600),over_s:(.state_age_seconds-.state_stale_after_seconds),reason:.state_stale_reason,dry_h:.state_dry_run_wall_hours,dry_gain_pct:.state_dry_run_campaign_gain_percent,dry_years:.state_dry_run_thread_years,threads:.state_handoff_threads}") ||
          find_n(quick_jq_line, quick_jq_end,
                 "low:.recommended_low_cpu_command,refresh:") ||
          find_n(quick_jq_line, quick_jq_end, "script:.next_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "low:.recommended_low_cpu_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "gentle:.run_next_gentle_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "gentle_preview:.run_next_gentle_preview_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "penalty:.freshness_penalty") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "perf:{hotspots:.perf_hotspots_open,watch:.perf_watchlist_state,worst:{case:.perf_worst_case,ratio:.perf_worst_ratio,slow:.perf_worst_slowdown_percent},opt:{action:.optimization_action,case:.optimization_case,ratio:.optimization_ratio,cmd:.optimization_command,target:.optimization_target_command}}") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "rt:{state:.runtime_surface_state,done:.runtime_coverage_done,total:.runtime_coverage_total}") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "crt:{state:.crt_surface_state,scope:.crt_surface_scope,behavior:.crt_behavior_state,next:.crt_behavior_next_action,done:.crt_coverage_done,total:.crt_coverage_total,families:.crt_unreferenced_family_count}") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "scratch:.scratch_root") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "tmp:.tmp_dir") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "xdg:.xdg_cache_home") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "nytrix_cache:.nytrix_cache_dir") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_writer:.active_old_nytrix_output_writer_present") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_action:.old_path_next_action") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_seen:.old_path_present_count") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_current:.old_path_remaining_count") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_wait_s:.old_path_wait_remaining_seconds") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_leaks:.old_path_artifact_leak_count") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "artifact_remaining:.old_path_artifact_remaining_count") ||
          find_n(quick_jq_line, quick_jq_end,
                 "old_remaining:.old_path_artifact_remaining_count") ||
          find_n(quick_jq_line, quick_jq_end,
                 "score:{coverage_percent,campaign_percent") ||
          find_n(quick_jq_line, quick_jq_end,
                 "recommended_repeat_mode,recommended_repeat_count") ||
          find_n(quick_jq_line, quick_jq_end, ".md.json") ||
          find_n(quick_jq_line, quick_jq_end, "/home/e/nytrix"))
        (void)string_list_push_copy(
            errors, "status Controls omit quick jq readout");
      const char *status_jq_line = controls_section ?
             strstr(controls_section,
                    "jq {ok,cases,ok_count,failure_count,ready,blockers,active_count,"
                    "coverage_percent,coverage_queue_count,"
                    "coverage_queue_non_advisory_count,"
                    "coverage_queue_advisory_count,"
                    "coverage_queue_lanes,campaign_percent,"
                    "campaign_remaining_percent,thread_years,"
                 "target_thread_years,score_percent,stability_percent,"
                 "stability_score_percent,language_score_percent,"
                 "language_score_label,completion_state,"
                 "language_score_good_threshold_percent,"
                 "language_score_signal_percent,"
                 "language_score_evidence_cap_percent,language_score_note,"
                 "language_score_gap_percent,"
                 "next_run_language_score_percent,"
                 "next_run_language_score_delta_percent,"
                 "runs_to_good_language_score,runs_to_good_days,"
                 "runs_to_good_language_days,days_to_good_language_score,"
                 "days_to_good_stability,"
                 "reports,full_pressure_reports,checked_subcases,"
                 "full_pressure_thread_years,"
                 "latest_report,latest_full_pressure_report,"
                 "latest_full_pressure_raw_ok,"
                 "latest_full_pressure_effective_clean,"
                 "latest_full_pressure_clean_reason,"
                 "latest_full_pressure_failure_count,"
                 "latest_full_pressure_demoted_non_reproducing_afl_timeout,"
                  "recommended_action,recommended_reason,"
                  "recommended_command,recommended_preview_command,"
                  "recommended_repeat_mode,recommended_repeat_count,"
                  "recommended_state_fresh,"
                  "recommended_state_stale_after_seconds,"
                  "recommended_state_stale_reason,"
                  "recommended_state_refresh_required,"
                  "state,state_phase,state_event,state_age_seconds,"
                  "state_stale_after_seconds,state_fresh,"
                  "state_live,state_child_status,state_command,"
                  "state_refresh_command,state_stale_reason,"
                  "recommended_state,recommended_state_live,"
                  "recommended_state_age_seconds,"
                  "recommended_state_child_status,recommended_state_command,"
                  "recommended_state_refresh_command,"
                  "recommended_state_refresh_reason,"
                  "recommended_low_cpu_command,run_next_command,"
                  "run_next_preview_command,run_next_low_cpu_command,"
                  "run_next_gentle_command,run_next_gentle_preview_command,"
                  "coverage_next_action,"
                  "coverage_next_category,coverage_next_severity,"
                  "coverage_next_lane,coverage_next_reason,"
                  "coverage_next_command,coverage_next_guarded_command,"
                  "coverage_next_low_cpu_command,coverage_next_preview_command,"
                  "coverage_next_state_file,"
                  "coverage_next_state,coverage_next_state_phase,"
                  "coverage_next_state_event,coverage_next_state_readable,"
                  "coverage_next_state_fresh,coverage_next_state_live,"
                  "coverage_next_state_age_seconds,"
                  "coverage_next_state_stale_after_seconds,"
                  "coverage_next_state_stale_reason,"
                  "coverage_next_state_child_status,"
                  "coverage_next_state_command,"
                  "coverage_next_state_refresh_command,"
                  "coverage_next_state_refresh_required,"
                  "coverage_next_state_refresh_reason,coverage_next_stop_file,"
                  "coverage_next_stop_command,coverage_next_resume_command,"
                 "latest_report_fresh,latest_full_pressure_report_fresh,"
                 "latest_full_pressure_report_age_hours,"
                 "perf_watchlist_artifact_fresh,"
                 "perf_watchlist_artifact_age_seconds,"
                 "perf_watchlist_threshold_ratio,"
                 "perf_watchlist_command,perf_watchlist_report,"
                 "perf_watchlist_markdown,perf_watchlist_action,"
                 "perf_watchlist_action_command,"
                 "optimization_action,optimization_reason,"
                 "optimization_command,optimization_target_command,"
                 "optimization_case,optimization_ratio,"
                 "optimization_slowdown_percent,"
                 "perf_watchlist_artifact_hotspots,"
                 "perf_watchlist_artifact_max_ratio,"
                 "perf_watchlist_artifact_max_case,"
                 "advisory_state,advisory_recheck_state,"
                 "current_advisory_timeouts,"
                 "effective_advisory_timeouts,advisory_effective_timeouts,"
                 "advisory_penalty_state,"
                  "historical_non_reproducing_afl_timeouts,"
                  "advisory_recheck_raw_repro_checked,"
                  "advisory_recheck_raw_repro_passed,"
                  "advisory_recheck_raw_repro_timeouts,"
                     "advisory_recheck_raw_repro_unexpected,advisory_penalty,"
                     "old_nytrix_test_scratch_absent,old_nytrix_fuzz_absent,"
                     "old_nytrix_build_cache_absent,"
                     "active_old_nytrix_output_writer_present,old_path_report,"
                     "old_path_markdown,old_path_cache_policy_ok,"
                     "old_path_present_count,old_path_moved_count,"
                     "old_path_remaining_count,"
                     "old_path_wait_remaining_seconds,"
                     "old_path_artifact_leak_count,old_path_artifact_moved_count,"
                     "old_path_artifact_remaining_count,compiler_findings,"
                  "known_bug_replay_findings,runtime_surface_state,"
                  "runtime_exports,direct_runtime_refs,"
                     "runtime_surface_scope,runtime_coverage_done,runtime_coverage_total,"
                  "runtime_export_coverage_percent,"
                  "runtime_unreferenced_count,runtime_wrapper_gap_count,"
                     "crt_surface_state,crt_surface_scope,crt_behavior_state,"
                     "crt_behavior_scope,crt_behavior_next_action,"
                     "crt_behavior_next_reason,crt_behavior_next_command,"
                     "crt_runtime_exports,crt_direct_refs,"
                  "crt_coverage_done,crt_coverage_total,"
                  "crt_export_coverage_percent,"
                  "crt_unreferenced_percent,crt_unreferenced_count,"
                  "crt_wrapper_gap_count,crt_unreferenced_family_count,"
                  "crt_top_unreferenced_family,"
                  "crt_top_unreferenced_family_count,crt_next_action,"
                  "crt_next_unreferenced_family,"
                  "crt_next_unreferenced_count,perf_hotspots_open,"
                  "perf_worst_ratio,perf_worst_slowdown_percent,"
                     "perf_worst_case,latest_full_pressure_perf_hotspots,latest_full_pressure_perf_max_ratio,latest_full_pressure_perf_max_slowdown_percent,latest_full_pressure_perf_max_case,latest_full_pressure_perf_rows,latest_full_pressure_perf_suite_current,perf_watchlist_state}") :
          NULL;
      const char *status_jq_end =
          status_jq_line ? strchr(status_jq_line, '\n') : NULL;
      if (!status_jq_line || !status_jq_end ||
          !find_n(status_jq_line, status_jq_end, expected_status_json) ||
          find_n(status_jq_line, status_jq_end, ".md.json") ||
          find_n(status_jq_line, status_jq_end, "/home/e/nytrix"))
        (void)string_list_push_copy(
            errors, "status Controls omit compact jq readout");
      if (quick_jq_line && status_jq_line && quick_jq_line > status_jq_line)
        (void)string_list_push_copy(
            errors, "status Controls put deep jq before quick jq");
         const char *state_jq_line = controls_section ?
             strstr(controls_section,
                    "jq {state,event,live,child_status,stale_after_seconds,"
                    "repeat_mode,repeat_count,"
                    "handoff_low_priority,handoff_nice,handoff_load_wait,"
                    "handoff_max_load_pct,handoff_space_guard,"
                    "handoff_min_free_gb,handoff_run_lock,handoff_threads,"
                    "heartbeat_s,heartbeat_count,child_pid,"
                    "cycle,cycles,max_cycles,cooldown_s,timestamp_utc,"
                    "updated_at,started_at,finished_at,pid,campaign_dir,"
                    "stop_file,status_report,status_json,progress_report,"
                    "progress_json,dry_run_exceeds_max,dry_run_wall_hours,"
                    "dry_run_wall_days,dry_run_thread_years,"
                    "dry_run_campaign_gain_percent,"
                    "dry_run_target_percent_per_run,"
                    "dry_run_thread_years_per_run,canonical_status_report,"
                       "canonical_progress_report,last_report}") :
          NULL;
      const char *state_jq_end =
          state_jq_line ? strchr(state_jq_line, '\n') : NULL;
      if (!expect_missing_evidence_preview &&
          (!state_jq_line || !state_jq_end ||
           !find_n(state_jq_line, state_jq_end, expected_state_file)))
        (void)string_list_push_copy(
            errors, "status Controls omit compact state jq readout");
      if (!expect_missing_evidence_preview && controls_section) {
        const char *controls_end =
            strstr(controls_section + strlen("\n## Controls\n"), "\n## ");
        if (!controls_end) controls_end = md.data + strlen(md.data);
        char raw_state_line[8192];
        snprintf(raw_state_line, sizeof(raw_state_line), "cat %s",
                 expected_state_file);
        if (find_n(controls_section, controls_end, raw_state_line))
          (void)string_list_push_copy(
              errors, "status Controls duplicate raw state dump");
      }
      if (!expect_missing_evidence_preview && md.data) {
        char raw_state_inspect[8192];
        snprintf(raw_state_inspect, sizeof(raw_state_inspect),
                 "inspect `cat %s`", expected_state_file);
        if (strstr(md.data, raw_state_inspect))
          (void)string_list_push_copy(
              errors, "status State summary uses raw state cat");
      }
      if ((state_after_next && controls_section &&
           state_after_next < controls_section) ||
          (stop_after_next && controls_section &&
           stop_after_next < controls_section))
        (void)string_list_push_copy(errors,
                                    "status markdown mixes controls into Next");
      const char *progress_refresh_in_next =
          next_section ? strstr(next_section,
                                "fuzz all progress --refresh") : NULL;
      const char *status_refresh_in_next =
          next_section ? strstr(next_section,
                                "fuzz all status --refresh") : NULL;
      if (!expect_missing_evidence_preview &&
          ((progress_refresh_in_next && next_end &&
            progress_refresh_in_next < next_end) ||
           (status_refresh_in_next && next_end &&
            status_refresh_in_next < next_end)))
        (void)string_list_push_copy(
            errors,
            "status Next includes maintenance refresh commands");
      if (!expect_missing_evidence_preview &&
          expected_recommended_repeat[0] && next_section) {
        char repeat_line[8192];
        char raw_line[8192];
        char raw_line_dot[8192];
        snprintf(repeat_line, sizeof(repeat_line), "\n%s\n",
                 recommended_command ? recommended_command : "");
        snprintf(raw_line, sizeof(raw_line), "\n%s\n", expected_run_next);
        snprintf(raw_line_dot, sizeof(raw_line_dot), "\n./%s\n",
                 expected_run_next);
        int repeat_count = 0;
        const char *p = next_section;
        while ((p = strstr(p, repeat_line)) && p < next_end) {
          ++repeat_count;
          p += strlen(repeat_line) > 0 ? strlen(repeat_line) : 1;
        }
        const char *raw = strstr(next_section, raw_line);
        const char *raw_dot = strstr(next_section, raw_line_dot);
        const char *target_repeat = strstr(next_section,
                                           "NYTRIX_RUN_REPEAT=target");
        if (repeat_count != 1)
          (void)string_list_push_copy(
              errors,
              "status Next should contain one recommended repeat command");
        if ((raw && raw < next_end) || (raw_dot && raw_dot < next_end))
          (void)string_list_push_copy(
              errors, "status Next includes raw run-next after repeat choice");
        if (strcmp(expected_recommended_repeat, "good") == 0 &&
            target_repeat && target_repeat < next_end)
          (void)string_list_push_copy(
              errors,
              "status Next includes target alternate after good repeat choice");
      }
      if (!expect_missing_evidence_preview &&
          expected_recommended_repeat[0] &&
          recommended_preview_command && *recommended_preview_command) {
        if (!controls_section ||
            !strstr(controls_section, recommended_preview_command))
          (void)string_list_push_copy(
              errors,
              "status controls do not refresh the recommended repeat preview");
        if (controls_section &&
            strcmp(expected_recommended_repeat, "good") == 0 &&
            strstr(controls_section, "NYTRIX_RUN_REPEAT=target"))
          (void)string_list_push_copy(
              errors,
              "status controls refresh target instead of good recommendation");
      }
  }
    if (md.data && expect_missing_evidence_preview &&
        (strstr(md.data, expected_run_next) ||
         strstr(md.data, expected_state_file) ||
         strstr(md.data, "NYTRIX_RUN_REPEAT=") ||
       strstr(md.data, "fuzz all progress --refresh") ||
       strstr(md.data, "fuzz all status --refresh") ||
       strstr(md.data, "\nunknown\n```") ||
       strstr(md.data, "--no-nytrix --no-sanitizers")))
      (void)string_list_push_copy(
          errors, "status markdown leaks stale run-next fallback in missing-evidence Next block");
  if (md.data && stale_evidence && strstr(md.data, "NYTRIX_RUN_REPEAT="))
    (void)string_list_push_copy(
        errors, "stale status markdown should not suggest repeat handoff");
  if (md.data && expect_optimization_action_markdown &&
      (!strstr(md.data, "Optimization action:") ||
       !strstr(md.data, expected_optimization_action) ||
       !strstr(md.data, "perf-watchlist.md") ||
       (optimization_ratio > 0.0 &&
        !strstr(md.data, "slower than C"))))
    (void)string_list_push_copy(errors,
                                "status markdown omits optimization action");
  free(md.data);
  free(coverage_state);
  free(markdown);
  char *scratch_root = summary_string_from_report(json, "scratch_root");
  if (!scratch_root || strcmp(scratch_root, "build/cache/scratch") != 0)
    (void)string_list_push_copy(errors,
                                "status scratch root is not repo-relative");
  char nytrix_root[4096] = {0};
  if (find_nytrix_root(nytrix_root, sizeof(nytrix_root)) &&
      nytrix_root[0] && strstr(json, nytrix_root))
    (void)string_list_push_copy(errors,
                                "status report leaked absolute nytrix paths");
  char *ny_bin = summary_string_from_report(json, "ny_bin");
  if (!ny_bin || !*ny_bin)
    (void)string_list_push_copy(errors, "status Ny binary path is missing");
  char *run_command = summary_string_from_report(json, "run_command");
  if (!run_command ||
      !selftest_command_uses_env_nice(run_command) ||
      !strstr(run_command, "--target-thread-years") ||
      !strstr(run_command, "--dir") ||
      !strstr(run_command, "fuzz all run") ||
      strstr(run_command, "fuzz --runs"))
    (void)string_list_push_copy(errors, "status run command does not preserve target and report dir");
  char *next_command = summary_string_from_report(json, "next_command");
  char *next_handoff_command =
      summary_string_from_report(json, "next_handoff_command");
  if (!next_command ||
      !selftest_command_uses_env_nice(next_command) ||
      !strstr(next_command, expected_run_next) ||
      strstr(next_command, "fuzz_all_status_canonical.json"))
    (void)string_list_push_copy(errors,
                                "status next command lost guarded cockpit handoff");
  if (!next_handoff_command ||
      selftest_command_uses_env_nice(next_handoff_command) ||
      !strstr(next_handoff_command, expected_run_next) ||
      strstr(next_handoff_command, "fuzz_all_status_canonical.json"))
    (void)string_list_push_copy(
        errors, "status next handoff command missing raw cockpit script");
  char *preview_command = summary_string_from_report(json, "preview_command");
  const char *expected_root_repeat =
      expected_recommended_repeat[0] ? expected_recommended_repeat : "target";
  char expected_root_repeat_env[64] = {0};
  snprintf(expected_root_repeat_env, sizeof(expected_root_repeat_env),
           "NYTRIX_RUN_REPEAT=%s", expected_root_repeat);
  if (!preview_command ||
      !strstr(preview_command, "NYTRIX_RUN_DRY_RUN=1") ||
      (stale_evidence ?
           strstr(preview_command, "NYTRIX_RUN_REPEAT=") != NULL :
           strstr(preview_command, expected_root_repeat_env) == NULL) ||
      !strstr(preview_command, expected_run_next) ||
      strstr(preview_command, "fuzz_all_status_canonical.json"))
    (void)string_list_push_copy(errors, "status preview command lost cockpit handoff");
  char *run_next_command_alias =
      summary_string_from_report(json, "run_next_command");
  char *run_next_preview_command_alias =
      summary_string_from_report(json, "run_next_preview_command");
  char *run_next_low_cpu_command_alias =
      summary_string_from_report(json, "run_next_low_cpu_command");
  if (!run_next_command_alias ||
      strcmp(run_next_command_alias, next_command ? next_command : "") != 0)
    (void)string_list_push_copy(errors,
                                "status run-next command alias wrong");
  if (!run_next_preview_command_alias ||
      strcmp(run_next_preview_command_alias,
             preview_command ? preview_command : "") != 0)
    (void)string_list_push_copy(errors,
                                "status run-next preview alias wrong");
  if (!run_next_low_cpu_command_alias ||
      strcmp(run_next_low_cpu_command_alias,
             next_command ? next_command : "") != 0)
    (void)string_list_push_copy(errors,
                                "status run-next low-cpu alias wrong");
  char *run_next_gentle_command =
      summary_string_from_report(json, "run_next_gentle_command");
  if (!run_next_gentle_command ||
      !selftest_command_uses_env_nice(run_next_gentle_command) ||
      !strstr(run_next_gentle_command, "NYTRIX_RUN_HOURS=1") ||
      !strstr(run_next_gentle_command, "NYTRIX_RUN_THREADS=10%") ||
      !strstr(run_next_gentle_command, expected_run_next) ||
      strstr(run_next_gentle_command, "NYTRIX_RUN_DRY_RUN=1") ||
      strstr(run_next_gentle_command, "NYTRIX_RUN_REPEAT=") ||
      strstr(run_next_gentle_command, "fuzz_all_status_canonical.json"))
    (void)string_list_push_copy(errors,
                                "status gentle run command lost low-impact handoff");
  char *run_next_gentle_preview_command =
      summary_string_from_report(json, "run_next_gentle_preview_command");
  if (!run_next_gentle_preview_command ||
      !selftest_command_uses_env_nice(run_next_gentle_preview_command) ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_RUN_DRY_RUN=1") ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_RUN_HOURS=1") ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_RUN_THREADS=10%") ||
      !strstr(run_next_gentle_preview_command, expected_run_next) ||
      strstr(run_next_gentle_preview_command, "NYTRIX_RUN_REPEAT=") ||
      strstr(run_next_gentle_preview_command, "fuzz_all_status_canonical.json"))
    (void)string_list_push_copy(errors,
                                "status gentle preview command lost low-impact handoff");
  char *state_refresh_command =
      summary_string_from_report(json, "state_refresh_command");
  if (state_refresh_command && *state_refresh_command &&
      (!strstr(state_refresh_command, "NYTRIX_RUN_DRY_RUN=1") ||
      (stale_evidence ?
           strstr(state_refresh_command, "NYTRIX_RUN_REPEAT=") != NULL :
           strstr(state_refresh_command, expected_root_repeat_env) == NULL) ||
      !strstr(state_refresh_command, expected_run_next) ||
      strstr(state_refresh_command, "fuzz_all_status_canonical.json")))
    (void)string_list_push_copy(errors,
                                "status state refresh command lost cockpit handoff");
  if (!recommended_action ||
      strcmp(recommended_action, expected_recommended_action) != 0)
    (void)string_list_push_copy(errors, "status recommended action wrong");
  if (!recommended_reason || !*recommended_reason)
    (void)string_list_push_copy(errors, "status recommended reason missing");
  if (!recommended_command ||
      (expect_missing_evidence ?
           !(coverage_next_guarded_command &&
             *coverage_next_guarded_command ?
                 strstr(recommended_command,
                        coverage_next_guarded_command) != NULL :
                 strstr(recommended_command, coverage_next_command) != NULL) :
           (!strstr(recommended_command, stale_evidence ?
                        expected_run_next :
                        (runs_to_good > 0.0 ? "NYTRIX_RUN_REPEAT=good" :
                                               "NYTRIX_RUN_REPEAT=target")) ||
            !strstr(recommended_command, expected_run_next))))
    (void)string_list_push_copy(errors, "status recommended command wrong");
     if (stale_evidence &&
         (!recommended_command ||
          !selftest_command_uses_env_nice(recommended_command) ||
          strstr(recommended_command, "NYTRIX_RUN_REPEAT=")))
       (void)string_list_push_copy(
           errors, "status stale-evidence command is not low-impact");
  if (expect_missing_evidence && coverage_next_command &&
      strstr(coverage_next_command, "fuzz all run") &&
      (!recommended_command ||
       !strstr(recommended_command, "run-missing-evidence.sh")))
    (void)string_list_push_copy(
        errors, "status missing-evidence command lost guarded script");
  if (expect_missing_evidence) {
    if (!recommended_low_cpu_command || !coverage_next_low_cpu_command ||
        strcmp(recommended_low_cpu_command,
               coverage_next_low_cpu_command) != 0)
      (void)string_list_push_copy(
          errors, "status recommended low-cpu command alias wrong");
     } else if (recommended_command &&
                strstr(recommended_command, "NYTRIX_LOW_PRIORITY=1")) {
       if (!recommended_low_cpu_command ||
           strcmp(recommended_low_cpu_command, recommended_command) != 0 ||
           !selftest_command_uses_env_nice(recommended_low_cpu_command))
         (void)string_list_push_copy(
             errors, "status recommended low-cpu command lost selected handoff");
  } else if (recommended_low_cpu_command && *recommended_low_cpu_command) {
    (void)string_list_push_copy(
        errors, "status unexpected recommended low-cpu command");
  }
  if (expect_missing_evidence && coverage_next_lane &&
      strcmp(coverage_next_lane, "afl") == 0 &&
      (!coverage_next_command || !strstr(coverage_next_command, "--only-lane afl")))
    (void)string_list_push_copy(
        errors, "status missing-evidence AFL command lost lane filter");
  if (expect_missing_evidence_preview) {
    if (!recommended_preview_command ||
        !strstr(recommended_preview_command,
                coverage_next_preview_command) ||
        !strstr(recommended_preview_command, "fuzz all preflight") ||
        !strstr(recommended_preview_command,
                "--allow-dirty-nytrix-baseline") ||
        !strstr(recommended_preview_command, "build/cache/scratch"))
      (void)string_list_push_copy(
          errors, "status recommended missing-evidence preview command wrong");
  } else if (expected_recommended_repeat[0]) {
    if (!recommended_preview_command ||
        !strstr(recommended_preview_command, "NYTRIX_RUN_DRY_RUN=1") ||
        !strstr(recommended_preview_command, expected_recommended_repeat) ||
        !strstr(recommended_preview_command, expected_run_next))
      (void)string_list_push_copy(
          errors, "status recommended preview command wrong");
    if (!expect_missing_evidence &&
        have_recommended_state_refresh_required &&
        recommended_state_refresh_required) {
      if (!recommended_state_refresh_command ||
          strcmp(recommended_state_refresh_command,
                 recommended_preview_command ? recommended_preview_command : "") != 0)
        (void)string_list_push_copy(
            errors,
            "status recommended state refresh does not follow selected preview");
      if (recommended_state_refresh_command &&
          strcmp(expected_recommended_repeat, "good") == 0 &&
          strstr(recommended_state_refresh_command,
                 "NYTRIX_RUN_REPEAT=target"))
        (void)string_list_push_copy(
            errors,
            "status recommended state refresh targets instead of good");
    }
     } else if (stale_evidence) {
       if (!recommended_preview_command ||
           !selftest_command_uses_env_nice(recommended_preview_command) ||
           !strstr(recommended_preview_command, "NYTRIX_RUN_DRY_RUN=1") ||
           !strstr(recommended_preview_command, expected_run_next) ||
           strstr(recommended_preview_command, "NYTRIX_RUN_REPEAT="))
      (void)string_list_push_copy(
          errors, "status stale-evidence preview command wrong");
    if (have_recommended_state_refresh_required &&
        recommended_state_refresh_required &&
        (!recommended_state_refresh_command ||
         strcmp(recommended_state_refresh_command,
                recommended_preview_command ? recommended_preview_command :
                                              "") != 0))
      (void)string_list_push_copy(
          errors,
          "status stale-evidence state refresh does not follow preview");
  } else if (recommended_preview_command && *recommended_preview_command) {
    (void)string_list_push_copy(
        errors, "status unexpected recommended preview command");
  }
  if (!recommended_repeat_mode ||
      strcmp(recommended_repeat_mode, expected_recommended_repeat) != 0)
    (void)string_list_push_copy(errors, "status recommended repeat mode wrong");
  if (!summary_number_from_report(json, "recommended_repeat_count",
                                  &recommended_repeat_count) ||
      recommended_repeat_count != expected_recommended_repeat_count)
    (void)string_list_push_copy(errors, "status recommended repeat count wrong");
  char *recommended_state_source =
      summary_string_from_report(json, "recommended_state_source");
  char *recommended_state_command =
      summary_string_from_report(json, "recommended_state_command");
  char *recommended_state =
      summary_string_from_report(json, "recommended_state");
  char *recommended_state_stale_reason =
      summary_string_from_report(json, "recommended_state_stale_reason");
  bool recommended_state_fresh = false;
  bool have_recommended_state_fresh =
      summary_bool_from_report(json, "recommended_state_fresh",
                               &recommended_state_fresh);
  if (expect_missing_evidence) {
    if (!recommended_state_source ||
        strcmp(recommended_state_source, "coverage-next") != 0 ||
        !recommended_state_command ||
        !strstr(recommended_state_command,
                "run-missing-evidence-state.json") ||
        !recommended_state ||
        (coverage_next_state &&
         strcmp(recommended_state, coverage_next_state) != 0) ||
        !recommended_state_stale_reason ||
        (coverage_next_state_stale_reason &&
         strcmp(recommended_state_stale_reason,
                coverage_next_state_stale_reason) != 0) ||
        !have_recommended_state_fresh)
      (void)string_list_push_copy(
          errors, "status recommended state aliases do not follow coverage-next");
  } else if (!recommended_state_source ||
             strcmp(recommended_state_source, "run") != 0 ||
             !recommended_state_command ||
             !strstr(recommended_state_command,
                     "jq {state,event,live,child_status,stale_after_seconds,"
                     "repeat_mode,repeat_count,") ||
             !strstr(recommended_state_command, "dry_run_wall_hours") ||
             !strstr(recommended_state_command, "canonical_status_report") ||
             !strstr(recommended_state_command, expected_state_file) ||
             !have_recommended_state_fresh) {
    (void)string_list_push_copy(
        errors, "status recommended state aliases do not follow run state");
  }
  char *recommended_state_canonical_status =
      summary_string_from_report(json,
                                 "recommended_state_canonical_status_report");
  char *recommended_state_canonical_progress =
      summary_string_from_report(json,
                                 "recommended_state_canonical_progress_report");
  if (canonical_pair && !stale_evidence && !expect_missing_evidence) {
    double recommended_state_dry_wall_hours = -1.0;
    double recommended_state_dry_thread_years = -1.0;
    double recommended_state_dry_gain = -1.0;
    if (!summary_number_from_report(json,
                                    "recommended_state_dry_run_wall_hours",
                                    &recommended_state_dry_wall_hours) ||
        !summary_number_from_report(json,
                                    "recommended_state_dry_run_thread_years",
                                    &recommended_state_dry_thread_years) ||
        !summary_number_from_report(
            json, "recommended_state_dry_run_campaign_gain_percent",
            &recommended_state_dry_gain) ||
        !recommended_state_canonical_status ||
        !recommended_state_canonical_progress)
      (void)string_list_push_copy(
          errors, "status recommended state dry-run metrics missing");
  }
  free(coverage_next_action);
  free(coverage_next_lane);
  free(coverage_next_severity);
  free(coverage_next_category);
  free(coverage_next_reason);
  free(coverage_next_command);
  free(coverage_next_guarded_command);
  free(coverage_next_low_cpu_command);
  free(coverage_next_preview_command);
  free(coverage_next_state_file);
  free(coverage_next_state_command);
  free(coverage_next_state_refresh_command);
  free(coverage_next_state_refresh_reason);
  free(recommended_state_refresh_command);
  free(coverage_next_state);
  free(coverage_next_state_stale_reason);
  free(coverage_next_state_phase);
  free(coverage_next_state_child_status);
  free(coverage_next_stop_file);
  free(coverage_next_stop_command);
  free(coverage_next_resume_command);
  free(recommended_state_source);
  free(recommended_state_command);
  free(recommended_state);
  free(recommended_state_stale_reason);
  free(recommended_state_canonical_status);
  free(recommended_state_canonical_progress);
  bool handoff_low_priority = false, handoff_load_wait = false;
  bool handoff_space_guard = false, handoff_run_lock = false;
  double handoff_nice = -1.0, handoff_max_load = -1.0;
  double handoff_min_free = -1.0;
  char *handoff_threads =
      summary_string_from_report(json, "handoff_threads_default");
  char *handoff_summary =
      summary_string_from_report(json, "handoff_guard_summary");
  if (!summary_bool_from_report(json, "handoff_low_priority_default",
                                &handoff_low_priority) ||
      !handoff_low_priority ||
      !summary_number_from_report(json, "handoff_nice_default",
                                  &handoff_nice) ||
      handoff_nice != 10.0 ||
      !summary_bool_from_report(json, "handoff_load_wait_default",
                                &handoff_load_wait) ||
      !handoff_load_wait ||
      !summary_number_from_report(json, "handoff_max_load_pct_default",
                                  &handoff_max_load) ||
      handoff_max_load != 75.0 ||
      !summary_bool_from_report(json, "handoff_space_guard_default",
                                &handoff_space_guard) ||
      !handoff_space_guard ||
      !summary_number_from_report(json, "handoff_min_free_gb_default",
                                  &handoff_min_free) ||
      handoff_min_free != 20.0 ||
      !summary_bool_from_report(json, "handoff_run_lock_default",
                                &handoff_run_lock) ||
      !handoff_run_lock ||
      !handoff_threads || strcmp(handoff_threads, "25%") != 0 ||
      !handoff_summary || !strstr(handoff_summary, "low-priority nice 10"))
    (void)string_list_push_copy(errors,
                                "status handoff guard defaults wrong");
  free(handoff_threads);
  free(handoff_summary);
  free(recommended_action);
  free(recommended_reason);
  free(recommended_command);
  free(recommended_low_cpu_command);
  free(recommended_preview_command);
  free(recommended_repeat_mode);
  free(run_next_command_alias);
  free(run_next_preview_command_alias);
  free(run_next_low_cpu_command_alias);
  free(run_next_gentle_command);
  free(run_next_gentle_preview_command);
  char *stop_file = summary_string_from_report(json, "stop_file");
  char *stop_command = summary_string_from_report(json, "stop_command");
  char *resume_command = summary_string_from_report(json, "resume_command");
  char *state_file = summary_string_from_report(json, "state_file");
  char *state_command = summary_string_from_report(json, "state_command");
  bool stop_file_is_canonical =
      strcmp(expected_stop_file, "build/fuzz/all/stop") == 0;
  bool state_file_is_canonical =
      strcmp(expected_state_file, "build/fuzz/all/run-next-state.json") == 0;
  if (!stop_file || !strstr(stop_file, expected_stop_file) ||
      (!stop_file_is_canonical && strstr(stop_file, "build/fuzz/all/stop")))
    (void)string_list_push_copy(errors, "status stop file lost cockpit path");
  if (!stop_command || !strstr(stop_command, "touch ") ||
      !strstr(stop_command, expected_stop_file) ||
      (!stop_file_is_canonical && strstr(stop_command, "build/fuzz/all/stop")))
    (void)string_list_push_copy(errors, "status stop command lost cockpit path");
  if (!resume_command || !strstr(resume_command, "rm -f ") ||
      !strstr(resume_command, expected_stop_file) ||
      (!stop_file_is_canonical && strstr(resume_command, "build/fuzz/all/stop")))
    (void)string_list_push_copy(errors, "status resume command lost cockpit path");
  if (!state_file || !strstr(state_file, expected_state_file) ||
      (!state_file_is_canonical &&
       strstr(state_file, "build/fuzz/all/run-next-state.json")))
    (void)string_list_push_copy(errors, "status state file lost cockpit path");
  if (!state_command ||
      !strstr(state_command,
              "jq {state,event,live,child_status,stale_after_seconds,repeat_mode,repeat_count,") ||
      !strstr(state_command, "dry_run_wall_hours") ||
      !strstr(state_command, "canonical_status_report") ||
      !strstr(state_command, expected_state_file) ||
      (!state_file_is_canonical &&
       strstr(state_command, "build/fuzz/all/run-next-state.json")))
    (void)string_list_push_copy(errors, "status state command lost cockpit path");
  bool state_readable = true, state_fresh = true, state_live = false;
  bool state_child_alive = false;
  double state_age = -2.0, state_heartbeat_s = -1.0;
  double state_stale_after = -1.0, state_heartbeat_count = -2.0;
  double state_pid = -1.0;
  if (!summary_bool_from_report(json, "state_readable", &state_readable))
    (void)string_list_push_copy(errors, "status state readable field missing");
  if (!summary_bool_from_report(json, "state_fresh", &state_fresh))
    (void)string_list_push_copy(errors, "status state fresh field missing");
  if (!summary_bool_from_report(json, "state_live", &state_live))
    (void)string_list_push_copy(errors, "status state live field missing");
  if (!summary_bool_from_report(json, "state_child_alive", &state_child_alive))
    (void)string_list_push_copy(errors, "status state child alive field missing");
  char *state_child_status =
      summary_string_from_report(json, "state_child_status");
  if (!state_child_status)
    (void)string_list_push_copy(errors, "status state child status missing");
  char *state_stale_reason =
      summary_string_from_report(json, "state_stale_reason");
  if (!state_stale_reason)
    (void)string_list_push_copy(errors, "status state stale reason missing");
  char *state_label = summary_string_from_report(json, "state");
  if (!state_label)
    (void)string_list_push_copy(errors, "status state label missing");
  char *state_phase = summary_string_from_report(json, "state_phase");
  if (!state_phase)
    (void)string_list_push_copy(errors, "status state phase missing");
  if (!summary_number_from_report(json, "state_age_seconds", &state_age))
    (void)string_list_push_copy(errors, "status state age missing");
  if (!summary_number_from_report(json, "state_stale_after_seconds",
                                  &state_stale_after))
    (void)string_list_push_copy(errors, "status state stale threshold missing");
  if (!summary_number_from_report(json, "state_heartbeat_s",
                                  &state_heartbeat_s))
    (void)string_list_push_copy(errors, "status state heartbeat interval missing");
  if (!summary_number_from_report(json, "state_heartbeat_count",
                                  &state_heartbeat_count))
    (void)string_list_push_copy(errors, "status state heartbeat count missing");
  if (!summary_number_from_report(json, "state_child_pid", &state_pid))
    (void)string_list_push_copy(errors, "status state child pid missing");
  bool state_handoff_low_priority = false, state_handoff_load_wait = false;
  bool state_handoff_space_guard = false, state_handoff_run_lock = false;
  double state_handoff_nice = -1.0, state_handoff_max_load = -1.0;
  double state_handoff_min_free = -1.0;
  char *state_handoff_threads =
      summary_string_from_report(json, "state_handoff_threads");
  if (!summary_bool_from_report(json, "state_handoff_low_priority",
                                &state_handoff_low_priority) ||
      !state_handoff_low_priority ||
      !summary_number_from_report(json, "state_handoff_nice",
                                  &state_handoff_nice) ||
      state_handoff_nice != 10.0 ||
      !summary_bool_from_report(json, "state_handoff_load_wait",
                                &state_handoff_load_wait) ||
      !state_handoff_load_wait ||
      !summary_number_from_report(json, "state_handoff_max_load_pct",
                                  &state_handoff_max_load) ||
      state_handoff_max_load != 75.0 ||
      !summary_bool_from_report(json, "state_handoff_space_guard",
                                &state_handoff_space_guard) ||
      !state_handoff_space_guard ||
      !summary_number_from_report(json, "state_handoff_min_free_gb",
                                  &state_handoff_min_free) ||
      state_handoff_min_free != 20.0 ||
      !summary_bool_from_report(json, "state_handoff_run_lock",
                                &state_handoff_run_lock) ||
      !state_handoff_run_lock ||
      !state_handoff_threads || strcmp(state_handoff_threads, "25%") != 0)
    (void)string_list_push_copy(errors,
                                "status state handoff guard fields wrong");
  free(state_handoff_threads);
  if (state_child_status &&
      strcmp(state_child_status,
             fuzz_all_state_child_status(state_pid, state_child_alive)) != 0)
    (void)string_list_push_copy(errors, "status state child status wrong");
  if (state_stale_reason &&
      strcmp(state_stale_reason,
             fuzz_all_state_stale_reason_values(state_readable, state_fresh,
                                                state_phase ? state_phase : "",
                                                state_age, state_heartbeat_s,
                                                state_pid, state_child_alive)) != 0)
    (void)string_list_push_copy(errors, "status state stale reason wrong");
  if (state_phase &&
      state_live != fuzz_all_state_phase_live(state_phase))
    (void)string_list_push_copy(errors, "status state live flag wrong");
  if (state_label &&
      strcmp(state_label,
             fuzz_all_state_label_values(state_readable,
                                         state_phase ? state_phase : "")) != 0)
    (void)string_list_push_copy(errors, "status state label wrong");
  char *state_canonical_status =
      summary_string_from_report(json, "state_canonical_status_report");
  char *state_canonical_progress =
      summary_string_from_report(json, "state_canonical_progress_report");
  if (canonical_pair && !stale_evidence && !expect_missing_evidence) {
    double state_dry_wall_hours = -1.0;
    double state_dry_thread_years = -1.0;
    double state_dry_gain = -1.0;
    if (!summary_number_from_report(json, "state_dry_run_wall_hours",
                                    &state_dry_wall_hours) ||
        !summary_number_from_report(json, "state_dry_run_thread_years",
                                    &state_dry_thread_years) ||
        !summary_number_from_report(json,
                                    "state_dry_run_campaign_gain_percent",
                                    &state_dry_gain) ||
        !state_canonical_status ||
        !state_canonical_progress)
      (void)string_list_push_copy(errors,
                                  "status state dry-run metrics missing");
  }
  double expected_stale_after =
      fuzz_all_state_stale_after_seconds(state_heartbeat_s);
    if (state_stale_after >= 0.0 &&
        (state_stale_after < expected_stale_after - 0.5 ||
         state_stale_after > expected_stale_after + 0.5))
      (void)string_list_push_copy(errors, "status state stale threshold wrong");
    bool expected_state_fresh =
        fuzz_all_state_fresh_values(state_readable,
                                    state_phase ? state_phase : "",
                                    state_age, state_heartbeat_s,
                                    state_pid, state_child_alive);
    if (state_fresh != expected_state_fresh)
      (void)string_list_push_copy(errors,
                                  "status state freshness ignores age");
    bool expect_state_refresh =
        !state_readable || (!state_live && !state_fresh);
    if (!expect_state_refresh &&
        state_refresh_command && *state_refresh_command)
      (void)string_list_push_copy(
          errors, "status fresh/live state exposed refresh command");
    if (expect_state_refresh &&
        (!state_refresh_command || !*state_refresh_command ||
         !strstr(state_refresh_command, "NYTRIX_RUN_DRY_RUN=1") ||
         (stale_evidence ?
              strstr(state_refresh_command, "NYTRIX_RUN_REPEAT=") != NULL :
              strstr(state_refresh_command, expected_root_repeat_env) == NULL) ||
         !strstr(state_refresh_command, expected_run_next) ||
         strstr(state_refresh_command, "fuzz_all_status_canonical.json")))
      (void)string_list_push_copy(
          errors, "status stale/missing state refresh command missing");
    if (!state_readable)
      (void)string_list_push_copy(errors,
                                  "status stale dry-run state was not readable");
    if (state_live)
      (void)string_list_push_copy(errors,
                                  "status stale dry-run state was reported live");
    if (state_fresh)
      (void)string_list_push_copy(errors,
                                  "status stale dry-run state was reported fresh");
    if (state_age <= state_stale_after)
      (void)string_list_push_copy(errors,
                                  "status stale dry-run age did not exceed threshold");
    if (state_stale_reason && strcmp(state_stale_reason, "old-state") != 0)
      (void)string_list_push_copy(errors,
                                  "status stale dry-run reason was not old-state");
    (void)state_heartbeat_count;
    free(state_child_status);
    free(state_stale_reason);
    char *freshness_action =
        summary_string_from_report(json, "freshness_action_command");
    char *latest_freshness_action =
        summary_string_from_report(json, "latest_report_freshness_command");
    char *full_pressure_freshness_action = summary_string_from_report(
        json, "latest_full_pressure_report_freshness_command");
    char *full_pressure_freshen_command =
        summary_string_from_report(json, "full_pressure_freshen_command");
    char *full_pressure_remediation_command =
        summary_string_from_report(json, "full_pressure_remediation_command");
    char *full_pressure_action_command =
        summary_string_from_report(json, "full_pressure_action_command");
    if (!freshness_action)
      (void)string_list_push_copy(errors,
                                  "status freshness action command is missing");
    else if (stale_evidence) {
      if (!selftest_command_uses_env_nice(freshness_action) ||
          strstr(freshness_action, "NYTRIX_RUN_REPEAT=") ||
          !strstr(freshness_action, expected_run_next))
        (void)string_list_push_copy(errors,
                                    "stale status freshness action lost handoff");
      if (!latest_freshness_action ||
          !selftest_command_uses_env_nice(latest_freshness_action) ||
          strstr(latest_freshness_action, "NYTRIX_RUN_REPEAT=") ||
          !strstr(latest_freshness_action, expected_run_next))
        (void)string_list_push_copy(
            errors, "stale status latest freshness command lost handoff");
      if (!full_pressure_freshness_action ||
          !selftest_command_uses_env_nice(full_pressure_freshness_action) ||
          strstr(full_pressure_freshness_action, "NYTRIX_RUN_REPEAT=") ||
          !strstr(full_pressure_freshness_action, expected_run_next))
        (void)string_list_push_copy(
            errors,
            "stale status full-pressure freshness command lost handoff");
      if (!full_pressure_freshen_command ||
          !selftest_command_uses_env_nice(full_pressure_freshen_command) ||
          strstr(full_pressure_freshen_command, "NYTRIX_RUN_REPEAT=") ||
          !strstr(full_pressure_freshen_command, expected_run_next))
        (void)string_list_push_copy(
            errors, "stale status full-pressure freshen command lost handoff");
      if (!full_pressure_remediation_command ||
          !selftest_command_uses_env_nice(full_pressure_remediation_command) ||
          strstr(full_pressure_remediation_command, "NYTRIX_RUN_REPEAT=") ||
          !strstr(full_pressure_remediation_command, expected_run_next))
        (void)string_list_push_copy(
            errors,
            "stale status full-pressure remediation command lost handoff");
      if (!full_pressure_action_command ||
          !selftest_command_uses_env_nice(full_pressure_action_command) ||
          strstr(full_pressure_action_command, "NYTRIX_RUN_REPEAT=") ||
          !strstr(full_pressure_action_command, expected_run_next))
        (void)string_list_push_copy(
            errors, "stale status full-pressure action command lost handoff");
    } else if (*freshness_action)
      (void)string_list_push_copy(errors,
                                  "fresh status unexpectedly has freshness action");
  char *progress_command = summary_string_from_report(json, "progress_command");
  if (!progress_command ||
      !selftest_command_uses_env_nice(progress_command) ||
      !strstr(progress_command, "fuzz all progress --refresh --strict") ||
      !strstr(progress_command, "--allow-full-pressure-remediation") ||
      !strstr(progress_command, "--dir") ||
      !strstr(progress_command, "--status") ||
      !strstr(progress_command, expected_progress_status_json) ||
      !strstr(progress_command, "--history") ||
      !strstr(progress_command, "--worklist") ||
      !strstr(progress_command, "--coverage") ||
      !strstr(progress_command, "--plan") ||
      !strstr(progress_command, "--target-thread-years") ||
      !strstr(progress_command, "--hours") ||
      !strstr(progress_command, "--threads") ||
      !strstr(progress_command, "--profile") ||
      !strstr(progress_command, cockpit_dir) ||
      !strstr(progress_command, expected_progress_json) ||
      !strstr(progress_command, expected_progress_md) ||
      strstr(progress_command, "fuzz_all_status_canonical.json"))
    (void)string_list_push_copy(errors,
                                "status progress command lost low-priority cockpit paths");
      char *status_command = summary_string_from_report(json, "status_command");
    if (!status_command ||
        !selftest_command_uses_env_nice(status_command) ||
        !strstr(status_command, "fuzz all status --refresh") ||
      !strstr(status_command, "--dir") ||
      !strstr(status_command, "--history") ||
      !strstr(status_command, "--worklist") ||
      !strstr(status_command, "--coverage") ||
      !strstr(status_command, "--plan") ||
      !strstr(status_command, "--target-thread-years") ||
      !strstr(status_command, "--hours") ||
      !strstr(status_command, "--threads") ||
      !strstr(status_command, "--profile") ||
        !strstr(status_command, "--json") ||
        !strstr(status_command, "--markdown"))
      (void)string_list_push_copy(errors, "status command does not preserve low-priority cockpit geometry");
    if (canonical_pair &&
        (!status_command ||
         !strstr(status_command, expected_status_json) ||
         !strstr(status_command, expected_status_md) ||
         strstr(status_command, "fuzz_all_status_canonical.json")))
      (void)string_list_push_copy(errors,
                                  "canonical status command used selftest row json");
    char *old_path_probe_command =
        summary_string_from_report(json, "old_path_probe_command");
    char *old_path_command = summary_string_from_report(json, "old_path_command");
    char *old_path_dry_run_command =
        summary_string_from_report(json, "old_path_dry_run_command");
    char *old_path_apply_command =
        summary_string_from_report(json, "old_path_apply_command");
    char *old_path_next_action =
        summary_string_from_report(json, "old_path_next_action");
    char *old_path_next_reason =
        summary_string_from_report(json, "old_path_next_reason");
    if (!old_path_probe_command ||
        !selftest_command_uses_env_nice(old_path_probe_command) ||
        strcmp(old_path_probe_command, NYTRIX_FUZZ_ALL_OLD_PATH_PROBE_COMMAND) !=
            0 ||
        !strstr(old_path_probe_command, "--probe"))
      (void)string_list_push_copy(errors,
                                  "status old-path probe command is missing");
    if (!old_path_command ||
        !selftest_command_uses_env_nice(old_path_command) ||
        !strstr(old_path_command, "fuzz all old-paths --dry-run") ||
        !strstr(old_path_command, "--nytrix-root ../nytrix") ||
        !strstr(old_path_command, "--archive-dir build/cache/old-nytrix") ||
        !strstr(old_path_command, "--json") ||
        !strstr(old_path_command, "old-paths.json") ||
        !strstr(old_path_command, "--markdown") ||
        !strstr(old_path_command, "old-paths.md"))
      (void)string_list_push_copy(errors, "status old-path cleanup command is missing");
    if (!old_path_command ||
        !strstr(old_path_command, expected_old_paths_json) ||
        !strstr(old_path_command, expected_old_paths_md) ||
        strstr(old_path_command, "build/fuzz/all/old-paths"))
      (void)string_list_push_copy(errors, "status old-path cleanup command lost custom cockpit");
    if (!old_path_dry_run_command ||
        !selftest_command_uses_env_nice(old_path_dry_run_command) ||
        !strstr(old_path_dry_run_command,
                "fuzz all old-paths --dry-run") ||
        !strstr(old_path_dry_run_command,
                "--archive-dir build/cache/old-nytrix") ||
        !strstr(old_path_dry_run_command, expected_old_paths_json) ||
        !strstr(old_path_dry_run_command, expected_old_paths_md))
      (void)string_list_push_copy(errors,
                                  "status old-path dry-run command missing");
    if (!old_path_apply_command ||
        !selftest_command_uses_env_nice(old_path_apply_command) ||
        !strstr(old_path_apply_command, "fuzz all old-paths --apply") ||
        !strstr(old_path_apply_command, "--wait-writers-s 300") ||
        !strstr(old_path_apply_command,
                "--archive-dir build/cache/old-nytrix") ||
        !strstr(old_path_apply_command, expected_old_paths_json) ||
        !strstr(old_path_apply_command, expected_old_paths_md))
      (void)string_list_push_copy(errors,
                                  "status old-path apply command missing");
    if (!old_path_next_action || !*old_path_next_action ||
        !old_path_next_reason || !*old_path_next_reason)
      (void)string_list_push_copy(errors,
                                  "status old-path next action missing");
    char *old_path_report = NULL;
    const char *old_path_json_arg =
        old_path_command ? strstr(old_path_command, "--json ") : NULL;
    if (old_path_json_arg) {
      old_path_json_arg += strlen("--json ");
      const char *end = old_path_json_arg;
      while (*end && !isspace((unsigned char)*end)) ++end;
      old_path_report = strndup_local(old_path_json_arg,
                                      (size_t)(end - old_path_json_arg));
    }
    file_buf_t old_path_report_buf = {0};
    if (!old_path_report || !read_file(old_path_report, &old_path_report_buf) ||
        !old_path_report_buf.data) {
      (void)string_list_push_copy(errors,
                                  "status refresh did not write old-path report");
    } else if (!strstr(old_path_report_buf.data,
                       "\"mode\":\"fuzz-all-old-paths\"") ||
               !strstr(old_path_report_buf.data,
                       "\"dry_run\":true")) {
      (void)string_list_push_copy(errors,
                                  "status old-path report has wrong shape");
    }
    free(old_path_report_buf.data);
    free(old_path_report);
    char *old_path_summary_report =
        summary_string_from_report(json, "old_path_report");
    char *old_path_summary_markdown =
        summary_string_from_report(json, "old_path_markdown");
    bool old_path_cache_policy_ok = false;
    double old_path_artifact_leak_count = -1.0;
    double old_path_artifact_moved_count = -1.0;
    double old_path_artifact_remaining_count = -1.0;
    double old_path_wait_remaining_seconds = -2.0;
    if (!old_path_summary_report ||
        !strstr(old_path_summary_report, expected_old_paths_json) ||
        strstr(old_path_summary_report, "build/fuzz/all/old-paths"))
      (void)string_list_push_copy(errors,
                                  "status old-path report lost cockpit path");
    if (!old_path_summary_markdown ||
        !strstr(old_path_summary_markdown, expected_old_paths_md) ||
        strstr(old_path_summary_markdown, "build/fuzz/all/old-paths"))
      (void)string_list_push_copy(errors,
                                  "status old-path markdown lost cockpit path");
    if (!summary_bool_from_report(json, "old_path_cache_policy_ok",
                                  &old_path_cache_policy_ok) ||
        !old_path_cache_policy_ok)
      (void)string_list_push_copy(errors,
                                  "status old-path cache policy mirror was not ok");
    if (!summary_number_from_report(json, "old_path_wait_remaining_seconds",
                                    &old_path_wait_remaining_seconds))
      (void)string_list_push_copy(errors,
                                  "status old-path wait estimate missing");
    if (!summary_number_from_report(json, "old_path_artifact_leak_count",
                                    &old_path_artifact_leak_count) ||
        old_path_artifact_leak_count != 0.0)
      (void)string_list_push_copy(errors,
                                  "status old-path artifact leak count was not zero");
    if (!summary_number_from_report(json, "old_path_artifact_moved_count",
                                    &old_path_artifact_moved_count) ||
        old_path_artifact_moved_count != 0.0)
      (void)string_list_push_copy(errors,
                                  "status old-path artifact moved count was not zero");
    if (!summary_number_from_report(json, "old_path_artifact_remaining_count",
                                    &old_path_artifact_remaining_count) ||
        old_path_artifact_remaining_count != 0.0)
      (void)string_list_push_copy(errors,
                                  "status old-path artifact remaining count was not zero");
    char *historical_worklist =
        summary_string_from_report(json, "historical_worklist_report");
  char *historical_worklist_md =
      summary_string_from_report(json, "historical_worklist_markdown");
    if (!historical_worklist ||
        !strstr(historical_worklist, expected_worklist_history_json) ||
        strstr(historical_worklist, "build/fuzz/all/worklist-history"))
      (void)string_list_push_copy(errors,
                                  "status historical worklist report lost custom cockpit");
    if (!historical_worklist_md ||
        !strstr(historical_worklist_md, expected_worklist_history_md) ||
        strstr(historical_worklist_md, "build/fuzz/all/worklist-history"))
      (void)string_list_push_copy(errors,
                                  "status historical worklist markdown lost custom cockpit");
    char *next_script = summary_string_from_report(json, "next_script");
    if (!next_script || !strstr(next_script, expected_run_next))
      (void)string_list_push_copy(errors,
                                  "next-run script path lost expected cockpit");
  char script_path[4096] = {0};
  if (next_script && *next_script && path_is_absolute(next_script)) {
    snprintf(script_path, sizeof(script_path), "%s", next_script);
  } else if (next_script && *next_script) {
    char nytrix_root[4096], absolute[4096];
    if (find_nytrix_root(nytrix_root, sizeof(nytrix_root)) &&
        path_join(absolute, sizeof(absolute), nytrix_root, next_script)) {
      snprintf(script_path, sizeof(script_path), "%s", absolute);
    } else {
      snprintf(script_path, sizeof(script_path), "%s", next_script);
    }
  }
  bool next_script_exists = script_path[0] && path_exists_file(script_path);
  if (!next_script_exists)
    (void)string_list_push_copy(errors, "next-run script was not generated");
  else {
    file_buf_t script = {0};
    if (!read_file(script_path, &script) || !script.data) {
      (void)string_list_push_copy(errors, "next-run script was not readable");
    } else {
        if (!strstr(script.data, "latest_coverage=") ||
            !strstr(script.data, "fuzz all coverage --strict --history") ||
            !strstr(script.data, "--json \"$latest_coverage\"") ||
            !strstr(script.data, "--coverage \"$latest_coverage\"") ||
            !strstr(script.data, "--target-thread-years") ||
            !strstr(script.data, "--hours") ||
            !strstr(script.data, "--threads") ||
            !strstr(script.data, "--profile") ||
            count_sub(script.data, strlen(script.data),
                      "fuzz all coverage --strict --history") < 3)
          (void)string_list_push_copy(errors, "next-run script does not refresh canonical history coverage");
        if (!strstr(script.data, "old_paths=") ||
            !strstr(script.data, "old_paths_md=") ||
            !strstr(script.data, "fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json \"$old_paths\" --markdown \"$old_paths_md\"") ||
            count_sub(script.data, strlen(script.data), "fuzz all old-paths --dry-run") < 2 ||
            !strstr(script.data, expected_old_paths_json) ||
            !strstr(script.data, expected_old_paths_md) ||
            strstr(script.data, "build/fuzz/all/old-paths"))
          (void)string_list_push_copy(errors, "next-run script does not refresh old-path diagnostics in its cockpit dir");
        if (!strstr(script.data, "progress=") ||
            !strstr(script.data, "progress_md=") ||
            !strstr(script.data, "repeat_status=") ||
            !strstr(script.data, "repeat_status_md=") ||
            !strstr(script.data, "repeat_progress=") ||
            !strstr(script.data, "repeat_progress_md=") ||
            !strstr(script.data, "fuzz all progress --strict --allow-full-pressure-remediation --dir") ||
            !strstr(script.data, "--status \"$repeat_status\" --history \"$history\"") ||
            !strstr(script.data, "--json \"$repeat_progress\" --markdown \"$repeat_progress_md\"") ||
            count_sub(script.data, strlen(script.data),
                      "--status \"$repeat_status\" --history \"$history\"") < 2 ||
            !strstr(script.data, "--status \"$status\" --history \"$history\"") ||
            !strstr(script.data, "--json \"$progress\" --markdown \"$progress_md\"") ||
            !strstr(script.data, expected_progress_json) ||
            !strstr(script.data, expected_progress_md) ||
            strstr(script.data, "build/fuzz/all/progress"))
          (void)string_list_push_copy(errors, "next-run script does not refresh compact progress in its cockpit dir");
        if (!strstr(script.data, "worklist_history=") ||
            !strstr(script.data, "worklist_history_md=") ||
            !strstr(script.data, "fuzz all worklist --history \"$history\" --include-history --json \"$worklist_history\" --markdown \"$worklist_history_md\"") ||
            count_sub(script.data, strlen(script.data),
                      "fuzz all worklist --history \"$history\" --include-history") < 2 ||
            !strstr(script.data, expected_worklist_history_json) ||
            !strstr(script.data, expected_worklist_history_md) ||
            strstr(script.data, "build/fuzz/all/worklist-history"))
          (void)string_list_push_copy(errors, "next-run script does not refresh historical worklist in its cockpit dir");
      if (!strstr(script.data, "set -euo pipefail") ||
          !strstr(script.data, "cd ") ||
          !shell_text_has_repo_cache_env(script.data))
        (void)string_list_push_copy(errors, "next-run script does not export repo-local cache env");
      if (!strstr(script.data, "NYTRIX_RUN_NICE=\"${NYTRIX_RUN_NICE:-10}\"") ||
          !strstr(script.data, "nytrix_low_priority()") ||
          !strstr(script.data, "ionice -c 3 nice -n \"$NYTRIX_RUN_NICE\"") ||
          !strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all run"))
        (void)string_list_push_copy(errors, "next-run script does not lower OS priority for heavy campaign runs");
      if (!strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all history") ||
          !strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all coverage --strict --history") ||
          !strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all worklist") ||
          !strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all plan") ||
          !strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all status") ||
          !strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all progress") ||
          !strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all old-paths") ||
          !strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all audit") ||
          !strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all findings") ||
          !strstr(script.data, "nytrix_low_priority ./build/nytrix fuzz all coverage --report"))
        (void)string_list_push_copy(errors, "next-run script does not lower OS priority for handoff refreshes");
      if (!strstr(script.data, "NYTRIX_MAX_LOAD_PCT=\"${NYTRIX_MAX_LOAD_PCT:-75}\"") ||
          !strstr(script.data, "NYTRIX_LOAD_SLEEP_S=\"${NYTRIX_LOAD_SLEEP_S:-60}\"") ||
          !strstr(script.data, "nytrix_wait_for_load()") ||
          !strstr(script.data, "/proc/loadavg") ||
          !strstr(script.data, "nytrix_wait_for_load\nnytrix_run_with_heartbeat nytrix_low_priority ./build/nytrix fuzz all run"))
        (void)string_list_push_copy(errors, "next-run script does not wait for system load before heavy campaign runs");
      if (!strstr(script.data, "NYTRIX_MIN_FREE_GB=\"${NYTRIX_MIN_FREE_GB:-20}\"") ||
          !strstr(script.data, "NYTRIX_SPACE_PATH=\"${NYTRIX_SPACE_PATH:-$NYTRIX_ROOT}\"") ||
          !strstr(script.data, "nytrix_require_free_space()") ||
          !strstr(script.data, "df -Pk \"$path\"") ||
          !strstr(script.data, "nytrix_require_free_space\nnytrix_wait_for_load\nnytrix_run_with_heartbeat nytrix_low_priority ./build/nytrix fuzz all run"))
        (void)string_list_push_copy(errors, "next-run script does not check free disk space before heavy campaign runs");
      if (!strstr(script.data, "NYTRIX_RUN_LOCK=\"${NYTRIX_RUN_LOCK:-1}\"") ||
          !strstr(script.data, "nytrix_acquire_campaign_lock()") ||
          !strstr(script.data, ".nytrix-run.lock") ||
          !strstr(script.data, "trap nytrix_release_campaign_lock EXIT") ||
          !strstr(script.data, "\nnytrix_acquire_campaign_lock "))
        (void)string_list_push_copy(errors, "next-run script does not lock the campaign before heavy handoff work");
      const char *dry_guard = strstr(script.data,
                                     "if [ \"$NYTRIX_RUN_DRY_RUN\" != \"0\" ]; then");
      const char *lock_call = strstr(script.data,
                                     "\nnytrix_acquire_campaign_lock ");
      if (!dry_guard || !lock_call || lock_call < dry_guard)
        (void)string_list_push_copy(errors,
                                    "next-run dry-run preview still takes campaign lock");
      const char *max_cycle_guard =
          strstr(script.data,
                 "resolved cycles $NYTRIX_RUN_REPEAT exceeds NYTRIX_RUN_MAX_CYCLES=$NYTRIX_RUN_MAX_CYCLES");
      if (!max_cycle_guard || !lock_call || lock_call < max_cycle_guard)
        (void)string_list_push_copy(errors,
                                    "next-run max-cycle guard runs after the campaign lock");
      const char *cooldown_guard =
          strstr(script.data,
                 "NYTRIX_RUN_COOLDOWN_S must be a non-negative integer");
      if (!cooldown_guard || !lock_call || lock_call < cooldown_guard)
        (void)string_list_push_copy(errors,
                                    "next-run cooldown guard runs after the campaign lock");
      const char *stop_prelock =
          strstr(script.data,
                 "stop file present before campaign: $NYTRIX_RUN_STOP_FILE");
      if (!stop_prelock || !lock_call || lock_call < stop_prelock) {
        (void)string_list_push_copy(errors,
                                    "next-run stop-file precheck runs after the campaign lock");
      }
      const char *pre_refresh = strstr(script.data,
                                       "refreshing status before repeat resolution");
      const char *missing_preview = strstr(
          script.data,
          "refreshed missing-evidence preview state=$missing_evidence");
      const char *repeat_case = strstr(script.data,
                                       "case \"$nytrix_repeat_mode\" in auto|good)");
      const char *missing_preview_gate = strstr(
          script.data,
          "grep -q '\"recommended_action\"[[:space:]]*:[[:space:]]*\"run-missing-evidence\"' \"$repeat_status\"");
      if (!missing_preview || !pre_refresh || missing_preview < pre_refresh ||
          !repeat_case || missing_preview > repeat_case ||
          !missing_preview_gate ||
          !strstr(script.data, "missing_evidence=") ||
          !strstr(script.data,
                  "NYTRIX_RUN_DRY_RUN=1 \"$missing_evidence\" >/dev/null 2>&1 || true"))
        (void)string_list_push_copy(
            errors,
            "next-run dry-run does not gate missing-evidence preview on repeat status");
      if (!pre_refresh || !repeat_case || repeat_case < pre_refresh ||
          !strstr(script.data, "repeat_status=") ||
          !strstr(script.data, "repeat_progress=") ||
          !strstr(script.data,
                  "fuzz all status --strict --allow-full-pressure-remediation --no-script") ||
          !strstr(script.data, "--next-command") ||
          count_sub(script.data, strlen(script.data), "--next-command") < 2 ||
          !strstr(script.data, expected_run_next) ||
          !strstr(script.data,
                  "--json \"$repeat_status\" --markdown \"$repeat_status_md\"") ||
          !strstr(script.data,
                  "if [ -r \"$repeat_status\" ]; then nytrix_repeat_status=\"$repeat_status\"; fi"))
        (void)string_list_push_copy(errors,
                                    "next-run repeat aliases resolve before fresh status");
      if (!strstr(script.data,
                  "if [ -r \"$nytrix_repeat_status\" ]; then status=\"$nytrix_repeat_status\"; fi") ||
          !strstr(script.data,
                  "if [ -r \"$repeat_progress\" ]; then progress=\"$repeat_progress\"; fi"))
        (void)string_list_push_copy(
            errors,
            "next-run dry-run state does not point at repeat reports");
      const char *dry_state_write =
          strstr(script.data, "nytrix_write_terminal_state dry-run preview");
      const char *dry_post_refresh =
          strstr(script.data,
                 "refreshing repeat reports after state write");
      const char *dry_canonical_refresh =
          strstr(script.data,
                 "refreshing canonical cockpit after state write");
      const char *dry_summary =
          strstr(script.data, "nytrix repeat dry-run: mode=");
      if (!dry_state_write || !dry_post_refresh || !dry_canonical_refresh ||
          !dry_summary ||
          dry_post_refresh < dry_state_write ||
          dry_canonical_refresh < dry_post_refresh ||
          dry_summary < dry_canonical_refresh ||
          count_sub(script.data, strlen(script.data),
                    "--json \"$repeat_status\" --markdown \"$repeat_status_md\"") < 3 ||
          !strstr(script.data, "nytrix_canonical_status=\"$status\"") ||
          !strstr(script.data,
                  "--json \"$nytrix_canonical_status\" --markdown \"$nytrix_canonical_status_md\"") ||
          !strstr(script.data,
                  "--status \"$nytrix_canonical_status\"") ||
          !strstr(script.data,
                  "--json \"$nytrix_canonical_progress\" --markdown \"$nytrix_canonical_progress_md\""))
        (void)string_list_push_copy(
            errors,
            "next-run dry-run does not refresh repeat/canonical reports after writing state");
      if (!strstr(script.data, "NYTRIX_RUN_REPEAT=\"${NYTRIX_RUN_REPEAT:-1}\"") ||
          !strstr(script.data, "nytrix_repeat_status=") ||
          !strstr(script.data, "nytrix_repeat_mode=\"$NYTRIX_RUN_REPEAT\"") ||
          !strstr(script.data, "1) nytrix_repeat_mode=once") ||
          !strstr(script.data, "*) nytrix_repeat_mode=count") ||
          !strstr(script.data, "auto|good)") ||
          !strstr(script.data, "\"runs_to_good_language_score\"") ||
          !strstr(script.data, "auto repeat count from") ||
          !strstr(script.data, "auto repeat is not projectable") ||
          !strstr(script.data, "target|campaign)") ||
          !strstr(script.data, "\"runs_needed\"") ||
          !strstr(script.data, "target repeat count from") ||
          !strstr(script.data, "campaign target already complete") ||
          !strstr(script.data, "\"language_score_gap_percent\"") ||
          !strstr(script.data, "good language score reached after") ||
          !strstr(script.data, "NYTRIX_RUN_DRY_RUN=\"${NYTRIX_RUN_DRY_RUN:-0}\"") ||
          !strstr(script.data, "nytrix repeat dry-run: mode=") ||
          !strstr(script.data, "cycles=$NYTRIX_RUN_REPEAT") ||
          !strstr(script.data, "\"target_percent_per_run\"") ||
          !strstr(script.data, "\"thread_years_per_run\"") ||
          !strstr(script.data, "wall_hours=$nytrix_wall_hours") ||
          !strstr(script.data, "campaign_gain_percent=$nytrix_campaign_gain") ||
          !strstr(script.data, "\"dry_run_exceeds_max\"") ||
          !strstr(script.data, "\"dry_run_wall_hours\":%s") ||
          !strstr(script.data, "\"dry_run_wall_days\":%s") ||
          !strstr(script.data, "\"dry_run_thread_years\":%s") ||
          !strstr(script.data, "\"dry_run_campaign_gain_percent\":%s") ||
          !strstr(script.data, "\"dry_run_target_percent_per_run\":%s") ||
          !strstr(script.data, "\"dry_run_thread_years_per_run\":%s") ||
          !strstr(script.data, "nytrix_json_number \"${nytrix_wall_hours:-0}\"") ||
          !strstr(script.data, "nytrix_json_number \"${nytrix_campaign_gain:-0}\"") ||
          !strstr(script.data, "\"canonical_status_report\":\"%s\"") ||
          !strstr(script.data, "\"canonical_progress_report\":\"%s\"") ||
          !strstr(script.data, "nytrix_dry_low_priority=$(nytrix_json_enabled") ||
          !strstr(script.data, "nytrix_dry_nice=$(nytrix_json_number") ||
          !strstr(script.data, "nytrix_dry_load_wait=$(nytrix_json_enabled") ||
          !strstr(script.data, "nytrix_dry_max_load_pct=$(nytrix_json_number") ||
          !strstr(script.data, "nytrix_dry_space_guard=false") ||
          !strstr(script.data, "nytrix_dry_min_free_gb=$(nytrix_json_number") ||
          !strstr(script.data, "nytrix_dry_run_lock=$(nytrix_json_enabled") ||
          !strstr(script.data, "nytrix repeat dry-run: guards low_priority=$nytrix_dry_low_priority") ||
          !strstr(script.data, "run_lock=$nytrix_dry_run_lock threads=$nytrix_run_threads") ||
          !strstr(script.data, "NYTRIX_RUN_REPEAT must be a positive integer") ||
          !strstr(script.data, "NYTRIX_RUN_MAX_CYCLES=\"${NYTRIX_RUN_MAX_CYCLES:-0}\"") ||
          !strstr(script.data, "NYTRIX_RUN_MAX_CYCLES must be a non-negative integer") ||
          !strstr(script.data, "NYTRIX_RUN_COOLDOWN_S=\"${NYTRIX_RUN_COOLDOWN_S:-0}\"") ||
          !strstr(script.data, "NYTRIX_RUN_COOLDOWN_S must be a non-negative integer") ||
          !strstr(script.data, "NYTRIX_RUN_HEARTBEAT_S=\"${NYTRIX_RUN_HEARTBEAT_S:-300}\"") ||
          !strstr(script.data, "NYTRIX_RUN_HEARTBEAT_S must be a non-negative integer") ||
          !strstr(script.data, "nytrix_default_stop_file=") ||
          !strstr(script.data, "/stop\nNYTRIX_RUN_STOP_FILE=\"${NYTRIX_RUN_STOP_FILE:-$nytrix_default_stop_file}\"") ||
          !strstr(script.data, "nytrix_default_state_file=") ||
          !strstr(script.data, "/run-next-state.json\nNYTRIX_RUN_STATE_FILE=\"${NYTRIX_RUN_STATE_FILE:-$nytrix_default_state_file}\"") ||
            !strstr(script.data, "nytrix_json_uint()") ||
          !strstr(script.data, "nytrix_json_number()") ||
          !strstr(script.data, "nytrix_json_enabled()") ||
            !strstr(script.data, "nytrix_write_state()") ||
            !strstr(script.data, "\"phase\":\"%s\"") ||
            !strstr(script.data, "\"state\":\"%s\"") ||
            !strstr(script.data, "\"timestamp_utc\":\"%s\"") ||
            !strstr(script.data, "\"updated_at\":\"%s\"") ||
            !strstr(script.data, "\"live\":%s") ||
            !strstr(script.data, "\"child_status\":\"%s\"") ||
            !strstr(script.data, "\"stale_after_seconds\":%s") ||
            !strstr(script.data, "heartbeat_s=\"${NYTRIX_RUN_HEARTBEAT_S:-0}\"") ||
            !strstr(script.data, "stale_after=$((heartbeat_s * 2 + 30))") ||
            !strstr(script.data, "\"started_at\":\"%s\"") ||
            !strstr(script.data, "\"finished_at\":\"%s\"") ||
            !strstr(script.data, "\"repeat_mode\":\"%s\"") ||
            !strstr(script.data, "\"repeat_count\":%s") ||
            !strstr(script.data, "\"cycle\":%s") ||
            !strstr(script.data, "\"heartbeat_s\":%s") ||
            !strstr(script.data, "\"heartbeat_count\":%s") ||
            !strstr(script.data, "\"child_pid\":%s") ||
            !strstr(script.data, "printf '\"heartbeat_s\":%s,' \"$heartbeat_s\"") ||
          !strstr(script.data, "\"low_priority\":%s") ||
          !strstr(script.data, "\"handoff_low_priority\":%s") ||
          !strstr(script.data, "\"nice\":%s") ||
          !strstr(script.data, "\"handoff_nice\":%s") ||
          !strstr(script.data, "\"load_wait\":%s") ||
          !strstr(script.data, "\"handoff_load_wait\":%s") ||
          !strstr(script.data, "\"max_load_pct\":%s") ||
          !strstr(script.data, "\"handoff_max_load_pct\":%s") ||
          !strstr(script.data, "\"space_guard\":false") ||
          !strstr(script.data, "\"handoff_space_guard\":false") ||
          !strstr(script.data, "\"min_free_gb\":%s") ||
          !strstr(script.data, "\"handoff_min_free_gb\":%s") ||
          !strstr(script.data, "\"run_lock\":%s") ||
          !strstr(script.data, "\"handoff_run_lock\":%s") ||
          !strstr(script.data, "\"threads\":\"%s\"") ||
          !strstr(script.data, "\"handoff_threads\":\"%s\"") ||
          !strstr(script.data, "nytrix_run_threads=") ||
            !strstr(script.data, "\"status_report\":\"%s\"") ||
            !strstr(script.data, "\"status_json\":\"%s\"") ||
            !strstr(script.data, "\"progress_report\":\"%s\"") ||
            !strstr(script.data, "\"progress_json\":\"%s\"") ||
            !strstr(script.data, "\"last_report\":\"%s\"") ||
          !strstr(script.data, "nytrix_run_with_heartbeat()") ||
          !strstr(script.data, "nytrix_write_state running child-start") ||
          !strstr(script.data, "nytrix_write_state running heartbeat") ||
          !strstr(script.data, "nytrix_write_state refreshing \"child-exit-$rc\"") ||
          !strstr(script.data, "nytrix_run_with_heartbeat nytrix_low_priority ./build/nytrix fuzz all run") ||
          !strstr(script.data, "nytrix_write_terminal_state dry-run preview") ||
          !strstr(script.data, "nytrix_write_state running cycle-start") ||
          !strstr(script.data, "nytrix_write_state cycle-complete report-ready") ||
          !strstr(script.data, "nytrix_write_terminal_state stopped after-cycle") ||
          !strstr(script.data, "nytrix_state_exit_trap()") ||
          !strstr(script.data, "nytrix_release_campaign_lock") ||
          !strstr(script.data, "nytrix_install_state_traps") ||
          count_sub(script.data, strlen(script.data), "nytrix_install_state_traps") < 3 ||
          !strstr(script.data, "nytrix_write_terminal_state failed \"exit-$rc\"") ||
          !strstr(script.data, "nytrix_repeat_exceeds_max=0") ||
          !strstr(script.data, "nytrix_repeat_exceeds_max=1") ||
          !strstr(script.data, "max_cycles=$NYTRIX_RUN_MAX_CYCLES") ||
          !strstr(script.data, "cooldown_s=$NYTRIX_RUN_COOLDOWN_S") ||
          !strstr(script.data, "heartbeat_s=$NYTRIX_RUN_HEARTBEAT_S") ||
          !strstr(script.data, "stop_file=$nytrix_stop_file_label") ||
          !strstr(script.data, "exceeds_max=$nytrix_exceeds_max") ||
          !strstr(script.data, "resolved cycles $NYTRIX_RUN_REPEAT exceeds NYTRIX_RUN_MAX_CYCLES=$NYTRIX_RUN_MAX_CYCLES") ||
          !strstr(script.data, "stop file present before campaign: $NYTRIX_RUN_STOP_FILE") ||
          !strstr(script.data, "nytrix campaign stop requested before cycle ${nytrix_run_i}: $NYTRIX_RUN_STOP_FILE") ||
          !strstr(script.data, "nytrix campaign stop requested after cycle ${nytrix_run_i}: $NYTRIX_RUN_STOP_FILE") ||
          !strstr(script.data, "nytrix campaign cooldown: sleep ${NYTRIX_RUN_COOLDOWN_S}s before next cycle") ||
          !strstr(script.data, "sleep \"$NYTRIX_RUN_COOLDOWN_S\"") ||
          !strstr(script.data, "while [ \"$nytrix_run_i\" -le \"$NYTRIX_RUN_REPEAT\" ]; do") ||
          !strstr(script.data, "nytrix campaign cycle ${nytrix_run_i}/${NYTRIX_RUN_REPEAT}") ||
          !strstr(script.data, "nytrix_run_i=$((nytrix_run_i + 1))") ||
          !strstr(script.data, "\ndone\n") ||
          !strstr(script.data,
                  "\ndone\nnytrix_write_terminal_state finished run-complete\n") ||
          count_sub(script.data, strlen(script.data),
                    "--json \"$status\" --markdown \"$status_md\"") < 2 ||
          count_sub(script.data, strlen(script.data),
                    "--status \"$status\"") < 2)
        (void)string_list_push_copy(errors, "next-run script does not support guarded repeat handoff runs");
      if (!strstr(script.data, "\"campaign_complete\":true") ||
          !strstr(script.data, "--no-script") ||
          !strstr(script.data, "--allow-full-pressure-remediation") ||
          !strstr(script.data, "campaign target already complete"))
        (void)string_list_push_copy(errors, "next-run script does not stop before completed target");
      if (count_sub(script.data, strlen(script.data),
                    "--allow-full-pressure-remediation") < 2)
        (void)string_list_push_copy(errors, "next-run script does not preserve remediation allowance for both status refreshes");
      const char *status_cmd = strstr(script.data, "fuzz all status");
      if (!status_cmd) {
        (void)string_list_push_copy(errors, "next-run script does not keep status handoff in its report dir");
      } else {
        const char *line_end = strchr(status_cmd, '\n');
        size_t line_len = line_end ? (size_t)(line_end - status_cmd) : strlen(status_cmd);
        char *status_line = strndup_local(status_cmd, line_len);
        if (!status_line ||
            !strstr(status_line, "--dir") ||
            !strstr(status_line, "--target-thread-years") ||
            !strstr(status_line, "--hours") ||
            !strstr(status_line, "--threads") ||
            !strstr(status_line, "--profile") ||
            !strstr(status_line, "--strict") ||
            !strstr(status_line, "--allow-full-pressure-remediation"))
          (void)string_list_push_copy(errors, "next-run status handoff does not preserve campaign geometry");
        free(status_line);
      }
      const char *run_cmd = strstr(script.data, "fuzz all run");
      if (!run_cmd) {
        (void)string_list_push_copy(errors, "next-run script does not include full-pressure run");
      } else {
        const char *line_start = run_cmd;
        while (line_start > script.data && line_start[-1] != '\n') --line_start;
        const char *line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char *run_line = strndup_local(line_start, line_len);
        if (!run_line ||
            !strstr(run_line, "--dir") ||
            !strstr(run_line, "--target-thread-years") ||
            !strstr(run_line, "--hours") ||
            !strstr(run_line, "--threads") ||
            !strstr(run_line, "--profile") ||
            !strstr(run_line, "nytrix_low_priority") ||
            !strstr(script.data, "nytrix_require_free_space\nnytrix_wait_for_load") ||
            !strstr(script.data, "nytrix_wait_for_load\nnytrix_run_with_heartbeat nytrix_low_priority") ||
            !strstr(run_line, "--json \"$report\""))
          (void)string_list_push_copy(errors, "next-run full-pressure run does not preserve campaign geometry");
        free(run_line);
      }
    }
    free(script.data);
  }
  free(mode);
  free(latest_full_pressure);
  free(thread_years_per_run_source);
  free(scratch_root);
    free(ny_bin);
  free(run_command);
  free(next_command);
  free(next_handoff_command);
  free(preview_command);
  free(state_refresh_command);
  free(stop_file);
  free(stop_command);
  free(resume_command);
  free(state_file);
  free(state_command);
  free(state_label);
  free(state_phase);
  free(state_canonical_status);
  free(state_canonical_progress);
  free(freshness_action);
  free(latest_freshness_action);
  free(full_pressure_freshness_action);
  free(full_pressure_freshen_command);
  free(full_pressure_remediation_command);
  free(full_pressure_action_command);
  free(progress_command);
  free(status_command);
  free(old_path_probe_command);
  free(old_path_command);
  free(old_path_dry_run_command);
  free(old_path_apply_command);
  free(old_path_next_action);
  free(old_path_next_reason);
  free(old_path_summary_report);
  free(old_path_summary_markdown);
  free(historical_worklist);
  free(historical_worklist_md);
  free(next_script);
}

static void selftest_validate_fuzz_all_repeat_status_progress(
    const char *json, string_list_t *errors, int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-status") != 0)
    (void)string_list_push_copy(errors, "repeat status mode mismatch");
  char *progress_command = summary_string_from_report(json, "progress_command");
  if (!progress_command ||
      !strstr(progress_command, "fuzz all progress --refresh --strict") ||
      !strstr(progress_command, "--allow-full-pressure-remediation") ||
      !strstr(progress_command, "--status") ||
      !strstr(progress_command, "fuzz_repeat_status/repeat-status.json") ||
      !strstr(progress_command, "--json") ||
      !strstr(progress_command, "fuzz_repeat_status/repeat-progress.json") ||
      !strstr(progress_command, "--markdown") ||
      !strstr(progress_command, "fuzz_repeat_status/repeat-progress.md") ||
      strstr(progress_command, "fuzz_repeat_status/progress.json") ||
      strstr(progress_command, "fuzz_repeat_status/progress.md"))
    (void)string_list_push_copy(
        errors, "repeat status progress command lost repeat-progress paths");
  char *next_script = summary_string_from_report(json, "next_script");
  if (!next_script ||
      !strstr(next_script, "fuzz_repeat_status/run-next.sh") ||
      path_is_absolute(next_script))
    (void)string_list_push_copy(
        errors, "repeat status next script lost cockpit handoff path");
  char *state_file = summary_string_from_report(json, "state_file");
  if (!state_file ||
      !strstr(state_file, "fuzz_repeat_status/run-next-state.json") ||
      path_is_absolute(state_file))
    (void)string_list_push_copy(
        errors, "repeat status state file lost cockpit handoff path");
  char root[4096] = {0};
  char script_abs[4096] = {0};
  char state_abs[4096] = {0};
  if (!find_nytrix_root(root, sizeof(root))) {
    (void)string_list_push_copy(
        errors, "repeat status dry-run could not locate Nytrix root");
  } else if (!next_script || !*next_script ||
             !path_join(script_abs, sizeof(script_abs), root, next_script) ||
             !state_file || !*state_file ||
             !path_join(state_abs, sizeof(state_abs), root, state_file)) {
    (void)string_list_push_copy(
        errors, "repeat status dry-run could not resolve handoff paths");
  } else {
    char *dry_argv[] = {
      "env",
      "NYTRIX_LOW_PRIORITY=1",
      "NYTRIX_RUN_NICE=10",
      "NYTRIX_RUN_DRY_RUN=1",
      "NYTRIX_RUN_REPEAT=target",
      script_abs,
      NULL
    };
    proc_result_t dry_pr = run_proc(dry_argv, root, 20.0);
    if (dry_pr.rc != 0) {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "repeat status dry-run state probe rc=%d", dry_pr.rc);
      (void)string_list_push_copy(errors, msg);
    } else {
      file_buf_t state = {0};
      if (!read_file(state_abs, &state) || !state.data) {
        (void)string_list_push_copy(
            errors, "repeat status dry-run state was not written");
      } else {
        char *state_label = json_string_or_empty(state.data, "state");
        char *event = json_string_or_empty(state.data, "event");
        char *repeat_mode = json_string_or_empty(state.data, "repeat_mode");
        char *started_at = json_string_or_empty(state.data, "started_at");
        char *finished_at = json_string_or_empty(state.data, "finished_at");
        char *status_json = json_string_or_empty(state.data, "status_json");
        char *progress_json = json_string_or_empty(state.data, "progress_json");
        char *canonical_status =
            json_string_or_empty(state.data, "canonical_status_report");
        char *canonical_progress =
            json_string_or_empty(state.data, "canonical_progress_report");
        double repeat_count = -1.0;
        double dry_wall_hours = -1.0;
        double dry_wall_days = -1.0;
        double dry_thread_years = -1.0;
        double dry_campaign_gain = -1.0;
        double dry_percent_per_run = -1.0;
        double dry_years_per_run = -1.0;
        if (!state_label || strcmp(state_label, "dry-run") != 0 ||
            !event || strcmp(event, "preview") != 0)
          (void)string_list_push_copy(
              errors, "repeat status dry-run state did not reach preview");
        if (!repeat_mode || strcmp(repeat_mode, "target") != 0 ||
            !extract_json_number(state.data, "repeat_count", &repeat_count) ||
            repeat_count < 1.0)
          (void)string_list_push_copy(
              errors, "repeat status dry-run repeat aliases are wrong");
        if (!started_at || !*started_at || !finished_at || !*finished_at)
          (void)string_list_push_copy(
              errors, "repeat status dry-run lifecycle timestamps missing");
        if (!status_json ||
            !strstr(status_json, "fuzz_repeat_status/repeat-status.json") ||
            !progress_json ||
            !strstr(progress_json,
                    "fuzz_repeat_status/repeat-progress.json"))
          (void)string_list_push_copy(
              errors, "repeat status dry-run report aliases are wrong");
        if (!extract_json_number(state.data, "dry_run_wall_hours",
                                 &dry_wall_hours) ||
            !extract_json_number(state.data, "dry_run_wall_days",
                                 &dry_wall_days) ||
            !extract_json_number(state.data, "dry_run_thread_years",
                                 &dry_thread_years) ||
            !extract_json_number(state.data,
                                 "dry_run_campaign_gain_percent",
                                 &dry_campaign_gain) ||
            !extract_json_number(state.data,
                                 "dry_run_target_percent_per_run",
                                 &dry_percent_per_run) ||
            !extract_json_number(state.data,
                                 "dry_run_thread_years_per_run",
                                 &dry_years_per_run) ||
            dry_wall_hours <= 0.0 || dry_wall_days <= 0.0 ||
            dry_thread_years <= 0.0 || dry_campaign_gain <= 0.0 ||
            dry_percent_per_run <= 0.0 || dry_years_per_run <= 0.0 ||
            !strstr(state.data, "\"dry_run_exceeds_max\":false"))
          (void)string_list_push_copy(
              errors, "repeat status dry-run preview metrics missing");
        if (!canonical_status || !strstr(canonical_status, "status.json") ||
            strstr(canonical_status, "repeat-status.json") ||
            !canonical_progress ||
            !strstr(canonical_progress, "progress.json") ||
            strstr(canonical_progress, "repeat-progress.json"))
          (void)string_list_push_copy(
              errors, "repeat status dry-run canonical aliases are wrong");
        free(state_label);
        free(event);
        free(repeat_mode);
        free(started_at);
        free(finished_at);
        free(status_json);
        free(progress_json);
        free(canonical_status);
        free(canonical_progress);
      }
      free(state.data);
    }
    proc_result_free(&dry_pr);
    char *default_argv[] = {
      "env",
      "NYTRIX_LOW_PRIORITY=1",
      "NYTRIX_RUN_NICE=10",
      "NYTRIX_RUN_DRY_RUN=1",
      script_abs,
      NULL
    };
    proc_result_t default_pr = run_proc(default_argv, root, 20.0);
    if (default_pr.rc != 0) {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "repeat status default dry-run rc=%d", default_pr.rc);
      (void)string_list_push_copy(errors, msg);
    } else {
      if (!default_pr.out || strstr(default_pr.out, "mode=1 ") ||
          !strstr(default_pr.out, "mode=once "))
        (void)string_list_push_copy(
            errors, "repeat status default dry-run leaks numeric mode");
      file_buf_t state = {0};
      if (!read_file(state_abs, &state) || !state.data) {
        (void)string_list_push_copy(
            errors, "repeat status default dry-run state was not written");
      } else {
        char *repeat_mode = json_string_or_empty(state.data, "repeat_mode");
        double repeat_count = -1.0;
        if (!repeat_mode || strcmp(repeat_mode, "once") != 0 ||
            !extract_json_number(state.data, "repeat_count", &repeat_count) ||
            repeat_count != 1.0)
          (void)string_list_push_copy(
              errors, "repeat status default dry-run repeat aliases are wrong");
        free(repeat_mode);
      }
      free(state.data);
    }
    proc_result_free(&default_pr);
  }
  char *coverage_next_command =
      summary_string_from_report(json, "coverage_next_command");
  char *coverage_next_guarded =
      summary_string_from_report(json, "coverage_next_guarded_command");
  char *coverage_next_state =
      summary_string_from_report(json, "coverage_next_state_command");
  char *coverage_next_stop =
      summary_string_from_report(json, "coverage_next_stop_command");
  char *coverage_next_resume =
      summary_string_from_report(json, "coverage_next_resume_command");
  char *recommended_command =
      summary_string_from_report(json, "recommended_command");
  if (coverage_next_command && strstr(coverage_next_command, "fuzz all run") &&
      (!coverage_next_guarded ||
       !strstr(coverage_next_guarded, "run-missing-evidence.sh") ||
       !coverage_next_state ||
       !strstr(coverage_next_state, "run-missing-evidence-state.json") ||
       !coverage_next_stop ||
       !strstr(coverage_next_stop, "missing-evidence-stop") ||
       !coverage_next_resume ||
       !strstr(coverage_next_resume, "missing-evidence-stop") ||
       !recommended_command ||
       !strstr(recommended_command, "run-missing-evidence.sh")))
    (void)string_list_push_copy(
        errors, "repeat status lost guarded missing-evidence handoff");
  char *markdown = summary_string_from_report(json, "markdown");
  char markdown_abs[4096] = {0};
  if (markdown && *markdown) {
    if (path_is_absolute(markdown)) {
      snprintf(markdown_abs, sizeof(markdown_abs), "%s", markdown);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(markdown_abs, sizeof(markdown_abs), root, markdown);
    }
  }
  file_buf_t md = {0};
  if (!markdown_abs[0] || !read_file(markdown_abs, &md) || !md.data) {
    (void)string_list_push_copy(errors,
                                "repeat status markdown was not readable");
  } else if (!strstr(md.data, "fuzz_repeat_status/repeat-progress.json") ||
             !strstr(md.data, "fuzz_repeat_status/repeat-progress.md") ||
             strstr(md.data, "fuzz_repeat_status/progress.json") ||
             strstr(md.data, "fuzz_repeat_status/progress.md")) {
    (void)string_list_push_copy(
        errors, "repeat status markdown lost repeat-progress paths");
  } else if (coverage_next_command &&
             strstr(coverage_next_command, "fuzz all run") &&
             (!strstr(md.data, "run-missing-evidence.sh") ||
              !strstr(md.data, "run-missing-evidence-state.json") ||
              !strstr(md.data, "missing-evidence-stop"))) {
    (void)string_list_push_copy(
        errors, "repeat status markdown lost guarded missing-evidence handoff");
  }
  free(md.data);
  free(mode);
  free(progress_command);
  free(next_script);
  free(state_file);
  free(coverage_next_command);
  free(coverage_next_guarded);
  free(coverage_next_state);
  free(coverage_next_stop);
  free(coverage_next_resume);
  free(recommended_command);
  free(markdown);
}

static void selftest_validate_fuzz_fresh_handoff(const char *json,
                                                 string_list_t *errors,
                                                 int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-status") != 0)
    (void)string_list_push_copy(errors, "fresh handoff status mode mismatch");
  bool allow_incomplete = false, ready = true, long_run_ready = true;
  if (!summary_bool_from_report(json, "allow_incomplete_coverage",
                                &allow_incomplete) ||
      !allow_incomplete)
    (void)string_list_push_copy(errors, "fresh handoff did not preserve incomplete-coverage allowance");
  if (!summary_bool_from_report(json, "ready", &ready) || ready)
    (void)string_list_push_copy(errors, "fresh handoff status should not be campaign-ready");
  if (!summary_bool_from_report(json, "long_run_ready", &long_run_ready) ||
      long_run_ready)
    (void)string_list_push_copy(errors, "fresh handoff long-run gate should remain false");
  double blocker_count = -1.0, blocker_gaps = -1.0, full_pressure_reports = -1.0;
  if (!summary_number_from_report(json, "blocker_count", &blocker_count) ||
      blocker_count != 1.0)
    (void)string_list_push_copy(errors, "fresh handoff blocker count mismatch");
  if (!summary_number_from_report(json, "coverage_blocker_gaps", &blocker_gaps) ||
      blocker_gaps <= 0.0)
    (void)string_list_push_copy(errors, "fresh handoff coverage blocker was missing");
  char *coverage_state = summary_string_from_report(json, "coverage_state");
  if (!coverage_state || strcmp(coverage_state, "blocked") != 0)
    (void)string_list_push_copy(errors,
                                "fresh handoff coverage state was not blocked");
  if (!summary_number_from_report(json, "full_pressure_reports",
                                  &full_pressure_reports) ||
      full_pressure_reports != 0.0)
    (void)string_list_push_copy(errors, "fresh handoff full-pressure count should be zero");
  if (!strstr(json, "\"category\":\"coverage\"") ||
      !strstr(json, "fuzz_fresh_handoff/coverage.json"))
    (void)string_list_push_copy(errors, "fresh handoff coverage command did not use custom cockpit");
  if (strstr(json, "\"command\":\"./build/nytrix fuzz all coverage --strict --history build/fuzz/all/history.json"))
    (void)string_list_push_copy(errors, "fresh handoff coverage command leaked canonical cockpit");
  char *run_command = summary_string_from_report(json, "run_command");
  if (!run_command ||
      !selftest_command_uses_env_nice(run_command) ||
      !strstr(run_command, "--dir") ||
      !strstr(run_command, "fuzz_fresh_handoff") ||
      !strstr(run_command, "--target-thread-years 0.25"))
    (void)string_list_push_copy(errors, "fresh handoff run command lost guarded campaign geometry");
  char *next_script = summary_string_from_report(json, "next_script");
  if (!next_script || !strstr(next_script, "fuzz_fresh_handoff/run-next.sh"))
    (void)string_list_push_copy(errors, "fresh handoff next script left custom cockpit");
  free(mode);
  free(run_command);
  free(next_script);
  free(coverage_state);
}

static void selftest_validate_fuzz_default_pressure(const char *json,
                                                    string_list_t *errors,
                                                    int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-plan") != 0)
    (void)string_list_push_copy(errors, "default pressure plan mode mismatch");
  bool top_ok = false;
  double failure_count = -1.0;
  if (!json_top_level_bool_from_report(json, "ok", &top_ok) ||
      !summary_number_from_report(json, "failure_count", &failure_count) ||
      top_ok != (failure_count == 0.0))
    (void)string_list_push_copy(errors,
                                "plan top ok alias missing or diverged");
  selftest_expect_top_alias_number(json, "cases", "cases", errors);
  selftest_expect_top_alias_number(json, "ok_count", "ok", errors);
  selftest_expect_top_alias_number(json, "failure_count", "failure_count",
                                   errors);
  selftest_expect_top_alias_number(json, "campaign_percent",
                                   "campaign_percent", errors);
  selftest_expect_top_alias_number(json, "campaign_remaining_percent",
                                   "campaign_remaining_percent", errors);
  selftest_expect_top_alias_number(json, "thread_years", "thread_years",
                                   errors);
  selftest_expect_top_alias_number(json, "target_thread_years",
                                   "target_thread_years", errors);
  selftest_expect_top_alias_number(json, "campaign_thread_years",
                                   "campaign_thread_years", errors);
  selftest_expect_top_alias_number(json, "campaign_target_thread_years",
                                   "campaign_target_thread_years", errors);
  selftest_expect_top_alias_number(json, "campaign_remaining_thread_years",
                                   "campaign_remaining_thread_years", errors);
  selftest_expect_top_alias_number(json, "campaign_done_percent",
                                   "campaign_done_percent", errors);
  selftest_expect_top_alias_number(json, "campaign_runs_needed",
                                   "campaign_runs_needed", errors);
  selftest_expect_top_alias_number(json, "campaign_wall_hours_needed",
                                   "campaign_wall_hours_needed", errors);
  selftest_expect_top_alias_number(json, "campaign_wall_days_needed",
                                   "campaign_wall_days_needed", errors);
  selftest_expect_top_alias_number(json, "campaign_thread_years_per_run",
                                   "campaign_thread_years_per_run", errors);
  selftest_expect_top_alias_number(json, "campaign_percent_per_run",
                                   "campaign_percent_per_run", errors);
  selftest_expect_top_alias_number(json, "campaign_plan_wall_hours",
                                   "campaign_plan_wall_hours", errors);
  selftest_expect_top_alias_string(json, "campaign_plan_threads", errors);
  selftest_expect_top_alias_number(json, "campaign_calendar_percent_10y",
                                   "campaign_calendar_percent_10y", errors);
  selftest_expect_top_alias_string(json, "campaign_eta_local", errors);
  selftest_expect_top_alias_number(json, "score_percent", "score_percent",
                                   errors);
  selftest_expect_top_alias_number(json, "score", "score_percent",
                                   errors);
  selftest_expect_top_alias_string_from(json, "score_label",
                                        "language_score_label", errors);
  selftest_expect_top_alias_number(json, "stability_percent",
                                   "stability_percent", errors);
  selftest_expect_top_alias_number(json, "stability_score_percent",
                                   "stability_score_percent", errors);
  selftest_expect_top_alias_number(json, "language_score",
                                   "language_score", errors);
  selftest_expect_top_alias_number(json, "language_score_percent",
                                   "language_score_percent", errors);
  selftest_expect_top_alias_number(
      json, "language_score_good_threshold_percent",
      "language_score_good_threshold_percent", errors);
  selftest_expect_top_alias_number(json, "language_score_signal_percent",
                                   "language_score_signal_percent", errors);
  selftest_expect_top_alias_number(
      json, "language_score_evidence_cap_percent",
      "language_score_evidence_cap_percent", errors);
  selftest_expect_top_alias_number(json, "signal_health_percent",
                                   "signal_health_percent", errors);
  selftest_expect_top_alias_number(json, "evidence_cap_percent",
                                   "evidence_cap_percent", errors);
  selftest_expect_top_alias_number(
      json, "next_run_language_score_percent",
      "next_run_language_score_percent", errors);
  selftest_expect_top_alias_number(json, "next_run_language_score",
                                   "next_run_language_score", errors);
  selftest_expect_top_alias_number(
      json, "next_run_language_score_delta_percent",
      "next_run_language_score_delta_percent", errors);
     selftest_expect_top_alias_number(json, "coverage_percent",
                                      "coverage_percent", errors);
     selftest_expect_top_alias_number(json, "coverage_backlog_lanes",
                                      "coverage_backlog_lanes", errors);
     selftest_expect_top_alias_number(json, "coverage_queue_count",
                                      "coverage_queue_count", errors);
     selftest_expect_top_alias_number(
         json, "coverage_queue_non_advisory_count",
         "coverage_queue_non_advisory_count", errors);
     selftest_expect_top_alias_number(json, "coverage_queue_advisory_count",
                                      "coverage_queue_advisory_count",
                                      errors);
     selftest_expect_top_alias_string(json, "coverage_queue_lanes", errors);
     selftest_expect_top_alias_number(json, "coverage_blocker_gaps",
                                      "coverage_blocker_gaps", errors);
  selftest_expect_top_alias_number(json, "active_worklist_items",
                                   "active_worklist_items", errors);
  selftest_expect_top_alias_number(json, "runs_needed",
                                   "runs_needed", errors);
  selftest_expect_top_alias_number(json, "wall_days_needed",
                                   "wall_days_needed", errors);
  selftest_expect_top_alias_number(json, "recommended_repeat_count",
                                   "recommended_repeat_count", errors);
  selftest_expect_top_alias_string(json, "recommended_action", errors);
  selftest_expect_top_alias_string(json, "recommended_reason", errors);
  selftest_expect_top_alias_string(json, "recommended_repeat_mode", errors);
  selftest_expect_top_alias_string(json, "recommended_command", errors);
  selftest_expect_top_alias_string(json, "recommended_low_cpu_command",
                                   errors);
  selftest_expect_top_alias_string(json, "recommended_preview_command",
                                   errors);
  selftest_expect_top_alias_string(json, "language_score_label", errors);
  selftest_expect_top_alias_string(json, "language_score_note", errors);
  selftest_expect_top_alias_string(json, "completion_state", errors);
  selftest_expect_top_alias_string(json, "completion_reason", errors);
  selftest_expect_top_alias_string(json, "plan_next_action", errors);
  selftest_expect_top_alias_string(json, "plan_next_lane", errors);
  selftest_expect_top_alias_string(json, "plan_next_reason", errors);
  selftest_expect_top_alias_string(json, "plan_next_command", errors);
  selftest_expect_top_alias_string(json, "plan_next_low_cpu_command",
                                   errors);
  selftest_expect_top_alias_string(json, "plan_next_preview_command",
                                   errors);
  selftest_expect_top_alias_string(json, "completion_eta_local", errors);
  int expected_threads = gc_parse_thread_count("25%", gc_online_thread_count());
  if (expected_threads < 1) expected_threads = 1;
  int default_threads = gc_default_fuzz_thread_count();
  if (default_threads != expected_threads)
    (void)string_list_push_copy(errors, "default fuzz thread count is not the 25% low-impact policy");
  double actual_threads = -1.0, thread_hours_per_run = -1.0;
  double runs_needed = -1.0, thread_hours_needed = -1.0;
  if (!summary_number_from_report(json, "threads", &actual_threads) ||
      (int)(actual_threads + 0.5) != expected_threads)
    (void)string_list_push_copy(errors, "plan default threads did not resolve to 25% of online CPUs");
  if (!summary_number_from_report(json, "thread_hours_per_run", &thread_hours_per_run) ||
      (int)(thread_hours_per_run + 0.5) != expected_threads)
    (void)string_list_push_copy(errors, "plan default thread-hours do not match one-hour low-impact default");
  if (!summary_number_from_report(json, "runs_needed", &runs_needed) ||
      runs_needed < 0.0)
    (void)string_list_push_copy(errors, "plan runs-needed field is missing");
  if (!summary_number_from_report(json, "thread_hours_needed",
                                  &thread_hours_needed) ||
      thread_hours_needed < 0.0)
    (void)string_list_push_copy(errors, "plan total thread-hours field is missing");
  if (runs_needed >= 0.0 && thread_hours_per_run >= 0.0 &&
      thread_hours_needed >= 0.0) {
    double expected_thread_hours = runs_needed * thread_hours_per_run;
    double delta = thread_hours_needed - expected_thread_hours;
    if (delta < -0.0001 || delta > 0.0001)
      (void)string_list_push_copy(errors, "plan total thread-hours do not match runs x per-run thread-hours");
  }
  double target_percent = -1.0, campaign_percent = -1.0;
  double campaign_confidence = -1.0, campaign_remaining = -1.0;
  double current_thread_years = -1.0, thread_years_alias = -1.0;
  if (!summary_number_from_report(json, "current_thread_years",
                                  &current_thread_years) ||
      !summary_number_from_report(json, "thread_years",
                                  &thread_years_alias) ||
      thread_years_alias != current_thread_years)
    (void)string_list_push_copy(errors,
                                "plan thread-years compact alias is wrong");
  if (!summary_number_from_report(json, "target_percent", &target_percent) ||
      target_percent < 0.0)
    (void)string_list_push_copy(errors, "plan target percent is missing");
  if (!summary_number_from_report(json, "campaign_percent",
                                  &campaign_percent) ||
      campaign_percent < 0.0)
    (void)string_list_push_copy(errors, "plan campaign percent is missing");
  if (!summary_number_from_report(json, "campaign_confidence_percent",
                                  &campaign_confidence) ||
      campaign_confidence < 0.0)
    (void)string_list_push_copy(errors, "plan campaign confidence is missing");
  if (!summary_number_from_report(json, "campaign_remaining_percent",
                                  &campaign_remaining) ||
      campaign_remaining < 0.0 || campaign_remaining > 100.0)
    (void)string_list_push_copy(errors, "plan campaign remaining is missing");
  double remaining_alias = -1.0;
  if (!summary_number_from_report(json, "remaining_percent",
                                  &remaining_alias) ||
      remaining_alias != campaign_remaining)
    (void)string_list_push_copy(errors,
                                "plan remaining percent alias is wrong");
  double campaign_delta = campaign_percent - target_percent;
  if (campaign_delta < -0.0001 || campaign_delta > 0.0001)
    (void)string_list_push_copy(errors,
                                "plan campaign percent diverges from target percent");
  double confidence_delta = campaign_confidence - target_percent;
  if (confidence_delta < -0.0001 || confidence_delta > 0.0001)
    (void)string_list_push_copy(errors, "plan campaign confidence diverges from target percent");
  double expected_remaining = 100.0 - target_percent;
  if (expected_remaining < 0.0) expected_remaining = 0.0;
  if (expected_remaining > 100.0) expected_remaining = 100.0;
  double remaining_delta = campaign_remaining - expected_remaining;
  if (remaining_delta < -0.0001 || remaining_delta > 0.0001)
    (void)string_list_push_copy(errors,
                                "plan campaign remaining does not match target percent");
  double language_score = -1.0, language_alias = -1.0;
  double score_alias = -1.0, compact_score_alias = -1.0;
  double stability_alias = -1.0;
  double stability_score_alias = -1.0;
  double language_good = -1.0;
  double language_gap = -1.0, next_language_score = -1.0;
  double next_language_alias = -1.0;
  double language_signal = -1.0, language_cap = -1.0;
  double signal_alias = -1.0, cap_alias = -1.0;
  double coverage_depth = -1.0, coverage_percent = -1.0;
  double runs_to_good = -2.0;
  if (!summary_number_from_report(json, "language_score_percent",
                                  &language_score) ||
      language_score < 0.0 || language_score > 100.0)
    (void)string_list_push_copy(errors,
                                "plan language score is missing");
  if (!summary_number_from_report(json, "language_score",
                                  &language_alias) ||
      language_alias != language_score)
    (void)string_list_push_copy(errors,
                                "plan language score alias is wrong");
  if (!summary_number_from_report(json, "score_percent",
                                  &score_alias) ||
      !summary_number_from_report(json, "score",
                                  &compact_score_alias) ||
      !summary_number_from_report(json, "stability_percent",
                                  &stability_alias) ||
      !summary_number_from_report(json, "stability_score_percent",
                                  &stability_score_alias) ||
      score_alias != language_score ||
      compact_score_alias != language_score ||
      stability_alias != language_score ||
      stability_score_alias != language_score)
    (void)string_list_push_copy(errors,
                                "plan score compact aliases are wrong");
  if (!summary_number_from_report(json,
                                  "language_score_good_threshold_percent",
                                  &language_good) ||
      language_good != 75.0)
    (void)string_list_push_copy(errors,
                                "plan language good threshold is missing");
  if (!summary_number_from_report(json, "language_score_gap_percent",
                                  &language_gap) ||
      language_gap < 0.0 || language_gap > 100.0)
    (void)string_list_push_copy(errors,
                                "plan language gap is missing");
  if (!summary_number_from_report(json,
                                  "next_run_language_score_percent",
                                  &next_language_score) ||
      next_language_score < 0.0 || next_language_score > 100.0)
    (void)string_list_push_copy(errors,
                                "plan next-run language score is missing");
  if (!summary_number_from_report(json, "next_run_language_score",
                                  &next_language_alias) ||
      next_language_alias != next_language_score)
    (void)string_list_push_copy(errors,
                                "plan next-run language alias is wrong");
  if (!summary_number_from_report(json, "coverage_depth_percent",
                                  &coverage_depth) ||
      !summary_number_from_report(json, "coverage_percent",
                                  &coverage_percent) ||
      coverage_depth < -1.0 || coverage_depth > 100.0 ||
      coverage_percent != coverage_depth)
    (void)string_list_push_copy(errors,
                                "plan coverage percent alias is missing");
  if (!summary_number_from_report(json,
                                  "language_score_signal_percent",
                                  &language_signal) ||
      !summary_number_from_report(json,
                                  "language_score_evidence_cap_percent",
                                  &language_cap) ||
      !summary_number_from_report(json, "signal_health_percent",
                                  &signal_alias) ||
      !summary_number_from_report(json, "evidence_cap_percent",
                                  &cap_alias) ||
      language_signal != signal_alias || language_cap != cap_alias)
    (void)string_list_push_copy(errors,
                                "plan score signal aliases are wrong");
  if (!summary_number_from_report(json, "runs_to_good_language_score",
                                  &runs_to_good))
    (void)string_list_push_copy(errors,
                                "plan runs-to-good language score is missing");
  double plan_advisory_timeouts = -1.0;
  bool plan_cache_ok = false, plan_ny_ok = false, plan_old_writer = true;
  if (!summary_number_from_report(json,
                                  "language_score_advisory_timeouts",
                                  &plan_advisory_timeouts) ||
      plan_advisory_timeouts < 0.0)
    (void)string_list_push_copy(errors,
                                "plan language advisory timeout count is missing");
  if (!summary_bool_from_report(json,
                                "language_score_cache_policy_ok",
                                &plan_cache_ok) ||
      !plan_cache_ok)
    (void)string_list_push_copy(errors,
                                "plan language cache guard is missing");
  if (!summary_bool_from_report(json,
                                "language_score_ny_bin_exists",
                                &plan_ny_ok) ||
      !plan_ny_ok)
    (void)string_list_push_copy(errors,
                                "plan language Ny binary guard is missing");
  if (!summary_bool_from_report(json,
                                "language_score_active_old_writer_present",
                                &plan_old_writer))
    (void)string_list_push_copy(errors,
                                "plan language old-writer guard is missing");
  (void)plan_old_writer;
  char *campaign_state = summary_string_from_report(json, "campaign_state");
  char *campaign_reason =
      summary_string_from_report(json, "campaign_incomplete_reason");
  char *completion_state = summary_string_from_report(json, "completion_state");
  char *completion_reason =
      summary_string_from_report(json, "completion_reason");
  if (!campaign_state || !*campaign_state ||
      !completion_state || strcmp(campaign_state, completion_state) != 0)
    (void)string_list_push_copy(errors,
                                "plan completion state alias is wrong");
  if (!campaign_reason || !*campaign_reason ||
      !completion_reason || strcmp(campaign_reason, completion_reason) != 0)
    (void)string_list_push_copy(errors,
                                "plan completion reason alias is wrong");
  char *language_label = summary_string_from_report(json,
                                                    "language_score_label");
  if (!language_label || !*language_label)
    (void)string_list_push_copy(errors,
                                "plan language score label is missing");
  char *score_label = summary_string_from_report(json, "score_label");
  if (!score_label || !language_label ||
      strcmp(score_label, language_label) != 0)
    (void)string_list_push_copy(errors,
                                "plan compact score label is missing");
  char *run_command = summary_string_from_report(json, "run_command");
  if (!run_command ||
      !selftest_command_uses_env_nice(run_command) ||
      !strstr(run_command, "--threads 25%") ||
      !strstr(run_command, "fuzz all run") ||
      strstr(run_command, "fuzz --runs"))
    (void)string_list_push_copy(errors, "default run command does not preserve guarded --threads 25%");
  if (run_command && (strstr(run_command, "--threads all") ||
                      strstr(run_command, "--threads cpu") ||
                      strstr(run_command, "--threads 75%")))
    (void)string_list_push_copy(errors, "default run command advertises a high-pressure thread policy");
  char *handoff_command = summary_string_from_report(json, "handoff_command");
  if (!handoff_command ||
      !strstr(handoff_command, "fuzz_default_pressure/run-next.sh") ||
      strstr(handoff_command, "fuzz all run"))
    (void)string_list_push_copy(errors, "default plan does not expose guarded handoff command");
  char *plan_next_action =
      summary_string_from_report(json, "plan_next_action");
  char *plan_next_command =
      summary_string_from_report(json, "plan_next_command");
  char *plan_next_low_cpu =
      summary_string_from_report(json, "plan_next_low_cpu_command");
  char *plan_next_preview =
      summary_string_from_report(json, "plan_next_preview_command");
  char *recommended_action =
      summary_string_from_report(json, "recommended_action");
  char *recommended_command =
      summary_string_from_report(json, "recommended_command");
  char *recommended_low_cpu =
      summary_string_from_report(json, "recommended_low_cpu_command");
  char *recommended_preview =
      summary_string_from_report(json, "recommended_preview_command");
  char *recommended_repeat_mode =
      summary_string_from_report(json, "recommended_repeat_mode");
  double recommended_repeat_count = -1.0;
  bool latest_fresh = false, full_pressure_fresh = false;
  bool have_latest_fresh =
      summary_bool_from_report(json, "latest_report_fresh", &latest_fresh);
  bool have_full_pressure_fresh =
      summary_bool_from_report(json, "latest_full_pressure_report_fresh",
                               &full_pressure_fresh);
  bool evidence_fresh =
      have_latest_fresh && have_full_pressure_fresh &&
      latest_fresh && full_pressure_fresh;
  const char *expected_plan_action = !evidence_fresh ? "freshen-evidence" :
      (runs_to_good > 0.0 ? "run-good" : "run-target");
  const char *expected_repeat = !evidence_fresh ? "" :
      (runs_to_good > 0.0 ? "good" : "target");
  if (!plan_next_action ||
      strcmp(plan_next_action, expected_plan_action) != 0)
    (void)string_list_push_copy(errors,
                                "default plan recommendation action wrong");
     if (!plan_next_command ||
         !strstr(plan_next_command, "fuzz_default_pressure/run-next.sh") ||
         strstr(plan_next_command, "fuzz all run") ||
         !selftest_command_uses_env_nice(plan_next_command) ||
         (expected_repeat[0] && !strstr(plan_next_command, expected_repeat)) ||
         (!expected_repeat[0] && strstr(plan_next_command, "NYTRIX_RUN_REPEAT=")))
       (void)string_list_push_copy(errors,
                                   "default plan recommendation command wrong");
     if (!plan_next_preview ||
         !selftest_command_uses_env_nice(plan_next_preview) ||
         !strstr(plan_next_preview, "NYTRIX_RUN_DRY_RUN=1") ||
         (expected_repeat[0] && !strstr(plan_next_preview, expected_repeat)) ||
         (!expected_repeat[0] && strstr(plan_next_preview, "NYTRIX_RUN_REPEAT=")) ||
         !strstr(plan_next_preview, "fuzz_default_pressure/run-next.sh"))
       (void)string_list_push_copy(errors,
                                   "default plan recommendation preview wrong");
     if (!plan_next_low_cpu ||
         strcmp(plan_next_low_cpu, plan_next_command) != 0 ||
         !selftest_command_uses_env_nice(plan_next_low_cpu))
       (void)string_list_push_copy(errors,
                                   "default plan low-cpu command lost selected handoff");
  if (!recommended_action || !plan_next_action ||
      strcmp(recommended_action, plan_next_action) != 0 ||
      !recommended_command || !plan_next_command ||
      strcmp(recommended_command, plan_next_command) != 0 ||
      !recommended_low_cpu || !plan_next_low_cpu ||
      strcmp(recommended_low_cpu, plan_next_low_cpu) != 0 ||
      !recommended_preview || !plan_next_preview ||
      strcmp(recommended_preview, plan_next_preview) != 0)
    (void)string_list_push_copy(errors,
                                "default plan recommended aliases diverge");
  if (!recommended_repeat_mode ||
      strcmp(recommended_repeat_mode, expected_repeat) != 0 ||
      !summary_number_from_report(json, "recommended_repeat_count",
                                  &recommended_repeat_count) ||
      (evidence_fresh ? recommended_repeat_count <= 0.0 :
                        recommended_repeat_count != 0.0))
    (void)string_list_push_copy(errors,
                                "default plan repeat aliases missing");
  char *markdown = summary_string_from_report(json, "markdown");
  char markdown_abs[4096] = {0};
  if (markdown && *markdown) {
    if (path_is_absolute(markdown)) {
      snprintf(markdown_abs, sizeof(markdown_abs), "%s", markdown);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(markdown_abs, sizeof(markdown_abs), root, markdown);
    }
  }
  file_buf_t md = {0};
  if (!markdown_abs[0] || !read_file(markdown_abs, &md) || !md.data) {
    (void)string_list_push_copy(errors, "default plan markdown was not readable");
  } else if (!strstr(md.data, "fuzz_default_pressure/run-next.sh") ||
             strstr(md.data, "fuzz all run")) {
    (void)string_list_push_copy(errors, "default plan markdown does not prefer guarded handoff");
  } else if (!strstr(md.data, "Recommended:") ||
             !strstr(md.data, expected_plan_action) ||
             !strstr(md.data, "Preview recommended:") ||
             !strstr(md.data, "NYTRIX_LOW_PRIORITY=1") ||
             (expected_repeat[0] &&
              !strstr(md.data, expected_repeat))) {
    (void)string_list_push_copy(errors,
                                "default plan markdown omits selected recommendation");
  } else if (!strstr(md.data, "- Confidence:") ||
             !strstr(md.data, "campaign evidence")) {
    (void)string_list_push_copy(errors, "default plan markdown omits campaign confidence");
  } else if (!strstr(md.data, "- Language:") ||
             !strstr(md.data, "good >=") ||
             !strstr(md.data, "next run")) {
    (void)string_list_push_copy(errors,
                                "default plan markdown omits language score");
  } else if (!strstr(md.data, "- Score guards:") ||
             !strstr(md.data, "advisory timeouts") ||
             !strstr(md.data, "old writer")) {
    (void)string_list_push_copy(errors,
                                "default plan markdown omits score guards");
     } else if (!strstr(md.data, "wall-hours; x") ||
                !strstr(md.data, "thread-hours")) {
       (void)string_list_push_copy(errors, "default plan markdown mixes budget units");
     }
     if (md.data) {
       const char *controls_section = strstr(md.data, "\n## Controls\n");
       const char *plan_jq_line =
           controls_section ?
               strstr(controls_section,
                      "jq '{ok,cases,ok_count,failure_count,campaign_percent,"
                      "campaign_remaining_percent,thread_years,"
                      "target_thread_years,score_percent,stability_percent,"
                      "stability_score_percent,language_score_percent,") :
               NULL;
       const char *plan_jq_end =
           plan_jq_line ? strchr(plan_jq_line, '\n') : NULL;
       if (!plan_jq_line || !plan_jq_end ||
           !find_n(plan_jq_line, plan_jq_end,
                   "fuzz_default_pressure/plan.json") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "recommended_command") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "recommended_preview_command") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "recommended_low_cpu_command") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "language_score_good_threshold_percent") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "language_score_evidence_cap_percent") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "language_score_note") ||
              !find_n(plan_jq_line, plan_jq_end,
                      "next_run_language_score_delta_percent") ||
              !find_n(plan_jq_line, plan_jq_end,
                      "coverage_queue_count") ||
              !find_n(plan_jq_line, plan_jq_end,
                      "coverage_queue_lanes") ||
              !find_n(plan_jq_line, plan_jq_end,
                      "plan_next_command") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "plan_next_preview_command") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "plan_next_low_cpu_command") ||
           find_n(plan_jq_line, plan_jq_end, ".md.json") ||
           find_n(plan_jq_line, plan_jq_end, "/home/e/nytrix"))
         (void)string_list_push_copy(
             errors, "default plan Controls omit compact plan jq readout");
     }
     free(md.data);
  free(markdown);
  free(plan_next_action);
  free(plan_next_command);
  free(plan_next_low_cpu);
  free(plan_next_preview);
  free(recommended_action);
  free(recommended_command);
  free(recommended_low_cpu);
  free(recommended_preview);
  free(recommended_repeat_mode);
  free(handoff_command);
  free(language_label);
  free(score_label);
  free(campaign_state);
  free(campaign_reason);
  free(completion_state);
  free(completion_reason);
  free(mode);
  free(run_command);
}

static void selftest_validate_fuzz_all_plan_coverage_next(const char *json,
                                                          string_list_t *errors,
                                                          int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-plan") != 0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan mode mismatch");
  selftest_expect_top_alias_string(json, "recommended_action", errors);
  selftest_expect_top_alias_string(json, "recommended_reason", errors);
  selftest_expect_top_alias_string(json, "recommended_command", errors);
  selftest_expect_top_alias_string(json, "recommended_low_cpu_command",
                                   errors);
  selftest_expect_top_alias_string(json, "recommended_preview_command",
                                   errors);
  selftest_expect_top_alias_number(json, "recommended_repeat_count",
                                   "recommended_repeat_count", errors);
  selftest_expect_top_alias_string(json, "plan_next_action", errors);
  selftest_expect_top_alias_string(json, "plan_next_lane", errors);
  selftest_expect_top_alias_string(json, "plan_next_reason", errors);
  selftest_expect_top_alias_string(json, "plan_next_command", errors);
  selftest_expect_top_alias_string(json, "plan_next_low_cpu_command",
                                   errors);
     selftest_expect_top_alias_string(json, "plan_next_preview_command",
                                      errors);
     selftest_expect_top_alias_number(json, "coverage_queue_count",
                                      "coverage_queue_count", errors);
     selftest_expect_top_alias_number(
         json, "coverage_queue_non_advisory_count",
         "coverage_queue_non_advisory_count", errors);
     selftest_expect_top_alias_number(json, "coverage_queue_advisory_count",
                                      "coverage_queue_advisory_count",
                                      errors);
     selftest_expect_top_alias_string(json, "coverage_queue_lanes", errors);
     double backlog_lanes = -1.0;
  if (!summary_number_from_report(json, "coverage_backlog_lanes",
                                  &backlog_lanes) ||
      backlog_lanes <= 0.0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan backlog missing");
  double queue_count = -1.0, queue_primary = -1.0, queue_advisory = -1.0;
  if (!summary_number_from_report(json, "coverage_queue_count",
                                  &queue_count) ||
      queue_count <= 0.0 ||
      queue_count != backlog_lanes)
    (void)string_list_push_copy(errors,
                                "coverage-next plan queue count missing");
  if (!summary_number_from_report(json,
                                  "coverage_queue_non_advisory_count",
                                  &queue_primary) ||
      queue_primary <= 0.0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan primary queue count missing");
  if (!summary_number_from_report(json,
                                  "coverage_queue_advisory_count",
                                  &queue_advisory) ||
      queue_advisory < 0.0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan advisory queue count missing");
  char *coverage_queue_lanes =
      summary_string_from_report(json, "coverage_queue_lanes");
  if (!coverage_queue_lanes ||
      !strstr(coverage_queue_lanes, "afl") ||
      !strstr(coverage_queue_lanes, "compiler_findings"))
    (void)string_list_push_copy(errors,
                                "coverage-next plan queue lanes missing");
  char *coverage_queue_json =
      summary_array_from_report(json, "coverage_queue");
  if (!coverage_queue_json ||
      count_json_array_items(coverage_queue_json) != 2 ||
      !strstr(coverage_queue_json, "\"lane\":\"afl\"") ||
      !strstr(coverage_queue_json, "\"lane\":\"compiler_findings\"") ||
      !strstr(coverage_queue_json, "\"command\":\"./build/nytrix"))
    (void)string_list_push_copy(errors,
                                "coverage-next plan queue array missing");
  char *plan_next_action =
      summary_string_from_report(json, "plan_next_action");
  char *plan_next_lane =
      summary_string_from_report(json, "plan_next_lane");
  char *plan_next_reason =
      summary_string_from_report(json, "plan_next_reason");
  char *plan_next_command =
      summary_string_from_report(json, "plan_next_command");
  char *plan_next_low_cpu =
      summary_string_from_report(json, "plan_next_low_cpu_command");
  char *plan_next_preview =
      summary_string_from_report(json, "plan_next_preview_command");
  char *recommended_action =
      summary_string_from_report(json, "recommended_action");
  char *recommended_reason =
      summary_string_from_report(json, "recommended_reason");
  char *recommended_command =
      summary_string_from_report(json, "recommended_command");
  char *recommended_low_cpu =
      summary_string_from_report(json, "recommended_low_cpu_command");
  char *recommended_preview =
      summary_string_from_report(json, "recommended_preview_command");
  char *plan_state_refresh =
      summary_string_from_report(json, "coverage_next_state_refresh_command");
  char *handoff_command =
      summary_string_from_report(json, "handoff_command");
  char *handoff_preview =
      summary_string_from_report(json, "handoff_preview_command");
  char *run_command = summary_string_from_report(json, "run_command");
  if (!plan_next_action ||
      strcmp(plan_next_action, "run-missing-evidence") != 0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan action was not promoted");
  if (!plan_next_lane || strcmp(plan_next_lane, "afl") != 0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan lane was not copied");
  if (!plan_next_reason || !strstr(plan_next_reason, "budget"))
    (void)string_list_push_copy(errors,
                                "coverage-next plan reason was not copied");
  if (!plan_next_command ||
      !strstr(plan_next_command, "run-missing-evidence.sh") ||
      strstr(plan_next_command, "fuzz all run"))
    (void)string_list_push_copy(errors,
                                "coverage-next plan command did not prefer guarded handoff");
     if (!plan_next_low_cpu ||
         !selftest_command_uses_env_nice(plan_next_low_cpu) ||
         !strstr(plan_next_low_cpu, "NYTRIX_MISSING_EVIDENCE_HOURS=1") ||
         !strstr(plan_next_low_cpu, "NYTRIX_MISSING_EVIDENCE_THREADS=10%") ||
         !strstr(plan_next_low_cpu, "run-missing-evidence.sh"))
       (void)string_list_push_copy(errors,
                                   "coverage-next plan low-cpu command was missing");
     if (!plan_next_preview ||
         !selftest_command_uses_env_nice(plan_next_preview) ||
         !strstr(plan_next_preview, "fuzz all preflight") ||
         !strstr(plan_next_preview, "build/cache/scratch"))
       (void)string_list_push_copy(errors,
                                   "coverage-next plan preview was missing");
  if (!recommended_action || !plan_next_action ||
      strcmp(recommended_action, plan_next_action) != 0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan recommended action alias wrong");
  if (!recommended_reason || !plan_next_reason ||
      strcmp(recommended_reason, plan_next_reason) != 0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan recommended reason alias wrong");
  if (!recommended_command || !plan_next_command ||
      strcmp(recommended_command, plan_next_command) != 0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan recommended command alias wrong");
  if (!recommended_low_cpu || !plan_next_low_cpu ||
      strcmp(recommended_low_cpu, plan_next_low_cpu) != 0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan recommended low-cpu alias wrong");
  if (!recommended_preview || !plan_next_preview ||
      strcmp(recommended_preview, plan_next_preview) != 0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan recommended preview alias wrong");
     if (!plan_state_refresh ||
         !selftest_command_uses_env_nice(plan_state_refresh) ||
         !strstr(plan_state_refresh, "NYTRIX_RUN_DRY_RUN=1") ||
         !strstr(plan_state_refresh, "run-missing-evidence.sh"))
       (void)string_list_push_copy(errors,
                                   "coverage-next plan state refresh missing");
  if (!handoff_command ||
      !strstr(handoff_command, "fuzz_plan_coverage_next/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "coverage-next plan lost campaign handoff");
  if (!handoff_preview ||
      !strstr(handoff_preview, "NYTRIX_RUN_DRY_RUN=1") ||
      !strstr(handoff_preview, "NYTRIX_RUN_REPEAT=target") ||
      !strstr(handoff_preview, "fuzz_plan_coverage_next/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "coverage-next plan lost campaign dry-run preview");
  if (!run_command ||
      !selftest_command_uses_env_nice(run_command) ||
      !strstr(run_command, "fuzz all run") ||
      !strstr(run_command, "--threads 25%"))
    (void)string_list_push_copy(errors,
                                "coverage-next plan lost guarded campaign command");
  double language_score = -1.0, language_alias = -1.0;
  double language_gap = -1.0;
  double language_signal = -1.0, language_cap = -1.0;
  double signal_alias = -1.0, cap_alias = -1.0;
  double next_language_score = -1.0, next_language_alias = -1.0;
  double runs_to_good = -2.0;
  double coverage_depth = -1.0, coverage_percent = -1.0;
  if (!summary_number_from_report(json, "language_score_percent",
                                  &language_score) ||
      language_score < 0.0 || language_score > 100.0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan language score missing");
  if (!summary_number_from_report(json, "language_score",
                                  &language_alias) ||
      language_alias != language_score)
    (void)string_list_push_copy(errors,
                                "coverage-next plan language score alias missing");
  if (!summary_number_from_report(json, "language_score_gap_percent",
                                  &language_gap) ||
      language_gap < 0.0 || language_gap > 100.0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan language gap missing");
  if (!summary_number_from_report(json,
                                  "next_run_language_score_percent",
                                  &next_language_score) ||
      next_language_score < 0.0 || next_language_score > 100.0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan next-run score missing");
  if (!summary_number_from_report(json, "next_run_language_score",
                                  &next_language_alias) ||
      next_language_alias != next_language_score)
    (void)string_list_push_copy(errors,
                                "coverage-next plan next-run alias missing");
  if (!summary_number_from_report(json, "coverage_depth_percent",
                                  &coverage_depth) ||
      coverage_depth < 0.0 ||
      !summary_number_from_report(json, "coverage_percent",
                                  &coverage_percent) ||
      coverage_percent != coverage_depth)
    (void)string_list_push_copy(errors,
                                "coverage-next plan coverage percent alias missing");
  if (!summary_number_from_report(json,
                                  "language_score_signal_percent",
                                  &language_signal) ||
      !summary_number_from_report(json,
                                  "language_score_evidence_cap_percent",
                                  &language_cap) ||
      !summary_number_from_report(json, "signal_health_percent",
                                  &signal_alias) ||
      !summary_number_from_report(json, "evidence_cap_percent",
                                  &cap_alias) ||
      language_signal != signal_alias || language_cap != cap_alias)
    (void)string_list_push_copy(errors,
                                "coverage-next plan score aliases missing");
  if (!summary_number_from_report(json, "runs_to_good_language_score",
                                  &runs_to_good))
    (void)string_list_push_copy(errors,
                                "coverage-next plan runs-to-good missing");
  double plan_advisory_timeouts = -1.0;
  bool plan_cache_ok = false, plan_ny_ok = false, plan_old_writer = true;
  if (!summary_number_from_report(json,
                                  "language_score_advisory_timeouts",
                                  &plan_advisory_timeouts) ||
      plan_advisory_timeouts < 0.0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan advisory timeout count missing");
  if (!summary_bool_from_report(json,
                                "language_score_cache_policy_ok",
                                &plan_cache_ok) ||
      !plan_cache_ok)
    (void)string_list_push_copy(errors,
                                "coverage-next plan cache guard missing");
  if (!summary_bool_from_report(json,
                                "language_score_ny_bin_exists",
                                &plan_ny_ok) ||
      !plan_ny_ok)
    (void)string_list_push_copy(errors,
                                "coverage-next plan Ny binary guard missing");
  if (!summary_bool_from_report(json,
                                "language_score_active_old_writer_present",
                                &plan_old_writer))
    (void)string_list_push_copy(errors,
                                "coverage-next plan old-writer guard missing");
  (void)plan_old_writer;
  char *campaign_state = summary_string_from_report(json, "campaign_state");
  char *campaign_reason =
      summary_string_from_report(json, "campaign_incomplete_reason");
  char *completion_state = summary_string_from_report(json, "completion_state");
  char *completion_reason =
      summary_string_from_report(json, "completion_reason");
  if (!campaign_state || !*campaign_state ||
      !completion_state || strcmp(campaign_state, completion_state) != 0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan completion state alias missing");
  if (!campaign_reason || !*campaign_reason ||
      !completion_reason || strcmp(campaign_reason, completion_reason) != 0)
    (void)string_list_push_copy(errors,
                                "coverage-next plan completion reason alias missing");
  char *language_label = summary_string_from_report(json,
                                                    "language_score_label");
  if (!language_label || !*language_label)
    (void)string_list_push_copy(errors,
                                "coverage-next plan language label missing");

  char *markdown = summary_string_from_report(json, "markdown");
  char root[4096] = {0};
  file_buf_t md = {0};
  if (!markdown || !*markdown) {
    (void)string_list_push_copy(errors,
                                "coverage-next plan markdown path missing");
  } else if (!find_nytrix_root(root, sizeof(root)) ||
             !read_file_maybe_root(root, markdown, &md) ||
             !md.data) {
    (void)string_list_push_copy(errors,
                                "coverage-next plan markdown was not readable");
  } else {
    const char *next_section = strstr(md.data, "\n## Next\n");
       const char *controls_section = strstr(md.data, "\n## Controls\n");
    const char *campaign_section = strstr(md.data, "\n## Campaign Run\n");
    const char *missing_handoff = strstr(md.data,
                                         "run-missing-evidence.sh");
    const char *low_cpu_handoff =
        strstr(md.data, "NYTRIX_MISSING_EVIDENCE_THREADS=10%");
    const char *state_command =
        controls_section ?
            strstr(controls_section, "run-missing-evidence-state.json") :
            NULL;
    const char *refresh_command =
        controls_section ? strstr(controls_section,
                                  "NYTRIX_RUN_DRY_RUN=1") : NULL;
    const char *stop_command =
        controls_section ? strstr(controls_section,
                                  "missing-evidence-stop") : NULL;
    const char *state_in_next =
        next_section ? strstr(next_section,
                              "run-missing-evidence-state.json") : NULL;
    const char *stop_in_next =
        next_section ? strstr(next_section, "missing-evidence-stop") : NULL;
    const char *run_next = strstr(md.data, "run-next.sh");
       const char *campaign_preview =
           campaign_section ? strstr(campaign_section,
                                     "NYTRIX_RUN_DRY_RUN=1 NYTRIX_RUN_REPEAT=target") : NULL;
       const char *plan_jq_line =
           controls_section ?
               strstr(controls_section,
                      "jq '{ok,cases,ok_count,failure_count,campaign_percent,"
                      "campaign_remaining_percent,thread_years,"
                      "target_thread_years,score_percent,stability_percent,"
                      "stability_score_percent,language_score_percent,") :
               NULL;
       const char *plan_jq_end =
           plan_jq_line ? strchr(plan_jq_line, '\n') : NULL;
       if (!plan_jq_line || !plan_jq_end ||
           !find_n(plan_jq_line, plan_jq_end,
                   "fuzz_plan_coverage_next/plan.json") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "recommended_command") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "recommended_preview_command") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "recommended_low_cpu_command") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "language_score_good_threshold_percent") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "language_score_evidence_cap_percent") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "language_score_note") ||
              !find_n(plan_jq_line, plan_jq_end,
                      "next_run_language_score_delta_percent") ||
              !find_n(plan_jq_line, plan_jq_end,
                      "coverage_queue_count") ||
              !find_n(plan_jq_line, plan_jq_end,
                      "coverage_queue_lanes") ||
              !find_n(plan_jq_line, plan_jq_end,
                      "plan_next_command") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "plan_next_preview_command") ||
           !find_n(plan_jq_line, plan_jq_end,
                   "plan_next_low_cpu_command") ||
           find_n(plan_jq_line, plan_jq_end, ".md.json") ||
           find_n(plan_jq_line, plan_jq_end, "/home/e/nytrix"))
         (void)string_list_push_copy(
             errors, "coverage-next plan Controls omit compact plan jq readout");
       if (!strstr(md.data,
                   "collect missing coverage evidence before unattended campaign runs"))
      (void)string_list_push_copy(errors,
                                  "coverage-next plan TLDR was not action-first");
    if (!strstr(md.data, "- Coverage queue:") ||
        !strstr(md.data, "compiler_findings") ||
        !strstr(md.data, "primary"))
      (void)string_list_push_copy(errors,
                                  "coverage-next plan markdown lost queue");
    if (!strstr(md.data, "- Language:") ||
        !strstr(md.data, "good >=") ||
        !strstr(md.data, "next run"))
      (void)string_list_push_copy(errors,
                                  "coverage-next plan markdown lost language score");
    if (!strstr(md.data, "- Score guards:") ||
        !strstr(md.data, "advisory timeouts") ||
        !strstr(md.data, "old writer"))
      (void)string_list_push_copy(errors,
                                  "coverage-next plan markdown lost score guards");
    if (!next_section || !controls_section || !campaign_section ||
        (next_section && controls_section &&
         controls_section < next_section) ||
        (controls_section && campaign_section &&
         campaign_section < controls_section))
      (void)string_list_push_copy(errors,
                                  "coverage-next plan lost Next/Controls/Campaign split");
    if (!missing_handoff || !next_section ||
        (controls_section && missing_handoff > controls_section))
      (void)string_list_push_copy(errors,
                                  "coverage-next plan did not put missing evidence in Next");
    if (!low_cpu_handoff || !next_section ||
        low_cpu_handoff < next_section ||
        (controls_section && low_cpu_handoff > controls_section))
      (void)string_list_push_copy(errors,
                                  "coverage-next plan did not put low-cpu handoff in Next");
    if (!state_command || !refresh_command || !stop_command ||
        (campaign_section &&
         (state_command > campaign_section ||
          refresh_command > campaign_section ||
          stop_command > campaign_section)))
      (void)string_list_push_copy(errors,
                                  "coverage-next plan lost Controls state refresh or pause commands");
    if ((state_in_next && controls_section &&
         state_in_next < controls_section) ||
        (stop_in_next && controls_section && stop_in_next < controls_section))
      (void)string_list_push_copy(errors,
                                  "coverage-next plan leaked controls into Next");
    if (!run_next || !campaign_section || run_next < campaign_section)
      (void)string_list_push_copy(errors,
                                  "coverage-next plan leaked run-next into Next");
    if (!campaign_preview || !run_next ||
        campaign_preview > run_next)
      (void)string_list_push_copy(errors,
                                  "coverage-next plan lost campaign dry-run preview");
    if (strstr(md.data, "$JSON"))
      (void)string_list_push_copy(errors,
                                  "coverage-next plan leaked stale placeholders");
  }
  free(md.data);
  free(markdown);
  free(plan_next_action);
  free(plan_next_lane);
  free(plan_next_reason);
  free(plan_next_command);
  free(plan_next_low_cpu);
  free(plan_next_preview);
  free(recommended_action);
  free(recommended_reason);
  free(recommended_command);
  free(recommended_low_cpu);
  free(recommended_preview);
  free(plan_state_refresh);
  free(handoff_command);
  free(handoff_preview);
  free(run_command);
  free(language_label);
  free(coverage_queue_json);
  free(coverage_queue_lanes);
  free(campaign_state);
  free(campaign_reason);
  free(completion_state);
  free(completion_reason);
  free(mode);
}

static void selftest_validate_fuzz_all_coverage_commands(const char *json,
                                                         string_list_t *errors,
                                                         int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-coverage") != 0)
    (void)string_list_push_copy(errors,
                                "coverage command report mode mismatch");
  double detail_rows = -1.0, backlog_lanes = -1.0;
  double coverage_depth = -1.0, coverage_percent = -1.0;
  if (!summary_number_from_report(json, "coverage_detail_rows",
                                  &detail_rows) ||
      detail_rows <= 0.0)
    (void)string_list_push_copy(errors,
                                "coverage command fixture had no detail rows");
  if (!summary_number_from_report(json, "coverage_depth_percent",
                                  &coverage_depth) ||
      coverage_depth < 0.0 ||
      !summary_number_from_report(json, "coverage_percent",
                                  &coverage_percent) ||
      coverage_percent != coverage_depth)
    (void)string_list_push_copy(errors,
                                "coverage percent alias missing or wrong");
  if (!summary_number_from_report(json, "coverage_backlog_lanes",
                                  &backlog_lanes) ||
      backlog_lanes <= 0.0)
    (void)string_list_push_copy(errors,
                                "coverage command fixture had no backlog lanes");
  if (!strstr(json, "\"lane\":\"nytrix_fuzz\"") ||
      !strstr(json, "--dir build/fuzz/all --allow-nytrix --json build/fuzz/all/with-nytrix.json"))
    (void)string_list_push_copy(errors,
                                "Nytrix coverage backlog command is not explicit");
  if (!strstr(json, "\"lane\":\"nytrix_sanitizers\"") ||
      !strstr(json, "--dir build/fuzz/all --allow-nytrix --json build/fuzz/all/with-sanitizers.json"))
    (void)string_list_push_copy(errors,
                                "sanitizer coverage backlog command is not explicit");
  if (!strstr(json, "--only-lane afl --profile insane") ||
      !strstr(json, "--dir build/fuzz/all --json build/fuzz/all/with-afl.json"))
    (void)string_list_push_copy(errors,
                                "AFL coverage backlog command lost focused repo-local dir");
  if (!strstr(json, "--dir build/fuzz/all --json build/fuzz/all/with-synth.json"))
    (void)string_list_push_copy(errors,
                                "synth coverage backlog command lost repo-local dir");
  if (strstr(json, "$JSON") ||
      strstr(json, "--threads 25% --json build/fuzz/all/with-nytrix.json") ||
      strstr(json, "--threads 25% --json build/fuzz/all/with-sanitizers.json"))
    (void)string_list_push_copy(errors,
                                "coverage backlog command leaked stale placeholders");
  double queue_count = -1.0, queue_non_advisory = -1.0;
  double queue_advisory = -1.0;
  if (!summary_number_from_report(json, "coverage_queue_count",
                                  &queue_count) ||
      queue_count <= 0.0)
    (void)string_list_push_copy(errors,
                                "coverage queue count was missing");
  if (!summary_number_from_report(json,
                                  "coverage_queue_non_advisory_count",
                                  &queue_non_advisory) ||
      queue_non_advisory <= 0.0)
    (void)string_list_push_copy(errors,
                                "coverage primary queue count was missing");
  if (!summary_number_from_report(json,
                                  "coverage_queue_advisory_count",
                                  &queue_advisory) ||
      queue_advisory <= 0.0)
    (void)string_list_push_copy(errors,
                                "coverage advisory queue count was missing");
  if (queue_count > 0.0 && backlog_lanes > 0.0 &&
      queue_count != backlog_lanes)
    (void)string_list_push_copy(errors,
                                "coverage queue count diverged from backlog");
  char *coverage_queue_lanes =
      summary_string_from_report(json, "coverage_queue_lanes");
  if (!coverage_queue_lanes || !strstr(coverage_queue_lanes, "afl") ||
      !strstr(coverage_queue_lanes, "synth_lanes") ||
      !strstr(coverage_queue_lanes, "nytrix_fuzz"))
    (void)string_list_push_copy(errors,
                                "coverage queue lanes were missing");
  if (coverage_queue_lanes) {
    char *q_afl = strstr(coverage_queue_lanes, "afl");
    char *q_synth = strstr(coverage_queue_lanes, "synth_lanes");
    char *q_nytrix = strstr(coverage_queue_lanes, "nytrix_fuzz");
    if ((q_afl && q_nytrix && q_nytrix < q_afl) ||
        (q_synth && q_nytrix && q_nytrix < q_synth))
      (void)string_list_push_copy(errors,
                                  "coverage queue put advisory lanes first");
  }
  if (!strstr(json, "\"coverage_queue\":[") ||
      !strstr(json, "\"lane\":\"afl\"") ||
      !strstr(json, "\"lane\":\"nytrix_fuzz\""))
    (void)string_list_push_copy(errors,
                                "coverage queue JSON array was missing");

  char *coverage_next_action =
      summary_string_from_report(json, "coverage_next_action");
  char *coverage_next_lane =
      summary_string_from_report(json, "coverage_next_lane");
  char *coverage_next_reason =
      summary_string_from_report(json, "coverage_next_reason");
  char *coverage_next_command =
      summary_string_from_report(json, "coverage_next_command");
  char *coverage_next_guarded =
      summary_string_from_report(json, "coverage_next_guarded_command");
  char *coverage_next_low_cpu =
      summary_string_from_report(json, "coverage_next_low_cpu_command");
  char *coverage_next_preview =
      summary_string_from_report(json, "coverage_next_preview_command");
  char *recommended_action =
      summary_string_from_report(json, "recommended_action");
  char *recommended_reason =
      summary_string_from_report(json, "recommended_reason");
  char *recommended_command =
      summary_string_from_report(json, "recommended_command");
  char *recommended_preview =
      summary_string_from_report(json, "recommended_preview_command");
  char *coverage_next_state_command =
      summary_string_from_report(json, "coverage_next_state_command");
  char *coverage_next_state_refresh =
      summary_string_from_report(json, "coverage_next_state_refresh_command");
  bool coverage_next_state_refresh_required = false;
  bool have_coverage_next_state_refresh_required =
      summary_bool_from_report(json,
                               "coverage_next_state_refresh_required",
                               &coverage_next_state_refresh_required);
  char *coverage_next_state_refresh_reason =
      summary_string_from_report(json,
                                 "coverage_next_state_refresh_reason");
  char *recommended_state_refresh_command =
      summary_string_from_report(json, "recommended_state_refresh_command");
  char *coverage_next_state =
      summary_string_from_report(json, "coverage_next_state");
  char *coverage_next_state_stale_reason =
      summary_string_from_report(json, "coverage_next_state_stale_reason");
  char *coverage_next_state_phase =
      summary_string_from_report(json, "coverage_next_state_phase");
  char *coverage_next_state_child_status =
      summary_string_from_report(json, "coverage_next_state_child_status");
  char *coverage_next_stop_command =
      summary_string_from_report(json, "coverage_next_stop_command");
  char *coverage_next_resume_command =
      summary_string_from_report(json, "coverage_next_resume_command");
  if (backlog_lanes > 0.0) {
    if (!coverage_next_action ||
        strcmp(coverage_next_action, "run-missing-evidence") != 0)
      (void)string_list_push_copy(errors,
                                  "coverage report next action was missing");
    if (!coverage_next_lane || !*coverage_next_lane)
      (void)string_list_push_copy(errors,
                                  "coverage report next lane was missing");
    if (!coverage_next_command || !strstr(coverage_next_command, "fuzz all run"))
      (void)string_list_push_copy(errors,
                                  "coverage report next command was missing");
    if (coverage_next_command && strstr(coverage_next_command, "fuzz all run") &&
           (!coverage_next_guarded ||
            !strstr(coverage_next_guarded, "run-missing-evidence.sh") ||
            !coverage_next_low_cpu ||
            !selftest_command_uses_env_nice(coverage_next_low_cpu) ||
            !strstr(coverage_next_low_cpu, "NYTRIX_MISSING_EVIDENCE_HOURS=1") ||
            !strstr(coverage_next_low_cpu, "NYTRIX_MISSING_EVIDENCE_THREADS=10%") ||
            !strstr(coverage_next_low_cpu, "run-missing-evidence.sh") ||
            !coverage_next_preview ||
            !strstr(coverage_next_preview, "fuzz all preflight") ||
            !selftest_command_uses_env_nice(coverage_next_preview) ||
            !strstr(coverage_next_preview, "build/cache/scratch") ||
         !coverage_next_state_command ||
         !strstr(coverage_next_state_command,
                 "jq {state,event,live,fresh,") ||
         !strstr(coverage_next_state_command,
                 "run-missing-evidence-state.json") ||
         strstr(coverage_next_state_command,
                "cat build/fuzz/all/run-missing-evidence-state.json") ||
            !coverage_next_state_refresh ||
            !selftest_command_uses_env_nice(coverage_next_state_refresh) ||
            !strstr(coverage_next_state_refresh, "NYTRIX_RUN_DRY_RUN=1") ||
            !strstr(coverage_next_state_refresh,
                    "run-missing-evidence.sh") ||
         !coverage_next_stop_command ||
         !strstr(coverage_next_stop_command, "missing-evidence-stop") ||
         !coverage_next_resume_command ||
         !strstr(coverage_next_resume_command, "missing-evidence-stop")))
      (void)string_list_push_copy(errors,
                                  "coverage report guarded next handoff was missing");
    const char *expected_recommended_command =
        coverage_next_guarded && *coverage_next_guarded ?
            coverage_next_guarded : coverage_next_command;
    if (!recommended_action || !coverage_next_action ||
        strcmp(recommended_action, coverage_next_action) != 0)
      (void)string_list_push_copy(errors,
                                  "coverage recommended action alias wrong");
    if (!recommended_reason || !coverage_next_reason ||
        strcmp(recommended_reason, coverage_next_reason) != 0)
      (void)string_list_push_copy(errors,
                                  "coverage recommended reason alias wrong");
    if (!recommended_command || !expected_recommended_command ||
        strcmp(recommended_command, expected_recommended_command) != 0)
      (void)string_list_push_copy(errors,
                                  "coverage recommended command alias wrong");
    if (!recommended_preview || !coverage_next_preview ||
        strcmp(recommended_preview, coverage_next_preview) != 0)
      (void)string_list_push_copy(errors,
                                  "coverage recommended preview alias wrong");
    if (coverage_next_command && strstr(coverage_next_command, "fuzz all run") &&
        (!coverage_next_state || !*coverage_next_state ||
         !coverage_next_state_stale_reason ||
         !*coverage_next_state_stale_reason))
      (void)string_list_push_copy(errors,
                                  "coverage report next state summary was missing");
    bool coverage_next_state_readable = false;
    bool coverage_next_state_live = false;
    double coverage_next_state_age = -1.0;
    double coverage_next_state_stale_after = -1.0;
    if (coverage_next_command && strstr(coverage_next_command, "fuzz all run") &&
        !summary_bool_from_report(json, "coverage_next_state_readable",
                                  &coverage_next_state_readable))
      (void)string_list_push_copy(errors,
                                  "coverage report next state readability missing");
    if (coverage_next_command && strstr(coverage_next_command, "fuzz all run") &&
        !summary_bool_from_report(json, "coverage_next_state_live",
                                  &coverage_next_state_live))
      (void)string_list_push_copy(errors,
                                  "coverage report next state liveness missing");
    if (coverage_next_command && strstr(coverage_next_command, "fuzz all run") &&
        coverage_next_state_readable &&
        (!coverage_next_state_phase ||
         !coverage_next_state_child_status ||
         !*coverage_next_state_child_status ||
         !summary_number_from_report(json, "coverage_next_state_age_seconds",
                                     &coverage_next_state_age) ||
         coverage_next_state_age < 0.0 ||
         !summary_number_from_report(json,
                                     "coverage_next_state_stale_after_seconds",
                                     &coverage_next_state_stale_after) ||
         coverage_next_state_stale_after <= 0.0))
      (void)string_list_push_copy(
          errors, "coverage report next readable state fields were missing");
    if (coverage_next_command && strstr(coverage_next_command, "fuzz all run") &&
        !coverage_next_state_readable &&
        (coverage_next_state && strcmp(coverage_next_state, "missing") != 0))
      (void)string_list_push_copy(
          errors, "coverage report missing next state label was wrong");
    bool expected_state_refresh_required =
        coverage_next_command && strstr(coverage_next_command, "fuzz all run") &&
        coverage_next_state_refresh && *coverage_next_state_refresh &&
        !coverage_next_state_live &&
        coverage_next_state_stale_reason &&
        *coverage_next_state_stale_reason &&
        strcmp(coverage_next_state_stale_reason, "none") != 0;
    if (expected_state_refresh_required &&
        (!have_coverage_next_state_refresh_required ||
         !coverage_next_state_refresh_required ||
         !coverage_next_state_refresh_reason ||
         strcmp(coverage_next_state_refresh_reason,
                coverage_next_state_stale_reason) != 0 ||
         !recommended_state_refresh_command ||
         !strstr(recommended_state_refresh_command,
                 coverage_next_state_refresh)))
      (void)string_list_push_copy(
          errors, "coverage report next state refresh requirement was missing");
  } else if (coverage_next_action &&
             strcmp(coverage_next_action, "none") != 0) {
    (void)string_list_push_copy(errors,
                                "coverage report next action should be none");
  }

  char *markdown = summary_string_from_report(json, "markdown");
  char root[4096] = {0};
  file_buf_t md = {0};
  if (!markdown || !*markdown) {
    (void)string_list_push_copy(errors,
                                "coverage command markdown path missing");
  } else if (!find_nytrix_root(root, sizeof(root)) ||
             !read_file_maybe_root(root, markdown, &md) ||
             !md.data) {
    (void)string_list_push_copy(errors,
                                "coverage command markdown was not readable");
  } else {
    if (!strstr(md.data, "Coverage Backlog") ||
        !strstr(md.data, "Coverage Queue") ||
        !strstr(md.data, "Order:") ||
        !strstr(md.data, "Counts:") ||
        !strstr(md.data, "--dir build/fuzz/all --allow-nytrix --json build/fuzz/all/with-nytrix.json") ||
        !strstr(md.data, "--dir build/fuzz/all --allow-nytrix --json build/fuzz/all/with-sanitizers.json") ||
        !strstr(md.data, "--only-lane afl --profile insane"))
      (void)string_list_push_copy(errors,
                                  "coverage markdown lost explicit focused commands");
    const char *queue_section = strstr(md.data, "\n## Coverage Queue\n");
    const char *backlog_section = strstr(md.data, "\n## Coverage Backlog\n");
    if (!queue_section || !backlog_section || backlog_section < queue_section) {
      (void)string_list_push_copy(errors,
                                  "coverage markdown queue section missing");
    } else {
      const char *q_afl = strstr(queue_section, "afl");
      const char *q_synth = strstr(queue_section, "synth_lanes");
      const char *q_nytrix = strstr(queue_section, "nytrix_fuzz");
      if ((q_afl && q_nytrix && q_nytrix < q_afl) ||
          (q_synth && q_nytrix && q_nytrix < q_synth))
        (void)string_list_push_copy(
            errors, "coverage markdown queue put advisory lanes first");
    }
    if (backlog_lanes > 0.0 && coverage_next_command &&
        strstr(coverage_next_command, "fuzz all run") &&
        (!strstr(md.data, "Next evidence:") ||
         !strstr(md.data, coverage_next_command) ||
         !strstr(md.data, "Guarded:") ||
         !strstr(md.data, coverage_next_guarded ?
                          coverage_next_guarded : "") ||
         !strstr(md.data, "Preview:") ||
         !strstr(md.data, coverage_next_preview ?
                          coverage_next_preview : "") ||
         !strstr(md.data, "State:") ||
         !strstr(md.data, coverage_next_state_refresh ?
                          coverage_next_state_refresh : "") ||
         !strstr(md.data, "Next evidence state:") ||
         (coverage_next_state_refresh_required &&
          (!strstr(md.data, "Next evidence state refresh:") ||
           !strstr(md.data,
                   coverage_next_state_refresh_reason ?
                       coverage_next_state_refresh_reason : "") ||
           !strstr(md.data,
                   recommended_state_refresh_command ?
                       recommended_state_refresh_command : ""))) ||
         !strstr(md.data, "Pause:")))
      (void)string_list_push_copy(errors,
                                  "coverage markdown lost guarded next handoff");
    const char *next_section = strstr(md.data, "\n## Next\n");
    const char *controls_section = strstr(md.data, "\n## Controls\n");
    const char *refresh_section = strstr(md.data, "\n## Refresh\n");
    const char *coverage_refresh =
        strstr(md.data, "./build/nytrix fuzz all coverage");
    const char *status_refresh =
        strstr(md.data, "./build/nytrix fuzz all status --refresh");
    const char *afl_parser_gap = strstr(md.data, "`afl_parsers`");
    if (afl_parser_gap && coverage_next_guarded &&
        *coverage_next_guarded && coverage_next_command &&
        *coverage_next_command) {
      const char *next_gap = strstr(afl_parser_gap + 1, "\n- ");
      const char *raw_command_after_parser =
          strstr(afl_parser_gap, coverage_next_command);
      const char *guarded_after_parser =
          strstr(afl_parser_gap, coverage_next_guarded);
      if (raw_command_after_parser &&
          (!next_gap || raw_command_after_parser < next_gap) &&
          (!guarded_after_parser ||
           (next_gap && guarded_after_parser > next_gap) ||
           (next_section && guarded_after_parser > next_section)))
        (void)string_list_push_copy(
            errors,
            "coverage AFL parser backlog does not reuse guarded missing-evidence handoff");
    }
    const char *state_in_next =
        next_section ? strstr(next_section,
                              "run-missing-evidence-state.json") : NULL;
      const char *stop_in_next =
          next_section ? strstr(next_section, "missing-evidence-stop") : NULL;
      const char *state_in_controls =
          controls_section ?
              strstr(controls_section, "run-missing-evidence-state.json") :
              NULL;
      const char *refresh_in_controls =
          controls_section ?
              strstr(controls_section, "NYTRIX_RUN_DRY_RUN=1") : NULL;
      const char *stop_in_controls =
          controls_section ? strstr(controls_section,
                                    "missing-evidence-stop") : NULL;
      if (!next_section || !refresh_section ||
          (next_section && refresh_section && refresh_section < next_section))
        (void)string_list_push_copy(errors,
                                    "coverage markdown lost Next/Refresh split");
      if (backlog_lanes > 0.0 &&
          (!controls_section ||
           (next_section && controls_section &&
            controls_section < next_section) ||
           (controls_section && refresh_section &&
            refresh_section < controls_section)))
        (void)string_list_push_copy(errors,
                                    "coverage markdown lost Controls split");
      if ((state_in_next && controls_section &&
           state_in_next < controls_section) ||
          (stop_in_next && controls_section && stop_in_next < controls_section))
        (void)string_list_push_copy(errors,
                                    "coverage markdown mixed controls into Next");
      if (backlog_lanes > 0.0 &&
          (!state_in_controls || !refresh_in_controls ||
           !stop_in_controls ||
           !strstr(controls_section,
                   "jq {state,event,live,fresh,") ||
           strstr(controls_section,
                  "cat build/fuzz/all/run-missing-evidence-state.json") ||
           (refresh_section &&
            (state_in_controls > refresh_section ||
             refresh_in_controls > refresh_section ||
             stop_in_controls > refresh_section))))
        (void)string_list_push_copy(errors,
                                    "coverage markdown Controls block was missing");
      if (next_section && refresh_section && coverage_refresh &&
          coverage_refresh > next_section && coverage_refresh < refresh_section)
        (void)string_list_push_copy(errors,
                                  "coverage markdown mixed refresh into Next");
    if (!coverage_refresh || !status_refresh ||
        (refresh_section &&
         (coverage_refresh < refresh_section ||
          status_refresh < refresh_section)))
      (void)string_list_push_copy(errors,
                                  "coverage markdown refresh block was missing");
    if (strstr(md.data, "$JSON"))
      (void)string_list_push_copy(errors,
                                  "coverage markdown leaked stale placeholders");
  }
  free(md.data);
  free(markdown);
  free(coverage_next_action);
  free(coverage_next_lane);
  free(coverage_next_reason);
  free(coverage_next_command);
  free(coverage_next_guarded);
  free(coverage_next_low_cpu);
  free(coverage_next_preview);
  free(recommended_action);
  free(recommended_reason);
  free(recommended_command);
  free(recommended_preview);
  free(coverage_next_state_command);
  free(coverage_next_state_refresh);
  free(coverage_next_state_refresh_reason);
  free(recommended_state_refresh_command);
  free(coverage_next_state);
  free(coverage_next_state_stale_reason);
  free(coverage_next_state_phase);
  free(coverage_next_state_child_status);
  free(coverage_next_stop_command);
  free(coverage_next_resume_command);
  free(coverage_queue_lanes);
  free(mode);
}

static void selftest_validate_fuzz_all_coverage_focus_companions(
    const char *json, string_list_t *errors, int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-coverage") != 0)
    (void)string_list_push_copy(errors,
                                "coverage focus companion mode mismatch");
  double companion_reports = -1.0, backlog_lanes = -1.0, queue_count = -1.0;
  if (!summary_number_from_report(json,
                                  "coverage_companion_reports_considered",
                                  &companion_reports) ||
      companion_reports < 1.0)
    (void)string_list_push_copy(errors,
                                "coverage focus companion report was not counted");
  if (!summary_number_from_report(json, "coverage_backlog_lanes",
                                  &backlog_lanes) ||
      backlog_lanes <= 0.0)
    (void)string_list_push_copy(errors,
                                "coverage focus companion backlog missing");
  if (!summary_number_from_report(json, "coverage_queue_count",
                                  &queue_count) ||
      queue_count != backlog_lanes)
    (void)string_list_push_copy(errors,
                                "coverage focus companion queue count mismatch");
  char *coverage_queue_lanes =
      summary_string_from_report(json, "coverage_queue_lanes");
  if (!coverage_queue_lanes || strstr(coverage_queue_lanes, "afl") ||
      !strstr(coverage_queue_lanes, "synth_lanes") ||
      strstr(coverage_queue_lanes, "compiler_std_audit"))
    (void)string_list_push_copy(errors,
                                "coverage focus companion queue lanes wrong");
  char *coverage_queue_json =
      summary_array_from_report(json, "coverage_queue");
  if (!coverage_queue_json ||
      strstr(coverage_queue_json, "\"lane\":\"afl\"") ||
      !strstr(coverage_queue_json, "\"lane\":\"synth_lanes\"") ||
      strstr(coverage_queue_json, "\"lane\":\"compiler_std_audit\"") ||
      !strstr(json, "\"kind\":\"fuzz-all-coverage-companion\"") ||
      !strstr(json, "\"lane\":\"compiler_std_audit\"") ||
      !strstr(json, "\"evidence_mode\":\"compiler-std-audit\""))
    (void)string_list_push_copy(errors,
                                "coverage focus companion evidence row missing");
  char *markdown = summary_string_from_report(json, "markdown");
  char root[4096] = {0};
  file_buf_t md = {0};
  if (!markdown || !*markdown) {
    (void)string_list_push_copy(errors,
                                "coverage focus companion markdown missing");
  } else if (!find_nytrix_root(root, sizeof(root)) ||
             !read_file_maybe_root(root, markdown, &md) ||
             !md.data) {
    (void)string_list_push_copy(errors,
                                "coverage focus companion markdown unreadable");
  } else {
    const char *queue = strstr(md.data, "\n## Coverage Queue\n");
    const char *backlog = strstr(md.data, "\n## Coverage Backlog\n");
    if (!queue || !backlog || !strstr(queue, "`synth_lanes`") ||
        strstr(queue, "`afl`") ||
        (backlog && strstr(backlog, "compiler_std_audit")))
      (void)string_list_push_copy(errors,
                                  "coverage focus companion markdown queue wrong");
    if (strstr(md.data, "$JSON"))
      (void)string_list_push_copy(errors,
                                  "coverage focus companion markdown leaked placeholder");
    if (strstr(md.data,
               "cat build/fuzz/all/run-missing-evidence-state.json") ||
        strstr(md.data,
               "inspect `cat build/fuzz/all/run-missing-evidence-state.json`"))
      (void)string_list_push_copy(
          errors,
          "coverage focus companion markdown used raw missing-evidence state cat");
  }
  free(md.data);
  free(markdown);
  free(coverage_queue_json);
  free(coverage_queue_lanes);
  free(mode);
}

static void selftest_validate_fuzz_all_preflight_isolation(
    const char *json, string_list_t *errors, int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-preflight") != 0)
    (void)string_list_push_copy(errors,
                                "preflight isolation report mode mismatch");
  bool preflight_ok = false;
  if (!summary_bool_from_report(json, "preflight_ok", &preflight_ok) ||
      !preflight_ok)
    (void)string_list_push_copy(errors,
                                "preflight isolation did not pass preflight");
  bool cache_policy_ok = false;
  bool old_test_absent = false;
  bool old_fuzz_absent = false;
  bool old_build_absent = false;
  bool status_old_build_absent = false;
  bool active_output_writer = true;
  bool active_cache_writer = true;
  if (!summary_bool_from_report(json, "cache_policy_ok", &cache_policy_ok) ||
      !cache_policy_ok)
    (void)string_list_push_copy(errors,
                                "preflight isolation cache policy alias missing");
  if (!summary_bool_from_report(json, "old_nytrix_test_scratch_absent",
                                &old_test_absent) ||
      !old_test_absent)
    (void)string_list_push_copy(errors,
                                "preflight isolation old test alias missing");
  if (!summary_bool_from_report(json, "old_nytrix_fuzz_absent",
                                &old_fuzz_absent) ||
      !old_fuzz_absent)
    (void)string_list_push_copy(errors,
                                "preflight isolation old fuzz alias missing");
  if (!summary_bool_from_report(json, "old_nytrix_build_cache_absent",
                                &old_build_absent))
    (void)string_list_push_copy(errors,
                                "preflight isolation old build alias missing");
  if (!summary_bool_from_report(json, "status_old_nytrix_build_cache_absent",
                                &status_old_build_absent) ||
      status_old_build_absent != old_build_absent)
    (void)string_list_push_copy(errors,
                                "preflight isolation old build aliases diverge");
  if (!summary_bool_from_report(json,
                                "active_old_nytrix_output_writer_present",
                                &active_output_writer))
    (void)string_list_push_copy(errors,
                                "preflight isolation output writer alias wrong");
  if (!summary_bool_from_report(json,
                                "active_old_nytrix_cache_writer_present",
                                &active_cache_writer))
    (void)string_list_push_copy(errors,
                                "preflight isolation cache writer alias wrong");
  if (active_output_writer != active_cache_writer)
    (void)string_list_push_copy(errors,
                                "preflight isolation writer aliases diverge");
  double blocker_count = -1.0;
  double blockers = -1.0;
  double active_items = -1.0;
  double active_runs = -1.0;
  if (!summary_number_from_report(json, "blocker_count", &blocker_count) ||
      blocker_count < 0.0)
    (void)string_list_push_copy(errors,
                                "preflight isolation blocker alias missing");
  if (!summary_number_from_report(json, "blockers", &blockers) ||
      blockers != blocker_count)
    (void)string_list_push_copy(errors,
                                "preflight isolation blockers alias wrong");
  if (!summary_number_from_report(json, "active_items", &active_items) ||
      active_items < 0.0)
    (void)string_list_push_copy(errors,
                                "preflight isolation active alias missing");
  if (!summary_number_from_report(json, "active_runs", &active_runs) ||
      active_runs != active_items)
    (void)string_list_push_copy(errors,
                                "preflight isolation active_runs alias wrong");

  char *work_dir = summary_string_from_report(json, "work_dir");
  char *campaign_dir = summary_string_from_report(json, "campaign_dir");
  char *dir = summary_string_from_report(json, "dir");
  char *run_report = summary_string_from_report(json, "run_report");
  if (!work_dir || !strstr(work_dir, "fuzz_preflight_isolation/work"))
    (void)string_list_push_copy(errors,
                                "preflight isolation work_dir mismatch");
  if (!campaign_dir || !strstr(campaign_dir, "fuzz_preflight_isolation/campaign"))
    (void)string_list_push_copy(errors,
                                "preflight isolation campaign_dir mismatch");
  if (!dir || !campaign_dir || strcmp(dir, campaign_dir) != 0)
    (void)string_list_push_copy(errors,
                                "preflight isolation dir alias mismatch");
  if (!run_report || !strstr(run_report, "fuzz_preflight_isolation/work/all-smoke.json"))
    (void)string_list_push_copy(errors,
                                "preflight isolation run report escaped work_dir");
  if (run_report && campaign_dir && strstr(run_report, campaign_dir))
    (void)string_list_push_copy(errors,
                                "preflight isolation run report points at campaign_dir");

  char root[4096] = {0};
  if (!find_nytrix_root(root, sizeof(root)))
    (void)string_list_push_copy(errors,
                                "Nytrix root not found for preflight isolation");

  file_buf_t run = {0};
  if (!run_report || !*run_report ||
      !read_file_maybe_root(root, run_report, &run) || !run.data) {
    (void)string_list_push_copy(errors,
                                "preflight isolation work report unreadable");
  } else {
    if (!strstr(run.data, "\"lane_filter\":\"afl\"") ||
        !strstr(run.data, "\"lane_filter_active\":true"))
      (void)string_list_push_copy(errors,
                                  "preflight isolation did not preserve AFL filter in work report");
    if (strstr(run.data, "\"isolation_sentinel\":\"campaign\""))
      (void)string_list_push_copy(errors,
                                  "preflight isolation work report reused campaign sentinel");
  }

  char *campaign_smoke = NULL;
  if (campaign_dir && *campaign_dir)
    (void)asprintf(&campaign_smoke, "%s/all-smoke.json", campaign_dir);
  file_buf_t canonical = {0};
  if (!campaign_smoke || !read_file_maybe_root(root, campaign_smoke,
                                               &canonical) ||
      !canonical.data) {
    (void)string_list_push_copy(errors,
                                "preflight isolation campaign smoke unreadable");
  } else {
    if (!strstr(canonical.data, "\"isolation_sentinel\":\"campaign\"") ||
        !strstr(canonical.data, "\"lane_filter\":\"all\"") ||
        !strstr(canonical.data, "\"lane_filter_active\":false"))
      (void)string_list_push_copy(errors,
                                  "preflight isolation overwrote canonical smoke");
    if (strstr(canonical.data, "\"lane_filter\":\"afl\""))
      (void)string_list_push_copy(errors,
                                  "preflight isolation polluted canonical smoke with AFL filter");
  }
  if (!strstr(json, "\"name\":\"preflight_artifacts\"") ||
      !strstr(json, "fuzz_preflight_isolation/work/all-smoke-audit.json") ||
      !strstr(json, "fuzz_preflight_isolation/work/all-smoke-findings.json"))
    (void)string_list_push_copy(errors,
                                "preflight isolation artifact row lost work_dir reports");
  char *recommended_action =
      summary_string_from_report(json, "recommended_action");
  char *recommended_command =
      summary_string_from_report(json, "recommended_command");
  char *status_run_command =
      summary_string_from_report(json, "status_run_command");
  char *next_command = summary_string_from_report(json, "next_command");
  char *preview_command = summary_string_from_report(json, "preview_command");
  if (!recommended_action || strcmp(recommended_action, "triage-worklist") != 0)
    (void)string_list_push_copy(errors,
                                "preflight isolation recommendation action wrong");
  if (!recommended_command ||
      !strstr(recommended_command, "fuzz all worklist") ||
      strcmp(recommended_command, "./build/nytrix fuzz") == 0)
    (void)string_list_push_copy(errors,
                                "preflight isolation recommendation command is stale");
  if (!status_run_command ||
      !selftest_command_uses_env_nice(status_run_command) ||
      !strstr(status_run_command, "fuzz all run") ||
      !strstr(status_run_command, "fuzz_preflight_isolation/campaign"))
    (void)string_list_push_copy(errors,
                                "preflight isolation status run command missing guard");
  if (!next_command ||
      !selftest_command_uses_env_nice(next_command) ||
      !strstr(next_command, "fuzz_preflight_isolation/campaign/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "preflight isolation next command missing");
  if (!preview_command ||
      !strstr(preview_command, "NYTRIX_RUN_DRY_RUN=1") ||
      !strstr(preview_command, "fuzz_preflight_isolation/campaign/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "preflight isolation preview command missing");

  free(preview_command);
  free(next_command);
  free(status_run_command);
  free(recommended_command);
  free(recommended_action);
  free(canonical.data);
  free(run.data);
  free(campaign_smoke);
  free(run_report);
  free(dir);
  free(campaign_dir);
  free(work_dir);
  free(mode);
}

static void selftest_validate_fuzz_all_history_commands(const char *json,
                                                        string_list_t *errors,
                                                        int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-history") != 0)
    (void)string_list_push_copy(errors,
                                "history command report mode mismatch");
  double target = -1.0, hours = -1.0, reports = -1.0;
  double ignored_no_evidence = -1.0;
  if (!summary_number_from_report(json, "target_thread_years", &target) ||
      target < 0.0009 || target > 0.0011)
    (void)string_list_push_copy(errors,
                                "history command target geometry missing");
  if (!summary_number_from_report(json, "hours_per_run", &hours) ||
      hours < 0.99 || hours > 1.01)
    (void)string_list_push_copy(errors,
                                "history command hours geometry missing");
  if (!summary_number_from_report(json, "reports", &reports) ||
      reports != 1.0)
    (void)string_list_push_copy(errors,
                                "history command fixture report count wrong");
  if (!summary_number_from_report(json, "ignored_no_evidence_reports",
                                  &ignored_no_evidence) ||
      ignored_no_evidence != 1.0)
    (void)string_list_push_copy(errors,
                                "history command no-evidence report count wrong");
  char *thread_request = summary_string_from_report(json, "thread_request");
  char *profile = summary_string_from_report(json, "profile");
  if (!thread_request || strcmp(thread_request, "1") != 0)
    (void)string_list_push_copy(errors,
                                "history command thread request missing");
  if (!profile || strcmp(profile, "insane") != 0)
    (void)string_list_push_copy(errors,
                                "history command profile missing");

  char *markdown = summary_string_from_report(json, "markdown");
  char root[4096] = {0};
  file_buf_t md = {0};
  if (!markdown || !*markdown) {
    (void)string_list_push_copy(errors,
                                "history command markdown path missing");
  } else if (!find_nytrix_root(root, sizeof(root)) ||
             !read_file_maybe_root(root, markdown, &md) ||
             !md.data) {
    (void)string_list_push_copy(errors,
                                "history command markdown was not readable");
  } else {
    if (!strstr(md.data, "fuzz all status --refresh --strict --allow-full-pressure-remediation --dir") ||
        !strstr(md.data, "fuzz_history_commands") ||
        !strstr(md.data, "--history") ||
        !strstr(md.data, "fuzz_all_history_commands.json") ||
        !strstr(md.data, "--worklist") ||
        !strstr(md.data, "worklist.json") ||
        !strstr(md.data, "--coverage") ||
        !strstr(md.data, "coverage.json") ||
        !strstr(md.data, "--plan") ||
        !strstr(md.data, "plan.json") ||
        !strstr(md.data, "--target-thread-years 0.001") ||
        !strstr(md.data, "--hours 1") ||
        !strstr(md.data, "--threads 1") ||
        !strstr(md.data, "--profile insane") ||
        !strstr(md.data, "--json") ||
        !strstr(md.data, "status.json") ||
        !strstr(md.data, "--markdown") ||
        !strstr(md.data, "status.md"))
      (void)string_list_push_copy(errors,
                                  "history markdown status refresh command is incomplete");
    if (!strstr(md.data, "Ignored no-evidence attempts: 1"))
      (void)string_list_push_copy(errors,
                                  "history markdown omitted ignored no-evidence count");
    if (strstr(md.data, "$JSON") ||
        strstr(md.data, "/home/e/nytrix/build/cache/projects/test") ||
        strstr(md.data, "/home/e/nytrix/fuzz"))
      (void)string_list_push_copy(errors,
                                  "history markdown leaked stale placeholders");
  }
  free(md.data);
  free(markdown);
  free(thread_request);
  free(profile);
  free(mode);
}

static void selftest_validate_fuzz_full_pressure_remediation(const char *json,
                                                             string_list_t *errors,
                                                             int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-status") != 0)
    (void)string_list_push_copy(errors, "full-pressure remediation status mode mismatch");
  bool allow_remediation = false, ready = false, long_run_ready = false;
  bool latest_report_clean = false, latest_full_pressure_clean = false;
  bool latest_full_pressure_ok = true;
  bool latest_report_demoted_non_reproducing_afl_timeout = true;
  bool latest_full_pressure_demoted_non_reproducing_afl_timeout = false;
  bool strict = false, latest_only_non_reproducing_afl_timeout = false;
  double latest_full_pressure_failure_count = -1.0;
  if (!summary_bool_from_report(json, "strict", &strict) || !strict)
    (void)string_list_push_copy(errors, "full-pressure remediation did not preserve strict mode");
  if (!summary_bool_from_report(json, "allow_full_pressure_remediation",
                                &allow_remediation) ||
      !allow_remediation)
    (void)string_list_push_copy(errors, "full-pressure remediation allowance missing");
  if (!summary_bool_from_report(json, "ready", &ready) || !ready)
    (void)string_list_push_copy(errors, "full-pressure remediation should report a clean ready gate after raw replay demotion");
  if (!summary_bool_from_report(json, "long_run_ready", &long_run_ready) ||
      !long_run_ready)
    (void)string_list_push_copy(errors, "full-pressure remediation should keep the long-run gate ready after raw replay demotion");
  if (!summary_bool_from_report(json, "latest_report_clean",
                                &latest_report_clean) ||
      !latest_report_clean)
    (void)string_list_push_copy(errors, "full-pressure remediation should demote a non-reproducing latest report");
  if (!summary_bool_from_report(json, "latest_full_pressure_clean",
                                &latest_full_pressure_clean) ||
      !latest_full_pressure_clean)
    (void)string_list_push_copy(errors, "full-pressure remediation should demote a non-reproducing full-pressure report");
  if (!summary_bool_from_report(json, "latest_full_pressure_ok",
                                &latest_full_pressure_ok) ||
      latest_full_pressure_ok)
    (void)string_list_push_copy(errors, "full-pressure remediation should preserve the raw failed full-pressure gate");
  if (!summary_number_from_report(json, "latest_full_pressure_failure_count",
                                  &latest_full_pressure_failure_count) ||
      latest_full_pressure_failure_count < 1.0)
    (void)string_list_push_copy(errors, "full-pressure remediation should keep the raw full-pressure failure count");
  if (!summary_bool_from_report(
          json, "latest_report_demoted_non_reproducing_afl_timeout",
          &latest_report_demoted_non_reproducing_afl_timeout) ||
      latest_report_demoted_non_reproducing_afl_timeout)
    (void)string_list_push_copy(errors, "full-pressure remediation should not demote the clean latest smoke report");
  if (!summary_bool_from_report(
          json, "latest_full_pressure_demoted_non_reproducing_afl_timeout",
          &latest_full_pressure_demoted_non_reproducing_afl_timeout) ||
      !latest_full_pressure_demoted_non_reproducing_afl_timeout)
    (void)string_list_push_copy(errors, "full-pressure remediation should mark the full-pressure demotion");
  char *latest_full_pressure_clean_reason =
      summary_string_from_report(json, "latest_full_pressure_clean_reason");
  if (!latest_full_pressure_clean_reason ||
      strcmp(latest_full_pressure_clean_reason,
             "demoted-non-reproducing-afl-timeout") != 0)
    (void)string_list_push_copy(errors, "full-pressure remediation reason is wrong");
  if (!summary_bool_from_report(json, "latest_only_non_reproducing_afl_timeout",
                                &latest_only_non_reproducing_afl_timeout) ||
      latest_only_non_reproducing_afl_timeout)
    (void)string_list_push_copy(errors, "full-pressure remediation should not mark a clean latest smoke report as an AFL timeout");
  char *latest_report = summary_string_from_report(json, "latest_report");
  char *latest_full_pressure_report =
      summary_string_from_report(json, "latest_full_pressure_report");
  if (!latest_report || !strstr(latest_report, "latest-smoke.json"))
    (void)string_list_push_copy(errors, "full-pressure remediation fixture should keep clean smoke as latest report");
  if (!latest_full_pressure_report ||
      !strstr(latest_full_pressure_report, "failed-full-pressure.json"))
    (void)string_list_push_copy(errors, "full-pressure remediation fixture should keep failed report as latest full-pressure report");
  if (latest_report && latest_full_pressure_report &&
      strcmp(latest_report, latest_full_pressure_report) == 0)
    (void)string_list_push_copy(errors, "full-pressure remediation should distinguish latest report from latest full-pressure report");
  free(latest_report);
  free(latest_full_pressure_report);
  free(latest_full_pressure_clean_reason);
  double blocker_count = -1.0, full_pressure_reports = -1.0;
  double non_reproducing_afl_timeouts = -1.0;
  double historical_non_reproducing_afl_timeouts = -1.0;
  double advisory_timeouts = -1.0, current_advisory_timeouts = -1.0;
  double historical_advisory_timeouts = -1.0;
  if (!summary_number_from_report(json, "blocker_count", &blocker_count) ||
      blocker_count != 0.0)
    (void)string_list_push_copy(errors, "full-pressure remediation blocker count mismatch");
  if (!summary_number_from_report(json, "full_pressure_reports",
                                  &full_pressure_reports) ||
      full_pressure_reports != 1.0)
    (void)string_list_push_copy(errors, "full-pressure remediation fixture full-pressure count mismatch");
  if (!summary_number_from_report(json, "non_reproducing_afl_timeouts",
                                  &non_reproducing_afl_timeouts) ||
      non_reproducing_afl_timeouts != 1.0)
    (void)string_list_push_copy(errors, "full-pressure remediation did not count the demoted AFL timeout");
  if (!summary_number_from_report(json,
                                  "historical_non_reproducing_afl_timeouts",
                                  &historical_non_reproducing_afl_timeouts) ||
      historical_non_reproducing_afl_timeouts != 1.0)
    (void)string_list_push_copy(errors,
                                "full-pressure remediation did not count historical demoted AFL timeout rows");
  if (!summary_number_from_report(json, "advisory_timeouts",
                                  &advisory_timeouts) ||
      advisory_timeouts != 1.0)
    (void)string_list_push_copy(errors,
                                "full-pressure remediation advisory alias missing");
  if (!summary_number_from_report(json, "current_advisory_timeouts",
                                  &current_advisory_timeouts) ||
      current_advisory_timeouts != non_reproducing_afl_timeouts)
    (void)string_list_push_copy(errors,
                                "full-pressure remediation current advisory alias wrong");
  if (!summary_number_from_report(json, "historical_advisory_timeouts",
                                  &historical_advisory_timeouts) ||
      historical_advisory_timeouts != historical_non_reproducing_afl_timeouts)
    (void)string_list_push_copy(errors,
                                "full-pressure remediation historical advisory alias wrong");
  char *advisory_state = summary_string_from_report(json, "advisory_state");
  if (!advisory_state ||
      strcmp(advisory_state, "current-timeouts") != 0)
    (void)string_list_push_copy(errors,
                                "full-pressure remediation advisory state wrong");
  char *advisory_action =
      summary_string_from_report(json, "advisory_action_command");
  if (!advisory_action ||
      !selftest_command_uses_env_nice(advisory_action) ||
      !strstr(advisory_action, "fuzz all worklist") ||
      !strstr(advisory_action, "--include-history") ||
      !strstr(advisory_action, "fuzz_full_pressure_remediation/history.json") ||
      !strstr(advisory_action, "worklist-history.json") ||
      !strstr(advisory_action, "worklist-history.md"))
    (void)string_list_push_copy(errors,
                                "full-pressure remediation advisory action missing");
  char *advisory_recheck =
      summary_string_from_report(json, "advisory_recheck_command");
  if (!advisory_recheck ||
      !strstr(advisory_recheck, "NYTRIX_AFL_RAW=1") ||
      !strstr(advisory_recheck, "timeout 15s") ||
      !strstr(advisory_recheck, "ny-core-normalize.sh") ||
      !strstr(advisory_recheck, "/hangs/"))
    (void)string_list_push_copy(errors,
                                "full-pressure remediation advisory recheck command missing");
  else {
    char nytrix_root[4096] = {0};
    char absolute_cache[4096] = {0};
    if (find_nytrix_root(nytrix_root, sizeof(nytrix_root)) &&
        path_join(absolute_cache, sizeof(absolute_cache), nytrix_root,
                  "build/cache/scratch") &&
        strstr(advisory_recheck, absolute_cache))
      (void)string_list_push_copy(
          errors,
          "full-pressure remediation advisory recheck used absolute cache paths");
  }
  char *advisory_recheck_state =
      summary_string_from_report(json, "advisory_recheck_state");
  double advisory_recheck_checked = -1.0, advisory_recheck_passed = -1.0;
  double advisory_recheck_timeouts = -1.0, advisory_recheck_unexpected = -1.0;
  if (!advisory_recheck_state ||
      strcmp(advisory_recheck_state, "passed") != 0 ||
      !summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_checked",
                                  &advisory_recheck_checked) ||
      advisory_recheck_checked != 2.0 ||
      !summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_passed",
                                  &advisory_recheck_passed) ||
      advisory_recheck_passed != 2.0 ||
      !summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_timeouts",
                                  &advisory_recheck_timeouts) ||
      advisory_recheck_timeouts != 0.0 ||
      !summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_unexpected",
                                  &advisory_recheck_unexpected) ||
      advisory_recheck_unexpected != 0.0)
    (void)string_list_push_copy(
        errors,
        "full-pressure remediation advisory recheck proof counters wrong");
  double effective_advisory_timeouts = -1.0;
  double advisory_effective_timeouts = -1.0;
  double advisory_penalty = -1.0;
  if (!summary_number_from_report(json, "effective_advisory_timeouts",
                                  &effective_advisory_timeouts) ||
      effective_advisory_timeouts != 0.0 ||
      !summary_number_from_report(json, "advisory_effective_timeouts",
                                  &advisory_effective_timeouts) ||
      advisory_effective_timeouts != 0.0 ||
      !summary_number_from_report(json, "advisory_penalty",
                                  &advisory_penalty) ||
      advisory_penalty != 0.0)
    (void)string_list_push_copy(
        errors,
        "full-pressure remediation effective advisory penalty was not cleared");
  char *advisory_penalty_state =
      summary_string_from_report(json, "advisory_penalty_state");
  if (!advisory_penalty_state ||
      strcmp(advisory_penalty_state, "clear") != 0)
    (void)string_list_push_copy(
        errors,
        "full-pressure remediation advisory penalty state was not cleared");
  free(advisory_state);
  free(advisory_action);
  free(advisory_recheck);
  free(advisory_recheck_state);
  free(advisory_penalty_state);
  selftest_expect_top_alias_number(
      json, "current_advisory_timeouts", "current_advisory_timeouts", errors);
  selftest_expect_top_alias_number(
      json, "historical_non_reproducing_afl_timeouts",
      "historical_non_reproducing_afl_timeouts", errors);
  if (count_sub(json, strlen(json), "\"historical_advisory_timeouts\"") != 2)
    (void)string_list_push_copy(errors,
                                "full-pressure remediation advisory aliases duplicated incorrectly");
  if (count_sub(json, strlen(json), "\"advisory_action_command\"") != 2)
    (void)string_list_push_copy(errors,
                                "full-pressure remediation advisory action duplicated incorrectly");
  if (count_sub(json, strlen(json), "\"advisory_recheck_command\"") != 2)
    (void)string_list_push_copy(errors,
                                "full-pressure remediation advisory recheck duplicated incorrectly");
  if (strstr(json, "\"category\":\"latest-full-pressure\"") ||
      strstr(json, "\"category\":\"latest-report\"") ||
      strstr(json, "\"category\":\"active-worklist\""))
    (void)string_list_push_copy(errors, "full-pressure remediation emitted blocker rows for a demoted AFL timeout");
  if (!strstr(json, "\"next_script\"") ||
      !strstr(json, "run-next.sh"))
    (void)string_list_push_copy(errors, "full-pressure remediation next script is missing");
  if (!strstr(json, "\"active_failure_detail_count\":0") ||
      !strstr(json, "\"active_saved_hangs\":0") ||
      !strstr(json, "\"active_saved_crashes\":0") ||
      !strstr(json, "\"active_saved_inputs\":0") ||
      !strstr(json, "\"active_repro_commands\":0") ||
      !strstr(json, "\"active_raw_repro_commands\":0") ||
      !strstr(json, "\"active_repro_ready\":0"))
    (void)string_list_push_copy(errors, "full-pressure remediation status should have no active failure evidence counters");
  char *active_primary = summary_string_from_report(json, "active_primary_command");
  if (!active_primary || active_primary[0] != '\0')
    (void)string_list_push_copy(errors, "full-pressure remediation should not expose an active primary command after demotion");
  free(active_primary);
  char *active_raw = summary_string_from_report(json, "active_raw_repro_command");
  if (!active_raw || active_raw[0] != '\0')
    (void)string_list_push_copy(errors, "full-pressure remediation should not expose an active raw replay command after demotion");
  free(active_raw);
  char *status_markdown = summary_string_from_report(json, "markdown");
  char status_markdown_abs[4096] = {0};
  if (status_markdown && *status_markdown) {
    if (path_is_absolute(status_markdown)) {
      snprintf(status_markdown_abs, sizeof(status_markdown_abs), "%s",
               status_markdown);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(status_markdown_abs, sizeof(status_markdown_abs),
                        root, status_markdown);
    }
  }
  file_buf_t status_md = {0};
  if (!status_markdown_abs[0] ||
      !read_file(status_markdown_abs, &status_md) || !status_md.data) {
    (void)string_list_push_copy(errors,
                                "full-pressure remediation status markdown was not readable");
  } else if (!strstr(status_md.data, "Advisory action:") ||
             !strstr(status_md.data, "Advisory recheck:") ||
             !strstr(status_md.data, "Full-pressure gate:") ||
             !strstr(status_md.data, "effective clean `true`") ||
             !strstr(status_md.data, "raw ok `false`") ||
             !strstr(status_md.data,
                     "reason `demoted-non-reproducing-afl-timeout`") ||
             !strstr(status_md.data, "failures 1") ||
             !strstr(status_md.data, "Advisory:") ||
             !strstr(status_md.data, "current-timeouts") ||
             !strstr(status_md.data, "effective clear/0") ||
             !strstr(status_md.data, "current timeouts 1") ||
             !strstr(status_md.data, "penalty 0.00") ||
             !strstr(status_md.data, "raw replay 2/2 passed") ||
             !strstr(status_md.data, "timeouts 0") ||
             !strstr(status_md.data, "unexpected 0") ||
             !strstr(status_md.data, "NYTRIX_AFL_RAW=1") ||
             !strstr(status_md.data, "ny-core-normalize.sh") ||
             !strstr(status_md.data, "--include-history") ||
             !strstr(status_md.data, "worklist-history.json") ||
             !strstr(status_md.data, "worklist-history.md")) {
    (void)string_list_push_copy(errors,
                                "full-pressure remediation status markdown omits advisory action");
  }
  free(status_md.data);
  free(status_markdown);
  char *worklist_path = summary_string_from_report(json, "worklist_report");
  char worklist_abs[4096] = {0};
  if (worklist_path && *worklist_path) {
    if (path_is_absolute(worklist_path)) {
      snprintf(worklist_abs, sizeof(worklist_abs), "%s", worklist_path);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(worklist_abs, sizeof(worklist_abs), root, worklist_path);
    }
  }
  file_buf_t worklist = {0};
  if (!worklist_abs[0] || !read_file(worklist_abs, &worklist) || !worklist.data) {
    (void)string_list_push_copy(errors, "full-pressure remediation worklist was not readable");
  } else {
    if (!strstr(worklist.data, "\"active_items\":0") ||
        !strstr(worklist.data, "\"historical_items\":0") ||
        !strstr(worklist.data, "\"historical_attention_reports\":1") ||
        !strstr(worklist.data, "\"active_failure_detail_count\":0") ||
        !strstr(worklist.data, "\"active_saved_hangs\":0") ||
        !strstr(worklist.data, "\"active_saved_crashes\":0") ||
        !strstr(worklist.data, "\"active_saved_inputs\":0") ||
        !strstr(worklist.data, "\"active_repro_commands\":0") ||
        !strstr(worklist.data, "\"active_raw_repro_commands\":0") ||
        !strstr(worklist.data, "\"active_repro_ready\":0") ||
        !strstr(worklist.data, "\"include_history\":false") ||
        !strstr(worklist.data, "\"active_clear\":true") ||
        !strstr(worklist.data, "latest-smoke.json"))
      (void)string_list_push_copy(errors, "full-pressure remediation default worklist should stay active-only");
    if (strstr(worklist.data, "\"kind\":\"fuzz-all-workitem\"") ||
        strstr(worklist.data, "\"saved_inputs\"") ||
        strstr(worklist.data, "\"primary_command\"") ||
        strstr(worklist.data, "\"raw_repro_commands\""))
      (void)string_list_push_copy(errors, "full-pressure remediation default worklist leaked historical advisory evidence");
    if (strstr(worklist.data, "README.txt"))
      (void)string_list_push_copy(errors, "full-pressure remediation worklist treated AFL metadata as a saved input");

    char *markdown = summary_string_from_report(worklist.data, "markdown");
    char markdown_abs[4096] = {0};
    if (markdown && *markdown) {
      if (path_is_absolute(markdown)) {
        snprintf(markdown_abs, sizeof(markdown_abs), "%s", markdown);
      } else {
        char root[4096];
        if (find_nytrix_root(root, sizeof(root)))
          (void)path_join(markdown_abs, sizeof(markdown_abs), root, markdown);
      }
    }
    file_buf_t md = {0};
    if (!markdown_abs[0] || !read_file(markdown_abs, &md) || !md.data) {
      (void)string_list_push_copy(errors, "full-pressure remediation worklist markdown was not readable");
    } else if (!strstr(md.data, "# Nytrix Active Worklist") ||
               !strstr(md.data, "Active items: 0; historical emitted: 0; hidden attention: 1") ||
               !strstr(md.data, "Current latest report has no open worklist items.") ||
               !strstr(md.data, "No active work items.") ||
               !strstr(md.data, "## Refresh") ||
               !strstr(md.data, "--include-history") ||
               !strstr(md.data, "worklist-history.json") ||
               !strstr(md.data, "worklist-history.md")) {
      (void)string_list_push_copy(errors, "full-pressure remediation default worklist markdown is not compact");
    } else if (strstr(md.data, "Historical Context") ||
               strstr(md.data, "## Next") ||
               strstr(md.data, "non-reproducing AFL timeout rows:") ||
               strstr(md.data, "Run:") ||
               strstr(md.data, "Active failure evidence:") ||
               strstr(md.data, "Active direct AFL replays:")) {
      (void)string_list_push_copy(errors, "full-pressure remediation default worklist markdown leaked historical evidence");
    } else if (strstr(md.data, "build/fuzz/all/history.json") ||
               !strstr(md.data, "/fuzz_full_pressure_remediation/history.json") ||
               !strstr(md.data, "/fuzz_full_pressure_remediation/worklist.json") ||
               !strstr(md.data, "/fuzz_full_pressure_remediation/worklist.md")) {
      (void)string_list_push_copy(errors, "full-pressure remediation worklist markdown refresh commands do not preserve custom paths");
    } else if (strstr(md.data, "README.txt")) {
      (void)string_list_push_copy(errors, "full-pressure remediation markdown treated AFL metadata as a saved input");
    }
    free(md.data);
    free(markdown);

    char *generated_history_json =
        path_with_suffix_ext(worklist_abs, "-history", ".json");
    char *generated_history_md =
        path_with_suffix_ext(worklist_abs, "-history", ".md");
    file_buf_t generated_hist = {0};
    if (!generated_history_json ||
        !read_file(generated_history_json, &generated_hist) ||
        !generated_hist.data) {
      (void)string_list_push_copy(errors,
                                  "full-pressure remediation generated historical worklist was not readable");
    } else if (!strstr(generated_hist.data, "\"include_history\":true") ||
               !strstr(generated_hist.data, "\"historical_items\":1") ||
               !strstr(generated_hist.data, "\"historical_attention_reports\":1") ||
               !strstr(generated_hist.data, "\"saved_hangs\":2") ||
               !strstr(generated_hist.data, "\"raw_repro_command_count\":2")) {
      (void)string_list_push_copy(errors,
                                  "full-pressure remediation generated historical worklist lost evidence");
    }
    file_buf_t generated_hist_md = {0};
    if (!generated_history_md ||
        !read_file(generated_history_md, &generated_hist_md) ||
        !generated_hist_md.data) {
      (void)string_list_push_copy(errors,
                                  "full-pressure remediation generated historical markdown was not readable");
    } else if (!strstr(generated_hist_md.data, "# Nytrix Historical Worklist") ||
               !strstr(generated_hist_md.data, "## Active Snapshot") ||
               !strstr(generated_hist_md.data,
                       "No active work items in latest report.") ||
               !strstr(generated_hist_md.data, "Historical Context") ||
               !strstr(generated_hist_md.data,
                       "demoted timeout: raw replay 2/2 passed, hangs 2, inputs 2, raw timeouts 0, unexpected 0") ||
               !strstr(generated_hist_md.data, "Recheck:") ||
               !strstr(generated_hist_md.data,
                       "Recheck: `cd . && env NYTRIX_AFL_RAW=1") ||
               !strstr(generated_hist_md.data, "NYTRIX_AFL_RAW=1") ||
               !strstr(generated_hist_md.data, "ny-core-normalize.sh") ||
               !strstr(generated_hist_md.data,
                       "Historical non-reproducing AFL timeout rows: 1 verified by raw replay") ||
               !strstr(generated_hist_md.data, "## Refresh") ||
               !strstr(generated_hist_md.data, "--include-history") ||
               !strstr(generated_hist_md.data, "worklist-history.json") ||
               !strstr(generated_hist_md.data, "worklist-history.md")) {
      (void)string_list_push_copy(errors,
                                  "full-pressure remediation generated historical markdown lost drill-down");
    } else if (strstr(generated_hist_md.data, "## Next")) {
      (void)string_list_push_copy(errors,
                                  "full-pressure remediation generated historical markdown used Next for refresh commands");
    }
    free(generated_hist.data);
    free(generated_hist_md.data);
    free(generated_history_json);
    free(generated_history_md);

    char *history_report = summary_string_from_report(worklist.data,
                                                      "history_report");
    char *history_json = NULL, *history_md = NULL;
    bool history_paths_ok =
        asprintf(&history_json, "%s.include-history.json", worklist_abs) >= 0 &&
        asprintf(&history_md, "%s.include-history.md", worklist_abs) >= 0;
    char repo_root[4096] = {0};
    bool repo_ok = find_nytrix_root(repo_root, sizeof(repo_root));
    if (!history_paths_ok || !history_report || !*history_report || !repo_ok) {
      (void)string_list_push_copy(errors, "full-pressure remediation include-history worklist setup failed");
    } else {
      char *cmd_argv[] = {
        "./build/nytrix", "fuzz", "all", "worklist",
        "--history", history_report,
        "--include-history",
        "--json", history_json,
        "--markdown", history_md,
        NULL
      };
      proc_result_t hist_pr = run_proc(cmd_argv, repo_root, 20.0);
      if (hist_pr.rc != 0)
        (void)string_list_push_copy(errors, "full-pressure remediation include-history worklist command failed");
      proc_result_free(&hist_pr);
    }
    file_buf_t hist = {0};
    if (!history_json || !read_file(history_json, &hist) || !hist.data) {
      (void)string_list_push_copy(errors, "full-pressure remediation include-history worklist was not readable");
    } else {
      if (!strstr(hist.data, "\"active_items\":0") ||
          !strstr(hist.data, "\"historical_items\":1") ||
          !strstr(hist.data, "\"failure_detail_count\":2") ||
          !strstr(hist.data, "\"saved_hangs\":2") ||
          !strstr(hist.data, "\"saved_crashes\":0") ||
          !strstr(hist.data, "\"active_failure_detail_count\":0") ||
          !strstr(hist.data, "\"active_saved_hangs\":0") ||
          !strstr(hist.data, "\"active_saved_crashes\":0") ||
          !strstr(hist.data, "\"active_saved_inputs\":0") ||
          !strstr(hist.data, "\"active_repro_commands\":0") ||
          !strstr(hist.data, "\"active_raw_repro_commands\":0") ||
          !strstr(hist.data, "\"active_repro_ready\":0") ||
          !strstr(hist.data, "\"saved_input_count\":2") ||
          !strstr(hist.data, "\"repro_command_count\":2") ||
          !strstr(hist.data, "\"raw_repro_command_count\":2") ||
          !strstr(hist.data, "\"saved_input_files\":2") ||
          !strstr(hist.data, "\"repro_wrapper_executables\":1") ||
          !strstr(hist.data, "\"repro_ready\":true") ||
          !strstr(hist.data, "\"raw_repro_checked\":2") ||
          !strstr(hist.data, "\"raw_repro_passed\":2") ||
          !strstr(hist.data, "\"raw_repro_timeouts\":0") ||
          !strstr(hist.data, "\"raw_repro_unexpected\":0") ||
          !strstr(hist.data, "\"non_reproducing_afl_timeout\":true") ||
          !strstr(hist.data, "\"active\":false") ||
          !strstr(hist.data, "\"severity\":\"historical\"") ||
          !strstr(hist.data, "\"first_failed_report\"") ||
          !strstr(hist.data, "\"first_command_log\"") ||
          !strstr(hist.data, "\"first_afl_output_dir\"") ||
          !strstr(hist.data, "\"first_saved_input\"") ||
          !strstr(hist.data, "\"first_repro_wrapper\"") ||
          !strstr(hist.data, "\"repro_command\"") ||
          !strstr(hist.data, "\"raw_repro_command\"") ||
          !strstr(hist.data, "\"first_raw_repro_command\"") ||
          !strstr(hist.data, "NYTRIX_AFL_RAW=1") ||
          !strstr(hist.data, "&& timeout 15s") ||
            !strstr(hist.data, "build/cache/scratch/nytrix_selftest_native_") ||
          !strstr(hist.data, "ny-core-normalize.sh") ||
          !strstr(hist.data, "\"failed_reports\"") ||
          !strstr(hist.data, "\"afl_output_dirs\"") ||
          !strstr(hist.data, "\"saved_inputs\"") ||
          !strstr(hist.data, "\"repro_wrappers\"") ||
          !strstr(hist.data, "\"repro_commands\"") ||
          !strstr(hist.data, "\"raw_repro_commands\""))
        (void)string_list_push_copy(errors, "full-pressure remediation include-history worklist does not expose advisory failure evidence");
      if (strstr(hist.data, "README.txt"))
        (void)string_list_push_copy(errors, "full-pressure remediation include-history worklist treated AFL metadata as a saved input");
      if (strstr(hist.data, "/home/e/nytrix/build/cache/projects/test") ||
          strstr(hist.data, "/home/e/nytrix/fuzz/"))
        (void)string_list_push_copy(errors, "full-pressure remediation include-history worklist used old Nytrix paths");
      if (repo_ok && strstr(hist.data, repo_root))
        (void)string_list_push_copy(errors, "full-pressure remediation include-history worklist leaked absolute repo path");
      if (!strstr(hist.data, "\"primary_command\":\"cd . && timeout 15s") ||
          !strstr(hist.data, "&& timeout 15s") ||
          !strstr(hist.data, "ny-core-normalize.sh"))
        (void)string_list_push_copy(errors, "full-pressure remediation worklist did not promote direct AFL replay to primary command");
      char *raw_repro = NULL;
      const char *raw_field = strstr(hist.data, "\"first_raw_repro_command\"");
      if (raw_field)
        raw_repro = json_extract_string_range(raw_field,
                                              hist.data + strlen(hist.data),
                                              "first_raw_repro_command");
      if (!raw_repro || !strstr(raw_repro, "NYTRIX_AFL_RAW=1") ||
          !strstr(raw_repro, "timeout 15s")) {
        (void)string_list_push_copy(errors, "full-pressure remediation raw replay command is missing");
      } else {
        char nytrix_root[4096] = {0};
        char absolute_cache[4096] = {0};
        if (find_nytrix_root(nytrix_root, sizeof(nytrix_root)) &&
            path_join(absolute_cache, sizeof(absolute_cache), nytrix_root,
                      "build/cache/scratch") &&
            strstr(raw_repro, absolute_cache))
          (void)string_list_push_copy(
              errors,
              "full-pressure remediation raw replay command used absolute cache paths");
        char *cmd_argv[] = {"sh", "-lc", raw_repro, NULL};
        proc_result_t raw = run_proc(cmd_argv, repo_root, 20.0);
        if (raw.rc != 0 && raw.rc != 1 && raw.rc != 2)
          (void)string_list_push_copy(errors, "full-pressure remediation raw replay command failed from repo cwd");
        proc_result_free(&raw);
      }
      free(raw_repro);
    }
    file_buf_t hist_markdown = {0};
    if (!history_md || !read_file(history_md, &hist_markdown) ||
        !hist_markdown.data) {
      (void)string_list_push_copy(errors, "full-pressure remediation include-history markdown was not readable");
    } else if (!strstr(hist_markdown.data, "# Nytrix Historical Worklist") ||
               !strstr(hist_markdown.data, "Active items: 0; historical emitted: 1; hidden attention: 1") ||
               !strstr(hist_markdown.data, "## Active Snapshot") ||
               !strstr(hist_markdown.data,
                       "No active work items in latest report.") ||
               !strstr(hist_markdown.data, "Historical non-reproducing AFL timeout rows: 1 verified by raw replay") ||
               !strstr(hist_markdown.data,
                       "demoted timeout: raw replay 2/2 passed, hangs 2, inputs 2, raw timeouts 0, unexpected 0") ||
               !strstr(hist_markdown.data, "Historical Context") ||
               !strstr(hist_markdown.data, "## Refresh") ||
               !strstr(hist_markdown.data, "--include-history") ||
               !strstr(hist_markdown.data, "worklist.json.include-history.json") ||
               !strstr(hist_markdown.data, "worklist.json.include-history.md")) {
      (void)string_list_push_copy(errors, "full-pressure remediation include-history markdown omits advisory demotion evidence");
    } else if (strstr(hist_markdown.data, "Active failure evidence:") ||
               strstr(hist_markdown.data, "Active direct AFL replays:") ||
               strstr(hist_markdown.data, "Run:") ||
               strstr(hist_markdown.data, "## Next")) {
      (void)string_list_push_copy(errors, "full-pressure remediation include-history markdown still treats demoted timeout as active");
    } else if (strstr(hist_markdown.data, "README.txt")) {
      (void)string_list_push_copy(errors, "full-pressure remediation include-history markdown treated AFL metadata as a saved input");
    } else if (repo_ok && strstr(hist_markdown.data, repo_root)) {
      (void)string_list_push_copy(errors, "full-pressure remediation include-history markdown leaked absolute repo path");
    }
    free(hist_markdown.data);
    free(hist.data);
    free(history_report);
    free(history_json);
    free(history_md);
  }
  free(worklist.data);
  free(worklist_path);
  free(mode);
}

static void selftest_validate_fuzz_repro_ready_missing_wrapper(const char *json,
                                                               string_list_t *errors,
                                                               int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-worklist") != 0)
    (void)string_list_push_copy(errors, "missing-wrapper replay fixture mode mismatch");
  if (*row_count != 1)
    (void)string_list_push_copy(errors, "missing-wrapper replay fixture row count mismatch");
  double active_items = -1.0, active_saved_inputs = -1.0;
  double active_repro_commands = -1.0, active_raw_repro_commands = -1.0;
  double active_repro_ready = -1.0;
  if (!summary_number_from_report(json, "active_items", &active_items) ||
      active_items != 1.0)
    (void)string_list_push_copy(errors, "missing-wrapper fixture active item count mismatch");
  if (!summary_number_from_report(json, "active_saved_inputs",
                                  &active_saved_inputs) ||
      active_saved_inputs != 2.0)
    (void)string_list_push_copy(errors, "missing-wrapper fixture saved-input count mismatch");
  if (!summary_number_from_report(json, "active_repro_commands",
                                  &active_repro_commands) ||
      active_repro_commands != 2.0)
    (void)string_list_push_copy(errors, "missing-wrapper fixture replay command count mismatch");
  if (!summary_number_from_report(json, "active_raw_repro_commands",
                                  &active_raw_repro_commands) ||
      active_raw_repro_commands != 2.0)
    (void)string_list_push_copy(errors, "missing-wrapper fixture raw replay command count mismatch");
  if (!summary_number_from_report(json, "active_repro_ready",
                                  &active_repro_ready) ||
      active_repro_ready != 0.0)
    (void)string_list_push_copy(errors, "missing-wrapper fixture incorrectly reported replay readiness");
  if (!strstr(json, "\"saved_input_count\":2") ||
      !strstr(json, "\"repro_command_count\":2") ||
      !strstr(json, "\"raw_repro_command_count\":2") ||
      !strstr(json, "\"saved_input_files\":2") ||
      !strstr(json, "\"repro_wrapper_files\":1") ||
      !strstr(json, "\"repro_wrapper_executables\":1") ||
      !strstr(json, "\"repro_ready\":false") ||
      !strstr(json, "ny-core-normalize.sh") ||
      !strstr(json, "missing-wrapper-selftest-normalize.sh"))
    (void)string_list_push_copy(errors, "missing-wrapper fixture did not expose expected replay evidence");
  if (strstr(json, "\"repro_ready\":true"))
    (void)string_list_push_copy(errors, "missing-wrapper fixture leaked ready=true");

  char *markdown = summary_string_from_report(json, "markdown");
  char markdown_abs[4096] = {0};
  if (markdown && *markdown) {
    if (path_is_absolute(markdown)) {
      snprintf(markdown_abs, sizeof(markdown_abs), "%s", markdown);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(markdown_abs, sizeof(markdown_abs), root, markdown);
    }
  }
  file_buf_t md = {0};
  if (!markdown_abs[0] || !read_file(markdown_abs, &md) || !md.data) {
    (void)string_list_push_copy(errors, "missing-wrapper fixture markdown was not readable");
  } else if (!strstr(md.data, "Active direct AFL replays: 2 saved inputs, 2 replay commands, 2 raw commands, 0 ready") ||
             !strstr(md.data, "replay_ready `false`") ||
             !strstr(md.data, "input_files 2") ||
             !strstr(md.data, "executable_wrappers 1")) {
    (void)string_list_push_copy(errors, "missing-wrapper fixture markdown hid replay readiness state");
  }
  free(md.data);
  free(markdown);
  free(mode);
}

static void selftest_validate_fuzz_repro_ready_missing_command(const char *json,
                                                               string_list_t *errors,
                                                               int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-worklist") != 0)
    (void)string_list_push_copy(errors, "missing-command replay fixture mode mismatch");
  if (*row_count != 1)
    (void)string_list_push_copy(errors, "missing-command replay fixture row count mismatch");
  double active_items = -1.0, active_saved_inputs = -1.0;
  double active_repro_commands = -1.0, active_raw_repro_commands = -1.0;
  double active_repro_ready = -1.0;
  if (!summary_number_from_report(json, "active_items", &active_items) ||
      active_items != 1.0)
    (void)string_list_push_copy(errors, "missing-command fixture active item count mismatch");
  if (!summary_number_from_report(json, "active_saved_inputs",
                                  &active_saved_inputs) ||
      active_saved_inputs != 2.0)
    (void)string_list_push_copy(errors, "missing-command fixture saved-input count mismatch");
  if (!summary_number_from_report(json, "active_repro_commands",
                                  &active_repro_commands) ||
      active_repro_commands != 1.0)
    (void)string_list_push_copy(errors, "missing-command fixture replay command count mismatch");
  if (!summary_number_from_report(json, "active_raw_repro_commands",
                                  &active_raw_repro_commands) ||
      active_raw_repro_commands != 1.0)
    (void)string_list_push_copy(errors, "missing-command fixture raw replay command count mismatch");
  if (!summary_number_from_report(json, "active_repro_ready",
                                  &active_repro_ready) ||
      active_repro_ready != 0.0)
    (void)string_list_push_copy(errors, "missing-command fixture incorrectly reported replay readiness");
  if (!strstr(json, "\"saved_input_count\":2") ||
      !strstr(json, "\"repro_command_count\":1") ||
      !strstr(json, "\"raw_repro_command_count\":1") ||
      !strstr(json, "\"saved_input_files\":2") ||
      !strstr(json, "\"repro_wrapper_files\":1") ||
      !strstr(json, "\"repro_wrapper_executables\":1") ||
      !strstr(json, "\"repro_ready\":false") ||
      !strstr(json, "ny-core-normalize.sh") ||
      !strstr(json, "unsafe/target") ||
      strstr(json, "unsafe/target-normalize.sh"))
    (void)string_list_push_copy(errors, "missing-command fixture did not expose expected replay evidence");
  if (strstr(json, "\"repro_ready\":true"))
    (void)string_list_push_copy(errors, "missing-command fixture leaked ready=true");

  char *markdown = summary_string_from_report(json, "markdown");
  char markdown_abs[4096] = {0};
  if (markdown && *markdown) {
    if (path_is_absolute(markdown)) {
      snprintf(markdown_abs, sizeof(markdown_abs), "%s", markdown);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(markdown_abs, sizeof(markdown_abs), root, markdown);
    }
  }
  file_buf_t md = {0};
  if (!markdown_abs[0] || !read_file(markdown_abs, &md) || !md.data) {
    (void)string_list_push_copy(errors, "missing-command fixture markdown was not readable");
  } else if (!strstr(md.data, "Active direct AFL replays: 2 saved inputs, 1 replay commands, 1 raw commands, 0 ready") ||
             !strstr(md.data, "replay_ready `false`") ||
             !strstr(md.data, "input_files 2") ||
             !strstr(md.data, "executable_wrappers 1")) {
    (void)string_list_push_copy(errors, "missing-command fixture markdown hid replay readiness state");
  }
  free(md.data);
  free(markdown);
  free(mode);
}

static void selftest_validate_perf_triage_args(const char *json,
                                               string_list_t *errors,
                                               int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "perf-triage") != 0)
    (void)string_list_push_copy(errors, "perf triage mode mismatch");
  selftest_expect_report_result_top_aliases(json, errors);
  double candidates = -1.0, emitted = -1.0, runs = -1.0, warmup = -1.0;
  double samples = -1.0, threshold = -1.0, bench_limit = -1.0;
  double bench_timeout_s = -1.0;
  double hotspots = -1.0, perf_hotspots = -1.0;
  double max_ratio = -1.0, perf_max_ratio = -1.0;
  double perf_worst_ratio = -1.0, perf_slowdown = -999.0;
  double perf_threshold = -1.0;
  if (!summary_number_from_report(json, "candidates", &candidates) ||
      candidates != 1.0)
    (void)string_list_push_copy(errors, "perf triage equals-form limit did not bound candidates");
  if (!summary_number_from_report(json, "emitted", &emitted) || emitted != 1.0)
    (void)string_list_push_copy(errors, "perf triage emitted count mismatch");
  if (!summary_number_from_report(json, "runs", &runs) || runs != 1.0)
    (void)string_list_push_copy(errors, "perf triage equals-form runs were ignored");
  if (!summary_number_from_report(json, "warmup", &warmup) || warmup != 0.0)
    (void)string_list_push_copy(errors, "perf triage equals-form warmup was ignored");
  if (!summary_number_from_report(json, "measurement_samples", &samples) ||
      samples != 1.0)
    (void)string_list_push_copy(errors, "perf triage sample count mismatch");
  if (!summary_number_from_report(json, "threshold_ratio", &threshold) ||
      threshold != 999.0)
    (void)string_list_push_copy(errors, "perf triage equals-form threshold was ignored");
  if (!summary_number_from_report(json, "perf_threshold_ratio",
                                  &perf_threshold) ||
      perf_threshold != threshold)
    (void)string_list_push_copy(errors, "perf triage threshold alias wrong");
  if (!summary_number_from_report(json, "hotspots", &hotspots) ||
      !summary_number_from_report(json, "perf_hotspots", &perf_hotspots) ||
      perf_hotspots != hotspots)
    (void)string_list_push_copy(errors, "perf triage hotspot alias wrong");
  if (!summary_number_from_report(json, "max_ratio", &max_ratio) ||
      !summary_number_from_report(json, "perf_max_ratio", &perf_max_ratio) ||
      !summary_number_from_report(json, "perf_worst_ratio",
                                  &perf_worst_ratio) ||
      perf_max_ratio != max_ratio || perf_worst_ratio != max_ratio)
    (void)string_list_push_copy(errors, "perf triage max ratio aliases wrong");
  if (!summary_number_from_report(json,
                                  "perf_worst_slowdown_percent",
                                  &perf_slowdown)) {
    (void)string_list_push_copy(errors, "perf triage slowdown alias missing");
  } else {
    double expected_slowdown = (perf_worst_ratio - 1.0) * 100.0;
    double slowdown_delta = perf_slowdown - expected_slowdown;
    if (slowdown_delta < 0.0) slowdown_delta = -slowdown_delta;
    if (slowdown_delta > 0.05)
      (void)string_list_push_copy(errors, "perf triage slowdown alias wrong");
  }
  if (!strstr(json, "\"c_elapsed_ms\":") ||
      !strstr(json, "\"ny_elapsed_ms\":") ||
      !strstr(json, "\"delta_elapsed_ns\":") ||
      !strstr(json, "\"delta_elapsed_ms\":") ||
      !strstr(json, "\"slowdown_percent\":") ||
      !strstr(json, "\"hotspot\":"))
    (void)string_list_push_copy(errors, "perf triage row timing aliases missing");
  char *max_case = summary_string_from_report(json, "max_case");
  char *perf_max_case = summary_string_from_report(json, "perf_max_case");
  char *perf_worst_case = summary_string_from_report(json, "perf_worst_case");
  if (!max_case || !perf_max_case || !perf_worst_case ||
      strcmp(max_case, perf_max_case) != 0 ||
      strcmp(max_case, perf_worst_case) != 0)
    (void)string_list_push_copy(errors, "perf triage max case aliases wrong");
  if (!summary_number_from_report(json, "bench_limit", &bench_limit) ||
      bench_limit != 1.0)
    (void)string_list_push_copy(errors, "perf triage bench limit was not recorded");
  if (!summary_number_from_report(json, "bench_timeout_s",
                                  &bench_timeout_s) ||
      bench_timeout_s < 60.0)
    (void)string_list_push_copy(errors, "perf triage bench wall timeout was not recorded");
  selftest_expect_top_alias_number(json, "candidates", "candidates", errors);
  selftest_expect_top_alias_number(json, "emitted", "emitted", errors);
  selftest_expect_top_alias_number(json, "runs", "runs", errors);
  selftest_expect_top_alias_number(json, "warmup", "warmup", errors);
  selftest_expect_top_alias_number(json, "measurement_samples",
                                   "measurement_samples", errors);
  selftest_expect_top_alias_number(json, "hotspots", "hotspots", errors);
  selftest_expect_top_alias_number(json, "perf_hotspots",
                                   "perf_hotspots", errors);
  selftest_expect_top_alias_number(json, "max_ratio", "max_ratio", errors);
  selftest_expect_top_alias_number(json, "perf_max_ratio",
                                   "perf_max_ratio", errors);
  selftest_expect_top_alias_number(json, "perf_worst_ratio",
                                   "perf_worst_ratio", errors);
  selftest_expect_top_alias_number(json, "perf_worst_slowdown_percent",
                                   "perf_worst_slowdown_percent", errors);
  selftest_expect_top_alias_number(json, "threshold_ratio",
                                   "threshold_ratio", errors);
  selftest_expect_top_alias_number(json, "perf_threshold_ratio",
                                   "perf_threshold_ratio", errors);
  selftest_expect_top_alias_number(json, "bench_limit", "bench_limit",
                                   errors);
  selftest_expect_top_alias_number(json, "bench_timeout_s",
                                   "bench_timeout_s", errors);
  selftest_expect_top_alias_string(json, "max_case", errors);
  selftest_expect_top_alias_string(json, "perf_max_case", errors);
  selftest_expect_top_alias_string(json, "perf_worst_case", errors);
  selftest_expect_top_alias_string(json, "scratch_root", errors);
  selftest_expect_top_alias_string(json, "findings_dir", errors);
  selftest_expect_top_alias_string(json, "markdown", errors);
  char *findings_dir = summary_string_from_report(json, "findings_dir");
  if (!findings_dir || !strstr(findings_dir, "build/cache/scratch/perf_triage_findings/") ||
      strstr(findings_dir, "etc/assets/dict/fuzz/work"))
    (void)string_list_push_copy(errors, "perf triage default findings dir escaped build/cache/scratch");
  if (findings_dir && path_is_absolute(findings_dir))
    (void)string_list_push_copy(errors, "perf triage findings dir was absolute");
  char *scratch_root = summary_string_from_report(json, "scratch_root");
  if (!scratch_root || strcmp(scratch_root, "build/cache/scratch") != 0)
    (void)string_list_push_copy(errors, "perf triage scratch root was not repo-relative");
  char nytrix_root[4096] = {0};
  bool have_nytrix_root = find_nytrix_root(nytrix_root, sizeof(nytrix_root));
  if (have_nytrix_root && nytrix_root[0] && strstr(json, nytrix_root))
    (void)string_list_push_copy(errors, "perf triage report leaked absolute nytrix paths");
  char *markdown = summary_string_from_report(json, "markdown");
  if (!markdown || !strstr(markdown, "perf-triage.md")) {
    (void)string_list_push_copy(errors, "perf triage markdown path missing");
  } else {
    char markdown_abs[4096] = {0};
    if (path_is_absolute(markdown)) {
      snprintf(markdown_abs, sizeof(markdown_abs), "%s", markdown);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(markdown_abs, sizeof(markdown_abs), root, markdown);
    }
    file_buf_t md = {0};
    if (!markdown_abs[0] || !read_file(markdown_abs, &md) || !md.data) {
      (void)string_list_push_copy(errors, "perf triage markdown was not readable");
    } else if (!strstr(md.data, "Nytrix Perf Triage") ||
               !strstr(md.data, "TLDR") ||
               !strstr(md.data, "Ranked Cases") ||
               !strstr(md.data, "threshold 999.00x") ||
               !strstr(md.data, "Refresh") ||
               !strstr(md.data, "env NYTRIX_LOW_PRIORITY=1") ||
               !strstr(md.data, "NYTRIX_RUN_NICE=10") ||
               !strstr(md.data, "nice -n 10") ||
               !strstr(md.data, "perf triage --fast") ||
               !strstr(md.data, "--markdown")) {
      (void)string_list_push_copy(errors, "perf triage markdown is incomplete");
    } else if (strstr(md.data, "+-")) {
      (void)string_list_push_copy(
          errors, "perf triage markdown rendered a malformed slowdown sign");
    } else if ((have_nytrix_root && nytrix_root[0] && strstr(md.data, nytrix_root)) ||
               strstr(md.data, "/home/e/nytrix/build/cache/projects/test") ||
               strstr(md.data, "/home/e/nytrix/fuzz")) {
      (void)string_list_push_copy(errors, "perf triage markdown leaked local absolute paths");
    }
    free(md.data);
  }
  free(markdown);
  free(scratch_root);
  free(max_case);
  free(perf_max_case);
  free(perf_worst_case);
  free(findings_dir);
  free(mode);
}

static void selftest_validate_compiler_known_bugs(const char *json,
                                                  string_list_t *errors,
                                                  int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "compiler-known-bugs") != 0)
    (void)string_list_push_copy(errors, "compiler known-bugs mode mismatch");
  selftest_expect_report_result_top_aliases(json, errors);
  double known_count = -1.0, reproduced = -1.0, fixed = -1.0;
  double lost = -1.0, baseline = -1.0;
  double alias_reproduced = -1.0, alias_fixed = -1.0;
  double alias_lost = -1.0, alias_baseline = -1.0;
  if (!summary_number_from_report(json, "known_bug_count", &known_count) ||
      known_count <= 0.0)
    (void)string_list_push_copy(errors, "compiler known-bugs count missing");
  if (!summary_number_from_report(json, "reproduced", &reproduced) ||
      !summary_number_from_report(json, "fixed_candidates", &fixed) ||
      !summary_number_from_report(json, "lost_signal", &lost) ||
      !summary_number_from_report(json, "baseline_failures", &baseline))
    (void)string_list_push_copy(errors, "compiler known-bugs old fields missing");
  if (!summary_number_from_report(json, "known_bug_reproduced",
                                  &alias_reproduced) ||
      alias_reproduced != reproduced ||
      !summary_number_from_report(json, "known_bug_fixed_candidates",
                                  &alias_fixed) ||
      alias_fixed != fixed ||
      !summary_number_from_report(json, "known_bug_lost_signal",
                                  &alias_lost) ||
      alias_lost != lost ||
      !summary_number_from_report(json, "known_bug_baseline_failures",
                                  &alias_baseline) ||
      alias_baseline != baseline)
    (void)string_list_push_copy(errors, "compiler known-bugs aliases drifted");
  selftest_expect_top_alias_number(json, "known_bug_count",
                                   "known_bug_count", errors);
  selftest_expect_top_alias_number(json, "reproduced", "reproduced",
                                   errors);
  selftest_expect_top_alias_number(json, "fixed_candidates",
                                   "fixed_candidates", errors);
  selftest_expect_top_alias_number(json, "lost_signal", "lost_signal",
                                   errors);
  selftest_expect_top_alias_number(json, "baseline_failures",
                                   "baseline_failures", errors);
  selftest_expect_top_alias_number(json, "known_bug_reproduced",
                                   "known_bug_reproduced", errors);
  selftest_expect_top_alias_number(json, "known_bug_fixed_candidates",
                                   "known_bug_fixed_candidates", errors);
  selftest_expect_top_alias_number(json, "known_bug_lost_signal",
                                   "known_bug_lost_signal", errors);
  selftest_expect_top_alias_number(json, "known_bug_baseline_failures",
                                   "known_bug_baseline_failures", errors);
  selftest_expect_top_alias_bool(json, "strict_open", errors);
  selftest_expect_top_alias_string(json, "ny_bin", errors);
  selftest_expect_top_alias_string(json, "out_dir", errors);
  if (known_count > 0.0 && reproduced + fixed + lost + baseline <= 0.0)
    (void)string_list_push_copy(errors,
                                "compiler known-bugs replay outcome missing");
  free(mode);
}

static void selftest_validate_compiler_std_audit(const char *json,
                                                 string_list_t *errors,
                                                 int *row_count) {
     selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "compiler-std-audit") != 0)
    (void)string_list_push_copy(errors, "compiler std audit mode mismatch");
  selftest_expect_top_alias_number(json, "cases", "cases", errors);
  selftest_expect_top_alias_number(json, "ok_count", "ok", errors);
  selftest_expect_top_alias_number(json, "failure_count",
                                   "failure_count", errors);
  selftest_expect_top_alias_string(json, "runtime_surface_state", errors);
  selftest_expect_top_alias_string(json, "runtime_surface_scope", errors);
  selftest_expect_top_alias_number(json, "runtime_export_coverage_percent",
                                   "runtime_export_coverage_percent", errors);
  selftest_expect_top_alias_number(json, "runtime_unreferenced_count",
                                   "runtime_unreferenced_count", errors);
  selftest_expect_top_alias_number(json, "runtime_wrapper_gap_count",
                                   "runtime_wrapper_gap_count", errors);
  selftest_expect_top_alias_string(json, "crt_surface_state", errors);
  selftest_expect_top_alias_string(json, "crt_surface_scope", errors);
  selftest_expect_top_alias_string(json, "crt_behavior_state", errors);
  selftest_expect_top_alias_string(json, "crt_behavior_scope", errors);
  selftest_expect_top_alias_string(json, "crt_behavior_next_action", errors);
  selftest_expect_top_alias_string(json, "crt_behavior_next_reason", errors);
  selftest_expect_top_alias_string(json, "crt_behavior_next_command", errors);
  selftest_expect_top_alias_number(json, "crt_export_coverage_percent",
                                   "crt_export_coverage_percent", errors);
  selftest_expect_top_alias_number(json, "crt_unreferenced_percent",
                                   "crt_unreferenced_percent", errors);
  selftest_expect_top_alias_number(json, "crt_unreferenced_count",
                                   "crt_unreferenced_count", errors);
  selftest_expect_top_alias_number(json, "crt_wrapper_gap_count",
                                   "crt_wrapper_gap_count", errors);
  selftest_expect_top_alias_number(json, "crt_unreferenced_family_count",
                                   "crt_unreferenced_family_count", errors);
  selftest_expect_top_alias_string(json, "crt_top_unreferenced_family",
                                   errors);
  selftest_expect_top_alias_number(json, "crt_top_unreferenced_family_count",
                                   "crt_top_unreferenced_family_count",
                                   errors);
  selftest_expect_top_alias_string(json, "crt_next_action", errors);
  selftest_expect_top_alias_string(json, "crt_next_unreferenced_family",
                                   errors);
  selftest_expect_top_alias_number(json, "crt_next_unreferenced_count",
                                   "crt_next_unreferenced_count", errors);
  char *state = summary_string_from_report(json, "runtime_surface_state");
  char *crt_state = summary_string_from_report(json, "crt_surface_state");
  char *runtime_scope =
      summary_string_from_report(json, "runtime_surface_scope");
  char *crt_scope = summary_string_from_report(json, "crt_surface_scope");
  char *crt_behavior_state =
      summary_string_from_report(json, "crt_behavior_state");
  char *crt_behavior_scope =
      summary_string_from_report(json, "crt_behavior_scope");
  char *crt_behavior_next_action =
      summary_string_from_report(json, "crt_behavior_next_action");
  char *crt_behavior_next_reason =
      summary_string_from_report(json, "crt_behavior_next_reason");
  char *crt_behavior_next_command =
      summary_string_from_report(json, "crt_behavior_next_command");

  double runtime_exports = -1.0, direct_refs = -1.0, coverage = -1.0;
  double unreferenced = -1.0, unreferenced_count = -1.0, gap_count = -1.0;
  double crt_exports = -1.0, crt_refs = -1.0, crt_unreferenced = -1.0;
  double crt_coverage = -1.0, crt_unreferenced_percent = -1.0;
  double crt_unreferenced_count = -1.0, crt_gaps = -1.0;
  double crt_family_count = -1.0, crt_top_family_count = -1.0;
  double simmd_exports = -1.0, simmd_refs = -1.0;
  double simmd_missing = -1.0, simmd_unknown = -1.0;
  double simd_exports = -1.0, simd_refs = -1.0, simd_missing = -1.0;
  double simd_unknown = -1.0;
  if (!summary_number_from_report(json, "runtime_exports", &runtime_exports) ||
      runtime_exports <= 0.0)
    (void)string_list_push_copy(errors, "compiler std audit runtime exports missing");
  if (!summary_number_from_report(json, "direct_runtime_refs", &direct_refs) ||
      direct_refs <= 0.0)
    (void)string_list_push_copy(errors, "compiler std audit direct refs missing");
  if (!summary_number_from_report(json, "runtime_export_coverage_percent", &coverage) ||
      coverage <= 0.0 || coverage > 100.0)
    (void)string_list_push_copy(errors, "compiler std audit runtime coverage percent invalid");
  if (!summary_number_from_report(json, "runtime_unreferenced_percent", &unreferenced) ||
      unreferenced < 0.0 || unreferenced > 100.0)
    (void)string_list_push_copy(errors, "compiler std audit unreferenced percent invalid");
  if (!summary_number_from_report(json, "runtime_unreferenced_count", &unreferenced_count) ||
      unreferenced_count < 0.0)
    (void)string_list_push_copy(errors, "compiler std audit unreferenced count missing");
  if (!summary_number_from_report(json, "runtime_wrapper_gap_count", &gap_count) ||
      gap_count != 0.0)
    (void)string_list_push_copy(errors, "compiler std audit wrapper gap count was not zero");
  const char *expected_surface_state =
      compiler_std_audit_surface_state((int)gap_count,
                                       (int)unreferenced_count);
  if (!state || strcmp(state, expected_surface_state) != 0)
    (void)string_list_push_copy(errors,
                                "compiler std audit runtime surface state wrong");
  if (!crt_state || strcmp(crt_state, state ? state : "") != 0)
    (void)string_list_push_copy(errors,
                                "compiler std audit CRT alias state mismatch");
  if (!runtime_scope ||
      strcmp(runtime_scope, NYTRIX_RUNTIME_SURFACE_SCOPE) != 0 ||
      !crt_scope || strcmp(crt_scope, NYTRIX_CRT_SURFACE_SCOPE) != 0 ||
      !crt_behavior_state ||
      strcmp(crt_behavior_state, NYTRIX_CRT_BEHAVIOR_STATE) != 0 ||
      !crt_behavior_scope ||
      strcmp(crt_behavior_scope, NYTRIX_CRT_BEHAVIOR_SCOPE) != 0)
    (void)string_list_push_copy(
        errors, "compiler std audit CRT claim scope missing");
  if (!crt_behavior_next_action ||
      strcmp(crt_behavior_next_action, NYTRIX_CRT_BEHAVIOR_NEXT_ACTION) != 0 ||
      !crt_behavior_next_reason ||
      !strstr(crt_behavior_next_reason, "campaign-gated") ||
      !crt_behavior_next_command ||
      !selftest_command_uses_env_nice(crt_behavior_next_command) ||
      !strstr(crt_behavior_next_command, "./build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(
        errors, "compiler std audit CRT behavior next action missing");
  if (!summary_number_from_report(json, "crt_runtime_exports", &crt_exports) ||
      crt_exports != runtime_exports ||
      !summary_number_from_report(json, "crt_direct_refs", &crt_refs) ||
      crt_refs != direct_refs ||
      !summary_number_from_report(json, "crt_unreferenced_exports", &crt_unreferenced) ||
      crt_unreferenced != unreferenced_count ||
      !summary_number_from_report(json, "crt_export_coverage_percent", &crt_coverage) ||
      crt_coverage != coverage ||
      !summary_number_from_report(json, "crt_unreferenced_percent",
                                  &crt_unreferenced_percent) ||
      crt_unreferenced_percent != unreferenced ||
      !summary_number_from_report(json, "crt_unreferenced_count",
                                  &crt_unreferenced_count) ||
      crt_unreferenced_count != unreferenced_count ||
      !summary_number_from_report(json, "crt_wrapper_gap_count", &crt_gaps) ||
      crt_gaps != gap_count)
    (void)string_list_push_copy(errors, "compiler std audit CRT aliases drifted");
  char *crt_top_family =
      summary_string_from_report(json, "crt_top_unreferenced_family");
  char *crt_families =
      summary_array_from_report(json, "crt_unreferenced_families");
  char *crt_next_action = summary_string_from_report(json, "crt_next_action");
  char *crt_next_reason = summary_string_from_report(json, "crt_next_reason");
  char *crt_next_family =
      summary_string_from_report(json, "crt_next_unreferenced_family");
  char *crt_next_exports =
      summary_array_from_report(json, "crt_next_unreferenced_exports");
  char *crt_next_definition_file =
      summary_string_from_report(json, "crt_next_definition_file");
  char *crt_next_definition_locations =
      summary_array_from_report(json, "crt_next_definition_locations");
  char *crt_next_inspect_command =
      summary_string_from_report(json, "crt_next_inspect_command");
  double crt_next_count = -1.0;
  if (unreferenced_count > 0.0) {
    if (!summary_number_from_report(json, "crt_unreferenced_family_count",
                                    &crt_family_count) ||
        crt_family_count <= 0.0 ||
        !summary_number_from_report(json,
                                    "crt_top_unreferenced_family_count",
                                    &crt_top_family_count) ||
        crt_top_family_count <= 0.0 ||
        !crt_top_family || !*crt_top_family ||
        !crt_families || !strstr(crt_families, "\"family\":") ||
        !strstr(crt_families, "\"rank\":1") ||
        !strstr(crt_families, "\"next\":true") ||
        !strstr(crt_families, "\"coverage_gain_percent\"") ||
        !strstr(crt_families, "\"after_unreferenced_count\"") ||
        !strstr(crt_families, "\"after_export_coverage_percent\"") ||
        !strstr(crt_families, "\"sample\"") ||
        !strstr(crt_families, "\"exports\""))
      (void)string_list_push_copy(errors,
                                  "compiler std audit CRT family summary missing");
    if (crt_top_family && *crt_top_family && crt_families) {
      char *rank1 = NULL;
      (void)asprintf(&rank1,
                     "{\"rank\":1,\"family\":\"%s\",\"count\":%.0f,\"next\":true",
                     crt_top_family, crt_top_family_count);
      if (!rank1 || !strstr(crt_families, rank1))
        (void)string_list_push_copy(
            errors,
            "compiler std audit CRT top family did not match rank-1 next");
      free(rank1);
    }
    if (!crt_next_action ||
        strcmp(crt_next_action, "cover-unreferenced-family") != 0 ||
        !crt_next_reason || !strstr(crt_next_reason, "largest") ||
        !crt_next_family || !*crt_next_family ||
        (crt_top_family && *crt_top_family &&
         strcmp(crt_next_family, crt_top_family) != 0) ||
        !summary_number_from_report(json, "crt_next_unreferenced_count",
                                    &crt_next_count) ||
        crt_next_count != crt_top_family_count ||
        !crt_next_exports || !strstr(crt_next_exports, "\"__") ||
        !strstr(crt_next_exports, "\""))
      (void)string_list_push_copy(errors,
                                  "compiler std audit CRT next action missing");
    if (crt_next_family && strcmp(crt_next_family, "trace") == 0 &&
        (!crt_next_exports || !strstr(crt_next_exports, "__trace_") ||
         !crt_next_definition_locations ||
         !strstr(crt_next_definition_locations, "rt_trace_") ||
         !crt_next_inspect_command ||
         !strstr(crt_next_inspect_command, "__trace_")))
      (void)string_list_push_copy(errors,
                                  "compiler std audit trace next exports missing");
    if (!crt_next_definition_file ||
        !strstr(crt_next_definition_file, "src/rt/defs.h") ||
        !crt_next_definition_locations ||
        !strstr(crt_next_definition_locations, "\"line\"") ||
        !strstr(crt_next_definition_locations, "\"arity\"") ||
        !strstr(crt_next_definition_locations, "\"runtime_symbol\"") ||
        !strstr(crt_next_definition_locations, "\"signature\"") ||
        !crt_next_inspect_command ||
        !strstr(crt_next_inspect_command, "sed -n") ||
        !strstr(crt_next_inspect_command, "rg -n") ||
        !strstr(crt_next_inspect_command, "../nytrix/src/rt/defs.h"))
      (void)string_list_push_copy(errors,
                                  "compiler std audit CRT next definition details missing");
  } else {
    if (!summary_number_from_report(json, "crt_next_unreferenced_count",
                                    &crt_next_count) ||
        crt_next_count != 0.0 || !crt_next_action ||
        strcmp(crt_next_action, "none") != 0)
      (void)string_list_push_copy(errors,
                                  "compiler std audit clean CRT next action wrong");
  }
  if (!summary_number_from_report(json, "simmd_runtime_exports", &simmd_exports) ||
      !summary_number_from_report(json, "simmd_refs", &simmd_refs) ||
      !summary_number_from_report(json, "simmd_missing_wrappers", &simmd_missing) ||
      !summary_number_from_report(json, "simmd_unknown_refs", &simmd_unknown) ||
      !summary_number_from_report(json, "simd_runtime_exports", &simd_exports) ||
      !summary_number_from_report(json, "simd_refs", &simd_refs) ||
      !summary_number_from_report(json, "simd_missing_wrappers", &simd_missing) ||
      !summary_number_from_report(json, "simd_unknown_refs", &simd_unknown) ||
      simd_exports != simmd_exports || simd_refs != simmd_refs ||
      simd_missing != simmd_missing || simd_unknown != simmd_unknown)
    (void)string_list_push_copy(errors, "compiler std audit SIMD aliases drifted");
  if (simmd_exports <= 0.0 || simmd_refs <= 0.0)
    (void)string_list_push_copy(errors, "compiler std audit SIMD coverage missing");
  if (!strstr(json, "\"runtime_surface_state\":\"") ||
      !strstr(json, "\"crt_surface_state\":\"") ||
      !strstr(json, "\"runtime_surface_scope\":\"") ||
      !strstr(json, "\"crt_behavior_scope\":\"") ||
      !strstr(json, "\"crt_behavior_next_action\":\"") ||
      !strstr(json, "\"simd_runtime_exports\":"))
    (void)string_list_push_copy(errors, "compiler std audit aliases missing from rows");

  char *markdown = summary_string_from_report(json, "markdown");
  if (!markdown || !strstr(markdown, "compiler_std_audit.md")) {
    (void)string_list_push_copy(errors, "compiler std audit markdown path missing");
  } else {
    char markdown_abs[4096] = {0};
    if (path_is_absolute(markdown)) {
      snprintf(markdown_abs, sizeof(markdown_abs), "%s", markdown);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(markdown_abs, sizeof(markdown_abs), root, markdown);
    }
    file_buf_t md = {0};
    if (!markdown_abs[0] || !read_file(markdown_abs, &md) || !md.data) {
      (void)string_list_push_copy(errors, "compiler std audit markdown was not readable");
    } else if (!strstr(md.data, "# Nytrix Compiler Std Audit") ||
                  !strstr(md.data, "TLDR") ||
                  !strstr(md.data, "Runtime surface") ||
                     !strstr(md.data, "CRT aliases") ||
                     !strstr(md.data, "Claim scope") ||
                     !strstr(md.data, "not a bugless CRT behavior proof") ||
                  !strstr(md.data, "CRT behavior next") ||
                  !strstr(md.data, "./build/fuzz/all/run-next.sh") ||
                  (unreferenced_count > 0.0 &&
                (!strstr(md.data, "CRT families") ||
                 !strstr(md.data, "## CRT Families") ||
                 !strstr(md.data, "## Next CRT") ||
                 !strstr(md.data, "cover-unreferenced-family") ||
                 !strstr(md.data, "Definitions:") ||
                 !strstr(md.data, "Inspect:") ||
                 !strstr(md.data, "#1 `") ||
                 !strstr(md.data, "gain ") ||
                 !strstr(md.data, "after "))) ||
               !strstr(md.data, "SIMD wrappers") ||
               !strstr(md.data, "Refresh") ||
               !strstr(md.data, "env NYTRIX_LOW_PRIORITY=1") ||
               !strstr(md.data, "NYTRIX_RUN_NICE=10") ||
               !strstr(md.data, "nice -n 10") ||
               !strstr(md.data, "compiler std-audit --json") ||
               !strstr(md.data, "--markdown") ||
               strstr(md.data, "$JSON")) {
      (void)string_list_push_copy(errors, "compiler std audit markdown is incomplete");
    }
    free(md.data);
  }
  free(markdown);
  free(crt_next_inspect_command);
  free(crt_next_definition_locations);
  free(crt_next_definition_file);
  free(crt_next_exports);
  free(crt_next_family);
  free(crt_next_reason);
  free(crt_next_action);
  free(crt_families);
  free(crt_top_family);
  free(crt_state);
  free(state);
  free(crt_behavior_scope);
  free(crt_behavior_state);
  free(crt_behavior_next_command);
  free(crt_behavior_next_reason);
  free(crt_behavior_next_action);
  free(crt_scope);
  free(runtime_scope);
  free(mode);
}

static void selftest_validate_synth_print_report(const char *json,
                                                 string_list_t *errors,
                                                 int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "synth-print") != 0)
    (void)string_list_push_copy(errors, "synth print mode mismatch");
  char *work_dir = summary_string_from_report(json, "work_dir");
  if (!work_dir || !strstr(work_dir, "build/cache/scratch/selftest_synth_print_") ||
      strstr(work_dir, "/build/cache/"))
    (void)string_list_push_copy(errors, "synth print work dir escaped build/cache/scratch");
  free(work_dir);
  free(mode);
}

static void selftest_validate_reduce_artifact_report(const char *json,
                                                    string_list_t *errors,
                                                    int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "reduce-artifact") != 0)
    (void)string_list_push_copy(errors, "reduce artifact mode mismatch");
  char *out = summary_string_from_report(json, "out");
  if (!out || !strstr(out, "build/cache/scratch/reduced/") ||
      strstr(out, "etc/assets/dict/fuzz/work") || strstr(out, "/tmp/"))
    (void)string_list_push_copy(errors, "reduce artifact default output escaped build/cache/scratch");
  free(out);
  free(mode);
}

static void selftest_validate_cli_equals_args(const char *json,
                                              string_list_t *errors,
                                              int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  if (*row_count != 1)
    (void)string_list_push_copy(errors, "equals-form json command row count mismatch");
  double files_scanned = -1.0;
  if (!summary_number_from_report(json, "files_scanned", &files_scanned) ||
      files_scanned <= 0.0)
    (void)string_list_push_copy(errors, "equals-form json command did not run python-clean");
  if (!strstr(json, "\"selftest_scope\":\"nytrix-tree-clean\""))
    (void)string_list_push_copy(errors, "equals-form json command wrote unexpected report");
}

static void selftest_validate_shape_audit(const char *json, string_list_t *errors, int *row_count) {
  *row_count = 0;
  double count = 0.0, errors_count = 1.0;
  if (!extract_json_number(json, "count", &count)) {
    (void)string_list_push_copy(errors, "missing shape count");
  }
  if (!extract_json_number(json, "errors", &errors_count)) {
    (void)string_list_push_copy(errors, "missing shape error count");
  } else if (errors_count != 0.0) {
    (void)string_list_push_copy(errors, "shape audit reported errors");
  }
  const char *generators = json_value_after_key(json, "generators");
  if (!generators || *generators != '{') {
    (void)string_list_push_copy(errors, "missing generators object");
  } else if (!strstr(generators, "\"stress\"")) {
    (void)string_list_push_copy(errors, "missing stress generator count");
  }
}

static void selftest_validate_unsupported_stdout(const char *json, string_list_t *errors, int *row_count) {
  *row_count = 1;
  if (!json || !*json) {
    (void)string_list_push_copy(errors, "missing stdout json");
    return;
  }
  char *error = json_string_or_empty(json, "error");
  char *category = json_string_or_empty(json, "diagnostic_category");
  double line = 0.0;
  if (strcmp(error ? error : "", "unsupported") != 0)
    (void)string_list_push_copy(errors, "stdout json did not report unsupported");
  if (!category || !*category)
    (void)string_list_push_copy(errors, "missing diagnostic category");
  if (!extract_json_number(json, "line", &line) || line <= 0.0)
    (void)string_list_push_copy(errors, "missing diagnostic line");
  free(error);
  free(category);
}

static void selftest_validate_fuzz_all_help_stdout(const char *json,
                                                   string_list_t *errors,
                                                   int *row_count) {
  *row_count = 1;
  if (!json || !*json) {
    (void)string_list_push_copy(errors, "missing fuzz-all help stdout");
    return;
  }
  char *topic = json_string_or_empty(json, "topic");
  char *purpose = json_string_or_empty(json, "purpose");
  if (!topic || strcmp(topic, "fuzz all") != 0)
    (void)string_list_push_copy(errors, "fuzz-all help topic missing");
  if (!purpose || !strstr(purpose, "long-run") ||
      !strstr(purpose, "C-vs-Ny perf"))
    (void)string_list_push_copy(errors, "fuzz-all help purpose is too weak");
  bool top_ok = false;
  double top_cases = -1.0, top_ok_count = -1.0, top_failure_count = -1.0;
  double command_count = -1.0, example_count = -1.0;
  if (!json_top_level_bool_from_report(json, "ok", &top_ok) || !top_ok ||
      !json_top_level_number_from_report(json, "cases", &top_cases) ||
      top_cases != 1.0 ||
      !json_top_level_number_from_report(json, "ok_count", &top_ok_count) ||
      top_ok_count != 1.0 ||
      !json_top_level_number_from_report(json, "failure_count",
                                         &top_failure_count) ||
      top_failure_count != 0.0)
    (void)string_list_push_copy(errors,
                                "fuzz-all help result aliases missing");
  if (!json_top_level_number_from_report(json, "command_count",
                                         &command_count) ||
      command_count != 19.0 ||
      !json_top_level_number_from_report(json, "example_count",
                                         &example_count) ||
      example_count != 16.0)
    (void)string_list_push_copy(errors,
                                "fuzz-all help command/example counts missing");
  const char *commands_json = json_top_level_value_after_key(json, "commands");
  const char *examples_json = json_top_level_value_after_key(json, "examples");
  int actual_command_count = count_json_array_items(commands_json);
  int actual_example_count = count_json_array_items(examples_json);
  if (!commands_json || *commands_json != '[' ||
      actual_command_count != (int)command_count)
    (void)string_list_push_copy(errors,
                                "fuzz-all help command_count does not match commands");
  if (!examples_json || *examples_json != '[' ||
      actual_example_count != (int)example_count)
    (void)string_list_push_copy(errors,
                                "fuzz-all help example_count does not match examples");
  char *quick_probe_command =
      json_string_or_empty(json, "quick_probe_command");
  char *expected_quick_probe_command =
      fuzz_all_quick_jq_command("build/fuzz/all/status.json");
  char *state_probe_command =
      json_string_or_empty(json, "state_probe_command");
  char *expected_state_probe_command =
      fuzz_all_state_command("build/fuzz/all/run-next-state.json");
  char *status_command =
      json_string_or_empty(json, "status_command");
  char *progress_command =
      json_string_or_empty(json, "progress_command");
  char *run_next_command =
      json_string_or_empty(json, "run_next_command");
  char *run_next_preview_command =
      json_string_or_empty(json, "run_next_preview_command");
  char *run_next_low_cpu_command =
      json_string_or_empty(json, "run_next_low_cpu_command");
  char *run_next_gentle_command =
      json_string_or_empty(json, "run_next_gentle_command");
  char *run_next_gentle_preview_command =
      json_string_or_empty(json, "run_next_gentle_preview_command");
  char *old_path_probe_command =
      json_string_or_empty(json, "old_path_probe_command");
  char *old_path_dry_run_command =
      json_string_or_empty(json, "old_path_dry_run_command");
  char *old_path_apply_command =
      json_string_or_empty(json, "old_path_apply_command");
  char *selftest_catalog_command =
      json_string_or_empty(json, "selftest_catalog_command");
  char *selftest_result_probe_command =
      json_string_or_empty(json, "selftest_result_probe_command");
  char *selftest_cockpit_run_command =
      json_string_or_empty(json, "selftest_cockpit_run_command");
  char *selftest_cockpit_result_probe_command =
      json_string_or_empty(json, "selftest_cockpit_result_probe_command");
  char *known_bugs_command =
      json_string_or_empty(json, "known_bugs_command");
  char *known_bugs_report =
      json_string_or_empty(json, "known_bugs_report");
  char *known_bugs_result_probe_command =
      json_string_or_empty(json, "known_bugs_result_probe_command");
  char *perf_triage_command =
      json_string_or_empty(json, "perf_triage_command");
  char *perf_triage_report =
      json_string_or_empty(json, "perf_triage_report");
  char *perf_triage_markdown =
      json_string_or_empty(json, "perf_triage_markdown");
  char *perf_triage_result_probe_command =
      json_string_or_empty(json, "perf_triage_result_probe_command");
  if (!quick_probe_command ||
      !strstr(quick_probe_command, "gate:{ready,blockers,active:.active_count") ||
      !strstr(quick_probe_command, "script:.next_script") ||
      !strstr(quick_probe_command, "handoff:.next_handoff_command") ||
      !strstr(quick_probe_command, "run:.next_command") ||
      !strstr(quick_probe_command, "recommended:.recommended_command") ||
      !strstr(quick_probe_command,
              "state_refresh:.recommended_state_refresh_command") ||
      !strstr(quick_probe_command,
              "freshen:.freshness_action_command") ||
      !strstr(quick_probe_command,
              "runstate:{state:.state,event:.state_event,fresh:.state_fresh,age:.state_age_seconds,age_h:(.state_age_seconds/3600),stale_after:.state_stale_after_seconds,stale_after_h:(.state_stale_after_seconds/3600),over_s:(.state_age_seconds-.state_stale_after_seconds),reason:.state_stale_reason,dry_h:.state_dry_run_wall_hours,dry_gain_pct:.state_dry_run_campaign_gain_percent,dry_years:.state_dry_run_thread_years,threads:.state_handoff_threads}") ||
      strstr(quick_probe_command,
             "low:.recommended_low_cpu_command,refresh:") ||
      strstr(quick_probe_command, "script:.next_command") ||
      !strstr(quick_probe_command, "low:.recommended_low_cpu_command") ||
      !strstr(quick_probe_command, "gentle:.run_next_gentle_command") ||
      !strstr(quick_probe_command,
              "gentle_preview:.run_next_gentle_preview_command") ||
      !strstr(quick_probe_command,
              "perf:{hotspots:.perf_hotspots_open,watch:.perf_watchlist_state,worst:{case:.perf_worst_case,ratio:.perf_worst_ratio,slow:.perf_worst_slowdown_percent},opt:{action:.optimization_action,case:.optimization_case,ratio:.optimization_ratio,cmd:.optimization_command,target:.optimization_target_command}}") ||
      !strstr(quick_probe_command, "scratch:.scratch_root") ||
      !strstr(quick_probe_command, "tmp:.tmp_dir") ||
      !strstr(quick_probe_command, "xdg:.xdg_cache_home") ||
      !strstr(quick_probe_command, "nytrix_cache:.nytrix_cache_dir") ||
      !strstr(quick_probe_command, "next:.crt_behavior_next_action") ||
      !strstr(quick_probe_command, "build/fuzz/all/status.json"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help quick probe alias missing");
  if (!expected_quick_probe_command ||
      strcmp(quick_probe_command ? quick_probe_command : "",
             expected_quick_probe_command) != 0)
    (void)string_list_push_copy(errors,
                                "fuzz-all help quick probe drifted from canonical generator");
  if (!state_probe_command ||
      !strstr(state_probe_command, "live,child_status,stale_after_seconds") ||
      !strstr(state_probe_command, "dry_run_wall_hours") ||
      !strstr(state_probe_command, "build/fuzz/all/run-next-state.json"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help state probe alias missing");
  if (!expected_state_probe_command ||
      strcmp(state_probe_command ? state_probe_command : "",
             expected_state_probe_command) != 0)
    (void)string_list_push_copy(errors,
                                "fuzz-all help state probe drifted from canonical generator");
  if (!status_command ||
      !selftest_command_uses_env_nice(status_command) ||
      !strstr(status_command, "fuzz all status --refresh --strict") ||
      !strstr(status_command, "--allow-full-pressure-remediation") ||
      !strstr(status_command, "--dir build/fuzz/all") ||
      !strstr(status_command, "--target-thread-years 10") ||
      !strstr(status_command, "--hours 8") ||
      !strstr(status_command, "--json build/fuzz/all/status.json") ||
      !strstr(status_command, "--markdown build/fuzz/all/status.md") ||
      strstr(status_command, "--target-thread-years N") ||
      strstr(status_command, "--hours H"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help status alias missing");
  if (!progress_command ||
      !selftest_command_uses_env_nice(progress_command) ||
      !strstr(progress_command, "fuzz all progress --refresh --strict") ||
      !strstr(progress_command, "--allow-full-pressure-remediation") ||
      !strstr(progress_command, "--status build/fuzz/all/status.json") ||
      !strstr(progress_command, "--target-thread-years 10") ||
      !strstr(progress_command, "--hours 8") ||
      !strstr(progress_command, "--json build/fuzz/all/progress.json") ||
      !strstr(progress_command, "--markdown build/fuzz/all/progress.md") ||
      strstr(progress_command, "--target-thread-years N") ||
      strstr(progress_command, "--hours H"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help progress alias missing");
  if (!run_next_command ||
      !strstr(run_next_command, "env NYTRIX_LOW_PRIORITY=1") ||
      !strstr(run_next_command, "NYTRIX_RUN_NICE=10") ||
      !strstr(run_next_command, "nice -n 10") ||
      strstr(run_next_command, "NYTRIX_RUN_DRY_RUN=1") ||
      !strstr(run_next_command, "./build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help run-next alias missing");
  if (!run_next_preview_command ||
      !strstr(run_next_preview_command, "env NYTRIX_LOW_PRIORITY=1") ||
      !strstr(run_next_preview_command, "NYTRIX_RUN_NICE=10") ||
      !strstr(run_next_preview_command, "NYTRIX_RUN_DRY_RUN=1") ||
      !strstr(run_next_preview_command, "nice -n 10") ||
      !strstr(run_next_preview_command, "./build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help preview alias missing");
  if (!run_next_low_cpu_command ||
      !strstr(run_next_low_cpu_command, "env NYTRIX_LOW_PRIORITY=1") ||
      !strstr(run_next_low_cpu_command, "NYTRIX_LOW_PRIORITY=1") ||
      !strstr(run_next_low_cpu_command, "NYTRIX_RUN_NICE=10") ||
      !strstr(run_next_low_cpu_command, "nice -n 10") ||
      strstr(run_next_low_cpu_command, "NYTRIX_RUN_DRY_RUN=1") ||
      !strstr(run_next_low_cpu_command, "./build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help low-cpu run alias missing");
  if (!run_next_gentle_command ||
      !strstr(run_next_gentle_command, "env NYTRIX_LOW_PRIORITY=1") ||
      !strstr(run_next_gentle_command, "NYTRIX_LOW_PRIORITY=1") ||
      !strstr(run_next_gentle_command, "NYTRIX_RUN_NICE=10") ||
      !strstr(run_next_gentle_command, "NYTRIX_RUN_HOURS=1") ||
      !strstr(run_next_gentle_command, "NYTRIX_RUN_THREADS=10%") ||
      !strstr(run_next_gentle_command, "nice -n 10") ||
      strstr(run_next_gentle_command, "NYTRIX_RUN_DRY_RUN=1") ||
      !strstr(run_next_gentle_command, "./build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help gentle run alias missing");
  if (!run_next_gentle_preview_command ||
      !strstr(run_next_gentle_preview_command, "env NYTRIX_LOW_PRIORITY=1") ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_LOW_PRIORITY=1") ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_RUN_NICE=10") ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_RUN_DRY_RUN=1") ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_RUN_HOURS=1") ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_RUN_THREADS=10%") ||
      !strstr(run_next_gentle_preview_command, "nice -n 10") ||
      !strstr(run_next_gentle_preview_command, "./build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help gentle preview alias missing");
  if (!old_path_probe_command ||
      !selftest_command_uses_env_nice(old_path_probe_command) ||
      !strstr(old_path_probe_command, "fuzz all old-paths --dry-run --probe") ||
      !strstr(old_path_probe_command, "build/fuzz/all/old-paths.json") ||
      !old_path_dry_run_command ||
      !selftest_command_uses_env_nice(old_path_dry_run_command) ||
      !strstr(old_path_dry_run_command, "fuzz all old-paths --dry-run") ||
      !strstr(old_path_dry_run_command, "--archive-dir build/cache/old-nytrix") ||
      !old_path_apply_command ||
      !selftest_command_uses_env_nice(old_path_apply_command) ||
      !strstr(old_path_apply_command, "fuzz all old-paths --apply") ||
      !strstr(old_path_apply_command, "--wait-writers-s 300"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help old-path aliases missing low-priority guard");
  if (strcmp(old_path_probe_command ? old_path_probe_command : "",
             NYTRIX_FUZZ_ALL_OLD_PATH_PROBE_COMMAND) != 0 ||
      strcmp(old_path_dry_run_command ? old_path_dry_run_command : "",
             NYTRIX_FUZZ_ALL_OLD_PATH_DRY_RUN_COMMAND) != 0 ||
      strcmp(old_path_apply_command ? old_path_apply_command : "",
             NYTRIX_FUZZ_ALL_OLD_PATH_APPLY_COMMAND) != 0)
    (void)string_list_push_copy(errors,
                                "fuzz-all help old-path aliases drifted from shared commands");
  if (!selftest_catalog_command ||
      !strstr(selftest_catalog_command, "env NYTRIX_LOW_PRIORITY=1") ||
      !strstr(selftest_catalog_command, "NYTRIX_RUN_NICE=10") ||
      !strstr(selftest_catalog_command, "nice -n 10") ||
      !strstr(selftest_catalog_command, "selftest run --list") ||
      !selftest_result_probe_command ||
      !selftest_command_uses_env_nice(selftest_result_probe_command) ||
      !strstr(selftest_result_probe_command, "selftest run --list --probe") ||
      !strstr(selftest_result_probe_command, "build/fuzz/all/selftest-catalog.json") ||
      strstr(selftest_result_probe_command, "jq '") ||
      !selftest_cockpit_run_command ||
      !strstr(selftest_cockpit_run_command, "env NYTRIX_LOW_PRIORITY=1") ||
      !strstr(selftest_cockpit_run_command, "NYTRIX_RUN_NICE=10") ||
      !strstr(selftest_cockpit_run_command, "nice -n 10") ||
      !strstr(selftest_cockpit_run_command,
              "./build/nytrix selftest run --only fuzz_all_help") ||
      !strstr(selftest_cockpit_run_command, "--only compiler_findings") ||
      !strstr(selftest_cockpit_run_command, "--only compiler_known_bugs") ||
      !strstr(selftest_cockpit_run_command, "--only compiler_std_audit") ||
      !strstr(selftest_cockpit_run_command, "--only perf_triage_args") ||
      strstr(selftest_cockpit_run_command, "jq -r .cockpit_command") ||
      !selftest_cockpit_result_probe_command ||
      !strstr(selftest_cockpit_result_probe_command, "requested_cases") ||
      !strstr(selftest_cockpit_result_probe_command, "executed_cases") ||
      !strstr(selftest_cockpit_result_probe_command, "skipped_slow_count") ||
      !strstr(selftest_cockpit_result_probe_command,
              "all_requested_executed") ||
      strstr(selftest_cockpit_result_probe_command, "jq '") ||
      !strstr(selftest_cockpit_result_probe_command,
              "build/fuzz/all/selftest-cockpit.json"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help selftest aliases missing");
  if (strcmp(selftest_catalog_command ? selftest_catalog_command : "",
             NYTRIX_FUZZ_ALL_SELFTEST_CATALOG) != 0 ||
      strcmp(selftest_result_probe_command ? selftest_result_probe_command : "",
             NYTRIX_FUZZ_ALL_SELFTEST_PROBE) != 0 ||
      strcmp(selftest_cockpit_run_command ? selftest_cockpit_run_command : "",
             NYTRIX_FUZZ_ALL_SELFTEST_RUN) != 0 ||
      strcmp(selftest_cockpit_result_probe_command ?
                 selftest_cockpit_result_probe_command : "",
             NYTRIX_FUZZ_ALL_SELFTEST_COCKPIT_PROBE) != 0)
    (void)string_list_push_copy(errors,
                                "fuzz-all help selftest aliases drifted from shared commands");
  if (!known_bugs_command ||
      !strstr(known_bugs_command, "env NYTRIX_LOW_PRIORITY=1") ||
      !strstr(known_bugs_command, "NYTRIX_LOW_PRIORITY=1") ||
      !strstr(known_bugs_command, "NYTRIX_RUN_NICE=10") ||
      !strstr(known_bugs_command, "nice -n 10") ||
      !strstr(known_bugs_command, "./build/nytrix") ||
      !strstr(known_bugs_command, "compiler known-bugs --timeout-s 15") ||
      !known_bugs_report ||
      strcmp(known_bugs_report, NYTRIX_FUZZ_ALL_KNOWN_BUGS_REPORT) != 0 ||
      !known_bugs_result_probe_command ||
      strcmp(known_bugs_result_probe_command,
             NYTRIX_FUZZ_ALL_KNOWN_BUGS_PROBE) != 0 ||
      !perf_triage_command ||
      !strstr(perf_triage_command, "env NYTRIX_LOW_PRIORITY=1") ||
      !strstr(perf_triage_command, "NYTRIX_LOW_PRIORITY=1") ||
      !strstr(perf_triage_command, "NYTRIX_RUN_NICE=10") ||
      !strstr(perf_triage_command, "nice -n 10") ||
      !strstr(perf_triage_command, "./build/nytrix") ||
      !strstr(perf_triage_command, "perf triage --fast --limit 5") ||
      !perf_triage_report ||
      strcmp(perf_triage_report, NYTRIX_FUZZ_ALL_PERF_TRIAGE_REPORT) != 0 ||
      !perf_triage_markdown ||
      strcmp(perf_triage_markdown,
             NYTRIX_FUZZ_ALL_PERF_TRIAGE_MARKDOWN) != 0 ||
      !perf_triage_result_probe_command ||
      strcmp(perf_triage_result_probe_command,
             NYTRIX_FUZZ_ALL_PERF_TRIAGE_PROBE) != 0)
    (void)string_list_push_copy(errors,
                                "fuzz-all help proof aliases missing");
  if (strcmp(known_bugs_command ? known_bugs_command : "",
             NYTRIX_FUZZ_ALL_KNOWN_BUGS_COMMAND) != 0 ||
      strcmp(known_bugs_report ? known_bugs_report : "",
             NYTRIX_FUZZ_ALL_KNOWN_BUGS_REPORT) != 0 ||
      strcmp(known_bugs_result_probe_command ?
                 known_bugs_result_probe_command : "",
             NYTRIX_FUZZ_ALL_KNOWN_BUGS_PROBE) != 0 ||
      strcmp(perf_triage_command ? perf_triage_command : "",
             NYTRIX_FUZZ_ALL_PERF_TRIAGE_COMMAND) != 0 ||
      strcmp(perf_triage_report ? perf_triage_report : "",
             NYTRIX_FUZZ_ALL_PERF_TRIAGE_REPORT) != 0 ||
      strcmp(perf_triage_markdown ? perf_triage_markdown : "",
             NYTRIX_FUZZ_ALL_PERF_TRIAGE_MARKDOWN) != 0 ||
      strcmp(perf_triage_result_probe_command ?
                 perf_triage_result_probe_command : "",
             NYTRIX_FUZZ_ALL_PERF_TRIAGE_PROBE) != 0)
    (void)string_list_push_copy(errors,
                                "fuzz-all help proof aliases drifted from shared commands");
  if (!strstr(json, "\"commands\"") ||
      !strstr(json, "fuzz all preflight") ||
      !strstr(json, "fuzz all status --refresh") ||
      !strstr(json, "fuzz all status --refresh --strict --allow-full-pressure-remediation --dir build/fuzz/all --history build/fuzz/all/history.json") ||
      !strstr(json, "--json build/fuzz/all/status.json") ||
      !strstr(json, "--markdown build/fuzz/all/status.md") ||
      !strstr(json, "fuzz all progress --refresh --strict") ||
      !strstr(json, "jq {gate:{ready,blockers,active:.active_count}") ||
      !strstr(json, "score:{pct:.language_score_percent,label:.language_score_label,state:.completion_state}") ||
      !strstr(json, "campaign:{pct:.campaign_done_percent,years:.campaign_thread_years,target:.campaign_target_thread_years") ||
      !strstr(json, "runs:.campaign_runs_needed,days:.campaign_wall_days_needed,per:.campaign_percent_per_run,per_day:.campaign_percent_per_wall_day,eq_days:.campaign_equivalent_wall_days,src:.thread_years_per_run_source,plan_h:.campaign_plan_wall_hours,plan_threads:.campaign_plan_threads,span_days:.campaign_calendar_span_days,age_days:.campaign_calendar_age_days,calendar_pct:.campaign_calendar_percent_10y,eta:.campaign_eta_local}") ||
      !strstr(json, "fresh:{ok:.evidence_fresh,penalty:.freshness_penalty,latest_h:.latest_report_age_hours") ||
      !strstr(json, "latest_win_h:.latest_report_stale_after_hours,latest_over_h:.latest_report_freshness_overdue_hours") ||
      !strstr(json, "full_h:.latest_full_pressure_report_age_hours,full_win_h:.latest_full_pressure_report_stale_after_hours,full_over_h:.latest_full_pressure_report_freshness_overdue_hours,over_h:.evidence_freshness_overdue_hours}") ||
      !strstr(json, "next:{action:.recommended_action,reason:.recommended_reason,script:.next_script,handoff:.next_handoff_command,run:.next_command,recommended:.recommended_command") ||
      !strstr(json, "preview:.recommended_preview_command,low:.recommended_low_cpu_command,gentle:.run_next_gentle_command,gentle_preview:.run_next_gentle_preview_command,state_refresh:.recommended_state_refresh_command,freshen:.freshness_action_command") ||
      !strstr(json, "runstate:{state:.state,event:.state_event,fresh:.state_fresh,age:.state_age_seconds,age_h:(.state_age_seconds/3600),stale_after:.state_stale_after_seconds,stale_after_h:(.state_stale_after_seconds/3600),over_s:(.state_age_seconds-.state_stale_after_seconds),reason:.state_stale_reason,dry_h:.state_dry_run_wall_hours,dry_gain_pct:.state_dry_run_campaign_gain_percent,dry_years:.state_dry_run_thread_years,threads:.state_handoff_threads}") ||
      !strstr(json, "surfaces:{compiler:.compiler_findings,known:.known_bug_replay_findings,perf:{hotspots:.perf_hotspots_open,watch:.perf_watchlist_state,worst:{case:.perf_worst_case,ratio:.perf_worst_ratio,slow:.perf_worst_slowdown_percent},opt:{action:.optimization_action,case:.optimization_case,ratio:.optimization_ratio,cmd:.optimization_command,target:.optimization_target_command}}") ||
      !strstr(json, "rt:{state:.runtime_surface_state,done:.runtime_coverage_done,total:.runtime_coverage_total}") ||
      !strstr(json, "crt:{state:.crt_surface_state,scope:.crt_surface_scope,behavior:.crt_behavior_state,next:.crt_behavior_next_action,done:.crt_coverage_done,total:.crt_coverage_total,families:.crt_unreferenced_family_count}") ||
      !strstr(json, "paths:{scratch:.scratch_root,tmp:.tmp_dir,xdg:.xdg_cache_home,nytrix_cache:.nytrix_cache_dir,old_test:.old_nytrix_test_scratch_absent,old_fuzz:.old_nytrix_fuzz_absent,old_cache:.old_nytrix_build_cache_absent,old_writer:.active_old_nytrix_output_writer_present,old_policy:.old_path_cache_policy_ok,old_action:.old_path_next_action,old_seen:.old_path_present_count,old_moved:.old_path_moved_count,old_current:.old_path_remaining_count,old_wait_s:.old_path_wait_remaining_seconds,old_leaks:.old_path_artifact_leak_count,artifact_remaining:.old_path_artifact_remaining_count}} build/fuzz/all/status.json") ||
      !strstr(json, "\"details\":\"Use build/fuzz/all/status.md") ||
      !strstr(json, NYTRIX_FUZZ_ALL_STATE_JQ_DEFAULT) ||
      !strstr(json, "--status build/fuzz/all/status.json") ||
      !strstr(json, "--history build/fuzz/all/history.json") ||
      !strstr(json, "--worklist build/fuzz/all/worklist.json") ||
      !strstr(json, "--coverage build/fuzz/all/coverage.json") ||
      !strstr(json, "--plan build/fuzz/all/plan.json") ||
      !strstr(json, "--target-thread-years 10") ||
      !strstr(json, "--hours 8") ||
      !strstr(json, "--threads 25%") ||
      !strstr(json, "--profile insane") ||
      !strstr(json, "--json build/fuzz/all/progress.json") ||
      !strstr(json, "fuzz all old-paths --dry-run") ||
      !strstr(json, "fuzz all old-paths --apply --wait-writers-s 300") ||
      !strstr(json, "--nytrix-root ../nytrix") ||
      !strstr(json, "--archive-dir build/cache/old-nytrix") ||
      !strstr(json, "--json build/fuzz/all/old-paths.json") ||
      !strstr(json, "--markdown build/fuzz/all/old-paths.md") ||
      !strstr(json, "fuzz all old-paths --dry-run --probe --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json build/fuzz/all/old-paths.json --markdown build/fuzz/all/old-paths.md") ||
      !strstr(json, "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix compiler known-bugs") ||
      !strstr(json, "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix perf triage") ||
      !strstr(json, "compiler known-bugs --timeout-s 15 --json build/fuzz/ultra/compiler-known-bugs.json") ||
      !strstr(json, "perf triage --fast --limit 5 --threshold 1.50 --json build/fuzz/ultra/perf-triage-current.json --markdown build/fuzz/ultra/perf-triage-current.md") ||
      !strstr(json, "selftest run --list --json build/fuzz/all/selftest-catalog.json --markdown build/fuzz/all/selftest-catalog.md") ||
      !strstr(json, "selftest run --list --probe --json build/fuzz/all/selftest-catalog.json --markdown build/fuzz/all/selftest-catalog.md") ||
      !strstr(json, "jq {ok,cases,ok_count,failure_count,requested_cases,executed_cases,skipped_slow_count,all_requested_executed} build/fuzz/all/selftest-cockpit.json") ||
      !strstr(json, "env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix selftest run --only fuzz_all_help") ||
      !strstr(json, "--only compiler_findings") ||
      !strstr(json, "--only compiler_known_bugs") ||
      !strstr(json, "--only compiler_std_audit") ||
      !strstr(json, "--only perf_triage_args") ||
      strstr(json, "compiler-known-bugs-current.json") ||
      strstr(json, "perf-triage-fast.json") ||
      !strstr(json, "NYTRIX_LOW_PRIORITY=1") ||
      !strstr(json, "NYTRIX_RUN_NICE=10") ||
      !strstr(json, "NYTRIX_RUN_DRY_RUN=1 nice -n 10 ./build/fuzz/all/run-next.sh") ||
      !strstr(json, "NYTRIX_RUN_NICE=10 nice -n 10 ./build/fuzz/all/run-next.sh") ||
      !strstr(json, "NYTRIX_RUN_HOURS=1 NYTRIX_RUN_THREADS=10% nice -n 10 ./build/fuzz/all/run-next.sh") ||
      !strstr(json, "fuzz all run --profile insane"))
    (void)string_list_push_copy(errors, "fuzz-all help missing campaign commands");
  if (strstr(json, "NYTRIX_RUN_REPEAT=good ./build/fuzz/all/run-next.sh") ||
      strstr(json, "NYTRIX_RUN_DRY_RUN=1 NYTRIX_RUN_REPEAT=good"))
    (void)string_list_push_copy(
        errors, "fuzz-all help hardcodes stale run-good handoff");
  if (strstr(quick_probe_command, "jq '") ||
      strstr(state_probe_command, "jq '") ||
      strstr(old_path_probe_command, "jq '"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help exposes quoted JSON probe alias");
  if (strstr(json, "\"./build/fuzz/all/run-next.sh\"") ||
      strstr(json, "\"build/fuzz/all/run-next.sh\""))
    (void)string_list_push_copy(errors,
                                "fuzz-all help exposes bare run-next handoff");
  if (strstr(json, "jq '{ok,cases,failure_count,present_count") ||
      strstr(json, "present_count,moved_count,remaining_count"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help old-path probe omits ok_count");
  if (strstr(json, "coverage_queue_non_advisory_count") ||
      strstr(json, "latest_full_pressure_perf_rows"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help leaked deep status probe fields");
  if (strstr(json, "score:{coverage_percent,campaign_percent") ||
      strstr(json, "recommended_repeat_mode,recommended_repeat_count"))
    (void)string_list_push_copy(errors,
                                "fuzz-all help leaked old kitchen-sink quick probe");
  if (!strstr(json, "\"core_flags\"") ||
      !strstr(json, "--target-thread-years N") ||
      !strstr(json, "--threads N|25%") ||
      !strstr(json, "--wait-writers-s N") ||
      !strstr(json, "--allow-full-pressure-remediation"))
    (void)string_list_push_copy(errors, "fuzz-all help missing core flags");
  if (!strstr(json, "\"guardrails\"") ||
      !strstr(json, "repo-local build/cache") ||
      !strstr(json, "Nytrix-owned lanes require --allow-nytrix") ||
      !strstr(json, "campaign lock") ||
      !strstr(json, "max-cycle repeat guard") ||
      !strstr(json, "inter-cycle cooldown") ||
      !strstr(json, "stop-file graceful pause") ||
      !strstr(json, "fuzz_all_preflight"))
    (void)string_list_push_copy(errors, "fuzz-all help missing guardrails");
  if (!strstr(json, "\"cache_policy\"") ||
      !strstr(json, "\"scratch\":\"build/cache/\"") ||
      !strstr(json, "old-sibling-test-scratch") ||
      !strstr(json, "old-sibling-fuzz-dir") ||
      !strstr(json, "old-sibling-build-cache"))
    (void)string_list_push_copy(errors, "fuzz-all help missing cache policy");
  if (strstr(json, "/home/e/nytrix/fuzz") ||
      strstr(json, "/home/e/nytrix/build/cache") ||
      strstr(json, "../nytrix/fuzz") ||
      strstr(json, "../nytrix/build/cache"))
    (void)string_list_push_copy(errors, "fuzz-all help leaks old path strings");
  if (strstr(json, "\"commands\":[\"shapes audit\""))
    (void)string_list_push_copy(errors, "fuzz-all help fell back to generic top-level help");
  free(topic);
  free(purpose);
  free(quick_probe_command);
  free(expected_quick_probe_command);
  free(state_probe_command);
  free(expected_state_probe_command);
  free(status_command);
  free(progress_command);
  free(run_next_command);
  free(run_next_preview_command);
  free(run_next_low_cpu_command);
  free(run_next_gentle_command);
  free(run_next_gentle_preview_command);
  free(old_path_probe_command);
  free(old_path_dry_run_command);
  free(old_path_apply_command);
  free(selftest_catalog_command);
  free(selftest_result_probe_command);
  free(selftest_cockpit_run_command);
  free(selftest_cockpit_result_probe_command);
  free(known_bugs_command);
  free(known_bugs_report);
  free(known_bugs_result_probe_command);
  free(perf_triage_command);
  free(perf_triage_report);
  free(perf_triage_markdown);
  free(perf_triage_result_probe_command);
}

static void selftest_validate_old_writer_classifier(string_list_t *errors) {
  if (!errors) return;
  if (process_cmdline_is_old_nytrix_output_writer(
          "timeout 10 bash -lc rg -n '/home/e/nytrix/fuzz|/home/e/nytrix/build/cache' src/cli.c",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier treats rg as a writer");
  if (process_cmdline_is_old_nytrix_output_writer(
          "grep -R /home/e/nytrix/build/cache README.md BUGS.md",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier treats grep as a writer");
  if (process_cmdline_is_old_nytrix_output_writer(
          "python3 -m http.server 8766 --bind 127.0.0.1 --directory build/cache/web/demos",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier treats read-only web serving as a writer");
  if (process_cmdline_is_old_nytrix_output_writer(
          "./build/nytrix fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json build/fuzz/all/old-paths.json",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier treats cleanup scan as a writer");
  if (process_cmdline_is_old_nytrix_output_writer(
          "/home/e/nytrix/build/release/ny --compiler-asserts -emit-only "
          "/home/e/nytrix/build/cache/scratch/afl_runs/ny-core/hangs/id:000000",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier treats Nytrix cache input as an old writer");
  if (process_cmdline_is_old_nytrix_output_writer(
          "python tool.py /tmp/not-nytrix/build/cache/out.json",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier treats absolute non-Nytrix cache as a writer");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "python3 -B ./build/cache/tools/rev/tool dec build/cache/inspiration/rev/sample "
          "--stdout --no-write --timeout 120",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier missed no-write Nytrix rev cache parent");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "python3 -B ./build/cache/tools/rev/tool dec build/cache/inspiration/rev/crackmes/"
          "angr-examples/examples/xmllint/xmllint_bin --stdout --no-write "
          "--truth --focus sub_b5f0 --timeout 90 --heartbeat 20 "
          "--max-functions 4 --max-bytes 1400 --scan-bytes 4096",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier missed live no-write rev cache parent");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "./build/nytrix fuzz all run --dir /home/e/nytrix/fuzz --json build/fuzz/all/run.json",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier missed old fuzz all output dir");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "afl-fuzz -i seeds -o /home/e/nytrix/fuzz/afl -- ./build/nytrix",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier missed old AFL output dir");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "python tool.py build/cache/out.json",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier missed Nytrix cwd cache writer");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "python tool.py ../build/cache/out.json",
          "/home/e/nytrix/tmp"))
    (void)string_list_push_copy(errors, "old-writer classifier missed parent-relative Nytrix cache writer");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "python3 -B ./build/cache/tools/rev/tool triage build/cache/inspiration/rev/sample "
          "--triage-dir build/cache/rev/triage-sample",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier missed Nytrix tmp tool cache writer");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "python3 -B ./build/cache/tools/rev/tool parity --all --suite ioli "
          "--limit 4 --timeout 80 --heartbeat 20 --max-functions 12 "
          "--max-bytes 1024 --scan-bytes 65536 --top 8",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier missed Nytrix rev parity cache writer");
  if (process_cmdline_is_old_nytrix_output_writer(
          "python3 -B ./build/cache/tools/rev/tool parity --all --suite ioli",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier treats Nytrix rev parity as old writer");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "codex-linux-sandbox --sandbox-policy-cwd /home/e/nytrix "
          "--command-cwd /home/e/nytrix -- /bin/zsh -c "
          "./build/cache/tools/rev/tool parity --all --suite ioli --limit 2",
          "/"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier missed sandboxed Nytrix rev parity");
  if (process_cmdline_is_old_nytrix_output_writer(
          "codex-linux-sandbox --sandbox-policy-cwd /home/e/nytrix "
          "--command-cwd /home/e/nytrix -- /bin/zsh -c "
          "./build/cache/tools/rev/tool parity --all --suite ioli --limit 2",
          "/"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier treats sandboxed Nytrix rev parity as old writer");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "/home/e/nytrix/build/release/ny --color=never "
          "/home/e/nytrix/build/cache/rev/probe/rev_decomp_selected.1.ny",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier missed absolute Nytrix cache probe");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "python3 -B build/cache/tools/public publish --all --apply --push --site-source public",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier missed Nytrix public publish");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "build/release/ny tools web --out public",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier missed Nytrix docs web writer");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "/home/e/nytrix/build/release/ny-lsp",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier missed Nytrix lsp cache user");
  if (process_cmdline_is_old_nytrix_output_writer(
          "/home/e/nytrix/build/release/ny-lsp",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier treats non-Nytrix lsp cwd as old writer");
  if (process_cmdline_is_old_nytrix_output_writer(
          "python3 -B ./make webasm --clean --require-wasm",
          "/home/e/nytrix"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier treats webasm parent without selected old cache as a writer");
  if (!process_cmdline_is_old_nytrix_output_writer(
          "python3 -B ./make docs --site https://x3ric.com/nytrix/",
          "/tmp/nytrix"))
    (void)string_list_push_copy(errors, "old-writer classifier missed temp Nytrix docs build");
}

static void selftest_validate_old_paths_policy_fields(
    const char *json, string_list_t *errors, bool expect_old_absent) {
  bool cache_policy_ok = false;
  bool old_test_absent = !expect_old_absent;
  bool old_fuzz_absent = !expect_old_absent;
  bool old_cache_absent = !expect_old_absent;
  if (!summary_bool_from_report(json, "cache_policy_ok", &cache_policy_ok) ||
      !cache_policy_ok)
    (void)string_list_push_copy(errors,
                                "old-path cache policy field missing");
  if (!summary_bool_from_report(json, "old_path_cache_policy_ok",
                                &cache_policy_ok) ||
      !cache_policy_ok)
    (void)string_list_push_copy(errors,
                                "old-path cache policy alias missing");
  if (!summary_bool_from_report(json, "old_nytrix_test_scratch_absent",
                                &old_test_absent) ||
      old_test_absent != expect_old_absent ||
      !summary_bool_from_report(json, "old_nytrix_fuzz_absent",
                                &old_fuzz_absent) ||
      old_fuzz_absent != expect_old_absent ||
      !summary_bool_from_report(json, "old_nytrix_build_cache_absent",
                                &old_cache_absent) ||
      old_cache_absent != expect_old_absent)
    (void)string_list_push_copy(errors,
                                "old-path absent proof fields wrong");
  selftest_expect_top_alias_bool(json, "cache_policy_ok", errors);
  selftest_expect_top_alias_bool(json, "old_path_cache_policy_ok", errors);
  selftest_expect_top_alias_bool(json, "old_nytrix_test_scratch_absent",
                                 errors);
  selftest_expect_top_alias_bool(json, "old_nytrix_fuzz_absent", errors);
  selftest_expect_top_alias_bool(json, "old_nytrix_build_cache_absent",
                                 errors);
  selftest_expect_top_alias_string(json, "tmp_dir", errors);
  selftest_expect_top_alias_string(json, "scratch_root", errors);
  selftest_expect_top_alias_string(json, "xdg_cache_home", errors);
  selftest_expect_top_alias_string(json, "nytrix_cache_dir", errors);
  selftest_expect_top_alias_string(json, "old_path_next_action", errors);
  selftest_expect_top_alias_string(json, "old_path_next_reason", errors);
  bool active_old_writer = true;
  bool active_old_nytrix_writer = true;
  if (!summary_bool_from_report(json, "active_old_writer",
                                &active_old_writer) ||
      active_old_writer ||
      !summary_bool_from_report(json, "active_old_nytrix_output_writer_present",
                                &active_old_nytrix_writer) ||
      active_old_nytrix_writer != active_old_writer)
    (void)string_list_push_copy(errors,
                                "old-path active writer aliases mismatch");
  const char *next_json = json_top_level_value_after_key(json, "next");
  if (!next_json || *next_json != '{' ||
      !strstr(next_json, "\"action\"") ||
      !strstr(next_json, "\"reason\""))
    (void)string_list_push_copy(errors,
                                "old-path compact next object missing");
  char *scratch_root = summary_string_from_report(json, "scratch_root");
  char *nytrix_cache_dir =
      summary_string_from_report(json, "nytrix_cache_dir");
  if (!scratch_root || strcmp(scratch_root, "build/cache/scratch") != 0 ||
      !nytrix_cache_dir ||
      strcmp(nytrix_cache_dir, "build/cache/nytrix") != 0)
    (void)string_list_push_copy(errors,
                                "old-path cache roots are not repo-local");
  free(scratch_root);
  free(nytrix_cache_dir);
}

static void selftest_validate_fuzz_all_old_paths(const char *json,
                                                 string_list_t *errors,
                                                 int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  selftest_expect_report_result_top_aliases(json, errors);
  selftest_validate_old_writer_classifier(errors);
  selftest_validate_old_paths_policy_fields(json, errors, true);
  if (*row_count != 6)
    (void)string_list_push_copy(errors, "old-path selftest row count mismatch");
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-old-paths") != 0)
    (void)string_list_push_copy(errors, "old-path report mode mismatch");
  bool dry_run = true, apply = false;
  if (!summary_bool_from_report(json, "dry_run", &dry_run) || dry_run)
    (void)string_list_push_copy(errors, "old-path apply run reported dry-run");
  if (!summary_bool_from_report(json, "apply", &apply) || !apply)
    (void)string_list_push_copy(errors, "old-path apply flag missing");
  double present = -1.0, moved = -1.0, remaining = -1.0;
  if (!summary_number_from_report(json, "present_count", &present) ||
      present != 6.0)
    (void)string_list_push_copy(errors, "old-path present count mismatch");
  if (!summary_number_from_report(json, "moved_count", &moved) ||
      moved != 6.0)
    (void)string_list_push_copy(errors, "old-path moved count mismatch");
  if (!summary_number_from_report(json, "remaining_count", &remaining) ||
      remaining != 0.0)
    (void)string_list_push_copy(errors, "old-path remaining count mismatch");
  if (!summary_number_from_report(json, "old_path_present_count", &present) ||
      present != 6.0 ||
      !summary_number_from_report(json, "old_path_moved_count", &moved) ||
      moved != 6.0 ||
      !summary_number_from_report(json, "old_path_remaining_count",
                                  &remaining) ||
      remaining != 0.0)
    (void)string_list_push_copy(errors, "old-path count aliases mismatch");
  double artifact_leaks = -1.0, artifact_moved = -1.0;
  double artifact_remaining = -1.0;
  if (!summary_number_from_report(json, "artifact_leak_count",
                                  &artifact_leaks) ||
      artifact_leaks != 3.0 ||
      !summary_number_from_report(json, "artifact_moved_count",
                                  &artifact_moved) ||
      artifact_moved != 3.0 ||
      !summary_number_from_report(json, "artifact_remaining_count",
                                  &artifact_remaining) ||
      artifact_remaining != 0.0)
    (void)string_list_push_copy(errors,
                                "old-path artifact archive counts mismatch");
  selftest_expect_top_alias_number(json, "artifact_leak_count",
                                   "artifact_leak_count", errors);
  selftest_expect_top_alias_number(json, "artifact_moved_count",
                                   "artifact_moved_count", errors);
  selftest_expect_top_alias_number(json, "artifact_remaining_count",
                                   "artifact_remaining_count", errors);
  selftest_expect_top_alias_number(json, "old_path_present_count",
                                   "old_path_present_count", errors);
  selftest_expect_top_alias_number(json, "old_seen", "old_seen", errors);
  selftest_expect_top_alias_number(json, "old_path_moved_count",
                                   "old_path_moved_count", errors);
  selftest_expect_top_alias_number(json, "old_moved", "old_moved", errors);
  selftest_expect_top_alias_number(json, "old_path_remaining_count",
                                   "old_path_remaining_count", errors);
  selftest_expect_top_alias_number(json, "old_current", "old_current",
                                   errors);
  selftest_expect_top_alias_number(json, "old_path_wait_remaining_seconds",
                                   "old_path_wait_remaining_seconds", errors);
  selftest_expect_top_alias_number(json, "wait_remaining_s",
                                   "wait_remaining_s", errors);
  selftest_expect_top_alias_number(json, "old_path_artifact_leak_count",
                                   "old_path_artifact_leak_count", errors);
  selftest_expect_top_alias_number(json, "old_leaks", "old_leaks", errors);
  selftest_expect_top_alias_number(json, "old_path_artifact_moved_count",
                                   "old_path_artifact_moved_count", errors);
  selftest_expect_top_alias_number(json, "old_artifacts_moved",
                                   "old_artifacts_moved", errors);
  selftest_expect_top_alias_number(json, "old_path_artifact_remaining_count",
                                   "old_path_artifact_remaining_count",
                                   errors);
  selftest_expect_top_alias_number(json, "artifact_remaining",
                                   "artifact_remaining", errors);
  double wait_s = -1.0, waited_s = -1.0;
  if (!summary_number_from_report(json, "wait_writers_s", &wait_s) ||
      wait_s != 1.0)
    (void)string_list_push_copy(errors, "old-path wait writer limit mismatch");
  if (!summary_number_from_report(json, "waited_writers_s", &waited_s) ||
      waited_s != 0.0)
    (void)string_list_push_copy(errors, "old-path waited despite no selected writer");
  bool cleared_after_wait = true;
  if (!summary_bool_from_report(json, "old_writer_cleared_after_wait",
                                &cleared_after_wait) ||
      cleared_after_wait)
    (void)string_list_push_copy(errors, "old-path wait clear flag mismatch");
  if (!strstr(json, "\"name\":\"old-test-scratch\"") ||
      !strstr(json, "\"name\":\"old-fuzz\"") ||
      !strstr(json, "\"name\":\"old-build-cache\"") ||
      !strstr(json, "\"source\":\"stale-report-artifact\"") ||
      !strstr(json, "\"action\":\"archived\""))
    (void)string_list_push_copy(errors, "old-path rows did not archive all expected paths");

  char root[4096] = {0};
  (void)find_nytrix_root(root, sizeof(root));
  char *nytrix_root = summary_string_from_report(json, "nytrix_root");
  char *archive_run = summary_string_from_report(json, "archive_run_dir");
  char *markdown = summary_string_from_report(json, "markdown");
  char ny_abs[4096] = {0}, archive_abs[4096] = {0}, md_abs[4096] = {0};
  if (nytrix_root && *nytrix_root) {
    if (path_is_absolute(nytrix_root)) snprintf(ny_abs, sizeof(ny_abs), "%s", nytrix_root);
    else (void)path_join(ny_abs, sizeof(ny_abs), root, nytrix_root);
  }
  if (archive_run && *archive_run) {
    if (path_is_absolute(archive_run)) snprintf(archive_abs, sizeof(archive_abs), "%s", archive_run);
    else (void)path_join(archive_abs, sizeof(archive_abs), root, archive_run);
  }
  if (markdown && *markdown) {
    if (path_is_absolute(markdown)) snprintf(md_abs, sizeof(md_abs), "%s", markdown);
    else (void)path_join(md_abs, sizeof(md_abs), root, markdown);
  }
  const char *old_rels[] = {"build/cache/projects/test", "fuzz", "build/cache"};
  for (size_t i = 0; i < sizeof(old_rels) / sizeof(old_rels[0]); ++i) {
    char p[4096] = {0};
    if (!path_join(p, sizeof(p), ny_abs, old_rels[i]) || exists_path(p))
      (void)string_list_push_copy(errors, "old-path source still exists after apply");
  }
  const char *archive_rels[] = {"tmp-projects-test", "fuzz", "build-cache",
                                "report-artifacts/stale-report.json",
                                "report-artifacts/stale-cockpit.md",
                                "report-artifacts/progress-r999.md"};
  for (size_t i = 0; i < sizeof(archive_rels) / sizeof(archive_rels[0]); ++i) {
    char p[4096] = {0};
    if (!path_join(p, sizeof(p), archive_abs, archive_rels[i]) ||
        !exists_path(p))
      (void)string_list_push_copy(errors, "old-path archive target missing after apply");
  }
  file_buf_t md = {0};
  if (!md_abs[0] || !read_file(md_abs, &md) || !md.data) {
    (void)string_list_push_copy(errors, "old-path markdown was not readable");
  } else if (!strstr(md.data, "Mode: `apply`") ||
             !strstr(md.data, "wait 0s/1s") ||
             !strstr(md.data, "moved 6") ||
             !strstr(md.data, "Stale build-artifact leaks: `3`") ||
             !strstr(md.data, "stale-report-artifact") ||
             !strstr(md.data, "remaining 0")) {
    (void)string_list_push_copy(errors, "old-path markdown summary mismatch");
  } else if (!strstr(md.data, "fuzz all status --refresh") ||
             !strstr(md.data, "fuzz_old_paths/status.json") ||
             !strstr(md.data, "fuzz_old_paths/status.md") ||
             strstr(md.data, "build/fuzz/all/status")) {
    (void)string_list_push_copy(errors, "old-path apply markdown lost custom status follow-up");
  }
  free(md.data);
  free(mode);
  free(nytrix_root);
  free(archive_run);
  free(markdown);
}

static void selftest_validate_fuzz_all_old_paths_dry_run(const char *json,
                                                         string_list_t *errors,
                                                         int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  selftest_expect_report_result_top_aliases(json, errors);
  selftest_validate_old_writer_classifier(errors);
  selftest_validate_old_paths_policy_fields(json, errors, false);
  if (*row_count != 6)
    (void)string_list_push_copy(errors, "old-path dry-run row count mismatch");
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-old-paths") != 0)
    (void)string_list_push_copy(errors, "old-path dry-run mode mismatch");
  bool dry_run = false, apply = true;
  if (!summary_bool_from_report(json, "dry_run", &dry_run) || !dry_run)
    (void)string_list_push_copy(errors, "old-path dry-run flag missing");
  if (!summary_bool_from_report(json, "apply", &apply) || apply)
    (void)string_list_push_copy(errors, "old-path dry-run reported apply");
  double present = -1.0, moved = -1.0, remaining = -1.0;
  if (!summary_number_from_report(json, "present_count", &present) ||
      present != 6.0)
    (void)string_list_push_copy(errors, "old-path dry-run present count mismatch");
  if (!summary_number_from_report(json, "moved_count", &moved) ||
      moved != 0.0)
    (void)string_list_push_copy(errors, "old-path dry-run moved count mismatch");
  if (!summary_number_from_report(json, "remaining_count", &remaining) ||
      remaining != 6.0)
    (void)string_list_push_copy(errors, "old-path dry-run remaining count mismatch");
  if (!summary_number_from_report(json, "old_path_present_count", &present) ||
      present != 6.0 ||
      !summary_number_from_report(json, "old_path_moved_count", &moved) ||
      moved != 0.0 ||
      !summary_number_from_report(json, "old_path_remaining_count",
                                  &remaining) ||
      remaining != 6.0)
    (void)string_list_push_copy(errors,
                                "old-path dry-run count aliases mismatch");
  double artifact_leaks = -1.0, artifact_moved = -1.0;
  double artifact_remaining = -1.0;
  if (!summary_number_from_report(json, "artifact_leak_count",
                                  &artifact_leaks) ||
      artifact_leaks != 3.0 ||
      !summary_number_from_report(json, "artifact_moved_count",
                                  &artifact_moved) ||
      artifact_moved != 0.0 ||
      !summary_number_from_report(json, "artifact_remaining_count",
                                  &artifact_remaining) ||
      artifact_remaining != 3.0)
    (void)string_list_push_copy(errors,
                                "old-path dry-run artifact counts mismatch");
  if (!strstr(json, "\"action\":\"would-archive\""))
    (void)string_list_push_copy(errors, "old-path dry-run action mismatch");

  char root[4096] = {0};
  (void)find_nytrix_root(root, sizeof(root));
  char *nytrix_root = summary_string_from_report(json, "nytrix_root");
  char *archive_dir = summary_string_from_report(json, "archive_dir");
  char *markdown = summary_string_from_report(json, "markdown");
  char ny_abs[4096] = {0}, md_abs[4096] = {0};
  if (nytrix_root && *nytrix_root) {
    if (path_is_absolute(nytrix_root)) snprintf(ny_abs, sizeof(ny_abs), "%s", nytrix_root);
    else (void)path_join(ny_abs, sizeof(ny_abs), root, nytrix_root);
  }
  if (markdown && *markdown) {
    if (path_is_absolute(markdown)) snprintf(md_abs, sizeof(md_abs), "%s", markdown);
    else (void)path_join(md_abs, sizeof(md_abs), root, markdown);
  }
  const char *old_rels[] = {"build/cache/projects/test", "fuzz", "build/cache"};
  for (size_t i = 0; i < sizeof(old_rels) / sizeof(old_rels[0]); ++i) {
    char p[4096] = {0};
    if (!path_join(p, sizeof(p), ny_abs, old_rels[i]) || !exists_path(p))
      (void)string_list_push_copy(errors, "old-path dry-run source missing after dry-run");
  }
  file_buf_t md = {0};
  if (!md_abs[0] || !read_file(md_abs, &md) || !md.data) {
    (void)string_list_push_copy(errors, "old-path dry-run markdown was not readable");
  } else if (!strstr(md.data, "Mode: `dry-run`") ||
             !strstr(md.data, "moved 0") ||
             !strstr(md.data, "remaining 6") ||
             !strstr(md.data, "Stale build-artifact leaks: `3`") ||
             !strstr(md.data, "stale-report-artifact") ||
             !strstr(md.data, "fuzz all old-paths --apply") ||
             !strstr(md.data, "--nytrix-root") ||
             !strstr(md.data, "fuzz_old_paths_dry/fake_nytrix") ||
             !strstr(md.data, "--archive-dir") ||
             !strstr(md.data, "fuzz_old_paths_dry/archive") ||
             !strstr(md.data, "fuzz_old_paths_dry/old-paths.md") ||
             strstr(md.data, "build/fuzz/all/old-paths")) {
    (void)string_list_push_copy(errors, "old-path dry-run markdown lost custom apply follow-up");
  }
  free(md.data);
  if (!archive_dir || !strstr(archive_dir, "fuzz_old_paths_dry/archive"))
    (void)string_list_push_copy(errors, "old-path dry-run archive dir mismatch");
  free(mode);
  free(nytrix_root);
  free(archive_dir);
  free(markdown);
}

static void selftest_validate_fuzz_all_old_paths_empty_dry_run(
  const char *json, string_list_t *errors, int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  selftest_expect_report_result_top_aliases(json, errors);
  selftest_validate_old_writer_classifier(errors);
  selftest_validate_old_paths_policy_fields(json, errors, true);
  if (*row_count != 3)
    (void)string_list_push_copy(errors,
                                "empty old-path dry-run row count mismatch");
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-old-paths") != 0)
    (void)string_list_push_copy(errors,
                                "empty old-path dry-run mode mismatch");
  bool dry_run = false, apply = true;
  if (!summary_bool_from_report(json, "dry_run", &dry_run) || !dry_run)
    (void)string_list_push_copy(errors,
                                "empty old-path dry-run flag missing");
  if (!summary_bool_from_report(json, "apply", &apply) || apply)
    (void)string_list_push_copy(errors,
                                "empty old-path dry-run reported apply");
  double present = -1.0, moved = -1.0, remaining = -1.0;
  if (!summary_number_from_report(json, "present_count", &present) ||
      present != 0.0)
    (void)string_list_push_copy(errors,
                                "empty old-path present count mismatch");
  if (!summary_number_from_report(json, "moved_count", &moved) ||
      moved != 0.0)
    (void)string_list_push_copy(errors,
                                "empty old-path moved count mismatch");
  if (!summary_number_from_report(json, "remaining_count", &remaining) ||
      remaining != 0.0)
    (void)string_list_push_copy(errors,
                                "empty old-path remaining count mismatch");
  if (!summary_number_from_report(json, "old_path_present_count", &present) ||
      present != 0.0 ||
      !summary_number_from_report(json, "old_path_moved_count", &moved) ||
      moved != 0.0 ||
      !summary_number_from_report(json, "old_path_remaining_count",
                                  &remaining) ||
      remaining != 0.0)
    (void)string_list_push_copy(
        errors, "empty old-path count aliases mismatch");
  double artifact_leaks = -1.0;
  if (!summary_number_from_report(json, "artifact_leak_count",
                                  &artifact_leaks) ||
      artifact_leaks != 0.0)
    (void)string_list_push_copy(errors,
                                "empty old-path artifact count mismatch");
  if (strstr(json, "\"action\":\"would-archive\""))
    (void)string_list_push_copy(errors,
                                "empty old-path dry-run offered archive rows");

  char root[4096] = {0};
  (void)find_nytrix_root(root, sizeof(root));
  char *markdown = summary_string_from_report(json, "markdown");
  char md_abs[4096] = {0};
  if (markdown && *markdown) {
    if (path_is_absolute(markdown)) snprintf(md_abs, sizeof(md_abs), "%s", markdown);
    else (void)path_join(md_abs, sizeof(md_abs), root, markdown);
  }
  file_buf_t md = {0};
  if (!md_abs[0] || !read_file(md_abs, &md) || !md.data) {
    (void)string_list_push_copy(errors,
                                "empty old-path markdown was not readable");
  } else if (!strstr(md.data, "Mode: `dry-run`") ||
             !strstr(md.data, "present 0") ||
             !strstr(md.data, "remaining 0") ||
             !strstr(md.data, "fuzz all status --refresh") ||
             !strstr(md.data, "fuzz_old_paths_empty/status.json") ||
             !strstr(md.data, "fuzz_old_paths_empty/status.md") ||
             strstr(md.data, "fuzz all old-paths --apply") ||
             strstr(md.data, "build/fuzz/all/status")) {
    (void)string_list_push_copy(errors,
                                "empty old-path markdown lost status follow-up");
  }
  free(md.data);
  free(mode);
  free(markdown);
}

static void selftest_validate_fuzz_all_old_writer_classifier(
    const char *json, string_list_t *errors, int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  selftest_expect_report_result_top_aliases(json, errors);
  selftest_validate_old_writer_classifier(errors);
  selftest_validate_old_paths_policy_fields(json, errors, true);
  if (*row_count != 3)
    (void)string_list_push_copy(errors,
                                "old-writer classifier fixture row count mismatch");
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-old-paths") != 0)
    (void)string_list_push_copy(errors,
                                "old-writer classifier fixture mode mismatch");
  bool dry_run = false, apply = true;
  if (!summary_bool_from_report(json, "dry_run", &dry_run) || !dry_run)
    (void)string_list_push_copy(errors,
                                "old-writer classifier fixture lost dry-run");
  if (!summary_bool_from_report(json, "apply", &apply) || apply)
    (void)string_list_push_copy(errors,
                                "old-writer classifier fixture reported apply");
  double present = -1.0, moved = -1.0, remaining = -1.0;
  if (!summary_number_from_report(json, "present_count", &present) ||
      present != 0.0)
    (void)string_list_push_copy(errors,
                                "old-writer classifier fixture present mismatch");
  if (!summary_number_from_report(json, "moved_count", &moved) ||
      moved != 0.0)
    (void)string_list_push_copy(errors,
                                "old-writer classifier fixture moved mismatch");
  if (!summary_number_from_report(json, "remaining_count", &remaining) ||
      remaining != 0.0)
    (void)string_list_push_copy(errors,
                                "old-writer classifier fixture remaining mismatch");
  double artifact_leaks = -1.0;
  if (!summary_number_from_report(json, "artifact_leak_count",
                                  &artifact_leaks) ||
      artifact_leaks != 0.0)
    (void)string_list_push_copy(errors,
                                "old-writer classifier artifact count mismatch");
  char *markdown = summary_string_from_report(json, "markdown");
  if (!markdown || !strstr(markdown, "fuzz_old_writer_classifier/old-paths.md"))
    (void)string_list_push_copy(errors,
                                "old-writer classifier fixture markdown path mismatch");
  free(markdown);
  free(mode);
}

static void selftest_validate_fuzz_all_progress(const char *json,
                                                string_list_t *errors,
                                                int *row_count,
                                                const char *expected_markdown,
                                                bool canonical_pair) {
  selftest_validate_standard_report(json, errors, row_count);
  selftest_validate_fuzz_all_top_aliases(json, errors);
  const char *expected_progress_md =
      expected_markdown && *expected_markdown ?
          expected_markdown : "fuzz_progress/progress.md";
  char expected_stop_file[512];
  char expected_state_file[512];
  char expected_old_path_report[512];
  char expected_old_path_markdown[512];
  const char *slash = strrchr(expected_progress_md, '/');
  if (slash) {
    size_t prefix_len = (size_t)(slash - expected_progress_md) + 1;
    if (prefix_len > sizeof(expected_stop_file) - 5)
      prefix_len = sizeof(expected_stop_file) - 5;
    memcpy(expected_stop_file, expected_progress_md, prefix_len);
    memcpy(expected_stop_file + prefix_len, "stop", 5);
    if (prefix_len > sizeof(expected_state_file) - 20)
      prefix_len = sizeof(expected_state_file) - 20;
    memcpy(expected_state_file, expected_progress_md, prefix_len);
    memcpy(expected_state_file + prefix_len, "run-next-state.json", 20);
    size_t old_prefix_len = (size_t)(slash - expected_progress_md) + 1;
    if (old_prefix_len > sizeof(expected_old_path_report) - 15)
      old_prefix_len = sizeof(expected_old_path_report) - 15;
    memcpy(expected_old_path_report, expected_progress_md, old_prefix_len);
    memcpy(expected_old_path_report + old_prefix_len, "old-paths.json", 15);
    if (old_prefix_len > sizeof(expected_old_path_markdown) - 13)
      old_prefix_len = sizeof(expected_old_path_markdown) - 13;
    memcpy(expected_old_path_markdown, expected_progress_md, old_prefix_len);
    memcpy(expected_old_path_markdown + old_prefix_len, "old-paths.md", 13);
  } else {
    snprintf(expected_stop_file, sizeof(expected_stop_file),
             "build/fuzz/all/stop");
    snprintf(expected_state_file, sizeof(expected_state_file),
             "build/fuzz/all/run-next-state.json");
    snprintf(expected_old_path_report, sizeof(expected_old_path_report),
             "build/fuzz/all/old-paths.json");
    snprintf(expected_old_path_markdown, sizeof(expected_old_path_markdown),
             "build/fuzz/all/old-paths.md");
  }
  double target_percent = 0.0;
  double blocker_count = 0.0;
  double active_items = 0.0;
  bool ready = false;
  bool target_reached = true;
  bool campaign_complete = true;
  bool strict = false;
  bool allow_incomplete_coverage = true;
  bool allow_full_pressure_remediation = false;
  bool status_refresh_attempted = true;
  bool cache_policy_ok = false;
  bool ny_bin_exists = false;
  bool old_test_absent = false;
  bool old_fuzz_absent = false;
  bool old_build_cache_absent = true;
  bool active_old_writer_present = true;
  if (!summary_number_from_report(json, "target_percent", &target_percent) ||
      target_percent != 12.5)
    (void)string_list_push_copy(errors, "progress target percent missing");
  double campaign_percent = 0.0;
  if (!summary_number_from_report(json, "campaign_percent",
                                  &campaign_percent) ||
      campaign_percent != 12.5)
    (void)string_list_push_copy(errors, "progress campaign percent missing");
  double campaign_confidence = 0.0;
  if (!summary_number_from_report(json, "campaign_confidence_percent",
                                  &campaign_confidence) ||
      campaign_confidence != 12.5)
    (void)string_list_push_copy(errors, "progress campaign confidence missing");
  if (!summary_bool_from_report(json, "ready", &ready) || !ready)
    (void)string_list_push_copy(errors, "progress ready flag missing");
  if (!summary_bool_from_report(json, "target_reached", &target_reached) ||
      target_reached)
    (void)string_list_push_copy(errors, "progress target reached flag wrong");
  if (!summary_bool_from_report(json, "campaign_complete",
                                &campaign_complete) ||
      campaign_complete)
    (void)string_list_push_copy(errors, "progress campaign complete flag wrong");
  if (!summary_bool_from_report(json, "strict", &strict) || !strict)
    (void)string_list_push_copy(errors, "progress strict flag missing");
  if (!summary_bool_from_report(json, "allow_incomplete_coverage",
                                &allow_incomplete_coverage) ||
      allow_incomplete_coverage)
    (void)string_list_push_copy(errors,
                                "progress incomplete-coverage flag wrong");
  if (!summary_bool_from_report(json, "allow_full_pressure_remediation",
                                &allow_full_pressure_remediation) ||
      !allow_full_pressure_remediation)
    (void)string_list_push_copy(errors,
                                "progress full-pressure remediation flag wrong");
  if (!summary_bool_from_report(json, "status_refresh_attempted",
                                &status_refresh_attempted) ||
      status_refresh_attempted)
    (void)string_list_push_copy(errors,
                                "progress status refresh attempted flag wrong");
  char *campaign_state = summary_string_from_report(json, "campaign_state");
  char *campaign_reason =
      summary_string_from_report(json, "campaign_incomplete_reason");
  char *completion_state = summary_string_from_report(json, "completion_state");
  char *completion_reason =
      summary_string_from_report(json, "completion_reason");
  if (!campaign_state ||
      strcmp(campaign_state, "ready-needs-evidence") != 0)
    (void)string_list_push_copy(errors, "progress campaign state wrong");
  if (!campaign_reason || strcmp(campaign_reason, "target-not-reached") != 0)
    (void)string_list_push_copy(errors,
                                "progress campaign incomplete reason wrong");
  if (!completion_state || !campaign_state ||
      strcmp(completion_state, campaign_state) != 0)
    (void)string_list_push_copy(errors,
                                "progress completion state alias wrong");
  if (!completion_reason || !campaign_reason ||
      strcmp(completion_reason, campaign_reason) != 0)
    (void)string_list_push_copy(errors,
                                "progress completion reason alias wrong");
  if (!summary_bool_from_report(json, "cache_policy_ok", &cache_policy_ok) ||
      !cache_policy_ok)
    (void)string_list_push_copy(errors, "progress cache policy missing");
  if (!summary_bool_from_report(json, "ny_bin_exists", &ny_bin_exists) ||
      !ny_bin_exists)
    (void)string_list_push_copy(errors, "progress Ny binary provenance missing");
  if (!summary_bool_from_report(json, "old_nytrix_test_scratch_absent",
                                &old_test_absent) ||
      !old_test_absent)
    (void)string_list_push_copy(errors, "progress old test scratch diagnostic wrong");
  if (!summary_bool_from_report(json, "old_nytrix_fuzz_absent",
                                &old_fuzz_absent) ||
      !old_fuzz_absent)
    (void)string_list_push_copy(errors, "progress old fuzz diagnostic wrong");
  if (!summary_bool_from_report(json, "old_nytrix_build_cache_absent",
                                &old_build_cache_absent) ||
      old_build_cache_absent)
    (void)string_list_push_copy(errors, "progress old build cache diagnostic wrong");
  if (!summary_bool_from_report(json,
                                "active_old_nytrix_output_writer_present",
                                &active_old_writer_present) ||
      active_old_writer_present)
    (void)string_list_push_copy(errors, "progress old writer diagnostic wrong");
  if (!summary_number_from_report(json, "blocker_count", &blocker_count) ||
      blocker_count != 0.0)
    (void)string_list_push_copy(errors, "progress blocker count wrong");
  if (!summary_number_from_report(json, "active_items", &active_items) ||
      active_items != 0.0)
    (void)string_list_push_copy(errors, "progress active item count wrong");
  double blockers_alias = -1.0, active_count_alias = -1.0;
  double active_runs_alias = -1.0;
  if (!summary_number_from_report(json, "blockers", &blockers_alias) ||
      blockers_alias != blocker_count)
    (void)string_list_push_copy(errors, "progress blockers alias wrong");
  if (!summary_number_from_report(json, "active_count",
                                  &active_count_alias) ||
      active_count_alias != active_items)
    (void)string_list_push_copy(errors, "progress active_count alias wrong");
  if (!summary_number_from_report(json, "active_runs", &active_runs_alias) ||
      active_runs_alias != active_items)
    (void)string_list_push_copy(errors, "progress active_runs alias wrong");
  double historical_attention = -1.0;
  if (!summary_number_from_report(json, "historical_attention_reports",
                                  &historical_attention) ||
      historical_attention != 5.0)
    (void)string_list_push_copy(errors, "progress historical attention count wrong");
  double stability_score = 0.0;
  if (!summary_number_from_report(json, "stability_score_percent",
                                  &stability_score) ||
      stability_score < 73.7 || stability_score > 73.8)
    (void)string_list_push_copy(errors, "progress stability score wrong");
  double score_alias = 0.0;
  if (!summary_number_from_report(json, "score", &score_alias) ||
      score_alias != stability_score)
    (void)string_list_push_copy(errors, "progress compact score alias wrong");
  double stability_alias = 0.0;
  if (!summary_number_from_report(json, "stability_score",
                                  &stability_alias) ||
      stability_alias != stability_score)
    (void)string_list_push_copy(errors,
                                "progress stability score alias wrong");
  double language_score = 0.0;
  if (!summary_number_from_report(json, "language_score_percent",
                                  &language_score) ||
      language_score < 73.7 || language_score > 73.8)
    (void)string_list_push_copy(errors, "progress language score wrong");
  double language_alias = 0.0;
  if (!summary_number_from_report(json, "language_score",
                                  &language_alias) ||
      language_alias != language_score)
    (void)string_list_push_copy(errors,
                                "progress language score alias wrong");
  double signal_health = 0.0, evidence_cap = 0.0;
  double language_signal = 0.0, language_cap = 0.0;
  if (!summary_number_from_report(json, "signal_health_percent",
                                  &signal_health) ||
      signal_health < 99.99 || signal_health > 100.01)
    (void)string_list_push_copy(errors, "progress signal health wrong");
  if (!summary_number_from_report(json, "evidence_cap_percent",
                                  &evidence_cap) ||
      evidence_cap < 73.7 || evidence_cap > 73.8)
    (void)string_list_push_copy(errors, "progress evidence cap wrong");
  if (!summary_number_from_report(json, "language_score_signal_percent",
                                  &language_signal) ||
      language_signal != signal_health)
    (void)string_list_push_copy(errors,
                                "progress language signal alias wrong");
  if (!summary_number_from_report(json,
                                  "language_score_evidence_cap_percent",
                                  &language_cap) ||
      language_cap != evidence_cap)
    (void)string_list_push_copy(errors,
                                "progress language cap alias wrong");
  double language_good_threshold = 0.0;
  if (!summary_number_from_report(json,
                                  "language_score_good_threshold_percent",
                                  &language_good_threshold) ||
      language_good_threshold < 74.99 || language_good_threshold > 75.01)
    (void)string_list_push_copy(errors, "progress language good threshold wrong");
  double percent_per_run = 0.0;
  double thread_years_per_run = 0.0;
  if (!summary_number_from_report(json, "thread_years_per_run",
                                  &thread_years_per_run) ||
      thread_years_per_run < 0.02199 || thread_years_per_run > 0.02201)
    (void)string_list_push_copy(errors, "progress thread-years-per-run wrong");
  char *thread_years_per_run_source =
      summary_string_from_report(json, "thread_years_per_run_source");
  if (!thread_years_per_run_source ||
      strcmp(thread_years_per_run_source, "plan-rate") != 0)
    (void)string_list_push_copy(errors, "progress per-run projection source wrong");
  if (!summary_number_from_report(json, "target_percent_per_run",
                                  &percent_per_run) ||
      percent_per_run < 0.2199 || percent_per_run > 0.2201)
    (void)string_list_push_copy(errors, "progress percent-per-run wrong");
  double next_stability = 0.0;
  if (!summary_number_from_report(json, "next_run_stability_score_percent",
                                  &next_stability) ||
      next_stability < 74.07 || next_stability > 74.09)
    (void)string_list_push_copy(errors, "progress next-run stability wrong");
  double next_stability_alias = 0.0;
  if (!summary_number_from_report(json, "next_run_stability_score",
                                  &next_stability_alias) ||
      next_stability_alias != next_stability)
    (void)string_list_push_copy(errors,
                                "progress next-run stability alias wrong");
  double next_language_score = 0.0;
  if (!summary_number_from_report(json, "next_run_language_score_percent",
                                  &next_language_score) ||
      next_language_score < 74.07 || next_language_score > 74.09)
    (void)string_list_push_copy(errors, "progress next-run language score wrong");
  double next_language_alias = 0.0;
  if (!summary_number_from_report(json, "next_run_language_score",
                                  &next_language_alias) ||
      next_language_alias != next_language_score)
    (void)string_list_push_copy(errors,
                                "progress next-run language alias wrong");
  double next_language_delta = 0.0;
  if (!summary_number_from_report(json, "next_run_language_score_delta_percent",
                                  &next_language_delta) ||
      next_language_delta < 0.32 || next_language_delta > 0.34)
    (void)string_list_push_copy(errors, "progress next-run language delta wrong");
  double runs_to_good = 0.0;
  if (!summary_number_from_report(json, "runs_to_good_stability",
                                  &runs_to_good) ||
      runs_to_good != 4.0)
    (void)string_list_push_copy(errors, "progress runs-to-good wrong");
  if (!summary_number_from_report(json, "runs_to_good_stability_score",
                                  &runs_to_good) ||
      runs_to_good != 4.0)
    (void)string_list_push_copy(errors,
                                "progress runs-to-good stability alias wrong");
  if (!summary_number_from_report(json, "runs_to_good_language_score",
                                  &runs_to_good) ||
      runs_to_good != 4.0)
    (void)string_list_push_copy(errors, "progress runs-to-good language wrong");
  char *recommended_repeat_mode =
      summary_string_from_report(json, "recommended_repeat_mode");
  double recommended_repeat_count = -1.0;
  if (!recommended_repeat_mode)
    (void)string_list_push_copy(errors,
                                "progress recommended repeat mode missing");
  if (!summary_number_from_report(json, "recommended_repeat_count",
                                  &recommended_repeat_count))
    (void)string_list_push_copy(errors,
                                "progress recommended repeat count missing");
  double days_to_good = 0.0;
  if (!summary_number_from_report(json, "runs_to_good_days",
                                  &days_to_good) ||
      days_to_good < 1.99 || days_to_good > 2.01)
    (void)string_list_push_copy(errors, "progress days-to-good wrong");
  if (!summary_number_from_report(json, "runs_to_good_stability_days",
                                  &days_to_good) ||
      days_to_good < 1.99 || days_to_good > 2.01)
    (void)string_list_push_copy(errors,
                                "progress days-to-good stability alias wrong");
  if (!summary_number_from_report(json, "days_to_good_stability",
                                  &days_to_good) ||
      days_to_good < 1.99 || days_to_good > 2.01)
    (void)string_list_push_copy(errors,
                                "progress days-to-good stability short alias wrong");
  if (!summary_number_from_report(json, "runs_to_good_language_days",
                                  &days_to_good) ||
      days_to_good < 1.99 || days_to_good > 2.01)
    (void)string_list_push_copy(errors, "progress language days-to-good wrong");
  if (!summary_number_from_report(json, "days_to_good_language_score",
                                  &days_to_good) ||
      days_to_good < 1.99 || days_to_good > 2.01)
    (void)string_list_push_copy(errors,
                                "progress language days-to-good short alias wrong");
  double status_age = -1.0;
  if (!summary_number_from_report(json, "status_age_seconds", &status_age) ||
      status_age < 0.0 || status_age > 60.0)
    (void)string_list_push_copy(errors, "progress status age seconds wrong");
  double status_age_minutes = -1.0;
  if (!summary_number_from_report(json, "status_age_minutes",
                                  &status_age_minutes) ||
      status_age_minutes < 0.0 || status_age_minutes > 1.0)
    (void)string_list_push_copy(errors, "progress status age minutes wrong");
  double latest_age = -1.0;
  if (!summary_number_from_report(json, "latest_report_age_seconds",
                                  &latest_age) ||
      latest_age < 0.0 || latest_age > 60.0)
    (void)string_list_push_copy(errors, "progress latest report age seconds wrong");
  if (!summary_number_from_report(json, "latest_report_age_hours",
                                  &latest_age) ||
      latest_age < 0.0 || latest_age > 0.02)
    (void)string_list_push_copy(errors, "progress latest report age hours wrong");
  if (!summary_number_from_report(json,
                                  "latest_full_pressure_report_age_seconds",
                                  &latest_age) ||
      latest_age < 0.0 || latest_age > 60.0)
    (void)string_list_push_copy(errors, "progress full-pressure report age seconds wrong");
  if (!summary_number_from_report(json,
                                  "latest_full_pressure_report_age_hours",
                                  &latest_age) ||
      latest_age < 0.0 || latest_age > 0.02)
    (void)string_list_push_copy(errors, "progress full-pressure report age hours wrong");
  if (!summary_number_from_report(json, "latest_report_stale_after_hours",
                                  &latest_age) ||
      latest_age != 24.0)
    (void)string_list_push_copy(errors, "progress latest report freshness threshold wrong");
  if (!summary_number_from_report(json,
                                  "latest_full_pressure_report_stale_after_hours",
                                  &latest_age) ||
      latest_age != 72.0)
    (void)string_list_push_copy(errors, "progress full-pressure freshness threshold wrong");
  double compact_latest = -1.0, compact_latest_over = -1.0;
  double compact_full = -1.0, compact_full_over = -1.0;
  double compact_over = -1.0;
  if (!summary_number_from_report(json, "latest_h", &compact_latest) ||
      compact_latest < 0.0 || compact_latest > 0.02)
    (void)string_list_push_copy(errors, "progress compact latest_h wrong");
  if (!summary_number_from_report(json, "latest_over_h",
                                  &compact_latest_over) ||
      compact_latest_over != 0.0)
    (void)string_list_push_copy(errors,
                                "progress compact latest_over_h wrong");
  if (!summary_number_from_report(json, "full_h", &compact_full) ||
      compact_full < 0.0 || compact_full > 0.02)
    (void)string_list_push_copy(errors, "progress compact full_h wrong");
  if (!summary_number_from_report(json, "full_over_h",
                                  &compact_full_over) ||
      compact_full_over != 0.0)
    (void)string_list_push_copy(errors,
                                "progress compact full_over_h wrong");
  if (!summary_number_from_report(json, "over_h", &compact_over) ||
      compact_over != 0.0)
    (void)string_list_push_copy(errors, "progress compact over_h wrong");
  bool latest_fresh = false, full_pressure_fresh = false, evidence_fresh = false;
  if (!summary_bool_from_report(json, "latest_report_fresh", &latest_fresh) ||
      !latest_fresh)
    (void)string_list_push_copy(errors, "progress latest report freshness flag wrong");
  if (!summary_bool_from_report(json, "latest_full_pressure_report_fresh",
                                &full_pressure_fresh) || !full_pressure_fresh)
    (void)string_list_push_copy(errors, "progress full-pressure freshness flag wrong");
  if (!summary_bool_from_report(json, "evidence_fresh", &evidence_fresh) ||
      !evidence_fresh)
    (void)string_list_push_copy(errors, "progress evidence freshness flag wrong");
  double campaign_remaining = -1.0;
  if (!summary_number_from_report(json, "campaign_remaining_percent",
                                  &campaign_remaining) ||
      campaign_remaining < 87.49 || campaign_remaining > 87.51)
    (void)string_list_push_copy(errors,
                                "progress campaign remaining percent wrong");
  double remaining_alias = -1.0;
  if (!summary_number_from_report(json, "remaining_percent",
                                  &remaining_alias) ||
      remaining_alias != campaign_remaining)
    (void)string_list_push_copy(errors,
                                "progress remaining percent alias wrong");
  double evidence_value = -1.0;
  if (!summary_number_from_report(json, "evidence_reports",
                                  &evidence_value) ||
      evidence_value != 8.0)
    (void)string_list_push_copy(errors, "progress evidence reports wrong");
  if (!summary_number_from_report(json, "reports", &evidence_value) ||
      evidence_value != 8.0)
    (void)string_list_push_copy(errors, "progress reports alias wrong");
  double ignored_no_evidence = -1.0;
  if (!summary_number_from_report(json, "ignored_no_evidence_reports",
                                  &ignored_no_evidence) ||
      ignored_no_evidence != 2.0)
    (void)string_list_push_copy(errors,
                                "progress ignored no-evidence count wrong");
  if (!summary_number_from_report(json,
                                  "evidence_ignored_no_evidence_reports",
                                  &evidence_value) ||
      evidence_value != ignored_no_evidence)
    (void)string_list_push_copy(errors,
                                "progress ignored no-evidence alias wrong");
  if (!summary_number_from_report(json, "evidence_full_pressure_reports",
                                  &evidence_value) ||
      evidence_value != 3.0)
    (void)string_list_push_copy(errors, "progress full-pressure reports wrong");
  if (!summary_number_from_report(json, "full_pressure_reports",
                                  &evidence_value) ||
      evidence_value != 3.0)
    (void)string_list_push_copy(errors,
                                "progress full-pressure reports alias wrong");
  if (!summary_number_from_report(json,
                                  "evidence_full_pressure_thread_years",
                                  &evidence_value) ||
      evidence_value < 1.124 || evidence_value > 1.126)
    (void)string_list_push_copy(errors, "progress full-pressure years wrong");
  if (!summary_number_from_report(json, "evidence_checked_subcases",
                                  &evidence_value) ||
      evidence_value != 640.0)
    (void)string_list_push_copy(errors, "progress checked subcases wrong");
  if (!summary_number_from_report(json, "checked_subcases",
                                  &evidence_value) ||
      evidence_value != 640.0)
    (void)string_list_push_copy(errors,
                                "progress checked subcases alias wrong");
  if (!summary_number_from_report(json, "evidence_coverage_ran_lanes",
                                  &evidence_value) ||
      evidence_value != 41.0)
    (void)string_list_push_copy(errors, "progress coverage ran lanes wrong");
  if (!summary_number_from_report(json, "evidence_coverage_lanes",
                                  &evidence_value) ||
      evidence_value != 45.0)
    (void)string_list_push_copy(errors, "progress coverage lanes wrong");
  if (!summary_number_from_report(json, "evidence_coverage_depth_percent",
                                  &evidence_value) ||
      evidence_value < 91.10 || evidence_value > 91.12)
    (void)string_list_push_copy(errors, "progress coverage depth wrong");
  if (!summary_number_from_report(json, "coverage_depth_percent",
                                  &evidence_value) ||
      evidence_value < 91.10 || evidence_value > 91.12)
    (void)string_list_push_copy(errors,
                                "progress compact coverage depth wrong");
  double coverage_percent_alias = -1.0;
  if (!summary_number_from_report(json, "coverage_percent",
                                  &coverage_percent_alias) ||
      coverage_percent_alias != evidence_value)
    (void)string_list_push_copy(errors,
                                "progress coverage percent alias wrong");
  if (!summary_number_from_report(json, "evidence_coverage_not_run_lanes",
                                  &evidence_value) ||
      evidence_value != 4.0)
    (void)string_list_push_copy(errors, "progress coverage not-run lanes wrong");
  if (!summary_number_from_report(json, "coverage_not_run_lanes",
                                  &evidence_value) ||
      evidence_value != 4.0)
    (void)string_list_push_copy(errors,
                                "progress compact coverage not-run lanes wrong");
  if (!summary_number_from_report(json, "coverage_skipped_lanes",
                                  &evidence_value) ||
      evidence_value != 4.0)
    (void)string_list_push_copy(errors,
                                "progress coverage skipped lanes wrong");
  if (!summary_number_from_report(json, "coverage_failed_lanes",
                                  &evidence_value) ||
      evidence_value != 0.0)
    (void)string_list_push_copy(errors,
                                "progress coverage failed lanes wrong");
  if (!summary_number_from_report(json, "coverage_disabled_lanes",
                                  &evidence_value) ||
      evidence_value != 3.0)
    (void)string_list_push_copy(errors,
                                "progress coverage disabled lanes wrong");
  if (!summary_number_from_report(json, "coverage_budget_short_lanes",
                                  &evidence_value) ||
      evidence_value != 1.0)
    (void)string_list_push_copy(errors,
                                "progress coverage budget-short lanes wrong");
  if (!summary_number_from_report(json, "coverage_missing_tool_lanes",
                                  &evidence_value) ||
      evidence_value != 0.0)
    (void)string_list_push_copy(errors,
                                "progress coverage missing-tool lanes wrong");
  if (!summary_number_from_report(json, "coverage_detail_count",
                                  &evidence_value) ||
      evidence_value < 0.0)
    (void)string_list_push_copy(errors,
                                "progress coverage detail count missing");
  if (!summary_number_from_report(json, "coverage_backlog_lanes",
                                  &evidence_value) ||
      evidence_value < 0.0)
    (void)string_list_push_copy(errors,
                                "progress coverage backlog lanes missing");
  if (!summary_number_from_report(json, "coverage_detail_rows",
                                  &evidence_value) ||
      evidence_value < 0.0)
    (void)string_list_push_copy(errors,
                                "progress coverage raw detail rows missing");
  double progress_queue_count = -1.0, progress_queue_primary = -1.0;
  double progress_queue_advisory = -1.0;
  if (!summary_number_from_report(json, "coverage_queue_count",
                                  &progress_queue_count) ||
      progress_queue_count != 2.0)
    (void)string_list_push_copy(errors,
                                "progress coverage queue count wrong");
  if (!summary_number_from_report(
          json, "coverage_queue_non_advisory_count",
          &progress_queue_primary) ||
      progress_queue_primary != 1.0)
    (void)string_list_push_copy(errors,
                                "progress primary coverage queue wrong");
  if (!summary_number_from_report(json, "coverage_queue_advisory_count",
                                  &progress_queue_advisory) ||
      progress_queue_advisory != 1.0)
    (void)string_list_push_copy(errors,
                                "progress advisory coverage queue wrong");
  char *progress_queue_lanes =
      summary_string_from_report(json, "coverage_queue_lanes");
  char *progress_queue_json =
      summary_array_from_report(json, "coverage_queue");
  if (!progress_queue_lanes ||
      !strstr(progress_queue_lanes, "afl -> nytrix_fuzz"))
    (void)string_list_push_copy(errors,
                                "progress coverage queue lanes missing");
  if (!progress_queue_json ||
      count_json_array_items(progress_queue_json) != 2 ||
      !strstr(progress_queue_json, "\"lane\":\"afl\"") ||
      !strstr(progress_queue_json, "\"lane\":\"nytrix_fuzz\"") ||
      !strstr(progress_queue_json, "--allow-nytrix"))
    (void)string_list_push_copy(errors,
                                "progress coverage queue array missing");
  free(progress_queue_lanes);
  free(progress_queue_json);
  if (!summary_number_from_report(json, "coverage_advisory_gaps",
                                  &evidence_value) ||
      evidence_value != 0.0)
    (void)string_list_push_copy(errors,
                                "progress coverage advisory gaps wrong");
  if (!summary_number_from_report(json, "coverage_reports_considered",
                                  &evidence_value) ||
      evidence_value != 9.0)
    (void)string_list_push_copy(errors,
                                "progress coverage report count wrong");
  if (!summary_number_from_report(json,
                                  "coverage_campaign_reports_considered",
                                  &evidence_value) ||
      evidence_value != 8.0)
    (void)string_list_push_copy(errors,
                                "progress coverage campaign report count wrong");
  if (!summary_number_from_report(json,
                                  "coverage_companion_reports_considered",
                                  &evidence_value) ||
      evidence_value != 1.0)
    (void)string_list_push_copy(errors,
                                "progress coverage companion report count wrong");
  if (!summary_number_from_report(json,
                                  "coverage_latest_report_advisory_gaps",
                                  &evidence_value) ||
      evidence_value != 1.0)
    (void)string_list_push_copy(errors,
                                "progress coverage latest advisory count wrong");
  if (!summary_number_from_report(
          json, "coverage_latest_report_companion_skipped_lanes",
          &evidence_value) ||
      evidence_value != 1.0)
    (void)string_list_push_copy(
        errors, "progress coverage latest companion skip count wrong");
  char *coverage_state = summary_string_from_report(json, "coverage_state");
  if (!coverage_state || strcmp(coverage_state, "partial") != 0)
    (void)string_list_push_copy(errors, "progress coverage state wrong");
  char *progress_coverage_next_action =
      summary_string_from_report(json, "coverage_next_action");
  char *progress_coverage_next_lane =
      summary_string_from_report(json, "coverage_next_lane");
  char *progress_coverage_next_command =
      summary_string_from_report(json, "coverage_next_command");
  char *progress_coverage_next_guarded =
      summary_string_from_report(json, "coverage_next_guarded_command");
  char *progress_coverage_next_low_cpu =
      summary_string_from_report(json, "coverage_next_low_cpu_command");
  char *progress_coverage_next_preview =
      summary_string_from_report(json, "coverage_next_preview_command");
  char *progress_coverage_next_state_file =
      summary_string_from_report(json, "coverage_next_state_file");
  char *progress_coverage_next_state_command =
      summary_string_from_report(json, "coverage_next_state_command");
  char *progress_coverage_next_state_refresh =
      summary_string_from_report(json,
                                 "coverage_next_state_refresh_command");
  bool progress_coverage_next_state_refresh_required = false;
  bool have_progress_coverage_next_state_refresh_required =
      summary_bool_from_report(json,
                               "coverage_next_state_refresh_required",
                               &progress_coverage_next_state_refresh_required);
  char *progress_coverage_next_state_refresh_reason =
      summary_string_from_report(json,
                                 "coverage_next_state_refresh_reason");
  char *progress_recommended_state_refresh_command =
      summary_string_from_report(json, "recommended_state_refresh_command");
  char *progress_coverage_next_state =
      summary_string_from_report(json, "coverage_next_state");
  char *progress_coverage_next_state_stale_reason =
      summary_string_from_report(json, "coverage_next_state_stale_reason");
  char *progress_coverage_next_state_phase =
      summary_string_from_report(json, "coverage_next_state_phase");
  char *progress_coverage_next_state_child_status =
      summary_string_from_report(json, "coverage_next_state_child_status");
  char *progress_coverage_next_stop_file =
      summary_string_from_report(json, "coverage_next_stop_file");
  char *progress_coverage_next_stop_command =
      summary_string_from_report(json, "coverage_next_stop_command");
  char *progress_coverage_next_resume_command =
      summary_string_from_report(json, "coverage_next_resume_command");
  bool progress_has_coverage_next =
      progress_coverage_next_action &&
      strcmp(progress_coverage_next_action, "none") != 0;
  bool expected_progress_state_refresh_required = false;
    if (progress_has_coverage_next) {
    if (strcmp(progress_coverage_next_action,
               "run-missing-evidence") != 0)
      (void)string_list_push_copy(errors,
                                  "progress coverage next action wrong");
    if (!progress_coverage_next_lane || !*progress_coverage_next_lane)
      (void)string_list_push_copy(errors,
                                  "progress coverage next lane missing");
    if (!progress_coverage_next_command ||
        !strstr(progress_coverage_next_command, "./build/"))
      (void)string_list_push_copy(errors,
                                  "progress coverage next command missing");
    if (progress_coverage_next_command &&
        strstr(progress_coverage_next_command, "fuzz all run") &&
        progress_coverage_next_lane &&
        strcmp(progress_coverage_next_lane, "afl") == 0 &&
        !strstr(progress_coverage_next_command, "--only-lane afl"))
      (void)string_list_push_copy(errors,
                                  "progress AFL coverage next command was not focused");
    if (progress_coverage_next_command &&
        strstr(progress_coverage_next_command, "fuzz all run") &&
        (!progress_coverage_next_guarded ||
         !strstr(progress_coverage_next_guarded,
                 "run-missing-evidence.sh")))
      (void)string_list_push_copy(errors,
                                  "progress coverage next guarded command missing");
       if (progress_coverage_next_command &&
           strstr(progress_coverage_next_command, "fuzz all run") &&
           (!progress_coverage_next_low_cpu ||
            !selftest_command_uses_env_nice(progress_coverage_next_low_cpu) ||
            !strstr(progress_coverage_next_low_cpu,
                    "NYTRIX_MISSING_EVIDENCE_HOURS=1") ||
            !strstr(progress_coverage_next_low_cpu,
                 "NYTRIX_MISSING_EVIDENCE_THREADS=10%") ||
         !strstr(progress_coverage_next_low_cpu,
                 "run-missing-evidence.sh")))
      (void)string_list_push_copy(errors,
                                  "progress coverage next low-cpu command missing");
    if (progress_coverage_next_command &&
        strstr(progress_coverage_next_command, "fuzz all run") &&
        (!progress_coverage_next_state_file ||
    !strstr(progress_coverage_next_state_file,
            "run-missing-evidence-state.json") ||
    !progress_coverage_next_state_command ||
    !strstr(progress_coverage_next_state_command,
            "jq {state,event,live,fresh,child_status,") ||
            !strstr(progress_coverage_next_state_command,
               "run-missing-evidence-state.json") ||
            !progress_coverage_next_state_refresh ||
            !selftest_command_uses_env_nice(
                progress_coverage_next_state_refresh) ||
            !strstr(progress_coverage_next_state_refresh,
                    "NYTRIX_RUN_DRY_RUN=1") ||
            !strstr(progress_coverage_next_state_refresh,
                 "run-missing-evidence.sh") ||
         !progress_coverage_next_stop_file ||
         !strstr(progress_coverage_next_stop_file,
                 "missing-evidence-stop") ||
         !progress_coverage_next_stop_command ||
         !strstr(progress_coverage_next_stop_command, "touch ") ||
         !strstr(progress_coverage_next_stop_command,
                 "missing-evidence-stop") ||
         !progress_coverage_next_resume_command ||
         !strstr(progress_coverage_next_resume_command, "rm -f ") ||
         !strstr(progress_coverage_next_resume_command,
                 "missing-evidence-stop")))
      (void)string_list_push_copy(
          errors, "progress coverage next state or pause command missing");
    if (progress_coverage_next_command &&
        strstr(progress_coverage_next_command, "fuzz all run") &&
        (!progress_coverage_next_state ||
         !*progress_coverage_next_state ||
         !progress_coverage_next_state_stale_reason ||
         !*progress_coverage_next_state_stale_reason ||
         !progress_coverage_next_state_child_status ||
         !*progress_coverage_next_state_child_status ||
         !progress_coverage_next_state_phase))
      (void)string_list_push_copy(
          errors, "progress coverage next parsed state fields missing");
    bool progress_coverage_next_state_live = false;
    bool have_progress_coverage_next_state_live =
        summary_bool_from_report(json, "coverage_next_state_live",
                                 &progress_coverage_next_state_live);
    expected_progress_state_refresh_required =
        progress_coverage_next_command &&
        strstr(progress_coverage_next_command, "fuzz all run") &&
        progress_coverage_next_state_refresh &&
        *progress_coverage_next_state_refresh &&
        (!have_progress_coverage_next_state_live ||
         !progress_coverage_next_state_live) &&
        progress_coverage_next_state_stale_reason &&
        *progress_coverage_next_state_stale_reason &&
        strcmp(progress_coverage_next_state_stale_reason, "none") != 0;
    if (expected_progress_state_refresh_required &&
        (!have_progress_coverage_next_state_refresh_required ||
         !progress_coverage_next_state_refresh_required ||
         !progress_coverage_next_state_refresh_reason ||
         strcmp(progress_coverage_next_state_refresh_reason,
                progress_coverage_next_state_stale_reason) != 0 ||
         !progress_recommended_state_refresh_command ||
         !strstr(progress_recommended_state_refresh_command,
                 progress_coverage_next_state_refresh)))
      (void)string_list_push_copy(
          errors, "progress coverage next state refresh requirement missing");
    if (progress_coverage_next_command &&
        strstr(progress_coverage_next_command, "fuzz all run") &&
           (!progress_coverage_next_preview ||
            !strstr(progress_coverage_next_preview, "fuzz all preflight") ||
            (progress_coverage_next_lane &&
             strcmp(progress_coverage_next_lane, "afl") == 0 &&
             !strstr(progress_coverage_next_preview, "--only-lane afl")) ||
            !selftest_command_uses_env_nice(progress_coverage_next_preview) ||
            !strstr(progress_coverage_next_preview,
                    "--allow-dirty-nytrix-baseline") ||
            !strstr(progress_coverage_next_preview, "build/cache/scratch")))
        (void)string_list_push_copy(errors,
                                    "progress coverage next preview missing");
    }
  bool progress_compiler_std_audit_readable = false;
  char *progress_compiler_std_audit_report =
      summary_string_from_report(json, "compiler_std_audit_report");
  char *progress_compiler_std_audit_markdown =
      summary_string_from_report(json, "compiler_std_audit_markdown");
  char *progress_compiler_std_audit_command =
      summary_string_from_report(json, "compiler_std_audit_command");
  char *progress_runtime_state =
      summary_string_from_report(json, "runtime_surface_state");
  char *progress_crt_state =
      summary_string_from_report(json, "crt_surface_state");
  char *progress_runtime_scope =
      summary_string_from_report(json, "runtime_surface_scope");
  char *progress_crt_scope =
      summary_string_from_report(json, "crt_surface_scope");
  char *progress_crt_behavior_state =
      summary_string_from_report(json, "crt_behavior_state");
  char *progress_crt_behavior_scope =
      summary_string_from_report(json, "crt_behavior_scope");
  char *progress_crt_behavior_next_action =
      summary_string_from_report(json, "crt_behavior_next_action");
  char *progress_crt_behavior_next_reason =
      summary_string_from_report(json, "crt_behavior_next_reason");
  char *progress_crt_behavior_next_command =
      summary_string_from_report(json, "crt_behavior_next_command");
  char *progress_crt_top_family =
      summary_string_from_report(json, "crt_top_unreferenced_family");
  char *progress_crt_families =
      summary_array_from_report(json, "crt_unreferenced_families");
  char *progress_crt_next_action =
      summary_string_from_report(json, "crt_next_action");
  char *progress_crt_next_reason =
      summary_string_from_report(json, "crt_next_reason");
  char *progress_crt_next_family =
      summary_string_from_report(json, "crt_next_unreferenced_family");
  char *progress_crt_next_exports =
      summary_array_from_report(json, "crt_next_unreferenced_exports");
  char *progress_crt_next_definition_file =
      summary_string_from_report(json, "crt_next_definition_file");
  char *progress_crt_next_definition_locations =
      summary_array_from_report(json, "crt_next_definition_locations");
  char *progress_crt_next_inspect_command =
      summary_string_from_report(json, "crt_next_inspect_command");
  double progress_runtime_exports = -1.0;
  double progress_direct_runtime_refs = -1.0;
  double progress_runtime_coverage_done = -1.0;
  double progress_runtime_coverage_total = -1.0;
  double progress_runtime_coverage = -1.0;
  double progress_crt_runtime_exports = -1.0;
  double progress_crt_direct_refs = -1.0;
  double progress_crt_coverage_done = -1.0;
  double progress_crt_coverage_total = -1.0;
  double progress_crt_coverage = -1.0;
  double progress_crt_unreferenced = -1.0;
  double progress_crt_unreferenced_percent = -1.0;
  double progress_crt_family_count = -1.0;
  double progress_crt_top_family_count = -1.0;
  double progress_crt_next_count = -1.0;
  if (!summary_bool_from_report(json, "compiler_std_audit_readable",
                                &progress_compiler_std_audit_readable))
    (void)string_list_push_copy(
        errors, "progress compiler std-audit readable flag missing");
  if (!progress_compiler_std_audit_report ||
      !strstr(progress_compiler_std_audit_report, "compiler-std-audit.json"))
    (void)string_list_push_copy(errors,
                                "progress compiler std-audit report path missing");
  if (!progress_compiler_std_audit_markdown ||
      !strstr(progress_compiler_std_audit_markdown, "compiler-std-audit.md"))
    (void)string_list_push_copy(
        errors, "progress compiler std-audit markdown path missing");
  if (!progress_compiler_std_audit_command ||
      !selftest_command_uses_env_nice(progress_compiler_std_audit_command) ||
      !strstr(progress_compiler_std_audit_command,
              "compiler std-audit --json") ||
      !strstr(progress_compiler_std_audit_command,
              "compiler-std-audit.json") ||
      !strstr(progress_compiler_std_audit_command, "compiler-std-audit.md"))
    (void)string_list_push_copy(errors,
                                "progress compiler std-audit command missing low-priority guard");
  if (!progress_runtime_state || !*progress_runtime_state ||
      !progress_crt_state || !*progress_crt_state)
    (void)string_list_push_copy(errors,
                                "progress compiler std-audit states missing");
  if (!progress_runtime_scope ||
      strcmp(progress_runtime_scope, NYTRIX_RUNTIME_SURFACE_SCOPE) != 0 ||
      !progress_crt_scope ||
      strcmp(progress_crt_scope, NYTRIX_CRT_SURFACE_SCOPE) != 0 ||
      !progress_crt_behavior_state ||
      strcmp(progress_crt_behavior_state, NYTRIX_CRT_BEHAVIOR_STATE) != 0 ||
      !progress_crt_behavior_scope ||
      strcmp(progress_crt_behavior_scope, NYTRIX_CRT_BEHAVIOR_SCOPE) != 0)
    (void)string_list_push_copy(errors,
                                "progress CRT claim scope missing");
  if (!progress_crt_behavior_next_action ||
      strcmp(progress_crt_behavior_next_action,
             NYTRIX_CRT_BEHAVIOR_NEXT_ACTION) != 0 ||
      !progress_crt_behavior_next_reason ||
      !strstr(progress_crt_behavior_next_reason, "campaign-gated") ||
      !progress_crt_behavior_next_command ||
      !selftest_command_uses_env_nice(progress_crt_behavior_next_command) ||
      !strstr(progress_crt_behavior_next_command,
              "./build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "progress CRT behavior next action missing");
  if (!summary_number_from_report(json, "runtime_export_coverage_percent",
                                  &progress_runtime_coverage) ||
      progress_runtime_coverage < 0.0)
    (void)string_list_push_copy(errors,
                                "progress runtime coverage percent missing");
  bool progress_runtime_counts_ok =
      summary_number_from_report(json, "runtime_exports",
                                 &progress_runtime_exports) &&
      summary_number_from_report(json, "direct_runtime_refs",
                                 &progress_direct_runtime_refs) &&
      summary_number_from_report(json, "runtime_coverage_done",
                                 &progress_runtime_coverage_done) &&
      summary_number_from_report(json, "runtime_coverage_total",
                                 &progress_runtime_coverage_total);
  if (!progress_runtime_counts_ok ||
      progress_runtime_coverage_done != progress_direct_runtime_refs ||
      progress_runtime_coverage_total != progress_runtime_exports ||
      progress_direct_runtime_refs < 0.0 ||
      progress_runtime_exports < 0.0 ||
      progress_direct_runtime_refs > progress_runtime_exports ||
      (progress_compiler_std_audit_readable &&
       progress_runtime_exports <= 0.0))
    (void)string_list_push_copy(errors,
                                "progress runtime coverage counts missing");
  if (!summary_number_from_report(json, "crt_export_coverage_percent",
                                  &progress_crt_coverage) ||
      progress_crt_coverage < 0.0)
    (void)string_list_push_copy(errors,
                                "progress CRT coverage percent missing");
  bool progress_crt_counts_ok =
      summary_number_from_report(json, "crt_runtime_exports",
                                 &progress_crt_runtime_exports) &&
      summary_number_from_report(json, "crt_direct_refs",
                                 &progress_crt_direct_refs) &&
      summary_number_from_report(json, "crt_coverage_done",
                                 &progress_crt_coverage_done) &&
      summary_number_from_report(json, "crt_coverage_total",
                                 &progress_crt_coverage_total);
  if (!progress_crt_counts_ok ||
      progress_crt_coverage_done != progress_crt_direct_refs ||
      progress_crt_coverage_total != progress_crt_runtime_exports ||
      progress_crt_direct_refs < 0.0 ||
      progress_crt_runtime_exports < 0.0 ||
      progress_crt_direct_refs > progress_crt_runtime_exports ||
      (progress_compiler_std_audit_readable &&
       progress_crt_runtime_exports <= 0.0))
    (void)string_list_push_copy(errors,
                                "progress CRT coverage counts missing");
  if (!summary_number_from_report(json, "crt_unreferenced_count",
                                  &progress_crt_unreferenced) ||
      progress_crt_unreferenced < 0.0)
    (void)string_list_push_copy(errors,
                                "progress CRT unreferenced count missing");
  if (!summary_number_from_report(json, "crt_unreferenced_percent",
                                  &progress_crt_unreferenced_percent) ||
      progress_crt_unreferenced_percent < 0.0)
    (void)string_list_push_copy(errors,
                                "progress CRT unreferenced percent missing");
  if (progress_crt_unreferenced > 0.0 &&
      (!progress_runtime_state ||
       strcmp(progress_runtime_state, "partial") != 0 ||
       !progress_crt_state || strcmp(progress_crt_state, "partial") != 0))
    (void)string_list_push_copy(errors,
                                "progress CRT surface state overclaimed clean");
  if (!summary_number_from_report(json, "crt_unreferenced_family_count",
                                  &progress_crt_family_count) ||
      progress_crt_family_count < 0.0)
    (void)string_list_push_copy(errors,
                                "progress CRT family count missing");
  if (!summary_number_from_report(json,
                                  "crt_top_unreferenced_family_count",
                                  &progress_crt_top_family_count) ||
      progress_crt_top_family_count < 0.0)
    (void)string_list_push_copy(errors,
                                "progress CRT top family count missing");
  if (progress_compiler_std_audit_readable &&
      progress_crt_family_count > 0.0 &&
      (!progress_crt_top_family || !*progress_crt_top_family ||
       !progress_crt_families ||
       !strstr(progress_crt_families, "\"family\"") ||
       !strstr(progress_crt_families, "\"count\"")))
    (void)string_list_push_copy(errors,
                                "progress CRT family summary missing");
  if (progress_compiler_std_audit_readable &&
      progress_crt_family_count > 0.0 &&
      (!progress_crt_next_action ||
       strcmp(progress_crt_next_action, "cover-unreferenced-family") != 0 ||
       !progress_crt_next_reason ||
       !strstr(progress_crt_next_reason, "largest") ||
       !progress_crt_next_family || !*progress_crt_next_family ||
       (progress_crt_top_family && *progress_crt_top_family &&
        strcmp(progress_crt_next_family, progress_crt_top_family) != 0) ||
       !summary_number_from_report(json, "crt_next_unreferenced_count",
                                   &progress_crt_next_count) ||
       progress_crt_next_count != progress_crt_top_family_count ||
       !progress_crt_next_exports ||
       !strstr(progress_crt_next_exports, "\"__") ||
       !progress_crt_next_definition_file ||
       !strstr(progress_crt_next_definition_file, "src/rt/defs.h") ||
       !progress_crt_next_definition_locations ||
       !strstr(progress_crt_next_definition_locations, "\"line\"") ||
       !strstr(progress_crt_next_definition_locations, "\"signature\"") ||
       !progress_crt_next_inspect_command ||
       !strstr(progress_crt_next_inspect_command, "sed -n") ||
       !strstr(progress_crt_next_inspect_command, "rg -n")))
    (void)string_list_push_copy(errors,
                                "progress CRT next action missing");
  if (progress_crt_families &&
      (strstr(progress_crt_families, "/home/e/nytrix/build/cache/projects/test") ||
       strstr(progress_crt_families, "/home/e/nytrix/fuzz")))
    (void)string_list_push_copy(errors,
                                "progress CRT family summary leaked stale paths");
  free(progress_compiler_std_audit_report);
  free(progress_compiler_std_audit_markdown);
  free(progress_compiler_std_audit_command);
  free(progress_runtime_state);
  free(progress_crt_state);
  free(progress_runtime_scope);
  free(progress_crt_scope);
  free(progress_crt_behavior_state);
  free(progress_crt_behavior_scope);
  free(progress_crt_behavior_next_action);
  free(progress_crt_behavior_next_reason);
  free(progress_crt_behavior_next_command);
  free(progress_crt_top_family);
  free(progress_crt_families);
  free(progress_crt_next_action);
  free(progress_crt_next_reason);
  free(progress_crt_next_family);
  free(progress_crt_next_exports);
  free(progress_crt_next_definition_file);
  free(progress_crt_next_definition_locations);
  free(progress_crt_next_inspect_command);
  double finding_count = -1.0;
  if (!summary_number_from_report(json, "correctness_findings",
                                  &finding_count) ||
      finding_count != 0.0)
    (void)string_list_push_copy(errors, "progress correctness findings wrong");
  if (!summary_number_from_report(json, "compiler_findings", &finding_count) ||
      finding_count != 0.0)
    (void)string_list_push_copy(errors, "progress compiler findings wrong");
  if (!summary_number_from_report(json, "known_bug_replay_findings",
                                  &finding_count) ||
      finding_count != 0.0)
    (void)string_list_push_copy(errors, "progress known-bug replay findings wrong");
  if (!summary_number_from_report(json, "perf_hotspots_open",
                                  &finding_count) ||
      finding_count != 0.0)
    (void)string_list_push_copy(errors, "progress perf hotspots wrong");
  if (!summary_number_from_report(json, "perf_worst_ratio", &finding_count) ||
      finding_count < 1.249 || finding_count > 1.251)
    (void)string_list_push_copy(errors, "progress perf worst ratio wrong");
  double progress_perf_slowdown = -1.0;
  if (!summary_number_from_report(json, "perf_worst_slowdown_percent",
                                  &progress_perf_slowdown) ||
      progress_perf_slowdown < 24.99 || progress_perf_slowdown > 25.01)
    (void)string_list_push_copy(errors, "progress perf worst slowdown wrong");
  if (!summary_number_from_report(json, "perf_watchlist_open", &finding_count) ||
      finding_count != 1.0)
    (void)string_list_push_copy(errors, "progress perf watchlist count wrong");
  if (!summary_number_from_report(json, "perf_watchlist_threshold_ratio",
                                  &finding_count) ||
      finding_count < 1.249 || finding_count > 1.251)
    (void)string_list_push_copy(errors, "progress perf watchlist threshold wrong");
  char *perf_case = summary_string_from_report(json, "perf_worst_case");
  if (!perf_case || strcmp(perf_case, "fixture-poly") != 0)
    (void)string_list_push_copy(errors, "progress perf worst case wrong");
  char *perf_watchlist_case =
      summary_string_from_report(json, "perf_watchlist_case");
  if (!perf_watchlist_case ||
      strcmp(perf_watchlist_case, "fixture-poly") != 0)
    (void)string_list_push_copy(errors, "progress perf watchlist case wrong");
  char *perf_watchlist_command =
      summary_string_from_report(json, "perf_watchlist_command");
  if (!perf_watchlist_command ||
      !selftest_command_uses_env_nice(perf_watchlist_command) ||
      !strstr(perf_watchlist_command, "perf triage --fast") ||
      !strstr(perf_watchlist_command, "--threshold 1.25") ||
      !strstr(perf_watchlist_command, "perf-watchlist.json") ||
      !strstr(perf_watchlist_command, "--markdown") ||
      !strstr(perf_watchlist_command, "perf-watchlist.md"))
    (void)string_list_push_copy(errors,
                                "progress perf watchlist command wrong");
  char *perf_watchlist_state =
      summary_string_from_report(json, "perf_watchlist_state");
  if (!fuzz_all_perf_watchlist_state_valid(perf_watchlist_state))
    (void)string_list_push_copy(errors,
                                "progress perf watchlist state wrong");
  char *perf_watchlist_action =
      summary_string_from_report(json, "perf_watchlist_action");
  char *perf_watchlist_action_command =
      summary_string_from_report(json, "perf_watchlist_action_command");
  char *optimization_action =
      summary_string_from_report(json, "optimization_action");
  char *optimization_reason =
      summary_string_from_report(json, "optimization_reason");
    char *optimization_command =
        summary_string_from_report(json, "optimization_command");
  char *optimization_target_command =
      summary_string_from_report(json, "optimization_target_command");
      char *optimization_case =
        summary_string_from_report(json, "optimization_case");
  char *optimization_artifact =
      summary_string_from_report(json, "optimization_artifact");
  char *optimization_ny_source =
      summary_string_from_report(json, "optimization_ny_source");
  char *optimization_c_source =
      summary_string_from_report(json, "optimization_c_source");
    double optimization_ratio = -1.0;
  double optimization_slowdown = -1.0;
  if (!fuzz_all_perf_watchlist_action_valid(perf_watchlist_action))
    (void)string_list_push_copy(errors,
                                "progress perf watchlist action wrong");
  if (!fuzz_all_perf_watchlist_action_valid(optimization_action) ||
      (perf_watchlist_action && optimization_action &&
       strcmp(optimization_action, perf_watchlist_action) != 0))
    (void)string_list_push_copy(errors,
                                "progress optimization action wrong");
  if (!optimization_reason || !*optimization_reason)
    (void)string_list_push_copy(errors,
                                "progress optimization reason missing");
  if (!optimization_command ||
      (perf_watchlist_action_command &&
       strcmp(optimization_command, perf_watchlist_action_command) != 0))
    (void)string_list_push_copy(errors,
                                "progress optimization command wrong");
  if (!optimization_case || strcmp(optimization_case, "fixture-poly") != 0)
    (void)string_list_push_copy(errors,
                                "progress optimization case wrong");
  if (!strstr(json, "\"optimization_target_command\""))
    (void)string_list_push_copy(errors,
                                "progress optimization target command missing");
  if (!strstr(json, "\"optimization_artifact\"") ||
      !strstr(json, "\"optimization_ny_source\"") ||
      !strstr(json, "\"optimization_c_source\""))
    (void)string_list_push_copy(errors,
                                "progress optimization target file fields missing");
  if (!summary_number_from_report(json, "optimization_ratio",
                                  &optimization_ratio) ||
      optimization_ratio < 1.249 || optimization_ratio > 1.251)
    (void)string_list_push_copy(errors,
                                "progress optimization ratio wrong");
  if (!summary_number_from_report(json, "optimization_slowdown_percent",
                                  &optimization_slowdown) ||
      optimization_slowdown < 24.99 || optimization_slowdown > 25.01)
    (void)string_list_push_copy(errors,
                                "progress optimization slowdown wrong");
  if (perf_watchlist_action &&
      strcmp(perf_watchlist_action, "inspect-watchlist") == 0 &&
      (!perf_watchlist_action_command ||
       !strstr(perf_watchlist_action_command, "perf-watchlist.md")))
    (void)string_list_push_copy(errors,
                                "progress perf watchlist inspect command wrong");
  if (perf_watchlist_action &&
      strcmp(perf_watchlist_action, "refresh-watchlist") == 0 &&
      (!perf_watchlist_action_command ||
       !selftest_command_uses_env_nice(perf_watchlist_action_command) ||
       !strstr(perf_watchlist_action_command, "perf triage --fast")))
    (void)string_list_push_copy(errors,
                                "progress perf watchlist refresh command wrong");
  char *perf_watchlist_report =
      summary_string_from_report(json, "perf_watchlist_report");
  char *perf_watchlist_markdown =
      summary_string_from_report(json, "perf_watchlist_markdown");
  bool perf_watchlist_artifact_readable = false;
  bool perf_watchlist_artifact_fresh = false;
  double perf_watchlist_artifact_age = -2.0;
  double perf_watchlist_artifact_stale_after = -1.0;
  if (!perf_watchlist_report ||
      !strstr(perf_watchlist_report, "perf-watchlist.json"))
    (void)string_list_push_copy(errors,
                                "progress perf watchlist report path wrong");
  if (!perf_watchlist_markdown ||
      !strstr(perf_watchlist_markdown, "perf-watchlist.md"))
    (void)string_list_push_copy(errors,
                                "progress perf watchlist markdown path wrong");
  if (!summary_bool_from_report(json, "perf_watchlist_artifact_readable",
                                &perf_watchlist_artifact_readable))
    (void)string_list_push_copy(
        errors, "progress perf watchlist artifact readable flag missing");
  if (!summary_bool_from_report(json, "perf_watchlist_artifact_fresh",
                                &perf_watchlist_artifact_fresh))
    (void)string_list_push_copy(
        errors, "progress perf watchlist artifact fresh flag missing");
  if (!summary_number_from_report(json,
                                  "perf_watchlist_artifact_age_seconds",
                                  &perf_watchlist_artifact_age))
    (void)string_list_push_copy(errors,
                                "progress perf watchlist artifact age missing");
  if (!summary_number_from_report(json,
                                  "perf_watchlist_artifact_stale_after_hours",
                                  &perf_watchlist_artifact_stale_after) ||
      perf_watchlist_artifact_stale_after < 23.9 ||
      perf_watchlist_artifact_stale_after > 24.1)
    (void)string_list_push_copy(
        errors, "progress perf watchlist artifact freshness window wrong");
  double perf_watchlist_artifact_slowdown = -1.0;
  if (!summary_number_from_report(
          json, "perf_watchlist_artifact_max_slowdown_percent",
          &perf_watchlist_artifact_slowdown))
    (void)string_list_push_copy(
        errors, "progress perf watchlist artifact slowdown missing");
  if (perf_watchlist_artifact_readable && perf_watchlist_artifact_fresh &&
      optimization_action &&
      strcmp(optimization_action, "inspect-watchlist") == 0 &&
      optimization_ratio > 0.0 &&
      (!optimization_artifact || !*optimization_artifact ||
       !optimization_ny_source || !*optimization_ny_source ||
       !optimization_c_source || !*optimization_c_source))
    (void)string_list_push_copy(
        errors, "progress optimization target file fields empty for fresh watchlist");
  if (optimization_artifact && *optimization_artifact &&
      (!optimization_target_command ||
       !strstr(optimization_target_command, optimization_artifact)))
    (void)string_list_push_copy(
        errors, "progress optimization target command omits artifact");
  if (optimization_ny_source && *optimization_ny_source &&
      (!optimization_target_command ||
       !strstr(optimization_target_command, optimization_ny_source)))
    (void)string_list_push_copy(
        errors, "progress optimization target command omits Ny source");
  if (optimization_c_source && *optimization_c_source &&
      (!optimization_target_command ||
       !strstr(optimization_target_command, optimization_c_source)))
    (void)string_list_push_copy(
        errors, "progress optimization target command omits C source");
  char *latest_report = summary_string_from_report(json, "latest_report");
  char *latest_full_pressure_report =
      summary_string_from_report(json, "latest_full_pressure_report");
  if (!latest_report || !strstr(latest_report, "latest-smoke.json"))
    (void)string_list_push_copy(errors, "progress latest smoke report wrong");
  if (!latest_full_pressure_report ||
      !strstr(latest_full_pressure_report, "latest-full-pressure.json"))
    (void)string_list_push_copy(errors, "progress latest full-pressure report wrong");
  bool progress_latest_full_pressure_ok = false;
  bool progress_latest_full_pressure_clean = false;
  bool progress_latest_report_demoted = true;
  bool progress_latest_full_pressure_demoted = true;
  double progress_latest_full_pressure_failures = -1.0;
  if (!summary_bool_from_report(json, "latest_full_pressure_ok",
                                &progress_latest_full_pressure_ok) ||
      !progress_latest_full_pressure_ok)
    (void)string_list_push_copy(errors,
                                "progress latest full-pressure raw ok gate wrong");
  if (!summary_bool_from_report(json, "latest_full_pressure_clean",
                                &progress_latest_full_pressure_clean) ||
      !progress_latest_full_pressure_clean)
    (void)string_list_push_copy(errors,
                                "progress latest full-pressure clean gate wrong");
  bool progress_latest_full_pressure_raw_ok_alias = false;
  bool progress_latest_full_pressure_effective_clean_alias = false;
  if (!summary_bool_from_report(json, "latest_full_pressure_raw_ok",
                                &progress_latest_full_pressure_raw_ok_alias) ||
      progress_latest_full_pressure_raw_ok_alias !=
          progress_latest_full_pressure_ok)
    (void)string_list_push_copy(
        errors, "progress latest full-pressure raw ok alias wrong");
  if (!summary_bool_from_report(
          json, "latest_full_pressure_effective_clean",
          &progress_latest_full_pressure_effective_clean_alias) ||
      progress_latest_full_pressure_effective_clean_alias !=
          progress_latest_full_pressure_clean)
    (void)string_list_push_copy(
        errors, "progress latest full-pressure effective clean alias wrong");
  if (!summary_number_from_report(json, "latest_full_pressure_failure_count",
                                  &progress_latest_full_pressure_failures) ||
      progress_latest_full_pressure_failures != 0.0)
    (void)string_list_push_copy(errors,
                                "progress latest full-pressure failure count wrong");
  if (!summary_bool_from_report(
          json, "latest_report_demoted_non_reproducing_afl_timeout",
          &progress_latest_report_demoted) ||
      progress_latest_report_demoted)
    (void)string_list_push_copy(errors,
                                "progress latest report demotion flag wrong");
  if (!summary_bool_from_report(
          json, "latest_full_pressure_demoted_non_reproducing_afl_timeout",
          &progress_latest_full_pressure_demoted) ||
      progress_latest_full_pressure_demoted)
    (void)string_list_push_copy(errors,
                                "progress latest full-pressure demotion flag wrong");
  char *progress_latest_full_pressure_reason =
      summary_string_from_report(json, "latest_full_pressure_clean_reason");
  if (!progress_latest_full_pressure_reason ||
      strcmp(progress_latest_full_pressure_reason, "ok") != 0)
    (void)string_list_push_copy(errors,
                                "progress latest full-pressure clean reason wrong");
  if (!summary_number_from_report(json, "advisory_timeouts", &finding_count) ||
      finding_count != 0.0)
    (void)string_list_push_copy(errors, "progress advisory timeouts wrong");
  if (!summary_number_from_report(json, "current_advisory_timeouts",
                                  &finding_count) ||
      finding_count != 0.0)
    (void)string_list_push_copy(errors, "progress current advisory timeouts wrong");
  if (!summary_number_from_report(json, "non_reproducing_afl_timeouts",
                                  &finding_count) ||
      finding_count != 0.0)
    (void)string_list_push_copy(errors,
                                "progress non-reproducing timeout alias wrong");
  if (!summary_number_from_report(json, "historical_advisory_timeouts",
                                  &finding_count) ||
      finding_count != 0.0)
    (void)string_list_push_copy(errors, "progress historical advisory timeouts wrong");
  if (!summary_number_from_report(json,
                                  "historical_non_reproducing_afl_timeouts",
                                  &finding_count) ||
      finding_count != 0.0)
    (void)string_list_push_copy(
        errors, "progress historical non-reproducing timeout alias wrong");
  char *advisory_state = summary_string_from_report(json, "advisory_state");
  if (!advisory_state || strcmp(advisory_state, "clear") != 0)
    (void)string_list_push_copy(errors, "progress advisory state wrong");
  double effective_advisory_timeouts = -1.0;
  double advisory_effective_timeouts = -1.0;
  if (!summary_number_from_report(json, "effective_advisory_timeouts",
                                  &effective_advisory_timeouts) ||
      effective_advisory_timeouts != 0.0 ||
      !summary_number_from_report(json, "advisory_effective_timeouts",
                                  &advisory_effective_timeouts) ||
      advisory_effective_timeouts != 0.0)
    (void)string_list_push_copy(errors,
                                "progress effective advisory timeouts wrong");
  char *advisory_penalty_state =
      summary_string_from_report(json, "advisory_penalty_state");
  if (!advisory_penalty_state ||
      strcmp(advisory_penalty_state, "clear") != 0)
    (void)string_list_push_copy(errors,
                                "progress advisory penalty state wrong");
  double penalty = -1.0;
  if (!summary_number_from_report(json, "gate_penalty", &penalty) ||
      penalty != 0.0)
    (void)string_list_push_copy(errors, "progress gate penalty wrong");
  if (!summary_number_from_report(json, "correctness_penalty", &penalty) ||
      penalty != 0.0)
    (void)string_list_push_copy(errors, "progress correctness penalty wrong");
  if (!summary_number_from_report(json, "perf_penalty", &penalty) ||
      penalty != 0.0)
    (void)string_list_push_copy(errors, "progress perf penalty wrong");
  if (!summary_number_from_report(json, "advisory_penalty", &penalty) ||
      penalty != 0.0)
    (void)string_list_push_copy(errors, "progress advisory penalty wrong");
  if (!summary_number_from_report(json, "environment_penalty", &penalty) ||
      penalty != 0.0)
    (void)string_list_push_copy(errors, "progress environment penalty wrong");
  if (!summary_number_from_report(json, "freshness_penalty", &penalty) ||
      penalty != 0.0)
    (void)string_list_push_copy(errors, "progress freshness penalty wrong");
  double language_gap = -1.0;
  if (!summary_number_from_report(json, "language_score_gap_percent",
                                  &language_gap) ||
      language_gap < 1.24 || language_gap > 1.26)
    (void)string_list_push_copy(errors,
                                "progress language score gap wrong");
  double wall_hours_needed = -1.0, thread_hours_needed = -1.0;
  if (!summary_number_from_report(json, "wall_hours_needed",
                                  &wall_hours_needed) ||
      wall_hours_needed != 4800.0)
    (void)string_list_push_copy(errors, "progress wall-hours budget wrong");
  if (!summary_number_from_report(json, "thread_hours_needed",
                                  &thread_hours_needed) ||
      thread_hours_needed != 9600.0)
    (void)string_list_push_copy(errors, "progress thread-hours budget wrong");
  char *label = summary_string_from_report(json, "stability_label");
  if (!label || strcmp(label, "promising") != 0)
    (void)string_list_push_copy(errors, "progress stability label wrong");
  char *language_label = summary_string_from_report(json, "language_score_label");
  if (!language_label || strcmp(language_label, "promising") != 0)
    (void)string_list_push_copy(errors, "progress language score label wrong");
  char *score_label = summary_string_from_report(json, "score_label");
  if (!score_label || strcmp(score_label, "promising") != 0)
    (void)string_list_push_copy(errors, "progress compact score label wrong");
  char *language_note = summary_string_from_report(json, "language_score_note");
  if (!language_note ||
      !strstr(language_note,
              "evidence cap 73.75% from 12.5000% campaign evidence"))
    (void)string_list_push_copy(errors, "progress language score note wrong");
  char advisory_note[256] = {0};
  fuzz_all_progress_score_note(advisory_note, sizeof(advisory_note), true, 0.0,
                               0.0, 0.0, 2.0, 98.0, 73.75, 12.5);
  if (!strstr(advisory_note, "advisory timeout penalty") ||
      !strstr(advisory_note,
              "evidence cap 73.75% from 12.5000% campaign evidence"))
    (void)string_list_push_copy(errors,
                                "progress advisory score note wrong");
  char *next = summary_string_from_report(json, "next_command");
  char *next_handoff = summary_string_from_report(json, "next_handoff_command");
  if (!next || !selftest_command_uses_env_nice(next) ||
      !strstr(next, "build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(errors, "progress next command missing guard");
  if (!next_handoff || selftest_command_uses_env_nice(next_handoff) ||
      !strstr(next_handoff, "build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(
        errors, "progress next handoff command missing raw script");
  char *preview_command = summary_string_from_report(json, "preview_command");
  if (!preview_command ||
      !strstr(preview_command, "NYTRIX_RUN_DRY_RUN=1") ||
      !strstr(preview_command, "NYTRIX_RUN_REPEAT=target") ||
      !strstr(preview_command, "build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(errors, "progress preview command missing");
  char *run_next_command_alias =
      summary_string_from_report(json, "run_next_command");
  char *run_next_preview_command_alias =
      summary_string_from_report(json, "run_next_preview_command");
  char *run_next_low_cpu_command_alias =
      summary_string_from_report(json, "run_next_low_cpu_command");
  if (!run_next_command_alias ||
      strcmp(run_next_command_alias, next ? next : "") != 0)
    (void)string_list_push_copy(errors,
                                "progress run-next command alias wrong");
  if (!run_next_preview_command_alias ||
      strcmp(run_next_preview_command_alias,
             preview_command ? preview_command : "") != 0)
    (void)string_list_push_copy(errors,
                                "progress run-next preview alias wrong");
  if (!run_next_low_cpu_command_alias ||
      strcmp(run_next_low_cpu_command_alias, next ? next : "") != 0)
    (void)string_list_push_copy(errors,
                                "progress run-next low-cpu alias wrong");
  char *run_next_gentle_command =
      summary_string_from_report(json, "run_next_gentle_command");
  if (!run_next_gentle_command ||
      !selftest_command_uses_env_nice(run_next_gentle_command) ||
      !strstr(run_next_gentle_command, "NYTRIX_RUN_HOURS=1") ||
      !strstr(run_next_gentle_command, "NYTRIX_RUN_THREADS=10%") ||
      !strstr(run_next_gentle_command, "build/fuzz/all/run-next.sh") ||
      strstr(run_next_gentle_command, "NYTRIX_RUN_DRY_RUN=1") ||
      strstr(run_next_gentle_command, "NYTRIX_RUN_REPEAT="))
    (void)string_list_push_copy(errors,
                                "progress gentle run command missing guard");
  char *run_next_gentle_preview_command =
      summary_string_from_report(json, "run_next_gentle_preview_command");
  if (!run_next_gentle_preview_command ||
      !selftest_command_uses_env_nice(run_next_gentle_preview_command) ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_RUN_DRY_RUN=1") ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_RUN_HOURS=1") ||
      !strstr(run_next_gentle_preview_command, "NYTRIX_RUN_THREADS=10%") ||
      !strstr(run_next_gentle_preview_command, "build/fuzz/all/run-next.sh") ||
      strstr(run_next_gentle_preview_command, "NYTRIX_RUN_REPEAT="))
    (void)string_list_push_copy(errors,
                                "progress gentle preview command missing guard");
  char *state_refresh_command =
      summary_string_from_report(json, "state_refresh_command");
  if (state_refresh_command && *state_refresh_command &&
      (!strstr(state_refresh_command, "NYTRIX_RUN_DRY_RUN=1") ||
       !strstr(state_refresh_command, "NYTRIX_RUN_REPEAT=target") ||
       !strstr(state_refresh_command, "build/fuzz/all/run-next.sh")))
    (void)string_list_push_copy(errors,
                                "progress state refresh command malformed");
  char *stop_file = summary_string_from_report(json, "stop_file");
  char *stop_command = summary_string_from_report(json, "stop_command");
  char *resume_command = summary_string_from_report(json, "resume_command");
  char *state_file = summary_string_from_report(json, "state_file");
  char *state_command = summary_string_from_report(json, "state_command");
  if (!stop_file || !strstr(stop_file, expected_stop_file))
    (void)string_list_push_copy(errors, "progress stop file missing");
  if (!stop_command || !strstr(stop_command, "touch ") ||
      !strstr(stop_command, expected_stop_file))
    (void)string_list_push_copy(errors, "progress stop command missing");
  if (!resume_command || !strstr(resume_command, "rm -f ") ||
      !strstr(resume_command, expected_stop_file))
    (void)string_list_push_copy(errors, "progress resume command missing");
  if (!state_file || !strstr(state_file, expected_state_file))
    (void)string_list_push_copy(errors, "progress state file missing");
  if (!state_command ||
      !strstr(state_command,
              "jq {state,event,live,child_status,stale_after_seconds,repeat_mode,repeat_count,") ||
      !strstr(state_command, "dry_run_wall_hours") ||
      !strstr(state_command, "canonical_status_report") ||
      !strstr(state_command, expected_state_file))
    (void)string_list_push_copy(errors, "progress state command missing");
  selftest_expect_top_alias_string(json, "quick_probe_command", errors);
  selftest_expect_top_alias_string(json, "state_probe_command", errors);
  selftest_expect_top_alias_string(json, "selftest_catalog_command", errors);
  selftest_expect_top_alias_string(json, "selftest_result_probe_command",
                                   errors);
  selftest_expect_top_alias_string(json, "selftest_cockpit_run_command",
                                   errors);
  selftest_expect_top_alias_string(json,
                                   "selftest_cockpit_result_probe_command",
                                   errors);
  selftest_expect_top_alias_string(json, "known_bugs_command", errors);
  selftest_expect_top_alias_string(json, "known_bugs_report", errors);
  selftest_expect_top_alias_string(json, "known_bugs_markdown", errors);
  selftest_expect_top_alias_string(json, "known_bugs_result_probe_command",
                                   errors);
  selftest_expect_top_alias_bool(json, "known_bugs_readable", errors);
  selftest_expect_top_alias_number(json, "known_bug_count",
                                   "known_bug_count", errors);
  selftest_expect_top_alias_number(json, "known_bug_fixed_candidates",
                                   "known_bug_fixed_candidates", errors);
  selftest_expect_top_alias_string(json, "perf_triage_command", errors);
  selftest_expect_top_alias_string(json, "perf_triage_report", errors);
  selftest_expect_top_alias_string(json, "perf_triage_markdown", errors);
  selftest_expect_top_alias_string(json, "perf_triage_result_probe_command",
                                   errors);
  selftest_expect_top_alias_bool(json, "perf_triage_readable", errors);
  selftest_expect_top_alias_number(json, "perf_triage_cases",
                                   "perf_triage_cases", errors);
  selftest_expect_top_alias_number(json, "perf_triage_failure_count",
                                   "perf_triage_failure_count", errors);
  selftest_expect_top_alias_number(json, "perf_triage_hotspots",
                                   "perf_triage_hotspots", errors);
  selftest_expect_top_alias_number(json, "perf_triage_worst_ratio",
                                   "perf_triage_worst_ratio", errors);
  selftest_expect_top_alias_number(json, "perf_triage_worst_slowdown_percent",
                                   "perf_triage_worst_slowdown_percent",
                                   errors);
  selftest_expect_top_alias_string(json, "perf_triage_worst_case", errors);
  char *quick_probe_command =
      summary_string_from_report(json, "quick_probe_command");
  if (!quick_probe_command ||
      !strstr(quick_probe_command, "progress.json") ||
      !strstr(quick_probe_command, "paths:{scratch:.scratch_root"))
    (void)string_list_push_copy(errors,
                                "progress quick probe alias missing");
  free(quick_probe_command);
  char *progress_scratch_root =
      summary_string_from_report(json, "scratch_root");
  char *progress_tmp_dir = summary_string_from_report(json, "tmp_dir");
  char *progress_xdg_cache_home =
      summary_string_from_report(json, "xdg_cache_home");
  char *progress_nytrix_cache_dir =
      summary_string_from_report(json, "nytrix_cache_dir");
  if (!progress_scratch_root ||
      strcmp(progress_scratch_root, "build/cache/scratch") != 0 ||
      !progress_tmp_dir || strcmp(progress_tmp_dir, "build/cache/tmp") != 0 ||
      !progress_xdg_cache_home ||
      strcmp(progress_xdg_cache_home, "build/cache/xdg") != 0 ||
      !progress_nytrix_cache_dir ||
      strcmp(progress_nytrix_cache_dir, "build/cache/nytrix") != 0)
    (void)string_list_push_copy(errors,
                                "progress repo-local cache roots missing");
  free(progress_scratch_root);
  free(progress_tmp_dir);
  free(progress_xdg_cache_home);
  free(progress_nytrix_cache_dir);
  char *recommended_action =
      summary_string_from_report(json, "recommended_action");
  char *recommended_reason =
      summary_string_from_report(json, "recommended_reason");
  char *recommended_command =
      summary_string_from_report(json, "recommended_command");
  char *recommended_low_cpu_command =
      summary_string_from_report(json, "recommended_low_cpu_command");
  char *recommended_preview_command =
      summary_string_from_report(json, "recommended_preview_command");
  if (!recommended_action || strcmp(recommended_action, "monitor-run") != 0)
    (void)string_list_push_copy(errors, "progress recommended action wrong");
  if (!recommended_reason || !strstr(recommended_reason, "live"))
    (void)string_list_push_copy(errors, "progress recommended reason wrong");
  if (!recommended_command ||
      !strstr(recommended_command,
              "jq {state,event,live,child_status,stale_after_seconds,repeat_mode,repeat_count,") ||
      !strstr(recommended_command, "dry_run_wall_hours") ||
      !strstr(recommended_command, "canonical_status_report") ||
      !strstr(recommended_command, expected_state_file))
    (void)string_list_push_copy(errors, "progress recommended command wrong");
  if (!recommended_preview_command)
    (void)string_list_push_copy(errors,
                                "progress recommended preview missing");
  else if (*recommended_preview_command)
    (void)string_list_push_copy(errors,
                                "progress monitor-run preview should be empty");
  if (recommended_low_cpu_command && *recommended_low_cpu_command)
    (void)string_list_push_copy(errors,
                                "progress monitor-run low-cpu should be empty");
  char *recommended_state_source =
      summary_string_from_report(json, "recommended_state_source");
  char *recommended_state_command =
      summary_string_from_report(json, "recommended_state_command");
  bool recommended_state_fresh = false;
  bool recommended_state_live = false;
  if (!recommended_state_source ||
      strcmp(recommended_state_source, "run") != 0 ||
      !recommended_state_command ||
      !strstr(recommended_state_command, expected_state_file) ||
      !strstr(recommended_state_command, "dry_run_wall_hours") ||
      !strstr(recommended_state_command, "canonical_status_report") ||
      !summary_bool_from_report(json, "recommended_state_fresh",
                                &recommended_state_fresh) ||
      !recommended_state_fresh ||
      !summary_bool_from_report(json, "recommended_state_live",
                                &recommended_state_live) ||
      !recommended_state_live)
    (void)string_list_push_copy(
        errors, "progress recommended state aliases do not follow live run");
  double state_dry_wall_hours = -1.0;
  double state_dry_thread_years = -1.0;
  double state_dry_gain = -1.0;
  double recommended_state_dry_wall_hours = -1.0;
  double recommended_state_dry_thread_years = -1.0;
  double recommended_state_dry_gain = -1.0;
  char *state_canonical_status =
      summary_string_from_report(json, "state_canonical_status_report");
  char *state_canonical_progress =
      summary_string_from_report(json, "state_canonical_progress_report");
  char *recommended_state_canonical_status =
      summary_string_from_report(json,
                                 "recommended_state_canonical_status_report");
  char *recommended_state_canonical_progress =
      summary_string_from_report(json,
                                 "recommended_state_canonical_progress_report");
  if (!summary_number_from_report(json, "state_dry_run_wall_hours",
                                  &state_dry_wall_hours) ||
      !summary_number_from_report(json, "state_dry_run_thread_years",
                                  &state_dry_thread_years) ||
      !summary_number_from_report(json, "state_dry_run_campaign_gain_percent",
                                  &state_dry_gain) ||
      !state_canonical_status ||
      !state_canonical_progress)
    (void)string_list_push_copy(errors,
                                "progress state dry-run metrics missing");
  if (!summary_number_from_report(json,
                                  "recommended_state_dry_run_wall_hours",
                                  &recommended_state_dry_wall_hours) ||
      !summary_number_from_report(json,
                                  "recommended_state_dry_run_thread_years",
                                  &recommended_state_dry_thread_years) ||
      !summary_number_from_report(
          json, "recommended_state_dry_run_campaign_gain_percent",
          &recommended_state_dry_gain) ||
      !recommended_state_canonical_status ||
      !recommended_state_canonical_progress)
    (void)string_list_push_copy(
        errors, "progress recommended state dry-run metrics missing");
  if (recommended_repeat_mode && *recommended_repeat_mode)
    (void)string_list_push_copy(errors,
                                "progress monitor-run repeat mode should be empty");
  if (recommended_repeat_count != 0.0)
    (void)string_list_push_copy(errors,
                                "progress monitor-run repeat count should be zero");
  bool handoff_low_priority = false, handoff_load_wait = false;
  bool handoff_space_guard = false, handoff_run_lock = false;
  double handoff_nice = -1.0, handoff_max_load = -1.0;
  double handoff_min_free = -1.0;
  char *handoff_threads =
      summary_string_from_report(json, "handoff_threads_default");
  char *handoff_summary =
      summary_string_from_report(json, "handoff_guard_summary");
  if (!summary_bool_from_report(json, "handoff_low_priority_default",
                                &handoff_low_priority) ||
      !handoff_low_priority ||
      !summary_number_from_report(json, "handoff_nice_default",
                                  &handoff_nice) ||
      handoff_nice != 10.0 ||
      !summary_bool_from_report(json, "handoff_load_wait_default",
                                &handoff_load_wait) ||
      !handoff_load_wait ||
      !summary_number_from_report(json, "handoff_max_load_pct_default",
                                  &handoff_max_load) ||
      handoff_max_load != 75.0 ||
      !summary_bool_from_report(json, "handoff_space_guard_default",
                                &handoff_space_guard) ||
      !handoff_space_guard ||
      !summary_number_from_report(json, "handoff_min_free_gb_default",
                                  &handoff_min_free) ||
      handoff_min_free != 20.0 ||
      !summary_bool_from_report(json, "handoff_run_lock_default",
                                &handoff_run_lock) ||
      !handoff_run_lock ||
      !handoff_threads || strcmp(handoff_threads, "25%") != 0 ||
      !handoff_summary || !strstr(handoff_summary, "low-priority nice 10"))
    (void)string_list_push_copy(errors,
                                "progress handoff guard defaults wrong");
  free(handoff_threads);
  free(handoff_summary);
  bool state_readable = false, state_fresh = false, state_live = false;
  bool state_child_alive = false;
  double state_value = -2.0, state_pid = -1.0;
  if (!summary_bool_from_report(json, "state_readable", &state_readable))
    (void)string_list_push_copy(errors, "progress state readable field missing");
  else if (!state_readable)
    (void)string_list_push_copy(errors, "progress state was not readable");
  if (!summary_bool_from_report(json, "state_fresh", &state_fresh))
    (void)string_list_push_copy(errors, "progress state fresh field missing");
  else if (!state_fresh)
    (void)string_list_push_copy(errors, "progress state was stale");
  if (!summary_bool_from_report(json, "state_live", &state_live))
    (void)string_list_push_copy(errors, "progress state live field missing");
  else if (!state_live)
    (void)string_list_push_copy(errors, "progress state was not live");
  if (state_refresh_command && *state_refresh_command)
    (void)string_list_push_copy(
        errors, "progress live state exposed refresh command");
  if (!summary_bool_from_report(json, "state_child_alive", &state_child_alive))
    (void)string_list_push_copy(errors, "progress state child alive field missing");
  else if (!state_child_alive)
    (void)string_list_push_copy(errors, "progress state child was not alive");
  char *state_child_status =
      summary_string_from_report(json, "state_child_status");
  if (!state_child_status ||
      strcmp(state_child_status, "alive") != 0)
    (void)string_list_push_copy(errors, "progress state child status wrong");
  char *state_stale_reason =
      summary_string_from_report(json, "state_stale_reason");
  if (!state_stale_reason ||
      strcmp(state_stale_reason, "none") != 0)
    (void)string_list_push_copy(errors, "progress state stale reason wrong");
  char *state_label = summary_string_from_report(json, "state");
  if (!state_label)
    (void)string_list_push_copy(errors, "progress state label missing");
  char *state_phase = summary_string_from_report(json, "state_phase");
  if (!state_phase)
    (void)string_list_push_copy(errors, "progress state phase missing");
  if (state_label &&
      strcmp(state_label,
             fuzz_all_state_label_values(state_readable,
                                         state_phase ? state_phase : "")) != 0)
    (void)string_list_push_copy(errors, "progress state label wrong");
  if (!summary_number_from_report(json, "state_age_seconds", &state_value))
    (void)string_list_push_copy(errors, "progress state age missing");
  if (!summary_number_from_report(json, "state_stale_after_seconds",
                                  &state_value) ||
      state_value != 630.0)
    (void)string_list_push_copy(errors, "progress state stale threshold wrong");
  if (!summary_number_from_report(json, "state_child_pid", &state_pid))
    (void)string_list_push_copy(errors, "progress state child pid missing");
  else if (state_pid <= 0.0)
    (void)string_list_push_copy(errors, "progress state child pid was zero");
  bool state_handoff_low_priority = false, state_handoff_load_wait = false;
  bool state_handoff_space_guard = false, state_handoff_run_lock = false;
  double state_handoff_nice = -1.0, state_handoff_max_load = -1.0;
  double state_handoff_min_free = -1.0;
  char *state_handoff_threads =
      summary_string_from_report(json, "state_handoff_threads");
  if (!summary_bool_from_report(json, "state_handoff_low_priority",
                                &state_handoff_low_priority) ||
      !state_handoff_low_priority ||
      !summary_number_from_report(json, "state_handoff_nice",
                                  &state_handoff_nice) ||
      state_handoff_nice != 10.0 ||
      !summary_bool_from_report(json, "state_handoff_load_wait",
                                &state_handoff_load_wait) ||
      !state_handoff_load_wait ||
      !summary_number_from_report(json, "state_handoff_max_load_pct",
                                  &state_handoff_max_load) ||
      state_handoff_max_load != 75.0 ||
      !summary_bool_from_report(json, "state_handoff_space_guard",
                                &state_handoff_space_guard) ||
      !state_handoff_space_guard ||
      !summary_number_from_report(json, "state_handoff_min_free_gb",
                                  &state_handoff_min_free) ||
      state_handoff_min_free != 20.0 ||
      !summary_bool_from_report(json, "state_handoff_run_lock",
                                &state_handoff_run_lock) ||
      !state_handoff_run_lock ||
      !state_handoff_threads || strcmp(state_handoff_threads, "25%") != 0)
    (void)string_list_push_copy(errors,
                                "progress state handoff guard fields wrong");
  free(state_handoff_threads);
  (void)state_value;
  free(state_label);
  free(state_phase);
  free(state_child_status);
  free(state_stale_reason);
  char *freshness_action =
      summary_string_from_report(json, "freshness_action_command");
  if (freshness_action && *freshness_action)
    (void)string_list_push_copy(
        errors, "progress fresh evidence unexpectedly has freshness action");
  char *advisory_action =
      summary_string_from_report(json, "advisory_action_command");
  if (!advisory_action)
    (void)string_list_push_copy(errors,
                                "progress advisory action command missing");
  else if (*advisory_action)
    (void)string_list_push_copy(errors,
                                "progress advisory action should be empty");
  char *advisory_recheck =
      summary_string_from_report(json, "advisory_recheck_command");
  if (!advisory_recheck)
    (void)string_list_push_copy(errors,
                                "progress advisory recheck command missing");
  else if (*advisory_recheck)
    (void)string_list_push_copy(errors,
                                "progress advisory recheck should be empty");
  char *advisory_recheck_state =
      summary_string_from_report(json, "advisory_recheck_state");
  double advisory_recheck_checked = -1.0, advisory_recheck_passed = -1.0;
  double advisory_recheck_timeouts = -1.0, advisory_recheck_unexpected = -1.0;
  if (!advisory_recheck_state ||
      strcmp(advisory_recheck_state, "clear") != 0)
    (void)string_list_push_copy(errors,
                                "progress advisory recheck state should be clear");
  if (!summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_checked",
                                  &advisory_recheck_checked) ||
      advisory_recheck_checked != 0.0 ||
      !summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_passed",
                                  &advisory_recheck_passed) ||
      advisory_recheck_passed != 0.0 ||
      !summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_timeouts",
                                  &advisory_recheck_timeouts) ||
      advisory_recheck_timeouts != 0.0 ||
      !summary_number_from_report(json,
                                  "advisory_recheck_raw_repro_unexpected",
                                  &advisory_recheck_unexpected) ||
      advisory_recheck_unexpected != 0.0)
    (void)string_list_push_copy(errors,
                                "progress advisory recheck counters should be zero");
  char *old_path_probe_command =
      summary_string_from_report(json, "old_path_probe_command");
  char *old_path_command = summary_string_from_report(json, "old_path_command");
  char *old_path_dry_run_command =
      summary_string_from_report(json, "old_path_dry_run_command");
  char *old_path_apply_command =
      summary_string_from_report(json, "old_path_apply_command");
  char *old_path_next_action =
      summary_string_from_report(json, "old_path_next_action");
  char *old_path_next_reason =
      summary_string_from_report(json, "old_path_next_reason");
  if (!old_path_probe_command ||
      !selftest_command_uses_env_nice(old_path_probe_command) ||
      strcmp(old_path_probe_command, NYTRIX_FUZZ_ALL_OLD_PATH_PROBE_COMMAND) !=
          0 ||
      !strstr(old_path_probe_command, "--probe"))
    (void)string_list_push_copy(errors,
                                "progress old-path probe command missing");
  if (!old_path_command ||
      !selftest_command_uses_env_nice(old_path_command) ||
      !strstr(old_path_command, "fuzz all old-paths --dry-run") ||
      !strstr(old_path_command, "--nytrix-root ../nytrix") ||
      !strstr(old_path_command, "--archive-dir build/cache/old-nytrix") ||
      !strstr(old_path_command, expected_old_path_report) ||
      !strstr(old_path_command, expected_old_path_markdown))
    (void)string_list_push_copy(errors, "progress old-path command missing");
  if (!old_path_dry_run_command ||
      !selftest_command_uses_env_nice(old_path_dry_run_command) ||
      !strstr(old_path_dry_run_command, "fuzz all old-paths --dry-run") ||
      !strstr(old_path_dry_run_command,
              "--archive-dir build/cache/old-nytrix") ||
      !strstr(old_path_dry_run_command, expected_old_path_report) ||
      !strstr(old_path_dry_run_command, expected_old_path_markdown))
    (void)string_list_push_copy(errors,
                                "progress old-path dry-run command missing");
  if (!old_path_apply_command ||
      !selftest_command_uses_env_nice(old_path_apply_command) ||
      !strstr(old_path_apply_command, "fuzz all old-paths --apply") ||
      !strstr(old_path_apply_command, "--wait-writers-s 300") ||
      !strstr(old_path_apply_command,
              "--archive-dir build/cache/old-nytrix") ||
      !strstr(old_path_apply_command, expected_old_path_report) ||
      !strstr(old_path_apply_command, expected_old_path_markdown))
    (void)string_list_push_copy(errors,
                                "progress old-path apply command missing");
  if (!old_path_next_action || !*old_path_next_action ||
      !old_path_next_reason || !*old_path_next_reason)
    (void)string_list_push_copy(errors,
                                "progress old-path next action missing");
  char *old_path_report = summary_string_from_report(json, "old_path_report");
  char *old_path_markdown =
      summary_string_from_report(json, "old_path_markdown");
  bool old_path_cache_policy_ok = false;
  double old_path_artifact_leak_count = -1.0;
  double old_path_artifact_moved_count = -1.0;
  double old_path_artifact_remaining_count = -1.0;
  double old_path_wait_remaining_seconds = -2.0;
  if (!old_path_report || !strstr(old_path_report, expected_old_path_report))
    (void)string_list_push_copy(errors, "progress old-path report missing");
  if (!old_path_markdown ||
      !strstr(old_path_markdown, expected_old_path_markdown))
    (void)string_list_push_copy(errors, "progress old-path markdown missing");
  if (!summary_bool_from_report(json, "old_path_cache_policy_ok",
                                &old_path_cache_policy_ok) ||
      !old_path_cache_policy_ok)
    (void)string_list_push_copy(errors,
                                "progress old-path cache policy mirror was not ok");
  if (!summary_number_from_report(json, "old_path_wait_remaining_seconds",
                                  &old_path_wait_remaining_seconds))
    (void)string_list_push_copy(errors,
                                "progress old-path wait estimate missing");
  if (!summary_number_from_report(json, "old_path_artifact_leak_count",
                                  &old_path_artifact_leak_count) ||
      old_path_artifact_leak_count != 0.0)
    (void)string_list_push_copy(errors,
                                "progress old-path artifact leak count was not zero");
  if (!summary_number_from_report(json, "old_path_artifact_moved_count",
                                  &old_path_artifact_moved_count) ||
      old_path_artifact_moved_count != 0.0)
    (void)string_list_push_copy(errors,
                                "progress old-path artifact moved count was not zero");
  if (!summary_number_from_report(json, "old_path_artifact_remaining_count",
                                  &old_path_artifact_remaining_count) ||
      old_path_artifact_remaining_count != 0.0)
    (void)string_list_push_copy(errors,
                                "progress old-path artifact remaining count was not zero");
  char *run_command = summary_string_from_report(json, "run_command");
  if (!run_command ||
      !selftest_command_uses_env_nice(run_command) ||
      !strstr(run_command, "fuzz all run") ||
      !strstr(run_command, "--target-thread-years") ||
      !strstr(run_command, "--dir") ||
      strstr(run_command, "fuzz --runs"))
    (void)string_list_push_copy(errors,
                                "progress run command missing campaign run");
  char *status_command = summary_string_from_report(json, "status_command");
  if (!status_command ||
      !selftest_command_uses_env_nice(status_command) ||
      !strstr(status_command, "fuzz all status --refresh"))
    (void)string_list_push_copy(errors,
                                "progress status refresh command missing low-priority guard");
  char *next_script = summary_string_from_report(json, "next_script");
  if (!next_script)
    (void)string_list_push_copy(errors, "progress next script field missing");
  else if (canonical_pair &&
           (!strstr(next_script, "build/fuzz/all/run-next.sh") ||
            path_is_absolute(next_script)))
    (void)string_list_push_copy(errors,
                                "progress next script lost status handoff path");
  char *progress_command = summary_string_from_report(json, "progress_command");
  if (!progress_command ||
      !selftest_command_uses_env_nice(progress_command) ||
      !strstr(progress_command, "fuzz all progress --refresh --strict") ||
      !strstr(progress_command, "--allow-full-pressure-remediation") ||
      !strstr(progress_command, "--status") ||
      !strstr(progress_command, "status.json") ||
      !strstr(progress_command, "--history") ||
      !strstr(progress_command, "history.json") ||
      !strstr(progress_command, "--worklist") ||
      !strstr(progress_command, "worklist.json") ||
      !strstr(progress_command, "--coverage") ||
      !strstr(progress_command, "coverage.json") ||
      !strstr(progress_command, "--plan") ||
      !strstr(progress_command, "plan.json") ||
      !strstr(progress_command, "--json") ||
      !strstr(progress_command, "--markdown") ||
      !strstr(progress_command, expected_markdown ?
                                   expected_markdown :
                                   "fuzz_progress/progress.md") ||
      strstr(progress_command, "build/fuzz/all"))
    (void)string_list_push_copy(errors, "progress refresh command lost low-priority custom cockpit");
  if (canonical_pair &&
      (!progress_command ||
       !strstr(progress_command, "fuzz_progress_canonical/progress.json") ||
       strstr(progress_command, "fuzz_all_progress_canonical.json")))
    (void)string_list_push_copy(errors,
                                "canonical progress command used selftest row json");
  char *markdown = summary_string_from_report(json, "markdown");
  if (!markdown || !strstr(markdown, expected_markdown ?
                                           expected_markdown :
                                           "fuzz_progress/progress.md"))
    (void)string_list_push_copy(errors, "progress markdown path missing");
  char markdown_abs[4096] = {0};
  if (markdown && *markdown) {
    if (path_is_absolute(markdown)) {
      snprintf(markdown_abs, sizeof(markdown_abs), "%s", markdown);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(markdown_abs, sizeof(markdown_abs), root, markdown);
    }
  }
  file_buf_t md = {0};
  if (!markdown_abs[0] || !read_file(markdown_abs, &md) || !md.data) {
    (void)string_list_push_copy(errors, "progress markdown was not readable");
  } else if (!strstr(md.data, "fuzz all old-paths --dry-run")) {
    (void)string_list_push_copy(errors, "progress markdown omits old-path cleanup command");
  } else if (!strstr(md.data, "Refresh:") ||
             !strstr(md.data, "fuzz all progress --refresh") ||
             !strstr(md.data, expected_markdown ?
                                  expected_markdown :
                                  "fuzz_progress/progress.md") ||
             strstr(md.data, "build/fuzz/all/status.json")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits self refresh command");
    } else if (!strstr(md.data, "Preview target:") ||
               !strstr(md.data, "NYTRIX_RUN_DRY_RUN=1") ||
               !strstr(md.data, "NYTRIX_RUN_REPEAT=target") ||
               !strstr(md.data, "build/fuzz/all/run-next.sh")) {
      (void)string_list_push_copy(errors,
                                  "progress markdown omits dry-run preview command");
  } else if (!strstr(md.data, "State:") ||
             !strstr(md.data,
                     "jq {state,event,live,child_status,stale_after_seconds,repeat_mode,repeat_count,") ||
             !strstr(md.data, expected_state_file) ||
             !strstr(md.data, "; live; fresh") ||
             !strstr(md.data, "child ") ||
             !strstr(md.data, " alive")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits live state inspect command");
    } else if (!strstr(md.data, "Pause:") ||
               !strstr(md.data, expected_stop_file) ||
               !strstr(md.data, "touch ") ||
               !strstr(md.data, "rm -f ")) {
      (void)string_list_push_copy(errors,
                                  "progress markdown omits stop-file pause command");
    } else if (!strstr(md.data, "- Progress:") ||
               !strstr(md.data, "remaining 87.5000%") ||
               !strstr(md.data, expected_markdown ?
                                  expected_markdown :
                                  "fuzz_progress/progress.md")) {
    (void)string_list_push_copy(errors, "progress markdown omits own report path");
  } else if (canonical_pair &&
             (!strstr(md.data, "fuzz_progress_canonical/progress.json") ||
              strstr(md.data, "fuzz_all_progress_canonical.json") ||
              strstr(md.data, "progress-r222.json"))) {
    (void)string_list_push_copy(errors,
                                "canonical progress markdown self-link wrong");
  } else if (!strstr(md.data, "Lang score:") ||
             !strstr(md.data, "73.75%") ||
             !strstr(md.data, "good >= 75.00%") ||
             !strstr(md.data, "gap 1.25%")) {
    (void)string_list_push_copy(errors, "progress markdown omits language score");
  } else if (!strstr(md.data, "Confidence:") ||
             !strstr(md.data, "campaign evidence 12.5000%") ||
             !strstr(md.data, "lang score 73.75%") ||
             strstr(md.data, "stability score 73.75%")) {
    (void)string_list_push_copy(errors, "progress markdown omits confidence");
  } else if (!strstr(md.data, "Completion:") ||
             !strstr(md.data, "ready-needs-evidence") ||
             !strstr(md.data, "target-not-reached")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits completion state");
  } else if (!strstr(md.data, "Recommended:") ||
             !strstr(md.data, "monitor-run") ||
             !strstr(md.data, expected_state_file)) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits recommendation");
  } else if (strstr(md.data, "Preview recommended:")) {
    (void)string_list_push_copy(errors,
                                "progress markdown has unexpected monitor preview");
  } else if (!strstr(md.data, "- Handoff guards:") ||
             !strstr(md.data, "low-priority nice 10") ||
             !strstr(md.data, "load-wait 75%") ||
             !strstr(md.data, "disk >=20GB") ||
             !strstr(md.data, "lock on") ||
             !strstr(md.data, "threads 25%")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits handoff guards");
  } else if (!strstr(md.data, "coverage 41/45 lanes") ||
             !strstr(md.data, "91.11%") ||
             !strstr(md.data, "not-run 4")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits coverage depth");
  } else if (!strstr(md.data, "Coverage:") ||
             !strstr(md.data, "partial") ||
             !strstr(md.data, "41/45 lanes") ||
             !strstr(md.data, "skipped 4") ||
             !strstr(md.data, "disabled 3") ||
             !strstr(md.data, "budget-short 1") ||
             !strstr(md.data, "missing-tool 0")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits coverage state");
  } else if (progress_has_coverage_next &&
             (!strstr(md.data, "Coverage next:") ||
             !progress_coverage_next_lane ||
             !strstr(md.data, progress_coverage_next_lane) ||
             !progress_coverage_next_command ||
             !strstr(md.data, progress_coverage_next_command) ||
             (progress_coverage_next_guarded &&
              *progress_coverage_next_guarded &&
              !strstr(md.data, progress_coverage_next_guarded)) ||
             (progress_coverage_next_low_cpu &&
              *progress_coverage_next_low_cpu &&
              !strstr(md.data, progress_coverage_next_low_cpu)) ||
             !strstr(md.data, "Coverage next state:") ||
             !strstr(md.data, "inspect ") ||
             (progress_coverage_next_state_command &&
              *progress_coverage_next_state_command &&
              !strstr(md.data, progress_coverage_next_state_command)) ||
             (progress_coverage_next_state_refresh &&
              *progress_coverage_next_state_refresh &&
              !strstr(md.data, progress_coverage_next_state_refresh)) ||
             (expected_progress_state_refresh_required &&
              (!strstr(md.data, "Coverage next state refresh:") ||
               !strstr(md.data,
                       progress_coverage_next_state_refresh_reason ?
                           progress_coverage_next_state_refresh_reason : "") ||
               !strstr(md.data,
                       progress_recommended_state_refresh_command ?
                           progress_recommended_state_refresh_command : ""))))) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits coverage next");
  } else if (!strstr(md.data, "Coverage evidence:") ||
             !strstr(md.data, "9 reports") ||
             !strstr(md.data, "8 campaign + 1 companion") ||
             !strstr(md.data, "latest advisory 1") ||
             !strstr(md.data, "companion skips 1")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits coverage report split");
  } else if (!strstr(md.data, "Evidence hygiene:") ||
             !strstr(md.data, "ignored 2 no-evidence attempts")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits ignored no-evidence count");
  } else if (!strstr(md.data, "Source: status age") ||
             !strstr(md.data, "latest ") ||
             !strstr(md.data, "/24h ok") ||
             !strstr(md.data, "full-pressure ") ||
             !strstr(md.data, "/72h ok")) {
    (void)string_list_push_copy(errors, "progress markdown omits source age");
  } else if (!strstr(md.data, "Full-pressure gate:") ||
             !strstr(md.data, "effective clean `true`") ||
             !strstr(md.data, "raw ok `true`") ||
             !strstr(md.data, "reason `ok`") ||
             !strstr(md.data, "failures 0")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits full-pressure gate");
  } else if (!strstr(md.data, "Compiler std audit:") ||
             !strstr(md.data, "CRT") ||
             !strstr(md.data, "families") ||
             !strstr(md.data, "compiler-std-audit.md")) {
    (void)string_list_push_copy(
        errors, "progress markdown omits compiler std-audit CRT summary");
  } else if (!strstr(md.data, "Full-pressure perf provenance:") ||
             !strstr(md.data, "fixture-poly") ||
             !strstr(md.data, "rows 7/7") ||
             !strstr(md.data, "suite `current`")) {
    (void)string_list_push_copy(
        errors, "progress markdown omits full-pressure perf provenance");
  } else if (!strstr(md.data, "freshness 0.00")) {
    (void)string_list_push_copy(errors, "progress markdown omits freshness penalty");
  } else if (!strstr(md.data, "Budget: 4800.00 wall-hours; 9600.00 thread-hours")) {
    (void)string_list_push_copy(errors, "progress markdown omits split budget");
  } else if (!strstr(md.data, "lang score 74.08%") ||
             !strstr(md.data, "good lang score in 4 runs")) {
    (void)string_list_push_copy(errors, "progress markdown omits next-run language score");
  } else if (!strstr(md.data,
                     "NYTRIX_RUN_REPEAT=good ./build/fuzz/all/run-next.sh")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits guarded repeat handoff");
  } else if (!strstr(md.data,
                     "NYTRIX_RUN_REPEAT=target ./build/fuzz/all/run-next.sh")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits campaign target handoff");
  } else if (!strstr(md.data, "plan-rate")) {
    (void)string_list_push_copy(errors, "progress markdown omits per-run projection source");
  } else if (!strstr(md.data, "History: 5 hidden attention reports") ||
             !strstr(md.data, "worklist-history.md")) {
    (void)string_list_push_copy(errors, "progress markdown omits historical drill-down");
  } else if (!strstr(md.data, "worst 1.2500x") ||
             !strstr(md.data, "fixture-poly")) {
    (void)string_list_push_copy(errors, "progress markdown omits full-pressure perf worst ratio");
  } else if (!strstr(md.data, "Perf watchlist:") ||
             !strstr(md.data, "env NYTRIX_LOW_PRIORITY=1") ||
             !strstr(md.data, "NYTRIX_RUN_NICE=10") ||
             !strstr(md.data, "nice -n 10") ||
             !strstr(md.data, "perf triage --fast") ||
             !strstr(md.data, "perf-watchlist.json") ||
             !strstr(md.data, "perf-watchlist.md")) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits perf watchlist command");
  } else if (perf_watchlist_action &&
             strcmp(perf_watchlist_action, "none") != 0 &&
             (!strstr(md.data, "Optimization action:") ||
              !strstr(md.data, perf_watchlist_action) ||
              !strstr(md.data, "fixture-poly") ||
              !strstr(md.data, "25.00% slower than C") ||
              !strstr(md.data, "perf-watchlist.md"))) {
    (void)string_list_push_copy(errors,
                                "progress markdown omits optimization action");
    } else if (!strstr(md.data, "Advisory:") ||
               !strstr(md.data, "clear") ||
               !strstr(md.data, "current timeouts 0") ||
               !strstr(md.data, "historical 0") ||
               !strstr(md.data, "penalty 0.00")) {
      (void)string_list_push_copy(errors, "progress markdown omits advisory state");
    }
    if (md.data) {
      const char *next_section = strstr(md.data, "\n## Next\n");
      const char *controls_section = strstr(md.data, "\n## Controls\n");
      const char *state_in_controls =
          controls_section ? strstr(controls_section,
                                    expected_state_file) : NULL;
      const char *refresh_in_controls =
          progress_has_coverage_next && controls_section ?
              strstr(controls_section, "NYTRIX_RUN_DRY_RUN=1") : NULL;
      const char *stop_in_controls =
          controls_section ? strstr(controls_section,
                                    expected_stop_file) : NULL;
      const char *refresh_after_next =
          progress_has_coverage_next && next_section ?
              strstr(next_section, "NYTRIX_RUN_DRY_RUN=1") : NULL;
      const char *stop_after_next =
          next_section ? strstr(next_section, expected_stop_file) : NULL;
      if (!controls_section || !state_in_controls ||
          (progress_has_coverage_next && !refresh_in_controls) ||
          !stop_in_controls)
        (void)string_list_push_copy(errors,
                                    "progress markdown omits Controls block");
      const char *quick_jq_line = controls_section ?
          strstr(controls_section,
                 "jq {gate:{ready,blockers,active:.active_count}") :
          NULL;
      const char *quick_jq_end =
          quick_jq_line ? strchr(quick_jq_line, '\n') : NULL;
      if (!quick_jq_line || !quick_jq_end ||
          !find_n(quick_jq_line, quick_jq_end, "progress.json") ||
          !find_n(quick_jq_line, quick_jq_end, "script:.next_script") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "handoff:.next_handoff_command") ||
          !find_n(quick_jq_line, quick_jq_end, "run:.next_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "recommended:.recommended_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "state_refresh:.recommended_state_refresh_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "freshen:.freshness_action_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "runstate:{state:.state,event:.state_event,fresh:.state_fresh,age:.state_age_seconds,age_h:(.state_age_seconds/3600),stale_after:.state_stale_after_seconds,stale_after_h:(.state_stale_after_seconds/3600),over_s:(.state_age_seconds-.state_stale_after_seconds),reason:.state_stale_reason,dry_h:.state_dry_run_wall_hours,dry_gain_pct:.state_dry_run_campaign_gain_percent,dry_years:.state_dry_run_thread_years,threads:.state_handoff_threads}") ||
          find_n(quick_jq_line, quick_jq_end,
                 "low:.recommended_low_cpu_command,refresh:") ||
          find_n(quick_jq_line, quick_jq_end, "script:.next_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "low:.recommended_low_cpu_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "gentle:.run_next_gentle_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "gentle_preview:.run_next_gentle_preview_command") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "penalty:.freshness_penalty") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "perf:{hotspots:.perf_hotspots_open,watch:.perf_watchlist_state,worst:{case:.perf_worst_case,ratio:.perf_worst_ratio,slow:.perf_worst_slowdown_percent},opt:{action:.optimization_action,case:.optimization_case,ratio:.optimization_ratio,cmd:.optimization_command,target:.optimization_target_command}}") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "rt:{state:.runtime_surface_state,done:.runtime_coverage_done,total:.runtime_coverage_total}") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "crt:{state:.crt_surface_state,scope:.crt_surface_scope,behavior:.crt_behavior_state,next:.crt_behavior_next_action,done:.crt_coverage_done,total:.crt_coverage_total,families:.crt_unreferenced_family_count}") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "scratch:.scratch_root") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "tmp:.tmp_dir") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "xdg:.xdg_cache_home") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "nytrix_cache:.nytrix_cache_dir") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_writer:.active_old_nytrix_output_writer_present") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_action:.old_path_next_action") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_seen:.old_path_present_count") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_current:.old_path_remaining_count") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "old_leaks:.old_path_artifact_leak_count") ||
          !find_n(quick_jq_line, quick_jq_end,
                  "artifact_remaining:.old_path_artifact_remaining_count") ||
          find_n(quick_jq_line, quick_jq_end,
                 "old_remaining:.old_path_artifact_remaining_count") ||
          find_n(quick_jq_line, quick_jq_end,
                 "score:{coverage_percent,campaign_percent") ||
          find_n(quick_jq_line, quick_jq_end,
                 "recommended_repeat_mode,recommended_repeat_count") ||
          find_n(quick_jq_line, quick_jq_end, ".md.json") ||
          find_n(quick_jq_line, quick_jq_end, "/home/e/nytrix"))
        (void)string_list_push_copy(
            errors, "progress Controls omit quick jq readout");
      const char *progress_jq_line = controls_section ?
             strstr(controls_section,
                    "jq {ok,cases,ok_count,failure_count,ready,blockers,active_count,"
                    "coverage_percent,coverage_queue_count,"
                    "coverage_queue_non_advisory_count,"
                    "coverage_queue_advisory_count,"
                    "coverage_queue_lanes,campaign_percent,"
                    "campaign_remaining_percent,thread_years,"
                 "target_thread_years,score_percent,stability_percent,"
                 "stability_score_percent,language_score_percent,"
                 "language_score_label,completion_state,"
                 "language_score_good_threshold_percent,"
                 "language_score_signal_percent,"
                 "language_score_evidence_cap_percent,language_score_note,"
                 "language_score_gap_percent,"
                 "next_run_language_score_percent,"
                 "next_run_language_score_delta_percent,"
                 "runs_to_good_language_score,runs_to_good_days,"
                 "runs_to_good_language_days,days_to_good_language_score,"
                 "days_to_good_stability,"
                 "reports,full_pressure_reports,checked_subcases,"
                 "full_pressure_thread_years,"
                 "latest_report,latest_full_pressure_report,"
                 "latest_full_pressure_raw_ok,"
                 "latest_full_pressure_effective_clean,"
                 "latest_full_pressure_clean_reason,"
                 "latest_full_pressure_failure_count,"
                 "latest_full_pressure_demoted_non_reproducing_afl_timeout,"
                 "recommended_action,recommended_reason,"
                 "recommended_command,recommended_preview_command,"
                 "recommended_repeat_mode,recommended_repeat_count,"
                 "recommended_state_fresh,"
                 "recommended_state_stale_after_seconds,"
                 "recommended_state_stale_reason,"
                 "recommended_state_refresh_required,"
                 "state,state_phase,state_event,state_age_seconds,"
                 "state_stale_after_seconds,state_fresh,"
                 "state_live,state_child_status,state_command,"
                 "state_refresh_command,state_stale_reason,"
                 "recommended_state,recommended_state_live,"
                 "recommended_state_age_seconds,"
                 "recommended_state_child_status,recommended_state_command,"
                 "recommended_state_refresh_command,"
                 "recommended_state_refresh_reason,"
                 "recommended_low_cpu_command,run_next_command,"
                 "run_next_preview_command,run_next_low_cpu_command,"
                 "run_next_gentle_command,run_next_gentle_preview_command,"
                 "coverage_next_action,"
                 "coverage_next_category,coverage_next_severity,"
                 "coverage_next_lane,coverage_next_reason,"
                 "coverage_next_command,coverage_next_guarded_command,"
                 "coverage_next_low_cpu_command,coverage_next_preview_command,"
                 "coverage_next_state_file,"
                 "coverage_next_state,coverage_next_state_phase,"
                 "coverage_next_state_event,coverage_next_state_readable,"
                 "coverage_next_state_fresh,coverage_next_state_live,"
                 "coverage_next_state_age_seconds,"
                 "coverage_next_state_stale_after_seconds,"
                 "coverage_next_state_stale_reason,"
                 "coverage_next_state_child_status,"
                 "coverage_next_state_command,"
                 "coverage_next_state_refresh_command,"
                 "coverage_next_state_refresh_required,"
                 "coverage_next_state_refresh_reason,coverage_next_stop_file,"
                 "coverage_next_stop_command,coverage_next_resume_command,"
                 "latest_report_fresh,latest_full_pressure_report_fresh,"
                 "latest_full_pressure_report_age_hours,"
                 "perf_watchlist_artifact_fresh,"
                 "perf_watchlist_artifact_age_seconds,"
                 "perf_watchlist_threshold_ratio,"
                 "perf_watchlist_command,perf_watchlist_report,"
                 "perf_watchlist_markdown,perf_watchlist_action,"
                 "perf_watchlist_action_command,"
                 "optimization_action,optimization_reason,"
                 "optimization_command,optimization_target_command,"
                 "optimization_case,optimization_ratio,"
                 "optimization_slowdown_percent,"
                 "perf_watchlist_artifact_hotspots,"
                 "perf_watchlist_artifact_max_ratio,"
                 "perf_watchlist_artifact_max_case,"
                 "advisory_state,advisory_recheck_state,"
                 "current_advisory_timeouts,"
                 "effective_advisory_timeouts,advisory_effective_timeouts,"
                 "advisory_penalty_state,"
                 "historical_non_reproducing_afl_timeouts,"
                 "advisory_recheck_raw_repro_checked,"
                 "advisory_recheck_raw_repro_passed,"
                 "advisory_recheck_raw_repro_timeouts,"
                    "advisory_recheck_raw_repro_unexpected,advisory_penalty,"
                    "old_nytrix_test_scratch_absent,old_nytrix_fuzz_absent,"
                    "old_nytrix_build_cache_absent,"
                    "active_old_nytrix_output_writer_present,old_path_report,"
                    "old_path_markdown,old_path_cache_policy_ok,"
                    "old_path_present_count,old_path_moved_count,"
                    "old_path_remaining_count,"
                    "old_path_wait_remaining_seconds,"
                    "old_path_artifact_leak_count,old_path_artifact_moved_count,"
                    "old_path_artifact_remaining_count,compiler_findings,"
                 "known_bug_replay_findings,runtime_surface_state,"
                 "runtime_exports,direct_runtime_refs,"
                    "runtime_surface_scope,runtime_coverage_done,runtime_coverage_total,"
                 "runtime_export_coverage_percent,"
                 "runtime_unreferenced_count,runtime_wrapper_gap_count,"
                    "crt_surface_state,crt_surface_scope,crt_behavior_state,"
                    "crt_behavior_scope,crt_behavior_next_action,"
                    "crt_behavior_next_reason,crt_behavior_next_command,"
                    "crt_runtime_exports,crt_direct_refs,"
                 "crt_coverage_done,crt_coverage_total,"
                 "crt_export_coverage_percent,"
                 "crt_unreferenced_percent,crt_unreferenced_count,"
                 "crt_wrapper_gap_count,crt_unreferenced_family_count,"
                 "crt_top_unreferenced_family,"
                 "crt_top_unreferenced_family_count,crt_next_action,"
                 "crt_next_unreferenced_family,"
                 "crt_next_unreferenced_count,perf_hotspots_open,"
                 "perf_worst_ratio,perf_worst_slowdown_percent,"
                    "perf_worst_case,latest_full_pressure_perf_hotspots,latest_full_pressure_perf_max_ratio,latest_full_pressure_perf_max_slowdown_percent,latest_full_pressure_perf_max_case,latest_full_pressure_perf_rows,latest_full_pressure_perf_suite_current,perf_watchlist_state}") :
          NULL;
      const char *progress_jq_end =
          progress_jq_line ? strchr(progress_jq_line, '\n') : NULL;
      if (!progress_jq_line || !progress_jq_end ||
          !find_n(progress_jq_line, progress_jq_end, "progress.json") ||
          find_n(progress_jq_line, progress_jq_end, ".md.json") ||
          find_n(progress_jq_line, progress_jq_end, "/home/e/nytrix"))
        (void)string_list_push_copy(
            errors, "progress Controls omit compact jq readout");
      if (quick_jq_line && progress_jq_line &&
          quick_jq_line > progress_jq_line)
        (void)string_list_push_copy(
            errors, "progress Controls put deep jq before quick jq");
      const char *state_jq_line = controls_section ?
          strstr(controls_section,
                 "jq {state,event,live,child_status,stale_after_seconds,"
                 "repeat_mode,repeat_count,"
                    "handoff_low_priority,handoff_nice,handoff_load_wait,"
                    "handoff_max_load_pct,handoff_space_guard,"
                    "handoff_min_free_gb,handoff_run_lock,handoff_threads,"
                    "heartbeat_s,heartbeat_count,child_pid,"
                    "cycle,cycles,max_cycles,cooldown_s,timestamp_utc,"
                    "updated_at,started_at,finished_at,pid,campaign_dir,"
                    "stop_file,status_report,status_json,progress_report,"
                    "progress_json,dry_run_exceeds_max,dry_run_wall_hours,"
                    "dry_run_wall_days,dry_run_thread_years,"
                    "dry_run_campaign_gain_percent,"
                    "dry_run_target_percent_per_run,"
                    "dry_run_thread_years_per_run,canonical_status_report,"
                       "canonical_progress_report,last_report}") :
          NULL;
      const char *state_jq_end =
          state_jq_line ? strchr(state_jq_line, '\n') : NULL;
      if (!state_jq_line || !state_jq_end ||
          !find_n(state_jq_line, state_jq_end, expected_state_file))
        (void)string_list_push_copy(
            errors, "progress Controls omit compact state jq readout");
      if (controls_section) {
        const char *controls_end =
            strstr(controls_section + strlen("\n## Controls\n"), "\n## ");
        if (!controls_end) controls_end = md.data + strlen(md.data);
        char raw_state_line[8192];
        snprintf(raw_state_line, sizeof(raw_state_line), "cat %s",
                 expected_state_file);
        if (find_n(controls_section, controls_end, raw_state_line))
          (void)string_list_push_copy(
              errors, "progress Controls duplicate raw state dump");
      }
      if (md.data) {
        char raw_state_inspect[8192];
        snprintf(raw_state_inspect, sizeof(raw_state_inspect),
                 "inspect `cat %s`", expected_state_file);
        if (strstr(md.data, raw_state_inspect))
          (void)string_list_push_copy(
              errors, "progress State summary uses raw state cat");
      }
      if ((refresh_after_next && controls_section &&
           refresh_after_next < controls_section) ||
          (stop_after_next && controls_section &&
           stop_after_next < controls_section))
        (void)string_list_push_copy(errors,
                                    "progress markdown mixes controls into Next");
    }
    free(md.data);
  char *worklist = summary_string_from_report(json, "worklist_report");
  if (!worklist || !strstr(worklist, "worklist.json"))
    (void)string_list_push_copy(errors, "progress worklist report path missing");
  char *worklist_md = summary_string_from_report(json, "worklist_markdown");
  if (!worklist_md || !strstr(worklist_md, "worklist.md"))
    (void)string_list_push_copy(errors, "progress worklist markdown path missing");
  char *historical_worklist =
      summary_string_from_report(json, "historical_worklist_report");
  if (!historical_worklist ||
      !strstr(historical_worklist, "worklist-history.json"))
    (void)string_list_push_copy(errors,
                                "progress historical worklist report path missing");
  char *historical_worklist_md =
      summary_string_from_report(json, "historical_worklist_markdown");
  if (!historical_worklist_md ||
      !strstr(historical_worklist_md, "worklist-history.md"))
    (void)string_list_push_copy(errors,
                                "progress historical worklist markdown path missing");
  char *coverage = summary_string_from_report(json, "coverage_report");
  if (!coverage || !strstr(coverage, "coverage.json"))
    (void)string_list_push_copy(errors, "progress coverage report path missing");
  char *plan = summary_string_from_report(json, "plan_report");
  if (!plan || !strstr(plan, "plan.json"))
    (void)string_list_push_copy(errors, "progress plan report path missing");
  if (!strstr(json, "\"mode\":\"fuzz-all-progress\"") ||
      !strstr(json, "\"kind\":\"fuzz-all-progress\""))
    (void)string_list_push_copy(errors, "progress report mode/kind missing");
  free(label);
  free(language_label);
  free(score_label);
  free(language_note);
  free(campaign_state);
  free(campaign_reason);
  free(completion_state);
  free(completion_reason);
  free(thread_years_per_run_source);
  free(run_next_command_alias);
  free(run_next_preview_command_alias);
  free(run_next_low_cpu_command_alias);
  free(run_next_gentle_command);
  free(run_next_gentle_preview_command);
  free(next);
  free(next_handoff);
    free(preview_command);
  free(state_refresh_command);
    free(stop_file);
  free(stop_command);
  free(resume_command);
  free(state_file);
  free(state_command);
  free(state_canonical_status);
  free(state_canonical_progress);
  free(recommended_action);
  free(recommended_reason);
  free(recommended_command);
  free(recommended_low_cpu_command);
  free(recommended_preview_command);
  free(recommended_state_source);
  free(recommended_state_command);
  free(recommended_state_canonical_status);
  free(recommended_state_canonical_progress);
  free(recommended_repeat_mode);
  free(freshness_action);
  free(advisory_action);
  free(advisory_recheck);
  free(advisory_recheck_state);
  free(advisory_state);
  free(advisory_penalty_state);
  free(coverage_state);
  free(progress_coverage_next_action);
  free(progress_coverage_next_lane);
  free(progress_coverage_next_command);
  free(progress_coverage_next_guarded);
  free(progress_coverage_next_low_cpu);
  free(progress_coverage_next_preview);
  free(progress_coverage_next_state_file);
  free(progress_coverage_next_state_command);
  free(progress_coverage_next_state_refresh);
  free(progress_coverage_next_state_refresh_reason);
  free(progress_recommended_state_refresh_command);
  free(progress_coverage_next_state);
  free(progress_coverage_next_state_stale_reason);
  free(progress_coverage_next_state_phase);
  free(progress_coverage_next_state_child_status);
  free(progress_coverage_next_stop_file);
  free(progress_coverage_next_stop_command);
  free(progress_coverage_next_resume_command);
  free(old_path_probe_command);
  free(old_path_command);
  free(old_path_dry_run_command);
  free(old_path_apply_command);
  free(old_path_next_action);
  free(old_path_next_reason);
  free(old_path_report);
  free(old_path_markdown);
  free(run_command);
  free(status_command);
  free(next_script);
  free(progress_command);
  free(perf_watchlist_command);
  free(perf_watchlist_state);
  free(perf_watchlist_action);
  free(perf_watchlist_action_command);
  free(optimization_action);
  free(optimization_reason);
  free(optimization_command);
  free(optimization_target_command);
  free(optimization_case);
  free(optimization_artifact);
  free(optimization_ny_source);
  free(optimization_c_source);
  free(perf_watchlist_report);
  free(perf_watchlist_markdown);
  free(markdown);
  free(worklist);
  free(worklist_md);
  free(historical_worklist);
  free(historical_worklist_md);
  free(coverage);
  free(plan);
  free(perf_case);
  free(perf_watchlist_case);
  free(latest_report);
  free(latest_full_pressure_report);
  free(progress_latest_full_pressure_reason);
  }

static void selftest_validate_fuzz_all_progress_stale_evidence(
    const char *json, string_list_t *errors, int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  double value = -1.0;
  if (!summary_number_from_report(json, "target_percent", &value) ||
      value != 12.5)
    (void)string_list_push_copy(errors, "stale progress target percent wrong");
  if (!summary_number_from_report(json, "campaign_percent", &value) ||
      value != 12.5)
    (void)string_list_push_copy(errors, "stale progress campaign percent wrong");
  if (!summary_number_from_report(json, "latest_report_age_hours", &value) ||
      value < 24.9 || value > 25.1)
    (void)string_list_push_copy(errors, "stale progress latest age wrong");
  if (!summary_number_from_report(json, "latest_h", &value) ||
      value < 24.9 || value > 25.1)
    (void)string_list_push_copy(errors,
                                "stale progress compact latest_h wrong");
  if (!summary_number_from_report(json,
                                  "latest_full_pressure_report_age_hours",
                                  &value) ||
      value < 72.9 || value > 73.1)
    (void)string_list_push_copy(errors, "stale progress full-pressure age wrong");
  if (!summary_number_from_report(json, "full_h", &value) ||
      value < 72.9 || value > 73.1)
    (void)string_list_push_copy(errors,
                                "stale progress compact full_h wrong");
  if (!summary_number_from_report(json, "freshness_penalty", &value) ||
      value != 20.0)
    (void)string_list_push_copy(errors, "stale progress freshness penalty wrong");
  if (!summary_number_from_report(json, "language_score_percent", &value) ||
      value < 53.7 || value > 53.8)
    (void)string_list_push_copy(errors, "stale progress language score wrong");
  if (!summary_number_from_report(json, "score", &value) ||
      value < 53.7 || value > 53.8)
    (void)string_list_push_copy(errors,
                                "stale progress compact score wrong");
  if (!summary_number_from_report(json, "over_h", &value) ||
      value < 0.9 || value > 1.1)
    (void)string_list_push_copy(errors,
                                "stale progress compact over_h wrong");
  if (!summary_number_from_report(json, "campaign_remaining_percent",
                                  &value) ||
      value < 87.49 || value > 87.51)
    (void)string_list_push_copy(errors,
                                "stale progress campaign remaining wrong");
  if (!summary_number_from_report(json, "language_score_gap_percent",
                                  &value) ||
      value < 21.2 || value > 21.3)
    (void)string_list_push_copy(errors, "stale progress language gap wrong");
  bool latest_fresh = true, full_fresh = true, evidence_fresh = true;
  bool strict = false, allow_incomplete_coverage = true;
  bool allow_full_pressure_remediation = false;
  bool status_refresh_attempted = true;
  if (!summary_bool_from_report(json, "latest_report_fresh", &latest_fresh) ||
      latest_fresh)
    (void)string_list_push_copy(errors, "stale progress latest freshness flag wrong");
  if (!summary_bool_from_report(json, "latest_full_pressure_report_fresh",
                                &full_fresh) || full_fresh)
    (void)string_list_push_copy(errors, "stale progress full-pressure freshness flag wrong");
  if (!summary_bool_from_report(json, "evidence_fresh", &evidence_fresh) ||
      evidence_fresh)
    (void)string_list_push_copy(errors, "stale progress evidence freshness flag wrong");
  if (!summary_bool_from_report(json, "strict", &strict) || !strict)
    (void)string_list_push_copy(errors, "stale progress strict flag missing");
  if (!summary_bool_from_report(json, "allow_incomplete_coverage",
                                &allow_incomplete_coverage) ||
      allow_incomplete_coverage)
    (void)string_list_push_copy(errors,
                                "stale progress incomplete-coverage flag wrong");
  if (!summary_bool_from_report(json, "allow_full_pressure_remediation",
                                &allow_full_pressure_remediation) ||
      !allow_full_pressure_remediation)
    (void)string_list_push_copy(errors,
                                "stale progress remediation flag wrong");
  if (!summary_bool_from_report(json, "status_refresh_attempted",
                                &status_refresh_attempted) ||
      status_refresh_attempted)
    (void)string_list_push_copy(errors,
                                "stale progress status refresh attempted flag wrong");
  char *label = summary_string_from_report(json, "language_score_label");
  if (!label || strcmp(label, "shaky") != 0)
    (void)string_list_push_copy(errors, "stale progress language label wrong");
  char *score_label = summary_string_from_report(json, "score_label");
  if (!score_label || strcmp(score_label, "shaky") != 0)
    (void)string_list_push_copy(errors,
                                "stale progress compact score label wrong");
  char *note = summary_string_from_report(json, "language_score_note");
  if (!note || !strstr(note, "stale latest/full-pressure evidence"))
    (void)string_list_push_copy(errors, "stale progress note wrong");
  char *freshness_action =
      summary_string_from_report(json, "freshness_action_command");
  if (!freshness_action || !selftest_command_uses_env_nice(freshness_action) ||
      strstr(freshness_action, "NYTRIX_RUN_REPEAT=") ||
      !strstr(freshness_action, "build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(errors,
                                "stale progress freshness action missing");
  char *latest_freshness_action =
      summary_string_from_report(json, "latest_report_freshness_command");
  if (!latest_freshness_action ||
      !selftest_command_uses_env_nice(latest_freshness_action) ||
      strstr(latest_freshness_action, "NYTRIX_RUN_REPEAT=") ||
      !strstr(latest_freshness_action, "build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(
        errors, "stale progress latest freshness command missing");
  char *full_pressure_freshness_action = summary_string_from_report(
      json, "latest_full_pressure_report_freshness_command");
  if (!full_pressure_freshness_action ||
      !selftest_command_uses_env_nice(full_pressure_freshness_action) ||
      strstr(full_pressure_freshness_action, "NYTRIX_RUN_REPEAT=") ||
      !strstr(full_pressure_freshness_action, "build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(
        errors, "stale progress full-pressure freshness command missing");
  char *full_pressure_freshen_command =
      summary_string_from_report(json, "full_pressure_freshen_command");
  if (!full_pressure_freshen_command ||
      !selftest_command_uses_env_nice(full_pressure_freshen_command) ||
      strstr(full_pressure_freshen_command, "NYTRIX_RUN_REPEAT=") ||
      !strstr(full_pressure_freshen_command, "build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(
        errors, "stale progress full-pressure freshen command missing");
  char *full_pressure_remediation_command =
      summary_string_from_report(json, "full_pressure_remediation_command");
  if (!full_pressure_remediation_command ||
      !selftest_command_uses_env_nice(full_pressure_remediation_command) ||
      strstr(full_pressure_remediation_command, "NYTRIX_RUN_REPEAT=") ||
      !strstr(full_pressure_remediation_command, "build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(
        errors, "stale progress full-pressure remediation command missing");
  char *full_pressure_action_command =
      summary_string_from_report(json, "full_pressure_action_command");
  if (!full_pressure_action_command ||
      !selftest_command_uses_env_nice(full_pressure_action_command) ||
      strstr(full_pressure_action_command, "NYTRIX_RUN_REPEAT=") ||
      !strstr(full_pressure_action_command, "build/fuzz/all/run-next.sh"))
    (void)string_list_push_copy(
        errors, "stale progress full-pressure action command missing");
  char *recommended_action =
      summary_string_from_report(json, "recommended_action");
  char *preview_command = summary_string_from_report(json, "preview_command");
  bool freshness_selected =
      recommended_action && strcmp(recommended_action,
                                   "freshen-evidence") == 0;
  if (freshness_selected &&
      (!preview_command || !selftest_command_uses_env_nice(preview_command) ||
       !strstr(preview_command, "NYTRIX_RUN_DRY_RUN=1") ||
       strstr(preview_command, "NYTRIX_RUN_REPEAT=") ||
       !strstr(preview_command, "build/fuzz/all/run-next.sh")))
    (void)string_list_push_copy(
        errors, "stale progress preview should follow freshness handoff");
  char *state_refresh_command =
      summary_string_from_report(json, "state_refresh_command");
  bool state_readable = false, state_fresh = false, state_live = false;
  if (!summary_bool_from_report(json, "state_readable", &state_readable) ||
      !summary_bool_from_report(json, "state_fresh", &state_fresh) ||
      !summary_bool_from_report(json, "state_live", &state_live))
    (void)string_list_push_copy(errors,
                                "stale progress state freshness fields missing");
  bool expect_state_refresh =
      !state_readable || (!state_live && !state_fresh);
  if (!expect_state_refresh &&
      state_refresh_command && *state_refresh_command)
    (void)string_list_push_copy(
        errors, "stale progress fresh/live state exposed refresh command");
  if (freshness_selected && expect_state_refresh &&
      (!state_refresh_command ||
       !selftest_command_uses_env_nice(state_refresh_command) ||
       !strstr(state_refresh_command, "NYTRIX_RUN_DRY_RUN=1") ||
       strstr(state_refresh_command, "NYTRIX_RUN_REPEAT=") ||
       !strstr(state_refresh_command, "build/fuzz/all/run-next.sh")))
    (void)string_list_push_copy(
        errors, "stale progress state refresh should follow freshness preview");
  char *markdown = summary_string_from_report(json, "markdown");
  char markdown_abs[4096] = {0};
  if (markdown && *markdown) {
    if (path_is_absolute(markdown)) {
      snprintf(markdown_abs, sizeof(markdown_abs), "%s", markdown);
    } else {
      char root[4096];
      if (find_nytrix_root(root, sizeof(root)))
        (void)path_join(markdown_abs, sizeof(markdown_abs), root, markdown);
    }
  }
  file_buf_t md = {0};
  if (!markdown_abs[0] || !read_file(markdown_abs, &md) || !md.data) {
    (void)string_list_push_copy(errors,
                                "stale progress markdown was not readable");
  } else if (!strstr(md.data, "/24h stale") ||
             !strstr(md.data, "/72h stale") ||
             !strstr(md.data, "remaining 87.5000%") ||
             !strstr(md.data, "gap 21.25%") ||
             !strstr(md.data, "freshness 20.00") ||
             !strstr(md.data, "stale latest/full-pressure evidence") ||
             !strstr(md.data, "Freshen evidence:") ||
             !strstr(md.data, "NYTRIX_LOW_PRIORITY=1") ||
             !strstr(md.data, "build/fuzz/all/run-next.sh")) {
    (void)string_list_push_copy(
        errors, "stale progress markdown omitted freshness penalty");
  }
  if (md.data && strstr(md.data, "NYTRIX_RUN_REPEAT="))
    (void)string_list_push_copy(
        errors, "stale progress markdown should not suggest repeat handoff");
  free(md.data);
  free(markdown);
  free(label);
  free(score_label);
  free(note);
  free(freshness_action);
  free(latest_freshness_action);
  free(full_pressure_freshness_action);
  free(full_pressure_freshen_command);
  free(full_pressure_remediation_command);
  free(full_pressure_action_command);
  free(recommended_action);
  free(preview_command);
  free(state_refresh_command);
}

static void selftest_validate_fuzz_all_progress_refresh_fail(
    const char *json, string_list_t *errors, int *row_count) {
  *row_count = -1;
  const char *rows = json_top_level_value_after_key(json, "rows");
  const char *failures = json_top_level_value_after_key(json, "failures");
  const char *summary = json_top_level_value_after_key(json, "summary");
  if (!rows || *rows != '[')
    (void)string_list_push_copy(errors, "progress refresh-fail rows missing");
  else
    *row_count = count_json_array_items(rows);
  if (!failures || *failures != '[')
    (void)string_list_push_copy(errors, "progress refresh-fail failures missing");
  else if (!json_failures_nonempty(json))
    (void)string_list_push_copy(errors, "progress refresh-fail did not fail closed");
  if (!summary || *summary != '{')
    (void)string_list_push_copy(errors, "progress refresh-fail summary missing");

  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-all-progress") != 0)
    (void)string_list_push_copy(errors, "progress refresh-fail mode mismatch");
  bool refreshed = false, ready = true, strict = false;
  bool status_refresh_attempted = false;
  bool allow_incomplete_coverage = true;
  bool allow_full_pressure_remediation = false;
  if (!summary_bool_from_report(json, "refreshed", &refreshed) || !refreshed)
    (void)string_list_push_copy(errors, "progress refresh-fail refreshed flag missing");
  if (!summary_bool_from_report(json, "ready", &ready) || ready)
    (void)string_list_push_copy(errors, "progress refresh-fail ready flag wrong");
  if (!summary_bool_from_report(json, "status_refresh_attempted",
                                &status_refresh_attempted) ||
      !status_refresh_attempted)
    (void)string_list_push_copy(errors,
                                "progress refresh-fail attempted flag missing");
  if (!summary_bool_from_report(json, "strict", &strict) || !strict)
    (void)string_list_push_copy(errors, "progress refresh-fail strict flag missing");
  if (!summary_bool_from_report(json, "allow_incomplete_coverage",
                                &allow_incomplete_coverage) ||
      allow_incomplete_coverage)
    (void)string_list_push_copy(errors,
                                "progress refresh-fail incomplete-coverage flag wrong");
  if (!summary_bool_from_report(json, "allow_full_pressure_remediation",
                                &allow_full_pressure_remediation) ||
      !allow_full_pressure_remediation)
    (void)string_list_push_copy(errors,
                                "progress refresh-fail remediation flag wrong");
  double failure_count = 0.0, status_refresh_rc = 0.0, blocker_count = 0.0;
  if (!summary_number_from_report(json, "failure_count", &failure_count) ||
      failure_count < 1.0)
    (void)string_list_push_copy(errors, "progress refresh-fail failure count wrong");
  if (!summary_number_from_report(json, "status_refresh_rc",
                                  &status_refresh_rc) ||
      status_refresh_rc < 1.0)
    (void)string_list_push_copy(errors, "progress refresh-fail status rc wrong");
  if (!summary_number_from_report(json, "blocker_count", &blocker_count) ||
      blocker_count < 1.0)
    (void)string_list_push_copy(errors, "progress refresh-fail blocker count wrong");
  if (!strstr(json, "status refresh failed rc="))
    (void)string_list_push_copy(errors, "progress refresh-fail reason missing");
  free(mode);
}

static void append_json_string_list(str_buf_t *b, const string_list_t *items) {
  append_string_list_json(b, items);
}

static void append_tail_json_str(str_buf_t *b, const char *s, size_t limit) {
  size_t n = s ? strlen(s) : 0;
  const char *start = s ? s : "";
  if (n > limit) start += n - limit;
  (void)sb_append_json_str(b, start);
}

static void print_tail_text(FILE *out, const char *s, size_t limit) {
  if (!out || !s || !*s) return;
  size_t n = strlen(s);
  const char *start = s;
  if (n > limit) start += n - limit;
  fputs(start, out);
  if (n && s[n - 1] != '\n') fputc('\n', out);
}

static char *selftest_failure_json(const char *name, const string_list_t *errors,
                                   const char *stderr_text) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"name\":");
  (void)sb_append_json_str(&b, name);
  (void)sb_append(&b, ",\"errors\":");
  append_json_string_list(&b, errors);
  (void)sb_append(&b, ",\"cli_stderr\":");
  append_tail_json_str(&b, stderr_text, 2000);
  (void)sb_append(&b, "}");
  return sb_take(&b);
}

static char *selftest_row_json(const selftest_spec_t *spec, bool ok, int cli_rc,
                               double timeout_s, int cli_rows, int error_count,
                               const char *root, const char *report_path) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"name\":");
  (void)sb_append_json_str(&b, spec->name);
  (void)sb_appendf(&b,
                   ",\"ok\":%s,\"cli_rc\":%d,"
                   "\"canonical_only\":true,\"slow\":%s,\"timeout_s\":%.2f,"
                   "\"cli_rows\":%d",
                   ok ? "true" : "false", cli_rc, spec->slow ? "true" : "false",
                   timeout_s, cli_rows);
  if (report_path && *report_path) {
    char nytrix_root[4096];
    const char *report_root = root;
    if (find_nytrix_root(nytrix_root, sizeof(nytrix_root)))
      report_root = nytrix_root;
    (void)sb_append(&b, ",\"report\":");
    append_rel_json_str(&b, report_root, report_path);
  }
  (void)sb_appendf(&b, ",\"error_count\":%d}", error_count);
  return sb_take(&b);
}

static void selftest_collect_only(int argc, char **argv, string_list_t *only) {
  for (int i = 3; i < argc; ++i) {
    if (strncmp(argv[i], "--only=", 7) == 0) {
      (void)string_list_push_unique_copy(only, argv[i] + 7);
      continue;
    }
    if (strcmp(argv[i], "--only") == 0) {
      if (i + 1 < argc) {
        (void)string_list_push_unique_copy(only, argv[i + 1]);
        ++i;
      }
    }
  }
}

static bool selftest_prepare_spec(const char *name, const char *work_dir) {
  if (!name) return true;
  if (strcmp(name, "creal_build") == 0 || strcmp(name, "synth_creal") == 0) {
    char *db_path = NULL;
    if (asprintf(&db_path, "%s/creal_functions.json", work_dir ? work_dir : "/tmp") < 0)
      return false;
    const char *db =
      "["
      "{\"function_name\":\"realfunc_bad_pointer\","
      "\"parameter_types\":[\"int *\"],\"return_type\":\"int\","
      "\"function\":\"int realfunc_bad_pointer(int *x) { return *x; }\","
      "\"io_list\":[[[\"1\"],\"1\"]],\"misc\":[],\"src_file\":\"bad.c\","
      "\"include_headers\":[],\"include_sources\":[]},"
      "{\"function_name\":\"realfunc_selftest_add\","
      "\"parameter_types\":[\"int\"],\"return_type\":\"int\","
      "\"function\":\"int realfunc_selftest_add(int x) { return x + 7; }\","
      "\"io_list\":[[[\"1\"],\"8\"],[[\"35\"],\"42\"]],\"misc\":[],"
      "\"src_file\":\"selftest.c\",\"include_headers\":[],\"include_sources\":[]}"
      "]\n";
    bool ok = write_file_text(db_path, db);
    free(db_path);
    return ok;
  }
  if (strcmp(name, "bridge_unsupported_diagnostic") == 0) {
    char *source_path = NULL;
    if (asprintf(&source_path, "%s/unsupported_struct.c", work_dir ? work_dir : "/tmp") < 0)
      return false;
    const char *source =
      "#include <stdio.h>\n"
      "struct Unsupported { int x; };\n"
      "int main(void) { return 0; }\n";
    bool ok = write_file_text(source_path, source);
    free(source_path);
    return ok;
  }
  if (strcmp(name, "fuzz_all_audit") == 0) {
    char *report_path = NULL, *kernel_path = NULL;
    bool path_ok = asprintf(&report_path, "%s/fuzz_all_report.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&kernel_path, "%s/fuzz_all_kernels.json", work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(report_path);
      free(kernel_path);
      return false;
    }
    const char *kernel_json =
      "{\"rows\":[],\"failures\":[],\"summary\":{\"weak_results\":0,"
      "\"low_signal_results\":0,\"strict_signal\":true,\"failure_count\":0},"
      "\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    str_buf_t report = {0};
    (void)sb_append(&report, "{\"rows\":[");
    const char *lanes[] = {
      "corpus_prepare", "workspace_audit", "harness_all", "libs_import",
      "compiler_std_audit", "compiler_findings", "compiler_known_bugs",
      "prove_lab", "perf_triage", "snippets_mixed", "frontend_corpus"
    };
    for (size_t i = 0; i < sizeof(lanes) / sizeof(lanes[0]); ++i) {
      if (i) (void)sb_append_c(&report, ',');
      (void)sb_append(&report, "{\"name\":");
      (void)sb_append_json_str(&report, lanes[i]);
      (void)sb_append(&report, ",\"phase\":\"selftest\",\"ok\":true,\"required\":true,"
                      "\"elapsed_ms\":1,\"sub_rows\":1,\"sub_failures\":0}");
    }
    (void)sb_append(&report, ",{\"name\":\"kernels_smoke\",\"phase\":\"selftest\","
                    "\"ok\":true,\"required\":true,\"elapsed_ms\":2,\"sub_rows\":1,"
                    "\"sub_failures\":0,\"report\":");
    (void)sb_append_json_str(&report, kernel_path ? kernel_path : "");
    (void)sb_append(&report, "}],\"failures\":[],\"summary\":{\"failure_count\":0},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n");
    bool ok = write_file_text(kernel_path, kernel_json) &&
              write_file_text(report_path, report.data ? report.data : "");
    free(report.data);
    free(report_path);
    free(kernel_path);
    return ok;
  }
  if (strcmp(name, "fuzz_all_old_paths_empty_dry_run") == 0 ||
      strcmp(name, "fuzz_all_old_writer_classifier") == 0) {
    char *base_dir = NULL;
    const char *base = strcmp(name, "fuzz_all_old_writer_classifier") == 0 ?
        "fuzz_old_writer_classifier" : "fuzz_old_paths_empty";
    bool path_ok =
        asprintf(&base_dir, "%s/%s", work_dir ? work_dir : "/tmp", base) >= 0;
    bool ok = path_ok && mkdir_p(base_dir);
    free(base_dir);
    return ok;
  }
  if (strcmp(name, "fuzz_all_old_paths") == 0 ||
      strcmp(name, "fuzz_all_old_paths_dry_run") == 0) {
    const char *base = strcmp(name, "fuzz_all_old_paths_dry_run") == 0 ?
        "fuzz_old_paths_dry" : "fuzz_old_paths";
    char *test_dir = NULL, *fuzz_dir = NULL, *cache_dir = NULL;
    char *artifact_dir = NULL, *artifact_file = NULL;
    char *stale_command_artifact = NULL, *stale_run_good_artifact = NULL;
    char *test_marker = NULL, *fuzz_marker = NULL, *cache_marker = NULL;
    bool path_ok =
        asprintf(&test_dir, "%s/%s/fake_nytrix/build/cache/projects/test",
                 work_dir ? work_dir : "/tmp", base) >= 0 &&
        asprintf(&fuzz_dir, "%s/%s/fake_nytrix/fuzz",
                 work_dir ? work_dir : "/tmp", base) >= 0 &&
        asprintf(&cache_dir, "%s/%s/fake_nytrix/build/cache",
                 work_dir ? work_dir : "/tmp", base) >= 0 &&
        asprintf(&artifact_dir, "%s/%s/artifacts",
                 work_dir ? work_dir : "/tmp", base) >= 0 &&
        asprintf(&artifact_file, "%s/stale-report.json",
                 artifact_dir ? artifact_dir : "") >= 0 &&
        asprintf(&stale_command_artifact, "%s/stale-cockpit.md",
                 artifact_dir ? artifact_dir : "") >= 0 &&
        asprintf(&stale_run_good_artifact, "%s/progress-r999.md",
                 artifact_dir ? artifact_dir : "") >= 0 &&
        asprintf(&test_marker, "%s/marker.txt", test_dir ? test_dir : "") >= 0 &&
        asprintf(&fuzz_marker, "%s/marker.txt", fuzz_dir ? fuzz_dir : "") >= 0 &&
        asprintf(&cache_marker, "%s/marker.txt", cache_dir ? cache_dir : "") >= 0;
    bool ok = path_ok &&
              mkdir_p(test_dir) && mkdir_p(fuzz_dir) && mkdir_p(cache_dir) &&
              mkdir_p(artifact_dir) &&
              write_file_text(test_marker, "old test scratch\n") &&
              write_file_text(fuzz_marker, "old fuzz\n") &&
              write_file_text(cache_marker, "old build cache\n") &&
              write_file_text(artifact_file,
                              "{\"wrapper\":\"/home/e/nytrix/build/cache/projects/test/scratch/old.ny\"}\n") &&
                 write_file_text(
                     stale_command_artifact,
                     "- Cockpit: `env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 "
                     "nice -n 10 ./build/nytrix selftest run --only fuzz_all_help "
                     "--only fuzz_all_default_pressure "
                     "--only fuzz_all_full_pressure_remediation "
                     "--only fuzz_all_plan_coverage_next "
                     "--json build/fuzz/all/selftest-cockpit.json "
                     "--markdown build/fuzz/all/selftest-cockpit.md`\n") &&
              write_file_text(
                  stale_run_good_artifact,
                  "NYTRIX_RUN_REPEAT=good ./build/fuzz/all/run-next.sh\n");
    free(test_dir);
    free(fuzz_dir);
    free(cache_dir);
    free(artifact_dir);
    free(artifact_file);
    free(stale_command_artifact);
    free(stale_run_good_artifact);
    free(test_marker);
    free(fuzz_marker);
    free(cache_marker);
    return ok;
  }
  if (strcmp(name, "fuzz_all_reporting") == 0 ||
      strcmp(name, "fuzz_all_status_canonical") == 0 ||
      strcmp(name, "fuzz_all_status_stale_evidence") == 0 ||
      strcmp(name, "fuzz_all_repeat_status_progress") == 0) {
    bool status_canonical =
        strcmp(name, "fuzz_all_status_canonical") == 0;
    bool status_stale =
        strcmp(name, "fuzz_all_status_stale_evidence") == 0;
    bool repeat_status =
        strcmp(name, "fuzz_all_repeat_status_progress") == 0;
    const char *fixture_dir =
        repeat_status ? "fuzz_repeat_status" :
        (status_stale ? "fuzz_status_stale" :
        (status_canonical ? "fuzz_status_canonical" : "fuzz_reporting"));
    char *report_dir = NULL, *report_path = NULL, *state_path = NULL;
    char *no_evidence_path = NULL;
    bool path_ok = asprintf(&report_dir, "%s/%s",
                            work_dir ? work_dir : "/tmp", fixture_dir) >= 0 &&
                   asprintf(&report_path, "%s/%s/full-pressure.json",
                            work_dir ? work_dir : "/tmp", fixture_dir) >= 0 &&
                   asprintf(&state_path, "%s/%s/run-next-state.json",
                            work_dir ? work_dir : "/tmp", fixture_dir) >= 0;
    if (path_ok && status_canonical)
      path_ok = asprintf(&no_evidence_path,
                         "%s/%s/short-afl-no-evidence.json",
                         work_dir ? work_dir : "/tmp", fixture_dir) >= 0;
    if (!path_ok) {
      free(report_dir);
      free(report_path);
      free(state_path);
      free(no_evidence_path);
      return false;
    }
    const char *lanes[] = {
      "compiler_std_audit", "compiler_findings", "compiler_known_bugs",
      "perf_triage", "prove_lab", "synth_mixed", "synth_ir",
      "synth_stress", "synth_pure", "campaign_core", "gc_soak",
      "afl_compiler", "afl_parsers", "nytrix_fuzz", "nytrix_asan",
      "nytrix_ubsan"
    };
    str_buf_t report = {0};
    (void)sb_append(&report, "{\"rows\":[");
    for (size_t i = 0; i < sizeof(lanes) / sizeof(lanes[0]); ++i) {
      if (i) (void)sb_append_c(&report, ',');
      int sub_rows =
          strcmp(lanes[i], "perf_triage") == 0 ? perf_real_case_count() : 2;
      (void)sb_append(&report, "{\"name\":");
      (void)sb_append_json_str(&report, lanes[i]);
      (void)sb_appendf(&report, ",\"phase\":\"selftest\",\"ok\":true,"
                       "\"required\":true,\"elapsed_ms\":1,"
                       "\"sub_rows\":%d,\"sub_failures\":0}",
                       sub_rows);
    }
    (void)sb_appendf(&report,
                    "],\"failures\":[],\"summary\":{\"cases\":%zu,"
                    "\"ok\":%zu,\"failure_count\":0,\"engine\":\"nytrix_core\","
                    "\"mode\":\"fuzz-all\",\"smoke\":false,"
                    "\"full_pressure\":true,\"duration_s\":3600,"
                    "\"budget_s\":3600,\"threads\":4,"
                    "\"finding_live\":0,\"finding_missing\":0,"
                    "\"known_bug_reproduced\":0,"
                    "\"known_bug_fixed_candidates\":0,"
                    "\"known_bug_lost_signal\":0,"
                    "\"known_bug_baseline_failures\":0,"
                    "\"perf_hotspots\":0,\"perf_max_ratio\":0},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n",
                    sizeof(lanes) / sizeof(lanes[0]),
                    sizeof(lanes) / sizeof(lanes[0]));
    const char *state =
        "{\"phase\":\"dry-run\",\"event\":\"preview\","
        "\"timestamp_utc\":\"selftest\","
        "\"cycle\":0,\"cycles\":4,\"heartbeat_s\":0,"
        "\"heartbeat_count\":0,\"child_pid\":0,"
        "\"low_priority\":true,\"nice\":10,"
        "\"load_wait\":true,\"max_load_pct\":75,"
        "\"space_guard\":true,\"min_free_gb\":20,"
            "\"run_lock\":true,\"threads\":\"25%\","
        "\"last_report\":\"\"}\n";
    const char *no_evidence =
        "{\"rows\":["
        "{\"name\":\"corpus_prepare\",\"kind\":\"fuzz-all\","
        "\"phase\":\"setup\",\"ok\":true,\"required\":true,"
        "\"elapsed_ms\":1,\"sub_rows\":1,\"sub_failures\":0},"
        "{\"name\":\"workspace_audit\",\"kind\":\"fuzz-all\","
        "\"phase\":\"setup\",\"ok\":true,\"required\":true,"
        "\"elapsed_ms\":1,\"sub_rows\":1,\"sub_failures\":0},"
        "{\"name\":\"afl\",\"kind\":\"fuzz-all\",\"phase\":\"afl\","
        "\"ok\":true,\"skipped\":true,"
        "\"reason\":\"budget too short for productive AFL lane\"}"
        "],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all\","
        "\"cases\":3,\"ok\":3,\"failure_count\":0,"
        "\"smoke\":false,\"full_pressure\":false,"
        "\"duration_s\":0.01,\"budget_s\":180,\"effective_budget_s\":180,"
        "\"threads\":10,\"lane_filter\":\"afl\","
        "\"lane_filter_active\":true},"
        "\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    bool ok = mkdir_p(report_dir) &&
              write_file_text(report_path, report.data ? report.data : "") &&
              write_file_text(state_path, state);
    if (ok && no_evidence_path)
      ok = write_file_text(no_evidence_path, no_evidence);
    if (ok) {
      time_t stale_time = time(NULL) - 4000;
      struct timespec state_ts[2] = {
        {.tv_sec = stale_time, .tv_nsec = 0},
        {.tv_sec = stale_time, .tv_nsec = 0},
      };
      ok = utimensat(AT_FDCWD, state_path, state_ts, 0) == 0;
    }
    if (ok && status_stale) {
      time_t stale_time = time(NULL) - (80 * 3600);
      struct timespec stale_ts[2] = {
        {.tv_sec = stale_time, .tv_nsec = 0},
        {.tv_sec = stale_time, .tv_nsec = 0},
      };
      ok = utimensat(AT_FDCWD, report_path, stale_ts, 0) == 0;
    }
    free(report.data);
    free(report_dir);
    free(report_path);
    free(state_path);
    free(no_evidence_path);
    return ok;
  }
  if (strcmp(name, "fuzz_repro_ready_missing_wrapper") == 0) {
    char *fixture_dir = NULL, *history_path = NULL, *failed_path = NULL;
    char *reports_dir = NULL, *child_path = NULL;
    char *core_out = NULL, *missing_out = NULL;
    char *core_hangs = NULL, *missing_hangs = NULL;
    char *core_saved = NULL, *missing_saved = NULL;
    bool path_ok = asprintf(&fixture_dir, "%s/fuzz_repro_ready_missing_wrapper", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&history_path, "%s/fuzz_repro_ready_missing_wrapper/history.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&failed_path, "%s/fuzz_repro_ready_missing_wrapper/failed-full-pressure.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&reports_dir, "%s/fuzz_repro_ready_missing_wrapper/reports", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&child_path, "%s/fuzz_repro_ready_missing_wrapper/reports/afl_compiler.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&core_out, "%s/fuzz_repro_ready_missing_wrapper/afl_runs/ny-core", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&missing_out, "%s/fuzz_repro_ready_missing_wrapper/afl_runs/missing-wrapper-selftest", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&core_hangs, "%s/fuzz_repro_ready_missing_wrapper/afl_runs/ny-core/hangs", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&missing_hangs, "%s/fuzz_repro_ready_missing_wrapper/afl_runs/missing-wrapper-selftest/hangs", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&core_saved, "%s/fuzz_repro_ready_missing_wrapper/afl_runs/ny-core/hangs/id:000000,src:000001,time:1,execs:1,op:havoc,rep:1", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&missing_saved, "%s/fuzz_repro_ready_missing_wrapper/afl_runs/missing-wrapper-selftest/hangs/id:000000,src:000002,time:2,execs:2,op:havoc,rep:1", work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(fixture_dir);
      free(history_path);
      free(failed_path);
      free(reports_dir);
      free(child_path);
      free(core_out);
      free(missing_out);
      free(core_hangs);
      free(missing_hangs);
      free(core_saved);
      free(missing_saved);
      return false;
    }

    char *wrapper_dir = nytrix_scratch_pathf(NULL, "afl_wrappers");
    char *core_wrapper = nytrix_scratch_pathf(NULL,
                                             "afl_wrappers/ny-core-normalize.sh");
    char *missing_wrapper = nytrix_scratch_pathf(NULL,
                                                "afl_wrappers/missing-wrapper-selftest-normalize.sh");
    bool wrapper_ok = wrapper_dir && core_wrapper && missing_wrapper &&
                      mkdir_p(wrapper_dir);
    if (wrapper_ok && !exists_path(core_wrapper))
      wrapper_ok = write_file_text(core_wrapper, "#!/usr/bin/env sh\nexit 0\n");
    if (wrapper_ok) {
      (void)chmod(core_wrapper, 0755);
      (void)unlink(missing_wrapper);
    }

    str_buf_t child = {0}, failed = {0}, history = {0};
    (void)sb_append(&child, "{\"rows\":[{\"target\":\"ny-core\","
                    "\"ok\":false,\"executed\":true,\"timed_out\":false,"
                    "\"compile_only\":true,\"saved_hangs\":1,"
                    "\"saved_crashes\":0,\"out\":");
    (void)sb_append_json_str(&child, core_out ? core_out : "");
    (void)sb_append(&child, "},{\"target\":\"missing-wrapper-selftest\","
                    "\"ok\":false,\"executed\":true,\"timed_out\":false,"
                    "\"compile_only\":true,\"saved_hangs\":1,"
                    "\"saved_crashes\":0,\"out\":");
    (void)sb_append_json_str(&child, missing_out ? missing_out : "");
    (void)sb_append(&child, "}],\"failures\":[],\"summary\":{\"failure_count\":2,"
                    "\"mode\":\"fuzz-afl\",\"engine\":\"nytrix_core\"},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n");

    (void)sb_append(&failed, "{\"rows\":[{\"name\":\"afl_compiler\","
                    "\"phase\":\"selftest\",\"ok\":false,\"required\":true,"
                    "\"elapsed_ms\":1,\"sub_rows\":2,\"sub_failures\":2,"
                    "\"report\":");
    (void)sb_append_json_str(&failed, child_path ? child_path : "");
    (void)sb_append(&failed, "}],\"failures\":[{\"name\":\"afl_compiler\","
                    "\"reason\":\"fixture missing one replay wrapper\"}],"
                    "\"summary\":{\"cases\":1,\"ok\":0,\"failure_count\":1,"
                    "\"engine\":\"nytrix_core\",\"mode\":\"fuzz-all\","
                    "\"full_pressure\":true,\"finding_live\":0,"
                    "\"finding_missing\":0,\"known_bug_reproduced\":0,"
                    "\"known_bug_lost_signal\":0,"
                    "\"known_bug_baseline_failures\":0,"
                    "\"perf_hotspots\":0,\"perf_max_ratio\":0},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n");

    (void)sb_append(&history, "{\"rows\":[{\"report\":");
    (void)sb_append_json_str(&history, failed_path ? failed_path : "");
    (void)sb_append(&history, ",\"attention\":true,\"failure_count\":1,"
                    "\"finding_live\":0,\"finding_missing\":0,"
                    "\"known_bug_reproduced\":0,"
                    "\"known_bug_lost_signal\":0,"
                    "\"known_bug_baseline_failures\":0,"
                    "\"perf_hotspots\":0,\"perf_max_ratio\":0}],"
                    "\"failures\":[],\"summary\":{\"mode\":\"fuzz-all-history\","
                    "\"latest_report\":");
    (void)sb_append_json_str(&history, failed_path ? failed_path : "");
    (void)sb_append(&history, ",\"attention_reports\":1,"
                    "\"failure_count\":0,\"engine\":\"nytrix_core\"},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n");

    bool ok = wrapper_ok &&
              mkdir_p(fixture_dir) &&
              mkdir_p(reports_dir) &&
              mkdir_p(core_hangs) &&
              mkdir_p(missing_hangs) &&
              write_file_text(core_saved, "use std.core\n") &&
              write_file_text(missing_saved, "use std.core\n") &&
              write_file_text(child_path, child.data ? child.data : "") &&
              write_file_text(failed_path, failed.data ? failed.data : "") &&
              write_file_text(history_path, history.data ? history.data : "");
    free(child.data);
    free(failed.data);
    free(history.data);
    free(fixture_dir);
    free(history_path);
    free(failed_path);
    free(reports_dir);
    free(child_path);
    free(core_out);
    free(missing_out);
    free(core_hangs);
    free(missing_hangs);
    free(core_saved);
    free(missing_saved);
    free(wrapper_dir);
    free(core_wrapper);
    free(missing_wrapper);
    return ok;
  }
  if (strcmp(name, "fuzz_repro_ready_missing_command") == 0) {
    char *fixture_dir = NULL, *history_path = NULL, *failed_path = NULL;
    char *reports_dir = NULL, *child_path = NULL;
    char *core_out = NULL, *unsafe_out = NULL;
    char *core_hangs = NULL, *unsafe_hangs = NULL;
    char *core_saved = NULL, *unsafe_saved = NULL;
    bool path_ok = asprintf(&fixture_dir, "%s/fuzz_repro_ready_missing_command", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&history_path, "%s/fuzz_repro_ready_missing_command/history.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&failed_path, "%s/fuzz_repro_ready_missing_command/failed-full-pressure.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&reports_dir, "%s/fuzz_repro_ready_missing_command/reports", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&child_path, "%s/fuzz_repro_ready_missing_command/reports/afl_compiler.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&core_out, "%s/fuzz_repro_ready_missing_command/afl_runs/ny-core", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&unsafe_out, "%s/fuzz_repro_ready_missing_command/afl_runs/unsafe-target", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&core_hangs, "%s/fuzz_repro_ready_missing_command/afl_runs/ny-core/hangs", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&unsafe_hangs, "%s/fuzz_repro_ready_missing_command/afl_runs/unsafe-target/hangs", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&core_saved, "%s/fuzz_repro_ready_missing_command/afl_runs/ny-core/hangs/id:000000,src:000001,time:1,execs:1,op:havoc,rep:1", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&unsafe_saved, "%s/fuzz_repro_ready_missing_command/afl_runs/unsafe-target/hangs/id:000000,src:000002,time:2,execs:2,op:havoc,rep:1", work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(fixture_dir);
      free(history_path);
      free(failed_path);
      free(reports_dir);
      free(child_path);
      free(core_out);
      free(unsafe_out);
      free(core_hangs);
      free(unsafe_hangs);
      free(core_saved);
      free(unsafe_saved);
      return false;
    }

    char *wrapper_dir = nytrix_scratch_pathf(NULL, "afl_wrappers");
    char *core_wrapper = nytrix_scratch_pathf(NULL,
                                             "afl_wrappers/ny-core-normalize.sh");
    bool wrapper_ok = wrapper_dir && core_wrapper && mkdir_p(wrapper_dir);
    if (wrapper_ok && !exists_path(core_wrapper))
      wrapper_ok = write_file_text(core_wrapper, "#!/usr/bin/env sh\nexit 0\n");
    if (wrapper_ok) (void)chmod(core_wrapper, 0755);

    str_buf_t child = {0}, failed = {0}, history = {0};
    (void)sb_append(&child, "{\"rows\":[{\"target\":\"ny-core\","
                    "\"ok\":false,\"executed\":true,\"timed_out\":false,"
                    "\"compile_only\":true,\"saved_hangs\":1,"
                    "\"saved_crashes\":0,\"out\":");
    (void)sb_append_json_str(&child, core_out ? core_out : "");
    (void)sb_append(&child, "},{\"target\":\"unsafe/target\","
                    "\"ok\":false,\"executed\":true,\"timed_out\":false,"
                    "\"compile_only\":true,\"saved_hangs\":1,"
                    "\"saved_crashes\":0,\"out\":");
    (void)sb_append_json_str(&child, unsafe_out ? unsafe_out : "");
    (void)sb_append(&child, "}],\"failures\":[],\"summary\":{\"failure_count\":2,"
                    "\"mode\":\"fuzz-afl\",\"engine\":\"nytrix_core\"},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n");

    (void)sb_append(&failed, "{\"rows\":[{\"name\":\"afl_compiler\","
                    "\"phase\":\"selftest\",\"ok\":false,\"required\":true,"
                    "\"elapsed_ms\":1,\"sub_rows\":2,\"sub_failures\":2,"
                    "\"report\":");
    (void)sb_append_json_str(&failed, child_path ? child_path : "");
    (void)sb_append(&failed, "}],\"failures\":[{\"name\":\"afl_compiler\","
                    "\"reason\":\"fixture missing one replay command\"}],"
                    "\"summary\":{\"cases\":1,\"ok\":0,\"failure_count\":1,"
                    "\"engine\":\"nytrix_core\",\"mode\":\"fuzz-all\","
                    "\"full_pressure\":true,\"finding_live\":0,"
                    "\"finding_missing\":0,\"known_bug_reproduced\":0,"
                    "\"known_bug_lost_signal\":0,"
                    "\"known_bug_baseline_failures\":0,"
                    "\"perf_hotspots\":0,\"perf_max_ratio\":0},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n");

    (void)sb_append(&history, "{\"rows\":[{\"report\":");
    (void)sb_append_json_str(&history, failed_path ? failed_path : "");
    (void)sb_append(&history, ",\"attention\":true,\"failure_count\":1,"
                    "\"finding_live\":0,\"finding_missing\":0,"
                    "\"known_bug_reproduced\":0,"
                    "\"known_bug_lost_signal\":0,"
                    "\"known_bug_baseline_failures\":0,"
                    "\"perf_hotspots\":0,\"perf_max_ratio\":0}],"
                    "\"failures\":[],\"summary\":{\"mode\":\"fuzz-all-history\","
                    "\"latest_report\":");
    (void)sb_append_json_str(&history, failed_path ? failed_path : "");
    (void)sb_append(&history, ",\"attention_reports\":1,"
                    "\"failure_count\":0,\"engine\":\"nytrix_core\"},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n");

    bool ok = wrapper_ok &&
              mkdir_p(fixture_dir) &&
              mkdir_p(reports_dir) &&
              mkdir_p(core_hangs) &&
              mkdir_p(unsafe_hangs) &&
              write_file_text(core_saved, "use std.core\n") &&
              write_file_text(unsafe_saved, "use std.core\n") &&
              write_file_text(child_path, child.data ? child.data : "") &&
              write_file_text(failed_path, failed.data ? failed.data : "") &&
              write_file_text(history_path, history.data ? history.data : "");
    free(child.data);
    free(failed.data);
    free(history.data);
    free(fixture_dir);
    free(history_path);
    free(failed_path);
    free(reports_dir);
    free(child_path);
    free(core_out);
    free(unsafe_out);
    free(core_hangs);
    free(unsafe_hangs);
    free(core_saved);
    free(unsafe_saved);
    free(wrapper_dir);
    free(core_wrapper);
    return ok;
  }
  if (strcmp(name, "fuzz_all_full_pressure_remediation") == 0) {
    char *report_dir = NULL, *failed_path = NULL, *clean_path = NULL;
    char *reports_dir = NULL, *logs_dir = NULL, *afl_child_path = NULL;
    char *stdout_log = NULL, *stderr_log = NULL, *command_log = NULL;
    char *afl_out = NULL, *afl_stats = NULL, *afl_hangs = NULL;
    char *afl_saved_hang = NULL, *afl_saved_hang2 = NULL;
    char *afl_hangs_readme = NULL;
    bool path_ok = asprintf(&report_dir, "%s/fuzz_full_pressure_remediation", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&failed_path, "%s/fuzz_full_pressure_remediation/failed-full-pressure.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&clean_path, "%s/fuzz_full_pressure_remediation/latest-smoke.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&reports_dir, "%s/fuzz_full_pressure_remediation/reports", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&logs_dir, "%s/fuzz_full_pressure_remediation/logs", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&afl_child_path, "%s/fuzz_full_pressure_remediation/reports/afl_compiler.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&stdout_log, "%s/fuzz_full_pressure_remediation/logs/afl_compiler.stdout.log", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&stderr_log, "%s/fuzz_full_pressure_remediation/logs/afl_compiler.stderr.log", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&command_log, "%s/fuzz_full_pressure_remediation/logs/afl_compiler.command.txt", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&afl_out, "%s/fuzz_full_pressure_remediation/afl_runs/ny-core", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&afl_stats, "%s/fuzz_full_pressure_remediation/afl_runs/ny-core/plot_data", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&afl_hangs, "%s/fuzz_full_pressure_remediation/afl_runs/ny-core/hangs", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&afl_saved_hang, "%s/fuzz_full_pressure_remediation/afl_runs/ny-core/hangs/id:000000,src:000004,time:1,execs:1,op:havoc,rep:1", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&afl_saved_hang2, "%s/fuzz_full_pressure_remediation/afl_runs/ny-core/hangs/id:000001,src:000004,time:2,execs:2,op:havoc,rep:4", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&afl_hangs_readme, "%s/fuzz_full_pressure_remediation/afl_runs/ny-core/hangs/README.txt", work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(report_dir);
      free(failed_path);
      free(clean_path);
      free(reports_dir);
      free(logs_dir);
      free(afl_child_path);
      free(stdout_log);
      free(stderr_log);
      free(command_log);
      free(afl_out);
      free(afl_stats);
      free(afl_hangs);
      free(afl_saved_hang);
      free(afl_saved_hang2);
      free(afl_hangs_readme);
      return false;
    }
    const char *lanes[] = {
      "compiler_std_audit", "compiler_findings", "compiler_known_bugs",
      "perf_triage", "prove_lab", "synth_mixed", "synth_ir",
      "synth_stress", "synth_pure", "campaign_core", "gc_soak",
      "afl_compiler", "afl_parsers", "nytrix_fuzz", "nytrix_asan",
      "nytrix_ubsan"
    };
    str_buf_t failed = {0}, clean = {0};
    (void)sb_append(&failed, "{\"rows\":[");
    (void)sb_append(&clean, "{\"rows\":[");
    int failed_ok = 0;
    for (size_t i = 0; i < sizeof(lanes) / sizeof(lanes[0]); ++i) {
      bool lane_ok = strcmp(lanes[i], "afl_compiler") != 0;
      int sub_rows =
          strcmp(lanes[i], "perf_triage") == 0 ? perf_real_case_count() : 2;
      if (i) {
        (void)sb_append_c(&failed, ',');
        (void)sb_append_c(&clean, ',');
      }
      (void)sb_append(&failed, "{\"name\":");
      (void)sb_append_json_str(&failed, lanes[i]);
      (void)sb_appendf(&failed,
                       ",\"phase\":\"selftest\",\"ok\":%s,"
                       "\"required\":true,\"elapsed_ms\":1,"
                       "\"sub_rows\":%d,\"sub_failures\":%d}",
                       lane_ok ? "true" : "false", sub_rows,
                       lane_ok ? 0 : 1);
      if (!lane_ok) {
        size_t len = failed.len;
        if (len > 0 && failed.data && failed.data[len - 1] == '}') {
          failed.data[--failed.len] = '\0';
          (void)sb_append(&failed, ",\"report\":");
          (void)sb_append_json_str(&failed, afl_child_path ? afl_child_path : "");
          (void)sb_append(&failed, ",\"stdout_log\":");
          (void)sb_append_json_str(&failed, stdout_log ? stdout_log : "");
          (void)sb_append(&failed, ",\"stderr_log\":");
          (void)sb_append_json_str(&failed, stderr_log ? stderr_log : "");
          (void)sb_append(&failed, ",\"command_log\":");
          (void)sb_append_json_str(&failed, command_log ? command_log : "");
          (void)sb_append_c(&failed, '}');
        }
      }
      if (lane_ok) ++failed_ok;
      (void)sb_append(&clean, "{\"name\":");
      (void)sb_append_json_str(&clean, lanes[i]);
      (void)sb_append(&clean,
                      ",\"phase\":\"selftest\",\"ok\":true,"
                      "\"required\":true,\"elapsed_ms\":1,"
                      "\"sub_rows\":1,\"sub_failures\":0}");
    }
    size_t lane_count = sizeof(lanes) / sizeof(lanes[0]);
    (void)sb_appendf(&failed,
                    "],\"failures\":[{\"name\":\"afl_compiler\","
                    "\"reason\":\"fixture stale full-pressure failure\"}],"
                    "\"summary\":{\"cases\":%zu,\"ok\":%d,"
                    "\"failure_count\":1,\"engine\":\"nytrix_core\","
                    "\"mode\":\"fuzz-all\",\"smoke\":false,"
                    "\"full_pressure\":true,\"duration_s\":3600,"
                    "\"budget_s\":3600,\"threads\":4,"
                    "\"finding_live\":0,\"finding_missing\":0,"
                    "\"known_bug_reproduced\":0,"
                    "\"known_bug_fixed_candidates\":0,"
                    "\"known_bug_lost_signal\":0,"
                    "\"known_bug_baseline_failures\":0,"
                    "\"perf_hotspots\":0,\"perf_max_ratio\":0},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n",
                    lane_count, failed_ok);
    (void)sb_appendf(&clean,
                    "],\"failures\":[],\"summary\":{\"cases\":%zu,"
                    "\"ok\":%zu,\"failure_count\":0,"
                    "\"engine\":\"nytrix_core\",\"mode\":\"fuzz-all\","
                    "\"smoke\":true,\"full_pressure\":false,"
                    "\"duration_s\":60,\"budget_s\":60,\"threads\":1,"
                    "\"finding_live\":0,\"finding_missing\":0,"
                    "\"known_bug_reproduced\":0,"
                    "\"known_bug_fixed_candidates\":0,"
                    "\"known_bug_lost_signal\":0,"
                    "\"known_bug_baseline_failures\":0,"
                    "\"perf_hotspots\":0,\"perf_max_ratio\":0},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n",
                    lane_count, lane_count);
    str_buf_t child = {0};
    (void)sb_append(&child,
                    "{\"rows\":[{\"target\":\"ny-core\",\"ok\":false,"
                    "\"executed\":true,\"rc\":0,\"timed_out\":false,"
                    "\"compile_only\":true,"
                    "\"saved_hangs\":2,\"saved_crashes\":0,\"out\":");
    (void)sb_append_json_str(&child, afl_out ? afl_out : "");
    (void)sb_append(&child, ",\"stats\":");
    (void)sb_append_json_str(&child, afl_stats ? afl_stats : "");
    (void)sb_append(&child, "}],\"failures\":[],\"summary\":{\"failure_count\":1,"
                    "\"mode\":\"fuzz-afl\",\"engine\":\"nytrix_core\"},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n");
    char *wrapper_dir = nytrix_scratch_pathf(NULL, "afl_wrappers");
    char *wrapper_path = nytrix_scratch_pathf(NULL,
                                             "afl_wrappers/ny-core-normalize.sh");
    bool wrapper_ok = wrapper_dir && wrapper_path && mkdir_p(wrapper_dir);
    if (wrapper_ok) {
      wrapper_ok = write_file_text(wrapper_path,
                                   "#!/usr/bin/env sh\n"
                                   "if [ \"${NYTRIX_AFL_RAW:-0}\" = \"1\" ]; then exit 0; fi\n"
                                   "exit 0\n");
    }
    if (wrapper_ok) (void)chmod(wrapper_path, 0755);
    bool ok = mkdir_p(report_dir) &&
              wrapper_ok &&
              mkdir_p(reports_dir) &&
              mkdir_p(logs_dir) &&
              mkdir_p(afl_out) &&
              mkdir_p(afl_hangs) &&
              write_file_text(stdout_log, "fixture stdout\n") &&
              write_file_text(stderr_log, "fixture stderr\n") &&
              write_file_text(command_log, "fixture command\n") &&
              write_file_text(afl_saved_hang, "use std.core\n") &&
              write_file_text(afl_saved_hang2, "use std.core\n") &&
              write_file_text(afl_hangs_readme, "not an AFL testcase\n") &&
              write_file_text(afl_stats, "unix_time,cycles_done\n") &&
              write_file_text(afl_child_path, child.data ? child.data : "") &&
              write_file_text(failed_path, failed.data ? failed.data : "") &&
              write_file_text(clean_path, clean.data ? clean.data : "");
    if (ok) {
      time_t now = time(NULL);
      struct timespec clean_ts[2] = {
        {.tv_sec = now - 10, .tv_nsec = 0},
        {.tv_sec = now - 10, .tv_nsec = 0},
      };
      struct timespec failed_ts[2] = {
        {.tv_sec = now - 20, .tv_nsec = 0},
        {.tv_sec = now - 20, .tv_nsec = 0},
      };
      (void)utimensat(AT_FDCWD, clean_path, clean_ts, 0);
      (void)utimensat(AT_FDCWD, failed_path, failed_ts, 0);
    }
    free(failed.data);
    free(clean.data);
    free(child.data);
    free(report_dir);
    free(failed_path);
    free(clean_path);
    free(reports_dir);
    free(logs_dir);
    free(afl_child_path);
    free(stdout_log);
    free(stderr_log);
    free(command_log);
    free(afl_out);
    free(afl_stats);
    free(afl_hangs);
    free(afl_saved_hang);
    free(afl_saved_hang2);
    free(afl_hangs_readme);
    free(wrapper_dir);
    free(wrapper_path);
    return ok;
  }
  if (strcmp(name, "fuzz_all_fresh_handoff") == 0) {
    char *report_dir = NULL, *report_path = NULL;
    bool path_ok = asprintf(&report_dir, "%s/fuzz_fresh_handoff", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&report_path, "%s/fuzz_fresh_handoff/smoke-only.json", work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(report_dir);
      free(report_path);
      return false;
    }
    const char *report =
      "{\"rows\":[{\"name\":\"harness_all\",\"phase\":\"selftest\","
      "\"ok\":true,\"required\":true,\"elapsed_ms\":1,"
      "\"sub_rows\":1,\"sub_failures\":0}],"
      "\"failures\":[],"
      "\"summary\":{\"cases\":1,\"ok\":1,\"failure_count\":0,"
      "\"engine\":\"nytrix_core\",\"mode\":\"fuzz-all\","
      "\"smoke\":true,\"full_pressure\":false,"
      "\"duration_s\":1,\"budget_s\":1,\"threads\":1,"
      "\"finding_live\":0,\"finding_missing\":0,"
      "\"known_bug_reproduced\":0,\"known_bug_fixed_candidates\":0,"
      "\"known_bug_lost_signal\":0,\"known_bug_baseline_failures\":0,"
      "\"perf_hotspots\":0,\"perf_max_ratio\":0},"
      "\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    bool ok = mkdir_p(report_dir) && write_file_text(report_path, report);
    free(report_dir);
    free(report_path);
    return ok;
  }
  if (strcmp(name, "fuzz_all_default_pressure") == 0) {
    char *report_dir = NULL, *history_path = NULL, *worklist_path = NULL, *coverage_path = NULL;
    bool path_ok = asprintf(&report_dir, "%s/fuzz_default_pressure", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&history_path, "%s/fuzz_default_pressure/history.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&worklist_path, "%s/fuzz_default_pressure/worklist.json", work_dir ? work_dir : "/tmp") >= 0 &&
                   asprintf(&coverage_path, "%s/fuzz_default_pressure/coverage.json", work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(report_dir);
      free(history_path);
      free(worklist_path);
      free(coverage_path);
      return false;
    }
    const char *history =
      "{\"rows\":[],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all-history\","
      "\"thread_years\":0,\"thread_hours\":0,\"checked_subcases\":0,"
      "\"failure_count\":0},\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    const char *worklist =
      "{\"rows\":[],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all-worklist\","
      "\"active_items\":0,\"failure_count\":0},"
      "\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    const char *coverage =
      "{\"rows\":[],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all-coverage\","
      "\"blocker_gaps\":0,\"failure_count\":0},"
      "\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    bool ok = mkdir_p(report_dir) &&
              write_file_text(history_path, history) &&
              write_file_text(worklist_path, worklist) &&
              write_file_text(coverage_path, coverage);
    free(report_dir);
    free(history_path);
    free(worklist_path);
    free(coverage_path);
    return ok;
  }
  if (strcmp(name, "fuzz_all_plan_coverage_next") == 0) {
    char *report_dir = NULL, *history_path = NULL;
    char *worklist_path = NULL, *coverage_path = NULL;
    bool path_ok =
        asprintf(&report_dir, "%s/fuzz_plan_coverage_next",
                 work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&history_path, "%s/fuzz_plan_coverage_next/history.json",
                 work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&worklist_path, "%s/fuzz_plan_coverage_next/worklist.json",
                 work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&coverage_path, "%s/fuzz_plan_coverage_next/coverage.json",
                 work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(report_dir);
      free(history_path);
      free(worklist_path);
      free(coverage_path);
      return false;
    }
    const char *history =
      "{\"rows\":[],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all-history\","
      "\"thread_years\":0,\"thread_hours\":0,\"checked_subcases\":0,"
      "\"failure_count\":0},\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    const char *worklist =
      "{\"rows\":[],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all-worklist\","
      "\"active_items\":0,\"failure_count\":0},"
      "\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    const char *coverage =
      "{\"rows\":[],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all-coverage\","
      "\"blocker_gaps\":0,\"coverage_backlog_lanes\":2,"
      "\"coverage_lanes\":45,\"coverage_ran_lanes\":41,"
      "\"coverage_depth_percent\":91.11,\"coverage_percent\":91.11,"
      "\"coverage_not_run_lanes\":4,"
      "\"coverage_queue_count\":2,"
      "\"coverage_queue_non_advisory_count\":2,"
      "\"coverage_queue_advisory_count\":0,"
      "\"coverage_queue_lanes\":\"afl -> compiler_findings\","
      "\"coverage_queue\":["
      "{\"lane\":\"afl\",\"severity\":\"medium\",\"category\":\"afl\","
      "\"reason\":\"budget too short\","
      "\"command\":\"./build/nytrix fuzz all run --only-lane afl --profile insane --hours 1.00 --threads 25% --target-thread-years 0.25 --dir build/fuzz/all --json build/fuzz/all/with-afl.json\"},"
      "{\"lane\":\"compiler_findings\",\"severity\":\"medium\","
      "\"category\":\"compiler\",\"reason\":\"disabled by --no-compiler-findings\","
      "\"command\":\"./build/nytrix compiler findings --timeout-s 15 --json build/fuzz/ultra/compiler-findings-current.json\"}],"
      "\"coverage_next_action\":\"run-missing-evidence\","
      "\"coverage_next_lane\":\"afl\","
      "\"coverage_next_reason\":\"budget too short\","
      "\"coverage_next_command\":\"./build/nytrix fuzz all run --only-lane afl --profile insane --hours 1.00 --threads 25% --target-thread-years 0.25 --dir build/fuzz/all --json build/fuzz/all/with-afl.json\","
      "\"coverage_next_guarded_command\":\"./build/fuzz/all/run-missing-evidence.sh\","
      "\"coverage_next_preview_command\":\"env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/nytrix fuzz all preflight --only-lane afl --profile insane --hours 1.00 --threads 25% --target-thread-years 0.25 --dir build/fuzz/all --work-dir build/cache/scratch --allow-dirty-nytrix-baseline\","
      "\"coverage_next_state_command\":\"jq {state,event,live,fresh,child_status,child_pid,heartbeat_s,heartbeat_count,low_priority,nice,load_wait,max_load_pct,space_guard,min_free_gb,run_lock,hours,threads,profile,json,target_thread_years,timestamp_utc,pid,stop_file,status_report,progress_report,last_report} build/fuzz/all/run-missing-evidence-state.json\","
      "\"coverage_next_state_refresh_command\":\"env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 NYTRIX_RUN_DRY_RUN=1 nice -n 10 ./build/fuzz/all/run-missing-evidence.sh\","
      "\"coverage_next_stop_command\":\"touch build/fuzz/all/missing-evidence-stop\","
      "\"coverage_next_resume_command\":\"rm -f build/fuzz/all/missing-evidence-stop\","
      "\"failure_count\":0},\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    bool ok = mkdir_p(report_dir) &&
              write_file_text(history_path, history) &&
              write_file_text(worklist_path, worklist) &&
              write_file_text(coverage_path, coverage);
    free(report_dir);
    free(history_path);
    free(worklist_path);
    free(coverage_path);
    return ok;
  }
  if (strcmp(name, "fuzz_all_coverage_commands") == 0) {
    char *report_dir = NULL, *report_path = NULL;
    bool path_ok =
        asprintf(&report_dir, "%s/fuzz_coverage_commands",
                 work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&report_path, "%s/fuzz_coverage_commands/all-run.json",
                 work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(report_dir);
      free(report_path);
      return false;
    }
    const char *report =
      "{\"rows\":["
      "{\"name\":\"corpus_prepare\",\"kind\":\"fuzz-all\",\"phase\":\"setup\","
      "\"ok\":true,\"required\":true,\"elapsed_ms\":1,"
      "\"sub_rows\":1,\"sub_failures\":0},"
      "{\"name\":\"nytrix_fuzz\",\"kind\":\"fuzz-all\","
      "\"phase\":\"nytrix\",\"ok\":true,\"skipped\":true,"
      "\"reason\":\"disabled by default; pass --allow-nytrix\"},"
      "{\"name\":\"nytrix_sanitizers\",\"kind\":\"fuzz-all\","
      "\"phase\":\"sanitizer\",\"ok\":true,\"skipped\":true,"
      "\"reason\":\"disabled by --no-sanitizers\"},"
      "{\"name\":\"afl\",\"kind\":\"fuzz-all\",\"phase\":\"afl\","
      "\"ok\":true,\"skipped\":true,\"reason\":\"disabled by --no-afl\"},"
      "{\"name\":\"synth_lanes\",\"kind\":\"fuzz-all\",\"phase\":\"synth\","
      "\"ok\":true,\"skipped\":true,\"reason\":\"disabled by --no-synth\"}"
      "],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all\","
      "\"failure_count\":0},\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    bool ok = mkdir_p(report_dir) && write_file_text(report_path, report);
    free(report_dir);
    free(report_path);
    return ok;
  }
  if (strcmp(name, "fuzz_all_coverage_focus_companions") == 0) {
    char *report_dir = NULL, *history_path = NULL, *report_path = NULL;
    char *focus_dir = NULL, *std_audit_path = NULL;
    bool path_ok =
        asprintf(&report_dir, "%s/fuzz_coverage_focus",
                 work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&history_path, "%s/fuzz_coverage_focus/history.json",
                 work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&report_path, "%s/fuzz_coverage_focus/all-run.json",
                 work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&focus_dir, "%s/ultra", work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&std_audit_path, "%s/ultra/compiler-std-audit.json",
                 work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(report_dir);
      free(history_path);
      free(report_path);
      free(focus_dir);
      free(std_audit_path);
      return false;
    }
    str_buf_t history = {0};
    (void)sb_append(&history, "{\"rows\":[{\"kind\":\"fuzz-all-history\","
                    "\"report\":");
    (void)sb_append_json_str(&history, report_path ? report_path : "");
    (void)sb_append(&history, ",\"ok\":true,\"attention\":false,"
                    "\"failure_count\":0,\"engine\":\"nytrix_core\"}],"
                    "\"failures\":[],\"summary\":{\"mode\":\"fuzz-all-history\","
                    "\"latest_report\":");
    (void)sb_append_json_str(&history, report_path ? report_path : "");
    (void)sb_append(&history, ",\"failure_count\":0,"
                    "\"engine\":\"nytrix_core\"},"
                    "\"meta\":{\"engine\":\"nytrix_core\"}}\n");
    const char *report =
      "{\"rows\":["
      "{\"name\":\"corpus_prepare\",\"kind\":\"fuzz-all\",\"phase\":\"setup\","
      "\"ok\":true,\"required\":true,\"elapsed_ms\":1,"
      "\"sub_rows\":1,\"sub_failures\":0},"
      "{\"name\":\"compiler_std_audit\",\"kind\":\"fuzz-all\","
      "\"phase\":\"compiler\",\"ok\":true,\"skipped\":true,"
      "\"reason\":\"disabled by --no-compiler-audit\"},"
      "{\"name\":\"afl\",\"kind\":\"fuzz-all\",\"phase\":\"afl\","
      "\"ok\":true,\"skipped\":true,"
      "\"reason\":\"budget too short for productive AFL lane\"},"
      "{\"name\":\"afl_compiler\",\"kind\":\"fuzz-all\","
      "\"phase\":\"afl\",\"ok\":true,\"required\":true,"
      "\"elapsed_ms\":2,\"sub_rows\":2,\"sub_failures\":0},"
      "{\"name\":\"afl_parsers\",\"kind\":\"fuzz-all\","
      "\"phase\":\"afl\",\"ok\":true,\"required\":true,"
      "\"elapsed_ms\":2,\"sub_rows\":7,\"sub_failures\":0},"
      "{\"name\":\"synth_lanes\",\"kind\":\"fuzz-all\",\"phase\":\"synth\","
      "\"ok\":true,\"skipped\":true,\"reason\":\"disabled by --no-synth\"}"
      "],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all\","
      "\"failure_count\":0},\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    const char *std_audit =
      "{\"rows\":[{\"name\":\"std_runtime_surface\","
      "\"kind\":\"compiler-std-audit\",\"ok\":true,"
      "\"runtime_surface_state\":\"clean\","
      "\"runtime_wrapper_gap_count\":0}],"
      "\"failures\":[],\"summary\":{\"mode\":\"compiler-std-audit\","
      "\"failure_count\":0,\"runtime_surface_state\":\"clean\","
      "\"runtime_wrapper_gap_count\":0},"
      "\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    bool ok = mkdir_p(report_dir) && mkdir_p(focus_dir) &&
              write_file_text(report_path, report) &&
              write_file_text(history_path, history.data ? history.data : "") &&
              write_file_text(std_audit_path, std_audit);
    free(history.data);
    free(report_dir);
    free(history_path);
    free(report_path);
    free(focus_dir);
    free(std_audit_path);
    return ok;
  }
  if (strcmp(name, "fuzz_all_history_commands") == 0) {
    char *report_dir = NULL, *report_path = NULL, *no_evidence_path = NULL;
    bool path_ok =
        asprintf(&report_dir, "%s/fuzz_history_commands",
                 work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&report_path, "%s/fuzz_history_commands/insane-fixture.json",
                 work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&no_evidence_path,
                 "%s/fuzz_history_commands/short-afl-no-evidence.json",
                 work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(report_dir);
      free(report_path);
      free(no_evidence_path);
      return false;
    }
    const char *report =
      "{\"rows\":["
      "{\"name\":\"corpus_prepare\",\"kind\":\"fuzz-all\","
      "\"phase\":\"setup\",\"ok\":true,\"required\":true,"
      "\"elapsed_ms\":1,\"sub_rows\":1,\"sub_failures\":0},"
      "{\"name\":\"compiler_findings\",\"kind\":\"fuzz-all\","
      "\"phase\":\"compiler\",\"ok\":true,\"required\":true,"
      "\"elapsed_ms\":2,\"sub_rows\":1,\"sub_failures\":0}"
      "],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all\","
      "\"cases\":2,\"ok\":2,\"failure_count\":0,"
      "\"smoke\":true,\"full_pressure\":false,"
      "\"duration_s\":3600,\"budget_s\":3600,\"effective_budget_s\":3600,"
      "\"threads\":1},\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    const char *no_evidence =
      "{\"rows\":["
      "{\"name\":\"corpus_prepare\",\"kind\":\"fuzz-all\","
      "\"phase\":\"setup\",\"ok\":true,\"required\":true,"
      "\"elapsed_ms\":1,\"sub_rows\":1,\"sub_failures\":0},"
      "{\"name\":\"workspace_audit\",\"kind\":\"fuzz-all\","
      "\"phase\":\"setup\",\"ok\":true,\"required\":true,"
      "\"elapsed_ms\":1,\"sub_rows\":1,\"sub_failures\":0},"
      "{\"name\":\"afl\",\"kind\":\"fuzz-all\",\"phase\":\"afl\","
      "\"ok\":true,\"skipped\":true,"
      "\"reason\":\"budget too short for productive AFL lane\"}"
      "],\"failures\":[],\"summary\":{\"mode\":\"fuzz-all\","
      "\"cases\":3,\"ok\":3,\"failure_count\":0,"
      "\"smoke\":false,\"full_pressure\":false,"
      "\"duration_s\":0.01,\"budget_s\":180,\"effective_budget_s\":180,"
      "\"threads\":10,\"lane_filter\":\"afl\","
      "\"lane_filter_active\":true},"
      "\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    bool ok = mkdir_p(report_dir) &&
              write_file_text(report_path, report) &&
              write_file_text(no_evidence_path, no_evidence);
    free(report_dir);
    free(report_path);
    free(no_evidence_path);
    return ok;
  }
  if (strcmp(name, "fuzz_all_preflight_isolation") == 0) {
    char *campaign_dir = NULL, *sentinel_path = NULL;
    bool path_ok =
        asprintf(&campaign_dir, "%s/fuzz_preflight_isolation/campaign",
                 work_dir ? work_dir : "/tmp") >= 0 &&
        asprintf(&sentinel_path,
                 "%s/fuzz_preflight_isolation/campaign/all-smoke.json",
                 work_dir ? work_dir : "/tmp") >= 0;
    if (!path_ok) {
      free(campaign_dir);
      free(sentinel_path);
      return false;
    }
    const char *sentinel =
      "{\"rows\":[{\"name\":\"campaign_sentinel\",\"kind\":\"fuzz-all\","
      "\"phase\":\"setup\",\"ok\":true,\"required\":true,\"elapsed_ms\":1,"
      "\"sub_rows\":1,\"sub_failures\":0}],\"failures\":[],"
      "\"summary\":{\"mode\":\"fuzz-all\",\"failure_count\":0,"
      "\"smoke\":true,\"full_pressure\":false,\"duration_s\":1,"
      "\"budget_s\":1,\"threads\":1,\"lane_filter\":\"all\","
      "\"lane_filter_active\":false,"
      "\"isolation_sentinel\":\"campaign\"},"
      "\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    bool ok = mkdir_p(campaign_dir) && write_file_text(sentinel_path, sentinel);
    free(campaign_dir);
    free(sentinel_path);
    return ok;
  }
  if (strcmp(name, "fuzz_all_progress") == 0 ||
      strcmp(name, "fuzz_all_progress_canonical") == 0 ||
      strcmp(name, "fuzz_all_progress_stale_evidence") == 0) {
    bool stale_evidence =
        strcmp(name, "fuzz_all_progress_stale_evidence") == 0;
    bool progress_side_report = strcmp(name, "fuzz_all_progress") == 0;
    const char *fixture_dir = stale_evidence ?
                                  "fuzz_progress_stale" :
                                  (strcmp(name,
                                          "fuzz_all_progress_canonical") == 0 ?
                                       "fuzz_progress_canonical" :
                                       "fuzz_progress");
    char *report_dir = NULL, *status_path = NULL, *state_path = NULL;
    char *latest_smoke_path = NULL, *latest_full_pressure_path = NULL;
    bool path_ok =
        asprintf(&report_dir, "%s/%s", work_dir ? work_dir : "/tmp",
                 fixture_dir) >= 0 &&
        asprintf(&status_path, "%s/%s/status.json",
                 work_dir ? work_dir : "/tmp", fixture_dir) >= 0 &&
        asprintf(&state_path, "%s/%s/run-next-state.json",
                 work_dir ? work_dir : "/tmp", fixture_dir) >= 0 &&
        asprintf(&latest_smoke_path, "%s/%s/latest-smoke.json",
                 work_dir ? work_dir : "/tmp", fixture_dir) >= 0 &&
        asprintf(&latest_full_pressure_path,
                 "%s/%s/latest-full-pressure.json",
                 work_dir ? work_dir : "/tmp", fixture_dir) >= 0;
    if (!path_ok) {
      free(report_dir);
      free(status_path);
      free(state_path);
      free(latest_smoke_path);
      free(latest_full_pressure_path);
      return false;
    }
    char root[4096] = {0};
    const char *artifact_root = "";
    if (find_nytrix_root(root, sizeof(root))) artifact_root = root;
    char *latest_smoke_rel = rel_path_dup(artifact_root, latest_smoke_path);
    char *latest_full_pressure_rel =
        rel_path_dup(artifact_root, latest_full_pressure_path);
    const char *fixture_report =
      "{\"rows\":[],\"failures\":[],\"summary\":{\"mode\":\"fixture-report\","
      "\"failure_count\":0,\"engine\":\"nytrix_core\"},"
      "\"meta\":{\"engine\":\"nytrix_core\"}}\n";
    str_buf_t state = {0};
    (void)sb_appendf(&state,
      "{\"phase\":\"running\",\"event\":\"heartbeat\","
      "\"timestamp_utc\":\"selftest\","
      "\"cycle\":1,\"cycles\":4,\"heartbeat_s\":300,"
      "\"heartbeat_count\":2,\"child_pid\":%ld,"
      "\"low_priority\":true,\"nice\":10,"
      "\"load_wait\":true,\"max_load_pct\":75,"
      "\"space_guard\":true,\"min_free_gb\":20,"
         "\"run_lock\":true,\"threads\":\"25%%\","
      "\"last_report\":\"latest-smoke.json\"}\n",
      (long)getpid());
    str_buf_t status = {0};
    (void)sb_append(&status,
      "{\"rows\":[{\"kind\":\"fuzz-all-status\",\"ok\":true}],"
      "\"failures\":[],"
      "\"summary\":{\"cases\":1,\"ok\":1,\"failure_count\":0,"
      "\"engine\":\"nytrix_core\",\"mode\":\"fuzz-all-status\","
      "\"target_percent\":12.5,"
      "\"thread_years\":1.25,"
      "\"target_thread_years\":10.0,"
      "\"remaining_thread_years\":8.75,"
      "\"runs_needed\":400,"
      "\"runs_per_day\":2.0,"
      "\"thread_years_per_day\":0.044,"
      "\"wall_hours_needed\":4800,"
      "\"thread_hours_needed\":9600,"
      "\"wall_days_needed\":133.3,"
      "\"reports\":8,"
      "\"ignored_no_evidence_reports\":2,"
      "\"full_pressure_reports\":3,"
      "\"full_pressure_thread_years\":1.125,"
      "\"checked_subcases\":640,"
      "\"coverage_ran_lanes\":41,"
      "\"coverage_lanes\":45,"
      "\"coverage_skipped_lanes\":4,"
      "\"coverage_failed_lanes\":0,"
      "\"coverage_disabled_lanes\":3,"
      "\"coverage_budget_short_lanes\":1,"
      "\"coverage_missing_tool_lanes\":0,"
      "\"coverage_advisory_gaps\":0,"
      "\"coverage_reports_considered\":9,"
      "\"coverage_campaign_reports_considered\":8,"
      "\"coverage_companion_reports_considered\":1,"
      "\"coverage_latest_report_advisory_gaps\":1,"
      "\"coverage_latest_report_companion_skipped_lanes\":1,"
      "\"coverage_backlog_lanes\":2,"
      "\"coverage_queue_count\":2,"
      "\"coverage_queue_non_advisory_count\":1,"
      "\"coverage_queue_advisory_count\":1,"
      "\"coverage_queue_lanes\":\"afl -> nytrix_fuzz\","
      "\"coverage_queue\":["
      "{\"lane\":\"afl\",\"severity\":\"medium\",\"category\":\"afl\","
      "\"reason\":\"budget too short\","
      "\"command\":\"./build/nytrix fuzz all run --only-lane afl --profile insane --hours 8 --threads 25% --dir build/fuzz/all --json build/fuzz/all/with-afl.json\"},"
      "{\"lane\":\"nytrix_fuzz\",\"severity\":\"advisory\","
      "\"category\":\"nytrix\",\"reason\":\"disabled by default; pass --allow-nytrix\","
      "\"command\":\"./build/nytrix fuzz all run --profile insane --hours 8 --threads 25% --dir build/fuzz/all --allow-nytrix --json build/fuzz/all/with-nytrix.json\"}],"
      "\"completion_eta_local\":\"2026-10-20 05:00:00 +0200\","
      "\"ready\":true,"
      "\"blocker_count\":0,"
      "\"active_items\":0,"
      "\"historical_attention_reports\":5,"
      "\"coverage_blocker_gaps\":0,"
      "\"latest_compiler_finding_live\":0,"
      "\"latest_compiler_finding_missing\":0,"
      "\"latest_known_bug_reproduced\":0,"
      "\"latest_known_bug_lost_signal\":0,"
      "\"latest_known_bug_baseline_failures\":0,"
      "\"latest_perf_hotspots\":0,"
      "\"latest_perf_max_ratio\":1.10,"
      "\"latest_perf_max_case\":\"fixture-smoke\","
      "\"latest_full_pressure_perf_hotspots\":0,"
      "\"latest_full_pressure_perf_max_ratio\":1.25,"
      "\"latest_full_pressure_perf_max_case\":\"fixture-poly\","
      "\"latest_full_pressure_perf_rows\":7,"
      "\"current_perf_cases\":7,"
      "\"latest_full_pressure_perf_suite_current\":true,"
      "\"non_reproducing_afl_timeouts\":0,"
      "\"cache_policy_ok\":true,"
      "\"ny_bin_exists\":true,"
        "\"old_nytrix_test_scratch_absent\":true,"
        "\"old_nytrix_fuzz_absent\":true,"
        "\"old_nytrix_build_cache_absent\":false,"
        "\"active_old_nytrix_output_writer_present\":false,"
        "\"campaign_complete\":false,");
    if (progress_side_report) {
      (void)sb_append(&status,
                      "\"next_script\":\"\","
                      "\"next_handoff_command\":\"./build/fuzz/all/run-next.sh\","
                      "\"next_command\":\"env NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 nice -n 10 ./build/fuzz/all/run-next.sh\",");
    } else {
      (void)sb_append(&status,
                      "\"next_script\":\"build/fuzz/all/run-next.sh\",");
    }
    (void)sb_append(&status,
          "\"run_command\":\"./build/nytrix fuzz all run --profile insane --hours 8 --threads 25% --target-thread-years 10 --dir build/fuzz/all --fail-fast --no-nytrix --no-sanitizers\","
        "\"status_command\":\"./build/nytrix fuzz all status --refresh\","
        "\"old_path_command\":\"./build/nytrix fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json build/fuzz/all/old-paths.json --markdown build/fuzz/all/old-paths.md\","
        "\"latest_report\":");
    (void)sb_append_json_str(&status, latest_smoke_rel ? latest_smoke_rel : "");
    (void)sb_append(&status, ",\"latest_full_pressure_report\":");
    (void)sb_append_json_str(&status,
                             latest_full_pressure_rel ?
                                 latest_full_pressure_rel : "");
    (void)sb_append(&status,
      "},\"meta\":{\"engine\":\"nytrix_core\"}}\n");
    bool ok = mkdir_p(report_dir) &&
              write_file_text(latest_smoke_path, fixture_report) &&
              write_file_text(latest_full_pressure_path, fixture_report) &&
              write_file_text(state_path, state.data ? state.data : "") &&
              write_file_text(status_path, status.data ? status.data : "");
    if (ok && stale_evidence) {
      time_t now = time(NULL);
      struct timespec smoke_ts[2] = {
        {.tv_sec = now - (25 * 3600), .tv_nsec = 0},
        {.tv_sec = now - (25 * 3600), .tv_nsec = 0},
      };
      struct timespec full_ts[2] = {
        {.tv_sec = now - (73 * 3600), .tv_nsec = 0},
        {.tv_sec = now - (73 * 3600), .tv_nsec = 0},
      };
      (void)utimensat(AT_FDCWD, latest_smoke_path, smoke_ts, 0);
      (void)utimensat(AT_FDCWD, latest_full_pressure_path, full_ts, 0);
    }
    free(report_dir);
    free(status_path);
    free(state_path);
    free(latest_smoke_path);
    free(latest_full_pressure_path);
    free(latest_smoke_rel);
    free(latest_full_pressure_rel);
    free(state.data);
    free(status.data);
    return ok;
  }
  if (strcmp(name, "reduce_artifact") != 0) return true;
  char *source_path = NULL, *artifact_path = NULL;
  bool path_ok = asprintf(&source_path, "%s/reducer_bad.ny", work_dir ? work_dir : "/tmp") >= 0 &&
                 asprintf(&artifact_path, "%s/reducer_artifact.json", work_dir ? work_dir : "/tmp") >= 0;
  if (!path_ok) {
    free(source_path);
    free(artifact_path);
    return false;
  }
  const char *source =
    "use std.core\n"
    "mut keep = 1\n"
    "print(keep + not_defined_symbol)\n";
  str_buf_t artifact = {0};
  (void)sb_append(&artifact, "{\"source\":");
  (void)sb_append_json_str(&artifact, source_path);
  (void)sb_append(&artifact, ",\"mode\":\"compile_fail\",\"expect_substring\":\"not_defined_symbol\","
                  "\"engine\":\"nytrix_core\"}\n");
  bool ok = write_file_text(source_path, source) &&
            write_file_text(artifact_path, artifact.data ? artifact.data : "{}\n");
  free(source_path);
  free(artifact_path);
  free(artifact.data);
  return ok;
}

static int cmd_public_selftest_run(int argc, char **argv) {
  if (selftest_run_wants_catalog(argc, argv))
    return cmd_public_selftest_catalog(argc, argv);
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    char nytrix_root[4096], sibling[4096];
    bool have_repo = false;
    if (find_nytrix_root(nytrix_root, sizeof(nytrix_root)) &&
        path_join(sibling, sizeof(sibling), nytrix_root, "../nytrix")) {
      have_repo = find_repo_root_from_path(sibling, root, sizeof(root));
    }
    if (!have_repo) {
      printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
      return 2;
    }
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *markdown_path = value_after(argc, argv, 3, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after(argc, argv, 3, "--md", "");
  bool full = has_flag_after(argc, argv, 3, "--full");
  int requested_jobs = atoi(value_after(argc, argv, 3, "--jobs", "1"));
  if (requested_jobs < 1) requested_jobs = 1;
  const char *max_timeout_arg = value_after(argc, argv, 3, "--max-timeout-s", "");
  double max_timeout_s = max_timeout_arg && *max_timeout_arg ? atof(max_timeout_arg) : (full ? 0.0 : 45.0);
  const char *scratch_root = nytrix_scratch_root_arg(argc, argv, 3);
  char *scratch_root_abs = nytrix_absolute_scratch_root(scratch_root);
  string_list_t only = {0}, rows = {0}, failures = {0}, skipped_slow = {0};
  selftest_collect_only(argc, argv, &only);
  char *work_dir = NULL;
  if (scratch_root_abs && *scratch_root_abs) ny_ensure_dir_recursive(scratch_root_abs);
  (void)asprintf(&work_dir, "%s/nytrix_selftest_native_%ld",
                 scratch_root_abs && *scratch_root_abs ? scratch_root_abs : NYTRIX_DEFAULT_SCRATCH_ROOT,
                 (long)getpid());
  if (!work_dir || !mkdir_p(work_dir)) {
    printf("{\"ok\":false,\"error\":\"selftest-workdir-failed\"}\n");
    free(work_dir);
    free(scratch_root_abs);
    string_list_free(&only);
    return 2;
  }

  int spec_count = (int)(sizeof(SELFTEST_SPECS) / sizeof(SELFTEST_SPECS[0]));
  int requested_count = 0;
  for (int i = 0; i < spec_count; ++i) {
    const selftest_spec_t *spec = &SELFTEST_SPECS[i];
    if (!selftest_wanted(&only, spec->name)) continue;
    ++requested_count;
    if (spec->slow && !full) {
      (void)string_list_push_copy(&skipped_slow, spec->name);
      continue;
    }
    double timeout_s = spec->timeout_s;
    if (max_timeout_s > 0.0 && timeout_s > max_timeout_s) timeout_s = max_timeout_s;
      char *case_json = NULL;
      if (strcmp(spec->name, "fuzz_all_progress_canonical") == 0)
        (void)asprintf(&case_json, "%s/fuzz_progress_canonical/progress.json",
                       work_dir);
      else if (strcmp(spec->name, "fuzz_all_status_canonical") == 0)
        (void)asprintf(&case_json, "%s/fuzz_status_canonical/status.json",
                       work_dir);
      else if (strcmp(spec->name, "fuzz_all_status_stale_evidence") == 0)
        (void)asprintf(&case_json, "%s/fuzz_status_stale/status.json",
                       work_dir);
      else if (strcmp(spec->name,
                      "fuzz_all_repeat_status_progress") == 0)
        (void)asprintf(&case_json,
                       "%s/fuzz_repeat_status/repeat-status.json",
                       work_dir);
      else
        (void)asprintf(&case_json, "%s/%s.json", work_dir, spec->name);
    char *cmd_argv[64];
    char *allocated[64];
    int a = 0, allocated_count = 0;
    cmd_argv[a++] = g_self_path;
    for (int j = 0; j < spec->arg_count && a < 63; ++j) {
      char *expanded = selftest_expand_arg(spec->args[j], case_json, work_dir);
      allocated[allocated_count++] = expanded;
      cmd_argv[a++] = expanded;
    }
    if (scratch_root_abs && *scratch_root_abs && selftest_spec_uses_scratch_root(spec->name) && a + 2 < 63) {
      cmd_argv[a++] = "--scratch-root";
      cmd_argv[a++] = scratch_root_abs;
    }
    cmd_argv[a] = NULL;
    string_list_t errors = {0};
    bool isolated_scratch = selftest_spec_uses_scratch_root(spec->name) &&
                            work_dir && *work_dir;
    const char *old_scratch_env = getenv("NYTRIX_SCRATCH_ROOT");
    char *old_scratch_copy = old_scratch_env ? strdup(old_scratch_env) : NULL;
    if (isolated_scratch) (void)setenv("NYTRIX_SCRATCH_ROOT", work_dir, 1);
    bool prepared = selftest_prepare_spec(spec->name, work_dir);
    proc_result_t pr = {0};
    if (prepared) {
      pr = run_proc(cmd_argv, root, timeout_s);
    } else {
      pr.rc = 1;
      pr.err = strdup("selftest fixture prepare failed");
      (void)string_list_push_copy(&errors, "fixture prepare failed");
    }
    if (isolated_scratch) {
      if (old_scratch_copy) (void)setenv("NYTRIX_SCRATCH_ROOT", old_scratch_copy, 1);
      else (void)unsetenv("NYTRIX_SCRATCH_ROOT");
    }
    free(old_scratch_copy);
    int cli_rows = -1;
    int expected_rc = selftest_expected_rc(spec);
    if (pr.rc != expected_rc) {
      char buf[128];
      snprintf(buf, sizeof(buf), "canonical command rc=%d, expected=%d", pr.rc, expected_rc);
      (void)string_list_push_copy(&errors, buf);
    }
    file_buf_t report = {0};
    if (spec->validator == SELFTEST_UNSUPPORTED_STDOUT) {
      selftest_validate_unsupported_stdout(pr.out, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_ALL_HELP) {
      selftest_validate_fuzz_all_help_stdout(pr.out, &errors, &cli_rows);
    } else if (!case_json || !read_file(case_json, &report)) {
      (void)string_list_push_copy(&errors, "json report not written");
      } else if (spec->validator == SELFTEST_SHAPE_AUDIT) {
        selftest_validate_shape_audit(report.data, &errors, &cli_rows);
      } else if (spec->validator == SELFTEST_FUZZ_REPORTING) {
        selftest_validate_fuzz_reporting(report.data, &errors, &cli_rows,
                                         "fuzz_reporting", false, false);
      } else if (spec->validator == SELFTEST_FUZZ_ALL_STATUS_CANONICAL) {
        selftest_validate_fuzz_reporting(report.data, &errors, &cli_rows,
                                         "fuzz_status_canonical", true,
                                         false);
      } else if (spec->validator ==
                 SELFTEST_FUZZ_ALL_STATUS_STALE_EVIDENCE) {
        selftest_validate_fuzz_reporting(report.data, &errors, &cli_rows,
                                         "fuzz_status_stale", true, true);
      } else if (spec->validator ==
                 SELFTEST_FUZZ_ALL_REPEAT_STATUS_PROGRESS) {
        selftest_validate_fuzz_all_repeat_status_progress(report.data,
                                                          &errors,
                                                          &cli_rows);
      } else if (spec->validator == SELFTEST_FUZZ_FRESH_HANDOFF) {
      selftest_validate_fuzz_fresh_handoff(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_FULL_PRESSURE_REMEDIATION) {
      selftest_validate_fuzz_full_pressure_remediation(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_REPRO_READY_MISSING_WRAPPER) {
      selftest_validate_fuzz_repro_ready_missing_wrapper(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_REPRO_READY_MISSING_COMMAND) {
      selftest_validate_fuzz_repro_ready_missing_command(report.data, &errors, &cli_rows);
      } else if (spec->validator == SELFTEST_FUZZ_DEFAULT_PRESSURE) {
        selftest_validate_fuzz_default_pressure(report.data, &errors, &cli_rows);
      } else if (spec->validator == SELFTEST_FUZZ_ALL_PLAN_COVERAGE_NEXT) {
        selftest_validate_fuzz_all_plan_coverage_next(report.data, &errors,
                                                      &cli_rows);
      } else if (spec->validator == SELFTEST_FUZZ_ALL_COVERAGE_COMMANDS) {
        selftest_validate_fuzz_all_coverage_commands(report.data, &errors,
                                                     &cli_rows);
    } else if (spec->validator ==
               SELFTEST_FUZZ_ALL_COVERAGE_FOCUS_COMPANIONS) {
      selftest_validate_fuzz_all_coverage_focus_companions(report.data,
                                                           &errors,
                                                           &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_ALL_HISTORY_COMMANDS) {
      selftest_validate_fuzz_all_history_commands(report.data, &errors,
                                                  &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_ALL_PREFLIGHT_ISOLATION) {
      selftest_validate_fuzz_all_preflight_isolation(report.data, &errors,
                                                     &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_ALL_PROGRESS) {
      selftest_validate_fuzz_all_progress(report.data, &errors, &cli_rows,
                                          "fuzz_progress/progress.md", false);
    } else if (spec->validator == SELFTEST_FUZZ_ALL_PROGRESS_CANONICAL) {
      selftest_validate_fuzz_all_progress(
          report.data, &errors, &cli_rows,
          "fuzz_progress_canonical/progress.md", true);
    } else if (spec->validator ==
               SELFTEST_FUZZ_ALL_PROGRESS_STALE_EVIDENCE) {
      selftest_validate_fuzz_all_progress_stale_evidence(report.data, &errors,
                                                         &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_ALL_PROGRESS_REFRESH_FAIL) {
      selftest_validate_fuzz_all_progress_refresh_fail(report.data, &errors,
                                                       &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_ALL_OLD_PATHS) {
      selftest_validate_fuzz_all_old_paths(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_ALL_OLD_PATHS_DRY_RUN) {
      selftest_validate_fuzz_all_old_paths_dry_run(report.data, &errors,
                                                   &cli_rows);
    } else if (spec->validator ==
               SELFTEST_FUZZ_ALL_OLD_PATHS_EMPTY_DRY_RUN) {
      selftest_validate_fuzz_all_old_paths_empty_dry_run(report.data, &errors,
                                                         &cli_rows);
    } else if (spec->validator ==
               SELFTEST_FUZZ_ALL_OLD_WRITER_CLASSIFIER) {
      selftest_validate_fuzz_all_old_writer_classifier(report.data, &errors,
                                                       &cli_rows);
    } else if (spec->validator == SELFTEST_PERF_TRIAGE_ARGS_REPORT) {
      selftest_validate_perf_triage_args(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_COMPILER_KNOWN_BUGS_REPORT) {
      selftest_validate_compiler_known_bugs(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_COMPILER_STD_AUDIT_REPORT) {
      selftest_validate_compiler_std_audit(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_SYNTH_PRINT_REPORT) {
      selftest_validate_synth_print_report(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_REDUCE_ARTIFACT_REPORT) {
      selftest_validate_reduce_artifact_report(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_CLI_EQUALS_ARGS_REPORT) {
      selftest_validate_cli_equals_args(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_SANITIZER_DRY_RUN) {
      selftest_validate_sanitizer_dry_run(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_AFL_COMPILER_DRY_RUN) {
      selftest_validate_afl_compiler_dry_run(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_FUZZ_GC_CAMPAIGN_COMPACT) {
      selftest_validate_fuzz_gc_campaign_compact(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_SELFTEST_ROW_REPORTS) {
      selftest_validate_selftest_row_reports(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_SELFTEST_SKIP_REPORTS) {
      selftest_validate_selftest_skip_reports(report.data, &errors, &cli_rows);
    } else if (spec->validator == SELFTEST_SELFTEST_CATALOG) {
      selftest_validate_selftest_catalog(report.data, &errors, &cli_rows);
    } else {
      selftest_validate_standard_report(report.data, &errors, &cli_rows);
    }
    bool ok = errors.count == 0;
    const char *row_report =
        spec->validator == SELFTEST_UNSUPPORTED_STDOUT ? "" : case_json;
    (void)string_list_push_take(&rows,
                                selftest_row_json(spec, ok, pr.rc, timeout_s,
                                                  cli_rows, errors.count,
                                                  root, row_report));
    if (!ok) (void)string_list_push_take(&failures, selftest_failure_json(spec->name, &errors, pr.err));
    free(report.data);
    string_list_free(&errors);
    proc_result_free(&pr);
    for (int j = 0; j < allocated_count; ++j) free(allocated[j]);
    free(case_json);
  }

  char nytrix_root[4096];
  const char *artifact_root = root;
  if (find_nytrix_root(nytrix_root, sizeof(nytrix_root)))
    artifact_root = nytrix_root;
  bool markdown_written = true;
  if (markdown_path && *markdown_path) {
    int markdown_ok_count = rows.count - failures.count;
    if (markdown_ok_count < 0) markdown_ok_count = 0;
    markdown_written =
        write_selftest_run_markdown(markdown_path, json_path, artifact_root,
                                    &rows, &failures, &skipped_slow,
                                    markdown_ok_count, full, work_dir,
                                    scratch_root_abs);
    if (!markdown_written)
      (void)string_list_push_take(
          &failures,
          make_worker_failure_row("selftest_run", "selftest-markdown", 1,
                                  "", "failed to write selftest markdown"));
  }
  int ok_count = rows.count - failures.count;
  if (ok_count < 0) ok_count = 0;
  int failure_count = failures.count;
  int skipped_count = skipped_slow.count;
  str_buf_t out = {0};
  (void)sb_appendf(&out,
                   "{\"ok\":%s,\"cases\":%d,\"ok_count\":%d,"
                   "\"failure_count\":%d,"
                   "\"requested_cases\":%d,\"executed_cases\":%d,"
                   "\"skipped_count\":%d,\"skipped_slow_count\":%d,"
                   "\"all_requested_executed\":%s,"
                   "\"skipped_slow\":",
                   failure_count == 0 ? "true" : "false", rows.count,
                   ok_count, failure_count, requested_count, rows.count,
                   skipped_count, skipped_slow.count,
                   skipped_count == 0 ? "true" : "false");
  append_string_list_json(&out, &skipped_slow);
  (void)sb_append(&out, ",\"rows\":");
  append_raw_json_list(&out, &rows);
  (void)sb_append(&out, ",\"failures\":");
  append_raw_json_list(&out, &failures);
  (void)sb_appendf(&out,
                   ",\"summary\":{\"cases\":%d,\"ok\":%d,\"ok_count\":%d,"
                   "\"failure_count\":%d,\"requested_cases\":%d,"
                   "\"executed_cases\":%d,\"skipped_count\":%d,"
                   "\"skipped_slow_count\":%d,\"all_requested_executed\":%s,"
                   "\"skipped_slow\":",
                   rows.count, ok_count, ok_count, failure_count,
                   requested_count, rows.count, skipped_count,
                   skipped_slow.count, skipped_count == 0 ? "true" : "false");
  append_string_list_json(&out, &skipped_slow);
  int summary_jobs = requested_jobs;
  if (summary_jobs > (rows.count > 0 ? rows.count : 1)) summary_jobs = rows.count > 0 ? rows.count : 1;
  (void)sb_appendf(&out, ",\"jobs\":%d,\"full\":%s,\"work_dir\":",
                   summary_jobs, full ? "true" : "false");
  append_rel_json_str(&out, artifact_root, work_dir ? work_dir : "");
  (void)sb_append(&out, ",\"scratch_root\":");
  append_rel_json_str(&out, artifact_root, scratch_root_abs && *scratch_root_abs ? scratch_root_abs : "");
  (void)sb_append(&out, ",\"max_timeout_s\":");
  if (max_timeout_s > 0.0) (void)sb_appendf(&out, "%.2f", max_timeout_s);
  else (void)sb_append(&out, "null");
  if (markdown_path && *markdown_path) {
    (void)sb_append(&out, ",\"markdown\":");
    append_rel_json_str(&out, artifact_root, markdown_path);
    (void)sb_appendf(&out, ",\"markdown_written\":%s",
                     markdown_written ? "true" : "false");
  }
  (void)sb_append(&out, ",\"engine\":\"nytrix_core\"},\"meta\":{\"engine\":\"nytrix_core\",\"selftest_scope\":\"native-supported-commands\"}}");
  char *report_json = sb_take(&out);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
    free(report_json);
    string_list_free(&only); string_list_free(&rows); string_list_free(&failures); string_list_free(&skipped_slow);
    free(scratch_root_abs);
    free(work_dir);
    return 2;
  }
  printf("cases: %d\n", rows.count);
  printf("ok: %d/%d\n", ok_count, rows.count);
  if (failures.count) {
    for (int i = 0; i < failures.count; ++i) {
      char *name = json_string_or_empty(failures.items[i], "name");
      printf("%s: issue(s)\n", name ? name : "");
      free(name);
    }
  }
  int rc = failures.count ? 1 : 0;
  free(report_json);
  string_list_free(&only); string_list_free(&rows); string_list_free(&failures); string_list_free(&skipped_slow);
  free(scratch_root_abs);
  free(work_dir);
  return rc;
}

static int json_failures_nonempty(const char *json) {
  const char *p = json_top_level_value_after_key(json, "failures");
  if (!p || *p != '[') return 0;
  const char *q = matching_json_end(p, '[', ']');
  if (!q) return 0;
  const char *content = skip_ws_const(p + 1);
  while (q > content && isspace((unsigned char)q[-1])) --q;
  return content < q;
}

static char *generated_cases_fingerprint(const generated_case_list_t *cases) {
  str_buf_t b = {0};
  for (int i = 0; i < cases->count; ++i) {
    const generated_case_t *gc = &cases->items[i];
    file_buf_t c = {0}, ny = {0}, ir = {0};
    uint64_t ch = 0, nh = 0, ih = 0;
    if (read_file(gc->c_path, &c)) ch = fnv1a64(c.data, c.len);
    if (read_file(gc->ny_path, &ny)) nh = fnv1a64(ny.data, ny.len);
    if (read_file(gc->ir_path, &ir)) ih = fnv1a64(ir.data, ir.len);
    (void)sb_appendf(&b, "%s:%016" PRIx64 ":%016" PRIx64 ":%016" PRIx64 "\n",
                     gc->name, ch, nh, ih);
    free(c.data); free(ny.data); free(ir.data);
  }
  return sb_take(&b);
}

static char *run_generate_batch_json(const char *root, const char *shape_dir, const char *out_dir,
                                     const char *profile, const char *generator, int cases,
                                     int seed, bool fast, double timeout_s) {
  char cases_buf[32], seed_buf[32];
  snprintf(cases_buf, sizeof(cases_buf), "%d", cases);
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
  char *argv[24];
  int a = 0;
  argv[a++] = g_self_path;
  argv[a++] = "generate-batch";
  argv[a++] = "--shape-dir"; argv[a++] = (char *)shape_dir;
  argv[a++] = "--profile"; argv[a++] = (char *)profile;
  argv[a++] = "--seed"; argv[a++] = seed_buf;
  argv[a++] = "--cases"; argv[a++] = cases_buf;
  argv[a++] = "--out"; argv[a++] = (char *)out_dir;
  argv[a++] = "--generator"; argv[a++] = (char *)generator;
  if (fast) argv[a++] = "--fast";
  argv[a] = NULL;
  proc_result_t pr = run_proc(argv, root, timeout_s > 0.0 ? timeout_s : 0.0);
  char *json = NULL;
  if (pr.rc == 0 && pr.out && strstr(pr.out, "\"cases\"")) json = trim_trailing_copy(pr.out);
  else json = NULL;
  proc_result_free(&pr);
  return json;
}

static void proof_add_row(report_rows_t *report, const char *name, bool ok,
                          bool skipped, const char *detail_key,
                          const char *detail_value, const char *error) {
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, name);
  (void)sb_append(&row, ",\"case\":");
  (void)sb_append_json_str(&row, name);
  (void)sb_appendf(&row, ",\"ok\":%s", ok ? "true" : "false");
  if (skipped) (void)sb_append(&row, ",\"skipped\":true");
  if (detail_key && *detail_key) {
    (void)sb_append_c(&row, ',');
    (void)sb_append_json_str(&row, detail_key);
    (void)sb_append_c(&row, ':');
    (void)sb_append_json_str(&row, detail_value ? detail_value : "");
  }
  (void)sb_append(&row, ",\"failures\":[");
  if (!ok) {
    (void)sb_append(&row, "{\"name\":");
    (void)sb_append_json_str(&row, name);
    (void)sb_append(&row, ",\"error\":");
    (void)sb_append_json_str(&row, error ? error : "proof failed");
    (void)sb_append(&row, ",\"reducer_mode\":\"proof_failure\"}");
  }
  (void)sb_append(&row, "]}");
  report_add_row(report, sb_take(&row));
}

static void proof_add_report_step(report_rows_t *proof, const char *name, const char *json) {
  double failures = 0.0;
  bool has_failure_count = extract_json_number(json, "failure_count", &failures);
  bool ok = (!has_failure_count || failures == 0.0) && !json_failures_nonempty(json);
  proof_add_row(proof, name, ok, false, "engine", "nytrix_core", ok ? NULL : "subreport has failures");
}

static int prove_typed_determinism(const char *root, const char *shape_dir, bool fast,
                                   int seed, double timeout_s, str_buf_t *detail) {
  int cases = fast ? 3 : 8;
  char *out_a = NULL, *out_b = NULL;
  long pid = (long)getpid();
  if (nytrix_asprintf(&out_a, "build/generated/proof/native_det_a_%d_%ld", seed, pid) < 0 ||
      nytrix_asprintf(&out_b, "build/generated/proof/native_det_b_%d_%ld", seed, pid) < 0) {
    free(out_a); free(out_b);
    return 1;
  }
  char *json_a = run_generate_batch_json(root, shape_dir, out_a, "optimizer", "mixed", cases, seed, fast, timeout_s);
  char *json_b = run_generate_batch_json(root, shape_dir, out_b, "optimizer", "mixed", cases, seed, fast, timeout_s);
  generated_case_list_t a = {0}, b = {0};
  int rc = 1;
  if (json_a && json_b && parse_generated_cases(json_a, &a) && parse_generated_cases(json_b, &b)) {
    char *fp_a = generated_cases_fingerprint(&a);
    char *fp_b = generated_cases_fingerprint(&b);
    rc = strcmp(fp_a ? fp_a : "", fp_b ? fp_b : "") == 0 ? 0 : 1;
    if (detail) (void)sb_appendf(detail, "cases=%d", a.count);
    free(fp_a); free(fp_b);
  }
  generated_case_list_free(&a); generated_case_list_free(&b);
  free(json_a); free(json_b); free(out_a); free(out_b);
  return rc;
}

static int prove_typed_canary(const char *root, const char *shape_dir, const char *ny_bin,
                              int seed, double timeout_s) {
  char *out_dir = NULL;
  if (nytrix_asprintf(&out_dir, "build/generated/proof/native_canary_%d_%ld", seed, (long)getpid()) < 0)
    return 1;
  char *json = run_generate_batch_json(root, shape_dir, out_dir, "optimizer", "typed", 1, seed, true, timeout_s);
  generated_case_list_t cases = {0};
  int rc = 1;
  if (json && parse_generated_cases(json, &cases) && cases.count > 0) {
    generated_case_t *gc = &cases.items[0];
    (void)write_file_text(gc->ny_path, "use std.core\nprint(999999)\n");
    char *bin_dir = NULL;
    if (asprintf(&bin_dir, "%s/build", out_dir) >= 0) {
      char timeout_buf[64];
      snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
      char *argv[] = {
        g_self_path, "compare-case",
        "--case", gc->name,
        "--c", gc->c_path,
        "--ny", gc->ny_path,
        "--ir", gc->ir_path,
        "--root", (char *)root,
        "--ny-bin", (char *)ny_bin,
        "--bin-dir", bin_dir,
        "--timeout-s", timeout_buf,
        NULL
      };
      proc_result_t pr = run_proc(argv, root, worker_outer_timeout(timeout_s, 1, 0));
      rc = (pr.out && json_failures_nonempty(pr.out)) ? 0 : 1;
      proc_result_free(&pr);
      free(bin_dir);
    }
  }
  generated_case_list_free(&cases);
  free(json); free(out_dir);
  return rc;
}

static char *build_proof_report_json(const report_rows_t *proof, bool fast, int seed,
                                     const char *proof_dir, const char *ny_bin) {
  int ok = proof->rows.count - proof->failed_rows;
  if (ok < 0) ok = 0;
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"rows\":");
  append_rows_json(&b, &proof->rows);
  (void)sb_append(&b, ",\"failures\":[");
  if (proof->failures_json.data) (void)sb_append(&b, proof->failures_json.data);
  (void)sb_appendf(&b, "],\"summary\":{\"proofs\":%d,\"ok\":%d,\"failure_count\":%d,"
                   "\"fast\":%s,\"seed\":%d,\"engine\":\"nytrix_core\","
                   "\"proof_contract\":[\"shape validation\",\"deterministic typed generation\","
                   "\"typed C-vs-Ny execution\",\"metamorphic negative canary\","
                   "\"bridge suite execution\",\"corpus replay\",\"normalized report schema\"]}",
                   proof->rows.count, ok, proof->failure_count, fast ? "true" : "false", seed);
  (void)sb_append(&b, ",\"meta\":{\"ny_bin\":");
  (void)sb_append_json_str(&b, ny_bin);
  (void)sb_append(&b, ",\"proof_dir\":");
  (void)sb_append_json_str(&b, proof_dir);
  (void)sb_append(&b, "}}");
  return sb_take(&b);
}

static int cmd_public_prove_lab(int argc, char **argv) {
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
  int seed = atoi(value_after(argc, argv, 3, "--seed", "1337"));
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "90"));
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  char *shape_dir = NULL, *proof_dir = NULL;
  if (nytrix_asprintf(&shape_dir, "etc/tests/fuzz/shapes") < 0 ||
      nytrix_asprintf(&proof_dir, "build/generated/proof/native") < 0) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(shape_dir); free(proof_dir);
    return 2;
  }
  report_rows_t proof;
  memset(&proof, 0, sizeof(proof));

  char *argv_shapes[] = {g_self_path, "validate-shapes", shape_dir, NULL};
  proc_result_t shapes = run_proc(argv_shapes, root, timeout_s);
  double shape_errors = 1.0;
  if (shapes.out) (void)extract_json_number(shapes.out, "errors", &shape_errors);
  proof_add_row(&proof, "shape_validation", shapes.rc == 0 && shape_errors == 0.0,
                false, "engine", "nytrix_core", "shape validation failed");
  proc_result_free(&shapes);

  str_buf_t det_detail = {0};
  int det_rc = prove_typed_determinism(root, shape_dir, fast, seed, timeout_s, &det_detail);
  proof_add_row(&proof, "typed_determinism", det_rc == 0, false, "detail",
                det_detail.data ? det_detail.data : "", "same seed generated different bytes");
  free(det_detail.data);

  char *typed_json_path = make_tmp_json_path(root, "proof_typed", seed, 0);
  char *typed_out_dir = NULL, *typed_build_dir = NULL;
  long proof_pid = (long)getpid();
  if (nytrix_asprintf(&typed_out_dir, "build/generated/proof/native_typed_%d_%ld", seed, proof_pid) < 0 ||
      nytrix_asprintf(&typed_build_dir, "build/proof/native_typed_%d_%ld", seed, proof_pid) < 0) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(shape_dir); free(proof_dir); free(typed_json_path);
    free(typed_out_dir); free(typed_build_dir);
    report_rows_free(&proof);
    return 2;
  }
  char seed_buf[32], cases_buf[32], timeout_buf[64];
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed + 17);
  snprintf(cases_buf, sizeof(cases_buf), "%d", fast ? 2 : 5);
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  char *typed_argv[24];
  int ta = 0;
  typed_argv[ta++] = g_self_path; typed_argv[ta++] = "synth"; typed_argv[ta++] = "generate";
  typed_argv[ta++] = "--cases"; typed_argv[ta++] = cases_buf;
  typed_argv[ta++] = "--seed"; typed_argv[ta++] = seed_buf;
  typed_argv[ta++] = "--profile"; typed_argv[ta++] = "optimizer";
  typed_argv[ta++] = "--timeout-s"; typed_argv[ta++] = timeout_buf;
  typed_argv[ta++] = "--runs"; typed_argv[ta++] = "1";
  typed_argv[ta++] = "--warmup"; typed_argv[ta++] = "0";
  typed_argv[ta++] = "--out"; typed_argv[ta++] = typed_out_dir;
  typed_argv[ta++] = "--build-dir"; typed_argv[ta++] = typed_build_dir;
  typed_argv[ta++] = "--json"; typed_argv[ta++] = typed_json_path;
  if (fast) typed_argv[ta++] = "--fast";
  typed_argv[ta] = NULL;
  proc_result_t typed = run_proc(typed_argv, root, worker_outer_timeout(timeout_s, 1, 0));
  file_buf_t typed_report = {0};
  if (typed_json_path && read_file(typed_json_path, &typed_report)) proof_add_report_step(&proof, "typed_execution", typed_report.data);
  else proof_add_row(&proof, "typed_execution", false, false, "engine", "nytrix_core", "typed generation report missing");
  free(typed_report.data); proc_result_free(&typed);
  free(typed_json_path); free(typed_out_dir); free(typed_build_dir);

  int canary_rc = prove_typed_canary(root, shape_dir, ny_bin, seed + 101, timeout_s);
  proof_add_row(&proof, "typed_mismatch_canary", canary_rc == 0, false, "engine", "nytrix_core",
                "intentional mismatch was not detected");

  char *bridge_argv[] = {g_self_path, "bridge", "suite", "--fast", "--timeout-s", timeout_buf, "--json", NULL};
  proc_result_t bridge = run_proc(bridge_argv, root, worker_outer_timeout(timeout_s, 1, 0));
  if (bridge.out && strstr(bridge.out, "\"rows\"")) proof_add_report_step(&proof, "bridge_suite", bridge.out);
  else proof_add_row(&proof, "bridge_suite", false, false, "engine", "nytrix_core", "bridge suite failed");
  proc_result_free(&bridge);

  char *corpus_json_path = make_tmp_json_path(root, "proof_corpus", seed, 0);
  char *corpus_argv[] = {g_self_path, "corpus", "replay", "--limit", "1", "--timeout-s", timeout_buf, "--json", corpus_json_path, NULL};
  proc_result_t corpus = run_proc(corpus_argv, root, worker_outer_timeout(timeout_s, 1, 0));
  file_buf_t corpus_report = {0};
  if (corpus_json_path && read_file(corpus_json_path, &corpus_report)) proof_add_report_step(&proof, "corpus_replay", corpus_report.data);
  else proof_add_row(&proof, "corpus_replay", false, false, "engine", "nytrix_core", "corpus replay report missing");
  free(corpus_report.data); proc_result_free(&corpus); free(corpus_json_path);

  char *report_json = build_proof_report_json(&proof, fast, seed, proof_dir, ny_bin);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    int ok = proof.rows.count - proof.failed_rows;
    if (ok < 0) ok = 0;
    printf("proofs: %d/%d\n", ok, proof.rows.count);
    printf("failures: %d\n", proof.failure_count);
  }
  int rc = proof.failure_count ? 1 : 0;
  free(report_json);
  report_rows_free(&proof);
  free(shape_dir); free(proof_dir);
  return rc;
}

static bool synth_print_kernel_generator(const char *generator) {
  return generator &&
         (strcmp(generator, "kernel") == 0 || strcmp(generator, "kernels") == 0 ||
          strcmp(generator, "training-kernel") == 0 || strcmp(generator, "training-kernels") == 0);
}

static bool shape_file_declares_name(const char *path, const char *shape) {
  if (!shape || !*shape) return true;
  file_buf_t f = {0};
  if (!read_file(path, &f)) return false;
  bool ok = false;
  const char *p = f.data;
  while ((p = strstr(p, "shape ")) != NULL) {
    p += 6;
    while (*p == ' ' || *p == '\t') ++p;
    const char *q = p;
    while (*q && *q != ' ' && *q != '\t' && *q != '{' && *q != '\n') ++q;
    if (strlen(shape) == (size_t)(q - p) && memcmp(p, shape, (size_t)(q - p)) == 0) {
      ok = true;
      break;
    }
    p = q;
  }
  free(f.data);
  return ok;
}

static char *synth_print_embedded_ny_source(const char *shape_dir, const char *shape,
                                            const char *generator, int seed) {
  if ((!shape || !*shape) && !synth_print_kernel_generator(generator)) return NULL;
  char *scan_dir = NULL;
  if (shape && *shape) (void)asprintf(&scan_dir, "%s", shape_dir ? shape_dir : "etc/tests/fuzz/shapes");
  else (void)nytrix_asprintf(&scan_dir, "etc/tests/fuzz/shapes/kernels");
  string_list_t candidates = {0}, files = {0};
  if (scan_dir && collect_regular_files_recursive(scan_dir, &files)) {
    qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
    for (int i = 0; i < files.count; ++i) {
      if (!ny_has_suffix(files.items[i], ".nshape") ||
          !shape_file_declares_name(files.items[i], shape)) continue;
      char *source = nytrix_shape_source_block(files.items[i], "ny");
      if (source) {
        free(source);
        (void)string_list_push_copy(&candidates, files.items[i]);
      }
    }
  }
  char *out = NULL;
  if (candidates.count > 0) {
    uint32_t pick = shape && *shape ? 0u : (uint32_t)seed % (uint32_t)candidates.count;
    out = nytrix_shape_source_block(candidates.items[pick], "ny");
  }
  string_list_free(&candidates);
  string_list_free(&files);
  free(scan_dir);
  return out;
}

static bool synth_print_pair_lang(const char *lang) {
  return lang && (strcmp(lang, "both") == 0 || strcmp(lang, "pair") == 0);
}

static bool synth_print_use_program_c(const char *shape, const char *generator,
                                      bool generator_given) {
  if (generator && strcmp(generator, "program") == 0) return true;
  if (shape && *shape) return strncmp(shape, "program-", 8) == 0;
  return !generator_given;
}

static int cmd_public_synth_print(int argc, char **argv) {
  char root[4096];
  if (!find_nytrix_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nytrix-root-not-found\"}\n");
    return 2;
  }
  const char *generator_arg = value_after(argc, argv, 3, "--generator", "");
  const char *profile = value_after(argc, argv, 3, "--profile", "balanced");
  const char *lang = value_after(argc, argv, 3, "--lang", "c");
  const char *shape = value_after(argc, argv, 3, "--shape", "");
  bool fast = has_flag_after(argc, argv, 3, "--fast");
  bool insane = has_flag_after(argc, argv, 3, "--insane");
  bool generator_given = has_flag_after(argc, argv, 3, "--generator");
  bool list_shapes = has_flag_after(argc, argv, 3, "--list") ||
                     has_flag_after(argc, argv, 3, "--list-shapes");
  bool pair_lang = synth_print_pair_lang(lang);
  const char *generator = generator_given ? generator_arg :
                          ((shape && *shape) || pair_lang || list_shapes ? "auto" : "ir");
  const char *seed_text = value_after(argc, argv, 3, "--seed", NULL);
  int seed = seed_text && *seed_text ?
    atoi(seed_text) :
    (int)(((uint64_t)time(NULL) ^ ((uint64_t)getpid() << 16) ^
           (uint64_t)(now_ms() * 1000.0)) & UINT64_C(0x7fffffff));
  char *shape_dir = NULL, *default_out = NULL;
  bool paths_ok = nytrix_asprintf(&shape_dir, "etc/tests/fuzz/shapes") >= 0 &&
                  (default_out = nytrix_scratch_pathf(NULL,
                                                     "synth_print/nytrix_print_%ld_%d",
                                                     (long)getpid(), seed)) != NULL;
  if (!paths_ok) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(shape_dir); free(default_out);
    return 2;
  }
  if (list_shapes) {
    char *list_argv[14];
    int la = 0;
    list_argv[la++] = g_self_path;
    list_argv[la++] = "generate-batch";
    list_argv[la++] = "--shape-dir"; list_argv[la++] = shape_dir;
    list_argv[la++] = "--profile"; list_argv[la++] = (char *)profile;
    list_argv[la++] = "--generator"; list_argv[la++] = (char *)generator;
    if (shape && *shape) { list_argv[la++] = "--shape"; list_argv[la++] = (char *)shape; }
    list_argv[la++] = "--list";
    list_argv[la] = NULL;
    int rc = cmd_generate_batch(la, list_argv);
    free(shape_dir); free(default_out);
    return rc;
  }
  if (strcmp(lang, "ny") == 0 || strcmp(lang, "nytrix") == 0) {
    char *source = synth_print_embedded_ny_source(shape_dir, shape, generator, seed);
    if (source) {
      fputs(source, stdout);
      if (!*source || source[strlen(source) - 1] != '\n') fputc('\n', stdout);
      free(source);
      free(shape_dir); free(default_out);
      return 0;
    }
  }
  if (strcmp(lang, "c") == 0 && synth_print_use_program_c(shape, generator, generator_given)) {
    int rc = nytrix_synth_print_c_program(stdout, shape_dir, generator, profile,
                                         shape, seed, fast, insane);
    free(shape_dir); free(default_out);
    return rc;
  }
  const char *out_dir = value_after(argc, argv, 3, "--out", default_out);
  char seed_buf[32];
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
  char *gen_argv[24];
  int ga = 0;
  gen_argv[ga++] = g_self_path;
  gen_argv[ga++] = "generate-batch";
  gen_argv[ga++] = "--shape-dir"; gen_argv[ga++] = shape_dir;
  gen_argv[ga++] = "--profile"; gen_argv[ga++] = (char *)profile;
  gen_argv[ga++] = "--seed"; gen_argv[ga++] = seed_buf;
  gen_argv[ga++] = "--cases"; gen_argv[ga++] = "1";
  gen_argv[ga++] = "--out"; gen_argv[ga++] = (char *)out_dir;
  gen_argv[ga++] = "--generator"; gen_argv[ga++] = (char *)generator;
  if (shape && *shape) { gen_argv[ga++] = "--shape"; gen_argv[ga++] = (char *)shape; }
  if (fast) gen_argv[ga++] = "--fast";
  if (insane) gen_argv[ga++] = "--insane";
  gen_argv[ga] = NULL;
  proc_result_t gen = run_proc(gen_argv, root, 30.0);
  generated_case_list_t cases;
  memset(&cases, 0, sizeof(cases));
  if (gen.rc != 0 || !gen.out || !parse_generated_cases(gen.out, &cases) || cases.count <= 0) {
    printf("{\"ok\":false,\"error\":\"program-generation-failed\",\"rc\":%d,\"stderr\":", gen.rc);
    json_str(stdout, gen.err ? gen.err : "");
    printf(",\"stdout\":");
    json_str(stdout, gen.out ? gen.out : "");
    printf("}\n");
    generated_case_list_free(&cases);
    proc_result_free(&gen);
    free(shape_dir); free(default_out);
    return 1;
  }
  const generated_case_t *gc = &cases.items[0];
  if (pair_lang) {
    printf("{\"ok\":true,\"seed\":%d,\"out_dir\":", seed);
    json_str(stdout, out_dir);
    printf(",\"case\":%s}\n", gc->json ? gc->json : "{}");
    generated_case_list_free(&cases);
    proc_result_free(&gen);
    free(shape_dir); free(default_out);
    return 0;
  }
  const char *path = gc->c_path;
  if (strcmp(lang, "ny") == 0 || strcmp(lang, "nytrix") == 0) {
    path = gc->ny_path;
  } else if (strcmp(lang, "ir") == 0 || strcmp(lang, "json") == 0) {
    path = gc->ir_path;
  } else if (strcmp(lang, "c") != 0) {
    printf("{\"ok\":false,\"error\":\"unsupported-language\",\"lang\":");
    json_str(stdout, lang);
    printf(",\"supported\":[\"c\",\"ny\",\"ir\",\"both\"]}\n");
    generated_case_list_free(&cases);
    proc_result_free(&gen);
    free(shape_dir); free(default_out);
    return 2;
  }
  file_buf_t source = {0};
  if (!read_file(path, &source)) {
    printf("{\"ok\":false,\"error\":\"program-read-failed\",\"path\":");
    json_str(stdout, path ? path : "");
    printf("}\n");
    generated_case_list_free(&cases);
    proc_result_free(&gen);
    free(shape_dir); free(default_out);
    return 1;
  }
  fwrite(source.data, 1, source.len, stdout);
  if (source.len == 0 || source.data[source.len - 1] != '\n') fputc('\n', stdout);
  free(source.data);
  generated_case_list_free(&cases);
  proc_result_free(&gen);
  free(shape_dir); free(default_out);
  return 0;
}

static char *json_string_or_empty_range_local(const char *start, const char *end, const char *key) {
  char *value = json_extract_string_range(start, end, key);
  return value ? value : strdup("");
}

static int json_int_or_zero_range(const char *start, const char *end, const char *key) {
  const char *p = json_value_after_key_range(start, end, key);
  if (!p || p >= end) return 0;
  char *num_end = NULL;
  long value = strtol(p, &num_end, 10);
  if (num_end == p) return 0;
  return (int)value;
}

static bool json_bool_or_false_range(const char *start, const char *end, const char *key) {
  const char *p = json_value_after_key_range(start, end, key);
  if (!p || p >= end) return false;
  return strncmp(p, "true", 4) == 0;
}

static void captured_failure_free(captured_failure_t *f) {
  if (!f) return;
  free(f->tool);
  free(f->phase);
  free(f->flavor);
  free(f->failure_kind);
  free(f->stderr_tail);
  free(f->stdout_tail);
  free(f->stderr_text);
  free(f->stdout_text);
  memset(f, 0, sizeof(*f));
}

static bool row_find_ny_compiler_crash(const char *row, captured_failure_t *out) {
  memset(out, 0, sizeof(*out));
  const char *failures = json_top_level_value_after_key(row, "failures");
  if (!failures || *failures != '[') failures = json_value_after_key(row, "failures");
  if (!failures || *failures != '[') return false;
  const char *end = matching_json_end(failures, '[', ']');
  if (!end) return false;
  const char *p = failures + 1;
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
    char *kind = json_string_or_empty_range_local(p, obj_end + 1, "failure_kind");
    char *tool = json_string_or_empty_range_local(p, obj_end + 1, "tool");
    char *phase = json_string_or_empty_range_local(p, obj_end + 1, "phase");
    bool match = strcmp(kind ? kind : "", "compiler_crash") == 0 &&
                 strcmp(tool ? tool : "", "ny") == 0 &&
                 strcmp(phase ? phase : "", "compile") == 0;
    if (match) {
      out->found = true;
      out->failure_kind = kind;
      out->tool = tool;
      out->phase = phase;
      out->flavor = json_string_or_empty_range_local(p, obj_end + 1, "flavor");
      out->stderr_tail = json_string_or_empty_range_local(p, obj_end + 1, "stderr_tail");
      out->stdout_tail = json_string_or_empty_range_local(p, obj_end + 1, "stdout_tail");
      out->stderr_text = json_string_or_empty_range_local(p, obj_end + 1, "stderr");
      out->stdout_text = json_string_or_empty_range_local(p, obj_end + 1, "stdout");
      out->rc = json_int_or_zero_range(p, obj_end + 1, "rc");
      out->timed_out = json_bool_or_false_range(p, obj_end + 1, "timed_out");
      return true;
    }
    free(kind);
    free(tool);
    free(phase);
    p = obj_end + 1;
  }
  return false;
}

static char *synth_capture_expect_substring(const captured_failure_t *f) {
  const char *texts[] = {
    f ? f->stderr_text : NULL, f ? f->stderr_tail : NULL,
    f ? f->stdout_text : NULL, f ? f->stdout_tail : NULL
  };
  for (int i = 0; i < 4; ++i) {
    const char *s = texts[i];
    if (!s) continue;
    if (contains_ci(s, "segmentationfault")) return strdup("SegmentationFault");
    if (contains_ci(s, "segmentation fault")) return strdup("segmentation fault");
    if (contains_ci(s, "signal 11")) return strdup("signal 11");
    if (contains_ci(s, "sigsegv")) return strdup("SIGSEGV");
    if (contains_ci(s, "internal compiler error")) return strdup("internal compiler error");
    if (contains_ci(s, "panic")) return strdup("panic");
    if (contains_ci(s, "assertion")) return strdup("assertion");
  }
  return strdup("");
}

static char *synth_ny_compile_command(const char *ny_bin, const generated_case_t *gc,
                                      const captured_failure_t *failure,
                                      const char *build_root) {
  const char *flavor = failure && failure->flavor && *failure->flavor ? failure->flavor : "o3";
  const char *opt = strcmp(flavor, "o0") == 0 ? "-O0" : "-O3";
  bool insane = strcmp(flavor, "o3i") == 0;
  char *elf = NULL;
  (void)asprintf(&elf, "%s/%s/%s_ny_%s.elf",
                 build_root ? build_root : "build/native_compare",
                 gc && gc->name ? gc->name : "case",
                 gc && gc->name ? gc->name : "case",
                 flavor);
  str_buf_t b = {0};
  (void)sb_append(&b, ny_bin ? ny_bin : "ny");
  (void)sb_append(&b, " --compiler-asserts ");
  (void)sb_append(&b, opt);
  if (insane) (void)sb_append(&b, " --profile=peak");
  (void)sb_append(&b, " -o ");
  (void)sb_append(&b, elf ? elf : "case.elf");
  (void)sb_append_c(&b, ' ');
  (void)sb_append(&b, gc && gc->ny_path ? gc->ny_path : "");
  free(elf);
  return sb_take(&b);
}

static bool write_synth_crash_artifact(const char *root, const char *ny_bin,
                                       const generated_case_t *gc,
                                       const captured_failure_t *failure,
                                       const char *profile, const char *generator,
                                       int seed, const char *build_root,
                                       char **artifact_out) {
  *artifact_out = NULL;
  char *dir = NULL;
  if (asprintf(&dir, "%s/build/repro/synth", root) < 0 || !dir) return false;
  if (!mkdir_p(dir)) {
    free(dir);
    return false;
  }
  char case_stem[256], flavor_stem[96];
  safe_stem(case_stem, sizeof(case_stem), gc && gc->name ? gc->name : "case");
  safe_stem(flavor_stem, sizeof(flavor_stem),
            failure && failure->flavor && *failure->flavor ? failure->flavor : "ny");
  char *path = NULL;
  if (asprintf(&path, "%s/%s_%s_compiler_crash.json", dir, case_stem, flavor_stem) < 0 || !path) {
    free(dir);
    return false;
  }
  char *expect = synth_capture_expect_substring(failure);
  char *command = synth_ny_compile_command(ny_bin, gc, failure, build_root);
  char *shape = gc && gc->json ? json_string_or_empty(gc->json, "shape") : strdup("");
  char *family = gc && gc->json ? json_string_or_empty(gc->json, "family") : strdup("");
  char *method = gc && gc->json ? json_string_or_empty(gc->json, "method") : strdup("");
  char *source_kind = gc && gc->json ? json_string_or_empty(gc->json, "source_kind") : strdup("");
  char *features = gc && gc->json ? json_array_or_empty(gc->json, "features") : strdup("[]");
  str_buf_t a = {0};
  (void)sb_append(&a, "{\"schema\":\"nytrix.synth.repro.v1\",\"captured\":true,"
                     "\"failure_kind\":\"compiler_crash\",\"mode\":\"compile_crash\","
                     "\"reducer_mode\":\"compile_crash\",\"tool\":");
  (void)sb_append_json_str(&a, failure && failure->tool ? failure->tool : "ny");
  (void)sb_append(&a, ",\"phase\":");
  (void)sb_append_json_str(&a, failure && failure->phase ? failure->phase : "compile");
  (void)sb_append(&a, ",\"flavor\":");
  (void)sb_append_json_str(&a, failure && failure->flavor ? failure->flavor : "");
  (void)sb_appendf(&a, ",\"rc\":%d,\"timed_out\":%s,\"case\":",
                   failure ? failure->rc : 1,
                   failure && failure->timed_out ? "true" : "false");
  (void)sb_append_json_str(&a, gc && gc->name ? gc->name : "");
  (void)sb_append(&a, ",\"seed\":");
  (void)sb_appendf(&a, "%d", seed);
  (void)sb_append(&a, ",\"profile\":");
  (void)sb_append_json_str(&a, profile ? profile : "");
  (void)sb_append(&a, ",\"generator\":");
  (void)sb_append_json_str(&a, generator ? generator : "");
  (void)sb_append(&a, ",\"shape\":");
  (void)sb_append_json_str(&a, shape ? shape : "");
  (void)sb_append(&a, ",\"family\":");
  (void)sb_append_json_str(&a, family ? family : "");
  (void)sb_append(&a, ",\"method\":");
  (void)sb_append_json_str(&a, method ? method : "");
  (void)sb_append(&a, ",\"source_kind\":");
  (void)sb_append_json_str(&a, source_kind ? source_kind : "");
  (void)sb_append(&a, ",\"features\":");
  (void)sb_append(&a, features ? features : "[]");
  (void)sb_append(&a, ",\"ny_source\":");
  append_rel_json_str(&a, root, gc && gc->ny_path ? gc->ny_path : "");
  (void)sb_append(&a, ",\"c_source\":");
  append_rel_json_str(&a, root, gc && gc->c_path ? gc->c_path : "");
  (void)sb_append(&a, ",\"nytrix_ir\":");
  append_rel_json_str(&a, root, gc && gc->ir_path ? gc->ir_path : "");
  (void)sb_append(&a, ",\"command\":");
  (void)sb_append_json_str(&a, command ? command : "");
  (void)sb_append(&a, ",\"expect_substring\":");
  (void)sb_append_json_str(&a, expect ? expect : "");
  (void)sb_append(&a, ",\"stderr_tail\":");
  (void)sb_append_json_str(&a, failure && failure->stderr_tail ? failure->stderr_tail : "");
  (void)sb_append(&a, ",\"stdout_tail\":");
  (void)sb_append_json_str(&a, failure && failure->stdout_tail ? failure->stdout_tail : "");
  if (gc && gc->json) {
    (void)sb_append(&a, ",\"generator_case\":");
    (void)sb_append(&a, gc->json);
  }
  (void)sb_append(&a, "}\n");
  bool ok = write_file_text(path, a.data ? a.data : "{}\n");
  if (ok) *artifact_out = path;
  else free(path);
  free(dir);
  free(expect);
  free(command);
  free(shape);
  free(family);
  free(method);
  free(source_kind);
  free(features);
  free(a.data);
  return ok;
}

static bool reduce_synth_crash_artifact(const char *root, const char *ny_bin,
                                        const char *artifact_path, double timeout_s,
                                        int max_checks, char **reduced_out,
                                        int *checks_out) {
  *reduced_out = NULL;
  if (checks_out) *checks_out = 0;
  char *json_path = NULL;
  if (artifact_path) {
    if (asprintf(&json_path, "%s.reduce.json", artifact_path) < 0 || !json_path)
      return false;
  } else {
    json_path = nytrix_scratch_pathf(NULL, "reduced/nytrix_reduce_%ld.reduce.json",
                                    (long)getpid());
    if (!json_path) return false;
    (void)mkdir_parent(json_path);
  }
  char timeout_buf[64], checks_buf[32];
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s > 0.0 ? timeout_s : 0.0);
  snprintf(checks_buf, sizeof(checks_buf), "%d", max_checks > 0 ? max_checks : 80);
  char *argv[] = {
    g_self_path, "reduce", "artifact",
    "--artifact", (char *)artifact_path,
    "--ny-bin", (char *)ny_bin,
    "--timeout-s", timeout_buf,
    "--max-checks", checks_buf,
    "--json", json_path,
    NULL
  };
  double outer = timeout_s > 0.0 ? timeout_s * 4.0 + 30.0 : 0.0;
  proc_result_t pr = run_proc(argv, root, outer);
  file_buf_t report = {0};
  bool ok = pr.rc == 0 && read_file(json_path, &report) && !json_failures_nonempty(report.data);
  char *reduced = ok ? json_string_or_empty(report.data, "reduced_source") : NULL;
  double checks = 0.0;
  if (report.data) (void)extract_json_number(report.data, "checks", &checks);
  char *resolved = reduced && *reduced ? resolve_existing_file(root, reduced) : NULL;
  if (ok && resolved) {
    *reduced_out = resolved;
    if (checks_out) *checks_out = (int)checks;
  } else {
    ok = false;
    free(resolved);
  }
  free(reduced);
  free(report.data);
  proc_result_free(&pr);
  free(json_path);
  return ok;
}

static char *row_with_capture_fields(const char *row, const char *root,
                                     const char *artifact_path,
                                     const char *reduced_source,
                                     int reduction_checks) {
  size_t n = strlen(row ? row : "");
  while (n > 0 && isspace((unsigned char)row[n - 1])) --n;
  if (n == 0 || row[n - 1] != '}') return strdup(row ? row : "");
  str_buf_t b = {0};
  (void)sb_append_n(&b, row, n - 1);
  (void)sb_append(&b, ",\"captured\":true,\"artifact\":");
  append_rel_json_str(&b, root, artifact_path ? artifact_path : "");
  (void)sb_append(&b, ",\"reduced_source\":");
  append_rel_json_str(&b, root, reduced_source ? reduced_source : "");
  (void)sb_appendf(&b, ",\"reduction_checks\":%d}", reduction_checks);
  return sb_take(&b);
}

static bool synth_row_matches_known_bug_ny008(const char *root, const char *row) {
  if (!row || !strstr(row, "\"failure_kind\":\"output_diff\"")) return false;
  if (!strstr(row, "\"flavor\":\"o3i\"")) return false;
  char *ny_source = json_string_or_empty(row, "ny_source");
  char *resolved = resolve_existing_file(root, ny_source);
  file_buf_t source = {0};
  bool ok = resolved && read_file(resolved, &source) && source.data;
  bool matches = false;
  if (ok) {
    matches =
      strstr(source.data, "def int: row =") &&
      strstr(source.data, "acc -=") &&
      strstr(source.data, "print(acc)") &&
      (strstr(source.data, "row *") || strstr(source.data, "-= row"));
  }
  free(source.data);
  free(resolved);
  free(ny_source);
  return matches;
}

static char *row_with_known_bug_fields(const char *row, const char *bug_id,
                                       const char *reason) {
  size_t n = strlen(row ? row : "");
  while (n > 0 && isspace((unsigned char)row[n - 1])) --n;
  if (n == 0 || row[n - 1] != '}') return strdup(row ? row : "");
  str_buf_t b = {0};
  (void)sb_append_n(&b, row, n - 1);
  (void)sb_append(&b, ",\"known_bug\":true,\"known_bug_id\":");
  (void)sb_append_json_str(&b, bug_id ? bug_id : "");
  (void)sb_append(&b, ",\"quarantined\":true,\"quarantine_reason\":");
  (void)sb_append_json_str(&b, reason ? reason : "known upstream bug");
  (void)sb_append_c(&b, '}');
  return sb_take(&b);
}

static int cmd_public_synth_generate(int argc, char **argv) {
  char root[4096], ny_bin[4096];
  if (!find_nytrix_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nytrix-root-not-found\"}\n");
    return 2;
  }
  if (!find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-binary-not-found\",\"reason\":\"run ./make ny first or set NYTRIX_NY_BIN\"}\n");
    return 2;
  }
  bool fast = has_flag_after(argc, argv, 3, "--fast");
  bool insane = has_flag_after(argc, argv, 3, "--insane");
  bool capture_failures = fast || has_flag_after(argc, argv, 3, "--capture-failures");
  bool strict_failures = has_flag_after(argc, argv, 3, "--strict-failures");
  bool quarantine_known_bugs = has_flag_after(argc, argv, 3, "--quarantine-known-bugs") ||
                               has_flag_after(argc, argv, 3, "--known-bugs-ok");
  int cases = atoi(value_after(argc, argv, 3, "--cases", "8"));
  if (cases < 1) cases = 1;
  int seed = atoi(value_after(argc, argv, 3, "--seed", "1337"));
  int runs = atoi(value_after(argc, argv, 3, "--runs", "1"));
  if (runs < 1) runs = 1;
  int warmup = atoi(value_after(argc, argv, 3, "--warmup", "0"));
  if (warmup < 0) warmup = 0;
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "90"));
  const char *profile = value_after(argc, argv, 3, "--profile", "balanced");
  const char *generator = value_after(argc, argv, 3, "--generator", "mixed");
  const char *schedule = canonical_synth_schedule(value_after(argc, argv, 3, "--schedule", "smart"));
  const char *shape = value_after(argc, argv, 3, "--shape", "");
  const char *method = canonical_native_method(generator);
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  int max_reduce_checks = atoi(value_after(argc, argv, 3, "--max-reduce-checks",
                                           fast ? "80" : "250"));
  if (max_reduce_checks < 1) max_reduce_checks = fast ? 80 : 250;
  char *default_out = NULL, *shape_dir = NULL, *build_dir = NULL;
  bool paths_ok = nytrix_asprintf(&default_out, "%s",
                           default_generated_leaf_for_method(method)) >= 0 &&
                  nytrix_asprintf(&shape_dir, "etc/tests/fuzz/shapes") >= 0 &&
                  nytrix_asprintf(&build_dir, "build/%s/native/%s_%d", method, profile, seed) >= 0;
  if (!paths_ok) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(default_out); free(shape_dir); free(build_dir);
    return 2;
  }
  const char *out_dir = value_after(argc, argv, 3, "--out", default_out);
  const char *build_root = value_after(argc, argv, 3, "--build-dir", build_dir);
  char cases_buf[32], seed_buf[32], runs_buf[32], warmup_buf[32], timeout_buf[64];
  snprintf(cases_buf, sizeof(cases_buf), "%d", cases);
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
  snprintf(runs_buf, sizeof(runs_buf), "%d", runs);
  snprintf(warmup_buf, sizeof(warmup_buf), "%d", warmup);
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  char *gen_argv[32];
  int ga = 0;
  gen_argv[ga++] = g_self_path;
  gen_argv[ga++] = "generate-batch";
  gen_argv[ga++] = "--shape-dir"; gen_argv[ga++] = shape_dir;
  gen_argv[ga++] = "--profile"; gen_argv[ga++] = (char *)profile;
  gen_argv[ga++] = "--seed"; gen_argv[ga++] = seed_buf;
  gen_argv[ga++] = "--cases"; gen_argv[ga++] = cases_buf;
  gen_argv[ga++] = "--out"; gen_argv[ga++] = (char *)out_dir;
  gen_argv[ga++] = "--generator"; gen_argv[ga++] = (char *)generator;
  gen_argv[ga++] = "--schedule"; gen_argv[ga++] = (char *)schedule;
  if (shape && *shape) { gen_argv[ga++] = "--shape"; gen_argv[ga++] = (char *)shape; }
  if (fast) gen_argv[ga++] = "--fast";
  if (insane) gen_argv[ga++] = "--insane";
  gen_argv[ga] = NULL;
  proc_result_t gen = run_proc(gen_argv, root, timeout_s > 0.0 ? timeout_s : 0.0);
  double selected_shape_count_d = 0.0, total_shape_count_d = 0.0;
  int selected_shape_count = 0, total_shape_count = 0;
  if (gen.out) {
    if (extract_json_number(gen.out, "selected_shape_count", &selected_shape_count_d))
      selected_shape_count = (int)selected_shape_count_d;
    if (extract_json_number(gen.out, "shape_count", &total_shape_count_d))
      total_shape_count = (int)total_shape_count_d;
  }
  if (gen.rc != 0 || !gen.out || !strstr(gen.out, "\"cases\"")) {
    char *row = make_worker_failure_row("generate-batch", "generate-batch", gen.rc, gen.out, gen.err);
    report_rows_t report;
    memset(&report, 0, sizeof(report));
    report_add_row(&report, row);
    generated_case_list_t none;
    memset(&none, 0, sizeof(none));
    char *report_json = build_synth_generate_report_json(&report, &none, profile, generator, schedule, fast,
                                                         capture_failures, strict_failures, 0,
                                                         quarantine_known_bugs, 0, seed,
                                                         out_dir, build_root, ny_bin, runs, warmup, gen.elapsed_ms,
                                                         selected_shape_count, total_shape_count);
    if (json_path && *json_path) (void)write_file_text(json_path, report_json);
    print_synth_generate_human(&report);
    free(report_json);
    report_rows_free(&report);
    proc_result_free(&gen);
    free(default_out); free(shape_dir); free(build_dir);
    return 1;
  }
  generated_case_list_t generated;
  memset(&generated, 0, sizeof(generated));
  if (!parse_generated_cases(gen.out, &generated)) {
    printf("{\"ok\":false,\"error\":\"generated-case-parse-failed\"}\n");
    proc_result_free(&gen);
    free(default_out); free(shape_dir); free(build_dir);
    return 1;
  }
  report_rows_t report;
  memset(&report, 0, sizeof(report));
  report.worker_ms = 0.0;
  int captured_failures = 0;
  int quarantined_known_bugs = 0;
  double outer_timeout = worker_outer_timeout(timeout_s, runs, warmup);
  for (int i = 0; i < generated.count; ++i) {
    generated_case_t *gc = &generated.items[i];
    char *case_bin_dir = NULL;
    if (asprintf(&case_bin_dir, "%s/%s", build_root, gc->name) < 0) {
      char *row = make_worker_failure_row(gc->name, "compare-prepare", 1, "", "bin dir allocation failed");
      report_add_row(&report, row);
      continue;
    }
    char *cmp_argv[40];
    int ca = 0;
    cmp_argv[ca++] = g_self_path;
    cmp_argv[ca++] = "compare-case";
    cmp_argv[ca++] = "--case"; cmp_argv[ca++] = gc->name;
    cmp_argv[ca++] = "--c"; cmp_argv[ca++] = gc->c_path;
    cmp_argv[ca++] = "--ny"; cmp_argv[ca++] = gc->ny_path;
    cmp_argv[ca++] = "--ir"; cmp_argv[ca++] = gc->ir_path;
    cmp_argv[ca++] = "--root"; cmp_argv[ca++] = root;
    cmp_argv[ca++] = "--ny-bin"; cmp_argv[ca++] = ny_bin;
    cmp_argv[ca++] = "--bin-dir"; cmp_argv[ca++] = case_bin_dir;
    cmp_argv[ca++] = "--timeout-s"; cmp_argv[ca++] = timeout_buf;
    cmp_argv[ca++] = "--runs"; cmp_argv[ca++] = runs_buf;
    cmp_argv[ca++] = "--warmup"; cmp_argv[ca++] = warmup_buf;
    if (gc->features_csv && *gc->features_csv) {
      cmp_argv[ca++] = "--features";
      cmp_argv[ca++] = gc->features_csv;
    }
    cmp_argv[ca] = NULL;
    proc_result_t pr = run_proc(cmp_argv, root, outer_timeout);
    report.worker_ms += pr.elapsed_ms;
    char *row = NULL;
    if (pr.rc == 0 && pr.out && strstr(pr.out, "\"failures\"")) {
      row = trim_trailing_copy(pr.out);
    } else {
      row = make_worker_failure_row(gc->name, "compare-case", pr.rc, pr.out, pr.err);
    }
    bool row_added = false;
    if (quarantine_known_bugs && row && synth_row_matches_known_bug_ny008(root, row)) {
      char *known_row = row_with_known_bug_fields(row, "NY-008",
                                                  "known Nytrix peak def snapshot miscompile");
      free(row);
      row = known_row;
      report_add_row_unscored(&report, row);
      quarantined_known_bugs++;
      row_added = true;
    }
    if (!row_added && capture_failures && row) {
      captured_failure_t failure;
      if (row_find_ny_compiler_crash(row, &failure)) {
        char *artifact = NULL;
        char *reduced = NULL;
        int reduction_checks = 0;
        bool captured = write_synth_crash_artifact(root, ny_bin, gc, &failure,
                                                   profile, generator, seed, build_root,
                                                   &artifact) &&
                        reduce_synth_crash_artifact(root, ny_bin, artifact, timeout_s,
                                                    max_reduce_checks, &reduced,
                                                    &reduction_checks);
        if (captured) {
          char *captured_row = row_with_capture_fields(row, root, artifact, reduced, reduction_checks);
          free(row);
          row = captured_row;
          captured_failures++;
          if (strict_failures) report_add_row(&report, row);
          else report_add_row_unscored(&report, row);
          row_added = true;
        }
        free(artifact);
        free(reduced);
      }
      captured_failure_free(&failure);
    }
    if (!row_added) report_add_row(&report, row);
    proc_result_free(&pr);
    free(case_bin_dir);
  }
  char *report_json = build_synth_generate_report_json(&report, &generated, profile, generator, schedule, fast,
                                                       capture_failures, strict_failures,
                                                       captured_failures, quarantine_known_bugs,
                                                       quarantined_known_bugs, seed,
                                                       out_dir, build_root, ny_bin, runs, warmup, gen.elapsed_ms,
                                                       selected_shape_count, total_shape_count);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    print_synth_generate_human(&report);
  }
  int rc = report.failure_count ? 1 : 0;
  free(report_json);
  report_rows_free(&report);
  generated_case_list_free(&generated);
  proc_result_free(&gen);
  free(default_out); free(shape_dir); free(build_dir);
  return rc;
}

