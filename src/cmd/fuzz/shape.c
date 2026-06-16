#include "core.h"

int cmd_shape_count(const char *path) {
  file_buf_t f = {0};
  if (!read_file(path, &f)) {
    printf("{\"ok\":false,\"error\":\"read-failed\",\"path\":");
    json_str(stdout, path);
    printf("}\n");
    return 1;
  }
  int shape_count = count_sub(f.data, f.len, "\nshape ");
  if (f.len >= 6 && memcmp(f.data, "shape ", 6) == 0) ++shape_count;
  printf("{\"ok\":true,\"path\":");
  json_str(stdout, path);
  printf(",\"bytes\":%zu,\"lines\":%d,\"shape_count\":%d,"
         "\"generators\":{\"typed\":%d,\"optimizer\":%d,\"torture\":%d,\"stress\":%d,\"program\":%d,\"error\":%d}}\n",
         f.len, count_lines(f.data, f.len), shape_count,
         count_sub(f.data, f.len, "generator \"typed\""),
         count_sub(f.data, f.len, "generator \"optimizer\""),
         count_sub(f.data, f.len, "generator \"torture\""),
         count_sub(f.data, f.len, "generator \"stress\""),
         count_sub(f.data, f.len, "generator \"program\""),
         count_sub(f.data, f.len, "generator \"error\""));
  free(f.data);
  return 0;
}

typedef struct {
  char names[256][96];
  int count;
} name_set_t;

static void set_add(name_set_t *set, const char *name, size_t len) {
  if (!len || len >= sizeof(set->names[0])) return;
  for (int i = 0; i < set->count; ++i) {
    if (strlen(set->names[i]) == len && memcmp(set->names[i], name, len) == 0) return;
  }
  if (set->count >= 256) return;
  memcpy(set->names[set->count], name, len);
  set->names[set->count][len] = '\0';
  ++set->count;
}

static bool line_declares_array(const char *line, size_t len) {
  return (memmem(line, len, "int ", 4) || memmem(line, len, "long ", 5) ||
          memmem(line, len, "char ", 5) || memmem(line, len, "bool ", 5) ||
          memmem(line, len, "unsigned ", 9) || memmem(line, len, "const ", 6)) &&
         memmem(line, len, "[", 1) && memmem(line, len, "]", 1);
}

static void collect_array_before_bracket(const char *line, size_t len, name_set_t *set) {
  for (size_t i = 0; i < len; ++i) {
    if (line[i] != '[') continue;
    size_t end = i;
    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
    size_t start = end;
    while (start > 0 && ident_char(line[start - 1])) --start;
    set_add(set, line + start, end - start);
  }
}

static void collect_written_array(const char *line, size_t len, name_set_t *set) {
  const char *br = memmem(line, len, "[", 1);
  const char *rb = br ? memmem(br, (size_t)(line + len - br), "]", 1) : NULL;
  if (!br || !rb) return;
  const char *p = rb + 1;
  while (p < line + len && (*p == ' ' || *p == '\t')) ++p;
  bool writes = false;
  if (p < line + len && *p == '=') writes = true;
  if (p + 1 < line + len && (p[0] == '+' || p[0] == '-' || p[0] == '*' ||
                              p[0] == '/' || p[0] == '%' || p[0] == '&' ||
                              p[0] == '|' || p[0] == '^') && p[1] == '=') writes = true;
  if (p + 1 < line + len && ((p[0] == '+' && p[1] == '+') || (p[0] == '-' && p[1] == '-'))) writes = true;
  if (!writes) return;
  size_t end = (size_t)(br - line);
  while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
  size_t start = end;
  while (start > 0 && ident_char(line[start - 1])) --start;
  set_add(set, line + start, end - start);
}

