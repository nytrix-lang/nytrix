/*
 * Native backend entry: orchestrates NYIR lowering, optimization,
 * emission, object writing, and linking into a single compilation path.
 */
#include "code/native/internal.h"
#include "code/native/ir.h"
#include "code/c/c.h"
#include "code/priv.h"
#include "base/common.h"
#include "base/util.h"
#include "base/time.h"
#include "wire/build.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern int64_t rt_bigint_add(int64_t a, int64_t b);
extern int64_t rt_ticks_ns(void);

/*
 * Non-LLVM native backend entry point and target registry.
 *
 * LLVM remains the default backend. When a native backend is explicitly
 * selected, unsupported registered targets must fail with a precise diagnostic
 * instead of silently falling back. x86-64 is the only assembly emitter today;
 * other registered target names exist so the roadmap can add emitters
 * incrementally behind stable option parsing and tests.
 */

void ny_native_set_err(char *err, size_t err_len, const char *fmt, ...) {
  if (!err || err_len == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_len, fmt, ap);
  va_end(ap);
}

typedef struct {
  int64_t *data;
  size_t cap;
} ny_native_local_slot_t;

typedef struct {
  ny_native_local_slot_t *slots;
  size_t len;
  size_t cap;
} ny_native_local_pool_t;

static _Thread_local ny_native_local_pool_t ny_native_local_pool;

static int64_t *ny_native_locals_acquire(size_t depth, size_t count) {
  if (depth >= ny_native_local_pool.len) {
    size_t len = depth + 1;
    if (len > SIZE_MAX / sizeof(*ny_native_local_pool.slots))
      return NULL;
    if (len > ny_native_local_pool.cap) {
      size_t cap = ny_native_local_pool.cap ? ny_native_local_pool.cap * 2 : 8;
      while (cap < len) {
        if (cap > SIZE_MAX / 2)
          return NULL;
        cap *= 2;
      }
      ny_native_local_slot_t *slots =
          realloc(ny_native_local_pool.slots, cap * sizeof(*slots));
      if (!slots)
        return NULL;
      memset(slots + ny_native_local_pool.cap, 0,
             (cap - ny_native_local_pool.cap) * sizeof(*slots));
      ny_native_local_pool.slots = slots;
      ny_native_local_pool.cap = cap;
    }
    ny_native_local_pool.len = len;
  }
  ny_native_local_slot_t *slot = &ny_native_local_pool.slots[depth];
  if (count > slot->cap) {
    if (count > SIZE_MAX / sizeof(*slot->data))
      return NULL;
    int64_t *data = realloc(slot->data, count * sizeof(*data));
    if (!data)
      return NULL;
    slot->data = data;
    slot->cap = count;
  }
  if (count)
    memset(slot->data, 0, count * sizeof(*slot->data));
  return slot->data;
}

static bool ny_native_reserve(ny_native_writer_t *w, size_t add) {
  if (!w)
    return false;
  if (add > SIZE_MAX - w->len - 1)
    return false;
  size_t need = w->len + add + 1;
  if (need <= w->cap)
    return true;
  size_t cap = w->cap ? w->cap : 4096;
  while (cap < need) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  char *data = realloc(w->data, cap);
  if (!data)
    return false;
  w->data = data;
  w->cap = cap;
  return true;
}

bool ny_native_put(ny_native_writer_t *w, const char *s) {
  if (!s)
    return true;
  size_t n = strlen(s);
  if (!ny_native_reserve(w, n))
    return false;
  memcpy(w->data + w->len, s, n + 1);
  w->len += n;
  return true;
}

bool ny_native_printf(ny_native_writer_t *w, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return false;
  }
  if (!ny_native_reserve(w, (size_t)n)) {
    va_end(ap2);
    return false;
  }
  vsnprintf(w->data + w->len, w->cap - w->len, fmt, ap2);
  va_end(ap2);
  w->len += (size_t)n;
  return true;
}

