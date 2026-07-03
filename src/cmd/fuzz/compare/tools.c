static int cmd_public_selftest_synth_print(int argc, char **argv) {
  char root[4096], ny_bin[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  bool has_ny = find_ny_bin(root, ny_bin, sizeof(ny_bin));
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *cc = getenv("CC");
  if (!cc || !*cc) cc = "cc";
  char *shape_dir = NULL, *work_dir = NULL;
  bool path_ok = nynth_asprintf(&shape_dir, "shapes") >= 0 &&
                 (work_dir = nynth_scratch_pathf(NULL, "selftest_synth_print_%ld",
                                                 (long)getpid())) != NULL;
  string_list_t rows = {0}, failures = {0};
  if (!path_ok || !work_dir || !mkdir_p(work_dir)) {
    (void)string_list_push_take(&failures, make_worker_failure_row("synth-print", "prepare", 1, "", "workdir allocation failed"));
  } else {
    char *shape_argv[] = {g_self_path, "validate-shapes", shape_dir, NULL};
    proc_result_t shape_pr = run_proc(shape_argv, root, 60.0);
    double shape_errors = 1.0;
    if (shape_pr.out) (void)extract_json_number(shape_pr.out, "errors", &shape_errors);
    bool shapes_ok = shape_pr.rc == 0 && shape_errors == 0.0;
    (void)string_list_push_take(&rows, native_row_status("validate_shapes", "synth-trust",
                                                        shapes_ok, "source", shape_dir));
    if (!shapes_ok)
      (void)string_list_push_take(&failures, make_worker_failure_row("validate-shapes", "shape-validation",
                                                                    shape_pr.rc, shape_pr.out, shape_pr.err));
    proc_result_free(&shape_pr);

    char *list_argv[] = {g_self_path, "synth", "print", "--list", "--generator", "auto", NULL};
    proc_result_t list_pr = run_proc(list_argv, root, 30.0);
    bool list_ok = list_pr.rc == 0 && list_pr.out &&
                   strstr(list_pr.out, "\"mode\":\"list-shapes\"") &&
                   strstr(list_pr.out, "optimizer-induction-affine");
    (void)string_list_push_take(&rows, native_row_status("synth_print_list", "synth-trust",
                                                        list_ok, "generator", "auto"));
    if (!list_ok)
      (void)string_list_push_take(&failures, make_worker_failure_row("synth-print-list", "synth-print-list",
                                                                    list_pr.rc, list_pr.out, list_pr.err));
    proc_result_free(&list_pr);

    generated_case_t pair;
    memset(&pair, 0, sizeof(pair));
    char *pair_out_dir = NULL;
    (void)asprintf(&pair_out_dir, "%s/auto_pair", work_dir);
    char *pair_argv[] = {
      g_self_path, "synth", "print", "--lang", "both",
      "--shape", "optimizer-induction-affine",
      "--seed", "7", "--generator", "auto",
      "--out", pair_out_dir, NULL
    };
    proc_result_t pair_pr = run_proc(pair_argv, root, 45.0);
    bool pair_ok = false;
    if (pair_pr.rc == 0 && pair_pr.out) {
      const char *obj = json_value_after_key(pair_pr.out, "case");
      const char *obj_end = obj && *obj == '{' ? matching_json_end(obj, '{', '}') : NULL;
      if (obj_end) {
        pair.name = json_string_or_empty_range_local(obj, obj_end + 1, "name");
        pair.c_path = json_string_or_empty_range_local(obj, obj_end + 1, "c_source");
        pair.ny_path = json_string_or_empty_range_local(obj, obj_end + 1, "ny_source");
        pair.ir_path = json_string_or_empty_range_local(obj, obj_end + 1, "nynth_ir");
        pair.json = strndup_local(obj, (size_t)(obj_end - obj + 1));
        pair_ok = pair.name && *pair.name && pair.c_path && *pair.c_path &&
                  pair.ny_path && *pair.ny_path && pair.ir_path && *pair.ir_path;
      }
    }
    (void)string_list_push_take(&rows, native_row_status("synth_print_auto_pair", "synth-trust",
                                                        pair_ok, "shape", "optimizer-induction-affine"));
    if (!pair_ok)
      (void)string_list_push_take(&failures, make_worker_failure_row("synth-print-auto-pair", "synth-print-pair",
                                                                    pair_pr.rc, pair_pr.out, pair_pr.err));
    proc_result_free(&pair_pr);

    if (pair_ok) {
      char *bin = NULL;
      (void)asprintf(&bin, "%s/auto_pair_c", work_dir);
      char *compile_argv[] = {
        (char *)cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
        pair.c_path, "-o", bin, NULL
      };
      proc_result_t compile = run_proc(compile_argv, root, 45.0);
      proc_result_t run = {0};
      bool c_ok = compile.rc == 0;
      if (c_ok) {
        char *run_argv[] = {bin, NULL};
        run = run_proc(run_argv, root, 20.0);
        c_ok = run.rc == 0;
      }
      if (c_ok) {
        (void)string_list_push_take(&rows, synth_print_selftest_row(root, "synth_pair_c_compile_run",
                                                                    "optimizer-induction-affine", 7,
                                                                    pair.c_path, bin, &run));
      } else {
        proc_result_t *bad = compile.rc == 0 ? &run : &compile;
        (void)string_list_push_take(&rows, make_worker_failure_row("synth-pair-c", "synth-pair-c",
                                                                  bad->rc, bad->out, bad->err));
        (void)string_list_push_take(&failures, make_worker_failure_row("synth-pair-c", "synth-pair-c",
                                                                      bad->rc, bad->out, bad->err));
      }
      proc_result_free(&run);
      proc_result_free(&compile);
      free(bin);

      proc_result_t ny_emit = {0};
      bool ny_ok = has_ny && compile_or_run_ny_source(root, ny_bin, pair.ny_path, false, 60.0, &ny_emit);
      (void)string_list_push_take(&rows, native_row_status("synth_pair_ny_emit_only", "synth-trust",
                                                          ny_ok, "source", pair.ny_path));
      if (!ny_ok)
        (void)string_list_push_take(&failures, make_worker_failure_row("synth-pair-ny", "ny-emit-only",
                                                                      has_ny ? ny_emit.rc : 1,
                                                                      ny_emit.out,
                                                                      has_ny ? ny_emit.err : "ny binary not found"));
      proc_result_free(&ny_emit);
    }

    char *trust_json = NULL;
    (void)asprintf(&trust_json, "%s/trust.json", work_dir);
    char *trust_argv[] = {
      g_self_path, "synth", "generate", "--fast", "--cases", "1", "--seed", "7",
      "--generator", "auto", "--capture-failures", "--max-reduce-checks", "80",
      "--timeout-s", "60", "--json", trust_json, NULL
    };
    proc_result_t trust_pr = run_proc(trust_argv, root, 180.0);
    file_buf_t trust = {0};
    bool trust_read = trust_json && read_file(trust_json, &trust);
    bool trust_ok = trust_pr.rc == 0 && trust_read && !json_failures_nonempty(trust.data);
    bool captured = trust_read && strstr(trust.data, "\"captured\":true");
    if (trust_ok && captured) {
      char *reduced = json_string_or_empty(trust.data, "reduced_source");
      char *resolved = reduced && *reduced ? resolve_existing_file(root, reduced) : NULL;
      trust_ok = resolved != NULL;
      free(resolved);
      free(reduced);
    }
    str_buf_t trust_row = {0};
    (void)sb_append(&trust_row, "{\"name\":\"synth_generate_fast_capture\","
                                "\"kind\":\"synth-trust\",\"ok\":");
    (void)sb_append(&trust_row, trust_ok ? "true" : "false");
    (void)sb_append(&trust_row, ",\"captured\":");
    (void)sb_append(&trust_row, captured ? "true" : "false");
    (void)sb_append(&trust_row, ",\"report\":");
    append_rel_json_str(&trust_row, root, trust_json ? trust_json : "");
    (void)sb_append(&trust_row, ",\"engine\":\"nynth_core\"}");
    (void)string_list_push_take(&rows, sb_take(&trust_row));
    if (!trust_ok)
      (void)string_list_push_take(&failures, make_worker_failure_row("synth-generate-trust", "synth-generate-fast",
                                                                    trust_pr.rc, trust_pr.out,
                                                                    trust_pr.err && *trust_pr.err ? trust_pr.err : "trust report failed"));
    free(trust.data);
    proc_result_free(&trust_pr);
    free(trust_json);

    char *det_a = NULL, *det_b = NULL;
    bool det_paths_ok = asprintf(&det_a, "%s/determinism_a.c", work_dir) >= 0 &&
                        asprintf(&det_b, "%s/determinism_b.c", work_dir) >= 0;
    bool det_ok = false;
    if (det_paths_ok && det_a && det_b &&
        synth_print_generate_file(shape_dir, "program-callgraph-loop", 77, true, det_a) &&
        synth_print_generate_file(shape_dir, "program-callgraph-loop", 77, true, det_b)) {
      file_buf_t a = {0}, b = {0};
      det_ok = read_file(det_a, &a) && read_file(det_b, &b) &&
               a.len == b.len && memcmp(a.data, b.data, a.len) == 0;
      free(a.data);
      free(b.data);
    }
    (void)string_list_push_take(&rows, synth_print_determinism_row(root, det_ok, det_a, det_b));
    if (!det_ok)
      (void)string_list_push_take(&failures, make_worker_failure_row("synth-print", "determinism", 1, "", "same shape and seed did not reproduce identical C"));
    free(det_a);
    free(det_b);
    free(pair_out_dir);
    free(pair.name);
    free(pair.c_path);
    free(pair.ny_path);
    free(pair.ir_path);
    free(pair.json);
  }
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"cc\":");
  (void)sb_append_json_str(&extra, cc);
  (void)sb_append(&extra, ",\"ny_bin\":");
  if (has_ny) append_rel_json_str(&extra, root, ny_bin);
  else (void)sb_append_json_str(&extra, "");
  (void)sb_append(&extra, ",\"work_dir\":");
  append_rel_json_str(&extra, root, work_dir ? work_dir : "");
  char *report_json = build_native_report_json(&rows, &failures, "synth-print", extra.data ? extra.data : "");
  int rc = emit_native_report(report_json, json_path, "synth print", rows.count, failures.count);
  free(extra.data);
  free(shape_dir);
  free(work_dir);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static char *synth_schedule_row_json(const char *name, bool ok, int cases,
                                     int unique_shapes, int unique_generators,
                                     const char *schedule, const char *note) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"name\":");
  (void)sb_append_json_str(&b, name ? name : "");
  (void)sb_append(&b, ",\"kind\":\"synth-schedule\",\"ok\":");
  (void)sb_append(&b, ok ? "true" : "false");
  (void)sb_appendf(&b, ",\"cases\":%d,\"unique_shapes\":%d,"
                       "\"unique_generators\":%d,\"schedule\":",
                   cases, unique_shapes, unique_generators);
  (void)sb_append_json_str(&b, schedule ? schedule : "smart");
  (void)sb_append(&b, ",\"note\":");
  (void)sb_append_json_str(&b, note ? note : "");
  (void)sb_append(&b, ",\"engine\":\"nynth_core\"}");
  return sb_take(&b);
}

static int json_array_unique_string_field_count(const char *json, const char *array_key,
                                                const char *field_key) {
  const char *items = json_top_level_value_after_key(json ? json : "", array_key);
  if (!items || *items != '[') return 0;
  const char *end = matching_json_end(items, '[', ']');
  if (!end) return 0;
  string_list_t keys = {0};
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
    char *value = json_extract_string_range(p, obj_end + 1, field_key);
    if (value && *value) (void)string_list_push_unique_copy(&keys, value);
    free(value);
    p = obj_end + 1;
  }
  int count = keys.count;
  string_list_free(&keys);
  return count;
}

static int synth_schedule_run_batch(const char *root, const char *work_dir,
                                    const char *leaf, const char *schedule,
                                    int cases, int seed,
                                    generated_case_list_t *out,
                                    proc_result_t *pr_out) {
  char cases_buf[32], seed_buf[32];
  snprintf(cases_buf, sizeof(cases_buf), "%d", cases);
  snprintf(seed_buf, sizeof(seed_buf), "%d", seed);
  char *out_dir = NULL;
  if (asprintf(&out_dir, "%s/%s", work_dir ? work_dir : "/tmp", leaf ? leaf : "batch") < 0)
    return 2;
  char *argv[] = {
    g_self_path, "generate-batch",
    "--shape-dir", "shapes",
    "--profile", "optimizer",
    "--generator", "mixed",
    "--schedule", (char *)(schedule ? schedule : "smart"),
    "--seed", seed_buf,
    "--cases", cases_buf,
    "--out", out_dir,
    NULL
  };
  *pr_out = run_proc(argv, root, 60.0);
  int rc = 1;
  if (pr_out->rc == 0 && pr_out->out && parse_generated_cases(pr_out->out, out))
    rc = 0;
  free(out_dir);
  return rc;
}

static int cmd_public_selftest_synth_schedule(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *scratch_root_arg = nynth_scratch_root_arg(argc, argv, 3);
  char *scratch_root = nynth_absolute_scratch_root(scratch_root_arg);
  char *work_dir = NULL;
  bool path_ok = asprintf(&work_dir, "%s/nynth_synth_schedule_%ld",
                          scratch_root && *scratch_root ? scratch_root : NYNTH_DEFAULT_SCRATCH_ROOT,
                          (long)getpid()) >= 0;
  string_list_t rows = {0}, failures = {0};
  if (!path_ok || !work_dir || !mkdir_p(work_dir)) {
    (void)string_list_push_take(&rows, synth_schedule_row_json("prepare", false, 0, 0, 0,
                                                              "smart", "scratch workdir failed"));
    (void)string_list_push_take(&failures, make_worker_failure_row("synth-schedule", "prepare",
                                                                  1, "", "scratch workdir failed"));
  } else {
    char *list_argv[] = {
      g_self_path, "generate-batch", "--shape-dir", "shapes",
      "--profile", "optimizer", "--generator", "mixed",
      "--schedule", "smart", "--list", NULL
    };
    proc_result_t list_pr = run_proc(list_argv, root, 30.0);
    double pool_count_d = 0.0;
    bool list_ok = list_pr.rc == 0 && list_pr.out &&
                   extract_json_number(list_pr.out, "count", &pool_count_d) &&
                   pool_count_d >= 8.0;
    int pool_count = list_ok ? (int)pool_count_d : 0;
    int pool_unique_shapes = list_ok ? json_array_unique_string_field_count(list_pr.out, "shapes", "name") : 0;
    int pool_unique_generators = list_ok ? json_array_unique_string_field_count(list_pr.out, "shapes", "generator") : 0;
    list_ok = list_ok && pool_unique_shapes == pool_count && pool_unique_generators >= 4;
    (void)string_list_push_take(&rows, synth_schedule_row_json("shape_pool", list_ok,
                                                              pool_count, pool_unique_shapes,
                                                              pool_unique_generators,
                                                              "smart", "mixed schedule pool"));
    if (!list_ok)
      (void)string_list_push_take(&failures, make_worker_failure_row("synth-schedule-list",
                                                                    "generate-batch-list",
                                                                    list_pr.rc, list_pr.out,
                                                                    list_pr.err));
    proc_result_free(&list_pr);

    if (pool_count > 0) {
      generated_case_list_t epoch = {0};
      proc_result_t epoch_pr = {0};
      int epoch_rc = synth_schedule_run_batch(root, work_dir, "smart_epoch",
                                              "smart", pool_count, 101,
                                              &epoch, &epoch_pr);
      int unique_shapes = generated_case_unique_field_count(&epoch, "shape");
      int unique_generators = generated_case_unique_field_count(&epoch, "generator");
      bool epoch_ok = epoch_rc == 0 && epoch.count == pool_count &&
                      unique_shapes == epoch.count && unique_generators >= 4;
      (void)string_list_push_take(&rows, synth_schedule_row_json("smart_epoch_unique",
                                                                epoch_ok, epoch.count,
                                                                unique_shapes,
                                                                unique_generators,
                                                                "smart",
                                                                "one full smart epoch should not repeat shapes"));
      if (!epoch_ok)
        (void)string_list_push_take(&failures, make_worker_failure_row("smart-epoch",
                                                                      "synth-schedule",
                                                                      epoch_pr.rc,
                                                                      epoch_pr.out,
                                                                      epoch_pr.err && *epoch_pr.err
                                                                          ? epoch_pr.err
                                                                          : "smart epoch did not cover unique mixed shapes"));
      generated_case_list_free(&epoch);
      proc_result_free(&epoch_pr);

      generated_case_list_t short_mix = {0};
      proc_result_t short_pr = {0};
      int short_rc = synth_schedule_run_batch(root, work_dir, "smart_short",
                                              "smart", 8, 101,
                                              &short_mix, &short_pr);
      int short_shapes = generated_case_unique_field_count(&short_mix, "shape");
      int short_generators = generated_case_unique_field_count(&short_mix, "generator");
      bool short_ok = short_rc == 0 && short_mix.count == 8 &&
                      short_shapes == 8 && short_generators >= 4;
      (void)string_list_push_take(&rows, synth_schedule_row_json("smart_short_mix",
                                                                short_ok, short_mix.count,
                                                                short_shapes,
                                                                short_generators,
                                                                "smart",
                                                                "short smart runs should fan out across generators"));
      if (!short_ok)
        (void)string_list_push_take(&failures, make_worker_failure_row("smart-short",
                                                                      "synth-schedule",
                                                                      short_pr.rc,
                                                                      short_pr.out,
                                                                      short_pr.err && *short_pr.err
                                                                          ? short_pr.err
                                                                          : "short smart run collapsed coverage"));
      generated_case_list_free(&short_mix);
      proc_result_free(&short_pr);

      generated_case_list_t det_a = {0}, det_b = {0};
      proc_result_t det_pr_a = {0}, det_pr_b = {0};
      int det_rc_a = synth_schedule_run_batch(root, work_dir, "det_a", "smart",
                                              16, 31337, &det_a, &det_pr_a);
      int det_rc_b = synth_schedule_run_batch(root, work_dir, "det_b", "smart",
                                              16, 31337, &det_b, &det_pr_b);
      char *fp_a = det_rc_a == 0 ? generated_cases_fingerprint(&det_a) : NULL;
      char *fp_b = det_rc_b == 0 ? generated_cases_fingerprint(&det_b) : NULL;
      bool det_ok = det_rc_a == 0 && det_rc_b == 0 &&
                    strcmp(fp_a ? fp_a : "", fp_b ? fp_b : "") == 0;
      (void)string_list_push_take(&rows, synth_schedule_row_json("smart_deterministic",
                                                                det_ok, det_a.count,
                                                                generated_case_unique_field_count(&det_a, "shape"),
                                                                generated_case_unique_field_count(&det_a, "generator"),
                                                                "smart",
                                                                "same seed and schedule should reproduce files"));
      if (!det_ok)
        (void)string_list_push_take(&failures, make_worker_failure_row("smart-determinism",
                                                                      "synth-schedule",
                                                                      det_pr_a.rc ? det_pr_a.rc : det_pr_b.rc,
                                                                      det_pr_a.out ? det_pr_a.out : det_pr_b.out,
                                                                      "smart schedule was not deterministic"));
      free(fp_a);
      free(fp_b);
      generated_case_list_free(&det_a);
      generated_case_list_free(&det_b);
      proc_result_free(&det_pr_a);
      proc_result_free(&det_pr_b);

      generated_case_list_t weighted = {0};
      proc_result_t weighted_pr = {0};
      int weighted_rc = synth_schedule_run_batch(root, work_dir, "weighted_smoke",
                                                 "weighted", 8, 202,
                                                 &weighted, &weighted_pr);
      bool weighted_ok = weighted_rc == 0 && weighted.count == 8 &&
                         generated_case_unique_field_count(&weighted, "shape") > 0;
      (void)string_list_push_take(&rows, synth_schedule_row_json("weighted_smoke",
                                                                weighted_ok, weighted.count,
                                                                generated_case_unique_field_count(&weighted, "shape"),
                                                                generated_case_unique_field_count(&weighted, "generator"),
                                                                "weighted",
                                                                "weighted schedule remains available"));
      if (!weighted_ok)
        (void)string_list_push_take(&failures, make_worker_failure_row("weighted-smoke",
                                                                      "synth-schedule",
                                                                      weighted_pr.rc,
                                                                      weighted_pr.out,
                                                                      weighted_pr.err));
      generated_case_list_free(&weighted);
      proc_result_free(&weighted_pr);
    }
  }
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"work_dir\":");
  append_rel_json_str(&extra, root, work_dir ? work_dir : "");
  char *report_json = build_native_report_json(&rows, &failures,
                                               "synth-schedule", extra.data ? extra.data : "");
  int rc = emit_native_report(report_json, json_path, "synth schedule", rows.count, failures.count);
  free(extra.data);
  free(work_dir);
  free(scratch_root);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static char *synth_pure_row_json(const char *root, const char *shape, const char *feature,
                                 bool ok, int seed, const char *report_path,
                                 const char *note) {
  str_buf_t b = {0};
  (void)sb_append(&b, "{\"name\":");
  (void)sb_append_json_str(&b, shape ? shape : "");
  (void)sb_append(&b, ",\"kind\":\"synth-pure\",\"ok\":");
  (void)sb_append(&b, ok ? "true" : "false");
  (void)sb_append(&b, ",\"shape\":");
  (void)sb_append_json_str(&b, shape ? shape : "");
  (void)sb_append(&b, ",\"feature\":");
  (void)sb_append_json_str(&b, feature ? feature : "");
  (void)sb_appendf(&b, ",\"seed\":%d,\"report\":", seed);
  append_rel_json_str(&b, root, report_path ? report_path : "");
  (void)sb_append(&b, ",\"note\":");
  (void)sb_append_json_str(&b, note ? note : "");
  (void)sb_append(&b, ",\"engine\":\"nynth_core\"}");
  return sb_take(&b);
}

static int cmd_public_selftest_synth_pure(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *scratch_root_arg = nynth_scratch_root_arg(argc, argv, 3);
  char *scratch_root = nynth_absolute_scratch_root(scratch_root_arg);
  char *work_dir = NULL;
  bool path_ok = asprintf(&work_dir, "%s/nynth_synth_pure_%ld",
                          scratch_root && *scratch_root ? scratch_root : NYNTH_DEFAULT_SCRATCH_ROOT,
                          (long)getpid()) >= 0;
  string_list_t rows = {0}, failures = {0};
  if (!path_ok || !work_dir || !mkdir_p(work_dir)) {
    (void)string_list_push_take(&rows, synth_pure_row_json(root, "prepare", "pure-functions",
                                                          false, 0, "", "scratch workdir failed"));
    (void)string_list_push_take(&failures, make_worker_failure_row("synth-pure", "prepare",
                                                                  1, "", "scratch workdir failed"));
  } else {
    typedef struct {
      const char *shape;
      const char *feature;
      int seed;
    } synth_pure_case_t;
    static const synth_pure_case_t pure_cases[] = {
      {"optimizer-pure-call-cse", "call-cse", 5150},
      {"optimizer-pure-array-read-cse", "array-read-cse", 6101},
      {"optimizer-pure-branch-compose", "branch-compose", 7127},
    };
    for (int i = 0; i < (int)(sizeof(pure_cases) / sizeof(pure_cases[0])); ++i) {
      const synth_pure_case_t *tc = &pure_cases[i];
      char seed_buf[32];
      snprintf(seed_buf, sizeof(seed_buf), "%d", tc->seed);
      char *out_dir = NULL, *build_dir = NULL, *report_path = NULL;
      bool case_paths_ok = asprintf(&out_dir, "%s/%s/out", work_dir, tc->shape) >= 0 &&
                           asprintf(&build_dir, "%s/%s/build", work_dir, tc->shape) >= 0 &&
                           asprintf(&report_path, "%s/%s/report.json", work_dir, tc->shape) >= 0;
      proc_result_t pr = {0};
      file_buf_t report = {0};
      bool have_report = false, has_pure = false, has_feature = false, has_shape = false;
      bool report_has_failures = false;
      if (case_paths_ok && out_dir && build_dir && report_path) {
        char *pure_argv[] = {
          g_self_path, "synth", "generate",
          "--fast",
          "--generator", "optimizer",
          "--shape", (char *)tc->shape,
          "--profile", "optimizer",
          "--cases", "1",
          "--seed", seed_buf,
          "--runs", "1",
          "--warmup", "0",
          "--timeout-s", "90",
          "--out", out_dir,
          "--build-dir", build_dir,
          "--capture-failures",
          "--strict-failures",
          "--max-reduce-checks", "80",
          "--json", report_path,
          NULL
        };
        pr = run_proc(pure_argv, root, 120.0);
        have_report = read_file(report_path, &report) && report.data;
        report_has_failures = have_report && json_failures_nonempty(report.data);
        has_pure = have_report && strstr(report.data, "pure-functions");
        has_feature = have_report && strstr(report.data, tc->feature);
        has_shape = have_report && strstr(report.data, tc->shape);
      }
      const char *note = "pure synth generate comparison";
      if (!case_paths_ok) note = "path allocation failed";
      else if (pr.rc != 0) note = "synth generate failed";
      else if (!have_report) note = "report missing";
      else if (report_has_failures) note = "report contains failures";
      else if (!has_shape) note = "shape missing from report";
      else if (!has_pure) note = "pure-functions feature missing";
      else if (!has_feature) note = "expected pure feature missing";
      bool ok = case_paths_ok && pr.rc == 0 && have_report && !report_has_failures &&
                has_shape && has_pure && has_feature;
      (void)string_list_push_take(&rows, synth_pure_row_json(root, tc->shape, tc->feature,
                                                            ok, tc->seed, report_path, note));
      if (!ok) {
        (void)string_list_push_take(&failures, make_worker_failure_row(tc->shape, "synth-pure",
                                                                      pr.rc ? pr.rc : 1,
                                                                      pr.out,
                                                                      pr.err && *pr.err ? pr.err : note));
      }
      free(report.data);
      proc_result_free(&pr);
      free(out_dir);
      free(build_dir);
      free(report_path);
    }
  }
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"work_dir\":");
  append_rel_json_str(&extra, root, work_dir ? work_dir : "");
  char *report_json = build_native_report_json(&rows, &failures,
                                               "synth-pure", extra.data ? extra.data : "");
  int rc = emit_native_report(report_json, json_path, "synth pure", rows.count, failures.count);
  free(extra.data);
  free(work_dir);
  free(scratch_root);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static bool triage_item_list_push_take(triage_item_list_t *list, triage_item_t item) {
  if (list->count == list->cap) {
    int next_cap = list->cap ? list->cap * 2 : 16;
    triage_item_t *next = (triage_item_t *)realloc(list->items, (size_t)next_cap * sizeof(*next));
    if (!next) return false;
    list->items = next;
    list->cap = next_cap;
  }
  list->items[list->count++] = item;
  return true;
}

static void triage_item_free(triage_item_t *item) {
  if (!item) return;
  free(item->row);
  free(item->case_name);
  free(item->ny_source);
  free(item->c_source);
  memset(item, 0, sizeof(*item));
}

static void triage_item_list_free(triage_item_list_t *list) {
  for (int i = 0; i < list->count; ++i) {
    triage_item_free(&list->items[i]);
  }
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static bool triage_item_from_bench_row(const char *row, int default_runs,
                                       int default_warmup,
                                       triage_item_t *item) {
  if (!row || !item) return false;
  memset(item, 0, sizeof(*item));
  item->runs = default_runs;
  item->warmup = default_warmup;
  item->row = strdup(row);
  item->case_name = json_string_or_empty(row, "case");
  item->ny_source = json_string_or_empty(row, "ny_source");
  item->c_source = json_string_or_empty(row, "c_source");
  item->ok = json_bool_field(row, "ok", false);
  if (!extract_json_number(row, "ny_peak_vs_c_o3_run", &item->ratio) &&
      !extract_json_number(row, "ny_o3_vs_c_o3_run", &item->ratio))
    (void)extract_json_number(row, "ny_vs_c_elapsed_ns", &item->ratio);
  double number = 0.0;
  if (extract_json_number(row, "runs", &number)) item->runs = (int)number;
  if (extract_json_number(row, "warmup", &number)) item->warmup = (int)number;
  if (extract_json_number(row, "c_elapsed_ns", &number)) item->c_elapsed_ns = number;
  if (extract_json_number(row, "ny_elapsed_ns", &number)) item->ny_elapsed_ns = number;
  if (extract_json_number(row, "c_instructions", &number)) item->c_instructions = number;
  if (extract_json_number(row, "ny_instructions", &number)) item->ny_instructions = number;
  item->initial_ratio = item->ratio;
  if (!item->row) {
    triage_item_free(item);
    return false;
  }
  return true;
}

static triage_item_t *triage_item_list_find_case(triage_item_list_t *list,
                                                 const char *case_name) {
  if (!list || !case_name) return NULL;
  for (int i = 0; i < list->count; ++i) {
    if (strcmp(list->items[i].case_name ? list->items[i].case_name : "",
               case_name) == 0)
      return &list->items[i];
  }
  return NULL;
}

static int triage_cmp_desc(const void *a, const void *b) {
  const triage_item_t *x = (const triage_item_t *)a;
  const triage_item_t *y = (const triage_item_t *)b;
  if (x->ok != y->ok) return x->ok ? 1 : -1;
  return (x->ratio < y->ratio) - (x->ratio > y->ratio);
}

static bool byte_buf_append(byte_buf_t *b, const char *data, size_t len) {
  if (!len) return true;
  if (len > SIZE_MAX - b->len - 1u) return false;
  if (b->len + len + 1u > b->cap) {
    size_t next = b->cap ? b->cap * 2u : 4096u;
    while (next < b->len + len + 1u) next *= 2u;
    char *p = (char *)realloc(b->data, next);
    if (!p) return false;
    b->data = p;
    b->cap = next;
  }
  memcpy(b->data + b->len, data, len);
  b->len += len;
  b->data[b->len] = '\0';
  return true;
}

static proc_result_t run_nytrix_proc(char *const argv[], const char *root,
                                     double timeout_s) {
  const char *old_root_env = getenv("NYTRIX_ROOT");
  char *old_root = old_root_env ? strdup(old_root_env) : NULL;
  bool had_old_root = old_root_env != NULL;
  if (root && *root) (void)setenv("NYTRIX_ROOT", root, 1);
  proc_result_t pr = run_proc(argv, root, timeout_s);
  if (had_old_root) (void)setenv("NYTRIX_ROOT", old_root ? old_root : "", 1);
  else (void)unsetenv("NYTRIX_ROOT");
  free(old_root);
  return pr;
}

static bool compile_or_run_ny_source_flavor_once(const char *root, const char *ny_bin,
                                                 const char *path, bool run,
                                                 const char *flavor, double timeout_s,
                                                 proc_result_t *out) {
  if (run) {
    char *argv[] = {(char *)ny_bin, "--compiler-asserts", "-run", (char *)path, NULL};
    *out = run_nytrix_proc(argv, root, timeout_s);
  } else {
    const char *opt = "-O0";
    bool insane = false;
    if (flavor && (strcmp(flavor, "o3") == 0 || strcmp(flavor, "o3i") == 0)) opt = "-O3";
    if (flavor && strcmp(flavor, "o3i") == 0) insane = true;
    if (flavor && *flavor) {
      char *elf = NULL;
      elf = nynth_scratch_pathf(NULL, "compile/nynth_compile_%ld.elf",
                                (long)getpid());
      if (elf) (void)mkdir_parent(elf);
      if (!elf) {
        memset(out, 0, sizeof(*out));
        out->rc = 1;
        out->err = strdup("temporary compiler output path allocation failed");
        return false;
      }
      char *argv_plain[] = {(char *)ny_bin, "--compiler-asserts", (char *)opt,
                            "-o", elf, (char *)path, NULL};
      char *argv_insane[] = {(char *)ny_bin, "--compiler-asserts", (char *)opt,
                             "--profile=peak", "-o", elf, (char *)path, NULL};
      *out = run_nytrix_proc(insane ? argv_insane : argv_plain, root, timeout_s);
      if (elf) unlink(elf);
      free(elf);
    } else {
      char *argv_plain[] = {(char *)ny_bin, "--compiler-asserts", (char *)opt,
                            "-emit-only", (char *)path, NULL};
      *out = run_nytrix_proc(argv_plain, root, timeout_s);
    }
  }
  return out->rc == 0;
}

static bool symbol_suffix_match(const char *sym, size_t len,
                                const char *suffix) {
  size_t slen = suffix ? strlen(suffix) : 0;
  return sym && suffix && slen > 0 && len >= slen &&
         memcmp(sym + len - slen, suffix, slen) == 0;
}

static bool symbol_contains_match(const char *sym, size_t len,
                                  const char *needle) {
  size_t nlen = needle ? strlen(needle) : 0;
  if (!sym || !needle || nlen == 0 || len < nlen) return false;
  for (size_t i = 0; i + nlen <= len; ++i)
    if (memcmp(sym + i, needle, nlen) == 0) return true;
  return false;
}

static bool nytrix_generated_layout_symbol_name(const char *sym, size_t len) {
  if (!sym || len == 0) return false;
  return symbol_contains_match(sym, len, "_load_") ||
         symbol_suffix_match(sym, len, "_from") ||
         symbol_suffix_match(sym, len, "_store") ||
         symbol_suffix_match(sym, len, "_zero") ||
         symbol_suffix_match(sym, len, "_debug_str");
}

static bool nytrix_stale_layout_cache_error_text(const char *err) {
  const char *p = err;
  const char *needle = "undefined symbol:";
  if (!p) return false;
  while ((p = strstr(p, needle)) != NULL) {
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p == '"' || *p == '\'' || *p == '`') ++p;
    const char *start = p;
    while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '.')) ++p;
    if (nytrix_generated_layout_symbol_name(start, (size_t)(p - start)))
      return true;
  }
  return false;
}

static bool nytrix_stale_layout_cache_failure(const proc_result_t *pr) {
  return nytrix_stale_layout_cache_error_text(pr ? pr->err : NULL);
}

static bool nytrix_clean_cache_for_retry(const char *root, const char *ny_bin,
                                         double timeout_s) {
  char *argv[] = {(char *)ny_bin, "--clean-cache", NULL};
  double clean_timeout_s = timeout_s > 30.0 ? timeout_s : 30.0;
  proc_result_t pr = run_nytrix_proc(argv, root, clean_timeout_s);
  bool ok = pr.rc == 0;
  proc_result_free(&pr);
  return ok;
}

static bool compile_or_run_ny_source_flavor(const char *root, const char *ny_bin, const char *path,
                                            bool run, const char *flavor, double timeout_s,
                                            proc_result_t *out) {
  proc_result_t first = {0};
  bool ok = compile_or_run_ny_source_flavor_once(root, ny_bin, path, run,
                                                 flavor, timeout_s, &first);
  if (!ok && run && nytrix_stale_layout_cache_failure(&first) &&
      nytrix_clean_cache_for_retry(root, ny_bin, timeout_s)) {
    proc_result_free(&first);
    return compile_or_run_ny_source_flavor_once(root, ny_bin, path, run,
                                                flavor, timeout_s, out);
  }
  *out = first;
  return ok;
}

static int cmd_public_selftest_compiler_cache_classifier(int argc, char **argv) {
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  typedef struct {
    const char *name;
    const char *stderr_text;
    bool expected;
  } cache_classifier_case_t;
  static const cache_classifier_case_t cases[] = {
    {"layout_load", "mold: error: undefined symbol: PacketHeader_load_sender\n", true},
    {"layout_from", "mold: error: undefined symbol: RenderFrame_from\n", true},
    {"layout_store", "mold: error: undefined symbol: TextureInfo_store\n", true},
    {"layout_zero", "mold: error: undefined symbol: TextureInfo_zero\n", true},
    {"layout_debug", "mold: error: undefined symbol: HeaderInfo_debug_str\n", true},
    {"unrelated_c", "mold: error: undefined symbol: sqlite3_open\n", false},
    {"unrelated_runtime", "mold: error: undefined symbol: ny_runtime_boot\n", false},
    {"no_linker_marker", "warning: symbol PacketHeader_load_sender referenced\n", false}
  };
  string_list_t rows = {0}, failures = {0};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const cache_classifier_case_t *tc = &cases[i];
    bool got = nytrix_stale_layout_cache_error_text(tc->stderr_text);
    bool ok = got == tc->expected;
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"name\":");
    (void)sb_append_json_str(&row, tc->name);
    (void)sb_append(&row, ",\"kind\":\"compiler-cache-classifier\",\"ok\":");
    (void)sb_append(&row, ok ? "true" : "false");
    (void)sb_append(&row, ",\"engine\":\"nynth_core\",\"classified\":");
    (void)sb_append(&row, got ? "true" : "false");
    (void)sb_append(&row, ",\"expected\":");
    (void)sb_append(&row, tc->expected ? "true" : "false");
    (void)sb_append_c(&row, '}');
    (void)string_list_push_take(&rows, sb_take(&row));
    if (!ok)
      (void)string_list_push_take(
          &failures,
          make_worker_failure_row(tc->name, "compiler-cache-classifier", 1,
                                  "", "stale layout cache classifier mismatch"));
  }
  char *report = build_native_report_json(&rows, &failures,
                                          "compiler-cache-classifier", "");
  int rc = emit_native_report(report, json_path,
                              "selftest compiler cache classifier",
                              rows.count, failures.count);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static bool compile_or_run_ny_source(const char *root, const char *ny_bin, const char *path,
                                     bool run, double timeout_s, proc_result_t *out) {
  return compile_or_run_ny_source_flavor(root, ny_bin, path, run, "", timeout_s, out);
}

static bool compiler_smoke_wants(const char *selector, const char *name) {
  return !selector || !*selector || strcmp(selector, "all") == 0 || strcmp(selector, name) == 0;
}

static void compiler_smoke_add_case(const char *root, const char *ny_bin, const char *selector,
                                    const char *name, const char *rel_path, double timeout_s,
                                    string_list_t *rows, string_list_t *failures) {
  if (!compiler_smoke_wants(selector, name)) return;
  char *path = NULL;
  (void)asprintf(&path, "%s/%s", root, rel_path);
  proc_result_t pr = {0};
  bool ok = path && path_exists_file(path) &&
            compile_or_run_ny_source(root, ny_bin, path, true, timeout_s, &pr);
  if (!ok) {
    (void)string_list_push_take(failures,
                                make_worker_failure_row(name, "compiler-smoke",
                                                        path && path_exists_file(path) ? pr.rc : 1,
                                                        pr.out ? pr.out : "",
                                                        path && path_exists_file(path)
                                                            ? (pr.err ? pr.err : "")
                                                            : "runtime test source missing"));
  }
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, name);
  (void)sb_append(&row, ",\"kind\":\"compiler-smoke\",\"ok\":");
  (void)sb_append(&row, ok ? "true" : "false");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\",\"source\":");
  append_rel_json_str(&row, root, path ? path : "");
  (void)sb_appendf(&row, ",\"rc\":%d,\"elapsed_ms\":%.2f}", pr.rc, pr.elapsed_ms);
  (void)string_list_push_take(rows, sb_take(&row));
  proc_result_free(&pr);
  free(path);
}

static int cmd_public_compiler_smoke(int argc, char **argv) {
  char root[4096], ny_bin[4096];
  if (!find_repo_root(root, sizeof(root)) || !find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *selector = value_after(argc, argv, 3, "--only", "all");
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "90"));
  if (timeout_s <= 0.0) timeout_s = 90.0;
  string_list_t rows = {0}, failures = {0};
  compiler_smoke_add_case(root, ny_bin, selector, "type", "etc/tests/rt/type.ny",
                          timeout_s, &rows, &failures);
  compiler_smoke_add_case(root, ny_bin, selector, "simmd", "lib/math/simmd.ny",
                          timeout_s, &rows, &failures);
  if (rows.count == 0) {
    (void)string_list_push_take(&failures,
                                make_worker_failure_row(selector ? selector : "", "compiler-smoke",
                                                        1, "", "unknown compiler smoke selector"));
    (void)string_list_push_take(&rows,
                                native_row_status(selector ? selector : "", "compiler-smoke",
                                                  false, "reason", "unknown selector"));
  }
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"selector\":");
  (void)sb_append_json_str(&extra, selector ? selector : "all");
  (void)sb_append(&extra, ",\"ny_bin\":");
  append_rel_json_str(&extra, root, ny_bin);
  char *report = build_native_report_json(&rows, &failures, "compiler-smoke", extra.data);
  int rc = emit_native_report(report, json_path, "compiler smoke", rows.count, failures.count);
  free(extra.data);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

typedef struct {
  const char *id;
  const char *title;
  const char *source_rel;
  const char *good_output;
  const char *known_bad_output;
  const char *bad_flavor;
} compiler_known_bug_t;

typedef struct {
  const char *id;
  const char *title;
  const char *source_rel;
  const char *expected_signal;
  const char *expected_substring;
  const char *baseline_source_rel;
  double case_timeout_s;
} compiler_known_process_bug_t;

typedef struct {
  const char *id;
  const char *title;
  const char *mode;
  const char *source_rel;
  const char *target_rel;
  const char *expected_signal;
  double case_timeout_s;
} compiler_active_finding_t;

static bool compiler_selector_matches(const char *selector, const char *id) {
  if (!selector || !*selector || strcmp(selector, "all") == 0) return true;
  if (!id || !*id) return false;
  if (strcmp(selector, id) == 0) return true;
  size_t n = strlen(selector);
  return strncmp(selector, id, n) == 0 && id[n] == '-';
}

static int compiler_known_bugs_add_case(const char *root, const char *ny_bin,
                                        const compiler_known_bug_t *spec,
                                        const char *selector, const char *out_root,
                                        double timeout_s, bool strict_open,
                                        string_list_t *rows, string_list_t *failures,
                                        int *reproduced_count, int *fixed_count,
                                        int *lost_count, int *baseline_failure_count) {
  if (!spec || !spec->id || !*spec->id) return 0;
  if (selector && *selector && strcmp(selector, "all") != 0 &&
      strcmp(selector, spec->id) != 0)
    return 0;
  char source_path[4096];
  if (!path_join(source_path, sizeof(source_path), root, spec->source_rel) ||
      !path_exists_file(source_path)) {
    (void)string_list_push_take(failures,
                                make_worker_failure_row(spec->id, "compiler-known-bugs",
                                                        1, "", "known bug source missing"));
    (void)string_list_push_take(rows, native_row_status(spec->id, "compiler-known-bug",
                                                       false, "reason", "source missing"));
    if (lost_count) ++*lost_count;
    return 1;
  }
  char *case_dir = NULL;
  (void)asprintf(&case_dir, "%s/%s", out_root ? out_root : "/tmp", spec->id);
  if (case_dir) ny_ensure_dir_recursive(case_dir);

  static const char *flavors[] = {"o0", "o3", "o3_speed", "o3_peak"};
  str_buf_t variants = {0};
  bool baseline_failed = false;
  bool known_bug_reproduced = false;
  bool fixed_candidate = false;
  bool lost_signal = false;
  for (int i = 0; i < (int)(sizeof(flavors) / sizeof(flavors[0])); ++i) {
    const char *flavor = flavors[i];
    bool bad_flavor = spec->bad_flavor && strcmp(flavor, spec->bad_flavor) == 0;
    char *binary = NULL;
    (void)asprintf(&binary, "%s/%s_%s.elf", case_dir ? case_dir : "/tmp",
                   spec->id, flavor);
    char *compile_argv[12];
    int ca = 0;
    compile_argv[ca++] = (char *)ny_bin;
    compile_argv[ca++] = "--compiler-asserts";
    if (strcmp(flavor, "o3") == 0) {
      compile_argv[ca++] = "-O3";
    } else if (strcmp(flavor, "o3_speed") == 0) {
      compile_argv[ca++] = "-O3";
      compile_argv[ca++] = "--profile=speed";
    } else if (strcmp(flavor, "o3_peak") == 0) {
      compile_argv[ca++] = "-O3";
      compile_argv[ca++] = "--profile=peak";
    }
    compile_argv[ca++] = "-o";
    compile_argv[ca++] = binary;
    compile_argv[ca++] = source_path;
    compile_argv[ca] = NULL;
    proc_result_t compile = run_proc(compile_argv, root, timeout_s);
    proc_result_t run = {0};
    bool compile_ok = compile.rc == 0 && binary && path_exists_file(binary);
    bool run_ok = false;
    char *output = NULL;
    if (compile_ok) {
      char *run_argv[] = {binary, NULL};
      run = run_proc(run_argv, root, timeout_s);
      run_ok = run.rc == 0;
      output = trim_trailing_copy(run.out ? run.out : "");
    }
    bool matches_good = run_ok && output && strcmp(output, spec->good_output) == 0;
    bool matches_bad = run_ok && output && strcmp(output, spec->known_bad_output) == 0;
    if (!compile_ok || !run_ok) {
      if (bad_flavor) lost_signal = true;
      else baseline_failed = true;
    } else if (bad_flavor) {
      if (matches_bad && !matches_good) known_bug_reproduced = true;
      else if (matches_good) fixed_candidate = true;
      else lost_signal = true;
    } else if (!matches_good) {
      baseline_failed = true;
    }
    if (i) (void)sb_append_c(&variants, ',');
    (void)sb_append(&variants, "{\"flavor\":");
    (void)sb_append_json_str(&variants, flavor);
    (void)sb_appendf(&variants,
                     ",\"bad_flavor\":%s,\"compile_ok\":%s,\"run_ok\":%s,"
                     "\"compile_rc\":%d,\"run_rc\":%d,\"compile_ms\":%.2f,"
                     "\"run_ms\":%.2f,\"matches_good\":%s,\"matches_known_bad\":%s,"
                     "\"output\":",
                     bad_flavor ? "true" : "false",
                     compile_ok ? "true" : "false",
                     run_ok ? "true" : "false",
                     compile.rc, run.rc, compile.elapsed_ms, run.elapsed_ms,
                     matches_good ? "true" : "false",
                     matches_bad ? "true" : "false");
    (void)sb_append_json_str(&variants, output ? output : "");
    (void)sb_append(&variants, ",\"binary\":");
    append_rel_json_str(&variants, root, binary ? binary : "");
    (void)sb_append_c(&variants, '}');
    proc_result_free(&compile);
    proc_result_free(&run);
    free(output);
    free(binary);
  }

  const char *status = "lost_signal";
  bool ok = false;
  if (baseline_failed) {
    status = "baseline_failed";
    if (baseline_failure_count) ++*baseline_failure_count;
  } else if (known_bug_reproduced) {
    status = "known_bug_reproduced";
    ok = true;
    if (reproduced_count) ++*reproduced_count;
  } else if (fixed_candidate) {
    status = "fixed_candidate";
    ok = !strict_open;
    if (fixed_count) ++*fixed_count;
  } else {
    if (lost_count) ++*lost_count;
  }
  if (!ok) {
    (void)string_list_push_take(failures,
                                make_worker_failure_row(spec->id, "compiler-known-bugs",
                                                        1, "", status));
  }
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, spec->id);
  (void)sb_append(&row, ",\"kind\":\"compiler-known-bug\",\"ok\":");
  (void)sb_append(&row, ok ? "true" : "false");
  (void)sb_append(&row, ",\"status\":");
  (void)sb_append_json_str(&row, status);
  (void)sb_append(&row, ",\"title\":");
  (void)sb_append_json_str(&row, spec->title ? spec->title : "");
  (void)sb_append(&row, ",\"source\":");
  append_rel_json_str(&row, root, source_path);
  (void)sb_append(&row, ",\"good_output\":");
  (void)sb_append_json_str(&row, spec->good_output);
  (void)sb_append(&row, ",\"known_bad_output\":");
  (void)sb_append_json_str(&row, spec->known_bad_output);
  (void)sb_append(&row, ",\"bad_flavor\":");
  (void)sb_append_json_str(&row, spec->bad_flavor);
  (void)sb_appendf(&row,
                   ",\"known_bug_reproduced\":%s,\"fixed_candidate\":%s,"
                   "\"lost_signal\":%s,\"baseline_failed\":%s,"
                   "\"strict_open\":%s,\"engine\":\"nynth_core\",\"variants\":[",
                   known_bug_reproduced ? "true" : "false",
                   fixed_candidate ? "true" : "false",
                   lost_signal ? "true" : "false",
                   baseline_failed ? "true" : "false",
                   strict_open ? "true" : "false");
  if (variants.data) (void)sb_append(&row, variants.data);
  (void)sb_append(&row, "]}");
  (void)string_list_push_take(rows, sb_take(&row));
  free(variants.data);
  free(case_dir);
  return 1;
}

static int compiler_findings_add_case(const char *root, const char *ny_bin,
                                      const compiler_active_finding_t *spec,
                                      const char *selector, double timeout_s,
                                      string_list_t *rows, string_list_t *failures,
                                      int *live_count, int *cleared_count,
                                      int *missing_count) {
  if (!spec || !spec->id || !*spec->id) return 0;
  if (!compiler_selector_matches(selector, spec->id)) return 0;

  char source_path[4096];
  bool source_ok = path_join(source_path, sizeof(source_path), root, spec->source_rel) &&
                   path_exists_file(source_path);
  char target_path[4096] = "";
  bool needs_target = spec->target_rel && *spec->target_rel;
  bool target_ok = !needs_target ||
                   (path_join(target_path, sizeof(target_path), root, spec->target_rel) &&
                    path_exists_file(target_path));
  if (!source_ok || !target_ok) {
    if (missing_count) ++*missing_count;
    (void)string_list_push_take(failures,
                                make_worker_failure_row(spec->id, "compiler-findings",
                                                        1, "",
                                                        source_ok ? "target missing"
                                                                  : "source missing"));
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"name\":");
    (void)sb_append_json_str(&row, spec->id);
    (void)sb_append(&row, ",\"kind\":\"compiler-finding\",\"ok\":false");
    (void)sb_append(&row, ",\"status\":\"missing\",\"title\":");
    (void)sb_append_json_str(&row, spec->title ? spec->title : "");
    (void)sb_append(&row, ",\"source\":");
    append_rel_json_str(&row, root, source_ok ? source_path : spec->source_rel);
    (void)sb_append(&row, ",\"target\":");
    append_rel_json_str(&row, root, target_ok ? target_path : spec->target_rel);
    (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
    (void)string_list_push_take(rows, sb_take(&row));
    return 1;
  }

  double case_timeout_s = timeout_s;
  if (spec->case_timeout_s > 0.0 &&
      (case_timeout_s <= 0.0 || spec->case_timeout_s < case_timeout_s))
    case_timeout_s = spec->case_timeout_s;
  proc_result_t pr = {0};
  if (strcmp(spec->mode ? spec->mode : "", "syntax-target") == 0) {
    char *argv[] = {(char *)ny_bin, target_path, source_path, NULL};
    pr = run_proc(argv, root, case_timeout_s);
  } else {
    char *argv[] = {
      (char *)ny_bin, "--compiler-asserts", "-emit-only", source_path, NULL
    };
    pr = run_proc(argv, root, case_timeout_s);
  }

  bool expect_timeout = spec->expected_signal &&
                        strcmp(spec->expected_signal, "timeout") == 0;
  bool expect_crash = spec->expected_signal &&
                      strcmp(spec->expected_signal, "crash") == 0;
  bool live = (expect_timeout && pr.timed_out) ||
              (expect_crash && proc_result_crashed(&pr));
  const char *status = live ? "live" : "cleared";
  bool ok = !live;
  if (live) {
    if (live_count) ++*live_count;
    (void)string_list_push_take(failures,
                                make_worker_failure_row(spec->id, "compiler-findings",
                                                        pr.rc ? pr.rc : 1,
                                                        pr.out ? pr.out : "",
                                                        "active finding reproduced"));
  } else if (cleared_count) {
    ++*cleared_count;
  }

  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, spec->id);
  (void)sb_append(&row, ",\"kind\":\"compiler-finding\",\"ok\":");
  (void)sb_append(&row, ok ? "true" : "false");
  (void)sb_append(&row, ",\"status\":");
  (void)sb_append_json_str(&row, status);
  (void)sb_append(&row, ",\"title\":");
  (void)sb_append_json_str(&row, spec->title ? spec->title : "");
  (void)sb_append(&row, ",\"mode\":");
  (void)sb_append_json_str(&row, spec->mode ? spec->mode : "");
  (void)sb_append(&row, ",\"expected_signal\":");
  (void)sb_append_json_str(&row, spec->expected_signal ? spec->expected_signal : "");
  (void)sb_append(&row, ",\"source\":");
  append_rel_json_str(&row, root, source_path);
  (void)sb_append(&row, ",\"target\":");
  append_rel_json_str(&row, root, needs_target ? target_path : "");
  (void)sb_appendf(&row,
                   ",\"rc\":%d,\"timed_out\":%s,\"crashed\":%s,"
                   "\"elapsed_ms\":%.2f,\"timeout_s\":%.2f",
                   pr.rc, pr.timed_out ? "true" : "false",
                   proc_result_crashed(&pr) ? "true" : "false",
                   pr.elapsed_ms, case_timeout_s);
  append_proc_tail_fields(&row, &pr);
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  proc_result_free(&pr);
  return 1;
}

static int cmd_public_compiler_findings(int argc, char **argv) {
  char root[4096], ny_root[4096], ny_bin[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  if (!find_repo_root(ny_root, sizeof(ny_root)) || !find_ny_bin(ny_root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *selector = value_after(argc, argv, 3, "--only", "all");
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "15"));
  if (timeout_s <= 0.0) timeout_s = 15.0;
  static const compiler_active_finding_t specs[] = {
    {
      "NY-001-sig04",
      "syntax fuzz target crash seed",
      "syntax-target",
      "fuzz/work/afl_runs/syntax_1779469604_2801121/default/crashes/"
      "id:000000,sig:04,src:000005,time:615428,execs:264020,op:havoc,rep:2",
      "fuzz/targets/fuzz_syntax.ny",
      "crash",
      15.0
    },
    {
      "NY-001-sig11",
      "syntax fuzz target crash seed",
      "syntax-target",
      "fuzz/work/afl_runs/syntax_1779469604_2801121/default/crashes/"
      "id:000001,sig:11,src:000005,time:615542,execs:264021,op:havoc,rep:1",
      "fuzz/targets/fuzz_syntax.ny",
      "crash",
      15.0
    },
    {
      "NY-002-hang-a",
      "compiler emit-only hang seed",
      "emit-only",
      "fuzz/work/afl_runs/ny-core_1779470422_2801121/hangs/"
      "id:000000,src:000001,time:905478,execs:21077,op:havoc,rep:6",
      NULL,
      "timeout",
      15.0
    },
    {
      "NY-002-hang-b",
      "compiler emit-only hang seed",
      "emit-only",
      "fuzz/work/afl_runs/ny-core_1779447084_1097816/hangs/"
      "id:000000,src:000001,time:46143,execs:765,op:havoc,rep:30",
      NULL,
      "timeout",
      15.0
    },
    {
      "NY-002-hang-c",
      "compiler emit-only hang seed",
      "emit-only",
      "fuzz/work/afl_runs/ny-core_1779447084_1097816/hangs/"
      "id:000001,src:000001,time:61419,execs:934,op:havoc,rep:44",
      NULL,
      "timeout",
      15.0
    }
  };
  string_list_t rows = {0}, failures = {0};
  int matched = 0, live = 0, cleared = 0, missing = 0;
  for (int i = 0; i < (int)(sizeof(specs) / sizeof(specs[0])); ++i) {
    matched += compiler_findings_add_case(root, ny_bin, &specs[i], selector,
                                          timeout_s, &rows, &failures,
                                          &live, &cleared, &missing);
  }
  if (matched == 0) {
    (void)string_list_push_take(&failures,
                                make_worker_failure_row(selector ? selector : "",
                                                        "compiler-findings", 1, "",
                                                        "unknown finding selector"));
    (void)string_list_push_take(&rows,
                                native_row_status(selector ? selector : "",
                                                  "compiler-finding", false,
                                                  "reason", "unknown selector"));
  }
  str_buf_t extra = {0};
  (void)sb_appendf(&extra,
                   ",\"finding_count\":%d,\"live\":%d,\"cleared\":%d,"
                   "\"missing\":%d,\"selector\":",
                   matched, live, cleared, missing);
  (void)sb_append_json_str(&extra, selector ? selector : "all");
  (void)sb_append(&extra, ",\"ny_bin\":");
  append_rel_json_str(&extra, root, ny_bin);
  char *report = build_native_report_json(&rows, &failures,
                                          "compiler-findings", extra.data);
  int rc = emit_native_report(report, json_path, "compiler findings",
                              rows.count, failures.count);
  free(extra.data);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static int compiler_known_bugs_add_process_case(const char *root, const char *ny_bin,
                                                const compiler_known_process_bug_t *spec,
                                                const char *selector, double timeout_s,
                                                bool strict_open, string_list_t *rows,
                                                string_list_t *failures,
                                                int *reproduced_count,
                                                int *fixed_count, int *lost_count,
                                                int *baseline_failure_count) {
  if (!spec || !spec->id || !*spec->id) return 0;
  if (selector && *selector && strcmp(selector, "all") != 0 &&
      strcmp(selector, spec->id) != 0)
    return 0;
  char source_path[4096];
  if (!path_join(source_path, sizeof(source_path), root, spec->source_rel) ||
      !path_exists_file(source_path)) {
    (void)string_list_push_take(failures,
                                make_worker_failure_row(spec->id, "compiler-known-bugs",
                                                        1, "", "known bug source missing"));
    (void)string_list_push_take(rows, native_row_status(spec->id, "compiler-known-bug",
                                                       false, "reason", "source missing"));
    if (lost_count) ++*lost_count;
    return 1;
  }

  file_buf_t source = {0};
  if (!read_file(source_path, &source) || !source.data) {
    (void)string_list_push_take(failures,
                                make_worker_failure_row(spec->id, "compiler-known-bugs",
                                                        1, "", "known bug source unreadable"));
    (void)string_list_push_take(rows, native_row_status(spec->id, "compiler-known-bug",
                                                       false, "reason", "source unreadable"));
    free(source.data);
    if (lost_count) ++*lost_count;
    return 1;
  }

  double case_timeout_s = timeout_s;
  if (spec->case_timeout_s > 0.0 &&
      (case_timeout_s <= 0.0 || spec->case_timeout_s < case_timeout_s))
    case_timeout_s = spec->case_timeout_s;
  char *argv[] = {(char *)ny_bin, "--compiler-asserts", "-c", source.data, NULL};
  proc_result_t pr = run_proc(argv, root, case_timeout_s);
  char *normalized = normalize_output_pair(pr.out, pr.err);
  bool has_baseline = spec->baseline_source_rel && *spec->baseline_source_rel;
  bool baseline_failed = false;
  bool baseline_source_ok = !has_baseline;
  bool baseline_ok = !has_baseline;
  char baseline_source_path[4096] = "";
  file_buf_t baseline_source = {0};
  proc_result_t baseline_pr = {0};
  char *baseline_normalized = NULL;
  if (has_baseline) {
    baseline_source_ok = path_join(baseline_source_path, sizeof(baseline_source_path),
                                   root, spec->baseline_source_rel) &&
                         path_exists_file(baseline_source_path) &&
                         read_file(baseline_source_path, &baseline_source) &&
                         baseline_source.data;
    if (baseline_source_ok) {
      char *baseline_argv[] = {
        (char *)ny_bin, "--compiler-asserts", "-c", baseline_source.data, NULL
      };
      baseline_pr = run_proc(baseline_argv, root, case_timeout_s);
      baseline_ok = baseline_pr.rc == 0 && !baseline_pr.timed_out &&
                    !proc_result_crashed(&baseline_pr);
      baseline_normalized = normalize_output_pair(baseline_pr.out, baseline_pr.err);
    }
    baseline_failed = !baseline_source_ok || !baseline_ok;
  }
  bool expected_timeout = spec->expected_signal &&
                          strcmp(spec->expected_signal, "timeout") == 0;
  bool expected_crash = spec->expected_signal &&
                        strcmp(spec->expected_signal, "crash") == 0;
  bool expected_compile_error = spec->expected_signal &&
                                strcmp(spec->expected_signal, "compile_error") == 0;
  bool substring_matches = !spec->expected_substring || !*spec->expected_substring ||
                           (normalized && strstr(normalized, spec->expected_substring));
  bool expected_reproduced = (expected_timeout && pr.timed_out) ||
                             (expected_crash && proc_result_crashed(&pr)) ||
                             (expected_compile_error && pr.rc != 0 && !pr.timed_out &&
                              !proc_result_crashed(&pr) && substring_matches);
  bool fixed_candidate = !expected_reproduced && !pr.timed_out && pr.rc == 0;
  bool lost_signal = !expected_reproduced && !fixed_candidate;
  bool ok = false;
  const char *status = "lost_signal";
  if (baseline_failed) {
    status = "baseline_failed";
    if (baseline_failure_count) ++*baseline_failure_count;
  } else if (expected_reproduced) {
    status = "known_bug_reproduced";
    ok = true;
    if (reproduced_count) ++*reproduced_count;
  } else if (fixed_candidate) {
    status = "fixed_candidate";
    ok = !strict_open;
    if (fixed_count) ++*fixed_count;
  } else {
    if (lost_count) ++*lost_count;
  }
  if (!ok) {
    const char *failure_err = baseline_failed ? "baseline_failed" : status;
    (void)string_list_push_take(failures,
                                make_worker_failure_row(spec->id, "compiler-known-bugs",
                                                        pr.rc ? pr.rc : 1, pr.out,
                                                        failure_err));
  }

  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, spec->id);
  (void)sb_append(&row, ",\"kind\":\"compiler-known-bug\",\"ok\":");
  (void)sb_append(&row, ok ? "true" : "false");
  (void)sb_append(&row, ",\"status\":");
  (void)sb_append_json_str(&row, status);
  (void)sb_append(&row, ",\"title\":");
  (void)sb_append_json_str(&row, spec->title ? spec->title : "");
  (void)sb_append(&row, ",\"source\":");
  append_rel_json_str(&row, root, source_path);
  (void)sb_append(&row, ",\"source_mode\":\"inline-c\",\"expected_signal\":");
  (void)sb_append_json_str(&row, spec->expected_signal ? spec->expected_signal : "");
  (void)sb_append(&row, ",\"expected_substring\":");
  (void)sb_append_json_str(&row, spec->expected_substring ? spec->expected_substring : "");
  (void)sb_append(&row, ",\"baseline_source\":");
  if (has_baseline && baseline_source_path[0])
    append_rel_json_str(&row, root, baseline_source_path);
  else
    (void)sb_append_json_str(&row, "");
  (void)sb_appendf(&row,
                   ",\"known_bug_reproduced\":%s,\"fixed_candidate\":%s,"
                   "\"lost_signal\":%s,\"baseline_failed\":%s,"
                   "\"baseline_source_ok\":%s,\"baseline_ok\":%s,"
                   "\"baseline_rc\":%d,\"baseline_timed_out\":%s,"
                   "\"baseline_elapsed_ms\":%.2f,\"strict_open\":%s,"
                   "\"rc\":%d,\"timed_out\":%s,\"elapsed_ms\":%.2f,"
                   "\"timeout_s\":%.2f,\"baseline_output\":",
                   expected_reproduced ? "true" : "false",
                   fixed_candidate ? "true" : "false",
                   lost_signal ? "true" : "false",
                   baseline_failed ? "true" : "false",
                   baseline_source_ok ? "true" : "false",
                   baseline_ok ? "true" : "false",
                   baseline_pr.rc,
                   baseline_pr.timed_out ? "true" : "false",
                   baseline_pr.elapsed_ms,
                   strict_open ? "true" : "false",
                   pr.rc, pr.timed_out ? "true" : "false", pr.elapsed_ms,
                   case_timeout_s);
  (void)sb_append_json_str(&row, baseline_normalized ? baseline_normalized : "");
  (void)sb_append(&row, ",\"output\":");
  (void)sb_append_json_str(&row, normalized ? normalized : "");
  append_proc_tail_fields(&row, &pr);
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  free(baseline_normalized);
  free(normalized);
  proc_result_free(&baseline_pr);
  proc_result_free(&pr);
  free(baseline_source.data);
  free(source.data);
  return 1;
}

static int cmd_public_compiler_known_bugs(int argc, char **argv) {
  char root[4096], ny_root[4096], ny_bin[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  if (!find_repo_root(ny_root, sizeof(ny_root)) || !find_ny_bin(ny_root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *selector = value_after(argc, argv, 3, "--only", "all");
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "30"));
  if (timeout_s <= 0.0) timeout_s = 30.0;
  bool strict_open = has_flag_after(argc, argv, 3, "--strict-open") ||
                     has_flag_after(argc, argv, 3, "--fail-on-fixed");
  char *out_root = NULL;
  (void)asprintf(&out_root, "%s/build/repro/known_bugs", root);
  if (out_root) ny_ensure_dir_recursive(out_root);
  static const compiler_known_bug_t specs[] = {
    {
      "NY-008",
      "peak profile stale def snapshot after mutable source update",
      "repro/nytrix/NY-008-peak-def-snapshot.ny",
      "-1",
      "0",
      "o3_peak"
    }
  };
  static const compiler_known_process_bug_t process_specs[] = {
    {
      "NY-006",
      "std.os.args.argv out-of-range runtime crash",
      "repro/nytrix/NY-006-argv-oob.ny",
      "crash",
      NULL,
      NULL,
      8.0
    },
    {
      "NY-007",
      "std.math.parse.syntax CMake tokenizer timeout on tiny seed",
      "repro/nytrix/NY-007-cmake-tokenizer-hang.ny",
      "timeout",
      NULL,
      NULL,
      5.0
    },
    {
      "NY-005",
      "bare std.core.dict import fails without prior std.core import",
      "repro/nytrix/NY-005-bare-dict-import.ny",
      "compile_error",
      "Could not load std.ny or standard library source files",
      "repro/nytrix/NY-005-dict-import-with-core-baseline.ny",
      8.0
    }
  };
  string_list_t rows = {0}, failures = {0};
  int matched = 0, reproduced = 0, fixed = 0, lost = 0, baseline_failed = 0;
  for (int i = 0; i < (int)(sizeof(specs) / sizeof(specs[0])); ++i) {
    matched += compiler_known_bugs_add_case(root, ny_bin, &specs[i], selector, out_root,
                                           timeout_s, strict_open, &rows, &failures,
                                           &reproduced, &fixed, &lost, &baseline_failed);
  }
  for (int i = 0; i < (int)(sizeof(process_specs) / sizeof(process_specs[0])); ++i) {
    matched += compiler_known_bugs_add_process_case(root, ny_bin, &process_specs[i],
                                                    selector, timeout_s, strict_open,
                                                    &rows, &failures, &reproduced,
                                                    &fixed, &lost,
                                                    &baseline_failed);
  }
  if (matched == 0) {
    (void)string_list_push_take(&failures,
                                make_worker_failure_row(selector ? selector : "",
                                                        "compiler-known-bugs", 1, "",
                                                        "unknown known-bug selector"));
    (void)string_list_push_take(&rows,
                                native_row_status(selector ? selector : "",
                                                  "compiler-known-bug", false,
                                                  "reason", "unknown selector"));
  }
  str_buf_t extra = {0};
  (void)sb_appendf(&extra,
                   ",\"known_bug_count\":%d,\"reproduced\":%d,"
                   "\"fixed_candidates\":%d,\"lost_signal\":%d,"
                   "\"baseline_failures\":%d,"
                   "\"known_bug_reproduced\":%d,"
                   "\"known_bug_fixed_candidates\":%d,"
                   "\"known_bug_lost_signal\":%d,"
                   "\"known_bug_baseline_failures\":%d,"
                   "\"strict_open\":%s,\"ny_bin\":",
                   matched, reproduced, fixed, lost, baseline_failed,
                   reproduced, fixed, lost, baseline_failed,
                   strict_open ? "true" : "false");
  append_rel_json_str(&extra, root, ny_bin);
  (void)sb_append(&extra, ",\"out_dir\":");
  append_rel_json_str(&extra, root, out_root ? out_root : "");
  char *report = build_native_report_json_with_top_aliases(
      &rows, &failures, "compiler-known-bugs", extra.data, true);
  int rc = emit_native_report(report, json_path, "compiler known bugs", rows.count, failures.count);
  free(extra.data);
  free(out_root);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static bool magic_name_char(unsigned char c) {
  return isalnum(c) || c == '_';
}

static void audit_push_unique_or_duplicate(string_list_t *items, string_list_t *duplicates,
                                           const char *name) {
  if (!name || !*name) return;
  if (string_list_contains(items, name)) {
    (void)string_list_push_unique_copy(duplicates, name);
    return;
  }
  (void)string_list_push_copy(items, name);
}

static void audit_extract_runtime_defs(const char *text, string_list_t *defs,
                                       string_list_t *duplicates) {
  const char *p = text ? text : "";
  while ((p = strstr(p, "RT_DEF(\"")) != NULL) {
    p += strlen("RT_DEF(\"");
    const char *q = strchr(p, '"');
    if (!q) break;
    char *name = strndup_local(p, (size_t)(q - p));
    if (name) {
      audit_push_unique_or_duplicate(defs, duplicates, name);
      free(name);
    }
    p = q + 1;
  }
}

static void audit_extract_magic_refs(const char *text, string_list_t *refs) {
  const char *p = text ? text : "";
  while ((p = strstr(p, "__")) != NULL) {
    if (p > text && magic_name_char((unsigned char)p[-1])) {
      p += 2;
      continue;
    }
    const char *q = p + 2;
    while (*q && magic_name_char((unsigned char)*q)) ++q;
    if (q > p + 2) {
      char *name = strndup_local(p, (size_t)(q - p));
      if (name) {
        (void)string_list_push_unique_copy(refs, name);
        free(name);
      }
    }
    p = q;
  }
}

static int audit_count_intersection(const string_list_t *a, const string_list_t *b) {
  int count = 0;
  for (int i = 0; i < a->count; ++i) {
    if (string_list_contains(b, a->items[i])) ++count;
  }
  return count;
}

static void audit_collect_difference(const string_list_t *left, const string_list_t *right,
                                     string_list_t *out) {
  for (int i = 0; i < left->count; ++i) {
    if (!string_list_contains(right, left->items[i]))
      (void)string_list_push_unique_copy(out, left->items[i]);
  }
}

static bool audit_name_has_prefix(const char *name, const char *prefix) {
  size_t n = prefix ? strlen(prefix) : 0;
  return name && prefix && strncmp(name, prefix, n) == 0;
}

static void audit_collect_prefix(const string_list_t *items, const char *prefix, string_list_t *out) {
  for (int i = 0; i < items->count; ++i) {
    if (audit_name_has_prefix(items->items[i], prefix))
      (void)string_list_push_unique_copy(out, items->items[i]);
  }
}

static void append_string_list_sample(str_buf_t *b, const string_list_t *items, int limit) {
  (void)sb_append_c(b, '[');
  int n = items->count < limit ? items->count : limit;
  for (int i = 0; i < n; ++i) {
    if (i) (void)sb_append_c(b, ',');
    (void)sb_append_json_str(b, items->items[i]);
  }
  (void)sb_append_c(b, ']');
}

static void append_markdown_name_sample(str_buf_t *md, const char *label,
                                        const string_list_t *items, int limit) {
  if (!md || !items || items->count <= 0) return;
  int n = items->count < limit ? items->count : limit;
  (void)sb_append(md, "- ");
  (void)sb_append(md, label ? label : "Sample");
  (void)sb_appendf(md, " (%d", items->count);
  if (items->count > n) (void)sb_appendf(md, ", showing %d", n);
  (void)sb_append(md, "): ");
  for (int i = 0; i < n; ++i) {
    if (i) (void)sb_append(md, ", ");
    md_append_code(md, items->items[i]);
  }
  (void)sb_append_c(md, '\n');
}

typedef struct {
  int line;
  int arity;
  char runtime_symbol[128];
  char signature[256];
} audit_runtime_def_info_t;

static int audit_line_number_for_ptr(const char *text, const char *ptr) {
  if (!text || !ptr || ptr < text) return 0;
  int line = 1;
  for (const char *p = text; p < ptr && *p; ++p) {
    if (*p == '\n') ++line;
  }
  return line;
}

static void audit_copy_trimmed_segment(char *out, size_t out_sz,
                                       const char *start, const char *end) {
  if (!out || out_sz == 0) return;
  out[0] = '\0';
  if (!start || !end || end < start) return;
  while (start < end && isspace((unsigned char)*start)) ++start;
  while (end > start && isspace((unsigned char)end[-1])) --end;
  size_t n = (size_t)(end - start);
  if (n >= out_sz) n = out_sz - 1;
  memcpy(out, start, n);
  out[n] = '\0';
}

static bool audit_parse_runtime_def_info(const char *defs_text,
                                         const char *name,
                                         audit_runtime_def_info_t *info) {
  if (!defs_text || !name || !*name || !info) return false;
  memset(info, 0, sizeof(*info));
  info->arity = -1;
  char *needle = NULL;
  if (asprintf(&needle, "RT_DEF(\"%s\"", name) < 0 || !needle)
    return false;
  const char *p = strstr(defs_text, needle);
  free(needle);
  if (!p) return false;
  info->line = audit_line_number_for_ptr(defs_text, p);

  const char *q = strchr(p, '"');
  if (!q) return true;
  q = strchr(q + 1, ',');
  if (!q) return true;
  q = skip_ws_const(q + 1);
  const char *sym_start = q;
  while (*q && *q != ',') ++q;
  audit_copy_trimmed_segment(info->runtime_symbol,
                             sizeof(info->runtime_symbol), sym_start, q);
  if (*q != ',') return true;

  q = skip_ws_const(q + 1);
  char *endptr = NULL;
  long arity = strtol(q, &endptr, 10);
  if (endptr && endptr != q && arity >= 0 && arity <= INT_MAX)
    info->arity = (int)arity;
  q = strchr(q, ',');
  if (!q) return true;
  q = skip_ws_const(q + 1);
  if (*q != '"') return true;
  const char *sig_start = q + 1;
  const char *sig_end = sig_start;
  while (*sig_end && *sig_end != '"') ++sig_end;
  audit_copy_trimmed_segment(info->signature, sizeof(info->signature),
                             sig_start, sig_end);
  return true;
}

typedef struct {
  const char *name;
  int count;
  string_list_t samples;
  string_list_t exports;
} audit_runtime_family_bucket_t;

typedef struct {
  audit_runtime_family_bucket_t buckets[18];
  int bucket_count;
  int nonzero_count;
  const char *top_family;
  int top_count;
} audit_runtime_family_summary_t;

static bool audit_name_is_one_of(const char *name, const char *const *names,
                                 int count) {
  if (!name) return false;
  for (int i = 0; i < count; ++i) {
    if (strcmp(name, names[i]) == 0) return true;
  }
  return false;
}

static const char *audit_runtime_family_for_name(const char *name) {
  static const char *const socket_names[] = {
    "__accept", "__bind", "__closesocket", "__connect", "__listen", "__recv",
    "__recvfrom", "__send", "__sendto", "__setsockopt", "__socket"
  };
  static const char *const process_names[] = {
    "__argvp", "__set_args"
  };
  static const char *const panic_names[] = {
    "__clear_panic_env", "__get_backtrace", "__get_panic_val",
    "__jmpbuf_align", "__jmpbuf_size", "__set_panic_env"
  };
  static const char *const io_names[] = {
    "__nanosleep", "__print_flush", "__write_buffered"
  };
  if (!name || !*name) return "other";
  if (audit_name_has_prefix(name, "__trace_")) return "trace";
  if (audit_name_has_prefix(name, "__async_")) return "async";
  if (audit_name_has_prefix(name, "__bigint_") ||
      audit_name_has_prefix(name, "__big_") ||
      strcmp(name, "__long") == 0)
    return "bigint";
  if (audit_name_is_one_of(name, socket_names,
                           (int)(sizeof(socket_names) / sizeof(socket_names[0]))))
    return "socket";
  if (audit_name_has_prefix(name, "__dict_") ||
      audit_name_has_prefix(name, "__list_") ||
      strcmp(name, "__sort_list") == 0 ||
      strcmp(name, "__store_item") == 0)
    return "container";
  if (audit_name_has_prefix(name, "__str_builder_") ||
      audit_name_has_prefix(name, "__cstr_"))
    return "string";
  if (audit_name_has_prefix(name, "__mat4_"))
    return "graphics";
  if (audit_name_is_one_of(name, panic_names,
                           (int)(sizeof(panic_names) / sizeof(panic_names[0]))))
    return "panic";
  if (audit_name_has_prefix(name, "__complex_")) return "complex";
  if (audit_name_has_prefix(name, "__zlib_")) return "compression";
  if (strcmp(name, "__push_defer") == 0 ||
      strcmp(name, "__pop_run_defer") == 0 ||
      strcmp(name, "__run_defers_to") == 0)
    return "defer";
  if (strcmp(name, "__drop_owned_slot") == 0 ||
      strcmp(name, "__release_owned") == 0 ||
      strcmp(name, "__runtime_cleanup") == 0)
    return "memory";
  if (audit_name_is_one_of(name, process_names,
                           (int)(sizeof(process_names) / sizeof(process_names[0]))))
    return "process";
  if (audit_name_is_one_of(name, io_names,
                           (int)(sizeof(io_names) / sizeof(io_names[0]))))
    return "io";
  if (strcmp(name, "__fix_fn_ptr") == 0) return "function";
  if (strcmp(name, "__index_read_probe_enabled") == 0) return "probe";
  return "other";
}

static void audit_runtime_family_summary_init(
    audit_runtime_family_summary_t *summary) {
  static const char *const names[] = {
    "socket", "trace", "bigint", "async", "container", "string",
    "graphics", "panic", "complex", "compression", "defer", "memory",
    "process", "io", "function", "probe", "other"
  };
  if (!summary) return;
  memset(summary, 0, sizeof(*summary));
  summary->bucket_count = (int)(sizeof(names) / sizeof(names[0]));
  for (int i = 0; i < summary->bucket_count; ++i)
    summary->buckets[i].name = names[i];
}

static void audit_runtime_family_summary_collect(
    audit_runtime_family_summary_t *summary, const string_list_t *items,
    int sample_limit) {
  if (!summary || !items) return;
  int per_family_limit = sample_limit;
  if (per_family_limit > 6) per_family_limit = 6;
  if (per_family_limit < 0) per_family_limit = 0;
  for (int i = 0; i < items->count; ++i) {
    const char *family = audit_runtime_family_for_name(items->items[i]);
    audit_runtime_family_bucket_t *bucket = NULL;
    for (int j = 0; j < summary->bucket_count; ++j) {
      if (strcmp(summary->buckets[j].name, family) == 0) {
        bucket = &summary->buckets[j];
        break;
      }
    }
    if (!bucket && summary->bucket_count > 0)
      bucket = &summary->buckets[summary->bucket_count - 1];
    if (!bucket) continue;
    if (bucket->count == 0) summary->nonzero_count++;
    bucket->count++;
    (void)string_list_push_copy(&bucket->exports, items->items[i]);
    if (bucket->samples.count < per_family_limit)
      (void)string_list_push_copy(&bucket->samples, items->items[i]);
    if (bucket->count > summary->top_count ||
        (bucket->count == summary->top_count && bucket->name &&
         (!summary->top_family ||
          strcmp(bucket->name, summary->top_family) < 0))) {
      summary->top_count = bucket->count;
      summary->top_family = bucket->name;
    }
  }
  if (!summary->top_family) summary->top_family = "";
}

static void audit_runtime_family_summary_free(
    audit_runtime_family_summary_t *summary) {
  if (!summary) return;
  for (int i = 0; i < summary->bucket_count; ++i) {
    string_list_free(&summary->buckets[i].samples);
    string_list_free(&summary->buckets[i].exports);
  }
}

static const audit_runtime_family_bucket_t *audit_runtime_family_top_bucket(
    const audit_runtime_family_summary_t *summary) {
  if (!summary || summary->top_count <= 0) return NULL;
  for (int i = 0; i < summary->bucket_count; ++i) {
    const audit_runtime_family_bucket_t *bucket = &summary->buckets[i];
    if (bucket->count == summary->top_count &&
        bucket->name && summary->top_family &&
        strcmp(bucket->name, summary->top_family) == 0)
      return bucket;
  }
  return NULL;
}

static void append_runtime_next_family_json(
    str_buf_t *b, const audit_runtime_family_summary_t *summary) {
  const audit_runtime_family_bucket_t *bucket =
      audit_runtime_family_top_bucket(summary);
  bool has_next = bucket && bucket->count > 0;
  (void)sb_append(b, ",\"crt_next_action\":");
  (void)sb_append_json_str(b, has_next ? "cover-unreferenced-family" : "none");
  (void)sb_append(b, ",\"crt_next_reason\":");
  (void)sb_append_json_str(
      b, has_next ? "largest unreferenced CRT/runtime export family" :
                    "runtime surface complete");
  (void)sb_append(b, ",\"crt_next_unreferenced_family\":");
  (void)sb_append_json_str(b, has_next && bucket->name ? bucket->name : "");
  (void)sb_appendf(b, ",\"crt_next_unreferenced_count\":%d",
                   has_next ? bucket->count : 0);
  (void)sb_append(b, ",\"crt_next_unreferenced_exports\":");
  if (has_next)
    append_string_list_json(b, &bucket->exports);
  else
    (void)sb_append(b, "[]");
}

static void audit_runtime_next_definition_range(
    const char *defs_text, const audit_runtime_family_bucket_t *bucket,
    int *min_line, int *max_line) {
  if (min_line) *min_line = 0;
  if (max_line) *max_line = 0;
  if (!defs_text || !bucket) return;
  for (int i = 0; i < bucket->exports.count; ++i) {
    audit_runtime_def_info_t info;
    if (!audit_parse_runtime_def_info(defs_text, bucket->exports.items[i],
                                      &info) ||
        info.line <= 0)
      continue;
    if (min_line && (*min_line == 0 || info.line < *min_line))
      *min_line = info.line;
    if (max_line && info.line > *max_line) *max_line = info.line;
  }
}

static char *audit_runtime_next_export_pattern_dup(
    const audit_runtime_family_bucket_t *bucket) {
  if (!bucket || bucket->exports.count <= 0) return strdup("");
  str_buf_t pattern = {0};
  for (int i = 0; i < bucket->exports.count; ++i) {
    if (i) (void)sb_append_c(&pattern, '|');
    (void)sb_append(&pattern, bucket->exports.items[i]);
  }
  return sb_take(&pattern);
}

static char *audit_runtime_next_inspect_command_dup(
    const char *artifact_root, const char *nytrix_root, const char *defs_path,
    const audit_runtime_family_bucket_t *bucket, int min_line, int max_line) {
  if (!bucket || bucket->exports.count <= 0) return strdup("");
  char lib_dir[4096] = {0};
  char rt_dir[4096] = {0};
  (void)path_join(lib_dir, sizeof(lib_dir), nytrix_root ? nytrix_root : "",
                  "lib");
  (void)path_join(rt_dir, sizeof(rt_dir), nytrix_root ? nytrix_root : "",
                  "etc/tests/rt");
  char *defs_rel =
      repo_or_sibling_rel_path_dup(artifact_root ? artifact_root : "",
                                   defs_path ? defs_path : "");
  char *lib_rel =
      repo_or_sibling_rel_path_dup(artifact_root ? artifact_root : "",
                                   lib_dir);
  char *rt_rel =
      repo_or_sibling_rel_path_dup(artifact_root ? artifact_root : "",
                                   rt_dir);
  char *pattern = audit_runtime_next_export_pattern_dup(bucket);
  str_buf_t cmd = {0};
  if (min_line > 0 && max_line >= min_line && defs_rel && *defs_rel) {
    char range[64];
    snprintf(range, sizeof(range), "%d,%dp", min_line, max_line);
    (void)sb_append(&cmd, "sed -n ");
    append_shell_single_quoted(&cmd, range);
    (void)sb_append_c(&cmd, ' ');
    append_shell_single_quoted(&cmd, defs_rel);
  }
  if (pattern && *pattern) {
    if (cmd.len) (void)sb_append(&cmd, "; ");
    (void)sb_append(&cmd, "rg -n ");
    append_shell_single_quoted(&cmd, pattern);
    if (lib_rel && *lib_rel) {
      (void)sb_append_c(&cmd, ' ');
      append_shell_single_quoted(&cmd, lib_rel);
    }
    if (rt_rel && *rt_rel) {
      (void)sb_append_c(&cmd, ' ');
      append_shell_single_quoted(&cmd, rt_rel);
    }
  }
  free(pattern);
  free(rt_rel);
  free(lib_rel);
  free(defs_rel);
  return sb_take(&cmd);
}

static void append_runtime_next_definition_locations_json(
    str_buf_t *b, const char *artifact_root, const char *defs_path,
    const char *defs_text, const audit_runtime_family_bucket_t *bucket) {
  (void)sb_append_c(b, '[');
  if (bucket) {
    for (int i = 0; i < bucket->exports.count; ++i) {
      audit_runtime_def_info_t info;
      bool found = audit_parse_runtime_def_info(
          defs_text, bucket->exports.items[i], &info);
      if (i) (void)sb_append_c(b, ',');
      (void)sb_append(b, "{\"export\":");
      (void)sb_append_json_str(b, bucket->exports.items[i]);
      (void)sb_append(b, ",\"path\":");
      append_repo_or_sibling_rel_json_str(b, artifact_root ? artifact_root : "",
                                          defs_path ? defs_path : "");
      (void)sb_appendf(b, ",\"line\":%d,\"arity\":%d",
                       found ? info.line : 0, found ? info.arity : -1);
      (void)sb_append(b, ",\"runtime_symbol\":");
      (void)sb_append_json_str(b, found ? info.runtime_symbol : "");
      (void)sb_append(b, ",\"signature\":");
      (void)sb_append_json_str(b, found ? info.signature : "");
      (void)sb_append_c(b, '}');
    }
  }
  (void)sb_append_c(b, ']');
}

static void append_runtime_next_definition_json(
    str_buf_t *b, const char *artifact_root, const char *nytrix_root,
    const char *defs_path, const char *defs_text,
    const audit_runtime_family_summary_t *summary) {
  const audit_runtime_family_bucket_t *bucket =
      audit_runtime_family_top_bucket(summary);
  int min_line = 0, max_line = 0;
  audit_runtime_next_definition_range(defs_text, bucket, &min_line, &max_line);
  char *inspect = audit_runtime_next_inspect_command_dup(
      artifact_root, nytrix_root, defs_path, bucket, min_line, max_line);
  (void)sb_append(b, ",\"crt_next_definition_file\":");
  append_repo_or_sibling_rel_json_str(b, artifact_root ? artifact_root : "",
                                      defs_path ? defs_path : "");
  (void)sb_append(b, ",\"crt_next_definition_locations\":");
  append_runtime_next_definition_locations_json(b, artifact_root, defs_path,
                                                defs_text, bucket);
  (void)sb_append(b, ",\"crt_next_inspect_command\":");
  (void)sb_append_json_str(b, inspect ? inspect : "");
  free(inspect);
}

static void append_runtime_next_family_markdown(
    str_buf_t *md, const audit_runtime_family_summary_t *summary,
    const char *artifact_root, const char *nytrix_root, const char *defs_path,
    const char *defs_text) {
  const audit_runtime_family_bucket_t *bucket =
      audit_runtime_family_top_bucket(summary);
  if (!md || !bucket || bucket->count <= 0) return;
  (void)sb_append(md, "## Next CRT\n\n");
  (void)sb_append(md, "- Action: ");
  md_append_code(md, "cover-unreferenced-family");
  (void)sb_append(md, "; family ");
  md_append_code(md, bucket->name ? bucket->name : "other");
  (void)sb_appendf(md, "; exports %d.\n", bucket->count);
  (void)sb_append(md, "- Exports: ");
  for (int i = 0; i < bucket->exports.count; ++i) {
    if (i) (void)sb_append(md, ", ");
    md_append_code(md, bucket->exports.items[i]);
  }
  (void)sb_append_c(md, '\n');
  int min_line = 0, max_line = 0;
  audit_runtime_next_definition_range(defs_text, bucket, &min_line, &max_line);
  if (min_line > 0) {
    char *defs_rel = repo_or_sibling_rel_path_dup(
        artifact_root ? artifact_root : "", defs_path ? defs_path : "");
    (void)sb_append(md, "- Definitions: ");
    for (int i = 0; i < bucket->exports.count; ++i) {
      audit_runtime_def_info_t info;
      if (!audit_parse_runtime_def_info(defs_text, bucket->exports.items[i],
                                        &info) ||
          info.line <= 0)
        continue;
      if (i) (void)sb_append(md, ", ");
      str_buf_t loc = {0};
      (void)sb_appendf(&loc, "%s:%d", defs_rel ? defs_rel : "", info.line);
      md_append_code(md, loc.data ? loc.data : "");
      free(loc.data);
    }
    (void)sb_append(md, "\n");
    free(defs_rel);
  }
  char *inspect = audit_runtime_next_inspect_command_dup(
      artifact_root, nytrix_root, defs_path, bucket, min_line, max_line);
  if (inspect && *inspect) {
    (void)sb_append(md, "- Inspect: ");
    md_append_code(md, inspect);
    (void)sb_append(md, "\n");
  }
  free(inspect);
  (void)sb_append_c(md, '\n');
}

static void append_runtime_family_summary_json(
    str_buf_t *b, const audit_runtime_family_summary_t *summary,
    int total_exports, int total_unreferenced) {
  (void)sb_append_c(b, '[');
  if (summary && summary->nonzero_count > 0) {
    int order[32];
    int order_count = 0;
    for (int i = 0; i < summary->bucket_count &&
                    order_count < (int)(sizeof(order) / sizeof(order[0])); ++i) {
      if (summary->buckets[i].count <= 0) continue;
      order[order_count++] = i;
    }
    for (int i = 0; i < order_count; ++i) {
      for (int j = i + 1; j < order_count; ++j) {
        const audit_runtime_family_bucket_t *a =
            &summary->buckets[order[i]];
        const audit_runtime_family_bucket_t *c =
            &summary->buckets[order[j]];
        bool swap = c->count > a->count;
        if (c->count == a->count &&
            strcmp(c->name ? c->name : "", a->name ? a->name : "") < 0)
          swap = true;
        if (swap) {
          int tmp = order[i];
          order[i] = order[j];
          order[j] = tmp;
        }
      }
    }
    for (int rank = 0; rank < order_count; ++rank) {
      const audit_runtime_family_bucket_t *bucket =
          &summary->buckets[order[rank]];
      int after_count = total_unreferenced - bucket->count;
      if (after_count < 0) after_count = 0;
      double gain = total_exports > 0 ?
          ((double)bucket->count * 100.0) / (double)total_exports : 0.0;
      double after_unreferenced_percent = total_exports > 0 ?
          ((double)after_count * 100.0) / (double)total_exports : 0.0;
      double after_coverage = 100.0 - after_unreferenced_percent;
      if (rank) (void)sb_append_c(b, ',');
      (void)sb_append(b, "{\"rank\":");
      (void)sb_appendf(b, "%d", rank + 1);
      (void)sb_append(b, ",\"family\":");
      (void)sb_append_json_str(b, bucket->name ? bucket->name : "other");
      (void)sb_appendf(
          b,
          ",\"count\":%d,\"next\":%s,"
          "\"coverage_gain_percent\":%.4f,"
          "\"after_unreferenced_count\":%d,"
          "\"after_unreferenced_percent\":%.4f,"
          "\"after_export_coverage_percent\":%.4f,\"sample\":",
          bucket->count,
          (bucket->name && summary->top_family &&
           strcmp(bucket->name, summary->top_family) == 0) ? "true" : "false",
          gain, after_count, after_unreferenced_percent, after_coverage);
      append_string_list_sample(b, &bucket->samples, bucket->samples.count);
      (void)sb_append(b, ",\"exports\":");
      append_string_list_json(b, &bucket->exports);
      (void)sb_append_c(b, '}');
    }
  }
  (void)sb_append_c(b, ']');
}

static void append_runtime_family_summary_markdown(
    str_buf_t *md, const audit_runtime_family_summary_t *summary, int limit,
    int total_exports, int total_unreferenced) {
  if (!md || !summary || summary->nonzero_count <= 0) return;
  int order[32];
  int order_count = 0;
  for (int i = 0; i < summary->bucket_count &&
                  order_count < (int)(sizeof(order) / sizeof(order[0])); ++i) {
    if (summary->buckets[i].count <= 0) continue;
    order[order_count++] = i;
  }
  for (int i = 0; i < order_count; ++i) {
    for (int j = i + 1; j < order_count; ++j) {
      const audit_runtime_family_bucket_t *a = &summary->buckets[order[i]];
      const audit_runtime_family_bucket_t *c = &summary->buckets[order[j]];
      bool swap = c->count > a->count;
      if (c->count == a->count &&
          strcmp(c->name ? c->name : "", a->name ? a->name : "") < 0)
        swap = true;
      if (swap) {
        int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
      }
    }
  }
  for (int emitted = 0; emitted < order_count; ++emitted) {
    if (limit > 0 && emitted >= limit) break;
    const audit_runtime_family_bucket_t *bucket =
        &summary->buckets[order[emitted]];
    int after_count = total_unreferenced - bucket->count;
    if (after_count < 0) after_count = 0;
    double gain = total_exports > 0 ?
        ((double)bucket->count * 100.0) / (double)total_exports : 0.0;
    (void)sb_append(md, "- ");
    (void)sb_appendf(md, "#%d ", emitted + 1);
    md_append_code(md, bucket->name ? bucket->name : "other");
    (void)sb_appendf(md, " %d; gain %.2f%%; after %d",
                     bucket->count, gain, after_count);
    if (bucket->samples.count > 0) {
      (void)sb_append(md, ": ");
      int n = bucket->samples.count < 4 ? bucket->samples.count : 4;
      for (int j = 0; j < n; ++j) {
        if (j) (void)sb_append(md, ", ");
        md_append_code(md, bucket->samples.items[j]);
      }
    }
    (void)sb_append_c(md, '\n');
  }
}

static const char *compiler_std_audit_surface_state(int wrapper_gap_count,
                                                    int unreferenced_exports) {
  if (wrapper_gap_count > 0) return "attention";
  if (unreferenced_exports > 0) return "partial";
  return "clean";
}

static bool write_compiler_std_audit_markdown(
    const char *artifact_root, const char *markdown_path, const char *json_path,
    const char *nytrix_root, const char *defs_path, const char *defs_text,
    int ny_files, int runtime_exports, int magic_refs, int direct_refs,
    int language_private_refs, int unreferenced_exports,
    int duplicate_exports, int scan_errors,
    int simmd_runtime_exports, int simmd_refs, int simmd_missing_wrappers,
    int simmd_unknown_refs, int wrapper_gap_count,
    double runtime_coverage_percent, double unreferenced_percent,
    const char *surface_state, const string_list_t *language_private,
    const string_list_t *unreferenced_defs, const string_list_t *duplicate_defs,
    const string_list_t *scan_error_list,
    const string_list_t *simmd_missing_wrapper_list,
    const string_list_t *simmd_unknown_ref_list,
    const audit_runtime_family_summary_t *family_summary, int sample_limit) {
  if (!markdown_path || !*markdown_path) return true;
  char *json_rel = rel_path_dup(artifact_root ? artifact_root : "",
                                json_path && *json_path ? json_path :
                                    "build/fuzz/ultra/compiler-std-audit.json");
  char *md_rel = rel_path_dup(artifact_root ? artifact_root : "",
                              markdown_path);
  char *scan_rel = rel_path_dup(artifact_root ? artifact_root : "",
                                nytrix_root ? nytrix_root : "");
  str_buf_t md = {0};
  (void)sb_append(&md, "# Nynth Compiler Std Audit\n\n");
  (void)sb_append(&md, "## TLDR\n\n");
  (void)sb_append(&md, "- Runtime surface: ");
  md_append_code(&md, surface_state && *surface_state ? surface_state : "unknown");
  (void)sb_appendf(&md,
                   "; wrapper gaps %d; duplicate exports %d; scan errors %d.\n",
                   wrapper_gap_count, duplicate_exports, scan_errors);
  (void)sb_appendf(&md,
                   "- Runtime exports: %d total; %d directly referenced by std/tests; coverage %.2f%%.\n",
                   runtime_exports, direct_refs, runtime_coverage_percent);
  (void)sb_appendf(&md,
                   "- CRT aliases: state `%s`; coverage %.2f%%; unreferenced %d (%.2f%%); wrapper gaps %d.\n",
                   surface_state && *surface_state ? surface_state : "unknown",
                   runtime_coverage_percent, unreferenced_exports,
                   unreferenced_percent, wrapper_gap_count);
  (void)sb_append(&md,
                  "- Claim scope: `surface-reference-coverage`; direct Ny std/runtime-test symbol references, not a bugless CRT behavior proof.\n");
  (void)sb_append(&md, "- CRT behavior next: ");
  md_append_code(&md, NYNTH_CRT_BEHAVIOR_NEXT_ACTION);
  (void)sb_append(&md, "; ");
  md_append_code(&md, NYNTH_CRT_BEHAVIOR_NEXT_COMMAND);
  (void)sb_append(&md, ".\n");
  if (family_summary && family_summary->nonzero_count > 0) {
    (void)sb_appendf(&md,
                     "- CRT families: %d unreferenced groups; top `%s` %d.\n",
                     family_summary->nonzero_count,
                     family_summary->top_family && *family_summary->top_family ?
                         family_summary->top_family : "none",
                     family_summary->top_count);
  }
  (void)sb_appendf(&md,
                   "- Unreferenced exports: %d (%.2f%%); language-private refs %d; magic refs %d across %d Ny files.\n",
                   unreferenced_exports, unreferenced_percent,
                   language_private_refs, magic_refs, ny_files);
  (void)sb_appendf(&md,
                   "- SIMD wrappers: exports %d; refs %d; missing wrappers %d; unknown refs %d.\n",
                   simmd_runtime_exports, simmd_refs,
                   simmd_missing_wrappers, simmd_unknown_refs);
  if (scan_rel && *scan_rel) {
    (void)sb_append(&md, "- Scan root: ");
    md_append_code(&md, scan_rel);
    (void)sb_append_c(&md, '\n');
  }
  (void)sb_append_c(&md, '\n');

  append_runtime_next_family_markdown(&md, family_summary, artifact_root,
                                      nytrix_root, defs_path, defs_text);

  (void)sb_append(&md, "## Samples\n\n");
  int emitted_samples = 0;
  int md_sample_limit = sample_limit > 12 ? 12 : sample_limit;
  if (language_private && language_private->count > 0) {
    append_markdown_name_sample(&md, "Language-private refs",
                                language_private, md_sample_limit);
    ++emitted_samples;
  }
  if (unreferenced_defs && unreferenced_defs->count > 0) {
    append_markdown_name_sample(&md, "Unreferenced runtime exports",
                                unreferenced_defs, md_sample_limit);
    ++emitted_samples;
  }
  if (duplicate_defs && duplicate_defs->count > 0) {
    append_markdown_name_sample(&md, "Duplicate runtime exports",
                                duplicate_defs, md_sample_limit);
    ++emitted_samples;
  }
  if (scan_error_list && scan_error_list->count > 0) {
    append_markdown_name_sample(&md, "Scan errors", scan_error_list,
                                md_sample_limit);
    ++emitted_samples;
  }
  if (simmd_missing_wrapper_list && simmd_missing_wrapper_list->count > 0) {
    append_markdown_name_sample(&md, "Missing SIMD wrappers",
                                simmd_missing_wrapper_list, md_sample_limit);
    ++emitted_samples;
  }
  if (simmd_unknown_ref_list && simmd_unknown_ref_list->count > 0) {
    append_markdown_name_sample(&md, "Unknown SIMD refs",
                                simmd_unknown_ref_list, md_sample_limit);
    ++emitted_samples;
  }
  if (!emitted_samples)
    (void)sb_append(&md, "No sample lists to show.\n");
  if (family_summary && family_summary->nonzero_count > 0) {
    (void)sb_append(&md, "\n## CRT Families\n\n");
    append_runtime_family_summary_markdown(&md, family_summary, 0,
                                           runtime_exports,
                                           unreferenced_exports);
  }
  (void)sb_append(&md, "\n## Refresh\n\n```bash\n");
  (void)sb_append(&md,
                  "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                  "./build/nynth compiler std-audit --json ");
  (void)sb_append(&md, json_rel && *json_rel ? json_rel :
                  "build/fuzz/ultra/compiler-std-audit.json");
  (void)sb_append(&md, " --markdown ");
  (void)sb_append(&md, md_rel && *md_rel ? md_rel :
                  "build/fuzz/ultra/compiler-std-audit.md");
  (void)sb_append(&md, "\n```\n");
  bool ok = md.data && write_file_text(markdown_path, md.data);
  free(md.data);
  free(json_rel);
  free(md_rel);
  free(scan_rel);
  return ok;
}

static int cmd_public_compiler_std_audit(int argc, char **argv) {
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *markdown_path = value_after(argc, argv, 3, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after(argc, argv, 3, "--md", "");
  int sample_limit = atoi(value_after(argc, argv, 3, "--sample", "24"));
  if (sample_limit < 0) sample_limit = 0;
  char artifact_root[4096];
  if (!find_nynth_root(artifact_root, sizeof(artifact_root)))
    snprintf(artifact_root, sizeof(artifact_root), "%s", root);
  string_list_t rows = {0}, failures = {0};
  string_list_t defs = {0}, duplicate_defs = {0}, refs = {0}, files = {0};
  string_list_t language_private = {0}, unreferenced_defs = {0}, scan_errors = {0};
  string_list_t simmd_defs = {0}, simmd_refs = {0}, simmd_missing_wrappers = {0}, simmd_unknown_refs = {0};
  char *defs_path = NULL, *lib_dir = NULL, *rt_dir = NULL;
  (void)asprintf(&defs_path, "%s/src/rt/defs.h", root);
  (void)asprintf(&lib_dir, "%s/lib", root);
  (void)asprintf(&rt_dir, "%s/etc/tests/rt", root);
  file_buf_t defs_file = {0};
  bool ok = defs_path && read_file(defs_path, &defs_file);
  if (ok) {
    audit_extract_runtime_defs(defs_file.data, &defs, &duplicate_defs);
  } else {
    (void)string_list_push_copy(&scan_errors, "src/rt/defs.h");
  }
  if (lib_dir && !collect_regular_files_recursive(lib_dir, &files))
    (void)string_list_push_copy(&scan_errors, "lib");
  if (rt_dir && !collect_regular_files_recursive(rt_dir, &files))
    (void)string_list_push_copy(&scan_errors, "etc/tests/rt");
  qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
  int ny_files = 0;
  for (int i = 0; i < files.count; ++i) {
    if (!ny_has_suffix(files.items[i], ".ny")) continue;
    ++ny_files;
    file_buf_t f = {0};
    if (!read_file(files.items[i], &f)) {
      char *rel = NULL;
      if (asprintf(&rel, "%s", files.items[i]) >= 0)
        (void)string_list_push_take(&scan_errors, rel);
      continue;
    }
    audit_extract_magic_refs(f.data, &refs);
    free(f.data);
  }
  audit_collect_difference(&refs, &defs, &language_private);
  audit_collect_difference(&defs, &refs, &unreferenced_defs);
  audit_collect_prefix(&defs, "__simmd_", &simmd_defs);
  audit_collect_prefix(&refs, "__simmd_", &simmd_refs);
  audit_collect_difference(&simmd_defs, &simmd_refs, &simmd_missing_wrappers);
  audit_collect_difference(&simmd_refs, &simmd_defs, &simmd_unknown_refs);
  qsort(defs.items, (size_t)defs.count, sizeof(char *), cmp_cstr);
  qsort(refs.items, (size_t)refs.count, sizeof(char *), cmp_cstr);
  qsort(language_private.items, (size_t)language_private.count, sizeof(char *), cmp_cstr);
  qsort(unreferenced_defs.items, (size_t)unreferenced_defs.count, sizeof(char *), cmp_cstr);
  qsort(duplicate_defs.items, (size_t)duplicate_defs.count, sizeof(char *), cmp_cstr);
  qsort(scan_errors.items, (size_t)scan_errors.count, sizeof(char *), cmp_cstr);
  qsort(simmd_defs.items, (size_t)simmd_defs.count, sizeof(char *), cmp_cstr);
  qsort(simmd_refs.items, (size_t)simmd_refs.count, sizeof(char *), cmp_cstr);
  qsort(simmd_missing_wrappers.items, (size_t)simmd_missing_wrappers.count, sizeof(char *), cmp_cstr);
  qsort(simmd_unknown_refs.items, (size_t)simmd_unknown_refs.count, sizeof(char *), cmp_cstr);
  audit_runtime_family_summary_t runtime_families = {0};
  audit_runtime_family_summary_init(&runtime_families);
  audit_runtime_family_summary_collect(&runtime_families, &unreferenced_defs,
                                       sample_limit);

  if (duplicate_defs.count)
    (void)string_list_push_take(&failures,
                                make_worker_failure_row("std_runtime_surface", "compiler-std-audit",
                                                        1, "", "duplicate RT_DEF runtime exports"));
  if (scan_errors.count)
    (void)string_list_push_take(&failures,
                                make_worker_failure_row("std_runtime_surface", "compiler-std-audit",
                                                        1, "", "std/runtime scan errors"));
  if (simmd_missing_wrappers.count)
    (void)string_list_push_take(&failures,
                                make_worker_failure_row("std_runtime_surface", "compiler-std-audit",
                                                        1, "", "SIMD runtime export lacks Ny wrapper/test reference"));
  if (simmd_unknown_refs.count)
    (void)string_list_push_take(&failures,
                                make_worker_failure_row("std_runtime_surface", "compiler-std-audit",
                                                        1, "", "Ny SIMD wrapper references missing runtime export"));
  int direct_refs = audit_count_intersection(&refs, &defs);
  int wrapper_gap_count = duplicate_defs.count + scan_errors.count +
                          simmd_missing_wrappers.count +
                          simmd_unknown_refs.count;
  double runtime_coverage_percent =
      defs.count > 0 ? ((double)direct_refs * 100.0) / (double)defs.count : 0.0;
  double unreferenced_percent =
      defs.count > 0 ? ((double)unreferenced_defs.count * 100.0) / (double)defs.count : 0.0;
  const char *surface_state =
      compiler_std_audit_surface_state(wrapper_gap_count,
                                       unreferenced_defs.count);
  if (markdown_path && *markdown_path &&
      !write_compiler_std_audit_markdown(
          artifact_root, markdown_path, json_path, root, defs_path,
          defs_file.data, ny_files, defs.count, refs.count, direct_refs,
          language_private.count,
          unreferenced_defs.count, duplicate_defs.count, scan_errors.count,
          simmd_defs.count, simmd_refs.count, simmd_missing_wrappers.count,
          simmd_unknown_refs.count, wrapper_gap_count,
          runtime_coverage_percent, unreferenced_percent, surface_state,
          &language_private, &unreferenced_defs, &duplicate_defs,
          &scan_errors, &simmd_missing_wrappers, &simmd_unknown_refs,
          &runtime_families, sample_limit))
    (void)string_list_push_take(&failures,
                                make_worker_failure_row("std_runtime_surface", "compiler-std-audit",
                                                        1, "", "compiler std audit markdown write failed"));
  const char *report_state = failures.count ? "attention" : surface_state;
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":\"std_runtime_surface\",\"ok\":");
  (void)sb_append(&row, failures.count ? "false" : "true");
  (void)sb_appendf(&row,
                   ",\"kind\":\"compiler-std-audit\",\"engine\":\"nynth_core\","
                   "\"runtime_surface_state\":\"%s\",\"crt_surface_state\":\"%s\","
                   "\"ny_files\":%d,\"runtime_exports\":%d,\"magic_refs\":%d,"
                   "\"direct_runtime_refs\":%d,\"language_private_refs\":%d,"
                   "\"unreferenced_runtime_exports\":%d,\"duplicate_runtime_exports\":%d,"
                   "\"scan_errors\":%d,\"runtime_export_coverage_percent\":%.4f,"
                   "\"runtime_unreferenced_percent\":%.4f,"
                   "\"runtime_unreferenced_count\":%d,"
                   "\"runtime_wrapper_gap_count\":%d,"
                   "\"crt_runtime_exports\":%d,\"crt_direct_refs\":%d,"
                   "\"crt_unreferenced_exports\":%d,"
                   "\"crt_export_coverage_percent\":%.4f,"
                   "\"crt_unreferenced_percent\":%.4f,"
                   "\"crt_unreferenced_count\":%d,"
                   "\"crt_wrapper_gap_count\":%d,"
                   "\"crt_unreferenced_family_count\":%d,"
                   "\"crt_top_unreferenced_family\":",
                   report_state, report_state,
                   ny_files, defs.count, refs.count, direct_refs, language_private.count,
                   unreferenced_defs.count, duplicate_defs.count, scan_errors.count,
                   runtime_coverage_percent, unreferenced_percent,
                   unreferenced_defs.count, wrapper_gap_count,
                   defs.count, direct_refs, unreferenced_defs.count,
                   runtime_coverage_percent, unreferenced_percent,
                   unreferenced_defs.count,
                   wrapper_gap_count, runtime_families.nonzero_count);
  (void)sb_append_json_str(&row, runtime_families.top_family ?
                                 runtime_families.top_family : "");
  (void)sb_append(&row, ",\"runtime_surface_scope\":");
  (void)sb_append_json_str(&row, NYNTH_RUNTIME_SURFACE_SCOPE);
  (void)sb_append(&row, ",\"crt_surface_scope\":");
  (void)sb_append_json_str(&row, NYNTH_CRT_SURFACE_SCOPE);
  (void)sb_append(&row, ",\"crt_behavior_state\":");
  (void)sb_append_json_str(&row, NYNTH_CRT_BEHAVIOR_STATE);
  (void)sb_append(&row, ",\"crt_behavior_scope\":");
  (void)sb_append_json_str(&row, NYNTH_CRT_BEHAVIOR_SCOPE);
  append_fuzz_all_crt_behavior_next_fields(&row);
  (void)sb_appendf(&row,
                   ",\"crt_top_unreferenced_family_count\":%d,"
                   "\"crt_unreferenced_families\":",
                   runtime_families.top_count);
  append_runtime_family_summary_json(&row, &runtime_families, defs.count,
                                     unreferenced_defs.count);
  append_runtime_next_family_json(&row, &runtime_families);
  append_runtime_next_definition_json(&row, artifact_root, root, defs_path,
                                      defs_file.data, &runtime_families);
  (void)sb_appendf(&row,
                   ",\"simmd_runtime_exports\":%d,\"simmd_refs\":%d,"
                   "\"simmd_missing_wrappers\":%d,\"simmd_unknown_refs\":%d,"
                   "\"simd_runtime_exports\":%d,\"simd_refs\":%d,"
                   "\"simd_missing_wrappers\":%d,\"simd_unknown_refs\":%d",
                   simmd_defs.count, simmd_refs.count,
                   simmd_missing_wrappers.count, simmd_unknown_refs.count,
                   simmd_defs.count, simmd_refs.count,
                   simmd_missing_wrappers.count, simmd_unknown_refs.count);
  (void)sb_append(&row, ",\"language_private_sample\":");
  append_string_list_sample(&row, &language_private, sample_limit);
  (void)sb_append(&row, ",\"unreferenced_runtime_sample\":");
  append_string_list_sample(&row, &unreferenced_defs, sample_limit);
  (void)sb_append(&row, ",\"duplicate_runtime_sample\":");
  append_string_list_sample(&row, &duplicate_defs, sample_limit);
  (void)sb_append(&row, ",\"scan_error_sample\":");
  append_string_list_sample(&row, &scan_errors, sample_limit);
  (void)sb_append(&row, ",\"simmd_missing_wrapper_sample\":");
  append_string_list_sample(&row, &simmd_missing_wrappers, sample_limit);
  (void)sb_append(&row, ",\"simmd_unknown_ref_sample\":");
  append_string_list_sample(&row, &simmd_unknown_refs, sample_limit);
  if (markdown_path && *markdown_path) {
    (void)sb_append(&row, ",\"markdown\":");
    append_rel_json_str(&row, artifact_root, markdown_path);
  }
  (void)sb_append_c(&row, '}');
  (void)string_list_push_take(&rows, sb_take(&row));

  str_buf_t extra = {0};
  (void)sb_appendf(&extra,
                   ",\"runtime_surface_state\":\"%s\",\"crt_surface_state\":\"%s\","
                   "\"ny_files\":%d,\"runtime_exports\":%d,\"magic_refs\":%d,"
                   "\"direct_runtime_refs\":%d,\"language_private_refs\":%d,"
                   "\"unreferenced_runtime_exports\":%d,"
                   "\"runtime_export_coverage_percent\":%.4f,"
                   "\"runtime_unreferenced_percent\":%.4f,"
                   "\"runtime_unreferenced_count\":%d,"
                   "\"runtime_wrapper_gap_count\":%d,"
                   "\"crt_runtime_exports\":%d,\"crt_direct_refs\":%d,"
                   "\"crt_unreferenced_exports\":%d,"
                   "\"crt_export_coverage_percent\":%.4f,"
                   "\"crt_unreferenced_percent\":%.4f,"
                   "\"crt_unreferenced_count\":%d,"
                   "\"crt_wrapper_gap_count\":%d,"
                   "\"crt_unreferenced_family_count\":%d,"
                   "\"crt_top_unreferenced_family\":",
                   report_state, report_state,
                   ny_files, defs.count, refs.count, direct_refs, language_private.count,
                   unreferenced_defs.count, runtime_coverage_percent,
                   unreferenced_percent, unreferenced_defs.count,
                   wrapper_gap_count, defs.count, direct_refs,
                   unreferenced_defs.count,
                   runtime_coverage_percent, unreferenced_percent,
                   unreferenced_defs.count, wrapper_gap_count,
                   runtime_families.nonzero_count);
  (void)sb_append_json_str(&extra, runtime_families.top_family ?
                                   runtime_families.top_family : "");
  (void)sb_append(&extra, ",\"runtime_surface_scope\":");
  (void)sb_append_json_str(&extra, NYNTH_RUNTIME_SURFACE_SCOPE);
  (void)sb_append(&extra, ",\"crt_surface_scope\":");
  (void)sb_append_json_str(&extra, NYNTH_CRT_SURFACE_SCOPE);
  (void)sb_append(&extra, ",\"crt_behavior_state\":");
  (void)sb_append_json_str(&extra, NYNTH_CRT_BEHAVIOR_STATE);
  (void)sb_append(&extra, ",\"crt_behavior_scope\":");
  (void)sb_append_json_str(&extra, NYNTH_CRT_BEHAVIOR_SCOPE);
  append_fuzz_all_crt_behavior_next_fields(&extra);
  (void)sb_appendf(&extra,
                   ",\"crt_top_unreferenced_family_count\":%d,"
                   "\"crt_unreferenced_families\":",
                   runtime_families.top_count);
  append_runtime_family_summary_json(&extra, &runtime_families, defs.count,
                                     unreferenced_defs.count);
  append_runtime_next_family_json(&extra, &runtime_families);
  append_runtime_next_definition_json(&extra, artifact_root, root, defs_path,
                                      defs_file.data, &runtime_families);
  (void)sb_appendf(&extra,
                   ",\"simmd_runtime_exports\":%d,"
                   "\"simmd_refs\":%d,\"simmd_missing_wrappers\":%d,\"simmd_unknown_refs\":%d,"
                   "\"simd_runtime_exports\":%d,\"simd_refs\":%d,"
                   "\"simd_missing_wrappers\":%d,\"simd_unknown_refs\":%d",
                   simmd_defs.count, simmd_refs.count,
                   simmd_missing_wrappers.count, simmd_unknown_refs.count,
                   simmd_defs.count, simmd_refs.count,
                   simmd_missing_wrappers.count, simmd_unknown_refs.count);
  if (markdown_path && *markdown_path) {
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, artifact_root, markdown_path);
  }
  (void)sb_append(&extra, ",\"nytrix_root\":");
  append_rel_json_str(&extra, artifact_root, root);
  char *report = build_native_report_json_with_top_aliases(
      &rows, &failures, "compiler-std-audit", extra.data, true);
  int rc = emit_native_report(report, json_path, "compiler std audit", rows.count, failures.count);
  free(extra.data);
  free(defs_file.data);
  free(defs_path); free(lib_dir); free(rt_dir);
  audit_runtime_family_summary_free(&runtime_families);
  string_list_free(&defs); string_list_free(&duplicate_defs); string_list_free(&refs);
  string_list_free(&files); string_list_free(&language_private);
  string_list_free(&unreferenced_defs); string_list_free(&scan_errors);
  string_list_free(&simmd_defs); string_list_free(&simmd_refs);
  string_list_free(&simmd_missing_wrappers); string_list_free(&simmd_unknown_refs);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static void creal_io_sample_list_free(creal_io_sample_list_t *list) {
  for (int i = 0; i < list->count; ++i) {
    string_list_free(&list->items[i].inputs);
    free(list->items[i].expected);
  }
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static bool creal_io_sample_list_push_take(creal_io_sample_list_t *list, creal_io_sample_t sample) {
  if (list->count == list->cap) {
    int next_cap = list->cap ? list->cap * 2 : 8;
    creal_io_sample_t *next = (creal_io_sample_t *)realloc(list->items, (size_t)next_cap * sizeof(*next));
    if (!next) return false;
    list->items = next;
    list->cap = next_cap;
  }
  list->items[list->count++] = sample;
  return true;
}

static void creal_record_free(creal_record_t *r) {
  free(r->function_name);
  free(r->parameter_types);
  free(r->return_type);
  free(r->function_source);
  free(r->io_list);
  free(r->misc);
  free(r->src_file);
  free(r->include_headers);
  free(r->include_sources);
  free(r->raw_json);
  memset(r, 0, sizeof(*r));
}

static void creal_exec_result_free(creal_exec_result_t *r) {
  proc_result_free(&r->compile);
  run_many_result_free(&r->run);
  free(r->exe_path);
  memset(r, 0, sizeof(*r));
}

static char *trim_spaces_dup(const char *s) {
  if (!s) return strdup("");
  while (*s && isspace((unsigned char)*s)) ++s;
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) --n;
  return strndup_local(s, n);
}

static bool creal_valid_c_identifier(const char *s) {
  if (!s || !*s) return false;
  if (!isalpha((unsigned char)s[0]) && s[0] != '_') return false;
  for (const char *p = s + 1; *p; ++p) {
    if (!isalnum((unsigned char)*p) && *p != '_') return false;
  }
  return true;
}

static bool creal_type_is_allowed_name(const char *t, bool allow_void) {
  static const char *allowed[] = {
    "char", "signed char", "unsigned char",
    "short", "short int", "signed short", "signed short int",
    "unsigned short", "unsigned short int",
    "int", "signed", "signed int", "unsigned", "unsigned int",
    "long", "long int", "signed long", "signed long int",
    "unsigned long", "unsigned long int",
    "long long", "long long int", "signed long long", "signed long long int",
    "unsigned long long", "unsigned long long int",
    "_Bool", "bool"
  };
  if (allow_void && strcmp(t, "void") == 0) return true;
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
    if (strcmp(t, allowed[i]) == 0) return true;
  }
  return false;
}

static bool creal_scalar_type_supported(const char *type, bool allow_void) {
  char *t = trim_spaces_dup(type);
  bool ok = t && !strchr(t, '*') && !strchr(t, '[') && !strchr(t, ']') &&
            !strstr(t, "float") && !strstr(t, "double") &&
            creal_type_is_allowed_name(t, allow_void);
  free(t);
  return ok;
}

static bool creal_type_is_unsigned(const char *type) {
  char *t = trim_spaces_dup(type);
  bool yes = t && strstr(t, "unsigned") != NULL;
  free(t);
  return yes;
}

static bool creal_numeric_literal_ok(const char *text) {
  char *t = trim_spaces_dup(text);
  if (!t || !*t) {
    free(t);
    return false;
  }
  char *end = NULL;
  (void)strtoll(t, &end, 10);
  while (end && *end && isspace((unsigned char)*end)) ++end;
  bool ok = end && *end == '\0';
  free(t);
  return ok;
}

static bool creal_parse_string_array(const char *array_text, string_list_t *out) {
  if (!array_text || array_text[0] != '[') return false;
  const char *end = matching_json_end(array_text, '[', ']');
  if (!end) return false;
  const char *p = array_text + 1;
  while (p < end) {
    p = skip_ws_const(p);
    if (p >= end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '"') return false;
    char *item = parse_json_string_dup(&p, end);
    if (!item) return false;
    if (!string_list_push_take(out, item)) {
      free(item);
      return false;
    }
  }
  return true;
}

static char *creal_parse_json_scalar_text(const char **cursor, const char *end) {
  const char *p = skip_ws_const(*cursor);
  if (p >= end) return NULL;
  if (*p == '"') {
    char *s = parse_json_string_dup(&p, end);
    *cursor = p;
    return s;
  }
  const char *start = p;
  while (p < end && *p != ',' && *p != ']' && *p != '}') ++p;
  const char *tail = p;
  while (tail > start && isspace((unsigned char)tail[-1])) --tail;
  *cursor = p;
  return strndup_local(start, (size_t)(tail - start));
}

static bool creal_parse_io_list(const char *io_list, creal_io_sample_list_t *out, int max_samples) {
  if (!io_list || io_list[0] != '[') return false;
  const char *end = matching_json_end(io_list, '[', ']');
  if (!end) return false;
  const char *p = io_list + 1;
  while (p < end && (max_samples <= 0 || out->count < max_samples)) {
    p = skip_ws_const(p);
    if (p >= end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '[') return false;
    const char *sample_end = matching_json_end(p, '[', ']');
    if (!sample_end || sample_end > end) return false;
    const char *q = skip_ws_const(p + 1);
    if (q >= sample_end || *q != '[') return false;
    const char *inputs_end = matching_json_end(q, '[', ']');
    if (!inputs_end || inputs_end > sample_end) return false;
    creal_io_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    char *inputs_text = strndup_local(q, (size_t)(inputs_end - q + 1));
    bool ok = inputs_text && creal_parse_string_array(inputs_text, &sample.inputs);
    free(inputs_text);
    q = skip_ws_const(inputs_end + 1);
    if (ok && q < sample_end && *q == ',') {
      ++q;
      sample.expected = creal_parse_json_scalar_text(&q, sample_end);
    } else {
      ok = false;
    }
    if (!ok || !sample.expected) {
      string_list_free(&sample.inputs);
      free(sample.expected);
      return false;
    }
    if (!creal_io_sample_list_push_take(out, sample)) {
      string_list_free(&sample.inputs);
      free(sample.expected);
      return false;
    }
    p = sample_end + 1;
  }
  return out->count > 0;
}

static char *creal_expected_output(const creal_io_sample_list_t *samples) {
  str_buf_t out = {0};
  for (int i = 0; i < samples->count; ++i) {
    if (i) (void)sb_append_c(&out, '\n');
    (void)sb_append(&out, samples->items[i].expected ? samples->items[i].expected : "");
  }
  return sb_take(&out);
}

static void creal_append_include_headers(str_buf_t *b, const char *array_text) {
  string_list_t headers = {0};
  if (!creal_parse_string_array(array_text ? array_text : "[]", &headers)) return;
  for (int i = 0; i < headers.count; ++i) {
    const char *h = headers.items[i];
    if (!h || !*h || strchr(h, '\n') || strchr(h, '\r')) continue;
    if (h[0] == '<' || h[0] == '"') (void)sb_appendf(b, "#include %s\n", h);
    else (void)sb_appendf(b, "#include <%s>\n", h);
  }
  string_list_free(&headers);
}

static void creal_append_string_array_lines(str_buf_t *b, const char *array_text) {
  string_list_t items = {0};
  if (!creal_parse_string_array(array_text ? array_text : "[]", &items)) return;
  for (int i = 0; i < items.count; ++i) {
    if (!items.items[i] || !*items.items[i]) continue;
    (void)sb_append(b, items.items[i]);
    if (items.items[i][strlen(items.items[i]) - 1] != '\n') (void)sb_append_c(b, '\n');
  }
  string_list_free(&items);
}

static char *creal_build_c_source(const creal_record_t *rec, const creal_io_sample_list_t *samples,
                                  char *error, size_t error_sz) {
  if (error_sz) error[0] = '\0';
  if (!creal_valid_c_identifier(rec->function_name)) {
    snprintf(error, error_sz, "unsupported function name");
    return NULL;
  }
  if (!creal_scalar_type_supported(rec->return_type, false)) {
    snprintf(error, error_sz, "unsupported return type");
    return NULL;
  }
  string_list_t param_types = {0};
  if (!creal_parse_string_array(rec->parameter_types ? rec->parameter_types : "[]", &param_types)) {
    snprintf(error, error_sz, "invalid parameter_types array");
    return NULL;
  }
  for (int i = 0; i < param_types.count; ++i) {
    if (!creal_scalar_type_supported(param_types.items[i], false)) {
      snprintf(error, error_sz, "unsupported parameter type");
      string_list_free(&param_types);
      return NULL;
    }
  }
  for (int i = 0; i < samples->count; ++i) {
    if (samples->items[i].inputs.count != param_types.count) {
      snprintf(error, error_sz, "sample arity mismatch");
      string_list_free(&param_types);
      return NULL;
    }
    for (int j = 0; j < samples->items[i].inputs.count; ++j) {
      if (!creal_numeric_literal_ok(samples->items[i].inputs.items[j])) {
        snprintf(error, error_sz, "non-integer sample input");
        string_list_free(&param_types);
        return NULL;
      }
    }
    if (!creal_numeric_literal_ok(samples->items[i].expected)) {
      snprintf(error, error_sz, "non-integer expected output");
      string_list_free(&param_types);
      return NULL;
    }
  }

  str_buf_t b = {0};
  (void)sb_append(&b, "/* generated by nynth from a Creal function database record */\n");
  (void)sb_append(&b, "#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n");
  (void)sb_append(&b, "#ifndef __GNUC__\n#define __attribute__(x)\n#endif\n");
  creal_append_include_headers(&b, rec->include_headers);
  creal_append_string_array_lines(&b, rec->misc);
  creal_append_string_array_lines(&b, rec->include_sources);
  if (rec->function_source && *rec->function_source) {
    (void)sb_append(&b, rec->function_source);
    if (rec->function_source[strlen(rec->function_source) - 1] != '\n') (void)sb_append_c(&b, '\n');
  }
  (void)sb_append(&b, "\nint main(void) {\n");
  for (int i = 0; i < samples->count; ++i) {
    (void)sb_append(&b, "  {\n    ");
    (void)sb_append(&b, rec->return_type);
    (void)sb_appendf(&b, " nynth_creal_ret_%d = %s(", i, rec->function_name);
    for (int j = 0; j < param_types.count; ++j) {
      if (j) (void)sb_append(&b, ", ");
      char *t = trim_spaces_dup(param_types.items[j]);
      char *v = trim_spaces_dup(samples->items[i].inputs.items[j]);
      (void)sb_append_c(&b, '(');
      (void)sb_append(&b, t ? t : param_types.items[j]);
      (void)sb_append_c(&b, ')');
      (void)sb_append(&b, v ? v : samples->items[i].inputs.items[j]);
      free(t);
      free(v);
    }
    (void)sb_append(&b, ");\n    ");
    if (creal_type_is_unsigned(rec->return_type)) {
      (void)sb_appendf(&b, "printf(\"%%llu\\n\", (unsigned long long)nynth_creal_ret_%d);\n", i);
    } else {
      (void)sb_appendf(&b, "printf(\"%%lld\\n\", (long long)nynth_creal_ret_%d);\n", i);
    }
    (void)sb_append(&b, "  }\n");
  }
  (void)sb_append(&b, "  return 0;\n}\n");
  string_list_free(&param_types);
  return sb_take(&b);
}

static bool creal_parse_record(const char *start, const char *end, creal_record_t *rec) {
  memset(rec, 0, sizeof(*rec));
  rec->function_name = json_extract_string_range(start, end, "function_name");
  rec->parameter_types = json_extract_array_range(start, end, "parameter_types");
  rec->return_type = json_extract_string_range(start, end, "return_type");
  rec->function_source = json_extract_string_range(start, end, "function");
  rec->io_list = json_extract_array_range(start, end, "io_list");
  rec->misc = json_extract_array_range(start, end, "misc");
  rec->src_file = json_extract_string_range(start, end, "src_file");
  rec->include_headers = json_extract_array_range(start, end, "include_headers");
  rec->include_sources = json_extract_array_range(start, end, "include_sources");
  rec->raw_json = strndup_local(start, (size_t)(end - start));
  if (!rec->misc) rec->misc = strdup("[]");
  if (!rec->src_file) rec->src_file = strdup("");
  if (!rec->include_headers) rec->include_headers = strdup("[]");
  if (!rec->include_sources) rec->include_sources = strdup("[]");
  bool ok = rec->function_name && rec->parameter_types && rec->return_type &&
            rec->function_source && rec->io_list && rec->misc && rec->src_file &&
            rec->include_headers && rec->include_sources && rec->raw_json;
  if (!ok) creal_record_free(rec);
  return ok;
}

static const char *creal_database_array(const char *json) {
  const char *p = skip_ws_const(json);
  if (!p) return NULL;
  if (*p == '[') return p;
  if (*p == '{') {
    const char *arr = json_value_after_key(json, "functions");
    if (arr && *arr == '[') return arr;
    arr = json_value_after_key(json, "entries");
    if (arr && *arr == '[') return arr;
  }
  return NULL;
}

static char *make_creal_entry_id(const creal_record_t *rec, const char *source_hash,
                                 const char *sample_hash) {
  str_buf_t raw = {0};
  (void)sb_append(&raw, "creal:");
  (void)sb_append(&raw, rec->function_name ? rec->function_name : "");
  (void)sb_append_c(&raw, ':');
  (void)sb_append(&raw, rec->src_file ? rec->src_file : "");
  (void)sb_append_c(&raw, ':');
  (void)sb_append(&raw, source_hash ? source_hash : "");
  (void)sb_append_c(&raw, ':');
  (void)sb_append(&raw, sample_hash ? sample_hash : "");
  uint64_t a = fnv1a64(raw.data ? raw.data : "", raw.len);
  (void)sb_append(&raw, ":nynth-core-creal");
  uint64_t b = fnv1a64(raw.data ? raw.data : "", raw.len);
  char *id = NULL;
  (void)asprintf(&id, "%016" PRIx64 "%04" PRIx64, a, b & UINT64_C(0xffff));
  free(raw.data);
  return id;
}

static const char *creal_features_json(void) {
  return "[\"creal\",\"real-c\",\"function-db\",\"sample-io\",\"native-c-harness\"]";
}

static char *default_creal_db_path(const char *root) {
  (void)root;
  char *path = NULL;
  (void)nynth_asprintf(&path, "corpus/creal/functions_pointer_global_io.json");
  return path;
}

static char *default_creal_corpus_dir(const char *root) {
  (void)root;
  char *path = NULL;
  (void)nynth_asprintf(&path, "corpus/creal");
  return path;
}

static const char *creal_cc_from_args(int argc, char **argv, int arg_start) {
  const char *env = getenv("CC");
  return value_after(argc, argv, arg_start, "--cc", env && *env ? env : "cc");
}

static creal_exec_result_t creal_compile_and_run(const char *root, const char *cc,
                                                 const char *c_path, const char *build_dir,
                                                 const char *expected, double timeout_s) {
  creal_exec_result_t result;
  memset(&result, 0, sizeof(result));
  if (!mkdir_p(build_dir) || asprintf(&result.exe_path, "%s/case", build_dir) < 0) {
    result.compile.rc = 1;
    result.compile.err = strdup("failed to prepare creal build directory");
    return result;
  }
  char *compile_argv[] = {
    (char *)(cc && *cc ? cc : "cc"),
    "-std=c99",
    "-O2",
    "-w",
    (char *)c_path,
    "-o",
    result.exe_path,
    NULL
  };
  result.compile = run_proc(compile_argv, root, timeout_s);
  result.compile_ok = result.compile.rc == 0;
  result.worker_ms += result.compile.elapsed_ms;
  if (!result.compile_ok) return result;
  result.run = run_binary_many_native(root, result.exe_path, timeout_s, 1, 0);
  result.run_ok = result.run.rc == 0;
  result.worker_ms += result.run.median_ms;
  result.output_ok = result.run_ok && (!expected || !*expected ||
                                       strcmp(expected, result.run.normalized ? result.run.normalized : "") == 0);
  result.ok = result.compile_ok && result.run_ok && result.output_ok;
  return result;
}

static void append_creal_failure_json(str_buf_t *b, const creal_exec_result_t *exec,
                                      const char *reject_reason, const char *expected) {
  bool first = true;
#define CREAL_SEP() do { if (!first) (void)sb_append_c(b, ','); first = false; } while (0)
  if (reject_reason && *reject_reason) {
    CREAL_SEP();
    (void)sb_append(b, "{\"tool\":\"nynth_core\",\"phase\":\"creal-db-filter\",\"engine\":\"nynth_core\",\"rc\":1,\"reason\":");
    (void)sb_append_json_str(b, reject_reason);
    (void)sb_append_c(b, '}');
  } else if (exec && !exec->compile_ok) {
    CREAL_SEP();
    (void)sb_append(&*b, "{\"tool\":\"cc\",\"phase\":\"creal-c-compile\",\"engine\":\"nynth_core\",\"rc\":");
    (void)sb_appendf(b, "%d,\"reason\":", exec->compile.rc);
    (void)sb_append_json_str(b, exec->compile.err && *exec->compile.err ? exec->compile.err : "C compile failed");
    append_proc_tail_fields(b, &exec->compile);
    (void)sb_append_c(b, '}');
  } else if (exec && !exec->run_ok) {
    CREAL_SEP();
    (void)sb_append(b, "{\"tool\":\"c\",\"phase\":\"creal-c-run\",\"engine\":\"nynth_core\",\"rc\":");
    (void)sb_appendf(b, "%d,\"reason\":", exec->run.rc);
    (void)sb_append_json_str(b, exec->run.err && *exec->run.err ? exec->run.err : "C run failed");
    if (exec->run.out && *exec->run.out) {
      (void)sb_append(b, ",\"stdout\":");
      (void)sb_append_json_str(b, exec->run.out);
    }
    if (exec->run.err && *exec->run.err) {
      (void)sb_append(b, ",\"stderr\":");
      (void)sb_append_json_str(b, exec->run.err);
    }
    (void)sb_append_c(b, '}');
  } else if (exec && !exec->output_ok) {
    CREAL_SEP();
    (void)sb_append(b, "{\"tool\":\"c\",\"phase\":\"creal-output\",\"engine\":\"nynth_core\",\"rc\":1,\"reason\":\"output mismatch\",\"expected\":");
    (void)sb_append_json_str(b, expected ? expected : "");
    (void)sb_append(b, ",\"observed\":");
    (void)sb_append_json_str(b, exec->run.normalized ? exec->run.normalized : "");
    (void)sb_append_c(b, '}');
  }
#undef CREAL_SEP
}

static char *make_creal_manifest_entry(const creal_record_t *rec, const char *id,
                                       const char *case_name, const char *db_path,
                                       const char *dst_c, const char *source_hash,
                                       const char *sample_hash, const char *expected,
                                       int io_samples, double worker_ms) {
  char *feature_key = feature_key_from_array_text(creal_features_json());
  str_buf_t entry = {0};
  (void)sb_append(&entry, "{\"id\":");
  (void)sb_append_json_str(&entry, id);
  (void)sb_append(&entry, ",\"case\":");
  (void)sb_append_json_str(&entry, case_name);
  (void)sb_append(&entry, ",\"shape\":\"creal-function\",\"family\":\"creal-function\","
                  "\"lane\":\"creal\",\"generator\":\"creal\",\"generator_kind\":\"creal\","
                  "\"method\":\"creal\",\"source_kind\":\"creal-function-db\",\"shape_source\":");
  (void)sb_append_json_str(&entry, db_path ? db_path : "");
  (void)sb_append(&entry, ",\"shape_hash\":");
  (void)sb_append_json_str(&entry, source_hash ? source_hash : "");
  (void)sb_append(&entry, ",\"shape_dsl_version\":1,\"template\":\"creal-function-db\","
                  "\"features\":");
  (void)sb_append(&entry, creal_features_json());
  (void)sb_append(&entry, ",\"profile\":\"creal\",\"seed\":null,\"structural_hash\":");
  (void)sb_append_json_str(&entry, source_hash ? source_hash : "");
  (void)sb_append(&entry, ",\"behavior_hash\":");
  (void)sb_append_json_str(&entry, sample_hash ? sample_hash : "");
  (void)sb_append(&entry, ",\"sample_output_hash\":");
  (void)sb_append_json_str(&entry, sample_hash ? sample_hash : "");
  (void)sb_append(&entry, ",\"c_emitter_hash\":");
  (void)sb_append_json_str(&entry, source_hash ? source_hash : "");
  (void)sb_append(&entry, ",\"ny_emitter_hash\":\"\",\"expected_output\":");
  (void)sb_append_json_str(&entry, expected ? expected : "");
  (void)sb_append(&entry, ",\"ratio_sample\":null,\"ir_blocker_key\":\"\",\"feature_key\":");
  (void)sb_append_json_str(&entry, feature_key ? feature_key : "");
  char blocker_hash[32];
  snprintf(blocker_hash, sizeof(blocker_hash), "%016" PRIx64, fnv1a64("", 0));
  (void)sb_append(&entry, ",\"blocker_hash\":");
  (void)sb_append_json_str(&entry, blocker_hash);
  (void)sb_appendf(&entry, ",\"timing_history\":[],\"promotion_reason\":\"creal-db-build\","
                   "\"last_replay\":{\"ok\":true,\"expected_output\":");
  (void)sb_append_json_str(&entry, expected ? expected : "");
  (void)sb_append(&entry, "},\"note\":\"creal-db-build\",\"c\":");
  (void)sb_append_json_str(&entry, dst_c ? dst_c : "");
  (void)sb_append(&entry, ",\"creal\":{\"function_name\":");
  (void)sb_append_json_str(&entry, rec->function_name ? rec->function_name : "");
  (void)sb_append(&entry, ",\"return_type\":");
  (void)sb_append_json_str(&entry, rec->return_type ? rec->return_type : "");
  (void)sb_append(&entry, ",\"parameter_types\":");
  (void)sb_append(&entry, rec->parameter_types ? rec->parameter_types : "[]");
  (void)sb_append(&entry, ",\"src_file\":");
  (void)sb_append_json_str(&entry, rec->src_file ? rec->src_file : "");
  (void)sb_appendf(&entry, ",\"io_samples\":%d,\"validation_ms\":%.2f,\"compat_level\":\"function-db-sample-io\","
                   "\"function_db\":", io_samples, worker_ms);
  (void)sb_append_json_str(&entry, db_path ? db_path : "");
  (void)sb_append(&entry, "},\"paths\":{\"c\":");
  (void)sb_append_json_str(&entry, dst_c ? dst_c : "");
  (void)sb_append(&entry, ",\"function_db\":");
  (void)sb_append_json_str(&entry, db_path ? db_path : "");
  (void)sb_append(&entry, "}}");
  free(feature_key);
  return sb_take(&entry);
}

static char *make_creal_row(const creal_record_t *rec, const char *id, const char *case_name,
                            const char *db_path, const char *c_path, const char *source_hash,
                            const char *sample_hash, const char *expected,
                            const creal_exec_result_t *exec, const char *reject_reason,
                            bool promoted, bool duplicate, int io_samples) {
  bool ok = duplicate || (exec && exec->ok && promoted);
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"ok\":");
  (void)sb_append(&row, ok ? "true" : "false");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\",\"case\":");
  (void)sb_append_json_str(&row, case_name ? case_name : (rec && rec->function_name ? rec->function_name : "creal"));
  (void)sb_append(&row, ",\"id\":");
  (void)sb_append_json_str(&row, id ? id : "");
  (void)sb_append(&row, ",\"generator\":\"creal\",\"generator_kind\":\"creal\",\"method\":\"creal\","
                  "\"source_kind\":\"creal-function-db\",\"shape\":\"creal-function\","
                  "\"family\":\"creal-function\",\"features\":");
  (void)sb_append(&row, creal_features_json());
  (void)sb_append(&row, ",\"function_name\":");
  (void)sb_append_json_str(&row, rec && rec->function_name ? rec->function_name : "");
  (void)sb_append(&row, ",\"return_type\":");
  (void)sb_append_json_str(&row, rec && rec->return_type ? rec->return_type : "");
  (void)sb_append(&row, ",\"parameter_types\":");
  (void)sb_append(&row, rec && rec->parameter_types ? rec->parameter_types : "[]");
  (void)sb_append(&row, ",\"src_file\":");
  (void)sb_append_json_str(&row, rec && rec->src_file ? rec->src_file : "");
  (void)sb_append(&row, ",\"db_source\":");
  (void)sb_append_json_str(&row, db_path ? db_path : "");
  (void)sb_append(&row, ",\"c_source\":");
  (void)sb_append_json_str(&row, c_path ? c_path : "");
  (void)sb_append(&row, ",\"expected_output\":");
  (void)sb_append_json_str(&row, expected ? expected : "");
  (void)sb_append(&row, ",\"normalized_output\":");
  (void)sb_append_json_str(&row, exec && exec->run.normalized ? exec->run.normalized : "");
  (void)sb_append(&row, ",\"structural_hash\":");
  (void)sb_append_json_str(&row, source_hash ? source_hash : "");
  (void)sb_append(&row, ",\"behavior_hash\":");
  (void)sb_append_json_str(&row, sample_hash ? sample_hash : "");
  (void)sb_append(&row, ",\"sample_output_hash\":");
  (void)sb_append_json_str(&row, sample_hash ? sample_hash : "");
  (void)sb_appendf(&row, ",\"io_samples\":%d,\"worker_ms\":%.2f,\"promoted\":%s,\"duplicate\":%s",
                   io_samples, exec ? exec->worker_ms : 0.0,
                   promoted ? "true" : "false", duplicate ? "true" : "false");
  (void)sb_append(&row, ",\"variants\":[{\"tool\":\"cc\",\"flavor\":\"o2\",\"compile_ms\":");
  if (exec) (void)sb_appendf(&row, "%.2f,\"rc\":%d", exec->compile.elapsed_ms, exec->compile.rc);
  else (void)sb_append(&row, "0,\"rc\":0");
  (void)sb_append(&row, "},{\"tool\":\"c\",\"flavor\":\"run\",\"run_ms\":");
  if (exec) (void)sb_appendf(&row, "%.2f,\"output\":", exec->run.median_ms);
  else (void)sb_append(&row, "0,\"output\":");
  (void)sb_append_json_str(&row, exec && exec->run.normalized ? exec->run.normalized : "");
  (void)sb_append(&row, "}],\"ratios\":{},\"shape_counts\":{},\"ir_analysis\":{},\"failures\":[");
  if (!ok) append_creal_failure_json(&row, exec, reject_reason, expected);
  (void)sb_append(&row, "]}");
  return sb_take(&row);
}

static char *build_creal_import_report_json(const string_list_t *rows, const string_list_t *failures,
                                            const char *db_path, const char *corpus_dir,
                                            int scanned, int supported, int validated,
                                            int promoted, int duplicates, int rejected,
                                            int max_io, const char *cc) {
  str_buf_t b = {0};
  int cases = rows ? rows->count : 0;
  int failure_count = failures ? failures->count : 0;
  int ok_count = cases - failure_count;
  if (ok_count < 0) ok_count = 0;
  (void)sb_append(&b, "{\"rows\":");
  append_raw_json_list(&b, rows);
  (void)sb_append(&b, ",\"failures\":");
  append_raw_json_list(&b, failures);
  (void)sb_appendf(&b,
                   ",\"ok\":%s,\"cases\":%d,\"ok_count\":%d,"
                   "\"failure_count\":%d",
                   failure_count == 0 ? "true" : "false", cases, ok_count,
                   failure_count);
  (void)sb_appendf(&b, ",\"summary\":{\"cases\":%d,\"ok\":%d,\"ok_count\":%d,"
                   "\"scanned\":%d,\"supported\":%d,\"validated\":%d,"
                   "\"promoted\":%d,\"duplicates\":%d,\"rejected\":%d,"
                   "\"failure_count\":%d,"
                   "\"generator\":\"creal\",\"generator_kind\":\"creal\",\"method\":\"creal\","
                   "\"max_io\":%d,\"cc\":",
                   cases, ok_count, ok_count, scanned, supported, validated,
                   promoted, duplicates, rejected, failure_count, max_io);
  (void)sb_append_json_str(&b, cc && *cc ? cc : "cc");
  (void)sb_append(&b, ",\"function_db\":");
  (void)sb_append_json_str(&b, db_path ? db_path : "");
  (void)sb_append(&b, ",\"corpus_dir\":");
  (void)sb_append_json_str(&b, corpus_dir ? corpus_dir : "");
  (void)sb_append(&b, ",\"engine\":\"nynth_core\",\"native_workers\":{\"native_generation\":");
  (void)sb_appendf(&b, "%d,\"native_compare\":0,\"native_replay\":%d}},\"meta\":{\"engine\":\"nynth_core\"}}",
                   supported, validated);
  return sb_take(&b);
}

static bool entry_is_creal_db(const char *entry_json) {
  char *method = json_method_or_generator(entry_json ? entry_json : "{}");
  bool real = method && strcmp(method, "creal") == 0;
  free(method);
  return real;
}

static int count_creal_entries_for_dir(const char *corpus_dir) {
  manifest_entry_list_t entries = {0};
  int count = 0;
  if (load_manifest_entries(corpus_dir, &entries)) {
    for (int i = 0; i < entries.count; ++i)
      if (entry_is_creal_db(entries.items[i].json)) ++count;
  }
  manifest_entry_list_free(&entries);
  return count;
}

static char *make_creal_db_replay_row(const char *root, const char *entry_id,
                                      const char *entry_json, const char *corpus_dir,
                                      double timeout_s) {
  char *case_name = json_string_or_empty(entry_json, "case");
  char *features = json_array_or_empty(entry_json, "features");
  char *expected = json_string_or_empty(entry_json, "expected_output");
  char *sample_hash = json_string_or_empty(entry_json, "sample_output_hash");
  char *source_hash = json_string_or_empty(entry_json, "structural_hash");
  char *function_name = json_string_or_empty(entry_json, "function_name");
  if (!function_name || !*function_name) {
    free(function_name);
    function_name = json_string_or_empty(entry_json, "function_name");
  }
  char *c_ref = json_string_or_empty(entry_json, "c");
  char *c_path = resolve_existing_file(root, c_ref);
  if (!c_path) c_path = entry_default_path(corpus_dir, entry_id, "case.c");
  if (!path_exists_file(c_path)) {
    char *row = make_worker_failure_row(case_name && *case_name ? case_name : entry_id,
                                        "creal-replay", 1, "", "missing Creal C source");
    char *with_id = row_with_id_field(row, entry_id);
    free(row);
    free(case_name); free(features); free(expected); free(sample_hash);
    free(source_hash); free(function_name); free(c_ref); free(c_path);
    return with_id;
  }

  char *build_dir = NULL;
  (void)asprintf(&build_dir, "%s/build/replay_%ld/%s", corpus_dir, (long)getpid(), entry_id);
  const char *cc = getenv("CC");
  if (!cc || !*cc) cc = "cc";
  creal_exec_result_t exec = creal_compile_and_run(root, cc, c_path, build_dir ? build_dir : "/tmp",
                                                   expected ? expected : "", timeout_s);
  bool ok = exec.ok;
  char observed_hash[32];
  snprintf(observed_hash, sizeof(observed_hash), "%016" PRIx64,
           fnv1a64(exec.run.normalized ? exec.run.normalized : "",
                   strlen(exec.run.normalized ? exec.run.normalized : "")));
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"ok\":");
  (void)sb_append(&row, ok ? "true" : "false");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\",\"case\":");
  (void)sb_append_json_str(&row, case_name && *case_name ? case_name : entry_id);
  (void)sb_append(&row, ",\"id\":");
  (void)sb_append_json_str(&row, entry_id);
  (void)sb_append(&row, ",\"generator\":\"creal\",\"generator_kind\":\"creal\",\"method\":\"creal\","
                  "\"source_kind\":\"creal-function-db\",\"features\":");
  (void)sb_append(&row, features && *features ? features : creal_features_json());
  (void)sb_append(&row, ",\"function_name\":");
  (void)sb_append_json_str(&row, function_name ? function_name : "");
  (void)sb_append(&row, ",\"c_source\":");
  (void)sb_append_json_str(&row, c_path ? c_path : "");
  (void)sb_append(&row, ",\"expected_output\":");
  (void)sb_append_json_str(&row, expected ? expected : "");
  (void)sb_append(&row, ",\"normalized_output\":");
  (void)sb_append_json_str(&row, exec.run.normalized ? exec.run.normalized : "");
  (void)sb_append(&row, ",\"behavior_hash\":");
  (void)sb_append_json_str(&row, sample_hash && *sample_hash ? sample_hash : observed_hash);
  (void)sb_append(&row, ",\"sample_output_hash\":");
  (void)sb_append_json_str(&row, sample_hash && *sample_hash ? sample_hash : observed_hash);
  (void)sb_append(&row, ",\"structural_hash\":");
  (void)sb_append_json_str(&row, source_hash ? source_hash : "");
  (void)sb_appendf(&row, ",\"worker_ms\":%.2f,\"variants\":[{\"tool\":\"cc\",\"flavor\":\"o2\",\"compile_ms\":%.2f,\"rc\":%d},"
                   "{\"tool\":\"c\",\"flavor\":\"run\",\"run_ms\":%.2f,\"output\":",
                   exec.worker_ms, exec.compile.elapsed_ms, exec.compile.rc, exec.run.median_ms);
  (void)sb_append_json_str(&row, exec.run.normalized ? exec.run.normalized : "");
  (void)sb_append(&row, "}],\"ratios\":{},\"shape_counts\":{},\"ir_analysis\":{},\"failures\":[");
  if (!ok) append_creal_failure_json(&row, &exec, NULL, expected);
  (void)sb_append(&row, "]}");
  char *out = sb_take(&row);
  creal_exec_result_free(&exec);
  free(case_name); free(features); free(expected); free(sample_hash);
  free(source_hash); free(function_name); free(c_ref); free(c_path); free(build_dir);
  return out;
}

static int cmd_public_corpus_creal_build(int argc, char **argv) {
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  int arg_start = 4;
  char *default_db = default_creal_db_path(root);
  char *default_corpus = default_creal_corpus_dir(root);
  const char *env_db = getenv("CREAL_FUNCTION_DB_FILE");
  const char *db_path = value_after(argc, argv, arg_start, "--function-db",
                                    env_db && *env_db ? env_db : (default_db ? default_db : ""));
  const char *corpus_dir = value_after(argc, argv, arg_start, "--corpus-dir",
                                       default_corpus ? default_corpus : "");
  const char *json_path = value_after(argc, argv, arg_start, "--json", "");
  const char *cc = creal_cc_from_args(argc, argv, arg_start);
  int limit = atoi(value_after(argc, argv, arg_start, "--limit", "0"));
  if (limit <= 0) limit = has_flag_after(argc, argv, arg_start, "--fast") ? 8 : 32;
  int max_io = atoi(value_after(argc, argv, arg_start, "--max-io", "5"));
  if (max_io < 1) max_io = 1;
  double timeout_s = atof(value_after(argc, argv, arg_start, "--timeout-s", "30"));
  file_buf_t db = {0};
  if (!db_path || !*db_path || !read_file(db_path, &db)) {
    printf("{\"ok\":false,\"error\":\"creal-db-read-failed\",\"function_db\":");
    json_str(stdout, db_path ? db_path : "");
    printf(",\"hint\":\"download databaseconstructor/functions_pointer_global_io.json and pass --function-db or set CREAL_FUNCTION_DB_FILE\"}\n");
    free(default_db); free(default_corpus);
    return 1;
  }
  const char *arr = creal_database_array(db.data);
  const char *arr_end = arr && *arr == '[' ? matching_json_end(arr, '[', ']') : NULL;
  if (!arr || !arr_end) {
    printf("{\"ok\":false,\"error\":\"creal-db-format\",\"reason\":\"expected top-level array or functions array\"}\n");
    free(db.data); free(default_db); free(default_corpus);
    return 1;
  }
  manifest_entry_list_t manifest = {0};
  if (!load_manifest_entries(corpus_dir, &manifest)) {
    printf("{\"ok\":false,\"error\":\"manifest-read-failed\",\"corpus_dir\":");
    json_str(stdout, corpus_dir);
    printf("}\n");
    free(db.data); free(default_db); free(default_corpus);
    return 1;
  }

  string_list_t rows = {0}, failures = {0};
  int scanned = 0, supported = 0, validated = 0, promoted = 0, duplicates = 0, rejected = 0;
  bool manifest_dirty = false;
  const char *p = arr + 1;
  int scan_cap = atoi(value_after(argc, argv, arg_start, "--scan-limit", "0"));
  if (scan_cap <= 0) scan_cap = limit * 20;
  if (scan_cap < limit) scan_cap = limit;
  while (p < arr_end && (promoted + duplicates) < limit && scanned < scan_cap) {
    p = skip_ws_const(p);
    if (p >= arr_end || *p == ']') break;
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '{') break;
    const char *obj_end = matching_json_end(p, '{', '}');
    if (!obj_end || obj_end > arr_end) break;
    ++scanned;
    creal_record_t rec;
    memset(&rec, 0, sizeof(rec));
    creal_io_sample_list_t samples = {0};
    creal_exec_result_t exec;
    memset(&exec, 0, sizeof(exec));
    char error[256] = {0};
    char *source = NULL, *expected = NULL, *id = NULL, *case_dir = NULL, *dst_c = NULL;
    char source_hash[32] = {0}, sample_hash[32] = {0};
    bool did_promote = false, duplicate = false;
    const char *reject_reason = NULL;

    if (!creal_parse_record(p, obj_end + 1, &rec)) {
      reject_reason = "invalid Creal record";
    } else if (!creal_parse_io_list(rec.io_list, &samples, max_io)) {
      reject_reason = "invalid or empty io_list";
    } else {
      expected = creal_expected_output(&samples);
      source = creal_build_c_source(&rec, &samples, error, sizeof(error));
      if (!source) reject_reason = error[0] ? error : "unsupported Creal record";
    }
    if (source && expected) {
      ++supported;
      snprintf(source_hash, sizeof(source_hash), "%016" PRIx64, fnv1a64(source, strlen(source)));
      snprintf(sample_hash, sizeof(sample_hash), "%016" PRIx64, fnv1a64(expected, strlen(expected)));
      id = make_creal_entry_id(&rec, source_hash, sample_hash);
      char stem[128];
      safe_stem(stem, sizeof(stem), rec.function_name ? rec.function_name : "creal");
      char case_name[192];
      snprintf(case_name, sizeof(case_name), "creal_%s", stem);
      duplicate = id && manifest_contains_id(&manifest, id);
      if (duplicate) {
        ++duplicates;
      } else if (!id ||
                 asprintf(&case_dir, "%s/cases/%s", corpus_dir, id) < 0 ||
                 asprintf(&dst_c, "%s/case.c", case_dir) < 0 ||
                 !mkdir_p(case_dir) ||
                 !write_file_text(dst_c, source)) {
        reject_reason = "failed to write Creal C harness";
      } else {
        char *build_dir = NULL;
        (void)asprintf(&build_dir, "%s/build/import_%ld/%s", corpus_dir, (long)getpid(), id);
        exec = creal_compile_and_run(root, cc, dst_c, build_dir ? build_dir : "/tmp", expected, timeout_s);
        free(build_dir);
        if (exec.ok) {
          ++validated;
          char *entry_json = make_creal_manifest_entry(&rec, id, case_name, db_path, dst_c,
                                                       source_hash, sample_hash, expected,
                                                       samples.count, exec.worker_ms);
          char *id_copy = id ? strdup(id) : NULL;
          if (entry_json && id_copy && manifest_entry_list_push_take(&manifest, id_copy, entry_json)) {
            manifest_dirty = true;
            did_promote = true;
            ++promoted;
          } else {
            free(entry_json);
            free(id_copy);
            reject_reason = "manifest append failed";
          }
        }
      }
      if (!did_promote && !duplicate) ++rejected;
      char *row = make_creal_row(&rec, id, case_name, db_path, dst_c, source_hash, sample_hash,
                                 expected, source ? &exec : NULL, reject_reason,
                                 did_promote, duplicate, samples.count);
      (void)string_list_push_take(&rows, row);
    } else {
      ++rejected;
      char *row = make_creal_row(&rec, "", rec.function_name ? rec.function_name : "creal",
                                 db_path, "", "", "", expected ? expected : "",
                                 NULL, reject_reason, false, false, samples.count);
      (void)string_list_push_take(&rows, row);
    }
    creal_exec_result_free(&exec);
    creal_io_sample_list_free(&samples);
    creal_record_free(&rec);
    free(source); free(expected); free(id); free(case_dir); free(dst_c);
    p = obj_end + 1;
  }
  if (manifest_dirty && !save_manifest_entries(corpus_dir, &manifest)) {
    (void)string_list_push_take(&failures, strdup("{\"ok\":false,\"phase\":\"creal-manifest-save\",\"reason\":\"manifest write failed\"}"));
  }
  if (!rows.count) {
    (void)string_list_push_take(&rows, native_row_status("creal", "creal-db", false, "reason", "no Creal function candidates found"));
    (void)string_list_push_take(&failures, strdup("{\"ok\":false,\"phase\":\"creal-db\",\"reason\":\"no candidates\"}"));
  } else if (promoted + duplicates <= 0) {
    (void)string_list_push_take(&failures, strdup("{\"ok\":false,\"phase\":\"creal-db\",\"reason\":\"no validated Creal functions\"}"));
  }
  char *report = build_creal_import_report_json(&rows, &failures, db_path, corpus_dir,
                                                scanned, supported, validated, promoted,
                                                duplicates, rejected, max_io, cc);
  int rc = emit_native_report(report, json_path, "creal import", rows.count, failures.count);
  string_list_free(&rows); string_list_free(&failures);
  manifest_entry_list_free(&manifest);
  free(db.data); free(default_db); free(default_corpus);
  return rc;
}

static const char *real_db_source_kind(const char *kind) {
  if (strcmp(kind ? kind : "", "functions") == 0) return "real-db-functions";
  if (strcmp(kind ? kind : "", "hosts") == 0) return "real-db-hosts";
  if (strcmp(kind ? kind : "", "mined-hosts") == 0) return "real-db-mined-hosts";
  return "real-db-source";
}

static const char *real_db_family(const char *kind) {
  if (strcmp(kind ? kind : "", "functions") == 0) return "real-db-functions";
  if (strcmp(kind ? kind : "", "hosts") == 0) return "real-db-hosts";
  if (strcmp(kind ? kind : "", "mined-hosts") == 0) return "real-db-mined-hosts";
  return "real-db";
}

static const char *real_db_features_json(const char *kind, bool run_samples) {
  if (strcmp(kind ? kind : "", "functions") == 0)
    return "[\"real-db\",\"ny-source\",\"library\",\"function-sample\"]";
  if (strcmp(kind ? kind : "", "hosts") == 0)
    return "[\"real-db\",\"ny-source\",\"runtime-host\",\"source-derived\",\"run-sample\"]";
  if (strcmp(kind ? kind : "", "mined-hosts") == 0)
    return run_samples ?
      "[\"real-db\",\"ny-source\",\"mined-host\",\"run-sample\"]" :
      "[\"real-db\",\"ny-source\",\"mined-host\",\"compile-sample\"]";
  return "[\"real-db\",\"ny-source\"]";
}

static char *make_real_db_entry_id(const char *kind, const char *rel_source,
                                   const char *source_hash, const char *sample_hash) {
  str_buf_t raw = {0};
  (void)sb_append(&raw, "real-db:");
  (void)sb_append(&raw, kind ? kind : "");
  (void)sb_append_c(&raw, ':');
  (void)sb_append(&raw, rel_source ? rel_source : "");
  (void)sb_append_c(&raw, ':');
  (void)sb_append(&raw, source_hash ? source_hash : "");
  (void)sb_append_c(&raw, ':');
  (void)sb_append(&raw, sample_hash ? sample_hash : "");
  uint64_t a = fnv1a64(raw.data ? raw.data : "", raw.len);
  (void)sb_append(&raw, ":nynth-core-real-db");
  uint64_t b = fnv1a64(raw.data ? raw.data : "", raw.len);
  char *id = NULL;
  (void)asprintf(&id, "%016" PRIx64 "%04" PRIx64, a, b & UINT64_C(0xffff));
  free(raw.data);
  return id;
}

static char *promote_real_db_source(const char *root, const char *corpus_dir,
                                    const char *kind, const char *source_path,
                                    const char *source_hash, const char *sample_hash,
                                    const char *expected_output, bool run_samples,
                                    double elapsed_ms, bool *ok_out,
                                    bool *promoted_out, bool *duplicate_out) {
  if (ok_out) *ok_out = false;
  if (promoted_out) *promoted_out = false;
  if (duplicate_out) *duplicate_out = false;
  char *rel_source = rel_path_dup(root, source_path);
  char *id = make_real_db_entry_id(kind, rel_source, source_hash, sample_hash);
  if (!id || !rel_source) {
    free(id); free(rel_source);
    return strdup("{\"ok\":false,\"promoted\":false,\"reason\":\"allocation-failed\"}");
  }

  manifest_entry_list_t entries = {0};
  if (!load_manifest_entries(corpus_dir, &entries)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"manifest-read-failed\",\"source\":");
    (void)sb_append_json_str(&fail, rel_source);
    (void)sb_append_c(&fail, '}');
    free(id); free(rel_source);
    return sb_take(&fail);
  }
  if (manifest_contains_id(&entries, id)) {
    if (ok_out) *ok_out = true;
    if (duplicate_out) *duplicate_out = true;
    str_buf_t dup = {0};
    (void)sb_append(&dup, "{\"ok\":true,\"promoted\":false,\"reason\":\"duplicate\",\"id\":");
    (void)sb_append_json_str(&dup, id);
    (void)sb_append(&dup, ",\"source\":");
    (void)sb_append_json_str(&dup, rel_source);
    (void)sb_append_c(&dup, '}');
    manifest_entry_list_free(&entries);
    free(id); free(rel_source);
    return sb_take(&dup);
  }

  char stem[128];
  safe_stem(stem, sizeof(stem), source_path);
  char *case_dir = NULL, *dst_ny = NULL;
  bool path_ok = asprintf(&case_dir, "%s/cases/%s", corpus_dir, id) >= 0 &&
                 asprintf(&dst_ny, "%s/case.ny", case_dir) >= 0;
  if (!path_ok || !mkdir_p(case_dir) || !copy_file_bytes(source_path, dst_ny)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"copy-failed\",\"id\":");
    (void)sb_append_json_str(&fail, id);
    (void)sb_append(&fail, ",\"source\":");
    (void)sb_append_json_str(&fail, rel_source);
    (void)sb_append_c(&fail, '}');
    manifest_entry_list_free(&entries);
    free(id); free(rel_source); free(case_dir); free(dst_ny);
    return sb_take(&fail);
  }

  const char *features = real_db_features_json(kind, run_samples);
  char *feature_key = feature_key_from_array_text(features);
  char blocker_hash[32];
  snprintf(blocker_hash, sizeof(blocker_hash), "%016" PRIx64, fnv1a64("", 0));
  char case_name[192];
  snprintf(case_name, sizeof(case_name), "real_%s", stem);
  str_buf_t entry = {0};
  (void)sb_append(&entry, "{\"id\":");
  (void)sb_append_json_str(&entry, id);
  (void)sb_append(&entry, ",\"case\":");
  (void)sb_append_json_str(&entry, case_name);
  (void)sb_append(&entry, ",\"shape\":");
  (void)sb_append_json_str(&entry, "real-db-source");
  (void)sb_append(&entry, ",\"family\":");
  (void)sb_append_json_str(&entry, real_db_family(kind));
  (void)sb_append(&entry, ",\"lane\":\"real-db\",\"generator\":\"real-db\","
                  "\"generator_kind\":\"real-db\",\"method\":\"real-db\",\"source_kind\":");
  (void)sb_append_json_str(&entry, real_db_source_kind(kind));
  (void)sb_append(&entry, ",\"shape_source\":");
  (void)sb_append_json_str(&entry, rel_source);
  (void)sb_append(&entry, ",\"shape_hash\":");
  (void)sb_append_json_str(&entry, source_hash);
  (void)sb_append(&entry, ",\"shape_dsl_version\":1,\"template\":\"real-db-source\","
                  "\"features\":");
  (void)sb_append(&entry, features);
  (void)sb_append(&entry, ",\"profile\":\"real\",\"seed\":null,\"structural_hash\":");
  (void)sb_append_json_str(&entry, source_hash);
  (void)sb_append(&entry, ",\"behavior_hash\":");
  (void)sb_append_json_str(&entry, sample_hash);
  (void)sb_append(&entry, ",\"c_emitter_hash\":\"\",\"ny_emitter_hash\":");
  (void)sb_append_json_str(&entry, source_hash);
  (void)sb_append(&entry, ",\"expected_output\":");
  (void)sb_append_json_str(&entry, expected_output ? expected_output : "");
  (void)sb_append(&entry, ",\"ratio_sample\":null,\"ir_blocker_key\":\"\",\"feature_key\":");
  (void)sb_append_json_str(&entry, feature_key ? feature_key : "");
  (void)sb_append(&entry, ",\"blocker_hash\":");
  (void)sb_append_json_str(&entry, blocker_hash);
  (void)sb_appendf(&entry, ",\"timing_history\":[],\"promotion_reason\":\"real-db-build\","
                   "\"last_replay\":{\"ok\":true,\"expected_output\":");
  (void)sb_append_json_str(&entry, expected_output ? expected_output : "");
  (void)sb_append(&entry, "},\"note\":\"real-db-build\",\"real_db\":{\"kind\":");
  (void)sb_append_json_str(&entry, kind ? kind : "");
  (void)sb_append(&entry, ",\"validation\":");
  (void)sb_append_json_str(&entry, run_samples ? "run" : "compile");
  (void)sb_append(&entry, ",\"source\":");
  (void)sb_append_json_str(&entry, rel_source);
  (void)sb_append(&entry, ",\"source_hash\":");
  (void)sb_append_json_str(&entry, source_hash);
  (void)sb_append(&entry, ",\"sample_output_hash\":");
  (void)sb_append_json_str(&entry, sample_hash);
  (void)sb_appendf(&entry, ",\"validation_ms\":%.2f},\"paths\":{\"ny\":", elapsed_ms);
  (void)sb_append_json_str(&entry, dst_ny);
  (void)sb_append(&entry, ",\"source\":");
  (void)sb_append_json_str(&entry, source_path);
  (void)sb_append(&entry, "}}");

  char *entry_json = sb_take(&entry);
  bool pushed_entry = false;
  if (entry_json) {
    char *id_copy = strdup(id);
    if (id_copy && manifest_entry_list_push_take(&entries, id_copy, entry_json)) {
      pushed_entry = true;
    } else {
      free(id_copy);
    }
  }
  if (!entry_json || !pushed_entry || !save_manifest_entries(corpus_dir, &entries)) {
    str_buf_t fail = {0};
    (void)sb_append(&fail, "{\"ok\":false,\"promoted\":false,\"reason\":\"manifest-write-failed\",\"id\":");
    (void)sb_append_json_str(&fail, id);
    (void)sb_append(&fail, ",\"source\":");
    (void)sb_append_json_str(&fail, rel_source);
    (void)sb_append_c(&fail, '}');
    if (!pushed_entry) free(entry_json);
    manifest_entry_list_free(&entries);
    free(id); free(rel_source); free(case_dir); free(dst_ny); free(feature_key);
    return sb_take(&fail);
  }

  if (ok_out) *ok_out = true;
  if (promoted_out) *promoted_out = true;
  str_buf_t result = {0};
  (void)sb_append(&result, "{\"ok\":true,\"promoted\":true,\"id\":");
  (void)sb_append_json_str(&result, id);
  (void)sb_append(&result, ",\"source\":");
  (void)sb_append_json_str(&result, rel_source);
  (void)sb_append(&result, ",\"case\":");
  (void)sb_append_json_str(&result, case_name);
  (void)sb_append(&result, ",\"method\":\"real-db\"}");
  manifest_entry_list_free(&entries);
  free(id); free(rel_source); free(case_dir); free(dst_ny); free(feature_key);
  return sb_take(&result);
}

static bool entry_is_real_db(const char *entry_json) {
  char *method = json_method_or_generator(entry_json ? entry_json : "{}");
  bool real = method && strcmp(method, "real-db") == 0;
  free(method);
  return real;
}

static int count_real_db_entries_in_manifest(const manifest_entry_list_t *entries) {
  int count = 0;
  for (int i = 0; entries && i < entries->count; ++i)
    if (entry_is_real_db(entries->items[i].json)) ++count;
  return count;
}

static int count_real_db_entries_for_dir(const char *corpus_dir) {
  manifest_entry_list_t entries = {0};
  int count = 0;
  if (load_manifest_entries(corpus_dir, &entries))
    count = count_real_db_entries_in_manifest(&entries);
  manifest_entry_list_free(&entries);
  return count;
}

static char *make_real_db_replay_row(const char *root, const char *ny_bin, const char *entry_id,
                                     const char *entry_json, const char *corpus_dir,
                                     const char *ny_override, double timeout_s) {
  char *case_name = json_string_or_empty(entry_json, "case");
  char *source_kind = json_string_or_empty(entry_json, "source_kind");
  char *features = json_array_or_empty(entry_json, "features");
  char *expected = json_string_or_empty(entry_json, "expected_output");
  char *validation = json_string_or_empty(entry_json, "validation");
  char *sample_hash = json_string_or_empty(entry_json, "sample_output_hash");
  char *source_hash = json_string_or_empty(entry_json, "source_hash");
  char *source_ref = json_string_or_empty(entry_json, "source");
  char *ny_path = resolve_existing_file(root, source_ref);
  if (!ny_path && ny_override && *ny_override) ny_path = strdup(ny_override);
  if (!ny_path) ny_path = json_string_or_empty(entry_json, "ny");
  if (!ny_path || !*ny_path) {
    free(ny_path);
    ny_path = entry_default_path(corpus_dir, entry_id, "case.ny");
  }
  if (!path_exists_file(ny_path)) {
    char *row = make_worker_failure_row(case_name && *case_name ? case_name : entry_id,
                                        "real-db-replay", 1, "", "missing real-db ny source");
    char *with_id = row_with_id_field(row, entry_id);
    free(row);
    free(case_name); free(source_kind); free(features); free(expected); free(validation);
    free(sample_hash); free(source_hash); free(source_ref); free(ny_path);
    return with_id;
  }

  bool run = validation && strcmp(validation, "run") == 0;
  proc_result_t pr = {0};
  bool ok_proc = compile_or_run_ny_source(root, ny_bin, ny_path, run, timeout_s, &pr);
  char *normalized = normalize_output_pair(pr.out, pr.err);
  bool output_ok = !expected || !*expected || strcmp(expected, normalized ? normalized : "") == 0;
  char observed_hash[32];
  snprintf(observed_hash, sizeof(observed_hash), "%016" PRIx64,
           fnv1a64(normalized ? normalized : "", strlen(normalized ? normalized : "")));
  bool ok = ok_proc && output_ok;

  str_buf_t row = {0};
  (void)sb_append(&row, "{\"ok\":");
  (void)sb_append(&row, ok ? "true" : "false");
  (void)sb_append(&row, ",\"engine\":\"nynth_core\",\"case\":");
  (void)sb_append_json_str(&row, case_name && *case_name ? case_name : entry_id);
  (void)sb_append(&row, ",\"id\":");
  (void)sb_append_json_str(&row, entry_id);
  (void)sb_append(&row, ",\"generator\":\"real-db\",\"generator_kind\":\"real-db\","
                  "\"method\":\"real-db\",\"source_kind\":");
  (void)sb_append_json_str(&row, source_kind && *source_kind ? source_kind : "real-db-source");
  (void)sb_append(&row, ",\"features\":");
  (void)sb_append(&row, features && *features ? features : "[]");
  (void)sb_append(&row, ",\"ny_source\":");
  (void)sb_append_json_str(&row, ny_path);
  (void)sb_append(&row, ",\"expected_output\":");
  (void)sb_append_json_str(&row, expected ? expected : "");
  (void)sb_append(&row, ",\"normalized_output\":");
  (void)sb_append_json_str(&row, normalized ? normalized : "");
  (void)sb_append(&row, ",\"behavior_hash\":");
  (void)sb_append_json_str(&row, observed_hash);
  (void)sb_append(&row, ",\"sample_output_hash\":");
  (void)sb_append_json_str(&row, sample_hash && *sample_hash ? sample_hash : observed_hash);
  (void)sb_append(&row, ",\"structural_hash\":");
  (void)sb_append_json_str(&row, source_hash ? source_hash : "");
  (void)sb_appendf(&row, ",\"worker_ms\":%.2f,\"variants\":[{\"tool\":\"ny\",\"flavor\":", pr.elapsed_ms);
  (void)sb_append_json_str(&row, run ? "run" : "compile");
  (void)sb_appendf(&row, ",\"run_ms\":%.2f,\"output\":", pr.elapsed_ms);
  (void)sb_append_json_str(&row, normalized ? normalized : "");
  (void)sb_append(&row, "}],\"ratios\":{},\"shape_counts\":{},\"ir_analysis\":{},\"failures\":[");
  if (!ok) {
    (void)sb_append(&row, "{\"tool\":\"ny\",\"phase\":\"real-db-replay\",\"engine\":\"nynth_core\",\"rc\":");
    (void)sb_appendf(&row, "%d,\"reason\":", pr.rc);
    (void)sb_append_json_str(&row, ok_proc ? "output mismatch" : (pr.err && *pr.err ? pr.err : "ny replay failed"));
    append_proc_tail_fields(&row, &pr);
    (void)sb_append_c(&row, '}');
  }
  (void)sb_append(&row, "]}");

  char *out = sb_take(&row);
  proc_result_free(&pr);
  free(case_name); free(source_kind); free(features); free(expected); free(validation);
  free(sample_hash); free(source_hash); free(source_ref); free(ny_path); free(normalized);
  return out;
}

static bool ensure_real_db_corpus(const char *root, const char *corpus_dir,
                                  int needed, bool fast, double timeout_s) {
  if (count_real_db_entries_for_dir(corpus_dir) >= needed) return true;
  char *json_path = make_tmp_json_path(root, "real_db_build", needed, 0);
  char limit_buf[32], timeout_buf[64];
  int limit = needed > 8 ? needed : 8;
  snprintf(limit_buf, sizeof(limit_buf), "%d", limit);
  snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
  char *argv_functions[16];
  int a = 0;
  argv_functions[a++] = g_self_path;
  argv_functions[a++] = "corpus";
  argv_functions[a++] = "build-functions";
  argv_functions[a++] = "--corpus-dir"; argv_functions[a++] = (char *)corpus_dir;
  argv_functions[a++] = "--limit"; argv_functions[a++] = limit_buf;
  argv_functions[a++] = "--timeout-s"; argv_functions[a++] = timeout_buf;
  if (json_path) { argv_functions[a++] = "--json"; argv_functions[a++] = json_path; }
  if (fast) argv_functions[a++] = "--fast";
  argv_functions[a] = NULL;
  proc_result_t pr = run_proc(argv_functions, root, worker_outer_timeout(timeout_s, 1, 0));
  proc_result_free(&pr);
  if (count_real_db_entries_for_dir(corpus_dir) >= needed) {
    free(json_path);
    return true;
  }
  char *argv_hosts[18];
  a = 0;
  argv_hosts[a++] = g_self_path;
  argv_hosts[a++] = "corpus";
  argv_hosts[a++] = "build-hosts";
  argv_hosts[a++] = "--corpus-dir"; argv_hosts[a++] = (char *)corpus_dir;
  argv_hosts[a++] = "--limit"; argv_hosts[a++] = limit_buf;
  argv_hosts[a++] = "--timeout-s"; argv_hosts[a++] = timeout_buf;
  if (json_path) { argv_hosts[a++] = "--json"; argv_hosts[a++] = json_path; }
  if (fast) argv_hosts[a++] = "--fast";
  argv_hosts[a] = NULL;
  pr = run_proc(argv_hosts, root, worker_outer_timeout(timeout_s, 1, 0));
  proc_result_free(&pr);
  free(json_path);
  return count_real_db_entries_for_dir(corpus_dir) > 0;
}

static char *build_synth_real_report_json(const report_rows_t *report, const string_list_t *generated,
                                          const char *corpus_dir, const char *out_dir,
                                          int requested_cases, int seed, bool fast,
                                          bool built_if_missing) {
  str_buf_t b = {0};
  int ok_cases = report->rows.count - report->failed_rows;
  if (ok_cases < 0) ok_cases = 0;
  (void)sb_append(&b, "{\"rows\":");
  append_rows_json(&b, &report->rows);
  (void)sb_append(&b, ",\"failures\":[");
  if (report->failures_json.data) (void)sb_append(&b, report->failures_json.data);
  (void)sb_appendf(&b,
                   "],\"ok\":%s,\"cases\":%d,\"ok_count\":%d,"
                   "\"failure_count\":%d,\"summary\":{\"cases\":%d,"
                   "\"ok\":%d,\"ok_count\":%d,\"ok_cases\":%d,"
                   "\"generated_cases\":%d,"
                   "\"failure_count\":%d,\"seed\":%d,\"fast\":%s,\"built_if_missing\":%s,"
                   "\"generator\":\"real-db\",\"generator_kind\":\"real-db\",\"method\":\"real-db\","
                   "\"corpus_dir\":",
                   report->failure_count == 0 ? "true" : "false",
                   requested_cases, ok_cases, report->failure_count,
                   requested_cases, ok_cases, ok_cases, ok_cases,
                   generated->count, report->failure_count, seed,
                   fast ? "true" : "false", built_if_missing ? "true" : "false");
  (void)sb_append_json_str(&b, corpus_dir);
  (void)sb_append(&b, ",\"generated_dir\":");
  (void)sb_append_json_str(&b, out_dir);
  (void)sb_appendf(&b, ",\"jobs\":1,\"engine\":\"nynth_core\",\"native_workers\":{\"native_replay\":%d,"
                   "\"native_compare\":0,\"native_generation\":%d}},\"generated_cases\":",
                   report->rows.count, built_if_missing ? 1 : 0);
  append_raw_json_list(&b, generated);
  (void)sb_append(&b, ",\"meta\":{\"engine\":\"nynth_core\",\"source\":\"real-db-manifest\"}}");
  return sb_take(&b);
}

static int run_synth_generate_alias(int argc, char **argv, const char *alias, const char *generator,
                                    int default_rounds) {
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  const char *rounds_text = value_after(argc, argv, 3, "--rounds", "");
  if (!rounds_text || !*rounds_text) rounds_text = value_after(argc, argv, 3, "--cases", "");
  int rounds = rounds_text && *rounds_text ? atoi(rounds_text) : default_rounds;
  if (rounds < 1) rounds = 1;
  if (has_flag_after(argc, argv, 3, "--fast") && rounds > 8) rounds = 8;
  const char *seed = value_after(argc, argv, 3, "--seed", "1337");
  const char *timeout_s = value_after(argc, argv, 3, "--timeout-s", "90");
  const char *profile = value_after(argc, argv, 3, "--profile", "balanced");
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *out_arg = value_after(argc, argv, 3, "--out", "");
  char rounds_buf[32];
  snprintf(rounds_buf, sizeof(rounds_buf), "%d", rounds);
  char *default_out = NULL;
  if (!out_arg || !*out_arg) {
    (void)nynth_asprintf(&default_out, "build/generated/%s_native", alias);
    out_arg = default_out ? default_out : "";
  }
  char *sub_argv[28];
  int a = 0;
  sub_argv[a++] = g_self_path;
  sub_argv[a++] = "synth";
  sub_argv[a++] = "generate";
  sub_argv[a++] = "--cases"; sub_argv[a++] = rounds_buf;
  sub_argv[a++] = "--seed"; sub_argv[a++] = (char *)seed;
  sub_argv[a++] = "--profile"; sub_argv[a++] = (char *)profile;
  sub_argv[a++] = "--generator"; sub_argv[a++] = (char *)generator;
  sub_argv[a++] = "--timeout-s"; sub_argv[a++] = (char *)timeout_s;
  sub_argv[a++] = "--out"; sub_argv[a++] = (char *)out_arg;
  sub_argv[a++] = "--json"; sub_argv[a++] = (char *)json_path;
  if (has_flag_after(argc, argv, 3, "--fast")) sub_argv[a++] = "--fast";
  sub_argv[a] = NULL;
  proc_result_t pr = run_proc(sub_argv, root, worker_outer_timeout(atof(timeout_s), 1, 0));
  if (pr.out) fputs(pr.out, stdout);
  if (pr.err) fputs(pr.err, stderr);
  int rc = pr.rc;
  proc_result_free(&pr);
  free(default_out);
  return rc;
}

static int cmd_public_synth_random(int argc, char **argv) {
  return run_synth_generate_alias(argc, argv, "random", "mixed", 24);
}

static int cmd_public_synth_real(int argc, char **argv) {
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
  const char *rounds_text = value_after(argc, argv, 3, "--rounds", "");
  if (!rounds_text || !*rounds_text) rounds_text = value_after(argc, argv, 3, "--cases", "");
  int cases = rounds_text && *rounds_text ? atoi(rounds_text) : 8;
  if (cases < 1) cases = 1;
  if (fast && cases > 8) cases = 8;
  int limit = atoi(value_after(argc, argv, 3, "--limit", "0"));
  if (limit > 0 && cases > limit) cases = limit;
  int seed = atoi(value_after(argc, argv, 3, "--seed", "1337"));
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "90"));
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  char *default_corpus = NULL, *default_out = NULL;
  bool paths_ok = nynth_asprintf(&default_corpus, "corpus/real-db") >= 0 &&
                  nynth_asprintf(&default_out, "build/generated/nyreal_native") >= 0;
  if (!paths_ok) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(default_corpus); free(default_out);
    return 2;
  }
  const char *corpus_dir = value_after(argc, argv, 3, "--corpus-dir", default_corpus);
  const char *out_dir = value_after(argc, argv, 3, "--out", default_out);
  int before_entries = count_real_db_entries_for_dir(corpus_dir);
  bool built_if_missing = before_entries < cases;
  bool ensured = ensure_real_db_corpus(root, corpus_dir, cases, fast, timeout_s);

  manifest_entry_list_t entries = {0};
  report_rows_t report;
  memset(&report, 0, sizeof(report));
  string_list_t generated = {0};
  if (!ensured || !load_manifest_entries(corpus_dir, &entries)) {
    char *row = make_worker_failure_row("synth-real", "real-db-load", 1, "",
                                        ensured ? "manifest read failed" : "no real DB entries available");
    report_add_row(&report, row);
  } else {
    int *real_indexes = (int *)calloc((size_t)entries.count, sizeof(int));
    int real_count = 0;
    for (int i = 0; i < entries.count; ++i) {
      if (entry_is_real_db(entries.items[i].json)) real_indexes[real_count++] = i;
    }
    if (!real_indexes || real_count <= 0) {
      char *row = make_worker_failure_row("synth-real", "real-db-empty", 1, "",
                                          "no real DB entries available");
      report_add_row(&report, row);
    } else {
      int offset = seed >= 0 ? seed % real_count : (-seed) % real_count;
      for (int i = 0; i < cases; ++i) {
        int entry_idx = real_indexes[(offset + i) % real_count];
        const char *entry_json = entries.items[entry_idx].json;
        const char *entry_id = entries.items[entry_idx].id;
        char *case_name = json_string_or_empty(entry_json, "case");
        char *source_kind = json_string_or_empty(entry_json, "source_kind");
        char stem[128], generated_name[192];
        safe_stem(stem, sizeof(stem), case_name && *case_name ? case_name : entry_id);
        snprintf(generated_name, sizeof(generated_name), "real_%03d_%s", i, stem);
        char *src_ny = json_string_or_empty(entry_json, "ny");
        if (!src_ny || !*src_ny) {
          free(src_ny);
          src_ny = entry_default_path(corpus_dir, entry_id, "case.ny");
        }
        char *case_dir = NULL, *dst_ny = NULL;
        bool path_ok = asprintf(&case_dir, "%s/%s", out_dir, generated_name) >= 0 &&
                       asprintf(&dst_ny, "%s/%s.ny", case_dir, generated_name) >= 0;
        if (!path_ok || !mkdir_p(case_dir) || !copy_file_bytes(src_ny, dst_ny)) {
          char *row = make_worker_failure_row(generated_name, "real-db-copy", 1, "",
                                              "failed to copy real DB source");
          report_add_row(&report, row);
        } else {
          str_buf_t gen_case = {0};
          (void)sb_append(&gen_case, "{\"name\":");
          (void)sb_append_json_str(&gen_case, generated_name);
          (void)sb_append(&gen_case, ",\"id\":");
          (void)sb_append_json_str(&gen_case, entry_id);
          (void)sb_append(&gen_case, ",\"ny_source\":");
          (void)sb_append_json_str(&gen_case, dst_ny);
          (void)sb_append(&gen_case, ",\"generator\":\"real-db\",\"generator_kind\":\"real-db\","
                          "\"method\":\"real-db\",\"source_kind\":");
          (void)sb_append_json_str(&gen_case, source_kind && *source_kind ? source_kind : "real-db-source");
          (void)sb_append_c(&gen_case, '}');
          (void)string_list_push_take(&generated, sb_take(&gen_case));
          char *row = make_real_db_replay_row(root, ny_bin, entry_id, entry_json,
                                              corpus_dir, NULL, timeout_s);
          report_add_row(&report, row);
        }
        free(case_name); free(source_kind); free(src_ny); free(case_dir); free(dst_ny);
      }
    }
    free(real_indexes);
  }

  char *report_json = build_synth_real_report_json(&report, &generated, corpus_dir, out_dir,
                                                   cases, seed, fast, built_if_missing);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    int ok_cases = report.rows.count - report.failed_rows;
    if (ok_cases < 0) ok_cases = 0;
    printf("real cases: %d/%d\n", ok_cases, report.rows.count);
    printf("failures: %d\n", report.failure_count);
  }
  int rc = report.failure_count ? 1 : 0;
  free(report_json);
  manifest_entry_list_free(&entries);
  report_rows_free(&report);
  string_list_free(&generated);
  free(default_corpus); free(default_out);
  return rc;
}

static char *build_synth_creal_report_json(const report_rows_t *report, const string_list_t *generated,
                                           const char *corpus_dir, const char *out_dir,
                                           const char *function_db, int requested_cases,
                                           int seed, bool fast, bool built_if_missing) {
  str_buf_t b = {0};
  int ok_cases = report->rows.count - report->failed_rows;
  if (ok_cases < 0) ok_cases = 0;
  (void)sb_append(&b, "{\"rows\":");
  append_rows_json(&b, &report->rows);
  (void)sb_append(&b, ",\"failures\":[");
  if (report->failures_json.data) (void)sb_append(&b, report->failures_json.data);
  (void)sb_appendf(&b,
                   "],\"ok\":%s,\"cases\":%d,\"ok_count\":%d,"
                   "\"failure_count\":%d,\"summary\":{\"cases\":%d,"
                   "\"ok\":%d,\"ok_count\":%d,\"ok_cases\":%d,"
                   "\"generated_cases\":%d,"
                   "\"failure_count\":%d,\"seed\":%d,\"fast\":%s,\"built_if_missing\":%s,"
                   "\"generator\":\"creal\",\"generator_kind\":\"creal\",\"method\":\"creal\","
                   "\"function_db\":",
                   report->failure_count == 0 ? "true" : "false",
                   requested_cases, ok_cases, report->failure_count,
                   requested_cases, ok_cases, ok_cases, ok_cases,
                   generated->count, report->failure_count, seed,
                   fast ? "true" : "false", built_if_missing ? "true" : "false");
  (void)sb_append_json_str(&b, function_db ? function_db : "");
  (void)sb_append(&b, ",\"corpus_dir\":");
  (void)sb_append_json_str(&b, corpus_dir);
  (void)sb_append(&b, ",\"generated_dir\":");
  (void)sb_append_json_str(&b, out_dir);
  (void)sb_appendf(&b, ",\"jobs\":1,\"engine\":\"nynth_core\",\"native_workers\":{\"native_replay\":%d,"
                   "\"native_compare\":0,\"native_generation\":%d}},\"generated_cases\":",
                   report->rows.count, built_if_missing ? 1 : 0);
  append_raw_json_list(&b, generated);
  (void)sb_append(&b, ",\"meta\":{\"engine\":\"nynth_core\",\"source\":\"creal-function-db\"}}");
  return sb_take(&b);
}

static int cmd_public_synth_creal(int argc, char **argv) {
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  bool fast = has_flag_after(argc, argv, 3, "--fast");
  const char *rounds_text = value_after(argc, argv, 3, "--rounds", "");
  if (!rounds_text || !*rounds_text) rounds_text = value_after(argc, argv, 3, "--cases", "");
  int cases = rounds_text && *rounds_text ? atoi(rounds_text) : 8;
  if (cases < 1) cases = 1;
  if (fast && cases > 8) cases = 8;
  int limit = atoi(value_after(argc, argv, 3, "--limit", "0"));
  if (limit > 0 && cases > limit) cases = limit;
  int seed = atoi(value_after(argc, argv, 3, "--seed", "1337"));
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "90"));
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *cc = creal_cc_from_args(argc, argv, 3);
  const char *max_io = value_after(argc, argv, 3, "--max-io", "5");
  char *default_db = default_creal_db_path(root);
  char *default_corpus = default_creal_corpus_dir(root);
  char *default_out = NULL;
  (void)nynth_asprintf(&default_out, "build/generated/creal_native");
  const char *env_db = getenv("CREAL_FUNCTION_DB_FILE");
  const char *function_db = value_after(argc, argv, 3, "--function-db",
                                        env_db && *env_db ? env_db : (default_db ? default_db : ""));
  const char *corpus_dir = value_after(argc, argv, 3, "--corpus-dir", default_corpus ? default_corpus : "");
  const char *out_dir = value_after(argc, argv, 3, "--out", default_out ? default_out : "");
  int before_entries = count_creal_entries_for_dir(corpus_dir);
  bool built_if_missing = before_entries < cases;
  if (built_if_missing && function_db && *function_db && path_exists_file(function_db)) {
    char *json_tmp = make_tmp_json_path(root, "creal_build", seed, 0);
    char cases_buf[32], timeout_buf[64];
    snprintf(cases_buf, sizeof(cases_buf), "%d", cases > 8 ? cases : 8);
    snprintf(timeout_buf, sizeof(timeout_buf), "%.6f", timeout_s);
    char *build_argv[28];
    int a = 0;
    build_argv[a++] = g_self_path;
    build_argv[a++] = "corpus";
    build_argv[a++] = "creal";
    build_argv[a++] = "build";
    build_argv[a++] = "--function-db"; build_argv[a++] = (char *)function_db;
    build_argv[a++] = "--corpus-dir"; build_argv[a++] = (char *)corpus_dir;
    build_argv[a++] = "--limit"; build_argv[a++] = cases_buf;
    build_argv[a++] = "--timeout-s"; build_argv[a++] = timeout_buf;
    build_argv[a++] = "--max-io"; build_argv[a++] = (char *)max_io;
    build_argv[a++] = "--cc"; build_argv[a++] = (char *)cc;
    if (json_tmp) { build_argv[a++] = "--json"; build_argv[a++] = json_tmp; }
    if (fast) build_argv[a++] = "--fast";
    build_argv[a] = NULL;
    proc_result_t pr = run_proc(build_argv, root, worker_outer_timeout(timeout_s, 1, 0));
    proc_result_free(&pr);
    free(json_tmp);
  }

  manifest_entry_list_t entries = {0};
  report_rows_t report;
  memset(&report, 0, sizeof(report));
  string_list_t generated = {0};
  if (!load_manifest_entries(corpus_dir, &entries)) {
    char *row = make_worker_failure_row("synth-creal", "creal-db-load", 1, "",
                                        "manifest read failed");
    report_add_row(&report, row);
  } else {
    int *creal_indexes = (int *)calloc((size_t)entries.count, sizeof(int));
    int creal_count = 0;
    for (int i = 0; i < entries.count; ++i) {
      if (entry_is_creal_db(entries.items[i].json)) creal_indexes[creal_count++] = i;
    }
    if (!creal_indexes || creal_count <= 0) {
      char *row = make_worker_failure_row("synth-creal", "creal-db-empty", 1, "",
                                          "no Creal function DB entries available; pass --function-db");
      report_add_row(&report, row);
    } else {
      int offset = seed >= 0 ? seed % creal_count : (-seed) % creal_count;
      for (int i = 0; i < cases; ++i) {
        int entry_idx = creal_indexes[(offset + i) % creal_count];
        const char *entry_json = entries.items[entry_idx].json;
        const char *entry_id = entries.items[entry_idx].id;
        char *case_name = json_string_or_empty(entry_json, "case");
        char stem[128], generated_name[192];
        safe_stem(stem, sizeof(stem), case_name && *case_name ? case_name : entry_id);
        snprintf(generated_name, sizeof(generated_name), "creal_%03d_%s", i, stem);
        char *src_c_ref = json_string_or_empty(entry_json, "c");
        char *src_c = resolve_existing_file(root, src_c_ref);
        if (!src_c) src_c = entry_default_path(corpus_dir, entry_id, "case.c");
        char *case_dir = NULL, *dst_c = NULL;
        bool path_ok = asprintf(&case_dir, "%s/%s", out_dir, generated_name) >= 0 &&
                       asprintf(&dst_c, "%s/%s.c", case_dir, generated_name) >= 0;
        if (!path_ok || !mkdir_p(case_dir) || !copy_file_bytes(src_c, dst_c)) {
          char *row = make_worker_failure_row(generated_name, "creal-copy", 1, "",
                                              "failed to copy Creal C source");
          report_add_row(&report, row);
        } else {
          str_buf_t gen_case = {0};
          (void)sb_append(&gen_case, "{\"name\":");
          (void)sb_append_json_str(&gen_case, generated_name);
          (void)sb_append(&gen_case, ",\"id\":");
          (void)sb_append_json_str(&gen_case, entry_id);
          (void)sb_append(&gen_case, ",\"c_source\":");
          (void)sb_append_json_str(&gen_case, dst_c);
          (void)sb_append(&gen_case, ",\"generator\":\"creal\",\"generator_kind\":\"creal\","
                          "\"method\":\"creal\",\"source_kind\":\"creal-function-db\"}");
          (void)string_list_push_take(&generated, sb_take(&gen_case));
          char *row = make_creal_db_replay_row(root, entry_id, entry_json, corpus_dir, timeout_s);
          report_add_row(&report, row);
        }
        free(case_name); free(src_c_ref); free(src_c); free(case_dir); free(dst_c);
      }
    }
    free(creal_indexes);
  }

  char *report_json = build_synth_creal_report_json(&report, &generated, corpus_dir, out_dir,
                                                    function_db, cases, seed, fast,
                                                    built_if_missing);
  if (json_path && *json_path && !write_file_text(json_path, report_json)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"path\":");
    json_str(stdout, json_path);
    printf("}\n");
  } else {
    int ok_cases = report.rows.count - report.failed_rows;
    if (ok_cases < 0) ok_cases = 0;
    printf("creal cases: %d/%d\n", ok_cases, report.rows.count);
    printf("failures: %d\n", report.failure_count);
  }
  int rc = report.failure_count ? 1 : 0;
  free(report_json);
  manifest_entry_list_free(&entries);
  report_rows_free(&report);
  string_list_free(&generated);
  free(default_db); free(default_corpus); free(default_out);
  return rc;
}

static int cmd_public_bench_compile(int argc, char **argv) {
  char root[4096], ny_bin[4096];
  if (!find_repo_root(root, sizeof(root)) || !find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  int runs = atoi(value_after(argc, argv, 3, "--runs", "3"));
  if (runs < 1) runs = 1;
  if (has_flag_after(argc, argv, 3, "--fast") && runs > 1) runs = 1;
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "180"));
  string_list_t rows = {0}, failures = {0};
  const char *cases[] = {"tiny_use_std_core", "inline_loop"};
  const char *sources[] = {"use std.core\nprint(1 + 1)\n",
                           "use std.core\nmut acc = 0\nmut i = 0\nwhile(i < 16){\n   acc += i\n   i += 1\n}\nprint(acc)\n"};
  int case_count = has_flag_after(argc, argv, 3, "--fast") ? 1 : 2;
  for (int i = 0; i < case_count; ++i) {
    double total_ms = 0.0;
    int rc = 0;
    for (int r = 0; r < runs; ++r) {
      char *cmd_argv[] = {ny_bin, "-emit-only", "-c", (char *)sources[i], NULL};
      proc_result_t pr = run_proc(cmd_argv, root, timeout_s);
      total_ms += pr.elapsed_ms;
      if (pr.rc != 0) {
        rc = pr.rc;
        (void)string_list_push_take(&failures, make_worker_failure_row(cases[i], "compile-bench", pr.rc, pr.out, pr.err));
      }
      proc_result_free(&pr);
      if (rc != 0) break;
    }
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"case\":");
    (void)sb_append_json_str(&row, cases[i]);
    (void)sb_appendf(&row, ",\"ok\":%s,\"runs\":%d,\"emit_ms\":%.2f,\"engine\":\"nynth_core\"}",
                     rc == 0 ? "true" : "false", runs, total_ms / (double)runs);
    (void)string_list_push_take(&rows, sb_take(&row));
  }
  str_buf_t extra = {0};
  (void)sb_appendf(&extra, ",\"runs\":%d,\"cache_mode\":", runs);
  (void)sb_append_json_str(&extra, value_after(argc, argv, 3, "--cache-mode", "both"));
  char *report = build_native_report_json(&rows, &failures, "bench-compile", extra.data);
  int rc = emit_native_report(report, json_path, "compile bench", rows.count, failures.count);
  free(extra.data);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static int cmd_public_bench_repl_jit(int argc, char **argv) {
  char root[4096], ny_bin[4096];
  if (!find_repo_root(root, sizeof(root)) || !find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  int runs = atoi(value_after(argc, argv, 3, "--runs", "5"));
  int warmup = atoi(value_after(argc, argv, 3, "--warmup", "1"));
  if (runs < 1) runs = 1;
  if (warmup < 0) warmup = 0;
  if (has_flag_after(argc, argv, 3, "--fast") && runs > 3) runs = 3;
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "60"));
  const char *cases[] = {"repl_expr", "repl_loop", "repl_fn"};
  const char *sources[] = {
    "use std.core\nprint(21 + 21)\n",
    "use std.core\nmut acc = 0\nmut i = 0\nwhile(i < 12){\n   acc += i\n   i += 1\n}\nprint(acc)\n",
    "use std.core\nfn mix(x){\n   x * x + 3\n}\nprint(mix(9))\n"
  };
  int case_count = has_flag_after(argc, argv, 3, "--fast") ? 2 : 3;
  string_list_t rows = {0}, failures = {0};
  for (int i = 0; i < case_count; ++i) {
    double *samples = (double *)calloc((size_t)runs, sizeof(double));
    char *baseline = NULL;
    int sample_count = 0;
    bool ok = samples != NULL;
    int rc = samples ? 0 : 1;
    proc_result_t last = {0};
    if (!samples) {
      (void)string_list_push_take(&failures, make_worker_failure_row(cases[i], "repl-jit", 1, "", "allocation failed"));
    }
    for (int r = 0; ok && r < warmup + runs; ++r) {
      char *cmd_argv[] = {ny_bin, "-repl", "--plain-repl", "-c", (char *)sources[i], NULL};
      proc_result_t pr = run_proc(cmd_argv, root, timeout_s);
      char *norm = normalize_output_pair(pr.out, pr.err);
      if (pr.rc != 0) {
        ok = false;
        rc = pr.rc;
        (void)string_list_push_take(&failures, make_native_proc_failure_row(cases[i], "repl-jit", &pr));
      } else if (!baseline) {
        baseline = strdup(norm ? norm : "");
      } else if (strcmp(baseline ? baseline : "", norm ? norm : "") != 0) {
        ok = false;
        rc = 1;
        str_buf_t msg = {0};
        (void)sb_append(&msg, "unstable repl output: expected ");
        (void)sb_append(&msg, baseline ? baseline : "");
        (void)sb_append(&msg, ", got ");
        (void)sb_append(&msg, norm ? norm : "");
        (void)string_list_push_take(&failures, make_worker_failure_row(cases[i], "repl-jit-output", 1, pr.out, msg.data));
        free(msg.data);
      }
      if (r >= warmup && sample_count < runs) samples[sample_count++] = pr.elapsed_ms;
      proc_result_free(&last);
      last = pr;
      free(norm);
    }
    double median_ms = ok ? median_double(samples, sample_count) : 0.0;
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"case\":");
    (void)sb_append_json_str(&row, cases[i]);
    (void)sb_appendf(&row,
                     ",\"ok\":%s,\"rc\":%d,\"runs\":%d,\"warmup\":%d,"
                     "\"median_ms\":%.2f,\"repl_mode\":\"plain-command\","
                     "\"jit\":true,\"engine\":\"nynth_core\"",
                     ok ? "true" : "false", rc, runs, warmup, median_ms);
    (void)sb_append(&row, ",\"normalized_output\":");
    (void)sb_append_json_str(&row, baseline ? baseline : "");
    if (!ok) append_proc_tail_fields(&row, &last);
    (void)sb_append_c(&row, '}');
    (void)string_list_push_take(&rows, sb_take(&row));
    proc_result_free(&last);
    free(samples);
    free(baseline);
  }
  str_buf_t extra = {0};
  (void)sb_appendf(&extra, ",\"runs\":%d,\"warmup\":%d,\"case_count\":%d", runs, warmup, case_count);
  char *report = build_native_report_json(&rows, &failures, "bench-repl-jit", extra.data);
  int rc = emit_native_report(report, json_path, "repl jit bench", rows.count, failures.count);
  free(extra.data);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static int cmd_public_bench_real(int argc, char **argv) {
  char root[4096], ny_bin[4096];
  if (!find_repo_root(root, sizeof(root)) || !find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after_equals(argc, argv, 3, "--json", "");
  int runs = atoi(value_after_equals(argc, argv, 3, "--runs", "1"));
  int warmup = atoi(value_after_equals(argc, argv, 3, "--warmup", "0"));
  int limit = atoi(value_after_equals(argc, argv, 3, "--limit", "0"));
  double timeout_s = atof(value_after_equals(argc, argv, 3, "--timeout-s", "120"));
  const char *ny_opt = value_after_equals(argc, argv, 3, "--ny-opt", "");
  if (runs < 1) runs = 1;
  if (warmup < 0) warmup = 0;
  const char *ny_opt_arg = NULL;
  const char *ny_profile_arg = NULL;
  const char *ny_flavor = "native";
  if (has_flag_after(argc, argv, 3, "--ny-native") ||
      strcmp(ny_opt, "native") == 0 || strcmp(ny_opt, "none") == 0) {
    ny_opt_arg = NULL;
    ny_profile_arg = NULL;
    ny_flavor = "native";
  } else if (has_flag_after(argc, argv, 3, "--ny-o3") ||
      strcmp(ny_opt, "o3") == 0 || strcmp(ny_opt, "O3") == 0 || strcmp(ny_opt, "-O3") == 0) {
    ny_opt_arg = "-O3";
    ny_profile_arg = "--profile=peak";
    ny_flavor = "peak";
  } else if (strcmp(ny_opt, "o2") == 0 || strcmp(ny_opt, "O2") == 0 || strcmp(ny_opt, "-O2") == 0) {
    ny_opt_arg = "-O2";
    ny_profile_arg = NULL;
    ny_flavor = "o2";
  } else if (strcmp(ny_opt, "o1") == 0 || strcmp(ny_opt, "O1") == 0 || strcmp(ny_opt, "-O1") == 0) {
    ny_opt_arg = "-O1";
    ny_profile_arg = NULL;
    ny_flavor = "o1";
  } else if (strcmp(ny_opt, "o0") == 0 || strcmp(ny_opt, "O0") == 0 || strcmp(ny_opt, "-O0") == 0) {
    ny_opt_arg = "-O0";
    ny_profile_arg = NULL;
    ny_flavor = "o0";
  } else if (strcmp(ny_opt, "peak") == 0 || strcmp(ny_opt, "o3-peak") == 0 ||
             strcmp(ny_opt, "O3-peak") == 0) {
    ny_opt_arg = "-O3";
    ny_profile_arg = "--profile=peak";
    ny_flavor = "peak";
  }
  const char *const *cases = PERF_REAL_CASES;
  int case_count = perf_real_case_count();
  if (limit > 0 && limit < case_count) case_count = limit;
  const char *cc = getenv("CC");
  if (!cc || !*cc) cc = "gcc";
  string_list_t rows = {0}, failures = {0};
  char *bench_dir = NULL;
  (void)asprintf(&bench_dir, "%s/build/native_perf/run_%ld_%d",
                 root, (long)time(NULL), (int)getpid());
  if (bench_dir) ny_ensure_dir_recursive(bench_dir);
  if (!bench_dir || !mkdir_p(bench_dir)) {
    (void)string_list_push_take(&failures, make_worker_failure_row("bench-real", "prepare",
                                                                  1, "", "build directory failed"));
  }
  for (int i = 0; i < case_count; ++i) {
    char *ny_path = NULL, *c_path = NULL, *shape_path = NULL, *case_dir = NULL, *c_elf = NULL, *ny_elf = NULL;
    (void)asprintf(&ny_path, "%s/build/perf/baked-sources/%s/perf_ny.ny", root, cases[i]);
    (void)asprintf(&shape_path, "%s/etc/tests/fuzz/bench/%s.nshape", root, cases[i]);
    (void)asprintf(&c_path, "%s/build/perf/baked-sources/%s/perf_c.c", root, cases[i]);
    (void)asprintf(&case_dir, "%s/%s", bench_dir ? bench_dir : "build/native_perf", cases[i]);
    (void)asprintf(&c_elf, "%s/%s_c_o3.elf", case_dir ? case_dir : "", cases[i]);
    (void)asprintf(&ny_elf, "%s/%s_ny_%s.elf", case_dir ? case_dir : "", cases[i], ny_flavor);
    bool ok = ny_path && c_path && shape_path &&
              materialize_shape_source_block(shape_path, "ny", ny_path) &&
              materialize_shape_source_block(shape_path, "c", c_path);
    proc_result_t c_compile = {0}, ny_compile = {0};
    perf_run_result_t c_run = {0}, ny_run = {0};
    if (!ok || !case_dir || !c_elf || !ny_elf || !mkdir_p(case_dir)) {
      ok = false;
      (void)string_list_push_take(&failures, make_worker_failure_row(cases[i], "bench-real", 1, "", "missing perf source or build directory"));
    } else {
      char *c_argv[] = {(char *)cc, "-O3", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-DNDEBUG", "-o", c_elf, c_path, NULL};
      c_compile = run_proc(c_argv, root, timeout_s);
      if (c_compile.rc != 0) {
        ok = false;
        (void)string_list_push_take(&failures, make_worker_failure_row(cases[i], "c-compile", c_compile.rc, c_compile.out, c_compile.err));
      }
    }
    if (ok) {
      if (ny_opt_arg && ny_profile_arg) {
        char *ny_argv[] = {ny_bin, "--compiler-asserts", (char *)ny_opt_arg,
                           (char *)ny_profile_arg, "-o", ny_elf, ny_path, NULL};
        ny_compile = run_proc(ny_argv, root, timeout_s);
      } else if (ny_opt_arg) {
        char *ny_argv[] = {ny_bin, "--compiler-asserts", (char *)ny_opt_arg,
                           "-o", ny_elf, ny_path, NULL};
        ny_compile = run_proc(ny_argv, root, timeout_s);
      } else {
        char *ny_argv[] = {ny_bin, "--compiler-asserts", "-o", ny_elf, ny_path, NULL};
        ny_compile = run_proc(ny_argv, root, timeout_s);
      }
      if (ny_compile.rc != 0) {
        ok = false;
        (void)string_list_push_take(&failures, make_worker_failure_row(cases[i], "ny-compile", ny_compile.rc, ny_compile.out, ny_compile.err));
      }
    }
    if (ok) {
      c_run = run_perf_executable(root, c_elf, runs, warmup, timeout_s);
      if (!c_run.ok) {
        ok = false;
        (void)string_list_push_take(&failures, make_worker_failure_row(cases[i], "c-run", c_run.rc, c_run.out, c_run.err));
      }
    }
    if (ok) {
      ny_run = run_perf_executable(root, ny_elf, runs, warmup, timeout_s);
      if (!ny_run.ok) {
        ok = false;
        (void)string_list_push_take(&failures, make_worker_failure_row(cases[i], "ny-run", ny_run.rc, ny_run.out, ny_run.err));
      }
    }
    if (ok && strcmp(c_run.checksum, ny_run.checksum) != 0) {
      ok = false;
      str_buf_t mismatch = {0};
      (void)sb_appendf(&mismatch, "checksum mismatch: c=%s ny=%s", c_run.checksum, ny_run.checksum);
      (void)string_list_push_take(&failures, make_worker_failure_row(cases[i], "bench-real-output", 1, "", mismatch.data ? mismatch.data : ""));
      free(mismatch.data);
    }
    double ratio = (ok && c_run.median_elapsed_ns > 0.0) ? ny_run.median_elapsed_ns / c_run.median_elapsed_ns : 0.0;
    double inst_ratio = (ok && c_run.median_instructions > 0.0) ? ny_run.median_instructions / c_run.median_instructions : 0.0;
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"case\":");
    (void)sb_append_json_str(&row, cases[i]);
    (void)sb_appendf(&row, ",\"ok\":%s,\"source_check\":true,\"runs\":%d,\"warmup\":%d,\"checksum\":",
                     ok ? "true" : "false", runs, warmup);
    (void)sb_append_json_str(&row, ok ? c_run.checksum : "");
    (void)sb_append(&row, ",\"ny_source\":");
    append_rel_json_str(&row, root, ny_path ? ny_path : "");
    (void)sb_append(&row, ",\"c_source\":");
    append_rel_json_str(&row, root, c_path ? c_path : "");
    (void)sb_append(&row, ",\"c_binary\":");
    append_rel_json_str(&row, root, c_elf ? c_elf : "");
    (void)sb_append(&row, ",\"ny_binary\":");
    append_rel_json_str(&row, root, ny_elf ? ny_elf : "");
    (void)sb_append(&row, ",\"c_flavor\":\"o3\",\"ny_flavor\":");
    (void)sb_append_json_str(&row, ny_flavor);
    if (ok) {
      (void)sb_appendf(&row,
                       ",\"c_elapsed_ns\":%.0f,\"ny_elapsed_ns\":%.0f,"
                       "\"c_process_ms\":%.2f,\"ny_process_ms\":%.2f,"
                       "\"c_instructions\":%.0f,\"ny_instructions\":%.0f,"
                       "\"c_cycles\":%.0f,\"ny_cycles\":%.0f,"
                       "\"c_branches\":%.0f,\"ny_branches\":%.0f,"
                       "\"c_branch_misses\":%.0f,\"ny_branch_misses\":%.0f,"
                       "\"ratios\":{\"ny_%s_vs_c_o3_run\":%.4f,\"ny_vs_c_elapsed_ns\":%.4f,\"ny_vs_c_instructions\":%.4f}",
                       c_run.median_elapsed_ns, ny_run.median_elapsed_ns,
                       c_run.process_median_ms, ny_run.process_median_ms,
                       c_run.median_instructions, ny_run.median_instructions,
                       c_run.median_cycles, ny_run.median_cycles,
                       c_run.median_branches, ny_run.median_branches,
                       c_run.median_branch_misses, ny_run.median_branch_misses,
                       ny_flavor, ratio, ratio, inst_ratio);
    } else {
      (void)sb_append(&row, ",\"c_elapsed_ns\":null,\"ny_elapsed_ns\":null,\"ratios\":{}");
    }
    (void)sb_append(&row, ",\"variants\":[{\"engine\":\"c\",\"flavor\":\"o3\"},{\"engine\":\"ny\",\"flavor\":");
    (void)sb_append_json_str(&row, ny_flavor);
    (void)sb_append(&row, "}],\"engine\":\"nynth_core\"}");
    (void)string_list_push_take(&rows, sb_take(&row));
    proc_result_free(&c_compile); proc_result_free(&ny_compile);
    perf_run_result_free(&c_run); perf_run_result_free(&ny_run);
    free(ny_path); free(c_path); free(shape_path); free(case_dir); free(c_elf); free(ny_elf);
  }
  str_buf_t extra = {0};
  (void)sb_appendf(&extra, ",\"runs\":%d,\"warmup\":%d,\"case_count\":%d,\"ny_flavor\":", runs, warmup, case_count);
  (void)sb_append_json_str(&extra, ny_flavor);
  (void)sb_append(&extra, ",\"build_dir\":");
  append_rel_json_str(&extra, root, bench_dir ? bench_dir : "");
  char *report = build_native_report_json(&rows, &failures, "bench-real", extra.data);
  int rc = emit_native_report(report, json_path, "real bench", rows.count, failures.count);
  free(extra.data);
  free(bench_dir);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static bool perf_triage_stale_artifact_name(const char *name) {
  if (!name || !*name) return false;
  if (strncmp(name, "rank_", 5) == 0 && ny_has_suffix(name, ".json")) return true;
  return strstr(name, "_ny_over_c_") != NULL && ny_has_suffix(name, ".json");
}

static int clean_perf_triage_stale_artifacts(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  int removed = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    if (!perf_triage_stale_artifact_name(ent->d_name)) continue;
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    if (n <= 0 || (size_t)n >= sizeof(path)) continue;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
    if (unlink(path) == 0) ++removed;
  }
  closedir(d);
  return removed;
}

static bool write_perf_triage_markdown(const char *root,
                                       const char *markdown_path,
                                       const char *json_path,
                                       const string_list_t *rows,
                                       int candidates,
                                       int emitted,
                                       int hotspots,
                                       double max_ratio,
                                       const char *max_case,
                                       double threshold_ratio,
                                       int runs,
                                       int warmup,
                                       int confirmed_candidates,
                                       int confirmed_hotspots,
                                       int demoted_hotspots,
                                       bool confirm_enabled,
                                       bool confirm_attempted,
                                       int confirm_runs,
                                       int confirm_warmup,
                                       bool fast_mode,
                                       const char *confirmation_report,
                                       const char *scratch_root,
                                       const char *findings_dir) {
  if (!markdown_path || !*markdown_path) return true;
  char *json_rel = rel_path_dup(root ? root : "", json_path ? json_path : "");
  char *md_rel = rel_path_dup(root ? root : "", markdown_path);
  char *confirm_rel =
      rel_path_dup(root ? root : "", confirmation_report ? confirmation_report : "");
  char *scratch_rel =
      rel_path_dup(root ? root : "", scratch_root ? scratch_root : "");
  char *findings_rel =
      rel_path_dup(root ? root : "", findings_dir ? findings_dir : "");
  time_t now = time(NULL);
  struct tm tm_now;
  char stamp[64] = {0};
  if (localtime_r(&now, &tm_now))
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %z", &tm_now);

  str_buf_t md = {0};
  (void)sb_append(&md, "# Nynth Perf Triage\n\n");
  if (stamp[0]) {
    (void)sb_append(&md, "Generated: ");
    md_append_code(&md, stamp);
    (void)sb_append(&md, "\n\n");
  }
  (void)sb_append(&md, "## TLDR\n\n");
  (void)sb_appendf(&md,
                   "- Cases: %d emitted from %d candidates; threshold %.2fx; "
                   "hotspots %d.\n",
                   emitted, candidates, threshold_ratio, hotspots);
  (void)sb_append(&md, "- Worst: ");
  md_append_code(&md, max_case && *max_case ? max_case : "none");
  (void)sb_appendf(&md, " at %.4fx Ny/C.\n", max_ratio);
  (void)sb_appendf(&md, "- Samples: %d runs, %d warmup", runs, warmup);
  if (confirm_enabled) {
    (void)sb_appendf(&md,
                     "; confirmation %s, %d candidates, %d confirmed, %d demoted, "
                     "%d runs, %d warmup",
                     confirm_attempted ? "attempted" : "not-needed",
                     confirmed_candidates, confirmed_hotspots, demoted_hotspots,
                     confirm_runs, confirm_warmup);
  } else {
    (void)sb_append(&md, "; confirmation disabled");
  }
  (void)sb_append(&md, ".\n");
  if (json_rel && *json_rel) {
    (void)sb_append(&md, "- JSON: ");
    md_append_code(&md, json_rel);
    (void)sb_append(&md, ".\n");
  }
  if (findings_rel && *findings_rel) {
    (void)sb_append(&md, "- Findings dir: ");
    md_append_code(&md, findings_rel);
    (void)sb_append(&md, ".\n");
  }
  if (scratch_rel && *scratch_rel) {
    (void)sb_append(&md, "- Scratch: ");
    md_append_code(&md, scratch_rel);
    (void)sb_append(&md, ".\n");
  }
  if (confirm_rel && *confirm_rel) {
    (void)sb_append(&md, "- Confirmation report: ");
    md_append_code(&md, confirm_rel);
    (void)sb_append(&md, ".\n");
  }

  (void)sb_append(&md, "\n## Ranked Cases\n\n");
  int printed = 0;
  for (int i = 0; rows && i < rows->count; ++i) {
    const char *row = rows->items[i];
    char *case_name = json_string_or_empty(row, "case");
    char *artifact = json_string_or_empty(row, "artifact");
    char *ny_source = json_string_or_empty(row, "ny_source");
    char *c_source = json_string_or_empty(row, "c_source");
    double rank = 0.0, ratio = 0.0, row_runs = 0.0, row_warmup = 0.0;
    double c_elapsed_ns = 0.0, ny_elapsed_ns = 0.0;
    double c_instructions = 0.0, ny_instructions = 0.0;
    double slowdown_percent = 0.0;
    (void)extract_json_number(row, "rank", &rank);
    (void)extract_json_number(row, "ratio", &ratio);
    (void)extract_json_number(row, "runs", &row_runs);
    (void)extract_json_number(row, "warmup", &row_warmup);
    (void)extract_json_number(row, "c_elapsed_ns", &c_elapsed_ns);
    (void)extract_json_number(row, "ny_elapsed_ns", &ny_elapsed_ns);
    (void)extract_json_number(row, "c_instructions", &c_instructions);
    (void)extract_json_number(row, "ny_instructions", &ny_instructions);
    if (!extract_json_number(row, "slowdown_percent", &slowdown_percent))
      slowdown_percent = ratio > 0.0 ? (ratio - 1.0) * 100.0 : 0.0;
    bool hot = strstr(row, "\"hot\":true") != NULL;
    bool confirmed = strstr(row, "\"confirmed\":true") != NULL;
    bool demoted = strstr(row, "\"demoted_hotspot\":true") != NULL;
    char *artifact_rel = rel_path_dup(root ? root : "", artifact ? artifact : "");
    char *ny_source_rel = rel_path_dup(root ? root : "", ny_source ? ny_source : "");
    char *c_source_rel = rel_path_dup(root ? root : "", c_source ? c_source : "");
    (void)sb_appendf(&md, "- #%.0f ", rank);
    md_append_code(&md, case_name && *case_name ? case_name : "unknown");
    (void)sb_appendf(&md,
                     ": %s; %.4fx Ny/C; %+.2f%%; %.0f runs, %.0f warmup; "
                     "C %.3f ms; Ny %.3f ms",
                     hot ? "watch" : "ok", ratio, slowdown_percent,
                     row_runs, row_warmup,
                     c_elapsed_ns / 1000000.0, ny_elapsed_ns / 1000000.0);
    if (ny_instructions > 0.0 && c_instructions > 0.0) {
      (void)sb_appendf(&md, "; inst %.2fx", ny_instructions / c_instructions);
    }
    if (confirmed) (void)sb_append(&md, "; confirmed");
    if (demoted) (void)sb_append(&md, "; demoted");
    md_append_labeled_path(&md, "artifact", artifact_rel);
    md_append_labeled_path(&md, "ny", ny_source_rel);
    md_append_labeled_path(&md, "c", c_source_rel);
    (void)sb_append(&md, "\n");
    ++printed;
    free(artifact_rel);
    free(ny_source_rel);
    free(c_source_rel);
    free(case_name);
    free(artifact);
    free(ny_source);
    free(c_source);
  }
  if (!printed)
    (void)sb_append(&md, "No measured cases were emitted.\n");
  (void)sb_append(&md,
                  "\n## Refresh\n\n```bash\n"
                  "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 "
                  "./build/nynth perf triage");
  if (fast_mode) (void)sb_append(&md, " --fast");
  if (emitted > 0) (void)sb_appendf(&md, " --limit %d", emitted);
  (void)sb_appendf(&md, " --threshold %.2f", threshold_ratio);
  if (json_rel && *json_rel) {
    (void)sb_append(&md, " --json ");
    (void)sb_append(&md, json_rel);
  }
  if (md_rel && *md_rel) {
    (void)sb_append(&md, " --markdown ");
    (void)sb_append(&md, md_rel);
  }
  (void)sb_append(&md, "\n```\n");

  bool ok = md.data && write_file_text(markdown_path, md.data);
  free(json_rel);
  free(md_rel);
  free(confirm_rel);
  free(scratch_rel);
  free(findings_rel);
  free(md.data);
  return ok;
}

static int cmd_public_perf_triage(int argc, char **argv) {
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  char artifact_root[4096];
  if (!find_nynth_root(artifact_root, sizeof(artifact_root)))
    snprintf(artifact_root, sizeof(artifact_root), "%s", root);
  const char *json_path = value_after_equals(argc, argv, 3, "--json", "");
  const char *markdown_path = value_after_equals(argc, argv, 3, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after_equals(argc, argv, 3, "--md", "");
  const char *scratch_root_arg = nynth_scratch_root_arg(argc, argv, 3);
  char *scratch_root = nynth_absolute_scratch_root(scratch_root_arg);
  if (scratch_root && *scratch_root) ny_ensure_dir_recursive(scratch_root);
  int limit = atoi(value_after_equals(argc, argv, 3, "--limit", "5"));
  if (limit < 1) limit = 5;
  char *tmp_json = make_tmp_json_path(root, "perf_triage_bench", 0, 0);
  int runs_i = atoi(value_after_equals(argc, argv, 3, "--runs", "11"));
  int warmup_i = atoi(value_after_equals(argc, argv, 3, "--warmup", "3"));
  if (runs_i < 1) runs_i = 1;
  if (warmup_i < 0) warmup_i = 0;
  const char *threshold_text = value_after_equals(argc, argv, 3, "--threshold", "");
  if (!threshold_text || !*threshold_text)
    threshold_text = value_after_equals(argc, argv, 3, "--min-ratio", "");
  double threshold_ratio = threshold_text && *threshold_text ? atof(threshold_text) : 1.5;
  if (threshold_ratio < 0.0) threshold_ratio = 0.0;
  bool confirm_enabled = !has_flag_after(argc, argv, 3, "--no-confirm-hotspots");
  const char *confirm_runs_text = value_after_equals(argc, argv, 3, "--confirm-runs", "");
  int confirm_runs_i = confirm_runs_text && *confirm_runs_text ? atoi(confirm_runs_text) : 0;
  if (confirm_runs_i <= 0) confirm_runs_i = runs_i < 31 ? 31 : runs_i;
  if (confirm_runs_i < runs_i) confirm_runs_i = runs_i;
  const char *confirm_warmup_text = value_after_equals(argc, argv, 3, "--confirm-warmup", "");
  int confirm_warmup_i = confirm_warmup_text && *confirm_warmup_text
                             ? atoi(confirm_warmup_text)
                             : warmup_i;
  if ((!confirm_warmup_text || !*confirm_warmup_text) &&
      confirm_runs_i > runs_i && confirm_warmup_i < 8)
    confirm_warmup_i = 8;
  if (confirm_warmup_i < 0) confirm_warmup_i = 0;
  char runs[32], warmup[32];
  snprintf(runs, sizeof(runs), "%d", runs_i);
  snprintf(warmup, sizeof(warmup), "%d", warmup_i);
  const char *timeout_text = value_after_equals(argc, argv, 3, "--timeout-s", "120");
  const char *bench_limit_text = value_after_equals(argc, argv, 3, "--bench-limit", "");
  if (!bench_limit_text || !*bench_limit_text)
    bench_limit_text = value_after_equals(argc, argv, 3, "--candidate-limit", "");
  int bench_limit = bench_limit_text && *bench_limit_text ? atoi(bench_limit_text) : limit;
  if (bench_limit < 1) bench_limit = limit;
  char bench_limit_buf[32];
  snprintf(bench_limit_buf, sizeof(bench_limit_buf), "%d", bench_limit);
  double timeout_s = atof(timeout_text);
  if (timeout_s <= 0.0) timeout_s = 120.0;
  const char *bench_timeout_text =
      value_after_equals(argc, argv, 3, "--bench-timeout-s", "");
  if (!bench_timeout_text || !*bench_timeout_text)
    bench_timeout_text = value_after_equals(argc, argv, 3, "--wall-timeout-s", "");
  double bench_timeout_s = bench_timeout_text && *bench_timeout_text
                               ? atof(bench_timeout_text)
                               : 0.0;
  if (bench_timeout_s <= 0.0)
    bench_timeout_s = timeout_s * (double)bench_limit + 60.0;
  if (bench_timeout_s < timeout_s)
    bench_timeout_s = timeout_s;
  char *sub_argv[18];
  int a = 0;
  sub_argv[a++] = g_self_path;
  sub_argv[a++] = "bench";
  sub_argv[a++] = "real";
  if (has_flag_after(argc, argv, 3, "--fast")) sub_argv[a++] = "--fast";
  sub_argv[a++] = "--runs"; sub_argv[a++] = runs;
  sub_argv[a++] = "--warmup"; sub_argv[a++] = warmup;
  sub_argv[a++] = "--timeout-s"; sub_argv[a++] = (char *)timeout_text;
  sub_argv[a++] = "--limit"; sub_argv[a++] = bench_limit_buf;
  sub_argv[a++] = "--json"; sub_argv[a++] = tmp_json;
  sub_argv[a] = NULL;
  proc_result_t pr = run_proc(sub_argv, root, bench_timeout_s);
  file_buf_t f = {0};
  string_list_t rows = {0}, failures = {0};
  triage_item_list_t items = {0};
  bool have_report = tmp_json && read_file(tmp_json, &f) && f.data;
  if (have_report) {
    string_list_t bench_rows = {0};
    (void)collect_rows_from_report_json(f.data, &bench_rows);
    for (int i = 0; i < bench_rows.count; ++i) {
      triage_item_t item;
      if (triage_item_from_bench_row(bench_rows.items[i], runs_i, warmup_i, &item) &&
          !triage_item_list_push_take(&items, item))
        triage_item_free(&item);
    }
    string_list_free(&bench_rows);
  }
  if (pr.rc != 0 || !have_report) {
    (void)string_list_push_take(&failures, make_native_proc_failure_row("perf-triage", "bench-real", &pr));
  }
  qsort(items.items, (size_t)items.count, sizeof(items.items[0]), triage_cmp_desc);
  int emitted = items.count < limit ? items.count : limit;
  int initial_hotspots = 0;
  int initial_timing_hotspots = 0;
  double initial_max_ratio = 0.0;
  char initial_max_case[128] = {0};
  bool need_confirmation = false;
  for (int i = 0; i < emitted; ++i) {
    triage_item_t *it = &items.items[i];
    it->initial_ratio = it->ratio;
    it->initially_hot = !it->ok || it->ratio > threshold_ratio;
    if (it->initially_hot) ++initial_hotspots;
    if (it->ratio > initial_max_ratio) {
      initial_max_ratio = it->ratio;
      snprintf(initial_max_case, sizeof(initial_max_case), "%s",
               it->case_name ? it->case_name : "");
    }
    int item_runs = it->runs > 0 ? it->runs : runs_i;
    if (confirm_enabled && it->ok && it->ratio > threshold_ratio &&
        confirm_runs_i > item_runs) {
      need_confirmation = true;
      ++initial_timing_hotspots;
    }
  }
  char *tmp_confirm_json = NULL;
  file_buf_t confirm_file = {0};
  proc_result_t confirm_pr = {0};
  triage_item_list_t confirm_items = {0};
  bool confirm_attempted = false;
  bool confirm_have_report = false;
  double confirm_bench_timeout_s = 0.0;
  int confirmed_candidates = 0;
  int demoted_hotspots = 0;
  if (need_confirmation) {
    confirm_attempted = true;
    tmp_confirm_json = make_tmp_json_path(root, "perf_triage_confirm", 0, 0);
    char confirm_runs[32], confirm_warmup[32];
    snprintf(confirm_runs, sizeof(confirm_runs), "%d", confirm_runs_i);
    snprintf(confirm_warmup, sizeof(confirm_warmup), "%d", confirm_warmup_i);
    confirm_bench_timeout_s = bench_timeout_s;
    if (runs_i > 0 && confirm_runs_i > runs_i) {
      confirm_bench_timeout_s =
          bench_timeout_s * ((double)confirm_runs_i / (double)runs_i) + 60.0;
    }
    double min_confirm_timeout = timeout_s * (double)bench_limit + 60.0;
    if (confirm_bench_timeout_s < min_confirm_timeout)
      confirm_bench_timeout_s = min_confirm_timeout;
    char *confirm_argv[18];
    int ca = 0;
    confirm_argv[ca++] = g_self_path;
    confirm_argv[ca++] = "bench";
    confirm_argv[ca++] = "real";
    if (has_flag_after(argc, argv, 3, "--fast")) confirm_argv[ca++] = "--fast";
    confirm_argv[ca++] = "--runs"; confirm_argv[ca++] = confirm_runs;
    confirm_argv[ca++] = "--warmup"; confirm_argv[ca++] = confirm_warmup;
    confirm_argv[ca++] = "--timeout-s"; confirm_argv[ca++] = (char *)timeout_text;
    confirm_argv[ca++] = "--limit"; confirm_argv[ca++] = bench_limit_buf;
    confirm_argv[ca++] = "--json"; confirm_argv[ca++] = tmp_confirm_json;
    confirm_argv[ca] = NULL;
    confirm_pr = run_proc(confirm_argv, root, confirm_bench_timeout_s);
    confirm_have_report = tmp_confirm_json && read_file(tmp_confirm_json, &confirm_file) &&
                          confirm_file.data;
    if (confirm_have_report) {
      string_list_t confirm_rows = {0};
      (void)collect_rows_from_report_json(confirm_file.data, &confirm_rows);
      for (int i = 0; i < confirm_rows.count; ++i) {
        triage_item_t item;
        if (triage_item_from_bench_row(confirm_rows.items[i], confirm_runs_i,
                                       confirm_warmup_i, &item) &&
            !triage_item_list_push_take(&confirm_items, item))
          triage_item_free(&item);
      }
      string_list_free(&confirm_rows);
    }
    if (confirm_pr.rc != 0 || !confirm_have_report) {
      (void)string_list_push_take(&failures,
                                  make_native_proc_failure_row("perf-triage",
                                                               "bench-real-confirm",
                                                               &confirm_pr));
    } else {
      for (int i = 0; i < emitted; ++i) {
        triage_item_t *it = &items.items[i];
        if (!it->ok || it->initial_ratio <= threshold_ratio) continue;
        triage_item_t *confirmed =
            triage_item_list_find_case(&confirm_items, it->case_name ? it->case_name : "");
        if (!confirmed) continue;
        it->confirmed = true;
        it->confirmation_ok = confirmed->ok;
        it->confirmation_runs = confirmed->runs;
        it->confirmation_warmup = confirmed->warmup;
        it->confirmation_ratio = confirmed->ratio;
        ++confirmed_candidates;
        if (confirmed->ok) {
          char *confirmed_row = strdup(confirmed->row ? confirmed->row : "{}");
          if (confirmed_row) {
            free(it->row);
            it->row = confirmed_row;
          }
          it->ratio = confirmed->ratio;
          it->c_elapsed_ns = confirmed->c_elapsed_ns;
          it->ny_elapsed_ns = confirmed->ny_elapsed_ns;
          it->runs = confirmed->runs;
          it->warmup = confirmed->warmup;
          if (it->ratio <= threshold_ratio) {
            it->demoted_hotspot = true;
            ++demoted_hotspots;
          }
        }
      }
    }
  }
  const char *findings_dir_arg = value_after_equals(argc, argv, 3, "--findings-dir", "");
  char *findings_dir = NULL;
  if (findings_dir_arg && *findings_dir_arg) {
    if (path_is_absolute(findings_dir_arg)) findings_dir = strdup(findings_dir_arg);
    else (void)asprintf(&findings_dir, "%s/%s", root, findings_dir_arg);
  } else {
    const char *base = scratch_root && *scratch_root ? scratch_root : NYNTH_DEFAULT_SCRATCH_ROOT;
    (void)asprintf(&findings_dir, "%s/perf_triage_findings/run_%ld_%d",
                   base, (long)time(NULL), (int)getpid());
  }
  int cleaned_stale = 0;
  if (findings_dir) {
    ny_ensure_dir_recursive(findings_dir);
    if (findings_dir_arg && *findings_dir_arg)
      cleaned_stale = clean_perf_triage_stale_artifacts(findings_dir);
  }
  int hotspots = 0;
  int confirmed_hotspots = 0;
  double max_ratio = 0.0;
  char max_case[128] = {0};
  for (int i = 0; i < emitted; ++i) {
    triage_item_t *it = &items.items[i];
    bool hot = !it->ok || it->ratio > threshold_ratio;
    if (hot) ++hotspots;
    if (hot && it->confirmed) ++confirmed_hotspots;
    if (it->ratio > max_ratio) {
      max_ratio = it->ratio;
      snprintf(max_case, sizeof(max_case), "%s", it->case_name ? it->case_name : "");
    }
    char stem[256];
    safe_stem(stem, sizeof(stem), it->case_name && *it->case_name ? it->case_name : "perf_case");
    char *artifact = NULL;
    (void)asprintf(&artifact, "%s/rank_%03d_%s.json", findings_dir ? findings_dir : "/tmp", i + 1, stem);
    str_buf_t artifact_json = {0};
    int item_runs = it->runs > 0 ? it->runs : runs_i;
    int item_warmup = it->warmup >= 0 ? it->warmup : warmup_i;
    double c_elapsed_ms = it->c_elapsed_ns / 1000000.0;
    double ny_elapsed_ms = it->ny_elapsed_ns / 1000000.0;
    double delta_elapsed_ns = it->ny_elapsed_ns - it->c_elapsed_ns;
    double delta_elapsed_ms = delta_elapsed_ns / 1000000.0;
    double slowdown_percent =
        it->ratio > 0.0 ? (it->ratio - 1.0) * 100.0 : 0.0;
    char confirmation_ratio_buf[64];
    const char *confirmation_ratio_json = "null";
    if (it->confirmed) {
      snprintf(confirmation_ratio_buf, sizeof(confirmation_ratio_buf), "%.4f",
               it->confirmation_ratio);
      confirmation_ratio_json = confirmation_ratio_buf;
    }
    (void)sb_appendf(&artifact_json, "{\"rank\":%d,\"case\":", i + 1);
    (void)sb_append_json_str(&artifact_json, it->case_name ? it->case_name : "");
    (void)sb_appendf(&artifact_json,
                     ",\"ok\":%s,\"hot\":%s,\"ratio\":%.4f,"
                     "\"threshold_ratio\":%.4f,\"runs\":%d,\"warmup\":%d,"
                     "\"samples\":%d,\"c_elapsed_ns\":%.0f,\"ny_elapsed_ns\":%.0f,"
                     "\"c_instructions\":%.0f,\"ny_instructions\":%.0f,"
                     "\"c_elapsed_ms\":%.6f,\"ny_elapsed_ms\":%.6f,"
                     "\"delta_elapsed_ns\":%.0f,\"delta_elapsed_ms\":%.6f,"
                     "\"slowdown_percent\":%.2f,\"hotspot\":%s,"
                     "\"initial_hot\":%s,\"initial_ratio\":%.4f,"
                     "\"confirmed\":%s,\"confirmation_ok\":%s,"
                     "\"confirmation_runs\":%d,\"confirmation_warmup\":%d,"
                     "\"confirmation_ratio\":%s,\"demoted_hotspot\":%s,"
                     "\"engine\":\"nynth_core\",\"row\":",
                     it->ok ? "true" : "false", hot ? "true" : "false",
                     it->ratio, threshold_ratio, item_runs, item_warmup,
                     item_runs, it->c_elapsed_ns, it->ny_elapsed_ns,
                     it->c_instructions, it->ny_instructions,
                     c_elapsed_ms, ny_elapsed_ms, delta_elapsed_ns,
                     delta_elapsed_ms, slowdown_percent,
                     hot ? "true" : "false",
                     it->initially_hot ? "true" : "false", it->initial_ratio,
                     it->confirmed ? "true" : "false",
                     it->confirmation_ok ? "true" : "false",
                     it->confirmation_runs, it->confirmation_warmup,
                     confirmation_ratio_json,
                     it->demoted_hotspot ? "true" : "false");
    (void)sb_append(&artifact_json, it->row ? it->row : "{}");
    (void)sb_append_c(&artifact_json, '}');
    if (artifact) (void)write_file_text(artifact, artifact_json.data ? artifact_json.data : "{}");
    str_buf_t row = {0};
    (void)sb_appendf(&row, "{\"case\":");
    (void)sb_append_json_str(&row, it->case_name ? it->case_name : "");
    (void)sb_appendf(&row,
                     ",\"rank\":%d,\"ok\":%s,\"ratio\":%.4f,\"runs\":%d,"
                     "\"warmup\":%d,\"samples\":%d,\"c_elapsed_ns\":%.0f,"
                     "\"ny_elapsed_ns\":%.0f,\"c_instructions\":%.0f,"
                     "\"ny_instructions\":%.0f,\"threshold_ratio\":%.4f,"
                     "\"c_elapsed_ms\":%.6f,\"ny_elapsed_ms\":%.6f,"
                     "\"delta_elapsed_ns\":%.0f,\"delta_elapsed_ms\":%.6f,"
                     "\"slowdown_percent\":%.2f,\"hot\":%s,\"hotspot\":%s,"
                     "\"engine\":\"nynth_core\",\"artifact\":",
                     i + 1, it->ok ? "true" : "false", it->ratio,
                     item_runs, item_warmup, item_runs,
                     it->c_elapsed_ns, it->ny_elapsed_ns,
                     it->c_instructions, it->ny_instructions,
                     threshold_ratio, c_elapsed_ms, ny_elapsed_ms,
                     delta_elapsed_ns, delta_elapsed_ms, slowdown_percent,
                     hot ? "true" : "false", hot ? "true" : "false");
    append_rel_json_str(&row, artifact_root, artifact ? artifact : "");
    (void)sb_append(&row, ",\"ny_source\":");
    append_rel_json_str(&row, artifact_root, it->ny_source ? it->ny_source : "");
    (void)sb_append(&row, ",\"c_source\":");
    append_rel_json_str(&row, artifact_root, it->c_source ? it->c_source : "");
    (void)sb_appendf(&row,
                     ",\"initial_hot\":%s,\"initial_ratio\":%.4f,"
                     "\"confirmed\":%s,\"confirmation_ok\":%s,"
                     "\"confirmation_runs\":%d,\"confirmation_warmup\":%d,"
                     "\"confirmation_ratio\":%s,\"demoted_hotspot\":%s",
                     it->initially_hot ? "true" : "false", it->initial_ratio,
                     it->confirmed ? "true" : "false",
                     it->confirmation_ok ? "true" : "false",
                     it->confirmation_runs, it->confirmation_warmup,
                     confirmation_ratio_json,
                     it->demoted_hotspot ? "true" : "false");
    (void)sb_append_c(&row, '}');
    (void)string_list_push_take(&rows, sb_take(&row));
    free(artifact_json.data);
    free(artifact);
  }
    if (findings_dir) {
      char *summary_path = NULL;
      (void)asprintf(&summary_path, "%s/summary.json", findings_dir);
    str_buf_t summary = {0};
    (void)sb_appendf(&summary,
                     "{\"engine\":\"nynth_core\",\"candidates\":%d,\"emitted\":%d,"
                     "\"hotspots\":%d,\"perf_hotspots\":%d,"
                     "\"max_ratio\":%.4f,\"perf_max_ratio\":%.4f,"
                     "\"perf_worst_ratio\":%.4f,"
                     "\"perf_worst_slowdown_percent\":%.2f,"
                     "\"max_case\":",
                     items.count, emitted, hotspots, hotspots,
                     max_ratio, max_ratio, max_ratio,
                     max_ratio > 0.0 ? (max_ratio - 1.0) * 100.0 : 0.0);
    (void)sb_append_json_str(&summary, max_case);
    (void)sb_append(&summary, ",\"perf_max_case\":");
    (void)sb_append_json_str(&summary, max_case);
    (void)sb_append(&summary, ",\"perf_worst_case\":");
    (void)sb_append_json_str(&summary, max_case);
    (void)sb_appendf(&summary,
                         ",\"initial_hotspots\":%d,"
                         "\"initial_timing_hotspots\":%d,"
                         "\"initial_max_ratio\":%.4f,"
                         "\"perf_initial_max_ratio\":%.4f,"
                         "\"initial_max_case\":",
                         initial_hotspots, initial_timing_hotspots,
                         initial_max_ratio, initial_max_ratio);
    (void)sb_append_json_str(&summary, initial_max_case);
    (void)sb_append(&summary, ",\"perf_initial_max_case\":");
    (void)sb_append_json_str(&summary, initial_max_case);
    (void)sb_appendf(&summary,
                         ",\"confirmed_candidates\":%d,"
                         "\"confirmed_hotspots\":%d,\"demoted_hotspots\":%d,"
                         "\"confirm_enabled\":%s,\"confirm_attempted\":%s,"
                         "\"confirm_runs\":%d,\"confirm_warmup\":%d,"
                         "\"confirm_bench_timeout_s\":%.3f,\"confirm_rc\":%d,"
                         "\"confirmation_report\":",
                         confirmed_candidates, confirmed_hotspots,
                         demoted_hotspots,
                         confirm_enabled ? "true" : "false",
                         confirm_attempted ? "true" : "false",
                         confirm_runs_i, confirm_warmup_i,
                         confirm_bench_timeout_s,
                         confirm_attempted ? confirm_pr.rc : 0);
    append_rel_json_str(&summary, artifact_root,
                        tmp_confirm_json ? tmp_confirm_json : "");
    (void)sb_appendf(&summary,
                     ",\"threshold_ratio\":%.4f,\"bench_limit\":%d,"
                         "\"bench_timeout_s\":%.3f,"
                     "\"runs\":%d,\"warmup\":%d,\"measurement_samples\":%d,"
                     "\"cleaned_stale\":%d,\"source_report\":",
                     threshold_ratio, bench_limit, bench_timeout_s, runs_i,
                         warmup_i, runs_i, cleaned_stale);
    append_rel_json_str(&summary, artifact_root, tmp_json ? tmp_json : "");
    (void)sb_append_c(&summary, '}');
    if (summary_path) (void)write_file_text(summary_path, summary.data ? summary.data : "{}");
      free(summary.data);
      free(summary_path);
    }
  if (markdown_path && *markdown_path &&
      !write_perf_triage_markdown(artifact_root, markdown_path, json_path, &rows,
                                  items.count, emitted, hotspots, max_ratio,
                                  max_case, threshold_ratio, runs_i, warmup_i,
                                  confirmed_candidates, confirmed_hotspots,
                                  demoted_hotspots, confirm_enabled,
                                  confirm_attempted, confirm_runs_i,
                                  confirm_warmup_i,
                                  has_flag_after(argc, argv, 3, "--fast"),
                                  tmp_confirm_json ? tmp_confirm_json : "",
                                  scratch_root ? scratch_root : "",
                                  findings_dir ? findings_dir : "")) {
    (void)string_list_push_take(
        &failures,
        make_worker_failure_row("perf-triage", "markdown", 1, "",
                                "perf triage markdown write failed"));
  }
    str_buf_t extra = {0};
    (void)sb_appendf(&extra,
                   ",\"candidates\":%d,\"emitted\":%d,\"runs\":%d,\"warmup\":%d,"
                   "\"measurement_samples\":%d,\"hotspots\":%d,"
                   "\"perf_hotspots\":%d,"
                   "\"max_ratio\":%.4f,\"perf_max_ratio\":%.4f,"
                   "\"perf_worst_ratio\":%.4f,"
                   "\"perf_worst_slowdown_percent\":%.2f,"
                   "\"threshold_ratio\":%.4f,\"perf_threshold_ratio\":%.4f,"
                   "\"bench_limit\":%d,\"bench_timeout_s\":%.3f,"
                       "\"initial_hotspots\":%d,\"initial_timing_hotspots\":%d,"
                       "\"initial_max_ratio\":%.4f,"
                       "\"perf_initial_max_ratio\":%.4f,"
                       "\"confirmed_candidates\":%d,\"confirmed_hotspots\":%d,"
                       "\"perf_confirmed_hotspots\":%d,"
                       "\"demoted_hotspots\":%d,\"confirm_enabled\":%s,"
                       "\"confirm_attempted\":%s,\"confirm_runs\":%d,"
                       "\"confirm_warmup\":%d,\"confirm_bench_timeout_s\":%.3f,"
                       "\"confirm_rc\":%d,"
                       "\"bench_rc\":%d,\"cleaned_stale\":%d,"
                   "\"max_case\":",
                   items.count, emitted, runs_i, warmup_i, runs_i,
                   hotspots, hotspots,
                   max_ratio, max_ratio, max_ratio,
                   max_ratio > 0.0 ? (max_ratio - 1.0) * 100.0 : 0.0,
                   threshold_ratio, threshold_ratio, bench_limit,
                       bench_timeout_s, initial_hotspots, initial_timing_hotspots,
                       initial_max_ratio, initial_max_ratio,
                       confirmed_candidates, confirmed_hotspots,
                       confirmed_hotspots,
                       demoted_hotspots, confirm_enabled ? "true" : "false",
                       confirm_attempted ? "true" : "false", confirm_runs_i,
                       confirm_warmup_i, confirm_bench_timeout_s,
                       confirm_attempted ? confirm_pr.rc : 0, pr.rc, cleaned_stale);
  (void)sb_append_json_str(&extra, max_case);
  (void)sb_append(&extra, ",\"perf_max_case\":");
  (void)sb_append_json_str(&extra, max_case);
  (void)sb_append(&extra, ",\"perf_worst_case\":");
  (void)sb_append_json_str(&extra, max_case);
  (void)sb_append(&extra, ",\"initial_max_case\":");
  (void)sb_append_json_str(&extra, initial_max_case);
  (void)sb_append(&extra, ",\"perf_initial_max_case\":");
  (void)sb_append_json_str(&extra, initial_max_case);
  (void)sb_append(&extra, ",\"confirmation_report\":");
  append_rel_json_str(&extra, artifact_root, tmp_confirm_json ? tmp_confirm_json : "");
  (void)sb_append(&extra, ",\"scratch_root\":");
  append_rel_json_str(&extra, artifact_root, scratch_root ? scratch_root : "");
    (void)sb_append(&extra, ",\"findings_dir\":");
    append_rel_json_str(&extra, artifact_root, findings_dir ? findings_dir : "");
  if (markdown_path && *markdown_path) {
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, artifact_root, markdown_path);
  }
    char *report = build_native_report_json_with_top_aliases(
        &rows, &failures, "perf-triage", extra.data, true);
  int rc = emit_native_report(report, json_path, "perf", rows.count, failures.count);
  free(extra.data); free(findings_dir); free(scratch_root);
  free(f.data); free(confirm_file.data); free(tmp_json); free(tmp_confirm_json);
  proc_result_free(&pr); proc_result_free(&confirm_pr);
  triage_item_list_free(&items); triage_item_list_free(&confirm_items);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static int cmd_public_corpus_real_db(int argc, char **argv, const char *kind) {
  char root[4096], ny_bin[4096];
  if (!find_repo_root(root, sizeof(root)) || !find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  string_list_t rows = {0}, failures = {0}, files = {0};
  const char *scan_dir = "etc/tests/rt";
  if (strcmp(kind, "functions") == 0) scan_dir = "lib";
  char *dir = NULL, *default_corpus = NULL;
  if (strcmp(kind, "mined-hosts") == 0) {
    (void)nynth_asprintf(&dir, "etc/tests/rt");
  } else {
    (void)asprintf(&dir, "%s/%s", root, scan_dir);
  }
  (void)nynth_asprintf(&default_corpus, "corpus/real-db");
  const char *corpus_dir = value_after(argc, argv, 3, "--corpus-dir", default_corpus ? default_corpus : "");
  if (dir) (void)collect_regular_files_recursive(dir, &files);
  qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
  int limit = atoi(value_after(argc, argv, 3, "--limit", "0"));
  if (limit <= 0) limit = has_flag_after(argc, argv, 3, "--fast") ? 8 : 32;
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "30"));
  bool run_samples = strcmp(kind, "hosts") == 0 || has_flag_after(argc, argv, 3, "--run-samples");
  int scanned = 0, validated = 0, rejected = 0, promoted = 0, duplicates = 0;
  for (int i = 0; i < files.count && rows.count < limit; ++i) {
    if (!ny_has_suffix(files.items[i], ".ny")) continue;
    ++scanned;
    proc_result_t pr = {0};
    bool compile_ok = compile_or_run_ny_source(root, ny_bin, files.items[i], run_samples, timeout_s, &pr);
    bool crash = proc_result_crashed(&pr);
    char *normalized = normalize_output_pair(pr.out, pr.err);
    char sample_hash[32], source_hash[32];
    snprintf(sample_hash, sizeof(sample_hash), "%016" PRIx64,
             fnv1a64(normalized ? normalized : "", strlen(normalized ? normalized : "")));
    file_hash_hex(files.items[i], source_hash, sizeof(source_hash));
    if (compile_ok) ++validated;
    else ++rejected;
    if (crash) {
      (void)string_list_push_take(&failures, make_native_proc_failure_row(ny_base_name(files.items[i]), kind, &pr));
    }
    bool promote_ok = false, did_promote = false, duplicate = false;
    char *promotion = NULL;
    if (compile_ok && corpus_dir && *corpus_dir) {
      promotion = promote_real_db_source(root, corpus_dir, kind, files.items[i],
                                         source_hash, sample_hash, normalized ? normalized : "",
                                         run_samples, pr.elapsed_ms,
                                         &promote_ok, &did_promote, &duplicate);
      if (did_promote) ++promoted;
      if (duplicate) ++duplicates;
      if (!promote_ok && promotion) (void)string_list_push_copy(&failures, promotion);
    }
    char stem[128];
    safe_stem(stem, sizeof(stem), files.items[i]);
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"case\":");
    (void)sb_append_json_str(&row, stem);
    (void)sb_append(&row, ",\"name\":");
    (void)sb_append_json_str(&row, ny_base_name(files.items[i]));
    (void)sb_appendf(&row, ",\"ok\":%s,\"validated\":%s,\"crash\":%s,\"rc\":%d,\"kind\":",
                     crash ? "false" : "true", compile_ok ? "true" : "false",
                     crash ? "true" : "false", pr.rc);
    (void)sb_append_json_str(&row, kind);
    (void)sb_append(&row, ",\"generator\":\"real-db\",\"generator_kind\":\"real-db\",\"method\":\"real-db\",\"source_kind\":");
    (void)sb_append_json_str(&row, real_db_source_kind(kind));
    (void)sb_append(&row, ",\"family\":");
    (void)sb_append_json_str(&row, real_db_family(kind));
    (void)sb_append(&row, ",\"shape\":\"real-db-source\",\"features\":");
    (void)sb_append(&row, real_db_features_json(kind, run_samples));
    (void)sb_append(&row, ",\"validation\":");
    (void)sb_append_json_str(&row, run_samples ? "run" : "compile");
    (void)sb_append(&row, ",\"source\":");
    append_rel_json_str(&row, root, files.items[i]);
    (void)sb_append(&row, ",\"ny_source\":");
    (void)sb_append_json_str(&row, files.items[i]);
    (void)sb_append(&row, ",\"source_hash\":");
    (void)sb_append_json_str(&row, source_hash);
    (void)sb_append(&row, ",\"structural_hash\":");
    (void)sb_append_json_str(&row, source_hash);
    (void)sb_append(&row, ",\"behavior_hash\":");
    (void)sb_append_json_str(&row, sample_hash);
    (void)sb_append(&row, ",\"expected_output\":");
    (void)sb_append_json_str(&row, normalized ? normalized : "");
    (void)sb_append(&row, ",\"sample_output_hash\":");
    (void)sb_append_json_str(&row, sample_hash);
    if (promotion) {
      char *entry_id = json_string_or_empty(promotion, "id");
      (void)sb_append(&row, ",\"entry_id\":");
      (void)sb_append_json_str(&row, entry_id ? entry_id : "");
      free(entry_id);
    }
    (void)sb_appendf(&row, ",\"promoted\":%s,\"duplicate\":%s",
                     did_promote ? "true" : "false", duplicate ? "true" : "false");
    (void)sb_appendf(&row, ",\"elapsed_ms\":%.2f", pr.elapsed_ms);
    if (!compile_ok) append_proc_tail_fields(&row, &pr);
    (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
    (void)string_list_push_take(&rows, sb_take(&row));
    free(promotion);
    free(normalized);
    proc_result_free(&pr);
  }
  if (!rows.count) (void)string_list_push_take(&rows, native_row_status(kind, "corpus-real-db", true, "note", "no candidates found"));
  str_buf_t extra = {0};
  (void)sb_append(&extra, ",\"kind\":");
  (void)sb_append_json_str(&extra, kind);
  (void)sb_append(&extra, ",\"generator\":\"real-db\",\"generator_kind\":\"real-db\",\"method\":\"real-db\"");
  (void)sb_append(&extra, ",\"corpus_dir\":");
  (void)sb_append_json_str(&extra, corpus_dir);
  (void)sb_appendf(&extra, ",\"entries\":%d,\"scanned\":%d,\"validated\":%d,\"rejected\":%d,"
                   "\"promoted\":%d,\"duplicates\":%d,\"run_samples\":%s",
                   rows.count, scanned, validated, rejected, promoted, duplicates,
                   run_samples ? "true" : "false");
  char *report = build_native_report_json(&rows, &failures, kind, extra.data);
  int rc = emit_native_report(report, json_path, kind, rows.count, failures.count);
  free(extra.data); free(dir); free(default_corpus);
  string_list_free(&files); string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static int cmd_public_replay_list(int argc, char **argv) {
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"entries\":[]}\n");
    return 2;
  }
  bool emit_json = has_flag_after(argc, argv, 3, "--json");
  const char *tag = value_after(argc, argv, 3, "--tag", "");
  char *dir = nynth_cache_replay_dir();
  char *legacy_dir = NULL;
  (void)nynth_asprintf(&legacy_dir, "fuzz/work/replay");
  string_list_t files = {0};
  if (dir) (void)collect_regular_files_recursive(dir, &files);
  if (legacy_dir) (void)collect_regular_files_recursive(legacy_dir, &files);
  qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
  str_buf_t out = {0};
  if (emit_json) (void)sb_append(&out, "{\"entries\":[");
  bool first = true;
  for (int i = 0; i < files.count; ++i) {
    if (!ny_has_suffix(files.items[i], ".ny")) continue;
    char stem[256];
    stem_name(files.items[i], stem, sizeof(stem));
    if (tag && *tag) {
      char *meta = NULL, *legacy_meta = NULL;
      (void)asprintf(&meta, "%s/%s.json", dir ? dir : "", stem);
      (void)asprintf(&legacy_meta, "%s/%s.json", legacy_dir ? legacy_dir : "", stem);
      file_buf_t mf = {0};
      bool has_tag = read_file(meta, &mf) && mf.data && strstr(mf.data, tag);
      free(mf.data);
      mf.data = NULL;
      if (!has_tag) has_tag = read_file(legacy_meta, &mf) && mf.data && strstr(mf.data, tag);
      free(mf.data); free(meta); free(legacy_meta);
      if (!has_tag) continue;
    }
    if (emit_json) {
      if (!first) (void)sb_append_c(&out, ',');
      first = false;
      (void)sb_append(&out, "{\"id\":");
      (void)sb_append_json_str(&out, stem);
      (void)sb_append(&out, ",\"source\":");
      append_rel_json_str(&out, root, files.items[i]);
      (void)sb_append(&out, "}");
    } else {
      printf("%s\t%s\n", stem, files.items[i]);
    }
  }
  if (emit_json) {
    (void)sb_append(&out, "]}");
    puts(out.data ? out.data : "{\"entries\":[]}");
  }
  free(out.data); free(dir); free(legacy_dir); string_list_free(&files);
  return 0;
}

static void safe_stem(char *out, size_t out_sz, const char *raw) {
  if (!out_sz) return;
  size_t n = 0;
  const char *base = ny_base_name(raw && *raw ? raw : "replay_case");
  for (const char *p = base; *p && n + 1 < out_sz; ++p) {
    char c = *p;
    if (c == '.') break;
    out[n++] = (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
  }
  if (!n) out[n++] = 'r';
  out[n] = '\0';
}

static int cmd_public_replay_promote(int argc, char **argv) {
  if (argc < 4) return worker_usage();
  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *artifact = argv[3];
  const char *name_arg = value_after(argc, argv, 4, "--name", "");
  const char *note = value_after(argc, argv, 4, "--note", "");
  string_list_t tags = {0};
  for (int i = 4; i < argc; ++i) {
    if (strncmp(argv[i], "--tag=", 6) == 0) {
      (void)string_list_push_copy(&tags, argv[i] + 6);
      continue;
    }
    if (strcmp(argv[i], "--tag") == 0) {
      if (i + 1 < argc) {
        (void)string_list_push_copy(&tags, argv[i + 1]);
        ++i;
      }
    }
  }
  char *source = NULL;
  file_buf_t payload = {0};
  if (ny_has_suffix(artifact, ".json") && read_file(artifact, &payload) && payload.data) {
    source = json_string_or_empty(payload.data, "source");
    if (!source || !*source) { free(source); source = json_string_or_empty(payload.data, "path"); }
    if (!source || !*source) { free(source); source = json_string_or_empty(payload.data, "ny_source"); }
  } else {
    source = strdup(artifact);
  }
  char *resolved_source = resolve_existing_file(root, source);
  if (!source || !resolved_source) {
    printf("{\"ok\":false,\"error\":\"artifact-source-missing\",\"artifact\":");
    json_str(stdout, artifact);
    printf("}\n");
    string_list_free(&tags);
    free(source); free(resolved_source); free(payload.data);
    return 1;
  }
  char stem[256];
  safe_stem(stem, sizeof(stem), name_arg && *name_arg ? name_arg : resolved_source);
  char *replay_dir = NULL, *dst = NULL, *meta = NULL;
  replay_dir = nynth_cache_replay_dir();
  (void)asprintf(&dst, "%s/%s.ny", replay_dir ? replay_dir : "", stem);
  (void)asprintf(&meta, "%s/%s.json", replay_dir ? replay_dir : "", stem);
  file_buf_t sf = {0};
  bool ok = replay_dir && dst && meta && mkdir_p(replay_dir) && read_file(resolved_source, &sf) &&
            write_file_bytes(dst, (unsigned char *)sf.data, sf.len);
  free(sf.data);
  if (!ok) {
    printf("{\"ok\":false,\"error\":\"replay-promote-failed\"}\n");
    string_list_free(&tags);
    free(source); free(resolved_source); free(payload.data); free(replay_dir); free(dst); free(meta);
    return 1;
  }
  str_buf_t m = {0};
  (void)sb_append(&m, "{\"id\":");
  (void)sb_append_json_str(&m, stem);
  (void)sb_append(&m, ",\"source\":");
  append_rel_json_str(&m, root, dst);
  (void)sb_append(&m, ",\"origin_artifact\":");
  (void)sb_append_json_str(&m, artifact);
  (void)sb_append(&m, ",\"expected\":\"no_crash\",\"tags\":");
  append_string_list_json(&m, &tags);
  (void)sb_append(&m, ",\"note\":");
  (void)sb_append_json_str(&m, note);
  (void)sb_append(&m, ",\"engine\":\"nynth_core\"}\n");
  (void)write_file_text(meta, m.data ? m.data : "{}\n");
  printf("replay source: %s\n", dst);
  printf("replay meta:   %s\n", meta);
  free(m.data); string_list_free(&tags);
  free(source); free(resolved_source); free(payload.data); free(replay_dir); free(dst); free(meta);
  return 0;
}

static int reducer_line_count(const char *data, size_t len) {
  if (!data || !len) return 0;
  int lines = 0;
  for (size_t i = 0; i < len; ++i) {
    if (data[i] == '\n') ++lines;
  }
  if (len && data[len - 1] != '\n') ++lines;
  return lines;
}

static bool reducer_line_bounds(const char *data, size_t len, int line_index,
                                size_t *start, size_t *end) {
  int line = 0;
  size_t s = 0;
  for (size_t i = 0; i <= len; ++i) {
    if (i != len && data[i] != '\n') continue;
    size_t e = i < len ? i + 1u : i;
    if (line == line_index) {
      *start = s;
      *end = e;
      return true;
    }
    s = i + 1u;
    ++line;
  }
  return false;
}

static char *reducer_remove_line_range(const char *data, size_t len,
                                       int first_line, int line_count,
                                       size_t *out_len) {
  size_t start = 0, end = 0, last_start = 0, last_end = 0;
  if (line_count <= 0 ||
      !reducer_line_bounds(data, len, first_line, &start, &end) ||
      !reducer_line_bounds(data, len, first_line + line_count - 1, &last_start, &last_end))
    return NULL;
  (void)end;
  (void)last_start;
  if (start > len || last_end > len || start > last_end) return NULL;
  byte_buf_t b = {0};
  if (!byte_buf_append(&b, data, start) ||
      !byte_buf_append(&b, data + last_end, len - last_end)) {
    free(b.data);
    return NULL;
  }
  if (!b.len || b.data[b.len - 1] != '\n') (void)byte_buf_append(&b, "\n", 1);
  if (out_len) *out_len = b.len;
  return b.data;
}

static bool reducer_output_has_expect(const proc_result_t *pr, const char *expect) {
  if (!expect || !*expect) return true;
  return (pr && pr->out && strstr(pr->out, expect)) ||
         (pr && pr->err && strstr(pr->err, expect));
}

static bool reducer_mode_wants_runtime(const char *mode) {
  return contains_ci(mode, "runtime") || contains_ci(mode, "differential") ||
         contains_ci(mode, "metamorphic") || contains_ci(mode, "run");
}

static bool reducer_candidate_preserves(reducer_context_t *ctx,
                                        const char *data, size_t len) {
  if (!ctx || !ctx->tmp_source || !ctx->ny_bin || !data) return false;
  if (ctx->max_checks > 0 && ctx->checks >= ctx->max_checks) return false;
  ++ctx->checks;
  if (!write_file_bytes(ctx->tmp_source, (const unsigned char *)data, len)) return false;
  bool run = reducer_mode_wants_runtime(ctx->mode);
  proc_result_t pr = {0};
  (void)compile_or_run_ny_source_flavor(ctx->root, ctx->ny_bin, ctx->tmp_source, run,
                                        ctx->flavor, ctx->timeout_s, &pr);
  bool preserved = false;
  if (contains_ci(ctx->mode, "timeout")) preserved = pr.timed_out;
  else if (contains_ci(ctx->mode, "crash")) preserved = proc_result_crashed(&pr);
  else preserved = pr.rc != 0;
  if (preserved && !reducer_output_has_expect(&pr, ctx->expect_substring)) preserved = false;
  proc_result_free(&pr);
  return preserved;
}

static char *reduce_source_greedy_lines(reducer_context_t *ctx,
                                        const char *source, size_t source_len,
                                        size_t *out_len, int *removed_lines) {
  char *current = strndup_local(source, source_len);
  if (!current) return NULL;
  size_t current_len = source_len;
  if (!current_len || current[current_len - 1] != '\n') {
    char *next = NULL;
    if (asprintf(&next, "%s\n", current) >= 0 && next) {
      free(current);
      current = next;
      current_len = strlen(current);
    }
  }
  int original_lines = reducer_line_count(current, current_len);
  for (int chunk = original_lines > 1 ? original_lines / 2 : 1; chunk >= 1; chunk /= 2) {
    bool changed = true;
    while (changed) {
      changed = false;
      int lines = reducer_line_count(current, current_len);
      for (int line = 0; line < lines; ) {
        int take = chunk;
        if (line + take > lines) take = lines - line;
        size_t candidate_len = 0;
        char *candidate = reducer_remove_line_range(current, current_len, line, take, &candidate_len);
        if (candidate && candidate_len > 0 && reducer_candidate_preserves(ctx, candidate, candidate_len)) {
          free(current);
          current = candidate;
          current_len = candidate_len;
          changed = true;
          lines = reducer_line_count(current, current_len);
          continue;
        }
        free(candidate);
        line += take > 0 ? take : 1;
      }
      if (ctx->max_checks > 0 && ctx->checks >= ctx->max_checks) break;
    }
    if (chunk == 1) break;
  }
  if (out_len) *out_len = current_len;
  if (removed_lines) {
    int final_lines = reducer_line_count(current, current_len);
    *removed_lines = original_lines > final_lines ? original_lines - final_lines : 0;
  }
  return current;
}

static int cmd_public_reduce_artifact(int argc, char **argv) {
  char root[4096], ny_bin[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"nynth-root-not-found\"}\n");
    return 2;
  }
  const char *ny_bin_arg = value_after(argc, argv, 3, "--ny-bin", "");
  if (ny_bin_arg && *ny_bin_arg) {
    if (!executable_path(ny_bin_arg) || strlen(ny_bin_arg) >= sizeof(ny_bin)) {
      printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
      return 2;
    }
    snprintf(ny_bin, sizeof(ny_bin), "%s", ny_bin_arg);
  } else if (!find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
    return 2;
  }
  const char *artifact = value_after(argc, argv, 3, "--artifact", "");
  const char *source_arg = value_after(argc, argv, 3, "--source", "");
  const char *out_arg = value_after(argc, argv, 3, "--out", "");
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *mode_arg = value_after(argc, argv, 3, "--mode", "");
  const char *expect_arg = value_after(argc, argv, 3, "--expect-substring", "");
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "20"));
  int max_checks = atoi(value_after(argc, argv, 3, "--max-checks", "250"));
  if (max_checks < 1) max_checks = 250;
  if (!artifact || !*artifact) {
    for (int i = 3; i < argc; ++i) {
      if (strncmp(argv[i], "--", 2) == 0) {
        if (i + 1 < argc &&
            (strcmp(argv[i], "--artifact") == 0 || strcmp(argv[i], "--source") == 0 ||
             strcmp(argv[i], "--out") == 0 || strcmp(argv[i], "--json") == 0 ||
             strcmp(argv[i], "--ny-bin") == 0 || strcmp(argv[i], "--timeout-s") == 0 ||
             strcmp(argv[i], "--max-checks") == 0 ||
             strcmp(argv[i], "--mode") == 0 || strcmp(argv[i], "--expect-stage") == 0 ||
             strcmp(argv[i], "--expect-substring") == 0)) {
          ++i;
        }
        continue;
      }
      artifact = argv[i];
      break;
    }
  }
  if ((!artifact || !*artifact) && source_arg && *source_arg) artifact = source_arg;
  string_list_t rows = {0}, failures = {0};
  char *artifact_path = resolve_existing_file(root, artifact);
  file_buf_t artifact_data = {0};
  if (artifact_path) (void)read_file(artifact_path, &artifact_data);
  char *json_source = NULL, *json_mode = NULL, *json_expect = NULL, *json_flavor = NULL;
  if (artifact_data.data && ny_has_suffix(artifact_path, ".json")) {
    json_source = json_string_or_empty(artifact_data.data, "source");
    if (!json_source || !*json_source) { free(json_source); json_source = json_string_or_empty(artifact_data.data, "path"); }
    if (!json_source || !*json_source) { free(json_source); json_source = json_string_or_empty(artifact_data.data, "ny_source"); }
    if (!json_source || !*json_source) { free(json_source); json_source = json_string_or_empty(artifact_data.data, "base_source"); }
    json_mode = json_string_or_empty(artifact_data.data, "mode");
    if (!json_mode || !*json_mode) { free(json_mode); json_mode = json_string_or_empty(artifact_data.data, "phase"); }
    if (!json_mode || !*json_mode) { free(json_mode); json_mode = json_string_or_empty(artifact_data.data, "reducer_mode"); }
    json_expect = json_string_or_empty(artifact_data.data, "expect_substring");
    if (!json_expect || !*json_expect) { free(json_expect); json_expect = json_string_or_empty(artifact_data.data, "expected_error"); }
    if (!json_expect || !*json_expect) { free(json_expect); json_expect = json_string_or_empty(artifact_data.data, "stderr_substring"); }
    json_flavor = json_string_or_empty(artifact_data.data, "flavor");
    if (!json_flavor || !*json_flavor) { free(json_flavor); json_flavor = json_string_or_empty(artifact_data.data, "failing_flavor"); }
  }
  const char *mode = mode_arg && *mode_arg ? mode_arg : (json_mode && *json_mode ? json_mode : "compile_fail");
  const char *expect = expect_arg && *expect_arg ? expect_arg : (json_expect ? json_expect : "");
  const char *flavor = json_flavor ? json_flavor : "";
  const char *source_choice = source_arg && *source_arg ? source_arg : (json_source ? json_source : "");
  if ((!source_choice || !*source_choice) && artifact_path && !ny_has_suffix(artifact_path, ".json")) source_choice = artifact_path;
  char *source_path = resolve_existing_file(root, source_choice);
  file_buf_t source_data = {0};
  bool ok = artifact_path && source_path && read_file(source_path, &source_data);
  char *out_path = NULL;
  if (ok) {
    if (out_arg && *out_arg) {
      out_path = path_is_absolute(out_arg) ? strdup(out_arg) : NULL;
      if (!out_path) (void)asprintf(&out_path, "%s/%s", root, out_arg);
    } else {
      char stem[256];
      safe_stem(stem, sizeof(stem), source_path);
      out_path = nynth_cache_reduced_path(stem);
    }
  }
  if (!ok || !out_path) {
    if (!artifact_path)
      (void)string_list_push_take(&failures, make_worker_failure_row("reduce", "artifact", 1, "", "artifact missing"));
    else if (!source_path)
      (void)string_list_push_take(&failures, make_worker_failure_row("reduce", "artifact", 1, "", "source missing"));
    else
      (void)string_list_push_take(&failures, make_worker_failure_row("reduce", "artifact", 1, "", "output path allocation failed"));
    (void)string_list_push_take(&rows, native_row_status("reduce_artifact", "reducer", false, "artifact", artifact ? artifact : ""));
  } else {
    char *tmp_source = NULL;
    tmp_source = nynth_scratch_pathf(NULL, "reduce/tmp_%ld.ny", (long)getpid());
    if (tmp_source) (void)mkdir_parent(tmp_source);
    reducer_context_t ctx = {
      .root = root,
      .ny_bin = ny_bin,
      .mode = mode,
      .expect_substring = expect,
      .flavor = flavor,
      .timeout_s = timeout_s,
      .tmp_source = tmp_source,
      .checks = 0,
      .max_checks = max_checks
    };
    bool preserves_original = reducer_candidate_preserves(&ctx, source_data.data, source_data.len);
    size_t reduced_len = 0;
    int removed_lines = 0;
    char *reduced = NULL;
    if (preserves_original) {
      reduced = reduce_source_greedy_lines(&ctx, source_data.data, source_data.len,
                                           &reduced_len, &removed_lines);
    }
    if (!reduced) {
      reduced = strndup_local(source_data.data, source_data.len);
      reduced_len = source_data.len;
      if (reduced && (!reduced_len || reduced[reduced_len - 1] != '\n')) {
        char *with_newline = NULL;
        if (asprintf(&with_newline, "%s\n", reduced) >= 0 && with_newline) {
          free(reduced);
          reduced = with_newline;
          reduced_len = strlen(reduced);
        }
      }
    }
    ok = reduced && write_file_bytes(out_path, (const unsigned char *)reduced, reduced_len);
    if (!ok) {
      (void)string_list_push_take(&failures, make_worker_failure_row("reduce", "artifact", 1, "", "write failed"));
    } else if (!preserves_original) {
      (void)string_list_push_take(&failures, make_worker_failure_row("reduce", "artifact", 1, "", "original source did not preserve requested failure predicate"));
    }
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"name\":\"reduce_artifact\",\"kind\":\"reducer\",\"ok\":");
    (void)sb_append(&row, (ok && preserves_original) ? "true" : "false");
    (void)sb_append(&row, ",\"engine\":\"nynth_core\",\"artifact\":");
    append_rel_json_str(&row, root, artifact_path);
    (void)sb_append(&row, ",\"source\":");
    append_rel_json_str(&row, root, source_path);
    (void)sb_append(&row, ",\"reduced_source\":");
    append_rel_json_str(&row, root, out_path);
    (void)sb_append(&row, ",\"expect_substring\":");
    (void)sb_append_json_str(&row, expect);
    (void)sb_append(&row, ",\"flavor\":");
    (void)sb_append_json_str(&row, flavor);
    (void)sb_appendf(&row,
                     ",\"original_bytes\":%zu,\"reduced_bytes\":%zu,"
                     "\"original_lines\":%d,\"reduced_lines\":%d,"
                     "\"removed_lines\":%d,\"checks\":%d,\"max_checks\":%d,"
                     "\"preserved\":%s,\"timeout_s\":%.2f,\"reducer_mode\":",
                     source_data.len, reduced_len,
                     count_lines(source_data.data, source_data.len),
                     count_lines(reduced ? reduced : "", reduced_len),
                     removed_lines, ctx.checks, max_checks,
                     preserves_original ? "true" : "false", timeout_s);
    (void)sb_append_json_str(&row, mode);
    (void)sb_append_c(&row, '}');
    (void)string_list_push_take(&rows, sb_take(&row));
    if (tmp_source) unlink(tmp_source);
    free(tmp_source);
    free(reduced);
  }
  str_buf_t extra = {0};
  if (out_path) {
    (void)sb_append(&extra, ",\"out\":");
    append_rel_json_str(&extra, root, out_path);
  }
  (void)sb_append(&extra, ",\"reducer_mode\":");
  (void)sb_append_json_str(&extra, mode);
  if (flavor && *flavor) {
    (void)sb_append(&extra, ",\"flavor\":");
    (void)sb_append_json_str(&extra, flavor);
  }
  char *report = build_native_report_json(&rows, &failures, "reduce-artifact", extra.data);
  int rc = emit_native_report(report, json_path, "reduce", rows.count, failures.count);
  free(extra.data);
  free(artifact_path); free(artifact_data.data); free(json_source); free(json_mode); free(json_expect); free(json_flavor);
  free(source_path); free(source_data.data); free(out_path);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static int cmd_public_fuzz_frontend(int argc, char **argv) {
  char root[4096], ny_bin[4096];
  if (!find_repo_root_or_sibling(root, sizeof(root)) || !find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  int rounds = atoi(value_after(argc, argv, 3, "--rounds", "120"));
  if (rounds < 1) rounds = 1;
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "20"));
  string_list_t rows = {0}, failures = {0}, files = {0};
  char *dir = NULL, *replay_dir = NULL;
  (void)nynth_asprintf(&dir, "etc/tests/rt");
  replay_dir = nynth_cache_replay_dir();
  char *legacy_replay_dir = NULL;
  (void)nynth_asprintf(&legacy_replay_dir, "fuzz/work/replay");
  if (dir) (void)collect_regular_files_recursive(dir, &files);
  if (replay_dir) (void)collect_regular_files_recursive(replay_dir, &files);
  if (legacy_replay_dir) (void)collect_regular_files_recursive(legacy_replay_dir, &files);
  qsort(files.items, (size_t)files.count, sizeof(char *), cmp_cstr);
  int attempted = 0, compiled = 0, diagnostics = 0;
  for (int i = 0; i < files.count && attempted < rounds; ++i) {
    if (!ny_has_suffix(files.items[i], ".ny")) continue;
    ++attempted;
    proc_result_t pr = {0};
    bool ok_compile = compile_or_run_ny_source(root, ny_bin, files.items[i], false, timeout_s, &pr);
    bool crash = proc_result_crashed(&pr);
    if (ok_compile) ++compiled;
    else ++diagnostics;
    if (crash) {
      (void)string_list_push_take(&failures, make_native_proc_failure_row(ny_base_name(files.items[i]), "fuzz-frontend", &pr));
    }
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"case\":");
    (void)sb_append_json_str(&row, ny_base_name(files.items[i]));
    (void)sb_appendf(&row, ",\"ok\":%s,\"compiled\":%s,\"diagnostic\":%s,\"crash\":%s,\"rc\":%d,\"elapsed_ms\":%.2f,\"source\":",
                     crash ? "false" : "true", ok_compile ? "true" : "false",
                     (!ok_compile && !crash) ? "true" : "false", crash ? "true" : "false",
                     pr.rc, pr.elapsed_ms);
    append_rel_json_str(&row, root, files.items[i]);
    if (!ok_compile) append_proc_tail_fields(&row, &pr);
    (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
    (void)string_list_push_take(&rows, sb_take(&row));
    proc_result_free(&pr);
  }
  if (!rows.count) (void)string_list_push_take(&rows, native_row_status("frontend_seed_scan", "fuzz", true, "note", "no ny corpus seeds found"));
  str_buf_t extra = {0};
  (void)sb_appendf(&extra, ",\"attempted\":%d,\"compiled\":%d,\"diagnostics\":%d,\"timeout_s\":%.2f",
                   attempted, compiled, diagnostics, timeout_s);
  char *report = build_native_report_json(&rows, &failures, "fuzz-frontend", extra.data);
  int rc = emit_native_report(report, json_path, "frontend fuzz", rows.count, failures.count);
  free(extra.data); free(dir); free(replay_dir); free(legacy_replay_dir);
  string_list_free(&files); string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static const char *snippet_actual_mode(const char *mode, int idx) {
  static const char *modes[] = {
    "parser", "memory", "imports", "slices", "match", "sugar", "data", "layout",
    "dict", "closures", "nullable", "strings", "bitops", "loops"
  };
  if (!mode || !*mode || strcmp(mode, "mixed") == 0)
    return modes[idx % (int)(sizeof(modes) / sizeof(modes[0]))];
  return mode;
}

static char *make_snippet_source(const char *mode, int idx) {
  int a = (idx % 17) + 3;
  int b = (idx % 11) + 5;
  if (strcmp(mode, "memory") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "def xs = [%d, %d, %d, %d]\n"
      "mut acc = 0\n"
      "mut i = 0\n"
      "while(i < xs.len){\n"
      "   acc += xs[i]\n"
      "   i += 1\n"
      "}\n"
      "print(acc)\n", a, b, a + b, a * 2);
    return s;
  }
  if (strcmp(mode, "imports") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "use std.os.time\n"
      "def t = ticks()\n"
      "print(to_str(t >= 0))\n");
    return s;
  }
  if (strcmp(mode, "slices") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "def xs = [%d, %d, %d]\n"
      "print(xs[0] + xs[-1])\n", a, a + 1, a + 2);
    return s;
  }
  if (strcmp(mode, "match") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "def x = %d\n"
      "def y = match x {\n"
      "   %d -> \"hit\"\n"
      "   _ -> \"miss\"\n"
      "}\n"
      "print(y)\n", a, a);
    return s;
  }
  if (strcmp(mode, "sugar") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "fn inc(x){\n"
      "   x + 1\n"
      "}\n"
      "def x = %d |> inc()\n"
      "def y = x > %d ? x : %d\n"
      "print(y)\n", a, b, b);
    return s;
  }
  if (strcmp(mode, "data") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "def d = {\"a\": %d, \"b\": %d}\n"
      "def xs = [d[\"a\"], d[\"b\"]]\n"
      "print(xs[0] + xs[1])\n", a, b);
    return s;
  }
  if (strcmp(mode, "layout") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "layout Point {\n"
      "   x: i64,\n"
      "   y: i64\n"
      "}\n"
      "print(%d + %d)\n", a, b);
    return s;
  }
  if (strcmp(mode, "dict") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "def d = dict().set(\"a\", %d).set(\"b\", %d)\n"
      "def e = d.set(\"a\", d.get(\"a\", 0) + 1)\n"
      "print(e.get(\"a\", 0) + e.get(\"b\", 0))\n", a, b);
    return s;
  }
  if (strcmp(mode, "closures") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "fn apply(x, f){\n"
      "   f(x)\n"
      "}\n"
      "def y = apply(%d, fn(v){ v + %d })\n"
      "print(y)\n", a, b);
    return s;
  }
  if (strcmp(mode, "nullable") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "fn pick(flag){\n"
      "   flag ? [%d, %d] : nil\n"
      "}\n"
      "def xs = pick(true)\n"
      "if(xs != nil){ print(xs[0] + xs[1]) } else { print(0) }\n", a, b);
    return s;
  }
  if (strcmp(mode, "strings") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "def s = \"n\" + to_str(%d) + \"y\"\n"
      "print(s.len + ord(get(s, 0)) + ord(get(s, s.len - 1)))\n", a);
    return s;
  }
  if (strcmp(mode, "bitops") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "def x = ((%d << 3) ^ %d) & 255\n"
      "print((x | 1) + (x >> 2))\n", a, b);
    return s;
  }
  if (strcmp(mode, "loops") == 0) {
    char *s = NULL;
    (void)asprintf(&s,
      "use std.core\n"
      "mut acc = 0\n"
      "mut i = 0\n"
      "while(i < %d){\n"
      "   mut j = 0\n"
      "   while(j < %d){\n"
      "      acc += (i + j) & 7\n"
      "      j += 1\n"
      "   }\n"
      "   i += 1\n"
      "}\n"
      "print(acc)\n", a, b);
    return s;
  }
  char *s = NULL;
  (void)asprintf(&s,
    "use std.core\n"
    "def x = ((%d + %d) * 2) - %d\n"
    "assert(x > 0, \"parser snippet\")\n"
    "print(x)\n", a, b, b);
  return s;
}

static int cmd_public_fuzz_snippets(int argc, char **argv) {
  char root[4096], ny_bin[4096];
  if (!find_repo_root_or_sibling(root, sizeof(root)) || !find_ny_bin(root, ny_bin, sizeof(ny_bin))) {
    printf("{\"ok\":false,\"error\":\"ny-bin-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  const char *mode = value_after(argc, argv, 3, "--mode", "mixed");
  int iterations = atoi(value_after(argc, argv, 3, "--iterations", "64"));
  if (iterations < 1) iterations = 1;
  if (has_flag_after(argc, argv, 3, "--fast") && iterations > 16) iterations = 16;
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", "20"));
  string_list_t rows = {0}, failures = {0};
  char *out_dir = NULL;
  (void)nynth_asprintf(&out_dir, "build/fuzz/generated_snippets");
  if (out_dir) ny_ensure_dir_recursive(out_dir);
  int compiled = 0, diagnostics = 0;
  for (int i = 0; i < iterations; ++i) {
    const char *actual_mode = snippet_actual_mode(mode, i);
    char *source = make_snippet_source(actual_mode, i);
    char *path = NULL;
    (void)asprintf(&path, "%s/snippet_%04d_%s.ny", out_dir ? out_dir : "/tmp", i, actual_mode);
    bool wrote = source && path && write_file_text(path, source);
    proc_result_t pr = {0};
    bool compile_ok = false;
    bool crash = false;
    if (wrote) {
      compile_ok = compile_or_run_ny_source(root, ny_bin, path, false, timeout_s, &pr);
      crash = proc_result_crashed(&pr);
    }
    if (compile_ok) ++compiled;
    else ++diagnostics;
    if (!wrote) {
      (void)string_list_push_take(&failures, make_worker_failure_row("snippet", "fuzz-snippets", 1, "", "snippet write failed"));
    } else if (crash) {
      (void)string_list_push_take(&failures, make_native_proc_failure_row(ny_base_name(path), "fuzz-snippets", &pr));
    }
    str_buf_t row = {0};
    (void)sb_append(&row, "{\"case\":");
    (void)sb_append_json_str(&row, path ? ny_base_name(path) : "snippet");
    (void)sb_append(&row, ",\"mode\":");
    (void)sb_append_json_str(&row, actual_mode);
    (void)sb_appendf(&row, ",\"ok\":%s,\"compiled\":%s,\"diagnostic\":%s,\"crash\":%s,\"rc\":%d,\"elapsed_ms\":%.2f,\"source\":",
                     (!crash && wrote) ? "true" : "false", compile_ok ? "true" : "false",
                     (wrote && !compile_ok && !crash) ? "true" : "false",
                     crash ? "true" : "false", wrote ? pr.rc : 1, wrote ? pr.elapsed_ms : 0.0);
    append_rel_json_str(&row, root, path ? path : "");
    if (wrote && !compile_ok) append_proc_tail_fields(&row, &pr);
    (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
    (void)string_list_push_take(&rows, sb_take(&row));
    proc_result_free(&pr);
    free(source);
    free(path);
  }
  str_buf_t extra = {0};
  (void)sb_appendf(&extra, ",\"iterations\":%d,\"compiled\":%d,\"diagnostics\":%d,\"out_dir\":",
                   iterations, compiled, diagnostics);
  append_rel_json_str(&extra, root, out_dir ? out_dir : "");
  char *report = build_native_report_json(&rows, &failures, "fuzz-snippets", extra.data);
  int rc = emit_native_report(report, json_path, "snippet fuzz", rows.count, failures.count);
  free(extra.data);
  free(out_dir);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static void stress_add_step(const char *root, const char *name, char **cmd_argv,
                            const char *report_path, double timeout_s,
                            string_list_t *rows, string_list_t *failures) {
  proc_result_t pr = run_proc(cmd_argv, root, timeout_s);
  file_buf_t report = {0};
  int row_count = -1;
  double failure_count = pr.rc == 0 ? 0.0 : 1.0;
  bool have_report = report_path && *report_path && read_file(report_path, &report) && report.data;
  if (have_report) {
    const char *rows_json = json_value_after_key(report.data, "rows");
    row_count = count_json_array_items(rows_json);
    if (!extract_json_number(report.data, "failure_count", &failure_count))
      failure_count = json_failures_nonempty(report.data) ? 1.0 : 0.0;
  }
  bool ok = pr.rc == 0 && failure_count == 0.0;
  if (!ok) (void)string_list_push_take(failures, make_native_proc_failure_row(name, "stress", &pr));
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"case\":");
  (void)sb_append_json_str(&row, name);
  (void)sb_appendf(&row, ",\"ok\":%s,\"rc\":%d,\"elapsed_ms\":%.2f,\"sub_rows\":%d,\"sub_failures\":%.0f,\"report\":",
                   ok ? "true" : "false", pr.rc, pr.elapsed_ms, row_count, failure_count);
  append_rel_json_str(&row, root, report_path ? report_path : "");
  if (!ok) append_proc_tail_fields(&row, &pr);
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  (void)string_list_push_take(rows, sb_take(&row));
  free(report.data);
  proc_result_free(&pr);
}

static int cmd_public_stress_run(int argc, char **argv) {
  char root[4096];
  if (!find_repo_root(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  bool fast = has_flag_after(argc, argv, 3, "--fast");
  bool full = has_flag_after(argc, argv, 3, "--full");
  double timeout_s = atof(value_after(argc, argv, 3, "--timeout-s", fast ? "180" : "600"));
  string_list_t rows = {0}, failures = {0};

  char *self_json = make_tmp_json_path(root, "stress_selftest", 0, 0);
  char *self_argv[] = {g_self_path, "selftest", "run", "--max-timeout-s", fast ? "30" : "60", "--json", self_json, NULL};
  stress_add_step(root, "selftest", self_argv, self_json, fast ? 90.0 : 180.0, &rows, &failures);

  char *compile_json = make_tmp_json_path(root, "stress_compile", 0, 1);
  char *compile_argv[] = {g_self_path, "bench", "compile", "--fast", "--runs", "1", "--json", compile_json, NULL};
  stress_add_step(root, "bench_compile", compile_argv, compile_json, 90.0, &rows, &failures);

  char *repl_json = make_tmp_json_path(root, "stress_repl_jit", 0, 2);
  char *repl_argv[] = {g_self_path, "bench", "repl-jit", "--fast", "--runs", "2", "--warmup", "0", "--json", repl_json, NULL};
  stress_add_step(root, "bench_repl_jit", repl_argv, repl_json, 120.0, &rows, &failures);

  char *snip_json = make_tmp_json_path(root, "stress_snippets", 0, 3);
  char *snip_argv[] = {g_self_path, "fuzz", "snippets", "--iterations", fast ? "8" : "24", "--mode", "mixed", "--json", snip_json, NULL};
  stress_add_step(root, "fuzz_snippets", snip_argv, snip_json, 180.0, &rows, &failures);

  char *front_json = make_tmp_json_path(root, "stress_frontend", 0, 4);
  char *front_argv[] = {g_self_path, "fuzz", "frontend", "--rounds", fast ? "8" : "32", "--json", front_json, NULL};
  stress_add_step(root, "fuzz_frontend", front_argv, front_json, 180.0, &rows, &failures);

  char *func_json = make_tmp_json_path(root, "stress_functions", 0, 5);
  char *func_argv[] = {g_self_path, "corpus", "build-functions", "--fast", "--limit", fast ? "4" : "12", "--json", func_json, NULL};
  stress_add_step(root, "corpus_build_functions", func_argv, func_json, 180.0, &rows, &failures);

  if (!fast || full) {
    char *hosts_json = make_tmp_json_path(root, "stress_hosts", 0, 6);
    char *hosts_argv[] = {g_self_path, "corpus", "build-hosts", "--limit", "6", "--json", hosts_json, NULL};
    stress_add_step(root, "corpus_build_hosts", hosts_argv, hosts_json, 240.0, &rows, &failures);
    free(hosts_json);

    char *real_json = make_tmp_json_path(root, "stress_real", 0, 7);
    char *real_argv[] = {g_self_path, "bench", "real", "--fast", "--limit", "2", "--runs", "1", "--json", real_json, NULL};
    stress_add_step(root, "bench_real", real_argv, real_json, timeout_s, &rows, &failures);
    free(real_json);
  }

  if ((full || !fast) && !has_flag_after(argc, argv, 3, "--skip-prove")) {
    char *prove_json = make_tmp_json_path(root, "stress_prove", 0, 8);
    char *prove_argv[] = {g_self_path, "prove", "lab", "--fast", "--timeout-s", "60", "--json", prove_json, NULL};
    stress_add_step(root, "prove_lab", prove_argv, prove_json, 240.0, &rows, &failures);
    free(prove_json);
  }

  str_buf_t extra = {0};
  (void)sb_appendf(&extra, ",\"fast\":%s,\"full\":%s,\"lanes\":%d",
                   fast ? "true" : "false", full ? "true" : "false", rows.count);
  char *report = build_native_report_json(&rows, &failures, "stress-run", extra.data);
  int rc = emit_native_report(report, json_path, "stress", rows.count, failures.count);
  free(extra.data);
  free(self_json); free(compile_json); free(repl_json); free(snip_json); free(front_json); free(func_json);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static int cmd_public_campaign_audit(int argc, char **argv) {
  char root[4096];
  if (!find_repo_root_or_sibling(root, sizeof(root))) {
    printf("{\"ok\":false,\"error\":\"repo-root-not-found\"}\n");
    return 2;
  }
  const char *json_path = value_after(argc, argv, 3, "--json", "");
  char *corpus_json = make_tmp_json_path(root, "campaign_audit_corpus", 0, 0);
  char *fuzz_json = make_tmp_json_path(root, "campaign_audit_fuzz", 0, 1);
  char *corpus_argv[] = {g_self_path, "corpus", "audit", "--fast", "--json", corpus_json, NULL};
  char *fuzz_argv[] = {g_self_path, "fuzz", "workspace", "audit", "--json", fuzz_json, NULL};
  proc_result_t cr = run_proc(corpus_argv, root, 60.0);
  proc_result_t fr = run_proc(fuzz_argv, root, 60.0);
  string_list_t rows = {0}, failures = {0};
  bool cok = cr.rc == 0, fok = fr.rc == 0;
  if (!cok) (void)string_list_push_take(&failures, make_worker_failure_row("corpus", "campaign-audit", cr.rc, cr.out, cr.err));
  if (!fok) (void)string_list_push_take(&failures, make_worker_failure_row("fuzz-workspace", "campaign-audit", fr.rc, fr.out, fr.err));
  (void)string_list_push_take(&rows, native_row_status("corpus_audit", "campaign-audit", cok, "report", corpus_json));
  (void)string_list_push_take(&rows, native_row_status("fuzz_workspace", "campaign-audit", fok, "report", fuzz_json));
  char *report = build_native_report_json(&rows, &failures, "campaign-audit", "");
  int rc = emit_native_report(report, json_path, "campaign", rows.count, failures.count);
  free(corpus_json); free(fuzz_json); proc_result_free(&cr); proc_result_free(&fr);
  string_list_free(&rows); string_list_free(&failures);
  return rc;
}

static const char *SELFTEST_FUZZ_PREPARE[] = {"fuzz", "corpus", "prepare", "--json", "$JSON"};
static const char *SELFTEST_FUZZ_AUDIT[] = {"fuzz", "workspace", "audit", "--json", "$JSON"};
static const char *SELFTEST_FUZZ_HARNESS_SMOKE[] = {
  "fuzz", "harness", "smoke", "--target", "all", "--limit", "2", "--timeout-s", "5", "--json", "$JSON"
};
static const char *SELFTEST_FUZZ_LIBS_SMOKE[] = {
  "fuzz", "libs", "smoke", "--mode", "import", "--limit", "32", "--timeout-s", "4", "--json", "$JSON"
};
static const char *SELFTEST_FUZZ_KERNELS_SMOKE[] = {
  "fuzz", "kernels", "smoke", "--limit", "4", "--compile-only", "--timeout-s", "10", "--json", "$JSON"
};
static const char *SELFTEST_FUZZ_ALL_AUDIT[] = {
  "fuzz", "all", "audit", "--report", "$WORK/fuzz_all_report.json", "--strict", "--json", "$JSON"
};
static const char *SELFTEST_FUZZ_ALL_REPORTING[] = {
  "fuzz", "all", "status", "--refresh", "--strict",
  "--dir", "$WORK/fuzz_reporting",
  "--history", "$WORK/fuzz_reporting/history.json",
  "--worklist", "$WORK/fuzz_reporting/worklist.json",
  "--coverage", "$WORK/fuzz_reporting/coverage.json",
  "--plan", "$WORK/fuzz_reporting/plan.json",
  "--target-thread-years", "0.001",
  "--hours", "1",
  "--threads", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_reporting/status.md"
};
static const char *SELFTEST_FUZZ_ALL_STATUS_CANONICAL_CMD[] = {
  "fuzz", "all", "status", "--refresh", "--strict",
  "--dir", "$WORK/fuzz_status_canonical",
  "--history", "$WORK/fuzz_status_canonical/history.json",
  "--worklist", "$WORK/fuzz_status_canonical/worklist.json",
  "--coverage", "$WORK/fuzz_status_canonical/coverage.json",
  "--plan", "$WORK/fuzz_status_canonical/plan.json",
  "--target-thread-years", "0.001",
  "--hours", "1",
  "--threads", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_status_canonical/status.md"
};
static const char *SELFTEST_FUZZ_ALL_STATUS_STALE_EVIDENCE_CMD[] = {
  "fuzz", "all", "status", "--refresh", "--strict",
  "--dir", "$WORK/fuzz_status_stale",
  "--history", "$WORK/fuzz_status_stale/history.json",
  "--worklist", "$WORK/fuzz_status_stale/worklist.json",
  "--coverage", "$WORK/fuzz_status_stale/coverage.json",
  "--plan", "$WORK/fuzz_status_stale/plan.json",
  "--target-thread-years", "0.001",
  "--hours", "1",
  "--threads", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_status_stale/status.md"
};
static const char *SELFTEST_FUZZ_ALL_REPEAT_STATUS_PROGRESS_CMD[] = {
  "fuzz", "all", "status", "--refresh", "--strict",
  "--allow-full-pressure-remediation",
  "--dir", "$WORK/fuzz_repeat_status",
  "--history", "$WORK/fuzz_repeat_status/history.json",
  "--worklist", "$WORK/fuzz_repeat_status/worklist.json",
  "--coverage", "$WORK/fuzz_repeat_status/coverage.json",
  "--plan", "$WORK/fuzz_repeat_status/plan.json",
  "--target-thread-years", "0.001",
  "--hours", "1",
  "--threads", "1",
  "--json", "$WORK/fuzz_repeat_status/repeat-status.json",
  "--markdown", "$WORK/fuzz_repeat_status/repeat-status.md"
};
static const char *SELFTEST_FUZZ_ALL_FRESH_HANDOFF[] = {
  "fuzz", "all", "status", "--refresh", "--strict",
  "--allow-incomplete-coverage",
  "--dir", "$WORK/fuzz_fresh_handoff",
  "--history", "$WORK/fuzz_fresh_handoff/history.json",
  "--worklist", "$WORK/fuzz_fresh_handoff/worklist.json",
  "--coverage", "$WORK/fuzz_fresh_handoff/coverage.json",
  "--plan", "$WORK/fuzz_fresh_handoff/plan.json",
  "--target-thread-years", "0.25",
  "--hours", "1",
  "--threads", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_fresh_handoff/status.md"
};
static const char *SELFTEST_FUZZ_ALL_FULL_PRESSURE_REMEDIATION[] = {
  "fuzz", "all", "status", "--refresh", "--strict",
  "--allow-full-pressure-remediation",
  "--dir", "$WORK/fuzz_full_pressure_remediation",
  "--history", "$WORK/fuzz_full_pressure_remediation/history.json",
  "--worklist", "$WORK/fuzz_full_pressure_remediation/worklist.json",
  "--coverage", "$WORK/fuzz_full_pressure_remediation/coverage.json",
  "--plan", "$WORK/fuzz_full_pressure_remediation/plan.json",
  "--target-thread-years", "0.25",
  "--hours", "1",
  "--threads", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_full_pressure_remediation/status.md"
};
static const char *SELFTEST_FUZZ_REPRO_READY_MISSING_WRAPPER_CMD[] = {
  "fuzz", "all", "worklist",
  "--history", "$WORK/fuzz_repro_ready_missing_wrapper/history.json",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_repro_ready_missing_wrapper/worklist.md"
};
static const char *SELFTEST_FUZZ_REPRO_READY_MISSING_COMMAND_CMD[] = {
  "fuzz", "all", "worklist",
  "--history", "$WORK/fuzz_repro_ready_missing_command/history.json",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_repro_ready_missing_command/worklist.md"
};
static const char *SELFTEST_FUZZ_ALL_DEFAULT_PRESSURE[] = {
  "fuzz", "all", "plan",
  "--dir", "$WORK/fuzz_default_pressure",
  "--history", "$WORK/fuzz_default_pressure/history.json",
  "--worklist", "$WORK/fuzz_default_pressure/worklist.json",
  "--coverage", "$WORK/fuzz_default_pressure/coverage.json",
  "--target-thread-years", "0.25",
  "--hours", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_default_pressure/plan.md"
};
static const char *SELFTEST_FUZZ_ALL_PLAN_COVERAGE_NEXT_CMD[] = {
  "fuzz", "all", "plan",
  "--dir", "$WORK/fuzz_plan_coverage_next",
  "--history", "$WORK/fuzz_plan_coverage_next/history.json",
  "--worklist", "$WORK/fuzz_plan_coverage_next/worklist.json",
  "--coverage", "$WORK/fuzz_plan_coverage_next/coverage.json",
  "--target-thread-years", "0.25",
  "--hours", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_plan_coverage_next/plan.md"
};
static const char *SELFTEST_FUZZ_ALL_COVERAGE_COMMANDS_CMD[] = {
  "fuzz", "all", "coverage",
  "--report", "$WORK/fuzz_coverage_commands/all-run.json",
  "--target-thread-years", "0.001",
  "--hours", "1",
  "--threads", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_coverage_commands/coverage.md"
};
static const char *SELFTEST_FUZZ_ALL_COVERAGE_FOCUS_COMPANIONS_CMD[] = {
  "fuzz", "all", "coverage",
  "--history", "$WORK/fuzz_coverage_focus/history.json",
  "--target-thread-years", "0.001",
  "--hours", "1",
  "--threads", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_coverage_focus/coverage.md"
};
static const char *SELFTEST_FUZZ_ALL_HISTORY_COMMANDS_CMD[] = {
  "fuzz", "all", "history",
  "--dir", "$WORK/fuzz_history_commands",
  "--target-thread-years", "0.001",
  "--hours", "1",
  "--threads", "1",
  "--profile", "insane",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_history_commands/history.md"
};
static const char *SELFTEST_FUZZ_ALL_PREFLIGHT_ISOLATION_CMD[] = {
  "fuzz", "all", "preflight",
  "--no-nytrix-guard",
  "--no-afl",
  "--only-lane", "afl",
  "--work-dir", "$WORK/fuzz_preflight_isolation/work",
  "--dir", "$WORK/fuzz_preflight_isolation/campaign",
  "--target-thread-years", "0.001",
  "--hours", "0.01",
  "--threads", "1",
  "--profile", "insane",
  "--json", "$JSON"
};
static const char *SELFTEST_FUZZ_ALL_HELP_CMD[] = {
  "fuzz", "all", "status", "--help"
};
static const char *SELFTEST_FUZZ_ALL_PROGRESS_CMD[] = {
  "fuzz", "all", "progress",
  "--strict", "--allow-full-pressure-remediation",
  "--status", "$WORK/fuzz_progress/status.json",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_progress/progress.md"
};
static const char *SELFTEST_FUZZ_ALL_PROGRESS_CANONICAL_CMD[] = {
  "fuzz", "all", "progress",
  "--strict", "--allow-full-pressure-remediation",
  "--status", "$WORK/fuzz_progress_canonical/status.json",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_progress_canonical/progress.md"
};
static const char *SELFTEST_FUZZ_ALL_PROGRESS_STALE_EVIDENCE_CMD[] = {
  "fuzz", "all", "progress",
  "--strict", "--allow-full-pressure-remediation",
  "--status", "$WORK/fuzz_progress_stale/status.json",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_progress_stale/progress.md"
};
static const char *SELFTEST_FUZZ_ALL_PROGRESS_REFRESH_FAIL_CMD[] = {
  "fuzz", "all", "progress",
  "--dir", "$WORK/fuzz_progress_refresh_fail",
  "--refresh", "--strict", "--allow-full-pressure-remediation",
  "--target-thread-years", "10",
  "--hours", "8",
  "--threads", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_progress_refresh_fail/progress.md"
};
static const char *SELFTEST_FUZZ_ALL_OLD_PATHS_CMD[] = {
  "fuzz", "all", "old-paths",
  "--nytrix-root", "$WORK/fuzz_old_paths/fake_nytrix",
  "--archive-dir", "$WORK/fuzz_old_paths/archive",
  "--artifact-scan-dir", "$WORK/fuzz_old_paths/artifacts",
  "--apply",
  "--wait-writers-s", "1",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_old_paths/old-paths.md"
};
static const char *SELFTEST_FUZZ_ALL_OLD_PATHS_DRY_RUN_CMD[] = {
  "fuzz", "all", "old-paths",
  "--nytrix-root", "$WORK/fuzz_old_paths_dry/fake_nytrix",
  "--archive-dir", "$WORK/fuzz_old_paths_dry/archive",
  "--artifact-scan-dir", "$WORK/fuzz_old_paths_dry/artifacts",
  "--dry-run",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_old_paths_dry/old-paths.md"
};
static const char *SELFTEST_FUZZ_ALL_OLD_PATHS_EMPTY_DRY_RUN_CMD[] = {
  "fuzz", "all", "old-paths",
  "--nytrix-root", "$WORK/fuzz_old_paths_empty/fake_nytrix",
  "--archive-dir", "$WORK/fuzz_old_paths_empty/archive",
  "--artifact-scan-dir", "$WORK/fuzz_old_paths_empty/artifacts",
  "--dry-run",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_old_paths_empty/old-paths.md"
};
static const char *SELFTEST_FUZZ_ALL_OLD_WRITER_CLASSIFIER_CMD[] = {
  "fuzz", "all", "old-paths",
  "--nytrix-root", "$WORK/fuzz_old_writer_classifier/fake_nytrix",
  "--archive-dir", "$WORK/fuzz_old_writer_classifier/archive",
  "--artifact-scan-dir", "$WORK/fuzz_old_writer_classifier/artifacts",
  "--dry-run",
  "--json", "$JSON",
  "--markdown", "$WORK/fuzz_old_writer_classifier/old-paths.md"
};
static const char *SELFTEST_AFL_DRY[] = {"fuzz", "afl", "run", "--target", "json", "--minutes", "0", "--dry-run", "--json", "$JSON"};
static const char *SELFTEST_AFL_COMPILER_DRY[] = {
  "fuzz", "afl", "run", "--target", "compiler", "--minutes", "0",
  "--dry-run", "--json", "$JSON"
};
static const char *SELFTEST_SAN_DRY[] = {"fuzz", "sanitizers", "run", "--dry-run", "--json", "$JSON"};
static const char *SELFTEST_GC_SMOKE[] = {
  "fuzz", "gc", "run", "--smoke", "--direct-only", "--iterations", "512",
  "--threads", "1", "--json", "$JSON"};
static const char *SELFTEST_GC_CAMPAIGN_COMPACT[] = {
  "fuzz", "gc", "run", "--profile=smoke", "--smoke", "--direct-only",
  "--no-sanitizers", "--no-ny", "--threads", "1", "--json", "$JSON"
};
static const char *SELFTEST_SHAPES[] = {"shapes", "audit", "--json", "$JSON"};
static const char *SELFTEST_CHILD_TMP_ENV[] = {"selftest", "child-tmp-env", "--json", "$JSON"};
static const char *SELFTEST_PYTHON_CLEAN[] = {"selftest", "python-clean", "--json", "$JSON"};
static const char *SELFTEST_CLI_EQUALS_ARGS_CMD[] = {"selftest", "python-clean", "--json=$JSON"};
static const char *SELFTEST_WORKER_EQUALS_ARGS_CMD[] = {"selftest", "worker-args", "--json=$JSON"};
static const char *SELFTEST_ROW_REPORTS_CMD[] = {
  "selftest", "run",
  "--only", "fuzz_gc_campaign_compact",
  "--only", "bridge_unsupported_diagnostic",
  "--json", "$JSON",
  "--markdown", "$WORK/selftest_row_reports.md"
};
static const char *SELFTEST_SKIP_REPORTS_CMD[] = {
  "selftest", "run",
  "--only", "perf_triage",
  "--json", "$JSON",
  "--markdown", "$WORK/selftest_skip_reports.md"
};
static const char *SELFTEST_CATALOG_CMD[] = {
  "selftest", "run", "--list", "--json", "$JSON",
  "--markdown", "$WORK/selftest_catalog.md"
};
static const char *SELFTEST_SYNTH_PRINT[] = {"selftest", "synth-print", "--json", "$JSON"};
static const char *SELFTEST_SYNTH_SCHEDULE[] = {"selftest", "synth-schedule", "--json", "$JSON"};
static const char *SELFTEST_SYNTH_PURE[] = {"selftest", "synth-pure", "--json", "$JSON"};
static const char *SELFTEST_COMPILER_TYPE[] = {
  "compiler", "smoke", "--only", "type", "--json", "$JSON"
};
static const char *SELFTEST_COMPILER_CACHE_CLASSIFIER[] = {
  "selftest", "compiler-cache-classifier", "--json", "$JSON"
};
static const char *SELFTEST_COMPILER_FINDINGS[] = {
  "compiler", "findings", "--timeout-s", "15", "--json", "$JSON"
};
static const char *SELFTEST_COMPILER_KNOWN_BUGS[] = {
  "compiler", "known-bugs", "--timeout-s", "15", "--json", "$JSON"
};
static const char *SELFTEST_COMPILER_STD_AUDIT[] = {
  "compiler", "std-audit", "--json", "$JSON",
  "--markdown", "$WORK/compiler_std_audit.md"
};
static const char *SELFTEST_BENCH_COMPILE[] = {"bench", "compile", "--fast", "--runs", "1", "--json", "$JSON"};
static const char *SELFTEST_BENCH_REPL_JIT[] = {
  "bench", "repl-jit", "--fast", "--runs", "2", "--warmup", "0", "--json", "$JSON"
};
static const char *SELFTEST_FUZZ_FRONTEND[] = {"fuzz", "frontend", "--rounds", "4", "--json", "$JSON"};
static const char *SELFTEST_FUZZ_SNIPPETS[] = {
  "fuzz", "snippets", "--iterations", "4", "--mode", "mixed", "--json", "$JSON"
};
static const char *SELFTEST_PERF_TRIAGE[] = {"perf", "triage", "--fast", "--limit", "2", "--json", "$JSON"};
static const char *SELFTEST_REDUCE_ARTIFACT[] = {
  "reduce", "artifact", "--artifact", "$WORK/reducer_artifact.json",
  "--json", "$JSON",
  "--expect-substring", "not_defined_symbol", "--max-checks", "80"
};
static const char *SELFTEST_SYNTH_GENERATE[] = {
  "synth", "generate", "--fast", "--profile", "optimizer", "--cases", "1",
  "--seed", "11", "--runs", "1", "--warmup", "0", "--timeout-s", "60",
  "--out", "$WORK/generated", "--json", "$JSON"
};
static const char *SELFTEST_SYNTH_IR[] = {
  "synth", "generate", "--fast", "--generator", "ir", "--profile", "optimizer", "--cases", "1",
  "--seed", "12", "--runs", "1", "--warmup", "0", "--timeout-s", "60",
  "--out", "$WORK/generated_ir", "--json", "$JSON"
};
static const char *SELFTEST_SYNTH_STRESS[] = {
  "synth", "generate", "--fast", "--generator", "stress", "--profile", "optimizer",
  "--cases", "1", "--seed", "13", "--runs", "1", "--warmup", "0",
  "--timeout-s", "60", "--out", "$WORK/generated_stress", "--json", "$JSON"
};
static const char *SELFTEST_REAL_DB_FUNCTIONS[] = {
  "corpus", "build-functions", "--corpus-dir", "$WORK/real_db",
  "--fast", "--limit", "2", "--json", "$JSON"
};
static const char *SELFTEST_REAL_DB_HOSTS[] = {
  "corpus", "build-hosts", "--corpus-dir", "$WORK/real_db",
  "--fast", "--limit", "2", "--json", "$JSON"
};
static const char *SELFTEST_SYNTH_REAL[] = {
  "synth", "real", "--corpus-dir", "$WORK/real_db", "--fast", "--cases", "2",
  "--seed", "37", "--timeout-s", "60", "--json", "$JSON"
};
static const char *SELFTEST_CREAL_BUILD[] = {
  "corpus", "creal", "build", "--function-db", "$WORK/creal_functions.json",
  "--corpus-dir", "$WORK/creal", "--limit", "1", "--max-io", "2",
  "--timeout-s", "60", "--json", "$JSON"
};
static const char *SELFTEST_SYNTH_CREAL[] = {
  "synth", "creal", "--function-db", "$WORK/creal_functions.json",
  "--corpus-dir", "$WORK/creal", "--cases", "1", "--seed", "41",
  "--timeout-s", "60", "--json", "$JSON"
};
static const char *SELFTEST_PROVE[] = {"prove", "lab", "--fast", "--timeout-s", "60", "--json", "$JSON"};
static const char *SELFTEST_PERF_TRIAGE_ARGS_CMD[] = {
  "perf", "triage", "--fast", "--limit=1", "--runs=1", "--warmup=0",
  "--timeout-s=60", "--threshold=999", "--json=$JSON",
  "--markdown=$WORK/perf-triage.md"
};
static const char *SELFTEST_BRIDGE_SUITE[] = {"bridge", "suite", "--fast", "--timeout-s", "60", "--json", "--report-json", "$JSON"};
static const char *SELFTEST_BRIDGE_UNSUPPORTED[] = {"bridge", "convert", "$WORK/unsupported_struct.c", "--json"};
static const char *SELFTEST_BRIDGE_GENERATE[] = {
  "bridge", "generate", "--fast", "--profile", "optimizer", "--cases", "1",
  "--seed", "17", "--runs", "1", "--warmup", "0", "--timeout-s", "60", "--json", "$JSON"
};
static const char *SELFTEST_CORPUS_BUILD[] = {
  "corpus", "build", "--corpus-dir", "$WORK/corpus", "--fast", "--cases", "1",
  "--seed", "19", "--profile", "optimizer", "--timeout-s", "60", "--json", "$JSON"
};
static const char *SELFTEST_CORPUS_AUDIT[] = {"corpus", "audit", "--corpus-dir", "$WORK/corpus", "--fast", "--json", "$JSON"};
static const char *SELFTEST_CORPUS_REPLAY[] = {
  "corpus", "replay", "--corpus-dir", "$WORK/corpus", "--limit", "1",
  "--timeout-s", "60", "--json", "$JSON"
};
static const char *SELFTEST_CAMPAIGN_RUN[] = {
  "campaign", "run", "--fast", "--lanes", "typed", "--cases", "1", "--seed", "23",
  "--profile", "optimizer", "--timeout-s", "60", "--json", "$JSON"
};
static const char *SELFTEST_CAMPAIGN_RUN_ALL[] = {
  "campaign", "run", "--fast", "--lanes", "typed,optimizer,torture,ir,stress,cbridge,random,afl",
  "--cases", "1", "--seed", "29", "--profile", "optimizer", "--runs", "1",
  "--warmup", "0", "--timeout-s", "60", "--json", "$JSON"
};
static const char *SELFTEST_CAMPAIGN_OPTIMIZE[] = {
  "campaign", "optimize", "--fast", "--profiles", "optimizer", "--variants", "raw-mutation-off",
  "--cases", "1", "--runs", "1", "--warmup", "0", "--timeout-s", "60",
  "--skip-compile-bench", "--skip-repl-jit", "--skip-correctness", "--skip-corpus", "--json", "$JSON"
};

static const selftest_spec_t SELFTEST_SPECS[] = {
  {"nynth_tree_clean", SELFTEST_PYTHON_CLEAN, 4, 30.0, false, SELFTEST_STANDARD_REPORT},
  {"child_tmp_env", SELFTEST_CHILD_TMP_ENV, 4, 30.0, false, SELFTEST_STANDARD_REPORT},
  {"cli_equals_args", SELFTEST_CLI_EQUALS_ARGS_CMD, 3, 30.0, false, SELFTEST_CLI_EQUALS_ARGS_REPORT},
  {"worker_equals_args", SELFTEST_WORKER_EQUALS_ARGS_CMD, 3, 45.0, false, SELFTEST_WORKER_EQUALS_ARGS_REPORT},
  {"selftest_row_reports", SELFTEST_ROW_REPORTS_CMD, 10, 90.0, false, SELFTEST_SELFTEST_ROW_REPORTS},
  {"selftest_skip_reports", SELFTEST_SKIP_REPORTS_CMD, 8, 30.0, false, SELFTEST_SELFTEST_SKIP_REPORTS},
  {"selftest_catalog", SELFTEST_CATALOG_CMD, 7, 30.0, false, SELFTEST_SELFTEST_CATALOG},
  {"fuzz_corpus_prepare", SELFTEST_FUZZ_PREPARE, 5, 30.0, false, SELFTEST_STANDARD_REPORT},
  {"fuzz_workspace_audit", SELFTEST_FUZZ_AUDIT, 5, 30.0, false, SELFTEST_STANDARD_REPORT},
  {"fuzz_harness_smoke", SELFTEST_FUZZ_HARNESS_SMOKE, 11, 60.0, false, SELFTEST_STANDARD_REPORT},
  {"fuzz_libs_smoke", SELFTEST_FUZZ_LIBS_SMOKE, 11, 90.0, false, SELFTEST_STANDARD_REPORT},
  {"fuzz_kernels_smoke", SELFTEST_FUZZ_KERNELS_SMOKE, 10, 60.0, false, SELFTEST_STANDARD_REPORT},
  {"fuzz_all_audit", SELFTEST_FUZZ_ALL_AUDIT, 9, 30.0, false, SELFTEST_STANDARD_REPORT},
  {"fuzz_all_reporting", SELFTEST_FUZZ_ALL_REPORTING, 25, 30.0, false, SELFTEST_FUZZ_REPORTING},
  {"fuzz_all_status_canonical", SELFTEST_FUZZ_ALL_STATUS_CANONICAL_CMD, 25, 30.0, false, SELFTEST_FUZZ_ALL_STATUS_CANONICAL},
  {"fuzz_all_status_stale_evidence", SELFTEST_FUZZ_ALL_STATUS_STALE_EVIDENCE_CMD, 25, 30.0, false, SELFTEST_FUZZ_ALL_STATUS_STALE_EVIDENCE},
  {"fuzz_all_repeat_status_progress", SELFTEST_FUZZ_ALL_REPEAT_STATUS_PROGRESS_CMD, 26, 30.0, false, SELFTEST_FUZZ_ALL_REPEAT_STATUS_PROGRESS},
  {"fuzz_all_fresh_handoff", SELFTEST_FUZZ_ALL_FRESH_HANDOFF, 26, 30.0, false, SELFTEST_FUZZ_FRESH_HANDOFF},
  {"fuzz_all_full_pressure_remediation", SELFTEST_FUZZ_ALL_FULL_PRESSURE_REMEDIATION, 26, 30.0, false, SELFTEST_FUZZ_FULL_PRESSURE_REMEDIATION},
  {"fuzz_repro_ready_missing_wrapper", SELFTEST_FUZZ_REPRO_READY_MISSING_WRAPPER_CMD, 9, 30.0, false, SELFTEST_FUZZ_REPRO_READY_MISSING_WRAPPER},
  {"fuzz_repro_ready_missing_command", SELFTEST_FUZZ_REPRO_READY_MISSING_COMMAND_CMD, 9, 30.0, false, SELFTEST_FUZZ_REPRO_READY_MISSING_COMMAND},
  {"fuzz_all_default_pressure", SELFTEST_FUZZ_ALL_DEFAULT_PRESSURE, 19, 30.0, false, SELFTEST_FUZZ_DEFAULT_PRESSURE},
  {"fuzz_all_plan_coverage_next", SELFTEST_FUZZ_ALL_PLAN_COVERAGE_NEXT_CMD, 19, 30.0, false, SELFTEST_FUZZ_ALL_PLAN_COVERAGE_NEXT},
  {"fuzz_all_coverage_commands", SELFTEST_FUZZ_ALL_COVERAGE_COMMANDS_CMD, 15, 30.0, false, SELFTEST_FUZZ_ALL_COVERAGE_COMMANDS},
  {"fuzz_all_coverage_focus_companions", SELFTEST_FUZZ_ALL_COVERAGE_FOCUS_COMPANIONS_CMD, 15, 30.0, false, SELFTEST_FUZZ_ALL_COVERAGE_FOCUS_COMPANIONS},
  {"fuzz_all_history_commands", SELFTEST_FUZZ_ALL_HISTORY_COMMANDS_CMD, 17, 30.0, false, SELFTEST_FUZZ_ALL_HISTORY_COMMANDS},
  {"fuzz_all_preflight_isolation", SELFTEST_FUZZ_ALL_PREFLIGHT_ISOLATION_CMD, 21, 45.0, false, SELFTEST_FUZZ_ALL_PREFLIGHT_ISOLATION},
  {"fuzz_all_help", SELFTEST_FUZZ_ALL_HELP_CMD, 4, 30.0, false, SELFTEST_FUZZ_ALL_HELP},
  {"fuzz_all_progress", SELFTEST_FUZZ_ALL_PROGRESS_CMD, 11, 30.0, false, SELFTEST_FUZZ_ALL_PROGRESS},
  {"fuzz_all_progress_canonical", SELFTEST_FUZZ_ALL_PROGRESS_CANONICAL_CMD, 11, 30.0, false, SELFTEST_FUZZ_ALL_PROGRESS_CANONICAL},
  {"fuzz_all_progress_stale_evidence", SELFTEST_FUZZ_ALL_PROGRESS_STALE_EVIDENCE_CMD, 11, 30.0, false, SELFTEST_FUZZ_ALL_PROGRESS_STALE_EVIDENCE},
  {"fuzz_all_progress_refresh_fail", SELFTEST_FUZZ_ALL_PROGRESS_REFRESH_FAIL_CMD, 18, 30.0, false, SELFTEST_FUZZ_ALL_PROGRESS_REFRESH_FAIL},
  {"fuzz_all_old_paths", SELFTEST_FUZZ_ALL_OLD_PATHS_CMD, 16, 30.0, false, SELFTEST_FUZZ_ALL_OLD_PATHS},
  {"fuzz_all_old_paths_dry_run", SELFTEST_FUZZ_ALL_OLD_PATHS_DRY_RUN_CMD, 14, 30.0, false, SELFTEST_FUZZ_ALL_OLD_PATHS_DRY_RUN},
  {"fuzz_all_old_paths_empty_dry_run", SELFTEST_FUZZ_ALL_OLD_PATHS_EMPTY_DRY_RUN_CMD, 14, 30.0, false, SELFTEST_FUZZ_ALL_OLD_PATHS_EMPTY_DRY_RUN},
  {"fuzz_all_old_writer_classifier", SELFTEST_FUZZ_ALL_OLD_WRITER_CLASSIFIER_CMD, 14, 30.0, false, SELFTEST_FUZZ_ALL_OLD_WRITER_CLASSIFIER},
  {"fuzz_afl_dry_run", SELFTEST_AFL_DRY, 10, 30.0, false, SELFTEST_STANDARD_REPORT},
  {"fuzz_afl_compiler_dry_run", SELFTEST_AFL_COMPILER_DRY, 10, 30.0, false, SELFTEST_AFL_COMPILER_DRY_RUN},
  {"fuzz_sanitizers_dry_run", SELFTEST_SAN_DRY, 6, 30.0, false, SELFTEST_SANITIZER_DRY_RUN},
  {"fuzz_gc_smoke", SELFTEST_GC_SMOKE, 11, 60.0, false, SELFTEST_STANDARD_REPORT},
  {"fuzz_gc_campaign_compact", SELFTEST_GC_CAMPAIGN_COMPACT, 12, 60.0, false, SELFTEST_FUZZ_GC_CAMPAIGN_COMPACT},
  {"shapes_audit", SELFTEST_SHAPES, 4, 30.0, false, SELFTEST_SHAPE_AUDIT},
  {"synth_print", SELFTEST_SYNTH_PRINT, 4, 60.0, false, SELFTEST_SYNTH_PRINT_REPORT},
  {"synth_schedule", SELFTEST_SYNTH_SCHEDULE, 4, 60.0, false, SELFTEST_STANDARD_REPORT},
  {"synth_pure", SELFTEST_SYNTH_PURE, 4, 120.0, false, SELFTEST_STANDARD_REPORT},
  {"compiler_type", SELFTEST_COMPILER_TYPE, 6, 120.0, false, SELFTEST_STANDARD_REPORT},
  {"compiler_cache_classifier", SELFTEST_COMPILER_CACHE_CLASSIFIER, 4, 30.0, false, SELFTEST_STANDARD_REPORT},
  {"compiler_findings", SELFTEST_COMPILER_FINDINGS, 6, 60.0, false, SELFTEST_STANDARD_REPORT},
  {"compiler_known_bugs", SELFTEST_COMPILER_KNOWN_BUGS, 6, 60.0, false, SELFTEST_COMPILER_KNOWN_BUGS_REPORT},
  {"compiler_std_audit", SELFTEST_COMPILER_STD_AUDIT, 6, 30.0, false, SELFTEST_COMPILER_STD_AUDIT_REPORT},
  {"bench_compile", SELFTEST_BENCH_COMPILE, 7, 120.0, false, SELFTEST_STANDARD_REPORT},
  {"perf_triage_args", SELFTEST_PERF_TRIAGE_ARGS_CMD, 10, 120.0, false, SELFTEST_PERF_TRIAGE_ARGS_REPORT},
  {"fuzz_frontend", SELFTEST_FUZZ_FRONTEND, 6, 90.0, false, SELFTEST_STANDARD_REPORT},
  {"fuzz_snippets", SELFTEST_FUZZ_SNIPPETS, 8, 120.0, false, SELFTEST_STANDARD_REPORT},
  {"reduce_artifact", SELFTEST_REDUCE_ARTIFACT, 10, 120.0, false, SELFTEST_REDUCE_ARTIFACT_REPORT},
  {"synth_generate", SELFTEST_SYNTH_GENERATE, 19, 90.0, false, SELFTEST_STANDARD_REPORT},
  {"synth_ir", SELFTEST_SYNTH_IR, 21, 90.0, false, SELFTEST_STANDARD_REPORT},
  {"synth_stress", SELFTEST_SYNTH_STRESS, 21, 90.0, false, SELFTEST_STANDARD_REPORT},
  {"real_db_functions", SELFTEST_REAL_DB_FUNCTIONS, 9, 90.0, false, SELFTEST_STANDARD_REPORT},
  {"synth_real", SELFTEST_SYNTH_REAL, 13, 90.0, false, SELFTEST_STANDARD_REPORT},
  {"creal_build", SELFTEST_CREAL_BUILD, 15, 90.0, false, SELFTEST_STANDARD_REPORT},
  {"synth_creal", SELFTEST_SYNTH_CREAL, 14, 90.0, false, SELFTEST_STANDARD_REPORT},
  {"bench_repl_jit", SELFTEST_BENCH_REPL_JIT, 9, 180.0, true, SELFTEST_STANDARD_REPORT},
  {"perf_triage", SELFTEST_PERF_TRIAGE, 7, 180.0, true, SELFTEST_STANDARD_REPORT},
  {"real_db_hosts", SELFTEST_REAL_DB_HOSTS, 9, 180.0, true, SELFTEST_STANDARD_REPORT},
  {"prove_lab", SELFTEST_PROVE, 7, 180.0, true, SELFTEST_STANDARD_REPORT},
  {"bridge_suite", SELFTEST_BRIDGE_SUITE, 8, 120.0, true, SELFTEST_STANDARD_REPORT},
  {"bridge_unsupported_diagnostic", SELFTEST_BRIDGE_UNSUPPORTED, 4, 30.0, false, SELFTEST_UNSUPPORTED_STDOUT},
  {"bridge_generate", SELFTEST_BRIDGE_GENERATE, 17, 120.0, true, SELFTEST_STANDARD_REPORT},
  {"corpus_build", SELFTEST_CORPUS_BUILD, 15, 120.0, true, SELFTEST_STANDARD_REPORT},
  {"corpus_audit", SELFTEST_CORPUS_AUDIT, 7, 60.0, true, SELFTEST_STANDARD_REPORT},
  {"corpus_replay", SELFTEST_CORPUS_REPLAY, 10, 120.0, true, SELFTEST_STANDARD_REPORT},
  {"campaign_run", SELFTEST_CAMPAIGN_RUN, 15, 180.0, true, SELFTEST_STANDARD_REPORT},
  {"campaign_run_all_lanes", SELFTEST_CAMPAIGN_RUN_ALL, 19, 240.0, true, SELFTEST_STANDARD_REPORT},
  {"campaign_optimize", SELFTEST_CAMPAIGN_OPTIMIZE, 21, 180.0, true, SELFTEST_STANDARD_REPORT},
};

static bool selftest_wanted(const string_list_t *only, const char *name) {
  return only->count == 0 || string_list_contains(only, name);
}

static int selftest_expected_rc(const selftest_spec_t *spec) {
  if (!spec) return 0;
  if (spec->validator == SELFTEST_UNSUPPORTED_STDOUT) return 3;
  if (spec->validator == SELFTEST_FUZZ_ALL_PROGRESS_REFRESH_FAIL) return 1;
  return 0;
}

static const char *selftest_validator_name(selftest_validator_t validator) {
  switch (validator) {
    case SELFTEST_STANDARD_REPORT: return "standard";
    case SELFTEST_SHAPE_AUDIT: return "shape_audit";
    case SELFTEST_UNSUPPORTED_STDOUT: return "unsupported_stdout";
    case SELFTEST_FUZZ_REPORTING: return "fuzz_reporting";
    case SELFTEST_SANITIZER_DRY_RUN: return "sanitizer_dry_run";
    case SELFTEST_FUZZ_FRESH_HANDOFF: return "fuzz_fresh_handoff";
    case SELFTEST_PERF_TRIAGE_ARGS_REPORT: return "perf_triage_args";
    case SELFTEST_CLI_EQUALS_ARGS_REPORT: return "cli_equals_args";
    case SELFTEST_WORKER_EQUALS_ARGS_REPORT: return "worker_equals_args";
    case SELFTEST_SYNTH_PRINT_REPORT: return "synth_print";
    case SELFTEST_REDUCE_ARTIFACT_REPORT: return "reduce_artifact";
    case SELFTEST_AFL_COMPILER_DRY_RUN: return "afl_compiler_dry_run";
    case SELFTEST_FUZZ_FULL_PRESSURE_REMEDIATION: return "fuzz_full_pressure_remediation";
    case SELFTEST_FUZZ_DEFAULT_PRESSURE: return "fuzz_default_pressure";
    case SELFTEST_FUZZ_ALL_COVERAGE_COMMANDS:
      return "fuzz_all_coverage_commands";
    case SELFTEST_FUZZ_ALL_COVERAGE_FOCUS_COMPANIONS:
      return "fuzz_all_coverage_focus_companions";
    case SELFTEST_FUZZ_ALL_HISTORY_COMMANDS:
      return "fuzz_all_history_commands";
    case SELFTEST_FUZZ_ALL_PREFLIGHT_ISOLATION:
      return "fuzz_all_preflight_isolation";
    case SELFTEST_FUZZ_ALL_PLAN_COVERAGE_NEXT:
      return "fuzz_all_plan_coverage_next";
    case SELFTEST_COMPILER_KNOWN_BUGS_REPORT:
      return "compiler_known_bugs";
    case SELFTEST_COMPILER_STD_AUDIT_REPORT:
      return "compiler_std_audit";
    case SELFTEST_FUZZ_REPRO_READY_MISSING_WRAPPER: return "fuzz_repro_ready_missing_wrapper";
    case SELFTEST_FUZZ_REPRO_READY_MISSING_COMMAND: return "fuzz_repro_ready_missing_command";
    case SELFTEST_FUZZ_ALL_HELP: return "fuzz_all_help";
    case SELFTEST_FUZZ_ALL_PROGRESS: return "fuzz_all_progress";
    case SELFTEST_FUZZ_ALL_PROGRESS_CANONICAL:
      return "fuzz_all_progress_canonical";
    case SELFTEST_FUZZ_ALL_STATUS_CANONICAL:
      return "fuzz_all_status_canonical";
    case SELFTEST_FUZZ_ALL_STATUS_STALE_EVIDENCE:
      return "fuzz_all_status_stale_evidence";
    case SELFTEST_FUZZ_ALL_REPEAT_STATUS_PROGRESS:
      return "fuzz_all_repeat_status_progress";
    case SELFTEST_FUZZ_ALL_PROGRESS_STALE_EVIDENCE: return "fuzz_all_progress_stale_evidence";
    case SELFTEST_FUZZ_ALL_PROGRESS_REFRESH_FAIL: return "fuzz_all_progress_refresh_fail";
    case SELFTEST_FUZZ_ALL_OLD_PATHS: return "fuzz_all_old_paths";
    case SELFTEST_FUZZ_ALL_OLD_PATHS_DRY_RUN: return "fuzz_all_old_paths_dry_run";
    case SELFTEST_FUZZ_ALL_OLD_PATHS_EMPTY_DRY_RUN: return "fuzz_all_old_paths_empty_dry_run";
    case SELFTEST_FUZZ_ALL_OLD_WRITER_CLASSIFIER:
      return "fuzz_all_old_writer_classifier";
    case SELFTEST_FUZZ_GC_CAMPAIGN_COMPACT: return "fuzz_gc_campaign_compact";
    case SELFTEST_SELFTEST_ROW_REPORTS: return "selftest_row_reports";
    case SELFTEST_SELFTEST_SKIP_REPORTS: return "selftest_skip_reports";
    case SELFTEST_SELFTEST_CATALOG: return "selftest_catalog";
  }
  return "unknown";
}

static const char *selftest_category(const char *name) {
  if (!name) return "other";
  if (strncmp(name, "fuzz_all_", 9) == 0 ||
      strncmp(name, "fuzz_repro_", 11) == 0)
    return "fuzz-all";
  if (strncmp(name, "fuzz_gc", 7) == 0) return "gc";
  if (strncmp(name, "fuzz_", 5) == 0) return "fuzz";
  if (strncmp(name, "compiler_", 9) == 0) return "compiler";
  if (strncmp(name, "perf_", 5) == 0) return "perf";
  if (strncmp(name, "synth_", 6) == 0) return "synth";
  if (strncmp(name, "bench_", 6) == 0) return "bench";
  if (strncmp(name, "bridge_", 7) == 0) return "bridge";
  if (strncmp(name, "corpus_", 7) == 0) return "corpus";
  if (strncmp(name, "selftest_", 9) == 0 ||
      strcmp(name, "cli_equals_args") == 0 ||
      strcmp(name, "worker_equals_args") == 0 ||
      strcmp(name, "child_tmp_env") == 0 ||
      strcmp(name, "nynth_tree_clean") == 0)
    return "selftest";
  return "other";
}

static bool selftest_run_wants_catalog(int argc, char **argv) {
  if (argc < 3 || strcmp(argv[1], "selftest") != 0 ||
      strcmp(argv[2], "run") != 0)
    return false;
  for (int i = 3; i < argc; ++i) {
    if (strcmp(argv[i], "--list") == 0 ||
        strcmp(argv[i], "list") == 0 ||
        strcmp(argv[i], "catalog") == 0 ||
        strcmp(argv[i], "help") == 0 ||
        is_help_flag(argv[i]))
      return true;
  }
  return false;
}

static char *replace_token_copy(const char *s, const char *token,
                                const char *replacement) {
  if (!s || !token || !*token) return strdup(s ? s : "");
  const char *hit = strstr(s, token);
  if (!hit) return strdup(s);
  str_buf_t out = {0};
  const char *p = s;
  size_t token_len = strlen(token);
  while ((hit = strstr(p, token)) != NULL) {
    (void)sb_append_n(&out, p, (size_t)(hit - p));
    (void)sb_append(&out, replacement ? replacement : "");
    p = hit + token_len;
  }
  (void)sb_append(&out, p);
  return sb_take(&out);
}

static char *selftest_catalog_display_arg(const selftest_spec_t *spec,
                                          const char *arg) {
  char *json_path = NULL, *work_root = NULL;
  if (asprintf(&json_path, "build/fuzz/all/selftest-%s.json",
               spec && spec->name ? spec->name : "case") < 0)
    json_path = NULL;
  if (asprintf(&work_root, "build/cache/scratch/selftest-catalog/%s",
               spec && spec->name ? spec->name : "case") < 0)
    work_root = NULL;
  char *json_resolved =
      replace_token_copy(arg ? arg : "", "$JSON",
                         json_path ? json_path :
                         "build/fuzz/all/selftest-case.json");
  char *work_resolved =
      replace_token_copy(json_resolved ? json_resolved : "", "$WORK",
                         work_root ? work_root :
                         "build/cache/scratch/selftest-catalog/case");
  free(json_resolved);
  free(json_path);
  free(work_root);
  return work_resolved ? work_resolved : strdup(arg ? arg : "");
}

static char *selftest_catalog_row_json(const selftest_spec_t *spec) {
  str_buf_t row = {0};
  (void)sb_append(&row, "{\"name\":");
  (void)sb_append_json_str(&row, spec->name);
  (void)sb_append(&row, ",\"kind\":\"selftest-catalog\",\"category\":");
  (void)sb_append_json_str(&row, selftest_category(spec->name));
  (void)sb_appendf(&row,
                   ",\"slow\":%s,\"timeout_s\":%.2f,\"arg_count\":%d,"
                   "\"expected_rc\":%d,\"validator\":",
                   spec->slow ? "true" : "false", spec->timeout_s,
                   spec->arg_count, selftest_expected_rc(spec));
  (void)sb_append_json_str(&row, selftest_validator_name(spec->validator));
  (void)sb_append(&row, ",\"args\":[");
  for (int i = 0; i < spec->arg_count; ++i) {
    if (i) (void)sb_append_c(&row, ',');
    char *display_arg = selftest_catalog_display_arg(spec, spec->args[i]);
    (void)sb_append_json_str(&row, display_arg ? display_arg : "");
    free(display_arg);
     }
     (void)sb_append(&row, "],\"only_command\":");
     str_buf_t command = {0};
     (void)sb_append(&command,
                     "env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 "
                     "nice -n 10 ./build/nynth selftest run --only ");
     (void)sb_append(&command, spec->name);
  (void)sb_append(&command, " --json build/fuzz/all/selftest-");
  (void)sb_append(&command, spec->name);
  (void)sb_append(&command, ".json");
  (void)sb_append(&command, " --markdown build/fuzz/all/selftest-");
  (void)sb_append(&command, spec->name);
  (void)sb_append(&command, ".md");
  (void)sb_append_json_str(&row, command.data ? command.data : "");
  free(command.data);
  (void)sb_append(&row, ",\"engine\":\"nynth_core\"}");
  return sb_take(&row);
}

static const char *selftest_catalog_focused_command(void) {
  return NYNTH_FUZZ_ALL_SELFTEST_FOCUSED_COMMAND;
}

static const char *selftest_catalog_focused_template_command(void) {
  return NYNTH_FUZZ_ALL_SELFTEST_TEMPLATE_COMMAND;
}

static const char *selftest_catalog_focused_example_command(void) {
  return NYNTH_FUZZ_ALL_SELFTEST_FOCUSED_COMMAND;
}

static const char *selftest_catalog_full_command(void) {
  return NYNTH_FUZZ_ALL_SELFTEST_FULL_COMMAND;
}

static const char *selftest_catalog_catalog_command(void) {
  return NYNTH_FUZZ_ALL_SELFTEST_CATALOG;
}

static const char *selftest_catalog_cockpit_command(void) {
  return NYNTH_FUZZ_ALL_SELFTEST_RUN;
}

static const char *selftest_catalog_result_probe_command(void) {
  return NYNTH_FUZZ_ALL_SELFTEST_PROBE;
}

static const char *selftest_catalog_cockpit_result_probe_command(void) {
  return NYNTH_FUZZ_ALL_SELFTEST_COCKPIT_PROBE;
}

static bool write_selftest_catalog_markdown(const char *markdown_path,
                                            const selftest_spec_t *specs,
                                            int spec_count,
                                            int slow_count) {
  if (!markdown_path || !*markdown_path) return true;
  str_buf_t md = {0};
  (void)sb_append(&md, "# Nynth Selftest Catalog\n\n");
  (void)sb_appendf(&md, "- Cases: %d (%d fast, %d slow)\n",
                   spec_count, spec_count - slow_count, slow_count);
  (void)sb_append(&md, "- Focus: ");
  md_append_code(&md, selftest_catalog_focused_command());
  (void)sb_append(&md, "\n- Focus template: ");
  md_append_code(&md, selftest_catalog_focused_template_command());
     (void)sb_append(&md, "\n- Full: ");
        md_append_code(&md, selftest_catalog_full_command());
        (void)sb_append(&md, "\n- Catalog: ");
     md_append_code(&md, selftest_catalog_catalog_command());
  (void)sb_append(&md, "\n- Probe: ");
  md_append_code(&md, selftest_catalog_result_probe_command());
  (void)sb_append(&md, "\n- Cockpit: ");
  md_append_code(&md, selftest_catalog_cockpit_command());
  (void)sb_append(&md, "\n- Cockpit probe: ");
  md_append_code(&md, selftest_catalog_cockpit_result_probe_command());
  (void)sb_append(&md, "\n\n## Focused Checks\n\n");
  for (int i = 0; i < spec_count; ++i) {
    const selftest_spec_t *spec = &specs[i];
    (void)sb_append(&md, "- ");
    md_append_code(&md, spec->name);
    (void)sb_appendf(&md, ": %s; %.0fs; %s; validator ",
                     selftest_category(spec->name), spec->timeout_s,
                     spec->slow ? "slow" : "fast");
    md_append_code(&md, selftest_validator_name(spec->validator));
    (void)sb_append(&md, "\n");
  }
  bool ok = md.data && write_file_text(markdown_path, md.data);
  free(md.data);
  return ok;
}

static bool write_selftest_run_markdown(const char *markdown_path,
                                        const char *json_path,
                                        const char *artifact_root,
                                        const string_list_t *rows,
                                        const string_list_t *failures,
                                        const string_list_t *skipped_slow,
                                        int ok_count,
                                        bool full,
                                        const char *work_dir,
                                        const char *scratch_root) {
  if (!markdown_path || !*markdown_path) return true;
  int cases = rows ? rows->count : 0;
  int failure_count = failures ? failures->count : 0;
  char *json_rel = rel_path_dup(artifact_root ? artifact_root : "",
                                json_path ? json_path : "");
  char *work_rel = rel_path_dup(artifact_root ? artifact_root : "",
                                work_dir ? work_dir : "");
  char *scratch_rel = rel_path_dup(artifact_root ? artifact_root : "",
                                   scratch_root ? scratch_root : "");
  str_buf_t md = {0};
  (void)sb_append(&md, "# Nynth Selftest Run\n\n");
  (void)sb_appendf(&md, "- Executed cases: `%d`; ok `%d`; failures `%d`\n",
                   cases, ok_count, failure_count);
  (void)sb_append(&md, "- Scope: ");
  md_append_code(&md, full ? "full" : "fast");
  (void)sb_appendf(&md, "; skipped slow `%d`\n",
                   skipped_slow ? skipped_slow->count : 0);
  if (json_rel && *json_rel) {
    (void)sb_append(&md, "- JSON: ");
    md_append_code(&md, json_rel);
    (void)sb_append_c(&md, '\n');
  }
  if (work_rel && *work_rel) {
    (void)sb_append(&md, "- Work: ");
    md_append_code(&md, work_rel);
    (void)sb_append_c(&md, '\n');
  }
  if (scratch_rel && *scratch_rel) {
    (void)sb_append(&md, "- Scratch: ");
    md_append_code(&md, scratch_rel);
    (void)sb_append_c(&md, '\n');
  }
  (void)sb_append(&md, "\n## Cases\n\n");
  if (!rows || rows->count == 0) {
    (void)sb_append(&md, "- None\n");
  } else {
    for (int i = 0; i < rows->count; ++i) {
      const char *row = rows->items[i] ? rows->items[i] : "";
      char *name = json_string_or_empty(row, "name");
      bool ok = json_bool_range(row, row + strlen(row), "ok", false);
      (void)sb_append(&md, "- ");
      md_append_code(&md, name && *name ? name : "unknown");
      (void)sb_appendf(&md, ": %s\n", ok ? "ok" : "fail");
      free(name);
    }
  }
  (void)sb_append(&md, "\n## Failures\n\n");
  if (!failures || failures->count == 0) {
    (void)sb_append(&md, "- None\n");
  } else {
    for (int i = 0; i < failures->count; ++i) {
      const char *failure = failures->items[i] ? failures->items[i] : "";
      char *name = json_string_or_empty(failure, "name");
      char *error = json_string_or_empty(failure, "error");
      (void)sb_append(&md, "- ");
      md_append_code(&md, name && *name ? name : "unknown");
      if (error && *error) {
        (void)sb_append(&md, ": ");
        md_append_code(&md, error);
      }
      (void)sb_append_c(&md, '\n');
      free(name);
      free(error);
    }
  }
  if (skipped_slow && skipped_slow->count > 0) {
    (void)sb_append(&md, "\n## Skipped Slow\n\n");
    for (int i = 0; i < skipped_slow->count; ++i) {
      (void)sb_append(&md, "- ");
      md_append_code(&md, skipped_slow->items[i]);
      (void)sb_append_c(&md, '\n');
    }
  }
  bool ok = md.data && write_file_text(markdown_path, md.data);
  free(json_rel);
  free(work_rel);
  free(scratch_rel);
  free(md.data);
  return ok;
}

static int cmd_public_selftest_catalog(int argc, char **argv) {
  const char *json_path = value_after_equals(argc, argv, 3, "--json", "");
  const char *markdown_path = value_after_equals(argc, argv, 3, "--markdown", "");
  if (!markdown_path || !*markdown_path)
    markdown_path = value_after_equals(argc, argv, 3, "--md", "");
  bool probe = has_flag_after(argc, argv, 3, "--probe") ||
               has_flag_after(argc, argv, 3, "--compact");
  string_list_t rows = {0}, failures = {0};
  int slow_count = 0;
  int spec_count = (int)(sizeof(SELFTEST_SPECS) / sizeof(SELFTEST_SPECS[0]));
  for (int i = 0; i < spec_count; ++i) {
    if (SELFTEST_SPECS[i].slow) ++slow_count;
    (void)string_list_push_take(&rows,
                                selftest_catalog_row_json(&SELFTEST_SPECS[i]));
  }
  bool markdown_written = true;
  if (markdown_path && *markdown_path) {
    markdown_written = write_selftest_catalog_markdown(markdown_path,
                                                       SELFTEST_SPECS,
                                                       spec_count,
                                                       slow_count);
    if (!markdown_written)
      (void)string_list_push_take(&failures,
                                  make_worker_failure_row("selftest_catalog",
                                                          "selftest-catalog-markdown",
                                                          1, "",
                                                          "failed to write catalog markdown"));
  }
  str_buf_t extra = {0};
  (void)sb_appendf(&extra,
                   ",\"total_cases\":%d,\"fast_cases\":%d,\"slow_cases\":%d,"
                      "\"focused_command\":",
                      spec_count, spec_count - slow_count, slow_count);
     (void)sb_append_json_str(&extra, selftest_catalog_focused_command());
  (void)sb_append(&extra, ",\"focused_template_command\":");
  (void)sb_append_json_str(&extra,
                           selftest_catalog_focused_template_command());
  (void)sb_append(&extra, ",\"focused_example_command\":");
  (void)sb_append_json_str(&extra,
                           selftest_catalog_focused_example_command());
     (void)sb_append(&extra, ",\"full_command\":");
  (void)sb_append_json_str(&extra, selftest_catalog_full_command());
     (void)sb_append(&extra, ",\"catalog_command\":");
     (void)sb_append_json_str(&extra, selftest_catalog_catalog_command());
  (void)sb_append(&extra, ",\"result_probe_command\":");
  (void)sb_append_json_str(&extra, selftest_catalog_result_probe_command());
  (void)sb_append(&extra, ",\"cockpit_command\":");
  (void)sb_append_json_str(&extra, selftest_catalog_cockpit_command());
  (void)sb_append(&extra, ",\"cockpit_result_probe_command\":");
  (void)sb_append_json_str(&extra,
                           selftest_catalog_cockpit_result_probe_command());
  if (markdown_path && *markdown_path) {
    char root[4096] = {0};
    const char *artifact_root = ".";
    if (find_nynth_root(root, sizeof(root))) artifact_root = root;
    (void)sb_append(&extra, ",\"markdown\":");
    append_rel_json_str(&extra, artifact_root, markdown_path);
    (void)sb_appendf(&extra, ",\"markdown_written\":%s",
                     markdown_written ? "true" : "false");
  }
  char *report = build_native_report_json_with_top_aliases(
      &rows, &failures, "selftest-catalog", extra.data, true);
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
      (void)sb_append(&compact, ",\"cockpit_command\":");
      (void)sb_append_json_str(&compact, selftest_catalog_cockpit_command());
      (void)sb_append(&compact, "}\n");
      fputs(compact.data ? compact.data : "{}\n", stdout);
      free(compact.data);
      rc = failures.count ? 1 : 0;
    }
    free(report);
  } else if (json_path && *json_path) {
    rc = emit_native_report(report, json_path, "selftest catalog",
                            rows.count, failures.count);
  } else {
    if (report) {
      fputs(report, stdout);
      fputc('\n', stdout);
    }
    rc = failures.count ? 1 : 0;
    free(report);
  }
  free(extra.data);
  string_list_free(&rows);
  string_list_free(&failures);
  return rc;
}

static char *selftest_expand_arg(const char *arg, const char *json_path, const char *work_dir) {
  if (strcmp(arg, "$JSON") == 0) return strdup(json_path ? json_path : "");
  if (strcmp(arg, "$WORK") == 0) return strdup(work_dir ? work_dir : "");
  if (strncmp(arg, "$WORK/", 6) == 0) {
    char *out = NULL;
    (void)asprintf(&out, "%s/%s", work_dir ? work_dir : "", arg + 6);
    return out;
  }
  const char *json_marker = strstr(arg, "$JSON");
  if (json_marker) {
    const char *rep = json_path ? json_path : "";
    const char *suffix = json_marker + 5;
    size_t pre = (size_t)(json_marker - arg);
    size_t rep_len = strlen(rep);
    size_t suffix_len = strlen(suffix);
    char *out = (char *)malloc(pre + rep_len + suffix_len + 1);
    if (!out) return strdup(arg);
    memcpy(out, arg, pre);
    memcpy(out + pre, rep, rep_len);
    memcpy(out + pre + rep_len, suffix, suffix_len + 1);
    return out;
  }
  const char *work_marker = strstr(arg, "$WORK");
  if (work_marker) {
    const char *rep = work_dir ? work_dir : "";
    const char *suffix = work_marker + 5;
    size_t pre = (size_t)(work_marker - arg);
    size_t rep_len = strlen(rep);
    size_t suffix_len = strlen(suffix);
    char *out = (char *)malloc(pre + rep_len + suffix_len + 1);
    if (!out) return strdup(arg);
    memcpy(out, arg, pre);
    memcpy(out + pre, rep, rep_len);
    memcpy(out + pre + rep_len, suffix, suffix_len + 1);
    return out;
  }
  return strdup(arg);
}

static void selftest_validate_standard_report(const char *json, string_list_t *errors, int *row_count) {
  *row_count = -1;
  const char *rows = json_top_level_value_after_key(json, "rows");
  const char *failures = json_top_level_value_after_key(json, "failures");
  const char *summary = json_top_level_value_after_key(json, "summary");
  if (!rows || *rows != '[') {
    (void)string_list_push_copy(errors, "missing rows list");
  } else {
    *row_count = count_json_array_items(rows);
  }
  if (!failures || *failures != '[') {
    (void)string_list_push_copy(errors, "missing failures list");
  } else if (json_failures_nonempty(json)) {
    (void)string_list_push_copy(errors, "report failures list is nonempty");
  }
  if (!summary || *summary != '{') {
    (void)string_list_push_copy(errors, "missing summary dict");
  } else {
    bool top_ok = false;
    double summary_failures = -1.0;
    if (!json_top_level_bool_from_report(json, "ok", &top_ok) ||
        !summary_number_from_report(json, "failure_count",
                                    &summary_failures) ||
        top_ok != (summary_failures == 0.0))
      (void)string_list_push_copy(errors,
                                  "standard report top ok alias wrong");
    double top_cases = -1.0, summary_cases = -1.0;
    double top_ok_count = -1.0, summary_ok = -1.0;
    double top_failure_count = -1.0;
    if (!json_top_level_number_from_report(json, "cases", &top_cases) ||
        !summary_number_from_report(json, "cases", &summary_cases) ||
        top_cases != summary_cases)
      (void)string_list_push_copy(errors,
                                  "standard report top cases alias wrong");
    if (!json_top_level_number_from_report(json, "ok_count",
                                           &top_ok_count) ||
        !summary_number_from_report(json, "ok", &summary_ok) ||
        top_ok_count != summary_ok)
      (void)string_list_push_copy(errors,
                                  "standard report top ok_count alias wrong");
    if (!json_top_level_number_from_report(json, "failure_count",
                                           &top_failure_count) ||
        top_failure_count != summary_failures)
      (void)string_list_push_copy(
          errors, "standard report top failure_count alias wrong");
  }
}

static void selftest_validate_sanitizer_dry_run(const char *json,
                                                string_list_t *errors,
                                                int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-sanitizers") != 0)
    (void)string_list_push_copy(errors, "sanitizer report mode mismatch");
  bool dry_run = false;
  if (!summary_bool_from_report(json, "dry_run", &dry_run) || !dry_run)
    (void)string_list_push_copy(errors, "sanitizer dry-run flag missing");
  if (!strstr(json, "\"name\":\"nytrix_asan\"") ||
      !strstr(json, "\"name\":\"nytrix_ubsan\""))
    (void)string_list_push_copy(errors, "sanitizer lane names missing");
  if (!strstr(json, "\"phase\":\"sanitizer\""))
    (void)string_list_push_copy(errors, "sanitizer phase missing");
  if (!strstr(json, "\"nytrix_make\"") || !strstr(json, "/make"))
    (void)string_list_push_copy(errors, "sanitizer dry-run does not record Nytrix make");
  if (strstr(json, "/nynth/make"))
    (void)string_list_push_copy(errors, "sanitizer dry-run targets Nynth make");
  if (strstr(json, "\"asan\",\"test\"") || strstr(json, "\"ubsan\",\"test\""))
    (void)string_list_push_copy(errors, "sanitizer command still passes stale test target");
  free(mode);
}

static void selftest_validate_afl_compiler_dry_run(const char *json,
                                                   string_list_t *errors,
                                                   int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  bool qemu_requested = true;
  if (!summary_bool_from_report(json, "qemu_requested", &qemu_requested) ||
      qemu_requested)
    (void)string_list_push_copy(errors, "AFL compiler dry-run unexpectedly requested QEMU");
  if (!strstr(json, "\"target\":\"syntax\"") ||
      !strstr(json, "\"qemu_mode\":false") ||
      !strstr(json, "\"dumb_mode\":true"))
    (void)string_list_push_copy(errors, "AFL syntax dry-run did not use dumb non-QEMU mode");
  if (!strstr(json, "build/cache/scratch/afl_wrappers/syntax-run.sh"))
    (void)string_list_push_copy(errors, "AFL syntax dry-run did not use cwd/cache passthrough wrapper");
  if (!strstr(json, "\"target\":\"ny-core\"") ||
      !strstr(json, "\"normalized_compiler_exit\":true") ||
      !strstr(json, "build/cache/scratch/afl_wrappers/ny-core-normalize.sh"))
    (void)string_list_push_copy(errors, "AFL ny-core dry-run did not use normalized compiler wrapper");
  char root[4096];
  if (find_nynth_root(root, sizeof(root))) {
    char wrapper_path[4096];
    if (path_join(wrapper_path, sizeof(wrapper_path), root,
                  "build/cache/scratch/afl_wrappers/ny-core-normalize.sh")) {
      file_buf_t wrapper = {0};
      if (!read_file(wrapper_path, &wrapper) || !wrapper.data) {
        (void)string_list_push_copy(errors, "AFL ny-core wrapper script was not readable");
      } else if (!shell_text_has_repo_cache_env(wrapper.data) ||
                 !strstr(wrapper.data, "NYNTH_AFL_RAW") ||
                 !strstr(wrapper.data, "export NYNTH_ROOT=\"$nynth_root\"") ||
                 !strstr(wrapper.data, "export NYTRIX_CACHE_DIR=\"$nynth_root/build/cache/nytrix\"") ||
                 !strstr(wrapper.data, "export NYTRIX_ROOT=\"$ny_workdir\"") ||
                 !strstr(wrapper.data, "cd \"$ny_workdir\" || exit 125")) {
        (void)string_list_push_copy(errors, "AFL ny-core wrapper does not force repo-local cache and sibling Nytrix cwd");
      }
      free(wrapper.data);
    }
    if (path_join(wrapper_path, sizeof(wrapper_path), root,
                  "build/cache/scratch/afl_wrappers/syntax-run.sh")) {
      file_buf_t wrapper = {0};
      if (!read_file(wrapper_path, &wrapper) || !wrapper.data) {
        (void)string_list_push_copy(errors, "AFL syntax wrapper script was not readable");
      } else if (!shell_text_has_repo_cache_env(wrapper.data) ||
                 !strstr(wrapper.data, "export NYNTH_ROOT=\"$nynth_root\"") ||
                 !strstr(wrapper.data, "export NYTRIX_CACHE_DIR=\"$nynth_root/build/cache/nytrix\"") ||
                 !strstr(wrapper.data, "export NYTRIX_ROOT=\"$ny_workdir\"") ||
                 !strstr(wrapper.data, "cd \"$ny_workdir\" || exit 125") ||
                 !strstr(wrapper.data, "exec \"$ny_bin\" \"$@\"")) {
        (void)string_list_push_copy(errors, "AFL syntax wrapper does not force repo-local cache and sibling Nytrix cwd");
      }
      free(wrapper.data);
    }
  }
  if (strstr(json, "\"-Q\""))
    (void)string_list_push_copy(errors, "AFL compiler dry-run still enables implicit QEMU mode");
}

static void selftest_validate_fuzz_gc_campaign_compact(const char *json,
                                                       string_list_t *errors,
                                                       int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "fuzz-gc-campaign") != 0)
    (void)string_list_push_copy(errors, "GC campaign report mode mismatch");
  double failure_count = -1.0, lane_count = -1.0;
  if (!summary_number_from_report(json, "failure_count", &failure_count) ||
      failure_count != 0.0)
    (void)string_list_push_copy(errors, "GC campaign failure count was not zero");
  if (!summary_number_from_report(json, "lane_count", &lane_count) ||
      lane_count < 1.0)
    (void)string_list_push_copy(errors, "GC campaign lane count is missing");
  bool skip_sanitizers = false, skip_ny = false;
  if (!summary_bool_from_report(json, "skip_sanitizers", &skip_sanitizers) ||
      !skip_sanitizers)
    (void)string_list_push_copy(errors, "GC compact campaign did not skip sanitizers");
  if (!summary_bool_from_report(json, "skip_ny", &skip_ny) || !skip_ny)
    (void)string_list_push_copy(errors, "GC compact campaign did not skip Ny lanes");
  if (strstr(json, "\"forever_command\""))
    (void)string_list_push_copy(errors, "GC campaign report embedded duplicate forever command");

  char *forever_script = summary_string_from_report(json, "forever_script");
  char *manifest = summary_string_from_report(json, "manifest");
  if (!forever_script || !*forever_script)
    (void)string_list_push_copy(errors, "GC campaign forever script path is missing");
  if (!manifest || !*manifest)
    (void)string_list_push_copy(errors, "GC campaign manifest path is missing");

  char root[4096];
  if (!find_nynth_root(root, sizeof(root))) {
    (void)string_list_push_copy(errors, "Nynth root not found for GC campaign compact validation");
  } else {
    char script_abs[4096] = {0};
    if (forever_script && *forever_script) {
      if (path_is_absolute(forever_script)) {
        snprintf(script_abs, sizeof(script_abs), "%s", forever_script);
      } else {
        (void)path_join(script_abs, sizeof(script_abs), root, forever_script);
      }
    }
    if (!script_abs[0] || !path_exists_file(script_abs)) {
      (void)string_list_push_copy(errors, "GC campaign forever script was not written");
    } else if (!executable_path(script_abs)) {
      (void)string_list_push_copy(errors, "GC campaign forever script is not executable");
    }
    file_buf_t script_file = {0};
    if (!forever_script || !*forever_script ||
        !read_file_maybe_root(root, forever_script, &script_file) ||
        !script_file.data) {
      (void)string_list_push_copy(errors, "GC campaign forever script was not readable");
    } else {
      if (!strstr(script_file.data, "set -euo pipefail") ||
          !shell_text_has_repo_cache_env(script_file.data))
        (void)string_list_push_copy(errors, "GC campaign forever script lost repo-local cache env");
      if (!strstr(script_file.data, "fuzz gc run --profile=soak") ||
          !strstr(script_file.data, "--checkpoint-s=3600") ||
          !strstr(script_file.data, "--validate-gc"))
        (void)string_list_push_copy(errors, "GC campaign forever script lost soak handoff");
    }
    free(script_file.data);

    file_buf_t manifest_file = {0};
    if (!manifest || !*manifest ||
        !read_file_maybe_root(root, manifest, &manifest_file) ||
        !manifest_file.data) {
      (void)string_list_push_copy(errors, "GC campaign manifest was not readable");
    } else {
      if (strstr(manifest_file.data, "\"forever_command\""))
        (void)string_list_push_copy(errors, "GC campaign manifest embedded duplicate forever command");
      if (!strstr(manifest_file.data, "\"forever_script\""))
        (void)string_list_push_copy(errors, "GC campaign manifest omitted forever script path");
    }
    free(manifest_file.data);
  }
  free(mode);
  free(forever_script);
  free(manifest);
}

static void selftest_validate_selftest_row_reports(const char *json,
                                                   string_list_t *errors,
                                                   int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  double cases = -1.0, ok_count = -1.0, ok_alias = -1.0;
  double failure_count = -1.0, top_cases = -1.0, top_ok_count = -1.0;
  double top_failure_count = -1.0;
  double requested_cases = -1.0, executed_cases = -1.0;
  double skipped_count = -1.0, skipped_slow_count = -1.0;
  bool all_requested_executed = false;
  if (!summary_number_from_report(json, "cases", &cases) || cases != 2.0)
    (void)string_list_push_copy(errors, "nested selftest case count wrong");
  if (!summary_number_from_report(json, "ok", &ok_count) || ok_count != 2.0)
    (void)string_list_push_copy(errors, "nested selftest ok count wrong");
  if (!summary_number_from_report(json, "ok_count", &ok_alias) ||
      ok_alias != ok_count)
    (void)string_list_push_copy(errors, "nested selftest ok_count alias wrong");
  if (!summary_number_from_report(json, "failure_count", &failure_count) ||
      failure_count != 0.0)
    (void)string_list_push_copy(errors, "nested selftest failure count wrong");
  if (!summary_number_from_report(json, "requested_cases", &requested_cases) ||
      requested_cases != 2.0 ||
      !summary_number_from_report(json, "executed_cases", &executed_cases) ||
      executed_cases != 2.0 ||
      !summary_number_from_report(json, "skipped_count", &skipped_count) ||
      skipped_count != 0.0 ||
      !summary_number_from_report(json, "skipped_slow_count",
                                  &skipped_slow_count) ||
      skipped_slow_count != 0.0 ||
      !summary_bool_from_report(json, "all_requested_executed",
                                &all_requested_executed) ||
      !all_requested_executed)
    (void)string_list_push_copy(errors,
                                "nested selftest execution aliases wrong");
  const char *top_ok = json_top_level_value_after_key(json, "ok");
  if (!top_ok || strncmp(top_ok, "true", 4) != 0)
    (void)string_list_push_copy(errors, "nested selftest top ok alias wrong");
  const char *top_cases_json = json_top_level_value_after_key(json, "cases");
  const char *top_ok_count_json = json_top_level_value_after_key(json, "ok_count");
  const char *top_failure_count_json =
      json_top_level_value_after_key(json, "failure_count");
  char *top_cases_end = NULL, *top_ok_count_end = NULL;
  char *top_failure_count_end = NULL;
  if (!top_cases_json ||
      (top_cases = strtod(top_cases_json, &top_cases_end),
       top_cases_end == top_cases_json) ||
      top_cases != cases)
    (void)string_list_push_copy(errors, "nested selftest top cases alias wrong");
  if (!top_ok_count_json ||
      (top_ok_count = strtod(top_ok_count_json, &top_ok_count_end),
       top_ok_count_end == top_ok_count_json) ||
      top_ok_count != ok_count)
    (void)string_list_push_copy(errors,
                                "nested selftest top ok_count alias wrong");
  if (!top_failure_count_json ||
      (top_failure_count = strtod(top_failure_count_json,
                                  &top_failure_count_end),
       top_failure_count_end == top_failure_count_json) ||
      top_failure_count != failure_count)
    (void)string_list_push_copy(errors,
                                "nested selftest top failure_count alias wrong");

  const char *rows = json_top_level_value_after_key(json, "rows");
  const char *rows_end = rows && *rows == '[' ? matching_json_end(rows, '[', ']') : NULL;
  if (!rows || !rows_end) {
    (void)string_list_push_copy(errors, "nested selftest rows were not readable");
    return;
  }

  char nynth_root[4096] = {0};
  bool have_root = find_nynth_root(nynth_root, sizeof(nynth_root));
  if (!have_root)
    (void)string_list_push_copy(errors, "Nynth root not found for selftest row report validation");

  char *work_dir = summary_string_from_report(json, "work_dir");
  char *scratch_root = summary_string_from_report(json, "scratch_root");
  if (!work_dir || !*work_dir) {
    (void)string_list_push_copy(errors, "nested selftest work dir is missing");
  } else if (path_is_absolute(work_dir)) {
    (void)string_list_push_copy(errors, "nested selftest work dir was not repo-relative");
  } else if (strncmp(work_dir, "build/cache/scratch/", 20) != 0) {
    (void)string_list_push_copy(errors, "nested selftest work dir is outside repo scratch cache");
  }
  if (!scratch_root || !*scratch_root) {
    (void)string_list_push_copy(errors, "nested selftest scratch root is missing");
  } else if (path_is_absolute(scratch_root)) {
    (void)string_list_push_copy(errors, "nested selftest scratch root was not repo-relative");
  } else if (strcmp(scratch_root, "build/cache/scratch") != 0) {
    (void)string_list_push_copy(errors, "nested selftest scratch root is outside repo scratch cache");
  }

  char *markdown = summary_string_from_report(json, "markdown");
  bool markdown_written = false;
  if (!markdown || !*markdown) {
    (void)string_list_push_copy(errors, "nested selftest markdown path is missing");
  } else if (path_is_absolute(markdown)) {
    (void)string_list_push_copy(errors, "nested selftest markdown path was not repo-relative");
  } else if (strncmp(markdown, "build/cache/scratch/", 20) != 0) {
    (void)string_list_push_copy(errors, "nested selftest markdown path is outside repo scratch cache");
  }
  if (!summary_bool_from_report(json, "markdown_written", &markdown_written) ||
      !markdown_written)
    (void)string_list_push_copy(errors, "nested selftest markdown write flag missing");
  file_buf_t markdown_file = {0};
  if (have_root && markdown && *markdown) {
    if (!read_file_maybe_root(nynth_root, markdown, &markdown_file) ||
        !markdown_file.data) {
      (void)string_list_push_copy(errors, "nested selftest markdown was not readable");
    } else if (!strstr(markdown_file.data, "# Nynth Selftest Run") ||
               !strstr(markdown_file.data,
                       "Executed cases: `2`; ok `2`; failures `0`") ||
               !strstr(markdown_file.data, "## Cases") ||
               !strstr(markdown_file.data, "`fuzz_gc_campaign_compact`") ||
               !strstr(markdown_file.data, "`bridge_unsupported_diagnostic`") ||
               !strstr(markdown_file.data, "## Failures") ||
               !strstr(markdown_file.data, "- None")) {
      (void)string_list_push_copy(errors, "nested selftest markdown content is incomplete");
    }
  }
  free(markdown_file.data);

  bool saw_gc = false, saw_stdout = false;
  const char *p = rows + 1;
  while (p && p < rows_end) {
    const char *obj = strchr(p, '{');
    if (!obj || obj >= rows_end) break;
    const char *obj_end = matching_json_end(obj, '{', '}');
    if (!obj_end || obj_end > rows_end) break;
    char *name = json_extract_string_range(obj, obj_end + 1, "name");
    char *report = json_extract_string_range(obj, obj_end + 1, "report");
    bool ok = json_bool_range(obj, obj_end + 1, "ok", false);
    if (name && strcmp(name, "fuzz_gc_campaign_compact") == 0) {
      saw_gc = true;
      if (!ok)
        (void)string_list_push_copy(errors, "nested GC compact row was not ok");
      if (!report || !*report) {
        (void)string_list_push_copy(errors, "nested GC compact row omitted child report");
      } else if (path_is_absolute(report)) {
        (void)string_list_push_copy(errors, "nested GC compact report path was not repo-relative");
      } else if (strncmp(report, "build/cache/scratch/", 20) != 0) {
        (void)string_list_push_copy(errors, "nested GC compact report path is outside repo scratch cache");
      } else if (have_root) {
        file_buf_t child = {0};
        if (!read_file_maybe_root(nynth_root, report, &child) || !child.data) {
          (void)string_list_push_copy(errors, "nested GC compact child report was not readable");
        } else {
          int child_rows = -1;
          selftest_validate_fuzz_gc_campaign_compact(child.data, errors, &child_rows);
        }
        free(child.data);
      }
    } else if (name && strcmp(name, "bridge_unsupported_diagnostic") == 0) {
      saw_stdout = true;
      if (!ok)
        (void)string_list_push_copy(errors, "nested stdout-only diagnostic row was not ok");
      if (report && *report)
        (void)string_list_push_copy(errors, "stdout-only diagnostic row invented a child report path");
    }
    free(name);
    free(report);
    p = obj_end + 1;
  }
  if (!saw_gc)
    (void)string_list_push_copy(errors, "nested selftest omitted GC compact row");
  if (!saw_stdout)
    (void)string_list_push_copy(errors, "nested selftest omitted stdout-only diagnostic row");
  free(work_dir);
  free(scratch_root);
  free(markdown);
}

static void selftest_validate_selftest_skip_reports(const char *json,
                                                    string_list_t *errors,
                                                    int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  double cases = -1.0, ok_count = -1.0, failure_count = -1.0;
  double requested_cases = -1.0, executed_cases = -1.0;
  double skipped_count = -1.0, skipped_slow_count = -1.0;
  bool all_requested_executed = true;
  if (!summary_number_from_report(json, "cases", &cases) || cases != 0.0 ||
      !summary_number_from_report(json, "ok_count", &ok_count) ||
      ok_count != 0.0 ||
      !summary_number_from_report(json, "failure_count", &failure_count) ||
      failure_count != 0.0)
    (void)string_list_push_copy(errors,
                                "skipped selftest executed-count aliases wrong");
  if (!summary_number_from_report(json, "requested_cases", &requested_cases) ||
      requested_cases != 1.0 ||
      !summary_number_from_report(json, "executed_cases", &executed_cases) ||
      executed_cases != 0.0 ||
      !summary_number_from_report(json, "skipped_count", &skipped_count) ||
      skipped_count != 1.0 ||
      !summary_number_from_report(json, "skipped_slow_count",
                                  &skipped_slow_count) ||
      skipped_slow_count != 1.0 ||
      !summary_bool_from_report(json, "all_requested_executed",
                                &all_requested_executed) ||
      all_requested_executed)
    (void)string_list_push_copy(errors,
                                "skipped selftest skip aliases wrong");
  if (!strstr(json, "\"skipped_slow\":[\"perf_triage\"]"))
    (void)string_list_push_copy(errors,
                                "skipped selftest slow list missing");

  char *markdown = summary_string_from_report(json, "markdown");
  char root[4096] = {0};
  file_buf_t markdown_file = {0};
  if (!markdown || !*markdown) {
    (void)string_list_push_copy(errors,
                                "skipped selftest markdown path missing");
  } else if (!find_nynth_root(root, sizeof(root)) ||
             !read_file_maybe_root(root, markdown, &markdown_file) ||
             !markdown_file.data) {
    (void)string_list_push_copy(errors,
                                "skipped selftest markdown was not readable");
  } else if (!strstr(markdown_file.data,
                     "Executed cases: `0`; ok `0`; failures `0`") ||
             !strstr(markdown_file.data, "skipped slow `1`") ||
             !strstr(markdown_file.data, "## Skipped Slow") ||
             !strstr(markdown_file.data, "`perf_triage`")) {
    (void)string_list_push_copy(errors,
                                "skipped selftest markdown content incomplete");
  }
  free(markdown_file.data);
  free(markdown);
}

static void selftest_validate_selftest_catalog(const char *json,
                                               string_list_t *errors,
                                               int *row_count) {
  selftest_validate_standard_report(json, errors, row_count);
  char *mode = summary_string_from_report(json, "mode");
  if (!mode || strcmp(mode, "selftest-catalog") != 0)
    (void)string_list_push_copy(errors, "selftest catalog mode mismatch");
  if (*row_count < 40)
    (void)string_list_push_copy(errors, "selftest catalog row count is too small");
  double total_cases = -1.0, fast_cases = -1.0, slow_cases = -1.0;
  double summary_ok_count = -1.0;
  double failure_count = -1.0;
  if (!summary_number_from_report(json, "total_cases", &total_cases) ||
      total_cases != (double)*row_count)
    (void)string_list_push_copy(errors, "selftest catalog total case count mismatch");
  if (!summary_number_from_report(json, "fast_cases", &fast_cases) ||
      fast_cases <= 0.0)
    (void)string_list_push_copy(errors, "selftest catalog fast case count missing");
  if (!summary_number_from_report(json, "slow_cases", &slow_cases) ||
      slow_cases <= 0.0)
    (void)string_list_push_copy(errors, "selftest catalog slow case count missing");
  if (fast_cases >= 0.0 && slow_cases >= 0.0 && total_cases >= 0.0 &&
      fast_cases + slow_cases != total_cases)
    (void)string_list_push_copy(errors, "selftest catalog fast/slow counts do not add up");
  if (!summary_number_from_report(json, "ok_count", &summary_ok_count) ||
      summary_ok_count != total_cases)
    (void)string_list_push_copy(errors,
                                "selftest catalog summary ok_count wrong");
  if (!summary_number_from_report(json, "failure_count", &failure_count) ||
      failure_count != 0.0)
    (void)string_list_push_copy(errors, "selftest catalog failure count wrong");
  const char *top_ok = json_top_level_value_after_key(json, "ok");
  const char *top_cases_json = json_top_level_value_after_key(json, "cases");
  const char *top_ok_count_json = json_top_level_value_after_key(json, "ok_count");
  const char *top_failure_count_json =
      json_top_level_value_after_key(json, "failure_count");
  char *top_end = NULL;
  double top_number = -1.0;
  if (!top_ok || strncmp(top_ok, "true", 4) != 0)
    (void)string_list_push_copy(errors, "selftest catalog top ok alias wrong");
  if (!top_cases_json ||
      (top_number = strtod(top_cases_json, &top_end),
       top_end == top_cases_json || top_number != total_cases))
    (void)string_list_push_copy(errors, "selftest catalog top cases alias wrong");
  top_end = NULL;
  if (!top_ok_count_json ||
      (top_number = strtod(top_ok_count_json, &top_end),
       top_end == top_ok_count_json || top_number != total_cases))
    (void)string_list_push_copy(errors,
                                "selftest catalog top ok_count alias wrong");
  top_end = NULL;
  if (!top_failure_count_json ||
      (top_number = strtod(top_failure_count_json, &top_end),
       top_end == top_failure_count_json || top_number != 0.0))
    (void)string_list_push_copy(errors,
                                "selftest catalog top failure_count alias wrong");
  if (!strstr(json, "\"name\":\"selftest_catalog\"") ||
        !strstr(json, "\"name\":\"selftest_row_reports\"") ||
        !strstr(json, "\"name\":\"selftest_skip_reports\"") ||
        !strstr(json, "\"name\":\"fuzz_gc_campaign_compact\"") ||
        !strstr(json, "\"name\":\"fuzz_all_audit\"") ||
        !strstr(json, "\"name\":\"fuzz_all_progress\"") ||
        !strstr(json, "\"name\":\"fuzz_all_coverage_commands\"") ||
        !strstr(json, "\"name\":\"fuzz_all_history_commands\"") ||
        !strstr(json, "\"name\":\"fuzz_all_preflight_isolation\"") ||
        !strstr(json, "\"name\":\"fuzz_all_status_canonical\"") ||
        !strstr(json, "\"name\":\"fuzz_all_status_stale_evidence\"") ||
        !strstr(json, "\"name\":\"fuzz_all_repeat_status_progress\"") ||
        !strstr(json, "\"name\":\"fuzz_all_full_pressure_remediation\"") ||
      !strstr(json, "\"name\":\"fuzz_repro_ready_missing_wrapper\"") ||
      !strstr(json, "\"name\":\"fuzz_repro_ready_missing_command\"") ||
      !strstr(json, "\"name\":\"fuzz_all_plan_coverage_next\"") ||
      !strstr(json, "\"name\":\"fuzz_all_old_writer_classifier\"") ||
      !strstr(json, "\"name\":\"perf_triage_args\"") ||
      !strstr(json, "\"name\":\"compiler_findings\""))
    (void)string_list_push_copy(errors, "selftest catalog omitted critical focused checks");
  if (!strstr(json, "\"category\":\"fuzz-all\"") ||
      !strstr(json, "\"category\":\"gc\"") ||
      !strstr(json, "\"category\":\"compiler\"") ||
      !strstr(json, "\"category\":\"selftest\""))
    (void)string_list_push_copy(errors, "selftest catalog omitted useful categories");
  if (strstr(json, "$JSON") || strstr(json, "$WORK"))
    (void)string_list_push_copy(errors,
                                "selftest catalog leaked internal placeholders");
  if (!strstr(json, "--json=build/fuzz/all/selftest-cli_equals_args.json") ||
      !strstr(json,
              "build/cache/scratch/selftest-catalog/selftest_catalog/selftest_catalog.md"))
    (void)string_list_push_copy(errors,
                                "selftest catalog did not resolve example args");
  if (!strstr(json, "\"validator\":\"selftest_catalog\"") ||
      !strstr(json, "\"validator\":\"selftest_row_reports\"") ||
      !strstr(json, "\"validator\":\"selftest_skip_reports\"") ||
      !strstr(json, "\"validator\":\"fuzz_gc_campaign_compact\"") ||
      !strstr(json, "\"validator\":\"standard\"") ||
      !strstr(json, "\"name\":\"fuzz_all_audit\"") ||
      !strstr(json, "\"validator\":\"fuzz_all_status_canonical\"") ||
      !strstr(json, "\"validator\":\"fuzz_all_preflight_isolation\"") ||
      !strstr(json, "\"validator\":\"fuzz_all_status_stale_evidence\"") ||
      !strstr(json, "\"validator\":\"fuzz_all_repeat_status_progress\"") ||
      !strstr(json, "\"validator\":\"fuzz_repro_ready_missing_wrapper\"") ||
      !strstr(json, "\"validator\":\"fuzz_repro_ready_missing_command\"") ||
      !strstr(json, "\"validator\":\"fuzz_all_plan_coverage_next\"") ||
      !strstr(json, "\"validator\":\"fuzz_all_old_writer_classifier\""))
    (void)string_list_push_copy(errors, "selftest catalog validator names missing");
        if (!strstr(json, "\"only_command\":\"env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth selftest run --only fuzz_gc_campaign_compact") ||
            !strstr(json, "--markdown build/fuzz/all/selftest-fuzz_gc_campaign_compact.md") ||
            !strstr(json, "\"focused_command\":\"env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth selftest run --only fuzz_all_help") ||
            !strstr(json, "\"focused_template_command\":\"env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth selftest run --only NAME") ||
            !strstr(json, "--markdown build/fuzz/all/selftest-NAME.md") ||
            !strstr(json, "\"focused_example_command\":\"env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth selftest run --only fuzz_all_help") ||
            !strstr(json, "--markdown build/fuzz/all/selftest-fuzz_all_help.md") ||
            !strstr(json, "\"full_command\":\"env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth selftest run --full") ||
         !strstr(json, "--markdown build/fuzz/all/selftest-full.md") ||
         !strstr(json, "\"catalog_command\":\"env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth selftest run --list") ||
         !strstr(json, "\"result_probe_command\":\"env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth selftest run --list --probe --json build/fuzz/all/selftest-catalog.json --markdown build/fuzz/all/selftest-catalog.md\"") ||
         !strstr(json, "\"cockpit_command\":\"env NYNTH_LOW_PRIORITY=1 NYNTH_RUN_NICE=10 nice -n 10 ./build/nynth selftest run --only fuzz_all_help") ||
         !strstr(json, "\"cockpit_result_probe_command\":\"jq {ok,cases,ok_count,failure_count,requested_cases,executed_cases,skipped_slow_count,all_requested_executed} build/fuzz/all/selftest-cockpit.json\"") ||
      !strstr(json, "--only fuzz_all_audit") ||
      !strstr(json, "--only fuzz_all_default_pressure") ||
      !strstr(json, "--only fuzz_all_coverage_commands") ||
      !strstr(json, "--only fuzz_all_history_commands") ||
      !strstr(json, "--only fuzz_all_preflight_isolation") ||
      !strstr(json, "--only fuzz_all_repeat_status_progress") ||
      !strstr(json, "--only fuzz_all_fresh_handoff") ||
      !strstr(json, "--only fuzz_all_progress_refresh_fail") ||
        !strstr(json, "--only fuzz_all_full_pressure_remediation") ||
        !strstr(json, "--only fuzz_repro_ready_missing_wrapper") ||
        !strstr(json, "--only fuzz_repro_ready_missing_command") ||
        !strstr(json, "--only fuzz_all_plan_coverage_next") ||
          !strstr(json, "--only fuzz_all_old_paths") ||
        !strstr(json, "--only fuzz_all_old_paths_dry_run") ||
        !strstr(json, "--only fuzz_all_old_paths_empty_dry_run") ||
        !strstr(json, "--only fuzz_all_old_writer_classifier") ||
        !strstr(json, "--only compiler_findings") ||
        !strstr(json, "--only compiler_known_bugs") ||
        !strstr(json, "--only compiler_std_audit") ||
        !strstr(json, "--only perf_triage_args") ||
        !strstr(json, "--json build/fuzz/all/selftest-cockpit.json") ||
        !strstr(json, "--markdown build/fuzz/all/selftest-cockpit.md") ||
      !strstr(json, "--markdown build/fuzz/all/selftest-catalog.md"))
    (void)string_list_push_copy(errors, "selftest catalog focused commands missing");
  const char *summary_section = strstr(json, "\"summary\"");
  const char *top_focused = strstr(json, "\"focused_command\"");
  const char *top_focused_template = strstr(json, "\"focused_template_command\"");
  const char *top_focused_example = strstr(json, "\"focused_example_command\"");
  const char *top_catalog = strstr(json, "\"catalog_command\"");
  const char *top_probe = strstr(json, "\"result_probe_command\"");
  const char *top_cockpit = strstr(json, "\"cockpit_command\"");
  const char *top_cockpit_probe =
      strstr(json, "\"cockpit_result_probe_command\"");
  if (!summary_section || !top_focused || !top_focused_template ||
      !top_focused_example || !top_catalog || !top_probe ||
      !top_cockpit || !top_cockpit_probe || top_focused > summary_section ||
      top_focused_template > summary_section ||
      top_focused_example > summary_section ||
      top_catalog > summary_section || top_probe > summary_section ||
      top_cockpit > summary_section || top_cockpit_probe > summary_section)
    (void)string_list_push_copy(
        errors, "selftest catalog command aliases missing at top level");
  if (strstr(json, "/home/e/nytrix/tmp/projects/test") ||
      strstr(json, "/home/e/nytrix/fuzz") ||
      strstr(json, "/home/e/nynth/build/cache/scratch"))
    (void)string_list_push_copy(errors, "selftest catalog leaked stale absolute paths");
  char *markdown = summary_string_from_report(json, "markdown");
  bool markdown_written = false;
  if (!markdown || !*markdown)
    (void)string_list_push_copy(errors, "selftest catalog markdown path missing");
  if (!summary_bool_from_report(json, "markdown_written", &markdown_written) ||
      !markdown_written)
    (void)string_list_push_copy(errors, "selftest catalog markdown write flag missing");
  char root[4096] = {0};
  file_buf_t md = {0};
  if (!find_nynth_root(root, sizeof(root))) {
    (void)string_list_push_copy(errors, "Nynth root not found for selftest catalog markdown validation");
  } else if (!markdown || !*markdown ||
             !read_file_maybe_root(root, markdown, &md) || !md.data) {
    (void)string_list_push_copy(errors, "selftest catalog markdown was not readable");
  } else {
    if (!strstr(md.data, "# Nynth Selftest Catalog") ||
        !strstr(md.data, "Cases: ") ||
        !strstr(md.data, "## Focused Checks") ||
        !strstr(md.data, "`selftest_catalog`") ||
          !strstr(md.data, "`selftest_row_reports`") ||
          !strstr(md.data, "`selftest_skip_reports`") ||
          !strstr(md.data, "`fuzz_gc_campaign_compact`") ||
          !strstr(md.data, "`fuzz_all_audit`") ||
          !strstr(md.data, "`fuzz_all_progress`") ||
          !strstr(md.data, "`fuzz_all_coverage_commands`") ||
          !strstr(md.data, "`fuzz_all_history_commands`") ||
          !strstr(md.data, "`fuzz_all_preflight_isolation`") ||
          !strstr(md.data, "`fuzz_all_status_canonical`") ||
            !strstr(md.data, "`fuzz_all_status_stale_evidence`") ||
            !strstr(md.data, "`fuzz_all_repeat_status_progress`") ||
            !strstr(md.data, "`fuzz_repro_ready_missing_wrapper`") ||
            !strstr(md.data, "`fuzz_repro_ready_missing_command`") ||
            !strstr(md.data, "`fuzz_all_plan_coverage_next`") ||
            !strstr(md.data, "`fuzz_all_old_writer_classifier`") ||
        !strstr(md.data, "Cockpit:") ||
        !strstr(md.data, "--only fuzz_all_help") ||
        !strstr(md.data, "--only fuzz_all_audit") ||
        !strstr(md.data, "--only fuzz_all_default_pressure") ||
        !strstr(md.data, "--only fuzz_all_coverage_commands") ||
        !strstr(md.data, "--only fuzz_all_history_commands") ||
        !strstr(md.data, "--only fuzz_all_preflight_isolation") ||
        !strstr(md.data, "--only fuzz_all_repeat_status_progress") ||
        !strstr(md.data, "--only fuzz_all_fresh_handoff") ||
          !strstr(md.data, "--only fuzz_all_progress_refresh_fail") ||
          !strstr(md.data, "--only fuzz_repro_ready_missing_wrapper") ||
          !strstr(md.data, "--only fuzz_repro_ready_missing_command") ||
          !strstr(md.data, "--only fuzz_all_plan_coverage_next") ||
          !strstr(md.data, "--only fuzz_all_old_paths_empty_dry_run") ||
          !strstr(md.data, "--only fuzz_all_old_writer_classifier") ||
          !strstr(md.data, "--only compiler_findings") ||
          !strstr(md.data, "--only compiler_known_bugs") ||
          !strstr(md.data, "--only compiler_std_audit") ||
          !strstr(md.data, "--only perf_triage_args") ||
          !strstr(md.data, "--json build/fuzz/all/selftest-cockpit.json") ||
          !strstr(md.data, "--markdown build/fuzz/all/selftest-cockpit.md") ||
             !strstr(md.data, "Focus template:") ||
             !strstr(md.data, "--only fuzz_all_help --json build/fuzz/all/selftest-fuzz_all_help.json") ||
             !strstr(md.data, "Probe:") ||
          !strstr(md.data, "selftest run --list --probe --json build/fuzz/all/selftest-catalog.json --markdown build/fuzz/all/selftest-catalog.md") ||
          !strstr(md.data, "Cockpit probe:") ||
          !strstr(md.data, "jq {ok,cases,ok_count,failure_count,requested_cases,executed_cases,skipped_slow_count,all_requested_executed} build/fuzz/all/selftest-cockpit.json") ||
        !strstr(md.data, "--only NAME") ||
        !strstr(md.data, "--markdown build/fuzz/all/selftest-NAME.md") ||
        !strstr(md.data, "--full --json build/fuzz/all/selftest-full.json --markdown build/fuzz/all/selftest-full.md") ||
        !strstr(md.data, "--markdown build/fuzz/all/selftest-catalog.md"))
      (void)string_list_push_copy(errors, "selftest catalog markdown omitted focused handoff rows");
    if (strstr(md.data, "/home/e/nytrix/tmp/projects/test") ||
        strstr(md.data, "/home/e/nytrix/fuzz") ||
        strstr(md.data, "/home/e/nynth/build/cache/scratch"))
      (void)string_list_push_copy(errors, "selftest catalog markdown leaked stale absolute paths");
  }
  free(md.data);
  free(markdown);
  free(mode);
}

static void selftest_expect_top_alias_number(const char *json,
                                             const char *top_key,
                                             const char *summary_key,
                                             string_list_t *errors) {
  double top = 0.0, summary = 0.0;
  if (!json_top_level_number_from_report(json, top_key, &top) ||
      !summary_number_from_report(json, summary_key, &summary)) {
    char msg[160];
    snprintf(msg, sizeof(msg), "top alias %s missing", top_key);
    (void)string_list_push_copy(errors, msg);
    return;
  }
  double delta = top - summary;
  if (delta < 0.0) delta = -delta;
  if (delta > 0.0001) {
    char msg[160];
    snprintf(msg, sizeof(msg), "top alias %s diverged", top_key);
    (void)string_list_push_copy(errors, msg);
  }
}

static void selftest_expect_top_alias_bool(const char *json,
                                           const char *key,
                                           string_list_t *errors) {
  bool top = false, summary = false;
  if (!json_top_level_bool_from_report(json, key, &top) ||
      !summary_bool_from_report(json, key, &summary) ||
      top != summary) {
    char msg[160];
    snprintf(msg, sizeof(msg), "top alias %s missing or diverged", key);
    (void)string_list_push_copy(errors, msg);
  }
}

static void selftest_expect_top_alias_string(const char *json,
                                             const char *key,
                                             string_list_t *errors) {
  if (!json_top_level_value_after_key(json, key)) {
    char msg[160];
    snprintf(msg, sizeof(msg), "top alias %s missing", key);
    (void)string_list_push_copy(errors, msg);
    return;
  }
  char *top = json_top_level_string_from_report(json, key);
  char *summary = summary_string_from_report(json, key);
  if (!top || !summary || strcmp(top, summary) != 0) {
    char msg[160];
    snprintf(msg, sizeof(msg), "top alias %s diverged", key);
    (void)string_list_push_copy(errors, msg);
  }
  free(top);
  free(summary);
}

static void selftest_expect_top_alias_string_from(const char *json,
                                                  const char *top_key,
                                                  const char *summary_key,
                                                  string_list_t *errors) {
  if (!json_top_level_value_after_key(json, top_key)) {
    char msg[160];
    snprintf(msg, sizeof(msg), "top alias %s missing", top_key);
    (void)string_list_push_copy(errors, msg);
    return;
  }
  char *top = json_top_level_string_from_report(json, top_key);
  char *summary = summary_string_from_report(json, summary_key);
  if (!top || !summary || strcmp(top, summary) != 0) {
    char msg[160];
    snprintf(msg, sizeof(msg), "top alias %s diverged", top_key);
    (void)string_list_push_copy(errors, msg);
  }
  free(top);
  free(summary);
}

static void selftest_expect_report_result_top_aliases(const char *json,
                                                      string_list_t *errors) {
  bool report_ok = false;
  double failure_count = -1.0;
  if (!json_top_level_bool_from_report(json, "ok", &report_ok) ||
      !summary_number_from_report(json, "failure_count", &failure_count) ||
      report_ok != (failure_count == 0.0))
    (void)string_list_push_copy(errors,
                                "top alias ok missing or diverged");
  selftest_expect_top_alias_number(json, "cases", "cases", errors);
  selftest_expect_top_alias_number(json, "ok_count", "ok", errors);
  selftest_expect_top_alias_number(json, "failure_count",
                                   "failure_count", errors);
}

static void selftest_validate_fuzz_all_top_aliases(const char *json,
                                                   string_list_t *errors) {
  bool report_ok = false;
  double failure_count = -1.0;
  if (!json_top_level_bool_from_report(json, "ok", &report_ok) ||
      !summary_number_from_report(json, "failure_count", &failure_count) ||
      report_ok != (failure_count == 0.0))
    (void)string_list_push_copy(errors,
                                "top alias ok missing or diverged");
  selftest_expect_top_alias_number(json, "cases", "cases", errors);
  selftest_expect_top_alias_number(json, "ok_count", "ok", errors);
  selftest_expect_top_alias_number(json, "failure_count", "failure_count",
                                   errors);
  selftest_expect_top_alias_bool(json, "ready", errors);
  selftest_expect_top_alias_bool(json, "recommended_state_fresh", errors);
  selftest_expect_top_alias_bool(json, "recommended_state_live", errors);
  selftest_expect_top_alias_bool(json, "recommended_state_refresh_required",
                                 errors);
  selftest_expect_top_alias_bool(json, "recommended_state_dry_run_exceeds_max",
                                 errors);
  selftest_expect_top_alias_bool(json,
                                 "coverage_next_state_refresh_required",
                                 errors);
  selftest_expect_top_alias_bool(json,
                                 "coverage_next_state_dry_run_exceeds_max",
                                 errors);
  selftest_expect_top_alias_bool(json, "state_live", errors);
  selftest_expect_top_alias_bool(json, "state_fresh", errors);
  selftest_expect_top_alias_bool(json, "state_dry_run_exceeds_max", errors);
  selftest_expect_top_alias_bool(json, "latest_report_fresh", errors);
  selftest_expect_top_alias_bool(json, "latest_full_pressure_report_fresh",
                                 errors);
  selftest_expect_top_alias_bool(json, "evidence_fresh", errors);
  selftest_expect_top_alias_number(json, "latest_h",
                                   "latest_report_age_hours", errors);
  selftest_expect_top_alias_number(
      json, "latest_over_h", "latest_report_freshness_overdue_hours",
      errors);
  selftest_expect_top_alias_number(
      json, "full_h", "latest_full_pressure_report_age_hours", errors);
  selftest_expect_top_alias_number(
      json, "full_over_h",
      "latest_full_pressure_report_freshness_overdue_hours", errors);
  selftest_expect_top_alias_number(json, "over_h",
                                   "evidence_freshness_overdue_hours",
                                   errors);
  selftest_expect_top_alias_bool(json, "latest_full_pressure_raw_ok",
                                 errors);
  selftest_expect_top_alias_bool(json, "latest_full_pressure_effective_clean",
                                 errors);
  selftest_expect_top_alias_bool(
      json, "latest_full_pressure_demoted_non_reproducing_afl_timeout",
      errors);
  selftest_expect_top_alias_bool(json, "perf_watchlist_artifact_fresh",
                                 errors);
  selftest_expect_top_alias_bool(json, "old_nytrix_test_scratch_absent",
                                 errors);
  selftest_expect_top_alias_bool(json, "old_nytrix_fuzz_absent", errors);
  selftest_expect_top_alias_bool(json, "old_nytrix_build_cache_absent",
                                 errors);
  selftest_expect_top_alias_bool(json, "active_old_nytrix_output_writer_present",
                                 errors);
  selftest_expect_top_alias_bool(json, "old_path_cache_policy_ok", errors);
  selftest_expect_top_alias_number(json, "old_path_wait_remaining_seconds",
                                   "old_path_wait_remaining_seconds", errors);
  selftest_expect_top_alias_number(json, "correctness_findings",
                                   "correctness_findings", errors);
  selftest_expect_top_alias_number(json, "compiler_findings",
                                   "compiler_findings", errors);
  selftest_expect_top_alias_number(json, "known_bug_replay_findings",
                                   "known_bug_replay_findings", errors);
  selftest_expect_top_alias_number(json, "blockers", "blockers", errors);
  selftest_expect_top_alias_number(json, "blocker_count", "blocker_count",
                                   errors);
  selftest_expect_top_alias_number(json, "active_count", "active_items",
                                   errors);
  selftest_expect_top_alias_number(json, "active_items", "active_items",
                                   errors);
  selftest_expect_top_alias_number(json, "coverage_percent",
                                   "coverage_percent", errors);
  selftest_expect_top_alias_number(json, "coverage_backlog_lanes",
                                   "coverage_backlog_lanes", errors);
  selftest_expect_top_alias_number(json, "reports", "reports", errors);
  selftest_expect_top_alias_number(json, "full_pressure_reports",
                                   "full_pressure_reports", errors);
  selftest_expect_top_alias_number(json, "checked_subcases",
                                   "checked_subcases", errors);
  selftest_expect_top_alias_number(json, "full_pressure_thread_years",
                                   "full_pressure_thread_years", errors);
  selftest_expect_top_alias_number(json, "campaign_percent",
                                   "campaign_percent", errors);
  selftest_expect_top_alias_number(json, "campaign_remaining_percent",
                                   "campaign_remaining_percent", errors);
  selftest_expect_top_alias_number(json, "thread_years", "thread_years",
                                   errors);
  selftest_expect_top_alias_number(json, "target_thread_years",
                                   "target_thread_years", errors);
  selftest_expect_top_alias_number(json, "remaining_thread_years",
                                   "remaining_thread_years", errors);
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
  selftest_expect_top_alias_string(json, "thread_years_per_run_source",
                                   errors);
  selftest_expect_top_alias_number(json, "campaign_plan_wall_hours",
                                   "campaign_plan_wall_hours", errors);
  selftest_expect_top_alias_string(json, "campaign_plan_threads", errors);
  selftest_expect_top_alias_number(json, "campaign_runs_per_wall_day",
                                   "campaign_runs_per_wall_day", errors);
  selftest_expect_top_alias_number(json,
                                   "campaign_thread_years_per_wall_day",
                                   "campaign_thread_years_per_wall_day",
                                   errors);
  selftest_expect_top_alias_number(json, "campaign_percent_per_wall_day",
                                   "campaign_percent_per_wall_day", errors);
  selftest_expect_top_alias_number(json, "campaign_equivalent_wall_days",
                                   "campaign_equivalent_wall_days", errors);
  selftest_expect_top_alias_string(json, "campaign_first_report", errors);
  selftest_expect_top_alias_number(json, "campaign_first_report_epoch",
                                   "campaign_first_report_epoch", errors);
     selftest_expect_top_alias_number(json, "campaign_latest_report_epoch",
                                      "campaign_latest_report_epoch", errors);
     selftest_expect_top_alias_number(json, "campaign_calendar_span_days",
                                      "campaign_calendar_span_days", errors);
     selftest_expect_top_alias_number(json, "campaign_calendar_age_days",
                                      "campaign_calendar_age_days", errors);
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
  selftest_expect_top_alias_number(json, "stability_score",
                                   "stability_score", errors);
  selftest_expect_top_alias_string(json, "stability_label", errors);
  selftest_expect_top_alias_string(json, "stability_note", errors);
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
  selftest_expect_top_alias_number(json, "language_score_gap_percent",
                                   "language_score_gap_percent", errors);
  selftest_expect_top_alias_number(
      json, "next_run_language_score_percent",
      "next_run_language_score_percent", errors);
  selftest_expect_top_alias_number(json, "next_run_language_score",
                                   "next_run_language_score", errors);
  selftest_expect_top_alias_number(
      json, "next_run_language_score_delta_percent",
      "next_run_language_score_delta_percent", errors);
  selftest_expect_top_alias_number(json, "stability_score_percent",
                                   "stability_score_percent", errors);
  selftest_expect_top_alias_number(
      json, "next_run_stability_score_percent",
      "next_run_stability_score_percent", errors);
  selftest_expect_top_alias_number(json, "next_run_stability_delta_percent",
                                   "next_run_stability_delta_percent", errors);
  selftest_expect_top_alias_number(json, "runs_to_good_stability",
                                   "runs_to_good_stability", errors);
  selftest_expect_top_alias_number(json, "runs_to_good_stability_days",
                                   "runs_to_good_stability_days", errors);
  selftest_expect_top_alias_number(json, "days_to_good_stability",
                                   "days_to_good_stability", errors);
  selftest_expect_top_alias_number(json, "runs_to_good_language_score",
                                   "runs_to_good_language_score", errors);
  selftest_expect_top_alias_number(json, "runs_to_good_language_days",
                                   "runs_to_good_language_days", errors);
  selftest_expect_top_alias_number(json, "runs_to_good_days",
                                   "runs_to_good_days", errors);
  selftest_expect_top_alias_number(json, "days_to_good_language_score",
                                   "days_to_good_language_score", errors);
  selftest_expect_top_alias_number(json, "recommended_repeat_count",
                                   "recommended_repeat_count", errors);
  selftest_expect_top_alias_number(json, "recommended_state_age_seconds",
                                   "recommended_state_age_seconds", errors);
  selftest_expect_top_alias_number(
      json, "recommended_state_stale_after_seconds",
      "recommended_state_stale_after_seconds", errors);
  selftest_expect_top_alias_string(json, "recommended_state_stale_reason",
                                   errors);
  selftest_expect_top_alias_number(json, "recommended_state_dry_run_wall_hours",
                                   "recommended_state_dry_run_wall_hours",
                                   errors);
  selftest_expect_top_alias_number(json, "recommended_state_dry_run_wall_days",
                                   "recommended_state_dry_run_wall_days",
                                   errors);
  selftest_expect_top_alias_number(
      json, "recommended_state_dry_run_thread_years",
      "recommended_state_dry_run_thread_years", errors);
  selftest_expect_top_alias_number(
      json, "recommended_state_dry_run_campaign_gain_percent",
      "recommended_state_dry_run_campaign_gain_percent", errors);
  selftest_expect_top_alias_number(
      json, "recommended_state_dry_run_target_percent_per_run",
      "recommended_state_dry_run_target_percent_per_run", errors);
  selftest_expect_top_alias_number(
      json, "recommended_state_dry_run_thread_years_per_run",
      "recommended_state_dry_run_thread_years_per_run", errors);
  selftest_expect_top_alias_string(json, "recommended_state_handoff_threads",
                                   errors);
  selftest_expect_top_alias_number(json, "state_age_seconds",
                                   "state_age_seconds", errors);
  selftest_expect_top_alias_number(json, "state_stale_after_seconds",
                                   "state_stale_after_seconds", errors);
  selftest_expect_top_alias_number(json, "state_dry_run_wall_hours",
                                   "state_dry_run_wall_hours", errors);
  selftest_expect_top_alias_number(json, "state_dry_run_wall_days",
                                   "state_dry_run_wall_days", errors);
  selftest_expect_top_alias_number(json, "state_dry_run_thread_years",
                                   "state_dry_run_thread_years", errors);
  selftest_expect_top_alias_number(
      json, "state_dry_run_campaign_gain_percent",
      "state_dry_run_campaign_gain_percent", errors);
  selftest_expect_top_alias_number(
      json, "state_dry_run_target_percent_per_run",
      "state_dry_run_target_percent_per_run", errors);
  selftest_expect_top_alias_number(
      json, "state_dry_run_thread_years_per_run",
      "state_dry_run_thread_years_per_run", errors);
  selftest_expect_top_alias_string(json, "state_handoff_threads", errors);
  selftest_expect_top_alias_number(
      json, "coverage_next_state_age_seconds",
      "coverage_next_state_age_seconds", errors);
  selftest_expect_top_alias_number(
      json, "coverage_next_state_stale_after_seconds",
      "coverage_next_state_stale_after_seconds", errors);
  selftest_expect_top_alias_bool(json, "coverage_next_state_readable",
                                 errors);
  selftest_expect_top_alias_bool(json, "coverage_next_state_fresh", errors);
  selftest_expect_top_alias_bool(json, "coverage_next_state_live", errors);
  selftest_expect_top_alias_string(json, "coverage_next_state", errors);
  selftest_expect_top_alias_string(json, "coverage_next_state_phase", errors);
  selftest_expect_top_alias_string(json, "coverage_next_state_event", errors);
  selftest_expect_top_alias_string(json, "coverage_next_state_stale_reason",
                                   errors);
  selftest_expect_top_alias_string(json, "coverage_next_state_child_status",
                                   errors);
  selftest_expect_top_alias_number(
      json, "coverage_next_state_dry_run_wall_hours",
      "coverage_next_state_dry_run_wall_hours", errors);
  selftest_expect_top_alias_number(
      json, "coverage_next_state_dry_run_thread_years",
      "coverage_next_state_dry_run_thread_years", errors);
  selftest_expect_top_alias_string(json, "coverage_next_state_handoff_threads",
                                   errors);
  selftest_expect_top_alias_number(json, "latest_report_age_hours",
                                   "latest_report_age_hours", errors);
     selftest_expect_top_alias_number(json, "latest_report_stale_after_hours",
                                      "latest_report_stale_after_hours", errors);
     selftest_expect_top_alias_number(
         json, "latest_report_freshness_remaining_hours",
         "latest_report_freshness_remaining_hours", errors);
     selftest_expect_top_alias_number(
         json, "latest_report_freshness_overdue_hours",
         "latest_report_freshness_overdue_hours", errors);
     selftest_expect_top_alias_number(json, "latest_full_pressure_report_age_hours",
                                      "latest_full_pressure_report_age_hours",
                                      errors);
  selftest_expect_top_alias_number(json, "latest_full_pressure_failure_count",
                                   "latest_full_pressure_failure_count",
                                   errors);
     selftest_expect_top_alias_number(
         json, "latest_full_pressure_report_stale_after_hours",
         "latest_full_pressure_report_stale_after_hours", errors);
     selftest_expect_top_alias_number(
         json, "latest_full_pressure_report_freshness_remaining_hours",
         "latest_full_pressure_report_freshness_remaining_hours", errors);
     selftest_expect_top_alias_number(
         json, "latest_full_pressure_report_freshness_overdue_hours",
         "latest_full_pressure_report_freshness_overdue_hours", errors);
     selftest_expect_top_alias_number(json, "evidence_freshness_overdue_hours",
                                      "evidence_freshness_overdue_hours",
                                      errors);
     selftest_expect_top_alias_number(json, "freshness_penalty",
                                      "freshness_penalty", errors);
  selftest_expect_top_alias_number(json, "current_advisory_timeouts",
                                   "current_advisory_timeouts", errors);
  selftest_expect_top_alias_number(json, "effective_advisory_timeouts",
                                   "effective_advisory_timeouts", errors);
  selftest_expect_top_alias_number(json, "advisory_effective_timeouts",
                                   "advisory_effective_timeouts", errors);
  selftest_expect_top_alias_string(json, "advisory_penalty_state", errors);
  selftest_expect_top_alias_number(
      json, "historical_non_reproducing_afl_timeouts",
      "historical_non_reproducing_afl_timeouts", errors);
  selftest_expect_top_alias_number(json, "advisory_recheck_raw_repro_checked",
                                   "advisory_recheck_raw_repro_checked",
                                   errors);
  selftest_expect_top_alias_number(json, "advisory_recheck_raw_repro_passed",
                                   "advisory_recheck_raw_repro_passed",
                                   errors);
  selftest_expect_top_alias_number(json, "advisory_recheck_raw_repro_timeouts",
                                   "advisory_recheck_raw_repro_timeouts",
                                   errors);
  selftest_expect_top_alias_number(
      json, "advisory_recheck_raw_repro_unexpected",
      "advisory_recheck_raw_repro_unexpected", errors);
  selftest_expect_top_alias_number(json, "advisory_penalty",
                                   "advisory_penalty", errors);
  selftest_expect_top_alias_number(json, "old_path_present_count",
                                   "old_path_present_count", errors);
  selftest_expect_top_alias_number(json, "old_path_moved_count",
                                   "old_path_moved_count", errors);
  selftest_expect_top_alias_number(json, "old_path_remaining_count",
                                   "old_path_remaining_count", errors);
  selftest_expect_top_alias_number(json, "old_path_artifact_leak_count",
                                   "old_path_artifact_leak_count", errors);
  selftest_expect_top_alias_number(json, "old_path_artifact_moved_count",
                                   "old_path_artifact_moved_count", errors);
  selftest_expect_top_alias_number(json,
                                   "old_path_artifact_remaining_count",
                                   "old_path_artifact_remaining_count",
                                   errors);
  selftest_expect_top_alias_number(json, "perf_watchlist_open",
                                   "perf_watchlist_open", errors);
  selftest_expect_top_alias_number(json, "perf_hotspots_open",
                                   "perf_hotspots_open", errors);
  selftest_expect_top_alias_number(json, "perf_watchlist_artifact_age_seconds",
                                   "perf_watchlist_artifact_age_seconds",
                                   errors);
  selftest_expect_top_alias_number(
      json, "perf_watchlist_artifact_stale_after_hours",
      "perf_watchlist_artifact_stale_after_hours", errors);
  selftest_expect_top_alias_number(json, "perf_watchlist_threshold_ratio",
                                   "perf_watchlist_threshold_ratio", errors);
  selftest_expect_top_alias_number(json, "perf_watchlist_artifact_hotspots",
                                   "perf_watchlist_artifact_hotspots",
                                   errors);
  selftest_expect_top_alias_number(json, "perf_watchlist_artifact_max_ratio",
                                   "perf_watchlist_artifact_max_ratio",
                                   errors);
  selftest_expect_top_alias_number(
      json, "perf_watchlist_artifact_max_slowdown_percent",
      "perf_watchlist_artifact_max_slowdown_percent", errors);
  selftest_expect_top_alias_number(json, "optimization_ratio",
                                   "optimization_ratio", errors);
  selftest_expect_top_alias_number(json, "optimization_slowdown_percent",
                                   "optimization_slowdown_percent", errors);
  selftest_expect_top_alias_number(json, "perf_worst_ratio",
                                   "perf_worst_ratio", errors);
  selftest_expect_top_alias_number(json, "perf_worst_slowdown_percent",
                                   "perf_worst_slowdown_percent", errors);
  selftest_expect_top_alias_number(json,
                                   "latest_full_pressure_perf_hotspots",
                                   "latest_full_pressure_perf_hotspots",
                                   errors);
  selftest_expect_top_alias_number(json,
                                   "latest_full_pressure_perf_max_ratio",
                                   "latest_full_pressure_perf_max_ratio",
                                   errors);
  selftest_expect_top_alias_number(
      json, "latest_full_pressure_perf_max_slowdown_percent",
      "latest_full_pressure_perf_max_slowdown_percent", errors);
  selftest_expect_top_alias_number(json,
                                   "latest_full_pressure_perf_rows",
                                   "latest_full_pressure_perf_rows",
                                   errors);
  selftest_expect_top_alias_bool(json,
                                 "latest_full_pressure_perf_suite_current",
                                 errors);
  selftest_expect_top_alias_number(json, "runtime_export_coverage_percent",
                                   "runtime_export_coverage_percent", errors);
  selftest_expect_top_alias_number(json, "runtime_unreferenced_count",
                                   "runtime_unreferenced_count", errors);
  selftest_expect_top_alias_number(json, "runtime_wrapper_gap_count",
                                   "runtime_wrapper_gap_count", errors);
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
  selftest_expect_top_alias_number(json,
                                   "crt_top_unreferenced_family_count",
                                   "crt_top_unreferenced_family_count",
                                   errors);
  selftest_expect_top_alias_number(json, "crt_next_unreferenced_count",
                                   "crt_next_unreferenced_count", errors);
  selftest_expect_top_alias_string(json, "coverage_state", errors);
  selftest_expect_top_alias_string(json, "language_score_label", errors);
  selftest_expect_top_alias_string(json, "language_score_note", errors);
  selftest_expect_top_alias_string(json, "completion_state", errors);
  selftest_expect_top_alias_string(json, "completion_reason", errors);
  selftest_expect_top_alias_string(json, "latest_report", errors);
  selftest_expect_top_alias_string(json, "latest_full_pressure_report",
                                   errors);
  selftest_expect_top_alias_string(json, "next_script", errors);
  selftest_expect_top_alias_string(json, "next_command", errors);
  selftest_expect_top_alias_string(json, "preview_command", errors);
  selftest_expect_top_alias_string(json, "run_next_command", errors);
  selftest_expect_top_alias_string(json, "run_next_preview_command", errors);
  selftest_expect_top_alias_string(json, "run_next_low_cpu_command", errors);
  selftest_expect_top_alias_string(json, "run_next_gentle_command", errors);
  selftest_expect_top_alias_string(json, "run_next_gentle_preview_command",
                                   errors);
  selftest_expect_top_alias_string(json, "stop_file", errors);
  selftest_expect_top_alias_string(json, "stop_command", errors);
  selftest_expect_top_alias_string(json, "resume_command", errors);
  selftest_expect_top_alias_string(json, "progress_command", errors);
  selftest_expect_top_alias_string(json, "status_command", errors);
  selftest_expect_top_alias_string(json, "old_path_probe_command", errors);
  selftest_expect_top_alias_string(json, "old_path_command", errors);
  selftest_expect_top_alias_string(json, "old_path_dry_run_command", errors);
  selftest_expect_top_alias_string(json, "old_path_apply_command", errors);
  selftest_expect_top_alias_string(json, "old_path_next_action", errors);
  selftest_expect_top_alias_string(json, "old_path_next_reason", errors);
  selftest_expect_top_alias_string(json, "old_path_report", errors);
  selftest_expect_top_alias_string(json, "old_path_markdown", errors);
  selftest_expect_top_alias_string(json, "advisory_state", errors);
  selftest_expect_top_alias_string(json, "advisory_recheck_state", errors);
  selftest_expect_top_alias_string(json, "latest_full_pressure_clean_reason",
                                   errors);
  selftest_expect_top_alias_string(json, "recommended_action", errors);
  selftest_expect_top_alias_string(json, "recommended_reason", errors);
  selftest_expect_top_alias_string(json, "recommended_repeat_mode", errors);
  selftest_expect_top_alias_string(json, "recommended_command", errors);
  selftest_expect_top_alias_string(json, "recommended_low_cpu_command",
                                   errors);
  selftest_expect_top_alias_string(json, "recommended_preview_command",
                                   errors);
  selftest_expect_top_alias_string(json, "coverage_next_action", errors);
  selftest_expect_top_alias_string(json, "coverage_next_category", errors);
  selftest_expect_top_alias_string(json, "coverage_next_severity", errors);
  selftest_expect_top_alias_string(json, "coverage_next_lane", errors);
  selftest_expect_top_alias_string(json, "coverage_next_reason", errors);
  selftest_expect_top_alias_string(json, "coverage_next_command", errors);
  selftest_expect_top_alias_string(json, "coverage_next_guarded_command",
                                   errors);
  selftest_expect_top_alias_string(json, "coverage_next_low_cpu_command",
                                   errors);
  selftest_expect_top_alias_string(json, "coverage_next_preview_command",
                                   errors);
  selftest_expect_top_alias_string(json, "coverage_next_state_file", errors);
  selftest_expect_top_alias_string(json, "coverage_next_state_command",
                                   errors);
  selftest_expect_top_alias_string(json,
                                   "coverage_next_state_refresh_command",
                                   errors);
  selftest_expect_top_alias_string(json,
                                   "coverage_next_state_refresh_reason",
                                   errors);
  selftest_expect_top_alias_string(json, "coverage_next_stop_file", errors);
  selftest_expect_top_alias_string(json, "coverage_next_stop_command", errors);
  selftest_expect_top_alias_string(json, "coverage_next_resume_command",
                                   errors);
  selftest_expect_top_alias_string(json, "recommended_state", errors);
  selftest_expect_top_alias_string(json, "recommended_state_child_status",
                                   errors);
  selftest_expect_top_alias_string(json, "recommended_state_source", errors);
  selftest_expect_top_alias_string(json, "recommended_state_file", errors);
  selftest_expect_top_alias_string(json, "recommended_state_command", errors);
  selftest_expect_top_alias_string(json,
                                   "recommended_state_refresh_command",
                                   errors);
  selftest_expect_top_alias_string(json,
                                   "recommended_state_refresh_reason",
                                   errors);
  selftest_expect_top_alias_string(json,
                                   "recommended_state_canonical_status_report",
                                   errors);
  selftest_expect_top_alias_string(json,
                                   "recommended_state_canonical_progress_report",
                                   errors);
     selftest_expect_top_alias_string(json, "freshness_action_command", errors);
  selftest_expect_top_alias_string(json, "latest_report_freshness_command",
                                   errors);
  selftest_expect_top_alias_string(
      json, "latest_full_pressure_report_freshness_command", errors);
  selftest_expect_top_alias_string(json, "full_pressure_freshen_command",
                                   errors);
  selftest_expect_top_alias_string(json, "full_pressure_remediation_command",
                                   errors);
  selftest_expect_top_alias_string(json, "full_pressure_action_command",
                                   errors);
     selftest_expect_top_alias_string(json, "state_file", errors);
  selftest_expect_top_alias_string(json, "state_command", errors);
  selftest_expect_top_alias_string(json, "state_refresh_command", errors);
  selftest_expect_top_alias_string(json, "state", errors);
  selftest_expect_top_alias_string(json, "state_phase", errors);
  selftest_expect_top_alias_string(json, "state_event", errors);
  selftest_expect_top_alias_string(json, "state_child_status", errors);
  selftest_expect_top_alias_string(json, "state_stale_reason", errors);
  selftest_expect_top_alias_string(json, "state_canonical_status_report",
                                   errors);
  selftest_expect_top_alias_string(json, "state_canonical_progress_report",
                                   errors);
  selftest_expect_top_alias_string(
      json, "coverage_next_state_canonical_status_report", errors);
  selftest_expect_top_alias_string(
      json, "coverage_next_state_canonical_progress_report", errors);
  selftest_expect_top_alias_string(json, "perf_watchlist_state", errors);
  selftest_expect_top_alias_string(json, "perf_watchlist_command", errors);
  selftest_expect_top_alias_string(json, "perf_watchlist_report", errors);
  selftest_expect_top_alias_string(json, "perf_watchlist_markdown", errors);
  selftest_expect_top_alias_string(json, "perf_watchlist_action", errors);
  selftest_expect_top_alias_string(json, "perf_watchlist_action_command",
                                   errors);
  selftest_expect_top_alias_string(json, "optimization_action", errors);
  selftest_expect_top_alias_string(json, "optimization_reason", errors);
  selftest_expect_top_alias_string(json, "optimization_command", errors);
  selftest_expect_top_alias_string(json, "optimization_target_command",
                                   errors);
  selftest_expect_top_alias_string(json, "optimization_case", errors);
  selftest_expect_top_alias_string(json, "optimization_artifact", errors);
  selftest_expect_top_alias_string(json, "optimization_ny_source", errors);
  selftest_expect_top_alias_string(json, "optimization_c_source", errors);
  selftest_expect_top_alias_string(json, "perf_watchlist_artifact_max_case",
                                   errors);
  selftest_expect_top_alias_string(json,
                                   "perf_watchlist_artifact_max_artifact",
                                   errors);
  selftest_expect_top_alias_string(
      json, "perf_watchlist_artifact_max_ny_source", errors);
  selftest_expect_top_alias_string(
      json, "perf_watchlist_artifact_max_c_source", errors);
  selftest_expect_top_alias_string(json, "perf_worst_case", errors);
  selftest_expect_top_alias_string(json,
                                   "latest_full_pressure_perf_max_case",
                                   errors);
  selftest_expect_top_alias_string(json, "runtime_surface_state", errors);
  selftest_expect_top_alias_string(json, "runtime_surface_scope", errors);
  selftest_expect_top_alias_string(json, "crt_surface_state", errors);
  selftest_expect_top_alias_string(json, "crt_surface_scope", errors);
  selftest_expect_top_alias_string(json, "crt_behavior_state", errors);
  selftest_expect_top_alias_string(json, "crt_behavior_scope", errors);
  selftest_expect_top_alias_string(json, "crt_behavior_next_action", errors);
  selftest_expect_top_alias_string(json, "crt_behavior_next_reason", errors);
  selftest_expect_top_alias_string(json, "crt_behavior_next_command", errors);
  selftest_expect_top_alias_string(json, "crt_top_unreferenced_family",
                                   errors);
  selftest_expect_top_alias_string(json, "crt_next_action", errors);
  selftest_expect_top_alias_string(json, "crt_next_unreferenced_family",
                                   errors);
}