bool compute_source_shape_counts(const char *c_path, const char *ny_path,
                                        source_counts_t *out) {
  file_buf_t c = {0}, ny = {0};
  if (!read_file(c_path, &c) || !read_file(ny_path, &ny)) {
    if (c.data) free(c.data);
    if (ny.data) free(ny.data);
    return false;
  }
  name_set_t declared = {0}, written = {0};
  size_t start = 0;
  for (size_t i = 0; i <= c.len; ++i) {
    if (i != c.len && c.data[i] != '\n') continue;
    const char *line = c.data + start;
    size_t line_len = i - start;
    if (line_declares_array(line, line_len)) collect_array_before_bracket(line, line_len, &declared);
    collect_written_array(line, line_len, &written);
    start = i + 1;
  }
  int literal_lists = count_sub(ny.data, ny.len, "= [");
  int def_lists = count_regexish_assign_list(ny.data, ny.len, "def ");
  int mut_lists = count_regexish_assign_list(ny.data, ny.len, "mut ");
  int has_set_idx = count_sub(ny.data, ny.len, "set_idx(") > 0;
  memset(out, 0, sizeof(*out));
  out->c_for = count_sub(c.data, c.len, "for (");
  out->c_if = count_sub(c.data, c.len, "if (");
  out->c_array_reads = count_sub(c.data, c.len, "[");
  out->c_array_writes = written.count;
  out->c_declared_arrays = declared.count;
  out->c_readonly_arrays = declared.count > written.count ? declared.count - written.count : 0;
  out->ny_for = count_sub(ny.data, ny.len, "for(") + count_sub(ny.data, ny.len, "for (");
  out->ny_if = count_sub(ny.data, ny.len, "if(") + count_sub(ny.data, ny.len, "if (");
  out->ny_get = count_sub(ny.data, ny.len, "get(");
  out->ny_set_idx = count_sub(ny.data, ny.len, "set_idx(");
  out->ny_load8 = count_sub(ny.data, ny.len, "load8(");
  out->ny_mut = count_word_call(ny.data, ny.len, "mut");
  out->ny_def = count_word_call(ny.data, ny.len, "def");
  out->ny_literal_int_lists = literal_lists;
  out->ny_def_int_list_bindings = def_lists;
  out->ny_mut_int_list_bindings = mut_lists;
  out->ny_static_list_elide_candidates = def_lists;
  out->static_list_elide_candidate = def_lists;
  out->ny_fixed_int_list_mutation_candidates = has_set_idx ? mut_lists : 0;
  out->fixed_int_list_mutation_candidate = has_set_idx ? mut_lists : 0;
  free(c.data);
  free(ny.data);
  return true;
}

void print_source_counts_json(FILE *out, const source_counts_t *c) {
  fprintf(out,
          "{\"c_for\":%d,\"c_if\":%d,\"c_array_reads\":%d,\"c_array_writes\":%d,"
          "\"c_declared_arrays\":%d,\"c_readonly_arrays\":%d,"
          "\"ny_for\":%d,\"ny_if\":%d,\"ny_get\":%d,\"ny_set_idx\":%d,\"ny_load8\":%d,"
          "\"ny_mut\":%d,\"ny_def\":%d,\"ny_literal_int_lists\":%d,"
          "\"ny_def_int_list_bindings\":%d,\"ny_mut_int_list_bindings\":%d,"
          "\"ny_static_list_elide_candidates\":%d,\"static_list_elide_candidate\":%d,"
          "\"ny_fixed_int_list_mutation_candidates\":%d,\"fixed_int_list_mutation_candidate\":%d}",
          c->c_for, c->c_if, c->c_array_reads, c->c_array_writes,
          c->c_declared_arrays, c->c_readonly_arrays, c->ny_for, c->ny_if,
          c->ny_get, c->ny_set_idx, c->ny_load8, c->ny_mut, c->ny_def,
          c->ny_literal_int_lists, c->ny_def_int_list_bindings,
          c->ny_mut_int_list_bindings, c->ny_static_list_elide_candidates,
          c->static_list_elide_candidate, c->ny_fixed_int_list_mutation_candidates,
          c->fixed_int_list_mutation_candidate);
}

int cmd_source_shape_counts(const char *c_path, const char *ny_path) {
  source_counts_t counts;
  if (!compute_source_shape_counts(c_path, ny_path, &counts)) {
    printf("{\"ok\":false,\"error\":\"read-failed\"}\n");
    return 1;
  }
  printf("{\"ok\":true,\"counts\":");
  print_source_counts_json(stdout, &counts);
  printf("}\n");
  return 0;
}

typedef struct {
  int files;
  int errors;
  int typed;
  int optimizer;
  int torture;
  int stress;
  int program;
  int error;
  int fixtures;
  int embedded_sources;
  char first_error[512];
} shape_audit_t;