static bool ny_native_write_eval_profile(const ny_options *opt,
                                         const nyir_eval_result_t *result,
                                         const char *name, char *err,
                                         size_t err_len) {
  if (!opt || !opt->nyir_run_profile)
    return true;
  FILE *out = stderr;
  if (opt->nyir_run_profile_path && opt->nyir_run_profile_path[0]) {
    ny_native_ensure_parent_dir_for_path(opt->nyir_run_profile_path);
    out = fopen(opt->nyir_run_profile_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len,
                        "native NYIR VM profile: failed to open %s: %s",
                        opt->nyir_run_profile_path, strerror(errno));
      return false;
    }
  }
  nyir_eval_result_dump(out, name, result);
  if (out != stderr)
    fclose(out);
  return true;
}

static bool ny_native_write_eval_result(const ny_options *opt,
                                        const nyir_eval_result_t *result,
                                        const char *name, char *err,
                                        size_t err_len) {
  FILE *out = stderr;
  if (opt && opt->nyir_run_path && opt->nyir_run_path[0]) {
    ny_native_ensure_parent_dir_for_path(opt->nyir_run_path);
    out = fopen(opt->nyir_run_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len, "native NYIR VM: failed to open %s: %s",
                        opt->nyir_run_path, strerror(errno));
      return false;
    }
  }
  fprintf(out, "nyir vm function=%s returned=%s result=%" PRId64 " steps=%zu\n",
          name && name[0] ? name : "rt_main",
          result && result->returned ? "yes" : "no",
          result ? result->result : 0, result ? result->steps : 0);
  if (out != stderr)
    fclose(out);
  if (!ny_native_write_eval_profile(opt, result, name, err, err_len))
    return false;
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

static size_t ny_native_vm_max_steps(const ny_options *opt) {
  if (opt && opt->nyir_run_max_steps >= 0)
    return (size_t)opt->nyir_run_max_steps;
  return 1000000;
}

static size_t ny_native_vm_recursion_limit(const ny_options *opt) {
  if (opt && opt->nyir_run_recursion_limit >= 0)
    return (size_t)opt->nyir_run_recursion_limit;
  return 256;
}

static bool ny_native_eval_ir_func(nyir_func_t *rt_main,
                                   const ny_options *opt, const char *name,
                                   char *err, size_t err_len) {
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = ny_native_locals_acquire(0, local_count);
  if (local_count && !locals) {
    ny_native_set_err(err, err_len, NY_NATIVE_OOM);
    return false;
  }
  nyir_eval_result_t result = {0};
  bool ok = nyir_eval(rt_main, locals, local_count,
                      ny_native_vm_max_steps(opt), &result, err, err_len);
  if (!ok)
    return false;
  bool wrote = ny_native_write_eval_result(opt, &result, name, err, err_len);
  nyir_eval_result_free(&result);
  return wrote;
}

bool ny_native_emit_nir_func(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const nyir_func_t *nyir, const char *label,
                             bool tag_return, char *err, size_t err_len);

typedef struct {
  nyir_func_t *funcs;
  const char **names;
  size_t count;
  size_t depth;
  size_t recursion_limit;
  size_t max_steps;
  nyir_eval_result_t *profile;
} ny_native_vm_call_ctx_t;

static void ny_native_vm_profile_take_detail(nyir_eval_result_t *dst,
                                             nyir_eval_result_t *src) {
  if (!dst || !src)
    return;
  free(dst->pc_counts);
  free(dst->edges);
  dst->pc_counts = src->pc_counts;
  dst->pc_count_len = src->pc_count_len;
  dst->edges = src->edges;
  dst->edge_count = src->edge_count;
  dst->edge_cap = src->edge_cap;
  src->pc_counts = NULL;
  src->pc_count_len = 0;
  src->edges = NULL;
  src->edge_count = 0;
  src->edge_cap = 0;
}

