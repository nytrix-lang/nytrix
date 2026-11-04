/*
 * Compare saved ny-test benchmark reports. Keep this in the native test tool
 * so report comparison does not need a scripting runtime.
 */
#define _POSIX_C_SOURCE 200809L
#include "cmd/test/perf/compare.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *begin;
  const char *end;
} JsonSpan;

typedef struct {
  char name[256], backend[128], engine[128], cache[128];
  char status[128], checksum[256], ny_checksum[256], llvm_checksum[256];
  char c_checksum[256];
  double metrics[7];
  bool metric_seen[7];
  bool checksum_ok;
} BenchRow;

typedef struct {
  BenchRow *items;
  size_t len, cap;
} BenchRows;

typedef struct {
  const char *field;
  const char *label;
  const char *seen_field;
} Metric;

static const Metric metrics[] = {
    {"ny_native_ms", "NYR runtime ms", "ny_static_metrics_seen"},
    {"ny_compile_ms", "NYR compile ms", "ny_static_metrics_seen"},
    {"ny_total_ms", "NYR total ms", "ny_static_metrics_seen"},
    {"ny_code_bytes", "NYR code bytes", "ny_code_bytes_seen"},
    {"ny_spills", "NYR spills", "ny_static_metrics_seen"},
    {"ny_reloads", "NYR reloads", "ny_static_metrics_seen"},
    {"ny_peak_compiler_rss_kb", "NYR peak RSS KiB",
     "ny_peak_compiler_rss_seen"},
};

static const char *const config_keys[] = {
    "os", "arch", "cpu_model", "target", "target_features", "c_compiler",
    "c_compiler_version", "c_flags", "nyc_compiler_version", "nyc_flags",
    "opt_level", "profile", "tier", "engine", "cache", "fixture_workers",
    "route_workers", "measurement_mode", "timing_source_scope",
    "runtime_counters_enabled", "samples", "warmups"};

static const char *skip_ws(const char *p, const char *end) {
  while (p < end && isspace((unsigned char)*p))
    ++p;
  return p;
}

static const char *skip_string(const char *p, const char *end) {
  if (p >= end || *p != '"')
    return NULL;
  for (++p; p < end; ++p) {
    if (*p == '\\') {
      if (++p >= end)
        return NULL;
    } else if (*p == '"')
      return p + 1;
  }
  return NULL;
}

static const char *skip_value(const char *p, const char *end, unsigned depth) {
  p = skip_ws(p, end);
  if (p >= end || depth > 128)
    return NULL;
  if (*p == '"')
    return skip_string(p, end);
  if (*p == '{' || *p == '[') {
    char close = *p == '{' ? '}' : ']';
    ++p;
    p = skip_ws(p, end);
    if (p < end && *p == close)
      return p + 1;
    while (p < end) {
      if (*p == '{' && close == '}') {
        p = skip_value(p, end, depth + 1);
      } else if (*p == '[' && close == ']') {
        p = skip_value(p, end, depth + 1);
      } else if (close == '}' && *p == '"') {
        p = skip_string(p, end);
        if (!p)
          return NULL;
        p = skip_ws(p, end);
        if (p >= end || *p++ != ':')
          return NULL;
        p = skip_value(p, end, depth + 1);
      } else {
        p = skip_value(p, end, depth + 1);
      }
      if (!p)
        return NULL;
      p = skip_ws(p, end);
      if (p < end && *p == close)
        return p + 1;
      if (p >= end || *p++ != ',')
        return NULL;
      p = skip_ws(p, end);
    }
    return NULL;
  }
  while (p < end && !isspace((unsigned char)*p) && *p != ',' && *p != ']' &&
         *p != '}')
    ++p;
  return p;
}

