#include "cbridge.h"
#include "base/util.h"

#include <stdarg.h>

typedef struct {
  char names[256][96];
  int count;
} cbridge_name_set_t;

typedef struct {
  char *items[256];
  int count;
} cbridge_stack_t;

typedef struct {
  str_buf_t out;
  bool last_blank;
  cbridge_name_set_t string_vars;
  cbridge_name_set_t readonly_arrays;
  cbridge_name_set_t declared_loop_vars;
  cbridge_stack_t block_stack;
  const char *source_path;
  int current_line;
  int error_line;
  char error_category[64];
  char error[512];
  const char *features[32];
  int feature_count;
} cbridge_state_t;

static char *xstrndup(const char *s, size_t n) {
  char *out = (char *)malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static char *xstrdup(const char *s) {
  return xstrndup(s ? s : "", strlen(s ? s : ""));
}

static bool c_ident_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool c_ident_char(char c) {
  return c_ident_start(c) || (c >= '0' && c <= '9');
}

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  return p;
}

static void rstrip_in_place(char *s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
    s[--n] = '\0';
  }
}

static char *trim_dup_n(const char *s, size_t n) {
  while (n && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
    ++s;
    --n;
  }
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) --n;
  return xstrndup(s, n);
}

static char *trim_dup(const char *s) {
  return trim_dup_n(s, strlen(s));
}

static bool starts_word(const char *s, const char *word) {
  size_t n = strlen(word);
  if (strlen(s) < n) return false;
  return memcmp(s, word, n) == 0 && !c_ident_char(s[n]);
}

static bool consume_word(const char **p, const char *word) {
  const char *q = skip_ws(*p);
  if (!starts_word(q, word)) return false;
  *p = q + strlen(word);
  return true;
}

static bool parse_identifier(const char **p, char *out, size_t out_sz) {
  const char *q = skip_ws(*p);
  if (!c_ident_start(*q)) return false;
  const char *start = q++;
  while (c_ident_char(*q)) ++q;
  size_t n = (size_t)(q - start);
  if (n >= out_sz) return false;
  memcpy(out, start, n);
  out[n] = '\0';
  *p = q;
  return true;
}

static bool set_contains(const cbridge_name_set_t *set, const char *name) {
  for (int i = 0; i < set->count; ++i) {
    if (strcmp(set->names[i], name) == 0) return true;
  }
  return false;
}

static void set_add(cbridge_name_set_t *set, const char *name) {
  if (!name || !*name || strlen(name) >= sizeof(set->names[0]) || set_contains(set, name)) return;
  if (set->count >= (int)(sizeof(set->names) / sizeof(set->names[0]))) return;
  snprintf(set->names[set->count++], sizeof(set->names[0]), "%s", name);
}

static void set_add_n(cbridge_name_set_t *set, const char *name, size_t len) {
  if (!len || len >= sizeof(set->names[0])) return;
  char tmp[96];
  memcpy(tmp, name, len);
  tmp[len] = '\0';
  set_add(set, tmp);
}

static void set_error_v(cbridge_state_t *st, const char *category, const char *fmt, va_list ap) {
  if (st->error[0]) return;
  st->error_line = st->current_line;
  snprintf(st->error_category, sizeof(st->error_category), "%s",
           category && *category ? category : "unsupported");
  vsnprintf(st->error, sizeof(st->error), fmt, ap);
}

static void set_error_category(cbridge_state_t *st, const char *category, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  set_error_v(st, category, fmt, ap);
  va_end(ap);
}

static void set_error(cbridge_state_t *st, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  set_error_v(st, "unsupported", fmt, ap);
  va_end(ap);
}

static void feature_add(cbridge_state_t *st, const char *name) {
  for (int i = 0; i < st->feature_count; ++i) {
    if (strcmp(st->features[i], name) == 0) return;
  }
  if (st->feature_count < (int)(sizeof(st->features) / sizeof(st->features[0])))
    st->features[st->feature_count++] = name;
}

static int feature_cmp(const void *a, const void *b) {
  const char *const *sa = (const char *const *)a;
  const char *const *sb = (const char *const *)b;
  return strcmp(*sa, *sb);
}

static void stack_push(cbridge_state_t *st, char *update) {
  if (st->block_stack.count >= (int)(sizeof(st->block_stack.items) / sizeof(st->block_stack.items[0]))) {
    free(update);
    set_error(st, "block stack overflow");
    return;
  }
  st->block_stack.items[st->block_stack.count++] = update;
}

static char *stack_pop(cbridge_state_t *st) {
  if (st->block_stack.count <= 0) return NULL;
  return st->block_stack.items[--st->block_stack.count];
}

static char *nearest_loop_update(cbridge_state_t *st) {
  for (int i = st->block_stack.count - 1; i >= 0; --i) {
    if (st->block_stack.items[i]) return st->block_stack.items[i];
  }
  return NULL;
}

static void append_line(cbridge_state_t *st, const char *line) {
  if (!line || !*line) {
    if (!st->last_blank) {
      (void)sb_append_c(&st->out, '\n');
      st->last_blank = true;
    }
    return;
  }
  (void)sb_append(&st->out, line);
  (void)sb_append_c(&st->out, '\n');
  st->last_blank = false;
}

static void append_text_lines(cbridge_state_t *st, const char *text) {
  const char *start = text;
  for (const char *p = text; ; ++p) {
    if (*p != '\n' && *p != '\0') continue;
    char *line = trim_dup_n(start, (size_t)(p - start));
    append_line(st, line);
    free(line);
    if (*p == '\0') break;
    start = p + 1;
  }
}

static char *finish_source(cbridge_state_t *st) {
  while (st->out.len && (st->out.data[st->out.len - 1] == '\n' ||
                         st->out.data[st->out.len - 1] == '\r' ||
                         st->out.data[st->out.len - 1] == ' ' ||
                         st->out.data[st->out.len - 1] == '\t')) {
    st->out.data[--st->out.len] = '\0';
  }
  (void)sb_append_c(&st->out, '\n');
  return sb_take(&st->out);
}

static void state_free(cbridge_state_t *st) {
  free(st->out.data);
  for (int i = 0; i < st->block_stack.count; ++i) free(st->block_stack.items[i]);
}

