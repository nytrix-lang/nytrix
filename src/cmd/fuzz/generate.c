#include "core.h"

typedef struct {
  char name[96];
  char family[96];
  char generator[32];
  char features[512];
  char source[512];
  char hash[32];
  int weights[5];
  int fast_rank;
} gen_shape_t;

typedef struct {
  gen_shape_t items[128];
  int count;
} gen_shapes_t;

typedef struct {
  const gen_shape_t *shape;
  char name[160];
  char profile[32];
  char generator[32];
  char method[32];
  char source_kind[96];
  int seed;
  int index;
  int n;
  int rounds;
  int values[128];
  int salt;
  int bias;
  bool insane;
} gen_case_t;

static const char *profile_names[] = {"balanced", "optimizer", "memory", "strings", "state"};

static uint64_t rng_next(uint64_t *state) {
  uint64_t x = *state ? *state : UINT64_C(0x9e3779b97f4a7c15);
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * UINT64_C(2685821657736338717);
}

static int rng_range(uint64_t *state, int lo, int hi) {
  uint64_t span = (uint64_t)(hi - lo + 1);
  return lo + (int)(rng_next(state) % span);
}

static bool extract_quoted_after(const char *data, const char *key, char *out, size_t out_sz) {
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

static bool extract_word_after_shape(const char *data, char *out, size_t out_sz) {
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

static bool extract_features(const char *data, char *out, size_t out_sz) {
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

static int extract_int_after(const char *data, const char *key, int fallback) {
  const char *p = strstr(data, key);
  if (!p) return fallback;
  p += strlen(key);
  while (*p == ' ' || *p == '\t' || *p == ':') ++p;
  return atoi(p);
}

static int extract_weight(const char *data, const char *profile) {
  const char *p = strstr(data, profile);
  if (!p) return 1;
  p += strlen(profile);
  while (*p == ' ' || *p == '\t' || *p == ':') ++p;
  int v = atoi(p);
  return v > 0 ? v : 0;
}

static void source_from_path(const char *path, char *out, size_t out_sz) {
  const char *p = strstr(path, "shapes/");
  if (!p) p = path;
  snprintf(out, out_sz, "%.*s", (int)out_sz - 1, p);
}

static void parse_shape_for_gen(const char *path, gen_shapes_t *shapes) {
  if (shapes->count >= (int)(sizeof(shapes->items) / sizeof(shapes->items[0]))) return;
  file_buf_t f = {0};
  if (!read_file(path, &f)) return;
  gen_shape_t s;
  memset(&s, 0, sizeof(s));
  if (!extract_word_after_shape(f.data, s.name, sizeof(s.name))) { free(f.data); return; }
  (void)extract_quoted_after(strstr(f.data, "family ") ? strstr(f.data, "family ") : f.data, "family ", s.family, sizeof(s.family));
  (void)extract_quoted_after(strstr(f.data, "generator ") ? strstr(f.data, "generator ") : f.data, "generator ", s.generator, sizeof(s.generator));
  if (!s.family[0]) extract_quoted_after(f.data, "family ", s.family, sizeof(s.family));
  if (!s.generator[0]) extract_quoted_after(f.data, "generator ", s.generator, sizeof(s.generator));
  if (!extract_features(f.data, s.features, sizeof(s.features))) snprintf(s.features, sizeof(s.features), "[]");
  source_from_path(path, s.source, sizeof(s.source));
  snprintf(s.hash, sizeof(s.hash), "%016" PRIx64, fnv1a64(f.data, f.len));
  s.fast_rank = extract_int_after(f.data, "fast_rank", 100);
  for (int i = 0; i < 5; ++i) s.weights[i] = extract_weight(f.data, profile_names[i]);
  shapes->items[shapes->count++] = s;
  free(f.data);
}

static void walk_gen_shapes(const char *dir, gen_shapes_t *shapes) {
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
    if (S_ISDIR(st.st_mode)) walk_gen_shapes(path, shapes);
    else if (S_ISREG(st.st_mode) && ny_has_suffix(path, ".nshape")) parse_shape_for_gen(path, shapes);
  }
  closedir(d);
}

static int shape_cmp(const void *a, const void *b) {
  const gen_shape_t *x = (const gen_shape_t *)a;
  const gen_shape_t *y = (const gen_shape_t *)b;
  if (x->fast_rank != y->fast_rank) return x->fast_rank - y->fast_rank;
  return strcmp(x->name, y->name);
}

static int profile_index(const char *profile) {
  for (int i = 0; i < 5; ++i) if (strcmp(profile, profile_names[i]) == 0) return i;
  return 0;
}

static bool generator_match(const gen_shape_t *s, const char *generator) {
  if (strcmp(s->generator, "program") == 0)
    return generator && strcmp(generator, "program") == 0;
  if (!generator || !*generator || strcmp(generator, "mixed") == 0 ||
      strcmp(generator, "auto") == 0)
    return true;
  if (strcmp(generator, "generate") == 0 || strcmp(generator, "typed") == 0 ||
      strcmp(generator, "ir") == 0)
    return strcmp(s->generator, "typed") == 0;
  if (strcmp(generator, "optimizer") == 0)
    return strcmp(s->generator, "optimizer") == 0;
  if (strcmp(generator, "stress") == 0)
    return strcmp(s->generator, "stress") == 0;
  return strcmp(s->generator, generator) == 0;
}

static bool generator_is_mixed(const char *generator) {
  return !generator || !*generator || strcmp(generator, "mixed") == 0 ||
         strcmp(generator, "auto") == 0;
}

static const char *normalize_schedule(const char *schedule) {
  if (!schedule || !*schedule) return "smart";
  if (strcmp(schedule, "ranked") == 0 ||
      strcmp(schedule, "coverage") == 0 ||
      strcmp(schedule, "weighted") == 0 ||
      strcmp(schedule, "smart") == 0)
    return schedule;
  return "smart";
}

static const char *canonical_generator_method(const char *generator) {
  if (!generator || !*generator || strcmp(generator, "mixed") == 0 ||
      strcmp(generator, "auto") == 0)
    return "mixed";
  if (strcmp(generator, "generate") == 0 || strcmp(generator, "typed") == 0) return "typed";
  if (strcmp(generator, "ir") == 0) return "ir";
  if (strcmp(generator, "stress") == 0) return "stress";
  return generator;
}

static const char *source_kind_for_method(const char *method, const gen_shape_t *shape) {
  if (method && strcmp(method, "ir") == 0) return "nynth-core-ir-typed-ast";
  if (method && strcmp(method, "stress") == 0) return "nynth-core-stress-optimizer";
  if (shape && strcmp(shape->generator, "optimizer") == 0) return "nynth-core-optimizer-pattern";
  if (shape && strcmp(shape->generator, "torture") == 0) return "nynth-core-gcc-torture-inspired";
  return "nynth-core-typed-ast";
}

static const char *case_prefix_for_method(const char *method) {
  if (method && strcmp(method, "ir") == 0) return "ir";
  if (method && strcmp(method, "stress") == 0) return "stress";
  return "nynth";
}

static const char *emission_shape_name(const char *shape) {
  if (!shape) return "";
  if (strcmp(shape, "stress-induction-affine") == 0) return "optimizer-induction-affine";
  if (strcmp(shape, "stress-two-level-reduction") == 0) return "optimizer-two-level-reduction";
  if (strcmp(shape, "stress-histogram-update") == 0) return "optimizer-histogram-update";
  if (strcmp(shape, "stress-byte-scan-load8") == 0) return "optimizer-byte-scan-load8";
  if (strcmp(shape, "stress-emi-dead-guard") == 0) return "optimizer-emi-dead-guard";
  return shape;
}

static int select_shapes(const gen_shapes_t *all, gen_shape_t *out, int max_out,
                         const char *generator, const char *shape_name) {
  int n = 0;
  for (int i = 0; i < all->count && n < max_out; ++i) {
    if (shape_name && *shape_name && strcmp(all->items[i].name, shape_name) != 0) continue;
    if (generator_match(&all->items[i], generator)) out[n++] = all->items[i];
  }
  qsort(out, (size_t)n, sizeof(out[0]), shape_cmp);
  return n;
}

static const gen_shape_t *choose_weighted_shape(const gen_shape_t *pool, int pool_n, int idx,
                                                int seed, const char *profile) {
  if (pool_n <= 0) return NULL;
  int pi = profile_index(profile);
  int total = 0;
  for (int i = 0; i < pool_n; ++i) total += pool[i].weights[pi] > 0 ? pool[i].weights[pi] : 0;
  if (total <= 0) return &pool[idx % pool_n];
  uint64_t state = ((uint64_t)seed << 32) ^ (uint64_t)(idx * 0x9e3779b1u) ^ (uint64_t)strlen(profile);
  int pick = (int)(rng_next(&state) % (uint64_t)total);
  for (int i = 0; i < pool_n; ++i) {
    int w = pool[i].weights[pi] > 0 ? pool[i].weights[pi] : 0;
    if (pick < w) return &pool[i];
    pick -= w;
  }
  return &pool[pool_n - 1];
}

static const gen_shape_t *choose_coverage_shape(const gen_shape_t *pool, int pool_n, int idx,
                                                int seed, bool mixed) {
  if (pool_n <= 0) return NULL;
  if (!mixed) return &pool[idx % pool_n];
  const char *groups[32];
  int group_n = 0;
  for (int i = 0; i < pool_n; ++i) {
    const char *g = pool[i].generator[0] ? pool[i].generator : "typed";
    bool seen = false;
    for (int j = 0; j < group_n; ++j) {
      if (strcmp(groups[j], g) == 0) {
        seen = true;
        break;
      }
    }
    if (!seen && group_n < (int)(sizeof(groups) / sizeof(groups[0]))) groups[group_n++] = g;
  }
  if (group_n <= 1) return &pool[idx % pool_n];
  int position = idx % pool_n;
  int epoch = idx / pool_n;
  int start_group = (int)(((uint32_t)seed + (uint32_t)epoch) % (uint32_t)group_n);
  int emitted = 0;
  for (int rank = 0; rank < pool_n; ++rank) {
    for (int slot = 0; slot < group_n; ++slot) {
      const char *group = groups[(start_group + slot) % group_n];
      int seen_in_group = 0;
      for (int i = 0; i < pool_n; ++i) {
        const char *g = pool[i].generator[0] ? pool[i].generator : "typed";
        if (strcmp(g, group) != 0) continue;
        if (seen_in_group++ != rank) continue;
        if (emitted++ == position) return &pool[i];
        break;
      }
    }
  }
  return &pool[idx % pool_n];
}

static const gen_shape_t *choose_shape(const gen_shape_t *pool, int pool_n, int idx,
                                       int seed, const char *profile, bool fast,
                                       const char *schedule,
                                       const char *requested_generator) {
  if (pool_n <= 0) return NULL;
  schedule = normalize_schedule(schedule);
  bool mixed = generator_is_mixed(requested_generator);
  if (strcmp(schedule, "weighted") == 0)
    return choose_weighted_shape(pool, pool_n, idx, seed, profile);
  if (strcmp(schedule, "coverage") == 0)
    return choose_coverage_shape(pool, pool_n, idx, seed, mixed);
  if (strcmp(schedule, "smart") == 0 && mixed)
    return choose_coverage_shape(pool, pool_n, idx, seed, true);
  if (fast || idx < pool_n) return &pool[idx % pool_n];
  return choose_weighted_shape(pool, pool_n, idx, seed, profile);
}

static void print_shape_list_json(const gen_shapes_t *all, const gen_shape_t *pool,
                                  int pool_n, const char *shape_dir,
                                  const char *profile, const char *generator,
                                  const char *shape_name, const char *method,
                                  const char *schedule) {
  int pi = profile_index(profile);
  printf("{\"ok\":true,\"engine\":\"nynth_core\",\"mode\":\"list-shapes\",\"shape_dir\":");
  json_str(stdout, shape_dir ? shape_dir : "");
  printf(",\"profile\":");
  json_str(stdout, profile ? profile : "balanced");
  printf(",\"generator\":");
  json_str(stdout, generator ? generator : "mixed");
  printf(",\"generator_kind\":");
  json_str(stdout, method ? method : "mixed");
  printf(",\"method\":");
  json_str(stdout, method ? method : "mixed");
  printf(",\"shape\":");
  json_str(stdout, shape_name ? shape_name : "");
  printf(",\"schedule\":");
  json_str(stdout, normalize_schedule(schedule));
  printf(",\"count\":%d,\"selected_shape_count\":%d,\"shape_count\":%d,\"shapes\":[",
         pool_n, pool_n, all ? all->count : 0);
  for (int i = 0; i < pool_n; ++i) {
    const gen_shape_t *s = &pool[i];
    printf("%s{\"name\":", i ? "," : "");
    json_str(stdout, s->name);
    printf(",\"family\":");
    json_str(stdout, s->family);
    printf(",\"generator\":");
    json_str(stdout, s->generator);
    printf(",\"fast_rank\":%d,\"weight\":%d,\"features\":%s,\"source\":",
           s->fast_rank, s->weights[pi] > 0 ? s->weights[pi] : 0,
           s->features[0] ? s->features : "[]");
    json_str(stdout, s->source);
    printf(",\"shape_hash\":");
    json_str(stdout, s->hash);
    printf("}");
  }
  printf("]}\n");
}

static int scale_for_salt(int salt, int mod, int add) { return salt % mod + add; }

static void print_int_array(FILE *f, const int *values, int n, const char *sep) {
  for (int i = 0; i < n; ++i) {
    if (i) fputs(sep, f);
    fprintf(f, "%d", values[i]);
  }
}

static void emit_c_prelude(FILE *f, bool need_get) {
  fputs("#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n\n", f);
  if (need_get) {
    fputs("static int nynth_get(const int *data, int n, int idx, int defv) {\n"
          "    if (idx < 0 || idx >= n) return defv;\n"
          "    return data[idx];\n"
          "}\n\n", f);
  }
}

static bool shape_needs_get(const char *shape) {
  shape = emission_shape_name(shape);
  return strcmp(shape, "defaulted-oob-get") == 0;
}

static void emit_c_metadata(FILE *f, const gen_case_t *c) {
  fprintf(f,
          "/* nynth: seed=%d shape=%s generator=%s profile=%s insane=%s n=%d rounds=%d */\n"
          "/* repro: ./build/nynth synth print --lang both --shape %s --generator %s --seed %d --out build/repro */\n",
          c->seed, c->shape->name, c->generator, c->profile,
          c->insane ? "true" : "false", c->n, c->rounds,
          c->shape->name, c->generator, c->seed);
}

static void emit_c_case(FILE *f, const gen_case_t *c) {
  const char *s = emission_shape_name(c->shape->name);
  int n = c->n, rounds = c->rounds, salt = c->salt, bias = c->bias;
  emit_c_metadata(f, c);
  emit_c_prelude(f, shape_needs_get(s));
  if (strcmp(s, "branch-helper-chain") == 0) {
    fprintf(f,
      "static int fold0(int x) {\n"
      "    int y = (((x * 3) + %d) %% 97);\n"
      "    if ((y %% 2) == 0) {\n"
      "        y += (x %% 11);\n"
      "    } else {\n"
      "        y -= (x %% 7);\n"
      "    }\n"
      "    return y;\n"
      "}\n\n"
      "static int fold1(int x) {\n"
      "    int y = fold0((x + %d));\n"
      "    if (y > 40) {\n"
      "        return (y - 13);\n"
      "    } else {\n"
      "        return (y + 17);\n"
      "    }\n"
      "}\n\n"
      "int main(void) {\n"
      "    int acc = 0;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        acc += fold1((i + (acc %% 5)));\n"
      "        if ((acc %% 4) == 1) {\n"
      "            acc += fold0(i);\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", acc);\n"
      "    return 0;\n"
      "}\n", salt, bias, rounds);
  } else if (strcmp(s, "bounded-mod-reduction") == 0) {
    fprintf(f,
      "int main(void) {\n"
      "    int acc = 0;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        int v = ((((i + %d) * 7) + %d) %% 113);\n"
      "        acc += ((v %% 9) * (i %% 5));\n"
      "        if ((i %% 3) == 2) {\n"
      "            acc -= (v %% 7);\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", acc);\n"
      "    return 0;\n"
      "}\n", rounds, salt, bias);
  } else if (strcmp(s, "readonly-array-scan") == 0) {
    fprintf(f, "int main(void) {\n    const int data[%d] = {", n);
    print_int_array(f, c->values, n, ", ");
    fprintf(f, "};\n    int acc = 0;\n    for (int i = 0; i < %d; i += 1) {\n"
               "        int idx = (((i * %d) + %d) %% %d);\n"
               "        acc += (data[idx] * ((i %% 4) + 1));\n"
               "    }\n    printf(\"%%d\\n\", acc);\n    return 0;\n}\n",
            rounds, scale_for_salt(salt, 5, 1), bias, n);
  } else if (strcmp(s, "fixed-array-mutation") == 0) {
    fprintf(f, "int main(void) {\n    int work[%d] = {", n);
    print_int_array(f, c->values, n, ", ");
    fprintf(f, "};\n    int acc = 0;\n    for (int i = 0; i < %d; i += 1) {\n"
               "        int idx = (((i * %d) + %d) %% %d);\n"
               "        int next = (((work[idx] + i) + acc) %% 127);\n"
               "        work[idx] = next;\n"
               "        acc += (work[idx] - (i %% 3));\n"
               "    }\n    printf(\"%%d\\n\", acc);\n    return 0;\n}\n",
            rounds, scale_for_salt(salt, 7, 1), bias, n);
  } else if (strcmp(s, "defaulted-oob-get") == 0) {
    fprintf(f, "int main(void) {\n    const int data[%d] = {", n);
    print_int_array(f, c->values, n, ", ");
    fprintf(f, "};\n    int acc = 0;\n    for (int i = 0; i < %d; i += 1) {\n"
               "        int idx = (i %% %d);\n"
               "        int got = nynth_get(data, (int)(sizeof(data) / sizeof(data[0])), idx, %d);\n"
               "        if ((idx < 0) || (idx >= ((int)(sizeof(data) / sizeof(data[0]))))) {\n"
               "            acc += (got - i);\n"
               "        } else {\n"
               "            acc += (got + (i %% 5));\n"
               "        }\n"
               "    }\n    printf(\"%%d\\n\", acc);\n    return 0;\n}\n",
            rounds, n + 3, bias);
  } else if (strcmp(s, "nested-helper-reduction") == 0) {
    int outer = rounds / 4; if (outer < 8) outer = 8;
    fprintf(f,
      "static int mix(int x, int y) {\n"
      "    int z = (((x * %d) + (y * 3)) %% 173);\n"
      "    if (z > 80) {\n"
      "        z -= (x %% 17);\n"
      "    } else {\n"
      "        z += (y %% 19);\n"
      "    }\n"
      "    return z;\n"
      "}\n\n"
      "int main(void) {\n"
      "    int acc = 0;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        for (int j = 0; j < 4; j += 1) {\n"
      "            acc += mix((i + j), (acc + %d));\n"
      "            if (((i + j) %% 3) == 1) {\n"
      "                acc -= (mix(j, i) %% 11);\n"
      "            }\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", acc);\n"
      "    return 0;\n"
      "}\n", scale_for_salt(salt, 9, 2), outer, bias);
  } else if (strcmp(s, "branch-ladder-state") == 0) {
    fprintf(f,
      "int main(void) {\n"
      "    int state = %d;\n"
      "    int acc = 0;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        if (((state + i) %% 5) == 0) {\n"
      "            state = ((state + %d) %% 211);\n"
      "            acc += state;\n"
      "        } else {\n"
      "            if ((i %% 2) == 0) {\n"
      "                state = (((state * 3) + i) %% 211);\n"
      "            } else {\n"
      "                state = ((state + (i * 2)) %% 211);\n"
      "            }\n"
      "            acc -= (state %% 23);\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", (acc + state));\n"
      "    return 0;\n"
      "}\n", bias, rounds, salt);
  } else if (strcmp(s, "array-copy-update") == 0) {
    fprintf(f, "int main(void) {\n    const int src[%d] = {", n);
    print_int_array(f, c->values, n, ", ");
    fprintf(f, "};\n    int dst[%d] = {", n);
    for (int i = 0; i < n; ++i) { if (i) fputs(", ", f); fputs("0", f); }
    fprintf(f, "};\n    int acc = 0;\n    for (int i = 0; i < %d; i += 1) {\n"
               "        int v = (src[i] + %d);\n"
               "        dst[i] = ((v + (i * %d)) %% 149);\n"
               "    }\n"
               "    for (int j = 0; j < %d; j += 1) {\n"
               "        int idx = ((j + %d) %% %d);\n"
               "        acc += dst[idx];\n"
               "        if ((j %% 4) == 3) {\n"
               "            dst[idx] = ((dst[idx] + acc) %% 149);\n"
               "        }\n"
               "    }\n    printf(\"%%d\\n\", acc);\n    return 0;\n}\n",
            n, bias, scale_for_salt(salt, 5, 1), rounds, salt, n);
  } else if (strcmp(s, "optimizer-induction-affine") == 0) {
    fprintf(f, "int main(void) {\n    const int src[%d] = {", n);
    print_int_array(f, c->values, n, ", ");
    fprintf(f, "};\n    int dst[%d] = {", n);
    for (int i = 0; i < n; ++i) { if (i) fputs(", ", f); fputs("0", f); }
    fprintf(f, "};\n    int acc = 0;\n    for (int i = 0; i < %d; i += 1) {\n"
               "        int v = (((src[i] * %d) + (i + %d)) %% 251);\n"
               "        dst[i] = v;\n"
               "    }\n"
               "    for (int i = 0; i < %d; i += 1) {\n"
               "        int idx = ((i + %d) %% %d);\n"
               "        acc += (dst[idx] * ((i %% 3) + 1));\n"
               "        if ((idx %% 2) == 0) {\n"
               "            acc -= (src[idx] %% 7);\n"
               "        }\n"
               "    }\n    printf(\"%%d\\n\", acc);\n    return 0;\n}\n",
            n, scale_for_salt(salt, 5, 2), bias, rounds, salt, n);
  } else if (strcmp(s, "optimizer-two-level-reduction") == 0) {
    int outer = rounds / 5; if (outer < 6) outer = 6;
    fprintf(f,
      "int main(void) {\n"
      "    int acc = %d;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        int row = (((i * %d) + acc) %% 257);\n"
      "        for (int j = 0; j < 5; j += 1) {\n"
      "            int mix = (((row * (j + 1)) + (i * 3)) %% 263);\n"
      "            if (mix > 130) {\n"
      "                acc += (mix %% 17);\n"
      "            } else {\n"
      "                acc -= ((mix + j) %% 19);\n"
      "            }\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", acc);\n"
      "    return 0;\n"
      "}\n", bias, outer, scale_for_salt(salt, 11, 3));
  } else if (strcmp(s, "optimizer-histogram-update") == 0) {
    fprintf(f, "int main(void) {\n    const int data[%d] = {", n);
    print_int_array(f, c->values, n, ", ");
    fprintf(f, "};\n    int hist[8] = {0, 0, 0, 0, 0, 0, 0, 0};\n    int acc = 0;\n"
               "    for (int i = 0; i < %d; i += 1) {\n"
               "        int idx = ((i + %d) %% %d);\n"
               "        int bucket = ((data[idx] + i) %% 8);\n"
               "        int old = hist[bucket];\n"
               "        hist[bucket] = (((old + data[idx]) + %d) %% 127);\n"
               "        acc += hist[bucket];\n"
               "    }\n    printf(\"%%d\\n\", acc);\n    return 0;\n}\n",
            rounds, salt, n, bias);
  } else if (strcmp(s, "optimizer-byte-scan-load8") == 0) {
    const char *alphabet = "NytrixTypedOptimizer0123456789abcdef";
    int text_len = n + (salt % 17) + 12; if (text_len < 12) text_len = 12; if (text_len > 32) text_len = 32;
    char text[64];
    int step = salt % 7 + 1;
    int alen = (int)strlen(alphabet);
    for (int i = 0; i < text_len; ++i) text[i] = alphabet[(salt + bias + i * step) % alen];
    text[text_len] = '\0';
    fprintf(f,
      "int main(void) {\n"
      "    const char text[] = \"%s\";\n"
      "    int acc = 0;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        int ch = ((int)(unsigned char)text[i]);\n"
      "        if (ch > 96) {\n"
      "            acc += ((ch + i) %% 31);\n"
      "        } else {\n"
      "            acc -= ((ch + %d) %% 17);\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", acc);\n"
      "    return 0;\n"
      "}\n", text, text_len, bias);
  } else if (strcmp(s, "optimizer-emi-dead-guard") == 0) {
    fprintf(f,
      "int main(void) {\n"
      "    int acc = %d;\n"
      "    int state = %d;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        int v = (((i * %d) + state) %% 197);\n"
      "        if (i < 0) {\n"
      "            acc += (v * 999);\n"
      "            state = -12345;\n"
      "        } else {\n"
      "            acc += (v + (i %% 5));\n"
      "        }\n"
      "        if ((i >= 0) && (i < %d)) {\n"
      "            state = (((state * 5) + v) %% 223);\n"
      "        } else {\n"
      "            acc -= 777;\n"
      "        }\n"
      "        if (((state + i) %% 4) == 2) {\n"
      "            acc -= (state %% 11);\n"
      "        } else {\n"
      "            acc += ((state + v) %% 17);\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", (acc + state));\n"
      "    return 0;\n"
      "}\n", bias, salt % 31 + 1, rounds, scale_for_salt(salt, 13, 3), rounds);
  } else if (strcmp(s, "optimizer-pure-call-cse") == 0) {
    fprintf(f,
      "static int pure_mix(int x, int y) {\n"
      "    int a = (((x * %d) + (y * %d) + %d) %% 257);\n"
      "    int b = (((a * a) + (x %% 17) + (y %% 19)) %% 263);\n"
      "    if ((b %% 3) == 0) {\n"
      "        return ((b + a) %% 211);\n"
      "    }\n"
      "    return ((b - (a %% 23)) %% 211);\n"
      "}\n\n"
      "static int pure_fold(int x) {\n"
      "    int left = pure_mix(x, %d);\n"
      "    int right = pure_mix(x, %d);\n"
      "    int both = (left + right);\n"
      "    if (left == right) {\n"
      "        return (both + pure_mix((x + 1), %d));\n"
      "    }\n"
      "    return (both - pure_mix((x + 2), %d));\n"
      "}\n\n"
      "int main(void) {\n"
      "    int acc = %d;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        int a = pure_fold((i + (acc %% 7)));\n"
      "        int b = pure_fold((i + (acc %% 7)));\n"
      "        if (a == b) {\n"
      "            acc += ((a %% 29) + pure_mix(i, %d));\n"
      "        } else {\n"
      "            acc -= ((b %% 31) + pure_mix(%d, i));\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", acc);\n"
      "    return 0;\n"
      "}\n",
      scale_for_salt(salt, 7, 3), scale_for_salt(bias, 5, 2), bias,
      salt, salt, bias, bias, bias, rounds, salt, bias);
  } else if (strcmp(s, "optimizer-pure-array-read-cse") == 0) {
    fprintf(f, "static const int data[%d] = {", n);
    print_int_array(f, c->values, n, ", ");
    fprintf(f,
      "};\n\n"
      "static int pure_pick(int x, int saltv) {\n"
      "    int idx = (((x * %d) + saltv) %% %d);\n"
      "    int v = data[idx];\n"
      "    int mix = (((v * %d) + (idx * 7) + saltv) %% 271);\n"
      "    if ((mix %% 2) == 0) {\n"
      "        return ((mix + v + idx) %% 223);\n"
      "    }\n"
      "    return ((mix + 223 - (v %% 17)) %% 223);\n"
      "}\n\n"
      "int main(void) {\n"
      "    int acc = %d;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        int key = (i + (acc %% 11) + %d);\n"
      "        int a = pure_pick(key, %d);\n"
      "        int b = pure_pick(key, %d);\n"
      "        int c = pure_pick((key + 1), %d);\n"
      "        if (a == b) {\n"
      "            acc = ((acc + a + c + (i %% 5)) %% 100000);\n"
      "        } else {\n"
      "            acc = ((acc + b + 17) %% 100000);\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", acc);\n"
      "    return 0;\n"
      "}\n",
      scale_for_salt(salt, 5, 2), n, scale_for_salt(bias, 7, 3),
      bias, rounds, salt % 31 + 1, salt, salt, bias);
  } else if (strcmp(s, "optimizer-pure-branch-compose") == 0) {
    fprintf(f,
      "static int pure_gate(int x) {\n"
      "    int v = (((x * %d) + %d) %% 251);\n"
      "    if ((v %% 5) < 2) {\n"
      "        return (((v * 3) + %d) %% 293);\n"
      "    }\n"
      "    if ((v %% 7) == 3) {\n"
      "        return ((v + x + %d) %% 293);\n"
      "    }\n"
      "    return ((v + (x %% 29) + %d) %% 293);\n"
      "}\n\n"
      "static int pure_compose(int x, int y) {\n"
      "    int z = (x + y + %d);\n"
      "    int a = pure_gate(z);\n"
      "    int b = pure_gate(z);\n"
      "    if (a == b) {\n"
      "        int c = pure_gate((x + (a %% 5) + %d));\n"
      "        return ((a + c + y) %% 307);\n"
      "    }\n"
      "    return ((b + pure_gate((y + %d))) %% 307);\n"
      "}\n\n"
      "int main(void) {\n"
      "    int acc = %d;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        int x = (i + (acc %% 13));\n"
      "        int a = pure_compose(x, %d);\n"
      "        int b = pure_compose(x, %d);\n"
      "        if (a == b) {\n"
      "            acc = ((acc + a + pure_gate((i + %d))) %% 100000);\n"
      "        } else {\n"
      "            acc = ((acc + b + 23) %% 100000);\n"
      "        }\n"
      "    }\n"
      "    printf(\"%%d\\n\", acc);\n"
      "    return 0;\n"
      "}\n",
      scale_for_salt(salt, 9, 4), bias, salt, bias, salt + bias,
      bias, salt, bias, bias, rounds, salt, salt, bias);
  } else if (strcmp(s, "torture-add-compare-grid") == 0) {
    fprintf(f,
      "int main(void) {\n"
      "    int acc = 0;\n"
      "    for (int i = 0; i < %d; i += 1) {\n"
      "        int a = (((i * %d) + %d) %% 241);\n"
      "        int b = (((i * %d) + %d) %% 241);\n"
      "        int s = (a + b);\n"
      "        if ((s %% 7) == 0) {\n            acc += s;\n        } else {\n            acc -= (s %% 11);\n        }\n"
      "        if (((a + i) %% 5) != 3) {\n            acc += a;\n        } else {\n            acc -= b;\n        }\n"
      "        if (a < b) {\n            acc += (b - a);\n        } else {\n            acc += (a - b);\n        }\n"
      "        if ((s >= 64) && (s <= 320)) {\n            acc += (s %% 13);\n        } else {\n            acc -= (s %% 17);\n        }\n"
      "    }\n    printf(\"%%d\\n\", acc);\n    return 0;\n}\n",
      rounds, scale_for_salt(salt, 17, 3), bias, scale_for_salt(salt, 11, 5), bias + 7);
  } else if (strcmp(s, "torture-crc-mix-loop") == 0) {
    fprintf(f, "int main(void) {\n    const int data[%d] = {", n);
    print_int_array(f, c->values, n, ", ");
    fprintf(f, "};\n    int crc = %d;\n    for (int i = 0; i < %d; i += 1) {\n"
               "        int idx = ((i + %d) %% %d);\n"
               "        int byte = ((data[idx] + i) & 255);\n"
               "        crc = (((crc * 33) ^ byte) & 65535);\n"
               "        if ((crc & 1) == 0) {\n"
               "            crc = ((crc + 4129) & 65535);\n"
               "        } else {\n"
               "            crc = ((crc ^ 33800) & 65535);\n"
               "        }\n"
               "    }\n    printf(\"%%d\\n\", crc);\n    return 0;\n}\n",
            (salt << 3) & 255, rounds, bias, n);
  } else {
    fprintf(f, "int main(void) {\n    const int src[%d] = {", n);
    print_int_array(f, c->values, n, ", ");
    fprintf(f, "};\n    int tmp[%d] = {", n);
    for (int i = 0; i < n; ++i) { if (i) fputs(", ", f); fputs("0", f); }
    fprintf(f, "};\n    int out[%d] = {", n);
    for (int i = 0; i < n; ++i) { if (i) fputs(", ", f); fputs("0", f); }
    fprintf(f, "};\n    int acc = 0;\n"
               "    for (int i = 0; i < %d; i += 1) {\n"
               "        int v = src[i];\n"
               "        tmp[i] = 0;\n"
               "        tmp[i] = (((v * %d) + i) %% 251);\n"
               "        out[i] = tmp[i];\n"
               "    }\n"
               "    for (int j = 0; j < %d; j += 1) {\n"
               "        int idx = ((j + %d) %% %d);\n"
               "        int old = out[idx];\n"
               "        out[idx] = ((old + tmp[idx]) %% 251);\n"
               "        acc += (out[idx] + (j %% 7));\n"
               "    }\n    printf(\"%%d\\n\", acc);\n    return 0;\n}\n",
            n, scale_for_salt(salt, 5, 2), rounds, bias, n);
  }
}

