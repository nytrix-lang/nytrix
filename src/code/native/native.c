#include "code/native/internal.h"
#include "code/native/ir.h"
#include "code/c/c.h"
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
#include <sys/wait.h>
#include <unistd.h>

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
                                         const ny_nir_eval_result_t *result,
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
  ny_nir_eval_result_dump(out, name, result);
  if (out != stderr)
    fclose(out);
  return true;
}

static bool ny_native_write_eval_result(const ny_options *opt,
                                        const ny_nir_eval_result_t *result,
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

static bool ny_native_eval_ir_func(ny_nir_func_t *rt_main,
                                   const ny_options *opt, const char *name,
                                   char *err, size_t err_len) {
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = local_count ? (int64_t *)calloc(local_count, sizeof(*locals))
                                : NULL;
  if (local_count && !locals) {
    ny_native_set_err(err, err_len, "native NYIR VM: out of memory");
    return false;
  }
  ny_nir_eval_result_t result = {0};
  bool ok = ny_nir_eval(rt_main, locals, local_count,
                        ny_native_vm_max_steps(opt), &result, err, err_len);
  free(locals);
  if (!ok)
    return false;
  return ny_native_write_eval_result(opt, &result, name, err, err_len);
}

bool ny_native_emit_nir_func(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const ny_nir_func_t *nir, const char *label,
                             bool tag_return, char *err, size_t err_len);

typedef struct {
  ny_nir_func_t *funcs;
  const char **names;
  size_t count;
  size_t depth;
  size_t recursion_limit;
  size_t max_steps;
  ny_nir_eval_result_t *profile;
} ny_native_vm_call_ctx_t;

static void ny_native_vm_profile_merge(ny_nir_eval_result_t *dst,
                                       const ny_nir_eval_result_t *src) {
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
    ny_nir_func_t *callee = &ctx->funcs[i];
    size_t local_count = ny_native_nir_local_count(callee);
    if (local_count < arg_count)
      local_count = arg_count;
    int64_t *locals = local_count ? (int64_t *)calloc(local_count, sizeof(*locals))
                                  : NULL;
    if (local_count && !locals)
      return ny_native_set_err(err, err_len, "native NYIR VM: out of memory"), false;
    for (size_t a = 0; a < arg_count; ++a)
      locals[a] = args ? args[a] : 0;
    ny_nir_eval_result_t r = {0};
    ctx->depth++;
    bool ok = ny_nir_eval_with_calls(callee, locals, local_count,
                                     ctx->max_steps, &r,
                                     ny_native_vm_call_resolve, ctx, err,
                                     err_len);
    ctx->depth--;
    free(locals);
    if (!ok)
      return false;
    ny_native_vm_profile_merge(ctx->profile, &r);
    if (!r.returned)
      return ny_native_set_err(err, err_len,
                               "native NYIR VM: callee '%s' did not return",
                               ctx->names[i] ? ctx->names[i] : symbol),
             false;
    if (out)
      *out = r.result;
    return true;
  }
  return ny_native_set_err(err, err_len,
                           "native NYIR VM: unresolved call target '%s'",
                           symbol),
         false;
}

static bool ny_native_eval_ir_func_with_calls(ny_nir_func_t *rt_main,
                                              ny_nir_func_t *funcs,
                                              const char **names, size_t count,
                                              const ny_options *opt,
                                              const char *name, char *err,
                                              size_t err_len) {
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = local_count ? (int64_t *)calloc(local_count, sizeof(*locals))
                                : NULL;
  if (local_count && !locals) {
    ny_native_set_err(err, err_len, "native NYIR VM: out of memory");
    return false;
  }
  ny_native_vm_call_ctx_t ctx = {.funcs = funcs,
                                  .names = names,
                                  .count = count,
                                  .recursion_limit =
                                      ny_native_vm_recursion_limit(opt),
                                  .max_steps = ny_native_vm_max_steps(opt)};
  ny_nir_eval_result_t result = {0};
  ny_nir_eval_result_t nested_profile = {0};
  ctx.profile = &nested_profile;
  bool ok = ny_nir_eval_with_calls(rt_main, locals, local_count,
                                   ny_native_vm_max_steps(opt), &result,
                                   ny_native_vm_call_resolve, &ctx, err,
                                   err_len);
  free(locals);
  if (!ok)
    return false;
  ny_native_vm_profile_merge(&nested_profile, &result);
  nested_profile.returned = result.returned;
  nested_profile.result = result.result;
  return ny_native_write_eval_result(opt, &nested_profile, name, err, err_len);
}