static bool parse_type(const char **p, char *out, size_t out_sz, bool allow_void) {
  const char *q = skip_ws(*p);
  str_buf_t b = {0};
  if (consume_word(&q, "const")) (void)sb_append(&b, "const ");
  bool signed_kw = consume_word(&q, "signed");
  bool unsigned_kw = consume_word(&q, "unsigned");
  if (allow_void && consume_word(&q, "void")) {
    (void)sb_append(&b, "void");
    goto ok;
  }
  const char *fixed_types[] = {
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "intptr_t", "uintptr_t", "size_t", "ssize_t"
  };
  for (size_t i = 0; i < sizeof(fixed_types) / sizeof(fixed_types[0]); ++i) {
    if (consume_word(&q, fixed_types[i])) {
      (void)sb_append(&b, fixed_types[i]);
      goto ok;
    }
  }
  if (consume_word(&q, "short")) {
    if (unsigned_kw) (void)sb_append(&b, "unsigned ");
    else if (signed_kw) (void)sb_append(&b, "signed ");
    (void)sb_append(&b, "short");
    goto ok;
  }
  if (consume_word(&q, "long")) {
    if (unsigned_kw) (void)sb_append(&b, "unsigned ");
    else if (signed_kw) (void)sb_append(&b, "signed ");
    if (consume_word(&q, "long")) (void)sb_append(&b, "long long");
    else (void)sb_append(&b, "long");
    goto ok;
  }
  if (consume_word(&q, "int")) {
    if (unsigned_kw) (void)sb_append(&b, "unsigned ");
    else if (signed_kw) (void)sb_append(&b, "signed ");
    (void)sb_append(&b, "int");
    goto ok;
  }
  if (consume_word(&q, "char")) {
    if (unsigned_kw) (void)sb_append(&b, "unsigned ");
    else if (signed_kw) (void)sb_append(&b, "signed ");
    (void)sb_append(&b, "char");
    goto ok;
  }
  if (consume_word(&q, "bool")) {
    (void)sb_append(&b, "bool");
    goto ok;
  }
  if (unsigned_kw) {
    (void)sb_append(&b, "unsigned int");
    goto ok;
  }
  if (signed_kw) {
    (void)sb_append(&b, "signed int");
    goto ok;
  }
  free(b.data);
  return false;
ok:
  if (!b.data || b.len >= out_sz) {
    free(b.data);
    return false;
  }
  snprintf(out, out_sz, "%s", b.data);
  free(b.data);
  *p = q;
  return true;
}

static const char *map_type_to_ny(const char *type_name) {
  return strstr(type_name, "bool") ? "bool" : "int";
}

static bool is_cast_type_text(const char *s) {
  char *t = trim_dup(s);
  const char *p = t;
  char type[64];
  bool ok = parse_type(&p, type, sizeof(type), true) && *skip_ws(p) == '\0';
  free(t);
  return ok;
}

static char *convert_char_literals(const char *expr) {
  str_buf_t out = {0};
  for (size_t i = 0; expr[i]; ++i) {
    if (expr[i] != '\'') {
      (void)sb_append_c(&out, expr[i]);
      continue;
    }
    ++i;
    int value = 0;
    if (expr[i] == '\\') {
      ++i;
      switch (expr[i]) {
      case 'n': value = 10; break;
      case 't': value = 9; break;
      case 'r': value = 13; break;
      case '0': value = 0; break;
      default: value = (unsigned char)expr[i]; break;
      }
    } else {
      value = (unsigned char)expr[i];
    }
    while (expr[i] && expr[i] != '\'') ++i;
    char num[32];
    snprintf(num, sizeof(num), "%d", value);
    (void)sb_append(&out, num);
  }
  return sb_take(&out);
}

static char *remove_casts(const char *expr) {
  str_buf_t out = {0};
  for (size_t i = 0; expr[i]; ) {
    if (expr[i] == '(') {
      const char *close = strchr(expr + i + 1, ')');
      if (close) {
        char *inside = xstrndup(expr + i + 1, (size_t)(close - (expr + i + 1)));
        if (inside && is_cast_type_text(inside)) {
          free(inside);
          i = (size_t)(close - expr) + 1;
          continue;
        }
        free(inside);
      }
    }
    (void)sb_append_c(&out, expr[i++]);
  }
  return sb_take(&out);
}

static bool parse_array_zero_inside(const char *inside, char *name, size_t name_sz) {
  const char *p = skip_ws(inside);
  if (!parse_identifier(&p, name, name_sz)) return false;
  p = skip_ws(p);
  if (*p++ != '[') return false;
  p = skip_ws(p);
  if (*p++ != '0') return false;
  p = skip_ws(p);
  if (*p++ != ']') return false;
  p = skip_ws(p);
  return *p == '\0';
}

static bool parse_identifier_only(const char *inside, char *name, size_t name_sz) {
  const char *p = skip_ws(inside);
  if (!parse_identifier(&p, name, name_sz)) return false;
  p = skip_ws(p);
  return *p == '\0';
}

static bool parse_sizeof_at(const char *s, size_t i, char **inside_out, size_t *end_out) {
  if (strncmp(s + i, "sizeof", 6) != 0 || c_ident_char(i ? s[i - 1] : '\0') || c_ident_char(s[i + 6]))
    return false;
  const char *p = skip_ws(s + i + 6);
  if (*p++ != '(') return false;
  const char *start = p;
  int depth = 1;
  while (*p && depth > 0) {
    if (*p == '(') ++depth;
    else if (*p == ')') --depth;
    if (depth > 0) ++p;
  }
  if (depth != 0) return false;
  *inside_out = trim_dup_n(start, (size_t)(p - start));
  *end_out = (size_t)(p - s) + 1;
  return true;
}

static char *convert_sizeof_expr(const char *expr) {
  str_buf_t out = {0};
  for (size_t i = 0; expr[i]; ) {
    char *inside = NULL;
    size_t end = 0;
    if (!parse_sizeof_at(expr, i, &inside, &end)) {
      (void)sb_append_c(&out, expr[i++]);
      continue;
    }
    char name[96], other[96];
    const char *after = skip_ws(expr + end);
    if (parse_identifier_only(inside, name, sizeof(name)) && *after == '/') {
      const char *after_slash = skip_ws(after + 1);
      char *inside2 = NULL;
      size_t end2_rel = 0;
      if (parse_sizeof_at(after_slash, 0, &inside2, &end2_rel) &&
          parse_array_zero_inside(inside2, other, sizeof(other)) &&
          strcmp(name, other) == 0) {
        (void)sb_append(&out, "len(");
        (void)sb_append(&out, name);
        (void)sb_append_c(&out, ')');
        i = (size_t)(after_slash - expr) + end2_rel;
        free(inside);
        free(inside2);
        continue;
      }
      free(inside2);
    }
    if (parse_array_zero_inside(inside, name, sizeof(name))) {
      (void)sb_append_c(&out, '1');
    } else if (parse_identifier_only(inside, name, sizeof(name))) {
      (void)sb_append(&out, "len(");
      (void)sb_append(&out, name);
      (void)sb_append_c(&out, ')');
    } else {
      (void)sb_append_n(&out, expr + i, end - i);
    }
    free(inside);
    i = end;
  }
  return sb_take(&out);
}