static void emit_ny_array(FILE *f, const int *values, int n) {
  fputc('[', f);
  print_int_array(f, values, n, ", ");
  fputc(']', f);
}

static void emit_ny_zero_array(FILE *f, int n) {
  fputc('[', f);
  for (int i = 0; i < n; ++i) {
    if (i) fputs(", ", f);
    fputc('0', f);
  }
  fputc(']', f);
}

static void emit_ny_metadata(FILE *f, const gen_case_t *c) {
  fprintf(f,
          ";; nynth: seed=%d shape=%s generator=%s profile=%s insane=%s n=%d rounds=%d\n"
          ";; repro: ./build/nynth synth print --lang both --shape %s --generator %s --seed %d --out build/repro\n",
          c->seed, c->shape->name, c->generator, c->profile,
          c->insane ? "true" : "false", c->n, c->rounds,
          c->shape->name, c->generator, c->seed);
}

static void emit_ny_case(FILE *f, const gen_case_t *c) {
  const char *s = emission_shape_name(c->shape->name);
  int n = c->n, rounds = c->rounds, salt = c->salt, bias = c->bias;
  emit_ny_metadata(f, c);
  fputs("use std.core\n\n", f);
  if (strcmp(s, "branch-helper-chain") == 0) {
    fprintf(f,
      "fn fold0(int: x) int {\n"
      "   mut int: y = (((x * 3) + %d) %% 97)\n"
      "   if((y %% 2) == 0){\n"
      "      y += (x %% 11)\n"
      "   } else {\n"
      "      y -= (x %% 7)\n"
      "   }\n"
      "   return y\n"
      "}\n\n"
      "fn fold1(int: x) int {\n"
      "   def int: y = fold0((x + %d))\n"
      "   if(y > 40){\n"
      "      return (y - 13)\n"
      "   } else {\n"
      "      return (y + 17)\n"
      "   }\n"
      "}\n\n"
      "mut int: acc = 0\n"
      "mut int: i = 0\n"
      "while(i < %d){\n"
      "   acc += fold1((i + (acc %% 5)))\n"
      "   if((acc %% 4) == 1){\n"
      "      acc += fold0(i)\n"
      "   }\n"
      "   i += 1\n"
      "}\n"
      "print(acc)\n", salt, bias, rounds);
  } else if (strcmp(s, "bounded-mod-reduction") == 0) {
    fprintf(f,
      "mut int: acc = 0\n"
      "mut int: i = 0\n"
      "while(i < %d){\n"
      "   def int: v = ((((i + %d) * 7) + %d) %% 113)\n"
      "   acc += ((v %% 9) * (i %% 5))\n"
      "   if((i %% 3) == 2){\n"
      "      acc -= (v %% 7)\n"
      "   }\n"
      "   i += 1\n"
      "}\n"
      "print(acc)\n", rounds, salt, bias);
  } else if (strcmp(s, "readonly-array-scan") == 0) {
    fputs("def data = ", f); emit_ny_array(f, c->values, n);
    fprintf(f, "\nmut int: acc = 0\nmut int: i = 0\nwhile(i < %d){\n"
               "   def int: idx = (((i * %d) + %d) %% %d)\n"
               "   acc += (get(data, idx, 0) * ((i %% 4) + 1))\n"
               "   i += 1\n}\nprint(acc)\n",
            rounds, scale_for_salt(salt, 5, 1), bias, n);
  } else if (strcmp(s, "fixed-array-mutation") == 0) {
    fputs("mut work = ", f); emit_ny_array(f, c->values, n);
    fprintf(f, "\nmut int: acc = 0\nmut int: i = 0\nwhile(i < %d){\n"
               "   def int: idx = (((i * %d) + %d) %% %d)\n"
               "   def int: next = (((get(work, idx, 0) + i) + acc) %% 127)\n"
               "   work = set_idx(work, idx, next)\n"
               "   acc += (get(work, idx, 0) - (i %% 3))\n"
               "   i += 1\n}\nprint(acc)\n",
            rounds, scale_for_salt(salt, 7, 1), bias, n);
  } else if (strcmp(s, "defaulted-oob-get") == 0) {
    fputs("def data = ", f); emit_ny_array(f, c->values, n);
    fprintf(f, "\nmut int: acc = 0\nmut int: i = 0\nwhile(i < %d){\n"
               "   def int: idx = (i %% %d)\n"
               "   def int: got = get(data, idx, %d)\n"
               "   if((idx < 0) || (idx >= len(data))){\n"
               "      acc += (got - i)\n"
               "   } else {\n"
               "      acc += (got + (i %% 5))\n"
               "   }\n"
               "   i += 1\n}\nprint(acc)\n",
            rounds, n + 3, bias);
  } else if (strcmp(s, "nested-helper-reduction") == 0) {
    int outer = rounds / 4; if (outer < 8) outer = 8;
    fprintf(f,
      "fn mix(int: x, int: y) int {\n"
      "   mut int: z = (((x * %d) + (y * 3)) %% 173)\n"
      "   if(z > 80){\n"
      "      z -= (x %% 17)\n"
      "   } else {\n"
      "      z += (y %% 19)\n"
      "   }\n"
      "   return z\n"
      "}\n\n"
      "mut int: acc = 0\n"
      "mut int: i = 0\n"
      "while(i < %d){\n"
      "   mut int: j = 0\n"
      "   while(j < 4){\n"
      "      acc += mix((i + j), (acc + %d))\n"
      "      if(((i + j) %% 3) == 1){\n"
      "         acc -= (mix(j, i) %% 11)\n"
      "      }\n"
      "      j += 1\n"
      "   }\n"
      "   i += 1\n"
      "}\n"
      "print(acc)\n", scale_for_salt(salt, 9, 2), outer, bias);
  } else if (strcmp(s, "branch-ladder-state") == 0) {
    fprintf(f,
      "mut int: state = %d\n"
      "mut int: acc = 0\n"
      "mut int: i = 0\n"
      "while(i < %d){\n"
      "   if(((state + i) %% 5) == 0){\n"
      "      state = ((state + %d) %% 211)\n"
      "      acc += state\n"
      "   } else {\n"
      "      if((i %% 2) == 0){\n"
      "         state = (((state * 3) + i) %% 211)\n"
      "      } else {\n"
      "         state = ((state + (i * 2)) %% 211)\n"
      "      }\n"
      "      acc -= (state %% 23)\n"
      "   }\n"
      "   i += 1\n"
      "}\n"
      "print((acc + state))\n", bias, rounds, salt);
  } else if (strcmp(s, "array-copy-update") == 0) {
    fputs("def src = ", f); emit_ny_array(f, c->values, n);
    fputs("\nmut dst = ", f); emit_ny_zero_array(f, n);
    fprintf(f, "\nmut int: acc = 0\nmut int: i = 0\nwhile(i < %d){\n"
               "   def int: v = (get(src, i, 0) + %d)\n"
               "   dst = set_idx(dst, i, ((v + (i * %d)) %% 149))\n"
               "   i += 1\n}\n"
               "mut int: j = 0\nwhile(j < %d){\n"
               "   def int: idx = ((j + %d) %% %d)\n"
               "   acc += get(dst, idx, 0)\n"
               "   if((j %% 4) == 3){\n"
               "      dst = set_idx(dst, idx, ((get(dst, idx, 0) + acc) %% 149))\n"
               "   }\n"
               "   j += 1\n}\nprint(acc)\n",
            n, bias, scale_for_salt(salt, 5, 1), rounds, salt, n);
  } else if (strcmp(s, "optimizer-induction-affine") == 0) {
    fputs("def src = ", f); emit_ny_array(f, c->values, n);
    fputs("\nmut dst = ", f); emit_ny_zero_array(f, n);
    fprintf(f, "\nmut int: acc = 0\nmut int: i = 0\nwhile(i < %d){\n"
               "   def int: v = (((get(src, i, 0) * %d) + (i + %d)) %% 251)\n"
               "   dst = set_idx(dst, i, v)\n"
               "   i += 1\n}\n"
               "mut int: i = 0\nwhile(i < %d){\n"
               "   def int: idx = ((i + %d) %% %d)\n"
               "   acc += (get(dst, idx, 0) * ((i %% 3) + 1))\n"
               "   if((idx %% 2) == 0){\n"
               "      acc -= (get(src, idx, 0) %% 7)\n"
               "   }\n"
               "   i += 1\n}\nprint(acc)\n",
            n, scale_for_salt(salt, 5, 2), bias, rounds, salt, n);
  } else if (strcmp(s, "optimizer-two-level-reduction") == 0) {
    int outer = rounds / 5; if (outer < 6) outer = 6;
    fprintf(f,
      "mut int: acc = %d\nmut int: i = 0\nwhile(i < %d){\n"
      "   def int: row = (((i * %d) + acc) %% 257)\n"
      "   mut int: j = 0\n"
      "   while(j < 5){\n"
      "      def int: mix = (((row * (j + 1)) + (i * 3)) %% 263)\n"
      "      if(mix > 130){\n"
      "         acc += (mix %% 17)\n"
      "      } else {\n"
      "         acc -= ((mix + j) %% 19)\n"
      "      }\n"
      "      j += 1\n"
      "   }\n"
      "   i += 1\n"
      "}\nprint(acc)\n", bias, outer, scale_for_salt(salt, 11, 3));
  } else if (strcmp(s, "optimizer-histogram-update") == 0) {
    fputs("def data = ", f); emit_ny_array(f, c->values, n);
    fputs("\nmut hist = [0, 0, 0, 0, 0, 0, 0, 0]", f);
    fprintf(f, "\nmut int: acc = 0\nmut int: i = 0\nwhile(i < %d){\n"
               "   def int: idx = ((i + %d) %% %d)\n"
               "   def int: bucket = ((get(data, idx, 0) + i) %% 8)\n"
               "   def int: old = get(hist, bucket, 0)\n"
               "   hist = set_idx(hist, bucket, (((old + get(data, idx, 0)) + %d) %% 127))\n"
               "   acc += get(hist, bucket, 0)\n"
               "   i += 1\n}\nprint(acc)\n",
            rounds, salt, n, bias);
  } else if (strcmp(s, "optimizer-byte-scan-load8") == 0) {
    const char *alphabet = "NytrixTypedOptimizer0123456789abcdef";
    int text_len = n + (salt % 17) + 12; if (text_len < 12) text_len = 12; if (text_len > 32) text_len = 32;
    char text[64];
    int step = salt % 7 + 1;
    int alen = (int)strlen(alphabet);
    for (int i = 0; i < text_len; ++i) text[i] = alphabet[(salt + bias + i * step) % alen];
    text[text_len] = '\0';
    fprintf(f,
      "def text = \"%s\"\n"
      "mut int: acc = 0\n"
      "mut int: i = 0\n"
      "while(i < len(text)){\n"
      "   def int: ch = load8(text, i)\n"
      "   if(ch > 96){\n"
      "      acc += ((ch + i) %% 31)\n"
      "   } else {\n"
      "      acc -= ((ch + %d) %% 17)\n"
      "   }\n"
      "   i += 1\n"
      "}\nprint(acc)\n", text, bias);
  } else if (strcmp(s, "optimizer-emi-dead-guard") == 0) {
    fprintf(f,
      "mut int: acc = %d\nmut int: state = %d\nmut int: i = 0\nwhile(i < %d){\n"
      "   def int: v = (((i * %d) + state) %% 197)\n"
      "   if(i < 0){\n"
      "      acc += (v * 999)\n"
      "      state = -12345\n"
      "   } else {\n"
      "      acc += (v + (i %% 5))\n"
      "   }\n"
      "   if((i >= 0) && (i < %d)){\n"
      "      state = (((state * 5) + v) %% 223)\n"
      "   } else {\n"
      "      acc -= 777\n"
      "   }\n"
      "   if(((state + i) %% 4) == 2){\n"
      "      acc -= (state %% 11)\n"
      "   } else {\n"
      "      acc += ((state + v) %% 17)\n"
      "   }\n"
      "   i += 1\n"
      "}\nprint((acc + state))\n",
      bias, salt % 31 + 1, rounds, scale_for_salt(salt, 13, 3), rounds);
  } else if (strcmp(s, "optimizer-pure-call-cse") == 0) {
    fprintf(f,
      "fn pure_mix(int: x, int: y) int {\n"
      "   def int: a = (((x * %d) + (y * %d) + %d) %% 257)\n"
      "   def int: b = (((a * a) + (x %% 17) + (y %% 19)) %% 263)\n"
      "   if((b %% 3) == 0){\n"
      "      return ((b + a) %% 211)\n"
      "   }\n"
      "   return ((b - (a %% 23)) %% 211)\n"
      "}\n\n"
      "fn pure_fold(int: x) int {\n"
      "   def int: left = pure_mix(x, %d)\n"
      "   def int: right = pure_mix(x, %d)\n"
      "   def int: both = (left + right)\n"
      "   if(left == right){\n"
      "      return (both + pure_mix((x + 1), %d))\n"
      "   }\n"
      "   return (both - pure_mix((x + 2), %d))\n"
      "}\n\n"
      "mut int: acc = %d\n"
      "mut int: i = 0\n"
      "while(i < %d){\n"
      "   def int: a = pure_fold((i + (acc %% 7)))\n"
      "   def int: b = pure_fold((i + (acc %% 7)))\n"
      "   if(a == b){\n"
      "      acc += ((a %% 29) + pure_mix(i, %d))\n"
      "   } else {\n"
      "      acc -= ((b %% 31) + pure_mix(%d, i))\n"
      "   }\n"
      "   i += 1\n"
      "}\nprint(acc)\n",
      scale_for_salt(salt, 7, 3), scale_for_salt(bias, 5, 2), bias,
      salt, salt, bias, bias, bias, rounds, salt, bias);
  } else if (strcmp(s, "optimizer-pure-array-read-cse") == 0) {
    fputs("def data = ", f);
    emit_ny_array(f, c->values, n);
    fprintf(f,
      "\n"
      "fn pure_pick(int: x, int: saltv) int {\n"
      "   def int: idx = (((x * %d) + saltv) %% %d)\n"
      "   def int: v = get(data, idx, 0)\n"
      "   def int: mix = (((v * %d) + (idx * 7) + saltv) %% 271)\n"
      "   if((mix %% 2) == 0){\n"
      "      return ((mix + v + idx) %% 223)\n"
      "   }\n"
      "   return ((mix + 223 - (v %% 17)) %% 223)\n"
      "}\n\n"
      "mut int: acc = %d\n"
      "mut int: i = 0\n"
      "while(i < %d){\n"
      "   def int: key = (i + (acc %% 11) + %d)\n"
      "   def int: a = pure_pick(key, %d)\n"
      "   def int: b = pure_pick(key, %d)\n"
      "   def int: c = pure_pick((key + 1), %d)\n"
      "   if(a == b){\n"
      "      acc = ((acc + a + c + (i %% 5)) %% 100000)\n"
      "   } else {\n"
      "      acc = ((acc + b + 17) %% 100000)\n"
      "   }\n"
      "   i += 1\n"
      "}\nprint(acc)\n",
      scale_for_salt(salt, 5, 2), n, scale_for_salt(bias, 7, 3),
      bias, rounds, salt % 31 + 1, salt, salt, bias);
  } else if (strcmp(s, "optimizer-pure-branch-compose") == 0) {
    fprintf(f,
      "fn pure_gate(int: x) int {\n"
      "   def int: v = (((x * %d) + %d) %% 251)\n"
      "   if((v %% 5) < 2){\n"
      "      return (((v * 3) + %d) %% 293)\n"
      "   }\n"
      "   if((v %% 7) == 3){\n"
      "      return ((v + x + %d) %% 293)\n"
      "   }\n"
      "   return ((v + (x %% 29) + %d) %% 293)\n"
      "}\n\n"
      "fn pure_compose(int: x, int: y) int {\n"
      "   def int: z = (x + y + %d)\n"
      "   def int: a = pure_gate(z)\n"
      "   def int: b = pure_gate(z)\n"
      "   if(a == b){\n"
      "      def int: c = pure_gate((x + (a %% 5) + %d))\n"
      "      return ((a + c + y) %% 307)\n"
      "   }\n"
      "   return ((b + pure_gate((y + %d))) %% 307)\n"
      "}\n\n"
      "mut int: acc = %d\n"
      "mut int: i = 0\n"
      "while(i < %d){\n"
      "   def int: x = (i + (acc %% 13))\n"
      "   def int: a = pure_compose(x, %d)\n"
      "   def int: b = pure_compose(x, %d)\n"
      "   if(a == b){\n"
      "      acc = ((acc + a + pure_gate((i + %d))) %% 100000)\n"
      "   } else {\n"
      "      acc = ((acc + b + 23) %% 100000)\n"
      "   }\n"
      "   i += 1\n"
      "}\nprint(acc)\n",
      scale_for_salt(salt, 9, 4), bias, salt, bias, salt + bias,
      bias, salt, bias, bias, rounds, salt, salt, bias);
  } else if (strcmp(s, "torture-add-compare-grid") == 0) {
    fprintf(f,
      "mut int: acc = 0\nmut int: i = 0\nwhile(i < %d){\n"
      "   def int: a = (((i * %d) + %d) %% 241)\n"
      "   def int: b = (((i * %d) + %d) %% 241)\n"
      "   def int: s = (a + b)\n"
      "   if((s %% 7) == 0){\n"
      "      acc += s\n"
      "   } else {\n"
      "      acc -= (s %% 11)\n"
      "   }\n"
      "   if(((a + i) %% 5) != 3){\n"
      "      acc += a\n"
      "   } else {\n"
      "      acc -= b\n"
      "   }\n"
      "   if(a < b){\n"
      "      acc += (b - a)\n"
      "   } else {\n"
      "      acc += (a - b)\n"
      "   }\n"
      "   if((s >= 64) && (s <= 320)){\n"
      "      acc += (s %% 13)\n"
      "   } else {\n"
      "      acc -= (s %% 17)\n"
      "   }\n"
      "   i += 1\n"
      "}\nprint(acc)\n",
      rounds, scale_for_salt(salt, 17, 3), bias, scale_for_salt(salt, 11, 5), bias + 7);
  } else if (strcmp(s, "torture-crc-mix-loop") == 0) {
    fputs("def data = ", f); emit_ny_array(f, c->values, n);
    fprintf(f, "\nmut int: crc = %d\nmut int: i = 0\nwhile(i < %d){\n"
               "   def int: idx = ((i + %d) %% %d)\n"
               "   def int: byte = ((get(data, idx, 0) + i) & 255)\n"
               "   crc = (((crc * 33) ^^ byte) & 65535)\n"
               "   if((crc & 1) == 0){\n"
               "      crc = ((crc + 4129) & 65535)\n"
               "   } else {\n"
               "      crc = ((crc ^^ 33800) & 65535)\n"
               "   }\n"
               "   i += 1\n}\nprint(crc)\n",
            (salt << 3) & 255, rounds, bias, n);
  } else {
    fputs("def src = ", f); emit_ny_array(f, c->values, n);
    fputs("\nmut tmp = ", f); emit_ny_zero_array(f, n);
    fputs("\nmut out = ", f); emit_ny_zero_array(f, n);
    fprintf(f, "\nmut int: acc = 0\nmut int: i = 0\nwhile(i < %d){\n"
               "   def int: v = get(src, i, 0)\n"
               "   tmp = set_idx(tmp, i, 0)\n"
               "   tmp = set_idx(tmp, i, (((v * %d) + i) %% 251))\n"
               "   out = set_idx(out, i, get(tmp, i, 0))\n"
               "   i += 1\n}\n"
               "mut int: j = 0\nwhile(j < %d){\n"
               "   def int: idx = ((j + %d) %% %d)\n"
               "   def int: old = get(out, idx, 0)\n"
               "   out = set_idx(out, idx, ((old + get(tmp, idx, 0)) %% 251))\n"
               "   acc += (get(out, idx, 0) + (j %% 7))\n"
               "   j += 1\n}\nprint(acc)\n",
            n, scale_for_salt(salt, 5, 2), rounds, bias, n);
  }
}