static void ny_native_vm_profile_merge(nyir_eval_result_t *dst,
                                       const nyir_eval_result_t *src) {
  if (!dst || !src)
    return;
  dst->steps += src->steps;
  dst->branch_taken += src->branch_taken;
  dst->branch_not_taken += src->branch_not_taken;
  dst->call_count += src->call_count;
  if (src->max_value_index > dst->max_value_index)
    dst->max_value_index = src->max_value_index;
  if (src->max_local_index > dst->max_local_index)
    dst->max_local_index = src->max_local_index;
  if (src->max_pc > dst->max_pc)
    dst->max_pc = src->max_pc;
  for (size_t i = 0; i < (size_t)NYIR_OP_COUNT; ++i)
    dst->op_counts[i] += src->op_counts[i];
}

static bool ny_native_vm_symbol_matches(const char *symbol, const char *name) {
  if (!symbol || !name)
    return false;
  if (strcmp(symbol, name) == 0)
    return true;
  return strncmp(symbol, "ny_fn_", 6) == 0 && strcmp(symbol + 6, name) == 0;
}

static bool ny_native_vm_call_resolve(void *opaque, const char *symbol,
                                      const int64_t *args, size_t arg_count,
                                      int64_t *out, char *err,
                                      size_t err_len) {
  ny_native_vm_call_ctx_t *ctx = (ny_native_vm_call_ctx_t *)opaque;
  if (!ctx || !symbol)
    return ny_native_set_err(err, err_len, "native NYIR VM: missing call target"), false;
  if (ctx->depth >= ctx->recursion_limit)
    return ny_native_set_err(err, err_len,
                             "native NYIR VM: recursive call limit exceeded at depth %zu",
                             ctx->depth),
           false;
  if (strcmp(symbol, "rt_ticks_ns") == 0 && arg_count == 0) {
    if (out)
      *out = rt_ticks_ns();
    return true;
  }
  if (strcmp(symbol, "rt_bigint_from_i64_raw") == 0 && arg_count == 1) {
    if (out)
      *out = rt_bigint_from_i64_raw(args ? args[0] : 0);
    return true;
  }
  if (strcmp(symbol, "rt_bigint_to_i64_raw") == 0 && arg_count == 1) {
    if (out)
      *out = rt_bigint_to_i64_raw(args ? args[0] : 0);
    return true;
  }
  if (strcmp(symbol, "rt_bigint_add") == 0 && arg_count == 2) {
    if (out)
      *out = rt_bigint_add(args ? args[0] : 0, args ? args[1] : 0);
    return true;
  }
  if (strcmp(symbol, "rt_native_is_int") == 0 && arg_count == 1) {
    if (out)
      *out = rt_native_is_int(args ? args[0] : 0);
    return true;
  }
  if (strcmp(symbol, "rt_native_has_tag") == 0 && arg_count == 2) {
    if (out)
      *out = rt_native_has_tag(args ? args[0] : 0, args ? args[1] : 0);
    return true;
  }
  if (strcmp(symbol, "rt_native_tbuf_new") == 0 && arg_count == 2) {
    int64_t count = args ? args[0] : 0;
    int64_t elem_size = args ? args[1] : 0;
    if (out)
      *out = rt_native_tbuf_new(count, elem_size);
    return true;
  }
  if (strcmp(symbol, "rt_native_cstr_builder_new") == 0 && arg_count == 1) {
    if (out) *out = rt_native_cstr_builder_new(args ? args[0] : 0);
    return true;
  }
  if (strcmp(symbol, "rt_native_cstr_builder_append") == 0 && arg_count == 2) {
    if (out) *out = rt_native_cstr_builder_append(args ? args[0] : 0, args ? args[1] : 0);
    return true;
  }
  if (strcmp(symbol, "rt_native_cstr_builder_finalize") == 0 && arg_count == 1) {
    if (out) *out = rt_native_cstr_builder_finalize(args ? args[0] : 0);
    return true;
  }
  if ((strcmp(symbol, "malloc") == 0 || strcmp(symbol, "__malloc") == 0) &&
      arg_count == 1) {
    void *p = malloc((size_t)(args ? args[0] : 0));
    if (!p)
      return ny_native_set_err(err, err_len, "native NYIR VM: malloc failed"),
             false;
    if (out)
      *out = (int64_t)(uintptr_t)p;
    return true;
  }
  if ((strcmp(symbol, "free") == 0 || strcmp(symbol, "__free") == 0) &&
      arg_count == 1) {
    free((void *)(uintptr_t)(args ? args[0] : 0));
    if (out)
      *out = 0;
    return true;
  }
  for (size_t i = 0; i < ctx->count; ++i) {
    if (!ny_native_vm_symbol_matches(symbol, ctx->names[i]))
      continue;
    nyir_func_t *callee = &ctx->funcs[i];
    size_t local_count = ny_native_nir_local_count(callee);
    if (local_count < arg_count)
      local_count = arg_count;
    int64_t *locals =
        ny_native_locals_acquire(ctx->depth + 1, local_count);
    if (local_count && !locals)
      return ny_native_set_err(err, err_len, NY_NATIVE_OOM), false;
    for (size_t a = 0; a < arg_count; ++a)
      locals[a] = args ? args[a] : 0;
    nyir_eval_result_t r = {0};
    ctx->depth++;
    bool ok = nyir_eval_with_calls(callee, locals, local_count,
                                   ctx->max_steps, &r,
                                   ny_native_vm_call_resolve, ctx, err,
                                   err_len);
    ctx->depth--;
    if (!ok)
      return false;
    ny_native_vm_profile_merge(ctx->profile, &r);
    if (!r.returned) {
      nyir_eval_result_free(&r);
      return ny_native_set_err(err, err_len,
                               "native NYIR VM: callee '%s' did not return",
                               ctx->names[i] ? ctx->names[i] : symbol),
             false;
    }
    if (out)
      *out = r.result;
    nyir_eval_result_free(&r);
    return true;
  }
  return ny_native_set_err(err, err_len,
                           "native NYIR VM: unresolved call target '%s'",
                           symbol),
         false;
}

