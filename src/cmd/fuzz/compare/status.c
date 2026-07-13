static int cmd_public_fuzz_all_status(int argc, char **argv) {
  char root[4096];
  if (!find_nytrix_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nytrix-root-not-found\"}\n");
    return 2;
  }
  const char *history_arg = value_after_equals(argc, argv, 4, "--history", "build/fuzz/all/history.json");
  const char *worklist_arg = value_after_equals(argc, argv, 4, "--worklist", "build/fuzz/all/worklist.json");
  const char *coverage_arg = value_after_equals(argc, argv, 4, "--coverage", "build/fuzz/all/coverage.json");
  const char *plan_arg = value_after_equals(argc, argv, 4, "--plan", "build/fuzz/all/plan.json");
  const char *dir_arg = value_after_equals(argc, argv, 4, "--dir", "");
  if (!dir_arg || !*dir_arg)
    dir_arg = value_after_equals(argc, argv, 4, "--history-dir", "");
  const char *target_arg = value_after_equals(argc, argv, 4, "--target-thread-years", "");
  if (!target_arg || !*target_arg)
    target_arg = value_after_equals(argc, argv, 4, "--target-years", "");
  if (!target_arg || !*target_arg)
    target_arg = value_after_equals(argc, argv, 4, "--target", "10");
  const char *hours_arg = value_after_equals(argc, argv, 4, "--hours-per-run", "");
  if (!hours_arg || !*hours_arg)
    hours_arg = value_after_equals(argc, argv, 4, "--run-hours", "");
  if (!hours_arg || !*hours_arg)
    hours_arg = value_after_equals(argc, argv, 4, "--hours", "8");
  const char *threads_arg = value_after_equals(argc, argv, 4, "--threads",
                                               NYTRIX_DEFAULT_FUZZ_THREADS);
  const char *profile_arg = value_after_equals(argc, argv, 4, "--profile", "insane");
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *markdown_path = value_after_equals(argc, argv, 4, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after_equals(argc, argv, 4, "--md", "");
  const char *script_arg = value_after_equals(argc, argv, 4, "--script", "");
  if (!script_arg || !*script_arg)
    script_arg = value_after_equals(argc, argv, 4, "--next-script", "");
  const char *handoff_command_arg =
      value_after_equals(argc, argv, 4, "--handoff-command", "");
  if (!handoff_command_arg || !*handoff_command_arg)
    handoff_command_arg = value_after_equals(argc, argv, 4, "--next-command", "");
  bool strict = has_flag_after(argc, argv, 4, "--strict");
  bool allow_incomplete_coverage =
      has_flag_after(argc, argv, 4, "--allow-incomplete-coverage") ||
      has_flag_after(argc, argv, 4, "--allow-smoke-coverage-gaps");
  bool allow_empty_preflight_history =
      has_flag_after(argc, argv, 4, "--allow-empty-preflight-history");
  bool allow_full_pressure_remediation =
      has_flag_after(argc, argv, 4, "--allow-full-pressure-remediation") ||
      has_flag_after(argc, argv, 4, "--allow-stale-full-pressure");
  bool refresh = has_flag_after(argc, argv, 4, "--refresh") ||
                 has_flag_after(argc, argv, 4, "--update");
  bool write_script = !has_flag_after(argc, argv, 4, "--no-script");

  char *history_path = NULL, *worklist_path = NULL, *coverage_path = NULL;
  char *plan_path = NULL, *dir_path = NULL, *script_path = NULL;
  char *missing_evidence_script_path = NULL;
  char *status_json_script_path = NULL, *status_md_script_path = NULL;
  if (history_arg && *history_arg) {
    if (path_is_absolute(history_arg)) history_path = strdup(history_arg);
    else (void)nytrix_asprintf(&history_path, "%s", history_arg);
  }
  if (worklist_arg && *worklist_arg) {
    if (path_is_absolute(worklist_arg)) worklist_path = strdup(worklist_arg);
    else (void)nytrix_asprintf(&worklist_path, "%s", worklist_arg);
  }
  if (coverage_arg && *coverage_arg) {
    if (path_is_absolute(coverage_arg)) coverage_path = strdup(coverage_arg);
    else (void)nytrix_asprintf(&coverage_path, "%s", coverage_arg);
  }
  if (plan_arg && *plan_arg) {
    if (path_is_absolute(plan_arg)) plan_path = strdup(plan_arg);
    else (void)nytrix_asprintf(&plan_path, "%s", plan_arg);
  }
  if (dir_arg && *dir_arg) {
    if (path_is_absolute(dir_arg)) dir_path = strdup(dir_arg);
    else (void)nytrix_asprintf(&dir_path, "%s", dir_arg);
  }
  nytrix_redirect_nytrix_output_dir(&dir_path, root, "fuzz-all-status");
  if (script_arg && *script_arg) {
    if (path_is_absolute(script_arg)) script_path = strdup(script_arg);
    else (void)nytrix_asprintf(&script_path, "%s", script_arg);
  } else if (dir_path && *dir_path) {
    (void)asprintf(&script_path, "%s/run-next.sh", dir_path);
  }
  if (dir_path && *dir_path) {
    (void)asprintf(&status_json_script_path, "%s/status.json", dir_path);
  }
  if (status_json_script_path && *status_json_script_path) {
    status_md_script_path = path_with_suffix_ext(status_json_script_path, "", ".md");
  }

  char *history_md_path = history_path ? path_with_suffix_ext(history_path, "", ".md") : NULL;
  char *worklist_md_path = worklist_path ? path_with_suffix_ext(worklist_path, "", ".md") : NULL;
  char *worklist_history_path =
      worklist_path ? path_with_suffix_ext(worklist_path, "-history", ".json") : NULL;
  char *worklist_history_md_path =
      worklist_path ? path_with_suffix_ext(worklist_path, "-history", ".md") : NULL;
  char *coverage_md_path = coverage_path ? path_with_suffix_ext(coverage_path, "", ".md") : NULL;
  char *plan_md_path = plan_path ? path_with_suffix_ext(plan_path, "", ".md") : NULL;
  char *old_paths_json_path = NULL;
  char *old_paths_md_path = NULL;
  const char *old_paths_dir = dir_path && *dir_path ? dir_path : "build/fuzz/all";
  (void)asprintf(&old_paths_json_path, "%s/old-paths.json", old_paths_dir);
  (void)asprintf(&old_paths_md_path, "%s/old-paths.md", old_paths_dir);
  char *dir_cmd_path = rel_path_dup(root, dir_path ? dir_path : "build/fuzz/all");
  char *history_cmd_path = rel_path_dup(root, history_path ? history_path : "build/fuzz/all/history.json");
  char *history_md_cmd_path = rel_path_dup(root, history_md_path ? history_md_path : "build/fuzz/all/history.md");
  char *worklist_cmd_path = rel_path_dup(root, worklist_path ? worklist_path : "build/fuzz/all/worklist.json");
  char *worklist_md_cmd_path = rel_path_dup(root, worklist_md_path ? worklist_md_path : "build/fuzz/all/worklist.md");
  char *worklist_history_cmd_path = rel_path_dup(root, worklist_history_path ?
                                                        worklist_history_path :
                                                        "build/fuzz/all/worklist-history.json");
  char *worklist_history_md_cmd_path = rel_path_dup(root, worklist_history_md_path ?
                                                           worklist_history_md_path :
                                                           "build/fuzz/all/worklist-history.md");
  char *coverage_cmd_path = rel_path_dup(root, coverage_path ? coverage_path : "build/fuzz/all/coverage.json");
  char *coverage_md_cmd_path = rel_path_dup(root, coverage_md_path ? coverage_md_path : "build/fuzz/all/coverage.md");
  char *plan_cmd_path = rel_path_dup(root, plan_path ? plan_path : "build/fuzz/all/plan.json");
  char *plan_md_cmd_path = rel_path_dup(root, plan_md_path ? plan_md_path : "build/fuzz/all/plan.md");
  char *status_json_cmd_path =
      rel_path_dup(root, json_path && *json_path ? json_path :
                   (status_json_script_path && *status_json_script_path ?
                    status_json_script_path : "build/fuzz/all/status.json"));
  char *status_md_cmd_path =
      rel_path_dup(root, markdown_path && *markdown_path ? markdown_path :
                   (status_md_script_path && *status_md_script_path ?
                    status_md_script_path : "build/fuzz/all/status.md"));
  const char *dir_cmd = dir_cmd_path && *dir_cmd_path ? dir_cmd_path : "build/fuzz/all";
  const char *history_cmd = history_cmd_path && *history_cmd_path ? history_cmd_path : "build/fuzz/all/history.json";
  const char *history_md_cmd = history_md_cmd_path && *history_md_cmd_path ? history_md_cmd_path : "build/fuzz/all/history.md";
  const char *worklist_cmd = worklist_cmd_path && *worklist_cmd_path ? worklist_cmd_path : "build/fuzz/all/worklist.json";
  const char *worklist_md_cmd = worklist_md_cmd_path && *worklist_md_cmd_path ? worklist_md_cmd_path : "build/fuzz/all/worklist.md";
  const char *worklist_history_cmd =
      worklist_history_cmd_path && *worklist_history_cmd_path ?
          worklist_history_cmd_path : "build/fuzz/all/worklist-history.json";
  const char *worklist_history_md_cmd =
      worklist_history_md_cmd_path && *worklist_history_md_cmd_path ?
          worklist_history_md_cmd_path : "build/fuzz/all/worklist-history.md";
  const char *coverage_cmd = coverage_cmd_path && *coverage_cmd_path ? coverage_cmd_path : "build/fuzz/all/coverage.json";
  const char *coverage_md_cmd = coverage_md_cmd_path && *coverage_md_cmd_path ? coverage_md_cmd_path : "build/fuzz/all/coverage.md";
  const char *plan_cmd = plan_cmd_path && *plan_cmd_path ? plan_cmd_path : "build/fuzz/all/plan.json";
  const char *plan_md_cmd = plan_md_cmd_path && *plan_md_cmd_path ? plan_md_cmd_path : "build/fuzz/all/plan.md";
  const char *status_json_cmd = status_json_cmd_path && *status_json_cmd_path ? status_json_cmd_path : "build/fuzz/all/status.json";
  const char *status_md_cmd = status_md_cmd_path && *status_md_cmd_path ? status_md_cmd_path : "build/fuzz/all/status.md";
  const char *target_cmd = target_arg && *target_arg ? target_arg : "10";
  const char *hours_cmd = hours_arg && *hours_arg ? hours_arg : "8";
  const char *threads_cmd = threads_arg && *threads_arg ? threads_arg : NYTRIX_DEFAULT_FUZZ_THREADS;
  const char *profile_cmd = profile_arg && *profile_arg ? profile_arg : "insane";
  bool strict_coverage_refresh = strict && !allow_incomplete_coverage;
  char *status_progress_json_cmd =
      fuzz_all_status_progress_output_path(dir_cmd, status_json_cmd, false);
  char *status_progress_md_cmd =
      fuzz_all_status_progress_output_path(dir_cmd, status_json_cmd, true);
  char *history_command = NULL, *worklist_command = NULL, *coverage_command = NULL;
  char *plan_command = NULL, *progress_command = NULL, *status_command = NULL;
  char *progress_command_raw = NULL, *status_command_raw = NULL;
  char *old_path_command = NULL, *old_path_command_raw = NULL;
  char *old_path_apply_command = NULL, *old_path_apply_command_raw = NULL;
  char *advisory_action_command = NULL;
  char *perf_watchlist_command = NULL;
  char *perf_watchlist_report_path = NULL;
  char *perf_watchlist_markdown_path = NULL;
  char *stop_file = NULL, *stop_command = NULL, *resume_command = NULL;
  char *state_file = NULL, *state_command = NULL;
  char *coverage_next_low_cpu_command = NULL;
  char *coverage_next_state_file = NULL, *coverage_next_state_command = NULL;
  char *coverage_next_state_refresh_command = NULL;
  char *coverage_next_stop_file = NULL, *coverage_next_stop_command = NULL;
  char *coverage_next_resume_command = NULL;
  char *preview_command = NULL;
  char *full_run_command = NULL, *full_run_command_raw = NULL;
  char *history_command_raw = NULL, *worklist_command_raw = NULL;
  (void)asprintf(&history_command_raw,
                 "./build/nytrix fuzz all history --dir %s --json %s --markdown %s",
                 dir_cmd, history_cmd, history_md_cmd);
  (void)asprintf(&worklist_command_raw,
                 "./build/nytrix fuzz all worklist --history %s --json %s --markdown %s",
                 history_cmd, worklist_cmd, worklist_md_cmd);
  history_command = fuzz_all_low_priority_command_dup(history_command_raw);
  worklist_command = fuzz_all_low_priority_command_dup(worklist_command_raw);
  free(history_command_raw);
  free(worklist_command_raw);
  advisory_action_command =
      fuzz_all_advisory_action_command(history_cmd, worklist_history_cmd,
                                       worklist_history_md_cmd);
  perf_watchlist_command = fuzz_all_perf_watchlist_command(dir_cmd);
  perf_watchlist_report_path =
      fuzz_all_perf_watchlist_report_path(dir_cmd, false);
  perf_watchlist_markdown_path =
      fuzz_all_perf_watchlist_report_path(dir_cmd, true);
  (void)asprintf(&coverage_command,
                 "./build/nytrix fuzz all coverage%s --history %s "
                 "--target-thread-years %s --hours %s --threads %s "
                 "--profile %s --json %s --markdown %s",
                 strict_coverage_refresh ? " --strict" : "", history_cmd, target_cmd,
                 hours_cmd, threads_cmd, profile_cmd, coverage_cmd,
                 coverage_md_cmd);
    (void)asprintf(&plan_command,
                   "./build/nytrix fuzz all plan --dir %s --history %s --worklist %s --coverage %s "
                   "--target-thread-years %s --hours %s --threads %s --profile %s --json %s --markdown %s",
                   dir_cmd, history_cmd, worklist_cmd, coverage_cmd,
                   target_cmd, hours_cmd, threads_cmd, profile_cmd,
                   plan_cmd, plan_md_cmd);
  (void)asprintf(&progress_command_raw,
                 "./build/nytrix fuzz all progress --refresh --strict --allow-full-pressure-remediation "
                 "--dir %s --status %s --history %s --worklist %s "
                 "--coverage %s --plan %s --target-thread-years %s "
                 "--hours %s --threads %s --profile %s --json %s "
                 "--markdown %s",
                 dir_cmd, status_json_cmd, history_cmd, worklist_cmd,
                 coverage_cmd, plan_cmd, target_cmd, hours_cmd, threads_cmd,
                 profile_cmd,
                 status_progress_json_cmd && *status_progress_json_cmd ?
                     status_progress_json_cmd : "build/fuzz/all/progress.json",
                 status_progress_md_cmd && *status_progress_md_cmd ?
                     status_progress_md_cmd : "build/fuzz/all/progress.md");
  progress_command =
      fuzz_all_low_priority_command_dup(progress_command_raw);
    (void)asprintf(&status_command_raw,
                 "./build/nytrix fuzz all status --refresh%s%s%s%s --dir %s "
                 "--history %s --worklist %s --coverage %s --plan %s "
                 "--target-thread-years %s --hours %s --threads %s --profile %s "
                 "--json %s --markdown %s",
                 strict ? " --strict" : "",
                 allow_incomplete_coverage ? " --allow-incomplete-coverage" : "",
                 allow_empty_preflight_history ?
                     " --allow-empty-preflight-history" : "",
                 allow_full_pressure_remediation ? " --allow-full-pressure-remediation" : "",
                 dir_cmd, history_cmd, worklist_cmd, coverage_cmd, plan_cmd,
                 target_cmd, hours_cmd, threads_cmd, profile_cmd,
                 status_json_cmd, status_md_cmd);
  status_command = fuzz_all_low_priority_command_dup(status_command_raw);
  (void)asprintf(&old_path_command_raw,
                 "./build/nytrix fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json %s/old-paths.json --markdown %s/old-paths.md",
                 dir_cmd, dir_cmd);
  old_path_command = fuzz_all_low_priority_command_dup(old_path_command_raw);
  (void)asprintf(&old_path_apply_command_raw,
                 "./build/nytrix fuzz all old-paths --apply --wait-writers-s 300 --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json %s/old-paths.json --markdown %s/old-paths.md",
                 dir_cmd, dir_cmd);
  old_path_apply_command =
      fuzz_all_low_priority_command_dup(old_path_apply_command_raw);
  stop_file = fuzz_all_stop_file_path(dir_cmd);
  stop_command = fuzz_all_stop_command(stop_file);
  resume_command = fuzz_all_resume_command(stop_file);
  state_file = fuzz_all_state_file_path(dir_cmd);
  state_command = fuzz_all_state_command(state_file);
  (void)asprintf(&full_run_command_raw,
                 "./build/nytrix fuzz all run --profile %s --hours %s --threads %s --target-thread-years %s "
                 "--dir %s --fail-fast --json %s/insane-%sh.json",
                 profile_cmd, hours_cmd, threads_cmd, target_cmd, dir_cmd, dir_cmd, hours_cmd);
  full_run_command = fuzz_all_low_priority_command_dup(full_run_command_raw);

  fuzz_all_status_summary_t status;
  memset(&status, 0, sizeof(status));
  status.strict = strict;
  status.allow_incomplete_coverage = allow_incomplete_coverage;
  status.allow_full_pressure_remediation = allow_full_pressure_remediation;
  status.refreshed = refresh;
  status.campaign_plan_wall_hours =
      atof(hours_arg && *hours_arg ? hours_arg : "8");
  snprintf(status.campaign_plan_threads,
           sizeof(status.campaign_plan_threads), "%s",
           threads_arg && *threads_arg ? threads_arg :
               NYTRIX_DEFAULT_FUZZ_THREADS);
  snprintf(status.status_command, sizeof(status.status_command), "%s",
           status_command ? status_command : "");
  snprintf(status.stop_file, sizeof(status.stop_file), "%s",
           stop_file ? stop_file : "");
  snprintf(status.stop_command, sizeof(status.stop_command), "%s",
           stop_command ? stop_command : "");
  snprintf(status.resume_command, sizeof(status.resume_command), "%s",
           resume_command ? resume_command : "");
  snprintf(status.state_file, sizeof(status.state_file), "%s",
           state_file ? state_file : "");
  snprintf(status.state_command, sizeof(status.state_command), "%s",
           state_command ? state_command : "");
  fuzz_all_run_state_summary_t run_state;
  fuzz_all_load_run_state(root, status.state_file, &run_state);
  fuzz_all_status_set_run_state(&status, &run_state);
  snprintf(status.old_path_command, sizeof(status.old_path_command), "%s",
           old_path_command && *old_path_command ?
               old_path_command : NYTRIX_OLD_PATH_DRY_RUN_COMMAND);
  snprintf(status.old_path_dry_run_command,
           sizeof(status.old_path_dry_run_command), "%s",
           status.old_path_command);
  snprintf(status.old_path_apply_command,
           sizeof(status.old_path_apply_command), "%s",
           old_path_apply_command && *old_path_apply_command ?
               old_path_apply_command : NYTRIX_OLD_PATH_APPLY_COMMAND);
  snprintf(status.advisory_action_command,
           sizeof(status.advisory_action_command), "%s",
           advisory_action_command ? advisory_action_command : "");
  snprintf(status.perf_watchlist_command,
           sizeof(status.perf_watchlist_command), "%s",
           perf_watchlist_command ? perf_watchlist_command : "");
  snprintf(status.progress_command, sizeof(status.progress_command), "%s",
           progress_command ? progress_command : "");
  status.current_perf_cases = (double)perf_real_case_count();
  status.latest_full_pressure_perf_suite_current = true;
  status_capture_provenance(&status, root);
  status_set_rel_path(status.history_report, sizeof(status.history_report), root, history_path ? history_path : history_arg);
  status_set_rel_path(status.worklist_report, sizeof(status.worklist_report), root, worklist_path ? worklist_path : worklist_arg);
  status_set_rel_path(status.historical_worklist_report,
                      sizeof(status.historical_worklist_report), root,
                      worklist_history_path ? worklist_history_path : "");
  status_set_rel_path(status.historical_worklist_markdown,
                      sizeof(status.historical_worklist_markdown), root,
                      worklist_history_md_path ? worklist_history_md_path : "");
  status_set_rel_path(status.coverage_report, sizeof(status.coverage_report), root, coverage_path ? coverage_path : coverage_arg);
  status_set_rel_path(status.plan_report, sizeof(status.plan_report), root, plan_path ? plan_path : plan_arg);
  status_set_rel_path(status.next_script, sizeof(status.next_script), root, script_path ? script_path : "");
  status_set_rel_path(status.perf_watchlist_report,
                      sizeof(status.perf_watchlist_report), root,
                      perf_watchlist_report_path ? perf_watchlist_report_path : "");
  status_set_rel_path(status.perf_watchlist_markdown,
                      sizeof(status.perf_watchlist_markdown), root,
                      perf_watchlist_markdown_path ?
                          perf_watchlist_markdown_path : "");
    status.perf_watchlist_artifact_readable =
        fuzz_all_load_perf_watchlist_artifact(
            root, status.perf_watchlist_report,
            &status.perf_watchlist_artifact_hotspots,
            &status.perf_watchlist_artifact_max_ratio,
            status.perf_watchlist_artifact_max_case,
            sizeof(status.perf_watchlist_artifact_max_case),
          status.perf_watchlist_artifact_max_artifact,
          sizeof(status.perf_watchlist_artifact_max_artifact),
          status.perf_watchlist_artifact_max_ny_source,
          sizeof(status.perf_watchlist_artifact_max_ny_source),
          status.perf_watchlist_artifact_max_c_source,
          sizeof(status.perf_watchlist_artifact_max_c_source));
  status.perf_watchlist_artifact_age_seconds =
      fuzz_all_score_report_age_seconds(root, status.perf_watchlist_report);
  status.perf_watchlist_artifact_stale_after_hours =
      fuzz_all_perf_watchlist_artifact_fresh_hours();
  status.perf_watchlist_artifact_fresh =
      status.perf_watchlist_artifact_readable &&
      fuzz_all_score_age_fresh(status.perf_watchlist_artifact_age_seconds,
                               status.perf_watchlist_artifact_stale_after_hours);
  fuzz_all_load_compiler_std_audit_summary(root, &status);
  char handoff_command[4096] = {0};
  fuzz_all_normalize_handoff_command(handoff_command, sizeof(handoff_command),
                                     handoff_command_arg);

  file_buf_t history = {0}, worklist = {0}, worklist_history = {0};
  file_buf_t coverage = {0}, plan = {0};
  string_list_t rows = {0}, failures = {0}, coverage_rows = {0}, coverage_seen = {0};
  char *coverage_queue_json = strdup("[]");

  if (refresh) {
    int refresh_rc = 0;
    if (history_path && history_md_path) {
      char *history_argv[] = {
        g_self_path, "fuzz", "all", "history",
        "--dir", dir_path ? dir_path : "build/fuzz/all",
        "--json", history_path, "--markdown", history_md_path, NULL
      };
      refresh_rc = cmd_public_fuzz_all_history(10, history_argv);
      if (refresh_rc != 0 && !allow_empty_preflight_history)
        (void)string_list_push_take(&failures,
                                    make_fuzz_failure(root, "fuzz-all-status",
                                                      "refresh history failed",
                                                      history_path));
    }
    if (coverage_path && coverage_md_path) {
      char *coverage_argv[20];
      int ca = 0;
      coverage_argv[ca++] = g_self_path;
      coverage_argv[ca++] = "fuzz";
      coverage_argv[ca++] = "all";
      coverage_argv[ca++] = "coverage";
      if (strict_coverage_refresh) coverage_argv[ca++] = "--strict";
      coverage_argv[ca++] = "--history";
      coverage_argv[ca++] = history_path ? history_path :
          "build/fuzz/all/history.json";
      coverage_argv[ca++] = "--target-thread-years";
      coverage_argv[ca++] = (char *)target_arg;
      coverage_argv[ca++] = "--hours";
      coverage_argv[ca++] = (char *)hours_arg;
      coverage_argv[ca++] = "--threads";
      coverage_argv[ca++] = (char *)threads_arg;
      coverage_argv[ca++] = "--profile";
      coverage_argv[ca++] = (char *)profile_arg;
      coverage_argv[ca++] = "--json";
      coverage_argv[ca++] = coverage_path;
      coverage_argv[ca++] = "--markdown";
      coverage_argv[ca++] = coverage_md_path;
      coverage_argv[ca] = NULL;
      refresh_rc = cmd_public_fuzz_all_coverage(ca, coverage_argv);
      if (refresh_rc != 0 && !allow_empty_preflight_history)
        (void)string_list_push_take(&failures,
                                    make_fuzz_failure(root, "fuzz-all-status",
                                                      "refresh coverage failed",
                                                      coverage_path));
    }
    if (worklist_path && worklist_md_path) {
      char *worklist_argv[] = {
        g_self_path, "fuzz", "all", "worklist",
        "--history", history_path ? history_path : "build/fuzz/all/history.json",
        "--json", worklist_path, "--markdown", worklist_md_path, NULL
      };
      refresh_rc = cmd_public_fuzz_all_worklist(10, worklist_argv);
      if (refresh_rc != 0)
        (void)string_list_push_take(&failures,
                                    make_fuzz_failure(root, "fuzz-all-status",
                                                      "refresh worklist failed",
                                                      worklist_path));
    }
    if (worklist_history_path && worklist_history_md_path) {
      char *worklist_history_argv[] = {
        g_self_path, "fuzz", "all", "worklist",
        "--history", history_path ? history_path : "build/fuzz/all/history.json",
        "--include-history",
        "--json", worklist_history_path,
        "--markdown", worklist_history_md_path, NULL
      };
      refresh_rc = cmd_public_fuzz_all_worklist(11, worklist_history_argv);
      if (refresh_rc != 0)
        (void)string_list_push_take(&failures,
                                    make_fuzz_failure(root, "fuzz-all-status",
                                                      "refresh historical worklist failed",
                                                      worklist_history_path));
    }
      if (plan_path && plan_md_path) {
        char *plan_argv[] = {
          g_self_path, "fuzz", "all", "plan",
        "--dir", dir_path ? dir_path : "build/fuzz/all",
        "--history", history_path ? history_path : "build/fuzz/all/history.json",
        "--worklist", worklist_path ? worklist_path : "build/fuzz/all/worklist.json",
        "--coverage", coverage_path ? coverage_path : "build/fuzz/all/coverage.json",
        "--target-thread-years", (char *)target_arg,
        "--hours", (char *)hours_arg,
        "--threads", (char *)threads_arg,
        "--profile", (char *)profile_arg,
        "--json", plan_path, "--markdown", plan_md_path, NULL
      };
      refresh_rc = cmd_public_fuzz_all_plan(24, plan_argv);
      if (refresh_rc != 0)
        (void)string_list_push_take(&failures,
                                      make_fuzz_failure(root, "fuzz-all-status",
                                                        "refresh plan failed",
                                                        plan_path));
      }
      if (old_paths_json_path && *old_paths_json_path &&
          old_paths_md_path && *old_paths_md_path) {
        char *old_paths_argv[] = {
          g_self_path, "fuzz", "all", "old-paths", "--dry-run",
          "--nytrix-root", "../nytrix", "--archive-dir",
          "build/cache/old-nytrix", "--json", old_paths_json_path,
          "--markdown", old_paths_md_path, NULL
        };
        refresh_rc = cmd_public_fuzz_all_old_paths(13, old_paths_argv);
        if (refresh_rc != 0)
          (void)string_list_push_take(&failures,
                                      make_fuzz_failure(root, "fuzz-all-status",
                                                        "refresh old-paths failed",
                                                        old_paths_json_path));
      }
    }

  status_load_old_path_report(&status, root, old_paths_json_path,
                              old_paths_md_path);
  snprintf(status.old_path_next_action,
           sizeof(status.old_path_next_action), "%s",
           fuzz_all_old_path_next_action(
               status.old_path_remaining_count,
               status.active_old_nytrix_output_writer_present));
  snprintf(status.old_path_next_reason,
           sizeof(status.old_path_next_reason), "%s",
           fuzz_all_old_path_next_reason(
               status.old_path_remaining_count,
               status.active_old_nytrix_output_writer_present));

  if (history_path && read_file(history_path, &history) && history.data) {
    status.history_readable = true;
    (void)summary_number_from_report(history.data, "reports", &status.reports);
    (void)summary_number_from_report(history.data, "ignored_no_evidence_reports",
                                     &status.ignored_no_evidence_reports);
    (void)summary_number_from_report(history.data, "ok_reports", &status.ok_reports);
    (void)summary_number_from_report(history.data, "failed_reports", &status.failed_reports);
    (void)summary_number_from_report(history.data, "attention_reports", &status.attention_reports);
    (void)summary_number_from_report(history.data, "full_pressure_reports",
                                     &status.full_pressure_reports);
    (void)summary_number_from_report(history.data, "full_pressure_ok_reports",
                                     &status.full_pressure_ok_reports);
    (void)summary_number_from_report(history.data,
                                     "full_pressure_attention_reports",
                                     &status.full_pressure_attention_reports);
       (void)summary_number_from_report(history.data, "thread_hours", &status.thread_hours);
       (void)summary_number_from_report(history.data, "thread_years", &status.thread_years);
    (void)summary_number_from_report(history.data, "campaign_first_report_epoch",
                                     &status.campaign_first_report_epoch);
    (void)summary_number_from_report(history.data, "campaign_latest_report_epoch",
                                     &status.campaign_latest_report_epoch);
    (void)summary_number_from_report(history.data, "campaign_calendar_span_days",
                                     &status.campaign_calendar_span_days);
    (void)summary_number_from_report(history.data, "campaign_calendar_age_days",
                                     &status.campaign_calendar_age_days);
    char *first = summary_string_from_report(history.data,
                                             "campaign_first_report");
    snprintf(status.campaign_first_report,
             sizeof(status.campaign_first_report), "%s", first ? first : "");
    free(first);
       (void)summary_number_from_report(history.data, "full_pressure_thread_hours",
                                        &status.full_pressure_thread_hours);
    (void)summary_number_from_report(history.data, "full_pressure_thread_years",
                                     &status.full_pressure_thread_years);
    (void)summary_number_from_report(history.data, "checked_subcases", &status.checked_subcases);
    (void)summary_number_from_report(history.data, "sub_failures_total", &status.sub_failures_total);
    (void)summary_number_from_report(history.data, "finding_live_total",
                                     &status.historical_finding_live);
    (void)summary_number_from_report(history.data, "finding_missing_total",
                                     &status.historical_finding_missing);
    (void)summary_number_from_report(history.data, "known_bug_reproduced_total",
                                     &status.historical_known_reproduced);
    (void)summary_number_from_report(history.data, "known_bug_lost_signal_total",
                                     &status.historical_known_lost);
    (void)summary_number_from_report(history.data,
                                     "known_bug_baseline_failures_total",
                                     &status.historical_known_baseline);
    (void)summary_number_from_report(history.data, "perf_hotspots_total",
                                     &status.historical_perf_hotspots);
    (void)summary_number_from_report(history.data, "perf_max_ratio",
                                     &status.historical_perf_max_ratio);
    char *historical_perf_case =
        summary_string_from_report(history.data, "perf_max_case");
    snprintf(status.historical_perf_max_case,
             sizeof(status.historical_perf_max_case), "%s",
             historical_perf_case ? historical_perf_case : "");
    free(historical_perf_case);
    (void)summary_bool_from_report(history.data, "latest_report_ok",
                                   &status.latest_report_ok);
    (void)summary_bool_from_report(history.data, "latest_report_attention",
                                   &status.latest_report_attention);
    status.latest_report_clean =
        status.latest_report_ok && !status.latest_report_attention;
    (void)summary_bool_from_report(history.data, "latest_full_pressure_ok",
                                   &status.latest_full_pressure_ok);
    (void)summary_bool_from_report(history.data,
                                   "latest_full_pressure_attention",
                                   &status.latest_full_pressure_attention);
    status.latest_full_pressure_clean =
        status.latest_full_pressure_ok &&
        !status.latest_full_pressure_attention;
    (void)summary_number_from_report(history.data, "latest_failure_count",
                                     &status.latest_failure_count);
    (void)summary_number_from_report(history.data, "latest_sub_failures",
                                     &status.latest_sub_failures);
    (void)summary_number_from_report(history.data, "latest_finding_live",
                                     &status.latest_finding_live);
    (void)summary_number_from_report(history.data, "latest_finding_missing",
                                     &status.latest_finding_missing);
    (void)summary_number_from_report(history.data,
                                     "latest_known_bug_reproduced",
                                     &status.latest_known_reproduced);
    (void)summary_number_from_report(history.data,
                                     "latest_known_bug_lost_signal",
                                     &status.latest_known_lost);
    (void)summary_number_from_report(history.data,
                                     "latest_known_bug_baseline_failures",
                                     &status.latest_known_baseline);
    (void)summary_number_from_report(history.data, "latest_perf_hotspots",
                                     &status.latest_perf_hotspots);
    (void)summary_number_from_report(history.data, "latest_perf_max_ratio",
                                     &status.latest_perf_max_ratio);
    char *latest_perf_case =
        summary_string_from_report(history.data, "latest_perf_max_case");
    snprintf(status.latest_perf_max_case,
             sizeof(status.latest_perf_max_case), "%s",
             latest_perf_case ? latest_perf_case : "");
    snprintf(status.perf_max_case, sizeof(status.perf_max_case), "%s",
             status.latest_perf_max_case);
    free(latest_perf_case);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_failure_count",
                                     &status.latest_full_pressure_failure_count);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_sub_failures",
                                     &status.latest_full_pressure_sub_failures);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_finding_live",
                                     &status.latest_full_pressure_finding_live);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_finding_missing",
                                     &status.latest_full_pressure_finding_missing);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_known_bug_reproduced",
                                     &status.latest_full_pressure_known_reproduced);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_known_bug_lost_signal",
                                     &status.latest_full_pressure_known_lost);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_known_bug_baseline_failures",
                                     &status.latest_full_pressure_known_baseline);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_perf_hotspots",
                                     &status.latest_full_pressure_perf_hotspots);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_perf_max_ratio",
                                     &status.latest_full_pressure_perf_max_ratio);
    char *latest_full_pressure_perf_case =
        summary_string_from_report(history.data,
                                   "latest_full_pressure_perf_max_case");
    snprintf(status.latest_full_pressure_perf_max_case,
             sizeof(status.latest_full_pressure_perf_max_case), "%s",
             latest_full_pressure_perf_case ?
                 latest_full_pressure_perf_case : "");
    free(latest_full_pressure_perf_case);
            status.perf_hotspots = status.latest_perf_hotspots;
            status.perf_max_ratio = status.latest_perf_max_ratio;
    char *latest = summary_string_from_report(history.data, "latest_report");
    snprintf(status.latest_report, sizeof(status.latest_report), "%s", latest ? latest : "");
    free(latest);
    char *latest_full = summary_string_from_report(history.data,
                                                   "latest_full_pressure_report");
    snprintf(status.latest_full_pressure_report,
             sizeof(status.latest_full_pressure_report), "%s",
             latest_full ? latest_full : "");
    free(latest_full);
  } else {
    status_add_blocker("missing-history", "critical", "campaign history report is not readable",
                       1.0, history_command ? history_command : "",
                       &status, &rows);
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-status",
                                                  "history report not readable",
                                                  history_path ? history_path : ""));
  }

  if (worklist_path && read_file(worklist_path, &worklist) && worklist.data) {
    status.worklist_readable = true;
    (void)summary_number_from_report(worklist.data, "active_items", &status.active_items);
    (void)summary_number_from_report(worklist.data, "active_failure_detail_count",
                                     &status.active_failure_detail_count);
    (void)summary_number_from_report(worklist.data, "active_saved_hangs",
                                     &status.active_saved_hangs);
    (void)summary_number_from_report(worklist.data, "active_saved_crashes",
                                     &status.active_saved_crashes);
    (void)summary_number_from_report(worklist.data, "active_saved_inputs",
                                     &status.active_saved_inputs);
    (void)summary_number_from_report(worklist.data, "active_repro_commands",
                                     &status.active_repro_commands);
    (void)summary_number_from_report(worklist.data, "active_raw_repro_commands",
                                     &status.active_raw_repro_commands);
    (void)summary_number_from_report(worklist.data, "active_repro_ready",
                                     &status.active_repro_ready);
    (void)summary_number_from_report(worklist.data,
                                     "non_reproducing_afl_timeouts",
                                     &status.non_reproducing_afl_timeouts);
    string_list_t worklist_rows = {0};
    if (collect_rows_from_report_json(worklist.data, &worklist_rows)) {
      bool have_primary = false;
      bool have_raw = false;
      for (int i = 0; i < worklist_rows.count; ++i) {
        const char *work_row = worklist_rows.items[i];
        bool active = json_bool_range(work_row, work_row + strlen(work_row),
                                      "active", false);
        if (!active) continue;
        char *primary = json_string_or_empty(work_row, "primary_command");
        char *raw = json_string_or_empty(work_row, "first_raw_repro_command");
        if (!raw || !*raw) {
          free(raw);
          raw = json_string_or_empty(work_row, "raw_repro_command");
        }
        if (!have_primary && primary && *primary) {
          snprintf(status.active_primary_command,
                   sizeof(status.active_primary_command), "%s", primary);
          have_primary = true;
        }
        if (!have_raw && raw && *raw) {
          snprintf(status.active_raw_repro_command,
                   sizeof(status.active_raw_repro_command), "%s", raw);
          have_raw = true;
        }
        free(raw);
        free(primary);
        if (have_primary && have_raw) break;
      }
    }
    string_list_free(&worklist_rows);
    if (!summary_number_from_report(worklist.data, "historical_attention_reports",
                                    &status.historical_attention_reports))
      (void)summary_number_from_report(worklist.data, "history_attention_reports",
                                       &status.historical_attention_reports);
    if (!summary_bool_from_report(worklist.data, "active_clear", &status.active_clear))
      status.active_clear = status.active_items <= 0.0;
  } else {
    status_add_blocker("missing-worklist", "critical", "active worklist report is not readable",
                       1.0, worklist_command ? worklist_command : "",
                       &status, &rows);
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-status",
                                                  "worklist report not readable",
                                                  worklist_path ? worklist_path : ""));
  }

  if (worklist_history_path && read_file(worklist_history_path,
                                         &worklist_history) &&
      worklist_history.data) {
    (void)summary_number_from_report(worklist_history.data,
                                     "non_reproducing_afl_timeouts",
                                     &status.historical_non_reproducing_afl_timeouts);
    fuzz_all_advisory_recheck_summary_t advisory_recheck = {0};
    fuzz_all_advisory_recheck_summary_from_worklist(
        worklist_history.data, status.latest_full_pressure_report,
        &advisory_recheck);
    snprintf(status.advisory_recheck_command,
             sizeof(status.advisory_recheck_command), "%s",
             advisory_recheck.command);
    status.advisory_recheck_raw_repro_checked =
        advisory_recheck.raw_repro_checked;
    status.advisory_recheck_raw_repro_passed =
        advisory_recheck.raw_repro_passed;
    status.advisory_recheck_raw_repro_timeouts =
        advisory_recheck.raw_repro_timeouts;
    status.advisory_recheck_raw_repro_unexpected =
        advisory_recheck.raw_repro_unexpected;
  }

  if (coverage_path && read_file(coverage_path, &coverage) && coverage.data) {
    status.coverage_readable = true;
    (void)summary_number_from_report(coverage.data, "lanes", &status.coverage_lanes);
    (void)summary_number_from_report(coverage.data, "ran_lanes", &status.coverage_ran_lanes);
    (void)summary_number_from_report(coverage.data, "skipped_lanes", &status.coverage_skipped_lanes);
    (void)summary_number_from_report(coverage.data, "failed_lanes", &status.coverage_failed_lanes);
    (void)summary_number_from_report(coverage.data, "coverage_gaps", &status.coverage_gaps);
    (void)summary_number_from_report(coverage.data, "blocker_gaps", &status.coverage_blocker_gaps);
    (void)summary_number_from_report(coverage.data, "reports_considered",
                                     &status.coverage_reports_considered);
    (void)summary_number_from_report(coverage.data,
                                     "campaign_reports_considered",
                                     &status.coverage_campaign_reports_considered);
    (void)summary_number_from_report(coverage.data,
                                     "companion_reports_considered",
                                     &status.coverage_companion_reports_considered);
    if (status.coverage_campaign_reports_considered <= 0.0 &&
        status.coverage_reports_considered >=
            status.coverage_companion_reports_considered) {
      status.coverage_campaign_reports_considered =
          status.coverage_reports_considered -
          status.coverage_companion_reports_considered;
    }
    (void)summary_number_from_report(coverage.data, "disabled_lanes",
                                     &status.coverage_disabled_lanes);
    (void)summary_number_from_report(coverage.data, "budget_short_lanes",
                                     &status.coverage_budget_short_lanes);
    (void)summary_number_from_report(coverage.data, "missing_tool_lanes",
                                     &status.coverage_missing_tool_lanes);
    double coverage_detail_rows_value = 0.0;
    if (summary_number_from_report(coverage.data, "coverage_detail_rows",
                                   &coverage_detail_rows_value) &&
        coverage_detail_rows_value > 0.0)
      status.coverage_detail_rows = (int)coverage_detail_rows_value;
    double coverage_backlog_lanes_value = 0.0;
    if (summary_number_from_report(coverage.data,
                                   "coverage_backlog_lanes",
                                   &coverage_backlog_lanes_value) &&
        coverage_backlog_lanes_value > 0.0) {
      status.coverage_backlog_lanes = (int)coverage_backlog_lanes_value;
    } else if (summary_number_from_report(coverage.data,
                                          "coverage_skipped_unique_lanes",
                                          &coverage_backlog_lanes_value) &&
               coverage_backlog_lanes_value > 0.0) {
      status.coverage_backlog_lanes = (int)coverage_backlog_lanes_value;
    }
    double coverage_queue_count_value = 0.0;
    if (summary_number_from_report(coverage.data, "coverage_queue_count",
                                   &coverage_queue_count_value) &&
        coverage_queue_count_value > 0.0)
      status.coverage_queue_count = (int)coverage_queue_count_value;
    double coverage_queue_primary_value = 0.0;
    if (summary_number_from_report(coverage.data,
                                   "coverage_queue_non_advisory_count",
                                   &coverage_queue_primary_value) &&
        coverage_queue_primary_value > 0.0)
      status.coverage_queue_non_advisory_count =
          (int)coverage_queue_primary_value;
    double coverage_queue_advisory_value = 0.0;
    if (summary_number_from_report(coverage.data,
                                   "coverage_queue_advisory_count",
                                   &coverage_queue_advisory_value) &&
        coverage_queue_advisory_value > 0.0)
      status.coverage_queue_advisory_count =
          (int)coverage_queue_advisory_value;
    char *coverage_queue_lanes =
        summary_string_from_report(coverage.data, "coverage_queue_lanes");
    if (coverage_queue_lanes) {
      snprintf(status.coverage_queue_lanes,
               sizeof(status.coverage_queue_lanes), "%s",
               coverage_queue_lanes);
      free(coverage_queue_lanes);
    }
    char *coverage_queue_array =
        summary_array_from_report(coverage.data, "coverage_queue");
    if (coverage_queue_array) {
      free(coverage_queue_json);
      coverage_queue_json = coverage_queue_array;
    }
    (void)summary_number_from_report(coverage.data, "latest_report_advisory_gaps",
                                     &status.coverage_latest_report_advisory_gaps);
    (void)summary_number_from_report(coverage.data,
                                     "latest_report_companion_skipped_lanes",
                                     &status.coverage_latest_report_companion_skipped_lanes);
    status.coverage_advisory_gaps = status.coverage_gaps - status.coverage_blocker_gaps;
    if (status.coverage_advisory_gaps < 0.0)
      status.coverage_advisory_gaps = 0.0;
    if (!summary_bool_from_report(coverage.data, "full_pressure_ready",
                                  &status.full_pressure_ready))
      status.full_pressure_ready =
        status.coverage_blocker_gaps <= 0.0 && status.coverage_failed_lanes <= 0.0;
    if (collect_rows_from_report_json(coverage.data, &coverage_rows)) {
      for (int i = 0; i < coverage_rows.count; ++i) {
        char *category = json_string_or_empty(coverage_rows.items[i], "category");
        char *severity = json_string_or_empty(coverage_rows.items[i], "severity");
        char *lane = json_string_or_empty(coverage_rows.items[i], "lane");
        char *reason = json_string_or_empty(coverage_rows.items[i], "reason");
        char *command = json_string_or_empty(coverage_rows.items[i], "command");
        bool has_command = command && *command;
        if (has_command) {
          status_add_coverage_detail(category, severity, lane, reason,
                                     command, &status, &rows, &coverage_seen);
        }
        free(category);
        free(severity);
        free(lane);
        free(reason);
        free(command);
      }
      if (status.coverage_detail_count > 0)
        status.coverage_backlog_lanes = status.coverage_detail_count;
      if (status.coverage_queue_count <= 0)
        status.coverage_queue_count = status.coverage_detail_count;
      if (status.coverage_detail_rows <= 0)
        status.coverage_detail_rows = coverage_rows.count;
    }
  } else {
    status_add_blocker("missing-coverage", "critical", "coverage report is not readable",
                       1.0, coverage_command ? coverage_command : "",
                       &status, &rows);
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-status",
                                                  "coverage report not readable",
                                                  coverage_path ? coverage_path : ""));
  }

  if (plan_path && read_file(plan_path, &plan) && plan.data) {
    status.plan_readable = true;
    (void)summary_number_from_report(plan.data, "target_thread_years", &status.target_thread_years);
    (void)summary_number_from_report(plan.data, "remaining_thread_years", &status.remaining_thread_years);
    (void)summary_number_from_report(plan.data, "target_percent", &status.target_percent);
    (void)summary_number_from_report(plan.data, "runs_needed", &status.runs_needed);
    (void)summary_number_from_report(plan.data, "wall_hours_needed",
                                     &status.wall_hours_needed);
    (void)summary_number_from_report(plan.data, "thread_hours_needed",
                                     &status.thread_hours_needed);
    (void)summary_number_from_report(plan.data, "wall_days_needed", &status.wall_days_needed);
    (void)summary_number_from_report(plan.data, "runs_per_day",
                                     &status.runs_per_day);
    (void)summary_number_from_report(plan.data, "thread_years_per_day",
                                     &status.thread_years_per_day);
    (void)summary_number_from_report(plan.data, "completion_eta_epoch",
                                     &status.completion_eta_epoch);
    char *eta = summary_string_from_report(plan.data, "completion_eta_local");
    snprintf(status.completion_eta_local, sizeof(status.completion_eta_local),
             "%s", eta ? eta : "");
    free(eta);
    if (!status.coverage_readable)
      (void)summary_number_from_report(plan.data, "coverage_blocker_gaps", &status.coverage_blocker_gaps);
    if (!summary_bool_from_report(plan.data, "long_run_ready", &status.long_run_ready))
      status.long_run_ready = status.active_clear && status.full_pressure_ready;
    if (!summary_bool_from_report(plan.data, "target_reached",
                                  &status.target_reached))
      status.target_reached = status.target_thread_years > 0.0 &&
                              status.remaining_thread_years <= 0.0;
    char *run_command = summary_string_from_report(plan.data, "run_command");
    char *guarded_run_command =
        run_command && *run_command ?
            fuzz_all_low_priority_command_dup(run_command) : strdup("");
    snprintf(status.run_command, sizeof(status.run_command), "%s",
             guarded_run_command ? guarded_run_command : "");
    free(guarded_run_command);
    free(run_command);
  } else {
    status_add_blocker("missing-plan", "critical", "campaign plan report is not readable",
                       1.0, plan_command ? plan_command : "",
                       &status, &rows);
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-status",
                                                  "plan report not readable",
                                                  plan_path ? plan_path : ""));
  }

  if (status.history_readable && status.latest_full_pressure_report[0]) {
    status.latest_full_pressure_perf_rows =
        fuzz_all_report_lane_sub_rows(root, status.latest_full_pressure_report,
                                      "perf_triage");
    status.latest_full_pressure_perf_suite_current =
        status.current_perf_cases <= 0.0 ||
        status.latest_full_pressure_perf_rows >= status.current_perf_cases;
    if (!status.latest_full_pressure_perf_suite_current) {
      status.latest_full_pressure_clean = false;
      status.latest_full_pressure_attention = true;
      status.full_pressure_ready = false;
      if (status.latest_report[0] &&
          strcmp(status.latest_report, status.latest_full_pressure_report) == 0) {
        status.latest_report_clean = false;
        status.latest_report_attention = true;
      }
    }
  }

  bool latest_only_non_reproducing_afl_timeout =
      status.worklist_readable &&
      status.active_items <= 0.0 &&
      status.non_reproducing_afl_timeouts > 0.0 &&
      status.latest_failure_count > 0.0 &&
      status.latest_failure_count <= status.non_reproducing_afl_timeouts &&
      status.latest_finding_live <= 0.0 &&
      status.latest_finding_missing <= 0.0 &&
      status.latest_known_reproduced <= 0.0 &&
      status.latest_known_lost <= 0.0 &&
      status.latest_known_baseline <= 0.0 &&
      status.latest_perf_hotspots <= 0.0;
  if (latest_only_non_reproducing_afl_timeout) {
    status.latest_only_non_reproducing_afl_timeout = 1.0;
    status.latest_report_demoted_non_reproducing_afl_timeout = true;
    status.latest_report_clean = true;
    status.latest_report_attention = false;
    if (status.latest_full_pressure_report[0] &&
        status.latest_report[0] &&
        strcmp(status.latest_full_pressure_report, status.latest_report) == 0 &&
        status.latest_full_pressure_perf_suite_current) {
      status.latest_full_pressure_demoted_non_reproducing_afl_timeout = true;
      status.latest_full_pressure_clean = true;
      status.latest_full_pressure_attention = false;
    }
  }

  bool latest_report_is_latest_full_pressure =
      status.latest_full_pressure_report[0] &&
      status.latest_report[0] &&
      strcmp(status.latest_full_pressure_report, status.latest_report) == 0;
  bool latest_full_pressure_only_non_reproducing_afl_timeout =
      latest_report_is_latest_full_pressure &&
      latest_only_non_reproducing_afl_timeout;
  if (!latest_full_pressure_only_non_reproducing_afl_timeout &&
      status.history_readable &&
      status.latest_full_pressure_report[0] &&
      !status.latest_full_pressure_clean &&
      status.latest_full_pressure_perf_suite_current &&
      status.latest_full_pressure_failure_count > 0.0 &&
      status.latest_full_pressure_finding_live <= 0.0 &&
      status.latest_full_pressure_finding_missing <= 0.0 &&
      status.latest_full_pressure_known_reproduced <= 0.0 &&
      status.latest_full_pressure_known_lost <= 0.0 &&
      status.latest_full_pressure_known_baseline <= 0.0 &&
      status.latest_full_pressure_perf_hotspots <= 0.0) {
    latest_full_pressure_only_non_reproducing_afl_timeout =
        fuzz_all_report_only_non_reproducing_afl_timeout(
            root, status.latest_full_pressure_report);
  }
  if (latest_full_pressure_only_non_reproducing_afl_timeout) {
    if (status.non_reproducing_afl_timeouts < 1.0)
      status.non_reproducing_afl_timeouts = 1.0;
    status.latest_full_pressure_demoted_non_reproducing_afl_timeout = true;
    status.latest_full_pressure_clean = true;
    status.latest_full_pressure_attention = false;
    if (latest_report_is_latest_full_pressure) {
      status.latest_only_non_reproducing_afl_timeout = 1.0;
      status.latest_report_demoted_non_reproducing_afl_timeout = true;
      status.latest_report_clean = true;
      status.latest_report_attention = false;
    }
  }
  snprintf(status.latest_full_pressure_clean_reason,
           sizeof(status.latest_full_pressure_clean_reason), "%s",
           status.latest_full_pressure_clean ?
               (status.latest_full_pressure_demoted_non_reproducing_afl_timeout ?
                    "demoted-non-reproducing-afl-timeout" : "ok") :
               (status.latest_full_pressure_perf_suite_current ?
                    "attention" : "stale-perf-suite"));

  const char *active_attention_command =
      status.active_raw_repro_command[0] ? status.active_raw_repro_command :
      status.active_primary_command[0] ? status.active_primary_command :
      (worklist_command ? worklist_command : "");
  char guarded_full_pressure_command[4096] = {0};
  if (status.next_script[0]) {
    if (path_is_absolute(status.next_script))
      snprintf(guarded_full_pressure_command,
               sizeof(guarded_full_pressure_command), "%s",
               status.next_script);
    else {
      size_t n = strlen(status.next_script);
      if (n > sizeof(guarded_full_pressure_command) - 3)
        n = sizeof(guarded_full_pressure_command) - 3;
      guarded_full_pressure_command[0] = '.';
      guarded_full_pressure_command[1] = '/';
      memcpy(guarded_full_pressure_command + 2, status.next_script, n);
      guarded_full_pressure_command[2 + n] = '\0';
    }
  } else if (handoff_command[0]) {
    snprintf(guarded_full_pressure_command,
             sizeof(guarded_full_pressure_command), "%s",
             handoff_command);
  } else if (full_run_command && *full_run_command) {
    snprintf(guarded_full_pressure_command,
             sizeof(guarded_full_pressure_command), "%s",
             full_run_command);
  }
  if (status.worklist_readable && status.active_items > 0.0)
    status_add_blocker("active-worklist", "critical", "latest campaign has active work items",
                       status.active_items,
                       active_attention_command,
                       &status, &rows);
  if (status.coverage_readable && status.coverage_blocker_gaps > 0.0)
    status_add_blocker("coverage", "high", "full-pressure coverage blockers remain",
                       status.coverage_blocker_gaps,
                       coverage_command ? coverage_command : "",
                       &status, &rows);
  if (status.history_readable && status.reports <= 0.0)
    status_add_blocker("history-empty", "high", "no saved all-run reports were found",
                       1.0,
                       guarded_full_pressure_command,
                       &status, &rows);
      if (status.history_readable && !status.latest_report_clean)
        status_add_blocker("latest-report", "critical", "latest all-run report still needs attention",
                           1.0,
                           active_attention_command,
                           &status, &rows);
      if (status.history_readable && status.latest_full_pressure_report[0] &&
          !status.latest_full_pressure_clean) {
        const char *reason =
            "latest full-pressure all-run report still needs attention";
        double count = 1.0;
        if (!status.latest_full_pressure_perf_suite_current) {
          reason =
              "latest full-pressure all-run report predates the current perf suite";
          count =
              status.current_perf_cases - status.latest_full_pressure_perf_rows;
          if (count < 1.0) count = 1.0;
        }
        status_add_blocker("latest-full-pressure", "critical", reason,
                           count,
                           guarded_full_pressure_command,
                           &status, &rows);
      }
  if (!status.ny_bin_exists)
    status_add_blocker("nytrix-bin", "critical", "Nytrix compiler binary is not executable",
                       1.0,
                       "cmake --build ../nytrix/build/release -j$(nproc)",
                       &status, &rows);
  if (!status.cache_policy_ok) {
    char cache_reason[1024];
    snprintf(cache_reason, sizeof(cache_reason),
             "campaign cache/scratch policy is not clean");
    status_add_blocker("cache-policy", "critical", cache_reason,
                       1.0,
                       "mkdir -p build/cache/tmp build/cache/scratch build/cache/xdg build/cache/nytrix",
                       &status, &rows);
  }
  if (!status.plan_readable) {
    status.long_run_ready = false;
  }
  if (status.plan_readable && status.long_run_ready &&
      (!status.active_clear || !status.full_pressure_ready)) {
    status.long_run_ready = false;
  }
  status.campaign_complete =
      status.target_reached && status.blocker_count == 0 && status.long_run_ready;

  if (status.next_script[0]) {
    if (path_is_absolute(status.next_script) ||
        strncmp(status.next_script, "./", 2) == 0) {
      snprintf(status.next_handoff_command, sizeof(status.next_handoff_command), "%s",
               status.next_script);
    } else {
      snprintf(status.next_handoff_command, sizeof(status.next_handoff_command), "./%s",
               status.next_script);
    }
  } else if (handoff_command[0]) {
    snprintf(status.next_handoff_command, sizeof(status.next_handoff_command), "%s",
             handoff_command);
  } else if (status.run_command[0]) {
    snprintf(status.next_handoff_command, sizeof(status.next_handoff_command), "%s",
             status.run_command);
  } else if (full_run_command && *full_run_command) {
    snprintf(status.next_handoff_command, sizeof(status.next_handoff_command), "%s",
             full_run_command);
  }
  char *guarded_next_command =
      fuzz_all_low_priority_command_dup(status.next_handoff_command);
  snprintf(status.next_command, sizeof(status.next_command), "%s",
           guarded_next_command ? guarded_next_command : "");
  free(guarded_next_command);
  preview_command = fuzz_all_preview_command(status.next_handoff_command);
  snprintf(status.preview_command, sizeof(status.preview_command), "%s",
           preview_command ? preview_command : "");
  snprintf(status.state_refresh_command,
           sizeof(status.state_refresh_command), "%s",
           status.preview_command);
  char *coverage_next_preview_command =
      fuzz_all_coverage_next_preview_command(
          status.coverage_next_command, dir_cmd, target_cmd, hours_cmd,
          threads_cmd, profile_cmd);
  snprintf(status.coverage_next_preview_command,
           sizeof(status.coverage_next_preview_command), "%s",
           coverage_next_preview_command ? coverage_next_preview_command : "");
  free(coverage_next_preview_command);

  if (status.coverage_next_command[0] &&
      strstr(status.coverage_next_command, "fuzz all run") && dir_path &&
      *dir_path) {
    (void)asprintf(&missing_evidence_script_path,
                   "%s/run-missing-evidence.sh", dir_path);
    if (missing_evidence_script_path && *missing_evidence_script_path) {
      char *missing_evidence_rel =
          rel_path_dup(root, missing_evidence_script_path);
      if (missing_evidence_rel && *missing_evidence_rel) {
        if (path_is_absolute(missing_evidence_rel) ||
            strncmp(missing_evidence_rel, "./", 2) == 0)
          snprintf(status.coverage_next_guarded_command,
                   sizeof(status.coverage_next_guarded_command), "%s",
                   missing_evidence_rel);
        else
          snprintf(status.coverage_next_guarded_command,
                   sizeof(status.coverage_next_guarded_command), "./%s",
                   missing_evidence_rel);
      }
      free(missing_evidence_rel);
    }
    coverage_next_state_file =
        fuzz_all_missing_evidence_state_file_path(dir_cmd);
    coverage_next_state_command =
        fuzz_all_missing_evidence_state_command(coverage_next_state_file);
    coverage_next_state_refresh_command =
        fuzz_all_missing_evidence_state_refresh_command(
            status.coverage_next_guarded_command);
    coverage_next_low_cpu_command =
        fuzz_all_missing_evidence_low_cpu_command(
            status.coverage_next_guarded_command);
    coverage_next_stop_file =
        fuzz_all_missing_evidence_stop_file_path(dir_cmd);
    coverage_next_stop_command =
        fuzz_all_missing_evidence_stop_command(coverage_next_stop_file);
    coverage_next_resume_command =
        fuzz_all_missing_evidence_resume_command(coverage_next_stop_file);
    snprintf(status.coverage_next_state_file,
             sizeof(status.coverage_next_state_file), "%s",
             coverage_next_state_file ? coverage_next_state_file : "");
    snprintf(status.coverage_next_state_command,
             sizeof(status.coverage_next_state_command), "%s",
             coverage_next_state_command ? coverage_next_state_command : "");
    snprintf(status.coverage_next_state_refresh_command,
             sizeof(status.coverage_next_state_refresh_command), "%s",
             coverage_next_state_refresh_command ?
                 coverage_next_state_refresh_command : "");
    snprintf(status.coverage_next_low_cpu_command,
             sizeof(status.coverage_next_low_cpu_command), "%s",
             coverage_next_low_cpu_command ? coverage_next_low_cpu_command : "");
    snprintf(status.coverage_next_stop_file,
             sizeof(status.coverage_next_stop_file), "%s",
             coverage_next_stop_file ? coverage_next_stop_file : "");
    snprintf(status.coverage_next_stop_command,
             sizeof(status.coverage_next_stop_command), "%s",
             coverage_next_stop_command ? coverage_next_stop_command : "");
    snprintf(status.coverage_next_resume_command,
             sizeof(status.coverage_next_resume_command), "%s",
             coverage_next_resume_command ? coverage_next_resume_command : "");
  }

  if (write_script && script_path && *script_path &&
      !write_fuzz_all_next_run_script(root, script_path, dir_path,
                                      history_path, worklist_path,
                                      coverage_path, plan_path,
                                      status_json_script_path,
                                      status_md_script_path,
                                      target_arg, hours_arg,
                                      threads_arg, profile_arg)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-status",
                                                  "next-run script write failed",
                                                  script_path));
  }
  if (write_script && missing_evidence_script_path &&
      *missing_evidence_script_path &&
      !write_fuzz_all_missing_evidence_script(
          root, missing_evidence_script_path, dir_path, history_path,
          worklist_path, coverage_path, plan_path, status_json_script_path,
          status_md_script_path, status.coverage_next_command, target_arg,
          hours_arg, threads_arg, profile_arg)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-status",
                                                  "missing-evidence script write failed",
                                                  missing_evidence_script_path));
  }

  fuzz_all_run_state_summary_t coverage_next_run_state;
  fuzz_all_load_run_state(root, status.coverage_next_state_file,
                          &coverage_next_run_state);
  status.coverage_next_state_refresh_required =
      status.coverage_next_command[0] &&
      status.coverage_next_state_refresh_command[0] &&
      fuzz_all_run_state_refresh_required(&coverage_next_run_state);
  snprintf(status.coverage_next_state_refresh_reason,
           sizeof(status.coverage_next_state_refresh_reason), "%s",
           status.coverage_next_state_refresh_required ?
               fuzz_all_run_state_refresh_reason(&coverage_next_run_state) :
               "");
  snprintf(status.recommended_state_refresh_command,
           sizeof(status.recommended_state_refresh_command), "%s",
           status.coverage_next_state_refresh_required ?
               status.coverage_next_state_refresh_command : "");

  (void)string_list_push_take(
      &rows, make_fuzz_all_status_summary_row(&status,
                                              &run_state,
                                              &coverage_next_run_state,
                                              coverage_queue_json));
  string_list_move_last_to_front(&rows);

  bool incomplete_coverage_only =
      allow_incomplete_coverage && status.coverage_readable &&
      status.coverage_blocker_gaps > 0.0 && status.blocker_count == 1;
  bool empty_preflight_history_only =
      allow_empty_preflight_history && status.history_readable &&
      status.reports <= 0.0;
  bool full_pressure_remediation_only =
      status_allows_full_pressure_remediation(&status);
  if (strict && status.blocker_count > 0 && !incomplete_coverage_only &&
      !empty_preflight_history_only &&
      !full_pressure_remediation_only) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-status",
                                                  "status blockers present",
                                                  status.plan_report));
  }
  if (markdown_path && *markdown_path &&
      !write_fuzz_all_status_markdown(markdown_path, &status, &rows,
                                      &coverage_next_run_state)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-status",
                                                  "status markdown write failed",
                                                  markdown_path));
  }

  fuzz_all_score_summary_t status_score;
  fuzz_all_score_from_status(&status, &status_score);
  fuzz_all_projection_summary_t status_projection;
  fuzz_all_projection_from_status(&status, &status_score, &status_projection);
  bool status_campaign_ready =
      status.blocker_count == 0 && status.long_run_ready;
  double status_compiler_findings =
      status.latest_finding_live + status.latest_finding_missing;
  double status_known_bug_findings =
      status.latest_known_reproduced + status.latest_known_lost +
      status.latest_known_baseline;
  double status_correctness_findings =
      status_compiler_findings + status_known_bug_findings;
  double status_perf_hotspots_open = status.latest_perf_hotspots;
  double status_perf_worst_ratio = status.latest_perf_max_ratio;
  const char *status_perf_worst_case = status.latest_perf_max_case;
  if (status.latest_full_pressure_perf_suite_current &&
      status.latest_full_pressure_perf_max_ratio > status_perf_worst_ratio) {
    status_perf_worst_ratio = status.latest_full_pressure_perf_max_ratio;
    status_perf_worst_case = status.latest_full_pressure_perf_max_case;
  }
  status_perf_worst_case = fuzz_all_perf_effective_worst_case(
      status_perf_worst_case, status.perf_watchlist_artifact_readable,
      status.perf_watchlist_artifact_fresh,
      status.perf_watchlist_artifact_max_ratio,
      status.perf_watchlist_artifact_max_case);
  status_perf_worst_ratio = fuzz_all_perf_effective_worst_ratio(
      status_perf_worst_ratio, status.perf_watchlist_artifact_readable,
      status.perf_watchlist_artifact_fresh,
      status.perf_watchlist_artifact_max_ratio);
  double status_perf_watchlist_open =
      fuzz_all_perf_watchlist_effective_open(
          status_perf_hotspots_open, status_perf_worst_ratio,
          status.perf_watchlist_artifact_readable,
          status.perf_watchlist_artifact_fresh,
          status.perf_watchlist_artifact_hotspots);
  const char *status_perf_watchlist_case =
      status_perf_watchlist_open > 0.0 ? status_perf_worst_case : "";
  const char *status_perf_watchlist_state =
      fuzz_all_perf_watchlist_state(status_perf_watchlist_open,
                                    status.perf_watchlist_artifact_readable,
                                    status.perf_watchlist_artifact_fresh,
                                    status.perf_watchlist_artifact_hotspots);
  const char *status_perf_watchlist_action =
      fuzz_all_perf_watchlist_action(status_perf_watchlist_state);
  char *status_perf_watchlist_action_command =
      fuzz_all_perf_watchlist_action_command(
          status_perf_watchlist_state, status.perf_watchlist_command,
          status.perf_watchlist_markdown, status.perf_watchlist_report);
  const char *status_optimization_reason =
      fuzz_all_optimization_reason(status_perf_watchlist_state);
  const char *status_optimization_case =
      fuzz_all_optimization_case(status.perf_watchlist_artifact_readable,
                                 status.perf_watchlist_artifact_fresh,
                                 status.perf_watchlist_artifact_hotspots,
                                 status.perf_watchlist_artifact_max_case,
                                 status.perf_watchlist_artifact_max_ratio,
                                 status_perf_watchlist_open,
                                 status_perf_watchlist_case);
  double status_optimization_ratio =
      fuzz_all_optimization_ratio(status.perf_watchlist_artifact_readable,
                                  status.perf_watchlist_artifact_fresh,
                                  status.perf_watchlist_artifact_hotspots,
                                  status.perf_watchlist_artifact_max_ratio,
                                  status_perf_watchlist_open,
                                  status_perf_worst_ratio);
  const char *status_optimization_artifact =
      fuzz_all_optimization_artifact_path(
          status.perf_watchlist_artifact_readable,
          status.perf_watchlist_artifact_fresh,
          status.perf_watchlist_artifact_hotspots,
          status.perf_watchlist_artifact_max_ratio,
          status.perf_watchlist_artifact_max_artifact);
  const char *status_optimization_ny_source =
      fuzz_all_optimization_artifact_path(
          status.perf_watchlist_artifact_readable,
          status.perf_watchlist_artifact_fresh,
          status.perf_watchlist_artifact_hotspots,
          status.perf_watchlist_artifact_max_ratio,
          status.perf_watchlist_artifact_max_ny_source);
  const char *status_optimization_c_source =
      fuzz_all_optimization_artifact_path(
          status.perf_watchlist_artifact_readable,
          status.perf_watchlist_artifact_fresh,
          status.perf_watchlist_artifact_hotspots,
          status.perf_watchlist_artifact_max_ratio,
          status.perf_watchlist_artifact_max_c_source);
  char *status_optimization_target_command =
      fuzz_all_optimization_target_command(status_optimization_ny_source,
                                           status_optimization_c_source,
                                           status_optimization_artifact);
  fuzz_all_recommendation_t status_recommendation;
  fuzz_all_recommendation_make(
      &status_recommendation, status_campaign_ready,
      fuzz_all_state_phase_live(status.state_phase), status_score.evidence_fresh,
      status.cache_policy_ok, status.ny_bin_exists,
      status.active_old_nytrix_output_writer_present, status.blocker_count,
      status.active_items, status_correctness_findings,
      status_perf_hotspots_open, status_projection.runs_to_good_stability,
        status.runs_needed, status.target_reached, status.campaign_complete,
        status.next_handoff_command, status.state_command, status.old_path_command,
        status.advisory_action_command, status.progress_command,
        status.coverage_next_command, status.coverage_next_guarded_command,
        status.coverage_next_low_cpu_command,
        status.coverage_next_preview_command);
  const char *status_selected_preview_command =
      fuzz_all_selected_run_preview_command(status.preview_command,
                                            &status_recommendation);
  const char *status_selected_preview_for_refresh =
      fuzz_all_recommendation_run_preview_selected(&status_recommendation) ?
          status_recommendation.preview_command : "";
  const char *status_selected_state_refresh_command =
      fuzz_all_selected_run_state_refresh_command(
          status.state_refresh_command, status_selected_preview_for_refresh,
          &run_state);
  char status_run_next_gentle_command[4096] = {0};
  char status_run_next_gentle_preview_command[4096] = {0};
  fuzz_all_gentle_run_command(
      status_run_next_gentle_command, sizeof(status_run_next_gentle_command),
      status.next_handoff_command[0] ? status.next_handoff_command :
                                       status.next_command);
  fuzz_all_gentle_preview_command(
      status_run_next_gentle_preview_command,
      sizeof(status_run_next_gentle_preview_command),
      status.next_handoff_command[0] ? status.next_handoff_command :
                                       status.next_command);
  str_buf_t extra = {0};
  (void)sb_appendf(&extra, ",\"blockers\":%d,\"active_runs\":%.0f",
                   status.blocker_count, status.active_items);
  (void)sb_append(&extra, ",\"history_report\":");
  (void)sb_append_json_str(&extra, status.history_report);
  (void)sb_append(&extra, ",\"worklist_report\":");
  (void)sb_append_json_str(&extra, status.worklist_report);
  (void)sb_append(&extra, ",\"historical_worklist_report\":");
  (void)sb_append_json_str(&extra, status.historical_worklist_report);
  (void)sb_append(&extra, ",\"historical_worklist_markdown\":");
  (void)sb_append_json_str(&extra, status.historical_worklist_markdown);
  (void)sb_append(&extra, ",\"coverage_report\":");
  (void)sb_append_json_str(&extra, status.coverage_report);
  (void)sb_append(&extra, ",\"plan_report\":");
  (void)sb_append_json_str(&extra, status.plan_report);
  append_fuzz_all_compiler_std_audit_fields(&extra, &status);
  if (markdown_path && *markdown_path) {
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, root, markdown_path);
  }
  (void)sb_append(&extra, ",\"latest_report\":");
  (void)sb_append_json_str(&extra, status.latest_report);
  (void)sb_append(&extra, ",\"latest_full_pressure_report\":");
  (void)sb_append_json_str(&extra, status.latest_full_pressure_report);
    (void)sb_append(&extra, ",\"next_script\":");
  (void)sb_append_json_str(&extra, status.next_script);
  (void)sb_append(&extra, ",\"next_handoff_command\":");
  (void)sb_append_json_str(&extra, status.next_handoff_command);
  (void)sb_append(&extra, ",\"next_command\":");
  (void)sb_append_json_str(&extra, status.next_command);
  (void)sb_append(&extra, ",\"preview_command\":");
  (void)sb_append_json_str(&extra, status_selected_preview_command);
  (void)sb_append(&extra, ",\"run_next_command\":");
  (void)sb_append_json_str(&extra, status.next_command);
  (void)sb_append(&extra, ",\"run_next_preview_command\":");
  (void)sb_append_json_str(&extra, status_selected_preview_command);
  (void)sb_append(&extra, ",\"run_next_low_cpu_command\":");
  (void)sb_append_json_str(&extra, status.next_command);
  (void)sb_append(&extra, ",\"run_next_gentle_command\":");
  (void)sb_append_json_str(&extra, status_run_next_gentle_command);
  (void)sb_append(&extra, ",\"run_next_gentle_preview_command\":");
  (void)sb_append_json_str(&extra, status_run_next_gentle_preview_command);
  (void)sb_append(&extra, ",\"stop_file\":");
  (void)sb_append_json_str(&extra, status.stop_file);
  (void)sb_append(&extra, ",\"stop_command\":");
  (void)sb_append_json_str(&extra, status.stop_command);
  (void)sb_append(&extra, ",\"resume_command\":");
  (void)sb_append_json_str(&extra, status.resume_command);
  (void)sb_append(&extra, ",\"state_file\":");
  (void)sb_append_json_str(&extra, status.state_file);
  (void)sb_append(&extra, ",\"state_command\":");
  (void)sb_append_json_str(&extra, status.state_command);
  (void)sb_append(&extra, ",\"state_refresh_command\":");
  (void)sb_append_json_str(&extra, status_selected_state_refresh_command);
  append_fuzz_all_run_state_fields(&extra, &run_state);
  (void)sb_append(&extra, ",\"progress_command\":");
  (void)sb_append_json_str(&extra, status.progress_command);
  append_fuzz_all_probe_alias_fields(&extra, root, status_json_cmd,
                                     status.state_file);
  char status_extra_freshness_action_command[4096] = {0};
  if (status_score.freshness_penalty > 0.0)
    fuzz_all_freshness_action_command(status_extra_freshness_action_command,
                                      sizeof(status_extra_freshness_action_command),
                                      status.next_handoff_command);
  (void)sb_append(&extra, ",\"freshness_action_command\":");
  (void)sb_append_json_str(&extra, status_extra_freshness_action_command);
  (void)sb_append(&extra, ",\"latest_report_freshness_command\":");
  (void)sb_append_json_str(&extra,
                           !status_score.latest_report_fresh ?
                               status_extra_freshness_action_command : "");
  (void)sb_append(&extra, ",\"latest_full_pressure_report_freshness_command\":");
  (void)sb_append_json_str(&extra,
                           !status_score.latest_full_pressure_report_fresh ?
                               status_extra_freshness_action_command : "");
  (void)sb_append(&extra, ",\"full_pressure_freshen_command\":");
  (void)sb_append_json_str(&extra,
                           !status_score.latest_full_pressure_report_fresh ?
                               status_extra_freshness_action_command : "");
  (void)sb_append(&extra, ",\"full_pressure_remediation_command\":");
  (void)sb_append_json_str(&extra,
                           !status_score.latest_full_pressure_report_fresh ?
                               status_extra_freshness_action_command : "");
  (void)sb_append(&extra, ",\"full_pressure_action_command\":");
  (void)sb_append_json_str(&extra,
                           !status_score.latest_full_pressure_report_fresh ?
                               status_extra_freshness_action_command : "");
  (void)sb_append(&extra, ",\"status_command\":");
  (void)sb_append_json_str(&extra, status.status_command);
  (void)sb_append(&extra, ",\"old_path_probe_command\":");
  (void)sb_append_json_str(&extra, NYTRIX_FUZZ_ALL_OLD_PATH_PROBE_COMMAND);
  (void)sb_append(&extra, ",\"old_path_command\":");
  (void)sb_append_json_str(&extra, status.old_path_command);
  (void)sb_append(&extra, ",\"old_path_dry_run_command\":");
  (void)sb_append_json_str(&extra, status.old_path_dry_run_command);
  (void)sb_append(&extra, ",\"old_path_apply_command\":");
  (void)sb_append_json_str(&extra, status.old_path_apply_command);
  (void)sb_append(&extra, ",\"old_path_next_action\":");
  (void)sb_append_json_str(&extra, status.old_path_next_action);
  (void)sb_append(&extra, ",\"old_path_next_reason\":");
  (void)sb_append_json_str(&extra, status.old_path_next_reason);
  (void)sb_append(&extra, ",\"old_path_report\":");
  (void)sb_append_json_str(&extra, status.old_path_report);
  (void)sb_append(&extra, ",\"old_path_markdown\":");
  (void)sb_append_json_str(&extra, status.old_path_markdown);
  (void)sb_appendf(&extra,
                   ",\"old_path_cache_policy_ok\":%s,"
                   "\"old_path_present_count\":%.0f,"
                   "\"old_path_moved_count\":%.0f,"
                   "\"old_path_remaining_count\":%.0f,"
                   "\"old_path_wait_remaining_seconds\":%.0f,"
                   "\"old_path_artifact_leak_count\":%.0f,"
                   "\"old_path_artifact_moved_count\":%.0f,"
                   "\"old_path_artifact_remaining_count\":%.0f",
                   status.old_path_cache_policy_ok ? "true" : "false",
                   status.old_path_present_count,
                   status.old_path_moved_count,
                   status.old_path_remaining_count,
                   status.old_path_wait_remaining_seconds,
                   status.old_path_artifact_leak_count,
                   status.old_path_artifact_moved_count,
                   status.old_path_artifact_remaining_count);
  (void)sb_append(&extra, ",\"advisory_action_command\":");
  (void)sb_append_json_str(&extra,
                           status.non_reproducing_afl_timeouts > 0.0 ||
                                   status.historical_non_reproducing_afl_timeouts > 0.0 ?
                               status.advisory_action_command : "");
  (void)sb_append(&extra, ",\"advisory_recheck_command\":");
  (void)sb_append_json_str(&extra,
                           status.non_reproducing_afl_timeouts > 0.0 ||
                                   status.historical_non_reproducing_afl_timeouts > 0.0 ?
                               status.advisory_recheck_command : "");
  bool status_advisory_present =
      status.non_reproducing_afl_timeouts > 0.0 ||
      status.historical_non_reproducing_afl_timeouts > 0.0;
  (void)sb_append(&extra, ",\"advisory_recheck_state\":");
  (void)sb_append_json_str(
      &extra,
      fuzz_all_advisory_recheck_state(
          status_advisory_present,
          status.advisory_recheck_raw_repro_checked,
          status.advisory_recheck_raw_repro_passed,
          status.advisory_recheck_raw_repro_timeouts,
          status.advisory_recheck_raw_repro_unexpected,
          status.advisory_recheck_command));
  (void)sb_appendf(
      &extra,
      ",\"advisory_recheck_raw_repro_checked\":%.0f,"
      "\"advisory_recheck_raw_repro_passed\":%.0f,"
      "\"advisory_recheck_raw_repro_timeouts\":%.0f,"
      "\"advisory_recheck_raw_repro_unexpected\":%.0f",
      status_advisory_present ? status.advisory_recheck_raw_repro_checked : 0.0,
      status_advisory_present ? status.advisory_recheck_raw_repro_passed : 0.0,
      status_advisory_present ? status.advisory_recheck_raw_repro_timeouts : 0.0,
      status_advisory_present ? status.advisory_recheck_raw_repro_unexpected : 0.0);
  (void)sb_append(&extra, ",\"perf_watchlist_command\":");
  (void)sb_append_json_str(&extra, status.perf_watchlist_command);
  (void)sb_append(&extra, ",\"perf_watchlist_state\":");
  (void)sb_append_json_str(&extra, status_perf_watchlist_state);
  (void)sb_append(&extra, ",\"perf_watchlist_action\":");
  (void)sb_append_json_str(&extra, status_perf_watchlist_action);
  (void)sb_append(&extra, ",\"perf_watchlist_action_command\":");
  (void)sb_append_json_str(&extra,
                           status_perf_watchlist_action_command ?
                               status_perf_watchlist_action_command : "");
  (void)sb_append(&extra, ",\"optimization_action\":");
  (void)sb_append_json_str(&extra, status_perf_watchlist_action);
  (void)sb_append(&extra, ",\"optimization_reason\":");
  (void)sb_append_json_str(&extra, status_optimization_reason);
    (void)sb_append(&extra, ",\"optimization_command\":");
    (void)sb_append_json_str(&extra,
                             status_perf_watchlist_action_command ?
                                 status_perf_watchlist_action_command : "");
  (void)sb_append(&extra, ",\"optimization_target_command\":");
  (void)sb_append_json_str(&extra,
                           status_optimization_target_command ?
                               status_optimization_target_command : "");
      (void)sb_append(&extra, ",\"optimization_case\":");
    (void)sb_append_json_str(&extra, status_optimization_case);
  (void)sb_append(&extra, ",\"optimization_artifact\":");
  (void)sb_append_json_str(&extra, status_optimization_artifact);
  (void)sb_append(&extra, ",\"optimization_ny_source\":");
  (void)sb_append_json_str(&extra, status_optimization_ny_source);
  (void)sb_append(&extra, ",\"optimization_c_source\":");
  (void)sb_append_json_str(&extra, status_optimization_c_source);
    (void)sb_appendf(&extra,
                     ",\"optimization_ratio\":%.4f,"
                     "\"optimization_slowdown_percent\":%.2f",
                   status_optimization_ratio,
                   fuzz_all_perf_slowdown_percent(
                       status_optimization_ratio));
  (void)sb_append(&extra, ",\"perf_watchlist_report\":");
  (void)sb_append_json_str(&extra, status.perf_watchlist_report);
  (void)sb_append(&extra, ",\"perf_watchlist_markdown\":");
  (void)sb_append_json_str(&extra, status.perf_watchlist_markdown);
  (void)sb_append(&extra, ",\"perf_watchlist_artifact_readable\":");
  (void)sb_append(&extra,
                  status.perf_watchlist_artifact_readable ? "true" : "false");
  (void)sb_append(&extra, ",\"perf_watchlist_artifact_fresh\":");
  (void)sb_append(&extra,
                  status.perf_watchlist_artifact_fresh ? "true" : "false");
  (void)sb_appendf(&extra,
                   ",\"perf_watchlist_artifact_hotspots\":%.0f,"
                   "\"perf_watchlist_artifact_max_ratio\":%.4f,"
                   "\"perf_watchlist_artifact_max_slowdown_percent\":%.2f,"
                   "\"perf_watchlist_artifact_age_seconds\":%.0f,"
                   "\"perf_watchlist_artifact_stale_after_hours\":%.2f",
                   status.perf_watchlist_artifact_hotspots,
                   status.perf_watchlist_artifact_max_ratio,
                   fuzz_all_perf_slowdown_percent(
                       status.perf_watchlist_artifact_max_ratio),
                   status.perf_watchlist_artifact_age_seconds,
                   status.perf_watchlist_artifact_stale_after_hours);
    (void)sb_append(&extra, ",\"perf_watchlist_artifact_max_case\":");
    (void)sb_append_json_str(&extra,
                             status.perf_watchlist_artifact_max_case);
  (void)sb_append(&extra, ",\"perf_watchlist_artifact_max_artifact\":");
  (void)sb_append_json_str(&extra,
                           status.perf_watchlist_artifact_max_artifact);
  (void)sb_append(&extra, ",\"perf_watchlist_artifact_max_ny_source\":");
  (void)sb_append_json_str(&extra,
                           status.perf_watchlist_artifact_max_ny_source);
  (void)sb_append(&extra, ",\"perf_watchlist_artifact_max_c_source\":");
  (void)sb_append_json_str(&extra,
                           status.perf_watchlist_artifact_max_c_source);
  (void)sb_append(&extra, ",\"recommended_action\":");
  (void)sb_append_json_str(&extra, status_recommendation.action);
  (void)sb_append(&extra, ",\"recommended_reason\":");
  (void)sb_append_json_str(&extra, status_recommendation.reason);
    (void)sb_append(&extra, ",\"recommended_command\":");
    (void)sb_append_json_str(&extra, status_recommendation.command);
    (void)sb_append(&extra, ",\"recommended_low_cpu_command\":");
    (void)sb_append_json_str(&extra, status_recommendation.low_cpu_command);
    (void)sb_append(&extra, ",\"recommended_preview_command\":");
    (void)sb_append_json_str(&extra, status_recommendation.preview_command);
  (void)sb_append(&extra, ",\"recommended_repeat_mode\":");
  (void)sb_append_json_str(&extra, status_recommendation.repeat_mode);
  (void)sb_appendf(&extra, ",\"recommended_repeat_count\":%.0f",
                   status_recommendation.repeat_count);
  append_fuzz_all_coverage_next_fields(&extra, &status);
  append_fuzz_all_run_state_fields_prefixed(&extra, "coverage_next_state",
                                            &coverage_next_run_state);
  append_fuzz_all_recommended_state_fields(
      &extra, status_recommendation.action, &run_state,
      &coverage_next_run_state, status.state_file, status.state_command,
      status.state_refresh_command, status_recommendation.preview_command,
      status.coverage_next_state_file,
      status.coverage_next_state_command,
      status.coverage_next_state_refresh_command,
      status.coverage_next_state_refresh_required,
      status.coverage_next_state_refresh_reason);
  append_fuzz_all_handoff_guard_fields(&extra);
    (void)sb_append(&extra, ",\"active_primary_command\":");
  (void)sb_append_json_str(&extra, status.active_primary_command);
  (void)sb_append(&extra, ",\"active_raw_repro_command\":");
  (void)sb_append_json_str(&extra, status.active_raw_repro_command);
  (void)sb_append(&extra, ",\"nytrix_root\":");
  append_repo_or_sibling_rel_json_str(&extra, status.nytrix_root,
                                      status.nytrix_root);
  (void)sb_append(&extra, ",\"nytrix_root\":");
  append_repo_or_sibling_rel_json_str(&extra, status.nytrix_root,
                                      status.nytrix_root);
  (void)sb_append(&extra, ",\"ny_bin\":");
  append_repo_or_sibling_rel_json_str(&extra, status.nytrix_root, status.ny_bin);
  (void)sb_append(&extra, ",\"tmp_dir\":");
  append_repo_or_sibling_rel_json_str(&extra, status.nytrix_root,
                                      status.tmp_dir);
  (void)sb_append(&extra, ",\"scratch_root\":");
  append_repo_or_sibling_rel_json_str(&extra, status.nytrix_root,
                                      status.scratch_root);
  (void)sb_append(&extra, ",\"xdg_cache_home\":");
  append_repo_or_sibling_rel_json_str(&extra, status.nytrix_root,
                                      status.xdg_cache_home);
  (void)sb_append(&extra, ",\"nytrix_cache_dir\":");
  append_repo_or_sibling_rel_json_str(&extra, status.nytrix_root,
                                      status.nytrix_cache_dir);
  (void)sb_append(&extra, ",\"old_nytrix_test_scratch\":");
  (void)sb_append_json_str(&extra, "old-sibling-test-scratch");
  (void)sb_append(&extra, ",\"old_nytrix_fuzz_dir\":");
  (void)sb_append_json_str(&extra, "old-sibling-fuzz-dir");
  (void)sb_append(&extra, ",\"old_nytrix_build_cache_dir\":");
  (void)sb_append_json_str(&extra, "old-sibling-build-cache");
  (void)sb_append(&extra, ",\"old_nytrix_build_cache_absent\":");
  (void)sb_append(&extra, status.old_nytrix_build_cache_absent ? "true" : "false");
  (void)sb_append(&extra, ",\"active_old_nytrix_cache_writer_present\":");
  (void)sb_append(&extra, status.active_old_nytrix_cache_writer_present ? "true" : "false");
  (void)sb_append(&extra, ",\"active_old_nytrix_cache_writer\":");
  (void)sb_append_json_str(&extra, status.active_old_nytrix_cache_writer);
  (void)sb_append(&extra, ",\"active_old_nytrix_output_writer_present\":");
  (void)sb_append(&extra, status.active_old_nytrix_output_writer_present ? "true" : "false");
  (void)sb_append(&extra, ",\"active_old_nytrix_output_writer\":");
  (void)sb_append_json_str(&extra, status.active_old_nytrix_output_writer);
  (void)sb_append(&extra, ",\"nytrix_git_head\":");
  (void)sb_append_json_str(&extra, status.nytrix_git_head);
  (void)sb_append(&extra, ",\"nytrix_git_head\":");
  (void)sb_append_json_str(&extra, status.nytrix_git_head);
  (void)sb_append(&extra, ",\"nytrix_git_status_hash\":");
  (void)sb_append_json_str(&extra, status.nytrix_git_status_hash);
  (void)sb_append(&extra, ",\"nytrix_git_status_hash\":");
  (void)sb_append_json_str(&extra, status.nytrix_git_status_hash);
  (void)sb_append(&extra, ",\"nytrix_bin_hash\":");
  (void)sb_append_json_str(&extra, status.nytrix_bin_hash);
  (void)sb_append(&extra, ",\"ny_bin_hash\":");
  (void)sb_append_json_str(&extra, status.ny_bin_hash);
  (void)sb_appendf(&extra,
                   ",\"history_readable\":%s,\"worklist_readable\":%s,"
                   "\"coverage_readable\":%s,\"plan_readable\":%s,"
                   "\"reports\":%.0f,"
                   "\"ignored_no_evidence_reports\":%.0f,"
                   "\"evidence_ignored_no_evidence_reports\":%.0f,"
                   "\"ok_reports\":%.0f,"
                   "\"failed_reports\":%.0f,\"attention_reports\":%.0f,"
                      "\"full_pressure_reports\":%.0f,"
                      "\"full_pressure_ok_reports\":%.0f,"
                      "\"full_pressure_attention_reports\":%.0f,"
                      "\"thread_hours\":%.4f,\"thread_years\":%.8f,"
                      "\"campaign_first_report_epoch\":%.0f,"
                      "\"campaign_latest_report_epoch\":%.0f,"
                      "\"campaign_calendar_span_days\":%.4f,"
                      "\"campaign_calendar_age_days\":%.4f,"
                      "\"campaign_calendar_percent_10y\":%.4f,"
                      "\"full_pressure_thread_hours\":%.4f,"
                      "\"full_pressure_thread_years\":%.8f,"
                   "\"checked_subcases\":%.0f,\"sub_failures_total\":%.0f,"
                   "\"active_items\":%.0f,"
                   "\"active_count\":%.0f,"
                   "\"active_failure_detail_count\":%.0f,"
                   "\"active_saved_hangs\":%.0f,"
                   "\"active_saved_crashes\":%.0f,"
                   "\"active_saved_inputs\":%.0f,"
                   "\"active_repro_commands\":%.0f,"
                   "\"active_raw_repro_commands\":%.0f,"
                   "\"active_repro_ready\":%.0f,"
                   "\"historical_attention_reports\":%.0f,"
                   "\"coverage_lanes\":%.0f,\"coverage_ran_lanes\":%.0f,"
                   "\"coverage_skipped_lanes\":%.0f,\"coverage_failed_lanes\":%.0f,"
                   "\"coverage_gaps\":%.0f,\"coverage_blocker_gaps\":%.0f,"
                   "\"coverage_advisory_gaps\":%.0f,"
                   "\"coverage_latest_report_advisory_gaps\":%.0f,"
                   "\"coverage_latest_report_companion_skipped_lanes\":%.0f,"
                     "\"coverage_reports_considered\":%.0f,"
                     "\"coverage_campaign_reports_considered\":%.0f,"
                     "\"coverage_companion_reports_considered\":%.0f,"
                     "\"coverage_disabled_lanes\":%.0f,"
                     "\"coverage_budget_short_lanes\":%.0f,"
                     "\"coverage_missing_tool_lanes\":%.0f,"
                     "\"coverage_depth_percent\":%.2f,"
                     "\"coverage_percent\":%.2f,"
                     "\"coverage_not_run_lanes\":%.0f,"
                       "\"target_thread_years\":%.8f,"
                       "\"remaining_thread_years\":%.8f,"
                       "\"target_percent\":%.4f,"
                       "\"campaign_percent\":%.4f,"
                       "\"campaign_confidence_percent\":%.4f,"
                       "\"campaign_remaining_percent\":%.4f,"
                       "\"remaining_percent\":%.4f,"
                     "\"runs_needed\":%.0f,\"wall_days_needed\":%.4f,"
                   "\"wall_hours_needed\":%.4f,"
                   "\"thread_hours_needed\":%.4f,"
                   "\"runs_per_day\":%.4f,"
                   "\"thread_years_per_day\":%.8f,"
                   "\"completion_eta_epoch\":%.0f,"
                   "\"active_compiler_finding_live\":%.0f,"
                   "\"active_compiler_finding_missing\":%.0f,"
                   "\"historical_compiler_finding_live\":%.0f,"
                   "\"historical_compiler_finding_missing\":%.0f,"
                   "\"active_known_bug_reproduced\":%.0f,"
                   "\"active_known_bug_lost_signal\":%.0f,"
                   "\"active_known_bug_baseline_failures\":%.0f,"
                   "\"historical_known_bug_reproduced\":%.0f,"
                   "\"historical_known_bug_lost_signal\":%.0f,"
                   "\"historical_known_bug_baseline_failures\":%.0f,"
                   "\"perf_hotspots\":%.0f,\"perf_max_ratio\":%.4f,"
                   "\"historical_perf_hotspots\":%.0f,"
                   "\"historical_perf_max_ratio\":%.4f,"
                   "\"active_perf_hotspots\":%.0f,"
                   "\"active_perf_max_ratio\":%.4f,"
                   "\"latest_failure_count\":%.0f,"
                   "\"latest_sub_failures\":%.0f,"
                   "\"latest_compiler_finding_live\":%.0f,"
                   "\"latest_compiler_finding_missing\":%.0f,"
                   "\"latest_known_bug_reproduced\":%.0f,"
                   "\"latest_known_bug_lost_signal\":%.0f,"
                   "\"latest_known_bug_baseline_failures\":%.0f,"
                       "\"latest_perf_hotspots\":%.0f,"
                       "\"latest_perf_max_ratio\":%.4f,"
                       "\"latest_full_pressure_failure_count\":%.0f,"
                       "\"latest_full_pressure_sub_failures\":%.0f,"
                       "\"latest_full_pressure_compiler_finding_live\":%.0f,"
                       "\"latest_full_pressure_compiler_finding_missing\":%.0f,"
                       "\"latest_full_pressure_known_bug_reproduced\":%.0f,"
                       "\"latest_full_pressure_known_bug_lost_signal\":%.0f,"
                       "\"latest_full_pressure_known_bug_baseline_failures\":%.0f,"
                       "\"latest_full_pressure_perf_hotspots\":%.0f,"
                       "\"latest_full_pressure_perf_max_ratio\":%.4f,"
                       "\"current_perf_cases\":%.0f,"
                       "\"latest_full_pressure_perf_rows\":%.0f,"
                       "\"latest_full_pressure_perf_suite_current\":%s,"
                       "\"correctness_findings\":%.0f,"
                       "\"compiler_findings\":%.0f,"
                         "\"known_bug_replay_findings\":%.0f,"
                         "\"perf_hotspots_open\":%.0f,"
                         "\"perf_worst_ratio\":%.4f,"
                         "\"perf_watchlist_open\":%.0f,"
                         "\"perf_watchlist_threshold_ratio\":%.4f,"
                         "\"coverage_detail_count\":%d,"
                         "\"coverage_backlog_lanes\":%d,"
                         "\"coverage_detail_rows\":%d,"
                   "\"active_clear\":%s,\"full_pressure_ready\":%s,"
                   "\"long_run_ready\":%s,\"target_reached\":%s,"
                   "\"campaign_complete\":%s,\"ny_bin_exists\":%s,"
                   "\"cache_policy_ok\":%s,"
                   "\"old_nytrix_test_scratch_absent\":%s,"
                   "\"old_nytrix_fuzz_absent\":%s,"
                   "\"nytrix_git_ok\":%s,\"nytrix_git_dirty\":%s,"
                   "\"nytrix_git_ok\":%s,\"nytrix_git_dirty\":%s,"
                       "\"latest_report_ok\":%s,"
                       "\"latest_report_attention\":%s,"
                       "\"latest_report_clean\":%s,"
                       "\"latest_full_pressure_ok\":%s,"
                       "\"latest_full_pressure_attention\":%s,"
                       "\"latest_full_pressure_clean\":%s,"
                       "\"latest_report_demoted_non_reproducing_afl_timeout\":%s,"
                       "\"latest_full_pressure_demoted_non_reproducing_afl_timeout\":%s,"
                       "\"strict\":%s,\"allow_incomplete_coverage\":%s,"
                       "\"allow_full_pressure_remediation\":%s,"
                   "\"refreshed\":%s,"
                   "\"blocker_count\":%d,\"ready\":%s",
                   status.history_readable ? "true" : "false",
                   status.worklist_readable ? "true" : "false",
                   status.coverage_readable ? "true" : "false",
                   status.plan_readable ? "true" : "false",
                   status.reports,
                   status.ignored_no_evidence_reports,
                   status.ignored_no_evidence_reports,
                   status.ok_reports,
                   status.failed_reports, status.attention_reports,
                   status.full_pressure_reports,
                   status.full_pressure_ok_reports,
                   status.full_pressure_attention_reports,
                      status.thread_hours, status.thread_years,
                      status.campaign_first_report_epoch,
                      status.campaign_latest_report_epoch,
                      status.campaign_calendar_span_days,
                      status.campaign_calendar_age_days,
                      fuzz_all_campaign_calendar_percent_10y(
                          status.campaign_calendar_age_days),
                      status.full_pressure_thread_hours,
                   status.full_pressure_thread_years,
                   status.checked_subcases, status.sub_failures_total,
                   status.active_items,
                   status.active_items,
                   status.active_failure_detail_count,
                   status.active_saved_hangs,
                   status.active_saved_crashes,
                   status.active_saved_inputs,
                   status.active_repro_commands,
                   status.active_raw_repro_commands,
                   status.active_repro_ready,
                   status.historical_attention_reports,
                   status.coverage_lanes, status.coverage_ran_lanes,
                   status.coverage_skipped_lanes, status.coverage_failed_lanes,
                   status.coverage_gaps, status.coverage_blocker_gaps,
                   status.coverage_advisory_gaps,
                   status.coverage_latest_report_advisory_gaps,
                   status.coverage_latest_report_companion_skipped_lanes,
                   status.coverage_reports_considered,
                   status.coverage_campaign_reports_considered,
                     status.coverage_companion_reports_considered,
                     status.coverage_disabled_lanes,
                     status.coverage_budget_short_lanes,
                     status.coverage_missing_tool_lanes,
                     fuzz_all_ratio_percent(status.coverage_ran_lanes,
                                            status.coverage_lanes),
                     fuzz_all_ratio_percent(status.coverage_ran_lanes,
                                            status.coverage_lanes),
                     fuzz_all_not_run_lanes(status.coverage_ran_lanes,
                                            status.coverage_lanes),
                     status.target_thread_years, status.remaining_thread_years,
                       status.target_percent, status.target_percent,
                           status.target_percent,
                       fuzz_all_campaign_remaining_percent(status.target_percent),
                       fuzz_all_campaign_remaining_percent(status.target_percent),
                     status.runs_needed,
                   status.wall_days_needed, status.wall_hours_needed,
                   status.thread_hours_needed,
                   status.runs_per_day, status.thread_years_per_day,
                   status.completion_eta_epoch,
                   status.latest_finding_live,
                   status.latest_finding_missing,
                   status.historical_finding_live,
                   status.historical_finding_missing,
                   status.latest_known_reproduced,
                   status.latest_known_lost,
                   status.latest_known_baseline,
                   status.historical_known_reproduced,
                   status.historical_known_lost,
                   status.historical_known_baseline,
                   status.perf_hotspots,
                   status.perf_max_ratio,
                   status.historical_perf_hotspots,
                   status.historical_perf_max_ratio,
                   status.latest_perf_hotspots,
                   status.latest_perf_max_ratio,
                   status.latest_failure_count,
                   status.latest_sub_failures,
                   status.latest_finding_live,
                   status.latest_finding_missing,
                   status.latest_known_reproduced,
                   status.latest_known_lost,
                   status.latest_known_baseline,
                       status.latest_perf_hotspots,
                       status.latest_perf_max_ratio,
                       status.latest_full_pressure_failure_count,
                       status.latest_full_pressure_sub_failures,
                       status.latest_full_pressure_finding_live,
                       status.latest_full_pressure_finding_missing,
                       status.latest_full_pressure_known_reproduced,
                       status.latest_full_pressure_known_lost,
                       status.latest_full_pressure_known_baseline,
                       status.latest_full_pressure_perf_hotspots,
                       status.latest_full_pressure_perf_max_ratio,
                       status.current_perf_cases,
                       status.latest_full_pressure_perf_rows,
                       status.latest_full_pressure_perf_suite_current ? "true" : "false",
                       status_correctness_findings,
                       status_compiler_findings,
                       status_known_bug_findings,
                       status_perf_hotspots_open,
                       status_perf_worst_ratio,
                       status_perf_watchlist_open,
                       fuzz_all_perf_watchlist_threshold(),
                       status.coverage_detail_count,
                       status.coverage_backlog_lanes,
                       status.coverage_detail_rows,
                   status.active_clear ? "true" : "false",
                   status.full_pressure_ready ? "true" : "false",
                   status.long_run_ready ? "true" : "false",
                   status.target_reached ? "true" : "false",
                   status.campaign_complete ? "true" : "false",
                   status.ny_bin_exists ? "true" : "false",
                   status.cache_policy_ok ? "true" : "false",
                   status.old_nytrix_test_scratch_absent ? "true" : "false",
                   status.old_nytrix_fuzz_absent ? "true" : "false",
                   status.nytrix_git_ok ? "true" : "false",
                   status.nytrix_git_dirty ? "true" : "false",
                   status.nytrix_git_ok ? "true" : "false",
                   status.nytrix_git_dirty ? "true" : "false",
                       status.latest_report_ok ? "true" : "false",
                       status.latest_report_attention ? "true" : "false",
                       status.latest_report_clean ? "true" : "false",
                       status.latest_full_pressure_ok ? "true" : "false",
                   status.latest_full_pressure_attention ? "true" : "false",
                   status.latest_full_pressure_clean ? "true" : "false",
                       status.latest_report_demoted_non_reproducing_afl_timeout ?
                           "true" : "false",
                       status.latest_full_pressure_demoted_non_reproducing_afl_timeout ?
                           "true" : "false",
                       strict ? "true" : "false",
                   allow_incomplete_coverage ? "true" : "false",
                          allow_full_pressure_remediation ? "true" : "false",
                          refresh ? "true" : "false",
                          status.blocker_count,
                          status.blocker_count == 0 && status.long_run_ready ? "true" : "false");
           append_fuzz_all_campaign_alias_fields(
               &extra, status.thread_years, status.target_thread_years,
            status.remaining_thread_years, status.target_percent,
            status.runs_needed, status.wall_hours_needed,
            status.wall_days_needed, status_projection.thread_years_per_run,
            status_projection.target_percent_per_run,
               status.runs_per_day, status.thread_years_per_day,
               status.campaign_plan_wall_hours,
               status.campaign_plan_threads,
               status.completion_eta_local);
  (void)sb_append(&extra, ",\"campaign_first_report\":");
  (void)sb_append_json_str(&extra, status.campaign_first_report);
        (void)sb_appendf(&extra,
                      ",\"coverage_queue_count\":%d,"
                   "\"coverage_queue_non_advisory_count\":%d,"
                   "\"coverage_queue_advisory_count\":%d",
                   status.coverage_queue_count,
                   status.coverage_queue_non_advisory_count,
                   status.coverage_queue_advisory_count);
  (void)sb_append(&extra, ",\"coverage_queue_lanes\":");
  (void)sb_append_json_str(&extra, status.coverage_queue_lanes);
  append_fuzz_all_coverage_queue_json(&extra, coverage_queue_json);
  (void)sb_append(&extra, ",\"coverage_state\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_coverage_state(status.coverage_lanes,
                                      status.coverage_ran_lanes,
                                      status.coverage_blocker_gaps,
                                      status.coverage_failed_lanes));
  (void)sb_append(&extra, ",\"campaign_state\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_campaign_state(status_campaign_ready,
                                      status.target_reached,
                                      status.campaign_complete));
  (void)sb_append(&extra, ",\"campaign_incomplete_reason\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_campaign_incomplete_reason(status_campaign_ready,
                                                  status.blocker_count,
                                                  status.active_items,
                                                  status.target_reached,
                                                  status.campaign_complete));
  (void)sb_append(&extra, ",\"completion_state\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_campaign_state(status_campaign_ready,
                                      status.target_reached,
                                      status.campaign_complete));
  (void)sb_append(&extra, ",\"completion_reason\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_campaign_incomplete_reason(status_campaign_ready,
                                                  status.blocker_count,
                                                  status.active_items,
                                                  status.target_reached,
                                                  status.campaign_complete));
  (void)sb_append(&extra, ",\"perf_max_case\":");
  (void)sb_append_json_str(&extra, status.perf_max_case);
  (void)sb_append(&extra, ",\"historical_perf_max_case\":");
  (void)sb_append_json_str(&extra, status.historical_perf_max_case);
  (void)sb_append(&extra, ",\"active_perf_max_case\":");
  (void)sb_append_json_str(&extra, status.perf_max_case);
  (void)sb_append(&extra, ",\"latest_perf_max_case\":");
  (void)sb_append_json_str(&extra, status.latest_perf_max_case);
  (void)sb_append(&extra, ",\"latest_full_pressure_perf_max_case\":");
  (void)sb_append_json_str(&extra, status.latest_full_pressure_perf_max_case);
  (void)sb_appendf(
      &extra, ",\"latest_full_pressure_perf_max_slowdown_percent\":%.2f",
      fuzz_all_perf_slowdown_percent(
          status.latest_full_pressure_perf_max_ratio));
  (void)sb_appendf(&extra, ",\"perf_worst_slowdown_percent\":%.2f",
                   fuzz_all_perf_slowdown_percent(status_perf_worst_ratio));
  (void)sb_append(&extra, ",\"perf_worst_case\":");
  (void)sb_append_json_str(&extra,
                           status_perf_worst_case ? status_perf_worst_case : "");
  (void)sb_append(&extra, ",\"perf_watchlist_case\":");
  (void)sb_append_json_str(&extra, status_perf_watchlist_case ?
                                       status_perf_watchlist_case : "");
    (void)sb_appendf(&extra,
                     ",\"advisory_timeouts\":%.0f,"
                   "\"current_advisory_timeouts\":%.0f,"
                   "\"historical_advisory_timeouts\":%.0f,"
                   "\"non_reproducing_afl_timeouts\":%.0f,"
                   "\"historical_non_reproducing_afl_timeouts\":%.0f,"
                   "\"latest_only_non_reproducing_afl_timeout\":%s",
                   status.non_reproducing_afl_timeouts,
                   status.non_reproducing_afl_timeouts,
                   status.historical_non_reproducing_afl_timeouts,
                   status.non_reproducing_afl_timeouts,
                     status.historical_non_reproducing_afl_timeouts,
                     status.latest_only_non_reproducing_afl_timeout > 0.0 ?
                         "true" : "false");
  (void)sb_append(&extra, ",\"advisory_state\":");
  (void)sb_append_json_str(
      &extra,
      fuzz_all_advisory_state(status.non_reproducing_afl_timeouts,
                              status.historical_non_reproducing_afl_timeouts));
  double status_effective_advisory_timeouts =
      fuzz_all_effective_advisory_timeouts(
          status.non_reproducing_afl_timeouts,
          status.advisory_recheck_raw_repro_checked,
          status.advisory_recheck_raw_repro_passed,
          status.advisory_recheck_raw_repro_timeouts,
          status.advisory_recheck_raw_repro_unexpected,
          status.advisory_recheck_command);
  (void)sb_appendf(&extra,
                   ",\"effective_advisory_timeouts\":%.0f,"
                   "\"advisory_effective_timeouts\":%.0f",
                   status_effective_advisory_timeouts,
                   status_effective_advisory_timeouts);
  (void)sb_append(&extra, ",\"advisory_penalty_state\":");
  (void)sb_append_json_str(
      &extra,
      fuzz_all_advisory_penalty_state(status_effective_advisory_timeouts));
  (void)sb_append(&extra, ",\"latest_full_pressure_clean_reason\":");
  (void)sb_append_json_str(&extra, status.latest_full_pressure_clean_reason);
  (void)sb_appendf(&extra,
                   ",\"latest_full_pressure_raw_ok\":%s,"
                   "\"latest_full_pressure_effective_clean\":%s",
                   status.latest_full_pressure_ok ? "true" : "false",
                   status.latest_full_pressure_clean ? "true" : "false");
  (void)sb_appendf(&extra,
                   ",\"stability_score_percent\":%.2f,"
                   "\"stability_percent\":%.2f,"
                   "\"score_percent\":%.2f,"
                   "\"stability_score\":%.2f,"
                   "\"language_score_percent\":%.2f,"
                   "\"language_score\":%.2f,"
                   "\"language_score_good_threshold_percent\":%.2f,"
                   "\"language_score_gap_percent\":%.2f,"
                   "\"signal_health_percent\":%.2f,"
                   "\"evidence_cap_percent\":%.2f,"
                   "\"language_score_signal_percent\":%.2f,"
                   "\"language_score_evidence_cap_percent\":%.2f,"
                   "\"thread_years_per_run\":%.8f,"
                   "\"target_percent_per_run\":%.4f,"
                   "\"next_run_target_percent\":%.4f,"
                   "\"next_run_stability_score_percent\":%.2f,"
                   "\"next_run_stability_score\":%.2f,"
                   "\"next_run_stability_delta_percent\":%.2f,"
                   "\"next_run_language_score_percent\":%.2f,"
                   "\"next_run_language_score\":%.2f,"
                   "\"next_run_language_score_delta_percent\":%.2f,"
                   "\"runs_to_good_stability\":%.0f,"
                   "\"runs_to_good_stability_score\":%.0f,"
                   "\"runs_to_good_language_score\":%.0f,"
                   "\"runs_to_good_days\":%.4f,"
                   "\"runs_to_good_stability_days\":%.4f,"
                   "\"runs_to_good_language_days\":%.4f,"
                   "\"days_to_good_stability\":%.4f,"
                   "\"days_to_good_language_score\":%.4f,"
                   "\"latest_report_age_seconds\":%.0f,"
                   "\"latest_report_age_hours\":%.2f,"
                   "\"latest_report_stale_after_hours\":%.2f,"
                   "\"latest_report_freshness_remaining_hours\":%.2f,"
                   "\"latest_report_freshness_overdue_hours\":%.2f,"
                   "\"latest_report_fresh\":%s,"
                   "\"latest_full_pressure_report_age_seconds\":%.0f,"
                   "\"latest_full_pressure_report_age_hours\":%.2f,"
                   "\"latest_full_pressure_report_stale_after_hours\":%.2f,"
                   "\"latest_full_pressure_report_freshness_remaining_hours\":%.2f,"
                   "\"latest_full_pressure_report_freshness_overdue_hours\":%.2f,"
                   "\"latest_full_pressure_report_fresh\":%s,"
                   "\"evidence_fresh\":%s,"
                   "\"evidence_freshness_overdue_hours\":%.2f,"
                   "\"gate_penalty\":%.2f,"
                   "\"correctness_penalty\":%.2f,"
                   "\"perf_penalty\":%.2f,"
                   "\"advisory_penalty\":%.2f,"
                   "\"environment_penalty\":%.2f,"
                   "\"freshness_penalty\":%.2f",
                   status_score.stability_score, status_score.stability_score,
                   status_score.stability_score, status_score.stability_score,
                   status_score.stability_score, status_score.stability_score,
                   fuzz_all_score_good_threshold(),
                   fuzz_all_language_good_gap_percent(status_score.stability_score),
                   status_score.signal_score,
                   status_score.evidence_cap,
                   status_score.signal_score,
                   status_score.evidence_cap,
                   status_projection.thread_years_per_run,
                   status_projection.target_percent_per_run,
                   status_projection.next_run_target_percent,
                   status_projection.next_run_stability_score,
                   status_projection.next_run_stability_score,
                   status_projection.next_run_stability_delta,
                   status_projection.next_run_stability_score,
                   status_projection.next_run_stability_score,
                   status_projection.next_run_stability_delta,
                   status_projection.runs_to_good_stability,
                   status_projection.runs_to_good_stability,
                   status_projection.runs_to_good_stability,
                   status_projection.runs_to_good_days,
                   status_projection.runs_to_good_days,
                   status_projection.runs_to_good_days,
                   status_projection.runs_to_good_days,
                   status_projection.runs_to_good_days,
                   status_score.latest_report_age_seconds,
                   status_score.latest_report_age_seconds >= 0.0 ?
                       fuzz_all_freshness_age_hours(
                           status_score.latest_report_age_seconds) : -1.0,
                   status_score.latest_report_stale_after_hours,
                   fuzz_all_freshness_remaining_hours(
                       status_score.latest_report_age_seconds,
                       status_score.latest_report_stale_after_hours),
                   fuzz_all_freshness_overdue_hours(
                       status_score.latest_report_age_seconds,
                       status_score.latest_report_stale_after_hours),
                   status_score.latest_report_fresh ? "true" : "false",
                   status_score.latest_full_pressure_report_age_seconds,
                   status_score.latest_full_pressure_report_age_seconds >= 0.0 ?
                       fuzz_all_freshness_age_hours(
                           status_score.latest_full_pressure_report_age_seconds) : -1.0,
                   status_score.latest_full_pressure_report_stale_after_hours,
                   fuzz_all_freshness_remaining_hours(
                       status_score.latest_full_pressure_report_age_seconds,
                       status_score.latest_full_pressure_report_stale_after_hours),
                   fuzz_all_freshness_overdue_hours(
                       status_score.latest_full_pressure_report_age_seconds,
                       status_score.latest_full_pressure_report_stale_after_hours),
                   status_score.latest_full_pressure_report_fresh ? "true" : "false",
                   status_score.evidence_fresh ? "true" : "false",
                   fuzz_all_evidence_freshness_overdue_hours(
                       status_score.latest_report_age_seconds,
                       status_score.latest_report_stale_after_hours,
                       status_score.latest_full_pressure_report_age_seconds,
                       status_score.latest_full_pressure_report_stale_after_hours),
                   status_score.gate_penalty, status_score.correctness_penalty,
                   status_score.perf_penalty, status_score.advisory_penalty,
                   status_score.environment_penalty,
                   status_score.freshness_penalty);
  append_fuzz_all_compact_score_alias_fields(
      &extra, status_score.stability_score, status_score.label,
      status_projection.next_run_stability_score,
      status_score.latest_report_age_seconds,
      status_score.latest_report_stale_after_hours,
      status_score.latest_full_pressure_report_age_seconds,
      status_score.latest_full_pressure_report_stale_after_hours);
  (void)sb_append(&extra, ",\"stability_label\":");
  (void)sb_append_json_str(&extra,
                           status_score.label ? status_score.label : "");
  (void)sb_append(&extra, ",\"thread_years_per_run_source\":");
  (void)sb_append_json_str(&extra,
                           status_projection.thread_years_per_run_source ?
                               status_projection.thread_years_per_run_source :
                               "");
  (void)sb_append(&extra, ",\"language_score_label\":");
  (void)sb_append_json_str(&extra,
                           status_score.label ? status_score.label : "");
  (void)sb_append(&extra, ",\"stability_note\":");
  (void)sb_append_json_str(&extra, status_score.note);
  (void)sb_append(&extra, ",\"language_score_note\":");
  (void)sb_append_json_str(&extra, status_score.note);
    (void)sb_append(&extra, ",\"completion_eta_local\":");
  (void)sb_append_json_str(&extra, status.completion_eta_local);
  if (status.run_command[0]) {
    (void)sb_append(&extra, ",\"run_command\":");
    (void)sb_append_json_str(&extra, status.run_command);
  }
  char *report = build_native_report_json_with_top_aliases(
      &rows, &failures, "fuzz-all-status", extra.data, true);
  int rc = emit_native_report(report, json_path, "all fuzz status",
                              rows.count, failures.count);
  free(extra.data);
  free(coverage_queue_json);
  free(history_path);
  free(worklist_path);
  free(coverage_path);
  free(plan_path);
  free(dir_path);
  free(script_path);
  free(missing_evidence_script_path);
  free(status_json_script_path);
  free(status_md_script_path);
  free(history_md_path);
  free(worklist_md_path);
  free(worklist_history_path);
  free(worklist_history_md_path);
  free(coverage_md_path);
  free(plan_md_path);
  free(old_paths_json_path);
  free(old_paths_md_path);
  free(dir_cmd_path);
  free(history_cmd_path);
  free(history_md_cmd_path);
  free(worklist_cmd_path);
  free(worklist_md_cmd_path);
  free(worklist_history_cmd_path);
  free(worklist_history_md_cmd_path);
  free(coverage_cmd_path);
  free(coverage_md_cmd_path);
  free(plan_cmd_path);
  free(plan_md_cmd_path);
  free(status_json_cmd_path);
  free(status_md_cmd_path);
  free(status_progress_json_cmd);
  free(status_progress_md_cmd);
  free(history_command);
  free(worklist_command);
  free(coverage_command);
  free(plan_command);
  free(progress_command);
  free(progress_command_raw);
  free(preview_command);
    free(status_command);
    free(status_command_raw);
  free(old_path_command);
  free(old_path_command_raw);
  free(old_path_apply_command);
  free(old_path_apply_command_raw);
  free(advisory_action_command);
    free(perf_watchlist_command);
    free(status_perf_watchlist_action_command);
  free(status_optimization_target_command);
    free(perf_watchlist_report_path);
  free(perf_watchlist_markdown_path);
  free(stop_file);
  free(stop_command);
  free(resume_command);
  free(state_file);
  free(state_command);
  free(coverage_next_low_cpu_command);
  free(coverage_next_state_file);
  free(coverage_next_state_command);
  free(coverage_next_state_refresh_command);
  free(coverage_next_stop_file);
  free(coverage_next_stop_command);
  free(coverage_next_resume_command);
  free(full_run_command);
  free(full_run_command_raw);
  free(history.data);
  free(worklist.data);
  free(worklist_history.data);
  free(coverage.data);
  free(plan.data);
  string_list_free(&coverage_rows);
  string_list_free(&coverage_seen);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static char *fuzz_all_progress_abs_path(const char *root, const char *path) {
  if (!path || !*path) return strdup("");
  if (path_is_absolute(path)) return strdup(path);
  char *out = NULL;
  (void)asprintf(&out, "%s/%s", root && *root ? root : ".", path);
  return out;
}

static double fuzz_all_progress_report_age_seconds(const char *root,
                                                   const char *path) {
  return fuzz_all_score_report_age_seconds(root, path);
}

static char *fuzz_all_progress_path_in_dir(const char *dir_path,
                                           const char *leaf) {
  char *out = NULL;
  (void)asprintf(&out, "%s/%s",
                 dir_path && *dir_path ? dir_path : "build/fuzz/all",
                 leaf && *leaf ? leaf : "");
  return out;
}

static char *fuzz_all_progress_next_command(const char *next_script,
                                            const char *status_next_command,
                                            const char *run_command,
                                            const char *status_command) {
  if (next_script && *next_script) {
    if (path_is_absolute(next_script) || strncmp(next_script, "./", 2) == 0)
      return strdup(next_script);
    char *out = NULL;
    (void)asprintf(&out, "./%s", next_script);
    return out;
  }
  if (status_next_command && *status_next_command)
    return strdup(status_next_command);
  if (run_command && *run_command) return strdup(run_command);
  if (status_command && *status_command) return strdup(status_command);
  return strdup("");
}

static char *fuzz_all_advisory_action_command(const char *history_path,
                                              const char *worklist_history_path,
                                              const char *worklist_history_md_path) {
  const char *history =
      history_path && *history_path ? history_path : "build/fuzz/all/history.json";
  const char *worklist =
      worklist_history_path && *worklist_history_path ?
          worklist_history_path : "build/fuzz/all/worklist-history.json";
  const char *markdown =
      worklist_history_md_path && *worklist_history_md_path ?
          worklist_history_md_path : "build/fuzz/all/worklist-history.md";
  char *raw = NULL;
  if (asprintf(&raw,
               "./build/nytrix fuzz all worklist --history %s --include-history "
               "--json %s --markdown %s",
               history, worklist, markdown) < 0)
    return strdup("");
  char *out = fuzz_all_low_priority_command_dup(raw);
  free(raw);
  return out;
}

static double fuzz_all_progress_clamp(double v, double lo, double hi) {
  return fuzz_all_score_clamp(v, lo, hi);
}

static const char *fuzz_all_progress_score_label(double score) {
  return fuzz_all_score_label(score);
}

static void fuzz_all_progress_score_note(char *out, size_t out_sz,
                                         bool ready, double blocker_count,
                                         double active_items,
                                         double freshness_penalty,
                                         double advisory_penalty,
                                         double signal_score,
                                         double evidence_cap,
                                         double target_percent) {
  fuzz_all_score_note(out, out_sz, ready, blocker_count, active_items,
                      freshness_penalty, advisory_penalty, signal_score,
                      evidence_cap, target_percent);
}

static void fuzz_all_progress_signal_breakdown(bool ready, double blocker_count,
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
                                               bool ny_bin_exists,
                                               double *gate_penalty,
                                               double *correctness_penalty,
                                               double *perf_penalty,
                                               double *advisory_penalty,
                                               double *env_penalty) {
  fuzz_all_score_signal_breakdown(ready, blocker_count, active_items,
                                  coverage_blocker_gaps, compiler_live,
                                  compiler_missing, known_reproduced,
                                  known_lost, known_baseline, perf_hotspots,
                                  non_reproducing_timeouts, cache_policy_ok,
                                  ny_bin_exists, gate_penalty,
                                  correctness_penalty, perf_penalty,
                                  advisory_penalty, env_penalty);
}

static double fuzz_all_progress_signal_score(bool ready, double blocker_count,
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
                                             bool ny_bin_exists) {
  return fuzz_all_score_signal_score(ready, blocker_count, active_items,
                                     coverage_blocker_gaps, compiler_live,
                                     compiler_missing, known_reproduced,
                                     known_lost, known_baseline, perf_hotspots,
                                     non_reproducing_timeouts, cache_policy_ok,
                                     ny_bin_exists);
}

static double fuzz_all_progress_evidence_cap(double target_percent,
                                             bool campaign_complete) {
  return fuzz_all_score_evidence_cap(target_percent, campaign_complete);
}

static double fuzz_all_progress_thread_years_per_run(double remaining_thread_years,
                                                     double runs_needed) {
  if (remaining_thread_years <= 0.0 || runs_needed <= 0.0) return 0.0;
  return remaining_thread_years / runs_needed;
}

static double fuzz_all_progress_percent_per_run(double thread_years_per_run,
                                                double target_thread_years) {
  if (thread_years_per_run <= 0.0 || target_thread_years <= 0.0) return 0.0;
  return fuzz_all_progress_clamp((thread_years_per_run / target_thread_years) *
                                 100.0,
                                 0.0, 100.0);
}

static double fuzz_all_progress_good_threshold(void) {
  return fuzz_all_score_good_threshold();
}

static double fuzz_all_progress_latest_fresh_hours(void) {
  return fuzz_all_score_latest_fresh_hours();
}

static double fuzz_all_progress_full_pressure_fresh_hours(void) {
  return fuzz_all_score_full_pressure_fresh_hours();
}

static bool fuzz_all_progress_age_fresh(double age_seconds,
                                        double stale_after_hours) {
  return fuzz_all_score_age_fresh(age_seconds, stale_after_hours);
}

static void fuzz_all_advisory_recheck_copy(
    fuzz_all_advisory_recheck_summary_t *dst,
    const fuzz_all_advisory_recheck_summary_t *src) {
  if (!dst || !src) return;
  memcpy(dst, src, sizeof(*dst));
}

static void fuzz_all_advisory_recheck_normalize_command(
    const char *raw, char *out, size_t out_sz) {
  if (!out || out_sz == 0) return;
  out[0] = '\0';
  if (!raw || !*raw) return;
  char root[4096] = {0};
  if (find_nytrix_root(root, sizeof(root)) && root[0]) {
    char prefix[4608];
    snprintf(prefix, sizeof(prefix), "cd '%s' && ", root);
    size_t prefix_len = strlen(prefix);
    if (strncmp(raw, prefix, prefix_len) == 0) {
      snprintf(out, out_sz, "cd . && %s", raw + prefix_len);
      return;
    }
  }
  snprintf(out, out_sz, "%s", raw);
}

static void fuzz_all_advisory_recheck_summary_from_worklist(
    const char *worklist_json, const char *preferred_report,
    fuzz_all_advisory_recheck_summary_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!worklist_json || !*worklist_json) return;
  string_list_t rows = {0};
  if (!collect_rows_from_report_json(worklist_json, &rows)) return;
  fuzz_all_advisory_recheck_summary_t fallback = {0};
  fuzz_all_advisory_recheck_summary_t preferred = {0};
  for (int i = 0; i < rows.count; ++i) {
    const char *row = rows.items[i];
    if (!json_bool_range(row, row + strlen(row),
                         "non_reproducing_afl_timeout", false))
      continue;
    char *raw = json_string_or_empty(row, "first_raw_repro_command");
    if (!raw || !*raw) {
      free(raw);
      raw = json_string_or_empty(row, "raw_repro_command");
    }
    if (!raw || !*raw) {
      free(raw);
      continue;
    }
    fuzz_all_advisory_recheck_summary_t candidate = {0};
    fuzz_all_advisory_recheck_normalize_command(
        raw, candidate.command, sizeof(candidate.command));
    (void)extract_json_number(row, "raw_repro_checked",
                              &candidate.raw_repro_checked);
    (void)extract_json_number(row, "raw_repro_passed",
                              &candidate.raw_repro_passed);
    (void)extract_json_number(row, "raw_repro_timeouts",
                              &candidate.raw_repro_timeouts);
    (void)extract_json_number(row, "raw_repro_unexpected",
                              &candidate.raw_repro_unexpected);
    (void)extract_json_number(row, "saved_hangs",
                              &candidate.saved_hangs);
    (void)extract_json_number(row, "saved_input_count",
                              &candidate.saved_inputs);
    candidate.found = true;
    fuzz_all_advisory_recheck_copy(&fallback, &candidate);
    char *report = json_string_or_empty(row, "report");
    if (preferred_report && *preferred_report && report && *report &&
        strcmp(report, preferred_report) == 0)
      fuzz_all_advisory_recheck_copy(&preferred, &candidate);
    free(report);
    free(raw);
  }
  string_list_free(&rows);
  if (preferred.found)
    fuzz_all_advisory_recheck_copy(out, &preferred);
  else if (fallback.found)
    fuzz_all_advisory_recheck_copy(out, &fallback);
}

static double fuzz_all_progress_runs_to_good(double target_percent,
                                             double target_percent_per_run,
                                             double signal_score,
                                             double stability_score,
                                             bool campaign_complete) {
  const double good_score = fuzz_all_progress_good_threshold();
  if (stability_score >= good_score - 0.005) return 0.0;
  if (campaign_complete || signal_score < good_score || target_percent_per_run <= 0.0)
    return -1.0;
  double needed_target_percent = (good_score - 55.0) / 1.5;
  if (target_percent >= needed_target_percent) return 0.0;
  double raw_runs = (needed_target_percent - target_percent) / target_percent_per_run;
  long runs = (long)raw_runs;
  if ((double)runs + 0.000000001 < raw_runs) ++runs;
  if (runs < 1) runs = 1;
  return (double)runs;
}

static double fuzz_all_progress_runs_to_days(double runs, double runs_per_day) {
  if (runs < 0.0 || runs_per_day <= 0.0) return -1.0;
  return runs / runs_per_day;
}

static int fuzz_all_status_quiet(int argc, char **argv) {
  fflush(stdout);
  int saved_stdout = dup(STDOUT_FILENO);
  int null_fd = open("/dev/null", O_WRONLY);
  if (saved_stdout < 0 || null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0) {
    if (null_fd >= 0) close(null_fd);
    if (saved_stdout >= 0) close(saved_stdout);
    return cmd_public_fuzz_all_status(argc, argv);
  }
  int rc = cmd_public_fuzz_all_status(argc, argv);
  fflush(stdout);
  (void)dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
  close(null_fd);
  return rc;
}

static bool write_fuzz_all_progress_markdown(const char *markdown_path,
                                             const char *root,
                                             const char *progress_json_path,
                                             const char *status_path,
                                             const char *worklist_path,
                                             const char *worklist_markdown_path,
                                             const char *coverage_path,
                                             const char *coverage_markdown_path,
                                             const char *plan_path,
                                             const char *plan_markdown_path,
                                             double status_age_seconds,
                                             double target_percent,
                                             double thread_years,
                                             double target_thread_years,
                                             double remaining_thread_years,
                                             double runs_needed,
                                             double wall_hours_needed,
                                             double thread_hours_needed,
                                             double wall_days_needed,
                                             double evidence_reports,
                                             double evidence_ignored_no_evidence_reports,
                                             double evidence_full_pressure_reports,
                                             double evidence_full_pressure_thread_years,
                                               double evidence_checked_subcases,
                                               double evidence_coverage_ran_lanes,
                                               double evidence_coverage_lanes,
                                               double coverage_blocker_gaps,
                                               double coverage_failed_lanes,
                                             double coverage_skipped_lanes,
                                             double coverage_disabled_lanes,
                                             double coverage_budget_short_lanes,
                                             double coverage_missing_tool_lanes,
                                             double coverage_detail_count,
                                             double coverage_backlog_lanes,
                                             double coverage_detail_rows,
                                             double coverage_queue_count,
                                             double coverage_queue_non_advisory_count,
                                             double coverage_queue_advisory_count,
                                             const char *coverage_queue_lanes,
                                             double coverage_advisory_gaps,
                                             double coverage_reports_considered,
                                             double coverage_campaign_reports_considered,
                                             double coverage_companion_reports_considered,
                                             double coverage_latest_report_advisory_gaps,
                                             double coverage_latest_report_companion_skipped_lanes,
                                             const char *coverage_next_action,
                                             const char *coverage_next_category,
                                             const char *coverage_next_severity,
                                             const char *coverage_next_lane,
                                             const char *coverage_next_reason,
                                             const char *coverage_next_command,
                                             const char *coverage_next_guarded_command,
                                             const char *coverage_next_low_cpu_command,
                                             const char *coverage_next_preview_command,
                                             const char *coverage_next_state_command,
                                             const char *coverage_next_state_refresh_command,
                                             bool coverage_next_state_refresh_required,
                                             const char *coverage_next_state_refresh_reason,
                                             const char *recommended_state_refresh_command,
                                             const char *coverage_next_stop_command,
                                             const char *coverage_next_resume_command,
                                             const fuzz_all_run_state_summary_t
                                                 *coverage_next_run_state,
                                               double thread_years_per_run,
                                             const char *thread_years_per_run_source,
                                             double target_percent_per_run,
                                             double next_run_target_percent,
                                             double next_run_stability_score,
                                             double next_run_stability_delta,
                                             double runs_to_good_stability,
                                             double runs_to_good_days,
                                             bool ready,
                                             double blocker_count,
                                             double active_items,
                                             double historical_attention_reports,
                                             bool target_reached,
                                             bool campaign_complete,
                                             double correctness_findings,
                                             double compiler_findings,
                                             double known_bug_replay_findings,
                                             double perf_hotspots,
                                             double perf_worst_ratio,
                                             const char *perf_worst_case,
                                             double latest_full_pressure_perf_hotspots,
                                             double latest_full_pressure_perf_ratio,
                                             const char *latest_full_pressure_perf_case,
                                             double latest_full_pressure_perf_rows,
                                             bool latest_full_pressure_perf_suite_current,
                                             bool latest_full_pressure_ok,
                                             bool latest_full_pressure_clean,
                                             double latest_full_pressure_failure_count,
                                             bool latest_report_demoted_non_reproducing_afl_timeout,
                                             bool latest_full_pressure_demoted_non_reproducing_afl_timeout,
                                             const char *latest_full_pressure_clean_reason,
                                             double advisory_timeouts,
                                             double effective_advisory_timeouts,
                                             double historical_advisory_timeouts,
                                             bool cache_policy_ok,
                                             bool ny_bin_exists,
                                             bool old_nytrix_test_scratch_absent,
                                             bool old_nytrix_fuzz_absent,
                                             bool old_nytrix_build_cache_absent,
                                             bool active_old_nytrix_writer_present,
                                             double old_path_present_count,
                                             double old_path_moved_count,
                                             double old_path_remaining_count,
                                             double old_path_wait_remaining_seconds,
                                             double stability_score,
                                             double signal_score,
                                             double evidence_cap,
                                             double gate_penalty,
                                             double correctness_penalty,
                                             double perf_penalty,
                                             double advisory_penalty,
                                             double env_penalty,
                                             double freshness_penalty,
                                             const char *stability_label,
                                             const char *stability_note,
                                             const char *completion_eta,
                                             const char *progress_command,
                                             const char *next_command,
                                             const char *next_handoff_command,
                                             const char *preview_command,
                                             const char *stop_file,
                                             const char *stop_command,
                                             const char *resume_command,
                                             const char *state_file,
                                             const char *state_command,
                                             const char *state_refresh_command,
                                             const fuzz_all_run_state_summary_t *run_state,
                                             const char *old_path_command,
                                             const char *old_path_dry_run_command,
                                             const char *old_path_apply_command,
                                             const char *old_path_next_action,
                                             const char *old_path_next_reason,
                                             const char *advisory_action_command,
                                             const char *advisory_recheck_command,
                                             double advisory_recheck_raw_repro_checked,
                                             double advisory_recheck_raw_repro_passed,
                                             double advisory_recheck_raw_repro_timeouts,
                                             double advisory_recheck_raw_repro_unexpected,
                                             const char *perf_watchlist_command,
                                             const char *perf_watchlist_report,
                                             const char *perf_watchlist_markdown,
                                             bool perf_watchlist_artifact_readable,
                                             bool perf_watchlist_artifact_fresh,
                                             double perf_watchlist_artifact_hotspots,
                                             double perf_watchlist_artifact_max_ratio,
                                             double perf_watchlist_artifact_age_seconds,
                                             double perf_watchlist_artifact_stale_after_hours,
                                             const char *perf_watchlist_artifact_max_case,
                                             const char *perf_watchlist_artifact_max_artifact,
                                             const char *perf_watchlist_artifact_max_ny_source,
                                             const char *perf_watchlist_artifact_max_c_source,
                                             bool compiler_std_audit_readable,
                                             const char *compiler_std_audit_report,
                                             const char *compiler_std_audit_markdown,
                                             const char *compiler_std_audit_command,
                                             const char *runtime_surface_state,
                                             const char *crt_surface_state,
                                             double crt_export_coverage_percent,
                                             double crt_unreferenced_count,
                                             double crt_unreferenced_percent,
                                             double crt_unreferenced_family_count,
                                             const char *crt_top_unreferenced_family,
                                             double crt_top_unreferenced_family_count,
                                             const char *crt_next_action,
                                             const char *crt_next_unreferenced_family,
                                             double crt_next_unreferenced_count,
                                             const char *latest_report,
                                             const char *latest_full_pressure_report,
                                             double latest_report_age_seconds,
                                             double latest_full_pressure_report_age_seconds) {
  if (!markdown_path || !*markdown_path) return true;
  (void)stop_file;
  (void)state_file;
  (void)coverage_next_action;
  time_t now = time(NULL);
  struct tm tm_now;
  char stamp[64] = {0};
  if (localtime_r(&now, &tm_now))
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %z", &tm_now);

  char *progress_json_rel = rel_path_dup(root ? root : "",
                                         progress_json_path ?
                                             progress_json_path : "");
  char *quick_jq_command =
      fuzz_all_quick_jq_command(
          progress_json_rel && *progress_json_rel ? progress_json_rel :
                                                    "build/fuzz/all/progress.json");
  char *compact_jq_command =
      fuzz_all_compact_jq_command(
          progress_json_rel && *progress_json_rel ? progress_json_rel :
                                                    "build/fuzz/all/progress.json");
  char *state_rel = rel_path_dup(root ? root : "", state_file ? state_file : "");
  char *state_compact_jq_command =
      fuzz_all_state_compact_jq_command(
          state_rel && *state_rel ? state_rel :
                                    "build/fuzz/all/run-next-state.json");
  char *progress_md_rel = rel_path_dup(root ? root : "",
                                       markdown_path ? markdown_path : "");
  char *status_rel = rel_path_dup(root ? root : "", status_path ? status_path : "");
  char *worklist_rel = rel_path_dup(root ? root : "",
                                    worklist_path ? worklist_path : "");
  char *worklist_md_rel = rel_path_dup(root ? root : "",
                                       worklist_markdown_path ?
                                           worklist_markdown_path : "");
  char *history_worklist_rel =
      path_with_suffix_ext(worklist_rel && *worklist_rel ?
                               worklist_rel : "build/fuzz/all/worklist.json",
                           "-history", ".json");
  char *history_worklist_md_rel =
      path_with_suffix_ext(worklist_rel && *worklist_rel ?
                               worklist_rel : "build/fuzz/all/worklist.json",
                           "-history", ".md");
  char *coverage_rel = rel_path_dup(root ? root : "",
                                    coverage_path ? coverage_path : "");
  char *coverage_md_rel = rel_path_dup(root ? root : "",
                                       coverage_markdown_path ?
                                           coverage_markdown_path : "");
  char *plan_rel = rel_path_dup(root ? root : "", plan_path ? plan_path : "");
  char *plan_md_rel = rel_path_dup(root ? root : "",
                                   plan_markdown_path ? plan_markdown_path : "");
  str_buf_t md = {0};
  (void)sb_append(&md, "# Nytrix Progress\n\n");
  if (stamp[0]) {
    (void)sb_append(&md, "Generated: ");
    md_append_code(&md, stamp);
    (void)sb_append(&md, "\n\n");
  }
  (void)sb_append(&md, "## TLDR\n\n");
  (void)sb_appendf(&md, "- Progress: %.4f%%; remaining %.4f%%.\n",
                   target_percent,
                   fuzz_all_campaign_remaining_percent(target_percent));
  (void)sb_appendf(&md,
                   "- Confidence: campaign evidence %.4f%%; lang score %.2f%% `%s`.\n",
                   target_percent, stability_score,
                   stability_label ? stability_label : "");
  (void)sb_appendf(&md,
                   "- Gate: %s; blockers %.0f; active %.0f; complete `%s`; cache `%s`; ny `%s`.\n",
                   ready ? "`ready`" : "`blocked`", blocker_count,
                   active_items, campaign_complete ? "true" : "false",
                   cache_policy_ok ? "ok" : "bad",
                   ny_bin_exists ? "ok" : "missing");
  (void)sb_append(&md, "- Completion: ");
  md_append_code(&md, fuzz_all_campaign_state(ready, target_reached,
                                              campaign_complete));
  (void)sb_append(&md, "; reason ");
  md_append_code(&md, fuzz_all_campaign_incomplete_reason(ready, blocker_count,
                                                          active_items,
                                                          target_reached,
                                                          campaign_complete));
  (void)sb_appendf(&md, "; target reached `%s`.\n",
                   target_reached ? "true" : "false");
  fuzz_all_recommendation_t recommendation;
  fuzz_all_recommendation_make(
      &recommendation, ready,
      run_state && fuzz_all_state_phase_live(run_state->phase),
      freshness_penalty <= 0.0, cache_policy_ok, ny_bin_exists,
      active_old_nytrix_writer_present, blocker_count, active_items,
      correctness_findings, perf_hotspots, runs_to_good_stability, runs_needed,
      target_reached, campaign_complete,
      next_handoff_command && *next_handoff_command ?
          next_handoff_command : next_command,
      state_command,
      old_path_command, advisory_action_command, progress_command,
      coverage_next_command, coverage_next_guarded_command,
      coverage_next_low_cpu_command,
      coverage_next_preview_command);
  bool recommendation_missing_evidence =
      strcmp(recommendation.action, "run-missing-evidence") == 0;
  bool campaign_state_refresh_required =
      run_state && fuzz_all_run_state_refresh_required(run_state);
  const char *campaign_state_refresh_command =
      campaign_state_refresh_required ? state_refresh_command : "";
  if (campaign_state_refresh_required &&
      !recommendation_missing_evidence &&
      recommendation.preview_command[0])
    campaign_state_refresh_command = recommendation.preview_command;
  (void)sb_append(&md, "- Recommended: ");
  md_append_code(&md, recommendation.action);
  if (recommendation.repeat_mode[0]) {
    (void)sb_append(&md, "; repeat ");
    md_append_code(&md, recommendation.repeat_mode);
    (void)sb_appendf(&md, " x%.0f", recommendation.repeat_count);
  }
  (void)sb_append(&md, "; reason ");
  md_append_code(&md, recommendation.reason);
  if (recommendation.command[0]) {
    (void)sb_append(&md, "; command ");
    md_append_code(&md, recommendation.command);
  }
  (void)sb_append(&md, ".\n");
  if (recommendation.preview_command[0]) {
    (void)sb_append(&md, "- Preview recommended: ");
    md_append_code(&md, recommendation.preview_command);
    (void)sb_append(&md, ".\n");
  }
  append_fuzz_all_handoff_guard_markdown(&md);
  (void)sb_appendf(&md,
                   "- Lang score: %.2f%% `%s`; good >= %.2f%%; gap %.2f%%; signal %.2f%%; evidence cap %.2f%%.\n",
                   stability_score, stability_label ? stability_label : "",
                   fuzz_all_progress_good_threshold(),
                   fuzz_all_language_good_gap_percent(stability_score),
                   signal_score, evidence_cap);
  if (stability_note && *stability_note) {
    (void)sb_append(&md, "- Score note: ");
    md_append_inline(&md, stability_note);
    (void)sb_append(&md, ".\n");
  }
  if (status_age_seconds >= 0.0) {
    (void)sb_appendf(&md, "- Source: status age %.1f min",
                     status_age_seconds / 60.0);
    if (latest_report_age_seconds >= 0.0) {
      double stale_after = fuzz_all_progress_latest_fresh_hours();
      bool fresh = fuzz_all_progress_age_fresh(latest_report_age_seconds,
                                               stale_after);
      (void)sb_appendf(&md, "; latest %.1f h/%.0fh %s",
                       latest_report_age_seconds / 3600.0, stale_after,
                       fresh ? "ok" : "stale");
    }
    if (latest_full_pressure_report_age_seconds >= 0.0) {
      double stale_after = fuzz_all_progress_full_pressure_fresh_hours();
      bool fresh =
          fuzz_all_progress_age_fresh(latest_full_pressure_report_age_seconds,
                                      stale_after);
      (void)sb_appendf(&md, "; full-pressure %.1f h/%.0fh %s",
                       latest_full_pressure_report_age_seconds / 3600.0,
                       stale_after, fresh ? "ok" : "stale");
    }
    (void)sb_append(&md, ".\n");
  }
  (void)sb_appendf(&md,
                   "- Full-pressure gate: effective clean `%s`; raw ok `%s`; reason ",
                   latest_full_pressure_clean ? "true" : "false",
                   latest_full_pressure_ok ? "true" : "false");
  md_append_code(&md,
                 latest_full_pressure_clean_reason &&
                         *latest_full_pressure_clean_reason ?
                     latest_full_pressure_clean_reason : "unknown");
  (void)sb_appendf(&md, "; failures %.0f",
                   latest_full_pressure_failure_count);
  if (latest_report_demoted_non_reproducing_afl_timeout ||
      latest_full_pressure_demoted_non_reproducing_afl_timeout)
    (void)sb_appendf(&md, "; demoted latest `%s`, full-pressure `%s`",
                     latest_report_demoted_non_reproducing_afl_timeout ?
                         "true" : "false",
                     latest_full_pressure_demoted_non_reproducing_afl_timeout ?
                         "true" : "false");
  (void)sb_append(&md, ".\n");
  append_fuzz_all_compiler_std_audit_markdown(
      &md, compiler_std_audit_readable, compiler_std_audit_markdown,
      compiler_std_audit_report, compiler_std_audit_command,
      runtime_surface_state, crt_surface_state, crt_export_coverage_percent,
      crt_unreferenced_count, crt_unreferenced_percent,
      crt_unreferenced_family_count, crt_top_unreferenced_family,
      crt_top_unreferenced_family_count, crt_next_action,
      crt_next_unreferenced_family, crt_next_unreferenced_count);
  char progress_md_freshness_action_command[4096] = {0};
  if (freshness_penalty > 0.0 && next_command && *next_command)
    fuzz_all_freshness_action_command(
        progress_md_freshness_action_command,
        sizeof(progress_md_freshness_action_command),
        next_handoff_command && *next_handoff_command ?
            next_handoff_command : next_command);
  if (progress_md_freshness_action_command[0]) {
    (void)sb_append(&md, "- Freshen evidence: ");
    md_append_code(&md, progress_md_freshness_action_command);
    (void)sb_append(&md, ".\n");
  }
  (void)sb_appendf(
      &md,
      "- Evidence: %.0f reports; %.0f full-pressure / %.6f thread-years; %.0f checked rows; coverage %.0f/%.0f lanes (%.2f%%; not-run %.0f, actionable %.0f).\n",
      evidence_reports, evidence_full_pressure_reports,
      evidence_full_pressure_thread_years,
      evidence_checked_subcases, evidence_coverage_ran_lanes,
      evidence_coverage_lanes,
      fuzz_all_ratio_percent(evidence_coverage_ran_lanes,
                             evidence_coverage_lanes),
      fuzz_all_not_run_lanes(evidence_coverage_ran_lanes,
                             evidence_coverage_lanes),
      coverage_backlog_lanes > 0.0 ? coverage_backlog_lanes :
          coverage_detail_count);
  if (evidence_ignored_no_evidence_reports > 0.0) {
    (void)sb_appendf(&md,
                     "- Evidence hygiene: ignored %.0f no-evidence attempt%s.\n",
                     evidence_ignored_no_evidence_reports,
                     evidence_ignored_no_evidence_reports == 1.0 ? "" : "s");
  }
  (void)sb_append(&md, "- Coverage: ");
  md_append_code(&md, fuzz_all_coverage_state(evidence_coverage_lanes,
                                              evidence_coverage_ran_lanes,
                                              coverage_blocker_gaps,
                                              coverage_failed_lanes));
  (void)sb_appendf(&md,
                   "; %.0f/%.0f lanes; not-run %.0f "
                   "(actionable backlog %.0f; disabled %.0f; "
                   "budget-short %.0f; missing-tool %.0f); skipped %.0f; "
                   "details %.0f; blockers %.0f; advisory %.0f.\n",
                   evidence_coverage_ran_lanes, evidence_coverage_lanes,
                   fuzz_all_not_run_lanes(evidence_coverage_ran_lanes,
                                          evidence_coverage_lanes),
                   coverage_backlog_lanes > 0.0 ? coverage_backlog_lanes :
                       coverage_detail_count,
                   coverage_disabled_lanes, coverage_budget_short_lanes,
                   coverage_missing_tool_lanes, coverage_skipped_lanes,
                   coverage_detail_rows, coverage_blocker_gaps,
                   coverage_advisory_gaps);
  if (coverage_queue_lanes && *coverage_queue_lanes) {
    (void)sb_append(&md, "- Coverage queue: ");
    md_append_inline(&md, coverage_queue_lanes);
    (void)sb_appendf(&md, "; total %.0f; primary %.0f; advisory %.0f.\n",
                     coverage_queue_count,
                     coverage_queue_non_advisory_count,
                     coverage_queue_advisory_count);
  }
  if (coverage_next_lane && *coverage_next_lane &&
      coverage_next_command && *coverage_next_command) {
    (void)sb_append(&md, "- Coverage next: ");
    md_append_code(&md, coverage_next_lane);
    (void)sb_append(&md, " ");
    md_append_code(&md, coverage_next_severity && *coverage_next_severity ?
                       coverage_next_severity : "medium");
    if (coverage_next_category && *coverage_next_category) {
      (void)sb_append(&md, " ");
      md_append_code(&md, coverage_next_category);
    }
    if (coverage_next_reason && *coverage_next_reason) {
      (void)sb_append(&md, "; reason ");
      md_append_code(&md, coverage_next_reason);
    }
    (void)sb_append(&md, "; command ");
    md_append_code(&md, coverage_next_command);
    if (coverage_next_guarded_command && *coverage_next_guarded_command) {
      (void)sb_append(&md, "; guarded ");
      md_append_code(&md, coverage_next_guarded_command);
    }
    if (coverage_next_low_cpu_command && *coverage_next_low_cpu_command) {
      (void)sb_append(&md, "; low-cpu ");
      md_append_code(&md, coverage_next_low_cpu_command);
    }
    if (coverage_next_preview_command && *coverage_next_preview_command) {
      (void)sb_append(&md, "; preview ");
      md_append_code(&md, coverage_next_preview_command);
    }
    if (coverage_next_state_command && *coverage_next_state_command) {
      (void)sb_append(&md, "; state ");
      md_append_code(&md, coverage_next_state_command);
    }
    if (coverage_next_state_refresh_command &&
        *coverage_next_state_refresh_command) {
      (void)sb_append(&md, "; refresh-state ");
      md_append_code(&md, coverage_next_state_refresh_command);
    }
    if (coverage_next_stop_command && *coverage_next_stop_command) {
      (void)sb_append(&md, "; pause ");
      md_append_code(&md, coverage_next_stop_command);
      if (coverage_next_resume_command && *coverage_next_resume_command) {
        (void)sb_append(&md, "; resume ");
        md_append_code(&md, coverage_next_resume_command);
      }
    }
    (void)sb_append(&md, ".\n");
    append_fuzz_all_state_summary_markdown(
        &md, recommendation_missing_evidence ?
                 "Recommended state" : "Coverage next state",
        coverage_next_state_command,
        coverage_next_state_refresh_command,
        coverage_next_run_state);
    if (coverage_next_state_refresh_required) {
      (void)sb_append(&md, recommendation_missing_evidence ?
                               "- Recommended state refresh: " :
                               "- Coverage next state refresh: ");
      md_append_code(&md, coverage_next_state_refresh_reason &&
                             *coverage_next_state_refresh_reason ?
                         coverage_next_state_refresh_reason : "stale");
      if (recommended_state_refresh_command &&
          *recommended_state_refresh_command) {
        (void)sb_append(&md, "; command ");
        md_append_code(&md, recommended_state_refresh_command);
      }
      (void)sb_append(&md, ".\n");
    }
  }
  if (coverage_reports_considered > 0.0) {
    (void)sb_appendf(
        &md,
        "- Coverage evidence: %.0f reports (%.0f campaign + %.0f companion); latest advisory %.0f, companion skips %.0f.\n",
        coverage_reports_considered, coverage_campaign_reports_considered,
        coverage_companion_reports_considered,
        coverage_latest_report_advisory_gaps,
        coverage_latest_report_companion_skipped_lanes);
  }
  if (historical_attention_reports > 0.0) {
    (void)sb_appendf(&md,
                     "- History: %.0f hidden attention reports; drill-down in ",
                     historical_attention_reports);
    md_append_code(&md, history_worklist_md_rel && *history_worklist_md_rel ?
                        history_worklist_md_rel :
                        "build/fuzz/all/worklist-history.md");
    (void)sb_append(&md, ".\n");
  }
  (void)sb_appendf(&md,
                   "- Paths: old test scratch `%s`; old fuzz `%s`; old build cache `%s`; active old writer `%s`; present `%.0f`; moved `%.0f`; remaining `%.0f`.\n",
                   old_nytrix_test_scratch_absent ? "absent" : "present",
                   old_nytrix_fuzz_absent ? "absent" : "present",
                   old_nytrix_build_cache_absent ? "absent" : "present",
                   active_old_nytrix_writer_present ? "present" : "none",
                   old_path_present_count, old_path_moved_count,
                   old_path_remaining_count);
  (void)sb_append(&md,
                  "- Old-path action: ");
  md_append_code(&md, old_path_next_action && *old_path_next_action ?
                     old_path_next_action : "none");
  (void)sb_append(&md, "; reason ");
  md_append_code(&md, old_path_next_reason && *old_path_next_reason ?
                     old_path_next_reason :
                     fuzz_all_old_path_next_reason(
                         old_path_remaining_count,
                         active_old_nytrix_writer_present));
  if (old_path_wait_remaining_seconds >= 0.0)
    (void)sb_appendf(&md, "; wait estimate `%.0fs`",
                     old_path_wait_remaining_seconds);
  if (old_path_apply_command && *old_path_apply_command) {
    (void)sb_append(&md, "; apply ");
    md_append_code(&md, old_path_apply_command);
  }
  (void)sb_append(&md, "; dry-run ");
  md_append_code(&md, old_path_dry_run_command && *old_path_dry_run_command ?
                     old_path_dry_run_command :
                     (old_path_command && *old_path_command ?
                          old_path_command : NYTRIX_OLD_PATH_DRY_RUN_COMMAND));
  (void)sb_append(&md, ".\n");
  if (progress_command && *progress_command) {
    (void)sb_append(&md, "- Refresh: ");
    md_append_code(&md, progress_command);
    (void)sb_append(&md, ".\n");
  }
  if (!recommendation_missing_evidence &&
      freshness_penalty <= 0.0 && preview_command && *preview_command &&
      strcmp(preview_command, recommendation.preview_command) != 0) {
    (void)sb_append(&md, "- Preview target: ");
    md_append_code(&md, preview_command);
    (void)sb_append(&md, ".\n");
  }
  if (!recommendation_missing_evidence && state_command && *state_command) {
    char *state_inspect_command = fuzz_all_state_compact_jq_command(
        state_file && *state_file ? state_file : NULL);
    (void)sb_append(&md, "- State: ");
    if (run_state && run_state->readable) {
      md_append_code(&md, run_state->phase[0] ? run_state->phase : "unknown");
      if (run_state->event[0]) {
        (void)sb_append(&md, "/");
        md_append_code(&md, run_state->event);
      }
      (void)sb_appendf(&md, "; age %.0fs; cycle %.0f/%.0f",
                       run_state->age_seconds, run_state->cycle,
                       run_state->cycles);
      if (run_state->child_pid > 0.0)
        (void)sb_appendf(&md, "; child %.0f %s", run_state->child_pid,
                         run_state->child_alive ? "alive" : "dead");
      if (run_state->heartbeat_count > 0.0)
        (void)sb_appendf(&md, "; heartbeats %.0f",
                         run_state->heartbeat_count);
      if (fuzz_all_state_phase_live(run_state->phase))
        (void)sb_appendf(&md, "; stale after %.0fs",
                         fuzz_all_state_stale_after_seconds(
                             run_state->heartbeat_s));
      (void)sb_appendf(&md, "; %s",
                       fuzz_all_state_phase_live(run_state->phase) ? "live"
                                                                   : "not-live");
      (void)sb_appendf(&md, "; %s", run_state->fresh ? "fresh" : "stale");
      if (!run_state->fresh)
        (void)sb_appendf(&md, " (%s)",
            fuzz_all_state_stale_reason_values(run_state->readable,
                                               run_state->fresh,
                                               run_state->phase,
                                               run_state->age_seconds,
                                               run_state->heartbeat_s,
                                               run_state->child_pid,
                                               run_state->child_alive));
      (void)sb_append(&md, "; inspect ");
    } else {
      (void)sb_append(&md, "not-readable; not-live; stale (missing); inspect ");
    }
    md_append_code(&md, state_inspect_command && *state_inspect_command ?
                        state_inspect_command : state_command);
    free(state_inspect_command);
      if (freshness_penalty <= 0.0 &&
          campaign_state_refresh_command &&
          *campaign_state_refresh_command) {
        (void)sb_append(&md, "; refresh-state ");
        md_append_code(&md, campaign_state_refresh_command);
      }
    (void)sb_append(&md, ".\n");
  }
  if (!recommendation_missing_evidence && stop_command && *stop_command) {
    (void)sb_append(&md, "- Pause: ");
    md_append_code(&md, stop_command);
    if (resume_command && *resume_command) {
      (void)sb_append(&md, "; resume ");
      md_append_code(&md, resume_command);
    }
    (void)sb_append(&md, ".\n");
  }
  double perf_watchlist_open =
      fuzz_all_perf_watchlist_effective_open(
          perf_hotspots, perf_worst_ratio, perf_watchlist_artifact_readable,
          perf_watchlist_artifact_fresh, perf_watchlist_artifact_hotspots);
  const char *perf_watchlist_state =
      fuzz_all_perf_watchlist_state(perf_watchlist_open,
                                    perf_watchlist_artifact_readable,
                                    perf_watchlist_artifact_fresh,
                                    perf_watchlist_artifact_hotspots);
  const char *perf_watchlist_action =
      fuzz_all_perf_watchlist_action(perf_watchlist_state);
  char *perf_watchlist_action_command =
      fuzz_all_perf_watchlist_action_command(
          perf_watchlist_state, perf_watchlist_command,
          perf_watchlist_markdown, perf_watchlist_report);
  const char *optimization_reason =
      fuzz_all_optimization_reason(perf_watchlist_state);
  const char *optimization_case =
      fuzz_all_optimization_case(perf_watchlist_artifact_readable,
                                 perf_watchlist_artifact_fresh,
                                 perf_watchlist_artifact_hotspots,
                                 perf_watchlist_artifact_max_case,
                                 perf_watchlist_artifact_max_ratio,
                                 perf_watchlist_open, perf_worst_case);
  double optimization_ratio =
      fuzz_all_optimization_ratio(perf_watchlist_artifact_readable,
                                  perf_watchlist_artifact_fresh,
                                  perf_watchlist_artifact_hotspots,
                                  perf_watchlist_artifact_max_ratio,
                                  perf_watchlist_open, perf_worst_ratio);
  const char *optimization_artifact =
      fuzz_all_optimization_artifact_path(
          perf_watchlist_artifact_readable, perf_watchlist_artifact_fresh,
          perf_watchlist_artifact_hotspots, perf_watchlist_artifact_max_ratio,
          perf_watchlist_artifact_max_artifact);
  const char *optimization_ny_source =
      fuzz_all_optimization_artifact_path(
          perf_watchlist_artifact_readable, perf_watchlist_artifact_fresh,
          perf_watchlist_artifact_hotspots, perf_watchlist_artifact_max_ratio,
          perf_watchlist_artifact_max_ny_source);
  const char *optimization_c_source =
      fuzz_all_optimization_artifact_path(
          perf_watchlist_artifact_readable, perf_watchlist_artifact_fresh,
          perf_watchlist_artifact_hotspots, perf_watchlist_artifact_max_ratio,
          perf_watchlist_artifact_max_c_source);
  char *optimization_target_command =
      fuzz_all_optimization_target_command(optimization_ny_source,
                                           optimization_c_source,
                                           optimization_artifact);
  (void)sb_appendf(&md,
                   "- Findings: correctness %.0f (compiler %.0f, known-bug replay %.0f); C-vs-Ny perf %.0f; watch %.0f >= %.2fx `%s`",
                   correctness_findings, compiler_findings,
                   known_bug_replay_findings, perf_hotspots,
                   perf_watchlist_open,
                   fuzz_all_perf_watchlist_threshold(),
                   perf_watchlist_state);
  if (perf_worst_ratio > 0.0) {
    (void)sb_appendf(&md, " (worst %.4fx, %.2f%% slower than C",
                     perf_worst_ratio,
                     fuzz_all_perf_slowdown_percent(perf_worst_ratio));
    if (perf_worst_case && *perf_worst_case) {
      (void)sb_append(&md, " at ");
      md_append_code(&md, perf_worst_case);
    }
    (void)sb_append(&md, ")");
  }
  (void)sb_appendf(&md, "; current advisory timeouts %.0f",
                   advisory_timeouts);
  if (historical_advisory_timeouts > 0.0)
    (void)sb_appendf(&md, "; historical demoted AFL rows %.0f",
                     historical_advisory_timeouts);
  (void)sb_append(&md, ".\n");
  if ((advisory_timeouts > 0.0 || historical_advisory_timeouts > 0.0) &&
      advisory_action_command && *advisory_action_command) {
    (void)sb_append(&md, "- Advisory action: ");
    md_append_code(&md, advisory_action_command);
    (void)sb_append(&md, ".\n");
  }
  if ((advisory_timeouts > 0.0 || historical_advisory_timeouts > 0.0) &&
      advisory_recheck_command && *advisory_recheck_command) {
    (void)sb_append(&md, "- Advisory recheck: ");
    md_append_code(
        &md,
        fuzz_all_advisory_recheck_state(
            true, advisory_recheck_raw_repro_checked,
            advisory_recheck_raw_repro_passed,
            advisory_recheck_raw_repro_timeouts,
            advisory_recheck_raw_repro_unexpected,
            advisory_recheck_command));
    if (advisory_recheck_raw_repro_checked > 0.0)
      (void)sb_appendf(&md,
                       "; raw replay %.0f/%.0f passed, timeouts %.0f, unexpected %.0f; command ",
                       advisory_recheck_raw_repro_passed,
                       advisory_recheck_raw_repro_checked,
                       advisory_recheck_raw_repro_timeouts,
                       advisory_recheck_raw_repro_unexpected);
    else
      (void)sb_append(&md, "; command ");
    md_append_code(&md, advisory_recheck_command);
    (void)sb_append(&md, ".\n");
  }
  if (perf_watchlist_open > 0.0 &&
      perf_watchlist_command && *perf_watchlist_command) {
    (void)sb_append(&md, "- Perf watchlist: ");
    md_append_code(&md, perf_watchlist_command);
    (void)sb_append(&md, ".\n");
  }
  if (perf_watchlist_artifact_readable) {
    (void)sb_appendf(&md,
                     "- Perf watchlist artifact: state `%s`, %.0f rows, %s, age %.1fh/%.0fh, worst ",
                     perf_watchlist_state,
                     perf_watchlist_artifact_hotspots,
                     perf_watchlist_artifact_fresh ? "fresh" : "stale",
                     perf_watchlist_artifact_age_seconds >= 0.0 ?
                         perf_watchlist_artifact_age_seconds / 3600.0 : -1.0,
                     perf_watchlist_artifact_stale_after_hours);
    if (perf_watchlist_artifact_max_case &&
        *perf_watchlist_artifact_max_case)
      md_append_code(&md, perf_watchlist_artifact_max_case);
    else
      md_append_code(&md, "unknown");
    (void)sb_appendf(&md, " %.4fx (%.2f%% slower than C); report ",
                     perf_watchlist_artifact_max_ratio,
                     fuzz_all_perf_slowdown_percent(
                         perf_watchlist_artifact_max_ratio));
    md_append_code(&md, perf_watchlist_markdown &&
                            *perf_watchlist_markdown ?
                            perf_watchlist_markdown :
                            (perf_watchlist_report ?
                                 perf_watchlist_report : ""));
    (void)sb_append(&md, ".\n");
  }
  fuzz_all_append_full_pressure_perf_markdown(
      &md, latest_full_pressure_perf_hotspots,
      latest_full_pressure_perf_ratio, latest_full_pressure_perf_case,
      latest_full_pressure_perf_rows, perf_real_case_count(),
      latest_full_pressure_perf_suite_current);
  if (perf_watchlist_action_command && *perf_watchlist_action_command) {
    (void)sb_append(&md, "- Optimization action: ");
    md_append_code(&md, perf_watchlist_action);
    (void)sb_append(&md, "; reason ");
    md_append_code(&md, optimization_reason);
    if (optimization_ratio > 0.0) {
      (void)sb_append(&md, "; target ");
      md_append_code(&md, optimization_case && *optimization_case ?
                              optimization_case : "unknown");
      (void)sb_appendf(&md, " %.4fx (%.2f%% slower than C)",
                       optimization_ratio,
                       fuzz_all_perf_slowdown_percent(optimization_ratio));
    }
    (void)sb_append(&md, "; command ");
    md_append_code(&md, perf_watchlist_action_command);
    (void)sb_append(&md, ".\n");
  }
  if ((optimization_artifact && *optimization_artifact) ||
      (optimization_ny_source && *optimization_ny_source) ||
      (optimization_c_source && *optimization_c_source)) {
    (void)sb_append(&md, "- Optimization files:");
    bool wrote_path = false;
    md_append_path_segment(&md, &wrote_path, "ny", optimization_ny_source);
    md_append_path_segment(&md, &wrote_path, "c", optimization_c_source);
    md_append_path_segment(&md, &wrote_path, "artifact", optimization_artifact);
    (void)sb_append(&md, ".\n");
  }
  if (optimization_target_command && *optimization_target_command) {
    (void)sb_append(&md, "- Optimization inspect: ");
    md_append_code(&md, optimization_target_command);
    (void)sb_append(&md, ".\n");
  }
  (void)sb_append(&md, "- Advisory: ");
  md_append_code(&md, fuzz_all_advisory_state(advisory_timeouts,
                                              historical_advisory_timeouts));
  (void)sb_appendf(&md,
                   "; effective %s/%.0f; current timeouts %.0f; historical %.0f; penalty %.2f.\n",
                   fuzz_all_advisory_penalty_state(
                       effective_advisory_timeouts),
                   effective_advisory_timeouts,
                   advisory_timeouts, historical_advisory_timeouts,
                   advisory_penalty);
  (void)sb_appendf(&md,
                   "- Penalties: gate %.2f; correctness %.2f; perf %.2f; advisory %.2f; env %.2f; freshness %.2f.\n",
                   gate_penalty, correctness_penalty, perf_penalty,
                   advisory_penalty, env_penalty, freshness_penalty);
  (void)sb_appendf(&md,
                   "- Thread-years: %.6f/%.6f; %.6f remaining.\n",
                   thread_years, target_thread_years,
                   remaining_thread_years);
  (void)sb_appendf(&md, "- Runs: %.0f; wall-days %.2f",
                   runs_needed, wall_days_needed);
  if (completion_eta && *completion_eta) {
    (void)sb_append(&md, "; ETA ");
    md_append_code(&md, completion_eta);
  }
  (void)sb_append(&md, ".\n");
  if (wall_hours_needed > 0.0 || thread_hours_needed > 0.0) {
    (void)sb_appendf(&md,
                     "- Budget: %.2f wall-hours; %.2f thread-hours.\n",
                     wall_hours_needed, thread_hours_needed);
  }
  if (target_percent_per_run > 0.0) {
    (void)sb_appendf(&md,
                     "- Next clean run: +%.6f thread-years",
                     thread_years_per_run);
    if (thread_years_per_run_source && *thread_years_per_run_source)
      (void)sb_appendf(&md, " (%s)", thread_years_per_run_source);
    (void)sb_appendf(&md,
                     " / +%.4f%% campaign to %.4f%%; lang score %.2f%% (%+.2f)",
                     target_percent_per_run,
                     next_run_target_percent, next_run_stability_score,
                     next_run_stability_delta);
    if (runs_to_good_stability > 0.0) {
      (void)sb_appendf(&md, "; good lang score in %.0f runs",
                       runs_to_good_stability);
      if (runs_to_good_days >= 0.0)
        (void)sb_appendf(&md, " / %.2f days", runs_to_good_days);
    }
    else if (runs_to_good_stability == 0.0)
      (void)sb_append(&md, "; good reached");
    else
      (void)sb_append(&md, "; good needs penalties/progress");
    (void)sb_append(&md, ".\n");
  }

  (void)sb_append(&md, "\n## Next\n\n```bash\n");
  string_list_t next_lines = {0};
  if (recommendation.preview_command[0]) {
    md_append_unique_shell_line(&md, &next_lines,
                                recommendation.preview_command);
  }
  if (recommendation.command[0] &&
      (!next_command || !*next_command ||
       strcmp(recommendation.command, next_command) != 0)) {
    md_append_unique_shell_line(&md, &next_lines, recommendation.command);
  }
  if (!recommendation_missing_evidence &&
      !recommendation.repeat_mode[0] &&
      next_command && *next_command &&
      (strcmp(recommendation.action, "freshen-evidence") != 0 ||
       !recommendation.command[0] ||
       strcmp(recommendation.command, next_command) == 0)) {
    md_append_unique_shell_line(&md, &next_lines, next_command);
    if (freshness_penalty <= 0.0 && runs_to_good_stability > 1.0) {
      char repeat_good[8192];
      snprintf(repeat_good, sizeof(repeat_good),
               "NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 "
               "NYTRIX_RUN_REPEAT=good %s",
               next_handoff_command && *next_handoff_command ?
                   next_handoff_command : next_command);
      md_append_unique_shell_line(&md, &next_lines, repeat_good);
    }
    if (freshness_penalty <= 0.0 &&
        runs_needed > 1.0 && runs_to_good_stability >= 0.0) {
      char repeat_target[8192];
      snprintf(repeat_target, sizeof(repeat_target),
               "NYTRIX_LOW_PRIORITY=1 NYTRIX_RUN_NICE=10 "
               "NYTRIX_RUN_REPEAT=target %s",
               next_handoff_command && *next_handoff_command ?
                   next_handoff_command : next_command);
      md_append_unique_shell_line(&md, &next_lines, repeat_target);
    }
  } else if (!recommendation_missing_evidence &&
             !recommendation.repeat_mode[0] &&
             (strcmp(recommendation.action, "freshen-evidence") != 0 ||
              !recommendation.command[0])) {
    md_append_unique_shell_line(&md, &next_lines, "unknown");
  }
  string_list_free(&next_lines);
  (void)sb_append(&md, "```\n\n");

  bool has_coverage_controls =
      (coverage_next_state_command && *coverage_next_state_command) ||
      (coverage_next_state_refresh_command &&
       *coverage_next_state_refresh_command) ||
      (coverage_next_stop_command && *coverage_next_stop_command) ||
      (coverage_next_resume_command && *coverage_next_resume_command);
    bool has_campaign_controls =
        !recommendation_missing_evidence &&
        ((freshness_penalty <= 0.0 &&
          campaign_state_refresh_command &&
          *campaign_state_refresh_command) ||
         (state_command && *state_command) ||
         (stop_command && *stop_command) ||
         (resume_command && *resume_command));
  if ((quick_jq_command && *quick_jq_command) ||
      (compact_jq_command && *compact_jq_command) ||
      has_coverage_controls || has_campaign_controls) {
    (void)sb_append(&md, "## Controls\n\n```bash\n");
    if (quick_jq_command && *quick_jq_command)
      (void)sb_appendf(&md, "%s\n", quick_jq_command);
    if (compact_jq_command && *compact_jq_command)
      (void)sb_appendf(&md, "%s\n", compact_jq_command);
    if (has_coverage_controls) {
      if (coverage_next_state_refresh_command &&
          *coverage_next_state_refresh_command)
        (void)sb_appendf(&md, "%s\n",
                         coverage_next_state_refresh_command);
      if (coverage_next_state_command && *coverage_next_state_command)
        (void)sb_appendf(&md, "%s\n", coverage_next_state_command);
      if (coverage_next_stop_command && *coverage_next_stop_command)
        (void)sb_appendf(&md, "%s\n", coverage_next_stop_command);
      if (coverage_next_resume_command && *coverage_next_resume_command)
        (void)sb_appendf(&md, "%s\n", coverage_next_resume_command);
    }
  if (has_campaign_controls) {
        if (freshness_penalty <= 0.0 &&
            campaign_state_refresh_command &&
            *campaign_state_refresh_command)
          (void)sb_appendf(&md, "%s\n", campaign_state_refresh_command);
      if (state_compact_jq_command && *state_compact_jq_command)
        (void)sb_appendf(&md, "%s\n", state_compact_jq_command);
      if (state_command && *state_command &&
          (!state_compact_jq_command || !*state_compact_jq_command))
        (void)sb_appendf(&md, "%s\n", state_command);
      if (stop_command && *stop_command)
        (void)sb_appendf(&md, "%s\n", stop_command);
      if (resume_command && *resume_command)
        (void)sb_appendf(&md, "%s\n", resume_command);
    }
    (void)sb_append(&md, "```\n\n");
  }

  (void)sb_append(&md, "## Reports\n\n");
  if ((progress_md_rel && *progress_md_rel) ||
      (progress_json_rel && *progress_json_rel)) {
    (void)sb_append(&md, "- Progress: ");
    if (progress_md_rel && *progress_md_rel)
      md_append_code(&md, progress_md_rel);
    if (progress_json_rel && *progress_json_rel) {
      if (progress_md_rel && *progress_md_rel) (void)sb_append(&md, " / ");
      md_append_code(&md, progress_json_rel);
    }
    (void)sb_append(&md, "\n");
  }
  (void)sb_append(&md, "- Status: ");
  md_append_code(&md, status_rel ? status_rel : "");
  (void)sb_append(&md, "\n");
  if (worklist_md_rel && *worklist_md_rel) {
    (void)sb_append(&md, "- Worklist: ");
    md_append_code(&md, worklist_md_rel);
    if (worklist_rel && *worklist_rel) {
      (void)sb_append(&md, " / ");
      md_append_code(&md, worklist_rel);
    }
    (void)sb_append(&md, "\n");
  }
  if (historical_attention_reports > 0.0 &&
      history_worklist_md_rel && *history_worklist_md_rel) {
    (void)sb_append(&md, "- Historical worklist: ");
    md_append_code(&md, history_worklist_md_rel);
    if (history_worklist_rel && *history_worklist_rel) {
      (void)sb_append(&md, " / ");
      md_append_code(&md, history_worklist_rel);
    }
    (void)sb_append(&md, "\n");
  }
  if (coverage_md_rel && *coverage_md_rel) {
    (void)sb_append(&md, "- Coverage: ");
    md_append_code(&md, coverage_md_rel);
    if (coverage_rel && *coverage_rel) {
      (void)sb_append(&md, " / ");
      md_append_code(&md, coverage_rel);
    }
    (void)sb_append(&md, "\n");
  }
  if (plan_md_rel && *plan_md_rel) {
    (void)sb_append(&md, "- Plan: ");
    md_append_code(&md, plan_md_rel);
    if (plan_rel && *plan_rel) {
      (void)sb_append(&md, " / ");
      md_append_code(&md, plan_rel);
    }
    (void)sb_append(&md, "\n");
  }
  if ((compiler_std_audit_markdown && *compiler_std_audit_markdown) ||
      (compiler_std_audit_report && *compiler_std_audit_report)) {
    (void)sb_append(&md, "- Compiler std audit: ");
    md_append_code(&md, compiler_std_audit_markdown &&
                            *compiler_std_audit_markdown ?
                            compiler_std_audit_markdown :
                            NYTRIX_COMPILER_STD_AUDIT_MARKDOWN);
    if (compiler_std_audit_report && *compiler_std_audit_report) {
      (void)sb_append(&md, " / ");
      md_append_code(&md, compiler_std_audit_report);
    }
    (void)sb_append(&md, "\n");
  }
  if (latest_report && *latest_report) {
    (void)sb_append(&md, "- Latest: ");
    md_append_code(&md, latest_report);
    (void)sb_append(&md, "\n");
  }
  if (latest_full_pressure_report && *latest_full_pressure_report &&
      (!latest_report || strcmp(latest_full_pressure_report, latest_report) != 0)) {
    (void)sb_append(&md, "- Full-pressure: ");
    md_append_code(&md, latest_full_pressure_report);
    (void)sb_append(&md, "\n");
  }

  bool ok = md.data && write_file_text(markdown_path, md.data);
  free(status_rel);
  free(quick_jq_command);
  free(compact_jq_command);
  free(state_rel);
  free(state_compact_jq_command);
  free(progress_json_rel);
  free(progress_md_rel);
  free(worklist_rel);
  free(worklist_md_rel);
  free(history_worklist_rel);
  free(history_worklist_md_rel);
  free(coverage_rel);
  free(coverage_md_rel);
  free(plan_rel);
  free(plan_md_rel);
  free(perf_watchlist_action_command);
  free(optimization_target_command);
  free(md.data);
  return ok;
}

static int cmd_public_fuzz_all_progress(int argc, char **argv) {
  char root[4096];
  if (!find_nytrix_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nytrix-root-not-found\"}\n");
    return 2;
  }

  const char *dir_arg = value_after_equals(argc, argv, 4, "--dir", "");
  if (!dir_arg || !*dir_arg)
    dir_arg = value_after_equals(argc, argv, 4, "--history-dir", "");
  const char *status_arg = value_after_equals(argc, argv, 4, "--status", "");
  const char *history_arg = value_after_equals(argc, argv, 4, "--history", "");
  const char *worklist_arg = value_after_equals(argc, argv, 4, "--worklist", "");
  const char *coverage_arg = value_after_equals(argc, argv, 4, "--coverage", "");
  const char *plan_arg = value_after_equals(argc, argv, 4, "--plan", "");
  const char *target_arg = value_after_equals(argc, argv, 4, "--target-thread-years", "");
  if (!target_arg || !*target_arg)
    target_arg = value_after_equals(argc, argv, 4, "--target-years", "");
  if (!target_arg || !*target_arg)
    target_arg = value_after_equals(argc, argv, 4, "--target", "10");
  const char *hours_arg = value_after_equals(argc, argv, 4, "--hours-per-run", "");
  if (!hours_arg || !*hours_arg)
    hours_arg = value_after_equals(argc, argv, 4, "--run-hours", "");
  if (!hours_arg || !*hours_arg)
    hours_arg = value_after_equals(argc, argv, 4, "--hours", "8");
  const char *threads_arg = value_after_equals(argc, argv, 4, "--threads",
                                               NYTRIX_DEFAULT_FUZZ_THREADS);
  const char *profile_arg = value_after_equals(argc, argv, 4, "--profile", "insane");
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *progress_markdown_arg =
      value_after_equals(argc, argv, 4, "--markdown", "");
  if (!progress_markdown_arg || !*progress_markdown_arg)
    progress_markdown_arg = value_after_equals(argc, argv, 4, "--md", "");
  bool refresh = has_flag_after(argc, argv, 4, "--refresh") ||
                 has_flag_after(argc, argv, 4, "--update");
  bool strict = has_flag_after(argc, argv, 4, "--strict");
  bool allow_incomplete_coverage =
      has_flag_after(argc, argv, 4, "--allow-incomplete-coverage") ||
      has_flag_after(argc, argv, 4, "--allow-smoke-coverage-gaps");
  bool allow_full_pressure_remediation =
      has_flag_after(argc, argv, 4, "--allow-full-pressure-remediation") ||
      has_flag_after(argc, argv, 4, "--allow-stale-full-pressure");

  char *status_path = status_arg && *status_arg
                          ? fuzz_all_progress_abs_path(root, status_arg)
                          : NULL;
  char *status_parent = status_path ?
      path_parent_dup(status_path, "build/fuzz/all") : NULL;
  char *dir_path = NULL;
  if (dir_arg && *dir_arg)
    dir_path = fuzz_all_progress_abs_path(root, dir_arg);
  else if (status_parent && *status_parent)
    dir_path = strdup(status_parent);
  else
    dir_path = fuzz_all_progress_abs_path(root, "build/fuzz/all");
  nytrix_redirect_nytrix_output_dir(&dir_path, root, "fuzz-all-progress");
  if (!status_path)
    status_path = fuzz_all_progress_path_in_dir(dir_path, "status.json");
  free(status_parent);
  char *status_markdown_path = path_with_suffix_ext(status_path, "", ".md");
  char *progress_markdown_path =
      progress_markdown_arg && *progress_markdown_arg
          ? fuzz_all_progress_abs_path(root, progress_markdown_arg)
          : NULL;
  char *history_path = history_arg && *history_arg
                           ? fuzz_all_progress_abs_path(root, history_arg)
                           : fuzz_all_progress_path_in_dir(dir_path, "history.json");
  char *worklist_path = worklist_arg && *worklist_arg
                            ? fuzz_all_progress_abs_path(root, worklist_arg)
                            : fuzz_all_progress_path_in_dir(dir_path, "worklist.json");
  char *worklist_markdown_path = path_with_suffix_ext(worklist_path, "", ".md");
  char *worklist_history_path =
      path_with_suffix_ext(worklist_path, "-history", ".json");
  char *worklist_history_markdown_path =
      path_with_suffix_ext(worklist_path, "-history", ".md");
  char *coverage_path = coverage_arg && *coverage_arg
                            ? fuzz_all_progress_abs_path(root, coverage_arg)
                            : fuzz_all_progress_path_in_dir(dir_path, "coverage.json");
  char *coverage_markdown_path = path_with_suffix_ext(coverage_path, "", ".md");
  char *plan_path = plan_arg && *plan_arg
                        ? fuzz_all_progress_abs_path(root, plan_arg)
                        : fuzz_all_progress_path_in_dir(dir_path, "plan.json");
  char *plan_markdown_path = path_with_suffix_ext(plan_path, "", ".md");
  char *dir_cmd_path = rel_path_dup(root, dir_path ? dir_path : "build/fuzz/all");
  char *status_cmd_path = rel_path_dup(root, status_path ? status_path : "");
  char *history_cmd_path = rel_path_dup(root, history_path ? history_path : "");
  char *worklist_cmd_path = rel_path_dup(root, worklist_path ? worklist_path : "");
  char *coverage_cmd_path = rel_path_dup(root, coverage_path ? coverage_path : "");
  char *plan_cmd_path = rel_path_dup(root, plan_path ? plan_path : "");
  char *progress_json_cmd_path =
      rel_path_dup(root, json_path && *json_path ? json_path : "");
  char *progress_md_cmd_path =
      rel_path_dup(root, progress_markdown_path ? progress_markdown_path : "");
  const char *dir_cmd = dir_cmd_path && *dir_cmd_path ?
      dir_cmd_path : "build/fuzz/all";
  const char *status_cmd = status_cmd_path && *status_cmd_path ?
      status_cmd_path : "build/fuzz/all/status.json";
  const char *history_cmd = history_cmd_path && *history_cmd_path ?
      history_cmd_path : "build/fuzz/all/history.json";
  const char *worklist_cmd = worklist_cmd_path && *worklist_cmd_path ?
      worklist_cmd_path : "build/fuzz/all/worklist.json";
  const char *coverage_cmd = coverage_cmd_path && *coverage_cmd_path ?
      coverage_cmd_path : "build/fuzz/all/coverage.json";
  const char *plan_cmd = plan_cmd_path && *plan_cmd_path ?
      plan_cmd_path : "build/fuzz/all/plan.json";
  const char *progress_json_cmd = progress_json_cmd_path && *progress_json_cmd_path ?
      progress_json_cmd_path : "";
  const char *progress_md_cmd = progress_md_cmd_path && *progress_md_cmd_path ?
      progress_md_cmd_path : "";
  char *progress_command = NULL;
  char *progress_command_raw = NULL;
  char *old_path_dry_run_command = NULL;
  char *old_path_dry_run_command_raw = NULL;
  char *old_path_apply_command = NULL;
  char *old_path_apply_command_raw = NULL;
  (void)asprintf(&progress_command_raw,
                 "./build/nytrix fuzz all progress --refresh%s%s%s --dir %s "
                 "--status %s --history %s --worklist %s --coverage %s "
                 "--plan %s --target-thread-years %s --hours %s "
                 "--threads %s --profile %s%s%s%s%s",
                 strict ? " --strict" : "",
                 allow_incomplete_coverage ?
                     " --allow-incomplete-coverage" : "",
                 allow_full_pressure_remediation ?
                     " --allow-full-pressure-remediation" : "",
                 dir_cmd, status_cmd, history_cmd, worklist_cmd,
                 coverage_cmd, plan_cmd,
                 target_arg && *target_arg ? target_arg : "10",
                 hours_arg && *hours_arg ? hours_arg : "8",
                 threads_arg && *threads_arg ? threads_arg :
                     NYTRIX_DEFAULT_FUZZ_THREADS,
                 profile_arg && *profile_arg ? profile_arg : "insane",
                 progress_json_cmd[0] ? " --json " : "",
                 progress_json_cmd[0] ? progress_json_cmd : "",
                 progress_md_cmd[0] ? " --markdown " : "",
                 progress_md_cmd[0] ? progress_md_cmd : "");
  progress_command =
      fuzz_all_low_priority_command_dup(progress_command_raw);
  (void)asprintf(&old_path_dry_run_command_raw,
                 "./build/nytrix fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json %s/old-paths.json --markdown %s/old-paths.md",
                 dir_cmd, dir_cmd);
  old_path_dry_run_command =
      fuzz_all_low_priority_command_dup(old_path_dry_run_command_raw);
  (void)asprintf(&old_path_apply_command_raw,
                 "./build/nytrix fuzz all old-paths --apply --wait-writers-s 300 --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json %s/old-paths.json --markdown %s/old-paths.md",
                 dir_cmd, dir_cmd);
  old_path_apply_command =
      fuzz_all_low_priority_command_dup(old_path_apply_command_raw);

  int status_rc = 0;
  if (refresh) {
    char *status_argv[40];
    int a = 0;
    status_argv[a++] = g_self_path;
    status_argv[a++] = "fuzz";
    status_argv[a++] = "all";
    status_argv[a++] = "status";
    status_argv[a++] = "--refresh";
    if (strict) status_argv[a++] = "--strict";
    if (allow_incomplete_coverage)
      status_argv[a++] = "--allow-incomplete-coverage";
    if (allow_full_pressure_remediation)
      status_argv[a++] = "--allow-full-pressure-remediation";
    status_argv[a++] = "--dir";
    status_argv[a++] = dir_path;
    status_argv[a++] = "--history";
    status_argv[a++] = history_path;
    status_argv[a++] = "--worklist";
    status_argv[a++] = worklist_path;
    status_argv[a++] = "--coverage";
    status_argv[a++] = coverage_path;
    status_argv[a++] = "--plan";
    status_argv[a++] = plan_path;
    status_argv[a++] = "--target-thread-years";
    status_argv[a++] = (char *)target_arg;
    status_argv[a++] = "--hours";
    status_argv[a++] = (char *)hours_arg;
    status_argv[a++] = "--threads";
    status_argv[a++] = (char *)threads_arg;
    status_argv[a++] = "--profile";
    status_argv[a++] = (char *)profile_arg;
    status_argv[a++] = "--json";
    status_argv[a++] = status_path;
    status_argv[a++] = "--markdown";
    status_argv[a++] = status_markdown_path;
    status_argv[a] = NULL;
    status_rc = fuzz_all_status_quiet(a, status_argv);
  }

  file_buf_t status = {0};
  string_list_t rows = {0}, failures = {0};
  if (refresh && status_rc != 0) {
    char refresh_error[128];
    snprintf(refresh_error, sizeof(refresh_error),
             "status refresh failed rc=%d", status_rc);
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-progress",
                                                  refresh_error,
                                                  status_path ? status_path : ""));
  }
  bool readable = status_path && *status_path && read_file(status_path, &status) && status.data;
  if (!readable) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-progress",
                                                  "status report not readable",
                                                  status_path ? status_path : ""));
  }

  double target_percent = 0.0;
  double thread_years = 0.0;
  double target_thread_years = 0.0;
  double remaining_thread_years = 0.0;
  double runs_needed = 0.0;
  double runs_per_day = 0.0;
  double thread_years_per_day = 0.0;
  double campaign_plan_wall_hours =
      atof(hours_arg && *hours_arg ? hours_arg : "8");
  double campaign_first_report_epoch = 0.0;
  double campaign_latest_report_epoch = 0.0;
  double campaign_calendar_span_days = 0.0;
  double campaign_calendar_age_days = 0.0;
  double wall_hours_needed = 0.0;
  double thread_hours_needed = 0.0;
  double wall_days_needed = 0.0;
  double evidence_reports = 0.0;
  double evidence_ignored_no_evidence_reports = 0.0;
  double evidence_full_pressure_reports = 0.0;
  double evidence_full_pressure_thread_years = 0.0;
  double evidence_checked_subcases = 0.0;
  double evidence_coverage_ran_lanes = 0.0;
  double evidence_coverage_lanes = 0.0;
  double evidence_coverage_skipped_lanes = 0.0;
  double status_age_seconds = -1.0;
  double blocker_count = 0.0;
  double active_items = 0.0;
  double coverage_blocker_gaps = 0.0;
  double coverage_advisory_gaps = 0.0;
  double coverage_failed_lanes = 0.0;
  double coverage_disabled_lanes = 0.0;
  double coverage_budget_short_lanes = 0.0;
  double coverage_missing_tool_lanes = 0.0;
  double coverage_detail_count = 0.0;
  double coverage_backlog_lanes = 0.0;
  double coverage_detail_rows = 0.0;
  double coverage_queue_count = 0.0;
  double coverage_queue_non_advisory_count = 0.0;
  double coverage_queue_advisory_count = 0.0;
  double coverage_reports_considered = 0.0;
  double coverage_campaign_reports_considered = 0.0;
  double coverage_companion_reports_considered = 0.0;
  double coverage_latest_report_advisory_gaps = 0.0;
  double coverage_latest_report_companion_skipped_lanes = 0.0;
  double historical_attention_reports = 0.0;
  double compiler_live = 0.0;
  double compiler_missing = 0.0;
  double known_reproduced = 0.0;
  double known_lost = 0.0;
  double known_baseline = 0.0;
  double perf_hotspots = 0.0;
  double perf_worst_ratio = 0.0;
  double latest_full_pressure_perf_hotspots = 0.0;
  double latest_full_pressure_perf_worst_ratio = 0.0;
  double latest_full_pressure_perf_rows = 0.0;
  double latest_full_pressure_failure_count = 0.0;
  double non_reproducing_timeouts = 0.0;
  double historical_non_reproducing_timeouts = 0.0;
  double advisory_recheck_raw_repro_checked = 0.0;
  double advisory_recheck_raw_repro_passed = 0.0;
  double advisory_recheck_raw_repro_timeouts = 0.0;
  double advisory_recheck_raw_repro_unexpected = 0.0;
  bool ready = false;
  bool target_reached = false;
  bool campaign_complete = false;
  bool cache_policy_ok = true;
  bool old_path_cache_policy_ok = true;
  bool ny_bin_exists = true;
  bool old_nytrix_test_scratch_absent = true;
  bool old_nytrix_fuzz_absent = true;
  bool old_nytrix_build_cache_absent = true;
  bool active_old_nytrix_writer_present = false;
  bool latest_full_pressure_perf_suite_current = false;
  bool latest_full_pressure_ok = false;
  bool latest_full_pressure_clean = false;
  bool latest_report_demoted_non_reproducing_afl_timeout = false;
  bool latest_full_pressure_demoted_non_reproducing_afl_timeout = false;
  char *completion_eta = strdup("");
  char *campaign_plan_threads =
      strdup(threads_arg && *threads_arg ? threads_arg :
                 NYTRIX_DEFAULT_FUZZ_THREADS);
  char *campaign_first_report = strdup("");
  char *next_script = strdup("");
  char *status_next_handoff_command = strdup("");
  char *status_next_command = strdup("");
  char *run_command = strdup("");
  char *status_command = strdup("");
  char *stop_file = fuzz_all_stop_file_path(dir_cmd);
  char *stop_command = fuzz_all_stop_command(stop_file);
  char *resume_command = fuzz_all_resume_command(stop_file);
  char *state_file = fuzz_all_state_file_path(dir_cmd);
  char *state_command = fuzz_all_state_command(state_file);
  char *state_refresh_command = strdup("");
  char *tmp_dir = strdup("build/cache/tmp");
  char *scratch_root = strdup("build/cache/scratch");
  char *xdg_cache_home = strdup("build/cache/xdg");
  char *nytrix_cache_dir = strdup("build/cache/nytrix");
  char *old_path_command =
      strdup(old_path_dry_run_command && *old_path_dry_run_command ?
                 old_path_dry_run_command : NYTRIX_OLD_PATH_DRY_RUN_COMMAND);
  char *old_path_next_action = strdup("none");
  char *old_path_next_reason = strdup("");
  char *old_path_report = NULL;
  char *old_path_markdown = NULL;
  char *advisory_action_command = strdup("");
  char *advisory_recheck_command = strdup("");
  char *perf_watchlist_command = strdup("");
  char *perf_watchlist_report =
      fuzz_all_perf_watchlist_report_path(dir_cmd, false);
  char *perf_watchlist_markdown =
      fuzz_all_perf_watchlist_report_path(dir_cmd, true);
  bool perf_watchlist_artifact_readable = false;
  bool perf_watchlist_artifact_fresh = false;
  double perf_watchlist_artifact_hotspots = 0.0;
  double perf_watchlist_artifact_max_ratio = 0.0;
  double perf_watchlist_artifact_age_seconds = -1.0;
    double perf_watchlist_artifact_stale_after_hours =
        fuzz_all_perf_watchlist_artifact_fresh_hours();
    char *perf_watchlist_artifact_max_case = strdup("");
  char *perf_watchlist_artifact_max_artifact = strdup("");
  char *perf_watchlist_artifact_max_ny_source = strdup("");
  char *perf_watchlist_artifact_max_c_source = strdup("");
  char *latest_report = strdup("");
  char *latest_full_pressure_report = strdup("");
  char *latest_full_pressure_clean_reason = strdup("");
  char *perf_worst_case = strdup("");
  char *latest_full_pressure_perf_case = strdup("");
  char *coverage_next_action = strdup("none");
  char *coverage_next_category = strdup("");
  char *coverage_next_severity = strdup("");
  char *coverage_next_lane = strdup("");
  char *coverage_next_reason = strdup("");
  char *coverage_next_command = strdup("");
  char *coverage_next_guarded_command = strdup("");
  char *coverage_next_low_cpu_command = strdup("");
  char *coverage_next_preview_command = strdup("");
  char *coverage_next_state_file = strdup("");
  char *coverage_next_state_command = strdup("");
  char *coverage_next_state_refresh_command = strdup("");
  bool coverage_next_state_refresh_required = false;
  char *coverage_next_state_refresh_reason = strdup("");
  char *recommended_state_refresh_command = strdup("");
  char *coverage_next_stop_file = strdup("");
  char *coverage_next_stop_command = strdup("");
  char *coverage_next_resume_command = strdup("");
  char *coverage_queue_lanes = strdup("");
  char *coverage_queue_json = strdup("[]");
  bool compiler_std_audit_readable = false;
  char *compiler_std_audit_report = strdup(NYTRIX_COMPILER_STD_AUDIT_JSON);
  char *compiler_std_audit_markdown = strdup(NYTRIX_COMPILER_STD_AUDIT_MARKDOWN);
  char *compiler_std_audit_command = strdup(NYTRIX_COMPILER_STD_AUDIT_COMMAND);
  char *runtime_surface_state = strdup("unknown");
  char *crt_surface_state = strdup("unknown");
  char *crt_top_unreferenced_family = strdup("");
  char *crt_unreferenced_families = strdup("[]");
  char *crt_next_action = strdup("none");
  char *crt_next_reason = strdup("");
  char *crt_next_unreferenced_family = strdup("");
  char *crt_next_unreferenced_exports = strdup("[]");
  char *crt_next_definition_file = strdup("");
  char *crt_next_definition_locations = strdup("[]");
  char *crt_next_inspect_command = strdup("");
  double runtime_exports = 0.0;
  double direct_runtime_refs = 0.0;
  double runtime_export_coverage_percent = 0.0;
  double runtime_unreferenced_percent = 0.0;
  double runtime_unreferenced_count = 0.0;
  double runtime_wrapper_gap_count = 0.0;
  double crt_runtime_exports = 0.0;
  double crt_direct_refs = 0.0;
  double crt_export_coverage_percent = 0.0;
  double crt_unreferenced_percent = 0.0;
  double crt_unreferenced_count = 0.0;
  double crt_wrapper_gap_count = 0.0;
  double crt_unreferenced_family_count = 0.0;
  double crt_top_unreferenced_family_count = 0.0;
  double crt_next_unreferenced_count = 0.0;
  double old_path_artifact_leak_count = 0.0;
  double old_path_artifact_moved_count = 0.0;
  double old_path_artifact_remaining_count = 0.0;
  double old_path_present_count = 0.0;
  double old_path_moved_count = 0.0;
  double old_path_remaining_count = 0.0;
  double old_path_wait_remaining_seconds = -1.0;
  (void)asprintf(&old_path_report, "%s/old-paths.json", dir_cmd);
  (void)asprintf(&old_path_markdown, "%s/old-paths.md", dir_cmd);
  if (!old_path_report)
    old_path_report = strdup("build/fuzz/all/old-paths.json");
  if (!old_path_markdown)
    old_path_markdown = strdup("build/fuzz/all/old-paths.md");

  if (readable) {
    struct stat status_stat;
    if (status_path && stat(status_path, &status_stat) == 0) {
      double age = difftime(time(NULL), status_stat.st_mtime);
      status_age_seconds = age < 0.0 ? 0.0 : age;
    }
    if (!summary_number_from_report(status.data, "target_percent", &target_percent))
      (void)string_list_push_take(&failures,
                                  make_fuzz_failure(root, "fuzz-all-progress",
                                                    "status missing target_percent",
                                                    status_path));
    if (!summary_number_from_report(status.data, "thread_years", &thread_years))
      (void)string_list_push_take(&failures,
                                  make_fuzz_failure(root, "fuzz-all-progress",
                                                    "status missing thread_years",
                                                    status_path));
    if (!summary_number_from_report(status.data, "target_thread_years",
                                    &target_thread_years))
      (void)string_list_push_take(&failures,
                                  make_fuzz_failure(root, "fuzz-all-progress",
                                                    "status missing target_thread_years",
                                                    status_path));
    if (!summary_number_from_report(status.data, "remaining_thread_years",
                                    &remaining_thread_years))
      (void)string_list_push_take(&failures,
                                  make_fuzz_failure(root, "fuzz-all-progress",
                                                    "status missing remaining_thread_years",
                                                    status_path));
    (void)summary_number_from_report(status.data, "runs_needed", &runs_needed);
       (void)summary_number_from_report(status.data, "runs_per_day",
                                        &runs_per_day);
    (void)summary_number_from_report(status.data, "thread_years_per_day",
                                     &thread_years_per_day);
    (void)summary_number_from_report(status.data,
                                     "campaign_plan_wall_hours",
                                     &campaign_plan_wall_hours);
    char *status_campaign_plan_threads =
        summary_string_from_report(status.data, "campaign_plan_threads");
    if (status_campaign_plan_threads && *status_campaign_plan_threads) {
      free(campaign_plan_threads);
      campaign_plan_threads = status_campaign_plan_threads;
    } else {
      free(status_campaign_plan_threads);
    }
    (void)summary_number_from_report(status.data, "campaign_first_report_epoch",
                                     &campaign_first_report_epoch);
    (void)summary_number_from_report(status.data, "campaign_latest_report_epoch",
                                     &campaign_latest_report_epoch);
    (void)summary_number_from_report(status.data, "campaign_calendar_span_days",
                                     &campaign_calendar_span_days);
    (void)summary_number_from_report(status.data, "campaign_calendar_age_days",
                                     &campaign_calendar_age_days);
    free(campaign_first_report);
    campaign_first_report =
        summary_string_from_report(status.data, "campaign_first_report");
    if (!campaign_first_report) campaign_first_report = strdup("");
       (void)summary_number_from_report(status.data, "wall_hours_needed",
                                     &wall_hours_needed);
    (void)summary_number_from_report(status.data, "thread_hours_needed",
                                     &thread_hours_needed);
    (void)summary_number_from_report(status.data, "wall_days_needed",
                                     &wall_days_needed);
    (void)summary_number_from_report(status.data, "reports",
                                     &evidence_reports);
    if (!summary_number_from_report(status.data, "ignored_no_evidence_reports",
                                    &evidence_ignored_no_evidence_reports))
      (void)summary_number_from_report(
          status.data, "evidence_ignored_no_evidence_reports",
          &evidence_ignored_no_evidence_reports);
    (void)summary_number_from_report(status.data, "full_pressure_reports",
                                     &evidence_full_pressure_reports);
    (void)summary_number_from_report(status.data,
                                     "full_pressure_thread_years",
                                     &evidence_full_pressure_thread_years);
    (void)summary_number_from_report(status.data, "checked_subcases",
                                     &evidence_checked_subcases);
    (void)summary_number_from_report(status.data, "coverage_ran_lanes",
                                     &evidence_coverage_ran_lanes);
    (void)summary_number_from_report(status.data, "coverage_lanes",
                                     &evidence_coverage_lanes);
    (void)summary_number_from_report(status.data, "coverage_skipped_lanes",
                                     &evidence_coverage_skipped_lanes);
    (void)summary_number_from_report(status.data, "blocker_count", &blocker_count);
    (void)summary_number_from_report(status.data, "active_items", &active_items);
    (void)summary_number_from_report(status.data,
                                     "historical_attention_reports",
                                     &historical_attention_reports);
    (void)summary_number_from_report(status.data, "coverage_blocker_gaps",
                                     &coverage_blocker_gaps);
    (void)summary_number_from_report(status.data, "coverage_advisory_gaps",
                                     &coverage_advisory_gaps);
    (void)summary_number_from_report(status.data, "coverage_failed_lanes",
                                     &coverage_failed_lanes);
    (void)summary_number_from_report(status.data, "coverage_disabled_lanes",
                                     &coverage_disabled_lanes);
    (void)summary_number_from_report(status.data,
                                     "coverage_budget_short_lanes",
                                     &coverage_budget_short_lanes);
    (void)summary_number_from_report(status.data,
                                     "coverage_missing_tool_lanes",
                                     &coverage_missing_tool_lanes);
    (void)summary_number_from_report(status.data,
                                     "coverage_detail_count",
                                     &coverage_detail_count);
    if (!summary_number_from_report(status.data,
                                    "coverage_backlog_lanes",
                                    &coverage_backlog_lanes))
      coverage_backlog_lanes = coverage_detail_count;
    (void)summary_number_from_report(status.data,
                                     "coverage_detail_rows",
                                     &coverage_detail_rows);
    (void)summary_number_from_report(status.data,
                                     "coverage_queue_count",
                                     &coverage_queue_count);
    (void)summary_number_from_report(status.data,
                                     "coverage_queue_non_advisory_count",
                                     &coverage_queue_non_advisory_count);
    (void)summary_number_from_report(status.data,
                                     "coverage_queue_advisory_count",
                                     &coverage_queue_advisory_count);
    free(coverage_queue_lanes);
    coverage_queue_lanes =
        summary_string_from_report(status.data, "coverage_queue_lanes");
    if (!coverage_queue_lanes)
      coverage_queue_lanes = strdup("");
    char *qjson = summary_array_from_report(status.data, "coverage_queue");
    if (qjson) {
      free(coverage_queue_json);
      coverage_queue_json = qjson;
    }
    (void)summary_number_from_report(status.data,
                                     "coverage_reports_considered",
                                     &coverage_reports_considered);
    (void)summary_number_from_report(status.data,
                                     "coverage_campaign_reports_considered",
                                     &coverage_campaign_reports_considered);
    (void)summary_number_from_report(status.data,
                                     "coverage_companion_reports_considered",
                                     &coverage_companion_reports_considered);
    (void)summary_number_from_report(
        status.data, "coverage_latest_report_advisory_gaps",
        &coverage_latest_report_advisory_gaps);
    (void)summary_number_from_report(
        status.data, "coverage_latest_report_companion_skipped_lanes",
        &coverage_latest_report_companion_skipped_lanes);
    (void)summary_bool_from_report(status.data,
                                   "compiler_std_audit_readable",
                                   &compiler_std_audit_readable);
    free(compiler_std_audit_report);
    compiler_std_audit_report =
        summary_string_from_report(status.data, "compiler_std_audit_report");
    if (!compiler_std_audit_report || !*compiler_std_audit_report) {
      free(compiler_std_audit_report);
      compiler_std_audit_report = strdup(NYTRIX_COMPILER_STD_AUDIT_JSON);
    }
    free(compiler_std_audit_markdown);
    compiler_std_audit_markdown =
        summary_string_from_report(status.data, "compiler_std_audit_markdown");
    if (!compiler_std_audit_markdown || !*compiler_std_audit_markdown) {
      free(compiler_std_audit_markdown);
      compiler_std_audit_markdown = strdup(NYTRIX_COMPILER_STD_AUDIT_MARKDOWN);
    }
    free(compiler_std_audit_command);
    compiler_std_audit_command =
        summary_string_from_report(status.data, "compiler_std_audit_command");
    if (!compiler_std_audit_command || !*compiler_std_audit_command) {
      free(compiler_std_audit_command);
      compiler_std_audit_command = strdup(NYTRIX_COMPILER_STD_AUDIT_COMMAND);
    }
    free(runtime_surface_state);
    runtime_surface_state =
        summary_string_from_report(status.data, "runtime_surface_state");
    if (!runtime_surface_state || !*runtime_surface_state) {
      free(runtime_surface_state);
      runtime_surface_state = strdup("unknown");
    }
    free(crt_surface_state);
    crt_surface_state =
        summary_string_from_report(status.data, "crt_surface_state");
    if (!crt_surface_state || !*crt_surface_state) {
      free(crt_surface_state);
      crt_surface_state = strdup("unknown");
    }
    free(crt_top_unreferenced_family);
    crt_top_unreferenced_family =
        summary_string_from_report(status.data, "crt_top_unreferenced_family");
    if (!crt_top_unreferenced_family)
      crt_top_unreferenced_family = strdup("");
    free(crt_unreferenced_families);
    crt_unreferenced_families =
        summary_array_from_report(status.data, "crt_unreferenced_families");
    if (!crt_unreferenced_families ||
        crt_unreferenced_families[0] != '[') {
      free(crt_unreferenced_families);
      crt_unreferenced_families = strdup("[]");
    }
    free(crt_next_action);
    crt_next_action =
        summary_string_from_report(status.data, "crt_next_action");
    if (!crt_next_action || !*crt_next_action) {
      free(crt_next_action);
      crt_next_action = strdup("none");
    }
    free(crt_next_reason);
    crt_next_reason =
        summary_string_from_report(status.data, "crt_next_reason");
    if (!crt_next_reason)
      crt_next_reason = strdup("");
    free(crt_next_unreferenced_family);
    crt_next_unreferenced_family = summary_string_from_report(
        status.data, "crt_next_unreferenced_family");
    if (!crt_next_unreferenced_family)
      crt_next_unreferenced_family = strdup("");
    free(crt_next_unreferenced_exports);
    crt_next_unreferenced_exports = summary_array_from_report(
        status.data, "crt_next_unreferenced_exports");
    if (!crt_next_unreferenced_exports ||
        crt_next_unreferenced_exports[0] != '[') {
      free(crt_next_unreferenced_exports);
      crt_next_unreferenced_exports = strdup("[]");
    }
    free(crt_next_definition_file);
    crt_next_definition_file =
        summary_string_from_report(status.data, "crt_next_definition_file");
    if (!crt_next_definition_file)
      crt_next_definition_file = strdup("");
    free(crt_next_definition_locations);
    crt_next_definition_locations = summary_array_from_report(
        status.data, "crt_next_definition_locations");
    if (!crt_next_definition_locations ||
        crt_next_definition_locations[0] != '[') {
      free(crt_next_definition_locations);
      crt_next_definition_locations = strdup("[]");
    }
    free(crt_next_inspect_command);
    crt_next_inspect_command =
        summary_string_from_report(status.data, "crt_next_inspect_command");
    if (!crt_next_inspect_command)
      crt_next_inspect_command = strdup("");
    if (!summary_number_from_report(status.data, "runtime_exports",
                                    &runtime_exports))
      (void)summary_number_from_report(status.data, "runtime_coverage_total",
                                       &runtime_exports);
    if (!summary_number_from_report(status.data, "direct_runtime_refs",
                                    &direct_runtime_refs))
      (void)summary_number_from_report(status.data, "runtime_coverage_done",
                                       &direct_runtime_refs);
    (void)summary_number_from_report(
        status.data, "runtime_export_coverage_percent",
        &runtime_export_coverage_percent);
    (void)summary_number_from_report(status.data,
                                     "runtime_unreferenced_percent",
                                     &runtime_unreferenced_percent);
    (void)summary_number_from_report(status.data,
                                     "runtime_unreferenced_count",
                                     &runtime_unreferenced_count);
    (void)summary_number_from_report(status.data,
                                     "runtime_wrapper_gap_count",
                                     &runtime_wrapper_gap_count);
    if (!summary_number_from_report(status.data, "crt_runtime_exports",
                                    &crt_runtime_exports))
      (void)summary_number_from_report(status.data, "crt_coverage_total",
                                       &crt_runtime_exports);
    if (!summary_number_from_report(status.data, "crt_direct_refs",
                                    &crt_direct_refs))
      (void)summary_number_from_report(status.data, "crt_coverage_done",
                                       &crt_direct_refs);
    (void)summary_number_from_report(status.data,
                                     "crt_export_coverage_percent",
                                     &crt_export_coverage_percent);
    (void)summary_number_from_report(status.data,
                                     "crt_unreferenced_percent",
                                     &crt_unreferenced_percent);
    (void)summary_number_from_report(status.data, "crt_unreferenced_count",
                                     &crt_unreferenced_count);
    (void)summary_number_from_report(status.data, "crt_wrapper_gap_count",
                                     &crt_wrapper_gap_count);
    (void)summary_number_from_report(status.data,
                                     "crt_unreferenced_family_count",
                                     &crt_unreferenced_family_count);
    (void)summary_number_from_report(
        status.data, "crt_top_unreferenced_family_count",
        &crt_top_unreferenced_family_count);
    (void)summary_number_from_report(
        status.data, "crt_next_unreferenced_count",
        &crt_next_unreferenced_count);
    (void)summary_number_from_report(status.data, "latest_compiler_finding_live",
                                     &compiler_live);
    (void)summary_number_from_report(status.data, "latest_compiler_finding_missing",
                                     &compiler_missing);
    (void)summary_number_from_report(status.data, "latest_known_bug_reproduced",
                                     &known_reproduced);
    (void)summary_number_from_report(status.data, "latest_known_bug_lost_signal",
                                     &known_lost);
    (void)summary_number_from_report(status.data,
                                     "latest_known_bug_baseline_failures",
                                     &known_baseline);
    (void)summary_number_from_report(status.data, "latest_perf_hotspots",
                                     &perf_hotspots);
    if (!summary_number_from_report(status.data, "latest_perf_max_ratio",
                                    &perf_worst_ratio))
      (void)summary_number_from_report(status.data, "perf_max_ratio",
                                       &perf_worst_ratio);
    (void)summary_number_from_report(status.data,
                                     "latest_full_pressure_perf_hotspots",
                                     &latest_full_pressure_perf_hotspots);
    (void)summary_number_from_report(status.data,
                                     "latest_full_pressure_perf_max_ratio",
                                     &latest_full_pressure_perf_worst_ratio);
    (void)summary_number_from_report(status.data,
                                     "latest_full_pressure_perf_rows",
                                     &latest_full_pressure_perf_rows);
    (void)summary_bool_from_report(status.data,
                                   "latest_full_pressure_perf_suite_current",
                                   &latest_full_pressure_perf_suite_current);
    (void)summary_number_from_report(status.data,
                                     "latest_full_pressure_failure_count",
                                     &latest_full_pressure_failure_count);
    bool have_latest_full_pressure_ok =
        summary_bool_from_report(status.data, "latest_full_pressure_ok",
                                 &latest_full_pressure_ok);
    bool have_latest_full_pressure_clean =
        summary_bool_from_report(status.data, "latest_full_pressure_clean",
                                 &latest_full_pressure_clean);
    (void)summary_bool_from_report(
        status.data, "latest_report_demoted_non_reproducing_afl_timeout",
        &latest_report_demoted_non_reproducing_afl_timeout);
    (void)summary_bool_from_report(
        status.data, "latest_full_pressure_demoted_non_reproducing_afl_timeout",
        &latest_full_pressure_demoted_non_reproducing_afl_timeout);
    free(latest_full_pressure_clean_reason);
    latest_full_pressure_clean_reason =
        summary_string_from_report(status.data,
                                   "latest_full_pressure_clean_reason");
    if (!latest_full_pressure_clean_reason)
      latest_full_pressure_clean_reason = strdup("");
    if (!have_latest_full_pressure_ok)
      latest_full_pressure_ok =
          latest_full_pressure_failure_count <= 0.0 &&
          latest_full_pressure_perf_suite_current;
    if (!have_latest_full_pressure_clean)
      latest_full_pressure_clean = latest_full_pressure_ok &&
                                   latest_full_pressure_perf_suite_current;
    if (!latest_full_pressure_clean_reason ||
        !*latest_full_pressure_clean_reason) {
      free(latest_full_pressure_clean_reason);
      latest_full_pressure_clean_reason =
          strdup(latest_full_pressure_clean ?
                     (latest_full_pressure_demoted_non_reproducing_afl_timeout ?
                          "demoted-non-reproducing-afl-timeout" : "ok") :
                     (latest_full_pressure_perf_suite_current ?
                          "attention" : "stale-perf-suite"));
    }
    (void)summary_number_from_report(status.data, "non_reproducing_afl_timeouts",
                                     &non_reproducing_timeouts);
    (void)summary_number_from_report(status.data,
                                     "historical_non_reproducing_afl_timeouts",
                                     &historical_non_reproducing_timeouts);
    (void)summary_bool_from_report(status.data, "ready", &ready);
    (void)summary_bool_from_report(status.data, "target_reached",
                                   &target_reached);
    (void)summary_bool_from_report(status.data, "campaign_complete",
                                   &campaign_complete);
    (void)summary_bool_from_report(status.data, "cache_policy_ok",
                                   &cache_policy_ok);
    old_path_cache_policy_ok = cache_policy_ok;
    (void)summary_bool_from_report(status.data, "old_path_cache_policy_ok",
                                   &old_path_cache_policy_ok);
    (void)summary_bool_from_report(status.data, "ny_bin_exists",
                                   &ny_bin_exists);
    (void)summary_bool_from_report(status.data, "old_nytrix_test_scratch_absent",
                                   &old_nytrix_test_scratch_absent);
    (void)summary_bool_from_report(status.data, "old_nytrix_fuzz_absent",
                                   &old_nytrix_fuzz_absent);
    (void)summary_bool_from_report(status.data, "old_nytrix_build_cache_absent",
                                   &old_nytrix_build_cache_absent);
    old_path_present_count =
        (old_nytrix_test_scratch_absent ? 0.0 : 1.0) +
        (old_nytrix_fuzz_absent ? 0.0 : 1.0) +
        (old_nytrix_build_cache_absent ? 0.0 : 1.0);
    old_path_remaining_count = old_path_present_count;
    (void)summary_bool_from_report(status.data,
                                   "active_old_nytrix_output_writer_present",
                                   &active_old_nytrix_writer_present);
    free(completion_eta);
    free(next_script);
    free(status_next_handoff_command);
    free(status_next_command);
    free(run_command);
    free(status_command);
    free(stop_file);
    free(stop_command);
    free(resume_command);
  free(state_file);
    free(state_command);
    free(state_refresh_command);
    free(tmp_dir);
    free(scratch_root);
    free(xdg_cache_home);
    free(nytrix_cache_dir);
    free(old_path_command);
    free(advisory_action_command);
    free(advisory_recheck_command);
    free(perf_watchlist_command);
      free(perf_watchlist_report);
      free(perf_watchlist_markdown);
      free(perf_watchlist_artifact_max_case);
    free(perf_watchlist_artifact_max_artifact);
    free(perf_watchlist_artifact_max_ny_source);
    free(perf_watchlist_artifact_max_c_source);
    free(latest_report);
    free(latest_full_pressure_report);
    free(perf_worst_case);
    free(latest_full_pressure_perf_case);
    free(coverage_next_action);
    free(coverage_next_category);
    free(coverage_next_severity);
    free(coverage_next_lane);
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
    free(coverage_next_stop_file);
    free(coverage_next_stop_command);
    free(coverage_next_resume_command);
    completion_eta = summary_string_from_report(status.data, "completion_eta_local");
    next_script = summary_string_from_report(status.data, "next_script");
    status_next_handoff_command =
        summary_string_from_report(status.data, "next_handoff_command");
    if (!status_next_handoff_command)
      status_next_handoff_command = strdup("");
    status_next_command = summary_string_from_report(status.data, "next_command");
    run_command = summary_string_from_report(status.data, "run_command");
    if (run_command && *run_command &&
        !fuzz_all_command_uses_env_nice(run_command)) {
      char *guarded_run_command =
          fuzz_all_low_priority_command_dup(run_command);
      free(run_command);
      run_command = guarded_run_command;
    }
    status_command = summary_string_from_report(status.data, "status_command");
    if (status_command && *status_command &&
        !fuzz_all_command_uses_env_nice(status_command)) {
      char *guarded_status_command =
          fuzz_all_low_priority_command_dup(status_command);
      free(status_command);
      status_command = guarded_status_command;
    }
    stop_file = summary_string_from_report(status.data, "stop_file");
    if (!stop_file || !*stop_file) {
      free(stop_file);
      stop_file = fuzz_all_stop_file_path(dir_cmd);
    }
    stop_command = summary_string_from_report(status.data, "stop_command");
    if (!stop_command || !*stop_command) {
      free(stop_command);
      stop_command = fuzz_all_stop_command(stop_file);
    }
    resume_command = summary_string_from_report(status.data, "resume_command");
    if (!resume_command || !*resume_command) {
      free(resume_command);
      resume_command = fuzz_all_resume_command(stop_file);
    }
    state_file = summary_string_from_report(status.data, "state_file");
    if (!state_file || !*state_file) {
      free(state_file);
      state_file = fuzz_all_state_file_path(dir_cmd);
    }
    state_command = summary_string_from_report(status.data, "state_command");
    if (!state_command || !*state_command) {
      free(state_command);
      state_command = fuzz_all_state_command(state_file);
    }
    state_refresh_command =
        summary_string_from_report(status.data, "state_refresh_command");
    if (!state_refresh_command)
      state_refresh_command = strdup("");
    tmp_dir = summary_string_from_report(status.data, "tmp_dir");
    if (!tmp_dir || !*tmp_dir) {
      free(tmp_dir);
      tmp_dir = strdup("build/cache/tmp");
    }
    scratch_root = summary_string_from_report(status.data, "scratch_root");
    if (!scratch_root || !*scratch_root) {
      free(scratch_root);
      scratch_root = strdup("build/cache/scratch");
    }
    xdg_cache_home = summary_string_from_report(status.data, "xdg_cache_home");
    if (!xdg_cache_home || !*xdg_cache_home) {
      free(xdg_cache_home);
      xdg_cache_home = strdup("build/cache/xdg");
    }
    nytrix_cache_dir =
        summary_string_from_report(status.data, "nytrix_cache_dir");
    if (!nytrix_cache_dir || !*nytrix_cache_dir) {
      free(nytrix_cache_dir);
      nytrix_cache_dir = strdup("build/cache/nytrix");
    }
    old_path_command =
        summary_string_from_report(status.data, "old_path_command");
    if (!old_path_command || !*old_path_command) {
      free(old_path_command);
      old_path_command = strdup(NYTRIX_OLD_PATH_DRY_RUN_COMMAND);
    } else if (!fuzz_all_command_uses_env_nice(old_path_command)) {
      char *guarded_old_path_command =
          fuzz_all_low_priority_command_dup(old_path_command);
      free(old_path_command);
      old_path_command = guarded_old_path_command;
    }
    char *status_old_path_dry_run_command =
        summary_string_from_report(status.data, "old_path_dry_run_command");
    if (!status_old_path_dry_run_command || !*status_old_path_dry_run_command) {
      free(status_old_path_dry_run_command);
      status_old_path_dry_run_command =
          strdup(old_path_dry_run_command && *old_path_dry_run_command ?
                     old_path_dry_run_command :
                     (old_path_command && *old_path_command ?
                          old_path_command : NYTRIX_OLD_PATH_DRY_RUN_COMMAND));
    } else if (!fuzz_all_command_uses_env_nice(
                   status_old_path_dry_run_command)) {
      char *guarded_old_path_dry_run_command =
          fuzz_all_low_priority_command_dup(status_old_path_dry_run_command);
      free(status_old_path_dry_run_command);
      status_old_path_dry_run_command = guarded_old_path_dry_run_command;
    }
    free(old_path_dry_run_command);
    old_path_dry_run_command = status_old_path_dry_run_command;
    if (old_path_dry_run_command && *old_path_dry_run_command &&
        (!old_path_command ||
         strcmp(old_path_command, old_path_dry_run_command) != 0)) {
      free(old_path_command);
      old_path_command = strdup(old_path_dry_run_command);
    }
    char *status_old_path_apply_command =
        summary_string_from_report(status.data, "old_path_apply_command");
    if (!status_old_path_apply_command || !*status_old_path_apply_command) {
      free(status_old_path_apply_command);
      status_old_path_apply_command =
          strdup(old_path_apply_command && *old_path_apply_command ?
                     old_path_apply_command : NYTRIX_OLD_PATH_APPLY_COMMAND);
    } else if (!fuzz_all_command_uses_env_nice(
                   status_old_path_apply_command)) {
      char *guarded_old_path_apply_command =
          fuzz_all_low_priority_command_dup(status_old_path_apply_command);
      free(status_old_path_apply_command);
      status_old_path_apply_command = guarded_old_path_apply_command;
    }
    free(old_path_apply_command);
    old_path_apply_command = status_old_path_apply_command;
    free(old_path_report);
    old_path_report = summary_string_from_report(status.data, "old_path_report");
    if (!old_path_report || !*old_path_report) {
      free(old_path_report);
      (void)asprintf(&old_path_report, "%s/old-paths.json", dir_cmd);
      if (!old_path_report)
        old_path_report = strdup("build/fuzz/all/old-paths.json");
    }
    free(old_path_markdown);
    old_path_markdown =
        summary_string_from_report(status.data, "old_path_markdown");
    if (!old_path_markdown || !*old_path_markdown) {
      free(old_path_markdown);
      (void)asprintf(&old_path_markdown, "%s/old-paths.md", dir_cmd);
      if (!old_path_markdown)
        old_path_markdown = strdup("build/fuzz/all/old-paths.md");
    }
    (void)summary_number_from_report(status.data,
                                     "old_path_artifact_leak_count",
                                     &old_path_artifact_leak_count);
    (void)summary_number_from_report(status.data,
                                     "old_path_artifact_moved_count",
                                     &old_path_artifact_moved_count);
    (void)summary_number_from_report(status.data,
                                     "old_path_artifact_remaining_count",
                                     &old_path_artifact_remaining_count);
    (void)summary_number_from_report(status.data, "old_path_present_count",
                                     &old_path_present_count);
    (void)summary_number_from_report(status.data, "old_path_moved_count",
                                     &old_path_moved_count);
    (void)summary_number_from_report(status.data, "old_path_remaining_count",
                                     &old_path_remaining_count);
    (void)summary_number_from_report(status.data,
                                     "old_path_wait_remaining_seconds",
                                     &old_path_wait_remaining_seconds);
    free(old_path_next_action);
    old_path_next_action =
        summary_string_from_report(status.data, "old_path_next_action");
    if (!old_path_next_action || !*old_path_next_action) {
      free(old_path_next_action);
      old_path_next_action =
          strdup(fuzz_all_old_path_next_action(
              old_path_remaining_count, active_old_nytrix_writer_present));
    }
    free(old_path_next_reason);
    old_path_next_reason =
        summary_string_from_report(status.data, "old_path_next_reason");
    if (!old_path_next_reason || !*old_path_next_reason) {
      free(old_path_next_reason);
      old_path_next_reason =
          strdup(fuzz_all_old_path_next_reason(
              old_path_remaining_count, active_old_nytrix_writer_present));
    }
    advisory_action_command =
        summary_string_from_report(status.data, "advisory_action_command");
    if (!advisory_action_command || !*advisory_action_command) {
      free(advisory_action_command);
      char *history_rel = rel_path_dup(root, history_path ? history_path : "");
      char *worklist_history_rel =
          rel_path_dup(root, worklist_history_path ? worklist_history_path : "");
      char *worklist_history_md_rel =
          rel_path_dup(root, worklist_history_markdown_path ?
                                worklist_history_markdown_path : "");
      advisory_action_command =
          fuzz_all_advisory_action_command(history_rel, worklist_history_rel,
                                           worklist_history_md_rel);
      free(history_rel);
      free(worklist_history_rel);
      free(worklist_history_md_rel);
    } else if (!fuzz_all_command_uses_env_nice(advisory_action_command)) {
      char *guarded_advisory_action_command =
          fuzz_all_low_priority_command_dup(advisory_action_command);
      free(advisory_action_command);
      advisory_action_command = guarded_advisory_action_command;
    }
    advisory_recheck_command =
        summary_string_from_report(status.data, "advisory_recheck_command");
    if (!advisory_recheck_command)
      advisory_recheck_command = strdup("");
    (void)summary_number_from_report(
        status.data, "advisory_recheck_raw_repro_checked",
        &advisory_recheck_raw_repro_checked);
    (void)summary_number_from_report(
        status.data, "advisory_recheck_raw_repro_passed",
        &advisory_recheck_raw_repro_passed);
    (void)summary_number_from_report(
        status.data, "advisory_recheck_raw_repro_timeouts",
        &advisory_recheck_raw_repro_timeouts);
    (void)summary_number_from_report(
        status.data, "advisory_recheck_raw_repro_unexpected",
        &advisory_recheck_raw_repro_unexpected);
    perf_watchlist_command =
        summary_string_from_report(status.data, "perf_watchlist_command");
    if (!perf_watchlist_command || !*perf_watchlist_command) {
      free(perf_watchlist_command);
      perf_watchlist_command = fuzz_all_perf_watchlist_command(dir_cmd);
    } else if (!fuzz_all_command_uses_env_nice(perf_watchlist_command)) {
      char *guarded_perf_watchlist_command =
          fuzz_all_low_priority_command_dup(perf_watchlist_command);
      free(perf_watchlist_command);
      perf_watchlist_command = guarded_perf_watchlist_command;
    }
    perf_watchlist_report =
        summary_string_from_report(status.data, "perf_watchlist_report");
    if (!perf_watchlist_report || !*perf_watchlist_report) {
      free(perf_watchlist_report);
      perf_watchlist_report =
          fuzz_all_perf_watchlist_report_path(dir_cmd, false);
    }
    perf_watchlist_markdown =
        summary_string_from_report(status.data, "perf_watchlist_markdown");
    if (!perf_watchlist_markdown || !*perf_watchlist_markdown) {
      free(perf_watchlist_markdown);
      perf_watchlist_markdown =
          fuzz_all_perf_watchlist_report_path(dir_cmd, true);
    }
    (void)summary_bool_from_report(status.data,
                                   "perf_watchlist_artifact_readable",
                                   &perf_watchlist_artifact_readable);
    (void)summary_bool_from_report(status.data,
                                   "perf_watchlist_artifact_fresh",
                                   &perf_watchlist_artifact_fresh);
    (void)summary_number_from_report(status.data,
                                     "perf_watchlist_artifact_hotspots",
                                     &perf_watchlist_artifact_hotspots);
    (void)summary_number_from_report(status.data,
                                     "perf_watchlist_artifact_max_ratio",
                                     &perf_watchlist_artifact_max_ratio);
    (void)summary_number_from_report(status.data,
                                     "perf_watchlist_artifact_age_seconds",
                                     &perf_watchlist_artifact_age_seconds);
    (void)summary_number_from_report(status.data,
                                     "perf_watchlist_artifact_stale_after_hours",
                                     &perf_watchlist_artifact_stale_after_hours);
      perf_watchlist_artifact_max_case =
          summary_string_from_report(status.data,
                                     "perf_watchlist_artifact_max_case");
      if (!perf_watchlist_artifact_max_case)
        perf_watchlist_artifact_max_case = strdup("");
    perf_watchlist_artifact_max_artifact =
        summary_string_from_report(status.data,
                                   "perf_watchlist_artifact_max_artifact");
    if (!perf_watchlist_artifact_max_artifact ||
        !*perf_watchlist_artifact_max_artifact) {
      free(perf_watchlist_artifact_max_artifact);
      perf_watchlist_artifact_max_artifact =
          summary_string_from_report(status.data, "optimization_artifact");
    }
    if (!perf_watchlist_artifact_max_artifact)
      perf_watchlist_artifact_max_artifact = strdup("");
    perf_watchlist_artifact_max_ny_source =
        summary_string_from_report(status.data,
                                   "perf_watchlist_artifact_max_ny_source");
    if (!perf_watchlist_artifact_max_ny_source ||
        !*perf_watchlist_artifact_max_ny_source) {
      free(perf_watchlist_artifact_max_ny_source);
      perf_watchlist_artifact_max_ny_source =
          summary_string_from_report(status.data, "optimization_ny_source");
    }
    if (!perf_watchlist_artifact_max_ny_source)
      perf_watchlist_artifact_max_ny_source = strdup("");
    perf_watchlist_artifact_max_c_source =
        summary_string_from_report(status.data,
                                   "perf_watchlist_artifact_max_c_source");
    if (!perf_watchlist_artifact_max_c_source ||
        !*perf_watchlist_artifact_max_c_source) {
      free(perf_watchlist_artifact_max_c_source);
      perf_watchlist_artifact_max_c_source =
          summary_string_from_report(status.data, "optimization_c_source");
    }
    if (!perf_watchlist_artifact_max_c_source)
      perf_watchlist_artifact_max_c_source = strdup("");
    if (!perf_watchlist_artifact_readable ||
        !*perf_watchlist_artifact_max_artifact ||
        !*perf_watchlist_artifact_max_ny_source ||
        !*perf_watchlist_artifact_max_c_source) {
        char artifact_case[128] = {0};
      char artifact_path[4096] = {0};
      char ny_source_path[4096] = {0};
      char c_source_path[4096] = {0};
        if (fuzz_all_load_perf_watchlist_artifact(
                root, perf_watchlist_report,
                &perf_watchlist_artifact_hotspots,
                &perf_watchlist_artifact_max_ratio, artifact_case,
              sizeof(artifact_case),
              artifact_path, sizeof(artifact_path),
              ny_source_path, sizeof(ny_source_path),
              c_source_path, sizeof(c_source_path))) {
          perf_watchlist_artifact_readable = true;
          free(perf_watchlist_artifact_max_case);
          perf_watchlist_artifact_max_case = strdup(artifact_case);
        free(perf_watchlist_artifact_max_artifact);
        perf_watchlist_artifact_max_artifact = strdup(artifact_path);
        free(perf_watchlist_artifact_max_ny_source);
        perf_watchlist_artifact_max_ny_source = strdup(ny_source_path);
        free(perf_watchlist_artifact_max_c_source);
        perf_watchlist_artifact_max_c_source = strdup(c_source_path);
        }
      }
    if (perf_watchlist_artifact_age_seconds < 0.0)
      perf_watchlist_artifact_age_seconds =
          fuzz_all_score_report_age_seconds(root, perf_watchlist_report);
    if (perf_watchlist_artifact_stale_after_hours <= 0.0)
      perf_watchlist_artifact_stale_after_hours =
          fuzz_all_perf_watchlist_artifact_fresh_hours();
    perf_watchlist_artifact_fresh =
        perf_watchlist_artifact_readable &&
        fuzz_all_score_age_fresh(perf_watchlist_artifact_age_seconds,
                                 perf_watchlist_artifact_stale_after_hours);
    latest_report = summary_string_from_report(status.data, "latest_report");
    latest_full_pressure_report =
        summary_string_from_report(status.data, "latest_full_pressure_report");
    latest_full_pressure_perf_case =
        summary_string_from_report(status.data,
                                   "latest_full_pressure_perf_max_case");
    if (!latest_full_pressure_perf_case)
      latest_full_pressure_perf_case = strdup("");
    perf_worst_case =
        summary_string_from_report(status.data, "latest_perf_max_case");
    if (!perf_worst_case || !*perf_worst_case) {
      free(perf_worst_case);
      perf_worst_case = summary_string_from_report(status.data, "perf_max_case");
    }
    if (latest_full_pressure_perf_suite_current &&
        latest_full_pressure_perf_worst_ratio > perf_worst_ratio) {
      perf_worst_ratio = latest_full_pressure_perf_worst_ratio;
      free(perf_worst_case);
      perf_worst_case = strdup(latest_full_pressure_perf_case ?
                                   latest_full_pressure_perf_case : "");
      if (!perf_worst_case)
        perf_worst_case = strdup("");
    }
    char *effective_perf_worst_case = strdup(
        fuzz_all_perf_effective_worst_case(
            perf_worst_case, perf_watchlist_artifact_readable,
            perf_watchlist_artifact_fresh,
            perf_watchlist_artifact_max_ratio,
            perf_watchlist_artifact_max_case));
    if (effective_perf_worst_case) {
      free(perf_worst_case);
      perf_worst_case = effective_perf_worst_case;
    }
    perf_worst_ratio = fuzz_all_perf_effective_worst_ratio(
        perf_worst_ratio, perf_watchlist_artifact_readable,
        perf_watchlist_artifact_fresh,
        perf_watchlist_artifact_max_ratio);
    coverage_next_action =
        summary_string_from_report(status.data, "coverage_next_action");
    if (!coverage_next_action)
      coverage_next_action = strdup("none");
    coverage_next_category =
        summary_string_from_report(status.data, "coverage_next_category");
    if (!coverage_next_category)
      coverage_next_category = strdup("");
    coverage_next_severity =
        summary_string_from_report(status.data, "coverage_next_severity");
    if (!coverage_next_severity)
      coverage_next_severity = strdup("");
    coverage_next_lane =
        summary_string_from_report(status.data, "coverage_next_lane");
    if (!coverage_next_lane)
      coverage_next_lane = strdup("");
    coverage_next_reason =
        summary_string_from_report(status.data, "coverage_next_reason");
    if (!coverage_next_reason)
      coverage_next_reason = strdup("");
    coverage_next_command =
        summary_string_from_report(status.data, "coverage_next_command");
    if (!coverage_next_command)
      coverage_next_command = strdup("");
    coverage_next_guarded_command =
        summary_string_from_report(status.data,
                                   "coverage_next_guarded_command");
    if (!coverage_next_guarded_command)
      coverage_next_guarded_command = strdup("");
    coverage_next_low_cpu_command =
        summary_string_from_report(status.data,
                                   "coverage_next_low_cpu_command");
    if (!coverage_next_low_cpu_command ||
        !*coverage_next_low_cpu_command) {
      free(coverage_next_low_cpu_command);
      coverage_next_low_cpu_command =
          coverage_next_guarded_command && *coverage_next_guarded_command ?
              fuzz_all_missing_evidence_low_cpu_command(
                  coverage_next_guarded_command) : strdup("");
    }
    coverage_next_preview_command =
        summary_string_from_report(status.data,
                                   "coverage_next_preview_command");
    if (!coverage_next_preview_command || !*coverage_next_preview_command) {
      free(coverage_next_preview_command);
      coverage_next_preview_command =
          fuzz_all_coverage_next_preview_command(
              coverage_next_command, dir_cmd, target_arg, hours_arg,
              threads_arg, profile_arg);
    }
    coverage_next_state_file =
        summary_string_from_report(status.data, "coverage_next_state_file");
    if (!coverage_next_state_file || !*coverage_next_state_file) {
      free(coverage_next_state_file);
      coverage_next_state_file =
          coverage_next_guarded_command && *coverage_next_guarded_command ?
              fuzz_all_missing_evidence_state_file_path(dir_cmd) : strdup("");
    }
    coverage_next_state_command =
        summary_string_from_report(status.data, "coverage_next_state_command");
    if (!coverage_next_state_command || !*coverage_next_state_command) {
      free(coverage_next_state_command);
      coverage_next_state_command =
          coverage_next_state_file && *coverage_next_state_file ?
              fuzz_all_missing_evidence_state_command(
                  coverage_next_state_file) : strdup("");
    }
    coverage_next_state_refresh_command =
        summary_string_from_report(status.data,
                                   "coverage_next_state_refresh_command");
    if (!coverage_next_state_refresh_command ||
        !*coverage_next_state_refresh_command) {
      free(coverage_next_state_refresh_command);
      coverage_next_state_refresh_command =
          coverage_next_guarded_command && *coverage_next_guarded_command ?
              fuzz_all_missing_evidence_state_refresh_command(
                  coverage_next_guarded_command) : strdup("");
    }
    (void)summary_bool_from_report(
        status.data, "coverage_next_state_refresh_required",
        &coverage_next_state_refresh_required);
    coverage_next_state_refresh_reason =
        summary_string_from_report(status.data,
                                   "coverage_next_state_refresh_reason");
    if (!coverage_next_state_refresh_reason)
      coverage_next_state_refresh_reason = strdup("");
    recommended_state_refresh_command =
        summary_string_from_report(status.data,
                                   "recommended_state_refresh_command");
    if (!recommended_state_refresh_command)
      recommended_state_refresh_command = strdup("");
    coverage_next_stop_file =
        summary_string_from_report(status.data, "coverage_next_stop_file");
    if (!coverage_next_stop_file || !*coverage_next_stop_file) {
      free(coverage_next_stop_file);
      coverage_next_stop_file =
          coverage_next_guarded_command && *coverage_next_guarded_command ?
              fuzz_all_missing_evidence_stop_file_path(dir_cmd) : strdup("");
    }
    coverage_next_stop_command =
        summary_string_from_report(status.data, "coverage_next_stop_command");
    if (!coverage_next_stop_command || !*coverage_next_stop_command) {
      free(coverage_next_stop_command);
      coverage_next_stop_command =
          coverage_next_stop_file && *coverage_next_stop_file ?
              fuzz_all_missing_evidence_stop_command(coverage_next_stop_file) :
              strdup("");
    }
    coverage_next_resume_command =
        summary_string_from_report(status.data,
                                   "coverage_next_resume_command");
    if (!coverage_next_resume_command || !*coverage_next_resume_command) {
      free(coverage_next_resume_command);
      coverage_next_resume_command =
          coverage_next_stop_file && *coverage_next_stop_file ?
              fuzz_all_missing_evidence_resume_command(
                  coverage_next_stop_file) : strdup("");
    }
  }

  fuzz_all_run_state_summary_t run_state;
  fuzz_all_load_run_state(root, state_file, &run_state);
  if (!run_state.readable && readable)
    fuzz_all_run_state_from_report(status.data, &run_state);
  fuzz_all_run_state_summary_t coverage_next_run_state;
  fuzz_all_load_run_state(root, coverage_next_state_file,
                          &coverage_next_run_state);

  char *next_handoff_command =
      fuzz_all_progress_next_command(next_script,
                                     status_next_handoff_command &&
                                             *status_next_handoff_command ?
                                         status_next_handoff_command :
                                         status_next_command,
                                     run_command, status_command);
  char *guarded_next_command =
      fuzz_all_low_priority_command_dup(next_handoff_command);
  char *next_command = guarded_next_command ? guarded_next_command : strdup("");
  char *preview_command = fuzz_all_preview_command(next_handoff_command);
  char run_next_gentle_command[4096] = {0};
  char run_next_gentle_preview_command[4096] = {0};
  fuzz_all_gentle_run_command(run_next_gentle_command,
                              sizeof(run_next_gentle_command),
                              next_handoff_command);
  fuzz_all_gentle_preview_command(run_next_gentle_preview_command,
                                  sizeof(run_next_gentle_preview_command),
                                  next_handoff_command);
  if (!state_refresh_command || !*state_refresh_command) {
    free(state_refresh_command);
    state_refresh_command = strdup(preview_command ? preview_command : "");
  }
  double latest_report_age_seconds =
      fuzz_all_progress_report_age_seconds(root, latest_report);
  double latest_full_pressure_report_age_seconds =
      fuzz_all_progress_report_age_seconds(root, latest_full_pressure_report);
  double latest_report_stale_after_hours =
      fuzz_all_progress_latest_fresh_hours();
  double latest_full_pressure_report_stale_after_hours =
      fuzz_all_progress_full_pressure_fresh_hours();
  bool latest_report_fresh =
      fuzz_all_progress_age_fresh(latest_report_age_seconds,
                                  latest_report_stale_after_hours);
  bool latest_full_pressure_report_fresh =
      fuzz_all_progress_age_fresh(latest_full_pressure_report_age_seconds,
                                  latest_full_pressure_report_stale_after_hours);
  bool evidence_fresh = latest_report_fresh && latest_full_pressure_report_fresh;
  double freshness_penalty = 0.0;
  if (!latest_report_fresh) freshness_penalty += 8.0;
  if (!latest_full_pressure_report_fresh) freshness_penalty += 12.0;

  double effective_advisory_timeouts = fuzz_all_effective_advisory_timeouts(
      non_reproducing_timeouts,
      advisory_recheck_raw_repro_checked,
      advisory_recheck_raw_repro_passed,
      advisory_recheck_raw_repro_timeouts,
      advisory_recheck_raw_repro_unexpected,
      advisory_recheck_command);
  double signal_score =
      fuzz_all_progress_signal_score(ready, blocker_count, active_items,
                                     coverage_blocker_gaps, compiler_live,
                                     compiler_missing, known_reproduced,
                                     known_lost, known_baseline,
                                     perf_hotspots,
                                     effective_advisory_timeouts,
                                     cache_policy_ok, ny_bin_exists);
  double gate_penalty = 0.0, correctness_penalty = 0.0, perf_penalty = 0.0;
  double advisory_penalty = 0.0, env_penalty = 0.0;
  fuzz_all_progress_signal_breakdown(ready, blocker_count, active_items,
                                     coverage_blocker_gaps, compiler_live,
                                     compiler_missing, known_reproduced,
                                     known_lost, known_baseline,
                                     perf_hotspots,
                                     effective_advisory_timeouts,
                                     cache_policy_ok, ny_bin_exists,
                                     &gate_penalty, &correctness_penalty,
                                     &perf_penalty, &advisory_penalty,
                                     &env_penalty);
  double evidence_cap =
      fuzz_all_progress_evidence_cap(target_percent, campaign_complete);
  double capped_score =
      signal_score < evidence_cap ? signal_score : evidence_cap;
  double stability_score =
      fuzz_all_progress_clamp(capped_score - freshness_penalty, 0.0, 100.0);
  const char *stability_label =
      fuzz_all_progress_score_label(stability_score);
  char stability_note[256];
  fuzz_all_progress_score_note(stability_note, sizeof(stability_note),
                               ready, blocker_count, active_items,
                               freshness_penalty, advisory_penalty,
                               signal_score, evidence_cap, target_percent);
  double thread_years_per_run = 0.0;
  const char *thread_years_per_run_source = "remaining-even";
  if (thread_years_per_day > 0.0 && runs_per_day > 0.0) {
    thread_years_per_run = thread_years_per_day / runs_per_day;
    thread_years_per_run_source = "plan-rate";
  } else {
    thread_years_per_run =
        fuzz_all_progress_thread_years_per_run(remaining_thread_years,
                                               runs_needed);
  }
  double target_percent_per_run =
      fuzz_all_progress_percent_per_run(thread_years_per_run,
                                        target_thread_years);
  double next_run_target_percent =
      fuzz_all_progress_clamp(target_percent + target_percent_per_run, 0.0,
                              100.0);
  bool next_run_complete = campaign_complete || next_run_target_percent >= 100.0;
  double next_run_evidence_cap =
      fuzz_all_progress_evidence_cap(next_run_target_percent, next_run_complete);
  double next_run_stability_score =
      signal_score < next_run_evidence_cap ? signal_score : next_run_evidence_cap;
  double next_run_stability_delta =
      next_run_stability_score - stability_score;
  double runs_to_good_stability =
      fuzz_all_progress_runs_to_good(target_percent, target_percent_per_run,
                                     signal_score, stability_score,
                                     campaign_complete);
  if (freshness_penalty > 0.0) runs_to_good_stability = -1.0;
  double runs_to_good_days =
      fuzz_all_progress_runs_to_days(runs_to_good_stability, runs_per_day);
  double compiler_findings = compiler_live + compiler_missing;
  double known_bug_replay_findings =
      known_reproduced + known_lost + known_baseline;
  double correctness_findings = compiler_findings + known_bug_replay_findings;
  double perf_watchlist_open =
      fuzz_all_perf_watchlist_effective_open(
          perf_hotspots, perf_worst_ratio, perf_watchlist_artifact_readable,
          perf_watchlist_artifact_fresh, perf_watchlist_artifact_hotspots);
  const char *perf_watchlist_case =
      perf_watchlist_open > 0.0 ? perf_worst_case : "";
  const char *perf_watchlist_state =
      fuzz_all_perf_watchlist_state(perf_watchlist_open,
                                    perf_watchlist_artifact_readable,
                                    perf_watchlist_artifact_fresh,
                                    perf_watchlist_artifact_hotspots);
  const char *perf_watchlist_action =
      fuzz_all_perf_watchlist_action(perf_watchlist_state);
  char *perf_watchlist_action_command =
      fuzz_all_perf_watchlist_action_command(
          perf_watchlist_state, perf_watchlist_command,
          perf_watchlist_markdown, perf_watchlist_report);
  const char *optimization_reason =
      fuzz_all_optimization_reason(perf_watchlist_state);
  const char *optimization_case =
      fuzz_all_optimization_case(perf_watchlist_artifact_readable,
                                 perf_watchlist_artifact_fresh,
                                 perf_watchlist_artifact_hotspots,
                                 perf_watchlist_artifact_max_case,
                                 perf_watchlist_artifact_max_ratio,
                                 perf_watchlist_open, perf_watchlist_case);
    double optimization_ratio =
        fuzz_all_optimization_ratio(perf_watchlist_artifact_readable,
                                    perf_watchlist_artifact_fresh,
                                    perf_watchlist_artifact_hotspots,
                                    perf_watchlist_artifact_max_ratio,
                                    perf_watchlist_open, perf_worst_ratio);
  const char *optimization_artifact =
      fuzz_all_optimization_artifact_path(
          perf_watchlist_artifact_readable, perf_watchlist_artifact_fresh,
          perf_watchlist_artifact_hotspots, perf_watchlist_artifact_max_ratio,
          perf_watchlist_artifact_max_artifact);
  const char *optimization_ny_source =
      fuzz_all_optimization_artifact_path(
          perf_watchlist_artifact_readable, perf_watchlist_artifact_fresh,
          perf_watchlist_artifact_hotspots, perf_watchlist_artifact_max_ratio,
          perf_watchlist_artifact_max_ny_source);
  const char *optimization_c_source =
      fuzz_all_optimization_artifact_path(
          perf_watchlist_artifact_readable, perf_watchlist_artifact_fresh,
          perf_watchlist_artifact_hotspots, perf_watchlist_artifact_max_ratio,
          perf_watchlist_artifact_max_c_source);
  char *optimization_target_command =
      fuzz_all_optimization_target_command(optimization_ny_source,
                                           optimization_c_source,
                                           optimization_artifact);
    double evidence_coverage_depth_percent =
      fuzz_all_ratio_percent(evidence_coverage_ran_lanes,
                             evidence_coverage_lanes);
  double evidence_coverage_not_run_lanes =
      fuzz_all_not_run_lanes(evidence_coverage_ran_lanes,
                             evidence_coverage_lanes);
  bool have_coverage_next =
      coverage_next_lane && *coverage_next_lane &&
      coverage_next_command && *coverage_next_command;
  coverage_next_state_refresh_required =
      have_coverage_next && coverage_next_state_refresh_command &&
      *coverage_next_state_refresh_command &&
      fuzz_all_run_state_refresh_required(&coverage_next_run_state);
  free(coverage_next_state_refresh_reason);
  coverage_next_state_refresh_reason = strdup(
      coverage_next_state_refresh_required ?
          fuzz_all_run_state_refresh_reason(&coverage_next_run_state) : "");
  if (!coverage_next_state_refresh_reason)
    coverage_next_state_refresh_reason = strdup("");
  fuzz_all_recommendation_t recommendation;
  fuzz_all_recommendation_make(
      &recommendation, ready, fuzz_all_state_phase_live(run_state.phase),
      evidence_fresh, cache_policy_ok, ny_bin_exists,
      active_old_nytrix_writer_present, blocker_count, active_items,
        correctness_findings, perf_hotspots, runs_to_good_stability, runs_needed,
        target_reached, campaign_complete, next_handoff_command, state_command,
        old_path_command, advisory_action_command, progress_command,
        coverage_next_command, coverage_next_guarded_command,
        coverage_next_low_cpu_command,
        coverage_next_preview_command);
  const char *selected_preview_command =
      fuzz_all_selected_run_preview_command(preview_command, &recommendation);
  const char *selected_preview_for_refresh =
      fuzz_all_recommendation_run_preview_selected(&recommendation) ?
          recommendation.preview_command : "";
  const char *selected_state_refresh_command =
      fuzz_all_selected_run_state_refresh_command(
          state_refresh_command, selected_preview_for_refresh, &run_state);
  bool use_coverage_next_state =
      strcmp(recommendation.action, "run-missing-evidence") == 0 &&
      have_coverage_next;
  const char *run_state_refresh =
      recommendation.preview_command[0] ? recommendation.preview_command :
                                          state_refresh_command;
  bool run_state_refresh_required =
      !use_coverage_next_state && run_state_refresh && *run_state_refresh &&
      fuzz_all_run_state_refresh_required(&run_state);
  free(recommended_state_refresh_command);
  recommended_state_refresh_command = strdup(
      use_coverage_next_state && coverage_next_state_refresh_required &&
              coverage_next_state_refresh_command ?
          coverage_next_state_refresh_command :
          (run_state_refresh_required ? run_state_refresh : ""));
  if (!recommended_state_refresh_command)
    recommended_state_refresh_command = strdup("");
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"fuzz-all-progress\",\"status_report\":");
  append_rel_json_str(&row, root, status_path ? status_path : "");
  (void)sb_append(&row, ",\"worklist_report\":");
  append_rel_json_str(&row, root, worklist_path ? worklist_path : "");
  (void)sb_append(&row, ",\"worklist_markdown\":");
  append_rel_json_str(&row, root,
                      worklist_markdown_path ? worklist_markdown_path : "");
  (void)sb_append(&row, ",\"historical_worklist_report\":");
  append_rel_json_str(&row, root,
                      worklist_history_path ? worklist_history_path : "");
  (void)sb_append(&row, ",\"historical_worklist_markdown\":");
  append_rel_json_str(&row, root,
                      worklist_history_markdown_path ?
                          worklist_history_markdown_path : "");
  (void)sb_append(&row, ",\"coverage_report\":");
  append_rel_json_str(&row, root, coverage_path ? coverage_path : "");
  (void)sb_append(&row, ",\"coverage_markdown\":");
  append_rel_json_str(&row, root,
                      coverage_markdown_path ? coverage_markdown_path : "");
  (void)sb_append(&row, ",\"plan_report\":");
  append_rel_json_str(&row, root, plan_path ? plan_path : "");
  (void)sb_append(&row, ",\"plan_markdown\":");
  append_rel_json_str(&row, root, plan_markdown_path ? plan_markdown_path : "");
  append_fuzz_all_compiler_std_audit_values(
      &row, compiler_std_audit_readable, compiler_std_audit_report,
      compiler_std_audit_markdown, compiler_std_audit_command,
      runtime_surface_state, runtime_exports, direct_runtime_refs,
      runtime_export_coverage_percent,
      runtime_unreferenced_percent, runtime_unreferenced_count,
      runtime_wrapper_gap_count, crt_surface_state, crt_runtime_exports,
      crt_direct_refs, crt_export_coverage_percent,
      crt_unreferenced_percent, crt_unreferenced_count, crt_wrapper_gap_count,
      crt_unreferenced_family_count, crt_top_unreferenced_family,
      crt_top_unreferenced_family_count, crt_unreferenced_families,
      crt_next_action, crt_next_reason, crt_next_unreferenced_family,
      crt_next_unreferenced_count, crt_next_unreferenced_exports,
      crt_next_definition_file, crt_next_definition_locations,
      crt_next_inspect_command);
  (void)sb_appendf(&row,
                     ",\"target_percent\":%.4f,"
                     "\"campaign_percent\":%.4f,"
                     "\"campaign_confidence_percent\":%.4f,"
                     "\"campaign_remaining_percent\":%.4f,"
                     "\"remaining_percent\":%.4f,"
                      "\"thread_years\":%.8f,"
                      "\"campaign_first_report_epoch\":%.0f,"
                      "\"campaign_latest_report_epoch\":%.0f,"
                      "\"campaign_calendar_span_days\":%.4f,"
                      "\"campaign_calendar_age_days\":%.4f,"
                      "\"campaign_calendar_percent_10y\":%.4f,"
                      "\"target_thread_years\":%.8f,"
                   "\"remaining_thread_years\":%.8f,"
                   "\"runs_needed\":%.0f,"
                   "\"runs_per_day\":%.4f,"
                   "\"wall_hours_needed\":%.4f,"
                   "\"thread_hours_needed\":%.4f,"
                   "\"wall_days_needed\":%.4f,"
                   "\"status_age_seconds\":%.0f,"
                   "\"status_age_minutes\":%.2f,"
                   "\"latest_report_age_seconds\":%.0f,"
                   "\"latest_report_age_hours\":%.2f,"
                   "\"latest_full_pressure_report_age_seconds\":%.0f,"
                   "\"latest_full_pressure_report_age_hours\":%.2f,"
                   "\"latest_report_stale_after_hours\":%.2f,"
                   "\"latest_report_freshness_remaining_hours\":%.2f,"
                   "\"latest_report_freshness_overdue_hours\":%.2f,"
                     "\"latest_report_fresh\":%s,"
                     "\"latest_full_pressure_report_stale_after_hours\":%.2f,"
                     "\"latest_full_pressure_report_freshness_remaining_hours\":%.2f,"
                     "\"latest_full_pressure_report_freshness_overdue_hours\":%.2f,"
                     "\"latest_full_pressure_report_fresh\":%s,"
                     "\"evidence_fresh\":%s,"
                     "\"evidence_freshness_overdue_hours\":%.2f,"
                     "\"strict\":%s,"
                     "\"allow_incomplete_coverage\":%s,"
                   "\"allow_full_pressure_remediation\":%s,"
                   "\"ready\":%s,"
                   "\"blocker_count\":%.0f,"
                   "\"active_items\":%.0f,"
                   "\"active_count\":%.0f,"
                   "\"historical_attention_reports\":%.0f,"
                   "\"target_reached\":%s,"
                   "\"campaign_complete\":%s,"
                   "\"cache_policy_ok\":%s,"
                   "\"ny_bin_exists\":%s,"
                   "\"old_nytrix_test_scratch_absent\":%s,"
                   "\"old_nytrix_fuzz_absent\":%s,"
                   "\"old_nytrix_build_cache_absent\":%s,"
                   "\"active_old_nytrix_output_writer_present\":%s,"
                   "\"stability_score_percent\":%.2f,"
                   "\"stability_percent\":%.2f,"
                   "\"score_percent\":%.2f,"
                   "\"stability_score\":%.2f,"
                   "\"language_score_percent\":%.2f,"
                   "\"language_score\":%.2f,"
                   "\"language_score_good_threshold_percent\":%.2f,"
                   "\"language_score_gap_percent\":%.2f,"
                   "\"signal_health_percent\":%.2f,"
                   "\"evidence_cap_percent\":%.2f,"
                   "\"thread_years_per_run\":%.8f,"
                   "\"target_percent_per_run\":%.4f,"
                   "\"next_run_target_percent\":%.4f,"
                   "\"next_run_stability_score_percent\":%.2f,"
                   "\"next_run_stability_score\":%.2f,"
                   "\"next_run_stability_delta_percent\":%.2f,"
                   "\"next_run_language_score_percent\":%.2f,"
                   "\"next_run_language_score\":%.2f,"
                   "\"next_run_language_score_delta_percent\":%.2f,"
                   "\"runs_to_good_stability\":%.0f,"
                   "\"runs_to_good_stability_score\":%.0f,"
                   "\"runs_to_good_language_score\":%.0f,"
                   "\"runs_to_good_days\":%.4f,"
                   "\"runs_to_good_stability_days\":%.4f,"
                   "\"runs_to_good_language_days\":%.4f,"
                   "\"days_to_good_stability\":%.4f,"
                   "\"days_to_good_language_score\":%.4f,"
                   "\"correctness_findings\":%.0f,"
                   "\"compiler_findings\":%.0f,"
                     "\"known_bug_replay_findings\":%.0f,"
                     "\"perf_hotspots_open\":%.0f,"
                     "\"perf_worst_ratio\":%.4f,"
                     "\"latest_full_pressure_perf_hotspots\":%.0f,"
                     "\"latest_full_pressure_perf_max_ratio\":%.4f,"
                     "\"latest_full_pressure_perf_rows\":%.0f,"
                     "\"latest_full_pressure_perf_suite_current\":%s,"
                     "\"latest_full_pressure_ok\":%s,"
                     "\"latest_full_pressure_clean\":%s,"
                     "\"latest_full_pressure_failure_count\":%.0f,"
                     "\"latest_report_demoted_non_reproducing_afl_timeout\":%s,"
                     "\"latest_full_pressure_demoted_non_reproducing_afl_timeout\":%s,"
                       "\"perf_watchlist_open\":%.0f,"
                       "\"perf_watchlist_threshold_ratio\":%.4f,"
                       "\"advisory_timeouts\":%.0f,"
                       "\"current_advisory_timeouts\":%.0f,"
                       "\"historical_advisory_timeouts\":%.0f,"
                       "\"non_reproducing_afl_timeouts\":%.0f,"
                       "\"historical_non_reproducing_afl_timeouts\":%.0f,"
                       "\"gate_penalty\":%.2f,"
                   "\"correctness_penalty\":%.2f,"
                   "\"perf_penalty\":%.2f,"
                   "\"advisory_penalty\":%.2f,"
                   "\"environment_penalty\":%.2f,"
                   "\"freshness_penalty\":%.2f",
                     target_percent, target_percent, target_percent,
                     fuzz_all_campaign_remaining_percent(target_percent),
                     fuzz_all_campaign_remaining_percent(target_percent),
                      thread_years, campaign_first_report_epoch,
                      campaign_latest_report_epoch,
                      campaign_calendar_span_days,
                      campaign_calendar_age_days,
                      fuzz_all_campaign_calendar_percent_10y(
                          campaign_calendar_age_days),
                      target_thread_years,
                   remaining_thread_years, runs_needed, runs_per_day,
                   wall_hours_needed, thread_hours_needed,
                   wall_days_needed, status_age_seconds,
                   status_age_seconds >= 0.0 ? status_age_seconds / 60.0 : -1.0,
                   latest_report_age_seconds,
                   latest_report_age_seconds >= 0.0 ?
                       latest_report_age_seconds / 3600.0 : -1.0,
                   latest_full_pressure_report_age_seconds,
                   latest_full_pressure_report_age_seconds >= 0.0 ?
                       latest_full_pressure_report_age_seconds / 3600.0 : -1.0,
                   latest_report_stale_after_hours,
                   fuzz_all_freshness_remaining_hours(
                       latest_report_age_seconds,
                       latest_report_stale_after_hours),
                   fuzz_all_freshness_overdue_hours(
                       latest_report_age_seconds,
                       latest_report_stale_after_hours),
                   latest_report_fresh ? "true" : "false",
                     latest_full_pressure_report_stale_after_hours,
                     fuzz_all_freshness_remaining_hours(
                         latest_full_pressure_report_age_seconds,
                         latest_full_pressure_report_stale_after_hours),
                     fuzz_all_freshness_overdue_hours(
                         latest_full_pressure_report_age_seconds,
                         latest_full_pressure_report_stale_after_hours),
                     latest_full_pressure_report_fresh ? "true" : "false",
                     evidence_fresh ? "true" : "false",
                     fuzz_all_evidence_freshness_overdue_hours(
                         latest_report_age_seconds,
                         latest_report_stale_after_hours,
                         latest_full_pressure_report_age_seconds,
                         latest_full_pressure_report_stale_after_hours),
                     strict ? "true" : "false",
                     allow_incomplete_coverage ? "true" : "false",
                     allow_full_pressure_remediation ? "true" : "false",
                     ready ? "true" : "false", blocker_count,
                   active_items, active_items, historical_attention_reports,
                   target_reached ? "true" : "false",
                   campaign_complete ? "true" : "false",
                   cache_policy_ok ? "true" : "false",
                   ny_bin_exists ? "true" : "false",
                   old_nytrix_test_scratch_absent ? "true" : "false",
                   old_nytrix_fuzz_absent ? "true" : "false",
                   old_nytrix_build_cache_absent ? "true" : "false",
                   active_old_nytrix_writer_present ? "true" : "false",
                   stability_score, stability_score,
                   stability_score, stability_score,
                   stability_score, stability_score,
                   fuzz_all_progress_good_threshold(),
                   fuzz_all_language_good_gap_percent(stability_score),
                   signal_score,
                   evidence_cap,
                   thread_years_per_run, target_percent_per_run,
                   next_run_target_percent, next_run_stability_score,
                   next_run_stability_score,
                   next_run_stability_delta, next_run_stability_score,
                   next_run_stability_score,
                   next_run_stability_delta, runs_to_good_stability,
                   runs_to_good_stability, runs_to_good_stability,
                   runs_to_good_days, runs_to_good_days,
                       runs_to_good_days, runs_to_good_days,
                       runs_to_good_days, correctness_findings, compiler_findings,
                     known_bug_replay_findings, perf_hotspots, perf_worst_ratio,
                     latest_full_pressure_perf_hotspots,
                     latest_full_pressure_perf_worst_ratio,
                     latest_full_pressure_perf_rows,
                     latest_full_pressure_perf_suite_current ? "true" : "false",
                     latest_full_pressure_ok ? "true" : "false",
                     latest_full_pressure_clean ? "true" : "false",
                     latest_full_pressure_failure_count,
                     latest_report_demoted_non_reproducing_afl_timeout ?
                         "true" : "false",
                     latest_full_pressure_demoted_non_reproducing_afl_timeout ?
                         "true" : "false",
                     perf_watchlist_open,
                       fuzz_all_perf_watchlist_threshold(),
                       non_reproducing_timeouts, non_reproducing_timeouts,
                       historical_non_reproducing_timeouts,
                       non_reproducing_timeouts,
                       historical_non_reproducing_timeouts,
                          gate_penalty, correctness_penalty,
                      perf_penalty, advisory_penalty, env_penalty,
                      freshness_penalty);
  append_fuzz_all_compact_score_alias_fields(
      &row, stability_score, stability_label, next_run_stability_score,
      latest_report_age_seconds, latest_report_stale_after_hours,
      latest_full_pressure_report_age_seconds,
      latest_full_pressure_report_stale_after_hours);
  append_fuzz_all_campaign_alias_fields(
      &row, thread_years, target_thread_years, remaining_thread_years,
      target_percent, runs_needed, wall_hours_needed, wall_days_needed,
      thread_years_per_run, target_percent_per_run, runs_per_day,
      thread_years_per_day, campaign_plan_wall_hours, campaign_plan_threads,
      completion_eta ? completion_eta : "");
  (void)sb_append(&row, ",\"campaign_first_report\":");
  (void)sb_append_json_str(&row, campaign_first_report ? campaign_first_report : "");
        (void)sb_append(&row, ",\"campaign_state\":");
  (void)sb_append_json_str(&row, fuzz_all_campaign_state(
                                     ready, target_reached,
                                     campaign_complete));
  (void)sb_append(&row, ",\"campaign_incomplete_reason\":");
  (void)sb_append_json_str(
      &row, fuzz_all_campaign_incomplete_reason(ready, blocker_count,
                                                active_items, target_reached,
                                                campaign_complete));
  (void)sb_append(&row, ",\"completion_state\":");
  (void)sb_append_json_str(&row, fuzz_all_campaign_state(
                                     ready, target_reached,
                                     campaign_complete));
  (void)sb_append(&row, ",\"completion_reason\":");
  (void)sb_append_json_str(
      &row, fuzz_all_campaign_incomplete_reason(ready, blocker_count,
                                                active_items, target_reached,
                                                campaign_complete));
    (void)sb_appendf(&row,
                     ",\"reports\":%.0f,"
                     "\"evidence_reports\":%.0f,"
                     "\"ignored_no_evidence_reports\":%.0f,"
                     "\"evidence_ignored_no_evidence_reports\":%.0f,"
                     "\"full_pressure_reports\":%.0f,"
                     "\"evidence_full_pressure_reports\":%.0f,"
                     "\"full_pressure_thread_years\":%.8f,"
                     "\"evidence_full_pressure_thread_years\":%.8f,"
                     "\"checked_subcases\":%.0f,"
                     "\"evidence_checked_subcases\":%.0f,"
                   "\"evidence_coverage_ran_lanes\":%.0f,"
                   "\"evidence_coverage_lanes\":%.0f,"
                   "\"coverage_depth_percent\":%.2f,"
                   "\"coverage_percent\":%.2f,"
                   "\"coverage_not_run_lanes\":%.0f,"
                   "\"evidence_coverage_depth_percent\":%.2f,"
                   "\"evidence_coverage_not_run_lanes\":%.0f,"
                   "\"coverage_detail_count\":%.0f,"
                   "\"coverage_backlog_lanes\":%.0f,"
                   "\"coverage_detail_rows\":%.0f",
                     evidence_reports, evidence_reports,
                     evidence_ignored_no_evidence_reports,
                     evidence_ignored_no_evidence_reports,
                     evidence_full_pressure_reports,
                     evidence_full_pressure_reports,
                     evidence_full_pressure_thread_years,
                     evidence_full_pressure_thread_years,
                     evidence_checked_subcases, evidence_checked_subcases,
                     evidence_coverage_ran_lanes,
                   evidence_coverage_lanes, evidence_coverage_depth_percent,
                   evidence_coverage_depth_percent,
                   evidence_coverage_not_run_lanes,
                   evidence_coverage_depth_percent,
                   evidence_coverage_not_run_lanes, coverage_detail_count,
                   coverage_backlog_lanes, coverage_detail_rows);
  (void)sb_appendf(&row,
                   ",\"coverage_queue_count\":%.0f,"
                   "\"coverage_queue_non_advisory_count\":%.0f,"
                   "\"coverage_queue_advisory_count\":%.0f",
                   coverage_queue_count,
                   coverage_queue_non_advisory_count,
                   coverage_queue_advisory_count);
  (void)sb_append(&row, ",\"coverage_queue_lanes\":");
  (void)sb_append_json_str(&row, coverage_queue_lanes ?
                                     coverage_queue_lanes : "");
  append_fuzz_all_coverage_queue_json(&row, coverage_queue_json);
  (void)sb_appendf(&row,
                   ",\"coverage_lanes\":%.0f,"
                   "\"coverage_ran_lanes\":%.0f,"
                   "\"coverage_skipped_lanes\":%.0f,"
                   "\"coverage_failed_lanes\":%.0f,"
                   "\"coverage_disabled_lanes\":%.0f,"
                   "\"coverage_budget_short_lanes\":%.0f,"
                   "\"coverage_missing_tool_lanes\":%.0f,"
                   "\"coverage_blocker_gaps\":%.0f,"
                   "\"coverage_advisory_gaps\":%.0f,"
                   "\"coverage_reports_considered\":%.0f,"
                   "\"coverage_campaign_reports_considered\":%.0f,"
                   "\"coverage_companion_reports_considered\":%.0f,"
                   "\"coverage_latest_report_advisory_gaps\":%.0f,"
                   "\"coverage_latest_report_companion_skipped_lanes\":%.0f",
                   evidence_coverage_lanes, evidence_coverage_ran_lanes,
                   evidence_coverage_skipped_lanes, coverage_failed_lanes,
                   coverage_disabled_lanes, coverage_budget_short_lanes,
                   coverage_missing_tool_lanes, coverage_blocker_gaps,
                   coverage_advisory_gaps, coverage_reports_considered,
                   coverage_campaign_reports_considered,
                   coverage_companion_reports_considered,
                   coverage_latest_report_advisory_gaps,
                   coverage_latest_report_companion_skipped_lanes);
  (void)sb_append(&row, ",\"coverage_state\":");
  (void)sb_append_json_str(
      &row, fuzz_all_coverage_state(evidence_coverage_lanes,
                                    evidence_coverage_ran_lanes,
                                    coverage_blocker_gaps,
                                    coverage_failed_lanes));
  (void)sb_append(&row, ",\"coverage_next_action\":");
  (void)sb_append_json_str(&row, have_coverage_next ?
                                     (coverage_next_action ?
                                          coverage_next_action :
                                          "run-missing-evidence") : "none");
  (void)sb_append(&row, ",\"coverage_next_category\":");
  (void)sb_append_json_str(&row, have_coverage_next && coverage_next_category ?
                                     coverage_next_category : "");
  (void)sb_append(&row, ",\"coverage_next_severity\":");
  (void)sb_append_json_str(&row, have_coverage_next && coverage_next_severity ?
                                     coverage_next_severity : "");
  (void)sb_append(&row, ",\"coverage_next_lane\":");
  (void)sb_append_json_str(&row, have_coverage_next && coverage_next_lane ?
                                     coverage_next_lane : "");
  (void)sb_append(&row, ",\"coverage_next_reason\":");
  (void)sb_append_json_str(&row, have_coverage_next && coverage_next_reason ?
                                     coverage_next_reason : "");
  (void)sb_append(&row, ",\"coverage_next_command\":");
  (void)sb_append_json_str(&row, have_coverage_next && coverage_next_command ?
                                     coverage_next_command : "");
  (void)sb_append(&row, ",\"coverage_next_guarded_command\":");
  (void)sb_append_json_str(&row, have_coverage_next &&
                                     coverage_next_guarded_command ?
                                     coverage_next_guarded_command : "");
  (void)sb_append(&row, ",\"coverage_next_low_cpu_command\":");
  (void)sb_append_json_str(&row, have_coverage_next &&
                                     coverage_next_low_cpu_command ?
                                     coverage_next_low_cpu_command : "");
  (void)sb_append(&row, ",\"coverage_next_preview_command\":");
  (void)sb_append_json_str(&row, have_coverage_next &&
                                     coverage_next_preview_command ?
                                     coverage_next_preview_command : "");
  (void)sb_append(&row, ",\"coverage_next_state_file\":");
  (void)sb_append_json_str(&row, have_coverage_next &&
                                     coverage_next_state_file ?
                                     coverage_next_state_file : "");
  (void)sb_append(&row, ",\"coverage_next_state_command\":");
  (void)sb_append_json_str(&row, have_coverage_next &&
                                     coverage_next_state_command ?
                                     coverage_next_state_command : "");
  (void)sb_append(&row, ",\"coverage_next_state_refresh_command\":");
  (void)sb_append_json_str(&row, have_coverage_next &&
                                     coverage_next_state_refresh_command ?
                                     coverage_next_state_refresh_command : "");
  (void)sb_append(&row, ",\"coverage_next_state_refresh_required\":");
  (void)sb_append(&row, have_coverage_next &&
                           coverage_next_state_refresh_required ?
                       "true" : "false");
  (void)sb_append(&row, ",\"coverage_next_state_refresh_reason\":");
  (void)sb_append_json_str(&row, have_coverage_next &&
                                     coverage_next_state_refresh_reason ?
                                     coverage_next_state_refresh_reason : "");
  (void)sb_append(&row, ",\"recommended_state_refresh_command\":");
  (void)sb_append_json_str(&row, recommended_state_refresh_command ?
                                     recommended_state_refresh_command : "");
  (void)sb_append(&row, ",\"coverage_next_stop_file\":");
  (void)sb_append_json_str(&row, have_coverage_next &&
                                     coverage_next_stop_file ?
                                     coverage_next_stop_file : "");
  (void)sb_append(&row, ",\"coverage_next_stop_command\":");
  (void)sb_append_json_str(&row, have_coverage_next &&
                                     coverage_next_stop_command ?
                                     coverage_next_stop_command : "");
  (void)sb_append(&row, ",\"coverage_next_resume_command\":");
  (void)sb_append_json_str(&row, have_coverage_next &&
                                     coverage_next_resume_command ?
                                     coverage_next_resume_command : "");
  append_fuzz_all_run_state_fields_prefixed(&row, "coverage_next_state",
                                            &coverage_next_run_state);
  (void)sb_appendf(&row, ",\"perf_worst_slowdown_percent\":%.2f",
                   fuzz_all_perf_slowdown_percent(perf_worst_ratio));
  (void)sb_append(&row, ",\"perf_worst_case\":");
  (void)sb_append_json_str(&row, perf_worst_case ? perf_worst_case : "");
  (void)sb_append(&row, ",\"latest_full_pressure_perf_max_case\":");
  (void)sb_append_json_str(&row, latest_full_pressure_perf_case ?
                                     latest_full_pressure_perf_case : "");
  (void)sb_appendf(
      &row, ",\"latest_full_pressure_perf_max_slowdown_percent\":%.2f",
      fuzz_all_perf_slowdown_percent(latest_full_pressure_perf_worst_ratio));
  (void)sb_append(&row, ",\"latest_full_pressure_clean_reason\":");
  (void)sb_append_json_str(&row, latest_full_pressure_clean_reason ?
                                     latest_full_pressure_clean_reason : "");
  (void)sb_append(&row, ",\"perf_watchlist_case\":");
  (void)sb_append_json_str(&row, perf_watchlist_case ? perf_watchlist_case : "");
  (void)sb_append(&row, ",\"advisory_state\":");
  (void)sb_append_json_str(
      &row, fuzz_all_advisory_state(non_reproducing_timeouts,
                                    historical_non_reproducing_timeouts));
  (void)sb_appendf(&row,
                   ",\"effective_advisory_timeouts\":%.0f,"
                   "\"advisory_effective_timeouts\":%.0f",
                   effective_advisory_timeouts,
                   effective_advisory_timeouts);
  (void)sb_append(&row, ",\"advisory_penalty_state\":");
  (void)sb_append_json_str(
      &row, fuzz_all_advisory_penalty_state(effective_advisory_timeouts));
  (void)sb_append(&row, ",\"thread_years_per_run_source\":");
  (void)sb_append_json_str(&row, thread_years_per_run_source);
  (void)sb_append(&row, ",\"stability_label\":");
  (void)sb_append_json_str(&row, stability_label ? stability_label : "");
  (void)sb_append(&row, ",\"language_score_label\":");
  (void)sb_append_json_str(&row, stability_label ? stability_label : "");
  (void)sb_append(&row, ",\"stability_note\":");
  (void)sb_append_json_str(&row, stability_note);
  (void)sb_append(&row, ",\"language_score_note\":");
  (void)sb_append_json_str(&row, stability_note);
  (void)sb_append(&row, ",\"completion_eta_local\":");
  (void)sb_append_json_str(&row, completion_eta ? completion_eta : "");
  (void)sb_append(&row, ",\"run_command\":");
  (void)sb_append_json_str(&row, run_command ? run_command : "");
  (void)sb_append(&row, ",\"status_command\":");
  (void)sb_append_json_str(&row, status_command ? status_command : "");
  (void)sb_append(&row, ",\"next_script\":");
  (void)sb_append_json_str(&row, next_script ? next_script : "");
  (void)sb_append(&row, ",\"progress_command\":");
  (void)sb_append_json_str(&row, progress_command ? progress_command : "");
  (void)sb_append(&row, ",\"next_handoff_command\":");
  (void)sb_append_json_str(&row, next_handoff_command ?
                                   next_handoff_command : "");
  (void)sb_append(&row, ",\"next_command\":");
  (void)sb_append_json_str(&row, next_command ? next_command : "");
  (void)sb_append(&row, ",\"preview_command\":");
  (void)sb_append_json_str(&row, selected_preview_command);
  (void)sb_append(&row, ",\"run_next_command\":");
  (void)sb_append_json_str(&row, next_command ? next_command : "");
  (void)sb_append(&row, ",\"run_next_preview_command\":");
  (void)sb_append_json_str(&row, selected_preview_command);
  (void)sb_append(&row, ",\"run_next_low_cpu_command\":");
  (void)sb_append_json_str(&row, next_command ? next_command : "");
  (void)sb_append(&row, ",\"run_next_gentle_command\":");
  (void)sb_append_json_str(&row, run_next_gentle_command);
  (void)sb_append(&row, ",\"run_next_gentle_preview_command\":");
  (void)sb_append_json_str(&row, run_next_gentle_preview_command);
  (void)sb_append(&row, ",\"stop_file\":");
  (void)sb_append_json_str(&row, stop_file ? stop_file : "");
  (void)sb_append(&row, ",\"stop_command\":");
  (void)sb_append_json_str(&row, stop_command ? stop_command : "");
  (void)sb_append(&row, ",\"resume_command\":");
  (void)sb_append_json_str(&row, resume_command ? resume_command : "");
  (void)sb_append(&row, ",\"state_file\":");
  (void)sb_append_json_str(&row, state_file ? state_file : "");
  (void)sb_append(&row, ",\"state_command\":");
  (void)sb_append_json_str(&row, state_command ? state_command : "");
  append_fuzz_all_probe_alias_fields(
      &row, root, progress_json_cmd[0] ? progress_json_cmd :
              "build/fuzz/all/progress.json",
      state_file ? state_file : "");
  (void)sb_append(&row, ",\"state_refresh_command\":");
  (void)sb_append_json_str(&row, selected_state_refresh_command);
  append_fuzz_all_run_state_fields(&row, &run_state);
  char progress_freshness_action_command[4096] = {0};
  if (freshness_penalty > 0.0)
    fuzz_all_freshness_action_command(progress_freshness_action_command,
                                      sizeof(progress_freshness_action_command),
                                      next_handoff_command);
  (void)sb_append(&row, ",\"freshness_action_command\":");
  (void)sb_append_json_str(&row, progress_freshness_action_command);
  (void)sb_append(&row, ",\"latest_report_freshness_command\":");
  (void)sb_append_json_str(&row,
                           !latest_report_fresh ?
                               progress_freshness_action_command : "");
  (void)sb_append(&row, ",\"latest_full_pressure_report_freshness_command\":");
  (void)sb_append_json_str(&row,
                           !latest_full_pressure_report_fresh ?
                               progress_freshness_action_command : "");
  (void)sb_append(&row, ",\"full_pressure_freshen_command\":");
  (void)sb_append_json_str(&row,
                           !latest_full_pressure_report_fresh ?
                               progress_freshness_action_command : "");
  (void)sb_append(&row, ",\"full_pressure_remediation_command\":");
  (void)sb_append_json_str(&row,
                           !latest_full_pressure_report_fresh ?
                               progress_freshness_action_command : "");
  (void)sb_append(&row, ",\"full_pressure_action_command\":");
  (void)sb_append_json_str(&row,
                           !latest_full_pressure_report_fresh ?
                               progress_freshness_action_command : "");
  (void)sb_append(&row, ",\"advisory_action_command\":");
  (void)sb_append_json_str(&row,
                           (non_reproducing_timeouts > 0.0 ||
                            historical_non_reproducing_timeouts > 0.0) &&
                                   advisory_action_command ?
                               advisory_action_command : "");
  (void)sb_append(&row, ",\"advisory_recheck_command\":");
  (void)sb_append_json_str(&row,
                           (non_reproducing_timeouts > 0.0 ||
                            historical_non_reproducing_timeouts > 0.0) &&
                                   advisory_recheck_command ?
                               advisory_recheck_command : "");
  bool progress_advisory_present =
      non_reproducing_timeouts > 0.0 ||
      historical_non_reproducing_timeouts > 0.0;
  (void)sb_append(&row, ",\"advisory_recheck_state\":");
  (void)sb_append_json_str(
      &row,
      fuzz_all_advisory_recheck_state(
          progress_advisory_present,
          advisory_recheck_raw_repro_checked,
          advisory_recheck_raw_repro_passed,
          advisory_recheck_raw_repro_timeouts,
          advisory_recheck_raw_repro_unexpected,
          advisory_recheck_command));
  (void)sb_appendf(
      &row,
      ",\"advisory_recheck_raw_repro_checked\":%.0f,"
      "\"advisory_recheck_raw_repro_passed\":%.0f,"
      "\"advisory_recheck_raw_repro_timeouts\":%.0f,"
      "\"advisory_recheck_raw_repro_unexpected\":%.0f",
      progress_advisory_present ? advisory_recheck_raw_repro_checked : 0.0,
      progress_advisory_present ? advisory_recheck_raw_repro_passed : 0.0,
      progress_advisory_present ? advisory_recheck_raw_repro_timeouts : 0.0,
      progress_advisory_present ? advisory_recheck_raw_repro_unexpected : 0.0);
  (void)sb_append(&row, ",\"perf_watchlist_command\":");
  (void)sb_append_json_str(&row, perf_watchlist_command ?
                                     perf_watchlist_command : "");
  (void)sb_append(&row, ",\"perf_watchlist_state\":");
  (void)sb_append_json_str(&row, perf_watchlist_state);
  (void)sb_append(&row, ",\"perf_watchlist_action\":");
  (void)sb_append_json_str(&row, perf_watchlist_action);
  (void)sb_append(&row, ",\"perf_watchlist_action_command\":");
  (void)sb_append_json_str(&row, perf_watchlist_action_command ?
                                     perf_watchlist_action_command : "");
  (void)sb_append(&row, ",\"optimization_action\":");
  (void)sb_append_json_str(&row, perf_watchlist_action);
  (void)sb_append(&row, ",\"optimization_reason\":");
  (void)sb_append_json_str(&row, optimization_reason);
    (void)sb_append(&row, ",\"optimization_command\":");
    (void)sb_append_json_str(&row, perf_watchlist_action_command ?
                                       perf_watchlist_action_command : "");
  (void)sb_append(&row, ",\"optimization_target_command\":");
  (void)sb_append_json_str(&row, optimization_target_command ?
                                     optimization_target_command : "");
    (void)sb_append(&row, ",\"optimization_case\":");
    (void)sb_append_json_str(&row, optimization_case);
  (void)sb_append(&row, ",\"optimization_artifact\":");
  (void)sb_append_json_str(&row, optimization_artifact);
  (void)sb_append(&row, ",\"optimization_ny_source\":");
  (void)sb_append_json_str(&row, optimization_ny_source);
  (void)sb_append(&row, ",\"optimization_c_source\":");
  (void)sb_append_json_str(&row, optimization_c_source);
    (void)sb_appendf(&row,
                     ",\"optimization_ratio\":%.4f,"
                     "\"optimization_slowdown_percent\":%.2f",
                   optimization_ratio,
                   fuzz_all_perf_slowdown_percent(optimization_ratio));
  (void)sb_append(&row, ",\"perf_watchlist_report\":");
  (void)sb_append_json_str(&row, perf_watchlist_report ?
                                     perf_watchlist_report : "");
  (void)sb_append(&row, ",\"perf_watchlist_markdown\":");
  (void)sb_append_json_str(&row, perf_watchlist_markdown ?
                                     perf_watchlist_markdown : "");
  (void)sb_append(&row, ",\"perf_watchlist_artifact_readable\":");
  (void)sb_append(&row, perf_watchlist_artifact_readable ? "true" : "false");
  (void)sb_append(&row, ",\"perf_watchlist_artifact_fresh\":");
  (void)sb_append(&row, perf_watchlist_artifact_fresh ? "true" : "false");
  (void)sb_appendf(&row,
                   ",\"perf_watchlist_artifact_hotspots\":%.0f,"
                   "\"perf_watchlist_artifact_max_ratio\":%.4f,"
                   "\"perf_watchlist_artifact_max_slowdown_percent\":%.2f,"
                   "\"perf_watchlist_artifact_age_seconds\":%.0f,"
                   "\"perf_watchlist_artifact_stale_after_hours\":%.2f",
                   perf_watchlist_artifact_hotspots,
                   perf_watchlist_artifact_max_ratio,
                   fuzz_all_perf_slowdown_percent(
                       perf_watchlist_artifact_max_ratio),
                   perf_watchlist_artifact_age_seconds,
                   perf_watchlist_artifact_stale_after_hours);
    (void)sb_append(&row, ",\"perf_watchlist_artifact_max_case\":");
    (void)sb_append_json_str(&row, perf_watchlist_artifact_max_case ?
                                       perf_watchlist_artifact_max_case : "");
  (void)sb_append(&row, ",\"perf_watchlist_artifact_max_artifact\":");
  (void)sb_append_json_str(&row, perf_watchlist_artifact_max_artifact ?
                                     perf_watchlist_artifact_max_artifact : "");
  (void)sb_append(&row, ",\"perf_watchlist_artifact_max_ny_source\":");
  (void)sb_append_json_str(&row, perf_watchlist_artifact_max_ny_source ?
                                     perf_watchlist_artifact_max_ny_source : "");
  (void)sb_append(&row, ",\"perf_watchlist_artifact_max_c_source\":");
  (void)sb_append_json_str(&row, perf_watchlist_artifact_max_c_source ?
                                     perf_watchlist_artifact_max_c_source : "");
  (void)sb_append(&row, ",\"recommended_action\":");
  (void)sb_append_json_str(&row, recommendation.action);
  (void)sb_append(&row, ",\"recommended_reason\":");
  (void)sb_append_json_str(&row, recommendation.reason);
  (void)sb_append(&row, ",\"recommended_command\":");
  (void)sb_append_json_str(&row, recommendation.command);
  (void)sb_append(&row, ",\"recommended_low_cpu_command\":");
  (void)sb_append_json_str(&row, recommendation.low_cpu_command);
  (void)sb_append(&row, ",\"recommended_preview_command\":");
  (void)sb_append_json_str(&row, recommendation.preview_command);
  (void)sb_append(&row, ",\"recommended_repeat_mode\":");
  (void)sb_append_json_str(&row, recommendation.repeat_mode);
  (void)sb_appendf(&row, ",\"recommended_repeat_count\":%.0f",
                   recommendation.repeat_count);
  append_fuzz_all_recommended_state_fields(
      &row, recommendation.action, &run_state, &coverage_next_run_state,
      state_file, state_command, state_refresh_command,
      recommendation.preview_command,
      have_coverage_next ? coverage_next_state_file : "",
      have_coverage_next ? coverage_next_state_command : "",
      have_coverage_next ? coverage_next_state_refresh_command : "",
      have_coverage_next && coverage_next_state_refresh_required,
      have_coverage_next ? coverage_next_state_refresh_reason : "");
  append_fuzz_all_handoff_guard_fields(&row);
  (void)sb_append(&row, ",\"tmp_dir\":");
  (void)sb_append_json_str(&row, tmp_dir ? tmp_dir : "");
  (void)sb_append(&row, ",\"scratch_root\":");
  (void)sb_append_json_str(&row, scratch_root ? scratch_root : "");
  (void)sb_append(&row, ",\"xdg_cache_home\":");
  (void)sb_append_json_str(&row, xdg_cache_home ? xdg_cache_home : "");
  (void)sb_append(&row, ",\"nytrix_cache_dir\":");
  (void)sb_append_json_str(&row, nytrix_cache_dir ? nytrix_cache_dir : "");
  (void)sb_append(&row, ",\"old_path_probe_command\":");
  (void)sb_append_json_str(&row, NYTRIX_FUZZ_ALL_OLD_PATH_PROBE_COMMAND);
  (void)sb_append(&row, ",\"old_path_command\":");
  (void)sb_append_json_str(&row, old_path_command ? old_path_command : "");
  (void)sb_append(&row, ",\"old_path_dry_run_command\":");
  (void)sb_append_json_str(&row, old_path_dry_run_command ?
                                     old_path_dry_run_command : "");
  (void)sb_append(&row, ",\"old_path_apply_command\":");
  (void)sb_append_json_str(&row, old_path_apply_command ?
                                     old_path_apply_command : "");
  (void)sb_append(&row, ",\"old_path_next_action\":");
  (void)sb_append_json_str(&row, old_path_next_action ?
                                     old_path_next_action : "none");
  (void)sb_append(&row, ",\"old_path_next_reason\":");
  (void)sb_append_json_str(&row, old_path_next_reason ?
                                     old_path_next_reason : "");
  (void)sb_append(&row, ",\"old_path_report\":");
  (void)sb_append_json_str(&row, old_path_report ? old_path_report : "");
  (void)sb_append(&row, ",\"old_path_markdown\":");
  (void)sb_append_json_str(&row, old_path_markdown ? old_path_markdown : "");
  (void)sb_appendf(&row,
                   ",\"old_path_cache_policy_ok\":%s,"
                   "\"old_path_present_count\":%.0f,"
                   "\"old_path_moved_count\":%.0f,"
                   "\"old_path_remaining_count\":%.0f,"
                   "\"old_path_wait_remaining_seconds\":%.0f,"
                   "\"old_path_artifact_leak_count\":%.0f,"
                   "\"old_path_artifact_moved_count\":%.0f,"
                   "\"old_path_artifact_remaining_count\":%.0f",
                   old_path_cache_policy_ok ? "true" : "false",
                   old_path_present_count,
                   old_path_moved_count,
                   old_path_remaining_count,
                   old_path_wait_remaining_seconds,
                   old_path_artifact_leak_count,
                   old_path_artifact_moved_count,
                   old_path_artifact_remaining_count);
  (void)sb_append(&row, ",\"latest_report\":");
  (void)sb_append_json_str(&row, latest_report ? latest_report : "");
  (void)sb_append(&row, ",\"latest_full_pressure_report\":");
  (void)sb_append_json_str(&row, latest_full_pressure_report ?
                                 latest_full_pressure_report : "");
  (void)sb_append(&row, ",\"engine\":\"nytrix_core\"}");
  (void)string_list_push_take(&rows, sb_take(&row));

  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"status_report\":");
  append_rel_json_str(&extra, root, status_path ? status_path : "");
  (void)sb_append(&extra, ",\"worklist_report\":");
  append_rel_json_str(&extra, root, worklist_path ? worklist_path : "");
  (void)sb_append(&extra, ",\"worklist_markdown\":");
  append_rel_json_str(&extra, root,
                      worklist_markdown_path ? worklist_markdown_path : "");
  (void)sb_append(&extra, ",\"historical_worklist_report\":");
  append_rel_json_str(&extra, root,
                      worklist_history_path ? worklist_history_path : "");
  (void)sb_append(&extra, ",\"historical_worklist_markdown\":");
  append_rel_json_str(&extra, root,
                      worklist_history_markdown_path ?
                          worklist_history_markdown_path : "");
  (void)sb_append(&extra, ",\"coverage_report\":");
  append_rel_json_str(&extra, root, coverage_path ? coverage_path : "");
  (void)sb_append(&extra, ",\"coverage_markdown\":");
  append_rel_json_str(&extra, root,
                      coverage_markdown_path ? coverage_markdown_path : "");
  (void)sb_append(&extra, ",\"plan_report\":");
  append_rel_json_str(&extra, root, plan_path ? plan_path : "");
  (void)sb_append(&extra, ",\"plan_markdown\":");
  append_rel_json_str(&extra, root, plan_markdown_path ? plan_markdown_path : "");
  append_fuzz_all_probe_alias_fields(
      &extra, root, progress_json_cmd[0] ? progress_json_cmd :
                  "build/fuzz/all/progress.json",
      state_file ? state_file : "");
  append_fuzz_all_compiler_std_audit_values(
      &extra, compiler_std_audit_readable, compiler_std_audit_report,
      compiler_std_audit_markdown, compiler_std_audit_command,
      runtime_surface_state, runtime_exports, direct_runtime_refs,
      runtime_export_coverage_percent,
      runtime_unreferenced_percent, runtime_unreferenced_count,
      runtime_wrapper_gap_count, crt_surface_state, crt_runtime_exports,
      crt_direct_refs, crt_export_coverage_percent,
      crt_unreferenced_percent, crt_unreferenced_count, crt_wrapper_gap_count,
      crt_unreferenced_family_count, crt_top_unreferenced_family,
      crt_top_unreferenced_family_count, crt_unreferenced_families,
      crt_next_action, crt_next_reason, crt_next_unreferenced_family,
      crt_next_unreferenced_count, crt_next_unreferenced_exports,
      crt_next_definition_file, crt_next_definition_locations,
      crt_next_inspect_command);
  (void)sb_appendf(&extra, ",\"blockers\":%.0f,\"active_runs\":%.0f",
                   blocker_count, active_items);
  (void)sb_appendf(&extra,
                     ",\"target_percent\":%.4f,"
                     "\"campaign_percent\":%.4f,"
                     "\"campaign_confidence_percent\":%.4f,"
                     "\"campaign_remaining_percent\":%.4f,"
                     "\"remaining_percent\":%.4f,"
                      "\"thread_years\":%.8f,"
                      "\"campaign_first_report_epoch\":%.0f,"
                      "\"campaign_latest_report_epoch\":%.0f,"
                      "\"campaign_calendar_span_days\":%.4f,"
                      "\"campaign_calendar_age_days\":%.4f,"
                      "\"campaign_calendar_percent_10y\":%.4f,"
                      "\"target_thread_years\":%.8f,"
                   "\"remaining_thread_years\":%.8f,"
                   "\"runs_needed\":%.0f,"
                   "\"runs_per_day\":%.4f,"
                   "\"wall_hours_needed\":%.4f,"
                   "\"thread_hours_needed\":%.4f,"
                   "\"wall_days_needed\":%.4f,"
                   "\"status_age_seconds\":%.0f,"
                   "\"status_age_minutes\":%.2f,"
                   "\"latest_report_age_seconds\":%.0f,"
                   "\"latest_report_age_hours\":%.2f,"
                   "\"latest_full_pressure_report_age_seconds\":%.0f,"
                   "\"latest_full_pressure_report_age_hours\":%.2f,"
                   "\"latest_report_stale_after_hours\":%.2f,"
                   "\"latest_report_freshness_remaining_hours\":%.2f,"
                   "\"latest_report_freshness_overdue_hours\":%.2f,"
                     "\"latest_report_fresh\":%s,"
                     "\"latest_full_pressure_report_stale_after_hours\":%.2f,"
                     "\"latest_full_pressure_report_freshness_remaining_hours\":%.2f,"
                     "\"latest_full_pressure_report_freshness_overdue_hours\":%.2f,"
                     "\"latest_full_pressure_report_fresh\":%s,"
                     "\"evidence_fresh\":%s,"
                     "\"evidence_freshness_overdue_hours\":%.2f,"
                     "\"strict\":%s,"
                     "\"allow_incomplete_coverage\":%s,"
                     "\"allow_full_pressure_remediation\":%s,"
                     "\"ready\":%s,"
                   "\"blocker_count\":%.0f,"
                   "\"active_items\":%.0f,"
                   "\"active_count\":%.0f,"
                   "\"historical_attention_reports\":%.0f,"
                   "\"target_reached\":%s,"
                   "\"campaign_complete\":%s,"
                   "\"cache_policy_ok\":%s,"
                   "\"ny_bin_exists\":%s,"
                   "\"old_nytrix_test_scratch_absent\":%s,"
                   "\"old_nytrix_fuzz_absent\":%s,"
                   "\"old_nytrix_build_cache_absent\":%s,"
                   "\"active_old_nytrix_output_writer_present\":%s,"
                   "\"stability_score_percent\":%.2f,"
                   "\"stability_percent\":%.2f,"
                   "\"score_percent\":%.2f,"
                   "\"stability_score\":%.2f,"
                   "\"language_score_percent\":%.2f,"
                   "\"language_score\":%.2f,"
                   "\"language_score_good_threshold_percent\":%.2f,"
                   "\"language_score_gap_percent\":%.2f,"
                   "\"signal_health_percent\":%.2f,"
                   "\"evidence_cap_percent\":%.2f,"
                   "\"language_score_signal_percent\":%.2f,"
                   "\"language_score_evidence_cap_percent\":%.2f,"
                   "\"thread_years_per_run\":%.8f,"
                   "\"target_percent_per_run\":%.4f,"
                   "\"next_run_target_percent\":%.4f,"
                   "\"next_run_stability_score_percent\":%.2f,"
                   "\"next_run_stability_score\":%.2f,"
                   "\"next_run_stability_delta_percent\":%.2f,"
                   "\"next_run_language_score_percent\":%.2f,"
                   "\"next_run_language_score\":%.2f,"
                   "\"next_run_language_score_delta_percent\":%.2f,"
                   "\"runs_to_good_stability\":%.0f,"
                   "\"runs_to_good_stability_score\":%.0f,"
                   "\"runs_to_good_language_score\":%.0f,"
                   "\"runs_to_good_days\":%.4f,"
                   "\"runs_to_good_stability_days\":%.4f,"
                   "\"runs_to_good_language_days\":%.4f,"
                   "\"days_to_good_stability\":%.4f,"
                   "\"days_to_good_language_score\":%.4f,"
                   "\"correctness_findings\":%.0f,"
                   "\"compiler_findings\":%.0f,"
                     "\"known_bug_replay_findings\":%.0f,"
                     "\"perf_hotspots_open\":%.0f,"
                     "\"perf_worst_ratio\":%.4f,"
                     "\"latest_full_pressure_perf_hotspots\":%.0f,"
                     "\"latest_full_pressure_perf_max_ratio\":%.4f,"
                     "\"latest_full_pressure_perf_rows\":%.0f,"
                     "\"latest_full_pressure_perf_suite_current\":%s,"
                     "\"latest_full_pressure_ok\":%s,"
                     "\"latest_full_pressure_clean\":%s,"
                     "\"latest_full_pressure_failure_count\":%.0f,"
                     "\"latest_report_demoted_non_reproducing_afl_timeout\":%s,"
                     "\"latest_full_pressure_demoted_non_reproducing_afl_timeout\":%s,"
                     "\"perf_watchlist_open\":%.0f,"
                       "\"perf_watchlist_threshold_ratio\":%.4f,"
                       "\"advisory_timeouts\":%.0f,"
                     "\"current_advisory_timeouts\":%.0f,"
                     "\"historical_advisory_timeouts\":%.0f,"
                     "\"non_reproducing_afl_timeouts\":%.0f,"
                     "\"historical_non_reproducing_afl_timeouts\":%.0f,"
                     "\"gate_penalty\":%.2f,"
                   "\"correctness_penalty\":%.2f,"
                   "\"perf_penalty\":%.2f,"
                   "\"advisory_penalty\":%.2f,"
                     "\"environment_penalty\":%.2f,"
                     "\"freshness_penalty\":%.2f,"
                     "\"status_refresh_rc\":%d,"
                     "\"status_refresh_attempted\":%s,"
                     "\"refreshed\":%s",
                     target_percent, target_percent, target_percent,
                     fuzz_all_campaign_remaining_percent(target_percent),
                     fuzz_all_campaign_remaining_percent(target_percent),
                      thread_years, campaign_first_report_epoch,
                      campaign_latest_report_epoch,
                      campaign_calendar_span_days,
                      campaign_calendar_age_days,
                      fuzz_all_campaign_calendar_percent_10y(
                          campaign_calendar_age_days),
                      target_thread_years,
                   remaining_thread_years, runs_needed, runs_per_day,
                   wall_hours_needed, thread_hours_needed,
                   wall_days_needed, status_age_seconds,
                   status_age_seconds >= 0.0 ? status_age_seconds / 60.0 : -1.0,
                   latest_report_age_seconds,
                   latest_report_age_seconds >= 0.0 ?
                       latest_report_age_seconds / 3600.0 : -1.0,
                   latest_full_pressure_report_age_seconds,
                   latest_full_pressure_report_age_seconds >= 0.0 ?
                       latest_full_pressure_report_age_seconds / 3600.0 : -1.0,
                   latest_report_stale_after_hours,
                   fuzz_all_freshness_remaining_hours(
                       latest_report_age_seconds,
                       latest_report_stale_after_hours),
                   fuzz_all_freshness_overdue_hours(
                       latest_report_age_seconds,
                       latest_report_stale_after_hours),
                   latest_report_fresh ? "true" : "false",
                     latest_full_pressure_report_stale_after_hours,
                     fuzz_all_freshness_remaining_hours(
                         latest_full_pressure_report_age_seconds,
                         latest_full_pressure_report_stale_after_hours),
                     fuzz_all_freshness_overdue_hours(
                         latest_full_pressure_report_age_seconds,
                         latest_full_pressure_report_stale_after_hours),
                     latest_full_pressure_report_fresh ? "true" : "false",
                     evidence_fresh ? "true" : "false",
                     fuzz_all_evidence_freshness_overdue_hours(
                         latest_report_age_seconds,
                         latest_report_stale_after_hours,
                         latest_full_pressure_report_age_seconds,
                         latest_full_pressure_report_stale_after_hours),
                     strict ? "true" : "false",
                     allow_incomplete_coverage ? "true" : "false",
                     allow_full_pressure_remediation ? "true" : "false",
                     ready ? "true" : "false", blocker_count,
                   active_items, active_items, historical_attention_reports,
                   target_reached ? "true" : "false",
                   campaign_complete ? "true" : "false",
                   cache_policy_ok ? "true" : "false",
                   ny_bin_exists ? "true" : "false",
                   old_nytrix_test_scratch_absent ? "true" : "false",
                   old_nytrix_fuzz_absent ? "true" : "false",
                   old_nytrix_build_cache_absent ? "true" : "false",
                   active_old_nytrix_writer_present ? "true" : "false",
                   stability_score, stability_score,
                   stability_score, stability_score,
                   stability_score, stability_score,
                   fuzz_all_progress_good_threshold(),
                   fuzz_all_language_good_gap_percent(stability_score),
                   signal_score,
                   evidence_cap,
                   signal_score,
                   evidence_cap,
                   thread_years_per_run, target_percent_per_run,
                   next_run_target_percent, next_run_stability_score,
                   next_run_stability_score,
                   next_run_stability_delta, next_run_stability_score,
                   next_run_stability_score,
                   next_run_stability_delta, runs_to_good_stability,
                   runs_to_good_stability, runs_to_good_stability,
                   runs_to_good_days, runs_to_good_days,
                       runs_to_good_days, runs_to_good_days,
                       runs_to_good_days, correctness_findings, compiler_findings,
                     known_bug_replay_findings, perf_hotspots, perf_worst_ratio,
                     latest_full_pressure_perf_hotspots,
                     latest_full_pressure_perf_worst_ratio,
                     latest_full_pressure_perf_rows,
                     latest_full_pressure_perf_suite_current ? "true" : "false",
                     latest_full_pressure_ok ? "true" : "false",
                     latest_full_pressure_clean ? "true" : "false",
                     latest_full_pressure_failure_count,
                     latest_report_demoted_non_reproducing_afl_timeout ?
                         "true" : "false",
                     latest_full_pressure_demoted_non_reproducing_afl_timeout ?
                         "true" : "false",
                     perf_watchlist_open,
                     fuzz_all_perf_watchlist_threshold(),
                       non_reproducing_timeouts, non_reproducing_timeouts,
                     historical_non_reproducing_timeouts,
                     non_reproducing_timeouts,
                     historical_non_reproducing_timeouts,
                     gate_penalty, correctness_penalty,
                   perf_penalty, advisory_penalty, env_penalty,
                   freshness_penalty, status_rc, refresh ? "true" : "false",
                   refresh ? "true" : "false");
  append_fuzz_all_compact_score_alias_fields(
      &extra, stability_score, stability_label, next_run_stability_score,
      latest_report_age_seconds, latest_report_stale_after_hours,
      latest_full_pressure_report_age_seconds,
      latest_full_pressure_report_stale_after_hours);
  append_fuzz_all_campaign_alias_fields(
      &extra, thread_years, target_thread_years, remaining_thread_years,
      target_percent, runs_needed, wall_hours_needed, wall_days_needed,
      thread_years_per_run, target_percent_per_run, runs_per_day,
      thread_years_per_day, campaign_plan_wall_hours, campaign_plan_threads,
      completion_eta ? completion_eta : "");
  (void)sb_append(&extra, ",\"campaign_first_report\":");
  (void)sb_append_json_str(&extra, campaign_first_report ? campaign_first_report : "");
     (void)sb_append(&extra, ",\"campaign_state\":");
  (void)sb_append_json_str(&extra, fuzz_all_campaign_state(
                                      ready, target_reached,
                                      campaign_complete));
  (void)sb_append(&extra, ",\"campaign_incomplete_reason\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_campaign_incomplete_reason(ready, blocker_count,
                                                  active_items, target_reached,
                                                  campaign_complete));
  (void)sb_append(&extra, ",\"completion_state\":");
  (void)sb_append_json_str(&extra, fuzz_all_campaign_state(
                                      ready, target_reached,
                                      campaign_complete));
  (void)sb_append(&extra, ",\"completion_reason\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_campaign_incomplete_reason(ready, blocker_count,
                                                  active_items, target_reached,
                                                  campaign_complete));
    (void)sb_appendf(&extra,
                     ",\"reports\":%.0f,"
                     "\"evidence_reports\":%.0f,"
                     "\"ignored_no_evidence_reports\":%.0f,"
                     "\"evidence_ignored_no_evidence_reports\":%.0f,"
                     "\"full_pressure_reports\":%.0f,"
                     "\"evidence_full_pressure_reports\":%.0f,"
                     "\"full_pressure_thread_years\":%.8f,"
                     "\"evidence_full_pressure_thread_years\":%.8f,"
                     "\"checked_subcases\":%.0f,"
                     "\"evidence_checked_subcases\":%.0f,"
                   "\"evidence_coverage_ran_lanes\":%.0f,"
                   "\"evidence_coverage_lanes\":%.0f,"
                   "\"coverage_depth_percent\":%.2f,"
                   "\"coverage_percent\":%.2f,"
                   "\"coverage_not_run_lanes\":%.0f,"
                   "\"evidence_coverage_depth_percent\":%.2f,"
                   "\"evidence_coverage_not_run_lanes\":%.0f,"
                   "\"coverage_detail_count\":%.0f,"
                   "\"coverage_backlog_lanes\":%.0f,"
                   "\"coverage_detail_rows\":%.0f",
                     evidence_reports, evidence_reports,
                     evidence_ignored_no_evidence_reports,
                     evidence_ignored_no_evidence_reports,
                     evidence_full_pressure_reports,
                     evidence_full_pressure_reports,
                     evidence_full_pressure_thread_years,
                     evidence_full_pressure_thread_years,
                     evidence_checked_subcases, evidence_checked_subcases,
                     evidence_coverage_ran_lanes,
                   evidence_coverage_lanes, evidence_coverage_depth_percent,
                   evidence_coverage_depth_percent,
                   evidence_coverage_not_run_lanes,
                   evidence_coverage_depth_percent,
                   evidence_coverage_not_run_lanes, coverage_detail_count,
                   coverage_backlog_lanes, coverage_detail_rows);
  (void)sb_appendf(&extra,
                   ",\"coverage_queue_count\":%.0f,"
                   "\"coverage_queue_non_advisory_count\":%.0f,"
                   "\"coverage_queue_advisory_count\":%.0f",
                   coverage_queue_count,
                   coverage_queue_non_advisory_count,
                   coverage_queue_advisory_count);
  (void)sb_append(&extra, ",\"coverage_queue_lanes\":");
  (void)sb_append_json_str(&extra, coverage_queue_lanes ?
                                      coverage_queue_lanes : "");
  append_fuzz_all_coverage_queue_json(&extra, coverage_queue_json);
  (void)sb_appendf(&extra,
                   ",\"coverage_lanes\":%.0f,"
                   "\"coverage_ran_lanes\":%.0f,"
                   "\"coverage_skipped_lanes\":%.0f,"
                   "\"coverage_failed_lanes\":%.0f,"
                   "\"coverage_disabled_lanes\":%.0f,"
                   "\"coverage_budget_short_lanes\":%.0f,"
                   "\"coverage_missing_tool_lanes\":%.0f,"
                   "\"coverage_blocker_gaps\":%.0f,"
                   "\"coverage_advisory_gaps\":%.0f,"
                   "\"coverage_reports_considered\":%.0f,"
                   "\"coverage_campaign_reports_considered\":%.0f,"
                   "\"coverage_companion_reports_considered\":%.0f,"
                   "\"coverage_latest_report_advisory_gaps\":%.0f,"
                   "\"coverage_latest_report_companion_skipped_lanes\":%.0f",
                   evidence_coverage_lanes, evidence_coverage_ran_lanes,
                   evidence_coverage_skipped_lanes, coverage_failed_lanes,
                   coverage_disabled_lanes, coverage_budget_short_lanes,
                   coverage_missing_tool_lanes, coverage_blocker_gaps,
                   coverage_advisory_gaps, coverage_reports_considered,
                   coverage_campaign_reports_considered,
                   coverage_companion_reports_considered,
                   coverage_latest_report_advisory_gaps,
                   coverage_latest_report_companion_skipped_lanes);
  (void)sb_append(&extra, ",\"coverage_state\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_coverage_state(evidence_coverage_lanes,
                                      evidence_coverage_ran_lanes,
                                      coverage_blocker_gaps,
                                      coverage_failed_lanes));
  (void)sb_append(&extra, ",\"coverage_next_action\":");
  (void)sb_append_json_str(&extra, have_coverage_next ?
                                      (coverage_next_action ?
                                           coverage_next_action :
                                           "run-missing-evidence") : "none");
  (void)sb_append(&extra, ",\"coverage_next_category\":");
  (void)sb_append_json_str(&extra, have_coverage_next && coverage_next_category ?
                                      coverage_next_category : "");
  (void)sb_append(&extra, ",\"coverage_next_severity\":");
  (void)sb_append_json_str(&extra, have_coverage_next && coverage_next_severity ?
                                      coverage_next_severity : "");
  (void)sb_append(&extra, ",\"coverage_next_lane\":");
  (void)sb_append_json_str(&extra, have_coverage_next && coverage_next_lane ?
                                      coverage_next_lane : "");
  (void)sb_append(&extra, ",\"coverage_next_reason\":");
  (void)sb_append_json_str(&extra, have_coverage_next && coverage_next_reason ?
                                      coverage_next_reason : "");
  (void)sb_append(&extra, ",\"coverage_next_command\":");
  (void)sb_append_json_str(&extra, have_coverage_next && coverage_next_command ?
                                      coverage_next_command : "");
  (void)sb_append(&extra, ",\"coverage_next_guarded_command\":");
  (void)sb_append_json_str(&extra, have_coverage_next &&
                                      coverage_next_guarded_command ?
                                      coverage_next_guarded_command : "");
  (void)sb_append(&extra, ",\"coverage_next_low_cpu_command\":");
  (void)sb_append_json_str(&extra, have_coverage_next &&
                                      coverage_next_low_cpu_command ?
                                      coverage_next_low_cpu_command : "");
  (void)sb_append(&extra, ",\"coverage_next_preview_command\":");
  (void)sb_append_json_str(&extra, have_coverage_next &&
                                      coverage_next_preview_command ?
                                      coverage_next_preview_command : "");
  (void)sb_append(&extra, ",\"coverage_next_state_file\":");
  (void)sb_append_json_str(&extra, have_coverage_next &&
                                      coverage_next_state_file ?
                                      coverage_next_state_file : "");
  (void)sb_append(&extra, ",\"coverage_next_state_command\":");
  (void)sb_append_json_str(&extra, have_coverage_next &&
                                      coverage_next_state_command ?
                                      coverage_next_state_command : "");
  (void)sb_append(&extra, ",\"coverage_next_state_refresh_command\":");
  (void)sb_append_json_str(&extra, have_coverage_next &&
                                      coverage_next_state_refresh_command ?
                                      coverage_next_state_refresh_command : "");
  (void)sb_append(&extra, ",\"coverage_next_state_refresh_required\":");
  (void)sb_append(&extra, have_coverage_next &&
                             coverage_next_state_refresh_required ?
                         "true" : "false");
  (void)sb_append(&extra, ",\"coverage_next_state_refresh_reason\":");
  (void)sb_append_json_str(&extra, have_coverage_next &&
                                      coverage_next_state_refresh_reason ?
                                      coverage_next_state_refresh_reason : "");
  (void)sb_append(&extra, ",\"recommended_state_refresh_command\":");
  (void)sb_append_json_str(&extra, recommended_state_refresh_command ?
                                      recommended_state_refresh_command : "");
  (void)sb_append(&extra, ",\"coverage_next_stop_file\":");
  (void)sb_append_json_str(&extra, have_coverage_next &&
                                      coverage_next_stop_file ?
                                      coverage_next_stop_file : "");
  (void)sb_append(&extra, ",\"coverage_next_stop_command\":");
  (void)sb_append_json_str(&extra, have_coverage_next &&
                                      coverage_next_stop_command ?
                                      coverage_next_stop_command : "");
  (void)sb_append(&extra, ",\"coverage_next_resume_command\":");
  (void)sb_append_json_str(&extra, have_coverage_next &&
                                      coverage_next_resume_command ?
                                      coverage_next_resume_command : "");
  append_fuzz_all_run_state_fields_prefixed(&extra, "coverage_next_state",
                                            &coverage_next_run_state);
  (void)sb_appendf(&extra, ",\"perf_worst_slowdown_percent\":%.2f",
                   fuzz_all_perf_slowdown_percent(perf_worst_ratio));
  (void)sb_append(&extra, ",\"perf_worst_case\":");
  (void)sb_append_json_str(&extra, perf_worst_case ? perf_worst_case : "");
  (void)sb_append(&extra, ",\"latest_full_pressure_perf_max_case\":");
  (void)sb_append_json_str(&extra, latest_full_pressure_perf_case ?
                                      latest_full_pressure_perf_case : "");
  (void)sb_appendf(
      &extra, ",\"latest_full_pressure_perf_max_slowdown_percent\":%.2f",
      fuzz_all_perf_slowdown_percent(latest_full_pressure_perf_worst_ratio));
  (void)sb_append(&extra, ",\"latest_full_pressure_clean_reason\":");
  (void)sb_append_json_str(&extra, latest_full_pressure_clean_reason ?
                                      latest_full_pressure_clean_reason : "");
  (void)sb_appendf(&extra,
                   ",\"latest_full_pressure_raw_ok\":%s,"
                   "\"latest_full_pressure_effective_clean\":%s",
                   latest_full_pressure_ok ? "true" : "false",
                   latest_full_pressure_clean ? "true" : "false");
  (void)sb_append(&extra, ",\"perf_watchlist_case\":");
  (void)sb_append_json_str(&extra, perf_watchlist_case ? perf_watchlist_case : "");
  (void)sb_append(&extra, ",\"advisory_state\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_advisory_state(non_reproducing_timeouts,
                                      historical_non_reproducing_timeouts));
  (void)sb_appendf(&extra,
                   ",\"effective_advisory_timeouts\":%.0f,"
                   "\"advisory_effective_timeouts\":%.0f",
                   effective_advisory_timeouts,
                   effective_advisory_timeouts);
  (void)sb_append(&extra, ",\"advisory_penalty_state\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_advisory_penalty_state(effective_advisory_timeouts));
  (void)sb_append(&extra, ",\"thread_years_per_run_source\":");
  (void)sb_append_json_str(&extra, thread_years_per_run_source);
  (void)sb_append(&extra, ",\"stability_label\":");
  (void)sb_append_json_str(&extra, stability_label ? stability_label : "");
  (void)sb_append(&extra, ",\"language_score_label\":");
  (void)sb_append_json_str(&extra, stability_label ? stability_label : "");
  (void)sb_append(&extra, ",\"stability_note\":");
  (void)sb_append_json_str(&extra, stability_note);
  (void)sb_append(&extra, ",\"language_score_note\":");
  (void)sb_append_json_str(&extra, stability_note);
  (void)sb_append(&extra, ",\"completion_eta_local\":");
  (void)sb_append_json_str(&extra, completion_eta ? completion_eta : "");
  (void)sb_append(&extra, ",\"run_command\":");
  (void)sb_append_json_str(&extra, run_command ? run_command : "");
  (void)sb_append(&extra, ",\"status_command\":");
  (void)sb_append_json_str(&extra, status_command ? status_command : "");
  (void)sb_append(&extra, ",\"next_script\":");
  (void)sb_append_json_str(&extra, next_script ? next_script : "");
  (void)sb_append(&extra, ",\"progress_command\":");
  (void)sb_append_json_str(&extra, progress_command ? progress_command : "");
  (void)sb_append(&extra, ",\"next_handoff_command\":");
  (void)sb_append_json_str(&extra, next_handoff_command ?
                                   next_handoff_command : "");
  (void)sb_append(&extra, ",\"next_command\":");
  (void)sb_append_json_str(&extra, next_command ? next_command : "");
  (void)sb_append(&extra, ",\"preview_command\":");
  (void)sb_append_json_str(&extra, selected_preview_command);
  (void)sb_append(&extra, ",\"run_next_command\":");
  (void)sb_append_json_str(&extra, next_command ? next_command : "");
  (void)sb_append(&extra, ",\"run_next_preview_command\":");
  (void)sb_append_json_str(&extra, selected_preview_command);
  (void)sb_append(&extra, ",\"run_next_low_cpu_command\":");
  (void)sb_append_json_str(&extra, next_command ? next_command : "");
  (void)sb_append(&extra, ",\"run_next_gentle_command\":");
  (void)sb_append_json_str(&extra, run_next_gentle_command);
  (void)sb_append(&extra, ",\"run_next_gentle_preview_command\":");
  (void)sb_append_json_str(&extra, run_next_gentle_preview_command);
  (void)sb_append(&extra, ",\"stop_file\":");
  (void)sb_append_json_str(&extra, stop_file ? stop_file : "");
  (void)sb_append(&extra, ",\"stop_command\":");
  (void)sb_append_json_str(&extra, stop_command ? stop_command : "");
  (void)sb_append(&extra, ",\"resume_command\":");
  (void)sb_append_json_str(&extra, resume_command ? resume_command : "");
  (void)sb_append(&extra, ",\"state_file\":");
  (void)sb_append_json_str(&extra, state_file ? state_file : "");
  (void)sb_append(&extra, ",\"state_command\":");
  (void)sb_append_json_str(&extra, state_command ? state_command : "");
  (void)sb_append(&extra, ",\"state_refresh_command\":");
  (void)sb_append_json_str(&extra, selected_state_refresh_command);
  append_fuzz_all_run_state_fields(&extra, &run_state);
  char progress_extra_freshness_action_command[4096] = {0};
  if (freshness_penalty > 0.0)
    fuzz_all_freshness_action_command(progress_extra_freshness_action_command,
                                      sizeof(progress_extra_freshness_action_command),
                                      next_handoff_command);
  (void)sb_append(&extra, ",\"freshness_action_command\":");
  (void)sb_append_json_str(&extra, progress_extra_freshness_action_command);
  (void)sb_append(&extra, ",\"latest_report_freshness_command\":");
  (void)sb_append_json_str(&extra,
                           !latest_report_fresh ?
                               progress_extra_freshness_action_command : "");
  (void)sb_append(&extra, ",\"latest_full_pressure_report_freshness_command\":");
  (void)sb_append_json_str(&extra,
                           !latest_full_pressure_report_fresh ?
                               progress_extra_freshness_action_command : "");
  (void)sb_append(&extra, ",\"full_pressure_freshen_command\":");
  (void)sb_append_json_str(&extra,
                           !latest_full_pressure_report_fresh ?
                               progress_extra_freshness_action_command : "");
  (void)sb_append(&extra, ",\"full_pressure_remediation_command\":");
  (void)sb_append_json_str(&extra,
                           !latest_full_pressure_report_fresh ?
                               progress_extra_freshness_action_command : "");
  (void)sb_append(&extra, ",\"full_pressure_action_command\":");
  (void)sb_append_json_str(&extra,
                           !latest_full_pressure_report_fresh ?
                               progress_extra_freshness_action_command : "");
  (void)sb_append(&extra, ",\"advisory_action_command\":");
  (void)sb_append_json_str(&extra,
                           (non_reproducing_timeouts > 0.0 ||
                            historical_non_reproducing_timeouts > 0.0) &&
                                   advisory_action_command ?
                               advisory_action_command : "");
  (void)sb_append(&extra, ",\"advisory_recheck_command\":");
  (void)sb_append_json_str(&extra,
                           (non_reproducing_timeouts > 0.0 ||
                            historical_non_reproducing_timeouts > 0.0) &&
                                   advisory_recheck_command ?
                               advisory_recheck_command : "");
  (void)sb_append(&extra, ",\"advisory_recheck_state\":");
  (void)sb_append_json_str(
      &extra,
      fuzz_all_advisory_recheck_state(
          progress_advisory_present,
          advisory_recheck_raw_repro_checked,
          advisory_recheck_raw_repro_passed,
          advisory_recheck_raw_repro_timeouts,
          advisory_recheck_raw_repro_unexpected,
          advisory_recheck_command));
  (void)sb_appendf(
      &extra,
      ",\"advisory_recheck_raw_repro_checked\":%.0f,"
      "\"advisory_recheck_raw_repro_passed\":%.0f,"
      "\"advisory_recheck_raw_repro_timeouts\":%.0f,"
      "\"advisory_recheck_raw_repro_unexpected\":%.0f",
      progress_advisory_present ? advisory_recheck_raw_repro_checked : 0.0,
      progress_advisory_present ? advisory_recheck_raw_repro_passed : 0.0,
      progress_advisory_present ? advisory_recheck_raw_repro_timeouts : 0.0,
      progress_advisory_present ? advisory_recheck_raw_repro_unexpected : 0.0);
  (void)sb_append(&extra, ",\"perf_watchlist_command\":");
  (void)sb_append_json_str(&extra, perf_watchlist_command ?
                                       perf_watchlist_command : "");
  (void)sb_append(&extra, ",\"perf_watchlist_state\":");
  (void)sb_append_json_str(&extra, perf_watchlist_state);
  (void)sb_append(&extra, ",\"perf_watchlist_action\":");
  (void)sb_append_json_str(&extra, perf_watchlist_action);
  (void)sb_append(&extra, ",\"perf_watchlist_action_command\":");
  (void)sb_append_json_str(&extra, perf_watchlist_action_command ?
                                       perf_watchlist_action_command : "");
  (void)sb_append(&extra, ",\"optimization_action\":");
  (void)sb_append_json_str(&extra, perf_watchlist_action);
  (void)sb_append(&extra, ",\"optimization_reason\":");
  (void)sb_append_json_str(&extra, optimization_reason);
    (void)sb_append(&extra, ",\"optimization_command\":");
    (void)sb_append_json_str(&extra, perf_watchlist_action_command ?
                                        perf_watchlist_action_command : "");
  (void)sb_append(&extra, ",\"optimization_target_command\":");
  (void)sb_append_json_str(&extra, optimization_target_command ?
                                      optimization_target_command : "");
    (void)sb_append(&extra, ",\"optimization_case\":");
    (void)sb_append_json_str(&extra, optimization_case);
  (void)sb_append(&extra, ",\"optimization_artifact\":");
  (void)sb_append_json_str(&extra, optimization_artifact);
  (void)sb_append(&extra, ",\"optimization_ny_source\":");
  (void)sb_append_json_str(&extra, optimization_ny_source);
  (void)sb_append(&extra, ",\"optimization_c_source\":");
  (void)sb_append_json_str(&extra, optimization_c_source);
    (void)sb_appendf(&extra,
                     ",\"optimization_ratio\":%.4f,"
                     "\"optimization_slowdown_percent\":%.2f",
                   optimization_ratio,
                   fuzz_all_perf_slowdown_percent(optimization_ratio));
  (void)sb_append(&extra, ",\"perf_watchlist_report\":");
  (void)sb_append_json_str(&extra, perf_watchlist_report ?
                                       perf_watchlist_report : "");
  (void)sb_append(&extra, ",\"perf_watchlist_markdown\":");
  (void)sb_append_json_str(&extra, perf_watchlist_markdown ?
                                       perf_watchlist_markdown : "");
  (void)sb_append(&extra, ",\"perf_watchlist_artifact_readable\":");
  (void)sb_append(&extra,
                  perf_watchlist_artifact_readable ? "true" : "false");
  (void)sb_append(&extra, ",\"perf_watchlist_artifact_fresh\":");
  (void)sb_append(&extra,
                  perf_watchlist_artifact_fresh ? "true" : "false");
  (void)sb_appendf(&extra,
                   ",\"perf_watchlist_artifact_hotspots\":%.0f,"
                   "\"perf_watchlist_artifact_max_ratio\":%.4f,"
                   "\"perf_watchlist_artifact_max_slowdown_percent\":%.2f,"
                   "\"perf_watchlist_artifact_age_seconds\":%.0f,"
                   "\"perf_watchlist_artifact_stale_after_hours\":%.2f",
                   perf_watchlist_artifact_hotspots,
                   perf_watchlist_artifact_max_ratio,
                   fuzz_all_perf_slowdown_percent(
                       perf_watchlist_artifact_max_ratio),
                   perf_watchlist_artifact_age_seconds,
                   perf_watchlist_artifact_stale_after_hours);
    (void)sb_append(&extra, ",\"perf_watchlist_artifact_max_case\":");
    (void)sb_append_json_str(&extra, perf_watchlist_artifact_max_case ?
                                         perf_watchlist_artifact_max_case : "");
  (void)sb_append(&extra, ",\"perf_watchlist_artifact_max_artifact\":");
  (void)sb_append_json_str(
      &extra, perf_watchlist_artifact_max_artifact ?
                  perf_watchlist_artifact_max_artifact : "");
  (void)sb_append(&extra, ",\"perf_watchlist_artifact_max_ny_source\":");
  (void)sb_append_json_str(
      &extra, perf_watchlist_artifact_max_ny_source ?
                  perf_watchlist_artifact_max_ny_source : "");
  (void)sb_append(&extra, ",\"perf_watchlist_artifact_max_c_source\":");
  (void)sb_append_json_str(
      &extra, perf_watchlist_artifact_max_c_source ?
                  perf_watchlist_artifact_max_c_source : "");
  (void)sb_append(&extra, ",\"recommended_action\":");
  (void)sb_append_json_str(&extra, recommendation.action);
  (void)sb_append(&extra, ",\"recommended_reason\":");
  (void)sb_append_json_str(&extra, recommendation.reason);
  (void)sb_append(&extra, ",\"recommended_command\":");
  (void)sb_append_json_str(&extra, recommendation.command);
  (void)sb_append(&extra, ",\"recommended_low_cpu_command\":");
  (void)sb_append_json_str(&extra, recommendation.low_cpu_command);
  (void)sb_append(&extra, ",\"recommended_preview_command\":");
  (void)sb_append_json_str(&extra, recommendation.preview_command);
  (void)sb_append(&extra, ",\"recommended_repeat_mode\":");
  (void)sb_append_json_str(&extra, recommendation.repeat_mode);
  (void)sb_appendf(&extra, ",\"recommended_repeat_count\":%.0f",
                   recommendation.repeat_count);
  append_fuzz_all_recommended_state_fields(
      &extra, recommendation.action, &run_state, &coverage_next_run_state,
      state_file, state_command, state_refresh_command,
      recommendation.preview_command,
      have_coverage_next ? coverage_next_state_file : "",
      have_coverage_next ? coverage_next_state_command : "",
      have_coverage_next ? coverage_next_state_refresh_command : "",
      have_coverage_next && coverage_next_state_refresh_required,
      have_coverage_next ? coverage_next_state_refresh_reason : "");
  append_fuzz_all_handoff_guard_fields(&extra);
  (void)sb_append(&extra, ",\"tmp_dir\":");
  (void)sb_append_json_str(&extra, tmp_dir ? tmp_dir : "");
  (void)sb_append(&extra, ",\"scratch_root\":");
  (void)sb_append_json_str(&extra, scratch_root ? scratch_root : "");
  (void)sb_append(&extra, ",\"xdg_cache_home\":");
  (void)sb_append_json_str(&extra, xdg_cache_home ? xdg_cache_home : "");
  (void)sb_append(&extra, ",\"nytrix_cache_dir\":");
  (void)sb_append_json_str(&extra, nytrix_cache_dir ? nytrix_cache_dir : "");
  (void)sb_append(&extra, ",\"old_path_probe_command\":");
  (void)sb_append_json_str(&extra, NYTRIX_FUZZ_ALL_OLD_PATH_PROBE_COMMAND);
  (void)sb_append(&extra, ",\"old_path_command\":");
  (void)sb_append_json_str(&extra, old_path_command ? old_path_command : "");
  (void)sb_append(&extra, ",\"old_path_dry_run_command\":");
  (void)sb_append_json_str(&extra, old_path_dry_run_command ?
                                       old_path_dry_run_command : "");
  (void)sb_append(&extra, ",\"old_path_apply_command\":");
  (void)sb_append_json_str(&extra, old_path_apply_command ?
                                       old_path_apply_command : "");
  (void)sb_append(&extra, ",\"old_path_next_action\":");
  (void)sb_append_json_str(&extra, old_path_next_action ?
                                       old_path_next_action : "none");
  (void)sb_append(&extra, ",\"old_path_next_reason\":");
  (void)sb_append_json_str(&extra, old_path_next_reason ?
                                       old_path_next_reason : "");
  (void)sb_append(&extra, ",\"old_path_report\":");
  (void)sb_append_json_str(&extra, old_path_report ? old_path_report : "");
  (void)sb_append(&extra, ",\"old_path_markdown\":");
  (void)sb_append_json_str(&extra, old_path_markdown ? old_path_markdown : "");
  (void)sb_appendf(&extra,
                   ",\"old_path_cache_policy_ok\":%s,"
                   "\"old_path_present_count\":%.0f,"
                   "\"old_path_moved_count\":%.0f,"
                   "\"old_path_remaining_count\":%.0f,"
                   "\"old_path_wait_remaining_seconds\":%.0f,"
                   "\"old_path_artifact_leak_count\":%.0f,"
                   "\"old_path_artifact_moved_count\":%.0f,"
                   "\"old_path_artifact_remaining_count\":%.0f",
                   old_path_cache_policy_ok ? "true" : "false",
                   old_path_present_count,
                   old_path_moved_count,
                   old_path_remaining_count,
                   old_path_wait_remaining_seconds,
                   old_path_artifact_leak_count,
                   old_path_artifact_moved_count,
                   old_path_artifact_remaining_count);
  (void)sb_append(&extra, ",\"latest_report\":");
  (void)sb_append_json_str(&extra, latest_report ? latest_report : "");
  (void)sb_append(&extra, ",\"latest_full_pressure_report\":");
  (void)sb_append_json_str(&extra, latest_full_pressure_report ?
                                  latest_full_pressure_report : "");
  if (progress_markdown_path && *progress_markdown_path) {
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, root, progress_markdown_path);
  }

  if (progress_markdown_path && *progress_markdown_path &&
      !write_fuzz_all_progress_markdown(progress_markdown_path, root, json_path,
                                        status_path,
                                        worklist_path, worklist_markdown_path,
                                        coverage_path, coverage_markdown_path,
                                        plan_path, plan_markdown_path,
                                        status_age_seconds,
                                        target_percent, thread_years,
                                        target_thread_years,
                                        remaining_thread_years,
                                        runs_needed, wall_hours_needed,
                                        thread_hours_needed, wall_days_needed,
                                        evidence_reports,
                                        evidence_ignored_no_evidence_reports,
                                        evidence_full_pressure_reports,
                                        evidence_full_pressure_thread_years,
                                        evidence_checked_subcases,
                                        evidence_coverage_ran_lanes,
                                        evidence_coverage_lanes,
                                        coverage_blocker_gaps,
                                        coverage_failed_lanes,
                                        evidence_coverage_skipped_lanes,
                                        coverage_disabled_lanes,
                                        coverage_budget_short_lanes,
                                        coverage_missing_tool_lanes,
                                        coverage_detail_count,
                                        coverage_backlog_lanes,
                                        coverage_detail_rows,
                                        coverage_queue_count,
                                        coverage_queue_non_advisory_count,
                                        coverage_queue_advisory_count,
                                        coverage_queue_lanes,
                                        coverage_advisory_gaps,
                                        coverage_reports_considered,
                                        coverage_campaign_reports_considered,
                                        coverage_companion_reports_considered,
                                        coverage_latest_report_advisory_gaps,
                                        coverage_latest_report_companion_skipped_lanes,
                                        coverage_next_action,
                                        coverage_next_category,
                                        coverage_next_severity,
                                        coverage_next_lane,
                                        coverage_next_reason,
                                        coverage_next_command,
                                        coverage_next_guarded_command,
                                        coverage_next_low_cpu_command,
                                        coverage_next_preview_command,
                                        coverage_next_state_command,
                                        coverage_next_state_refresh_command,
                                        coverage_next_state_refresh_required,
                                        coverage_next_state_refresh_reason,
                                        recommended_state_refresh_command,
                                        coverage_next_stop_command,
                                        coverage_next_resume_command,
                                        &coverage_next_run_state,
                                        thread_years_per_run,
                                        thread_years_per_run_source,
                                        target_percent_per_run,
                                        next_run_target_percent,
                                        next_run_stability_score,
                                        next_run_stability_delta,
                                        runs_to_good_stability,
                                        runs_to_good_days,
                                        ready, blocker_count, active_items,
                                        historical_attention_reports,
                                        target_reached,
                                        campaign_complete, correctness_findings,
                                        compiler_findings,
                                        known_bug_replay_findings,
                                        perf_hotspots, perf_worst_ratio,
                                        perf_worst_case,
                                        latest_full_pressure_perf_hotspots,
                                        latest_full_pressure_perf_worst_ratio,
                                        latest_full_pressure_perf_case,
                                        latest_full_pressure_perf_rows,
                                        latest_full_pressure_perf_suite_current,
                                        latest_full_pressure_ok,
                                        latest_full_pressure_clean,
                                        latest_full_pressure_failure_count,
                                        latest_report_demoted_non_reproducing_afl_timeout,
                                        latest_full_pressure_demoted_non_reproducing_afl_timeout,
                                        latest_full_pressure_clean_reason,
                                        non_reproducing_timeouts,
                                        effective_advisory_timeouts,
                                        historical_non_reproducing_timeouts,
                                        cache_policy_ok,
                                        ny_bin_exists,
                                        old_nytrix_test_scratch_absent,
                                        old_nytrix_fuzz_absent,
                                        old_nytrix_build_cache_absent,
                                        active_old_nytrix_writer_present,
                                        old_path_present_count,
                                        old_path_moved_count,
                                        old_path_remaining_count,
                                        old_path_wait_remaining_seconds,
                                        stability_score,
                                        signal_score, evidence_cap,
                                        gate_penalty, correctness_penalty,
                                        perf_penalty, advisory_penalty,
                                        env_penalty, freshness_penalty,
                                        stability_label, stability_note,
                                        completion_eta,
                                        progress_command,
                                        next_command, next_handoff_command,
                                        preview_command,
                                        stop_file, stop_command,
                                        resume_command,
                                        state_file, state_command,
                                        state_refresh_command, &run_state,
                                        old_path_command,
                                        old_path_dry_run_command,
                                        old_path_apply_command,
                                        old_path_next_action,
                                        old_path_next_reason,
                                        advisory_action_command,
                                            advisory_recheck_command,
                                            advisory_recheck_raw_repro_checked,
                                            advisory_recheck_raw_repro_passed,
                                            advisory_recheck_raw_repro_timeouts,
                                            advisory_recheck_raw_repro_unexpected,
                                          perf_watchlist_command,
                                          perf_watchlist_report,
                                          perf_watchlist_markdown,
                                          perf_watchlist_artifact_readable,
                                          perf_watchlist_artifact_fresh,
                                          perf_watchlist_artifact_hotspots,
                                            perf_watchlist_artifact_max_ratio,
                                            perf_watchlist_artifact_age_seconds,
                                            perf_watchlist_artifact_stale_after_hours,
                                            perf_watchlist_artifact_max_case,
                                            perf_watchlist_artifact_max_artifact,
                                            perf_watchlist_artifact_max_ny_source,
                                            perf_watchlist_artifact_max_c_source,
                                            compiler_std_audit_readable,
                                            compiler_std_audit_report,
                                            compiler_std_audit_markdown,
                                            compiler_std_audit_command,
                                            runtime_surface_state,
                                            crt_surface_state,
                                            crt_export_coverage_percent,
                                            crt_unreferenced_count,
                                            crt_unreferenced_percent,
                                            crt_unreferenced_family_count,
                                            crt_top_unreferenced_family,
                                            crt_top_unreferenced_family_count,
                                            crt_next_action,
                                            crt_next_unreferenced_family,
                                            crt_next_unreferenced_count,
                                            latest_report,
                                            latest_full_pressure_report,
                                            latest_report_age_seconds,
                                        latest_full_pressure_report_age_seconds)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-progress",
                                                  "progress markdown write failed",
                                                  progress_markdown_path));
  }

  int rc = failures.count ? 1 : 0;
  if (json_path && *json_path) {
    char *report = build_native_report_json_with_top_aliases(
        &rows, &failures, "fuzz-all-progress", extra.data, true);
    rc = emit_native_report(report, json_path, "all fuzz progress",
                            rows.count, failures.count);
  } else if (failures.count) {
    printf("progress unavailable: %s\n", status_path ? status_path : "");
  } else {
    printf("progress: %.4f%%\n", target_percent);
    printf("confidence: campaign=%.4f%% stability=%.2f%% label=%s\n",
           target_percent, stability_score, stability_label ? stability_label : "");
    printf("thread-years: %.6f/%.6f\n", thread_years, target_thread_years);
    printf("remaining: %.6f; runs: %.0f; eta: %s\n",
           remaining_thread_years, runs_needed,
           completion_eta && *completion_eta ? completion_eta : "unknown");
    if (wall_hours_needed > 0.0 || thread_hours_needed > 0.0)
      printf("budget: wall-hours=%.2f thread-hours=%.2f\n",
             wall_hours_needed, thread_hours_needed);
    if (status_age_seconds >= 0.0)
      printf("source: status-age=%.1fm\n", status_age_seconds / 60.0);
    printf("gate: %s; blockers=%.0f; active=%.0f; cache=%s; ny=%s\n",
           ready ? "ready" : "blocked", blocker_count, active_items,
           cache_policy_ok ? "ok" : "bad",
           ny_bin_exists ? "ok" : "missing");
    printf("full-pressure-gate: effective_clean=%s raw_ok=%s reason=%s failures=%.0f demoted-latest=%s demoted-full-pressure=%s\n",
           latest_full_pressure_clean ? "true" : "false",
           latest_full_pressure_ok ? "true" : "false",
           latest_full_pressure_clean_reason &&
                   *latest_full_pressure_clean_reason ?
               latest_full_pressure_clean_reason : "unknown",
           latest_full_pressure_failure_count,
           latest_report_demoted_non_reproducing_afl_timeout ? "true" : "false",
           latest_full_pressure_demoted_non_reproducing_afl_timeout ?
               "true" : "false");
    if (compiler_std_audit_readable) {
      printf("compiler-std-audit: runtime=%s crt=%s coverage=%.2f%% unreferenced=%.0f/%.2f%% families=%.0f top=%s/%.0f report=%s\n",
             runtime_surface_state && *runtime_surface_state ?
                 runtime_surface_state : "unknown",
             crt_surface_state && *crt_surface_state ?
                 crt_surface_state : "unknown",
             crt_export_coverage_percent, crt_unreferenced_count,
             crt_unreferenced_percent, crt_unreferenced_family_count,
             crt_top_unreferenced_family && *crt_top_unreferenced_family ?
                 crt_top_unreferenced_family : "none",
             crt_top_unreferenced_family_count,
             compiler_std_audit_markdown && *compiler_std_audit_markdown ?
                 compiler_std_audit_markdown : NYTRIX_COMPILER_STD_AUDIT_MARKDOWN);
    } else {
      printf("compiler-std-audit: not-readable command=%s\n",
             compiler_std_audit_command && *compiler_std_audit_command ?
                 compiler_std_audit_command : NYTRIX_COMPILER_STD_AUDIT_COMMAND);
    }
    printf("lang-score: %.2f%% (%s); signal=%.2f%%; cap=%.2f%%\n",
           stability_score, stability_label ? stability_label : "",
           signal_score, evidence_cap);
    if (stability_note[0]) printf("score-note: %s\n", stability_note);
    printf("evidence: reports=%.0f full-pressure=%.0f/%.6fy checked=%.0f coverage=%.0f/%.0f\n",
           evidence_reports, evidence_full_pressure_reports,
           evidence_full_pressure_thread_years, evidence_checked_subcases,
           evidence_coverage_ran_lanes, evidence_coverage_lanes);
    printf("coverage: state=%s lanes=%.0f/%.0f not-run=%.0f actionable=%.0f skipped=%.0f disabled=%.0f budget-short=%.0f missing-tool=%.0f reports=%.0f\n",
           fuzz_all_coverage_state(evidence_coverage_lanes,
                                   evidence_coverage_ran_lanes,
                                   coverage_blocker_gaps,
                                   coverage_failed_lanes),
           evidence_coverage_ran_lanes, evidence_coverage_lanes,
           evidence_coverage_not_run_lanes,
           coverage_backlog_lanes > 0.0 ? coverage_backlog_lanes :
               coverage_detail_count,
           evidence_coverage_skipped_lanes,
           coverage_disabled_lanes, coverage_budget_short_lanes,
           coverage_missing_tool_lanes, coverage_reports_considered);
    if (historical_attention_reports > 0.0) {
      char *worklist_md_rel =
          rel_path_dup(root, worklist_history_markdown_path ?
                                 worklist_history_markdown_path : "");
      printf("history: hidden-attention=%.0f drilldown=%s\n",
             historical_attention_reports,
             worklist_md_rel && *worklist_md_rel ? worklist_md_rel :
                                                    "build/fuzz/all/worklist-history.md");
      free(worklist_md_rel);
    }
    printf("paths: old-test=%s old-fuzz=%s old-build-cache=%s old-writer=%s old-present=%.0f old-moved=%.0f old-remaining=%.0f old-artifact-leaks=%.0f old-artifacts-remaining=%.0f\n",
           old_nytrix_test_scratch_absent ? "absent" : "present",
           old_nytrix_fuzz_absent ? "absent" : "present",
           old_nytrix_build_cache_absent ? "absent" : "present",
           active_old_nytrix_writer_present ? "present" : "none",
           old_path_present_count,
           old_path_moved_count,
           old_path_remaining_count,
           old_path_artifact_leak_count,
           old_path_artifact_remaining_count);
    printf("old-paths: %s\n",
           old_path_command && *old_path_command ? old_path_command :
                                                    NYTRIX_OLD_PATH_DRY_RUN_COMMAND);
    printf("old-path-action: %s reason=%s wait-remaining-s=%.0f\n",
           old_path_next_action && *old_path_next_action ?
               old_path_next_action : "none",
           old_path_next_reason && *old_path_next_reason ?
               old_path_next_reason :
               fuzz_all_old_path_next_reason(
                   old_path_remaining_count,
                   active_old_nytrix_writer_present),
           old_path_wait_remaining_seconds);
    printf("old-path-apply: %s\n",
           old_path_apply_command && *old_path_apply_command ?
               old_path_apply_command : NYTRIX_OLD_PATH_APPLY_COMMAND);
    printf("findings: correctness=%.0f compiler=%.0f known-bug=%.0f perf=%.0f",
           correctness_findings, compiler_findings,
           known_bug_replay_findings, perf_hotspots);
    if (perf_worst_ratio > 0.0) {
      printf(" worst=%.4fx slowdown=%.2f%%", perf_worst_ratio,
             fuzz_all_perf_slowdown_percent(perf_worst_ratio));
      if (perf_worst_case && *perf_worst_case) printf("/%s", perf_worst_case);
    }
    printf(" current-advisory-timeouts=%.0f historical-advisory-timeouts=%.0f\n",
           non_reproducing_timeouts, historical_non_reproducing_timeouts);
    if (latest_full_pressure_perf_worst_ratio > 0.0) {
      printf("full-pressure-perf: hotspots=%.0f worst=%.4fx slowdown=%.2f%%",
             latest_full_pressure_perf_hotspots,
             latest_full_pressure_perf_worst_ratio,
             fuzz_all_perf_slowdown_percent(
                 latest_full_pressure_perf_worst_ratio));
      if (latest_full_pressure_perf_case && *latest_full_pressure_perf_case)
        printf("/%s", latest_full_pressure_perf_case);
      printf(" rows=%.0f/%d suite=%s\n", latest_full_pressure_perf_rows,
             perf_real_case_count(),
             latest_full_pressure_perf_suite_current ? "current" : "stale");
    }
    if ((non_reproducing_timeouts > 0.0 ||
         historical_non_reproducing_timeouts > 0.0) &&
        advisory_action_command && *advisory_action_command)
      printf("advisory-action: %s\n", advisory_action_command);
    if ((non_reproducing_timeouts > 0.0 ||
         historical_non_reproducing_timeouts > 0.0) &&
        advisory_recheck_command && *advisory_recheck_command)
      printf("advisory-recheck: state=%s raw=%.0f/%.0f timeouts=%.0f unexpected=%.0f command=%s\n",
             fuzz_all_advisory_recheck_state(
                 true, advisory_recheck_raw_repro_checked,
                 advisory_recheck_raw_repro_passed,
                 advisory_recheck_raw_repro_timeouts,
                 advisory_recheck_raw_repro_unexpected,
                 advisory_recheck_command),
             advisory_recheck_raw_repro_passed,
             advisory_recheck_raw_repro_checked,
             advisory_recheck_raw_repro_timeouts,
             advisory_recheck_raw_repro_unexpected,
             advisory_recheck_command);
    if (target_percent_per_run > 0.0) {
      if (runs_to_good_stability >= 0.0) {
        printf("next-run: +%.6f thread-years (%s), +%.4f%% progress; lang-score=%.2f%% (%+.2f); good-lang-runs=%.0f",
               thread_years_per_run, thread_years_per_run_source,
               target_percent_per_run,
               next_run_stability_score, next_run_stability_delta,
               runs_to_good_stability);
        if (runs_to_good_days >= 0.0) printf("; good-lang-days=%.2f", runs_to_good_days);
        printf("\n");
      } else {
        printf("next-run: +%.6f thread-years (%s), +%.4f%% progress; lang-score=%.2f%% (%+.2f); good-lang-runs=unknown\n",
               thread_years_per_run, thread_years_per_run_source,
               target_percent_per_run,
               next_run_stability_score, next_run_stability_delta);
      }
    }
    printf("penalties: gate=%.2f correctness=%.2f perf=%.2f advisory=%.2f env=%.2f\n",
           gate_penalty, correctness_penalty, perf_penalty, advisory_penalty,
           env_penalty);
    printf("recommended: %s", recommendation.action);
    if (recommendation.reason && *recommendation.reason)
      printf(" reason=%s", recommendation.reason);
    if (recommendation.command[0]) printf(" command=%s", recommendation.command);
    printf("\n");
    if (recommendation.preview_command[0])
      printf("preview-recommended: %s\n", recommendation.preview_command);
    printf("next: %s\n", next_command && *next_command ? next_command : "unknown");
    if (preview_command && *preview_command &&
        strcmp(preview_command, recommendation.preview_command) != 0)
      printf("preview-target: %s\n", preview_command);
    if (state_command && *state_command)
      printf("state: %s\n", state_command);
    if (stop_command && *stop_command) printf("pause: %s\n", stop_command);
    if (resume_command && *resume_command)
      printf("resume: %s\n", resume_command);
  }

  free(extra.data);
  free(status.data);
  free(dir_path);
  free(status_path);
  free(status_markdown_path);
  free(progress_markdown_path);
  free(history_path);
  free(worklist_path);
  free(worklist_markdown_path);
  free(worklist_history_path);
  free(worklist_history_markdown_path);
  free(coverage_path);
  free(coverage_markdown_path);
  free(plan_path);
  free(plan_markdown_path);
  free(dir_cmd_path);
  free(status_cmd_path);
  free(history_cmd_path);
  free(worklist_cmd_path);
  free(coverage_cmd_path);
  free(plan_cmd_path);
  free(progress_json_cmd_path);
  free(progress_md_cmd_path);
  free(progress_command);
  free(progress_command_raw);
  free(old_path_dry_run_command);
  free(old_path_dry_run_command_raw);
  free(old_path_apply_command);
  free(old_path_apply_command_raw);
  free(completion_eta);
  free(campaign_plan_threads);
  free(campaign_first_report);
  free(next_script);
  free(status_next_handoff_command);
  free(status_next_command);
  free(run_command);
  free(status_command);
  free(stop_file);
  free(stop_command);
  free(resume_command);
    free(state_file);
    free(state_command);
  free(state_refresh_command);
  free(tmp_dir);
  free(scratch_root);
  free(xdg_cache_home);
  free(nytrix_cache_dir);
    free(old_path_command);
  free(old_path_next_action);
  free(old_path_next_reason);
  free(old_path_report);
  free(old_path_markdown);
      free(advisory_action_command);
  free(advisory_recheck_command);
      free(perf_watchlist_command);
  free(perf_watchlist_report);
    free(perf_watchlist_markdown);
    free(perf_watchlist_action_command);
  free(optimization_target_command);
    free(perf_watchlist_artifact_max_case);
  free(perf_watchlist_artifact_max_artifact);
  free(perf_watchlist_artifact_max_ny_source);
  free(perf_watchlist_artifact_max_c_source);
  free(latest_report);
  free(latest_full_pressure_report);
  free(latest_full_pressure_clean_reason);
  free(perf_worst_case);
  free(latest_full_pressure_perf_case);
  free(coverage_next_action);
  free(coverage_next_category);
  free(coverage_next_severity);
  free(coverage_next_lane);
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
  free(coverage_next_stop_file);
  free(coverage_next_stop_command);
  free(coverage_next_resume_command);
  free(coverage_queue_lanes);
  free(coverage_queue_json);
  free(compiler_std_audit_report);
  free(compiler_std_audit_markdown);
  free(compiler_std_audit_command);
  free(runtime_surface_state);
  free(crt_surface_state);
  free(crt_top_unreferenced_family);
  free(crt_unreferenced_families);
  free(crt_next_action);
  free(crt_next_reason);
  free(crt_next_unreferenced_family);
  free(crt_next_unreferenced_exports);
  free(crt_next_definition_file);
  free(crt_next_definition_locations);
  free(crt_next_inspect_command);
  free(preview_command);
  free(next_handoff_command);
  free(next_command);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static int cmd_public_fuzz_all_run(int argc, char **argv) {
  char root[4096];
  if (!find_nytrix_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nytrix-root-not-found\"}\n");
    return 2;
  }
  const char *low_priority_env = getenv("NYTRIX_LOW_PRIORITY");
  bool low_priority_enabled =
      !(low_priority_env && strcmp(low_priority_env, "0") == 0);
  int run_nice_target = NYTRIX_DEFAULT_RUN_NICE;
  int run_nice_value = 0;
  bool run_low_priority_applied =
      nytrix_apply_process_low_priority(&run_nice_target, &run_nice_value);
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *profile = value_after_equals(argc, argv, 4, "--profile", "insane");
  const char *dir_arg = value_after_equals(argc, argv, 4, "--dir", "");
  if (!dir_arg || !*dir_arg)
    dir_arg = value_after_equals(argc, argv, 4, "--history-dir", "");
  const char *target_arg = value_after_equals(argc, argv, 4, "--target-thread-years", "");
  if (!target_arg || !*target_arg)
    target_arg = value_after_equals(argc, argv, 4, "--target-years", "");
  if (!target_arg || !*target_arg)
    target_arg = value_after_equals(argc, argv, 4, "--target", "10");
  const char *lane_filter_arg = value_after_equals(argc, argv, 4, "--only-lane", "");
  if (!lane_filter_arg || !*lane_filter_arg)
    lane_filter_arg = value_after_equals(argc, argv, 4, "--only-phase", "");
  if (!lane_filter_arg || !*lane_filter_arg)
    lane_filter_arg = value_after_equals(argc, argv, 4, "--only", "all");
  bool lane_filter_active =
      lane_filter_arg && *lane_filter_arg && strcmp(lane_filter_arg, "all") != 0;
  bool lane_filter_afl =
      lane_filter_active &&
      (strcmp(lane_filter_arg, "afl") == 0 ||
       strcmp(lane_filter_arg, "afl_compiler") == 0 ||
       strcmp(lane_filter_arg, "afl_parsers") == 0);
  if (lane_filter_active && !lane_filter_afl) {
    str_buf_t err = {0};
    (void)sb_append(&err, "{\"ok\":false,\"error\":\"unsupported-lane-filter\",\"lane_filter\":");
    (void)sb_append_json_str(&err, lane_filter_arg);
    (void)sb_append(&err, "}\n");
    fputs(err.data ? err.data : "{\"ok\":false,\"error\":\"unsupported-lane-filter\"}\n", stdout);
    free(err.data);
    return 2;
  }
  bool smoke = has_flag_after(argc, argv, 4, "--smoke") || has_flag_after(argc, argv, 4, "--fast");
  bool fail_fast = !has_flag_after(argc, argv, 4, "--keep-going");
  bool skip_afl = has_flag_after(argc, argv, 4, "--no-afl") || has_flag_after(argc, argv, 4, "--skip-afl");
  bool skip_gc = has_flag_after(argc, argv, 4, "--no-gc") || has_flag_after(argc, argv, 4, "--skip-gc");
  bool skip_synth = has_flag_after(argc, argv, 4, "--no-synth") || has_flag_after(argc, argv, 4, "--skip-synth");
  bool skip_sanitizers = has_flag_after(argc, argv, 4, "--no-sanitizers") ||
                         has_flag_after(argc, argv, 4, "--skip-sanitizers");
  bool allow_nytrix = has_flag_after(argc, argv, 4, "--allow-nytrix");
  bool deny_nytrix = has_flag_after(argc, argv, 4, "--no-nytrix") ||
                     has_flag_after(argc, argv, 4, "--skip-nytrix");
  bool skip_nytrix = deny_nytrix || !allow_nytrix;
  bool skip_compiler_audit = has_flag_after(argc, argv, 4, "--no-compiler-audit") ||
                             has_flag_after(argc, argv, 4, "--skip-compiler-audit") ||
                             has_flag_after(argc, argv, 4, "--no-std-audit") ||
                             has_flag_after(argc, argv, 4, "--skip-std-audit");
  bool skip_compiler_findings = has_flag_after(argc, argv, 4, "--no-compiler-findings") ||
                                has_flag_after(argc, argv, 4, "--skip-compiler-findings") ||
                                has_flag_after(argc, argv, 4, "--no-findings-watchlist") ||
                                has_flag_after(argc, argv, 4, "--skip-findings-watchlist");
  bool skip_known_bugs = has_flag_after(argc, argv, 4, "--no-known-bugs") ||
                         has_flag_after(argc, argv, 4, "--skip-known-bugs") ||
                         has_flag_after(argc, argv, 4, "--no-bug-repros") ||
                         has_flag_after(argc, argv, 4, "--skip-bug-repros");
  bool skip_prove = has_flag_after(argc, argv, 4, "--no-prove") ||
                    has_flag_after(argc, argv, 4, "--skip-prove") ||
                    has_flag_after(argc, argv, 4, "--no-proof") ||
                    has_flag_after(argc, argv, 4, "--skip-proof");
  bool skip_perf = has_flag_after(argc, argv, 4, "--no-perf") ||
                   has_flag_after(argc, argv, 4, "--skip-perf");
  if (lane_filter_afl) {
    skip_gc = true;
    skip_synth = true;
    skip_sanitizers = true;
    skip_compiler_audit = true;
    skip_compiler_findings = true;
    skip_known_bugs = true;
    skip_prove = true;
    skip_perf = true;
  }
  const char *scratch_root_arg = value_after_equals(argc, argv, 4, "--scratch-root", "");
  double duration_s = fuzz_gc_duration_s(argc, argv, smoke);
  if (!fuzz_gc_has_duration_arg(argc, argv)) duration_s = smoke ? 45.0 : 8.0 * 3600.0;
  int cpu_threads = gc_online_thread_count();
  int default_threads = gc_default_fuzz_thread_count();
  const char *threads_arg = value_after_equals(argc, argv, 4, "--threads",
                                               NYTRIX_DEFAULT_FUZZ_THREADS);
  int threads = gc_parse_thread_count(threads_arg, default_threads);
  char threads_buf[32], seed_buf[32], cases_buf[32], timeout_buf[64];
  char gc_budget_buf[64], snippets_buf[32], frontend_buf[32];
  char prove_timeout_buf[64], perf_timeout_buf[64], perf_wall_timeout_buf[64];
  char perf_limit_buf[32];
  char afl_compiler_minutes_buf[32], afl_parser_minutes_buf[32];
  int seed = atoi(value_after_equals(argc, argv, 4, "--seed", "12648430"));
  int synth_cases = smoke ? 1 : (duration_s < 600.0 ? 2 : (duration_s < 3600.0 ? 4 : 12));
  int campaign_cases = synth_cases > 2 ? synth_cases / 2 : 1;
  int snippets = smoke ? 16 : (duration_s < 600.0 ? 48 : (duration_s < 3600.0 ? 128 : 512));
  int frontend_rounds = smoke ? 8 : (duration_s < 600.0 ? 24 : (duration_s < 3600.0 ? 64 : 240));
  int harness_limit = smoke ? 1 : (duration_s < 1800.0 ? 4 : 0);
  int libs_limit = smoke ? 64 : (duration_s < 1800.0 ? 128 : 0);
  int kernel_limit = smoke ? 8 : (duration_s < 1800.0 ? 16 : 0);
  int perf_limit = smoke ? 3 : (duration_s < 1800.0 ? 6 : 10);
  double synth_timeout_s = smoke ? 45.0 : (duration_s < 600.0 ? 60.0 : 120.0);
  double prove_timeout_s = smoke ? 60.0 : (duration_s < 1800.0 ? 90.0 : 120.0);
  double perf_timeout_s = smoke ? 180.0 : (duration_s < 1800.0 ? 300.0 : 900.0);
  double perf_wall_timeout_s = perf_timeout_s * (double)perf_limit + 180.0;
  double gc_budget_s = smoke ? 8.0 : duration_s * 0.35;
  if (gc_budget_s < 10.0) gc_budget_s = 10.0;
  if (gc_budget_s > 14400.0) gc_budget_s = 14400.0;
  int afl_compiler_minutes = 0;
  int afl_parser_minutes = 0;
  if (!smoke && duration_s >= 300.0) {
    int total_afl_minutes = (int)((duration_s * 0.30) / 60.0);
    if (total_afl_minutes < 1) total_afl_minutes = 1;
    int compiler_targets = 2;
    int parser_targets = parser_fuzz_target_count();
    int compiler_slice = total_afl_minutes / 2;
    int parser_slice = total_afl_minutes - compiler_slice;
    if (compiler_slice < compiler_targets) compiler_slice = compiler_targets;
    afl_compiler_minutes = compiler_slice / compiler_targets;
    if (parser_targets > 0 && parser_slice >= parser_targets)
      afl_parser_minutes = parser_slice / parser_targets;
    if (afl_compiler_minutes > 120) afl_compiler_minutes = 120;
    if (afl_parser_minutes > 120) afl_parser_minutes = 120;
  }
  snprintf(threads_buf, sizeof(threads_buf), "%d", threads);
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
  snprintf(cases_buf, sizeof(cases_buf), "%d", synth_cases);
  snprintf(timeout_buf, sizeof(timeout_buf), "%.3f", synth_timeout_s);
  snprintf(gc_budget_buf, sizeof(gc_budget_buf), "%.3f", gc_budget_s);
  snprintf(snippets_buf, sizeof(snippets_buf), "%d", snippets);
  snprintf(frontend_buf, sizeof(frontend_buf), "%d", frontend_rounds);
  snprintf(prove_timeout_buf, sizeof(prove_timeout_buf), "%.3f", prove_timeout_s);
  snprintf(perf_timeout_buf, sizeof(perf_timeout_buf), "%.3f", perf_timeout_s);
  snprintf(perf_wall_timeout_buf, sizeof(perf_wall_timeout_buf), "%.3f",
           perf_wall_timeout_s);
  snprintf(perf_limit_buf, sizeof(perf_limit_buf), "%d", perf_limit);
  snprintf(afl_compiler_minutes_buf, sizeof(afl_compiler_minutes_buf), "%d", afl_compiler_minutes);
  snprintf(afl_parser_minutes_buf, sizeof(afl_parser_minutes_buf), "%d", afl_parser_minutes);

  char *campaign_dir_path = NULL, *campaign_dir_rel = NULL;
  if (dir_arg && *dir_arg) {
    if (path_is_absolute(dir_arg)) campaign_dir_path = strdup(dir_arg);
    else (void)asprintf(&campaign_dir_path, "%s/%s", root, dir_arg);
  } else {
    (void)asprintf(&campaign_dir_path, "%s/build/fuzz/all", root);
  }
  nytrix_redirect_nytrix_output_dir(&campaign_dir_path, root, "fuzz-all-run");
  campaign_dir_rel = rel_path_dup(root, campaign_dir_path ? campaign_dir_path : "");
  const char *campaign_dir_cmd =
      campaign_dir_rel && *campaign_dir_rel ? campaign_dir_rel : "build/fuzz/all";

  char *work_dir = NULL, *report_dir = NULL, *log_dir = NULL;
  char *command_path = NULL, *manifest_path = NULL, *forever_script_path = NULL;
  (void)asprintf(&work_dir, "%s/run_%ld_%d",
                 campaign_dir_path ? campaign_dir_path : "",
                 (long)time(NULL), (int)getpid());
  if (!campaign_dir_path || !mkdir_p(campaign_dir_path) ||
      !work_dir || !mkdir_p(work_dir)) {
    printf("{\"ok\":false,\"error\":\"fuzz-all-workdir-failed\"}\n");
    free(campaign_dir_path);
    free(campaign_dir_rel);
    free(work_dir);
    return 2;
  }
  (void)asprintf(&report_dir, "%s/reports", work_dir);
  (void)asprintf(&log_dir, "%s/logs", work_dir);
  (void)asprintf(&command_path, "%s/command.txt", work_dir);
  (void)asprintf(&manifest_path, "%s/manifest.json", work_dir);
  (void)asprintf(&forever_script_path, "%s/run-forever.sh", work_dir);
  if (report_dir) ny_ensure_dir_recursive(report_dir);
  if (log_dir) ny_ensure_dir_recursive(log_dir);
  if (command_path) {
    str_buf_t command = {0};
    append_argv_shell_string(&command, argv);
    (void)sb_append_c(&command, '\n');
    (void)write_file_text(command_path, command.data ? command.data : "\n");
    free(command.data);
  }

  string_list_t rows = {0}, failures = {0};
  bool continue_running = true;
  char *lane_json = NULL;
  double run_start_ms = now_ms();

#define FUZZ_ALL_RUN(name_value, phase_value, timeout_value, required_value, argv_builder) do { \
    if (continue_running) {                                                                    \
      free(lane_json);                                                                         \
      lane_json = fuzz_all_report_path(report_dir, (name_value));                              \
      char *lane_markdown = path_with_suffix_ext(lane_json, "", ".md");                        \
      char *lane_argv[48];                                                                      \
      int la = 0;                                                                               \
      argv_builder;                                                                             \
      lane_argv[la] = NULL;                                                                     \
      (void)fuzz_all_add_step(root, (name_value), (phase_value), lane_argv, lane_json,          \
                              (timeout_value), log_dir, (required_value), fail_fast,            \
                              &rows, &failures, &continue_running);                             \
      free(lane_markdown);                                                                      \
    }                                                                                           \
  } while (0)

  FUZZ_ALL_RUN("corpus_prepare", "setup", 180.0, true, {
    lane_argv[la++] = g_self_path; lane_argv[la++] = "fuzz"; lane_argv[la++] = "corpus";
    lane_argv[la++] = "prepare"; lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
  });

  FUZZ_ALL_RUN("workspace_audit", "setup", 120.0, true, {
    lane_argv[la++] = g_self_path; lane_argv[la++] = "fuzz"; lane_argv[la++] = "workspace";
    lane_argv[la++] = "audit"; lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
  });

  char harness_limit_buf[32], libs_limit_buf[32], kernel_limit_buf[32];
  snprintf(harness_limit_buf, sizeof(harness_limit_buf), "%d", harness_limit);
  snprintf(libs_limit_buf, sizeof(libs_limit_buf), "%d", libs_limit);
  snprintf(kernel_limit_buf, sizeof(kernel_limit_buf), "%d", kernel_limit);

  if (lane_filter_afl) {
    fuzz_all_add_skip(root, &rows, "harness_all", "library",
                      "filtered by --only-lane afl");
    fuzz_all_add_skip(root, &rows, "libs_import", "library",
                      "filtered by --only-lane afl");
    fuzz_all_add_skip(root, &rows, "kernels_smoke", "library",
                      "filtered by --only-lane afl");
  } else {
    FUZZ_ALL_RUN("harness_all", "library", synth_timeout_s * (smoke ? 3.0 : 16.0), true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "fuzz"; lane_argv[la++] = "harness";
      lane_argv[la++] = "smoke"; lane_argv[la++] = "--target"; lane_argv[la++] = "all";
      lane_argv[la++] = "--limit"; lane_argv[la++] = harness_limit_buf;
      lane_argv[la++] = "--timeout-s"; lane_argv[la++] = smoke ? "6" : "12";
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });

    FUZZ_ALL_RUN("libs_import", "library", synth_timeout_s * (smoke ? 4.0 : 24.0), true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "fuzz"; lane_argv[la++] = "libs";
      lane_argv[la++] = "smoke"; lane_argv[la++] = "--mode"; lane_argv[la++] = "import";
      lane_argv[la++] = "--limit"; lane_argv[la++] = libs_limit_buf;
      lane_argv[la++] = "--timeout-s"; lane_argv[la++] = smoke ? "4" : "8";
      if (scratch_root_arg && *scratch_root_arg) {
        lane_argv[la++] = "--scratch-root"; lane_argv[la++] = (char *)scratch_root_arg;
      }
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });

    FUZZ_ALL_RUN("kernels_smoke", "library", synth_timeout_s * (smoke ? 5.0 : 18.0), true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "fuzz"; lane_argv[la++] = "kernels";
      lane_argv[la++] = "smoke";
      lane_argv[la++] = "--limit"; lane_argv[la++] = kernel_limit_buf;
      lane_argv[la++] = "--timeout-s"; lane_argv[la++] = smoke ? "20" : "30";
      lane_argv[la++] = "--strict-signal";
      lane_argv[la++] = "--min-signal-ns"; lane_argv[la++] = "1000000";
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });
  }

  if (skip_compiler_audit) {
    fuzz_all_add_skip(root, &rows, "compiler_std_audit", "compiler",
                      lane_filter_afl ? "filtered by --only-lane afl" :
                                        "disabled by --no-compiler-audit");
  } else {
    FUZZ_ALL_RUN("compiler_std_audit", "compiler", smoke ? 120.0 : 300.0, true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "compiler"; lane_argv[la++] = "std-audit";
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
      if (lane_markdown && *lane_markdown) {
        lane_argv[la++] = "--markdown"; lane_argv[la++] = lane_markdown;
      }
    });
  }

  if (skip_compiler_findings) {
    fuzz_all_add_skip(root, &rows, "compiler_findings", "compiler",
                      lane_filter_afl ? "filtered by --only-lane afl" :
                                        "disabled by --no-compiler-findings");
  } else {
    FUZZ_ALL_RUN("compiler_findings", "compiler", smoke ? 45.0 : 90.0, true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "compiler"; lane_argv[la++] = "findings";
      lane_argv[la++] = "--timeout-s"; lane_argv[la++] = smoke ? "15" : "30";
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });
  }

  if (skip_known_bugs) {
    fuzz_all_add_skip(root, &rows, "compiler_known_bugs", "compiler",
                      lane_filter_afl ? "filtered by --only-lane afl" :
                                        "disabled by --no-known-bugs");
  } else {
    FUZZ_ALL_RUN("compiler_known_bugs", "compiler", smoke ? 60.0 : 180.0, true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "compiler"; lane_argv[la++] = "known-bugs";
      lane_argv[la++] = "--timeout-s"; lane_argv[la++] = smoke ? "20" : "30";
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });
  }

  if (skip_prove) {
    fuzz_all_add_skip(root, &rows, "prove_lab", "proof",
                      lane_filter_afl ? "filtered by --only-lane afl" :
                                        "disabled by --no-prove");
  } else {
    FUZZ_ALL_RUN("prove_lab", "proof", prove_timeout_s * (smoke ? 8.0 : 10.0) + 120.0, true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "prove"; lane_argv[la++] = "lab";
      if (smoke || duration_s < 1800.0) lane_argv[la++] = "--fast";
      lane_argv[la++] = "--timeout-s"; lane_argv[la++] = prove_timeout_buf;
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });
  }

  if (skip_perf) {
    fuzz_all_add_skip(root, &rows, "perf_triage", "perf",
                      lane_filter_afl ? "filtered by --only-lane afl" :
                                        "disabled by --no-perf");
  } else {
    FUZZ_ALL_RUN("perf_triage", "perf", perf_wall_timeout_s + 60.0, true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "perf"; lane_argv[la++] = "triage";
      if (smoke || duration_s < 1800.0) lane_argv[la++] = "--fast";
      lane_argv[la++] = "--limit"; lane_argv[la++] = perf_limit_buf;
      lane_argv[la++] = "--timeout-s"; lane_argv[la++] = perf_timeout_buf;
      lane_argv[la++] = "--bench-timeout-s"; lane_argv[la++] = perf_wall_timeout_buf;
      if (scratch_root_arg && *scratch_root_arg) {
        lane_argv[la++] = "--scratch-root"; lane_argv[la++] = (char *)scratch_root_arg;
      }
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });
  }

  if (skip_synth) {
    fuzz_all_add_skip(root, &rows, "synth_lanes", "synth",
                      lane_filter_afl ? "filtered by --only-lane afl" :
                                        "disabled by --no-synth");
  } else {
    static const char *generators[] = {"mixed", "ir", "stress"};
    for (int i = 0; i < (int)(sizeof(generators) / sizeof(generators[0])); ++i) {
      char lane_name[64], lane_seed[32], out_dir[256], build_dir[256];
      snprintf(lane_name, sizeof(lane_name), "synth_%s", generators[i]);
      snprintf(lane_seed, sizeof(lane_seed), "%d", seed + 1009 * (i + 1));
      snprintf(out_dir, sizeof(out_dir), "%s/generated_%s", work_dir, generators[i]);
      snprintf(build_dir, sizeof(build_dir), "%s/build_%s", work_dir, generators[i]);
      FUZZ_ALL_RUN(lane_name, "synth", synth_timeout_s * (double)(synth_cases + 2), true, {
        lane_argv[la++] = g_self_path; lane_argv[la++] = "synth"; lane_argv[la++] = "generate";
        lane_argv[la++] = "--cases"; lane_argv[la++] = cases_buf;
        lane_argv[la++] = "--seed"; lane_argv[la++] = lane_seed;
        lane_argv[la++] = "--timeout-s"; lane_argv[la++] = timeout_buf;
        lane_argv[la++] = "--profile"; lane_argv[la++] = "optimizer";
        lane_argv[la++] = "--generator"; lane_argv[la++] = (char *)generators[i];
        lane_argv[la++] = "--out"; lane_argv[la++] = out_dir;
        lane_argv[la++] = "--build-dir"; lane_argv[la++] = build_dir;
        lane_argv[la++] = "--runs"; lane_argv[la++] = "1";
        lane_argv[la++] = "--warmup"; lane_argv[la++] = "0";
        lane_argv[la++] = "--capture-failures"; lane_argv[la++] = "--strict-failures";
        if (!skip_known_bugs) lane_argv[la++] = "--quarantine-known-bugs";
        lane_argv[la++] = "--max-reduce-checks"; lane_argv[la++] = smoke ? "40" : "120";
        lane_argv[la++] = "--insane";
        if (smoke) lane_argv[la++] = "--fast";
        lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
      });
      }

    FUZZ_ALL_RUN("synth_pure", "synth", synth_timeout_s * (smoke ? 6.0 : 10.0), true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "selftest"; lane_argv[la++] = "synth-pure";
      if (scratch_root_arg && *scratch_root_arg) {
        lane_argv[la++] = "--scratch-root"; lane_argv[la++] = (char *)scratch_root_arg;
      }
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });

    char campaign_cases_buf[32];
    snprintf(campaign_cases_buf, sizeof(campaign_cases_buf), "%d", campaign_cases);
    FUZZ_ALL_RUN("campaign_core", "synth-campaign", synth_timeout_s * (double)(campaign_cases + 4), true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "campaign"; lane_argv[la++] = "run";
      lane_argv[la++] = "--lanes"; lane_argv[la++] = "typed,optimizer,torture,ir,stress,random,cbridge,afl";
      lane_argv[la++] = "--profile"; lane_argv[la++] = "optimizer";
      lane_argv[la++] = "--cases"; lane_argv[la++] = campaign_cases_buf;
      lane_argv[la++] = "--seed"; lane_argv[la++] = seed_buf;
      lane_argv[la++] = "--timeout-s"; lane_argv[la++] = timeout_buf;
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
      if (!skip_known_bugs) lane_argv[la++] = "--quarantine-known-bugs";
      if (smoke) lane_argv[la++] = "--fast";
    });
  }

  if (lane_filter_afl) {
    fuzz_all_add_skip(root, &rows, "snippets_mixed", "frontend",
                      "filtered by --only-lane afl");
    fuzz_all_add_skip(root, &rows, "frontend_corpus", "frontend",
                      "filtered by --only-lane afl");
  } else {
    FUZZ_ALL_RUN("snippets_mixed", "frontend", synth_timeout_s * 3.0, true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "fuzz"; lane_argv[la++] = "snippets";
      lane_argv[la++] = "--iterations"; lane_argv[la++] = snippets_buf;
      lane_argv[la++] = "--mode"; lane_argv[la++] = "mixed";
      lane_argv[la++] = "--timeout-s"; lane_argv[la++] = timeout_buf;
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });

    FUZZ_ALL_RUN("frontend_corpus", "frontend", synth_timeout_s * 3.0, true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "fuzz"; lane_argv[la++] = "frontend";
      lane_argv[la++] = "--rounds"; lane_argv[la++] = frontend_buf;
      lane_argv[la++] = "--timeout-s"; lane_argv[la++] = timeout_buf;
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });
  }

  if (skip_gc) {
    fuzz_all_add_skip(root, &rows, "gc_soak", "gc",
                      lane_filter_afl ? "filtered by --only-lane afl" :
                                        "disabled by --no-gc");
  } else {
    FUZZ_ALL_RUN("gc_soak", "gc", gc_budget_s + 1200.0, true, {
      lane_argv[la++] = g_self_path; lane_argv[la++] = "fuzz"; lane_argv[la++] = "gc"; lane_argv[la++] = "run";
      lane_argv[la++] = "--profile=soak";
      lane_argv[la++] = "--budget-s"; lane_argv[la++] = gc_budget_buf;
      lane_argv[la++] = "--threads"; lane_argv[la++] = threads_buf;
      lane_argv[la++] = "--checkpoint-s"; lane_argv[la++] = smoke ? "0" : "1800";
      lane_argv[la++] = "--fail-fast"; lane_argv[la++] = "--validate-gc";
      if (skip_sanitizers) lane_argv[la++] = "--no-sanitizers";
      if (smoke) { lane_argv[la++] = "--ny-cases"; lane_argv[la++] = "1"; lane_argv[la++] = "--ny-rounds"; lane_argv[la++] = "4"; }
      lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
    });
  }

  bool afl_available = command_exists_path("afl-fuzz");
  if (skip_afl) {
    fuzz_all_add_skip(root, &rows, "afl", "afl", "disabled by --no-afl");
  } else if (!afl_available) {
    fuzz_all_add_skip(root, &rows, "afl", "afl", "afl-fuzz not found in PATH");
  } else if (afl_compiler_minutes <= 0 && afl_parser_minutes <= 0) {
    fuzz_all_add_skip(root, &rows, "afl", "afl", "budget too short for productive AFL lane");
  } else {
    if (afl_compiler_minutes > 0) {
      FUZZ_ALL_RUN("afl_compiler", "afl",
                   (double)afl_compiler_minutes * 60.0 * 2.0 + 300.0, true, {
        lane_argv[la++] = g_self_path; lane_argv[la++] = "fuzz"; lane_argv[la++] = "afl"; lane_argv[la++] = "run";
        lane_argv[la++] = "--target"; lane_argv[la++] = "compiler";
        lane_argv[la++] = "--minutes"; lane_argv[la++] = afl_compiler_minutes_buf;
        lane_argv[la++] = "--timeout-ms"; lane_argv[la++] = "10000";
        lane_argv[la++] = "--aggressive"; lane_argv[la++] = "--power-schedule"; lane_argv[la++] = "explore";
        lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
      });
    } else {
      fuzz_all_add_skip(root, &rows, "afl_compiler", "afl", "compiler AFL budget too short");
    }
    if (afl_parser_minutes > 0) {
      int parser_targets = parser_fuzz_target_count();
      FUZZ_ALL_RUN("afl_parsers", "afl",
                   (double)afl_parser_minutes * 60.0 * (double)parser_targets + 300.0, true, {
        lane_argv[la++] = g_self_path; lane_argv[la++] = "fuzz"; lane_argv[la++] = "afl"; lane_argv[la++] = "run";
        lane_argv[la++] = "--target"; lane_argv[la++] = "parsers";
        lane_argv[la++] = "--minutes"; lane_argv[la++] = afl_parser_minutes_buf;
        lane_argv[la++] = "--timeout-ms"; lane_argv[la++] = "10000";
        lane_argv[la++] = "--aggressive"; lane_argv[la++] = "--power-schedule"; lane_argv[la++] = "rare";
        lane_argv[la++] = "--json"; lane_argv[la++] = lane_json;
      });
    } else {
      fuzz_all_add_skip(root, &rows, "afl_parsers", "afl", "parser AFL budget too short");
    }
  }

  char nytrix_make[4096];
  bool have_nytrix_make = false;
  if (snprintf(nytrix_make, sizeof(nytrix_make), "%s/../nytrix/make", root) > 0)
    have_nytrix_make = access(nytrix_make, X_OK) == 0;
  if (skip_nytrix) {
    fuzz_all_add_skip(root, &rows, "nytrix_fuzz", "nytrix",
                      deny_nytrix ? "disabled by --no-nytrix" :
                                    "disabled by default; pass --allow-nytrix");
  } else if (!have_nytrix_make) {
    fuzz_all_add_skip(root, &rows, "nytrix_fuzz", "nytrix", "sibling /home/e/nytrix/make not found");
  } else {
    char *nytrix_argv[] = {nytrix_make, "fuzz", NULL};
    (void)fuzz_all_add_step(root, "nytrix_fuzz", "nytrix", nytrix_argv, NULL,
                            smoke ? 180.0 : 900.0, log_dir, true, fail_fast,
                            &rows, &failures, &continue_running);
  }

  if (skip_sanitizers || skip_nytrix) {
    fuzz_all_add_skip(root, &rows, "nytrix_sanitizers", "sanitizer",
                      lane_filter_afl ? "filtered by --only-lane afl" :
                      (skip_sanitizers ? "disabled by --no-sanitizers" :
                                        (deny_nytrix ? "disabled by --no-nytrix" :
                                                       "disabled by default; pass --allow-nytrix")));
  } else if (!have_nytrix_make) {
    fuzz_all_add_skip(root, &rows, "nytrix_sanitizers", "sanitizer", "sibling /home/e/nytrix/make not found");
  } else if (smoke || duration_s < 1800.0) {
    fuzz_all_add_skip(root, &rows, "nytrix_sanitizers", "sanitizer", "budget too short for full asan/ubsan lanes");
  } else {
    char *asan_argv[] = {nytrix_make, "asan", NULL};
    (void)fuzz_all_add_step(root, "nytrix_asan", "sanitizer", asan_argv, NULL,
                            1800.0, log_dir, true, fail_fast, &rows, &failures, &continue_running);
    char *ubsan_argv[] = {nytrix_make, "ubsan", NULL};
    (void)fuzz_all_add_step(root, "nytrix_ubsan", "sanitizer", ubsan_argv, NULL,
                            1800.0, log_dir, true, fail_fast, &rows, &failures, &continue_running);
  }

#undef FUZZ_ALL_RUN

  str_buf_t forever_opts = {0};
  (void)sb_append(&forever_opts, fail_fast ? " --fail-fast" : " --keep-going");
  if (skip_afl) (void)sb_append(&forever_opts, " --no-afl");
  if (skip_gc) (void)sb_append(&forever_opts, " --no-gc");
  if (skip_synth) (void)sb_append(&forever_opts, " --no-synth");
  if (skip_sanitizers) (void)sb_append(&forever_opts, " --no-sanitizers");
  if (skip_nytrix) (void)sb_append(&forever_opts, " --no-nytrix");
  else if (allow_nytrix) (void)sb_append(&forever_opts, " --allow-nytrix");
  if (skip_compiler_audit) (void)sb_append(&forever_opts, " --no-compiler-audit");
  if (skip_compiler_findings) (void)sb_append(&forever_opts, " --no-compiler-findings");
  if (skip_known_bugs) (void)sb_append(&forever_opts, " --no-known-bugs");
  if (skip_prove) (void)sb_append(&forever_opts, " --no-prove");
  if (skip_perf) (void)sb_append(&forever_opts, " --no-perf");
  if (lane_filter_afl) (void)sb_append(&forever_opts, " --only-lane afl");
  const char *handoff_profile = profile && *profile ? profile : "insane";
  const char *handoff_threads = threads_arg && *threads_arg ? threads_arg : NYTRIX_DEFAULT_FUZZ_THREADS;
  double handoff_hours = smoke ? 8.0 : duration_s / 3600.0;
  if (handoff_hours <= 0.0) handoff_hours = 8.0;
  char handoff_hours_buf[32];
  snprintf(handoff_hours_buf, sizeof(handoff_hours_buf), "%.2f", handoff_hours);
  if (forever_script_path) {
    str_buf_t script = {0};
    (void)sb_append(&script, "#!/usr/bin/env bash\n");
    (void)sb_append(&script, "set -euo pipefail\n");
    append_repo_cache_env_script(&script, root);
    append_low_priority_shell_helper(&script);
    append_load_wait_shell_helper(&script);
    append_space_guard_shell_helper(&script);
    append_campaign_lock_shell_helper(&script);
    (void)sb_appendf(&script,
                     "dir=");
    (void)sb_append_json_str(&script, campaign_dir_cmd);
    (void)sb_append(&script, "\n");
    (void)sb_appendf(&script,
                     "mkdir -p \"$dir\"\n"
                     "nytrix_acquire_campaign_lock \"$dir\"\n"
                     "while true; do\n"
                     "  ts=$(date +%%Y%%m%%d-%%H%%M%%S)\n"
                     "  report=\"$dir/insane-${ts}.json\"\n"
                     "  audit=\"$dir/insane-${ts}-audit.json\"\n"
                     "  findings=\"$dir/insane-${ts}-findings.json\"\n"
                     "  findings_md=\"$dir/insane-${ts}-findings.md\"\n"
                     "  coverage=\"$dir/insane-${ts}-coverage.json\"\n"
                     "  coverage_md=\"$dir/insane-${ts}-coverage.md\"\n"
                     "  coverage_latest=\"$dir/coverage.json\"\n"
                     "  coverage_latest_md=\"$dir/coverage.md\"\n"
                     "  history=\"$dir/history.json\"\n"
                     "  history_md=\"$dir/history.md\"\n"
                     "  worklist=\"$dir/worklist.json\"\n"
                     "  worklist_md=\"$dir/worklist.md\"\n"
                     "  plan=\"$dir/plan.json\"\n"
                     "  plan_md=\"$dir/plan.md\"\n"
                     "  status=\"$dir/status.json\"\n"
                     "  status_md=\"$dir/status.md\"\n"
                     "  progress=\"$dir/progress.json\"\n"
                     "  progress_md=\"$dir/progress.md\"\n"
                     "  old_paths=\"$dir/old-paths.json\"\n"
                     "  old_paths_md=\"$dir/old-paths.md\"\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json \"$old_paths\" --markdown \"$old_paths_md\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz old-paths stopped with rc=$rc; inspect $old_paths\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  if [ -f \"$history\" ]; then\n"
                     "    nytrix_low_priority ./build/nytrix fuzz all status --refresh --strict --allow-full-pressure-remediation --no-script --dir \"$dir\" --history \"$history\" --worklist \"$worklist\" --coverage \"$coverage_latest\" --plan \"$plan\" --target-thread-years %s --hours %s --threads %s --profile %s --json \"$status\" --markdown \"$status_md\"\n"
                     "    rc=$?\n"
                     "    if [ \"$rc\" -ne 0 ]; then\n"
                     "      echo \"all-fuzz pre-run status stopped with rc=$rc; inspect $status\"\n"
                     "      exit \"$rc\"\n"
                     "    fi\n"
                     "    nytrix_low_priority ./build/nytrix fuzz all progress --status \"$status\" --json \"$progress\" --markdown \"$progress_md\"\n"
                     "    rc=$?\n"
                     "    if [ \"$rc\" -ne 0 ]; then\n"
                     "      echo \"all-fuzz pre-run progress stopped with rc=$rc; inspect $progress\"\n"
                     "      exit \"$rc\"\n"
                     "    fi\n"
                     "    if grep -q '\"campaign_complete\":true' \"$status\"; then\n"
                     "      echo \"campaign target complete: $status\"\n"
                     "      exit 0\n"
                     "    fi\n"
                     "  fi\n"
                     "  nytrix_require_free_space\n"
                     "  nytrix_wait_for_load\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all run --profile %s --hours %s --threads %s%s "
                     "--dir \"$dir\" --target-thread-years %s --json \"$report\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz stopped with rc=$rc; inspect $report\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all audit --report \"$report\" --strict --json \"$audit\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz audit stopped with rc=$rc; inspect $audit and $report\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all findings --report \"$report\" --json \"$findings\" --markdown \"$findings_md\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz findings stopped with rc=$rc; inspect $findings and $report\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all coverage --report \"$report\" --json \"$coverage\" --markdown \"$coverage_md\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz coverage stopped with rc=$rc; inspect $coverage and $report\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all history --dir \"$dir\" --json \"$history\" --markdown \"$history_md\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz history stopped with rc=$rc; inspect $history\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all coverage --strict --history \"$history\" --target-thread-years %s --hours %s --threads %s --profile %s --json \"$coverage_latest\" --markdown \"$coverage_latest_md\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz history coverage stopped with rc=$rc; inspect $coverage_latest and $history\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all worklist --history \"$history\" --json \"$worklist\" --markdown \"$worklist_md\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz worklist stopped with rc=$rc; inspect $worklist\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all plan --dir \"$dir\" --history \"$history\" --worklist \"$worklist\" --coverage \"$coverage_latest\" --target-thread-years %s --hours %s --threads %s --profile %s --json \"$plan\" --markdown \"$plan_md\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz plan stopped with rc=$rc; inspect $plan\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json \"$old_paths\" --markdown \"$old_paths_md\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz final old-paths stopped with rc=$rc; inspect $old_paths\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all status --strict --allow-full-pressure-remediation --dir \"$dir\" --history \"$history\" --worklist \"$worklist\" --coverage \"$coverage_latest\" --plan \"$plan\" --target-thread-years %s --hours %s --threads %s --profile %s --json \"$status\" --markdown \"$status_md\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz status stopped with rc=$rc; inspect $status\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  nytrix_low_priority ./build/nytrix fuzz all progress --status \"$status\" --json \"$progress\" --markdown \"$progress_md\"\n"
                     "  rc=$?\n"
                     "  if [ \"$rc\" -ne 0 ]; then\n"
                     "    echo \"all-fuzz progress stopped with rc=$rc; inspect $progress\"\n"
                     "    exit \"$rc\"\n"
                     "  fi\n"
                     "  sleep 5\n"
                     "done\n",
                       target_arg && *target_arg ? target_arg : "10",
                       handoff_hours_buf, handoff_threads, handoff_profile,
                       handoff_profile, handoff_hours_buf, handoff_threads,
                       forever_opts.data ? forever_opts.data : "",
                       target_arg && *target_arg ? target_arg : "10",
                       target_arg && *target_arg ? target_arg : "10",
                       handoff_hours_buf, handoff_threads, handoff_profile,
                       target_arg && *target_arg ? target_arg : "10",
                       handoff_hours_buf, handoff_threads, handoff_profile,
                       target_arg && *target_arg ? target_arg : "10",
                     handoff_hours_buf, handoff_threads, handoff_profile);
    if (script.data && write_file_text(forever_script_path, script.data))
      (void)chmod(forever_script_path, 0755);
    free(script.data);
  }

  int total_quarantined_known_bugs = 0, lanes_with_quarantine = 0;
  int total_finding_count = 0, total_finding_live = 0;
  int total_finding_cleared = 0, total_finding_missing = 0;
  int total_known_bug_count = 0, total_known_bug_reproduced = 0;
  int total_known_bug_fixed_candidates = 0, total_known_bug_lost_signal = 0;
  int total_known_bug_baseline_failures = 0, total_perf_hotspots = 0;
  double total_perf_max_ratio = 0.0;
  char total_perf_max_case[128] = {0};
  for (int i = 0; i < rows.count; ++i) {
    double quarantined = 0.0;
    if (extract_json_number(rows.items[i], "sub_quarantined_known_bugs", &quarantined) &&
        quarantined > 0.0) {
      total_quarantined_known_bugs += (int)quarantined;
      ++lanes_with_quarantine;
    }
    double value = 0.0;
    if (extract_json_number(rows.items[i], "sub_finding_count", &value))
      total_finding_count += (int)value;
    if (extract_json_number(rows.items[i], "sub_finding_live", &value))
      total_finding_live += (int)value;
    if (extract_json_number(rows.items[i], "sub_finding_cleared", &value))
      total_finding_cleared += (int)value;
    if (extract_json_number(rows.items[i], "sub_finding_missing", &value))
      total_finding_missing += (int)value;
    if (extract_json_number(rows.items[i], "sub_known_bug_count", &value))
      total_known_bug_count += (int)value;
    if (extract_json_number(rows.items[i], "sub_known_bug_reproduced", &value))
      total_known_bug_reproduced += (int)value;
    if (extract_json_number(rows.items[i], "sub_known_bug_fixed_candidates", &value))
      total_known_bug_fixed_candidates += (int)value;
    if (extract_json_number(rows.items[i], "sub_known_bug_lost_signal", &value))
      total_known_bug_lost_signal += (int)value;
    if (extract_json_number(rows.items[i], "sub_known_bug_baseline_failures", &value))
      total_known_bug_baseline_failures += (int)value;
    if (extract_json_number(rows.items[i], "sub_perf_hotspots", &value))
      total_perf_hotspots += (int)value;
    if (extract_json_number(rows.items[i], "sub_perf_max_ratio", &value) &&
        value > total_perf_max_ratio) {
      total_perf_max_ratio = value;
      char *case_name = json_string_or_empty(rows.items[i], "sub_perf_max_case");
      snprintf(total_perf_max_case, sizeof(total_perf_max_case), "%s",
               case_name ? case_name : "");
      free(case_name);
    }
  }
  bool full_pressure =
      !smoke && duration_s >= 1800.0 && !skip_afl && afl_available &&
      !skip_gc && !skip_synth && !skip_compiler_audit &&
      !skip_compiler_findings && !skip_known_bugs && !skip_prove &&
      !skip_perf;

      double run_end_ms = now_ms();
      double actual_duration_s = 0.0;
      if (run_start_ms > 0.0 && run_end_ms > run_start_ms)
        actual_duration_s = (run_end_ms - run_start_ms) / 1000.0;
      if (actual_duration_s <= 0.0) actual_duration_s = duration_s;
      double effective_budget_s = duration_s;
      if (failures.count > 0 && actual_duration_s > 0.0 &&
          actual_duration_s < effective_budget_s)
        effective_budget_s = actual_duration_s;
      if (effective_budget_s <= 0.0) effective_budget_s = duration_s;
      if (duration_s > 0.0 && effective_budget_s > duration_s)
        effective_budget_s = duration_s;

      str_buf_t extra = {0};
      (void)sb_append(&extra, ",\"profile\":");
      (void)sb_append_json_str(&extra, profile ? profile : "insane");
      (void)sb_append(&extra, ",\"lane_filter\":");
      (void)sb_append_json_str(&extra, lane_filter_active ?
                                   lane_filter_arg : "all");
      (void)sb_appendf(&extra, ",\"lane_filter_active\":%s",
                       lane_filter_active ? "true" : "false");
      (void)sb_appendf(&extra,
                       ",\"low_priority_enabled\":%s,"
                       "\"low_priority_applied\":%s,"
                       "\"nice_target\":%d,\"nice_value\":%d",
                       low_priority_enabled ? "true" : "false",
                       run_low_priority_applied ? "true" : "false",
                       run_nice_target, run_nice_value);
      (void)sb_appendf(&extra,
                       ",\"smoke\":%s,\"full_pressure\":%s,"
                       "\"duration_s\":%.3f,\"budget_s\":%.3f,"
                       "\"requested_duration_s\":%.3f,"
                       "\"effective_budget_s\":%.3f,\"seed\":%d,"
                         "\"cpu_threads\":%d,\"threads\":%d,\"synth_cases\":%d,"
                   "\"campaign_cases\":%d,\"snippets\":%d,\"frontend_rounds\":%d,"
                   "\"harness_limit_per_target\":%d,\"libs_import_limit\":%d,"
                   "\"kernel_run_limit\":%d,\"prove_timeout_s\":%.3f,"
                   "\"perf_timeout_s\":%.3f,\"perf_limit\":%d,"
                   "\"gc_budget_s\":%.3f,\"afl_compiler_minutes_per_target\":%d,"
                   "\"afl_parser_minutes_per_target\":%d,"
                   "\"quarantine_known_bugs\":%s,\"quarantined_known_bugs\":%d,"
                   "\"lanes_with_quarantine\":%d,"
                   "\"finding_count\":%d,\"finding_live\":%d,"
                   "\"finding_cleared\":%d,\"finding_missing\":%d,"
                   "\"known_bug_count\":%d,\"known_bug_reproduced\":%d,"
                   "\"known_bug_fixed_candidates\":%d,"
                   "\"known_bug_lost_signal\":%d,"
                   "\"known_bug_baseline_failures\":%d,"
                   "\"perf_hotspots\":%d,\"perf_max_ratio\":%.4f,"
                   "\"fail_fast\":%s,\"afl_available\":%s,"
                   "\"allow_nytrix\":%s,\"skip_nytrix\":%s,"
                   "\"skip_sanitizers\":%s",
                         smoke ? "true" : "false",
                         full_pressure ? "true" : "false",
                         actual_duration_s, duration_s, duration_s,
                         effective_budget_s, seed, cpu_threads, threads, synth_cases,
                   campaign_cases, snippets, frontend_rounds, harness_limit, libs_limit,
                   kernel_limit, prove_timeout_s, perf_timeout_s, perf_limit,
                   gc_budget_s,
                   afl_compiler_minutes, afl_parser_minutes,
                   skip_known_bugs ? "false" : "true",
                   total_quarantined_known_bugs, lanes_with_quarantine,
                   total_finding_count, total_finding_live,
                   total_finding_cleared, total_finding_missing,
                   total_known_bug_count, total_known_bug_reproduced,
                   total_known_bug_fixed_candidates,
                   total_known_bug_lost_signal,
                   total_known_bug_baseline_failures,
                   total_perf_hotspots, total_perf_max_ratio,
                   fail_fast ? "true" : "false", afl_available ? "true" : "false",
                   allow_nytrix ? "true" : "false",
                   skip_nytrix ? "true" : "false",
                   skip_sanitizers ? "true" : "false");
  (void)sb_append(&extra, ",\"perf_max_case\":");
  (void)sb_append_json_str(&extra, total_perf_max_case);
  (void)sb_append(&extra, ",\"thread_request\":");
  (void)sb_append_json_str(&extra, threads_arg && *threads_arg ? threads_arg : NYTRIX_DEFAULT_FUZZ_THREADS);
  if (scratch_root_arg && *scratch_root_arg) {
    (void)sb_append(&extra, ",\"scratch_root\":");
    append_rel_json_str(&extra, root, scratch_root_arg);
  }
  (void)sb_append(&extra, ",\"campaign_dir\":");
  append_rel_json_str(&extra, root, campaign_dir_path ? campaign_dir_path : "");
  (void)sb_append(&extra, ",\"work_dir\":");
  append_rel_json_str(&extra, root, work_dir ? work_dir : "");
  (void)sb_append(&extra, ",\"report_dir\":");
  append_rel_json_str(&extra, root, report_dir ? report_dir : "");
  (void)sb_append(&extra, ",\"log_dir\":");
  append_rel_json_str(&extra, root, log_dir ? log_dir : "");
  (void)sb_append(&extra, ",\"command_file\":");
  append_rel_json_str(&extra, root, command_path ? command_path : "");
  (void)sb_append(&extra, ",\"manifest\":");
  append_rel_json_str(&extra, root, manifest_path ? manifest_path : "");
  (void)sb_append(&extra, ",\"forever_script\":");
  append_rel_json_str(&extra, root, forever_script_path ? forever_script_path : "");
  if (manifest_path) {
    str_buf_t invocation = {0}, manifest = {0};
    append_argv_shell_string(&invocation, argv);
    (void)sb_append(&manifest, "{\"kind\":\"fuzz-all-manifest\",\"command\":");
    (void)sb_append_json_str(&manifest, invocation.data ? invocation.data : "");
    (void)sb_append(&manifest, ",\"summary\":{\"engine\":\"nytrix_core\"");
    if (extra.data) (void)sb_append(&manifest, extra.data);
    (void)sb_append(&manifest, "}}\n");
    if (manifest.data) (void)write_file_text(manifest_path, manifest.data);
    free(invocation.data);
    free(manifest.data);
  }
  char *report = build_native_report_json(&rows, &failures, "fuzz-all", extra.data);
  int rc = emit_native_report(report, json_path, "all fuzz", rows.count, failures.count);
  free(forever_opts.data);
  free(extra.data);
  free(lane_json);
  free(campaign_dir_path);
  free(campaign_dir_rel);
  free(work_dir);
  free(report_dir);
  free(log_dir);
  free(command_path);
  free(manifest_path);
  free(forever_script_path);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static bool cli_arg_matches_name(const char *arg, const char *name) {
  if (!arg || !name) return false;
  size_t n = strlen(name);
  return strcmp(arg, name) == 0 || (strncmp(arg, name, n) == 0 && arg[n] == '=');
}

static bool cli_has_named_arg(int argc, char **argv, int start, const char *name) {
  for (int i = start; i < argc; ++i)
    if (cli_arg_matches_name(argv[i], name)) return true;
  return false;
}

static bool cli_has_any_duration_arg(int argc, char **argv, int start) {
  return cli_has_named_arg(argc, argv, start, "--budget-s") ||
         cli_has_named_arg(argc, argv, start, "--duration-s") ||
         cli_has_named_arg(argc, argv, start, "--minutes") ||
         cli_has_named_arg(argc, argv, start, "--hours");
}

static bool fuzz_auto_skip_copy_arg(int argc, char **argv, int *index) {
  const char *arg = argv[*index];
  if (strcmp(arg, "--once") == 0 || strcmp(arg, "--single") == 0 ||
      strcmp(arg, "--forever") == 0 || strcmp(arg, "--auto") == 0 ||
      strcmp(arg, "--allow-nytrix") == 0)
    return true;
  if (cli_arg_matches_name(arg, "--runs")) {
    if (strcmp(arg, "--runs") == 0 && *index + 1 < argc) ++*index;
    return true;
  }
  if (cli_arg_matches_name(arg, "--json")) {
    if (strcmp(arg, "--json") == 0 && *index + 1 < argc) ++*index;
    return true;
  }
  if (cli_arg_matches_name(arg, "--dir") || cli_arg_matches_name(arg, "--history-dir")) {
    if ((strcmp(arg, "--dir") == 0 || strcmp(arg, "--history-dir") == 0) &&
        *index + 1 < argc)
      ++*index;
    return true;
  }
  if (cli_arg_matches_name(arg, "--target-thread-years") ||
      cli_arg_matches_name(arg, "--target-years") ||
      cli_arg_matches_name(arg, "--target") ||
      cli_arg_matches_name(arg, "--hours-per-run") ||
      cli_arg_matches_name(arg, "--run-hours") ||
      cli_arg_matches_name(arg, "--hours")) {
    if ((strcmp(arg, "--target-thread-years") == 0 ||
         strcmp(arg, "--target-years") == 0 ||
         strcmp(arg, "--target") == 0 ||
         strcmp(arg, "--hours-per-run") == 0 ||
         strcmp(arg, "--run-hours") == 0 ||
         strcmp(arg, "--hours") == 0) &&
        *index + 1 < argc)
      ++*index;
    return true;
  }
  return false;
}

static int fuzz_auto_refresh_status_guard(const char *root, const char *dir_path,
                                          const char *target_arg,
                                          const char *hours_arg,
                                          const char *threads_arg,
                                          const char *profile_arg,
                                          bool *campaign_complete) {
  if (campaign_complete) *campaign_complete = false;
  const char *dir = dir_path && *dir_path ? dir_path : "build/fuzz/all";
  char *history_path = NULL, *worklist_path = NULL, *coverage_path = NULL;
  char *plan_path = NULL, *status_path = NULL, *status_md_path = NULL;
  (void)asprintf(&history_path, "%s/history.json", dir);
  (void)asprintf(&worklist_path, "%s/worklist.json", dir);
  (void)asprintf(&coverage_path, "%s/coverage.json", dir);
  (void)asprintf(&plan_path, "%s/plan.json", dir);
  (void)asprintf(&status_path, "%s/status.json", dir);
  (void)asprintf(&status_md_path, "%s/status.md", dir);
  if (!history_path || !worklist_path || !coverage_path || !plan_path ||
      !status_path || !status_md_path) {
    free(history_path);
    free(worklist_path);
    free(coverage_path);
    free(plan_path);
    free(status_path);
    free(status_md_path);
    return 2;
  }
  char absolute_history[4096] = {0};
  const char *history_check = history_path;
  if (!path_is_absolute(history_path) && root && *root &&
      path_join(absolute_history, sizeof(absolute_history), root, history_path))
    history_check = absolute_history;
  if (!path_exists_file(history_check)) {
    free(history_path);
    free(worklist_path);
    free(coverage_path);
    free(plan_path);
    free(status_path);
    free(status_md_path);
    return 0;
  }
  char *status_argv[] = {
    g_self_path, "fuzz", "all", "status", "--refresh", "--strict",
    "--no-script",
    "--dir", (char *)dir,
    "--history", history_path,
    "--worklist", worklist_path,
    "--coverage", coverage_path,
    "--plan", plan_path,
    "--target-thread-years", (char *)(target_arg && *target_arg ? target_arg : "10"),
    "--hours", (char *)(hours_arg && *hours_arg ? hours_arg : "8"),
    "--threads", (char *)(threads_arg && *threads_arg ? threads_arg : NYTRIX_DEFAULT_FUZZ_THREADS),
    "--profile", (char *)(profile_arg && *profile_arg ? profile_arg : "insane"),
    "--json", status_path,
    "--markdown", status_md_path,
    NULL
  };
  int rc = cmd_public_fuzz_all_status(29, status_argv);
  if (rc == 0 && campaign_complete) {
    file_buf_t status = {0};
    if (read_file(status_path, &status) && status.data)
      (void)summary_bool_from_report(status.data, "campaign_complete",
                                     campaign_complete);
    free(status.data);
  }
  free(history_path);
  free(worklist_path);
  free(coverage_path);
  free(plan_path);
  free(status_path);
  free(status_md_path);
  return rc;
}

static bool fuzz_all_preflight_skip_copy_arg(int argc, char **argv, int *index) {
  const char *arg = argv[*index];
  if (strcmp(arg, "--include-afl") == 0 || strcmp(arg, "--allow-nytrix") == 0 ||
      strcmp(arg, "--no-nytrix-guard") == 0 || strcmp(arg, "--smoke") == 0 ||
      strcmp(arg, "--fast") == 0 ||
      strcmp(arg, "--allow-dirty-nytrix-baseline") == 0 ||
      strcmp(arg, "--dirty-nytrix-baseline") == 0)
    return true;
  if (cli_arg_matches_name(arg, "--json") ||
      cli_arg_matches_name(arg, "--work-dir") ||
      cli_arg_matches_name(arg, "--dir") ||
      cli_arg_matches_name(arg, "--history-dir")) {
    if ((strcmp(arg, "--json") == 0 || strcmp(arg, "--work-dir") == 0 ||
         strcmp(arg, "--dir") == 0 || strcmp(arg, "--history-dir") == 0) &&
        *index + 1 < argc)
      ++*index;
    return true;
  }
  if (cli_arg_matches_name(arg, "--target-thread-years") ||
      cli_arg_matches_name(arg, "--target-years") ||
      cli_arg_matches_name(arg, "--target") ||
      cli_arg_matches_name(arg, "--hours-per-run") ||
      cli_arg_matches_name(arg, "--run-hours") ||
      cli_arg_matches_name(arg, "--hours")) {
    if ((strcmp(arg, "--target-thread-years") == 0 ||
         strcmp(arg, "--target-years") == 0 ||
         strcmp(arg, "--target") == 0 ||
         strcmp(arg, "--hours-per-run") == 0 ||
         strcmp(arg, "--run-hours") == 0 ||
         strcmp(arg, "--hours") == 0) &&
        *index + 1 < argc)
      ++*index;
    return true;
  }
  return false;
}

static bool path_exists_maybe_root(const char *root, const char *path) {
  if (!path || !*path) return false;
  if (path_is_absolute(path)) return path_exists_file(path);
  char joined[4096];
  if (!path_join(joined, sizeof(joined), root, path)) return false;
  return path_exists_file(joined);
}

static bool read_file_maybe_root(const char *root, const char *path, file_buf_t *out) {
  if (!path || !*path || !out) return false;
  if (path_is_absolute(path)) return read_file(path, out);
  char joined[4096];
  if (!path_join(joined, sizeof(joined), root, path)) return false;
  return read_file(joined, out);
}

static char *make_fuzz_all_preflight_row(const char *root, const char *name,
                                         const char *phase, bool ok, int rc,
                                         const char *report_path,
                                         double sub_rows, double sub_failures,
                                         const char *detail_key,
                                         const char *detail_value,
                                         char *const cmd_argv[]) {
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, name ? name : "");
  (void)sb_append(&row, ",\"kind\":\"fuzz-all-preflight\",\"phase\":");
  (void)sb_append_json_str(&row, phase ? phase : "");
  (void)sb_appendf(&row,
                   ",\"ok\":%s,\"rc\":%d,\"sub_rows\":%.0f,"
                   "\"sub_failures\":%.0f,\"engine\":\"nytrix_core\"",
                   ok ? "true" : "false", rc, sub_rows, sub_failures);
  if (report_path && *report_path) {
    (void)sb_append(&row, ",\"report\":");
    append_rel_json_str(&row, root, report_path);
  }
  if (detail_key && *detail_key) {
    (void)sb_append_c(&row, ',');
    (void)sb_append_json_str(&row, detail_key);
    (void)sb_append_c(&row, ':');
    (void)sb_append_json_str(&row, detail_value ? detail_value : "");
  }
  if (cmd_argv) {
    (void)sb_append(&row, ",\"command\":");
    append_argv_json_array(&row, cmd_argv);
  }
  (void)sb_append_c(&row, '}');
  return sb_take(&row);
}

static void fuzz_all_preflight_add_report_step(const char *root, string_list_t *rows,
                                               string_list_t *failures, const char *name,
                                               const char *phase, int rc,
                                               const char *report_path,
                                               char *const cmd_argv[]) {
  file_buf_t report = {0};
  bool have_report = report_path && *report_path && read_file(report_path, &report) && report.data;
  double failure_count = rc == 0 ? 0.0 : 1.0;
  double row_count = -1.0;
  if (have_report) {
    const char *rows_json = json_top_level_value_after_key(report.data, "rows");
    row_count = (double)count_json_array_items(rows_json);
    if (!extract_json_number(report.data, "failure_count", &failure_count))
      failure_count = json_failures_nonempty(report.data) ? 1.0 : 0.0;
  }
  bool ok = rc == 0 && have_report && failure_count == 0.0;
  if (!ok) {
    (void)string_list_push_take(failures,
                                make_fuzz_failure(root, name ? name : "preflight",
                                                  have_report ? "preflight report failed"
                                                              : "preflight report missing",
                                                  report_path ? report_path : ""));
  }
  (void)string_list_push_take(rows,
                              make_fuzz_all_preflight_row(root, name, phase, ok, rc,
                                                          report_path, row_count,
                                                          failure_count, NULL, NULL,
                                                          cmd_argv));
  free(report.data);
}

typedef struct {
  bool exists;
  bool git_ok;
  bool clean;
  char nytrix_root[4096];
  char *status;
  char *stderr_text;
  int status_rc;
  int diff_rc;
  int staged_diff_rc;
  double elapsed_ms;
  size_t status_bytes;
  size_t diff_bytes;
  size_t staged_diff_bytes;
  char status_hash[32];
  char diff_hash[32];
  char staged_diff_hash[32];
  char state_hash[32];
} nytrix_git_state_t;

static void hash_bytes_hex(const char *data, size_t len, char *out, size_t out_sz) {
  if (!out || out_sz == 0) return;
  snprintf(out, out_sz, "%016" PRIx64, fnv1a64(data ? data : "", len));
}

static void nytrix_git_state_free(nytrix_git_state_t *state) {
  if (!state) return;
  free(state->status);
  free(state->stderr_text);
  memset(state, 0, sizeof(*state));
}

static void nytrix_git_state_compute_state_hash(nytrix_git_state_t *state) {
  if (!state) return;
  str_buf_t raw = {0};
  (void)sb_appendf(&raw, "exists=%d;git=%d;clean=%d;status_rc=%d;diff_rc=%d;staged_rc=%d;",
                   state->exists ? 1 : 0, state->git_ok ? 1 : 0,
                   state->clean ? 1 : 0, state->status_rc, state->diff_rc,
                   state->staged_diff_rc);
  (void)sb_appendf(&raw, "status=%zu:%s;diff=%zu:%s;staged=%zu:%s;",
                   state->status_bytes, state->status_hash,
                   state->diff_bytes, state->diff_hash,
                   state->staged_diff_bytes, state->staged_diff_hash);
  hash_bytes_hex(raw.data ? raw.data : "", raw.len, state->state_hash,
                 sizeof(state->state_hash));
  free(raw.data);
}

static bool nytrix_git_state_capture(const char *root, nytrix_git_state_t *state) {
  if (!state) return false;
  memset(state, 0, sizeof(*state));
  bool have_path = path_join(state->nytrix_root, sizeof(state->nytrix_root),
                             root, "../nytrix") &&
                   exists_path(state->nytrix_root);
  state->exists = have_path;
  hash_bytes_hex("", 0, state->status_hash, sizeof(state->status_hash));
  hash_bytes_hex("", 0, state->diff_hash, sizeof(state->diff_hash));
  hash_bytes_hex("", 0, state->staged_diff_hash, sizeof(state->staged_diff_hash));
  if (!have_path) {
    nytrix_git_state_compute_state_hash(state);
    return false;
  }

  char *status_argv[] = {"git", "-C", state->nytrix_root, "status", "--short", NULL};
  proc_result_t status_pr = run_proc(status_argv, root, 30.0);
  state->status_rc = status_pr.rc;
  state->elapsed_ms += status_pr.elapsed_ms;
  state->status = trim_trailing_copy(status_pr.out ? status_pr.out : "");
  state->status_bytes = state->status ? strlen(state->status) : 0u;
  hash_bytes_hex(state->status ? state->status : "", state->status_bytes,
                 state->status_hash, sizeof(state->status_hash));
  if (status_pr.err && *status_pr.err) state->stderr_text = strdup(status_pr.err);
  proc_result_free(&status_pr);

  char *diff_argv[] = {"git", "-C", state->nytrix_root, "diff",
                       "--no-ext-diff", "--binary", NULL};
  proc_result_t diff_pr = run_proc(diff_argv, root, 30.0);
  state->diff_rc = diff_pr.rc;
  state->elapsed_ms += diff_pr.elapsed_ms;
  state->diff_bytes = diff_pr.out ? strlen(diff_pr.out) : 0u;
  hash_bytes_hex(diff_pr.out ? diff_pr.out : "", state->diff_bytes,
                 state->diff_hash, sizeof(state->diff_hash));
  if (!state->stderr_text && diff_pr.err && *diff_pr.err)
    state->stderr_text = strdup(diff_pr.err);
  proc_result_free(&diff_pr);

  char *staged_argv[] = {"git", "-C", state->nytrix_root, "diff",
                         "--cached", "--no-ext-diff", "--binary", NULL};
  proc_result_t staged_pr = run_proc(staged_argv, root, 30.0);
  state->staged_diff_rc = staged_pr.rc;
  state->elapsed_ms += staged_pr.elapsed_ms;
  state->staged_diff_bytes = staged_pr.out ? strlen(staged_pr.out) : 0u;
  hash_bytes_hex(staged_pr.out ? staged_pr.out : "", state->staged_diff_bytes,
                 state->staged_diff_hash, sizeof(state->staged_diff_hash));
  if (!state->stderr_text && staged_pr.err && *staged_pr.err)
    state->stderr_text = strdup(staged_pr.err);
  proc_result_free(&staged_pr);

  state->git_ok = state->status_rc == 0 && state->diff_rc == 0 &&
                  state->staged_diff_rc == 0;
  state->clean = state->git_ok && state->status && state->status[0] == '\0';
  nytrix_git_state_compute_state_hash(state);
  return state->exists && state->git_ok;
}

static bool nytrix_git_state_matches(const nytrix_git_state_t *a,
                                     const nytrix_git_state_t *b) {
  if (!a || !b) return false;
  return a->exists == b->exists &&
         a->git_ok == b->git_ok &&
         a->clean == b->clean &&
         a->status_rc == b->status_rc &&
         a->diff_rc == b->diff_rc &&
         a->staged_diff_rc == b->staged_diff_rc &&
         a->status_bytes == b->status_bytes &&
         a->diff_bytes == b->diff_bytes &&
         a->staged_diff_bytes == b->staged_diff_bytes &&
         strcmp(a->status_hash, b->status_hash) == 0 &&
         strcmp(a->diff_hash, b->diff_hash) == 0 &&
         strcmp(a->staged_diff_hash, b->staged_diff_hash) == 0 &&
         strcmp(a->state_hash, b->state_hash) == 0;
}

static bool nytrix_git_state_status_matches(const nytrix_git_state_t *a,
                                            const nytrix_git_state_t *b) {
  if (!a || !b) return false;
  return a->exists == b->exists &&
         a->git_ok == b->git_ok &&
         a->clean == b->clean &&
         a->status_rc == b->status_rc &&
         a->staged_diff_rc == b->staged_diff_rc &&
         a->status_bytes == b->status_bytes &&
         a->staged_diff_bytes == b->staged_diff_bytes &&
         strcmp(a->status_hash, b->status_hash) == 0 &&
         strcmp(a->staged_diff_hash, b->staged_diff_hash) == 0;
}

static bool fuzz_all_preflight_add_nytrix_status(const char *root, string_list_t *rows,
                                                 string_list_t *failures,
                                                 const char *name, bool required_clean,
                                                 bool allow_dirty_baseline,
                                                 const nytrix_git_state_t *baseline,
                                                 nytrix_git_state_t *captured) {
  nytrix_git_state_t state;
  (void)nytrix_git_state_capture(root, &state);
  bool baseline_match = baseline ? nytrix_git_state_matches(baseline, &state) : false;
  bool baseline_status_match =
      baseline ? nytrix_git_state_status_matches(baseline, &state) : false;
  bool dirty_baseline_ok = allow_dirty_baseline &&
                           (!baseline || baseline_status_match) &&
                           state.exists && state.git_ok;
  bool ok = state.exists && state.git_ok &&
            (state.clean || !required_clean || dirty_baseline_ok);
  if (!ok && required_clean) {
    const char *reason = "nytrix checkout is dirty";
    if (!state.exists) reason = "sibling nytrix checkout missing";
    else if (!state.git_ok) reason = "nytrix status check failed";
    else if (baseline && allow_dirty_baseline) reason = "nytrix checkout changed during preflight";
    (void)string_list_push_take(failures,
                                make_fuzz_failure(root, name ? name : "nytrix-status",
                                                  reason,
                                                  state.exists ? state.nytrix_root : ""));
  }
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, name ? name : "nytrix_status");
  (void)sb_append(&row, ",\"kind\":\"fuzz-all-preflight\",\"phase\":\"guard\"");
  (void)sb_appendf(&row,
                   ",\"ok\":%s,\"required_clean\":%s,\"exists\":%s,"
                   "\"clean\":%s,\"dirty_baseline_allowed\":%s,"
                   "\"baseline_match\":%s,\"baseline_status_match\":%s,"
                   "\"rc\":%d,\"diff_rc\":%d,"
                   "\"staged_diff_rc\":%d,\"elapsed_ms\":%.2f,"
                   "\"status_bytes\":%zu,\"diff_bytes\":%zu,"
                   "\"staged_diff_bytes\":%zu,\"status_hash\":",
                   ok ? "true" : "false", required_clean ? "true" : "false",
                   state.exists ? "true" : "false",
                   state.clean ? "true" : "false",
                   allow_dirty_baseline ? "true" : "false",
                   baseline_match ? "true" : "false",
                   baseline_status_match ? "true" : "false",
                   state.exists ? state.status_rc : 1, state.diff_rc,
                   state.staged_diff_rc, state.elapsed_ms,
                   state.status_bytes, state.diff_bytes,
                   state.staged_diff_bytes);
  (void)sb_append_json_str(&row, state.status_hash);
  (void)sb_append(&row, ",\"diff_hash\":");
  (void)sb_append_json_str(&row, state.diff_hash);
  (void)sb_append(&row, ",\"staged_diff_hash\":");
  (void)sb_append_json_str(&row, state.staged_diff_hash);
  (void)sb_append(&row, ",\"state_hash\":");
  (void)sb_append_json_str(&row, state.state_hash);
  (void)sb_append(&row, ",\"nytrix_root\":");
  append_rel_json_str(&row, root, state.exists ? state.nytrix_root : "");
  if (state.status && *state.status) {
    (void)sb_append(&row, ",\"status_tail\":");
    append_tail_json_str(&row, state.status, 1200);
  }
  if (state.stderr_text && *state.stderr_text) {
    (void)sb_append(&row, ",\"stderr_tail\":");
    append_tail_json_str(&row, state.stderr_text, 1200);
  }
  (void)sb_append(&row, ",\"engine\":\"nytrix_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  if (captured) {
    *captured = state;
  } else {
    nytrix_git_state_free(&state);
  }
  return ok;
}

static void fuzz_all_preflight_add_artifact_row(const char *root, string_list_t *rows,
                                                string_list_t *failures,
                                                const char *run_report,
                                                const char *audit_report,
                                                const char *findings_report,
                                                const char *findings_markdown) {
  file_buf_t report = {0};
  bool have_run = run_report && read_file(run_report, &report) && report.data;
  char *manifest = have_run ? json_string_or_empty(report.data, "manifest") : strdup("");
  char *command_file = have_run ? json_string_or_empty(report.data, "command_file") : strdup("");
  char *forever_script = have_run ? json_string_or_empty(report.data, "forever_script") : strdup("");
  bool manifest_ok = manifest && *manifest && path_exists_maybe_root(root, manifest);
  bool command_ok = command_file && *command_file && path_exists_maybe_root(root, command_file);
  bool forever_ok = forever_script && *forever_script && path_exists_maybe_root(root, forever_script);
  file_buf_t forever_file = {0};
  bool forever_readable = forever_ok &&
                          read_file_maybe_root(root, forever_script, &forever_file) &&
                          forever_file.data;
  bool forever_script_cache_env_ok =
    forever_readable &&
    strstr(forever_file.data, "set -euo pipefail") &&
    strstr(forever_file.data, "cd ") &&
    shell_text_has_repo_cache_env(forever_file.data);
  bool forever_script_strict_status_ok =
    forever_readable &&
    strstr(forever_file.data, "fuzz all status --strict") &&
    strstr(forever_file.data, "--allow-full-pressure-remediation");
  bool forever_script_history_coverage_ok =
    forever_readable &&
    strstr(forever_file.data, "fuzz all coverage --strict --history") &&
    strstr(forever_file.data, "$coverage_latest") &&
    strstr(forever_file.data, "--target-thread-years") &&
    strstr(forever_file.data, "--hours") &&
    strstr(forever_file.data, "--threads") &&
    strstr(forever_file.data, "--profile");
  bool forever_script_old_paths_ok =
    forever_readable &&
    strstr(forever_file.data, "fuzz all old-paths --dry-run") &&
    strstr(forever_file.data, "--nytrix-root ../nytrix") &&
    strstr(forever_file.data, "--archive-dir build/cache/old-nytrix") &&
    strstr(forever_file.data, "$old_paths") &&
    strstr(forever_file.data, "$old_paths_md");
  bool forever_script_progress_ok =
    forever_readable &&
    strstr(forever_file.data, "fuzz all progress --status \"$status\"") &&
    strstr(forever_file.data, "$progress") &&
    strstr(forever_file.data, "$progress_md");
  bool forever_script_completion_guard_ok =
    forever_readable &&
    strstr(forever_file.data, "\"campaign_complete\":true") &&
    strstr(forever_file.data, "--no-script") &&
    strstr(forever_file.data, "--allow-full-pressure-remediation") &&
    strstr(forever_file.data, "campaign target complete");
  bool forever_script_refresh_low_priority_ok =
    forever_readable &&
    strstr(forever_file.data, "nytrix_low_priority ./build/nytrix fuzz all old-paths") &&
    strstr(forever_file.data, "nytrix_low_priority ./build/nytrix fuzz all status --refresh") &&
    strstr(forever_file.data, "nytrix_low_priority ./build/nytrix fuzz all progress --status") &&
    strstr(forever_file.data, "nytrix_low_priority ./build/nytrix fuzz all audit") &&
    strstr(forever_file.data, "nytrix_low_priority ./build/nytrix fuzz all findings") &&
    strstr(forever_file.data, "nytrix_low_priority ./build/nytrix fuzz all coverage --strict --history") &&
    strstr(forever_file.data, "nytrix_low_priority ./build/nytrix fuzz all history") &&
    strstr(forever_file.data, "nytrix_low_priority ./build/nytrix fuzz all worklist") &&
    strstr(forever_file.data, "nytrix_low_priority ./build/nytrix fuzz all plan") &&
    strstr(forever_file.data, "nytrix_low_priority ./build/nytrix fuzz all status --strict");
  bool run_ok = run_report && *run_report && path_exists_file(run_report);
  bool audit_ok = audit_report && *audit_report && path_exists_file(audit_report);
  bool findings_ok = findings_report && *findings_report && path_exists_file(findings_report);
  bool findings_md_ok = findings_markdown && *findings_markdown &&
                        path_exists_file(findings_markdown);
  bool ok = run_ok && audit_ok && findings_ok && findings_md_ok &&
            manifest_ok && command_ok && forever_ok &&
            forever_script_cache_env_ok &&
            forever_script_strict_status_ok &&
            forever_script_history_coverage_ok &&
            forever_script_old_paths_ok &&
            forever_script_progress_ok &&
            forever_script_completion_guard_ok &&
            forever_script_refresh_low_priority_ok;
  if (!ok) {
    (void)string_list_push_take(failures,
                                make_fuzz_failure(root, "preflight_artifacts",
                                                  "preflight did not preserve required artifacts",
                                                  run_report ? run_report : ""));
  }
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":\"preflight_artifacts\","
                  "\"kind\":\"fuzz-all-preflight\",\"phase\":\"artifact\",");
  (void)sb_appendf(&row,
                   "\"ok\":%s,\"run_report_exists\":%s,"
                   "\"audit_report_exists\":%s,\"findings_report_exists\":%s,"
                   "\"findings_markdown_exists\":%s,"
                   "\"manifest_exists\":%s,\"command_file_exists\":%s,"
                   "\"forever_script_exists\":%s,"
                   "\"forever_script_cache_env_ok\":%s,"
                   "\"forever_script_strict_status_ok\":%s,"
                   "\"forever_script_history_coverage_ok\":%s,"
                   "\"forever_script_old_paths_ok\":%s,"
                   "\"forever_script_progress_ok\":%s,"
                   "\"forever_script_completion_guard_ok\":%s,"
                   "\"forever_script_refresh_low_priority_ok\":%s",
                   ok ? "true" : "false", run_ok ? "true" : "false",
                   audit_ok ? "true" : "false", findings_ok ? "true" : "false",
                   findings_md_ok ? "true" : "false",
                   manifest_ok ? "true" : "false", command_ok ? "true" : "false",
                   forever_ok ? "true" : "false",
                   forever_script_cache_env_ok ? "true" : "false",
                   forever_script_strict_status_ok ? "true" : "false",
                   forever_script_history_coverage_ok ? "true" : "false",
                   forever_script_old_paths_ok ? "true" : "false",
                   forever_script_progress_ok ? "true" : "false",
                   forever_script_completion_guard_ok ? "true" : "false",
                   forever_script_refresh_low_priority_ok ? "true" : "false");
  (void)sb_append(&row, ",\"run_report\":");
  append_rel_json_str(&row, root, run_report ? run_report : "");
  (void)sb_append(&row, ",\"audit_report\":");
  append_rel_json_str(&row, root, audit_report ? audit_report : "");
  (void)sb_append(&row, ",\"findings_report\":");
  append_rel_json_str(&row, root, findings_report ? findings_report : "");
  (void)sb_append(&row, ",\"findings_markdown\":");
  append_rel_json_str(&row, root, findings_markdown ? findings_markdown : "");
  (void)sb_append(&row, ",\"manifest\":");
  append_rel_json_str(&row, root, manifest ? manifest : "");
  (void)sb_append(&row, ",\"command_file\":");
  append_rel_json_str(&row, root, command_file ? command_file : "");
  (void)sb_append(&row, ",\"forever_script\":");
  append_rel_json_str(&row, root, forever_script ? forever_script : "");
  (void)sb_append(&row, ",\"engine\":\"nytrix_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  free(report.data);
  free(forever_file.data);
  free(manifest);
  free(command_file);
  free(forever_script);
}

static int cmd_public_fuzz_all_preflight(int argc, char **argv) {
  char root[4096];
  if (!find_nytrix_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nytrix-root-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *work_arg = value_after_equals(argc, argv, 4, "--work-dir", "");
  const char *dir_arg = value_after_equals(argc, argv, 4, "--dir", "");
  if (!dir_arg || !*dir_arg)
    dir_arg = value_after_equals(argc, argv, 4, "--history-dir", "build/fuzz/all");
  const char *target_arg = value_after_equals(argc, argv, 4, "--target-thread-years", "");
  if (!target_arg || !*target_arg)
    target_arg = value_after_equals(argc, argv, 4, "--target-years", "");
  if (!target_arg || !*target_arg)
    target_arg = value_after_equals(argc, argv, 4, "--target", "10");
  const char *hours_arg = value_after_equals(argc, argv, 4, "--hours-per-run", "");
  if (!hours_arg || !*hours_arg)
    hours_arg = value_after_equals(argc, argv, 4, "--run-hours", "");
  if (!hours_arg || !*hours_arg)
    hours_arg = value_after_equals(argc, argv, 4, "--hours", "8");
  const char *threads_arg = value_after_equals(argc, argv, 4, "--threads",
                                               NYTRIX_DEFAULT_FUZZ_THREADS);
  const char *profile_arg = value_after_equals(argc, argv, 4, "--profile", "insane");
  bool include_afl = !cli_has_named_arg(argc, argv, 4, "--no-afl") &&
                     !cli_has_named_arg(argc, argv, 4, "--skip-afl");
  bool allow_nytrix = has_flag_after(argc, argv, 4, "--allow-nytrix");
  bool has_nytrix_policy = cli_has_named_arg(argc, argv, 4, "--no-nytrix") ||
                           cli_has_named_arg(argc, argv, 4, "--skip-nytrix");
  bool guard_nytrix = !has_flag_after(argc, argv, 4, "--no-nytrix-guard");
  bool allow_dirty_nytrix_baseline =
    has_flag_after(argc, argv, 4, "--allow-dirty-nytrix-baseline") ||
    has_flag_after(argc, argv, 4, "--dirty-nytrix-baseline");
  bool has_profile = cli_has_named_arg(argc, argv, 4, "--profile");
  bool has_threads = cli_has_named_arg(argc, argv, 4, "--threads");
  bool keep_going = has_flag_after(argc, argv, 4, "--keep-going");
  const char *low_priority_env = getenv("NYTRIX_LOW_PRIORITY");
  bool low_priority_enabled = !(low_priority_env && strcmp(low_priority_env, "0") == 0);
  int preflight_nice_target = 10;
  int preflight_nice = 0;
  bool preflight_low_priority_applied =
    nytrix_apply_process_low_priority(&preflight_nice_target,
                                     &preflight_nice);

  char *campaign_dir = dir_arg && *dir_arg ? strdup(dir_arg) : strdup("build/fuzz/all");
  nytrix_redirect_nytrix_output_dir(&campaign_dir, root, "fuzz-all-preflight");
  char *work_dir = NULL;
  if (work_arg && *work_arg) {
    (void)asprintf(&work_dir, "%s", work_arg);
  } else {
    (void)asprintf(&work_dir, "%s/build/cache/scratch/fuzz_all_preflight/preflight_%ld_%d",
                   root, (long)time(NULL), (int)getpid());
  }
  if (!work_dir || !mkdir_p(work_dir)) {
    printf("{\"ok\":false,\"error\":\"preflight-workdir-failed\"}\n");
    free(work_dir);
    free(campaign_dir);
    return 2;
  }
  if (!campaign_dir || !mkdir_p(campaign_dir)) {
    printf("{\"ok\":false,\"error\":\"preflight-campaign-dir-failed\"}\n");
    free(work_dir);
    free(campaign_dir);
    return 2;
  }
  char *run_report = NULL, *audit_report = NULL, *findings_report = NULL;
  char *findings_markdown = NULL;
  char *history_report = NULL, *worklist_report = NULL, *coverage_report = NULL;
  char *plan_report = NULL, *status_report = NULL, *status_markdown = NULL;
  (void)asprintf(&run_report, "%s/all-smoke.json", work_dir);
  (void)asprintf(&audit_report, "%s/all-smoke-audit.json", work_dir);
  (void)asprintf(&findings_report, "%s/all-smoke-findings.json", work_dir);
  (void)asprintf(&findings_markdown, "%s/all-smoke-findings.md", work_dir);
  if (campaign_dir && *campaign_dir) {
    (void)asprintf(&history_report, "%s/history.json", campaign_dir);
    (void)asprintf(&worklist_report, "%s/worklist.json", campaign_dir);
    (void)asprintf(&coverage_report, "%s/coverage.json", campaign_dir);
    (void)asprintf(&plan_report, "%s/plan.json", campaign_dir);
    (void)asprintf(&status_report, "%s/status.json", campaign_dir);
    (void)asprintf(&status_markdown, "%s/status.md", campaign_dir);
  }
  if (!run_report || !audit_report || !findings_report || !findings_markdown ||
      !campaign_dir || !history_report || !worklist_report || !coverage_report ||
      !plan_report || !status_report || !status_markdown) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(work_dir);
    free(campaign_dir);
    free(run_report);
    free(audit_report);
    free(findings_report);
    free(findings_markdown);
    free(history_report);
    free(worklist_report);
    free(coverage_report);
    free(plan_report);
    free(status_report);
    free(status_markdown);
    return 2;
  }

  string_list_t rows = {0}, failures = {0};
  nytrix_git_state_t nytrix_before = {0};
  bool nytrix_clean_before = true;
  if (guard_nytrix)
    nytrix_clean_before =
      fuzz_all_preflight_add_nytrix_status(root, &rows, &failures,
                                           "nytrix_status_before", !allow_nytrix,
                                           allow_dirty_nytrix_baseline, NULL,
                                           &nytrix_before);
  bool aborted_before_run = guard_nytrix && !allow_nytrix && !nytrix_clean_before;
  const char *abort_reason = aborted_before_run ? "nytrix checkout dirty before run" : "";

  if (!aborted_before_run) {
    char *child_argv[192];
    int ca = 0;
    child_argv[ca++] = g_self_path;
    child_argv[ca++] = "fuzz";
    child_argv[ca++] = "all";
    child_argv[ca++] = "run";
    child_argv[ca++] = "--smoke";
    if (!has_profile) {
      child_argv[ca++] = "--profile";
      child_argv[ca++] = "insane";
    }
    if (!has_threads) {
      child_argv[ca++] = "--threads";
      child_argv[ca++] = (char *)NYTRIX_DEFAULT_FUZZ_THREADS;
    }
    if (!keep_going) child_argv[ca++] = "--fail-fast";
    if (allow_nytrix && !has_nytrix_policy)
      child_argv[ca++] = "--allow-nytrix";
    else if (!allow_nytrix && !has_nytrix_policy)
      child_argv[ca++] = "--no-nytrix";
    child_argv[ca++] = "--dir";
    child_argv[ca++] = work_dir;
    child_argv[ca++] = "--target-thread-years";
    child_argv[ca++] = (char *)target_arg;
    bool arg_overflow = false;
    for (int i = 4; i < argc; ++i) {
      if (fuzz_all_preflight_skip_copy_arg(argc, argv, &i)) continue;
      if (ca >= (int)(sizeof(child_argv) / sizeof(child_argv[0])) - 3) {
        arg_overflow = true;
        break;
      }
      child_argv[ca++] = argv[i];
    }
    child_argv[ca++] = "--json";
    child_argv[ca++] = run_report;
    child_argv[ca] = NULL;
    if (arg_overflow) {
      (void)string_list_push_take(&failures,
                                  make_fuzz_failure(root, "preflight_args",
                                                    "too many forwarded all-run arguments", ""));
      (void)string_list_push_take(&rows,
                                  make_fuzz_all_preflight_row(root, "preflight_args",
                                                              "setup", false, 1, "",
                                                              0.0, 1.0, NULL, NULL,
                                                              NULL));
    }

    int rc = arg_overflow ? 1 : cmd_public_fuzz_all_run(ca, child_argv);
    fuzz_all_preflight_add_report_step(root, &rows, &failures, "preflight_all_run",
                                       "run", rc, run_report, child_argv);

    char *audit_argv[] = {
      g_self_path, "fuzz", "all", "audit", "--report", run_report,
      "--strict", "--json", audit_report, NULL
    };
    int audit_rc = cmd_public_fuzz_all_audit(9, audit_argv);
    fuzz_all_preflight_add_report_step(root, &rows, &failures, "preflight_strict_audit",
                                       "audit", audit_rc, audit_report, audit_argv);

    char *findings_argv[] = {
      g_self_path, "fuzz", "all", "findings", "--report", run_report,
      "--json", findings_report, "--markdown", findings_markdown, NULL
    };
    int findings_rc = cmd_public_fuzz_all_findings(10, findings_argv);
    fuzz_all_preflight_add_report_step(root, &rows, &failures, "preflight_findings",
                                       "findings", findings_rc, findings_report,
                                       findings_argv);

    fuzz_all_preflight_add_artifact_row(root, &rows, &failures, run_report,
                                        audit_report, findings_report,
                                        findings_markdown);

    char *status_argv[] = {
      g_self_path, "fuzz", "all", "status", "--refresh", "--strict",
      "--allow-incomplete-coverage",
      "--allow-empty-preflight-history",
      "--allow-full-pressure-remediation",
      "--dir", campaign_dir,
      "--history", history_report,
      "--worklist", worklist_report,
      "--coverage", coverage_report,
      "--plan", plan_report,
      "--target-thread-years", (char *)target_arg,
      "--hours", (char *)hours_arg,
      "--threads", (char *)threads_arg,
      "--profile", (char *)profile_arg,
      "--json", status_report,
      "--markdown", status_markdown,
      NULL
    };
    int status_rc = cmd_public_fuzz_all_status(31, status_argv);
    fuzz_all_preflight_add_report_step(root, &rows, &failures,
                                       "preflight_status", "status",
                                       status_rc, status_report, status_argv);

    if (guard_nytrix)
      (void)fuzz_all_preflight_add_nytrix_status(root, &rows, &failures,
                                                 "nytrix_status_after",
                                                 !allow_nytrix,
                                                 allow_dirty_nytrix_baseline,
                                                 allow_dirty_nytrix_baseline
                                                   ? &nytrix_before
                                                   : NULL,
                                                 NULL);
  }

  double finding_count = 0.0, finding_live = 0.0, finding_cleared = 0.0;
  double finding_missing = 0.0;
  double known_count = 0.0, known_reproduced = 0.0, known_fixed = 0.0;
  double known_lost = 0.0, known_baseline = 0.0;
  double quarantined = 0.0, quarantine_lanes = 0.0;
  double perf_hotspots = 0.0, perf_max_ratio = 0.0;
  double status_blockers = -1.0, runs_needed = -1.0;
  double wall_days_needed = -1.0, wall_hours_needed = -1.0;
  double thread_hours_needed = -1.0;
  double runs_per_day = -1.0, thread_years_per_day = -1.0;
  double status_active_perf_hotspots = -1.0, status_active_perf_max_ratio = -1.0;
  double status_historical_perf_hotspots = -1.0;
  double status_historical_perf_max_ratio = -1.0;
  double status_active_finding_live = -1.0, status_active_finding_missing = -1.0;
  double status_historical_finding_live = -1.0, status_historical_finding_missing = -1.0;
  double status_active_known_reproduced = -1.0, status_active_known_lost = -1.0;
  double status_active_known_baseline = -1.0;
  double status_historical_known_reproduced = -1.0, status_historical_known_lost = -1.0;
  double status_historical_known_baseline = -1.0;
  double status_coverage_advisory_gaps = -1.0;
  double status_coverage_disabled_lanes = -1.0;
  double status_coverage_budget_short_lanes = -1.0;
  double status_coverage_missing_tool_lanes = -1.0;
  double status_full_pressure_reports = -1.0;
  double status_full_pressure_thread_hours = -1.0;
  double status_active_items = -1.0;
  bool status_ready = false, long_run_ready = false;
  bool status_allow_incomplete_coverage = false;
  bool status_target_reached = false, status_campaign_complete = false;
  bool status_latest_full_pressure_clean = false;
  bool status_cache_policy_ok = false, status_old_scratch_absent = false;
  bool status_old_fuzz_absent = false;
  bool status_old_build_cache_absent = false;
  bool status_active_old_writer_present = false;
  bool status_active_old_cache_writer_present = false;
  bool status_ny_bin_exists = false;
  char *perf_case = strdup("");
  char *forever_script = strdup("");
  char *status_next_script = strdup("");
  char *status_latest_full_pressure_report = strdup("");
  char *status_run_command = strdup("");
  char *status_recommended_action = strdup("");
  char *status_recommended_reason = strdup("");
  char *status_recommended_command = strdup("");
  char *status_recommended_low_cpu_command = strdup("");
  char *status_recommended_preview_command = strdup("");
  char *status_next_command = strdup("");
  char *status_preview_command = strdup("");
  char *status_coverage_next_command = strdup("");
  char *status_coverage_next_guarded_command = strdup("");
  char *status_coverage_next_low_cpu_command = strdup("");
  char *status_coverage_next_preview_command = strdup("");
  char *status_ny_bin = strdup("");
  char *status_ny_bin_hash = strdup("");
  char *status_nytrix_git_head = strdup("");
  char *completion_eta_local = strdup("");
  file_buf_t run_summary = {0};
  if (read_file(run_report, &run_summary) && run_summary.data) {
    (void)summary_number_from_report(run_summary.data, "finding_count", &finding_count);
    (void)summary_number_from_report(run_summary.data, "finding_live", &finding_live);
    (void)summary_number_from_report(run_summary.data, "finding_cleared", &finding_cleared);
    (void)summary_number_from_report(run_summary.data, "finding_missing", &finding_missing);
    (void)summary_number_from_report(run_summary.data, "known_bug_count", &known_count);
    (void)summary_number_from_report(run_summary.data, "known_bug_reproduced", &known_reproduced);
    (void)summary_number_from_report(run_summary.data, "known_bug_fixed_candidates", &known_fixed);
    (void)summary_number_from_report(run_summary.data, "known_bug_lost_signal", &known_lost);
    (void)summary_number_from_report(run_summary.data, "known_bug_baseline_failures", &known_baseline);
    (void)summary_number_from_report(run_summary.data, "quarantined_known_bugs", &quarantined);
    (void)summary_number_from_report(run_summary.data, "lanes_with_quarantine", &quarantine_lanes);
    (void)summary_number_from_report(run_summary.data, "perf_hotspots", &perf_hotspots);
    (void)summary_number_from_report(run_summary.data, "perf_max_ratio", &perf_max_ratio);
    free(perf_case);
    free(forever_script);
    perf_case = json_string_or_empty(run_summary.data, "perf_max_case");
    forever_script = json_string_or_empty(run_summary.data, "forever_script");
  }
  file_buf_t status_summary = {0};
  if (read_file(status_report, &status_summary) && status_summary.data) {
    (void)summary_number_from_report(status_summary.data, "blocker_count",
                                     &status_blockers);
    (void)summary_number_from_report(status_summary.data, "runs_needed",
                                     &runs_needed);
    (void)summary_number_from_report(status_summary.data, "wall_days_needed",
                                     &wall_days_needed);
    (void)summary_number_from_report(status_summary.data, "wall_hours_needed",
                                     &wall_hours_needed);
    (void)summary_number_from_report(status_summary.data, "thread_hours_needed",
                                     &thread_hours_needed);
    (void)summary_number_from_report(status_summary.data, "runs_per_day",
                                     &runs_per_day);
    (void)summary_number_from_report(status_summary.data, "thread_years_per_day",
                                     &thread_years_per_day);
    (void)summary_number_from_report(status_summary.data, "active_perf_hotspots",
                                     &status_active_perf_hotspots);
    (void)summary_number_from_report(status_summary.data, "active_perf_max_ratio",
                                     &status_active_perf_max_ratio);
    (void)summary_number_from_report(status_summary.data,
                                     "historical_perf_hotspots",
                                     &status_historical_perf_hotspots);
    (void)summary_number_from_report(status_summary.data,
                                     "historical_perf_max_ratio",
                                     &status_historical_perf_max_ratio);
    (void)summary_number_from_report(status_summary.data,
                                     "active_compiler_finding_live",
                                     &status_active_finding_live);
    (void)summary_number_from_report(status_summary.data,
                                     "active_compiler_finding_missing",
                                     &status_active_finding_missing);
    (void)summary_number_from_report(status_summary.data,
                                     "historical_compiler_finding_live",
                                     &status_historical_finding_live);
    (void)summary_number_from_report(status_summary.data,
                                     "historical_compiler_finding_missing",
                                     &status_historical_finding_missing);
    (void)summary_number_from_report(status_summary.data,
                                     "active_known_bug_reproduced",
                                     &status_active_known_reproduced);
    (void)summary_number_from_report(status_summary.data,
                                     "active_known_bug_lost_signal",
                                     &status_active_known_lost);
    (void)summary_number_from_report(status_summary.data,
                                     "active_known_bug_baseline_failures",
                                     &status_active_known_baseline);
    (void)summary_number_from_report(status_summary.data,
                                     "historical_known_bug_reproduced",
                                     &status_historical_known_reproduced);
    (void)summary_number_from_report(status_summary.data,
                                     "historical_known_bug_lost_signal",
                                     &status_historical_known_lost);
    (void)summary_number_from_report(status_summary.data,
                                     "historical_known_bug_baseline_failures",
                                     &status_historical_known_baseline);
    (void)summary_number_from_report(status_summary.data,
                                     "coverage_advisory_gaps",
                                     &status_coverage_advisory_gaps);
    (void)summary_number_from_report(status_summary.data,
                                     "coverage_disabled_lanes",
                                     &status_coverage_disabled_lanes);
    (void)summary_number_from_report(status_summary.data,
                                     "coverage_budget_short_lanes",
                                     &status_coverage_budget_short_lanes);
    (void)summary_number_from_report(status_summary.data,
                                     "coverage_missing_tool_lanes",
                                     &status_coverage_missing_tool_lanes);
    (void)summary_number_from_report(status_summary.data,
                                     "full_pressure_reports",
                                     &status_full_pressure_reports);
      (void)summary_number_from_report(status_summary.data,
                                       "full_pressure_thread_hours",
                                       &status_full_pressure_thread_hours);
    (void)summary_number_from_report(status_summary.data, "active_items",
                                     &status_active_items);
      (void)summary_bool_from_report(status_summary.data, "ready", &status_ready);
    (void)summary_bool_from_report(status_summary.data,
                                   "allow_incomplete_coverage",
                                   &status_allow_incomplete_coverage);
    (void)summary_bool_from_report(status_summary.data, "long_run_ready",
                                   &long_run_ready);
    (void)summary_bool_from_report(status_summary.data, "target_reached",
                                   &status_target_reached);
    (void)summary_bool_from_report(status_summary.data, "campaign_complete",
                                   &status_campaign_complete);
    (void)summary_bool_from_report(status_summary.data,
                                   "latest_full_pressure_clean",
                                   &status_latest_full_pressure_clean);
    (void)summary_bool_from_report(status_summary.data, "cache_policy_ok",
                                   &status_cache_policy_ok);
    (void)summary_bool_from_report(status_summary.data,
                                   "old_nytrix_test_scratch_absent",
                                   &status_old_scratch_absent);
    (void)summary_bool_from_report(status_summary.data,
                                   "old_nytrix_fuzz_absent",
                                   &status_old_fuzz_absent);
    (void)summary_bool_from_report(status_summary.data,
                                   "old_nytrix_build_cache_absent",
                                   &status_old_build_cache_absent);
      (void)summary_bool_from_report(status_summary.data,
                                     "active_old_nytrix_output_writer_present",
                                     &status_active_old_writer_present);
    (void)summary_bool_from_report(status_summary.data,
                                   "active_old_nytrix_cache_writer_present",
                                   &status_active_old_cache_writer_present);
      (void)summary_bool_from_report(status_summary.data, "ny_bin_exists",
                                     &status_ny_bin_exists);
      free(status_next_script);
      free(status_latest_full_pressure_report);
      free(status_run_command);
    free(status_recommended_action);
    free(status_recommended_reason);
    free(status_recommended_command);
    free(status_recommended_low_cpu_command);
    free(status_recommended_preview_command);
    free(status_next_command);
    free(status_preview_command);
    free(status_coverage_next_command);
    free(status_coverage_next_guarded_command);
    free(status_coverage_next_low_cpu_command);
    free(status_coverage_next_preview_command);
      free(status_ny_bin);
      free(status_ny_bin_hash);
    free(status_nytrix_git_head);
    free(status_nytrix_git_head);
    status_next_script = summary_string_from_report(status_summary.data,
                                                    "next_script");
    status_latest_full_pressure_report =
        summary_string_from_report(status_summary.data,
                                   "latest_full_pressure_report");
      status_run_command = summary_string_from_report(status_summary.data,
                                                      "run_command");
    status_recommended_action =
        summary_string_from_report(status_summary.data, "recommended_action");
    status_recommended_reason =
        summary_string_from_report(status_summary.data, "recommended_reason");
    status_recommended_command =
        summary_string_from_report(status_summary.data, "recommended_command");
    status_recommended_low_cpu_command =
        summary_string_from_report(status_summary.data,
                                   "recommended_low_cpu_command");
    status_recommended_preview_command =
        summary_string_from_report(status_summary.data,
                                   "recommended_preview_command");
    status_next_command =
        summary_string_from_report(status_summary.data, "next_command");
    status_preview_command =
        summary_string_from_report(status_summary.data, "preview_command");
    status_coverage_next_command =
        summary_string_from_report(status_summary.data, "coverage_next_command");
    status_coverage_next_guarded_command =
        summary_string_from_report(status_summary.data,
                                   "coverage_next_guarded_command");
    status_coverage_next_low_cpu_command =
        summary_string_from_report(status_summary.data,
                                   "coverage_next_low_cpu_command");
    status_coverage_next_preview_command =
        summary_string_from_report(status_summary.data,
                                   "coverage_next_preview_command");
    if (!status_recommended_action) {
      status_recommended_action = strdup("");
    }
    if (!status_recommended_reason) {
      status_recommended_reason = strdup("");
    }
    if (!status_recommended_command) {
      status_recommended_command = strdup("");
    }
    if (!status_recommended_low_cpu_command) {
      status_recommended_low_cpu_command = strdup("");
    }
    if (!status_recommended_preview_command) {
      status_recommended_preview_command = strdup("");
    }
    if (!status_next_command) {
      status_next_command = strdup("");
    }
    if (!status_preview_command) {
      status_preview_command = strdup("");
    }
    if (!status_coverage_next_command) {
      status_coverage_next_command = strdup("");
    }
    if (!status_coverage_next_guarded_command) {
      status_coverage_next_guarded_command = strdup("");
    }
    if (!status_coverage_next_low_cpu_command) {
      status_coverage_next_low_cpu_command = strdup("");
    }
    if (!status_coverage_next_preview_command) {
      status_coverage_next_preview_command = strdup("");
    }
    status_ny_bin = summary_string_from_report(status_summary.data, "ny_bin");
    status_ny_bin_hash = summary_string_from_report(status_summary.data,
                                                    "ny_bin_hash");
    status_nytrix_git_head = summary_string_from_report(status_summary.data,
                                                       "nytrix_git_head");
    status_nytrix_git_head = summary_string_from_report(status_summary.data,
                                                        "nytrix_git_head");
    free(completion_eta_local);
    completion_eta_local = summary_string_from_report(status_summary.data,
                                                      "completion_eta_local");
  }

  bool preflight_ok = failures.count == 0;
  bool handoff_ready = preflight_ok && status_ready;
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"ready\":");
  (void)sb_append(&extra, handoff_ready ? "true" : "false");
  (void)sb_append(&extra, ",\"preflight_ok\":");
  (void)sb_append(&extra, preflight_ok ? "true" : "false");
  (void)sb_append(&extra, ",\"handoff_ready\":");
  (void)sb_append(&extra, handoff_ready ? "true" : "false");
  (void)sb_append(&extra, ",\"cache_policy_ok\":");
  (void)sb_append(&extra, status_cache_policy_ok ? "true" : "false");
  (void)sb_append(&extra, ",\"old_nytrix_test_scratch_absent\":");
  (void)sb_append(&extra, status_old_scratch_absent ? "true" : "false");
  (void)sb_append(&extra, ",\"old_nytrix_fuzz_absent\":");
  (void)sb_append(&extra, status_old_fuzz_absent ? "true" : "false");
  (void)sb_append(&extra, ",\"old_nytrix_build_cache_absent\":");
  (void)sb_append(&extra, status_old_build_cache_absent ? "true" : "false");
  (void)sb_append(&extra, ",\"active_old_nytrix_output_writer_present\":");
  (void)sb_append(&extra, status_active_old_writer_present ? "true" : "false");
  (void)sb_append(&extra, ",\"active_old_nytrix_cache_writer_present\":");
  (void)sb_append(&extra,
                  status_active_old_cache_writer_present ? "true" : "false");
  (void)sb_appendf(&extra,
                   ",\"blocker_count\":%.0f,\"blockers\":%.0f,"
                   "\"active_items\":%.0f,\"active_runs\":%.0f",
                   status_blockers, status_blockers,
                   status_active_items, status_active_items);
  (void)sb_append(&extra, ",\"status_old_nytrix_build_cache_absent\":");
  (void)sb_append(&extra, status_old_build_cache_absent ? "true" : "false");
  (void)sb_append(&extra, ",\"status_active_old_nytrix_output_writer_present\":");
  (void)sb_append(&extra, status_active_old_writer_present ? "true" : "false");
  (void)sb_append(&extra, ",\"status_active_old_nytrix_cache_writer_present\":");
  (void)sb_append(&extra,
                  status_active_old_cache_writer_present ? "true" : "false");
  (void)sb_appendf(&extra,
                   ",\"aborted\":%s,\"read_only_nytrix\":%s,\"nytrix_guard\":%s,"
                   "\"include_afl\":%s,\"allow_nytrix\":%s,"
                   "\"allow_dirty_nytrix_baseline\":%s,"
                   "\"low_priority_enabled\":%s,"
                   "\"low_priority_applied\":%s,"
                   "\"nice_target\":%d,"
                   "\"nice_value\":%d,"
                   "\"nytrix_baseline_captured\":%s,"
                   "\"nytrix_baseline_clean\":%s,"
                   "\"finding_count\":%.0f,"
                   "\"finding_live\":%.0f,"
                   "\"finding_cleared\":%.0f,"
                   "\"finding_missing\":%.0f,"
                   "\"known_bug_count\":%.0f,"
                   "\"known_bug_reproduced\":%.0f,"
                   "\"known_bug_fixed_candidates\":%.0f,"
                   "\"known_bug_lost_signal\":%.0f,"
                   "\"known_bug_baseline_failures\":%.0f,"
                   "\"quarantined_known_bugs\":%.0f,"
                   "\"lanes_with_quarantine\":%.0f,"
                   "\"perf_hotspots\":%.0f,\"perf_max_ratio\":%.4f,"
                   "\"status_blockers\":%.0f,"
                   "\"status_ready\":%s,"
                   "\"status_allow_incomplete_coverage\":%s,"
                   "\"long_run_ready\":%s,"
                   "\"target_reached\":%s,"
                   "\"campaign_complete\":%s,"
                   "\"status_cache_policy_ok\":%s,"
                   "\"status_old_nytrix_test_scratch_absent\":%s,"
                   "\"status_old_nytrix_fuzz_absent\":%s,"
                   "\"status_ny_bin_exists\":%s,"
                   "\"runs_needed\":%.0f,"
                   "\"wall_days_needed\":%.4f,"
                   "\"wall_hours_needed\":%.4f,"
                   "\"thread_hours_needed\":%.4f,"
                   "\"runs_per_day\":%.4f,"
                   "\"thread_years_per_day\":%.8f,"
                   "\"status_coverage_advisory_gaps\":%.0f,"
                   "\"status_coverage_disabled_lanes\":%.0f,"
                   "\"status_coverage_budget_short_lanes\":%.0f,"
                   "\"status_coverage_missing_tool_lanes\":%.0f,"
                   "\"status_full_pressure_reports\":%.0f,"
                   "\"status_full_pressure_thread_hours\":%.4f,"
                   "\"status_latest_full_pressure_clean\":%s,"
                   "\"status_active_compiler_finding_live\":%.0f,"
                   "\"status_active_compiler_finding_missing\":%.0f,"
                   "\"status_historical_compiler_finding_live\":%.0f,"
                   "\"status_historical_compiler_finding_missing\":%.0f,"
                   "\"status_active_known_bug_reproduced\":%.0f,"
                   "\"status_active_known_bug_lost_signal\":%.0f,"
                   "\"status_active_known_bug_baseline_failures\":%.0f,"
                   "\"status_historical_known_bug_reproduced\":%.0f,"
                   "\"status_historical_known_bug_lost_signal\":%.0f,"
                   "\"status_historical_known_bug_baseline_failures\":%.0f,"
                   "\"status_active_perf_hotspots\":%.0f,"
                   "\"status_active_perf_max_ratio\":%.4f,"
                   "\"status_historical_perf_hotspots\":%.0f,"
                   "\"status_historical_perf_max_ratio\":%.4f,"
                   "\"perf_max_case\":",
                   aborted_before_run ? "true" : "false",
                   allow_nytrix ? "false" : "true",
                   guard_nytrix ? "true" : "false",
                   include_afl ? "true" : "false",
                   allow_nytrix ? "true" : "false",
                   allow_dirty_nytrix_baseline ? "true" : "false",
                   low_priority_enabled ? "true" : "false",
                   preflight_low_priority_applied ? "true" : "false",
                   preflight_nice_target,
                   preflight_nice,
                   (guard_nytrix && nytrix_before.exists) ? "true" : "false",
                   nytrix_before.clean ? "true" : "false",
                   finding_count, finding_live, finding_cleared, finding_missing,
                   known_count, known_reproduced, known_fixed, known_lost,
                   known_baseline, quarantined, quarantine_lanes,
                   perf_hotspots, perf_max_ratio,
                   status_blockers, status_ready ? "true" : "false",
                   status_allow_incomplete_coverage ? "true" : "false",
                   long_run_ready ? "true" : "false",
                   status_target_reached ? "true" : "false",
                   status_campaign_complete ? "true" : "false",
                   status_cache_policy_ok ? "true" : "false",
                   status_old_scratch_absent ? "true" : "false",
                   status_old_fuzz_absent ? "true" : "false",
                   status_ny_bin_exists ? "true" : "false",
                   runs_needed,
                   wall_days_needed,
                   wall_hours_needed,
                   thread_hours_needed,
                   runs_per_day,
                   thread_years_per_day,
                   status_coverage_advisory_gaps,
                   status_coverage_disabled_lanes,
                   status_coverage_budget_short_lanes,
                   status_coverage_missing_tool_lanes,
                   status_full_pressure_reports,
                   status_full_pressure_thread_hours,
                   status_latest_full_pressure_clean ? "true" : "false",
                   status_active_finding_live,
                   status_active_finding_missing,
                   status_historical_finding_live,
                   status_historical_finding_missing,
                   status_active_known_reproduced,
                   status_active_known_lost,
                   status_active_known_baseline,
                   status_historical_known_reproduced,
                   status_historical_known_lost,
                   status_historical_known_baseline,
                   status_active_perf_hotspots,
                   status_active_perf_max_ratio,
                   status_historical_perf_hotspots,
                   status_historical_perf_max_ratio);
  (void)sb_append_json_str(&extra, perf_case ? perf_case : "");
  (void)sb_append(&extra, ",\"completion_eta_local\":");
  (void)sb_append_json_str(&extra, completion_eta_local ? completion_eta_local : "");
  (void)sb_append(&extra, ",\"nytrix_baseline_state_hash\":");
  (void)sb_append_json_str(&extra, nytrix_before.state_hash);
  (void)sb_append(&extra, ",\"abort_reason\":");
  (void)sb_append_json_str(&extra, abort_reason);
  (void)sb_append(&extra, ",\"work_dir\":");
  append_rel_json_str(&extra, root, work_dir ? work_dir : "");
  (void)sb_append(&extra, ",\"campaign_dir\":");
  append_rel_json_str(&extra, root, campaign_dir ? campaign_dir : "");
  (void)sb_append(&extra, ",\"run_report\":");
  append_rel_json_str(&extra, root, run_report);
  (void)sb_append(&extra, ",\"audit_report\":");
  append_rel_json_str(&extra, root, audit_report);
  (void)sb_append(&extra, ",\"findings_report\":");
  append_rel_json_str(&extra, root, findings_report);
  (void)sb_append(&extra, ",\"findings_markdown\":");
  append_rel_json_str(&extra, root, findings_markdown);
  (void)sb_append(&extra, ",\"history_report\":");
  append_rel_json_str(&extra, root, history_report);
  (void)sb_append(&extra, ",\"worklist_report\":");
  append_rel_json_str(&extra, root, worklist_report);
  (void)sb_append(&extra, ",\"coverage_report\":");
  append_rel_json_str(&extra, root, coverage_report);
  (void)sb_append(&extra, ",\"plan_report\":");
  append_rel_json_str(&extra, root, plan_report);
  (void)sb_append(&extra, ",\"status_report\":");
  append_rel_json_str(&extra, root, status_report);
  (void)sb_append(&extra, ",\"status_markdown\":");
  append_rel_json_str(&extra, root, status_markdown);
  (void)sb_append(&extra, ",\"dir\":");
  append_rel_json_str(&extra, root, campaign_dir ? campaign_dir : "");
  (void)sb_append(&extra, ",\"recommended_command\":");
  (void)sb_append_json_str(&extra,
                           status_recommended_command ?
                               status_recommended_command : "");
  (void)sb_append(&extra, ",\"recommended_action\":");
  (void)sb_append_json_str(&extra,
                           status_recommended_action ?
                               status_recommended_action : "");
  (void)sb_append(&extra, ",\"recommended_reason\":");
  (void)sb_append_json_str(&extra,
                           status_recommended_reason ?
                               status_recommended_reason : "");
  (void)sb_append(&extra, ",\"recommended_preview_command\":");
  (void)sb_append_json_str(&extra,
                           status_recommended_preview_command ?
                               status_recommended_preview_command : "");
  (void)sb_append(&extra, ",\"next_command\":");
  (void)sb_append_json_str(&extra,
                           status_next_command ? status_next_command : "");
  (void)sb_append(&extra, ",\"preview_command\":");
  (void)sb_append_json_str(&extra,
                           status_preview_command ?
                               status_preview_command : "");
  (void)sb_append(&extra, ",\"coverage_next_command\":");
  (void)sb_append_json_str(&extra,
                           status_coverage_next_command ?
                               status_coverage_next_command : "");
  (void)sb_append(&extra, ",\"coverage_next_guarded_command\":");
  (void)sb_append_json_str(&extra,
                           status_coverage_next_guarded_command ?
                               status_coverage_next_guarded_command : "");
  (void)sb_append(&extra, ",\"coverage_next_low_cpu_command\":");
  (void)sb_append_json_str(&extra,
                           status_coverage_next_low_cpu_command ?
                               status_coverage_next_low_cpu_command : "");
  (void)sb_append(&extra, ",\"recommended_low_cpu_command\":");
  (void)sb_append_json_str(&extra,
                           status_recommended_low_cpu_command ?
                               status_recommended_low_cpu_command : "");
  (void)sb_append(&extra, ",\"coverage_next_preview_command\":");
  (void)sb_append_json_str(&extra,
                           status_coverage_next_preview_command ?
                               status_coverage_next_preview_command : "");
  (void)sb_append(&extra, ",\"forever_script\":");
  append_rel_json_str(&extra, root, forever_script ? forever_script : "");
  (void)sb_append(&extra, ",\"next_script\":");
  append_rel_json_str(&extra, root, status_next_script ? status_next_script : "");
  (void)sb_append(&extra, ",\"status_latest_full_pressure_report\":");
  (void)sb_append_json_str(&extra, status_latest_full_pressure_report
                           ? status_latest_full_pressure_report : "");
  (void)sb_append(&extra, ",\"status_run_command\":");
  (void)sb_append_json_str(&extra, status_run_command ? status_run_command : "");
  (void)sb_append(&extra, ",\"status_ny_bin\":");
  (void)sb_append_json_str(&extra, status_ny_bin ? status_ny_bin : "");
  (void)sb_append(&extra, ",\"status_ny_bin_hash\":");
  (void)sb_append_json_str(&extra, status_ny_bin_hash ? status_ny_bin_hash : "");
  (void)sb_append(&extra, ",\"status_nytrix_git_head\":");
  (void)sb_append_json_str(&extra, status_nytrix_git_head ? status_nytrix_git_head : "");
  (void)sb_append(&extra, ",\"status_nytrix_git_head\":");
  (void)sb_append_json_str(&extra, status_nytrix_git_head ? status_nytrix_git_head : "");
  char *report = build_native_report_json(&rows, &failures, "fuzz-all-preflight", extra.data);
  int out_rc = emit_native_report(report, json_path, "all fuzz preflight",
                                  rows.count, failures.count);
  free(run_summary.data);
  free(status_summary.data);
  free(perf_case);
  free(forever_script);
  free(status_next_script);
  free(status_latest_full_pressure_report);
  free(status_run_command);
  free(status_recommended_action);
  free(status_recommended_reason);
  free(status_recommended_command);
  free(status_recommended_low_cpu_command);
  free(status_recommended_preview_command);
  free(status_next_command);
  free(status_preview_command);
  free(status_coverage_next_command);
  free(status_coverage_next_guarded_command);
  free(status_coverage_next_low_cpu_command);
  free(status_coverage_next_preview_command);
  free(status_ny_bin);
  free(status_ny_bin_hash);
  free(status_nytrix_git_head);
  free(status_nytrix_git_head);
  free(completion_eta_local);
  free(extra.data);
  free(work_dir);
  free(campaign_dir);
  free(run_report);
  free(audit_report);
  free(findings_report);
  free(findings_markdown);
  free(history_report);
  free(worklist_report);
  free(coverage_report);
  free(plan_report);
  free(status_report);
  free(status_markdown);
  nytrix_git_state_free(&nytrix_before);
  string_list_free(&rows);
  string_list_free(&failures);
  return out_rc;
}

static int cmd_public_fuzz_auto(int argc, char **argv) {
  char root[4096];
  if (!find_nytrix_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nytrix-root-not-found\"}\n");
    return 2;
  }
  bool once = has_flag_after(argc, argv, 2, "--once") ||
              has_flag_after(argc, argv, 2, "--single");
  int max_runs = atoi(value_after_equals(argc, argv, 2, "--runs", once ? "1" : "0"));
  if (once && max_runs < 1) max_runs = 1;
  const char *json_arg = value_after_equals(argc, argv, 2, "--json", "");
  bool has_profile = cli_has_named_arg(argc, argv, 2, "--profile");
  bool has_threads = cli_has_named_arg(argc, argv, 2, "--threads");
  bool has_duration = cli_has_any_duration_arg(argc, argv, 2);
  bool allow_nytrix = has_flag_after(argc, argv, 2, "--allow-nytrix");
  bool has_nytrix_policy = cli_has_named_arg(argc, argv, 2, "--no-nytrix") ||
                           cli_has_named_arg(argc, argv, 2, "--skip-nytrix");
  bool smoke = has_flag_after(argc, argv, 2, "--smoke") ||
               has_flag_after(argc, argv, 2, "--fast");
  bool keep_going = has_flag_after(argc, argv, 2, "--keep-going");
  const char *target_arg = value_after_equals(argc, argv, 2, "--target-thread-years", "");
  if (!target_arg || !*target_arg)
    target_arg = value_after_equals(argc, argv, 2, "--target-years", "");
  if (!target_arg || !*target_arg)
    target_arg = value_after_equals(argc, argv, 2, "--target", "10");
  const char *run_hours_arg = value_after_equals(argc, argv, 2, "--hours-per-run", "");
  if (!run_hours_arg || !*run_hours_arg)
    run_hours_arg = value_after_equals(argc, argv, 2, "--run-hours", "");
  const char *auto_threads_arg = value_after_equals(argc, argv, 2, "--threads",
                                                    NYTRIX_DEFAULT_FUZZ_THREADS);
  const char *auto_profile_arg = value_after_equals(argc, argv, 2, "--profile", "insane");
  double auto_duration_s = atof(value_after_equals(argc, argv, 2, "--budget-s", "0"));
  if (auto_duration_s <= 0.0)
    auto_duration_s = atof(value_after_equals(argc, argv, 2, "--duration-s", "0"));
  if (auto_duration_s <= 0.0) {
    double minutes = atof(value_after_equals(argc, argv, 2, "--minutes", "0"));
    if (minutes > 0.0) auto_duration_s = minutes * 60.0;
  }
  if (auto_duration_s <= 0.0) {
    double hours = atof(value_after_equals(argc, argv, 2, "--hours", "0"));
    if (hours > 0.0) auto_duration_s = hours * 3600.0;
  }
  if (auto_duration_s <= 0.0 && run_hours_arg && *run_hours_arg) {
    double hours = atof(run_hours_arg);
    if (hours > 0.0) auto_duration_s = hours * 3600.0;
  }
  if (auto_duration_s <= 0.0) auto_duration_s = smoke ? 45.0 : 8.0 * 3600.0;
  char auto_hours_buf[64];
  snprintf(auto_hours_buf, sizeof(auto_hours_buf), "%.6f", auto_duration_s / 3600.0);
  const char *dir_arg = value_after_equals(argc, argv, 2, "--dir", "");
  if (!dir_arg || !*dir_arg)
    dir_arg = value_after_equals(argc, argv, 2, "--history-dir", "build/fuzz/all");
  char *dir_path = NULL;
  if (dir_arg && *dir_arg) {
    if (path_is_absolute(dir_arg)) dir_path = strdup(dir_arg);
    else (void)nytrix_asprintf(&dir_path, "%s", dir_arg);
  }
  if (!dir_path || !*dir_path) {
    free(dir_path);
    dir_path = strdup("build/fuzz/all");
  }
  nytrix_redirect_nytrix_output_dir(&dir_path, root, "fuzz-auto");
  if (dir_path) ny_ensure_dir_recursive(dir_path);
  int run_index = 0;
  while (max_runs <= 0 || run_index < max_runs) {
    bool campaign_complete = false;
    int guard_rc = fuzz_auto_refresh_status_guard(root, dir_path, target_arg,
                                                  auto_hours_buf,
                                                  auto_threads_arg,
                                                  auto_profile_arg,
                                                  &campaign_complete);
    if (guard_rc != 0) {
      free(dir_path);
      return guard_rc;
    }
    if (campaign_complete) {
      printf("nytrix fuzz auto target complete; no further runs needed\n");
      free(dir_path);
      return 0;
    }
    char *json_path = NULL;
    if (json_arg && *json_arg && max_runs == 1) {
      json_path = strdup(json_arg);
    } else {
      const char *auto_dir = dir_path && *dir_path ? dir_path : "build/fuzz/all";
      if (path_is_absolute(auto_dir))
        (void)asprintf(&json_path, "%s/auto_%ld_%04d.json",
                       auto_dir, (long)time(NULL), run_index);
      else
        (void)asprintf(&json_path, "%s/%s/auto_%ld_%04d.json",
                       root, auto_dir, (long)time(NULL), run_index);
    }
    if (!json_path) {
      printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
      return 2;
    }
    char *child_argv[128];
    int ca = 0;
    child_argv[ca++] = g_self_path;
    child_argv[ca++] = "fuzz";
    child_argv[ca++] = "all";
    child_argv[ca++] = "run";
    if (!has_profile) {
      child_argv[ca++] = "--profile";
      child_argv[ca++] = "insane";
    }
    if (!has_duration && run_hours_arg && *run_hours_arg) {
      child_argv[ca++] = "--hours";
      child_argv[ca++] = (char *)run_hours_arg;
    } else if (!has_duration && !smoke) {
      child_argv[ca++] = "--hours";
      child_argv[ca++] = "8";
    }
    if (!has_threads) {
      child_argv[ca++] = "--threads";
      child_argv[ca++] = (char *)NYTRIX_DEFAULT_FUZZ_THREADS;
    }
    if (!keep_going) child_argv[ca++] = "--fail-fast";
    if (allow_nytrix && !has_nytrix_policy) child_argv[ca++] = "--allow-nytrix";
    else if (!allow_nytrix && !has_nytrix_policy) child_argv[ca++] = "--no-nytrix";
    child_argv[ca++] = "--dir";
    child_argv[ca++] = dir_path && *dir_path ? dir_path : "build/fuzz/all";
    child_argv[ca++] = "--target-thread-years";
    child_argv[ca++] = (char *)(target_arg && *target_arg ? target_arg : "10");
    for (int i = 2; i < argc && ca < (int)(sizeof(child_argv) / sizeof(child_argv[0])) - 3; ++i) {
      if (fuzz_auto_skip_copy_arg(argc, argv, &i)) continue;
      child_argv[ca++] = argv[i];
    }
    child_argv[ca++] = "--json";
    child_argv[ca++] = json_path;
    child_argv[ca] = NULL;
    printf("nytrix fuzz auto run %d json=%s\n", run_index + 1, json_path);
    fflush(stdout);
    int rc = cmd_public_fuzz_all_run(ca, child_argv);
    if (rc != 0) {
      free(json_path);
      return rc;
    }
    char *audit_path = json_path_with_suffix(json_path, "-audit");
    char *findings_path = json_path_with_suffix(json_path, "-findings");
    char *findings_md_path = path_with_suffix_ext(json_path, "-findings", ".md");
    char *coverage_path = json_path_with_suffix(json_path, "-coverage");
    char *coverage_md_path = path_with_suffix_ext(json_path, "-coverage", ".md");
    if (!audit_path || !findings_path || !findings_md_path ||
        !coverage_path || !coverage_md_path) {
      free(json_path);
      free(audit_path);
      free(findings_path);
      free(findings_md_path);
      free(coverage_path);
      free(coverage_md_path);
      printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
      return 2;
    }
    char *audit_argv[] = {
      g_self_path, "fuzz", "all", "audit", "--report", json_path,
      "--strict", "--json", audit_path, NULL
    };
    printf("nytrix fuzz auto audit %d json=%s\n", run_index + 1, audit_path);
    fflush(stdout);
    rc = cmd_public_fuzz_all_audit(9, audit_argv);
    if (rc != 0) {
      free(json_path);
      free(audit_path);
      free(findings_path);
      free(findings_md_path);
      return rc;
    }
    char *findings_argv[] = {
      g_self_path, "fuzz", "all", "findings", "--report", json_path,
      "--json", findings_path, "--markdown", findings_md_path, NULL
    };
    printf("nytrix fuzz auto findings %d json=%s\n", run_index + 1, findings_path);
    fflush(stdout);
    rc = cmd_public_fuzz_all_findings(10, findings_argv);
    if (rc == 0) {
      char *coverage_argv[] = {
        g_self_path, "fuzz", "all", "coverage", "--report", json_path,
        "--json", coverage_path, "--markdown", coverage_md_path, NULL
      };
      printf("nytrix fuzz auto coverage %d json=%s\n", run_index + 1, coverage_path);
      fflush(stdout);
      rc = cmd_public_fuzz_all_coverage(10, coverage_argv);
    }
    char *coverage_latest_path = NULL, *coverage_latest_md_path = NULL;
    (void)asprintf(&coverage_latest_path, "%s/coverage.json",
                   dir_path && *dir_path ? dir_path : "build/fuzz/all");
    (void)asprintf(&coverage_latest_md_path, "%s/coverage.md",
                   dir_path && *dir_path ? dir_path : "build/fuzz/all");
    if (rc == 0) {
      char *history_path = NULL, *history_md_path = NULL;
      (void)asprintf(&history_path, "%s/history.json",
                     dir_path && *dir_path ? dir_path : "build/fuzz/all");
      (void)asprintf(&history_md_path, "%s/history.md",
                     dir_path && *dir_path ? dir_path : "build/fuzz/all");
      if (history_path && history_md_path) {
        char *history_argv[] = {
          g_self_path, "fuzz", "all", "history", "--dir",
          dir_path && *dir_path ? dir_path : "build/fuzz/all",
          "--json", history_path, "--markdown", history_md_path, NULL
        };
        printf("nytrix fuzz auto history %d json=%s\n", run_index + 1, history_path);
        fflush(stdout);
        rc = cmd_public_fuzz_all_history(10, history_argv);
      } else {
        rc = 2;
      }
      free(history_path);
      free(history_md_path);
    }
    if (rc == 0) {
      char *history_path = NULL;
      (void)asprintf(&history_path, "%s/history.json",
                     dir_path && *dir_path ? dir_path : "build/fuzz/all");
      if (history_path && coverage_latest_path && coverage_latest_md_path) {
        char *coverage_history_argv[] = {
          g_self_path, "fuzz", "all", "coverage", "--strict",
          "--history", history_path,
          "--target-thread-years", (char *)(target_arg && *target_arg ?
                                            target_arg : "10"),
          "--hours", auto_hours_buf,
          "--threads", (char *)(auto_threads_arg && *auto_threads_arg ?
                                auto_threads_arg : NYTRIX_DEFAULT_FUZZ_THREADS),
          "--profile", (char *)(auto_profile_arg && *auto_profile_arg ?
                                 auto_profile_arg : "insane"),
          "--json", coverage_latest_path,
          "--markdown", coverage_latest_md_path, NULL
        };
        printf("nytrix fuzz auto history coverage %d json=%s\n", run_index + 1,
               coverage_latest_path);
        fflush(stdout);
        rc = cmd_public_fuzz_all_coverage(19, coverage_history_argv);
      } else {
        rc = 2;
      }
      free(history_path);
    }
    if (rc == 0) {
      char *worklist_path = NULL, *worklist_md_path = NULL;
      (void)asprintf(&worklist_path, "%s/worklist.json",
                     dir_path && *dir_path ? dir_path : "build/fuzz/all");
      (void)asprintf(&worklist_md_path, "%s/worklist.md",
                     dir_path && *dir_path ? dir_path : "build/fuzz/all");
      if (worklist_path && worklist_md_path) {
        char *history_path = NULL;
        (void)asprintf(&history_path, "%s/history.json",
                       dir_path && *dir_path ? dir_path : "build/fuzz/all");
        char *worklist_argv[] = {
          g_self_path, "fuzz", "all", "worklist",
          "--history", history_path ? history_path : "build/fuzz/all/history.json",
          "--json", worklist_path, "--markdown", worklist_md_path, NULL
        };
        printf("nytrix fuzz auto worklist %d json=%s\n", run_index + 1, worklist_path);
        fflush(stdout);
        rc = cmd_public_fuzz_all_worklist(10, worklist_argv);
        free(history_path);
      } else {
        rc = 2;
      }
      free(worklist_path);
      free(worklist_md_path);
    }
    if (rc == 0) {
      char *plan_path = NULL, *plan_md_path = NULL;
      (void)asprintf(&plan_path, "%s/plan.json",
                     dir_path && *dir_path ? dir_path : "build/fuzz/all");
      (void)asprintf(&plan_md_path, "%s/plan.md",
                     dir_path && *dir_path ? dir_path : "build/fuzz/all");
      if (plan_path && plan_md_path) {
        char *history_path = NULL, *worklist_path = NULL;
        (void)asprintf(&history_path, "%s/history.json",
                       dir_path && *dir_path ? dir_path : "build/fuzz/all");
        (void)asprintf(&worklist_path, "%s/worklist.json",
                       dir_path && *dir_path ? dir_path : "build/fuzz/all");
        char *plan_argv[] = {
          g_self_path, "fuzz", "all", "plan",
          "--dir", dir_path && *dir_path ? dir_path : "build/fuzz/all",
          "--history", history_path ? history_path : "build/fuzz/all/history.json",
          "--worklist", worklist_path ? worklist_path : "build/fuzz/all/worklist.json",
          "--coverage", coverage_latest_path ? coverage_latest_path : "build/fuzz/all/coverage.json",
          "--target-thread-years", (char *)target_arg,
          "--hours", auto_hours_buf,
          "--threads", (char *)auto_threads_arg,
          "--profile", (char *)auto_profile_arg,
          "--json", plan_path, "--markdown", plan_md_path, NULL
        };
        printf("nytrix fuzz auto plan %d json=%s\n", run_index + 1, plan_path);
        fflush(stdout);
        rc = cmd_public_fuzz_all_plan(24, plan_argv);
        if (rc == 0) {
          char *status_path = NULL, *status_md_path = NULL;
          (void)asprintf(&status_path, "%s/status.json",
                         dir_path && *dir_path ? dir_path : "build/fuzz/all");
          (void)asprintf(&status_md_path, "%s/status.md",
                         dir_path && *dir_path ? dir_path : "build/fuzz/all");
          if (status_path && status_md_path) {
            char *status_argv[] = {
              g_self_path, "fuzz", "all", "status", "--strict",
              "--dir", dir_path && *dir_path ? dir_path : "build/fuzz/all",
              "--history", history_path ? history_path : "build/fuzz/all/history.json",
              "--worklist", worklist_path ? worklist_path : "build/fuzz/all/worklist.json",
              "--coverage", coverage_latest_path ? coverage_latest_path : "build/fuzz/all/coverage.json",
              "--plan", plan_path,
              "--target-thread-years", (char *)target_arg,
              "--hours", auto_hours_buf,
              "--threads", (char *)auto_threads_arg,
              "--profile", (char *)auto_profile_arg,
              "--json", status_path, "--markdown", status_md_path, NULL
            };
            printf("nytrix fuzz auto status %d json=%s\n", run_index + 1, status_path);
            fflush(stdout);
            rc = cmd_public_fuzz_all_status(27, status_argv);
          } else {
            rc = 2;
          }
          free(status_path);
          free(status_md_path);
        }
        free(history_path);
        free(worklist_path);
      } else {
        rc = 2;
      }
      free(plan_path);
      free(plan_md_path);
    }
    free(json_path);
    free(audit_path);
    free(findings_path);
    free(findings_md_path);
    free(coverage_path);
    free(coverage_md_path);
    free(coverage_latest_path);
    free(coverage_latest_md_path);
    if (rc != 0) return rc;
    ++run_index;
    if (max_runs > 0 && run_index >= max_runs) break;
    sleep(5);
  }
  free(dir_path);
  return 0;
}

typedef struct {
  const char *dst;
  const char *src;
} json_alias_spec_t;

static const char *json_scalar_token_end(const char *p, const char *end) {
  if (!p || !end || p >= end) return NULL;
  p = skip_ws_const(p);
  if (p >= end) return NULL;
  if (*p == '"') {
    const char *q = p + 1;
    bool escaped = false;
    while (q < end) {
      if (escaped) {
        escaped = false;
      } else if (*q == '\\') {
        escaped = true;
      } else if (*q == '"') {
        return q + 1;
      }
      ++q;
    }
    return NULL;
  }
  if (*p == '{') {
    const char *q = matching_json_end(p, '{', '}');
    return q ? q + 1 : NULL;
  }
  if (*p == '[') {
    const char *q = matching_json_end(p, '[', ']');
    return q ? q + 1 : NULL;
  }
  const char *q = p;
  while (q < end && *q != ',' && *q != '}' && *q != ']') ++q;
  while (q > p && isspace((unsigned char)q[-1])) --q;
  return q > p ? q : NULL;
}

static bool json_top_level_number_from_report(const char *json,
                                              const char *key,
                                              double *out) {
  if (!json || !key || !*key) return false;
  const char *p = json_top_level_value_after_key(json, key);
  if (!p) return false;
  const char *end = json + strlen(json);
  const char *q = json_scalar_token_end(p, end);
  if (!q || q <= p) return false;
  char *next = NULL;
  double value = strtod(p, &next);
  if (next == p || next > q) return false;
  if (out) *out = value;
  return true;
}

static bool json_top_level_bool_from_report(const char *json,
                                            const char *key,
                                            bool *out) {
  if (!json || !key || !*key) return false;
  const char *p = json_top_level_value_after_key(json, key);
  if (!p) return false;
  const char *end = json + strlen(json);
  if ((size_t)(end - p) >= 4 && strncmp(p, "true", 4) == 0) {
    if (out) *out = true;
    return true;
  }
  if ((size_t)(end - p) >= 5 && strncmp(p, "false", 5) == 0) {
    if (out) *out = false;
    return true;
  }
  return false;
}

static char *json_top_level_string_from_report(const char *json,
                                               const char *key) {
  if (!json || !key || !*key) return strdup("");
  const char *p = json_top_level_value_after_key(json, key);
  if (!p || *p != '"') return strdup("");
  const char *end = json + strlen(json);
  char *value = parse_json_string_dup(&p, end);
  return value ? value : strdup("");
}

static void append_json_alias_from_fragment(str_buf_t *b,
                                            const char *fragment,
                                            const char *dst,
                                            const char *src) {
  if (!b || !fragment || !*fragment || !dst || !*dst || !src || !*src)
    return;
  const char *end = fragment + strlen(fragment);
  const char *p = json_value_after_key_range(fragment, end, src);
  if (!p || p >= end) return;
  const char *q = json_scalar_token_end(p, end);
  if (!q || q <= p || q > end) return;
  (void)sb_append(b, ",");
  (void)sb_append_json_str(b, dst);
  (void)sb_append(b, ":");
  (void)sb_append_n(b, p, (size_t)(q - p));
}

static void append_fuzz_all_top_aliases(str_buf_t *b,
                                        const char *summary_extra) {
  static const json_alias_spec_t aliases[] = {
    {"ready", "ready"},
    {"blockers", "blockers"},
    {"blocker_count", "blocker_count"},
    {"active_count", "active_items"},
    {"active_items", "active_items"},
    {"active_runs", "active_runs"},
    {"coverage_percent", "coverage_percent"},
    {"coverage_state", "coverage_state"},
    {"coverage_backlog_lanes", "coverage_backlog_lanes"},
    {"coverage_queue_count", "coverage_queue_count"},
    {"coverage_queue_non_advisory_count",
     "coverage_queue_non_advisory_count"},
    {"coverage_queue_advisory_count", "coverage_queue_advisory_count"},
    {"coverage_queue_lanes", "coverage_queue_lanes"},
    {"coverage_queue", "coverage_queue"},
    {"coverage_blocker_gaps", "coverage_blocker_gaps"},
    {"active_worklist_items", "active_worklist_items"},
    {"reports", "reports"},
    {"full_pressure_reports", "full_pressure_reports"},
    {"checked_subcases", "checked_subcases"},
    {"full_pressure_thread_years", "full_pressure_thread_years"},
    {"latest_report", "latest_report"},
    {"latest_full_pressure_report", "latest_full_pressure_report"},
    {"next_script", "next_script"},
    {"next_handoff_command", "next_handoff_command"},
    {"next", "next"},
    {"next_command", "next_command"},
    {"preview_command", "preview_command"},
    {"run_next_command", "run_next_command"},
    {"run_next_preview_command", "run_next_preview_command"},
    {"run_next_low_cpu_command", "run_next_low_cpu_command"},
    {"run_next_gentle_command", "run_next_gentle_command"},
    {"run_next_gentle_preview_command",
     "run_next_gentle_preview_command"},
    {"stop_file", "stop_file"},
    {"stop_command", "stop_command"},
    {"resume_command", "resume_command"},
    {"progress_command", "progress_command"},
    {"status_command", "status_command"},
    {"quick_probe_command", "quick_probe_command"},
    {"state_probe_command", "state_probe_command"},
    {"selftest_catalog_command", "selftest_catalog_command"},
    {"selftest_result_probe_command", "selftest_result_probe_command"},
    {"selftest_cockpit_run_command", "selftest_cockpit_run_command"},
    {"selftest_cockpit_result_probe_command",
     "selftest_cockpit_result_probe_command"},
    {"known_bugs_command", "known_bugs_command"},
    {"known_bugs_report", "known_bugs_report"},
    {"known_bugs_markdown", "known_bugs_markdown"},
    {"known_bugs_result_probe_command", "known_bugs_result_probe_command"},
    {"known_bugs_readable", "known_bugs_readable"},
    {"perf_triage_command", "perf_triage_command"},
    {"perf_triage_report", "perf_triage_report"},
    {"perf_triage_markdown", "perf_triage_markdown"},
    {"perf_triage_result_probe_command", "perf_triage_result_probe_command"},
    {"perf_triage_readable", "perf_triage_readable"},
    {"perf_triage_cases", "perf_triage_cases"},
    {"perf_triage_ok_count", "perf_triage_ok_count"},
    {"perf_triage_failure_count", "perf_triage_failure_count"},
    {"perf_triage_hotspots", "perf_triage_hotspots"},
    {"perf_triage_worst_ratio", "perf_triage_worst_ratio"},
    {"perf_triage_worst_slowdown_percent",
     "perf_triage_worst_slowdown_percent"},
    {"perf_triage_worst_case", "perf_triage_worst_case"},
    {"old_path_probe_command", "old_path_probe_command"},
    {"old_path_command", "old_path_command"},
    {"old_path_dry_run_command", "old_path_dry_run_command"},
    {"old_path_apply_command", "old_path_apply_command"},
    {"old_path_next_action", "old_path_next_action"},
    {"old_path_next_reason", "old_path_next_reason"},
    {"old_path_report", "old_path_report"},
    {"old_path_markdown", "old_path_markdown"},
    {"old_path_cache_policy_ok", "old_path_cache_policy_ok"},
    {"old_path_present_count", "old_path_present_count"},
    {"old_path_moved_count", "old_path_moved_count"},
    {"old_path_remaining_count", "old_path_remaining_count"},
    {"old_path_wait_remaining_seconds",
     "old_path_wait_remaining_seconds"},
    {"old_path_artifact_leak_count", "old_path_artifact_leak_count"},
    {"old_path_artifact_moved_count", "old_path_artifact_moved_count"},
    {"old_path_artifact_remaining_count",
     "old_path_artifact_remaining_count"},
    {"latest_full_pressure_raw_ok", "latest_full_pressure_raw_ok"},
    {"latest_full_pressure_effective_clean",
     "latest_full_pressure_effective_clean"},
    {"latest_full_pressure_clean_reason",
     "latest_full_pressure_clean_reason"},
    {"latest_full_pressure_failure_count",
     "latest_full_pressure_failure_count"},
       {"latest_full_pressure_demoted_non_reproducing_afl_timeout",
        "latest_full_pressure_demoted_non_reproducing_afl_timeout"},
       {"latest_report_stale_after_hours", "latest_report_stale_after_hours"},
       {"latest_report_freshness_remaining_hours",
        "latest_report_freshness_remaining_hours"},
       {"latest_report_freshness_overdue_hours",
        "latest_report_freshness_overdue_hours"},
       {"latest_full_pressure_report_freshness_remaining_hours",
        "latest_full_pressure_report_freshness_remaining_hours"},
       {"latest_full_pressure_report_freshness_overdue_hours",
        "latest_full_pressure_report_freshness_overdue_hours"},
       {"evidence_fresh", "evidence_fresh"},
       {"evidence_freshness_overdue_hours",
        "evidence_freshness_overdue_hours"},
       {"freshness_penalty", "freshness_penalty"},
    {"latest_h", "latest_report_age_hours"},
    {"latest_over_h", "latest_report_freshness_overdue_hours"},
    {"full_h", "latest_full_pressure_report_age_hours"},
    {"full_over_h",
     "latest_full_pressure_report_freshness_overdue_hours"},
    {"over_h", "evidence_freshness_overdue_hours"},
    {"freshness_action_command", "freshness_action_command"},
    {"latest_report_freshness_command", "latest_report_freshness_command"},
    {"latest_full_pressure_report_freshness_command",
     "latest_full_pressure_report_freshness_command"},
    {"full_pressure_freshen_command", "full_pressure_freshen_command"},
    {"full_pressure_remediation_command", "full_pressure_remediation_command"},
    {"full_pressure_action_command", "full_pressure_action_command"},
    {"campaign_percent", "campaign_percent"},
    {"campaign_remaining_percent", "campaign_remaining_percent"},
    {"thread_years", "thread_years"},
    {"target_thread_years", "target_thread_years"},
    {"remaining_thread_years", "remaining_thread_years"},
    {"campaign_thread_years", "campaign_thread_years"},
    {"campaign_target_thread_years", "campaign_target_thread_years"},
    {"campaign_remaining_thread_years", "campaign_remaining_thread_years"},
    {"campaign_done_percent", "campaign_done_percent"},
    {"campaign_runs_needed", "campaign_runs_needed"},
    {"campaign_wall_hours_needed", "campaign_wall_hours_needed"},
    {"campaign_wall_days_needed", "campaign_wall_days_needed"},
    {"campaign_thread_years_per_run", "campaign_thread_years_per_run"},
    {"campaign_percent_per_run", "campaign_percent_per_run"},
    {"thread_years_per_run_source", "thread_years_per_run_source"},
    {"campaign_plan_wall_hours", "campaign_plan_wall_hours"},
    {"campaign_plan_threads", "campaign_plan_threads"},
    {"campaign_runs_per_wall_day", "campaign_runs_per_wall_day"},
    {"campaign_thread_years_per_wall_day",
     "campaign_thread_years_per_wall_day"},
    {"campaign_percent_per_wall_day", "campaign_percent_per_wall_day"},
    {"campaign_equivalent_wall_days", "campaign_equivalent_wall_days"},
    {"campaign_first_report", "campaign_first_report"},
    {"campaign_first_report_epoch", "campaign_first_report_epoch"},
    {"campaign_latest_report_epoch", "campaign_latest_report_epoch"},
    {"campaign_calendar_span_days", "campaign_calendar_span_days"},
       {"campaign_calendar_age_days", "campaign_calendar_age_days"},
       {"campaign_calendar_percent_10y", "campaign_calendar_percent_10y"},
       {"campaign_eta_local", "campaign_eta_local"},
    {"score", "score_percent"},
    {"score_percent", "score_percent"},
    {"score_label", "language_score_label"},
    {"stability_percent", "stability_percent"},
    {"stability_score", "stability_score"},
    {"stability_label", "stability_label"},
    {"stability_note", "stability_note"},
    {"language_score", "language_score"},
    {"language_score_percent", "language_score_percent"},
    {"language_score_label", "language_score_label"},
    {"completion_state", "completion_state"},
    {"completion_reason", "completion_reason"},
    {"language_score_good_threshold_percent",
     "language_score_good_threshold_percent"},
    {"language_score_signal_percent", "language_score_signal_percent"},
    {"language_score_evidence_cap_percent",
     "language_score_evidence_cap_percent"},
    {"signal_health_percent", "signal_health_percent"},
    {"evidence_cap_percent", "evidence_cap_percent"},
    {"language_score_note", "language_score_note"},
    {"language_score_gap_percent", "language_score_gap_percent"},
    {"next_run_language_score_percent",
     "next_run_language_score_percent"},
    {"next_run_language_score", "next_run_language_score"},
    {"next_run_language_score_delta_percent",
     "next_run_language_score_delta_percent"},
    {"stability_score_percent", "stability_score_percent"},
    {"next_run_stability_score_percent",
     "next_run_stability_score_percent"},
    {"next_run_stability_delta_percent",
     "next_run_stability_delta_percent"},
    {"runs_to_good_stability", "runs_to_good_stability"},
    {"runs_to_good_stability_days", "runs_to_good_stability_days"},
    {"days_to_good_stability", "days_to_good_stability"},
    {"runs_to_good_language_score", "runs_to_good_language_score"},
    {"runs_to_good_language_days", "runs_to_good_language_days"},
    {"runs_to_good_days", "runs_to_good_days"},
    {"days_to_good_language_score", "days_to_good_language_score"},
    {"runs_needed", "runs_needed"},
    {"wall_days_needed", "wall_days_needed"},
    {"completion_eta_local", "completion_eta_local"},
    {"recommended_action", "recommended_action"},
    {"recommended_reason", "recommended_reason"},
    {"recommended_repeat_mode", "recommended_repeat_mode"},
    {"recommended_repeat_count", "recommended_repeat_count"},
    {"recommended_command", "recommended_command"},
    {"recommended_low_cpu_command", "recommended_low_cpu_command"},
    {"recommended_preview_command", "recommended_preview_command"},
    {"plan_next_action", "plan_next_action"},
    {"plan_next_lane", "plan_next_lane"},
    {"plan_next_reason", "plan_next_reason"},
    {"plan_next_command", "plan_next_command"},
    {"plan_next_low_cpu_command", "plan_next_low_cpu_command"},
    {"plan_next_preview_command", "plan_next_preview_command"},
    {"coverage_next_action", "coverage_next_action"},
    {"coverage_next_category", "coverage_next_category"},
    {"coverage_next_severity", "coverage_next_severity"},
    {"coverage_next_lane", "coverage_next_lane"},
    {"coverage_next_reason", "coverage_next_reason"},
    {"coverage_next_command", "coverage_next_command"},
    {"coverage_next_guarded_command", "coverage_next_guarded_command"},
    {"coverage_next_low_cpu_command", "coverage_next_low_cpu_command"},
    {"coverage_next_preview_command", "coverage_next_preview_command"},
    {"coverage_next_state", "coverage_next_state"},
    {"coverage_next_state_phase", "coverage_next_state_phase"},
    {"coverage_next_state_event", "coverage_next_state_event"},
    {"coverage_next_state_readable", "coverage_next_state_readable"},
    {"coverage_next_state_fresh", "coverage_next_state_fresh"},
    {"coverage_next_state_live", "coverage_next_state_live"},
    {"coverage_next_state_age_seconds", "coverage_next_state_age_seconds"},
    {"coverage_next_state_stale_after_seconds",
     "coverage_next_state_stale_after_seconds"},
    {"coverage_next_state_stale_reason", "coverage_next_state_stale_reason"},
    {"coverage_next_state_child_status", "coverage_next_state_child_status"},
    {"coverage_next_state_file", "coverage_next_state_file"},
    {"coverage_next_state_command", "coverage_next_state_command"},
    {"coverage_next_state_refresh_command",
     "coverage_next_state_refresh_command"},
    {"coverage_next_state_refresh_required",
     "coverage_next_state_refresh_required"},
    {"coverage_next_state_refresh_reason",
     "coverage_next_state_refresh_reason"},
    {"coverage_next_state_dry_run_exceeds_max",
     "coverage_next_state_dry_run_exceeds_max"},
    {"coverage_next_state_dry_run_wall_hours",
     "coverage_next_state_dry_run_wall_hours"},
    {"coverage_next_state_dry_run_wall_days",
     "coverage_next_state_dry_run_wall_days"},
    {"coverage_next_state_dry_run_thread_years",
     "coverage_next_state_dry_run_thread_years"},
    {"coverage_next_state_dry_run_campaign_gain_percent",
     "coverage_next_state_dry_run_campaign_gain_percent"},
    {"coverage_next_state_dry_run_target_percent_per_run",
     "coverage_next_state_dry_run_target_percent_per_run"},
    {"coverage_next_state_dry_run_thread_years_per_run",
     "coverage_next_state_dry_run_thread_years_per_run"},
    {"coverage_next_state_handoff_threads",
     "coverage_next_state_handoff_threads"},
    {"coverage_next_state_canonical_status_report",
     "coverage_next_state_canonical_status_report"},
    {"coverage_next_state_canonical_progress_report",
     "coverage_next_state_canonical_progress_report"},
    {"coverage_next_stop_file", "coverage_next_stop_file"},
    {"coverage_next_stop_command", "coverage_next_stop_command"},
    {"coverage_next_resume_command", "coverage_next_resume_command"},
    {"recommended_state", "recommended_state"},
    {"recommended_state_fresh", "recommended_state_fresh"},
    {"recommended_state_live", "recommended_state_live"},
    {"recommended_state_age_seconds", "recommended_state_age_seconds"},
    {"recommended_state_stale_after_seconds",
     "recommended_state_stale_after_seconds"},
    {"recommended_state_stale_reason", "recommended_state_stale_reason"},
    {"recommended_state_child_status", "recommended_state_child_status"},
    {"recommended_state_source", "recommended_state_source"},
    {"recommended_state_file", "recommended_state_file"},
    {"recommended_state_command", "recommended_state_command"},
    {"recommended_state_refresh_required",
     "recommended_state_refresh_required"},
    {"recommended_state_refresh_reason",
     "recommended_state_refresh_reason"},
    {"recommended_state_refresh_command",
     "recommended_state_refresh_command"},
    {"recommended_state_dry_run_exceeds_max",
     "recommended_state_dry_run_exceeds_max"},
    {"recommended_state_dry_run_wall_hours",
     "recommended_state_dry_run_wall_hours"},
    {"recommended_state_dry_run_wall_days",
     "recommended_state_dry_run_wall_days"},
    {"recommended_state_dry_run_thread_years",
     "recommended_state_dry_run_thread_years"},
    {"recommended_state_dry_run_campaign_gain_percent",
     "recommended_state_dry_run_campaign_gain_percent"},
    {"recommended_state_dry_run_target_percent_per_run",
     "recommended_state_dry_run_target_percent_per_run"},
    {"recommended_state_dry_run_thread_years_per_run",
     "recommended_state_dry_run_thread_years_per_run"},
    {"recommended_state_handoff_threads",
     "recommended_state_handoff_threads"},
    {"recommended_state_canonical_status_report",
     "recommended_state_canonical_status_report"},
    {"recommended_state_canonical_progress_report",
     "recommended_state_canonical_progress_report"},
    {"state_live", "state_live"},
    {"state_file", "state_file"},
    {"state_command", "state_command"},
    {"state_refresh_command", "state_refresh_command"},
    {"state", "state"},
    {"state_phase", "state_phase"},
    {"state_event", "state_event"},
    {"state_age_seconds", "state_age_seconds"},
    {"state_stale_after_seconds", "state_stale_after_seconds"},
    {"state_fresh", "state_fresh"},
    {"state_child_status", "state_child_status"},
    {"state_stale_reason", "state_stale_reason"},
    {"state_dry_run_exceeds_max", "state_dry_run_exceeds_max"},
    {"state_dry_run_wall_hours", "state_dry_run_wall_hours"},
    {"state_dry_run_wall_days", "state_dry_run_wall_days"},
    {"state_dry_run_thread_years", "state_dry_run_thread_years"},
    {"state_dry_run_campaign_gain_percent",
     "state_dry_run_campaign_gain_percent"},
    {"state_dry_run_target_percent_per_run",
     "state_dry_run_target_percent_per_run"},
    {"state_dry_run_thread_years_per_run",
     "state_dry_run_thread_years_per_run"},
    {"state_handoff_threads", "state_handoff_threads"},
    {"state_canonical_status_report", "state_canonical_status_report"},
    {"state_canonical_progress_report", "state_canonical_progress_report"},
    {"latest_report_fresh", "latest_report_fresh"},
    {"latest_report_age_hours", "latest_report_age_hours"},
    {"latest_full_pressure_report_fresh",
     "latest_full_pressure_report_fresh"},
    {"latest_full_pressure_report_age_hours",
     "latest_full_pressure_report_age_hours"},
    {"latest_full_pressure_report_stale_after_hours",
     "latest_full_pressure_report_stale_after_hours"},
    {"advisory_state", "advisory_state"},
    {"advisory_recheck_state", "advisory_recheck_state"},
    {"current_advisory_timeouts", "current_advisory_timeouts"},
    {"effective_advisory_timeouts", "effective_advisory_timeouts"},
    {"advisory_effective_timeouts", "advisory_effective_timeouts"},
    {"advisory_penalty_state", "advisory_penalty_state"},
    {"historical_non_reproducing_afl_timeouts",
     "historical_non_reproducing_afl_timeouts"},
    {"advisory_recheck_raw_repro_checked",
     "advisory_recheck_raw_repro_checked"},
    {"advisory_recheck_raw_repro_passed",
     "advisory_recheck_raw_repro_passed"},
    {"advisory_recheck_raw_repro_timeouts",
     "advisory_recheck_raw_repro_timeouts"},
    {"advisory_recheck_raw_repro_unexpected",
     "advisory_recheck_raw_repro_unexpected"},
    {"advisory_penalty", "advisory_penalty"},
    {"correctness_findings", "correctness_findings"},
    {"compiler_findings", "compiler_findings"},
    {"known_bug_replay_findings", "known_bug_replay_findings"},
    {"known_bug_count", "known_bug_count"},
    {"reproduced", "reproduced"},
    {"fixed_candidates", "fixed_candidates"},
    {"lost_signal", "lost_signal"},
    {"baseline_failures", "baseline_failures"},
    {"known_bug_reproduced", "known_bug_reproduced"},
    {"known_bug_fixed_candidates", "known_bug_fixed_candidates"},
    {"known_bug_lost_signal", "known_bug_lost_signal"},
    {"known_bug_baseline_failures", "known_bug_baseline_failures"},
    {"strict_open", "strict_open"},
    {"ny_bin", "ny_bin"},
    {"out_dir", "out_dir"},
    {"dry_run", "dry_run"},
    {"apply", "apply"},
    {"nytrix_root", "nytrix_root"},
    {"archive_dir", "archive_dir"},
    {"archive_run_dir", "archive_run_dir"},
    {"tmp_dir", "tmp_dir"},
    {"scratch_root", "scratch_root"},
    {"xdg_cache_home", "xdg_cache_home"},
    {"nytrix_cache_dir", "nytrix_cache_dir"},
    {"cache_policy_ok", "cache_policy_ok"},
    {"old_path_cache_policy_ok", "old_path_cache_policy_ok"},
    {"present_count", "present_count"},
    {"moved_count", "moved_count"},
    {"remaining_count", "remaining_count"},
    {"old_path_present_count", "old_path_present_count"},
    {"old_path_moved_count", "old_path_moved_count"},
    {"old_path_remaining_count", "old_path_remaining_count"},
    {"old_seen", "old_seen"},
    {"old_moved", "old_moved"},
    {"old_current", "old_current"},
    {"wait_writers_s", "wait_writers_s"},
    {"waited_writers_s", "waited_writers_s"},
    {"old_writer_cleared_after_wait", "old_writer_cleared_after_wait"},
    {"active_old_nytrix_output_writer",
     "active_old_nytrix_output_writer"},
    {"active_old_writer", "active_old_writer"},
    {"recent_old_cache_write", "recent_old_cache_write"},
    {"recent_old_cache_write_age_seconds",
     "recent_old_cache_write_age_seconds"},
    {"old_path_settle_recent_writes_s",
     "old_path_settle_recent_writes_s"},
    {"old_path_wait_remaining_seconds",
     "old_path_wait_remaining_seconds"},
    {"wait_remaining_s", "wait_remaining_s"},
    {"artifact_scan_enabled", "artifact_scan_enabled"},
    {"artifact_scan_dir", "artifact_scan_dir"},
    {"artifact_leak_count", "artifact_leak_count"},
    {"artifact_moved_count", "artifact_moved_count"},
    {"artifact_remaining_count", "artifact_remaining_count"},
    {"old_path_artifact_leak_count", "old_path_artifact_leak_count"},
    {"old_path_artifact_moved_count", "old_path_artifact_moved_count"},
    {"old_path_artifact_remaining_count",
     "old_path_artifact_remaining_count"},
    {"old_leaks", "old_leaks"},
    {"old_artifacts_moved", "old_artifacts_moved"},
    {"artifact_remaining", "artifact_remaining"},
    {"old_path_next_action", "old_path_next_action"},
    {"old_path_next_reason", "old_path_next_reason"},
    {"candidates", "candidates"},
    {"emitted", "emitted"},
    {"runs", "runs"},
    {"warmup", "warmup"},
    {"measurement_samples", "measurement_samples"},
    {"hotspots", "hotspots"},
    {"perf_hotspots", "perf_hotspots"},
    {"max_ratio", "max_ratio"},
    {"perf_max_ratio", "perf_max_ratio"},
    {"perf_worst_ratio", "perf_worst_ratio"},
    {"perf_worst_slowdown_percent", "perf_worst_slowdown_percent"},
    {"threshold_ratio", "threshold_ratio"},
    {"perf_threshold_ratio", "perf_threshold_ratio"},
    {"bench_limit", "bench_limit"},
    {"bench_timeout_s", "bench_timeout_s"},
    {"initial_hotspots", "initial_hotspots"},
    {"initial_timing_hotspots", "initial_timing_hotspots"},
    {"initial_max_ratio", "initial_max_ratio"},
    {"perf_initial_max_ratio", "perf_initial_max_ratio"},
    {"confirmed_candidates", "confirmed_candidates"},
    {"confirmed_hotspots", "confirmed_hotspots"},
    {"perf_confirmed_hotspots", "perf_confirmed_hotspots"},
    {"demoted_hotspots", "demoted_hotspots"},
    {"confirm_enabled", "confirm_enabled"},
    {"confirm_attempted", "confirm_attempted"},
    {"confirm_runs", "confirm_runs"},
    {"confirm_warmup", "confirm_warmup"},
    {"confirm_bench_timeout_s", "confirm_bench_timeout_s"},
    {"confirm_rc", "confirm_rc"},
    {"bench_rc", "bench_rc"},
    {"cleaned_stale", "cleaned_stale"},
    {"max_case", "max_case"},
    {"perf_max_case", "perf_max_case"},
    {"initial_max_case", "initial_max_case"},
    {"perf_initial_max_case", "perf_initial_max_case"},
    {"confirmation_report", "confirmation_report"},
    {"findings_dir", "findings_dir"},
    {"markdown", "markdown"},
    {"perf_hotspots_open", "perf_hotspots_open"},
    {"perf_watchlist_state", "perf_watchlist_state"},
    {"perf_watchlist_open", "perf_watchlist_open"},
    {"perf_watchlist_command", "perf_watchlist_command"},
    {"perf_watchlist_report", "perf_watchlist_report"},
    {"perf_watchlist_markdown", "perf_watchlist_markdown"},
    {"perf_watchlist_action", "perf_watchlist_action"},
    {"perf_watchlist_action_command", "perf_watchlist_action_command"},
    {"optimization_action", "optimization_action"},
    {"optimization_reason", "optimization_reason"},
    {"optimization_command", "optimization_command"},
    {"optimization_target_command", "optimization_target_command"},
    {"optimization_case", "optimization_case"},
    {"optimization_artifact", "optimization_artifact"},
    {"optimization_ny_source", "optimization_ny_source"},
    {"optimization_c_source", "optimization_c_source"},
    {"optimization_ratio", "optimization_ratio"},
    {"optimization_slowdown_percent", "optimization_slowdown_percent"},
    {"perf_watchlist_artifact_fresh", "perf_watchlist_artifact_fresh"},
    {"perf_watchlist_artifact_hotspots",
     "perf_watchlist_artifact_hotspots"},
    {"perf_watchlist_artifact_max_ratio",
     "perf_watchlist_artifact_max_ratio"},
    {"perf_watchlist_artifact_max_slowdown_percent",
     "perf_watchlist_artifact_max_slowdown_percent"},
    {"perf_watchlist_artifact_max_case",
     "perf_watchlist_artifact_max_case"},
    {"perf_watchlist_artifact_max_artifact",
     "perf_watchlist_artifact_max_artifact"},
    {"perf_watchlist_artifact_max_ny_source",
     "perf_watchlist_artifact_max_ny_source"},
    {"perf_watchlist_artifact_max_c_source",
     "perf_watchlist_artifact_max_c_source"},
    {"perf_watchlist_artifact_age_seconds",
     "perf_watchlist_artifact_age_seconds"},
    {"perf_watchlist_artifact_stale_after_hours",
     "perf_watchlist_artifact_stale_after_hours"},
    {"perf_watchlist_threshold_ratio", "perf_watchlist_threshold_ratio"},
    {"perf_worst_ratio", "perf_worst_ratio"},
    {"perf_worst_slowdown_percent", "perf_worst_slowdown_percent"},
    {"perf_worst_case", "perf_worst_case"},
    {"latest_full_pressure_perf_hotspots",
     "latest_full_pressure_perf_hotspots"},
    {"latest_full_pressure_perf_max_ratio",
     "latest_full_pressure_perf_max_ratio"},
    {"latest_full_pressure_perf_max_slowdown_percent",
     "latest_full_pressure_perf_max_slowdown_percent"},
    {"latest_full_pressure_perf_max_case",
     "latest_full_pressure_perf_max_case"},
    {"latest_full_pressure_perf_rows",
     "latest_full_pressure_perf_rows"},
    {"latest_full_pressure_perf_suite_current",
     "latest_full_pressure_perf_suite_current"},
    {"runtime_surface_state", "runtime_surface_state"},
    {"runtime_surface_scope", "runtime_surface_scope"},
    {"runtime_exports", "runtime_exports"},
    {"direct_runtime_refs", "direct_runtime_refs"},
    {"runtime_coverage_done", "runtime_coverage_done"},
    {"runtime_coverage_total", "runtime_coverage_total"},
    {"runtime_export_coverage_percent", "runtime_export_coverage_percent"},
    {"runtime_unreferenced_count", "runtime_unreferenced_count"},
    {"runtime_wrapper_gap_count", "runtime_wrapper_gap_count"},
    {"crt_surface_state", "crt_surface_state"},
    {"crt_surface_scope", "crt_surface_scope"},
    {"crt_behavior_state", "crt_behavior_state"},
    {"crt_behavior_scope", "crt_behavior_scope"},
    {"crt_behavior_next_action", "crt_behavior_next_action"},
    {"crt_behavior_next_reason", "crt_behavior_next_reason"},
    {"crt_behavior_next_command", "crt_behavior_next_command"},
    {"crt_runtime_exports", "crt_runtime_exports"},
    {"crt_direct_refs", "crt_direct_refs"},
    {"crt_coverage_done", "crt_coverage_done"},
    {"crt_coverage_total", "crt_coverage_total"},
    {"crt_export_coverage_percent", "crt_export_coverage_percent"},
    {"crt_unreferenced_percent", "crt_unreferenced_percent"},
    {"crt_unreferenced_count", "crt_unreferenced_count"},
    {"crt_wrapper_gap_count", "crt_wrapper_gap_count"},
    {"crt_unreferenced_family_count", "crt_unreferenced_family_count"},
    {"crt_top_unreferenced_family", "crt_top_unreferenced_family"},
    {"crt_top_unreferenced_family_count",
     "crt_top_unreferenced_family_count"},
    {"crt_next_action", "crt_next_action"},
    {"crt_next_unreferenced_family", "crt_next_unreferenced_family"},
    {"crt_next_unreferenced_count", "crt_next_unreferenced_count"},
    {"old_nytrix_test_scratch_absent", "old_nytrix_test_scratch_absent"},
    {"old_nytrix_fuzz_absent", "old_nytrix_fuzz_absent"},
    {"old_nytrix_build_cache_absent", "old_nytrix_build_cache_absent"},
    {"active_old_nytrix_output_writer_present",
     "active_old_nytrix_output_writer_present"},
    {"focused_command", "focused_command"},
    {"focused_template_command", "focused_template_command"},
    {"focused_example_command", "focused_example_command"},
    {"full_command", "full_command"},
    {"catalog_command", "catalog_command"},
    {"result_probe_command", "result_probe_command"},
    {"cockpit_command", "cockpit_command"},
    {"cockpit_result_probe_command", "cockpit_result_probe_command"},
  };
  if (!b || !summary_extra || !*summary_extra) return;
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i)
    append_json_alias_from_fragment(b, summary_extra, aliases[i].dst,
                                    aliases[i].src);
}

static char *build_native_report_json_with_top_aliases(
    const string_list_t *rows, const string_list_t *failures,
    const char *mode, const char *summary_extra, bool top_aliases) {
  str_buf_t b = {0};
  int ok = rows->count - failures->count;
  if (ok < 0) ok = 0;
  (void)sb_append(&b, "{\"rows\":");
  append_raw_json_list(&b, rows);
  (void)sb_append(&b, ",\"failures\":");
  append_raw_json_list(&b, failures);
  if (top_aliases) {
    (void)sb_appendf(&b,
                     ",\"ok\":%s,\"cases\":%d,\"ok_count\":%d,"
                     "\"failure_count\":%d",
                     failures->count == 0 ? "true" : "false",
                     rows->count, ok, failures->count);
    append_fuzz_all_top_aliases(&b, summary_extra);
  }
  (void)sb_appendf(&b,
                   ",\"summary\":{\"cases\":%d,\"ok\":%d,\"ok_count\":%d,"
                   "\"failure_count\":%d,\"engine\":\"nytrix_core\"",
                   rows->count, ok, ok, failures->count);
  if (mode && *mode) {
    (void)sb_append(&b, ",\"mode\":");
    (void)sb_append_json_str(&b, mode);
  }
  if (summary_extra && *summary_extra) (void)sb_append(&b, summary_extra);
  (void)sb_append(&b, "},\"meta\":{\"engine\":\"nytrix_core\"}}");
  return sb_take(&b);
}

static char *build_native_report_json(const string_list_t *rows,
                                      const string_list_t *failures,
                                      const char *mode,
                                      const char *summary_extra) {
  return build_native_report_json_with_top_aliases(rows, failures, mode,
                                                   summary_extra, true);
}

static int emit_native_report(char *report_json, const char *json_path,
                              const char *label, int rows, int failures) {
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
    free(report_json);
    return 2;
  }
  printf("%s rows: %d\n", label && *label ? label : "nytrix", rows);
  printf("failures: %d\n", failures);
  free(report_json);
  return failures ? 1 : 0;
}

static char *native_row_status(const char *name, const char *kind, bool ok, const char *detail_key,
                               const char *detail_value) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"name\":");
  (void)sb_append_json_str(&b, name ? name : "");
  (void)sb_append(&b, ",\"kind\":");
  (void)sb_append_json_str(&b, kind ? kind : "native");
  (void)sb_appendf(&b, ",\"ok\":%s,\"engine\":\"nytrix_core\"", ok ? "true" : "false");
  if (detail_key && *detail_key) {
    (void)sb_append_c(&b, ',');
    (void)sb_append_json_str(&b, detail_key);
    (void)sb_append_c(&b, ':');
    (void)sb_append_json_str(&b, detail_value ? detail_value : "");
  }
  (void)sb_append_c(&b, '}');
  return sb_take(&b);
}

static bool json_bool_field(const char *json, const char *key, bool fallback) {
  const char *p = json_value_after_key(json, key);
  if (!p) return fallback;
  if (strncmp(p, "true", 4) == 0) return true;
  if (strncmp(p, "false", 5) == 0) return false;
  return fallback;
}

static bool contains_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle || !*needle) return false;
  size_t n = strlen(needle);
  for (const char *p = haystack; *p; ++p) {
    size_t i = 0;
    while (i < n && p[i] &&
           tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
      ++i;
    if (i == n) return true;
  }
  return false;
}

static bool proc_result_crashed(const proc_result_t *pr) {
  if (!pr) return false;
  if (pr->rc >= 128) return true;
  return contains_ci(pr->err, "assertion") ||
         contains_ci(pr->err, "segmentation fault") ||
         contains_ci(pr->err, "segmentationfault") ||
         contains_ci(pr->err, "signal 11") ||
         contains_ci(pr->err, "sigsegv") ||
         contains_ci(pr->err, "addresssanitizer") ||
         contains_ci(pr->err, "undefinedbehavior") ||
         contains_ci(pr->err, "internal compiler error") ||
         contains_ci(pr->err, "panic") ||
         contains_ci(pr->out, "assertion") ||
         contains_ci(pr->out, "segmentation fault") ||
         contains_ci(pr->out, "segmentationfault") ||
         contains_ci(pr->out, "signal 11") ||
         contains_ci(pr->out, "sigsegv") ||
         contains_ci(pr->out, "addresssanitizer") ||
         contains_ci(pr->out, "undefinedbehavior") ||
         contains_ci(pr->out, "internal compiler error") ||
         contains_ci(pr->out, "panic");
}

static void append_proc_tail_fields(str_buf_t *b, const proc_result_t *pr) {
  (void)sb_append(b, ",\"stdout_tail\":");
  append_tail_json_str(b, pr && pr->out ? pr->out : "", 1200);
  (void)sb_append(b, ",\"stderr_tail\":");
  append_tail_json_str(b, pr && pr->err ? pr->err : "", 1200);
}

static char *make_native_proc_failure_row(const char *case_name, const char *phase,
                                          const proc_result_t *pr) {
  return make_worker_failure_row(case_name, phase, pr ? pr->rc : 1,
                                 pr ? pr->out : "", pr ? pr->err : "");
}

static char *synth_print_selftest_row(const char *root, const char *name,
                                      const char *shape, int seed,
                                      const char *source_path, const char *binary_path,
                                      const proc_result_t *run) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"name\":");
  (void)sb_append_json_str(&b, name ? name : "");
  (void)sb_append(&b, ",\"kind\":\"synth-print\",\"ok\":true,\"shape\":");
  (void)sb_append_json_str(&b, shape ? shape : "");
  (void)sb_appendf(&b, ",\"seed\":%d,\"source\":", seed);
  append_rel_json_str(&b, root, source_path ? source_path : "");
  (void)sb_append(&b, ",\"binary\":");
  append_rel_json_str(&b, root, binary_path ? binary_path : "");
  if (run) append_proc_tail_fields(&b, run);
  (void)sb_append_c(&b, '}');
  return sb_take(&b);
}

static char *synth_print_determinism_row(const char *root, bool ok,
                                         const char *first_path, const char *second_path) {
  str_buf_t b = {0};
  (void)sb_appendf(&b, "{\"name\":\"synth_print_deterministic\",\"kind\":\"synth-print\","
                       "\"ok\":%s,\"source_a\":", ok ? "true" : "false");
  append_rel_json_str(&b, root, first_path ? first_path : "");
  (void)sb_append(&b, ",\"source_b\":");
  append_rel_json_str(&b, root, second_path ? second_path : "");
  (void)sb_append_c(&b, '}');
  return sb_take(&b);
}

static bool synth_print_generate_file(const char *shape_dir, const char *shape,
                                      int seed, bool fast, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  int rc = nytrix_synth_print_c_program(f, shape_dir, "ir", "balanced", shape, seed, fast, false);
  bool ok = rc == 0;
  if (fclose(f) != 0) ok = false;
  return ok;
}