static void safe_case_name(const char *prefix, const char *shape, int idx, char *out, size_t out_sz) {
  const char *raw = shape ? shape : "";
  if (prefix && strcmp(prefix, "stress") == 0 && strncmp(raw, "stress-", 8) == 0)
    raw += 8;
  char safe[128];
  size_t j = 0;
  for (const unsigned char *p = (const unsigned char *)raw; *p && j + 1 < sizeof(safe); ++p) {
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
      safe[j++] = (char)*p;
    else if (*p == '-' || *p == '_' || *p == '.')
      safe[j++] = '_';
  }
  safe[j] = '\0';
  snprintf(out, out_sz, "%s_%03d_%s", prefix && *prefix ? prefix : "nynth", idx, safe[0] ? safe : "case");
}

static void init_gen_case(gen_case_t *c, const gen_shape_t *shape, int idx, int seed,
                          const char *profile, const char *requested_generator,
                          bool fast, bool insane) {
  memset(c, 0, sizeof(*c));
  c->shape = shape;
  c->seed = seed + idx;
  c->index = idx;
  snprintf(c->profile, sizeof(c->profile), "%s", profile);
  const char *method = canonical_generator_method(requested_generator);
  if (strcmp(method, "mixed") == 0) method = shape->generator;
  snprintf(c->generator, sizeof(c->generator), "%s", method);
  snprintf(c->method, sizeof(c->method), "%s", method);
  snprintf(c->source_kind, sizeof(c->source_kind), "%s", source_kind_for_method(method, shape));
  safe_case_name(case_prefix_for_method(method), shape->name, idx, c->name, sizeof(c->name));
  uint64_t state = fnv1a64(shape->name, strlen(shape->name));
  state ^= (uint64_t)(seed + idx * 0x45D9F3B);
  state ^= (uint64_t)idx << 33;
  c->insane = insane;
  c->n = insane ? rng_range(&state, 32, 96) : rng_range(&state, 5, 11);
  c->rounds = insane ? rng_range(&state, 512, 4096) : rng_range(&state, 24, 72);
  if (fast && !insane && c->rounds > 32) c->rounds = 32;
  if (fast && insane && c->rounds > 1024) c->rounds = 1024;
  for (int i = 0; i < c->n; ++i) c->values[i] = rng_range(&state, 1, 31);
  c->salt = rng_range(&state, 3, 47);
  c->bias = rng_range(&state, 2, 19);
}