static bool ny_native_eval_ir_func_with_calls(nyir_func_t *rt_main,
                                              nyir_func_t *funcs,
                                              const char **names, size_t count,
                                              const ny_options *opt,
                                              const char *name, char *err,
                                              size_t err_len) {
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = ny_native_locals_acquire(0, local_count);
  if (local_count && !locals) {
    ny_native_set_err(err, err_len, NY_NATIVE_OOM);
    return false;
  }
  ny_native_vm_call_ctx_t ctx = {.funcs = funcs,
                                 .names = names,
                                 .count = count,
                                 .recursion_limit =
                                     ny_native_vm_recursion_limit(opt),
                                 .max_steps = ny_native_vm_max_steps(opt)};
  nyir_eval_result_t result = {0};
  nyir_eval_result_t nested_profile = {0};
  ctx.profile = &nested_profile;
  bool ok = nyir_eval_with_calls(rt_main, locals, local_count,
                                 ny_native_vm_max_steps(opt), &result,
                                 ny_native_vm_call_resolve, &ctx, err,
                                 err_len);
  if (!ok)
    return false;
  ny_native_vm_profile_merge(&nested_profile, &result);
  ny_native_vm_profile_take_detail(&nested_profile, &result);
  nested_profile.returned = result.returned;
  nested_profile.result = result.result;
  bool wrote = ny_native_write_eval_result(opt, &nested_profile, name, err, err_len);
  nyir_eval_result_free(&result);
  nyir_eval_result_free(&nested_profile);
  return wrote;
}

bool ny_native_eval_ir_value(nyir_func_t *rt_main, nyir_func_t *funcs,
                             const char **names, size_t count,
                             const ny_options *opt,
                             nyir_eval_result_t *out, char *err,
                             size_t err_len) {
  if (!rt_main || !out) {
    ny_native_set_err(err, err_len, "native oracle: missing NYIR entry");
    return false;
  }
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = ny_native_locals_acquire(0, local_count);
  if (local_count && !locals) {
    ny_native_set_err(err, err_len, "native oracle: out of memory");
    return false;
  }
  ny_native_vm_call_ctx_t ctx = {.funcs = funcs,
                                 .names = names,
                                 .count = count,
                                 .recursion_limit =
                                     ny_native_vm_recursion_limit(opt),
                                 .max_steps = ny_native_vm_max_steps(opt)};
  nyir_eval_result_t top = {0};
  nyir_eval_result_t nested = {0};
  ctx.profile = &nested;
  bool ok = nyir_eval_with_calls(rt_main, locals, local_count,
                                 ny_native_vm_max_steps(opt), &top,
                                 ny_native_vm_call_resolve, &ctx, err,
                                 err_len);
  if (!ok)
    return false;
  ny_native_vm_profile_merge(&nested, &top);
  ny_native_vm_profile_take_detail(&nested, &top);
  nested.returned = top.returned;
  nested.result = top.result;
  *out = nested;
  nyir_eval_result_free(&top);
  return true;
}