static char *convert_indexes(const char *expr, const cbridge_name_set_t *string_vars) {
  str_buf_t out = {0};
  for (size_t i = 0; expr[i]; ) {
    if (!c_ident_start(expr[i])) {
      (void)sb_append_c(&out, expr[i++]);
      continue;
    }
    size_t start = i++;
    while (c_ident_char(expr[i])) ++i;
    size_t end = i;
    const char *p = skip_ws(expr + i);
    if (*p != '[') {
      if (strncmp(p, ".length", 7) == 0 && !c_ident_char(p[7])) {
        char *name = xstrndup(expr + start, end - start);
        (void)sb_append(&out, "len(");
        (void)sb_append(&out, name);
        (void)sb_append_c(&out, ')');
        free(name);
        i = (size_t)(p - expr) + 7;
      } else {
        (void)sb_append_n(&out, expr + start, end - start);
      }
      continue;
    }
    const char *idx_start = p + 1;
    const char *idx_end = strchr(idx_start, ']');
    if (!idx_end) {
      (void)sb_append_n(&out, expr + start, end - start);
      continue;
    }
    char *name = xstrndup(expr + start, end - start);
    char *idx = xstrndup(idx_start, (size_t)(idx_end - idx_start));
    if (set_contains(string_vars, name)) {
      (void)sb_append(&out, "load8(");
      (void)sb_append(&out, name);
      (void)sb_append(&out, ", ");
      (void)sb_append(&out, idx);
      (void)sb_append_c(&out, ')');
    } else {
      (void)sb_append(&out, "get(");
      (void)sb_append(&out, name);
      (void)sb_append(&out, ", ");
      (void)sb_append(&out, idx);
      (void)sb_append(&out, ", 0)");
    }
    free(name);
    free(idx);
    i = (size_t)(idx_end - expr) + 1;
  }
  return sb_take(&out);
}

static char *convert_expr(const char *expr, const cbridge_name_set_t *string_vars) {
  char *trimmed = trim_dup(expr);
  char *chars = convert_char_literals(trimmed);
  char *casts = remove_casts(chars);
  char *sizes = convert_sizeof_expr(casts);
  char *indexed = convert_indexes(sizes, string_vars);
  free(trimmed);
  free(chars);
  free(casts);
  free(sizes);
  return indexed;
}

static bool line_declares_array_cbridge(const char *line, char *name, size_t name_sz) {
  const char *p = line;
  char type[64];
  if (!parse_type(&p, type, sizeof(type), false)) return false;
  if (!parse_identifier(&p, name, name_sz)) return false;
  p = skip_ws(p);
  return *p == '[';
}

static void collect_written_array_cbridge(const char *line, cbridge_name_set_t *set) {
  const char *br = strchr(line, '[');
  const char *rb = br ? strchr(br, ']') : NULL;
  if (!br || !rb) return;
  const char *p = skip_ws(rb + 1);
  bool writes = false;
  if (*p == '=' && p[1] != '=') writes = true;
  if ((p[0] == '+' || p[0] == '-' || p[0] == '*' || p[0] == '/' || p[0] == '%' ||
       p[0] == '&' || p[0] == '|' || p[0] == '^') && p[1] == '=') writes = true;
  if ((p[0] == '+' && p[1] == '+') || (p[0] == '-' && p[1] == '-')) writes = true;
  if (!writes) return;
  const char *end = br;
  while (end > line && (end[-1] == ' ' || end[-1] == '\t')) --end;
  const char *start = end;
  while (start > line && c_ident_char(start[-1])) --start;
  set_add_n(set, start, (size_t)(end - start));
}

static void collect_readonly_arrays(const char *text, cbridge_name_set_t *readonly) {
  cbridge_name_set_t declared = {0}, written = {0};
  size_t start = 0;
  for (size_t i = 0; ; ++i) {
    if (text[i] != '\n' && text[i] != '\0') continue;
    char *line = xstrndup(text + start, i - start);
    if (!line) break;
    char *comment = strstr(line, "//");
    if (comment) *comment = '\0';
    rstrip_in_place(line);
    char name[96];
    if (line_declares_array_cbridge(skip_ws(line), name, sizeof(name))) {
      set_add(&declared, name);
    } else {
      collect_written_array_cbridge(line, &written);
    }
    free(line);
    if (text[i] == '\0') break;
    start = i + 1;
  }
  for (int i = 0; i < declared.count; ++i) {
    if (!set_contains(&written, declared.names[i])) set_add(readonly, declared.names[i]);
  }
}

static char *typed_decl_line(const char *keyword, const char *type, const char *name, const char *value) {
  char *out = NULL;
  (void)asprintf(&out, "%s %s: %s = %s", keyword, map_type_to_ny(type), name, value);
  return out;
}

static char *convert_csv_values(const char *values, const cbridge_name_set_t *string_vars) {
  str_buf_t out = {0};
  const char *start = values;
  bool first = true;
  for (const char *p = values; ; ++p) {
    if (*p != ',' && *p != '\0') continue;
    char *part = trim_dup_n(start, (size_t)(p - start));
    if (part && *part) {
      char *expr = convert_expr(part, string_vars);
      if (!first) (void)sb_append(&out, ", ");
      first = false;
      (void)sb_append(&out, expr);
      free(expr);
    }
    free(part);
    if (*p == '\0') break;
    start = p + 1;
  }
  return sb_take(&out);
}