static bool ny_native_eval_ir_value(ny_nir_func_t *rt_main,
                                    ny_nir_func_t *funcs,
                                    const char **names, size_t count,
                                    const ny_options *opt,
                                    ny_nir_eval_result_t *out, char *err,
                                    size_t err_len) {
  if (!rt_main || !out) {
    ny_native_set_err(err, err_len, "native oracle: missing NYIR entry");
    return false;
  }
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = local_count ? (int64_t *)calloc(local_count, sizeof(*locals))
                                : NULL;
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
  ny_nir_eval_result_t top = {0};
  ny_nir_eval_result_t nested = {0};
  ctx.profile = &nested;
  bool ok = ny_nir_eval_with_calls(rt_main, locals, local_count,
                                   ny_native_vm_max_steps(opt), &top,
                                   ny_native_vm_call_resolve, &ctx, err,
                                   err_len);
  free(locals);
  if (!ok)
    return false;
  ny_native_vm_profile_merge(&nested, &top);
  nested.returned = top.returned;
  nested.result = top.result;
  *out = nested;
  return true;
}

bool ny_native_collect_vm_profile(ny_nir_func_t *rt_main,
                                  ny_nir_func_t *funcs,
                                  const char **names, size_t count,
                                  const ny_options *opt,
                                  ny_nir_eval_result_t *profile,
                                  char *err, size_t err_len) {
  if (!rt_main || !profile)
    return false;
  memset(profile, 0, sizeof(*profile));
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = local_count ? (int64_t *)calloc(local_count, sizeof(*locals))
                                : NULL;
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
  ny_nir_eval_result_t top = {0};
  bool ok = ny_nir_eval_with_calls(rt_main, locals, local_count,
                                   ny_native_vm_max_steps(opt), &top,
                                   ny_native_vm_call_resolve, &ctx, err,
                                   err_len);
  free(locals);
  if (!ok)
    return false;
  ny_native_vm_profile_merge(profile, &top);
  profile->returned = top.returned;
  profile->result = top.result;
  return true;
}

static bool ny_native_result_oracle_emit_asm(
    const ny_native_target_info_t *target, const ny_nir_func_t *rt_main,
    const ny_nir_func_t *funcs, const char **names, size_t count,
    const char *path, char *err, size_t err_len) {
  ny_native_writer_t w = {0};
  bool ok = false;
  if (!target || !rt_main || !path || !*path) {
    ny_native_set_err(err, err_len, "native oracle: missing assembly target");
    return false;
  }
  if (target->target != NY_NATIVE_TARGET_X86_64) {
    ny_native_set_err(err, err_len,
                      "native oracle: only x86-64 raw-int native is supported");
    return false;
  }
  if (!ny_native_printf(&w, "# Nytrix native result oracle (NYIR only)\n") ||
      !ny_native_put(&w, "\t.text\n"))
    goto done;
  for (size_t i = 0; i < count; ++i) {
    char label[256];
    snprintf(label, sizeof(label), "ny_fn_%s",
             names && names[i] && names[i][0] ? names[i] : "unknown_fn");
    if (!ny_native_emit_nir_func(&w, target, &funcs[i], label, false, err,
                                 err_len))
      goto done;
  }
  if (!ny_native_emit_nir_func(&w, target, rt_main, "rt_main", false, err,
                               err_len))
    goto done;
  ok = ny_write_file(path, w.data ? w.data : "", w.len) == 0;
  if (!ok)
    ny_native_set_err(err, err_len, "native oracle: failed to write %s: %s",
                      path, strerror(errno));
done:
  free(w.data);
  return ok;
}

static bool ny_native_parse_i64(const char *s, int64_t *out) {
  if (!s || !*s || !out)
    return false;
  errno = 0;
  char *end = NULL;
  long long v = strtoll(s, &end, 10);
  if (errno != 0 || end == s)
    return false;
  while (end && *end && isspace((unsigned char)*end))
    end++;
  if (end && *end)
    return false;
  *out = (int64_t)v;
  return true;
}