bool ny_native_collect_vm_profile(nyir_func_t *rt_main,
                                  nyir_func_t *funcs,
                                  const char **names, size_t count,
                                  const ny_options *opt,
                                  nyir_eval_result_t *profile,
                                  char *err, size_t err_len) {
  if (!rt_main || !profile)
    return false;
  memset(profile, 0, sizeof(*profile));
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = ny_native_locals_acquire(0, local_count);
  if (local_count && !locals) {
    ny_native_set_err(err, err_len,
                      "native tier report VM profile: out of memory");
    return false;
  }
  ny_native_vm_call_ctx_t ctx = {.funcs = funcs,
                                 .names = names,
                                 .count = count,
                                 .recursion_limit =
                                     ny_native_vm_recursion_limit(opt),
                                 .max_steps = ny_native_vm_max_steps(opt),
                                 .profile = profile};
  nyir_eval_result_t top = {0};
  bool ok = nyir_eval_with_calls(rt_main, locals, local_count,
                                 ny_native_vm_max_steps(opt), &top,
                                 ny_native_vm_call_resolve, &ctx, err,
                                 err_len);
  if (!ok)
    return false;
  ny_native_vm_profile_merge(profile, &top);
  ny_native_vm_profile_take_detail(profile, &top);
  profile->returned = top.returned;
  profile->result = top.result;
  nyir_eval_result_free(&top);
  return true;
}

static bool ny_native_nir_read_u16le(FILE *in, uint16_t *out) {
  unsigned char bytes[2];
  if (!in || !out || fread(bytes, 1, sizeof(bytes), in) != sizeof(bytes))
    return false;
  *out = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
  return true;
}

static bool ny_native_nir_read_u32le(FILE *in, uint32_t *out) {
  unsigned char bytes[4];
  if (!in || !out || fread(bytes, 1, sizeof(bytes), in) != sizeof(bytes))
    return false;
  *out = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
  return true;
}

static bool ny_native_nir_load_bundle_member(FILE *in, nyir_func_t *out,
                                             char *name, size_t name_len,
                                             char *err, size_t err_len) {
  uint32_t bytes = 0;
  if (!ny_native_nir_read_u32le(in, &bytes) || bytes == 0 || bytes > 64u * 1024u * 1024u)
    return ny_native_set_err(err, err_len, "native NYIR bundle: invalid member size"), false;
  unsigned char *blob = malloc(bytes);
  if (!blob)
    return ny_native_set_err(err, err_len, NY_NATIVE_BUNDLE_OOM), false;
  bool ok = fread(blob, 1, bytes, in) == bytes;
  FILE *tmp = ok ? tmpfile() : NULL;
  if (!tmp)
    ok = false;
  if (ok && fwrite(blob, 1, bytes, tmp) != bytes)
    ok = false;
  if (ok && fseek(tmp, 0, SEEK_SET) != 0)
    ok = false;
  if (ok)
    ok = nyir_load_binary(tmp, out, name, name_len, err, err_len);
  if (ok && fgetc(tmp) != EOF) {
    ny_native_set_err(err, err_len,
                      "native NYIR bundle: trailing data in member");
    ok = false;
  }
  if (tmp)
    fclose(tmp);
  free(blob);
  if (!ok && err && err_len && !err[0])
    ny_native_set_err(err, err_len, "native NYIR bundle: malformed member");
  return ok;
}