static char *convert_decl(cbridge_state_t *st, const char *line) {
  const char *p = line;
  if (consume_word(&p, "const") && consume_word(&p, "char")) {
    p = skip_ws(p);
    if (*p == '*') {
      ++p;
      char name[96];
      if (parse_identifier(&p, name, sizeof(name))) {
        p = skip_ws(p);
        if (*p == '=') {
          ++p;
          const char *value_start = skip_ws(p);
          const char *semi = strrchr(value_start, ';');
          if (semi) {
            char *value = trim_dup_n(value_start, (size_t)(semi - value_start));
            set_add(&st->string_vars, name);
            char *out = NULL;
            (void)asprintf(&out, "def %s = %s", name, value);
            free(value);
            return out;
          }
        }
      }
    }
  }

  p = line;
  char type[64], name[96];
  if (!parse_type(&p, type, sizeof(type), false)) return NULL;
  if (!parse_identifier(&p, name, sizeof(name))) return NULL;
  p = skip_ws(p);
  if (*p == '[') {
    const char *rb = strchr(p, ']');
    if (!rb) return NULL;
    p = skip_ws(rb + 1);
    if (*p++ != '=') return NULL;
    p = skip_ws(p);
    if (*p++ != '{') return NULL;
    const char *end = strrchr(p, '}');
    if (!end) return NULL;
    char *values = trim_dup_n(p, (size_t)(end - p));
    char *items = convert_csv_values(values, &st->string_vars);
    const char *keyword = set_contains(&st->readonly_arrays, name) ? "def" : "mut";
    char *out = NULL;
    (void)asprintf(&out, "%s %s = [%s]", keyword, name, items);
    free(values);
    free(items);
    return out;
  }
  if (*p == '=') {
    ++p;
    const char *semi = strrchr(p, ';');
    if (!semi) return NULL;
    char *raw = trim_dup_n(p, (size_t)(semi - p));
    char *expr = convert_expr(raw, &st->string_vars);
    char *out = typed_decl_line(strstr(type, "const") ? "def" : "mut", type, name, expr);
    free(raw);
    free(expr);
    return out;
  }
  if (*p == ';') {
    const char *zero = strcmp(map_type_to_ny(type), "bool") == 0 ? "false" : "0";
    return typed_decl_line("mut", type, name, zero);
  }
  return NULL;
}

static char *convert_multi_decl(cbridge_state_t *st, const char *line) {
  const char *p = line;
  char type[64];
  if (!parse_type(&p, type, sizeof(type), false)) return NULL;
  const char *tail = skip_ws(p);
  if (strchr(tail, '[') || strchr(tail, '*')) return NULL;
  const char *semi = strrchr(tail, ';');
  if (!semi) return NULL;
  char *body = trim_dup_n(tail, (size_t)(semi - tail));
  int parts = 1;
  for (char *q = body; *q; ++q) if (*q == ',') ++parts;
  if (parts < 2) {
    free(body);
    return NULL;
  }
  str_buf_t out = {0};
  char *start = body;
  for (char *q = body; ; ++q) {
    if (*q != ',' && *q != '\0') continue;
    char save = *q;
    *q = '\0';
    char *part = trim_dup(start);
    char *eq = strchr(part, '=');
    char name[96];
    char *line_out = NULL;
    if (eq) {
      *eq = '\0';
      char *name_trim = trim_dup(part);
      char *value_trim = trim_dup(eq + 1);
      const char *np = name_trim;
      if (!parse_identifier(&np, name, sizeof(name)) || *skip_ws(np) != '\0') {
        free(name_trim); free(value_trim); free(part); free(body); free(out.data);
        return NULL;
      }
      char *expr = convert_expr(value_trim, &st->string_vars);
      line_out = typed_decl_line(strstr(type, "const") ? "def" : "mut", type, name, expr);
      free(expr);
      free(name_trim);
      free(value_trim);
    } else {
      const char *np = part;
      if (!parse_identifier(&np, name, sizeof(name)) || *skip_ws(np) != '\0') {
        free(part); free(body); free(out.data);
        return NULL;
      }
      line_out = typed_decl_line("mut", type, name, strcmp(map_type_to_ny(type), "bool") == 0 ? "false" : "0");
    }
    if (out.len) (void)sb_append_c(&out, '\n');
    (void)sb_append(&out, line_out);
    free(line_out);
    free(part);
    if (save == '\0') break;
    start = q + 1;
  }
  free(body);
  return sb_take(&out);
}

static char *convert_function_header(cbridge_state_t *st, const char *line) {
  const char *p = line;
  (void)consume_word(&p, "static");
  char ret_type[64], name[96];
  if (!parse_type(&p, ret_type, sizeof(ret_type), true)) return NULL;
  if (!parse_identifier(&p, name, sizeof(name))) return NULL;
  p = skip_ws(p);
  if (*p++ != '(') return NULL;
  const char *params_start = p;
  const char *close = strchr(params_start, ')');
  if (!close) return NULL;
  p = skip_ws(close + 1);
  if (*p++ != '{' || *skip_ws(p) != '\0') return NULL;
  char *params_text = trim_dup_n(params_start, (size_t)(close - params_start));
  str_buf_t params = {0};
  if (*params_text && strcmp(params_text, "void") != 0) {
    char *start = params_text;
    bool first = true;
    for (char *q = params_text; ; ++q) {
      if (*q != ',' && *q != '\0') continue;
      char save = *q;
      *q = '\0';
      char *raw = trim_dup(start);
      const char *rp = raw;
      if (consume_word(&rp, "const") && consume_word(&rp, "char")) {
        rp = skip_ws(rp);
        if (*rp++ != '*') {
          set_error(st, "unsupported parameter syntax: %s", raw);
          free(raw); free(params_text); free(params.data);
          return NULL;
        }
        char pname[96];
        if (!parse_identifier(&rp, pname, sizeof(pname)) || *skip_ws(rp) != '\0') {
          set_error(st, "unsupported parameter syntax: %s", raw);
          free(raw); free(params_text); free(params.data);
          return NULL;
        }
        set_add(&st->string_vars, pname);
        if (!first) (void)sb_append(&params, ", ");
        first = false;
        (void)sb_append(&params, "str: ");
        (void)sb_append(&params, pname);
      } else {
        rp = raw;
        char ptype[64], pname[96];
        if (!parse_type(&rp, ptype, sizeof(ptype), false) ||
            !parse_identifier(&rp, pname, sizeof(pname)) || *skip_ws(rp) != '\0') {
          set_error(st, "unsupported parameter syntax: %s", raw);
          free(raw); free(params_text); free(params.data);
          return NULL;
        }
        if (!first) (void)sb_append(&params, ", ");
        first = false;
        (void)sb_append(&params, map_type_to_ny(ptype));
        (void)sb_append(&params, ": ");
        (void)sb_append(&params, pname);
      }
      free(raw);
      if (save == '\0') break;
      start = q + 1;
    }
  }
  char *out = NULL;
  if (strcmp(ret_type, "void") == 0) {
    (void)asprintf(&out, "fn %s(%s) {", name, params.data ? params.data : "");
  } else {
    (void)asprintf(&out, "fn %s(%s) %s {", name, params.data ? params.data : "", map_type_to_ny(ret_type));
  }
  free(params.data);
  free(params_text);
  return out;
}

static bool split_for_header(const char *line, char **a, char **b, char **c) {
  const char *p = skip_ws(line);
  if (!consume_word(&p, "for")) return false;
  p = skip_ws(p);
  if (*p++ != '(') return false;
  const char *header_start = p;
  const char *close = strrchr(header_start, ')');
  if (!close) return false;
  const char *after = skip_ws(close + 1);
  if (*after++ != '{' || *skip_ws(after) != '\0') return false;
  const char *s1 = memchr(header_start, ';', (size_t)(close - header_start));
  if (!s1) return false;
  const char *s2 = memchr(s1 + 1, ';', (size_t)(close - s1 - 1));
  if (!s2) return false;
  *a = trim_dup_n(header_start, (size_t)(s1 - header_start));
  *b = trim_dup_n(s1 + 1, (size_t)(s2 - s1 - 1));
  *c = trim_dup_n(s2 + 1, (size_t)(close - s2 - 1));
  return true;
}