static bool decode_string(JsonSpan span, char *out, size_t cap) {
  if (!span.begin || span.begin >= span.end || *span.begin != '"' || cap == 0)
    return false;
  size_t n = 0;
  const char *p = span.begin + 1;
  while (p < span.end && *p != '"') {
    unsigned char c = (unsigned char)*p++;
    if (c == '\\') {
      if (p >= span.end)
        return false;
      c = (unsigned char)*p++;
      switch (c) {
      case '"': case '\\': case '/': break;
      case 'b': c = '\b'; break;
      case 'f': c = '\f'; break;
      case 'n': c = '\n'; break;
      case 'r': c = '\r'; break;
      case 't': c = '\t'; break;
      case 'u':
        if (span.end - p < 4)
          return false;
        p += 4;
        c = '?';
        break;
      default: return false;
      }
    }
    if (n + 1 >= cap)
      return false;
    out[n++] = (char)c;
  }
  if (p >= span.end || *p != '"')
    return false;
  out[n] = '\0';
  return true;
}

static bool key_equal(JsonSpan key, const char *wanted) {
  char text[256];
  return decode_string(key, text, sizeof(text)) && strcmp(text, wanted) == 0;
}

static JsonSpan object_field(JsonSpan object, const char *wanted) {
  JsonSpan missing = {0};
  if (!object.begin || object.begin >= object.end || *object.begin != '{')
    return missing;
  const char *p = skip_ws(object.begin + 1, object.end);
  while (p < object.end && *p != '}') {
    const char *key_end = skip_string(p, object.end);
    if (!key_end)
      return missing;
    JsonSpan key = {p, key_end};
    p = skip_ws(key_end, object.end);
    if (p >= object.end || *p++ != ':')
      return missing;
    p = skip_ws(p, object.end);
    const char *value_end = skip_value(p, object.end, 0);
    if (!value_end)
      return missing;
    if (key_equal(key, wanted))
      return (JsonSpan){p, value_end};
    p = skip_ws(value_end, object.end);
    if (p < object.end && *p == ',')
      p = skip_ws(p + 1, object.end);
    else
      break;
  }
  return missing;
}

static bool span_string(JsonSpan span, char *out, size_t cap) {
  return decode_string(span, out, cap);
}

