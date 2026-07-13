#include "core.h"

typedef struct {
  const char *tool;
  const char *flavor;
  const char **flags;
  int flag_count;
  double compile_ms;
  double run_ms;
  int run_repeats;
  int warmup;
  char *output;
} compare_variant_t;

typedef struct {
  const char *tool;
  const char *phase;
  const char *flavor;
  const char *failure_kind;
  int rc;
  bool timed_out;
  char *stderr_text;
  char *stdout_text;
  char *expected;
  char *actual;
} compare_failure_t;

typedef struct {
  const char *case_name;
  const char *c_path;
  const char *ny_path;
  const char *ir_path;
  source_counts_t shape_counts;
  bool has_shape_counts;
  const char **features;
  int feature_count;
  compare_variant_t variants[5];
  int variant_count;
  compare_failure_t failures[8];
  int failure_count;
  double worker_ms;
} compare_state_t;

typedef struct {
  char *shape;
  char *family;
  char *profile;
  char *generator;
  char *generator_kind;
  char *method;
  char *source_kind;
  char *shape_source;
  char *shape_hash;
  char *template_name;
  char *features_json;
  long seed;
  int shape_dsl_version;
  bool has_seed;
  bool has_shape_dsl_version;
  char structural_hash[32];
  char c_emitter_hash[32];
  char ny_emitter_hash[32];
} compare_ir_meta_t;

static void compare_state_free(compare_state_t *st) {
  for (int i = 0; i < st->variant_count; ++i) free(st->variants[i].output);
  for (int i = 0; i < st->failure_count; ++i) {
    free(st->failures[i].stderr_text);
    free(st->failures[i].stdout_text);
    free(st->failures[i].expected);
    free(st->failures[i].actual);
  }
}

static void compare_ir_meta_free(compare_ir_meta_t *meta) {
  free(meta->shape);
  free(meta->family);
  free(meta->profile);
  free(meta->generator);
  free(meta->generator_kind);
  free(meta->method);
  free(meta->source_kind);
  free(meta->shape_source);
  free(meta->shape_hash);
  free(meta->template_name);
  free(meta->features_json);
  memset(meta, 0, sizeof(*meta));
}

static const char *cmp_skip_ws(const char *p) {
  while (p && *p && isspace((unsigned char)*p)) ++p;
  return p;
}

static const char *cmp_json_value_after_key_range(const char *start, const char *end,
                                                  const char *key) {
  char pat[128];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  size_t pat_len = strlen(pat);
  for (const char *p = start; p && p + pat_len <= end; ++p) {
    if (memcmp(p, pat, pat_len) != 0) continue;
    p += pat_len;
    while (p < end && isspace((unsigned char)*p)) ++p;
    if (p >= end || *p != ':') return NULL;
    return cmp_skip_ws(p + 1);
  }
  return NULL;
}

static const char *cmp_matching_json_end(const char *open, char lhs, char rhs) {
  bool in_string = false;
  bool escape = false;
  int depth = 0;
  for (const char *p = open; p && *p; ++p) {
    if (in_string) {
      if (escape) escape = false;
      else if (*p == '\\') escape = true;
      else if (*p == '"') in_string = false;
      continue;
    }
    if (*p == '"') in_string = true;
    else if (*p == lhs) ++depth;
    else if (*p == rhs) {
      --depth;
      if (depth == 0) return p;
    }
  }
  return NULL;
}

