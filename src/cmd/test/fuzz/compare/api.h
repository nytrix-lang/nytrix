#ifndef NY_CMD_TEST_FUZZ_COMPARE_API_H
#define NY_CMD_TEST_FUZZ_COMPARE_API_H

typedef struct {
  char **items;
  int count;
  int cap;
} string_list_t;

static bool find_nytrix_root(char *out, size_t out_sz);
static const char *skip_ws_const(const char *p);
static const char *value_after_equals(int argc, char **argv, int start,
                                      const char *flag, const char *fallback);
static bool has_flag_after(int argc, char **argv, int start, const char *flag);
static bool string_list_push_take(string_list_t *list, char *item);
static bool string_list_push_copy(string_list_t *list, const char *item);
static bool string_list_push_unique_copy(string_list_t *list, const char *item);
static bool string_list_contains(const string_list_t *list, const char *item);
static void string_list_free(string_list_t *list);
static char *make_fuzz_failure(const char *root, const char *target,
                               const char *error, const char *path);
static bool extract_json_number(const char *json, const char *key, double *out);
static char *json_extract_string_range(const char *start, const char *end,
                                       const char *key);
static bool json_bool_range(const char *start, const char *end, const char *key,
                            bool fallback);
static bool json_number_range(const char *start, const char *end,
                              const char *key, double *out);
static const char *json_top_level_value_after_key(const char *json,
                                                  const char *key);
static const char *matching_json_end(const char *open, char lhs, char rhs);
static bool path_exists_maybe_root(const char *root, const char *path);
static bool read_file_maybe_root(const char *root, const char *path,
                                 file_buf_t *out);
static bool summarize_known_bug_report(const char *json, int *known_bug_count,
                                       int *reproduced, int *fixed_candidates,
                                       int *lost_signal,
                                       int *baseline_failures);
static bool summarize_perf_triage_report(const char *json, int *hotspots,
                                         double *max_ratio, char *max_case,
                                         size_t max_case_sz);
static bool summary_number_from_report(const char *json, const char *key,
                                       double *out);
static char *make_fuzz_all_audit_lane_row(
    const char *root, const char *name, const char *phase, bool ok,
    bool required, bool skipped, double elapsed_ms, double sub_rows,
    double sub_failures, double sub_quarantined_known_bugs,
    double sub_finding_count, double sub_finding_live,
    double sub_finding_cleared, double sub_finding_missing,
    double sub_known_bug_count, double sub_known_bug_reproduced,
    double sub_known_bug_fixed_candidates, double sub_known_bug_lost_signal,
    double sub_known_bug_baseline_failures, double sub_perf_hotspots,
    double sub_perf_max_ratio, const char *sub_perf_max_case,
    const char *report, bool report_exists, bool logs_ok);
static void selftest_validate_standard_report(const char *json,
                                              string_list_t *errors,
                                              int *row_count);
static bool json_top_level_bool_from_report(const char *json, const char *key,
                                            bool *out);
static void selftest_expect_top_alias_number(const char *json,
                                             const char *top_key,
                                             const char *summary_key,
                                             string_list_t *errors);
static void selftest_expect_top_alias_string(const char *json, const char *key,
                                             string_list_t *errors);
static void selftest_expect_top_alias_string_from(const char *json,
                                                  const char *top_key,
                                                  const char *summary_key,
                                                  string_list_t *errors);

#endif