static bool span_bool(JsonSpan span, bool *out) {
  if (!span.begin)
    return false;
  if ((size_t)(span.end - span.begin) == 4 &&
      memcmp(span.begin, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if ((size_t)(span.end - span.begin) == 5 &&
      memcmp(span.begin, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}

static bool span_number(JsonSpan span, double *out) {
  if (!span.begin || span.begin == span.end ||
      (size_t)(span.end - span.begin) >= 128)
    return false;
  char text[128];
  size_t n = (size_t)(span.end - span.begin);
  memcpy(text, span.begin, n);
  text[n] = '\0';
  char *tail = NULL;
  errno = 0;
  double value = strtod(text, &tail);
  if (errno || tail == text || *tail || !isfinite(value) || value < 0)
    return false;
  *out = value;
  return true;
}

static char *read_all(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long length = ftell(f);
  if (length < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  char *data = malloc((size_t)length + 1);
  if (!data) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(data, 1, (size_t)length, f);
  fclose(f);
  if (got != (size_t)length) {
    free(data);
    return NULL;
  }
  data[got] = '\0';
  return data;
}

static bool report_sections(const char *path, char *json, JsonSpan *env,
                            JsonSpan *rows) {
  const char *begin = json;
  const char *end = json + strlen(json);
  JsonSpan root = {skip_ws(begin, end), end};
  JsonSpan benchmarks = object_field(root, "benchmarks");
  *env = object_field(root, "environment");
  if (!benchmarks.begin || *benchmarks.begin != '[' || !env->begin ||
      *env->begin != '{') {
    fprintf(stderr, "error: %s: expected report with environment and benchmarks\n",
            path);
    return false;
  }
  *rows = benchmarks;
  return true;
}

static bool config_equal(JsonSpan a, JsonSpan b) {
  if (!a.begin || !b.begin)
    return a.begin == b.begin;
  if (*a.begin == '"' && *b.begin == '"') {
    char av[1024], bv[1024];
    return decode_string(a, av, sizeof(av)) &&
           decode_string(b, bv, sizeof(bv)) && strcmp(av, bv) == 0;
  }
  size_t an = (size_t)(a.end - a.begin), bn = (size_t)(b.end - b.begin);
  return an == bn && memcmp(a.begin, b.begin, an) == 0;
}

static bool rows_push(BenchRows *rows, const BenchRow *row) {
  if (rows->len == rows->cap) {
    size_t cap = rows->cap ? rows->cap * 2 : 64;
    BenchRow *items = realloc(rows->items, cap * sizeof(*items));
    if (!items)
      return false;
    rows->items = items;
    rows->cap = cap;
  }
  rows->items[rows->len++] = *row;
  return true;
}

static bool field_text(JsonSpan obj, const char *field, char *out,
                       size_t cap) {
  JsonSpan span = object_field(obj, field);
  if (!span.begin)
    return true;
  if (*span.begin == '"')
    return span_string(span, out, cap);
  size_t n = (size_t)(span.end - span.begin);
  if (n >= cap)
    return false;
  memcpy(out, span.begin, n);
  out[n] = '\0';
  return true;
}

static bool parse_rows(const char *path, JsonSpan array, BenchRows *rows) {
  const char *p = skip_ws(array.begin + 1, array.end);
  while (p < array.end && *p != ']') {
    const char *q = skip_value(p, array.end, 0);
    if (!q || *p != '{') {
      fprintf(stderr, "error: %s: benchmark entry is not an object\n", path);
      return false;
    }
    JsonSpan obj = {p, q};
    BenchRow row = {0};
    if (!field_text(obj, "name", row.name, sizeof(row.name)) ||
        !field_text(obj, "backend", row.backend, sizeof(row.backend)) ||
        !field_text(obj, "engine", row.engine, sizeof(row.engine)) ||
        !field_text(obj, "cache", row.cache, sizeof(row.cache)) ||
        !field_text(obj, "status", row.status, sizeof(row.status)) ||
        !field_text(obj, "checksum", row.checksum, sizeof(row.checksum)) ||
        !field_text(obj, "ny_checksum", row.ny_checksum,
                    sizeof(row.ny_checksum)) ||
        !field_text(obj, "llvm_checksum", row.llvm_checksum,
                    sizeof(row.llvm_checksum)) ||
        !field_text(obj, "c_checksum", row.c_checksum,
                    sizeof(row.c_checksum))) {
      fprintf(stderr, "error: %s: malformed or oversized benchmark row\n", path);
      return false;
    }
    (void)span_bool(object_field(obj, "checksum_ok"), &row.checksum_ok);
    for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i) {
      bool seen = true;
      JsonSpan seen_span = object_field(obj, metrics[i].seen_field);
      if (seen_span.begin)
        (void)span_bool(seen_span, &seen);
      JsonSpan value = object_field(obj, metrics[i].field);
      row.metric_seen[i] =
          seen && span_number(value, &row.metrics[i]);
    }
    for (size_t i = 0; i < rows->len; ++i) {
      BenchRow *old = &rows->items[i];
      if (strcmp(old->name, row.name) == 0 &&
          strcmp(old->backend, row.backend) == 0 &&
          strcmp(old->engine, row.engine) == 0 &&
          strcmp(old->cache, row.cache) == 0) {
        fprintf(stderr, "error: %s: duplicate benchmark key %s\n", path,
                row.name);
        return false;
      }
    }
    if (!rows_push(rows, &row)) {
      fprintf(stderr, "error: out of memory reading benchmark rows\n");
      return false;
    }
    p = skip_ws(q, array.end);
    if (p < array.end && *p == ',')
      p = skip_ws(p + 1, array.end);
    else
      break;
  }
  return true;
}

static int row_key_cmp(const void *aa, const void *bb) {
  const BenchRow *a = aa, *b = bb;
  int c = strcmp(a->name, b->name);
  if (!c) c = strcmp(a->backend, b->backend);
  if (!c) c = strcmp(a->engine, b->engine);
  if (!c) c = strcmp(a->cache, b->cache);
  return c;
}

static bool checksum_pair(const BenchRow *a, const BenchRow *b) {
  if (!a->checksum_ok || !b->checksum_ok)
    return false;
  const char *const left[] = {a->checksum, a->ny_checksum,
                              a->llvm_checksum, a->c_checksum};
  const char *const right[] = {b->checksum, b->ny_checksum,
                               b->llvm_checksum, b->c_checksum};
  for (size_t i = 0; i < 4; ++i)
    if (*left[i] && *right[i] && strcmp(left[i], right[i]) != 0)
      return false;
  return *a->ny_checksum && strcmp(a->ny_checksum, b->ny_checksum) == 0;
}

static const BenchRow *find_row(const BenchRows *rows, const BenchRow *key) {
  for (size_t i = 0; i < rows->len; ++i)
    if (row_key_cmp(&rows->items[i], key) == 0)
      return &rows->items[i];
  return NULL;
}

static int compare_reports(const char *before_path, const char *after_path,
                           size_t metric, double max_regression) {
  char *before_json = read_all(before_path);
  char *after_json = read_all(after_path);
  if (!before_json || !after_json) {
    fprintf(stderr, "error: cannot read benchmark reports: %s%s%s\n",
            !before_json ? before_path : "", !before_json && !after_json ? ", " : "",
            !after_json ? after_path : "");
    free(before_json); free(after_json);
    return 2;
  }
  JsonSpan before_env, after_env, before_array, after_array;
  BenchRows before = {0}, after = {0};
  int rc = 2;
  if (!report_sections(before_path, before_json, &before_env, &before_array) ||
      !report_sections(after_path, after_json, &after_env, &after_array) ||
      !parse_rows(before_path, before_array, &before) ||
      !parse_rows(after_path, after_array, &after))
    goto done;

  bool config_diff = false;
  for (size_t i = 0; i < sizeof(config_keys) / sizeof(config_keys[0]); ++i) {
    JsonSpan a = object_field(before_env, config_keys[i]);
    JsonSpan b = object_field(after_env, config_keys[i]);
    if (!config_equal(a, b)) {
      if (!config_diff)
        puts("CONFIGURATION MISMATCH");
      config_diff = true;
      printf("  %s: baseline=%.*s candidate=%.*s\n", config_keys[i],
             a.begin ? (int)(a.end - a.begin) : 4,
             a.begin ? a.begin : "null", b.begin ? (int)(b.end - b.begin) : 4,
             b.begin ? b.begin : "null");
    }
  }
  if (config_diff) {
    puts("Use reports with matching target, engine, workers, samples and toolchains.");
    goto done;
  }

  size_t total = before.len + after.len;
  BenchRow *keys = malloc((total ? total : 1) * sizeof(*keys));
  if (!keys) {
    fprintf(stderr, "error: out of memory comparing reports\n");
    goto done;
  }
  size_t key_count = 0;
  for (size_t i = 0; i < before.len; ++i) keys[key_count++] = before.items[i];
  for (size_t i = 0; i < after.len; ++i)
    if (!find_row(&before, &after.items[i]))
      keys[key_count++] = after.items[i];
  qsort(keys, key_count, sizeof(*keys), row_key_cmp);
  printf("baseline:  %s\ncandidate: %s\nmetric: %s · regression limit %.2f%%\n",
         before_path, after_path, metrics[metric].label, max_regression);
  printf("%-28s %12s %12s %10s  result\n", "fixture", "before", "after",
         "change");
  puts("------------------------------------------------------------------------------");
  size_t failures = 0, compared = 0;
  for (size_t i = 0; i < key_count; ++i) {
    const BenchRow *a = find_row(&before, &keys[i]);
    const BenchRow *b = find_row(&after, &keys[i]);
    if (!a || !b) {
      printf("%-28s %12s %12s %10s  missing row\n", keys[i].name, "—", "—", "—");
      ++failures;
      continue;
    }
    if ((!strcmp(a->status, "timer-floor") || !strcmp(a->status, "floor")) &&
        (!strcmp(b->status, "timer-floor") || !strcmp(b->status, "floor"))) {
      printf("%-28s %12s %12s %10s  timer floor\n", a->name, "—", "—", "—");
      continue;
    }
    if (strcmp(a->status, "ok") || strcmp(b->status, "ok")) {
      printf("%-28s %12s %12s %10s  status %s → %s\n",
             a->name, "—", "—", "—", a->status, b->status);
      ++failures;
      continue;
    }
    if (!checksum_pair(a, b)) {
      printf("%-28s %12s %12s %10s  checksum/status failure\n",
             a->name, "—", "—", "—");
      ++failures;
      continue;
    }
    if (!a->metric_seen[metric] || !b->metric_seen[metric]) {
      printf("%-28s %12s %12s %10s  metric unavailable\n",
             a->name, "—", "—", "—");
      ++failures;
      continue;
    }
    double delta = a->metrics[metric] == 0
                      ? (b->metrics[metric] == 0 ? 0 : INFINITY)
                      : (b->metrics[metric] / a->metrics[metric] - 1.0) * 100.0;
    const char *result = delta <= max_regression ? "pass" : "REGRESSION";
    if (delta > max_regression) ++failures;
    ++compared;
    char delta_text[32];
    if (isfinite(delta)) snprintf(delta_text, sizeof(delta_text), "%+.2f%%", delta);
    else snprintf(delta_text, sizeof(delta_text), "+inf");
    printf("%-28s %12.5g %12.5g %10s  %s\n",
           a->name, a->metrics[metric], b->metrics[metric], delta_text, result);
  }
  puts("------------------------------------------------------------------------------");
  printf("compared %zu fixtures; %zu failures\n", compared, failures);
  free(keys);
  rc = failures ? 1 : 0;
done:
  free(before.items); free(after.items);
  free(before_json); free(after_json);
  return rc;
}

int ny_bench_compare_main(int argc, char **argv) {
  const char *paths[2] = {0};
  size_t path_count = 0, metric = 0;
  double max_regression = 2.0;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      puts("Compare two saved ny-test benchmark reports.");
      puts("usage: ny-test bench-compare baseline.json candidate.json "
           "[--metric FIELD] [--max-regression-pct PCT]");
      puts("metrics: ny_native_ms, ny_compile_ms, ny_total_ms, "
           "ny_code_bytes, ny_spills, ny_reloads, ny_peak_compiler_rss_kb");
      return 0;
    } else if (strcmp(argv[i], "--metric") == 0 && i + 1 < argc) {
      const char *name = argv[++i];
      bool found = false;
      for (size_t m = 0; m < sizeof(metrics) / sizeof(metrics[0]); ++m)
        if (strcmp(name, metrics[m].field) == 0) {
          metric = m;
          found = true;
          break;
        }
      if (!found) {
        fprintf(stderr, "error: unknown metric '%s'\n", name);
        return 2;
      }
    } else if (strncmp(argv[i], "--metric=", 9) == 0) {
      char *value = argv[i] + 9;
      bool found = false;
      for (size_t m = 0; m < sizeof(metrics) / sizeof(metrics[0]); ++m)
        if (strcmp(value, metrics[m].field) == 0) {
          metric = m; found = true; break;
        }
      if (!found) { fprintf(stderr, "error: unknown metric '%s'\n", value); return 2; }
    } else if (strcmp(argv[i], "--max-regression-pct") == 0 &&
               i + 1 < argc) {
      char *tail = NULL;
      max_regression = strtod(argv[++i], &tail);
      if (tail == argv[i] || *tail || !isfinite(max_regression) ||
          max_regression < 0) {
        fputs("error: --max-regression-pct must be finite and nonnegative\n", stderr);
        return 2;
      }
    } else if (strncmp(argv[i], "--max-regression-pct=", 21) == 0) {
      char *tail = NULL;
      max_regression = strtod(argv[i] + 21, &tail);
      if (tail == argv[i] + 21 || *tail || !isfinite(max_regression) ||
          max_regression < 0) {
        fputs("error: --max-regression-pct must be finite and nonnegative\n", stderr);
        return 2;
      }
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
      return 2;
    } else if (path_count < 2) {
      paths[path_count++] = argv[i];
    } else {
      fputs("usage: ny-test bench-compare baseline.json candidate.json "
            "[--metric FIELD] [--max-regression-pct PCT]\n", stderr);
      return 2;
    }
  }
  if (path_count != 2) {
    fputs("usage: ny-test bench-compare baseline.json candidate.json "
          "[--metric FIELD] [--max-regression-pct PCT]\n", stderr);
    return 2;
  }
  return compare_reports(paths[0], paths[1], metric, max_regression);
}