static char *cmp_parse_json_string_dup(const char **cursor, const char *end) {
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

static char *cmp_json_extract_string(const char *json, const char *key) {
  const char *end = json + strlen(json);
  const char *p = cmp_json_value_after_key_range(json, end, key);
  if (!p || p >= end || *p != '"') return NULL;
  return cmp_parse_json_string_dup(&p, end);
}

static char *cmp_json_extract_array(const char *json, const char *key) {
  const char *end = json + strlen(json);
  const char *p = cmp_json_value_after_key_range(json, end, key);
  if (!p || p >= end || *p != '[') return NULL;
  const char *q = cmp_matching_json_end(p, '[', ']');
  if (!q) return NULL;
  return strndup(p, (size_t)(q - p + 1));
}

static bool cmp_json_extract_long(const char *json, const char *key, long *out) {
  const char *end = json + strlen(json);
  const char *p = cmp_json_value_after_key_range(json, end, key);
  if (!p) return false;
  char *num_end = NULL;
  long value = strtol(p, &num_end, 10);
  if (num_end == p) return false;
  *out = value;
  return true;
}

static void hash_file_hex(const char *path, char *out, size_t out_sz) {
  if (out_sz) out[0] = '\0';
  if (!path || !*path || !out_sz) return;
  file_buf_t f = {0};
  if (!read_file(path, &f)) return;
  snprintf(out, out_sz, "%016" PRIx64, fnv1a64(f.data, f.len));
  free(f.data);
}

static void load_compare_ir_meta(compare_state_t *st, compare_ir_meta_t *meta) {
  memset(meta, 0, sizeof(*meta));
  hash_file_hex(st->c_path, meta->c_emitter_hash, sizeof(meta->c_emitter_hash));
  hash_file_hex(st->ny_path, meta->ny_emitter_hash, sizeof(meta->ny_emitter_hash));
  if (!st->ir_path || !*st->ir_path) return;
  file_buf_t f = {0};
  if (!read_file(st->ir_path, &f)) return;
  snprintf(meta->structural_hash, sizeof(meta->structural_hash), "%016" PRIx64,
           fnv1a64(f.data, f.len));
  meta->shape = cmp_json_extract_string(f.data, "shape");
  meta->family = cmp_json_extract_string(f.data, "family");
  meta->profile = cmp_json_extract_string(f.data, "profile");
  meta->generator = cmp_json_extract_string(f.data, "generator");
  meta->generator_kind = cmp_json_extract_string(f.data, "generator_kind");
  meta->method = cmp_json_extract_string(f.data, "method");
  meta->source_kind = cmp_json_extract_string(f.data, "source_kind");
  meta->shape_source = cmp_json_extract_string(f.data, "shape_source");
  meta->shape_hash = cmp_json_extract_string(f.data, "shape_hash");
  meta->template_name = cmp_json_extract_string(f.data, "template");
  meta->features_json = cmp_json_extract_array(f.data, "features");
  long value = 0;
  if (cmp_json_extract_long(f.data, "seed", &value)) {
    meta->seed = value;
    meta->has_seed = true;
  }
  if (cmp_json_extract_long(f.data, "shape_dsl_version", &value)) {
    meta->shape_dsl_version = (int)value;
    meta->has_shape_dsl_version = true;
  }
  free(f.data);
}

static bool cmp_contains_ci(const char *haystack, const char *needle) {
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

static bool cmp_text_has_crash(const char *stderr_text, const char *stdout_text) {
  const char *texts[2] = {stderr_text, stdout_text};
  for (int i = 0; i < 2; ++i) {
    const char *s = texts[i];
    if (!s) continue;
    if (cmp_contains_ci(s, "segmentation fault") ||
        cmp_contains_ci(s, "segmentationfault") ||
        cmp_contains_ci(s, "signal 11") ||
        cmp_contains_ci(s, "sigsegv") ||
        cmp_contains_ci(s, "internal compiler error") ||
        cmp_contains_ci(s, "addresssanitizer") ||
        cmp_contains_ci(s, "undefinedbehavior") ||
        cmp_contains_ci(s, "assertion") ||
        cmp_contains_ci(s, "panic"))
      return true;
  }
  return false;
}

static const char *classify_failure_kind(const char *phase, int rc, bool timed_out,
                                         const char *stderr_text, const char *stdout_text) {
  if (timed_out || rc == 124) return "timeout";
  if (phase && strcmp(phase, "prepare") == 0) return "prepare_error";
  if (phase && strcmp(phase, "diff") == 0) return "output_diff";
  bool crashed = rc >= 128 || cmp_text_has_crash(stderr_text, stdout_text);
  if (phase && strcmp(phase, "compile") == 0)
    return crashed ? "compiler_crash" : "compile_error";
  if (phase && strcmp(phase, "run") == 0) return "runtime_error";
  return crashed ? "compiler_crash" : "prepare_error";
}

static void add_failure(compare_state_t *st, const char *tool, const char *phase,
                        const char *flavor, int rc, bool timed_out,
                        const char *stderr_text, const char *stdout_text,
                        const char *expected, const char *actual) {
  if (st->failure_count >= (int)(sizeof(st->failures) / sizeof(st->failures[0]))) return;
  compare_failure_t *f = &st->failures[st->failure_count++];
  f->tool = tool;
  f->phase = phase;
  f->flavor = flavor;
  f->failure_kind = classify_failure_kind(phase, rc, timed_out, stderr_text, stdout_text);
  f->rc = rc;
  f->timed_out = timed_out;
  f->stderr_text = strdup(stderr_text ? stderr_text : "");
  f->stdout_text = stdout_text && *stdout_text ? strdup(stdout_text) : NULL;
  f->expected = expected ? strdup(expected) : NULL;
  f->actual = actual ? strdup(actual) : NULL;
}

static void add_variant(compare_state_t *st, const char *tool, const char *flavor,
                        const char **flags, int flag_count, double compile_ms,
                        double run_ms, int run_repeats, int warmup,
                        const char *output) {
  if (st->variant_count >= (int)(sizeof(st->variants) / sizeof(st->variants[0]))) return;
  compare_variant_t *v = &st->variants[st->variant_count++];
  v->tool = tool;
  v->flavor = flavor;
  v->flags = flags;
  v->flag_count = flag_count;
  v->compile_ms = compile_ms;
  v->run_ms = run_ms;
  v->run_repeats = run_repeats;
  v->warmup = warmup;
  v->output = strdup(output ? output : "");
}

static compare_variant_t *find_variant(compare_state_t *st, const char *tool, const char *flavor) {
  for (int i = 0; i < st->variant_count; ++i) {
    if (strcmp(st->variants[i].tool, tool) == 0 && strcmp(st->variants[i].flavor, flavor) == 0)
      return &st->variants[i];
  }
  return NULL;
}

static void print_flags_json(FILE *out, const char **flags, int count) {
  fputc('[', out);
  for (int i = 0; i < count; ++i) {
    if (i) fputc(',', out);
    json_str(out, flags[i]);
  }
  fputc(']', out);
}

static void print_variants_json(FILE *out, compare_state_t *st) {
  fputc('[', out);
  for (int i = 0; i < st->variant_count; ++i) {
    compare_variant_t *v = &st->variants[i];
    if (i) fputc(',', out);
    fprintf(out, "{\"tool\":");
    json_str(out, v->tool);
    fprintf(out, ",\"flavor\":");
    json_str(out, v->flavor);
    fprintf(out, ",\"flags\":");
    print_flags_json(out, v->flags, v->flag_count);
    fprintf(out, ",\"compile_ms\":%.2f,\"run_ms\":%.2f,\"run_repeats\":%d,\"warmup\":%d,\"output\":",
            v->compile_ms, v->run_ms, v->run_repeats, v->warmup);
    json_str(out, v->output);
    fputc('}', out);
  }
  fputc(']', out);
}

static void print_tail_json(FILE *out, const char *s, size_t limit) {
  size_t n = s ? strlen(s) : 0;
  const char *start = s ? s : "";
  if (n > limit) start += n - limit;
  json_str(out, start);
}

static void print_failures_json(FILE *out, compare_state_t *st) {
  fputc('[', out);
  for (int i = 0; i < st->failure_count; ++i) {
    compare_failure_t *f = &st->failures[i];
    if (i) fputc(',', out);
    fprintf(out, "{\"failure_kind\":");
    json_str(out, f->failure_kind ? f->failure_kind : "prepare_error");
    fprintf(out, ",\"tool\":");
    json_str(out, f->tool);
    fprintf(out, ",\"phase\":");
    json_str(out, f->phase);
    if (f->flavor && *f->flavor) {
      fprintf(out, ",\"flavor\":");
      json_str(out, f->flavor);
    }
    fprintf(out, ",\"rc\":%d", f->rc);
    if (f->timed_out) fprintf(out, ",\"timed_out\":true");
    fprintf(out, ",\"c_source\":");
    json_str(out, st->c_path ? st->c_path : "");
    fprintf(out, ",\"ny_source\":");
    json_str(out, st->ny_path ? st->ny_path : "");
    if (st->ir_path && *st->ir_path) {
      fprintf(out, ",\"nytrix_ir\":");
      json_str(out, st->ir_path);
    }
    if (f->stderr_text) {
      fprintf(out, ",\"stderr_tail\":");
      print_tail_json(out, f->stderr_text, 2000);
      fprintf(out, ",\"stderr\":");
      json_str(out, f->stderr_text);
    }
    if (f->stdout_text) {
      fprintf(out, ",\"stdout_tail\":");
      print_tail_json(out, f->stdout_text, 2000);
      fprintf(out, ",\"stdout\":");
      json_str(out, f->stdout_text);
    }
    if (f->expected) {
      fprintf(out, ",\"expected\":");
      json_str(out, f->expected);
    }
    if (f->actual) {
      fprintf(out, ",\"actual\":");
      json_str(out, f->actual);
    }
    fputc('}', out);
  }
  fputc(']', out);
}

static void print_ratios_json(FILE *out, compare_state_t *st) {
  compare_variant_t *c_o0 = find_variant(st, "c", "o0");
  compare_variant_t *c_o3 = find_variant(st, "c", "o3");
  compare_variant_t *ny_o0 = find_variant(st, "ny", "o0");
  compare_variant_t *ny_o3 = find_variant(st, "ny", "o3");
  compare_variant_t *ny_o3i = find_variant(st, "ny", "o3i");
  bool first = true;
#define RATIO(name, num, den, field) \
  do { if ((num) && (den) && (den)->field > 0.0) { \
    if (!first) fputc(',', out); \
    first = false; \
    fprintf(out, "\"%s\":%.4f", (name), (num)->field / (den)->field); \
  } } while (0)
  fputc('{', out);
  if (!st->failure_count) {
    RATIO("ny_o0_vs_c_o0_compile", ny_o0, c_o0, compile_ms);
    RATIO("ny_o0_vs_c_o0_run", ny_o0, c_o0, run_ms);
    RATIO("ny_o3_vs_c_o3_compile", ny_o3, c_o3, compile_ms);
    RATIO("ny_o3_vs_c_o3_run", ny_o3, c_o3, run_ms);
    RATIO("ny_o3i_vs_c_o3_compile", ny_o3i, c_o3, compile_ms);
    RATIO("ny_o3i_vs_c_o3_run", ny_o3i, c_o3, run_ms);
  }
  fputc('}', out);
#undef RATIO
}

static void print_compare_features_json(FILE *out, compare_state_t *st) {
  fputc('[', out);
  for (int i = 0; i < st->feature_count; ++i) {
    if (i) fputc(',', out);
    json_str(out, st->features[i]);
  }
  fputc(']', out);
}

static const char *expected_output(compare_state_t *st) {
  compare_variant_t *c_o0 = find_variant(st, "c", "o0");
  return c_o0 ? c_o0->output : "";
}

static void print_compare_row(compare_state_t *st) {
  const char *behavior = expected_output(st);
  uint64_t behavior_hash = fnv1a64(behavior, strlen(behavior));
  compare_ir_meta_t meta;
  load_compare_ir_meta(st, &meta);
  printf("{\"ok\":true,\"engine\":\"nytrix_core\",\"compare_engine\":\"nytrix_core\",\"case\":");
  json_str(stdout, st->case_name);
  if (meta.shape) {
    printf(",\"shape\":");
    json_str(stdout, meta.shape);
  }
  if (meta.family) {
    printf(",\"family\":");
    json_str(stdout, meta.family);
  }
  if (meta.profile) {
    printf(",\"profile\":");
    json_str(stdout, meta.profile);
  }
  if (meta.generator) {
    printf(",\"generator\":");
    json_str(stdout, meta.generator);
  }
  if (meta.generator_kind) {
    printf(",\"generator_kind\":");
    json_str(stdout, meta.generator_kind);
  } else if (meta.generator) {
    printf(",\"generator_kind\":");
    json_str(stdout, meta.generator);
  }
  if (meta.method) {
    printf(",\"method\":");
    json_str(stdout, meta.method);
  } else if (meta.generator_kind) {
    printf(",\"method\":");
    json_str(stdout, meta.generator_kind);
  } else if (meta.generator) {
    printf(",\"method\":");
    json_str(stdout, meta.generator);
  }
  if (meta.source_kind) {
    printf(",\"source_kind\":");
    json_str(stdout, meta.source_kind);
  }
  if (meta.shape_source) {
    printf(",\"shape_source\":");
    json_str(stdout, meta.shape_source);
  }
  if (meta.shape_hash) {
    printf(",\"shape_hash\":");
    json_str(stdout, meta.shape_hash);
  }
  if (meta.template_name) {
    printf(",\"template\":");
    json_str(stdout, meta.template_name);
  }
  if (meta.has_seed) printf(",\"seed\":%ld", meta.seed);
  if (meta.has_shape_dsl_version) printf(",\"shape_dsl_version\":%d", meta.shape_dsl_version);
  printf(",\"features\":");
  if (st->feature_count > 0) print_compare_features_json(stdout, st);
  else if (meta.features_json) fputs(meta.features_json, stdout);
  else print_compare_features_json(stdout, st);
  printf(",\"c_source\":");
  json_str(stdout, st->c_path);
  printf(",\"ny_source\":");
  json_str(stdout, st->ny_path);
  if (st->ir_path && *st->ir_path) {
    printf(",\"nytrix_ir\":");
    json_str(stdout, st->ir_path);
  }
  printf(",\"worker_ms\":%.2f,\"expected_output\":", st->worker_ms);
  json_str(stdout, behavior);
  printf(",\"behavior_hash\":\"%016" PRIx64 "\",\"behavior_hash_fnv1a64\":\"%016" PRIx64 "\"", behavior_hash, behavior_hash);
  if (meta.structural_hash[0]) {
    printf(",\"structural_hash\":");
    json_str(stdout, meta.structural_hash);
  }
  if (meta.c_emitter_hash[0]) {
    printf(",\"c_emitter_hash\":");
    json_str(stdout, meta.c_emitter_hash);
  }
  if (meta.ny_emitter_hash[0]) {
    printf(",\"ny_emitter_hash\":");
    json_str(stdout, meta.ny_emitter_hash);
  }
  printf(",\"reducer_mode\":\"native_compare\",\"variants\":");
  print_variants_json(stdout, st);
  printf(",\"ratios\":");
  print_ratios_json(stdout, st);
  printf(",\"shape_counts\":");
  if (st->has_shape_counts) print_source_counts_json(stdout, &st->shape_counts);
  else printf("{}");
  printf(",\"ir_analysis\":{},\"failures\":");
  print_failures_json(stdout, st);
  printf("}\n");
  compare_ir_meta_free(&meta);
}

static bool compare_elf_path(char **out, const char *bin_dir, const char *case_name,
                             const char *tool, const char *flavor) {
  int n = asprintf(out, "%s/%s_%s_%s.elf", bin_dir, case_name, tool, flavor);
  return n >= 0 && *out;
}

int native_compare_case_with_features(const char *case_name, const char *c_path, const char *ny_path,
                                      const char *ir_path, const char *root_dir, const char *ny_bin_path,
                                      const char *bin_dir, double timeout_s, int run_repeats,
                                      int warmup, const char **features, int feature_count) {
  double start = now_ms();
  if (!case_name || !*case_name || !c_path || !*c_path || !ny_path || !*ny_path ||
      !root_dir || !*root_dir || !ny_bin_path || !*ny_bin_path || !bin_dir || !*bin_dir) {
    printf("{\"ok\":false,\"error\":\"unsupported\",\"reason\":\"missing-required-path\"}\n");
    return 3;
  }
  compare_state_t st;
  memset(&st, 0, sizeof(st));
  st.case_name = case_name;
  st.c_path = c_path;
  st.ny_path = ny_path;
  st.ir_path = ir_path && *ir_path ? ir_path : "";
  st.features = features;
  st.feature_count = feature_count;
  st.has_shape_counts = compute_source_shape_counts(c_path, ny_path, &st.shape_counts);
  if (!mkdir_p(bin_dir)) {
    add_failure(&st, "nytrix_core", "prepare", "", 1, false,
                "mkdir failed", NULL, NULL, NULL);
    st.worker_ms = now_ms() - start;
    print_compare_row(&st);
    compare_state_free(&st);
    return 0;
  }

  static const char *c_o0_flags[] = {"-O0"};
  static const char *c_o3_flags[] = {"-O3", "-march=native", "-DNDEBUG"};
  static const char *ny_o0_flags[] = {"-O0"};
  static const char *ny_o3_flags[] = {"-O3"};
  static const char *ny_o3i_flags[] = {"-O3", "--profile=peak"};
  const struct { const char *flavor; const char **flags; int flag_count; } c_flavors[] = {
    {"o0", c_o0_flags, 1},
    {"o3", c_o3_flags, 3},
  };
  const struct { const char *flavor; const char **flags; int flag_count; } ny_flavors[] = {
    {"o0", ny_o0_flags, 1},
    {"o3", ny_o3_flags, 1},
    {"o3i", ny_o3i_flags, 2},
  };

  for (int i = 0; i < 2; ++i) {
    char *elf = NULL;
    if (!compare_elf_path(&elf, bin_dir, case_name, "c", c_flavors[i].flavor)) {
      add_failure(&st, "c", "compile", c_flavors[i].flavor, 1, false,
                  "path allocation failed", NULL, NULL, NULL);
      break;
    }
    char *argv_o0[] = {"gcc", "-O0", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra", "-o", elf, (char *)c_path, NULL};
    char *argv_o3[] = {"gcc", "-O3", "-march=native", "-DNDEBUG", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra", "-o", elf, (char *)c_path, NULL};
    proc_result_t compile = run_proc(i == 0 ? argv_o0 : argv_o3, root_dir, timeout_s);
    if (compile.rc != 0) {
      add_failure(&st, "c", "compile", c_flavors[i].flavor, compile.rc, compile.timed_out,
                  compile.err, compile.out, NULL, NULL);
      proc_result_free(&compile);
      free(elf);
      break;
    }
    run_many_result_t run = run_binary_many_native(root_dir, elf, timeout_s, run_repeats, warmup);
    if (run.rc != 0) {
      str_buf_t msg = {0};
      if (run.out) (void)sb_append(&msg, run.out);
      if (run.err && *run.err) {
        if (msg.len) (void)sb_append_c(&msg, '\n');
        (void)sb_append(&msg, run.err);
      }
      add_failure(&st, "c", "run", c_flavors[i].flavor, run.rc, false,
                  msg.data ? msg.data : "", run.out, NULL, NULL);
      free(msg.data);
      run_many_result_free(&run);
      proc_result_free(&compile);
      free(elf);
      break;
    }
    add_variant(&st, "c", c_flavors[i].flavor, c_flavors[i].flags, c_flavors[i].flag_count,
                compile.elapsed_ms, run.median_ms, run_repeats, warmup, run.normalized);
    run_many_result_free(&run);
    proc_result_free(&compile);
    free(elf);
  }
  compare_variant_t *c_o0 = find_variant(&st, "c", "o0");
  compare_variant_t *c_o3 = find_variant(&st, "c", "o3");
  if (!st.failure_count && c_o0 && c_o3 && strcmp(c_o0->output, c_o3->output) != 0) {
    add_failure(&st, "c", "diff", "", 0, false, "", NULL, c_o0->output, c_o3->output);
  }

  for (int i = 0; !st.failure_count && i < 3; ++i) {
    char *elf = NULL;
    if (!compare_elf_path(&elf, bin_dir, case_name, "ny", ny_flavors[i].flavor)) {
      add_failure(&st, "ny", "compile", ny_flavors[i].flavor, 1, false,
                  "path allocation failed", NULL, NULL, NULL);
      break;
    }
    char *argv_o0[] = {(char *)ny_bin_path, "--compiler-asserts", "-O0", "-o", elf, (char *)ny_path, NULL};
    char *argv_o3[] = {(char *)ny_bin_path, "--compiler-asserts", "-O3", "-o", elf, (char *)ny_path, NULL};
    char *argv_o3i[] = {(char *)ny_bin_path, "--compiler-asserts", "-O3", "--profile=peak", "-o", elf, (char *)ny_path, NULL};
    char **argv = i == 0 ? argv_o0 : (i == 1 ? argv_o3 : argv_o3i);
    proc_result_t compile = run_proc(argv, root_dir, timeout_s);
    if (compile.rc != 0) {
      add_failure(&st, "ny", "compile", ny_flavors[i].flavor, compile.rc, compile.timed_out,
                  compile.err, compile.out, NULL, NULL);
      proc_result_free(&compile);
      free(elf);
      break;
    }
    run_many_result_t run = run_binary_many_native(root_dir, elf, timeout_s, run_repeats, warmup);
    if (run.rc != 0) {
      str_buf_t msg = {0};
      if (run.out) (void)sb_append(&msg, run.out);
      if (run.err && *run.err) {
        if (msg.len) (void)sb_append_c(&msg, '\n');
        (void)sb_append(&msg, run.err);
      }
      add_failure(&st, "ny", "run", ny_flavors[i].flavor, run.rc, false,
                  msg.data ? msg.data : "", run.out, NULL, NULL);
      free(msg.data);
      run_many_result_free(&run);
      proc_result_free(&compile);
      free(elf);
      break;
    }
    add_variant(&st, "ny", ny_flavors[i].flavor, ny_flavors[i].flags, ny_flavors[i].flag_count,
                compile.elapsed_ms, run.median_ms, run_repeats, warmup, run.normalized);
    run_many_result_free(&run);
    proc_result_free(&compile);
    free(elf);
  }
  if (!st.failure_count && c_o0) {
    for (int i = 0; i < 3; ++i) {
      compare_variant_t *ny_v = find_variant(&st, "ny", ny_flavors[i].flavor);
      if (ny_v && strcmp(ny_v->output, c_o0->output) != 0) {
        add_failure(&st, "cross", "diff", ny_flavors[i].flavor, 0, false,
                    "", NULL, c_o0->output, ny_v->output);
        break;
      }
    }
  }
  st.worker_ms = now_ms() - start;
  print_compare_row(&st);
  compare_state_free(&st);
  return 0;
}

static int parse_features_arg(char *text, const char **features, int cap) {
  int count = 0;
  if (!text || !*text) return 0;
  char *start = text;
  for (char *p = text; ; ++p) {
    if (*p != ',' && *p != '\0') continue;
    char save = *p;
    *p = '\0';
    while (*start == ' ' || *start == '\t') ++start;
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    if (*start && count < cap) features[count++] = start;
    if (save == '\0') break;
    start = p + 1;
  }
  return count;
}

static void features_csv_from_ir_file(const char *ir_path, char *out, size_t out_sz) {
  if (!out_sz) return;
  out[0] = '\0';
  if (!ir_path || !*ir_path) return;
  file_buf_t f = {0};
  if (!read_file(ir_path, &f)) return;
  const char *p = strstr(f.data, "\"features\"");
  if (!p) {
    free(f.data);
    return;
  }
  p = strchr(p, '[');
  if (!p) {
    free(f.data);
    return;
  }
  const char *end = strchr(p, ']');
  if (!end) {
    free(f.data);
    return;
  }
  size_t len = 0;
  for (++p; p < end; ++p) {
    while (p < end && *p != '"') ++p;
    if (p >= end) break;
    ++p;
    char item[128];
    size_t item_len = 0;
    bool escape = false;
    while (p < end && *p) {
      char c = *p++;
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
        continue;
      } else if (c == '"') {
        break;
      }
      if (item_len + 1u < sizeof(item)) item[item_len++] = c;
    }
    item[item_len] = '\0';
    if (!item_len) continue;
    size_t need = item_len + (len ? 1u : 0u);
    if (len + need + 1u >= out_sz) break;
    if (len) out[len++] = ',';
    memcpy(out + len, item, item_len);
    len += item_len;
    out[len] = '\0';
  }
  free(f.data);
}

int cmd_compare_case(int argc, char **argv) {
  const char *c_path = arg_value(argc, argv, "--c", "");
  const char *ny_path = arg_value(argc, argv, "--ny", "");
  const char *ir_path = arg_value(argc, argv, "--ir", "");
  const char *root_dir = arg_value(argc, argv, "--root", ".");
  const char *ny_bin_path = arg_value(argc, argv, "--ny-bin", "");
  const char *bin_dir = arg_value(argc, argv, "--bin-dir", "build/native_compare");
  const char *name = arg_value(argc, argv, "--case", "");
  char derived[256];
  if (!name || !*name) {
    stem_name(c_path, derived, sizeof(derived));
    name = derived;
  }
  double timeout_s = atof(arg_value(argc, argv, "--timeout-s", "60"));
  int runs = atoi(arg_value(argc, argv, "--runs", "1"));
  int warmup = atoi(arg_value(argc, argv, "--warmup", "0"));
  char features_buf[1024];
  snprintf(features_buf, sizeof(features_buf), "%s", arg_value(argc, argv, "--features", ""));
  const char *features[64];
  int feature_count = parse_features_arg(features_buf, features, 64);
  return native_compare_case_with_features(name, c_path, ny_path, ir_path, root_dir, ny_bin_path,
                                           bin_dir, timeout_s, runs, warmup, features, feature_count);
}

int cmd_replay_corpus_entry(int argc, char **argv) {
  const char *corpus_dir = arg_value(argc, argv, "--corpus-dir", "");
  const char *entry_id = arg_value(argc, argv, "--entry-id", "");
  const char *root_dir = arg_value(argc, argv, "--root", ".");
  const char *ny_bin_path = arg_value(argc, argv, "--ny-bin", "");
  const char *bin_dir = arg_value(argc, argv, "--bin-dir", "build/native_replay");
  double timeout_s = atof(arg_value(argc, argv, "--timeout-s", "60"));
  int runs = atoi(arg_value(argc, argv, "--runs", "1"));
  int warmup = atoi(arg_value(argc, argv, "--warmup", "0"));
  if (!corpus_dir || !*corpus_dir || !entry_id || !*entry_id) {
    printf("{\"ok\":false,\"error\":\"unsupported\",\"reason\":\"missing-corpus-entry\"}\n");
    return 3;
  }
  char *case_dir = NULL, *c_path = NULL, *ny_path = NULL, *ir_path = NULL;
  bool ok = asprintf(&case_dir, "%s/cases/%s", corpus_dir, entry_id) >= 0 &&
            asprintf(&c_path, "%s/case.c", case_dir) >= 0 &&
            asprintf(&ny_path, "%s/case.ny", case_dir) >= 0 &&
            asprintf(&ir_path, "%s/case.nytrix.json", case_dir) >= 0;
  if (!ok) {
    printf("{\"ok\":false,\"error\":\"allocation-failed\"}\n");
    free(case_dir); free(c_path); free(ny_path); free(ir_path);
    return 3;
  }
  struct stat st;
  (void)st;
  char features_buf[1024];
  features_csv_from_ir_file(ir_path, features_buf, sizeof(features_buf));
  const char *features[64];
  int feature_count = parse_features_arg(features_buf, features, 64);
  int rc = native_compare_case_with_features(entry_id, c_path, ny_path, ir_path, root_dir, ny_bin_path,
                                             bin_dir, timeout_s, runs, warmup, features, feature_count);
  free(case_dir); free(c_path); free(ny_path); free(ir_path);
  return rc;
}