static void audit_append_text(char *dst, size_t cap, const char *text) {
  if (!dst || !cap || !text) return;
  size_t used = strlen(dst);
  if (used >= cap - 1) return;
  size_t n = strlen(text);
  if (n > cap - 1 - used) n = cap - 1 - used;
  memcpy(dst + used, text, n);
  dst[used + n] = '\0';
}

static void audit_error(shape_audit_t *audit, const char *path, const char *reason) {
  ++audit->errors;
  if (!audit->first_error[0]) {
    audit_append_text(audit->first_error, sizeof(audit->first_error), path ? path : "");
    audit_append_text(audit->first_error, sizeof(audit->first_error), ": ");
    audit_append_text(audit->first_error, sizeof(audit->first_error), reason ? reason : "");
  }
}

static void audit_error_key(shape_audit_t *audit, const char *path,
                            const char *prefix, const char *key) {
  char reason[160];
  snprintf(reason, sizeof(reason), "%s-%s", prefix, key ? key : "");
  audit_error(audit, path, reason);
}

static const char *audit_key_after(const char *data, const char *key) {
  if (!data || !key || !*key) return NULL;
  size_t n = strlen(key);
  const char *p = data;
  while ((p = strstr(p, key)) != NULL) {
    bool before_ok = p == data || !ident_char(p[-1]);
    bool after_ok = !ident_char(p[n]);
    if (before_ok && after_ok) return p + n;
    p += n;
  }
  return NULL;
}

static bool audit_parse_range_strict(const char *block, const char *key,
                                     int *lo_out, int *hi_out, bool *present) {
  if (present) *present = false;
  const char *p = audit_key_after(block, key);
  if (!p) return true;
  if (present) *present = true;
  while (*p == ' ' || *p == '\t' || *p == ':') ++p;
  if (*p == '-') return false;
  char *end = NULL;
  long lo = strtol(p, &end, 10);
  if (end == p) return false;
  long hi = lo;
  p = end;
  while (*p == ' ' || *p == '\t') ++p;
  if (p[0] == '.' && p[1] == '.') {
    p += 2;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '-') return false;
    hi = strtol(p, &end, 10);
    if (end == p) return false;
    p = end;
  }
  if (lo < 0 || hi < lo || hi > 10000) return false;
  if (*p && !isspace((unsigned char)*p) && *p != '}') return false;
  if (lo_out) *lo_out = (int)lo;
  if (hi_out) *hi_out = (int)hi;
  return true;
}

static bool audit_parse_chance_strict(const char *block, const char *key,
                                      int *value_out, bool *present) {
  if (present) *present = false;
  const char *p = audit_key_after(block, key);
  if (!p) return true;
  if (present) *present = true;
  while (*p == ' ' || *p == '\t' || *p == ':') ++p;
  if (*p == '-') return false;
  char *end = NULL;
  long v = strtol(p, &end, 10);
  if (end == p || v < 0 || v > 100) return false;
  if (*end && !isspace((unsigned char)*end) && *end != '}') return false;
  if (value_out) *value_out = (int)v;
  return true;
}

