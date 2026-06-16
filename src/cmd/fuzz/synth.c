#include "core.h"

#include <stdarg.h>

typedef struct {
  int lo;
  int hi;
} synth_range_t;

typedef enum {
  SYNTH_SCALAR_U8 = 0,
  SYNTH_SCALAR_U16,
  SYNTH_SCALAR_U32,
  SYNTH_SCALAR_U64,
  SYNTH_SCALAR_COUNT
} synth_scalar_kind_t;

typedef enum {
  SYNTH_STMT_BALANCED = 0,
  SYNTH_STMT_BRANCHY,
  SYNTH_STMT_MEMORY,
  SYNTH_STMT_CALLHEAVY
} synth_statement_style_t;

typedef struct {
  int param_count;
  int local_count;
  synth_scalar_kind_t param_kind[4];
  synth_scalar_kind_t local_kind[16];
} synth_func_model_t;

typedef struct {
  char name[96];
  char family[96];
  char generator[32];
  char features[512];
  char source[512];
  char hash[32];
  int weights[5];
  int fast_rank;
  synth_range_t globals;
  synth_range_t arrays;
  synth_range_t structs;
  synth_range_t functions;
  synth_range_t statements;
  synth_range_t expression_depth;
  synth_range_t loop_depth;
  synth_range_t locals;
  synth_range_t params;
  synth_range_t array_dims;
  char scalar_widths[64];
  char statement_mix[32];
  char helper_mix[32];
  int call_chance;
  int switch_chance;
  int pointer_chance;
} synth_program_shape_t;

typedef struct {
  synth_program_shape_t items[64];
  int count;
} synth_program_shapes_t;

typedef struct {
  FILE *out;
  synth_program_shape_t shape;
  uint64_t rng;
  int seed;
  const char *requested_generator;
  const char *profile;
  bool fast;
  int globals;
  int arrays;
  int structs;
  int functions;
  int statements;
  int expression_depth;
  int loop_depth;
  int scalar_mask;
  int helper_level;
  synth_statement_style_t statement_style;
  int array_lens[8];
  int array_dims[8];
  int array_rows[8];
  int array_cols[8];
  int struct_lens[6];
  synth_func_model_t funcs[10];
  int indent;
  int loop_serial;
} synth_emit_t;

static const char *synth_profile_names[] = {"balanced", "optimizer", "memory", "strings", "state"};

static uint64_t synth_rng_next(synth_emit_t *s) {
  uint64_t x = s->rng ? s->rng : UINT64_C(0x9e3779b97f4a7c15);
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  s->rng = x;
  return x * UINT64_C(2685821657736338717);
}

static int synth_rng_range(synth_emit_t *s, int lo, int hi) {
  if (hi <= lo) return lo;
  return lo + (int)(synth_rng_next(s) % (uint64_t)(hi - lo + 1));
}

static uint32_t synth_rng_u32(synth_emit_t *s) {
  return (uint32_t)(synth_rng_next(s) >> 32);
}