static bool ny_native_eval_ir_bundle(FILE *in, const ny_options *opt,
                                     char *err, size_t err_len) {
  uint16_t version = 0, flags = 0;
  uint32_t members = 0;
  if (!ny_native_nir_read_u16le(in, &version) || !ny_native_nir_read_u16le(in, &flags) ||
      !ny_native_nir_read_u32le(in, &members) || version != 1 || flags != 0 ||
      members == 0 || members > NY_NATIVE_NIR_BUNDLE_MAX_FUNCS + 1)
    return ny_native_set_err(err, err_len, "native NYIR bundle: unsupported or malformed header"), false;
  nyir_func_t entry = {0};
  size_t function_cap = (size_t)members - 1;
  nyir_func_t *funcs =
      function_cap ? calloc(function_cap, sizeof(*funcs)) : NULL;
  char **names = function_cap ? calloc(function_cap, sizeof(*names)) : NULL;
  if (function_cap && (!funcs || !names)) {
    free(funcs);
    free(names);
    return ny_native_set_err(err, err_len, NY_NATIVE_BUNDLE_OOM), false;
  }
  char entry_name[128] = {0};
  size_t loaded = 0;
  bool ok = ny_native_nir_load_bundle_member(in, &entry, entry_name, sizeof(entry_name), err, err_len) &&
            strcmp(entry_name, "rt_main") == 0;
  if (!ok && err && err_len && !err[0])
    ny_native_set_err(err, err_len, "native NYIR bundle: first member must be rt_main");
  for (uint32_t i = 1; ok && i < members; ++i) {
    char name[128] = {0};
    ok = ny_native_nir_load_bundle_member(in, &funcs[loaded], name, sizeof(name), err, err_len);
    if (ok && (!name[0] || strcmp(name, "rt_main") == 0)) {
      ny_native_set_err(err, err_len, "native NYIR bundle: invalid function name");
      ok = false;
    }
    for (size_t j = 0; ok && j < loaded; ++j) {
      if (strcmp(name, names[j]) == 0) {
        ny_native_set_err(err, err_len,
                          "native NYIR bundle: duplicate function name '%s'",
                          name);
        ok = false;
      }
    }
    if (ok) {
      names[loaded] = ny_strdup(name);
      if (!names[loaded]) {
        ny_native_set_err(err, err_len, NY_NATIVE_BUNDLE_OOM);
        ok = false;
      } else {
        loaded++;
      }
    }
  }
  if (ok && fgetc(in) != EOF) {
    ny_native_set_err(err, err_len,
                      "native NYIR bundle: trailing data after final member");
    ok = false;
  }
  if (ok)
    ok = ny_native_eval_ir_func_with_calls(&entry, funcs, (const char **)names,
                                           loaded, opt, entry_name, err, err_len);
  nyir_func_free(&entry);
  for (size_t i = 0; i < loaded; ++i) {
    nyir_func_free(&funcs[i]);
    free(names[i]);
  }
  free(funcs);
  free(names);
  return ok;
}

bool ny_native_eval_ir_binary_file(const char *path, const ny_options *opt,
                                   char *err, size_t err_len) {
  if (!path || !*path) {
    ny_native_set_err(err, err_len,
                      "native NYIR VM: missing binary input path");
    return false;
  }
  FILE *in = fopen(path, "rb");
  if (!in) {
    ny_native_set_err(err, err_len, "native NYIR VM: failed to open %s: %s",
                      path, strerror(errno));
    return false;
  }
  char magic[4];
  if (fread(magic, 1, sizeof(magic), in) != sizeof(magic)) {
    fclose(in);
    ny_native_set_err(err, err_len, "native NYIR VM: malformed binary input");
    return false;
  }
  if (memcmp(magic, "NYIP", 4) == 0) {
    bool bundle_ok = ny_native_eval_ir_bundle(in, opt, err, err_len);
    fclose(in);
    return bundle_ok;
  }
  if (memcmp(magic, "NYIR", 4) != 0 || fseek(in, 0, SEEK_SET) != 0) {
    fclose(in);
    ny_native_set_err(err, err_len, "native NYIR VM: malformed binary input");
    return false;
  }
  nyir_func_t f = {0};
  char name[128] = {0};
  bool ok = nyir_load_binary(in, &f, name, sizeof(name), err, err_len);
  if (ok && fgetc(in) != EOF) {
    ny_native_set_err(err, err_len,
                      "native NYIR load: trailing data after function");
    ok = false;
  }
  fclose(in);
  if (!ok) {
    nyir_func_free(&f);
    return false;
  }
  ok = ny_native_eval_ir_func(&f, opt, name[0] ? name : "rt_main", err,
                              err_len);
  nyir_func_free(&f);
  return ok;
}

