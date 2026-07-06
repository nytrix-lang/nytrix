static int cmd_public_fuzz_all_audit(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *report_path = value_after_equals(argc, argv, 4, "--report", "");
  if (!report_path || !*report_path)
    report_path = value_after_equals(argc, argv, 4, "--input", "");
  if (!report_path || !*report_path)
    report_path = value_after_equals(argc, argv, 4, "--from", "");
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  bool strict = has_flag_after(argc, argv, 4, "--strict");
  file_buf_t report_file = {0};
  string_list_t rows = {0}, failures = {0}, seen_lanes = {0};
  if (!report_path || !*report_path || !read_file(report_path, &report_file) || !report_file.data) {
    (void)string_list_push_take(&failures, make_fuzz_failure(root, "fuzz-all-audit",
                                                            "all-run report not readable",
                                                            report_path ? report_path : ""));
  }

  int lanes = 0, required_lanes = 0, missing_logs = 0, failed_required = 0;
  int skipped_required = 0, missing_reports = 0, kernel_weak = 0, kernel_low_signal = 0;
  int quarantined_known_bugs = 0, lanes_with_quarantine = 0;
  int finding_count = 0, finding_live = 0, finding_cleared = 0, finding_missing = 0;
  int known_bug_count = 0, known_bug_reproduced = 0, known_bug_fixed_candidates = 0;
  int known_bug_lost_signal = 0, known_bug_baseline_failures = 0;
  int perf_hotspots = 0;
  double perf_max_ratio = 0.0;
  char perf_max_case[128] = {0};
  double total_elapsed_ms = 0.0, slowest_elapsed_ms = 0.0;
  char slowest_lane[128] = {0};
  double summary_failure_count = 0.0;
  if (report_file.data) {
    (void)extract_json_number(report_file.data, "failure_count", &summary_failure_count);
    if (summary_failure_count != 0.0) {
      (void)string_list_push_take(&failures, make_fuzz_failure(root, "fuzz-all-audit",
                                                              "source report has failures",
                                                              report_path));
    }
    const char *rows_json = json_top_level_value_after_key(report_file.data, "rows");
    const char *rows_end = rows_json && *rows_json == '[' ? matching_json_end(rows_json, '[', ']') : NULL;
    if (!rows_json || !rows_end) {
      (void)string_list_push_take(&failures, make_fuzz_failure(root, "fuzz-all-audit",
                                                              "source report missing rows",
                                                              report_path));
    } else {
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
        char *name = json_extract_string_range(p, obj_end + 1, "name");
        char *phase = json_extract_string_range(p, obj_end + 1, "phase");
        char *sub_report = json_extract_string_range(p, obj_end + 1, "report");
        char *stdout_log = json_extract_string_range(p, obj_end + 1, "stdout_log");
        char *stderr_log = json_extract_string_range(p, obj_end + 1, "stderr_log");
        char *command_log = json_extract_string_range(p, obj_end + 1, "command_log");
        bool ok = json_bool_range(p, obj_end + 1, "ok", false);
        bool required = json_bool_range(p, obj_end + 1, "required", false);
        bool skipped = json_bool_range(p, obj_end + 1, "skipped", false);
        double elapsed_ms = 0.0, sub_rows = -1.0, sub_failures = 0.0;
        double sub_quarantined_known_bugs = 0.0;
        double sub_finding_count = 0.0, sub_finding_live = 0.0;
        double sub_finding_cleared = 0.0, sub_finding_missing = 0.0;
        double sub_known_bug_count = 0.0, sub_known_bug_reproduced = 0.0;
        double sub_known_bug_fixed_candidates = 0.0, sub_known_bug_lost_signal = 0.0;
        double sub_known_bug_baseline_failures = 0.0, sub_perf_hotspots = 0.0;
        double sub_perf_max_ratio = 0.0;
        char *sub_perf_max_case = json_extract_string_range(p, obj_end + 1, "sub_perf_max_case");
        (void)json_number_range(p, obj_end + 1, "elapsed_ms", &elapsed_ms);
        (void)json_number_range(p, obj_end + 1, "sub_rows", &sub_rows);
        (void)json_number_range(p, obj_end + 1, "sub_failures", &sub_failures);
        (void)json_number_range(p, obj_end + 1, "sub_quarantined_known_bugs", &sub_quarantined_known_bugs);
        (void)json_number_range(p, obj_end + 1, "sub_finding_count", &sub_finding_count);
        (void)json_number_range(p, obj_end + 1, "sub_finding_live", &sub_finding_live);
        (void)json_number_range(p, obj_end + 1, "sub_finding_cleared", &sub_finding_cleared);
        (void)json_number_range(p, obj_end + 1, "sub_finding_missing", &sub_finding_missing);
        (void)json_number_range(p, obj_end + 1, "sub_known_bug_count", &sub_known_bug_count);
        (void)json_number_range(p, obj_end + 1, "sub_known_bug_reproduced", &sub_known_bug_reproduced);
        (void)json_number_range(p, obj_end + 1, "sub_known_bug_fixed_candidates", &sub_known_bug_fixed_candidates);
        (void)json_number_range(p, obj_end + 1, "sub_known_bug_lost_signal", &sub_known_bug_lost_signal);
        (void)json_number_range(p, obj_end + 1, "sub_known_bug_baseline_failures", &sub_known_bug_baseline_failures);
        (void)json_number_range(p, obj_end + 1, "sub_perf_hotspots", &sub_perf_hotspots);
        (void)json_number_range(p, obj_end + 1, "sub_perf_max_ratio", &sub_perf_max_ratio);
        ++lanes;
        if (sub_quarantined_known_bugs > 0.0) {
          quarantined_known_bugs += (int)sub_quarantined_known_bugs;
          ++lanes_with_quarantine;
        }
        if (name && *name) (void)string_list_push_unique_copy(&seen_lanes, name);
        if (required) ++required_lanes;
        if (required && !ok) ++failed_required;
        if (required && skipped) ++skipped_required;
        total_elapsed_ms += elapsed_ms;
        if (elapsed_ms > slowest_elapsed_ms) {
          slowest_elapsed_ms = elapsed_ms;
          snprintf(slowest_lane, sizeof(slowest_lane), "%s", name ? name : "");
        }
        bool report_exists =
            !sub_report || !*sub_report || path_exists_maybe_root(root, sub_report);
        if (sub_report && *sub_report && !report_exists) ++missing_reports;
        if (sub_report && *sub_report && report_exists) {
          file_buf_t sub_file = {0};
          if (read_file_maybe_root(root, sub_report, &sub_file) && sub_file.data) {
            int kb_count = 0, kb_reproduced = 0, kb_fixed = 0, kb_lost = 0, kb_baseline = 0;
            if (summarize_known_bug_report(sub_file.data, &kb_count, &kb_reproduced,
                                           &kb_fixed, &kb_lost, &kb_baseline)) {
              sub_known_bug_count = kb_count;
              sub_known_bug_reproduced = kb_reproduced;
              sub_known_bug_fixed_candidates = kb_fixed;
              sub_known_bug_lost_signal = kb_lost;
              sub_known_bug_baseline_failures = kb_baseline;
            }
            double fv = 0.0;
            if (summary_number_from_report(sub_file.data, "finding_count", &fv))
              sub_finding_count = fv;
            if (summary_number_from_report(sub_file.data, "live", &fv))
              sub_finding_live = fv;
            if (summary_number_from_report(sub_file.data, "cleared", &fv))
              sub_finding_cleared = fv;
            if (summary_number_from_report(sub_file.data, "missing", &fv))
              sub_finding_missing = fv;
            int hotspots = 0;
            double max_ratio = 0.0;
            char max_case[128] = {0};
            if (summarize_perf_triage_report(sub_file.data, &hotspots, &max_ratio,
                                             max_case, sizeof(max_case))) {
              sub_perf_hotspots = hotspots;
              sub_perf_max_ratio = max_ratio;
              free(sub_perf_max_case);
              sub_perf_max_case = strdup(max_case);
            }
            free(sub_file.data);
          }
        }
        finding_count += (int)sub_finding_count;
        finding_live += (int)sub_finding_live;
        finding_cleared += (int)sub_finding_cleared;
        finding_missing += (int)sub_finding_missing;
        known_bug_count += (int)sub_known_bug_count;
        known_bug_reproduced += (int)sub_known_bug_reproduced;
        known_bug_fixed_candidates += (int)sub_known_bug_fixed_candidates;
        known_bug_lost_signal += (int)sub_known_bug_lost_signal;
        known_bug_baseline_failures += (int)sub_known_bug_baseline_failures;
        perf_hotspots += (int)sub_perf_hotspots;
        if (sub_perf_max_ratio > perf_max_ratio) {
          perf_max_ratio = sub_perf_max_ratio;
          snprintf(perf_max_case, sizeof(perf_max_case), "%s",
                   sub_perf_max_case ? sub_perf_max_case : "");
        }
        bool logs_ok = true;
        if (stdout_log && *stdout_log && !path_exists_maybe_root(root, stdout_log))
          logs_ok = false;
        if (stderr_log && *stderr_log && !path_exists_maybe_root(root, stderr_log))
          logs_ok = false;
        if (command_log && *command_log && !path_exists_maybe_root(root, command_log))
          logs_ok = false;
        if (!logs_ok) ++missing_logs;
        if (required && skipped) {
          (void)string_list_push_take(&failures, make_fuzz_failure(root, name ? name : "lane",
                                                                  "required lane skipped",
                                                                  report_path));
        }
        if (required && !ok) {
          (void)string_list_push_take(&failures, make_fuzz_failure(root, name ? name : "lane",
                                                                  "required lane failed",
                                                                  report_path));
        }
        if (sub_report && *sub_report && !report_exists) {
          (void)string_list_push_take(&failures, make_fuzz_failure(root, name ? name : "lane",
                                                                  "subreport missing",
                                                                  sub_report));
        }
        if (!logs_ok && strict) {
          (void)string_list_push_take(&failures, make_fuzz_failure(root, name ? name : "lane",
                                                                  "lane log missing",
                                                                  report_path));
        }
        if (name && strcmp(name, "kernels_smoke") == 0 && sub_report && *sub_report && report_exists) {
          file_buf_t kernel_report = {0};
          if (read_file(sub_report, &kernel_report) && kernel_report.data) {
            double weak = 0.0, low = 0.0;
            (void)extract_json_number(kernel_report.data, "weak_results", &weak);
            (void)extract_json_number(kernel_report.data, "low_signal_results", &low);
            kernel_weak = (int)weak;
            kernel_low_signal = (int)low;
            if (kernel_weak > 0 || kernel_low_signal > 0) {
              (void)string_list_push_take(&failures, make_fuzz_failure(root, "kernels_smoke",
                                                                      "kernel gate reported weak signal",
                                                                      sub_report));
            }
            if (!strstr(kernel_report.data, "\"strict_signal\":true") && strict) {
              (void)string_list_push_take(&failures, make_fuzz_failure(root, "kernels_smoke",
                                                                      "kernel gate was not strict-signal",
                                                                      sub_report));
            }
            free(kernel_report.data);
          }
        }
        (void)string_list_push_take(&rows,
                                    make_fuzz_all_audit_lane_row(root, name, phase, ok, required,
                                                                 skipped, elapsed_ms, sub_rows,
                                                                 sub_failures,
                                                                 sub_quarantined_known_bugs,
                                                                 sub_finding_count,
                                                                 sub_finding_live,
                                                                 sub_finding_cleared,
                                                                 sub_finding_missing,
                                                                 sub_known_bug_count,
                                                                 sub_known_bug_reproduced,
                                                                 sub_known_bug_fixed_candidates,
                                                                 sub_known_bug_lost_signal,
                                                                 sub_known_bug_baseline_failures,
                                                                 sub_perf_hotspots,
                                                                 sub_perf_max_ratio,
                                                                 sub_perf_max_case, sub_report,
                                                                 report_exists, logs_ok));
        free(name);
        free(phase);
        free(sub_report);
        free(sub_perf_max_case);
        free(stdout_log);
        free(stderr_log);
        free(command_log);
        p = obj_end + 1;
      }
    }
  }

  const char *must_have[] = {
    "corpus_prepare", "workspace_audit", "harness_all", "libs_import",
    "kernels_smoke", "compiler_std_audit", "compiler_findings",
    "compiler_known_bugs",
    "prove_lab", "perf_triage", "snippets_mixed", "frontend_corpus"
  };
  int missing_core = 0;
  for (size_t i = 0; i < sizeof(must_have) / sizeof(must_have[0]); ++i) {
    if (!fuzz_all_audit_lane_seen(&seen_lanes, must_have[i])) {
      ++missing_core;
      (void)string_list_push_take(&failures, make_fuzz_failure(root, must_have[i],
                                                              "core lane missing",
                                                              report_path ? report_path : ""));
    }
  }

  char *workspace = make_fuzz_path(root, "");
  char *workspace_rel = rel_path_dup(root, workspace ? workspace : "");
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"source_report\":");
  append_rel_json_str(&extra, root, report_path ? report_path : "");
  (void)sb_appendf(&extra,
                   ",\"lanes\":%d,\"required_lanes\":%d,\"failed_required\":%d,"
                   "\"skipped_required\":%d,\"missing_core_lanes\":%d,"
                   "\"missing_reports\":%d,\"missing_logs\":%d,"
                   "\"kernel_weak_results\":%d,\"kernel_low_signal_results\":%d,"
                   "\"quarantined_known_bugs\":%d,\"lanes_with_quarantine\":%d,"
                   "\"finding_count\":%d,\"finding_live\":%d,"
                   "\"finding_cleared\":%d,\"finding_missing\":%d,"
                   "\"known_bug_count\":%d,\"known_bug_reproduced\":%d,"
                   "\"known_bug_fixed_candidates\":%d,"
                   "\"known_bug_lost_signal\":%d,"
                   "\"known_bug_baseline_failures\":%d,"
                   "\"perf_hotspots\":%d,\"perf_max_ratio\":%.4f,"
                   "\"perf_max_case\":",
                   lanes, required_lanes, failed_required, skipped_required,
                   missing_core, missing_reports, missing_logs,
                   kernel_weak, kernel_low_signal, quarantined_known_bugs,
                   lanes_with_quarantine, finding_count, finding_live,
                   finding_cleared, finding_missing, known_bug_count,
                   known_bug_reproduced, known_bug_fixed_candidates,
                   known_bug_lost_signal, known_bug_baseline_failures,
                   perf_hotspots, perf_max_ratio);
  (void)sb_append_json_str(&extra, perf_max_case);
  (void)sb_appendf(&extra,
                   ",\"total_lane_elapsed_ms\":%.2f,\"slowest_lane\":",
                   total_elapsed_ms);
  (void)sb_append_json_str(&extra, slowest_lane);
  (void)sb_appendf(&extra, ",\"slowest_lane_ms\":%.2f,\"strict\":%s",
                   slowest_elapsed_ms, strict ? "true" : "false");
  char *report_json = build_fuzz_report_json(&rows, &failures,
                                             workspace_rel ? workspace_rel : "",
                                             &extra, "nynth fuzz all audit");
  int rc = emit_rows_failures_report(report_json, json_path, workspace_rel,
                                     rows.count, failures.count);
  free(workspace_rel);
  free(workspace);
  free(extra.data);
  free(report_file.data);
  string_list_free(&seen_lanes);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static void findings_add_lane_row(string_list_t *findings, const char *name,
                                  const char *reason, double sub_failures,
                                  bool skipped, const char *report) {
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"lane-attention\",\"name\":");
  (void)sb_append_json_str(&row, name ? name : "");
  (void)sb_append(&row, ",\"reason\":");
  (void)sb_append_json_str(&row, reason ? reason : "");
  (void)sb_appendf(&row, ",\"sub_failures\":%.0f,\"skipped\":%s,\"report\":",
                   sub_failures, skipped ? "true" : "false");
  (void)sb_append_json_str(&row, report ? report : "");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(findings, sb_take(&row));
}

static void findings_add_quarantine_row(string_list_t *findings, const char *name,
                                        double quarantined, const char *report) {
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"known-bug-quarantine\",\"name\":");
  (void)sb_append_json_str(&row, name ? name : "");
  (void)sb_appendf(&row, ",\"quarantined_known_bugs\":%.0f,\"report\":", quarantined);
  (void)sb_append_json_str(&row, report ? report : "");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(findings, sb_take(&row));
}

static void findings_collect_known_bug_rows(const char *root, const char *sub_json,
                                            string_list_t *findings,
                                            bool include_fixed) {
  const char *rows_json = json_top_level_value_after_key(sub_json, "rows");
  const char *rows_end = rows_json && *rows_json == '[' ? matching_json_end(rows_json, '[', ']') : NULL;
  if (!rows_json || !rows_end) return;
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
    char *name = json_extract_string_range(p, obj_end + 1, "name");
    char *status = json_extract_string_range(p, obj_end + 1, "status");
    char *source = json_extract_string_range(p, obj_end + 1, "source");
    char *expected_signal = json_extract_string_range(p, obj_end + 1, "expected_signal");
    char *bad_flavor = json_extract_string_range(p, obj_end + 1, "bad_flavor");
    bool reproduced = json_bool_range(p, obj_end + 1, "known_bug_reproduced", false);
    bool fixed = json_bool_range(p, obj_end + 1, "fixed_candidate", false);
    bool lost = json_bool_range(p, obj_end + 1, "lost_signal", false);
    bool baseline = json_bool_range(p, obj_end + 1, "baseline_failed", false);
    bool actionable = reproduced || lost || baseline || !fixed;
    if (include_fixed || actionable) {
      str_buf_t row = {0};
      (void)sb_append(&row, "{\"kind\":\"known-bug\",\"name\":");
      (void)sb_append_json_str(&row, name ? name : "");
      (void)sb_append(&row, ",\"status\":");
      (void)sb_append_json_str(&row, status ? status : "");
      (void)sb_append(&row, ",\"source\":");
      append_rel_json_str(&row, root, source ? source : "");
      (void)sb_append(&row, ",\"expected_signal\":");
      (void)sb_append_json_str(&row, expected_signal ? expected_signal : "");
      (void)sb_append(&row, ",\"bad_flavor\":");
      (void)sb_append_json_str(&row, bad_flavor ? bad_flavor : "");
      (void)sb_appendf(&row,
                       ",\"known_bug_reproduced\":%s,\"fixed_candidate\":%s,"
                       "\"lost_signal\":%s,\"baseline_failed\":%s,"
                       "\"engine\":\"nynth_core\"}",
                       reproduced ? "true" : "false", fixed ? "true" : "false",
                       lost ? "true" : "false", baseline ? "true" : "false");
      (void)string_list_push_take(findings, sb_take(&row));
    }
    free(name);
    free(status);
    free(source);
    free(expected_signal);
    free(bad_flavor);
    p = obj_end + 1;
  }
}

static void findings_collect_compiler_finding_rows(const char *root, const char *sub_json,
                                                   string_list_t *findings) {
  const char *rows_json = json_top_level_value_after_key(sub_json, "rows");
  const char *rows_end = rows_json && *rows_json == '[' ? matching_json_end(rows_json, '[', ']') : NULL;
  if (!rows_json || !rows_end) return;
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
    bool ok = json_bool_range(p, obj_end + 1, "ok", false);
    char *status = json_extract_string_range(p, obj_end + 1, "status");
    bool cleared = status && strcmp(status, "cleared") == 0;
    if (!ok || !cleared) {
      char *name = json_extract_string_range(p, obj_end + 1, "name");
      char *title = json_extract_string_range(p, obj_end + 1, "title");
      char *mode = json_extract_string_range(p, obj_end + 1, "mode");
      char *source = json_extract_string_range(p, obj_end + 1, "source");
      char *target = json_extract_string_range(p, obj_end + 1, "target");
      char *expected_signal = json_extract_string_range(p, obj_end + 1, "expected_signal");
      double rc = 0.0, elapsed_ms = 0.0;
      (void)json_number_range(p, obj_end + 1, "rc", &rc);
      (void)json_number_range(p, obj_end + 1, "elapsed_ms", &elapsed_ms);
      bool timed_out = json_bool_range(p, obj_end + 1, "timed_out", false);
      bool crashed = json_bool_range(p, obj_end + 1, "crashed", false);
      str_buf_t row = {0};
      (void)sb_append(&row, "{\"kind\":\"compiler-finding\",\"name\":");
      (void)sb_append_json_str(&row, name ? name : "");
      (void)sb_append(&row, ",\"status\":");
      (void)sb_append_json_str(&row, status ? status : "");
      (void)sb_append(&row, ",\"title\":");
      (void)sb_append_json_str(&row, title ? title : "");
      (void)sb_append(&row, ",\"mode\":");
      (void)sb_append_json_str(&row, mode ? mode : "");
      (void)sb_append(&row, ",\"expected_signal\":");
      (void)sb_append_json_str(&row, expected_signal ? expected_signal : "");
      (void)sb_append(&row, ",\"source\":");
      append_rel_json_str(&row, root, source ? source : "");
      (void)sb_append(&row, ",\"target\":");
      append_rel_json_str(&row, root, target ? target : "");
      (void)sb_appendf(&row,
                       ",\"ok\":%s,\"rc\":%.0f,\"timed_out\":%s,"
                       "\"crashed\":%s,\"elapsed_ms\":%.2f,"
                       "\"engine\":\"nynth_core\"}",
                       ok ? "true" : "false", rc,
                       timed_out ? "true" : "false",
                       crashed ? "true" : "false", elapsed_ms);
      (void)string_list_push_take(findings, sb_take(&row));
      free(name);
      free(title);
      free(mode);
      free(source);
      free(target);
      free(expected_signal);
    }
    free(status);
    p = obj_end + 1;
  }
}

static void findings_collect_perf_rows(const char *root, const char *sub_json,
                                       string_list_t *findings) {
  const char *rows_json = json_top_level_value_after_key(sub_json, "rows");
  const char *rows_end = rows_json && *rows_json == '[' ? matching_json_end(rows_json, '[', ']') : NULL;
  if (!rows_json || !rows_end) return;
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
    bool hot = json_bool_range(p, obj_end + 1, "hot", false);
    double ratio = 0.0, rank = 0.0, runs = 0.0, warmup = 0.0, samples = 0.0;
    double c_elapsed_ns = 0.0, ny_elapsed_ns = 0.0;
    double slowdown_percent = 0.0;
    (void)json_number_range(p, obj_end + 1, "ratio", &ratio);
    (void)json_number_range(p, obj_end + 1, "rank", &rank);
    (void)json_number_range(p, obj_end + 1, "runs", &runs);
    (void)json_number_range(p, obj_end + 1, "warmup", &warmup);
    (void)json_number_range(p, obj_end + 1, "samples", &samples);
    (void)json_number_range(p, obj_end + 1, "c_elapsed_ns", &c_elapsed_ns);
    (void)json_number_range(p, obj_end + 1, "ny_elapsed_ns", &ny_elapsed_ns);
    if (!json_number_range(p, obj_end + 1, "slowdown_percent",
                           &slowdown_percent))
      slowdown_percent = ratio > 0.0 ? (ratio - 1.0) * 100.0 : 0.0;
    if (hot) {
      double c_elapsed_ms = c_elapsed_ns / 1000000.0;
      double ny_elapsed_ms = ny_elapsed_ns / 1000000.0;
      double delta_elapsed_ns = ny_elapsed_ns - c_elapsed_ns;
      double delta_elapsed_ms = delta_elapsed_ns / 1000000.0;
      char *case_name = json_extract_string_range(p, obj_end + 1, "case");
      char *artifact = json_extract_string_range(p, obj_end + 1, "artifact");
      char *ny_source = json_extract_string_range(p, obj_end + 1, "ny_source");
      char *c_source = json_extract_string_range(p, obj_end + 1, "c_source");
      str_buf_t row = {0};
      (void)sb_append(&row, "{\"kind\":\"perf-hotspot\",\"case\":");
      (void)sb_append_json_str(&row, case_name ? case_name : "");
      (void)sb_appendf(&row,
                       ",\"rank\":%.0f,\"ratio\":%.4f,\"runs\":%.0f,"
                       "\"warmup\":%.0f,\"samples\":%.0f,"
                       "\"c_elapsed_ns\":%.0f,\"ny_elapsed_ns\":%.0f,"
                       "\"c_elapsed_ms\":%.6f,\"ny_elapsed_ms\":%.6f,"
                       "\"delta_elapsed_ns\":%.0f,\"delta_elapsed_ms\":%.6f,"
                       "\"slowdown_percent\":%.2f,"
                       "\"hot\":%s,\"hotspot\":%s,\"artifact\":",
                       rank, ratio, runs, warmup, samples, c_elapsed_ns,
                       ny_elapsed_ns, c_elapsed_ms, ny_elapsed_ms,
                       delta_elapsed_ns, delta_elapsed_ms, slowdown_percent,
                       hot ? "true" : "false", hot ? "true" : "false");
      append_rel_json_str(&row, root, artifact ? artifact : "");
      (void)sb_append(&row, ",\"ny_source\":");
      append_rel_json_str(&row, root, ny_source ? ny_source : "");
      (void)sb_append(&row, ",\"c_source\":");
      append_rel_json_str(&row, root, c_source ? c_source : "");
      (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
      (void)string_list_push_take(findings, sb_take(&row));
      free(case_name);
      free(artifact);
      free(ny_source);
      free(c_source);
    }
    p = obj_end + 1;
  }
}

static void md_append_inline(str_buf_t *b, const char *s) {
  if (!b) return;
  if (!s || !*s) {
    (void)sb_append(b, "-");
    return;
  }
  for (const char *p = s; *p; ++p) {
    char c = *p;
    if (c == '\r') continue;
    if (c == '\n' || c == '\t') c = ' ';
    (void)sb_append_c(b, c);
  }
}

static void md_append_code(str_buf_t *b, const char *s) {
  if (!b) return;
  (void)sb_append_c(b, '`');
  if (s && *s) md_append_inline(b, s);
  else (void)sb_append_c(b, '-');
  (void)sb_append_c(b, '`');
}

static void md_append_labeled_path(str_buf_t *b, const char *label, const char *path) {
  if (!b || !path || !*path) return;
  (void)sb_append(b, "; ");
  (void)sb_append(b, label ? label : "path");
  (void)sb_append_c(b, ' ');
  md_append_code(b, path);
}

static void md_append_path_segment(str_buf_t *b, bool *wrote,
                                   const char *label, const char *path) {
  if (!b || !path || !*path) return;
  (void)sb_append(b, wrote && *wrote ? "; " : " ");
  (void)sb_append(b, label ? label : "path");
  (void)sb_append_c(b, ' ');
  md_append_code(b, path);
  if (wrote) *wrote = true;
}

static void findings_kind_counts(const string_list_t *findings,
                                 int *lane_attention,
                                 int *compiler_findings,
                                 int *known_bugs,
                                 int *perf_hotspots,
                                 int *quarantines) {
  if (lane_attention) *lane_attention = 0;
  if (compiler_findings) *compiler_findings = 0;
  if (known_bugs) *known_bugs = 0;
  if (perf_hotspots) *perf_hotspots = 0;
  if (quarantines) *quarantines = 0;
  if (!findings) return;
  for (int i = 0; i < findings->count; ++i) {
    char *kind = json_string_or_empty(findings->items[i], "kind");
    if (kind && strcmp(kind, "lane-attention") == 0) {
      if (lane_attention) ++*lane_attention;
    } else if (kind && strcmp(kind, "compiler-finding") == 0) {
      if (compiler_findings) ++*compiler_findings;
    } else if (kind && strcmp(kind, "known-bug") == 0) {
      if (known_bugs) ++*known_bugs;
    } else if (kind && strcmp(kind, "perf-hotspot") == 0) {
      if (perf_hotspots) ++*perf_hotspots;
    } else if (kind && strcmp(kind, "known-bug-quarantine") == 0) {
      if (quarantines) ++*quarantines;
    }
    free(kind);
  }
}

static void findings_markdown_append_lane_rows(str_buf_t *md,
                                               const string_list_t *findings) {
  if (!md || !findings) return;
  (void)sb_append(md, "## Lane Attention\n\n");
  int printed = 0;
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    char *kind = json_string_or_empty(row, "kind");
    if (!kind || strcmp(kind, "lane-attention") != 0) {
      free(kind);
      continue;
    }
    char *name = json_string_or_empty(row, "name");
    char *reason = json_string_or_empty(row, "reason");
    char *report = json_string_or_empty(row, "report");
    double sub_failures = 0.0;
    (void)extract_json_number(row, "sub_failures", &sub_failures);
    (void)sb_append(md, "- ");
    md_append_code(md, name);
    (void)sb_append(md, ": ");
    md_append_inline(md, reason);
    if (sub_failures > 0.0) (void)sb_appendf(md, "; subfailures %.0f", sub_failures);
    md_append_labeled_path(md, "report", report);
    (void)sb_append(md, "\n");
    ++printed;
    free(kind);
    free(name);
    free(reason);
    free(report);
  }
  if (!printed) (void)sb_append(md, "No lane-level failures or missing subreports.\n");
  (void)sb_append(md, "\n");
}

static void findings_markdown_append_compiler_rows(str_buf_t *md,
                                                   const string_list_t *findings) {
  if (!md || !findings) return;
  (void)sb_append(md, "## Compiler Findings\n\n");
  int printed = 0;
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    char *kind = json_string_or_empty(row, "kind");
    if (!kind || strcmp(kind, "compiler-finding") != 0) {
      free(kind);
      continue;
    }
    char *name = json_string_or_empty(row, "name");
    char *status = json_string_or_empty(row, "status");
    char *signal = json_string_or_empty(row, "expected_signal");
    char *source = json_string_or_empty(row, "source");
    char *target = json_string_or_empty(row, "target");
    double rc = 0.0, elapsed_ms = 0.0;
    (void)extract_json_number(row, "rc", &rc);
    (void)extract_json_number(row, "elapsed_ms", &elapsed_ms);
    (void)sb_append(md, "- ");
    md_append_code(md, name);
    (void)sb_append(md, ": ");
    md_append_inline(md, status);
    if (signal && *signal) {
      (void)sb_append(md, "; expected ");
      md_append_code(md, signal);
    }
    (void)sb_appendf(md, "; rc %.0f; %.2f ms", rc, elapsed_ms);
    md_append_labeled_path(md, "source", source);
    md_append_labeled_path(md, "target", target);
    (void)sb_append(md, "\n");
    ++printed;
    free(kind);
    free(name);
    free(status);
    free(signal);
    free(source);
    free(target);
  }
  if (!printed) (void)sb_append(md, "No live compiler watchlist seeds.\n");
  (void)sb_append(md, "\n");
}

static void findings_markdown_append_known_rows(str_buf_t *md,
                                                const string_list_t *findings) {
  if (!md || !findings) return;
  (void)sb_append(md, "## Known-Bug Replay Rows\n\n");
  int printed = 0;
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    char *kind = json_string_or_empty(row, "kind");
    if (!kind || strcmp(kind, "known-bug") != 0) {
      free(kind);
      continue;
    }
    char *name = json_string_or_empty(row, "name");
    char *status = json_string_or_empty(row, "status");
    char *signal = json_string_or_empty(row, "expected_signal");
    char *flavor = json_string_or_empty(row, "bad_flavor");
    char *source = json_string_or_empty(row, "source");
    (void)sb_append(md, "- ");
    md_append_code(md, name);
    (void)sb_append(md, ": ");
    md_append_inline(md, status);
    if (signal && *signal) {
      (void)sb_append(md, "; expected ");
      md_append_code(md, signal);
    } else if (flavor && *flavor) {
      (void)sb_append(md, "; flavor ");
      md_append_code(md, flavor);
    }
    md_append_labeled_path(md, "source", source);
    (void)sb_append(md, "\n");
    ++printed;
    free(kind);
    free(name);
    free(status);
    free(signal);
    free(flavor);
    free(source);
  }
  if (!printed) (void)sb_append(md, "No saved known-bug replay rows need action.\n");
  (void)sb_append(md, "\n");
}

static void findings_markdown_append_perf_rows(str_buf_t *md,
                                               const string_list_t *findings) {
  if (!md || !findings) return;
  (void)sb_append(md, "## C-vs-Ny Perf Hotspots\n\n");
  int printed = 0;
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    char *kind = json_string_or_empty(row, "kind");
    if (!kind || strcmp(kind, "perf-hotspot") != 0) {
      free(kind);
      continue;
    }
    char *case_name = json_string_or_empty(row, "case");
    char *artifact = json_string_or_empty(row, "artifact");
    char *ny_source = json_string_or_empty(row, "ny_source");
    char *c_source = json_string_or_empty(row, "c_source");
    double rank = 0.0, ratio = 0.0, runs = 0.0, warmup = 0.0;
    double c_elapsed_ns = 0.0, ny_elapsed_ns = 0.0;
    (void)extract_json_number(row, "rank", &rank);
    (void)extract_json_number(row, "ratio", &ratio);
    (void)extract_json_number(row, "runs", &runs);
    (void)extract_json_number(row, "warmup", &warmup);
    (void)extract_json_number(row, "c_elapsed_ns", &c_elapsed_ns);
    (void)extract_json_number(row, "ny_elapsed_ns", &ny_elapsed_ns);
    (void)sb_appendf(md, "- #%.0f ", rank);
    md_append_code(md, case_name);
    (void)sb_appendf(md, ": %.4fx Ny/C; %.0f runs, %.0f warmup; C %.0f ns; Ny %.0f ns",
                     ratio, runs, warmup, c_elapsed_ns, ny_elapsed_ns);
    md_append_labeled_path(md, "artifact", artifact);
    md_append_labeled_path(md, "ny", ny_source);
    md_append_labeled_path(md, "c", c_source);
    (void)sb_append(md, "\n");
    ++printed;
    free(kind);
    free(case_name);
    free(artifact);
    free(ny_source);
    free(c_source);
  }
  if (!printed) (void)sb_append(md, "No perf hotspot rows above the current triage threshold.\n");
  (void)sb_append(md, "\n");
}

static void findings_markdown_append_quarantine_rows(str_buf_t *md,
                                                     const string_list_t *findings) {
  if (!md || !findings) return;
  (void)sb_append(md, "## Quarantine\n\n");
  int printed = 0;
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    char *kind = json_string_or_empty(row, "kind");
    if (!kind || strcmp(kind, "known-bug-quarantine") != 0) {
      free(kind);
      continue;
    }
    char *name = json_string_or_empty(row, "name");
    char *report = json_string_or_empty(row, "report");
    double count = 0.0;
    (void)extract_json_number(row, "quarantined_known_bugs", &count);
    (void)sb_append(md, "- ");
    md_append_code(md, name);
    (void)sb_appendf(md, ": %.0f quarantined known-bug rows", count);
    md_append_labeled_path(md, "report", report);
    (void)sb_append(md, "\n");
    ++printed;
    free(kind);
    free(name);
    free(report);
  }
  if (!printed) (void)sb_append(md, "No known-bug quarantine rows.\n");
  (void)sb_append(md, "\n");
}

static bool write_fuzz_all_findings_markdown(const char *root,
                                             const char *markdown_path,
                                             const char *source_report,
                                             const char *source_json,
                                             const string_list_t *findings) {
  if (!markdown_path || !*markdown_path) return true;
  double failure_count = 0.0, lanes = 0.0, ok = 0.0;
  double finding_count = 0.0, finding_live = 0.0, finding_cleared = 0.0;
  double finding_missing = 0.0, known_count = 0.0, known_reproduced = 0.0;
  double known_fixed = 0.0, known_lost = 0.0, known_baseline = 0.0;
  double quarantined = 0.0, quarantine_lanes = 0.0;
  double perf_hotspots = 0.0, perf_max_ratio = 0.0;
  if (source_json) {
    (void)summary_number_from_report(source_json, "failure_count", &failure_count);
    (void)summary_number_from_report(source_json, "cases", &lanes);
    (void)summary_number_from_report(source_json, "ok", &ok);
    (void)summary_number_from_report(source_json, "finding_count", &finding_count);
    (void)summary_number_from_report(source_json, "finding_live", &finding_live);
    (void)summary_number_from_report(source_json, "finding_cleared", &finding_cleared);
    (void)summary_number_from_report(source_json, "finding_missing", &finding_missing);
    (void)summary_number_from_report(source_json, "known_bug_count", &known_count);
    (void)summary_number_from_report(source_json, "known_bug_reproduced", &known_reproduced);
    (void)summary_number_from_report(source_json, "known_bug_fixed_candidates", &known_fixed);
    (void)summary_number_from_report(source_json, "known_bug_lost_signal", &known_lost);
    (void)summary_number_from_report(source_json, "known_bug_baseline_failures", &known_baseline);
    (void)summary_number_from_report(source_json, "quarantined_known_bugs", &quarantined);
    (void)summary_number_from_report(source_json, "lanes_with_quarantine", &quarantine_lanes);
    (void)summary_number_from_report(source_json, "perf_hotspots", &perf_hotspots);
    (void)summary_number_from_report(source_json, "perf_max_ratio", &perf_max_ratio);
  }
  char *perf_case = source_json ? json_string_or_empty(source_json, "perf_max_case") : strdup("");
  char *source_rel = rel_path_dup(root ? root : "", source_report ? source_report : "");
  int lane_rows = 0, compiler_rows = 0, known_rows = 0, perf_rows = 0, quarantine_rows = 0;
  findings_kind_counts(findings, &lane_rows, &compiler_rows, &known_rows, &perf_rows,
                       &quarantine_rows);
  time_t now = time(NULL);
  struct tm tm_now;
  char stamp[64] = {0};
  if (localtime_r(&now, &tm_now))
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %z", &tm_now);

  str_buf_t md = {0};
  (void)sb_append(&md, "# Nynth Findings Digest\n\n");
  if (stamp[0]) {
    (void)sb_append(&md, "Generated: ");
    md_append_code(&md, stamp);
    (void)sb_append(&md, "\n\n");
  }
  (void)sb_append(&md, "Source report: ");
  md_append_code(&md, source_rel);
  (void)sb_append(&md, "\n\n");
  (void)sb_append(&md, "## TLDR\n\n");
  (void)sb_appendf(&md, "- Lanes: %.0f/%.0f ok, %.0f source failures.\n",
                   ok, lanes, failure_count);
  (void)sb_appendf(&md, "- Active finding watchlist: %.0f checked, %.0f live, %.0f cleared, %.0f missing.\n",
                   finding_count, finding_live, finding_cleared, finding_missing);
  (void)sb_appendf(&md, "- Known-bug replay: %.0f checked, %.0f reproduced, %.0f fixed candidates, %.0f lost signal, %.0f baseline failures.\n",
                   known_count, known_reproduced, known_fixed, known_lost, known_baseline);
  (void)sb_appendf(&md, "- Perf: %.0f hotspots, worst %.4fx Ny/C",
                   perf_hotspots, perf_max_ratio);
  if (perf_case && *perf_case) {
    (void)sb_append(&md, " at ");
    md_append_code(&md, perf_case);
  }
  (void)sb_append(&md, ".\n");
  (void)sb_appendf(&md, "- Quarantine: %.0f rows across %.0f lanes.\n",
                   quarantined, quarantine_lanes);
  (void)sb_appendf(&md, "- Digest rows: %d lane, %d compiler, %d known-bug, %d perf, %d quarantine.\n\n",
                   lane_rows, compiler_rows, known_rows, perf_rows, quarantine_rows);
  if (known_fixed > 0.0 && known_rows == 0)
    (void)sb_append(&md, "Fixed known-bug replay rows are summarized above and suppressed from the actionable digest by default. Pass `--include-fixed-known-bugs` to emit them.\n\n");
  if (quarantined > 0.0 && quarantine_rows == 0)
    (void)sb_append(&md, "Known-bug quarantine rows are summarized above and suppressed from the actionable digest by default. Pass `--include-quarantine` to emit them.\n\n");
  if (!lane_rows && !compiler_rows && !known_rows && !perf_rows && !quarantine_rows)
    (void)sb_append(&md, "No actionable rows were emitted by this findings pass.\n\n");

  findings_markdown_append_lane_rows(&md, findings);
  findings_markdown_append_compiler_rows(&md, findings);
  findings_markdown_append_known_rows(&md, findings);
  findings_markdown_append_perf_rows(&md, findings);
  findings_markdown_append_quarantine_rows(&md, findings);
  (void)sb_append(&md, "## Next\n\n");
  (void)sb_append(&md, "```bash\n");
  (void)sb_append(&md,
                  "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                  "./build/nynth fuzz all audit --report ");
  (void)sb_append(&md, source_rel && *source_rel ? source_rel : "<report.json>");
  (void)sb_append(&md, " --strict\n");
  (void)sb_append(&md,
                  "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                  "./build/nynth fuzz all findings --report ");
  (void)sb_append(&md, source_rel && *source_rel ? source_rel : "<report.json>");
  (void)sb_append(&md, " --json <findings.json> --markdown <findings.md>\n");
  (void)sb_append(&md, "```\n");

  bool ok_write = md.data && write_file_text(markdown_path, md.data);
  free(md.data);
  free(perf_case);
  free(source_rel);
  return ok_write;
}

static void print_fuzz_all_findings_human(const char *report_path, const char *report_json,
                                          const string_list_t *findings) {
  double failure_count = 0.0, lanes = 0.0, ok = 0.0;
  double finding_count = 0.0, finding_live = 0.0, finding_cleared = 0.0;
  double finding_missing = 0.0;
  double known_count = 0.0, known_reproduced = 0.0, known_fixed = 0.0;
  double known_lost = 0.0, known_baseline = 0.0;
  double quarantined = 0.0, quarantine_lanes = 0.0;
  double perf_hotspots = 0.0, perf_max_ratio = 0.0;
  (void)summary_number_from_report(report_json, "failure_count", &failure_count);
  (void)summary_number_from_report(report_json, "cases", &lanes);
  (void)summary_number_from_report(report_json, "ok", &ok);
  (void)summary_number_from_report(report_json, "finding_count", &finding_count);
  (void)summary_number_from_report(report_json, "finding_live", &finding_live);
  (void)summary_number_from_report(report_json, "finding_cleared", &finding_cleared);
  (void)summary_number_from_report(report_json, "finding_missing", &finding_missing);
  (void)summary_number_from_report(report_json, "known_bug_count", &known_count);
  (void)summary_number_from_report(report_json, "known_bug_reproduced", &known_reproduced);
  (void)summary_number_from_report(report_json, "known_bug_fixed_candidates", &known_fixed);
  (void)summary_number_from_report(report_json, "known_bug_lost_signal", &known_lost);
  (void)summary_number_from_report(report_json, "known_bug_baseline_failures", &known_baseline);
  (void)summary_number_from_report(report_json, "quarantined_known_bugs", &quarantined);
  (void)summary_number_from_report(report_json, "lanes_with_quarantine", &quarantine_lanes);
  (void)summary_number_from_report(report_json, "perf_hotspots", &perf_hotspots);
  (void)summary_number_from_report(report_json, "perf_max_ratio", &perf_max_ratio);
  char *perf_case = json_string_or_empty(report_json, "perf_max_case");

  int derived_known_count = 0, derived_known_reproduced = 0, derived_known_fixed = 0;
  int derived_known_lost = 0, derived_known_baseline = 0;
  int derived_quarantine_lanes = 0, derived_perf_hotspots = 0;
  double derived_quarantined = 0.0, derived_perf_max_ratio = 0.0;
  char derived_perf_case[128] = {0};
  for (int i = 0; i < findings->count; ++i) {
    char *kind = json_string_or_empty(findings->items[i], "kind");
    if (kind && strcmp(kind, "known-bug") == 0) {
      ++derived_known_count;
      char *status = json_string_or_empty(findings->items[i], "status");
      if (status && strcmp(status, "known_bug_reproduced") == 0) ++derived_known_reproduced;
      else if (status && strcmp(status, "fixed_candidate") == 0) ++derived_known_fixed;
      else if (status && strcmp(status, "baseline_failed") == 0) ++derived_known_baseline;
      else if (status && *status) ++derived_known_lost;
      free(status);
    } else if (kind && strcmp(kind, "known-bug-quarantine") == 0) {
      double count = 0.0;
      (void)extract_json_number(findings->items[i], "quarantined_known_bugs", &count);
      derived_quarantined += count;
      ++derived_quarantine_lanes;
    } else if (kind && strcmp(kind, "perf-hotspot") == 0) {
      ++derived_perf_hotspots;
      double ratio = 0.0;
      if (extract_json_number(findings->items[i], "ratio", &ratio) &&
          ratio > derived_perf_max_ratio) {
        derived_perf_max_ratio = ratio;
        char *case_name = json_string_or_empty(findings->items[i], "case");
        snprintf(derived_perf_case, sizeof(derived_perf_case), "%s",
                 case_name ? case_name : "");
        free(case_name);
      }
    }
    free(kind);
  }
  if (known_count <= 0.0 && derived_known_count > 0) known_count = derived_known_count;
  if (known_reproduced <= 0.0 && derived_known_reproduced > 0) known_reproduced = derived_known_reproduced;
  if (known_fixed <= 0.0 && derived_known_fixed > 0) known_fixed = derived_known_fixed;
  if (known_lost <= 0.0 && derived_known_lost > 0) known_lost = derived_known_lost;
  if (known_baseline <= 0.0 && derived_known_baseline > 0) known_baseline = derived_known_baseline;
  if (quarantined <= 0.0 && derived_quarantined > 0.0) quarantined = derived_quarantined;
  if (quarantine_lanes <= 0.0 && derived_quarantine_lanes > 0) quarantine_lanes = derived_quarantine_lanes;
  if (perf_hotspots <= 0.0 && derived_perf_hotspots > 0) perf_hotspots = derived_perf_hotspots;
  if (perf_max_ratio <= 0.0 && derived_perf_max_ratio > 0.0) {
    perf_max_ratio = derived_perf_max_ratio;
    free(perf_case);
    perf_case = strdup(derived_perf_case);
  }

  printf("all-fuzz findings: %s\n", report_path ? report_path : "");
  printf("status: %.0f/%.0f lanes ok, %.0f failures\n", ok, lanes, failure_count);
  printf("active findings: %.0f checked, %.0f live, %.0f cleared, %.0f missing\n",
         finding_count, finding_live, finding_cleared, finding_missing);
  printf("known bugs: %.0f checked, %.0f reproduced, %.0f fixed candidates, %.0f lost signal, %.0f baseline failures\n",
         known_count, known_reproduced, known_fixed, known_lost, known_baseline);
  printf("quarantine: %.0f known-bug rows across %.0f lanes\n", quarantined, quarantine_lanes);
  printf("perf: %.0f hotspots", perf_hotspots);
  if (perf_max_ratio > 0.0)
    printf(", worst %s %.4fx Ny/C", perf_case && *perf_case ? perf_case : "unknown", perf_max_ratio);
  printf("\n");

  int lane_attention = 0, compiler_finding_rows = 0;
  int known_rows = 0, perf_rows = 0, quarantine_rows = 0;
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    if (strstr(row, "\"kind\":\"lane-attention\"")) ++lane_attention;
    else if (strstr(row, "\"kind\":\"compiler-finding\"")) ++compiler_finding_rows;
    else if (strstr(row, "\"kind\":\"known-bug\"")) ++known_rows;
    else if (strstr(row, "\"kind\":\"perf-hotspot\"")) ++perf_rows;
    else if (strstr(row, "\"kind\":\"known-bug-quarantine\"")) ++quarantine_rows;
  }
  printf("lane attention: %d\n", lane_attention);
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    if (!strstr(row, "\"kind\":\"lane-attention\"")) continue;
    char *name = json_string_or_empty(row, "name");
    char *reason = json_string_or_empty(row, "reason");
    char *subreport = json_string_or_empty(row, "report");
    double sub_failures = 0.0;
    (void)extract_json_number(row, "sub_failures", &sub_failures);
    printf("  - %s: %s", name && *name ? name : "lane", reason && *reason ? reason : "attention");
    if (sub_failures > 0.0) printf(" (subfailures %.0f)", sub_failures);
    if (subreport && *subreport) printf(" [%s]", subreport);
    printf("\n");
    free(name);
    free(reason);
    free(subreport);
  }

  printf("live compiler findings: %d\n", compiler_finding_rows);
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    if (!strstr(row, "\"kind\":\"compiler-finding\"")) continue;
    char *name = json_string_or_empty(row, "name");
    char *status = json_string_or_empty(row, "status");
    char *signal = json_string_or_empty(row, "expected_signal");
    char *source = json_string_or_empty(row, "source");
    double rc = 0.0;
    (void)extract_json_number(row, "rc", &rc);
    printf("  - %s: %s", name && *name ? name : "finding",
           status && *status ? status : "");
    if (signal && *signal) printf(" (%s)", signal);
    printf(" rc %.0f", rc);
    if (source && *source) printf(" [%s]", source);
    printf("\n");
    free(name);
    free(status);
    free(signal);
    free(source);
  }

  printf("known-bug replay rows: %d\n", known_rows);
  if (known_fixed > 0.0 && known_rows == 0)
    printf("  fixed known-bug rows are summarized but suppressed; use --include-fixed-known-bugs to emit them\n");
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    if (!strstr(row, "\"kind\":\"known-bug\"")) continue;
    char *name = json_string_or_empty(row, "name");
    char *status = json_string_or_empty(row, "status");
    char *signal = json_string_or_empty(row, "expected_signal");
    char *flavor = json_string_or_empty(row, "bad_flavor");
    printf("  - %s: %s", name && *name ? name : "known-bug", status && *status ? status : "");
    if (signal && *signal) printf(" (%s)", signal);
    else if (flavor && *flavor) printf(" (%s)", flavor);
    printf("\n");
    free(name);
    free(status);
    free(signal);
    free(flavor);
  }

  printf("perf hotspots: %d\n", perf_rows);
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    if (!strstr(row, "\"kind\":\"perf-hotspot\"")) continue;
    char *case_name = json_string_or_empty(row, "case");
    char *artifact = json_string_or_empty(row, "artifact");
    double ratio = 0.0, rank = 0.0, runs = 0.0, warmup = 0.0;
    (void)extract_json_number(row, "ratio", &ratio);
    (void)extract_json_number(row, "rank", &rank);
    (void)extract_json_number(row, "runs", &runs);
    (void)extract_json_number(row, "warmup", &warmup);
    printf("  - #%.0f %s: %.4fx Ny/C", rank, case_name && *case_name ? case_name : "case", ratio);
    if (runs > 0.0) printf(" (%.0f runs, %.0f warmup)", runs, warmup);
    if (artifact && *artifact) printf(" [%s]", artifact);
    printf("\n");
    free(case_name);
    free(artifact);
  }

  printf("known-bug quarantine lanes: %d\n", quarantine_rows);
  if (quarantined > 0.0 && quarantine_rows == 0)
    printf("  quarantined known-bug rows are summarized but suppressed; use --include-quarantine to emit them\n");
  for (int i = 0; i < findings->count; ++i) {
    const char *row = findings->items[i];
    if (!strstr(row, "\"kind\":\"known-bug-quarantine\"")) continue;
    char *name = json_string_or_empty(row, "name");
    double count = 0.0;
    (void)extract_json_number(row, "quarantined_known_bugs", &count);
    printf("  - %s: %.0f quarantined rows\n", name && *name ? name : "lane", count);
    free(name);
  }
  free(perf_case);
}

static int cmd_public_fuzz_all_findings(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *report_path = value_after_equals(argc, argv, 4, "--report", "");
  if (!report_path || !*report_path)
    report_path = value_after_equals(argc, argv, 4, "--input", "");
  if (!report_path || !*report_path)
    report_path = value_after_equals(argc, argv, 4, "--from", "");
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *markdown_path = value_after_equals(argc, argv, 4, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after_equals(argc, argv, 4, "--md", "");
  bool include_fixed_known_bugs =
      has_flag_after(argc, argv, 4, "--include-fixed-known-bugs") ||
      has_flag_after(argc, argv, 4, "--include-fixed") ||
      has_flag_after(argc, argv, 4, "--include-cleared") ||
      has_flag_after(argc, argv, 4, "--include-solved");
  bool include_quarantine_rows =
      has_flag_after(argc, argv, 4, "--include-quarantine") ||
      has_flag_after(argc, argv, 4, "--include-quarantined") ||
      has_flag_after(argc, argv, 4, "--include-quarantine-rows");
  file_buf_t report_file = {0};
  string_list_t findings = {0}, failures = {0};
  if (!report_path || !*report_path || !read_file(report_path, &report_file) || !report_file.data) {
    (void)string_list_push_take(&failures, make_fuzz_failure(root, "fuzz-all-findings",
                                                            "all-run report not readable",
                                                            report_path ? report_path : ""));
  }
  if (report_file.data) {
    const char *rows_json = json_top_level_value_after_key(report_file.data, "rows");
    const char *rows_end = rows_json && *rows_json == '[' ? matching_json_end(rows_json, '[', ']') : NULL;
    if (!rows_json || !rows_end) {
      (void)string_list_push_take(&failures, make_fuzz_failure(root, "fuzz-all-findings",
                                                              "source report missing rows",
                                                              report_path));
    } else {
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
        char *name = json_extract_string_range(p, obj_end + 1, "name");
        char *sub_report = json_extract_string_range(p, obj_end + 1, "report");
        bool ok = json_bool_range(p, obj_end + 1, "ok", false);
        bool skipped = json_bool_range(p, obj_end + 1, "skipped", false);
        double sub_failures = 0.0, quarantined = 0.0;
        (void)json_number_range(p, obj_end + 1, "sub_failures", &sub_failures);
        (void)json_number_range(p, obj_end + 1, "sub_quarantined_known_bugs", &quarantined);
        if (!ok || sub_failures > 0.0)
          findings_add_lane_row(&findings, name, ok ? "subreport failures" : "lane failed",
                                sub_failures, skipped, sub_report);
        if (quarantined > 0.0 && include_quarantine_rows)
          findings_add_quarantine_row(&findings, name, quarantined, sub_report);
        if (sub_report && *sub_report && path_exists_file(sub_report)) {
          file_buf_t sub = {0};
          if (read_file(sub_report, &sub) && sub.data) {
            if (name && strcmp(name, "compiler_findings") == 0)
              findings_collect_compiler_finding_rows(root, sub.data, &findings);
            else if (name && strcmp(name, "compiler_known_bugs") == 0)
              findings_collect_known_bug_rows(root, sub.data, &findings,
                                              include_fixed_known_bugs);
            else if (name && strcmp(name, "perf_triage") == 0)
              findings_collect_perf_rows(root, sub.data, &findings);
            free(sub.data);
          }
        } else if (sub_report && *sub_report) {
          findings_add_lane_row(&findings, name, "subreport missing", sub_failures, skipped, sub_report);
        }
        free(name);
        free(sub_report);
        p = obj_end + 1;
      }
    }
  }

  double failure_count = 0.0, lanes = 0.0, ok = 0.0;
  double finding_count = 0.0, finding_live = 0.0, finding_cleared = 0.0;
  double finding_missing = 0.0;
  double known_count = 0.0, known_reproduced = 0.0, known_fixed = 0.0;
  double known_lost = 0.0, known_baseline = 0.0, quarantined = 0.0;
  double quarantine_lanes = 0.0, perf_hotspots = 0.0, perf_max_ratio = 0.0;
  if (report_file.data) {
    (void)summary_number_from_report(report_file.data, "failure_count", &failure_count);
    (void)summary_number_from_report(report_file.data, "cases", &lanes);
    (void)summary_number_from_report(report_file.data, "ok", &ok);
    (void)summary_number_from_report(report_file.data, "finding_count", &finding_count);
    (void)summary_number_from_report(report_file.data, "finding_live", &finding_live);
    (void)summary_number_from_report(report_file.data, "finding_cleared", &finding_cleared);
    (void)summary_number_from_report(report_file.data, "finding_missing", &finding_missing);
    (void)summary_number_from_report(report_file.data, "known_bug_count", &known_count);
    (void)summary_number_from_report(report_file.data, "known_bug_reproduced", &known_reproduced);
    (void)summary_number_from_report(report_file.data, "known_bug_fixed_candidates", &known_fixed);
    (void)summary_number_from_report(report_file.data, "known_bug_lost_signal", &known_lost);
    (void)summary_number_from_report(report_file.data, "known_bug_baseline_failures", &known_baseline);
    (void)summary_number_from_report(report_file.data, "quarantined_known_bugs", &quarantined);
    (void)summary_number_from_report(report_file.data, "lanes_with_quarantine", &quarantine_lanes);
    (void)summary_number_from_report(report_file.data, "perf_hotspots", &perf_hotspots);
    (void)summary_number_from_report(report_file.data, "perf_max_ratio", &perf_max_ratio);
  }
  char *perf_case = report_file.data ? json_string_or_empty(report_file.data, "perf_max_case") : strdup("");
  int derived_known_count = 0, derived_known_reproduced = 0, derived_known_fixed = 0;
  int derived_known_lost = 0, derived_known_baseline = 0;
  int derived_quarantine_lanes = 0, derived_perf_hotspots = 0;
  double derived_quarantined = 0.0, derived_perf_max_ratio = 0.0;
  char derived_perf_case[128] = {0};
  for (int i = 0; i < findings.count; ++i) {
    char *kind = json_string_or_empty(findings.items[i], "kind");
    if (kind && strcmp(kind, "known-bug") == 0) {
      ++derived_known_count;
      char *status = json_string_or_empty(findings.items[i], "status");
      if (status && strcmp(status, "known_bug_reproduced") == 0) ++derived_known_reproduced;
      else if (status && strcmp(status, "fixed_candidate") == 0) ++derived_known_fixed;
      else if (status && strcmp(status, "baseline_failed") == 0) ++derived_known_baseline;
      else if (status && *status) ++derived_known_lost;
      free(status);
    } else if (kind && strcmp(kind, "known-bug-quarantine") == 0) {
      double count = 0.0;
      (void)extract_json_number(findings.items[i], "quarantined_known_bugs", &count);
      derived_quarantined += count;
      ++derived_quarantine_lanes;
    } else if (kind && strcmp(kind, "perf-hotspot") == 0) {
      bool hot = json_bool_range(findings.items[i],
                                 findings.items[i] + strlen(findings.items[i]),
                                 "hot", false);
      if (hot) ++derived_perf_hotspots;
      double ratio = 0.0;
      if (extract_json_number(findings.items[i], "ratio", &ratio) &&
          ratio > derived_perf_max_ratio) {
        derived_perf_max_ratio = ratio;
        char *case_name = json_string_or_empty(findings.items[i], "case");
        snprintf(derived_perf_case, sizeof(derived_perf_case), "%s",
                 case_name ? case_name : "");
        free(case_name);
      }
    }
    free(kind);
  }
  if (known_count <= 0.0 && derived_known_count > 0) known_count = derived_known_count;
  if (known_reproduced <= 0.0 && derived_known_reproduced > 0) known_reproduced = derived_known_reproduced;
  if (known_fixed <= 0.0 && derived_known_fixed > 0) known_fixed = derived_known_fixed;
  if (known_lost <= 0.0 && derived_known_lost > 0) known_lost = derived_known_lost;
  if (known_baseline <= 0.0 && derived_known_baseline > 0) known_baseline = derived_known_baseline;
  if (quarantined <= 0.0 && derived_quarantined > 0.0) quarantined = derived_quarantined;
  if (quarantine_lanes <= 0.0 && derived_quarantine_lanes > 0) quarantine_lanes = derived_quarantine_lanes;
  if (perf_hotspots <= 0.0 && derived_perf_hotspots > 0) perf_hotspots = derived_perf_hotspots;
  if (perf_max_ratio <= 0.0 && derived_perf_max_ratio > 0.0) {
    perf_max_ratio = derived_perf_max_ratio;
    free(perf_case);
    perf_case = strdup(derived_perf_case);
  }
  double suppressed_fixed_known_bug_rows = 0.0;
  if (!include_fixed_known_bugs && known_fixed > (double)derived_known_fixed)
    suppressed_fixed_known_bug_rows = known_fixed - (double)derived_known_fixed;
  double suppressed_quarantine_rows = 0.0;
  if (!include_quarantine_rows && quarantined > derived_quarantined)
    suppressed_quarantine_rows = quarantined - derived_quarantined;
  if (markdown_path && *markdown_path &&
      !write_fuzz_all_findings_markdown(root, markdown_path, report_path,
                                        report_file.data, &findings)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-findings",
                                                  "markdown findings write failed",
                                                  markdown_path));
  }
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"source_report\":");
  append_rel_json_str(&extra, root, report_path ? report_path : "");
  if (markdown_path && *markdown_path) {
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, root, markdown_path);
  }
  (void)sb_appendf(&extra,
                   ",\"source_failures\":%.0f,\"source_lanes\":%.0f,"
                   "\"source_ok_lanes\":%.0f,"
                   "\"finding_count\":%.0f,\"finding_live\":%.0f,"
                   "\"finding_cleared\":%.0f,\"finding_missing\":%.0f,"
                   "\"known_bug_count\":%.0f,\"known_bug_reproduced\":%.0f,"
                   "\"known_bug_fixed_candidates\":%.0f,"
                   "\"known_bug_fixed_rows_suppressed\":%.0f,"
                   "\"include_fixed_known_bugs\":%s,"
                   "\"known_bug_lost_signal\":%.0f,"
                   "\"known_bug_baseline_failures\":%.0f,"
                   "\"quarantined_known_bugs\":%.0f,"
                   "\"quarantine_rows_suppressed\":%.0f,"
                   "\"include_quarantine_rows\":%s,"
                   "\"lanes_with_quarantine\":%.0f,"
                   "\"perf_hotspots\":%.0f,\"perf_max_ratio\":%.4f,"
                   "\"perf_max_case\":",
                   failure_count, lanes, ok,
                   finding_count, finding_live, finding_cleared, finding_missing,
                   known_count, known_reproduced,
                   known_fixed, suppressed_fixed_known_bug_rows,
                   include_fixed_known_bugs ? "true" : "false",
                   known_lost, known_baseline,
                   quarantined, suppressed_quarantine_rows,
                   include_quarantine_rows ? "true" : "false",
                   quarantine_lanes, perf_hotspots, perf_max_ratio);
  (void)sb_append_json_str(&extra, perf_case ? perf_case : "");
  char *report_json = build_native_report_json(&findings, &failures,
                                               "fuzz-all-findings", extra.data);
  int rc = 0;
  if (json_path && *json_path) {
    rc = emit_native_report(report_json, json_path, "all fuzz findings",
                            findings.count, failures.count);
  } else {
    if (report_file.data)
      print_fuzz_all_findings_human(report_path, report_file.data, &findings);
    if (failures.count > 0) {
      printf("findings errors: %d\n", failures.count);
      rc = 1;
    }
    free(report_json);
  }
  free(perf_case);
  free(extra.data);
  free(report_file.data);
  string_list_free(&findings);
  string_list_free(&failures);
  return rc;
}

static bool report_summary_mode_is(const char *json, const char *want) {
  char *mode = summary_string_from_report(json, "mode");
  bool ok = mode && want && strcmp(mode, want) == 0;
  free(mode);
  return ok;
}

static bool report_is_fuzz_all(const char *json) {
  return report_summary_mode_is(json, "fuzz-all");
}

static bool report_is_fuzz_sanitizers(const char *json) {
  return report_summary_mode_is(json, "fuzz-sanitizers");
}

static bool fuzz_all_history_row_ran(const string_list_t *rows, const char *name) {
  if (!rows || !name || !*name) return false;
  for (int i = 0; i < rows->count; ++i) {
    const char *row = rows->items[i];
    if (!row) continue;
    char *row_name = json_string_or_empty(row, "name");
    bool matches = row_name && strcmp(row_name, name) == 0;
    bool ok = json_bool_range(row, row + strlen(row), "ok", false);
    bool skipped = json_bool_range(row, row + strlen(row), "skipped", false);
    free(row_name);
    if (matches && ok && !skipped) return true;
  }
  return false;
}

static bool fuzz_all_history_has_major_disabled_skip(const string_list_t *rows) {
  if (!rows) return false;
  for (int i = 0; i < rows->count; ++i) {
    const char *row = rows->items[i];
    if (!row) continue;
    bool skipped = json_bool_range(row, row + strlen(row), "skipped", false);
    if (!skipped) continue;
    char *phase = json_string_or_empty(row, "phase");
    char *reason = json_string_or_empty(row, "reason");
    bool advisory =
        phase && (strcmp(phase, "nytrix") == 0 || strcmp(phase, "sanitizer") == 0);
    bool disabled = reason && strncmp(reason, "disabled by --no-", 17) == 0;
    bool budget_short = reason && strstr(reason, "budget too short");
    bool major = !advisory && (disabled || budget_short);
    free(phase);
    free(reason);
    if (major) return true;
  }
  return false;
}

static bool fuzz_all_history_row_productive_evidence(const char *row) {
  if (!row) return false;
  const char *end = row + strlen(row);
  bool ok = json_bool_range(row, end, "ok", false);
  bool skipped = json_bool_range(row, end, "skipped", false);
  bool dry_run = json_bool_range(row, end, "dry_run", false);
  if (!ok || skipped || dry_run) return false;
  char *name = json_string_or_empty(row, "name");
  char *phase = json_string_or_empty(row, "phase");
  bool setup_phase = phase && strcmp(phase, "setup") == 0;
  bool setup_name =
      name && (strcmp(name, "corpus_prepare") == 0 ||
               strcmp(name, "workspace_audit") == 0);
  free(phase);
  free(name);
  return !setup_phase && !setup_name;
}

static bool fuzz_all_history_has_productive_evidence_rows(
    const string_list_t *rows) {
  if (!rows) return false;
  for (int i = 0; i < rows->count; ++i) {
    if (fuzz_all_history_row_productive_evidence(rows->items[i]))
      return true;
  }
  return false;
}

static bool fuzz_all_history_report_has_attention_summary(const char *json) {
  if (!json) return false;
  static const char *keys[] = {
    "failure_count",
    "finding_live",
    "finding_missing",
    "known_bug_reproduced",
    "known_bug_lost_signal",
    "known_bug_baseline_failures",
    "perf_hotspots"
  };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    double value = 0.0;
    if (summary_number_from_report(json, keys[i], &value) && value > 0.0)
      return true;
  }
  return false;
}

static bool fuzz_all_history_report_counts_as_evidence(
    const char *json, const string_list_t *rows) {
  return fuzz_all_history_report_has_attention_summary(json) ||
         fuzz_all_history_has_productive_evidence_rows(rows);
}

static bool fuzz_all_infer_full_pressure_report(const char *json,
                                                const string_list_t *rows,
                                                double duration_s,
                                                double budget_s) {
  bool explicit_full_pressure = false;
  if (summary_bool_from_report(json, "full_pressure", &explicit_full_pressure))
    return explicit_full_pressure;
  bool smoke = false;
  if (summary_bool_from_report(json, "smoke", &smoke) && smoke) return false;
  double effective_budget_s = budget_s > 0.0 ? budget_s : duration_s;
  if (effective_budget_s < 1800.0) return false;
  double synth_cases = 0.0, gc_budget_s = 0.0, perf_limit = 0.0;
  double harness_limit = -1.0, libs_limit = -1.0, kernel_limit = -1.0;
  (void)summary_number_from_report(json, "synth_cases", &synth_cases);
  (void)summary_number_from_report(json, "gc_budget_s", &gc_budget_s);
  (void)summary_number_from_report(json, "perf_limit", &perf_limit);
  (void)summary_number_from_report(json, "harness_limit_per_target", &harness_limit);
  (void)summary_number_from_report(json, "libs_import_limit", &libs_limit);
  (void)summary_number_from_report(json, "kernel_run_limit", &kernel_limit);
  bool pressure_shape =
      synth_cases >= 4.0 && gc_budget_s >= 600.0 && perf_limit >= 6.0 &&
      harness_limit == 0.0 && libs_limit == 0.0 && kernel_limit == 0.0;
  if (!pressure_shape) return false;
  if (fuzz_all_history_has_major_disabled_skip(rows)) return false;
  static const char *required[] = {
    "compiler_std_audit", "compiler_findings", "compiler_known_bugs",
    "perf_triage", "prove_lab", "synth_mixed", "synth_ir", "synth_stress",
    "synth_pure", "campaign_core", "gc_soak", "afl_compiler", "afl_parsers"
  };
  for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
    if (!fuzz_all_history_row_ran(rows, required[i])) return false;
  }
  return true;
}

static bool write_fuzz_all_history_markdown(const char *root,
                                            const char *markdown_path,
                                            const char *history_json_path,
                                            const fuzz_all_history_summary_t *summary,
                                            const string_list_t *rows,
                                            const char *target_thread_years,
                                            const char *hours_per_run,
                                            const char *threads,
                                            const char *profile) {
  if (!markdown_path || !*markdown_path) return true;
  if (!summary || !rows) return false;
  time_t now = time(NULL);
  struct tm tm_now;
  char stamp[64] = {0};
  if (localtime_r(&now, &tm_now))
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %z", &tm_now);
  char *scan_rel = rel_path_dup(root ? root : "", summary->scan_dir);
  char *history_rel = rel_path_dup(root ? root : "",
                                   history_json_path && *history_json_path ?
                                       history_json_path : "build/fuzz/all/history.json");
  char *history_dir = path_parent_dup(history_rel && *history_rel ?
                                          history_rel : "build/fuzz/all/history.json",
                                      "build/fuzz/all");
  char *worklist_json = path_child_dup(history_dir, "worklist.json");
  char *worklist_md = path_child_dup(history_dir, "worklist.md");
  char *coverage_json = path_child_dup(history_dir, "coverage.json");
  char *plan_json = path_child_dup(history_dir, "plan.json");
  char *status_json = path_child_dup(history_dir, "status.json");
  char *status_md = path_child_dup(history_dir, "status.md");

  str_buf_t md = {0};
  (void)sb_append(&md, "# Nynth Campaign History\n\n");
  if (stamp[0]) {
    (void)sb_append(&md, "Generated: ");
    md_append_code(&md, stamp);
    (void)sb_append(&md, "\n\n");
  }
  (void)sb_append(&md, "Scan dir: ");
  md_append_code(&md, scan_rel);
  (void)sb_append(&md, "\n\n");

  (void)sb_append(&md, "## TLDR\n\n");
  (void)sb_append(&md,
                  "- Scope: historical archive; current blockers live in `worklist.md` and `status.md`.\n");
  (void)sb_appendf(&md, "- Reports: %d emitted of %d matched; %d ok, %d failed, %d attention.\n",
                   summary->emitted_reports, summary->reports,
                   summary->ok_reports, summary->failed_reports,
                   summary->attention_reports);
  if (summary->ignored_no_evidence_reports > 0) {
    (void)sb_appendf(&md,
                     "- Ignored no-evidence attempts: %d clean setup-only report%s.\n",
                     summary->ignored_no_evidence_reports,
                     summary->ignored_no_evidence_reports == 1 ? "" : "s");
  }
     (void)sb_appendf(&md,
                      "- Compute: %.6f thread-years (%.2f thread-hours); %.2f credited wall-hours; max %.0f threads.\n",
                      summary->total_thread_s / (3600.0 * 24.0 * 365.0),
                      summary->total_thread_s / 3600.0,
                      summary->total_effective_budget_s / 3600.0,
                      summary->max_threads);
  if (summary->first_report[0]) {
    (void)sb_appendf(&md,
                     "- Calendar: %.2f days first-to-latest evidence; %.2f days since first evidence",
                     summary->campaign_calendar_span_days,
                     summary->campaign_calendar_age_days);
    (void)sb_append(&md, "; first ");
    md_append_code(&md, summary->first_report);
    (void)sb_append(&md, ".\n");
  }
     (void)sb_appendf(&md,
                      "- Full-pressure: %d reports, %d ok, %d attention, %.2f thread-hours.\n",
                   summary->full_pressure_reports,
                   summary->full_pressure_ok_reports,
                   summary->full_pressure_attention_reports,
                   summary->full_pressure_thread_s / 3600.0);
  (void)sb_appendf(&md,
                   "- Workload: %.0f subcase rows, %.0f sub-failures; %.0f/%.0f lanes ok.\n",
                   summary->total_sub_rows, summary->total_sub_failures,
                   summary->total_ok_lanes, summary->total_lanes);
  (void)sb_appendf(&md,
                   "- Historical bugs: %.0f live findings, %.0f missing, %.0f reproduced known bugs, %.0f lost signals, %.0f baseline failures.\n",
                   summary->finding_live_total, summary->finding_missing_total,
                   summary->known_reproduced_total, summary->known_lost_total,
                   summary->known_baseline_total);
  (void)sb_appendf(&md, "- Historical perf: %.0f hotspot rows; worst %.4fx Ny/C",
                   summary->perf_hotspots_total, summary->perf_max_ratio);
  if (summary->worst_perf_case[0]) {
    (void)sb_append(&md, " at ");
    md_append_code(&md, summary->worst_perf_case);
  }
  (void)sb_append(&md, ".\n");
  if (summary->latest_report[0]) {
    (void)sb_append(&md, "- Latest: ");
    md_append_code(&md, summary->latest_report);
    (void)sb_append(&md, "; ");
    (void)sb_appendf(&md,
                     "clean %s, failures %.0f, sub-failures %.0f, known %.0f, perf %.0f",
                     (!summary->latest_report_attention && summary->latest_report_ok) ? "`true`" : "`false`",
                     summary->latest_failure_count, summary->latest_sub_failures,
                     summary->latest_known_reproduced,
                     summary->latest_perf_hotspots);
    if (summary->latest_perf_max_ratio > 0.0) {
      (void)sb_appendf(&md, ", worst %.4fx", summary->latest_perf_max_ratio);
      if (summary->latest_perf_case[0]) {
        (void)sb_append(&md, " ");
        md_append_code(&md, summary->latest_perf_case);
      }
    }
    (void)sb_append(&md, ".\n");
  }
  if (summary->latest_full_pressure_report[0]) {
    (void)sb_append(&md, "- Latest full-pressure: ");
    md_append_code(&md, summary->latest_full_pressure_report);
    (void)sb_append(&md, "; ");
    (void)sb_appendf(&md,
                     "clean %s, failures %.0f, sub-failures %.0f, known %.0f, perf %.0f",
                     (!summary->latest_full_pressure_attention &&
                      summary->latest_full_pressure_ok) ? "`true`" : "`false`",
                     summary->latest_full_pressure_failure_count,
                     summary->latest_full_pressure_sub_failures,
                     summary->latest_full_pressure_known_reproduced,
                     summary->latest_full_pressure_perf_hotspots);
    if (summary->latest_full_pressure_perf_max_ratio > 0.0) {
      (void)sb_appendf(&md, ", worst %.4fx",
                       summary->latest_full_pressure_perf_max_ratio);
      if (summary->latest_full_pressure_perf_case[0]) {
        (void)sb_append(&md, " ");
        md_append_code(&md, summary->latest_full_pressure_perf_case);
      }
    }
    (void)sb_append(&md, ".\n");
  } else {
    (void)sb_append(&md, "- Latest full-pressure: none recorded yet.\n");
  }
  if (summary->worst_report[0]) {
    (void)sb_append(&md, "- Highest attention: ");
    md_append_code(&md, summary->worst_report);
    (void)sb_append(&md, ".\n");
  }
  (void)sb_append(&md, "\n");

  (void)sb_append(&md, "## Attention\n\n");
  int attention_printed = 0;
  int attention_skip = summary->attention_reports > 5 ?
      summary->attention_reports - 5 : 0;
  for (int i = 0; i < rows->count; ++i) {
    const char *row = rows->items[i];
    bool attention = json_bool_range(row, row + strlen(row), "attention", false);
    if (!attention) continue;
    if (attention_skip > 0) {
      --attention_skip;
      continue;
    }
    char *report = json_string_or_empty(row, "report");
    char *perf_case = json_string_or_empty(row, "perf_max_case");
    double failures = 0.0, live = 0.0, missing = 0.0, known = 0.0;
    double lost = 0.0, baseline = 0.0, hotspots = 0.0, ratio = 0.0;
    (void)extract_json_number(row, "failure_count", &failures);
    (void)extract_json_number(row, "finding_live", &live);
    (void)extract_json_number(row, "finding_missing", &missing);
    (void)extract_json_number(row, "known_bug_reproduced", &known);
    (void)extract_json_number(row, "known_bug_lost_signal", &lost);
    (void)extract_json_number(row, "known_bug_baseline_failures", &baseline);
    (void)extract_json_number(row, "perf_hotspots", &hotspots);
    (void)extract_json_number(row, "perf_max_ratio", &ratio);
    (void)sb_append(&md, "- ");
    md_append_code(&md, report);
    (void)sb_appendf(&md,
                     ": fail %.0f, live %.0f, missing %.0f, known %.0f, lost %.0f, baseline %.0f, perf %.0f",
                     failures, live, missing, known, lost, baseline, hotspots);
    if (ratio > 0.0) {
      (void)sb_appendf(&md, ", worst %.4fx", ratio);
      if (perf_case && *perf_case) {
        (void)sb_append(&md, " ");
        md_append_code(&md, perf_case);
      }
    }
    (void)sb_append(&md, "\n");
    ++attention_printed;
    free(report);
    free(perf_case);
  }
  if (!attention_printed)
    (void)sb_append(&md, "No emitted all-run reports need attention.\n");
  else if (summary->attention_reports > attention_printed)
    (void)sb_appendf(&md, "Showing latest %d of %d attention reports.\n",
                     attention_printed, summary->attention_reports);
  (void)sb_append(&md, "\n");

  (void)sb_append(&md, "## Next\n\n```bash\n");
  (void)sb_append(&md, "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth fuzz all worklist --history ");
  (void)sb_append(&md, history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json");
  (void)sb_append(&md, " --json ");
  (void)sb_append(&md, worklist_json && *worklist_json ? worklist_json : "build/fuzz/all/worklist.json");
  (void)sb_append(&md, " --markdown ");
  (void)sb_append(&md, worklist_md && *worklist_md ? worklist_md : "build/fuzz/all/worklist.md");
  (void)sb_append(&md, "\n");
  (void)sb_append(&md, "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth fuzz all status --refresh --strict --allow-full-pressure-remediation --dir ");
  (void)sb_append(&md, scan_rel && *scan_rel ? scan_rel : "build/fuzz/all");
  (void)sb_append(&md, " --history ");
  (void)sb_append(&md, history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json");
  (void)sb_append(&md, " --worklist ");
  (void)sb_append(&md, worklist_json && *worklist_json ? worklist_json : "build/fuzz/all/worklist.json");
  (void)sb_append(&md, " --coverage ");
  (void)sb_append(&md, coverage_json && *coverage_json ? coverage_json : "build/fuzz/all/coverage.json");
  (void)sb_append(&md, " --plan ");
  (void)sb_append(&md, plan_json && *plan_json ? plan_json : "build/fuzz/all/plan.json");
  (void)sb_append(&md, " --target-thread-years ");
  (void)sb_append(&md, target_thread_years && *target_thread_years ? target_thread_years : "10");
  (void)sb_append(&md, " --hours ");
  (void)sb_append(&md, hours_per_run && *hours_per_run ? hours_per_run : "8");
  (void)sb_append(&md, " --threads ");
  (void)sb_append(&md, threads && *threads ? threads : NYNTH_DEFAULT_FUZZ_THREADS);
  (void)sb_append(&md, " --profile ");
  (void)sb_append(&md, profile && *profile ? profile : "insane");
  (void)sb_append(&md, " --json ");
  (void)sb_append(&md, status_json && *status_json ? status_json : "build/fuzz/all/status.json");
  (void)sb_append(&md, " --markdown ");
  (void)sb_append(&md, status_md && *status_md ? status_md : "build/fuzz/all/status.md");
  (void)sb_append(&md, "\n");
  (void)sb_append(&md, "```\n");

  bool ok = md.data && write_file_text(markdown_path, md.data);
  free(md.data);
  free(scan_rel);
  free(history_rel);
  free(history_dir);
  free(worklist_json);
  free(worklist_md);
  free(coverage_json);
  free(plan_json);
  free(status_json);
  free(status_md);
  return ok;
}

static bool fuzz_all_history_skip_path(const char *path) {
  const char *p = path ? path : "";
  while (*p) {
    while (*p == '/') ++p;
    if (strncmp(p, "preflight_", 10) == 0) return true;
    const char *slash = strchr(p, '/');
    if (!slash) break;
    p = slash + 1;
  }
  return false;
}

static int cmd_public_fuzz_all_history(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *dir_arg = value_after_equals(argc, argv, 4, "--dir", "build/fuzz/all");
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *markdown_path = value_after_equals(argc, argv, 4, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after_equals(argc, argv, 4, "--md", "");
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
                                               NYNTH_DEFAULT_FUZZ_THREADS);
  const char *profile_arg = value_after_equals(argc, argv, 4, "--profile", "insane");
  bool all_rows = has_flag_after(argc, argv, 4, "--all");
  int limit = atoi(value_after_equals(argc, argv, 4, "--limit", "500"));
  if (limit < 0) limit = 0;
  if (all_rows) limit = 0;

  char *scan_dir = NULL;
  if (dir_arg && *dir_arg) {
    if (path_is_absolute(dir_arg)) scan_dir = strdup(dir_arg);
    else (void)nynth_asprintf(&scan_dir, "%s", dir_arg);
  }
  string_list_t files = {0}, rows = {0}, failures = {0};
  fuzz_all_history_summary_t summary;
  memset(&summary, 0, sizeof(summary));
  snprintf(summary.scan_dir, sizeof(summary.scan_dir), "%s",
           scan_dir ? scan_dir : "");
  if (!scan_dir || !collect_regular_files_recursive(scan_dir, &files)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-history",
                                                  "history scan directory not readable",
                                                  scan_dir ? scan_dir : ""));
  }
  qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
  summary.files_scanned = files.count;

  int matched_total = 0;
  int skipped_preflight_json = 0;
  for (int i = 0; i < files.count; ++i) {
    if (!ny_has_suffix(files.items[i], ".json")) continue;
    ++summary.json_files;
    if (fuzz_all_history_skip_path(files.items[i])) {
      ++skipped_preflight_json;
      continue;
    }
    file_buf_t f = {0};
    if (!read_file(files.items[i], &f) || !f.data) {
      free(f.data);
      continue;
    }
    if (report_is_fuzz_all(f.data)) {
      string_list_t report_rows = {0};
      bool counts = collect_rows_from_report_json(f.data, &report_rows) &&
                    fuzz_all_history_report_counts_as_evidence(f.data,
                                                               &report_rows);
      string_list_free(&report_rows);
      if (counts) ++matched_total;
    }
    free(f.data);
  }
  int first_emit = (limit > 0 && matched_total > limit) ? matched_total - limit : 0;
  int matched_index = 0;
  double worst_attention_score = 0.0;
  time_t first_report_mtime = 0;
  time_t latest_report_mtime = 0;
  time_t latest_full_pressure_mtime = 0;
  for (int i = 0; i < files.count; ++i) {
    if (!ny_has_suffix(files.items[i], ".json")) continue;
    if (fuzz_all_history_skip_path(files.items[i])) continue;
    file_buf_t f = {0};
    if (!read_file(files.items[i], &f) || !f.data) {
      free(f.data);
      continue;
    }
    if (!report_is_fuzz_all(f.data)) {
      free(f.data);
      continue;
    }
    string_list_t report_rows = {0};
    bool have_report_rows = collect_rows_from_report_json(f.data, &report_rows);
    if (!have_report_rows ||
        !fuzz_all_history_report_counts_as_evidence(f.data, &report_rows)) {
      ++summary.ignored_no_evidence_reports;
      string_list_free(&report_rows);
      free(f.data);
      continue;
    }
    ++matched_index;
    ++summary.reports;
    char *report_rel = rel_path_dup(root, files.items[i]);
    struct stat report_st;
    bool have_report_stat = stat(files.items[i], &report_st) == 0;
    bool latest_candidate = false;
    if (have_report_stat) {
      if (!summary.first_report[0] || report_st.st_mtime < first_report_mtime) {
        first_report_mtime = report_st.st_mtime;
        snprintf(summary.first_report, sizeof(summary.first_report), "%s",
                 report_rel ? report_rel : "");
      }
      if (!summary.latest_report[0] || report_st.st_mtime >= latest_report_mtime) {
        latest_report_mtime = report_st.st_mtime;
        snprintf(summary.latest_report, sizeof(summary.latest_report), "%s",
                 report_rel ? report_rel : "");
        latest_candidate = true;
      }
    } else if (!summary.latest_report[0]) {
      snprintf(summary.latest_report, sizeof(summary.latest_report), "%s",
               report_rel ? report_rel : "");
      latest_candidate = true;
    }
    double lanes = 0.0, ok_lanes = 0.0, failure_count = 0.0;
    double duration_s = 0.0, budget_s = 0.0;
    double threads = 1.0;
    double finding_live = 0.0, finding_missing = 0.0;
    double known_reproduced = 0.0, known_fixed = 0.0;
    double known_lost = 0.0, known_baseline = 0.0;
    double perf_hotspots = 0.0, perf_max_ratio = 0.0;
    (void)summary_number_from_report(f.data, "cases", &lanes);
    (void)summary_number_from_report(f.data, "ok", &ok_lanes);
    (void)summary_number_from_report(f.data, "failure_count", &failure_count);
    (void)summary_number_from_report(f.data, "duration_s", &duration_s);
    (void)summary_number_from_report(f.data, "budget_s", &budget_s);
    (void)summary_number_from_report(f.data, "threads", &threads);
    if (threads < 1.0) threads = 1.0;
    (void)summary_number_from_report(f.data, "finding_live", &finding_live);
    (void)summary_number_from_report(f.data, "finding_missing", &finding_missing);
    (void)summary_number_from_report(f.data, "known_bug_reproduced", &known_reproduced);
    (void)summary_number_from_report(f.data, "known_bug_fixed_candidates", &known_fixed);
    (void)summary_number_from_report(f.data, "known_bug_lost_signal", &known_lost);
    (void)summary_number_from_report(f.data, "known_bug_baseline_failures", &known_baseline);
    (void)summary_number_from_report(f.data, "perf_hotspots", &perf_hotspots);
    (void)summary_number_from_report(f.data, "perf_max_ratio", &perf_max_ratio);
    char *perf_case = summary_string_from_report(f.data, "perf_max_case");
    char *work_dir = summary_string_from_report(f.data, "work_dir");
    char *forever_script = summary_string_from_report(f.data, "forever_script");
    double sub_rows_total = 0.0, sub_failures_total = 0.0;
    double row_elapsed_ms_total = 0.0;
    for (int ri = 0; ri < report_rows.count; ++ri) {
      const char *rr = report_rows.items[ri];
      double value = 0.0;
      if (json_number_range(rr, rr + strlen(rr), "elapsed_ms", &value) && value > 0.0)
        row_elapsed_ms_total += value;
      value = 0.0;
      if (json_number_range(rr, rr + strlen(rr), "sub_rows", &value) && value > 0.0)
        sub_rows_total += value;
      value = 0.0;
      if (json_number_range(rr, rr + strlen(rr), "sub_failures", &value) && value > 0.0)
        sub_failures_total += value;
    }
    double requested_budget_s = budget_s > 0.0 ? budget_s : duration_s;
    double effective_budget_s = 0.0;
    bool have_effective_budget =
        summary_number_from_report(f.data, "effective_budget_s", &effective_budget_s) &&
        effective_budget_s > 0.0;
    if (!have_effective_budget) effective_budget_s = requested_budget_s;
    double row_elapsed_s = row_elapsed_ms_total / 1000.0;
    if (failure_count > 0.0 && row_elapsed_s > 0.0 &&
        (effective_budget_s <= 0.0 || row_elapsed_s < effective_budget_s))
      effective_budget_s = row_elapsed_s;
    if (requested_budget_s > 0.0 && effective_budget_s > requested_budget_s)
      effective_budget_s = requested_budget_s;
    if (effective_budget_s < 0.0) effective_budget_s = 0.0;
    double thread_s = effective_budget_s * threads;
    bool full_pressure =
        fuzz_all_infer_full_pressure_report(f.data, &report_rows,
                                            duration_s, budget_s);

    bool report_ok = failure_count <= 0.0 && (lanes <= 0.0 || ok_lanes >= lanes);
    bool attention = !report_ok || finding_live > 0.0 || finding_missing > 0.0 ||
                     known_reproduced > 0.0 || known_lost > 0.0 ||
                     known_baseline > 0.0 || perf_hotspots > 0.0;
    if (report_ok) ++summary.ok_reports;
    else ++summary.failed_reports;
    if (attention) ++summary.attention_reports;
    bool latest_full_pressure_candidate = false;
    if (full_pressure) {
      ++summary.full_pressure_reports;
      if (report_ok) ++summary.full_pressure_ok_reports;
      if (attention) ++summary.full_pressure_attention_reports;
      summary.full_pressure_thread_s += thread_s;
      if (have_report_stat) {
        if (!summary.latest_full_pressure_report[0] ||
            report_st.st_mtime >= latest_full_pressure_mtime) {
          latest_full_pressure_mtime = report_st.st_mtime;
          snprintf(summary.latest_full_pressure_report,
                   sizeof(summary.latest_full_pressure_report), "%s",
                   report_rel ? report_rel : "");
          latest_full_pressure_candidate = true;
        }
      } else if (!summary.latest_full_pressure_report[0]) {
        snprintf(summary.latest_full_pressure_report,
                 sizeof(summary.latest_full_pressure_report), "%s",
                 report_rel ? report_rel : "");
        latest_full_pressure_candidate = true;
      }
    }
    summary.total_duration_s += duration_s;
    summary.total_budget_s += budget_s;
    summary.total_effective_budget_s += effective_budget_s;
    summary.total_thread_s += thread_s;
    summary.total_lanes += lanes;
    summary.total_ok_lanes += ok_lanes;
    summary.total_failures += failure_count;
    summary.total_sub_rows += sub_rows_total;
    summary.total_sub_failures += sub_failures_total;
    if (threads > summary.max_threads) summary.max_threads = threads;
    summary.finding_live_total += finding_live;
    summary.finding_missing_total += finding_missing;
    summary.known_reproduced_total += known_reproduced;
    summary.known_lost_total += known_lost;
    summary.known_baseline_total += known_baseline;
    summary.perf_hotspots_total += perf_hotspots;
    if (perf_max_ratio > summary.perf_max_ratio) {
      summary.perf_max_ratio = perf_max_ratio;
      snprintf(summary.worst_perf_case, sizeof(summary.worst_perf_case), "%s",
               perf_case ? perf_case : "");
    }
    if (latest_candidate) {
      summary.latest_lanes = lanes;
      summary.latest_ok_lanes = ok_lanes;
      summary.latest_failure_count = failure_count;
      summary.latest_sub_failures = sub_failures_total;
      summary.latest_finding_live = finding_live;
      summary.latest_finding_missing = finding_missing;
      summary.latest_known_reproduced = known_reproduced;
      summary.latest_known_lost = known_lost;
      summary.latest_known_baseline = known_baseline;
      summary.latest_perf_hotspots = perf_hotspots;
      summary.latest_perf_max_ratio = perf_max_ratio;
      summary.latest_report_ok = report_ok;
      summary.latest_report_attention = attention;
      snprintf(summary.latest_perf_case, sizeof(summary.latest_perf_case), "%s",
               perf_case ? perf_case : "");
    }
    if (latest_full_pressure_candidate) {
      summary.latest_full_pressure_lanes = lanes;
      summary.latest_full_pressure_ok_lanes = ok_lanes;
      summary.latest_full_pressure_failure_count = failure_count;
      summary.latest_full_pressure_sub_failures = sub_failures_total;
      summary.latest_full_pressure_finding_live = finding_live;
      summary.latest_full_pressure_finding_missing = finding_missing;
      summary.latest_full_pressure_known_reproduced = known_reproduced;
      summary.latest_full_pressure_known_lost = known_lost;
      summary.latest_full_pressure_known_baseline = known_baseline;
      summary.latest_full_pressure_perf_hotspots = perf_hotspots;
      summary.latest_full_pressure_perf_max_ratio = perf_max_ratio;
      summary.latest_full_pressure_ok = report_ok;
      summary.latest_full_pressure_attention = attention;
      snprintf(summary.latest_full_pressure_perf_case,
               sizeof(summary.latest_full_pressure_perf_case), "%s",
               perf_case ? perf_case : "");
    }
    double attention_score = failure_count * 10.0 + finding_live * 100.0 +
                             finding_missing * 50.0 + known_reproduced * 80.0 +
                             known_lost * 30.0 + known_baseline * 80.0 +
                             perf_hotspots;
    if (attention && attention_score >= worst_attention_score) {
      worst_attention_score = attention_score;
      snprintf(summary.worst_report, sizeof(summary.worst_report), "%s",
               report_rel ? report_rel : "");
    }

    if (matched_index > first_emit) {
      str_buf_t row = {0};
      (void)sb_append(&row, "{\"kind\":\"fuzz-all-history\",\"report\":");
      append_rel_json_str(&row, root, files.items[i]);
      (void)sb_appendf(&row,
                       ",\"ok\":%s,\"attention\":%s,"
                       "\"full_pressure\":%s,"
                       "\"lanes\":%.0f,\"ok_lanes\":%.0f,"
                       "\"failure_count\":%.0f,\"duration_s\":%.3f,"
                       "\"budget_s\":%.3f,\"effective_budget_s\":%.3f,"
                       "\"threads\":%.0f,"
                       "\"thread_hours\":%.4f,\"thread_years\":%.8f,"
                       "\"sub_rows\":%.0f,\"sub_failures\":%.0f,"
                       "\"finding_live\":%.0f,"
                       "\"finding_missing\":%.0f,"
                       "\"known_bug_reproduced\":%.0f,"
                       "\"known_bug_fixed_candidates\":%.0f,"
                       "\"known_bug_lost_signal\":%.0f,"
                       "\"known_bug_baseline_failures\":%.0f,"
                       "\"perf_hotspots\":%.0f,"
                       "\"perf_max_ratio\":%.4f,\"perf_max_case\":",
                       report_ok ? "true" : "false",
                       attention ? "true" : "false",
                       full_pressure ? "true" : "false",
                       lanes, ok_lanes, failure_count, duration_s, budget_s,
                       effective_budget_s, threads, thread_s / 3600.0,
                       thread_s / (3600.0 * 24.0 * 365.0),
                       sub_rows_total, sub_failures_total,
                       finding_live, finding_missing, known_reproduced,
                       known_fixed, known_lost, known_baseline,
                       perf_hotspots, perf_max_ratio);
      (void)sb_append_json_str(&row, perf_case ? perf_case : "");
      (void)sb_append(&row, ",\"work_dir\":");
      append_rel_json_str(&row, root, work_dir ? work_dir : "");
      (void)sb_append(&row, ",\"forever_script\":");
      append_rel_json_str(&row, root, forever_script ? forever_script : "");
      (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
      (void)string_list_push_take(&rows, sb_take(&row));
      ++summary.emitted_reports;
    }

    free(report_rel);
    free(perf_case);
    free(work_dir);
    free(forever_script);
    string_list_free(&report_rows);
    free(f.data);
  }
     if (summary.reports <= 0 && failures.count == 0) {
       (void)string_list_push_take(&failures,
                                   make_fuzz_failure(root, "fuzz-all-history",
                                                     "no fuzz-all reports found",
                                                     scan_dir ? scan_dir : ""));
     }
  if (first_report_mtime > 0) {
    time_t now = time(NULL);
    time_t latest_for_span =
        latest_report_mtime > 0 ? latest_report_mtime : first_report_mtime;
    double span_s = difftime(latest_for_span, first_report_mtime);
    double age_s = difftime(now, first_report_mtime);
    if (span_s < 0.0) span_s = 0.0;
    if (age_s < 0.0) age_s = 0.0;
    summary.first_report_epoch = (double)first_report_mtime;
    summary.latest_report_epoch = (double)latest_for_span;
    summary.campaign_calendar_span_days = span_s / 86400.0;
    summary.campaign_calendar_age_days = age_s / 86400.0;
  }
     if (markdown_path && *markdown_path &&
         !write_fuzz_all_history_markdown(root, markdown_path, json_path,
                                          &summary, &rows, target_arg,
                                       hours_arg, threads_arg, profile_arg)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-history",
                                                  "history markdown write failed",
                                                  markdown_path));
  }

  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"scan_dir\":");
  append_rel_json_str(&extra, root, summary.scan_dir);
  if (markdown_path && *markdown_path) {
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, root, markdown_path);
  }
  (void)sb_append(&extra, ",\"target_thread_years\":");
  double target_years = atof(target_arg && *target_arg ? target_arg : "10");
  if (target_years > 0.0) (void)sb_appendf(&extra, "%.8f", target_years);
  else (void)sb_append(&extra, "10.00000000");
  (void)sb_append(&extra, ",\"hours_per_run\":");
  double hours_per_run = atof(hours_arg && *hours_arg ? hours_arg : "8");
  if (hours_per_run > 0.0) (void)sb_appendf(&extra, "%.4f", hours_per_run);
  else (void)sb_append(&extra, "8.0000");
  (void)sb_append(&extra, ",\"thread_request\":");
  (void)sb_append_json_str(&extra, threads_arg && *threads_arg ? threads_arg : NYNTH_DEFAULT_FUZZ_THREADS);
  (void)sb_append(&extra, ",\"profile\":");
  (void)sb_append_json_str(&extra, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_appendf(&extra,
                   ",\"files_scanned\":%d,\"json_files\":%d,"
                   "\"skipped_preflight_json\":%d,"
                   "\"reports\":%d,\"emitted_reports\":%d,"
                   "\"ignored_no_evidence_reports\":%d,"
                   "\"row_limit\":%d,\"ok_reports\":%d,"
                   "\"failed_reports\":%d,\"attention_reports\":%d,"
                      "\"full_pressure_reports\":%d,"
                      "\"full_pressure_ok_reports\":%d,"
                         "\"full_pressure_attention_reports\":%d,"
                             "\"campaign_hours\":%.4f,"
                       "\"requested_campaign_hours\":%.4f,"
                       "\"total_duration_s\":%.3f,\"total_budget_s\":%.3f,"
                       "\"total_effective_budget_s\":%.3f,"
                      "\"thread_hours\":%.4f,\"thread_years\":%.8f,"
                      "\"campaign_first_report_epoch\":%.0f,"
                      "\"campaign_latest_report_epoch\":%.0f,"
                      "\"campaign_calendar_span_days\":%.4f,"
                      "\"campaign_calendar_age_days\":%.4f,"
                      "\"campaign_calendar_percent_10y\":%.4f,"
                      "\"full_pressure_thread_hours\":%.4f,"
                   "\"full_pressure_thread_years\":%.8f,"
                   "\"max_threads\":%.0f,"
                   "\"checked_subcases\":%.0f,\"sub_failures_total\":%.0f,"
                   "\"total_lanes\":%.0f,\"total_ok_lanes\":%.0f,"
                   "\"total_failures\":%.0f,"
                   "\"finding_live_total\":%.0f,"
                   "\"finding_missing_total\":%.0f,"
                   "\"known_bug_reproduced_total\":%.0f,"
                   "\"known_bug_lost_signal_total\":%.0f,"
                   "\"known_bug_baseline_failures_total\":%.0f,"
                   "\"perf_hotspots_total\":%.0f,"
                   "\"perf_max_ratio\":%.4f",
                   summary.files_scanned, summary.json_files,
                   skipped_preflight_json,
                   summary.reports, summary.emitted_reports,
                   summary.ignored_no_evidence_reports, limit,
                   summary.ok_reports, summary.failed_reports,
                   summary.attention_reports,
                         summary.full_pressure_reports,
                         summary.full_pressure_ok_reports,
                         summary.full_pressure_attention_reports,
                          summary.total_effective_budget_s / 3600.0,
                       summary.total_budget_s / 3600.0,
                       summary.total_duration_s, summary.total_budget_s,
                          summary.total_effective_budget_s,
                          summary.total_thread_s / 3600.0,
                      summary.total_thread_s / (3600.0 * 24.0 * 365.0),
                      summary.first_report_epoch,
                      summary.latest_report_epoch,
                      summary.campaign_calendar_span_days,
                      summary.campaign_calendar_age_days,
                      fuzz_all_campaign_calendar_percent_10y(
                          summary.campaign_calendar_age_days),
                      summary.full_pressure_thread_s / 3600.0,
                      summary.full_pressure_thread_s / (3600.0 * 24.0 * 365.0),
                   summary.max_threads,
                   summary.total_sub_rows, summary.total_sub_failures,
                   summary.total_lanes, summary.total_ok_lanes,
                   summary.total_failures,
                   summary.finding_live_total, summary.finding_missing_total,
                   summary.known_reproduced_total,
                   summary.known_lost_total, summary.known_baseline_total,
                   summary.perf_hotspots_total, summary.perf_max_ratio);
     (void)sb_append(&extra, ",\"perf_max_case\":");
     (void)sb_append_json_str(&extra, summary.worst_perf_case);
  (void)sb_append(&extra, ",\"campaign_first_report\":");
  (void)sb_append_json_str(&extra, summary.first_report);
     (void)sb_append(&extra, ",\"latest_report\":");
  (void)sb_append_json_str(&extra, summary.latest_report);
  (void)sb_append(&extra, ",\"latest_full_pressure_report\":");
  (void)sb_append_json_str(&extra, summary.latest_full_pressure_report);
  (void)sb_appendf(&extra,
                   ",\"latest_report_ok\":%s,"
                   "\"latest_report_attention\":%s,"
                   "\"latest_full_pressure_ok\":%s,"
                   "\"latest_full_pressure_attention\":%s,"
                   "\"latest_lanes\":%.0f,"
                   "\"latest_ok_lanes\":%.0f,"
                   "\"latest_failure_count\":%.0f,"
                   "\"latest_sub_failures\":%.0f,"
                   "\"latest_finding_live\":%.0f,"
                   "\"latest_finding_missing\":%.0f,"
                   "\"latest_known_bug_reproduced\":%.0f,"
                   "\"latest_known_bug_lost_signal\":%.0f,"
                   "\"latest_known_bug_baseline_failures\":%.0f,"
                   "\"latest_perf_hotspots\":%.0f,"
                   "\"latest_perf_max_ratio\":%.4f",
                   summary.latest_report_ok ? "true" : "false",
                   summary.latest_report_attention ? "true" : "false",
                   summary.latest_full_pressure_ok ? "true" : "false",
                   summary.latest_full_pressure_attention ? "true" : "false",
                   summary.latest_lanes, summary.latest_ok_lanes,
                   summary.latest_failure_count, summary.latest_sub_failures,
                   summary.latest_finding_live, summary.latest_finding_missing,
                   summary.latest_known_reproduced, summary.latest_known_lost,
                   summary.latest_known_baseline, summary.latest_perf_hotspots,
                   summary.latest_perf_max_ratio);
  (void)sb_append(&extra, ",\"latest_perf_max_case\":");
  (void)sb_append_json_str(&extra, summary.latest_perf_case);
  (void)sb_appendf(&extra,
                   ",\"latest_full_pressure_lanes\":%.0f,"
                   "\"latest_full_pressure_ok_lanes\":%.0f,"
                   "\"latest_full_pressure_failure_count\":%.0f,"
                   "\"latest_full_pressure_sub_failures\":%.0f,"
                   "\"latest_full_pressure_finding_live\":%.0f,"
                   "\"latest_full_pressure_finding_missing\":%.0f,"
                   "\"latest_full_pressure_known_bug_reproduced\":%.0f,"
                   "\"latest_full_pressure_known_bug_lost_signal\":%.0f,"
                   "\"latest_full_pressure_known_bug_baseline_failures\":%.0f,"
                   "\"latest_full_pressure_perf_hotspots\":%.0f,"
                   "\"latest_full_pressure_perf_max_ratio\":%.4f",
                   summary.latest_full_pressure_lanes,
                   summary.latest_full_pressure_ok_lanes,
                   summary.latest_full_pressure_failure_count,
                   summary.latest_full_pressure_sub_failures,
                   summary.latest_full_pressure_finding_live,
                   summary.latest_full_pressure_finding_missing,
                   summary.latest_full_pressure_known_reproduced,
                   summary.latest_full_pressure_known_lost,
                   summary.latest_full_pressure_known_baseline,
                   summary.latest_full_pressure_perf_hotspots,
                   summary.latest_full_pressure_perf_max_ratio);
  (void)sb_append(&extra, ",\"latest_full_pressure_perf_max_case\":");
  (void)sb_append_json_str(&extra, summary.latest_full_pressure_perf_case);
  (void)sb_append(&extra, ",\"worst_report\":");
  (void)sb_append_json_str(&extra, summary.worst_report);

  char *report = build_native_report_json(&rows, &failures,
                                          "fuzz-all-history", extra.data);
  int rc = emit_native_report(report, json_path, "all fuzz history",
                              rows.count, failures.count);
  free(extra.data);
  free(scan_dir);
  string_list_free(&files);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static void worklist_append_command(str_buf_t *row, const char *key,
                                    const char *cmd) {
  if (!row || !key || !*key) return;
  (void)sb_append(row, ",\"");
  (void)sb_append(row, key);
  (void)sb_append(row, "\":");
  (void)sb_append_json_str(row, cmd ? cmd : "");
}

static char *worklist_abs_path(const char *root, const char *path) {
  if (!path || !*path) return NULL;
  if (path_is_absolute(path)) return strdup(path);
  char *out = NULL;
  (void)asprintf(&out, "%s/%s", root && *root ? root : ".", path);
  return out;
}

static void worklist_collect_failed_subjects(const char *root, const char *report,
                                             string_list_t *subjects, int depth) {
  if (!subjects || !report || !*report || depth > 2) return;
  char *path = worklist_abs_path(root, report);
  file_buf_t f = {0};
  string_list_t rows = {0};
  if (!path || !read_file(path, &f) || !f.data ||
      !collect_rows_from_report_json(f.data, &rows)) {
    free(path);
    free(f.data);
    return;
  }
  for (int i = 0; i < rows.count; ++i) {
    const char *row = rows.items[i];
    bool ok = json_bool_range(row, row + strlen(row), "ok", true);
    bool skipped = json_bool_range(row, row + strlen(row), "skipped", false);
    if (ok || skipped) continue;
    char *name = json_string_or_empty(row, "name");
    if (!name || !*name) {
      free(name);
      name = json_string_or_empty(row, "target");
    }
    if (!name || !*name) {
      free(name);
      name = json_string_or_empty(row, "case");
    }
    if (name && *name) (void)string_list_push_unique_copy(subjects, name);
    char *child_report = json_string_or_empty(row, "report");
    if (child_report && *child_report)
      worklist_collect_failed_subjects(root, child_report, subjects, depth + 1);
    free(child_report);
    free(name);
  }
  string_list_free(&rows);
  free(f.data);
  free(path);
}

static char *worklist_failure_subject(const char *root, const char *report) {
  string_list_t subjects = {0};
  worklist_collect_failed_subjects(root, report, &subjects, 0);
  str_buf_t out = {0};
  for (int i = 0; i < subjects.count && i < 6; ++i) {
    if (i) (void)sb_append(&out, ", ");
    (void)sb_append(&out, subjects.items[i]);
  }
  if (subjects.count > 6) (void)sb_append(&out, ", ...");
  string_list_free(&subjects);
  if (!out.data || !*out.data) {
    free(out.data);
    return strdup("unknown lane");
  }
  return sb_take(&out);
}

typedef struct {
  string_list_t reports;
  string_list_t stdout_logs;
  string_list_t stderr_logs;
  string_list_t command_logs;
  string_list_t afl_output_dirs;
  string_list_t afl_stats;
  string_list_t saved_inputs;
  string_list_t repro_wrappers;
  string_list_t repro_commands;
  string_list_t raw_repro_commands;
  int failed_rows;
  int timed_out_rows;
  int saved_hangs;
  int saved_crashes;
} worklist_failure_details_t;

static void worklist_push_rel_unique(const char *root, string_list_t *items,
                                     const char *path) {
  if (!items || !path || !*path) return;
  char *rel = rel_path_dup(root ? root : "", path);
  (void)string_list_push_unique_copy(items, rel && *rel ? rel : path);
  free(rel);
}

static void worklist_failure_details_free(worklist_failure_details_t *details) {
  if (!details) return;
  string_list_free(&details->reports);
  string_list_free(&details->stdout_logs);
  string_list_free(&details->stderr_logs);
  string_list_free(&details->command_logs);
  string_list_free(&details->afl_output_dirs);
  string_list_free(&details->afl_stats);
  string_list_free(&details->saved_inputs);
  string_list_free(&details->repro_wrappers);
  string_list_free(&details->repro_commands);
  string_list_free(&details->raw_repro_commands);
}

static bool worklist_is_afl_saved_testcase(const char *path) {
  if (!path || !*path) return false;
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  return strncmp(base, "id:", 3) == 0;
}

static void worklist_collect_afl_saved_inputs(const char *root,
                                              const char *afl_out,
                                              string_list_t *saved) {
  if (!saved || !afl_out || !*afl_out) return;
  char *out_abs = worklist_abs_path(root, afl_out);
  if (!out_abs) return;
  const char *subdirs[] = {"crashes", "default/crashes", "hangs", "default/hangs"};
  for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); ++i) {
    char dir[4096];
    if (!path_join(dir, sizeof(dir), out_abs, subdirs[i])) continue;
    string_list_t files = {0};
    if (!collect_regular_files_recursive(dir, &files)) {
      string_list_free(&files);
      continue;
    }
    qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
    for (int f = 0; f < files.count; ++f)
      if (worklist_is_afl_saved_testcase(files.items[f]))
        (void)string_list_push_unique_copy(saved, files.items[f]);
    string_list_free(&files);
  }
  free(out_abs);
}

static bool worklist_target_name_safe(const char *target) {
  if (!target || !*target) return false;
  for (const char *p = target; *p; ++p) {
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
      return false;
  }
  return true;
}

static char *worklist_direct_afl_repro_wrapper(const char *target) {
  if (!worklist_target_name_safe(target)) return NULL;
  const char *scratch = getenv("NYNTH_SCRATCH_ROOT");
  if (scratch && *scratch)
    return nynth_scratch_pathf(scratch, "afl_wrappers/%s-normalize.sh",
                               target);
  char wrapper[512];
  snprintf(wrapper, sizeof(wrapper),
           "build/cache/scratch/afl_wrappers/%s-normalize.sh", target);
  return strdup(wrapper);
}

static char *worklist_direct_afl_repro_command(const char *root,
                                               const char *target,
                                               const char *saved_input,
                                               bool raw) {
  if (!worklist_target_name_safe(target) || !saved_input || !*saved_input)
    return NULL;
  char *wrapper = worklist_direct_afl_repro_wrapper(target);
  if (!wrapper) return NULL;
  char *saved_input_abs = worklist_abs_path(root, saved_input);
  str_buf_t cmd = {0};
  char nynth_root[4096] = {0};
  bool repo_root =
      root && *root &&
      find_nynth_root(nynth_root, sizeof(nynth_root)) &&
      strcmp(root, nynth_root) == 0;
  if (repo_root || !root || !*root || strcmp(root, ".") == 0) {
    (void)sb_append(&cmd, "cd .");
  } else {
    (void)sb_append(&cmd, "cd ");
    append_shell_single_quoted(&cmd, root);
  }
  (void)sb_append(&cmd, " && ");
  if (raw) (void)sb_append(&cmd, "env NYNTH_AFL_RAW=1 ");
  (void)sb_append(&cmd, "timeout 15s ");
  char *wrapper_cmd = rel_path_dup(root ? root : "", wrapper);
  append_shell_single_quoted(&cmd, wrapper_cmd && *wrapper_cmd ? wrapper_cmd : wrapper);
  (void)sb_append_c(&cmd, ' ');
  char *saved_input_cmd =
      rel_path_dup(root ? root : "",
                   saved_input_abs && *saved_input_abs ? saved_input_abs
                                                       : saved_input);
  append_shell_single_quoted(&cmd, saved_input_cmd && *saved_input_cmd
                                       ? saved_input_cmd
                                       : saved_input);
  free(saved_input_cmd);
  free(wrapper_cmd);
  free(saved_input_abs);
  free(wrapper);
  return sb_take(&cmd);
}

static int worklist_count_existing_paths(const char *root,
                                         const string_list_t *items,
                                         bool executable) {
  if (!items) return 0;
  int count = 0;
  for (int i = 0; i < items->count; ++i) {
    char *abs = worklist_abs_path(root, items->items[i]);
    bool ok = abs && (executable ? executable_path(abs) : exists_path(abs));
    if (ok) ++count;
    free(abs);
  }
  return count;
}

static void worklist_verify_raw_replays(const string_list_t *commands,
                                        int *checked_out,
                                        int *passed_out,
                                        int *timeout_out,
                                        int *unexpected_out) {
  if (checked_out) *checked_out = 0;
  if (passed_out) *passed_out = 0;
  if (timeout_out) *timeout_out = 0;
  if (unexpected_out) *unexpected_out = 0;
  if (!commands) return;
  const int limit = 8;
  char nynth_root[4096] = {0};
  const char *cwd =
      find_nynth_root(nynth_root, sizeof(nynth_root)) && nynth_root[0] ?
          nynth_root : "/tmp";
  for (int i = 0; i < commands->count && i < limit; ++i) {
    const char *cmd = commands->items[i];
    if (!cmd || !*cmd) continue;
    char *argv[] = {"sh", "-lc", (char *)cmd, NULL};
    proc_result_t pr = run_proc(argv, cwd, 20.0);
    if (checked_out) ++*checked_out;
    if (pr.timed_out || pr.rc == 124) {
      if (timeout_out) ++*timeout_out;
    } else if (pr.rc == 0 || pr.rc == 1 || pr.rc == 2) {
      if (passed_out) ++*passed_out;
    } else {
      if (unexpected_out) ++*unexpected_out;
    }
    proc_result_free(&pr);
  }
}

static void worklist_collect_failure_details(const char *root, const char *report,
                                             worklist_failure_details_t *details,
                                             int depth) {
  if (!details || !report || !*report || depth > 2) return;
  char *path = worklist_abs_path(root, report);
  file_buf_t f = {0};
  string_list_t rows = {0};
  if (!path || !read_file(path, &f) || !f.data ||
      !collect_rows_from_report_json(f.data, &rows)) {
    free(path);
    free(f.data);
    return;
  }
  for (int i = 0; i < rows.count; ++i) {
    const char *row = rows.items[i];
    bool ok = json_bool_range(row, row + strlen(row), "ok", true);
    bool skipped = json_bool_range(row, row + strlen(row), "skipped", false);
    if (ok || skipped) continue;
    ++details->failed_rows;
    if (json_bool_range(row, row + strlen(row), "timed_out", false))
      ++details->timed_out_rows;
    double saved = 0.0;
    if (extract_json_number(row, "saved_hangs", &saved) && saved > 0.0)
      details->saved_hangs += (int)saved;
    if (extract_json_number(row, "saved_crashes", &saved) && saved > 0.0)
      details->saved_crashes += (int)saved;

    char *child_report = json_string_or_empty(row, "report");
    char *stdout_log = json_string_or_empty(row, "stdout_log");
    char *stderr_log = json_string_or_empty(row, "stderr_log");
    char *command_log = json_string_or_empty(row, "command_log");
    char *afl_out = json_string_or_empty(row, "out");
    char *afl_stats = json_string_or_empty(row, "stats");
    char *target = json_string_or_empty(row, "target");
    bool compile_only = json_bool_range(row, row + strlen(row), "compile_only", false);
    worklist_push_rel_unique(root, &details->reports, child_report);
    worklist_push_rel_unique(root, &details->stdout_logs, stdout_log);
    worklist_push_rel_unique(root, &details->stderr_logs, stderr_log);
    worklist_push_rel_unique(root, &details->command_logs, command_log);
    worklist_push_rel_unique(root, &details->afl_output_dirs, afl_out);
    worklist_push_rel_unique(root, &details->afl_stats, afl_stats);
    string_list_t saved_inputs = {0};
    worklist_collect_afl_saved_inputs(root, afl_out, &saved_inputs);
    for (int s = 0; s < saved_inputs.count; ++s) {
      char *saved_input_rel = rel_path_dup(root ? root : "", saved_inputs.items[s]);
      if (saved_input_rel && *saved_input_rel) {
        (void)string_list_push_unique_copy(&details->saved_inputs, saved_input_rel);
        if (compile_only) {
          char *wrapper = worklist_direct_afl_repro_wrapper(target);
          char *wrapper_rel = rel_path_dup(root ? root : "", wrapper ? wrapper : "");
          if (wrapper_rel && *wrapper_rel)
            (void)string_list_push_unique_copy(&details->repro_wrappers, wrapper_rel);
          char *repro = worklist_direct_afl_repro_command(root, target,
                                                          saved_input_rel, false);
          if (repro && *repro)
            (void)string_list_push_unique_copy(&details->repro_commands, repro);
          char *raw_repro = worklist_direct_afl_repro_command(root, target,
                                                              saved_input_rel, true);
          if (raw_repro && *raw_repro)
            (void)string_list_push_unique_copy(&details->raw_repro_commands, raw_repro);
          free(wrapper_rel);
          free(wrapper);
          free(repro);
          free(raw_repro);
        }
      }
      free(saved_input_rel);
    }
    string_list_free(&saved_inputs);
    if (child_report && *child_report)
      worklist_collect_failure_details(root, child_report, details, depth + 1);
    free(child_report);
    free(stdout_log);
    free(stderr_log);
    free(command_log);
    free(afl_out);
    free(afl_stats);
    free(target);
  }
  string_list_free(&rows);
  free(f.data);
  free(path);
}

static void worklist_append_string_list_field(str_buf_t *row, const char *key,
                                              const string_list_t *items) {
  if (!row || !key || !*key || !items) return;
  (void)sb_append(row, ",\"");
  (void)sb_append(row, key);
  (void)sb_append(row, "\":");
  append_string_list_json(row, items);
}

static bool worklist_add_item(const char *root, string_list_t *rows,
                              const char *report, bool active,
                              const char *category, const char *severity,
                              const char *reason, double count,
                              double ratio, const char *perf_case,
                              const char *subject) {
  if (!rows || !report || !*report || !category || !*category) return false;
  char *findings_json = path_with_suffix_ext(report, "-findings", ".json");
  char *findings_md = path_with_suffix_ext(report, "-findings", ".md");
  char *audit_cmd = NULL, *findings_cmd = NULL;
  (void)asprintf(&audit_cmd,
                 "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                 "./build/nynth fuzz all audit --report %s --strict",
                 report);
  (void)asprintf(&findings_cmd,
                 "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                 "./build/nynth fuzz all findings --report %s --json %s --markdown %s",
                 report,
                 findings_json ? findings_json : "<findings.json>",
                 findings_md ? findings_md : "<findings.md>");
      char *primary = NULL;
      if (strcmp(category, "perf-hotspot") == 0) {
        primary = strdup(
            "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
            "./build/nynth perf triage --fast --limit 5 --threshold 1.50 "
            "--json build/fuzz/ultra/perf-triage-current.json "
            "--markdown build/fuzz/ultra/perf-triage-current.md");
      } else if (strcmp(category, "known-bug-replay") == 0) {
      primary = strdup("env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth compiler known-bugs --timeout-s 15 --json build/fuzz/ultra/compiler-known-bugs.json");
    } else if (strcmp(category, "compiler-finding") == 0) {
      primary = strdup("env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth compiler findings --timeout-s 15 --json build/fuzz/ultra/compiler-findings-current.json");
  } else {
    primary = strdup(audit_cmd ? audit_cmd : "");
  }

  worklist_failure_details_t details = {0};
  bool have_failure_details = strcmp(category, "lane-failure") == 0;
  int saved_input_files = 0, repro_wrapper_files = 0;
  int repro_wrapper_executables = 0;
  bool repro_ready = false;
  int raw_checked = 0, raw_passed = 0, raw_timeouts = 0, raw_unexpected = 0;
  bool non_reproducing_afl_timeout = false;
  if (have_failure_details) {
    worklist_collect_failure_details(root, report, &details, 0);
    saved_input_files =
        worklist_count_existing_paths(root, &details.saved_inputs, false);
    repro_wrapper_files =
        worklist_count_existing_paths(root, &details.repro_wrappers, false);
    repro_wrapper_executables =
        worklist_count_existing_paths(root, &details.repro_wrappers, true);
    repro_ready = details.saved_inputs.count > 0 &&
                  details.repro_wrappers.count > 0 &&
                  details.repro_commands.count > 0 &&
                  details.raw_repro_commands.count == details.saved_inputs.count &&
                  details.repro_commands.count == details.saved_inputs.count &&
                  saved_input_files == details.saved_inputs.count &&
                  repro_wrapper_files == details.repro_wrappers.count &&
                  repro_wrapper_executables == details.repro_wrappers.count;
    if (repro_ready && details.saved_hangs > 0 && details.saved_crashes == 0 &&
        details.raw_repro_commands.count == details.saved_inputs.count &&
        details.raw_repro_commands.count <= 8) {
      worklist_verify_raw_replays(&details.raw_repro_commands, &raw_checked,
                                  &raw_passed, &raw_timeouts,
                                  &raw_unexpected);
      non_reproducing_afl_timeout =
          raw_checked == details.raw_repro_commands.count &&
          raw_passed == raw_checked &&
          raw_timeouts == 0 &&
          raw_unexpected == 0;
    }
  }

  bool row_active = active;
  const char *row_severity = severity ? severity : (active ? "high" : "historical");
  const char *row_reason = reason ? reason : "";
  char *demoted_reason = NULL;
  if (active && non_reproducing_afl_timeout) {
    row_active = false;
    row_severity = "advisory";
    (void)asprintf(&demoted_reason,
                   "%s; raw replays checked %d/%d and did not reproduce a timeout",
                   reason && *reason ? reason : "AFL timeout attention",
                   raw_passed, raw_checked);
    if (demoted_reason) row_reason = demoted_reason;
  }

  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"fuzz-all-workitem\",\"category\":");
  (void)sb_append_json_str(&row, category);
  (void)sb_append(&row, ",\"severity\":");
  (void)sb_append_json_str(&row, row_severity);
  (void)sb_append(&row, ",\"active\":");
  (void)sb_append(&row, row_active ? "true" : "false");
  (void)sb_append(&row, ",\"reason\":");
  (void)sb_append_json_str(&row, row_reason);
  (void)sb_append(&row, ",\"lane\":");
  (void)sb_append_json_str(&row, subject ? subject : "");
  (void)sb_appendf(&row, ",\"count\":%.0f,\"ratio\":%.4f", count, ratio);
  (void)sb_append(&row, ",\"perf_case\":");
  (void)sb_append_json_str(&row, perf_case ? perf_case : "");
  (void)sb_append(&row, ",\"report\":");
  append_rel_json_str(&row, root, report);
  (void)sb_append(&row, ",\"findings_report\":");
  append_rel_json_str(&row, root, findings_json ? findings_json : "");
  (void)sb_append(&row, ",\"findings_markdown\":");
  append_rel_json_str(&row, root, findings_md ? findings_md : "");
  if (have_failure_details) {
    (void)sb_appendf(&row,
                     ",\"failure_detail_count\":%d,"
                     "\"timed_out_rows\":%d,"
                     "\"saved_hangs\":%d,"
                     "\"saved_crashes\":%d,"
                     "\"saved_input_count\":%d,"
                     "\"repro_command_count\":%d,"
                     "\"raw_repro_command_count\":%d,"
                     "\"saved_input_files\":%d,"
                     "\"repro_wrapper_files\":%d,"
                     "\"repro_wrapper_executables\":%d,"
                     "\"repro_ready\":%s,"
                     "\"raw_repro_checked\":%d,"
                     "\"raw_repro_passed\":%d,"
                     "\"raw_repro_timeouts\":%d,"
                     "\"raw_repro_unexpected\":%d,"
                     "\"non_reproducing_afl_timeout\":%s",
                     details.failed_rows, details.timed_out_rows,
                     details.saved_hangs, details.saved_crashes,
                     details.saved_inputs.count, details.repro_commands.count,
                     details.raw_repro_commands.count,
                     saved_input_files, repro_wrapper_files,
                     repro_wrapper_executables,
                     repro_ready ? "true" : "false",
                     raw_checked, raw_passed, raw_timeouts, raw_unexpected,
                     non_reproducing_afl_timeout ? "true" : "false");
    (void)sb_append(&row, ",\"first_failed_report\":");
    (void)sb_append_json_str(&row, details.reports.count > 0 ? details.reports.items[0] : "");
    (void)sb_append(&row, ",\"first_stdout_log\":");
    (void)sb_append_json_str(&row, details.stdout_logs.count > 0 ? details.stdout_logs.items[0] : "");
    (void)sb_append(&row, ",\"first_stderr_log\":");
    (void)sb_append_json_str(&row, details.stderr_logs.count > 0 ? details.stderr_logs.items[0] : "");
    (void)sb_append(&row, ",\"first_command_log\":");
    (void)sb_append_json_str(&row, details.command_logs.count > 0 ? details.command_logs.items[0] : "");
    (void)sb_append(&row, ",\"first_afl_output_dir\":");
    (void)sb_append_json_str(&row, details.afl_output_dirs.count > 0 ? details.afl_output_dirs.items[0] : "");
    (void)sb_append(&row, ",\"first_saved_input\":");
    (void)sb_append_json_str(&row, details.saved_inputs.count > 0 ? details.saved_inputs.items[0] : "");
    (void)sb_append(&row, ",\"first_repro_wrapper\":");
    (void)sb_append_json_str(&row, details.repro_wrappers.count > 0 ? details.repro_wrappers.items[0] : "");
    (void)sb_append(&row, ",\"repro_command\":");
    (void)sb_append_json_str(&row, details.repro_commands.count > 0 ? details.repro_commands.items[0] : "");
    (void)sb_append(&row, ",\"raw_repro_command\":");
    (void)sb_append_json_str(&row, details.raw_repro_commands.count > 0 ? details.raw_repro_commands.items[0] : "");
    (void)sb_append(&row, ",\"first_raw_repro_command\":");
    (void)sb_append_json_str(&row, details.raw_repro_commands.count > 0 ? details.raw_repro_commands.items[0] : "");
    worklist_append_string_list_field(&row, "failed_reports", &details.reports);
    worklist_append_string_list_field(&row, "failed_stdout_logs", &details.stdout_logs);
    worklist_append_string_list_field(&row, "failed_stderr_logs", &details.stderr_logs);
    worklist_append_string_list_field(&row, "failed_command_logs", &details.command_logs);
    worklist_append_string_list_field(&row, "afl_output_dirs", &details.afl_output_dirs);
    worklist_append_string_list_field(&row, "afl_stats", &details.afl_stats);
    worklist_append_string_list_field(&row, "saved_inputs", &details.saved_inputs);
    worklist_append_string_list_field(&row, "repro_wrappers", &details.repro_wrappers);
    worklist_append_string_list_field(&row, "repro_commands", &details.repro_commands);
    worklist_append_string_list_field(&row, "raw_repro_commands", &details.raw_repro_commands);
    if (details.repro_commands.count > 0 && details.repro_commands.items[0] &&
        *details.repro_commands.items[0]) {
      char *direct_primary = strdup(details.repro_commands.items[0]);
      if (direct_primary) {
        free(primary);
        primary = direct_primary;
      }
    }
  }
  worklist_append_command(&row, "primary_command", primary);
  worklist_append_command(&row, "audit_command", audit_cmd);
  worklist_append_command(&row, "findings_command", findings_cmd);
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  if (have_failure_details) worklist_failure_details_free(&details);
  free(findings_json);
  free(findings_md);
  free(audit_cmd);
  free(findings_cmd);
  free(primary);
  free(demoted_reason);
  return row_active;
}

static bool fuzz_all_report_only_non_reproducing_afl_timeout(const char *root,
                                                             const char *report) {
  if (!report || !*report) return false;
  worklist_failure_details_t details = {0};
  worklist_collect_failure_details(root, report, &details, 0);
  int saved_input_files =
      worklist_count_existing_paths(root, &details.saved_inputs, false);
  int repro_wrapper_files =
      worklist_count_existing_paths(root, &details.repro_wrappers, false);
  int repro_wrapper_executables =
      worklist_count_existing_paths(root, &details.repro_wrappers, true);
  bool repro_ready = details.saved_inputs.count > 0 &&
                     details.repro_wrappers.count > 0 &&
                     details.repro_commands.count > 0 &&
                     details.raw_repro_commands.count == details.saved_inputs.count &&
                     details.repro_commands.count == details.saved_inputs.count &&
                     saved_input_files == details.saved_inputs.count &&
                     repro_wrapper_files == details.repro_wrappers.count &&
                     repro_wrapper_executables == details.repro_wrappers.count;
  int raw_checked = 0, raw_passed = 0, raw_timeouts = 0, raw_unexpected = 0;
  bool non_reproducing =
      repro_ready &&
      details.saved_hangs > 0 &&
      details.saved_crashes == 0 &&
      details.raw_repro_commands.count == details.saved_inputs.count &&
      details.raw_repro_commands.count <= 8;
  if (non_reproducing) {
    worklist_verify_raw_replays(&details.raw_repro_commands, &raw_checked,
                                &raw_passed, &raw_timeouts,
                                &raw_unexpected);
    non_reproducing =
        raw_checked == details.raw_repro_commands.count &&
        raw_passed == raw_checked &&
        raw_timeouts == 0 &&
        raw_unexpected == 0;
  }
  worklist_failure_details_free(&details);
  return non_reproducing;
}

static bool write_fuzz_all_worklist_markdown(const char *root,
                                             const char *markdown_path,
                                             const char *history_path,
                                             const char *worklist_json_path,
                                             const char *latest_report,
                                             int active_items,
                                             int historical_items,
                                             int historical_attention,
                                             bool include_history,
                                             const string_list_t *rows) {
  if (!markdown_path || !*markdown_path) return true;
  if (!rows) return false;
  char *history_rel = rel_path_dup(root ? root : "", history_path ? history_path : "build/fuzz/all/history.json");
  char *history_md = path_with_suffix_ext(history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json", "", ".md");
  char *history_dir = path_parent_dup(history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json", "build/fuzz/all");
  char *worklist_rel = rel_path_dup(root ? root : "", worklist_json_path && *worklist_json_path ? worklist_json_path : "build/fuzz/all/worklist.json");
  char *worklist_md = rel_path_dup(root ? root : "", markdown_path ? markdown_path : "");
  if (!worklist_md || !*worklist_md) {
    free(worklist_md);
    worklist_md = path_with_suffix_ext(worklist_rel && *worklist_rel ? worklist_rel : "build/fuzz/all/worklist.json", "", ".md");
  }
  char *history_worklist_rel =
      path_with_suffix_ext(worklist_rel && *worklist_rel ?
                           worklist_rel : "build/fuzz/all/worklist.json",
                           "-history", ".json");
  char *history_worklist_md =
      path_with_suffix_ext(worklist_rel && *worklist_rel ?
                           worklist_rel : "build/fuzz/all/worklist.json",
                           "-history", ".md");
  time_t now = time(NULL);
  struct tm tm_now;
  char stamp[64] = {0};
  if (localtime_r(&now, &tm_now))
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %z", &tm_now);
  str_buf_t md = {0};
  (void)sb_append(&md, include_history ?
                           "# Nynth Historical Worklist\n\n" :
                           "# Nynth Active Worklist\n\n");
  if (stamp[0]) {
    (void)sb_append(&md, "Generated: ");
    md_append_code(&md, stamp);
    (void)sb_append(&md, "\n\n");
  }
  (void)sb_append(&md, "Latest: ");
  md_append_code(&md, latest_report);
  (void)sb_append(&md, "\n\n## TLDR\n\n");
  (void)sb_appendf(&md, "- Active items: %d; historical emitted: %d; hidden attention: %d.\n",
                   active_items, historical_items, historical_attention);
  double active_failure_details = 0.0, active_saved_hangs = 0.0;
  double active_saved_crashes = 0.0, active_saved_inputs = 0.0;
  double active_repro_commands = 0.0, active_raw_repro_commands = 0.0;
  double active_repro_ready = 0.0;
  double non_reproducing_afl_timeouts = 0.0;
  for (int i = 0; i < rows->count; ++i) {
    const char *row = rows->items[i];
    if (json_bool_range(row, row + strlen(row),
                        "non_reproducing_afl_timeout", false))
      non_reproducing_afl_timeouts += 1.0;
    bool active = json_bool_range(row, row + strlen(row), "active", false);
    if (!active) continue;
    bool repro_ready = json_bool_range(row, row + strlen(row),
                                       "repro_ready", false);
    double value = 0.0;
    if (extract_json_number(row, "failure_detail_count", &value))
      active_failure_details += value;
    if (extract_json_number(row, "saved_hangs", &value))
      active_saved_hangs += value;
    if (extract_json_number(row, "saved_crashes", &value))
      active_saved_crashes += value;
    if (extract_json_number(row, "saved_input_count", &value))
      active_saved_inputs += value;
    if (extract_json_number(row, "repro_command_count", &value))
      active_repro_commands += value;
    if (extract_json_number(row, "raw_repro_command_count", &value))
      active_raw_repro_commands += value;
    if (repro_ready) active_repro_ready += 1.0;
  }
  if (active_failure_details > 0.0 || active_saved_hangs > 0.0 ||
      active_saved_crashes > 0.0)
    (void)sb_appendf(&md,
                     "- Active failure evidence: %.0f failed rows, %.0f saved hangs, %.0f saved crashes.\n",
                     active_failure_details, active_saved_hangs,
                     active_saved_crashes);
  if (active_saved_inputs > 0.0 || active_repro_commands > 0.0)
    (void)sb_appendf(&md,
                     "- Active direct AFL replays: %.0f saved inputs, %.0f replay commands, %.0f raw commands, %.0f ready.\n",
                     active_saved_inputs, active_repro_commands,
                     active_raw_repro_commands,
                     active_repro_ready);
  if (non_reproducing_afl_timeouts > 0.0)
    (void)sb_appendf(&md,
                     "- %snon-reproducing AFL timeout rows: %.0f verified by raw replay.\n",
                     historical_items > 0 ? "Historical " : "",
                     non_reproducing_afl_timeouts);
  if (active_items == 0)
    (void)sb_append(&md, "- Current latest report has no open worklist items.\n");
  (void)sb_append(&md, include_history ?
                           "\n## Active Snapshot\n\n" :
                           "\n## Active\n\n");
  int printed = 0;
  for (int i = 0; i < rows->count; ++i) {
    const char *row = rows->items[i];
    bool active = json_bool_range(row, row + strlen(row), "active", false);
    if (!active) continue;
    char *category = json_string_or_empty(row, "category");
    char *reason = json_string_or_empty(row, "reason");
    char *report = json_string_or_empty(row, "report");
    char *primary = json_string_or_empty(row, "primary_command");
    char *raw = json_string_or_empty(row, "first_raw_repro_command");
    if (!raw || !*raw) {
      free(raw);
      raw = json_string_or_empty(row, "raw_repro_command");
    }
    char *failed_report = json_string_or_empty(row, "first_failed_report");
    char *stdout_log = json_string_or_empty(row, "first_stdout_log");
    char *stderr_log = json_string_or_empty(row, "first_stderr_log");
    char *command_log = json_string_or_empty(row, "first_command_log");
    char *afl_output = json_string_or_empty(row, "first_afl_output_dir");
    char *saved_input = json_string_or_empty(row, "first_saved_input");
    char *repro_wrapper = json_string_or_empty(row, "first_repro_wrapper");
    bool repro_ready = json_bool_range(row, row + strlen(row),
                                       "repro_ready", false);
    double count = 0.0, ratio = 0.0, detail_count = 0.0;
    double saved_hangs = 0.0, saved_crashes = 0.0, saved_input_files = 0.0;
    double repro_wrapper_executables = 0.0;
    (void)extract_json_number(row, "count", &count);
    (void)extract_json_number(row, "ratio", &ratio);
    (void)extract_json_number(row, "failure_detail_count", &detail_count);
    (void)extract_json_number(row, "saved_hangs", &saved_hangs);
    (void)extract_json_number(row, "saved_crashes", &saved_crashes);
    (void)extract_json_number(row, "saved_input_files", &saved_input_files);
    (void)extract_json_number(row, "repro_wrapper_executables",
                              &repro_wrapper_executables);
    (void)sb_append(&md, "- ");
    md_append_code(&md, category);
    (void)sb_appendf(&md, ": %.0f", count);
    if (ratio > 0.0) (void)sb_appendf(&md, ", %.4fx", ratio);
    (void)sb_append(&md, "; ");
    md_append_inline(&md, reason);
    md_append_labeled_path(&md, "report", report);
    (void)sb_append(&md, "\n  Run: ");
    md_append_code(&md, raw && *raw ? raw : primary);
    if (detail_count > 0.0 || saved_hangs > 0.0 || saved_crashes > 0.0) {
      (void)sb_append(&md, "\n  Evidence:");
      if (detail_count > 0.0) (void)sb_appendf(&md, " %.0f failed rows", detail_count);
      if (saved_hangs > 0.0) (void)sb_appendf(&md, ", %.0f saved hangs", saved_hangs);
      if (saved_crashes > 0.0) (void)sb_appendf(&md, ", %.0f saved crashes", saved_crashes);
      (void)sb_appendf(&md, "; replay_ready `%s`; input_files %.0f; executable_wrappers %.0f",
                       repro_ready ? "true" : "false", saved_input_files,
                       repro_wrapper_executables);
      (void)sb_append(&md, "\n  Artifacts: child_report ");
      md_append_code(&md, failed_report);
      md_append_labeled_path(&md, "saved_input", saved_input);
      md_append_labeled_path(&md, "command_log", command_log);
    }
    (void)sb_append(&md, "\n");
    ++printed;
    free(category);
    free(reason);
    free(report);
    free(primary);
    free(raw);
    free(failed_report);
    free(stdout_log);
    free(stderr_log);
    free(command_log);
    free(afl_output);
    free(saved_input);
    free(repro_wrapper);
  }
  if (!printed) {
    (void)sb_append(&md, include_history ?
                             "No active work items in latest report.\n" :
                             "No active work items.\n");
  }
  str_buf_t historical_md = {0};
  printed = 0;
  for (int i = 0; i < rows->count; ++i) {
    const char *row = rows->items[i];
    bool active = json_bool_range(row, row + strlen(row), "active", false);
    if (active) continue;
    char *category = json_string_or_empty(row, "category");
    char *reason = json_string_or_empty(row, "reason");
    char *report = json_string_or_empty(row, "report");
    char *raw = json_string_or_empty(row, "first_raw_repro_command");
    if (!raw || !*raw) {
      free(raw);
      raw = json_string_or_empty(row, "raw_repro_command");
    }
    double count = 0.0, ratio = 0.0;
    double saved_hangs = 0.0, saved_inputs = 0.0;
    double raw_checked = 0.0, raw_passed = 0.0;
    double raw_timeouts = 0.0, raw_unexpected = 0.0;
    bool non_reproducing = json_bool_range(row, row + strlen(row),
                                           "non_reproducing_afl_timeout",
                                           false);
    (void)extract_json_number(row, "count", &count);
    (void)extract_json_number(row, "ratio", &ratio);
    (void)extract_json_number(row, "saved_hangs", &saved_hangs);
    (void)extract_json_number(row, "saved_input_count", &saved_inputs);
    (void)extract_json_number(row, "raw_repro_checked", &raw_checked);
    (void)extract_json_number(row, "raw_repro_passed", &raw_passed);
    (void)extract_json_number(row, "raw_repro_timeouts", &raw_timeouts);
    (void)extract_json_number(row, "raw_repro_unexpected", &raw_unexpected);
    (void)sb_append(&historical_md, "- ");
    md_append_code(&historical_md, category);
    (void)sb_appendf(&historical_md, ": %.0f", count);
    if (ratio > 0.0) (void)sb_appendf(&historical_md, ", %.4fx", ratio);
    (void)sb_append(&historical_md, "; ");
    md_append_inline(&historical_md, reason);
    md_append_labeled_path(&historical_md, "report", report);
    if (non_reproducing) {
      (void)sb_appendf(&historical_md,
                       "; demoted timeout: raw replay %.0f/%.0f passed, hangs %.0f, inputs %.0f, raw timeouts %.0f, unexpected %.0f",
                       raw_passed, raw_checked, saved_hangs, saved_inputs,
                       raw_timeouts, raw_unexpected);
      if (raw && *raw) {
        (void)sb_append(&historical_md, "\n  Recheck: ");
        md_append_code(&historical_md, raw);
      }
    }
    (void)sb_append(&historical_md, "\n");
    ++printed;
    free(category);
    free(reason);
    free(report);
    free(raw);
  }
  if (printed) {
    (void)sb_append(&md, "\n## Historical Context\n\n");
    (void)sb_append(&md, historical_md.data ? historical_md.data : "");
  }
  free(historical_md.data);
  (void)sb_append(&md, "\n## Refresh\n\n```bash\n");
  (void)sb_appendf(&md,
                   "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                   "./build/nynth fuzz all history --dir %s --json %s --markdown %s\n",
                   history_dir && *history_dir ? history_dir : "build/fuzz/all",
                   history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json",
                   history_md && *history_md ? history_md : "build/fuzz/all/history.md");
  if (historical_items > 0) {
    (void)sb_appendf(&md,
                     "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                     "./build/nynth fuzz all worklist --history %s --include-history --json %s --markdown %s\n",
                     history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json",
                     worklist_rel && *worklist_rel ? worklist_rel : "build/fuzz/all/worklist.json",
                     worklist_md && *worklist_md ? worklist_md : "build/fuzz/all/worklist.md");
  } else {
    (void)sb_appendf(&md,
                     "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                     "./build/nynth fuzz all worklist --history %s --json %s --markdown %s\n",
                     history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json",
                     worklist_rel && *worklist_rel ? worklist_rel : "build/fuzz/all/worklist.json",
                     worklist_md && *worklist_md ? worklist_md : "build/fuzz/all/worklist.md");
    if (historical_attention > 0) {
      (void)sb_appendf(&md,
                       "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                       "./build/nynth fuzz all worklist --history %s --include-history --json %s --markdown %s\n",
                       history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json",
                       history_worklist_rel && *history_worklist_rel ?
                           history_worklist_rel : "build/fuzz/all/worklist-history.json",
                       history_worklist_md && *history_worklist_md ?
                           history_worklist_md : "build/fuzz/all/worklist-history.md");
    }
  }
  (void)sb_append(&md, "```\n");
  bool ok = md.data && write_file_text(markdown_path, md.data);
  free(md.data);
  free(history_rel);
  free(history_md);
  free(history_dir);
  free(worklist_rel);
  free(worklist_md);
  free(history_worklist_rel);
  free(history_worklist_md);
  return ok;
}

static int cmd_public_fuzz_all_worklist(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *history_arg = value_after_equals(argc, argv, 4, "--history", "build/fuzz/all/history.json");
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *markdown_path = value_after_equals(argc, argv, 4, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after_equals(argc, argv, 4, "--md", "");
  bool include_history = has_flag_after(argc, argv, 4, "--include-history") ||
                         has_flag_after(argc, argv, 4, "--all");
  char *history_path = NULL;
  if (history_arg && *history_arg) {
    if (path_is_absolute(history_arg)) history_path = strdup(history_arg);
    else (void)nynth_asprintf(&history_path, "%s", history_arg);
  }
  file_buf_t history = {0};
  string_list_t rows = {0}, hist_rows = {0}, failures = {0};
  if (!history_path || !read_file(history_path, &history) || !history.data) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-worklist",
                                                  "history report not readable",
                                                  history_path ? history_path : ""));
  }
  char *latest = history.data ? summary_string_from_report(history.data, "latest_report") : strdup("");
  double attention_reports = 0.0;
  if (history.data) {
    (void)summary_number_from_report(history.data, "attention_reports", &attention_reports);
    if (!collect_rows_from_report_json(history.data, &hist_rows)) {
      (void)string_list_push_take(&failures,
                                  make_fuzz_failure(root, "fuzz-all-worklist",
                                                    "history report missing rows",
                                                    history_path ? history_path : ""));
    }
  }
  if ((!latest || !*latest) && hist_rows.count > 0) {
    free(latest);
    latest = json_string_or_empty(hist_rows.items[hist_rows.count - 1], "report");
  }
  int active_items = 0, historical_items = 0, historical_attention = 0;
  for (int i = 0; i < hist_rows.count; ++i) {
    const char *row = hist_rows.items[i];
    char *report = json_string_or_empty(row, "report");
    char *perf_case = json_string_or_empty(row, "perf_max_case");
    bool active = report && latest && strcmp(report, latest) == 0;
    bool attention = json_bool_range(row, row + strlen(row), "attention", false);
    if (!active && attention) ++historical_attention;
    if (!active && !include_history) {
      free(report);
      free(perf_case);
      continue;
    }
    double failure_count = 0.0, finding_live = 0.0, finding_missing = 0.0;
    double known_reproduced = 0.0, known_lost = 0.0, known_baseline = 0.0;
    double perf_hotspots = 0.0, perf_ratio = 0.0;
    (void)extract_json_number(row, "failure_count", &failure_count);
    (void)extract_json_number(row, "finding_live", &finding_live);
    (void)extract_json_number(row, "finding_missing", &finding_missing);
    (void)extract_json_number(row, "known_bug_reproduced", &known_reproduced);
    (void)extract_json_number(row, "known_bug_lost_signal", &known_lost);
    (void)extract_json_number(row, "known_bug_baseline_failures", &known_baseline);
    (void)extract_json_number(row, "perf_hotspots", &perf_hotspots);
    (void)extract_json_number(row, "perf_max_ratio", &perf_ratio);
    if (failure_count > 0.0) {
      char *subject = worklist_failure_subject(root, report);
      char *reason = NULL;
      (void)asprintf(&reason, "all-run report has failing lanes: %s",
                     subject && *subject ? subject : "unknown lane");
      int before_call = rows.count;
      bool row_active =
          worklist_add_item(root, &rows, report, active, "lane-failure",
                            active ? "critical" : "historical",
                            reason ? reason : "all-run report has failing lanes",
                            failure_count, 0.0, "", subject);
      int delta = rows.count - before_call;
      if (delta > 0) {
        if (row_active) active_items += delta;
        else historical_items += delta;
      }
      free(reason);
      free(subject);
    }
    if (finding_live > 0.0) {
      int before_call = rows.count;
      bool row_active =
          worklist_add_item(root, &rows, report, active, "compiler-finding",
                            active ? "critical" : "historical",
                            "compiler watchlist seed is live", finding_live,
                            0.0, "", "");
      int delta = rows.count - before_call;
      if (delta > 0) {
        if (row_active) active_items += delta;
        else historical_items += delta;
      }
    }
    if (finding_missing > 0.0) {
      int before_call = rows.count;
      bool row_active =
          worklist_add_item(root, &rows, report, active, "compiler-finding",
                            active ? "high" : "historical",
                            "compiler watchlist artifact is missing", finding_missing,
                            0.0, "", "");
      int delta = rows.count - before_call;
      if (delta > 0) {
        if (row_active) active_items += delta;
        else historical_items += delta;
      }
    }
    if (known_reproduced > 0.0 || known_lost > 0.0 || known_baseline > 0.0) {
      double count = known_reproduced + known_lost + known_baseline;
      int before_call = rows.count;
      bool row_active =
          worklist_add_item(root, &rows, report, active, "known-bug-replay",
                            active ? "high" : "historical",
                            "saved known-bug replay needs review", count,
                            0.0, "", "");
      int delta = rows.count - before_call;
      if (delta > 0) {
        if (row_active) active_items += delta;
        else historical_items += delta;
      }
    }
    if (perf_hotspots > 0.0) {
      int before_call = rows.count;
      bool row_active =
          worklist_add_item(root, &rows, report, active, "perf-hotspot",
                            active ? "medium" : "historical",
                            "C-vs-Ny perf triage reported hotspots", perf_hotspots,
                            perf_ratio, perf_case, perf_case);
      int delta = rows.count - before_call;
      if (delta > 0) {
        if (row_active) active_items += delta;
        else historical_items += delta;
      }
    }
    free(report);
    free(perf_case);
  }
  if (markdown_path && *markdown_path &&
      !write_fuzz_all_worklist_markdown(root, markdown_path, history_path,
                                        json_path,
                                        latest ? latest : "", active_items,
                                        historical_items, historical_attention,
                                        include_history,
                                        &rows)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-worklist",
                                                  "worklist markdown write failed",
                                                  markdown_path));
  }
  double active_failure_details = 0.0, active_saved_hangs = 0.0;
  double active_saved_crashes = 0.0, active_saved_inputs = 0.0;
  double active_repro_commands = 0.0, active_raw_repro_commands = 0.0;
  double active_repro_ready = 0.0;
  double non_reproducing_afl_timeouts = 0.0;
  for (int i = 0; i < rows.count; ++i) {
    const char *row = rows.items[i];
    if (json_bool_range(row, row + strlen(row),
                        "non_reproducing_afl_timeout", false))
      non_reproducing_afl_timeouts += 1.0;
    bool active = json_bool_range(row, row + strlen(row), "active", false);
    if (!active) continue;
    bool repro_ready = json_bool_range(row, row + strlen(row),
                                       "repro_ready", false);
    double value = 0.0;
    if (extract_json_number(row, "failure_detail_count", &value))
      active_failure_details += value;
    if (extract_json_number(row, "saved_hangs", &value))
      active_saved_hangs += value;
    if (extract_json_number(row, "saved_crashes", &value))
      active_saved_crashes += value;
    if (extract_json_number(row, "saved_input_count", &value))
      active_saved_inputs += value;
    if (extract_json_number(row, "repro_command_count", &value))
      active_repro_commands += value;
    if (extract_json_number(row, "raw_repro_command_count", &value))
      active_raw_repro_commands += value;
    if (repro_ready) active_repro_ready += 1.0;
  }
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"history_report\":");
  append_rel_json_str(&extra, root, history_path ? history_path : "");
  if (markdown_path && *markdown_path) {
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, root, markdown_path);
  }
  (void)sb_append(&extra, ",\"latest_report\":");
  (void)sb_append_json_str(&extra, latest ? latest : "");
  (void)sb_appendf(&extra,
                   ",\"active_items\":%d,\"historical_items\":%d,"
                   "\"historical_attention_reports\":%d,"
                   "\"history_attention_reports\":%.0f,"
                   "\"active_failure_detail_count\":%.0f,"
                   "\"active_saved_hangs\":%.0f,"
                   "\"active_saved_crashes\":%.0f,"
                   "\"active_saved_inputs\":%.0f,"
                   "\"active_repro_commands\":%.0f,"
                   "\"active_raw_repro_commands\":%.0f,"
                   "\"active_repro_ready\":%.0f,"
                   "\"non_reproducing_afl_timeouts\":%.0f,"
                   "\"include_history\":%s,\"active_clear\":%s",
                   active_items, historical_items, historical_attention,
                   attention_reports, active_failure_details,
                   active_saved_hangs, active_saved_crashes,
                   active_saved_inputs, active_repro_commands,
                   active_raw_repro_commands,
                   active_repro_ready,
                   non_reproducing_afl_timeouts,
                   include_history ? "true" : "false",
                   active_items == 0 ? "true" : "false");
  char *report = build_native_report_json(&rows, &failures,
                                          "fuzz-all-worklist", extra.data);
  int rc = emit_native_report(report, json_path, "all fuzz worklist",
                              rows.count, failures.count);
  free(extra.data);
  free(history_path);
  free(history.data);
  free(latest);
  string_list_free(&hist_rows);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

typedef struct {
  const char *name;
  const char *category;
  const char *severity;
  const char *reason;
  const char *command;
} fuzz_all_expected_lane_t;

typedef struct {
  const char *lane;
  const char *phase;
  const char *file;
  const char *mode;
} fuzz_all_focus_companion_t;

static const fuzz_all_expected_lane_t FUZZ_ALL_EXPECTED_LANES[] = {
    {"compiler_std_audit", "compiler", "high", "stdlib compiler audit did not run", "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth compiler std-audit --json build/fuzz/ultra/compiler-std-audit.json --markdown build/fuzz/ultra/compiler-std-audit.md"},
    {"compiler_findings", "compiler", "critical", "active compiler finding replay did not run", "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth compiler findings --timeout-s 15 --json build/fuzz/ultra/compiler-findings-current.json"},
    {"compiler_known_bugs", "compiler", "critical", "saved compiler repro replay did not run", "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth compiler known-bugs --timeout-s 15 --json build/fuzz/ultra/compiler-known-bugs.json"},
      {"perf_triage", "perf", "high", "C-vs-Ny performance triage did not run", "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth perf triage --fast --limit 5 --threshold 1.50 --json build/fuzz/ultra/perf-triage-current.json --markdown build/fuzz/ultra/perf-triage-current.md"},
  {"prove_lab", "proof", "medium", "proof lab did not run", "./build/nynth prove lab --fast --json build/fuzz/ultra/prove-lab-fast.json"},
  {"synth_mixed", "synth", "high", "mixed synth optimizer generation did not run", "./build/nynth synth generate --fast --cases 1 --generator mixed --capture-failures --json build/fuzz/ultra/synth-mixed-fast.json"},
  {"synth_ir", "synth", "high", "IR synth generation did not run", "./build/nynth synth generate --fast --cases 1 --generator ir --capture-failures --json build/fuzz/ultra/synth-ir-fast.json"},
  {"synth_stress", "synth", "high", "stress synth generation did not run", "./build/nynth synth generate --fast --cases 1 --generator stress --capture-failures --json build/fuzz/ultra/synth-stress-fast.json"},
  {"synth_pure", "synth", "high", "pure optimizer selftest did not run", "./build/nynth selftest synth-pure --json build/fuzz/ultra/selftest-synth-pure.json"},
  {"campaign_core", "synth", "high", "multi-lane synth campaign did not run", "./build/nynth campaign run --lanes typed,optimizer,torture,ir,stress,random,cbridge,afl --cases 1 --fast --json build/fuzz/ultra/campaign-core-fast.json"},
  {"gc_soak", "gc", "high", "GC/runtime soak did not run", "./build/nynth fuzz gc run --smoke --direct-only --iterations 4096 --json build/fuzz/ultra/gc-smoke.json"},
  {"afl_compiler", "afl", "medium", "compiler AFL mutation lane did not run", "./build/nynth fuzz afl run --target compiler --minutes 20 --json build/fuzz/ultra/afl-compiler-20m.json"},
  {"afl_parsers", "afl", "medium", "parser AFL mutation lane did not run", "./build/nynth fuzz afl run --target parsers --minutes 20 --json build/fuzz/ultra/afl-parsers-20m.json"}
};

static const fuzz_all_focus_companion_t FUZZ_ALL_FOCUS_COMPANIONS[] = {
  {"perf_triage", "perf", "perf-triage-current.json", "perf-triage"},
  {"compiler_std_audit", "compiler", "compiler-std-audit.json", "compiler-std-audit"},
  {"compiler_findings", "compiler", "compiler-findings-current.json", "compiler-findings"},
  {"compiler_known_bugs", "compiler", "compiler-known-bugs.json", "compiler-known-bugs"},
  {"prove_lab", "proof", "prove-lab-fast.json", ""}
};

static bool fuzz_all_expected_lane_name(const char *name) {
  if (!name || !*name) return false;
  for (size_t i = 0; i < sizeof(FUZZ_ALL_EXPECTED_LANES) / sizeof(FUZZ_ALL_EXPECTED_LANES[0]); ++i) {
    if (strcmp(FUZZ_ALL_EXPECTED_LANES[i].name, name) == 0) return true;
  }
  return false;
}

static bool coverage_skip_has_expected_replacement(const char *name) {
  if (!name || !*name) return false;
  return fuzz_all_expected_lane_name(name) ||
         strcmp(name, "synth_lanes") == 0 ||
         strcmp(name, "afl") == 0 ||
         strcmp(name, "nytrix_fuzz") == 0 ||
         strcmp(name, "nytrix_sanitizers") == 0;
}

static bool coverage_is_companion_lane(const char *name, const char *phase) {
  if ((phase && (strcmp(phase, "nytrix") == 0 ||
                 strcmp(phase, "sanitizer") == 0)))
    return true;
  return name &&
         (strcmp(name, "nytrix_fuzz") == 0 ||
          strcmp(name, "nytrix_sanitizers") == 0 ||
          strcmp(name, "nytrix_asan") == 0 ||
          strcmp(name, "nytrix_ubsan") == 0);
}

static void coverage_count_latest_report_advisory(const string_list_t *report_rows,
                                                  int *advisory_out,
                                                  int *companion_skipped_out) {
  if (advisory_out) *advisory_out = 0;
  if (companion_skipped_out) *companion_skipped_out = 0;
  if (!report_rows) return;
  for (int i = 0; i < report_rows->count; ++i) {
    const char *row = report_rows->items[i];
    char *name = json_string_or_empty(row, "name");
    char *phase = json_string_or_empty(row, "phase");
    bool is_skipped = json_bool_range(row, row + strlen(row), "skipped", false);
    bool dry_run = json_bool_range(row, row + strlen(row), "dry_run", false);
    if (!dry_run && is_skipped && coverage_is_companion_lane(name, phase)) {
      if (advisory_out) ++*advisory_out;
      if (companion_skipped_out) ++*companion_skipped_out;
    }
    free(name);
    free(phase);
  }
}

static void coverage_add_gap(const char *category, const char *severity,
                             const char *lane, const char *reason,
                             const char *command, string_list_t *rows,
                             int *gap_count, int *blocker_count) {
  if (!rows || !lane || !*lane) return;
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"fuzz-all-coverage-gap\",\"category\":");
  (void)sb_append_json_str(&row, category ? category : "");
  (void)sb_append(&row, ",\"severity\":");
  (void)sb_append_json_str(&row, severity ? severity : "medium");
  (void)sb_append(&row, ",\"lane\":");
  (void)sb_append_json_str(&row, lane);
  (void)sb_append(&row, ",\"reason\":");
  (void)sb_append_json_str(&row, reason ? reason : "");
  (void)sb_append(&row, ",\"command\":");
  (void)sb_append_json_str(&row, command ? command : "");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  if (gap_count) ++*gap_count;
  if (blocker_count && (!severity || strcmp(severity, "advisory") != 0))
    ++*blocker_count;
}

static const char *coverage_detail_command_for(const char *name,
                                               const char *phase,
                                               const char *reason) {
  if ((name && strcmp(name, "nytrix_sanitizers") == 0) ||
      (phase && strcmp(phase, "sanitizer") == 0) ||
      (reason && strstr(reason, "--no-sanitizers")))
    return "./build/nynth fuzz all run --profile insane --hours 8 --threads 25% --dir build/fuzz/all --allow-nytrix --json build/fuzz/all/with-sanitizers.json";
  if ((name && strcmp(name, "nytrix_fuzz") == 0) ||
      (phase && strcmp(phase, "nytrix") == 0) ||
      (reason && strstr(reason, "--no-nytrix")))
    return "./build/nynth fuzz all run --profile insane --hours 8 --threads 25% --dir build/fuzz/all --allow-nytrix --json build/fuzz/all/with-nytrix.json";
  if ((phase && strcmp(phase, "afl") == 0) ||
      (name && (strcmp(name, "afl") == 0 ||
                strcmp(name, "afl_compiler") == 0 ||
                strcmp(name, "afl_parsers") == 0)))
    return "./build/nynth fuzz all run --only-lane afl --profile insane --hours 8 --threads 25% --dir build/fuzz/all --json build/fuzz/all/with-afl.json";
  if (reason && strstr(reason, "budget too short"))
    return "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 NYNTH_RUN_REPEAT=good nice -n 10 ./build/fuzz/all/run-next.sh";
  if (name && strcmp(name, "synth_lanes") == 0)
    return "./build/nynth fuzz all run --profile insane --hours 8 --threads 25% --dir build/fuzz/all --json build/fuzz/all/with-synth.json";
  if (name && strcmp(name, "gc_soak") == 0)
    return "./build/nynth fuzz gc run --profile soak --budget-s 4096 --threads 25% --validate-gc --json build/fuzz/ultra/gc-soak.json";
  if (name && strcmp(name, "perf_triage") == 0)
    return "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth perf triage --fast --limit 10 --threshold 1.50 --json build/fuzz/ultra/perf-triage-current.json --markdown build/fuzz/ultra/perf-triage-current.md";
  if (name && strcmp(name, "prove_lab") == 0)
    return "./build/nynth prove lab --fast --json build/fuzz/ultra/prove-lab-fast.json";
  if (name && strcmp(name, "compiler_std_audit") == 0)
    return NYNTH_COMPILER_STD_AUDIT_COMMAND;
  if (name && strcmp(name, "compiler_findings") == 0)
    return "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth compiler findings --timeout-s 15 --json build/fuzz/ultra/compiler-findings-current.json";
  if (name && strcmp(name, "compiler_known_bugs") == 0)
    return "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth compiler known-bugs --timeout-s 15 --json build/fuzz/ultra/compiler-known-bugs.json";
  return "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/fuzz/all/run-next.sh";
}

static bool coverage_report_is_clean_evidence(const char *json,
                                              const char *mode) {
  if (!json) return false;
  if (mode && *mode && !report_summary_mode_is(json, mode)) return false;
  double failure_count = 0.0;
  if (summary_number_from_report(json, "failure_count", &failure_count) &&
      failure_count > 0.0)
    return false;
  if (json_failures_nonempty(json)) return false;
  const char *rows = json_top_level_value_after_key(json, "rows");
  return rows && *rows == '[' && count_json_array_items(rows) > 0;
}

static bool coverage_add_focus_companion_report(
    const char *root, const char *focus_dir,
    const fuzz_all_focus_companion_t *spec, string_list_t *report_rows,
    string_list_t *coverage_rows,
    int *reports_considered, int *companion_reports_considered) {
  if (!root || !focus_dir || !*focus_dir || !spec || !spec->lane ||
      !*spec->lane || !report_rows)
    return false;
  if (fuzz_all_history_row_ran(report_rows, spec->lane)) return false;
  char *rel_path = path_child_dup(focus_dir, spec->file);
  char *abs_path = NULL;
  if (rel_path && path_is_absolute(rel_path)) {
    abs_path = strdup(rel_path);
  } else if (rel_path) {
    abs_path = path_child_dup(root, rel_path);
  }
  file_buf_t report = {0};
  bool clean = abs_path && read_file(abs_path, &report) &&
               coverage_report_is_clean_evidence(report.data, spec->mode);
  if (!clean) {
    free(report.data);
    free(abs_path);
    free(rel_path);
    return false;
  }
  const char *rows_json = json_top_level_value_after_key(report.data, "rows");
  int sub_rows = count_json_array_items(rows_json);
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, spec->lane);
  (void)sb_append(&row, ",\"kind\":\"fuzz-all-companion\",\"phase\":");
  (void)sb_append_json_str(&row, spec->phase ? spec->phase : "");
  (void)sb_appendf(&row,
                   ",\"ok\":true,\"required\":true,\"companion\":true,"
                   "\"rc\":0,\"elapsed_ms\":0,\"sub_rows\":%d,"
                   "\"sub_failures\":0,\"evidence_mode\":",
                   sub_rows);
  (void)sb_append_json_str(&row, spec->mode && *spec->mode ?
                           spec->mode : "native-report");
  (void)sb_append(&row, ",\"report\":");
  append_rel_json_str(&row, root, abs_path ? abs_path : rel_path);
  (void)sb_append(&row, ",\"command\":");
  (void)sb_append_json_str(&row,
                           coverage_detail_command_for(spec->lane,
                                                       spec->phase, ""));
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  bool pushed = string_list_push_take(report_rows, sb_take(&row));
  if (pushed && coverage_rows) {
    str_buf_t evidence = {0};
    (void)sb_append(&evidence,
                    "{\"kind\":\"fuzz-all-coverage-companion\",\"lane\":");
    (void)sb_append_json_str(&evidence, spec->lane);
    (void)sb_append(&evidence, ",\"category\":");
    (void)sb_append_json_str(&evidence, spec->phase ? spec->phase : "");
    (void)sb_append(&evidence, ",\"evidence_mode\":");
    (void)sb_append_json_str(&evidence, spec->mode && *spec->mode ?
                             spec->mode : "native-report");
    (void)sb_appendf(&evidence, ",\"sub_rows\":%d,\"report\":", sub_rows);
    append_rel_json_str(&evidence, root, abs_path ? abs_path : rel_path);
    (void)sb_append(&evidence, ",\"engine\":\"nynth_core\"}");
    (void)string_list_push_take(coverage_rows, sb_take(&evidence));
  }
  if (pushed) {
    if (reports_considered) ++*reports_considered;
    if (companion_reports_considered) ++*companion_reports_considered;
  }
  free(report.data);
  free(abs_path);
  free(rel_path);
  return pushed;
}

static char *coverage_make_detail_row(const char *category,
                                      const char *severity,
                                      const char *lane,
                                      const char *reason,
                                      const char *command) {
  if (!lane || !*lane) return NULL;
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"fuzz-all-coverage-detail\",\"state\":\"skipped\","
                       "\"blocking\":false,\"category\":");
  (void)sb_append_json_str(&row, category ? category : "");
  (void)sb_append(&row, ",\"severity\":");
  (void)sb_append_json_str(&row, severity ? severity : "info");
  (void)sb_append(&row, ",\"lane\":");
  (void)sb_append_json_str(&row, lane);
  (void)sb_append(&row, ",\"reason\":");
  (void)sb_append_json_str(&row, reason ? reason : "");
  (void)sb_append(&row, ",\"command\":");
  (void)sb_append_json_str(&row, command ? command : "");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  return sb_take(&row);
}

static void coverage_add_detail(const char *category, const char *severity,
                                const char *lane, const char *reason,
                                const char *command, string_list_t *rows,
                                int *detail_rows) {
  if (!rows || !lane || !*lane) return;
  char *row = coverage_make_detail_row(category, severity, lane, reason,
                                       command);
  if (!row) return;
  (void)string_list_push_take(rows, row);
  if (detail_rows) ++*detail_rows;
}

static int coverage_detail_priority(const char *severity,
                                    const char *reason) {
  int score = 30;
  if (severity && strcmp(severity, "critical") == 0) score += 30;
  else if (severity && strcmp(severity, "high") == 0) score += 20;
  else if (severity && strcmp(severity, "medium") == 0) score += 10;
  else if (severity && strcmp(severity, "advisory") == 0) score -= 5;
  if (reason && strstr(reason, "budget too short")) score += 40;
  else if (reason && strstr(reason, "not found")) score += 35;
  else if (reason && strstr(reason, "did not run")) score += 30;
  else if (reason && strncmp(reason, "disabled by", 11) == 0) score -= 15;
  else if (reason && strstr(reason, "filtered by --only-lane")) score -= 25;
  return score;
}

static int coverage_find_duplicate_detail(const string_list_t *rows,
                                          const char *category,
                                          const char *lane,
                                          const char *command) {
  if (!rows || !lane || !*lane || !command || !*command) return -1;
  for (int i = 0; i < rows->count; ++i) {
    char *kind = json_string_or_empty(rows->items[i], "kind");
    char *row_category = json_string_or_empty(rows->items[i], "category");
    char *row_lane = json_string_or_empty(rows->items[i], "lane");
    char *row_command = json_string_or_empty(rows->items[i], "command");
    bool match =
        kind && strcmp(kind, "fuzz-all-coverage-detail") == 0 &&
        row_lane && strcmp(row_lane, lane) == 0 &&
        row_command && strcmp(row_command, command) == 0 &&
        strcmp(row_category ? row_category : "",
               category ? category : "") == 0;
    free(kind);
    free(row_category);
    free(row_lane);
    free(row_command);
    if (match) return i;
  }
  return -1;
}

static bool coverage_detail_seen_or_record(string_list_t *seen,
                                           const char *category,
                                           const char *severity,
                                           const char *lane,
                                           const char *reason,
                                           const char *command) {
  if (!seen) return false;
  str_buf_t key = {0};
  (void)sb_append(&key, category ? category : "");
  (void)sb_append_c(&key, '\t');
  (void)sb_append(&key, severity ? severity : "");
  (void)sb_append_c(&key, '\t');
  (void)sb_append(&key, lane ? lane : "");
  (void)sb_append_c(&key, '\t');
  (void)sb_append(&key, reason ? reason : "");
  (void)sb_append_c(&key, '\t');
  (void)sb_append(&key, command ? command : "");
  bool seen_before = key.data && string_list_contains(seen, key.data);
  if (!seen_before && key.data) (void)string_list_push_copy(seen, key.data);
  free(key.data);
  return seen_before;
}

static void coverage_add_detail_unique(const char *category,
                                       const char *severity,
                                       const char *lane,
                                       const char *reason,
                                       const char *command,
                                       string_list_t *rows,
                                       string_list_t *seen,
                                       int *detail_rows) {
  if (coverage_detail_seen_or_record(seen, category, severity, lane, reason,
                                     command))
    return;
  int duplicate = coverage_find_duplicate_detail(rows, category, lane, command);
  if (duplicate >= 0) {
    char *old_severity = json_string_or_empty(rows->items[duplicate],
                                             "severity");
    char *old_reason = json_string_or_empty(rows->items[duplicate], "reason");
    int old_priority = coverage_detail_priority(old_severity, old_reason);
    int new_priority = coverage_detail_priority(severity, reason);
    free(old_severity);
    free(old_reason);
    if (new_priority <= old_priority) return;
    char *replacement = coverage_make_detail_row(category, severity, lane,
                                                reason, command);
    if (!replacement) return;
    free(rows->items[duplicate]);
    rows->items[duplicate] = replacement;
    return;
  }
  coverage_add_detail(category, severity, lane, reason, command, rows,
                      detail_rows);
}

static const char *fuzz_all_coverage_state(double coverage_lanes,
                                           double coverage_ran_lanes,
                                           double coverage_blocker_gaps,
                                           double coverage_failed_lanes);
static const char *fuzz_all_campaign_state(bool ready, bool target_reached,
                                           bool campaign_complete);
static const char *fuzz_all_campaign_incomplete_reason(
    bool ready, double blocker_count, double active_items,
    bool target_reached, bool campaign_complete);
static double fuzz_all_ratio_percent(double numerator, double denominator);
static double fuzz_all_not_run_lanes(double ran_lanes, double total_lanes);
static char *fuzz_all_coverage_next_preview_command(
    const char *coverage_command, const char *dir_arg,
    const char *target_arg, const char *hours_arg,
    const char *threads_arg, const char *profile_arg);
static char *fuzz_all_missing_evidence_state_file_path(const char *dir_path);
static char *fuzz_all_missing_evidence_state_command(const char *state_file);
static char *fuzz_all_missing_evidence_state_refresh_command(
    const char *script_command);
static char *fuzz_all_missing_evidence_low_cpu_command(
    const char *script_command);
static char *fuzz_all_missing_evidence_stop_file_path(const char *dir_path);
static char *fuzz_all_missing_evidence_stop_command(const char *stop_file);
static char *fuzz_all_missing_evidence_resume_command(const char *stop_file);
static bool fuzz_all_pid_alive(double pid_value);

typedef struct {
  char action[64];
  char category[64];
  char severity[64];
  char lane[128];
  char reason[512];
  char command[4096];
  char guarded_command[4096];
  char low_cpu_command[4096];
  char preview_command[4096];
  char state_file[4096];
  char state_command[4096];
  char state_refresh_command[4096];
  char stop_file[4096];
  char stop_command[4096];
  char resume_command[4096];
  char state_label[64];
  char state_phase[64];
  char state_event[64];
  char state_timestamp_utc[64];
  char state_last_report[4096];
  char state_stale_reason[64];
  char state_child_status[64];
  char state_threads[64];
  bool state_readable;
  bool state_fresh;
  bool state_live;
  bool state_child_alive;
  bool state_handoff_low_priority;
  bool state_handoff_load_wait;
  bool state_handoff_space_guard;
  bool state_handoff_run_lock;
  double state_age_seconds;
  double state_stale_after_seconds;
  double state_heartbeat_s;
  double state_heartbeat_count;
  double state_child_pid;
  double state_handoff_nice;
  double state_handoff_max_load_pct;
  double state_handoff_min_free_gb;
} fuzz_all_coverage_next_summary_t;

static void fuzz_all_coverage_next_load_state(
    const char *root, fuzz_all_coverage_next_summary_t *next) {
  if (!next || !next->state_file[0]) return;
  next->state_age_seconds = -1.0;
  next->state_handoff_nice = -1.0;
  next->state_handoff_max_load_pct = -1.0;
  next->state_handoff_min_free_gb = -1.0;
  char abs[4096] = {0};
  if (path_is_absolute(next->state_file)) {
    snprintf(abs, sizeof(abs), "%s", next->state_file);
  } else if (root && *root) {
    (void)path_join(abs, sizeof(abs), root, next->state_file);
  } else {
    snprintf(abs, sizeof(abs), "%s", next->state_file);
  }
  file_buf_t f = {0};
  if (!abs[0] || !read_file(abs, &f) || !f.data) {
    snprintf(next->state_label, sizeof(next->state_label), "%s", "missing");
    snprintf(next->state_stale_reason, sizeof(next->state_stale_reason), "%s",
             "missing");
    free(f.data);
    return;
  }
  next->state_readable = true;
  char *s = json_string_or_empty(f.data, "phase");
  snprintf(next->state_phase, sizeof(next->state_phase), "%s", s ? s : "");
  free(s);
  s = json_string_or_empty(f.data, "event");
  snprintf(next->state_event, sizeof(next->state_event), "%s", s ? s : "");
  free(s);
  s = json_string_or_empty(f.data, "timestamp_utc");
  snprintf(next->state_timestamp_utc, sizeof(next->state_timestamp_utc), "%s",
           s ? s : "");
  free(s);
  s = json_string_or_empty(f.data, "last_report");
  snprintf(next->state_last_report, sizeof(next->state_last_report), "%s",
           s ? s : "");
  free(s);
  s = json_string_or_empty(f.data, "threads");
  snprintf(next->state_threads, sizeof(next->state_threads), "%s", s ? s : "");
  free(s);
  const char *end = f.data + f.len;
  (void)json_number_range(f.data, end, "heartbeat_s",
                          &next->state_heartbeat_s);
  (void)json_number_range(f.data, end, "heartbeat_count",
                          &next->state_heartbeat_count);
  (void)json_number_range(f.data, end, "child_pid",
                          &next->state_child_pid);
  (void)json_number_range(f.data, end, "nice",
                          &next->state_handoff_nice);
  (void)json_number_range(f.data, end, "max_load_pct",
                          &next->state_handoff_max_load_pct);
  (void)json_number_range(f.data, end, "min_free_gb",
                          &next->state_handoff_min_free_gb);
  next->state_handoff_low_priority =
      json_bool_range(f.data, end, "low_priority", false);
  next->state_handoff_load_wait =
      json_bool_range(f.data, end, "load_wait", false);
  next->state_handoff_space_guard =
      json_bool_range(f.data, end, "space_guard", false);
  next->state_handoff_run_lock =
      json_bool_range(f.data, end, "run_lock", false);
  next->state_child_alive = fuzz_all_pid_alive(next->state_child_pid);
  struct stat st;
  if (stat(abs, &st) == 0) {
    next->state_age_seconds = difftime(time(NULL), st.st_mtime);
    if (next->state_age_seconds < 0.0) next->state_age_seconds = 0.0;
  }
  next->state_stale_after_seconds =
      fuzz_all_state_stale_after_seconds(next->state_heartbeat_s);
  next->state_fresh =
      fuzz_all_state_fresh_values(next->state_readable, next->state_phase,
                                  next->state_age_seconds,
                                  next->state_heartbeat_s,
                                  next->state_child_pid,
                                  next->state_child_alive);
  next->state_live = fuzz_all_state_phase_live(next->state_phase);
  snprintf(next->state_label, sizeof(next->state_label), "%s",
           fuzz_all_state_label_values(next->state_readable,
                                       next->state_phase));
  snprintf(next->state_stale_reason, sizeof(next->state_stale_reason), "%s",
           fuzz_all_state_stale_reason_values(
               next->state_readable, next->state_fresh, next->state_phase,
               next->state_age_seconds, next->state_heartbeat_s,
               next->state_child_pid, next->state_child_alive));
  snprintf(next->state_child_status, sizeof(next->state_child_status), "%s",
           fuzz_all_state_child_status(next->state_child_pid,
                                       next->state_child_alive));
  free(f.data);
}

static bool fuzz_all_coverage_next_state_refresh_required(
    const fuzz_all_coverage_next_summary_t *next) {
  if (!next || !next->lane[0] || !next->state_refresh_command[0])
    return false;
  if (!next->state_readable) return true;
  if (next->state_live) return false;
  return !next->state_fresh;
}

static const char *fuzz_all_coverage_next_state_refresh_reason(
    const fuzz_all_coverage_next_summary_t *next) {
  if (!fuzz_all_coverage_next_state_refresh_required(next)) return "";
  if (!next->state_readable)
    return next->state_stale_reason[0] ? next->state_stale_reason : "missing";
  if (!next->state_fresh)
    return next->state_stale_reason[0] ? next->state_stale_reason : "stale";
  return "";
}

static void fuzz_all_coverage_next_fill_guarded(
    fuzz_all_coverage_next_summary_t *next, const char *coverage_dir,
    const char *target_arg, const char *hours_arg, const char *threads_arg,
    const char *profile_arg) {
  if (!next || !next->command[0] ||
      !strstr(next->command, "fuzz all run")) {
    return;
  }
  char *script = path_child_dup(coverage_dir && *coverage_dir ?
                                    coverage_dir : "build/fuzz/all",
                                "run-missing-evidence.sh");
  if (script && *script) {
    if (path_is_absolute(script) || strncmp(script, "./", 2) == 0) {
      snprintf(next->guarded_command, sizeof(next->guarded_command), "%s",
               script);
    } else {
      snprintf(next->guarded_command, sizeof(next->guarded_command), "./%s",
               script);
    }
  }
  char *preview = fuzz_all_coverage_next_preview_command(
      next->command, coverage_dir, target_arg, hours_arg, threads_arg,
      profile_arg);
  snprintf(next->preview_command, sizeof(next->preview_command), "%s",
           preview ? preview : "");
  char *state_file = fuzz_all_missing_evidence_state_file_path(coverage_dir);
  char *state_command = fuzz_all_missing_evidence_state_command(state_file);
  char *state_refresh_command =
      fuzz_all_missing_evidence_state_refresh_command(next->guarded_command);
  char *low_cpu_command =
      fuzz_all_missing_evidence_low_cpu_command(next->guarded_command);
  char *stop_file = fuzz_all_missing_evidence_stop_file_path(coverage_dir);
  char *stop_command = fuzz_all_missing_evidence_stop_command(stop_file);
  char *resume_command = fuzz_all_missing_evidence_resume_command(stop_file);
  snprintf(next->state_file, sizeof(next->state_file), "%s",
           state_file ? state_file : "");
  snprintf(next->state_command, sizeof(next->state_command), "%s",
           state_command ? state_command : "");
  snprintf(next->state_refresh_command, sizeof(next->state_refresh_command),
           "%s", state_refresh_command ? state_refresh_command : "");
  snprintf(next->low_cpu_command, sizeof(next->low_cpu_command), "%s",
           low_cpu_command ? low_cpu_command : "");
  snprintf(next->stop_file, sizeof(next->stop_file), "%s",
           stop_file ? stop_file : "");
  snprintf(next->stop_command, sizeof(next->stop_command), "%s",
           stop_command ? stop_command : "");
  snprintf(next->resume_command, sizeof(next->resume_command), "%s",
           resume_command ? resume_command : "");
  free(script);
  free(preview);
  free(state_file);
  free(state_command);
  free(state_refresh_command);
  free(low_cpu_command);
  free(stop_file);
  free(stop_command);
  free(resume_command);
}

static void fuzz_all_coverage_select_next(
    const string_list_t *rows, const char *coverage_dir,
    const char *target_arg, const char *hours_arg, const char *threads_arg,
    const char *profile_arg, fuzz_all_coverage_next_summary_t *next) {
  if (!rows || !next) return;
  memset(next, 0, sizeof(*next));
  for (int i = 0; i < rows->count; ++i) {
    char *kind = json_string_or_empty(rows->items[i], "kind");
    bool is_coverage_row =
        kind && (strcmp(kind, "fuzz-all-coverage-detail") == 0 ||
                 strcmp(kind, "fuzz-all-coverage-gap") == 0);
    char *category = json_string_or_empty(rows->items[i], "category");
    char *severity = json_string_or_empty(rows->items[i], "severity");
    char *lane = json_string_or_empty(rows->items[i], "lane");
    char *reason = json_string_or_empty(rows->items[i], "reason");
    char *command = json_string_or_empty(rows->items[i], "command");
    bool filtered = reason && strstr(reason, "filtered by --only-lane");
    bool have_next = next->lane[0] != '\0';
    bool have_advisory =
        strcmp(next->severity, "advisory") == 0;
    bool new_advisory = severity && strcmp(severity, "advisory") == 0;
    bool should_select =
        is_coverage_row && lane && *lane && command && *command && !filtered &&
        (!have_next || (have_advisory && !new_advisory));
    if (should_select) {
      snprintf(next->action, sizeof(next->action), "%s",
               strstr(command, "fuzz all run") ?
                   "run-missing-evidence" : "run-coverage-command");
      snprintf(next->category, sizeof(next->category), "%s",
               category ? category : "");
      snprintf(next->severity, sizeof(next->severity), "%s",
               severity && *severity ? severity : "medium");
      snprintf(next->lane, sizeof(next->lane), "%s", lane);
      snprintf(next->reason, sizeof(next->reason), "%s",
               reason ? reason : "");
      snprintf(next->command, sizeof(next->command), "%s", command);
      next->guarded_command[0] = '\0';
      next->low_cpu_command[0] = '\0';
      next->preview_command[0] = '\0';
      next->state_file[0] = '\0';
      next->state_command[0] = '\0';
      next->state_refresh_command[0] = '\0';
      next->stop_file[0] = '\0';
      next->stop_command[0] = '\0';
      next->resume_command[0] = '\0';
      fuzz_all_coverage_next_fill_guarded(next, coverage_dir, target_arg,
                                          hours_arg, threads_arg,
                                          profile_arg);
    }
    bool selected_non_advisory =
        next->lane[0] && strcmp(next->severity, "advisory") != 0;
    free(kind);
    free(category);
    free(severity);
    free(lane);
    free(reason);
    free(command);
    if (selected_non_advisory) break;
  }
}

static void append_fuzz_all_coverage_report_next_fields(
    str_buf_t *row, const fuzz_all_coverage_next_summary_t *next) {
  bool have = next && next->lane[0] && next->command[0];
  const char *recommended_command =
      have && next->guarded_command[0] ? next->guarded_command :
          (have ? next->command : "");
  bool state_refresh_required =
      have && fuzz_all_coverage_next_state_refresh_required(next);
  const char *state_refresh_reason =
      state_refresh_required ?
          fuzz_all_coverage_next_state_refresh_reason(next) : "";
  (void)sb_append(row, ",\"coverage_next_action\":");
  (void)sb_append_json_str(row, have ? next->action : "none");
  (void)sb_append(row, ",\"coverage_next_category\":");
  (void)sb_append_json_str(row, have ? next->category : "");
  (void)sb_append(row, ",\"coverage_next_severity\":");
  (void)sb_append_json_str(row, have ? next->severity : "");
  (void)sb_append(row, ",\"coverage_next_lane\":");
  (void)sb_append_json_str(row, have ? next->lane : "");
  (void)sb_append(row, ",\"coverage_next_reason\":");
  (void)sb_append_json_str(row, have ? next->reason : "");
  (void)sb_append(row, ",\"coverage_next_command\":");
  (void)sb_append_json_str(row, have ? next->command : "");
  (void)sb_append(row, ",\"coverage_next_guarded_command\":");
  (void)sb_append_json_str(row, have ? next->guarded_command : "");
  (void)sb_append(row, ",\"coverage_next_low_cpu_command\":");
  (void)sb_append_json_str(row, have ? next->low_cpu_command : "");
  (void)sb_append(row, ",\"coverage_next_preview_command\":");
  (void)sb_append_json_str(row, have ? next->preview_command : "");
  (void)sb_append(row, ",\"recommended_action\":");
  (void)sb_append_json_str(row, have ? next->action : "none");
  (void)sb_append(row, ",\"recommended_reason\":");
  (void)sb_append_json_str(row, have ? next->reason : "");
  (void)sb_append(row, ",\"recommended_command\":");
  (void)sb_append_json_str(row, recommended_command);
  (void)sb_append(row, ",\"recommended_low_cpu_command\":");
  (void)sb_append_json_str(row, have ? next->low_cpu_command : "");
  (void)sb_append(row, ",\"recommended_preview_command\":");
  (void)sb_append_json_str(row, have ? next->preview_command : "");
  (void)sb_append(row, ",\"coverage_next_state_file\":");
  (void)sb_append_json_str(row, have ? next->state_file : "");
  (void)sb_append(row, ",\"coverage_next_state_command\":");
  (void)sb_append_json_str(row, have ? next->state_command : "");
  (void)sb_append(row, ",\"coverage_next_state_refresh_command\":");
  (void)sb_append_json_str(row, have ? next->state_refresh_command : "");
  (void)sb_append(row, ",\"coverage_next_state_refresh_required\":");
  (void)sb_append(row, state_refresh_required ? "true" : "false");
  (void)sb_append(row, ",\"coverage_next_state_refresh_reason\":");
  (void)sb_append_json_str(row, state_refresh_reason);
  (void)sb_append(row, ",\"recommended_state_refresh_command\":");
  (void)sb_append_json_str(
      row, state_refresh_required ? next->state_refresh_command : "");
  (void)sb_append(row, ",\"coverage_next_stop_file\":");
  (void)sb_append_json_str(row, have ? next->stop_file : "");
  (void)sb_append(row, ",\"coverage_next_stop_command\":");
  (void)sb_append_json_str(row, have ? next->stop_command : "");
  (void)sb_append(row, ",\"coverage_next_resume_command\":");
  (void)sb_append_json_str(row, have ? next->resume_command : "");
  (void)sb_append(row, ",\"coverage_next_state\":");
  (void)sb_append_json_str(row, have ? next->state_label : "");
  (void)sb_append(row, ",\"coverage_next_state_readable\":");
  (void)sb_append(row, have && next->state_readable ? "true" : "false");
  (void)sb_append(row, ",\"coverage_next_state_fresh\":");
  (void)sb_append(row, have && next->state_fresh ? "true" : "false");
  (void)sb_append(row, ",\"coverage_next_state_live\":");
  (void)sb_append(row, have && next->state_live ? "true" : "false");
  (void)sb_append(row, ",\"coverage_next_state_child_alive\":");
  (void)sb_append(row, have && next->state_child_alive ? "true" : "false");
  (void)sb_append(row, ",\"coverage_next_state_child_status\":");
  (void)sb_append_json_str(row, have ? next->state_child_status : "");
  (void)sb_append(row, ",\"coverage_next_state_stale_reason\":");
  (void)sb_append_json_str(row, have ? next->state_stale_reason : "");
  (void)sb_appendf(row,
                   ",\"coverage_next_state_age_seconds\":%.0f,"
                   "\"coverage_next_state_stale_after_seconds\":%.0f,"
                   "\"coverage_next_state_heartbeat_s\":%.0f,"
                   "\"coverage_next_state_heartbeat_count\":%.0f,"
                   "\"coverage_next_state_child_pid\":%.0f",
                   have ? next->state_age_seconds : -1.0,
                   have ? next->state_stale_after_seconds : 0.0,
                   have ? next->state_heartbeat_s : 0.0,
                   have ? next->state_heartbeat_count : 0.0,
                   have ? next->state_child_pid : 0.0);
  (void)sb_appendf(row,
                   ",\"coverage_next_state_handoff_low_priority\":%s,"
                   "\"coverage_next_state_handoff_nice\":%.0f,"
                   "\"coverage_next_state_handoff_load_wait\":%s,"
                   "\"coverage_next_state_handoff_max_load_pct\":%.0f,"
                   "\"coverage_next_state_handoff_space_guard\":%s,"
                   "\"coverage_next_state_handoff_min_free_gb\":%.0f,"
                   "\"coverage_next_state_handoff_run_lock\":%s",
                   have && next->state_handoff_low_priority ? "true" : "false",
                   have ? next->state_handoff_nice : -1.0,
                   have && next->state_handoff_load_wait ? "true" : "false",
                   have ? next->state_handoff_max_load_pct : -1.0,
                   have && next->state_handoff_space_guard ? "true" : "false",
                   have ? next->state_handoff_min_free_gb : -1.0,
                   have && next->state_handoff_run_lock ? "true" : "false");
  (void)sb_append(row, ",\"coverage_next_state_handoff_threads\":");
  (void)sb_append_json_str(row, have ? next->state_threads : "");
  (void)sb_append(row, ",\"coverage_next_state_phase\":");
  (void)sb_append_json_str(row, have ? next->state_phase : "");
  (void)sb_append(row, ",\"coverage_next_state_event\":");
  (void)sb_append_json_str(row, have ? next->state_event : "");
  (void)sb_append(row, ",\"coverage_next_state_timestamp_utc\":");
  (void)sb_append_json_str(row, have ? next->state_timestamp_utc : "");
  (void)sb_append(row, ",\"coverage_next_state_last_report\":");
  (void)sb_append_json_str(row, have ? next->state_last_report : "");
}

static bool fuzz_all_coverage_queue_consider_row(
    const char *row, bool want_advisory, string_list_t *seen_lanes,
    string_list_t *queue_rows, int *accepted_count) {
  if (!row || !queue_rows || !seen_lanes) return true;
  char *kind = json_string_or_empty(row, "kind");
  char *severity = json_string_or_empty(row, "severity");
  char *lane = json_string_or_empty(row, "lane");
  char *reason = json_string_or_empty(row, "reason");
  char *command = json_string_or_empty(row, "command");
  bool is_coverage_row =
      kind && (strcmp(kind, "fuzz-all-coverage-detail") == 0 ||
               strcmp(kind, "fuzz-all-coverage-gap") == 0);
  bool is_advisory = severity && strcmp(severity, "advisory") == 0;
  bool filtered = reason && strstr(reason, "filtered by --only-lane");
  bool accept = is_coverage_row && lane && *lane && command && *command &&
                !filtered && is_advisory == want_advisory &&
                !string_list_contains(seen_lanes, lane);
  bool ok = true;
  if (accept) {
    ok = string_list_push_unique_copy(seen_lanes, lane) &&
         string_list_push_copy(queue_rows, row);
    if (ok && accepted_count) ++*accepted_count;
  }
  free(kind);
  free(severity);
  free(lane);
  free(reason);
  free(command);
  return ok;
}

static bool fuzz_all_coverage_queue_build(const string_list_t *rows,
                                          string_list_t *queue_rows,
                                          int *non_advisory_count,
                                          int *advisory_count) {
  if (!rows || !queue_rows) return false;
  string_list_t seen_lanes = {0};
  if (non_advisory_count) *non_advisory_count = 0;
  if (advisory_count) *advisory_count = 0;
  for (int pass = 0; pass < 2; ++pass) {
    bool want_advisory = pass == 1;
    for (int i = 0; i < rows->count; ++i) {
      if (!fuzz_all_coverage_queue_consider_row(
              rows->items[i], want_advisory, &seen_lanes, queue_rows,
              want_advisory ? advisory_count : non_advisory_count)) {
        string_list_free(&seen_lanes);
        return false;
      }
    }
  }
  string_list_free(&seen_lanes);
  return true;
}

static char *fuzz_all_coverage_queue_lanes_string(
    const string_list_t *queue_rows) {
  str_buf_t out = {0};
  if (!queue_rows) return strdup("");
  for (int i = 0; i < queue_rows->count; ++i) {
    char *lane = json_string_or_empty(queue_rows->items[i], "lane");
    if (lane && *lane) {
      if (out.len > 0) (void)sb_append(&out, " -> ");
      (void)sb_append(&out, lane);
    }
    free(lane);
  }
  if (!out.data) return strdup("");
  return sb_take(&out);
}

static void append_fuzz_all_coverage_queue_json(str_buf_t *row,
                                                const char *queue_json) {
  (void)sb_append(row, ",\"coverage_queue\":");
  if (queue_json && *queue_json == '[' &&
      matching_json_end(queue_json, '[', ']')) {
    (void)sb_append(row, queue_json);
  } else {
    (void)sb_append(row, "[]");
  }
}

static void append_fuzz_all_coverage_queue_fields(
    str_buf_t *row, const string_list_t *queue_rows,
    int non_advisory_count, int advisory_count, const char *queue_lanes) {
  int queue_count = queue_rows ? queue_rows->count : 0;
  (void)sb_appendf(row,
                   ",\"coverage_queue_count\":%d,"
                   "\"coverage_queue_non_advisory_count\":%d,"
                   "\"coverage_queue_advisory_count\":%d",
                   queue_count, non_advisory_count, advisory_count);
  (void)sb_append(row, ",\"coverage_queue_lanes\":");
  (void)sb_append_json_str(row, queue_lanes ? queue_lanes : "");
  (void)sb_append(row, ",\"coverage_queue\":[");
  for (int i = 0; queue_rows && i < queue_rows->count; ++i) {
    char *category = json_string_or_empty(queue_rows->items[i], "category");
    char *severity = json_string_or_empty(queue_rows->items[i], "severity");
    char *lane = json_string_or_empty(queue_rows->items[i], "lane");
    char *reason = json_string_or_empty(queue_rows->items[i], "reason");
    char *command = json_string_or_empty(queue_rows->items[i], "command");
    if (i > 0) (void)sb_append(row, ",");
    (void)sb_append(row, "{\"lane\":");
    (void)sb_append_json_str(row, lane ? lane : "");
    (void)sb_append(row, ",\"severity\":");
    (void)sb_append_json_str(row, severity ? severity : "");
    (void)sb_append(row, ",\"category\":");
    (void)sb_append_json_str(row, category ? category : "");
    (void)sb_append(row, ",\"reason\":");
    (void)sb_append_json_str(row, reason ? reason : "");
    (void)sb_append(row, ",\"command\":");
    (void)sb_append_json_str(row, command ? command : "");
    (void)sb_append(row, "}");
    free(category);
    free(severity);
    free(lane);
    free(reason);
    free(command);
  }
  (void)sb_append(row, "]");
}

static bool fuzz_all_coverage_rows_by_queue(const string_list_t *rows,
                                            const string_list_t *queue_rows,
                                            string_list_t *ordered_rows) {
  if (!rows || !ordered_rows) return false;
  string_list_t queued_lanes = {0};
  for (int qi = 0; queue_rows && qi < queue_rows->count; ++qi) {
    char *queue_lane = json_string_or_empty(queue_rows->items[qi], "lane");
    if (!queue_lane || !*queue_lane ||
        string_list_contains(&queued_lanes, queue_lane)) {
      free(queue_lane);
      continue;
    }
    for (int ri = 0; ri < rows->count; ++ri) {
      char *lane = json_string_or_empty(rows->items[ri], "lane");
      bool match = lane && strcmp(lane, queue_lane) == 0;
      free(lane);
      if (match && !string_list_push_copy(ordered_rows, rows->items[ri])) {
        free(queue_lane);
        string_list_free(&queued_lanes);
        return false;
      }
    }
    if (!string_list_push_unique_copy(&queued_lanes, queue_lane)) {
      free(queue_lane);
      string_list_free(&queued_lanes);
      return false;
    }
    free(queue_lane);
  }
  for (int i = 0; i < rows->count; ++i) {
    char *lane = json_string_or_empty(rows->items[i], "lane");
    bool already_queued = lane && *lane &&
                          string_list_contains(&queued_lanes, lane);
    free(lane);
    if (!already_queued &&
        !string_list_push_copy(ordered_rows, rows->items[i])) {
      string_list_free(&queued_lanes);
      return false;
    }
  }
  string_list_free(&queued_lanes);
  return true;
}

static bool write_fuzz_all_coverage_markdown(const char *root,
                                             const char *markdown_path,
                                             const char *coverage_json_path,
                                             const char *report_path,
                                             bool aggregate_history,
                                             bool strict,
                                             const char *target_thread_years,
                                             const char *hours_per_run,
                                             const char *threads,
                                             const char *profile,
                                             int lanes, int ran_lanes,
                                             int failed_lanes,
                                             int disabled_lanes,
                                             int budget_short_lanes,
                                             int missing_tool_lanes,
                                             int blocker_gaps,
                                             int advisory_gaps,
                                             int reports_considered,
                                             int companion_reports_considered,
                                             int latest_report_advisory_gaps,
                                             int latest_report_companion_skipped_lanes,
                                             double checked_subcases,
                                             double sub_failures,
                                             const fuzz_all_coverage_next_summary_t *next,
                                             const string_list_t *rows) {
  if (!markdown_path || !*markdown_path) return true;
  if (!rows) return false;
  char *report_rel = rel_path_dup(root ? root : "", report_path ? report_path : "");
  char *coverage_json_rel =
      rel_path_dup(root ? root : "", coverage_json_path ? coverage_json_path : "");
  char *coverage_md_rel =
      rel_path_dup(root ? root : "", markdown_path ? markdown_path : "");
  char *coverage_dir =
      path_parent_dup(coverage_json_rel && *coverage_json_rel ?
                          coverage_json_rel :
                          (coverage_md_rel && *coverage_md_rel ?
                               coverage_md_rel : "build/fuzz/all/coverage.json"),
                      "build/fuzz/all");
  if (!coverage_json_rel || !*coverage_json_rel) {
    free(coverage_json_rel);
    coverage_json_rel = path_child_dup(coverage_dir, "coverage.json");
  }
  if (!coverage_md_rel || !*coverage_md_rel) {
    free(coverage_md_rel);
    coverage_md_rel = path_child_dup(coverage_dir, "coverage.md");
  }
  char *history_rel = aggregate_history ? strdup(report_rel ? report_rel : "") :
                                          path_child_dup(coverage_dir,
                                                         "history.json");
  char *worklist_rel = path_child_dup(coverage_dir, "worklist.json");
  char *plan_rel = path_child_dup(coverage_dir, "plan.json");
  char *status_rel = path_child_dup(coverage_dir, "status.json");
  char *status_md_rel = path_child_dup(coverage_dir, "status.md");
  time_t now = time(NULL);
  struct tm tm_now;
  char stamp[64] = {0};
  if (localtime_r(&now, &tm_now))
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %z", &tm_now);
  str_buf_t md = {0};
  (void)sb_append(&md, "# Nynth Campaign Coverage\n\n");
  if (stamp[0]) {
    (void)sb_append(&md, "Generated: ");
    md_append_code(&md, stamp);
    (void)sb_append(&md, "\n\n");
  }
  (void)sb_append(&md, "Report: ");
  md_append_code(&md, report_rel);
  (void)sb_append(&md, "\n\n## TLDR\n\n");
  const char *coverage_state =
      fuzz_all_coverage_state((double)lanes, (double)ran_lanes,
                              (double)blocker_gaps, (double)failed_lanes);
  int not_run_lanes =
      (int)fuzz_all_not_run_lanes((double)ran_lanes, (double)lanes);
  string_list_t tldr_queue_rows = {0};
  int tldr_queue_primary = 0, tldr_queue_advisory = 0;
  int actionable_lanes = 0;
  if (rows->count > 0 &&
      fuzz_all_coverage_queue_build(rows, &tldr_queue_rows,
                                    &tldr_queue_primary,
                                    &tldr_queue_advisory)) {
    actionable_lanes = tldr_queue_rows.count;
  }
  string_list_free(&tldr_queue_rows);
  (void)sb_append(&md, "- Coverage: ");
  md_append_code(&md, coverage_state);
  (void)sb_appendf(&md,
                   "; %d/%d lanes (%.2f%%); not-run %d "
                   "(actionable %d; disabled %d; budget-short %d; "
                   "missing-tool %d); failed %d.\n",
                   ran_lanes, lanes,
                   fuzz_all_ratio_percent((double)ran_lanes, (double)lanes),
                   not_run_lanes, actionable_lanes, disabled_lanes,
                   budget_short_lanes, missing_tool_lanes, failed_lanes);
  int campaign_reports_considered =
      reports_considered - companion_reports_considered;
  if (campaign_reports_considered < 0) campaign_reports_considered = 0;
  (void)sb_appendf(&md,
                   "- Evidence: %d campaign report%s + %d sanitizer companion%s; %.0f subcase rows, %.0f sub-failures.\n",
                   campaign_reports_considered,
                   campaign_reports_considered == 1 ? "" : "s",
                   companion_reports_considered,
                   companion_reports_considered == 1 ? "" : "s",
                   checked_subcases, sub_failures);
  (void)sb_appendf(&md, "- Gate: %s; %d coverage blockers; %d advisory gaps.\n",
                   (blocker_gaps == 0 && failed_lanes == 0) ? "`ready`" : "`blocked`",
                   blocker_gaps, advisory_gaps);
  if (latest_report_advisory_gaps > 0 ||
      latest_report_companion_skipped_lanes > 0) {
    (void)sb_appendf(&md,
                     "- Latest report companion skips: %d advisory gaps, %d skipped companion lanes.\n",
                     latest_report_advisory_gaps,
                     latest_report_companion_skipped_lanes);
  }
  if (next && next->lane[0]) {
    (void)sb_append(&md, "- Next evidence: ");
    md_append_code(&md, next->lane);
    (void)sb_append(&md, " ");
    md_append_code(&md, next->severity[0] ? next->severity : "medium");
    if (next->category[0]) {
      (void)sb_append(&md, " ");
      md_append_code(&md, next->category);
    }
    if (next->reason[0]) {
      (void)sb_append(&md, "; reason ");
      md_append_code(&md, next->reason);
    }
    if (next->guarded_command[0]) {
      (void)sb_append(&md, "; guarded ");
      md_append_code(&md, next->guarded_command);
    } else if (next->command[0]) {
      (void)sb_append(&md, "; command ");
      md_append_code(&md, next->command);
    }
    if (next->low_cpu_command[0]) {
      (void)sb_append(&md, "; low-cpu ");
      md_append_code(&md, next->low_cpu_command);
    }
    (void)sb_append(&md, ".\n");
    if (next->state_label[0] || next->state_command[0]) {
      (void)sb_append(&md, "- Next evidence state: ");
      md_append_code(&md, next->state_label[0] ? next->state_label :
                                                "missing");
      if (next->state_event[0]) {
        (void)sb_append(&md, "/");
        md_append_code(&md, next->state_event);
      }
      if (next->state_age_seconds >= 0.0)
        (void)sb_appendf(&md, "; age %.0fs", next->state_age_seconds);
      (void)sb_append(&md, next->state_live ? "; live" : "; not-live");
      (void)sb_append(&md, next->state_fresh ? "; fresh" : "; stale");
      if (!next->state_fresh && next->state_stale_reason[0]) {
        (void)sb_append(&md, " ");
        md_append_code(&md, next->state_stale_reason);
      }
      if (next->state_last_report[0]) {
        (void)sb_append(&md, "; last ");
        md_append_code(&md, next->state_last_report);
      }
      if (next->state_command[0]) {
        (void)sb_append(&md, "; inspect ");
        md_append_code(&md, next->state_command);
      }
      (void)sb_append(&md, ".\n");
    }
    if (fuzz_all_coverage_next_state_refresh_required(next)) {
      (void)sb_append(&md, "- Next evidence state refresh: ");
      md_append_code(&md, fuzz_all_coverage_next_state_refresh_reason(next));
      if (next->state_refresh_command[0]) {
        (void)sb_append(&md, "; command ");
        md_append_code(&md, next->state_refresh_command);
      }
      (void)sb_append(&md, ".\n");
    }
  }
  if (rows->count > 0) {
    string_list_t queue_rows = {0};
    int queue_non_advisory = 0, queue_advisory = 0;
    (void)fuzz_all_coverage_queue_build(rows, &queue_rows,
                                        &queue_non_advisory,
                                        &queue_advisory);
    char *queue_lanes = fuzz_all_coverage_queue_lanes_string(&queue_rows);
    if (queue_rows.count > 0) {
      (void)sb_append(&md, "\n## Coverage Queue\n\n");
      (void)sb_append(&md, "- Order: ");
      md_append_inline(&md, queue_lanes && *queue_lanes ? queue_lanes : "");
      (void)sb_append(&md, ".\n");
      (void)sb_appendf(&md, "- Counts: %d primary, %d advisory.\n",
                       queue_non_advisory, queue_advisory);
    }
    (void)sb_append(&md, "\n## Coverage Backlog\n\n");
    const string_list_t *display_rows =
        queue_rows.count > 0 ? &queue_rows : rows;
    int shown_count = 0;
    for (int i = 0; i < display_rows->count; ++i) {
      if (shown_count >= 24) break;
      char *category = json_string_or_empty(display_rows->items[i], "category");
      char *severity = json_string_or_empty(display_rows->items[i], "severity");
      char *lane = json_string_or_empty(display_rows->items[i], "lane");
      char *reason = json_string_or_empty(display_rows->items[i], "reason");
      char *command = json_string_or_empty(display_rows->items[i], "command");
      (void)sb_append(&md, "- ");
      md_append_code(&md, lane);
      (void)sb_append(&md, " ");
      md_append_code(&md, severity);
      (void)sb_append(&md, " ");
      md_append_code(&md, category);
      (void)sb_append(&md, ": ");
      md_append_inline(&md, reason);
      if (command && *command) {
        (void)sb_append(&md, "\n  Command: ");
        md_append_code(&md, command);
      }
      bool same_next_lane =
          next && next->lane[0] && lane && strcmp(next->lane, lane) == 0;
      bool same_next_command =
          next && next->command[0] && command && *command &&
          strcmp(next->command, command) == 0;
      if (same_next_lane || same_next_command) {
        if (next->guarded_command[0]) {
          (void)sb_append(&md, "\n  Guarded: ");
          md_append_code(&md, next->guarded_command);
        }
        if (next->low_cpu_command[0]) {
          (void)sb_append(&md, "\n  Low CPU: ");
          md_append_code(&md, next->low_cpu_command);
        }
        if (next->preview_command[0]) {
          (void)sb_append(&md, "\n  Preview: ");
          md_append_code(&md, next->preview_command);
        }
        if (next->state_command[0]) {
          (void)sb_append(&md, "\n  State: ");
          md_append_code(&md, next->state_command);
          if (next->state_refresh_command[0]) {
            (void)sb_append(&md, "; refresh ");
            md_append_code(&md, next->state_refresh_command);
          }
          if (next->state_label[0]) {
            (void)sb_append(&md, " (");
            md_append_inline(&md, next->state_label);
            if (next->state_event[0]) {
              (void)sb_append(&md, "/");
              md_append_inline(&md, next->state_event);
            }
            if (next->state_age_seconds >= 0.0)
              (void)sb_appendf(&md, "; age %.0fs", next->state_age_seconds);
            (void)sb_append(&md, next->state_live ? "; live" : "; not-live");
            (void)sb_append(&md, next->state_fresh ? "; fresh" : "; stale");
            (void)sb_append(&md, ")");
          }
        }
        if (next->stop_command[0]) {
          (void)sb_append(&md, "\n  Pause: ");
          md_append_code(&md, next->stop_command);
          if (next->resume_command[0]) {
            (void)sb_append(&md, "; resume ");
            md_append_code(&md, next->resume_command);
          }
        }
      }
      (void)sb_append(&md, "\n");
      ++shown_count;
      free(category);
      free(severity);
      free(lane);
      free(reason);
      free(command);
    }
    if (display_rows->count > shown_count) {
      (void)sb_appendf(&md, "- ... %d more unique lanes in ",
                       display_rows->count - shown_count);
      md_append_code(&md, coverage_json_rel);
      (void)sb_append(&md, ".\n");
    }
    if (rows->count > display_rows->count) {
      (void)sb_appendf(&md,
                       "- JSON detail rows: %d total skipped/gap observations.\n",
                       rows->count);
    }
    free(queue_lanes);
    string_list_free(&queue_rows);
  }
  (void)sb_append(&md, "\n## Next\n\n```bash\n");
  if (next && next->preview_command[0])
    (void)sb_appendf(&md, "%s\n", next->preview_command);
  if (next && next->low_cpu_command[0])
    (void)sb_appendf(&md, "%s\n", next->low_cpu_command);
  if (next && next->guarded_command[0])
    (void)sb_appendf(&md, "%s\n", next->guarded_command);
  (void)sb_append(&md, "```\n");

  if (next && (next->state_command[0] ||
               next->state_refresh_command[0] ||
               next->stop_command[0] ||
               next->resume_command[0])) {
    (void)sb_append(&md, "\n## Controls\n\n```bash\n");
    if (next->state_refresh_command[0])
      (void)sb_appendf(&md, "%s\n", next->state_refresh_command);
    if (next->state_command[0])
      (void)sb_appendf(&md, "%s\n", next->state_command);
    if (next->stop_command[0])
      (void)sb_appendf(&md, "%s\n", next->stop_command);
    if (next->resume_command[0])
      (void)sb_appendf(&md, "%s\n", next->resume_command);
    (void)sb_append(&md, "```\n");
  }

  (void)sb_append(&md, "\n## Refresh\n\n```bash\n");
  if (aggregate_history) {
    (void)sb_appendf(&md,
                     "./build/nynth fuzz all coverage%s --history %s --target-thread-years %s --hours %s --threads %s --profile %s --json %s --markdown %s\n",
                     strict ? " --strict" : "",
                     history_rel && *history_rel ? history_rel :
                                                    "build/fuzz/all/history.json",
                     target_thread_years && *target_thread_years ?
                         target_thread_years : "10",
                     hours_per_run && *hours_per_run ? hours_per_run : "8",
                     threads && *threads ? threads : NYNTH_DEFAULT_FUZZ_THREADS,
                     profile && *profile ? profile : "insane",
                     coverage_json_rel && *coverage_json_rel ?
                         coverage_json_rel : "build/fuzz/all/coverage.json",
                     coverage_md_rel && *coverage_md_rel ?
                         coverage_md_rel : "build/fuzz/all/coverage.md");
  } else {
    (void)sb_appendf(&md,
                     "./build/nynth fuzz all coverage%s --report %s --target-thread-years %s --hours %s --threads %s --profile %s --json %s --markdown %s\n",
                     strict ? " --strict" : "",
                     report_rel && *report_rel ? report_rel :
                                                  "build/fuzz/all/latest.json",
                     target_thread_years && *target_thread_years ?
                         target_thread_years : "10",
                     hours_per_run && *hours_per_run ? hours_per_run : "8",
                     threads && *threads ? threads : NYNTH_DEFAULT_FUZZ_THREADS,
                     profile && *profile ? profile : "insane",
                     coverage_json_rel && *coverage_json_rel ?
                         coverage_json_rel : "build/fuzz/all/coverage.json",
                     coverage_md_rel && *coverage_md_rel ?
                         coverage_md_rel : "build/fuzz/all/coverage.md");
  }
  (void)sb_appendf(
      &md,
      "./build/nynth fuzz all status --refresh --strict --allow-full-pressure-remediation --dir %s --history %s --worklist %s --coverage %s --plan %s --target-thread-years %s --hours %s --threads %s --profile %s --json %s --markdown %s\n",
      coverage_dir && *coverage_dir ? coverage_dir : "build/fuzz/all",
      history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json",
      worklist_rel && *worklist_rel ? worklist_rel : "build/fuzz/all/worklist.json",
      coverage_json_rel && *coverage_json_rel ?
          coverage_json_rel : "build/fuzz/all/coverage.json",
      plan_rel && *plan_rel ? plan_rel : "build/fuzz/all/plan.json",
      target_thread_years && *target_thread_years ? target_thread_years : "10",
      hours_per_run && *hours_per_run ? hours_per_run : "8",
      threads && *threads ? threads : NYNTH_DEFAULT_FUZZ_THREADS,
      profile && *profile ? profile : "insane",
      status_rel && *status_rel ? status_rel : "build/fuzz/all/status.json",
      status_md_rel && *status_md_rel ? status_md_rel : "build/fuzz/all/status.md");
  (void)sb_append(&md, "```\n");
  bool ok = md.data && write_file_text(markdown_path, md.data);
  free(md.data);
  free(report_rel);
  free(coverage_json_rel);
  free(coverage_md_rel);
  free(coverage_dir);
  free(history_rel);
  free(worklist_rel);
  free(plan_rel);
  free(status_rel);
  free(status_md_rel);
  return ok;
}

static bool coverage_lane_ran(const string_list_t *ran, const char *name) {
  return name && *name && string_list_contains(ran, name);
}

static bool coverage_group_skip_covered(const string_list_t *covered,
                                        const char *name) {
  if (!covered || !name || !*name) return false;
  if (strcmp(name, "synth_lanes") == 0) {
    return coverage_lane_ran(covered, "synth_mixed") &&
           coverage_lane_ran(covered, "synth_ir") &&
           coverage_lane_ran(covered, "synth_stress") &&
           coverage_lane_ran(covered, "synth_pure") &&
           coverage_lane_ran(covered, "campaign_core");
  }
  if (strcmp(name, "nytrix_sanitizers") == 0) {
    return coverage_lane_ran(covered, "nytrix_asan") &&
           coverage_lane_ran(covered, "nytrix_ubsan");
  }
  if (strcmp(name, "afl") == 0) {
    return coverage_lane_ran(covered, "afl_compiler") &&
           coverage_lane_ran(covered, "afl_parsers");
  }
  return false;
}

static char *fuzz_all_dup_path_arg(const char *path) {
  if (!path || !*path) return NULL;
  if (path_is_absolute(path)) return strdup(path);
  char *out = NULL;
  (void)nynth_asprintf(&out, "%s", path);
  return out;
}

static int cmd_public_fuzz_all_coverage(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *report_arg = value_after_equals(argc, argv, 4, "--report", "");
  if (!report_arg || !*report_arg)
    report_arg = value_after_equals(argc, argv, 4, "--input", "");
  const char *history_arg = value_after_equals(argc, argv, 4, "--history", "build/fuzz/all/history.json");
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *markdown_path = value_after_equals(argc, argv, 4, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after_equals(argc, argv, 4, "--md", "");
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
                                               NYNTH_DEFAULT_FUZZ_THREADS);
  const char *profile_arg = value_after_equals(argc, argv, 4, "--profile", "insane");
  bool strict = has_flag_after(argc, argv, 4, "--strict");
  bool explicit_report = report_arg && *report_arg;
  bool aggregate_history = !explicit_report && history_arg && *history_arg;
  char *report_path = explicit_report ? fuzz_all_dup_path_arg(report_arg) : NULL;
  char *history_path = aggregate_history ? fuzz_all_dup_path_arg(history_arg) : NULL;
  char *source_path = explicit_report ? fuzz_all_dup_path_arg(report_arg)
                                      : fuzz_all_dup_path_arg(history_arg);
  char *latest_report_path = NULL;
  char *coverage_json_rel =
      rel_path_dup(root, json_path ? json_path : "");
  char *coverage_md_rel =
      rel_path_dup(root, markdown_path ? markdown_path : "");
  char *coverage_dir =
      path_parent_dup(coverage_json_rel && *coverage_json_rel ?
                          coverage_json_rel :
                          (coverage_md_rel && *coverage_md_rel ?
                               coverage_md_rel : "build/fuzz/all/coverage.json"),
                      "build/fuzz/all");
  char *focus_source_dir =
      path_parent_dup(aggregate_history && history_path && *history_path ?
                          history_path :
                          (report_path && *report_path ? report_path :
                              (coverage_dir && *coverage_dir ? coverage_dir :
                                   "build/fuzz/all")),
                      "build/fuzz/all");
  char *focus_parent_dir =
      path_parent_dup(focus_source_dir && *focus_source_dir ? focus_source_dir :
                          "build/fuzz/all",
                      "build/fuzz");
  char *focus_dir = path_child_dup(focus_parent_dir, "ultra");

  string_list_t rows = {0}, failures = {0}, report_rows = {0}, history_rows = {0};
  string_list_t ran = {0}, skipped = {0}, failed = {0}, covered = {0};
  string_list_t filtered = {0}, detail_seen = {0};
  int lanes = 0, ran_lanes = 0, skipped_lanes = 0, required_lanes = 0;
  int failed_lanes = 0, disabled_lanes = 0, budget_short_lanes = 0;
  int missing_tool_lanes = 0, gap_count = 0, blocker_gaps = 0;
  int detail_rows = 0;
  int latest_report_advisory_gaps = 0, latest_report_companion_skipped_lanes = 0;
  int reports_considered = 0, companion_reports_considered = 0;
  double checked_subcases = 0.0, sub_failures = 0.0;
  if (explicit_report) {
    file_buf_t report = {0};
    if (!report_path || !read_file(report_path, &report) || !report.data) {
      (void)string_list_push_take(&failures,
                                  make_fuzz_failure(root, "fuzz-all-coverage",
                                                    "all-run report not readable",
                                                    report_path ? report_path : ""));
    } else if (!collect_rows_from_report_json(report.data, &report_rows)) {
      (void)string_list_push_take(&failures,
                                  make_fuzz_failure(root, "fuzz-all-coverage",
                                                    "all-run report missing rows",
                                                    report_path ? report_path : ""));
    } else {
      reports_considered = 1;
    }
    free(report.data);
  } else if (aggregate_history) {
    file_buf_t hist = {0};
    if (!history_path || !read_file(history_path, &hist) || !hist.data) {
      (void)string_list_push_take(&failures,
                                  make_fuzz_failure(root, "fuzz-all-coverage",
                                                    "campaign history not readable",
                                                    history_path ? history_path : ""));
    } else {
      char *latest = summary_string_from_report(hist.data, "latest_report");
      latest_report_path = fuzz_all_dup_path_arg(latest);
      free(latest);
      if (collect_rows_from_report_json(hist.data, &history_rows)) {
        for (int i = 0; i < history_rows.count; ++i) {
          char *kind = json_string_or_empty(history_rows.items[i], "kind");
          if (kind && strcmp(kind, "fuzz-all-history") == 0) {
            char *candidate = json_string_or_empty(history_rows.items[i], "report");
            char *candidate_path = fuzz_all_dup_path_arg(candidate);
            file_buf_t report = {0};
            if (candidate_path && read_file(candidate_path, &report) && report.data &&
                collect_rows_from_report_json(report.data, &report_rows)) {
              ++reports_considered;
            }
            free(report.data);
            free(candidate_path);
            free(candidate);
          }
          free(kind);
        }
      }
      char *scan_dir_rel = summary_string_from_report(hist.data, "scan_dir");
      char *scan_dir_path = fuzz_all_dup_path_arg(scan_dir_rel);
      if (scan_dir_path && *scan_dir_path) {
        string_list_t companion_files = {0};
        if (collect_regular_files_recursive(scan_dir_path, &companion_files)) {
          qsort(companion_files.items, (size_t)companion_files.count, sizeof(char *), cmp_cstr);
          for (int ci = 0; ci < companion_files.count; ++ci) {
            if (!ny_has_suffix(companion_files.items[ci], ".json")) continue;
            file_buf_t companion = {0};
            if (read_file(companion_files.items[ci], &companion) && companion.data &&
                report_is_fuzz_sanitizers(companion.data) &&
                collect_rows_from_report_json(companion.data, &report_rows)) {
              ++reports_considered;
              ++companion_reports_considered;
            }
            free(companion.data);
          }
        }
        string_list_free(&companion_files);
      }
      free(scan_dir_path);
      free(scan_dir_rel);
      if (focus_dir && *focus_dir) {
        for (size_t fi = 0;
             fi < sizeof(FUZZ_ALL_FOCUS_COMPANIONS) /
                      sizeof(FUZZ_ALL_FOCUS_COMPANIONS[0]);
             ++fi) {
          (void)coverage_add_focus_companion_report(
              root, focus_dir, &FUZZ_ALL_FOCUS_COMPANIONS[fi], &report_rows,
              &rows, &reports_considered, &companion_reports_considered);
        }
      }
      if (reports_considered == 0 && latest_report_path) {
        file_buf_t report = {0};
        if (read_file(latest_report_path, &report) && report.data &&
            collect_rows_from_report_json(report.data, &report_rows)) {
          ++reports_considered;
        }
        free(report.data);
      }
      if (reports_considered == 0) {
        (void)string_list_push_take(&failures,
                                    make_fuzz_failure(root, "fuzz-all-coverage",
                                                      "history contains no readable all-run reports",
                                                      history_path ? history_path : ""));
      }
    }
    free(hist.data);
  } else {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-coverage",
                                                  "coverage requires --report or --history",
                                                  ""));
  }

  for (int i = 0; i < report_rows.count; ++i) {
    const char *row = report_rows.items[i];
    char *name = json_string_or_empty(row, "name");
    bool ok = json_bool_range(row, row + strlen(row), "ok", false);
    bool is_skipped = json_bool_range(row, row + strlen(row), "skipped", false);
    bool dry_run = json_bool_range(row, row + strlen(row), "dry_run", false);
    if (ok && !is_skipped && !dry_run && name && *name)
      (void)string_list_push_unique_copy(&covered, name);
    free(name);
  }

  for (int i = 0; i < report_rows.count; ++i) {
    const char *row = report_rows.items[i];
    char *name = json_string_or_empty(row, "name");
    char *phase = json_string_or_empty(row, "phase");
    char *reason = json_string_or_empty(row, "reason");
    bool ok = json_bool_range(row, row + strlen(row), "ok", false);
    bool required = json_bool_range(row, row + strlen(row), "required", false);
    bool is_skipped = json_bool_range(row, row + strlen(row), "skipped", false);
    bool dry_run = json_bool_range(row, row + strlen(row), "dry_run", false);
    double value = 0.0;
    if (dry_run) {
      free(name);
      free(phase);
      free(reason);
      continue;
    }
    if (is_skipped && (coverage_lane_ran(&covered, name) ||
                       coverage_group_skip_covered(&covered, name))) {
      free(name);
      free(phase);
      free(reason);
      continue;
    }
    bool is_filtered_skip =
        is_skipped && reason && strstr(reason, "filtered by --only-lane");
    if (is_filtered_skip) {
      if (name && *name) (void)string_list_push_unique_copy(&filtered, name);
      coverage_add_detail_unique(
          phase, "advisory", name,
          reason && *reason ? reason : "lane filtered",
          coverage_detail_command_for(name, phase, reason),
          &rows, &detail_seen, &detail_rows);
      free(name);
      free(phase);
      free(reason);
      continue;
    }
    ++lanes;
    if (required) ++required_lanes;
    if (json_number_range(row, row + strlen(row), "sub_rows", &value) && value > 0.0)
      checked_subcases += value;
    value = 0.0;
    if (json_number_range(row, row + strlen(row), "sub_failures", &value) && value > 0.0)
      sub_failures += value;
    if (is_skipped) {
      ++skipped_lanes;
      if (name && *name) (void)string_list_push_unique_copy(&skipped, name);
      if (reason && strncmp(reason, "disabled by", 11) == 0) ++disabled_lanes;
      if (reason && strstr(reason, "budget too short")) ++budget_short_lanes;
      if (reason && strstr(reason, "not found")) ++missing_tool_lanes;
      const char *skip_severity =
        (phase && (strcmp(phase, "nytrix") == 0 || strcmp(phase, "sanitizer") == 0))
          ? "advisory" : "medium";
      coverage_add_detail_unique(
          phase, skip_severity, name,
          reason && *reason ? reason : "lane skipped",
          coverage_detail_command_for(name, phase, reason),
          &rows, &detail_seen, &detail_rows);
      if (!coverage_skip_has_expected_replacement(name))
        coverage_add_gap(phase, skip_severity, name, reason && *reason ? reason : "lane skipped",
                         "", &rows, &gap_count, &blocker_gaps);
    } else {
      ++ran_lanes;
      if (name && *name) (void)string_list_push_unique_copy(&ran, name);
    }
    if (!ok && (!name || !*name || !coverage_lane_ran(&covered, name))) {
      ++failed_lanes;
      if (name && *name) (void)string_list_push_unique_copy(&failed, name);
      coverage_add_gap(phase, "critical", name, "lane failed",
                       "", &rows, &gap_count, &blocker_gaps);
    }
    free(name);
    free(phase);
    free(reason);
  }

  for (size_t i = 0; i < sizeof(FUZZ_ALL_EXPECTED_LANES) / sizeof(FUZZ_ALL_EXPECTED_LANES[0]); ++i) {
    if (!coverage_lane_ran(&ran, FUZZ_ALL_EXPECTED_LANES[i].name) &&
        !coverage_lane_ran(&filtered, FUZZ_ALL_EXPECTED_LANES[i].name)) {
      coverage_add_gap(FUZZ_ALL_EXPECTED_LANES[i].category, FUZZ_ALL_EXPECTED_LANES[i].severity,
                       FUZZ_ALL_EXPECTED_LANES[i].name, FUZZ_ALL_EXPECTED_LANES[i].reason,
                       FUZZ_ALL_EXPECTED_LANES[i].command, &rows, &gap_count, &blocker_gaps);
    }
  }
  if (!coverage_lane_ran(&ran, "nytrix_fuzz"))
    coverage_add_gap("nytrix", "advisory", "nytrix_fuzz",
                     "Nytrix-owned fuzz lane did not run",
                     "./build/nynth fuzz all run --profile insane --hours 8 --threads 25% --dir build/fuzz/all --allow-nytrix --json build/fuzz/all/with-nytrix.json",
                     &rows, &gap_count, &blocker_gaps);
  if (!coverage_lane_ran(&ran, "nytrix_asan") || !coverage_lane_ran(&ran, "nytrix_ubsan"))
    coverage_add_gap("sanitizer", "advisory", "nytrix_sanitizers",
                     "Nytrix sanitizer lanes did not run",
                     "./build/nynth fuzz all run --profile insane --hours 8 --threads 25% --dir build/fuzz/all --allow-nytrix --json build/fuzz/all/with-sanitizers.json",
                     &rows, &gap_count, &blocker_gaps);

  if (explicit_report) {
    coverage_count_latest_report_advisory(&report_rows, &latest_report_advisory_gaps,
                                          &latest_report_companion_skipped_lanes);
  } else if (latest_report_path && *latest_report_path) {
    file_buf_t latest_file = {0};
    string_list_t latest_rows = {0};
    if (read_file(latest_report_path, &latest_file) && latest_file.data &&
        collect_rows_from_report_json(latest_file.data, &latest_rows)) {
      coverage_count_latest_report_advisory(&latest_rows, &latest_report_advisory_gaps,
                                            &latest_report_companion_skipped_lanes);
    }
    string_list_free(&latest_rows);
    free(latest_file.data);
  }

  if (strict && blocker_gaps > 0)
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-coverage",
                                                  "full-pressure coverage gaps present",
                                                  source_path ? source_path : ""));
  int advisory_gaps = gap_count - blocker_gaps;
  if (advisory_gaps < 0) advisory_gaps = 0;
  fuzz_all_coverage_next_summary_t coverage_next = {0};
  fuzz_all_coverage_select_next(&rows, coverage_dir, target_arg, hours_arg,
                                threads_arg, profile_arg, &coverage_next);
  fuzz_all_coverage_next_load_state(root, &coverage_next);
  string_list_t coverage_queue = {0};
  string_list_t ordered_rows = {0};
  int coverage_queue_non_advisory = 0, coverage_queue_advisory = 0;
  if (!fuzz_all_coverage_queue_build(&rows, &coverage_queue,
                                     &coverage_queue_non_advisory,
                                     &coverage_queue_advisory)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-coverage",
                                                  "coverage queue build failed",
                                                  source_path ? source_path : ""));
  }
  if (coverage_queue.count > 0 &&
      !fuzz_all_coverage_rows_by_queue(&rows, &coverage_queue,
                                       &ordered_rows)) {
    string_list_free(&ordered_rows);
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-coverage",
                                                  "coverage row prioritization failed",
                                                  source_path ? source_path : ""));
  }
  char *coverage_queue_lanes =
      fuzz_all_coverage_queue_lanes_string(&coverage_queue);
  int coverage_backlog_count =
      coverage_queue.count > 0 ? coverage_queue.count : skipped.count;
  if (markdown_path && *markdown_path &&
      !write_fuzz_all_coverage_markdown(root, markdown_path, json_path,
                                        source_path, aggregate_history, strict,
                                        target_arg, hours_arg, threads_arg,
                                        profile_arg, lanes, ran_lanes,
                                        failed_lanes, disabled_lanes,
                                        budget_short_lanes, missing_tool_lanes,
                                        blocker_gaps, advisory_gaps,
                                        reports_considered,
                                        companion_reports_considered,
                                        latest_report_advisory_gaps,
                                        latest_report_companion_skipped_lanes,
                                        checked_subcases, sub_failures,
                                        &coverage_next, &rows)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-coverage",
                                                  "coverage markdown write failed",
                                                  markdown_path));
  }

  str_buf_t extra = {0};
  int campaign_reports_considered =
      reports_considered - companion_reports_considered;
  if (campaign_reports_considered < 0) campaign_reports_considered = 0;
  (void)sb_append(&extra, ",\"source_report\":");
  append_rel_json_str(&extra, root, source_path ? source_path : "");
  (void)sb_append(&extra, ",\"coverage_scope\":");
  (void)sb_append_json_str(&extra, aggregate_history ? "history" : "single-report");
  (void)sb_appendf(&extra,
                   ",\"reports_considered\":%d,"
                   "\"campaign_reports_considered\":%d,"
                   "\"companion_reports_considered\":%d,"
                   "\"coverage_reports_considered\":%d,"
                   "\"coverage_campaign_reports_considered\":%d,"
                   "\"coverage_companion_reports_considered\":%d",
                   reports_considered, campaign_reports_considered,
                   companion_reports_considered,
                   reports_considered, campaign_reports_considered,
                   companion_reports_considered);
  if (aggregate_history) {
    (void)sb_append(&extra, ",\"source_history\":");
    append_rel_json_str(&extra, root, history_path ? history_path : "");
    (void)sb_append(&extra, ",\"latest_report\":");
    append_rel_json_str(&extra, root, latest_report_path ? latest_report_path : "");
  }
  if (markdown_path && *markdown_path) {
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, root, markdown_path);
  }
  (void)sb_append(&extra, ",\"coverage_state\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_coverage_state((double)lanes, (double)ran_lanes,
                                      (double)blocker_gaps,
                                      (double)failed_lanes));
  (void)sb_appendf(&extra,
                   ",\"lanes\":%d,\"ran_lanes\":%d,\"skipped_lanes\":%d,"
                   "\"required_lanes\":%d,\"failed_lanes\":%d,"
                   "\"disabled_lanes\":%d,\"budget_short_lanes\":%d,"
                   "\"missing_tool_lanes\":%d,"
                   "\"coverage_lanes\":%d,\"coverage_ran_lanes\":%d,"
                   "\"coverage_skipped_lanes\":%d,"
                   "\"coverage_required_lanes\":%d,"
                   "\"coverage_failed_lanes\":%d,"
                   "\"coverage_disabled_lanes\":%d,"
                   "\"coverage_budget_short_lanes\":%d,"
                   "\"coverage_missing_tool_lanes\":%d,"
                   "\"coverage_depth_percent\":%.2f,"
                   "\"coverage_percent\":%.2f,"
                   "\"coverage_not_run_lanes\":%.0f,"
                   "\"coverage_detail_rows\":%d,"
                   "\"coverage_skipped_unique_lanes\":%d,"
                   "\"coverage_backlog_lanes\":%d,"
                   "\"coverage_detail_count\":%d,"
                   "\"checked_subcases\":%.0f,\"sub_failures_total\":%.0f,"
                   "\"coverage_gaps\":%d,\"blocker_gaps\":%d,"
                   "\"coverage_blocker_gaps\":%d,"
                   "\"advisory_gaps\":%d,"
                   "\"coverage_advisory_gaps\":%d,"
                   "\"latest_report_advisory_gaps\":%d,"
                   "\"coverage_latest_report_advisory_gaps\":%d,"
                   "\"latest_report_companion_skipped_lanes\":%d,"
                   "\"coverage_latest_report_companion_skipped_lanes\":%d,"
                   "\"strict\":%s,\"full_pressure_ready\":%s",
                   lanes, ran_lanes, skipped_lanes, required_lanes, failed_lanes,
                   disabled_lanes, budget_short_lanes, missing_tool_lanes,
                   lanes, ran_lanes, skipped_lanes, required_lanes, failed_lanes,
                   disabled_lanes, budget_short_lanes, missing_tool_lanes,
                   fuzz_all_ratio_percent((double)ran_lanes, (double)lanes),
                   fuzz_all_ratio_percent((double)ran_lanes, (double)lanes),
                   fuzz_all_not_run_lanes((double)ran_lanes, (double)lanes),
                   detail_rows, skipped.count, coverage_backlog_count,
                   coverage_backlog_count,
                   checked_subcases, sub_failures, gap_count, blocker_gaps,
                   blocker_gaps,
                   advisory_gaps, advisory_gaps,
                   latest_report_advisory_gaps,
                   latest_report_advisory_gaps,
                   latest_report_companion_skipped_lanes,
                   latest_report_companion_skipped_lanes,
                   strict ? "true" : "false",
                   blocker_gaps == 0 && failed_lanes == 0 ? "true" : "false");
  append_fuzz_all_coverage_queue_fields(&extra, &coverage_queue,
                                        coverage_queue_non_advisory,
                                        coverage_queue_advisory,
                                        coverage_queue_lanes);
  append_fuzz_all_coverage_report_next_fields(&extra, &coverage_next);
  const string_list_t *emit_rows =
      ordered_rows.count > 0 ? &ordered_rows : &rows;
  char *out = build_native_report_json(emit_rows, &failures,
                                       "fuzz-all-coverage", extra.data);
  int rc = emit_native_report(out, json_path, "all fuzz coverage",
                              emit_rows->count, failures.count);
  free(extra.data);
  free(coverage_queue_lanes);
  free(report_path);
  free(history_path);
  free(source_path);
  free(latest_report_path);
  free(coverage_json_rel);
  free(coverage_md_rel);
  free(coverage_dir);
  free(focus_source_dir);
  free(focus_parent_dir);
  free(focus_dir);
  string_list_free(&coverage_queue);
  string_list_free(&ordered_rows);
  string_list_free(&report_rows);
  string_list_free(&history_rows);
  string_list_free(&rows);
  string_list_free(&failures);
  string_list_free(&ran);
  string_list_free(&skipped);
  string_list_free(&failed);
  string_list_free(&covered);
  string_list_free(&filtered);
  string_list_free(&detail_seen);
  return rc;
}

static bool write_fuzz_all_plan_markdown(const char *root,
                                         const char *markdown_path,
                                         const char *history_path,
                                         const char *worklist_path,
                                         const char *coverage_path,
                                         double target_years,
                                         double current_years,
                                         double remaining_years,
                                         int threads,
                                         double hours_per_run,
                                         long runs_needed,
                                         double wall_hours,
                                         double runs_per_day,
                                         double thread_years_per_day,
                                         const char *completion_eta_local,
                                         double checked_subcases,
                                         double active_items,
                                         double coverage_blocker_gaps,
                                         double coverage_backlog_lanes,
                                         double coverage_queue_count,
                                         double coverage_queue_non_advisory_count,
                                         double coverage_queue_advisory_count,
                                         const char *coverage_queue_lanes,
                                         double language_score_percent,
                                         const char *language_score_label,
                                         double language_good_threshold_percent,
                                         double language_score_gap_percent,
                                         double next_run_language_score_percent,
                                         double next_run_language_score_delta_percent,
                                         double runs_to_good_language_score,
                                         double runs_to_good_language_days,
                                         bool language_evidence_fresh,
                                         double language_advisory_timeouts,
                                         bool language_cache_policy_ok,
                                         bool language_ny_bin_exists,
                                         bool language_active_old_writer,
                                         const char *coverage_next_action,
                                         const char *coverage_next_lane,
                                         const char *coverage_next_reason,
                                         const char *coverage_next_command,
                                         const char *coverage_next_state_command,
                                         const char *coverage_next_state_refresh_command,
                                         const char *coverage_next_stop_command,
                                         const char *coverage_next_resume_command,
                                         const char *plan_next_action,
                                         const char *plan_next_reason,
                                         const char *plan_next_command,
                                         const char *plan_next_low_cpu_command,
                                         const char *plan_next_preview_command,
                                         const char *handoff_preview_command,
                                         const char *handoff_command,
                                         const char *run_command,
                                         const char *refresh_command) {
  if (!markdown_path || !*markdown_path) return true;
  char *history_rel = rel_path_dup(root ? root : "", history_path ? history_path : "");
  char *worklist_rel = rel_path_dup(root ? root : "", worklist_path ? worklist_path : "");
  char *coverage_rel = rel_path_dup(root ? root : "", coverage_path ? coverage_path : "");
  char *plan_json_control_path = NULL;
  size_t plan_md_len = strlen(markdown_path);
  if (plan_md_len > 3 &&
      strcmp(markdown_path + plan_md_len - 3, ".md") == 0) {
    (void)asprintf(&plan_json_control_path, "%.*s.json",
                   (int)(plan_md_len - 3), markdown_path);
  } else {
    plan_json_control_path = path_with_suffix_ext(markdown_path, "", ".json");
  }
  char *plan_json_control_rel =
      rel_path_dup(root ? root : "", plan_json_control_path ?
                                       plan_json_control_path : "");
  char *plan_compact_jq_command = NULL;
  (void)asprintf(
      &plan_compact_jq_command,
      "jq '{ok,cases,ok_count,failure_count,campaign_percent,"
      "campaign_remaining_percent,"
      "thread_years,target_thread_years,score_percent,stability_percent,"
      "stability_score_percent,language_score_percent,language_score_label,"
      "completion_state,language_score_good_threshold_percent,"
      "language_score_signal_percent,language_score_evidence_cap_percent,"
      "language_score_note,language_score_gap_percent,"
      "next_run_language_score_percent,next_run_language_score_delta_percent,"
      "runs_to_good_language_score,runs_to_good_days,"
      "runs_to_good_language_days,days_to_good_language_score,"
      "recommended_action,recommended_reason,"
      "recommended_command,recommended_low_cpu_command,"
      "recommended_preview_command,recommended_repeat_mode,"
      "recommended_repeat_count,plan_next_action,plan_next_lane,"
      "plan_next_reason,plan_next_command,plan_next_low_cpu_command,"
      "plan_next_preview_command,coverage_percent,coverage_backlog_lanes,"
      "coverage_queue_count,coverage_queue_non_advisory_count,"
      "coverage_queue_advisory_count,coverage_queue_lanes,"
      "active_worklist_items,coverage_blocker_gaps,runs_needed,"
      "wall_days_needed,completion_eta_local}' %s",
      plan_json_control_rel && *plan_json_control_rel ?
          plan_json_control_rel : "build/fuzz/all/plan.json");
  time_t now = time(NULL);
  struct tm tm_now;
  char stamp[64] = {0};
  if (localtime_r(&now, &tm_now))
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %z", &tm_now);
  str_buf_t md = {0};
  (void)sb_append(&md, "# Nynth Campaign Plan\n\n");
  if (stamp[0]) {
    (void)sb_append(&md, "Generated: ");
    md_append_code(&md, stamp);
    (void)sb_append(&md, "\n\n");
  }
  (void)sb_append(&md, "## TLDR\n\n");
  double target_percent = target_years > 0.0 ? (current_years / target_years) * 100.0 : 0.0;
  double campaign_remaining_percent = 100.0 - target_percent;
  if (campaign_remaining_percent < 0.0) campaign_remaining_percent = 0.0;
  if (campaign_remaining_percent > 100.0) campaign_remaining_percent = 100.0;
  (void)sb_appendf(&md,
                   "- Progress: %.4f%%; %.6f/%.4f thread-years, %.6f remaining; %.0f checked rows.\n",
                   target_percent, current_years, target_years, remaining_years,
                   checked_subcases);
  (void)sb_appendf(&md,
                   "- Confidence: campaign evidence %.4f%%; remaining %.4f%%.\n",
                   target_percent, campaign_remaining_percent);
  (void)sb_appendf(&md,
                   "- Language: %.2f%% `%s`; good >= %.2f%%; gap %.2f%%; next run %.2f%% (%+.2f%%)",
                   language_score_percent,
                   language_score_label && *language_score_label ?
                       language_score_label : "unknown",
                   language_good_threshold_percent, language_score_gap_percent,
                   next_run_language_score_percent,
                   next_run_language_score_delta_percent);
  if (runs_to_good_language_score > 0.0) {
    (void)sb_appendf(&md, "; good score in %.0f runs",
                     runs_to_good_language_score);
    if (runs_to_good_language_days >= 0.0)
      (void)sb_appendf(&md, " / %.2f days", runs_to_good_language_days);
  } else if (runs_to_good_language_score == 0.0) {
    (void)sb_append(&md, "; good score already reached");
  } else {
    (void)sb_append(&md, "; good score not projectable");
  }
  (void)sb_appendf(&md, "; evidence `%s`.\n",
                   language_evidence_fresh ? "fresh" : "stale");
  (void)sb_appendf(&md,
                   "- Score guards: advisory timeouts %.0f; cache `%s`; ny `%s`; old writer `%s`.\n",
                   language_advisory_timeouts,
                   language_cache_policy_ok ? "ok" : "bad",
                   language_ny_bin_exists ? "ok" : "missing",
                   language_active_old_writer ? "present" : "none");
  (void)sb_appendf(&md,
                   "- Gate: %.0f active items; %.0f coverage blockers; %.0f coverage backlog lanes.\n",
                   active_items, coverage_blocker_gaps,
                   coverage_backlog_lanes < 0.0 ? 0.0 : coverage_backlog_lanes);
  if (plan_next_action && *plan_next_action) {
    (void)sb_append(&md, "- Recommended: ");
    md_append_code(&md, plan_next_action);
    if (plan_next_reason && *plan_next_reason) {
      (void)sb_append(&md, "; reason ");
      md_append_code(&md, plan_next_reason);
    }
    if (plan_next_command && *plan_next_command) {
      (void)sb_append(&md, "; command ");
      md_append_code(&md, plan_next_command);
    }
    (void)sb_append(&md, ".\n");
    if (plan_next_preview_command && *plan_next_preview_command) {
      (void)sb_append(&md, "- Preview recommended: ");
      md_append_code(&md, plan_next_preview_command);
      (void)sb_append(&md, ".\n");
    }
  }
  if (coverage_queue_lanes && *coverage_queue_lanes) {
    (void)sb_append(&md, "- Coverage queue: ");
    md_append_inline(&md, coverage_queue_lanes);
    (void)sb_appendf(&md, "; total %.0f; primary %.0f; advisory %.0f.\n",
                     coverage_queue_count,
                     coverage_queue_non_advisory_count,
                     coverage_queue_advisory_count);
  }
  double thread_hours_total = wall_hours * (double)threads;
  (void)sb_appendf(&md,
                   "- Budget: %ld runs x %.2fh = %.2f wall-hours; x %d threads = %.2f thread-hours.\n",
                   runs_needed, hours_per_run, wall_hours, threads,
                   thread_hours_total);
  if (runs_per_day > 0.0 || thread_years_per_day > 0.0) {
    (void)sb_appendf(&md, "- Pace: %.2f runs/day, %.6f thread-years/day",
                     runs_per_day, thread_years_per_day);
    if (completion_eta_local && *completion_eta_local) {
      (void)sb_append(&md, "; ETA ");
      md_append_code(&md, completion_eta_local);
    }
    (void)sb_append(&md, ".\n");
  }
  bool has_coverage_next =
      coverage_backlog_lanes > 0.0 &&
      coverage_next_action && *coverage_next_action &&
      strcmp(coverage_next_action, "none") != 0 &&
      coverage_next_command && *coverage_next_command;
  if (remaining_years <= 0.0)
    (void)sb_append(&md, "- Next: target already met; refresh status.\n");
  else if (active_items > 0.0)
    (void)sb_append(&md, "- Next: clear active worklist before unattended runs.\n");
  else if (coverage_blocker_gaps > 0.0)
    (void)sb_append(&md, "- Next: clear coverage blockers before unattended runs.\n");
  else if (has_coverage_next) {
    (void)sb_appendf(&md,
                     "- Next: collect missing coverage evidence before unattended campaign runs; %.0f backlog lane%s",
                     coverage_backlog_lanes,
                     coverage_backlog_lanes == 1.0 ? "" : "s");
    if (coverage_next_lane && *coverage_next_lane) {
      (void)sb_append(&md, "; lane ");
      md_append_code(&md, coverage_next_lane);
    }
    if (coverage_next_reason && *coverage_next_reason) {
      (void)sb_append(&md, "; reason ");
      md_append_code(&md, coverage_next_reason);
    }
    (void)sb_append(&md, ".\n");
  }
  (void)sb_append(&md, "- Files: history ");
  md_append_code(&md, history_rel);
  (void)sb_append(&md, "; worklist ");
  md_append_code(&md, worklist_rel);
  if (coverage_path && *coverage_path) {
    (void)sb_append(&md, "; coverage ");
    md_append_code(&md, coverage_rel);
  }
  (void)sb_append(&md, ".\n");
  bool clear_to_run = runs_needed > 0 &&
                      active_items <= 0.0 &&
                      coverage_blocker_gaps <= 0.0;
  (void)sb_append(&md, "\n## Next\n\n```bash\n");
  if (has_coverage_next) {
    if (plan_next_preview_command && *plan_next_preview_command) {
      (void)sb_append(&md, plan_next_preview_command);
      (void)sb_append(&md, "\n");
    }
    if (plan_next_low_cpu_command && *plan_next_low_cpu_command) {
      (void)sb_append(&md, plan_next_low_cpu_command);
      (void)sb_append(&md, "\n");
    }
    if (plan_next_command && *plan_next_command) {
      (void)sb_append(&md, plan_next_command);
      (void)sb_append(&md, "\n");
    }
  } else if (clear_to_run && plan_next_preview_command &&
             *plan_next_preview_command) {
    (void)sb_append(&md, plan_next_preview_command);
    (void)sb_append(&md, "\n");
    if (plan_next_command && *plan_next_command) {
      (void)sb_append(&md, plan_next_command);
      (void)sb_append(&md, "\n");
    }
  } else if (clear_to_run && plan_next_command && *plan_next_command) {
    (void)sb_append(&md, plan_next_command);
    (void)sb_append(&md, "\n");
  } else if (clear_to_run && run_command && *run_command) {
    (void)sb_append(&md, run_command);
    (void)sb_append(&md, "\n");
  } else if (refresh_command && *refresh_command) {
    (void)sb_append(&md, refresh_command);
    (void)sb_append(&md, "\n");
  }
  (void)sb_append(&md, "```\n");
  bool has_coverage_controls =
      has_coverage_next &&
      ((coverage_next_state_command && *coverage_next_state_command) ||
       (coverage_next_state_refresh_command &&
        *coverage_next_state_refresh_command) ||
       (coverage_next_stop_command && *coverage_next_stop_command) ||
       (coverage_next_resume_command && *coverage_next_resume_command));
  if ((plan_compact_jq_command && *plan_compact_jq_command) ||
      has_coverage_controls) {
    (void)sb_append(&md, "\n## Controls\n\n```bash\n");
    if (plan_compact_jq_command && *plan_compact_jq_command)
      (void)sb_appendf(&md, "%s\n", plan_compact_jq_command);
    if (coverage_next_state_refresh_command &&
        *coverage_next_state_refresh_command)
      (void)sb_appendf(&md, "%s\n", coverage_next_state_refresh_command);
    if (coverage_next_state_command && *coverage_next_state_command)
      (void)sb_appendf(&md, "%s\n", coverage_next_state_command);
    if (coverage_next_stop_command && *coverage_next_stop_command)
      (void)sb_appendf(&md, "%s\n", coverage_next_stop_command);
    if (coverage_next_resume_command && *coverage_next_resume_command)
      (void)sb_appendf(&md, "%s\n", coverage_next_resume_command);
    (void)sb_append(&md, "```\n");
  }
  if (has_coverage_next && clear_to_run &&
      ((handoff_command && *handoff_command) ||
       (run_command && *run_command))) {
    (void)sb_append(&md, "\n## Campaign Run\n\n```bash\n");
    if (handoff_preview_command && *handoff_preview_command)
      (void)sb_appendf(&md, "%s\n", handoff_preview_command);
    if (handoff_command && *handoff_command)
      (void)sb_appendf(&md, "%s\n", handoff_command);
    else if (run_command && *run_command)
      (void)sb_appendf(&md, "%s\n", run_command);
    (void)sb_append(&md, "```\n");
  }
  bool ok = md.data && write_file_text(markdown_path, md.data);
  free(md.data);
  free(history_rel);
  free(worklist_rel);
  free(coverage_rel);
  free(plan_json_control_path);
  free(plan_json_control_rel);
  free(plan_compact_jq_command);
  return ok;
}

static int cmd_public_fuzz_all_plan(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *history_arg = value_after_equals(argc, argv, 4, "--history", "build/fuzz/all/history.json");
  const char *worklist_arg = value_after_equals(argc, argv, 4, "--worklist", "build/fuzz/all/worklist.json");
  const char *coverage_arg = value_after_equals(argc, argv, 4, "--coverage", "");
  const char *dir_arg = value_after_equals(argc, argv, 4, "--dir", "");
  if (!dir_arg || !*dir_arg)
    dir_arg = value_after_equals(argc, argv, 4, "--history-dir", "");
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *markdown_path = value_after_equals(argc, argv, 4, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after_equals(argc, argv, 4, "--md", "");
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
                                               NYNTH_DEFAULT_FUZZ_THREADS);
  const char *profile = value_after_equals(argc, argv, 4, "--profile", "insane");
  double target_years = atof(target_arg ? target_arg : "10");
  double hours_per_run = atof(hours_arg ? hours_arg : "8");
  int default_threads = gc_default_fuzz_thread_count();
  int threads = gc_parse_thread_count(threads_arg, default_threads);
  if (threads < 1) threads = 1;

  char *history_path = NULL, *worklist_path = NULL, *coverage_path = NULL;
  if (history_arg && *history_arg) {
    if (path_is_absolute(history_arg)) history_path = strdup(history_arg);
    else (void)nynth_asprintf(&history_path, "%s", history_arg);
  }
  if (worklist_arg && *worklist_arg) {
    if (path_is_absolute(worklist_arg)) worklist_path = strdup(worklist_arg);
    else (void)nynth_asprintf(&worklist_path, "%s", worklist_arg);
  }
  if (coverage_arg && *coverage_arg) {
    if (path_is_absolute(coverage_arg)) coverage_path = strdup(coverage_arg);
    else (void)nynth_asprintf(&coverage_path, "%s", coverage_arg);
  }

  file_buf_t history = {0}, worklist = {0}, coverage = {0};
  string_list_t rows = {0}, failures = {0};
  double current_years = 0.0, current_hours = 0.0, checked_subcases = 0.0;
  double campaign_calendar_span_days = 0.0;
  double campaign_calendar_age_days = 0.0;
  double active_items = -1.0, coverage_blocker_gaps = -1.0;
  double coverage_backlog_lanes = -1.0;
  double coverage_queue_count = 0.0;
  double coverage_queue_non_advisory_count = 0.0;
  double coverage_queue_advisory_count = 0.0;
  double coverage_depth_percent = -1.0;
  double coverage_not_run_lanes = -1.0;
  double active_worklist_non_reproducing_afl_timeouts = 0.0;
  double latest_finding_live = 0.0, latest_finding_missing = 0.0;
  double latest_known_reproduced = 0.0, latest_known_lost = 0.0;
  double latest_known_baseline = 0.0, latest_perf_hotspots = 0.0;
  double latest_full_pressure_finding_live = 0.0;
  double latest_full_pressure_finding_missing = 0.0;
  double latest_full_pressure_known_reproduced = 0.0;
  double latest_full_pressure_known_lost = 0.0;
  double latest_full_pressure_known_baseline = 0.0;
  double latest_full_pressure_failure_count = 0.0;
  double latest_full_pressure_perf_hotspots = 0.0;
  char *latest_report_path = strdup("");
  char *latest_full_pressure_report_path = strdup("");
  char *coverage_next_action = strdup("none");
  char *coverage_next_lane = strdup("");
  char *coverage_next_reason = strdup("");
  char *coverage_next_command = strdup("");
  char *coverage_next_guarded_command = strdup("");
  char *coverage_next_low_cpu_command = strdup("");
  char *coverage_next_preview_command = strdup("");
  char *coverage_next_state_command = strdup("");
  char *coverage_next_state_refresh_command = strdup("");
  char *coverage_next_stop_command = strdup("");
  char *coverage_next_resume_command = strdup("");
  char *coverage_queue_lanes = strdup("");
  char *coverage_queue_json = strdup("[]");
  if (!history_path || !read_file(history_path, &history) || !history.data) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-plan",
                                                  "history report not readable",
                                                  history_path ? history_path : ""));
  } else {
    if (!summary_number_from_report(history.data, "thread_years", &current_years)) {
      if (summary_number_from_report(history.data, "thread_hours", &current_hours))
        current_years = current_hours / (24.0 * 365.0);
    }
    (void)summary_number_from_report(history.data, "thread_hours", &current_hours);
    (void)summary_number_from_report(history.data, "campaign_calendar_span_days",
                                     &campaign_calendar_span_days);
    (void)summary_number_from_report(history.data, "campaign_calendar_age_days",
                                     &campaign_calendar_age_days);
    (void)summary_number_from_report(history.data, "checked_subcases", &checked_subcases);
    (void)summary_number_from_report(history.data, "latest_finding_live",
                                     &latest_finding_live);
    (void)summary_number_from_report(history.data, "latest_finding_missing",
                                     &latest_finding_missing);
    (void)summary_number_from_report(history.data,
                                     "latest_known_bug_reproduced",
                                     &latest_known_reproduced);
    (void)summary_number_from_report(history.data,
                                     "latest_known_bug_lost_signal",
                                     &latest_known_lost);
    (void)summary_number_from_report(history.data,
                                     "latest_known_bug_baseline_failures",
                                     &latest_known_baseline);
    (void)summary_number_from_report(history.data, "latest_perf_hotspots",
                                     &latest_perf_hotspots);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_finding_live",
                                     &latest_full_pressure_finding_live);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_finding_missing",
                                     &latest_full_pressure_finding_missing);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_known_bug_reproduced",
                                     &latest_full_pressure_known_reproduced);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_known_bug_lost_signal",
                                     &latest_full_pressure_known_lost);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_known_bug_baseline_failures",
                                     &latest_full_pressure_known_baseline);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_failure_count",
                                     &latest_full_pressure_failure_count);
    (void)summary_number_from_report(history.data,
                                     "latest_full_pressure_perf_hotspots",
                                     &latest_full_pressure_perf_hotspots);
    char *s = summary_string_from_report(history.data, "latest_report");
    if (s) {
      free(latest_report_path);
      latest_report_path = s;
    }
    s = summary_string_from_report(history.data, "latest_full_pressure_report");
    if (s) {
      free(latest_full_pressure_report_path);
      latest_full_pressure_report_path = s;
    }
  }
  if (worklist_path && read_file(worklist_path, &worklist) && worklist.data) {
    (void)summary_number_from_report(worklist.data, "active_items", &active_items);
    (void)summary_number_from_report(worklist.data,
                                     "non_reproducing_afl_timeouts",
                                     &active_worklist_non_reproducing_afl_timeouts);
  }
  if (coverage_path && read_file(coverage_path, &coverage) && coverage.data) {
    (void)summary_number_from_report(coverage.data, "blocker_gaps", &coverage_blocker_gaps);
    (void)summary_number_from_report(coverage.data, "coverage_backlog_lanes",
                                     &coverage_backlog_lanes);
    (void)summary_number_from_report(coverage.data, "coverage_queue_count",
                                     &coverage_queue_count);
    (void)summary_number_from_report(coverage.data,
                                     "coverage_queue_non_advisory_count",
                                     &coverage_queue_non_advisory_count);
    (void)summary_number_from_report(coverage.data,
                                     "coverage_queue_advisory_count",
                                     &coverage_queue_advisory_count);
    (void)summary_number_from_report(coverage.data,
                                     "coverage_depth_percent",
                                     &coverage_depth_percent);
    (void)summary_number_from_report(coverage.data,
                                     "coverage_not_run_lanes",
                                     &coverage_not_run_lanes);
    char *q = summary_string_from_report(coverage.data,
                                         "coverage_queue_lanes");
    if (q) {
      free(coverage_queue_lanes);
      coverage_queue_lanes = q;
    }
    char *qjson = summary_array_from_report(coverage.data, "coverage_queue");
    if (qjson) {
      free(coverage_queue_json);
      coverage_queue_json = qjson;
    }
    char *s = summary_string_from_report(coverage.data, "coverage_next_action");
    if (s) {
      free(coverage_next_action);
      coverage_next_action = s;
    }
    s = summary_string_from_report(coverage.data, "coverage_next_lane");
    if (s) {
      free(coverage_next_lane);
      coverage_next_lane = s;
    }
    s = summary_string_from_report(coverage.data, "coverage_next_reason");
    if (s) {
      free(coverage_next_reason);
      coverage_next_reason = s;
    }
    s = summary_string_from_report(coverage.data, "coverage_next_command");
    if (s) {
      free(coverage_next_command);
      coverage_next_command = s;
    }
    s = summary_string_from_report(coverage.data,
                                   "coverage_next_guarded_command");
    if (s) {
      free(coverage_next_guarded_command);
      coverage_next_guarded_command = s;
    }
    s = summary_string_from_report(coverage.data,
                                   "coverage_next_low_cpu_command");
    if (s) {
      free(coverage_next_low_cpu_command);
      coverage_next_low_cpu_command = s;
    }
    s = summary_string_from_report(coverage.data,
                                   "coverage_next_preview_command");
    if (s) {
      free(coverage_next_preview_command);
      coverage_next_preview_command = s;
    }
    s = summary_string_from_report(coverage.data,
                                   "coverage_next_state_command");
    if (s) {
      free(coverage_next_state_command);
      coverage_next_state_command = s;
    }
    s = summary_string_from_report(coverage.data,
                                   "coverage_next_state_refresh_command");
    if (s) {
      free(coverage_next_state_refresh_command);
      coverage_next_state_refresh_command = s;
    }
    s = summary_string_from_report(coverage.data, "coverage_next_stop_command");
    if (s) {
      free(coverage_next_stop_command);
      coverage_next_stop_command = s;
    }
    s = summary_string_from_report(coverage.data,
                                   "coverage_next_resume_command");
    if (s) {
      free(coverage_next_resume_command);
      coverage_next_resume_command = s;
    }
  }

  if (target_years <= 0.0)
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-plan",
                                                  "target thread-years must be positive",
                                                  target_arg ? target_arg : ""));
  if (hours_per_run <= 0.0)
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-plan",
                                                  "hours per run must be positive",
                                                  hours_arg ? hours_arg : ""));

  double remaining_years = target_years - current_years;
  if (remaining_years < 0.0) remaining_years = 0.0;
  double thread_hours_per_run = (double)threads * hours_per_run;
  double thread_years_per_run = thread_hours_per_run / (24.0 * 365.0);
  long runs_needed = 0;
  if (remaining_years > 0.0 && thread_years_per_run > 0.0) {
    runs_needed = (long)(remaining_years / thread_years_per_run);
    if ((double)runs_needed * thread_years_per_run + 0.000000001 < remaining_years)
      ++runs_needed;
  }
  double wall_hours = (double)runs_needed * hours_per_run;
  double thread_hours_needed = wall_hours * (double)threads;
  double runs_per_day = hours_per_run > 0.0 ? 24.0 / hours_per_run : 0.0;
  double thread_years_per_day = runs_per_day * thread_years_per_run;
  double completion_eta_epoch = 0.0;
  char completion_eta_local[64] = {0};
  if (runs_needed > 0 && wall_hours > 0.0) {
    time_t eta_time = time(NULL) + (time_t)(wall_hours * 3600.0 + 0.5);
    completion_eta_epoch = (double)eta_time;
    struct tm eta_tm;
    if (localtime_r(&eta_time, &eta_tm))
      (void)strftime(completion_eta_local, sizeof(completion_eta_local),
                     "%Y-%m-%d %H:%M:%S %z", &eta_tm);
  }
  double projected_years = current_years + (double)runs_needed * thread_years_per_run;
  double target_percent = target_years > 0.0 ? (current_years / target_years) * 100.0 : 0.0;
  if (target_percent > 100.0) target_percent = 100.0;
  double campaign_remaining_percent = 100.0 - target_percent;
  if (campaign_remaining_percent < 0.0) campaign_remaining_percent = 0.0;
  if (campaign_remaining_percent > 100.0) campaign_remaining_percent = 100.0;
  bool coverage_ready = coverage_blocker_gaps < 0.0 || coverage_blocker_gaps <= 0.0;
  bool ready = active_items <= 0.0 && coverage_ready;
  bool target_reached = remaining_years <= 0.0;
  bool campaign_complete = target_reached && ready;
  fuzz_all_status_summary_t plan_provenance;
  memset(&plan_provenance, 0, sizeof(plan_provenance));
  status_capture_provenance(&plan_provenance, root);
  bool score_cache_policy_ok = plan_provenance.cache_policy_ok;
  bool score_ny_bin_exists = plan_provenance.ny_bin_exists;
  bool score_active_old_writer =
      plan_provenance.active_old_nytrix_output_writer_present;
  double score_non_reproducing_afl_timeouts =
      active_worklist_non_reproducing_afl_timeouts;
  if (score_non_reproducing_afl_timeouts < 1.0 &&
      latest_full_pressure_report_path &&
      *latest_full_pressure_report_path &&
      latest_full_pressure_failure_count > 0.0 &&
      latest_full_pressure_finding_live <= 0.0 &&
      latest_full_pressure_finding_missing <= 0.0 &&
      latest_full_pressure_known_reproduced <= 0.0 &&
      latest_full_pressure_known_lost <= 0.0 &&
      latest_full_pressure_known_baseline <= 0.0 &&
      latest_full_pressure_perf_hotspots <= 0.0 &&
      fuzz_all_report_only_non_reproducing_afl_timeout(
          root, latest_full_pressure_report_path)) {
    score_non_reproducing_afl_timeouts = 1.0;
  }
  bool score_ready = ready && score_cache_policy_ok && score_ny_bin_exists;
  double latest_report_age_seconds =
      fuzz_all_score_report_age_seconds(root, latest_report_path);
  double latest_full_pressure_report_age_seconds =
      fuzz_all_score_report_age_seconds(root,
                                        latest_full_pressure_report_path);
  double latest_report_stale_after_hours = fuzz_all_score_latest_fresh_hours();
  double latest_full_pressure_report_stale_after_hours =
      fuzz_all_score_full_pressure_fresh_hours();
  bool latest_report_fresh =
      fuzz_all_score_age_fresh(latest_report_age_seconds,
                               latest_report_stale_after_hours);
  bool latest_full_pressure_report_fresh =
      fuzz_all_score_age_fresh(latest_full_pressure_report_age_seconds,
                               latest_full_pressure_report_stale_after_hours);
  bool language_evidence_fresh =
      latest_report_fresh && latest_full_pressure_report_fresh;
  double language_freshness_penalty = 0.0;
  if (!latest_report_fresh) language_freshness_penalty += 8.0;
  if (!latest_full_pressure_report_fresh) language_freshness_penalty += 12.0;
  double score_active_items = active_items > 0.0 ? active_items : 0.0;
  double score_coverage_blockers =
      coverage_blocker_gaps > 0.0 ? coverage_blocker_gaps : 0.0;
  double score_compiler_live =
      latest_finding_live > latest_full_pressure_finding_live ?
          latest_finding_live : latest_full_pressure_finding_live;
  double score_compiler_missing =
      latest_finding_missing > latest_full_pressure_finding_missing ?
          latest_finding_missing : latest_full_pressure_finding_missing;
  double score_known_reproduced =
      latest_known_reproduced > latest_full_pressure_known_reproduced ?
          latest_known_reproduced : latest_full_pressure_known_reproduced;
  double score_known_lost =
      latest_known_lost > latest_full_pressure_known_lost ?
          latest_known_lost : latest_full_pressure_known_lost;
  double score_known_baseline =
      latest_known_baseline > latest_full_pressure_known_baseline ?
          latest_known_baseline : latest_full_pressure_known_baseline;
  double score_perf_hotspots =
      latest_perf_hotspots > latest_full_pressure_perf_hotspots ?
          latest_perf_hotspots : latest_full_pressure_perf_hotspots;
  double language_signal_percent =
      fuzz_all_score_signal_score(score_ready, 0.0, score_active_items,
                                  score_coverage_blockers,
                                  score_compiler_live,
                                  score_compiler_missing,
                                  score_known_reproduced,
                                  score_known_lost,
                                  score_known_baseline,
                                  score_perf_hotspots,
                                  score_non_reproducing_afl_timeouts,
                                  score_cache_policy_ok,
                                  score_ny_bin_exists);
  double language_evidence_cap_percent =
      fuzz_all_score_evidence_cap(target_percent, campaign_complete);
  double language_capped_percent =
      language_signal_percent < language_evidence_cap_percent ?
          language_signal_percent : language_evidence_cap_percent;
  double language_score_percent =
      fuzz_all_score_clamp(language_capped_percent - language_freshness_penalty,
                           0.0, 100.0);
  const char *language_score_label =
      fuzz_all_score_label(language_score_percent);
  double language_good_threshold_percent = fuzz_all_score_good_threshold();
  double language_score_gap_percent =
      fuzz_all_language_good_gap_percent(language_score_percent);
  double target_percent_per_run =
      target_years > 0.0 ?
          fuzz_all_score_clamp((thread_years_per_run / target_years) * 100.0,
                               0.0, 100.0) : 0.0;
  double next_run_campaign_percent =
      fuzz_all_score_clamp(target_percent + target_percent_per_run, 0.0,
                           100.0);
  bool next_run_campaign_complete =
      campaign_complete || next_run_campaign_percent >= 100.0;
  double next_run_evidence_cap =
      fuzz_all_score_evidence_cap(next_run_campaign_percent,
                                  next_run_campaign_complete);
  double next_run_capped =
      language_signal_percent < next_run_evidence_cap ?
          language_signal_percent : next_run_evidence_cap;
  double next_run_language_score_percent =
      fuzz_all_score_clamp(next_run_capped - language_freshness_penalty, 0.0,
                           100.0);
  double next_run_language_score_delta_percent =
      next_run_language_score_percent - language_score_percent;
  double runs_to_good_language_score =
      fuzz_all_runs_to_good(target_percent, target_percent_per_run,
                            language_signal_percent, language_score_percent,
                            campaign_complete);
  if (language_freshness_penalty > 0.0) runs_to_good_language_score = -1.0;
  double runs_to_good_language_days =
      fuzz_all_runs_to_days(runs_to_good_language_score, runs_per_day);
  char language_score_note[256] = {0};
  if (!language_evidence_fresh) {
    snprintf(language_score_note, sizeof(language_score_note),
             "stale latest/full-pressure evidence limits plan score");
  } else if (language_signal_percent > language_evidence_cap_percent + 0.01) {
    snprintf(language_score_note, sizeof(language_score_note),
             "%s; evidence cap from campaign evidence",
             score_non_reproducing_afl_timeouts > 0.0 ?
                 "advisory timeout penalty" : "clean current gate");
  } else if (score_non_reproducing_afl_timeouts > 0.0) {
    snprintf(language_score_note, sizeof(language_score_note),
             "advisory timeout penalty limits stability");
  } else if (!score_cache_policy_ok || !score_ny_bin_exists) {
    snprintf(language_score_note, sizeof(language_score_note),
             "environment guard limits stability");
  } else {
    snprintf(language_score_note, sizeof(language_score_note),
             "score follows current compiler/perf/fuzz signals");
  }
  bool has_coverage_next =
      coverage_backlog_lanes > 0.0 &&
      coverage_next_action && *coverage_next_action &&
      strcmp(coverage_next_action, "none") != 0 &&
      coverage_next_command && *coverage_next_command;
  const char *coverage_next_effective_command =
      coverage_next_guarded_command && *coverage_next_guarded_command ?
          coverage_next_guarded_command : coverage_next_command;
  if ((!coverage_next_low_cpu_command ||
       !*coverage_next_low_cpu_command) &&
      coverage_next_guarded_command && *coverage_next_guarded_command) {
    free(coverage_next_low_cpu_command);
    coverage_next_low_cpu_command =
        fuzz_all_missing_evidence_low_cpu_command(
            coverage_next_guarded_command);
  }
  if ((!coverage_next_state_refresh_command ||
       !*coverage_next_state_refresh_command) &&
      coverage_next_guarded_command && *coverage_next_guarded_command) {
    free(coverage_next_state_refresh_command);
    coverage_next_state_refresh_command =
        fuzz_all_missing_evidence_state_refresh_command(
            coverage_next_guarded_command);
  }

  char *run_command = NULL, *run_command_raw = NULL, *handoff_command = NULL;
  char *handoff_preview_command = NULL, *refresh_command = NULL;
  char *history_md_path = path_with_suffix_ext(history_path ? history_path : "build/fuzz/all/history.json", "", ".md");
  char *worklist_md_path = path_with_suffix_ext(worklist_path ? worklist_path : "build/fuzz/all/worklist.json", "", ".md");
  char *coverage_md_path = coverage_path ? path_with_suffix_ext(coverage_path, "", ".md") : NULL;
  char *dir_cmd_path = rel_path_dup(root, dir_arg && *dir_arg ? dir_arg : "build/fuzz/all");
  char *history_cmd_path = rel_path_dup(root, history_path ? history_path : "build/fuzz/all/history.json");
  char *history_md_cmd_path = rel_path_dup(root, history_md_path ? history_md_path : "build/fuzz/all/history.md");
  char *worklist_cmd_path = rel_path_dup(root, worklist_path ? worklist_path : "build/fuzz/all/worklist.json");
  char *worklist_md_cmd_path = rel_path_dup(root, worklist_md_path ? worklist_md_path : "build/fuzz/all/worklist.md");
  char *coverage_cmd_path = coverage_path ? rel_path_dup(root, coverage_path) : NULL;
  char *coverage_md_cmd_path = coverage_md_path ? rel_path_dup(root, coverage_md_path) : NULL;
  const char *dir_cmd = dir_cmd_path && *dir_cmd_path ? dir_cmd_path : "build/fuzz/all";
  const char *history_cmd = history_cmd_path && *history_cmd_path ? history_cmd_path : "build/fuzz/all/history.json";
  const char *history_md_cmd = history_md_cmd_path && *history_md_cmd_path ? history_md_cmd_path : "build/fuzz/all/history.md";
  const char *worklist_cmd = worklist_cmd_path && *worklist_cmd_path ? worklist_cmd_path : "build/fuzz/all/worklist.json";
  const char *worklist_md_cmd = worklist_md_cmd_path && *worklist_md_cmd_path ? worklist_md_cmd_path : "build/fuzz/all/worklist.md";
  const char *coverage_cmd = coverage_cmd_path && *coverage_cmd_path ? coverage_cmd_path : "build/fuzz/all/coverage.json";
  const char *coverage_md_cmd = coverage_md_cmd_path && *coverage_md_cmd_path ? coverage_md_cmd_path : "build/fuzz/all/coverage.md";
  if (runs_needed > 0) {
    (void)asprintf(&run_command_raw,
                   "./build/nynth fuzz all run --profile %s --hours %.2f "
                   "--threads %s --target-thread-years %s --dir %s "
                   "--fail-fast --no-nytrix --no-sanitizers",
                   profile && *profile ? profile : "insane",
                   hours_per_run,
                   threads_arg && *threads_arg ? threads_arg : NYNTH_DEFAULT_FUZZ_THREADS,
                   target_arg && *target_arg ? target_arg : "10",
                   dir_cmd);
    run_command = fuzz_all_low_priority_command_dup(run_command_raw);
    if (ready) {
      if (path_is_absolute(dir_cmd)) {
        (void)asprintf(&handoff_command, "%s/run-next.sh", dir_cmd);
      } else {
        (void)asprintf(&handoff_command, "./%s/run-next.sh", dir_cmd);
      }
      handoff_preview_command = fuzz_all_preview_command(handoff_command);
    }
  } else {
    run_command = strdup("");
  }
  if (coverage_path && *coverage_path) {
    (void)asprintf(&refresh_command,
                   "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                   "./build/nynth fuzz all history --dir %s --json %s --markdown %s && "
                   "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                   "./build/nynth fuzz all worklist --history %s --json %s --markdown %s && "
                   "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                   "./build/nynth fuzz all coverage --strict --history %s "
                   "--target-thread-years %s --hours %s --threads %s --profile %s "
                   "--json %s --markdown %s",
                   dir_cmd, history_cmd, history_md_cmd,
                   history_cmd, worklist_cmd, worklist_md_cmd,
                   history_cmd,
                   target_arg && *target_arg ? target_arg : "10",
                   hours_arg && *hours_arg ? hours_arg : "8",
                   threads_arg && *threads_arg ? threads_arg :
                       NYNTH_DEFAULT_FUZZ_THREADS,
                   profile && *profile ? profile : "insane",
                   coverage_cmd, coverage_md_cmd);
  } else {
    (void)asprintf(&refresh_command,
                   "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                   "./build/nynth fuzz all history --dir %s --json %s --markdown %s && "
                   "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                   "./build/nynth fuzz all worklist --history %s --json %s --markdown %s",
                   dir_cmd, history_cmd, history_md_cmd,
                   history_cmd, worklist_cmd, worklist_md_cmd);
  }

  char plan_next_action[64] = {0};
  char plan_next_reason[256] = {0};
  char plan_next_command[4096] = {0};
  char plan_next_low_cpu_command[4096] = {0};
  char plan_next_preview_command[4096] = {0};
  char plan_next_repeat_mode[16] = {0};
  double plan_next_repeat_count = 0.0;
  if (has_coverage_next) {
    snprintf(plan_next_action, sizeof(plan_next_action), "%s",
             coverage_next_action ? coverage_next_action : "");
    snprintf(plan_next_reason, sizeof(plan_next_reason), "%s",
             coverage_next_reason ? coverage_next_reason : "");
    snprintf(plan_next_command, sizeof(plan_next_command), "%s",
             coverage_next_effective_command ?
                 coverage_next_effective_command : "");
    snprintf(plan_next_low_cpu_command, sizeof(plan_next_low_cpu_command),
             "%s", coverage_next_low_cpu_command ?
                       coverage_next_low_cpu_command : "");
    snprintf(plan_next_preview_command, sizeof(plan_next_preview_command),
             "%s", coverage_next_preview_command ?
                       coverage_next_preview_command : "");
  } else if (!ready || active_items > 0.0 || coverage_blocker_gaps > 0.0) {
    snprintf(plan_next_action, sizeof(plan_next_action), "triage-worklist");
    snprintf(plan_next_reason, sizeof(plan_next_reason),
             "active work, blockers, or coverage blockers need triage");
    snprintf(plan_next_command, sizeof(plan_next_command), "%s",
             refresh_command && *refresh_command ? refresh_command :
                 (run_command ? run_command : ""));
  } else if (remaining_years <= 0.0 || runs_needed <= 0) {
    snprintf(plan_next_action, sizeof(plan_next_action), "inspect-complete");
    snprintf(plan_next_reason, sizeof(plan_next_reason),
             "campaign target is complete");
    snprintf(plan_next_command, sizeof(plan_next_command), "%s",
             refresh_command ? refresh_command : "");
  } else if (!language_evidence_fresh) {
    snprintf(plan_next_action, sizeof(plan_next_action), "freshen-evidence");
    snprintf(plan_next_reason, sizeof(plan_next_reason),
             "latest or full-pressure evidence is stale");
    if (handoff_command && *handoff_command) {
      fuzz_all_env_command(plan_next_command, sizeof(plan_next_command),
                           "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10",
                           handoff_command);
      snprintf(plan_next_low_cpu_command, sizeof(plan_next_low_cpu_command),
               "%s", plan_next_command);
      fuzz_all_env_command(plan_next_preview_command,
                           sizeof(plan_next_preview_command),
                           "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                           "NYNTH_RUN_DRY_RUN=1",
                           handoff_command);
    } else {
      snprintf(plan_next_command, sizeof(plan_next_command), "%s",
               run_command ? run_command : "");
    }
  } else if (runs_to_good_language_score > 0.0 &&
             handoff_command && *handoff_command) {
    snprintf(plan_next_action, sizeof(plan_next_action), "run-good");
    snprintf(plan_next_reason, sizeof(plan_next_reason),
             "good language score is the nearest campaign milestone");
    fuzz_all_env_command(plan_next_command, sizeof(plan_next_command),
                         "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                         "NYNTH_RUN_REPEAT=good",
                         handoff_command);
    snprintf(plan_next_low_cpu_command, sizeof(plan_next_low_cpu_command),
             "%s", plan_next_command);
    fuzz_all_env_command(plan_next_preview_command,
                         sizeof(plan_next_preview_command),
                         "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                         "NYNTH_RUN_DRY_RUN=1 NYNTH_RUN_REPEAT=good",
                         handoff_command);
    snprintf(plan_next_repeat_mode, sizeof(plan_next_repeat_mode), "good");
    plan_next_repeat_count = runs_to_good_language_score;
  } else if (handoff_command && *handoff_command) {
    snprintf(plan_next_action, sizeof(plan_next_action), "run-target");
    snprintf(plan_next_reason, sizeof(plan_next_reason),
             "campaign evidence target is not reached");
    fuzz_all_env_command(plan_next_command, sizeof(plan_next_command),
                         "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                         "NYNTH_RUN_REPEAT=target",
                         handoff_command);
    snprintf(plan_next_low_cpu_command, sizeof(plan_next_low_cpu_command),
             "%s", plan_next_command);
    fuzz_all_env_command(plan_next_preview_command,
                         sizeof(plan_next_preview_command),
                         "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                         "NYNTH_RUN_DRY_RUN=1 NYNTH_RUN_REPEAT=target",
                         handoff_command);
    snprintf(plan_next_repeat_mode, sizeof(plan_next_repeat_mode), "target");
    plan_next_repeat_count = runs_needed > 0 ? (double)runs_needed : 0.0;
  } else {
    snprintf(plan_next_action, sizeof(plan_next_action), "inspect");
    snprintf(plan_next_reason, sizeof(plan_next_reason),
             "no automatic handoff selected");
    snprintf(plan_next_command, sizeof(plan_next_command), "%s",
             refresh_command ? refresh_command : "");
  }

  if (history.data && target_years > 0.0 && hours_per_run > 0.0) {
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"kind\":\"fuzz-all-plan\"");
    (void)sb_append(&row, ",\"history_report\":");
    append_rel_json_str(&row, root, history_path ? history_path : "");
    (void)sb_append(&row, ",\"worklist_report\":");
    append_rel_json_str(&row, root, worklist_path ? worklist_path : "");
    if (coverage_path && *coverage_path) {
      (void)sb_append(&row, ",\"coverage_report\":");
      append_rel_json_str(&row, root, coverage_path);
    }
    (void)sb_appendf(&row,
                        ",\"target_thread_years\":%.8f,"
                        "\"current_thread_years\":%.8f,"
                        "\"thread_years\":%.8f,"
                        "\"remaining_thread_years\":%.8f,"
                        "\"target_percent\":%.4f,"
                        "\"campaign_percent\":%.4f,"
                        "\"campaign_confidence_percent\":%.4f,"
                        "\"campaign_remaining_percent\":%.4f,"
                        "\"remaining_percent\":%.4f,"
                        "\"campaign_calendar_span_days\":%.4f,"
                        "\"campaign_calendar_age_days\":%.4f,"
                        "\"campaign_calendar_percent_10y\":%.4f,"
                        "\"score_percent\":%.2f,"
                        "\"stability_percent\":%.2f,"
                        "\"stability_score_percent\":%.2f,"
                        "\"language_score_percent\":%.2f,"
                        "\"language_score\":%.2f,"
                     "\"language_score_good_threshold_percent\":%.2f,"
                     "\"language_score_gap_percent\":%.2f,"
                     "\"language_score_signal_percent\":%.2f,"
                     "\"language_score_evidence_cap_percent\":%.2f,"
                     "\"signal_health_percent\":%.2f,"
                     "\"evidence_cap_percent\":%.2f,"
                     "\"language_score_evidence_fresh\":%s,"
                     "\"language_score_advisory_timeouts\":%.0f,"
                     "\"language_score_cache_policy_ok\":%s,"
                     "\"language_score_ny_bin_exists\":%s,"
                     "\"language_score_active_old_writer_present\":%s,"
                     "\"latest_report_age_seconds\":%.0f,"
                     "\"latest_full_pressure_report_age_seconds\":%.0f,"
                     "\"latest_report_fresh\":%s,"
                     "\"latest_full_pressure_report_fresh\":%s,"
                     "\"target_percent_per_run\":%.4f,"
                     "\"next_run_campaign_percent\":%.4f,"
                     "\"next_run_language_score_percent\":%.2f,"
                     "\"next_run_language_score\":%.2f,"
                     "\"next_run_language_score_delta_percent\":%.2f,"
                     "\"runs_to_good_language_score\":%.0f,"
                     "\"runs_to_good_days\":%.4f,"
                     "\"runs_to_good_language_days\":%.4f,"
                     "\"days_to_good_language_score\":%.4f,"
                     "\"checked_subcases\":%.0f,"
                     "\"threads\":%d,\"hours_per_run\":%.4f,"
                     "\"thread_hours_per_run\":%.4f,"
                     "\"thread_years_per_run\":%.8f,"
                     "\"runs_needed\":%ld,"
                     "\"wall_hours_needed\":%.4f,"
                     "\"thread_hours_needed\":%.4f,"
                     "\"wall_days_needed\":%.4f,"
                     "\"runs_per_day\":%.4f,"
                     "\"thread_years_per_day\":%.8f,"
                     "\"completion_eta_epoch\":%.0f,"
                     "\"projected_thread_years\":%.8f,"
                     "\"active_worklist_items\":%.0f,"
                     "\"coverage_blocker_gaps\":%.0f,"
                     "\"coverage_depth_percent\":%.2f,"
                     "\"coverage_percent\":%.2f,"
                     "\"coverage_not_run_lanes\":%.0f,"
                     "\"coverage_backlog_lanes\":%.0f,"
                     "\"coverage_queue_count\":%.0f,"
                     "\"coverage_queue_non_advisory_count\":%.0f,"
                     "\"coverage_queue_advisory_count\":%.0f,"
                     "\"full_pressure_ready\":%s,"
                     "\"target_reached\":%s,"
                     "\"long_run_ready\":%s",
                        target_years, current_years, current_years,
                        remaining_years,
                        target_percent, target_percent, target_percent,
                        campaign_remaining_percent,
                        campaign_remaining_percent,
                        campaign_calendar_span_days,
                        campaign_calendar_age_days,
                        fuzz_all_campaign_calendar_percent_10y(
                            campaign_calendar_age_days),
                        language_score_percent,
                        language_score_percent,
                        language_score_percent,
                        language_score_percent,
                        language_score_percent,
                     language_good_threshold_percent,
                     language_score_gap_percent,
                     language_signal_percent,
                     language_evidence_cap_percent,
                     language_signal_percent,
                     language_evidence_cap_percent,
                     language_evidence_fresh ? "true" : "false",
                     score_non_reproducing_afl_timeouts,
                     score_cache_policy_ok ? "true" : "false",
                     score_ny_bin_exists ? "true" : "false",
                     score_active_old_writer ? "true" : "false",
                     latest_report_age_seconds,
                     latest_full_pressure_report_age_seconds,
                     latest_report_fresh ? "true" : "false",
                     latest_full_pressure_report_fresh ? "true" : "false",
                     target_percent_per_run,
                     next_run_campaign_percent,
                     next_run_language_score_percent,
                     next_run_language_score_percent,
                     next_run_language_score_delta_percent,
                     runs_to_good_language_score,
                     runs_to_good_language_days,
                     runs_to_good_language_days,
                     runs_to_good_language_days,
                     checked_subcases,
                     threads, hours_per_run,
                     thread_hours_per_run, thread_years_per_run, runs_needed,
                     wall_hours, thread_hours_needed, wall_hours / 24.0, runs_per_day,
                     thread_years_per_day, completion_eta_epoch,
                     projected_years,
                     active_items < 0.0 ? -1.0 : active_items,
                     coverage_blocker_gaps < 0.0 ? -1.0 : coverage_blocker_gaps,
                     coverage_depth_percent < 0.0 ? -1.0 :
                         coverage_depth_percent,
                     coverage_depth_percent < 0.0 ? -1.0 :
                         coverage_depth_percent,
                     coverage_not_run_lanes < 0.0 ? -1.0 :
                         coverage_not_run_lanes,
                     coverage_backlog_lanes < 0.0 ? -1.0 : coverage_backlog_lanes,
                     coverage_queue_count,
                     coverage_queue_non_advisory_count,
                     coverage_queue_advisory_count,
                     coverage_ready ? "true" : "false",
                     remaining_years <= 0.0 ? "true" : "false",
                     ready ? "true" : "false");
    (void)sb_appendf(&row, ",\"score\":%.2f", language_score_percent);
    (void)sb_append(&row, ",\"coverage_queue_lanes\":");
    (void)sb_append_json_str(&row, coverage_queue_lanes ?
                                      coverage_queue_lanes : "");
    append_fuzz_all_coverage_queue_json(&row, coverage_queue_json);
       append_fuzz_all_campaign_alias_fields(
           &row, current_years, target_years, remaining_years, target_percent,
           runs_needed, wall_hours, wall_hours / 24.0, thread_years_per_run,
           target_percent_per_run, runs_per_day, thread_years_per_day,
           hours_per_run, threads_arg,
           completion_eta_local);
    (void)sb_append(&row, ",\"completion_eta_local\":");
    (void)sb_append_json_str(&row, completion_eta_local);
    (void)sb_append(&row, ",\"campaign_state\":");
    (void)sb_append_json_str(
        &row, fuzz_all_campaign_state(ready, remaining_years <= 0.0,
                                      ready && remaining_years <= 0.0));
    (void)sb_append(&row, ",\"campaign_incomplete_reason\":");
    (void)sb_append_json_str(
        &row,
        fuzz_all_campaign_incomplete_reason(
            ready, coverage_blocker_gaps < 0.0 ? 0.0 : coverage_blocker_gaps,
            active_items < 0.0 ? 0.0 : active_items, remaining_years <= 0.0,
            ready && remaining_years <= 0.0));
    (void)sb_append(&row, ",\"completion_state\":");
    (void)sb_append_json_str(
        &row, fuzz_all_campaign_state(ready, remaining_years <= 0.0,
                                      ready && remaining_years <= 0.0));
    (void)sb_append(&row, ",\"completion_reason\":");
    (void)sb_append_json_str(
        &row,
        fuzz_all_campaign_incomplete_reason(
            ready, coverage_blocker_gaps < 0.0 ? 0.0 : coverage_blocker_gaps,
            active_items < 0.0 ? 0.0 : active_items, remaining_years <= 0.0,
            ready && remaining_years <= 0.0));
    (void)sb_append(&row, ",\"language_score_label\":");
    (void)sb_append_json_str(&row, language_score_label);
    (void)sb_append(&row, ",\"score_label\":");
    (void)sb_append_json_str(&row, language_score_label);
    (void)sb_append(&row, ",\"language_score_note\":");
    (void)sb_append_json_str(&row, language_score_note);
    (void)sb_append(&row, ",\"run_command\":");
    (void)sb_append_json_str(&row, run_command ? run_command : "");
    (void)sb_append(&row, ",\"handoff_command\":");
    (void)sb_append_json_str(&row, handoff_command ? handoff_command : "");
    (void)sb_append(&row, ",\"handoff_preview_command\":");
    (void)sb_append_json_str(&row, handoff_preview_command ? handoff_preview_command : "");
    (void)sb_append(&row, ",\"refresh_command\":");
    (void)sb_append_json_str(&row, refresh_command ? refresh_command : "");
    (void)sb_append(&row, ",\"plan_next_action\":");
    (void)sb_append_json_str(&row, plan_next_action);
    (void)sb_append(&row, ",\"plan_next_lane\":");
    (void)sb_append_json_str(&row, has_coverage_next ?
                                      coverage_next_lane : "");
    (void)sb_append(&row, ",\"plan_next_reason\":");
    (void)sb_append_json_str(&row, plan_next_reason);
    (void)sb_append(&row, ",\"plan_next_command\":");
    (void)sb_append_json_str(&row, plan_next_command);
    (void)sb_append(&row, ",\"plan_next_low_cpu_command\":");
    (void)sb_append_json_str(&row, plan_next_low_cpu_command);
    (void)sb_append(&row, ",\"plan_next_preview_command\":");
    (void)sb_append_json_str(&row, plan_next_preview_command);
    (void)sb_append(&row, ",\"recommended_action\":");
    (void)sb_append_json_str(&row, plan_next_action);
    (void)sb_append(&row, ",\"recommended_reason\":");
    (void)sb_append_json_str(&row, plan_next_reason);
    (void)sb_append(&row, ",\"recommended_command\":");
    (void)sb_append_json_str(&row, plan_next_command);
    (void)sb_append(&row, ",\"recommended_low_cpu_command\":");
    (void)sb_append_json_str(&row, plan_next_low_cpu_command);
    (void)sb_append(&row, ",\"recommended_preview_command\":");
    (void)sb_append_json_str(&row, plan_next_preview_command);
    (void)sb_append(&row, ",\"recommended_repeat_mode\":");
    (void)sb_append_json_str(&row, plan_next_repeat_mode);
    (void)sb_appendf(&row, ",\"recommended_repeat_count\":%.0f",
                     plan_next_repeat_count);
    (void)sb_append(&row, ",\"coverage_next_state_refresh_command\":");
    (void)sb_append_json_str(&row, has_coverage_next ?
                                      coverage_next_state_refresh_command : "");
    (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
    (void)string_list_push_take(&rows, sb_take(&row));
  }

  if (markdown_path && *markdown_path &&
      !write_fuzz_all_plan_markdown(root, markdown_path, history_path,
                                    worklist_path, coverage_path, target_years,
                                    current_years, remaining_years, threads,
                                    hours_per_run, runs_needed, wall_hours,
                                    runs_per_day, thread_years_per_day,
                                    completion_eta_local,
                                    checked_subcases, active_items,
                                    coverage_blocker_gaps,
                                    coverage_backlog_lanes,
                                    coverage_queue_count,
                                    coverage_queue_non_advisory_count,
                                    coverage_queue_advisory_count,
                                    coverage_queue_lanes,
                                    language_score_percent,
                                    language_score_label,
                                    language_good_threshold_percent,
                                    language_score_gap_percent,
                                    next_run_language_score_percent,
                                    next_run_language_score_delta_percent,
                                    runs_to_good_language_score,
                                    runs_to_good_language_days,
                                    language_evidence_fresh,
                                    score_non_reproducing_afl_timeouts,
                                    score_cache_policy_ok,
                                    score_ny_bin_exists,
                                    score_active_old_writer,
                                    coverage_next_action,
                                    coverage_next_lane,
                                    coverage_next_reason,
                                    coverage_next_command,
                                    coverage_next_state_command,
                                    coverage_next_state_refresh_command,
                                    coverage_next_stop_command,
                                    coverage_next_resume_command,
                                    plan_next_action,
                                    plan_next_reason,
                                    plan_next_command,
                                    plan_next_low_cpu_command,
                                    plan_next_preview_command,
                                    handoff_preview_command,
                                    handoff_command,
                                    run_command,
                                    refresh_command)) {
    (void)string_list_push_take(&failures,
                                make_fuzz_failure(root, "fuzz-all-plan",
                                                  "plan markdown write failed",
                                                  markdown_path));
  }

  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"history_report\":");
  append_rel_json_str(&extra, root, history_path ? history_path : "");
  (void)sb_append(&extra, ",\"worklist_report\":");
  append_rel_json_str(&extra, root, worklist_path ? worklist_path : "");
  if (coverage_path && *coverage_path) {
    (void)sb_append(&extra, ",\"coverage_report\":");
    append_rel_json_str(&extra, root, coverage_path);
  }
  if (markdown_path && *markdown_path) {
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, root, markdown_path);
  }
  (void)sb_appendf(&extra,
                      ",\"target_thread_years\":%.8f,"
                      "\"current_thread_years\":%.8f,"
                      "\"thread_years\":%.8f,"
                      "\"remaining_thread_years\":%.8f,"
                      "\"target_percent\":%.4f,"
                      "\"campaign_percent\":%.4f,"
                      "\"campaign_confidence_percent\":%.4f,"
                      "\"campaign_remaining_percent\":%.4f,"
                      "\"remaining_percent\":%.4f,"
                      "\"campaign_calendar_span_days\":%.4f,"
                      "\"campaign_calendar_age_days\":%.4f,"
                      "\"campaign_calendar_percent_10y\":%.4f,"
                      "\"score_percent\":%.2f,"
                      "\"stability_percent\":%.2f,"
                      "\"stability_score_percent\":%.2f,"
                      "\"language_score_percent\":%.2f,"
                      "\"language_score\":%.2f,"
                   "\"language_score_good_threshold_percent\":%.2f,"
                   "\"language_score_gap_percent\":%.2f,"
                   "\"language_score_signal_percent\":%.2f,"
                   "\"language_score_evidence_cap_percent\":%.2f,"
                   "\"signal_health_percent\":%.2f,"
                   "\"evidence_cap_percent\":%.2f,"
                   "\"language_score_evidence_fresh\":%s,"
                   "\"language_score_advisory_timeouts\":%.0f,"
                   "\"language_score_cache_policy_ok\":%s,"
                   "\"language_score_ny_bin_exists\":%s,"
                   "\"language_score_active_old_writer_present\":%s,"
                   "\"latest_report_age_seconds\":%.0f,"
                   "\"latest_full_pressure_report_age_seconds\":%.0f,"
                   "\"latest_report_fresh\":%s,"
                   "\"latest_full_pressure_report_fresh\":%s,"
                   "\"target_percent_per_run\":%.4f,"
                   "\"next_run_campaign_percent\":%.4f,"
                   "\"next_run_language_score_percent\":%.2f,"
                   "\"next_run_language_score\":%.2f,"
                   "\"next_run_language_score_delta_percent\":%.2f,"
                   "\"runs_to_good_language_score\":%.0f,"
                   "\"runs_to_good_days\":%.4f,"
                   "\"runs_to_good_language_days\":%.4f,"
                   "\"checked_subcases\":%.0f,"
                   "\"threads\":%d,\"hours_per_run\":%.4f,"
                   "\"thread_hours_per_run\":%.4f,"
                   "\"thread_years_per_run\":%.8f,"
                   "\"runs_needed\":%ld,"
                   "\"wall_hours_needed\":%.4f,"
                   "\"thread_hours_needed\":%.4f,"
                   "\"wall_days_needed\":%.4f,"
                   "\"runs_per_day\":%.4f,"
                   "\"thread_years_per_day\":%.8f,"
                   "\"completion_eta_epoch\":%.0f,"
                   "\"projected_thread_years\":%.8f,"
                   "\"active_worklist_items\":%.0f,"
                   "\"coverage_blocker_gaps\":%.0f,"
                   "\"coverage_depth_percent\":%.2f,"
                   "\"coverage_percent\":%.2f,"
                   "\"coverage_not_run_lanes\":%.0f,"
                   "\"coverage_backlog_lanes\":%.0f,"
                   "\"coverage_queue_count\":%.0f,"
                   "\"coverage_queue_non_advisory_count\":%.0f,"
                   "\"coverage_queue_advisory_count\":%.0f,"
                   "\"full_pressure_ready\":%s,"
                   "\"target_reached\":%s,"
                   "\"long_run_ready\":%s",
                      target_years, current_years, current_years,
                      remaining_years,
                      target_percent, target_percent, target_percent,
                      campaign_remaining_percent,
                      campaign_remaining_percent,
                      campaign_calendar_span_days,
                      campaign_calendar_age_days,
                      fuzz_all_campaign_calendar_percent_10y(
                          campaign_calendar_age_days),
                      language_score_percent,
                      language_score_percent,
                      language_score_percent,
                      language_score_percent,
                      language_score_percent,
                   language_good_threshold_percent,
                   language_score_gap_percent,
                   language_signal_percent,
                   language_evidence_cap_percent,
                   language_signal_percent,
                   language_evidence_cap_percent,
                   language_evidence_fresh ? "true" : "false",
                   score_non_reproducing_afl_timeouts,
                   score_cache_policy_ok ? "true" : "false",
                   score_ny_bin_exists ? "true" : "false",
                   score_active_old_writer ? "true" : "false",
                   latest_report_age_seconds,
                   latest_full_pressure_report_age_seconds,
                   latest_report_fresh ? "true" : "false",
                   latest_full_pressure_report_fresh ? "true" : "false",
                   target_percent_per_run,
                   next_run_campaign_percent,
                   next_run_language_score_percent,
                   next_run_language_score_percent,
                   next_run_language_score_delta_percent,
                   runs_to_good_language_score,
                   runs_to_good_language_days,
                   runs_to_good_language_days,
                   checked_subcases,
                   threads, hours_per_run,
                   thread_hours_per_run, thread_years_per_run, runs_needed,
                   wall_hours, thread_hours_needed, wall_hours / 24.0, runs_per_day,
                   thread_years_per_day, completion_eta_epoch,
                   projected_years,
                   active_items < 0.0 ? -1.0 : active_items,
                   coverage_blocker_gaps < 0.0 ? -1.0 : coverage_blocker_gaps,
                   coverage_depth_percent < 0.0 ? -1.0 :
                       coverage_depth_percent,
                   coverage_depth_percent < 0.0 ? -1.0 :
                       coverage_depth_percent,
                   coverage_not_run_lanes < 0.0 ? -1.0 :
                       coverage_not_run_lanes,
                   coverage_backlog_lanes < 0.0 ? -1.0 : coverage_backlog_lanes,
                   coverage_queue_count,
                   coverage_queue_non_advisory_count,
                   coverage_queue_advisory_count,
                   coverage_ready ? "true" : "false",
                   remaining_years <= 0.0 ? "true" : "false",
                   ready ? "true" : "false");
  (void)sb_appendf(&extra, ",\"score\":%.2f", language_score_percent);
  (void)sb_append(&extra, ",\"coverage_queue_lanes\":");
  (void)sb_append_json_str(&extra, coverage_queue_lanes ?
                                      coverage_queue_lanes : "");
  append_fuzz_all_coverage_queue_json(&extra, coverage_queue_json);
     append_fuzz_all_campaign_alias_fields(
         &extra, current_years, target_years, remaining_years, target_percent,
         runs_needed, wall_hours, wall_hours / 24.0, thread_years_per_run,
         target_percent_per_run, runs_per_day, thread_years_per_day,
         hours_per_run, threads_arg,
         completion_eta_local);
  (void)sb_append(&extra, ",\"completion_eta_local\":");
  (void)sb_append_json_str(&extra, completion_eta_local);
  (void)sb_append(&extra, ",\"campaign_state\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_campaign_state(ready, remaining_years <= 0.0,
                                      ready && remaining_years <= 0.0));
  (void)sb_append(&extra, ",\"campaign_incomplete_reason\":");
  (void)sb_append_json_str(
      &extra,
      fuzz_all_campaign_incomplete_reason(
          ready, coverage_blocker_gaps < 0.0 ? 0.0 : coverage_blocker_gaps,
          active_items < 0.0 ? 0.0 : active_items, remaining_years <= 0.0,
          ready && remaining_years <= 0.0));
  (void)sb_append(&extra, ",\"completion_state\":");
  (void)sb_append_json_str(
      &extra, fuzz_all_campaign_state(ready, remaining_years <= 0.0,
                                      ready && remaining_years <= 0.0));
  (void)sb_append(&extra, ",\"completion_reason\":");
  (void)sb_append_json_str(
      &extra,
      fuzz_all_campaign_incomplete_reason(
          ready, coverage_blocker_gaps < 0.0 ? 0.0 : coverage_blocker_gaps,
          active_items < 0.0 ? 0.0 : active_items, remaining_years <= 0.0,
          ready && remaining_years <= 0.0));
  (void)sb_append(&extra, ",\"language_score_label\":");
  (void)sb_append_json_str(&extra, language_score_label);
  (void)sb_append(&extra, ",\"score_label\":");
  (void)sb_append_json_str(&extra, language_score_label);
  (void)sb_append(&extra, ",\"language_score_note\":");
  (void)sb_append_json_str(&extra, language_score_note);
  if (run_command) {
    (void)sb_append(&extra, ",\"run_command\":");
    (void)sb_append_json_str(&extra, run_command);
  }
  if (handoff_command) {
    (void)sb_append(&extra, ",\"handoff_command\":");
    (void)sb_append_json_str(&extra, handoff_command);
  }
  if (handoff_preview_command) {
    (void)sb_append(&extra, ",\"handoff_preview_command\":");
    (void)sb_append_json_str(&extra, handoff_preview_command);
  }
  (void)sb_append(&extra, ",\"plan_next_action\":");
  (void)sb_append_json_str(&extra, plan_next_action);
  (void)sb_append(&extra, ",\"plan_next_lane\":");
  (void)sb_append_json_str(&extra, has_coverage_next ?
                                    coverage_next_lane : "");
  (void)sb_append(&extra, ",\"plan_next_reason\":");
  (void)sb_append_json_str(&extra, plan_next_reason);
  (void)sb_append(&extra, ",\"plan_next_command\":");
  (void)sb_append_json_str(&extra, plan_next_command);
  (void)sb_append(&extra, ",\"plan_next_low_cpu_command\":");
  (void)sb_append_json_str(&extra, plan_next_low_cpu_command);
  (void)sb_append(&extra, ",\"plan_next_preview_command\":");
  (void)sb_append_json_str(&extra, plan_next_preview_command);
  (void)sb_append(&extra, ",\"recommended_action\":");
  (void)sb_append_json_str(&extra, plan_next_action);
  (void)sb_append(&extra, ",\"recommended_reason\":");
  (void)sb_append_json_str(&extra, plan_next_reason);
  (void)sb_append(&extra, ",\"recommended_command\":");
  (void)sb_append_json_str(&extra, plan_next_command);
  (void)sb_append(&extra, ",\"recommended_low_cpu_command\":");
  (void)sb_append_json_str(&extra, plan_next_low_cpu_command);
  (void)sb_append(&extra, ",\"recommended_preview_command\":");
  (void)sb_append_json_str(&extra, plan_next_preview_command);
  (void)sb_append(&extra, ",\"recommended_repeat_mode\":");
  (void)sb_append_json_str(&extra, plan_next_repeat_mode);
  (void)sb_appendf(&extra, ",\"recommended_repeat_count\":%.0f",
                   plan_next_repeat_count);
  (void)sb_append(&extra, ",\"coverage_next_state_refresh_command\":");
  (void)sb_append_json_str(&extra, has_coverage_next ?
                                    coverage_next_state_refresh_command : "");
     char *report = build_native_report_json_with_top_aliases(
         &rows, &failures, "fuzz-all-plan", extra.data, true);
  int rc = emit_native_report(report, json_path, "all fuzz plan",
                              rows.count, failures.count);
  free(extra.data);
  free(history_path);
  free(worklist_path);
  free(coverage_path);
  free(history.data);
  free(worklist.data);
  free(coverage.data);
  free(history_md_path);
  free(worklist_md_path);
  free(coverage_md_path);
  free(dir_cmd_path);
  free(history_cmd_path);
  free(history_md_cmd_path);
  free(worklist_cmd_path);
  free(worklist_md_cmd_path);
  free(coverage_cmd_path);
  free(coverage_md_cmd_path);
  free(coverage_next_action);
  free(coverage_next_lane);
  free(coverage_next_reason);
  free(coverage_next_command);
  free(coverage_next_guarded_command);
  free(coverage_next_low_cpu_command);
  free(coverage_next_preview_command);
  free(coverage_next_state_command);
  free(coverage_next_state_refresh_command);
  free(coverage_next_stop_command);
  free(coverage_next_resume_command);
  free(coverage_queue_lanes);
  free(coverage_queue_json);
  free(latest_report_path);
  free(latest_full_pressure_report_path);
  free(run_command);
  free(run_command_raw);
  free(handoff_command);
  free(handoff_preview_command);
  free(refresh_command);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static void status_set_rel_path(char *dst, size_t dst_sz,
                                const char *root, const char *path) {
  if (!dst || dst_sz == 0) return;
  char *rel = rel_path_dup(root ? root : "", path ? path : "");
  snprintf(dst, dst_sz, "%s", rel ? rel : "");
  free(rel);
}

static void status_add_blocker(const char *category, const char *severity,
                               const char *reason, double count,
                               const char *command,
                               fuzz_all_status_summary_t *summary,
                               string_list_t *rows) {
  if (!rows || !category || !*category) return;
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"fuzz-all-status-blocker\",\"category\":");
  (void)sb_append_json_str(&row, category);
  (void)sb_append(&row, ",\"severity\":");
  (void)sb_append_json_str(&row, severity ? severity : "high");
  (void)sb_append(&row, ",\"reason\":");
  (void)sb_append_json_str(&row, reason ? reason : "");
  (void)sb_appendf(&row, ",\"count\":%.0f,\"command\":", count);
  (void)sb_append_json_str(&row, command ? command : "");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  if (summary && (!severity || strcmp(severity, "advisory") != 0))
    ++summary->blocker_count;
}

static void status_add_coverage_detail(const char *category, const char *severity,
                                       const char *lane, const char *reason,
                                       const char *command,
                                       fuzz_all_status_summary_t *summary,
                                       string_list_t *rows,
                                       string_list_t *seen_lanes) {
  if (!rows || !lane || !*lane) return;
  if (reason && strstr(reason, "filtered by --only-lane")) return;
  if (seen_lanes && string_list_contains(seen_lanes, lane)) return;
  if (seen_lanes) (void)string_list_push_unique_copy(seen_lanes, lane);
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"fuzz-all-status-coverage-gap\",\"category\":");
  (void)sb_append_json_str(&row, category ? category : "");
  (void)sb_append(&row, ",\"severity\":");
  (void)sb_append_json_str(&row, severity ? severity : "medium");
  (void)sb_append(&row, ",\"lane\":");
  (void)sb_append_json_str(&row, lane);
  (void)sb_append(&row, ",\"reason\":");
  (void)sb_append_json_str(&row, reason ? reason : "");
  (void)sb_append(&row, ",\"command\":");
  (void)sb_append_json_str(&row, command ? command : "");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  if (summary) {
    bool have_next = summary->coverage_next_lane[0] != '\0';
    bool have_advisory =
        strcmp(summary->coverage_next_severity, "advisory") == 0;
    bool new_advisory = severity && strcmp(severity, "advisory") == 0;
    if (!have_next || (have_advisory && !new_advisory)) {
      snprintf(summary->coverage_next_action,
               sizeof(summary->coverage_next_action),
               "%s", "run-missing-evidence");
      snprintf(summary->coverage_next_category,
               sizeof(summary->coverage_next_category), "%s",
               category ? category : "");
      snprintf(summary->coverage_next_severity,
               sizeof(summary->coverage_next_severity), "%s",
               severity && *severity ? severity : "medium");
      snprintf(summary->coverage_next_lane,
               sizeof(summary->coverage_next_lane), "%s",
               lane ? lane : "");
      snprintf(summary->coverage_next_reason,
               sizeof(summary->coverage_next_reason), "%s",
               reason ? reason : "");
      snprintf(summary->coverage_next_command,
               sizeof(summary->coverage_next_command), "%s",
               command ? command : "");
    }
    ++summary->coverage_detail_count;
  }
}

static void append_fuzz_all_coverage_next_fields(str_buf_t *row,
                                                 const fuzz_all_status_summary_t *s) {
  bool have = s && s->coverage_next_lane[0] && s->coverage_next_command[0];
  (void)sb_append(row, ",\"coverage_next_action\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_action : "none");
  (void)sb_append(row, ",\"coverage_next_category\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_category : "");
  (void)sb_append(row, ",\"coverage_next_severity\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_severity : "");
  (void)sb_append(row, ",\"coverage_next_lane\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_lane : "");
  (void)sb_append(row, ",\"coverage_next_reason\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_reason : "");
  (void)sb_append(row, ",\"coverage_next_command\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_command : "");
  (void)sb_append(row, ",\"coverage_next_guarded_command\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_guarded_command : "");
  (void)sb_append(row, ",\"coverage_next_low_cpu_command\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_low_cpu_command : "");
  (void)sb_append(row, ",\"coverage_next_preview_command\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_preview_command : "");
  (void)sb_append(row, ",\"coverage_next_state_file\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_state_file : "");
  (void)sb_append(row, ",\"coverage_next_state_command\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_state_command : "");
  (void)sb_append(row, ",\"coverage_next_state_refresh_command\":");
  (void)sb_append_json_str(
      row, have ? s->coverage_next_state_refresh_command : "");
  (void)sb_append(row, ",\"coverage_next_state_refresh_required\":");
  (void)sb_append(row, have && s->coverage_next_state_refresh_required ?
                         "true" : "false");
  (void)sb_append(row, ",\"coverage_next_state_refresh_reason\":");
  (void)sb_append_json_str(
      row, have ? s->coverage_next_state_refresh_reason : "");
  (void)sb_append(row, ",\"coverage_next_stop_file\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_stop_file : "");
  (void)sb_append(row, ",\"coverage_next_stop_command\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_stop_command : "");
  (void)sb_append(row, ",\"coverage_next_resume_command\":");
  (void)sb_append_json_str(row, have ? s->coverage_next_resume_command : "");
}

static double fuzz_all_report_lane_sub_rows(const char *root,
                                            const char *report_path,
                                            const char *lane_name) {
  if (!report_path || !*report_path || !lane_name || !*lane_name) return 0.0;
  file_buf_t report = {0};
  bool read_ok = false;
  if (path_is_absolute(report_path)) {
    read_ok = read_file(report_path, &report);
  } else {
    char joined[4096] = {0};
    if (path_join(joined, sizeof(joined), root ? root : "", report_path))
      read_ok = read_file(joined, &report);
  }
  if (!read_ok || !report.data) {
    free(report.data);
    return 0.0;
  }

  string_list_t rows = {0};
  double out = 0.0;
  if (collect_rows_from_report_json(report.data, &rows)) {
    for (int i = 0; i < rows.count; ++i) {
      char *name = json_string_or_empty(rows.items[i], "name");
      bool match = name && strcmp(name, lane_name) == 0;
      free(name);
      if (!match) continue;
      (void)extract_json_number(rows.items[i], "sub_rows", &out);
      break;
    }
  }
  string_list_free(&rows);
  free(report.data);
  return out;
}

static bool status_allows_full_pressure_remediation(const fuzz_all_status_summary_t *s) {
  if (!s || !s->allow_full_pressure_remediation || !s->history_readable ||
      !s->worklist_readable || !s->coverage_readable || !s->plan_readable ||
      !s->latest_full_pressure_report[0] || s->latest_full_pressure_clean ||
      !s->ny_bin_exists || !s->cache_policy_ok)
    return false;
  if (s->reports <= 0.0 || s->coverage_blocker_gaps > 0.0 ||
      s->coverage_failed_lanes > 0.0)
    return false;

  int expected_blockers = 1;
  bool latest_is_full_pressure =
      s->latest_report[0] &&
      strcmp(s->latest_report, s->latest_full_pressure_report) == 0;
  if (latest_is_full_pressure && !s->latest_report_clean)
    ++expected_blockers;
  if (latest_is_full_pressure && s->active_items > 0.0)
    ++expected_blockers;
  return s->blocker_count == expected_blockers;
}

static bool path_inside_dir_prefix(const char *path, const char *dir) {
  if (!path || !*path || !dir || !*dir) return false;
  size_t n = strlen(dir);
  return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static bool cmdline_has_tool_token(const char *cmdline, const char *tool) {
  if (!cmdline || !*cmdline || !tool || !*tool) return false;
  size_t n = strlen(tool);
  const char *p = cmdline;
  while ((p = strstr(p, tool)) != NULL) {
    char before = p == cmdline ? ' ' : p[-1];
    char after = p[n];
    bool before_ok = isspace((unsigned char)before) || before == '/' ||
                     before == '(' || before == '\0';
    bool after_ok = after == '\0' || isspace((unsigned char)after) ||
                    after == ':' || after == '=';
    if (before_ok && after_ok) return true;
    p += n;
  }
  return false;
}

static bool cmdline_path_boundary(char c) {
  return c == '\0' || isspace((unsigned char)c) || c == '\'' ||
         c == '"' || c == '=' || c == ':' || c == ',' || c == '(' ||
         c == ')';
}

static bool cmdline_relative_path_start_ok(const char *cmdline, const char *p) {
  if (!cmdline || !p || p < cmdline) return false;
  const char *start = p;
  while (start > cmdline && start[-1] == '/') {
    if (start >= cmdline + 3 && start[-2] == '.' &&
               start[-3] == '.') {
      start -= 3;
    } else if (start >= cmdline + 2 && start[-2] == '.') {
      start -= 2;
    } else {
      break;
    }
  }
  return start == cmdline || cmdline_path_boundary(start[-1]);
}

static bool cmdline_mentions_relative_path(const char *cmdline,
                                           const char *rel_path) {
  if (!cmdline || !*cmdline || !rel_path || !*rel_path) return false;
  size_t n = strlen(rel_path);
  const char *p = cmdline;
  while ((p = strstr(p, rel_path)) != NULL) {
    char after = p[n];
    bool after_ok = after == '\0' || after == '/' ||
                    cmdline_path_boundary(after);
    if (cmdline_relative_path_start_ok(cmdline, p) && after_ok) return true;
    p += n;
  }
  return false;
}

static bool cmdline_looks_readonly_path_probe(const char *cmdline) {
  if (!cmdline || !*cmdline) return false;
  const char *tools[] = {
    "rg", "grep", "git", "jq", "sed", "cat", "head", "tail", "find",
    "pgrep", "awk", "wc", "less"
  };
  for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); ++i) {
    if (cmdline_has_tool_token(cmdline, tools[i])) return true;
  }
  if (strstr(cmdline, "http.server") && strstr(cmdline, "--directory"))
    return true;
  return false;
}

static bool cmdline_mentions_old_nytrix_output_path(const char *cmdline) {
  if (!cmdline || !*cmdline) return false;
  return strstr(cmdline, "/home/e/nytrix/fuzz") ||
         strstr(cmdline, "../nytrix/fuzz") ||
         strstr(cmdline, "/home/e/nytrix/build/cache") ||
         strstr(cmdline, "../nytrix/build/cache");
}

static bool cmdline_has_nytrix_repo_cache_writer_context(const char *cmdline) {
  if (!cmdline || !*cmdline) return false;
  const char *markers[] = {
    "build/cache/tools/public publish",
    "build/cache/tools/public",
    "ny tools web",
    "build/release/ny tools web",
    "build/debug/ny tools web",
    "make public",
    "make docs",
  };
  for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
    if (strstr(cmdline, markers[i])) return true;
  }
  return false;
}

static bool cmdline_has_nytrix_webasm_build_context(const char *cmdline) {
  if (!cmdline || !*cmdline) return false;
  return strstr(cmdline, "./make webasm") ||
         strstr(cmdline, "make webasm") ||
         strstr(cmdline, "make web-demos") ||
         strstr(cmdline, "make web demos");
}

static bool cmdline_has_nytrix_rev_cache_spawner_context(const char *cmdline) {
  if (!cmdline || !*cmdline) return false;
  if (!strstr(cmdline, "build/cache/tools/rev/tool")) return false;
  return cmdline_has_tool_token(cmdline, "dec") ||
         cmdline_has_tool_token(cmdline, "triage") ||
         cmdline_has_tool_token(cmdline, "parity") ||
         cmdline_has_tool_token(cmdline, "replay");
}

static bool cmdline_has_nytrix_cwd_marker(const char *cmdline) {
  if (!cmdline || !*cmdline) return false;
  return strstr(cmdline, "--command-cwd /home/e/nytrix") ||
         strstr(cmdline, "--sandbox-policy-cwd /home/e/nytrix") ||
         strstr(cmdline, "--command-cwd /tmp/nytrix") ||
         strstr(cmdline, "--sandbox-policy-cwd /tmp/nytrix");
}

static bool cmdline_has_no_write_mode(const char *cmdline) {
  return cmdline && strstr(cmdline, "--no-write");
}

static void status_git_snapshot(const char *cwd, const char *repo,
                                char *head, size_t head_sz,
                                char *status_hash, size_t hash_sz,
                                bool *ok, bool *dirty) {
  if (head && head_sz) head[0] = '\0';
  if (status_hash && hash_sz)
    snprintf(status_hash, hash_sz, "%016" PRIx64, fnv1a64("", 0));
  if (ok) *ok = false;
  if (dirty) *dirty = false;
  if (!repo || !*repo || !exists_path(repo)) return;

  char *head_argv[] = {"git", "-C", (char *)repo, "rev-parse", "--short=12",
                       "HEAD", NULL};
  proc_result_t head_pr = run_proc(head_argv, cwd, 10.0);
  bool head_ok = head_pr.rc == 0;
  if (head_ok && head && head_sz) {
    char *trimmed = trim_trailing_copy(head_pr.out ? head_pr.out : "");
    snprintf(head, head_sz, "%s", trimmed ? trimmed : "");
    free(trimmed);
  }
  proc_result_free(&head_pr);

  char *status_argv[] = {"git", "-C", (char *)repo, "status", "--short", NULL};
  proc_result_t status_pr = run_proc(status_argv, cwd, 10.0);
  bool status_ok = status_pr.rc == 0;
  char *trimmed_status = trim_trailing_copy(status_pr.out ? status_pr.out : "");
  size_t status_len = trimmed_status ? strlen(trimmed_status) : 0u;
  if (status_hash && hash_sz)
    snprintf(status_hash, hash_sz, "%016" PRIx64,
             fnv1a64(trimmed_status ? trimmed_status : "", status_len));
  if (dirty) *dirty = status_len > 0;
  free(trimmed_status);
  proc_result_free(&status_pr);
  if (ok) *ok = head_ok && status_ok;
}

static bool process_cmdline_is_old_nytrix_output_writer(const char *cmdline,
                                                        const char *cwd) {
  if (!cmdline || !*cmdline) return false;
  if (strstr(cmdline, "fuzz all old-paths") ||
      strstr(cmdline, "fuzz all old-path"))
    return false;
  if (cmdline_looks_readonly_path_probe(cmdline)) return false;
  bool mentions_old_output = cmdline_mentions_old_nytrix_output_path(cmdline);
  if (mentions_old_output) {
    return true;
  }
  bool in_nytrix = cwd && (path_inside_dir_prefix(cwd, "/home/e/nytrix") ||
                           path_inside_dir_prefix(cwd, "/tmp/nytrix"));
  if (!in_nytrix && cmdline_has_nytrix_cwd_marker(cmdline))
    in_nytrix = true;
  if (!in_nytrix) return false;
  if (cmdline_has_tool_token(cmdline, "ny-lsp")) return true;
  if (cmdline_has_nytrix_repo_cache_writer_context(cmdline)) return true;
  if (cmdline_has_nytrix_rev_cache_spawner_context(cmdline)) return true;
  if (cmdline_has_no_write_mode(cmdline) &&
      !cmdline_mentions_relative_path(cmdline, "build/cache") &&
      !cmdline_mentions_relative_path(cmdline, "build/cache/test/probe"))
    return false;
  return cmdline_mentions_relative_path(cmdline, "build/cache/test/probe") ||
         cmdline_mentions_relative_path(cmdline, "build/cache");
}

static bool status_find_old_nytrix_output_writer(char *out, size_t out_sz) {
  if (out && out_sz) out[0] = '\0';
#ifdef __linux__
  DIR *dir = opendir("/proc");
  if (!dir) return false;
  struct dirent *ent = NULL;
  while ((ent = readdir(dir)) != NULL) {
    if (!isdigit((unsigned char)ent->d_name[0])) continue;
    char path[512];
    snprintf(path, sizeof(path), "/proc/%s/cmdline", ent->d_name);
    FILE *f = fopen(path, "rb");
    if (!f) continue;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) continue;
    for (size_t i = 0; i < n; ++i) {
      if (buf[i] == '\0') buf[i] = ' ';
    }
    buf[n] = '\0';
    char cwd_path[4096] = {0};
    char cwd_link[512];
    snprintf(cwd_link, sizeof(cwd_link), "/proc/%s/cwd", ent->d_name);
    ssize_t cwd_len = readlink(cwd_link, cwd_path, sizeof(cwd_path) - 1);
    if (cwd_len > 0) cwd_path[cwd_len] = '\0';
    if (!process_cmdline_is_old_nytrix_output_writer(buf, cwd_path)) continue;
    if (out && out_sz) {
      size_t prefix_len = strlen("pid ") + strlen(ent->d_name) + strlen(": ");
      size_t max_cmd = out_sz > prefix_len + 1 ? out_sz - prefix_len - 1 : 0;
      if (max_cmd > INT_MAX) max_cmd = INT_MAX;
      snprintf(out, out_sz, "pid %s: %.*s", ent->d_name, (int)max_cmd, buf);
    }
    closedir(dir);
    return true;
  }
  closedir(dir);
#else
  (void)out;
  (void)out_sz;
#endif
  return false;
}

static bool status_find_old_nytrix_output_writer_for_root(
    const char *nytrix_root, char *out, size_t out_sz) {
  if (out && out_sz) out[0] = '\0';
  if (!nytrix_root || !*nytrix_root)
    return status_find_old_nytrix_output_writer(out, out_sz);
#ifdef __linux__
  char selected_root[4096] = {0};
  if (!realpath(nytrix_root, selected_root))
    snprintf(selected_root, sizeof(selected_root), "%s", nytrix_root);
  char old_test[4096] = {0};
  char old_fuzz[4096] = {0};
  char old_cache[4096] = {0};
  (void)path_join(old_test, sizeof(old_test), selected_root,
                  "build/cache/projects/test");
  (void)path_join(old_fuzz, sizeof(old_fuzz), selected_root, "fuzz");
  (void)path_join(old_cache, sizeof(old_cache), selected_root, "build/cache");

  DIR *dir = opendir("/proc");
  if (!dir) return false;
  struct dirent *ent = NULL;
  while ((ent = readdir(dir)) != NULL) {
    if (!isdigit((unsigned char)ent->d_name[0])) continue;
    char path[512];
    snprintf(path, sizeof(path), "/proc/%s/cmdline", ent->d_name);
    FILE *f = fopen(path, "rb");
    if (!f) continue;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) continue;
    for (size_t i = 0; i < n; ++i) {
      if (buf[i] == '\0') buf[i] = ' ';
    }
    buf[n] = '\0';
    char cwd_path[4096] = {0};
    char cwd_link[512];
    snprintf(cwd_link, sizeof(cwd_link), "/proc/%s/cwd", ent->d_name);
    ssize_t cwd_len = readlink(cwd_link, cwd_path, sizeof(cwd_path) - 1);
    if (cwd_len > 0) cwd_path[cwd_len] = '\0';
    bool writer_context =
        process_cmdline_is_old_nytrix_output_writer(buf, cwd_path);
    bool webasm_old_cache_writer =
        !writer_context && exists_path(old_cache) &&
        cmdline_has_nytrix_webasm_build_context(buf) && cwd_path[0] &&
        path_inside_dir_prefix(cwd_path, selected_root);
    if (!writer_context && !webasm_old_cache_writer) continue;
    bool selected_root_writer =
        (old_test[0] && strstr(buf, old_test)) ||
        (old_fuzz[0] && strstr(buf, old_fuzz)) ||
        (old_cache[0] && strstr(buf, old_cache)) ||
        (cwd_path[0] && path_inside_dir_prefix(cwd_path, selected_root));
    if (!selected_root_writer) continue;
    if (out && out_sz) {
      size_t prefix_len = strlen("pid ") + strlen(ent->d_name) + strlen(": ");
      size_t max_cmd = out_sz > prefix_len + 1 ? out_sz - prefix_len - 1 : 0;
      if (max_cmd > INT_MAX) max_cmd = INT_MAX;
      snprintf(out, out_sz, "pid %s: %.*s", ent->d_name, (int)max_cmd, buf);
    }
    closedir(dir);
    return true;
  }
  closedir(dir);
#else
  (void)nytrix_root;
#endif
  return false;
}

static void status_capture_provenance(fuzz_all_status_summary_t *s,
                                      const char *root) {
  if (!s || !root || !*root) return;
  snprintf(s->nynth_root, sizeof(s->nynth_root), "%s", root);
  (void)path_join(s->tmp_dir, sizeof(s->tmp_dir), root, "build/cache/tmp");
  (void)path_join(s->scratch_root, sizeof(s->scratch_root), root,
                  "build/cache/scratch");
  (void)path_join(s->xdg_cache_home, sizeof(s->xdg_cache_home), root,
                  "build/cache/xdg");
  (void)path_join(s->nytrix_cache_dir, sizeof(s->nytrix_cache_dir), root,
                  "build/cache/nytrix");
  (void)path_join(s->old_nytrix_test_scratch,
                  sizeof(s->old_nytrix_test_scratch), root,
                  "../nytrix/build/cache/projects/test");
  (void)path_join(s->old_nytrix_fuzz_dir, sizeof(s->old_nytrix_fuzz_dir),
                  root, "../nytrix/fuzz");
  (void)path_join(s->old_nytrix_build_cache_dir,
                  sizeof(s->old_nytrix_build_cache_dir), root,
                  "../nytrix/build/cache");
  char cache_root[4096] = {0};
  (void)path_join(cache_root, sizeof(cache_root), root, "build/cache");
  s->old_nytrix_test_scratch_absent =
      !exists_path(s->old_nytrix_test_scratch);
  s->old_nytrix_fuzz_absent = !exists_path(s->old_nytrix_fuzz_dir);
  s->old_nytrix_build_cache_absent =
      !exists_path(s->old_nytrix_build_cache_dir);
  s->old_path_present_count =
      (s->old_nytrix_test_scratch_absent ? 0.0 : 1.0) +
      (s->old_nytrix_fuzz_absent ? 0.0 : 1.0) +
      (s->old_nytrix_build_cache_absent ? 0.0 : 1.0);
  s->old_path_moved_count = 0.0;
  s->old_path_remaining_count = s->old_path_present_count;
  s->old_path_wait_remaining_seconds = -1.0;
  char old_nytrix_root[4096] = {0};
  (void)path_join(old_nytrix_root, sizeof(old_nytrix_root), root,
                  "../nytrix");
  s->active_old_nytrix_output_writer_present =
      status_find_old_nytrix_output_writer_for_root(
          old_nytrix_root,
          s->active_old_nytrix_output_writer,
          sizeof(s->active_old_nytrix_output_writer));
  s->active_old_nytrix_cache_writer_present =
      s->active_old_nytrix_output_writer_present;
  snprintf(s->active_old_nytrix_cache_writer,
           sizeof(s->active_old_nytrix_cache_writer), "%s",
           s->active_old_nytrix_output_writer);
  s->cache_policy_ok =
      path_inside_dir_prefix(s->tmp_dir, cache_root) &&
      path_inside_dir_prefix(s->scratch_root, cache_root) &&
      path_inside_dir_prefix(s->xdg_cache_home, cache_root) &&
      path_inside_dir_prefix(s->nytrix_cache_dir, cache_root);

  s->nytrix_git_status_hash[0] = '\0';
  s->nynth_git_status_hash[0] = '\0';
  status_git_snapshot(root, root, s->nynth_git_head, sizeof(s->nynth_git_head),
                      s->nynth_git_status_hash,
                      sizeof(s->nynth_git_status_hash),
                      &s->nynth_git_ok, &s->nynth_git_dirty);
  file_hash_hex(g_self_path, s->nynth_bin_hash, sizeof(s->nynth_bin_hash));

  char ny_root[4096] = {0};
  if (find_repo_root_or_sibling(ny_root, sizeof(ny_root)) && ny_root[0])
    snprintf(s->nytrix_root, sizeof(s->nytrix_root), "%s", ny_root);
  if (find_ny_bin(s->nytrix_root, s->ny_bin, sizeof(s->ny_bin)))
    s->ny_bin_exists = true;
  if (s->ny_bin_exists)
    file_hash_hex(s->ny_bin, s->ny_bin_hash, sizeof(s->ny_bin_hash));
  if (s->nytrix_root[0])
    status_git_snapshot(root, s->nytrix_root, s->nytrix_git_head,
                        sizeof(s->nytrix_git_head),
                        s->nytrix_git_status_hash,
                        sizeof(s->nytrix_git_status_hash),
                        &s->nytrix_git_ok, &s->nytrix_git_dirty);
}

static void status_load_old_path_report(fuzz_all_status_summary_t *s,
                                        const char *root,
                                        const char *json_path,
                                        const char *markdown_path) {
  if (!s) return;
  status_set_rel_path(s->old_path_report, sizeof(s->old_path_report), root,
                      json_path && *json_path ? json_path :
                      "build/fuzz/all/old-paths.json");
  status_set_rel_path(s->old_path_markdown, sizeof(s->old_path_markdown), root,
                      markdown_path && *markdown_path ? markdown_path :
                      "build/fuzz/all/old-paths.md");
  s->old_path_cache_policy_ok = s->cache_policy_ok;
  if (!json_path || !*json_path) return;
  file_buf_t report = {0};
  if (!read_file(json_path, &report) || !report.data) return;
  (void)summary_bool_from_report(report.data, "cache_policy_ok",
                                 &s->old_path_cache_policy_ok);
  (void)summary_bool_from_report(report.data,
                                 "old_nytrix_test_scratch_absent",
                                 &s->old_nytrix_test_scratch_absent);
  (void)summary_bool_from_report(report.data, "old_nytrix_fuzz_absent",
                                 &s->old_nytrix_fuzz_absent);
  (void)summary_bool_from_report(report.data,
                                 "old_nytrix_build_cache_absent",
                                 &s->old_nytrix_build_cache_absent);
  (void)summary_number_from_report(report.data, "artifact_leak_count",
                                   &s->old_path_artifact_leak_count);
  (void)summary_number_from_report(report.data, "artifact_moved_count",
                                   &s->old_path_artifact_moved_count);
  (void)summary_number_from_report(report.data, "artifact_remaining_count",
                                   &s->old_path_artifact_remaining_count);
  (void)summary_number_from_report(report.data, "present_count",
                                   &s->old_path_present_count);
  (void)summary_number_from_report(report.data, "moved_count",
                                   &s->old_path_moved_count);
  (void)summary_number_from_report(report.data, "remaining_count",
                                   &s->old_path_remaining_count);
  (void)summary_number_from_report(report.data,
                                   "old_path_wait_remaining_seconds",
                                   &s->old_path_wait_remaining_seconds);
  bool report_active_writer = false;
  if (summary_bool_from_report(report.data,
                               "active_old_nytrix_output_writer_present",
                               &report_active_writer) &&
      report_active_writer) {
    s->active_old_nytrix_output_writer_present = true;
    s->active_old_nytrix_cache_writer_present = true;
    char *writer = summary_string_from_report(
        report.data, "active_old_nytrix_output_writer");
    if (writer && *writer) {
      snprintf(s->active_old_nytrix_output_writer,
               sizeof(s->active_old_nytrix_output_writer), "%s", writer);
      snprintf(s->active_old_nytrix_cache_writer,
               sizeof(s->active_old_nytrix_cache_writer), "%s", writer);
    }
    free(writer);
  }
  free(report.data);
}

typedef struct {
  const char *name;
  const char *rel;
  const char *archive_leaf;
  char source[4096];
  char archive[4096];
  bool artifact;
  bool present_before;
  bool present_after;
  bool moved;
  bool blocked;
  bool failed;
  char error[256];
} fuzz_all_old_path_item_t;

static bool old_paths_abs_arg(const char *root, const char *arg,
                              const char *fallback_rel,
                              char *out, size_t out_sz) {
  if (!out || !out_sz) return false;
  const char *value = arg && *arg ? arg : fallback_rel;
  if (!value || !*value) return false;
  if (path_is_absolute(value)) {
    snprintf(out, out_sz, "%s", value);
    return out[0] != '\0';
  }
  return path_join(out, out_sz, root ? root : ".", value);
}

static const char *old_path_item_action(const fuzz_all_old_path_item_t *it,
                                        bool apply) {
  if (!it) return "unknown";
  if (it->failed) return "failed";
  if (it->blocked) return "blocked";
  if (it->moved) return "archived";
  if (!it->present_before) return "absent";
  return apply ? "unchanged" : "would-archive";
}

static const char *old_path_item_source_label(const fuzz_all_old_path_item_t *it) {
  if (!it || !it->name) return "old-sibling-output";
  if (it->artifact) return "stale-report-artifact";
  if (strcmp(it->name, "old-test-scratch") == 0)
    return "old-sibling-test-scratch";
  if (strcmp(it->name, "old-fuzz") == 0)
    return "old-sibling-fuzz-dir";
  if (strcmp(it->name, "old-build-cache") == 0)
    return "old-sibling-build-cache";
  return "old-sibling-output";
}

static time_t old_path_latest_mtime_under(const char *path, int depth,
                                          int *budget) {
  if (!path || !*path || !budget || *budget <= 0) return 0;
  struct stat st;
  if (lstat(path, &st) != 0) return 0;
  --(*budget);
  time_t latest = st.st_mtime;
  if (!S_ISDIR(st.st_mode) || depth <= 0) return latest;
  DIR *dir = opendir(path);
  if (!dir) return latest;
  struct dirent *ent = NULL;
  while (*budget > 0 && (ent = readdir(dir)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    char child[4096];
    if (!path_join(child, sizeof(child), path, ent->d_name)) continue;
    time_t child_latest =
        old_path_latest_mtime_under(child, depth - 1, budget);
    if (child_latest > latest) latest = child_latest;
  }
  closedir(dir);
  return latest;
}

static bool old_path_recent_rev_cache_write(const char *old_cache,
                                            int settle_s,
                                            double *age_seconds_out) {
  if (age_seconds_out) *age_seconds_out = -1.0;
  if (!old_cache || !*old_cache || settle_s <= 0) return false;
  char rev_dir[4096];
  if (!path_join(rev_dir, sizeof(rev_dir), old_cache, "rev")) return false;
  int budget = 4096;
  time_t latest = old_path_latest_mtime_under(rev_dir, 4, &budget);
  if (latest <= 0) return false;
  double age = difftime(time(NULL), latest);
  if (age < 0.0) age = 0.0;
  if (age_seconds_out) *age_seconds_out = age;
  return age <= (double)settle_s;
}

static bool old_paths_detect_active_or_recent_writer(
    const char *nytrix_root, const char *old_cache, int settle_s,
    char *detail, size_t detail_sz, bool *recent_write_out,
    double *recent_age_seconds_out) {
  if (detail && detail_sz) detail[0] = '\0';
  if (recent_write_out) *recent_write_out = false;
  if (recent_age_seconds_out) *recent_age_seconds_out = -1.0;
  if (status_find_old_nytrix_output_writer_for_root(
          nytrix_root, detail, detail_sz))
    return true;
  double age = -1.0;
  bool recent = old_path_recent_rev_cache_write(old_cache, settle_s, &age);
  if (recent_write_out) *recent_write_out = recent;
  if (recent_age_seconds_out) *recent_age_seconds_out = age;
  if (recent) {
    if (detail && detail_sz)
      snprintf(detail, detail_sz,
               "recent old Nytrix rev cache write %.0fs ago; settle window %ds",
               age, settle_s);
    return true;
  }
  return false;
}

static bool old_path_artifact_candidate_file(const char *path) {
  return path && (ny_has_suffix(path, ".json") || ny_has_suffix(path, ".md") ||
                  ny_has_suffix(path, ".txt") || ny_has_suffix(path, ".sh"));
}

static bool old_path_artifact_revision_snapshot(const char *path) {
  if (!path || !*path) return false;
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  return strstr(name, "-r") &&
         (ny_has_suffix(name, ".json") || ny_has_suffix(name, ".md"));
}

static bool old_path_artifact_live_cockpit_report(const char *path) {
  if (!path || !*path || !strstr(path, "/build/fuzz/all/")) return false;
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  return strcmp(name, "status.json") == 0 ||
         strcmp(name, "status.md") == 0 ||
         strcmp(name, "progress.json") == 0 ||
         strcmp(name, "progress.md") == 0 ||
         strcmp(name, "old-paths.json") == 0 ||
         strcmp(name, "old-paths.md") == 0;
}

static bool old_path_artifact_contains_stale_reference(const char *path) {
  if (!old_path_artifact_candidate_file(path)) return false;
  if (old_path_artifact_live_cockpit_report(path)) return false;
  file_buf_t f = {0};
  if (!read_file(path, &f) || !f.data) return false;
  bool stale_cockpit_command =
      strstr(f.data,
             "selftest run --only fuzz_all_help --only fuzz_all_default_pressure") ||
      strstr(f.data,
             "--only fuzz_all_full_pressure_remediation --only fuzz_all_plan_coverage_next") ||
      strstr(f.data,
             "--only fuzz_all_full_pressure_remediation --only fuzz_all_old_paths");
  bool stale_revision_run_good =
      old_path_artifact_revision_snapshot(path) &&
      (strstr(f.data,
              "NYNTH_RUN_REPEAT=good ./build/fuzz/all/run-next.sh") ||
       strstr(f.data,
              "NYNTH_RUN_DRY_RUN=1 NYNTH_RUN_REPEAT=good"));
  bool hit = strstr(f.data, "/home/e/nytrix/fuzz") ||
             strstr(f.data, "../nytrix/fuzz") ||
             strstr(f.data, "/home/e/nytrix/build/cache") ||
             strstr(f.data, "../nytrix/build/cache") ||
             stale_cockpit_command ||
             stale_revision_run_good;
  free(f.data);
  return hit;
}

static fuzz_all_old_path_item_t *old_paths_collect_artifact_items(
    const char *root, const char *scan_dir, const char *archive_run_dir,
    int *count_out) {
  if (count_out) *count_out = 0;
  if (!root || !*root || !scan_dir || !*scan_dir || !archive_run_dir ||
      !*archive_run_dir)
    return NULL;
  string_list_t files = {0};
  if (!collect_regular_files_recursive(scan_dir, &files)) return NULL;
  fuzz_all_old_path_item_t *items = NULL;
  int count = 0;
  for (int i = 0; i < files.count; ++i) {
    const char *path = files.items[i];
    if (!path_under_directory(path, scan_dir)) continue;
    if (!old_path_artifact_contains_stale_reference(path)) continue;
    fuzz_all_old_path_item_t *next =
        (fuzz_all_old_path_item_t *)realloc(
            items, (size_t)(count + 1) * sizeof(fuzz_all_old_path_item_t));
    if (!next) break;
    items = next;
    memset(&items[count], 0, sizeof(items[count]));
    items[count].name = "stale-report-artifact";
    items[count].artifact = true;
    snprintf(items[count].source, sizeof(items[count].source), "%s", path);
    char *rel = rel_path_dup(scan_dir, path);
    char *archive_rel = path_child_dup("report-artifacts",
                                       rel && *rel ? rel : "artifact");
    if (archive_rel) {
      (void)path_join(items[count].archive, sizeof(items[count].archive),
                      archive_run_dir, archive_rel);
    }
    free(rel);
    free(archive_rel);
    count++;
  }
  string_list_free(&files);
  if (count_out) *count_out = count;
  return items;
}

static char *make_fuzz_all_old_path_row(const char *root,
                                        const fuzz_all_old_path_item_t *it,
                                        bool apply,
                                        bool dry_run) {
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"fuzz-all-old-path\",\"name\":");
  (void)sb_append_json_str(&row, it ? it->name : "");
  (void)sb_append(&row, ",\"ok\":");
  (void)sb_append(&row, it && !it->failed && !it->blocked ? "true" : "false");
  (void)sb_append(&row, ",\"source\":");
  (void)root;
  (void)sb_append_json_str(&row, old_path_item_source_label(it));
  (void)sb_append(&row, ",\"archive\":");
  append_rel_json_str(&row, root ? root : "", it ? it->archive : "");
  (void)sb_append(&row, ",\"present_before\":");
  (void)sb_append(&row, it && it->present_before ? "true" : "false");
  (void)sb_append(&row, ",\"present_after\":");
  (void)sb_append(&row, it && it->present_after ? "true" : "false");
  (void)sb_append(&row, ",\"moved\":");
  (void)sb_append(&row, it && it->moved ? "true" : "false");
  (void)sb_append(&row, ",\"dry_run\":");
  (void)sb_append(&row, dry_run ? "true" : "false");
  (void)sb_append(&row, ",\"apply\":");
  (void)sb_append(&row, apply ? "true" : "false");
  (void)sb_append(&row, ",\"action\":");
  (void)sb_append_json_str(&row, old_path_item_action(it, apply));
  if (it && it->error[0]) {
    (void)sb_append(&row, ",\"error\":");
    (void)sb_append_json_str(&row, it->error);
  }
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  return sb_take(&row);
}

static void append_shell_option_text(str_buf_t *b, const char *flag,
                                     const char *value) {
  if (!b || !flag || !*flag || !value || !*value) return;
  (void)sb_append_c(b, ' ');
  (void)sb_append(b, flag);
  (void)sb_append_c(b, ' ');
  append_shell_single_quoted(b, value);
}

static char *old_paths_json_command_path_dup(const char *root,
                                             const char *json_path,
                                             const char *markdown_path) {
  if (json_path && *json_path)
    return rel_path_dup(root ? root : "", json_path);
  char *md_rel = rel_path_dup(root ? root : "",
                              markdown_path ? markdown_path : "");
  char *dir = path_parent_dup(md_rel && *md_rel ? md_rel : markdown_path,
                              "build/fuzz/all");
  char *out = NULL;
  (void)asprintf(&out, "%s/old-paths.json",
                 dir && *dir ? dir : "build/fuzz/all");
  free(md_rel);
  free(dir);
  return out;
}

static bool write_fuzz_all_old_paths_markdown(const char *markdown_path,
                                              const char *root,
                                              const char *nytrix_root,
                                              const char *archive_dir,
                                              const char *json_path,
                                              const fuzz_all_old_path_item_t *items,
                                              int item_count,
                                              const fuzz_all_old_path_item_t *artifact_items,
                                              int artifact_item_count,
                                              const char *artifact_scan_dir,
                                              const char *archive_run_dir,
                                              bool apply,
                                              bool dry_run,
                                              bool active_writer,
                                              int wait_writers_s,
                                              int waited_writers_s,
                                              int present_count,
                                              int moved_count,
                                              int remaining_count) {
  if (!markdown_path || !*markdown_path) return true;
  str_buf_t md = {0};
  (void)sb_append(&md, "# Nynth Old Paths\n\n## TLDR\n\n");
  (void)sb_appendf(&md,
                   "- Mode: `%s`; active old writer `%s`; wait %ds/%ds; present %d; moved %d; remaining %d.\n",
                   dry_run ? "dry-run" : "apply",
                   active_writer ? "present" : "none",
                   waited_writers_s, wait_writers_s, present_count,
                   moved_count, remaining_count);
  char *scan_rel = rel_path_dup(root ? root : "",
                                artifact_scan_dir ? artifact_scan_dir : "");
  (void)sb_appendf(&md,
                   "- Stale build-artifact leaks: `%d`; scan dir ",
                   artifact_item_count);
  md_append_code(&md, scan_rel ? scan_rel : "");
  free(scan_rel);
  (void)sb_append(&md, ".\n");
  (void)sb_append(&md, "- Archive dir: ");
  char *archive_rel = rel_path_dup(root ? root : "", archive_run_dir ? archive_run_dir : "");
  md_append_code(&md, archive_rel ? archive_rel : "");
  free(archive_rel);
  (void)sb_append(&md, ".\n\n## Paths\n\n");
  for (int i = 0; i < item_count; ++i) {
    char *dst_rel = rel_path_dup(root ? root : "", items[i].archive);
    (void)sb_append(&md, "- ");
    md_append_code(&md, items[i].name);
    (void)sb_append(&md, ": ");
    md_append_code(&md, old_path_item_action(&items[i], apply));
    (void)sb_append(&md, "; source ");
    md_append_code(&md, old_path_item_source_label(&items[i]));
    (void)sb_append(&md, "; archive ");
    md_append_code(&md, dst_rel ? dst_rel : items[i].archive);
    if (items[i].error[0]) {
      (void)sb_append(&md, "; error ");
      md_append_code(&md, items[i].error);
    }
    (void)sb_append(&md, ".\n");
    free(dst_rel);
  }
  for (int i = 0; i < artifact_item_count; ++i) {
    char *src_rel = rel_path_dup(root ? root : "", artifact_items[i].source);
    char *dst_rel = rel_path_dup(root ? root : "", artifact_items[i].archive);
    (void)sb_append(&md, "- ");
    md_append_code(&md, artifact_items[i].name);
    (void)sb_append(&md, ": ");
    md_append_code(&md, old_path_item_action(&artifact_items[i], apply));
    (void)sb_append(&md, "; source ");
    md_append_code(&md, src_rel ? src_rel : artifact_items[i].source);
    (void)sb_append(&md, "; archive ");
    md_append_code(&md, dst_rel ? dst_rel : artifact_items[i].archive);
    if (artifact_items[i].error[0]) {
      (void)sb_append(&md, "; error ");
      md_append_code(&md, artifact_items[i].error);
    }
    (void)sb_append(&md, ".\n");
    free(src_rel);
    free(dst_rel);
  }
  (void)sb_append(&md, "\n## Next\n\n```bash\n");
  char *nytrix_rel = rel_path_dup(root ? root : "", nytrix_root ? nytrix_root : "");
  char *archive_base_rel = rel_path_dup(root ? root : "", archive_dir ? archive_dir : "");
  char *json_rel = old_paths_json_command_path_dup(root, json_path, markdown_path);
  char *md_rel = rel_path_dup(root ? root : "", markdown_path ? markdown_path : "");
  char *status_dir = path_parent_dup(md_rel && *md_rel ? md_rel : markdown_path,
                                     "build/fuzz/all");
  bool offer_apply = dry_run && present_count > 0;
  if (offer_apply) {
    (void)sb_append(&md, "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth fuzz all old-paths --apply");
    if (active_writer) (void)sb_append(&md, " --wait-writers-s 300");
    append_shell_option_text(&md, "--nytrix-root", nytrix_rel);
    append_shell_option_text(&md, "--archive-dir", archive_base_rel);
    append_shell_option_text(&md, "--json", json_rel);
    append_shell_option_text(&md, "--markdown", md_rel);
    (void)sb_append_c(&md, '\n');
  } else {
    char *status_json = NULL;
    char *status_md = NULL;
    (void)asprintf(&status_json, "%s/status.json",
                   status_dir && *status_dir ? status_dir : "build/fuzz/all");
    (void)asprintf(&status_md, "%s/status.md",
                   status_dir && *status_dir ? status_dir : "build/fuzz/all");
    (void)sb_append(&md, "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth fuzz all status --refresh --strict --allow-full-pressure-remediation");
    append_shell_option_text(&md, "--dir",
                             status_dir && *status_dir ? status_dir : "build/fuzz/all");
    append_shell_option_text(&md, "--json", status_json);
    append_shell_option_text(&md, "--markdown", status_md);
    (void)sb_append_c(&md, '\n');
    free(status_json);
    free(status_md);
  }
  free(nytrix_rel);
  free(archive_base_rel);
  free(json_rel);
  free(md_rel);
  free(status_dir);
  (void)sb_append(&md, "```\n");
  bool ok = md.data && write_file_text(markdown_path, md.data);
  free(md.data);
  return ok;
}

static int cmd_public_fuzz_all_old_paths(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *nytrix_arg = value_after_equals(argc, argv, 4, "--nytrix-root", "");
  const char *archive_arg =
      value_after_equals(argc, argv, 4, "--archive-dir",
                         "build/cache/old-nytrix");
  const char *json_path = value_after_equals(argc, argv, 4, "--json", "");
  const char *markdown_path = value_after_equals(argc, argv, 4, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after_equals(argc, argv, 4, "--md", "");
  const char *artifact_scan_arg =
      value_after_equals(argc, argv, 4, "--artifact-scan-dir", "");
  if (!artifact_scan_arg || !*artifact_scan_arg)
    artifact_scan_arg = value_after_equals(argc, argv, 4,
                                           "--scan-artifacts-dir",
                                           "build/fuzz");
  bool artifact_scan_enabled =
      !has_flag_after(argc, argv, 4, "--no-artifact-scan");
  bool probe = has_flag_after(argc, argv, 4, "--probe") ||
               has_flag_after(argc, argv, 4, "--compact");
  bool apply = has_flag_after(argc, argv, 4, "--apply");
  bool dry_run = !apply;
  if (has_flag_after(argc, argv, 4, "--dry-run") ||
      has_flag_after(argc, argv, 4, "--no-apply")) {
    apply = false;
    dry_run = true;
  }
  const char *wait_arg =
      value_after_equals(argc, argv, 4, "--wait-writers-s", "");
  if (!wait_arg || !*wait_arg)
    wait_arg = value_after_equals(argc, argv, 4, "--wait-old-writers-s", "0");
  int wait_writers_s = atoi(wait_arg ? wait_arg : "0");
  if (wait_writers_s < 0) wait_writers_s = 0;
  if (wait_writers_s > 3600) wait_writers_s = 3600;
  const char *settle_arg =
      value_after_equals(argc, argv, 4, "--settle-recent-writes-s", "120");
  int settle_recent_writes_s = atoi(settle_arg ? settle_arg : "120");
  if (settle_recent_writes_s < 0) settle_recent_writes_s = 0;
  if (settle_recent_writes_s > 3600) settle_recent_writes_s = 3600;

  char nytrix_root[4096] = {0};
  if (nytrix_arg && *nytrix_arg) {
    if (!old_paths_abs_arg(root, nytrix_arg, "", nytrix_root,
                           sizeof(nytrix_root))) {
      printf("{\"ok\":false,\"error\":\"bad-nytrix-root\"}\n");
      return 2;
    }
  } else if (!path_join(nytrix_root, sizeof(nytrix_root), root, "../nytrix")) {
    printf("{\"ok\":false,\"error\":\"bad-nytrix-root\"}\n");
    return 2;
  }

  char archive_dir[4096] = {0};
  char repo_cache[4096] = {0};
  char repo_build[4096] = {0};
  if (!old_paths_abs_arg(root, archive_arg, "build/cache/old-nytrix",
                         archive_dir, sizeof(archive_dir)) ||
      !path_join(repo_cache, sizeof(repo_cache), root, "build/cache") ||
      !path_under_directory(archive_dir, repo_cache)) {
    printf("{\"ok\":false,\"error\":\"archive-dir-outside-build-cache\"}\n");
    return 2;
  }
  (void)path_join(repo_build, sizeof(repo_build), root, "build");

  char artifact_scan_dir[4096] = {0};
  if (artifact_scan_enabled &&
      (!old_paths_abs_arg(root, artifact_scan_arg, "build/fuzz",
                          artifact_scan_dir, sizeof(artifact_scan_dir)) ||
       !repo_build[0] ||
       !path_under_directory(artifact_scan_dir, repo_build))) {
    printf("{\"ok\":false,\"error\":\"artifact-scan-dir-outside-build\"}\n");
    return 2;
  }

  char stamp[64] = {0};
  time_t now = time(NULL);
  struct tm tm_now;
  if (localtime_r(&now, &tm_now))
    (void)strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_now);
  if (!stamp[0]) snprintf(stamp, sizeof(stamp), "%ld", (long)now);
  char archive_run_dir[4096] = {0};
  snprintf(archive_run_dir, sizeof(archive_run_dir), "%s/%s-pid%ld",
           archive_dir, stamp, (long)getpid());

  fuzz_all_old_path_item_t items[] = {
      {"old-test-scratch", "build/cache/projects/test", "tmp-projects-test", "", "",
       false, false, false, false, false, false, ""},
      {"old-fuzz", "fuzz", "fuzz", "", "", false, false, false, false,
       false, false, ""},
      {"old-build-cache", "build/cache", "build-cache", "", "", false,
       false, false, false, false, false, ""},
  };
  int item_count = (int)(sizeof(items) / sizeof(items[0]));
  for (int i = 0; i < item_count; ++i) {
    (void)path_join(items[i].source, sizeof(items[i].source), nytrix_root,
                    items[i].rel);
    (void)path_join(items[i].archive, sizeof(items[i].archive),
                    archive_run_dir, items[i].archive_leaf);
  }

  char active_writer_detail[512] = {0};
  bool recent_old_cache_write = false;
  double recent_old_cache_write_age_seconds = -1.0;
  bool active_writer =
      old_paths_detect_active_or_recent_writer(
          nytrix_root, items[2].source, settle_recent_writes_s,
          active_writer_detail, sizeof(active_writer_detail),
          &recent_old_cache_write, &recent_old_cache_write_age_seconds);
  int waited_writers_s = 0;
  if (apply && !dry_run && active_writer && wait_writers_s > 0) {
    while (active_writer && waited_writers_s < wait_writers_s) {
      sleep(1);
      waited_writers_s++;
      active_writer = old_paths_detect_active_or_recent_writer(
          nytrix_root, items[2].source, settle_recent_writes_s,
          active_writer_detail, sizeof(active_writer_detail),
          &recent_old_cache_write, &recent_old_cache_write_age_seconds);
    }
  }
  double old_path_wait_remaining_seconds = -1.0;
  if (recent_old_cache_write && recent_old_cache_write_age_seconds >= 0.0) {
    old_path_wait_remaining_seconds =
        (double)settle_recent_writes_s - recent_old_cache_write_age_seconds;
    if (old_path_wait_remaining_seconds < 0.0)
      old_path_wait_remaining_seconds = 0.0;
  }
  string_list_t rows = {0}, failures = {0};
  int present_count = 0, moved_count = 0, remaining_count = 0;
  int artifact_item_count = 0, artifact_moved_count = 0;
  int artifact_remaining_count = 0;
  fuzz_all_old_path_item_t *artifact_items = NULL;
  if (artifact_scan_enabled && artifact_scan_dir[0]) {
    artifact_items = old_paths_collect_artifact_items(
        root, artifact_scan_dir, archive_run_dir, &artifact_item_count);
  }
  if (apply && !dry_run && active_writer) {
    (void)string_list_push_take(&failures,
                                make_worker_failure_row(
                                    "old-paths", "fuzz-all-old-paths", 1, "",
                                    "active old Nytrix writer detected"));
  }

  for (int i = 0; i < item_count; ++i) {
    items[i].present_before = exists_path(items[i].source);
    if (items[i].present_before) ++present_count;
    if (items[i].present_before && apply && !dry_run) {
      if (active_writer) {
        items[i].blocked = true;
        snprintf(items[i].error, sizeof(items[i].error),
                 "active old Nytrix writer detected");
      } else if (!path_under_directory(items[i].source, nytrix_root)) {
        items[i].failed = true;
        snprintf(items[i].error, sizeof(items[i].error),
                 "source escaped selected Nytrix root");
      } else if (exists_path(items[i].archive)) {
        items[i].failed = true;
        snprintf(items[i].error, sizeof(items[i].error),
                 "archive destination already exists");
      } else if (!mkdir_parent(items[i].archive)) {
        items[i].failed = true;
        snprintf(items[i].error, sizeof(items[i].error),
                 "archive parent creation failed");
      } else if (rename(items[i].source, items[i].archive) != 0) {
        items[i].failed = true;
        snprintf(items[i].error, sizeof(items[i].error), "rename failed: %s",
                 strerror(errno));
      } else {
        items[i].moved = true;
        ++moved_count;
      }
      if (items[i].failed) {
        (void)string_list_push_take(&failures,
                                    make_worker_failure_row(
                                        items[i].name, "fuzz-all-old-paths", 1,
                                        "", items[i].error));
      }
    }
    items[i].present_after = exists_path(items[i].source);
    if (items[i].present_after) ++remaining_count;
    (void)string_list_push_take(&rows,
                                make_fuzz_all_old_path_row(root, &items[i],
                                                           apply, dry_run));
  }

  for (int i = 0; i < artifact_item_count; ++i) {
    artifact_items[i].present_before = exists_path(artifact_items[i].source);
    if (artifact_items[i].present_before) ++present_count;
    if (artifact_items[i].present_before && apply && !dry_run) {
      if (!path_under_directory(artifact_items[i].source,
                                artifact_scan_dir)) {
        artifact_items[i].failed = true;
        snprintf(artifact_items[i].error, sizeof(artifact_items[i].error),
                 "artifact escaped scan dir");
      } else if (exists_path(artifact_items[i].archive)) {
        artifact_items[i].failed = true;
        snprintf(artifact_items[i].error, sizeof(artifact_items[i].error),
                 "archive destination already exists");
      } else if (!mkdir_parent(artifact_items[i].archive)) {
        artifact_items[i].failed = true;
        snprintf(artifact_items[i].error, sizeof(artifact_items[i].error),
                 "archive parent creation failed");
      } else if (rename(artifact_items[i].source,
                        artifact_items[i].archive) != 0) {
        artifact_items[i].failed = true;
        snprintf(artifact_items[i].error, sizeof(artifact_items[i].error),
                 "rename failed: %s", strerror(errno));
      } else {
        artifact_items[i].moved = true;
        ++moved_count;
        ++artifact_moved_count;
      }
      if (artifact_items[i].failed) {
        (void)string_list_push_take(&failures,
                                    make_worker_failure_row(
                                        artifact_items[i].name,
                                        "fuzz-all-old-paths", 1, "",
                                        artifact_items[i].error));
      }
    }
    artifact_items[i].present_after = exists_path(artifact_items[i].source);
    if (artifact_items[i].present_after) {
      ++remaining_count;
      ++artifact_remaining_count;
    }
    (void)string_list_push_take(
        &rows,
        make_fuzz_all_old_path_row(root, &artifact_items[i], apply, dry_run));
  }

  if (markdown_path && *markdown_path &&
      !write_fuzz_all_old_paths_markdown(markdown_path, root, nytrix_root,
                                         archive_dir, json_path, items,
                                         item_count, artifact_items,
                                         artifact_item_count,
                                         artifact_scan_dir,
                                         archive_run_dir, apply,
                                         dry_run, active_writer,
                                         wait_writers_s, waited_writers_s,
                                         present_count,
                                         moved_count, remaining_count)) {
    (void)string_list_push_take(&failures,
                                make_worker_failure_row(
                                    "old-paths", "fuzz-all-old-paths", 1, "",
                                    "old-path markdown write failed"));
  }

  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"dry_run\":");
  (void)sb_append(&extra, dry_run ? "true" : "false");
  (void)sb_append(&extra, ",\"apply\":");
  (void)sb_append(&extra, apply ? "true" : "false");
  (void)sb_append(&extra, ",\"nytrix_root\":");
  append_rel_json_str(&extra, root, nytrix_root);
  (void)sb_append(&extra, ",\"archive_dir\":");
  append_rel_json_str(&extra, root, archive_dir);
  (void)sb_append(&extra, ",\"archive_run_dir\":");
  append_rel_json_str(&extra, root, archive_run_dir);
  (void)sb_append(&extra, ",\"artifact_scan_enabled\":");
  (void)sb_append(&extra, artifact_scan_enabled ? "true" : "false");
  (void)sb_append(&extra, ",\"artifact_scan_dir\":");
  append_rel_json_str(&extra, root, artifact_scan_dir);
  (void)sb_append(&extra, ",\"artifact_leak_count\":");
  (void)sb_appendf(&extra, "%d", artifact_item_count);
  (void)sb_append(&extra, ",\"artifact_moved_count\":");
  (void)sb_appendf(&extra, "%d", artifact_moved_count);
  (void)sb_append(&extra, ",\"artifact_remaining_count\":");
  (void)sb_appendf(&extra, "%d", artifact_remaining_count);
  char tmp_dir[4096] = {0};
  char scratch_root[4096] = {0};
  char xdg_cache_home[4096] = {0};
  char nytrix_cache_dir[4096] = {0};
  (void)path_join(tmp_dir, sizeof(tmp_dir), root, "build/cache/tmp");
  (void)path_join(scratch_root, sizeof(scratch_root), root,
                  "build/cache/scratch");
  (void)path_join(xdg_cache_home, sizeof(xdg_cache_home), root,
                  "build/cache/xdg");
  (void)path_join(nytrix_cache_dir, sizeof(nytrix_cache_dir), root,
                  "build/cache/nytrix");
  bool old_test_absent = !exists_path(items[0].source);
  bool old_fuzz_absent = !exists_path(items[1].source);
  bool old_build_cache_absent = !exists_path(items[2].source);
  bool cache_policy_ok =
      path_under_directory(tmp_dir, repo_cache) &&
      path_under_directory(scratch_root, repo_cache) &&
      path_under_directory(xdg_cache_home, repo_cache) &&
      path_under_directory(nytrix_cache_dir, repo_cache) &&
      path_under_directory(archive_dir, repo_cache);
  (void)sb_append(&extra, ",\"tmp_dir\":");
  append_rel_json_str(&extra, root, tmp_dir);
  (void)sb_append(&extra, ",\"scratch_root\":");
  append_rel_json_str(&extra, root, scratch_root);
  (void)sb_append(&extra, ",\"xdg_cache_home\":");
  append_rel_json_str(&extra, root, xdg_cache_home);
  (void)sb_append(&extra, ",\"nytrix_cache_dir\":");
  append_rel_json_str(&extra, root, nytrix_cache_dir);
  (void)sb_append(&extra, ",\"cache_policy_ok\":");
  (void)sb_append(&extra, cache_policy_ok ? "true" : "false");
  (void)sb_append(&extra, ",\"old_path_cache_policy_ok\":");
  (void)sb_append(&extra, cache_policy_ok ? "true" : "false");
  (void)sb_append(&extra, ",\"old_nytrix_test_scratch_absent\":");
  (void)sb_append(&extra, old_test_absent ? "true" : "false");
  (void)sb_append(&extra, ",\"old_nytrix_fuzz_absent\":");
  (void)sb_append(&extra, old_fuzz_absent ? "true" : "false");
  (void)sb_append(&extra, ",\"old_nytrix_build_cache_absent\":");
  (void)sb_append(&extra, old_build_cache_absent ? "true" : "false");
  (void)sb_append(&extra, ",\"present_count\":");
  (void)sb_appendf(&extra, "%d", present_count);
  (void)sb_append(&extra, ",\"old_path_present_count\":");
  (void)sb_appendf(&extra, "%d", present_count);
  (void)sb_append(&extra, ",\"old_seen\":");
  (void)sb_appendf(&extra, "%d", present_count);
  (void)sb_append(&extra, ",\"moved_count\":");
  (void)sb_appendf(&extra, "%d", moved_count);
  (void)sb_append(&extra, ",\"old_path_moved_count\":");
  (void)sb_appendf(&extra, "%d", moved_count);
  (void)sb_append(&extra, ",\"old_moved\":");
  (void)sb_appendf(&extra, "%d", moved_count);
  (void)sb_append(&extra, ",\"remaining_count\":");
  (void)sb_appendf(&extra, "%d", remaining_count);
  (void)sb_append(&extra, ",\"old_path_remaining_count\":");
  (void)sb_appendf(&extra, "%d", remaining_count);
  (void)sb_append(&extra, ",\"old_current\":");
  (void)sb_appendf(&extra, "%d", remaining_count);
  (void)sb_append(&extra, ",\"old_path_artifact_leak_count\":");
  (void)sb_appendf(&extra, "%d", artifact_item_count);
  (void)sb_append(&extra, ",\"old_leaks\":");
  (void)sb_appendf(&extra, "%d", artifact_item_count);
  (void)sb_append(&extra, ",\"old_path_artifact_moved_count\":");
  (void)sb_appendf(&extra, "%d", artifact_moved_count);
  (void)sb_append(&extra, ",\"old_artifacts_moved\":");
  (void)sb_appendf(&extra, "%d", artifact_moved_count);
  (void)sb_append(&extra, ",\"old_path_artifact_remaining_count\":");
  (void)sb_appendf(&extra, "%d", artifact_remaining_count);
  (void)sb_append(&extra, ",\"artifact_remaining\":");
  (void)sb_appendf(&extra, "%d", artifact_remaining_count);
  (void)sb_append(&extra, ",\"active_old_nytrix_output_writer_present\":");
  (void)sb_append(&extra, active_writer ? "true" : "false");
  (void)sb_append(&extra, ",\"active_old_writer\":");
  (void)sb_append(&extra, active_writer ? "true" : "false");
  (void)sb_append(&extra, ",\"active_old_nytrix_output_writer\":");
  (void)sb_append_json_str(&extra, active_writer_detail);
  (void)sb_append(&extra, ",\"recent_old_cache_write\":");
  (void)sb_append(&extra, recent_old_cache_write ? "true" : "false");
  (void)sb_append(&extra, ",\"recent_old_cache_write_age_seconds\":");
  (void)sb_appendf(&extra, "%.0f", recent_old_cache_write_age_seconds);
  (void)sb_append(&extra, ",\"old_path_settle_recent_writes_s\":");
  (void)sb_appendf(&extra, "%d", settle_recent_writes_s);
  (void)sb_append(&extra, ",\"old_path_wait_remaining_seconds\":");
  (void)sb_appendf(&extra, "%.0f", old_path_wait_remaining_seconds);
  (void)sb_append(&extra, ",\"wait_remaining_s\":");
  (void)sb_appendf(&extra, "%.0f", old_path_wait_remaining_seconds);
  (void)sb_append(&extra, ",\"old_path_next_action\":");
  (void)sb_append_json_str(&extra, fuzz_all_old_path_next_action(
                                       remaining_count, active_writer));
  (void)sb_append(&extra, ",\"old_path_next_reason\":");
  (void)sb_append_json_str(&extra, fuzz_all_old_path_next_reason(
                                       remaining_count, active_writer));
  (void)sb_append(&extra, ",\"next\":{\"action\":");
  (void)sb_append_json_str(&extra, fuzz_all_old_path_next_action(
                                     remaining_count, active_writer));
  (void)sb_append(&extra, ",\"reason\":");
  (void)sb_append_json_str(&extra, fuzz_all_old_path_next_reason(
                                     remaining_count, active_writer));
  (void)sb_append(&extra, "}");
  (void)sb_append(&extra, ",\"wait_writers_s\":");
  (void)sb_appendf(&extra, "%d", wait_writers_s);
  (void)sb_append(&extra, ",\"waited_writers_s\":");
  (void)sb_appendf(&extra, "%d", waited_writers_s);
  (void)sb_append(&extra, ",\"old_writer_cleared_after_wait\":");
  (void)sb_append(&extra,
                  waited_writers_s > 0 && !active_writer ? "true" : "false");
  if (markdown_path && *markdown_path) {
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, root, markdown_path);
  }
  char *report = build_native_report_json_with_top_aliases(
      &rows, &failures, "fuzz-all-old-paths",
      extra.data ? extra.data : "", true);
  int rc = 0;
  if (probe) {
    if (json_path && *json_path && !write_file_text(json_path, report)) {
      printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
      json_str(stdout, json_path);
      printf("}\n");
      rc = 2;
    } else {
      str_buf_t compact = {0};
      (void)sb_append(&compact, "{\"ok\":");
      (void)sb_append(&compact, failures.count ? "false" : "true");
      (void)sb_append(&compact, ",\"cases\":");
      (void)sb_appendf(&compact, "%d", rows.count);
      (void)sb_append(&compact, ",\"ok_count\":");
      (void)sb_appendf(&compact, "%d", rows.count - failures.count);
      (void)sb_append(&compact, ",\"failure_count\":");
      (void)sb_appendf(&compact, "%d", failures.count);
      (void)sb_append(&compact, ",\"cache_policy_ok\":");
      (void)sb_append(&compact, cache_policy_ok ? "true" : "false");
      (void)sb_append(&compact, ",\"old_policy\":");
      (void)sb_append(&compact, cache_policy_ok ? "true" : "false");
      (void)sb_append(&compact, ",\"old_nytrix_test_scratch_absent\":");
      (void)sb_append(&compact, old_test_absent ? "true" : "false");
      (void)sb_append(&compact, ",\"old_nytrix_fuzz_absent\":");
      (void)sb_append(&compact, old_fuzz_absent ? "true" : "false");
      (void)sb_append(&compact, ",\"old_nytrix_build_cache_absent\":");
      (void)sb_append(&compact, old_build_cache_absent ? "true" : "false");
      (void)sb_append(&compact, ",\"old_seen\":");
      (void)sb_appendf(&compact, "%d", present_count);
      (void)sb_append(&compact, ",\"old_moved\":");
      (void)sb_appendf(&compact, "%d", moved_count);
      (void)sb_append(&compact, ",\"old_current\":");
      (void)sb_appendf(&compact, "%d", remaining_count);
      (void)sb_append(&compact, ",\"old_leaks\":");
      (void)sb_appendf(&compact, "%d", artifact_item_count);
      (void)sb_append(&compact, ",\"old_artifacts_moved\":");
      (void)sb_appendf(&compact, "%d", artifact_moved_count);
      (void)sb_append(&compact, ",\"artifact_remaining\":");
      (void)sb_appendf(&compact, "%d", artifact_remaining_count);
      (void)sb_append(&compact, ",\"active_old_writer\":");
      (void)sb_append(&compact, active_writer ? "true" : "false");
      (void)sb_append(&compact, ",\"recent_old_cache_write\":");
      (void)sb_append(&compact, recent_old_cache_write ? "true" : "false");
      (void)sb_append(&compact, ",\"recent_old_cache_write_age_seconds\":");
      (void)sb_appendf(&compact, "%.0f", recent_old_cache_write_age_seconds);
      (void)sb_append(&compact, ",\"wait_remaining_s\":");
      (void)sb_appendf(&compact, "%.0f", old_path_wait_remaining_seconds);
      (void)sb_append(&compact, ",\"artifact_scan_dir\":");
      append_rel_json_str(&compact, root, artifact_scan_dir);
      (void)sb_append(&compact, ",\"archive_run_dir\":");
      append_rel_json_str(&compact, root, archive_run_dir);
      (void)sb_append(&compact, ",\"scratch_root\":");
      append_rel_json_str(&compact, root, scratch_root);
      (void)sb_append(&compact, ",\"nytrix_cache_dir\":");
      append_rel_json_str(&compact, root, nytrix_cache_dir);
      (void)sb_append(&compact, ",\"next\":{\"action\":");
      (void)sb_append_json_str(&compact, fuzz_all_old_path_next_action(
                                             remaining_count, active_writer));
      (void)sb_append(&compact, ",\"reason\":");
      (void)sb_append_json_str(&compact, fuzz_all_old_path_next_reason(
                                             remaining_count, active_writer));
      (void)sb_append(&compact, "}}\n");
      fputs(compact.data ? compact.data : "{}\n", stdout);
      free(compact.data);
      rc = failures.count ? 1 : 0;
    }
    free(report);
  } else {
    rc = emit_native_report(report, json_path, "all fuzz old paths",
                            rows.count, failures.count);
  }
  char *archive_rel = rel_path_dup(root, archive_run_dir);
  if (!probe)
    printf("old paths: mode=%s active-writer=%s present=%d moved=%d remaining=%d archive=%s\n",
           dry_run ? "dry-run" : "apply",
           active_writer ? "present" : "none",
           present_count, moved_count, remaining_count,
           archive_rel && *archive_rel ? archive_rel : archive_run_dir);
  free(archive_rel);
  free(extra.data);
  free(artifact_items);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

typedef struct {
  double latest_report_age_seconds;
  double latest_full_pressure_report_age_seconds;
  double latest_report_stale_after_hours;
  double latest_full_pressure_report_stale_after_hours;
  bool latest_report_fresh;
  bool latest_full_pressure_report_fresh;
  bool evidence_fresh;
  double gate_penalty;
  double correctness_penalty;
  double perf_penalty;
  double advisory_penalty;
  double environment_penalty;
  double freshness_penalty;
  double signal_score;
  double evidence_cap;
  double stability_score;
  const char *label;
  char note[256];
} fuzz_all_score_summary_t;

typedef struct {
  double thread_years_per_run;
  const char *thread_years_per_run_source;
  double target_percent_per_run;
  double next_run_target_percent;
  double next_run_stability_score;
  double next_run_stability_delta;
  double runs_to_good_stability;
  double runs_to_good_days;
} fuzz_all_projection_summary_t;

static double fuzz_all_score_clamp(double v, double lo, double hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static double fuzz_all_score_good_threshold(void) {
  return 75.0;
}

static double fuzz_all_score_latest_fresh_hours(void) {
  return 24.0;
}

static double fuzz_all_score_full_pressure_fresh_hours(void) {
  return 72.0;
}

static const char *fuzz_all_score_label(double score) {
  if (score >= 90.0) return "high";
  if (score >= 75.0) return "good";
  if (score >= 60.0) return "promising";
  if (score >= 40.0) return "shaky";
  return "blocked";
}

static bool fuzz_all_score_age_fresh(double age_seconds,
                                     double stale_after_hours) {
  return age_seconds >= 0.0 &&
         age_seconds <= stale_after_hours * 3600.0;
}

static double fuzz_all_freshness_age_hours(double age_seconds) {
  return age_seconds >= 0.0 ? age_seconds / 3600.0 : -1.0;
}

static double fuzz_all_freshness_remaining_hours(double age_seconds,
                                                 double stale_after_hours) {
  if (age_seconds < 0.0 || stale_after_hours < 0.0) return -1.0;
  double remaining = stale_after_hours - fuzz_all_freshness_age_hours(age_seconds);
  return remaining > 0.0 ? remaining : 0.0;
}

static double fuzz_all_freshness_overdue_hours(double age_seconds,
                                               double stale_after_hours) {
  if (age_seconds < 0.0 || stale_after_hours < 0.0) return -1.0;
  double overdue = fuzz_all_freshness_age_hours(age_seconds) - stale_after_hours;
  return overdue > 0.0 ? overdue : 0.0;
}

static double fuzz_all_evidence_freshness_overdue_hours(
    double latest_age_seconds, double latest_stale_after_hours,
    double full_age_seconds, double full_stale_after_hours) {
  double latest = fuzz_all_freshness_overdue_hours(
      latest_age_seconds, latest_stale_after_hours);
  double full = fuzz_all_freshness_overdue_hours(
      full_age_seconds, full_stale_after_hours);
  if (latest < 0.0) return full;
  if (full < 0.0) return latest;
  return latest > full ? latest : full;
}

static void append_fuzz_all_compact_score_alias_fields(
    str_buf_t *b, double score_percent, const char *score_label,
    double next_run_language_score_percent, double latest_age_seconds,
    double latest_stale_after_hours, double full_age_seconds,
    double full_stale_after_hours) {
  if (!b) return;
  (void)sb_appendf(
      b,
      ",\"score\":%.2f,"
      "\"latest_h\":%.2f,"
      "\"latest_over_h\":%.2f,"
      "\"full_h\":%.2f,"
      "\"full_over_h\":%.2f,"
      "\"over_h\":%.2f,"
      "\"next_run_language_score\":%.2f",
      score_percent,
      fuzz_all_freshness_age_hours(latest_age_seconds),
      fuzz_all_freshness_overdue_hours(latest_age_seconds,
                                       latest_stale_after_hours),
      fuzz_all_freshness_age_hours(full_age_seconds),
      fuzz_all_freshness_overdue_hours(full_age_seconds,
                                       full_stale_after_hours),
      fuzz_all_evidence_freshness_overdue_hours(
          latest_age_seconds, latest_stale_after_hours,
          full_age_seconds, full_stale_after_hours),
      next_run_language_score_percent);
  (void)sb_append(b, ",\"score_label\":");
  (void)sb_append_json_str(b, score_label ? score_label : "");
}

static double fuzz_all_score_report_age_seconds(const char *root,
                                                const char *path) {
  if (!path || !*path) return -1.0;
  char abs[4096] = {0};
  if (path_is_absolute(path)) {
    snprintf(abs, sizeof(abs), "%s", path);
  } else if (root && *root) {
    (void)path_join(abs, sizeof(abs), root, path);
  } else {
    snprintf(abs, sizeof(abs), "%s", path);
  }
  struct stat st;
  if (!abs[0] || stat(abs, &st) != 0) return -1.0;
  double age = difftime(time(NULL), st.st_mtime);
  return age < 0.0 ? 0.0 : age;
}

static void fuzz_all_score_note(char *out, size_t out_sz,
                                bool ready, double blocker_count,
                                double active_items,
                                double freshness_penalty,
                                double advisory_penalty,
                                double signal_score,
                                double evidence_cap,
                                double target_percent) {
  if (!out || out_sz == 0) return;
  if (!ready || blocker_count > 0.0 || active_items > 0.0) {
    snprintf(out, out_sz, "current gate/worklist still limits stability");
  } else if (freshness_penalty > 0.0) {
    snprintf(out, out_sz,
             "stale latest/full-pressure evidence limits stability");
  } else if (signal_score > evidence_cap + 0.01) {
    snprintf(out, out_sz,
             "%s; evidence cap %.2f%% from %.4f%% campaign evidence",
             advisory_penalty > 0.0 ? "advisory timeout penalty" :
                                      "clean current gate",
             evidence_cap, target_percent);
  } else if (advisory_penalty > 0.0) {
    snprintf(out, out_sz, "advisory timeout penalty limits stability");
  } else {
    snprintf(out, out_sz, "score follows current bug/perf/fuzz signals");
  }
}

static void fuzz_all_score_signal_breakdown(bool ready, double blocker_count,
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
  double gate = 0.0;
  if (!ready) gate += 25.0;
  gate += fuzz_all_score_clamp(blocker_count, 0.0, 4.0) * 15.0;
  gate += fuzz_all_score_clamp(active_items, 0.0, 5.0) * 8.0;
  gate += fuzz_all_score_clamp(coverage_blocker_gaps, 0.0, 5.0) * 8.0;
  double correctness =
      fuzz_all_score_clamp(compiler_live, 0.0, 4.0) * 20.0 +
      fuzz_all_score_clamp(compiler_missing, 0.0, 4.0) * 15.0 +
      fuzz_all_score_clamp(known_reproduced, 0.0, 4.0) * 20.0 +
      fuzz_all_score_clamp(known_lost, 0.0, 4.0) * 15.0 +
      fuzz_all_score_clamp(known_baseline, 0.0, 4.0) * 15.0;
  double perf = fuzz_all_score_clamp(perf_hotspots, 0.0, 4.0) * 12.0;
  double advisory =
      fuzz_all_score_clamp(non_reproducing_timeouts, 0.0, 3.0) * 2.0;
  double env = 0.0;
  if (!cache_policy_ok) env += 15.0;
  if (!ny_bin_exists) env += 20.0;
  if (gate_penalty) *gate_penalty = gate;
  if (correctness_penalty) *correctness_penalty = correctness;
  if (perf_penalty) *perf_penalty = perf;
  if (advisory_penalty) *advisory_penalty = advisory;
  if (env_penalty) *env_penalty = env;
}

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
                                          bool ny_bin_exists) {
  double gate = 0.0, correctness = 0.0, perf = 0.0, advisory = 0.0, env = 0.0;
  fuzz_all_score_signal_breakdown(ready, blocker_count, active_items,
                                  coverage_blocker_gaps, compiler_live,
                                  compiler_missing, known_reproduced,
                                  known_lost, known_baseline, perf_hotspots,
                                  non_reproducing_timeouts, cache_policy_ok,
                                  ny_bin_exists, &gate, &correctness, &perf,
                                  &advisory, &env);
  return fuzz_all_score_clamp(100.0 - gate - correctness - perf - advisory - env,
                              0.0, 100.0);
}

static const char *fuzz_all_advisory_state(double current_advisory_timeouts,
                                           double historical_advisory_timeouts) {
  if (current_advisory_timeouts > 0.0) return "current-timeouts";
  if (historical_advisory_timeouts > 0.0) return "historical-timeouts";
  return "clear";
}

static const char *fuzz_all_advisory_recheck_state(bool advisory_present,
                                                   double raw_checked,
                                                   double raw_passed,
                                                   double raw_timeouts,
                                                   double raw_unexpected,
                                                   const char *command) {
  if (!advisory_present) return "clear";
  if (!command || !*command || raw_checked <= 0.0) return "missing";
  if (raw_passed == raw_checked && raw_timeouts == 0.0 &&
      raw_unexpected == 0.0)
    return "passed";
  return "attention";
}

static bool fuzz_all_advisory_recheck_passed(double current_advisory_timeouts,
                                             double raw_checked,
                                             double raw_passed,
                                             double raw_timeouts,
                                             double raw_unexpected,
                                             const char *command) {
  if (current_advisory_timeouts <= 0.0) return false;
  return strcmp(fuzz_all_advisory_recheck_state(
                    true, raw_checked, raw_passed, raw_timeouts,
                    raw_unexpected, command),
                "passed") == 0;
}

static double fuzz_all_effective_advisory_timeouts(
    double current_advisory_timeouts, double raw_checked, double raw_passed,
    double raw_timeouts, double raw_unexpected, const char *command) {
  if (current_advisory_timeouts <= 0.0) return 0.0;
  if (fuzz_all_advisory_recheck_passed(current_advisory_timeouts, raw_checked,
                                       raw_passed, raw_timeouts,
                                       raw_unexpected, command))
    return 0.0;
  return current_advisory_timeouts;
}

static const char *fuzz_all_advisory_penalty_state(
    double effective_advisory_timeouts) {
  return effective_advisory_timeouts > 0.0 ? "current-timeouts" : "clear";
}

static const char *fuzz_all_coverage_state(double coverage_lanes,
                                           double coverage_ran_lanes,
                                           double coverage_blocker_gaps,
                                           double coverage_failed_lanes) {
  if (coverage_blocker_gaps > 0.0 || coverage_failed_lanes > 0.0)
    return "blocked";
  if (coverage_lanes <= 0.0)
    return "missing";
  if (coverage_ran_lanes < coverage_lanes)
    return "partial";
  return "complete";
}

static double fuzz_all_score_evidence_cap(double target_percent,
                                          bool campaign_complete) {
  if (campaign_complete) return 100.0;
  return fuzz_all_score_clamp(55.0 + target_percent * 1.5, 0.0, 85.0);
}

static double fuzz_all_campaign_remaining_percent(double target_percent) {
  return fuzz_all_score_clamp(100.0 - target_percent, 0.0, 100.0);
}

static double fuzz_all_ratio_percent(double numerator, double denominator) {
  if (denominator <= 0.0 || numerator <= 0.0) return 0.0;
  return fuzz_all_score_clamp((numerator / denominator) * 100.0, 0.0, 100.0);
}

static double fuzz_all_not_run_lanes(double ran_lanes, double total_lanes) {
  if (total_lanes <= ran_lanes) return 0.0;
  return total_lanes - ran_lanes;
}

static double fuzz_all_perf_watchlist_threshold(void) {
  return 1.25;
}

static double fuzz_all_perf_watchlist_artifact_fresh_hours(void) {
  return 24.0;
}

static double fuzz_all_perf_slowdown_percent(double ratio) {
  if (ratio <= 1.0) return 0.0;
  return (ratio - 1.0) * 100.0;
}

static double fuzz_all_perf_watchlist_open(double perf_hotspots,
                                           double worst_ratio) {
  if (perf_hotspots > 0.0) return perf_hotspots;
  return worst_ratio >= fuzz_all_perf_watchlist_threshold() ? 1.0 : 0.0;
}

static double fuzz_all_perf_watchlist_effective_open(
    double perf_hotspots, double worst_ratio, bool artifact_readable,
    bool artifact_fresh, double artifact_hotspots) {
  if (artifact_readable && artifact_fresh)
    return artifact_hotspots > 0.0 ? artifact_hotspots : 0.0;
  return fuzz_all_perf_watchlist_open(perf_hotspots, worst_ratio);
}

static bool fuzz_all_perf_artifact_worst_available(bool artifact_readable,
                                                   bool artifact_fresh,
                                                   double artifact_ratio) {
  return artifact_readable && artifact_fresh && artifact_ratio > 0.0;
}

static double fuzz_all_perf_effective_worst_ratio(
    double worst_ratio, bool artifact_readable, bool artifact_fresh,
    double artifact_ratio) {
  if (fuzz_all_perf_artifact_worst_available(
          artifact_readable, artifact_fresh, artifact_ratio))
    return artifact_ratio;
  return worst_ratio;
}

static const char *fuzz_all_perf_effective_worst_case(
    const char *worst_case, bool artifact_readable, bool artifact_fresh,
    double artifact_ratio, const char *artifact_case) {
  if (fuzz_all_perf_artifact_worst_available(
          artifact_readable, artifact_fresh, artifact_ratio) &&
      artifact_case && *artifact_case)
    return artifact_case;
  return worst_case ? worst_case : "";
}

static const char *fuzz_all_perf_watchlist_state(double perf_watchlist_open,
                                                 bool artifact_readable,
                                                 bool artifact_fresh,
                                                 double artifact_hotspots) {
  if (artifact_readable && artifact_fresh) {
    if (artifact_hotspots > 0.0) return "watch-open";
    return perf_watchlist_open > 0.0 ? "watch-inferred" : "clear";
  }
  if (artifact_readable)
    return perf_watchlist_open > 0.0 ? "stale-watch" : "stale";
  return perf_watchlist_open > 0.0 ? "needs-triage" : "not-run";
}

static bool fuzz_all_perf_watchlist_state_valid(const char *state) {
  return state &&
         (strcmp(state, "watch-open") == 0 ||
          strcmp(state, "watch-inferred") == 0 ||
          strcmp(state, "clear") == 0 ||
          strcmp(state, "stale-watch") == 0 ||
          strcmp(state, "stale") == 0 ||
          strcmp(state, "needs-triage") == 0 ||
          strcmp(state, "not-run") == 0);
}

static const char *fuzz_all_perf_watchlist_action(const char *state) {
  if (!state || !*state) return "unknown";
  if (strcmp(state, "watch-open") == 0 ||
      strcmp(state, "watch-inferred") == 0)
    return "inspect-watchlist";
  if (strcmp(state, "clear") == 0) return "none";
  if (strcmp(state, "stale-watch") == 0 ||
      strcmp(state, "stale") == 0 ||
      strcmp(state, "needs-triage") == 0 ||
      strcmp(state, "not-run") == 0)
    return "refresh-watchlist";
  return "unknown";
}

static bool fuzz_all_perf_watchlist_action_valid(const char *action) {
  return action &&
         (strcmp(action, "inspect-watchlist") == 0 ||
          strcmp(action, "refresh-watchlist") == 0 ||
          strcmp(action, "none") == 0 ||
          strcmp(action, "unknown") == 0);
}

static bool fuzz_all_optimization_use_artifact(bool artifact_readable,
                                               bool artifact_fresh,
                                               double artifact_hotspots,
                                               double artifact_ratio) {
  return artifact_readable && artifact_fresh &&
         artifact_hotspots > 0.0 && artifact_ratio > 0.0;
}

static const char *fuzz_all_optimization_case(
    bool artifact_readable, bool artifact_fresh, double artifact_hotspots,
    const char *artifact_case, double artifact_ratio,
    double perf_watchlist_open, const char *perf_watchlist_case) {
  if (fuzz_all_optimization_use_artifact(artifact_readable, artifact_fresh,
                                         artifact_hotspots, artifact_ratio) &&
      artifact_case && *artifact_case)
    return artifact_case;
  if (perf_watchlist_open > 0.0 && perf_watchlist_case && *perf_watchlist_case)
    return perf_watchlist_case;
  return "";
}

static double fuzz_all_optimization_ratio(bool artifact_readable,
                                          bool artifact_fresh,
                                          double artifact_hotspots,
                                          double artifact_ratio,
                                          double perf_watchlist_open,
                                          double perf_worst_ratio) {
  if (fuzz_all_optimization_use_artifact(artifact_readable, artifact_fresh,
                                         artifact_hotspots, artifact_ratio))
    return artifact_ratio;
  if (perf_watchlist_open > 0.0 && perf_worst_ratio > 0.0)
    return perf_worst_ratio;
  return 0.0;
}

static const char *fuzz_all_optimization_artifact_path(
    bool artifact_readable, bool artifact_fresh, double artifact_hotspots,
    double artifact_ratio, const char *path) {
  if (fuzz_all_optimization_use_artifact(artifact_readable, artifact_fresh,
                                         artifact_hotspots, artifact_ratio) &&
      path && *path)
    return path;
  return "";
}

static char *fuzz_all_optimization_target_command(const char *ny_source,
                                                  const char *c_source,
                                                  const char *artifact) {
  if ((!ny_source || !*ny_source) && (!c_source || !*c_source) &&
      (!artifact || !*artifact))
    return strdup("");
  str_buf_t cmd = {0};
  (void)sb_append(&cmd, "sed -n '1,220p'");
  if (ny_source && *ny_source) {
    (void)sb_append_c(&cmd, ' ');
    append_shell_single_quoted(&cmd, ny_source);
  }
  if (c_source && *c_source) {
    (void)sb_append_c(&cmd, ' ');
    append_shell_single_quoted(&cmd, c_source);
  }
  if (artifact && *artifact) {
    (void)sb_append_c(&cmd, ' ');
    append_shell_single_quoted(&cmd, artifact);
  }
  char *out = sb_take(&cmd);
  return out ? out : strdup("");
}

static void fuzz_all_append_full_pressure_perf_markdown(
    str_buf_t *md, double hotspots, double ratio, const char *perf_case,
    double rows, double expected_rows, bool suite_current) {
  if (!md || ratio <= 0.0) return;
  (void)sb_append(md, "- Full-pressure perf provenance: ");
  if (perf_case && *perf_case)
    md_append_code(md, perf_case);
  else
    md_append_code(md, "unknown");
  (void)sb_appendf(md, " %.4fx (%.2f%% slower than C); hotspots %.0f",
                   ratio, fuzz_all_perf_slowdown_percent(ratio), hotspots);
  if (expected_rows > 0.0)
    (void)sb_appendf(md, "; rows %.0f/%.0f", rows, expected_rows);
  else if (rows > 0.0)
    (void)sb_appendf(md, "; rows %.0f", rows);
  (void)sb_appendf(md, "; suite `%s`.\n",
                   suite_current ? "current" : "stale");
}

static const char *fuzz_all_optimization_reason(const char *state) {
  if (!state || !*state) return "optimization state is unknown";
  if (strcmp(state, "watch-open") == 0)
    return "confirmed soft C-vs-Ny watchlist rows need inspection";
  if (strcmp(state, "watch-inferred") == 0)
    return "latest or full-pressure C-vs-Ny ratio exceeds the soft watch threshold";
  if (strcmp(state, "clear") == 0)
    return "no soft C-vs-Ny optimization watchlist rows are open";
  if (strcmp(state, "stale-watch") == 0)
    return "watchlist artifact is stale while a soft C-vs-Ny signal is open";
  if (strcmp(state, "stale") == 0)
    return "watchlist artifact is stale; refresh before trusting optimization state";
  if (strcmp(state, "needs-triage") == 0 || strcmp(state, "not-run") == 0)
    return "watchlist artifact is missing; run local perf triage";
  return "optimization state is unknown";
}

static char *fuzz_all_perf_watchlist_action_command(
    const char *state, const char *refresh_command, const char *markdown_path,
    const char *report_path) {
  const char *action = fuzz_all_perf_watchlist_action(state);
  if (strcmp(action, "inspect-watchlist") == 0) {
    if (markdown_path && *markdown_path) {
      char *out = NULL;
      if (asprintf(&out, "sed -n '1,160p' %s", markdown_path) >= 0)
        return out ? out : strdup("");
      return strdup("");
    }
    if (report_path && *report_path) {
      char *out = NULL;
      if (asprintf(&out,
                   "jq '.summary, .rows[] | select(.hot==true)' %s",
                   report_path) >= 0)
        return out ? out : strdup("");
      return strdup("");
    }
  }
  if (strcmp(action, "refresh-watchlist") == 0)
    return strdup(refresh_command ? refresh_command : "");
  return strdup("");
}

static char *fuzz_all_perf_watchlist_command(const char *dir_path) {
  const char *dir = dir_path && *dir_path ? dir_path : "build/fuzz/all";
  int limit = perf_real_case_count();
  if (limit < 1) limit = 5;
  char *raw = NULL;
  if (asprintf(&raw,
               "./build/nynth perf triage --fast --limit %d --threshold %.2f "
               "--json %s/perf-watchlist.json --markdown %s/perf-watchlist.md",
               limit, fuzz_all_perf_watchlist_threshold(), dir, dir) < 0)
      return strdup("");
  char *out = fuzz_all_low_priority_command_dup(raw);
  free(raw);
  return out;
  }

static char *fuzz_all_perf_watchlist_report_path(const char *dir_path,
                                                 bool markdown) {
  const char *dir = dir_path && *dir_path ? dir_path : "build/fuzz/all";
  char *out = NULL;
  if (asprintf(&out, "%s/%s", dir,
               markdown ? "perf-watchlist.md" : "perf-watchlist.json") < 0)
    return strdup("");
  return out;
}

static bool fuzz_all_perf_watchlist_target_paths(
    const char *root, const char *json, const char *target_case,
    double target_ratio, char *artifact, size_t artifact_sz,
    char *ny_source, size_t ny_source_sz, char *c_source,
    size_t c_source_sz) {
  if (artifact && artifact_sz) artifact[0] = '\0';
  if (ny_source && ny_source_sz) ny_source[0] = '\0';
  if (c_source && c_source_sz) c_source[0] = '\0';
  if (!json) return false;
  const char *rows_json = json_top_level_value_after_key(json, "rows");
  const char *rows_end =
      rows_json && *rows_json == '[' ? matching_json_end(rows_json, '[', ']')
                                     : NULL;
  if (!rows_json || !rows_end) return false;

  const char *best = NULL;
  const char *best_end = NULL;
  double best_ratio = 0.0;
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
    char *case_name = json_extract_string_range(p, obj_end + 1, "case");
    bool case_match =
        target_case && *target_case && case_name &&
        strcmp(case_name, target_case) == 0;
    bool choose = false;
    if (case_match) {
      choose = best == NULL;
      if (target_ratio > 0.0 && has_ratio) {
        double delta = ratio - target_ratio;
        if (delta < 0.0) delta = -delta;
        if (delta <= 0.0001) choose = true;
      }
    } else if ((!target_case || !*target_case) && has_ratio &&
               (!best || ratio > best_ratio)) {
      choose = true;
    }
    if (choose) {
      best = p;
      best_end = obj_end;
      if (has_ratio) best_ratio = ratio;
      if (case_match && target_ratio > 0.0 && has_ratio) {
        double delta = ratio - target_ratio;
        if (delta < 0.0) delta = -delta;
        if (delta <= 0.0001) {
          free(case_name);
          break;
        }
      }
    }
    free(case_name);
    p = obj_end + 1;
  }

  if (!best || !best_end) return false;
  char *artifact_value =
      json_extract_string_range(best, best_end + 1, "artifact");
  char *ny_value =
      json_extract_string_range(best, best_end + 1, "ny_source");
  char *c_value =
      json_extract_string_range(best, best_end + 1, "c_source");
  if (artifact && artifact_sz)
    status_set_rel_path(artifact, artifact_sz, root ? root : "",
                        artifact_value ? artifact_value : "");
  if (ny_source && ny_source_sz)
    status_set_rel_path(ny_source, ny_source_sz, root ? root : "",
                        ny_value ? ny_value : "");
  if (c_source && c_source_sz)
    status_set_rel_path(c_source, c_source_sz, root ? root : "",
                        c_value ? c_value : "");
  bool found = (artifact && artifact_sz && artifact[0]) ||
               (ny_source && ny_source_sz && ny_source[0]) ||
               (c_source && c_source_sz && c_source[0]);
  free(artifact_value);
  free(ny_value);
  free(c_value);
  return found;
}

static bool fuzz_all_load_perf_watchlist_artifact(
    const char *root, const char *report_path, double *hotspots,
    double *max_ratio, char *max_case, size_t max_case_sz,
    char *max_artifact, size_t max_artifact_sz, char *max_ny_source,
    size_t max_ny_source_sz, char *max_c_source, size_t max_c_source_sz) {
  if (hotspots) *hotspots = 0.0;
  if (max_ratio) *max_ratio = 0.0;
  if (max_case && max_case_sz) max_case[0] = '\0';
  if (max_artifact && max_artifact_sz) max_artifact[0] = '\0';
  if (max_ny_source && max_ny_source_sz) max_ny_source[0] = '\0';
  if (max_c_source && max_c_source_sz) max_c_source[0] = '\0';
  char *resolved = resolve_existing_file(root ? root : "", report_path);
  if (!resolved) return false;
  file_buf_t report = {0};
  bool ok = read_file(resolved, &report) && report.data;
  int artifact_hotspots = 0;
  double artifact_max_ratio = 0.0;
  char artifact_max_case[128] = {0};
  if (ok) {
    ok = summarize_perf_triage_report(report.data, &artifact_hotspots,
                                      &artifact_max_ratio, artifact_max_case,
                                      sizeof(artifact_max_case));
  }
  if (ok) {
    if (hotspots) *hotspots = (double)artifact_hotspots;
    if (max_ratio) *max_ratio = artifact_max_ratio;
    if (max_case && max_case_sz)
      snprintf(max_case, max_case_sz, "%s", artifact_max_case);
    (void)fuzz_all_perf_watchlist_target_paths(
        root, report.data, artifact_max_case, artifact_max_ratio, max_artifact,
        max_artifact_sz, max_ny_source, max_ny_source_sz, max_c_source,
        max_c_source_sz);
  }
  free(report.data);
  free(resolved);
  return ok;
}

static void fuzz_all_load_compiler_std_audit_summary(
    const char *root, fuzz_all_status_summary_t *s) {
  if (!s) return;
  status_set_rel_path(s->compiler_std_audit_report,
                      sizeof(s->compiler_std_audit_report), root,
                      NYNTH_COMPILER_STD_AUDIT_JSON);
  status_set_rel_path(s->compiler_std_audit_markdown,
                      sizeof(s->compiler_std_audit_markdown), root,
                      NYNTH_COMPILER_STD_AUDIT_MARKDOWN);
  snprintf(s->compiler_std_audit_command,
           sizeof(s->compiler_std_audit_command), "%s",
           NYNTH_COMPILER_STD_AUDIT_COMMAND);
  snprintf(s->runtime_surface_state, sizeof(s->runtime_surface_state),
           "unknown");
  snprintf(s->crt_surface_state, sizeof(s->crt_surface_state), "unknown");
  snprintf(s->crt_unreferenced_families,
           sizeof(s->crt_unreferenced_families), "[]");
  snprintf(s->crt_next_action, sizeof(s->crt_next_action), "none");
  s->crt_next_reason[0] = '\0';
  s->crt_next_unreferenced_family[0] = '\0';
  snprintf(s->crt_next_unreferenced_exports,
           sizeof(s->crt_next_unreferenced_exports), "[]");
  s->crt_next_definition_file[0] = '\0';
  snprintf(s->crt_next_definition_locations,
           sizeof(s->crt_next_definition_locations), "[]");
  s->crt_next_inspect_command[0] = '\0';

  char *resolved = resolve_existing_file(root ? root : "",
                                         NYNTH_COMPILER_STD_AUDIT_JSON);
  if (!resolved) return;
  file_buf_t report = {0};
  bool ok = read_file(resolved, &report) && report.data;
  free(resolved);
  if (!ok) return;

  s->compiler_std_audit_readable = true;
  char *v = summary_string_from_report(report.data, "markdown");
  if (v && *v)
    status_set_rel_path(s->compiler_std_audit_markdown,
                        sizeof(s->compiler_std_audit_markdown), root, v);
  free(v);
  v = summary_string_from_report(report.data, "runtime_surface_state");
  if (v && *v)
    snprintf(s->runtime_surface_state, sizeof(s->runtime_surface_state), "%s",
             v);
  free(v);
  v = summary_string_from_report(report.data, "crt_surface_state");
  if (v && *v)
    snprintf(s->crt_surface_state, sizeof(s->crt_surface_state), "%s", v);
  free(v);
  v = summary_string_from_report(report.data, "crt_top_unreferenced_family");
  if (v && *v)
    snprintf(s->crt_top_unreferenced_family,
             sizeof(s->crt_top_unreferenced_family), "%s", v);
  free(v);
  v = summary_string_from_report(report.data, "crt_next_action");
  if (v && *v)
    snprintf(s->crt_next_action, sizeof(s->crt_next_action), "%s", v);
  free(v);
  v = summary_string_from_report(report.data, "crt_next_reason");
  if (v && *v)
    snprintf(s->crt_next_reason, sizeof(s->crt_next_reason), "%s", v);
  free(v);
  v = summary_string_from_report(report.data, "crt_next_unreferenced_family");
  if (v && *v)
    snprintf(s->crt_next_unreferenced_family,
             sizeof(s->crt_next_unreferenced_family), "%s", v);
  free(v);
  v = summary_string_from_report(report.data, "crt_next_definition_file");
  if (v && *v)
    snprintf(s->crt_next_definition_file,
             sizeof(s->crt_next_definition_file), "%s", v);
  free(v);
  v = summary_string_from_report(report.data, "crt_next_inspect_command");
  if (v && *v)
    snprintf(s->crt_next_inspect_command,
             sizeof(s->crt_next_inspect_command), "%s", v);
  free(v);
  (void)summary_number_from_report(report.data, "runtime_exports",
                                   &s->runtime_exports);
  (void)summary_number_from_report(report.data, "direct_runtime_refs",
                                   &s->direct_runtime_refs);
  (void)summary_number_from_report(
      report.data, "runtime_export_coverage_percent",
      &s->runtime_export_coverage_percent);
  (void)summary_number_from_report(report.data, "runtime_unreferenced_percent",
                                   &s->runtime_unreferenced_percent);
  (void)summary_number_from_report(report.data, "runtime_unreferenced_count",
                                   &s->runtime_unreferenced_count);
  (void)summary_number_from_report(report.data, "runtime_wrapper_gap_count",
                                   &s->runtime_wrapper_gap_count);
  (void)summary_number_from_report(report.data, "crt_runtime_exports",
                                   &s->crt_runtime_exports);
  (void)summary_number_from_report(report.data, "crt_direct_refs",
                                   &s->crt_direct_refs);
  (void)summary_number_from_report(report.data, "crt_export_coverage_percent",
                                   &s->crt_export_coverage_percent);
  (void)summary_number_from_report(report.data, "crt_unreferenced_percent",
                                   &s->crt_unreferenced_percent);
  (void)summary_number_from_report(report.data, "crt_unreferenced_count",
                                   &s->crt_unreferenced_count);
  (void)summary_number_from_report(report.data, "crt_wrapper_gap_count",
                                   &s->crt_wrapper_gap_count);
  (void)summary_number_from_report(report.data, "crt_unreferenced_family_count",
                                   &s->crt_unreferenced_family_count);
  (void)summary_number_from_report(
      report.data, "crt_top_unreferenced_family_count",
      &s->crt_top_unreferenced_family_count);
  (void)summary_number_from_report(
      report.data, "crt_next_unreferenced_count",
      &s->crt_next_unreferenced_count);
  char *families = summary_array_from_report(report.data,
                                             "crt_unreferenced_families");
  if (families && families[0] == '[' &&
      strlen(families) < sizeof(s->crt_unreferenced_families)) {
    snprintf(s->crt_unreferenced_families,
             sizeof(s->crt_unreferenced_families), "%s", families);
  }
  free(families);
  char *next_exports = summary_array_from_report(
      report.data, "crt_next_unreferenced_exports");
  if (next_exports && next_exports[0] == '[' &&
      strlen(next_exports) < sizeof(s->crt_next_unreferenced_exports)) {
    snprintf(s->crt_next_unreferenced_exports,
             sizeof(s->crt_next_unreferenced_exports), "%s", next_exports);
  }
  free(next_exports);
  char *next_defs = summary_array_from_report(
      report.data, "crt_next_definition_locations");
  if (next_defs && next_defs[0] == '[' &&
      strlen(next_defs) < sizeof(s->crt_next_definition_locations)) {
    snprintf(s->crt_next_definition_locations,
             sizeof(s->crt_next_definition_locations), "%s", next_defs);
  }
  free(next_defs);
  free(report.data);
}

static void append_fuzz_all_compiler_std_audit_values(
    str_buf_t *row, bool readable, const char *report, const char *markdown,
    const char *command, const char *runtime_state,
    double runtime_exports, double direct_runtime_refs,
    double runtime_coverage_percent, double runtime_unreferenced_percent,
    double runtime_unreferenced_count, double runtime_wrapper_gap_count,
    const char *crt_state, double crt_runtime_exports, double crt_direct_refs,
    double crt_coverage_percent,
    double crt_unreferenced_percent, double crt_unreferenced_count,
    double crt_wrapper_gap_count, double crt_family_count,
    const char *crt_top_family, double crt_top_family_count,
    const char *crt_families, const char *crt_next_action,
    const char *crt_next_reason, const char *crt_next_family,
    double crt_next_count, const char *crt_next_exports,
    const char *crt_next_definition_file,
    const char *crt_next_definition_locations,
    const char *crt_next_inspect_command) {
  if (!row) return;
  (void)sb_append(row, ",\"compiler_std_audit_readable\":");
  (void)sb_append(row, readable ? "true" : "false");
  (void)sb_append(row, ",\"compiler_std_audit_report\":");
  (void)sb_append_json_str(row, report ? report : "");
  (void)sb_append(row, ",\"compiler_std_audit_markdown\":");
  (void)sb_append_json_str(row, markdown ? markdown : "");
  (void)sb_append(row, ",\"compiler_std_audit_command\":");
  (void)sb_append_json_str(row, command && *command ?
                                command : NYNTH_COMPILER_STD_AUDIT_COMMAND);
  (void)sb_append(row, ",\"runtime_surface_state\":");
  (void)sb_append_json_str(row, runtime_state && *runtime_state ?
                                runtime_state : "unknown");
  (void)sb_append(row, ",\"crt_surface_state\":");
  (void)sb_append_json_str(row, crt_state && *crt_state ?
                                crt_state : "unknown");
  (void)sb_append(row, ",\"runtime_surface_scope\":");
  (void)sb_append_json_str(row, NYNTH_RUNTIME_SURFACE_SCOPE);
  (void)sb_append(row, ",\"crt_surface_scope\":");
  (void)sb_append_json_str(row, NYNTH_CRT_SURFACE_SCOPE);
  (void)sb_append(row, ",\"crt_behavior_state\":");
  (void)sb_append_json_str(row, NYNTH_CRT_BEHAVIOR_STATE);
  (void)sb_append(row, ",\"crt_behavior_scope\":");
  (void)sb_append_json_str(row, NYNTH_CRT_BEHAVIOR_SCOPE);
  append_fuzz_all_crt_behavior_next_fields(row);
  (void)sb_appendf(
      row,
      ",\"runtime_exports\":%.0f,"
      "\"direct_runtime_refs\":%.0f,"
      "\"runtime_coverage_done\":%.0f,"
      "\"runtime_coverage_total\":%.0f,"
      "\"runtime_export_coverage_percent\":%.4f,"
      "\"runtime_unreferenced_percent\":%.4f,"
      "\"runtime_unreferenced_count\":%.0f,"
      "\"runtime_wrapper_gap_count\":%.0f,"
      "\"crt_runtime_exports\":%.0f,"
      "\"crt_direct_refs\":%.0f,"
      "\"crt_coverage_done\":%.0f,"
      "\"crt_coverage_total\":%.0f,"
      "\"crt_export_coverage_percent\":%.4f,"
      "\"crt_unreferenced_percent\":%.4f,"
      "\"crt_unreferenced_count\":%.0f,"
      "\"crt_wrapper_gap_count\":%.0f,"
      "\"crt_unreferenced_family_count\":%.0f,"
      "\"crt_top_unreferenced_family_count\":%.0f",
      runtime_exports, direct_runtime_refs, direct_runtime_refs,
      runtime_exports,
      runtime_coverage_percent, runtime_unreferenced_percent,
      runtime_unreferenced_count, runtime_wrapper_gap_count,
      crt_runtime_exports, crt_direct_refs, crt_direct_refs,
      crt_runtime_exports,
      crt_coverage_percent, crt_unreferenced_percent, crt_unreferenced_count,
      crt_wrapper_gap_count, crt_family_count, crt_top_family_count);
  (void)sb_append(row, ",\"crt_top_unreferenced_family\":");
  (void)sb_append_json_str(row, crt_top_family ? crt_top_family : "");
  (void)sb_append(row, ",\"crt_unreferenced_families\":");
  (void)sb_append(row, crt_families && crt_families[0] == '[' ?
                       crt_families : "[]");
  (void)sb_append(row, ",\"crt_next_action\":");
  (void)sb_append_json_str(row, crt_next_action && *crt_next_action ?
                                crt_next_action : "none");
  (void)sb_append(row, ",\"crt_next_reason\":");
  (void)sb_append_json_str(row, crt_next_reason ? crt_next_reason : "");
  (void)sb_append(row, ",\"crt_next_unreferenced_family\":");
  (void)sb_append_json_str(row, crt_next_family ? crt_next_family : "");
  (void)sb_appendf(row, ",\"crt_next_unreferenced_count\":%.0f",
                   crt_next_count);
  (void)sb_append(row, ",\"crt_next_unreferenced_exports\":");
  (void)sb_append(row, crt_next_exports && crt_next_exports[0] == '[' ?
                       crt_next_exports : "[]");
  (void)sb_append(row, ",\"crt_next_definition_file\":");
  (void)sb_append_json_str(row, crt_next_definition_file ?
                                crt_next_definition_file : "");
  (void)sb_append(row, ",\"crt_next_definition_locations\":");
  (void)sb_append(row, crt_next_definition_locations &&
                           crt_next_definition_locations[0] == '[' ?
                       crt_next_definition_locations : "[]");
  (void)sb_append(row, ",\"crt_next_inspect_command\":");
  (void)sb_append_json_str(row, crt_next_inspect_command ?
                                crt_next_inspect_command : "");
}

static void append_fuzz_all_compiler_std_audit_fields(
    str_buf_t *row, const fuzz_all_status_summary_t *s) {
  append_fuzz_all_compiler_std_audit_values(
      row, s && s->compiler_std_audit_readable,
      s ? s->compiler_std_audit_report : "",
      s ? s->compiler_std_audit_markdown : "",
      s ? s->compiler_std_audit_command : "",
      s ? s->runtime_surface_state : "",
      s ? s->runtime_exports : 0.0,
      s ? s->direct_runtime_refs : 0.0,
      s ? s->runtime_export_coverage_percent : 0.0,
      s ? s->runtime_unreferenced_percent : 0.0,
      s ? s->runtime_unreferenced_count : 0.0,
      s ? s->runtime_wrapper_gap_count : 0.0, s ? s->crt_surface_state : "",
      s ? s->crt_runtime_exports : 0.0,
      s ? s->crt_direct_refs : 0.0,
      s ? s->crt_export_coverage_percent : 0.0,
      s ? s->crt_unreferenced_percent : 0.0,
      s ? s->crt_unreferenced_count : 0.0,
      s ? s->crt_wrapper_gap_count : 0.0,
      s ? s->crt_unreferenced_family_count : 0.0,
      s ? s->crt_top_unreferenced_family : "",
      s ? s->crt_top_unreferenced_family_count : 0.0,
      s ? s->crt_unreferenced_families : "[]",
      s ? s->crt_next_action : "none",
      s ? s->crt_next_reason : "",
      s ? s->crt_next_unreferenced_family : "",
      s ? s->crt_next_unreferenced_count : 0.0,
      s ? s->crt_next_unreferenced_exports : "[]",
      s ? s->crt_next_definition_file : "",
      s ? s->crt_next_definition_locations : "[]",
      s ? s->crt_next_inspect_command : "");
}

static void append_fuzz_all_compiler_std_audit_markdown(
    str_buf_t *md, bool readable, const char *markdown, const char *report,
    const char *command, const char *runtime_state, const char *crt_state,
    double crt_coverage_percent, double crt_unreferenced_count,
    double crt_unreferenced_percent, double crt_family_count,
    const char *crt_top_family, double crt_top_family_count,
    const char *crt_next_action, const char *crt_next_family,
    double crt_next_count) {
  if (!md) return;
  if (!readable) {
    (void)sb_append(
        md,
        "- Compiler std audit: not-readable; CRT families unavailable; refresh ");
    md_append_code(md, command && *command ? command :
                        NYNTH_COMPILER_STD_AUDIT_COMMAND);
    (void)sb_append(md, ".\n");
    return;
  }
  (void)sb_append(md, "- Compiler std audit: runtime ");
  md_append_code(md, runtime_state && *runtime_state ? runtime_state : "unknown");
  (void)sb_append(md, "; CRT ");
  md_append_code(md, crt_state && *crt_state ? crt_state : "unknown");
  (void)sb_append(md, "; scope ");
  md_append_code(md, NYNTH_CRT_SURFACE_SCOPE);
  (void)sb_append(md, "; behavior ");
  md_append_code(md, NYNTH_CRT_BEHAVIOR_STATE);
  (void)sb_append(md, "; behavior next ");
  md_append_code(md, NYNTH_CRT_BEHAVIOR_NEXT_ACTION);
  (void)sb_appendf(md,
                   "; coverage %.2f%%; unreferenced %.0f (%.2f%%); families %.0f top ",
                   crt_coverage_percent, crt_unreferenced_count,
                   crt_unreferenced_percent, crt_family_count);
  md_append_code(md, crt_top_family && *crt_top_family ?
                      crt_top_family : "none");
  (void)sb_appendf(md, " %.0f", crt_top_family_count);
  if (crt_next_action && strcmp(crt_next_action, "none") != 0) {
    (void)sb_append(md, "; next ");
    md_append_code(md, crt_next_action);
    (void)sb_append(md, " ");
    md_append_code(md, crt_next_family && *crt_next_family ?
                        crt_next_family : "none");
    (void)sb_appendf(md, "/%.0f", crt_next_count);
  }
  (void)sb_append(md, "; report ");
  md_append_code(md, markdown && *markdown ? markdown :
                      (report && *report ? report :
                       NYNTH_COMPILER_STD_AUDIT_MARKDOWN));
  (void)sb_append(md, ".\n");
}

typedef struct {
  const char *action;
  const char *reason;
  char command[4096];
  char low_cpu_command[4096];
  char preview_command[4096];
  char repeat_mode[16];
  double repeat_count;
} fuzz_all_recommendation_t;

static void fuzz_all_recommendation_command(fuzz_all_recommendation_t *out,
                                            const char *command) {
  if (!out) return;
  snprintf(out->command, sizeof(out->command), "%s",
           command && *command ? command : "");
}

static void fuzz_all_env_command(char *out, size_t out_size,
                                 const char *assignments,
                                 const char *command) {
  if (!out || out_size == 0) return;
  if (!command || !*command) {
    out[0] = '\0';
    return;
  }
  const char *parts_with_env[] = {"env ", assignments, " nice -n 10 ",
                                  command, NULL};
  const char *parts_plain[] = {command, NULL};
  const char **parts = assignments && *assignments ? parts_with_env :
                                                  parts_plain;
  size_t pos = 0;
  out[0] = '\0';
  for (size_t i = 0; parts[i]; ++i) {
    size_t n = strlen(parts[i]);
    if (pos + n >= out_size)
      n = out_size > pos + 1 ? out_size - pos - 1 : 0;
    if (n > 0) {
      memcpy(out + pos, parts[i], n);
      pos += n;
      out[pos] = '\0';
    }
    if (pos + 1 >= out_size) break;
  }
}

static bool fuzz_all_command_uses_env_nice(const char *command) {
  return command &&
         strncmp(command, "env NYNTH_LOW_PRIORITY=1", 24) == 0 &&
         strstr(command, "NYNTH_RUN_NICE=10") &&
         strstr(command, "nice -n 10");
}

static char *fuzz_all_low_priority_command_dup(const char *command) {
  if (!command || !*command) return strdup("");
  if (fuzz_all_command_uses_env_nice(command)) return strdup(command);
  char *out = NULL;
  (void)asprintf(&out,
                 "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                 "nice -n 10 %s",
                 command);
  return out ? out : strdup("");
}

static void fuzz_all_gentle_run_command(char *out, size_t out_size,
                                        const char *command) {
  fuzz_all_env_command(out, out_size,
                       "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                       "NYNTH_RUN_HOURS=1 NYNTH_RUN_THREADS=10%",
                       command);
}

static void fuzz_all_gentle_preview_command(char *out, size_t out_size,
                                            const char *command) {
  fuzz_all_env_command(out, out_size,
                       "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                       "NYNTH_RUN_DRY_RUN=1 "
                       "NYNTH_RUN_HOURS=1 NYNTH_RUN_THREADS=10%",
                       command);
}

static void fuzz_all_recommendation_low_cpu(fuzz_all_recommendation_t *out,
                                            const char *command) {
  if (!out) return;
  snprintf(out->low_cpu_command, sizeof(out->low_cpu_command), "%s",
           command && *command ? command : "");
}

static void fuzz_all_recommendation_preview(fuzz_all_recommendation_t *out,
                                            const char *prefix,
                                            const char *command) {
  if (!out) return;
  if (prefix && *prefix && command && *command)
    fuzz_all_env_command(out->preview_command, sizeof(out->preview_command),
                         prefix, command);
  else
    out->preview_command[0] = '\0';
}

static void fuzz_all_recommendation_prefixed(fuzz_all_recommendation_t *out,
                                             const char *prefix,
                                             const char *command) {
  if (!out) return;
  if (prefix && *prefix && command && *command)
    fuzz_all_env_command(out->command, sizeof(out->command), prefix, command);
  else
    fuzz_all_recommendation_command(out, command);
}

static void fuzz_all_freshness_action_command(char *out, size_t out_size,
                                              const char *next_command) {
  if (!out || out_size == 0) return;
  if (!next_command || !*next_command) {
    out[0] = '\0';
    return;
  }
  fuzz_all_env_command(out, out_size, "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10",
                       next_command);
}

static void fuzz_all_recommendation_repeat(fuzz_all_recommendation_t *out,
                                           const char *mode,
                                           double count) {
  if (!out) return;
  snprintf(out->repeat_mode, sizeof(out->repeat_mode), "%s",
           mode && *mode ? mode : "");
  out->repeat_count = count > 0.0 ? count : 0.0;
}

static void md_append_unique_shell_line(str_buf_t *md, string_list_t *seen,
                                        const char *command) {
  if (!md || !command || !*command) return;
  if (seen && string_list_contains(seen, command)) return;
  (void)sb_append(md, command);
  (void)sb_append(md, "\n");
  if (seen) (void)string_list_push_unique_copy(seen, command);
}

static char *fuzz_all_state_command(const char *state_file);
static const char *selftest_catalog_catalog_command(void);
static const char *selftest_catalog_result_probe_command(void);
static const char *selftest_catalog_cockpit_command(void);
static const char *selftest_catalog_cockpit_result_probe_command(void);

static char *fuzz_all_quick_jq_command(const char *json_path) {
  const char *path = json_path && *json_path ? json_path :
      "build/fuzz/all/status.json";
  char *out = NULL;
  (void)asprintf(&out, "jq " NYNTH_FUZZ_ALL_QUICK_JQ_EXPR " %s", path);
  return out ? out : strdup("");
}

static bool fuzz_all_known_bugs_summary_from_artifact(
    const char *root, int *known_bug_count, int *reproduced,
    int *fixed_candidates, int *lost_signal, int *baseline_failures) {
  char path[4096] = {0};
  if (root && *root)
    (void)path_join(path, sizeof(path), root,
                    NYNTH_FUZZ_ALL_KNOWN_BUGS_REPORT);
  else
    snprintf(path, sizeof(path), "%s", NYNTH_FUZZ_ALL_KNOWN_BUGS_REPORT);
  file_buf_t report = {0};
  bool ok = read_file(path, &report) && report.data &&
            summarize_known_bug_report(report.data, known_bug_count,
                                       reproduced, fixed_candidates,
                                       lost_signal, baseline_failures);
  free(report.data);
  return ok;
}

static void append_fuzz_all_known_bugs_proof_fields(str_buf_t *b,
                                                    const char *root) {
  if (!b) return;
  int known_bug_count = 0, reproduced = 0, fixed_candidates = 0;
  int lost_signal = 0, baseline_failures = 0;
  bool readable = fuzz_all_known_bugs_summary_from_artifact(
      root, &known_bug_count, &reproduced, &fixed_candidates, &lost_signal,
      &baseline_failures);
  bool strict_open = reproduced > 0 || lost_signal > 0 || baseline_failures > 0;
  (void)sb_append(b, ",\"known_bugs_report\":");
  (void)sb_append_json_str(b, NYNTH_FUZZ_ALL_KNOWN_BUGS_REPORT);
  (void)sb_append(b, ",\"known_bugs_markdown\":\"\"");
  (void)sb_append(b, ",\"known_bugs_result_probe_command\":");
  (void)sb_append_json_str(b, NYNTH_FUZZ_ALL_KNOWN_BUGS_PROBE);
  (void)sb_appendf(
      b,
      ",\"known_bugs_readable\":%s,"
      "\"known_bug_count\":%d,"
      "\"reproduced\":%d,"
      "\"fixed_candidates\":%d,"
      "\"lost_signal\":%d,"
      "\"baseline_failures\":%d,"
      "\"known_bug_reproduced\":%d,"
      "\"known_bug_fixed_candidates\":%d,"
      "\"known_bug_lost_signal\":%d,"
      "\"known_bug_baseline_failures\":%d,"
      "\"strict_open\":%s,"
      "\"out_dir\":\"build/repro/known_bugs\"",
      readable ? "true" : "false", known_bug_count, reproduced,
      fixed_candidates, lost_signal, baseline_failures, reproduced,
      fixed_candidates, lost_signal, baseline_failures,
      strict_open ? "true" : "false");
}

static bool fuzz_all_perf_triage_summary_from_artifact(
    const char *root, double *cases, double *ok_count, double *failure_count,
    int *hotspots, double *worst_ratio, char *worst_case, size_t worst_case_sz,
    double *worst_slowdown_percent) {
  if (cases) *cases = 0.0;
  if (ok_count) *ok_count = 0.0;
  if (failure_count) *failure_count = 0.0;
  if (hotspots) *hotspots = 0;
  if (worst_ratio) *worst_ratio = 0.0;
  if (worst_case && worst_case_sz) worst_case[0] = '\0';
  if (worst_slowdown_percent) *worst_slowdown_percent = 0.0;
  char path[4096] = {0};
  if (root && *root)
    (void)path_join(path, sizeof(path), root,
                    NYNTH_FUZZ_ALL_PERF_TRIAGE_REPORT);
  else
    snprintf(path, sizeof(path), "%s", NYNTH_FUZZ_ALL_PERF_TRIAGE_REPORT);
  file_buf_t report = {0};
  bool readable = read_file(path, &report) && report.data;
  if (!readable) {
    free(report.data);
    return false;
  }
  double parsed_cases = 0.0, parsed_ok = 0.0, parsed_failures = 0.0;
  double parsed_slowdown = 0.0;
  int parsed_hotspots = 0;
  double parsed_ratio = 0.0;
  char parsed_case[128] = {0};
  bool summarized = summarize_perf_triage_report(
      report.data, &parsed_hotspots, &parsed_ratio, parsed_case,
      sizeof(parsed_case));
  (void)summary_number_from_report(report.data, "cases", &parsed_cases);
  (void)summary_number_from_report(report.data, "ok_count", &parsed_ok);
  (void)summary_number_from_report(report.data, "failure_count",
                                   &parsed_failures);
  if (!summary_number_from_report(report.data,
                                  "perf_worst_slowdown_percent",
                                  &parsed_slowdown))
    parsed_slowdown = (parsed_ratio - 1.0) * 100.0;
  if (cases) *cases = parsed_cases;
  if (ok_count) *ok_count = parsed_ok;
  if (failure_count) *failure_count = parsed_failures;
  if (hotspots) *hotspots = parsed_hotspots;
  if (worst_ratio) *worst_ratio = parsed_ratio;
  if (worst_case && worst_case_sz)
    snprintf(worst_case, worst_case_sz, "%s", parsed_case);
  if (worst_slowdown_percent) *worst_slowdown_percent = parsed_slowdown;
  free(report.data);
  return summarized;
}

static void append_fuzz_all_perf_triage_proof_fields(str_buf_t *b,
                                                     const char *root) {
  if (!b) return;
  double cases = 0.0, ok_count = 0.0, failure_count = 0.0;
  double worst_ratio = 0.0, worst_slowdown = 0.0;
  int hotspots = 0;
  char worst_case[128] = {0};
  bool readable = fuzz_all_perf_triage_summary_from_artifact(
      root, &cases, &ok_count, &failure_count, &hotspots, &worst_ratio,
      worst_case, sizeof(worst_case), &worst_slowdown);
  (void)sb_append(b, ",\"perf_triage_report\":");
  (void)sb_append_json_str(b, NYNTH_FUZZ_ALL_PERF_TRIAGE_REPORT);
  (void)sb_append(b, ",\"perf_triage_markdown\":");
  (void)sb_append_json_str(b, NYNTH_FUZZ_ALL_PERF_TRIAGE_MARKDOWN);
  (void)sb_append(b, ",\"perf_triage_result_probe_command\":");
  (void)sb_append_json_str(b, NYNTH_FUZZ_ALL_PERF_TRIAGE_PROBE);
  (void)sb_appendf(
      b,
      ",\"perf_triage_readable\":%s,"
      "\"perf_triage_cases\":%.0f,"
      "\"perf_triage_ok_count\":%.0f,"
      "\"perf_triage_failure_count\":%.0f,"
      "\"perf_triage_hotspots\":%d,"
      "\"perf_triage_worst_ratio\":%.4f,"
      "\"perf_triage_worst_slowdown_percent\":%.2f,"
      "\"perf_triage_worst_case\":",
      readable ? "true" : "false", cases, ok_count, failure_count, hotspots,
      worst_ratio, worst_slowdown);
  (void)sb_append_json_str(b, worst_case);
}

static void append_fuzz_all_probe_alias_fields(str_buf_t *b,
                                               const char *root,
                                               const char *report_json_path,
                                               const char *state_file) {
  if (!b) return;
  const char *report_path = report_json_path && *report_json_path ?
      report_json_path : "build/fuzz/all/status.json";
  const char *state_path = state_file && *state_file ?
      state_file : "build/fuzz/all/run-next-state.json";
  char *quick_probe = fuzz_all_quick_jq_command(report_path);
  char *state_probe = fuzz_all_state_command(state_path);
  (void)sb_append(b, ",\"quick_probe_command\":");
  (void)sb_append_json_str(b, quick_probe ? quick_probe : "");
  (void)sb_append(b, ",\"state_probe_command\":");
  (void)sb_append_json_str(b, state_probe ? state_probe : "");
  (void)sb_append(b, ",\"selftest_catalog_command\":");
  (void)sb_append_json_str(b, selftest_catalog_catalog_command());
  (void)sb_append(b, ",\"selftest_result_probe_command\":");
  (void)sb_append_json_str(b, selftest_catalog_result_probe_command());
  (void)sb_append(b, ",\"selftest_cockpit_run_command\":");
  (void)sb_append_json_str(b, selftest_catalog_cockpit_command());
  (void)sb_append(b, ",\"selftest_cockpit_result_probe_command\":");
  (void)sb_append_json_str(b,
                           selftest_catalog_cockpit_result_probe_command());
  (void)sb_append(b, ",\"known_bugs_command\":");
  (void)sb_append_json_str(b, NYNTH_FUZZ_ALL_KNOWN_BUGS_COMMAND);
  append_fuzz_all_known_bugs_proof_fields(b, root);
  (void)sb_append(b, ",\"perf_triage_command\":");
  (void)sb_append_json_str(b, NYNTH_FUZZ_ALL_PERF_TRIAGE_COMMAND);
  append_fuzz_all_perf_triage_proof_fields(b, root);
  free(quick_probe);
  free(state_probe);
}

static char *fuzz_all_compact_jq_command(const char *json_path) {
  const char *path = json_path && *json_path ? json_path :
      "build/fuzz/all/status.json";
  char *out = NULL;
  (void)asprintf(
      &out,
      "jq {ok,cases,ok_count,failure_count,ready,blockers,active_count,"
      "coverage_percent,coverage_queue_count,"
      "coverage_queue_non_advisory_count,coverage_queue_advisory_count,"
      "coverage_queue_lanes,campaign_percent,campaign_remaining_percent,"
      "thread_years,target_thread_years,score_percent,stability_percent,"
      "stability_score_percent,language_score_percent,language_score_label,"
      "completion_state,language_score_good_threshold_percent,"
      "language_score_signal_percent,language_score_evidence_cap_percent,"
      "language_score_note,language_score_gap_percent,"
      "next_run_language_score_percent,next_run_language_score_delta_percent,"
      "runs_to_good_language_score,runs_to_good_days,"
      "runs_to_good_language_days,days_to_good_language_score,"
      "days_to_good_stability,"
      "reports,full_pressure_reports,checked_subcases,"
      "full_pressure_thread_years,latest_report,latest_full_pressure_report,"
      "latest_full_pressure_raw_ok,"
      "latest_full_pressure_effective_clean,"
      "latest_full_pressure_clean_reason,"
      "latest_full_pressure_failure_count,"
      "latest_full_pressure_demoted_non_reproducing_afl_timeout,"
      "recommended_action,recommended_reason,recommended_command,"
      "recommended_preview_command,recommended_repeat_mode,"
      "recommended_repeat_count,"
      "recommended_state_fresh,recommended_state_stale_after_seconds,"
      "recommended_state_stale_reason,recommended_state_refresh_required,"
      "state,state_phase,state_event,state_age_seconds,"
      "state_stale_after_seconds,state_fresh,"
      "state_live,state_child_status,state_command,state_refresh_command,"
      "state_stale_reason,"
      "recommended_state,"
      "recommended_state_live,recommended_state_age_seconds,"
      "recommended_state_child_status,recommended_state_command,"
      "recommended_state_refresh_command,recommended_state_refresh_reason,"
      "recommended_low_cpu_command,run_next_command,"
      "run_next_preview_command,run_next_low_cpu_command,"
      "run_next_gentle_command,run_next_gentle_preview_command,"
      "coverage_next_action,"
      "coverage_next_category,coverage_next_severity,coverage_next_lane,"
      "coverage_next_reason,coverage_next_command,"
      "coverage_next_guarded_command,coverage_next_low_cpu_command,"
      "coverage_next_preview_command,coverage_next_state_file,"
      "coverage_next_state,coverage_next_state_phase,"
      "coverage_next_state_event,coverage_next_state_readable,"
      "coverage_next_state_fresh,coverage_next_state_live,"
      "coverage_next_state_age_seconds,"
      "coverage_next_state_stale_after_seconds,"
      "coverage_next_state_stale_reason,"
      "coverage_next_state_child_status,"
      "coverage_next_state_command,coverage_next_state_refresh_command,"
      "coverage_next_state_refresh_required,"
      "coverage_next_state_refresh_reason,coverage_next_stop_file,"
      "coverage_next_stop_command,coverage_next_resume_command,"
      "latest_report_fresh,latest_full_pressure_report_fresh,"
      "latest_full_pressure_report_age_hours,perf_watchlist_artifact_fresh,"
      "perf_watchlist_artifact_age_seconds,perf_watchlist_threshold_ratio,"
      "perf_watchlist_command,perf_watchlist_report,perf_watchlist_markdown,"
      "perf_watchlist_action,perf_watchlist_action_command,"
      "optimization_action,optimization_reason,optimization_command,"
      "optimization_target_command,optimization_case,optimization_ratio,"
      "optimization_slowdown_percent,"
      "perf_watchlist_artifact_hotspots,perf_watchlist_artifact_max_ratio,"
      "perf_watchlist_artifact_max_case,"
      "advisory_state,advisory_recheck_state,current_advisory_timeouts,"
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
      "runtime_export_coverage_percent,runtime_unreferenced_count,"
      "runtime_wrapper_gap_count,crt_surface_state,crt_surface_scope,"
      "crt_behavior_state,crt_behavior_scope,crt_behavior_next_action,"
      "crt_behavior_next_reason,crt_behavior_next_command,"
      "crt_runtime_exports,crt_direct_refs,"
      "crt_coverage_done,crt_coverage_total,"
      "crt_export_coverage_percent,crt_unreferenced_percent,"
      "crt_unreferenced_count,crt_wrapper_gap_count,"
      "crt_unreferenced_family_count,crt_top_unreferenced_family,"
      "crt_top_unreferenced_family_count,crt_next_action,"
      "crt_next_unreferenced_family,crt_next_unreferenced_count,"
      "perf_hotspots_open,perf_worst_ratio,"
      "perf_worst_slowdown_percent,perf_worst_case,"
      "latest_full_pressure_perf_hotspots,"
      "latest_full_pressure_perf_max_ratio,"
      "latest_full_pressure_perf_max_slowdown_percent,"
      "latest_full_pressure_perf_max_case,"
      "latest_full_pressure_perf_rows,"
      "latest_full_pressure_perf_suite_current,perf_watchlist_state} %s",
      path);
  return out ? out : strdup("");
}

static char *fuzz_all_state_compact_jq_command(const char *state_path) {
  const char *path = state_path && *state_path ? state_path :
      "build/fuzz/all/run-next-state.json";
  char *out = NULL;
  (void)asprintf(&out, "jq " NYNTH_FUZZ_ALL_STATE_JQ_EXPR " %s", path);
  return out ? out : strdup("");
}

static void fuzz_all_recommendation_make(
    fuzz_all_recommendation_t *out, bool ready, bool state_live,
    bool evidence_fresh, bool cache_policy_ok, bool ny_bin_exists,
    bool active_old_writer_present, double blocker_count, double active_items,
    double correctness_findings, double perf_hotspots, double runs_to_good,
    double runs_needed, bool target_reached, bool campaign_complete,
    const char *next_command, const char *state_command,
    const char *old_path_command, const char *advisory_action_command,
    const char *progress_command, const char *coverage_next_command,
    const char *coverage_next_guarded_command,
    const char *coverage_next_low_cpu_command,
    const char *coverage_next_preview_command) {
  if (!out) return;
  (void)active_old_writer_present;
  out->action = "inspect";
  out->reason = "no automatic handoff selected";
  out->command[0] = '\0';
  out->low_cpu_command[0] = '\0';
  out->preview_command[0] = '\0';
  out->repeat_mode[0] = '\0';
  out->repeat_count = 0.0;

  if (state_live) {
    out->action = "monitor-run";
    out->reason = "campaign run is already live";
    fuzz_all_recommendation_command(out, state_command);
  } else if (!cache_policy_ok || !ny_bin_exists) {
    out->action = "fix-environment";
    out->reason = "cache or Ny binary guard is not clean";
    fuzz_all_recommendation_command(out, old_path_command && *old_path_command ?
                                         old_path_command : progress_command);
  } else if (!ready || blocker_count > 0.0 || active_items > 0.0 ||
             correctness_findings > 0.0 || perf_hotspots > 0.0) {
    out->action = "triage-worklist";
    out->reason = "active blockers, findings, or perf hotspots need triage";
    fuzz_all_recommendation_command(
        out, advisory_action_command && *advisory_action_command ?
                 advisory_action_command : progress_command);
  } else if (!evidence_fresh) {
    out->action = "freshen-evidence";
    out->reason = "latest or full-pressure evidence is stale";
    fuzz_all_recommendation_prefixed(out,
                                     "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10",
                                     next_command);
    fuzz_all_recommendation_low_cpu(out, out->command);
    fuzz_all_recommendation_preview(out,
                                    "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                                    "NYNTH_RUN_DRY_RUN=1",
                                    next_command);
  } else if (!campaign_complete && coverage_next_command &&
             *coverage_next_command) {
    out->action = "run-missing-evidence";
    out->reason = "coverage backlog evidence is the nearest missing lane";
    if (coverage_next_guarded_command && *coverage_next_guarded_command)
      fuzz_all_recommendation_command(out, coverage_next_guarded_command);
    else if (strstr(coverage_next_command, "fuzz all run"))
      fuzz_all_recommendation_prefixed(
          out, "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10",
          coverage_next_command);
    else
      fuzz_all_recommendation_command(out, coverage_next_command);
    fuzz_all_recommendation_low_cpu(
        out, coverage_next_low_cpu_command && *coverage_next_low_cpu_command ?
                 coverage_next_low_cpu_command :
                 (strstr(out->command, "NYNTH_LOW_PRIORITY=1") ?
                      out->command : ""));
    snprintf(out->preview_command, sizeof(out->preview_command), "%s",
             coverage_next_preview_command &&
             *coverage_next_preview_command ? coverage_next_preview_command : "");
  } else if (!campaign_complete && runs_to_good > 0.0) {
    out->action = "run-good";
    out->reason = "good language score is the nearest campaign milestone";
    fuzz_all_recommendation_prefixed(
        out, "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 NYNTH_RUN_REPEAT=good",
        next_command);
    fuzz_all_recommendation_low_cpu(out, out->command);
    fuzz_all_recommendation_preview(
        out,
        "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
        "NYNTH_RUN_DRY_RUN=1 NYNTH_RUN_REPEAT=good",
        next_command);
    fuzz_all_recommendation_repeat(out, "good", runs_to_good);
  } else if (!campaign_complete && !target_reached && runs_needed > 0.0) {
    out->action = "run-target";
    out->reason = "10-thread-year campaign target is not reached";
    fuzz_all_recommendation_prefixed(
        out,
        "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 NYNTH_RUN_REPEAT=target",
        next_command);
    fuzz_all_recommendation_low_cpu(out, out->command);
    fuzz_all_recommendation_preview(
        out,
        "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
        "NYNTH_RUN_DRY_RUN=1 NYNTH_RUN_REPEAT=target",
        next_command);
    fuzz_all_recommendation_repeat(out, "target", runs_needed);
  } else if (campaign_complete) {
    out->action = "inspect-complete";
    out->reason = "campaign target is complete";
    fuzz_all_recommendation_command(out, progress_command);
  } else {
    fuzz_all_recommendation_command(out, progress_command);
  }
}

static bool fuzz_all_recommendation_run_preview_selected(
    const fuzz_all_recommendation_t *recommendation) {
  return recommendation && recommendation->preview_command[0] &&
         (strcmp(recommendation->action, "freshen-evidence") == 0 ||
          strcmp(recommendation->action, "run-good") == 0 ||
          strcmp(recommendation->action, "run-target") == 0);
}

static const char *fuzz_all_selected_run_preview_command(
    const char *preview_command,
    const fuzz_all_recommendation_t *recommendation) {
  return fuzz_all_recommendation_run_preview_selected(recommendation) ?
             recommendation->preview_command :
             (preview_command ? preview_command : "");
}

static bool fuzz_all_run_state_refresh_required(
    const fuzz_all_run_state_summary_t *st);

static const char *fuzz_all_selected_run_state_refresh_command(
    const char *state_refresh_command, const char *selected_preview_command,
    const fuzz_all_run_state_summary_t *run_state) {
  if (!run_state || !fuzz_all_run_state_refresh_required(run_state)) return "";
  return selected_preview_command && *selected_preview_command ?
             selected_preview_command :
             (state_refresh_command ? state_refresh_command : "");
}

static const char *fuzz_all_campaign_state(bool ready, bool target_reached,
                                           bool campaign_complete) {
  if (campaign_complete) return "complete";
  if (!ready) return "blocked";
  return target_reached ? "ready" : "ready-needs-evidence";
}

static const char *fuzz_all_campaign_incomplete_reason(bool ready,
                                                       double blocker_count,
                                                       double active_items,
                                                       bool target_reached,
                                                       bool campaign_complete) {
  if (campaign_complete) return "none";
  if (!ready) {
    if (blocker_count > 0.0) return "blockers";
    if (active_items > 0.0) return "active-worklist";
    return "not-ready";
  }
  if (!target_reached) return "target-not-reached";
  return "not-complete";
}

static double fuzz_all_language_good_gap_percent(double score) {
  return fuzz_all_score_clamp(fuzz_all_score_good_threshold() - score, 0.0,
                              100.0);
}

static void fuzz_all_score_from_status(const fuzz_all_status_summary_t *s,
                                       fuzz_all_score_summary_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->latest_report_age_seconds = -1.0;
  out->latest_full_pressure_report_age_seconds = -1.0;
  out->latest_report_stale_after_hours = fuzz_all_score_latest_fresh_hours();
  out->latest_full_pressure_report_stale_after_hours =
      fuzz_all_score_full_pressure_fresh_hours();
  out->label = "blocked";
  if (!s) return;

  bool ready = s->blocker_count == 0 && s->long_run_ready;
  out->latest_report_age_seconds =
      fuzz_all_score_report_age_seconds(s->nynth_root, s->latest_report);
  out->latest_full_pressure_report_age_seconds =
      fuzz_all_score_report_age_seconds(s->nynth_root,
                                        s->latest_full_pressure_report);
  out->latest_report_fresh =
      fuzz_all_score_age_fresh(out->latest_report_age_seconds,
                               out->latest_report_stale_after_hours);
  out->latest_full_pressure_report_fresh =
      fuzz_all_score_age_fresh(out->latest_full_pressure_report_age_seconds,
                               out->latest_full_pressure_report_stale_after_hours);
  out->evidence_fresh =
      out->latest_report_fresh && out->latest_full_pressure_report_fresh;
  if (!out->latest_report_fresh) out->freshness_penalty += 8.0;
  if (!out->latest_full_pressure_report_fresh) out->freshness_penalty += 12.0;

  double effective_advisory_timeouts = fuzz_all_effective_advisory_timeouts(
      s->non_reproducing_afl_timeouts,
      s->advisory_recheck_raw_repro_checked,
      s->advisory_recheck_raw_repro_passed,
      s->advisory_recheck_raw_repro_timeouts,
      s->advisory_recheck_raw_repro_unexpected,
      s->advisory_recheck_command);
  out->signal_score =
      fuzz_all_score_signal_score(ready, s->blocker_count, s->active_items,
                                  s->coverage_blocker_gaps,
                                  s->latest_finding_live,
                                  s->latest_finding_missing,
                                  s->latest_known_reproduced,
                                  s->latest_known_lost,
                                  s->latest_known_baseline,
                                  s->latest_perf_hotspots,
                                  effective_advisory_timeouts,
                                  s->cache_policy_ok, s->ny_bin_exists);
  fuzz_all_score_signal_breakdown(ready, s->blocker_count, s->active_items,
                                  s->coverage_blocker_gaps,
                                  s->latest_finding_live,
                                  s->latest_finding_missing,
                                  s->latest_known_reproduced,
                                  s->latest_known_lost,
                                  s->latest_known_baseline,
                                  s->latest_perf_hotspots,
                                  effective_advisory_timeouts,
                                  s->cache_policy_ok, s->ny_bin_exists,
                                  &out->gate_penalty,
                                  &out->correctness_penalty,
                                  &out->perf_penalty,
                                  &out->advisory_penalty,
                                  &out->environment_penalty);
  out->evidence_cap =
      fuzz_all_score_evidence_cap(s->target_percent, s->campaign_complete);
  double capped =
      out->signal_score < out->evidence_cap ? out->signal_score : out->evidence_cap;
  out->stability_score =
      fuzz_all_score_clamp(capped - out->freshness_penalty, 0.0, 100.0);
  out->label = fuzz_all_score_label(out->stability_score);
  fuzz_all_score_note(out->note, sizeof(out->note), ready, s->blocker_count,
                      s->active_items, out->freshness_penalty,
                      out->advisory_penalty, out->signal_score,
                      out->evidence_cap, s->target_percent);
}

static double fuzz_all_thread_years_per_run(double remaining_thread_years,
                                            double runs_needed) {
  if (remaining_thread_years <= 0.0 || runs_needed <= 0.0) return 0.0;
  return remaining_thread_years / runs_needed;
}

static double fuzz_all_target_percent_per_run(double thread_years_per_run,
                                              double target_thread_years) {
  if (thread_years_per_run <= 0.0 || target_thread_years <= 0.0) return 0.0;
  return fuzz_all_score_clamp((thread_years_per_run / target_thread_years) *
                              100.0,
                              0.0, 100.0);
}

static double fuzz_all_runs_to_good(double target_percent,
                                    double target_percent_per_run,
                                    double signal_score,
                                    double stability_score,
                                    bool campaign_complete) {
  const double good_score = fuzz_all_score_good_threshold();
  if (stability_score >= good_score - 0.005) return 0.0;
  if (campaign_complete || signal_score < good_score ||
      target_percent_per_run <= 0.0)
    return -1.0;
  double needed_target_percent = (good_score - 55.0) / 1.5;
  if (target_percent >= needed_target_percent) return 0.0;
  double raw_runs =
      (needed_target_percent - target_percent) / target_percent_per_run;
  long runs = (long)raw_runs;
  if ((double)runs + 0.000000001 < raw_runs) ++runs;
  if (runs < 1) runs = 1;
  return (double)runs;
}

static double fuzz_all_runs_to_days(double runs, double runs_per_day) {
  if (runs < 0.0 || runs_per_day <= 0.0) return -1.0;
  return runs / runs_per_day;
}

static double fuzz_all_campaign_calendar_percent_10y(double age_days) {
  const double ten_year_days = 365.25 * 10.0;
  if (age_days <= 0.0 || ten_year_days <= 0.0) return 0.0;
  return (age_days / ten_year_days) * 100.0;
}

static void append_fuzz_all_campaign_alias_fields(
    str_buf_t *b, double thread_years, double target_thread_years,
    double remaining_thread_years, double campaign_percent, double runs_needed,
    double wall_hours_needed, double wall_days_needed,
    double thread_years_per_run, double percent_per_run,
    double runs_per_day, double thread_years_per_day,
    double campaign_plan_wall_hours, const char *campaign_plan_threads,
    const char *completion_eta_local) {
  if (!b) return;
  double percent_per_day = target_thread_years > 0.0 ?
      (thread_years_per_day / target_thread_years) * 100.0 : 0.0;
  double equivalent_wall_days = thread_years_per_day > 0.0 ?
      thread_years / thread_years_per_day : 0.0;
  (void)sb_appendf(b,
                   ",\"campaign_thread_years\":%.8f,"
                   "\"campaign_target_thread_years\":%.8f,"
                   "\"campaign_remaining_thread_years\":%.8f,"
                   "\"campaign_done_percent\":%.4f,"
                   "\"campaign_runs_needed\":%.0f,"
                   "\"campaign_wall_hours_needed\":%.4f,"
                   "\"campaign_wall_days_needed\":%.4f,"
                   "\"campaign_thread_years_per_run\":%.8f,"
                   "\"campaign_percent_per_run\":%.4f,"
                      "\"campaign_runs_per_wall_day\":%.4f,"
                      "\"campaign_thread_years_per_wall_day\":%.8f,"
                      "\"campaign_percent_per_wall_day\":%.4f,"
                      "\"campaign_equivalent_wall_days\":%.4f,"
                      "\"campaign_plan_wall_hours\":%.4f",
                      thread_years, target_thread_years,
                      remaining_thread_years, campaign_percent, runs_needed,
                      wall_hours_needed, wall_days_needed, thread_years_per_run,
                      percent_per_run, runs_per_day, thread_years_per_day,
                      percent_per_day, equivalent_wall_days,
                      campaign_plan_wall_hours);
  (void)sb_append(b, ",\"campaign_plan_threads\":");
  (void)sb_append_json_str(b, campaign_plan_threads ?
                                  campaign_plan_threads : "");
  (void)sb_append(b, ",\"campaign_eta_local\":");
  (void)sb_append_json_str(b, completion_eta_local ? completion_eta_local : "");
}

static void append_fuzz_all_recommended_state_fields(
    str_buf_t *b, const char *recommended_action,
    const fuzz_all_run_state_summary_t *run_state,
    const fuzz_all_run_state_summary_t *coverage_next_run_state,
    const char *state_file, const char *state_command,
    const char *state_refresh_command,
    const char *recommended_preview_command,
    const char *coverage_next_state_file,
    const char *coverage_next_state_command,
    const char *coverage_next_state_refresh_command,
    bool coverage_next_state_refresh_required,
    const char *coverage_next_state_refresh_reason);

static void fuzz_all_projection_from_status(const fuzz_all_status_summary_t *s,
                                            const fuzz_all_score_summary_t *score,
                                            fuzz_all_projection_summary_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->thread_years_per_run_source = "remaining-even";
  out->runs_to_good_stability = -1.0;
  out->runs_to_good_days = -1.0;
  if (!s || !score) return;

  if (s->thread_years_per_day > 0.0 && s->runs_per_day > 0.0) {
    out->thread_years_per_run = s->thread_years_per_day / s->runs_per_day;
    out->thread_years_per_run_source = "plan-rate";
  } else {
    out->thread_years_per_run =
        fuzz_all_thread_years_per_run(s->remaining_thread_years,
                                      s->runs_needed);
  }
  out->target_percent_per_run =
      fuzz_all_target_percent_per_run(out->thread_years_per_run,
                                      s->target_thread_years);
  out->next_run_target_percent =
      fuzz_all_score_clamp(s->target_percent + out->target_percent_per_run,
                           0.0, 100.0);
  bool next_run_complete =
      s->campaign_complete || out->next_run_target_percent >= 100.0;
  double next_run_evidence_cap =
      fuzz_all_score_evidence_cap(out->next_run_target_percent,
                                  next_run_complete);
  out->next_run_stability_score =
      score->signal_score < next_run_evidence_cap ?
          score->signal_score : next_run_evidence_cap;
  out->next_run_stability_delta =
      out->next_run_stability_score - score->stability_score;
  out->runs_to_good_stability =
      fuzz_all_runs_to_good(s->target_percent, out->target_percent_per_run,
                            score->signal_score, score->stability_score,
                            s->campaign_complete);
  if (score->freshness_penalty > 0.0) out->runs_to_good_stability = -1.0;
  out->runs_to_good_days =
      fuzz_all_runs_to_days(out->runs_to_good_stability, s->runs_per_day);
}

static char *make_fuzz_all_status_summary_row(
    const fuzz_all_status_summary_t *s,
    const fuzz_all_run_state_summary_t *run_state,
    const fuzz_all_run_state_summary_t *coverage_next_run_state,
    const char *coverage_queue_json) {
  fuzz_all_score_summary_t score;
  fuzz_all_score_from_status(s, &score);
  fuzz_all_projection_summary_t projection;
  fuzz_all_projection_from_status(s, &score, &projection);
  bool campaign_ready = s && s->blocker_count == 0 && s->long_run_ready;
  double compiler_findings =
      s ? s->latest_finding_live + s->latest_finding_missing : 0.0;
  double known_bug_findings =
      s ? s->latest_known_reproduced + s->latest_known_lost +
              s->latest_known_baseline : 0.0;
  double correctness_findings = compiler_findings + known_bug_findings;
  double perf_hotspots_open = s ? s->latest_perf_hotspots : 0.0;
  double perf_worst_ratio = s ? s->latest_perf_max_ratio : 0.0;
  const char *perf_worst_case = s ? s->latest_perf_max_case : "";
  if (s && s->latest_full_pressure_perf_suite_current &&
      s->latest_full_pressure_perf_max_ratio > perf_worst_ratio) {
    perf_worst_ratio = s->latest_full_pressure_perf_max_ratio;
    perf_worst_case = s->latest_full_pressure_perf_max_case;
  }
  perf_worst_case = fuzz_all_perf_effective_worst_case(
      perf_worst_case, s && s->perf_watchlist_artifact_readable,
      s && s->perf_watchlist_artifact_fresh,
      s ? s->perf_watchlist_artifact_max_ratio : 0.0,
      s ? s->perf_watchlist_artifact_max_case : "");
  perf_worst_ratio = fuzz_all_perf_effective_worst_ratio(
      perf_worst_ratio, s && s->perf_watchlist_artifact_readable,
      s && s->perf_watchlist_artifact_fresh,
      s ? s->perf_watchlist_artifact_max_ratio : 0.0);
  double perf_watchlist_open =
      fuzz_all_perf_watchlist_effective_open(
          perf_hotspots_open, perf_worst_ratio,
          s && s->perf_watchlist_artifact_readable,
          s && s->perf_watchlist_artifact_fresh,
          s ? s->perf_watchlist_artifact_hotspots : 0.0);
  const char *perf_watchlist_case =
      perf_watchlist_open > 0.0 ? perf_worst_case : "";
  const char *perf_watchlist_state =
      fuzz_all_perf_watchlist_state(
          perf_watchlist_open, s && s->perf_watchlist_artifact_readable,
          s && s->perf_watchlist_artifact_fresh,
          s ? s->perf_watchlist_artifact_hotspots : 0.0);
  const char *perf_watchlist_action =
      fuzz_all_perf_watchlist_action(perf_watchlist_state);
  char *perf_watchlist_action_command =
      fuzz_all_perf_watchlist_action_command(
          perf_watchlist_state, s ? s->perf_watchlist_command : "",
          s ? s->perf_watchlist_markdown : "",
          s ? s->perf_watchlist_report : "");
  const char *optimization_reason =
      fuzz_all_optimization_reason(perf_watchlist_state);
  const char *optimization_case =
      fuzz_all_optimization_case(
          s && s->perf_watchlist_artifact_readable,
          s && s->perf_watchlist_artifact_fresh,
          s ? s->perf_watchlist_artifact_hotspots : 0.0,
          s ? s->perf_watchlist_artifact_max_case : "",
          s ? s->perf_watchlist_artifact_max_ratio : 0.0,
          perf_watchlist_open, perf_watchlist_case);
  double optimization_ratio =
      fuzz_all_optimization_ratio(
          s && s->perf_watchlist_artifact_readable,
          s && s->perf_watchlist_artifact_fresh,
          s ? s->perf_watchlist_artifact_hotspots : 0.0,
          s ? s->perf_watchlist_artifact_max_ratio : 0.0,
          perf_watchlist_open, perf_worst_ratio);
  const char *optimization_artifact =
      fuzz_all_optimization_artifact_path(
          s && s->perf_watchlist_artifact_readable,
          s && s->perf_watchlist_artifact_fresh,
          s ? s->perf_watchlist_artifact_hotspots : 0.0,
          s ? s->perf_watchlist_artifact_max_ratio : 0.0,
          s ? s->perf_watchlist_artifact_max_artifact : "");
  const char *optimization_ny_source =
      fuzz_all_optimization_artifact_path(
          s && s->perf_watchlist_artifact_readable,
          s && s->perf_watchlist_artifact_fresh,
          s ? s->perf_watchlist_artifact_hotspots : 0.0,
          s ? s->perf_watchlist_artifact_max_ratio : 0.0,
          s ? s->perf_watchlist_artifact_max_ny_source : "");
  const char *optimization_c_source =
      fuzz_all_optimization_artifact_path(
          s && s->perf_watchlist_artifact_readable,
          s && s->perf_watchlist_artifact_fresh,
          s ? s->perf_watchlist_artifact_hotspots : 0.0,
          s ? s->perf_watchlist_artifact_max_ratio : 0.0,
          s ? s->perf_watchlist_artifact_max_c_source : "");
  char *optimization_target_command =
      fuzz_all_optimization_target_command(optimization_ny_source,
                                           optimization_c_source,
                                           optimization_artifact);
  fuzz_all_recommendation_t recommendation;
  fuzz_all_recommendation_make(
      &recommendation, campaign_ready,
      s && fuzz_all_state_phase_live(s->state_phase), score.evidence_fresh,
      s && s->cache_policy_ok, s && s->ny_bin_exists,
      s && s->active_old_nytrix_output_writer_present,
      s ? s->blocker_count : 0.0, s ? s->active_items : 0.0,
      correctness_findings, perf_hotspots_open,
      projection.runs_to_good_stability, s ? s->runs_needed : 0.0,
        s && s->target_reached, s && s->campaign_complete,
        s && s->next_handoff_command[0] ? s->next_handoff_command :
            (s ? s->next_command : ""),
        s ? s->state_command : "",
        s ? s->old_path_command : "", s ? s->advisory_action_command : "",
        s ? s->progress_command : "",
        s ? s->coverage_next_command : "",
        s ? s->coverage_next_guarded_command : "",
        s ? s->coverage_next_low_cpu_command : "",
        s ? s->coverage_next_preview_command : "");
  const char *selected_preview_command =
      fuzz_all_selected_run_preview_command(s ? s->preview_command : "",
                                            &recommendation);
  const char *selected_preview_for_refresh =
      fuzz_all_recommendation_run_preview_selected(&recommendation) ?
          recommendation.preview_command : "";
  const char *selected_state_refresh_command =
      fuzz_all_selected_run_state_refresh_command(
          s ? s->state_refresh_command : "", selected_preview_for_refresh,
          run_state);
  char run_next_gentle_command[4096] = {0};
  char run_next_gentle_preview_command[4096] = {0};
  if (s)
    fuzz_all_gentle_run_command(
        run_next_gentle_command, sizeof(run_next_gentle_command),
        s->next_handoff_command[0] ? s->next_handoff_command :
                                     s->next_command);
  if (s)
    fuzz_all_gentle_preview_command(
        run_next_gentle_preview_command,
        sizeof(run_next_gentle_preview_command),
        s->next_handoff_command[0] ? s->next_handoff_command :
                                     s->next_command);
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"kind\":\"fuzz-all-status\"");
  (void)sb_append(&row, ",\"history_report\":");
  (void)sb_append_json_str(&row, s ? s->history_report : "");
  (void)sb_append(&row, ",\"worklist_report\":");
  (void)sb_append_json_str(&row, s ? s->worklist_report : "");
  (void)sb_append(&row, ",\"historical_worklist_report\":");
  (void)sb_append_json_str(&row, s ? s->historical_worklist_report : "");
  (void)sb_append(&row, ",\"historical_worklist_markdown\":");
  (void)sb_append_json_str(&row, s ? s->historical_worklist_markdown : "");
  (void)sb_append(&row, ",\"coverage_report\":");
  (void)sb_append_json_str(&row, s ? s->coverage_report : "");
  (void)sb_append(&row, ",\"plan_report\":");
  (void)sb_append_json_str(&row, s ? s->plan_report : "");
  append_fuzz_all_compiler_std_audit_fields(&row, s);
  (void)sb_append(&row, ",\"latest_report\":");
  (void)sb_append_json_str(&row, s ? s->latest_report : "");
  (void)sb_append(&row, ",\"latest_full_pressure_report\":");
  (void)sb_append_json_str(&row, s ? s->latest_full_pressure_report : "");
  (void)sb_append(&row, ",\"next_script\":");
  (void)sb_append_json_str(&row, s ? s->next_script : "");
  (void)sb_append(&row, ",\"next_handoff_command\":");
  (void)sb_append_json_str(&row, s ? s->next_handoff_command : "");
  (void)sb_append(&row, ",\"next_command\":");
  (void)sb_append_json_str(&row, s ? s->next_command : "");
  (void)sb_append(&row, ",\"preview_command\":");
  (void)sb_append_json_str(&row, selected_preview_command);
  (void)sb_append(&row, ",\"run_next_command\":");
  (void)sb_append_json_str(&row, s ? s->next_command : "");
  (void)sb_append(&row, ",\"run_next_preview_command\":");
  (void)sb_append_json_str(&row, selected_preview_command);
  (void)sb_append(&row, ",\"run_next_low_cpu_command\":");
  (void)sb_append_json_str(&row, s ? s->next_command : "");
  (void)sb_append(&row, ",\"run_next_gentle_command\":");
  (void)sb_append_json_str(&row, run_next_gentle_command);
  (void)sb_append(&row, ",\"run_next_gentle_preview_command\":");
  (void)sb_append_json_str(&row, run_next_gentle_preview_command);
  (void)sb_append(&row, ",\"stop_file\":");
  (void)sb_append_json_str(&row, s ? s->stop_file : "");
  (void)sb_append(&row, ",\"stop_command\":");
  (void)sb_append_json_str(&row, s ? s->stop_command : "");
  (void)sb_append(&row, ",\"resume_command\":");
  (void)sb_append_json_str(&row, s ? s->resume_command : "");
  (void)sb_append(&row, ",\"state_file\":");
  (void)sb_append_json_str(&row, s ? s->state_file : "");
  (void)sb_append(&row, ",\"state_command\":");
  (void)sb_append_json_str(&row, s ? s->state_command : "");
  (void)sb_append(&row, ",\"state_refresh_command\":");
  (void)sb_append_json_str(&row, selected_state_refresh_command);
  (void)sb_append(&row, ",\"state\":");
  (void)sb_append_json_str(&row,
      s ? fuzz_all_state_label_values(s->state_readable,
                                      s->state_phase) : "missing");
  (void)sb_append(&row, ",\"state_readable\":");
  (void)sb_append(&row, s && s->state_readable ? "true" : "false");
  (void)sb_append(&row, ",\"state_fresh\":");
  (void)sb_append(&row, s && s->state_fresh ? "true" : "false");
  (void)sb_append(&row, ",\"state_live\":");
  (void)sb_append(&row,
                  s && fuzz_all_state_phase_live(s->state_phase) ? "true"
                                                                 : "false");
  (void)sb_append(&row, ",\"state_child_alive\":");
  (void)sb_append(&row, s && s->state_child_alive ? "true" : "false");
  (void)sb_append(&row, ",\"state_child_status\":");
  (void)sb_append_json_str(&row,
      s ? fuzz_all_state_child_status(s->state_child_pid,
                                      s->state_child_alive) : "none");
  (void)sb_append(&row, ",\"state_stale_reason\":");
  (void)sb_append_json_str(&row,
      s ? fuzz_all_state_stale_reason_values(s->state_readable,
                                             s->state_fresh,
                                             s->state_phase,
                                             s->state_age_seconds,
                                             s->state_heartbeat_s,
                                             s->state_child_pid,
                                             s->state_child_alive) : "missing");
  (void)sb_appendf(&row,
                   ",\"state_age_seconds\":%.0f,"
                   "\"state_stale_after_seconds\":%.0f,"
                   "\"state_cycle\":%.0f,"
                   "\"state_cycles\":%.0f,"
                   "\"state_heartbeat_s\":%.0f,"
                   "\"state_heartbeat_count\":%.0f,"
                   "\"state_child_pid\":%.0f",
                   s ? s->state_age_seconds : -1.0,
                   s ? fuzz_all_state_stale_after_seconds(s->state_heartbeat_s)
                     : 3600.0,
                   s ? s->state_cycle : 0.0,
                   s ? s->state_cycles : 0.0,
                   s ? s->state_heartbeat_s : 0.0,
                   s ? s->state_heartbeat_count : 0.0,
                   s ? s->state_child_pid : 0.0);
  (void)sb_append(&row, ",\"state_phase\":");
  (void)sb_append_json_str(&row, s ? s->state_phase : "");
  (void)sb_append(&row, ",\"state_event\":");
  (void)sb_append_json_str(&row, s ? s->state_event : "");
  (void)sb_append(&row, ",\"state_timestamp_utc\":");
  (void)sb_append_json_str(&row, s ? s->state_timestamp_utc : "");
  (void)sb_append(&row, ",\"state_last_report\":");
  (void)sb_append_json_str(&row, s ? s->state_last_report : "");
  (void)sb_append(&row, ",\"progress_command\":");
  (void)sb_append_json_str(&row, s ? s->progress_command : "");
  char status_freshness_action_command[4096] = {0};
  if (s && score.freshness_penalty > 0.0)
    fuzz_all_freshness_action_command(status_freshness_action_command,
                                      sizeof(status_freshness_action_command),
                                      s->next_handoff_command[0] ?
                                          s->next_handoff_command :
                                          s->next_command);
  (void)sb_append(&row, ",\"freshness_action_command\":");
  (void)sb_append_json_str(&row, status_freshness_action_command);
  (void)sb_append(&row, ",\"latest_report_freshness_command\":");
  (void)sb_append_json_str(&row,
                           s && !score.latest_report_fresh ?
                               status_freshness_action_command : "");
  (void)sb_append(&row, ",\"latest_full_pressure_report_freshness_command\":");
  (void)sb_append_json_str(&row,
                           s && !score.latest_full_pressure_report_fresh ?
                               status_freshness_action_command : "");
  (void)sb_append(&row, ",\"full_pressure_freshen_command\":");
  (void)sb_append_json_str(&row,
                           s && !score.latest_full_pressure_report_fresh ?
                               status_freshness_action_command : "");
  (void)sb_append(&row, ",\"full_pressure_remediation_command\":");
  (void)sb_append_json_str(&row,
                           s && !score.latest_full_pressure_report_fresh ?
                               status_freshness_action_command : "");
  (void)sb_append(&row, ",\"full_pressure_action_command\":");
  (void)sb_append_json_str(&row,
                           s && !score.latest_full_pressure_report_fresh ?
                               status_freshness_action_command : "");
  (void)sb_append(&row, ",\"status_command\":");
  (void)sb_append_json_str(&row, s ? s->status_command : "");
  (void)sb_append(&row, ",\"old_path_probe_command\":");
  (void)sb_append_json_str(&row, NYNTH_FUZZ_ALL_OLD_PATH_PROBE_COMMAND);
  (void)sb_append(&row, ",\"old_path_command\":");
  (void)sb_append_json_str(&row, s ? s->old_path_command : "");
  (void)sb_append(&row, ",\"old_path_dry_run_command\":");
  (void)sb_append_json_str(&row, s ? s->old_path_dry_run_command : "");
  (void)sb_append(&row, ",\"old_path_apply_command\":");
  (void)sb_append_json_str(&row, s ? s->old_path_apply_command : "");
  (void)sb_append(&row, ",\"old_path_next_action\":");
  (void)sb_append_json_str(&row, s ? s->old_path_next_action : "none");
  (void)sb_append(&row, ",\"old_path_next_reason\":");
  (void)sb_append_json_str(&row, s ? s->old_path_next_reason : "");
  (void)sb_append(&row, ",\"old_path_report\":");
  (void)sb_append_json_str(&row, s ? s->old_path_report : "");
  (void)sb_append(&row, ",\"old_path_markdown\":");
  (void)sb_append_json_str(&row, s ? s->old_path_markdown : "");
  (void)sb_appendf(&row,
                   ",\"old_path_cache_policy_ok\":%s,"
                   "\"old_path_present_count\":%.0f,"
                   "\"old_path_moved_count\":%.0f,"
                   "\"old_path_remaining_count\":%.0f,"
                   "\"old_path_wait_remaining_seconds\":%.0f,"
                   "\"old_path_artifact_leak_count\":%.0f,"
                   "\"old_path_artifact_moved_count\":%.0f,"
                   "\"old_path_artifact_remaining_count\":%.0f",
                   s && s->old_path_cache_policy_ok ? "true" : "false",
                   s ? s->old_path_present_count : 0.0,
                   s ? s->old_path_moved_count : 0.0,
                   s ? s->old_path_remaining_count : 0.0,
                   s ? s->old_path_wait_remaining_seconds : -1.0,
                   s ? s->old_path_artifact_leak_count : 0.0,
                   s ? s->old_path_artifact_moved_count : 0.0,
                   s ? s->old_path_artifact_remaining_count : 0.0);
  (void)sb_append(&row, ",\"advisory_action_command\":");
  (void)sb_append_json_str(&row,
                           s && (s->non_reproducing_afl_timeouts > 0.0 ||
                                 s->historical_non_reproducing_afl_timeouts > 0.0) ?
                               s->advisory_action_command : "");
  (void)sb_append(&row, ",\"advisory_recheck_command\":");
  (void)sb_append_json_str(&row,
                           s && (s->non_reproducing_afl_timeouts > 0.0 ||
                                 s->historical_non_reproducing_afl_timeouts > 0.0) ?
                               s->advisory_recheck_command : "");
  bool advisory_present =
      s && (s->non_reproducing_afl_timeouts > 0.0 ||
            s->historical_non_reproducing_afl_timeouts > 0.0);
  (void)sb_append(&row, ",\"advisory_recheck_state\":");
  (void)sb_append_json_str(
      &row,
      fuzz_all_advisory_recheck_state(
          advisory_present,
          s ? s->advisory_recheck_raw_repro_checked : 0.0,
          s ? s->advisory_recheck_raw_repro_passed : 0.0,
          s ? s->advisory_recheck_raw_repro_timeouts : 0.0,
          s ? s->advisory_recheck_raw_repro_unexpected : 0.0,
          s ? s->advisory_recheck_command : ""));
  (void)sb_appendf(
      &row,
      ",\"advisory_recheck_raw_repro_checked\":%.0f,"
      "\"advisory_recheck_raw_repro_passed\":%.0f,"
      "\"advisory_recheck_raw_repro_timeouts\":%.0f,"
      "\"advisory_recheck_raw_repro_unexpected\":%.0f",
      advisory_present && s ? s->advisory_recheck_raw_repro_checked : 0.0,
      advisory_present && s ? s->advisory_recheck_raw_repro_passed : 0.0,
      advisory_present && s ? s->advisory_recheck_raw_repro_timeouts : 0.0,
      advisory_present && s ? s->advisory_recheck_raw_repro_unexpected : 0.0);
  (void)sb_append(&row, ",\"perf_watchlist_command\":");
  (void)sb_append_json_str(&row, s ? s->perf_watchlist_command : "");
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
  (void)sb_append_json_str(&row, s ? s->perf_watchlist_report : "");
  (void)sb_append(&row, ",\"perf_watchlist_markdown\":");
  (void)sb_append_json_str(&row, s ? s->perf_watchlist_markdown : "");
  (void)sb_append(&row, ",\"perf_watchlist_artifact_readable\":");
  (void)sb_append(&row,
                  s && s->perf_watchlist_artifact_readable ? "true" : "false");
  (void)sb_append(&row, ",\"perf_watchlist_artifact_fresh\":");
  (void)sb_append(&row,
                  s && s->perf_watchlist_artifact_fresh ? "true" : "false");
  (void)sb_appendf(&row,
                   ",\"perf_watchlist_artifact_hotspots\":%.0f,"
                   "\"perf_watchlist_artifact_max_ratio\":%.4f,"
                   "\"perf_watchlist_artifact_max_slowdown_percent\":%.2f,"
                   "\"perf_watchlist_artifact_age_seconds\":%.0f,"
                   "\"perf_watchlist_artifact_stale_after_hours\":%.2f",
                   s ? s->perf_watchlist_artifact_hotspots : 0.0,
                   s ? s->perf_watchlist_artifact_max_ratio : 0.0,
                   s ? fuzz_all_perf_slowdown_percent(
                           s->perf_watchlist_artifact_max_ratio) : 0.0,
                   s ? s->perf_watchlist_artifact_age_seconds : -1.0,
                   s ? s->perf_watchlist_artifact_stale_after_hours :
                       fuzz_all_perf_watchlist_artifact_fresh_hours());
  (void)sb_append(&row, ",\"perf_watchlist_artifact_max_case\":");
  (void)sb_append_json_str(&row,
                           s ? s->perf_watchlist_artifact_max_case : "");
  (void)sb_append(&row, ",\"perf_watchlist_artifact_max_artifact\":");
  (void)sb_append_json_str(&row,
                           s ? s->perf_watchlist_artifact_max_artifact : "");
  (void)sb_append(&row, ",\"perf_watchlist_artifact_max_ny_source\":");
  (void)sb_append_json_str(
      &row, s ? s->perf_watchlist_artifact_max_ny_source : "");
  (void)sb_append(&row, ",\"perf_watchlist_artifact_max_c_source\":");
  (void)sb_append_json_str(
      &row, s ? s->perf_watchlist_artifact_max_c_source : "");
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
  append_fuzz_all_coverage_next_fields(&row, s);
  if (coverage_next_run_state)
    append_fuzz_all_run_state_fields_prefixed(&row, "coverage_next_state",
                                              coverage_next_run_state);
  append_fuzz_all_recommended_state_fields(
      &row, recommendation.action, run_state, coverage_next_run_state,
      s ? s->state_file : "", s ? s->state_command : "",
      s ? s->state_refresh_command : "",
      recommendation.preview_command,
      s ? s->coverage_next_state_file : "",
      s ? s->coverage_next_state_command : "",
      s ? s->coverage_next_state_refresh_command : "",
      s && s->coverage_next_state_refresh_required,
      s ? s->coverage_next_state_refresh_reason : "");
  append_fuzz_all_handoff_guard_fields(&row);
  (void)sb_append(&row, ",\"active_primary_command\":");
  (void)sb_append_json_str(&row, s ? s->active_primary_command : "");
  (void)sb_append(&row, ",\"active_raw_repro_command\":");
  (void)sb_append_json_str(&row, s ? s->active_raw_repro_command : "");
  (void)sb_append(&row, ",\"nynth_root\":");
  append_repo_or_sibling_rel_json_str(&row, s ? s->nynth_root : "",
                                      s ? s->nynth_root : "");
  (void)sb_append(&row, ",\"nytrix_root\":");
  append_repo_or_sibling_rel_json_str(&row, s ? s->nynth_root : "",
                                      s ? s->nytrix_root : "");
  (void)sb_append(&row, ",\"ny_bin\":");
  append_repo_or_sibling_rel_json_str(&row, s ? s->nynth_root : "",
                                      s ? s->ny_bin : "");
  (void)sb_append(&row, ",\"tmp_dir\":");
  append_repo_or_sibling_rel_json_str(&row, s ? s->nynth_root : "",
                                      s ? s->tmp_dir : "");
  (void)sb_append(&row, ",\"scratch_root\":");
  append_repo_or_sibling_rel_json_str(&row, s ? s->nynth_root : "",
                                      s ? s->scratch_root : "");
  (void)sb_append(&row, ",\"xdg_cache_home\":");
  append_repo_or_sibling_rel_json_str(&row, s ? s->nynth_root : "",
                                      s ? s->xdg_cache_home : "");
  (void)sb_append(&row, ",\"nytrix_cache_dir\":");
  append_repo_or_sibling_rel_json_str(&row, s ? s->nynth_root : "",
                                      s ? s->nytrix_cache_dir : "");
  (void)sb_append(&row, ",\"old_nytrix_test_scratch\":");
  (void)sb_append_json_str(&row, "old-sibling-test-scratch");
  (void)sb_append(&row, ",\"old_nytrix_fuzz_dir\":");
  (void)sb_append_json_str(&row, "old-sibling-fuzz-dir");
  (void)sb_append(&row, ",\"old_nytrix_build_cache_dir\":");
  (void)sb_append_json_str(&row, "old-sibling-build-cache");
  (void)sb_append(&row, ",\"old_nytrix_build_cache_absent\":");
  (void)sb_append(&row, s && s->old_nytrix_build_cache_absent ? "true" : "false");
  (void)sb_append(&row, ",\"active_old_nytrix_cache_writer_present\":");
  (void)sb_append(&row, s && s->active_old_nytrix_cache_writer_present ? "true" : "false");
  (void)sb_append(&row, ",\"active_old_nytrix_cache_writer\":");
  (void)sb_append_json_str(&row, s ? s->active_old_nytrix_cache_writer : "");
  (void)sb_append(&row, ",\"active_old_nytrix_output_writer_present\":");
  (void)sb_append(&row, s && s->active_old_nytrix_output_writer_present ? "true" : "false");
  (void)sb_append(&row, ",\"active_old_nytrix_output_writer\":");
  (void)sb_append_json_str(&row, s ? s->active_old_nytrix_output_writer : "");
  (void)sb_append(&row, ",\"old_path_wait_remaining_seconds\":");
  (void)sb_appendf(&row, "%.0f",
                   s ? s->old_path_wait_remaining_seconds : -1.0);
  (void)sb_append(&row, ",\"nynth_git_head\":");
  (void)sb_append_json_str(&row, s ? s->nynth_git_head : "");
  (void)sb_append(&row, ",\"nytrix_git_head\":");
  (void)sb_append_json_str(&row, s ? s->nytrix_git_head : "");
  (void)sb_append(&row, ",\"nynth_git_status_hash\":");
  (void)sb_append_json_str(&row, s ? s->nynth_git_status_hash : "");
  (void)sb_append(&row, ",\"nytrix_git_status_hash\":");
  (void)sb_append_json_str(&row, s ? s->nytrix_git_status_hash : "");
  (void)sb_append(&row, ",\"nynth_bin_hash\":");
  (void)sb_append_json_str(&row, s ? s->nynth_bin_hash : "");
  (void)sb_append(&row, ",\"ny_bin_hash\":");
  (void)sb_append_json_str(&row, s ? s->ny_bin_hash : "");
  (void)sb_appendf(&row,
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
                   "\"nynth_git_ok\":%s,\"nynth_git_dirty\":%s,"
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
                   s && s->history_readable ? "true" : "false",
                   s && s->worklist_readable ? "true" : "false",
                   s && s->coverage_readable ? "true" : "false",
                   s && s->plan_readable ? "true" : "false",
                   s ? s->reports : 0.0,
                   s ? s->ignored_no_evidence_reports : 0.0,
                   s ? s->ignored_no_evidence_reports : 0.0,
                   s ? s->ok_reports : 0.0,
                   s ? s->failed_reports : 0.0, s ? s->attention_reports : 0.0,
                   s ? s->full_pressure_reports : 0.0,
                   s ? s->full_pressure_ok_reports : 0.0,
                   s ? s->full_pressure_attention_reports : 0.0,
                      s ? s->thread_hours : 0.0, s ? s->thread_years : 0.0,
                      s ? s->campaign_first_report_epoch : 0.0,
                      s ? s->campaign_latest_report_epoch : 0.0,
                      s ? s->campaign_calendar_span_days : 0.0,
                      s ? s->campaign_calendar_age_days : 0.0,
                      fuzz_all_campaign_calendar_percent_10y(
                          s ? s->campaign_calendar_age_days : 0.0),
                      s ? s->full_pressure_thread_hours : 0.0,
                   s ? s->full_pressure_thread_years : 0.0,
                   s ? s->checked_subcases : 0.0,
                   s ? s->sub_failures_total : 0.0,
                   s ? s->active_items : 0.0,
                   s ? s->active_items : 0.0,
                   s ? s->active_failure_detail_count : 0.0,
                   s ? s->active_saved_hangs : 0.0,
                   s ? s->active_saved_crashes : 0.0,
                   s ? s->active_saved_inputs : 0.0,
                   s ? s->active_repro_commands : 0.0,
                   s ? s->active_raw_repro_commands : 0.0,
                   s ? s->active_repro_ready : 0.0,
                   s ? s->historical_attention_reports : 0.0,
                   s ? s->coverage_lanes : 0.0,
                   s ? s->coverage_ran_lanes : 0.0,
                   s ? s->coverage_skipped_lanes : 0.0,
                   s ? s->coverage_failed_lanes : 0.0,
                       s ? s->coverage_gaps : 0.0,
                       s ? s->coverage_blocker_gaps : 0.0,
                       s ? s->coverage_advisory_gaps : 0.0,
                       s ? s->coverage_latest_report_advisory_gaps : 0.0,
                       s ? s->coverage_latest_report_companion_skipped_lanes : 0.0,
                       s ? s->coverage_reports_considered : 0.0,
                   s ? s->coverage_campaign_reports_considered : 0.0,
                     s ? s->coverage_companion_reports_considered : 0.0,
                     s ? s->coverage_disabled_lanes : 0.0,
                     s ? s->coverage_budget_short_lanes : 0.0,
                     s ? s->coverage_missing_tool_lanes : 0.0,
                     fuzz_all_ratio_percent(s ? s->coverage_ran_lanes : 0.0,
                                            s ? s->coverage_lanes : 0.0),
                     fuzz_all_ratio_percent(s ? s->coverage_ran_lanes : 0.0,
                                            s ? s->coverage_lanes : 0.0),
                     fuzz_all_not_run_lanes(s ? s->coverage_ran_lanes : 0.0,
                                            s ? s->coverage_lanes : 0.0),
                       s ? s->target_thread_years : 0.0,
                     s ? s->remaining_thread_years : 0.0,
                       s ? s->target_percent : 0.0,
                       s ? s->target_percent : 0.0,
                       s ? s->target_percent : 0.0,
                       s ? fuzz_all_campaign_remaining_percent(s->target_percent) : 100.0,
                       s ? fuzz_all_campaign_remaining_percent(s->target_percent) : 100.0,
                     s ? s->runs_needed : 0.0,
                   s ? s->wall_days_needed : 0.0,
                   s ? s->wall_hours_needed : 0.0,
                   s ? s->thread_hours_needed : 0.0,
                   s ? s->runs_per_day : 0.0,
                   s ? s->thread_years_per_day : 0.0,
                   s ? s->completion_eta_epoch : 0.0,
                   s ? s->latest_finding_live : 0.0,
                   s ? s->latest_finding_missing : 0.0,
                   s ? s->historical_finding_live : 0.0,
                   s ? s->historical_finding_missing : 0.0,
                   s ? s->latest_known_reproduced : 0.0,
                   s ? s->latest_known_lost : 0.0,
                   s ? s->latest_known_baseline : 0.0,
                   s ? s->historical_known_reproduced : 0.0,
                   s ? s->historical_known_lost : 0.0,
                   s ? s->historical_known_baseline : 0.0,
                   s ? s->perf_hotspots : 0.0,
                   s ? s->perf_max_ratio : 0.0,
                   s ? s->historical_perf_hotspots : 0.0,
                   s ? s->historical_perf_max_ratio : 0.0,
                   s ? s->latest_perf_hotspots : 0.0,
                   s ? s->latest_perf_max_ratio : 0.0,
                   s ? s->latest_failure_count : 0.0,
                   s ? s->latest_sub_failures : 0.0,
                   s ? s->latest_finding_live : 0.0,
                   s ? s->latest_finding_missing : 0.0,
                   s ? s->latest_known_reproduced : 0.0,
                   s ? s->latest_known_lost : 0.0,
                   s ? s->latest_known_baseline : 0.0,
                       s ? s->latest_perf_hotspots : 0.0,
                       s ? s->latest_perf_max_ratio : 0.0,
                       s ? s->latest_full_pressure_failure_count : 0.0,
                       s ? s->latest_full_pressure_sub_failures : 0.0,
                       s ? s->latest_full_pressure_finding_live : 0.0,
                       s ? s->latest_full_pressure_finding_missing : 0.0,
                       s ? s->latest_full_pressure_known_reproduced : 0.0,
                       s ? s->latest_full_pressure_known_lost : 0.0,
                       s ? s->latest_full_pressure_known_baseline : 0.0,
                       s ? s->latest_full_pressure_perf_hotspots : 0.0,
                       s ? s->latest_full_pressure_perf_max_ratio : 0.0,
                       s ? s->current_perf_cases : 0.0,
                       s ? s->latest_full_pressure_perf_rows : 0.0,
                       s && s->latest_full_pressure_perf_suite_current ? "true" : "false",
                       correctness_findings,
                       compiler_findings,
                         known_bug_findings,
                         perf_hotspots_open,
                         perf_worst_ratio,
                         perf_watchlist_open,
                         fuzz_all_perf_watchlist_threshold(),
                         s ? s->coverage_detail_count : 0,
                         s ? s->coverage_backlog_lanes : 0,
                         s ? s->coverage_detail_rows : 0,
                   s && s->active_clear ? "true" : "false",
                   s && s->full_pressure_ready ? "true" : "false",
                   s && s->long_run_ready ? "true" : "false",
                   s && s->target_reached ? "true" : "false",
                   s && s->campaign_complete ? "true" : "false",
                   s && s->ny_bin_exists ? "true" : "false",
                   s && s->cache_policy_ok ? "true" : "false",
                   s && s->old_nytrix_test_scratch_absent ? "true" : "false",
                   s && s->old_nytrix_fuzz_absent ? "true" : "false",
                   s && s->nynth_git_ok ? "true" : "false",
                   s && s->nynth_git_dirty ? "true" : "false",
                   s && s->nytrix_git_ok ? "true" : "false",
                   s && s->nytrix_git_dirty ? "true" : "false",
                       s && s->latest_report_ok ? "true" : "false",
                       s && s->latest_report_attention ? "true" : "false",
                       s && s->latest_report_clean ? "true" : "false",
                       s && s->latest_full_pressure_ok ? "true" : "false",
                       s && s->latest_full_pressure_attention ? "true" : "false",
                       s && s->latest_full_pressure_clean ? "true" : "false",
                       s && s->latest_report_demoted_non_reproducing_afl_timeout ? "true" : "false",
                       s && s->latest_full_pressure_demoted_non_reproducing_afl_timeout ? "true" : "false",
                         s && s->strict ? "true" : "false",
                   s && s->allow_incomplete_coverage ? "true" : "false",
                       s && s->allow_full_pressure_remediation ? "true" : "false",
                       s && s->refreshed ? "true" : "false",
                       s ? s->blocker_count : 0,
                       s && s->blocker_count == 0 && s->long_run_ready ?
                           "true" : "false");
     append_fuzz_all_campaign_alias_fields(
         &row, s ? s->thread_years : 0.0,
         s ? s->target_thread_years : 0.0,
         s ? s->remaining_thread_years : 0.0,
         s ? s->target_percent : 0.0,
         s ? s->runs_needed : 0.0,
         s ? s->wall_hours_needed : 0.0,
         s ? s->wall_days_needed : 0.0,
         projection.thread_years_per_run, projection.target_percent_per_run,
         s ? s->runs_per_day : 0.0,
         s ? s->thread_years_per_day : 0.0,
         s ? s->campaign_plan_wall_hours : 0.0,
         s ? s->campaign_plan_threads : "",
         s ? s->completion_eta_local : "");
  (void)sb_append(&row, ",\"campaign_first_report\":");
  (void)sb_append_json_str(&row, s ? s->campaign_first_report : "");
  (void)sb_appendf(&row,
                   ",\"coverage_queue_count\":%d,"
                   "\"coverage_queue_non_advisory_count\":%d,"
                   "\"coverage_queue_advisory_count\":%d",
                   s ? s->coverage_queue_count : 0,
                   s ? s->coverage_queue_non_advisory_count : 0,
                   s ? s->coverage_queue_advisory_count : 0);
  (void)sb_append(&row, ",\"coverage_queue_lanes\":");
  (void)sb_append_json_str(&row, s ? s->coverage_queue_lanes : "");
  append_fuzz_all_coverage_queue_json(&row, coverage_queue_json);
  (void)sb_append(&row, ",\"coverage_state\":");
  (void)sb_append_json_str(
      &row,
      s ? fuzz_all_coverage_state(s->coverage_lanes, s->coverage_ran_lanes,
                                  s->coverage_blocker_gaps,
                                  s->coverage_failed_lanes)
        : "missing");
  (void)sb_append(&row, ",\"campaign_state\":");
  (void)sb_append_json_str(
      &row, fuzz_all_campaign_state(campaign_ready,
                                    s && s->target_reached,
                                    s && s->campaign_complete));
  (void)sb_append(&row, ",\"campaign_incomplete_reason\":");
  (void)sb_append_json_str(
      &row, fuzz_all_campaign_incomplete_reason(campaign_ready,
                                                s ? s->blocker_count : 0.0,
                                                s ? s->active_items : 0.0,
                                                s && s->target_reached,
                                                s && s->campaign_complete));
  (void)sb_append(&row, ",\"completion_state\":");
  (void)sb_append_json_str(
      &row, fuzz_all_campaign_state(campaign_ready,
                                    s && s->target_reached,
                                    s && s->campaign_complete));
  (void)sb_append(&row, ",\"completion_reason\":");
  (void)sb_append_json_str(
      &row, fuzz_all_campaign_incomplete_reason(campaign_ready,
                                                s ? s->blocker_count : 0.0,
                                                s ? s->active_items : 0.0,
                                                s && s->target_reached,
                                                s && s->campaign_complete));
  (void)sb_append(&row, ",\"perf_max_case\":");
  (void)sb_append_json_str(&row, s ? s->perf_max_case : "");
  (void)sb_append(&row, ",\"historical_perf_max_case\":");
  (void)sb_append_json_str(&row, s ? s->historical_perf_max_case : "");
  (void)sb_append(&row, ",\"active_perf_max_case\":");
  (void)sb_append_json_str(&row, s ? s->perf_max_case : "");
  (void)sb_append(&row, ",\"latest_perf_max_case\":");
  (void)sb_append_json_str(&row, s ? s->latest_perf_max_case : "");
  (void)sb_append(&row, ",\"latest_full_pressure_perf_max_case\":");
  (void)sb_append_json_str(&row, s ? s->latest_full_pressure_perf_max_case : "");
  (void)sb_appendf(
      &row, ",\"latest_full_pressure_perf_max_slowdown_percent\":%.2f",
      fuzz_all_perf_slowdown_percent(
          s ? s->latest_full_pressure_perf_max_ratio : 0.0));
  (void)sb_appendf(&row, ",\"perf_worst_slowdown_percent\":%.2f",
                   fuzz_all_perf_slowdown_percent(perf_worst_ratio));
  (void)sb_append(&row, ",\"perf_worst_case\":");
  (void)sb_append_json_str(&row, perf_worst_case ? perf_worst_case : "");
  (void)sb_append(&row, ",\"perf_watchlist_case\":");
  (void)sb_append_json_str(&row, perf_watchlist_case ? perf_watchlist_case : "");
    (void)sb_appendf(&row,
                     ",\"advisory_timeouts\":%.0f,"
                     "\"current_advisory_timeouts\":%.0f,"
                   "\"historical_advisory_timeouts\":%.0f,"
                   "\"non_reproducing_afl_timeouts\":%.0f,"
                   "\"historical_non_reproducing_afl_timeouts\":%.0f,"
                   "\"latest_only_non_reproducing_afl_timeout\":%s",
                   s ? s->non_reproducing_afl_timeouts : 0.0,
                   s ? s->non_reproducing_afl_timeouts : 0.0,
                   s ? s->historical_non_reproducing_afl_timeouts : 0.0,
                   s ? s->non_reproducing_afl_timeouts : 0.0,
                     s ? s->historical_non_reproducing_afl_timeouts : 0.0,
                     s && s->latest_only_non_reproducing_afl_timeout > 0.0 ?
                         "true" : "false");
     (void)sb_append(&row, ",\"advisory_state\":");
     (void)sb_append_json_str(&row,
         s ? fuzz_all_advisory_state(s->non_reproducing_afl_timeouts,
                                     s->historical_non_reproducing_afl_timeouts)
           : "clear");
  double effective_advisory_timeouts = s ?
      fuzz_all_effective_advisory_timeouts(
          s->non_reproducing_afl_timeouts,
          s->advisory_recheck_raw_repro_checked,
          s->advisory_recheck_raw_repro_passed,
          s->advisory_recheck_raw_repro_timeouts,
          s->advisory_recheck_raw_repro_unexpected,
          s->advisory_recheck_command) : 0.0;
  (void)sb_appendf(&row,
                   ",\"effective_advisory_timeouts\":%.0f,"
                   "\"advisory_effective_timeouts\":%.0f",
                   effective_advisory_timeouts,
                   effective_advisory_timeouts);
  (void)sb_append(&row, ",\"advisory_penalty_state\":");
  (void)sb_append_json_str(
      &row, fuzz_all_advisory_penalty_state(effective_advisory_timeouts));
     (void)sb_append(&row, ",\"latest_full_pressure_clean_reason\":");
  (void)sb_append_json_str(&row,
                           s ? s->latest_full_pressure_clean_reason : "");
  (void)sb_appendf(&row,
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
                   score.stability_score, score.stability_score,
                   score.stability_score, score.stability_score,
                   score.stability_score, score.stability_score,
                   fuzz_all_score_good_threshold(),
                   fuzz_all_language_good_gap_percent(score.stability_score),
                   score.signal_score,
                   score.evidence_cap,
                   score.signal_score,
                   score.evidence_cap,
                   projection.thread_years_per_run,
                   projection.target_percent_per_run,
                   projection.next_run_target_percent,
                   projection.next_run_stability_score,
                   projection.next_run_stability_score,
                   projection.next_run_stability_delta,
                   projection.next_run_stability_score,
                   projection.next_run_stability_score,
                   projection.next_run_stability_delta,
                   projection.runs_to_good_stability,
                   projection.runs_to_good_stability,
                   projection.runs_to_good_stability,
                   projection.runs_to_good_days,
                   projection.runs_to_good_days,
                   projection.runs_to_good_days,
                   projection.runs_to_good_days,
                   projection.runs_to_good_days,
                   score.latest_report_age_seconds,
                   score.latest_report_age_seconds >= 0.0 ?
                       fuzz_all_freshness_age_hours(score.latest_report_age_seconds) : -1.0,
                   score.latest_report_stale_after_hours,
                   fuzz_all_freshness_remaining_hours(
                       score.latest_report_age_seconds,
                       score.latest_report_stale_after_hours),
                   fuzz_all_freshness_overdue_hours(
                       score.latest_report_age_seconds,
                       score.latest_report_stale_after_hours),
                   score.latest_report_fresh ? "true" : "false",
                   score.latest_full_pressure_report_age_seconds,
                   score.latest_full_pressure_report_age_seconds >= 0.0 ?
                       fuzz_all_freshness_age_hours(
                           score.latest_full_pressure_report_age_seconds) : -1.0,
                   score.latest_full_pressure_report_stale_after_hours,
                   fuzz_all_freshness_remaining_hours(
                       score.latest_full_pressure_report_age_seconds,
                       score.latest_full_pressure_report_stale_after_hours),
                   fuzz_all_freshness_overdue_hours(
                       score.latest_full_pressure_report_age_seconds,
                       score.latest_full_pressure_report_stale_after_hours),
                   score.latest_full_pressure_report_fresh ? "true" : "false",
                   score.evidence_fresh ? "true" : "false",
                   fuzz_all_evidence_freshness_overdue_hours(
                       score.latest_report_age_seconds,
                       score.latest_report_stale_after_hours,
                       score.latest_full_pressure_report_age_seconds,
                       score.latest_full_pressure_report_stale_after_hours),
                   score.gate_penalty, score.correctness_penalty,
                   score.perf_penalty, score.advisory_penalty,
                   score.environment_penalty, score.freshness_penalty);
  append_fuzz_all_compact_score_alias_fields(
      &row, score.stability_score, score.label,
      projection.next_run_stability_score, score.latest_report_age_seconds,
      score.latest_report_stale_after_hours,
      score.latest_full_pressure_report_age_seconds,
      score.latest_full_pressure_report_stale_after_hours);
  (void)sb_append(&row, ",\"stability_label\":");
  (void)sb_append_json_str(&row, score.label ? score.label : "");
  (void)sb_append(&row, ",\"thread_years_per_run_source\":");
  (void)sb_append_json_str(&row,
                           projection.thread_years_per_run_source ?
                               projection.thread_years_per_run_source : "");
  (void)sb_append(&row, ",\"language_score_label\":");
  (void)sb_append_json_str(&row, score.label ? score.label : "");
  (void)sb_append(&row, ",\"stability_note\":");
  (void)sb_append_json_str(&row, score.note);
  (void)sb_append(&row, ",\"language_score_note\":");
  (void)sb_append_json_str(&row, score.note);
    (void)sb_append(&row, ",\"completion_eta_local\":");
  (void)sb_append_json_str(&row, s ? s->completion_eta_local : "");
  (void)sb_append(&row, ",\"run_command\":");
  (void)sb_append_json_str(&row, s ? s->run_command : "");
    (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
    char *out = sb_take(&row);
    free(perf_watchlist_action_command);
  free(optimization_target_command);
    return out;
}

static bool write_fuzz_all_status_markdown(const char *markdown_path,
                                           const fuzz_all_status_summary_t *s,
                                           const string_list_t *rows,
                                           const fuzz_all_run_state_summary_t
                                               *coverage_next_run_state) {
  if (!markdown_path || !*markdown_path) return true;
  if (!s || !rows) return false;
  fuzz_all_score_summary_t score;
  fuzz_all_score_from_status(s, &score);
  fuzz_all_projection_summary_t projection;
  fuzz_all_projection_from_status(s, &score, &projection);
  bool campaign_ready = s->blocker_count == 0 && s->long_run_ready;
  double compiler_findings = s->latest_finding_live + s->latest_finding_missing;
  double known_bug_findings = s->latest_known_reproduced +
                              s->latest_known_lost +
                              s->latest_known_baseline;
  double perf_worst_ratio = s->latest_perf_max_ratio;
  const char *perf_worst_case = s->latest_perf_max_case;
  if (s->latest_full_pressure_perf_suite_current &&
      s->latest_full_pressure_perf_max_ratio > perf_worst_ratio) {
    perf_worst_ratio = s->latest_full_pressure_perf_max_ratio;
    perf_worst_case = s->latest_full_pressure_perf_max_case;
  }
  perf_worst_case = fuzz_all_perf_effective_worst_case(
      perf_worst_case, s->perf_watchlist_artifact_readable,
      s->perf_watchlist_artifact_fresh,
      s->perf_watchlist_artifact_max_ratio,
      s->perf_watchlist_artifact_max_case);
  perf_worst_ratio = fuzz_all_perf_effective_worst_ratio(
      perf_worst_ratio, s->perf_watchlist_artifact_readable,
      s->perf_watchlist_artifact_fresh,
      s->perf_watchlist_artifact_max_ratio);
  double perf_watchlist_open =
      fuzz_all_perf_watchlist_effective_open(
          s->latest_perf_hotspots, perf_worst_ratio,
          s->perf_watchlist_artifact_readable,
          s->perf_watchlist_artifact_fresh,
          s->perf_watchlist_artifact_hotspots);
  const char *perf_watchlist_state =
      fuzz_all_perf_watchlist_state(perf_watchlist_open,
                                    s->perf_watchlist_artifact_readable,
                                    s->perf_watchlist_artifact_fresh,
                                    s->perf_watchlist_artifact_hotspots);
  const char *perf_watchlist_action =
      fuzz_all_perf_watchlist_action(perf_watchlist_state);
  char *perf_watchlist_action_command =
      fuzz_all_perf_watchlist_action_command(
          perf_watchlist_state, s->perf_watchlist_command,
          s->perf_watchlist_markdown, s->perf_watchlist_report);
  const char *optimization_reason =
      fuzz_all_optimization_reason(perf_watchlist_state);
  const char *optimization_case =
      fuzz_all_optimization_case(s->perf_watchlist_artifact_readable,
                                 s->perf_watchlist_artifact_fresh,
                                 s->perf_watchlist_artifact_hotspots,
                                 s->perf_watchlist_artifact_max_case,
                                 s->perf_watchlist_artifact_max_ratio,
                                 perf_watchlist_open, perf_worst_case);
  double optimization_ratio =
      fuzz_all_optimization_ratio(s->perf_watchlist_artifact_readable,
                                  s->perf_watchlist_artifact_fresh,
                                  s->perf_watchlist_artifact_hotspots,
                                  s->perf_watchlist_artifact_max_ratio,
                                  perf_watchlist_open, perf_worst_ratio);
  const char *optimization_artifact =
      fuzz_all_optimization_artifact_path(
          s->perf_watchlist_artifact_readable,
          s->perf_watchlist_artifact_fresh,
          s->perf_watchlist_artifact_hotspots,
          s->perf_watchlist_artifact_max_ratio,
          s->perf_watchlist_artifact_max_artifact);
  const char *optimization_ny_source =
      fuzz_all_optimization_artifact_path(
          s->perf_watchlist_artifact_readable,
          s->perf_watchlist_artifact_fresh,
          s->perf_watchlist_artifact_hotspots,
          s->perf_watchlist_artifact_max_ratio,
          s->perf_watchlist_artifact_max_ny_source);
  const char *optimization_c_source =
      fuzz_all_optimization_artifact_path(
          s->perf_watchlist_artifact_readable,
          s->perf_watchlist_artifact_fresh,
          s->perf_watchlist_artifact_hotspots,
          s->perf_watchlist_artifact_max_ratio,
          s->perf_watchlist_artifact_max_c_source);
  char *optimization_target_command =
      fuzz_all_optimization_target_command(optimization_ny_source,
                                           optimization_c_source,
                                           optimization_artifact);
  fuzz_all_recommendation_t recommendation;
  fuzz_all_recommendation_make(
      &recommendation, campaign_ready,
      fuzz_all_state_phase_live(s->state_phase), score.evidence_fresh,
      s->cache_policy_ok, s->ny_bin_exists,
      s->active_old_nytrix_output_writer_present, s->blocker_count,
      s->active_items, compiler_findings + known_bug_findings,
        s->latest_perf_hotspots, projection.runs_to_good_stability,
        s->runs_needed, s->target_reached, s->campaign_complete,
        s->next_handoff_command[0] ? s->next_handoff_command :
            s->next_command,
        s->state_command, s->old_path_command,
        s->advisory_action_command, s->progress_command,
        s->coverage_next_command, s->coverage_next_guarded_command,
        s->coverage_next_low_cpu_command,
        s->coverage_next_preview_command);
  bool recommendation_missing_evidence =
      strcmp(recommendation.action, "run-missing-evidence") == 0;
  bool campaign_state_refresh_required =
      !s->state_readable ||
      (!fuzz_all_state_phase_live(s->state_phase) && !s->state_fresh);
  const char *campaign_state_refresh_command =
      campaign_state_refresh_required ? s->state_refresh_command : "";
  if (campaign_state_refresh_required &&
      !recommendation_missing_evidence &&
      recommendation.preview_command[0])
    campaign_state_refresh_command = recommendation.preview_command;
  char *status_json_control_path = NULL;
  size_t status_md_len = strlen(markdown_path);
  if (status_md_len > 3 &&
      strcmp(markdown_path + status_md_len - 3, ".md") == 0) {
    (void)asprintf(&status_json_control_path, "%.*s.json",
                   (int)(status_md_len - 3), markdown_path);
  } else {
    status_json_control_path =
        path_with_suffix_ext(markdown_path, "", ".json");
  }
  char root[4096] = {0};
  (void)find_nynth_root(root, sizeof(root));
  char *status_json_control_rel =
      rel_path_dup(root, status_json_control_path ?
                             status_json_control_path : "");
  char *quick_jq_command =
      fuzz_all_quick_jq_command(
          status_json_control_rel && *status_json_control_rel ?
              status_json_control_rel : "build/fuzz/all/status.json");
  char *compact_jq_command =
      fuzz_all_compact_jq_command(
          status_json_control_rel && *status_json_control_rel ?
              status_json_control_rel : "build/fuzz/all/status.json");
  char *state_compact_jq_command =
      fuzz_all_state_compact_jq_command(
          s->state_file[0] ? s->state_file : "build/fuzz/all/run-next-state.json");
  time_t now = time(NULL);
  struct tm tm_now;
  char stamp[64] = {0};
  if (localtime_r(&now, &tm_now))
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %z", &tm_now);
  str_buf_t md = {0};
  (void)sb_append(&md, "# Nynth Campaign Status\n\n");
  if (stamp[0]) {
    (void)sb_append(&md, "Generated: ");
    md_append_code(&md, stamp);
    (void)sb_append(&md, "\n\n");
  }
  (void)sb_append(&md, "## TLDR\n\n");
  (void)sb_append(&md, "- Gate: ");
  (void)sb_append(&md, s->long_run_ready && s->blocker_count == 0 ? "`ready`" : "`blocked`");
  (void)sb_appendf(&md,
                   "; blockers %d; active %.0f; progress %.4f%%; complete `%s`; cache `%s`.\n",
                   s->blocker_count, s->active_items, s->target_percent,
                   s->campaign_complete ? "true" : "false",
                   s->cache_policy_ok ? "ok" : "bad");
  (void)sb_append(&md, "- Completion: ");
  md_append_code(&md, fuzz_all_campaign_state(campaign_ready,
                                              s->target_reached,
                                              s->campaign_complete));
  (void)sb_append(&md, "; reason ");
  md_append_code(&md, fuzz_all_campaign_incomplete_reason(campaign_ready,
                                                          s->blocker_count,
                                                          s->active_items,
                                                          s->target_reached,
                                                          s->campaign_complete));
  (void)sb_appendf(&md, "; target reached `%s`.\n",
                   s->target_reached ? "true" : "false");
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
                   "- Confidence: campaign evidence %.4f%%; remaining %.4f%%; lang score %.2f%% `%s`; good >= %.2f%%; gap %.2f%%.\n",
                   s->target_percent,
                   fuzz_all_campaign_remaining_percent(s->target_percent),
                   score.stability_score,
                   score.label ? score.label : "",
                   fuzz_all_score_good_threshold(),
                   fuzz_all_language_good_gap_percent(score.stability_score));
  (void)sb_appendf(&md, "- Score note: %s.\n", score.note);
  double effective_advisory_timeouts = fuzz_all_effective_advisory_timeouts(
      s->non_reproducing_afl_timeouts,
      s->advisory_recheck_raw_repro_checked,
      s->advisory_recheck_raw_repro_passed,
      s->advisory_recheck_raw_repro_timeouts,
      s->advisory_recheck_raw_repro_unexpected,
      s->advisory_recheck_command);
  (void)sb_append(&md, "- Advisory: ");
  md_append_code(&md, fuzz_all_advisory_state(
                          s->non_reproducing_afl_timeouts,
                          s->historical_non_reproducing_afl_timeouts));
  (void)sb_appendf(&md,
                   "; effective %s/%.0f; current timeouts %.0f; historical %.0f; penalty %.2f.\n",
                   fuzz_all_advisory_penalty_state(
                       effective_advisory_timeouts),
                   effective_advisory_timeouts,
                   s->non_reproducing_afl_timeouts,
                   s->historical_non_reproducing_afl_timeouts,
                   score.advisory_penalty);
  (void)sb_appendf(&md,
                   "- Source: latest %.1f h/%.0fh %s; full-pressure %.1f h/%.0fh %s.\n",
                   score.latest_report_age_seconds >= 0.0 ?
                       score.latest_report_age_seconds / 3600.0 : -1.0,
                   score.latest_report_stale_after_hours,
                   score.latest_report_fresh ? "ok" : "stale",
                   score.latest_full_pressure_report_age_seconds >= 0.0 ?
                       score.latest_full_pressure_report_age_seconds / 3600.0 : -1.0,
                   score.latest_full_pressure_report_stale_after_hours,
                   score.latest_full_pressure_report_fresh ? "ok" : "stale");
  (void)sb_appendf(&md,
                   "- Full-pressure gate: effective clean `%s`; raw ok `%s`; reason ",
                   s->latest_full_pressure_clean ? "true" : "false",
                   s->latest_full_pressure_ok ? "true" : "false");
  md_append_code(&md, s->latest_full_pressure_clean_reason[0] ?
                          s->latest_full_pressure_clean_reason : "unknown");
  (void)sb_appendf(&md, "; failures %.0f.\n",
                   s->latest_full_pressure_failure_count);
  append_fuzz_all_compiler_std_audit_markdown(
      &md, s->compiler_std_audit_readable, s->compiler_std_audit_markdown,
      s->compiler_std_audit_report, s->compiler_std_audit_command,
      s->runtime_surface_state, s->crt_surface_state,
      s->crt_export_coverage_percent, s->crt_unreferenced_count,
      s->crt_unreferenced_percent, s->crt_unreferenced_family_count,
      s->crt_top_unreferenced_family,
      s->crt_top_unreferenced_family_count,
      s->crt_next_action, s->crt_next_unreferenced_family,
      s->crt_next_unreferenced_count);
  if (s->coverage_lanes > 0.0) {
    (void)sb_append(&md, "- Coverage: ");
    md_append_code(&md, fuzz_all_coverage_state(s->coverage_lanes,
                                                s->coverage_ran_lanes,
                                                s->coverage_blocker_gaps,
                                                s->coverage_failed_lanes));
    (void)sb_appendf(&md,
                     "; %.0f/%.0f lanes (%.2f%%); not-run %.0f "
                     "(actionable backlog %.0f; disabled %.0f; "
                     "budget-short %.0f; missing-tool %.0f); details %.0f; "
                     "blockers %.0f.\n",
                     s->coverage_ran_lanes, s->coverage_lanes,
                     fuzz_all_ratio_percent(s->coverage_ran_lanes,
                                            s->coverage_lanes),
                     fuzz_all_not_run_lanes(s->coverage_ran_lanes,
                                            s->coverage_lanes),
                     (double)s->coverage_detail_count,
                     s->coverage_disabled_lanes,
                     s->coverage_budget_short_lanes,
                     s->coverage_missing_tool_lanes,
                     (double)s->coverage_detail_rows,
                       s->coverage_blocker_gaps);
    }
  if (s->coverage_queue_lanes[0]) {
    (void)sb_append(&md, "- Coverage queue: ");
    md_append_inline(&md, s->coverage_queue_lanes);
    (void)sb_appendf(&md, "; primary %d; advisory %d.\n",
                     s->coverage_queue_non_advisory_count,
                     s->coverage_queue_advisory_count);
  }
  if (s->coverage_next_lane[0] && s->coverage_next_command[0]) {
    (void)sb_append(&md, "- Coverage next: ");
    md_append_code(&md, s->coverage_next_lane);
    (void)sb_append(&md, " ");
    md_append_code(&md, s->coverage_next_severity[0] ?
                         s->coverage_next_severity : "medium");
    if (s->coverage_next_category[0]) {
      (void)sb_append(&md, " ");
      md_append_code(&md, s->coverage_next_category);
    }
    if (s->coverage_next_reason[0]) {
      (void)sb_append(&md, "; reason ");
      md_append_code(&md, s->coverage_next_reason);
    }
    (void)sb_append(&md, "; command ");
    md_append_code(&md, s->coverage_next_command);
    if (s->coverage_next_guarded_command[0]) {
      (void)sb_append(&md, "; guarded ");
      md_append_code(&md, s->coverage_next_guarded_command);
    }
    if (s->coverage_next_low_cpu_command[0]) {
      (void)sb_append(&md, "; low-cpu ");
      md_append_code(&md, s->coverage_next_low_cpu_command);
    }
    if (s->coverage_next_preview_command[0]) {
      (void)sb_append(&md, "; preview ");
      md_append_code(&md, s->coverage_next_preview_command);
    }
    if (s->coverage_next_state_command[0]) {
      (void)sb_append(&md, "; state ");
      md_append_code(&md, s->coverage_next_state_command);
    }
    if (s->coverage_next_state_refresh_command[0]) {
      (void)sb_append(&md, "; refresh-state ");
      md_append_code(&md, s->coverage_next_state_refresh_command);
    }
    if (s->coverage_next_stop_command[0]) {
      (void)sb_append(&md, "; pause ");
      md_append_code(&md, s->coverage_next_stop_command);
      if (s->coverage_next_resume_command[0]) {
        (void)sb_append(&md, "; resume ");
        md_append_code(&md, s->coverage_next_resume_command);
      }
    }
    (void)sb_append(&md, ".\n");
    append_fuzz_all_state_summary_markdown(
        &md, recommendation_missing_evidence ?
                 "Recommended state" : "Coverage next state",
        s->coverage_next_state_command,
        s->coverage_next_state_refresh_command,
        coverage_next_run_state);
    if (s->coverage_next_state_refresh_required) {
      (void)sb_append(&md, recommendation_missing_evidence ?
                               "- Recommended state refresh: " :
                               "- Coverage next state refresh: ");
      md_append_code(&md, s->coverage_next_state_refresh_reason[0] ?
                          s->coverage_next_state_refresh_reason : "stale");
      if (s->recommended_state_refresh_command[0]) {
        (void)sb_append(&md, "; command ");
        md_append_code(&md, s->recommended_state_refresh_command);
      }
      (void)sb_append(&md, ".\n");
    }
  }
  if (s->coverage_reports_considered > 0.0) {
    (void)sb_appendf(
        &md,
        "- Coverage evidence: %.0f reports (%.0f campaign + %.0f companion); latest advisory %.0f, companion skips %.0f.\n",
        s->coverage_reports_considered,
        s->coverage_campaign_reports_considered,
        s->coverage_companion_reports_considered,
        s->coverage_latest_report_advisory_gaps,
        s->coverage_latest_report_companion_skipped_lanes);
  }
  if (s->ignored_no_evidence_reports > 0.0) {
    (void)sb_appendf(&md,
                     "- Evidence hygiene: ignored %.0f no-evidence attempt%s.\n",
                     s->ignored_no_evidence_reports,
                     s->ignored_no_evidence_reports == 1.0 ? "" : "s");
  }
  char status_md_freshness_action_command[4096] = {0};
  if (score.freshness_penalty > 0.0 && s->next_command[0])
    fuzz_all_freshness_action_command(status_md_freshness_action_command,
                                      sizeof(status_md_freshness_action_command),
                                      s->next_handoff_command[0] ?
                                          s->next_handoff_command :
                                          s->next_command);
  if (status_md_freshness_action_command[0]) {
    (void)sb_append(&md, "- Freshen evidence: ");
    md_append_code(&md, status_md_freshness_action_command);
    (void)sb_append(&md, ".\n");
  }
  (void)sb_appendf(&md,
                   "- Progress: %.6f/%.6f thread-years; %.8f remaining; %.0f runs; %.2f wall-days",
                   s->thread_years, s->target_thread_years,
                   s->remaining_thread_years, s->runs_needed,
                   s->wall_days_needed);
  if (s->completion_eta_local[0]) {
    (void)sb_append(&md, "; ETA ");
    md_append_code(&md, s->completion_eta_local);
  }
  (void)sb_append(&md, ".\n");
  if (s->wall_hours_needed > 0.0 || s->thread_hours_needed > 0.0) {
    (void)sb_appendf(&md,
                     "- Budget: %.2f wall-hours; %.2f thread-hours.\n",
                     s->wall_hours_needed, s->thread_hours_needed);
  }
  if (s->runs_per_day > 0.0 || s->thread_years_per_day > 0.0) {
    (void)sb_appendf(&md, "- Pace: %.2f runs/day; %.6f thread-years/day.\n",
                     s->runs_per_day, s->thread_years_per_day);
  }
  if (!recommendation_missing_evidence &&
      score.freshness_penalty <= 0.0 &&
      s->preview_command[0] &&
      strcmp(s->preview_command, recommendation.preview_command) != 0) {
      (void)sb_append(&md, "- Preview target: ");
      md_append_code(&md, s->preview_command);
      (void)sb_append(&md, ".\n");
    }
  if (!recommendation_missing_evidence && s->state_command[0]) {
    char *state_inspect_command =
        fuzz_all_state_compact_jq_command(s->state_file[0] ?
                                          s->state_file : NULL);
    (void)sb_append(&md, "- State: ");
    if (s->state_readable) {
      md_append_code(&md, s->state_phase[0] ? s->state_phase : "unknown");
      if (s->state_event[0]) {
        (void)sb_append(&md, "/");
        md_append_code(&md, s->state_event);
      }
      (void)sb_appendf(&md, "; age %.0fs; cycle %.0f/%.0f",
                       s->state_age_seconds, s->state_cycle,
                       s->state_cycles);
      if (s->state_child_pid > 0.0)
        (void)sb_appendf(&md, "; child %.0f %s", s->state_child_pid,
                         s->state_child_alive ? "alive" : "dead");
      if (s->state_heartbeat_count > 0.0)
        (void)sb_appendf(&md, "; heartbeats %.0f",
                         s->state_heartbeat_count);
      if (fuzz_all_state_phase_live(s->state_phase))
        (void)sb_appendf(&md, "; stale after %.0fs",
                         fuzz_all_state_stale_after_seconds(
                             s->state_heartbeat_s));
      (void)sb_appendf(&md, "; %s",
                       fuzz_all_state_phase_live(s->state_phase) ? "live"
                                                                 : "not-live");
        (void)sb_appendf(&md, "; %s", s->state_fresh ? "fresh" : "stale");
        if (!s->state_fresh)
          (void)sb_appendf(&md, " (%s)",
              fuzz_all_state_stale_reason_values(s->state_readable,
                                                 s->state_fresh,
                                                 s->state_phase,
                                               s->state_age_seconds,
                                               s->state_heartbeat_s,
                                                 s->state_child_pid,
                                                 s->state_child_alive));
        (void)sb_append(&md, "; inspect ");
      } else {
        (void)sb_append(&md, "not-readable; not-live; stale (missing); inspect ");
      }
    md_append_code(&md, state_inspect_command && *state_inspect_command ?
                        state_inspect_command : s->state_command);
    free(state_inspect_command);
    if (score.freshness_penalty <= 0.0 &&
        campaign_state_refresh_command &&
        *campaign_state_refresh_command) {
      (void)sb_append(&md, "; refresh-state ");
      md_append_code(&md, campaign_state_refresh_command);
    }
    (void)sb_append(&md, ".\n");
  }
  if (!recommendation_missing_evidence && s->stop_command[0]) {
    (void)sb_append(&md, "- Pause: ");
    md_append_code(&md, s->stop_command);
    if (s->resume_command[0]) {
      (void)sb_append(&md, "; resume ");
      md_append_code(&md, s->resume_command);
    }
    (void)sb_append(&md, ".\n");
  }
  if (projection.target_percent_per_run > 0.0) {
    (void)sb_appendf(&md,
                     "- Next clean run: +%.6f thread-years",
                     projection.thread_years_per_run);
    if (projection.thread_years_per_run_source &&
        *projection.thread_years_per_run_source)
      (void)sb_appendf(&md, " (%s)",
                       projection.thread_years_per_run_source);
    (void)sb_appendf(&md,
                     " / +%.4f%% campaign to %.4f%%; lang score %.2f%% (%+.2f)",
                     projection.target_percent_per_run,
                     projection.next_run_target_percent,
                     projection.next_run_stability_score,
                     projection.next_run_stability_delta);
    if (projection.runs_to_good_stability > 0.0) {
      (void)sb_appendf(&md, "; good lang score in %.0f runs",
                       projection.runs_to_good_stability);
      if (projection.runs_to_good_days >= 0.0)
        (void)sb_appendf(&md, " / %.2f days",
                         projection.runs_to_good_days);
    } else if (projection.runs_to_good_stability == 0.0) {
      (void)sb_append(&md, "; already good");
    } else {
      (void)sb_append(&md, "; good milestone needs fresher/cleaner evidence");
    }
    (void)sb_append(&md, ".\n");
  }
  if (s->active_failure_detail_count > 0.0 || s->active_saved_hangs > 0.0 ||
      s->active_saved_crashes > 0.0)
    (void)sb_appendf(&md,
                     "- Active failure evidence: %.0f failed rows, %.0f saved hangs, %.0f saved crashes.\n",
                     s->active_failure_detail_count, s->active_saved_hangs,
                     s->active_saved_crashes);
  if (s->active_saved_inputs > 0.0 || s->active_repro_commands > 0.0)
    (void)sb_appendf(&md,
                     "- Active direct AFL replays: %.0f saved inputs, %.0f replay commands, %.0f raw commands, %.0f ready.\n",
                     s->active_saved_inputs, s->active_repro_commands,
                     s->active_raw_repro_commands,
                     s->active_repro_ready);
  if (s->non_reproducing_afl_timeouts > 0.0) {
    (void)sb_appendf(&md,
                     "- Current demoted AFL timeouts: %.0f non-reproducing after raw replay",
                     s->non_reproducing_afl_timeouts);
    if (s->advisory_recheck_raw_repro_checked > 0.0)
      (void)sb_appendf(&md,
                       "; recheck %.0f/%.0f passed, raw timeouts %.0f, unexpected %.0f",
                       s->advisory_recheck_raw_repro_passed,
                       s->advisory_recheck_raw_repro_checked,
                       s->advisory_recheck_raw_repro_timeouts,
                       s->advisory_recheck_raw_repro_unexpected);
    (void)sb_append(&md, ".\n");
  }
  if (s->historical_non_reproducing_afl_timeouts > 0.0)
  {
    (void)sb_appendf(&md,
                     "- Historical demoted AFL timeout rows: %.0f; see ",
                     s->historical_non_reproducing_afl_timeouts);
    md_append_code(&md, s->historical_worklist_markdown[0] ?
                        s->historical_worklist_markdown :
                        "build/fuzz/all/worklist-history.md");
    (void)sb_append(&md, ".\n");
  }
  if ((s->non_reproducing_afl_timeouts > 0.0 ||
       s->historical_non_reproducing_afl_timeouts > 0.0) &&
      s->advisory_action_command[0]) {
    (void)sb_append(&md, "- Advisory action: ");
    md_append_code(&md, s->advisory_action_command);
    (void)sb_append(&md, ".\n");
  }
  if ((s->non_reproducing_afl_timeouts > 0.0 ||
       s->historical_non_reproducing_afl_timeouts > 0.0) &&
      s->advisory_recheck_command[0]) {
    (void)sb_append(&md, "- Advisory recheck: ");
    md_append_code(
        &md,
        fuzz_all_advisory_recheck_state(
            true,
            s->advisory_recheck_raw_repro_checked,
            s->advisory_recheck_raw_repro_passed,
            s->advisory_recheck_raw_repro_timeouts,
            s->advisory_recheck_raw_repro_unexpected,
            s->advisory_recheck_command));
    if (s->advisory_recheck_raw_repro_checked > 0.0)
      (void)sb_appendf(&md,
                       "; raw replay %.0f/%.0f passed, timeouts %.0f, unexpected %.0f; command ",
                       s->advisory_recheck_raw_repro_passed,
                       s->advisory_recheck_raw_repro_checked,
                       s->advisory_recheck_raw_repro_timeouts,
                       s->advisory_recheck_raw_repro_unexpected);
    else
      (void)sb_append(&md, "; command ");
    md_append_code(&md, s->advisory_recheck_command);
    (void)sb_append(&md, ".\n");
  }
  if (perf_watchlist_open > 0.0 && s->perf_watchlist_command[0]) {
    (void)sb_append(&md, "- Perf watchlist: ");
    md_append_code(&md, s->perf_watchlist_command);
    (void)sb_append(&md, ".\n");
  }
  if (s->perf_watchlist_artifact_readable) {
    (void)sb_appendf(&md,
                     "- Perf watchlist artifact: state `%s`, %.0f rows, %s, age %.1fh/%.0fh, worst ",
                     perf_watchlist_state,
                     s->perf_watchlist_artifact_hotspots,
                     s->perf_watchlist_artifact_fresh ? "fresh" : "stale",
                     s->perf_watchlist_artifact_age_seconds >= 0.0 ?
                         s->perf_watchlist_artifact_age_seconds / 3600.0 : -1.0,
                     s->perf_watchlist_artifact_stale_after_hours);
    if (s->perf_watchlist_artifact_max_case[0])
      md_append_code(&md, s->perf_watchlist_artifact_max_case);
    else
      md_append_code(&md, "unknown");
    (void)sb_appendf(&md, " %.4fx (%.2f%% slower than C); report ",
                     s->perf_watchlist_artifact_max_ratio,
                     fuzz_all_perf_slowdown_percent(
                         s->perf_watchlist_artifact_max_ratio));
    md_append_code(&md, s->perf_watchlist_markdown[0] ?
                            s->perf_watchlist_markdown :
                            s->perf_watchlist_report);
    (void)sb_append(&md, ".\n");
  }
  fuzz_all_append_full_pressure_perf_markdown(
      &md, s->latest_full_pressure_perf_hotspots,
      s->latest_full_pressure_perf_max_ratio,
      s->latest_full_pressure_perf_max_case,
      s->latest_full_pressure_perf_rows, s->current_perf_cases,
      s->latest_full_pressure_perf_suite_current);
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
  if (s->active_raw_repro_command[0]) {
    (void)sb_append(&md, "- Replay: ");
    md_append_code(&md, s->active_raw_repro_command);
    (void)sb_append(&md, ".\n");
  } else if (s->active_primary_command[0]) {
    (void)sb_append(&md, "- Replay: ");
    md_append_code(&md, s->active_primary_command);
    (void)sb_append(&md, ".\n");
  }
  (void)sb_appendf(&md,
                       "- Signals: latest failures %.0f; live %.0f; known %.0f; perf %.0f; watch %.0f >= %.2fx `%s`; coverage %.0f blockers, %.0f advisory",
                       s->latest_failure_count, s->latest_finding_live,
                       s->latest_known_reproduced, s->latest_perf_hotspots,
                       perf_watchlist_open,
                       fuzz_all_perf_watchlist_threshold(),
                       perf_watchlist_state,
                       s->coverage_blocker_gaps, s->coverage_advisory_gaps);
  if (perf_worst_ratio > 0.0)
    (void)sb_appendf(&md, ", worst %.4fx (%.2f%% slower than C)",
                     perf_worst_ratio,
                     fuzz_all_perf_slowdown_percent(perf_worst_ratio));
  if (perf_worst_case && *perf_worst_case) {
    (void)sb_append(&md, " at ");
    md_append_code(&md, perf_worst_case);
  }
  (void)sb_append(&md, ".\n");
  (void)sb_append(&md, "- Report: ");
  if (s->latest_report[0]) {
    (void)sb_append(&md, "latest ");
    md_append_code(&md, s->latest_report);
  } else {
    (void)sb_append(&md, "latest none");
  }
  if (s->latest_full_pressure_report[0]) {
    if (strcmp(s->latest_full_pressure_report, s->latest_report) != 0) {
      (void)sb_append(&md, "; full-pressure ");
      md_append_code(&md, s->latest_full_pressure_report);
    }
    if (!s->latest_full_pressure_perf_suite_current) {
      (void)sb_appendf(&md, "; perf cases %.0f/%.0f",
                       s->latest_full_pressure_perf_rows,
                       s->current_perf_cases);
    }
  } else {
    (void)sb_append(&md, "; no full-pressure report");
  }
  (void)sb_append(&md, ".\n");
  char *ny_bin_rel = rel_path_dup(s->nynth_root, s->ny_bin);
  bool reports_ok = s->history_readable && s->worklist_readable &&
                    s->coverage_readable && s->plan_readable;
  (void)sb_append(&md, "- Files: reports ");
  (void)sb_append(&md, reports_ok ? "`ok`; ny " : "`missing`; ny ");
  md_append_code(&md, ny_bin_rel ? ny_bin_rel : s->ny_bin);
  (void)sb_append(&md, s->ny_bin_exists ? " ok" : " missing");
  free(ny_bin_rel);
  (void)sb_append(&md, ".\n");
  (void)sb_appendf(&md,
                   "- Old Nytrix paths: test scratch `%s`; fuzz `%s`; build cache `%s`; active writer `%s`; present `%.0f`; moved `%.0f`; remaining `%.0f`; artifact leaks `%.0f`; artifacts remaining `%.0f`.\n",
                   s->old_nytrix_test_scratch_absent ? "absent" : "present",
                   s->old_nytrix_fuzz_absent ? "absent" : "present",
                   s->old_nytrix_build_cache_absent ? "absent" : "present",
                   s->active_old_nytrix_output_writer_present ? "present" : "none",
                   s->old_path_present_count,
                   s->old_path_moved_count,
                   s->old_path_remaining_count,
                   s->old_path_artifact_leak_count,
                   s->old_path_artifact_remaining_count);
  (void)sb_append(&md,
                  "- Old-path action: ");
  md_append_code(&md, s->old_path_next_action[0] ?
                     s->old_path_next_action : "none");
  (void)sb_append(&md, "; reason ");
  md_append_code(&md, s->old_path_next_reason[0] ?
                     s->old_path_next_reason :
                     fuzz_all_old_path_next_reason(
                         s->old_path_remaining_count,
                         s->active_old_nytrix_output_writer_present));
  if (s->old_path_apply_command[0]) {
    (void)sb_append(&md, "; apply ");
    md_append_code(&md, s->old_path_apply_command);
  }
  (void)sb_append(&md, "; dry-run ");
  md_append_code(&md, s->old_path_dry_run_command[0] ?
                     s->old_path_dry_run_command :
                     (s->old_path_command[0] ? s->old_path_command :
                                             NYNTH_OLD_PATH_DRY_RUN_COMMAND));
  (void)sb_append(&md, ".\n");
  if (s->active_old_nytrix_output_writer_present) {
    char writer_label[64] = "detected";
    const char *writer = s->active_old_nytrix_output_writer;
    if (writer && strncmp(writer, "pid ", 4) == 0) {
      const char *colon = strchr(writer, ':');
      size_t n = colon ? (size_t)(colon - writer) : 0;
      if (n > 0 && n < sizeof(writer_label)) {
        memcpy(writer_label, writer, n);
        writer_label[n] = '\0';
      }
    }
    (void)sb_append(&md, "- External Nytrix writer: ");
    md_append_code(&md, writer_label);
    if (s->old_path_wait_remaining_seconds >= 0.0)
      (void)sb_appendf(&md, "; wait estimate `%.0fs`",
                       s->old_path_wait_remaining_seconds);
    (void)sb_append(&md, "; details in status JSON.\n");
  }

  (void)sb_append(&md, "\n## Blockers\n\n");
  int blockers = 0;
  for (int i = 0; i < rows->count; ++i) {
    char *kind = json_string_or_empty(rows->items[i], "kind");
    if (!kind || strcmp(kind, "fuzz-all-status-blocker") != 0) {
      free(kind);
      continue;
    }
    char *category = json_string_or_empty(rows->items[i], "category");
    char *severity = json_string_or_empty(rows->items[i], "severity");
    char *reason = json_string_or_empty(rows->items[i], "reason");
    char *command = json_string_or_empty(rows->items[i], "command");
    double count = 0.0;
    (void)extract_json_number(rows->items[i], "count", &count);
    (void)sb_append(&md, "- ");
    md_append_code(&md, category);
    (void)sb_append(&md, " ");
    md_append_code(&md, severity);
    (void)sb_appendf(&md, ": %.0f; ", count);
    md_append_inline(&md, reason);
    char next_script_command[4096] = {0};
    if (s->next_script[0]) {
      if (path_is_absolute(s->next_script))
        snprintf(next_script_command, sizeof(next_script_command), "%s", s->next_script);
      else {
        size_t n = strlen(s->next_script);
        if (n > sizeof(next_script_command) - 3)
          n = sizeof(next_script_command) - 3;
        next_script_command[0] = '.';
        next_script_command[1] = '/';
        memcpy(next_script_command + 2, s->next_script, n);
        next_script_command[2 + n] = '\0';
      }
    }
    bool command_already_shown =
        command && *command &&
        ((s->active_raw_repro_command[0] &&
          strcmp(command, s->active_raw_repro_command) == 0) ||
         (s->active_primary_command[0] &&
          strcmp(command, s->active_primary_command) == 0) ||
         (next_script_command[0] &&
          strcmp(command, next_script_command) == 0));
    if (command && *command && !command_already_shown) {
      (void)sb_append(&md, "\n  Command: ");
      md_append_code(&md, command);
    }
    (void)sb_append(&md, "\n");
    ++blockers;
    free(kind);
    free(category);
    free(severity);
    free(reason);
    free(command);
  }
  if (!blockers) (void)sb_append(&md, "No active blockers.\n");

  int gap_printed = 0;
  for (int i = 0; i < rows->count; ++i) {
    char *kind = json_string_or_empty(rows->items[i], "kind");
    if (!kind || strcmp(kind, "fuzz-all-status-coverage-gap") != 0) {
      free(kind);
      continue;
    }
    char *category = json_string_or_empty(rows->items[i], "category");
    char *severity = json_string_or_empty(rows->items[i], "severity");
    char *lane = json_string_or_empty(rows->items[i], "lane");
    char *reason = json_string_or_empty(rows->items[i], "reason");
    char *command = json_string_or_empty(rows->items[i], "command");
    if (!gap_printed) (void)sb_append(&md, "\n## Coverage Gaps\n\n");
    (void)sb_append(&md, "- ");
    md_append_code(&md, lane);
    (void)sb_append(&md, " ");
    md_append_code(&md, severity);
    (void)sb_append(&md, " ");
    md_append_code(&md, category);
    (void)sb_append(&md, ": ");
    md_append_inline(&md, reason);
    if (command && *command) {
      (void)sb_append(&md, "\n  Command: ");
      md_append_code(&md, command);
    }
    bool selected_coverage_next =
        command && *command && s->coverage_next_command[0] &&
        strcmp(command, s->coverage_next_command) == 0;
    if (selected_coverage_next) {
      if (s->coverage_next_guarded_command[0]) {
        (void)sb_append(&md, "\n  Guarded: ");
        md_append_code(&md, s->coverage_next_guarded_command);
      }
      if (s->coverage_next_low_cpu_command[0]) {
        (void)sb_append(&md, "\n  Low CPU: ");
        md_append_code(&md, s->coverage_next_low_cpu_command);
      }
      if (s->coverage_next_preview_command[0]) {
        (void)sb_append(&md, "\n  Preview: ");
        md_append_code(&md, s->coverage_next_preview_command);
      }
      if (s->coverage_next_state_command[0]) {
        (void)sb_append(&md, "\n  State: ");
        md_append_code(&md, s->coverage_next_state_command);
        if (s->coverage_next_state_refresh_command[0]) {
          (void)sb_append(&md, "; refresh ");
          md_append_code(&md, s->coverage_next_state_refresh_command);
        }
      }
      if (s->coverage_next_stop_command[0]) {
        (void)sb_append(&md, "\n  Pause: ");
        md_append_code(&md, s->coverage_next_stop_command);
        if (s->coverage_next_resume_command[0]) {
          (void)sb_append(&md, "; resume ");
          md_append_code(&md, s->coverage_next_resume_command);
        }
      }
    }
    (void)sb_append(&md, "\n");
    ++gap_printed;
    free(kind);
    free(category);
    free(severity);
    free(lane);
    free(reason);
    free(command);
  }

  (void)sb_append(&md, "\n## Next\n\n```bash\n");
  string_list_t next_lines = {0};
  const char *next_handoff_command =
      s->next_handoff_command[0] ? s->next_handoff_command : s->next_command;
  if (recommendation.preview_command[0]) {
    md_append_unique_shell_line(&md, &next_lines,
                                recommendation.preview_command);
  }
  if (recommendation.command[0] &&
      (!s->next_command[0] || strcmp(recommendation.command,
                                     s->next_command) != 0)) {
    md_append_unique_shell_line(&md, &next_lines, recommendation.command);
  }
  if (!recommendation_missing_evidence &&
      !recommendation.repeat_mode[0] &&
      s->next_command[0] &&
      (strcmp(recommendation.action, "freshen-evidence") != 0 ||
       !recommendation.command[0] ||
       strcmp(recommendation.command, s->next_command) == 0)) {
    md_append_unique_shell_line(&md, &next_lines, s->next_command);
    if (score.freshness_penalty <= 0.0 &&
        projection.runs_to_good_stability > 1.0) {
      char repeat_good[8192];
      snprintf(repeat_good, sizeof(repeat_good),
               "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
               "NYNTH_RUN_REPEAT=good %s",
               next_handoff_command);
      md_append_unique_shell_line(&md, &next_lines, repeat_good);
    }
    if (score.freshness_penalty <= 0.0 &&
        s->runs_needed > 1.0 &&
        projection.runs_to_good_stability >= 0.0) {
      char repeat_target[8192];
      snprintf(repeat_target, sizeof(repeat_target),
               "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
               "NYNTH_RUN_REPEAT=target %s", next_handoff_command);
      md_append_unique_shell_line(&md, &next_lines, repeat_target);
    }
  } else if (!recommendation_missing_evidence &&
             !recommendation.repeat_mode[0] &&
             (strcmp(recommendation.action, "freshen-evidence") != 0 ||
              !recommendation.command[0]) &&
             s->run_command[0]) {
    md_append_unique_shell_line(&md, &next_lines, s->run_command);
  }
  string_list_free(&next_lines);
  (void)sb_append(&md, "```\n");
  bool has_coverage_controls =
      s->coverage_next_state_command[0] ||
      s->coverage_next_state_refresh_command[0] ||
      s->coverage_next_stop_command[0] ||
      s->coverage_next_resume_command[0];
  bool has_campaign_controls =
      !recommendation_missing_evidence &&
      (s->progress_command[0] ||
       s->status_command[0] ||
       (score.freshness_penalty <= 0.0 &&
        campaign_state_refresh_command &&
        *campaign_state_refresh_command) ||
       s->state_command[0] ||
       s->stop_command[0] || s->resume_command[0]);
  if ((quick_jq_command && *quick_jq_command) ||
      (compact_jq_command && *compact_jq_command) ||
      has_coverage_controls || has_campaign_controls) {
    (void)sb_append(&md, "\n## Controls\n\n```bash\n");
    string_list_t control_lines = {0};
    if (quick_jq_command && *quick_jq_command)
      md_append_unique_shell_line(&md, &control_lines, quick_jq_command);
    if (compact_jq_command && *compact_jq_command)
      md_append_unique_shell_line(&md, &control_lines, compact_jq_command);
    if (has_coverage_controls) {
      if (s->coverage_next_state_refresh_command[0])
        md_append_unique_shell_line(&md, &control_lines,
                                    s->coverage_next_state_refresh_command);
      if (s->coverage_next_state_command[0])
        md_append_unique_shell_line(&md, &control_lines,
                                    s->coverage_next_state_command);
      if (s->coverage_next_stop_command[0])
        md_append_unique_shell_line(&md, &control_lines,
                                    s->coverage_next_stop_command);
      if (s->coverage_next_resume_command[0])
        md_append_unique_shell_line(&md, &control_lines,
                                    s->coverage_next_resume_command);
    }
    if (has_campaign_controls) {
      if (s->progress_command[0])
        md_append_unique_shell_line(&md, &control_lines,
                                    s->progress_command);
      if (s->status_command[0])
        md_append_unique_shell_line(&md, &control_lines,
                                    s->status_command);
      if (score.freshness_penalty <= 0.0 &&
          campaign_state_refresh_command &&
          *campaign_state_refresh_command)
        md_append_unique_shell_line(&md, &control_lines,
                                    campaign_state_refresh_command);
      if (state_compact_jq_command && *state_compact_jq_command)
        md_append_unique_shell_line(&md, &control_lines,
                                    state_compact_jq_command);
      if (s->state_command[0] &&
          (!state_compact_jq_command || !*state_compact_jq_command))
        md_append_unique_shell_line(&md, &control_lines,
                                    s->state_command);
      if (s->stop_command[0])
        md_append_unique_shell_line(&md, &control_lines,
                                    s->stop_command);
      if (s->resume_command[0])
        md_append_unique_shell_line(&md, &control_lines,
                                    s->resume_command);
    }
    string_list_free(&control_lines);
    (void)sb_append(&md, "```\n");
  }
    bool ok = md.data && write_file_text(markdown_path, md.data);
  free(perf_watchlist_action_command);
  free(optimization_target_command);
  free(status_json_control_path);
  free(status_json_control_rel);
  free(quick_jq_command);
  free(compact_jq_command);
  free(state_compact_jq_command);
    free(md.data);
  return ok;
}

static char *fuzz_all_stop_file_path(const char *dir_path) {
  char *out = NULL;
  (void)asprintf(&out, "%s/stop",
                 dir_path && *dir_path ? dir_path : "build/fuzz/all");
  return out ? out : strdup("build/fuzz/all/stop");
}

static char *fuzz_all_stop_command(const char *stop_file) {
  char *out = NULL;
  (void)asprintf(&out, "touch %s",
                 stop_file && *stop_file ? stop_file : "build/fuzz/all/stop");
  return out ? out : strdup("touch build/fuzz/all/stop");
}

static char *fuzz_all_resume_command(const char *stop_file) {
  char *out = NULL;
  (void)asprintf(&out, "rm -f %s",
                 stop_file && *stop_file ? stop_file : "build/fuzz/all/stop");
  return out ? out : strdup("rm -f build/fuzz/all/stop");
}

static char *fuzz_all_missing_evidence_state_file_path(const char *dir_path) {
  char *out = NULL;
  (void)asprintf(&out, "%s/run-missing-evidence-state.json",
                 dir_path && *dir_path ? dir_path : "build/fuzz/all");
  return out ? out :
               strdup("build/fuzz/all/run-missing-evidence-state.json");
}

static char *fuzz_all_missing_evidence_state_command(const char *state_file) {
  char *out = NULL;
  (void)asprintf(
      &out,
      "jq {state,event,live,fresh,child_status,child_pid,heartbeat_s,"
      "heartbeat_count,low_priority,nice,load_wait,max_load_pct,space_guard,"
      "min_free_gb,run_lock,hours,threads,profile,json,target_thread_years,"
      "timestamp_utc,pid,stop_file,status_report,progress_report,last_report} %s",
      state_file && *state_file ?
          state_file : "build/fuzz/all/run-missing-evidence-state.json");
  return out ? out :
               strdup("jq {state,event,live,fresh,child_status,child_pid,heartbeat_s,heartbeat_count,low_priority,nice,load_wait,max_load_pct,space_guard,min_free_gb,run_lock,hours,threads,profile,json,target_thread_years,timestamp_utc,pid,stop_file,status_report,progress_report,last_report} build/fuzz/all/run-missing-evidence-state.json");
}

static char *fuzz_all_missing_evidence_state_refresh_command(
    const char *script_command) {
  if (!script_command || !*script_command) return strdup("");
  char out[8192] = {0};
  fuzz_all_env_command(out, sizeof(out),
                       "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                       "NYNTH_RUN_DRY_RUN=1",
                       script_command);
  return strdup(out);
}

static char *fuzz_all_missing_evidence_low_cpu_command(
    const char *script_command) {
  if (!script_command || !*script_command) return strdup("");
  char env[512] = {0};
  snprintf(env, sizeof(env),
           "NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
           "NYNTH_MISSING_EVIDENCE_HOURS=%s "
           "NYNTH_MISSING_EVIDENCE_THREADS=%s",
           NYNTH_LOW_CPU_MISSING_EVIDENCE_HOURS,
           NYNTH_LOW_CPU_MISSING_EVIDENCE_THREADS);
  char out[8192] = {0};
  fuzz_all_env_command(out, sizeof(out), env, script_command);
  return strdup(out);
}

static char *fuzz_all_missing_evidence_stop_file_path(const char *dir_path) {
  char *out = NULL;
  (void)asprintf(&out, "%s/missing-evidence-stop",
                 dir_path && *dir_path ? dir_path : "build/fuzz/all");
  return out ? out : strdup("build/fuzz/all/missing-evidence-stop");
}

static char *fuzz_all_missing_evidence_stop_command(const char *stop_file) {
  char *out = NULL;
  (void)asprintf(
      &out, "touch %s",
      stop_file && *stop_file ?
          stop_file : "build/fuzz/all/missing-evidence-stop");
  return out ? out : strdup("touch build/fuzz/all/missing-evidence-stop");
}

static char *fuzz_all_missing_evidence_resume_command(const char *stop_file) {
  char *out = NULL;
  (void)asprintf(
      &out, "rm -f %s",
      stop_file && *stop_file ?
          stop_file : "build/fuzz/all/missing-evidence-stop");
  return out ? out : strdup("rm -f build/fuzz/all/missing-evidence-stop");
}

static void append_fuzz_all_repeat_progress_refresh_script(
    str_buf_t *sh, const char *dir_rel, const char *target_arg,
    const char *hours_arg, const char *threads_arg, const char *profile_arg) {
  if (!sh) return;
  (void)sb_append(
      sh,
      "  nynth_low_priority ./build/nynth fuzz all progress --strict "
      "--allow-full-pressure-remediation --dir ");
  (void)sb_append_json_str(sh,
                           dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(
      sh,
      " --status \"$repeat_status\" --history \"$history\" "
      "--worklist \"$worklist\" --coverage \"$latest_coverage\" "
      "--plan \"$plan\" --target-thread-years ");
  (void)sb_append(sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(sh, " --hours ");
  (void)sb_append(sh, hours_arg && *hours_arg ? hours_arg : "8");
  (void)sb_append(sh, " --threads ");
  (void)sb_append(sh, threads_arg && *threads_arg ? threads_arg :
                                      NYNTH_DEFAULT_FUZZ_THREADS);
  (void)sb_append(sh, " --profile ");
  (void)sb_append(sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(
      sh, " --json \"$repeat_progress\" --markdown \"$repeat_progress_md\"\n");
}

static void append_fuzz_all_status_progress_refresh_script(
    str_buf_t *sh, const char *dir_rel, const char *target_arg,
    const char *hours_arg, const char *threads_arg, const char *profile_arg) {
  if (!sh) return;
  (void)sb_append(
      sh,
      "nynth_low_priority ./build/nynth fuzz all progress --strict "
      "--allow-full-pressure-remediation --dir ");
  (void)sb_append_json_str(sh,
                           dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(
      sh,
      " --status \"$status\" --history \"$history\" "
      "--worklist \"$worklist\" --coverage \"$latest_coverage\" "
      "--plan \"$plan\" --target-thread-years ");
  (void)sb_append(sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(sh, " --hours ");
  (void)sb_append(sh, hours_arg && *hours_arg ? hours_arg : "8");
  (void)sb_append(sh, " --threads ");
  (void)sb_append(sh, threads_arg && *threads_arg ? threads_arg :
                                      NYNTH_DEFAULT_FUZZ_THREADS);
  (void)sb_append(sh, " --profile ");
  (void)sb_append(sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(sh, " --json \"$progress\" --markdown \"$progress_md\"\n");
}

static void append_fuzz_all_strict_history_coverage_refresh_script(
    str_buf_t *sh, const char *indent, const char *target_arg,
    const char *hours_arg, const char *threads_arg, const char *profile_arg) {
  if (!sh) return;
  if (indent && *indent) (void)sb_append(sh, indent);
  (void)sb_append(
      sh,
      "nynth_low_priority ./build/nynth fuzz all coverage --strict --history \"$history\" "
      "--target-thread-years ");
  (void)sb_append(sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(sh, " --hours ");
  (void)sb_append(sh, hours_arg && *hours_arg ? hours_arg : "8");
  (void)sb_append(sh, " --threads ");
  (void)sb_append(sh, threads_arg && *threads_arg ? threads_arg :
                                      NYNTH_DEFAULT_FUZZ_THREADS);
  (void)sb_append(sh, " --profile ");
  (void)sb_append(sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(
      sh, " --json \"$latest_coverage\" --markdown \"$latest_coverage_md\"\n");
}

static char *fuzz_all_preview_command(const char *next_command) {
  if (!next_command || !*next_command) return strdup("");
  char *out = NULL;
  (void)asprintf(&out,
                 "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                 "NYNTH_RUN_DRY_RUN=1 NYNTH_RUN_REPEAT=target "
                 "nice -n 10 %s",
                 next_command);
  return out ? out : strdup("");
}

static char *fuzz_all_coverage_next_preview_command(
    const char *coverage_command, const char *dir_arg,
    const char *target_arg, const char *hours_arg,
    const char *threads_arg, const char *profile_arg) {
  if (!coverage_command || !strstr(coverage_command, "fuzz all run"))
    return strdup("");
  const char *dir = dir_arg && *dir_arg ? dir_arg : "build/fuzz/all";
  const char *target = target_arg && *target_arg ? target_arg : "10";
  const char *hours = hours_arg && *hours_arg ? hours_arg : "8";
  const char *threads = threads_arg && *threads_arg ?
                            threads_arg : NYNTH_DEFAULT_FUZZ_THREADS;
  const char *profile = profile_arg && *profile_arg ? profile_arg : "insane";
  const char *nytrix_flag =
      strstr(coverage_command, "--allow-nytrix") ? " --allow-nytrix" : "";
  const char *only_lane_flag =
      strstr(coverage_command, "--only-lane afl") ? " --only-lane afl" : "";
  char *out = NULL;
  (void)asprintf(
      &out,
      "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
      "./build/nynth fuzz all "
      "preflight --allow-dirty-nytrix-baseline "
      "--work-dir build/cache/scratch/fuzz_all_preflight/"
      "missing-evidence-preview --dir %s --target-thread-years %s "
      "--hours %s --threads %s --profile %s%s%s --json %s/"
      "preflight-missing-evidence.json",
      dir, target, hours, threads, profile, only_lane_flag, nytrix_flag, dir);
  return out ? out : strdup("");
}

static bool write_fuzz_all_missing_evidence_script(
    const char *root, const char *script_path, const char *dir_path,
    const char *history_path, const char *worklist_path,
    const char *coverage_path, const char *plan_path,
    const char *status_json_path, const char *status_md_path,
    const char *coverage_command, const char *target_arg,
    const char *hours_arg, const char *threads_arg, const char *profile_arg) {
  if (!script_path || !*script_path || !coverage_command || !*coverage_command)
    return true;
  char *dir_rel =
      rel_path_dup(root ? root : "", dir_path ? dir_path : "build/fuzz/all");
  char *history_rel = rel_path_dup(
      root ? root : "", history_path ? history_path : "build/fuzz/all/history.json");
  char *worklist_rel = rel_path_dup(
      root ? root : "", worklist_path ? worklist_path : "build/fuzz/all/worklist.json");
  char *coverage_rel = rel_path_dup(
      root ? root : "", coverage_path ? coverage_path : "build/fuzz/all/coverage.json");
  char *plan_rel =
      rel_path_dup(root ? root : "", plan_path ? plan_path : "build/fuzz/all/plan.json");
  char *status_rel = rel_path_dup(
      root ? root : "", status_json_path ? status_json_path : "build/fuzz/all/status.json");
  char *status_md_rel = rel_path_dup(
      root ? root : "", status_md_path ? status_md_path : "build/fuzz/all/status.md");
  char *history_md =
      path_with_suffix_ext(history_rel && *history_rel ? history_rel :
                                                     "build/fuzz/all/history.json",
                           "", ".md");
  char *worklist_md =
      path_with_suffix_ext(worklist_rel && *worklist_rel ? worklist_rel :
                                                       "build/fuzz/all/worklist.json",
                           "", ".md");
  char *coverage_md =
      path_with_suffix_ext(coverage_rel && *coverage_rel ? coverage_rel :
                                                       "build/fuzz/all/coverage.json",
                           "", ".md");
  char *plan_md =
      path_with_suffix_ext(plan_rel && *plan_rel ? plan_rel :
                                                 "build/fuzz/all/plan.json",
                           "", ".md");
  char *missing_default_json = NULL;
  (void)asprintf(&missing_default_json, "%s/with-afl.json",
                 dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  char *script_dir = NULL;
  if (script_path) {
    script_dir = strdup(script_path);
    char *slash = script_dir ? strrchr(script_dir, '/') : NULL;
    if (slash) {
      *slash = '\0';
      if (*script_dir) ny_ensure_dir_recursive(script_dir);
    }
  }
  if (dir_path && *dir_path) ny_ensure_dir_recursive(dir_path);
  str_buf_t sh = {0};
  (void)sb_append(&sh, "#!/usr/bin/env bash\n");
  (void)sb_append(&sh, "set -euo pipefail\n");
  append_repo_cache_env_script(&sh, root ? root : ".");
  append_low_priority_shell_helper(&sh);
  append_load_wait_shell_helper(&sh);
  append_space_guard_shell_helper(&sh);
  append_campaign_lock_shell_helper(&sh);
  (void)sb_append(&sh, "mkdir -p ");
  (void)sb_append_json_str(&sh,
                           dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "history=");
  (void)sb_append_json_str(&sh, history_rel && *history_rel ?
                                    history_rel : "build/fuzz/all/history.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "history_md=");
  (void)sb_append_json_str(&sh, history_md ? history_md :
                                             "build/fuzz/all/history.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "latest_coverage=");
  (void)sb_append_json_str(&sh, coverage_rel && *coverage_rel ?
                                    coverage_rel : "build/fuzz/all/coverage.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "latest_coverage_md=");
  (void)sb_append_json_str(&sh, coverage_md ? coverage_md :
                                              "build/fuzz/all/coverage.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "worklist=");
  (void)sb_append_json_str(&sh, worklist_rel && *worklist_rel ?
                                    worklist_rel : "build/fuzz/all/worklist.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "worklist_md=");
  (void)sb_append_json_str(&sh, worklist_md ? worklist_md :
                                               "build/fuzz/all/worklist.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "plan=");
  (void)sb_append_json_str(&sh, plan_rel && *plan_rel ?
                                    plan_rel : "build/fuzz/all/plan.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "plan_md=");
  (void)sb_append_json_str(&sh, plan_md ? plan_md : "build/fuzz/all/plan.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "status=");
  (void)sb_append_json_str(&sh, status_rel && *status_rel ?
                                    status_rel : "build/fuzz/all/status.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "status_md=");
  (void)sb_append_json_str(&sh, status_md_rel && *status_md_rel ?
                                    status_md_rel : "build/fuzz/all/status.md");
  (void)sb_append(&sh, "\n");
  (void)sb_appendf(&sh, "progress=\"%s/progress.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "progress_md=\"%s/progress.md\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "old_paths=\"%s/old-paths.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "old_paths_md=\"%s/old-paths.md\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh,
                   "NYNTH_MISSING_EVIDENCE_STATE_FILE=\"${NYNTH_MISSING_EVIDENCE_STATE_FILE:-%s/run-missing-evidence-state.json}\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh,
                   "NYNTH_MISSING_EVIDENCE_STOP_FILE=\"${NYNTH_MISSING_EVIDENCE_STOP_FILE:-%s/missing-evidence-stop}\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh,
                  "NYNTH_MISSING_EVIDENCE_HEARTBEAT_S=\"${NYNTH_MISSING_EVIDENCE_HEARTBEAT_S:-300}\"\n"
                  "nynth_missing_dir=");
  (void)sb_append_json_str(&sh,
                           dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_missing_default_json=");
  (void)sb_append_json_str(&sh,
                           missing_default_json && *missing_default_json ?
                               missing_default_json :
                               "build/fuzz/all/with-afl.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_missing_default_command=");
  (void)sb_append_json_str(&sh, coverage_command);
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_missing_default_hours=");
  (void)sb_append_json_str(&sh, hours_arg && *hours_arg ? hours_arg : "8");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_missing_default_threads=");
  (void)sb_append_json_str(&sh, threads_arg && *threads_arg ?
                                    threads_arg : NYNTH_DEFAULT_FUZZ_THREADS);
  (void)sb_append(&sh,
                  "\n"
                  "nynth_missing_default_profile=");
  (void)sb_append_json_str(&sh, profile_arg && *profile_arg ?
                                    profile_arg : "insane");
  (void)sb_append(&sh,
                  "\n"
                  "nynth_missing_default_target_thread_years=");
  (void)sb_append_json_str(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh,
                  "\n"
                  "nynth_missing_hours=\"${NYNTH_MISSING_EVIDENCE_HOURS:-$nynth_missing_default_hours}\"\n"
                  "nynth_missing_threads=\"${NYNTH_MISSING_EVIDENCE_THREADS:-$nynth_missing_default_threads}\"\n"
                  "nynth_missing_profile=\"${NYNTH_MISSING_EVIDENCE_PROFILE:-$nynth_missing_default_profile}\"\n"
                  "nynth_missing_run_json=\"${NYNTH_MISSING_EVIDENCE_JSON:-$nynth_missing_default_json}\"\n"
                  "nynth_missing_target_thread_years=\"${NYNTH_MISSING_EVIDENCE_TARGET_THREAD_YEARS:-$nynth_missing_default_target_thread_years}\"\n"
                  "nynth_missing_json_escape() { printf '%s' \"$1\" | sed 's/\\\\/\\\\\\\\/g; s/\"/\\\\\"/g'; }\n"
                  "nynth_missing_json_uint() { case \"${1:-}\" in ''|*[!0-9]*) printf '0';; *) printf '%s' \"$1\";; esac; }\n"
                  "nynth_missing_json_bool_enabled() { case \"${1:-1}\" in 0|false|False|FALSE|no|No|NO) printf 'false';; *) printf 'true';; esac; }\n"
                  "nynth_missing_command_string() {\n"
                  "  local out='' arg quoted\n"
                  "  for arg in \"$@\"; do\n"
                  "    printf -v quoted '%q' \"$arg\"\n"
                  "    out=\"${out:+$out }$quoted\"\n"
                  "  done\n"
                  "  printf '%s' \"$out\"\n"
                  "}\n"
                  "nynth_missing_command=(./build/nynth fuzz all run)\n");
  if (strstr(coverage_command, "--only-lane afl"))
    (void)sb_append(&sh, "nynth_missing_command+=(--only-lane afl)\n");
  (void)sb_append(&sh,
                  "nynth_missing_command+=(--profile \"$nynth_missing_profile\" --hours \"$nynth_missing_hours\" --threads \"$nynth_missing_threads\")\n");
  if (strstr(coverage_command, "--target-thread-years"))
    (void)sb_append(&sh, "nynth_missing_command+=(--target-thread-years \"$nynth_missing_target_thread_years\")\n");
  (void)sb_append(&sh, "nynth_missing_command+=(--dir \"$nynth_missing_dir\")\n");
  if (strstr(coverage_command, "--allow-nytrix"))
    (void)sb_append(&sh, "nynth_missing_command+=(--allow-nytrix)\n");
  (void)sb_append(&sh,
                  "nynth_missing_command+=(--json \"$nynth_missing_run_json\")\n"
                  "nynth_missing_command_text=\"$(nynth_missing_command_string \"${nynth_missing_command[@]}\")\"\n"
                  "nynth_missing_state_child_pid=0\n"
                  "nynth_missing_state_heartbeat_pid=0\n"
                  "nynth_missing_state_heartbeat_count=0\n"
                  "nynth_missing_state_terminal=0\n"
                  "nynth_missing_write_state() {\n"
                  "  local phase=\"${1:-unknown}\" event=\"${2:-}\" ts state_dir child_status live\n"
                  "  child_status=\"none\"\n"
                  "  case \"${nynth_missing_state_child_pid:-0}\" in ''|*[!0-9]*|0) ;; *) if kill -0 \"$nynth_missing_state_child_pid\" 2>/dev/null; then child_status=\"alive\"; else child_status=\"dead\"; fi;; esac\n"
                  "  case \"$phase\" in running|locked|refreshing|cooldown) live=\"true\";; *) live=\"false\";; esac\n"
                  "  ts=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date +%Y-%m-%dT%H:%M:%S)\n"
                  "  state_dir=$(dirname \"$NYNTH_MISSING_EVIDENCE_STATE_FILE\" 2>/dev/null || printf '.')\n"
                  "  mkdir -p \"$state_dir\" 2>/dev/null || true\n"
                  "  {\n"
                  "    printf '{'\n"
                  "    printf '\"state\":\"%s\",' \"$(nynth_missing_json_escape \"$phase\")\"\n"
                  "    printf '\"phase\":\"%s\",' \"$(nynth_missing_json_escape \"$phase\")\"\n"
                  "    printf '\"event\":\"%s\",' \"$(nynth_missing_json_escape \"$event\")\"\n"
                  "    printf '\"timestamp_utc\":\"%s\",' \"$(nynth_missing_json_escape \"$ts\")\"\n"
                  "    printf '\"live\":%s,' \"$live\"\n"
                  "    printf '\"fresh\":true,'\n"
                  "    printf '\"child_status\":\"%s\",' \"$(nynth_missing_json_escape \"$child_status\")\"\n"
                  "    printf '\"pid\":%s,' \"$(nynth_missing_json_uint \"$$\")\"\n"
                  "    printf '\"child_pid\":%s,' \"$(nynth_missing_json_uint \"${nynth_missing_state_child_pid:-0}\")\"\n"
                  "    printf '\"heartbeat_s\":%s,' \"$(nynth_missing_json_uint \"${NYNTH_MISSING_EVIDENCE_HEARTBEAT_S:-0}\")\"\n"
                  "    printf '\"heartbeat_count\":%s,' \"$(nynth_missing_json_uint \"${nynth_missing_state_heartbeat_count:-0}\")\"\n"
                  "    printf '\"low_priority\":%s,' \"$(nynth_missing_json_bool_enabled \"${NYNTH_LOW_PRIORITY:-1}\")\"\n"
                  "    printf '\"nice\":%s,' \"$(nynth_missing_json_uint \"${NYNTH_RUN_NICE:-10}\")\"\n"
                  "    printf '\"load_wait\":%s,' \"$(nynth_missing_json_bool_enabled \"${NYNTH_LOAD_WAIT:-1}\")\"\n"
                  "    printf '\"max_load_pct\":%s,' \"$(nynth_missing_json_uint \"${NYNTH_MAX_LOAD_PCT:-75}\")\"\n"
                  "    printf '\"space_guard\":%s,' \"$(nynth_missing_json_bool_enabled \"${NYNTH_SPACE_GUARD:-1}\")\"\n"
                  "    printf '\"min_free_gb\":%s,' \"$(nynth_missing_json_uint \"${NYNTH_MIN_FREE_GB:-20}\")\"\n"
                  "    printf '\"run_lock\":%s,' \"$(nynth_missing_json_bool_enabled \"${NYNTH_RUN_LOCK:-1}\")\"\n"
                  "    printf '\"hours\":\"%s\",' \"$(nynth_missing_json_escape \"$nynth_missing_hours\")\"\n"
                  "    printf '\"threads\":\"%s\",' \"$(nynth_missing_json_escape \"$nynth_missing_threads\")\"\n"
                  "    printf '\"profile\":\"%s\",' \"$(nynth_missing_json_escape \"$nynth_missing_profile\")\"\n"
                  "    printf '\"json\":\"%s\",' \"$(nynth_missing_json_escape \"$nynth_missing_run_json\")\"\n"
                  "    printf '\"target_thread_years\":\"%s\",' \"$(nynth_missing_json_escape \"$nynth_missing_target_thread_years\")\"\n"
                  "    printf '\"command\":\"%s\",' \"$(nynth_missing_json_escape \"$nynth_missing_command_text\")\"\n"
                  "    printf '\"default_command\":\"%s\",' \"$(nynth_missing_json_escape \"$nynth_missing_default_command\")\"\n"
                  "    printf '\"stop_file\":\"%s\",' \"$(nynth_missing_json_escape \"${NYNTH_MISSING_EVIDENCE_STOP_FILE:-}\")\"\n"
                  "    printf '\"status_report\":\"%s\",' \"$(nynth_missing_json_escape \"${status:-}\")\"\n"
                  "    printf '\"progress_report\":\"%s\",' \"$(nynth_missing_json_escape \"${progress:-}\")\"\n"
                  "    printf '\"last_report\":\"%s\"' \"$(nynth_missing_json_escape \"$nynth_missing_run_json\")\"\n"
                  "    printf '}\\n'\n"
                  "  } > \"$NYNTH_MISSING_EVIDENCE_STATE_FILE\" 2>/dev/null || true\n"
                  "}\n"
                  "nynth_missing_write_terminal_state() { nynth_missing_state_terminal=1; nynth_missing_write_state \"$1\" \"${2:-}\"; }\n"
                  "nynth_missing_cleanup_child() {\n"
                  "  case \"${nynth_missing_state_child_pid:-0}\" in ''|*[!0-9]*|0) ;; *) kill -TERM \"$nynth_missing_state_child_pid\" 2>/dev/null || true; wait \"$nynth_missing_state_child_pid\" 2>/dev/null || true;; esac\n"
                  "  case \"${nynth_missing_state_heartbeat_pid:-0}\" in ''|*[!0-9]*|0) ;; *) kill -TERM \"$nynth_missing_state_heartbeat_pid\" 2>/dev/null || true; wait \"$nynth_missing_state_heartbeat_pid\" 2>/dev/null || true;; esac\n"
                  "}\n"
                  "nynth_missing_exit_trap() {\n"
                  "  local rc=\"$?\"\n"
                  "  if [ \"$rc\" -ne 0 ]; then nynth_missing_write_terminal_state failed \"exit-$rc\"; elif [ \"${nynth_missing_state_terminal:-0}\" = \"0\" ]; then nynth_missing_write_terminal_state finished exit-0; fi\n"
                  "  nynth_release_campaign_lock\n"
                  "  return \"$rc\"\n"
                  "}\n"
                  "nynth_missing_signal_trap() {\n"
                  "  nynth_missing_write_terminal_state interrupted signal\n"
                  "  nynth_missing_cleanup_child\n"
                  "  nynth_release_campaign_lock\n"
                  "  exit 130\n"
                  "}\n"
                  "nynth_missing_install_traps() {\n"
                  "  trap nynth_missing_exit_trap EXIT\n"
                  "  trap nynth_missing_signal_trap INT TERM\n"
                  "}\n"
                  "nynth_missing_run_with_heartbeat() {\n"
                  "  \"$@\" &\n"
                  "  nynth_missing_state_child_pid=\"$!\"\n"
                  "  nynth_missing_state_heartbeat_count=0\n"
                  "  nynth_missing_write_state running child-start\n"
                  "  nynth_missing_state_heartbeat_pid=0\n"
                  "  if [ \"${NYNTH_MISSING_EVIDENCE_HEARTBEAT_S:-0}\" -gt 0 ]; then\n"
                  "    (\n"
                  "      nynth_missing_state_child_pid=\"$nynth_missing_state_child_pid\"\n"
                  "      nynth_missing_state_heartbeat_count=0\n"
                  "      while kill -0 \"$nynth_missing_state_child_pid\" 2>/dev/null; do\n"
                  "        sleep \"$NYNTH_MISSING_EVIDENCE_HEARTBEAT_S\" || break\n"
                  "        if kill -0 \"$nynth_missing_state_child_pid\" 2>/dev/null; then\n"
                  "          nynth_missing_state_heartbeat_count=$((nynth_missing_state_heartbeat_count + 1))\n"
                  "          nynth_missing_write_state running heartbeat\n"
                  "        fi\n"
                  "      done\n"
                  "    ) &\n"
                  "    nynth_missing_state_heartbeat_pid=\"$!\"\n"
                  "  fi\n"
                  "  set +e\n"
                  "  wait \"$nynth_missing_state_child_pid\"\n"
                  "  local rc=\"$?\"\n"
                  "  set -e\n"
                  "  case \"${nynth_missing_state_heartbeat_pid:-0}\" in ''|*[!0-9]*|0) ;; *) kill -TERM \"$nynth_missing_state_heartbeat_pid\" 2>/dev/null || true; wait \"$nynth_missing_state_heartbeat_pid\" 2>/dev/null || true;; esac\n"
                  "  nynth_missing_state_heartbeat_pid=0\n"
                  "  nynth_missing_state_child_pid=0\n"
                  "  nynth_missing_write_state refreshing \"child-exit-$rc\"\n"
                  "  return \"$rc\"\n"
                  "}\n");
  (void)sb_append(&sh, "case \"$NYNTH_MISSING_EVIDENCE_HEARTBEAT_S\" in ''|*[!0-9]*) echo \"nynth missing-evidence guard: NYNTH_MISSING_EVIDENCE_HEARTBEAT_S must be a non-negative integer\"; nynth_missing_write_terminal_state failed invalid-heartbeat; exit 64;; esac\n");
  (void)sb_append(&sh, "if [ -z \"$nynth_missing_hours\" ] || [ -z \"$nynth_missing_threads\" ] || [ -z \"$nynth_missing_profile\" ] || [ -z \"$nynth_missing_run_json\" ]; then\n");
  (void)sb_append(&sh, "  echo \"nynth missing-evidence guard: NYNTH_MISSING_EVIDENCE_HOURS, THREADS, PROFILE, and JSON must be non-empty\"\n");
  (void)sb_append(&sh, "  nynth_missing_write_terminal_state failed invalid-budget\n");
  (void)sb_append(&sh, "  exit 64\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "if [ \"${NYNTH_RUN_DRY_RUN:-0}\" != \"0\" ]; then\n");
  (void)sb_append(&sh, "  echo \"nynth missing-evidence dry-run: budget hours=$nynth_missing_hours threads=$nynth_missing_threads profile=$nynth_missing_profile json=$nynth_missing_run_json\"\n");
  (void)sb_append(&sh, "  echo \"nynth missing-evidence dry-run: command=$nynth_missing_command_text\"\n");
  (void)sb_append(&sh, "  echo \"nynth missing-evidence dry-run: default_command=$nynth_missing_default_command\"\n");
  (void)sb_append(&sh, "  echo \"nynth missing-evidence dry-run: guards low_priority=${NYNTH_LOW_PRIORITY:-1} nice=${NYNTH_RUN_NICE:-10} load_wait=${NYNTH_LOAD_WAIT:-1} max_load_pct=${NYNTH_MAX_LOAD_PCT:-75} space_guard=${NYNTH_SPACE_GUARD:-1} min_free_gb=${NYNTH_MIN_FREE_GB:-20} run_lock=${NYNTH_RUN_LOCK:-1}\"\n");
  (void)sb_append(&sh, "  echo \"nynth missing-evidence dry-run: state=$NYNTH_MISSING_EVIDENCE_STATE_FILE stop=$NYNTH_MISSING_EVIDENCE_STOP_FILE heartbeat_s=$NYNTH_MISSING_EVIDENCE_HEARTBEAT_S\"\n");
  (void)sb_append(&sh, "  nynth_missing_write_terminal_state dry-run preview\n");
  (void)sb_append(&sh, "  exit 0\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "if [ -n \"$NYNTH_MISSING_EVIDENCE_STOP_FILE\" ] && [ -e \"$NYNTH_MISSING_EVIDENCE_STOP_FILE\" ]; then\n");
  (void)sb_append(&sh, "  nynth_missing_write_terminal_state stopped before-run\n");
  (void)sb_append(&sh, "  echo \"nynth missing-evidence stop requested before run: $NYNTH_MISSING_EVIDENCE_STOP_FILE\"\n");
  (void)sb_append(&sh, "  exit 0\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "nynth_require_free_space\n");
  (void)sb_append(&sh, "nynth_wait_for_load\n");
  (void)sb_append(&sh, "nynth_acquire_campaign_lock ");
  (void)sb_append_json_str(&sh,
                           dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_missing_install_traps\n");
  (void)sb_append(&sh, "nynth_missing_write_state locked acquired-lock\n");
  (void)sb_append(&sh, "echo \"nynth missing-evidence run: $nynth_missing_command_text\"\n");
  (void)sb_append(&sh, "nynth_missing_run_with_heartbeat nynth_low_priority \"${nynth_missing_command[@]}\"\n");
  (void)sb_append(&sh, "nynth_missing_write_state refreshing run-complete\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all history --dir ");
  (void)sb_append_json_str(&sh,
                           dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --json \"$history\" --markdown \"$history_md\"\n");
  append_fuzz_all_strict_history_coverage_refresh_script(
      &sh, "", target_arg, hours_arg, threads_arg, profile_arg);
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all worklist --history \"$history\" --json \"$worklist\" --markdown \"$worklist_md\"\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all plan --dir ");
  (void)sb_append_json_str(&sh,
                           dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, hours_arg && *hours_arg ? hours_arg : "8");
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, threads_arg && *threads_arg ?
                           threads_arg : NYNTH_DEFAULT_FUZZ_THREADS);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(&sh, " --json \"$plan\" --markdown \"$plan_md\"\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json \"$old_paths\" --markdown \"$old_paths_md\"\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all status --strict --allow-full-pressure-remediation --dir ");
  (void)sb_append_json_str(&sh,
                           dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --plan \"$plan\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, hours_arg && *hours_arg ? hours_arg : "8");
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, threads_arg && *threads_arg ?
                           threads_arg : NYNTH_DEFAULT_FUZZ_THREADS);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(&sh, " --json \"$status\" --markdown \"$status_md\"\n");
  append_fuzz_all_status_progress_refresh_script(
      &sh, dir_rel, target_arg, hours_arg, threads_arg, profile_arg);
  (void)sb_append(&sh, "nynth_missing_write_terminal_state finished refreshed\n");
  bool ok = sh.data && write_file_text(script_path, sh.data);
  if (ok) (void)chmod(script_path, 0755);
  free(dir_rel);
  free(history_rel);
  free(worklist_rel);
  free(coverage_rel);
  free(plan_rel);
  free(status_rel);
  free(status_md_rel);
  free(history_md);
  free(worklist_md);
  free(coverage_md);
  free(plan_md);
  free(missing_default_json);
  free(script_dir);
  free(sh.data);
  return ok;
}

static void fuzz_all_normalize_handoff_command(char *out, size_t out_sz,
                                               const char *command) {
  if (!out || out_sz == 0) return;
  out[0] = '\0';
  if (!command || !*command) return;
  bool has_space = false;
  for (const char *p = command; *p; ++p) {
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      has_space = true;
      break;
    }
  }
  if (!has_space && strchr(command, '/') && !path_is_absolute(command) &&
      strncmp(command, "./", 2) != 0) {
    snprintf(out, out_sz, "./%s", command);
  } else {
    snprintf(out, out_sz, "%s", command);
  }
}

static char *fuzz_all_state_file_path(const char *dir_path) {
  char *out = NULL;
  (void)asprintf(&out, "%s/run-next-state.json",
                 dir_path && *dir_path ? dir_path : "build/fuzz/all");
  return out ? out : strdup("build/fuzz/all/run-next-state.json");
}

static char *fuzz_all_state_command(const char *state_file) {
  return fuzz_all_state_compact_jq_command(
      state_file && *state_file ? state_file :
          "build/fuzz/all/run-next-state.json");
}

static bool path_leaf_matches(const char *path, const char *leaf) {
  if (!path || !leaf) return false;
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  return strcmp(base, leaf) == 0;
}

static char *fuzz_all_status_progress_output_path(const char *dir_path,
                                                  const char *status_json_path,
                                                  bool markdown) {
  const char *leaf = markdown ? "progress.md" : "progress.json";
  if (path_leaf_matches(status_json_path, "repeat-status.json"))
    leaf = markdown ? "repeat-progress.md" : "repeat-progress.json";
  char *out = NULL;
  (void)asprintf(&out, "%s/%s",
                 dir_path && *dir_path ? dir_path : "build/fuzz/all",
                 leaf);
  return out ? out : strdup(markdown ? "build/fuzz/all/progress.md" :
                                       "build/fuzz/all/progress.json");
}

struct fuzz_all_run_state_summary_t {
  char phase[64];
  char event[64];
  char timestamp_utc[64];
  char last_report[4096];
  bool readable;
  bool fresh;
  bool child_alive;
  double age_seconds;
  double cycle;
  double cycles;
  double heartbeat_s;
  double heartbeat_count;
  double child_pid;
  bool low_priority;
  bool load_wait;
  bool space_guard;
  bool run_lock;
  double nice;
  double max_load_pct;
  double min_free_gb;
  char threads[64];
  bool dry_run_exceeds_max;
  double dry_run_wall_hours;
  double dry_run_wall_days;
  double dry_run_thread_years;
  double dry_run_campaign_gain_percent;
  double dry_run_target_percent_per_run;
  double dry_run_thread_years_per_run;
  char canonical_status_report[4096];
  char canonical_progress_report[4096];
};

static void fuzz_all_run_state_init(fuzz_all_run_state_summary_t *st) {
  if (!st) return;
  memset(st, 0, sizeof(*st));
  st->age_seconds = -1.0;
  st->nice = -1.0;
  st->max_load_pct = -1.0;
  st->min_free_gb = -1.0;
  st->dry_run_wall_hours = -1.0;
  st->dry_run_wall_days = -1.0;
  st->dry_run_thread_years = -1.0;
  st->dry_run_campaign_gain_percent = -1.0;
  st->dry_run_target_percent_per_run = -1.0;
  st->dry_run_thread_years_per_run = -1.0;
}

static bool fuzz_all_state_phase_live(const char *phase) {
  return phase && (!strcmp(phase, "running") ||
                   !strcmp(phase, "locked") ||
                   !strcmp(phase, "refreshing") ||
                   !strcmp(phase, "cooldown"));
}

static bool fuzz_all_pid_alive(double pid_value) {
  if (pid_value <= 0.0) return false;
  pid_t pid = (pid_t)pid_value;
  if ((double)pid != pid_value || pid <= 0) return false;
  errno = 0;
  int rc = kill(pid, 0);
  return rc == 0 || errno == EPERM;
}

static const char *fuzz_all_state_child_status(double pid_value,
                                               bool child_alive) {
  if (pid_value <= 0.0) return "none";
  return child_alive ? "alive" : "dead";
}

static const char *fuzz_all_state_label_values(bool readable,
                                               const char *phase) {
  if (!readable) return "missing";
  if (phase && *phase) return phase;
  return "unknown";
}

static const char *fuzz_all_state_stale_reason_values(bool readable,
                                                      bool fresh,
                                                      const char *phase,
                                                      double age_seconds,
                                                      double heartbeat_s,
                                                      double child_pid,
                                                      bool child_alive) {
  if (fresh) return "none";
  if (!readable) return "missing";
  if (age_seconds < 0.0) return "unknown-age";
  double stale_after = fuzz_all_state_stale_after_seconds(heartbeat_s);
  if (age_seconds > stale_after)
    return fuzz_all_state_phase_live(phase) ? "old-heartbeat" : "old-state";
  if (fuzz_all_state_phase_live(phase)) {
    if (child_pid > 0.0 && !child_alive) return "dead-child";
  }
  return "stale";
}

static double fuzz_all_state_stale_after_seconds(double heartbeat_s) {
  double stale_after = heartbeat_s > 0.0 ? heartbeat_s * 2.0 + 30.0 : 3600.0;
  return stale_after < 60.0 ? 60.0 : stale_after;
}

static bool fuzz_all_state_fresh_values(bool readable, const char *phase,
                                        double age_seconds,
                                        double heartbeat_s,
                                        double child_pid,
                                        bool child_alive) {
  if (!readable || age_seconds < 0.0) return false;
  if (age_seconds > fuzz_all_state_stale_after_seconds(heartbeat_s))
    return false;
  if (fuzz_all_state_phase_live(phase) && child_pid > 0.0 && !child_alive)
    return false;
  return true;
}

static void fuzz_all_load_run_state(const char *root, const char *state_file,
                                    fuzz_all_run_state_summary_t *out) {
  fuzz_all_run_state_init(out);
  if (!out || !state_file || !*state_file) return;
  char abs[4096] = {0};
  if (path_is_absolute(state_file)) {
    snprintf(abs, sizeof(abs), "%s", state_file);
  } else if (root && *root) {
    (void)path_join(abs, sizeof(abs), root, state_file);
  } else {
    snprintf(abs, sizeof(abs), "%s", state_file);
  }
  file_buf_t f = {0};
  if (!abs[0] || !read_file(abs, &f) || !f.data) return;
  out->readable = true;
  char *s = json_string_or_empty(f.data, "phase");
  snprintf(out->phase, sizeof(out->phase), "%s", s ? s : "");
  free(s);
  s = json_string_or_empty(f.data, "event");
  snprintf(out->event, sizeof(out->event), "%s", s ? s : "");
  free(s);
  s = json_string_or_empty(f.data, "timestamp_utc");
  snprintf(out->timestamp_utc, sizeof(out->timestamp_utc), "%s", s ? s : "");
  free(s);
  s = json_string_or_empty(f.data, "last_report");
  snprintf(out->last_report, sizeof(out->last_report), "%s", s ? s : "");
  free(s);
  const char *end = f.data + f.len;
  (void)json_number_range(f.data, end, "cycle", &out->cycle);
  (void)json_number_range(f.data, end, "cycles", &out->cycles);
  (void)json_number_range(f.data, end, "heartbeat_s", &out->heartbeat_s);
  (void)json_number_range(f.data, end, "heartbeat_count",
                          &out->heartbeat_count);
  (void)json_number_range(f.data, end, "child_pid", &out->child_pid);
  out->low_priority = json_bool_range(f.data, end, "low_priority", false);
  out->load_wait = json_bool_range(f.data, end, "load_wait", false);
  out->space_guard = json_bool_range(f.data, end, "space_guard", false);
  out->run_lock = json_bool_range(f.data, end, "run_lock", false);
  (void)json_number_range(f.data, end, "nice", &out->nice);
  (void)json_number_range(f.data, end, "max_load_pct",
                          &out->max_load_pct);
  (void)json_number_range(f.data, end, "min_free_gb", &out->min_free_gb);
  s = json_string_or_empty(f.data, "threads");
  snprintf(out->threads, sizeof(out->threads), "%s", s ? s : "");
  free(s);
  out->dry_run_exceeds_max =
      json_bool_range(f.data, end, "dry_run_exceeds_max", false);
  (void)json_number_range(f.data, end, "dry_run_wall_hours",
                          &out->dry_run_wall_hours);
  (void)json_number_range(f.data, end, "dry_run_wall_days",
                          &out->dry_run_wall_days);
  (void)json_number_range(f.data, end, "dry_run_thread_years",
                          &out->dry_run_thread_years);
  (void)json_number_range(f.data, end, "dry_run_campaign_gain_percent",
                          &out->dry_run_campaign_gain_percent);
  (void)json_number_range(f.data, end, "dry_run_target_percent_per_run",
                          &out->dry_run_target_percent_per_run);
  (void)json_number_range(f.data, end, "dry_run_thread_years_per_run",
                          &out->dry_run_thread_years_per_run);
  s = json_string_or_empty(f.data, "canonical_status_report");
  snprintf(out->canonical_status_report, sizeof(out->canonical_status_report),
           "%s", s ? s : "");
  free(s);
  s = json_string_or_empty(f.data, "canonical_progress_report");
  snprintf(out->canonical_progress_report,
           sizeof(out->canonical_progress_report), "%s", s ? s : "");
  free(s);
  out->child_alive = fuzz_all_pid_alive(out->child_pid);
  struct stat st;
  if (stat(abs, &st) == 0) {
    out->age_seconds = difftime(time(NULL), st.st_mtime);
    if (out->age_seconds < 0.0) out->age_seconds = 0.0;
  }
  out->fresh = fuzz_all_state_fresh_values(out->readable, out->phase,
                                           out->age_seconds, out->heartbeat_s,
                                           out->child_pid, out->child_alive);
  free(f.data);
}

static void fuzz_all_status_set_run_state(fuzz_all_status_summary_t *s,
                                          const fuzz_all_run_state_summary_t *st) {
  if (!s || !st) return;
  s->state_readable = st->readable;
  s->state_fresh = st->fresh;
  s->state_child_alive = st->child_alive;
  s->state_age_seconds = st->age_seconds;
  s->state_cycle = st->cycle;
  s->state_cycles = st->cycles;
  s->state_heartbeat_s = st->heartbeat_s;
  s->state_heartbeat_count = st->heartbeat_count;
  s->state_child_pid = st->child_pid;
  snprintf(s->state_phase, sizeof(s->state_phase), "%s", st->phase);
  snprintf(s->state_event, sizeof(s->state_event), "%s", st->event);
  snprintf(s->state_timestamp_utc, sizeof(s->state_timestamp_utc), "%s",
           st->timestamp_utc);
  snprintf(s->state_last_report, sizeof(s->state_last_report), "%s",
           st->last_report);
}

static void fuzz_all_run_state_from_report(const char *json,
                                           fuzz_all_run_state_summary_t *st) {
  fuzz_all_run_state_init(st);
  if (!json || !*json || !st) return;
  bool b = false;
  if (summary_bool_from_report(json, "state_readable", &b)) st->readable = b;
  if (summary_bool_from_report(json, "state_fresh", &b)) st->fresh = b;
  if (summary_bool_from_report(json, "state_child_alive", &b))
    st->child_alive = b;
  (void)summary_number_from_report(json, "state_age_seconds",
                                   &st->age_seconds);
  (void)summary_number_from_report(json, "state_cycle", &st->cycle);
  (void)summary_number_from_report(json, "state_cycles", &st->cycles);
  (void)summary_number_from_report(json, "state_heartbeat_s",
                                   &st->heartbeat_s);
  (void)summary_number_from_report(json, "state_heartbeat_count",
                                   &st->heartbeat_count);
  (void)summary_number_from_report(json, "state_child_pid", &st->child_pid);
  if (summary_bool_from_report(json, "state_handoff_low_priority", &b))
    st->low_priority = b;
  if (summary_bool_from_report(json, "state_handoff_load_wait", &b))
    st->load_wait = b;
  if (summary_bool_from_report(json, "state_handoff_space_guard", &b))
    st->space_guard = b;
  if (summary_bool_from_report(json, "state_handoff_run_lock", &b))
    st->run_lock = b;
  (void)summary_number_from_report(json, "state_handoff_nice", &st->nice);
  (void)summary_number_from_report(json, "state_handoff_max_load_pct",
                                   &st->max_load_pct);
  (void)summary_number_from_report(json, "state_handoff_min_free_gb",
                                   &st->min_free_gb);
  char *s = summary_string_from_report(json, "state_handoff_threads");
  snprintf(st->threads, sizeof(st->threads), "%s", s ? s : "");
  free(s);
  if (summary_bool_from_report(json, "state_dry_run_exceeds_max", &b))
    st->dry_run_exceeds_max = b;
  (void)summary_number_from_report(json, "state_dry_run_wall_hours",
                                   &st->dry_run_wall_hours);
  (void)summary_number_from_report(json, "state_dry_run_wall_days",
                                   &st->dry_run_wall_days);
  (void)summary_number_from_report(json, "state_dry_run_thread_years",
                                   &st->dry_run_thread_years);
  (void)summary_number_from_report(
      json, "state_dry_run_campaign_gain_percent",
      &st->dry_run_campaign_gain_percent);
  (void)summary_number_from_report(
      json, "state_dry_run_target_percent_per_run",
      &st->dry_run_target_percent_per_run);
  (void)summary_number_from_report(
      json, "state_dry_run_thread_years_per_run",
      &st->dry_run_thread_years_per_run);
  s = summary_string_from_report(json, "state_canonical_status_report");
  snprintf(st->canonical_status_report, sizeof(st->canonical_status_report),
           "%s", s ? s : "");
  free(s);
  s = summary_string_from_report(json, "state_canonical_progress_report");
  snprintf(st->canonical_progress_report,
           sizeof(st->canonical_progress_report), "%s", s ? s : "");
  free(s);
  s = summary_string_from_report(json, "state_phase");
  snprintf(st->phase, sizeof(st->phase), "%s", s ? s : "");
  free(s);
  s = summary_string_from_report(json, "state_event");
  snprintf(st->event, sizeof(st->event), "%s", s ? s : "");
  free(s);
  s = summary_string_from_report(json, "state_timestamp_utc");
  snprintf(st->timestamp_utc, sizeof(st->timestamp_utc), "%s", s ? s : "");
  free(s);
  s = summary_string_from_report(json, "state_last_report");
  snprintf(st->last_report, sizeof(st->last_report), "%s", s ? s : "");
  free(s);
}

static void append_fuzz_all_run_state_fields(str_buf_t *b,
                                             const fuzz_all_run_state_summary_t *st) {
  if (!b || !st) return;
  (void)sb_append(b, ",\"state\":");
  (void)sb_append_json_str(b,
      fuzz_all_state_label_values(st->readable, st->phase));
  (void)sb_append(b, ",\"state_readable\":");
  (void)sb_append(b, st->readable ? "true" : "false");
  (void)sb_append(b, ",\"state_fresh\":");
  (void)sb_append(b, st->fresh ? "true" : "false");
  (void)sb_append(b, ",\"state_live\":");
  (void)sb_append(b, fuzz_all_state_phase_live(st->phase) ? "true" : "false");
  (void)sb_append(b, ",\"state_child_alive\":");
  (void)sb_append(b, st->child_alive ? "true" : "false");
  (void)sb_append(b, ",\"state_child_status\":");
  (void)sb_append_json_str(b,
      fuzz_all_state_child_status(st->child_pid, st->child_alive));
  (void)sb_append(b, ",\"state_stale_reason\":");
  (void)sb_append_json_str(b,
      fuzz_all_state_stale_reason_values(st->readable, st->fresh, st->phase,
                                         st->age_seconds, st->heartbeat_s,
                                         st->child_pid, st->child_alive));
  (void)sb_appendf(b,
                   ",\"state_age_seconds\":%.0f,"
                   "\"state_stale_after_seconds\":%.0f,"
                   "\"state_cycle\":%.0f,"
                   "\"state_cycles\":%.0f,"
                   "\"state_heartbeat_s\":%.0f,"
                   "\"state_heartbeat_count\":%.0f,"
                   "\"state_child_pid\":%.0f",
                   st->age_seconds,
                   fuzz_all_state_stale_after_seconds(st->heartbeat_s),
                   st->cycle, st->cycles,
                   st->heartbeat_s, st->heartbeat_count, st->child_pid);
  (void)sb_appendf(b,
                   ",\"state_handoff_low_priority\":%s,"
                   "\"state_handoff_nice\":%.0f,"
                   "\"state_handoff_load_wait\":%s,"
                   "\"state_handoff_max_load_pct\":%.0f,"
                   "\"state_handoff_space_guard\":%s,"
                   "\"state_handoff_min_free_gb\":%.0f,"
                   "\"state_handoff_run_lock\":%s",
                   st->low_priority ? "true" : "false",
                   st->nice,
                   st->load_wait ? "true" : "false",
                   st->max_load_pct,
                   st->space_guard ? "true" : "false",
                   st->min_free_gb,
                   st->run_lock ? "true" : "false");
  (void)sb_append(b, ",\"state_handoff_threads\":");
  (void)sb_append_json_str(b, st->threads);
  (void)sb_appendf(b,
                   ",\"state_dry_run_exceeds_max\":%s,"
                   "\"state_dry_run_wall_hours\":%.4f,"
                   "\"state_dry_run_wall_days\":%.4f,"
                   "\"state_dry_run_thread_years\":%.8f,"
                   "\"state_dry_run_campaign_gain_percent\":%.4f,"
                   "\"state_dry_run_target_percent_per_run\":%.4f,"
                   "\"state_dry_run_thread_years_per_run\":%.8f",
                   st->dry_run_exceeds_max ? "true" : "false",
                   st->dry_run_wall_hours, st->dry_run_wall_days,
                   st->dry_run_thread_years,
                   st->dry_run_campaign_gain_percent,
                   st->dry_run_target_percent_per_run,
                   st->dry_run_thread_years_per_run);
  (void)sb_append(b, ",\"state_canonical_status_report\":");
  (void)sb_append_json_str(b, st->canonical_status_report);
  (void)sb_append(b, ",\"state_canonical_progress_report\":");
  (void)sb_append_json_str(b, st->canonical_progress_report);
  (void)sb_append(b, ",\"state_phase\":");
  (void)sb_append_json_str(b, st->phase);
  (void)sb_append(b, ",\"state_event\":");
  (void)sb_append_json_str(b, st->event);
  (void)sb_append(b, ",\"state_timestamp_utc\":");
  (void)sb_append_json_str(b, st->timestamp_utc);
  (void)sb_append(b, ",\"state_last_report\":");
  (void)sb_append_json_str(b, st->last_report);
}

static void append_fuzz_all_run_state_fields_prefixed(
    str_buf_t *b, const char *prefix, const fuzz_all_run_state_summary_t *st) {
  if (!b || !prefix || !*prefix || !st) return;
  (void)sb_appendf(b, ",\"%s\":", prefix);
  (void)sb_append_json_str(b,
      fuzz_all_state_label_values(st->readable, st->phase));
  (void)sb_appendf(b, ",\"%s_readable\":%s", prefix,
                   st->readable ? "true" : "false");
  (void)sb_appendf(b, ",\"%s_fresh\":%s", prefix,
                   st->fresh ? "true" : "false");
  (void)sb_appendf(b, ",\"%s_live\":%s", prefix,
                   fuzz_all_state_phase_live(st->phase) ? "true" : "false");
  (void)sb_appendf(b, ",\"%s_child_alive\":%s", prefix,
                   st->child_alive ? "true" : "false");
  (void)sb_appendf(b, ",\"%s_child_status\":", prefix);
  (void)sb_append_json_str(b,
      fuzz_all_state_child_status(st->child_pid, st->child_alive));
  (void)sb_appendf(b, ",\"%s_stale_reason\":", prefix);
  (void)sb_append_json_str(b,
      fuzz_all_state_stale_reason_values(st->readable, st->fresh, st->phase,
                                         st->age_seconds, st->heartbeat_s,
                                         st->child_pid, st->child_alive));
  (void)sb_appendf(b,
                   ",\"%s_age_seconds\":%.0f,"
                   "\"%s_stale_after_seconds\":%.0f,"
                   "\"%s_heartbeat_s\":%.0f,"
                   "\"%s_heartbeat_count\":%.0f,"
                   "\"%s_child_pid\":%.0f",
                   prefix, st->age_seconds,
                   prefix, fuzz_all_state_stale_after_seconds(st->heartbeat_s),
                   prefix, st->heartbeat_s,
                   prefix, st->heartbeat_count,
                   prefix, st->child_pid);
  (void)sb_appendf(b, ",\"%s_handoff_low_priority\":%s", prefix,
                   st->low_priority ? "true" : "false");
  (void)sb_appendf(b, ",\"%s_handoff_nice\":%.0f", prefix, st->nice);
  (void)sb_appendf(b, ",\"%s_handoff_load_wait\":%s", prefix,
                   st->load_wait ? "true" : "false");
  (void)sb_appendf(b, ",\"%s_handoff_max_load_pct\":%.0f", prefix,
                   st->max_load_pct);
  (void)sb_appendf(b, ",\"%s_handoff_space_guard\":%s", prefix,
                   st->space_guard ? "true" : "false");
  (void)sb_appendf(b, ",\"%s_handoff_min_free_gb\":%.0f", prefix,
                   st->min_free_gb);
  (void)sb_appendf(b, ",\"%s_handoff_run_lock\":%s", prefix,
                   st->run_lock ? "true" : "false");
  (void)sb_appendf(b, ",\"%s_handoff_threads\":", prefix);
  (void)sb_append_json_str(b, st->threads);
  (void)sb_appendf(b, ",\"%s_dry_run_exceeds_max\":%s", prefix,
                   st->dry_run_exceeds_max ? "true" : "false");
  (void)sb_appendf(b, ",\"%s_dry_run_wall_hours\":%.4f", prefix,
                   st->dry_run_wall_hours);
  (void)sb_appendf(b, ",\"%s_dry_run_wall_days\":%.4f", prefix,
                   st->dry_run_wall_days);
  (void)sb_appendf(b, ",\"%s_dry_run_thread_years\":%.8f", prefix,
                   st->dry_run_thread_years);
  (void)sb_appendf(b, ",\"%s_dry_run_campaign_gain_percent\":%.4f", prefix,
                   st->dry_run_campaign_gain_percent);
  (void)sb_appendf(b, ",\"%s_dry_run_target_percent_per_run\":%.4f", prefix,
                   st->dry_run_target_percent_per_run);
  (void)sb_appendf(b, ",\"%s_dry_run_thread_years_per_run\":%.8f", prefix,
                   st->dry_run_thread_years_per_run);
  (void)sb_appendf(b, ",\"%s_canonical_status_report\":", prefix);
  (void)sb_append_json_str(b, st->canonical_status_report);
  (void)sb_appendf(b, ",\"%s_canonical_progress_report\":", prefix);
  (void)sb_append_json_str(b, st->canonical_progress_report);
  (void)sb_appendf(b, ",\"%s_phase\":", prefix);
  (void)sb_append_json_str(b, st->phase);
  (void)sb_appendf(b, ",\"%s_event\":", prefix);
  (void)sb_append_json_str(b, st->event);
  (void)sb_appendf(b, ",\"%s_timestamp_utc\":", prefix);
  (void)sb_append_json_str(b, st->timestamp_utc);
  (void)sb_appendf(b, ",\"%s_last_report\":", prefix);
  (void)sb_append_json_str(b, st->last_report);
}

static bool fuzz_all_run_state_refresh_required(
    const fuzz_all_run_state_summary_t *st);
static const char *fuzz_all_run_state_refresh_reason(
    const fuzz_all_run_state_summary_t *st);

static void append_fuzz_all_recommended_state_fields(
    str_buf_t *b, const char *recommended_action,
    const fuzz_all_run_state_summary_t *run_state,
    const fuzz_all_run_state_summary_t *coverage_next_run_state,
    const char *state_file, const char *state_command,
    const char *state_refresh_command,
    const char *recommended_preview_command,
    const char *coverage_next_state_file,
    const char *coverage_next_state_command,
    const char *coverage_next_state_refresh_command,
    bool coverage_next_state_refresh_required,
    const char *coverage_next_state_refresh_reason) {
  if (!b) return;
  bool use_coverage_next =
      recommended_action &&
      strcmp(recommended_action, "run-missing-evidence") == 0 &&
      coverage_next_run_state;
  const fuzz_all_run_state_summary_t *st =
      use_coverage_next ? coverage_next_run_state : run_state;
  const char *file = use_coverage_next ? coverage_next_state_file : state_file;
  const char *inspect =
      use_coverage_next ? coverage_next_state_command : state_command;
  const char *run_refresh =
      recommended_preview_command && *recommended_preview_command ?
          recommended_preview_command : state_refresh_command;
  const char *refresh =
      use_coverage_next ? coverage_next_state_refresh_command :
                          run_refresh;
  bool refresh_required =
      use_coverage_next ? coverage_next_state_refresh_required :
                          (st && refresh && *refresh &&
                           fuzz_all_run_state_refresh_required(st));
  const char *refresh_reason =
      use_coverage_next ? coverage_next_state_refresh_reason :
                          (refresh_required ?
                               fuzz_all_run_state_refresh_reason(st) : "");
  (void)sb_append(b, ",\"recommended_state_source\":");
  (void)sb_append_json_str(b, use_coverage_next ? "coverage-next" : "run");
  (void)sb_append(b, ",\"recommended_state_file\":");
  (void)sb_append_json_str(b, file ? file : "");
  (void)sb_append(b, ",\"recommended_state_command\":");
  (void)sb_append_json_str(b, inspect ? inspect : "");
  (void)sb_append(b, ",\"recommended_state_refresh_command\":");
  (void)sb_append_json_str(b,
                           refresh_required && refresh ? refresh : "");
  (void)sb_append(b, ",\"recommended_state_refresh_required\":");
  (void)sb_append(b, refresh_required ? "true" : "false");
  (void)sb_append(b, ",\"recommended_state_refresh_reason\":");
  (void)sb_append_json_str(b, refresh_reason ? refresh_reason : "");
  if (st)
    append_fuzz_all_run_state_fields_prefixed(b, "recommended_state", st);
}

static void append_fuzz_all_state_summary_markdown(
    str_buf_t *md, const char *label, const char *inspect_command,
    const char *refresh_command,
    const fuzz_all_run_state_summary_t *st) {
  if (!md || !label) return;
  bool stale = !st || !st->readable || !st->fresh;
  (void)sb_append(md, "- ");
  (void)sb_append(md, label);
  (void)sb_append(md, ": ");
  if (st && st->readable) {
    md_append_code(md, st->phase[0] ? st->phase : "unknown");
    if (st->event[0]) {
      (void)sb_append(md, "/");
      md_append_code(md, st->event);
    }
    (void)sb_appendf(md, "; age %.0fs; %s; %s",
                     st->age_seconds,
                     fuzz_all_state_phase_live(st->phase) ? "live" :
                         "not-live",
                     st->fresh ? "fresh" : "stale");
    if (!st->fresh)
      (void)sb_appendf(
          md, " (%s)",
          fuzz_all_state_stale_reason_values(st->readable, st->fresh,
                                             st->phase, st->age_seconds,
                                             st->heartbeat_s, st->child_pid,
                                             st->child_alive));
    if (st->child_pid > 0.0)
      (void)sb_appendf(md, "; child %.0f %s", st->child_pid,
                       st->child_alive ? "alive" : "dead");
    if (st->heartbeat_count > 0.0)
      (void)sb_appendf(md, "; heartbeats %.0f", st->heartbeat_count);
    if (st->last_report[0]) {
      (void)sb_append(md, "; last ");
      md_append_code(md, st->last_report);
    }
  } else {
    (void)sb_append(md, "not-readable; not-live; stale (missing)");
  }
  if (inspect_command && *inspect_command) {
    (void)sb_append(md, "; inspect ");
    md_append_code(md, inspect_command);
  }
  if (stale && refresh_command && *refresh_command) {
    (void)sb_append(md, "; refresh-state ");
    md_append_code(md, refresh_command);
  }
  (void)sb_append(md, ".\n");
}

static bool fuzz_all_run_state_refresh_required(
    const fuzz_all_run_state_summary_t *st) {
  if (!st || !st->readable) return true;
  if (fuzz_all_state_phase_live(st->phase)) return false;
  return !st->fresh;
}

static const char *fuzz_all_run_state_refresh_reason(
    const fuzz_all_run_state_summary_t *st) {
  if (!fuzz_all_run_state_refresh_required(st)) return "";
  if (!st || !st->readable) return "missing";
  return fuzz_all_state_stale_reason_values(
      st->readable, st->fresh, st->phase, st->age_seconds, st->heartbeat_s,
      st->child_pid, st->child_alive);
}

static bool write_fuzz_all_next_run_script(const char *root,
                                           const char *script_path,
                                           const char *dir_path,
                                           const char *history_path,
                                           const char *worklist_path,
                                           const char *coverage_path,
                                           const char *plan_path,
                                           const char *status_json_path,
                                           const char *status_md_path,
                                           const char *target_arg,
                                           const char *hours_arg,
                                           const char *threads_arg,
                                           const char *profile_arg) {
  if (!script_path || !*script_path) return true;
  char *dir_rel = rel_path_dup(root ? root : "", dir_path ? dir_path : "build/fuzz/all");
  char *history_rel = rel_path_dup(root ? root : "", history_path ? history_path : "build/fuzz/all/history.json");
  char *worklist_rel = rel_path_dup(root ? root : "", worklist_path ? worklist_path : "build/fuzz/all/worklist.json");
  char *coverage_rel = rel_path_dup(root ? root : "", coverage_path ? coverage_path : "build/fuzz/all/coverage.json");
  char *plan_rel = rel_path_dup(root ? root : "", plan_path ? plan_path : "build/fuzz/all/plan.json");
  char *status_rel = rel_path_dup(root ? root : "", status_json_path ? status_json_path : "build/fuzz/all/status.json");
  char *status_md_rel = rel_path_dup(root ? root : "", status_md_path ? status_md_path : "build/fuzz/all/status.md");
  char *script_rel = rel_path_dup(root ? root : "", script_path ? script_path : "build/fuzz/all/run-next.sh");
  char *coverage_canonical_md = path_with_suffix_ext(coverage_rel && *coverage_rel ? coverage_rel : "build/fuzz/all/coverage.json", "", ".md");
  char *pre_history_md =
      path_with_suffix_ext(history_rel && *history_rel ?
                               history_rel : "build/fuzz/all/history.json",
                           "", ".md");
  char *pre_worklist_md =
      path_with_suffix_ext(worklist_rel && *worklist_rel ?
                               worklist_rel : "build/fuzz/all/worklist.json",
                           "", ".md");
  char *pre_plan_md =
      path_with_suffix_ext(plan_rel && *plan_rel ?
                               plan_rel : "build/fuzz/all/plan.json",
                           "", ".md");
  char *script_dir = NULL;
  if (script_path) {
    script_dir = strdup(script_path);
    char *slash = script_dir ? strrchr(script_dir, '/') : NULL;
    if (slash) {
      *slash = '\0';
      if (*script_dir) ny_ensure_dir_recursive(script_dir);
    }
  }
  if (dir_path && *dir_path) ny_ensure_dir_recursive(dir_path);
  str_buf_t sh = {0};
  const char *run_hours_ref = "\"$nynth_run_hours\"";
  const char *run_threads_ref = "\"$nynth_run_threads\"";
  (void)sb_append(&sh, "#!/usr/bin/env bash\n");
  (void)sb_append(&sh, "set -euo pipefail\n");
  append_repo_cache_env_script(&sh, root ? root : ".");
  append_low_priority_shell_helper(&sh);
  append_load_wait_shell_helper(&sh);
  append_space_guard_shell_helper(&sh);
  append_campaign_lock_shell_helper(&sh);
  (void)sb_append(&sh, "mkdir -p ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "history=");
  (void)sb_append_json_str(&sh, history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "history_md=");
  (void)sb_append_json_str(&sh, pre_history_md ? pre_history_md : "build/fuzz/all/history.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "latest_coverage=");
  (void)sb_append_json_str(&sh, coverage_rel && *coverage_rel ? coverage_rel : "build/fuzz/all/coverage.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "latest_coverage_md=");
  (void)sb_append_json_str(&sh, coverage_canonical_md && *coverage_canonical_md ? coverage_canonical_md : "build/fuzz/all/coverage.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "worklist=");
  (void)sb_append_json_str(&sh, worklist_rel && *worklist_rel ? worklist_rel : "build/fuzz/all/worklist.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "worklist_md=");
  (void)sb_append_json_str(&sh, pre_worklist_md ? pre_worklist_md : "build/fuzz/all/worklist.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "plan=");
  (void)sb_append_json_str(&sh, plan_rel && *plan_rel ? plan_rel : "build/fuzz/all/plan.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "plan_md=");
  (void)sb_append_json_str(&sh, pre_plan_md ? pre_plan_md : "build/fuzz/all/plan.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "status=");
  (void)sb_append_json_str(&sh, status_rel && *status_rel ? status_rel : "build/fuzz/all/status.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "status_md=");
  (void)sb_append_json_str(&sh, status_md_rel && *status_md_rel ? status_md_rel : "build/fuzz/all/status.md");
  (void)sb_append(&sh, "\n");
  (void)sb_appendf(&sh, "progress=\"%s/progress.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "progress_md=\"%s/progress.md\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "repeat_status=\"%s/repeat-status.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "repeat_status_md=\"%s/repeat-status.md\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "repeat_progress=\"%s/repeat-progress.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "repeat_progress_md=\"%s/repeat-progress.md\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "missing_evidence=\"%s/run-missing-evidence.sh\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "NYNTH_RUN_DRY_RUN=\"${NYNTH_RUN_DRY_RUN:-0}\"\n");
  (void)sb_append(&sh, "nynth_default_hours=");
  (void)sb_append_json_str(&sh, hours_arg && *hours_arg ? hours_arg : "8");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_default_threads=");
  (void)sb_append_json_str(&sh, threads_arg && *threads_arg ?
                                    threads_arg : NYNTH_DEFAULT_FUZZ_THREADS);
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_run_hours=\"${NYNTH_RUN_HOURS:-$nynth_default_hours}\"\n");
  (void)sb_append(&sh, "nynth_run_threads=\"${NYNTH_RUN_THREADS:-$nynth_default_threads}\"\n");
  (void)sb_append(&sh, "if [ -f \"$history\" ]; then\n");
  (void)sb_append(&sh, "  echo \"nynth repeat guard: refreshing status before repeat resolution\"\n");
  (void)sb_append(&sh, "  nynth_low_priority ./build/nynth fuzz all history --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --json \"$history\" --markdown \"$history_md\"\n");
  append_fuzz_all_strict_history_coverage_refresh_script(
      &sh, "  ", target_arg, run_hours_ref, run_threads_ref, profile_arg);
  (void)sb_append(&sh, "  nynth_low_priority ./build/nynth fuzz all worklist --history \"$history\" --json \"$worklist\" --markdown \"$worklist_md\"\n");
  (void)sb_append(&sh, "  nynth_low_priority ./build/nynth fuzz all plan --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(&sh, " --json \"$plan\" --markdown \"$plan_md\"\n");
  (void)sb_append(&sh, "  nynth_low_priority ./build/nynth fuzz all status --strict --allow-full-pressure-remediation --no-script --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --plan \"$plan\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  append_shell_option_text(&sh, "--next-command",
                           script_rel && *script_rel ?
                               script_rel : "build/fuzz/all/run-next.sh");
  (void)sb_append(&sh, " --json \"$repeat_status\" --markdown \"$repeat_status_md\"\n");
  append_fuzz_all_repeat_progress_refresh_script(
      &sh, dir_rel, target_arg, run_hours_ref, run_threads_ref, profile_arg);
  (void)sb_append(&sh, "  if [ \"$NYNTH_RUN_DRY_RUN\" != \"0\" ] && [ -x \"$missing_evidence\" ] && grep -q '\"recommended_action\"[[:space:]]*:[[:space:]]*\"run-missing-evidence\"' \"$repeat_status\" 2>/dev/null; then\n");
  (void)sb_append(&sh, "    NYNTH_RUN_DRY_RUN=1 \"$missing_evidence\" >/dev/null 2>&1 || true\n");
  (void)sb_append(&sh, "    echo \"nynth repeat dry-run: refreshed missing-evidence preview state=$missing_evidence\"\n");
  (void)sb_append(&sh, "    nynth_low_priority ./build/nynth fuzz all status --strict --allow-full-pressure-remediation --no-script --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --plan \"$plan\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  append_shell_option_text(&sh, "--next-command",
                           script_rel && *script_rel ?
                               script_rel : "build/fuzz/all/run-next.sh");
  (void)sb_append(&sh, " --json \"$repeat_status\" --markdown \"$repeat_status_md\"\n");
  append_fuzz_all_repeat_progress_refresh_script(
      &sh, dir_rel, target_arg, run_hours_ref, run_threads_ref, profile_arg);
  (void)sb_append(&sh, "  fi\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "nynth_repeat_status=");
  (void)sb_append(&sh, "\"$status\"");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "if [ -r \"$repeat_status\" ]; then nynth_repeat_status=\"$repeat_status\"; fi\n");
  (void)sb_append(&sh, "NYNTH_RUN_REPEAT=\"${NYNTH_RUN_REPEAT:-1}\"\n");
  (void)sb_append(&sh, "NYNTH_RUN_DRY_RUN=\"${NYNTH_RUN_DRY_RUN:-0}\"\n");
  (void)sb_append(&sh, "NYNTH_RUN_MAX_CYCLES=\"${NYNTH_RUN_MAX_CYCLES:-0}\"\n");
  (void)sb_append(&sh, "NYNTH_RUN_COOLDOWN_S=\"${NYNTH_RUN_COOLDOWN_S:-0}\"\n");
  (void)sb_append(&sh, "NYNTH_RUN_HEARTBEAT_S=\"${NYNTH_RUN_HEARTBEAT_S:-300}\"\n");
  (void)sb_append(&sh, "nynth_default_stop_file=");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "/stop\n");
  (void)sb_append(&sh, "NYNTH_RUN_STOP_FILE=\"${NYNTH_RUN_STOP_FILE:-$nynth_default_stop_file}\"\n");
  (void)sb_append(&sh, "nynth_default_state_file=");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "/run-next-state.json\n");
  (void)sb_append(&sh, "NYNTH_RUN_STATE_FILE=\"${NYNTH_RUN_STATE_FILE:-$nynth_default_state_file}\"\n");
  (void)sb_append(&sh, "nynth_campaign_dir=");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_json_escape() { printf '%s' \"$1\" | sed 's/\\\\/\\\\\\\\/g; s/\"/\\\\\"/g'; }\n");
  (void)sb_append(&sh, "nynth_json_uint() { case \"${1:-}\" in ''|*[!0-9]*) printf '0';; *) printf '%s' \"$1\";; esac; }\n");
  (void)sb_append(&sh, "nynth_json_number() { awk -v v=\"${1:-0}\" 'BEGIN{if (v ~ /^-?[0-9]+([.][0-9]+)?$/) printf \"%s\", v; else printf \"0\"}'; }\n");
  (void)sb_append(&sh, "nynth_json_enabled() { if [ \"${1:-1}\" = \"0\" ]; then printf false; else printf true; fi; }\n");
  (void)sb_append(&sh, "nynth_write_state() {\n");
  (void)sb_append(&sh, "  local phase=\"${1:-unknown}\" event=\"${2:-}\"\n");
  (void)sb_append(&sh, "  local ts state_dir finished_at child_status live heartbeat_s stale_after\n");
  (void)sb_append(&sh, "  ts=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date +%Y-%m-%dT%H:%M:%S)\n");
  (void)sb_append(&sh, "  if [ -z \"${nynth_state_started_at:-}\" ]; then nynth_state_started_at=\"$ts\"; fi\n");
  (void)sb_append(&sh, "  finished_at=\"\"\n");
  (void)sb_append(&sh, "  if [ \"${nynth_state_terminal:-0}\" = \"1\" ]; then finished_at=\"$ts\"; fi\n");
  (void)sb_append(&sh, "  child_status=\"none\"\n");
  (void)sb_append(&sh, "  case \"${nynth_state_child_pid:-0}\" in ''|*[!0-9]*|0) ;; *) if kill -0 \"$nynth_state_child_pid\" 2>/dev/null; then child_status=\"alive\"; else child_status=\"dead\"; fi;; esac\n");
  (void)sb_append(&sh, "  case \"$phase\" in running|locked|refreshing|cooldown) live=\"true\";; *) live=\"false\";; esac\n");
  (void)sb_append(&sh, "  heartbeat_s=\"${NYNTH_RUN_HEARTBEAT_S:-0}\"\n");
  (void)sb_append(&sh, "  case \"$heartbeat_s\" in ''|*[!0-9]*) heartbeat_s=0;; esac\n");
  (void)sb_append(&sh, "  if [ \"$heartbeat_s\" -gt 0 ]; then stale_after=$((heartbeat_s * 2 + 30)); else stale_after=3600; fi\n");
  (void)sb_append(&sh, "  if [ \"$stale_after\" -lt 60 ]; then stale_after=60; fi\n");
  (void)sb_append(&sh, "  state_dir=$(dirname \"$NYNTH_RUN_STATE_FILE\" 2>/dev/null || printf '.')\n");
  (void)sb_append(&sh, "  mkdir -p \"$state_dir\" 2>/dev/null || true\n");
  (void)sb_append(&sh, "  {\n");
  (void)sb_append(&sh, "    printf '{'\n");
  (void)sb_append(&sh, "    printf '\"phase\":\"%s\",' \"$(nynth_json_escape \"$phase\")\"\n");
  (void)sb_append(&sh, "    printf '\"state\":\"%s\",' \"$(nynth_json_escape \"$phase\")\"\n");
  (void)sb_append(&sh, "    printf '\"event\":\"%s\",' \"$(nynth_json_escape \"$event\")\"\n");
  (void)sb_append(&sh, "    printf '\"timestamp_utc\":\"%s\",' \"$(nynth_json_escape \"$ts\")\"\n");
  (void)sb_append(&sh, "    printf '\"updated_at\":\"%s\",' \"$(nynth_json_escape \"$ts\")\"\n");
  (void)sb_append(&sh, "    printf '\"live\":%s,' \"$live\"\n");
  (void)sb_append(&sh, "    printf '\"child_status\":\"%s\",' \"$(nynth_json_escape \"$child_status\")\"\n");
  (void)sb_append(&sh, "    printf '\"stale_after_seconds\":%s,' \"$stale_after\"\n");
  (void)sb_append(&sh, "    printf '\"started_at\":\"%s\",' \"$(nynth_json_escape \"${nynth_state_started_at:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"finished_at\":\"%s\",' \"$(nynth_json_escape \"$finished_at\")\"\n");
  (void)sb_append(&sh, "    printf '\"pid\":%s,' \"$(nynth_json_uint \"$$\")\"\n");
  (void)sb_append(&sh, "    printf '\"mode\":\"%s\",' \"$(nynth_json_escape \"${nynth_repeat_mode:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"repeat_mode\":\"%s\",' \"$(nynth_json_escape \"${nynth_repeat_mode:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"cycle\":%s,' \"$(nynth_json_uint \"${nynth_run_i:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"cycles\":%s,' \"$(nynth_json_uint \"${NYNTH_RUN_REPEAT:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"repeat_count\":%s,' \"$(nynth_json_uint \"${NYNTH_RUN_REPEAT:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"max_cycles\":%s,' \"$(nynth_json_uint \"${NYNTH_RUN_MAX_CYCLES:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"cooldown_s\":%s,' \"$(nynth_json_uint \"${NYNTH_RUN_COOLDOWN_S:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"heartbeat_s\":%s,' \"$heartbeat_s\"\n");
  (void)sb_append(&sh, "    printf '\"heartbeat_count\":%s,' \"$(nynth_json_uint \"${nynth_state_heartbeat_count:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"child_pid\":%s,' \"$(nynth_json_uint \"${nynth_state_child_pid:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"low_priority\":%s,' \"$(nynth_json_enabled \"${NYNTH_LOW_PRIORITY:-1}\")\"\n");
  (void)sb_append(&sh, "    printf '\"handoff_low_priority\":%s,' \"$(nynth_json_enabled \"${NYNTH_LOW_PRIORITY:-1}\")\"\n");
  (void)sb_append(&sh, "    printf '\"nice\":%s,' \"$(nynth_json_number \"${NYNTH_RUN_NICE:-10}\")\"\n");
  (void)sb_append(&sh, "    printf '\"handoff_nice\":%s,' \"$(nynth_json_number \"${NYNTH_RUN_NICE:-10}\")\"\n");
  (void)sb_append(&sh, "    printf '\"load_wait\":%s,' \"$(nynth_json_enabled \"${NYNTH_LOAD_WAIT:-1}\")\"\n");
  (void)sb_append(&sh, "    printf '\"handoff_load_wait\":%s,' \"$(nynth_json_enabled \"${NYNTH_LOAD_WAIT:-1}\")\"\n");
  (void)sb_append(&sh, "    printf '\"max_load_pct\":%s,' \"$(nynth_json_number \"${NYNTH_MAX_LOAD_PCT:-75}\")\"\n");
  (void)sb_append(&sh, "    printf '\"handoff_max_load_pct\":%s,' \"$(nynth_json_number \"${NYNTH_MAX_LOAD_PCT:-75}\")\"\n");
  (void)sb_append(&sh, "    if [ \"${NYNTH_SPACE_GUARD:-1}\" = \"0\" ] || [ \"${NYNTH_MIN_FREE_GB:-20}\" = \"0\" ]; then printf '\"space_guard\":false,'; else printf '\"space_guard\":true,'; fi\n");
  (void)sb_append(&sh, "    if [ \"${NYNTH_SPACE_GUARD:-1}\" = \"0\" ] || [ \"${NYNTH_MIN_FREE_GB:-20}\" = \"0\" ]; then printf '\"handoff_space_guard\":false,'; else printf '\"handoff_space_guard\":true,'; fi\n");
  (void)sb_append(&sh, "    printf '\"min_free_gb\":%s,' \"$(nynth_json_number \"${NYNTH_MIN_FREE_GB:-20}\")\"\n");
  (void)sb_append(&sh, "    printf '\"handoff_min_free_gb\":%s,' \"$(nynth_json_number \"${NYNTH_MIN_FREE_GB:-20}\")\"\n");
  (void)sb_append(&sh, "    printf '\"run_lock\":%s,' \"$(nynth_json_enabled \"${NYNTH_RUN_LOCK:-1}\")\"\n");
  (void)sb_append(&sh, "    printf '\"handoff_run_lock\":%s,' \"$(nynth_json_enabled \"${NYNTH_RUN_LOCK:-1}\")\"\n");
  (void)sb_append(&sh, "    printf '\"threads\":\"%s\",' \"$(nynth_json_escape \"${nynth_run_threads:-25%}\")\"\n");
  (void)sb_append(&sh, "    printf '\"handoff_threads\":\"%s\",' \"$(nynth_json_escape \"${nynth_run_threads:-25%}\")\"\n");
  (void)sb_append(&sh, "    if [ \"${nynth_exceeds_max:-false}\" = \"true\" ]; then printf '\"dry_run_exceeds_max\":true,'; else printf '\"dry_run_exceeds_max\":false,'; fi\n");
  (void)sb_append(&sh, "    printf '\"dry_run_wall_hours\":%s,' \"$(nynth_json_number \"${nynth_wall_hours:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"dry_run_wall_days\":%s,' \"$(nynth_json_number \"${nynth_wall_days:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"dry_run_thread_years\":%s,' \"$(nynth_json_number \"${nynth_thread_years:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"dry_run_campaign_gain_percent\":%s,' \"$(nynth_json_number \"${nynth_campaign_gain:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"dry_run_target_percent_per_run\":%s,' \"$(nynth_json_number \"${nynth_target_percent_per_run:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"dry_run_thread_years_per_run\":%s,' \"$(nynth_json_number \"${nynth_thread_years_per_run:-0}\")\"\n");
  (void)sb_append(&sh, "    printf '\"campaign_dir\":\"%s\",' \"$(nynth_json_escape \"${nynth_campaign_dir:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"stop_file\":\"%s\",' \"$(nynth_json_escape \"${NYNTH_RUN_STOP_FILE:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"status_report\":\"%s\",' \"$(nynth_json_escape \"${status:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"status_json\":\"%s\",' \"$(nynth_json_escape \"${status:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"progress_report\":\"%s\",' \"$(nynth_json_escape \"${progress:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"progress_json\":\"%s\",' \"$(nynth_json_escape \"${progress:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"canonical_status_report\":\"%s\",' \"$(nynth_json_escape \"${nynth_canonical_status:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"canonical_progress_report\":\"%s\",' \"$(nynth_json_escape \"${nynth_canonical_progress:-}\")\"\n");
  (void)sb_append(&sh, "    printf '\"last_report\":\"%s\"' \"$(nynth_json_escape \"${report:-}\")\"\n");
  (void)sb_append(&sh, "    printf '}\\n'\n");
  (void)sb_append(&sh, "  } > \"$NYNTH_RUN_STATE_FILE\" 2>/dev/null || true\n");
  (void)sb_append(&sh, "  return 0\n");
  (void)sb_append(&sh, "}\n");
  (void)sb_append(&sh, "nynth_state_terminal=0\n");
  (void)sb_append(&sh, "nynth_state_started_at=\"\"\n");
  (void)sb_append(&sh, "nynth_state_child_pid=0\n");
  (void)sb_append(&sh, "nynth_state_heartbeat_pid=0\n");
  (void)sb_append(&sh, "nynth_state_heartbeat_count=0\n");
  (void)sb_append(&sh, "nynth_write_terminal_state() { nynth_state_terminal=1; nynth_write_state \"$1\" \"${2:-}\"; }\n");
  (void)sb_append(&sh, "nynth_state_exit_trap() {\n");
  (void)sb_append(&sh, "  local rc=\"$?\"\n");
  (void)sb_append(&sh, "  nynth_release_campaign_lock\n");
  (void)sb_append(&sh, "  if [ \"$rc\" -ne 0 ]; then nynth_write_terminal_state failed \"exit-$rc\"; elif [ \"${nynth_state_terminal:-0}\" = \"0\" ]; then nynth_write_terminal_state finished exit-0; fi\n");
  (void)sb_append(&sh, "  return \"$rc\"\n");
  (void)sb_append(&sh, "}\n");
  (void)sb_append(&sh, "nynth_state_signal_trap() {\n");
  (void)sb_append(&sh, "  nynth_write_terminal_state interrupted signal\n");
  (void)sb_append(&sh, "  case \"${nynth_state_child_pid:-0}\" in ''|*[!0-9]*|0) ;; *) kill -TERM \"$nynth_state_child_pid\" 2>/dev/null || true; wait \"$nynth_state_child_pid\" 2>/dev/null || true;; esac\n");
  (void)sb_append(&sh, "  case \"${nynth_state_heartbeat_pid:-0}\" in ''|*[!0-9]*|0) ;; *) kill -TERM \"$nynth_state_heartbeat_pid\" 2>/dev/null || true; wait \"$nynth_state_heartbeat_pid\" 2>/dev/null || true;; esac\n");
  (void)sb_append(&sh, "  nynth_release_campaign_lock\n");
  (void)sb_append(&sh, "  exit 130\n");
  (void)sb_append(&sh, "}\n");
  (void)sb_append(&sh, "nynth_install_state_traps() {\n");
  (void)sb_append(&sh, "  trap nynth_state_exit_trap EXIT\n");
  (void)sb_append(&sh, "  trap nynth_state_signal_trap INT TERM\n");
  (void)sb_append(&sh, "}\n");
  (void)sb_append(&sh, "nynth_install_state_traps\n");
  (void)sb_append(&sh, "nynth_run_with_heartbeat() {\n");
  (void)sb_append(&sh, "  \"$@\" &\n");
  (void)sb_append(&sh, "  nynth_state_child_pid=\"$!\"\n");
  (void)sb_append(&sh, "  nynth_state_heartbeat_count=0\n");
  (void)sb_append(&sh, "  nynth_write_state running child-start\n");
  (void)sb_append(&sh, "  nynth_state_heartbeat_pid=0\n");
  (void)sb_append(&sh, "  if [ \"${NYNTH_RUN_HEARTBEAT_S:-0}\" -gt 0 ]; then\n");
  (void)sb_append(&sh, "    (\n");
  (void)sb_append(&sh, "      nynth_state_child_pid=\"$nynth_state_child_pid\"\n");
  (void)sb_append(&sh, "      nynth_state_heartbeat_count=0\n");
  (void)sb_append(&sh, "      while kill -0 \"$nynth_state_child_pid\" 2>/dev/null; do\n");
  (void)sb_append(&sh, "        sleep \"$NYNTH_RUN_HEARTBEAT_S\" || break\n");
  (void)sb_append(&sh, "        if kill -0 \"$nynth_state_child_pid\" 2>/dev/null; then\n");
  (void)sb_append(&sh, "          nynth_state_heartbeat_count=$((nynth_state_heartbeat_count + 1))\n");
  (void)sb_append(&sh, "          nynth_write_state running heartbeat\n");
  (void)sb_append(&sh, "        fi\n");
  (void)sb_append(&sh, "      done\n");
  (void)sb_append(&sh, "    ) &\n");
  (void)sb_append(&sh, "    nynth_state_heartbeat_pid=\"$!\"\n");
  (void)sb_append(&sh, "  fi\n");
  (void)sb_append(&sh, "  set +e\n");
  (void)sb_append(&sh, "  wait \"$nynth_state_child_pid\"\n");
  (void)sb_append(&sh, "  local rc=\"$?\"\n");
  (void)sb_append(&sh, "  set -e\n");
  (void)sb_append(&sh, "  case \"${nynth_state_heartbeat_pid:-0}\" in ''|*[!0-9]*|0) ;; *) kill -TERM \"$nynth_state_heartbeat_pid\" 2>/dev/null || true; wait \"$nynth_state_heartbeat_pid\" 2>/dev/null || true;; esac\n");
  (void)sb_append(&sh, "  nynth_state_heartbeat_pid=0\n");
  (void)sb_append(&sh, "  nynth_state_child_pid=0\n");
  (void)sb_append(&sh, "  nynth_write_state refreshing \"child-exit-$rc\"\n");
  (void)sb_append(&sh, "  return \"$rc\"\n");
  (void)sb_append(&sh, "}\n");
  (void)sb_append(&sh, "nynth_repeat_hours=\"$nynth_run_hours\"\n");
  (void)sb_append(&sh, "nynth_repeat_mode=\"$NYNTH_RUN_REPEAT\"\n");
  (void)sb_append(&sh, "case \"$nynth_repeat_mode\" in auto|good)\n");
  (void)sb_append(&sh, "  if [ ! -r \"$nynth_repeat_status\" ]; then echo \"nynth repeat guard: status not readable for auto repeat: $nynth_repeat_status\"; exit 65; fi\n");
  (void)sb_append(&sh, "  NYNTH_RUN_REPEAT=$(awk 'match($0, /\"runs_to_good_language_score\"[[:space:]]*:[[:space:]]*-?[0-9]+/) { s=substr($0, RSTART, RLENGTH); sub(/.*:/, \"\", s); gsub(/[[:space:]]/, \"\", s); print s; found=1; exit } END { if (!found) exit 1 }' \"$nynth_repeat_status\" 2>/dev/null || true)\n");
  (void)sb_append(&sh, "  if [ -z \"$NYNTH_RUN_REPEAT\" ]; then echo \"nynth repeat guard: runs_to_good_language_score missing in $nynth_repeat_status\"; exit 65; fi\n");
  (void)sb_append(&sh, "  case \"$NYNTH_RUN_REPEAT\" in -*) echo \"nynth repeat guard: auto repeat is not projectable from $nynth_repeat_status\"; exit 65;; esac\n");
  (void)sb_append(&sh, "  if [ \"$NYNTH_RUN_REPEAT\" -eq 0 ]; then echo \"nynth repeat guard: good language score already reached\"; exit 0; fi\n");
  (void)sb_append(&sh, "  echo \"nynth repeat guard: auto repeat count from $nynth_repeat_status = $NYNTH_RUN_REPEAT\"\n");
  (void)sb_append(&sh, "  ;;\n");
  (void)sb_append(&sh, "  target|campaign)\n");
  (void)sb_append(&sh, "  if [ ! -r \"$nynth_repeat_status\" ]; then echo \"nynth repeat guard: status not readable for target repeat: $nynth_repeat_status\"; exit 65; fi\n");
  (void)sb_append(&sh, "  NYNTH_RUN_REPEAT=$(awk 'match($0, /\"runs_needed\"[[:space:]]*:[[:space:]]*-?[0-9]+/) { s=substr($0, RSTART, RLENGTH); sub(/.*:/, \"\", s); gsub(/[[:space:]]/, \"\", s); print s; found=1; exit } END { if (!found) exit 1 }' \"$nynth_repeat_status\" 2>/dev/null || true)\n");
  (void)sb_append(&sh, "  if [ -z \"$NYNTH_RUN_REPEAT\" ]; then echo \"nynth repeat guard: runs_needed missing in $nynth_repeat_status\"; exit 65; fi\n");
  (void)sb_append(&sh, "  case \"$NYNTH_RUN_REPEAT\" in -*) echo \"nynth repeat guard: target repeat is not projectable from $nynth_repeat_status\"; exit 65;; esac\n");
  (void)sb_append(&sh, "  if [ \"$NYNTH_RUN_REPEAT\" -eq 0 ]; then echo \"nynth repeat guard: campaign target already complete\"; exit 0; fi\n");
  (void)sb_append(&sh, "  echo \"nynth repeat guard: target repeat count from $nynth_repeat_status = $NYNTH_RUN_REPEAT\"\n");
  (void)sb_append(&sh, "  ;;\n");
  (void)sb_append(&sh, "esac\n");
  (void)sb_append(&sh, "case \"$NYNTH_RUN_REPEAT\" in ''|*[!0-9]*) echo \"nynth repeat guard: NYNTH_RUN_REPEAT must be a positive integer\"; exit 64;; esac\n");
  (void)sb_append(&sh, "if [ \"$NYNTH_RUN_REPEAT\" -lt 1 ]; then echo \"nynth repeat guard: NYNTH_RUN_REPEAT must be a positive integer\"; exit 64; fi\n");
  (void)sb_append(&sh, "case \"$nynth_repeat_mode\" in auto|good|target|campaign) ;; 1) nynth_repeat_mode=once ;; *) nynth_repeat_mode=count ;; esac\n");
  (void)sb_append(&sh, "case \"$NYNTH_RUN_MAX_CYCLES\" in ''|*[!0-9]*) echo \"nynth repeat guard: NYNTH_RUN_MAX_CYCLES must be a non-negative integer\"; exit 64;; esac\n");
  (void)sb_append(&sh, "case \"$NYNTH_RUN_COOLDOWN_S\" in ''|*[!0-9]*) echo \"nynth repeat guard: NYNTH_RUN_COOLDOWN_S must be a non-negative integer\"; exit 64;; esac\n");
  (void)sb_append(&sh, "case \"$NYNTH_RUN_HEARTBEAT_S\" in ''|*[!0-9]*) echo \"nynth repeat guard: NYNTH_RUN_HEARTBEAT_S must be a non-negative integer\"; exit 64;; esac\n");
  (void)sb_append(&sh, "nynth_repeat_exceeds_max=0\n");
  (void)sb_append(&sh, "if awk -v r=\"$NYNTH_RUN_REPEAT\" -v m=\"$NYNTH_RUN_MAX_CYCLES\" 'BEGIN { exit (m > 0 && r > m) ? 0 : 1 }'; then nynth_repeat_exceeds_max=1; fi\n");
  (void)sb_append(&sh, "if [ \"$NYNTH_RUN_DRY_RUN\" != \"0\" ]; then\n");
  (void)sb_append(&sh, "  nynth_target_percent_per_run=$(awk 'match($0, /\"target_percent_per_run\"[[:space:]]*:[[:space:]]*-?[0-9]+([.][0-9]+)?/) { s=substr($0, RSTART, RLENGTH); sub(/.*:/, \"\", s); gsub(/[[:space:]]/, \"\", s); print s; found=1; exit } END { if (!found) exit 1 }' \"$nynth_repeat_status\" 2>/dev/null || true)\n");
  (void)sb_append(&sh, "  nynth_thread_years_per_run=$(awk 'match($0, /\"thread_years_per_run\"[[:space:]]*:[[:space:]]*-?[0-9]+([.][0-9]+)?/) { s=substr($0, RSTART, RLENGTH); sub(/.*:/, \"\", s); gsub(/[[:space:]]/, \"\", s); print s; found=1; exit } END { if (!found) exit 1 }' \"$nynth_repeat_status\" 2>/dev/null || true)\n");
  (void)sb_append(&sh, "  nynth_wall_hours=$(awk -v c=\"$NYNTH_RUN_REPEAT\" -v h=\"$nynth_repeat_hours\" 'BEGIN { if (h ~ /^[0-9]+([.][0-9]+)?$/) printf \"%.2f\", c*h; else printf \"unknown\" }')\n");
  (void)sb_append(&sh, "  nynth_wall_days=$(awk -v h=\"$nynth_wall_hours\" 'BEGIN { if (h ~ /^[0-9]+([.][0-9]+)?$/) printf \"%.2f\", h/24.0; else printf \"unknown\" }')\n");
  (void)sb_append(&sh, "  nynth_thread_years=$(awk -v c=\"$NYNTH_RUN_REPEAT\" -v y=\"$nynth_thread_years_per_run\" 'BEGIN { if (y ~ /^-?[0-9]+([.][0-9]+)?$/) printf \"%.6f\", c*y; else printf \"unknown\" }')\n");
  (void)sb_append(&sh, "  nynth_campaign_gain=$(awk -v c=\"$NYNTH_RUN_REPEAT\" -v p=\"$nynth_target_percent_per_run\" 'BEGIN { if (p ~ /^-?[0-9]+([.][0-9]+)?$/) printf \"%.4f\", c*p; else printf \"unknown\" }')\n");
  (void)sb_append(&sh, "  nynth_exceeds_max=false\n");
  (void)sb_append(&sh, "  if [ \"$nynth_repeat_exceeds_max\" = \"1\" ]; then nynth_exceeds_max=true; fi\n");
  (void)sb_append(&sh, "  nynth_stop_file_label=\"${NYNTH_RUN_STOP_FILE:-none}\"\n");
  (void)sb_append(&sh, "  nynth_dry_low_priority=$(nynth_json_enabled \"${NYNTH_LOW_PRIORITY:-1}\")\n");
  (void)sb_append(&sh, "  nynth_dry_nice=$(nynth_json_number \"${NYNTH_RUN_NICE:-10}\")\n");
  (void)sb_append(&sh, "  nynth_dry_load_wait=$(nynth_json_enabled \"${NYNTH_LOAD_WAIT:-1}\")\n");
  (void)sb_append(&sh, "  nynth_dry_max_load_pct=$(nynth_json_number \"${NYNTH_MAX_LOAD_PCT:-75}\")\n");
  (void)sb_append(&sh, "  if [ \"${NYNTH_SPACE_GUARD:-1}\" = \"0\" ] || [ \"${NYNTH_MIN_FREE_GB:-20}\" = \"0\" ]; then nynth_dry_space_guard=false; else nynth_dry_space_guard=true; fi\n");
  (void)sb_append(&sh, "  nynth_dry_min_free_gb=$(nynth_json_number \"${NYNTH_MIN_FREE_GB:-20}\")\n");
  (void)sb_append(&sh, "  nynth_dry_run_lock=$(nynth_json_enabled \"${NYNTH_RUN_LOCK:-1}\")\n");
  (void)sb_append(&sh, "  nynth_canonical_status=\"$status\"\n");
  (void)sb_append(&sh, "  nynth_canonical_status_md=\"$status_md\"\n");
  (void)sb_append(&sh, "  nynth_canonical_progress=\"$progress\"\n");
  (void)sb_append(&sh, "  nynth_canonical_progress_md=\"$progress_md\"\n");
  (void)sb_append(&sh, "  if [ -r \"$nynth_repeat_status\" ]; then status=\"$nynth_repeat_status\"; fi\n");
  (void)sb_append(&sh, "  if [ -r \"$repeat_progress\" ]; then progress=\"$repeat_progress\"; fi\n");
  (void)sb_append(&sh, "  nynth_write_terminal_state dry-run preview\n");
  (void)sb_append(&sh, "  if [ -f \"$history\" ]; then\n");
  (void)sb_append(&sh, "    echo \"nynth repeat dry-run: refreshing repeat reports after state write\"\n");
  (void)sb_append(&sh, "    nynth_low_priority ./build/nynth fuzz all status --strict --allow-full-pressure-remediation --no-script --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --plan \"$plan\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  append_shell_option_text(&sh, "--next-command",
                           script_rel && *script_rel ?
                               script_rel : "build/fuzz/all/run-next.sh");
  (void)sb_append(&sh, " --json \"$repeat_status\" --markdown \"$repeat_status_md\"\n");
  append_fuzz_all_repeat_progress_refresh_script(
      &sh, dir_rel, target_arg, run_hours_ref, run_threads_ref, profile_arg);
  (void)sb_append(&sh, "    echo \"nynth repeat dry-run: refreshing canonical cockpit after state write\"\n");
  (void)sb_append(&sh, "    nynth_low_priority ./build/nynth fuzz all status --strict --allow-full-pressure-remediation --no-script --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --plan \"$plan\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  append_shell_option_text(&sh, "--next-command",
                           script_rel && *script_rel ?
                               script_rel : "build/fuzz/all/run-next.sh");
  (void)sb_append(&sh, " --json \"$nynth_canonical_status\" --markdown \"$nynth_canonical_status_md\"\n");
  (void)sb_append(&sh, "    nynth_low_priority ./build/nynth fuzz all progress --strict --allow-full-pressure-remediation --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --status \"$nynth_canonical_status\" --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --plan \"$plan\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(&sh, " --json \"$nynth_canonical_progress\" --markdown \"$nynth_canonical_progress_md\"\n");
  (void)sb_append(&sh, "  fi\n");
  (void)sb_append(&sh, "  echo \"nynth repeat dry-run: mode=$nynth_repeat_mode cycles=$NYNTH_RUN_REPEAT max_cycles=$NYNTH_RUN_MAX_CYCLES cooldown_s=$NYNTH_RUN_COOLDOWN_S heartbeat_s=$NYNTH_RUN_HEARTBEAT_S stop_file=$nynth_stop_file_label exceeds_max=$nynth_exceeds_max status=$nynth_repeat_status\"\n");
  (void)sb_append(&sh, "  echo \"nynth repeat dry-run: wall_hours=$nynth_wall_hours wall_days=$nynth_wall_days thread_years=$nynth_thread_years campaign_gain_percent=$nynth_campaign_gain\"\n");
  (void)sb_append(&sh, "  echo \"nynth repeat dry-run: guards low_priority=$nynth_dry_low_priority nice=$nynth_dry_nice load_wait=$nynth_dry_load_wait max_load_pct=$nynth_dry_max_load_pct space_guard=$nynth_dry_space_guard min_free_gb=$nynth_dry_min_free_gb run_lock=$nynth_dry_run_lock threads=$nynth_run_threads\"\n");
  (void)sb_append(&sh, "  echo \"nynth repeat dry-run: profile=");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(&sh, " hours=$nynth_run_hours threads=$nynth_run_threads target_years=");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " dir=");
  (void)sb_append(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "\"\n");
  (void)sb_append(&sh, "  exit 0\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "if [ \"$nynth_repeat_exceeds_max\" = \"1\" ]; then\n");
  (void)sb_append(&sh, "  nynth_write_terminal_state guarded max-cycles\n");
  (void)sb_append(&sh, "  echo \"nynth repeat guard: resolved cycles $NYNTH_RUN_REPEAT exceeds NYNTH_RUN_MAX_CYCLES=$NYNTH_RUN_MAX_CYCLES\"\n");
  (void)sb_append(&sh, "  exit 65\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "if [ -n \"$NYNTH_RUN_STOP_FILE\" ] && [ -e \"$NYNTH_RUN_STOP_FILE\" ]; then\n");
  (void)sb_append(&sh, "  nynth_write_terminal_state stopped before-campaign\n");
  (void)sb_append(&sh, "  echo \"nynth repeat guard: stop file present before campaign: $NYNTH_RUN_STOP_FILE\"\n");
  (void)sb_append(&sh, "  exit 0\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "nynth_acquire_campaign_lock ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_install_state_traps\n");
  (void)sb_append(&sh, "nynth_write_state locked acquired-lock\n");
  (void)sb_append(&sh, "nynth_run_i=1\n");
  (void)sb_append(&sh, "while [ \"$nynth_run_i\" -le \"$NYNTH_RUN_REPEAT\" ]; do\n");
  (void)sb_append(&sh, "if [ -n \"$NYNTH_RUN_STOP_FILE\" ] && [ -e \"$NYNTH_RUN_STOP_FILE\" ]; then\n");
  (void)sb_append(&sh, "  nynth_write_terminal_state stopped before-cycle\n");
  (void)sb_append(&sh, "  echo \"nynth campaign stop requested before cycle ${nynth_run_i}: $NYNTH_RUN_STOP_FILE\"\n");
  (void)sb_append(&sh, "  exit 0\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "echo \"nynth campaign cycle ${nynth_run_i}/${NYNTH_RUN_REPEAT}\"\n");
  (void)sb_append(&sh, "nynth_write_state running cycle-start\n");
  (void)sb_append(&sh, "ts=$(date +%Y%m%d-%H%M%S)\n");
  (void)sb_appendf(&sh, "report=\"%s/insane-${ts}.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "audit=\"%s/insane-${ts}-audit.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "findings=\"%s/insane-${ts}-findings.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "findings_md=\"%s/insane-${ts}-findings.md\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "coverage=\"%s/insane-${ts}-coverage.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "coverage_md=\"%s/insane-${ts}-coverage.md\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "latest_coverage=");
  (void)sb_append_json_str(&sh, coverage_rel && *coverage_rel ? coverage_rel : "build/fuzz/all/coverage.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "latest_coverage_md=");
  (void)sb_append_json_str(&sh, coverage_canonical_md && *coverage_canonical_md ? coverage_canonical_md : "build/fuzz/all/coverage.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "history=");
  (void)sb_append_json_str(&sh, history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "history_md=");
  char *history_md = path_with_suffix_ext(history_rel && *history_rel ? history_rel : "build/fuzz/all/history.json", "", ".md");
  (void)sb_append_json_str(&sh, history_md ? history_md : "build/fuzz/all/history.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "worklist=");
  (void)sb_append_json_str(&sh, worklist_rel && *worklist_rel ? worklist_rel : "build/fuzz/all/worklist.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "worklist_md=");
  char *worklist_md = path_with_suffix_ext(worklist_rel && *worklist_rel ? worklist_rel : "build/fuzz/all/worklist.json", "", ".md");
  (void)sb_append_json_str(&sh, worklist_md ? worklist_md : "build/fuzz/all/worklist.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "worklist_history=");
  char *worklist_history =
      path_with_suffix_ext(worklist_rel && *worklist_rel ?
                               worklist_rel : "build/fuzz/all/worklist.json",
                           "-history", ".json");
  (void)sb_append_json_str(&sh, worklist_history ?
                                   worklist_history :
                                   "build/fuzz/all/worklist-history.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "worklist_history_md=");
  char *worklist_history_md =
      path_with_suffix_ext(worklist_rel && *worklist_rel ?
                               worklist_rel : "build/fuzz/all/worklist.json",
                           "-history", ".md");
  (void)sb_append_json_str(&sh, worklist_history_md ?
                                   worklist_history_md :
                                   "build/fuzz/all/worklist-history.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "plan=");
  (void)sb_append_json_str(&sh, plan_rel && *plan_rel ? plan_rel : "build/fuzz/all/plan.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "plan_md=");
  char *plan_md = path_with_suffix_ext(plan_rel && *plan_rel ? plan_rel : "build/fuzz/all/plan.json", "", ".md");
  (void)sb_append_json_str(&sh, plan_md ? plan_md : "build/fuzz/all/plan.md");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "status=");
  (void)sb_append_json_str(&sh, status_rel && *status_rel ? status_rel : "build/fuzz/all/status.json");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "status_md=");
  (void)sb_append_json_str(&sh, status_md_rel && *status_md_rel ? status_md_rel : "build/fuzz/all/status.md");
  (void)sb_append(&sh, "\n");
  (void)sb_appendf(&sh, "progress=\"%s/progress.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "progress_md=\"%s/progress.md\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "old_paths=\"%s/old-paths.json\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_appendf(&sh, "old_paths_md=\"%s/old-paths.md\"\n",
                   dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json \"$old_paths\" --markdown \"$old_paths_md\"\n");
  (void)sb_append(&sh, "if [ -f \"$history\" ]; then\n");
  (void)sb_append(&sh, "  nynth_low_priority ./build/nynth fuzz all history --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --json \"$history\" --markdown \"$history_md\"\n");
  append_fuzz_all_strict_history_coverage_refresh_script(
      &sh, "  ", target_arg, run_hours_ref, run_threads_ref, profile_arg);
  (void)sb_append(&sh, "  nynth_low_priority ./build/nynth fuzz all worklist --history \"$history\" --json \"$worklist\" --markdown \"$worklist_md\"\n");
  (void)sb_append(&sh, "  nynth_low_priority ./build/nynth fuzz all worklist --history \"$history\" --include-history --json \"$worklist_history\" --markdown \"$worklist_history_md\"\n");
  (void)sb_append(&sh, "  nynth_low_priority ./build/nynth fuzz all plan --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(&sh, " --json \"$plan\" --markdown \"$plan_md\"\n");
  (void)sb_append(&sh, "  nynth_low_priority ./build/nynth fuzz all status --strict --allow-full-pressure-remediation --no-script --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --plan \"$plan\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  append_shell_option_text(&sh, "--next-command",
                           script_rel && *script_rel ?
                               script_rel : "build/fuzz/all/run-next.sh");
  (void)sb_append(&sh, " --json \"$repeat_status\" --markdown \"$repeat_status_md\"\n");
  append_fuzz_all_repeat_progress_refresh_script(
      &sh, dir_rel, target_arg, run_hours_ref, run_threads_ref, profile_arg);
  (void)sb_append(&sh, "  if grep -q '\"campaign_complete\":true' \"$repeat_status\"; then\n");
  (void)sb_append(&sh, "    nynth_write_terminal_state complete pre-cycle-status\n");
  (void)sb_append(&sh, "    echo \"campaign target already complete: $repeat_status\"\n");
  (void)sb_append(&sh, "    exit 0\n");
  (void)sb_append(&sh, "  fi\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "nynth_require_free_space\n");
  (void)sb_append(&sh, "nynth_wait_for_load\n");
  (void)sb_append(&sh, "nynth_run_with_heartbeat nynth_low_priority ./build/nynth fuzz all run --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --fail-fast --no-nytrix --no-sanitizers --json \"$report\"\n");
  (void)sb_append(&sh, "nynth_write_state refreshing run-complete\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all audit --report \"$report\" --strict --json \"$audit\"\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all findings --report \"$report\" --json \"$findings\" --markdown \"$findings_md\"\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all coverage --report \"$report\" --json \"$coverage\" --markdown \"$coverage_md\"\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all history --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --json \"$history\" --markdown \"$history_md\"\n");
  append_fuzz_all_strict_history_coverage_refresh_script(
      &sh, "", target_arg, run_hours_ref, run_threads_ref, profile_arg);
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all worklist --history \"$history\" --json \"$worklist\" --markdown \"$worklist_md\"");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all worklist --history \"$history\" --include-history --json \"$worklist_history\" --markdown \"$worklist_history_md\"\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all plan --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\"");
  (void)sb_append(&sh, " --coverage \"$latest_coverage\" --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(&sh, " --json \"$plan\" --markdown \"$plan_md\"");
  (void)sb_append(&sh, "\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json \"$old_paths\" --markdown \"$old_paths_md\"\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all status --strict --allow-full-pressure-remediation --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --plan \"$plan\"");
  (void)sb_append(&sh, " --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(&sh, " --json \"$status\" --markdown \"$status_md\"\n");
  append_fuzz_all_status_progress_refresh_script(
      &sh, dir_rel, target_arg, run_hours_ref, run_threads_ref, profile_arg);
  (void)sb_append(&sh, "if grep -q '\"campaign_complete\":true' \"$status\"; then\n");
  (void)sb_append(&sh, "  nynth_write_terminal_state complete campaign-target\n");
  (void)sb_append(&sh, "  echo \"campaign target complete after: $report\"\n");
  (void)sb_append(&sh, "  exit 0\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "if [ \"$nynth_repeat_mode\" = \"good\" ] || [ \"$nynth_repeat_mode\" = \"auto\" ]; then\n");
  (void)sb_append(&sh, "  nynth_language_gap=$(awk 'match($0, /\"language_score_gap_percent\"[[:space:]]*:[[:space:]]*-?[0-9]+([.][0-9]+)?/) { s=substr($0, RSTART, RLENGTH); sub(/.*:/, \"\", s); gsub(/[[:space:]]/, \"\", s); print s; found=1; exit } END { if (!found) exit 1 }' \"$status\" 2>/dev/null || true)\n");
  (void)sb_append(&sh, "  if [ -n \"$nynth_language_gap\" ] && awk -v g=\"$nynth_language_gap\" 'BEGIN { exit (g <= 0.0001) ? 0 : 1 }'; then\n");
  (void)sb_append(&sh, "    nynth_write_terminal_state good language-score\n");
  (void)sb_append(&sh, "    echo \"good language score reached after: $report\"\n");
  (void)sb_append(&sh, "    exit 0\n");
  (void)sb_append(&sh, "  fi\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "echo \"next campaign complete: $report\"\n");
  (void)sb_append(&sh, "nynth_write_state cycle-complete report-ready\n");
  (void)sb_append(&sh, "if [ -n \"$NYNTH_RUN_STOP_FILE\" ] && [ -e \"$NYNTH_RUN_STOP_FILE\" ]; then\n");
  (void)sb_append(&sh, "  nynth_write_terminal_state stopped after-cycle\n");
  (void)sb_append(&sh, "  echo \"nynth campaign stop requested after cycle ${nynth_run_i}: $NYNTH_RUN_STOP_FILE\"\n");
  (void)sb_append(&sh, "  exit 0\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "if [ \"$nynth_run_i\" -lt \"$NYNTH_RUN_REPEAT\" ] && [ \"$NYNTH_RUN_COOLDOWN_S\" -gt 0 ]; then\n");
  (void)sb_append(&sh, "  nynth_write_state cooldown sleep\n");
  (void)sb_append(&sh, "  echo \"nynth campaign cooldown: sleep ${NYNTH_RUN_COOLDOWN_S}s before next cycle\"\n");
  (void)sb_append(&sh, "  sleep \"$NYNTH_RUN_COOLDOWN_S\"\n");
  (void)sb_append(&sh, "fi\n");
  (void)sb_append(&sh, "nynth_run_i=$((nynth_run_i + 1))\n");
  (void)sb_append(&sh, "done\n");
  (void)sb_append(&sh, "nynth_write_terminal_state finished run-complete\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all old-paths --dry-run --nytrix-root ../nytrix --archive-dir build/cache/old-nytrix --json \"$old_paths\" --markdown \"$old_paths_md\"\n");
  (void)sb_append(&sh, "nynth_low_priority ./build/nynth fuzz all status --strict --allow-full-pressure-remediation --dir ");
  (void)sb_append_json_str(&sh, dir_rel && *dir_rel ? dir_rel : "build/fuzz/all");
  (void)sb_append(&sh, " --history \"$history\" --worklist \"$worklist\" --coverage \"$latest_coverage\" --plan \"$plan\"");
  (void)sb_append(&sh, " --target-thread-years ");
  (void)sb_append(&sh, target_arg && *target_arg ? target_arg : "10");
  (void)sb_append(&sh, " --hours ");
  (void)sb_append(&sh, run_hours_ref);
  (void)sb_append(&sh, " --threads ");
  (void)sb_append(&sh, run_threads_ref);
  (void)sb_append(&sh, " --profile ");
  (void)sb_append(&sh, profile_arg && *profile_arg ? profile_arg : "insane");
  (void)sb_append(&sh, " --json \"$status\" --markdown \"$status_md\"\n");
  append_fuzz_all_status_progress_refresh_script(
      &sh, dir_rel, target_arg, run_hours_ref, run_threads_ref, profile_arg);
  bool ok = sh.data && write_file_text(script_path, sh.data);
  if (ok) (void)chmod(script_path, 0755);
  free(sh.data);
  free(dir_rel);
  free(history_rel);
  free(worklist_rel);
  free(coverage_rel);
  free(plan_rel);
  free(status_rel);
  free(status_md_rel);
  free(script_rel);
  free(coverage_canonical_md);
  free(pre_history_md);
  free(pre_worklist_md);
  free(pre_plan_md);
  free(script_dir);
  free(history_md);
  free(worklist_md);
  free(worklist_history);
  free(worklist_history_md);
  free(plan_md);
  return ok;
}