static bool audit_value_after(const char *block, const char *key,
                              char *out, size_t out_sz, bool *present) {
  if (present) *present = false;
  if (!out_sz) return false;
  out[0] = '\0';
  const char *p = audit_key_after(block, key);
  if (!p) return true;
  if (present) *present = true;
  while (*p == ' ' || *p == '\t' || *p == ':') ++p;
  const char *q = p;
  if (*p == '"') {
    ++p;
    q = strchr(p, '"');
    if (!q) return false;
  } else {
    while (*q && !isspace((unsigned char)*q) && *q != '}') ++q;
  }
  size_t n = (size_t)(q - p);
  if (!n) return false;
  if (n >= out_sz) n = out_sz - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

static bool audit_scalar_widths_ok(const char *value) {
  return value && (strstr(value, "u8") || strstr(value, "uint8") ||
                   strstr(value, "u16") || strstr(value, "uint16") ||
                   strstr(value, "u32") || strstr(value, "uint32") ||
                   strstr(value, "u64") || strstr(value, "uint64"));
}

static bool audit_statement_mix_ok(const char *value) {
  return value &&
         (strcmp(value, "balanced") == 0 || strcmp(value, "branchy") == 0 ||
          strcmp(value, "memory") == 0 || strcmp(value, "callheavy") == 0 ||
          strcmp(value, "call-heavy") == 0);
}

static bool audit_helper_mix_ok(const char *value) {
  return value &&
         (strcmp(value, "basic") == 0 || strcmp(value, "rich") == 0 ||
          strcmp(value, "scramble") == 0 || strcmp(value, "extended") == 0);
}

typedef struct {
  const char *key;
  int min_lo;
  int max_hi;
} audit_program_range_spec_t;

static void audit_program_shape(const char *path, shape_audit_t *audit, const char *data) {
  const char *block = strstr(data, "synth {");
  if (!block) {
    audit_error(audit, path, "missing-program-synth-block");
    return;
  }
  static const audit_program_range_spec_t ranges[] = {
    {"globals", 1, 16},
    {"arrays", 0, 8},
    {"structs", 0, 6},
    {"functions", 1, 10},
    {"statements", 1, 400},
    {"expression_depth", 1, 6},
    {"loop_depth", 1, 5},
    {"locals", 2, 16},
    {"params", 1, 4},
    {"array_dims", 1, 2},
  };
  for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
    int lo = 0, hi = 0;
    bool present = false;
    if (!audit_parse_range_strict(block, ranges[i].key, &lo, &hi, &present)) {
      audit_error_key(audit, path, "invalid-program-range", ranges[i].key);
    } else if (present && (lo < ranges[i].min_lo || hi > ranges[i].max_hi)) {
      audit_error_key(audit, path, "program-range-out-of-bounds", ranges[i].key);
    }
  }
  const char *chances[] = {"call_chance", "switch_chance", "pointer_chance"};
  for (size_t i = 0; i < sizeof(chances) / sizeof(chances[0]); ++i) {
    int value = 0;
    bool present = false;
    if (!audit_parse_chance_strict(block, chances[i], &value, &present))
      audit_error_key(audit, path, "invalid-program-chance", chances[i]);
  }
  char value[128];
  bool present = false;
  if (!audit_value_after(block, "scalar_widths", value, sizeof(value), &present)) {
    audit_error(audit, path, "invalid-program-scalar-widths");
  } else if (present && !audit_scalar_widths_ok(value)) {
    audit_error(audit, path, "unsupported-program-scalar-widths");
  }
  if (!audit_value_after(block, "statement_mix", value, sizeof(value), &present)) {
    audit_error(audit, path, "invalid-program-statement-mix");
  } else if (present && !audit_statement_mix_ok(value)) {
    audit_error(audit, path, "unsupported-program-statement-mix");
  }
  if (!audit_value_after(block, "helper_mix", value, sizeof(value), &present)) {
    audit_error(audit, path, "invalid-program-helper-mix");
  } else if (present && !audit_helper_mix_ok(value)) {
    audit_error(audit, path, "unsupported-program-helper-mix");
  }
}

static bool audit_rel_path_safe(const char *rel) {
  if (!rel || !*rel) return false;
  if (rel[0] == '/') return true;
  if (strncmp(rel, "../", 3) == 0 || strcmp(rel, "..") == 0) return false;
  return strstr(rel, "/../") == NULL;
}

static bool audit_path_exists(const char *shape_dir, const char *rel) {
  char full[4096];
  const char *path = rel;
  if (!rel || !*rel) return false;
  if (rel[0] != '/') {
    int n = snprintf(full, sizeof(full), "%s/%s", shape_dir ? shape_dir : ".", rel);
    if (n <= 0 || (size_t)n >= sizeof(full)) return false;
    path = full;
  }
  struct stat st;
  return stat(path, &st) == 0 && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode));
}

static void audit_shape_fixtures(const char *shape_dir, const char *path,
                                 shape_audit_t *audit, const char *data) {
  const char *keys[] = {"fixture", "fixtures"};
  for (size_t ki = 0; ki < sizeof(keys) / sizeof(keys[0]); ++ki) {
    const char *key = keys[ki];
    size_t key_len = strlen(key);
    const char *p = data;
    while ((p = strstr(p, key)) != NULL) {
      bool before_ok = p == data || !ident_char(p[-1]);
      bool after_ok = !ident_char(p[key_len]);
      if (!before_ok || !after_ok) {
        p += key_len;
        continue;
      }
      const char *q = p + key_len;
      if (*q != ' ' && *q != '\t' && *q != ':') {
        p += key_len;
        continue;
      }
      while (*q == ' ' || *q == '\t' || *q == ':') ++q;
      if (*q != '"') {
        audit_error_key(audit, path, "invalid-fixture", key);
        p = q;
        continue;
      }
      ++q;
      const char *end = strchr(q, '"');
      if (!end || end == q) {
        audit_error_key(audit, path, "invalid-fixture", key);
        p = q;
        continue;
      }
      char rel[512];
      size_t n = (size_t)(end - q);
      if (n >= sizeof(rel)) n = sizeof(rel) - 1;
      memcpy(rel, q, n);
      rel[n] = '\0';
      ++audit->fixtures;
      if (!audit_rel_path_safe(rel)) {
        audit_error_key(audit, path, "unsafe-fixture", key);
      } else if (!audit_path_exists(shape_dir, rel)) {
        audit_error_key(audit, path, "missing-fixture", key);
      }
      p = end + 1;
    }
  }
}