bool ny_native_eval_ir_for_program(const program_t *prog,
                                   const ny_options *opt, char *err,
                                   size_t err_len) {
  if (opt && opt->nyir_run_bin_path && opt->nyir_run_bin_path[0])
    return ny_native_eval_ir_binary_file(opt->nyir_run_bin_path, opt, err,
                                         err_len);
  nyir_func_t rt_main = {0};
  nyir_func_t funcs[NY_NATIVE_LIVE_MAX_FUNCS] = {{0}};
  const char *names[NY_NATIVE_LIVE_MAX_FUNCS] = {0};
  size_t count = 0;
  if (!ny_native_build_nir(prog, opt, &rt_main, funcs, &count, names,
                           NY_NATIVE_LIVE_MAX_FUNCS, err, err_len))
    return false;
  nyir_eval_result_t profile = {0};
  bool ok = ny_native_collect_vm_profile(
      &rt_main, funcs, names, count, opt, &profile, err, err_len);
  nyir_eval_result_free(&profile);
  nyir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    nyir_func_free(&funcs[i]);
  return ok;
}

static bool ny_native_stmt_in_dump_scope(const ny_options *opt,
                                         const stmt_t *s) {
  if (!s)
    return false;
  ny_dump_scope_t scope = opt ? opt->dump_scope : NY_DUMP_SCOPE_PROGRAM;
  if (scope == NY_DUMP_SCOPE_BOTH)
    return true;
  bool is_std = ny_is_stdlib_tok(s->tok);
  return scope == NY_DUMP_SCOPE_LIB ? is_std : !is_std;
}

static bool ny_native_program_dump_view(const program_t *prog,
                                        const ny_options *opt,
                                        program_t *view,
                                        stmt_t ***storage) {
  if (!prog || !view || !storage)
    return false;
  *view = *prog;
  *storage = NULL;
  if (!prog->body.len)
    return true;
  stmt_t **data = calloc(prog->body.len, sizeof(*data));
  if (!data)
    return false;
  size_t len = 0;
  for (size_t i = 0; i < prog->body.len; ++i)
    if (ny_native_stmt_in_dump_scope(opt, prog->body.data[i]))
      data[len++] = prog->body.data[i];
  view->body.data = data;
  view->body.len = view->body.cap = len;
  *storage = data;
  return true;
}