static char *convert_for(cbridge_state_t *st, const char *line, char **update_out) {
  char *init_part = NULL, *cond_part = NULL, *inc_part = NULL;
  if (!split_for_header(line, &init_part, &cond_part, &inc_part)) return NULL;
  const char *p = init_part;
  char type[64] = "";
  bool has_decl = parse_type(&p, type, sizeof(type), false);
  char var[96];
  if (!parse_identifier(&p, var, sizeof(var))) goto unsupported;
  p = skip_ws(p);
  if (*p++ != '=') goto unsupported;
  char *init_expr = convert_expr(p, &st->string_vars);

  p = cond_part;
  char cond_var[96];
  if (!parse_identifier(&p, cond_var, sizeof(cond_var)) || strcmp(cond_var, var) != 0) {
    free(init_expr);
    set_error(st, "for-loop condition var mismatch");
    goto fail;
  }
  p = skip_ws(p);
  const char *op = NULL;
  if (strncmp(p, "<=", 2) == 0 || strncmp(p, ">=", 2) == 0 || strncmp(p, "!=", 2) == 0) {
    op = xstrndup(p, 2);
    p += 2;
  } else if (*p == '<' || *p == '>') {
    op = xstrndup(p, 1);
    p += 1;
  } else {
    free(init_expr);
    goto unsupported;
  }
  char *limit_expr = convert_expr(p, &st->string_vars);

  p = inc_part;
  char inc_name[96];
  char *step_expr = NULL;
  const char *update_op = "+=";
  if (p[0] == '+' && p[1] == '+') {
    p += 2;
    if (!parse_identifier(&p, inc_name, sizeof(inc_name))) goto for_bad_inc;
    step_expr = xstrdup("1");
  } else if (p[0] == '-' && p[1] == '-') {
    p += 2;
    if (!parse_identifier(&p, inc_name, sizeof(inc_name))) goto for_bad_inc;
    update_op = "-=";
    step_expr = xstrdup("1");
  } else if (parse_identifier(&p, inc_name, sizeof(inc_name))) {
    p = skip_ws(p);
    if (p[0] == '+' && p[1] == '+') {
      p += 2;
      step_expr = xstrdup("1");
    } else if (p[0] == '-' && p[1] == '-') {
      p += 2;
      update_op = "-=";
      step_expr = xstrdup("1");
    } else if (p[0] == '+' && p[1] == '=') {
      p += 2;
      step_expr = convert_expr(p, &st->string_vars);
    } else if (p[0] == '-' && p[1] == '=') {
      p += 2;
      update_op = "-=";
      step_expr = convert_expr(p, &st->string_vars);
    } else {
      goto for_bad_inc;
    }
  } else {
    goto for_bad_inc;
  }
  if (strcmp(inc_name, var) != 0) {
    set_error(st, "for-loop increment var mismatch");
    goto for_fail;
  }
  (void)asprintf(update_out, "%s %s %s", var, update_op, step_expr);
  char *out = NULL;
  if (has_decl) {
    if (set_contains(&st->declared_loop_vars, var)) {
      (void)asprintf(&out, "mut %s = %s\nwhile(%s%s%s){", var, init_expr, var, op, limit_expr);
    } else {
      set_add(&st->declared_loop_vars, var);
      (void)asprintf(&out, "mut %s: %s = %s\nwhile(%s%s%s){", map_type_to_ny(type), var, init_expr, var, op, limit_expr);
    }
  } else {
    (void)asprintf(&out, "mut %s = %s\nwhile(%s%s%s){", var, init_expr, var, op, limit_expr);
  }
  free(init_expr); free(limit_expr); free((char *)op); free(step_expr);
  free(init_part); free(cond_part); free(inc_part);
  return out;

for_bad_inc:
  set_error(st, "unsupported for-loop increment syntax: %s", inc_part);
for_fail:
  free(init_expr); free(limit_expr); free((char *)op); free(step_expr);
  goto fail;
unsupported:
  set_error(st, "unsupported C line in %s: %s", st->source_path, line);
fail:
  free(init_part); free(cond_part); free(inc_part);
  return NULL;
}

static char *convert_if_while(cbridge_state_t *st, const char *line) {
  (void)st;
  const char *p = skip_ws(line);
  bool leading_close = false;
  if (*p == '}') {
    leading_close = true;
    p = skip_ws(p + 1);
  }
  if (consume_word(&p, "else")) {
    const char *after_else = skip_ws(p);
    if (consume_word(&after_else, "if")) {
      after_else = skip_ws(after_else);
      if (*after_else++ != '(') return NULL;
      const char *cond_start = after_else;
      const char *close = strrchr(cond_start, ')');
      if (!close) return NULL;
      const char *after = skip_ws(close + 1);
      if (*after++ != '{' || *skip_ws(after) != '\0') return NULL;
      char *cond_raw = trim_dup_n(cond_start, (size_t)(close - cond_start));
      char *cond = convert_expr(cond_raw, &st->string_vars);
      char *out = NULL;
      (void)asprintf(&out, leading_close ? "} elif(%s){" : "elif(%s){", cond);
      free(cond_raw); free(cond);
      return out;
    }
    if (*after_else == '{' && *skip_ws(after_else + 1) == '\0') return xstrdup(leading_close ? "} else {" : "else{");
    return NULL;
  }
  if (leading_close) return NULL;
  for (int i = 0; i < 2; ++i) {
    const char *kw = i == 0 ? "if" : "while";
    p = skip_ws(line);
    if (!consume_word(&p, kw)) continue;
    p = skip_ws(p);
    if (*p++ != '(') continue;
    const char *cond_start = p;
    const char *close = strrchr(cond_start, ')');
    if (!close) continue;
    const char *after = skip_ws(close + 1);
    if (*after++ != '{' || *skip_ws(after) != '\0') continue;
    char *cond_raw = trim_dup_n(cond_start, (size_t)(close - cond_start));
    char *cond = convert_expr(cond_raw, &st->string_vars);
    char *out = NULL;
    (void)asprintf(&out, "%s(%s){", kw, cond);
    free(cond_raw); free(cond);
    return out;
  }
  return NULL;
}