static int audit_count_source_blocks(const char *data) {
  int count = 0;
  const char *p = data;
  while (p && *p) {
    const char *line = p;
    while (*line == ' ' || *line == '\t') ++line;
    if (strncmp(line, "source ", 7) == 0) ++count;
    p = strchr(p, '\n');
    if (p) ++p;
  }
  return count;
}

static void audit_shape_file(const char *shape_dir, const char *path, shape_audit_t *audit) {
  file_buf_t f = {0};
  ++audit->files;
  if (!read_file(path, &f)) {
    audit_error(audit, path, "read-failed");
    return;
  }
  if (!strstr(f.data, "shape ")) audit_error(audit, path, "missing-shape");
  if (!strstr(f.data, "generator ")) audit_error(audit, path, "missing-generator");
  if (strstr(f.data, "generator \"typed\"")) ++audit->typed;
  if (strstr(f.data, "generator \"optimizer\"")) ++audit->optimizer;
  if (strstr(f.data, "generator \"torture\"")) ++audit->torture;
  if (strstr(f.data, "generator \"stress\"")) ++audit->stress;
  if (strstr(f.data, "generator \"error\"")) ++audit->error;
  if (strstr(f.data, "generator \"program\"")) {
    ++audit->program;
    audit_program_shape(path, audit, f.data);
  }
  audit->embedded_sources += audit_count_source_blocks(f.data);
  audit_shape_fixtures(shape_dir, path, audit, f.data);
  free(f.data);
}

static void walk_shapes(const char *shape_dir, const char *dir, shape_audit_t *audit) {
  DIR *d = opendir(dir);
  if (!d) {
    audit_error(audit, dir, "open-dir-failed");
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
      audit_error(audit, dir, "path-too-long");
      continue;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
      audit_error(audit, path, "stat-failed");
      continue;
    }
    if (S_ISDIR(st.st_mode)) walk_shapes(shape_dir, path, audit);
    else if (S_ISREG(st.st_mode) && ny_has_suffix(path, ".nshape")) audit_shape_file(shape_dir, path, audit);
  }
  closedir(d);
}

int validate_shapes_emit_json(const char *dir, FILE *out, int *count, int *errors,
                              int *typed, int *optimizer, int *torture, int *stress,
                              int *program) {
  shape_audit_t audit;
  memset(&audit, 0, sizeof(audit));
  walk_shapes(dir, dir, &audit);
  fprintf(out, "{\"ok\":%s,\"shape_dir\":", audit.errors ? "false" : "true");
  json_str(out, dir);
  fprintf(out, ",\"count\":%d,\"errors\":%d,\"fixtures\":%d,\"embedded_sources\":%d,"
          "\"generators\":{\"typed\":%d,\"optimizer\":%d,\"torture\":%d,\"stress\":%d,\"program\":%d,\"error\":%d}",
          audit.files, audit.errors, audit.fixtures, audit.embedded_sources, audit.typed, audit.optimizer,
          audit.torture, audit.stress, audit.program, audit.error);
  if (audit.first_error[0]) {
    fprintf(out, ",\"first_error\":");
    json_str(out, audit.first_error);
  }
  fprintf(out, "}\n");
  if (count) *count = audit.files;
  if (errors) *errors = audit.errors;
  if (typed) *typed = audit.typed;
  if (optimizer) *optimizer = audit.optimizer;
  if (torture) *torture = audit.torture;
  if (stress) *stress = audit.stress;
  if (program) *program = audit.program;
  return audit.errors ? 1 : 0;
}

int cmd_validate_shapes(const char *dir) {
  return validate_shapes_emit_json(dir, stdout, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}