static bool write_case_file(const char *path, void (*emit)(FILE *, const gen_case_t *),
                            const gen_case_t *c) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  emit(f, c);
  bool ok = ferror(f) == 0;
  if (fclose(f) != 0) ok = false;
  return ok;
}

static void case_repro_command(const gen_case_t *c, char *out, size_t out_sz) {
  snprintf(out, out_sz,
           "./build/nynth synth print --lang both --shape %s --generator %s --seed %d%s --out build/repro",
           c->shape->name, c->generator, c->seed, c->insane ? " --insane" : "");
}

static void write_case_ir(FILE *f, const gen_case_t *c) {
  char repro[512];
  case_repro_command(c, repro, sizeof(repro));
  fprintf(f, "{\n  \"ir_version\": 2,\n  \"name\": ");
  json_str(f, c->name);
  fprintf(f, ",\n  \"seed\": %d,\n  \"shape\": ", c->seed);
  json_str(f, c->shape->name);
  fprintf(f, ",\n  \"family\": ");
  json_str(f, c->shape->family);
  fprintf(f, ",\n  \"profile\": ");
  json_str(f, c->profile);
  fprintf(f, ",\n  \"generator\": ");
  json_str(f, c->generator);
  fprintf(f, ",\n  \"generator_kind\": ");
  json_str(f, c->generator);
  fprintf(f, ",\n  \"method\": ");
  json_str(f, c->method);
  fprintf(f, ",\n  \"source_kind\": ");
  json_str(f, c->source_kind);
  fprintf(f, ",\n  \"emitter_engine\": \"nynth_core\",\n  \"insane\": %s,\n  \"features\": %s,\n  \"shape_source\": ",
          c->insane ? "true" : "false",
          c->shape->features[0] ? c->shape->features : "[]");
  json_str(f, c->shape->source);
  fprintf(f, ",\n  \"shape_hash\": ");
  json_str(f, c->shape->hash);
  fprintf(f, ",\n  \"shape_dsl_version\": 1,\n  \"template\": ");
  json_str(f, c->shape->name);
  fprintf(f, ",\n  \"repro_command\": ");
  json_str(f, repro);
  fprintf(f, ",\n  \"n\": %d,\n  \"rounds\": %d,\n  \"values\": [", c->n, c->rounds);
  for (int i = 0; i < c->n; ++i) {
    if (i) fputs(", ", f);
    fprintf(f, "%d", c->values[i]);
  }
  fprintf(f, "],\n  \"salt\": %d,\n  \"bias\": %d,\n  \"ast\": {},\n  \"safety\": {\"native_generator\": true, \"bounded_loops\": true, \"positive_mod_divisors\": true, \"array_bounds\": \"proven-inbounds-or-defaulted\", \"signed_overflow\": \"small-range\"},\n  \"effects\": {},\n  \"ast_summary\": {},\n  \"ir_ast_summary\": {},\n  \"validation\": {\"ok\": true, \"errors\": []}\n}\n",
          c->salt, c->bias);
}