static char *convert_break_continue(const char *line) {
  char *t = trim_dup(line);
  char *out = NULL;
  if (strcmp(t, "break;") == 0) out = xstrdup("break");
  else if (strcmp(t, "continue;") == 0) out = xstrdup("continue");
  free(t);
  return out;
}

static char *convert_printf(cbridge_state_t *st, const char *line) {
  const char *p = skip_ws(line);
  if (!consume_word(&p, "printf")) return NULL;
  p = skip_ws(p);
  if (*p++ != '(') return NULL;
  p = skip_ws(p);
  if (*p++ != '"') return NULL;
  const char *fmt_start = p;
  bool escaped = false;
  while (*p) {
    if (!escaped && *p == '"') break;
    escaped = !escaped && *p == '\\';
    if (*p != '\\') escaped = false;
    ++p;
  }
  if (*p != '"') return NULL;
  char *fmt = xstrndup(fmt_start, (size_t)(p - fmt_start));
  p = skip_ws(p + 1);
  char *arg = NULL;
  if (*p == ',') {
    ++p;
    const char *arg_start = skip_ws(p);
    const char *close = strrchr(arg_start, ')');
    if (!close) {
      free(fmt);
      return NULL;
    }
    arg = trim_dup_n(arg_start, (size_t)(close - arg_start));
    p = skip_ws(close + 1);
  } else if (*p == ')') {
    ++p;
  } else {
    free(fmt);
    return NULL;
  }
  if (*p++ != ';' || *skip_ws(p) != '\0') {
    free(fmt); free(arg);
    return NULL;
  }
  char *out = NULL;
  bool int_fmt = strcmp(fmt, "%d\\n") == 0 || strcmp(fmt, "%u\\n") == 0 ||
                 strcmp(fmt, "%ld\\n") == 0 || strcmp(fmt, "%lld\\n") == 0 ||
                 strcmp(fmt, "%c\\n") == 0 || strcmp(fmt, "%s\\n") == 0;
  if (int_fmt && arg && *arg) {
    char *expr = convert_expr(arg, &st->string_vars);
    (void)asprintf(&out, "print(%s)", expr);
    free(expr);
  } else if (strcmp(fmt, "\\n") == 0) {
    out = xstrdup("print(\"\")");
  } else {
    set_error(st, "unsupported printf format: %s", fmt);
  }
  free(fmt);
  free(arg);
  return out;
}

static char *convert_return(cbridge_state_t *st, const char *line) {
  const char *p = skip_ws(line);
  if (!consume_word(&p, "return")) return NULL;
  const char *semi = strrchr(p, ';');
  if (!semi || *skip_ws(semi + 1) != '\0') return NULL;
  char *value = trim_dup_n(p, (size_t)(semi - p));
  char *out = NULL;
  if (!*value) out = xstrdup("return");
  else {
    char *expr = convert_expr(value, &st->string_vars);
    (void)asprintf(&out, "return %s", expr);
    free(expr);
  }
  free(value);
  return out;
}

static char *convert_assignment(cbridge_state_t *st, const char *line) {
  char *t = trim_dup(line);
  size_t n = strlen(t);
  if (n < 2 || t[n - 1] != ';') {
    free(t);
    return NULL;
  }
  t[n - 1] = '\0';
  char *p = t;
  char name[96];
  const char *cp = p;
  if (parse_identifier(&cp, name, sizeof(name))) {
    const char *after = skip_ws(cp);
    if ((strcmp(after, "++") == 0) || (strcmp(after, "--") == 0)) {
      char *out = NULL;
      (void)asprintf(&out, "%s %s 1", name, strcmp(after, "++") == 0 ? "+=" : "-=");
      free(t);
      return out;
    }
  }
  cp = skip_ws(t);
  if ((cp[0] == '+' || cp[0] == '-') && cp[1] == cp[0]) {
    const char *np = cp + 2;
    if (parse_identifier(&np, name, sizeof(name)) && *skip_ws(np) == '\0') {
      char *out = NULL;
      (void)asprintf(&out, "%s %s 1", name, cp[0] == '+' ? "+=" : "-=");
      free(t);
      return out;
    }
  }
  cp = t;
  if (parse_identifier(&cp, name, sizeof(name))) {
    const char *br = skip_ws(cp);
    if (*br == '[') {
      const char *idx_start = br + 1;
      const char *idx_end = strchr(idx_start, ']');
      if (idx_end) {
        const char *after = skip_ws(idx_end + 1);
        bool post_inc = strcmp(after, "++") == 0 || strcmp(after, "--") == 0;
        bool compound = (after[0] == '+' || after[0] == '-' || after[0] == '*' ||
                         after[0] == '/' || after[0] == '%' || after[0] == '&' ||
                         after[0] == '|' || after[0] == '^') && after[1] == '=';
        if ((*after == '=' && after[1] != '=') || compound || post_inc) {
          char *idx_raw = trim_dup_n(idx_start, (size_t)(idx_end - idx_start));
          char *value_raw = NULL;
          if (post_inc) value_raw = xstrdup("1");
          else value_raw = trim_dup(after + (compound ? 2 : 1));
          char *idx = convert_expr(idx_raw, &st->string_vars);
          char *value = convert_expr(value_raw, &st->string_vars);
          char *out = NULL;
          if (compound || post_inc) {
            char op[2] = { post_inc ? after[0] : after[0], '\0' };
            (void)asprintf(&out, "%s = set_idx(%s, %s, get(%s, %s, 0) %s %s)",
                           name, name, idx, name, idx, op, value);
          } else {
            (void)asprintf(&out, "%s = set_idx(%s, %s, %s)", name, name, idx, value);
          }
          free(idx_raw); free(value_raw); free(idx); free(value); free(t);
          return out;
        }
      }
    }
  }
  cp = t;
  if (!parse_identifier(&cp, name, sizeof(name))) {
    free(t);
    return NULL;
  }
  const char *op_start = skip_ws(cp);
  const char *ops[] = {"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "="};
  const char *op = NULL;
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    size_t olen = strlen(ops[i]);
    if (strncmp(op_start, ops[i], olen) == 0) {
      op = ops[i];
      op_start += olen;
      break;
    }
  }
  if (!op) {
    free(t);
    return NULL;
  }
  char *value = convert_expr(op_start, &st->string_vars);
  char *out = NULL;
  if (strcmp(op, "=") == 0) (void)asprintf(&out, "%s = %s", name, value);
  else (void)asprintf(&out, "%s %s %s", name, op, value);
  free(value);
  free(t);
  return out;
}