static bool ny_native_nir_returns_f64(const ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return false;
  bool f64[4096] = {0};
  int limit = f->next_value < 4096 ? f->next_value : 4096;
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < f->len; ++i) {
      const ny_nir_inst_t *in = &f->data[i];
      if (in->dst >= 0 && in->dst < limit &&
          (in->op == NYIR_CONST_F64 || in->op == NYIR_ADD_F64 ||
           in->op == NYIR_SUB_F64 || in->op == NYIR_MUL_F64 ||
           in->op == NYIR_DIV_F64 || in->op == NYIR_I64_TO_F64 ||
           in->op == NYIR_F32_TO_F64 ||
           (in->op == NY_NIR_CALL && (in->flags & NY_NIR_INST_F_RET_F64))) &&
          !f64[in->dst]) {
        f64[in->dst] = true;
        changed = true;
      }
      if (in->op == NY_NIR_COPY && in->a >= 0 && in->a < limit &&
          in->dst >= 0 && in->dst < limit && f64[in->a] && !f64[in->dst]) {
        f64[in->dst] = true;
        changed = true;
      }
    }
  }
  for (size_t i = f->len; i > 0; --i) {
    const ny_nir_inst_t *in = &f->data[i - 1];
    if (in->op == NY_NIR_RET && in->a >= 0 && in->a < limit)
      return f64[in->a];
  }
  return false;
}

static bool ny_native_nir_returns_f32(const ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return false;
  bool f32v[4096] = {0};
  int limit = f->next_value < 4096 ? f->next_value : 4096;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->dst >= 0 && in->dst < limit &&
        (in->op == NYIR_CONST_F32 || in->op == NYIR_ADD_F32 ||
         in->op == NYIR_SUB_F32 || in->op == NYIR_MUL_F32 ||
         in->op == NYIR_DIV_F32 || in->op == NYIR_I64_TO_F32 ||
         in->op == NYIR_F64_TO_F32 ||
         (in->op == NY_NIR_CALL && (in->flags & NY_NIR_INST_F_RET_F32))) &&
        !f32v[in->dst])
      f32v[in->dst] = true;
  }
  for (size_t i = f->len; i > 0; --i) {
    const ny_nir_inst_t *in = &f->data[i - 1];
    if (in->op == NY_NIR_RET && in->a >= 0 && in->a < limit)
      return f32v[in->a];
  }
  return false;
}