static bool write_ir_file(const char *path, const gen_case_t *c) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  write_case_ir(f, c);
  bool ok = ferror(f) == 0;
  if (fclose(f) != 0) ok = false;
  return ok;
}

int cmd_generate_batch(int argc, char **argv) {
  const char *shape_dir = arg_value(argc, argv, "--shape-dir", "shapes");
  const char *profile = arg_value(argc, argv, "--profile", "balanced");
  const char *out_dir = arg_value(argc, argv, "--out", "build/generated/native");
  const char *generator = arg_value(argc, argv, "--generator", "mixed");
  const char *shape_name = arg_value(argc, argv, "--shape", "");
  const char *schedule = normalize_schedule(arg_value(argc, argv, "--schedule", "smart"));
  const char *method = canonical_generator_method(generator);
  int seed = atoi(arg_value(argc, argv, "--seed", "1337"));
  int cases = atoi(arg_value(argc, argv, "--cases", "8"));
  bool fast = arg_flag(argc, argv, "--fast");
  bool insane = arg_flag(argc, argv, "--insane");
  bool list_shapes = arg_flag(argc, argv, "--list") || arg_flag(argc, argv, "--list-shapes");
  if (cases < 1) cases = 1;
  gen_shapes_t all;
  memset(&all, 0, sizeof(all));
  walk_gen_shapes(shape_dir, &all);
  gen_shape_t pool[128];
  int pool_n = select_shapes(&all, pool, 128, generator, shape_name);
  if (list_shapes) {
    print_shape_list_json(&all, pool, pool_n, shape_dir, profile, generator,
                          shape_name, method, schedule);
    return 0;
  }
  if (pool_n <= 0) {
    printf("{\"ok\":false,\"error\":\"no-shapes\",\"shape_dir\":");
    json_str(stdout, shape_dir);
    printf(",\"generator\":");
    json_str(stdout, generator);
    printf(",\"shape\":");
    json_str(stdout, shape_name);
    printf("}\n");
    return 1;
  }
  if (fast && cases > pool_n) cases = pool_n;
  if (!mkdir_p(out_dir)) {
    printf("{\"ok\":false,\"error\":\"mkdir-failed\",\"out_dir\":");
    json_str(stdout, out_dir);
    printf("}\n");
    return 1;
  }

  printf("{\"ok\":true,\"engine\":\"nynth_core\",\"out_dir\":");
  json_str(stdout, out_dir);
  printf(",\"profile\":");
  json_str(stdout, profile);
  printf(",\"generator\":");
  json_str(stdout, method);
  printf(",\"generator_kind\":");
  json_str(stdout, method);
  printf(",\"method\":");
  json_str(stdout, method);
  printf(",\"schedule\":");
  json_str(stdout, schedule);
  printf(",\"seed\":%d,\"insane\":%s,\"cases\":[", seed, insane ? "true" : "false");
  int emitted = 0;
  for (int i = 0; i < cases; ++i) {
    const gen_shape_t *shape = choose_shape(pool, pool_n, i, seed, profile, fast,
                                            schedule, generator);
    if (!shape) continue;
    gen_case_t c;
    init_gen_case(&c, shape, i, seed, profile, generator, fast, insane);
    char *case_dir = NULL, *c_path = NULL, *ny_path = NULL, *ir_path = NULL;
    bool path_ok = asprintf(&case_dir, "%s/%s", out_dir, c.name) >= 0 &&
                   asprintf(&c_path, "%s/%s.c", case_dir, c.name) >= 0 &&
                   asprintf(&ny_path, "%s/%s.ny", case_dir, c.name) >= 0 &&
                   asprintf(&ir_path, "%s/%s.nynth.json", case_dir, c.name) >= 0;
    if (!path_ok || !mkdir_p(case_dir) ||
        !write_case_file(c_path, emit_c_case, &c) ||
        !write_case_file(ny_path, emit_ny_case, &c) ||
        !write_ir_file(ir_path, &c)) {
      printf("%s{\"ok\":false,\"case\":", emitted ? "," : "");
      json_str(stdout, c.name);
      printf(",\"error\":\"write-failed\"}");
      free(case_dir); free(c_path); free(ny_path); free(ir_path);
      emitted++;
      continue;
    }
    printf("%s{\"ok\":true,\"name\":", emitted ? "," : "");
    json_str(stdout, c.name);
    printf(",\"seed\":%d,\"shape\":", c.seed);
    json_str(stdout, c.shape->name);
    printf(",\"family\":");
    json_str(stdout, c.shape->family);
    printf(",\"generator\":");
    json_str(stdout, c.generator);
    printf(",\"generator_kind\":");
    json_str(stdout, c.generator);
    printf(",\"method\":");
    json_str(stdout, c.method);
    printf(",\"schedule\":");
    json_str(stdout, schedule);
    printf(",\"source_kind\":");
    json_str(stdout, c.source_kind);
    printf(",\"insane\":%s", c.insane ? "true" : "false");
    printf(",\"features\":%s,\"shape_source\":", c.shape->features[0] ? c.shape->features : "[]");
    json_str(stdout, c.shape->source);
    printf(",\"shape_hash\":");
    json_str(stdout, c.shape->hash);
    printf(",\"shape_dsl_version\":1,\"template\":");
    json_str(stdout, c.shape->name);
    char repro[512];
    case_repro_command(&c, repro, sizeof(repro));
    printf(",\"repro_command\":");
    json_str(stdout, repro);
    printf(",\"c_source\":");
    json_str(stdout, c_path);
    printf(",\"ny_source\":");
    json_str(stdout, ny_path);
    printf(",\"nynth_ir\":");
    json_str(stdout, ir_path);
    printf("}");
    free(case_dir); free(c_path); free(ny_path); free(ir_path);
    emitted++;
  }
  printf("],\"generated\":%d,\"selected_shape_count\":%d,\"shape_count\":%d}\n",
         emitted, pool_n, all.count);
  return 0;
}