static char *convert_call_stmt(cbridge_state_t *st, const char *line) {
  const char *p = skip_ws(line);
  char name[96];
  if (!parse_identifier(&p, name, sizeof(name)) || strcmp(name, "printf") == 0) return NULL;
  p = skip_ws(p);
  if (*p++ != '(') return NULL;
  const char *args_start = p;
  const char *close = strrchr(args_start, ')');
  if (!close) return NULL;
  const char *after = skip_ws(close + 1);
  if (*after++ != ';' || *skip_ws(after) != '\0') return NULL;
  char *args_raw = trim_dup_n(args_start, (size_t)(close - args_start));
  char *args = convert_expr(args_raw, &st->string_vars);
  char *out = NULL;
  (void)asprintf(&out, "%s(%s)", name, args);
  free(args_raw); free(args);
  return out;
}

static const char *find_matching_brace_cbridge(const char *open) {
  int depth = 0;
  for (const char *p = open; *p; ++p) {
    if (*p == '{') ++depth;
    else if (*p == '}') {
      --depth;
      if (depth == 0) return p;
    }
  }
  return NULL;
}

static char *convert_compact_stmt(cbridge_state_t *st, const char *inner) {
  char *trimmed = trim_dup(inner);
  if (!*trimmed) return trimmed;
  char *converted = convert_break_continue(trimmed);
  if (!converted) converted = convert_assignment(st, trimmed);
  if (!converted) converted = convert_return(st, trimmed);
  if (!converted) converted = convert_printf(st, trimmed);
  if (!converted) converted = convert_call_stmt(st, trimmed);
  if (!converted && !st->error[0]) {
    set_error_category(st, "compact-control", "unsupported compact if body: %s", trimmed);
  }
  free(trimmed);
  return converted;
}

static char *convert_compact_if(cbridge_state_t *st, const char *line) {
  const char *p = skip_ws(line);
  if (!consume_word(&p, "if")) return NULL;
  p = skip_ws(p);
  if (*p++ != '(') return NULL;
  const char *cond_start = p;
  const char *close = strchr(cond_start, ')');
  if (!close) return NULL;
  const char *after = skip_ws(close + 1);
  if (*after++ != '{') return NULL;
  const char *body_start = after;
  const char *body_end = find_matching_brace_cbridge(after - 1);
  if (!body_end) return NULL;
  const char *tail = skip_ws(body_end + 1);
  char *inner = trim_dup_n(body_start, (size_t)(body_end - body_start));
  char *converted = convert_compact_stmt(st, inner);
  if (!converted) {
    free(inner);
    return NULL;
  }
  char *cond_raw = trim_dup_n(cond_start, (size_t)(close - cond_start));
  char *cond = convert_expr(cond_raw, &st->string_vars);
  char *out = NULL;
  if (*tail == '\0') {
    if (*converted) (void)asprintf(&out, "if(%s){\n%s\n}", cond, converted);
    else (void)asprintf(&out, "if(%s){\n}", cond);
  } else {
    const char *ep = tail;
    if (!consume_word(&ep, "else")) {
      free(cond_raw); free(cond); free(converted); free(inner);
      return NULL;
    }
    ep = skip_ws(ep);
    if (*ep++ != '{') {
      free(cond_raw); free(cond); free(converted); free(inner);
      return NULL;
    }
    const char *else_start = ep;
    const char *else_end = find_matching_brace_cbridge(ep - 1);
    if (!else_end || *skip_ws(else_end + 1) != '\0') {
      free(cond_raw); free(cond); free(converted); free(inner);
      return NULL;
    }
    char *else_inner = trim_dup_n(else_start, (size_t)(else_end - else_start));
    char *else_converted = convert_compact_stmt(st, else_inner);
    if (!else_converted) {
      free(cond_raw); free(cond); free(converted); free(inner); free(else_inner);
      return NULL;
    }
    if (*converted && *else_converted)
      (void)asprintf(&out, "if(%s){\n%s\n} else {\n%s\n}", cond, converted, else_converted);
    else if (*converted)
      (void)asprintf(&out, "if(%s){\n%s\n} else {\n}", cond, converted);
    else if (*else_converted)
      (void)asprintf(&out, "if(%s){\n} else {\n%s\n}", cond, else_converted);
    else
      (void)asprintf(&out, "if(%s){\n} else {\n}", cond);
    free(else_inner);
    free(else_converted);
  }
  free(cond_raw); free(cond); free(converted); free(inner);
  return out;
}

static bool converted_starts_close(const char *s) {
  s = skip_ws(s);
  return *s == '}';
}

static bool converted_ends_open(const char *s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) --n;
  return n && s[n - 1] == '{';
}

static void process_converted(cbridge_state_t *st, char *converted) {
  if (converted_starts_close(converted)) {
    char *update = stack_pop(st);
    if (update) append_line(st, update);
  }
  if (strcmp(converted, "continue") == 0) {
    char *update = nearest_loop_update(st);
    if (update) append_line(st, update);
  }
  append_text_lines(st, converted);
  if (converted_ends_open(converted)) stack_push(st, NULL);
}

static bool convert_source(cbridge_state_t *st, const char *text, char **ny_source_out) {
  collect_readonly_arrays(text, &st->readonly_arrays);
  (void)sb_append(&st->out, "use std.core\n\n");
  st->last_blank = true;
  size_t start = 0;
  int line_no = 1;
  for (size_t i = 0; ; ++i) {
    if (text[i] != '\n' && text[i] != '\0') continue;
    st->current_line = line_no;
    char *line = xstrndup(text + start, i - start);
    if (!line) {
      set_error(st, "allocation failed");
      return false;
    }
    char *comment = strstr(line, "//");
    if (comment) *comment = '\0';
    rstrip_in_place(line);
    char *stripped = trim_dup(line);
    if (!*stripped) {
      append_line(st, "");
    } else if (starts_word(stripped, "#include") || starts_word(stripped, "#define")) {

    } else if (strcmp(stripped, "{") == 0) {
      stack_push(st, NULL);
      append_line(st, stripped);
    } else if (strcmp(stripped, "}") == 0) {
      char *update = stack_pop(st);
      if (update) append_line(st, update);
      append_line(st, stripped);
    } else if (starts_word(stripped, "struct") && strlen(stripped) >= 2 &&
               strcmp(stripped + strlen(stripped) - 2, "};") == 0) {
      set_error(st, "struct syntax not supported");
    } else {
      char *update = NULL;
      char *converted = convert_for(st, line, &update);
      if (converted) {
        feature_add(st, "for");
        append_text_lines(st, converted);
        stack_push(st, update);
        free(converted);
      } else if (!st->error[0]) {
        converted = convert_function_header(st, line);
        if (converted) feature_add(st, "functions");
        if (!converted) {
          converted = convert_compact_if(st, line);
          if (converted) feature_add(st, "control");
        }
        if (!converted && !st->error[0]) {
          converted = convert_if_while(st, line);
          if (converted) feature_add(st, "control");
        }
        if (!converted && !st->error[0]) {
          converted = convert_break_continue(line);
          if (converted) feature_add(st, "loop-control");
        }
        if (!converted && !st->error[0]) {
          converted = convert_printf(st, line);
          if (converted) feature_add(st, "printf");
        }
        if (!converted && !st->error[0]) {
          converted = convert_multi_decl(st, line);
          if (converted) feature_add(st, "decl");
        }
        if (!converted && !st->error[0]) {
          converted = convert_return(st, line);
          if (converted) feature_add(st, "return");
        }
        if (!converted && !st->error[0]) {
          converted = convert_assignment(st, line);
          if (converted) feature_add(st, "assign");
        }
        if (!converted && !st->error[0]) {
          converted = convert_call_stmt(st, line);
          if (converted) feature_add(st, "call");
        }
        if (!converted && !st->error[0]) {
          converted = convert_decl(st, line);
          if (converted) {
            feature_add(st, "decl");
            if (strstr(converted, "def ") && strchr(converted, '[')) feature_add(st, "readonly-array");
          }
        }
        if (converted) {
          process_converted(st, converted);
          free(converted);
        } else if (!st->error[0]) {
          set_error(st, "unsupported C line in %s: %s", st->source_path, line);
        }
      }
    }
    free(stripped);
    free(line);
    if (st->error[0]) return false;
    if (text[i] == '\0') break;
    start = i + 1;
    ++line_no;
  }
  *ny_source_out = finish_source(st);
  return *ny_source_out != NULL;
}