static int synth_clamp_chance(int value) {
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

static bool synth_chance(synth_emit_t *s, int chance) {
  return synth_rng_range(s, 0, 99) < synth_clamp_chance(chance);
}

static const char *synth_scalar_type(synth_scalar_kind_t kind) {
  switch (kind) {
  case SYNTH_SCALAR_U8: return "uint8_t";
  case SYNTH_SCALAR_U16: return "uint16_t";
  case SYNTH_SCALAR_U64: return "uint64_t";
  case SYNTH_SCALAR_U32:
  default: return "uint32_t";
  }
}

static int synth_scalar_bit(synth_scalar_kind_t kind) {
  return 1 << (int)kind;
}

static int synth_scalar_mask_from_text(const char *text) {
  if (!text || !*text) return synth_scalar_bit(SYNTH_SCALAR_U8) |
                              synth_scalar_bit(SYNTH_SCALAR_U16) |
                              synth_scalar_bit(SYNTH_SCALAR_U32);
  int mask = 0;
  if (strstr(text, "u8") || strstr(text, "uint8")) mask |= synth_scalar_bit(SYNTH_SCALAR_U8);
  if (strstr(text, "u16") || strstr(text, "uint16")) mask |= synth_scalar_bit(SYNTH_SCALAR_U16);
  if (strstr(text, "u32") || strstr(text, "uint32")) mask |= synth_scalar_bit(SYNTH_SCALAR_U32);
  if (strstr(text, "u64") || strstr(text, "uint64")) mask |= synth_scalar_bit(SYNTH_SCALAR_U64);
  return mask ? mask : synth_scalar_bit(SYNTH_SCALAR_U32);
}

static synth_scalar_kind_t synth_pick_scalar(synth_emit_t *s) {
  int choices[SYNTH_SCALAR_COUNT];
  int n = 0;
  for (int i = 0; i < SYNTH_SCALAR_COUNT; ++i) {
    if (s->scalar_mask & (1 << i)) choices[n++] = i;
  }
  if (n <= 0) return SYNTH_SCALAR_U32;
  return (synth_scalar_kind_t)choices[synth_rng_range(s, 0, n - 1)];
}

static synth_statement_style_t synth_statement_style_from_text(const char *text) {
  if (!text || !*text || strcmp(text, "balanced") == 0) return SYNTH_STMT_BALANCED;
  if (strcmp(text, "branchy") == 0) return SYNTH_STMT_BRANCHY;
  if (strcmp(text, "memory") == 0) return SYNTH_STMT_MEMORY;
  if (strcmp(text, "callheavy") == 0 || strcmp(text, "call-heavy") == 0) return SYNTH_STMT_CALLHEAVY;
  return SYNTH_STMT_BALANCED;
}

static int synth_helper_level_from_text(const char *text) {
  if (!text || !*text || strcmp(text, "basic") == 0) return 0;
  if (strstr(text, "rich") || strstr(text, "scramble") || strstr(text, "extended")) return 1;
  return 0;
}

static synth_range_t synth_range_default(int lo, int hi) {
  synth_range_t r;
  r.lo = lo;
  r.hi = hi;
  return r;
}

static int synth_pick_range(synth_emit_t *s, synth_range_t r) {
  return synth_rng_range(s, r.lo, r.hi);
}

static int synth_profile_index(const char *profile) {
  for (int i = 0; i < 5; ++i) {
    if (profile && strcmp(profile, synth_profile_names[i]) == 0) return i;
  }
  return 0;
}

static bool synth_extract_quoted_after(const char *data, const char *key,
                                       char *out, size_t out_sz) {
  const char *p = strstr(data, key);
  if (!p) return false;
  p = strchr(p, '"');
  if (!p) return false;
  ++p;
  const char *q = strchr(p, '"');
  if (!q) return false;
  size_t n = (size_t)(q - p);
  if (n >= out_sz) n = out_sz - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

static bool synth_extract_shape_name(const char *data, char *out, size_t out_sz) {
  const char *p = strstr(data, "shape ");
  if (!p) return false;
  p += 6;
  while (*p == ' ' || *p == '\t') ++p;
  const char *q = p;
  while (*q && *q != ' ' && *q != '\t' && *q != '{' && *q != '\n') ++q;
  size_t n = (size_t)(q - p);
  if (!n) return false;
  if (n >= out_sz) n = out_sz - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

static bool synth_extract_features(const char *data, char *out, size_t out_sz) {
  const char *p = strstr(data, "features ");
  if (!p) return false;
  p = strchr(p, '[');
  if (!p) return false;
  const char *q = strchr(p, ']');
  if (!q) return false;
  size_t n = (size_t)(q - p + 1);
  if (n >= out_sz) n = out_sz - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

static int synth_extract_int_after(const char *data, const char *key, int fallback) {
  const char *p = strstr(data, key);
  if (!p) return fallback;
  p += strlen(key);
  while (*p == ' ' || *p == '\t' || *p == ':') ++p;
  return atoi(p);
}

static int synth_extract_weight(const char *data, const char *profile) {
  const char *p = strstr(data, profile);
  if (!p) return 1;
  p += strlen(profile);
  while (*p == ' ' || *p == '\t' || *p == ':') ++p;
  int v = atoi(p);
  return v > 0 ? v : 0;
}

static bool synth_extract_token_after(const char *data, const char *key,
                                      char *out, size_t out_sz) {
  if (!out_sz) return false;
  out[0] = '\0';
  const char *p = strstr(data, key);
  if (!p) return false;
  p += strlen(key);
  while (*p == ' ' || *p == '\t' || *p == ':') ++p;
  if (*p == '"') {
    ++p;
    const char *q = strchr(p, '"');
    if (!q) return false;
    size_t n = (size_t)(q - p);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
  }
  const char *q = p;
  while (*q && !isspace((unsigned char)*q) && *q != '}') ++q;
  size_t n = (size_t)(q - p);
  if (!n) return false;
  if (n >= out_sz) n = out_sz - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

static bool synth_parse_range(const char *block, const char *key, synth_range_t *out) {
  const char *p = strstr(block, key);
  if (!p) return false;
  p += strlen(key);
  while (*p == ' ' || *p == '\t' || *p == ':') ++p;
  char *end = NULL;
  long lo = strtol(p, &end, 10);
  if (end == p) return false;
  long hi = lo;
  p = end;
  while (*p == ' ' || *p == '\t') ++p;
  if (p[0] == '.' && p[1] == '.') {
    p += 2;
    while (*p == ' ' || *p == '\t') ++p;
    hi = strtol(p, &end, 10);
    if (end == p) return false;
  }
  if (lo < 0) lo = 0;
  if (hi < lo) hi = lo;
  if (hi > 10000) hi = 10000;
  out->lo = (int)lo;
  out->hi = (int)hi;
  return true;
}

static void synth_source_from_path(const char *path, char *out, size_t out_sz) {
  const char *p = strstr(path, "shapes/");
  if (!p) p = path;
  snprintf(out, out_sz, "%.*s", (int)out_sz - 1, p);
}

static int synth_shape_cmp(const void *a, const void *b) {
  const synth_program_shape_t *x = (const synth_program_shape_t *)a;
  const synth_program_shape_t *y = (const synth_program_shape_t *)b;
  if (x->fast_rank != y->fast_rank) return x->fast_rank - y->fast_rank;
  return strcmp(x->name, y->name);
}

static void synth_shape_defaults(synth_program_shape_t *s) {
  memset(s, 0, sizeof(*s));
  snprintf(s->family, sizeof(s->family), "program-synth");
  snprintf(s->generator, sizeof(s->generator), "program");
  snprintf(s->features, sizeof(s->features), "[]");
  for (int i = 0; i < 5; ++i) s->weights[i] = 1;
  s->fast_rank = 100;
  s->globals = synth_range_default(3, 6);
  s->arrays = synth_range_default(1, 3);
  s->structs = synth_range_default(0, 2);
  s->functions = synth_range_default(3, 6);
  s->statements = synth_range_default(26, 60);
  s->expression_depth = synth_range_default(2, 4);
  s->loop_depth = synth_range_default(1, 3);
  s->locals = synth_range_default(4, 8);
  s->params = synth_range_default(2, 3);
  s->array_dims = synth_range_default(1, 1);
  snprintf(s->scalar_widths, sizeof(s->scalar_widths), "u8,u16,u32");
  snprintf(s->statement_mix, sizeof(s->statement_mix), "balanced");
  snprintf(s->helper_mix, sizeof(s->helper_mix), "basic");
  s->call_chance = 25;
  s->switch_chance = 15;
  s->pointer_chance = 10;
}

static void synth_parse_shape_file(const char *path, synth_program_shapes_t *shapes) {
  if (shapes->count >= (int)(sizeof(shapes->items) / sizeof(shapes->items[0]))) return;
  file_buf_t f = {0};
  if (!read_file(path, &f)) return;
  const char *block = strstr(f.data, "synth {");
  if (!block) {
    free(f.data);
    return;
  }
  synth_program_shape_t s;
  synth_shape_defaults(&s);
  if (!synth_extract_shape_name(f.data, s.name, sizeof(s.name))) {
    free(f.data);
    return;
  }
  (void)synth_extract_quoted_after(f.data, "family ", s.family, sizeof(s.family));
  (void)synth_extract_quoted_after(f.data, "generator ", s.generator, sizeof(s.generator));
  if (strcmp(s.generator, "program") != 0) {
    free(f.data);
    return;
  }
  if (!synth_extract_features(f.data, s.features, sizeof(s.features))) {
    snprintf(s.features, sizeof(s.features), "[]");
  }
  for (int i = 0; i < 5; ++i) s.weights[i] = synth_extract_weight(f.data, synth_profile_names[i]);
  s.fast_rank = synth_extract_int_after(f.data, "fast_rank", 100);
  (void)synth_parse_range(block, "globals", &s.globals);
  (void)synth_parse_range(block, "arrays", &s.arrays);
  (void)synth_parse_range(block, "structs", &s.structs);
  (void)synth_parse_range(block, "functions", &s.functions);
  (void)synth_parse_range(block, "statements", &s.statements);
  (void)synth_parse_range(block, "expression_depth", &s.expression_depth);
  (void)synth_parse_range(block, "loop_depth", &s.loop_depth);
  (void)synth_parse_range(block, "locals", &s.locals);
  (void)synth_parse_range(block, "params", &s.params);
  (void)synth_parse_range(block, "array_dims", &s.array_dims);
  (void)synth_extract_token_after(block, "scalar_widths", s.scalar_widths, sizeof(s.scalar_widths));
  (void)synth_extract_token_after(block, "statement_mix", s.statement_mix, sizeof(s.statement_mix));
  (void)synth_extract_token_after(block, "helper_mix", s.helper_mix, sizeof(s.helper_mix));
  s.call_chance = synth_extract_int_after(block, "call_chance", s.call_chance);
  s.switch_chance = synth_extract_int_after(block, "switch_chance", s.switch_chance);
  s.pointer_chance = synth_extract_int_after(block, "pointer_chance", s.pointer_chance);
  synth_source_from_path(path, s.source, sizeof(s.source));
  snprintf(s.hash, sizeof(s.hash), "%016" PRIx64, fnv1a64(f.data, f.len));
  shapes->items[shapes->count++] = s;
  free(f.data);
}

static void synth_walk_shapes(const char *dir, synth_program_shapes_t *shapes) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    if (n <= 0 || (size_t)n >= sizeof(path)) continue;
    struct stat st;
    if (stat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) synth_walk_shapes(path, shapes);
    else if (S_ISREG(st.st_mode) && ny_has_suffix(path, ".nshape")) synth_parse_shape_file(path, shapes);
  }
  closedir(d);
}

static const synth_program_shape_t *synth_choose_shape(const synth_program_shapes_t *shapes,
                                                       const char *shape_name,
                                                       const char *profile,
                                                       int seed,
                                                       const char *generator) {
  if (shape_name && *shape_name) {
    for (int i = 0; i < shapes->count; ++i) {
      if (strcmp(shapes->items[i].name, shape_name) == 0) return &shapes->items[i];
    }
    return NULL;
  }
  if (shapes->count <= 0) return NULL;
  int pi = synth_profile_index(profile);
  int total = 0;
  for (int i = 0; i < shapes->count; ++i) total += shapes->items[i].weights[pi] > 0 ? shapes->items[i].weights[pi] : 0;
  uint64_t state = ((uint64_t)(uint32_t)seed << 32) ^
                   fnv1a64(profile ? profile : "", strlen(profile ? profile : "")) ^
                   (fnv1a64(generator ? generator : "", strlen(generator ? generator : "")) << 1);
  if (total <= 0) return &shapes->items[(int)(state % (uint64_t)shapes->count)];
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  int pick = (int)((state * UINT64_C(2685821657736338717)) % (uint64_t)total);
  for (int i = 0; i < shapes->count; ++i) {
    int w = shapes->items[i].weights[pi] > 0 ? shapes->items[i].weights[pi] : 0;
    if (pick < w) return &shapes->items[i];
    pick -= w;
  }
  return &shapes->items[shapes->count - 1];
}

static void synth_indent(synth_emit_t *s) {
  for (int i = 0; i < s->indent; ++i) fputs("    ", s->out);
}

static void synth_line(synth_emit_t *s, const char *fmt, ...) {
  synth_indent(s);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(s->out, fmt, ap);
  va_end(ap);
  fputc('\n', s->out);
}

static void synth_hex(FILE *out, uint32_t v) {
  fprintf(out, "0x%08" PRIx32 "u", v);
}

static bool synth_sb_append_hex(str_buf_t *b, uint32_t v) {
  return sb_appendf(b, "0x%08" PRIx32 "u", v);
}

static void synth_emit_expr(synth_emit_t *s, str_buf_t *b, int depth, int func_idx);
static char *synth_expr_take(synth_emit_t *s, int depth, int func_idx);

static const synth_func_model_t *synth_func_model(const synth_emit_t *s, int func_idx) {
  if (func_idx < 0 || func_idx >= s->functions) return NULL;
  return &s->funcs[func_idx];
}

static int synth_random_param_index(synth_emit_t *s, int func_idx) {
  const synth_func_model_t *m = synth_func_model(s, func_idx);
  int n = m && m->param_count > 0 ? m->param_count : 2;
  return synth_rng_range(s, 0, n - 1);
}

static int synth_random_local_index(synth_emit_t *s, int func_idx) {
  const synth_func_model_t *m = synth_func_model(s, func_idx);
  int n = m && m->local_count > 0 ? m->local_count : 4;
  return synth_rng_range(s, 0, n - 1);
}

static void synth_append_call_args(synth_emit_t *s, str_buf_t *b, int target,
                                   int caller, int depth) {
  const synth_func_model_t *m = synth_func_model(s, target);
  int n = m && m->param_count > 0 ? m->param_count : 1;
  for (int i = 0; i < n; ++i) {
    if (i) (void)sb_append(b, ", ");
    char *arg = synth_expr_take(s, depth, caller);
    (void)sb_appendf(b, "(%s)(%s)",
                     synth_scalar_type(m ? m->param_kind[i] : SYNTH_SCALAR_U32),
                     arg ? arg : "0u");
    free(arg);
  }
}

static char *synth_call_expr_take(synth_emit_t *s, int target, int caller, int depth) {
  str_buf_t b = {0};
  (void)sb_appendf(&b, "func_%d(", target);
  synth_append_call_args(s, &b, target, caller, depth);
  (void)sb_append_c(&b, ')');
  return sb_take(&b);
}

static void synth_init_models(synth_emit_t *s) {
  for (int i = 0; i < s->functions; ++i) {
    synth_func_model_t *m = &s->funcs[i];
    m->param_count = synth_pick_range(s, s->shape.params);
    if (m->param_count < 1) m->param_count = 1;
    if (m->param_count > 4) m->param_count = 4;
    m->local_count = synth_pick_range(s, s->shape.locals);
    if (m->local_count < 2) m->local_count = 2;
    if (m->local_count > 16) m->local_count = 16;
    if (s->fast && m->local_count > 8) m->local_count = 8;
    for (int p = 0; p < m->param_count; ++p) m->param_kind[p] = synth_pick_scalar(s);
    for (int l = 0; l < m->local_count; ++l) m->local_kind[l] = synth_pick_scalar(s);
  }
}

static void synth_emit_func_signature(synth_emit_t *s, int func_idx) {
  const synth_func_model_t *m = synth_func_model(s, func_idx);
  int params = m && m->param_count > 0 ? m->param_count : 1;
  fprintf(s->out, "static uint32_t func_%d(", func_idx);
  for (int i = 0; i < params; ++i) {
    if (i) fputs(", ", s->out);
    fprintf(s->out, "%s p%d",
            synth_scalar_type(m ? m->param_kind[i] : SYNTH_SCALAR_U32), i);
  }
  fputc(')', s->out);
}

static void synth_append_main_array_ref(synth_emit_t *s, str_buf_t *b, int arr, const char *idx) {
  if (arr >= 0 && arr < s->arrays && s->array_dims[arr] == 2) {
    (void)sb_appendf(b,
                     "g_arr_%d[((%s) %% G_ARR_%d_LEN) / G_ARR_%d_COLS][((%s) %% G_ARR_%d_LEN) %% G_ARR_%d_COLS]",
                     arr, idx, arr, arr, idx, arr, arr);
  } else {
    (void)sb_appendf(b, "g_arr_%d[(%s) %% G_ARR_%d_LEN]", arr, idx, arr);
  }
}

static char *synth_main_arg_expr_take(synth_emit_t *s, int arg_idx) {
  str_buf_t b = {0};
  int choice = synth_rng_range(s, 0, 99);
  if (choice < 18) {
    (void)sb_append(&b, "checksum");
  } else if (choice < 34) {
    (void)sb_append(&b, "round");
  } else if (choice < 50 && s->globals > 0) {
    (void)sb_appendf(&b, "g_%d", synth_rng_range(s, 0, s->globals - 1));
  } else if (choice < 66 && s->arrays > 0) {
    synth_append_main_array_ref(s, &b, synth_rng_range(s, 0, s->arrays - 1),
                                (arg_idx & 1) ? "checksum + round" : "round");
  } else if (choice < 82) {
    (void)sb_appendf(&b, "mix32(checksum, round + 0x%08" PRIx32 "u)", synth_rng_u32(s));
  } else {
    (void)synth_sb_append_hex(&b, synth_rng_u32(s));
  }
  return sb_take(&b);
}

static void synth_append_main_call_args(synth_emit_t *s, str_buf_t *b, int target) {
  const synth_func_model_t *m = synth_func_model(s, target);
  int n = m && m->param_count > 0 ? m->param_count : 1;
  for (int i = 0; i < n; ++i) {
    if (i) (void)sb_append(b, ", ");
    char *arg = synth_main_arg_expr_take(s, i);
    (void)sb_appendf(b, "(%s)(%s)",
                     synth_scalar_type(m ? m->param_kind[i] : SYNTH_SCALAR_U32),
                     arg ? arg : "0u");
    free(arg);
  }
}

static char *synth_main_call_expr_take(synth_emit_t *s, int target) {
  str_buf_t b = {0};
  (void)sb_appendf(&b, "func_%d(", target);
  synth_append_main_call_args(s, &b, target);
  (void)sb_append_c(&b, ')');
  return sb_take(&b);
}

static char *synth_index_take(synth_emit_t *s, int func_idx) {
  str_buf_t b = {0};
  int pick = synth_rng_range(s, 0, 5);
  if (pick == 0) (void)sb_appendf(&b, "p%d", synth_random_param_index(s, func_idx));
  else if (pick == 1) (void)sb_appendf(&b, "p%d", synth_random_param_index(s, func_idx));
  else if (pick == 2) (void)sb_append(&b, "acc");
  else if (pick == 3) (void)sb_appendf(&b, "l_%d", synth_random_local_index(s, func_idx));
  else if (pick == 4 && s->globals > 0) (void)sb_appendf(&b, "g_%d", synth_rng_range(s, 0, s->globals - 1));
  else (void)synth_sb_append_hex(&b, synth_rng_u32(s));
  return sb_take(&b);
}

static char *synth_expr_take(synth_emit_t *s, int depth, int func_idx) {
  str_buf_t b = {0};
  synth_emit_expr(s, &b, depth, func_idx);
  char *out = sb_take(&b);
  if (!out) return strdup("0u");
  return out;
}

static void synth_append_array_lvalue(synth_emit_t *s, str_buf_t *b, int arr, const char *idx) {
  if (arr >= 0 && arr < s->arrays && s->array_dims[arr] == 2) {
    (void)sb_appendf(b,
                     "g_arr_%d[((%s) %% G_ARR_%d_LEN) / G_ARR_%d_COLS][((%s) %% G_ARR_%d_LEN) %% G_ARR_%d_COLS]",
                     arr, idx, arr, arr, idx, arr, arr);
  } else {
    (void)sb_appendf(b, "g_arr_%d[(%s) %% G_ARR_%d_LEN]", arr, idx, arr);
  }
}

static char *synth_array_lvalue_take(synth_emit_t *s, int arr, int func_idx) {
  char *idx = synth_index_take(s, func_idx);
  str_buf_t b = {0};
  synth_append_array_lvalue(s, &b, arr, idx ? idx : "0u");
  free(idx);
  return sb_take(&b);
}

static void synth_emit_array_ref(synth_emit_t *s, str_buf_t *b, int arr, int func_idx) {
  char *lv = synth_array_lvalue_take(s, arr, func_idx);
  (void)sb_append(b, lv ? lv : "0u");
  free(lv);
}

static void synth_emit_struct_ref(synth_emit_t *s, str_buf_t *b, int st, int func_idx, const char *field) {
  char *idx = synth_index_take(s, func_idx);
  (void)sb_appendf(b, "g_nodes_%d[(%s) %% G_NODE_%d_LEN].%s", st, idx, st, field);
  free(idx);
}

static void synth_emit_leaf_expr(synth_emit_t *s, str_buf_t *b, int func_idx) {
  const synth_func_model_t *m = synth_func_model(s, func_idx);
  int params = m && m->param_count > 0 ? m->param_count : 2;
  int locals = m && m->local_count > 0 ? m->local_count : 4;
  int choices = 3 + params + locals + s->globals + s->arrays + s->structs;
  int pick = synth_rng_range(s, 0, choices - 1);
  if (pick == 0) {
    (void)synth_sb_append_hex(b, synth_rng_u32(s));
  } else if (pick == 1) {
    (void)sb_append(b, "acc");
  } else if (pick == 2) {
    (void)synth_sb_append_hex(b, synth_rng_u32(s));
  } else {
    pick -= 3;
    if (pick < params) {
      (void)sb_appendf(b, "p%d", pick);
      return;
    }
    pick -= params;
    if (pick < locals) {
      (void)sb_appendf(b, "l_%d", pick);
      return;
    }
    pick -= locals;
    if (pick < s->globals) {
      (void)sb_appendf(b, "g_%d", pick);
    } else if ((pick -= s->globals) < s->arrays) {
      synth_emit_array_ref(s, b, pick, func_idx);
    } else if ((pick -= s->arrays) < s->structs) {
      const char *field = synth_rng_range(s, 0, 2) == 0 ? "a" : (synth_rng_range(s, 0, 1) == 0 ? "b" : "tag");
      synth_emit_struct_ref(s, b, pick, func_idx, field);
    } else {
      (void)synth_sb_append_hex(b, synth_rng_u32(s));
    }
  }
}

static void synth_emit_expr(synth_emit_t *s, str_buf_t *b, int depth, int func_idx) {
  if (depth <= 0) {
    synth_emit_leaf_expr(s, b, func_idx);
    return;
  }
  int choice = synth_rng_range(s, 0, 99);
  if (func_idx > 0 && choice < s->shape.call_chance) {
    int target = synth_rng_range(s, 0, func_idx - 1);
    char *call = synth_call_expr_take(s, target, func_idx, depth - 1);
    (void)sb_append(b, call ? call : "0u");
    free(call);
  } else if (choice < 35) {
    char *a = synth_expr_take(s, depth - 1, func_idx);
    char *c = synth_expr_take(s, depth - 1, func_idx);
    const char *op = (choice & 1) ? "^" : "+";
    (void)sb_appendf(b, "((%s) %s (%s))", a, op, c);
    free(a);
    free(c);
  } else if (choice < 52) {
    char *a = synth_expr_take(s, depth - 1, func_idx);
    char *c = synth_expr_take(s, depth - 1, func_idx);
    (void)sb_appendf(b, "mix32(%s, %s)", a, c);
    free(a);
    free(c);
  } else if (s->helper_level > 0 && choice < 60) {
    char *a = synth_expr_take(s, depth - 1, func_idx);
    (void)sb_appendf(b, "scramble32(%s)", a);
    free(a);
  } else if (choice < 70) {
    char *a = synth_expr_take(s, depth - 1, func_idx);
    char *c = synth_expr_take(s, depth - 1, func_idx);
    (void)sb_appendf(b, "rotl32(%s, (%s) & 31u)", a, c);
    free(a);
    free(c);
  } else if (choice < 82 && s->arrays > 0) {
    synth_emit_array_ref(s, b, synth_rng_range(s, 0, s->arrays - 1), func_idx);
  } else if (choice < 92 && s->structs > 0) {
    synth_emit_struct_ref(s, b, synth_rng_range(s, 0, s->structs - 1), func_idx,
                          synth_rng_range(s, 0, 1) ? "a" : "b");
  } else {
    char *a = synth_expr_take(s, depth - 1, func_idx);
    uint32_t mul = (synth_rng_u32(s) | 1u);
    (void)sb_appendf(b, "((%s) * 0x%08" PRIx32 "u)", a, mul);
    free(a);
  }
}

static void synth_emit_simple_statement(synth_emit_t *s, int func_idx) {
  int choice = synth_rng_range(s, 0, 99);
  int call_cut = s->shape.call_chance;
  int local_cut = 38;
  int global_cut = 55;
  int array_cut = 75;
  int struct_cut = 90;
  if (s->statement_style == SYNTH_STMT_MEMORY) {
    call_cut -= 8;
    local_cut = 28;
    global_cut = 52;
    array_cut = 86;
    struct_cut = 96;
  } else if (s->statement_style == SYNTH_STMT_CALLHEAVY) {
    call_cut += 22;
    local_cut = 44;
    global_cut = 58;
    array_cut = 76;
  } else if (s->statement_style == SYNTH_STMT_BRANCHY) {
    local_cut = 45;
    global_cut = 60;
    array_cut = 78;
  }
  call_cut = synth_clamp_chance(call_cut);
  char *expr = synth_expr_take(s, s->expression_depth, func_idx);
  if (func_idx > 0 && choice < call_cut) {
    int target = synth_rng_range(s, 0, func_idx - 1);
    char *call = synth_call_expr_take(s, target, func_idx, s->expression_depth - 1);
    synth_line(s, "l_%d ^= %s;", synth_random_local_index(s, func_idx), call ? call : "0u");
    free(call);
  } else if (choice < local_cut) {
    synth_line(s, "l_%d = mix32(l_%d + %s, acc);",
               synth_random_local_index(s, func_idx), synth_random_local_index(s, func_idx), expr);
  } else if (choice < global_cut && s->globals > 0) {
    int g = synth_rng_range(s, 0, s->globals - 1);
    synth_line(s, "g_%d = mix32(g_%d ^ %s, l_%d);", g, g, expr, synth_random_local_index(s, func_idx));
  } else if (choice < array_cut && s->arrays > 0) {
    int arr = synth_rng_range(s, 0, s->arrays - 1);
    char *lv = synth_array_lvalue_take(s, arr, func_idx);
    synth_line(s, "%s = mix32(%s, %s);", lv ? lv : "g_0", lv ? lv : "g_0", expr);
    free(lv);
  } else if (choice < struct_cut && s->structs > 0) {
    int st = synth_rng_range(s, 0, s->structs - 1);
    char *idx = synth_index_take(s, func_idx);
    synth_line(s, "g_nodes_%d[(%s) %% G_NODE_%d_LEN].a ^= %s;", st, idx, st, expr);
    synth_line(s, "g_nodes_%d[(%s) %% G_NODE_%d_LEN].tag = (uint8_t)((g_nodes_%d[(%s) %% G_NODE_%d_LEN].tag + %s) & 255u);",
               st, idx, st, st, idx, st, expr);
    free(idx);
  } else {
    synth_line(s, "acc = mix32(acc + %s, l_%d);", expr, synth_random_local_index(s, func_idx));
  }
  free(expr);
}

static void synth_emit_block(synth_emit_t *s, int func_idx, int statements, int depth);

static void synth_emit_if(synth_emit_t *s, int func_idx, int statements, int depth) {
  char *cond = synth_expr_take(s, 2, func_idx);
  synth_line(s, "if (((%s) & 3u) == %" PRIu32 "u) {", cond, synth_rng_u32(s) & 3u);
  free(cond);
  s->indent++;
  synth_emit_block(s, func_idx, statements / 2 + 1, depth + 1);
  s->indent--;
  synth_line(s, "} else {");
  s->indent++;
  synth_emit_block(s, func_idx, statements / 2 + 1, depth + 1);
  s->indent--;
  synth_line(s, "}");
}

static void synth_emit_for(synth_emit_t *s, int func_idx, int statements, int depth) {
  int loop_id = s->loop_serial++;
  int limit = synth_rng_range(s, s->fast ? 2 : 3, s->fast ? 5 : 9);
  synth_line(s, "for (uint32_t i_%d = 0; i_%d < %du; i_%d++) {", loop_id, loop_id, limit, loop_id);
  s->indent++;
  synth_line(s, "acc ^= mix32(i_%d + p0, l_%d);", loop_id, synth_random_local_index(s, func_idx));
  synth_emit_block(s, func_idx, statements, depth + 1);
  s->indent--;
  synth_line(s, "}");
}

static void synth_emit_switch(synth_emit_t *s, int func_idx, int statements, int depth) {
  char *expr = synth_expr_take(s, 2, func_idx);
  synth_line(s, "switch ((%s) & 3u) {", expr);
  free(expr);
  s->indent++;
  for (int c = 0; c < 3; ++c) {
    synth_line(s, "case %du:", c);
    s->indent++;
    synth_emit_block(s, func_idx, statements / 3 + 1, depth + 1);
    synth_line(s, "break;");
    s->indent--;
  }
  synth_line(s, "default:");
  s->indent++;
  synth_emit_simple_statement(s, func_idx);
  synth_line(s, "break;");
  s->indent--;
  s->indent--;
  synth_line(s, "}");
}

static void synth_emit_block(synth_emit_t *s, int func_idx, int statements, int depth) {
  if (statements < 1) statements = 1;
  if (statements > 12) statements = 12;
  for (int i = 0; i < statements; ++i) {
    int choice = synth_rng_range(s, 0, 99);
    int if_cut = 18;
    int for_cut = 30;
    int switch_cut = 30 + (s->shape.switch_chance / 2);
    if (s->statement_style == SYNTH_STMT_BRANCHY) {
      if_cut = 30;
      for_cut = 43;
      switch_cut = 43 + s->shape.switch_chance;
    } else if (s->statement_style == SYNTH_STMT_MEMORY) {
      if_cut = 12;
      for_cut = 33;
      switch_cut = 33 + (s->shape.switch_chance / 3);
    } else if (s->statement_style == SYNTH_STMT_CALLHEAVY) {
      if_cut = 14;
      for_cut = 25;
      switch_cut = 25 + (s->shape.switch_chance / 3);
    }
    if (switch_cut > 95) switch_cut = 95;
    if (depth < s->loop_depth && choice < if_cut) {
      synth_emit_if(s, func_idx, synth_rng_range(s, 1, 4), depth);
    } else if (depth < s->loop_depth && choice < for_cut) {
      synth_emit_for(s, func_idx, synth_rng_range(s, 1, 4), depth);
    } else if (depth < s->loop_depth && choice < switch_cut) {
      synth_emit_switch(s, func_idx, synth_rng_range(s, 1, 4), depth);
    } else {
      synth_emit_simple_statement(s, func_idx);
    }
  }
}

static void synth_emit_globals(synth_emit_t *s) {
  for (int i = 0; i < s->globals; ++i) {
    fprintf(s->out, "static uint32_t g_%d = ", i);
    synth_hex(s->out, synth_rng_u32(s));
    fputs(";\n", s->out);
  }
  if (s->globals) fputc('\n', s->out);
  for (int i = 0; i < s->arrays; ++i) {
    s->array_dims[i] = synth_pick_range(s, s->shape.array_dims);
    if (s->array_dims[i] < 1) s->array_dims[i] = 1;
    if (s->array_dims[i] > 2) s->array_dims[i] = 2;
    if (s->array_dims[i] == 2) {
      s->array_rows[i] = synth_rng_range(s, s->fast ? 2 : 3, s->fast ? 4 : 7);
      s->array_cols[i] = synth_rng_range(s, s->fast ? 3 : 4, s->fast ? 7 : 11);
      s->array_lens[i] = s->array_rows[i] * s->array_cols[i];
      fprintf(s->out, "enum { G_ARR_%d_ROWS = %d, G_ARR_%d_COLS = %d, G_ARR_%d_LEN = %d };\n",
              i, s->array_rows[i], i, s->array_cols[i], i, s->array_lens[i]);
      fprintf(s->out, "static uint32_t g_arr_%d[G_ARR_%d_ROWS][G_ARR_%d_COLS] = {\n", i, i, i);
      for (int r = 0; r < s->array_rows[i]; ++r) {
        fputs("    {", s->out);
        for (int c = 0; c < s->array_cols[i]; ++c) {
          if (c) fputs(", ", s->out);
          synth_hex(s->out, synth_rng_u32(s));
        }
        fprintf(s->out, "}%s\n", r + 1 == s->array_rows[i] ? "" : ",");
      }
      fputs("};\n", s->out);
    } else {
      s->array_lens[i] = synth_rng_range(s, s->fast ? 5 : 7, s->fast ? 9 : 17);
      fprintf(s->out, "enum { G_ARR_%d_LEN = %d };\n", i, s->array_lens[i]);
      fprintf(s->out, "static uint32_t g_arr_%d[G_ARR_%d_LEN] = {", i, i);
      for (int j = 0; j < s->array_lens[i]; ++j) {
        if (j) fputs(", ", s->out);
        synth_hex(s->out, synth_rng_u32(s));
      }
      fputs("};\n", s->out);
    }
  }
  if (s->arrays) fputc('\n', s->out);
  if (s->structs) {
    fputs("struct nynth_node { uint32_t a; uint32_t b; uint8_t tag; };\n", s->out);
    for (int i = 0; i < s->structs; ++i) {
      s->struct_lens[i] = synth_rng_range(s, s->fast ? 3 : 4, s->fast ? 6 : 10);
      fprintf(s->out, "enum { G_NODE_%d_LEN = %d };\n", i, s->struct_lens[i]);
      fprintf(s->out, "static struct nynth_node g_nodes_%d[G_NODE_%d_LEN] = {\n", i, i);
      for (int j = 0; j < s->struct_lens[i]; ++j) {
        fputs("    {", s->out);
        synth_hex(s->out, synth_rng_u32(s));
        fputs(", ", s->out);
        synth_hex(s->out, synth_rng_u32(s));
        fprintf(s->out, ", %" PRIu32 "u}%s\n", synth_rng_u32(s) & 255u,
                j + 1 == s->struct_lens[i] ? "" : ",");
      }
      fputs("};\n", s->out);
    }
    fputc('\n', s->out);
  }
}

static void synth_emit_helpers(synth_emit_t *s) {
  fputs("static uint32_t rotl32(uint32_t x, uint32_t k) {\n"
        "    k &= 31u;\n"
        "    return k ? ((x << k) | (x >> (32u - k))) : x;\n"
        "}\n\n"
        "static uint32_t mix32(uint32_t x, uint32_t y) {\n"
        "    x ^= y + 0x9e3779b9u + (x << 6u) + (x >> 2u);\n"
        "    x ^= x >> 15u;\n"
        "    x *= 0x85ebca6bu;\n"
        "    x ^= x >> 13u;\n"
        "    x *= 0xc2b2ae35u;\n"
        "    x ^= x >> 16u;\n"
        "    return x;\n"
        "}\n\n", s->out);
  if (s->helper_level > 0) {
    fputs("static uint32_t scramble32(uint32_t x) {\n"
          "    x ^= rotl32(x, 7u);\n"
          "    x *= 0x7feb352du;\n"
          "    x ^= x >> 15u;\n"
          "    x *= 0x846ca68bu;\n"
          "    x ^= x >> 16u;\n"
          "    return x;\n"
          "}\n\n", s->out);
  }
}

static void synth_emit_forward_decls(synth_emit_t *s) {
  for (int i = 0; i < s->functions; ++i) {
    synth_emit_func_signature(s, i);
    fputs(";\n", s->out);
  }
  fputc('\n', s->out);
}

static void synth_emit_function(synth_emit_t *s, int func_idx) {
  const synth_func_model_t *m = synth_func_model(s, func_idx);
  int params = m && m->param_count > 0 ? m->param_count : 1;
  int locals = m && m->local_count > 0 ? m->local_count : 2;
  synth_emit_func_signature(s, func_idx);
  fputs(" {\n", s->out);
  s->indent = 1;
  synth_line(s, "uint32_t acc = mix32((uint32_t)p0, 0x%08" PRIx32 "u);", synth_rng_u32(s));
  for (int i = 1; i < params; ++i) {
    synth_line(s, "acc = mix32(acc, (uint32_t)p%d + 0x%08" PRIx32 "u);", i, synth_rng_u32(s));
  }
  for (int i = 0; i < locals; ++i) {
    const char *type = synth_scalar_type(m ? m->local_kind[i] : SYNTH_SCALAR_U32);
    synth_line(s, "%s l_%d = (%s)mix32(acc + 0x%08" PRIx32 "u, (uint32_t)p%d);",
               type, i, type, synth_rng_u32(s), i % params);
  }
  bool use_ptr = s->globals > 0 && synth_chance(s, s->shape.pointer_chance);
  if (use_ptr) {
    synth_line(s, "uint32_t *ptr_0 = &g_%d;", synth_rng_range(s, 0, s->globals - 1));
    synth_line(s, "*ptr_0 = mix32(*ptr_0, acc);");
  }
  int body = s->statements / (s->functions > 0 ? s->functions : 1);
  body += synth_rng_range(s, 1, s->fast ? 4 : 8);
  if (body < 6) body = 6;
  if (s->fast && body > 14) body = 14;
  synth_emit_block(s, func_idx, body, 0);
  if (use_ptr) synth_line(s, "acc ^= *ptr_0;");
  synth_line(s, "uint32_t ret = acc;");
  for (int i = 0; i < locals; ++i) {
    synth_line(s, "ret = mix32(ret, (uint32_t)l_%d + 0x%08" PRIx32 "u);", i, synth_rng_u32(s));
  }
  for (int i = 0; i < params; ++i) {
    synth_line(s, "ret ^= (uint32_t)p%d + 0x%08" PRIx32 "u;", i, synth_rng_u32(s));
  }
  if (s->helper_level > 0) synth_line(s, "ret = scramble32(ret);");
  synth_line(s, "return mix32(ret, (uint32_t)p0 + 0x%08" PRIx32 "u);", synth_rng_u32(s));
  s->indent = 0;
  fputs("}\n\n", s->out);
}

static void synth_emit_main(synth_emit_t *s) {
  fputs("int main(int argc, char **argv) {\n", s->out);
  s->indent = 1;
  synth_line(s, "uint32_t checksum = 0x%08" PRIx32 "u;", synth_rng_u32(s));
  synth_line(s, "if (argc > 1 && argv[1] && argv[1][0] == '1') checksum ^= 1u;");
  int rounds = synth_rng_range(s, s->fast ? 5 : 12, s->fast ? 12 : 40);
  synth_line(s, "for (uint32_t round = 0; round < %du; round++) {", rounds);
  s->indent++;
  if (s->arrays > 0) {
    if (s->array_dims[0] == 2) {
      synth_line(s, "checksum ^= g_arr_0[(round %% G_ARR_0_LEN) / G_ARR_0_COLS][(round %% G_ARR_0_LEN) %% G_ARR_0_COLS];");
    } else {
      synth_line(s, "checksum ^= g_arr_0[round %% G_ARR_0_LEN];");
    }
  }
  for (int i = 0; i < s->functions; ++i) {
    char *call = synth_main_call_expr_take(s, i);
    if (i == 0) {
      synth_line(s, "checksum ^= %s;", call ? call : "0u");
    } else if (i & 1) {
      synth_line(s, "if (((checksum >> %du) & 1u) != 0u) checksum += %s;",
                 i % 13, call ? call : "0u");
    } else {
      synth_line(s, "else checksum ^= %s;", call ? call : "0u");
    }
    free(call);
  }
  synth_line(s, "checksum = mix32(checksum, round);");
  if (s->helper_level > 0) synth_line(s, "checksum = scramble32(checksum);");
  s->indent--;
  synth_line(s, "}");
  for (int g = 0; g < s->globals; ++g) {
    synth_line(s, "checksum ^= mix32(g_%d, 0x%08" PRIx32 "u);", g, synth_rng_u32(s));
  }
  for (int arr = 0; arr < s->arrays; ++arr) {
    if (s->array_dims[arr] == 2) {
      synth_line(s, "for (uint32_t i_%d = 0; i_%d < G_ARR_%d_LEN; i_%d++) checksum += mix32(g_arr_%d[i_%d / G_ARR_%d_COLS][i_%d %% G_ARR_%d_COLS], checksum);",
                 s->loop_serial, s->loop_serial, arr, s->loop_serial,
                 arr, s->loop_serial, arr, s->loop_serial, arr);
    } else {
      synth_line(s, "for (uint32_t i_%d = 0; i_%d < G_ARR_%d_LEN; i_%d++) checksum += mix32(g_arr_%d[i_%d], checksum);",
                 s->loop_serial, s->loop_serial, arr, s->loop_serial, arr, s->loop_serial);
    }
    s->loop_serial++;
  }
  for (int st = 0; st < s->structs; ++st) {
    synth_line(s, "for (uint32_t n_%d = 0; n_%d < G_NODE_%d_LEN; n_%d++) checksum ^= mix32(g_nodes_%d[n_%d].a, g_nodes_%d[n_%d].b + g_nodes_%d[n_%d].tag);",
               st, st, st, st, st, st, st, st, st, st);
  }
  synth_line(s, "printf(\"%%u\\n\", checksum);");
  synth_line(s, "return 0;");
  s->indent = 0;
  fputs("}\n", s->out);
}

static void synth_emit_program(synth_emit_t *s) {
  fprintf(s->out,
          "/*\n"
          " * RANDOMLY GENERATED Nynth C PROGRAM\n"
          " * seed: %d\n"
          " * requested-generator: %s\n"
          " * profile: %s\n"
          " * shape: %s\n"
          " * shape-source: %s\n"
          " */\n\n",
          s->seed,
          s->requested_generator && *s->requested_generator ? s->requested_generator : "ir",
          s->profile && *s->profile ? s->profile : "balanced",
          s->shape.name,
          s->shape.source);
  fputs("#include <stdint.h>\n#include <stdio.h>\n\n", s->out);
  synth_emit_globals(s);
  synth_emit_helpers(s);
  synth_emit_forward_decls(s);
  for (int i = 0; i < s->functions; ++i) synth_emit_function(s, i);
  synth_emit_main(s);
  fprintf(s->out,
          "\n/* nynth statistics: globals=%d arrays=%d structs=%d functions=%d statements=%d expr_depth=%d loop_depth=%d */\n",
          s->globals, s->arrays, s->structs, s->functions, s->statements,
          s->expression_depth, s->loop_depth);
}

int nynth_synth_print_c_program(FILE *out, const char *shape_dir,
                                const char *generator, const char *profile,
                                const char *shape_name, int seed, bool fast,
                                bool insane) {
  synth_program_shapes_t shapes;
  memset(&shapes, 0, sizeof(shapes));
  synth_walk_shapes(shape_dir, &shapes);
  qsort(shapes.items, (size_t)shapes.count, sizeof(shapes.items[0]), synth_shape_cmp);
  const synth_program_shape_t *shape = synth_choose_shape(&shapes, shape_name, profile, seed, generator);
  if (!shape) {
    fprintf(out, "{\"ok\":false,\"error\":\"no-program-shape\",\"shape_dir\":");
    json_str(out, shape_dir ? shape_dir : "");
    fprintf(out, ",\"shape\":");
    json_str(out, shape_name ? shape_name : "");
    fprintf(out, "}\n");
    return 1;
  }
  synth_emit_t s;
  memset(&s, 0, sizeof(s));
  s.out = out;
  s.shape = *shape;
  s.seed = seed;
  s.requested_generator = generator;
  s.profile = profile;
  s.fast = fast;
  s.rng = ((uint64_t)(uint32_t)seed << 32) ^
          fnv1a64(shape->name, strlen(shape->name)) ^
          (fnv1a64(generator ? generator : "", strlen(generator ? generator : "")) << 1) ^
          (fnv1a64(profile ? profile : "", strlen(profile ? profile : "")) << 2);
  s.globals = synth_pick_range(&s, shape->globals);
  s.arrays = synth_pick_range(&s, shape->arrays);
  s.structs = synth_pick_range(&s, shape->structs);
  s.functions = synth_pick_range(&s, shape->functions);
  s.statements = synth_pick_range(&s, shape->statements);
  s.expression_depth = synth_pick_range(&s, shape->expression_depth);
  s.loop_depth = synth_pick_range(&s, shape->loop_depth);
  s.scalar_mask = synth_scalar_mask_from_text(shape->scalar_widths);
  s.statement_style = synth_statement_style_from_text(shape->statement_mix);
  s.helper_level = synth_helper_level_from_text(shape->helper_mix);
  if (fast) {
    if (s.functions > 4) s.functions = 4;
    if (s.statements > 36) s.statements = 36;
    if (s.expression_depth > 3) s.expression_depth = 3;
    if (s.loop_depth > 2) s.loop_depth = 2;
  }
  if (insane) {
    if (s.globals < 6) s.globals = 6;
    if (s.arrays < 3) s.arrays = 3;
    if (s.structs < 2) s.structs = 2;
    if (s.functions < 7) s.functions = 7;
    if (s.statements < 140) s.statements = 140;
    if (s.expression_depth < 4) s.expression_depth = 4;
    if (s.loop_depth < 3) s.loop_depth = 3;
    s.helper_level = 1;
  }
  if (s.globals < 1) s.globals = 1;
  if (s.globals > 8) s.globals = 8;
  if (s.arrays < 0) s.arrays = 0;
  if (s.arrays > 8) s.arrays = 8;
  if (s.structs < 0) s.structs = 0;
  if (s.structs > 6) s.structs = 6;
  if (s.functions < 1) s.functions = 1;
  if (s.functions > 10) s.functions = 10;
  if (s.expression_depth < 1) s.expression_depth = 1;
  if (s.expression_depth > 5) s.expression_depth = 5;
  if (s.loop_depth < 1) s.loop_depth = 1;
  if (s.loop_depth > 4) s.loop_depth = 4;
  if (s.shape.array_dims.lo < 1) s.shape.array_dims.lo = 1;
  if (s.shape.array_dims.lo > 2) s.shape.array_dims.lo = 2;
  if (s.shape.array_dims.hi < s.shape.array_dims.lo) s.shape.array_dims.hi = s.shape.array_dims.lo;
  if (s.shape.array_dims.hi > 2) s.shape.array_dims.hi = 2;
  if (s.shape.locals.lo < 2) s.shape.locals.lo = 2;
  if (s.shape.locals.lo > 16) s.shape.locals.lo = 16;
  if (s.shape.locals.hi < s.shape.locals.lo) s.shape.locals.hi = s.shape.locals.lo;
  if (s.shape.locals.hi > 16) s.shape.locals.hi = 16;
  if (s.shape.params.lo < 1) s.shape.params.lo = 1;
  if (s.shape.params.lo > 4) s.shape.params.lo = 4;
  if (s.shape.params.hi < s.shape.params.lo) s.shape.params.hi = s.shape.params.lo;
  if (s.shape.params.hi > 4) s.shape.params.hi = 4;
  synth_init_models(&s);
  synth_emit_program(&s);
  return ferror(out) ? 1 : 0;
}