static int ny_native_run_capture_i64(const char *exe, int64_t *out,
                                     char *err, size_t err_len) {
#ifdef _WIN32
  (void)exe;
  (void)out;
  ny_native_set_err(err, err_len,
                    "native oracle: result capture is not implemented on Windows");
  return -1;
#else
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    ny_native_set_err(err, err_len, "native oracle: pipe failed: %s",
                      strerror(errno));
    return -1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    execl(exe, exe, (char *)NULL);
    _exit(127);
  }
  close(pipefd[1]);
  if (pid < 0) {
    close(pipefd[0]);
    ny_native_set_err(err, err_len, "native oracle: fork failed: %s",
                      strerror(errno));
    return -1;
  }
  char buf[256];
  size_t len = 0;
  for (;;) {
    ssize_t n = read(pipefd[0], buf + len, sizeof(buf) - 1 - len);
    if (n > 0) {
      len += (size_t)n;
      if (len >= sizeof(buf) - 1)
        break;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    break;
  }
  close(pipefd[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  buf[len] = '\0';
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    ny_native_set_err(err, err_len,
                      "native oracle: harness failed (status=%d output=%.*s)",
                      status, 180, buf);
    return -1;
  }
  char *line = strstr(buf, "native result function=rt_main returned=yes result=");
  if (!line) {
    ny_native_set_err(err, err_len,
                      "native oracle: missing result line (output=%.*s)", 180,
                      buf);
    return -1;
  }
  line += strlen("native result function=rt_main returned=yes result=");
  char *nl = strchr(line, '\n');
  if (nl)
    *nl = '\0';
  if (!ny_native_parse_i64(line, out)) {
    ny_native_set_err(err, err_len,
                      "native oracle: invalid result value '%s'", line);
    return -1;
  }
  return 0;
#endif
}

bool ny_native_result_oracle_for_program(const program_t *prog,
                                         const ny_options *opt, char *err,
                                         size_t err_len) {
  if (!prog || !opt || !opt->native_result_oracle)
    return true;
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, opt) ||
      target.target != NY_NATIVE_TARGET_X86_64) {
    ny_native_set_err(err, err_len,
                      "native oracle: x86-64 native backend is required");
    return false;
  }

  ny_nir_func_t rt_main = {0};
  ny_nir_func_t funcs[128];
  const char *names[128];
  memset(funcs, 0, sizeof(funcs));
  memset(names, 0, sizeof(names));
  size_t count = 0;
  char local_err[512] = {0};
  if (!ny_native_build_nir(prog, opt, &rt_main, funcs, &count, 128, local_err,
                           sizeof(local_err))) {
    ny_native_set_err(err, err_len, "native oracle: %s",
                      local_err[0] ? local_err : "failed to build NYIR");
    return false;
  }
  size_t name_index = 0;
  for (size_t i = 0; prog && i < prog->body.len && name_index < count; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      names[name_index++] = stmt->as.fn.name ? stmt->as.fn.name : "<fn>";
  }

  bool ok = false;
  ny_nir_eval_result_t vm = {0};
  int64_t native_result = 0;
  char asm_path[4096], obj_path[4096], c_path[4096], exe_path[4096];
  unsigned long long stamp = (unsigned long long)ny_ticks_now();
  snprintf(asm_path, sizeof(asm_path), "%s/ny_oracle_%ld_%llu.s",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(obj_path, sizeof(obj_path), "%s/ny_oracle_%ld_%llu.o",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(c_path, sizeof(c_path), "%s/ny_oracle_%ld_%llu.c",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(exe_path, sizeof(exe_path), "%s/ny_oracle_%ld_%llu",
           ny_get_temp_dir(), (long)getpid(), stamp);

  if (!ny_native_eval_ir_value(&rt_main, funcs, names, count, opt, &vm, err,
                               err_len))
    goto done;
  if (!vm.returned) {
    ny_native_set_err(err, err_len, "native oracle: VM did not return");
    goto done;
  }
  bool returns_f64 = ny_native_nir_returns_f64(&rt_main);
  bool returns_f32 = ny_native_nir_returns_f32(&rt_main);
  if (!ny_native_result_oracle_emit_asm(&target, &rt_main, funcs, names, count,
                                        asm_path, err, err_len))
    goto done;

  const char *cc = ny_builder_choose_cc();
  const char *as_argv[] = {cc, "-c", asm_path, "-o", obj_path, NULL};
  if (ny_exec_spawn(as_argv) != 0) {
    ny_native_set_err(err, err_len,
                      "native oracle: assembler failed for NYIR output");
    goto done;
  }
  const char *harness_i64 =
      "#include <stdio.h>\n"
      "extern long long rt_main(void);\n"
      "int main(void) {\n"
      "  long long r = rt_main();\n"
      "  printf(\"native result function=rt_main returned=yes result=%lld\\n\", r);\n"
      "  return 0;\n"
      "}\n";
  const char *harness_f64 =
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      "#include <string.h>\n"
      "extern double rt_main(void);\n"
      "int main(void) {\n"
      "  double r = rt_main();\n"
      "  int64_t bits = 0;\n"
      "  memcpy(&bits, &r, sizeof(bits));\n"
      "  printf(\"native result function=rt_main returned=yes result=%lld\\n\", (long long)bits);\n"
      "  return 0;\n"
      "}\n";
  const char *harness_f32 =
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      "#include <string.h>\n"
      "extern float rt_main(void);\n"
      "int main(void) {\n"
      "  float r = rt_main();\n"
      "  int32_t bits = 0;\n"
      "  memcpy(&bits, &r, sizeof(bits));\n"
      "  printf(\"native result function=rt_main returned=yes result=%lld\\n\", (long long)(int64_t)(uint32_t)bits);\n"
      "  return 0;\n"
      "}\n";
  const char *harness = returns_f32 ? harness_f32 :
                        returns_f64 ? harness_f64 : harness_i64;
  if (ny_write_file(c_path, harness, strlen(harness)) != 0) {
    ny_native_set_err(err, err_len, "native oracle: failed to write harness");
    goto done;
  }
  const char *link_argv[] = {cc, c_path, obj_path, "-no-pie", "-o", exe_path,
                             NULL};
  if (ny_exec_spawn(link_argv) != 0) {
    ny_native_set_err(err, err_len, "native oracle: harness link failed");
    goto done;
  }
  if (ny_native_run_capture_i64(exe_path, &native_result, err, err_len) != 0)
    goto done;
  if (native_result != vm.result) {
    ny_native_set_err(err, err_len,
                      "native oracle: VM/native mismatch vm=%" PRId64
                      " native=%" PRId64,
                      vm.result, native_result);
    goto done;
  }
  if (opt->native_result_oracle_expected &&
      opt->native_result_oracle_expected[0]) {
    int64_t expected = 0;
    if (!ny_native_parse_i64(opt->native_result_oracle_expected, &expected)) {
      ny_native_set_err(err, err_len,
                        "native oracle: invalid expected result '%s'",
                        opt->native_result_oracle_expected);
      goto done;
    }
    if (vm.result != expected) {
      ny_native_set_err(err, err_len,
                        "native oracle: expected=%" PRId64 " vm=%" PRId64
                        " native=%" PRId64,
                        expected, vm.result, native_result);
      goto done;
    }
  }
  fprintf(stderr,
          "native oracle function=rt_main vm=%" PRId64 " native=%" PRId64
          " ok=yes\n",
          vm.result, native_result);
  ok = true;

done:
  unlink(asm_path);
  unlink(obj_path);
  unlink(c_path);
  unlink(exe_path);
  ny_nir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    ny_nir_func_free(&funcs[i]);
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
  ny_nir_func_t f = {0};
  char name[128] = {0};
  bool ok = ny_nir_load_binary(in, &f, name, sizeof(name), err, err_len);
  fclose(in);
  if (!ok) {
    ny_nir_func_free(&f);
    return false;
  }
  ok = ny_native_eval_ir_func(&f, opt, name[0] ? name : "rt_main", err,
                              err_len);
  ny_nir_func_free(&f);
  return ok;
}

bool ny_native_eval_ir_for_program(const program_t *prog,
                                   const ny_options *opt, char *err,
                                   size_t err_len) {
  if (opt && opt->nyir_run_bin_path && opt->nyir_run_bin_path[0])
    return ny_native_eval_ir_binary_file(opt->nyir_run_bin_path, opt, err,
                                         err_len);
  ny_nir_func_t rt_main = {0};
  ny_nir_func_t funcs[128] = {{0}};
  const char *names[128] = {0};
  size_t count = 0;
  if (!ny_native_build_nir(prog, opt, &rt_main, funcs, &count, 128, err,
                           err_len))
    return false;
  size_t name_count = 0;
  for (size_t i = 0; prog && i < prog->body.len && name_count < count; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!s || s->kind != NY_S_FUNC)
      continue;
    names[name_count++] = s->as.fn.name;
  }
  bool ok = ny_native_eval_ir_func_with_calls(
      &rt_main, funcs, names, count, opt, "rt_main", err, err_len);
  ny_nir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    ny_nir_func_free(&funcs[i]);
  return ok;
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
    bool bok = ny_native_nir_dump_rt_main_binary(bout, prog, berr, sizeof(berr));
    if (bout != stderr)
      fclose(bout);
    if (!bok) {
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
    if (defer_metadata_bin_report &&
        !ny_native_write_nir_metadata_report(prog, opt, err, err_len))
      return false;
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
    if (!s || s->kind != NY_S_FUNC)
      continue;
    attempted_any = true;
    char local_err[512] = {0};
    if (!ny_native_nir_dump_function(out, s, local_err, sizeof(local_err),
                                     opt)) {
      fprintf(out, "native NYIR dump unavailable for function %s: %s\n",
              s->as.fn.name ? s->as.fn.name : "<anon>",
              local_err[0] ? local_err : "unsupported shape");
    }
  }

  attempted_any = true;
  char local_err[512] = {0};
  if (!ny_native_nir_dump_rt_main(out, prog, local_err, sizeof(local_err),
                                  opt)) {
    fprintf(out, "%s\n", local_err[0] ? local_err :
            "native NYIR dump unavailable: unsupported program shape");
  }

  if (!attempted_any)
    fputs("native NYIR dump unavailable: program has no dumpable body\n", out);
  if (out != stderr)
    fclose(out);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