static bool mkdir_parent(const char *path) {
  char tmp[4096];
  size_t len = strlen(path);
  if (len >= sizeof(tmp)) return false;
  memcpy(tmp, path, len + 1);
  char *slash = strrchr(tmp, '/');
  if (!slash) return true;
  if (slash == tmp) return true;
  *slash = '\0';
  ny_ensure_dir_recursive(tmp);
  return true;
}

static bool write_file(const char *path, const char *data) {
  if (!mkdir_parent(path)) return false;
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  size_t n = strlen(data);
  bool ok = fwrite(data, 1, n, f) == n;
  if (fclose(f) != 0) ok = false;
  return ok;
}

void print_cbridge_features_json(FILE *out, const cbridge_convert_result_t *result) {
  fputc('[', out);
  for (int i = 0; i < result->feature_count; ++i) {
    if (i) fputc(',', out);
    json_str(out, result->features[i]);
  }
  fputc(']', out);
}

cbridge_convert_result_t convert_cbridge_file(const char *c_path) {
  double start = now_ms();
  cbridge_convert_result_t result;
  memset(&result, 0, sizeof(result));
  file_buf_t input = {0};
  if (!read_file(c_path, &input)) {
    snprintf(result.error, sizeof(result.error), "read-failed");
    snprintf(result.error_category, sizeof(result.error_category), "io");
    result.worker_ms = now_ms() - start;
    return result;
  }
  cbridge_state_t st;
  memset(&st, 0, sizeof(st));
  st.source_path = c_path;
  char *ny_source = NULL;
  bool ok = convert_source(&st, input.data, &ny_source);
  result.worker_ms = now_ms() - start;
  if (!ok) {
    snprintf(result.error, sizeof(result.error), "%s", st.error[0] ? st.error : "conversion failed");
    snprintf(result.error_category, sizeof(result.error_category), "%s",
             st.error_category[0] ? st.error_category : "unsupported");
    result.error_line = st.error_line;
    free(input.data);
    free(ny_source);
    state_free(&st);
    return result;
  }
  qsort(st.features, (size_t)st.feature_count, sizeof(st.features[0]), feature_cmp);
  result.ny_source = ny_source;
  result.feature_count = st.feature_count;
  for (int i = 0; i < st.feature_count && i < (int)(sizeof(result.features) / sizeof(result.features[0])); ++i)
    result.features[i] = st.features[i];
  free(input.data);
  state_free(&st);
  return result;
}

void cbridge_convert_result_free(cbridge_convert_result_t *result) {
  if (!result) return;
  free(result->ny_source);
  memset(result, 0, sizeof(*result));
}

int cmd_convert_cbridge(int argc, char **argv) {
  const char *c_path = arg_value(argc, argv, "--c", "");
  const char *out_path = arg_value(argc, argv, "--out", "");
  if (!c_path || !*c_path || !out_path || !*out_path) {
    printf("{\"ok\":false,\"error\":\"unsupported\",\"reason\":\"missing-required-path\"}\n");
    return 3;
  }
  cbridge_convert_result_t result = convert_cbridge_file(c_path);
  if (!result.ny_source) {
    if (strcmp(result.error, "read-failed") == 0) {
      printf("{\"ok\":false,\"error\":\"read-failed\",\"c_source\":");
      json_str(stdout, c_path);
      printf("}\n");
      cbridge_convert_result_free(&result);
      return 1;
    }
    printf("{\"ok\":false,\"error\":\"unsupported\",\"reason\":");
    json_str(stdout, result.error[0] ? result.error : "conversion failed");
    printf(",\"engine\":\"nytrix_core\",\"convert_engine\":\"nytrix_core\",\"c_source\":");
    json_str(stdout, c_path);
    printf(",\"ny_source\":");
    json_str(stdout, out_path);
    printf(",\"line\":%d,\"diagnostic_category\":", result.error_line);
    json_str(stdout, result.error_category[0] ? result.error_category : "unsupported");
    printf(",\"worker_ms\":%.2f}\n", result.worker_ms);
    cbridge_convert_result_free(&result);
    return 3;
  }
  if (!write_file(out_path, result.ny_source)) {
    printf("{\"ok\":false,\"error\":\"write-failed\",\"engine\":\"nytrix_core\",\"c_source\":");
    json_str(stdout, c_path);
    printf(",\"ny_source\":");
    json_str(stdout, out_path);
    printf("}\n");
    cbridge_convert_result_free(&result);
    return 1;
  }
  printf("{\"ok\":true,\"engine\":\"nytrix_core\",\"convert_engine\":\"nytrix_core\",\"c_source\":");
  json_str(stdout, c_path);
  printf(",\"ny_source\":");
  json_str(stdout, out_path);
  printf(",\"features\":");
  print_cbridge_features_json(stdout, &result);
  printf(",\"bytes\":%zu,\"lines\":%d,\"worker_ms\":%.2f}\n",
         strlen(result.ny_source), count_lines(result.ny_source, strlen(result.ny_source)), result.worker_ms);
  cbridge_convert_result_free(&result);
  return 0;
}