bool ny_native_dump_ir_for_program(const program_t *prog,
                                   const ny_options *opt, char *err,
                                   size_t err_len) {
  if (!opt || !opt->native_dump_ir)
    return true;
  bool defer_metadata_bin_report =
      opt->nyir_metadata_report && opt->nyir_metadata_bin_path &&
      opt->nyir_metadata_bin_path[0] && opt->nyir_dump_bin &&
      opt->nyir_dump_bin_path && opt->nyir_dump_bin_path[0] &&
      strcmp(opt->nyir_metadata_bin_path, opt->nyir_dump_bin_path) == 0;
  if (!defer_metadata_bin_report &&
      !ny_native_write_nir_metadata_report(prog, opt, err, err_len))
    return false;
  bool run_binary_after_dump =
      opt->nyir_run && opt->nyir_run_bin_path && opt->nyir_run_bin_path[0] &&
      opt->nyir_dump_bin;
  if (opt->nyir_run && !run_binary_after_dump) {
    if (!ny_native_eval_ir_for_program(prog, opt, err, err_len))
      return false;
    if (!opt->nyir_dump_text && !opt->nyir_dump_bin)
      return true;
  }
  if (!opt->nyir_dump_text && !opt->nyir_dump_bin)
    return true;
  if (opt->nyir_dump_bin) {
    FILE *bout = stderr;
    if (opt->nyir_dump_bin_path && opt->nyir_dump_bin_path[0]) {
      bout = fopen(opt->nyir_dump_bin_path, "wb");
      if (!bout) {
        ny_native_set_err(err, err_len,
                          "native NYIR binary dump: failed to open %s: %s",
                          opt->nyir_dump_bin_path, strerror(errno));
        return false;
      }
    }
    char berr[512] = {0};
    bool bok = ny_native_nir_dump_program_binary(bout, prog, opt, berr, sizeof(berr));
    if (bout != stderr)
      fclose(bout);
    if (!bok) {
      /*
       * A named dump is an interchange artifact, not a best-effort log. Do
       * not leave an empty or partial file that a later --nyir-run-bin call
       * could treat as the requested program.
       */
      if (opt->nyir_dump_bin_path && opt->nyir_dump_bin_path[0])
        remove(opt->nyir_dump_bin_path);
      ny_native_set_err(err, err_len, "%s",
                        berr[0] ? berr : "native NYIR binary dump failed");
      return false;
    }
    if (run_binary_after_dump) {
      if (!opt->nyir_dump_bin_path || !opt->nyir_dump_bin_path[0]) {
        ny_native_set_err(err, err_len,
                          "native NYIR VM: --nyir-run-bin with same-process dump requires --nyir-dump-bin=PATH");
        return false;
      }
      if (!ny_native_eval_ir_binary_file(opt->nyir_run_bin_path, opt, err,
                                         err_len))
        return false;
    }
    if (defer_metadata_bin_report) {
      /*
       * The just-written NYIP bundle contains multiple NYIR members. Its
       * metadata is the same freshly-built program view, while the legacy
       * --nyir-metadata-bin reader accepts one NYIR member. Avoid reopening
       * a multi-function bundle through that single-function compatibility
       * reader.
       */
      ny_options metadata_opt = *opt;
      metadata_opt.nyir_metadata_bin_path = NULL;
      if (!ny_native_write_nir_metadata_report(prog, &metadata_opt, err,
                                               err_len))
        return false;
    }
    if (!opt->nyir_dump_text)
      return true;
  }
  FILE *out = stderr;
  if (opt->native_dump_ir_path && opt->native_dump_ir_path[0]) {
    out = fopen(opt->native_dump_ir_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len, "native NYIR dump: failed to open %s: %s",
                        opt->native_dump_ir_path, strerror(errno));
      return false;
    }
  }

  bool attempted_any = false;
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!s || s->kind != NY_S_FUNC || !ny_native_stmt_in_dump_scope(opt, s))
      continue;
    attempted_any = true;
    char local_err[512] = {0};
    if (!ny_native_nir_dump_function(out, prog, s, local_err,
                                     sizeof(local_err), opt)) {
      fprintf(out, "native NYIR dump unavailable for function %s: %s\n",
              s->as.fn.name ? s->as.fn.name : "<anon>",
              local_err[0] ? local_err : "unsupported shape");
    }
  }

  program_t dump_view = {0};
  stmt_t **dump_storage = NULL;
  if (!ny_native_program_dump_view(prog, opt, &dump_view, &dump_storage)) {
    if (out != stderr)
      fclose(out);
    ny_native_set_err(err, err_len, "native NYIR dump: out of memory");
    return false;
  }
  if (dump_view.body.len) {
    attempted_any = true;
    char local_err[512] = {0};
    if (!ny_native_nir_dump_rt_main(out, &dump_view, local_err,
                                    sizeof(local_err), opt)) {
      fprintf(out, "%s\n", local_err[0] ? local_err :
              "native NYIR dump unavailable: unsupported program shape");
    }
  }
  free(dump_storage);

  if (!attempted_any)
    fputs("native NYIR dump unavailable: program has no dumpable body\n", out);
  if (out != stderr)
    fclose(out);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
